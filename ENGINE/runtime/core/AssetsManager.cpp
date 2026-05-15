#include "AssetsManager.hpp"
#include "utils/sdl_render_conversions.hpp"

#include "utils/ranged_color.hpp"
#include "assets/asset/initialize_assets.hpp"

#include "find_current_room.hpp"
#include "assets/asset/Asset.hpp"
#include "assets/asset/asset_info.hpp"
#include "assets/asset/asset_utils.hpp"
#include "assets/asset/asset_types.hpp"
#include "animation/animation_update.hpp"
#include "animation/animation_runtime.hpp"
#include "animation/unstick_utils.hpp"
#include "animation/controllers/shared/anchor_bound_asset_helper.hpp"
#include "audio/audio_engine.hpp"
#include "devtools/dev_controls.hpp"
#include "devtools/dev_camera_controls.hpp"
#include "devtools/dm_styles.hpp"
#include "devtools/core/manifest_store.hpp"
#include "devtools/depth_cue_settings.hpp"
#include "devtools/font_cache.hpp"
#include "core/manifest/map_data.hpp"
#include "core/tile_builder.hpp"
#include "core/dev_mode_animation_policy.hpp"
#include "rendering/render/opengl_runtime_renderer.hpp"
#include "rendering/render/render_diagnostics.hpp"
#include "rendering/render/render_depth_policy.hpp"
#include "gameplay/world/chunk.hpp"
#include "gameplay/map_generation/room.hpp"
#include "gameplay/map_generation/map_layers_geometry.hpp"
#include "utils/integer_grid_math.hpp"
#include "utils/grid.hpp"
#include "utils/area.hpp"
#include "utils/input.hpp"
#include "utils/range_util.hpp"
#include "utils/map_grid_settings.hpp"
#include "utils/log.hpp"
#include "utils/task_editor.hpp"

#include <filesystem>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>
#include <cstdint>
#include <cctype>
#include <exception>
#include <stdexcept>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <utility>
#include <execution>
#include <unordered_set>
#include <nlohmann/json.hpp>
#include <functional>
#include <thread>
#include <vector>
#include <unordered_set>
#include <unordered_map>
#include <array>
#include <sstream>
#include <SDL3/SDL.h>
#include <SDL3_image/SDL_image.h>
#include <SDL3_ttf/SDL_ttf.h>

namespace {

constexpr std::size_t kNonPlayerParallelThreshold = 4;
constexpr Uint32 kCreateTaskButtonLifetimeMs = 10000;
constexpr Uint32 kCreateTaskButtonFadeMs = 1500;
constexpr float kDefaultBoundaryMinVisibleScreenRatio = 0.015f;
constexpr int kDefaultCameraHeightMinPx = 1;
constexpr int kDefaultCameraHeightMaxPx = 100000;
constexpr std::size_t kRuntimeAnchorConvergenceIterationCap = 8;
constexpr int kCollisionIndexSpacingMinPx = 64;
constexpr int kCollisionIndexSpacingMaxPx = 256;
constexpr int kAggressiveMaxCollisionSearchRadiusPx = 320;

int env_int_clamped(const char* name, int fallback, int min_value, int max_value) {
    const int safe_min = std::min(min_value, max_value);
    const int safe_max = std::max(min_value, max_value);
    const int safe_fallback = std::clamp(fallback, safe_min, safe_max);
    const char* raw = SDL_getenv(name);
    if (!raw || !*raw) {
        return safe_fallback;
    }
    try {
        const int parsed = std::stoi(raw);
        return std::clamp(parsed, safe_min, safe_max);
    } catch (...) {
        return safe_fallback;
    }
}

double env_double_clamped(const char* name, double fallback, double min_value, double max_value) {
    const double safe_min = std::min(min_value, max_value);
    const double safe_max = std::max(min_value, max_value);
    double safe_fallback = fallback;
    if (!std::isfinite(safe_fallback)) {
        safe_fallback = safe_min;
    }
    safe_fallback = std::clamp(safe_fallback, safe_min, safe_max);
    const char* raw = SDL_getenv(name);
    if (!raw || !*raw) {
        return safe_fallback;
    }
    try {
        const double parsed = std::stod(raw);
        if (!std::isfinite(parsed)) {
            return safe_fallback;
        }
        return std::clamp(parsed, safe_min, safe_max);
    } catch (...) {
        return safe_fallback;
    }
}

bool startup_runtime_safety_enabled() {
    return false;
}

std::uint32_t startup_runtime_safety_frames() {
    static const std::uint32_t frames = static_cast<std::uint32_t>(
        env_int_clamped("VIBBLE_STARTUP_RUNTIME_SAFETY_FRAMES", 360, 1, 5000));
    return frames;
}

bool startup_runtime_safety_active(std::uint32_t frame_id) {
    return startup_runtime_safety_enabled() && frame_id <= startup_runtime_safety_frames();
}

std::uint32_t startup_skip_runtime_effects_frames() {
    static const std::uint32_t frames = static_cast<std::uint32_t>(
        env_int_clamped("VIBBLE_STARTUP_SKIP_RUNTIME_EFFECTS_FRAMES", 8, 0, 1000));
    return frames;
}

std::uint32_t startup_skip_render_frames() {
    static const std::uint32_t frames = static_cast<std::uint32_t>(
        env_int_clamped("VIBBLE_STARTUP_SKIP_RENDER_FRAMES", 0, 0, 1000));
    return frames;
}

std::uint32_t startup_render_every_n_frames() {
    static const std::uint32_t n = static_cast<std::uint32_t>(
        env_int_clamped("VIBBLE_STARTUP_RENDER_EVERY_N_FRAMES", 1, 1, 16));
    return n;
}

std::size_t startup_non_player_update_batch_size() {
    static const std::size_t batch = static_cast<std::size_t>(
        env_int_clamped("VIBBLE_STARTUP_NON_PLAYER_UPDATE_BATCH", 96, 8, 50000));
    return batch;
}

std::size_t startup_runtime_pass_asset_batch_size() {
    static const std::size_t batch = static_cast<std::size_t>(
        env_int_clamped("VIBBLE_STARTUP_RUNTIME_PASS_ASSET_BATCH", 128, 8, 50000));
    return batch;
}

std::uint32_t startup_skip_trap_escape_frames() {
    static const std::uint32_t frames = static_cast<std::uint32_t>(
        env_int_clamped("VIBBLE_STARTUP_SKIP_TRAP_ESCAPE_FRAMES", 240, 0, 5000));
    return frames;
}

std::uint32_t startup_visibility_refresh_interval_frames() {
    static const std::uint32_t frames = static_cast<std::uint32_t>(
        env_int_clamped("VIBBLE_STARTUP_VISIBILITY_INTERVAL_FRAMES", 2, 1, 64));
    return frames;
}

double startup_stage_warning_ms() {
    return env_double_clamped("VIBBLE_STARTUP_STAGE_WARN_MS", 250.0, 10.0, 5000.0);
}

double dynamic_asset_camera_depth_from_focus_plane(const WarpedScreenGrid& camera, const Asset& asset) {
    const auto projection = camera.projection_params();
    const double focus_world_z = camera.current_focus_plane_world_z();
    const double effective_world_z =
        static_cast<double>(asset.world_z()) +
        static_cast<double>(asset.world_z_offset()) +
        static_cast<double>(asset.render_anchor_offset_z());
    const double depth_axis_sign = static_cast<double>(render_depth::normalize_depth_axis_sign(
        static_cast<float>(projection.forward_z)));
    return (effective_world_z - focus_world_z) * depth_axis_sign;
}

std::size_t runtime_convergence_iteration_cap_for_frame(std::uint32_t frame_id) {
    const std::size_t base_cap = static_cast<std::size_t>(
        env_int_clamped("VIBBLE_RUNTIME_CONVERGENCE_ITERATION_CAP",
                        static_cast<int>(kRuntimeAnchorConvergenceIterationCap),
                        1,
                        64));
    if (!startup_runtime_safety_active(frame_id)) {
        return base_cap;
    }
    const std::size_t startup_cap = static_cast<std::size_t>(
        env_int_clamped("VIBBLE_STARTUP_CONVERGENCE_ITERATION_CAP", 1, 1, 64));
    return std::min(base_cap, startup_cap);
}

double runtime_convergence_stage_budget_ms_for_frame(std::uint32_t frame_id) {
    if (!startup_runtime_safety_active(frame_id)) {
        return env_double_clamped("VIBBLE_RUNTIME_CONVERGENCE_BUDGET_MS", 0.0, 0.0, 2000.0);
    }
    return env_double_clamped("VIBBLE_STARTUP_CONVERGENCE_BUDGET_MS", 12.0, 0.0, 2000.0);
}

bool allow_traversal_refresh_for_frame(std::uint32_t frame_id) {
    if (!startup_runtime_safety_active(frame_id)) {
        return true;
    }
    const std::uint32_t interval = static_cast<std::uint32_t>(
        env_int_clamped("VIBBLE_STARTUP_TRAVERSAL_REFRESH_INTERVAL_FRAMES", 8, 1, 240));
    return interval <= 1 || (frame_id % interval) == 0;
}

float normalize_depth_axis_sign(float sign) {
    return render_depth::normalize_depth_axis_sign(sign);
}

float depth_axis_sign_from_forward_z(float forward_z) {
    return normalize_depth_axis_sign(forward_z);
}

float depth_offset_from_world_z(float world_z, float anchor_world_z, float depth_axis_sign) {
    if (!std::isfinite(world_z) || !std::isfinite(anchor_world_z)) {
        return 0.0f;
    }
    const float sign = render_depth::normalize_depth_axis_sign(depth_axis_sign);
    return (world_z - anchor_world_z) * sign;
}

bool runtime_impassable_shape_contributes(const Asset::RuntimeImpassableShape& shape) {
    return shape.enabled && shape.valid && shape.floor_points.size() >= 3;
}

bool build_impassable_area_for_shape(const Asset& asset,
                                     const Asset::RuntimeImpassableShape& shape,
                                     Area& out_area) {
    if (!runtime_impassable_shape_contributes(shape)) {
        return false;
    }

    std::vector<Area::Point> points;
    points.reserve(shape.floor_points.size());
    for (const auto& floor_point : shape.floor_points) {
        if (!std::isfinite(floor_point.x) || !std::isfinite(floor_point.y)) {
            continue;
        }
        points.push_back(Area::Point{
            static_cast<int>(std::lround(floor_point.x)),
            static_cast<int>(std::lround(floor_point.y))});
    }

    if (points.size() < 3) {
        return false;
    }

    out_area = Area("impassable", points, asset.grid_resolution);
    return true;
}

int runtime_collision_index_cell_spacing_world(const MapGridSettings& settings) {
    return std::clamp(settings.spacing(), kCollisionIndexSpacingMinPx, kCollisionIndexSpacingMaxPx);
}

struct AssetWorldBounds {
    float left = 0.0f;
    float right = 0.0f;
    float top = 0.0f;
    float bottom = 0.0f;
};

bool compute_asset_world_bounds(const Asset* asset,
                                float camera_scale,
                                AssetWorldBounds& bounds) {
    if (!asset || !asset->info) {
        return false;
    }

    if (const auto& tiling = asset->tiling_info(); tiling && tiling->is_valid()) {
        bounds.left   = static_cast<float>(tiling->coverage.x);
        bounds.top    = static_cast<float>(tiling->coverage.y);
        bounds.right  = bounds.left + static_cast<float>(tiling->coverage.w);
        bounds.bottom = bounds.top + static_cast<float>(tiling->coverage.h);
        return true;
    }

    const int base_w = std::max(1, asset->info->original_canvas_width);
    const int base_h = std::max(1, asset->info->original_canvas_height);
    float scale_factor = 1.0f;
    if (std::isfinite(asset->info->scale_factor) && asset->info->scale_factor > 0.0f) {
        scale_factor = asset->info->scale_factor;
    }

    const float width  = static_cast<float>(base_w) * scale_factor * camera_scale;
    const float height = static_cast<float>(base_h) * scale_factor * camera_scale;
    const float half_w = width * 0.5f;
    const float bottom = static_cast<float>(asset->world_y());

    bounds.left   = static_cast<float>(asset->world_x()) - half_w;
    bounds.right  = static_cast<float>(asset->world_x()) + half_w;
    bounds.bottom = bottom;
    bounds.top    = bottom - height;
    return true;
}

bool vibble_scale_trace_enabled() {
    static const bool enabled = [] {
        const char* raw = SDL_getenv("VIBBLE_SCALE_TRACE");
        if (!raw || !*raw) {
            return false;
        }
        const std::string value(raw);
        return value == "1" ||
               value == "true" ||
               value == "TRUE" ||
               value == "on" ||
               value == "ON";
    }();
    return enabled;
}

bool vibble_runtime_convergence_trace_enabled() {
    static const bool enabled = [] {
        const char* raw = SDL_getenv("VIBBLE_RUNTIME_CONVERGENCE_TRACE");
        if (!raw || !*raw) {
            return false;
        }
        const std::string value(raw);
        return value == "1" ||
               value == "true" ||
               value == "TRUE" ||
               value == "on" ||
               value == "ON";
    }();
    return enabled;
}

std::uint64_t mix_u64(std::uint64_t seed, std::uint64_t value) {
    seed ^= value + 0x9e3779b97f4a7c15ULL + (seed << 6) + (seed >> 2);
    return seed;
}

double live_dynamic_unit_interval(std::uint64_t value) {
    value ^= value >> 33;
    value *= 0xff51afd7ed558ccdULL;
    value ^= value >> 33;
    value *= 0xc4ceb9fe1a85ec53ULL;
    value ^= value >> 33;
    return static_cast<double>(value) / static_cast<double>(std::numeric_limits<std::uint64_t>::max());
}

double live_dynamic_candidate_weight(const nlohmann::json& candidate) {
    if (candidate.is_object()) {
        for (const char* key : {"chance", "weight", "probability"}) {
            auto it = candidate.find(key);
            if (it != candidate.end() && it->is_number()) {
                const double value = it->get<double>();
                return std::isfinite(value) && value > 0.0 ? value : 0.0;
            }
        }
        return 0.0;
    }
    if (candidate.is_string() || candidate.is_null()) {
        return 100.0;
    }
    if (candidate.is_number()) {
        const double value = candidate.get<double>();
        return std::isfinite(value) && value > 0.0 ? value : 0.0;
    }
    return 0.0;
}

std::string live_dynamic_candidate_name(const nlohmann::json& candidate, bool& is_tag, bool& is_null) {
    is_tag = false;
    is_null = candidate.is_null();
    std::string name;
    if (candidate.is_object()) {
        if (candidate.contains("name") && candidate["name"].is_string()) {
            name = candidate["name"].get<std::string>();
        } else if (candidate.contains("asset_name") && candidate["asset_name"].is_string()) {
            name = candidate["asset_name"].get<std::string>();
        }
        if (candidate.contains("tag")) {
            if (candidate["tag"].is_string()) {
                is_tag = true;
                name = candidate["tag"].get<std::string>();
            } else if (candidate["tag"].is_boolean() && candidate["tag"].get<bool>()) {
                is_tag = true;
            }
        }
        if (candidate.contains("tag_name") && candidate["tag_name"].is_string()) {
            is_tag = true;
            name = candidate["tag_name"].get<std::string>();
        }
    } else if (candidate.is_string()) {
        name = candidate.get<std::string>();
    }

    name.erase(name.begin(), std::find_if(name.begin(), name.end(), [](unsigned char ch) {
        return std::isspace(ch) == 0;
    }));
    name.erase(std::find_if(name.rbegin(), name.rend(), [](unsigned char ch) {
        return std::isspace(ch) == 0;
    }).base(), name.end());
    if (!name.empty() && name.front() == '#') {
        is_tag = true;
        name.erase(name.begin());
    }

    std::string lowered = name;
    std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    if (lowered == "null") {
        is_null = true;
    }
    return name;
}

bool live_dynamic_is_boundary_info(const AssetInfo* info) {
    if (!info) {
        return false;
    }
    // Boundary classification for live-dynamic spawning is tag-driven only.
    // Asset type is intentionally ignored.
    return info->has_tag("boundary");
}

bool live_dynamic_label_is_trail(const std::string& value) {
    if (value.empty()) {
        return false;
    }
    std::string lowered;
    lowered.reserve(value.size());
    for (unsigned char ch : value) {
        lowered.push_back(static_cast<char>(std::tolower(ch)));
    }
    return lowered == "trail";
}

bool live_dynamic_named_area_is_trail(const Room::NamedArea& named_area) {
    return named_area.area &&
           (live_dynamic_label_is_trail(named_area.type) ||
            live_dynamic_label_is_trail(named_area.kind) ||
            live_dynamic_label_is_trail(named_area.name));
}

bool live_dynamic_info_allowed(const AssetInfo* info, Assets::LiveDynamicMode mode) {
    const bool boundary = live_dynamic_is_boundary_info(info);
    if (mode == Assets::LiveDynamicMode::BoundaryArea) {
        return boundary;
    }
    return !boundary;
}

std::uint64_t hash_grid_cell(const world::GridCoord& coord) {
    const std::uint64_t ux = static_cast<std::uint32_t>(coord.x);
    const std::uint64_t uz = static_cast<std::uint32_t>(coord.z);
    return (ux << 32) | uz;
}

}

float Assets::frame_delta_seconds_clamped() const {
    constexpr float kFallbackDt = 1.0f / 60.0f;
    const float dt = last_frame_dt_seconds_;
    if (std::isfinite(dt) && dt > 0.0f) {
        return std::min(dt, 0.1f);
    }
    return kFallbackDt;
}

Assets::Assets(AssetLibrary& library,
               Asset*,
               std::shared_ptr<RuntimeWorldContext> world_context,
               int screen_width_,
               int screen_height_,
               int screen_center_x,
               int screen_center_z,
               int map_radius,
               SDL_Renderer* renderer,
               SDL_Window* window,
               const std::string& map_id,
               const nlohmann::json& map_manifest,
               std::string content_root,
               world::WorldGrid&& world_grid)
    : camera_(
          screen_width_,
          screen_height_,
          Area(
              "starting_camera",
              std::vector<SDL_Point>{

                  SDL_Point{-100,-100},
                  SDL_Point{ 100,-100},
                  SDL_Point{ 100,100},
                  SDL_Point{-100, 100}
              },
              0)
      ),
      screen_width(screen_width_),
      screen_height(screen_height_),
      world_grid_(std::move(world_grid)),
      library_(library),
      world_context_(std::move(world_context)),
      app_window_(window),
      map_id_(map_id),
      map_path_(std::move(content_root)),
      map_radius_world_(std::max(0, map_radius))
{
    if (!world_context_) {
        world_context_ = std::make_shared<RuntimeWorldContext>();
    }
    perf_counter_frequency_ = static_cast<double>(SDL_GetPerformanceFrequency());
    last_frame_counter_     = SDL_GetPerformanceCounter();
    map_info_json_ = map_manifest;
    if (!map_info_json_.is_object()) {
        map_info_json_ = nlohmann::json::object();
    }

    hydrate_map_info_sections();
    load_runtime_game_config_from_json();

    vibble::log::info("[Assets] Constructor: Starting InitializeAssets initialization");
    InitializeAssets::initialize(*this, screen_width_, screen_height_, screen_center_x, screen_center_z, map_radius);
    vibble::log::info("[Assets] Constructor: InitializeAssets complete");

    auto& room_refs = rooms();
    finder_ = new CurrentRoomFinder(room_refs, player);
    game_context_.rebuild_runtime_map_graph(room_refs);
    if (finder_) {
        finder_->setCamera(&camera_);
        camera_.set_up_rooms(finder_);
    }

    auto current_room = [&]() -> Room* {
        if (finder_) {
            return finder_->getCurrentRoom();
        }
        return nullptr;
};
    Room* intro_room = current_room();
    current_room_ = intro_room;
    game_context_.begin_frame(
        this,
        frame_id_,
        frame_delta_seconds_clamped(),
        current_room_,
        player,
        &camera_,
        &runtime_game_config());

    SDL_Point intro_center{screen_center_x, screen_center_z};
    if (player) {
        intro_center = SDL_Point{player->world_x(), player->world_z()};
    } else if (Room* room = intro_room) {
        if (room->room_area) {
            const SDL_Point room_center = room->room_area->get_center();
            intro_center = SDL_Point{
                room_center.x + room->camera_center_dx,
                room_center.y + room->camera_center_dz
            };
        }
    }
    camera_.set_screen_center(intro_center);
    SDL_Point center_px = camera_.get_screen_center();
    last_camera_center_for_grid_ = world::GridPoint::make_virtual(center_px.x, 0, center_px.y, 0);
    last_camera_scale_for_grid_ = camera_.get_scale();
    last_camera_pitch_for_grid_ = camera_.current_pitch_radians();
    last_camera_projection_state_version_for_grid_ = camera_.camera_state_version();
    if (player) {
        last_known_player_pos_ = world::GridPoint::make_virtual(player->world_x(), player->world_y(), player->world_z(), player->grid_resolution);
        last_player_pos_valid_ = true;
    } else {
        last_player_pos_valid_ = false;
    }



    renderer_ = renderer;
    if (!renderer_) {
        vibble::log::error("[Assets] OpenGL runtime renderer not created: SDL_Renderer pointer is null.");
    } else {
        vibble::log::info("[Assets] Constructor: Creating OpenGL runtime renderer");
        std::string runtime_error;
        opengl_renderer_ = OpenGLRuntimeRenderer::Create(renderer_, this, screen_width_, screen_height_, runtime_error);
        if (!opengl_renderer_) {
            throw std::runtime_error("[Assets] OpenGL runtime renderer initialization failed: " +
                                     (runtime_error.empty() ? std::string("unknown error") : runtime_error));
        }
        vibble::log::info("[Assets] Constructor: OpenGL runtime renderer created successfully");
    }
    apply_map_grid_settings(map_grid_settings_, false);

    pending_initial_rebuild_ = true;
    logged_initial_rebuild_warning_ = false;
    moving_assets_for_grid_.clear();
    moving_assets_for_grid_.reserve(all.size());
    pending_static_grid_registration_.clear();
    movement_commands_buffer_.clear();
    movement_commands_buffer_.reserve(all.size());
    grid_registration_buffer_.clear();
    grid_registration_buffer_.reserve(4);
    runtime_traversal_state_.clear();
    runtime_traversal_state_.reserve(all.size());
    vibble::log::info("[Assets] Constructor: Setting up assets (" + std::to_string(all.size()) + " total)");
    for (Asset* a : all) {
        if (!a) continue;
        a->set_assets(this);
    }
    vibble::log::info("[Assets] Constructor: Asset finalization complete");

    register_pending_static_assets();

    update_filtered_active_assets();

    const std::filesystem::path manifest_root =
        std::filesystem::absolute(std::filesystem::path(manifest::manifest_path()).parent_path());
    task_editor_ = std::make_unique<TaskEditor>(manifest_root);

    vibble::log::info("[Assets] Constructor: Initialization complete");
}

void Assets::set_output_dimensions(int width, int height) {
    const int safe_w = std::max(1, width);
    const int safe_h = std::max(1, height);
    if (screen_width == safe_w && screen_height == safe_h) {
        return;
    }

    screen_width = safe_w;
    screen_height = safe_h;

    camera_.set_screen_dimensions(screen_width, screen_height);
    if (opengl_renderer_) {
        opengl_renderer_->set_output_dimensions(screen_width, screen_height);
    }
    if (dev_controls_) {
        dev_controls_->set_screen_dimensions(screen_width, screen_height);
    }

    mark_grid_dirty();
    camera_view_dirty_ = true;
    needs_filtered_active_refresh_ = true;
}

std::optional<SDL_Point> Assets::opengl_postprocess_target_size() const {
    if (!opengl_renderer_) {
        return std::nullopt;
    }
    return opengl_renderer_->scene_target_size();
}

std::vector<const Room::NamedArea*> Assets::current_room_trigger_areas() const {
    std::vector<const Room::NamedArea*> result;
    if (!current_room_) {
        return result;
    }

    const auto is_trigger_string = [](const std::string& value) {
        if (value.empty()) {
            return false;
        }
        std::string lowered;
        lowered.reserve(value.size());
        for (unsigned char ch : value) {
            lowered.push_back(static_cast<char>(std::tolower(ch)));
        }
        if (lowered == "trigger") {
            return true;
        }
        return lowered.find("trigger") != std::string::npos;
};

    for (const auto& entry : current_room_->areas) {
        if (!entry.area) {
            continue;
        }
        if (is_trigger_string(entry.kind) ||
            is_trigger_string(entry.type) ||
            is_trigger_string(entry.name)) {
            result.push_back(&entry);
        }
    }

    return result;
}

