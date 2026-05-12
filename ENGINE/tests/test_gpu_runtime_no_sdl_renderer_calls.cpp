#include <doctest/doctest.h>

#include <SDL3/SDL.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <unordered_set>
#include <vector>
#include <string>
#include <utility>

#include "assets/asset/animation.hpp"
#include "assets/asset/animation_cloner.hpp"
#include "assets/asset/asset_info.hpp"
#include "assets/asset/Asset.hpp"
#include "gameplay/world/chunk.hpp"
#include "rendering/render/gpu_scene_renderer.hpp"
#include "rendering/render/render_diagnostics.hpp"
#include "rendering/render/opengl_runtime_renderer.hpp"
#include "rendering/render/warped_screen_grid.hpp"
#include "utils/cache_manager.hpp"
#include "stubs/asset/child_asset_runtime_test_support.hpp"

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

SDL_Texture* make_prepared_texture(SDL_Renderer* renderer, Uint32 pixel_rgba, int width, int height) {
    SDL_Surface* surface = SDL_CreateSurface(width, height, SDL_PIXELFORMAT_RGBA32);
    if (!surface) {
        return nullptr;
    }
    SDL_FillSurfaceRect(surface, nullptr, pixel_rgba);
    SDL_Texture* texture = CacheManager::surface_to_texture(renderer, surface);
    SDL_DestroySurface(surface);
    return texture;
}

GpuSpriteDrawPacket make_fullscreen_packet(SDL_Texture* texture,
                                           bool floor_packet,
                                           int depth_layer,
                                           std::uintptr_t stable_sort_id) {
    GpuSpriteDrawPacket packet{};
    packet.source_texture = texture;
    packet.source_asset_name = floor_packet ? "<floor>" : "<layer>";
    packet.source_animation_name = floor_packet ? "<floor>" : "<layer>";
    packet.source_texture_id = "texture_ptr=" + std::to_string(reinterpret_cast<std::uintptr_t>(texture));
    packet.source_frame_index = 0;
    packet.source_variant_index = 0;
    packet.sort_group = 0;
    packet.sort_key = 0.0f;
    packet.stable_sort_id = stable_sort_id;
    packet.is_floor_packet = floor_packet;
    packet.depth_layer = depth_layer;
    packet.modulate = SDL_FColor{1.0f, 1.0f, 1.0f, 1.0f};
    packet.vertices[0] = GpuSpriteVertex{-1.0f, -1.0f, 0.0f, 0.0f};
    packet.vertices[1] = GpuSpriteVertex{1.0f, -1.0f, 1.0f, 0.0f};
    packet.vertices[2] = GpuSpriteVertex{1.0f, 1.0f, 1.0f, 1.0f};
    packet.vertices[3] = GpuSpriteVertex{-1.0f, -1.0f, 0.0f, 0.0f};
    packet.vertices[4] = GpuSpriteVertex{1.0f, 1.0f, 1.0f, 1.0f};
    packet.vertices[5] = GpuSpriteVertex{-1.0f, 1.0f, 0.0f, 1.0f};
    return packet;
}

GpuSceneFrameData make_staged_frame_data(SDL_Texture* floor_texture,
                                         SDL_Texture* focused_texture,
                                         SDL_Texture* blurred_texture) {
    GpuSceneFrameData frame_data{};
    const GpuSpriteDrawPacket floor_packet = make_fullscreen_packet(floor_texture, true, 0, 0u);
    const GpuSpriteDrawPacket focus_packet = make_fullscreen_packet(focused_texture, false, 0, 1u);
    const GpuSpriteDrawPacket blur_packet = make_fullscreen_packet(blurred_texture, false, 1, 2u);

    frame_data.floor_draws = {floor_packet};
    frame_data.layer_draws = {focus_packet, blur_packet};
    frame_data.depth_layers = {
        GpuDepthLayerDrawPackets{1, 12.0f, {blur_packet}},
        GpuDepthLayerDrawPackets{0, 0.0f, {focus_packet}},
    };
    frame_data.floor_draw_count = 1u;
    frame_data.layer_sprite_draw_count = 2u;
    frame_data.active_depth_layer_count = 2u;
    frame_data.has_valid_composite_source = true;
    return frame_data;
}

