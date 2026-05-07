#include "rendering/render/gpu_runtime_pipeline.hpp"

#include "rendering/render/gpu_frame_graph.hpp"
#include "rendering/render/gpu_scene_renderer.hpp"
#include "rendering/render/render_diagnostics.hpp"

namespace {

GpuSceneRenderer::TextureResourceSpec make_scene_texture_spec(const GpuSceneRenderer& renderer,
                                                              std::uint32_t width,
                                                              std::uint32_t height) {
    GpuSceneRenderer::TextureResourceSpec spec{};
    spec.width = width;
    spec.height = height;
    spec.format = renderer.device()->format_policy().albedo_format;
    spec.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
    spec.layer_count_or_depth = 1;
    spec.num_levels = 1;
    spec.sample_count = SDL_GPU_SAMPLECOUNT_1;
    return spec;
}

struct ResolvedGpuSpriteDraw {
    SDL_GPUTexture* source_texture = nullptr;
    std::array<GpuSpriteVertex, 6> vertices{};
    SDL_FColor modulate{1.0f, 1.0f, 1.0f, 1.0f};
};

bool resolve_sprite_draws(GpuSceneRenderer& renderer,
                          const std::vector<GpuSpriteDrawPacket>& source_draws,
                          std::string_view pass_name,
                          std::vector<ResolvedGpuSpriteDraw>& out_draws,
                          std::string& out_error) {
    out_draws.clear();
    out_draws.reserve(source_draws.size());
    out_error.clear();

    for (const GpuSpriteDrawPacket& draw : source_draws) {
        std::string texture_error;
        SDL_GPUTexture* gpu_texture =
            renderer.resolve_gpu_texture_for_sdl_texture(draw.source_texture, texture_error);
        if (!gpu_texture) {
            out_error = "Failed to resolve draw texture for pass '" + std::string(pass_name) + "': " +
                        (texture_error.empty() ? "unknown SDL->GPU texture import failure." : texture_error);
            return false;
        }

        ResolvedGpuSpriteDraw resolved{};
        resolved.source_texture = gpu_texture;
        resolved.vertices = draw.vertices;
        resolved.modulate = draw.modulate;
        out_draws.push_back(resolved);
    }
    return true;
}

struct SpriteBatchVertexUniformData {
    SDL_FColor vertex_uv[6]{};
    SDL_FColor modulate{1.0f, 1.0f, 1.0f, 1.0f};
};

std::function<bool(const GpuFrameGraph::ExecuteContext&, SDL_GPURenderPass*, std::string&)>
make_sprite_draw_callback(std::vector<ResolvedGpuSpriteDraw> draws, std::string pass_name) {
    return [draws = std::move(draws), pass_name = std::move(pass_name)](
               const GpuFrameGraph::ExecuteContext& context,
               SDL_GPURenderPass* render_pass,
               std::string& out_error) -> bool {
        out_error.clear();
        if (draws.empty()) {
            return true;
        }
        if (!render_pass) {
            out_error = "Render pass handle was null.";
            return false;
        }
        if (!context.command_buffer) {
            out_error = "Command buffer was null.";
            return false;
        }
        if (!context.resolve_sampler) {
            out_error = "Sampler resolver unavailable.";
            return false;
        }

        SDL_GPUSampler* linear_sampler = context.resolve_sampler("linear_clamp");
        if (!linear_sampler) {
            out_error = "Sampler 'linear_clamp' not available.";
            return false;
        }

        for (const ResolvedGpuSpriteDraw& draw : draws) {
            SDL_GPUTextureSamplerBinding sampler_binding{};
            sampler_binding.texture = draw.source_texture;
            sampler_binding.sampler = linear_sampler;
            SDL_BindGPUFragmentSamplers(render_pass, 0u, &sampler_binding, 1u);

            SpriteBatchVertexUniformData uniform_data{};
            for (std::size_t i = 0; i < draw.vertices.size(); ++i) {
                const GpuSpriteVertex& vertex = draw.vertices[i];
                uniform_data.vertex_uv[i] = SDL_FColor{vertex.clip_x, vertex.clip_y, vertex.uv_x, vertex.uv_y};
            }
            uniform_data.modulate = draw.modulate;
            SDL_PushGPUVertexUniformData(context.command_buffer, 0u, &uniform_data, static_cast<Uint32>(sizeof(uniform_data)));

            SDL_DrawGPUPrimitives(render_pass, 6u, 1u, 0u, 0u);
            render_diagnostics::add_draw_call_count();
        }

        return true;
    };
}

} // namespace