void Assets::save_map_info_json() {
    write_camera_settings_to_json();
    write_runtime_game_config_to_json();
    if (map_id_.empty()) {
        std::cerr << "[Assets] Unable to persist map manifest entry: map ID is empty.\n";
        return;
    }
    devmode::core::ManifestStore* store = manifest_store();
    if (!store) {
        std::cerr << "[Assets] Unable to persist map manifest entry: manifest store unavailable.\n";
        return;
    }
    devmode::core::ManifestStore::MapPersistOptions options;
    options.flush = true;
    options.guard_reason = "Assets::save_map_info_json";
    if (!store->persist_map_entry(map_id_, map_info_json_, options)) {
        std::cerr << "[Assets] Failed to persist map manifest entry for " << map_id_ << "\n";
        return;
    }
}

bool Assets::mutate_map_data(const std::function<bool(manifest::MapData&)>& mutator) {
    if (!mutator) {
        return false;
    }
    try {
        manifest::MapData map_data = manifest::MapData::from_manifest_entry(map_id_, map_info_json_);
        if (!mutator(map_data)) {
            return false;
        }
        map_info_json_ = map_data.to_manifest_entry();
        mark_map_data_dirty();
        return true;
    } catch (const std::exception& ex) {
        std::cerr << "[Assets] mutate_map_data failed: " << ex.what() << "\n";
        return false;
    } catch (...) {
        std::cerr << "[Assets] mutate_map_data failed with unknown exception\n";
        return false;
    }
}

void Assets::mark_map_data_dirty() {
    map_data_dirty_ = true;
}

void Assets::snapshot_rooms_to_map_info() {
    if (!map_info_json_.is_object()) {
        map_info_json_ = nlohmann::json::object();
    }
    for (Room* room : rooms()) {
        if (!room) {
            continue;
        }
        room->snapshot_assets_to_map_info();
    }
}

void Assets::persist_map_info_json() {
    save_map_info_json();
    map_data_dirty_ = false;
}

void Assets::hydrate_map_info_sections() {
    if (!map_info_json_.is_object()) {
        return;
    }
    if (!map_info_json_.contains("schema_version") || !map_info_json_["schema_version"].is_number_integer()) {
        map_info_json_["schema_version"] = manifest::kMapSchemaVersion;
    }

    const auto ensure_object = [&](const char* key) {
        auto it = map_info_json_.find(key);
        if (it == map_info_json_.end()) {
            map_info_json_[key] = nlohmann::json::object();
            return;
        }
        if (!it->is_object()) {
            std::cerr << "[Assets] map_info." << key << " expected to be an object. Resetting." << "\n";
            *it = nlohmann::json::object();
        }
};

    migrate_live_dynamic_spawn_config();
    ensure_object("live_dynamic_spawns");
    ensure_object("rooms_data");
    ensure_object("trails_data");
    ensure_object("runtime_game_config");

    ensure_map_grid_settings(map_info_json_);
    map_grid_settings_ = MapGridSettings::from_json(&map_info_json_["map_grid_settings"]);
    rebuild_live_dynamic_selectors();
}

void Assets::migrate_live_dynamic_spawn_config() {
    if (!map_info_json_.is_object()) {
        return;
    }
    if (!map_info_json_.contains("live_dynamic_spawns") ||
        !map_info_json_["live_dynamic_spawns"].is_object()) {
        map_info_json_["live_dynamic_spawns"] = nlohmann::json::object();
    }
    nlohmann::json& live = map_info_json_["live_dynamic_spawns"];

    auto selector_empty = [](const nlohmann::json& owner, const char* key) {
        auto it = owner.find(key);
        return it == owner.end() || !it->is_array() || it->empty();
    };

    if (selector_empty(live, "boundary_area_selectors")) {
        auto legacy = map_info_json_.find("map_boundary_data");
        if (legacy != map_info_json_.end() && legacy->is_object()) {
            auto candidates = legacy->find("candidate_selectors");
            if (candidates != legacy->end() && candidates->is_array()) {
                live["boundary_area_selectors"] = *candidates;
            }
        }
    }
    if (map_info_json_.erase("map_boundary_data") > 0) {
        vibble::log::info("[LiveDynamicSpawn] Migrated map_boundary_data to live_dynamic_spawns.");
    }

    if (map_info_json_.erase("candidate_selectors") > 0) {
        vibble::log::info("[LiveDynamicSpawn] Dropped deprecated top-level candidate_selectors.");
    }
    // Canonical config uses one shared selector set:
    // live_dynamic_spawns.boundary_area_selectors.
    // inherited_map_selectors is deprecated and ignored at runtime.
    if (live.contains("inherited_map_selectors")) {
        live.erase("inherited_map_selectors");
        vibble::log::info("[LiveDynamicSpawn] Dropped deprecated live_dynamic_spawns.inherited_map_selectors.");
    }
}

void Assets::clear_live_dynamic_assets() {
    std::vector<Asset*> assets_to_delete;
    assets_to_delete.reserve(live_dynamic_asset_keys_.size());
    for (const auto& [asset, _] : live_dynamic_asset_keys_) {
        if (asset) {
            assets_to_delete.push_back(asset);
        }
    }
    (void)delete_live_dynamic_assets_now(assets_to_delete);
    live_dynamic_asset_keys_.clear();
    live_dynamic_spawned_keys_.clear();
    live_dynamic_null_keys_.clear();
    live_dynamic_selector_scan_state_.clear();
}

std::size_t Assets::delete_live_dynamic_assets_now(const std::vector<Asset*>& assets_to_delete) {
    if (assets_to_delete.empty()) {
        return 0;
    }

    std::vector<Asset*> unique_assets;
    unique_assets.reserve(assets_to_delete.size());
    std::unordered_set<Asset*> seen;
    seen.reserve(assets_to_delete.size());
    for (Asset* asset : assets_to_delete) {
        if (!asset || !asset->is_dynamic_spawned_asset()) {
            continue;
        }
        if (seen.insert(asset).second) {
            unique_assets.push_back(asset);
        }
    }
    if (unique_assets.empty()) {
        return 0;
    }

    for (Asset* asset : unique_assets) {
        if (asset) {
            asset->notify_pre_delete();
        }
    }

    for (Asset* asset : all) {
        if (!asset || seen.find(asset) != seen.end()) {
            continue;
        }
        for (Asset* removed : unique_assets) {
            asset->remove_child(removed);
        }
    }

    std::size_t deleted = 0;
    for (Asset* asset : unique_assets) {
        if (!asset) {
            continue;
        }

        auto key_it = live_dynamic_asset_keys_.find(asset);
        if (key_it != live_dynamic_asset_keys_.end()) {
            live_dynamic_spawned_keys_.erase(key_it->second);
            live_dynamic_asset_keys_.erase(key_it);
        }

        if (asset == focus_filter_asset_) {
            clear_focus_filter();
        }
        remove_asset_dimension_cache(asset);
        runtime_traversal_state_.erase(asset);
        asset->clear_grid_residency_cache();
        asset->dead = true;
        asset->active = false;
        (void)world_grid_.remove_asset(asset);
        ++deleted;
    }

    auto erase_dead = [&](std::vector<Asset*>& list) {
        list.erase(
            std::remove_if(list.begin(),
                           list.end(),
                           [&](Asset* asset) { return !asset || seen.find(asset) != seen.end(); }),
            list.end());
    };
    erase_dead(all);
    erase_dead(active_assets);
    erase_dead(filtered_active_assets);
    erase_dead(non_player_update_buffer_);
    filtered_active_asset_membership_.clear();
    for (Asset* asset : filtered_active_assets) {
        if (asset) {
            filtered_active_asset_membership_.insert(asset);
        }
    }

    if (deleted > 0) {
        mark_grid_dirty();
        mark_active_assets_dirty();
        mark_non_player_update_buffer_dirty();
        needs_filtered_active_refresh_ = true;
        mark_collision_context_dirty();
    }
    return deleted;
}

void Assets::rebuild_live_dynamic_selectors() {
    live_dynamic_boundary_selectors_.clear();
    live_dynamic_inherited_selectors_.clear();

    if (!map_info_json_.is_object()) {
        clear_live_dynamic_assets();
        return;
    }
    const auto live_it = map_info_json_.find("live_dynamic_spawns");
    if (live_it == map_info_json_.end() || !live_it->is_object()) {
        clear_live_dynamic_assets();
        return;
    }

    const auto catalog = library_.all();
    const auto append_selector = [&](const nlohmann::json& selector_json,
                                     LiveDynamicMode mode,
                                     std::vector<LiveDynamicSelector>& out) {
        if (!selector_json.is_object()) {
            return;
        }
        const nlohmann::json* candidates_json = nullptr;
        auto candidates_it = selector_json.find("candidates");
        if (candidates_it != selector_json.end() && candidates_it->is_array()) {
            candidates_json = &(*candidates_it);
        }
        if (!candidates_json || candidates_json->empty()) {
            return;
        }

        LiveDynamicSelector selector;
        selector.mode = mode;
        selector.spawn_id = selector_json.value("spawn_id", std::string{});
        if (selector.spawn_id.empty()) {
            selector.spawn_id = selector_json.value("name", std::string{});
        }
        if (selector.spawn_id.empty()) {
            selector.spawn_id = std::string("live_dynamic_") + std::to_string(out.size() + 1);
        }
        selector.display_name = selector_json.value("display_name", selector.spawn_id);
        const int raw_resolution = selector_json.contains("grid_resolution")
            ? selector_json.value("grid_resolution", map_grid_settings_.grid_resolution)
            : selector_json.value("resolution", map_grid_settings_.grid_resolution);
        selector.grid_resolution = vibble::grid::clamp_resolution(raw_resolution);
        const int selector_jitter = selector_json.value("jitter", map_grid_settings_.position_jitter_px);
        selector.jitter_px = std::clamp(selector_jitter, 0, 2048);

        for (const nlohmann::json& candidate_json : *candidates_json) {
            const double weight = live_dynamic_candidate_weight(candidate_json);
            if (weight <= 0.0) {
                continue;
            }
            bool is_tag = false;
            bool is_null = false;
            std::string name = live_dynamic_candidate_name(candidate_json, is_tag, is_null);
            if (is_null || name.empty()) {
                selector.candidates.push_back(LiveDynamicCandidate{{}, nullptr, weight, true});
                continue;
            }

            if (is_tag) {
                for (const auto& [asset_name, info] : catalog) {
                    if (!info || !info->has_tag(name) || !live_dynamic_info_allowed(info.get(), mode)) {
                        continue;
                    }
                    selector.candidates.push_back(LiveDynamicCandidate{asset_name, info, weight, false});
                }
                continue;
            }

            std::shared_ptr<AssetInfo> info = library_.get(name);
            if (!info || !live_dynamic_info_allowed(info.get(), mode)) {
                continue;
            }
            selector.candidates.push_back(LiveDynamicCandidate{name, info, weight, false});
        }

        if (!selector.candidates.empty()) {
            out.push_back(std::move(selector));
        }
    };

    auto append_section = [&](const char* key, LiveDynamicMode mode, std::vector<LiveDynamicSelector>& out) {
        auto section_it = live_it->find(key);
        if (section_it == live_it->end() || !section_it->is_array()) {
            return;
        }
        for (const nlohmann::json& selector : *section_it) {
            append_selector(selector, mode, out);
        }
    };

    // Shared dataset, mode-specific filtering:
    // - boundary pass resolves candidates allowed for boundary mode
    // - inherited pass resolves candidates allowed for room/trail inherited mode
    // Both passes read the same authored boundary_area_selectors JSON.
    append_section("boundary_area_selectors", LiveDynamicMode::BoundaryArea, live_dynamic_boundary_selectors_);
    append_section("boundary_area_selectors", LiveDynamicMode::InheritedMap, live_dynamic_inherited_selectors_);
    clear_live_dynamic_assets();
    force_live_dynamic_sync_next_rebuild_ = true;
}

void Assets::load_camera_settings_from_json() {
    if (!map_info_json_.is_object()) {
        return;
    }
    if (!map_info_json_.contains("camera_settings") || !map_info_json_["camera_settings"].is_object()) {
        map_info_json_["camera_settings"] = nlohmann::json::object();
    }
    const nlohmann::json& camera_settings = map_info_json_["camera_settings"];
    camera_.apply_camera_settings(camera_settings);

    const auto parse_live_dynamic_ratio = [&](const nlohmann::json& source) {
        auto it = source.find("live_dynamic_min_visible_screen_ratio");
        if (it == source.end()) {
            it = source.find("boundary_min_visible_screen_ratio");
        }
        if (it == source.end() || !it->is_number()) {
            return kDefaultBoundaryMinVisibleScreenRatio;
        }
        float value = kDefaultBoundaryMinVisibleScreenRatio;
        try {
            value = it->get<float>();
        } catch (...) {
            return kDefaultBoundaryMinVisibleScreenRatio;
        }
        if (!std::isfinite(value)) {
            return kDefaultBoundaryMinVisibleScreenRatio;
        }
        return std::clamp(value, 0.0f, 0.5f);
    };
    const auto parse_height_bound = [&](const nlohmann::json& source,
                                        const char* key,
                                        int fallback) {
        auto it = source.find(key);
        if (it == source.end() || !it->is_number_integer()) {
            return fallback;
        }
        try {
            const int value = it->get<int>();
            return value;
        } catch (...) {
            return fallback;
        }
    };
    const auto parse_margin_px = [&](const nlohmann::json& source,
                                     const char* key,
                                     int fallback) {
        auto it = source.find(key);
        if (it == source.end() || !it->is_number()) {
            return std::max(0, fallback);
        }
        try {
            const double raw = it->get<double>();
            if (!std::isfinite(raw)) {
                return std::max(0, fallback);
            }
            const std::int64_t rounded = static_cast<std::int64_t>(std::llround(raw));
            return static_cast<int>(std::clamp<std::int64_t>(
                rounded,
                0,
                static_cast<std::int64_t>(std::numeric_limits<int>::max())));
        } catch (...) {
            return std::max(0, fallback);
        }
    };
    const float boundary_ratio = parse_live_dynamic_ratio(camera_settings);
    const int min_height = parse_height_bound(camera_settings, "camera_height_min_px", kDefaultCameraHeightMinPx);
    int max_height = parse_height_bound(camera_settings, "camera_height_max_px", kDefaultCameraHeightMaxPx);
    const int preload_margin = parse_margin_px(
        camera_settings,
        "live_dynamic_preload_margin_world_px",
        live_dynamic_preload_margin_world_px_);
    int despawn_margin = parse_margin_px(
        camera_settings,
        "live_dynamic_despawn_margin_world_px",
        live_dynamic_despawn_margin_world_px_);
    if (max_height < min_height) {
        max_height = kDefaultCameraHeightMaxPx;
    }
    if (max_height < min_height) {
        max_height = min_height;
    }
    if (despawn_margin < preload_margin) {
        despawn_margin = preload_margin;
    }

    set_boundary_min_visible_screen_ratio(boundary_ratio);
    set_camera_height_bounds_px(min_height, max_height);
    live_dynamic_preload_margin_world_px_ = preload_margin;
    live_dynamic_despawn_margin_world_px_ = despawn_margin;
    sync_camera_settings_to_map_info_json();
    apply_camera_runtime_settings();
    camera_view_dirty_ = true;
}

void Assets::write_camera_settings_to_json() {
    if (!map_info_json_.is_object()) {
        return;
    }
    nlohmann::json camera_settings = camera_.camera_settings_to_json();
    camera_settings["live_dynamic_min_visible_screen_ratio"] = boundary_min_visible_screen_ratio_;
    camera_settings["camera_height_min_px"] = camera_height_min_px_;
    camera_settings["camera_height_max_px"] = camera_height_max_px_;
    camera_settings["live_dynamic_preload_margin_world_px"] = live_dynamic_preload_margin_world_px_;
    camera_settings["live_dynamic_despawn_margin_world_px"] = live_dynamic_despawn_margin_world_px_;
    map_info_json_["camera_settings"] = std::move(camera_settings);
}

void Assets::load_runtime_game_config_from_json() {
    if (!map_info_json_.is_object()) {
        return;
    }
    if (!map_info_json_.contains("runtime_game_config") || !map_info_json_["runtime_game_config"].is_object()) {
        map_info_json_["runtime_game_config"] = nlohmann::json::object();
    }

    nlohmann::json& runtime_cfg = map_info_json_["runtime_game_config"];
    if (!runtime_cfg.contains("fly_orbit_behavior") || !runtime_cfg["fly_orbit_behavior"].is_object()) {
        runtime_cfg["fly_orbit_behavior"] = nlohmann::json::object();
    }

    nlohmann::json& fly_cfg_json = runtime_cfg["fly_orbit_behavior"];
    runtime::config::RandomOrbit3DControllerBehaviorConfig cfg =
        runtime::config::make_default_fly_orbit_behavior_config();

    const auto read_int = [&](const char* key, int fallback) {
        auto it = fly_cfg_json.find(key);
        if (it == fly_cfg_json.end() || !it->is_number_integer()) {
            return fallback;
        }
        try {
            return it->get<int>();
        } catch (...) {
            return fallback;
        }
    };
    const auto read_u32 = [&](const char* key, std::uint32_t fallback) {
        auto it = fly_cfg_json.find(key);
        if (it == fly_cfg_json.end() || !it->is_number_integer()) {
            return fallback;
        }
        try {
            const int value = it->get<int>();
            if (value < 0) {
                return fallback;
            }
            return static_cast<std::uint32_t>(value);
        } catch (...) {
            return fallback;
        }
    };

    cfg.orbit_refresh_min_frames =
        read_u32("orbit_refresh_min_frames", cfg.orbit_refresh_min_frames);
    cfg.orbit_refresh_max_frames =
        read_u32("orbit_refresh_max_frames", cfg.orbit_refresh_max_frames);
    cfg.orbit_refresh_min_frames_aggressive =
        read_u32("orbit_refresh_min_frames_aggressive", cfg.orbit_refresh_min_frames_aggressive);
    cfg.orbit_refresh_max_frames_aggressive =
        read_u32("orbit_refresh_max_frames_aggressive", cfg.orbit_refresh_max_frames_aggressive);
    cfg.orbit_height_min_offset =
        read_int("orbit_height_min_offset", cfg.orbit_height_min_offset);
    cfg.orbit_height_max_offset =
        read_int("orbit_height_max_offset", cfg.orbit_height_max_offset);

    runtime_game_config().fly_orbit_behavior.orbit_refresh_min_frames =
        cfg.orbit_refresh_min_frames;
    runtime_game_config().fly_orbit_behavior.orbit_refresh_max_frames =
        cfg.orbit_refresh_max_frames;
    runtime_game_config().fly_orbit_behavior.orbit_refresh_min_frames_aggressive =
        cfg.orbit_refresh_min_frames_aggressive;
    runtime_game_config().fly_orbit_behavior.orbit_refresh_max_frames_aggressive =
        cfg.orbit_refresh_max_frames_aggressive;
    runtime_game_config().fly_orbit_behavior.orbit_height_min_offset =
        cfg.orbit_height_min_offset;
    runtime_game_config().fly_orbit_behavior.orbit_height_max_offset =
        cfg.orbit_height_max_offset;

    write_runtime_game_config_to_json();
}

void Assets::write_runtime_game_config_to_json() {
    if (!map_info_json_.is_object()) {
        return;
    }
    nlohmann::json& runtime_cfg = map_info_json_["runtime_game_config"];
    if (!runtime_cfg.is_object()) {
        runtime_cfg = nlohmann::json::object();
    }

    const auto& cfg = runtime_game_config().fly_orbit_behavior;
    runtime_cfg["fly_orbit_behavior"] = nlohmann::json{
        {"orbit_refresh_min_frames", cfg.orbit_refresh_min_frames},
        {"orbit_refresh_max_frames", cfg.orbit_refresh_max_frames},
        {"orbit_refresh_min_frames_aggressive", cfg.orbit_refresh_min_frames_aggressive},
        {"orbit_refresh_max_frames_aggressive", cfg.orbit_refresh_max_frames_aggressive},
        {"orbit_height_min_offset", cfg.orbit_height_min_offset},
        {"orbit_height_max_offset", cfg.orbit_height_max_offset}
    };
}

void Assets::on_camera_settings_changed() {
    apply_camera_runtime_settings();
    sync_camera_settings_to_map_info_json();
    force_live_dynamic_sync_next_rebuild_ = true;
    mark_camera_dirty();
    camera_view_dirty_ = true;
}

void Assets::mark_camera_dirty() {
    camera_settings_dirty_ = true;
    note_frame_rebuild_request();
}

void Assets::reload_camera_settings() {
    vibble::log::info("[Assets] Reloading camera settings from manifest");
    load_camera_settings_from_json();
    force_live_dynamic_sync_next_rebuild_ = true;
    mark_camera_dirty();  // CRITICAL: Mark camera dirty to trigger refresh on first frame
    camera_view_dirty_ = true;
    vibble::log::info("[Assets] Camera settings reloaded and marked dirty for refresh");
    // Fog logging removed
}

void Assets::force_camera_view_refresh() {
    // Ensure camera/grid state is rebuilt on startup without requiring a Dev Mode toggle
    Room* detected_room = finder_ ? finder_->getCurrentRoom() : nullptr;
    Room* active_room = detected_room;
    const bool dev_controls_enabled = dev_controls_ && dev_controls_->is_enabled();
    if (dev_controls_enabled) {
        active_room = dev_controls_->resolve_current_room(detected_room);
    }
    current_room_ = active_room;

    mark_camera_dirty();
    camera_view_dirty_ = true;
    grid_dirty_ = true;
    force_live_dynamic_sync_next_rebuild_ = true;

    camera_.update_camera_height(current_room_, finder_, player, true, last_frame_dt_seconds_, dev_mode);

    const SDL_Point center_px = camera_.get_screen_center();
    const world::GridPoint center_point = world::GridPoint::make_virtual(
        center_px.x,
        0,
        center_px.y,
        world_grid_.max_resolution_layers());
    const double current_scale = camera_.get_scale();
    const double current_pitch = camera_.current_pitch_radians();

    // Allow initial rebuild even if frame_id_ == last_active_rebuild_frame_id_
    if (last_active_rebuild_frame_id_ == frame_id_) {
        last_active_rebuild_frame_id_ = static_cast<std::uint32_t>(~frame_id_);
    }

    const std::uint64_t current_projection_version = camera_.camera_state_version();
    rebuild_world_grid_and_active_assets(center_point,
                                         current_scale,
                                         current_pitch,
                                         current_projection_version,
                                         true);
    pending_initial_rebuild_ = false;
    active_assets_dirty_.store(false, std::memory_order_release);
    needs_filtered_active_refresh_ = true;
}

void Assets::apply_camera_runtime_settings() {
    WarpedScreenGrid::RealismSettings settings = camera_.realism_settings();
    settings.boundary_min_visible_screen_ratio = boundary_min_visible_screen_ratio_;
    camera_.set_realism_settings(settings);
    render_pipeline::ScalingLogic::SetQualityCap(1.0f);
    finalize_max_asset_dimensions(max_asset_width_world_, max_asset_height_world_);
}

void Assets::log_camera_fog_state(const char* label) const {
    if (!label) {
        return;
    }

    const Room* room = current_room_;
    const auto& settings = camera_.realism_settings();
    const auto& transition = camera_.camera_transition_telemetry();
    const double camera_height = camera_.current_camera_height();
    const float pitch_deg = camera_.current_pitch_degrees();
    const auto& controller_state = camera_.camera_state();
    const SDL_Point center = camera_.get_screen_center();

    vibble::log::info(std::string("[CameraDebug] ") + label +
        " dev_mode=" + (dev_mode ? "YES" : "NO") +
        " room=" + (room ? room->room_name : std::string("none")) +
        " room_height_px=" + std::to_string(room ? room->camera_height_px : 0) +
        " room_tilt_deg=" + std::to_string(room ? room->camera_tilt_deg : 0.0f) +
        " room_zoom_percent=" + std::to_string(room ? room->camera_zoom_percent : 0) +
        " base_height_px=" + std::to_string(settings.base_height_px) +
        " min_visible_screen_ratio=" + std::to_string(settings.min_visible_screen_ratio) +
        " boundary_min_visible_screen_ratio=" + std::to_string(settings.boundary_min_visible_screen_ratio) +
        //" extra_cull_margin=" + std::to_string(settings.extra_cull_margin) +
        " view_height_world=" + std::to_string(camera_.view_height_world()) +
        " camera_height=" + std::to_string(camera_height) +
        " pitch_deg=" + std::to_string(pitch_deg) +
        " zoom_percent=" + std::to_string(controller_state.params.zoom_percent) +
        " scale=" + std::to_string(camera_.get_scale()) +
        " screen_center=(" + std::to_string(center.x) + "," + std::to_string(center.y) + ")" +
        " transition_state=" + std::string(WarpedScreenGrid::transition_state_name(transition.state)) +
        " transition_target=(" + std::to_string(transition.target.x) + "," + std::to_string(transition.target.y) + ")" +
        " transition_velocity=(" + std::to_string(transition.velocity.x) + "," + std::to_string(transition.velocity.y) + ")" +
        " transition_blend=" + std::to_string(transition.blend_factor)
    );
}

bool Assets::fog_visible() const {
    return false;
}