bool resolve_frame_data_textures(GpuSceneRenderer& renderer,
                                 GpuSceneFrameData& frame_data,
                                 std::string& out_error) {
    out_error.clear();
    const auto resolve_packet = [&renderer, &out_error](GpuSpriteDrawPacket& packet) -> bool {
        if (!packet.source_texture) {
            out_error = "Packet source texture was null.";
            return false;
        }
        packet.source_gpu_texture = renderer.resolve_gpu_texture_for_sdl_texture(packet.source_texture, out_error);
        return packet.source_gpu_texture != nullptr;
    };

    for (GpuSpriteDrawPacket& packet : frame_data.floor_draws) {
        if (!resolve_packet(packet)) {
            return false;
        }
    }
    for (GpuSpriteDrawPacket& packet : frame_data.layer_draws) {
        if (!resolve_packet(packet)) {
            return false;
        }
    }
    for (GpuDepthLayerDrawPackets& layer : frame_data.depth_layers) {
        for (GpuSpriteDrawPacket& packet : layer.packets) {
            if (!resolve_packet(packet)) {
                return false;
            }
        }
    }
    return true;
}

void destroy_prepared_texture(SDL_Texture*& texture) {
    if (!texture) {
        return;
    }
    CacheManager::unregister_prepared_gpu_upload(texture);
    SDL_DestroyTexture(texture);
    texture = nullptr;
}

} // namespace

TEST_CASE("CacheManager prepared GPU uploads preserve RGBA byte order") {
    ScopedSdlVideo sdl_video{};
    REQUIRE(sdl_video.initialized());

    SDL_Window* window = SDL_CreateWindow("cache_manager_rgba32_upload_window", 16, 16, SDL_WINDOW_HIDDEN);
    REQUIRE(window != nullptr);
    SDL_Renderer* renderer = SDL_CreateRenderer(window, SDL_SOFTWARE_RENDERER);
    if (!renderer) {
        SDL_DestroyWindow(window);
        return;
    }

    SDL_Surface* surface = SDL_CreateSurface(1, 1, SDL_PIXELFORMAT_RGBA32);
    REQUIRE(surface != nullptr);
    REQUIRE(surface->pixels != nullptr);
    REQUIRE(surface->pitch >= 4);
    auto* bytes = static_cast<std::uint8_t*>(surface->pixels);
    bytes[0] = 0x20u;
    bytes[1] = 0x40u;
    bytes[2] = 0x80u;
    bytes[3] = 0xFFu;

    SDL_Texture* texture = CacheManager::surface_to_texture(renderer, surface);
    SDL_DestroySurface(surface);
    REQUIRE(texture != nullptr);

    const CacheManager::PreparedGpuTextureUpload* prepared =
        CacheManager::prepared_gpu_upload_for_texture(texture);
    REQUIRE(prepared != nullptr);
    CHECK(prepared->format == SDL_PIXELFORMAT_RGBA32);
    CHECK(prepared->width == 1);
    CHECK(prepared->height == 1);
    CHECK(prepared->pitch == 4);
    REQUIRE(prepared->pixels.size() >= 4);
    CHECK(prepared->pixels[0] == 0x20u);
    CHECK(prepared->pixels[1] == 0x40u);
    CHECK(prepared->pixels[2] == 0x80u);
    CHECK(prepared->pixels[3] == 0xFFu);

    CacheManager::unregister_prepared_gpu_upload(texture);
    SDL_DestroyTexture(texture);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
}

