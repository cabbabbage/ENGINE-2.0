#include "room_editor.hpp"
#include "utils/sdl_render_conversions.hpp"
#include "utils/sdl_mouse_utils.hpp"
#include "utils/ttf_render_utils.hpp"

#include <algorithm>
#include <cmath>

#include "assets/asset/Asset.hpp"
#include "assets/asset/asset_info.hpp"
#include "assets/asset/asset_types.hpp"
#include "assets/asset/asset_utils.hpp"
#include "core/AssetsManager.hpp"
#include "devtools/asset_info_ui.hpp"
#include "map_layers_common.hpp"
#include "devtools/asset_library_ui.hpp"
#include "devtools/room_anchor_mode_utils.hpp"
#include "devtools/room_anchor_tools_panel.hpp"
#include "devtools/core/manifest_store.hpp"
#include "devtools/core/dev_edit_transaction.hpp"
#include "devtools/core/dev_save_coordinator.hpp"
#include "devtools/core/save_manager.hpp"
#include "devtools/DockableCollapsible.hpp"
#include "devtools/draw_utils.hpp"
#include "dev_mode_color_utils.hpp"
#include "spawn_groups/SpawnGroupConfig.hpp"
#include "spawn_groups/spawn_group_utils.hpp"
#include "devtools/dev_footer_bar.hpp"
#include "config/room_config/room_configurator.hpp"
#include "DockManager.hpp"
#include "devtools/widgets.hpp"
#include "dm_styles.hpp"
#include "room_overlay_renderer.hpp"
#include "animation/animation_update.hpp"
#include "rendering/render/projected_sprite_frame.hpp"
#include "rendering/render/render_object_projection.hpp"
#include "rendering/render/warped_screen_grid.hpp"
#include "core/axis_convention.hpp"
#include "gameplay/map_generation/room.hpp"
#include "gameplay/spawn/asset_spawn_planner.hpp"
#include "gameplay/spawn/asset_spawner.hpp"
#include "gameplay/spawn/check.hpp"
#include "gameplay/spawn/methods/spawn_method.hpp"
#include "gameplay/spawn/spawn_context.hpp"
#include "utils/input.hpp"
#include "utils/grid.hpp"
#include "utils/grid_occupancy.hpp"
#include "utils/map_grid_settings.hpp"
#include "utils/AnchorPointResolver.hpp"
#include "utils/relative_room_position.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <optional>
#include <iostream>
#include <cctype>
#include <limits>
#include <random>
#include <tuple>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <fstream>
#include <SDL3/SDL_log.h>

namespace {
constexpr double kPi = 3.14159265358979323846;
constexpr int kSavedCameraHeightMinPx = 1;
constexpr float kSavedCameraTiltMinDeg = 0.0f;
constexpr float kSavedCameraTiltMaxDeg = WarpedScreenGrid::kMaxPitchDegrees;
constexpr int kSavedCameraZoomMinPercent = 0;
constexpr int kSavedCameraZoomMaxPercent = 100;
constexpr float kShiftEdgePanThresholdFraction = 0.008f;
constexpr float kShiftEdgePanExponent = 1.5f;
constexpr float kShiftEdgePanBottomSampleInset = 0.92f;
constexpr float kShiftEdgePanMaxSpeedBottomWorldUnitsPerSecond = 1400.0f;
SDL_Point snap_world_point_to_overlay_grid(SDL_Point world, int resolution) {
    MapGridSettings settings;
    settings.grid_resolution = resolution;
    const int spacing = settings.spacing();
    if (spacing <= 0) {
        return world;
    }
    auto snap_axis = [spacing](int value) -> int {
        const long double ratio = static_cast<long double>(value) / static_cast<long double>(spacing);
        const long long rounded = static_cast<long long>(std::llround(ratio));
        const long long scaled = rounded * static_cast<long long>(spacing);
        if (scaled > std::numeric_limits<int>::max()) {
            return std::numeric_limits<int>::max();
        }
        if (scaled < std::numeric_limits<int>::min()) {
            return std::numeric_limits<int>::min();
        }
        return static_cast<int>(scaled);
    };
    return SDL_Point{ snap_axis(world.x), snap_axis(world.y) };
}

SDL_Point grid_point_for_asset(const Asset* asset) {
    if (!asset) {
        return SDL_Point{0, 0};
    }
    if (asset->grid_point()) {
        return asset->grid_point()->to_sdl_point();
    }
    return asset->world_xz_point();
}

SDL_Point grid_point_for_screen(const WarpedScreenGrid& cam, SDL_Point screen_point) {
    const SDL_FPoint world_f = cam.screen_to_map(screen_point);
    return SDL_Point{
        static_cast<int>(std::lround(world_f.x)),
        static_cast<int>(std::lround(world_f.y))
    };
}

bool build_render_object_projection(const WarpedScreenGrid& cam,
                                    const RenderObject& obj,
                                    float perspective_scale,
                                    float world_z,
                                    render_projection::ProjectedSpriteFrame& out_projection) {
    return render_projection::build_render_object_projected_frame(
        cam, obj, perspective_scale, world_z, out_projection);
}

void render_grid_point_marker(SDL_Renderer* renderer, const WarpedScreenGrid& cam, SDL_Point world_pt, SDL_Color color, int radius_px = 3) {
    if (!renderer) return;
    SDL_FPoint screen_f = cam.map_to_screen(world_pt);
    if (!std::isfinite(screen_f.x) || !std::isfinite(screen_f.y)) {
        return;
    }
    const int sx = static_cast<int>(std::lround(screen_f.x));
    const int sy = static_cast<int>(std::lround(screen_f.y));

    SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
    SDL_RenderPoint(renderer, sx, sy);
    for (int dx = -radius_px; dx <= radius_px; ++dx) {
        SDL_RenderPoint(renderer, sx + dx, sy);
    }
    for (int dy = -radius_px; dy <= radius_px; ++dy) {
        SDL_RenderPoint(renderer, sx, sy + dy);
    }
}

}
#include <nlohmann/json.hpp>
#include "utils/log.hpp"

namespace devmode::room_editor_detail {

nlohmann::json resolve_map_info_blob(const Assets* assets,
                                     const devmode::core::ManifestStore* manifest_store,
                                     const std::string& map_id) {
    if (assets) {
        const nlohmann::json& in_memory = assets->map_info_json();
        if (in_memory.is_object()) {
            return in_memory;
        }
    }

    if (manifest_store && !map_id.empty()) {
        if (const nlohmann::json* entry = manifest_store->find_map_entry(map_id)) {
            if (entry->is_null()) {
                return nlohmann::json::object();
            }
            return *entry;
        }
    }
    return nlohmann::json::object();
}

} // namespace devmode::room_editor_detail

using devmode::spawn::ensure_spawn_groups_array;
using devmode::spawn::find_spawn_groups_array;
using devmode::spawn::generate_spawn_id;

namespace {

constexpr int kClipboardNudge = 16;
constexpr float kCameraScaleEpsilon = 1e-4f;
constexpr double kCameraProjectionEpsilon = 1e-3;
constexpr int kCameraHeightScrollStep = 25;
constexpr int kCameraZoomScrollStep = 5;
constexpr float kCameraTiltDegreesPerPixel = 0.2f;
constexpr float kCameraPanPercentPerPixel = 0.2f;
constexpr std::int64_t kMaxSpatialCellsPerAsset = 4096;
constexpr int kMaxScreenCoordMagnitude = 1 << 20;
constexpr int kAnchorHandlePickRadiusPx = 12;
constexpr int kAnchorDragIterations = 3;
constexpr int kAnchorUiTopMargin = 16;
constexpr int kAnchorUiEdgeMargin = 16;
constexpr int kAnchorNavButtonSize = 30;
constexpr int kAnchorNavGap = 6;

int floor_div(int value, int divisor) {
    if (divisor == 0) {
        return 0;
    }
    if (value >= 0) {
        return value / divisor;
    }
    return (value - divisor + 1) / divisor;
}

std::int64_t floor_div_i64(std::int64_t value, int divisor) {
    if (divisor == 0) {
        return 0;
    }
    if (value >= 0) {
        return value / divisor;
    }
    return (value - divisor + 1) / divisor;
}

bool screen_rect_is_reasonable(const SDL_Rect& rect) {
    if (rect.w <= 0 || rect.h <= 0) {
        return false;
    }
    const auto abs_within_limit = [](int value) {
        return std::abs(static_cast<long long>(value)) <= static_cast<long long>(kMaxScreenCoordMagnitude);
    };
    if (!abs_within_limit(rect.x) || !abs_within_limit(rect.y)) {
        return false;
    }
    const std::int64_t right = static_cast<std::int64_t>(rect.x) + static_cast<std::int64_t>(rect.w);
    const std::int64_t bottom = static_cast<std::int64_t>(rect.y) + static_cast<std::int64_t>(rect.h);
    if (right < std::numeric_limits<int>::min() || right > std::numeric_limits<int>::max()) {
        return false;
    }
    if (bottom < std::numeric_limits<int>::min() || bottom > std::numeric_limits<int>::max()) {
        return false;
    }
    return true;
}

int64_t make_cell_key(int cell_x, int cell_y) {
    return (static_cast<int64_t>(cell_x) << 32) ^ static_cast<uint32_t>(cell_y);
}

std::string trim_copy_room_editor(const std::string& input) {
    auto is_space = [](unsigned char ch) { return std::isspace(ch) != 0; };
    std::string result = input;
    result.erase(result.begin(), std::find_if(result.begin(), result.end(), [&](unsigned char ch) {
        return !is_space(ch);
    }));
    result.erase(std::find_if(result.rbegin(), result.rend(), [&](unsigned char ch) {
        return !is_space(ch);
    }).base(), result.end());
    return result;
}

SDL_Point resolve_anchor_editor_frame_dimensions(const Asset* asset, const AnimationFrame* frame) {
    int frame_w = 0;
    int frame_h = 0;

    if (frame && !frame->variants.empty()) {
        const int variant_index = asset
            ? std::clamp(asset->current_variant_index, 0, static_cast<int>(frame->variants.size()) - 1)
            : 0;
        const FrameVariant& variant = frame->variants[static_cast<std::size_t>(variant_index)];
        if (variant.source_rect.w > 0 && variant.source_rect.h > 0) {
            frame_w = variant.source_rect.w;
            frame_h = variant.source_rect.h;
        } else {
            float tex_w = 0.0f;
            float tex_h = 0.0f;
            if (SDL_Texture* texture = variant.get_base_texture()) {
                if (SDL_GetTextureSize(texture, &tex_w, &tex_h)) {
                    frame_w = static_cast<int>(std::lround(tex_w));
                    frame_h = static_cast<int>(std::lround(tex_h));
                }
            }
        }
    }

    if ((frame_w <= 0 || frame_h <= 0) && asset) {
        if (SDL_Texture* texture = asset->get_current_frame()) {
            float tex_w = 0.0f;
            float tex_h = 0.0f;
            if (SDL_GetTextureSize(texture, &tex_w, &tex_h)) {
                frame_w = static_cast<int>(std::lround(tex_w));
                frame_h = static_cast<int>(std::lround(tex_h));
            }
        }
    }

    if ((frame_w <= 0 || frame_h <= 0) && asset && asset->info) {
        frame_w = std::max(frame_w, asset->info->original_canvas_width);
        frame_h = std::max(frame_h, asset->info->original_canvas_height);
    }

    return SDL_Point{std::max(1, frame_w), std::max(1, frame_h)};
}

std::vector<std::string> anchor_names_for_frame(const AnimationFrame* frame) {
    std::vector<std::string> names;
    if (!frame) {
        return names;
    }
    names.reserve(frame->anchor_points.size());
    for (const auto& anchor : frame->anchor_points) {
        if (anchor.is_valid()) {
            names.push_back(anchor.name);
        }
    }
    return names;
}

static bool is_visible_pixel_at(SDL_Renderer* renderer, SDL_Point screen_point) {
    if (!renderer) return true;

    Uint32 pixel = 0;
    SDL_Rect r{ screen_point.x, screen_point.y, 1, 1 };

    SDL_PixelFormat fmt = SDL_PIXELFORMAT_RGBA8888;

    if (SDL_Surface* captured = SDL_RenderReadPixels(renderer, &r)) {
        SDL_Surface* working = captured;
        if (captured->format != fmt) {
            working = SDL_ConvertSurface(captured, fmt);
            SDL_DestroySurface(captured);
            captured = nullptr;
        }
        if (working && working->pixels) {
            pixel = *static_cast<const Uint32*>(working->pixels);
            SDL_DestroySurface(working);
        } else {
            if (working) SDL_DestroySurface(working);
            return true;
        }
    } else {
        return true;
    }

    Uint8 a = 255;
    const SDL_PixelFormatDetails* pf = SDL_GetPixelFormatDetails(fmt);
    if (pf) {
        Uint8 r8, g8, b8;
        SDL_GetRGBA(pixel, pf, nullptr, &r8, &g8, &b8, &a);
    }
    return a > 0;
}

std::string sanitize_room_key_local(const std::string& input) {
    std::string out;
    out.reserve(input.size());
    bool last_underscore = false;
    for (char ch : input) {
        unsigned char uch = static_cast<unsigned char>(ch);
        if (std::isalnum(uch)) {
            out.push_back(static_cast<char>(std::tolower(uch)));
            last_underscore = false;
        } else if (ch == '_' || ch == '-') {
            if (!last_underscore && !out.empty()) {
                out.push_back('_');
                last_underscore = true;
            }
        } else if (std::isspace(uch)) {
            if (!last_underscore && !out.empty()) {
                out.push_back('_');
                last_underscore = true;
            }
        }
    }
    while (!out.empty() && out.back() == '_') {
        out.pop_back();
    }
    if (out.empty()) {
        out = "room";
    }
    return out;
}

std::string make_unique_room_key_excluding(const nlohmann::json& rooms_data,
                                           const std::string& base_key,
                                           const std::string& exclude_key) {
    std::string base = base_key.empty() ? std::string("room") : base_key;
    std::string candidate = base;
    int suffix = 1;
    while (rooms_data.is_object() && rooms_data.contains(candidate) && candidate != exclude_key) {
        candidate = base + "_" + std::to_string(suffix++);
    }
    return candidate;
}

nlohmann::json* find_spawn_entry_in_array(nlohmann::json& array, const std::string& spawn_id) {
    if (!array.is_array()) {
        return nullptr;
    }
    for (auto& entry : array) {
        if (!entry.is_object()) {
            continue;
        }
        auto id_it = entry.find("spawn_id");
        if (id_it != entry.end() && id_it->is_string() && id_it->get<std::string>() == spawn_id) {
            return &entry;
        }
    }
    return nullptr;
}

nlohmann::json* find_spawn_entry_recursive(nlohmann::json& node,
                                          const std::string& spawn_id,
                                          nlohmann::json** owner_array) {
    if (node.is_object()) {
        for (auto it = node.begin(); it != node.end(); ++it) {
            if (it.key() == "spawn_groups" && it->is_array()) {
                if (nlohmann::json* entry = find_spawn_entry_in_array(*it, spawn_id)) {
                    if (owner_array) {
                        *owner_array = &(*it);
                    }
                    return entry;
                }
            }
            if (it.key() == "spawn_groups") {
                continue;
            }
            if (nlohmann::json* nested = find_spawn_entry_recursive(it.value(), spawn_id, owner_array)) {
                return nested;
            }
        }
    } else if (node.is_array()) {
        for (auto& element : node) {
            if (nlohmann::json* nested = find_spawn_entry_recursive(element, spawn_id, owner_array)) {
                return nested;
            }
        }
    }
    return nullptr;
}

std::optional<double> ray_segment_distance(SDL_Point origin,
                                           SDL_FPoint direction,
                                           const SDL_Point& a,
                                           const SDL_Point& b) {
    SDL_FPoint segment{static_cast<float>(b.x - a.x), static_cast<float>(b.y - a.y)};
    SDL_FPoint offset{static_cast<float>(a.x - origin.x), static_cast<float>(a.y - origin.y)};

    double denom = static_cast<double>(direction.x) * segment.y - static_cast<double>(direction.y) * segment.x;
    if (std::fabs(denom) < 1e-6) {
        return std::nullopt;
    }

    double t = (offset.x * segment.y - offset.y * segment.x) / denom;
    double u = (offset.x * direction.y - offset.y * direction.x) / denom;
    if (t < 0.0 || u < 0.0 || u > 1.0) {
        return std::nullopt;
    }

    double dir_length = std::hypot(static_cast<double>(direction.x), static_cast<double>(direction.y));
    if (dir_length <= 1e-9) {
        return std::nullopt;
    }

    return t * dir_length;
}

void room_editor_trace(const std::string& message) {
    try {
        vibble::log::debug(std::string{"[RoomEditor] "} + message);
    } catch (...) {}
}

}

RoomEditor::RoomEditor(Assets* owner, int screen_w, int screen_h)
    : assets_(owner), screen_w_(screen_w), screen_h_(screen_h) {
    update_room_config_bounds();
    rebuild_room_spawn_id_cache();
    ensure_anchor_editor_widgets();
    update_anchor_editor_layout();
}

RoomEditor::~RoomEditor() {
    release_label_font();
    invalidate_all_room_labels();
    label_cache_.clear();
}

void RoomEditor::set_room_assets_saved_callback(RoomAssetsSavedCallback cb) {
    room_assets_saved_callback_ = std::move(cb);
}

void RoomEditor::notify_room_assets_saved() {
    if (room_assets_saved_callback_) {
        room_assets_saved_callback_();
    }
}

bool RoomEditor::enqueue_current_room_save(devmode::core::DevSaveCoordinator::Priority priority) {
    if (!current_room_) {
        return false;
    }

    current_room_->mark_dirty();
    if (mark_map_dirty_callback_) {
        mark_map_dirty_callback_(priority);
    }
    notify_room_assets_saved();
    return true;
}

bool RoomEditor::save_current_room_assets_json(devmode::core::DevSaveCoordinator::Priority priority) {
    if (!current_room_) {
        return false;
    }
    if (info_ui_ && info_ui_->is_locked()) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "[RoomEditor] Asset info panel is locked; save skipped.");
        return false;
    }
    if (room_cfg_ui_ && room_cfg_ui_->is_locked()) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "[RoomEditor] Room configurator is locked; save skipped.");
        return false;
    }

    return enqueue_current_room_save(priority);
}

bool RoomEditor::validate_room_edit_invariants(std::string* error) {
    if (!current_room_) {
        if (error) *error = "no active room";
        return false;
    }

    auto& root = current_room_->assets_data();
    auto& groups = ensure_spawn_groups_array(root);
    std::unordered_set<std::string> seen_ids;
    for (const auto& entry : groups) {
        if (!entry.is_object()) continue;
        const std::string id = entry.value("spawn_id", std::string{});
        if (id.empty()) {
            if (error) *error = "spawn group missing spawn_id";
            return false;
        }
        if (!seen_ids.insert(id).second) {
            if (error) *error = std::string("duplicate spawn_id: ") + id;
            return false;
        }
        if (!entry.contains("display_name") || !entry["display_name"].is_string()) {
            if (error) *error = std::string("spawn group missing display_name: ") + id;
            return false;
        }
    }

    if (active_spawn_group_id_ && !is_room_spawn_id(*active_spawn_group_id_)) {
        if (error) *error = "active selection points to missing spawn group";
        return false;
    }

    return true;
}

bool RoomEditor::commit_room_edit_transaction(const std::function<bool()>& mutate,
                                              const std::string& action_label,
                                              bool refresh_ui_on_success,
                                              devmode::core::DevSaveCoordinator::Priority save_priority) {
    if (!current_room_) {
        return false;
    }

    const auto selection_before = active_spawn_group_id_;
    devmode::core::DevEditTransaction tx(current_room_->assets_data(), room_assets_edit_version_, action_label);
    devmode::core::DevEditTransaction::Hooks hooks;
    hooks.mutate = mutate;
    hooks.validate = [this](const nlohmann::json&) {
        std::string error;
        const bool valid = validate_room_edit_invariants(&error);
        if (!valid) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "[RoomEditor] Invariant failed: %s", error.c_str());
        }
        return valid;
    };
    hooks.commit = [this, save_priority]() { return save_current_room_assets_json(save_priority); };
    hooks.on_commit_success = [this, refresh_ui_on_success]() {
        rebuild_room_spawn_id_cache();
        if (refresh_ui_on_success) {
            refresh_spawn_group_config_ui();
            reopen_room_configurator();
        }
    };
    hooks.on_rollback = [this, selection_before, action_label]() {
        active_spawn_group_id_ = selection_before;
        rebuild_room_spawn_id_cache();
        refresh_spawn_group_config_ui();
        reopen_room_configurator();
        show_notice("Failed to save room assets; " + action_label + " canceled.");
    };
    return tx.run(hooks);
}

void RoomEditor::mark_map_dirty_for_spawn_groups(
    devmode::core::DevSaveCoordinator::Priority priority) {
    if (mark_map_dirty_callback_) {
        mark_map_dirty_callback_(priority);
        return;
    }
    if (assets_) {
        assets_->mark_map_data_dirty();
    }
}

void RoomEditor::copy_selected_spawn_group() {
    auto spawn_id_opt = selected_spawn_group_id();
    if (!spawn_id_opt) {
        show_notice("Select a room spawn group to copy.");
        return;
    }

    const std::string& spawn_id = *spawn_id_opt;
    SpawnEntryResolution resolved = locate_spawn_entry(spawn_id);
    if (!resolved.valid() || resolved.source != SpawnEntryResolution::Source::Room || !resolved.entry) {
        show_notice("Map-wide spawn groups cannot be copied.");
        return;
    }

    if (spawn_group_is_boundary(spawn_id)) {
        show_notice("Boundary spawn groups cannot be copied.");
        return;
    }

    spawn_group_clipboard_.emplace();
    spawn_group_clipboard_->entry = *resolved.entry;
    spawn_group_clipboard_->entry.erase("priority");
    const std::string display_name = spawn_group_clipboard_->entry.value("display_name", std::string{"Spawn Group"});
    std::string base = strip_copy_suffix(display_name);
    if (base.empty()) {
        base = display_name;
    }
    if (base.empty()) {
        base = "Spawn Group";
    }
    spawn_group_clipboard_->base_display_name = std::move(base);
    spawn_group_clipboard_->paste_count = 0;

    if (!display_name.empty()) {
        show_notice(std::string("Copied spawn group \"") + display_name + "\"");
    } else {
        show_notice("Copied spawn group to clipboard");
    }
}

void RoomEditor::paste_spawn_group_from_clipboard() {
    if (!spawn_group_clipboard_) {
        show_notice("Clipboard is empty. Copy a spawn group first.");
        return;
    }

    Room* target_room = resolve_room_for_clipboard_action();
    if (!target_room || !target_room->room_area) {
        show_notice("No valid room available for paste.");
        return;
    }

    if (target_room != current_room_) {
        if (assets_) {
            assets_->set_editor_current_room(target_room);
        } else {
            set_current_room(target_room);
        }
    }

    if (!current_room_) {
        show_notice("Unable to determine active room for paste.");
        return;
    }

    auto& root = current_room_->assets_data();
    auto& groups = ensure_spawn_groups_array(root);

    nlohmann::json entry = spawn_group_clipboard_->entry;
    const std::string new_id = generate_spawn_id();
    entry["spawn_id"] = new_id;
    const std::string next_name = next_clipboard_display_name();
    if (!next_name.empty()) {
        entry["display_name"] = next_name;
    }

    const std::string display_name = entry.value("display_name", std::string{"Spawn Group"});
    const int default_resolution = current_room_ ? current_room_->map_grid_settings().grid_resolution : MapGridSettings::defaults().grid_resolution;
    devmode::spawn::ensure_spawn_group_entry_defaults(entry, display_name, default_resolution);
    remap_clipboard_entry_to_room(entry, current_room_);

    groups.push_back(entry);
    for (size_t i = 0; i < groups.size(); ++i) {
        if (groups[i].is_object()) {
            groups[i]["priority"] = static_cast<int>(i);
        }
    }

    sanitize_perimeter_spawn_groups(groups);
    save_current_room_assets_json();
    rebuild_room_spawn_id_cache();
    refresh_spawn_group_config_ui();
    reopen_room_configurator();

    nlohmann::json& inserted = groups.back();
    respawn_spawn_group(inserted);

    active_spawn_group_id_ = new_id;
    select_spawn_group_assets(new_id);

    if (!display_name.empty()) {
        show_notice(std::string("Pasted spawn group \"") + display_name + "\"");
    } else {
        show_notice("Pasted spawn group");
    }
}

std::optional<std::string> RoomEditor::selected_spawn_group_id() const {
    if (selected_assets_.empty()) {
        return std::nullopt;
    }
    std::optional<std::string> result;
    for (Asset* asset : selected_assets_) {
        if (!asset) {
            continue;
        }
        if (!asset_belongs_to_room(asset)) {
            continue;
        }
        if (asset->spawn_id.empty()) {
            return std::nullopt;
        }
        if (!result) {
            result = asset->spawn_id;
        } else if (*result != asset->spawn_id) {
            return std::nullopt;
        }
    }
    if (!result || result->empty()) {
        return std::nullopt;
    }
    return result;
}

bool RoomEditor::spawn_group_is_boundary(const std::string& spawn_id) const {
    if (spawn_id.empty() || !assets_) {
        return false;
    }
    for (Asset* asset : assets_->all) {
        if (!asset || asset->dead) {
            continue;
        }
        if (!asset_belongs_to_room(asset)) {
            continue;
        }
        if (asset->spawn_id == spawn_id) {
            if (asset->info && asset->info->type == asset_types::boundary) {
                return true;
            }
        }
    }
    return false;
}

Room* RoomEditor::resolve_room_for_clipboard_action() const {
    const Assets* assets = assets_;
    if (!assets) {
        return current_room_;
    }
    if (!input_) {
        return current_room_;
    }

    SDL_Point screen{input_->getX(), input_->getY()};
    SDL_Point world = screen;
    if (auto mapped_point = input_->screen_to_world(screen)) {
        world = *mapped_point;
    } else {
        SDL_FPoint mapped = assets->getView().screen_to_map(screen);
        world = SDL_Point{static_cast<int>(std::lround(mapped.x)), static_cast<int>(std::lround(mapped.y))};
    }

    if (current_room_ && current_room_->room_area && current_room_->room_area->contains_point(world)) {
        return current_room_;
    }

    for (Room* room : assets->rooms()) {
        if (!room || !room->room_area) {
            continue;
        }
        if (room->room_area->contains_point(world)) {
            return room;
        }
    }
    return current_room_;
}

void RoomEditor::select_spawn_group_assets(const std::string& spawn_id) {
    const std::vector<Asset*> previous_selection = selected_assets_;
    selected_assets_.clear();
    auto selection_changed = [&]() {
        if (previous_selection.size() != selected_assets_.size()) {
            return true;
        }
        return !std::equal(selected_assets_.begin(), selected_assets_.end(), previous_selection.begin());
};
    if (spawn_id.empty()) {
        sync_spawn_group_panel_with_selection();
        if (selection_changed()) {
            mark_highlight_dirty();
        }
        update_highlighted_assets();
        return;
    }

    if (spawn_group_locked(spawn_id)) {
        sync_spawn_group_panel_with_selection();
        if (selection_changed()) {
            mark_highlight_dirty();
        }
        update_highlighted_assets();
        return;
    }
    if (!assets_) {
        sync_spawn_group_panel_with_selection();
        if (selection_changed()) {
            mark_highlight_dirty();
        }
        update_highlighted_assets();
        return;
    }

    for (Asset* asset : assets_->all) {
        if (!asset || asset->dead) {
            continue;
        }
        if (!asset_belongs_to_room(asset)) {
            continue;
        }
        if (asset->spawn_id == spawn_id) {
            selected_assets_.push_back(asset);
        }
    }

    sync_spawn_group_panel_with_selection();
    if (selection_changed()) {
        mark_highlight_dirty();
    }
    update_highlighted_assets();
}

void RoomEditor::remap_clipboard_entry_to_room(nlohmann::json& entry, Room* room) {
    if (!room || !room->room_area) {
        return;
    }

    auto bounds = room->room_area->get_bounds();
    const int width = std::max(1, std::get<2>(bounds) - std::get<0>(bounds));
    const int height = std::max(1, std::get<3>(bounds) - std::get<1>(bounds));

    std::string method = entry.value("position", std::string{});
    if (method == "Exact Position") {
        method = "Exact";
    }

    if (method == "Exact" || method == "Perimeter") {
        int stored_dx = entry.value("dx", 0);
        int stored_dz = entry.value("dz", 0);
        int orig_w = std::max(1, entry.value("origional_width", width));
        int orig_h = std::max(1, entry.value("origional_height", height));
        RelativeRoomPosition relative(SDL_Point{stored_dx, stored_dz}, orig_w, orig_h);
        SDL_Point scaled = relative.scaled_offset(width, height);
        entry["dx"] = scaled.x;
        entry["dz"] = scaled.y;
        entry["origional_width"] = width;
        entry["origional_height"] = height;
        ensure_clipboard_position_is_valid(entry, room);
    } else if (method == "Percent") {
        entry["origional_width"] = width;
        entry["origional_height"] = height;
    }
}

void RoomEditor::ensure_clipboard_position_is_valid(nlohmann::json& entry, Room* room) {
    if (!room || !room->room_area) {
        return;
    }

    std::string method = entry.value("position", std::string{});
    if (method == "Exact Position") {
        method = "Exact";
    }
    if (method != "Exact" && method != "Perimeter") {
        return;
    }

    SDL_Point center = room->room_area->get_center();
    int dx = entry.value("dx", 0);
    int dz = entry.value("dz", 0);
    SDL_Point candidate{center.x + dx, center.y + dz};
    if (room->room_area->contains_point(candidate)) {
        return;
    }

    const std::array<SDL_Point, 8> adjustments{{
        SDL_Point{kClipboardNudge, 0},
        SDL_Point{-kClipboardNudge, 0},
        SDL_Point{0, kClipboardNudge},
        SDL_Point{0, -kClipboardNudge},
        SDL_Point{kClipboardNudge, kClipboardNudge},
        SDL_Point{kClipboardNudge, -kClipboardNudge},
        SDL_Point{-kClipboardNudge, kClipboardNudge},
        SDL_Point{-kClipboardNudge, -kClipboardNudge},
    }};

    for (const SDL_Point& delta : adjustments) {
        SDL_Point test{candidate.x + delta.x, candidate.y + delta.y};
        if (room->room_area->contains_point(test)) {
            entry["dx"] = test.x - center.x;
            entry["dz"] = test.y - center.y;
            return;
        }
    }

    entry["dx"] = 0;
    entry["dz"] = 0;
}

std::string RoomEditor::strip_copy_suffix(const std::string& name) {
    if (name.empty()) {
        return name;
    }
    const std::string marker = " (Copy";
    const size_t pos = name.rfind(marker);
    if (pos == std::string::npos) {
        return name;
    }
    if (name.back() != ')') {
        return name;
    }
    const std::string inside = name.substr(pos + 2, name.size() - pos - 3);
    if (inside == "Copy") {
        return name.substr(0, pos);
    }
    const std::string prefix = "Copy ";
    if (inside.rfind(prefix) == 0) {
        const bool digits = std::all_of(inside.begin() + prefix.size(), inside.end(), [](unsigned char ch) {
            return std::isdigit(ch) != 0;
        });
        if (digits) {
            return name.substr(0, pos);
        }
    }
    return name;
}

std::string RoomEditor::next_clipboard_display_name() {
    if (!spawn_group_clipboard_) {
        return {};
    }
    ++spawn_group_clipboard_->paste_count;
    std::string base = spawn_group_clipboard_->base_display_name;
    if (base.empty()) {
        base = "Spawn Group";
    }
    if (spawn_group_clipboard_->paste_count == 1) {
        return base + " (Copy)";
    }
    return base + " (Copy " + std::to_string(spawn_group_clipboard_->paste_count) + ")";
}

void RoomEditor::show_notice(const std::string& message) const {
    if (!assets_) {
        return;
    }
    assets_->show_dev_notice(message);
}

void RoomEditor::mark_highlight_dirty() {
    highlight_dirty_ = true;
}

void RoomEditor::set_input(Input* input) {
    input_ = input;
}

void RoomEditor::set_player(Asset* player) {
    player_ = player;
    mark_spatial_index_dirty();
}

void RoomEditor::set_active_assets(std::vector<Asset*>& actives, std::uint64_t generation) {
    const bool pointer_changed = active_assets_ != &actives;
    active_assets_ = &actives;
    if (pointer_changed || active_assets_version_ != generation) {
        active_assets_version_ = generation;
        mark_highlight_dirty();
        mark_spatial_index_dirty();
    }
}

void RoomEditor::set_screen_dimensions(int width, int height) {
    screen_w_ = width;
    screen_h_ = height;
    update_room_config_bounds();
    if (room_cfg_ui_ && room_config_dock_open_) {
        room_cfg_ui_->set_bounds(room_config_bounds_);
    }
    configure_shared_panel();
    refresh_room_config_visibility();

    if (spawn_group_panel_) {
        spawn_group_panel_->set_screen_dimensions(screen_w_, screen_h_);

        spawn_group_panel_->set_work_area(DockManager::instance().usableRect());
        update_spawn_group_config_anchor();
    }

    if (anchor_tools_panel_) {
        anchor_tools_panel_->set_screen_dimensions(screen_w_, screen_h_);
    }
    update_anchor_editor_layout();

}

void RoomEditor::set_room_config_visible(bool visible) {
    ensure_room_configurator();
    if (!room_cfg_ui_) return;
    if (visible && active_modal_ == ActiveModal::AssetInfo) {
        pulse_active_modal_header();
        return;
    }
    if (visible) {
        room_cfg_ui_->open(current_room_);
    }
    room_config_dock_open_ = visible;
    set_camera_settings_lock(visible);
    refresh_room_config_visibility();
}

void RoomEditor::set_shared_footer_bar(DevFooterBar* footer) {
    shared_footer_bar_ = footer;
    configure_shared_panel();
    update_spawn_group_config_anchor();
    Asset* primary = selected_assets_.empty() ? nullptr : selected_assets_.front();
    update_grid_resolution_for_selection(primary);
    refresh_cursor_snap();
}

void RoomEditor::set_snap_to_grid_enabled(bool enabled) {
    if (snap_to_grid_enabled_ == enabled) {
        return;
    }
    snap_to_grid_enabled_ = enabled;
    if (snap_to_grid_enabled_) {
        clear_selection_grid_resolution_override();
    } else {
        Asset* primary = selected_assets_.empty() ? nullptr : selected_assets_.front();
        update_grid_resolution_for_selection(primary);
    }
    refresh_cursor_snap();
}

void RoomEditor::set_header_visibility_callback(std::function<void(bool)> cb) {
    header_visibility_callback_ = std::move(cb);
    if (header_visibility_callback_) {

        header_visibility_callback_(false);
    }
    if (room_cfg_ui_) {
        room_cfg_ui_->set_header_visibility_controller([this](bool visible) {
            room_config_panel_visible_ = visible;
            if (header_visibility_callback_) {

                header_visibility_callback_(visible);
            }
        });
    }
    if (info_ui_) {
        info_ui_->set_header_visibility_callback([this](bool visible) {
            asset_info_panel_visible_ = visible;
            if (header_visibility_callback_) {

                header_visibility_callback_(visible);
            }
        });
    }
}

void RoomEditor::set_map_assets_panel_callback(std::function<void()> cb) {
    open_map_assets_panel_callback_ = std::move(cb);
}

void RoomEditor::set_boundary_assets_panel_callback(std::function<void()> cb) {
    open_boundary_assets_panel_callback_ = std::move(cb);
}

void RoomEditor::set_current_room(Room* room) {
    room_editor_trace("[RoomEditor] set_current_room begin");
    if (room) {
        room_editor_trace(std::string("[RoomEditor] target room -> ") + room->room_name);
    } else {
        room_editor_trace("[RoomEditor] target room -> <null>");
    }

    Room* previous_room = current_room_;
    const bool room_changed = (room != current_room_);

    if (room_changed && anchor_mode_active()) {
        exit_anchor_edit_mode(true);
    }

    if (room != current_room_) {
        room_editor_trace("[RoomEditor] clearing active spawn group target");
        clear_active_spawn_group_target();
        clear_geometry_selection();
    }

    current_room_ = room;
    if (room_changed) {
        invalidate_label_cache(previous_room);
        invalidate_label_cache(current_room_);
    }
    if (current_room_) {
        room_editor_trace("[RoomEditor] acquiring assets_data");
        auto& assets_json = current_room_->assets_data();
        room_editor_trace("[RoomEditor] ensuring spawn_groups array");
        auto& groups = ensure_spawn_groups_array(assets_json);
        if (sanitize_perimeter_spawn_groups(groups)) {
            room_editor_trace("[RoomEditor] perimeter groups sanitized, saving");
            save_current_room_assets_json();
        }
    }
    room_editor_trace("[RoomEditor] rebuilding room spawn id cache");
    rebuild_room_spawn_id_cache();
    room_editor_trace("[RoomEditor] refreshing spawn group config UI");
    refresh_spawn_group_config_ui();
    mark_spatial_index_dirty();

    if (room_cfg_ui_) {
        room_editor_trace("[RoomEditor] opening room config UI");
        room_cfg_ui_->open(current_room_);
        refresh_room_config_visibility();
    }

    if (!enabled_ && room_changed && current_room_) {
        room_editor_trace("[RoomEditor] focusing camera on room center");
        focus_camera_on_room_center();
    }

    room_editor_trace("[RoomEditor] set_current_room complete");

}

