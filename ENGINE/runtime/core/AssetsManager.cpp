#include "AssetsManager.hpp"
#include "utils/sdl_render_conversions.hpp"

#include "utils/ranged_color.hpp"
#include "assets/initialize_assets.hpp"

#include "find_current_room.hpp"
#include "assets/Asset.hpp"
#include "assets/asset/asset_info.hpp"
#include "assets/asset/asset_utils.hpp"
#include "assets/asset/asset_types.hpp"
#include "animation/animation_runtime.hpp"
#include "audio/audio_engine.hpp"
#include "devtools/dev_controls.hpp"
#include "devtools/depth_cue_settings.hpp"
#include "animation/controllers/custom_controllers/anchor_bound_asset_helper.hpp"
#include "rendering/render/render.hpp"
#include "rendering/render/terrain_field.hpp"
#include "gameplay/world/chunk.hpp"
#include "gameplay/map_generation/room.hpp"
#include "gameplay/map_generation/map_layers_geometry.hpp"
#include "utils/area.hpp"
#include "utils/input.hpp"
#include "utils/range_util.hpp"
#include "utils/map_grid_settings.hpp"
#include "utils/quick_task_popup.hpp"
#include "utils/log.hpp"

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
#include <SDL3_ttf/SDL_ttf.h>