TEST_CASE("GPU runtime frame executes staged floor/layer/composite path without SDL_Renderer calls") {
    ScopedSdlVideo sdl_video{};
    REQUIRE(sdl_video.initialized());

    SDL_Window* window = SDL_CreateWindow("gpu_runtime_staged_renderer", 128, 128, SDL_WINDOW_HIDDEN);
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
    SDL_Window* software_window = SDL_CreateWindow("gpu_runtime_staged_renderer_software", 64, 64, SDL_WINDOW_HIDDEN);
    REQUIRE(software_window != nullptr);
    SDL_Renderer* software_renderer = SDL_CreateRenderer(software_window, SDL_SOFTWARE_RENDERER);
    if (!software_renderer) {
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_DestroyWindow(software_window);
        return;
    }

    std::unique_ptr<GpuSceneRenderer> gpu_renderer = GpuSceneRenderer::Create(renderer, false, error);
    REQUIRE(gpu_renderer != nullptr);

    const std::filesystem::path manifest_path = find_shader_manifest_path();
    if (manifest_path.empty()) {
        SDL_DestroyRenderer(software_renderer);
        SDL_DestroyWindow(software_window);
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        return;
    }
    REQUIRE(gpu_renderer->load_shader_packages(manifest_path.string(), error));

    SDL_Texture* floor_texture = make_prepared_texture(software_renderer, 0xFF804020u, 4, 4);
    SDL_Texture* focus_texture = make_prepared_texture(software_renderer, 0xFF2050FFu, 4, 4);
    SDL_Texture* blurred_texture = make_prepared_texture(software_renderer, 0xFF20A050u, 4, 4);
    REQUIRE(floor_texture != nullptr);
    REQUIRE(focus_texture != nullptr);
    REQUIRE(blurred_texture != nullptr);

    GpuSceneFrameData frame_data = make_staged_frame_data(floor_texture, focus_texture, blurred_texture);
    REQUIRE(resolve_frame_data_textures(*gpu_renderer, frame_data, error));
    render_diagnostics::begin_frame();
    REQUIRE_MESSAGE(gpu_renderer->render_frame(frame_data, error), error);
    render_diagnostics::end_frame();

    const RenderFrameStats stats = render_diagnostics::current_frame_stats();
    CHECK(stats.sdl_renderer_target_call_count == 0);
    CHECK(stats.sdl_renderer_draw_call_count == 0);
    CHECK(stats.present_call_count == 0);
    CHECK(stats.floor_target_width == 128u);
    CHECK(stats.floor_target_height == 128u);
    CHECK(stats.floor_packet_count == 1u);
    CHECK(stats.sprite_packet_count == 2u);
    CHECK(stats.active_depth_layer_count == 2u);
    CHECK(stats.clear_executed);
    CHECK(stats.blur_pass_count == 3u);
    CHECK(stats.render_pass_count == 8u);
    CHECK(stats.submit_succeeded);
    CHECK(stats.packets_per_depth_layer == "1=1, 0=1");
    CHECK(stats.blur_strength_per_layer == "1=12, 0=0");
    CHECK(stats.composite_layers_submitted == "floor -> 1 -> 0");

    destroy_prepared_texture(floor_texture);
    destroy_prepared_texture(focus_texture);
    destroy_prepared_texture(blurred_texture);
    SDL_DestroyRenderer(software_renderer);
    SDL_DestroyWindow(software_window);

    gpu_renderer.reset();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
}