void RoomEditor::set_enabled(bool enabled, bool preserve_camera_state) {
    vibble::log::info(std::string("[RoomEditor] Dev Mode ") + (enabled ? "ENABLED" : "DISABLED") +
                     " (preserve_camera=" + (preserve_camera_state ? "true" : "false") + ")");
    enabled_ = enabled;
    if (!assets_) return;
    if (!enabled_) {
        if (anchor_mode_active()) {
            exit_anchor_edit_mode(true);
        }
        active_modal_ = ActiveModal::None;
        clear_geometry_selection();
        mouse_controls_enabled_last_frame_ = false;
        blocking_panel_visible_.fill(false);
        set_camera_settings_lock(false);
    }

    WarpedScreenGrid* cam = assets_ ? &assets_->getView() : nullptr;
    if (enabled_) {
        vibble::log::info("[RoomEditor] Dev Mode enabled: clearing camera overrides");
        if (cam && !preserve_camera_state) {
            cam->set_manual_height_override(false);
            cam->set_manual_zoom_override(false);
        }
        close_asset_info_editor();
        ensure_room_configurator();
        if (room_cfg_ui_) {
            room_cfg_ui_->open(current_room_);
            refresh_room_config_visibility();
        }
        configure_shared_panel();
    } else {
        vibble::log::info("[RoomEditor] Dev Mode disabled: clearing camera overrides and focus");
        if (cam && !preserve_camera_state) {
            cam->set_manual_height_override(false);
            cam->set_manual_zoom_override(false);
            cam->clear_focus_override();
            cam->clear_tilt_override();
        }
        if (library_ui_) library_ui_->close();
        if (info_ui_) info_ui_->close();
        if (spawn_group_panel_) spawn_group_panel_->set_visible(false);
        clear_active_spawn_group_target();
        clear_selection();
        reset_click_state();
        set_room_config_visible(false);
        refresh_room_config_visibility();
    }

    if (input_) input_->clearClickBuffer();
}

void RoomEditor::update(const Input& input) {
    handle_shortcuts(input);

    auto enforce_mouse_controls_disabled = [this]() {
        const bool panel_visible   = spawn_group_panel_ && spawn_group_panel_->is_visible();
        const bool has_spawn_target = active_spawn_group_id_.has_value();
        const bool has_selection   = !selected_assets_.empty();
        const bool has_highlight   = !highlighted_assets_.empty();
        const bool has_hover       = hovered_asset_ != nullptr;

        if (!panel_visible && !has_spawn_target && !has_selection && !has_highlight && !has_hover) {
            return;
        }

        if (has_spawn_target) {
            clear_active_spawn_group_target();
        }

        if (has_selection || has_highlight || has_hover) {
            clear_selection();
            clear_highlighted_assets();
        }
};

    if (!enabled_) {
        if (mouse_controls_enabled_last_frame_) {
            enforce_mouse_controls_disabled();
        }
        mouse_controls_enabled_last_frame_ = false;
        return;
    }

    validate_anchor_edit_target();
    handle_delete_shortcut(input);

    WarpedScreenGrid* cam = assets_ ? &assets_->getView() : nullptr;
    const int mx = input.getX();
    const int my = input.getY();
    const bool ui_blocked = is_ui_blocking_input(mx, my);

    if (!should_enable_mouse_controls()) {
        enforce_mouse_controls_disabled();
        if (cam && !camera_settings_lock_active_) {
            camera_controls_.cancel(*cam);
        }
        mouse_controls_enabled_last_frame_ = false;
        return;
    }

    mouse_controls_enabled_last_frame_ = true;

    if (anchor_mode_active()) {
        handle_mouse_input(input);
    } else if (!ui_blocked || dragging_) {
        handle_mouse_input(input);
    } else if (cam && !camera_settings_lock_active_) {
        camera_controls_.cancel(*cam);
    }

    if (camera_settings_lock_active_ && cam) {
        apply_camera_settings_lock(*cam);
    }

    update_highlighted_assets();
}

void RoomEditor::update_ui(const Input& input) {
    const bool config_visible_now = room_cfg_ui_ && room_cfg_ui_->visible();
    set_camera_settings_lock(enabled_ && config_visible_now);

    if (!enabled_) {
        room_config_was_visible_ = config_visible_now;
        return;
    }

    if (config_visible_now && !room_config_was_visible_) {
        reset_drag_state();
    }

    update_room_config_bounds();

    if (library_ui_ && library_ui_->is_visible()) {
        if (manifest_store_) {
            library_ui_->update(input, screen_w_, screen_h_, assets_->library(), *assets_, *manifest_store_);
        } else {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "[RoomEditor] Manifest store unavailable; asset library UI update skipped.");
        }
    }

    if (library_ui_) {
if (auto selected = library_ui_->consume_selection()) {
    last_selected_from_library_ = selected;
    const bool had_pending_spawn = pending_spawn_world_pos_.has_value();
    bool spawned_asset = false;
    if (pending_spawn_world_pos_) {
        SDL_Point world = *pending_spawn_world_pos_;
        pending_spawn_world_pos_.reset();
        if (current_room_ && assets_) {
            bool inside_room = !current_room_->room_area ||
                               current_room_->room_area->contains_point(world);
            if (inside_room) {
                if (Asset* spawned = assets_->spawn_asset(selected->name, world)) {
                    finalize_asset_drag(spawned, selected);
                    selected_assets_.clear();
                    selected_assets_.push_back(spawned);
                    if (hovered_asset_ != spawned) {
                        hovered_asset_ = spawned;
                    }
                    mark_highlight_dirty();
                    update_highlighted_assets();
                    sync_spawn_group_panel_with_selection();
                    spawned_asset = true;
                }
            }
        }
    }
    if (!spawned_asset && !had_pending_spawn) {
        pending_spawn_world_pos_.reset();
        open_asset_info_editor(selected);
    }
        }

        if (auto area_sel = library_ui_->consume_area_selection()) {
            const bool had_pending_spawn = pending_spawn_world_pos_.has_value();
            if (pending_spawn_world_pos_ && current_room_ && assets_) {
                SDL_Point world = *pending_spawn_world_pos_;
                pending_spawn_world_pos_.reset();

                Room* src_room = nullptr;
                for (Room* r : assets_->rooms()) {
                    if (r && r->room_name == area_sel->room_name) { src_room = r; break; }
                }
                if (src_room) {

                    nlohmann::json* src_entry = nullptr;
                    nlohmann::json& src_root = src_room->assets_data();
                    if (src_root.is_object() && src_root.contains("areas") && src_root["areas"].is_array()) {
                        for (auto& entry : src_root["areas"]) {
                            if (entry.is_object() && entry.value("name", std::string{}) == area_sel->area_name) {
                                src_entry = &entry; break;
                            }
                        }
                    }
                    if (src_entry) {

                        nlohmann::json copy = *src_entry;

                        std::string base = copy.value("name", area_sel->area_name);
                        if (base.empty()) base = "area";
                        std::string candidate = base;
                        int suffix = 1;
                        auto name_conflict = [&](const std::string& name){
                            for (const auto& na : current_room_->areas) {
                                if (na.name == name) return true;
                            }
                            return false;
};
                        while (name_conflict(candidate)) {
                            candidate = base + "_" + std::to_string(suffix++);
                        }
                        copy["name"] = candidate;

                        auto dims_of = [&](Room* room){
                            int w=0,h=0; if (room && room->room_area) {
                                auto b = room->room_area->get_bounds();
                                w = std::max(1, std::get<2>(b) - std::get<0>(b));
                                h = std::max(1, std::get<3>(b) - std::get<1>(b));
                            }
                            return std::pair<int,int>(w,h);
};
                        auto [src_w, src_h] = dims_of(src_room);
                        if (!copy.contains("origional_width") && src_w > 0) copy["origional_width"] = src_w;
                        if (!copy.contains("origional_height") && src_h > 0) copy["origional_height"] = src_h;

                        SDL_Point center{0,0};
                        if (current_room_->room_area) { auto c = current_room_->room_area->get_center(); center.x=c.x; center.y=c.y; }
                        copy["anchor_relative_to_center"] = true;
                        copy["anchor"] = nlohmann::json::object({ {"x", world.x - center.x}, {"y", world.y - center.y} });

                        nlohmann::json& dst_root = current_room_->assets_data();
                        if (!dst_root.contains("areas") || !dst_root["areas"].is_array()) {
                            dst_root["areas"] = nlohmann::json::array();
                        }
                        dst_root["areas"].push_back(copy);
                        enqueue_current_room_save(devmode::core::DevSaveCoordinator::Priority::Debounced);

                        ensure_area_anchor_spawn_entry(current_room_, candidate);
                    }
                }
            } else if (!had_pending_spawn) {

                pending_spawn_world_pos_.reset();
            }
        }
    }

    if (pending_spawn_world_pos_ && (!library_ui_ || !library_ui_->is_visible())) {
        pending_spawn_world_pos_.reset();
    }

    if (room_cfg_ui_ && room_cfg_ui_->visible()) {
        room_cfg_ui_->update(input, screen_w_, screen_h_);
        update_spawn_group_config_anchor();
    }

    if (spawn_group_panel_) {
        spawn_group_panel_->set_screen_dimensions(screen_w_, screen_h_);
        if (spawn_group_panel_->is_visible()) {
            spawn_group_panel_->update(input, screen_w_, screen_h_);
        }
    }

    if (anchor_tools_panel_) {
        anchor_tools_panel_->set_screen_dimensions(screen_w_, screen_h_);
        anchor_tools_panel_->set_visible(anchor_mode_active());
    }
    update_anchor_editor_layout();

    if (info_ui_ && info_ui_->is_visible()) {
        info_ui_->update(input, screen_w_, screen_h_);
    } else if (active_modal_ == ActiveModal::AssetInfo) {
        active_modal_ = ActiveModal::None;
    }

    room_config_was_visible_ = config_visible_now;
}

bool RoomEditor::handle_sdl_event(const SDL_Event& event) {
    int mx = 0;
    int my = 0;
    if (event.type == SDL_EVENT_MOUSE_MOTION) {
        mx = event.motion.x;
        my = event.motion.y;
    } else if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN || event.type == SDL_EVENT_MOUSE_BUTTON_UP) {
        mx = event.button.x;
        my = event.button.y;
    } else if (event.type == SDL_EVENT_MOUSE_WHEEL) {
        sdl_mouse_util::GetMouseState(&mx, &my);
    }

    const bool pointer_event =
        (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN || event.type == SDL_EVENT_MOUSE_BUTTON_UP || event.type == SDL_EVENT_MOUSE_MOTION);
    const bool wheel_event = (event.type == SDL_EVENT_MOUSE_WHEEL);
    const bool pointer_based = pointer_event || wheel_event;

    if (event.type == SDL_EVENT_MOUSE_BUTTON_UP &&
        event.button.button == SDL_BUTTON_LEFT &&
        event.button.clicks >= 2 &&
        !is_room_ui_blocking_point(mx, my) &&
        enabled_) {
        SDL_Point click_point{mx, my};
        Asset* target = selected_assets_.empty() ? nullptr : selected_asset_within_interaction_radius(click_point);
        if (target && !target->spawn_id.empty() && !spawn_group_locked(target->spawn_id)) {
            if (active_modal_ == ActiveModal::AssetInfo) {
                pulse_active_modal_header();
                if (input_) input_->consumeEvent(event);
                return true;
            }
            open_spawn_group_floating_panel(target->spawn_id, click_point);
            if (input_) input_->consumeEvent(event);
            return true;
        }
    }

    struct RouteResult {
        bool handled = false;
        bool pointer_blocked = false;
};

    auto apply_result = [&](const RouteResult& result, bool& pointer_blocked) -> bool {
        if (result.handled) {
            if (input_) {
                if (!pointer_based || result.pointer_blocked) {
                    input_->consumeEvent(event);
                }
            }
            return true;
        }
        if (pointer_based && result.pointer_blocked) {
            pointer_blocked = true;
        }
        return false;
};

    bool pointer_blocked = false;

    auto route_info_panel = [&]() -> RouteResult {
        RouteResult result;
        if (!info_ui_ || !info_ui_->is_visible()) {
            return result;
        }
        if (info_ui_->handle_event(event)) {
            result.handled = true;
            result.pointer_blocked = true;
            return result;
        }
        if (pointer_based && info_ui_->is_point_inside(mx, my)) {
            result.pointer_blocked = true;
        }
        return result;
};

    auto route_room_config = [&]() -> RouteResult {
        RouteResult result;
        if (!room_cfg_ui_ || !room_cfg_ui_->visible()) {
            return result;
        }
        room_cfg_ui_->prepare_for_event(screen_w_, screen_h_);
        if (room_cfg_ui_->handle_event(event)) {
            result.handled = true;
            result.pointer_blocked = true;
            return result;
        }
        if (pointer_based && room_cfg_ui_->is_point_inside(mx, my)) {
            result.pointer_blocked = true;
        }
        return result;
};

    auto route_spawn_groups = [&]() -> RouteResult {
        RouteResult result;
        if (!spawn_group_panel_ || !spawn_group_panel_->is_visible()) {
            return result;
        }
        spawn_group_panel_->set_screen_dimensions(screen_w_, screen_h_);
        if (spawn_group_panel_->handle_event(event)) {
            result.handled = true;
            result.pointer_blocked = true;
            return result;
        }
        if (pointer_based && spawn_group_panel_->is_point_inside(mx, my)) {
            result.pointer_blocked = true;
        }
        return result;
};

    auto route_library_panel = [&]() -> RouteResult {
        RouteResult result;
        if (!library_ui_ || !library_ui_->is_visible()) {
            return result;
        }
        if (library_ui_->handle_event(event)) {
            result.handled = true;
            result.pointer_blocked = true;
            return result;
        }
        if (pointer_based && library_ui_->is_input_blocking_at(mx, my)) {
            result.pointer_blocked = true;
        }
        return result;
    };

    auto route_anchor_ui = [&]() -> RouteResult {
        RouteResult result;
        if (!enabled_) {
            return result;
        }
        ensure_anchor_editor_widgets();
        update_anchor_editor_layout();

        auto handle_button = [&](DMButton* button, const std::function<void()>& on_click) {
            if (!button) {
                return;
            }
            if (button->handle_event(event)) {
                result.handled = true;
                result.pointer_blocked = true;
                if (event.type == SDL_EVENT_MOUSE_BUTTON_UP &&
                    event.button.button == SDL_BUTTON_LEFT &&
                    on_click) {
                    on_click();
                }
            }
            if (pointer_based) {
                SDL_Point point{mx, my};
                if (SDL_PointInRect(&point, &button->rect())) {
                    result.pointer_blocked = true;
                }
            }
        };

        if (anchor_tools_panel_ && anchor_tools_panel_->is_visible()) {
            if (anchor_tools_panel_->handle_event(event)) {
                result.handled = true;
                result.pointer_blocked = true;
            } else if (pointer_based && anchor_tools_panel_->is_point_inside(mx, my)) {
                result.pointer_blocked = true;
            }
        }

        if (should_show_anchor_mode_toggle()) {
            handle_button(anchor_mode_toggle_button_.get(), [this]() { toggle_anchor_edit_mode(); });
        }

        if (anchor_mode_active()) {
            handle_button(anchor_anim_prev_button_.get(), [this]() { navigate_anchor_animation(-1); });
            handle_button(anchor_anim_next_button_.get(), [this]() { navigate_anchor_animation(1); });
            handle_button(anchor_frame_prev_button_.get(), [this]() { navigate_anchor_frame(-1); });
            handle_button(anchor_frame_next_button_.get(), [this]() { navigate_anchor_frame(1); });
        }
        return result;
    };

    if (apply_result(route_info_panel(), pointer_blocked)) {
        return true;
    }
    if (apply_result(route_room_config(), pointer_blocked)) {
        return true;
    }
    if (apply_result(route_spawn_groups(), pointer_blocked)) {
        return true;
    }
    if (apply_result(route_library_panel(), pointer_blocked)) {
        return true;
    }
    if (apply_result(route_anchor_ui(), pointer_blocked)) {
        return true;
    }

    if (auto* dropdown = DMDropdown::active_dropdown()) {
        if (dropdown->handle_event(event)) {
            if (pointer_event && input_) {
                input_->clearClickBuffer();
            }
            return true;
        }
    }

    if (pointer_based && pointer_blocked) {
        return true;
    }

    return false;
}

bool RoomEditor::is_room_panel_blocking_point(int x, int y) const {
    if (!enabled_) {
        return false;
    }
    if (room_cfg_ui_ && room_cfg_ui_->visible() && room_cfg_ui_->is_point_inside(x, y)) {
        return true;
    }
    if (spawn_group_panel_ && spawn_group_panel_->is_visible() && spawn_group_panel_->is_point_inside(x, y)) {
        return true;
    }
    if (is_anchor_ui_blocking_point(x, y)) {
        return true;
    }
    return false;
}

bool RoomEditor::is_room_ui_blocking_point(int x, int y) const {

    if (!enabled_) {
        return false;
    }

    if (info_ui_ && info_ui_->is_visible() && info_ui_->is_point_inside(x, y)) {
        return true;
    }

    if (room_cfg_ui_ && room_cfg_ui_->visible() && room_cfg_ui_->is_point_inside(x, y)) {
        return true;
    }
    if (spawn_group_panel_ && spawn_group_panel_->is_visible() && spawn_group_panel_->is_point_inside(x, y)) {
        return true;
    }

    if (library_ui_ && library_ui_->is_visible() && library_ui_->is_input_blocking_at(x, y)) {
        return true;
    }

    if (is_anchor_ui_blocking_point(x, y)) {
        return true;
    }

    return false;
}

bool RoomEditor::is_shift_key_down() const {
    if (!input_) {
        return false;
    }
    return input_->isScancodeDown(SDL_SCANCODE_LSHIFT) || input_->isScancodeDown(SDL_SCANCODE_RSHIFT);
}

void RoomEditor::set_camera_settings_lock(bool active) {
    if (camera_settings_lock_active_ == active) {
        return;
    }
    camera_settings_lock_active_ = active;

    WarpedScreenGrid* cam = assets_ ? &assets_->getView() : nullptr;
    if (!cam) {
        return;
    }

    if (camera_settings_lock_active_) {
        camera_settings_drag_ = CameraSettingsDragState{};
        camera_lock_restore_.valid = true;
        camera_lock_restore_.manual_height_override = cam->is_manual_height_override();
        camera_lock_restore_.manual_zoom_override = cam->is_manual_zoom_override();
        camera_lock_restore_.had_focus_override = cam->has_focus_override();
        if (camera_lock_restore_.had_focus_override) {
            camera_lock_restore_.focus_point = cam->get_focus_override_point();
        }
        camera_lock_restore_.screen_center = cam->get_screen_center();
        camera_lock_restore_.camera_zoom_percent = cam->get_zoom_percent();
        // Keep live camera updates while locked by clearing manual overrides.
        cam->set_manual_height_override(false);
        cam->set_manual_zoom_override(false);
        apply_camera_settings_lock(*cam);
    } else {
        camera_settings_drag_ = CameraSettingsDragState{};
        const SDL_Point previous_center = cam->get_screen_center();
        if (camera_lock_restore_.valid) {
            cam->set_manual_height_override(camera_lock_restore_.manual_height_override);
            cam->set_zoom_percent(camera_lock_restore_.camera_zoom_percent);
            cam->set_manual_zoom_override(camera_lock_restore_.manual_zoom_override);
            if (camera_lock_restore_.had_focus_override) {
                cam->set_focus_override(camera_lock_restore_.focus_point);
            } else {
                cam->clear_focus_override();
            }
            cam->set_screen_center(camera_lock_restore_.screen_center);
        } else {
            cam->clear_focus_override();
        }
        const SDL_Point updated_center = cam->get_screen_center();
        if (previous_center.x != updated_center.x || previous_center.y != updated_center.y) {
            mark_spatial_index_dirty();
        }
        camera_lock_restore_ = CameraLockState{};
    }
}

SDL_Point RoomEditor::camera_lock_target() const {
    if (player_) {
        return player_->world_xz_point();
    }
    if (current_room_ && current_room_->room_area) {
        auto c = current_room_->room_area->get_center();
        return SDL_Point{
            c.x + current_room_->camera_center_dx,
            c.y + current_room_->camera_center_dz
        };
    }
    if (assets_) {
        return assets_->getView().get_screen_center();
    }
    return SDL_Point{0, 0};
}

void RoomEditor::apply_camera_settings_lock(WarpedScreenGrid& cam) {
    const SDL_Point previous_center = cam.get_screen_center();
    SDL_Point target = camera_lock_target();
    cam.set_focus_override(target);
    cam.set_screen_center(target);
    if (previous_center.x != target.x || previous_center.y != target.y) {
        mark_spatial_index_dirty();
    }
}

void RoomEditor::invalidate_label_cache(Room* room) {
    if (!room) {
        return;
    }
    auto it = label_cache_.find(room);
    if (it == label_cache_.end()) {
        return;
    }
    if (it->second.texture) {
        SDL_DestroyTexture(it->second.texture);
        it->second.texture = nullptr;
    }
    it->second.text_size = SDL_Point{0, 0};
    it->second.last_name.clear();
    it->second.last_color = SDL_Color{0, 0, 0, 0};
    it->second.dirty = true;
}

void RoomEditor::invalidate_all_room_labels() {
    for (auto& [room, entry] : label_cache_) {
        (void)room;
        if (entry.texture) {
            SDL_DestroyTexture(entry.texture);
            entry.texture = nullptr;
        }
        entry.text_size = SDL_Point{0, 0};
        entry.last_name.clear();
        entry.last_color = SDL_Color{0, 0, 0, 0};
        entry.dirty = true;
    }
}

void RoomEditor::prune_label_cache(const std::vector<Room*>& rooms) {
    std::unordered_set<Room*> active(rooms.begin(), rooms.end());
    for (auto it = label_cache_.begin(); it != label_cache_.end();) {
        if (active.find(it->first) == active.end()) {
            if (it->second.texture) {
                SDL_DestroyTexture(it->second.texture);
            }
            it = label_cache_.erase(it);
        } else {
            ++it;
        }
    }
}

void RoomEditor::render_room_labels(SDL_Renderer* renderer) {
    if (!enabled_) return;
    if (!renderer || !assets_) return;

    ensure_label_font();
    if (!label_font_) return;

    const std::vector<Room*>& rooms = assets_->rooms();
    if (rooms.empty()) return;

    prune_label_cache(rooms);

    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);

    label_rects_.clear();

    struct LabelInfo {
        Room* room = nullptr;
        SDL_FPoint desired_center{0.0f, 0.0f};
        float priority = 0.0f;
};

    std::vector<LabelInfo> render_queue;
    render_queue.reserve(rooms.size());

    SDL_FPoint screen_center{static_cast<float>(screen_w_) * 0.5f,
                             static_cast<float>(screen_h_) * 0.5f};

    WarpedScreenGrid& view = assets_->getView();

    for (Room* room : rooms) {
        if (!room || !room->room_area) continue;

        SDL_Point center = room->room_area->get_center();
        SDL_FPoint screen_pt = view.map_to_screen(center);
        SDL_FPoint desired_center{screen_pt.x,
                                  screen_pt.y - kLabelVerticalOffset};

        float dx = desired_center.x - screen_center.x;
        float dy = desired_center.y - screen_center.y;
        float dist2 = dx * dx + dy * dy;

        render_queue.push_back(LabelInfo{room, desired_center, dist2});
    }

    std::sort(render_queue.begin(), render_queue.end(), [](const LabelInfo& a, const LabelInfo& b) {
        if (a.priority == b.priority) {
            return a.room < b.room;
        }
        return a.priority < b.priority;
    });

    for (const auto& info : render_queue) {
        if (!info.room) continue;
        render_room_label(renderer, info.room, info.desired_center);
    }
}

void RoomEditor::render_room_label(SDL_Renderer* renderer, Room* room, SDL_FPoint desired_center) {
    if (!room || !room->room_area || !assets_) return;
    if (!label_font_) return;

    const std::string name = room->room_name.empty() ? std::string("<unnamed>") : room->room_name;
    SDL_Color base_color = room->display_color();

    auto& cache = label_cache_[room];
    if (cache.last_name != name || !colors_equal(cache.last_color, base_color)) {
        cache.dirty = true;
    }

    if (cache.dirty) {
        SDL_Color text_color = display_color_luminance(base_color) > 0.55f
                                   ? SDL_Color{20, 20, 20, 255}
                                   : kLabelText;

        SDL_Surface* text_surface = ttf_util::RenderTextBlended(label_font_, name.c_str(), text_color);
        if (!text_surface) {
            return;
        }

        SDL_Texture* new_texture = SDL_CreateTextureFromSurface(renderer, text_surface);
        if (!new_texture) {
            SDL_DestroySurface(text_surface);
            return;
        }

        if (cache.texture) {
            SDL_DestroyTexture(cache.texture);
        }
        cache.texture = new_texture;
        cache.text_size = SDL_Point{text_surface->w, text_surface->h};
        cache.last_name = name;
        cache.last_color = base_color;
        cache.dirty = false;

        SDL_DestroySurface(text_surface);
    }

    if (!cache.texture || cache.text_size.x <= 0 || cache.text_size.y <= 0) {
        return;
    }

    SDL_Rect bg_rect = label_background_rect(cache.text_size.x, cache.text_size.y, desired_center);
    bg_rect = resolve_edge_overlap(bg_rect, desired_center);

    label_rects_.push_back(bg_rect);

    SDL_Color bg_color = with_alpha(lighten(base_color, 0.08f), 205);
    SDL_Color border_color = with_alpha(darken(base_color, 0.3f), 235);

    const int radius = std::min(DMStyles::CornerRadius(), std::min(bg_rect.w, bg_rect.h) / 2);
    const int bevel = std::min(DMStyles::BevelDepth(), std::max(0, std::min(bg_rect.w, bg_rect.h) / 2));
    dm_draw::DrawBeveledRect( renderer, bg_rect, radius, bevel, bg_color, bg_color, bg_color, false, 0.0f, 0.0f);
    dm_draw::DrawRoundedOutline( renderer, bg_rect, radius, 1, border_color);

    SDL_Rect dst{bg_rect.x + kLabelPadding, bg_rect.y + kLabelPadding, cache.text_size.x, cache.text_size.y};
    sdl_render::Texture(renderer, cache.texture, nullptr, &dst);
}

SDL_Rect RoomEditor::label_background_rect(int text_w, int text_h, SDL_FPoint desired_center) const {
    int rect_w = text_w + kLabelPadding * 2;
    int rect_h = text_h + kLabelPadding * 2;

    SDL_Rect rect{};
    rect.w = rect_w;
    rect.h = rect_h;

    if (screen_w_ <= 0 || screen_h_ <= 0) {
        rect.x = static_cast<int>(std::lround(desired_center.x - static_cast<float>(rect_w) * 0.5f));
        rect.y = static_cast<int>(std::lround(desired_center.y - static_cast<float>(rect_h) * 0.5f));
        return rect;
    }

    const float half_w = static_cast<float>(rect_w) * 0.5f;
    const float half_h = static_cast<float>(rect_h) * 0.5f;
    const float min_x = half_w;
    const float max_x = static_cast<float>(screen_w_) - half_w;
    const float min_y = half_h;
    const float max_y = static_cast<float>(screen_h_) - half_h;

    auto clamp_center = [&](const SDL_FPoint& point) {
        SDL_FPoint clamped = point;
        clamped.x = std::clamp(clamped.x, min_x, max_x);
        clamped.y = std::clamp(clamped.y, min_y, max_y);
        return clamped;
};

    SDL_FPoint center = clamp_center(desired_center);

    const bool inside = desired_center.x >= min_x && desired_center.x <= max_x &&
                        desired_center.y >= min_y && desired_center.y <= max_y;

    if (!inside) {
        SDL_FPoint screen_center{static_cast<float>(screen_w_) * 0.5f,
                                 static_cast<float>(screen_h_) * 0.5f};
        const float dx = desired_center.x - screen_center.x;
        const float dy = desired_center.y - screen_center.y;
        const float epsilon = 0.0001f;

        if (std::fabs(dx) > epsilon || std::fabs(dy) > epsilon) {
            float t_min = 1.0f;

            auto update_t = [&](float boundary, float origin, float delta) {
                if (std::fabs(delta) < epsilon) return;
                float t = (boundary - origin) / delta;
                if (t >= 0.0f) {
                    t_min = std::min(t_min, t);
                }
};

            if (dx > 0.0f) update_t(max_x, screen_center.x, dx);
            else if (dx < 0.0f) update_t(min_x, screen_center.x, dx);

            if (dy > 0.0f) update_t(max_y, screen_center.y, dy);
            else if (dy < 0.0f) update_t(min_y, screen_center.y, dy);

            center.x = screen_center.x + dx * t_min;
            center.y = screen_center.y + dy * t_min;
            center = clamp_center(center);
        }
    }

    rect.x = static_cast<int>(std::lround(center.x - half_w));
    rect.y = static_cast<int>(std::lround(center.y - half_h));
    return rect;
}

SDL_Rect RoomEditor::resolve_edge_overlap(SDL_Rect rect, SDL_FPoint desired_center) {
    if (screen_w_ <= 0 || screen_h_ <= 0) {
        return rect;
    }

    const int tolerance = 1;
    const bool touches_left = rect.x <= tolerance;
    const bool touches_right = rect.x + rect.w >= screen_w_ - tolerance;
    const bool touches_top = rect.y <= tolerance;
    const bool touches_bottom = rect.y + rect.h >= screen_h_ - tolerance;

    if (touches_top || touches_bottom) {
        rect = resolve_horizontal_edge_overlap(rect, desired_center.x, touches_top);
    }

    if (touches_left || touches_right) {
        rect = resolve_vertical_edge_overlap(rect, desired_center.y, touches_left);
    }

    return rect;
}

SDL_Rect RoomEditor::resolve_horizontal_edge_overlap(SDL_Rect rect, float desired_center_x, bool top_edge) {
    if (screen_w_ <= 0) return rect;

    const int min_x = 0;
    const int max_x = std::max(0, screen_w_ - rect.w);
    if (max_x <= min_x) {
        rect.x = min_x;
        return rect;
    }

    std::vector<SDL_Rect> same_edge_rects;
    same_edge_rects.reserve(label_rects_.size());
    const int tolerance = 1;

    for (const SDL_Rect& other : label_rects_) {
        bool other_on_edge = top_edge ? other.y <= tolerance
                                      : other.y + other.h >= screen_h_ - tolerance;
        if (other_on_edge) {
            same_edge_rects.push_back(other);
        }
    }

    if (same_edge_rects.empty()) {
        rect.x = std::clamp(static_cast<int>(std::lround(desired_center_x - rect.w * 0.5f)), min_x, max_x);
        return rect;
    }

    std::vector<int> to_process;
    to_process.reserve(same_edge_rects.size() * 2 + 3);

    int target_x = std::clamp(static_cast<int>(std::lround(desired_center_x - rect.w * 0.5f)), min_x, max_x);
    to_process.push_back(target_x);
    to_process.push_back(min_x);
    to_process.push_back(max_x);

    std::vector<int> visited;
    visited.reserve(to_process.size());

    float best_penalty = std::numeric_limits<float>::max();
    int best_x = target_x;
    bool found_position = false;

    while (!to_process.empty()) {
        int candidate_x = to_process.back();
        to_process.pop_back();

        if (std::find(visited.begin(), visited.end(), candidate_x) != visited.end()) {
            continue;
        }
        visited.push_back(candidate_x);

        SDL_Rect candidate = rect;
        candidate.x = candidate_x;

        std::vector<SDL_Rect> overlapping;
        for (const SDL_Rect& other : same_edge_rects) {
            if (rects_overlap(candidate, other)) {
                overlapping.push_back(other);
            }
        }

        if (overlapping.empty()) {
            float center_x = static_cast<float>(candidate.x) + static_cast<float>(candidate.w) * 0.5f;
            float penalty = std::fabs(center_x - desired_center_x);
            if (penalty < best_penalty - 0.01f || (!found_position && penalty <= best_penalty + 0.01f)) {
                best_penalty = penalty;
                best_x = candidate_x;
                found_position = true;
                if (penalty <= 0.01f) {
                    break;
                }
            }
            continue;
        }

        for (const SDL_Rect& other : overlapping) {
            int left = std::clamp(other.x - rect.w, min_x, max_x);
            int right = std::clamp(other.x + other.w, min_x, max_x);

            if (std::find(visited.begin(), visited.end(), left) == visited.end()) {
                to_process.push_back(left);
            }
            if (std::find(visited.begin(), visited.end(), right) == visited.end()) {
                to_process.push_back(right);
            }
        }
    }

    rect.x = found_position ? best_x : target_x;
    return rect;
}

SDL_Rect RoomEditor::resolve_vertical_edge_overlap(SDL_Rect rect, float desired_center_y, bool left_edge) {
    if (screen_h_ <= 0) return rect;

    const int min_y = 0;
    const int max_y = std::max(0, screen_h_ - rect.h);
    if (max_y <= min_y) {
        rect.y = min_y;
        return rect;
    }

    std::vector<SDL_Rect> same_edge_rects;
    same_edge_rects.reserve(label_rects_.size());
    const int tolerance = 1;

    for (const SDL_Rect& other : label_rects_) {
        bool other_on_edge = left_edge ? other.x <= tolerance
                                       : other.x + other.w >= screen_w_ - tolerance;
        if (other_on_edge) {
            same_edge_rects.push_back(other);
        }
    }

    if (same_edge_rects.empty()) {
        rect.y = std::clamp(static_cast<int>(std::lround(desired_center_y - rect.h * 0.5f)), min_y, max_y);
        return rect;
    }

    std::vector<int> to_process;
    to_process.reserve(same_edge_rects.size() * 2 + 3);

    int target_y = std::clamp(static_cast<int>(std::lround(desired_center_y - rect.h * 0.5f)), min_y, max_y);
    to_process.push_back(target_y);
    to_process.push_back(min_y);
    to_process.push_back(max_y);

    std::vector<int> visited;
    visited.reserve(to_process.size());

    float best_penalty = std::numeric_limits<float>::max();
    int best_y = target_y;
    bool found_position = false;

    while (!to_process.empty()) {
        int candidate_y = to_process.back();
        to_process.pop_back();

        if (std::find(visited.begin(), visited.end(), candidate_y) != visited.end()) {
            continue;
        }
        visited.push_back(candidate_y);

        SDL_Rect candidate = rect;
        candidate.y = candidate_y;

        std::vector<SDL_Rect> overlapping;
        for (const SDL_Rect& other : same_edge_rects) {
            if (rects_overlap(candidate, other)) {
                overlapping.push_back(other);
            }
        }

        if (overlapping.empty()) {
            float center_y = static_cast<float>(candidate.y) + static_cast<float>(candidate.h) * 0.5f;
            float penalty = std::fabs(center_y - desired_center_y);
            if (penalty < best_penalty - 0.01f || (!found_position && penalty <= best_penalty + 0.01f)) {
                best_penalty = penalty;
                best_y = candidate_y;
                found_position = true;
                if (penalty <= 0.01f) {
                    break;
                }
            }
            continue;
        }

        for (const SDL_Rect& other : overlapping) {
            int up = std::clamp(other.y - rect.h, min_y, max_y);
            int down = std::clamp(other.y + other.h, min_y, max_y);

            if (std::find(visited.begin(), visited.end(), up) == visited.end()) {
                to_process.push_back(up);
            }
            if (std::find(visited.begin(), visited.end(), down) == visited.end()) {
                to_process.push_back(down);
            }
        }
    }

    rect.y = found_position ? best_y : target_y;
    return rect;
}

bool RoomEditor::rects_overlap(const SDL_Rect& a, const SDL_Rect& b) {
    return !(a.x + a.w <= b.x || b.x + b.w <= a.x || a.y + a.h <= b.y || b.y + b.h <= a.y);
}

void RoomEditor::ensure_label_font() {
    if (label_font_) return;
    label_font_ = TTF_OpenFont(dm::FONT_PATH, 18);
}

void RoomEditor::release_label_font() {
    if (label_font_) {
        TTF_CloseFont(label_font_);
        label_font_ = nullptr;
    }
}

