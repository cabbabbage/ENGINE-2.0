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
    if (width == 0 || height == 0) {
        out_error = "GPU runtime pipeline received invalid frame dimensions.";
        return false;
    }

    const GpuSceneRenderer::TextureResourceSpec spec =
        make_scene_texture_spec(renderer, width, height);

    return renderer.ensure_texture_resource("scene.floor", spec, out_error) &&
           renderer.ensure_texture_resource("scene.layers", spec, out_error) &&
           renderer.ensure_texture_resource("scene.composite", spec, out_error);
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

    std::vector<ResolvedGpuSpriteDraw> map_floor_draws{};
    std::vector<ResolvedGpuSpriteDraw> floor_sprite_draws{};
    std::vector<ResolvedGpuSpriteDraw> layer_draws{};
    if (!resolve_sprite_draws(renderer,
                              frame_data.map_floor_draws,
                              "scene.floor_map_tiles",
                              map_floor_draws,
                              out_error)) {
        return false;
    }
    if (!resolve_sprite_draws(renderer,
                              frame_data.floor_sprite_draws,
                              "scene.floor_sprites",
                              floor_sprite_draws,
                              out_error)) {
        return false;
    }
    if (!resolve_sprite_draws(renderer, frame_data.layer_draws, "scene.layers", layer_draws, out_error)) {
        return false;
    }

    GpuFrameGraph::PassDescriptor floor_base_pass{};
    floor_base_pass.type = GpuFrameGraph::PassType::Render;
    floor_base_pass.name = make_name("render_floor_base");
    floor_base_pass.resources = {GpuFrameGraph::ResourceDependency::write_resource("scene.floor")};
    floor_base_pass.render.pipeline_id = "floor_compose";
    floor_base_pass.render.render_state_key = 0x2000u;
    floor_base_pass.render.color_target = "scene.floor";
    floor_base_pass.render.load_op = SDL_GPU_LOADOP_CLEAR;
    floor_base_pass.render.store_op = SDL_GPU_STOREOP_STORE;
    floor_base_pass.render.clear_color = SDL_FColor{0.0f, 0.0f, 0.0f, 1.0f};
    floor_base_pass.render.execute_default_draw = true;
    renderer.add_pass(std::move(floor_base_pass));

    GpuFrameGraph::PassDescriptor floor_tiles_pass{};
    floor_tiles_pass.type = GpuFrameGraph::PassType::Render;
    floor_tiles_pass.name = make_name("render_floor_tiles");
    floor_tiles_pass.resources = {GpuFrameGraph::ResourceDependency::write_resource("scene.floor")};
    floor_tiles_pass.render.pipeline_id = "sprite_batched";
    floor_tiles_pass.render.render_state_key = 0x2001u;
    floor_tiles_pass.render.color_target = "scene.floor";
    floor_tiles_pass.render.load_op = SDL_GPU_LOADOP_LOAD;
    floor_tiles_pass.render.store_op = SDL_GPU_STOREOP_STORE;
    floor_tiles_pass.render.clear_color = SDL_FColor{0.0f, 0.0f, 0.0f, 0.0f};
    floor_tiles_pass.render.execute_default_draw = false;
    floor_tiles_pass.render.custom_render =
        make_sprite_draw_callback(std::move(map_floor_draws), floor_tiles_pass.name);
    renderer.add_pass(std::move(floor_tiles_pass));

    GpuFrameGraph::PassDescriptor floor_sprites_pass{};
    floor_sprites_pass.type = GpuFrameGraph::PassType::Render;
    floor_sprites_pass.name = make_name("render_floor_sprites");
    floor_sprites_pass.resources = {GpuFrameGraph::ResourceDependency::write_resource("scene.floor")};
    floor_sprites_pass.render.pipeline_id = "sprite_batched";
    floor_sprites_pass.render.render_state_key = 0x2002u;
    floor_sprites_pass.render.color_target = "scene.floor";
    floor_sprites_pass.render.load_op = SDL_GPU_LOADOP_LOAD;
    floor_sprites_pass.render.store_op = SDL_GPU_STOREOP_STORE;
    floor_sprites_pass.render.clear_color = SDL_FColor{0.0f, 0.0f, 0.0f, 0.0f};
    floor_sprites_pass.render.execute_default_draw = false;
    floor_sprites_pass.render.custom_render =
        make_sprite_draw_callback(std::move(floor_sprite_draws), floor_sprites_pass.name);
    renderer.add_pass(std::move(floor_sprites_pass));

    GpuFrameGraph::PassDescriptor layers_pass{};
    layers_pass.type = GpuFrameGraph::PassType::Render;
    layers_pass.name = make_name("render_layers");
    layers_pass.resources = {GpuFrameGraph::ResourceDependency::write_resource("scene.layers")};
    layers_pass.render.pipeline_id = "sprite_batched";
    layers_pass.render.render_state_key = 0x2003u;
    layers_pass.render.color_target = "scene.layers";
    layers_pass.render.load_op = SDL_GPU_LOADOP_CLEAR;
    layers_pass.render.store_op = SDL_GPU_STOREOP_STORE;
    // Layer target must start transparent so composition can alpha-over floor.
    layers_pass.render.clear_color = SDL_FColor{0.0f, 0.0f, 0.0f, 0.0f};
    layers_pass.render.execute_default_draw = false;
    layers_pass.render.custom_render =
        make_sprite_draw_callback(std::move(layer_draws), layers_pass.name);
    renderer.add_pass(std::move(layers_pass));

    GpuFrameGraph::PassDescriptor compose_pass{};
    compose_pass.type = GpuFrameGraph::PassType::Render;
    compose_pass.name = make_name("render_scene_composite");
    compose_pass.resources = {
        GpuFrameGraph::ResourceDependency::read("scene.floor"),
        GpuFrameGraph::ResourceDependency::read("scene.layers"),
        GpuFrameGraph::ResourceDependency::write_resource("scene.composite"),
    };
    compose_pass.render.pipeline_id = "final_compose";
    compose_pass.render.render_state_key = 0x2004u;
    compose_pass.render.color_target = "scene.composite";
    compose_pass.render.load_op = SDL_GPU_LOADOP_CLEAR;
    compose_pass.render.store_op = SDL_GPU_STOREOP_STORE;
    compose_pass.render.clear_color = SDL_FColor{0.0f, 0.0f, 0.0f, 1.0f};
    compose_pass.render.fragment_sampled_textures = {
        GpuFrameGraph::RenderPassPayload::SampledTextureBinding{"scene.floor", "linear_clamp"},
        GpuFrameGraph::RenderPassPayload::SampledTextureBinding{"scene.layers", "linear_clamp"},
    };
    renderer.add_pass(std::move(compose_pass));

    GpuFrameGraph::PassDescriptor present_pass{};
    present_pass.type = GpuFrameGraph::PassType::Render;
    present_pass.name = make_name("present_scene_composite");
    present_pass.resources = {
        GpuFrameGraph::ResourceDependency::read("scene.composite"),
        GpuFrameGraph::ResourceDependency::write_resource("scene.swapchain"),
    };
    present_pass.render.pipeline_id = "sprite_textured";
    present_pass.render.render_state_key = 0x2005u;
    present_pass.render.use_swapchain_target = true;
    present_pass.render.load_op = SDL_GPU_LOADOP_CLEAR;
    present_pass.render.clear_color = SDL_FColor{0.0f, 0.0f, 0.0f, 1.0f};
    present_pass.render.store_op = SDL_GPU_STOREOP_STORE;
    present_pass.render.fragment_sampled_textures = {
        GpuFrameGraph::RenderPassPayload::SampledTextureBinding{"scene.composite", "linear_clamp"},
    };
    renderer.add_pass(std::move(present_pass));

    return true;
}
