#include <doctest/doctest.h>

#include <SDL3/SDL.h>

#include <filesystem>
#include <vector>
#include <string>
#include <utility>

#include "gameplay/world/chunk.hpp"
#include "rendering/render/gpu_runtime_pipeline.hpp"
#include "rendering/render/gpu_scene_renderer.hpp"
#include "rendering/render/render_diagnostics.hpp"
#include "rendering/render/runtime_gpu_renderer.hpp"
#include "rendering/render/warped_screen_grid.hpp"

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

Area make_starting_area() {
    std::vector<SDL_Point> corners{
        SDL_Point{-100, -100},
        SDL_Point{100, -100},
        SDL_Point{100, 100},
        SDL_Point{-100, 100}};
    return Area("runtime_gpu_renderer_test_start", corners, 0);
}

GpuSpriteDrawPacket make_fullscreen_draw_packet(SDL_Texture* texture) {
    GpuSpriteDrawPacket packet{};
    packet.source_texture = texture;
    packet.modulate = SDL_FColor{1.0f, 1.0f, 1.0f, 1.0f};
    packet.sort_key = 0.0f;
    packet.stable_sort_id = reinterpret_cast<std::uintptr_t>(texture);
    packet.vertices = {
        GpuSpriteVertex{-1.0f, -1.0f, 0.0f, 0.0f},
        GpuSpriteVertex{1.0f, -1.0f, 1.0f, 0.0f},
        GpuSpriteVertex{1.0f, 1.0f, 1.0f, 1.0f},
        GpuSpriteVertex{-1.0f, -1.0f, 0.0f, 0.0f},
        GpuSpriteVertex{1.0f, 1.0f, 1.0f, 1.0f},
        GpuSpriteVertex{-1.0f, 1.0f, 0.0f, 1.0f},
    };
    return packet;
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
    GpuSceneRenderer::SamplerResourceSpec offscreen_sampler_spec{};
    REQUIRE(gpu_renderer->ensure_sampler_resource("linear_clamp", offscreen_sampler_spec, error));

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
        GpuFrameGraph::ResourceDependency{"scene.swapchain", true}
    };
    pass_two.render.pipeline_id = "sprite_textured";
    pass_two.render.render_state_key = 0x1006u;
    pass_two.render.use_swapchain_target = true;
    pass_two.render.clear_color = SDL_FColor{0.0f, 0.0f, 0.0f, 1.0f};
    pass_two.render.load_op = SDL_GPU_LOADOP_CLEAR;
    pass_two.render.store_op = SDL_GPU_STOREOP_STORE;
    pass_two.render.fragment_sampled_textures = {
        GpuFrameGraph::RenderPassPayload::SampledTextureBinding{"scene.output", "linear_clamp"},
    };
    pass_two.render.vertex_count = 3;
    pass_two.render.instance_count = 1;
    gpu_renderer->add_pass(std::move(pass_two));

    REQUIRE(gpu_renderer->end_frame(&error));
    render_diagnostics::end_frame();
    const RenderFrameStats stats = render_diagnostics::current_frame_stats();
    CHECK(stats.sdl_renderer_target_call_count == 0);
    CHECK(stats.sdl_renderer_draw_call_count == 0);
    CHECK(stats.present_call_count == 0);

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

    GpuRuntimePipeline pipeline{};
    REQUIRE(pipeline.ensure_resources(*gpu_renderer, 128u, 128u, error));
    REQUIRE(pipeline.ensure_shared_resources(*gpu_renderer, error));

    render_diagnostics::begin_frame();
    if (!gpu_renderer->begin_frame(&error, false)) {
        render_diagnostics::end_frame();
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        return;
    }
    gpu_renderer->reset_frame_graph();

    GpuSceneFrameData frame_data{};
    frame_data.has_valid_composite_source = false;
    REQUIRE(pipeline.enqueue_frame_graph(*gpu_renderer, frame_data, "runtime", 128u, 128u, error));

    REQUIRE(gpu_renderer->end_frame(&error));
    render_diagnostics::end_frame();

    const RenderFrameStats stats = render_diagnostics::current_frame_stats();
    CHECK(stats.render_pass_count == 6);
    CHECK(stats.copy_pass_count == 0);
    CHECK(stats.sdl_renderer_target_call_count == 0);
    CHECK(stats.sdl_renderer_draw_call_count == 0);
    CHECK(stats.present_call_count == 0);

    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
}

TEST_CASE("GPU runtime map floor packet helper emits packets for chunk tiles") {
    ScopedSdlVideo sdl_video{};
    REQUIRE(sdl_video.initialized());

    SDL_Window* window = SDL_CreateWindow("gpu_runtime_floor_packets", 128, 128, SDL_WINDOW_HIDDEN);
    REQUIRE(window != nullptr);
    SDL_Renderer* renderer = create_gpu_renderer(window);
    if (!renderer || !renderer_has_gpu_device(renderer)) {
        if (renderer) {
            SDL_DestroyRenderer(renderer);
        }
        SDL_DestroyWindow(window);
        return;
    }

    SDL_Texture* tile_texture = SDL_CreateTexture(renderer,
                                                  SDL_PIXELFORMAT_RGBA8888,
                                                  SDL_TEXTUREACCESS_STATIC,
                                                  8,
                                                  8);
    REQUIRE(tile_texture != nullptr);

    world::Chunk chunk{};
    GridTile tile{};
    tile.world_rect = SDL_Rect{0, 0, 32, 32};
    tile.texture = tile_texture;
    chunk.tiles.push_back(tile);

    WarpedScreenGrid grid(128, 128, make_starting_area());
    std::vector<world::Chunk*> chunks{&chunk};
    std::vector<GpuSpriteDrawPacket> packets{};
    REQUIRE(runtime_gpu_renderer_detail::build_map_floor_tile_draw_packets(
        grid,
        chunks,
        128u,
        128u,
        packets));
    CHECK_FALSE(packets.empty());

    chunk.releaseTileTextures();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
}