void RoomEditor::render_overlays(SDL_Renderer* renderer) {
    if (!assets_) {
        return;
    }
    const WarpedScreenGrid& cam = assets_->getView();

    if (renderer) {
        if (selected_geometry_room_ && selected_geometry_room_->room_area) {
            const auto& root = selected_geometry_room_->assets_data();
            const int min_w = std::max(1, root.value("min_width", 1));
            const int max_w = std::max(min_w, root.value("max_width", min_w));
            const SDL_Point center = selected_geometry_room_->room_area->get_center();
            auto draw_ring = [&](int diameter, SDL_Color color) {
                const int radius = std::max(1, diameter / 2);
                SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
                SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
                const int segments = 160;
                SDL_FPoint prev = cam.map_to_screen(SDL_Point{center.x + radius, center.y});
                for (int i = 1; i <= segments; ++i) {
                    double a = (static_cast<double>(i) / static_cast<double>(segments)) * 2.0 * kPi;
                    SDL_FPoint cur = cam.map_to_screen(SDL_Point{center.x + static_cast<int>(std::lround(std::cos(a) * radius)), center.y + static_cast<int>(std::lround(std::sin(a) * radius))});
                    SDL_RenderLine(renderer, prev.x, prev.y, cur.x, cur.y);
                    prev = cur;
                }
            };
            draw_ring(max_w, SDL_Color{255, 165, 0, 220});
            draw_ring(min_w, SDL_Color{80, 150, 255, 220});
        } else if (current_room_ && current_room_->room_area) {
            const SDL_Color accent_hover = DMStyles::AccentButton().hover_bg;
            auto style = dm_draw::ResolveRoomBoundsOverlayStyle(accent_hover);
            SDL_Color accent_fill = dm_draw::LightenColor(DMStyles::AccentButton().bg, 0.18f);
            accent_fill.a = 110;
            style.fill = accent_fill;
            style.outline = DMStyles::AccentButton().border;
            style.center = DMStyles::HighlightColor();
            style.center.a = 235;
            SDL_Color accent_glow = dm_draw::LightenColor(DMStyles::AccentButton().bg, 0.35f);
            accent_glow.a = 140;
            style.glow = accent_glow;
            dm_draw::RenderRoomBoundsOverlay(renderer, assets_->getView(), *current_room_->room_area, style);
        } else if (hovered_geometry_room_ && hovered_geometry_room_->room_area) {
            auto style = dm_draw::ResolveRoomBoundsOverlayStyle(SDL_Color{255,255,255,255});
            const SDL_Color white_outline{255, 255, 255, 240};
            const SDL_Color white_fill{255, 255, 255, 32};
            const SDL_Color white_center{255, 255, 255, 220};
            const SDL_Color white_glow{255, 255, 255, 140};
            style.outline = white_outline;
            style.fill = white_fill;
            style.center = white_center;
            style.glow = white_glow;
            dm_draw::RenderRoomBoundsOverlay(renderer, assets_->getView(), *hovered_geometry_room_->room_area, style);
        }
    }

    if (renderer && enabled_) {

        const bool shift_now = is_shift_key_down();

        if (shift_now && assets_ && active_assets_) {
            SDL_Color point_color{70, 170, 255, 215};
            SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
            for (Asset* asset : *active_assets_) {
                if (!asset || asset->dead) continue;
                if (!asset_belongs_to_room(asset)) continue;
                if (!asset_matches_selection_filter(asset)) continue;
                if (!asset->spawn_id.empty() && spawn_group_locked(asset->spawn_id)) continue;
                SDL_Point gp = grid_point_for_asset(asset);
                render_grid_point_marker(renderer, cam, gp, point_color, 3);
            }
        }

        const bool has_selected = !selected_assets_.empty();
        const bool has_hover_highlight = shift_now && !highlighted_assets_.empty();

        if (has_selected || has_hover_highlight) {
            ensure_spatial_index(cam);
            SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
            const int outline_offset = 3;  // Thicker outline for better visibility

            // Always render selected assets, regardless of Shift state.
            if (has_selected) {
                for (Asset* asset : selected_assets_) {
                    if (!asset_belongs_to_room(asset)) continue;
                    SDL_Color color{255, 165, 0, 255}; // orange for selection
                    render_asset_outline(renderer, asset, cam, color, outline_offset);
                }
            }

            // Render hover outlines only when Shift is held.
            if (has_hover_highlight) {
                for (Asset* asset : highlighted_assets_) {
                    if (!asset_belongs_to_room(asset)) continue;
                    if (std::find(selected_assets_.begin(), selected_assets_.end(), asset) != selected_assets_.end()) {
                        continue; // skip already-selected assets
                    }
                    SDL_Color color{60, 220, 255, 255}; // vivid teal for hover
                    render_asset_outline(renderer, asset, cam, color, outline_offset);
                }
            }
        }

        if (anchor_mode_active()) {
            refresh_anchor_mode_handles();
            SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
            for (const AnchorHandleSample& handle : anchor_edit_.handles) {
                if (!handle.has_final_screen_px) {
                    continue;
                }

                const bool selected = (handle.name == anchor_edit_.selected_anchor_name);
                const bool hovered = (handle.name == anchor_edit_.hovered_anchor_name);

                if (handle.has_flat_screen_px) {
                    SDL_SetRenderDrawColor(renderer, 80, 170, 255, 180);
                    SDL_RenderLine(renderer,
                                   handle.flat_screen_px.x,
                                   handle.flat_screen_px.y,
                                   handle.final_screen_px.x,
                                   handle.final_screen_px.y);
                    const int fx = static_cast<int>(std::lround(handle.flat_screen_px.x));
                    const int fy = static_cast<int>(std::lround(handle.flat_screen_px.y));
                    SDL_RenderLine(renderer, fx - 3, fy, fx + 3, fy);
                    SDL_RenderLine(renderer, fx, fy - 3, fx, fy + 3);
                }

                const int cx = static_cast<int>(std::lround(handle.final_screen_px.x));
                const int cy = static_cast<int>(std::lround(handle.final_screen_px.y));
                SDL_Color color{240, 240, 240, 230};
                if (selected) {
                    color = SDL_Color{255, 200, 60, 245};
                } else if (hovered) {
                    color = SDL_Color{120, 230, 255, 240};
                }
                SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
                SDL_Rect box{cx - 4, cy - 4, 9, 9};
                sdl_render::FillRect(renderer, &box);
                SDL_SetRenderDrawColor(renderer, 24, 24, 24, 255);
                sdl_render::Rect(renderer, &box);
            }
        }
    }

    if (library_ui_ && library_ui_->is_visible()) {
        library_ui_->render(renderer, screen_w_, screen_h_);
    }
    if (info_ui_ && info_ui_->is_visible()) {
        info_ui_->render_world_overlay(renderer, assets_->getView());
        info_ui_->render(renderer, screen_w_, screen_h_);
    }

    if (renderer && assets_ && current_room_ && current_room_->room_area) {
        auto overlay = compute_perimeter_overlay_for_drag();
        if (!overlay) {
            std::string spawn_id;
            if (hovered_asset_ && hovered_asset_->spawn_method == "Perimeter" && !hovered_asset_->spawn_id.empty()) {
                spawn_id = hovered_asset_->spawn_id;
            } else {
                for (Asset* asset : selected_assets_) {
                    if (!asset) continue;
                    if (asset->spawn_method == "Perimeter" && !asset->spawn_id.empty()) {
                        spawn_id = asset->spawn_id;
                        break;
                    }
                }
            }
            if (!spawn_id.empty()) {
                overlay = compute_perimeter_overlay_for_spawn(spawn_id);
            }
        }
        if (overlay && overlay->radius > 0.0) {
            SDL_FPoint screen_center_f = cam.map_to_screen(overlay->center);
            SDL_Point screen_center{static_cast<int>(std::lround(screen_center_f.x)),
                                    static_cast<int>(std::lround(screen_center_f.y))};
            SDL_FPoint edge_screen_f = cam.map_to_screen(SDL_Point{
                overlay->center.x + static_cast<int>(std::lround(overlay->radius)),
                overlay->center.y
            });
            const float dx = edge_screen_f.x - screen_center_f.x;
            const float dy = edge_screen_f.y - screen_center_f.y;
            int radius_px = static_cast<int>(std::lround(std::hypot(dx, dy)));
            radius_px = std::max(1, radius_px);
            SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
            const SDL_Color accent = DMStyles::AccentButton().hover_bg;
            SDL_SetRenderDrawColor(renderer, accent.r, accent.g, accent.b, 210);
            const int segments = std::clamp(radius_px * 4, 64, 720);
            for (int i = 0; i < segments; ++i) {
                double angle = (static_cast<double>(i) / static_cast<double>(segments)) * 2.0 * kPi;
                int px = screen_center.x + static_cast<int>(std::lround(std::cos(angle) * static_cast<double>(radius_px)));
                int py = screen_center.y + static_cast<int>(std::lround(std::sin(angle) * static_cast<double>(radius_px)));
                SDL_RenderPoint(renderer, px, py);
            }
            const int cross = std::max(6, radius_px / 4);
            SDL_RenderLine(renderer, screen_center.x - cross, screen_center.y, screen_center.x + cross, screen_center.y);
            SDL_RenderLine(renderer, screen_center.x, screen_center.y - cross, screen_center.x, screen_center.y + cross);
        }

    }
    if (room_cfg_ui_ && room_cfg_ui_->visible()) {
        room_cfg_ui_->render(renderer);
    }
    if (spawn_group_panel_ && spawn_group_panel_->is_visible()) {
        spawn_group_panel_->render(renderer);
    }
    if (renderer && enabled_) {
        update_anchor_editor_layout();
        if (should_show_anchor_mode_toggle() && anchor_mode_toggle_button_) {
            anchor_mode_toggle_button_->render(renderer);
        }
        if (anchor_mode_active()) {
            if (anchor_anim_prev_button_) anchor_anim_prev_button_->render(renderer);
            if (anchor_anim_next_button_) anchor_anim_next_button_->render(renderer);
            if (anchor_frame_prev_button_) anchor_frame_prev_button_->render(renderer);
            if (anchor_frame_next_button_) anchor_frame_next_button_->render(renderer);
            if (anchor_tools_panel_ && anchor_tools_panel_->is_visible()) {
                anchor_tools_panel_->render(renderer);
            }
        }
    }
    DMDropdown::render_active_options(renderer);
}

void RoomEditor::toggle_asset_library() {
    if (!library_ui_) library_ui_ = std::make_unique<AssetLibraryUI>();
    const bool currently_open = library_ui_ && library_ui_->is_visible();
    if (!currently_open && active_modal_ == ActiveModal::AssetInfo) {
        pulse_active_modal_header();
        return;
    }
    if (library_ui_ && library_ui_->is_locked()) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "[RoomEditor] Asset library is locked; toggle ignored.");
        return;
    }
    library_ui_->toggle();
    set_blocking_panel_visible(BlockingPanel::AssetLibrary, library_ui_ && library_ui_->is_visible());
}

void RoomEditor::open_asset_library() {
    if (!library_ui_) library_ui_ = std::make_unique<AssetLibraryUI>();
    if (active_modal_ == ActiveModal::AssetInfo && (!library_ui_ || !library_ui_->is_visible())) {
        pulse_active_modal_header();
        return;
    }
    library_ui_->open();
    set_blocking_panel_visible(BlockingPanel::AssetLibrary, library_ui_ && library_ui_->is_visible());
}

void RoomEditor::close_asset_library() {
    if (library_ui_) library_ui_->close();
    set_blocking_panel_visible(BlockingPanel::AssetLibrary, library_ui_ && library_ui_->is_visible());
    pending_spawn_world_pos_.reset();
}

bool RoomEditor::is_asset_library_open() const {
    return library_ui_ && library_ui_->is_visible();
}

bool RoomEditor::is_library_drag_active() const {
    return library_ui_ && library_ui_->is_visible() && library_ui_->is_dragging_asset();
}

std::shared_ptr<AssetInfo> RoomEditor::consume_selected_asset_from_library() {
    if (!library_ui_) return nullptr;
    return library_ui_->consume_selection();
}

void RoomEditor::open_asset_info_editor(const std::shared_ptr<AssetInfo>& info) {
    if (!info) return;
    if (library_ui_) library_ui_->close();
    clear_active_spawn_group_target();
    if (room_config_dock_open_) {
        set_room_config_visible(false);
    }
    if (!info_ui_) {
        info_ui_ = std::make_unique<AssetInfoUI>();
        if (info_ui_) {
            info_ui_->set_save_coordinator(save_coordinator_);
            info_ui_->set_manifest_store(manifest_store_);
        }
        info_ui_->set_header_visibility_callback([this](bool visible) {
            asset_info_panel_visible_ = visible;
            if (header_visibility_callback_) {
                header_visibility_callback_(room_config_panel_visible_ || asset_info_panel_visible_);
            }
        });
    }
    if (info_ui_) info_ui_->set_assets(assets_);
    if (info_ui_) {
        info_ui_->clear_info();
        info_ui_->set_info(info);
        info_ui_->set_target_asset(nullptr);
        info_ui_->open();
    }
    active_modal_ = ActiveModal::AssetInfo;
}

void RoomEditor::open_animation_editor_for_asset(const std::shared_ptr<AssetInfo>& info) {
    open_asset_info_editor(info);
    if (info_ui_) {
        info_ui_->open_animation_editor_panel();
    }
}

void RoomEditor::open_asset_info_editor_for_asset(Asset* asset) {
    if (!asset || !asset->info) return;
    std::cout << "Opening AssetInfoUI for asset: " << asset->info->name << std::endl;
    focus_camera_on_asset(asset, 0.8);
    open_asset_info_editor(asset->info);
    if (info_ui_) info_ui_->set_target_asset(asset);
}

void RoomEditor::set_manifest_store(devmode::core::ManifestStore* store) {
    manifest_store_ = store;
    if (info_ui_) {
        info_ui_->set_manifest_store(manifest_store_);
    }
    if (spawn_group_panel_) {
        spawn_group_panel_->set_manifest_store(manifest_store_);
    }
    if (room_cfg_ui_) {
        room_cfg_ui_->set_manifest_store(manifest_store_);
    }
}

void RoomEditor::set_save_coordinator(devmode::core::DevSaveCoordinator* coordinator) {
    save_coordinator_ = coordinator;
    if (info_ui_) {
        info_ui_->set_save_coordinator(save_coordinator_);
    }
}

void RoomEditor::set_save_manager(devmode::core::SaveManager* manager) {
    save_manager_ = manager;
}

void RoomEditor::close_asset_info_editor() {
    if (info_ui_) info_ui_->close();
    if (asset_info_panel_visible_) {
        asset_info_panel_visible_ = false;
        if (header_visibility_callback_) {
            header_visibility_callback_(room_config_panel_visible_ || asset_info_panel_visible_);
        }
    }
    if (active_modal_ == ActiveModal::AssetInfo) {
        active_modal_ = ActiveModal::None;
    }
}

bool RoomEditor::is_asset_info_editor_open() const {
    return info_ui_ && info_ui_->is_visible();
}

bool RoomEditor::has_active_modal() const {
    return active_modal_ != ActiveModal::None;
}

void RoomEditor::pulse_active_modal_header() {
    if (active_modal_ == ActiveModal::AssetInfo && info_ui_) {
        info_ui_->pulse_header();
    }
}

void RoomEditor::finalize_asset_drag(Asset* asset, const std::shared_ptr<AssetInfo>& info) {
    if (!asset || !info || !current_room_) return;

    auto& root = current_room_->assets_data();
    auto& arr  = ensure_spawn_groups_array(root);

    int width = 0;
    int height = 0;
    SDL_Point center{0, 0};

    if (current_room_->room_area) {
        auto bounds = current_room_->room_area->get_bounds();
        width  = std::max(1, std::get<2>(bounds) - std::get<0>(bounds));
        height = std::max(1, std::get<3>(bounds) - std::get<1>(bounds));
        auto c = current_room_->room_area->get_center();
        center.x = c.x;
        center.y = c.y;
    }

    std::string spawn_id = generate_spawn_id();

    nlohmann::json entry;
    entry["spawn_id"]        = spawn_id;
    entry["position"]        = "Exact";
    entry["dx"]              = asset->world_x() - center.x;
    entry["dz"]              = asset->world_z() - center.y;
    if (width  > 0) entry["origional_width"]  = width;
    if (height > 0) entry["origional_height"] = height;
    entry["display_name"]    = info->name;

    const int default_resolution =
        current_room_ ? current_room_->map_grid_settings().grid_resolution : MapGridSettings::defaults().grid_resolution;

    devmode::spawn::ensure_spawn_group_entry_defaults(entry, info->name, default_resolution);

    entry["candidates"].push_back({{"name", info->name}, {"chance", 100}});

    arr.push_back(entry);
    save_current_room_assets_json();

    asset->spawn_id     = spawn_id;
    asset->spawn_method = "Exact";

    if (asset) {
        refresh_asset_spatial_entry(assets_->getView(), asset);
        ensure_spatial_index(assets_->getView());
    }

    mark_highlight_dirty();

    active_spawn_group_id_ = spawn_id;
    refresh_spawn_group_config_ui();
    rebuild_room_spawn_id_cache();
}

void RoomEditor::toggle_room_config() {
    ensure_room_configurator();
    if (room_cfg_ui_ && room_cfg_ui_->is_locked()) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "[RoomEditor] Room configurator is locked; toggle ignored.");
        return;
    }
    set_room_config_visible(!is_room_config_open());
}

void RoomEditor::open_room_config() {
    ensure_room_configurator();
    if (room_cfg_ui_ && room_cfg_ui_->is_locked()) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "[RoomEditor] Room configurator is locked; open request ignored.");
        return;
    }
    set_room_config_visible(true);
}

void RoomEditor::open_room_config_for(Asset* asset) {
    if (!asset || asset->spawn_id.empty()) {
        open_room_config();
        return;
    }
    set_room_config_visible(true);
    if (room_cfg_ui_) {
        room_cfg_ui_->focus_spawn_group(asset->spawn_id);
    }
}

void RoomEditor::close_room_config() {
    set_room_config_visible(false);
}

bool RoomEditor::is_room_config_open() const {
    return room_config_dock_open_;
}

bool RoomEditor::is_camera_settings_open() const {
    return room_cfg_ui_ && room_cfg_ui_->camera_controls_enabled();
}

void RoomEditor::regenerate_room() {
    if (room_cfg_ui_ && room_cfg_ui_->is_locked()) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "[RoomEditor] Room configurator is locked; regeneration skipped.");
        return;
    }
    regenerate_current_room();
}

void RoomEditor::regenerate_room_from_template(Room* source_room) {
    if (room_cfg_ui_ && room_cfg_ui_->is_locked()) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "[RoomEditor] Room configurator is locked; regeneration from template skipped.");
        return;
    }
    if (!assets_ || !current_room_ || !source_room) return;

    nlohmann::json template_root = source_room->assets_data();
    auto& template_groups = ensure_spawn_groups_array(template_root);
    const int template_resolution = current_room_ ? current_room_->map_grid_settings().grid_resolution : MapGridSettings::defaults().grid_resolution;
    for (auto& entry : template_groups) {
        if (!entry.is_object()) continue;
        entry["spawn_id"] = generate_spawn_id();
        devmode::spawn::ensure_spawn_group_entry_defaults(
            entry,
            entry.contains("display_name") && entry["display_name"].is_string()
                ? entry["display_name"].get<std::string>()
                : std::string{"New Spawn"},
            template_resolution);
    }

    sanitize_perimeter_spawn_groups(template_groups);

    auto& target_root = current_room_->assets_data();
    nlohmann::json preserved_identity = nlohmann::json::object();
    static const std::array<const char*, 3> preserved_keys{ "name", "key", "room_key" };
    for (const char* key : preserved_keys) {
        if (target_root.contains(key)) {
            preserved_identity[key] = target_root[key];
        }
    }

    target_root = std::move(template_root);

    for (auto& [key, value] : preserved_identity.items()) {
        target_root[key] = value;
    }

    regenerate_current_room();

    rebuild_room_spawn_id_cache();
    save_current_room_assets_json();
}

void RoomEditor::focus_camera_on_asset(Asset* asset, double height_factor, int duration_steps) {
    if (!asset || !assets_) return;

    if (info_ui_ && info_ui_->is_visible()) {
        return;
    }

    WarpedScreenGrid& cam = assets_->getView();
    cam.set_manual_height_override(true);
    cam.set_focus_override(asset->world_xz_point());
    cam.animate_height_to_scale(height_factor * 1000.0, duration_steps);
    mark_spatial_index_dirty();
}

void RoomEditor::focus_camera_on_room_center(bool reframe_height) {
    if (!enabled_ || !assets_) return;
    if (!current_room_ || !current_room_->room_area) return;

    WarpedScreenGrid& cam = assets_->getView();
    const SDL_Point room_center = current_room_->room_area->get_center();
    const SDL_Point center{
        room_center.x + current_room_->camera_center_dx,
        room_center.y + current_room_->camera_center_dz
    };
    cam.set_manual_height_override(true);
    cam.set_focus_override(center);

    if (reframe_height) {
        cam.animate_height_to_scale(cam.default_camera_height_for_room(current_room_));
    }
    mark_spatial_index_dirty();
}

void RoomEditor::reset_click_state() {
    click_buffer_frames_ = 0;
    rclick_buffer_frames_ = 0;
    suppress_next_left_click_ = false;
    last_click_asset_ = nullptr;
    last_click_time_ms_ = 0;
    reset_drag_state();
}

void RoomEditor::clear_selection() {
    if (anchor_mode_active()) {
        exit_anchor_edit_mode(true);
    }
    clear_geometry_selection();
    const bool had_selection = !selected_assets_.empty();
    const bool had_highlight = !highlighted_assets_.empty();
    const bool had_hover = hovered_asset_ != nullptr;
    selected_assets_.clear();
    highlighted_assets_.clear();
    hovered_asset_ = nullptr;
    reset_drag_state();
    sync_spawn_group_panel_with_selection();
    if (had_selection || had_highlight || had_hover) {
        mark_highlight_dirty();
    }
    if (!active_assets_) return;
    for (Asset* asset : *active_assets_) {
        if (!asset) continue;
        asset->set_selected(false);
        asset->set_highlighted(false);
    }
}

void RoomEditor::clear_highlighted_assets() {
    const bool had_highlight = !highlighted_assets_.empty();
    const size_t prev_selection_size = selected_assets_.size();
    Asset* prev_hover = hovered_asset_;
    highlighted_assets_.clear();
    if (!active_assets_) {
        selected_assets_.clear();
        hovered_asset_ = nullptr;
        if (had_highlight || prev_selection_size != selected_assets_.size() || hovered_asset_ != prev_hover) {
            mark_highlight_dirty();
        }
        return;
    }
    auto erase_if_inactive = [this](Asset* asset) {
        if (!asset) return true;
        auto it = std::find(active_assets_->begin(), active_assets_->end(), asset);
        if (it == active_assets_->end()) {
            asset->set_highlighted(false);
            asset->set_selected(false);
            return true;
        }
        return false;
};

    selected_assets_.erase( std::remove_if(selected_assets_.begin(), selected_assets_.end(), erase_if_inactive), selected_assets_.end());

    if (hovered_asset_ && erase_if_inactive(hovered_asset_)) {
        hovered_asset_ = nullptr;
        hover_miss_frames_ = 0;
    }

    for (Asset* asset : *active_assets_) {
        if (!asset) {
            continue;
        }
        asset->set_highlighted(false);
        const bool is_selected = std::find(selected_assets_.begin(), selected_assets_.end(), asset) != selected_assets_.end();
        asset->set_selected(is_selected);
    }
    sync_spawn_group_panel_with_selection();
    if (had_highlight || prev_selection_size != selected_assets_.size() || hovered_asset_ != prev_hover) {
        mark_highlight_dirty();
    }
}

void RoomEditor::purge_asset(Asset* asset) {
    if (!asset) return;
    if (anchor_mode_active() && anchor_edit_.target_asset == asset) {
        exit_anchor_edit_mode(true);
    }
    bool highlight_sources_changed = false;
    if (hovered_asset_ == asset) {
        hovered_asset_ = nullptr;
        hover_miss_frames_ = 0;
        highlight_sources_changed = true;
    }
    remove_asset_from_spatial_index(asset);
    auto erase_from = [asset, &highlight_sources_changed](std::vector<Asset*>& vec) {
        const auto before = vec.size();
        vec.erase(std::remove(vec.begin(), vec.end(), asset), vec.end());
        if (vec.size() != before) {
            highlight_sources_changed = true;
        }
};
    erase_from(selected_assets_);
    erase_from(highlighted_assets_);
    if (drag_anchor_asset_ == asset) {
        drag_anchor_asset_ = nullptr;
        dragging_ = false;
    }
    drag_states_.erase(std::remove_if(drag_states_.begin(), drag_states_.end(),
                                      [asset](const DraggedAssetState& state) { return state.asset == asset; }),
                       drag_states_.end());
    if (drag_states_.empty()) {
        reset_drag_state();
    }
    sync_spawn_group_panel_with_selection();
    if (highlight_sources_changed) {
        mark_highlight_dirty();
    }
}


void RoomEditor::set_pointer_queries_suspended(bool suspended) {
    if (pointer_queries_suspended_ == suspended) {
        return;
    }
    pointer_queries_suspended_ = suspended;
    if (suspended) {
        hovered_asset_ = nullptr;
        hover_miss_frames_ = 3;
    }
    mark_highlight_dirty();
}

void RoomEditor::set_height_scale_factor(double factor) {
    height_scale_factor_ = (factor > 0.0) ? factor : 1.0;
    camera_controls_.set_height_scale_factor(height_scale_factor_);
}

void RoomEditor::apply_asset_scale_live_update(Asset* asset, int scale_percent) {
    if (!asset || !asset->info) {
        return;
    }

    const int clamped_percent = std::clamp(scale_percent, 1, 400);
    asset->info->set_scale_percentage(static_cast<float>(clamped_percent));
    asset->on_scale_factor_changed();

    if (save_coordinator_ && manifest_store_) {
        nlohmann::json payload = asset->info->manifest_payload();
        save_coordinator_->enqueue_manifest_asset(asset->info->name,
                                                  std::move(payload),
                                                  devmode::core::DevSaveCoordinator::Priority::Debounced,
                                                  "Scale",
                                                  [this]() {
                                                      if (assets_) {
                                                          assets_->mark_active_assets_dirty();
                                                      }
                                                  });
    } else if (manifest_store_) {
        auto session = manifest_store_->begin_asset_edit(asset->info->name, true);
        if (!session) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "[RoomEditor] Failed to open manifest session for '%s'",
                        asset->info->name.c_str());
            return;
        }
        session.data() = asset->info->manifest_payload();
        if (!session.commit()) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "[RoomEditor] Failed to commit manifest for '%s'",
                        asset->info->name.c_str());
            return;
        }
        if (assets_) {
            assets_->mark_active_assets_dirty();
        }
    }
}

bool RoomEditor::apply_scroll_size_adjustment(const Input& input) {
    const int scroll_y = input.getScrollY();
    if (scroll_y == 0) {
        return false;
    }

    Asset* primary = selected_assets_.empty() ? nullptr : selected_assets_.front();
    if (!primary || !primary->info) {
        return false;
    }

    const int ticks = std::abs(scroll_y);
    if (ticks <= 0) {
        return false;
    }

    const int direction = (scroll_y < 0) ? 1 : -1;
    const int current_percent = std::max(1, static_cast<int>(std::lround(primary->info->scale_factor * 100.0f)));
    const int next_percent = current_percent + (direction * ticks);
    apply_asset_scale_live_update(primary, next_percent);
    return true;
}

bool RoomEditor::select_asset_or_group(Asset* asset) {
    if (!asset || !asset_belongs_to_room(asset)) {
        return false;
    }

    clear_geometry_selection();
    selected_assets_.clear();

    bool select_group = !asset->spawn_id.empty();
    const std::string& method = asset->spawn_method;
    if (method == "Exact" || method == "Exact Position" || method == "Percent") {
        select_group = false;
    }

    if (select_group && active_assets_) {
        for (Asset* candidate : *active_assets_) {
            if (!candidate) continue;
            if (!asset_belongs_to_room(candidate)) continue;
            if (candidate->spawn_id == asset->spawn_id) {
                selected_assets_.push_back(candidate);
            }
        }
    } else {
        selected_assets_.push_back(asset);
    }

    hovered_asset_ = selected_assets_.empty() ? asset : selected_assets_.front();
    sync_spawn_group_panel_with_selection();
    mark_highlight_dirty();
    return !selected_assets_.empty();
}

Asset* RoomEditor::selected_asset_within_interaction_radius(SDL_Point screen_point) const {
    if (selected_assets_.empty() || !assets_) {
        return nullptr;
    }

    const WarpedScreenGrid& cam = assets_->getView();
    ensure_spatial_index(cam);
    constexpr int kInteractionRadiusPx = 24;
    const int kInteractionRadius2 = kInteractionRadiusPx * kInteractionRadiusPx;

    Asset* best = nullptr;
    int best_dist2 = std::numeric_limits<int>::max();
    int best_area = std::numeric_limits<int>::max();
    int best_screen_y = std::numeric_limits<int>::max();
    for (Asset* asset : selected_assets_) {
        if (!asset || asset->dead) continue;
        SDL_Rect bounds{0, 0, 0, 0};
        int screen_y = 0;
        bool have_bounds = false;

        auto bounds_it = asset_bounds_cache_.find(asset);
        if (bounds_it != asset_bounds_cache_.end()) {
            bounds = bounds_it->second.bounds;
            screen_y = bounds_it->second.screen_y;
            have_bounds = screen_rect_is_reasonable(bounds);
        }
        if (!have_bounds) {
            have_bounds = compute_asset_screen_bounds(cam, asset, bounds, screen_y);
        }

        int dist2 = std::numeric_limits<int>::max();
        int area = std::numeric_limits<int>::max();
        if (have_bounds) {
            const int min_x = bounds.x;
            const int max_x = bounds.x + bounds.w - 1;
            const int min_y = bounds.y;
            const int max_y = bounds.y + bounds.h - 1;
            const int closest_x = std::clamp(screen_point.x, min_x, max_x);
            const int closest_y = std::clamp(screen_point.y, min_y, max_y);
            const int dx = closest_x - screen_point.x;
            const int dy = closest_y - screen_point.y;
            dist2 = dx * dx + dy * dy;
            area = bounds.w * bounds.h;
        } else {
            SDL_Point gp = grid_point_for_asset(asset);
            SDL_FPoint screen_f = cam.map_to_screen(gp);
            if (!std::isfinite(screen_f.x) || !std::isfinite(screen_f.y)) {
                continue;
            }
            const int dx = static_cast<int>(std::lround(screen_f.x)) - screen_point.x;
            const int dy = static_cast<int>(std::lround(screen_f.y)) - screen_point.y;
            dist2 = dx * dx + dy * dy;
            screen_y = static_cast<int>(std::lround(screen_f.y));
        }

        const bool is_better =
            dist2 < best_dist2 ||
            (dist2 == best_dist2 && screen_y < best_screen_y) ||
            (dist2 == best_dist2 && screen_y == best_screen_y && area < best_area);
        if (dist2 <= kInteractionRadius2 && is_better) {
            best = asset;
            best_dist2 = dist2;
            best_screen_y = screen_y;
            best_area = area;
        }
    }

    return best;
}

bool RoomEditor::delete_selected_asset_or_group() {
    Asset* primary = selected_assets_.empty() ? nullptr : selected_assets_.front();
    if (!primary) {
        return false;
    }

    const std::string spawn_id = primary->spawn_id;
    if (spawn_id.empty()) {
        return false;
    }

    if (!delete_spawn_group_internal(spawn_id)) {
        return false;
    }

    clear_selection();
    show_notice("Deleted spawn group");
    return true;
}

bool RoomEditor::handle_camera_settings_mouse_controls(const Input& input) {
    if (!camera_settings_lock_active_ || !room_cfg_ui_ || !room_cfg_ui_->camera_controls_enabled()) {
        return false;
    }

    const bool shift_down =
        input.isScancodeDown(SDL_SCANCODE_LSHIFT) || input.isScancodeDown(SDL_SCANCODE_RSHIFT);
    bool consumed = false;
    RoomConfigurator::CameraAdjustment adjustment{};

    const int scroll_y = input.getScrollY();
    if (scroll_y != 0) {
        const int ticks = std::abs(scroll_y);
        const int direction = (scroll_y > 0) ? 1 : -1;
        if (shift_down) {
            adjustment.zoom_delta_percent = direction * kCameraZoomScrollStep * ticks;
        } else {
            adjustment.height_delta_px = direction * kCameraHeightScrollStep * ticks;
        }
        consumed = true;
    }

    if (!camera_settings_drag_.active) {
        if (input.wasPressed(Input::LEFT)) {
            camera_settings_drag_.active = true;
            camera_settings_drag_.button = Input::LEFT;
            camera_settings_drag_.mode = shift_down ? CameraSettingsDragState::Mode::Pan
                                                   : CameraSettingsDragState::Mode::Tilt;
            consumed = true;
        } else if (input.wasPressed(Input::RIGHT)) {
            camera_settings_drag_.active = true;
            camera_settings_drag_.button = Input::RIGHT;
            camera_settings_drag_.mode = CameraSettingsDragState::Mode::Pan;
            consumed = true;
        }
    }

    if (camera_settings_drag_.active) {
        const bool button_down = input.isDown(camera_settings_drag_.button);
        if (!button_down) {
            camera_settings_drag_.active = false;
            camera_settings_drag_.mode = CameraSettingsDragState::Mode::None;
            consumed = true;
        }
    }

    if (camera_settings_drag_.active) {
        const int dy = input.getDY();
        if (dy != 0) {
            const float delta = -static_cast<float>(dy);
            switch (camera_settings_drag_.mode) {
                case CameraSettingsDragState::Mode::Tilt:
                    adjustment.tilt_delta_deg = delta * kCameraTiltDegreesPerPixel;
                    consumed = true;
                    break;
                case CameraSettingsDragState::Mode::Pan:
                    adjustment.pan_delta_percent = delta * kCameraPanPercentPerPixel;
                    consumed = true;
                    break;
                case CameraSettingsDragState::Mode::None:
                    break;
            }
        }
    }

    if (consumed) {
        room_cfg_ui_->apply_camera_adjustment(adjustment);
    }

    if (assets_) {
        if (camera_settings_drag_.active && !camera_settings_drag_active_notified_) {
            assets_->notify_camera_activity(true);
            camera_settings_drag_active_notified_ = true;
        } else if (!camera_settings_drag_.active && camera_settings_drag_active_notified_) {
            assets_->notify_camera_activity(false);
            camera_settings_drag_active_notified_ = false;
        }
    }

    return consumed;
}

bool RoomEditor::is_spawn_group_panel_visible() const {
    return spawn_group_panel_ && spawn_group_panel_->is_visible();
}

void RoomEditor::set_blocking_panel_visible(BlockingPanel panel, bool visible) {
    const size_t index = static_cast<size_t>(panel);
    if (index >= blocking_panel_visible_.size()) {
        return;
    }
    blocking_panel_visible_[index] = visible;
}

bool RoomEditor::any_blocking_panel_visible() const {
    return std::any_of(blocking_panel_visible_.begin(),
                       blocking_panel_visible_.end(),
                       [](bool state) { return state; });
}

float RoomEditor::edge_pan_intensity(int value, int max_value, float threshold_fraction) {
    if (max_value <= 1) {
        return 0.0f;
    }
    const float clamped_fraction = std::clamp(threshold_fraction, 0.0f, 0.5f);
    const float threshold = static_cast<float>(max_value) * clamped_fraction;
    if (threshold <= 1e-3f) {
        return 0.0f;
    }
    const float signed_penetration = (value < 0)
        ? std::min(threshold, static_cast<float>(-value))
        : std::max(0.0f, threshold - static_cast<float>(value));
    if (signed_penetration <= 0.0f) {
        return 0.0f;
    }
    const float linear = std::clamp(signed_penetration / threshold, 0.0f, 1.0f);
    return std::pow(linear, kShiftEdgePanExponent);
}

bool RoomEditor::apply_shift_edge_pan(const Input& input, WarpedScreenGrid& cam) {
    const bool dragging_asset = dragging_;
    if (!dragging_asset || screen_w_ <= 1 || screen_h_ <= 1) {
        return false;
    }

    const SDL_Point screen_pt{input.getX(), input.getY()};
    if (is_ui_blocking_input(screen_pt.x, screen_pt.y)) {
        return false;
    }

    // Project the screen center and cursor onto the warped floor so the threshold
    // respects world-space distances instead of raw screen pixels.
    const SDL_Point screen_center{ screen_w_ / 2, screen_h_ / 2 };
    const SDL_FPoint center_world = cam.screen_to_map(screen_center);
    const SDL_FPoint cursor_world = cam.screen_to_map(screen_pt);
    if (!std::isfinite(center_world.x) || !std::isfinite(center_world.y) ||
        !std::isfinite(cursor_world.x) || !std::isfinite(cursor_world.y)) {
        return false;
    }

    // Use the distance from the screen center to a point near the bottom of the screen
    // to define the outer (max) half-extent of the panning square.
    const int bottom_sample_y = std::clamp(
        static_cast<int>(std::lround(static_cast<double>(screen_h_ - 1) * kShiftEdgePanBottomSampleInset)),
        0,
        std::max(0, screen_h_ - 1));
    const SDL_FPoint bottom_world = cam.screen_to_map(SDL_Point{ screen_w_ / 2, bottom_sample_y });
    const double outer_half_extent = std::hypot(
        static_cast<double>(bottom_world.x) - static_cast<double>(center_world.x),
        static_cast<double>(bottom_world.y) - static_cast<double>(center_world.y));
    if (!std::isfinite(outer_half_extent) || outer_half_extent <= 1e-6) {
        return false;
    }

    const double inner_half_extent = outer_half_extent * 0.98; // 95% sized inner square

    // Chebyshev distance gives us a square region (max(|dx|, |dy|)).
    const double offset_x = static_cast<double>(cursor_world.x) - static_cast<double>(center_world.x);
    const double offset_y = static_cast<double>(cursor_world.y) - static_cast<double>(center_world.y);
    const double cheb_dist = std::max(std::abs(offset_x), std::abs(offset_y));

    const double ramp_den = std::max(outer_half_extent - inner_half_extent, 1e-9);
    double intensity = (cheb_dist - inner_half_extent) / ramp_den;
    intensity = std::clamp(intensity, 0.0, 1.0);
    if (intensity <= 0.0) {
        return false;
    }

    const double dir_len = std::hypot(offset_x, offset_y);
    if (!std::isfinite(dir_len) || dir_len <= 1e-6) {
        return false;
    }

    const double world_dir_x = offset_x / dir_len;
    const double world_dir_y = offset_y / dir_len;

    constexpr float kFallbackDtSeconds = 1.0f / 60.0f;
    const double world_step = static_cast<double>(kShiftEdgePanMaxSpeedBottomWorldUnitsPerSecond) *
                              intensity *
                              static_cast<double>(kFallbackDtSeconds);

    const SDL_Point before_center = cam.get_screen_center();
    SDL_Point target_center = before_center;
    target_center.x = static_cast<int>(std::lround(static_cast<double>(target_center.x) + world_dir_x * world_step));
    target_center.y = static_cast<int>(std::lround(static_cast<double>(target_center.y) + world_dir_y * world_step));

    if (target_center.x == before_center.x && target_center.y == before_center.y) {
        return false;
    }

    cam.set_focus_override(target_center);
    cam.set_screen_center(target_center);
    return true;
}

