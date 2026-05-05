#include <doctest/doctest.h>

#include <SDL3/SDL.h>

#include <filesystem>
#include <string>
#include <utility>

#include "rendering/render/gpu_scene_renderer.hpp"
#include "rendering/render/render_diagnostics.hpp"

namespace {

class ScopedSdlVideo {
public:
    ScopedSdlVideo() : initialized_(SDL_InitSubSystem(SDL_INIT_VIDEO)) {}
    ~ScopedSdlVideo() {
        if (initialized_) {
            SDL_QuitSubSystem(SDL_INIT_VIDEO);
        }
    }

    bool initialized() const { return initialized_; }

private:
    bool initialized_ = false;
};

std::filesystem::path find_shader_manifest_path() {
    const std::filesystem::path cwd = std::filesystem::current_path();
    const std::filesystem::path candidates[] = {
        cwd / "ENGINE" / "runtime" / "rendering" / "shaders" / "runtime_shaders.json",
        cwd / "runtime" / "rendering" / "shaders" / "runtime_shaders.json",
        cwd.parent_path() / "runtime" / "rendering" / "shaders" / "runtime_shaders.json",
        cwd.parent_path() / "ENGINE" / "runtime" / "rendering" / "shaders" / "runtime_shaders.json",
    };
    for (const std::filesystem::path& candidate : candidates) {
        if (std::filesystem::exists(candidate)) {
            return candidate;
        }
    }
    return {};
}

SDL_Renderer* create_gpu_renderer(SDL_Window* window) {
    if (!window) {
        return nullptr;
    }
    SDL_PropertiesID props = SDL_CreateProperties();
    if (!props) {
        return nullptr;
    }
    SDL_SetPointerProperty(props, SDL_PROP_RENDERER_CREATE_WINDOW_POINTER, window);
    SDL_SetStringProperty(props, SDL_PROP_RENDERER_CREATE_NAME_STRING, SDL_GPU_RENDERER);
    SDL_SetNumberProperty(props, SDL_PROP_RENDERER_CREATE_PRESENT_VSYNC_NUMBER, 0);
    SDL_Renderer* renderer = SDL_CreateRendererWithProperties(props);
    SDL_DestroyProperties(props);
    return renderer;
}

bool renderer_has_gpu_device(SDL_Renderer* renderer) {
    if (!renderer) {
        return false;
    }
    SDL_PropertiesID props = SDL_GetRendererProperties(renderer);
    if (!props) {
        return false;
    }
    return SDL_GetPointerProperty(props, SDL_PROP_RENDERER_GPU_DEVICE_POINTER, nullptr) != nullptr;
}

} // namespace

TEST_CASE("GPU runtime frame executes with zero SDL_Renderer target/draw calls") {
    ScopedSdlVideo sdl_video{};
    REQUIRE(sdl_video.initialized());

    SDL_Window* window = SDL_CreateWindow("gpu_runtime_no_sdl_renderer_calls", 128, 128, SDL_WINDOW_HIDDEN);
    REQUIRE(window != nullptr);
    SDL_Renderer* renderer = create_gpu_renderer(window);
    if (!renderer || !renderer_has_gpu_device(renderer)) {
        if (renderer) {
            SDL_DestroyRenderer(renderer);
        }
        SDL_DestroyWindow(window);
        return;
    }

    std::string error;
    std::unique_ptr<GpuSceneRenderer> gpu_renderer = GpuSceneRenderer::Create(renderer, false, error);
    REQUIRE(gpu_renderer != nullptr);

    const std::filesystem::path manifest_path = find_shader_manifest_path();
    if (manifest_path.empty()) {
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        return;
    }
    REQUIRE(gpu_renderer->load_shader_packages(manifest_path.string(), error));

    const RuntimeGpuFormatPolicy& format_policy = gpu_renderer->device()->format_policy();
    GpuSceneRenderer::TextureResourceSpec offscreen_spec{};
    offscreen_spec.width = 128;
    offscreen_spec.height = 128;
    offscreen_spec.format = format_policy.albedo_format;
    offscreen_spec.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
    offscreen_spec.layer_count_or_depth = 1;
    offscreen_spec.num_levels = 1;
    offscreen_spec.sample_count = format_policy.sample_count;
    REQUIRE(gpu_renderer->ensure_texture_resource("scene_output", offscreen_spec, error));

    render_diagnostics::begin_frame();
    if (!gpu_renderer->begin_frame(&error)) {
        render_diagnostics::end_frame();
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        return;
    }

    GpuFrameGraph::PassDescriptor pass_one{};
    pass_one.type = GpuFrameGraph::PassType::Render;
    pass_one.name = "offscreen_render";
    pass_one.resources = {GpuFrameGraph::ResourceDependency{"scene.output", true}};
    pass_one.render.pipeline_id = "floor_compose";
    pass_one.render.render_state_key = 0x1001u;
    pass_one.render.color_target = "scene_output";
    pass_one.render.use_swapchain_target = false;
    pass_one.render.clear_color = SDL_FColor{0.0f, 0.0f, 0.0f, 1.0f};
    pass_one.render.load_op = SDL_GPU_LOADOP_CLEAR;
    pass_one.render.store_op = SDL_GPU_STOREOP_STORE;
    pass_one.render.vertex_count = 3;
    pass_one.render.instance_count = 1;
    gpu_renderer->add_pass(std::move(pass_one));

    GpuFrameGraph::PassDescriptor pass_two{};
    pass_two.type = GpuFrameGraph::PassType::Render;
    pass_two.name = "present_render";
    pass_two.resources = {
        GpuFrameGraph::ResourceDependency{"scene.output", false},
        GpuFrameGraph::ResourceDependency{"scene.present", true}
    };
    pass_two.render.pipeline_id = "final_compose";
    pass_two.render.render_state_key = 0x1006u;
    pass_two.render.use_swapchain_target = true;
    pass_two.render.clear_color = SDL_FColor{0.0f, 0.0f, 0.0f, 1.0f};
    pass_two.render.load_op = SDL_GPU_LOADOP_CLEAR;
    pass_two.render.store_op = SDL_GPU_STOREOP_STORE;
    pass_two.render.vertex_count = 3;
    pass_two.render.instance_count = 1;
    gpu_renderer->add_pass(std::move(pass_two));

    REQUIRE(gpu_renderer->end_frame(&error));
    render_diagnostics::end_frame();
    const RenderFrameStats stats = render_diagnostics::current_frame_stats();
    CHECK(stats.sdl_renderer_target_call_count == 0);
    CHECK(stats.sdl_renderer_draw_call_count == 0);

    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
}