bool Assets::boundary_assets_visible() const {
    if (!dev_controls_ || !dev_controls_->is_enabled()) {
        return true;
    }
    return dev_controls_->boundary_assets_visible();
}

bool Assets::live_dynamic_assets_visible() const {
    return boundary_assets_visible();
}

bool Assets::dev_grid_overlay_enabled() const {
    return dev_controls_ && dev_controls_->is_enabled() && dev_controls_->is_grid_overlay_enabled();
}

int Assets::dev_grid_overlay_cell_size_px() const {
    if (!dev_controls_ || !dev_controls_->is_enabled()) {
        return 1;
    }
    return std::max(1, dev_controls_->grid_cell_size_px());
}

Assets::DevGridOverlayContext Assets::dev_grid_overlay_context() const {
    if (!dev_controls_ || !dev_controls_->is_enabled()) {
        return {};
    }
    return dev_controls_->dev_grid_overlay_context();
}

std::vector<Assets::DevFloorProjectionMarker> Assets::dev_floor_projection_markers() {
    if (!dev_controls_ || !dev_controls_->is_enabled()) {
        return {};
    }
    return dev_controls_->floor_projection_markers_for_floor_pass();
}

float Assets::boundary_min_visible_screen_ratio() const {
    return boundary_min_visible_screen_ratio_;
}

void Assets::set_boundary_min_visible_screen_ratio(float value) {
    boundary_min_visible_screen_ratio_ = std::clamp(value, 0.0f, 0.5f);
}

std::pair<int, int> Assets::camera_height_bounds_px() const {
    return {camera_height_min_px_, camera_height_max_px_};
}

void Assets::set_camera_height_bounds_px(int min_value, int max_value) {
    const int clamped_min = std::clamp(min_value, kDefaultCameraHeightMinPx, kDefaultCameraHeightMaxPx);
    const int clamped_max = std::max(clamped_min,
                                     std::clamp(max_value, kDefaultCameraHeightMinPx, kDefaultCameraHeightMaxPx));
    camera_height_min_px_ = clamped_min;
    camera_height_max_px_ = clamped_max;
    DevCameraHeightBounds::set(static_cast<double>(camera_height_min_px_),
                               static_cast<double>(camera_height_max_px_));
}

void Assets::sync_camera_settings_to_map_info_json() {
    write_camera_settings_to_json();
}

Assets::~Assets() {
    run_exit_save_sequence("assets_shutdown");
    destroy_runtime_ui_overlay_texture();

    movement_commands_buffer_.clear();
    grid_registration_buffer_.clear();
    runtime_traversal_state_.clear();

    if (input) {
        input->clear_screen_to_world_mapper();
    }
    delete finder_;
    delete dev_controls_;

}

AssetLibrary& Assets::library() {
    return library_;
}

const AssetLibrary& Assets::library() const {
    return library_;
}

void Assets::ensure_light_textures_loaded(Asset* /*asset*/) {
}

std::vector<Room*>& Assets::rooms() {
    static std::vector<Room*> empty_rooms;
    return world_context_ ? world_context_->rooms() : empty_rooms;
}

const std::vector<Room*>& Assets::rooms() const {
    static const std::vector<Room*> empty_rooms;
    return world_context_ ? world_context_->rooms() : empty_rooms;
}

std::size_t Assets::rooms_generation() const {
    return world_context_ ? world_context_->topology_generation() : 0;
}

RuntimeWorldContext* Assets::runtime_world_context() {
    return world_context_.get();
}

const RuntimeWorldContext* Assets::runtime_world_context() const {
    return world_context_.get();
}

runtime::config::RuntimeGameConfig& Assets::runtime_game_config() {
    static runtime::config::RuntimeGameConfig fallback{};
    return world_context_ ? world_context_->game_config() : fallback;
}

const runtime::config::RuntimeGameConfig& Assets::runtime_game_config() const {
    static const runtime::config::RuntimeGameConfig fallback{};
    return world_context_ ? world_context_->game_config() : fallback;
}

void Assets::notify_rooms_changed() {
    if (world_context_) {
        world_context_->notify_topology_changed();
    }
    if (finder_) {
        auto& room_refs = rooms();
        finder_->setRooms(room_refs);
        game_context_.rebuild_runtime_map_graph(room_refs);
    }
    if (dev_controls_) {
        auto& room_refs = rooms();
        dev_controls_->set_rooms(&room_refs, rooms_generation());
    }
}

void Assets::refresh_active_asset_lists() {
    run_frame_rebuild_stage();
    run_runtime_effects_stage();
    update_filtered_active_assets();
}

void Assets::mark_anchor_basis_dirty(Asset* asset) {
    if (!asset || asset->dead) {
        return;
    }
    RuntimeTraversalState& state = runtime_traversal_state_[asset];
    state.pending_anchor_invalidation_version = next_anchor_invalidation_version();
}

void Assets::mark_anchor_bases_dirty_for_active_assets() {
    const std::uint64_t version = next_anchor_invalidation_version();
    auto mark_with_version = [&](Asset* asset) {
        if (!asset || asset->dead) {
            return;
        }
        RuntimeTraversalState& state = runtime_traversal_state_[asset];
        state.pending_anchor_invalidation_version = version;
    };

    mark_with_version(player);
    for (Asset* asset : active_assets) {
        if (asset == player) {
            continue;
        }
        mark_with_version(asset);
    }
}

std::uint64_t Assets::next_anchor_invalidation_version() {
    ++anchor_invalidation_version_counter_;
    if (anchor_invalidation_version_counter_ == 0) {
        ++anchor_invalidation_version_counter_;
    }
    return anchor_invalidation_version_counter_;
}

Assets::RuntimeConvergencePassResult Assets::run_active_runtime_single_pass(bool include_audio_update) {
    const SDL_Point camera_focus = camera_.get_screen_center();
    const std::uint64_t camera_state_version = camera_.camera_state_version();
    const float camera_anchor_world_z = static_cast<float>(camera_.anchor_world_z());
    const world::CameraProjectionParams projection = camera_.projection_params();
    const float depth_axis_sign = depth_axis_sign_from_forward_z(
        static_cast<float>(projection.forward_z));

    const bool startup_batching = startup_runtime_safety_active(frame_id_);
    const std::size_t active_total = active_assets.size();
    std::size_t active_begin = 0;
    std::size_t active_end = active_total;
    bool partial_pass = false;
    if (startup_batching) {
        const std::size_t batch_size = startup_runtime_pass_asset_batch_size();
        if (batch_size > 0 && active_total > batch_size) {
            if (startup_runtime_pass_cursor_ >= active_total) {
                startup_runtime_pass_cursor_ = 0;
            }
            active_begin = startup_runtime_pass_cursor_;
            active_end = std::min(active_total, active_begin + batch_size);
            startup_runtime_pass_cursor_ = (active_end >= active_total) ? 0 : active_end;
            partial_pass = active_end < active_total;
        } else {
            startup_runtime_pass_cursor_ = 0;
        }
    } else {
        startup_runtime_pass_cursor_ = 0;
    }

    if (player && asset_matches_focus_filter(player)) {
        run_active_runtime_single_pass_for_asset(player,
                                                 camera_focus,
                                                 camera_state_version,
                                                 camera_anchor_world_z,
                                                 depth_axis_sign);
    }
    for (std::size_t i = active_begin; i < active_end; ++i) {
        Asset* asset = active_assets[i];
        if (asset == player) {
            continue;
        }
        if (!asset_matches_focus_filter(asset)) {
            continue;
        }
        run_active_runtime_single_pass_for_asset(asset,
                                                 camera_focus,
                                                 camera_state_version,
                                                 camera_anchor_world_z,
                                                 depth_axis_sign);
    }

    run_camera_trap_escape_pass();

    if (include_audio_update && last_audio_engine_update_frame_id_ != frame_id_) {
        AudioEngine::instance().update();
        last_audio_engine_update_frame_id_ = frame_id_;
    }

    const anchor_bound_asset_helper::AnchorBoundAssetHelper::FlushResult helper_result =
        anchor_bound_asset_helper::AnchorBoundAssetHelper::instance().flush_pending_updates_detailed();

    RuntimeConvergencePassResult result{};
    result.any_change = helper_result.any_change;
    result.needs_repass = helper_result.needs_repass || partial_pass;
    result.needs_traversal_refresh = helper_result.needs_traversal_refresh;
    result.wave_count = helper_result.wave_count;
    result.children_considered = helper_result.children_considered;
    result.children_updated = helper_result.children_updated;
    return result;
}

void Assets::run_active_runtime_single_pass_for_asset(Asset* asset,
                                                      const SDL_Point& camera_focus,
                                                      std::uint64_t camera_state_version,
                                                      float camera_anchor_world_z,
                                                      float depth_axis_sign) {
    if (!asset || asset->dead) {
        return;
    }

    RuntimeTraversalState& state = runtime_traversal_state_[asset];
    const bool invalidation_pending =
        state.pending_anchor_invalidation_version != state.processed_anchor_invalidation_version;
    const bool anchor_revision_changed =
        state.processed_anchor_revision != asset->anchor_world_revision_;
    const bool camera_anchor_state_changed =
        state.processed_camera_state_version != camera_state_version;
    const int current_frame_index = asset->current_frame ? asset->current_frame->frame_index : -1;
    const bool frame_index_changed = state.processed_frame_index != current_frame_index;

    if (invalidation_pending || anchor_revision_changed || camera_anchor_state_changed || frame_index_changed) {
        asset->update_anchor_basis_if_needed();
        asset->refresh_anchor_point_cache_from_frame();
        asset->refresh_runtime_box_cache_from_frame();

        const Asset::RuntimeImpassableGeometrySignature current_signature =
            asset->runtime_impassable_geometry_signature();
        const bool signature_changed =
            !state.processed_impassable_signature_initialized ||
            state.processed_impassable_signature != current_signature;
        const bool runtime_geometry_may_have_changed =
            anchor_revision_changed || camera_anchor_state_changed || frame_index_changed;
        if (runtime_geometry_may_have_changed && signature_changed) {
            mark_collision_context_dirty();
        }

        state.processed_anchor_invalidation_version = state.pending_anchor_invalidation_version;
        state.processed_anchor_revision = asset->anchor_world_revision_;
        state.processed_camera_state_version = camera_state_version;
        state.processed_frame_index = current_frame_index;
        state.processed_impassable_signature = current_signature;
        state.processed_impassable_signature_initialized = true;
    }

    const float dx = static_cast<float>(asset->world_x() - camera_focus.x);
    const float dz = static_cast<float>(asset->world_z() - camera_focus.y);
    const float world_z = static_cast<float>(asset->world_z());
    const float effective_world_z = world_z + asset->world_z_offset() + asset->render_anchor_offset_z();

    RuntimeCameraMetrics metrics{};
    metrics.frame_id = frame_id_;
    metrics.camera_state_version = camera_state_version;
    metrics.anchor_revision = asset->anchor_world_revision();
    metrics.valid = true;
    metrics.planar_dx = dx;
    metrics.planar_dz = dz;
    metrics.planar_distance = std::sqrt(dx * dx + dz * dz);
    metrics.planar_angle_radians = std::atan2(dz, dx);
    metrics.anchor_world_z = camera_anchor_world_z;
    metrics.depth_axis_sign = normalize_depth_axis_sign(depth_axis_sign);
    metrics.world_z_depth_offset = depth_offset_from_world_z(
        world_z,
        camera_anchor_world_z,
        metrics.depth_axis_sign);
    metrics.effective_world_z_depth_offset = depth_offset_from_world_z(
        effective_world_z,
        camera_anchor_world_z,
        metrics.depth_axis_sign);
    metrics.world_z_depth_from_anchor = render_depth::depth_from_anchor(
        static_cast<double>(camera_anchor_world_z),
        static_cast<double>(world_z),
        asset->render_depth_bias());
    metrics.effective_world_z_depth_from_anchor = render_depth::depth_from_anchor(
        static_cast<double>(camera_anchor_world_z),
        static_cast<double>(effective_world_z),
        asset->render_depth_bias());

    asset->runtime_camera_metrics = metrics;
    asset->distance_from_camera = metrics.planar_distance;
    asset->angle_from_camera = metrics.planar_angle_radians;
}


void Assets::run_camera_trap_escape_pass() {
    if (startup_runtime_safety_active(frame_id_) &&
        frame_id_ <= startup_skip_trap_escape_frames()) {
        return;
    }

    std::unordered_set<Asset*> seen;
    seen.reserve(active_assets.size() + 1);
    std::size_t processed_assets = 0;

    auto process_asset = [&](Asset* asset) {
        if (!asset || asset->dead || !asset->info || !asset->isMovementEnabled()) {
            return;
        }
        if (!asset_matches_focus_filter(asset)) {
            return;
        }
        if (!seen.insert(asset).second) {
            return;
        }
        ++processed_assets;

        const world::GridPoint current = world::GridPoint::make_virtual(
            asset->world_x(),
            asset->world_y(),
            asset->world_z(),
            asset->grid_resolution);
        const world::GridPoint bottom = animation_update::detail::bottom_middle_for(*asset, current);

        std::vector<const FrameCollisionEntry*> entries;
        int search_radius = (asset->info && asset->info->NeighborSearchRadius > 0)
            ? asset->info->NeighborSearchRadius
            : 0;
        if (startup_runtime_safety_active(frame_id_)) {
            search_radius = std::min(search_radius, 128);
        }
        query_impassable_entries(*asset, search_radius, entries);

        bool inside_impassable = false;
        for (const FrameCollisionEntry* entry : entries) {
            if (!entry || !entry->asset || entry->asset == asset || !entry->asset->info) {
                continue;
            }
            if (entry->area.contains_point(bottom.to_sdl_point())) {
                inside_impassable = true;
                break;
            }
        }
        if (!inside_impassable) {
            return;
        }

        world::GridPoint destination = current;
        if (!animation::unstick::resolve_destination(*asset, this, entries, current, destination)) {
            return;
        }

        if (destination.world_x() == current.world_x() && destination.world_z() == current.world_z()) {
            return;
        }

        asset->move_to_world_position(destination.world_x(), asset->world_y(), destination.world_z());
        Asset* moved = world_grid_.move_asset(asset, current, destination);
        if (moved) {
            moved->cache_grid_residency(destination);
            mark_anchor_basis_dirty(moved);
        }
        mark_collision_context_dirty();
        note_frame_rebuild_request();
    };

    process_asset(player);
    const std::size_t startup_cap = startup_runtime_safety_active(frame_id_)
        ? startup_non_player_update_batch_size()
        : 0;
    for (Asset* asset : active_assets) {
        if (startup_cap > 0 && processed_assets >= startup_cap) {
            break;
        }
        process_asset(asset);
    }
}

void Assets::rebuild_frame_collision_context() const {
    if (!frame_collision_context_dirty_) {
        return;
    }

    std::size_t estimated_entry_count = 0;
    for (Asset* asset : active_assets) {
        if (!asset || asset->dead || !asset->info || !asset->affects_collision_context()) {
            continue;
        }
        const auto& impassable_shapes = asset->current_impassable_shapes();
        for (const auto& shape : impassable_shapes) {
            if (runtime_impassable_shape_contributes(shape)) {
                ++estimated_entry_count;
            }
        }
    }

    frame_collision_entries_.clear();
    frame_collision_entries_.reserve(estimated_entry_count);
    frame_collision_index_.clear();
    frame_collision_index_.reserve(estimated_entry_count);
    frame_collision_query_cache_.clear();
    ++frame_collision_context_version_;
    if (frame_collision_context_version_ == 0) {
        ++frame_collision_context_version_;
    }

    const world::GridPoint origin = world_grid_.origin();
    const int cell_spacing = runtime_collision_index_cell_spacing_world(map_grid_settings_);

    for (Asset* asset : active_assets) {
        if (!asset || asset->dead || !asset->info || !asset->affects_collision_context()) {
            continue;
        }

        auto push_collision_entry = [&](Area collision, const std::string& resolved_type, SDL_Point world_center) {
            if (collision.get_points().empty()) {
                return;
            }
            const world::GridPoint center = world::grid_math::from_sdl(
                world_center,
                asset->world_y(),
                asset->grid_resolution);
            const world::GridPoint bottom_middle =
                animation_update::detail::bottom_middle_for(*asset, center);
            frame_collision_entries_.push_back(FrameCollisionEntry{
                asset,
                std::move(collision),
                world_center,
                bottom_middle,
                resolved_type
            });
            FrameCollisionEntry& entry = frame_collision_entries_.back();
            const int cell_x = vibble::math::floor_div(world_center.x - origin.world_x(), cell_spacing);
            const int cell_z = vibble::math::floor_div(world_center.y - origin.world_z(), cell_spacing);
            const world::GridCoord cell{cell_x, cell_z};
            frame_collision_index_[hash_grid_cell(cell)].push_back(&entry);
        };

        const auto& impassable_shapes = asset->current_impassable_shapes();
        for (const auto& shape : impassable_shapes) {
            Area area{"impassable"};
            if (!build_impassable_area_for_shape(*asset, shape, area)) {
                continue;
            }
            const SDL_Point center = area.get_center();
            push_collision_entry(std::move(area), std::string("impassable_shape"), center);
        }
    }

    frame_collision_context_frame_id_ = frame_id_;
    frame_collision_context_dirty_ = false;
}

void Assets::query_impassable_entries(const Asset& self,
                                      int search_radius,
                                      std::vector<const FrameCollisionEntry*>& out) const {
    rebuild_frame_collision_context();

    out.clear();
    if (frame_collision_entries_.empty()) {
        return;
    }

    const int radius = (search_radius > 0)
        ? std::min(search_radius, kAggressiveMaxCollisionSearchRadiusPx)
        : 0;
    const std::int64_t radius_sq = static_cast<std::int64_t>(radius) * static_cast<std::int64_t>(radius);
    const SDL_Point self_center = self.world_xz_point();
    const FrameCollisionQueryKey cache_key{
        frame_collision_context_version_,
        &self,
        self_center.x,
        self_center.y,
        radius
    };
    if (const auto cache_it = frame_collision_query_cache_.find(cache_key);
        cache_it != frame_collision_query_cache_.end()) {
        out = cache_it->second;
        return;
    }

    const world::GridPoint origin = world_grid_.origin();
    const int cell_spacing = runtime_collision_index_cell_spacing_world(map_grid_settings_);
    const int self_cell_x = vibble::math::floor_div(self_center.x - origin.world_x(), cell_spacing);
    const int self_cell_z = vibble::math::floor_div(self_center.y - origin.world_z(), cell_spacing);
    const world::GridCoord self_cell{self_cell_x, self_cell_z};

    auto consider_entry = [&](const FrameCollisionEntry* entry) {
        if (!entry || !entry->asset || entry->asset == &self || !entry->asset->info) {
            return;
        }
        if (radius > 0) {
            const std::int64_t dx = static_cast<std::int64_t>(entry->world_center.x) - static_cast<std::int64_t>(self_center.x);
            const std::int64_t dy = static_cast<std::int64_t>(entry->world_center.y) - static_cast<std::int64_t>(self_center.y);
            const std::int64_t dist_sq = dx * dx + dy * dy;
            if (dist_sq > radius_sq) {
                return;
            }
        }
        out.push_back(entry);
    };

    out.reserve(frame_collision_entries_.size());
    auto cache_results = [&]() {
        constexpr std::size_t kMaxCachedCollisionQueries = 4096;
        if (frame_collision_query_cache_.size() >= kMaxCachedCollisionQueries) {
            frame_collision_query_cache_.clear();
        }
        frame_collision_query_cache_[cache_key] = out;
    };

    if (radius <= 0) {
        for (const auto& bucket : frame_collision_index_) {
            for (const FrameCollisionEntry* entry : bucket.second) {
                consider_entry(entry);
            }
        }
        cache_results();
        return;
    }

    const int cell_radius = (radius + cell_spacing - 1) / cell_spacing;
    for (int dz = -cell_radius; dz <= cell_radius; ++dz) {
        for (int dx = -cell_radius; dx <= cell_radius; ++dx) {
            world::GridCoord neighbor{self_cell.x + dx, self_cell.z + dz};
            const std::uint64_t cell_hash = hash_grid_cell(neighbor);
            const auto it = frame_collision_index_.find(cell_hash);
            if (it == frame_collision_index_.end()) {
                continue;
            }
            for (const FrameCollisionEntry* entry : it->second) {
                consider_entry(entry);
            }
        }
    }

    cache_results();
}


int Assets::max_impassable_query_radius() const {
    return kAggressiveMaxCollisionSearchRadiusPx;
}
void Assets::refresh_filtered_active_assets() {
    update_filtered_active_assets();
}

void Assets::rebuild_focus_filter_closure() {
    focus_filter_closure_.clear();
    focus_filter_closure_dirty_ = false;

    if (!focus_filter_active_ || !focus_filter_asset_ || focus_filter_asset_->dead) {
        return;
    }

    std::vector<const Asset*> stack;
    stack.push_back(focus_filter_asset_);

    while (!stack.empty()) {
        const Asset* current = stack.back();
        stack.pop_back();
        if (!current || current->dead) {
            continue;
        }
        if (!focus_filter_closure_.insert(current).second) {
            continue;
        }
        for (const Asset* child : current->children()) {
            stack.push_back(child);
        }
    }
}

void Assets::mark_focus_filter_closure_dirty() {
    focus_filter_closure_dirty_ = true;
}

bool Assets::asset_matches_focus_filter(const Asset* asset) const {
    if (!focus_filter_active_) {
        return true;
    }
    if (!asset || asset->dead) {
        return false;
    }
    if (focus_filter_asset_) {
        return focus_filter_closure_.find(asset) != focus_filter_closure_.end();
    }
    if (!focus_filter_spawn_id_.empty()) {
        return asset->spawn_id == focus_filter_spawn_id_;
    }
    return true;
}

void Assets::set_focus_filter(Asset* asset, const std::string& spawn_id) {
    const bool next_active = (asset != nullptr) || !spawn_id.empty();
    const bool same_state =
        focus_filter_active_ == next_active &&
        focus_filter_asset_ == asset &&
        focus_filter_spawn_id_ == (asset ? std::string{} : spawn_id);
    if (same_state) {
        if (focus_filter_closure_dirty_) {
            rebuild_focus_filter_closure();
        }
        return;
    }

    focus_filter_active_ = next_active;
    focus_filter_asset_ = asset;
    focus_filter_spawn_id_ = asset ? std::string{} : spawn_id;
    mark_focus_filter_closure_dirty();
    rebuild_focus_filter_closure();
    ++focus_filter_version_;
    if (focus_filter_version_ == 0) {
        ++focus_filter_version_;
    }

    needs_filtered_active_refresh_ = true;
    touch_dev_active_state_version();
    note_frame_rebuild_request();
}

void Assets::clear_focus_filter() {
    set_focus_filter(nullptr, std::string{});
}

bool Assets::is_asset_in_focus_filter(const Asset* asset) const {
    return asset_matches_focus_filter(asset);
}

bool Assets::is_spawn_id_in_focus_filter(const std::string& spawn_id) const {
    if (!focus_filter_active_) {
        return true;
    }
    if (focus_filter_asset_) {
        return false;
    }
    if (focus_filter_spawn_id_.empty()) {
        return true;
    }
    return spawn_id == focus_filter_spawn_id_;
}

void Assets::update_filtered_active_assets() {
    const bool dev_controls_enabled = dev_controls_ && dev_controls_->is_enabled();
    const std::uint64_t current_generation = active_assets_generation_;
    const std::uint64_t base_filter_version = dev_controls_enabled ? dev_controls_->other_settings_state_version() : 0;
    const std::uint64_t filter_version = base_filter_version ^ (focus_filter_version_ * 1099511628211ULL);

    if (dev_controls_enabled) {
        if (filtered_active_assets_source_generation_ == current_generation &&
            filtered_active_assets_filter_version_ == filter_version) {
            return;
        }

        // Snapshot the active size for pre-sizing both the filtered list and membership cache.
        const std::size_t active_size = active_assets.size();
        // When DevControls is off, ensure the membership cache is reset so future toggles rebuild reliably.
        filtered_active_assets.clear();
        filtered_active_asset_membership_.clear();
        filtered_active_assets.reserve(active_size);
        for (Asset* asset : active_assets) {
            if (!asset) {
                continue;
            }
            if (!dev_controls_->passes_asset_filters(asset)) {
                continue;
            }
            if (!asset_matches_focus_filter(asset)) {
                continue;
            }
            filtered_active_assets.push_back(asset);
            filtered_active_asset_membership_.insert(asset);
        }

        filtered_active_assets_source_generation_ = current_generation;
        filtered_active_assets_filter_version_ = filter_version;
        dev_controls_->filter_active_assets(filtered_active_assets);
    } else {
        if (filtered_active_assets.empty() &&
            filtered_active_asset_membership_.empty() &&
            filtered_active_assets_source_generation_ == current_generation &&
            filtered_active_assets_filter_version_ == 0) {
            return;
        }

        filtered_active_assets.clear();
        filtered_active_asset_membership_.clear();
        filtered_active_assets_source_generation_ = current_generation;
        filtered_active_assets_filter_version_ = 0;
    }

    touch_dev_active_state_version();
}