namespace {

constexpr int kQualityOptions[] = {100, 75, 50, 25, 10};
constexpr int kMinRenderQuality = kQualityOptions[sizeof(kQualityOptions) / sizeof(kQualityOptions[0]) - 1];
constexpr std::size_t kNonPlayerParallelThreshold = 4;

int align_render_quality_percent(int percent) {
    int best = kQualityOptions[0];
    int best_diff = std::abs(percent - best);
    for (int option : kQualityOptions) {
        const int diff = std::abs(percent - option);
        if (diff < best_diff) {
            best_diff = diff;
            best = option;
        }
    }
    return best;
}

int halved_render_quality_percent(int percent) {
    if (percent <= kMinRenderQuality) {
        return kMinRenderQuality;
    }
    const int halved = static_cast<int>(std::lround(percent * 0.5));
    return std::max(kMinRenderQuality, align_render_quality_percent(halved));
}

struct TerrainBakeBounds {
    int min_x = std::numeric_limits<int>::max();
    int min_y = std::numeric_limits<int>::max();
    int max_x = std::numeric_limits<int>::min();
    int max_y = std::numeric_limits<int>::min();
    int center_x = 0;
    int center_y = 0;
    int radius = 0;
};

TerrainBakeBounds compute_terrain_bake_bounds(const std::vector<Room*>& rooms,
                                              const nlohmann::json& map_info,
                                              int spacing) {
    TerrainBakeBounds b{};
    auto grow_with_area = [&](const std::unique_ptr<Area>& area) {
        if (!area) {
            return;
        }
        const auto [minx, miny, maxx, maxy] = area->get_bounds();
        b.min_x = std::min(b.min_x, minx);
        b.min_y = std::min(b.min_y, miny);
        b.max_x = std::max(b.max_x, maxx);
        b.max_y = std::max(b.max_y, maxy);
    };

    for (const Room* room : rooms) {
        if (!room) continue;
        grow_with_area(room->room_area);
        for (const auto& named : room->areas) {
            grow_with_area(named.area);
        }
    }

    const double map_radius_d = map_layers::map_radius_from_map_info(map_info);
    if (std::isfinite(map_radius_d) && map_radius_d > 0.0) {
        b.radius = static_cast<int>(std::ceil(map_radius_d));
        b.center_x = b.radius;
        b.center_y = b.radius;
        b.min_x = std::min(b.min_x, b.center_x - b.radius);
        b.min_y = std::min(b.min_y, b.center_y - b.radius);
        b.max_x = std::max(b.max_x, b.center_x + b.radius);
        b.max_y = std::max(b.max_y, b.center_y + b.radius);
    } else {
        if (b.min_x == std::numeric_limits<int>::max()) {
            b.min_x = b.min_y = -spacing;
            b.max_x = b.max_y = spacing;
        }
        b.center_x = (b.min_x + b.max_x) / 2;
        b.center_y = (b.min_y + b.max_y) / 2;
        const double dx = static_cast<double>(b.max_x - b.center_x);
        const double dy = static_cast<double>(b.max_y - b.center_y);
        b.radius = static_cast<int>(std::ceil(std::sqrt(dx * dx + dy * dy)));
    }

    const int pad = std::max(1, spacing);
    b.min_x -= pad;
    b.min_y -= pad;
    b.max_x += pad;
    b.max_y += pad;
    return b;
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

}

Assets::Assets(AssetLibrary& library,
               Asset*,
               std::vector<Room*> rooms,
               int screen_width_,
               int screen_height_,
               int screen_center_x,
               int screen_center_y,
               int map_radius,
               SDL_Renderer* renderer,
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
      map_id_(map_id),
      map_path_(std::move(content_root))
{
    perf_counter_frequency_ = static_cast<double>(SDL_GetPerformanceFrequency());
    last_frame_counter_     = SDL_GetPerformanceCounter();
    map_info_json_ = map_manifest;
    if (!map_info_json_.is_object()) {
        map_info_json_ = nlohmann::json::object();
    }

    hydrate_map_info_sections();
    depth_effects_enabled_ = true;

    vibble::log::info("[Assets] Constructor: Starting InitializeAssets initialization");
    InitializeAssets::initialize(*this, std::move(rooms), screen_width_, screen_height_, screen_center_x, screen_center_y, map_radius);
    vibble::log::info("[Assets] Constructor: InitializeAssets complete");

    finder_ = new CurrentRoomFinder(rooms_, player);
    if (finder_) {
        camera_.set_up_rooms(finder_);
    }

    auto current_room = [&]() -> Room* {
        if (finder_) {
            return finder_->getCurrentRoom();
        }
        return nullptr;
};
    Room* intro_room = current_room();

    SDL_Point intro_center{screen_center_x, screen_center_y};
    if (player) {
        intro_center = SDL_Point{player->world_x(), player->world_y()};
    } else if (Room* room = intro_room) {
        if (room->room_area) {
            intro_center = room->room_area->get_center();
        }
    }
    camera_.set_screen_center(intro_center);
    SDL_Point center_px = camera_.get_screen_center();
    last_camera_center_for_grid_ = world::GridPoint::make_virtual(center_px.x, center_px.y, 0, 0);
    last_camera_scale_for_grid_ = camera_.get_scale();
    last_camera_pitch_for_grid_ = camera_.current_pitch_radians();
    if (player) {
        last_known_player_pos_ = world::GridPoint::make_virtual(player->world_x(), player->world_y(), player->world_z(), player->grid_resolution);
        last_player_pos_valid_ = true;
    } else {
        last_player_pos_valid_ = false;
    }



    if (!renderer) {
        vibble::log::error("[Assets] SceneRenderer not created: SDL_Renderer pointer is null.");
    } else {
        vibble::log::info("[Assets] Constructor: Creating SceneRenderer");
        try {
            scene = new SceneRenderer(renderer, this, screen_width_, screen_height_, map_info_json_, map_id_);
            vibble::log::info("[Assets] Constructor: SceneRenderer created successfully");
        } catch (const std::exception& ex) {
            vibble::log::error(std::string{"[Assets] SceneRenderer initialization failed: "} + ex.what());
            scene = nullptr;
        } catch (...) {
            vibble::log::error("[Assets] SceneRenderer initialization failed with unknown exception");
            scene = nullptr;
        }
    }
    if (scene) {
        scene->set_movement_debug_enabled(movement_debug_enabled_);
        scene->set_anchor_point_debug_enabled(anchor_point_debug_enabled_);
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
    anchor_basis_dirty_queue_.clear();
    anchor_basis_dirty_queue_.reserve(all.size());
    anchor_basis_dirty_lookup_.clear();
    vibble::log::info("[Assets] Constructor: Setting up assets (" + std::to_string(all.size()) + " total)");
    for (Asset* a : all) {
        if (!a) continue;
        a->set_assets(this);
    }
    vibble::log::info("[Assets] Constructor: Asset finalization complete");
    register_pending_static_assets();

    update_filtered_active_assets();

    quick_task_popup_ = std::make_unique<QuickTaskPopup>();
    if (manifest_store_fallback_) {
        quick_task_popup_->set_manifest_store(manifest_store_fallback_.get());
    }

    vibble::log::info("[Assets] Constructor: Initialization complete");
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
    if (map_id_.empty()) {
        std::cerr << "[Assets] Unable to persist map manifest entry: map ID is empty.\n";
        return;
    }
    devmode::core::ManifestStore* store = manifest_store();
    if (!store) {
        std::cerr << "[Assets] Unable to persist map manifest entry: manifest store unavailable.\n";
        return;
    }
    auto guard = store->scoped_guard("Assets::save_map_info_json");
    if (!store->update_map_entry(map_id_, map_info_json_)) {
        std::cerr << "[Assets] Failed to persist map manifest entry for " << map_id_ << "\n";
        return;
    }
    store->flush();
}

bool Assets::mutate_map_data(const std::function<bool(manifest::MapData&)>& mutator) {
    if (!mutator) {
        return false;
    }
    manifest::MapData map_data = manifest::MapData::from_manifest_entry(map_id_, map_info_json_);
    if (!mutator(map_data)) {
        return false;
    }
    map_info_json_ = map_data.to_manifest_entry();
    mark_map_data_dirty();
    return true;
}

void Assets::mark_map_data_dirty() {
    map_data_dirty_ = true;
}

void Assets::snapshot_rooms_to_map_info() {
    if (!map_info_json_.is_object()) {
        map_info_json_ = nlohmann::json::object();
    }
    for (Room* room : rooms_) {
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

bool Assets::persist_map_if_dirty(const std::string& reason) {
    if (!map_data_dirty_) {
        return true;
    }
    try {
        persist_map_info_json();
        std::cout << "[Assets] Map manifest persisted during exit (reason='" << reason << "')\n";
        return true;
    } catch (const std::exception& ex) {
        std::cerr << "[Assets] Failed to persist map manifest during exit (reason='" << reason
                  << "'): " << ex.what() << "\n";
        return false;
    } catch (...) {
        std::cerr << "[Assets] Failed to persist map manifest during exit (reason='" << reason
                  << "')\n";
        return false;
    }
}

bool Assets::save_dirty_asset_caches(const std::string& reason) {
    SDL_Renderer* renderer_ptr = renderer();
    bool ok = true;
    for (const auto& entry : library_.all()) {
        if (!entry.second) {
            continue;
        }
        try {
            entry.second->save_self_to_cache_if_dirty(renderer_ptr);
        } catch (const std::exception& ex) {
            std::cerr << "[Assets] Failed to save asset cache for '" << entry.first
                      << "' during exit (reason='" << reason << "'): " << ex.what() << "\n";
            ok = false;
        } catch (...) {
            std::cerr << "[Assets] Failed to save asset cache for '" << entry.first
                      << "' during exit (reason='" << reason << "')\n";
            ok = false;
        }
    }
    return ok;
}

bool Assets::run_fallback_exit_save(const std::string& reason) {
    const bool had_map_changes = map_data_dirty_;
    bool ok = true;

    if (!persist_map_if_dirty(reason)) {
        ok = false;
    }

    if (!save_dirty_asset_caches(reason)) {
        ok = false;
    }

    if (auto* store = manifest_store()) {
        try {
            store->flush();
        } catch (const std::exception& ex) {
            std::cerr << "[Assets] Manifest flush failed during exit (reason='" << reason
                      << "'): " << ex.what() << "\n";
            ok = false;
        } catch (...) {
            std::cerr << "[Assets] Manifest flush failed during exit (reason='" << reason << "')\n";
            ok = false;
        }
    }

    std::cout << "[Assets] Fallback exit save "
              << (ok ? "finished" : "encountered errors")
              << " (map_dirty_prior=" << (had_map_changes ? "true" : "false")
              << ", reason='" << reason << "')\n";
    return ok;
}

void Assets::hydrate_map_info_sections() {
    if (!map_info_json_.is_object()) {
        return;
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

    ensure_object("map_assets_data");
    ensure_object("map_boundary_data");
    ensure_object("rooms_data");
    ensure_object("trails_data");

    ensure_map_grid_settings(map_info_json_);
    map_grid_settings_ = MapGridSettings::from_json(&map_info_json_["map_grid_settings"]);

}

void Assets::load_camera_settings_from_json() {
    if (!map_info_json_.is_object()) {
        return;
    }
    if (!map_info_json_.contains("camera_settings") || !map_info_json_["camera_settings"].is_object()) {
        map_info_json_["camera_settings"] = nlohmann::json::object();
    }
    camera_.apply_camera_settings(map_info_json_["camera_settings"]);
    apply_camera_runtime_settings();
    camera_view_dirty_ = true;
}

void Assets::write_camera_settings_to_json() {
    if (!map_info_json_.is_object()) {
        return;
    }
    map_info_json_["camera_settings"] = camera_.camera_settings_to_json();
}

void Assets::on_camera_settings_changed() {
    apply_camera_runtime_settings();
    mark_camera_dirty();
    camera_view_dirty_ = true;
}

void Assets::mark_camera_dirty() {
    camera_settings_dirty_ = true;
}

void Assets::reload_camera_settings() {
    vibble::log::info("[Assets] Reloading camera settings from manifest");
    load_camera_settings_from_json();
    mark_camera_dirty();  // CRITICAL: Mark camera dirty to trigger refresh on first frame
    camera_view_dirty_ = true;
    vibble::log::info("[Assets] Camera settings reloaded and marked dirty for refresh");
    log_camera_fog_state("startup-normal");
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

    camera_.update_camera_height(current_room_, finder_, player, true, last_frame_dt_seconds_, dev_mode);

    const SDL_Point center_px = camera_.get_screen_center();
    const world::GridPoint center_point = world::GridPoint::make_virtual(
        center_px.x,
        center_px.y,
        0,
        world_grid_.max_resolution_layers());
    const double current_scale = camera_.get_scale();
    const double current_pitch = camera_.current_pitch_radians();

    // Allow initial rebuild even if frame_id_ == last_active_rebuild_frame_id_
    if (last_active_rebuild_frame_id_ == frame_id_) {
        last_active_rebuild_frame_id_ = static_cast<std::uint32_t>(~frame_id_);
    }

    rebuild_world_grid_and_active_assets(center_point, current_scale, current_pitch);
    pending_initial_rebuild_ = false;
    active_assets_dirty_.store(false, std::memory_order_release);
    last_active_rebuild_frame_id_ = frame_id_;
    needs_filtered_active_refresh_ = true;
}

int Assets::saved_render_quality_percent() const {
    return 100;
}

int Assets::effective_render_quality_percent() const {
    int percent = saved_render_quality_percent();
    if (dev_mode && !force_high_quality_rendering_) {
        percent = halved_render_quality_percent(percent);
    }
    return percent;
}

void Assets::apply_camera_runtime_settings() {
    const int effective_percent = effective_render_quality_percent();
    const float quality_cap = static_cast<float>(effective_percent) / 100.0f;
    render_pipeline::ScalingLogic::SetQualityCap(quality_cap);
    if (scene) {
        const bool low_quality = (effective_percent < 100) && !force_high_quality_rendering_;

    }

}

void Assets::log_camera_fog_state(const char* label) const {
    if (!label) {
        return;
    }

    const Room* room = current_room_;
    const auto& settings = camera_.realism_settings();
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
        " meters_per_100_world_px=" + std::to_string(settings.meters_per_100_world_px) +
        " extra_cull_margin=" + std::to_string(settings.extra_cull_margin) +
        " near_camera_max_perspective_scale=" + std::to_string(settings.near_camera_max_perspective_scale) +
        " offscreen_fade_amount_px=" + std::to_string(settings.offscreen_fade_amount_px) +
        " view_height_world=" + std::to_string(camera_.view_height_world()) +
        " camera_height=" + std::to_string(camera_height) +
        " pitch_deg=" + std::to_string(pitch_deg) +
        " zoom_percent=" + std::to_string(controller_state.params.zoom_percent) +
        " scale=" + std::to_string(camera_.get_scale()) +
        " screen_center=(" + std::to_string(center.x) + "," + std::to_string(center.y) + ")"
    );
}

void Assets::set_depth_effects_enabled(bool enabled) {
    if (depth_effects_enabled_ == enabled) {
        return;
    }
    depth_effects_enabled_ = enabled;
}

void Assets::set_movement_debug_enabled(bool enabled) {
    if (movement_debug_enabled_ == enabled) {
        return;
    }
    movement_debug_enabled_ = enabled;
    if (scene) {
        scene->set_movement_debug_enabled(enabled);
    }
}


void Assets::set_anchor_point_debug_enabled(bool enabled) {
    if (anchor_point_debug_enabled_ == enabled) {
        return;
    }
    anchor_point_debug_enabled_ = enabled;
    if (scene) {
        scene->set_anchor_point_debug_enabled(enabled);
    }
}
void Assets::set_movement_debug_visible(bool visible) {
    if (movement_debug_visible_ == visible) {
        return;
    }
    movement_debug_visible_ = visible;
    if (scene) {
        scene->set_movement_debug_visible(visible);
    }
}

bool Assets::fog_visible() const {
    if (!dev_controls_ || !dev_controls_->is_enabled()) {
        return true;
    }
    return dev_controls_->fog_visible();
}

bool Assets::boundary_assets_visible() const {
    if (!dev_controls_ || !dev_controls_->is_enabled()) {
        return true;
    }
    return dev_controls_->boundary_assets_visible();
}

Assets::~Assets() {
    run_exit_save_sequence("assets_shutdown");

    movement_commands_buffer_.clear();
    grid_registration_buffer_.clear();
    anchor_basis_dirty_queue_.clear();
    anchor_basis_dirty_lookup_.clear();

    if (input) {
        input->clear_screen_to_world_mapper();
    }
    delete scene;
    scene = nullptr;
    delete finder_;
    delete dev_controls_;

}

AssetLibrary& Assets::library() {
    return library_;
}

const AssetLibrary& Assets::library() const {
    return library_;
}

void Assets::set_rooms(std::vector<Room*> rooms) {
    rooms_ = std::move(rooms);
    mark_camera_dirty();
    notify_rooms_changed();
}

void Assets::ensure_light_textures_loaded(Asset* /*asset*/) {
}

std::vector<Room*>& Assets::rooms() {
    return rooms_;
}

const std::vector<Room*>& Assets::rooms() const {
    return rooms_;
}

void Assets::notify_rooms_changed() {
    ++rooms_generation_;
    if (finder_) {
        finder_->setRooms(rooms_);
    }
    if (dev_controls_) {
        dev_controls_->set_rooms(&rooms_, rooms_generation_);
    }
}

void Assets::refresh_active_asset_lists() {
    rebuild_active_assets_if_needed();

    update_audio_camera_metrics();
    update_filtered_active_assets();
}

void Assets::mark_anchor_basis_dirty(Asset* asset) {
    if (!asset || asset->dead) {
        return;
    }
    if (anchor_basis_dirty_lookup_.insert(asset).second) {
        anchor_basis_dirty_queue_.push_back(asset);
    }
}

void Assets::mark_anchor_bases_dirty_for_active_assets() {
    for (Asset* asset : active_assets) {
        mark_anchor_basis_dirty(asset);
    }
}

void Assets::refresh_dirty_anchor_bases() {
    for (Asset* asset : anchor_basis_dirty_queue_) {
        if (!asset || asset->dead) {
            continue;
        }
        asset->update_anchor_basis_if_needed();
    }
    anchor_basis_dirty_queue_.clear();
    anchor_basis_dirty_lookup_.clear();
}

void Assets::update_audio_camera_metrics() {

    SDL_Point camera_focus = camera_.get_screen_center();
    auto update_audio_metrics = [&](Asset* asset) {
        if (!asset) return;
        const float dx = static_cast<float>(asset->world_x() - camera_focus.x);
        const float dy = static_cast<float>(asset->world_y() - camera_focus.y);
        asset->distance_from_camera = std::sqrt(dx * dx + dy * dy);
        asset->angle_from_camera = std::atan2(dy, dx);
};

    if (player) {
        update_audio_metrics(player);
    }
    for (Asset* asset : active_assets) {
        update_audio_metrics(asset);
    }

    AudioEngine::instance().update();
}

void Assets::refresh_filtered_active_assets() {
    update_filtered_active_assets();
}

void Assets::update_filtered_active_assets() {
    const bool dev_controls_enabled = dev_controls_ && dev_controls_->is_enabled();
    const std::uint64_t current_generation = active_assets_generation_;
    const std::uint64_t filter_version = dev_controls_enabled ? dev_controls_->other_settings_state_version() : 0;

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
    if (previous.world_x() == current.world_x() && previous.world_y() == current.world_y()) {
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
        dev_controls_->set_active_assets(filtered_active_assets, dev_active_state_version_);
        sync_dev_controls_current_room(current_room_, true);
        dev_controls_->set_screen_dimensions(screen_width, screen_height);
        dev_controls_->set_rooms(&rooms_, rooms_generation_);
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
            dev_controls_->set_rooms(&rooms_, rooms_generation_);
            dev_controls_->set_map_context(&map_info_json_, map_path_);
        }
    }
}

void Assets::update(const Input& input)
{
    const std::uint64_t now_counter = SDL_GetPerformanceCounter();

    if (!should_step_dev_frame(input)) {
        last_frame_dt_seconds_ = 0.0f;
        last_frame_counter_    = now_counter;
        return;
    }

    ++frame_id_;
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
    if (scene && ctrl_down && input.wasScancodePressed(SDL_SCANCODE_Q)) {

    }

    if (ctrl_down && input.wasScancodePressed(SDL_SCANCODE_B)) {
        asset_boundary_box_display_enabled_ = !asset_boundary_box_display_enabled_;
        std::cout << "[Assets] Asset boundary box display "
                  << (asset_boundary_box_display_enabled_ ? "enabled" : "disabled") << " (Ctrl+B).\n";
    }

    if (ctrl_down && input.wasScancodePressed(SDL_SCANCODE_T) && quick_task_popup_) {
        if (quick_task_popup_->is_open()) {
            quick_task_popup_->close();
        } else {
            quick_task_popup_->open();
        }
        std::cout << "[Assets] Quick Task popup "
                  << (quick_task_popup_->is_open() ? "opened" : "closed") << " (Ctrl+T).\n";
    }

    if (ctrl_down && shift_down && input.wasScancodePressed(SDL_SCANCODE_P)) {
        const bool enable = !anchor_point_debug_enabled_;
        set_anchor_point_debug_enabled(enable);
        std::cout << "[Assets] Anchor point overlay "
                  << (enable ? "enabled" : "disabled") << " (Ctrl+Shift+P).\n";
    }

    if (quick_task_popup_) {
        quick_task_popup_->update();
    }

    if (process_removals()) {
        mark_active_assets_dirty();
    }

    Room* detected_room = finder_ ? finder_->getCurrentRoom() : nullptr;
    Room* active_room = detected_room;
    if (dev_controls_ && dev_controls_->is_enabled()) {
        active_room = dev_controls_->resolve_current_room(detected_room);
    }
    const bool room_changed = (current_room_ != active_room);
    current_room_ = active_room;

    dx = dy = 0;

    // Pause runtime asset updates while in Dev Mode unless a frame editor session requires them.
    const bool runtime_updates_enabled = should_run_runtime_updates();

    int start_px = player ? player->world_x() : 0;
    int start_py = player ? player->world_y() : 0;

    if (player) {
        player->active = true;
        if (runtime_updates_enabled) {

            player->update();
        } else {

            if (player->info) {
                player->update_scale_values();
            }
        }
    }

    bool player_moved = false;
    if (player) {
        dx = player->world_x() - start_px;
        dy = player->world_y() - start_py;
        const bool moved_during_update = (dx != 0 || dy != 0);
        world::GridPoint current_player_pos = world::GridPoint::make_virtual(player->world_x(),
                                                                             player->world_y(),
                                                                             player->world_z(),
                                                                             player->grid_resolution);
        const bool moved_since_last_frame =
            !last_player_pos_valid_ ||
            current_player_pos.world_x() != last_known_player_pos_.world_x() ||
            current_player_pos.world_y() != last_known_player_pos_.world_y();

        last_known_player_pos_ = std::move(current_player_pos);
        last_player_pos_valid_ = true;

        player_moved = moved_during_update || moved_since_last_frame;
        if (runtime_updates_enabled && moved_during_update) {
            log_asset_movement(player,
                               world::GridPoint::make_virtual(start_px, start_py, player->world_z(), player->grid_resolution),
                               current_player_pos);
        }
    } else {
        last_player_pos_valid_ = false;
    }

    rebuild_non_player_update_buffer_if_needed();

    for (Asset* asset : non_player_update_buffer_) {
        if (!asset) continue;
        world::GridPoint previous_pos = world::GridPoint::make_virtual(asset->world_x(),
                                                                       asset->world_y(),
                                                                       asset->world_z(),
                                                                       asset->grid_resolution);
        asset->active = true;

        if (runtime_updates_enabled) {

            asset->update();
            if (previous_pos.world_x() != asset->world_x() || previous_pos.world_y() != asset->world_y()) {
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
                world_grid_.move_asset(cmd.asset, cmd.previous, cmd.current);
                cmd.asset->cache_grid_residency(cmd.current);
                mark_anchor_basis_dirty(cmd.asset);
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

    const bool height_animation_active = false;
    const bool camera_refresh_needed = room_changed || player_moved || height_animation_active || camera_settings_dirty_;
    if (dev_controls_) {
        dev_controls_->sync_camera_tilt_override();
    }
    camera_.update_camera_height(current_room_, finder_, player, camera_refresh_needed, last_frame_dt_seconds_, dev_mode);
    camera_settings_dirty_ = false;

    update_max_asset_dimensions();

    culled_debug_rects_.clear();

    if (dev_controls_ && dev_controls_->is_enabled()) {
        sync_dev_controls_current_room(current_room_);
        dev_controls_->update(input);

        dev_controls_->update_ui(input);
    }

    register_pending_static_assets();
    if (process_removals()) {
        mark_active_assets_dirty();
    }

    maybe_rebuild_world_grid();

    refresh_dirty_anchor_bases();

    // Run binding follow updates after anchors are refreshed so children see latest transforms.
    for (auto* helper : binding_helpers_) {
        if (helper) {
            helper->tick_for_frame();
        }
    }

    update_audio_camera_metrics();

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

    if (!suppress_render_ && scene) {
        scene->render();
    }

    render_overlays(renderer());

    last_camera_state_version_for_dev_ = camera_.camera_state_version();
    last_dev_active_state_version_snapshot_ = dev_active_state_version_;
    dev_frame_initialized_ = true;
}

void Assets::rebuild_non_player_update_buffer_if_needed() {
    if (!non_player_update_buffer_dirty_.load(std::memory_order_acquire)) {
        return;
    }

    auto compute_depth_hint = [](const Asset* asset) -> int {
        return asset ? asset->depth : 0;
    };

    non_player_update_buffer_.clear();
    non_player_update_buffer_.reserve(active_traversal_.size());
    std::unordered_set<const Asset*> buffer_set;

    for (world::GridPoint* point : active_points_) {
        if (!point) {
            continue;
        }

        std::vector<Asset*> point_assets;
        point_assets.reserve(point->occupants.size());
        for (const auto& occ : point->occupants) {
            Asset* asset = occ.get();
            if (!asset || asset->dead) {
                continue;
            }
            point_assets.push_back(asset);
        }

        if (point_assets.empty()) {
            continue;
        }

        std::stable_sort(point_assets.begin(),
                         point_assets.end(),
                         [&compute_depth_hint](const Asset* a, const Asset* b) {
                             if (!a || !b) return b != nullptr;
                             const int depth_a = compute_depth_hint(a);
                             const int depth_b = compute_depth_hint(b);
                             if (depth_a != depth_b) {
                                 return depth_a < depth_b;
                             }
                             return a < b;
                         });

        for (Asset* asset : point_assets) {
            if (!asset) {
                continue;
            }

            // Always respect traversal order; skip adding player itself to the buffer.
            if (asset != player && buffer_set.insert(asset).second) {
                non_player_update_buffer_.push_back(asset);
            }

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

    const float frustum_padding = std::max(max_asset_width_world_, max_asset_height_world_);
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
    const int width = std::max(0, maxx - minx);
    const int height = std::max(0, maxy - miny);
    return world::GridBounds::from_xywh(minx, miny, width, height, 0, world_grid_.default_resolution_layer());
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
        dev_mode = false;
        dev_frame_initialized_ = false;
        last_camera_state_version_for_dev_ = camera_.camera_state_version();
        last_dev_active_state_version_snapshot_ = dev_active_state_version_;
        show_dev_notice("Dev Mode disabled", 1500);
    }

    apply_camera_runtime_settings();
    log_camera_fog_state(dev_mode ? "dev-mode-enabled" : "dev-mode-disabled");
}

bool Assets::run_exit_save_sequence(const std::string& reason) {
    if (exit_save_sequence_ran_) {
        std::cout << "[Assets] Exit save sequence already executed; reusing result (reason='"
                  << reason << "', success=" << (exit_save_sequence_ok_ ? "true" : "false")
                  << ")\n";
        return exit_save_sequence_ok_;
    }

    exit_save_sequence_ran_ = true;

    bool ok = false;
    const bool had_dev_controls = (dev_controls_ != nullptr);
    if (dev_controls_) {
        ok = dev_controls_->run_exit_save_sequence(reason);
    }

    const bool need_fallback = !dev_controls_ || !ok;
    if (need_fallback) {
        ok = run_fallback_exit_save(reason) || ok;
    }

    exit_save_sequence_ok_ = ok;

    if (exit_save_sequence_ok_) {
        std::cout << "[Assets] Exit save sequence complete (reason='" << reason
                  << "', dev_path=" << (had_dev_controls ? "true" : "false")
                  << ", fallback=" << (need_fallback ? "true" : "false") << ")\n";
    } else {
        std::cerr << "[Assets] EXIT SAVE FAILURE (reason='" << reason
                  << "', dev_path=" << (had_dev_controls ? "true" : "false")
                  << ", fallback=" << (need_fallback ? "true" : "false") << ")\n";
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

    if (scene) {
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
    mark_anchor_bases_dirty_for_active_assets();
}

std::unique_ptr<Asset> Assets::extract_asset(Asset* asset) {
    if (!asset) {
        return nullptr;
    }
    return world_grid_.extract_asset(asset);
}

world::GridPoint Assets::resolve_floor_world_point(SDL_Point world_pos, int resolution_layer) const {
    const int max_layer = world_grid_.max_resolution_layers();
    const int requested_layer = (resolution_layer >= 0) ? resolution_layer : std::clamp(max_layer - map_grid_settings_.grid_resolution, 0, max_layer);
    const int layer = std::clamp(requested_layer, 0, max_layer);

    world::GridPoint resolved = world::GridPoint::make_virtual(world_pos.x, world_pos.y, 0, layer);
    const world::GridKey key{world_pos.x, world_pos.y, 0, layer};
    if (const world::GridPoint* baked = world_grid_.find_grid_point(key)) {
        resolved.terrain_elevation = baked->terrain_elevation;
        resolved.terrain_slope_x = baked->terrain_slope_x;
        resolved.terrain_slope_y = baked->terrain_slope_y;
        resolved.terrain_revision = baked->terrain_revision;
        const int floor_z = static_cast<int>(std::lround(static_cast<double>(baked->terrain_elevation)));
        resolved.set_world_z(floor_z);
    }
    return resolved;
}

Asset* Assets::attach_asset(std::unique_ptr<Asset> asset, int world_z, int resolution_layer) {
    if (!asset) {
        return nullptr;
    }

    Asset* raw = asset.get();
    // Avoid double insertion in all vector.
    const bool already_tracked = std::find(all.begin(), all.end(), raw) != all.end();

    const int resolved_layer = (resolution_layer >= 0) ? resolution_layer : world_grid_.default_resolution_layer();
    const int resolved_z = (world_z != 0) ? world_z : resolve_floor_world_point(raw->world_point(), resolved_layer).world_z();

    raw = world_grid_.attach_asset(std::move(asset), resolved_z, resolved_layer);
    if (!raw) {
        return nullptr;
    }

    if (!already_tracked) {
        all.push_back(raw);
    }

    queue_asset_dimension_update(raw);
    mark_grid_dirty();
    mark_active_assets_dirty();
    mark_anchor_basis_dirty(raw);
    mark_non_player_update_buffer_dirty();

    return raw;
}

void Assets::register_binding_helper(AnchorBoundAssetHelper* helper) {
    if (!helper) return;
    if (std::find(binding_helpers_.begin(), binding_helpers_.end(), helper) == binding_helpers_.end()) {
        binding_helpers_.push_back(helper);
    }
}

void Assets::unregister_binding_helper(AnchorBoundAssetHelper* helper) {
    if (!helper) return;
    auto it = std::remove(binding_helpers_.begin(), binding_helpers_.end(), helper);
    binding_helpers_.erase(it, binding_helpers_.end());
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
    auto uptr = std::make_unique<Asset>(info, spawn_area, world_pos, depth, std::string{}, std::string{}, map_grid_settings_.spacing());
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
                                        map_grid_settings_.spacing());
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
    initialize_active_assets(world::GridPoint::make_virtual(center_px.x, center_px.y, 0, world_grid_.max_resolution_layers()));
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

bool Assets::maybe_rebuild_world_grid() {
    // Prevent double rebuild in same frame
    if (frame_id_ == last_grid_rebuild_frame_ || frame_id_ == last_active_rebuild_frame_id_) {
        return false;
    }

    const SDL_Point center_px = camera_.get_screen_center();
    const world::GridPoint current_center = world::GridPoint::make_virtual(
        center_px.x,
        center_px.y,
        0,
        world_grid_.max_resolution_layers());
    const double current_scale = camera_.get_scale();
    const double current_pitch = camera_.current_pitch_radians();
    constexpr double kCameraGridEpsilon = 1e-4;
    const bool active_dirty = active_assets_dirty_.load(std::memory_order_acquire) || pending_initial_rebuild_;
    const bool camera_changed =
        current_center.world_x() != last_camera_center_for_grid_.world_x() ||
        current_center.world_y() != last_camera_center_for_grid_.world_y() ||
        std::fabs(current_scale - last_camera_scale_for_grid_) > kCameraGridEpsilon ||
        std::fabs(current_pitch - last_camera_pitch_for_grid_) > kCameraGridEpsilon;

    camera_view_dirty_ = camera_view_dirty_ || camera_changed;
    if (!grid_dirty_ && !camera_view_dirty_ && !active_dirty) {
        return false;
    }

    rebuild_world_grid_and_active_assets(current_center, current_scale, current_pitch);
    return true;
}

void Assets::rebuild_world_grid_and_active_assets(const world::GridPoint& current_center,
                                                  double current_scale,
                                                  double current_pitch) {
    last_grid_rebuild_frame_ = frame_id_;
    camera_.rebuild_grid(world_grid_,
                         last_frame_dt_seconds_,
                         frame_id_);
    world_grid_.update_active_chunks(screen_world_rect(), 0);
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
}

void Assets::untrack_asset_for_grid(Asset* asset) {
    if (!asset) {
        return;
    }
    (void)world_grid_.remove_asset(asset);
}

void Assets::mark_grid_dirty() {
    grid_dirty_ = true;
}

void Assets::register_pending_static_assets() {
    pending_static_grid_registration_.clear();
}

void Assets::rebuild_all_assets_from_grid() {
    all.clear();
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
}

bool Assets::rebuild_active_assets_if_needed() {
    const bool dirty = active_assets_dirty_.load(std::memory_order_acquire) || pending_initial_rebuild_;
    if (!dirty) {
        return false;
    }
    if (frame_id_ == last_active_rebuild_frame_id_) {
        return false;
    }

    const SDL_Point center_px = camera_.get_screen_center();
    const world::GridPoint current_center = world::GridPoint::make_virtual(center_px.x, center_px.y, 0, world_grid_.max_resolution_layers());
    const double current_scale = camera_.get_scale();
    const double current_pitch = camera_.current_pitch_radians();

    pending_initial_rebuild_ = false;
    initialize_active_assets(current_center);
    rebuild_world_grid_and_active_assets(current_center, current_scale, current_pitch);
    return true;
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
        const float base_z = 0.0f;
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

    // Purge anchor bindings that reference assets slated for removal (parents or children).
    if (!binding_helpers_.empty()) {
        for (Asset* victim : unique_removals) {
            for (auto* helper : binding_helpers_) {
                if (helper) {
                    helper->purge_bindings_for_asset(victim);
                }
            }
        }
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
        remove_asset_dimension_cache(asset);
        asset->clear_grid_residency_cache();
        (void)world_grid_.remove_asset(asset);
    }

    rebuild_all_assets_from_grid();
    active_assets.clear();
    filtered_active_assets.clear();
    moving_assets_for_grid_.clear();
    pending_static_grid_registration_.clear();
    active_points_.clear();
    anchor_basis_dirty_queue_.clear();
    anchor_basis_dirty_lookup_.clear();
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
    for (Asset* asset : all) {
        if (!asset || asset->dead || asset == player) {
            continue;
        }
        if (asset->spawn_id == spawn_id) {
            batch.mark_for_deletion(asset);
            ++count;
        }
    }

    if (count == 0) {
        return 0;
    }

    if (!batch.commit()) {
        return 0;
    }
    return count;
}

void Assets::render_overlays(SDL_Renderer* renderer) {
    if (renderer && dev_controls_ && dev_controls_->is_enabled()) {
        dev_controls_->render_overlays(renderer);
    }

    if (quick_task_popup_) {
        quick_task_popup_->render(renderer);
    }

    const Uint32 now = SDL_GetTicks();
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
}

SDL_Renderer* Assets::renderer() const {
    if (suppress_dev_renderer_) {
        return nullptr;
    }
    return scene ? scene->get_renderer() : nullptr;
}

std::optional<Asset::TilingInfo> Assets::compute_tiling_for_asset(const Asset* asset) const {
    if (!asset || !asset->info) {
        return std::nullopt;
    }
    if (!asset->info->tillable) {
        return std::nullopt;
    }

    int step = map_grid_settings_.tile_spacing();

    if (step <= 0) {
        const int raw_w = std::max(1, asset->info->original_canvas_width);
        const int raw_h = std::max(1, asset->info->original_canvas_height);
        double scale = 1.0;
        if (std::isfinite(asset->info->scale_factor) && asset->info->scale_factor > 0.0f) {
            scale = static_cast<double>(asset->info->scale_factor);
        }
        step = std::max(1, static_cast<int>(std::lround(static_cast<double>(std::max(raw_w, raw_h)) * scale)));
    }
    step = std::max(1, step);

    const SDL_Point world_pos{ asset->world_x(), asset->world_y() };
    const int base_w = std::max(1, asset->info->original_canvas_width);
    const int base_h = std::max(1, asset->info->original_canvas_height);
    double scale = 1.0;
    if (std::isfinite(asset->info->scale_factor) && asset->info->scale_factor > 0.0f) {
        scale = static_cast<double>(asset->info->scale_factor);
    }
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
    world_grid_.set_chunk_resolution(std::max(0, sanitized.grid_resolution));

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
        world_grid_.update_active_chunks(screen_world_rect(), 0);
        mark_grid_dirty();
    }
}

int Assets::map_grid_chunk_resolution() const {
    return std::max(0, map_grid_settings_.grid_resolution);
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
    if (dev_controls_ && dev_controls_->is_enabled()) {
        dev_controls_->toggle_room_config();
    }
}

void Assets::close_room_config() {
    if (dev_controls_ && dev_controls_->is_enabled()) {
        dev_controls_->close_room_config();
    }
}

bool Assets::is_room_config_open() const {
    return dev_controls_ && dev_controls_->is_enabled() && dev_controls_->is_room_config_open();
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

bool Assets::is_asset_info_editor_open() const {
    return dev_controls_ && dev_controls_->is_enabled() && dev_controls_->is_asset_info_editor_open();
}

void Assets::clear_editor_selection() {
    if (dev_controls_ && dev_controls_->is_enabled()) {
        dev_controls_->clear_selection();
    }
}

void Assets::handle_sdl_event(const SDL_Event& e) {

    if (quick_task_popup_ && quick_task_popup_->is_open()) {
        if (quick_task_popup_->handle_event(e)) {

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
    if (!manifest_store_fallback_) {
        manifest_store_fallback_ = std::make_unique<devmode::core::ManifestStore>();
    }
    return manifest_store_fallback_.get();
}

const devmode::core::ManifestStore* Assets::manifest_store() const {
    return const_cast<Assets*>(this)->manifest_store();
}

void Assets::notify_spawn_group_config_changed(const nlohmann::json& entry) {
    if (dev_controls_ && dev_controls_->is_enabled()) {
        dev_controls_->notify_spawn_group_config_changed(entry);
    }
}

void Assets::notify_spawn_group_removed(const std::string& spawn_id) {
    if (dev_controls_ && dev_controls_->is_enabled()) {
        dev_controls_->notify_spawn_group_removed(spawn_id);
    }
}

void Assets::invalidate_dynamic_boundary_system() {
    if (scene) {
        scene->invalidate_dynamic_boundary_system();
    }
}

void Assets::bake_terrain_if_needed(const TerrainRuntimeState& state, TerrainField& field) {
    if (!state.settings.enabled) {
        world_grid_.clear_terrain_bake_revision();
        return;
    }
    if (world_grid_.terrain_bake_revision() == state.revision) {
        return;
    }

    const int spacing = std::max(1, map_grid_settings_.spacing());
    const int max_layer = world_grid_.max_resolution_layers();
    const int bake_layer = std::clamp(max_layer - map_grid_settings_.grid_resolution, 0, max_layer);
    const TerrainBakeBounds bounds = compute_terrain_bake_bounds(rooms_, map_info_json_, spacing);
    const long long radius_sq = static_cast<long long>(bounds.radius) * static_cast<long long>(bounds.radius);

    field.set_runtime_state(state, rooms_);
    constexpr std::uint64_t frame_id = 0;

    auto sample_height = [&](const world::GridKey& key) -> float {
        return field.sample_elevation(key, world_grid_, rooms_, state, frame_id);
    };

    auto bake_point = [&](world::GridPoint& gp, const world::GridKey& key, int step) {
        const int safe_step = std::max(1, step);
        const float height = sample_height(key);
        const float hx1 = sample_height(world::GridKey{key.x + safe_step, key.y, key.z, key.layer});
        const float hx0 = sample_height(world::GridKey{key.x - safe_step, key.y, key.z, key.layer});
        const float hy1 = sample_height(world::GridKey{key.x, key.y + safe_step, key.z, key.layer});
        const float hy0 = sample_height(world::GridKey{key.x, key.y - safe_step, key.z, key.layer});
        const float denom = static_cast<float>(safe_step * 2);
        float slope_x = 0.0f;
        float slope_y = 0.0f;
        if (denom > 0.0f) {
            slope_x = (hx1 - hx0) / denom;
            slope_y = (hy1 - hy0) / denom;
        }

        gp.terrain_elevation = height;
        gp.terrain_slope_x = slope_x;
        gp.terrain_slope_y = slope_y;
        gp.terrain_revision = state.revision;
        gp.is_floor = (key.z == 0);
    };

    std::size_t baked_points = 0;
    for (int y = bounds.min_y; y <= bounds.max_y; y += spacing) {
        for (int x = bounds.min_x; x <= bounds.max_x; x += spacing) {
            if (bounds.radius > 0) {
                const long long dx = static_cast<long long>(x - bounds.center_x);
                const long long dy = static_cast<long long>(y - bounds.center_y);
                if (dx * dx + dy * dy > radius_sq) {
                    continue;
                }
            }
            world::GridKey key{x, y, 0, bake_layer};
            world::GridPoint& gp = world_grid_.find_or_create_grid_point(key);
            bake_point(gp, key, spacing);
            ++baked_points;
        }
    }

    for (auto& entry : world_grid_.points()) {
        world::GridPoint& gp = entry.second;
        const int layer = std::clamp(gp.resolution_layer(), 0, max_layer);
        const int step = std::max(1, world_grid_.grid_spacing_for_layer(layer));
        world::GridKey key{gp.world_x(), gp.world_y(), gp.world_z(), layer};
        bake_point(gp, key, step);
    }

    world_grid_.set_terrain_bake_revision(state.revision);
    vibble::log::info("[Assets] Terrain baked for " + std::to_string(baked_points) +
                      " cells (rev=" + std::to_string(state.revision) + ").");
}

void Assets::set_terrain_sources(TerrainField* field, const TerrainRuntimeState& state) {
    terrain_field_source_ = field;
    terrain_runtime_state_ = state;
    if (terrain_field_source_) {
        try {
            bake_terrain_if_needed(state, *terrain_field_source_);
        } catch (const std::exception& ex) {
            vibble::log::warn(std::string("[Assets] Terrain bake skipped due to error: ") + ex.what());
        } catch (...) {
            vibble::log::warn("[Assets] Terrain bake skipped due to unknown error.");
        }
    }
    mark_grid_dirty();
    camera_view_dirty_ = true;
}

void Assets::refresh_terrain_dependents() {
    if (terrain_runtime_state_) {
        world_grid_.clear_terrain_bake_revision();
        if (terrain_field_source_) {
            try {
                bake_terrain_if_needed(*terrain_runtime_state_, *terrain_field_source_);
            } catch (const std::exception& ex) {
                vibble::log::warn(std::string("[Assets] Terrain rebake skipped due to error: ") + ex.what());
            } catch (...) {
                vibble::log::warn("[Assets] Terrain rebake skipped due to unknown error.");
            }
        }
    }

    mark_grid_dirty();
    mark_active_assets_dirty();
    ++active_candidate_generation_;
    if (active_candidate_generation_ == 0) {
        ++active_candidate_generation_;
    }
    visible_candidate_buffer_.clear();
    camera_view_dirty_ = true;
    invalidate_dynamic_boundary_system();
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

namespace {
inline bool is_trail_string(const std::string& text) {
    if (text.size() != 5) return false;
    return std::tolower(static_cast<unsigned char>(text[0])) == 't' &&
           std::tolower(static_cast<unsigned char>(text[1])) == 'r' &&
           std::tolower(static_cast<unsigned char>(text[2])) == 'a' &&
           std::tolower(static_cast<unsigned char>(text[3])) == 'i' &&
           std::tolower(static_cast<unsigned char>(text[4])) == 'l';
}
}

void Assets::classify_region(world::GridPoint& point) {
    point.region_kind = world::GridPoint::RegionKind::Boundary;
    point.region_owner = nullptr;

    const SDL_Point pt{point.world_x(), point.world_y()};
    for (Room* room : rooms_) {
        if (!room) continue;
        const bool room_is_trail = is_trail_string(room->type);
        if (room->room_area && room->room_area->contains_point(pt)) {
            point.region_kind = room_is_trail ? world::GridPoint::RegionKind::Trail
                                              : world::GridPoint::RegionKind::Room;
            point.region_owner = room;
            return;
        }
        // Named areas that are marked as trail
        for (const auto& named : room->areas) {
            if (!named.area) continue;
            if (!(is_trail_string(named.type) || is_trail_string(named.kind) || is_trail_string(named.name))) {
                continue;
            }
            try {
                if (named.area->contains_point(pt)) {
                    point.region_kind = world::GridPoint::RegionKind::Trail;
                    point.region_owner = room;
                    return;
                }
            } catch (...) {
            }
        }
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

    active_points_.clear();
    active_traversal_.clear();
    active_assets.clear();
    active_assets.reserve(camera_.get_visible_points().size() * 4);
    visible_candidate_buffer_.clear();
    visible_candidate_buffer_.reserve(camera_.get_visible_points().size() * 2);
    std::unordered_set<Asset*> seen_assets;
    seen_assets.reserve(camera_.get_visible_points().size() * 2);

    const double anchor_world_y = camera_.anchor_world_y();

    // Screen Grid traversal: use per-frame visible nodes (already filtered by region + branch masks).
    for (world::GridPoint* point : camera_.get_visible_points()) {
        if (!point || !point->on_screen) {
            continue;
        }
        const float point_alpha = point->horizon_fade_alpha * point->near_camera_fade_alpha;
        if (!std::isfinite(point_alpha)) {
            continue;
        }
        if (!point->has_assets_or_active_children()) {
            continue;
        }
        classify_region(*point);
        active_points_.push_back(point);
        for (const auto& occ : point->occupants) {
            Asset* asset = occ.get();
            if (!asset || asset->dead) {
                continue;
            }
            if (!seen_assets.insert(asset).second) {
                continue; // already handled this frame
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
            active_traversal_.push_back(ActiveTraversalEntry{
                asset,
                point,
                anchor_world_y - static_cast<double>(asset->world_y()) + asset->render_depth_bias()
            });
        }
    }
    // Depth-sort traversal so rendering can interleave with fog/boundary sprites in a single pass.
    std::stable_sort(
        active_traversal_.begin(),
        active_traversal_.end(),
        [](const ActiveTraversalEntry& a, const ActiveTraversalEntry& b) {
            return a.depth_from_anchor > b.depth_from_anchor;
        });

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
    }

    // Update scale values for ALL active assets after grid rebuild
    // to ensure scales reflect current perspective (fixes single-frame asset scaling)
    for (Asset* asset : active_assets) {
        if (!asset) {
            continue;
        }
        asset->update_scale_values();
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

    if (has_pending_dev_work(false)) {
        return true;
    }

    return false;
}

bool Assets::has_pending_dev_work(bool include_animation_plans) const {
    if (pending_initial_rebuild_) return true;
    if (popup_manager_.has_active_content()) return true;
    if (quick_task_popup_ && quick_task_popup_->is_open()) return true;

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
    if (dev_controls_ && dev_controls_->is_frame_editor_session_active()) {
        return true;
    }
    return false;
}

bool Assets::should_advance_animation_for(const Asset* asset) const {
    if (!asset) {
        return false;
    }
    if (!dev_mode) {
        return true;
    }
    if (is_frame_editor_target_active(asset)) {
        return true;
    }
    return false;
}
