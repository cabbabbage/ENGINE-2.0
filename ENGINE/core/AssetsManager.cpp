#include "AssetsManager.hpp"

#include "utils/ranged_color.hpp"
#include "asset/initialize_assets.hpp"

#include "find_current_room.hpp"
#include "asset/Asset.hpp"
#include "asset/asset_info.hpp"
#include "asset/asset_utils.hpp"
#include "asset/asset_types.hpp"
#include "animation_update/animation_runtime.hpp"
#include "audio/audio_engine.hpp"
#include "dev_mode/dev_controls.hpp"
#include "dev_mode/depth_cue_settings.hpp"
#include "render/render.hpp"
#include "world/chunk.hpp"
#include "map_generation/room.hpp"
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
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <execution>
#include <nlohmann/json.hpp>
#include <functional>
#include <thread>
#include <vector>
#include <unordered_set>
#include <unordered_map>
#include <array>
#include <SDL.h>
#include <SDL_ttf.h>

namespace {

std::uint64_t hash_active_asset_list(const std::vector<Asset*>& list) {
    std::uint64_t hash = static_cast<std::uint64_t>(list.size());
    constexpr std::uint64_t prime = 1469598103934665603ull;
    for (const Asset* asset : list) {
        auto ptr_value = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(asset));
        hash ^= (ptr_value >> 4);
        hash *= prime;
    }
    return hash;
}

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
    const float bottom = static_cast<float>(asset->pos.y);

    bounds.left   = static_cast<float>(asset->pos.x) - half_w;
    bounds.right  = static_cast<float>(asset->pos.x) + half_w;
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
    depth_effects_enabled_ = false;

    InitializeAssets::initialize(*this, std::move(rooms), screen_width_, screen_height_, screen_center_x, screen_center_y, map_radius);

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
        intro_center = SDL_Point{player->pos.x, player->pos.y};
    } else if (Room* room = intro_room) {
        if (room->room_area) {
            intro_center = room->room_area->get_center();
        }
    }
    camera_.set_screen_center(intro_center);
    last_camera_center_for_grid_ = camera_.get_screen_center();
    last_camera_scale_for_grid_ = camera_.get_scale();
    last_camera_pitch_for_grid_ = camera_.current_pitch_radians();
    if (player) {
        last_known_player_pos_ = SDL_Point{player->pos.x, player->pos.y};
        last_player_pos_valid_ = true;
    } else {
        last_player_pos_valid_ = false;
    }



    if (!renderer) {
        vibble::log::error("[Assets] SceneRenderer not created: SDL_Renderer pointer is null.");
    } else {
        try {
            scene = new SceneRenderer(renderer, this, screen_width_, screen_height_, map_info_json_, map_id_);
        } catch (const std::exception& ex) {
            vibble::log::error(std::string{"[Assets] SceneRenderer initialization failed: "} + ex.what());
            scene = nullptr;
        }
    }
    if (scene) {
        scene->set_movement_debug_enabled(movement_debug_enabled_);
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
    for (Asset* a : all) {
        if (!a) continue;
        a->set_assets(this);
    }
    register_pending_static_assets();

    update_filtered_active_assets();

    quick_task_popup_ = std::make_unique<QuickTaskPopup>();
    if (manifest_store_fallback_) {
        quick_task_popup_->set_manifest_store(manifest_store_fallback_.get());
    }

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
    if (!store->update_map_entry(map_id_, map_info_json_)) {
        std::cerr << "[Assets] Failed to persist map manifest entry for " << map_id_ << "\n";
    }
}

void Assets::persist_map_info_json() {
    save_map_info_json();
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
}

void Assets::write_camera_settings_to_json() {
    if (!map_info_json_.is_object()) {
        return;
    }
    map_info_json_["camera_settings"] = camera_.camera_settings_to_json();
}

void Assets::on_camera_settings_changed() {
    apply_camera_runtime_settings();
}

void Assets::mark_camera_dirty() {
    camera_settings_dirty_ = true;
}

