#include "rendering/render/gpu_frame_graph.hpp"

#include "rendering/render/render_diagnostics.hpp"
#include "utils/log.hpp"

#include <algorithm>
#include <unordered_map>
#include <utility>

void GpuFrameGraph::reset() {
    passes_.clear();
    last_execution_stats_ = ExecutionStats{};
}

void GpuFrameGraph::add_pass(PassDescriptor pass) {
    passes_.push_back(std::move(pass));
}

bool GpuFrameGraph::fail_or_warn_missing(const std::string& message,
                                         bool fail,
                                         bool is_validation_error,
                                         ExecutionStats& stats) const {
    if (fail) {
        if (is_validation_error) {
            ++stats.dependency_error_count;
        }
        stats.success = false;
        stats.error_message = message;
        vibble::log::error(message);
        return false;
    }
    if (is_validation_error) {
        ++stats.dependency_warning_count;
    }
    vibble::log::warn(message);
    return true;
}

GpuFrameGraph::ExecutionStats GpuFrameGraph::execute(const ExecuteContext& context,
                                                     const ExecuteOptions& options) const {
    ExecutionStats stats{};
    if (!options.dry_run && !context.command_buffer) {
        stats.success = false;
        stats.error_message = "GpuFrameGraph execute called with null command buffer";
        last_execution_stats_ = stats;
        return stats;
    }

    std::unordered_map<std::string, std::size_t> resource_last_writer{};
    for (const PassDescriptor& pass : passes_) {
        switch (pass.type) {
        case PassType::Render:
            render_diagnostics::add_render_pass();
            ++stats.render_pass_count;
            break;
        case PassType::Copy:
            render_diagnostics::add_copy_pass();
            ++stats.copy_pass_count;
            break;
        case PassType::Compute:
            render_diagnostics::add_compute_pass();
            ++stats.compute_pass_count;
            break;
        }

        for (const ResourceDependency& dep : pass.resources) {
            if (dep.name.empty()) {
                continue;
            }
            if (!dep.write && resource_last_writer.find(dep.name) == resource_last_writer.end()) {
                const std::string message = "[GpuFrameGraph] Pass '" + pass.name +
                                            "' reads resource '" + dep.name +
                                            "' before any writer pass.";
                const bool should_continue = fail_or_warn_missing(
                    message,
                    options.strict_resource_validation && options.fail_on_validation_error,
                    true,
                    stats);
                if (!should_continue) {
                    last_execution_stats_ = stats;
                    return stats;
                }
            }
            if (dep.write) {
                resource_last_writer[dep.name] = static_cast<std::size_t>(&pass - passes_.data());
            }
        }

        if (options.dry_run) {
            continue;
        }

        if (pass.type == PassType::Render) {
            SDL_GPUTexture* target = nullptr;
            if (pass.render.use_swapchain_target) {
                target = context.swapchain_texture;
            } else if (context.resolve_texture) {
                target = context.resolve_texture(pass.render.color_target);
            }
            if (!target) {
                const std::string message = "[GpuFrameGraph] Pass '" + pass.name +
                                            "' failed to resolve render target '" +
                                            (pass.render.use_swapchain_target ? std::string("swapchain") : pass.render.color_target) + "'.";
                if (!fail_or_warn_missing(message, options.fail_on_missing_resource, false, stats)) {
                    last_execution_stats_ = stats;
                    return stats;
                }
                continue;
            }

            SDL_GPURenderPass* render_pass = nullptr;
            SDL_GPUColorTargetInfo target_info{};
            target_info.texture = target;
            target_info.mip_level = 0;
            target_info.layer_or_depth_plane = 0;
            target_info.clear_color = pass.render.clear_color;
            target_info.load_op = pass.render.load_op;
            target_info.store_op = pass.render.store_op;
            target_info.resolve_texture = nullptr;
            target_info.resolve_mip_level = 0;
            target_info.resolve_layer = 0;
            target_info.cycle = false;
            target_info.cycle_resolve_texture = false;

            render_pass = SDL_BeginGPURenderPass(context.command_buffer, &target_info, 1, nullptr);
            if (!render_pass) {
                stats.success = false;
                stats.error_message = "[GpuFrameGraph] SDL_BeginGPURenderPass failed for '" +
                                      pass.name + "': " + SDL_GetError();
                last_execution_stats_ = stats;
                return stats;
            }

            SDL_GPUGraphicsPipeline* pipeline = nullptr;
            if (context.resolve_graphics_pipeline) {
                pipeline = context.resolve_graphics_pipeline(pass.render.pipeline_id,
                                                             pass.render.render_state_key);
            }
            if (!pipeline) {
                SDL_EndGPURenderPass(render_pass);
                const std::string message = "[GpuFrameGraph] Pass '" + pass.name +
                                            "' failed to resolve graphics pipeline '" +
                                            pass.render.pipeline_id + "'.";
                if (!fail_or_warn_missing(message, options.fail_on_missing_pipeline, false, stats)) {
                    last_execution_stats_ = stats;
                    return stats;
                }
                continue;
            }

            SDL_BindGPUGraphicsPipeline(render_pass, pipeline);
            SDL_DrawGPUPrimitives(render_pass,
                                  std::max<Uint32>(1u, static_cast<Uint32>(pass.render.vertex_count)),
                                  std::max<Uint32>(1u, static_cast<Uint32>(pass.render.instance_count)),
                                  0u,
                                  0u);
            SDL_EndGPURenderPass(render_pass);
            render_diagnostics::add_draw_call_count();
            ++stats.draw_call_count;
            continue;
        }

        if (pass.type == PassType::Compute) {
            std::vector<SDL_GPUStorageBufferReadWriteBinding> rw_bindings{};
            rw_bindings.reserve(pass.compute.rw_buffer_bindings.size());
            for (const std::string& name : pass.compute.rw_buffer_bindings) {
                SDL_GPUBuffer* buffer = context.resolve_buffer ? context.resolve_buffer(name) : nullptr;
                if (!buffer) {
                    const std::string message = "[GpuFrameGraph] Pass '" + pass.name +
                                                "' failed to resolve compute buffer '" +
                                                name + "'.";
                    if (!fail_or_warn_missing(message, options.fail_on_missing_resource, false, stats)) {
                        last_execution_stats_ = stats;
                        return stats;
                    }
                    continue;
                }
                SDL_GPUStorageBufferReadWriteBinding binding{};
                binding.buffer = buffer;
                binding.cycle = false;
                rw_bindings.push_back(binding);
            }

            SDL_GPUComputePass* compute_pass = SDL_BeginGPUComputePass(
                context.command_buffer,
                nullptr,
                0,
                rw_bindings.empty() ? nullptr : rw_bindings.data(),
                static_cast<Uint32>(rw_bindings.size()));
            if (!compute_pass) {
                stats.success = false;
                stats.error_message = "[GpuFrameGraph] SDL_BeginGPUComputePass failed for '" +
                                      pass.name + "': " + SDL_GetError();
                last_execution_stats_ = stats;
                return stats;
            }

            SDL_GPUComputePipeline* pipeline = nullptr;
            if (context.resolve_compute_pipeline) {
                pipeline = context.resolve_compute_pipeline(pass.compute.pipeline_id,
                                                            pass.compute.render_state_key);
            }
            if (!pipeline) {
                SDL_EndGPUComputePass(compute_pass);
                const std::string message = "[GpuFrameGraph] Pass '" + pass.name +
                                            "' failed to resolve compute pipeline '" +
                                            pass.compute.pipeline_id + "'.";
                if (!fail_or_warn_missing(message, options.fail_on_missing_pipeline, false, stats)) {
                    last_execution_stats_ = stats;
                    return stats;
                }
                continue;
            }

            SDL_BindGPUComputePipeline(compute_pass, pipeline);
            SDL_DispatchGPUCompute(compute_pass,
                                   std::max<Uint32>(1u, static_cast<Uint32>(pass.compute.dispatch_x)),
                                   std::max<Uint32>(1u, static_cast<Uint32>(pass.compute.dispatch_y)),
                                   std::max<Uint32>(1u, static_cast<Uint32>(pass.compute.dispatch_z)));
            SDL_EndGPUComputePass(compute_pass);
            continue;
        }

        if (pass.type == PassType::Copy) {
            SDL_GPUTexture* source_texture = context.resolve_texture
                ? context.resolve_texture(pass.blit.source_texture)
                : nullptr;
            SDL_GPUTexture* destination_texture = pass.blit.use_swapchain_destination
                ? context.swapchain_texture
                : (context.resolve_texture ? context.resolve_texture(pass.blit.destination_texture) : nullptr);

            if (!source_texture || !destination_texture) {
                const std::string message = "[GpuFrameGraph] Pass '" + pass.name +
                                            "' failed to resolve copy source/destination textures.";
                if (!fail_or_warn_missing(message, options.fail_on_missing_resource, false, stats)) {
                    last_execution_stats_ = stats;
                    return stats;
                }
                continue;
            }

            const Uint32 copy_width =
                pass.blit.width > 0 ? static_cast<Uint32>(pass.blit.width)
                                    : std::max<Uint32>(1u, static_cast<Uint32>(context.swapchain_width));
            const Uint32 copy_height =
                pass.blit.height > 0 ? static_cast<Uint32>(pass.blit.height)
                                     : std::max<Uint32>(1u, static_cast<Uint32>(context.swapchain_height));

            SDL_GPUBlitInfo blit_info{};
            blit_info.source.texture = source_texture;
            blit_info.source.mip_level = 0;
            blit_info.source.layer_or_depth_plane = 0;
            blit_info.source.x = 0;
            blit_info.source.y = 0;
            blit_info.source.w = copy_width;
            blit_info.source.h = copy_height;
            blit_info.destination.texture = destination_texture;
            blit_info.destination.mip_level = 0;
            blit_info.destination.layer_or_depth_plane = 0;
            blit_info.destination.x = 0;
            blit_info.destination.y = 0;
            blit_info.destination.w = copy_width;
            blit_info.destination.h = copy_height;
            blit_info.load_op = pass.blit.load_op;
            blit_info.clear_color = SDL_FColor{0.0f, 0.0f, 0.0f, 0.0f};
            blit_info.flip_mode = SDL_FLIP_NONE;
            blit_info.filter = pass.blit.filter;
            blit_info.cycle = false;
            SDL_BlitGPUTexture(context.command_buffer, &blit_info);
            continue;
        }
    }

    stats.success = true;
    last_execution_stats_ = stats;
    return stats;
}