void Assets::log_asset_movement(Asset* asset, const world::GridPoint& previous, const world::GridPoint& current) {
    if (!asset) {
        return;
    }
    if (previous.world_x() == current.world_x() &&
        previous.world_y() == current.world_y() &&
        previous.world_z() == current.world_z()) {
        return;
    }
    if (!world_grid_.point_for_asset(asset)) {
        vibble::log::error("[Assets] Ignoring movement for asset that is not currently owned by WorldGrid.");
        return;
    }
    movement_commands_buffer_.push_back(GridMovementCommand{
        asset,
        previous,
        current
    });
}

void Assets::reset_dev_controls_current_room_cache() {
    dev_controls_last_room_ = nullptr;
}

void Assets::sync_dev_controls_current_room(Room* room, bool force_refresh) {
    if (!dev_controls_) {
        return;
    }
    if (!force_refresh && dev_controls_last_room_ == room) {
        return;
    }
    dev_controls_last_room_ = room;
    dev_controls_->set_current_room(room, force_refresh);
}

void Assets::ensure_dev_controls() {
    if (dev_controls_) {
        return;
    }

    suppress_dev_renderer_ = true;

    const char* msg_create = "[Assets] Creating Dev Controls";
    std::cout << msg_create << "\n";

    DevControls* created = nullptr;
    try {
        created = new DevControls(this, screen_width, screen_height);
    } catch (const std::exception& ex) {
        std::cout << "[Assets] Dev Controls constructor threw: " << ex.what() << "\n";
        created = nullptr;
    } catch (...) {
        std::cout << "[Assets] Dev Controls constructor threw unknown error\n";
        created = nullptr;
    }

    if (!created) {
        const char* msg_fail = "[Assets] Failed to allocate Dev Controls";
        std::cout << msg_fail << "\n";

        suppress_dev_renderer_ = false;
        return;
    }

    dev_controls_ = created;
    const char* msg_constructed = "[Assets] Dev Controls constructed, wiring context";
    std::cout << msg_constructed << "\n";

    try {
        reset_dev_controls_current_room_cache();

        dev_controls_->set_player(player);
        dev_controls_->set_parent_window(app_window_);
        dev_controls_->set_active_assets(filtered_active_assets, dev_active_state_version_);
        sync_dev_controls_current_room(current_room_, true);
        dev_controls_->set_screen_dimensions(screen_width, screen_height);
        auto& room_refs = rooms();
        dev_controls_->set_rooms(&room_refs, rooms_generation());
        dev_controls_->set_input(input);
        dev_controls_->set_map_info(&map_info_json_);
        dev_controls_->set_map_context(&map_info_json_, map_path_);
        suppress_dev_renderer_ = false;
    } catch (const std::exception& ex) {
        std::cout << "[Assets] Failed to wire Dev Controls: " << ex.what() << "\n";

        delete dev_controls_;
        dev_controls_ = nullptr;
    } catch (...) {
        std::cout << "[Assets] Failed to wire Dev Controls: unknown error\n";
        delete dev_controls_;
        dev_controls_ = nullptr;
    }
}

void Assets::set_input(Input* m) {
    if (input && input != m) {
        input->clear_screen_to_world_mapper();
    }

    input = m;

    if (input) {
        input->set_screen_to_world_mapper([this](SDL_Point screen) {
            SDL_FPoint mapped = camera_.screen_to_map(screen);
            return SDL_Point{static_cast<int>(std::lround(mapped.x)), static_cast<int>(std::lround(mapped.y))};
        });
    }

    if (dev_controls_) {
        dev_controls_->set_input(m);
        if (dev_controls_->is_enabled()) {
            dev_controls_->set_player(player);
            dev_controls_->set_active_assets(filtered_active_assets, dev_active_state_version_);
            sync_dev_controls_current_room(current_room_);
            dev_controls_->set_screen_dimensions(screen_width, screen_height);
            auto& room_refs = rooms();
            dev_controls_->set_rooms(&room_refs, rooms_generation());
            dev_controls_->set_map_context(&map_info_json_, map_path_);
        }
    }
}

void Assets::run_idle_frame_pipeline(const Input& input) {
    game_context_.begin_frame(
        this,
        frame_id_,
        0.0f,
        current_room_,
        player,
        &camera_,
        &runtime_game_config());
    run_visibility_build_stage();
    run_runtime_effects_stage(false);
    sync_dev_controls_for_frame(input);
    refresh_filtered_active_assets_if_needed();
    render_runtime_frame();
    finalize_dev_frame_state();
}

void Assets::run_world_update_stage(const Input& input, bool& room_changed, bool& player_moved) {
    Room* detected_room = finder_ ? finder_->getCurrentRoom() : nullptr;
    Room* active_room = detected_room;
    if (dev_controls_ && dev_controls_->is_enabled()) {
        active_room = dev_controls_->resolve_current_room(detected_room);
    }
    room_changed = (current_room_ != active_room);
    if (room_changed) {
        force_live_dynamic_sync_next_rebuild_ = true;
    }
    current_room_ = active_room;
    game_context_.begin_frame(
        this,
        frame_id_,
        frame_delta_seconds_clamped(),
        current_room_,
        player,
        &camera_,
        &runtime_game_config());

    delta_x_ = delta_z_ = 0;

    // Pause runtime asset updates while in Dev Mode unless a frame editor session requires them.
    const bool runtime_updates_enabled = should_run_runtime_updates();
    const auto should_process_asset = [this](const Asset* asset) {
        return asset_matches_focus_filter(asset);
    };

    int start_px = player ? player->world_x() : 0;
    int start_pz = player ? player->world_z() : 0;

    if (player && should_process_asset(player)) {
        player->active = true;
        if (runtime_updates_enabled) {
            player->update();
        } else {
            if (player->info) {
                player->update_scale_values();
            }
        }
    }

    player_moved = false;
    if (player) {
        delta_x_ = player->world_x() - start_px;
        delta_z_ = player->world_z() - start_pz;
        const bool moved_during_update = (delta_x_ != 0 || delta_z_ != 0);
        world::GridPoint current_player_pos = world::GridPoint::make_virtual(player->world_x(),
                                                                             player->world_y(),
                                                                             player->world_z(),
                                                                             player->grid_resolution);
        const bool moved_since_last_frame =
            !last_player_pos_valid_ ||
            current_player_pos.world_x() != last_known_player_pos_.world_x() ||
            current_player_pos.world_z() != last_known_player_pos_.world_z() ||
            current_player_pos.world_y() != last_known_player_pos_.world_y();

        last_known_player_pos_ = std::move(current_player_pos);
        last_player_pos_valid_ = true;

        player_moved = moved_during_update || moved_since_last_frame;
        if (runtime_updates_enabled && moved_during_update) {
            log_asset_movement(player,
                               world::GridPoint::make_virtual(start_px, 0, start_pz, player->grid_resolution),
                               current_player_pos);
        }
    } else {
        last_player_pos_valid_ = false;
    }

    rebuild_non_player_update_buffer_if_needed();
    const bool startup_batching = startup_runtime_safety_active(frame_id_);
    std::size_t update_begin = 0;
    std::size_t update_end = non_player_update_buffer_.size();
    if (startup_batching) {
        const std::size_t batch_size = startup_non_player_update_batch_size();
        if (batch_size > 0 && update_end > batch_size) {
            if (startup_non_player_update_cursor_ >= update_end) {
                startup_non_player_update_cursor_ = 0;
            }
            update_begin = startup_non_player_update_cursor_;
            update_end = std::min(update_end, update_begin + batch_size);
            startup_non_player_update_cursor_ = (update_end >= non_player_update_buffer_.size()) ? 0 : update_end;
        } else {
            startup_non_player_update_cursor_ = 0;
        }
    } else {
        startup_non_player_update_cursor_ = 0;
    }

    for (std::size_t index = update_begin; index < update_end; ++index) {
        Asset* asset = non_player_update_buffer_[index];
        if (!asset) continue;
        if (!should_process_asset(asset)) {
            asset->active = false;
            continue;
        }
        world::GridPoint previous_pos = world::GridPoint::make_virtual(asset->world_x(),
                                                                       asset->world_y(),
                                                                       asset->world_z(),
                                                                       asset->grid_resolution);
        asset->active = true;

        if (runtime_updates_enabled) {
            asset->update();
            if (previous_pos.world_x() != asset->world_x() ||
                previous_pos.world_y() != asset->world_y() ||
                previous_pos.world_z() != asset->world_z()) {
                log_asset_movement(asset,
                                   previous_pos,
                                   world::GridPoint::make_virtual(asset->world_x(),
                                                                  asset->world_y(),
                                                                  asset->world_z(),
                                                                  asset->grid_resolution));
            }
        } else {
            if (asset->info) {
                asset->update_scale_values();
            }
        }
    }

    if (runtime_updates_enabled) {
        if (!movement_commands_buffer_.empty()) {
            for (const GridMovementCommand& cmd : movement_commands_buffer_) {
                if (!cmd.asset) continue;
                Asset* moved = world_grid_.move_asset(cmd.asset, cmd.previous, cmd.current);
                if (!moved) {
                    vibble::log::error("[Assets] Skipping post-move cache updates because WorldGrid::move_asset failed.");
                    continue;
                }
                moved->cache_grid_residency(cmd.current);
                mark_anchor_basis_dirty(moved);
            }
            movement_commands_buffer_.clear();

            moving_assets_for_grid_.clear();
            grid_registration_buffer_.clear();
            touch_dev_active_state_version();
            mark_grid_dirty();
        }

    } else {
        movement_commands_buffer_.clear();
        moving_assets_for_grid_.clear();
        grid_registration_buffer_.clear();
    }

    const bool camera_motion_active_before_update = camera_.is_height_animating();
    const bool camera_refresh_needed = room_changed || player_moved || camera_motion_active_before_update || camera_settings_dirty_;
    if (dev_controls_) {
        dev_controls_->sync_camera_tilt_override();
    }
    camera_.update_camera_height(current_room_, finder_, player, camera_refresh_needed, last_frame_dt_seconds_, dev_mode);
    if (camera_refresh_needed || camera_.is_height_animating()) {
        note_frame_rebuild_request();
    }
    camera_settings_dirty_ = false;

    update_max_asset_dimensions();

    culled_debug_rects_.clear();

    sync_dev_controls_for_frame(input);

    register_pending_static_assets();
    if (process_removals()) {
        mark_active_assets_dirty();
    }
}

void Assets::run_visibility_build_stage() {
    if (startup_runtime_safety_active(frame_id_) && !pending_initial_rebuild_) {
        const std::uint32_t interval = startup_visibility_refresh_interval_frames();
        if (interval > 1 && (frame_id_ % interval) != 0) {
            return;
        }
    }

    run_frame_rebuild_stage();
    if (focus_filter_closure_dirty_) {
        rebuild_focus_filter_closure();
    }
    refresh_visible_asset_scaling_only();
}

void Assets::run_post_flush_traversal_refresh_once() {
    post_runtime_traversal_refresh_pending_ = true;
    note_frame_rebuild_request();
    run_frame_rebuild_stage();
    if (focus_filter_closure_dirty_) {
        rebuild_focus_filter_closure();
    }
    refresh_visible_asset_scaling_only();
}

void Assets::run_runtime_effects_stage(bool include_audio_update) {
    auto elapsed_ms = [this](Uint64 begin, Uint64 end) -> double {
        if (end <= begin || perf_counter_frequency_ <= 0.0) {
            return 0.0;
        }
        return static_cast<double>(end - begin) * 1000.0 / perf_counter_frequency_;
    };

    if (startup_runtime_safety_active(frame_id_) &&
        frame_id_ <= startup_skip_runtime_effects_frames()) {
        RuntimeConvergenceFrameStats skipped{};
        skipped.converged = false;
        skipped.iterations = 0;
        skipped.stage_ms = 0.0;
        last_runtime_convergence_stats_ = skipped;
        return;
    }

    const Uint64 stage_begin = SDL_GetPerformanceCounter();
    RuntimeConvergenceFrameStats stats{};
    bool audio_update_pending = include_audio_update;
    const std::size_t iteration_cap = runtime_convergence_iteration_cap_for_frame(frame_id_);
    const double stage_budget_ms = runtime_convergence_stage_budget_ms_for_frame(frame_id_);
    std::size_t iteration = 0;
    for (; iteration < iteration_cap; ++iteration) {
        const Uint64 pass_begin = SDL_GetPerformanceCounter();
        const RuntimeConvergencePassResult pass_result =
            run_active_runtime_single_pass(audio_update_pending);
        const Uint64 pass_end = SDL_GetPerformanceCounter();
        stats.pass_ms += elapsed_ms(pass_begin, pass_end);
        audio_update_pending = false;

        stats.iterations = iteration + 1;
        stats.wave_count += pass_result.wave_count;
        stats.children_considered += pass_result.children_considered;
        stats.children_updated += pass_result.children_updated;

        if (pass_result.needs_traversal_refresh && allow_traversal_refresh_for_frame(frame_id_)) {
            const Uint64 refresh_begin = SDL_GetPerformanceCounter();
            run_post_flush_traversal_refresh_once();
            const Uint64 refresh_end = SDL_GetPerformanceCounter();
            stats.refresh_ms += elapsed_ms(refresh_begin, refresh_end);
            ++stats.traversal_refresh_count;
        }

        if (!pass_result.needs_repass) {
            stats.converged = true;
            break;
        }

        if (stage_budget_ms > 0.0) {
            const Uint64 budget_now = SDL_GetPerformanceCounter();
            if (elapsed_ms(stage_begin, budget_now) >= stage_budget_ms) {
                break;
            }
        }
    }
    const Uint64 stage_end = SDL_GetPerformanceCounter();
    stats.stage_ms = elapsed_ms(stage_begin, stage_end);
    last_runtime_convergence_stats_ = stats;

    if (vibble_runtime_convergence_trace_enabled()) {
        vibble::log::debug(
            std::string("[Assets] Runtime convergence frame=") + std::to_string(frame_id_) +
            " iterations=" + std::to_string(stats.iterations) +
            " converged=" + (stats.converged ? "1" : "0") +
            " waves=" + std::to_string(stats.wave_count) +
            " children_considered=" + std::to_string(stats.children_considered) +
            " children_updated=" + std::to_string(stats.children_updated) +
            " traversal_refreshes=" + std::to_string(stats.traversal_refresh_count) +
            " pass_ms=" + std::to_string(stats.pass_ms) +
            " refresh_ms=" + std::to_string(stats.refresh_ms) +
            " stage_ms=" + std::to_string(stats.stage_ms));
    }

    if (!stats.converged && last_runtime_convergence_warning_frame_id_ != frame_id_) {
        last_runtime_convergence_warning_frame_id_ = frame_id_;
        std::cerr << "[Assets] Anchor convergence cap reached ("
                  << iteration_cap
                  << ") at frame " << frame_id_
                  << "; iterations=" << stats.iterations
                  << " waves=" << stats.wave_count
                  << " children_updated=" << stats.children_updated
                  << " traversal_refreshes=" << stats.traversal_refresh_count
                  << " pass_ms=" << stats.pass_ms
                  << " refresh_ms=" << stats.refresh_ms
                  << " stage_ms=" << stats.stage_ms
                  << "; deferring remaining anchor reconciliation to next frame.\n";
    }
}

void Assets::sync_dev_controls_for_frame(const Input& input) {
    if (dev_controls_ && dev_controls_->is_enabled()) {
        sync_dev_controls_current_room(current_room_);
        dev_controls_->update(input);
        dev_controls_->update_ui(input);
    }
}

void Assets::refresh_filtered_active_assets_if_needed() {
    bool needs_filtered_active_refresh = needs_filtered_active_refresh_;
    const bool dev_controls_enabled = dev_controls_ && dev_controls_->is_enabled();
    std::uint64_t dev_filter_version = 0;
    if (dev_controls_enabled) {
        dev_filter_version = dev_controls_->other_settings_state_version();
        if (!last_dev_controls_enabled_ || dev_filter_version != last_dev_filter_state_version_) {
            needs_filtered_active_refresh = true;
        }
    } else if (last_dev_controls_enabled_) {
        needs_filtered_active_refresh = true;
    }
    last_dev_controls_enabled_ = dev_controls_enabled;
    last_dev_filter_state_version_ = dev_controls_enabled ? dev_filter_version : 0;

    if (needs_filtered_active_refresh) {
        needs_filtered_active_refresh_ = false;
        update_filtered_active_assets();
        if (dev_controls_enabled) {
            dev_controls_->set_active_assets(filtered_active_assets, dev_active_state_version_);
            sync_dev_controls_current_room(current_room_);
        }
    }
}

void Assets::render_runtime_frame() {
    bool should_render = !suppress_render_ && opengl_renderer_ != nullptr;
    if (should_render && startup_runtime_safety_active(frame_id_)) {
        const std::uint32_t skip_frames = startup_skip_render_frames();
        if (frame_id_ <= skip_frames) {
            should_render = false;
        } else {
            const std::uint32_t every_n = startup_render_every_n_frames();
            if (every_n > 1 && (frame_id_ % every_n) != 0) {
                should_render = false;
            }
        }
    }

    auto render_guard_log = [this](const std::string& message) {
        if (frame_id_ <= 8) {
            vibble::log::info(message);
        } else {
            vibble::log::debug(message);
        }
    };
    if (should_render) {
        SDL_Texture* ui_overlay = prepare_runtime_ui_overlay_texture();
        render_guard_log("[RenderGuard] before renderer submission frame=" + std::to_string(frame_id_) +
                         " path=opengl ui_overlay=" + (ui_overlay ? std::string("true") : std::string("false")) +
                         " active=" + std::to_string(active_assets.size()) +
                         " live_dynamic_assets=" + std::to_string(live_dynamic_asset_keys_.size()));

        std::string frame_error;
        if (!opengl_renderer_->render_frame(frame_error, ui_overlay)) {
            render_diagnostics::set_renderer_runtime_info("opengl", "failed", "fatal");
            render_diagnostics::set_submit_result(false);
            const RenderFrameStats& stats = render_diagnostics::current_frame_stats();
            const std::string reason = frame_error.empty() ? std::string("Unknown OpenGL frame failure.") : frame_error;
            vibble::log::error("[Assets] OpenGL runtime frame failed: reason='" + reason +
                               "' floor_packet_count=" + std::to_string(stats.floor_packet_count) +
                               " xy_sprite_packet_count=" + std::to_string(stats.xy_sprite_packet_count) +
                               " draw_call_count=" + std::to_string(stats.draw_call_count) +
                               " skipped_textures=" + std::to_string(stats.skipped_texture_count) +
                               " failed_texture_names='" + stats.failed_texture_names + "'" +
                               " submit_succeeded=" + (stats.submit_succeeded ? std::string("true") : "false"));
            throw std::runtime_error("[Assets] Fatal OpenGL runtime failure: " + reason);
        }
        render_guard_log("[RenderGuard] after renderer submission frame=" + std::to_string(frame_id_) +
                         " path=opengl");
    }
}

void Assets::finalize_dev_frame_state() {
    last_camera_state_version_for_dev_ = camera_.camera_state_version();
    last_dev_active_state_version_snapshot_ = dev_active_state_version_;
    dev_frame_initialized_ = true;
}

void Assets::update(const Input& input)
{
    const std::uint64_t now_counter = SDL_GetPerformanceCounter();
    const bool should_step_frame = should_step_dev_frame(input);

    ++frame_id_;
    reset_frame_rebuild_stage();

    static bool startup_safety_logged = false;
    if (!startup_safety_logged && startup_runtime_safety_active(frame_id_)) {
        startup_safety_logged = true;
        vibble::log::info("[Assets] Startup runtime safety enabled for " +
                          std::to_string(startup_runtime_safety_frames()) +
                          " frame(s).");
    }

    if (!should_step_frame) {
        run_idle_frame_pipeline(input);
        last_frame_dt_seconds_ = 0.0f;
        last_frame_counter_    = now_counter;
        return;
    }

    float dt = 1.0f / 60.0f;
    if (last_frame_counter_ != 0 && perf_counter_frequency_ > 0.0) {
        const double elapsed = static_cast<double>(now_counter - last_frame_counter_) / perf_counter_frequency_;
        if (std::isfinite(elapsed) && elapsed > 0.0) {
            dt = static_cast<float>(std::clamp(elapsed, 0.0, 0.25));
        }
    }
    last_frame_counter_    = now_counter;
    last_frame_dt_seconds_ = dt;

    const bool ctrl_down = input.isScancodeDown(SDL_SCANCODE_LCTRL) || input.isScancodeDown(SDL_SCANCODE_RCTRL);
    const bool shift_down = input.isScancodeDown(SDL_SCANCODE_LSHIFT) || input.isScancodeDown(SDL_SCANCODE_RSHIFT);
    if (ctrl_down && input.wasScancodePressed(SDL_SCANCODE_B)) {
        asset_boundary_box_display_enabled_ = !asset_boundary_box_display_enabled_;
        std::cout << "[Assets] Asset boundary box display "
                  << (asset_boundary_box_display_enabled_ ? "enabled" : "disabled") << " (Ctrl+B).\n";
    }

    if (ctrl_down && input.wasScancodePressed(SDL_SCANCODE_T) && task_editor_) {
        if (task_editor_->is_open()) {
            task_editor_->close();
        } else {
            task_editor_->open();
        }
        std::cout << "[Assets] Task editor "
                  << (task_editor_->is_open() ? "opened" : "closed") << " (Ctrl+T).\n";
    }

    if (ctrl_down && input.wasScancodePressed(SDL_SCANCODE_S)) {
        screenshot_capture_pending_ = true;
        std::cout << "[Assets] Screenshot requested (Ctrl+S).\n";
    }

    if (ctrl_down && shift_down && input.wasScancodePressed(SDL_SCANCODE_P)) {
        const bool enable = !anchor_point_debug_enabled_;
        anchor_point_debug_enabled_ = enable;
        std::cout << "[Assets] Anchor point overlay "
                  << (enable ? "enabled" : "disabled") << " (Ctrl+Shift+P).\n";
    }

    if (task_editor_) {
        task_editor_->update();
    }

    auto elapsed_ms = [this](Uint64 begin, Uint64 end) -> double {
        if (end <= begin || perf_counter_frequency_ <= 0.0) {
            return 0.0;
        }
        return static_cast<double>(end - begin) * 1000.0 / perf_counter_frequency_;
    };
    const bool startup_active = startup_runtime_safety_active(frame_id_);
    const double warn_ms = startup_stage_warning_ms();

    bool room_changed = false;
    bool player_moved = false;
    const Uint64 world_begin = SDL_GetPerformanceCounter();
    run_world_update_stage(input, room_changed, player_moved);
    const Uint64 world_end = SDL_GetPerformanceCounter();

    // Stage: visibility/traversal refresh.
    const Uint64 visibility_begin = SDL_GetPerformanceCounter();
    run_visibility_build_stage();
    const Uint64 visibility_end = SDL_GetPerformanceCounter();

    // Stage: runtime effects (with optional one-time post-runtime traversal refresh).
    const Uint64 runtime_begin = SDL_GetPerformanceCounter();
    run_runtime_effects_stage();
    const Uint64 runtime_end = SDL_GetPerformanceCounter();

    // Stage: render + UI refresh.
    const Uint64 render_begin = SDL_GetPerformanceCounter();
    refresh_filtered_active_assets_if_needed();
    render_runtime_frame();
    const Uint64 render_end = SDL_GetPerformanceCounter();

    if (startup_active) {
        const double world_ms = elapsed_ms(world_begin, world_end);
        const double visibility_ms = elapsed_ms(visibility_begin, visibility_end);
        const double runtime_ms = elapsed_ms(runtime_begin, runtime_end);
        const double render_ms = elapsed_ms(render_begin, render_end);
        if (world_ms >= warn_ms || visibility_ms >= warn_ms ||
            runtime_ms >= warn_ms || render_ms >= warn_ms) {
            vibble::log::warn("[Assets] Slow startup frame " + std::to_string(frame_id_) +
                              "ms world=" + std::to_string(world_ms) +
                              " visibility=" + std::to_string(visibility_ms) +
                              " runtime=" + std::to_string(runtime_ms) +
                              " render=" + std::to_string(render_ms));
        }
    }

    finalize_dev_frame_state();
}

void Assets::refresh_visible_asset_scaling_only() {
    if (player && player->info && asset_matches_focus_filter(player)) {
        player->update_scale_values();
    }

    rebuild_non_player_update_buffer_if_needed();

    for (Asset* asset : non_player_update_buffer_) {
        if (!asset || !asset->info || !asset_matches_focus_filter(asset)) {
            continue;
        }
        asset->update_scale_values();
    }
}