TEST_CASE("GPU runtime floor-tagged packet classification routes to floor and layer packets") {
    GpuSceneFrameData frame_data{};
    GpuSpriteDrawPacket packet{};

    runtime_gpu_renderer_detail::append_classified_sprite_draw_packet(true, packet, frame_data);
    CHECK(frame_data.floor_sprite_draws.size() == 1);
    CHECK(frame_data.layer_draws.empty());

    runtime_gpu_renderer_detail::append_classified_sprite_draw_packet(false, packet, frame_data);
    CHECK(frame_data.floor_sprite_draws.size() == 1);
    CHECK(frame_data.layer_draws.size() == 1);
}

TEST_CASE("GPU runtime composite path remains valid when one source pass is empty") {
    ScopedSdlVideo sdl_video{};
    REQUIRE(sdl_video.initialized());

    SDL_Window* window = SDL_CreateWindow("gpu_runtime_composite_empty_source", 128, 128, SDL_WINDOW_HIDDEN);
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

    GpuRuntimePipeline pipeline{};
    REQUIRE(pipeline.ensure_resources(*gpu_renderer, 128u, 128u, error));
    REQUIRE(pipeline.ensure_shared_resources(*gpu_renderer, error));

    SDL_Texture* draw_texture = SDL_CreateTexture(renderer,
                                                  SDL_PIXELFORMAT_RGBA8888,
                                                  SDL_TEXTUREACCESS_STATIC,
                                                  8,
                                                  8);
    REQUIRE(draw_texture != nullptr);

    const auto run_frame = [&](const GpuSceneFrameData& frame_data, std::string_view pass_prefix) {
        std::string frame_error;
        render_diagnostics::begin_frame();
        if (!gpu_renderer->begin_frame(&frame_error, false)) {
            render_diagnostics::end_frame();
            return false;
        }
        gpu_renderer->reset_frame_graph();
        if (!pipeline.enqueue_frame_graph(*gpu_renderer, frame_data, pass_prefix, 128u, 128u, frame_error)) {
            gpu_renderer->abort_frame();
            render_diagnostics::end_frame();
            return false;
        }
        const bool ok = gpu_renderer->end_frame(&frame_error);
        render_diagnostics::end_frame();
        return ok;
    };

    GpuSceneFrameData floor_only{};
    floor_only.floor_sprite_draws.push_back(make_fullscreen_draw_packet(draw_texture));
    floor_only.floor_draw_count = 1;
    floor_only.floor_sprite_draw_count = 1;
    floor_only.layer_sprite_draw_count = 0;
    floor_only.has_valid_composite_source = true;
    CHECK(run_frame(floor_only, "runtime_floor_only"));

    GpuSceneFrameData layer_only{};
    layer_only.layer_draws.push_back(make_fullscreen_draw_packet(draw_texture));
    layer_only.floor_draw_count = 0;
    layer_only.floor_sprite_draw_count = 0;
    layer_only.layer_sprite_draw_count = 1;
    layer_only.has_valid_composite_source = true;
    CHECK(run_frame(layer_only, "runtime_layer_only"));

    SDL_DestroyTexture(draw_texture);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
}

TEST_CASE("GPU runtime texture resolve rejects non-bridged textures without readback fallback") {
    ScopedSdlVideo sdl_video{};
    REQUIRE(sdl_video.initialized());

    SDL_Window* gpu_window = SDL_CreateWindow("gpu_runtime_resolve_gpu_window", 64, 64, SDL_WINDOW_HIDDEN);
    REQUIRE(gpu_window != nullptr);
    SDL_Renderer* gpu_renderer_backend = create_gpu_renderer(gpu_window);
    if (!gpu_renderer_backend || !renderer_has_gpu_device(gpu_renderer_backend)) {
        if (gpu_renderer_backend) {
            SDL_DestroyRenderer(gpu_renderer_backend);
        }
        SDL_DestroyWindow(gpu_window);
        return;
    }

    std::string error;
    std::unique_ptr<GpuSceneRenderer> gpu_renderer = GpuSceneRenderer::Create(gpu_renderer_backend, false, error);
    REQUIRE(gpu_renderer != nullptr);

    SDL_Window* software_window = SDL_CreateWindow("gpu_runtime_resolve_software_window", 64, 64, SDL_WINDOW_HIDDEN);
    REQUIRE(software_window != nullptr);
    SDL_Renderer* software_renderer = SDL_CreateRenderer(software_window, SDL_SOFTWARE_RENDERER);
    if (!software_renderer) {
        SDL_DestroyRenderer(gpu_renderer_backend);
        SDL_DestroyWindow(gpu_window);
        SDL_DestroyWindow(software_window);
        return;
    }

    SDL_Texture* software_texture = SDL_CreateTexture(software_renderer,
                                                      SDL_PIXELFORMAT_RGBA8888,
                                                      SDL_TEXTUREACCESS_STATIC,
                                                      4,
                                                      4);
    REQUIRE(software_texture != nullptr);

    SDL_GPUTexture* resolved = gpu_renderer->resolve_gpu_texture_for_sdl_texture(software_texture, error);
    CHECK(resolved == nullptr);
    CHECK(error.find("readback fallback is disabled") != std::string::npos);

    SDL_DestroyTexture(software_texture);
    SDL_DestroyRenderer(software_renderer);
    SDL_DestroyWindow(software_window);
    SDL_DestroyRenderer(gpu_renderer_backend);
    SDL_DestroyWindow(gpu_window);
}