TEST_CASE("GPU authoritative frame-graph runtime smoke executes full topology and presents") {
    ScopedSdlVideo sdl_video{};
    REQUIRE(sdl_video.initialized());

    SDL_Window* window = SDL_CreateWindow("gpu_runtime_authoritative_graph_smoke", 128, 128, SDL_WINDOW_HIDDEN);
    REQUIRE(window != nullptr);
    SDL_Renderer* renderer = create_gpu_renderer(window);
    if (!renderer || !renderer_has_gpu_device(renderer)) {
        if (renderer) {
            SDL_DestroyRenderer(renderer);
        }
        SDL_DestroyWindow(window);
        return;
    }

    std::string error;
    std::unique_ptr<GpuSceneRenderer> gpu_renderer = GpuSceneRenderer::Create(renderer, false, error);
    REQUIRE(gpu_renderer != nullptr);

    const std::filesystem::path manifest_path = find_shader_manifest_path();
    if (manifest_path.empty()) {
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        return;
    }
    REQUIRE(gpu_renderer->load_shader_packages(manifest_path.string(), error));

    const RuntimeGpuFormatPolicy& format_policy = gpu_renderer->device()->format_policy();
    GpuSceneRenderer::TextureResourceSpec spec{};
    spec.width = 128;
    spec.height = 128;
    spec.format = format_policy.albedo_format;
    spec.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
    spec.layer_count_or_depth = 1;
    spec.num_levels = 1;
    spec.sample_count = SDL_GPU_SAMPLECOUNT_1;
    REQUIRE(gpu_renderer->ensure_texture_resource("scene.floor", spec, error));
    REQUIRE(gpu_renderer->ensure_texture_resource("scene.layers", spec, error));
    REQUIRE(gpu_renderer->ensure_texture_resource("scene.blur_background", spec, error));
    REQUIRE(gpu_renderer->ensure_texture_resource("scene.blur_foreground", spec, error));
    REQUIRE(gpu_renderer->ensure_texture_resource("scene.composite", spec, error));

    GpuSceneRenderer::SamplerResourceSpec sampler_spec{};
    REQUIRE(gpu_renderer->ensure_sampler_resource("linear_clamp", sampler_spec, error));

    render_diagnostics::begin_frame();
    if (!gpu_renderer->begin_frame(&error)) {
        render_diagnostics::end_frame();
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        return;
    }

    GpuFrameGraph::PassDescriptor floor_pass{};
    floor_pass.type = GpuFrameGraph::PassType::Render;
    floor_pass.name = "runtime_render_floor";
    floor_pass.resources = {GpuFrameGraph::ResourceDependency::write_resource("scene.floor")};
    floor_pass.render.pipeline_id = "floor_compose";
    floor_pass.render.color_target = "scene.floor";
    gpu_renderer->add_pass(std::move(floor_pass));

    GpuFrameGraph::PassDescriptor layers_pass{};
    layers_pass.type = GpuFrameGraph::PassType::Render;
    layers_pass.name = "runtime_render_layers";
    layers_pass.resources = {GpuFrameGraph::ResourceDependency::write_resource("scene.layers")};
    layers_pass.render.pipeline_id = "sprite_textured";
    layers_pass.render.color_target = "scene.layers";
    gpu_renderer->add_pass(std::move(layers_pass));

    GpuFrameGraph::PassDescriptor blur_bg_pass{};
    blur_bg_pass.type = GpuFrameGraph::PassType::Render;
    blur_bg_pass.name = "runtime_render_blur_background";
    blur_bg_pass.resources = {
        GpuFrameGraph::ResourceDependency::read("scene.layers"),
        GpuFrameGraph::ResourceDependency::write_resource("scene.blur_background"),
    };
    blur_bg_pass.render.pipeline_id = "dark_mask";
    blur_bg_pass.render.color_target = "scene.blur_background";
    blur_bg_pass.render.fragment_sampled_textures = {
        GpuFrameGraph::RenderPassPayload::SampledTextureBinding{"scene.layers", "linear_clamp"},
    };
    gpu_renderer->add_pass(std::move(blur_bg_pass));

    GpuFrameGraph::PassDescriptor blur_fg_pass{};
    blur_fg_pass.type = GpuFrameGraph::PassType::Render;
    blur_fg_pass.name = "runtime_render_blur_foreground";
    blur_fg_pass.resources = {
        GpuFrameGraph::ResourceDependency::read("scene.layers"),
        GpuFrameGraph::ResourceDependency::write_resource("scene.blur_foreground"),
    };
    blur_fg_pass.render.pipeline_id = "light_eval";
    blur_fg_pass.render.color_target = "scene.blur_foreground";
    blur_fg_pass.render.fragment_sampled_textures = {
        GpuFrameGraph::RenderPassPayload::SampledTextureBinding{"scene.layers", "linear_clamp"},
    };
    gpu_renderer->add_pass(std::move(blur_fg_pass));

    GpuFrameGraph::PassDescriptor composite_pass{};
    composite_pass.type = GpuFrameGraph::PassType::Render;
    composite_pass.name = "runtime_render_scene_composite";
    composite_pass.resources = {
        GpuFrameGraph::ResourceDependency::read("scene.floor"),
        GpuFrameGraph::ResourceDependency::read("scene.layers"),
        GpuFrameGraph::ResourceDependency::read("scene.blur_background"),
        GpuFrameGraph::ResourceDependency::read("scene.blur_foreground"),
        GpuFrameGraph::ResourceDependency::write_resource("scene.composite"),
    };
    composite_pass.render.pipeline_id = "final_compose";
    composite_pass.render.color_target = "scene.composite";
    composite_pass.render.fragment_sampled_textures = {
        GpuFrameGraph::RenderPassPayload::SampledTextureBinding{"scene.floor", "linear_clamp"},
        GpuFrameGraph::RenderPassPayload::SampledTextureBinding{"scene.layers", "linear_clamp"},
        GpuFrameGraph::RenderPassPayload::SampledTextureBinding{"scene.blur_background", "linear_clamp"},
        GpuFrameGraph::RenderPassPayload::SampledTextureBinding{"scene.blur_foreground", "linear_clamp"},
    };
    gpu_renderer->add_pass(std::move(composite_pass));

    GpuFrameGraph::PassDescriptor present_pass{};
    present_pass.type = GpuFrameGraph::PassType::Copy;
    present_pass.name = "runtime_present_scene_composite";
    present_pass.resources = {
        GpuFrameGraph::ResourceDependency::read("scene.composite"),
    };
    present_pass.blit.source_texture = "scene.composite";
    present_pass.blit.use_swapchain_destination = true;
    present_pass.blit.load_op = SDL_GPU_LOADOP_CLEAR;
    present_pass.blit.filter = SDL_GPU_FILTER_LINEAR;
    present_pass.blit.width = 128;
    present_pass.blit.height = 128;
    gpu_renderer->add_pass(std::move(present_pass));

    REQUIRE(gpu_renderer->end_frame(&error));
    render_diagnostics::end_frame();

    const RenderFrameStats stats = render_diagnostics::current_frame_stats();
    CHECK(stats.render_pass_count == 5);
    CHECK(stats.copy_pass_count == 1);
    CHECK(stats.sdl_renderer_target_call_count == 0);
    CHECK(stats.sdl_renderer_draw_call_count == 0);

    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
}