void Assets::rebuild_non_player_update_buffer_if_needed() {
    if (!non_player_update_buffer_dirty_.load(std::memory_order_acquire)) {
        return;
    }

    constexpr std::size_t kMaxBufferEntries = 250000;
    const auto& active_traversal = camera_.visible_traversal_entries();

    non_player_update_buffer_.clear();
    non_player_update_buffer_.reserve(
        std::min<std::size_t>(kMaxBufferEntries, active_traversal.size()));

    if (active_traversal.size() > kMaxBufferEntries) {
        std::cerr << "[Assets] Non-player buffer traversal exceeded cap ("
                  << active_traversal.size() << "); truncating to cap\n";
    }

    ++non_player_update_visit_epoch_;
    if (non_player_update_visit_epoch_ == 0) {
        non_player_update_visit_epoch_ = 1;
        for (auto& [asset, state] : runtime_traversal_state_) {
            (void)asset;
            state.non_player_update_visit_epoch = 0;
        }
    }
    const std::uint32_t current_visit_epoch = non_player_update_visit_epoch_;

    for (const ActiveTraversalEntry& entry : active_traversal) {
        Asset* asset = entry.asset;
        if (!asset || asset == player || asset->dead) {
            continue;
        }

        RuntimeTraversalState& state = runtime_traversal_state_[asset];
        if (state.non_player_update_visit_epoch == current_visit_epoch) {
            continue;
        }

        state.non_player_update_visit_epoch = current_visit_epoch;
        non_player_update_buffer_.push_back(asset);
        if (non_player_update_buffer_.size() >= kMaxBufferEntries) {
            break;
        }
    }

    non_player_update_buffer_dirty_.store(false, std::memory_order_release);
}

void Assets::invalidate_max_asset_dimensions() {
    max_asset_dimensions_dirty_ = true;
    asset_dimension_update_queue_.clear();
    asset_dimension_update_lookup_.clear();
}

void Assets::queue_asset_dimension_update(Asset* asset) {
    if (!asset) {
        return;
    }
    if (asset_dimension_update_lookup_.insert(asset).second) {
        asset_dimension_update_queue_.push_back(asset);
    }
}

void Assets::remove_asset_dimension_cache(Asset* asset) {
    if (!asset) {
        return;
    }
    asset_dimension_update_lookup_.erase(asset);
    asset_dimension_update_queue_.erase(
        std::remove(asset_dimension_update_queue_.begin(),
                    asset_dimension_update_queue_.end(),
                    asset),
        asset_dimension_update_queue_.end());
    auto it = asset_dimension_cache_.find(asset);
    if (it == asset_dimension_cache_.end()) {
        return;
    }
    const bool held_max_width = (max_asset_width_holder_ == asset);
    const bool held_max_height = (max_asset_height_holder_ == asset);
    asset_dimension_cache_.erase(it);
    if (held_max_width || held_max_height) {
        max_asset_dimensions_dirty_ = true;
    }
}

bool Assets::compute_asset_dimension_cache(const Asset* asset,
                                           float camera_scale,
                                           AssetDimensionCache& out) const {
    if (!asset || !asset->info) {
        return false;
    }
    if (asset->info->tillable) {
        return false;
    }
    float scale_factor = 1.0f;
    if (std::isfinite(asset->info->scale_factor) && asset->info->scale_factor > 0.0f) {
        scale_factor = asset->info->scale_factor;
    }
    const float width =
        static_cast<float>(std::max(1, asset->info->original_canvas_width)) * scale_factor * camera_scale;
    const float height =
        static_cast<float>(std::max(1, asset->info->original_canvas_height)) * scale_factor * camera_scale;
    out.width = width;
    out.height = height;
    return width > 0.0f && height > 0.0f;
}

void Assets::finalize_max_asset_dimensions(float max_width, float max_height) {
    if (max_width <= 0.0f) {
        max_width = static_cast<float>(screen_width);
    }
    if (max_height <= 0.0f) {
        max_height = static_cast<float>(screen_height);
    }
    max_asset_width_world_  = max_width;
    max_asset_height_world_ = max_height;

    float frustum_padding = std::max(max_asset_width_world_, max_asset_height_world_);
    if (dev_mode) {
        const float dev_cull_margin = devmode::camera_prefs::load_extra_cull_margin(0.0f);
        if (std::isfinite(dev_cull_margin) && dev_cull_margin > 0.0f) {
            frustum_padding = std::max(frustum_padding, dev_cull_margin);
        }
    }
    camera_.set_frustum_padding_world(frustum_padding);
}

void Assets::rebuild_asset_dimension_cache(float camera_scale) {
    max_asset_dimensions_dirty_ = false;
    asset_dimension_cache_.clear();
    asset_dimension_update_queue_.clear();
    asset_dimension_update_lookup_.clear();
    max_asset_width_holder_ = nullptr;
    max_asset_height_holder_ = nullptr;

    float max_width = 0.0f;
    float max_height = 0.0f;
    for (Asset* asset : all) {
        if (!asset) {
            continue;
        }
        AssetDimensionCache cache;
        if (!compute_asset_dimension_cache(asset, camera_scale, cache)) {
            continue;
        }
        asset_dimension_cache_.emplace(asset, cache);
        if (!max_asset_width_holder_ || cache.width >= max_width) {
            max_width = cache.width;
            max_asset_width_holder_ = asset;
        }
        if (!max_asset_height_holder_ || cache.height >= max_height) {
            max_height = cache.height;
            max_asset_height_holder_ = asset;
        }
    }

    cached_height_level_ = camera_scale;
    finalize_max_asset_dimensions(max_width, max_height);
}

void Assets::update_max_asset_dimensions() {
    const float camera_scale = 1.0f;
    bool height_changed = cached_height_level_ <= 0.0f;
    if (!height_changed && cached_height_level_ > 0.0f) {
        const float delta = std::fabs(camera_scale - cached_height_level_) / std::max(cached_height_level_, 0.0001f);
        height_changed = delta > 0.05f;
    }
    if (height_changed) {
        max_asset_dimensions_dirty_ = true;
    }

    if (max_asset_dimensions_dirty_) {
        rebuild_asset_dimension_cache(camera_scale);
        return;
    }

    if (asset_dimension_update_queue_.empty()) {
        return;
    }

    bool max_changed = false;
    bool requires_full_scan = false;
    for (Asset* asset : asset_dimension_update_queue_) {
        if (!asset) {
            continue;
        }
        auto it = asset_dimension_cache_.find(asset);
        const bool had_cache = it != asset_dimension_cache_.end();
        const float old_width = had_cache ? it->second.width : 0.0f;
        const float old_height = had_cache ? it->second.height : 0.0f;

        AssetDimensionCache updated;
        if (!compute_asset_dimension_cache(asset, camera_scale, updated)) {
            if (had_cache) {
                const bool was_width_holder = (max_asset_width_holder_ == asset);
                const bool was_height_holder = (max_asset_height_holder_ == asset);
                asset_dimension_cache_.erase(it);
                if (was_width_holder || was_height_holder) {
                    requires_full_scan = true;
                }
            }
            continue;
        }

        if (had_cache) {
            if (max_asset_width_holder_ == asset && updated.width < old_width) {
                requires_full_scan = true;
            }
            if (max_asset_height_holder_ == asset && updated.height < old_height) {
                requires_full_scan = true;
            }
        }

        if (requires_full_scan) {
            break;
        }

        asset_dimension_cache_[asset] = updated;
        if (!max_asset_width_holder_ || updated.width >= max_asset_width_world_) {
            max_asset_width_world_ = updated.width;
            max_asset_width_holder_ = asset;
            max_changed = true;
        }
        if (!max_asset_height_holder_ || updated.height >= max_asset_height_world_) {
            max_asset_height_world_ = updated.height;
            max_asset_height_holder_ = asset;
            max_changed = true;
        }
    }

    asset_dimension_update_queue_.clear();
    asset_dimension_update_lookup_.clear();

    if (requires_full_scan) {
        rebuild_asset_dimension_cache(camera_scale);
        return;
    }

    if (max_changed) {
        finalize_max_asset_dimensions(max_asset_width_world_, max_asset_height_world_);
    }
}

world::GridBounds Assets::screen_world_rect() const {
    const Area view = camera_.get_camera_area();
    auto [minx, miny, maxx, maxy] = view.get_bounds();
    const std::int64_t width64 = static_cast<std::int64_t>(maxx) - static_cast<std::int64_t>(minx);
    const std::int64_t height64 = static_cast<std::int64_t>(maxy) - static_cast<std::int64_t>(miny);
    const std::int64_t max_width_from_min =
        static_cast<std::int64_t>(std::numeric_limits<int>::max()) - static_cast<std::int64_t>(minx) + 1;
    const std::int64_t max_height_from_min =
        static_cast<std::int64_t>(std::numeric_limits<int>::max()) - static_cast<std::int64_t>(miny) + 1;
    const int width = static_cast<int>(std::clamp<std::int64_t>(
        std::max<std::int64_t>(0, width64),
        0,
        std::clamp<std::int64_t>(max_width_from_min, 0, static_cast<std::int64_t>(std::numeric_limits<int>::max()))));
    const int height = static_cast<int>(std::clamp<std::int64_t>(
        std::max<std::int64_t>(0, height64),
        0,
        std::clamp<std::int64_t>(max_height_from_min, 0, static_cast<std::int64_t>(std::numeric_limits<int>::max()))));
    return world::GridBounds::from_xywh(minx, miny, width, height, 0, world_grid_.default_resolution_layer());
}

world::GridBounds Assets::runtime_work_bounds_from_render_bounds(const world::GridBounds& render_bounds) {
    const auto clamp_i64_to_int = [](std::int64_t value) {
        return static_cast<int>(std::clamp<std::int64_t>(
            value,
            static_cast<std::int64_t>(std::numeric_limits<int>::min()),
            static_cast<std::int64_t>(std::numeric_limits<int>::max())));
    };

    const int render_min_x = std::min(render_bounds.min.world_x(), render_bounds.max.world_x());
    const int render_max_x = std::max(render_bounds.min.world_x(), render_bounds.max.world_x());
    const int render_min_z = std::min(render_bounds.min.world_z(), render_bounds.max.world_z());
    const int render_max_z = std::max(render_bounds.min.world_z(), render_bounds.max.world_z());
    if (render_min_x > render_max_x || render_min_z > render_max_z) {
        return render_bounds;
    }

    const auto& settings = camera_.get_settings();
    const int despawn_margin = std::max(0, live_dynamic_despawn_margin_world_px_);
    const int depth_bound = static_cast<int>(std::lround(std::max(
        static_cast<double>(settings.dynamic_renderer_depth_efficiency_depth),
        static_cast<double>(settings.max_cull_depth))));
    const int unclamped_radius = depth_bound + despawn_margin;
    constexpr int kMinRuntimeWorkRadiusWorldPx = 1200;
    constexpr int kMaxRuntimeWorkRadiusWorldPx = 48000;
    const int work_radius = std::clamp(unclamped_radius, kMinRuntimeWorkRadiusWorldPx, kMaxRuntimeWorkRadiusWorldPx);

    const SDL_Point camera_center = camera_.get_screen_center();
    const int clamp_min_x = clamp_i64_to_int(static_cast<std::int64_t>(camera_center.x) - work_radius);
    const int clamp_max_x = clamp_i64_to_int(static_cast<std::int64_t>(camera_center.x) + work_radius);
    const int clamp_min_z = clamp_i64_to_int(static_cast<std::int64_t>(camera_center.y) - work_radius);
    const int clamp_max_z = clamp_i64_to_int(static_cast<std::int64_t>(camera_center.y) + work_radius);

    const int bounded_min_x = std::max(render_min_x, clamp_min_x);
    const int bounded_max_x = std::min(render_max_x, clamp_max_x);
    const int bounded_min_z = std::max(render_min_z, clamp_min_z);
    const int bounded_max_z = std::min(render_max_z, clamp_max_z);

    if (bounded_min_x <= bounded_max_x && bounded_min_z <= bounded_max_z) {
        if ((bounded_min_x != render_min_x || bounded_max_x != render_max_x ||
             bounded_min_z != render_min_z || bounded_max_z != render_max_z) &&
            last_work_bounds_clamp_warning_frame_ != frame_id_) {
            last_work_bounds_clamp_warning_frame_ = frame_id_;
            vibble::log::warn("[DynamicWorkBounds] Clamped runtime bounds. frame=" +
                              std::to_string(frame_id_) +
                              " render_min=(" + std::to_string(render_min_x) + "," + std::to_string(render_min_z) + ")" +
                              " render_max=(" + std::to_string(render_max_x) + "," + std::to_string(render_max_z) + ")" +
                              " clamped_min=(" + std::to_string(bounded_min_x) + "," + std::to_string(bounded_min_z) + ")" +
                              " clamped_max=(" + std::to_string(bounded_max_x) + "," + std::to_string(bounded_max_z) + ")" +
                              " radius=" + std::to_string(work_radius));
        }
        return world::GridBounds::from_min_max(
            world::GridPoint::make_virtual(bounded_min_x,
                                           render_bounds.min.world_y(),
                                           bounded_min_z,
                                           render_bounds.min.resolution_layer()),
            world::GridPoint::make_virtual(bounded_max_x,
                                           render_bounds.max.world_y(),
                                           bounded_max_z,
                                           render_bounds.max.resolution_layer()));
    }

    return world::GridBounds::from_xywh(clamp_min_x,
                                        clamp_min_z,
                                        std::max(1, work_radius * 2 + 1),
                                        std::max(1, work_radius * 2 + 1),
                                        render_bounds.min.world_y(),
                                        render_bounds.min.resolution_layer());
}

bool Assets::should_run_live_dynamic_sync_for_bounds(const world::GridBounds& work_bounds,
                                                     bool allow_live_dynamic_sync) {
    if (!allow_live_dynamic_sync) {
        return false;
    }

    const int center_x = static_cast<int>((
        static_cast<std::int64_t>(work_bounds.min.world_x()) +
        static_cast<std::int64_t>(work_bounds.max.world_x())) / 2);
    const int center_z = static_cast<int>((
        static_cast<std::int64_t>(work_bounds.min.world_z()) +
        static_cast<std::int64_t>(work_bounds.max.world_z())) / 2);

    bool should_sync = force_live_dynamic_sync_next_rebuild_;
    if (last_live_dynamic_sync_frame_ == std::numeric_limits<std::uint32_t>::max()) {
        should_sync = true;
    }

    const auto abs_i64 = [](std::int64_t value) -> std::int64_t {
        return value < 0 ? -value : value;
    };
    const std::int64_t dx =
        abs_i64(static_cast<std::int64_t>(center_x) - static_cast<std::int64_t>(last_live_dynamic_sync_center_world_x_));
    const std::int64_t dz =
        abs_i64(static_cast<std::int64_t>(center_z) - static_cast<std::int64_t>(last_live_dynamic_sync_center_world_z_));
    const std::int64_t movement_threshold = std::max<std::int64_t>(1, live_dynamic_sync_min_camera_delta_world_px_);
    if (dx >= movement_threshold || dz >= movement_threshold) {
        should_sync = true;
    }

    if (last_live_dynamic_sync_frame_ != std::numeric_limits<std::uint32_t>::max()) {
        const std::uint32_t cadence = std::max<std::uint32_t>(1, live_dynamic_sync_min_interval_frames_);
        if ((frame_id_ - last_live_dynamic_sync_frame_) >= cadence) {
            should_sync = true;
        }
    }

    if (!should_sync) {
        return false;
    }

    last_live_dynamic_sync_center_world_x_ = center_x;
    last_live_dynamic_sync_center_world_z_ = center_z;
    last_live_dynamic_sync_frame_ = frame_id_;
    force_live_dynamic_sync_next_rebuild_ = false;
    return true;
}

