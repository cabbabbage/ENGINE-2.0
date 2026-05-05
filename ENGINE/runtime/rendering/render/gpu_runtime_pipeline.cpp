#include "rendering/render/gpu_runtime_pipeline.hpp"

#include "rendering/render/gpu_frame_graph.hpp"
#include "rendering/render/gpu_scene_renderer.hpp"

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
           renderer.ensure_texture_resource("scene.blur_background", spec, out_error) &&
           renderer.ensure_texture_resource("scene.blur_foreground", spec, out_error) &&
           renderer.ensure_texture_resource("scene.composite", spec, out_error);
}

bool GpuRuntimePipeline::ensure_shared_resources(GpuSceneRenderer& renderer,
                                                 std::string& out_error) const {
    GpuSceneRenderer::SamplerResourceSpec sampler_spec{};
    return renderer.ensure_sampler_resource("linear_clamp", sampler_spec, out_error);
}

void GpuRuntimePipeline::enqueue_frame_graph(GpuSceneRenderer& renderer,
                                             std::string_view pass_name_prefix,
                                             std::uint32_t width,
                                             std::uint32_t height) const {
    auto make_name = [pass_name_prefix](std::string_view suffix) {
        std::string value(pass_name_prefix);
        value += "_";
        value += suffix;
        return value;
    };

    GpuFrameGraph::PassDescriptor floor_pass{};
    floor_pass.type = GpuFrameGraph::PassType::Render;
    floor_pass.name = make_name("render_floor");
    floor_pass.resources = {GpuFrameGraph::ResourceDependency::write_resource("scene.floor")};
    floor_pass.render.pipeline_id = "floor_compose";
    floor_pass.render.render_state_key = 0x1001u;
    floor_pass.render.color_target = "scene.floor";
    renderer.add_pass(std::move(floor_pass));

    GpuFrameGraph::PassDescriptor layers_pass{};
    layers_pass.type = GpuFrameGraph::PassType::Render;
    layers_pass.name = make_name("render_layers");
    layers_pass.resources = {GpuFrameGraph::ResourceDependency::write_resource("scene.layers")};
    layers_pass.render.pipeline_id = "sprite_textured";
    layers_pass.render.render_state_key = 0x1002u;
    layers_pass.render.color_target = "scene.layers";
    renderer.add_pass(std::move(layers_pass));

    GpuFrameGraph::PassDescriptor blur_bg_pass{};
    blur_bg_pass.type = GpuFrameGraph::PassType::Render;
    blur_bg_pass.name = make_name("render_blur_background");
    blur_bg_pass.resources = {
        GpuFrameGraph::ResourceDependency::read("scene.layers"),
        GpuFrameGraph::ResourceDependency::write_resource("scene.blur_background"),
    };
    blur_bg_pass.render.pipeline_id = "dark_mask";
    blur_bg_pass.render.render_state_key = 0x1003u;
    blur_bg_pass.render.color_target = "scene.blur_background";
    blur_bg_pass.render.fragment_sampled_textures = {
        GpuFrameGraph::RenderPassPayload::SampledTextureBinding{"scene.layers", "linear_clamp"},
    };
    renderer.add_pass(std::move(blur_bg_pass));

    GpuFrameGraph::PassDescriptor blur_fg_pass{};
    blur_fg_pass.type = GpuFrameGraph::PassType::Render;
    blur_fg_pass.name = make_name("render_blur_foreground");
    blur_fg_pass.resources = {
        GpuFrameGraph::ResourceDependency::read("scene.layers"),
        GpuFrameGraph::ResourceDependency::write_resource("scene.blur_foreground"),
    };
    blur_fg_pass.render.pipeline_id = "light_eval";
    blur_fg_pass.render.render_state_key = 0x1004u;
    blur_fg_pass.render.color_target = "scene.blur_foreground";
    blur_fg_pass.render.fragment_sampled_textures = {
        GpuFrameGraph::RenderPassPayload::SampledTextureBinding{"scene.layers", "linear_clamp"},
    };
    renderer.add_pass(std::move(blur_fg_pass));

    GpuFrameGraph::PassDescriptor compose_pass{};
    compose_pass.type = GpuFrameGraph::PassType::Render;
    compose_pass.name = make_name("render_scene_composite");
    compose_pass.resources = {
        GpuFrameGraph::ResourceDependency::read("scene.floor"),
        GpuFrameGraph::ResourceDependency::read("scene.layers"),
        GpuFrameGraph::ResourceDependency::read("scene.blur_background"),
        GpuFrameGraph::ResourceDependency::read("scene.blur_foreground"),
        GpuFrameGraph::ResourceDependency::write_resource("scene.composite"),
    };
    compose_pass.render.pipeline_id = "final_compose";
    compose_pass.render.render_state_key = 0x1005u;
    compose_pass.render.color_target = "scene.composite";
    compose_pass.render.fragment_sampled_textures = {
        GpuFrameGraph::RenderPassPayload::SampledTextureBinding{"scene.floor", "linear_clamp"},
        GpuFrameGraph::RenderPassPayload::SampledTextureBinding{"scene.layers", "linear_clamp"},
        GpuFrameGraph::RenderPassPayload::SampledTextureBinding{"scene.blur_background", "linear_clamp"},
        GpuFrameGraph::RenderPassPayload::SampledTextureBinding{"scene.blur_foreground", "linear_clamp"},
    };
    renderer.add_pass(std::move(compose_pass));

    GpuFrameGraph::PassDescriptor present_pass{};
    present_pass.type = GpuFrameGraph::PassType::Copy;
    present_pass.name = make_name("present_scene_composite");
    present_pass.resources = {
        GpuFrameGraph::ResourceDependency::read("scene.composite"),
    };
    present_pass.blit.source_texture = "scene.composite";
    present_pass.blit.use_swapchain_destination = true;
    present_pass.blit.load_op = SDL_GPU_LOADOP_CLEAR;
    present_pass.blit.filter = SDL_GPU_FILTER_LINEAR;
    present_pass.blit.width = width;
    present_pass.blit.height = height;
    renderer.add_pass(std::move(present_pass));
}