void Assets::reload_camera_settings() {
    load_camera_settings_from_json();
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

void Assets::set_movement_debug_visible(bool visible) {
    if (movement_debug_visible_ == visible) {
        return;
    }
    movement_debug_visible_ = visible;
    if (scene) {
        scene->set_movement_debug_visible(visible);
    }
}

Assets::~Assets() {
    movement_commands_buffer_.clear();
    grid_registration_buffer_.clear();
    for (Asset* child : child_timeline_assets_) {
        delete child;
    }
    child_timeline_assets_.clear();

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

void Assets::update_audio_camera_metrics() {

    SDL_Point camera_focus = camera_.get_screen_center();
    auto update_audio_metrics = [&](Asset* asset) {
        if (!asset) return;
        const float dx = static_cast<float>(asset->pos.x - camera_focus.x);
        const float dy = static_cast<float>(asset->pos.y - camera_focus.y);
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
    const std::uint64_t previous_hash = filtered_active_assets_hash_;

    if (dev_controls_ && dev_controls_->is_enabled()) {
        const std::uint64_t active_hash = hash_active_asset_list(active_assets);
        const std::uint64_t filter_version = dev_controls_->other_settings_state_version();
        if (active_hash == filtered_active_assets_source_hash_ &&
            filter_version == filtered_active_assets_filter_version_) {
            return;
        }

        filtered_active_assets = active_assets;
        dev_controls_->filter_active_assets(filtered_active_assets);
        filtered_active_assets_hash_ = hash_active_asset_list(filtered_active_assets);
        filtered_active_assets_source_hash_ = active_hash;
        filtered_active_assets_filter_version_ = filter_version;
    } else {
        if (filtered_active_assets.empty() &&
            filtered_active_assets_hash_ == 0 &&
            filtered_active_assets_source_hash_ == 0 &&
            filtered_active_assets_filter_version_ == 0) {
            return;
        }

        filtered_active_assets.clear();
        filtered_active_assets_hash_ = hash_active_asset_list(filtered_active_assets);
        filtered_active_assets_source_hash_ = 0;
        filtered_active_assets_filter_version_ = 0;
    }

    if (filtered_active_assets_hash_ != previous_hash) {
        touch_dev_active_state_version();
    }
}

void Assets::log_asset_movement(Asset* asset, SDL_Point previous, SDL_Point current) {
    if (!asset) {
        return;
    }
    if (previous.x == current.x && previous.y == current.y) {
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
    ++frame_id_;
    const std::uint64_t now_counter = SDL_GetPerformanceCounter();
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

    if (quick_task_popup_) {
        quick_task_popup_->update();
    }

    if (process_removals()) {
        mark_active_assets_dirty();
        rebuild_active_assets_if_needed();
    }

    Room* detected_room = finder_ ? finder_->getCurrentRoom() : nullptr;
    Room* active_room = detected_room;
    if (dev_controls_ && dev_controls_->is_enabled()) {
        active_room = dev_controls_->resolve_current_room(detected_room);
    }
    const bool room_changed = (current_room_ != active_room);
    current_room_ = active_room;

    dx = dy = 0;

    int start_px = player ? player->pos.x : 0;
    int start_py = player ? player->pos.y : 0;

    if (player) {
        player->active = true;
        if (dev_mode) {

            if (player->info) {
                player->update_scale_values();
            }
            if (!player->dead && player->anim_runtime_) {
                player->anim_runtime_->update();
            }
            player->request_child_timeline_creation_if_needed();
        } else {

            player->update();
        }
    }

    bool player_moved = false;
    if (player) {
        dx = player->pos.x - start_px;
        dy = player->pos.y - start_py;
        const bool moved_during_update = (dx != 0 || dy != 0);
        SDL_Point current_player_pos{player->pos.x, player->pos.y};
        const bool moved_since_last_frame =
            !last_player_pos_valid_ ||
            current_player_pos.x != last_known_player_pos_.x ||
            current_player_pos.y != last_known_player_pos_.y;

        last_known_player_pos_ = current_player_pos;
        last_player_pos_valid_ = true;

        player_moved = moved_during_update || moved_since_last_frame;
        if (!dev_mode && moved_during_update) {

            log_asset_movement(player,
                               SDL_Point{start_px, start_py},
                               SDL_Point{player->pos.x, player->pos.y});
        }
    } else {
        last_player_pos_valid_ = false;
    }

    rebuild_non_player_update_buffer_if_needed();

    for (Asset* asset : non_player_update_buffer_) {
        if (!asset) continue;
        SDL_Point previous_pos{asset->pos.x, asset->pos.y};
        asset->active = true;

        if (dev_mode) {

            if (asset->info) {
                asset->update_scale_values();
            }
            if (!asset->dead && asset->anim_runtime_) {
                asset->anim_runtime_->update();
            }
            asset->request_child_timeline_creation_if_needed();
        } else {

            asset->update();
            if (previous_pos.x != asset->pos.x || previous_pos.y != asset->pos.y) {
                log_asset_movement(asset,
                                   previous_pos,
                                   SDL_Point{asset->pos.x, asset->pos.y});
            }
        }
    }

    if (!movement_commands_buffer_.empty()) {
        for (const GridMovementCommand& cmd : movement_commands_buffer_) {
            if (!cmd.asset) continue;
            world_grid_.move_asset(cmd.asset, cmd.previous, cmd.current);
            cmd.asset->cache_grid_residency(SDL_Point{cmd.asset->pos.x, cmd.asset->pos.y});
        }
        movement_commands_buffer_.clear();

        moving_assets_for_grid_.clear();
        grid_registration_buffer_.clear();
        touch_dev_active_state_version();
        mark_grid_dirty();
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

    maybe_rebuild_world_grid();

    update_audio_camera_metrics();

    if (dev_controls_ && dev_controls_->is_enabled()) {
        sync_dev_controls_current_room(current_room_);
        dev_controls_->update(input);

        dev_controls_->update_ui(input);

        maybe_rebuild_world_grid();
    }

    register_pending_static_assets();
    if (process_removals()) {
        mark_active_assets_dirty();
        rebuild_active_assets_if_needed();
    }

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
}

void Assets::rebuild_non_player_update_buffer_if_needed() {
    if (!non_player_update_buffer_dirty_.load(std::memory_order_acquire)) {
        return;
    }

    // Helper to compute parent depth for topological ordering
    auto compute_parent_depth = [](const Asset* asset) -> int {
        int depth = 0;
        const Asset* current = asset;
        while (current && current->parent) {
            ++depth;
            current = current->parent;
            if (depth > 1000) break; // Prevent infinite loops from circular references
        }
        return depth;
    };

    // Collect all non-player assets (including children)
    non_player_update_buffer_.clear();
    non_player_update_buffer_.reserve(active_assets.size());
    for (Asset* asset : active_assets) {
        if (asset && asset != player) {
            non_player_update_buffer_.push_back(asset);
        }
    }

    // Sort in parent-first order (lower depth = higher in hierarchy = updated first)
    std::stable_sort(non_player_update_buffer_.begin(),
                     non_player_update_buffer_.end(),
                     [&compute_parent_depth](const Asset* a, const Asset* b) {
                         if (!a || !b) return b != nullptr;
                         const int depth_a = compute_parent_depth(a);
                         const int depth_b = compute_parent_depth(b);
                         if (depth_a != depth_b) {
                             return depth_a < depth_b; // Parents (lower depth) before children
                         }
                         return a < b; // Stable ordering for same depth
                     });

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

SDL_Rect Assets::screen_world_rect() const {
    const Area view = camera_.get_camera_area();
    auto [minx, miny, maxx, maxy] = view.get_bounds();
    SDL_Rect rect{
        minx,
        miny,
        std::max(0, maxx - minx), std::max(0, maxy - miny) };
    return rect;
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
        show_dev_notice("Dev Mode disabled", 1500);
    }

    apply_camera_runtime_settings();
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

void Assets::initialize_active_assets(SDL_Point ) {

    active_assets.clear();
    active_assets.reserve(all.size() + child_timeline_assets_.size());
    for (Asset* a : all) {
        if (a) {
            active_assets.push_back(a);
        }
    }
    for (Asset* child : child_timeline_assets_) {
        if (child) {
            active_assets.push_back(child);
        }
    }

    active_assets_dirty_.store(false, std::memory_order_release);
    mark_non_player_update_buffer_dirty();
    needs_filtered_active_refresh_ = true;
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
    auto uptr = std::make_unique<Asset>(info, spawn_area, world_pos, depth, nullptr, std::string{}, std::string{}, map_grid_settings_.spacing());
    Asset* raw = uptr.get();
    if (!raw) {
        return nullptr;
    }
    raw->set_assets(this);
    raw->set_camera(&camera_);
    raw->finalize_setup();

    raw = world_grid_.create_asset_at_point(std::move(uptr));
    all.push_back(raw);

    queue_asset_dimension_update(raw);
    mark_grid_dirty();
    mark_active_assets_dirty();
    mark_non_player_update_buffer_dirty();

    return raw;
}

void Assets::request_child_timeline_creation(Asset* parent) {
    if (!parent || parent->dead || !parent->active) {
        return;
    }
    if (parent->animation_children_.empty()) {
        return;
    }

    std::string owning_room = map_id_;
    if (current_room_) {
        owning_room = current_room_->room_name;
    }
    Area spawn_area(owning_room, 0);
    bool created_any = false;

    for (std::size_t i = 0; i < parent->animation_children_.size(); ++i) {
        const auto& slot = parent->animation_children_[i];
        if (slot.child_index < 0 || slot.asset_name.empty() || !slot.timeline) {
            continue;
        }
        std::shared_ptr<AssetInfo> info = slot.info ? slot.info : library_.get(slot.asset_name);
        if (!info) {
            continue;
        }

        int depth = parent->depth;
        int grid_res = parent->grid_resolution;
        auto uptr = std::make_unique<Asset>(info, spawn_area, parent->pos, depth, parent, std::string{}, std::string{"ChildTimeline"}, grid_res);
        Asset* raw = uptr.release();
        if (!raw) {
            continue;
        }
        raw->set_assets(this);
        raw->set_camera(&camera_);
        raw->finalize_setup();
        raw->active = true;
        raw->is_child_timeline_asset_ = true;
        raw->child_timeline_index_ = static_cast<int>(i);

        child_timeline_assets_.push_back(raw);
        active_assets.push_back(raw);
        created_any = true;
    }

    if (created_any) {
        mark_non_player_update_buffer_dirty();
        needs_filtered_active_refresh_ = true;
        touch_dev_active_state_version();
    }
}

Asset* Assets::find_child_timeline_asset(const Asset* parent, int slot_index) const {
    if (!parent || slot_index < 0) {
        return nullptr;
    }
    for (Asset* child : child_timeline_assets_) {
        if (!child || child->dead) {
            continue;
        }
        if (child->parent == parent && child->child_timeline_index() == slot_index) {
            return child;
        }
    }
    return nullptr;
}

void Assets::rebuild_from_grid_state() {
    ++frame_id_;
    for (Asset* child : child_timeline_assets_) {
        delete child;
    }
    child_timeline_assets_.clear();
    rebuild_all_assets_from_grid();
    initialize_active_assets(camera_.get_screen_center());
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
    const SDL_Point current_center = camera_.get_screen_center();
    const double current_scale = camera_.get_scale();
    const double current_pitch = camera_.current_pitch_radians();
    constexpr double kCameraGridEpsilon = 1e-4;
    const bool camera_changed =
        current_center.x != last_camera_center_for_grid_.x ||
        current_center.y != last_camera_center_for_grid_.y ||
        std::fabs(current_scale - last_camera_scale_for_grid_) > kCameraGridEpsilon ||
        std::fabs(current_pitch - last_camera_pitch_for_grid_) > kCameraGridEpsilon;

    camera_view_dirty_ = camera_view_dirty_ || camera_changed;
    if (!grid_dirty_ && !camera_view_dirty_) {
        return false;
    }

    camera_.rebuild_grid(world_grid_, last_frame_dt_seconds_);
    world_grid_.update_active_chunks(screen_world_rect(), 0);
    rebuild_active_from_screen_grid();

    grid_dirty_ = false;
    camera_view_dirty_ = false;
    last_camera_center_for_grid_ = current_center;
    last_camera_scale_for_grid_ = current_scale;
    last_camera_pitch_for_grid_ = current_pitch;
    return true;
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
    pending_initial_rebuild_ = false;
    active_assets_dirty_.store(false, std::memory_order_release);
    initialize_active_assets(camera_.get_screen_center());
    return true;
}

bool Assets::asset_bounds_in_screen_space(const Asset* asset, SDL_FRect& out_rect) const {
    if (!asset || !asset->info) {
        return false;
    }
    float world_x = asset->smoothed_translation_x();
    float world_y = asset->smoothed_translation_y();
    if (dev_mode) {
        world_x = static_cast<float>(asset->pos.x);
        world_y = static_cast<float>(asset->pos.y);
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

bool Assets::process_removals() {
    std::vector<Asset*> pending_removals;
    {
        std::lock_guard<std::mutex> lock(removal_queue_mutex_);
        if (removal_queue.empty()) {
            return false;
        }
        pending_removals.swap(removal_queue);
    }

    if (pending_removals.empty()) {
        return false;
    }

    std::vector<Asset*> active_only_removals;
    std::vector<Asset*> grid_removals;
    active_only_removals.reserve(pending_removals.size());
    grid_removals.reserve(pending_removals.size());
    for (Asset* asset : pending_removals) {
        if (!asset) {
            continue;
        }
        if (asset->is_child_timeline_asset()) {
            active_only_removals.push_back(asset);
        } else {
            grid_removals.push_back(asset);
        }
    }

    if (!active_only_removals.empty()) {
        for (Asset* asset : active_only_removals) {
            remove_asset_dimension_cache(asset);
            asset->clear_grid_residency_cache();
            child_timeline_assets_.erase(std::remove(child_timeline_assets_.begin(),
                                                     child_timeline_assets_.end(),
                                                     asset),
                                         child_timeline_assets_.end());
            active_assets.erase(std::remove(active_assets.begin(), active_assets.end(), asset), active_assets.end());
            filtered_active_assets.erase(std::remove(filtered_active_assets.begin(), filtered_active_assets.end(), asset), filtered_active_assets.end());
            delete asset;
        }
        mark_non_player_update_buffer_dirty();
        needs_filtered_active_refresh_ = true;
        touch_dev_active_state_version();
    }

    if (grid_removals.empty()) {
        return !active_only_removals.empty();
    }

    for (Asset* asset : grid_removals) {
        if (!asset) {
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

    return true;
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
            SDL_RenderDrawRect(renderer, &r);
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
                SDL_RenderDrawRect(renderer, &draw_rect);
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

    const SDL_Point world_pos{ asset->pos.x, asset->pos.y };
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
        if (world_grid_.point_for_asset(asset)) {
            asset->cache_grid_residency(SDL_Point{asset->pos.x, asset->pos.y});
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
void Assets::rebuild_active_from_screen_grid() {
    const std::uint32_t current_frame_id = frame_id_;
    const std::uint32_t previous_active_frame_id = last_active_rebuild_frame_id_;
    bool active_changed = false;
    std::vector<Asset*> newly_active_assets;
    newly_active_assets.reserve(active_assets.size());

    active_points_.clear();

    visible_candidate_buffer_.clear();
    visible_candidate_buffer_.reserve(camera_.get_visible_points().size() * 2);

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
        active_points_.push_back(point);
        for (const auto& occ : point->occupants) {
            Asset* asset = occ.get();
            if (asset && asset->last_visible_frame_id != current_frame_id) {
                asset->last_visible_frame_id = current_frame_id;
                visible_candidate_buffer_.push_back(asset);
            }
        }
    }

    const double anchor_world_y = camera_.anchor_world_y();
    const auto distance_from_anchor = [anchor_world_y](const Asset* asset) {
        if (!asset) {
            return std::numeric_limits<double>::infinity();
        }
        return anchor_world_y - static_cast<double>(asset->pos.y);
    };
    auto depth_order = [&distance_from_anchor](Asset* lhs, Asset* rhs) {
        if (lhs == rhs) {
            return false;
        }
        if (!lhs || !rhs) {
            return rhs != nullptr;
        }
        const double ld = distance_from_anchor(lhs);
        const double rd = distance_from_anchor(rhs);
        if (ld != rd) {
            return ld > rd; // draw farther (larger Y-distance) first
        }
        return lhs < rhs;
    };

    auto parent_is_active = [&](Asset* asset) -> bool {
        if (!asset) {
            return false;
        }
        if (!asset->parent) {
            return true;
        }
        return asset->parent->last_active_frame_id == current_frame_id;
    };

    active_assets.erase(std::remove_if(active_assets.begin(),
                                       active_assets.end(),
                                       [&](Asset* asset) {
                                           if (!asset) {
                                               return true;
                                           }
                                           if (asset->dead) {
                                               return true;
                                           }
                                           if (asset->is_child_timeline_asset()) {
                                               const bool was_active = asset->last_active_frame_id == previous_active_frame_id;
                                               if (!was_active) {
                                                   newly_active_assets.push_back(asset);
                                                   active_changed = true;
                                               }
                                               asset->last_active_frame_id = current_frame_id;
                                               asset->active = true;
                                               return false;
                                           }
                                           const bool is_visible = asset->last_visible_frame_id == current_frame_id;
                                           const bool parent_active = parent_is_active(asset);
                                           if (!is_visible || !parent_active) {
                                               if (asset->last_active_frame_id == previous_active_frame_id) {
                                                   active_changed = true;
                                               }
                                               asset->active = false;
                                               asset->child_creation_requested_ = false;
                                               return true;
                                           }
                                           const bool was_active = asset->last_active_frame_id == previous_active_frame_id;
                                           if (!was_active) {
                                               newly_active_assets.push_back(asset);
                                               active_changed = true;
                                           }
                                           asset->last_active_frame_id = current_frame_id;
                                           asset->active = true;
                                           return false;
                                       }),
                        active_assets.end());

    for (Asset* asset : visible_candidate_buffer_) {
        if (!asset) {
            continue;
        }
        if (!parent_is_active(asset)) {
            continue;
        }
        if (asset->last_active_frame_id == current_frame_id) {
            continue;
        }
        const bool was_active = asset->last_active_frame_id == previous_active_frame_id;
        auto insert_pos = std::lower_bound(active_assets.begin(), active_assets.end(), asset, depth_order);
        active_assets.insert(insert_pos, asset);
        asset->last_active_frame_id = current_frame_id;
        asset->last_visible_frame_id = current_frame_id;
        asset->active = true;
        if (!was_active) {
            newly_active_assets.push_back(asset);
        }
        active_changed = true;
    }

    if (!std::is_sorted(active_assets.begin(), active_assets.end(), depth_order)) {
        std::sort(active_assets.begin(), active_assets.end(), depth_order);
    }

    visible_candidate_buffer_.clear();

    active_assets_dirty_.store(false, std::memory_order_release);
    if (active_changed) {
        mark_non_player_update_buffer_dirty();
        needs_filtered_active_refresh_ = true;
    }

    for (Asset* asset : newly_active_assets) {
        if (!asset) {
            continue;
        }
        asset->update_scale_values();
    }

    last_active_rebuild_frame_id_ = current_frame_id;
}