void Assets::sync_live_dynamic_assets_to_render_bounds(const world::GridBounds& render_bounds) {
    const auto clamp_i64_to_int = [](std::int64_t value) {
        return static_cast<int>(std::clamp<std::int64_t>(
            value,
            static_cast<std::int64_t>(std::numeric_limits<int>::min()),
            static_cast<std::int64_t>(std::numeric_limits<int>::max())));
    };
    const auto in_world_bounds = [](int world_x,
                                    int world_z,
                                    int min_world_x,
                                    int max_world_x,
                                    int min_world_z,
                                    int max_world_z) {
        return world_x >= min_world_x &&
               world_x <= max_world_x &&
               world_z >= min_world_z &&
               world_z <= max_world_z;
    };
    const auto inclusive_count_u64 = [](std::int64_t min_value, std::int64_t max_value) {
        if (max_value < min_value) {
            return std::uint64_t{0};
        }
        return static_cast<std::uint64_t>(max_value - min_value) + std::uint64_t{1};
    };
    const auto sat_mul_u64 = [](std::uint64_t a, std::uint64_t b) {
        if (a == 0 || b == 0) {
            return std::uint64_t{0};
        }
        if (a > std::numeric_limits<std::uint64_t>::max() / b) {
            return std::numeric_limits<std::uint64_t>::max();
        }
        return a * b;
    };
    const auto sat_add_u64 = [](std::uint64_t a, std::uint64_t b) {
        if (a > std::numeric_limits<std::uint64_t>::max() - b) {
            return std::numeric_limits<std::uint64_t>::max();
        }
        return a + b;
    };

    const int render_min_world_x = std::min(render_bounds.min.world_x(), render_bounds.max.world_x());
    const int render_max_world_x = std::max(render_bounds.min.world_x(), render_bounds.max.world_x());
    const int render_min_world_z = std::min(render_bounds.min.world_z(), render_bounds.max.world_z());
    const int render_max_world_z = std::max(render_bounds.min.world_z(), render_bounds.max.world_z());
    if (render_min_world_x > render_max_world_x || render_min_world_z > render_max_world_z) {
        clear_live_dynamic_assets();
        return;
    }

    const int preload_margin = std::max(0, live_dynamic_preload_margin_world_px_);
    const int despawn_margin = std::max(preload_margin, live_dynamic_despawn_margin_world_px_);
    const world::GridBounds spawn_bounds = render_bounds.expanded(preload_margin);
    const world::GridBounds keep_bounds = render_bounds.expanded(despawn_margin);

    const int spawn_min_world_x = std::min(spawn_bounds.min.world_x(), spawn_bounds.max.world_x());
    const int spawn_max_world_x = std::max(spawn_bounds.min.world_x(), spawn_bounds.max.world_x());
    const int spawn_min_world_z = std::min(spawn_bounds.min.world_z(), spawn_bounds.max.world_z());
    const int spawn_max_world_z = std::max(spawn_bounds.min.world_z(), spawn_bounds.max.world_z());
    const int keep_min_world_x = std::min(keep_bounds.min.world_x(), keep_bounds.max.world_x());
    const int keep_max_world_x = std::max(keep_bounds.min.world_x(), keep_bounds.max.world_x());
    const int keep_min_world_z = std::min(keep_bounds.min.world_z(), keep_bounds.max.world_z());
    const int keep_max_world_z = std::max(keep_bounds.min.world_z(), keep_bounds.max.world_z());

    std::vector<Asset*> assets_to_delete;
    assets_to_delete.reserve(live_dynamic_asset_keys_.size());
    for (const auto& [asset, _] : live_dynamic_asset_keys_) {
        if (!asset || asset->dead ||
            !in_world_bounds(asset->world_x(),
                             asset->world_z(),
                             keep_min_world_x,
                             keep_max_world_x,
                             keep_min_world_z,
                             keep_max_world_z)) {
            assets_to_delete.push_back(asset);
        }
    }
    (void)delete_live_dynamic_assets_now(assets_to_delete);

    const std::size_t scan_cell_cap =
        std::max<std::size_t>(1, max_live_dynamic_scan_cells_per_selector_per_frame_);
    const std::size_t new_spawn_cap = max_live_dynamic_new_spawns_per_frame_;
    const std::size_t total_spawn_cap = max_total_live_dynamic_assets_;
    std::size_t new_spawns_this_frame = 0;
    std::size_t throttled_selector_count = 0;
    bool scan_cap_guard_hit = false;
    bool frame_spawn_cap_guard_hit = false;
    bool total_spawn_cap_guard_hit = false;

    auto jittered_world_point = [&](const LiveDynamicSelector& selector,
                                    const LiveDynamicPointKey& key,
                                    SDL_Point base_point) {
        if (selector.jitter_px <= 0) {
            return base_point;
        }
        std::uint64_t seed = std::hash<std::string>{}(map_id_);
        seed = mix_u64(seed, std::hash<std::string>{}(selector.spawn_id));
        seed = mix_u64(seed, static_cast<std::uint64_t>(static_cast<std::uint32_t>(key.grid_x)));
        seed = mix_u64(seed, static_cast<std::uint64_t>(static_cast<std::uint32_t>(key.grid_z)));
        seed = mix_u64(seed, static_cast<std::uint64_t>(static_cast<int>(key.mode)));
        const double u0 = live_dynamic_unit_interval(seed);
        const double u1 = live_dynamic_unit_interval(mix_u64(seed, 0x9e3779b97f4a7c15ULL));
        const int jitter = selector.jitter_px;
        SDL_Point jittered = base_point;
        jittered.x = static_cast<int>(std::clamp<std::int64_t>(
            static_cast<std::int64_t>(base_point.x) +
                static_cast<int>(std::lround((u0 * 2.0 - 1.0) * static_cast<double>(jitter))),
            static_cast<std::int64_t>(std::numeric_limits<int>::min()),
            static_cast<std::int64_t>(std::numeric_limits<int>::max())));
        jittered.y = static_cast<int>(std::clamp<std::int64_t>(
            static_cast<std::int64_t>(base_point.y) +
                static_cast<int>(std::lround((u1 * 2.0 - 1.0) * static_cast<double>(jitter))),
            static_cast<std::int64_t>(std::numeric_limits<int>::min()),
            static_cast<std::int64_t>(std::numeric_limits<int>::max())));
        return jittered;
    };

    auto pick_candidate = [&](const LiveDynamicSelector& selector,
                              const LiveDynamicPointKey& key) -> const LiveDynamicCandidate* {
        double total_weight = 0.0;
        for (const LiveDynamicCandidate& candidate : selector.candidates) {
            if (candidate.weight > 0.0 && std::isfinite(candidate.weight)) {
                total_weight += candidate.weight;
            }
        }
        if (total_weight <= 0.0) {
            return nullptr;
        }

        std::uint64_t hash = std::hash<std::string>{}(map_id_);
        hash = mix_u64(hash, std::hash<std::string>{}(key.spawn_id));
        hash = mix_u64(hash, static_cast<std::uint64_t>(key.grid_resolution));
        hash = mix_u64(hash, static_cast<std::uint32_t>(key.grid_x));
        hash = mix_u64(hash, static_cast<std::uint32_t>(key.grid_z));
        hash = mix_u64(hash, static_cast<std::uint64_t>(key.mode));
        const double roll = live_dynamic_unit_interval(hash) * total_weight;

        double cumulative = 0.0;
        const LiveDynamicCandidate* fallback = nullptr;
        for (const LiveDynamicCandidate& candidate : selector.candidates) {
            if (candidate.weight <= 0.0 || !std::isfinite(candidate.weight)) {
                continue;
            }
            fallback = &candidate;
            cumulative += candidate.weight;
            if (roll < cumulative) {
                return &candidate;
            }
        }
        return fallback;
    };

    auto for_each_live_dynamic_area = [&](const Room* room, const auto& fn) {
        if (!room) {
            return;
        }
        if (room->room_area) {
            fn(room->room_area.get());
        }
        for (const auto& named_area : room->areas) {
            if (live_dynamic_named_area_is_trail(named_area)) {
                fn(named_area.area.get());
            }
        }
    };

    auto room_contains_live_dynamic_area = [&](const Room* room, SDL_Point point) {
        bool contains = false;
        for_each_live_dynamic_area(room, [&](const Area* area) {
            if (!contains && area && area->contains_point(point)) {
                contains = true;
            }
        });
        return contains;
    };

    auto room_for_point = [&](SDL_Point point, bool require_inherited, bool& inside_any_room) -> Room* {
        inside_any_room = false;
        for (Room* room : rooms()) {
            if (!room || !room_contains_live_dynamic_area(room, point)) {
                continue;
            }
            inside_any_room = true;
            if (!require_inherited || room->inherits_map_assets()) {
                return room;
            }
        }
        return nullptr;
    };

    const SDL_Point boundary_center_world = camera_.get_screen_center();
    auto point_in_boundary_area = [&](SDL_Point point, bool inside_any_room) {
        if (inside_any_room || map_radius_world_ <= 0) {
            return false;
        }
        const std::int64_t dx = static_cast<std::int64_t>(point.x) - static_cast<std::int64_t>(boundary_center_world.x);
        const std::int64_t dz = static_cast<std::int64_t>(point.y) - static_cast<std::int64_t>(boundary_center_world.y);
        const std::int64_t radius = static_cast<std::int64_t>(map_radius_world_);
        return dx * dx + dz * dz <= radius * radius;
    };

    auto point_occupied = [&](SDL_Point world_point, int resolution) {
        const world::GridPoint grid_point = world::GridPoint::make_virtual(
            world_point.x,
            0,
            world_point.y,
            resolution);
        const world::GridKey key = world_grid_.grid_key_from_world(grid_point, resolution);
        const world::GridPoint* point = world_grid_.find_grid_point(key);
        return point && !point->assets_here().empty();
    };

    auto spawn_live_asset = [&](const LiveDynamicSelector& selector,
                                const LiveDynamicPointKey& key,
                                SDL_Point world_point,
                                const LiveDynamicCandidate& candidate,
                                const std::string& owner_name) {
        if (!candidate.info || candidate.asset_name.empty()) {
            return false;
        }
        Area spawn_area(owner_name.empty() ? std::string("live_dynamic") : owner_name, selector.grid_resolution);
        auto asset = std::make_unique<Asset>(
            candidate.info,
            spawn_area,
            world_point,
            0,
            std::string("live_dynamic:") + key.spawn_id +
                ":" + std::to_string(key.grid_resolution) +
                ":" + std::to_string(key.grid_x) +
                ":" + std::to_string(key.grid_z),
            std::string("live_dynamic"),
            selector.grid_resolution);
        Asset* raw = asset.get();
        raw->set_assets(this);
        raw->set_camera(&camera_);
        raw->set_owning_room_name(owner_name);
        raw->set_dynamic_spawned_asset(true);
        raw->finalize_setup();
        const int initialized_world_y = raw->world_y();
        raw->set_provisional_grid_point(world_point.x, initialized_world_y, world_point.y, selector.grid_resolution);
        raw = world_grid_.create_asset_at_point(std::move(asset), world_point.y, selector.grid_resolution);
        if (!raw) {
            return false;
        }
        if (world::GridPoint* registered_point = world_grid_.point_for_asset(raw)) {
            raw->cache_grid_residency(*registered_point);
        }
        all.push_back(raw);
        live_dynamic_asset_keys_.emplace(raw, key);
        live_dynamic_spawned_keys_.insert(key);
        queue_asset_dimension_update(raw);
        mark_anchor_basis_dirty(raw);
        mark_collision_context_dirty();
        mark_non_player_update_buffer_dirty();
        return true;
    };

    auto sync_selector = [&](const LiveDynamicSelector& selector) -> bool {
        struct ScanRegion {
            std::int64_t min_x = 0;
            std::int64_t max_x = -1;
            std::int64_t min_z = 0;
            std::int64_t max_z = -1;
        };

        if (selector.candidates.empty() || selector.grid_resolution <= 0) {
            return true;
        }
        const int resolution = vibble::grid::clamp_resolution(selector.grid_resolution);
        if (resolution < 0 || resolution > vibble::grid::kMaxResolution) {
            return true;
        }

        const int jitter_margin = std::max(0, selector.jitter_px);
        const world::GridBounds scan_bounds = spawn_bounds.expanded(jitter_margin);
        const SDL_Point min_index = vibble::grid::global_grid().world_to_index(
            SDL_Point{scan_bounds.min.world_x(), scan_bounds.min.world_z()}, resolution);
        const SDL_Point max_index = vibble::grid::global_grid().world_to_index(
            SDL_Point{scan_bounds.max.world_x(), scan_bounds.max.world_z()}, resolution);
        const std::int64_t scan_min_x64 = clamp_i64_to_int(
            std::min<std::int64_t>(min_index.x, max_index.x) - std::int64_t{1});
        const std::int64_t scan_max_x64 = clamp_i64_to_int(
            std::max<std::int64_t>(min_index.x, max_index.x) + std::int64_t{1});
        const std::int64_t scan_min_z64 = clamp_i64_to_int(
            std::min<std::int64_t>(min_index.y, max_index.y) - std::int64_t{1});
        const std::int64_t scan_max_z64 = clamp_i64_to_int(
            std::max<std::int64_t>(min_index.y, max_index.y) + std::int64_t{1});
        if (scan_min_x64 > scan_max_x64 || scan_min_z64 > scan_max_z64) {
            return true;
        }

        const LiveDynamicSelectorStateKey selector_state_key{
            selector.mode,
            resolution,
            selector.spawn_id};
        const auto prior_state_it = live_dynamic_selector_scan_state_.find(selector_state_key);
        const bool have_prior_state =
            prior_state_it != live_dynamic_selector_scan_state_.end() &&
            prior_state_it->second.valid;

        const auto selector_key_matches = [&](const LiveDynamicPointKey& key) {
            return key.mode == selector.mode &&
                   key.grid_resolution == resolution &&
                   key.spawn_id == selector.spawn_id;
        };
        for (auto it = live_dynamic_null_keys_.begin(); it != live_dynamic_null_keys_.end();) {
            if (selector_key_matches(*it) &&
                (it->grid_x < scan_min_x64 || it->grid_x > scan_max_x64 ||
                 it->grid_z < scan_min_z64 || it->grid_z > scan_max_z64)) {
                it = live_dynamic_null_keys_.erase(it);
            } else {
                ++it;
            }
        }

        const auto append_region = [](std::vector<ScanRegion>& regions,
                                      std::int64_t min_x,
                                      std::int64_t max_x,
                                      std::int64_t min_z,
                                      std::int64_t max_z) {
            if (min_x > max_x || min_z > max_z) {
                return;
            }
            regions.push_back(ScanRegion{min_x, max_x, min_z, max_z});
        };

        std::vector<ScanRegion> scan_regions;
        if (!have_prior_state) {
            scan_regions.push_back(ScanRegion{scan_min_x64, scan_max_x64, scan_min_z64, scan_max_z64});
        } else {
            const LiveDynamicSelectorScanState& prior = prior_state_it->second;
            append_region(scan_regions,
                          scan_min_x64,
                          scan_max_x64,
                          scan_min_z64,
                          std::min(scan_max_z64, static_cast<std::int64_t>(prior.min_z) - 1));
            append_region(scan_regions,
                          scan_min_x64,
                          scan_max_x64,
                          std::max(scan_min_z64, static_cast<std::int64_t>(prior.max_z) + 1),
                          scan_max_z64);

            const std::int64_t overlap_min_z =
                std::max(scan_min_z64, static_cast<std::int64_t>(prior.min_z));
            const std::int64_t overlap_max_z =
                std::min(scan_max_z64, static_cast<std::int64_t>(prior.max_z));
            if (overlap_min_z <= overlap_max_z) {
                append_region(scan_regions,
                              scan_min_x64,
                              std::min(scan_max_x64, static_cast<std::int64_t>(prior.min_x) - 1),
                              overlap_min_z,
                              overlap_max_z);
                append_region(scan_regions,
                              std::max(scan_min_x64, static_cast<std::int64_t>(prior.max_x) + 1),
                              scan_max_x64,
                              overlap_min_z,
                              overlap_max_z);
            }
        }

        if (scan_regions.empty()) {
            live_dynamic_selector_scan_state_[selector_state_key] =
                LiveDynamicSelectorScanState{true,
                                             static_cast<int>(scan_min_x64),
                                             static_cast<int>(scan_max_x64),
                                             static_cast<int>(scan_min_z64),
                                             static_cast<int>(scan_max_z64)};
            return true;
        }

        std::uint64_t total_scan_cells = 0;
        for (const ScanRegion& region : scan_regions) {
            const std::uint64_t region_cells_x = inclusive_count_u64(region.min_x, region.max_x);
            const std::uint64_t region_cells_z = inclusive_count_u64(region.min_z, region.max_z);
            total_scan_cells = sat_add_u64(total_scan_cells, sat_mul_u64(region_cells_x, region_cells_z));
        }

        std::int64_t scan_stride = 1;
        if (total_scan_cells > static_cast<std::uint64_t>(scan_cell_cap)) {
            ++throttled_selector_count;
            const long double ratio =
                static_cast<long double>(total_scan_cells) /
                static_cast<long double>(scan_cell_cap);
            scan_stride = std::max<std::int64_t>(2, static_cast<std::int64_t>(std::ceil(std::sqrt(ratio))));
            const auto sampled_cells_for_stride = [&](std::int64_t stride) {
                std::uint64_t sampled_total = 0;
                const std::uint64_t stride_u64 = static_cast<std::uint64_t>(std::max<std::int64_t>(1, stride));
                for (const ScanRegion& region : scan_regions) {
                    const std::uint64_t region_cells_x = inclusive_count_u64(region.min_x, region.max_x);
                    const std::uint64_t region_cells_z = inclusive_count_u64(region.min_z, region.max_z);
                    const std::uint64_t sampled_x =
                        (region_cells_x + stride_u64 - std::uint64_t{1}) / stride_u64;
                    const std::uint64_t sampled_z =
                        (region_cells_z + stride_u64 - std::uint64_t{1}) / stride_u64;
                    sampled_total = sat_add_u64(sampled_total, sat_mul_u64(sampled_x, sampled_z));
                    if (sampled_total >= static_cast<std::uint64_t>(scan_cell_cap)) {
                        break;
                    }
                }
                return sampled_total;
            };
            while (scan_stride < static_cast<std::int64_t>(std::numeric_limits<int>::max())) {
                if (sampled_cells_for_stride(scan_stride) <= static_cast<std::uint64_t>(scan_cell_cap)) {
                    break;
                }
                ++scan_stride;
            }
        }

        std::size_t scanned_cells = 0;
        for (const ScanRegion& region : scan_regions) {
            bool should_break = false;
            for (std::int64_t ix = region.min_x; ix <= region.max_x; ix += scan_stride) {
                for (std::int64_t iz = region.min_z; iz <= region.max_z; iz += scan_stride) {
                    if (scanned_cells >= scan_cell_cap) {
                        scan_cap_guard_hit = true;
                        should_break = true;
                        break;
                    }
                    ++scanned_cells;

                    const LiveDynamicPointKey key{
                        selector.mode,
                        resolution,
                        static_cast<int>(ix),
                        static_cast<int>(iz),
                        selector.spawn_id};
                    if (live_dynamic_spawned_keys_.find(key) != live_dynamic_spawned_keys_.end() ||
                        live_dynamic_null_keys_.find(key) != live_dynamic_null_keys_.end()) {
                        continue;
                    }

                    const SDL_Point grid_vertex_world = vibble::grid::global_grid().index_to_world(
                        static_cast<int>(ix),
                        static_cast<int>(iz),
                        resolution);
                    const SDL_Point world_point = jittered_world_point(selector, key, grid_vertex_world);
                    if (!in_world_bounds(world_point.x,
                                         world_point.y,
                                         spawn_min_world_x,
                                         spawn_max_world_x,
                                         spawn_min_world_z,
                                         spawn_max_world_z)) {
                        continue;
                    }

                    if (total_spawn_cap > 0 && live_dynamic_asset_keys_.size() >= total_spawn_cap) {
                        total_spawn_cap_guard_hit = true;
                        return false;
                    }
                    if (new_spawn_cap > 0 && new_spawns_this_frame >= new_spawn_cap) {
                        frame_spawn_cap_guard_hit = true;
                        return false;
                    }

                    bool inside_any_room = false;
                    Room* owner = nullptr;
                    if (selector.mode == LiveDynamicMode::InheritedMap) {
                        owner = room_for_point(world_point, true, inside_any_room);
                        if (!owner) {
                            continue;
                        }
                    } else {
                        (void)room_for_point(world_point, false, inside_any_room);
                        if (!point_in_boundary_area(world_point, inside_any_room)) {
                            continue;
                        }
                    }

                    if (point_occupied(world_point, resolution)) {
                        continue;
                    }

                    const LiveDynamicCandidate* candidate = pick_candidate(selector, key);
                    if (!candidate || candidate->is_null) {
                        live_dynamic_null_keys_.insert(key);
                        continue;
                    }
                    const std::string owner_name = owner ? owner->room_name : map_id_;
                    if (spawn_live_asset(selector, key, world_point, *candidate, owner_name)) {
                        ++new_spawns_this_frame;
                    }
                }
                if (should_break) {
                    break;
                }
            }
        }

        live_dynamic_selector_scan_state_[selector_state_key] =
            LiveDynamicSelectorScanState{true,
                                         static_cast<int>(scan_min_x64),
                                         static_cast<int>(scan_max_x64),
                                         static_cast<int>(scan_min_z64),
                                         static_cast<int>(scan_max_z64)};
        return true;
    };

    bool continue_sync = true;
    for (const LiveDynamicSelector& selector : live_dynamic_boundary_selectors_) {
        if (!sync_selector(selector)) {
            continue_sync = false;
            break;
        }
    }
    if (continue_sync) {
        for (const LiveDynamicSelector& selector : live_dynamic_inherited_selectors_) {
            if (!sync_selector(selector)) {
                continue_sync = false;
                break;
            }
        }
    }

    if ((throttled_selector_count > 0 ||
         scan_cap_guard_hit ||
         frame_spawn_cap_guard_hit ||
         total_spawn_cap_guard_hit) &&
        last_live_dynamic_guard_warning_frame_ != frame_id_) {
        last_live_dynamic_guard_warning_frame_ = frame_id_;
        vibble::log::warn("[LiveDynamicSpawn] Guard engaged. frame=" +
                          std::to_string(frame_id_) +
                          " throttled_selectors=" + std::to_string(throttled_selector_count) +
                          " scan_cap_hit=" + (scan_cap_guard_hit ? std::string("1") : std::string("0")) +
                          " frame_spawn_cap_hit=" + (frame_spawn_cap_guard_hit ? std::string("1") : std::string("0")) +
                          " total_spawn_cap_hit=" + (total_spawn_cap_guard_hit ? std::string("1") : std::string("0")) +
                          " new_spawns=" + std::to_string(new_spawns_this_frame) +
                          " live_dynamic_assets=" + std::to_string(live_dynamic_asset_keys_.size()) +
                          " scan_cap=" + std::to_string(scan_cell_cap) +
                          " frame_spawn_cap=" + std::to_string(new_spawn_cap) +
                          " total_spawn_cap=" + std::to_string(total_spawn_cap));
    }
}

void Assets::test_sync_live_dynamic_assets_for_bounds(const world::GridBounds& bounds) {
    sync_live_dynamic_assets_to_render_bounds(bounds);
}

int Assets::audio_effect_max_distance_world() const {
    const_cast<Assets*>(this)->update_max_asset_dimensions();
    const float horizontal_padding = std::max(0.0f, max_asset_width_world_ * 1.5f);
    const float bottom_padding     = std::max(0.0f, max_asset_height_world_);
    const float radius             = std::max(horizontal_padding, bottom_padding);
    return std::max(1, static_cast<int>(std::ceil(radius)));
}

void Assets::set_dev_mode(bool mode) {
    if (dev_mode == mode) {
        return;
    }

    if (mode) {
        bool enabled_ok = false;
        try {
            ensure_dev_controls();
            if (dev_controls_) {
                dev_controls_->set_enabled(true);
                enabled_ok = true;
            }
        } catch (const std::exception& ex) {
            std::cerr << "[Assets] Failed to enable Dev Mode: " << ex.what() << "\n";
            enabled_ok = false;
        } catch (...) {
            std::cerr << "[Assets] Failed to enable Dev Mode: unknown error\n";
            enabled_ok = false;
        }

        if (enabled_ok) {
            dev_mode = true;
            dev_frame_initialized_ = false;
            last_camera_state_version_for_dev_ = camera_.camera_state_version();
            last_dev_active_state_version_snapshot_ = dev_active_state_version_;
            show_dev_notice("Dev Mode enabled (Ctrl+D to toggle)", 2000);
        } else {

            dev_mode = false;
            clear_focus_filter();
            if (dev_controls_) {
                try { dev_controls_->set_enabled(false); } catch (...) {}
            }
            show_dev_notice("Dev Mode failed to enable", 2000);
        }
    } else {

        try {
            if (dev_controls_) {
                dev_controls_->set_enabled(false);
            }
        } catch (...) {
        }
        clear_focus_filter();
        dev_mode = false;
        dev_frame_initialized_ = false;
        last_camera_state_version_for_dev_ = camera_.camera_state_version();
        last_dev_active_state_version_snapshot_ = dev_active_state_version_;
        show_dev_notice("Dev Mode disabled", 1500);
    }

    apply_camera_runtime_settings();
    try {
        force_camera_view_refresh();
        if (vibble_scale_trace_enabled()) {
            vibble::log::debug(std::string("[ScaleTrace][ModeToggle] mode=") +
                               (dev_mode ? "dev" : "normal") +
                               " camera_state=" + std::to_string(camera_.camera_state_version()) +
                               " active_generation=" + std::to_string(active_assets_generation_));
        }
    } catch (const std::exception& ex) {
        std::cerr << "[Assets] force_camera_view_refresh failed after mode toggle: "
                  << ex.what() << "\n";
    } catch (...) {
        std::cerr << "[Assets] force_camera_view_refresh failed after mode toggle: unknown error\n";
    }
    // Fog logging removed
}

bool Assets::run_exit_save_sequence(const std::string& reason) {
    if (exit_save_sequence_ran_) {
        std::cout << "[Assets] Exit save sequence already executed; reusing result (reason='"
                  << reason << "', success=" << (exit_save_sequence_ok_ ? "true" : "false")
                  << ")\n";
        return exit_save_sequence_ok_;
    }

    exit_save_sequence_ran_ = true;

    ensure_dev_controls();

    bool ok = false;
    if (dev_controls_) {
        ok = dev_controls_->run_exit_save_sequence(reason);
    } else {
        std::cerr << "[Assets] EXIT SAVE FAILURE (reason='" << reason
                  << "'): DevControls unavailable.\n";
    }

    exit_save_sequence_ok_ = ok;

    if (exit_save_sequence_ok_) {
        std::cout << "[Assets] Exit save sequence complete (reason='" << reason << "')\n";
    } else {
        std::cerr << "[Assets] EXIT SAVE FAILURE (reason='" << reason << "')\n";
    }

    return exit_save_sequence_ok_;
}

void Assets::set_force_high_quality_rendering(bool enable) {
    if (force_high_quality_rendering_ == enable) {
        return;
    }
    force_high_quality_rendering_ = enable;
    apply_camera_runtime_settings();
}

void Assets::set_render_suppressed(bool suppressed) {
    if (suppress_render_ == suppressed) {
        return;
    }
    suppress_render_ = suppressed;

    if (opengl_renderer_) {
        if (suppressed) {

        } else {

            apply_camera_runtime_settings();
        }
    }
}

const std::vector<Asset*>& Assets::getActive() const {
    return active_assets;
}

const std::vector<Asset*>& Assets::getFilteredActiveAssets() const {
    return filtered_active_assets;
}

void Assets::initialize_active_assets(const world::GridPoint& ) {

    active_assets.clear();
    active_assets.reserve(all.size());
    filtered_active_assets.clear();
    filtered_active_asset_membership_.clear();
    filtered_active_assets_source_generation_ = 0;
    filtered_active_assets_filter_version_ = 0;

    mark_non_player_update_buffer_dirty();
    mark_collision_context_dirty();
    needs_filtered_active_refresh_ = true;
    ++active_assets_generation_;
    if (active_assets_generation_ == 0) {
        ++active_assets_generation_;
    }
}

void Assets::touch_dev_active_state_version() {
    ++dev_active_state_version_;
    if (dev_active_state_version_ == 0) {
        ++dev_active_state_version_;
    }
}

void Assets::mark_active_assets_dirty() {
    active_assets_dirty_.store(true, std::memory_order_release);
    needs_filtered_active_refresh_ = true;
    mark_collision_context_dirty();
    mark_anchor_bases_dirty_for_active_assets();
    note_frame_rebuild_request();
}

std::unique_ptr<Asset> Assets::extract_asset(Asset* asset) {
    if (!asset) {
        return nullptr;
    }
    const world::GridPoint detached_pos = world::GridPoint::make_virtual(
        asset->world_x(),
        asset->world_y(),
        asset->world_z(),
        asset->grid_resolution);

    std::unique_ptr<Asset> extracted = world_grid_.extract_asset(asset);
    if (!extracted) {
        return nullptr;
    }

    extracted->set_provisional_grid_point(detached_pos);
    extracted->cache_grid_residency(detached_pos);
    auto live_key_it = live_dynamic_asset_keys_.find(extracted.get());
    if (live_key_it != live_dynamic_asset_keys_.end()) {
        live_dynamic_spawned_keys_.erase(live_key_it->second);
        live_dynamic_asset_keys_.erase(live_key_it);
    }
    runtime_traversal_state_.erase(extracted.get());
    if (focus_filter_asset_) {
        mark_focus_filter_closure_dirty();
    }
    return extracted;
}

world::GridPoint Assets::resolve_floor_world_point(SDL_Point world_pos, int resolution_layer) const {
    const int max_layer = world_grid_.max_resolution_layers();
    const int requested_layer = (resolution_layer >= 0)
        ? resolution_layer
        : vibble::grid::clamp_resolution(map_grid_settings_.grid_resolution);
    const int layer = std::clamp(requested_layer, 0, max_layer);

    return world::GridPoint::make_virtual(world_pos.x, 0, world_pos.y, layer);
}

Asset* Assets::attach_asset(std::unique_ptr<Asset> asset, int world_z, int resolution_layer) {
    if (!asset) {
        return nullptr;
    }

    Asset* raw = asset.get();
    // Avoid double insertion in all vector.
    const bool already_tracked = std::find(all.begin(), all.end(), raw) != all.end();

    const int resolved_layer = (resolution_layer >= 0) ? resolution_layer : world_grid_.default_resolution_layer();
    SDL_Point source_world_xz{0, 0};
    if (world::GridPoint* point = raw->grid_point()) {
        source_world_xz = SDL_Point{point->world_x(), point->world_z()};
    } else if (raw->has_grid_residency_cache()) {
        const world::GridKey cached = raw->grid_residency_cache();
        source_world_xz = SDL_Point{cached.x, cached.z};
        raw->set_provisional_grid_point(cached.x, cached.y, cached.z, cached.layer);
    }
    const int resolved_z = (world_z != 0) ? world_z : resolve_floor_world_point(source_world_xz, resolved_layer).world_z();

    raw = world_grid_.attach_asset(std::move(asset), resolved_z, resolved_layer);
    if (!raw) {
        return nullptr;
    }
    if (world::GridPoint* attached_point = world_grid_.point_for_asset(raw)) {
        raw->cache_grid_residency(*attached_point);
    }

    if (!already_tracked) {
        all.push_back(raw);
    }

    queue_asset_dimension_update(raw);
    mark_grid_dirty();
    mark_active_assets_dirty();
    if (focus_filter_asset_) {
        mark_focus_filter_closure_dirty();
    }
    mark_anchor_basis_dirty(raw);
    mark_non_player_update_buffer_dirty();

    return raw;
}

Asset* Assets::spawn_asset(const std::string& name, SDL_Point world_pos) {

    std::shared_ptr<AssetInfo> info = library_.get(name);
    if (!info) {
        return nullptr;
    }

    std::string owning_room = map_id_;
    if (current_room_) {
        owning_room = current_room_->room_name;
    }

    Area spawn_area(owning_room,  0);

    int depth = 0;
    auto uptr = std::make_unique<Asset>(info,
                                        spawn_area,
                                        world_pos,
                                        depth,
                                        std::string{},
                                        std::string{},
                                        vibble::grid::clamp_resolution(map_grid_settings_.grid_resolution));
    Asset* raw = uptr.get();
    if (!raw) {
        return nullptr;
    }
    raw->set_assets(this);
    raw->set_camera(&camera_);
    raw->finalize_setup();

    const world::GridPoint floor_point = resolve_floor_world_point(world_pos);
    raw = world_grid_.create_asset_at_point(std::move(uptr), floor_point.world_z(), floor_point.resolution_layer());
    all.push_back(raw);
    if (world::GridPoint* registered_point = world_grid_.point_for_asset(raw)) {
        raw->cache_grid_residency(*registered_point);
    }
    std::cerr << "[PLACEMENT_DEBUG] assets_spawn_asset"
              << " name=\"" << name << "\""
              << " requested_world=(" << world_pos.x << "," << world_pos.y << ")"
              << " floor_world=(" << floor_point.world_x() << "," << floor_point.world_z() << ")"
              << " floor_layer=" << floor_point.resolution_layer()
              << " runtime_world=(" << (raw ? raw->world_x() : 0) << "," << (raw ? raw->world_z() : 0) << ")"
              << " runtime_layer=" << (raw && raw->grid_point() ? raw->grid_point()->resolution_layer() : -1)
              << "\n";

    queue_asset_dimension_update(raw);
    mark_grid_dirty();
    mark_active_assets_dirty();
    mark_anchor_basis_dirty(raw);
    mark_non_player_update_buffer_dirty();

    return raw;
}

