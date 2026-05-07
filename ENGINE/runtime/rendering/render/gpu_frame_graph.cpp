#include "rendering/render/gpu_frame_graph.hpp"

#include "rendering/render/render_diagnostics.hpp"
#include "utils/log.hpp"

#include <algorithm>
#include <unordered_map>
#include <utility>

namespace {

bool has_read_dependency_for_resource(const GpuFrameGraph::PassDescriptor& pass,
                                      const std::string& resource_name) {
    for (const GpuFrameGraph::ResourceDependency& dependency : pass.resources) {
        if (dependency.name == resource_name && !dependency.write) {
            return true;
        }
    }
    return false;
}

bool has_write_dependency_for_resource(const GpuFrameGraph::PassDescriptor& pass,
                                       const std::string& resource_name) {
    for (const GpuFrameGraph::ResourceDependency& dependency : pass.resources) {
        if (dependency.name == resource_name && dependency.write) {
            return true;
        }
    }
    return false;
}

} // namespace

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
    // Frame-graph resource dependency rule:
    // - Any in-frame read (Source::FrameGraph + write=false) must have an earlier writer pass
    //   in this same execute() call.
    // - Resources that are intentionally provided by systems outside this frame graph (for
    //   example imported history buffers or external producer outputs) must be declared as
    //   Source::ExternalInput via ResourceDependency::imported_read().
    // This keeps validation strict for accidental read-before-write while still supporting
    // legitimate external inputs with an explicit annotation.
    ExecutionStats stats{};
    if (!options.dry_run && !context.command_buffer) {
        stats.success = false;
        stats.error_message = "GpuFrameGraph execute called with null command buffer";
        last_execution_stats_ = stats;
        return stats;
    }

    std::unordered_map<std::string, std::size_t> resource_last_writer{};
    bool swapchain_touched = false;
    bool swapchain_cleared = false;
    bool swapchain_fully_written = false;
    std::string first_swapchain_pass_name{};
    std::string first_swapchain_load_op{"none"};
    std::string first_swapchain_store_op{"none"};
    std::size_t swapchain_render_pass_count = 0;
    std::size_t last_swapchain_render_pass_index = 0;

    const auto load_op_name = [](SDL_GPULoadOp op) -> const char* {
        switch (op) {
        case SDL_GPU_LOADOP_LOAD: return "LOAD";
        case SDL_GPU_LOADOP_CLEAR: return "CLEAR";
        case SDL_GPU_LOADOP_DONT_CARE: return "DONT_CARE";
        default: return "UNKNOWN";
        }
    };
    const auto store_op_name = [](SDL_GPUStoreOp op) -> const char* {
        switch (op) {
        case SDL_GPU_STOREOP_STORE: return "STORE";
        case SDL_GPU_STOREOP_DONT_CARE: return "DONT_CARE";
        default: return "UNKNOWN";
        }
    };

    const auto is_written_in_pass = [](const PassDescriptor& pass, const std::string& name) {
        for (const ResourceDependency& dep : pass.resources) {
            if (dep.write && dep.name == name) {
                return true;
            }
        }
        return false;
    };

    for (const PassDescriptor& pass : passes_) {
        if (pass.name.empty()) {
            const std::string message = "[GpuFrameGraph] Pass has an empty name.";
            if (!fail_or_warn_missing(message, true, true, stats)) { last_execution_stats_ = stats; return stats; }
        }
        if (pass.type == PassType::Render) {
            if (!pass.render.use_swapchain_target && pass.render.color_target.empty()) {
                const std::string message = "[GpuFrameGraph] Render pass '" + pass.name + "' has no color target.";
                if (!fail_or_warn_missing(message, true, true, stats)) { last_execution_stats_ = stats; return stats; }
            }
            if (pass.render.use_swapchain_target && context.swapchain_texture == nullptr && !options.dry_run) {
                const std::string message = "[GpuFrameGraph] Render pass '" + pass.name + "' targets swapchain but swapchain texture is null.";
                if (!fail_or_warn_missing(message, true, true, stats)) { last_execution_stats_ = stats; return stats; }
            }
            if (pass.render.use_swapchain_target && pass.render.load_op != SDL_GPU_LOADOP_CLEAR) {
                const std::string message = "[GpuFrameGraph] Swapchain pass '" + pass.name + "' must use load_op=CLEAR.";
                if (!fail_or_warn_missing(message, true, true, stats)) { last_execution_stats_ = stats; return stats; }
            }
            if (pass.render.use_swapchain_target && pass.render.store_op != SDL_GPU_STOREOP_STORE) {
                const std::string message = "[GpuFrameGraph] Swapchain pass '" + pass.name + "' must use store_op=STORE.";
                if (!fail_or_warn_missing(message, true, true, stats)) { last_execution_stats_ = stats; return stats; }
            }
            for (const auto& sampled : pass.render.fragment_sampled_textures) {
                if (is_written_in_pass(pass, sampled.texture)) {
                    const std::string message = "[GpuFrameGraph] Pass '" + pass.name + "' reads and writes texture '" + sampled.texture + "' in the same pass.";
                    if (!fail_or_warn_missing(message, true, true, stats)) { last_execution_stats_ = stats; return stats; }
                }
            }
        }
        if (pass.type == PassType::Copy && pass.blit.use_swapchain_destination) {
            const std::string message = "[GpuFrameGraph] Swapchain copy/blit passes are not allowed. Use an explicit final render pass to the swapchain.";
            if (!fail_or_warn_missing(message, true, true, stats)) { last_execution_stats_ = stats; return stats; }
        }
    }

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
            const bool is_external_input = dep.source == ResourceDependency::Source::ExternalInput;
            if (!dep.write && !is_external_input &&
                resource_last_writer.find(dep.name) == resource_last_writer.end()) {
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

        if (pass.type == PassType::Render) {
            if (pass.render.use_swapchain_target) {
                ++swapchain_render_pass_count;
                last_swapchain_render_pass_index = static_cast<std::size_t>(&pass - passes_.data());
            }
            for (const RenderPassPayload::SampledTextureBinding& sampled :
                 pass.render.fragment_sampled_textures) {
                if (sampled.texture.empty()) {
                    const std::string message = "[GpuFrameGraph] Pass '" + pass.name +
                                                "' has a sampled fragment texture binding with an empty texture name.";
                    if (!fail_or_warn_missing(message, true, true, stats)) {
                        last_execution_stats_ = stats;
                        return stats;
                    }
                }
                if (sampled.sampler.empty()) {
                    const std::string message = "[GpuFrameGraph] Pass '" + pass.name +
                                                "' has a sampled fragment texture binding with an empty sampler name.";
                    if (!fail_or_warn_missing(message, true, true, stats)) {
                        last_execution_stats_ = stats;
                        return stats;
                    }
                }
                if (!has_read_dependency_for_resource(pass, sampled.texture)) {
                    const std::string message = "[GpuFrameGraph] Pass '" + pass.name +
                                                "' samples texture '" + sampled.texture +
                                                "' without declaring it as a read dependency.";
                    if (!fail_or_warn_missing(message, true, true, stats)) {
                        last_execution_stats_ = stats;
                        return stats;
                    }
                }
                if (!has_write_dependency_for_resource(pass, "scene.swapchain") &&
                    pass.render.use_swapchain_target) {
                    const std::string message = "[GpuFrameGraph] Swapchain pass '" + pass.name +
                                                "' must declare a write dependency on 'scene.swapchain'.";
                    if (!fail_or_warn_missing(message, true, true, stats)) {
                        last_execution_stats_ = stats;
                        return stats;
                    }
                }
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
            if (pass.render.use_swapchain_target) {
                if (!swapchain_touched) {
                    swapchain_touched = true;
                    first_swapchain_pass_name = pass.name;
                    first_swapchain_load_op = load_op_name(pass.render.load_op);
                    first_swapchain_store_op = store_op_name(pass.render.store_op);
                }
                swapchain_cleared = swapchain_cleared || (pass.render.load_op == SDL_GPU_LOADOP_CLEAR);
                swapchain_fully_written = swapchain_fully_written ||
                                          (pass.render.load_op == SDL_GPU_LOADOP_CLEAR);
                if (pass.render.load_op == SDL_GPU_LOADOP_LOAD) {
                    vibble::log::warn("[GpuFrameGraph] Swapchain render pass '" + pass.name +
                                      "' uses load_op=LOAD. This is only valid when intentionally compositing from known in-frame content.");
                }
            }
            target_info.resolve_texture = nullptr;
            target_info.resolve_mip_level = 0;
            target_info.resolve_layer = 0;
            target_info.cycle = target_info.load_op != SDL_GPU_LOADOP_LOAD;
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
            if (!pass.render.fragment_sampled_textures.empty()) {
                std::vector<SDL_GPUTextureSamplerBinding> fragment_sampler_bindings{};
                fragment_sampler_bindings.reserve(pass.render.fragment_sampled_textures.size());
                for (const RenderPassPayload::SampledTextureBinding& sampled :
                     pass.render.fragment_sampled_textures) {
                    SDL_GPUTexture* sampled_texture = context.resolve_texture
                        ? context.resolve_texture(sampled.texture)
                        : nullptr;
                    SDL_GPUSampler* sampler = context.resolve_sampler
                        ? context.resolve_sampler(sampled.sampler)
                        : nullptr;
                    if (!sampled_texture || !sampler) {
                        SDL_EndGPURenderPass(render_pass);
                        const std::string message = "[GpuFrameGraph] Pass '" + pass.name +
                                                    "' failed to resolve sampled texture binding texture='" +
                                                    sampled.texture + "' sampler='" + sampled.sampler + "'.";
                        if (!fail_or_warn_missing(message, options.fail_on_missing_resource, false, stats)) {
                            last_execution_stats_ = stats;
                            return stats;
                        }
                        fragment_sampler_bindings.clear();
                        break;
                    }
                    SDL_GPUTextureSamplerBinding binding{};
                    binding.texture = sampled_texture;
                    binding.sampler = sampler;
                    fragment_sampler_bindings.push_back(binding);
                }
                if (fragment_sampler_bindings.empty() && !pass.render.fragment_sampled_textures.empty()) {
                    continue;
                }
                SDL_BindGPUFragmentSamplers(render_pass,
                                            0u,
                                            fragment_sampler_bindings.data(),
                                            static_cast<Uint32>(fragment_sampler_bindings.size()));
            }
            if (pass.render.custom_render) {
                std::string custom_error;
                if (!pass.render.custom_render(context, render_pass, custom_error)) {
                    SDL_EndGPURenderPass(render_pass);
                    stats.success = false;
                    stats.error_message = custom_error.empty()
                        ? ("[GpuFrameGraph] Custom render callback failed for pass '" + pass.name + "'.")
                        : ("[GpuFrameGraph] Custom render callback failed for pass '" + pass.name + "': " + custom_error);
                    vibble::log::error(stats.error_message);
                    last_execution_stats_ = stats;
                    return stats;
                }
            }

            if (pass.render.execute_default_draw) {
                SDL_DrawGPUPrimitives(render_pass,
                                      std::max<Uint32>(1u, static_cast<Uint32>(pass.render.vertex_count)),
                                      std::max<Uint32>(1u, static_cast<Uint32>(pass.render.instance_count)),
                                      0u,
                                      0u);
                render_diagnostics::add_draw_call_count();
                ++stats.draw_call_count;
            }
            SDL_EndGPURenderPass(render_pass);
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
            if (pass.blit.use_swapchain_destination) {
                if (!swapchain_touched) {
                    swapchain_touched = true;
                    first_swapchain_pass_name = pass.name;
                    first_swapchain_load_op = load_op_name(pass.blit.load_op);
                    first_swapchain_store_op = "STORE";
                }
                swapchain_cleared = swapchain_cleared || (pass.blit.load_op == SDL_GPU_LOADOP_CLEAR);
                swapchain_fully_written = swapchain_fully_written ||
                                          (pass.blit.load_op == SDL_GPU_LOADOP_CLEAR);
                if (pass.blit.load_op == SDL_GPU_LOADOP_LOAD) {
                    vibble::log::warn("[GpuFrameGraph] Swapchain copy pass '" + pass.name +
                                      "' uses load_op=LOAD. This is only valid when intentionally compositing from known in-frame content.");
                }
            }
            blit_info.clear_color = SDL_FColor{0.0f, 0.0f, 0.0f, 0.0f};
            blit_info.flip_mode = SDL_FLIP_NONE;
            blit_info.filter = pass.blit.filter;
            blit_info.cycle = blit_info.load_op != SDL_GPU_LOADOP_LOAD;
            SDL_BlitGPUTexture(context.command_buffer, &blit_info);
            continue;
        }
    }

    if (swapchain_render_pass_count != 1) {
        stats.success = false;
        stats.error_message = "[GpuFrameGraph] Expected exactly one final swapchain render pass, found " +
                              std::to_string(swapchain_render_pass_count) + ".";
        vibble::log::error(stats.error_message);
        last_execution_stats_ = stats;
        return stats;
    }
    if (last_swapchain_render_pass_index != (passes_.empty() ? 0u : (passes_.size() - 1u))) {
        stats.success = false;
        stats.error_message = "[GpuFrameGraph] Final swapchain render pass must be the last pass in the frame graph.";
        vibble::log::error(stats.error_message);
        last_execution_stats_ = stats;
        return stats;
    }

    if (swapchain_touched && (!swapchain_cleared || !swapchain_fully_written)) {
        stats.success = false;
        stats.error_message = "[GpuFrameGraph] Final presented target may be invalid for this frame. first_swapchain_pass='" +
                              first_swapchain_pass_name + "' load_op=" + first_swapchain_load_op +
                              " store_op=" + first_swapchain_store_op +
                              " cleared=" + (swapchain_cleared ? std::string("true") : std::string("false")) +
                              " fully_written=" + (swapchain_fully_written ? std::string("true") : std::string("false")) + ".";
        vibble::log::error(stats.error_message);
        last_execution_stats_ = stats;
        return stats;
    }

    stats.success = true;
    last_execution_stats_ = stats;
    return stats;
}