TEST_CASE("GPU runtime floor-only frame uses opaque composite path") {
    ScopedSdlVideo sdl_video{};
    REQUIRE(sdl_video.initialized());

    SDL_Window* window = SDL_CreateWindow("gpu_runtime_floor_only_renderer", 128, 128, SDL_WINDOW_HIDDEN);
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
    SDL_Window* software_window = SDL_CreateWindow("gpu_runtime_floor_only_renderer_software", 64, 64, SDL_WINDOW_HIDDEN);
    REQUIRE(software_window != nullptr);
    SDL_Renderer* software_renderer = SDL_CreateRenderer(software_window, SDL_SOFTWARE_RENDERER);
    if (!software_renderer) {
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_DestroyWindow(software_window);
        return;
    }

    std::unique_ptr<GpuSceneRenderer> gpu_renderer = GpuSceneRenderer::Create(renderer, false, error);
    REQUIRE(gpu_renderer != nullptr);

    const std::filesystem::path manifest_path = find_shader_manifest_path();
    if (manifest_path.empty()) {
        SDL_DestroyRenderer(software_renderer);
        SDL_DestroyWindow(software_window);
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        return;
    }
    REQUIRE(gpu_renderer->load_shader_packages(manifest_path.string(), error));

    SDL_Texture* floor_texture = make_prepared_texture(software_renderer, 0xFF804020u, 4, 4);
    REQUIRE(floor_texture != nullptr);

    GpuSceneFrameData frame_data{};
    frame_data.floor_draws = {make_fullscreen_packet(floor_texture, true, 0, 0u)};
    frame_data.floor_draw_count = 1u;
    frame_data.has_valid_composite_source = true;
    REQUIRE(resolve_frame_data_textures(*gpu_renderer, frame_data, error));

    render_diagnostics::begin_frame();
    REQUIRE_MESSAGE(gpu_renderer->render_frame(frame_data, error), error);
    render_diagnostics::end_frame();

    const RenderFrameStats stats = render_diagnostics::current_frame_stats();
    CHECK(stats.floor_packet_count == 1u);
    CHECK(stats.sprite_packet_count == 0u);
    CHECK(stats.active_depth_layer_count == 0u);
    CHECK(stats.render_pass_count == 3u);
    CHECK(stats.submit_succeeded);
    CHECK(stats.composite_layers_submitted == "floor");

    destroy_prepared_texture(floor_texture);
    SDL_DestroyRenderer(software_renderer);
    SDL_DestroyWindow(software_window);

    gpu_renderer.reset();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
}