void RoomEditor::handle_mouse_input(const Input& input) {
    if (!input_) return;

    if (anchor_mode_active()) {
        handle_anchor_mode_mouse_input(input);
        return;
    }

    const bool shift_down =
        input.isScancodeDown(SDL_SCANCODE_LSHIFT) || input.isScancodeDown(SDL_SCANCODE_RSHIFT);
    const bool space_down = input.isScancodeDown(SDL_SCANCODE_SPACE);
    const bool shift_space_down = shift_down && space_down;
    const bool shift_down_just_pressed = shift_down && !shift_was_down_last_frame_;
    const bool shift_space_just_pressed = shift_space_down && !shift_space_was_down_last_frame_;

    shift_was_down_last_frame_ = shift_down;
    shift_space_was_down_last_frame_ = shift_space_down;

    if (handle_camera_settings_mouse_controls(input)) {
        return;
    }

    const bool consumed_scroll_for_scale = apply_scroll_size_adjustment(input);
    if (consumed_scroll_for_scale && input_) {
        input_->consumeScroll();
    }

    const int scroll_y = consumed_scroll_for_scale ? 0 : input.getScrollY();
    const bool camera_scroll_event = (scroll_y != 0);
    if (camera_scroll_event && assets_) {
        assets_->notify_camera_activity(true);
        assets_->notify_camera_activity(false);
    }

    WarpedScreenGrid& cam = assets_->getView();
    const float prev_scale = cam.get_scale();
    const SDL_Point prev_center = cam.get_screen_center();

    const SDL_Point screen_pt{ input_->getX(), input_->getY() };
    const bool left_down                = input_->isDown(Input::LEFT);
    const bool left_pressed_this_frame  = input_->wasPressed(Input::LEFT);
    const bool left_released_this_frame = input_->wasReleased(Input::LEFT);

    Asset* hit_before_pan = hit_test_asset(screen_pt, nullptr);
    if (!selected_assets_.empty()) {
        hit_before_pan = selected_asset_within_interaction_radius(screen_pt);
    } else if (!shift_down) {
        hit_before_pan = nullptr;
    }
    const bool has_selection = !selected_assets_.empty();
    const bool has_geometry_selection = selected_geometry_room_ != nullptr;
    const bool selection_interaction_active = shift_down || has_selection || has_geometry_selection;
    const bool selection_blocks_camera_pan = has_selection || has_geometry_selection;
    const bool pointer_blocks_pan = selection_blocks_camera_pan ||
                                    (!selection_interaction_active && dragging_) ||
                                    (selection_interaction_active && !dragging_ && hit_before_pan && (left_down || left_pressed_this_frame));

    if (shift_down_just_pressed) {
        reset_selection_filter();
    }
    if (shift_space_just_pressed) {
        cycle_selection_filter();
    }

    if (selection_blocks_camera_pan && camera_controls_.is_panning()) {
        // Stop any in-progress camera drag as soon as a selection exists so asset dragging has priority.
        camera_controls_.cancel(cam);
    }

    if (!camera_settings_lock_active_) {
        camera_controls_.handle_input(cam, input, pointer_blocks_pan);
        if (dragging_ && !camera_controls_.is_panning()) {
            apply_shift_edge_pan(input, cam);
        }
    }
    if (std::fabs(cam.get_scale() - prev_scale) > 1e-6 ||
        cam.get_screen_center().x != prev_center.x ||
        cam.get_screen_center().y != prev_center.y) {
        mark_spatial_index_dirty();
    }

    if (assets_) {
        const bool now_panning = camera_controls_.is_panning();
        if (now_panning) {
            if (!camera_pan_active_notified_) {
                camera_pan_active_notified_ = true;
                assets_->notify_camera_activity(true);
            }
        } else if (camera_pan_active_notified_) {
            camera_pan_active_notified_ = false;
            camera_pan_just_finished_ = true;
            suppress_left_click_frames_ = 2;
            if (input_) {
                input_->consumeMouseButton(Input::LEFT);
            }
            assets_->notify_camera_activity(false);
        }
    }

    if (camera_pan_just_finished_ && input_) {
        input_->clearClickBuffer();
    }

    const SDL_FPoint world_f = cam.screen_to_map(screen_pt);
    SDL_Point world_pt{ (int)std::lround(world_f.x), (int)std::lround(world_f.y) };

    last_raw_mouse_world_ = world_pt;
    has_last_raw_mouse_world_ = true;

    cursor_snap_resolution_ = snap_to_grid_enabled_ ? current_grid_resolution() : 0;
    snapped_cursor_world_ = snap_to_grid_enabled_
        ? snap_world_point_to_overlay_grid(world_pt, cursor_snap_resolution_)
        : world_pt;

    hovered_geometry_room_ = (shift_down && !selected_geometry_room_ && selected_assets_.empty())
        ? find_geometry_room_at_point(world_pt)
        : hovered_geometry_room_;

    if (selected_geometry_room_ && left_down) {
        if (geometry_drag_handle_ == GeometryHandle::None) {
            geometry_drag_handle_ = hit_test_geometry_handle(selected_geometry_room_, world_pt);
        }
        if (geometry_drag_handle_ != GeometryHandle::None) {
            auto& root = selected_geometry_room_->assets_data();
            const SDL_Point center = selected_geometry_room_->room_area ? selected_geometry_room_->room_area->get_center() : SDL_Point{0, 0};
            const double dx = static_cast<double>(world_pt.x - center.x);
            const double dy = static_cast<double>(world_pt.y - center.y);
            int candidate = static_cast<int>(std::lround(std::max(std::abs(dx), std::abs(dy)) * 2.0));
            candidate = std::max(1, candidate);
            const int prev_min_w = std::max(1, root.value("min_width", 1));
            const int prev_max_w = std::max(prev_min_w, root.value("max_width", prev_min_w));
            const int prev_min_h = std::max(1, root.value("min_height", prev_min_w));
            const int prev_max_h = std::max(prev_min_h, root.value("max_height", prev_max_w));
            int min_w = prev_min_w;
            int max_w = prev_max_w;
            if (geometry_drag_handle_ == GeometryHandle::Min) {
                min_w = std::min(candidate, max_w);
                if (candidate > max_w) max_w = candidate;
            } else {
                max_w = std::max(candidate, min_w);
                if (candidate < min_w) min_w = candidate;
            }
            const int new_min_h = min_w;
            const int new_max_h = max_w;
            const bool width_changed = (min_w != prev_min_w) || (max_w != prev_max_w);
            const bool height_changed = (new_min_h != prev_min_h) || (new_max_h != prev_max_h);
            if (width_changed || height_changed) {
                root["min_width"] = min_w;
                root["max_width"] = max_w;
                root["min_height"] = new_min_h;
                root["max_height"] = new_max_h;
                geometry_drag_pending_dirty_ = true;
            }
        }
    } else if (!left_down) {
        if (geometry_drag_pending_dirty_ && selected_geometry_room_) {
            mark_geometry_dirty(selected_geometry_room_);
        }
        geometry_drag_pending_dirty_ = false;
        geometry_drag_handle_ = GeometryHandle::None;
    }

    Asset* hit = hit_test_asset(screen_pt, nullptr);
    if (!selected_assets_.empty()) {
        hit = selected_asset_within_interaction_radius(screen_pt);
    } else if (!shift_down) {
        hit = nullptr;
    }

    auto rebuild_highlight = [this]() {
        highlighted_assets_.clear();
        if (!selected_assets_.empty()) {
            highlighted_assets_.insert(highlighted_assets_.end(), selected_assets_.begin(), selected_assets_.end());
        }
        if (hovered_asset_) {
            if (std::find(highlighted_assets_.begin(),
                          highlighted_assets_.end(),
                          hovered_asset_) == highlighted_assets_.end()) {
                highlighted_assets_.push_back(hovered_asset_);
            }
        }
        mark_highlight_dirty();
};

    static bool       prev_left_down = false;
    static SDL_Point  press_screen   = {0,0};
    static Asset*     pressed_asset  = nullptr;
    static bool       was_dragged    = false;
    static const int  kDragPx        = 4;

    if (!shift_down && selected_assets_.empty() && !left_down && !dragging_) {
        pressed_asset = nullptr;
        was_dragged = false;
    }

    if (suppress_next_left_click_) {
        if (click_buffer_frames_ > 0) {
            --click_buffer_frames_;
        } else {
            suppress_next_left_click_ = false;
        }
    }

    if (selection_interaction_active && left_down && !prev_left_down) {

        Asset* selection_hit = nullptr;
        if (!selected_assets_.empty()) {
            selection_hit = selected_asset_within_interaction_radius(screen_pt);
        } else if (shift_down) {
            selection_hit = hit_test_asset(screen_pt, nullptr);
        }

        if (!selection_hit && !selected_assets_.empty()) {
            clear_selection();
        }

        pressed_asset = selection_hit;
        was_dragged   = false;
        press_screen  = screen_pt;

        if (pressed_asset) {
            select_asset_or_group(pressed_asset);
            rebuild_highlight();
        } else {

            if (!selected_assets_.empty() || !highlighted_assets_.empty() || hovered_asset_) {
                selected_assets_.clear();
                highlighted_assets_.clear();
                hovered_asset_ = nullptr;
                sync_spawn_group_panel_with_selection();
                mark_highlight_dirty();
            }
        }
    }

    if (left_down && pressed_asset) {
        const int dx = screen_pt.x - press_screen.x;
        const int dy = screen_pt.y - press_screen.y;
        const int dist2 = dx*dx + dy*dy;

        if (!was_dragged && selection_interaction_active && dist2 > kDragPx*kDragPx) {
            was_dragged = true;
            dragging_ = true;
            drag_last_world_ = snapped_cursor_world_;
            const bool ctrl_modifier = input.isScancodeDown(SDL_SCANCODE_LCTRL) || input.isScancodeDown(SDL_SCANCODE_RCTRL);
            begin_drag_session(snapped_cursor_world_, ctrl_modifier);
        }

        if (was_dragged && dragging_) {
            update_drag_session(snapped_cursor_world_);

            if (hovered_asset_ != pressed_asset) {
                hovered_asset_ = pressed_asset;
                rebuild_highlight();
            }
        }
    }

    if (!left_down && prev_left_down && pressed_asset) {
        if (pressed_asset) {
            if (was_dragged) {

                if (dragging_) {
                    finalize_drag_session();
                    dragging_ = false;
                }

                suppress_next_left_click_ = true;
                click_buffer_frames_      = 3;

                sync_spawn_group_panel_with_selection();
                rebuild_highlight();
            } else {

                if (hovered_asset_ == pressed_asset) {
                    hovered_asset_ = pressed_asset;
                    rebuild_highlight();
                }

                suppress_next_left_click_ = true;
                click_buffer_frames_      = 2;
            }
        }

        pressed_asset = nullptr;
        was_dragged   = false;
    }

    if (!dragging_ && selected_assets_.empty() && !selected_geometry_room_) {
        Asset* hover_candidate = shift_down ? hit : nullptr;
        if (hovered_asset_ != hover_candidate) {
            hovered_asset_ = hover_candidate;
            rebuild_highlight();
        }
    } else if (!dragging_ && !selected_assets_.empty()) {
        Asset* hover_candidate = selected_asset_within_interaction_radius(screen_pt);
        if (!hover_candidate) {
            hover_candidate = selected_assets_.front();
        }
        if (hovered_asset_ != hover_candidate) {
            hovered_asset_ = hover_candidate;
            rebuild_highlight();
        }
    }

    const bool any_left_activity = left_pressed_this_frame || left_released_this_frame || left_down;
    const bool suppress_click_now = suppress_left_click_frames_ > 0 || camera_pan_just_finished_;
    if (!dragging_ && !suppress_next_left_click_ && !any_left_activity && !suppress_click_now) {
        handle_click(input);
    }

    if (suppress_left_click_frames_ > 0) {
        --suppress_left_click_frames_;
    }
    camera_pan_just_finished_ = false;

    prev_left_down = left_down;
}

Asset* RoomEditor::hit_test_asset(SDL_Point screen_point, SDL_Renderer* ) const {
    if (!active_assets_ || !assets_) return nullptr;

    const WarpedScreenGrid& cam = assets_->getView();
    const SDL_Point cursor_grid = grid_point_for_screen(cam, screen_point);

    if (!ensure_spatial_index(cam)) {

        return hit_test_asset_fallback(cam, screen_point);
    }

    const std::vector<Asset*> candidates = gather_candidate_assets_for_point(screen_point);
    if (!candidates.empty()) {
        Asset* best = nullptr;
        int best_bottom = std::numeric_limits<int>::max();
        int best_top = std::numeric_limits<int>::max();
        int best_screen_y = std::numeric_limits<int>::max();
        int best_area = std::numeric_limits<int>::max();
        int best_dist2 = std::numeric_limits<int>::max();

        auto consider_candidate = [&](Asset* asset,
                                      const SDL_Rect& rect,
                                      int screen_y) {
            if (!asset) return;
            if (!asset->spawn_id.empty() && spawn_group_locked(asset->spawn_id)) {
                return;
            }
            if (!asset_matches_selection_filter(asset)) {
                return;
            }
            if (!SDL_PointInRect(&screen_point, &rect)) {
                return;
            }
            const SDL_Point asset_grid = grid_point_for_asset(asset);
            const int dx = asset_grid.x - cursor_grid.x;
            const int dy = asset_grid.y - cursor_grid.y;
            const int dist2 = dx * dx + dy * dy;
            const int bottom = rect.y + rect.h;
            const int top = rect.y;
            const int area = rect.w * rect.h;
            const bool is_better =
                !best ||
                dist2 < best_dist2 ||
                (dist2 == best_dist2 && bottom < best_bottom) ||
                (dist2 == best_dist2 && bottom == best_bottom && top < best_top) ||
                (dist2 == best_dist2 && bottom == best_bottom && top == best_top && screen_y < best_screen_y) ||
                (dist2 == best_dist2 && bottom == best_bottom && top == best_top && screen_y == best_screen_y && area < best_area);
            if (is_better) {
                best = asset;
                best_bottom = bottom;
                best_top = top;
                best_screen_y = screen_y;
                best_dist2 = dist2;
                best_area = area;
            }
        };

        for (Asset* asset : candidates) {
            auto bc = asset_bounds_cache_.find(asset);
            if (bc == asset_bounds_cache_.end()) {
                continue;
            }
            const AssetSpatialEntry& entry = bc->second;
            consider_candidate(asset, entry.bounds, entry.screen_y);
        }

        if (best) {
            return best;
        }
    }

    return hit_test_asset_fallback(cam, screen_point);
}

void RoomEditor::mark_spatial_index_dirty() const {
    spatial_index_dirty_ = true;
    cached_camera_state_valid_ = false;
    asset_bounds_cache_.clear();
    spatial_grid_.clear();
}

bool RoomEditor::camera_state_changed(const WarpedScreenGrid& cam) const {
    if (!cached_camera_state_valid_) {
        return false;
    }
    const float scale = cam.get_scale();
    if (std::fabs(scale - cached_camera_scale_) > kCameraScaleEpsilon) {
        return true;
    }
    SDL_Point center = cam.get_screen_center();
    if (center.x != cached_camera_center_.x || center.y != cached_camera_center_.y) {
        return true;
    }
    const double zoom_percent = cam.get_zoom_percent();
    if (std::fabs(zoom_percent - cached_camera_zoom_percent_) > kCameraProjectionEpsilon) {
        return true;
    }
    const float pitch_deg = cam.current_pitch_degrees();
    if (std::fabs(static_cast<double>(pitch_deg) - static_cast<double>(cached_camera_pitch_deg_)) > kCameraProjectionEpsilon) {
        return true;
    }
    const double anchor_world_z = cam.current_anchor_world_z();
    if (std::fabs(anchor_world_z - cached_camera_anchor_world_z_) > kCameraProjectionEpsilon) {
        return true;
    }

    return false;
}

bool RoomEditor::ensure_spatial_index(const WarpedScreenGrid& cam) const {
    if (!active_assets_) {
        return false;
    }

    if (camera_state_changed(cam)) {
        mark_spatial_index_dirty();
    }

    if (spatial_index_dirty_) {
        rebuild_spatial_index(cam);
    }

    return !spatial_index_dirty_;
}

bool RoomEditor::compute_asset_render_package_bounds(const WarpedScreenGrid& cam,
                                                     Asset* asset,
                                                     SDL_Rect& out_rect) const {
    if (!asset) {
        return false;
    }
    if (asset->render_package.empty()) {
        return false;
    }

    float min_x = std::numeric_limits<float>::infinity();
    float min_y = std::numeric_limits<float>::infinity();
    float max_x = -std::numeric_limits<float>::infinity();
    float max_y = -std::numeric_limits<float>::infinity();
    SDL_FPoint base_screen{};
    bool have_bounds = false;
    const float base_depth = static_cast<float>(asset->world_z());
    const auto* gp = cam.grid_point_for_asset(asset);
    const float perspective_scale =
        (gp && std::isfinite(gp->perspective_scale) && gp->perspective_scale > 0.0f)
            ? gp->perspective_scale
            : 1.0f;

    for (const auto& obj : asset->render_package) {
        if (obj.screen_rect.w <= 0 || obj.screen_rect.h <= 0) {
            continue;
        }

        const float object_world_z = base_depth + obj.world_z_offset;
        render_projection::ProjectedSpriteFrame projection{};
        if (build_render_object_projection(cam, obj, perspective_scale, object_world_z, projection)) {
            const float obj_min_x = std::min(std::min(projection.screen_tl.x, projection.screen_tr.x),
                                             std::min(projection.screen_bl.x, projection.screen_br.x));
            const float obj_max_x = std::max(std::max(projection.screen_tl.x, projection.screen_tr.x),
                                             std::max(projection.screen_bl.x, projection.screen_br.x));
            const float obj_min_y = std::min(std::min(projection.screen_tl.y, projection.screen_tr.y),
                                             std::min(projection.screen_bl.y, projection.screen_br.y));
            const float obj_max_y = std::max(std::max(projection.screen_tl.y, projection.screen_tr.y),
                                             std::max(projection.screen_bl.y, projection.screen_br.y));
            if (std::isfinite(obj_min_x) && std::isfinite(obj_max_x) &&
                std::isfinite(obj_min_y) && std::isfinite(obj_max_y) &&
                obj_max_x > obj_min_x && obj_max_y > obj_min_y) {
                min_x = std::min(min_x, obj_min_x);
                min_y = std::min(min_y, obj_min_y);
                max_x = std::max(max_x, obj_max_x);
                max_y = std::max(max_y, obj_max_y);
                have_bounds = true;
                continue;
            }
        }

        SDL_FPoint world_point{
            static_cast<float>(obj.screen_rect.x),
            static_cast<float>(obj.screen_rect.y)
        };
        if (!cam.project_world_point(world_point, object_world_z, base_screen)) {
            continue;
        }
        const float half_width = static_cast<float>(obj.screen_rect.w) * 0.5f;
        const float height = static_cast<float>(obj.screen_rect.h);
        if (!std::isfinite(base_screen.x) || !std::isfinite(base_screen.y) ||
            !std::isfinite(half_width) || !std::isfinite(height)) {
            continue;
        }

        const float left = base_screen.x - half_width;
        const float right = base_screen.x + half_width;
        const float top = base_screen.y - height;
        const float bottom = base_screen.y;
        if (!std::isfinite(left) || !std::isfinite(right) ||
            !std::isfinite(top) || !std::isfinite(bottom)) {
            continue;
        }

        min_x = std::min(min_x, left);
        min_y = std::min(min_y, top);
        max_x = std::max(max_x, right);
        max_y = std::max(max_y, bottom);
        have_bounds = true;
    }

    if (!have_bounds) {
        return false;
    }

    const int left = static_cast<int>(std::floor(min_x));
    const int top = static_cast<int>(std::floor(min_y));
    const int right = static_cast<int>(std::ceil(max_x));
    const int bottom = static_cast<int>(std::ceil(max_y));

    const int width = std::max(1, right - left);
    const int height = std::max(1, bottom - top);

    out_rect = SDL_Rect{left, top, width, height};
    return screen_rect_is_reasonable(out_rect);
}

bool RoomEditor::compute_asset_screen_bounds(const WarpedScreenGrid& cam,
                                             Asset* asset,
                                             SDL_Rect& out_rect,
                                             int& out_screen_y) const {
    if (!asset) {
        return false;
    }

    auto* gp = cam.grid_point_for_asset(asset);
    if (!gp || !gp->on_screen) {
        return false;
    }

    if (gp->perspective_scale <= 0.0f || gp->vertical_scale <= 0.0f) {
        return false;
    }

    if (compute_asset_render_package_bounds(cam, asset, out_rect)) {
        out_screen_y = static_cast<int>(std::lround(gp->screen.y));
        return true;
    }

    SDL_Texture* tex = asset->get_current_frame();

    int fw = asset->cached_w;
    int fh = asset->cached_h;
    if ((fw == 0 || fh == 0) && tex) {
        float fwf = 0.0f;
        float fhf = 0.0f;
        if (SDL_GetTextureSize(tex, &fwf, &fhf)) {
            fw = static_cast<int>(std::lround(fwf));
            fh = static_cast<int>(std::lround(fhf));
        }
        if (asset->cached_w == 0) asset->cached_w = fw;
        if (asset->cached_h == 0) asset->cached_h = fh;
    }
    if ((fw == 0 || fh == 0) && asset->info) {
        fw = asset->info->original_canvas_width;
        fh = asset->info->original_canvas_height;
        if (asset->cached_w == 0) asset->cached_w = fw;
        if (asset->cached_h == 0) asset->cached_h = fh;
    }
    if (fw <= 0 || fh <= 0) return false;

    const float runtime_width = asset->runtime_width_px();
    const float runtime_height = asset->runtime_height_px();
    const float fallback_runtime_scale = asset->runtime_resolved_scale();

    float scaled_fw = runtime_width;
    float scaled_fh = runtime_height;
    if (!(scaled_fw > 0.0f) || !(scaled_fh > 0.0f)) {
        scaled_fw = static_cast<float>(fw) * fallback_runtime_scale;
        scaled_fh = static_cast<float>(fh) * fallback_runtime_scale;
    }

    const float screen_width = scaled_fw * gp->perspective_scale;
    const float screen_height = scaled_fh * gp->perspective_scale * gp->vertical_scale;
    if (!std::isfinite(screen_width) || !std::isfinite(screen_height) ||
        screen_width <= 0.0f || screen_height <= 0.0f) {
        return false;
    }

    const int sw = std::max(1, static_cast<int>(std::lround(static_cast<double>(screen_width))));
    const int sh = std::max(1, static_cast<int>(std::lround(static_cast<double>(screen_height))));

    const float center_x = gp->screen.x;
    const float center_y = gp->screen.y;
    const int   left     = static_cast<int>(std::lround(center_x - static_cast<float>(sw) * 0.5f));
    const int   top      = static_cast<int>(std::lround(center_y)) - sh;
    out_rect             = SDL_Rect{left, top, sw, sh};
    out_screen_y         = static_cast<int>(std::lround(center_y));
    return screen_rect_is_reasonable(out_rect);
}

void RoomEditor::rebuild_spatial_index(const WarpedScreenGrid& cam) const {
    asset_bounds_cache_.clear();
    spatial_grid_.clear();

    if (active_assets_) {
        for (Asset* asset : *active_assets_) {
            if (!asset) continue;
            SDL_Rect rect{0, 0, 0, 0};
            int screen_y = 0;
            if (!compute_asset_screen_bounds(cam, asset, rect, screen_y)) {
                continue;
            }
            insert_asset_entry(asset, rect, screen_y);
        }
    }

    cached_camera_scale_ = cam.get_scale();
    cached_camera_center_ = cam.get_screen_center();
    cached_camera_zoom_percent_ = cam.get_zoom_percent();
    cached_camera_pitch_deg_ = cam.current_pitch_degrees();
    cached_camera_anchor_world_z_ = cam.current_anchor_world_z();

    cached_camera_state_valid_ = true;
    spatial_index_dirty_ = false;
}

void RoomEditor::insert_asset_entry(Asset* asset, const SDL_Rect& rect, int screen_y) const {
    if (!asset || !screen_rect_is_reasonable(rect)) {
        return;
    }

    AssetSpatialEntry entry;
    entry.bounds = rect;
    entry.screen_y = screen_y;

    const std::int64_t right_px = static_cast<std::int64_t>(rect.x) + static_cast<std::int64_t>(rect.w) - 1;
    const std::int64_t bottom_px = static_cast<std::int64_t>(rect.y) + static_cast<std::int64_t>(rect.h) - 1;
    const std::int64_t left = floor_div_i64(rect.x, kSpatialCellSize);
    const std::int64_t right = floor_div_i64(right_px, kSpatialCellSize);
    const std::int64_t top = floor_div_i64(rect.y, kSpatialCellSize);
    const std::int64_t bottom = floor_div_i64(bottom_px, kSpatialCellSize);
    if (right < left || bottom < top) {
        return;
    }

    const std::int64_t cell_count_x = right - left + 1;
    const std::int64_t cell_count_y = bottom - top + 1;
    if (cell_count_x <= 0 || cell_count_y <= 0) {
        return;
    }
    const std::int64_t total_cells = cell_count_x * cell_count_y;
    if (total_cells > kMaxSpatialCellsPerAsset) {
        return;
    }

    for (std::int64_t cx = left; cx <= right; ++cx) {
        if (cx < std::numeric_limits<int>::min() || cx > std::numeric_limits<int>::max()) {
            continue;
        }
        for (std::int64_t cy = top; cy <= bottom; ++cy) {
            if (cy < std::numeric_limits<int>::min() || cy > std::numeric_limits<int>::max()) {
                continue;
            }
            add_asset_to_cell(asset, static_cast<int>(cx), static_cast<int>(cy), entry.cells);
        }
    }
    if (entry.cells.empty()) {
        return;
    }

    asset_bounds_cache_[asset] = std::move(entry);
}

void RoomEditor::add_asset_to_cell(Asset* asset, int cell_x, int cell_y, std::vector<int64_t>& cell_keys) const {
    if (!asset) return;
    const int64_t key = make_cell_key(cell_x, cell_y);
    auto& bucket = spatial_grid_[key];
    bucket.push_back(asset);
    cell_keys.push_back(key);
}

void RoomEditor::remove_asset_from_spatial_index(Asset* asset) const {
    if (!asset) return;
    auto it = asset_bounds_cache_.find(asset);
    if (it == asset_bounds_cache_.end()) {
        return;
    }
    const std::vector<int64_t> cells = it->second.cells;
    for (int64_t key : cells) {
        auto grid_it = spatial_grid_.find(key);
        if (grid_it == spatial_grid_.end()) {
            continue;
        }
        auto& bucket = grid_it->second;
        bucket.erase(std::remove(bucket.begin(), bucket.end(), asset), bucket.end());
        if (bucket.empty()) {
            spatial_grid_.erase(grid_it);
        }
    }
    asset_bounds_cache_.erase(it);
}

void RoomEditor::refresh_asset_spatial_entry(const WarpedScreenGrid& cam, Asset* asset) const {
    if (!asset) return;
    if (spatial_index_dirty_ || !cached_camera_state_valid_) {
        return;
    }

    remove_asset_from_spatial_index(asset);

    SDL_Rect rect{0, 0, 0, 0};
    int screen_y = 0;
    if (!compute_asset_screen_bounds(cam, asset, rect, screen_y)) {
        return;
    }
    insert_asset_entry(asset, rect, screen_y);
}

void RoomEditor::refresh_spatial_entries_for_dragged_assets() {
    if (!assets_) {
        return;
    }
    const WarpedScreenGrid& cam = assets_->getView();
    if (spatial_index_dirty_ || !cached_camera_state_valid_) {
        return;
    }

    for (const auto& state : drag_states_) {
        if (!state.asset) continue;
        refresh_asset_spatial_entry(cam, state.asset);
    }
}

void RoomEditor::sync_dragged_assets_immediately() {
    bool moved_any = false;
    for (auto& state : drag_states_) {
        Asset* asset = state.asset;
        if (!asset) {
            continue;
        }
        SDL_Point current{asset->world_x(), asset->world_z()};
        if (current.x == state.last_synced_pos.x && current.y == state.last_synced_pos.y) {
            continue;
        }
        asset->clear_grid_residency_cache();
        asset->sync_transform_to_position();
        asset->mark_composite_dirty();
        if (assets_) {
            const world::GridPoint old_pos = world::GridPoint::make_virtual(
                state.last_synced_pos.x,
                0,
                state.last_synced_pos.y,
                asset->grid_resolution);
            const world::GridPoint new_pos = world::GridPoint::make_virtual(
                current.x,
                0,
                current.y,
                asset->grid_resolution);
            (void)assets_->world_grid().move_asset(asset, old_pos, new_pos);
        }
        state.last_synced_pos = current;
        moved_any = true;
    }
    if (moved_any && assets_) {
        assets_->mark_active_assets_dirty();
    }
}

std::vector<Asset*> RoomEditor::gather_candidate_assets_for_point(SDL_Point screen_point) const {
    std::vector<Asset*> result;
    if (spatial_grid_.empty()) {
        return result;
    }

    const int cell_x = floor_div(screen_point.x, kSpatialCellSize);
    const int cell_y = floor_div(screen_point.y, kSpatialCellSize);
    std::unordered_set<Asset*> unique;
    unique.reserve(16);

    for (int dx = -1; dx <= 1; ++dx) {
        for (int dy = -1; dy <= 1; ++dy) {
            const int64_t key = make_cell_key(cell_x + dx, cell_y + dy);
            auto it = spatial_grid_.find(key);
            if (it == spatial_grid_.end()) {
                continue;
            }
            for (Asset* asset : it->second) {
                if (!asset) continue;
                if (unique.insert(asset).second) {
                    result.push_back(asset);
                }
            }
        }
    }

    return result;
}

Asset* RoomEditor::hit_test_asset_fallback(const WarpedScreenGrid& cam, SDL_Point screen_point) const {
    if (!active_assets_) {
        return nullptr;
    }

    const SDL_Point cursor_grid = grid_point_for_screen(cam, screen_point);
    Asset* best = nullptr;
    int best_bottom = std::numeric_limits<int>::max();
    int best_top = std::numeric_limits<int>::max();
    int best_screen_y = std::numeric_limits<int>::max();
    int best_area = std::numeric_limits<int>::max();
    int best_dist2 = std::numeric_limits<int>::max();

    auto consider_candidate = [&](Asset* asset,
                                  const SDL_Rect& rect,
                                  int screen_y) {
        if (!asset) return;
        if (!asset->spawn_id.empty() && spawn_group_locked(asset->spawn_id)) {
            return;
        }
        if (!asset_matches_selection_filter(asset)) {
            return;
        }
        if (!SDL_PointInRect(&screen_point, &rect)) {
            return;
        }
        const SDL_Point asset_grid = grid_point_for_asset(asset);
        const int dx = asset_grid.x - cursor_grid.x;
        const int dy = asset_grid.y - cursor_grid.y;
        const int dist2 = dx * dx + dy * dy;
        const int bottom = rect.y + rect.h;
        const int top = rect.y;
        const int area = rect.w * rect.h;
        const bool is_better =
            !best ||
            dist2 < best_dist2 ||
            (dist2 == best_dist2 && bottom < best_bottom) ||
            (dist2 == best_dist2 && bottom == best_bottom && top < best_top) ||
            (dist2 == best_dist2 && bottom == best_bottom && top == best_top && screen_y < best_screen_y) ||
            (dist2 == best_dist2 && bottom == best_bottom && top == best_top && screen_y == best_screen_y && area < best_area);
        if (is_better) {
            best = asset;
            best_bottom = bottom;
            best_top = top;
            best_screen_y = screen_y;
            best_dist2 = dist2;
            best_area = area;
        }
};

    for (Asset* asset : *active_assets_) {
        if (!asset) continue;
        if (!asset->spawn_id.empty() && spawn_group_locked(asset->spawn_id)) {
            continue;
        }
        if (!asset_matches_selection_filter(asset)) {
            continue;
        }

        SDL_Rect rect{0, 0, 0, 0};
        int screen_y = 0;
        if (!compute_asset_screen_bounds(cam, asset, rect, screen_y)) {
            continue;
        }

        consider_candidate(asset, rect, screen_y);
    }

    return best;
}

void RoomEditor::update_hover_state(Asset* hit) {
    if (pointer_queries_suspended_) {
        if (hovered_asset_) {
            hovered_asset_ = nullptr;
            hover_miss_frames_ = 3;
            mark_highlight_dirty();
        }
        return;
    }

    Asset* previous = hovered_asset_;
    if (hit) {
        hovered_asset_ = hit;
        hover_miss_frames_ = 0;
    } else {
        if (++hover_miss_frames_ >= 3) {
            hovered_asset_ = nullptr;
            hover_miss_frames_ = 3;
        }
    }
    if (hovered_asset_ != previous) {
        mark_highlight_dirty();
    }
}

Room* RoomEditor::find_geometry_room_at_point(SDL_Point world_point) const {
    if (!assets_) {
        return nullptr;
    }
    Room* best = nullptr;
    int best_layer = std::numeric_limits<int>::min();
    for (Room* room : assets_->rooms()) {
        if (!room || !room->room_area) continue;
        if (!room->room_area->contains_point(world_point)) continue;
        if (!best || room->layer >= best_layer) {
            best = room;
            best_layer = room->layer;
        }
    }
    return best;
}

bool RoomEditor::geometry_room_is_trail(const Room* room) const {
    if (!room) return false;
    std::string lowered = room->type;
    std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return lowered == "trail";
}