std::unique_ptr<Asset> Assets::create_unattached_asset(const std::string& name, SDL_Point world_pos) {
    std::shared_ptr<AssetInfo> info = library_.get(name);
    if (!info) {
        return nullptr;
    }

    std::string owning_room = map_id_;
    if (current_room_) {
        owning_room = current_room_->room_name;
    }

    Area spawn_area(owning_room, 0);
    int depth = 0;
    auto uptr = std::make_unique<Asset>(info,
                                        spawn_area,
                                        world_pos,
                                        depth,
                                        std::string{},
                                        std::string{},
                                        vibble::grid::clamp_resolution(map_grid_settings_.grid_resolution));
    if (!uptr) {
        return nullptr;
    }

    uptr->set_assets(this);
    uptr->set_camera(&camera_);
    uptr->finalize_setup();
    return uptr;
}

void Assets::rebuild_from_grid_state() {
    ++frame_id_;
    rebuild_all_assets_from_grid();
    const SDL_Point center_px = camera_.get_screen_center();
    initialize_active_assets(world::GridPoint::make_virtual(center_px.x, 0, center_px.y, world_grid_.max_resolution_layers()));
    refresh_filtered_active_assets();
    mark_non_player_update_buffer_dirty();
}

const std::vector<Asset*>& Assets::get_selected_assets() const {
    static std::vector<Asset*> empty;
    if (dev_controls_ && dev_controls_->is_enabled()) {
        return dev_controls_->get_selected_assets();
    }
    return empty;
}

const std::vector<Asset*>& Assets::get_highlighted_assets() const {
    static std::vector<Asset*> empty;
    if (dev_controls_ && dev_controls_->is_enabled()) {
        return dev_controls_->get_highlighted_assets();
    }
    return empty;
}

Asset* Assets::get_hovered_asset() const {
    if (dev_controls_ && dev_controls_->is_enabled()) {
        return dev_controls_->get_hovered_asset();
    }
    return nullptr;
}

void Assets::track_asset_for_grid(Asset* asset) {
    (void)asset;

}

void Assets::reset_frame_rebuild_stage() {
    frame_rebuild_metrics_frame_ = frame_id_;
    frame_rebuild_request_count_ = 0;
    frame_rebuild_execution_count_ = 0;
    frame_rebuild_metrics_initialized_ = true;
}

void Assets::note_frame_rebuild_request() {
    if (!frame_rebuild_metrics_initialized_ || frame_rebuild_metrics_frame_ != frame_id_) {
        reset_frame_rebuild_stage();
    }
    ++frame_rebuild_request_count_;
}

bool Assets::run_frame_rebuild_stage() {
    if (!frame_rebuild_metrics_initialized_ || frame_rebuild_metrics_frame_ != frame_id_) {
        reset_frame_rebuild_stage();
    }

    if (frame_rebuild_execution_count_ > 0 &&
        frame_rebuild_execution_count_ >= frame_rebuild_request_count_) {
        return false;
    }

    const bool rebuilt = maybe_rebuild_world_grid();
    if (rebuilt) {
        ++frame_rebuild_execution_count_;
        if (frame_rebuild_request_count_ > 1 && frame_rebuild_execution_count_ == 1) {
            vibble::log::debug(std::string("[Assets] Coalesced ") +
                               std::to_string(frame_rebuild_request_count_) +
                               " rebuild requests into frame rebuild passes.");
        }
    }
    return rebuilt;
}

bool Assets::maybe_rebuild_world_grid() {
    if (frame_id_ == last_grid_rebuild_frame_ && !post_runtime_traversal_refresh_pending_) {
        return false;
    }

    const SDL_Point center_px = camera_.get_screen_center();
    const world::GridPoint current_center = world::GridPoint::make_virtual(
        center_px.x,
        0,
        center_px.y,
        world_grid_.max_resolution_layers());
    const double current_scale = camera_.get_scale();
    const double current_pitch = camera_.current_pitch_radians();
    const std::uint64_t current_projection_version = camera_.camera_state_version();
    constexpr double kCameraGridEpsilon = 1e-4;
    const bool active_dirty = active_assets_dirty_.load(std::memory_order_acquire) || pending_initial_rebuild_;
    const bool camera_changed =
        current_center.world_x() != last_camera_center_for_grid_.world_x() ||
        current_center.world_z() != last_camera_center_for_grid_.world_z() ||
        std::fabs(current_scale - last_camera_scale_for_grid_) > kCameraGridEpsilon ||
        std::fabs(current_pitch - last_camera_pitch_for_grid_) > kCameraGridEpsilon ||
        current_projection_version != last_camera_projection_state_version_for_grid_;

    const bool camera_dirty = camera_view_dirty_ || camera_changed || post_runtime_traversal_refresh_pending_;
    if (!grid_dirty_ && !camera_dirty && !active_dirty) {
        post_runtime_traversal_refresh_pending_ = false;
        return false;
    }

    if (active_dirty) {
        pending_initial_rebuild_ = false;
        initialize_active_assets(current_center);
        active_assets_dirty_.store(false, std::memory_order_release);
    }

    camera_view_dirty_ = camera_dirty;
    rebuild_world_grid_and_active_assets(current_center,
                                         current_scale,
                                         current_pitch,
                                         current_projection_version);
    post_runtime_traversal_refresh_pending_ = false;
    return true;
}

void Assets::rebuild_world_grid_and_active_assets(const world::GridPoint& current_center,
                                                  double current_scale,
                                                  double current_pitch,
                                                  std::uint64_t current_projection_version,
                                                  bool allow_live_dynamic_sync) {
    last_grid_rebuild_frame_ = frame_id_;
    camera_.recompute_current_view();
    const world::GridBounds render_bounds = screen_world_rect();
    const world::GridBounds work_bounds = runtime_work_bounds_from_render_bounds(render_bounds);
    if (should_run_live_dynamic_sync_for_bounds(work_bounds, allow_live_dynamic_sync)) {
        sync_live_dynamic_assets_to_render_bounds(work_bounds);
    }
    camera_.rebuild_grid(world_grid_,
                         last_frame_dt_seconds_,
                         frame_id_);
    world_grid_.update_active_chunks(work_bounds, 0);
    rebuild_active_from_screen_grid();
    mark_anchor_bases_dirty_for_active_assets();

    grid_dirty_ = false;
    camera_view_dirty_ = false;
    last_camera_center_for_grid_ = world::GridPoint::make_virtual(
        current_center.world_x(),
        current_center.world_y(),
        current_center.world_z(),
        current_center.resolution_layer());
    last_camera_scale_for_grid_ = current_scale;
    last_camera_pitch_for_grid_ = current_pitch;
    last_camera_projection_state_version_for_grid_ = current_projection_version;
}

void Assets::untrack_asset_for_grid(Asset* asset) {
    if (!asset) {
        return;
    }
    (void)world_grid_.remove_asset(asset);
}

void Assets::mark_grid_dirty() {
    grid_dirty_ = true;
    note_frame_rebuild_request();
}

void Assets::register_pending_static_assets() {
    pending_static_grid_registration_.clear();
}

void Assets::rebuild_all_assets_from_grid() {
    all.clear();
    runtime_traversal_state_.clear();
    frame_collision_entries_.clear();
    frame_collision_context_dirty_ = true;
    auto collected = world_grid_.all_assets();
    std::sort(collected.begin(), collected.end(),
              [](Asset* lhs, Asset* rhs) { return lhs < rhs; });
    all.reserve(collected.size());
    asset_dimension_cache_.clear();
    asset_dimension_update_queue_.clear();
    asset_dimension_update_lookup_.clear();
    max_asset_width_holder_ = nullptr;
    max_asset_height_holder_ = nullptr;
    float max_width = 0.0f;
    float max_height = 0.0f;
    const float camera_scale = 1.0f;
    for (Asset* a : collected) {
        if (a) {
            all.push_back(a);
            AssetDimensionCache cache;
            if (compute_asset_dimension_cache(a, camera_scale, cache)) {
                asset_dimension_cache_.emplace(a, cache);
                if (!max_asset_width_holder_ || cache.width >= max_width) {
                    max_width = cache.width;
                    max_asset_width_holder_ = a;
                }
                if (!max_asset_height_holder_ || cache.height >= max_height) {
                    max_height = cache.height;
                    max_asset_height_holder_ = a;
                }
            }
        }
    }
    cached_height_level_ = camera_scale;
    max_asset_dimensions_dirty_ = false;
    finalize_max_asset_dimensions(max_width, max_height);
    if (focus_filter_asset_) {
        mark_focus_filter_closure_dirty();
    }
}

bool Assets::asset_bounds_in_screen_space(const Asset* asset, SDL_FRect& out_rect) const {
    if (!asset || !asset->info) {
        return false;
    }
    float world_x = asset->smoothed_translation_x();
    float world_y = asset->smoothed_translation_y();
    if (dev_mode) {
        world_x = static_cast<float>(asset->world_x());
        world_y = static_cast<float>(asset->world_y());
    }

    float asset_scale = asset->smoothed_scale();
    if (!std::isfinite(asset_scale) || asset_scale <= 0.0f) {
        asset_scale = 1.0f;
    }

    SDL_FRect sprite_rect{0.0f, 0.0f, 0.0f, 0.0f};
    bool      have_sprite_rect = false;
    const int base_w_px = std::max(1, asset->info->original_canvas_width);
    const int base_h_px = std::max(1, asset->info->original_canvas_height);
    const float width = static_cast<float>(base_w_px) * asset_scale;
    const float height = static_cast<float>(base_h_px) * asset_scale;
    const float half_width = width * 0.5f;
    if (width > 0.0f && height > 0.0f) {
        SDL_FPoint top_left{};
        SDL_FPoint top_right{};
        SDL_FPoint bottom_left{};
        SDL_FPoint bottom_right{};
        const float base_z = static_cast<float>(asset->world_z());
        const bool projected =
            camera_.project_world_point(SDL_FPoint{world_x - half_width, world_y}, base_z + height, top_left) &&
            camera_.project_world_point(SDL_FPoint{world_x + half_width, world_y}, base_z + height, top_right) &&
            camera_.project_world_point(SDL_FPoint{world_x - half_width, world_y}, base_z, bottom_left) &&
            camera_.project_world_point(SDL_FPoint{world_x + half_width, world_y}, base_z, bottom_right);
        if (projected &&
            std::isfinite(top_left.x) && std::isfinite(top_left.y) &&
            std::isfinite(top_right.x) && std::isfinite(top_right.y) &&
            std::isfinite(bottom_left.x) && std::isfinite(bottom_left.y) &&
            std::isfinite(bottom_right.x) && std::isfinite(bottom_right.y)) {
            const float left = std::min(top_left.x, bottom_left.x);
            const float right = std::max(top_right.x, bottom_right.x);
            const float top = std::min(top_left.y, top_right.y);
            const float bottom = std::max(bottom_left.y, bottom_right.y);
            const float rect_w = right - left;
            const float rect_h = bottom - top;
            if (rect_w > 0.0f && rect_h > 0.0f && std::isfinite(rect_w) && std::isfinite(rect_h)) {
                sprite_rect = SDL_FRect{left, top, rect_w, rect_h};
                have_sprite_rect = true;
            }
        }
    }

    if (!have_sprite_rect) {
        return false;
    }

    SDL_FRect combined = sprite_rect;

    out_rect = combined;
    return true;
}

void Assets::schedule_removal(Asset* a) {
    if (!a) {
        return;
    }
    std::lock_guard<std::mutex> lock(removal_queue_mutex_);
    removal_queue.push_back(a);
}

std::vector<Asset*> Assets::collect_removal_closure(const std::vector<Asset*>& roots) const {
    std::unordered_set<Asset*> visited;
    std::vector<Asset*> ordered;

    const auto enqueue_recursive = [&](auto&& self, Asset* asset) -> void {
        if (!asset || visited.find(asset) != visited.end()) {
            return;
        }
        visited.insert(asset);
        ordered.push_back(asset);
        for (Asset* child : asset->children()) {
            self(self, child);
        }
    };

    visited.reserve(roots.size());
    for (Asset* asset : roots) {
        enqueue_recursive(enqueue_recursive, asset);
    }

    return ordered;
}

void Assets::WorldMutationBatch::mark_for_deletion(Asset* asset) {
    if (!asset || staged_lookup_.find(asset) != staged_lookup_.end()) {
        return;
    }
    staged_lookup_.insert(asset);
    staged_removals_.push_back(asset);
}

bool Assets::WorldMutationBatch::commit() {
    if (!owner_) {
        return false;
    }
    return owner_->apply_world_mutation_batch(*this);
}

Assets::WorldMutationBatch Assets::begin_world_mutation_batch() {
    return WorldMutationBatch(this);
}

bool Assets::apply_world_mutation_batch(WorldMutationBatch& batch) {
    if (!batch.has_mutations()) {
        return true;
    }

    if (batch.pre_commit_save_ && !batch.pre_commit_save_()) {
        return false;
    }

    if (dev_controls_ && dev_controls_->is_enabled()) {
        dev_controls_->set_world_mutation_in_progress(true);
    }

    for (Asset* asset : batch.staged_removals_) {
        if (!asset) {
            continue;
        }
        asset->dead = true;
        asset->active = false;
        schedule_removal(asset);
    }

    const bool removed_any = process_pending_removals();
    if (removed_any) {
        rebuild_from_grid_state();
        refresh_active_asset_lists();
    }

    if (dev_controls_ && dev_controls_->is_enabled()) {
        dev_controls_->set_world_mutation_in_progress(false);
    }
    return removed_any;
}

std::size_t Assets::delete_assets_runtime(const std::vector<Asset*>& assets_to_delete) {
    const std::vector<Asset*> ordered_removals = collect_removal_closure(assets_to_delete);
    if (ordered_removals.empty()) {
        return 0;
    }

    std::unordered_set<Asset*> unique_removals;
    unique_removals.reserve(ordered_removals.size());
    for (Asset* asset : ordered_removals) {
        if (asset) {
            unique_removals.insert(asset);
        }
    }
    if (unique_removals.empty()) {
        return 0;
    }

    for (Asset* asset : ordered_removals) {
        if (!asset || unique_removals.find(asset) == unique_removals.end()) {
            continue;
        }
        asset->notify_pre_delete();
    }

    for (Asset* asset : all) {
        if (!asset || unique_removals.find(asset) != unique_removals.end()) {
            continue;
        }
        for (Asset* removed : ordered_removals) {
            asset->remove_child(removed);
        }
    }

    for (Asset* asset : ordered_removals) {
        if (!asset || unique_removals.find(asset) == unique_removals.end()) {
            continue;
        }
        auto live_key_it = live_dynamic_asset_keys_.find(asset);
        if (live_key_it != live_dynamic_asset_keys_.end()) {
            live_dynamic_spawned_keys_.erase(live_key_it->second);
            live_dynamic_asset_keys_.erase(live_key_it);
        }
        if (asset == focus_filter_asset_) {
            clear_focus_filter();
        }
        remove_asset_dimension_cache(asset);
        runtime_traversal_state_.erase(asset);
        asset->clear_grid_residency_cache();
        (void)world_grid_.remove_asset(asset);
    }

    rebuild_all_assets_from_grid();
    active_assets.clear();
    filtered_active_assets.clear();
    moving_assets_for_grid_.clear();
    pending_static_grid_registration_.clear();
    runtime_traversal_state_.clear();
    if (focus_filter_asset_) {
        mark_focus_filter_closure_dirty();
    }
    mark_grid_dirty();
    mark_active_assets_dirty();
    mark_non_player_update_buffer_dirty();

    if (dev_controls_ && dev_controls_->is_enabled()) {
        dev_controls_->clear_selection();
    }

    if (max_asset_dimensions_dirty_) {
        asset_dimension_update_queue_.clear();
        asset_dimension_update_lookup_.clear();
    }

#ifndef NDEBUG
    {
        const auto survivors = world_grid_.all_assets();
        (void)survivors;
    }
#endif

    return unique_removals.size();
}

bool Assets::process_removals() {
    std::vector<Asset*> pending_removals;
    {
        std::lock_guard<std::mutex> lock(removal_queue_mutex_);
        if (removal_queue.empty()) {
            return false;
        }
        pending_removals.swap(removal_queue);
    }

    return delete_assets_runtime(pending_removals) > 0;
}

bool Assets::process_pending_removals() {
    return process_removals();
}

std::size_t Assets::delete_assets_for_spawn_group(const std::string& spawn_id) {
    if (spawn_id.empty()) {
        return 0;
    }

    auto batch = begin_world_mutation_batch();
    std::size_t count = 0;
    bool removed_tileable_asset = false;
    for (Asset* asset : all) {
        if (!asset || asset->dead || asset == player) {
            continue;
        }
        if (asset->spawn_id == spawn_id) {
            batch.mark_for_deletion(asset);
            removed_tileable_asset = removed_tileable_asset || (asset->info && asset->info->tillable);
            ++count;
        }
    }

    if (count == 0) {
        return 0;
    }

    if (!batch.commit()) {
        return 0;
    }

    if (removed_tileable_asset) {
        loader_tiles::build_grid_tiles(renderer(), world_grid_, map_grid_settings_, all);
    }
    return count;
}

std::size_t Assets::delete_assets_for_spawn_groups(const std::vector<std::string>& spawn_ids) {
    if (spawn_ids.empty()) {
        return 0;
    }

    std::vector<std::string> filtered_spawn_ids;
    filtered_spawn_ids.reserve(spawn_ids.size());
    for (const std::string& spawn_id : spawn_ids) {
        if (!spawn_id.empty()) {
            filtered_spawn_ids.push_back(spawn_id);
        }
    }
    if (filtered_spawn_ids.empty()) {
        return 0;
    }
    std::sort(filtered_spawn_ids.begin(), filtered_spawn_ids.end());
    filtered_spawn_ids.erase(std::unique(filtered_spawn_ids.begin(), filtered_spawn_ids.end()),
                             filtered_spawn_ids.end());

    auto batch = begin_world_mutation_batch();
    std::size_t count = 0;
    bool removed_tileable_asset = false;
    for (Asset* asset : all) {
        if (!asset || asset->dead || asset == player) {
            continue;
        }
        if (!std::binary_search(filtered_spawn_ids.begin(), filtered_spawn_ids.end(), asset->spawn_id)) {
            continue;
        }
        batch.mark_for_deletion(asset);
        removed_tileable_asset = removed_tileable_asset || (asset->info && asset->info->tillable);
        ++count;
    }

    if (count == 0) {
        return 0;
    }

    if (!batch.commit()) {
        return 0;
    }

    if (removed_tileable_asset) {
        loader_tiles::build_grid_tiles(renderer(), world_grid_, map_grid_settings_, all);
    }
    return count;
}

void Assets::destroy_runtime_ui_overlay_texture() {
    if (!runtime_ui_overlay_texture_) {
        return;
    }
    SDL_DestroyTexture(runtime_ui_overlay_texture_);
    runtime_ui_overlay_texture_ = nullptr;
    runtime_ui_overlay_width_ = 0;
    runtime_ui_overlay_height_ = 0;
}

SDL_Texture* Assets::prepare_runtime_ui_overlay_texture() {
    SDL_Renderer* active_renderer = renderer();
    if (!active_renderer || screen_width <= 0 || screen_height <= 0) {
        return nullptr;
    }

    if (runtime_ui_overlay_texture_ &&
        (runtime_ui_overlay_width_ != screen_width || runtime_ui_overlay_height_ != screen_height)) {
        destroy_runtime_ui_overlay_texture();
    }

    if (!runtime_ui_overlay_texture_) {
        runtime_ui_overlay_texture_ = SDL_CreateTexture(active_renderer,
                                                        SDL_PIXELFORMAT_RGBA32,
                                                        SDL_TEXTUREACCESS_TARGET,
                                                        screen_width,
                                                        screen_height);
        if (!runtime_ui_overlay_texture_) {
            vibble::log::warn("[Assets] Failed to create GPU UI overlay target: " + std::string(SDL_GetError()));
            return nullptr;
        }
        runtime_ui_overlay_width_ = screen_width;
        runtime_ui_overlay_height_ = screen_height;
        SDL_SetTextureBlendMode(runtime_ui_overlay_texture_, SDL_BLENDMODE_BLEND);
        SDL_SetTextureScaleMode(runtime_ui_overlay_texture_, SDL_SCALEMODE_NEAREST);
    }

    SDL_Texture* previous_target = SDL_GetRenderTarget(active_renderer);
    if (!SDL_SetRenderTarget(active_renderer, runtime_ui_overlay_texture_)) {
        vibble::log::warn("[Assets] Failed to validate GPU UI overlay target: " + std::string(SDL_GetError()));
        SDL_SetRenderTarget(active_renderer, previous_target);
        return nullptr;
    }
    SDL_SetRenderTarget(active_renderer, previous_target);

    render_overlays(active_renderer, runtime_ui_overlay_texture_);
    if (!SDL_FlushRenderer(active_renderer)) {
        vibble::log::warn("[Assets] SDL_FlushRenderer failed after UI overlay target render: " +
                          std::string(SDL_GetError()));
        return nullptr;
    }
    return runtime_ui_overlay_texture_;
}

void Assets::render_overlays(SDL_Renderer* renderer, SDL_Texture* overlay_target) {
    const Uint32 now = SDL_GetTicks();
    const bool rendering_to_overlay_target = renderer && overlay_target;
    SDL_Texture* previous_target = nullptr;

    if (renderer) {
        previous_target = SDL_GetRenderTarget(renderer);
        if (!SDL_SetRenderTarget(renderer, rendering_to_overlay_target ? overlay_target : nullptr)) {
            vibble::log::warn("[Assets] Failed to bind overlay render target: " + std::string(SDL_GetError()));
            if (rendering_to_overlay_target) {
                SDL_SetRenderTarget(renderer, previous_target);
            }
            return;
        }
        SDL_SetRenderViewport(renderer, nullptr);
        SDL_SetRenderClipRect(renderer, nullptr);
        if (rendering_to_overlay_target) {
            SDL_BlendMode previous_blend_mode = SDL_BLENDMODE_NONE;
            SDL_GetRenderDrawBlendMode(renderer, &previous_blend_mode);
            SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
            SDL_SetRenderDrawColor(renderer, 0, 0, 0, 0);
            SDL_RenderClear(renderer);
            SDL_SetRenderDrawBlendMode(renderer, previous_blend_mode);
        }
    }

    if (renderer && dev_controls_ && dev_controls_->is_enabled()) {
        dev_controls_->render_overlays(renderer);
    }

    if (task_editor_) {
        task_editor_->render(renderer);
    }

    popup_manager_.update(now);
    if (!renderer) {
        return;
    }

    if (!culled_debug_rects_.empty()) {
        SDL_BlendMode prev_mode = SDL_BLENDMODE_NONE;
        SDL_GetRenderDrawBlendMode(renderer, &prev_mode);
        SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(renderer, 255, 0, 0, 160);
        for (const SDL_Rect& r : culled_debug_rects_) {
            sdl_render::Rect(renderer, &r);
        }
        SDL_SetRenderDrawBlendMode(renderer, prev_mode);
    }

    if (asset_boundary_box_display_enabled_) {
        const std::vector<Asset*>& overlay_assets =
            (dev_controls_ && dev_controls_->is_enabled()) ? filtered_active_assets : active_assets;
        if (!overlay_assets.empty()) {
            SDL_BlendMode previous_mode = SDL_BLENDMODE_NONE;
            SDL_GetRenderDrawBlendMode(renderer, &previous_mode);
            SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
            SDL_SetRenderDrawColor(renderer, 0, 255, 180, 200);
            for (Asset* asset : overlay_assets) {
                if (!asset) {
                    continue;
                }
                SDL_FRect screen_rect;
                if (!asset_bounds_in_screen_space(asset, screen_rect)) {
                    continue;
                }
                SDL_Rect draw_rect{
                    static_cast<int>(std::floor(screen_rect.x)), static_cast<int>(std::floor(screen_rect.y)), static_cast<int>(std::ceil(screen_rect.w)), static_cast<int>(std::ceil(screen_rect.h)) };
                if (draw_rect.w <= 0 || draw_rect.h <= 0) {
                    continue;
                }
                sdl_render::Rect(renderer, &draw_rect);
            }
            SDL_SetRenderDrawBlendMode(renderer, previous_mode);
        }
    }

    popup_manager_.render(renderer, screen_width, screen_height, now);

    if (screenshot_create_task_button_active(now)) {
        render_screenshot_create_task_button(renderer, now);
    }

    if (screenshot_capture_pending_) {
        if (rendering_to_overlay_target) {
            latest_screenshot_relative_path_.clear();
            screenshot_create_task_start_ticks_ = 0;
            popup_manager_.show_toast("Screenshot unavailable during GPU composite", 2200);
        } else {
            std::string screenshot_relative_path;
            if (capture_screenshot_to_root(renderer, screenshot_relative_path)) {
                latest_screenshot_relative_path_ = std::move(screenshot_relative_path);
                screenshot_create_task_start_ticks_ = now;
                popup_manager_.show_toast("Screenshot saved", 2200);
            } else {
                latest_screenshot_relative_path_.clear();
                screenshot_create_task_start_ticks_ = 0;
                popup_manager_.show_toast("Screenshot failed", 2200);
            }
        }
        screenshot_capture_pending_ = false;
    }

    if (rendering_to_overlay_target) {
        SDL_SetRenderTarget(renderer, previous_target);
        SDL_SetRenderViewport(renderer, nullptr);
        SDL_SetRenderClipRect(renderer, nullptr);
    }
}