TEST_CASE("GPU runtime floor tile packet helper emits packets for chunk tiles") {
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
                                                  SDL_PIXELFORMAT_RGBA32,
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
    REQUIRE(runtime_gpu_renderer_detail::build_floor_tile_draw_packets(
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

TEST_CASE("GPU runtime classified packet helper routes to floor and layer packets") {
    std::vector<GpuSpriteDrawPacket> floor_draws{};
    std::vector<GpuSpriteDrawPacket> layer_draws{};
    GpuSpriteDrawPacket packet{};

    runtime_gpu_renderer_detail::append_classified_sprite_draw_packet(true,
                                                                      packet,
                                                                      floor_draws,
                                                                      layer_draws);
    CHECK(floor_draws.size() == 1);
    CHECK(layer_draws.empty());

    runtime_gpu_renderer_detail::append_classified_sprite_draw_packet(false,
                                                                      packet,
                                                                      floor_draws,
                                                                      layer_draws);
    CHECK(floor_draws.size() == 1);
    CHECK(layer_draws.size() == 1);
}

TEST_CASE("GPU runtime depth layers and floor packets stay deterministic") {
    ScopedSdlVideo sdl_video{};
    REQUIRE(sdl_video.initialized());

    WarpedScreenGrid grid(128, 128, make_starting_area());
    auto& settings = grid.get_settings();
    settings.layer_depth_interval = 100.0f;
    settings.layer_depth_curve = 0.0f;
    settings.max_cull_depth = 1000.0f;

    auto focus_asset = test_child_asset_runtime::make_test_asset("focus_asset", 0, 0, 0, 0);
    auto background_asset = test_child_asset_runtime::make_test_asset("background_asset", 0, 0, 250, 0);
    auto foreground_asset = test_child_asset_runtime::make_test_asset("foreground_asset", 0, 0, -250, 0);

    CHECK(runtime_gpu_renderer_detail::classify_depth_layer_for_asset(grid, *focus_asset) == 0);
    CHECK(runtime_gpu_renderer_detail::classify_depth_layer_for_asset(grid, *background_asset) > 0);
    CHECK(runtime_gpu_renderer_detail::classify_depth_layer_for_asset(grid, *foreground_asset) < 0);

    std::vector<GpuSpriteDrawPacket> floor_draws{};
    std::vector<GpuSpriteDrawPacket> layer_draws{};
    GpuSpriteDrawPacket packet{};
    packet.is_floor_packet = true;
    packet.depth_layer = 0;
    runtime_gpu_renderer_detail::append_classified_sprite_draw_packet(true,
                                                                      packet,
                                                                      floor_draws,
                                                                      layer_draws);
    CHECK(floor_draws.size() == 1);
    CHECK(floor_draws[0].is_floor_packet);
    CHECK(floor_draws[0].depth_layer == 0);
    CHECK(layer_draws.empty());

    packet.is_floor_packet = false;
    packet.depth_layer = 4;
    runtime_gpu_renderer_detail::append_classified_sprite_draw_packet(false,
                                                                      packet,
                                                                      floor_draws,
                                                                      layer_draws);
    CHECK(floor_draws.size() == 1);
    CHECK(layer_draws.size() == 1);
    CHECK_FALSE(layer_draws[0].is_floor_packet);
    CHECK(layer_draws[0].depth_layer == 4);
}

TEST_CASE("GPU runtime draw comparator uses depth metric when screen-bottom sort key ties") {
    GpuSpriteDrawPacket nearer{};
    nearer.sort_key = 42.0f;
    nearer.depth_metric = 10.0f;
    nearer.stable_sort_id = 2u;

    GpuSpriteDrawPacket farther{};
    farther.sort_key = 42.0f;
    farther.depth_metric = 20.0f;
    farther.stable_sort_id = 1u;

    std::vector<GpuSpriteDrawPacket> layer_packets{farther, nearer};
    std::stable_sort(layer_packets.begin(),
                     layer_packets.end(),
                     runtime_gpu_renderer_detail::draw_packet_sort_predicate);

    CHECK(layer_packets.size() == 2);
    CHECK(layer_packets[0].depth_metric == doctest::Approx(10.0f));
    CHECK(layer_packets[1].depth_metric == doctest::Approx(20.0f));
}

TEST_CASE("GPU runtime non-floor packet stays in layer pass even with floor boxes enabled") {
    std::vector<GpuSpriteDrawPacket> floor_draws{};
    std::vector<GpuSpriteDrawPacket> layer_draws{};

    GpuSpriteDrawPacket packet{};
    packet.is_floor_packet = false;
    packet.depth_layer = -2;
    runtime_gpu_renderer_detail::append_classified_sprite_draw_packet(false,
                                                                      packet,
                                                                      floor_draws,
                                                                      layer_draws);

    CHECK(floor_draws.empty());
    REQUIRE(layer_draws.size() == 1);
    CHECK_FALSE(layer_draws[0].is_floor_packet);
    CHECK(layer_draws[0].depth_layer == -2);
}

TEST_CASE("GPU runtime fallback visibility path keeps ordering contract equivalent to primary path") {
    GpuSpriteDrawPacket a{};
    a.sort_key = 10.0f;
    a.depth_metric = 4.0f;
    a.stable_sort_id = 3u;
    a.depth_layer = 1;
    GpuSpriteDrawPacket b{};
    b.sort_key = 10.0f;
    b.depth_metric = 2.0f;
    b.stable_sort_id = 9u;
    b.depth_layer = 1;
    GpuSpriteDrawPacket c{};
    c.sort_key = 8.0f;
    c.depth_metric = 1.0f;
    c.stable_sort_id = 1u;
    c.depth_layer = 2;
    std::vector<GpuSpriteDrawPacket> primary{a, b, c};
    std::vector<GpuSpriteDrawPacket> fallback = primary;

    std::stable_sort(primary.begin(), primary.end(), runtime_gpu_renderer_detail::draw_packet_sort_predicate);
    std::stable_sort(fallback.begin(), fallback.end(), runtime_gpu_renderer_detail::draw_packet_sort_predicate);

    REQUIRE(primary.size() == fallback.size());
    for (std::size_t i = 0; i < primary.size(); ++i) {
        CHECK(primary[i].sort_key == doctest::Approx(fallback[i].sort_key));
        CHECK(primary[i].depth_metric == doctest::Approx(fallback[i].depth_metric));
        CHECK(primary[i].stable_sort_id == fallback[i].stable_sort_id);
    }
}

TEST_CASE("GPU runtime dev empty filtered list falls back to active assets") {
    Asset* active_asset = reinterpret_cast<Asset*>(static_cast<std::uintptr_t>(0x1u));
    Asset* filtered_asset = reinterpret_cast<Asset*>(static_cast<std::uintptr_t>(0x2u));
    std::vector<Asset*> active_assets{active_asset};
    std::vector<Asset*> filtered_assets{};
    bool used_fallback = false;

    const std::vector<Asset*>& selected_empty_filter =
        runtime_gpu_renderer_detail::select_visible_assets_for_gpu_frame(true,
                                                                         true,
                                                                         active_assets,
                                                                         filtered_assets,
                                                                         used_fallback);
    CHECK(&selected_empty_filter == &active_assets);
    CHECK(used_fallback);

    filtered_assets.push_back(filtered_asset);
    used_fallback = false;
    const std::vector<Asset*>& selected_filtered =
        runtime_gpu_renderer_detail::select_visible_assets_for_gpu_frame(true,
                                                                         true,
                                                                         active_assets,
                                                                         filtered_assets,
                                                                         used_fallback);
    CHECK(&selected_filtered == &filtered_assets);
    CHECK_FALSE(used_fallback);

    used_fallback = true;
    const std::vector<Asset*>& selected_stale_filter =
        runtime_gpu_renderer_detail::select_visible_assets_for_gpu_frame(true,
                                                                         false,
                                                                         active_assets,
                                                                         filtered_assets,
                                                                         used_fallback);
    CHECK(&selected_stale_filter == &active_assets);
    CHECK_FALSE(used_fallback);

    used_fallback = true;
    const std::vector<Asset*>& selected_runtime =
        runtime_gpu_renderer_detail::select_visible_assets_for_gpu_frame(false,
                                                                         false,
                                                                         active_assets,
                                                                         filtered_assets,
                                                                         used_fallback);
    CHECK(&selected_runtime == &active_assets);
    CHECK_FALSE(used_fallback);
}

TEST_CASE("GPU runtime floor packet helper emits packets for chunk tiles") {
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
                                                  SDL_PIXELFORMAT_RGBA32,
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
    REQUIRE(runtime_gpu_renderer_detail::build_floor_tile_draw_packets(
        grid,
        chunks,
        128u,
        128u,
        packets));
    REQUIRE(packets.size() == 1);
    CHECK(packets[0].is_floor_packet);
    CHECK(packets[0].depth_layer == 0);

    chunk.releaseTileTextures();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
}

TEST_CASE("GPU runtime render targets recreate on resize and prune inactive layers") {
    ScopedSdlVideo sdl_video{};
    REQUIRE(sdl_video.initialized());

    SDL_Window* window = SDL_CreateWindow("gpu_runtime_resize_targets", 128, 128, SDL_WINDOW_HIDDEN);
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

    const RuntimeGpuFormatPolicy& format_policy = gpu_renderer->device()->format_policy();
    GpuSceneRenderer::TextureResourceSpec spec{};
    spec.width = 128;
    spec.height = 128;
    spec.format = format_policy.albedo_format;
    spec.usage = static_cast<SDL_GPUTextureUsageFlags>(SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER);
    spec.layer_count_or_depth = 1;
    spec.num_levels = 1;
    spec.sample_count = SDL_GPU_SAMPLECOUNT_1;

    render_diagnostics::begin_frame();
    REQUIRE(gpu_renderer->ensure_texture_resource("runtime.scene.floor", spec, error));
    REQUIRE(gpu_renderer->find_texture_resource("runtime.scene.floor") != nullptr);
    const std::uint64_t created_before_resize = render_diagnostics::current_frame_stats().texture_create_count;
    spec.width = 64;
    spec.height = 64;
    REQUIRE(gpu_renderer->ensure_texture_resource("runtime.scene.floor", spec, error));
    REQUIRE(gpu_renderer->find_texture_resource("runtime.scene.floor") != nullptr);
    const RenderFrameStats resize_stats = render_diagnostics::current_frame_stats();
    CHECK(resize_stats.texture_create_count >= created_before_resize);
    CHECK(resize_stats.texture_destroy_count >= 1u);

    REQUIRE(gpu_renderer->ensure_texture_resource("runtime.scene.layer.-1", spec, error));
    REQUIRE(gpu_renderer->ensure_texture_resource("runtime.scene.layer.0", spec, error));
    CHECK(gpu_renderer->find_texture_resource("runtime.scene.layer.-1") !=
          gpu_renderer->find_texture_resource("runtime.scene.layer.0"));
    std::unordered_set<std::string> retained{"runtime.scene.layer.0"};
    gpu_renderer->release_texture_resources_with_prefix("runtime.scene.layer.", retained);
    CHECK(gpu_renderer->find_texture_resource("runtime.scene.layer.-1") == nullptr);
    CHECK(gpu_renderer->find_texture_resource("runtime.scene.layer.0") != nullptr);
    render_diagnostics::end_frame();

    gpu_renderer.reset();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
}


TEST_CASE("CacheManager unregisters prepared GPU uploads before texture destruction") {
    ScopedSdlVideo sdl_video{};
    REQUIRE(sdl_video.initialized());

    SDL_Window* window = SDL_CreateWindow("cache_manager_unregister_prepared_upload_window", 16, 16, SDL_WINDOW_HIDDEN);
    REQUIRE(window != nullptr);
    SDL_Renderer* renderer = SDL_CreateRenderer(window, SDL_SOFTWARE_RENDERER);
    if (!renderer) {
        SDL_DestroyWindow(window);
        return;
    }

    SDL_Surface* surface = SDL_CreateSurface(4, 4, SDL_PIXELFORMAT_RGBA32);
    REQUIRE(surface != nullptr);
    SDL_FillSurfaceRect(surface, nullptr, 0xFF806040u);

    SDL_Texture* texture = CacheManager::surface_to_texture(renderer, surface);
    SDL_DestroySurface(surface);
    REQUIRE(texture != nullptr);
    REQUIRE(CacheManager::prepared_gpu_upload_for_texture(texture) != nullptr);

    CacheManager::unregister_prepared_gpu_upload(texture);
    SDL_DestroyTexture(texture);
    CHECK(CacheManager::prepared_gpu_upload_for_texture(texture) == nullptr);

    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
}

TEST_CASE("GPU runtime texture resolve uploads prepared loading-step textures without SDL bridge") {
    ScopedSdlVideo sdl_video{};
    REQUIRE(sdl_video.initialized());

    SDL_Window* gpu_window = SDL_CreateWindow("gpu_runtime_resolve_prepared_gpu_window", 64, 64, SDL_WINDOW_HIDDEN);
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

    SDL_Window* software_window = SDL_CreateWindow("gpu_runtime_resolve_prepared_software_window", 64, 64, SDL_WINDOW_HIDDEN);
    REQUIRE(software_window != nullptr);
    SDL_Renderer* software_renderer = SDL_CreateRenderer(software_window, SDL_SOFTWARE_RENDERER);
    if (!software_renderer) {
        gpu_renderer.reset();
        SDL_DestroyRenderer(gpu_renderer_backend);
        SDL_DestroyWindow(gpu_window);
        SDL_DestroyWindow(software_window);
        return;
    }

    SDL_Surface* surface = SDL_CreateSurface(4, 4, SDL_PIXELFORMAT_RGBA32);
    REQUIRE(surface != nullptr);
    SDL_FillSurfaceRect(surface, nullptr, 0xFF4020FFu);
    SDL_Texture* prepared_texture = CacheManager::surface_to_texture(software_renderer, surface);
    SDL_DestroySurface(surface);
    REQUIRE(prepared_texture != nullptr);

    SDL_GPUTexture* resolved = gpu_renderer->resolve_gpu_texture_for_sdl_texture(prepared_texture, error);
    CHECK(resolved != nullptr);
    CHECK(error.empty());
    CHECK(gpu_renderer->find_gpu_texture_for_sdl_texture(prepared_texture) == resolved);

    CacheManager::unregister_prepared_gpu_upload(prepared_texture);
    SDL_DestroyTexture(prepared_texture);
    gpu_renderer.reset();
    SDL_DestroyRenderer(software_renderer);
    SDL_DestroyWindow(software_window);
    SDL_DestroyRenderer(gpu_renderer_backend);
    SDL_DestroyWindow(gpu_window);
}

TEST_CASE("GPU runtime resolves animation cloner derived frame textures from prepared payloads") {
    ScopedSdlVideo sdl_video{};
    REQUIRE(sdl_video.initialized());

    SDL_Window* gpu_window = SDL_CreateWindow("gpu_runtime_resolve_cloned_animation_gpu_window", 64, 64, SDL_WINDOW_HIDDEN);
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

    SDL_Window* loading_window = SDL_CreateWindow("gpu_runtime_resolve_cloned_animation_loading_window", 64, 64, SDL_WINDOW_HIDDEN);
    REQUIRE(loading_window != nullptr);
    SDL_Renderer* loading_renderer = SDL_CreateRenderer(loading_window, SDL_SOFTWARE_RENDERER);
    if (!loading_renderer) {
        gpu_renderer.reset();
        SDL_DestroyRenderer(gpu_renderer_backend);
        SDL_DestroyWindow(gpu_window);
        SDL_DestroyWindow(loading_window);
        return;
    }

    SDL_Surface* surface = SDL_CreateSurface(4, 2, SDL_PIXELFORMAT_RGBA32);
    REQUIRE(surface != nullptr);
    SDL_FillSurfaceRect(surface, nullptr, 0xFF2040FFu);
    SDL_Texture* source_texture = CacheManager::surface_to_texture(loading_renderer, surface);
    SDL_DestroySurface(surface);
    REQUIRE(source_texture != nullptr);
    REQUIRE(CacheManager::prepared_gpu_upload_for_texture(source_texture) != nullptr);

    Animation::FrameCache source_cache;
    source_cache.resize(1);
    source_cache.textures[0] = source_texture;
    source_cache.widths[0] = 4;
    source_cache.heights[0] = 2;
    source_cache.source_rects[0] = SDL_Rect{0, 0, 4, 2};

    Animation source_animation;
    source_animation.adopt_prebuilt_frames(std::vector<Animation::FrameCache>{std::move(source_cache)}, {1.0f});

    AssetInfo info("gpu_runtime_cloned_animation_asset");
    AnimationCloner::Options options{};
    options.flip_horizontal = true;

    Animation cloned_animation;
    REQUIRE(AnimationCloner::Clone(source_animation, cloned_animation, options, loading_renderer, info));
    REQUIRE(cloned_animation.cached_frame_count() == 1);
    REQUIRE_FALSE(cloned_animation.cached_frames()[0].textures.empty());
    SDL_Texture* cloned_texture = cloned_animation.cached_frames()[0].textures[0];
    REQUIRE(cloned_texture != nullptr);
    REQUIRE(cloned_texture != source_texture);
    REQUIRE(CacheManager::prepared_gpu_upload_for_texture(cloned_texture) != nullptr);

    SDL_GPUTexture* resolved = gpu_renderer->resolve_gpu_texture_for_sdl_texture(cloned_texture, error);
    CHECK(resolved != nullptr);
    CHECK(error.empty());
    CHECK(gpu_renderer->find_gpu_texture_for_sdl_texture(cloned_texture) == resolved);

    cloned_animation.clear_texture_cache();
    source_animation.clear_texture_cache();
    gpu_renderer.reset();
    SDL_DestroyRenderer(loading_renderer);
    SDL_DestroyWindow(loading_window);
    SDL_DestroyRenderer(gpu_renderer_backend);
    SDL_DestroyWindow(gpu_window);
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
                                                      SDL_PIXELFORMAT_RGBA32,
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