RoomEditor::GeometryHandle RoomEditor::hit_test_geometry_handle(Room* room, SDL_Point world_point) const {
    if (!room || !room->room_area) return GeometryHandle::None;
    const auto& root = room->assets_data();
    const bool is_circle = [&]() {
        std::string g = root.value("geometry", std::string{"square"});
        std::transform(g.begin(), g.end(), g.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        return g == "circle";
    }();

    const int min_w = std::max(1, root.value("min_width", 1));
    const int max_w = std::max(min_w, root.value("max_width", min_w));
    const SDL_Point c = room->room_area->get_center();
    const double dx = static_cast<double>(world_point.x - c.x);
    const double dy = static_cast<double>(world_point.y - c.y);
    const double dist = std::hypot(dx, dy);
    const double metric = is_circle ? dist : std::max(std::abs(dx), std::abs(dy));
    const double min_edge = static_cast<double>(min_w) * 0.5;
    const double max_edge = static_cast<double>(max_w) * 0.5;
    const double tol = std::max(12.0, static_cast<double>(max_w) * 0.05);

    if (std::abs(metric - max_edge) <= tol) return GeometryHandle::Max;
    if (std::abs(metric - min_edge) <= tol) return GeometryHandle::Min;
    return GeometryHandle::None;
}

bool RoomEditor::is_point_between_geometry_bounds(Room* room, SDL_Point world_point) const {
    if (!room || !room->room_area) return false;
    const auto& root = room->assets_data();
    const bool is_circle = [&]() {
        std::string g = root.value("geometry", std::string{"square"});
        std::transform(g.begin(), g.end(), g.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        return g == "circle";
    }();
    const int min_w = std::max(1, root.value("min_width", 1));
    int max_w = std::max(min_w, root.value("max_width", min_w));
    if (geometry_room_is_trail(room)) {
        max_w = std::max(max_w, root.value("max_height", max_w));
    }

    const SDL_Point c = room->room_area->get_center();
    const double dx = static_cast<double>(world_point.x - c.x);
    const double dy = static_cast<double>(world_point.y - c.y);
    const double metric = is_circle ? std::hypot(dx, dy) : std::max(std::abs(dx), std::abs(dy));
    return metric >= static_cast<double>(min_w) * 0.5 && metric <= static_cast<double>(max_w) * 0.5;
}

void RoomEditor::clear_geometry_selection() {
    if (geometry_drag_pending_dirty_ && selected_geometry_room_) {
        mark_geometry_dirty(selected_geometry_room_);
    }
    geometry_drag_pending_dirty_ = false;
    hovered_geometry_room_ = nullptr;
    selected_geometry_room_ = nullptr;
    geometry_drag_handle_ = GeometryHandle::None;
}

void RoomEditor::mark_geometry_dirty(Room* room) {
    if (!room) return;
    room->mark_dirty();
    mark_spatial_index_dirty();
    if (mark_map_dirty_callback_) {
        mark_map_dirty_callback_(devmode::core::DevSaveCoordinator::Priority::Debounced);
    }
}

bool RoomEditor::regenerate_geometry(Room* room) {
    if (!room || !room->room_area || !assets_) return false;
    auto& root = room->assets_data();
    const int min_w = std::max(1, root.value("min_width", 1));
    const int max_w = std::max(min_w, root.value("max_width", min_w));
    int chosen = min_w;
    if (max_w - min_w >= 2) {
        std::mt19937 rng(std::random_device{}());
        chosen = std::uniform_int_distribution<int>(min_w + 1, max_w - 1)(rng);
    } else {
        chosen = max_w;
    }

    const int edge_smoothness = root.value("edge_smoothness", 2);
    std::string geometry = root.value("geometry", std::string{"Square"});
    if (!geometry.empty()) geometry[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(geometry[0])));
    const SDL_Point center = room->room_area->get_center();
    const int map_w = std::max(1, std::abs(center.x) * 2 + max_w * 4);
    const int map_h = std::max(1, std::abs(center.y) * 2 + max_w * 4);
    try {
        room->room_area = std::make_unique<Area>(room->room_name, center, chosen, chosen, geometry, edge_smoothness, map_w, map_h, room->room_area->resolution());
        room->room_area->set_type(room->type);
    } catch (...) {
        return false;
    }

    std::vector<Asset*> to_delete;
    to_delete.reserve(assets_->all.size());
    for (Asset* asset : assets_->all) {
        if (!asset || asset->dead || asset == assets_->player) continue;
        if (asset->owning_room_name() == room->room_name) {
            to_delete.push_back(asset);
            continue;
        }
        if (room->room_area->contains_point(asset->world_xz_point())) {
            to_delete.push_back(asset);
        }
    }
    if (!to_delete.empty()) {
        auto batch = assets_->begin_world_mutation_batch();
        for (Asset* asset : to_delete) {
            batch.mark_for_deletion(asset);
        }
        (void)batch.commit();
    }

    Room* previous = current_room_;
    current_room_ = room;
    regenerate_current_room();
    current_room_ = previous;

    assets_->invalidate_dynamic_boundary_system();
    mark_geometry_dirty(room);
    return true;
}

std::optional<std::string> RoomEditor::find_room_area_at_point(SDL_Point world_point) {
    if (!current_room_) {
        return std::nullopt;
    }

    nlohmann::json& root = current_room_->assets_data();

    struct AreaMetadata {
        int z = 0;
        bool visible = true;
        std::size_t order = 0;
};

    std::unordered_map<std::string, AreaMetadata> metadata;
    std::size_t order_counter = 0;

    if (root.is_object() && root.contains("areas") && root["areas"].is_array()) {
        for (const auto& entry : root["areas"]) {
            if (!entry.is_object()) {
                ++order_counter;
                continue;
            }

            std::string name = entry.value("name", std::string{});
            if (name.empty()) {
                ++order_counter;
                continue;
            }

            AreaMetadata data;
            data.z = entry.value("z", 0);
            data.visible = !(entry.contains("visible") && entry["visible"].is_boolean() && !entry["visible"].get<bool>());
            data.order = order_counter;
            metadata.insert_or_assign(name, data);

            ++order_counter;
        }
    }

    std::size_t fallback_order = order_counter;
    std::optional<std::string> best_name;
    int best_z = std::numeric_limits<int>::min();
    std::size_t best_order = 0;
    bool have_best_order = false;

    auto consider_area = [&](const std::string& name, const Area* area) {
        if (!area) {
            return;
        }

        auto it = metadata.find(name);
        AreaMetadata info;
        if (it != metadata.end()) {
            info = it->second;
        } else {
            info.order = fallback_order++;
        }

        if (!info.visible) {
            return;
        }

        if (!area->contains_point(world_point)) {
            return;
        }

        bool take = false;
        if (!best_name) {
            take = true;
        } else if (info.z > best_z) {
            take = true;
        } else if (info.z == best_z) {
            if (!have_best_order || info.order >= best_order) {
                take = true;
            }
        }

        if (take) {
            best_name = name;
            best_z = info.z;
            best_order = info.order;
            have_best_order = true;
        }
};

    for (const auto& named : current_room_->areas) {
        if (named.name.empty()) {
            continue;
        }
        consider_area(named.name, named.area.get());
    }

    return best_name;
}

void RoomEditor::handle_click(const Input& input) {
    if (!input_) return;

    SDL_Point world_mouse = snapped_cursor_world_;
    SDL_Point screen_mouse{input_->getX(), input_->getY()};

    if (suppress_next_left_click_) {
        if (input_->wasClicked(Input::LEFT)) {
            suppress_next_left_click_ = false;
            click_buffer_frames_ = 0;
            return;
        }
    }

    const bool shift_modifier =
        input.isScancodeDown(SDL_SCANCODE_LSHIFT) || input.isScancodeDown(SDL_SCANCODE_RSHIFT);

    if (input_->wasClicked(Input::RIGHT)) {
        if (rclick_buffer_frames_ > 0) {
            --rclick_buffer_frames_;
            return;
        }
        rclick_buffer_frames_ = 2;

        if (shift_modifier) {
            Room* target_geom = selected_geometry_room_ ? selected_geometry_room_ : find_geometry_room_at_point(world_mouse);
            if (target_geom && is_point_between_geometry_bounds(target_geom, world_mouse)) {
                if (geometry_room_is_trail(target_geom)) {
                    open_room_config();
                } else {
                    if (target_geom != current_room_ && assets_) {
                        assets_->set_editor_current_room(target_geom);
                    }
                    open_room_config();
                }
                return;
            }
        }

        auto open_library_at = [&](const SDL_Point& point) {
            pending_spawn_world_pos_ = point;
            open_asset_library();
            if (!is_asset_library_open()) {
                pending_spawn_world_pos_.reset();
            }
        };

        Asset* target = selected_assets_.empty() ? nullptr : selected_asset_within_interaction_radius(screen_mouse);
        if (!target && selected_assets_.empty() && shift_modifier) {
            target = hovered_asset_;
        }
        if (target) {
            map_assets_panel_requested_by_shift_click_ = shift_modifier;
            select_asset_or_group(target);
            open_asset_info_editor_for_asset(target);
            return;
        }

        bool inside_room = true;
        if (current_room_ && current_room_->room_area) {
            inside_room = current_room_->room_area->contains_point(world_mouse);
        }
        if (inside_room) {
            open_library_at(world_mouse);
        } else {
            pending_spawn_world_pos_.reset();
            open_asset_library();
        }
        return;
    } else {
        rclick_buffer_frames_ = 0;
    }

    if (!input_->wasClicked(Input::LEFT)) {
        click_buffer_frames_ = 0;
        return;
    }

    click_buffer_frames_ = std::max(0, click_buffer_frames_ - 1);

    const bool asset_info_open =
        (active_modal_ == ActiveModal::AssetInfo) || (info_ui_ && info_ui_->is_visible());
    const bool floating_modal_open = DockManager::instance().active_panel() != nullptr;

    if (asset_info_open || floating_modal_open) {
        return;
    }

    if (shift_modifier) {
        Room* hit_room = find_geometry_room_at_point(world_mouse);
        if (hit_room) {
            GeometryHandle handle = hit_test_geometry_handle(hit_room, world_mouse);
            if (handle != GeometryHandle::None || is_point_between_geometry_bounds(hit_room, world_mouse)) {
                if (!selected_assets_.empty()) {
                    clear_selection();
                }
                selected_geometry_room_ = hit_room;
                hovered_geometry_room_ = hit_room;
                geometry_drag_handle_ = handle;

                const Uint32 now = SDL_GetTicks();
                if (geometry_last_click_ms_ != 0 && now - geometry_last_click_ms_ <= 300) {
                    regenerate_geometry(hit_room);
                }
                geometry_last_click_ms_ = now;
                return;
            }
        }
        if (selected_geometry_room_) {
            clear_geometry_selection();
            return;
        }
    }

    if (!selected_assets_.empty()) {
        if (!selected_asset_within_interaction_radius(screen_mouse)) {
            clear_selection();
        }
        return;
    }

    if (!shift_modifier) {
        if (selected_geometry_room_) {
            clear_geometry_selection();
        }
        return;
    }

    Asset* clicked_asset = nullptr;

    if (assets_ && active_assets_) {
        const WarpedScreenGrid& cam = assets_->getView();
        constexpr int kPickRadiusPx = 8;
        const int kPickRadius2 = kPickRadiusPx * kPickRadiusPx;
        int best_dist2 = std::numeric_limits<int>::max();
        for (Asset* asset : *active_assets_) {
            if (!asset || asset->dead) continue;
            if (!asset_belongs_to_room(asset)) continue;
            if (!asset_matches_selection_filter(asset)) continue;
            if (!asset->spawn_id.empty() && spawn_group_locked(asset->spawn_id)) continue;
            SDL_Point gp = grid_point_for_asset(asset);
            SDL_FPoint screen_f = cam.map_to_screen(gp);
            if (!std::isfinite(screen_f.x) || !std::isfinite(screen_f.y)) continue;
            int dx = static_cast<int>(std::lround(screen_f.x)) - screen_mouse.x;
            int dy = static_cast<int>(std::lround(screen_f.y)) - screen_mouse.y;
            int dist2 = dx * dx + dy * dy;
            if (dist2 <= kPickRadius2 && dist2 < best_dist2) {
                clicked_asset = asset;
                best_dist2 = dist2;
            }
        }
    }

    if (clicked_asset) {
        map_assets_panel_requested_by_shift_click_ = shift_modifier;
        select_asset_or_group(clicked_asset);
    } else {
        bool inside_room = true;
        if (current_room_ && current_room_->room_area) {
            inside_room = current_room_->room_area->contains_point(world_mouse);
        }

        if (!inside_room && assets_) {
            for (Room* r : assets_->rooms()) {
                if (!r || r == current_room_ || !r->room_area) continue;
                if (r->room_area->contains_point(world_mouse)) {
                    assets_->set_editor_current_room(r);
                    break;
                }
            }
        }
    }
}

nlohmann::json* RoomEditor::find_area_entry_json(Room* room, const std::string& area_name) const {
    if (!room) return nullptr;
    nlohmann::json& root = room->assets_data();
    if (root.is_object() && root.contains("areas") && root["areas"].is_array()) {
        for (auto& entry : root["areas"]) {
            if (!entry.is_object()) continue;
            if (entry.value("name", std::string{}) == area_name) {
                return &entry;
            }
        }
    }
    return nullptr;
}

void RoomEditor::ensure_area_anchor_spawn_entry(Room* room, const std::string& area_name) {
    if (!room) return;
    nlohmann::json& root = room->assets_data();
    auto& groups = ensure_spawn_groups_array(root);
    nlohmann::json* existing = nullptr;
    for (auto& entry : groups) {
        if (!entry.is_object()) continue;
        const bool linked = entry.value("link_to_area", false);
        const std::string linked_area = entry.value("linked_area", std::string{});
        const std::string display = entry.value("display_name", std::string{});
        if ((linked && linked_area == area_name) || (!linked && display == area_name)) {
            existing = &entry;
            break;
        }
    }
    const int default_resolution = room->map_grid_settings().grid_resolution;
    int width = 0, height = 0;
    if (room->room_area) {
        auto b = room->room_area->get_bounds();
        width = std::max(1, std::get<2>(b) - std::get<0>(b));
        height = std::max(1, std::get<3>(b) - std::get<1>(b));
    }
    if (!existing) {
        nlohmann::json entry = nlohmann::json::object();
        entry["display_name"] = area_name;
        entry["position"] = "Exact";
        entry["dx"] = 0;
        entry["dz"] = 0;
        if (width > 0) entry["origional_width"] = width;
        if (height > 0) entry["origional_height"] = height;
        entry["link_to_area"] = true;
        entry["linked_area"] = area_name;
        devmode::spawn::ensure_spawn_group_entry_defaults(entry, area_name, default_resolution);
        groups.push_back(std::move(entry));
        save_current_room_assets_json();
    } else {
        devmode::spawn::ensure_spawn_group_entry_defaults(*existing, area_name, default_resolution);
        if (existing->value("position", std::string{"Random"}) != std::string{"Exact"}) {
            (*existing)["position"] = "Exact";
        }
        if (width > 0 && !existing->contains("origional_width")) (*existing)["origional_width"] = width;
        if (height > 0 && !existing->contains("origional_height")) (*existing)["origional_height"] = height;
        if (!existing->value("link_to_area", false)) (*existing)["link_to_area"] = true;
        if (existing->value("linked_area", std::string{}) != area_name) (*existing)["linked_area"] = area_name;
        save_current_room_assets_json();
    }
}

void RoomEditor::begin_area_drag_session(const std::string& area_name, const SDL_Point& world_mouse) {
    area_dragging_ = true;
    area_drag_moved_ = false;
    area_drag_name_ = area_name;
    area_drag_last_world_ = world_mouse;
    area_drag_start_world_ = world_mouse;
    MapGridSettings map_settings = current_room_ ? current_room_->map_grid_settings() : MapGridSettings::defaults();
    map_settings.clamp();
    area_drag_resolution_ = snap_to_grid_enabled_
        ? vibble::grid::clamp_resolution(map_settings.grid_resolution)
        : 0;

    ensure_area_anchor_spawn_entry(current_room_, area_drag_name_);
}

void RoomEditor::update_area_drag_session(const SDL_Point& world_mouse) {
    area_drag_last_world_ = world_mouse;
    area_drag_moved_ = true;
}

void RoomEditor::finalize_area_drag_session() {
    if (!current_room_ || area_drag_name_.empty()) {
        area_dragging_ = false;
        area_drag_moved_ = false;
        return;
    }

    vibble::grid::Grid& grid_service = vibble::grid::global_grid();
    SDL_Point snapped = area_drag_last_world_;
    if (snap_to_grid_enabled_ && area_drag_resolution_ > 0) {
        snapped = grid_service.snap_to_vertex(area_drag_last_world_, area_drag_resolution_);
    }

    SDL_Point center{0, 0};
    if (current_room_->room_area) {
        auto c = current_room_->room_area->get_center();
        center.x = c.x; center.y = c.y;
    }
    const int dx = snapped.x - center.x;
    const int dy = snapped.y - center.y;

    if (nlohmann::json* area_entry = find_area_entry_json(current_room_, area_drag_name_)) {
        (*area_entry)["anchor_relative_to_center"] = true;
        (*area_entry)["anchor"] = nlohmann::json::object({ {"x", dx}, {"y", dy} });

        enqueue_current_room_save(devmode::core::DevSaveCoordinator::Priority::Debounced);
    }

    nlohmann::json& root = current_room_->assets_data();
    if (auto* groups_ptr = devmode::spawn::find_spawn_groups_array(root)) {
        for (auto& entry : const_cast<nlohmann::json&>(*groups_ptr)) {
            if (!entry.is_object()) continue;
            if (entry.value("link_to_area", false) && entry.value("linked_area", std::string{}) == area_drag_name_) {
                entry["position"] = "Exact";
                entry["dx"] = dx;
                entry["dz"] = dy;

                if (current_room_->room_area) {
                    auto b = current_room_->room_area->get_bounds();
                    const int w = std::max(1, std::get<2>(b) - std::get<0>(b));
                    const int h = std::max(1, std::get<3>(b) - std::get<1>(b));
                    if (!entry.contains("origional_width")) entry["origional_width"] = w;
                    if (!entry.contains("origional_height")) entry["origional_height"] = h;
                }
                break;
            }
        }
    }

    save_current_room_assets_json();
    area_dragging_ = false;
    area_drag_moved_ = false;
}

void RoomEditor::update_highlighted_assets() {
    if (!highlight_dirty_) {
        return;
    }
    highlight_dirty_ = false;
    if (!active_assets_) return;

    highlighted_assets_.clear();

    if (selected_geometry_room_) {
        selected_assets_.clear();
        hovered_asset_ = nullptr;
    } else if (!selected_assets_.empty()) {
        // When something is selected, lock highlights to the selection.
        highlighted_assets_ = selected_assets_;
    } else if (hovered_asset_ && asset_belongs_to_room(hovered_asset_) &&
               (hovered_asset_->spawn_id.empty() || !spawn_group_locked(hovered_asset_->spawn_id))) {
        // No selection: allow hover highlighting (plus its spawn group).
        highlighted_assets_.push_back(hovered_asset_);
        if (!hovered_asset_->spawn_id.empty()) {
            for (Asset* asset : *active_assets_) {
                if (!asset_belongs_to_room(asset)) continue;
                if (asset->spawn_id != hovered_asset_->spawn_id) continue;
                if (spawn_group_locked(asset->spawn_id)) continue;
                if (std::find(highlighted_assets_.begin(), highlighted_assets_.end(), asset) == highlighted_assets_.end()) {
                    highlighted_assets_.push_back(asset);
                }
            }
        }
    }

    for (Asset* asset : *active_assets_) {
        if (!asset) continue;
        asset->set_highlighted(false);
        asset->set_selected(false);
    }

    for (Asset* asset : highlighted_assets_) {
        if (!asset) continue;
        if (std::find(selected_assets_.begin(), selected_assets_.end(), asset) != selected_assets_.end()) {
            asset->set_selected(true);
            asset->set_highlighted(false);
        } else {
            asset->set_highlighted(is_shift_key_down());
            asset->set_selected(false);
        }
    }
}

bool RoomEditor::anchor_mode_active() const {
    return editor_mode_ == EditorMode::AnchorEdit;
}

Asset* RoomEditor::selected_anchor_mode_asset() const {
    if (selected_assets_.size() != 1) {
        return nullptr;
    }
    return selected_assets_.front();
}

void RoomEditor::ensure_anchor_editor_widgets() {
    if (!anchor_mode_toggle_button_) {
        anchor_mode_toggle_button_ = std::make_unique<DMButton>(
            "Edit Anchors",
            &DMStyles::AccentButton(),
            140,
            DMButton::height());
    }
    if (!anchor_anim_prev_button_) {
        anchor_anim_prev_button_ = std::make_unique<DMButton>(
            "^",
            &DMStyles::HeaderButton(),
            kAnchorNavButtonSize,
            kAnchorNavButtonSize);
    }
    if (!anchor_anim_next_button_) {
        anchor_anim_next_button_ = std::make_unique<DMButton>(
            "v",
            &DMStyles::HeaderButton(),
            kAnchorNavButtonSize,
            kAnchorNavButtonSize);
    }
    if (!anchor_frame_prev_button_) {
        anchor_frame_prev_button_ = std::make_unique<DMButton>(
            "<",
            &DMStyles::HeaderButton(),
            kAnchorNavButtonSize,
            kAnchorNavButtonSize);
    }
    if (!anchor_frame_next_button_) {
        anchor_frame_next_button_ = std::make_unique<DMButton>(
            ">",
            &DMStyles::HeaderButton(),
            kAnchorNavButtonSize,
            kAnchorNavButtonSize);
    }
    if (!anchor_tools_panel_) {
        anchor_tools_panel_ = std::make_unique<RoomAnchorToolsPanel>();
        anchor_tools_panel_->set_on_select([this](const std::string& name) {
            anchor_edit_.selected_anchor_name = name;
            sync_anchor_tools_panel();
        });
        anchor_tools_panel_->set_on_add([this]() {
            add_anchor_in_current_frame();
        });
        anchor_tools_panel_->set_on_rename([this](const std::string& name) {
            rename_selected_anchor_in_current_frame(name);
        });
        anchor_tools_panel_->set_on_delete([this]() {
            delete_selected_anchor_in_current_frame();
        });
    }

    if (anchor_tools_panel_) {
        anchor_tools_panel_->set_screen_dimensions(screen_w_, screen_h_);
        anchor_tools_panel_->set_visible(anchor_mode_active());
    }
}

bool RoomEditor::should_show_anchor_mode_toggle() const {
    if (!enabled_) {
        return false;
    }
    return anchor_mode_active() || (selected_anchor_mode_asset() != nullptr);
}

void RoomEditor::update_anchor_editor_layout() {
    ensure_anchor_editor_widgets();
    if (!anchor_mode_toggle_button_) {
        return;
    }

    anchor_mode_toggle_button_->set_text(anchor_mode_active() ? "Done Anchors" : "Edit Anchors");
    const int toggle_w = std::max(140, anchor_mode_toggle_button_->preferred_width());
    anchor_mode_toggle_button_->set_rect(SDL_Rect{
        std::max(0, kAnchorUiEdgeMargin),
        kAnchorUiTopMargin,
        toggle_w,
        DMButton::height(),
    });

    const int center_x = screen_w_ / 2;
    const int top_y = kAnchorUiTopMargin;
    const int center_button_x = center_x - (kAnchorNavButtonSize / 2);
    const int middle_row_y = top_y + kAnchorNavButtonSize + kAnchorNavGap;

    if (anchor_anim_prev_button_) {
        anchor_anim_prev_button_->set_rect(SDL_Rect{
            center_button_x,
            top_y,
            kAnchorNavButtonSize,
            kAnchorNavButtonSize,
        });
    }
    if (anchor_frame_prev_button_) {
        anchor_frame_prev_button_->set_rect(SDL_Rect{
            center_button_x - kAnchorNavButtonSize - kAnchorNavGap,
            middle_row_y,
            kAnchorNavButtonSize,
            kAnchorNavButtonSize,
        });
    }
    if (anchor_frame_next_button_) {
        anchor_frame_next_button_->set_rect(SDL_Rect{
            center_button_x + kAnchorNavButtonSize + kAnchorNavGap,
            middle_row_y,
            kAnchorNavButtonSize,
            kAnchorNavButtonSize,
        });
    }
    if (anchor_anim_next_button_) {
        anchor_anim_next_button_->set_rect(SDL_Rect{
            center_button_x,
            middle_row_y + kAnchorNavButtonSize + kAnchorNavGap,
            kAnchorNavButtonSize,
            kAnchorNavButtonSize,
        });
    }

    if (anchor_tools_panel_) {
        anchor_tools_panel_->set_screen_dimensions(screen_w_, screen_h_);
        anchor_tools_panel_->set_visible(anchor_mode_active());
    }
}

void RoomEditor::toggle_anchor_edit_mode() {
    if (anchor_mode_active()) {
        exit_anchor_edit_mode(true);
        return;
    }
    enter_anchor_edit_mode();
}

std::vector<std::string> RoomEditor::anchor_mode_animation_names() const {
    std::vector<std::string> names;
    if (!anchor_mode_active() || !anchor_edit_.target_asset || !anchor_edit_.target_asset->info) {
        return names;
    }
    for (const auto& [name, animation] : anchor_edit_.target_asset->info->animations) {
        if (!animation.frames.empty()) {
            names.push_back(name);
        }
    }
    return names;
}

int RoomEditor::resolve_anchor_mode_frame_index() const {
    if (!anchor_mode_active() || !anchor_edit_.target_asset || !anchor_edit_.target_asset->info) {
        return 0;
    }
    auto anim_it = anchor_edit_.target_asset->info->animations.find(anchor_edit_.animation_id);
    if (anim_it == anchor_edit_.target_asset->info->animations.end() || anim_it->second.frames.empty()) {
        return 0;
    }
    for (std::size_t i = 0; i < anim_it->second.frames.size(); ++i) {
        if (anim_it->second.frames[i] == anchor_edit_.target_asset->current_frame) {
            return static_cast<int>(i);
        }
    }
    return devmode::room_anchor_mode::wrap_index(anchor_edit_.frame_index,
                                                  static_cast<int>(anim_it->second.frames.size()));
}

bool RoomEditor::apply_anchor_animation_and_frame(const std::string& animation_id, int frame_index) {
    if (!anchor_mode_active() || !anchor_edit_.target_asset || !anchor_edit_.target_asset->info) {
        return false;
    }

    Asset* target = anchor_edit_.target_asset;
    auto anim_it = target->info->animations.find(animation_id);
    if (anim_it == target->info->animations.end() || anim_it->second.frames.empty()) {
        return false;
    }

    const int wrapped_index =
        devmode::room_anchor_mode::wrap_index(frame_index, static_cast<int>(anim_it->second.frames.size()));

    target->set_current_animation(animation_id);
    anim_it = target->info->animations.find(animation_id);
    if (anim_it == target->info->animations.end() || anim_it->second.frames.empty()) {
        return false;
    }

    const int resolved_index =
        devmode::room_anchor_mode::wrap_index(wrapped_index, static_cast<int>(anim_it->second.frames.size()));
    AnimationFrame* frame = anim_it->second.frames[static_cast<std::size_t>(resolved_index)];
    if (!frame) {
        return false;
    }

    target->current_frame = frame;
    target->set_frame_progress(0.0f);
    target->static_frame = true;
    target->refresh_frame_texture_bindings();
    if (assets_) {
        assets_->mark_active_assets_dirty();
    }

    anchor_edit_.animation_id = animation_id;
    anchor_edit_.frame_index = resolved_index;
    return true;
}

void RoomEditor::ensure_anchor_selection_valid() {
    if (!anchor_mode_active()) {
        return;
    }
    if (anchor_edit_.handles.empty()) {
        anchor_edit_.selected_anchor_name.clear();
        anchor_edit_.hovered_anchor_name.clear();
        anchor_edit_.dragging_anchor_name.clear();
        anchor_edit_.dragging = false;
        return;
    }

    auto has_anchor = [this](const std::string& name) {
        return std::find_if(anchor_edit_.handles.begin(), anchor_edit_.handles.end(),
                            [&](const AnchorHandleSample& sample) {
                                return sample.name == name;
                            }) != anchor_edit_.handles.end();
    };

    if (anchor_edit_.selected_anchor_name.empty() || !has_anchor(anchor_edit_.selected_anchor_name)) {
        anchor_edit_.selected_anchor_name = anchor_edit_.handles.front().name;
    }
    if (!anchor_edit_.hovered_anchor_name.empty() && !has_anchor(anchor_edit_.hovered_anchor_name)) {
        anchor_edit_.hovered_anchor_name.clear();
    }
    if (anchor_edit_.dragging && !has_anchor(anchor_edit_.dragging_anchor_name)) {
        anchor_edit_.dragging = false;
        anchor_edit_.dragging_anchor_name.clear();
    }
}

void RoomEditor::sync_anchor_tools_panel() {
    ensure_anchor_editor_widgets();
    if (!anchor_tools_panel_) {
        return;
    }

    if (!anchor_mode_active()) {
        anchor_tools_panel_->set_visible(false);
        anchor_tools_panel_->set_anchor_names({});
        anchor_tools_panel_->set_selected_anchor({});
        return;
    }

    std::vector<std::string> names;
    if (anchor_edit_.target_asset && anchor_edit_.target_asset->info) {
        auto anim_it = anchor_edit_.target_asset->info->animations.find(anchor_edit_.animation_id);
        if (anim_it != anchor_edit_.target_asset->info->animations.end() && !anim_it->second.frames.empty()) {
            const int frame_index =
                devmode::room_anchor_mode::wrap_index(anchor_edit_.frame_index, static_cast<int>(anim_it->second.frames.size()));
            AnimationFrame* frame = anim_it->second.frames[static_cast<std::size_t>(frame_index)];
            names = anchor_names_for_frame(frame);
        }
    }

    anchor_tools_panel_->set_visible(true);
    anchor_tools_panel_->set_anchor_names(names);
    ensure_anchor_selection_valid();
    anchor_tools_panel_->set_selected_anchor(anchor_edit_.selected_anchor_name);
}

void RoomEditor::refresh_anchor_mode_handles() {
    anchor_edit_.handles.clear();
    if (!anchor_mode_active() || !anchor_edit_.target_asset || !anchor_edit_.target_asset->info) {
        sync_anchor_tools_panel();
        return;
    }

    auto anim_it = anchor_edit_.target_asset->info->animations.find(anchor_edit_.animation_id);
    if (anim_it == anchor_edit_.target_asset->info->animations.end() || anim_it->second.frames.empty()) {
        sync_anchor_tools_panel();
        return;
    }

    const int frame_index =
        devmode::room_anchor_mode::wrap_index(anchor_edit_.frame_index, static_cast<int>(anim_it->second.frames.size()));
    AnimationFrame* frame = anim_it->second.frames[static_cast<std::size_t>(frame_index)];
    if (!frame) {
        sync_anchor_tools_panel();
        return;
    }

    anchor_edit_.handles.reserve(frame->anchor_points.size());
    for (const auto& anchor : frame->anchor_points) {
        if (!anchor.is_valid()) {
            continue;
        }
        const anchor_points::FrameAnchorSample sample =
            anchor_points::resolve_frame_anchor_sample(*anchor_edit_.target_asset,
                                                       anchor,
                                                       anchor_points::GridMaterialization::None);
        AnchorHandleSample handle{};
        handle.name = anchor.name;
        handle.texture_x = anchor.texture_x;
        handle.texture_y = anchor.texture_y;
        handle.depth_offset = anchor.depth_offset;
        handle.flat_screen_px = sample.flat_screen_px;
        handle.has_flat_screen_px = sample.has_flat_screen_px;
        handle.final_screen_px = sample.has_final_screen_px ? sample.final_screen_px : sample.screen_px;
        handle.has_final_screen_px = sample.has_final_screen_px ||
                                     (std::isfinite(sample.screen_px.x) && std::isfinite(sample.screen_px.y));
        anchor_edit_.handles.push_back(handle);
    }

    ensure_anchor_selection_valid();
    sync_anchor_tools_panel();
}

int RoomEditor::find_anchor_handle_at_point(SDL_Point screen_point, int radius_px) const {
    if (!anchor_mode_active()) {
        return -1;
    }
    const float radius_sq = static_cast<float>(radius_px * radius_px);
    float best_dist_sq = radius_sq;
    int best_index = -1;
    for (std::size_t i = 0; i < anchor_edit_.handles.size(); ++i) {
        const AnchorHandleSample& handle = anchor_edit_.handles[i];
        if (!handle.has_final_screen_px) {
            continue;
        }
        const float dx = handle.final_screen_px.x - static_cast<float>(screen_point.x);
        const float dy = handle.final_screen_px.y - static_cast<float>(screen_point.y);
        const float dist_sq = dx * dx + dy * dy;
        if (dist_sq <= best_dist_sq) {
            best_dist_sq = dist_sq;
            best_index = static_cast<int>(i);
        }
    }
    return best_index;
}

bool RoomEditor::persist_anchor_current_frame(devmode::core::DevSaveCoordinator::Priority priority, bool flush_now) {
    if (!anchor_mode_active() || !anchor_edit_.target_asset || !anchor_edit_.target_asset->info) {
        return false;
    }

    Asset* target = anchor_edit_.target_asset;
    auto anim_it = target->info->animations.find(anchor_edit_.animation_id);
    if (anim_it == target->info->animations.end() || anim_it->second.frames.empty()) {
        return false;
    }

    const std::size_t frame_count = anim_it->second.frames.size();
    const std::size_t frame_index = static_cast<std::size_t>(
        devmode::room_anchor_mode::wrap_index(anchor_edit_.frame_index, static_cast<int>(frame_count)));
    AnimationFrame* frame = anim_it->second.frames[frame_index];
    if (!frame) {
        return false;
    }

    nlohmann::json payload = target->info->animation_payload(anchor_edit_.animation_id);
    if (!devmode::room_anchor_mode::write_anchor_frame_to_payload(payload,
                                                                  frame_count,
                                                                  frame_index,
                                                                  frame->anchor_points)) {
        return false;
    }
    if (!target->info->upsert_animation(anchor_edit_.animation_id, payload)) {
        return false;
    }
    target->info->mark_dirty();

    nlohmann::json manifest_payload = target->info->manifest_payload();
    if (save_coordinator_ && manifest_store_) {
        save_coordinator_->enqueue_manifest_asset(
            target->info->name,
            std::move(manifest_payload),
            priority,
            "Anchor Edit",
            [this]() {
                if (assets_) {
                    assets_->mark_active_assets_dirty();
                }
            });
        if (flush_now) {
            save_coordinator_->flush_now("room-anchor-edit-exit");
            anchor_edit_.dirty_since_last_flush = false;
        }
        return true;
    }

    if (manifest_store_) {
        auto session = manifest_store_->begin_asset_edit(target->info->name, true);
        if (!session) {
            return false;
        }
        session.data() = std::move(manifest_payload);
        if (!session.commit()) {
            return false;
        }
        if (assets_) {
            assets_->mark_active_assets_dirty();
        }
        anchor_edit_.dirty_since_last_flush = false;
        return true;
    }

    return false;
}

bool RoomEditor::mutate_anchor_current_frame(
    const std::function<bool(std::vector<DisplacedAssetAnchorPoint>&)>& mutator,
    devmode::core::DevSaveCoordinator::Priority priority) {
    if (!anchor_mode_active() || !anchor_edit_.target_asset || !anchor_edit_.target_asset->info) {
        return false;
    }
    auto anim_it = anchor_edit_.target_asset->info->animations.find(anchor_edit_.animation_id);
    if (anim_it == anchor_edit_.target_asset->info->animations.end() || anim_it->second.frames.empty()) {
        return false;
    }
    const int frame_index =
        devmode::room_anchor_mode::wrap_index(anchor_edit_.frame_index, static_cast<int>(anim_it->second.frames.size()));

    AnimationFrame* frame = anim_it->second.frames[static_cast<std::size_t>(frame_index)];
    if (!frame) {
        return false;
    }

    std::vector<DisplacedAssetAnchorPoint> updated = frame->anchor_points;
    if (!mutator(updated)) {
        return false;
    }

    frame->set_anchor_points(updated);

    anchor_edit_.target_asset->refresh_frame_texture_bindings();
    if (assets_) {
        assets_->mark_active_assets_dirty();
    }

    anchor_edit_.dirty_since_last_flush = true;
    refresh_anchor_mode_handles();
    persist_anchor_current_frame(priority, false);
    return true;
}

bool RoomEditor::update_anchor_depth(const std::string& anchor_name, int delta) {
    if (anchor_name.empty() || delta == 0) {
        return false;
    }
    return mutate_anchor_current_frame(
        [&](std::vector<DisplacedAssetAnchorPoint>& anchors) {
            auto it = std::find_if(anchors.begin(), anchors.end(), [&](const DisplacedAssetAnchorPoint& anchor) {
                return anchor.name == anchor_name;
            });
            if (it == anchors.end()) {
                return false;
            }
            it->depth_offset += delta;
            return true;
        },
        devmode::core::DevSaveCoordinator::Priority::Debounced);
}

bool RoomEditor::drag_anchor_to_screen(const std::string& anchor_name, SDL_Point screen_point) {
    if (anchor_name.empty() || !anchor_mode_active() || !anchor_edit_.target_asset || !anchor_edit_.target_asset->info) {
        return false;
    }

    Asset* target = anchor_edit_.target_asset;
    auto anim_it = target->info->animations.find(anchor_edit_.animation_id);
    if (anim_it == target->info->animations.end() || anim_it->second.frames.empty()) {
        return false;
    }
    const int frame_index =
        devmode::room_anchor_mode::wrap_index(anchor_edit_.frame_index, static_cast<int>(anim_it->second.frames.size()));
    AnimationFrame* frame = anim_it->second.frames[static_cast<std::size_t>(frame_index)];
    const SDL_Point frame_dims = resolve_anchor_editor_frame_dimensions(target, frame);

    return mutate_anchor_current_frame(
        [&](std::vector<DisplacedAssetAnchorPoint>& anchors) {
            auto it = std::find_if(anchors.begin(), anchors.end(), [&](const DisplacedAssetAnchorPoint& anchor) {
                return anchor.name == anchor_name;
            });
            if (it == anchors.end()) {
                return false;
            }

            int tex_x = std::clamp(it->texture_x, 0, frame_dims.x - 1);
            int tex_y = std::clamp(it->texture_y, 0, frame_dims.y - 1);

            auto sample_screen = [&](int x, int y) {
                DisplacedAssetAnchorPoint sample_anchor = *it;
                sample_anchor.texture_x = std::clamp(x, 0, frame_dims.x - 1);
                sample_anchor.texture_y = std::clamp(y, 0, frame_dims.y - 1);
                const auto sample = anchor_points::resolve_frame_anchor_sample(
                    *target,
                    sample_anchor,
                    anchor_points::GridMaterialization::None);
                return sample.has_final_screen_px ? sample.final_screen_px : sample.screen_px;
            };

            for (int iter = 0; iter < kAnchorDragIterations; ++iter) {
                const SDL_FPoint base = sample_screen(tex_x, tex_y);
                const SDL_FPoint plus_x = sample_screen(std::min(frame_dims.x - 1, tex_x + 1), tex_y);
                const SDL_FPoint plus_y = sample_screen(tex_x, std::min(frame_dims.y - 1, tex_y + 1));

                const float vx_x = plus_x.x - base.x;
                const float vx_y = plus_x.y - base.y;
                const float vy_x = plus_y.x - base.x;
                const float vy_y = plus_y.y - base.y;
                const float det = vx_x * vy_y - vx_y * vy_x;
                if (std::fabs(det) < 1e-5f) {
                    break;
                }

                const float rx = static_cast<float>(screen_point.x) - base.x;
                const float ry = static_cast<float>(screen_point.y) - base.y;
                const float dx = (rx * vy_y - ry * vy_x) / det;
                const float dy = (ry * vx_x - rx * vx_y) / det;

                const int next_x = std::clamp(static_cast<int>(std::lround(static_cast<float>(tex_x) + dx)),
                                              0,
                                              frame_dims.x - 1);
                const int next_y = std::clamp(static_cast<int>(std::lround(static_cast<float>(tex_y) + dy)),
                                              0,
                                              frame_dims.y - 1);
                if (next_x == tex_x && next_y == tex_y) {
                    break;
                }
                tex_x = next_x;
                tex_y = next_y;
            }

            int best_x = tex_x;
            int best_y = tex_y;
            float best_dist_sq = std::numeric_limits<float>::max();
            auto consider_candidate = [&](int candidate_x, int candidate_y) {
                candidate_x = std::clamp(candidate_x, 0, frame_dims.x - 1);
                candidate_y = std::clamp(candidate_y, 0, frame_dims.y - 1);
                const SDL_FPoint screen = sample_screen(candidate_x, candidate_y);
                const float diff_x = screen.x - static_cast<float>(screen_point.x);
                const float diff_y = screen.y - static_cast<float>(screen_point.y);
                const float dist_sq = diff_x * diff_x + diff_y * diff_y;
                if (dist_sq < best_dist_sq) {
                    best_dist_sq = dist_sq;
                    best_x = candidate_x;
                    best_y = candidate_y;
                }
            };

            constexpr int kLocalSearchRadius = 6;
            for (int dy = -kLocalSearchRadius; dy <= kLocalSearchRadius; ++dy) {
                for (int dx = -kLocalSearchRadius; dx <= kLocalSearchRadius; ++dx) {
                    consider_candidate(tex_x + dx, tex_y + dy);
                }
            }

            // Keep the dragged point tightly linked to the cursor even when movement between
            // frames is large by widening the texture-space search adaptively.
            constexpr float kCursorSnapToleranceSq = 0.75f * 0.75f;
            if (best_dist_sq > kCursorSnapToleranceSq) {
                const SDL_FPoint base = sample_screen(tex_x, tex_y);
                const SDL_FPoint plus_x = sample_screen(std::min(frame_dims.x - 1, tex_x + 1), tex_y);
                const SDL_FPoint plus_y = sample_screen(tex_x, std::min(frame_dims.y - 1, tex_y + 1));
                const float step_px_x = std::max(0.001f, std::hypot(plus_x.x - base.x, plus_x.y - base.y));
                const float step_px_y = std::max(0.001f, std::hypot(plus_y.x - base.x, plus_y.y - base.y));
                const float err_px_x = std::fabs(static_cast<float>(screen_point.x) - base.x);
                const float err_px_y = std::fabs(static_cast<float>(screen_point.y) - base.y);
                const int max_search_radius = std::max(frame_dims.x, frame_dims.y);
                const int adaptive_radius = std::clamp(
                    static_cast<int>(std::ceil(std::max(err_px_x / step_px_x, err_px_y / step_px_y))) + 2,
                    kLocalSearchRadius,
                    max_search_radius);

                const int coarse_step = std::max(1, adaptive_radius / 12);
                for (int dy = -adaptive_radius; dy <= adaptive_radius; dy += coarse_step) {
                    for (int dx = -adaptive_radius; dx <= adaptive_radius; dx += coarse_step) {
                        consider_candidate(tex_x + dx, tex_y + dy);
                    }
                }

                const int refine_radius = std::max(kLocalSearchRadius, coarse_step * 2);
                for (int dy = -refine_radius; dy <= refine_radius; ++dy) {
                    for (int dx = -refine_radius; dx <= refine_radius; ++dx) {
                        consider_candidate(best_x + dx, best_y + dy);
                    }
                }
            }

            if (it->texture_x == best_x && it->texture_y == best_y) {
                return false;
            }
            it->texture_x = best_x;
            it->texture_y = best_y;
            return true;
        },
        devmode::core::DevSaveCoordinator::Priority::Debounced);
}

bool RoomEditor::add_anchor_in_current_frame() {
    if (!anchor_mode_active() || !anchor_edit_.target_asset || !anchor_edit_.target_asset->info) {
        return false;
    }

    auto anim_it = anchor_edit_.target_asset->info->animations.find(anchor_edit_.animation_id);
    if (anim_it == anchor_edit_.target_asset->info->animations.end() || anim_it->second.frames.empty()) {
        return false;
    }
    const int frame_index =
        devmode::room_anchor_mode::wrap_index(anchor_edit_.frame_index, static_cast<int>(anim_it->second.frames.size()));
    AnimationFrame* frame = anim_it->second.frames[static_cast<std::size_t>(frame_index)];
    const SDL_Point dims = resolve_anchor_editor_frame_dimensions(anchor_edit_.target_asset, frame);

    std::string new_anchor_name;
    const bool changed = mutate_anchor_current_frame(
        [&](std::vector<DisplacedAssetAnchorPoint>& anchors) {
            std::vector<std::string> names;
            names.reserve(anchors.size());
            for (const auto& anchor : anchors) {
                if (anchor.is_valid()) {
                    names.push_back(anchor.name);
                }
            }
            new_anchor_name = devmode::room_anchor_mode::next_default_anchor_name(names);
            anchors.push_back(devmode::room_anchor_mode::make_default_anchor_for_frame(new_anchor_name, dims.x, dims.y));
            anchor_edit_.selected_anchor_name = new_anchor_name;
            return true;
        },
        devmode::core::DevSaveCoordinator::Priority::Debounced);
    if (changed && anchor_tools_panel_) {
        anchor_tools_panel_->set_rename_text(anchor_edit_.selected_anchor_name);
    }
    return changed;
}

bool RoomEditor::rename_selected_anchor_in_current_frame(const std::string& desired_name) {
    if (!anchor_mode_active() || anchor_edit_.selected_anchor_name.empty()) {
        return false;
    }

    return mutate_anchor_current_frame(
        [&](std::vector<DisplacedAssetAnchorPoint>& anchors) {
            auto it = std::find_if(anchors.begin(), anchors.end(), [&](const DisplacedAssetAnchorPoint& anchor) {
                return anchor.name == anchor_edit_.selected_anchor_name;
            });
            if (it == anchors.end()) {
                return false;
            }
            std::vector<std::string> names;
            names.reserve(anchors.size());
            for (const auto& anchor : anchors) {
                if (anchor.is_valid()) {
                    names.push_back(anchor.name);
                }
            }
            const std::string normalized = devmode::room_anchor_mode::make_unique_anchor_name(
                desired_name,
                names,
                anchor_edit_.selected_anchor_name);
            if (normalized.empty() || normalized == it->name) {
                return false;
            }
            it->name = normalized;
            anchor_edit_.selected_anchor_name = normalized;
            return true;
        },
        devmode::core::DevSaveCoordinator::Priority::Debounced);
}

bool RoomEditor::delete_selected_anchor_in_current_frame() {
    if (!anchor_mode_active() || anchor_edit_.selected_anchor_name.empty()) {
        return false;
    }
    return mutate_anchor_current_frame(
        [&](std::vector<DisplacedAssetAnchorPoint>& anchors) {
            const auto before = anchors.size();
            anchors.erase(std::remove_if(anchors.begin(),
                                         anchors.end(),
                                         [&](const DisplacedAssetAnchorPoint& anchor) {
                                             return anchor.name == anchor_edit_.selected_anchor_name;
                                         }),
                          anchors.end());
            if (anchors.size() == before) {
                return false;
            }
            if (anchors.empty()) {
                anchor_edit_.selected_anchor_name.clear();
            } else {
                anchor_edit_.selected_anchor_name = anchors.front().name;
            }
            return true;
        },
        devmode::core::DevSaveCoordinator::Priority::Debounced);
}

bool RoomEditor::handle_anchor_mode_mouse_input(const Input& input) {
    if (!anchor_mode_active()) {
        return false;
    }
    refresh_anchor_mode_handles();

    const SDL_Point screen_pt{input_->getX(), input_->getY()};
    const bool left_down = input_->isDown(Input::LEFT);
    const bool left_pressed = input_->wasPressed(Input::LEFT);
    const bool left_released = input_->wasReleased(Input::LEFT);

    if (left_pressed && !is_anchor_ui_blocking_point(screen_pt.x, screen_pt.y)) {
        const int hit = find_anchor_handle_at_point(screen_pt, kAnchorHandlePickRadiusPx);
        if (hit >= 0 && static_cast<std::size_t>(hit) < anchor_edit_.handles.size()) {
            anchor_edit_.selected_anchor_name = anchor_edit_.handles[static_cast<std::size_t>(hit)].name;
            anchor_edit_.dragging_anchor_name = anchor_edit_.selected_anchor_name;
            anchor_edit_.dragging = true;
            sync_anchor_tools_panel();
        }
    }

    if (anchor_edit_.dragging && left_down) {
        if (!is_anchor_ui_blocking_point(screen_pt.x, screen_pt.y)) {
            drag_anchor_to_screen(anchor_edit_.dragging_anchor_name, screen_pt);
        }
    }

    if (left_released) {
        anchor_edit_.dragging = false;
        anchor_edit_.dragging_anchor_name.clear();
    }

    if (!anchor_edit_.dragging) {
        const int hover = find_anchor_handle_at_point(screen_pt, kAnchorHandlePickRadiusPx);
        if (hover >= 0 && static_cast<std::size_t>(hover) < anchor_edit_.handles.size()) {
            anchor_edit_.hovered_anchor_name = anchor_edit_.handles[static_cast<std::size_t>(hover)].name;
        } else {
            anchor_edit_.hovered_anchor_name.clear();
        }
    }

    const int scroll_y = input.getScrollY();
    if (scroll_y != 0 && !is_anchor_ui_blocking_point(screen_pt.x, screen_pt.y)) {
        ensure_anchor_selection_valid();
        if (!anchor_edit_.selected_anchor_name.empty() &&
            update_anchor_depth(anchor_edit_.selected_anchor_name, scroll_y) &&
            input_) {
            input_->consumeScroll();
        }
    }

    return true;
}

void RoomEditor::navigate_anchor_animation(int delta) {
    if (!anchor_mode_active() || delta == 0) {
        return;
    }
    std::vector<std::string> names = anchor_mode_animation_names();
    if (names.empty()) {
        return;
    }
    int index = 0;
    auto found = std::find(names.begin(), names.end(), anchor_edit_.animation_id);
    if (found != names.end()) {
        index = static_cast<int>(std::distance(names.begin(), found));
    }
    const int next_index = devmode::room_anchor_mode::wrap_index(index + delta, static_cast<int>(names.size()));
    const std::string& next_animation = names[static_cast<std::size_t>(next_index)];

    int next_frame_index = anchor_edit_.frame_index;
    if (anchor_edit_.target_asset && anchor_edit_.target_asset->info) {
        auto anim_it = anchor_edit_.target_asset->info->animations.find(next_animation);
        if (anim_it != anchor_edit_.target_asset->info->animations.end() && !anim_it->second.frames.empty()) {
            next_frame_index =
                devmode::room_anchor_mode::wrap_index(next_frame_index, static_cast<int>(anim_it->second.frames.size()));
        } else {
            next_frame_index = 0;
        }
    }

    if (apply_anchor_animation_and_frame(next_animation, next_frame_index)) {
        refresh_anchor_mode_handles();
    }
}

void RoomEditor::navigate_anchor_frame(int delta) {
    if (!anchor_mode_active() || delta == 0 || !anchor_edit_.target_asset || !anchor_edit_.target_asset->info) {
        return;
    }
    auto anim_it = anchor_edit_.target_asset->info->animations.find(anchor_edit_.animation_id);
    if (anim_it == anchor_edit_.target_asset->info->animations.end() || anim_it->second.frames.empty()) {
        return;
    }
    const int next_frame =
        devmode::room_anchor_mode::wrap_index(anchor_edit_.frame_index + delta, static_cast<int>(anim_it->second.frames.size()));
    if (apply_anchor_animation_and_frame(anchor_edit_.animation_id, next_frame)) {
        refresh_anchor_mode_handles();
    }
}

bool RoomEditor::enter_anchor_edit_mode() {
    if (anchor_mode_active()) {
        return true;
    }

    Asset* target = selected_anchor_mode_asset();
    if (!target || !target->info) {
        return false;
    }

    std::vector<std::string> animation_names;
    for (const auto& [name, animation] : target->info->animations) {
        if (!animation.frames.empty()) {
            animation_names.push_back(name);
        }
    }
    if (animation_names.empty()) {
        return false;
    }

    anchor_edit_ = AnchorEditState{};
    anchor_edit_.target_asset = target;
    anchor_edit_.had_static_frame_before = true;
    anchor_edit_.static_frame_before = target->static_frame;

    editor_mode_ = EditorMode::AnchorEdit;
    anchor_edit_.animation_id = target->current_animation;
    bool animation_valid = false;
    auto selected_anim_it = target->info->animations.find(anchor_edit_.animation_id);
    if (selected_anim_it != target->info->animations.end() && !selected_anim_it->second.frames.empty()) {
        animation_valid = true;
    }
    if (anchor_edit_.animation_id.empty() ||
        !animation_valid) {
        anchor_edit_.animation_id = animation_names.front();
    }
    anchor_edit_.frame_index = 0;

    if (!apply_anchor_animation_and_frame(anchor_edit_.animation_id, resolve_anchor_mode_frame_index())) {
        editor_mode_ = EditorMode::Normal;
        anchor_edit_ = AnchorEditState{};
        return false;
    }

    refresh_anchor_mode_handles();
    ensure_anchor_selection_valid();
    sync_anchor_tools_panel();
    update_anchor_editor_layout();
    return true;
}

void RoomEditor::exit_anchor_edit_mode(bool flush_immediately) {
    if (!anchor_mode_active()) {
        return;
    }

    if (flush_immediately) {
        persist_anchor_current_frame(devmode::core::DevSaveCoordinator::Priority::Immediate, true);
    }

    if (anchor_edit_.target_asset && anchor_edit_.had_static_frame_before) {
        anchor_edit_.target_asset->static_frame = anchor_edit_.static_frame_before;
    }

    editor_mode_ = EditorMode::Normal;
    anchor_edit_ = AnchorEditState{};
    sync_anchor_tools_panel();
    update_anchor_editor_layout();
}

void RoomEditor::validate_anchor_edit_target() {
    if (!anchor_mode_active()) {
        return;
    }

    Asset* target = anchor_edit_.target_asset;
    if (!enabled_ || !target || !target->info || target->dead) {
        exit_anchor_edit_mode(true);
        return;
    }
    if (selected_assets_.empty() || selected_assets_.front() != target) {
        // Anchor mode is bound to a concrete target asset. UI clicks (including the toggle button)
        // can transiently disturb room selection, so recover selection instead of forcing mode exit.
        selected_assets_.clear();
        selected_assets_.push_back(target);
        hovered_asset_ = target;
        sync_spawn_group_panel_with_selection();
        mark_highlight_dirty();
    }

    auto anim_it = target->info->animations.find(anchor_edit_.animation_id);
    if (anim_it == target->info->animations.end() || anim_it->second.frames.empty()) {
        std::vector<std::string> names;
        for (const auto& [name, animation] : target->info->animations) {
            if (!animation.frames.empty()) {
                names.push_back(name);
            }
        }
        if (names.empty()) {
            exit_anchor_edit_mode(true);
            return;
        }
        apply_anchor_animation_and_frame(names.front(), 0);
    }
}

bool RoomEditor::is_anchor_ui_blocking_point(int x, int y) const {
    if (!enabled_) {
        return false;
    }

    SDL_Point point{x, y};
    if (should_show_anchor_mode_toggle() && anchor_mode_toggle_button_ &&
        SDL_PointInRect(&point, &anchor_mode_toggle_button_->rect())) {
        return true;
    }

    if (anchor_mode_active()) {
        if (anchor_anim_prev_button_ && SDL_PointInRect(&point, &anchor_anim_prev_button_->rect())) return true;
        if (anchor_anim_next_button_ && SDL_PointInRect(&point, &anchor_anim_next_button_->rect())) return true;
        if (anchor_frame_prev_button_ && SDL_PointInRect(&point, &anchor_frame_prev_button_->rect())) return true;
        if (anchor_frame_next_button_ && SDL_PointInRect(&point, &anchor_frame_next_button_->rect())) return true;
        if (anchor_tools_panel_ && anchor_tools_panel_->is_visible() && anchor_tools_panel_->is_point_inside(x, y)) {
            return true;
        }
    }

    return false;
}

bool RoomEditor::is_ui_blocking_input(int mx, int my) const {
    if (info_ui_ && info_ui_->is_visible()) {
        if (info_ui_->is_point_inside(mx, my)) {
            return true;
        }
    }
    if (shared_footer_bar_ && shared_footer_bar_->visible()) {
        if (shared_footer_bar_->contains(mx, my)) {
            return true;
        }
    }
    if (room_cfg_ui_ && room_cfg_ui_->visible() && room_cfg_ui_->is_point_inside(mx, my)) {
        return true;
    }
    if (spawn_group_panel_ && spawn_group_panel_->is_visible() && spawn_group_panel_->is_point_inside(mx, my)) {
        return true;
    }
    if (library_ui_ && library_ui_->is_visible() && library_ui_->is_input_blocking_at(mx, my)) {
        return true;
    }
    if (is_anchor_ui_blocking_point(mx, my)) {
        return true;
    }
    auto floating = DockManager::instance().open_panels();
    for (DockableCollapsible* panel : floating) {
        if (!panel) continue;
        if (!panel->is_visible()) continue;
        if (spawn_group_panel_ && panel == spawn_group_panel_.get()) continue;
        if (panel->is_point_inside(mx, my)) {
            return true;
        }
    }

    return false;
}

bool RoomEditor::should_enable_mouse_controls() const {
    if (!enabled_) {
        return false;
    }

    if (active_modal_ != ActiveModal::None && active_modal_ != ActiveModal::AssetInfo) {
        return false;
    }

    return true;
}

void RoomEditor::handle_shortcuts(const Input& input) {
    const bool ctrl = input.isScancodeDown(SDL_SCANCODE_LCTRL) || input.isScancodeDown(SDL_SCANCODE_RCTRL);
    if (!ctrl) return;

    if (input.wasScancodePressed(SDL_SCANCODE_C)) {
        copy_selected_spawn_group();
    }
    if (input.wasScancodePressed(SDL_SCANCODE_V)) {
        paste_spawn_group_from_clipboard();
    }

    if (input.wasScancodePressed(SDL_SCANCODE_L)) {
        if (library_ui_ && library_ui_->is_locked()) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "[RoomEditor] Asset library is locked; shortcut ignored.");
        } else {
            const bool was_open = is_asset_library_open();
            toggle_asset_library();
            const bool now_open = is_asset_library_open();
            if (was_open != now_open) {
                show_notice(now_open ? "Opened asset library" : "Closed asset library");
            }
        }
    }

    const bool save_room_camera_shortcut =
        input.wasScancodePressed(SDL_SCANCODE_A) ||
        input.wasScancodePressed(SDL_SCANCODE_R);
    if (save_room_camera_shortcut) {
        if (!current_room_ || !assets_) {
            return;
        }
        const auto& params = assets_->getView().camera_state().params;
        if (!std::isfinite(params.height_px) || !std::isfinite(params.tilt_deg) ||
            !std::isfinite(params.zoom_percent)) {
            return;
        }

        const int height_px = std::max(kSavedCameraHeightMinPx, static_cast<int>(std::lround(params.height_px)));
        const float tilt_deg = std::clamp(static_cast<float>(params.tilt_deg), kSavedCameraTiltMinDeg, kSavedCameraTiltMaxDeg);
        const int zoom_percent = std::clamp(static_cast<int>(std::lround(params.zoom_percent)),
                                            kSavedCameraZoomMinPercent,
                                            kSavedCameraZoomMaxPercent);
        SDL_Point room_center{0, 0};
        if (current_room_->room_area) {
            room_center = current_room_->room_area->get_center();
        }
        const SDL_Point camera_center = assets_->getView().get_screen_center();
        const int camera_center_dx = camera_center.x - room_center.x;
        const int camera_center_dz = camera_center.y - room_center.y;

        current_room_->camera_height_px = height_px;
        current_room_->camera_tilt_deg = tilt_deg;
        current_room_->camera_zoom_percent = zoom_percent;
        current_room_->camera_center_dx = camera_center_dx;
        current_room_->camera_center_dz = camera_center_dz;

        auto& room_data = current_room_->assets_data();
        room_data["camera_height_px"] = height_px;
        room_data["camera_tilt_deg"] = tilt_deg;
        room_data["camera_zoom_percent"] = zoom_percent;
        room_data["camera_center_dx"] = camera_center_dx;
        room_data["camera_center_dz"] = camera_center_dz;

        enqueue_current_room_save(devmode::core::DevSaveCoordinator::Priority::Immediate);

        if (room_cfg_ui_) {
            room_cfg_ui_->reload_camera_state_from_room();
        }

        assets_->mark_camera_dirty();
        assets_->show_dev_notice("Saved camera defaults for room: " + current_room_->room_name, 2000);
        return;
    }
}