bool Assets::capture_screenshot_to_root(SDL_Renderer* renderer, std::string& out_relative_path) {
    out_relative_path.clear();
    if (!renderer) {
        return false;
    }

    const std::filesystem::path root = std::filesystem::absolute(std::filesystem::path(manifest::manifest_path()).parent_path());
    const std::filesystem::path screenshot_dir = root / "docs/screen_shots";
    std::error_code ec;
    std::filesystem::create_directories(screenshot_dir, ec);
    if (ec) {
        std::cerr << "[Assets] Failed to create screenshot directory: " << ec.message() << "\n";
        return false;
    }

    const auto now = std::chrono::system_clock::now();
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
    std::ostringstream stem;
    stem << "screenshot_" << ms;
    const std::filesystem::path png_path = screenshot_dir / (stem.str() + ".png");
    const std::filesystem::path bmp_path = screenshot_dir / (stem.str() + ".bmp");

    int out_w = 0;
    int out_h = 0;
    if (!SDL_GetCurrentRenderOutputSize(renderer, &out_w, &out_h)) {
        return false;
    }
    if (out_w <= 0 || out_h <= 0) {
        return false;
    }

    SDL_Rect capture_rect{0, 0, out_w, out_h};
    SDL_Surface* captured = SDL_RenderReadPixels(renderer, &capture_rect);
    if (!captured) {
        std::cerr << "[Assets] Failed to capture screenshot: " << SDL_GetError() << "\n";
        return false;
    }

    const int png_save_ok = IMG_SavePNG(captured, png_path.string().c_str());
    if (png_save_ok == 0) {
        SDL_DestroySurface(captured);
        out_relative_path = (std::filesystem::path("docs/screen_shots") / png_path.filename()).generic_string();
        return true;
    }

    const bool bmp_save_ok = SDL_SaveBMP(captured, bmp_path.string().c_str());
    SDL_DestroySurface(captured);
    if (bmp_save_ok) {
        out_relative_path = (std::filesystem::path("docs/screen_shots") / bmp_path.filename()).generic_string();
        return true;
    }

    std::cerr << "[Assets] Failed to save screenshot (PNG and BMP): " << SDL_GetError() << "\n";
    return false;
}

bool Assets::screenshot_create_task_button_active(Uint32 now_ticks) const {
    if (latest_screenshot_relative_path_.empty() || screenshot_create_task_start_ticks_ == 0) {
        return false;
    }
    if (task_editor_ && task_editor_->is_open()) {
        return false;
    }
    const Uint32 elapsed = now_ticks - screenshot_create_task_start_ticks_;
    return elapsed < kCreateTaskButtonLifetimeMs;
}

void Assets::render_screenshot_create_task_button(SDL_Renderer* renderer, Uint32 now_ticks) {
    if (!renderer || !screenshot_create_task_button_active(now_ticks)) {
        return;
    }

    int output_w = 0;
    int output_h = 0;
    if (!SDL_GetCurrentRenderOutputSize(renderer, &output_w, &output_h)) {
        return;
    }

    constexpr int kButtonW = 138;
    constexpr int kButtonH = 34;
    constexpr int kLeftMargin = 16;
    constexpr int kBottomSafePadding = 92;

    screenshot_create_task_button_rect_ = SDL_Rect{
        kLeftMargin,
        std::max(0, output_h - kBottomSafePadding - kButtonH),
        kButtonW,
        kButtonH
    };

    Uint8 alpha = 230;
    const Uint32 elapsed = now_ticks - screenshot_create_task_start_ticks_;
    if (elapsed > (kCreateTaskButtonLifetimeMs - kCreateTaskButtonFadeMs)) {
        const Uint32 fade_elapsed = elapsed - (kCreateTaskButtonLifetimeMs - kCreateTaskButtonFadeMs);
        const float t = static_cast<float>(fade_elapsed) / static_cast<float>(kCreateTaskButtonFadeMs);
        const float keep = std::clamp(1.0f - t, 0.0f, 1.0f);
        alpha = static_cast<Uint8>(std::lround(230.0f * keep));
    }

    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer, 42, 42, 48, alpha);
    sdl_render::FillRect(renderer, &screenshot_create_task_button_rect_);
    SDL_SetRenderDrawColor(renderer, 170, 170, 185, alpha);
    sdl_render::Rect(renderer, &screenshot_create_task_button_rect_);

    DMLabelStyle label_style = DMStyles::Label();
    label_style.color.a = alpha;
    const std::string label = "Create Task";
    SDL_Point size = DMFontCache::instance().measure_text(label_style, label);
    const int text_x = screenshot_create_task_button_rect_.x + (screenshot_create_task_button_rect_.w - size.x) / 2;
    const int text_y = screenshot_create_task_button_rect_.y + (screenshot_create_task_button_rect_.h - size.y) / 2;
    DMFontCache::instance().draw_text(renderer, label_style, label, text_x, text_y);
}

bool Assets::handle_screenshot_create_task_button_event(const SDL_Event& e, Uint32 now_ticks) {
    if (!screenshot_create_task_button_active(now_ticks)) {
        return false;
    }
    if (e.type != SDL_EVENT_MOUSE_BUTTON_UP || e.button.button != SDL_BUTTON_LEFT) {
        return false;
    }

    SDL_Point point{
        static_cast<int>(std::lround(e.button.x)),
        static_cast<int>(std::lround(e.button.y))
    };
    if (!SDL_PointInRect(&point, &screenshot_create_task_button_rect_)) {
        return false;
    }

    if (task_editor_) {
        task_editor_->open_with_new_task_attachment(latest_screenshot_relative_path_);
    }
    screenshot_create_task_start_ticks_ = 0;
    latest_screenshot_relative_path_.clear();
    return true;
}

SDL_Renderer* Assets::renderer() const {
    if (suppress_dev_renderer_) {
        return nullptr;
    }
    return renderer_;
}

std::optional<Asset::TilingInfo> Assets::compute_tiling_for_asset(const Asset* asset) const {
    if (!asset || !asset->info) {
        return std::nullopt;
    }
    if (!asset->info->tillable) {
        return std::nullopt;
    }
    if (asset->tiling_info() && asset->tiling_info()->is_valid()) {
        return asset->tiling_info();
    }

    const int raw_w = std::max(1, asset->info->original_canvas_width);
    const int raw_h = std::max(1, asset->info->original_canvas_height);
    double scale = 1.0;
    if (std::isfinite(asset->info->scale_factor) && asset->info->scale_factor > 0.0f) {
        scale = static_cast<double>(asset->info->scale_factor);
    }
    const int fallback_step = std::max(
        1,
        static_cast<int>(std::lround(static_cast<double>(std::max(raw_w, raw_h)) * scale)));
    const int step = resolve_tiled_asset_step_px(map_grid_settings_, fallback_step);

    const SDL_Point world_pos{ asset->world_x(), asset->world_z() };
    const int base_w = raw_w;
    const int base_h = raw_h;
    const int scaled_w = std::max(1, static_cast<int>(std::lround(static_cast<double>(base_w) * scale)));
    const int scaled_h = std::max(1, static_cast<int>(std::lround(static_cast<double>(base_h) * scale)));

    const int left   = world_pos.x - (scaled_w / 2);
    const int top    = world_pos.y - scaled_h;
    const int right  = left + scaled_w;
    const int bottom = world_pos.y;

    auto align_down = [](int value, int step_) {
        if (step_ <= 0) return value;
        const double scaled = std::floor(static_cast<double>(value) / static_cast<double>(step_));
        return static_cast<int>(scaled * static_cast<double>(step_));
};
    auto align_up = [](int value, int step_) {
        if (step_ <= 0) return value;
        const double scaled = std::ceil(static_cast<double>(value) / static_cast<double>(step_));
        return static_cast<int>(scaled * static_cast<double>(step_));
};

    const int origin_x = align_down(left, step);
    const int origin_y = align_down(top, step);
    const int limit_x  = align_up(right, step);
    const int limit_y  = align_up(bottom, step);

    Asset::TilingInfo tiling{};
    tiling.enabled    = true;
    tiling.tile_size  = SDL_Point{ step, step };
    tiling.grid_origin = SDL_Point{ origin_x, origin_y };
    tiling.anchor = SDL_Point{ align_down(world_pos.x, step) + step / 2,
                               align_down(world_pos.y, step) + step / 2 };

    const int coverage_w = std::max(step, limit_x - origin_x);
    const int coverage_h = std::max(step, limit_y - origin_y);
    tiling.coverage = SDL_Rect{ origin_x, origin_y, coverage_w, coverage_h };

    return tiling.is_valid() ? std::optional<Asset::TilingInfo>(tiling) : std::nullopt;
}

Asset* Assets::find_asset_by_name(const std::string& name) const {
    if (name.empty()) {
        return nullptr;
    }
    for (Asset* asset : active_assets) {
        if (asset && asset->info && asset->info->name == name) {
            return asset;
        }
    }
    for (Asset* asset : all) {
        if (asset && asset->info && asset->info->name == name) {
            return asset;
        }
    }
    return nullptr;
}

Asset* Assets::find_asset_by_stable_id(const std::string& id) const {
    if (id.empty()) {
        return nullptr;
    }
    auto matches_id = [&](Asset* asset) -> bool {
        if (!asset) {
            return false;
        }
        if (!asset->spawn_id.empty() && asset->spawn_id == id) {
            return true;
        }
        return asset->info && asset->info->name == id;
    };

    for (Asset* asset : active_assets) {
        if (matches_id(asset)) {
            return asset;
        }
    }
    for (Asset* asset : all) {
        if (matches_id(asset)) {
            return asset;
        }
    }
    return nullptr;
}

bool Assets::contains_asset(const Asset* asset) const {
    if (!asset) {
        return false;
    }

    if (std::find(all.begin(), all.end(), asset) != all.end()) {
        return true;
    }

    return false;
}

void Assets::apply_map_grid_settings(const MapGridSettings& settings, bool persist_json) {
    MapGridSettings sanitized = settings;
    sanitized.clamp();

    const bool resolution_changed = sanitized.grid_resolution != map_grid_settings_.grid_resolution;
    map_grid_settings_ = sanitized;

    if (persist_json) {
        nlohmann::json& section = map_info_json_["map_grid_settings"];
        sanitized.apply_to_json(section);
    }

    world_grid_.set_grid_resolution(std::max(0, sanitized.grid_resolution));

    if (resolution_changed) {
        for (Asset* asset : all) {
            if (!asset) {
                continue;
            }
            asset->clear_grid_residency_cache();
        }
    }

    for (Asset* asset : all) {
        if (!asset) {
            continue;
        }
        if (world::GridPoint* point = world_grid_.point_for_asset(asset)) {
            asset->cache_grid_residency(*point);
        }
    }

    if (resolution_changed) {
        update_max_asset_dimensions();
        camera_.recompute_current_view();
        world_grid_.update_active_chunks(runtime_work_bounds_from_render_bounds(screen_world_rect()), 0);
        mark_grid_dirty();
    }
}

void Assets::toggle_asset_library() {
    if (dev_controls_ && dev_controls_->is_enabled()) {
        dev_controls_->toggle_asset_library();
    }
}

void Assets::open_asset_library() {
    if (dev_controls_ && dev_controls_->is_enabled()) {
        dev_controls_->open_asset_library();
    }
}

void Assets::close_asset_library() {
    if (dev_controls_ && dev_controls_->is_enabled()) {
        dev_controls_->close_asset_library();
    }
}

bool Assets::is_asset_library_open() const {
    return dev_controls_ && dev_controls_->is_enabled() && dev_controls_->is_asset_library_open();
}

void Assets::toggle_room_config() {
    
}

void Assets::close_room_config() {
    
}

bool Assets::is_room_config_open() const {
    return false;
}

std::shared_ptr<AssetInfo> Assets::consume_selected_asset_from_library() {
    if (!dev_controls_ || !dev_controls_->is_enabled()) return nullptr;
    return dev_controls_->consume_selected_asset_from_library();
}

void Assets::open_asset_info_editor(const std::shared_ptr<AssetInfo>& info) {
    if (dev_controls_ && dev_controls_->is_enabled()) {
        dev_controls_->open_asset_info_editor(info);
    }
}

void Assets::open_asset_info_editor_for_asset(Asset* a) {
    if (dev_controls_ && dev_controls_->is_enabled()) {
        dev_controls_->open_asset_info_editor_for_asset(a);
    }
}

void Assets::finalize_asset_drag(Asset* a, const std::shared_ptr<AssetInfo>& info) {
    if (dev_controls_ && dev_controls_->is_enabled()) {
        dev_controls_->finalize_asset_drag(a, info);
    }
}

void Assets::close_asset_info_editor() {
    if (dev_controls_ && dev_controls_->is_enabled()) {
        dev_controls_->close_asset_info_editor();
    }
}

bool Assets::consume_escape_for_asset_editor_stack() {
    return dev_controls_ && dev_controls_->is_enabled() &&
           dev_controls_->consume_escape_for_asset_editor_stack();
}

bool Assets::is_asset_info_editor_open() const {
    return dev_controls_ && dev_controls_->is_enabled() && dev_controls_->is_asset_info_editor_open();
}

void Assets::clear_editor_selection() {
    if (dev_controls_ && dev_controls_->is_enabled()) {
        dev_controls_->clear_selection();
    }
}

void Assets::handle_sdl_event(const SDL_Event& e) {
    if (e.type == SDL_EVENT_KEY_DOWN && e.key.scancode == SDL_SCANCODE_S) {
        const bool ctrl_down = (e.key.mod & SDL_KMOD_CTRL) != 0;
        if (ctrl_down) {
            screenshot_capture_pending_ = true;
        }
    }

    const Uint32 now = SDL_GetTicks();
    if (handle_screenshot_create_task_button_event(e, now)) {
        if (input) {
            input->consumeEvent(e);
        }
        return;
    }

    if (task_editor_ && task_editor_->is_open()) {
        if (task_editor_->handle_event(e)) {

            if (input) {
                input->consumeEvent(e);
            }
            return;
        }
    }

    if (dev_controls_ && dev_controls_->is_enabled()) {
        dev_controls_->handle_sdl_event(e);
    }
}

void Assets::focus_camera_on_asset(Asset* a, double height_factor, int duration_steps) {
    if (dev_controls_ && dev_controls_->is_enabled()) {
        dev_controls_->focus_camera_on_asset(a, height_factor, duration_steps);
    }
}

void Assets::begin_frame_editor_session(Asset* asset,
                                        std::shared_ptr<animation_editor::AnimationDocument> document,
                                        std::shared_ptr<animation_editor::PreviewProvider> preview,
                                        const std::string& animation_id,
                                        FrameEditorLaunchMode launch_mode,
                                        std::function<void(const std::string&)> on_host_closed) {
    ensure_dev_controls();
    if (dev_controls_) {
        dev_controls_->begin_frame_editor_session(asset,
                                                  std::move(document),
                                                  std::move(preview),
                                                  animation_id,
                                                  launch_mode,
                                                  std::move(on_host_closed));
    }
}

devmode::core::ManifestStore* Assets::manifest_store() {
    if (dev_controls_) {
        auto& store = dev_controls_->manifest_store();
        return &store;
    }
    return nullptr;
}

const devmode::core::ManifestStore* Assets::manifest_store() const {
    return const_cast<Assets*>(this)->manifest_store();
}

void Assets::notify_spawn_group_config_changed(const nlohmann::json& entry) {
    rebuild_live_dynamic_selectors();
    mark_grid_dirty();
    if (dev_controls_ && dev_controls_->is_enabled()) {
        dev_controls_->notify_spawn_group_config_changed(entry);
    }
}

void Assets::notify_spawn_group_removed(const std::string& spawn_id) {
    rebuild_live_dynamic_selectors();
    mark_grid_dirty();
    if (dev_controls_ && dev_controls_->is_enabled()) {
        dev_controls_->notify_spawn_group_removed(spawn_id);
    }
}

void Assets::show_dev_notice(const std::string& message, Uint32 duration_ms) {
    popup_manager_.show_toast(message, duration_ms);
}

void Assets::notify_camera_activity(bool active) {
    const std::string room_label = current_room_ ? current_room_->room_name : std::string();
    popup_manager_.notify_camera_activity(room_label, active, SDL_GetTicks());
}

void Assets::set_editor_current_room(Room* room) {
    current_room_ = room;
    game_context_.mutable_map_graph().set_current_room(room);
    if (dev_controls_) {
        sync_dev_controls_current_room(room, true);
    }
    const std::string room_label = current_room_ ? current_room_->room_name : std::string();
    popup_manager_.notify_room_change(room_label, SDL_GetTicks());
}

void Assets::open_animation_editor_for_asset(const std::shared_ptr<AssetInfo>& info) {
    if (dev_controls_ && dev_controls_->is_enabled()) {
        dev_controls_->open_animation_editor_for_asset(info);
    }
}

void Assets::rebuild_active_from_screen_grid() {
    const std::uint32_t current_frame_id = frame_id_;
    if (current_frame_id == last_active_rebuild_frame_id_) {
        return;
    }
    const std::uint32_t previous_active_frame_id = last_active_rebuild_frame_id_;
    bool active_changed = false;
    std::vector<Asset*> previous_active = std::move(active_assets);

    active_assets.clear();

    const auto& visible_traversal = camera_.visible_traversal_entries();
    const bool traversal_empty = visible_traversal.empty();
    active_assets.reserve(visible_traversal.size());
    std::unordered_set<Asset*> seen_assets;
    seen_assets.reserve(visible_traversal.size() * 2);

    for (const WarpedScreenGrid::VisibleTraversalEntry& source_entry : visible_traversal) {
        Asset* asset = source_entry.asset;
        if (!asset || asset->dead) {
            continue;
        }
        if (!seen_assets.insert(asset).second) {
            continue;
        }

        const bool was_active = asset->last_active_frame_id == previous_active_frame_id;
        if (!was_active) {
            active_changed = true;
        }

        asset->last_active_frame_id = current_frame_id;
        asset->last_visible_frame_id = current_frame_id;
        asset->active = true;

        active_assets.push_back(asset);
        mark_anchor_basis_dirty(asset);
    }

    bool fail_open_applied = false;
    if (traversal_empty && active_assets.empty() && !previous_active.empty()) {
        vibble::log::warn("[Assets] Empty visibility traversal detected. frame=" +
                          std::to_string(current_frame_id) +
                          " previous_active=" + std::to_string(previous_active.size()) +
                          " fail_open_eligible=" + (visibility_fail_open_used_last_rebuild_ ? std::string("false") : std::string("true")) +
                          " camera_state=" + std::to_string(camera_.camera_state_version()) +
                          " depth_culled=" + std::to_string(camera_.last_depth_culled()) +
                          " nodes=" + std::to_string(camera_.last_nodes_visited()) +
                          " branches_skipped=" + std::to_string(camera_.last_branches_skipped()));
    }
    if (traversal_empty &&
        active_assets.empty() &&
        !previous_active.empty() &&
        !visibility_fail_open_used_last_rebuild_) {
        active_assets.reserve(previous_active.size());
        for (Asset* asset : previous_active) {
            if (!asset || asset->dead) {
                continue;
            }
            asset->last_active_frame_id = current_frame_id;
            asset->last_visible_frame_id = current_frame_id;
            asset->active = true;
            active_assets.push_back(asset);
            mark_anchor_basis_dirty(asset);
        }
        if (!active_assets.empty()) {
            ++visibility_fail_open_activation_count_;
            if (visibility_fail_open_activation_count_ == 0) {
                ++visibility_fail_open_activation_count_;
            }
            fail_open_applied = true;
            vibble::log::warn("[Assets] Visibility fail-open reused previous active set for one frame. frame=" +
                              std::to_string(current_frame_id) +
                              " reused=" + std::to_string(active_assets.size()) +
                              " activations=" + std::to_string(visibility_fail_open_activation_count_) +
                              " camera_state=" + std::to_string(camera_.camera_state_version()));
        }
    }

    // Deactivate assets no longer visible this frame.
    for (Asset* asset : previous_active) {
        if (!asset) {
            continue;
        }
        const bool still_active = asset->last_active_frame_id == current_frame_id;
        if (!still_active && asset->last_active_frame_id == previous_active_frame_id) {
            active_changed = true;
            asset->active = false;
            mark_anchor_basis_dirty(asset);
        }
    }

    active_assets_dirty_.store(false, std::memory_order_release);
    if (active_changed) {
        ++active_assets_generation_;
        if (active_assets_generation_ == 0) {
            ++active_assets_generation_;
        }
        mark_non_player_update_buffer_dirty();
        needs_filtered_active_refresh_ = true;
        mark_collision_context_dirty();
    }

    // Update scale values for ALL active assets after grid rebuild
    // to ensure scales reflect current perspective (fixes single-frame asset scaling)
    for (Asset* asset : active_assets) {
        if (!asset) {
            continue;
        }
        asset->update_scale_values();
    }

    if (!traversal_empty) {
        visibility_fail_open_used_last_rebuild_ = false;
    } else if (fail_open_applied) {
        visibility_fail_open_used_last_rebuild_ = true;
    }

    last_active_rebuild_frame_id_ = current_frame_id;
}

void Assets::touch_last_frame_counter() {
    last_frame_counter_ = SDL_GetPerformanceCounter();
}

bool Assets::should_step_dev_frame(const Input& input) const {
    if (!dev_mode) {
        return true;
    }

    // Explicit frame editor sessions should always be allowed to step.
    if (dev_controls_ && dev_controls_->is_frame_editor_session_active()) {
        return true;
    }

    if (!dev_frame_initialized_) {
        return true;
    }

    if (camera_.camera_state_version() != last_camera_state_version_for_dev_) {
        return true;
    }

    if (dev_active_state_version_ != last_dev_active_state_version_snapshot_) {
        return true;
    }

    if (camera_.is_height_animating() ||
        camera_settings_dirty_ ||
        grid_dirty_ ||
        needs_filtered_active_refresh_ ||
        active_assets_dirty_.load(std::memory_order_acquire)) {
        return true;
    }

    if (input.has_activity()) {
        return true;
    }

    if (has_pending_dev_work(movement_debug_enabled_)) {
        return true;
    }

    return false;
}

bool Assets::has_pending_dev_work(bool include_animation_plans) const {
    if (pending_initial_rebuild_) return true;
    if (popup_manager_.has_active_content()) return true;
    if (task_editor_ && task_editor_->is_open()) return true;
    if (screenshot_capture_pending_) return true;
    if (screenshot_create_task_button_active(SDL_GetTicks())) return true;

    if (!include_animation_plans) {
        return false;
    }

    // Check player for in-flight movement plan
    if (player && player->anim_runtime_ && player->anim_runtime_->has_active_plan()) return true;

    // Check all active non-player assets for in-flight movement plans
    for (const Asset* a : non_player_update_buffer_) {
        if (a && a->anim_runtime_ && a->anim_runtime_->has_active_plan()) return true;
    }
    return false;
}

bool Assets::is_frame_editor_target_active(const Asset* asset) const {
    if (!asset) {
        return false;
    }
    if (!dev_controls_ || !dev_controls_->is_frame_editor_session_active()) {
        return false;
    }
    const Asset* target = dev_controls_->frame_editor_target();
    return target && target == asset;
}

bool Assets::should_run_runtime_updates() const {
    if (!dev_mode) {
        return true;
    }
    if (movement_debug_enabled_ && movement_debug_visible_) {
        return true;
    }
    if (dev_controls_ && dev_controls_->is_frame_editor_session_active()) {
        return true;
    }
    return false;
}

bool Assets::should_render_runtime_lighting() const {
    if (camera_settings_panel_active_) {
        return true;
    }
    if (dev_controls_ && dev_controls_->is_runtime_light_editor_active()) {
        return true;
    }
    if (startup_runtime_safety_active(frame_id_)) {
        return false;
    }
    if (dev_mode) {
        return false;
    }
    return true;
}

void Assets::set_camera_settings_panel_active(bool active) {
    camera_settings_panel_active_ = active;
}

bool Assets::should_advance_animation_for(const Asset* asset) const {
    if (!asset) {
        return false;
    }

    if (asset->is_dynamic_spawned_asset()) {
        const auto& settings = camera_.get_settings();
        const double efficiency_depth =
            std::max(0.0, static_cast<double>(settings.dynamic_renderer_depth_efficiency_depth));
        if (efficiency_depth > 0.0) {
            const double depth_from_focus = dynamic_asset_camera_depth_from_focus_plane(camera_, *asset);
            if (depth_from_focus > efficiency_depth) {
                return false;
            }
        }
    }

    const bool frame_editor_session_active =
        dev_controls_ && dev_controls_->is_frame_editor_session_active();

    return runtime::dev_mode_policy::should_advance_animation_for_asset(
        dev_mode,
        should_run_runtime_updates(),
        frame_editor_session_active,
        is_frame_editor_target_active(asset));
}