bool GpuRuntimePipeline::ensure_resources(GpuSceneRenderer& renderer,
                                          std::uint32_t width,
                                          std::uint32_t height,
                                          std::string& out_error) const {
    (void)renderer; (void)width; (void)height;
    out_error.clear();
    return true;
}

bool GpuRuntimePipeline::ensure_shared_resources(GpuSceneRenderer& renderer,
                                                 std::string& out_error) const {
    GpuSceneRenderer::SamplerResourceSpec sampler_spec{};
    return renderer.ensure_sampler_resource("linear_clamp", sampler_spec, out_error);
}

bool GpuRuntimePipeline::enqueue_frame_graph(GpuSceneRenderer& renderer,
                                             const GpuSceneFrameData& frame_data,
                                             std::string_view pass_name_prefix,
                                             std::uint32_t width,
                                             std::uint32_t height,
                                             std::string& out_error) const {
    (void)width;
    (void)height;
    out_error.clear();
    auto make_name = [pass_name_prefix](std::string_view suffix) {
        std::string value(pass_name_prefix);
        value += "_";
        value += suffix;
        return value;
    };

    std::vector<ResolvedGpuSpriteDraw> floor_tile_draws{};
    std::vector<ResolvedGpuSpriteDraw> floor_sprite_draws{};
    std::vector<ResolvedGpuSpriteDraw> world_sprite_draws{};
    if (!resolve_sprite_draws(renderer, frame_data.map_floor_draws, "scene.floor_tiles", floor_tile_draws, out_error) ||
        !resolve_sprite_draws(renderer, frame_data.floor_sprite_draws, "scene.floor_sprites", floor_sprite_draws, out_error) ||
        !resolve_sprite_draws(renderer, frame_data.layer_draws, "scene.world_sprites", world_sprite_draws, out_error)) {
        return false;
    }

    GpuFrameGraph::PassDescriptor main_pass{};
    main_pass.type = GpuFrameGraph::PassType::Render;
    main_pass.name = make_name("render_main_scene");
    main_pass.resources = {GpuFrameGraph::ResourceDependency::write_resource("scene.swapchain")};
    main_pass.render.pipeline_id = "sprite_batched";
    main_pass.render.render_state_key = 0x2100u;
    main_pass.render.use_swapchain_target = true;
    main_pass.render.load_op = SDL_GPU_LOADOP_CLEAR;
    main_pass.render.store_op = SDL_GPU_STOREOP_STORE;
    main_pass.render.clear_color = frame_data.map_floor_draw_count == 0
        ? SDL_FColor{0.08f, 0.02f, 0.12f, 1.0f}
        : SDL_FColor{0.0f, 0.0f, 0.0f, 1.0f};
    main_pass.render.execute_default_draw = false;

    std::vector<ResolvedGpuSpriteDraw> unified;
    unified.reserve(floor_tile_draws.size()+floor_sprite_draws.size()+world_sprite_draws.size());
    unified.insert(unified.end(), floor_tile_draws.begin(), floor_tile_draws.end());
    unified.insert(unified.end(), floor_sprite_draws.begin(), floor_sprite_draws.end());
    unified.insert(unified.end(), world_sprite_draws.begin(), world_sprite_draws.end());
    main_pass.render.custom_render = make_sprite_draw_callback(std::move(unified), main_pass.name);
    renderer.add_pass(std::move(main_pass));

    return true;
}