void RoomEditor::ensure_room_configurator() {
    if (!room_cfg_ui_) {
        room_cfg_ui_ = std::make_unique<RoomConfigurator>();
    }
    if (room_cfg_ui_) {
        room_cfg_ui_->set_manifest_store(manifest_store_);
        room_cfg_ui_->set_room_save_callback([this](bool immediate) {
            return enqueue_current_room_save(immediate
                ? devmode::core::DevSaveCoordinator::Priority::Immediate
                : devmode::core::DevSaveCoordinator::Priority::Debounced);
        });
        room_cfg_ui_->set_header_visibility_controller([this](bool visible) {
            room_config_panel_visible_ = visible;
            if (header_visibility_callback_) {
                header_visibility_callback_(room_config_panel_visible_ || asset_info_panel_visible_);
            }
        });
        room_cfg_ui_->set_on_camera_changed([this](Room* changed_room) {
            if (assets_ && (!changed_room || changed_room == current_room_)) {
                assets_->getView().set_manual_height_override(false);
                assets_->getView().set_manual_zoom_override(false);
                assets_->mark_camera_dirty();
                mark_spatial_index_dirty();
            }
        });
        room_cfg_ui_->set_bounds(room_config_bounds_);

        room_cfg_ui_->set_work_area(DockManager::instance().usableRect());
        room_cfg_ui_->set_blocks_editor_interactions(false);
        room_cfg_ui_->set_on_close([this]() {
            room_config_dock_open_ = false;
            set_camera_settings_lock(false);
            update_spawn_group_config_anchor();
        });
        room_cfg_ui_->set_spawn_group_callbacks(
            [this](const std::string& spawn_id) {
                if (active_modal_ == ActiveModal::AssetInfo) {
                    pulse_active_modal_header();
                    return;
                }

                set_room_config_visible(true);
                if (room_cfg_ui_) {
                    room_cfg_ui_->focus_spawn_group(spawn_id);
                }

                if (spawn_group_panel_) {
                    spawn_group_panel_->close();
                    spawn_group_panel_->set_visible(false);
                }
            },
            [this](const std::string& spawn_id) {
                delete_spawn_group_internal(spawn_id);
            },
            [this](const std::string& spawn_id, size_t index) {
                reorder_spawn_group_internal(spawn_id, index);
            },
            [this]() {
                if (active_modal_ == ActiveModal::AssetInfo) {
                    pulse_active_modal_header();
                    return;
                }
                add_spawn_group_internal();
            },
            [this](const std::string& spawn_id) {
                if (spawn_id.empty()) {
                    clear_active_spawn_group_target();
                } else {
                    active_spawn_group_id_ = spawn_id;
                }
                refresh_spawn_group_config_ui();
                if (spawn_id.empty()) {
                    return;
                }
                if (nlohmann::json* entry = find_spawn_entry(spawn_id)) {
                    respawn_spawn_group(*entry);
                }
            },
            [this](const std::string& spawn_id, SDL_Point point) {
                open_spawn_group_floating_panel(spawn_id, point);
            });
        room_cfg_ui_->set_on_room_renamed([this](const std::string& old_name, const std::string& desired) {
            return this->rename_active_room(old_name, desired);
        });
    }
}

std::string RoomEditor::rename_active_room(const std::string& old_name, const std::string& desired_name) {
    std::string trimmed = trim_copy_room_editor(desired_name);
    std::string base = sanitize_room_key_local(trimmed.empty() ? desired_name : trimmed);
    if (!assets_ || !current_room_) {
        return base.empty() ? old_name : base;
    }

    auto& map_info = assets_->map_info_json();
    nlohmann::json& rooms_data = map_info["rooms_data"];
    if (!rooms_data.is_object()) {
        rooms_data = nlohmann::json::object();
    }

    std::string candidate = base.empty() ? current_room_->room_name : base;
    if (candidate.empty()) {
        candidate = old_name;
    }

    if (candidate == old_name) {
        return old_name;
    }

    if (rooms_data.contains(candidate)) {
        return old_name;
    }

    std::string final_key = candidate;

    if (final_key != current_room_->room_name) {
        current_room_->rename(final_key, map_info);
        map_layers::rename_room_references_in_layers(map_info, old_name, final_key);
        if (mark_map_dirty_callback_) {
            mark_map_dirty_callback_(devmode::core::DevSaveCoordinator::Priority::Debounced);
        }
        rebuild_room_spawn_id_cache();
        invalidate_label_cache(current_room_);
    }

    return final_key;
}

void RoomEditor::ensure_spawn_group_config_ui() {
    if (spawn_group_panel_) {
        return;
    }

    spawn_group_panel_ = std::make_unique<SpawnGroupConfig>();
    if (!spawn_group_panel_) {
        return;
    }

    spawn_group_panel_->set_manifest_store(manifest_store_);
    spawn_group_panel_->set_assets(assets_);
    spawn_group_panel_->set_show_header(true);
    spawn_group_panel_->set_close_button_enabled(true);
    spawn_group_panel_->set_scroll_enabled(true);
    spawn_group_panel_->set_visible(false);
    spawn_group_panel_->set_expanded(true);
    spawn_group_panel_->set_work_area(SDL_Rect{0, 0, screen_w_, screen_h_});
    spawn_group_panel_->set_screen_dimensions(screen_w_, screen_h_);
    spawn_group_panel_->set_on_close([this]() {
        if (suppress_spawn_group_close_clear_) {
            suppress_spawn_group_close_clear_ = false;
            return;
        }
        clear_active_spawn_group_target();
    });

    SpawnGroupConfig::Callbacks callbacks{};
    callbacks.on_add = [this]() { add_spawn_group_internal(); };
    callbacks.on_delete = [this](const std::string& id) { delete_spawn_group_internal(id); };
    callbacks.on_reorder = [this](const std::string& id, size_t index) {
        reorder_spawn_group_internal(id, index);
};
    callbacks.on_regenerate = [this](const std::string& id) {
        if (id.empty()) {
            return;
        }
        if (nlohmann::json* entry = find_spawn_entry(id)) {
            respawn_spawn_group(*entry);
        }
};
    callbacks.on_open_floating = [this](const std::string& id, SDL_Point point) {
        open_spawn_group_floating_panel(id, point);
};
    spawn_group_panel_->set_callbacks(std::move(callbacks));
    spawn_group_panel_->set_on_layout_changed([this]() { update_spawn_group_config_anchor(); });
}

void RoomEditor::update_room_config_bounds() {
    const int side_margin = 0;
    const int available_width = std::max(0, screen_w_ - 2 * side_margin);
    const int max_width = std::max(320, available_width);
    const int desired_width = std::max(360, screen_w_ / 3);
    const int width = std::min(max_width, desired_width);

    SDL_Rect usable = DockManager::instance().usableRect();
    const int height = std::max(1, usable.h > 0 ? usable.h : screen_h_);
    const int max_x = std::max(0, screen_w_ - width);
    const int desired_x = screen_w_ - width;
    const int x = std::clamp(desired_x, 0, max_x);
    const int y = usable.h > 0 ? usable.y : 0;
    room_config_bounds_ = SDL_Rect{x, y, width, height};
    if (room_cfg_ui_ && room_config_dock_open_) {
        room_cfg_ui_->set_bounds(room_config_bounds_);
    }
    refresh_room_config_visibility();
}

void RoomEditor::configure_shared_panel() {
    if (!shared_footer_bar_) {
        return;
    }
    shared_footer_bar_->set_bounds(screen_w_, screen_h_);
}

void RoomEditor::refresh_room_config_visibility() {
    ensure_room_configurator();
    if (!room_cfg_ui_) {
        return;
    }
    if (active_modal_ == ActiveModal::AssetInfo) {
        room_cfg_ui_->close();
        update_spawn_group_config_anchor();
        return;
    }
    if (room_config_dock_open_) {
        room_cfg_ui_->set_bounds(room_config_bounds_);
        room_cfg_ui_->open(current_room_);
    } else {
        room_cfg_ui_->close();
    }
    update_spawn_group_config_anchor();
}

void RoomEditor::handle_delete_shortcut(const Input& input) {
    if (input.wasScancodePressed(SDL_SCANCODE_ESCAPE) && !selected_assets_.empty()) {
        clear_selection();
        return;
    }

    if (!input.wasScancodePressed(SDL_SCANCODE_DELETE)) {
        return;
    }

    delete_selected_asset_or_group();
}

void RoomEditor::begin_drag_session(const SDL_Point& world_mouse, bool ctrl_modifier) {

    if (!selected_assets_.empty()) {
        Asset* primary = selected_assets_.front();
        if (primary && !primary->spawn_id.empty() && spawn_group_locked(primary->spawn_id)) {
            return;
        }
    }

    if (room_config_dock_open_) {

        suppress_room_config_selection_clear_ = true;
        set_room_config_visible(false);
        suppress_room_config_selection_clear_ = false;
    }

    drag_mode_ = DragMode::None;
    drag_states_.clear();
    drag_spawn_id_.clear();
    drag_perimeter_base_radius_ = 0.0;
    drag_moved_ = false;
    drag_room_center_ = get_room_center();
    drag_last_world_ = world_mouse;
    drag_anchor_asset_ = nullptr;
    drag_edge_area_ = nullptr;
    drag_edge_center_ = drag_room_center_;
    drag_edge_inset_percent_ = 100.0;

    if (selected_assets_.empty()) return;
    Asset* primary = selected_assets_.front();
    if (!primary) return;

    drag_anchor_asset_ = primary;
    drag_spawn_id_ = primary->spawn_id;

    MapGridSettings map_settings = current_room_ ? current_room_->map_grid_settings() : MapGridSettings::defaults();
    map_settings.clamp();

    SpawnEntryResolution resolved_entry = drag_spawn_id_.empty() ? SpawnEntryResolution{} : locate_spawn_entry(drag_spawn_id_);
    nlohmann::json* spawn_entry = resolved_entry.entry;

    int desired_resolution = current_grid_resolution();
    if (desired_resolution <= 0) {
        desired_resolution = map_settings.grid_resolution;
    }
    drag_resolution_ = vibble::grid::clamp_resolution(std::max(0, desired_resolution));

    const std::string& method = primary->spawn_method;
    if (method == "Exact" || method == "Exact Position") {
        drag_mode_ = DragMode::Exact;
    } else if (method == "Percent") {
        drag_mode_ = DragMode::Percent;
    } else if (method == "Perimeter") {

        drag_mode_ = ctrl_modifier ? DragMode::PerimeterCenter : DragMode::Perimeter;
    } else if (method == "Edge") {
        drag_mode_ = DragMode::Edge;
    } else if (method == "Random") {

        drag_mode_ = DragMode::Free;
    } else {
        drag_mode_ = DragMode::Free;
    }

    bool resolve_geometry = (method == "Exact" || method == "Exact Position" || method == "Perimeter");

    auto [room_w, room_h] = get_room_dimensions();
    drag_perimeter_curr_w_ = room_w;
    drag_perimeter_curr_h_ = room_h;
    drag_perimeter_orig_w_ = std::max(1, room_w);
    drag_perimeter_orig_h_ = std::max(1, room_h);
    drag_perimeter_center_offset_world_ = SDL_Point{0, 0};
    drag_perimeter_circle_center_ = drag_room_center_;

    if (spawn_entry) {
        resolve_geometry = spawn_entry->value( "resolve_geometry_to_room_size", resolve_geometry);
        int orig_w = std::max(1, drag_perimeter_curr_w_);
        int orig_h = std::max(1, drag_perimeter_curr_h_);
        if (resolve_geometry) {
            orig_w = std::max(1, spawn_entry->value("origional_width", orig_w));
            orig_h = std::max(1, spawn_entry->value("origional_height", orig_h));
        }
        drag_perimeter_orig_w_ = orig_w;
        drag_perimeter_orig_h_ = orig_h;
        const int stored_dx = spawn_entry->value("dx", 0);
        const int stored_dz = spawn_entry->value("dz", 0);
        RelativeRoomPosition relative(SDL_Point{stored_dx, stored_dz}, orig_w, orig_h);
        drag_perimeter_center_offset_world_ = relative.scaled_offset(room_w, room_h);
        drag_perimeter_circle_center_.x = drag_room_center_.x + drag_perimeter_center_offset_world_.x;
        drag_perimeter_circle_center_.y = drag_room_center_.y + drag_perimeter_center_offset_world_.y;
        if ((*spawn_entry).contains("radius") && (*spawn_entry)["radius"].is_number_integer()) {
            drag_perimeter_base_radius_ = std::max(0, (*spawn_entry)["radius"].get<int>());
            if (resolve_geometry && drag_perimeter_base_radius_ > 0.0) {
                const double width_ratio = static_cast<double>(std::max(1, room_w)) / static_cast<double>(orig_w);
                const double height_ratio = static_cast<double>(std::max(1, room_h)) / static_cast<double>(orig_h);
                const double ratio = (width_ratio + height_ratio) * 0.5;
                drag_perimeter_base_radius_ = std::max(0.0, drag_perimeter_base_radius_ * ratio);
            }
        }
    }

    if (drag_mode_ == DragMode::Edge) {
        if (spawn_entry) {
            drag_edge_area_ = find_edge_area_for_entry(*spawn_entry);
            drag_edge_inset_percent_ = static_cast<double>(std::clamp(spawn_entry->value("edge_inset_percent", 100), 0, 200));
        } else {
            drag_edge_area_ = current_room_ ? current_room_->room_area.get() : nullptr;
            drag_edge_inset_percent_ = 100.0;
        }
        if (drag_edge_area_) {
            SDL_Point center = drag_edge_area_->get_center();
            drag_edge_center_ = center;
        } else {
            drag_edge_center_ = drag_room_center_;
        }
    }

    if (drag_mode_ == DragMode::Perimeter || drag_mode_ == DragMode::PerimeterCenter) {
        if (drag_perimeter_base_radius_ <= 0.0) {
            double dx = static_cast<double>(primary->world_xz_point().x - drag_perimeter_circle_center_.x);
            double dy = static_cast<double>(primary->world_xz_point().y - drag_perimeter_circle_center_.y);
            drag_perimeter_base_radius_ = std::hypot(dx, dy);
        }
        if (!std::isfinite(drag_perimeter_base_radius_) || drag_perimeter_base_radius_ <= 0.0) {
            drag_perimeter_base_radius_ = 0.0;
        }
    }

    drag_states_.reserve(selected_assets_.size());
    for (Asset* asset : selected_assets_) {
        if (!asset) continue;
        DraggedAssetState state;
        state.asset = asset;
        state.start_pos = asset->world_xz_point();
        state.last_synced_pos = asset->world_xz_point();
        state.active = true;
        if (drag_mode_ == DragMode::Perimeter) {
            double dx = static_cast<double>(asset->world_x() - drag_perimeter_circle_center_.x);
            double dz = static_cast<double>(asset->world_z() - drag_perimeter_circle_center_.y);
            double len = std::hypot(dx, dz);
            if (len > 1e-6) {
                state.direction.x = static_cast<float>(dx / len);
                state.direction.y = static_cast<float>(dz / len);
            } else {
                state.direction.x = 0.0f;
                state.direction.y = -1.0f;
            }
        } else if (drag_mode_ == DragMode::Edge) {
            double dx = static_cast<double>(asset->world_x() - drag_edge_center_.x);
            double dz = static_cast<double>(asset->world_z() - drag_edge_center_.y);
            double len = std::hypot(dx, dz);
            if (len > 1e-6) {
                state.direction.x = static_cast<float>(dx / len);
                state.direction.y = static_cast<float>(dz / len);
            } else {
                state.direction.x = 0.0f;
                state.direction.y = -1.0f;
                len = 1.0;
            }

            if (drag_edge_area_) {
                state.edge_length = edge_length_along_direction(*drag_edge_area_, drag_edge_center_, state.direction);
            }
            if (state.edge_length <= 1e-6) {
                state.edge_length = len;
            }
        }
        drag_states_.push_back(state);
    }

}

void RoomEditor::update_drag_session(const SDL_Point& world_mouse) {
    if (drag_states_.empty()) {
        drag_last_world_ = world_mouse;
        return;
    }

    auto invalidate_after_move = [this]() {
        sync_dragged_assets_immediately();
        for (auto& st : drag_states_) {
            if (st.asset) {
                auto it = asset_bounds_cache_.find(st.asset);
                if (it != asset_bounds_cache_.end()) {
                    asset_bounds_cache_.erase(it);
                }
            }
        }
        mark_spatial_index_dirty();
        mark_highlight_dirty();
        refresh_spatial_entries_for_dragged_assets();
};

    if (drag_mode_ == DragMode::Perimeter) {
        apply_perimeter_drag(world_mouse);
        drag_last_world_ = world_mouse;
        drag_moved_ = true;
        invalidate_after_move();
        ensure_spatial_index(assets_->getView());
        return;
    }

    if (drag_mode_ == DragMode::Edge) {
        apply_edge_drag(world_mouse);
        drag_last_world_ = world_mouse;
        drag_moved_ = true;
        invalidate_after_move();
        ensure_spatial_index(assets_->getView());
        return;
    }

    SDL_Point delta{world_mouse.x - drag_last_world_.x, world_mouse.y - drag_last_world_.y};
    const bool anchor_should_follow_pointer =
        (drag_mode_ == DragMode::Exact || drag_mode_ == DragMode::Percent);
    if (anchor_should_follow_pointer) {
        Asset* anchor_asset = drag_anchor_asset_;
        if (!anchor_asset && !drag_states_.empty()) {
            anchor_asset = drag_states_.front().asset;
        }
        if (anchor_asset) {

            vibble::grid::Grid& grid_service = vibble::grid::global_grid();
            SDL_Point snapped_pointer = (snap_to_grid_enabled_ && drag_resolution_ > 0)
                ? grid_service.snap_to_vertex(world_mouse, drag_resolution_)
                : world_mouse;
            delta.x = snapped_pointer.x - anchor_asset->world_x();
            delta.y = snapped_pointer.y - anchor_asset->world_z();
        }
    }

    if (delta.x == 0 && delta.y == 0) {
        drag_last_world_ = world_mouse;
        return;
    }

    for (auto& state : drag_states_) {
        if (!state.asset) continue;
        state.asset->move_to_world_position(state.asset->world_x() + delta.x, state.asset->world_y(), state.asset->world_z() + delta.y);
    }

    if (drag_mode_ == DragMode::PerimeterCenter) {
        drag_perimeter_circle_center_.x += delta.x;
        drag_perimeter_circle_center_.y += delta.y;
        drag_perimeter_center_offset_world_.x += delta.x;
        drag_perimeter_center_offset_world_.y += delta.y;
    }

    snap_dragged_assets_to_grid();

    drag_last_world_ = world_mouse;
    drag_moved_ = true;

    invalidate_after_move();
    ensure_spatial_index(assets_->getView());

    update_spawn_json_during_drag();
}

void RoomEditor::apply_perimeter_drag(const SDL_Point& world_mouse) {
    if (drag_states_.empty()) return;

    const DraggedAssetState* ref = nullptr;
    for (const auto& state : drag_states_) {
        if (state.asset == drag_anchor_asset_) {
            ref = &state;
            break;
        }
    }
    if (!ref) ref = &drag_states_.front();

    auto compute_start_distance = [this](const DraggedAssetState& state) {
        double dx = static_cast<double>(state.start_pos.x - drag_perimeter_circle_center_.x);
        double dy = static_cast<double>(state.start_pos.y - drag_perimeter_circle_center_.y);
        return std::hypot(dx, dy);
};

    double reference_length = compute_start_distance(*ref);
    if (reference_length <= 1e-6) {
        double dx = static_cast<double>(ref->asset->world_x() - drag_perimeter_circle_center_.x);
        double dz = static_cast<double>(ref->asset->world_z() - drag_perimeter_circle_center_.y);
        reference_length = std::hypot(dx, dz);
    }
    if (reference_length <= 1e-6) reference_length = 1.0;

    double base_radius = drag_perimeter_base_radius_;
    if (base_radius <= 1e-6) base_radius = reference_length;

    double new_radius = std::hypot(static_cast<double>(world_mouse.x - drag_perimeter_circle_center_.x), static_cast<double>(world_mouse.y - drag_perimeter_circle_center_.y));
    if (!std::isfinite(new_radius)) {
        new_radius = 0.0;
    }

    double ratio = base_radius > 1e-6 ? new_radius / base_radius : 0.0;
    if (!std::isfinite(ratio)) ratio = 0.0;
    if (ratio < 0.0) ratio = 0.0;

    bool changed = false;
    for (auto& state : drag_states_) {
        if (!state.asset) continue;
        double base = compute_start_distance(state);
        SDL_FPoint state_dir = state.direction;
        if (base <= 0.0 || (state_dir.x == 0.0f && state_dir.y == 0.0f)) {
            double dx = static_cast<double>(state.asset->world_x() - drag_perimeter_circle_center_.x);
            double dz = static_cast<double>(state.asset->world_z() - drag_perimeter_circle_center_.y);
            if (base <= 0.0) base = std::hypot(dx, dz);
            if (dx != 0.0 || dz != 0.0) {
                state_dir.x = static_cast<float>(dx / std::hypot(dx, dz));
                state_dir.y = static_cast<float>(dz / std::hypot(dx, dz));
            } else {
                state_dir.x = 0.0f;
                state_dir.y = -1.0f;
            }
        }
        double desired = base * ratio;
        int new_x = drag_perimeter_circle_center_.x + static_cast<int>(std::lround(static_cast<double>(state_dir.x) * desired));
        int new_z = drag_perimeter_circle_center_.y + static_cast<int>(std::lround(static_cast<double>(state_dir.y) * desired));
        if (state.asset->world_x() != new_x || state.asset->world_z() != new_z) {
            state.asset->move_to_world_position(new_x, state.asset->world_y(), new_z);
            changed = true;
        }
    }
    if (changed) {
        drag_moved_ = true;
    }
    const double previous_percent = drag_edge_inset_percent_;
    if (std::fabs(previous_percent - drag_edge_inset_percent_) > 1e-6) {
        drag_moved_ = true;
    }

    const bool snapped = snap_dragged_assets_to_grid();
    if (changed || snapped) {
        refresh_spatial_entries_for_dragged_assets();
    }

    update_spawn_json_during_drag();
}

void RoomEditor::apply_edge_drag(const SDL_Point& world_mouse) {
    const SDL_Point center = drag_edge_center_;

    const DraggedAssetState* ref = nullptr;
    if (!drag_states_.empty()) {
        for (const auto& state : drag_states_) {
            if (state.asset == drag_anchor_asset_) {
                ref = &state;
                break;
            }
        }
        if (!ref) {
            ref = &drag_states_.front();
        }
    }

    SDL_FPoint reference_direction{0.0f, 0.0f};
    double reference_length = 0.0;

    if (ref) {
        reference_direction = ref->direction;
        double dir_len = std::hypot(static_cast<double>(reference_direction.x), static_cast<double>(reference_direction.y));
        if (dir_len > 1e-6) {
            reference_direction.x = static_cast<float>(reference_direction.x / dir_len);
            reference_direction.y = static_cast<float>(reference_direction.y / dir_len);
        } else {
            reference_direction.x = 0.0f;
            reference_direction.y = 0.0f;
        }

        reference_length = ref->edge_length;
        if (reference_length <= 1e-6 && ref->asset) {
            double dx = static_cast<double>(ref->asset->world_x() - center.x);
            double dz = static_cast<double>(ref->asset->world_z() - center.y);
            reference_length = std::hypot(dx, dz);
        }
    }

    double dx_mouse = static_cast<double>(world_mouse.x - center.x);
    double dz_mouse = static_cast<double>(world_mouse.y - center.y);
    double mouse_len = std::hypot(dx_mouse, dz_mouse);

    if ((reference_direction.x == 0.0f && reference_direction.y == 0.0f) && mouse_len > 1e-6) {
        reference_direction.x = static_cast<float>(dx_mouse / mouse_len);
        reference_direction.y = static_cast<float>(dz_mouse / mouse_len);
    }

    if (reference_length <= 1e-6 && drag_edge_area_ &&
        !(reference_direction.x == 0.0f && reference_direction.y == 0.0f)) {
        reference_length = edge_length_along_direction(*drag_edge_area_, center, reference_direction);
    }

    if (reference_length <= 1e-6) {
        reference_length = mouse_len;
    }
    if (!std::isfinite(reference_length) || reference_length <= 1e-6) {
        reference_length = 1.0;
    }

    double projected = dx_mouse * static_cast<double>(reference_direction.x) + dz_mouse * static_cast<double>(reference_direction.y);
    double ratio = projected / reference_length;
    if (!std::isfinite(ratio)) {
        ratio = 0.0;
    }
    ratio = std::clamp(ratio, 0.0, 2.0);

    int snapped_percent = std::clamp(static_cast<int>(std::lround(ratio * 100.0)), 0, 200);
    double snapped_ratio = static_cast<double>(snapped_percent) / 100.0;

    bool assets_changed = false;
    for (auto& state : drag_states_) {
        if (!state.asset) continue;
        double base_length = state.edge_length;
        if (base_length <= 1e-6) {
            double dx = static_cast<double>(state.asset->world_x() - center.x);
            double dz = static_cast<double>(state.asset->world_z() - center.y);
            base_length = std::hypot(dx, dz);
        }
        SDL_FPoint dir = state.direction;
        double dir_len = std::hypot(static_cast<double>(dir.x), static_cast<double>(dir.y));
        if (dir_len > 1e-6) {
            dir.x = static_cast<float>(dir.x / dir_len);
            dir.y = static_cast<float>(dir.y / dir_len);
        } else if (base_length > 1e-6) {
            double dx = static_cast<double>(state.asset->world_x() - center.x);
            double dz = static_cast<double>(state.asset->world_z() - center.y);
            if (dx != 0.0 || dz != 0.0) {
                dir.x = static_cast<float>(dx / std::hypot(dx, dz));
                dir.y = static_cast<float>(dz / std::hypot(dx, dz));
            }
        }
        state.direction = dir;
        double desired = base_length * snapped_ratio;
        int new_x = center.x + static_cast<int>(std::lround(static_cast<double>(dir.x) * desired));
        int new_z = center.y + static_cast<int>(std::lround(static_cast<double>(dir.y) * desired));
        if (state.asset->world_x() != new_x || state.asset->world_z() != new_z) {
            state.asset->move_to_world_position(new_x, state.asset->world_y(), new_z);
            assets_changed = true;
        }
    }

    double previous_percent = drag_edge_inset_percent_;
    drag_edge_inset_percent_ = static_cast<double>(snapped_percent);

    if (assets_changed) {
        drag_moved_ = true;
    }
    if (std::fabs(previous_percent - drag_edge_inset_percent_) > 1e-6) {
        drag_moved_ = true;
    }

    const bool snapped = snap_dragged_assets_to_grid();
    if (assets_changed || snapped) {
        refresh_spatial_entries_for_dragged_assets();
    }

    update_spawn_json_during_drag();
}

void RoomEditor::update_spawn_json_during_drag() {

    if (drag_spawn_id_.empty() || drag_states_.empty()) {
        return;
    }

    if (!spawn_group_panel_ || !spawn_group_panel_->is_visible()) {
        return;
    }

    SpawnEntryResolution resolved = locate_spawn_entry(drag_spawn_id_);
    nlohmann::json* entry = resolved.entry;
    if (!entry) {
        return;
    }

    Asset* primary = selected_assets_.empty() ? nullptr : selected_assets_.front();
    if (!primary) {
        return;
    }

    SDL_Point center = get_room_center();
    auto [width, height] = get_room_dimensions();

    switch (drag_mode_) {
        case DragMode::Exact:
            update_exact_json(*entry, *primary, center, width, height);
            break;

        case DragMode::Percent:
            update_percent_json(*entry, *primary, center, width, height);
            break;

        case DragMode::Perimeter:
        case DragMode::PerimeterCenter: {
            const int curr_w = std::max(1, drag_perimeter_curr_w_ > 0 ? drag_perimeter_curr_w_ : width);
            const int curr_h = std::max(1, drag_perimeter_curr_h_ > 0 ? drag_perimeter_curr_h_ : height);
            const int orig_w = std::max(1, drag_perimeter_orig_w_ > 0 ? drag_perimeter_orig_w_ : curr_w);
            const int orig_h = std::max(1, drag_perimeter_orig_h_ > 0 ? drag_perimeter_orig_h_ : curr_h);
            SDL_Point stored = RelativeRoomPosition::ToOriginal(drag_perimeter_center_offset_world_, orig_w, orig_h, curr_w, curr_h);
            const double dist = std::hypot(static_cast<double>(primary->world_xz_point().x - drag_perimeter_circle_center_.x), static_cast<double>(primary->world_xz_point().y - drag_perimeter_circle_center_.y));
            const int radius = static_cast<int>(std::lround(dist));
            save_perimeter_json(*entry, stored.x, stored.y, orig_w, orig_h, radius);
            break;
        }

        case DragMode::Edge: {
            int inset = static_cast<int>(std::lround(drag_edge_inset_percent_));
            inset = std::clamp(inset, 0, 200);
            save_edge_json(*entry, inset);
            break;
        }

        default:
            break;
    }

    if (spawn_group_panel_) {
        spawn_group_panel_->rebuild_rows();
    }
}

bool RoomEditor::snap_dragged_assets_to_grid() {
    if (drag_states_.empty()) return false;
    const int resolution = vibble::grid::clamp_resolution(drag_resolution_);
    if (!snap_to_grid_enabled_ || resolution <= 0) {
        return false;
    }
    vibble::grid::Grid& grid_service = vibble::grid::global_grid();
    bool changed = false;

    if (drag_mode_ == DragMode::PerimeterCenter) {
        SDL_Point snapped_center = grid_service.snap_to_vertex(drag_perimeter_circle_center_, resolution);
        if (snapped_center.x != drag_perimeter_circle_center_.x || snapped_center.y != drag_perimeter_circle_center_.y) {
            const int dx = snapped_center.x - drag_perimeter_circle_center_.x;
            const int dz = snapped_center.y - drag_perimeter_circle_center_.y;
            drag_perimeter_circle_center_ = snapped_center;
            drag_perimeter_center_offset_world_.x += dx;
            drag_perimeter_center_offset_world_.y += dz;
            for (auto& state : drag_states_) {
                if (!state.asset) continue;
                state.asset->move_to_world_position(state.asset->world_x() + dx, state.asset->world_y(), state.asset->world_z() + dz);
            }
            changed = true;
        }
    }

    for (auto& state : drag_states_) {
        if (!state.asset) continue;
        SDL_Point current{state.asset->world_x(), state.asset->world_z()};
        SDL_Point snapped = grid_service.snap_to_vertex(current, resolution);
        if (snapped.x != state.asset->world_x() || snapped.y != state.asset->world_z()) {
            state.asset->move_to_world_position(snapped.x, state.asset->world_y(), snapped.y);
            changed = true;
        }
    }

    if (changed) {
        drag_moved_ = true;
        sync_dragged_assets_immediately();
    }
    return changed;
}

void RoomEditor::finalize_drag_session() {

    const bool snap_was_enabled = snap_to_grid_enabled_;
    const int  resolution_used_for_drag = vibble::grid::clamp_resolution(drag_resolution_);

    if (drag_states_.empty()) {
        reset_drag_state();
        return;
    }

    Asset* primary = selected_assets_.empty() ? nullptr : selected_assets_.front();
    if (!primary) {
        reset_drag_state();
        return;
    }

    const bool drag_was_moved = drag_moved_;
    bool json_modified = false;
    SDL_Point center = get_room_center();
    auto [width, height] = get_room_dimensions();

    if (!drag_spawn_id_.empty()) {
        SpawnEntryResolution resolved = locate_spawn_entry(drag_spawn_id_);
        nlohmann::json* entry = resolved.entry;
        if (entry) {
            bool request_respawn = false;
            switch (drag_mode_) {
                case DragMode::Exact:
                    if (drag_moved_) {
                        update_exact_json(*entry, *primary, center, width, height);
                        json_modified = true;
                    }
                    break;
                case DragMode::Percent:
                    if (drag_moved_) {
                        update_percent_json(*entry, *primary, center, width, height);
                        json_modified = true;
                    }
                    break;
                case DragMode::Perimeter:
                    if (drag_moved_) {
                        const int curr_w = std::max(1, drag_perimeter_curr_w_ > 0 ? drag_perimeter_curr_w_ : width);
                        const int curr_h = std::max(1, drag_perimeter_curr_h_ > 0 ? drag_perimeter_curr_h_ : height);
                        const int orig_w = std::max(1, drag_perimeter_orig_w_ > 0 ? drag_perimeter_orig_w_ : curr_w);
                        const int orig_h = std::max(1, drag_perimeter_orig_h_ > 0 ? drag_perimeter_orig_h_ : curr_h);
                        SDL_Point stored = RelativeRoomPosition::ToOriginal(drag_perimeter_center_offset_world_, orig_w, orig_h, curr_w, curr_h);
                        const double dist = std::hypot(static_cast<double>(primary->world_xz_point().x - drag_perimeter_circle_center_.x), static_cast<double>(primary->world_xz_point().y - drag_perimeter_circle_center_.y));
                        const int radius = static_cast<int>(std::lround(dist));
                        save_perimeter_json(*entry, stored.x, stored.y, orig_w, orig_h, radius);
                        json_modified = true;
                    }
                    break;
                case DragMode::PerimeterCenter:
                    if (drag_moved_) {
                        const int curr_w = std::max(1, drag_perimeter_curr_w_ > 0 ? drag_perimeter_curr_w_ : width);
                        const int curr_h = std::max(1, drag_perimeter_curr_h_ > 0 ? drag_perimeter_curr_h_ : height);
                        const int orig_w = std::max(1, drag_perimeter_orig_w_ > 0 ? drag_perimeter_orig_w_ : curr_w);
                        const int orig_h = std::max(1, drag_perimeter_orig_h_ > 0 ? drag_perimeter_orig_h_ : curr_h);
                        SDL_Point stored = RelativeRoomPosition::ToOriginal(drag_perimeter_center_offset_world_, orig_w, orig_h, curr_w, curr_h);
                        const double dist = std::hypot(static_cast<double>(primary->world_xz_point().x - drag_perimeter_circle_center_.x), static_cast<double>(primary->world_xz_point().y - drag_perimeter_circle_center_.y));
                        const int radius = static_cast<int>(std::lround(dist));
                        save_perimeter_json(*entry, stored.x, stored.y, orig_w, orig_h, radius);
                        json_modified = true;
                    }
                    break;
                case DragMode::Edge:
                    if (drag_moved_) {
                        int inset = static_cast<int>(std::lround(drag_edge_inset_percent_));
                        inset = std::clamp(inset, 0, 200);
                        save_edge_json(*entry, inset);
                        json_modified = true;

                        request_respawn = true;
                    }
                    break;
                default:
                    break;
            }

            if (drag_moved_ && snap_was_enabled && resolution_used_for_drag > 0) {
                (*entry)["resolution"] = resolution_used_for_drag;
                for (auto& st : drag_states_) {
                    if (st.asset) {
                        st.asset->grid_resolution = resolution_used_for_drag;
                    }
                }
            }

            if (json_modified) {
                if (resolved.source == SpawnEntryResolution::Source::Room) {
                    save_current_room_assets_json();
                    if (request_respawn) {
                        respawn_spawn_group(*entry);
                    }
                } else if (resolved.source == SpawnEntryResolution::Source::Map) {
                    mark_map_dirty_for_spawn_groups(devmode::core::DevSaveCoordinator::Priority::Debounced);
                    if (assets_) {
                        assets_->notify_spawn_group_config_changed(*entry);
                    }
                }
            }
        }
    }

    if (json_modified) {
        if (!drag_spawn_id_.empty()) {
            active_spawn_group_id_ = drag_spawn_id_;
        }
        refresh_spawn_group_config_ui();
    }

    if (drag_was_moved) {
        suppress_next_left_click_ = true;
    }

    reset_drag_state();
}

void RoomEditor::reset_drag_state() {
    dragging_ = false;
    drag_anchor_asset_ = nullptr;
    drag_mode_ = DragMode::None;
    drag_states_.clear();
    drag_last_world_ = SDL_Point{0, 0};
    drag_room_center_ = SDL_Point{0, 0};
    drag_perimeter_circle_center_ = SDL_Point{0, 0};
    drag_perimeter_base_radius_ = 0.0;
    drag_perimeter_center_offset_world_ = SDL_Point{0, 0};
    drag_perimeter_orig_w_ = 0;
    drag_perimeter_orig_h_ = 0;
    drag_perimeter_curr_w_ = 0;
    drag_resolution_ = 0;
    drag_perimeter_curr_h_ = 0;
    drag_edge_area_ = nullptr;
    drag_edge_center_ = SDL_Point{0, 0};
    drag_edge_inset_percent_ = 100.0;
    drag_moved_ = false;
    drag_spawn_id_.clear();

}

nlohmann::json* RoomEditor::find_spawn_entry(const std::string& spawn_id) {
    if (!current_room_ || spawn_id.empty()) return nullptr;
    auto& root = current_room_->assets_data();
    auto& arr = ensure_spawn_groups_array(root);
    for (auto& entry : arr) {
        if (!entry.is_object()) continue;
        if (entry.contains("spawn_id") && entry["spawn_id"].is_string() &&
            entry["spawn_id"].get<std::string>() == spawn_id) {
            return &entry;
        }
    }
    return nullptr;
}

RoomEditor::SpawnEntryResolution RoomEditor::locate_spawn_entry(const std::string& spawn_id) {
    SpawnEntryResolution result;
    if (spawn_id.empty()) {
        return result;
    }

    if (current_room_) {
        auto& root = current_room_->assets_data();
        nlohmann::json& arr = ensure_spawn_groups_array(root);
        if (nlohmann::json* entry = find_spawn_entry(spawn_id)) {
            result.entry = entry;
            result.owner_array = &arr;
            result.source = SpawnEntryResolution::Source::Room;
            return result;
        }
    }

    if (assets_) {
        nlohmann::json& map_info = assets_->map_info_json();
        nlohmann::json* owner = nullptr;
        if (nlohmann::json* entry = find_spawn_entry_recursive(map_info, spawn_id, &owner)) {
            result.entry = entry;
            result.owner_array = owner;
            result.source = SpawnEntryResolution::Source::Map;
        }
    }

    return result;
}

const Area* RoomEditor::find_edge_area_for_entry(const nlohmann::json& entry) const {
    if (!current_room_) {
        return nullptr;
    }
    const std::string area_name = entry.value("area", std::string{});
    if (!area_name.empty()) {
        if (Area* area = current_room_->find_area(area_name)) {
            return area;
        }
    }
    if (current_room_->room_area) {
        return current_room_->room_area.get();
    }
    return nullptr;
}

SDL_Point RoomEditor::get_room_center() const {
    if (current_room_ && current_room_->room_area) {
        return current_room_->room_area->get_center();
    }
    return SDL_Point{0, 0};
}

std::pair<int, int> RoomEditor::get_room_dimensions() const {
    if (!current_room_ || !current_room_->room_area) return {0, 0};
    auto bounds = current_room_->room_area->get_bounds();
    int width = std::max(0, std::get<2>(bounds) - std::get<0>(bounds));
    int height = std::max(0, std::get<3>(bounds) - std::get<1>(bounds));
    return {width, height};
}

int RoomEditor::current_grid_resolution() const {
    if (shared_footer_bar_) {
        return vibble::grid::clamp_resolution(shared_footer_bar_->grid_resolution());
    }
    MapGridSettings settings = current_room_ ? current_room_->map_grid_settings() : MapGridSettings::defaults();
    settings.clamp();
    return vibble::grid::clamp_resolution(settings.grid_resolution);
}

void RoomEditor::refresh_cursor_snap() {
    if (!has_last_raw_mouse_world_) {
        return;
    }
    if (!snap_to_grid_enabled_) {
        cursor_snap_resolution_ = 0;
        snapped_cursor_world_ = last_raw_mouse_world_;
        return;
    }
    cursor_snap_resolution_ = current_grid_resolution();
    snapped_cursor_world_ = snap_world_point_to_overlay_grid(last_raw_mouse_world_, cursor_snap_resolution_);
}

void RoomEditor::refresh_spawn_group_config_ui() {
    if (!current_room_) {
        if (spawn_group_panel_) {
            spawn_group_panel_->set_visible(false);
        }
        return;
    }
    ensure_spawn_group_config_ui();
    if (!spawn_group_panel_) {
        return;
    }

    spawn_group_panel_->set_screen_dimensions(screen_w_, screen_h_);
    spawn_group_panel_->set_work_area(SDL_Rect{0, 0, screen_w_, screen_h_});
    auto reopen = spawn_group_panel_->expanded_groups();

    auto& root = current_room_->assets_data();
    auto& arr = ensure_spawn_groups_array(root);
    if (sanitize_perimeter_spawn_groups(arr)) {
        save_current_room_assets_json();
    }
    rebuild_room_spawn_id_cache();

    const int default_resolution = current_room_->map_grid_settings().grid_resolution;
    spawn_group_panel_->set_default_resolution(default_resolution);

    auto area_names_provider = [this]() {
        std::vector<std::string> names;
        if (!current_room_) {
            return names;
        }
        auto& data = current_room_->assets_data();
        if (data.contains("areas") && data["areas"].is_array()) {
            for (const auto& entry : data["areas"]) {
                if (!entry.is_object()) continue;
                const auto name_it = entry.find("name");
                if (name_it != entry.end() && name_it->is_string()) {
                    names.push_back(name_it->get<std::string>());
                }
            }
        }
        if (names.empty()) {
            for (const auto& named : current_room_->areas) {
                if (!named.name.empty()) {
                    names.push_back(named.name);
                }
            }
        }
        return names;
};

    auto on_change = [this]() {
        if (!current_room_) {
            return;
        }
        commit_room_edit_transaction([]() { return true; }, "spawn group update", false,
                                     devmode::core::DevSaveCoordinator::Priority::Debounced);
};

    auto on_entry_change = [this](const nlohmann::json& entry, const SpawnGroupConfig::ChangeSummary& summary) {
        if (!current_room_) {
            return;
        }
        bool sanitized = false;
        const bool committed = commit_room_edit_transaction([&sanitized, this, &entry]() {
            if (entry.is_object()) {
                const std::string id = entry.value("spawn_id", std::string{});
                SpawnEntryResolution current = locate_spawn_entry(id);
                if (current.owner_array) {
                    sanitized = sanitize_perimeter_spawn_groups(*current.owner_array);
                }
            }
            return true;
        }, "spawn group update", false,
        devmode::core::DevSaveCoordinator::Priority::Debounced);
        if (committed && (sanitized || summary.method_changed || summary.quantity_changed || summary.candidates_changed ||
            summary.resolution_changed)) {
            respawn_spawn_group(entry);
        }
};

    SpawnGroupConfig::ConfigureEntryCallback configure_entry = [area_names_provider, this](
                                                                 SpawnGroupConfig::EntryController& entry,
                                                                 const nlohmann::json&) {
        entry.set_area_names_provider(area_names_provider);
        if (current_room_) {
            const std::string label = current_room_->room_name.empty() ? std::string("Room") : current_room_->room_name;
            entry.set_ownership_label(label, SDL_Color{255, 224, 96, 255});
        }

};

    SpawnEntryResolution resolved;
    if (active_spawn_group_id_) {
        resolved = locate_spawn_entry(*active_spawn_group_id_);
        if (resolved.source == SpawnEntryResolution::Source::Map && resolved.owner_array) {
            if (sanitize_perimeter_spawn_groups(*resolved.owner_array)) {
                mark_map_dirty_for_spawn_groups(devmode::core::DevSaveCoordinator::Priority::Debounced);
            }
        }
    }

    auto map_on_change = [this]() {
        mark_map_dirty_for_spawn_groups(devmode::core::DevSaveCoordinator::Priority::Debounced);
};

    auto map_on_entry_change = [this](const nlohmann::json& entry, const SpawnGroupConfig::ChangeSummary& summary) {
        bool sanitized = false;
        if (entry.is_object()) {
            const std::string id = entry.value("spawn_id", std::string{});
            SpawnEntryResolution current = locate_spawn_entry(id);
            if (current.owner_array) {
                sanitized = sanitize_perimeter_spawn_groups(*current.owner_array);
            }
        }
        mark_map_dirty_for_spawn_groups(devmode::core::DevSaveCoordinator::Priority::Debounced);
        if (sanitized || summary.method_changed || summary.quantity_changed || summary.candidates_changed ||
            summary.resolution_changed) {
            if (assets_) {
                assets_->notify_spawn_group_config_changed(entry);
            }
        }
};

    if (resolved.valid()) {
        if (resolved.source == SpawnEntryResolution::Source::Room) {
            const std::string spawn_id = *active_spawn_group_id_;
            spawn_group_panel_->bind_entry_by_id(spawn_id,
                                                 [this, spawn_id]() -> nlohmann::json* {
                                                     SpawnEntryResolution lookup = locate_spawn_entry(spawn_id);
                                                     return lookup.source == SpawnEntryResolution::Source::Room ? lookup.entry : nullptr;
                                                 },
                                                 on_change,
                                                 on_entry_change,
                                                 SpawnGroupConfig::EntryCallbacks{},
                                                 configure_entry);
        } else {
            spawn_group_panel_->bind_entry(*resolved.entry,
                                           map_on_change,
                                           map_on_entry_change,
                                           SpawnGroupConfig::EntryCallbacks{},
                                           configure_entry);
        }
    } else {
        spawn_group_panel_->load(arr, on_change, on_entry_change, configure_entry);
        spawn_group_panel_->restore_expanded_groups(reopen);
        spawn_group_panel_->set_scroll_enabled(true);
    }
    update_spawn_group_config_anchor();
}

void RoomEditor::update_spawn_group_config_anchor() {
    if (!spawn_group_panel_) {
        return;
    }
    spawn_group_panel_->set_screen_dimensions(screen_w_, screen_h_);
    spawn_group_panel_->set_work_area(SDL_Rect{0, 0, screen_w_, screen_h_});
    SDL_Point anchor = spawn_groups_anchor_point();
    spawn_group_panel_->set_anchor(anchor.x, anchor.y);
}

SDL_Point RoomEditor::spawn_groups_anchor_point() const {
    SDL_Rect reference = room_config_bounds_;
    if (room_cfg_ui_) {
        const SDL_Rect rect = room_cfg_ui_->panel_rect();
        if (rect.w > 0 || rect.h > 0) {
            reference = rect;
        }
    }
    int anchor_x = reference.x + reference.w + 16;
    int anchor_y = reference.y;
    return SDL_Point{anchor_x, anchor_y};
}

void RoomEditor::clear_active_spawn_group_target() {
    active_spawn_group_id_.reset();
}

void RoomEditor::sync_spawn_group_panel_with_selection() {
    Asset* primary = selected_assets_.empty() ? nullptr : selected_assets_.front();
    std::string spawn_id;
    if (primary) {
        spawn_id = primary->spawn_id;
    }

    update_grid_resolution_for_selection(primary);

    if (spawn_id.empty() && spawn_group_panel_ && spawn_group_panel_->is_visible() && active_spawn_group_id_) {
        spawn_id = *active_spawn_group_id_;
    }

    if (spawn_id.empty()) {
        if (spawn_group_panel_) {
            spawn_group_panel_->close();
        }
        clear_active_spawn_group_target();
        return;
    }

    const bool boundary_asset = primary && primary->info && primary->info->type == asset_types::boundary;
    SpawnEntryResolution resolved = locate_spawn_entry(spawn_id);
    auto owner_matches_section = [&](const char* section_key) -> bool {
        if (resolved.source != SpawnEntryResolution::Source::Map) {
            return false;
        }
        if (!resolved.owner_array || !assets_) {
            return false;
        }
        nlohmann::json& map_info = assets_->map_info_json();
        if (!map_info.is_object()) {
            return false;
        }
        auto section_it = map_info.find(section_key);
        if (section_it == map_info.end() || !section_it->is_object()) {
            return false;
        }
        auto groups_it = section_it->find("spawn_groups");
        if (groups_it == section_it->end() || !groups_it->is_array()) {
            return false;
        }
        return &(*groups_it) == resolved.owner_array;
};

    const bool map_assets_entry = owner_matches_section("map_assets_data");
    const bool boundary_entry   = owner_matches_section("map_boundary_data");
    const bool should_open_map_assets_panel = map_assets_entry && map_assets_panel_requested_by_shift_click_;
    map_assets_panel_requested_by_shift_click_ = false;

    auto close_spawn_group_panel = [&]() {
        if (spawn_group_panel_) {
            spawn_group_panel_->close();
            spawn_group_panel_->set_visible(false);
        }
};

    auto close_room_config_preserving_selection = [this]() {
        if (!room_config_dock_open_) {
            return;
        }
        suppress_room_config_selection_clear_ = true;
        set_room_config_visible(false);
        suppress_room_config_selection_clear_ = false;
};

    if (boundary_entry || boundary_asset) {
        close_spawn_group_panel();
        clear_active_spawn_group_target();
        close_room_config_preserving_selection();
        return;
    }

    if (map_assets_entry) {
        close_spawn_group_panel();
        clear_active_spawn_group_target();
        close_room_config_preserving_selection();
        if (should_open_map_assets_panel && open_map_assets_panel_callback_) {
            open_map_assets_panel_callback_();
        }
        return;
    }

    active_spawn_group_id_ = spawn_id;

    bool focused = false;
    if (room_cfg_ui_ && room_config_dock_open_) {
        focused = room_cfg_ui_->focus_spawn_group(spawn_id);
    }

    if (focused && spawn_group_panel_ && spawn_group_panel_->is_visible()) {
        DockManager::instance().bring_to_front(spawn_group_panel_.get());
    }
}

void RoomEditor::update_grid_resolution_for_selection(Asset* primary) {
    if (!shared_footer_bar_) {
        return;
    }

    if (snap_to_grid_enabled_) {
        clear_selection_grid_resolution_override();
        if (!primary || primary->spawn_id.empty()) {
            return;
        }
        const int resolution = current_grid_resolution();
        if (resolution > 0) {
            snap_spawn_group_to_resolution(primary, resolution);
        }
        return;
    }

    if (!primary || primary->spawn_id.empty()) {
        clear_selection_grid_resolution_override();
        return;
    }

    int selected_resolution = 0;
    SpawnEntryResolution resolved = locate_spawn_entry(primary->spawn_id);
    if (resolved.entry) {
        selected_resolution = resolved.entry->value("resolution", 0);
    } else if (primary->grid_resolution > 0) {
        selected_resolution = primary->grid_resolution;
    }
    selected_resolution = vibble::grid::clamp_resolution(std::max(0, selected_resolution));
    if (selected_resolution <= 0) {
        clear_selection_grid_resolution_override();
        return;
    }

    if (!selection_overlay_resolution_before_override_.has_value()) {
        selection_overlay_resolution_before_override_ = vibble::grid::clamp_resolution(shared_footer_bar_->grid_resolution());
    }
    selection_overlay_resolution_override_ = selected_resolution;

    if (vibble::grid::clamp_resolution(shared_footer_bar_->grid_resolution()) != selected_resolution) {
        shared_footer_bar_->set_grid_resolution(selected_resolution);
    }
}

void RoomEditor::clear_selection_grid_resolution_override() {
    if (!shared_footer_bar_ || !selection_overlay_resolution_before_override_.has_value()) {
        selection_overlay_resolution_before_override_.reset();
        selection_overlay_resolution_override_.reset();
        return;
    }

    const int current = vibble::grid::clamp_resolution(shared_footer_bar_->grid_resolution());
    const bool should_restore = selection_overlay_resolution_override_.has_value() &&
                                current == *selection_overlay_resolution_override_;
    if (should_restore) {
        shared_footer_bar_->set_grid_resolution(*selection_overlay_resolution_before_override_);
    }

    selection_overlay_resolution_before_override_.reset();
    selection_overlay_resolution_override_.reset();
}

void RoomEditor::sanitize_perimeter_spawn_groups() {
    if (!current_room_) return;
    auto& root = current_room_->assets_data();
    auto& arr = ensure_spawn_groups_array(root);
    if (sanitize_perimeter_spawn_groups(arr)) {
        save_current_room_assets_json();
    }
}

bool RoomEditor::sanitize_perimeter_spawn_groups(nlohmann::json& groups) {
    return devmode::spawn::sanitize_perimeter_spawn_groups(groups);
}

std::optional<RoomEditor::PerimeterOverlay> RoomEditor::compute_perimeter_overlay_for_drag() {
    if (!dragging_) return std::nullopt;
    if (drag_mode_ != DragMode::Perimeter && drag_mode_ != DragMode::PerimeterCenter) {
        return std::nullopt;
    }
    Asset* reference = drag_anchor_asset_;
    if (!reference) {
        for (const auto& state : drag_states_) {
            if (state.asset) {
                reference = state.asset;
                break;
            }
        }
    }
    if (!reference) return std::nullopt;
    PerimeterOverlay overlay;
    overlay.center = drag_perimeter_circle_center_;
    double dx = static_cast<double>(reference->world_xz_point().x - overlay.center.x);
    double dy = static_cast<double>(reference->world_xz_point().y - overlay.center.y);
    overlay.radius = std::hypot(dx, dy);
    if (!std::isfinite(overlay.radius) || overlay.radius <= 0.0) {
        return std::nullopt;
    }
    return overlay;
}

std::optional<RoomEditor::PerimeterOverlay> RoomEditor::compute_perimeter_overlay_for_spawn(const std::string& spawn_id) {
    if (spawn_id.empty() || !current_room_) return std::nullopt;
    auto& root = current_room_->assets_data();
    auto& arr = ensure_spawn_groups_array(root);
    nlohmann::json* entry = nullptr;
    for (auto& item : arr) {
        if (!item.is_object()) continue;
        if (item.contains("spawn_id") && item["spawn_id"].is_string() && item["spawn_id"].get<std::string>() == spawn_id) {
            entry = &item;
            break;
        }
    }
    if (!entry) return std::nullopt;
    std::string method = entry->value("position", std::string{});
    if (method == "Exact Position") method = "Exact";
    if (method != "Perimeter") return std::nullopt;
    PerimeterOverlay overlay;
    overlay.center = get_room_center();
    auto [room_w, room_h] = get_room_dimensions();
    bool resolve_geometry = entry->value("resolve_geometry_to_room_size", true);
    int orig_w = std::max(1, entry->value("origional_width", room_w));
    int orig_h = std::max(1, entry->value("origional_height", room_h));
    if (!resolve_geometry) {
        orig_w = std::max(1, room_w);
        orig_h = std::max(1, room_h);
    }
    int stored_dx = entry->value("dx", 0);
    int stored_dz = entry->value("dz", 0);
    RelativeRoomPosition relative(SDL_Point{stored_dx, stored_dz}, orig_w, orig_h);
    SDL_Point scaled = relative.scaled_offset(room_w, room_h);
    overlay.center.x += scaled.x;
    overlay.center.y += scaled.y;
    int base_radius = entry->value("radius", 0);
    if (resolve_geometry) {
        const double width_ratio = static_cast<double>(std::max(1, room_w)) / static_cast<double>(std::max(1, orig_w));
        const double height_ratio = static_cast<double>(std::max(1, room_h)) / static_cast<double>(std::max(1, orig_h));
        const double ratio = (width_ratio + height_ratio) * 0.5;
        overlay.radius = static_cast<double>(base_radius) * ratio;
    } else {
        overlay.radius = static_cast<double>(base_radius);
    }
    if (overlay.radius <= 0.0 && active_assets_) {
        for (Asset* asset : *active_assets_) {
            if (!asset || asset->spawn_id != spawn_id) continue;
            double dx = static_cast<double>(asset->world_x() - overlay.center.x);
            double dz = static_cast<double>(asset->world_z() - overlay.center.y);
            overlay.radius = std::hypot(dx, dz);
            if (overlay.radius > 0.0) break;
        }
    }
    if (!std::isfinite(overlay.radius) || overlay.radius <= 0.0) {
        return std::nullopt;
    }
    return overlay;
}

std::optional<std::vector<SDL_Point>> RoomEditor::compute_edge_path_for_drag() {
    if (!dragging_) return std::nullopt;
    if (drag_mode_ != DragMode::Edge) return std::nullopt;
    const Area* area = drag_edge_area_ ? drag_edge_area_ : (current_room_ ? current_room_->room_area.get() : nullptr);
    if (!area) return std::nullopt;
    SDL_Point center = drag_edge_center_;
    int inset = static_cast<int>(std::lround(drag_edge_inset_percent_));
    inset = std::clamp(inset, 0, 200);
    const auto& pts = area->get_points();
    if (pts.size() < 2) return std::nullopt;
    const double scale = std::clamp(static_cast<double>(inset) / 100.0, 0.0, 2.0);
    std::vector<SDL_Point> path;
    path.reserve(pts.size() + 1);
    for (const auto& p : pts) {
        const double vx = static_cast<double>(p.x - center.x);
        const double vy = static_cast<double>(p.y - center.y);
        SDL_Point q{ static_cast<int>(std::lround(center.x + vx * scale)),
                     static_cast<int>(std::lround(center.y + vy * scale)) };
        path.push_back(q);
    }
    if (!path.empty()) path.push_back(path.front());
    return path;
}

std::optional<std::vector<SDL_Point>> RoomEditor::compute_edge_path_for_spawn(const std::string& spawn_id) {
    if (spawn_id.empty() || !current_room_) return std::nullopt;
    nlohmann::json* entry = find_spawn_entry(spawn_id);
    if (!entry) return std::nullopt;
    std::string method = entry->value("position", std::string{});
    if (method == "Exact Position") method = "Exact";
    if (method != "Edge") return std::nullopt;
    const Area* area = find_edge_area_for_entry(*entry);
    if (!area) return std::nullopt;
    SDL_Point center = area->get_center();
    int inset = std::clamp(entry->value("edge_inset_percent", 100), 0, 200);
    const auto& pts = area->get_points();
    if (pts.size() < 2) return std::nullopt;
    const double scale = std::clamp(static_cast<double>(inset) / 100.0, 0.0, 2.0);
    std::vector<SDL_Point> path;
    path.reserve(pts.size() + 1);
    for (const auto& p : pts) {
        const double vx = static_cast<double>(p.x - center.x);
        const double vy = static_cast<double>(p.y - center.y);
        SDL_Point q{ static_cast<int>(std::lround(center.x + vx * scale)),
                     static_cast<int>(std::lround(center.y + vy * scale)) };
        path.push_back(q);
    }
    if (!path.empty()) path.push_back(path.front());
    return path;
}

void RoomEditor::add_spawn_group_internal() {
    if (!current_room_) return;

    const std::string new_spawn_id = generate_spawn_id();
    const bool committed = commit_room_edit_transaction([this, &new_spawn_id]() {
        auto& root = current_room_->assets_data();
        auto& arr = ensure_spawn_groups_array(root);
        nlohmann::json entry;
        entry["spawn_id"] = new_spawn_id;
        const int add_default_resolution = current_grid_resolution();
        devmode::spawn::ensure_spawn_group_entry_defaults(entry, "New Spawn", add_default_resolution);
        arr.push_back(entry);
        for (size_t i = 0; i < arr.size(); ++i) {
            if (arr[i].is_object()) arr[i]["priority"] = static_cast<int>(i);
        }
        sanitize_perimeter_spawn_groups(arr);
        active_spawn_group_id_ = new_spawn_id;
        return true;
    }, "spawn group add", true, devmode::core::DevSaveCoordinator::Priority::Debounced);
    if (committed) {
        open_spawn_group_editor_by_id(new_spawn_id);
    }
}

bool RoomEditor::delete_spawn_group_internal(const std::string& spawn_id) {
    if (!current_room_) {
        return false;
    }

    const bool deleted = commit_room_edit_transaction([this, &spawn_id]() {
        if (!remove_spawn_group_by_id(spawn_id)) {
            return false;
        }
        if (active_spawn_group_id_ && *active_spawn_group_id_ == spawn_id) {
            clear_active_spawn_group_target();
        }
        return true;
    }, "spawn group deletion", true, devmode::core::DevSaveCoordinator::Priority::Debounced);
    if (!deleted) {
        return false;
    }

    if (assets_) {
        assets_->notify_spawn_group_removed(spawn_id);
        assets_->refresh_active_asset_lists();
        mark_highlight_dirty();
    }
    return true;
}

bool RoomEditor::remove_spawn_group_by_id(const std::string& spawn_id) {
    if (spawn_id.empty() || !current_room_) return false;
    auto& root = current_room_->assets_data();
    auto& arr = ensure_spawn_groups_array(root);
    if (!arr.is_array() || arr.size() <= 1) return false;
    auto it = std::find_if(arr.begin(), arr.end(), [&spawn_id](const nlohmann::json& e) {
        if (!e.is_object()) return false;
        if (!e.contains("spawn_id") || !e["spawn_id"].is_string()) return false;
        return e["spawn_id"].get<std::string>() == spawn_id;
    });
    if (it == arr.end()) {
        return false;
    }
    arr.erase(it);
    for (size_t i = 0; i < arr.size(); ++i) {
        if (arr[i].is_object()) {
            arr[i]["priority"] = static_cast<int>(i);
        }
    }
    return true;
}

void RoomEditor::reorder_spawn_group_internal(const std::string& spawn_id, size_t target_index) {
    if (!current_room_ || spawn_id.empty()) return;

    commit_room_edit_transaction([this, &spawn_id, target_index]() {
        auto& root = current_room_->assets_data();
        auto& arr = ensure_spawn_groups_array(root);
        if (!arr.is_array() || arr.empty()) return false;

        size_t current_index = arr.size();
        for (size_t i = 0; i < arr.size(); ++i) {
            const auto& entry = arr[i];
            if (!entry.is_object()) continue;
            if (entry.contains("spawn_id") && entry["spawn_id"].is_string() && entry["spawn_id"].get<std::string>() == spawn_id) {
                current_index = i;
                break;
            }
        }
        if (current_index >= arr.size()) return false;

        const size_t bounded_index = std::min(target_index, arr.size() - 1);
        if (current_index == bounded_index) return true;

        nlohmann::json entry = std::move(arr[current_index]);
        const auto erase_pos = arr.begin() + static_cast<nlohmann::json::difference_type>(current_index);
        arr.erase(erase_pos);
        size_t insert_index = std::min(bounded_index, arr.size());
        const auto insert_pos = arr.begin() + static_cast<nlohmann::json::difference_type>(insert_index);
        arr.insert(insert_pos, std::move(entry));

        for (size_t i = 0; i < arr.size(); ++i) {
            if (arr[i].is_object()) arr[i]["priority"] = static_cast<int>(i);
        }
        return true;
    }, "spawn group reorder", true, devmode::core::DevSaveCoordinator::Priority::Debounced);
}

void RoomEditor::open_spawn_group_editor_by_id(const std::string& spawn_id) {
    if (spawn_id.empty()) {
        return;
    }
    if (!current_room_) {
        return;
    }

    set_room_config_visible(true);
    if (room_cfg_ui_) {
        room_cfg_ui_->focus_spawn_group(spawn_id);
    }

    if (spawn_group_panel_) {
        spawn_group_panel_->close();
        spawn_group_panel_->set_visible(false);
    }
}

void RoomEditor::open_spawn_group_floating_panel(const std::string& spawn_id, std::optional<SDL_Point> screen_anchor) {
    if (spawn_id.empty()) {
        return;
    }
    if (active_modal_ == ActiveModal::AssetInfo) {
        pulse_active_modal_header();
        return;
    }

    ensure_spawn_group_config_ui();
    if (!spawn_group_panel_) {
        return;
    }

    SpawnEntryResolution resolved = locate_spawn_entry(spawn_id);
    if (!resolved.valid()) {
        return;
    }

    active_spawn_group_id_ = spawn_id;
    const bool locked = spawn_group_locked(spawn_id);

    bool has_matching_asset = false;
    if (assets_) {
        for (Asset* asset : assets_->all) {
            if (!asset || asset->dead) continue;
            if (!asset_belongs_to_room(asset)) continue;
            if (asset->spawn_id == spawn_id) {
                has_matching_asset = true;
                break;
            }
        }
    }
    if (has_matching_asset && !locked) {
        select_spawn_group_assets(spawn_id);
    }

    refresh_spawn_group_config_ui();
    update_spawn_group_config_anchor();
    spawn_group_panel_->set_screen_dimensions(screen_w_, screen_h_);
    spawn_group_panel_->set_work_area(DockManager::instance().usableRect());
    spawn_group_panel_->reset_scroll();
    spawn_group_panel_->open();
    spawn_group_panel_->force_pointer_ready();

    SDL_Rect work = DockManager::instance().usableRect();
    if (work.w <= 0 || work.h <= 0) {
        work = SDL_Rect{0, 0, screen_w_, screen_h_};
    }

    SDL_Point desired = screen_anchor.value_or(spawn_groups_anchor_point());
    if (screen_anchor) {
        desired.x += 12;
        desired.y += 12;
    }

    SDL_Rect rect = spawn_group_panel_->rect();
    const int width = std::max(rect.w, DockableCollapsible::kDefaultFloatingContentWidth);
    const int height = std::max(rect.h, 420);
    const int max_x = std::max(work.x, work.x + work.w - width);
    const int max_y = std::max(work.y, work.y + work.h - height);
    const int min_x = work.x;
    const int min_y = work.y;
    const int pos_x = std::clamp(desired.x, min_x, max_x);
    const int pos_y = std::clamp(desired.y, min_y, max_y);
    spawn_group_panel_->set_position(pos_x, pos_y);

    DockManager::instance().open_floating(
        "Spawn Group: " + spawn_id,
        spawn_group_panel_.get(),
        [this]() {
            if (spawn_group_panel_) {
                spawn_group_panel_->close();
            }
        },
        "spawn_group_panel");
}

void RoomEditor::reopen_room_configurator() {
    if (!room_cfg_ui_) return;
    if (!room_config_dock_open_) {
        return;
    }
    if (!room_cfg_ui_->refresh_spawn_groups(current_room_)) {
        room_cfg_ui_->open(current_room_);
    }
}

void RoomEditor::rebuild_room_spawn_id_cache() {
    room_spawn_ids_.clear();
    if (!current_room_) return;
    auto& root = current_room_->assets_data();
    auto& arr = ensure_spawn_groups_array(root);
    for (const auto& entry : arr) {
        if (!entry.is_object()) continue;
        if (entry.contains("spawn_id") && entry["spawn_id"].is_string()) {
            room_spawn_ids_.insert(entry["spawn_id"].get<std::string>());
        }
    }
}

bool RoomEditor::is_room_spawn_id(const std::string& spawn_id) const {
    if (spawn_id.empty()) return false;
    return room_spawn_ids_.find(spawn_id) != room_spawn_ids_.end();
}

bool RoomEditor::asset_belongs_to_room(const Asset* ) const {
    return true;
}

void RoomEditor::handle_spawn_config_change(const nlohmann::json& entry) {

    respawn_spawn_group(entry);
}

std::unique_ptr<vibble::grid::Occupancy> RoomEditor::build_room_grid(const std::string& ignore_spawn_id) const {
    if (!current_room_ || !current_room_->room_area) return nullptr;
    MapGridSettings grid_settings = current_room_->map_grid_settings();
    const int resolution = std::max(0, grid_settings.grid_resolution);
    vibble::grid::Grid& grid_service = vibble::grid::global_grid();
    auto occupancy = std::make_unique<vibble::grid::Occupancy>(*current_room_->room_area, resolution, grid_service);
    if (!assets_) return occupancy;
    for (Asset* asset : assets_->all) {
        if (!asset || asset->dead) continue;
        if (!asset_belongs_to_room(asset)) continue;
        if (!asset->spawn_id.empty() && asset->spawn_id == ignore_spawn_id) continue;
        SDL_Point pos{asset->world_x(), asset->world_z()};
        if (current_room_->room_area && !current_room_->room_area->contains_point(pos)) continue;
        if (auto* vertex = occupancy->vertex_at_world(pos)) {
            occupancy->set_occupied(vertex, true);
        }
    }
    return occupancy;
}

bool RoomEditor::snap_spawn_group_to_resolution(Asset* anchor, int resolution) {
    if (!anchor || !active_assets_ || resolution <= 0) return false;
    if (anchor->spawn_id.empty()) return false;
    if (spawn_group_locked(anchor->spawn_id)) return false;

    vibble::grid::Grid& grid_service = vibble::grid::global_grid();
    const int clamped = vibble::grid::clamp_resolution(std::max(0, resolution));
    SDL_Point current = anchor->world_xz_point();
    SDL_Point snapped = grid_service.snap_to_vertex(current, clamped);

    const int dx = snapped.x - current.x;
    const int dz = snapped.y - current.y;
    const bool moved = (dx != 0 || dz != 0);
    bool changed = false;

    // Move all assets in the spawn group together.
    for (Asset* asset : *active_assets_) {
        if (!asset || asset->dead) continue;
        if (!asset_belongs_to_room(asset)) continue;
        if (asset->spawn_id != anchor->spawn_id) continue;
        if (moved) {
            asset->move_to_world_position(asset->world_x() + dx, asset->world_y(), asset->world_z() + dz);
            changed = true;
        }
        if (asset->grid_resolution != clamped) {
            asset->grid_resolution = clamped;
            changed = true;
        }
    }

    bool persist_needed = false;
    SpawnEntryResolution resolved = locate_spawn_entry(anchor->spawn_id);
    if (resolved.entry) {
        if (resolved.entry->value("resolution", 0) != clamped) {
            (*resolved.entry)["resolution"] = clamped;
            persist_needed = true;
        }

        if (moved) {
            SDL_Point center = get_room_center();
            auto [width, height] = get_room_dimensions();
            std::string method = resolved.entry->value("position", std::string{});

            if (method == "Percent") {
                update_percent_json(*resolved.entry, *anchor, center, width, height);
                persist_needed = true;
            } else if (method.empty() || method == "Exact" || method == "Exact Position") {
                update_exact_json(*resolved.entry, *anchor, center, width, height);
                persist_needed = true;
            }
        }
    }

    if (persist_needed) {
        if (resolved.source == SpawnEntryResolution::Source::Room) {
            save_current_room_assets_json();
        } else if (resolved.source == SpawnEntryResolution::Source::Map) {
            mark_map_dirty_for_spawn_groups(devmode::core::DevSaveCoordinator::Priority::Debounced);
            if (assets_) {
                assets_->notify_spawn_group_config_changed(*resolved.entry);
            }
        }
        refresh_spawn_group_config_ui();
    }

    if (changed) {
        mark_spatial_index_dirty();
        mark_highlight_dirty();
    }
    return changed || persist_needed;
}

void RoomEditor::integrate_spawned_assets(std::vector<std::unique_ptr<Asset>>& spawned) {
    if (!assets_) return;
    if (spawned.empty()) return;
    for (auto& uptr : spawned) {
        if (!uptr) continue;
        Asset* raw = uptr.get();
        set_camera_recursive(raw, &assets_->getView());
        set_assets_owner_recursive(raw, assets_);
        raw->finalize_setup();
        raw = assets_->world_grid().create_asset_at_point(std::move(uptr));
        if (raw) {
            assets_->all.push_back(raw);
        }
    }
    const SDL_Point center_px = assets_->getView().get_screen_center();
    const world::GridPoint center_point = world::GridPoint::make_virtual(
        center_px.x, 0, center_px.y, assets_->world_grid().max_resolution_layers());
    assets_->initialize_active_assets(center_point);
    assets_->refresh_active_asset_lists();
    mark_spatial_index_dirty();
    spawned.clear();
    mark_highlight_dirty();
}

void RoomEditor::regenerate_current_room() {
    if (!assets_ || !current_room_ || !current_room_->room_area) {
        return;
    }

    auto& root = current_room_->assets_data();
    auto& groups = ensure_spawn_groups_array(root);
    std::vector<nlohmann::json> entries;
    entries.reserve(groups.size());
    for (const auto& entry : groups) {
        if (entry.is_object()) {
            entries.push_back(entry);
        }
    }

    for (const auto& entry : entries) {
        respawn_spawn_group(entry);
    }

    rebuild_room_spawn_id_cache();
    save_current_room_assets_json();
}

void RoomEditor::respawn_spawn_group(const nlohmann::json& entry) {
    if (!assets_ || !current_room_ || !current_room_->room_area) return;
    if (!entry.is_object()) return;
    std::string spawn_id = entry.value("spawn_id", std::string{});
    if (spawn_id.empty()) return;

    assets_->delete_assets_for_spawn_group(spawn_id);
    assets_->rebuild_from_grid_state();
    assets_->refresh_active_asset_lists();

    auto occupancy = build_room_grid(spawn_id);
    vibble::grid::Grid& grid_service = vibble::grid::global_grid();

    nlohmann::json root;
    root["spawn_groups"] = nlohmann::json::array();
    root["spawn_groups"].push_back(entry);
    std::vector<nlohmann::json> sources{root};
    AssetSpawnPlanner planner(sources, *current_room_->room_area, assets_->library());
    const auto& queue = planner.get_spawn_queue();
    if (queue.empty()) return;

    std::unordered_map<std::string, std::shared_ptr<AssetInfo>> asset_info_library = assets_->library().all();
    std::vector<std::unique_ptr<Asset>> spawned;
    std::vector<Area> exclusion;
    std::mt19937 rng(std::random_device{}());
    Check checker(false);
    int spawn_resolution = occupancy ? occupancy->resolution() : grid_service.default_resolution();
    checker.begin_session(grid_service, spawn_resolution);
    SpawnContext ctx(rng, checker, exclusion, asset_info_library, spawned, &assets_->library(), grid_service, occupancy.get());
    if (current_room_) {
        ctx.set_map_grid_settings(current_room_->map_grid_settings());
    }
    if (occupancy) {
        ctx.set_spawn_resolution(occupancy->resolution());
    }
    std::vector<const Area*> trail_areas;
    if (current_room_) {
        auto add_trail_area = [&trail_areas](const Area* candidate, const std::string& type) {
            if (!candidate) return;
            std::string lowered = type;
            std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char ch) {
                return static_cast<char>(std::tolower(ch));
            });
            if (lowered == "trail") {
                trail_areas.push_back(candidate);
            }
};
        if (current_room_->room_area) {
            add_trail_area(current_room_->room_area.get(), current_room_->room_area->get_type());
        }
        for (const auto& named : current_room_->areas) {
            add_trail_area(named.area.get(), named.type);
        }
    }
    ctx.set_trail_areas(std::move(trail_areas));
    SpawnMethod spawn_method;
    const Area* area = current_room_->room_area.get();
    for (const auto& info : queue) {
        spawn_method.spawn(info, area, ctx);
    }
    integrate_spawned_assets(spawned);
    checker.reset_session();

    const Area* old_area_copy = current_room_->room_area.get();
    const double old_area_size = old_area_copy ? old_area_copy->get_size() : 0.0;
    const double new_area_size = old_area_size;

    // NOTE: Boundary assets are now rendered dynamically via DynamicBoundarySystem.
    // No need to spawn static boundary assets when room area shrinks.
    (void)old_area_copy;
    (void)new_area_size;
    (void)old_area_size;

    std::string player_asset_name;
    if (assets_) {
        if (assets_->player && assets_->player->info) {
            player_asset_name = assets_->player->info->name;
        }
        if (player_asset_name.empty()) {
            for (const auto& pair : assets_->library().all()) {
                if (!pair.second) continue;
                if (pair.second->type == asset_types::player) {
                    player_asset_name = pair.second->name;
                    break;
                }
            }
        }
    }

    Asset* existing_player = nullptr;
    for (Asset* asset : assets_->all) {
        if (!asset || asset->dead || !asset->info) {
            continue;
        }
        if (asset->info->type == asset_types::player) {
            existing_player = asset;
            break;
        }
    }

    if (existing_player) {
        assets_->player = existing_player;
        player_ = existing_player;
    } else if (!player_asset_name.empty() && current_room_->room_area) {
        auto is_clear = [&](SDL_Point point) {
            for (Asset* asset : assets_->all) {
                if (!asset || asset->dead) {
                    continue;
                }
                Area impassable = asset->get_area("impassable");
                if (!impassable.get_points().empty() && impassable.contains_point(point)) {
                    return false;
                }
            }
            return true;
};

        auto bounds = current_room_->room_area->get_bounds();
        int minx = std::get<0>(bounds);
        int miny = std::get<1>(bounds);
        int maxx = std::get<2>(bounds);
        int maxy = std::get<3>(bounds);
        std::mt19937 regen_rng(std::random_device{}());
        std::uniform_int_distribution<int> dist_x(minx, maxx);
        std::uniform_int_distribution<int> dist_y(miny, maxy);

        SDL_Point spawn_point = current_room_->room_area->get_center();
        bool found_spot = current_room_->room_area->contains_point(spawn_point) && is_clear(spawn_point);
        if (!found_spot) {
            for (int attempt = 0; attempt < 200 && !found_spot; ++attempt) {
                SDL_Point candidate{dist_x(regen_rng), dist_y(regen_rng)};
                if (!current_room_->room_area->contains_point(candidate)) {
                    continue;
                }
                if (is_clear(candidate)) {
                    spawn_point = candidate;
                    found_spot = true;
                }
            }
        }
        if (!found_spot) {
            int step = std::max(1, std::min(maxx - minx + 1, maxy - miny + 1) / 25);
            for (int y = miny; y <= maxy && !found_spot; y += step) {
                for (int x = minx; x <= maxx && !found_spot; x += step) {
                    SDL_Point candidate{x, y};
                    if (!current_room_->room_area->contains_point(candidate)) {
                        continue;
                    }
                    if (is_clear(candidate)) {
                        spawn_point = candidate;
                        found_spot = true;
                    }
                }
            }
        }
        if (found_spot) {
            if (Asset* spawned_player = assets_->spawn_asset(player_asset_name, spawn_point)) {
                spawned_player->set_owning_room_name(current_room_->room_name);
                assets_->player = spawned_player;
                player_ = spawned_player;
            }
        }
    }

    refresh_spawn_group_config_ui();
    reopen_room_configurator();
}

void RoomEditor::update_exact_json(nlohmann::json& entry, const Asset& asset, SDL_Point center, int width, int height) {
    const int dx = asset.world_x() - center.x;
    const int dz = asset.world_z() - center.y;
    entry["dx"] = dx;
    entry["dz"] = dz;
    if (width > 0) entry["origional_width"] = width;
    if (height > 0) entry["origional_height"] = height;
}

void RoomEditor::update_percent_json(nlohmann::json& entry, const Asset& asset, SDL_Point center, int width, int height) {
    if (width <= 0 || height <= 0) return;
    auto clamp_percent = [](int v) { return std::max(-100, std::min(100, v)); };
    double half_w = static_cast<double>(width) / 2.0;
    double half_h = static_cast<double>(height) / 2.0;
    if (half_w <= 0.0 || half_h <= 0.0) return;
    double dx = static_cast<double>(asset.world_x() - center.x);
    double dz = static_cast<double>(asset.world_z() - center.y);
    int percent_x = clamp_percent(static_cast<int>(std::lround((dx / half_w) * 100.0)));
    int percent_z = clamp_percent(static_cast<int>(std::lround((dz / half_h) * 100.0)));
    entry["p_x_min"] = percent_x;
    entry["p_x_max"] = percent_x;
    entry["p_y_min"] = percent_z;
    entry["p_y_max"] = percent_z;
}

void RoomEditor::save_perimeter_json(nlohmann::json& entry, int dx, int dy, int orig_w, int orig_h, int radius) {
    entry["dx"] = dx;
    entry["dz"] = dy;
    entry["origional_width"] = orig_w;
    entry["origional_height"] = orig_h;
    entry["radius"] = radius;
    for (auto it = entry.begin(); it != entry.end(); ) {
        if (it.key().rfind("sector_") == 0) {
            it = entry.erase(it);
        } else {
            ++it;
        }
    }
}

void RoomEditor::save_edge_json(nlohmann::json& entry, int inset_percent) {
    int clamped = std::clamp(inset_percent, 0, 200);
    entry["edge_inset_percent"] = clamped;
}

double RoomEditor::edge_length_along_direction(const Area& area,
                                                   SDL_Point center,
                                                   SDL_FPoint direction) const {
    const auto& pts = area.get_points();
    const size_t count = pts.size();
    if (count < 2) {
        return 0.0;
    }
    double best = std::numeric_limits<double>::infinity();
    for (size_t i = 0; i < count; ++i) {
        const SDL_Point& a = pts[i];
        const SDL_Point& b = pts[(i + 1) % count];
        if (auto distance = ray_segment_distance(center, direction, a, b)) {
            if (*distance >= 0.0 && *distance < best) {
                best = *distance;
            }
        }
    }
    if (!std::isfinite(best) || best <= 0.0) {
        return 0.0;
    }
    return best;
}
bool RoomEditor::spawn_group_locked(const std::string& spawn_id) const {
    if (spawn_id.empty()) return false;

    RoomEditor* self = const_cast<RoomEditor*>(this);
    SpawnEntryResolution resolved = self->locate_spawn_entry(spawn_id);
    if (!resolved.valid() || !resolved.entry) return false;
    try {
        const auto& e = *resolved.entry;
        if (e.is_object() && e.contains("locked") && e["locked"].is_boolean()) {
            return e["locked"].get<bool>();
        }
    } catch (...) {}
    return false;
}

bool RoomEditor::asset_matches_selection_filter(const Asset* asset) const {
    if (!asset) return false;

    const bool is_anchored_asset = false;

    // Check if asset is a map asset
    const bool is_map_asset = !asset->spawn_id.empty() && !is_room_spawn_id(asset->spawn_id);

    // Check if asset is a boundary asset
    const bool is_boundary_asset = asset->info && asset->info->type == asset_types::boundary;

    // Check if asset is a tiled asset
    const bool is_tiled_asset = asset->info && asset->info->tillable;

    // Anchored assets are only selectable in anchored mode
    if (selection_filter_ != SelectionFilter::Anchored && is_anchored_asset) {
        return false;
    }

    switch (selection_filter_) {
        case SelectionFilter::Normal:
            // Normal assets: not map, not boundary, not tiled, not anchored
            return !is_map_asset && !is_boundary_asset && !is_tiled_asset;

        case SelectionFilter::Tiled:
            // Tiled assets only
            return is_tiled_asset;

        case SelectionFilter::MapWide:
            // Map-wide assets only
            return is_map_asset;

        case SelectionFilter::Boundary:
            // Boundary assets only
            return is_boundary_asset;

        case SelectionFilter::Anchored:
            // Anchored assets only
            return is_anchored_asset;

        default:
            return !is_anchored_asset;
    }
}

void RoomEditor::cycle_selection_filter() {
    switch (selection_filter_) {
        case SelectionFilter::Normal:
            selection_filter_ = SelectionFilter::Tiled;
            show_notice("Selecting tiled assets");
            break;
        case SelectionFilter::Tiled:
            selection_filter_ = SelectionFilter::MapWide;
            show_notice("Selecting map-wide assets");
            break;
        case SelectionFilter::MapWide:
            selection_filter_ = SelectionFilter::Boundary;
            show_notice("Selecting boundary assets");
            break;
        case SelectionFilter::Boundary:
            selection_filter_ = SelectionFilter::Anchored;
            show_notice("Selecting anchored assets");
            break;
        case SelectionFilter::Anchored:
            selection_filter_ = SelectionFilter::Normal;
            show_notice("Selecting normal assets");
            break;
        default:
            selection_filter_ = SelectionFilter::Normal;
            show_notice("Selecting normal assets");
            break;
    }

    // Clear hover and selection since filter changed
    hovered_asset_ = nullptr;
    hover_miss_frames_ = 3;
    mark_highlight_dirty();
}

void RoomEditor::reset_selection_filter() {
    const bool changed = (selection_filter_ != SelectionFilter::Normal);
    selection_filter_ = SelectionFilter::Normal;
    show_notice("Selecting normal assets");

    if (changed) {
        // Clear hover and selection since filter changed
        hovered_asset_ = nullptr;
        hover_miss_frames_ = 3;
        mark_highlight_dirty();
    }
}

void RoomEditor::render_asset_outline(SDL_Renderer* renderer, Asset* asset, const WarpedScreenGrid& cam, const SDL_Color& color, int outline_offset_px) const {
    if (!asset || !renderer) {
        return;
    }

    (void)outline_offset_px;

    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);

    SDL_Color overlay_color = color;
    const int base_alpha = (overlay_color.a == 0) ? 255 : overlay_color.a;
    const int target_alpha = static_cast<int>(std::lround(static_cast<float>(base_alpha) * 0.8f));
    overlay_color.a = static_cast<Uint8>(std::clamp(target_alpha, 0, 255));
    const float base_depth = static_cast<float>(asset->world_z());
    const auto* gp = cam.grid_point_for_asset(asset);
    const float perspective_scale =
        (gp && std::isfinite(gp->perspective_scale) && gp->perspective_scale > 0.0f)
            ? gp->perspective_scale
            : 1.0f;

    auto project_render_object_rect = [&](const RenderObject& obj, SDL_FRect& out_rect) -> bool {
        if (obj.screen_rect.w <= 0 || obj.screen_rect.h <= 0) {
            return false;
        }

        const float object_world_z = base_depth + obj.world_z_offset;
        render_projection::ProjectedSpriteFrame projection{};
        if (build_render_object_projection(cam, obj, perspective_scale, object_world_z, projection)) {
            const float min_x = std::min(std::min(projection.screen_tl.x, projection.screen_tr.x),
                                         std::min(projection.screen_bl.x, projection.screen_br.x));
            const float max_x = std::max(std::max(projection.screen_tl.x, projection.screen_tr.x),
                                         std::max(projection.screen_bl.x, projection.screen_br.x));
            const float min_y = std::min(std::min(projection.screen_tl.y, projection.screen_tr.y),
                                         std::min(projection.screen_bl.y, projection.screen_br.y));
            const float max_y = std::max(std::max(projection.screen_tl.y, projection.screen_tr.y),
                                         std::max(projection.screen_bl.y, projection.screen_br.y));
            if (std::isfinite(min_x) && std::isfinite(max_x) &&
                std::isfinite(min_y) && std::isfinite(max_y) &&
                max_x > min_x && max_y > min_y) {
                out_rect = SDL_FRect{min_x, min_y, max_x - min_x, max_y - min_y};
                return true;
            }
        }

        SDL_FPoint base_screen{};
        SDL_FPoint world_point{
            static_cast<float>(obj.screen_rect.x),
            static_cast<float>(obj.screen_rect.y)
        };
        if (!cam.project_world_point(world_point, object_world_z, base_screen)) {
            return false;
        }
        if (!std::isfinite(base_screen.x) || !std::isfinite(base_screen.y)) {
            return false;
        }

        const float half_width = static_cast<float>(obj.screen_rect.w) * 0.5f;
        const float height = static_cast<float>(obj.screen_rect.h);
        if (!std::isfinite(half_width) || !std::isfinite(height)) {
            return false;
        }

        out_rect = SDL_FRect{
            base_screen.x - half_width,
            base_screen.y - height,
            static_cast<float>(obj.screen_rect.w),
            static_cast<float>(obj.screen_rect.h)
        };
        return out_rect.w > 0.0f && out_rect.h > 0.0f &&
               std::isfinite(out_rect.x) && std::isfinite(out_rect.y);
    };

    auto render_mask = [&](SDL_Texture* texture, const SDL_FRect& dst_rect, const RenderObject* src_obj) -> bool {
        if (!texture) {
            return false;
        }

        SDL_FRect src_rect{};
        const SDL_FRect* src_ptr = nullptr;
        if (src_obj && src_obj->has_src_rect) {
            src_rect = SDL_FRect{
                static_cast<float>(src_obj->src_rect.x),
                static_cast<float>(src_obj->src_rect.y),
                static_cast<float>(src_obj->src_rect.w),
                static_cast<float>(src_obj->src_rect.h)
            };
            src_ptr = &src_rect;
        }

        Uint8 prev_r = 255, prev_g = 255, prev_b = 255, prev_a = 255;
        SDL_BlendMode prev_blend = SDL_BLENDMODE_BLEND;
        SDL_GetTextureColorMod(texture, &prev_r, &prev_g, &prev_b);
        SDL_GetTextureAlphaMod(texture, &prev_a);
        SDL_GetTextureBlendMode(texture, &prev_blend);

        SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
        SDL_SetTextureColorMod(texture, overlay_color.r, overlay_color.g, overlay_color.b);
        SDL_SetTextureAlphaMod(texture, overlay_color.a);

        bool ok = false;
        if (src_obj && src_obj->has_cached_mesh && !src_obj->mesh_dirty) {
            std::vector<SDL_Vertex> overlay_vertices;
            overlay_vertices.reserve(src_obj->cached_vertices.size());
            for (const auto& vert : src_obj->cached_vertices) {
                SDL_Vertex copy = vert;
                copy.color = SDL_FColor{1.0f, 1.0f, 1.0f, 1.0f};
                overlay_vertices.push_back(copy);
            }
            ok = (SDL_RenderGeometry(renderer,
                                     texture,
                                     overlay_vertices.data(),
                                     static_cast<int>(overlay_vertices.size()),
                                     src_obj->cached_indices.data(),
                                     static_cast<int>(src_obj->cached_indices.size())) == 0);
        }
        if (!ok) {
            ok = SDL_RenderTexture(renderer, texture, src_ptr, &dst_rect) == 0;
        }

        SDL_SetTextureColorMod(texture, prev_r, prev_g, prev_b);
        SDL_SetTextureAlphaMod(texture, prev_a);
        SDL_SetTextureBlendMode(texture, prev_blend);

        return ok;
    };

    bool drew_mask = false;

    SDL_Texture* base_texture = asset->get_current_frame();
    for (const auto& obj : asset->render_package) {
        // Only draw the primary sprite to avoid duplicating depth-cue/background layers.
        if (base_texture) {
            if (obj.texture != base_texture) {
                continue;
            }
        } else {
            // If no base texture, use the first render object only.
            if (&obj != &asset->render_package.front()) {
                continue;
            }
        }

        SDL_FRect rect{};
        if (!project_render_object_rect(obj, rect)) {
            continue;
        }
        if (render_mask(obj.texture, rect, &obj)) {
            drew_mask = true;
        }
        // We only need one overlay draw.
        break;
    }

    if (!drew_mask) {
        SDL_Rect cached_bounds{0, 0, 0, 0};
        bool have_bounds = false;

        auto cache_it = asset_bounds_cache_.find(asset);
        if (cache_it != asset_bounds_cache_.end()) {
            cached_bounds = cache_it->second.bounds;
            have_bounds = cached_bounds.w > 0 && cached_bounds.h > 0;
        }

        int screen_y = 0;
        if (!have_bounds) {
            have_bounds = compute_asset_screen_bounds(cam, asset, cached_bounds, screen_y);
        }

        if (have_bounds) {
            SDL_FRect cached_rect{
                static_cast<float>(cached_bounds.x),
                static_cast<float>(cached_bounds.y),
                static_cast<float>(cached_bounds.w),
                static_cast<float>(cached_bounds.h)
            };

            if (SDL_Texture* tex = asset->get_current_frame()) {
                render_mask(tex, cached_rect, nullptr);
            }
        }
    }
}
