#include "dev_controls.hpp"
#include "utils/sdl_render_conversions.hpp"
#include "utils/sdl_mouse_utils.hpp"
#include "utils/ttf_render_utils.hpp"

#ifdef _WIN32
#    ifndef WIN32_LEAN_AND_MEAN
#        define WIN32_LEAN_AND_MEAN
#        define DEV_CONTROLS_DEFINED_WIN32_LEAN_AND_MEAN
#    endif
#    include <windows.h>
#    include <shellapi.h>
#    ifdef DEV_CONTROLS_DEFINED_WIN32_LEAN_AND_MEAN
#        undef WIN32_LEAN_AND_MEAN
#        undef DEV_CONTROLS_DEFINED_WIN32_LEAN_AND_MEAN
#    endif
#endif

#include <SDL3/SDL.h>
#include <fstream>
#include <sstream>
#include <array>
#include <algorithm>
#include <cmath>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <numeric>
#include <limits>
#include <iterator>
#include <filesystem>
#include <regex>
#include <system_error>
#include <unordered_set>
#include <exception>

#include "devtools/map_editor.hpp"
#include "devtools/room_editor.hpp"
#include "devtools/map_mode_ui.hpp"
#include "devtools/frame_editor_session.hpp"
#include "devtools/dev_color_picker.hpp"
#include "DockManager.hpp"
#include "devtools/dev_footer_bar.hpp"
#include "devtools/camera_ui.hpp"
#include "devtools/animation_frame_import_service.hpp"
#include "devtools/frame_importer.hpp"
#include "devtools/font_cache.hpp"
#include "devtools/sdl_pointer_utils.hpp"
#include "devtools/dev_ui_settings.hpp"
#include "dev_mode_utils.hpp"
#include "asset_paths.hpp"
#include "utils/log.hpp"
#include "utils/frame_stats_recorder.hpp"
#include "assets/asset/asset_info.hpp"
#include "dm_styles.hpp"
#include "draw_utils.hpp"
#include "widgets.hpp"
#include "rendering/render/layer_depth_bins.hpp"
#include "rendering/render/render_depth_policy.hpp"
#include "gameplay/map_generation/map_layers_geometry.hpp"

#include "assets/asset/Asset.hpp"
#include "assets/asset/asset_types.hpp"
#include "assets/asset/asset_utils.hpp"
#include "core/AssetsManager.hpp"
#include "rendering/render/warped_screen_grid.hpp"
#include "gameplay/map_generation/room.hpp"
#include "utils/map_grid_settings.hpp"
#include "utils/area.hpp"
#include "utils/grid.hpp"
#include "utils/input.hpp"
#include "utils/stb_image.h"
#include "utils/stb_image_write.h"
#include "utils/string_utils.hpp"
#include "utils/display_color.hpp"
#include "assets/asset/primary_asset_cache.hpp"
#include "image_cache_generator.hpp"

#include <algorithm>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <tuple>
#include <cctype>
#include <string>
#include <limits>
#include <vector>
#include <optional>
#include <iostream>
#include <random>
#include <nlohmann/json.hpp>
#include <SDL3_ttf/SDL_ttf.h>

using devmode::sdl::event_point;
using devmode::sdl::is_pointer_event;

namespace {

using vibble::strings::to_lower_copy;

std::optional<float> project_depth_guide_screen_y(const WarpedScreenGrid& cam,
                                                  float depth_distance,
                                                  int screen_height) {
    if (!(screen_height > 0) || !std::isfinite(depth_distance)) {
        return std::nullopt;
    }

    const world::CameraProjectionParams projection = cam.projection_params();
    const float depth_axis_sign =
        render_depth::normalize_depth_axis_sign(static_cast<float>(projection.forward_z));
    const float world_z = render_depth::world_z_from_depth_offset(
        std::max(0.0f, depth_distance),
        static_cast<float>(projection.anchor_world_z),
        depth_axis_sign);

    const SDL_FPoint center = cam.get_view_center_f();
    SDL_FPoint screen{};
    if (!cam.project_world_point(SDL_FPoint{center.x, 0.0f}, world_z, screen)) {
        return std::nullopt;
    }

    const float y = cam.warp_floor_screen_y(0.0f, screen.y);
    if (!std::isfinite(y) || y < 0.0f || y >= static_cast<float>(screen_height)) {
        return std::nullopt;
    }
    return y;
}

std::filesystem::path repo_root_path() {
#ifdef PROJECT_ROOT
    return std::filesystem::path(PROJECT_ROOT);
#else
    return std::filesystem::current_path();
#endif
}

void open_path_in_file_explorer(const std::filesystem::path& path) {
    if (path.empty()) {
        return;
    }
#if defined(_WIN32)
    ShellExecuteW(nullptr, L"open", path.wstring().c_str(),  nullptr, nullptr, SW_SHOWDEFAULT);
#elif defined(__APPLE__)
    const std::string command = std::string("open \"") + path.u8string() + "\"";
    std::system(command.c_str());
#else
    const std::string command = std::string("xdg-open \"") + path.u8string() + "\"";
    std::system(command.c_str());
#endif
}

void open_repo_root_in_file_explorer() {
    open_path_in_file_explorer(repo_root_path());
}

class ImportBusyScope {
public:
    ImportBusyScope(DevControls* owner, const std::string& message, bool enabled = true)
        : owner_(owner), enabled_(enabled) {
        if (owner_ && enabled_) {
            owner_->begin_import_busy(message);
        }
    }
    ~ImportBusyScope() {
        if (owner_ && enabled_) {
            owner_->end_import_busy();
        }
    }
    ImportBusyScope(const ImportBusyScope&) = delete;
    ImportBusyScope& operator=(const ImportBusyScope&) = delete;
private:
    DevControls* owner_ = nullptr;
    bool enabled_ = false;
};

constexpr const char* kModeIdRoom = "room";
constexpr int kPopupOutlineThickness = 1;

int grid_world_spacing_for_resolution(int resolution) {
    return std::max(1, vibble::grid::delta(resolution));
}

constexpr const char* kGridOverlayEnabledKey = "dev.grid.overlay.enabled";
constexpr const char* kGridSnapEnabledKey    = "dev.grid.snap.enabled";
constexpr const char* kGridCellSizePxKey     = "dev.grid.cell_size_px";
constexpr const char* kGridOverlayResolutionKey = "dev.grid.overlay.r";
constexpr const char* kMovementDebugEnabledKey = "dev.movement.debug.enabled";
constexpr const char* kAnchorPointDebugEnabledKey = "dev.anchor_points.debug.enabled";
constexpr const char* kImpassFloorDebugEnabledKey = "dev.impass_floor.debug.enabled";
constexpr const char* kDevMapSettingsKey = "dev_map_settings";
constexpr const char* kMapColorKey = "default_floor_color";
constexpr const char* kLegacyMapColorKey = "map_color";

void persist_dev_bool(const char* key, bool value) {
    devmode::ui_settings::save_bool(key, value);
    devmode::ui_settings::flush_if_dirty();
}

void persist_dev_number(const char* key, double value) {
    devmode::ui_settings::save_number(key, value);
    devmode::ui_settings::flush_if_dirty();
}

struct SimpleLabelCacheKey {
    std::string font_path;
    int font_size = 0;
    std::string text;
    SDL_Color color{};

    bool operator==(const SimpleLabelCacheKey& other) const {
        return font_size == other.font_size
            && font_path == other.font_path
            && text == other.text
            && color.r == other.color.r
            && color.g == other.color.g
            && color.b == other.color.b
            && color.a == other.color.a;
    }
};

struct SimpleLabelCacheKeyHash {
    std::size_t operator()(const SimpleLabelCacheKey& key) const noexcept {
        std::size_t h1 = std::hash<std::string>{}(key.font_path);
        std::size_t h2 = std::hash<int>{}(key.font_size);
        std::size_t h3 = std::hash<std::string>{}(key.text);
        std::size_t h4 = (static_cast<std::size_t>(key.color.r) << 24)
            | (static_cast<std::size_t>(key.color.g) << 16)
            | (static_cast<std::size_t>(key.color.b) << 8)
            | static_cast<std::size_t>(key.color.a);
        std::size_t seed = h1 ^ (h2 + 0x9e3779b9 + (h1 << 6) + (h1 >> 2));
        seed ^= (h3 + 0x9e3779b9 + (seed << 6) + (seed >> 2));
        seed ^= (h4 + 0x9e3779b9 + (seed << 6) + (seed >> 2));
        return seed;
    }
};

struct SimpleLabelCacheEntry {
    SDL_Texture* texture = nullptr;
    SDL_Point size{0, 0};
};

class SimpleLabelCache {
public:
    void draw(SDL_Renderer* renderer, const DMLabelStyle& style, const std::string& text, int x, int y) {
        if (!renderer || text.empty()) {
            return;
        }
        if (renderer_ != renderer) {
            clear();
            renderer_ = renderer;
        }

        SimpleLabelCacheKey key{style.font_path, style.font_size, text, style.color};
        auto it = cache_.find(key);
        if (it == cache_.end()) {
            SimpleLabelCacheEntry entry;
            entry.texture = create_texture(renderer, style, text, &entry.size);
            if (!entry.texture) {
                return;
            }
            it = cache_.emplace(std::move(key), entry).first;
        }

        SDL_Rect dst{x, y, it->second.size.x, it->second.size.y};
        sdl_render::Texture(renderer, it->second.texture, nullptr, &dst);
    }

    void clear() {
        for (auto& entry : cache_) {
            if (entry.second.texture) {
                SDL_DestroyTexture(entry.second.texture);
            }
        }
        cache_.clear();
        renderer_ = nullptr;
    }

private:
    SDL_Texture* create_texture(SDL_Renderer* renderer,
                                const DMLabelStyle& style,
                                const std::string& text,
                                SDL_Point* out_size) {
        TTF_Font* font = DMFontCache::instance().get_font(style.font_path, style.font_size);
        if (!font) {
            return nullptr;
        }
        SDL_Surface* surf = ttf_util::RenderTextBlended(font, text.c_str(), style.color);
        if (!surf) {
            return nullptr;
        }
        SDL_Texture* texture = SDL_CreateTextureFromSurface(renderer, surf);
        if (texture) {
            if (out_size) {
                *out_size = SDL_Point{surf->w, surf->h};
            }
        }
        SDL_DestroySurface(surf);
        return texture;
    }

    SDL_Renderer* renderer_ = nullptr;
    std::unordered_map<SimpleLabelCacheKey, SimpleLabelCacheEntry, SimpleLabelCacheKeyHash> cache_;
};

SimpleLabelCache& simple_label_cache() {
    static SimpleLabelCache cache;
    return cache;
}

using DropKind = DevControls::DropContentKind;

bool has_extension_ci(const std::filesystem::path& path, std::string_view ext) {
    std::string a = vibble::strings::to_lower_copy(path.extension().string());
    std::string b = vibble::strings::to_lower_copy(std::string(ext));
    return a == b;
}

bool has_any_extension_ci(const std::filesystem::path& path, std::initializer_list<std::string_view> exts) {
    for (auto ext : exts) {
        if (has_extension_ci(path, ext)) {
            return true;
        }
    }
    return false;
}

bool is_supported_image_file(const std::filesystem::path& path) {
    return devmode::frame_importer::is_supported_image_file(path);
}

std::vector<std::filesystem::path> collect_existing_drop_items(const std::vector<std::filesystem::path>& raw_items) {
    std::vector<std::filesystem::path> items;
    items.reserve(raw_items.size());
    for (const auto& path : raw_items) {
        try {
            if (std::filesystem::exists(path)) {
                items.push_back(path);
            }
        } catch (...) {
        }
    }
    return items;
}

std::vector<std::filesystem::path> normalize_sequence(const std::vector<std::filesystem::path>& files) {
    return devmode::frame_importer::normalize_sequence(files);
}

struct DropValidationResult {
    DropKind kind = DropKind::None;
    std::vector<std::filesystem::path> files;
    std::filesystem::path folder;
    bool valid = false;
    std::string reason;
};

std::string sanitize_asset_name_local(const std::string& name) {
    return devmode::utils::normalize_asset_name(name);
}

std::vector<std::filesystem::path> collect_images_from_directory(const std::filesystem::path& directory) {
    std::vector<std::filesystem::path> images;
    try {
        for (const auto& entry : std::filesystem::directory_iterator(directory)) {
            if (!entry.is_regular_file()) continue;
            if (is_supported_image_file(entry.path())) {
                images.push_back(entry.path());
            }
        }
    } catch (...) {
    }
    return images;
}

std::filesystem::path find_frames_subfolder(const std::filesystem::path& folder) {
    try {
        for (const auto& entry : std::filesystem::directory_iterator(folder)) {
            if (!entry.is_directory()) continue;
            const std::string name = vibble::strings::to_lower_copy(entry.path().filename().string());
            if (name == "frames") {
                return entry.path();
            }
        }
    } catch (...) {
    }
    return {};
}

DropValidationResult validate_dropped_folder(const std::filesystem::path& folder) {
    DropValidationResult result;
    result.kind = DropKind::PngFolder;
    result.folder = folder;

    const std::filesystem::path frames_dir = find_frames_subfolder(folder);
    const bool has_frames_subfolder = !frames_dir.empty();
    const std::filesystem::path image_source_dir = has_frames_subfolder ? frames_dir : folder;
    std::vector<std::filesystem::path> images = collect_images_from_directory(image_source_dir);

    if (images.empty()) {
        const std::string folder_name = folder.filename().string();
        if (has_frames_subfolder) {
            result.reason = "Folder '" + folder_name + "' has a 'frames' folder but it contains no images.";
        } else {
            result.reason = "Folder '" + folder_name + "' has no images and no 'frames' folder.";
        }
        return result;
    }

    result.files = normalize_sequence(images);
    result.valid = true;
    return result;
}

bool is_multi_folder_drop(const std::vector<std::filesystem::path>& raw_items) {
    std::vector<std::filesystem::path> items = collect_existing_drop_items(raw_items);
    if (items.size() < 2) {
        return false;
    }
    for (const auto& item : items) {
        std::error_code ec;
        if (!std::filesystem::is_directory(item, ec) || ec) {
            return false;
        }
    }
    return true;
}

DropValidationResult validate_drop_items(const std::vector<std::filesystem::path>& raw_items) {
    DropValidationResult result;
    if (raw_items.empty()) {
        result.reason = "No items provided";
        return result;
    }

    std::vector<std::filesystem::path> items = collect_existing_drop_items(raw_items);
    if (items.empty()) {
        result.reason = "Dropped items missing";
        return result;
    }

    if (items.size() == 1) {
        const auto& only = items.front();
        std::error_code ec;
        const bool is_dir = std::filesystem::is_directory(only, ec);
        if (is_dir && !ec) {
            return validate_dropped_folder(only);
        }

        if (has_extension_ci(only, ".gif")) {
            result.kind = DropKind::Gif;
            result.files = {only};
            result.valid = true;
            return result;
        }
        if (is_supported_image_file(only)) {
            result.kind = DropKind::SinglePng;
            result.files = {only};
            result.valid = true;
            return result;
        }
        result.reason = "Unsupported file type";
        return result;
    }

    for (const auto& item : items) {
        std::error_code ec;
        if (!std::filesystem::is_regular_file(item, ec) || !is_supported_image_file(item)) {
            result.reason = "All dropped items must be image files";
            return result;
        }
    }
    result.kind = DropKind::MultiImages;
    result.files = normalize_sequence(items);
    result.valid = true;
    return result;
}

std::string suggest_name_from_drop(const DropValidationResult& validation) {
    auto base_from_path = [] (const std::filesystem::path& p) {
        std::string stem = p.stem().string();
        if (stem.empty()) return std::string{};
        return sanitize_asset_name_local(stem);
    };

    if (!validation.files.empty()) {
        if (validation.kind == DropKind::PngFolder && !validation.folder.empty()) {
            auto stem = validation.folder.filename().string();
            auto candidate = sanitize_asset_name_local(stem);
            if (!candidate.empty()) return candidate;
        }
        auto candidate = base_from_path(validation.files.front());
        if (!candidate.empty()) return candidate;
    }
    return std::string{"new_asset"};
}

void draw_simple_label(SDL_Renderer* renderer, const std::string& text, int x, int y) {
    simple_label_cache().draw(renderer, DMStyles::Label(), text, x, y);
}

Uint8 clamp_u8_channel(int value) {
    return static_cast<Uint8>(std::clamp(value, 0, 255));
}

SDL_Color clamp_rgba(SDL_Color color) {
    return SDL_Color{
        clamp_u8_channel(static_cast<int>(color.r)),
        clamp_u8_channel(static_cast<int>(color.g)),
        clamp_u8_channel(static_cast<int>(color.b)),
        clamp_u8_channel(static_cast<int>(color.a)),
    };
}

bool is_trail_room(const Room* room) {
    if (!room || room->type.empty()) {
        return false;
    }
    return to_lower_copy(room->type) == "trail";
}

std::string format_room_header_label(const Room* room) {
    if (!room) {
        return std::string("Boundary");
    }
    const bool trail = is_trail_room(room);
    if (room->room_name.empty()) {
        return trail ? std::string("Trail") : std::string("Room");
    }
    return (trail ? std::string("Trail: ") : std::string("Room: ")) + room->room_name;
}

template <class Modal>
bool consume_modal_event(Modal* modal,
                         const SDL_Event& event,
                         const SDL_Point& pointer,
                         bool pointer_relevant,
                         Input* input) {
    if (!modal || !modal->visible()) {
        return false;
    }
    const bool handled = modal->handle_event(event);
    const bool pointer_inside = pointer_relevant && modal->is_point_inside(pointer.x, pointer.y);
    if (handled && input) {
        if (!pointer_relevant || pointer_inside) {
            input->consumeEvent(event);
        }
    }
    return handled || pointer_inside;
}

std::string normalize_area_name_base(const std::string& raw) {
    if (raw.empty()) {
        return std::string{"area"};
    }

    std::string result;
    result.reserve(raw.size());
    bool last_was_separator = false;
    for (char ch : raw) {
        unsigned char uch = static_cast<unsigned char>(ch);
        if (std::isalnum(uch) != 0) {
            result.push_back(static_cast<char>(std::tolower(uch)));
            last_was_separator = false;
        } else if (ch == '_' || ch == '-' || std::isspace(uch)) {
            if (!last_was_separator && !result.empty()) {
                result.push_back('_');
                last_was_separator = true;
            }
        }
    }

    while (!result.empty() && result.back() == '_') {
        result.pop_back();
    }

    if (result.empty()) {
        return std::string{"area"};
    }

    return result;
}

std::string canonicalize_asset_area_type(std::string raw) {
    auto is_space = [](unsigned char ch) { return std::isspace(ch) != 0; };
    raw.erase(raw.begin(), std::find_if(raw.begin(), raw.end(), [&](unsigned char ch) { return !is_space(ch); }));
    raw.erase(std::find_if(raw.rbegin(), raw.rend(), [&](unsigned char ch) { return !is_space(ch); }).base(), raw.end());
    std::transform(raw.begin(), raw.end(), raw.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return raw;
}

bool is_known_asset_area_type(const std::string& type) {
    static const std::array<const char*, 3> kKnownTypes = {
        "impassable",
        "trigger",
        "spawning"
};
    for (const char* known : kKnownTypes) {
        if (type == known) {
            return true;
        }
    }
    return false;
}

std::string make_unique_asset_area_name(const AssetInfo& info, const std::string& preferred) {
    std::unordered_set<std::string> used_names;
    for (const auto& entry : info.areas) {
        if (!entry.name.empty()) {
            used_names.insert(entry.name);
        }
    }

    std::string base = normalize_area_name_base(preferred);
    if (base.size() < 5 || base.substr(base.size() - 5) != "_area") {
        base += "_area";
    }

    std::string candidate = base;
    int suffix = 1;
    while (used_names.count(candidate) > 0) {
        candidate = base + "_" + std::to_string(suffix++);
    }

    return candidate;
}

}

class RegenerateRoomPopup {
public:
    struct TemplateEntry {
        std::string key;
        std::string label;
    };

    using Callback = std::function<void(const std::string&)>;

    void open(std::vector<TemplateEntry> templates,
              Callback cb,
              int screen_w,
              int screen_h) {
        all_templates_ = std::move(templates);
        callback_ = std::move(cb);
        buttons_.clear();

        if (all_templates_.empty()) {
            visible_ = false;
            return;
        }
        rebuild_button_labels();
        if (button_labels_.empty()) {
            visible_ = false;
            return;
        }
        const int margin = DMSpacing::item_gap();
        const int spacing = DMSpacing::small_gap();
        const int button_height = DMButton::height();
        const int button_width = std::max(220, screen_w / 6);
        rect_.w = button_width + margin * 2;
        const int total_buttons = static_cast<int>(button_labels_.size());
        const int content_height = total_buttons * button_height + std::max(0, total_buttons - 1) * spacing;
        rect_.h = margin * 2 + content_height;
        const int padding = DMSpacing::panel_padding();
        const int max_height = std::max(240, screen_h - padding * 2);
        rect_.h = std::min(rect_.h, max_height);

        const int centered_x = screen_w / 2 - rect_.w / 2;
        const int centered_y = screen_h / 2 - rect_.h / 2;
        const int min_x = padding;
        const int max_x = screen_w - rect_.w - padding;
        const int min_y = padding;
        const int max_y = screen_h - rect_.h - padding;

        if (max_x < min_x) {
            rect_.x = min_x;
        } else {
            rect_.x = std::clamp(centered_x, min_x, max_x);
        }

        if (max_y < min_y) {
            rect_.y = min_y;
        } else {
            rect_.y = std::clamp(centered_y, min_y, max_y);
        }

        buttons_.reserve(button_labels_.size());
        for (const std::string& label : button_labels_) {
            auto btn = std::make_unique<DMButton>(label, &DMStyles::ListButton(), button_width, button_height);
            buttons_.push_back(std::move(btn));
        }
        visible_ = true;
    }

    void close() {
        visible_ = false;
        callback_ = nullptr;
    }

    bool visible() const { return visible_; }

    void update(const Input&) {}

    bool handle_event(const SDL_Event& e) {
        if (!visible_) return false;
        if (e.type == SDL_EVENT_KEY_DOWN && e.key.key == SDLK_ESCAPE) {
            close();
            return true;
        }
        if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN || e.type == SDL_EVENT_MOUSE_BUTTON_UP || e.type == SDL_EVENT_MOUSE_MOTION) {
            SDL_Point p = (e.type == SDL_EVENT_MOUSE_MOTION)
                              ? sdl_mouse_util::MotionPoint(e.motion)
                              : sdl_mouse_util::ButtonPoint(e.button);
            if (!SDL_PointInRect(&p, &rect_)) {
                if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN && e.button.button == SDL_BUTTON_LEFT) {
                    close();
                }
                return false;
            }
        }

        bool used = false;
        const int margin = DMSpacing::item_gap();
        const int spacing = DMSpacing::small_gap();
        const int button_height = DMButton::height();
        SDL_Rect btn_rect{ rect_.x + margin, rect_.y + margin, rect_.w - margin * 2, button_height };
        const int bottom = rect_.y + rect_.h - margin;
        for (size_t i = 0; i < buttons_.size(); ++i) {
            auto& btn = buttons_[i];
            if (!btn) continue;
            btn->set_rect(btn_rect);
            if (btn->handle_event(e)) {
                used = true;
                if (e.type == SDL_EVENT_MOUSE_BUTTON_UP && e.button.button == SDL_BUTTON_LEFT) {
                    on_button_pressed(i);
                }
            }
            btn_rect.y += button_height + spacing;
            if (btn_rect.y + button_height > bottom) {
                break;
            }
        }
        return used;
    }

    void render(SDL_Renderer* renderer) const {
        if (!visible_ || !renderer) return;
        SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
        const SDL_Color bg = DMStyles::PanelBG();
        const SDL_Color highlight = DMStyles::HighlightColor();
        const SDL_Color shadow = DMStyles::ShadowColor();
        dm_draw::DrawBeveledRect( renderer, rect_, DMStyles::CornerRadius(), DMStyles::BevelDepth(), bg, highlight, shadow, false, DMStyles::HighlightIntensity(), DMStyles::ShadowIntensity());
        const SDL_Color border = DMStyles::Border();
        dm_draw::DrawRoundedOutline( renderer, rect_, DMStyles::CornerRadius(), kPopupOutlineThickness, border);
        const int margin = DMSpacing::item_gap();
        const int spacing = DMSpacing::small_gap();
        const int button_height = DMButton::height();
        SDL_Rect btn_rect{ rect_.x + margin, rect_.y + margin, rect_.w - margin * 2, button_height };
        const int bottom = rect_.y + rect_.h - margin;
        for (const auto& btn : buttons_) {
            if (!btn) continue;
            btn->set_rect(btn_rect);
            btn->render(renderer);
            btn_rect.y += button_height + spacing;
            if (btn_rect.y > bottom) {
                break;
            }
        }
    }

    bool is_point_inside(int x, int y) const {
        if (!visible_) return false;
        SDL_Point p{x, y};
        return SDL_PointInRect(&p, &rect_);
    }

private:
    void rebuild_button_labels() {
        button_labels_.clear();
        visible_template_keys_.clear();

        std::vector<const TemplateEntry*> filtered;
        for (const TemplateEntry& entry : all_templates_) {
            if (entry.key.empty()) {
                continue;
            }
            filtered.push_back(&entry);
        }

        std::sort(filtered.begin(), filtered.end(), [](const TemplateEntry* a, const TemplateEntry* b) {
            const std::string a_label = a ? to_lower_copy(a->label) : std::string();
            const std::string b_label = b ? to_lower_copy(b->label) : std::string();
            return a_label < b_label;
        });

        for (const TemplateEntry* entry : filtered) {
            if (!entry) {
                continue;
            }
            button_labels_.push_back(entry->label.empty() ? entry->key : entry->label);
            visible_template_keys_.push_back(entry->key);
        }
    }

    void on_button_pressed(std::size_t index) {
        if (index >= visible_template_keys_.size()) {
            return;
        }
        if (callback_) {
            callback_(visible_template_keys_[index]);
        }
        close();
    }

    bool visible_ = false;
    SDL_Rect rect_{0, 0, 280, 320};
    std::vector<TemplateEntry> all_templates_;
    std::vector<std::string> button_labels_;
    std::vector<std::string> visible_template_keys_;
    std::vector<std::unique_ptr<DMButton>> buttons_;
    Callback callback_;
};

DevControls::DevControls(Assets* owner, int screen_w, int screen_h)
    : assets_(owner),
      screen_w_(screen_w),
      screen_h_(screen_h) {
    const char* ctor_start = "[DevControls] ctor start";
    std::cout << ctor_start << "\n";

    save_coordinator_.set_manifest_store(&manifest_store_);
    save_manager_.set_manifest_store(&manifest_store_);
    save_manager_.set_save_coordinator(&save_coordinator_);
    save_manager_.set_save_status_sink([this](devmode::core::SaveOrchestrator::Status status,
                                             const std::string& message) {
        if (!assets_) {
            return;
        }
        switch (status) {
            case devmode::core::SaveOrchestrator::Status::Saving:
                assets_->show_dev_notice("Saving...", 700u);
                break;
            case devmode::core::SaveOrchestrator::Status::Saved:
                assets_->show_dev_notice(message.empty() ? "Saved" : message, 1200u);
                break;
            case devmode::core::SaveOrchestrator::Status::SaveFailed:
                assets_->show_dev_notice(message.empty() ? "Save failed" : message, 2200u);
                break;
            case devmode::core::SaveOrchestrator::Status::ConflictDetected:
                assets_->show_dev_notice(message.empty() ? "Conflict detected" : message, 2200u);
                break;
        }
    });
    save_coordinator_.set_notice_sink([this](bool success, const std::string& message) {
        if (assets_) {
            assets_->show_dev_notice(message, success ? 1200u : 2000u);
        }
    });

    grid_overlay_enabled_ = devmode::ui_settings::load_bool(kGridOverlayEnabledKey, true);
    snap_to_grid_enabled_ = devmode::ui_settings::load_bool(kGridSnapEnabledKey, true);
    const int saved_overlay_r = static_cast<int>(devmode::ui_settings::load_number(kGridOverlayResolutionKey, -1));
    if (saved_overlay_r >= 0) {
        grid_overlay_resolution_user_override_ = true;
        grid_overlay_resolution_r_ = vibble::grid::clamp_resolution(saved_overlay_r);
    } else {
        grid_overlay_resolution_r_ = 0;
    }
    grid_cell_size_px_ = grid_world_spacing_for_resolution(grid_overlay_resolution_r_);
    movement_debug_enabled_ = devmode::ui_settings::load_bool(kMovementDebugEnabledKey, false);
    anchor_point_debug_enabled_ = devmode::ui_settings::load_bool(kAnchorPointDebugEnabledKey, false);
    impass_floor_debug_enabled_ = devmode::ui_settings::load_bool(kImpassFloorDebugEnabledKey, false);
    if (assets_) {
        assets_->set_impass_floor_debug_enabled(impass_floor_debug_enabled_);
    }
    room_editor_ = std::make_unique<RoomEditor>(assets_, screen_w_, screen_h_);
    if (room_editor_) {
        room_editor_->set_parent_window(parent_window_);
        room_editor_->set_manifest_store(&manifest_store_);
        room_editor_->set_save_coordinator(&save_coordinator_);
        room_editor_->set_save_manager(&save_manager_);

        room_editor_->set_header_visibility_callback([this](bool visible) {
            sliding_headers_hidden_ = visible;
            apply_header_suppression();
        });
        room_editor_->set_boundary_assets_panel_callback([this]() { this->open_boundary_assets_modal(); });
        room_editor_->set_snap_to_grid_enabled(snap_to_grid_enabled_);
    }
    map_editor_ = std::make_unique<MapEditor>(assets_);

    map_editor_->set_label_safe_area_provider([this]() -> SDL_Rect {

        SDL_Rect area{0, 0, screen_w_, screen_h_};

        if (!other_settings_.header_suppressed()) {
            const SDL_Rect header = other_settings_.header_rect();
            if (header.h > 0) {
                const int safe_top = header.y + header.h;
                if (safe_top > area.y && safe_top < area.y + area.h) {
                    area.h = std::max(0, (area.y + area.h) - safe_top);
                    area.y = safe_top;
                }
            }
        }

        if (map_mode_ui_) {
            if (DevFooterBar* fb = map_mode_ui_->get_footer_bar()) {
                if (fb->visible()) {
                    const SDL_Rect fr = fb->rect();
                    const int safe_bottom = fr.y;
                    if (safe_bottom > area.y) {
                        area.h = std::max(0, safe_bottom - area.y);
                    }
                }
            }
        }
        return area;
    });
    map_mode_ui_ = std::make_unique<MapModeUI>(assets_);
    if (map_mode_ui_) {
        map_mode_ui_->set_manifest_store(&manifest_store_);
        map_mode_ui_->set_save_coordinator(&save_coordinator_);
        map_mode_ui_->set_save_manager(&save_manager_);
        map_mode_ui_->set_dirty_callback([this](devmode::core::DevSaveCoordinator::Priority priority) {
            this->mark_map_dirty(priority);
        });
        map_mode_ui_->set_on_saved([this]() {
            if (room_editor_) {
                room_editor_->notify_room_assets_saved();
            }
            if (map_mode_ui_) {
                map_mode_ui_->mark_layers_clean();
            }
        });
        save_manager_.register_saveable({
            "map-session",
            [this]() { return map_dirty_; },
            [this](devmode::core::DevSaveCoordinator::Priority priority) {
                try {
                    if (!assets_) {
                        return false;
                    }
                    // Ensure in-memory room state (including spawn groups) is reflected in the map payload.
                    assets_->snapshot_rooms_to_map_info();
                    const std::string map_id = assets_->map_id();
                    if (map_id.empty()) {
                        std::cerr << "[DevControls] Cannot batch-save map: id empty\n";
                        return false;
                    }
                    nlohmann::json payload = assets_->map_info_json();
                    bool ok = save_manager_.persist_map_entry(
                        map_id, std::move(payload), priority, "Map session",
                        [this]() {
                            map_dirty_ = false;
                            if (assets_) {
                                assets_->clear_map_data_dirty();
                            }
                            if (map_mode_ui_) map_mode_ui_->mark_layers_clean();
                            if (map_mode_ui_) map_mode_ui_->notify_saved();
                            if (room_editor_) room_editor_->notify_room_assets_saved();
                        });
                    return ok;
                } catch (const std::exception& ex) {
                    std::cerr << "[DevControls] Map session save failed: " << ex.what() << "\n";
                    return false;
                } catch (...) {
                    std::cerr << "[DevControls] Map session save failed: unknown error\n";
                    return false;
                }
            },
            devmode::core::SaveManager::Stage::Manifest});

        save_manager_.register_saveable({
            "asset-cache",
            [this]() {
                if (!assets_) {
                    return false;
                }
                for (const auto& entry : assets_->library().all()) {
                    if (entry.second && entry.second->is_dirty()) {
                        return true;
                    }
                }
                return false;
            },
            [this](devmode::core::DevSaveCoordinator::Priority) {
                if (!assets_) {
                    return false;
                }
                bool any_saved = false;
                SDL_Renderer* renderer = assets_->renderer();
                for (const auto& entry : assets_->library().all()) {
                    if (!entry.second) {
                        continue;
                    }
                    if (entry.second->save_self_to_cache_if_dirty(renderer)) {
                        any_saved = true;
                    }
                }
                return any_saved;
            },
            devmode::core::SaveManager::Stage::Cache});
    }
    map_grid_regen_cb_ = [this]() {
        // Boundary regeneration is no longer driven through renderer hooks.
    };
    map_grid_save_cb_ = [this]() {
        this->mark_map_dirty(devmode::core::DevSaveCoordinator::Priority::Debounced);
        return true;
    };
    apply_header_suppression();
    camera_panel_ = std::make_unique<CameraUIPanel>(assets_, 72, 72);
    if (camera_panel_) {
        camera_panel_->set_dirty_callback([this]() {
            this->mark_map_dirty(devmode::core::DevSaveCoordinator::Priority::Debounced);
        });
    }
    if (camera_panel_) {
        camera_panel_->close();
    }
    if (map_editor_) {
        map_editor_->set_ui_blocker([this](int x, int y) { return is_pointer_over_dev_ui(x, y); });
    }
    if (map_mode_ui_) {
        map_mode_ui_->set_footer_always_visible(true);
        map_mode_ui_->set_header_mode(MapModeUI::HeaderMode::Room);
        apply_camera_area_render_flag();
    }
    if (room_editor_ && map_mode_ui_) {
        map_mode_ui_->set_room_editor(room_editor_.get());
        room_editor_->set_shared_footer_bar(map_mode_ui_->get_footer_bar());
        room_editor_->set_map_dirty_callback([this](devmode::core::DevSaveCoordinator::Priority priority) {
            this->mark_map_dirty(priority);
        });
    }

    if (map_mode_ui_) {
        if (auto* footer = map_mode_ui_->get_footer_bar()) {
            footer->set_settings_controls_visible(false);
            footer->set_grid_overlay_enabled(grid_overlay_enabled_, false);
            footer->set_grid_resolution(grid_overlay_resolution_r_, false);
            footer->set_grid_controls_callbacks(
                [this](bool enabled) {
                    apply_bool_setting(OtherSettingsAndControls::kShowGridSettingId, enabled, true);
                },
                [this](int resolution, bool /*from_user*/) {
                    const int clamped = vibble::grid::clamp_resolution(resolution);
                    if (clamped == grid_overlay_resolution_r_) {
                        return;
                    }
                    apply_overlay_grid_resolution(clamped, true, false, true);
                }
            );
            footer->set_movement_debug_enabled(movement_debug_enabled_);
            footer->set_movement_debug_callback([this](bool enabled) {
                apply_bool_setting(OtherSettingsAndControls::kMovementDebugSettingId, enabled, true);
            });
        }
    }
    update_movement_debug_visibility();
    configure_header_button_sets();
    
    other_settings_.initialize();
    other_settings_.set_assets_context(assets_);
    other_settings_.set_state_changed_callback([this]() { refresh_active_asset_filters(); });

    other_settings_.set_enabled(enabled_);
    other_settings_.set_screen_dimensions(screen_w_, screen_h_);
    other_settings_.set_map_info(map_info_json_);
    other_settings_.set_current_room(current_room_);

    other_settings_.set_extra_panel_height(0);
    other_settings_.set_extra_panel_renderer({});
    other_settings_.set_extra_panel_event_handler({});
    other_settings_.set_header_title(format_room_header_label(current_room_));

    rebuild_settings_schema();
    ensure_misc_options_widgets();
    const char* ctor_end = "[DevControls] ctor complete";
    std::cout << ctor_end << "\n";
    AssetInfo::set_manifest_store_provider([this]() -> devmode::core::ManifestStore* {
        return &manifest_store_;
    });

}

DevControls::~DevControls() {
    restore_filter_hidden_assets();
    const bool exit_save_ok = run_exit_save_sequence("shutdown");
    if (!exit_save_ok) {
        std::cerr << "[DevControls] EXIT SAVE FAILURE during shutdown. Pending edits may be lost.\n";
    }
    devmode::ui_settings::flush_if_dirty();
    AssetInfo::set_manifest_store_provider({});
    simple_label_cache().clear();
}

void DevControls::set_parent_window(SDL_Window* window) {
    parent_window_ = window;
    if (room_editor_) {
        room_editor_->set_parent_window(parent_window_);
    }
}

devmode::core::ManifestStore& DevControls::manifest_store() {
    return manifest_store_;
}

const devmode::core::ManifestStore& DevControls::manifest_store() const {
    return manifest_store_;
}

devmode::core::DevSaveCoordinator& DevControls::save_coordinator() {
    return save_coordinator_;
}

const devmode::core::DevSaveCoordinator& DevControls::save_coordinator() const {
    return save_coordinator_;
}

bool DevControls::run_exit_save_sequence(const std::string& reason) {
    if (exit_save_sequence_ran_) {
        std::cout << "[DevControls] Exit save sequence already executed; reusing cached result for reason='"
                  << reason << "' success=" << (exit_save_sequence_ok_ ? "true" : "false") << "\n";
        return exit_save_sequence_ok_;
    }

    exit_save_sequence_ran_ = true;

    std::cout << "[DevControls] Exit save sequence begin (reason='" << reason << "')\n";

    // Force-close editor panels so pending animation-document autosaves are committed
    // before the final save/cache flush.
    if (room_editor_) {
        room_editor_->close_asset_info_editor();
    }

    const bool batch_saved =
        save_manager_.save_dirty_with_reason(devmode::core::DevSaveCoordinator::Priority::Immediate,
                                           devmode::core::SaveOrchestrator::Reason::FocusChange,
                                           reason);

    const bool has_dirty_after = save_manager_.has_dirty_saveables();
    bool cache_rebuild_attempted = false;
    bool cache_rebuild_ok = true;
    bool cache_pending_after = false;

    if (!has_dirty_after && assets_) {
        struct PendingAssetRebuild {
            std::shared_ptr<AssetInfo> info;
            AssetInfo::TextureRebuildBucket pending;
        };

        class ExitRebuildLogger final : public imgcache::ILogger {
        public:
            void info(const std::string& msg) override {
                vibble::log::info("[ExitRebuild] " + msg);
            }
            void warn(const std::string& msg) override {
                vibble::log::warn("[ExitRebuild] " + msg);
            }
            void error(const std::string& msg) override {
                vibble::log::warn("[ExitRebuild] " + msg);
            }
        };

        std::vector<PendingAssetRebuild> pending_by_asset;
        pending_by_asset.reserve(assets_->library().all().size());

        imgcache::GeneratorOptions generator_options;
#if defined(PROJECT_ROOT)
        generator_options.manifest_path = std::filesystem::path(PROJECT_ROOT) / "manifest.json";
#endif
        generator_options.quiet_task_logs = true;
        generator_options.missing_only = false;
        generator_options.force_rebuild = false;

        std::unordered_set<std::string> assets_requiring_bundle_refresh;

        for (const auto& [asset_name, info] : assets_->library().all()) {
            if (asset_name.empty() || !info) {
                continue;
            }

            auto pending = info->consume_pending_texture_rebuild_on_close();
            if (pending.empty()) {
                continue;
            }

            if (pending.bundle_refresh_required) {
                assets_requiring_bundle_refresh.insert(asset_name);
            }

            for (const auto& [animation_name, animation_request] : pending.animations) {
                imgcache::GeneratorOptions::AnimationRebuildRequest request;
                request.asset_name = asset_name;
                request.animation_name = animation_name;
                request.all_frames_variant_mask =
                    static_cast<std::uint8_t>(animation_request.all_frames_variants &
                                              imgcache::kTextureVariantMaskAll);
                for (const auto& [frame_index, frame_mask] : animation_request.frame_variants) {
                    request.frame_variant_masks[frame_index] =
                        static_cast<std::uint8_t>(frame_mask & imgcache::kTextureVariantMaskAll);
                }
                generator_options.explicit_rebuild_requests.push_back(std::move(request));
            }

            pending_by_asset.push_back(PendingAssetRebuild{info, std::move(pending)});
        }

        if (!pending_by_asset.empty()) {
            cache_rebuild_attempted = true;

            auto restore_pending_flags = [&pending_by_asset]() {
                for (auto& entry : pending_by_asset) {
                    if (!entry.info) {
                        continue;
                    }
                    entry.info->merge_pending_texture_rebuild_on_close(entry.pending);
                }
            };

            if (!generator_options.explicit_rebuild_requests.empty()) {
                ExitRebuildLogger logger;
                auto generation_result = imgcache::ImageCacheGenerator::Run(generator_options, logger);
                if (!generation_result.ok) {
                    cache_rebuild_ok = false;
                    cache_pending_after = true;
                    restore_pending_flags();
                } else {
                    assets_requiring_bundle_refresh.insert(generation_result.touched_assets.begin(),
                                                           generation_result.touched_assets.end());
                }
            }

            if (cache_rebuild_ok && !assets_requiring_bundle_refresh.empty()) {
                PrimaryAssetCache bundle_cache(assets_->renderer());
                for (const auto& asset_name : assets_requiring_bundle_refresh) {
                    auto info = assets_->library().get(asset_name);
                    if (!info) {
                        continue;
                    }
                    if (!bundle_cache.save_current(*info)) {
                        cache_rebuild_ok = false;
                        cache_pending_after = true;
                        break;
                    }
                }
            }

            if (!cache_rebuild_ok && !cache_pending_after) {
                cache_pending_after = true;
            }
            if (!cache_rebuild_ok) {
                for (auto& entry : pending_by_asset) {
                    if (!entry.info) {
                        continue;
                    }
                    entry.info->merge_pending_texture_rebuild_on_close(entry.pending);
                }
            }
        }
    }

    exit_save_sequence_ok_ = !has_dirty_after && cache_rebuild_ok && !cache_pending_after;

    if (exit_save_sequence_ok_) {
        std::cout << "[DevControls] Exit save sequence complete (batch_saved="
                  << (batch_saved ? "true" : "false")
                  << ", dirty_remaining=false"
                  << ", cache_rebuild=" << (cache_rebuild_attempted ? "ran" : "not-needed")
                  << ")\n";
    } else {
        if (has_dirty_after) {
            std::cerr << "[DevControls] EXIT SAVE FAILURE: dirty saveables remain after exit flush (reason='"
                      << reason << "').\n";
        } else if (!cache_rebuild_ok) {
            std::cerr << "[DevControls] EXIT SAVE FAILURE: pending asset cache rebuild execution failed"
                      << " (reason='" << reason << "').\n";
        } else if (cache_pending_after) {
            std::cerr << "[DevControls] EXIT SAVE FAILURE: pending asset cache rebuild entries remain after exit flush"
                      << " (reason='" << reason << "').\n";
        } else {
            std::cerr << "[DevControls] EXIT SAVE FAILURE (reason='" << reason << "').\n";
        }
    }

    return exit_save_sequence_ok_;
}

void DevControls::set_input(Input* input) {
    input_ = input;
    if (room_editor_) room_editor_->set_input(input);
    if (map_editor_) map_editor_->set_input(input);
}

void DevControls::set_map_info(nlohmann::json* map_info) {
    map_info_json_ = map_info;
    if (map_mode_ui_) {
        map_mode_ui_->set_map_context(map_info_json_, map_path_);
    }
    other_settings_.set_map_info(map_info_json_);

    sync_misc_options_from_map_info();
    if (!map_info_json_) {
        apply_overlay_grid_resolution(grid_overlay_resolution_r_, false, false, true);
    }
    configure_header_button_sets();

    mark_layout_dirty();
}

void DevControls::rebuild_settings_schema() {
    using SettingSchema = OtherSettingsAndControls::SettingSchema;
    using SettingGroup = OtherSettingsAndControls::SettingGroup;
    using SettingControl = OtherSettingsAndControls::SettingControl;

    global_settings_schema_.clear();

    global_settings_schema_.push_back(SettingSchema{
        OtherSettingsAndControls::kShowGridSettingId,
        "Show Grid",
        SettingGroup::Grid,
        SettingControl::Toggle,
        0,
        0,
        [this]() { return grid_overlay_enabled_; },
        [this](bool enabled) {
            sync_grid_overlay_enabled(enabled, true);
        },
        {},
        {},
    });

    global_settings_schema_.push_back(SettingSchema{
        OtherSettingsAndControls::kSnapToGridSettingId,
        "Grid Snapping",
        SettingGroup::Grid,
        SettingControl::Toggle,
        0,
        0,
        [this]() { return snap_to_grid_enabled_; },
        [this](bool enabled) {
            snap_to_grid_enabled_ = enabled;
            persist_dev_bool(kGridSnapEnabledKey, enabled);
            if (room_editor_) {
                room_editor_->set_snap_to_grid_enabled(enabled);
                room_editor_->refresh_cursor_snap();
            }
        },
        {},
        {},
    });

    global_settings_schema_.push_back(SettingSchema{
        OtherSettingsAndControls::kMovementDebugSettingId,
        "Debug Movement",
        SettingGroup::Debug,
        SettingControl::Toggle,
        0,
        0,
        [this]() { return movement_debug_enabled_; },
        [this](bool enabled) {
            movement_debug_enabled_ = enabled;
            persist_dev_bool(kMovementDebugEnabledKey, enabled);
            if (map_mode_ui_) {
                if (auto* footer = map_mode_ui_->get_footer_bar()) {
                    footer->set_movement_debug_enabled(enabled);
                }
            }
            update_movement_debug_visibility();
        },
        {},
        {},
    });

    global_settings_schema_.push_back(SettingSchema{
        OtherSettingsAndControls::kAnchorPointDebugSettingId,
        "Debug Anchor Points",
        SettingGroup::Debug,
        SettingControl::Toggle,
        0,
        0,
        [this]() { return anchor_point_debug_enabled_; },
        [this](bool enabled) {
            anchor_point_debug_enabled_ = enabled;
            persist_dev_bool(kAnchorPointDebugEnabledKey, enabled);
        },
        {},
        {},
    });
    global_settings_schema_.push_back(SettingSchema{
        OtherSettingsAndControls::kImpassFloorDebugSettingId,
        "Debug Impass Floor",
        SettingGroup::Debug,
        SettingControl::Toggle,
        0,
        0,
        [this]() { return impass_floor_debug_enabled_; },
        [this](bool enabled) {
            impass_floor_debug_enabled_ = enabled;
            persist_dev_bool(kImpassFloorDebugEnabledKey, enabled);
            if (assets_) {
                assets_->set_impass_floor_debug_enabled(enabled);
            }
        },
        {},
        {},
    });

    other_settings_.set_settings_schema(global_settings_schema_);
    other_settings_.refresh_setting_values();
}

void DevControls::sync_grid_overlay_enabled(bool enabled, bool update_footer) {
    grid_overlay_enabled_ = enabled;
    persist_dev_bool(kGridOverlayEnabledKey, enabled);
    if (update_footer && map_mode_ui_) {
        if (auto* footer = map_mode_ui_->get_footer_bar()) {
            footer->set_grid_overlay_enabled(enabled, false);
        }
    }
}

void DevControls::apply_bool_setting(const char* id, bool value, bool sync_other_settings) {
    for (auto& setting : global_settings_schema_) {
        if (setting.id == id && setting.control == OtherSettingsAndControls::SettingControl::Toggle) {
            if (setting.bool_setter) {
                setting.bool_setter(value);
            }
            break;
        }
    }
    if (sync_other_settings) {
        other_settings_.set_setting_value(id, value);
    }
}

void DevControls::apply_int_setting(const char* id, int value, bool sync_other_settings) {
    for (auto& setting : global_settings_schema_) {
        if (setting.id == id && setting.control == OtherSettingsAndControls::SettingControl::Stepper) {
            if (setting.int_setter) {
                setting.int_setter(value);
            }
            break;
        }
    }
    if (sync_other_settings) {
        other_settings_.set_setting_value(id, value);
    }
}

void DevControls::apply_overlay_grid_resolution(int resolution, bool user_override, bool update_stepper, bool update_footer) {
    (void)update_stepper;
    const int clamped = vibble::grid::clamp_resolution(resolution);
    grid_overlay_resolution_r_ = clamped;
    grid_cell_size_px_ = grid_world_spacing_for_resolution(clamped);
    if (user_override) {
        grid_overlay_resolution_user_override_ = true;
        persist_dev_number(kGridOverlayResolutionKey, clamped);
        persist_dev_number(kGridCellSizePxKey, grid_cell_size_px_);
    }
    if (update_footer && map_mode_ui_) {
        if (auto* footer = map_mode_ui_->get_footer_bar()) {
            if (footer->grid_resolution() != clamped) {
                footer->set_grid_resolution(clamped, false);
            }
        }
    }
    if (frame_editor_session_ && frame_editor_session_->is_active()) {
        frame_editor_session_->set_snap_resolution(clamped);
    }
    if (room_editor_) {
        room_editor_->set_overlay_snap_resolution(clamped);
        room_editor_->refresh_cursor_snap();
        if (snap_to_grid_enabled_) {
            room_editor_->resnap_spawn_groups_to_overlay_resolution(clamped);
        }
    }
}

void DevControls::nudge_overlay_grid_resolution(int delta) {
    if (delta == 0) {
        return;
    }
    const int target = vibble::grid::clamp_resolution(grid_overlay_resolution_r_ + delta);
    const bool changed = (target != grid_overlay_resolution_r_);
    apply_overlay_grid_resolution(target, changed, true, true);
    push_grid_resolution_toast(target);
}

void DevControls::push_grid_resolution_toast(int resolution) {
    GridResolutionToast toast;
    toast.text = "Grid snap/overlay r: " + std::to_string(resolution);
    toast.start_ms = SDL_GetTicks();
    toast.duration_ms = 1800;
    grid_resolution_toast_ = std::move(toast);
}

void DevControls::apply_grid_resolution_change(int resolution) {
    const int clamped = vibble::grid::clamp_resolution(resolution);
    const bool changed = (clamped != grid_resolution_r_);
    grid_resolution_r_ = clamped;
    if (!map_info_json_) {
        return;
    }
    write_map_tile_size(clamped);
    nlohmann::json& section = (*map_info_json_)["map_grid_settings"];
    MapGridSettings settings = MapGridSettings::from_json(&section);
    if (assets_) {
        assets_->apply_map_grid_settings(settings, false);
    }
    if (map_grid_save_cb_) {
        map_grid_save_cb_();
    } else {
        mark_map_dirty(devmode::core::DevSaveCoordinator::Priority::Debounced);
    }
    (void)changed;
}

void DevControls::ensure_misc_options_widgets() {
    if (!misc_tile_size_stepper_) {
        misc_tile_size_stepper_ = std::make_unique<DMNumericStepper>(
            "Tile Size", 0, vibble::grid::kMaxResolution, 8);
        misc_tile_size_stepper_->set_on_change([this](int value) {
            if (misc_options_panel_suppress_callbacks_) {
                return;
            }
            apply_grid_resolution_change(value);
        });
    }

    if (!misc_map_color_button_) {
        misc_map_color_button_ = std::make_unique<DMButton>("Pick Floor Color", &DMStyles::AccentButton(), 0, DMButton::height());
    }
    if (!color_picker_) {
        color_picker_ = std::make_unique<DevColorPicker>();
        color_picker_->set_screen_size(screen_w_, screen_h_);
    }
}

int DevControls::read_map_tile_size_or_default8() const {
    if (!map_info_json_ || !map_info_json_->is_object()) {
        return 8;
    }
    auto grid_it = map_info_json_->find("map_grid_settings");
    if (grid_it == map_info_json_->end() || !grid_it->is_object()) {
        return 8;
    }
    const nlohmann::json& grid = *grid_it;
    auto resolution_it = grid.find("grid_resolution");
    if (resolution_it == grid.end() || !resolution_it->is_number_integer()) {
        return 8;
    }
    return vibble::grid::clamp_resolution(resolution_it->get<int>());
}

void DevControls::write_map_tile_size(int resolution) {
    if (!map_info_json_) {
        return;
    }
    ensure_map_grid_settings(*map_info_json_);
    nlohmann::json& section = (*map_info_json_)["map_grid_settings"];
    MapGridSettings settings = MapGridSettings::from_json(&section);
    settings.grid_resolution = vibble::grid::clamp_resolution(resolution);
    settings.apply_to_json(section);
}

SDL_Color DevControls::read_map_color_or_default() const {
    SDL_Color color{0, 0, 0, 255};
    if (!map_info_json_ || !map_info_json_->is_object()) {
        return color;
    }
    auto dev_it = map_info_json_->find(kDevMapSettingsKey);
    if (dev_it == map_info_json_->end() || !dev_it->is_object()) {
        return color;
    }
    auto color_it = dev_it->find(kMapColorKey);
    if (color_it == dev_it->end()) {
        color_it = dev_it->find(kLegacyMapColorKey);
    }
    if (color_it == dev_it->end() || !color_it->is_array() || (color_it->size() != 3 && color_it->size() != 4)) {
        return color;
    }

    std::array<int, 4> channels{0, 0, 0, 255};
    const std::size_t count = std::min<std::size_t>(4, color_it->size());
    for (std::size_t i = 0; i < count; ++i) {
        const nlohmann::json& value = (*color_it)[i];
        if (!value.is_number()) {
            return color;
        }
        channels[i] = static_cast<int>(std::lround(value.get<double>()));
    }
    color.r = clamp_u8_channel(channels[0]);
    color.g = clamp_u8_channel(channels[1]);
    color.b = clamp_u8_channel(channels[2]);
    color.a = clamp_u8_channel(channels[3]);
    return color;
}

void DevControls::write_map_color(SDL_Color color) {
    if (!map_info_json_) {
        return;
    }
    if (!map_info_json_->is_object()) {
        *map_info_json_ = nlohmann::json::object();
    }
    nlohmann::json& dev_settings = (*map_info_json_)[kDevMapSettingsKey];
    if (!dev_settings.is_object()) {
        dev_settings = nlohmann::json::object();
    }
    const SDL_Color clamped = clamp_rgba(color);
    const int alpha = 255;
    dev_settings[kMapColorKey] = nlohmann::json::array({
        static_cast<int>(clamped.r),
        static_cast<int>(clamped.g),
        static_cast<int>(clamped.b),
        alpha,
    });
}

void DevControls::sync_misc_options_from_map_info() {
    ensure_misc_options_widgets();
    if (!map_info_json_) {
        grid_resolution_r_ = 8;
        misc_map_color_ = SDL_Color{0, 0, 0, 255};
        misc_map_color_saved_ = misc_map_color_;
        misc_map_color_dirty_ = false;
        misc_options_panel_suppress_callbacks_ = true;
        if (misc_tile_size_stepper_) {
            misc_tile_size_stepper_->set_value(grid_resolution_r_);
        }
        misc_options_panel_suppress_callbacks_ = false;
        return;
    }

    const int tile_size = read_map_tile_size_or_default8();
    write_map_tile_size(tile_size);
    grid_resolution_r_ = tile_size;
    // Overlay resolution is owned by user dev setting (Ctrl+Up/Down path).
    // Never sync overlay resolution from map tile/grid resolution.
    apply_overlay_grid_resolution(grid_overlay_resolution_r_, false, false, true);
    if (assets_) {
        MapGridSettings settings = MapGridSettings::from_json(&(*map_info_json_)["map_grid_settings"]);
        assets_->apply_map_grid_settings(settings, false);
    }

    misc_map_color_ = read_map_color_or_default();
    misc_map_color_saved_ = misc_map_color_;
    misc_map_color_dirty_ = false;

    misc_options_panel_suppress_callbacks_ = true;
    if (misc_tile_size_stepper_) {
        misc_tile_size_stepper_->set_value(tile_size);
    }
    misc_options_panel_suppress_callbacks_ = false;
}

void DevControls::layout_misc_options_panel() {
    if (!misc_options_panel_open_) {
        misc_options_panel_rect_ = SDL_Rect{0, 0, 0, 0};
        return;
    }
    ensure_misc_options_widgets();

    const int panel_w = 320;
    const int padding = DMSpacing::panel_padding();
    const int gap = DMSpacing::item_gap();
    const int content_w = std::max(0, panel_w - padding * 2);
    const int label_h = DMStyles::Label().font_size;
    const int tile_h = misc_tile_size_stepper_ ? misc_tile_size_stepper_->preferred_height(content_w) : DMNumericStepper::height();
    const int color_h = DMButton::height();
    const int preview_h = 26;
    const int panel_h = padding + label_h + gap + tile_h + gap + label_h + gap + color_h + gap + preview_h + padding;

    int footer_top = screen_h_;
    if (map_mode_ui_) {
        if (auto* footer = map_mode_ui_->get_footer_bar()) {
            if (footer->visible()) {
                SDL_Rect footer_rect = footer->rect();
                footer_top = footer_rect.y;
            }
        }
    }

    const int x = std::max(0, screen_w_ - panel_w - 16);
    const int y = std::max(0, footer_top - panel_h - 10);
    misc_options_panel_rect_ = SDL_Rect{x, y, panel_w, panel_h};

    int cursor_y = y + padding + label_h + gap;
    if (misc_tile_size_stepper_) {
        misc_tile_size_stepper_->set_rect(SDL_Rect{x + padding, cursor_y, content_w, tile_h});
        cursor_y += tile_h + gap;
    }
    cursor_y += label_h + gap;
    if (misc_map_color_button_) {
        misc_map_color_button_->set_rect(SDL_Rect{x + padding, cursor_y, content_w, color_h});
    }
}

bool DevControls::handle_misc_options_panel_event(const SDL_Event& event) {
    if (!misc_options_panel_open_) {
        return false;
    }
    layout_misc_options_panel();

    bool used = false;
    if (misc_tile_size_stepper_ && misc_tile_size_stepper_->handle_event(event)) {
        used = true;
    }
    if (misc_map_color_button_ && misc_map_color_button_->handle_event(event)) {
        used = true;
        if (event.type == SDL_EVENT_MOUSE_BUTTON_UP && event.button.button == SDL_BUTTON_LEFT && color_picker_) {
            color_picker_->set_screen_size(screen_w_, screen_h_);
            color_picker_->open(
                misc_map_color_,
                [this](SDL_Color chosen) {
                    chosen.a = 255;
                    misc_map_color_ = chosen;
                    misc_map_color_dirty_ =
                        (misc_map_color_.r != misc_map_color_saved_.r) ||
                        (misc_map_color_.g != misc_map_color_saved_.g) ||
                        (misc_map_color_.b != misc_map_color_saved_.b);
                });
        }
    }
    if (color_picker_ && color_picker_->is_open()) {
        used = color_picker_->handle_event(event) || used;
    }

    if (!used && is_pointer_event(event)) {
        const SDL_Point p = event_point(event);
        if (SDL_PointInRect(&p, &misc_options_panel_rect_)) {
            used = true;
        }
    }
    return used;
}

void DevControls::render_misc_options_panel(SDL_Renderer* renderer) {
    if (!renderer || !misc_options_panel_open_) {
        return;
    }
    layout_misc_options_panel();
    if (misc_options_panel_rect_.w <= 0 || misc_options_panel_rect_.h <= 0) {
        return;
    }

    const SDL_Color panel_bg = DMStyles::PanelBG();
    const SDL_Color panel_border = DMStyles::Border();
    const SDL_Color panel_header = DMStyles::PanelHeader();
    SDL_SetRenderDrawColor(renderer, panel_bg.r, panel_bg.g, panel_bg.b, panel_bg.a);
    sdl_render::FillRect(renderer, &misc_options_panel_rect_);
    SDL_SetRenderDrawColor(renderer, panel_border.r, panel_border.g, panel_border.b, panel_border.a);
    sdl_render::Rect(renderer, &misc_options_panel_rect_);

    SDL_Rect header_rect = misc_options_panel_rect_;
    header_rect.h = DMStyles::Label().font_size + DMSpacing::panel_padding();
    SDL_SetRenderDrawColor(renderer, panel_header.r, panel_header.g, panel_header.b, panel_header.a);
    sdl_render::FillRect(renderer, &header_rect);
    draw_simple_label(renderer,
                      "Misc Options",
                      misc_options_panel_rect_.x + DMSpacing::panel_padding(),
                      misc_options_panel_rect_.y + (DMSpacing::panel_padding() / 2));

    if (misc_tile_size_stepper_) {
        misc_tile_size_stepper_->render(renderer);
    }
    draw_simple_label(renderer,
                      "Default Floor Color",
                      misc_options_panel_rect_.x + DMSpacing::panel_padding(),
                      (misc_tile_size_stepper_ ? misc_tile_size_stepper_->rect().y + misc_tile_size_stepper_->rect().h + DMSpacing::item_gap()
                                               : misc_options_panel_rect_.y + 64));
    if (misc_map_color_button_) {
        misc_map_color_button_->render(renderer);
    }

    const int preview_w = std::max(0, misc_options_panel_rect_.w - DMSpacing::panel_padding() * 2);
    const int preview_h = 22;
    SDL_Rect preview_rect{
        misc_options_panel_rect_.x + DMSpacing::panel_padding(),
        misc_options_panel_rect_.y + misc_options_panel_rect_.h - preview_h - DMSpacing::panel_padding(),
        preview_w,
        preview_h,
    };
    SDL_SetRenderDrawColor(renderer, misc_map_color_.r, misc_map_color_.g, misc_map_color_.b, misc_map_color_.a);
    sdl_render::FillRect(renderer, &preview_rect);
    SDL_SetRenderDrawColor(renderer, panel_border.r, panel_border.g, panel_border.b, panel_border.a);
    sdl_render::Rect(renderer, &preview_rect);

    if (color_picker_ && color_picker_->is_open()) {
        color_picker_->render(renderer);
    }
}

void DevControls::open_misc_options_panel() {
    if (misc_options_panel_open_) {
        return;
    }
    ensure_misc_options_widgets();
    sync_misc_options_from_map_info();
    misc_options_panel_open_ = true;
    mark_layout_dirty();
    sync_header_button_states();
}

void DevControls::close_misc_options_panel() {
    if (!misc_options_panel_open_) {
        return;
    }
    if (misc_map_color_dirty_) {
        write_map_color(misc_map_color_);
        mark_map_dirty(devmode::core::DevSaveCoordinator::Priority::Debounced);
        misc_map_color_saved_ = misc_map_color_;
        misc_map_color_dirty_ = false;
    }
    if (color_picker_ && color_picker_->is_open()) {
        color_picker_->close(false);
    }
    misc_options_panel_open_ = false;
    misc_options_panel_rect_ = SDL_Rect{0, 0, 0, 0};
    mark_layout_dirty();
    sync_header_button_states();
}

void DevControls::toggle_misc_options_panel() {
    if (misc_options_panel_open_) {
        close_misc_options_panel();
    } else {
        open_misc_options_panel();
    }
}

bool DevControls::is_misc_options_panel_open() const {
    return misc_options_panel_open_;
}

bool DevControls::layout_dirty() const {
    return !layout_cache_.valid || has_dirty(kDirtyLayout);
}

void DevControls::set_player(Asset* player) {
    player_ = player;
    if (room_editor_) room_editor_->set_player(player);
}

void DevControls::set_active_assets(std::vector<Asset*>& actives, std::uint64_t version) {
    const bool assets_changed = (active_assets_ != &actives) || (active_assets_version_ != version);
    if (!assets_changed) {
        return;
    }
    active_assets_ = &actives;
    active_assets_version_ = version;
    if (room_editor_) {
        room_editor_->set_active_assets(actives, version);
    }

    mark_layout_dirty();
}

void DevControls::set_screen_dimensions(int width, int height) {
    screen_w_ = width;
    screen_h_ = height;

    if (room_editor_) room_editor_->set_screen_dimensions(width, height);
    if (map_editor_) map_editor_->set_screen_dimensions(width, height);
    if (map_mode_ui_) map_mode_ui_->set_screen_dimensions(width, height);
    if (color_picker_) color_picker_->set_screen_size(width, height);

    SDL_Rect bounds{0, 0, screen_w_, screen_h_};
    if (camera_panel_) camera_panel_->set_work_area(bounds);
    

    other_settings_.set_screen_dimensions(width, height);
    if (boundary_assets_modal_) boundary_assets_modal_->set_screen_dimensions(width, height);

    other_settings_.set_right_accessory_width(0);
    other_settings_.ensure_layout();
    SDL_Rect usable = DockManager::instance().computeUsableRect(
        bounds,
        SDL_Rect{0, 0, 0, 0},
        SDL_Rect{0, 0, 0, 0},
        {});
    if (map_mode_ui_) {
        map_mode_ui_->set_sliding_area_bounds(usable);
    }

    mark_layout_dirty();
}

void DevControls::set_current_room(Room* room, bool force_refresh) {
    if (enabled_ && !force_refresh && dev_selected_room_ && room != dev_selected_room_) {
        room = dev_selected_room_;
    }
    const std::string header_label = format_room_header_label(room);

    if (!force_refresh && current_room_ == room) {
        current_room_ = room;
        dev_selected_room_ = room;
        other_settings_.set_header_title(header_label);
        if (map_mode_ui_) {
            if (auto* footer = map_mode_ui_->get_footer_bar()) {
                if (mode_ == Mode::RoomEditor) {
                    footer->set_title(header_label);
                } else {
                    footer->set_title(std::string("Map"));
                }
            }
        }
        return;
    }
    const bool room_changed = (current_room_ != room);
    {
        std::ostringstream oss;
        oss << "[DevControls] set_current_room begin -> "
            << (room ? room->room_name : std::string("<null>"));
        const std::string msg = oss.str();
        try {
            vibble::log::debug(msg);
        } catch (...) {}
    }
    current_room_ = room;

    dev_selected_room_ = room;
    if (room_changed && assets_) {
        WarpedScreenGrid& cam = assets_->getView();
        if (!cam.is_manual_height_override()) {
            cam.clear_focus_override();
        }
    }
    if (regenerate_popup_) regenerate_popup_->close();
    if (room_editor_) {
        try {
            vibble::log::debug("[DevControls] set_current_room -> room_editor set_current_room");
        } catch (...) {}
        room_editor_->set_current_room(room);
        if (map_editor_) {
            map_editor_->set_current_room(room);
        }
    }
    other_settings_.set_current_room(room);
    other_settings_.set_header_title(header_label);
    if (map_mode_ui_) {
        if (auto* footer = map_mode_ui_->get_footer_bar()) {
            if (mode_ == Mode::RoomEditor) {
                footer->set_title(header_label);
            } else {
                footer->set_title(std::string("Map"));
            }
        }
    }

    mark_layout_dirty();

    try {
        vibble::log::debug("[DevControls] set_current_room complete");
    } catch (...) {}
}

void DevControls::set_rooms(std::vector<Room*>* rooms, std::size_t generation) {
    if (rooms == rooms_ && generation == rooms_generation_) {
        return;
    }

    rooms_ = rooms;
    rooms_generation_ = generation;

    if (rooms_ && assets_) {
        const std::string map_id = assets_->map_id();
        nlohmann::json* map_info = &assets_->map_info_json();
        for (Room* room : *rooms_) {
            if (!room) continue;
            room->set_manifest_store(&manifest_store_, map_id, map_info,
                                     [this](const std::string&, const nlohmann::json&) {
                                         this->mark_map_dirty(devmode::core::DevSaveCoordinator::Priority::Debounced);
                                     });
        }
    }
    if (map_editor_) map_editor_->set_rooms(rooms);
}

void DevControls::set_camera_override_for_testing(WarpedScreenGrid* camera_override) {
    camera_override_for_testing_ = camera_override;
    if (map_editor_) {
        map_editor_->set_camera_override_for_testing(camera_override);
    }
    apply_camera_area_render_flag();
}

void DevControls::set_map_context(nlohmann::json* map_info, const std::string& map_path) {
    map_info_json_ = map_info;
    map_path_ = map_path;
    if (map_mode_ui_) {
        map_mode_ui_->set_map_context(map_info, map_path);
    }
    if (rooms_ && assets_) {
        const std::string map_id = assets_->map_id();
        nlohmann::json* info = &assets_->map_info_json();
        for (Room* room : *rooms_) {
            if (!room) continue;
            room->set_manifest_store(&manifest_store_, map_id, info,
                                     [this](const std::string& id, const nlohmann::json& payload) {
                                         (void)id;
                                         (void)payload;
                                         this->mark_map_dirty(devmode::core::DevSaveCoordinator::Priority::Debounced);
                                     });
        }
    }
    other_settings_.set_map_info(map_info_json_);
    sync_misc_options_from_map_info();
    configure_header_button_sets();

    mark_layout_dirty();
}

bool DevControls::is_pointer_over_dev_ui(int x, int y) const {
    if (camera_panel_ && camera_panel_->is_visible() && camera_panel_->is_point_inside(x, y)) {
        return true;
    }
    if (room_editor_ && room_editor_->is_room_ui_blocking_point(x, y)) {
        return true;
    }
    
    if (map_mode_ui_ && map_mode_ui_->is_point_inside(x, y)) {
        return true;
    }
    if (regenerate_popup_ && regenerate_popup_->visible() && regenerate_popup_->is_point_inside(x, y)) {
        return true;
    }
    if (misc_options_panel_open_) {
        SDL_Point p{x, y};
        if (SDL_PointInRect(&p, &misc_options_panel_rect_)) {
            return true;
        }
    }
    if (!is_modal_blocking_panels() && enabled_ && other_settings_.contains_point(x, y)) {
        return true;
    }
    return false;
}

Room* DevControls::resolve_current_room(Room* detected_room) {
    detected_room_ = detected_room;
    Room* target = dev_selected_room_;
    if (!target && assets_) {
        target = assets_->current_room();
    }
    if (!target) {
        target = choose_room(detected_room_);
    }
    if (!enabled_) {
        dev_selected_room_ = nullptr;
        if (current_room_ != target) {
            set_current_room(target);
        }
        return current_room_;
    }
    dev_selected_room_ = target;
    if (current_room_ != target) {
        set_current_room(target);
    }
    return current_room_;
}

void DevControls::set_enabled(bool enabled) {
    {
        std::ostringstream oss;
        oss << "[DevControls] set_enabled(" << (enabled ? "true" : "false") << ") begin";
        const std::string msg = oss.str();
        std::cout << msg << "\n";
    }
    if (enabled == enabled_) {
        const char* msg = "[DevControls] set_enabled unchanged, exiting";
        std::cout << msg << "\n";
        return;
    }
    enabled_ = enabled;

    other_settings_.set_enabled(enabled_);

    if (enabled_) {
        const bool persisted_grid_overlay = devmode::ui_settings::load_bool(kGridOverlayEnabledKey, grid_overlay_enabled_);
        const bool persisted_snap_to_grid = devmode::ui_settings::load_bool(kGridSnapEnabledKey, snap_to_grid_enabled_);
        const int persisted_overlay_r = static_cast<int>(devmode::ui_settings::load_number(kGridOverlayResolutionKey, -1));

        grid_overlay_enabled_ = persisted_grid_overlay;
        snap_to_grid_enabled_ = persisted_snap_to_grid;
        grid_overlay_resolution_user_override_ = persisted_overlay_r >= 0;
        other_settings_.set_setting_value(OtherSettingsAndControls::kShowGridSettingId, grid_overlay_enabled_);
        other_settings_.set_setting_value(OtherSettingsAndControls::kSnapToGridSettingId, snap_to_grid_enabled_);
        if (grid_overlay_resolution_user_override_) {
            apply_overlay_grid_resolution(vibble::grid::clamp_resolution(persisted_overlay_r), false, true, true);
        } else {
            apply_overlay_grid_resolution(grid_overlay_resolution_r_, false, true, true);
        }

        const char* msg = "[DevControls] preparing enable flow";
        std::cout << msg << "\n";
        struct CameraPoseSnapshot {
            bool valid = false;
            SDL_Point center{0, 0};
            double height_px = 1.0;
            double zoom_percent = 0.0;
            float pitch_deg = camera_math::kDefaultCameraTiltDeg;
            bool has_focus_override = false;
            SDL_Point focus_override{0, 0};
            bool manual_height_override = false;
            bool manual_zoom_override = false;
        };

        WarpedScreenGrid* camera_ptr = assets_ ? &assets_->getView() : nullptr;
        CameraPoseSnapshot preserved_camera{};
        if (camera_ptr) {
            preserved_camera.valid = true;
            preserved_camera.center = camera_ptr->get_screen_center();
            preserved_camera.height_px = std::max(1.0, static_cast<double>(camera_ptr->get_scale()));
            preserved_camera.zoom_percent = camera_ptr->get_zoom_percent();
            preserved_camera.pitch_deg = camera_ptr->current_pitch_degrees();
            preserved_camera.has_focus_override = camera_ptr->has_focus_override();
            if (preserved_camera.has_focus_override) {
                preserved_camera.focus_override = camera_ptr->get_focus_override_point();
            }
            preserved_camera.manual_height_override = camera_ptr->is_manual_height_override();
            preserved_camera.manual_zoom_override = camera_ptr->is_manual_zoom_override();
        }
        const bool camera_was_visible = camera_panel_ && camera_panel_->is_visible();
        close_all_floating_panels();
        set_mode(Mode::RoomEditor);
        Room* target = nullptr;
        if (assets_) {
            target = assets_->current_room();
        }
        if (!target) {
            target = choose_room(current_room_ ? current_room_ : detected_room_);
        }
        dev_selected_room_ = target;
        if (room_editor_) {
            room_editor_->set_enabled(true, true);
            room_editor_->set_snap_to_grid_enabled(snap_to_grid_enabled_);
            room_editor_->refresh_cursor_snap();
        }
        if (map_editor_) map_editor_->set_enabled(false);
        if (camera_panel_) camera_panel_->set_assets(assets_);
        set_current_room(target);
        if (map_mode_ui_) {
            if (auto* footer = map_mode_ui_->get_footer_bar()) {
                footer->set_grid_overlay_enabled(grid_overlay_enabled_, false);
            }
            map_mode_ui_->set_map_mode_active(false);
            map_mode_ui_->set_header_mode(MapModeUI::HeaderMode::Room);
        }
        if (preserved_camera.valid && camera_ptr) {
            camera_ptr->set_scale(preserved_camera.height_px);
            camera_ptr->set_zoom_percent(preserved_camera.zoom_percent);
            camera_ptr->set_tilt_override(preserved_camera.pitch_deg);
            // Always set focus override to preserve the exact camera position when
            // entering dev mode. Without this, update_camera_height() will center
            // the camera on the room on the next frame.
            camera_ptr->set_focus_override(preserved_camera.center);
            camera_ptr->set_screen_center(preserved_camera.center);
            camera_ptr->set_manual_height_override(preserved_camera.manual_height_override);
            camera_ptr->set_manual_zoom_override(preserved_camera.manual_zoom_override);
            // Preserve the exact camera pose on dev entry.
            camera_ptr->set_manual_height_override(true);
            camera_ptr->set_manual_zoom_override(true);
            camera_ptr->update();
        }
        if (camera_was_visible && camera_panel_) {
            camera_panel_->open();
        }
        const char* msg_enable_done = "[DevControls] enable flow complete";
        std::cout << msg_enable_done << "\n";
    } else {
        const char* msg_disable = "[DevControls] preparing disable flow";
        std::cout << msg_disable << "\n";
        if (room_editor_) {
            room_editor_->set_room_trail_nav_visibility(false);
        }
        WarpedScreenGrid* camera_ptr = assets_ ? &assets_->getView() : nullptr;
        close_all_floating_panels();
        if (map_editor_ && map_editor_->is_enabled()) {
            map_editor_->exit(true, false);
        }
        if (map_mode_ui_) {
            map_mode_ui_->set_map_mode_active(false);
            map_mode_ui_->set_header_mode(MapModeUI::HeaderMode::Room);
        }
        set_mode(Mode::RoomEditor);
        dev_selected_room_ = nullptr;
        if (room_editor_) {
            room_editor_->set_enabled(false);
        }
        if (camera_ptr) {
            camera_ptr->set_manual_height_override(false);
            camera_ptr->set_manual_zoom_override(false);
            camera_ptr->clear_focus_override();
            camera_ptr->clear_tilt_override();
            camera_ptr->update();
        }
        close_camera_panel();
        restore_filter_hidden_assets();
        const char* msg_disable_done = "[DevControls] disable flow complete";
        std::cout << msg_disable_done << "\n";
    }

    sync_header_button_states();
    if (enabled_) {
        other_settings_.ensure_layout();
    }
    mark_layout_dirty();
    update_movement_debug_visibility();
    {
        std::ostringstream oss;
        oss << "[DevControls] set_enabled(" << (enabled ? "true" : "false") << ") done";
        const std::string msg = oss.str();
        std::cout << msg << "\n";
    }
}

void DevControls::sync_camera_tilt_override() {
    if (!assets_) {
        return;
    }

    WarpedScreenGrid& cam = assets_->getView();
    if (!enabled_) {
        cam.clear_tilt_override();
        return;
    }

    if (frame_editor_session_ && frame_editor_session_->is_active()) {
        return;
    }

    const bool camera_settings_open = room_editor_ && room_editor_->is_camera_settings_open();
    if (camera_settings_open) {
        cam.clear_tilt_override();
        return;
    }

    if (cam.has_tilt_override()) {
        return;
    }

    cam.set_tilt_override(45.0f);
}

void DevControls::update(const Input& input) {
    save_coordinator_.begin_frame();
    if (!enabled_) return;
    auto& frame_stats = runtime_stats::FrameStatsRecorder::instance();
    auto elapsed_ms = [](Uint64 begin, Uint64 end) -> double {
        return runtime_stats::FrameStatsRecorder::elapsed_ms(begin, end);
    };
    if (map_info_dirty_ && assets_) {
        other_settings_.set_map_info(&assets_->map_info_json());
        map_info_dirty_ = false;
    }
    const bool ctrl = input.isScancodeDown(SDL_SCANCODE_LCTRL) || input.isScancodeDown(SDL_SCANCODE_RCTRL);
    const bool shift_down = input.isScancodeDown(SDL_SCANCODE_LSHIFT) || input.isScancodeDown(SDL_SCANCODE_RSHIFT);
    const bool shift_blocks_headers = shift_down && mode_ == Mode::RoomEditor;
    if (shift_block_headers_footers_ != shift_blocks_headers) {
        shift_block_headers_footers_ = shift_blocks_headers;
        apply_header_suppression();
        mark_layout_dirty();
    }
    if (ctrl) {
        if (input.wasScancodePressed(SDL_SCANCODE_UP)) {
            nudge_overlay_grid_resolution(1);
        } else if (input.wasScancodePressed(SDL_SCANCODE_DOWN)) {
            nudge_overlay_grid_resolution(-1);
        }
    }
    if (ctrl && input.wasScancodePressed(SDL_SCANCODE_C)) {
        const bool room_editor_active =
            mode_ == Mode::RoomEditor && room_editor_ && room_editor_->is_enabled();
        if (!room_editor_active) {
            const bool was_visible = camera_panel_ && camera_panel_->is_visible();
            toggle_camera_panel();
            const bool now_visible = camera_panel_ && camera_panel_->is_visible();
            if (assets_ && was_visible != now_visible) {
                assets_->show_dev_notice(now_visible ? "Camera panel opened" : "Camera panel closed", 1400);
            }
        }
    }

    pointer_over_camera_panel_ =
        camera_panel_ && camera_panel_->is_visible() && camera_panel_->is_point_inside(input.getX(), input.getY());
    if (!(camera_panel_ && camera_panel_->is_visible() && camera_panel_->is_debug_section_expanded())) {
        depth_guide_selection_ = DepthGuideSelection::None;
        depth_guide_drag_active_ = false;
        depth_guide_preview_active_ = false;
        depth_guide_blue_wheel_last_change_ms_ = 0;
    }
    if (map_mode_ui_ && input.wasScancodePressed(SDL_SCANCODE_F8)) {
        close_misc_options_panel();
        const bool was_visible = map_mode_ui_->is_layers_panel_visible();
        map_mode_ui_->toggle_layers_panel();
        const bool now_visible = map_mode_ui_->is_layers_panel_visible();
        if (assets_ && was_visible != now_visible) {
            assets_->show_dev_notice(now_visible ? "Layers panel opened" : "Layers panel closed", 1400);
        }
    }
    if (room_editor_) {
        const bool room_editor_active =
            mode_ == Mode::RoomEditor && room_editor_->is_enabled();
        const bool camera_open = camera_panel_ && camera_panel_->is_visible();
        const bool layers_open = map_mode_ui_ && map_mode_ui_->is_layers_panel_visible();
        const bool map_ui_panels_open = map_mode_ui_ && map_mode_ui_->is_any_panel_visible();
        const bool boundary_open = boundary_assets_modal_ && boundary_assets_modal_->visible();
        const bool regenerate_open = regenerate_popup_ && regenerate_popup_->visible();
        const bool external_selection_blocked =
            !room_editor_active ||
            camera_open ||
            layers_open ||
            misc_options_panel_open_ ||
            boundary_open ||
            regenerate_open ||
            map_ui_panels_open ||
            is_modal_blocking_panels();
        room_editor_->set_external_asset_selection_blocked(external_selection_blocked);
    }
    if (room_editor_ && room_editor_->is_enabled()) {

        const bool frame_editing = frame_editor_session_ && frame_editor_session_->is_active();
        if (!frame_editing) {
            const bool camera_panel_blocking =
                camera_panel_ && camera_panel_->is_visible() && pointer_over_camera_panel_;
            const bool depth_guide_blocking = depth_guide_drag_active_ || depth_guide_selection_ != DepthGuideSelection::None;
            if (!camera_panel_blocking && !depth_guide_blocking) {
                const Uint64 room_update_begin = SDL_GetPerformanceCounter();
                room_editor_->update(input);
                frame_stats.set("dev.room_editor_update_ms",
                                elapsed_ms(room_update_begin, SDL_GetPerformanceCounter()));
            }
        } else {
            room_editor_->clear_highlighted_assets();
        }
    }

    if (camera_panel_ && camera_panel_->is_visible()) {
        const Uint64 camera_panel_begin = SDL_GetPerformanceCounter();
        camera_panel_->update(input, screen_w_, screen_h_);
        frame_stats.set("dev.camera_panel_ms",
                        elapsed_ms(camera_panel_begin, SDL_GetPerformanceCounter()));
    }
    if (regenerate_popup_ && regenerate_popup_->visible()) {
        regenerate_popup_->update(input);
    }
    other_settings_.set_enabled(enabled_);
    apply_header_suppression();
    const Uint64 other_settings_begin = SDL_GetPerformanceCounter();
    other_settings_.update(input);
    frame_stats.set("dev.other_settings_ms",
                    elapsed_ms(other_settings_begin, SDL_GetPerformanceCounter()));
    if (map_mode_ui_) {
        const Uint64 map_mode_begin = SDL_GetPerformanceCounter();
        map_mode_ui_->update(input);
        frame_stats.set("dev.map_mode_ui_ms",
                        elapsed_ms(map_mode_begin, SDL_GetPerformanceCounter()));
    }
    if (depth_guide_selection_ == DepthGuideSelection::BlueLayer &&
        depth_guide_blue_wheel_last_change_ms_ > 0 &&
        !depth_guide_drag_active_) {
        const Uint64 now = SDL_GetTicks();
        if (now > depth_guide_blue_wheel_last_change_ms_ &&
            now - depth_guide_blue_wheel_last_change_ms_ >= 180 &&
            depth_guide_preview_active_ && assets_) {
            assets_->getView().set_realism_settings(depth_guide_preview_settings_);
            if (camera_panel_) {
                camera_panel_->sync_debug_controls_from_settings(depth_guide_preview_settings_);
            }
            assets_->on_camera_settings_changed();
            mark_map_dirty();
            depth_guide_preview_active_ = false;
            depth_guide_blue_wheel_last_change_ms_ = 0;
        }
    }
    if (boundary_assets_modal_ && boundary_assets_modal_->visible()) {
        boundary_assets_modal_->update(input);
    }

    

    const bool layout_was_dirty = layout_dirty();
    frame_stats.set("dev.layout_dirty", layout_was_dirty);
    const Uint64 layout_begin = SDL_GetPerformanceCounter();
    if (layout_was_dirty) {
        ensure_layout_cache();
    } else {
        update_header_and_footer_bounds();
    }
    if (misc_options_panel_open_ || layout_was_dirty) {
        layout_misc_options_panel();
    }
    frame_stats.set("dev.layout_ms",
                    elapsed_ms(layout_begin, SDL_GetPerformanceCounter()));

    if (room_editor_ && room_editor_->is_enabled()) {
        SDL_Point pointer{input.getX(), input.getY()};
        if (other_settings_.contains_point(pointer.x, pointer.y)) {
            room_editor_->clear_highlighted_assets();
        } else if (misc_options_panel_open_ && SDL_PointInRect(&pointer, &misc_options_panel_rect_)) {
            room_editor_->clear_highlighted_assets();
        } else if (last_header_rect_.w > 0 && last_header_rect_.h > 0 && SDL_PointInRect(&pointer, &last_header_rect_)) {
            room_editor_->clear_highlighted_assets();
        } else if (last_footer_rect_.w > 0 && last_footer_rect_.h > 0 && SDL_PointInRect(&pointer, &last_footer_rect_)) {
            room_editor_->clear_highlighted_assets();
        }
    }



    sync_header_button_states();

    if (frame_editor_session_ && frame_editor_session_->is_active()) {
        frame_editor_session_->update(input);
    }

    if (render_suppression_in_progress_) {
        WarpedScreenGrid* cam = assets_ ? &assets_->getView() : nullptr;
        const bool camera_idle = !cam || !cam->is_height_animating();
        if (camera_idle) {
            if (assets_) {
                assets_->set_render_suppressed(false);
            }
            render_suppression_in_progress_ = false;
        }
    }

    const Uint64 save_begin = SDL_GetPerformanceCounter();
    if (save_manager_.has_dirty_saveables()) {
        try {
            save_manager_.save_dirty_with_reason(devmode::core::DevSaveCoordinator::Priority::Debounced,
                                                 devmode::core::SaveOrchestrator::Reason::AutoSave,
                                                 "DevControls update dirty saveables");
        } catch (const std::exception& ex) {
            std::cerr << "[DevControls] save_dirty_with_reason failed: " << ex.what() << "\n";
            if (assets_) {
                assets_->show_dev_notice("Save failed");
            }
        } catch (...) {
            std::cerr << "[DevControls] save_dirty_with_reason failed: unknown error\n";
            if (assets_) {
                assets_->show_dev_notice("Save failed");
            }
        }
    }
    frame_stats.set("dev.save_ms", elapsed_ms(save_begin, SDL_GetPerformanceCounter()));

    save_coordinator_.tick();
}

void DevControls::update_ui(const Input& input) {
    if (!enabled_) return;
    if (!room_editor_) return;

    const bool room_editor_active = (mode_ == Mode::RoomEditor) && room_editor_->is_enabled();
    const bool spawn_panel_visible = room_editor_->is_spawn_group_panel_visible();

    if (!room_editor_active && !spawn_panel_visible) {
        return;
    }

    room_editor_->update_ui(input);
}

void DevControls::reset_drop_preview() {
    drop_state_ = DropPreviewState{};
}

void DevControls::reset_drop_modal() {
    drop_modal_ = DropNameModal{};
}

void DevControls::reset_drop_choice_modal() {
    drop_choice_modal_ = DropChoiceModal{};
}

void DevControls::reset_drop_conflict_modal() {
    drop_conflict_modal_ = DropConflictModal{};
}

void DevControls::reset_drop_error_popup() {
    drop_error_popup_ = DropErrorPopup{};
}

void DevControls::reset_multi_asset_import() {
    multi_asset_import_ = MultiAssetImportState{};
}

void DevControls::begin_import_busy(const std::string& message) {
    import_busy_.active = true;
    import_busy_.message = message.empty() ? "Importing assets..." : message;
    import_busy_.started_ms = SDL_GetTicks();
}

void DevControls::end_import_busy() {
    import_busy_ = ImportBusyOverlay{};
}

bool DevControls::is_import_busy() const {
    return import_busy_.active;
}

SDL_Point DevControls::drop_world_from_screen(SDL_Point screen) const {
    if (!assets_) {
        return screen;
    }
    SDL_FPoint mapped = assets_->getView().screen_to_map(screen);
    return SDL_Point{static_cast<int>(std::lround(mapped.x)), static_cast<int>(std::lround(mapped.y))};
}

void DevControls::layout_drop_modal() {
    if (!drop_modal_.visible) return;
    const int modal_w = 520;
    const int modal_h = 220;
    const int padding = 16;
    int x = std::max(0, screen_w_ / 2 - modal_w / 2);
    int y = std::max(0, screen_h_ / 2 - modal_h / 2);
    drop_modal_.modal_rect = SDL_Rect{x, y, modal_w, modal_h};

    const int textbox_w = modal_w - padding * 2;
    int textbox_h = DMTextBox::height();
    if (drop_modal_.name_box) {
        textbox_h = drop_modal_.name_box->preferred_height(textbox_w);
        drop_modal_.name_box->set_rect(SDL_Rect{x + padding, y + padding, textbox_w, textbox_h});
    }

    const int button_gap = 12;
    const int button_w = 140;
    const int button_h = DMButton::height();
    const int buttons_total_w = button_w * 2 + button_gap;
    const int buttons_x = x + (modal_w - buttons_total_w) / 2;
    const int buttons_y = y + modal_h - button_h - padding;
    drop_modal_.create_rect = SDL_Rect{buttons_x, buttons_y, button_w, button_h};
    drop_modal_.cancel_rect = SDL_Rect{buttons_x + button_w + button_gap, buttons_y, button_w, button_h};
    if (drop_modal_.create_button) drop_modal_.create_button->set_rect(drop_modal_.create_rect);
    if (drop_modal_.cancel_button) drop_modal_.cancel_button->set_rect(drop_modal_.cancel_rect);
}

void DevControls::layout_drop_choice_modal() {
    if (!drop_choice_modal_.visible) return;
    const int modal_w = 560;
    const int modal_h = 250;
    const int padding = 16;
    int x = std::max(0, screen_w_ / 2 - modal_w / 2);
    int y = std::max(0, screen_h_ / 2 - modal_h / 2);
    drop_choice_modal_.modal_rect = SDL_Rect{x, y, modal_w, modal_h};

    const int button_gap = 12;
    const int button_w = modal_w - padding * 2;
    const int button_h = DMButton::height();
    const int buttons_y = y + 72;
    drop_choice_modal_.single_rect = SDL_Rect{x + padding, buttons_y, button_w, button_h};
    drop_choice_modal_.multiple_rect = SDL_Rect{x + padding, buttons_y + button_h + button_gap, button_w, button_h};
    drop_choice_modal_.cancel_rect = SDL_Rect{x + padding, buttons_y + (button_h + button_gap) * 2, button_w, button_h};
    if (drop_choice_modal_.single_animation_button) drop_choice_modal_.single_animation_button->set_rect(drop_choice_modal_.single_rect);
    if (drop_choice_modal_.multiple_assets_button) drop_choice_modal_.multiple_assets_button->set_rect(drop_choice_modal_.multiple_rect);
    if (drop_choice_modal_.cancel_button) drop_choice_modal_.cancel_button->set_rect(drop_choice_modal_.cancel_rect);
}

void DevControls::layout_drop_conflict_modal() {
    if (!drop_conflict_modal_.visible) return;
    const int modal_w = 560;
    const int modal_h = 210;
    const int padding = 16;
    int x = std::max(0, screen_w_ / 2 - modal_w / 2);
    int y = std::max(0, screen_h_ / 2 - modal_h / 2);
    drop_conflict_modal_.modal_rect = SDL_Rect{x, y, modal_w, modal_h};

    const int button_gap = 12;
    const int button_w = 160;
    const int button_h = DMButton::height();
    const int total_w = button_w * 2 + button_gap;
    const int bx = x + (modal_w - total_w) / 2;
    const int by = y + modal_h - button_h - padding;
    drop_conflict_modal_.skip_rect = SDL_Rect{bx, by, button_w, button_h};
    drop_conflict_modal_.rename_rect = SDL_Rect{bx + button_w + button_gap, by, button_w, button_h};
    if (drop_conflict_modal_.skip_button) drop_conflict_modal_.skip_button->set_rect(drop_conflict_modal_.skip_rect);
    if (drop_conflict_modal_.rename_button) drop_conflict_modal_.rename_button->set_rect(drop_conflict_modal_.rename_rect);
}

void DevControls::layout_drop_error_popup() {
    if (!drop_error_popup_.visible) return;
    const int modal_w = 560;
    const int modal_h = 180;
    const int padding = 16;
    int x = std::max(0, screen_w_ / 2 - modal_w / 2);
    int y = std::max(0, screen_h_ / 2 - modal_h / 2);
    drop_error_popup_.modal_rect = SDL_Rect{x, y, modal_w, modal_h};

    const int button_w = 140;
    const int button_h = DMButton::height();
    const int bx = x + (modal_w - button_w) / 2;
    const int by = y + modal_h - button_h - padding;
    drop_error_popup_.ok_rect = SDL_Rect{bx, by, button_w, button_h};
    if (drop_error_popup_.ok_button) drop_error_popup_.ok_button->set_rect(drop_error_popup_.ok_rect);
}

void DevControls::open_drop_choice_modal(const DropImportRequest& request) {
    drop_choice_modal_ = DropChoiceModal{};
    drop_choice_modal_.visible = true;
    drop_choice_modal_.request = request;
    drop_choice_modal_.single_animation_button = std::make_unique<DMButton>("Single Animation (multiple frames)", &DMStyles::CreateButton(), 280, DMButton::height());
    drop_choice_modal_.multiple_assets_button = std::make_unique<DMButton>("Multiple Assets (one frame each)", &DMStyles::CreateButton(), 280, DMButton::height());
    drop_choice_modal_.cancel_button = std::make_unique<DMButton>("Cancel", &DMStyles::HeaderButton(), 280, DMButton::height());
    layout_drop_choice_modal();
}

void DevControls::open_drop_conflict_modal(const std::string& asset_name) {
    drop_conflict_modal_ = DropConflictModal{};
    drop_conflict_modal_.visible = true;
    drop_conflict_modal_.asset_name = asset_name;
    drop_conflict_modal_.skip_button = std::make_unique<DMButton>("Skip", &DMStyles::HeaderButton(), 160, DMButton::height());
    drop_conflict_modal_.rename_button = std::make_unique<DMButton>("Rename", &DMStyles::CreateButton(), 160, DMButton::height());
    layout_drop_conflict_modal();
}

void DevControls::open_drop_error_popup(const std::string& message) {
    drop_error_popup_ = DropErrorPopup{};
    drop_error_popup_.visible = true;
    drop_error_popup_.message = message;
    drop_error_popup_.ok_button = std::make_unique<DMButton>("OK", &DMStyles::CreateButton(), 140, DMButton::height());
    layout_drop_error_popup();
}

void DevControls::open_drop_modal(const DropImportRequest& request) {
    drop_modal_ = DropNameModal{};
    drop_modal_.visible = true;
    drop_modal_.request = request;
    DropValidationResult validation;
    validation.kind = request.kind;
    validation.files = request.files;
    validation.folder = request.folder;
    validation.valid = true;
    const std::string suggested = suggest_name_from_drop(validation);
    drop_modal_.name_box = std::make_unique<DMTextBox>("Animation / Asset Name", suggested);
    drop_modal_.create_button = std::make_unique<DMButton>("Create", &DMStyles::CreateButton(), 140, DMButton::height());
    drop_modal_.cancel_button = std::make_unique<DMButton>("Cancel", &DMStyles::HeaderButton(), 140, DMButton::height());
    layout_drop_modal();
}

void DevControls::begin_multi_asset_import(const DropImportRequest& request) {
    multi_asset_import_ = MultiAssetImportState{};
    multi_asset_import_.active = true;
    multi_asset_import_.index = 0;
    multi_asset_import_.waiting_for_rename = false;
    multi_asset_import_.imported_count = 0;

    multi_asset_import_.items.reserve(request.files.size());
    for (const auto& file : request.files) {
        MultiAssetImportState::Item item;
        item.request = request;
        item.request.kind = DropContentKind::SinglePng;
        item.request.files = {file};
        item.request.folder.clear();
        item.suggested_name = sanitize_asset_name_local(vibble::strings::to_lower_copy(file.stem().string()));
        if (item.suggested_name.empty()) {
            item.error_message = "Invalid filename for asset: " + file.filename().string();
        }
        multi_asset_import_.items.push_back(std::move(item));
    }

    process_next_multi_asset_item();
}

void DevControls::begin_multi_folder_import(const std::vector<std::filesystem::path>& folders, SDL_Point drop_screen) {
    multi_asset_import_ = MultiAssetImportState{};
    multi_asset_import_.active = true;
    multi_asset_import_.index = 0;
    multi_asset_import_.waiting_for_rename = false;
    multi_asset_import_.imported_count = 0;

    for (const auto& folder : folders) {
        std::error_code ec;
        if (!std::filesystem::exists(folder, ec) || ec || !std::filesystem::is_directory(folder, ec) || ec) {
            continue;
        }

        MultiAssetImportState::Item item;
        DropValidationResult validation = validate_drop_items({folder});
        if (!validation.valid) {
            item.error_message = validation.reason.empty()
                                     ? "Failed to import folder '" + folder.filename().string() + "'."
                                     : validation.reason;
            multi_asset_import_.items.push_back(std::move(item));
            continue;
        }

        DropImportRequest request;
        request.kind = validation.kind;
        request.files = validation.files;
        request.folder = validation.folder;
        request.drop_screen = drop_screen;
        item.request = std::move(request);
        item.suggested_name = sanitize_asset_name_local(folder.filename().string());
        if (item.suggested_name.empty()) {
            item.error_message = "Invalid folder name for asset: " + folder.filename().string();
        }
        multi_asset_import_.items.push_back(std::move(item));
    }

    if (multi_asset_import_.items.empty()) {
        reset_multi_asset_import();
        return;
    }

    process_next_multi_asset_item();
}

const DevControls::MultiAssetImportState::Item* DevControls::current_multi_asset_import_item() const {
    if (!multi_asset_import_.active || multi_asset_import_.index >= multi_asset_import_.items.size()) {
        return nullptr;
    }
    return &multi_asset_import_.items[multi_asset_import_.index];
}

void DevControls::process_next_multi_asset_item() {
    if (!multi_asset_import_.active || multi_asset_import_.waiting_for_rename) {
        return;
    }
    ImportBusyScope busy(this, "Importing assets...");
    if (assets_) {
        if (SDL_Renderer* r = assets_->renderer()) {
            render_import_busy_overlay(r);
        }
    }
    while (multi_asset_import_.index < multi_asset_import_.items.size()) {
        const MultiAssetImportState::Item& item = multi_asset_import_.items[multi_asset_import_.index];
        if (!item.error_message.empty()) {
            open_drop_error_popup(item.error_message);
            ++multi_asset_import_.index;
            return;
        }

        const std::filesystem::path source_file =
            !item.request.files.empty() ? item.request.files.front() : std::filesystem::path{};
        std::string candidate = sanitize_asset_name_local(item.suggested_name);
        if (candidate.empty()) {
            const std::string fallback_name = !source_file.empty() ? source_file.filename().string() : std::string{"(unknown)"};
            open_drop_error_popup("Invalid name for dropped asset: " + fallback_name);
            ++multi_asset_import_.index;
            return;
        }

        if (manifest_store_.resolve_asset_name(candidate) || (assets_ && assets_->library().get(candidate))) {
            open_drop_conflict_modal(candidate);
            multi_asset_import_.waiting_for_rename = true;
            return;
        }

        std::string error;
        if (!create_drop_asset(candidate, item.request.files, item.request, false, error)) {
            const std::string fallback_error =
                !source_file.empty()
                    ? "Import failed for " + source_file.filename().string()
                    : "Import failed for dropped item.";
            open_drop_error_popup(error.empty() ? fallback_error : error);
            ++multi_asset_import_.index;
            return;
        }
        ++multi_asset_import_.imported_count;
        ++multi_asset_import_.index;
    }

    if (assets_ && multi_asset_import_.imported_count > 0) {
        const std::string notice = "Imported " + std::to_string(multi_asset_import_.imported_count) + " dropped assets";
        assets_->show_dev_notice(notice, 1800);
    }
    reset_multi_asset_import();
}

bool DevControls::handle_drop_event(const SDL_Event& event) {
    if (!enabled_ || mode_ != Mode::RoomEditor || !room_editor_ || !room_editor_->is_enabled()) {
        return false;
    }
    if (frame_editor_session_ && frame_editor_session_->is_active()) {
        return false;
    }
    if (drop_modal_.visible || drop_choice_modal_.visible || drop_conflict_modal_.visible || drop_error_popup_.visible) {
        return false;
    }

    auto drop_point_from_event = [](const SDL_Event& e) -> SDL_Point {
        return SDL_Point{
            static_cast<int>(std::lround(e.drop.x)),
            static_cast<int>(std::lround(e.drop.y))
        };
    };

    switch (event.type) {
    case SDL_EVENT_DROP_BEGIN:
        reset_drop_preview();
        drop_state_.active = true;
        drop_state_.screen = drop_point_from_event(event);
        return true;
    case SDL_EVENT_DROP_FILE: {
        if (!drop_state_.active) {
            reset_drop_preview();
            drop_state_.active = true;
        }
        drop_state_.screen = drop_point_from_event(event);
        if (event.drop.data) {
            std::filesystem::path path = std::filesystem::u8path(event.drop.data);
            drop_state_.items.push_back(path);
        }
        DropValidationResult validation = validate_drop_items(drop_state_.items);
        drop_state_.valid = validation.valid || is_multi_folder_drop(drop_state_.items);
        return true;
    }
    case SDL_EVENT_DROP_COMPLETE: {
        if (is_multi_folder_drop(drop_state_.items)) {
            begin_multi_folder_import(collect_existing_drop_items(drop_state_.items), drop_point_from_event(event));
            reset_drop_preview();
            return true;
        }

        DropValidationResult validation = validate_drop_items(drop_state_.items);
        if (validation.valid) {
            DropImportRequest req;
            req.kind = validation.kind;
            req.files = validation.files;
            req.folder = validation.folder;
            req.drop_screen = drop_point_from_event(event);
            if (req.kind == DropContentKind::MultiImages) {
                bool all_images = !req.files.empty();
                for (const auto& p : req.files) {
                    if (!is_supported_image_file(p)) {
                        all_images = false;
                        break;
                    }
                }
                if (all_images) {
                    open_drop_choice_modal(req);
                } else {
                    open_drop_modal(req);
                }
            } else {
                open_drop_modal(req);
            }
        }
        reset_drop_preview();
        return true;
    }
    default:
        break;
    }
    return false;
}

bool DevControls::handle_drop_modal_event(const SDL_Event& event) {
    if (!drop_modal_.visible) {
        return false;
    }
    layout_drop_modal();
    bool consumed = false;
    SDL_Point pointer{0, 0};
    const bool pointer_event = is_pointer_event(event);
    if (pointer_event) {
        pointer = event_point(event);
    }

    if (drop_modal_.name_box && drop_modal_.name_box->handle_event(event)) {
        consumed = true;
    }

    if (drop_modal_.create_button && drop_modal_.create_button->handle_event(event)) {
        consumed = true;
        if (event.type == SDL_EVENT_MOUSE_BUTTON_UP && event.button.button == SDL_BUTTON_LEFT) {
            const std::string desired = drop_modal_.name_box ? drop_modal_.name_box->value() : std::string{};
            if (finalize_drop_creation(desired)) {
                reset_drop_modal();
                if (multi_asset_import_.active) {
                    multi_asset_import_.waiting_for_rename = false;
                    ++multi_asset_import_.index;
                    process_next_multi_asset_item();
                }
            }
        }
    }

    if (drop_modal_.cancel_button && drop_modal_.cancel_button->handle_event(event)) {
        consumed = true;
        if (event.type == SDL_EVENT_MOUSE_BUTTON_UP && event.button.button == SDL_BUTTON_LEFT) {
            reset_drop_modal();
            if (multi_asset_import_.active) {
                multi_asset_import_.waiting_for_rename = false;
                ++multi_asset_import_.index;
                process_next_multi_asset_item();
            }
        }
    }

    if (event.type == SDL_EVENT_KEY_DOWN) {
        if (event.key.key == SDLK_ESCAPE) {
            reset_drop_modal();
            if (multi_asset_import_.active) {
                multi_asset_import_.waiting_for_rename = false;
                ++multi_asset_import_.index;
                process_next_multi_asset_item();
            }
            consumed = true;
        } else if (event.key.key == SDLK_RETURN || event.key.key == SDLK_KP_ENTER) {
            const std::string desired = drop_modal_.name_box ? drop_modal_.name_box->value() : std::string{};
            if (finalize_drop_creation(desired)) {
                reset_drop_modal();
                if (multi_asset_import_.active) {
                    multi_asset_import_.waiting_for_rename = false;
                    ++multi_asset_import_.index;
                    process_next_multi_asset_item();
                }
            }
            consumed = true;
        }
    }
    if (event.type == SDL_EVENT_TEXT_INPUT) {
        consumed = true;
    }

    const bool pointer_inside = pointer_event && SDL_PointInRect(&pointer, &drop_modal_.modal_rect);
    return consumed || pointer_inside;
}

bool DevControls::handle_drop_choice_modal_event(const SDL_Event& event) {
    if (!drop_choice_modal_.visible) {
        return false;
    }
    layout_drop_choice_modal();
    bool consumed = false;
    SDL_Point pointer{0, 0};
    const bool pointer_event = is_pointer_event(event);
    if (pointer_event) {
        pointer = event_point(event);
    }

    if (drop_choice_modal_.single_animation_button && drop_choice_modal_.single_animation_button->handle_event(event)) {
        consumed = true;
        if (event.type == SDL_EVENT_MOUSE_BUTTON_UP && event.button.button == SDL_BUTTON_LEFT) {
            DropImportRequest request = drop_choice_modal_.request;
            reset_drop_choice_modal();
            open_drop_modal(request);
            return true;
        }
    }
    if (drop_choice_modal_.multiple_assets_button && drop_choice_modal_.multiple_assets_button->handle_event(event)) {
        consumed = true;
        if (event.type == SDL_EVENT_MOUSE_BUTTON_UP && event.button.button == SDL_BUTTON_LEFT) {
            DropImportRequest request = drop_choice_modal_.request;
            reset_drop_choice_modal();
            begin_multi_asset_import(request);
            return true;
        }
    }
    if (drop_choice_modal_.cancel_button && drop_choice_modal_.cancel_button->handle_event(event)) {
        consumed = true;
        if (event.type == SDL_EVENT_MOUSE_BUTTON_UP && event.button.button == SDL_BUTTON_LEFT) {
            reset_drop_choice_modal();
            reset_multi_asset_import();
            return true;
        }
    }
    if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_ESCAPE) {
        reset_drop_choice_modal();
        reset_multi_asset_import();
        return true;
    }

    const bool pointer_inside = pointer_event && SDL_PointInRect(&pointer, &drop_choice_modal_.modal_rect);
    return consumed || pointer_inside;
}

bool DevControls::handle_drop_conflict_modal_event(const SDL_Event& event) {
    if (!drop_conflict_modal_.visible) {
        return false;
    }
    layout_drop_conflict_modal();
    bool consumed = false;
    SDL_Point pointer{0, 0};
    const bool pointer_event = is_pointer_event(event);
    if (pointer_event) {
        pointer = event_point(event);
    }

    if (drop_conflict_modal_.skip_button && drop_conflict_modal_.skip_button->handle_event(event)) {
        consumed = true;
        if (event.type == SDL_EVENT_MOUSE_BUTTON_UP && event.button.button == SDL_BUTTON_LEFT) {
            reset_drop_conflict_modal();
            multi_asset_import_.waiting_for_rename = false;
            ++multi_asset_import_.index;
            process_next_multi_asset_item();
            return true;
        }
    }
    if (drop_conflict_modal_.rename_button && drop_conflict_modal_.rename_button->handle_event(event)) {
        consumed = true;
        if (event.type == SDL_EVENT_MOUSE_BUTTON_UP && event.button.button == SDL_BUTTON_LEFT) {
            DropImportRequest request;
            const MultiAssetImportState::Item* item = current_multi_asset_import_item();
            if (!item) {
                reset_drop_conflict_modal();
                multi_asset_import_.waiting_for_rename = false;
                ++multi_asset_import_.index;
                process_next_multi_asset_item();
                return true;
            }
            request = item->request;
            reset_drop_conflict_modal();
            multi_asset_import_.waiting_for_rename = false;
            open_drop_modal(request);
            return true;
        }
    }

    if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_ESCAPE) {
        reset_drop_conflict_modal();
        multi_asset_import_.waiting_for_rename = false;
        ++multi_asset_import_.index;
        process_next_multi_asset_item();
        return true;
    }

    const bool pointer_inside = pointer_event && SDL_PointInRect(&pointer, &drop_conflict_modal_.modal_rect);
    return consumed || pointer_inside;
}

bool DevControls::handle_drop_error_popup_event(const SDL_Event& event) {
    if (!drop_error_popup_.visible) {
        return false;
    }
    layout_drop_error_popup();
    bool consumed = false;
    SDL_Point pointer{0, 0};
    const bool pointer_event = is_pointer_event(event);
    if (pointer_event) {
        pointer = event_point(event);
    }

    if (drop_error_popup_.ok_button && drop_error_popup_.ok_button->handle_event(event)) {
        consumed = true;
        if (event.type == SDL_EVENT_MOUSE_BUTTON_UP && event.button.button == SDL_BUTTON_LEFT) {
            reset_drop_error_popup();
            process_next_multi_asset_item();
            return true;
        }
    }
    if (event.type == SDL_EVENT_KEY_DOWN &&
        (event.key.key == SDLK_ESCAPE || event.key.key == SDLK_RETURN || event.key.key == SDLK_KP_ENTER)) {
        reset_drop_error_popup();
        process_next_multi_asset_item();
        return true;
    }

    const bool pointer_inside = pointer_event && SDL_PointInRect(&pointer, &drop_error_popup_.modal_rect);
    return consumed || pointer_inside;
}

void DevControls::render_drop_overlay(SDL_Renderer* renderer) {
    if (!renderer) return;
    if (!drop_state_.active) return;
    const int thickness = 10;
    SDL_BlendMode prev = SDL_BLENDMODE_NONE;
    SDL_GetRenderDrawBlendMode(renderer, &prev);
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    SDL_Color c = drop_state_.valid ? SDL_Color{255, 255, 255, 220} : SDL_Color{230, 40, 40, 220};
    SDL_SetRenderDrawColor(renderer, c.r, c.g, c.b, c.a);
    SDL_Rect top{0, 0, screen_w_, thickness};
    SDL_Rect bottom{0, std::max(0, screen_h_ - thickness), screen_w_, thickness};
    SDL_Rect left{0, 0, thickness, screen_h_};
    SDL_Rect right{std::max(0, screen_w_ - thickness), 0, thickness, screen_h_};
    sdl_render::FillRect(renderer, &top);
    sdl_render::FillRect(renderer, &bottom);
    sdl_render::FillRect(renderer, &left);
    sdl_render::FillRect(renderer, &right);
    SDL_SetRenderDrawBlendMode(renderer, prev);
}

void DevControls::render_drop_modal(SDL_Renderer* renderer) {
    if (!renderer || !drop_modal_.visible) return;
    layout_drop_modal();
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    dm_draw::DrawBeveledRect(renderer,
                             drop_modal_.modal_rect,
                             DMStyles::CornerRadius(),
                             DMStyles::BevelDepth(),
                             DMStyles::PanelBG(),
                             DMStyles::HighlightColor(),
                             DMStyles::ShadowColor(),
                             false,
                             DMStyles::HighlightIntensity(),
                             DMStyles::ShadowIntensity());

    const int title_x = drop_modal_.modal_rect.x + 16;
    const int title_y = drop_modal_.modal_rect.y + 8;
    DrawLabelText(renderer, "Create animation from dropped files", title_x, title_y, DMStyles::Label());

    if (drop_modal_.name_box) drop_modal_.name_box->render(renderer);
    if (drop_modal_.create_button) drop_modal_.create_button->render(renderer);
    if (drop_modal_.cancel_button) drop_modal_.cancel_button->render(renderer);

    if (!drop_modal_.error.empty()) {
        DMLabelStyle warn = DMStyles::Label();
        warn.color = SDL_Color{220, 60, 60, 255};
        const int err_x = drop_modal_.modal_rect.x + 16;
        const int err_y = drop_modal_.modal_rect.y + drop_modal_.modal_rect.h / 2 + 6;
        DrawLabelText(renderer, drop_modal_.error, err_x, err_y, warn);
    }
}

void DevControls::render_drop_choice_modal(SDL_Renderer* renderer) {
    if (!renderer || !drop_choice_modal_.visible) return;
    layout_drop_choice_modal();
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    dm_draw::DrawBeveledRect(renderer,
                             drop_choice_modal_.modal_rect,
                             DMStyles::CornerRadius(),
                             DMStyles::BevelDepth(),
                             DMStyles::PanelBG(),
                             DMStyles::HighlightColor(),
                             DMStyles::ShadowColor(),
                             false,
                             DMStyles::HighlightIntensity(),
                             DMStyles::ShadowIntensity());
    DrawLabelText(renderer,
                  "Import dropped image files",
                  drop_choice_modal_.modal_rect.x + 16,
                  drop_choice_modal_.modal_rect.y + 8,
                  DMStyles::Label());
    if (drop_choice_modal_.single_animation_button) drop_choice_modal_.single_animation_button->render(renderer);
    if (drop_choice_modal_.multiple_assets_button) drop_choice_modal_.multiple_assets_button->render(renderer);
    if (drop_choice_modal_.cancel_button) drop_choice_modal_.cancel_button->render(renderer);
}

void DevControls::render_drop_conflict_modal(SDL_Renderer* renderer) {
    if (!renderer || !drop_conflict_modal_.visible) return;
    layout_drop_conflict_modal();
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    dm_draw::DrawBeveledRect(renderer,
                             drop_conflict_modal_.modal_rect,
                             DMStyles::CornerRadius(),
                             DMStyles::BevelDepth(),
                             DMStyles::PanelBG(),
                             DMStyles::HighlightColor(),
                             DMStyles::ShadowColor(),
                             false,
                             DMStyles::HighlightIntensity(),
                             DMStyles::ShadowIntensity());
    DrawLabelText(renderer,
                  "Asset already exists: " + drop_conflict_modal_.asset_name,
                  drop_conflict_modal_.modal_rect.x + 16,
                  drop_conflict_modal_.modal_rect.y + 16,
                  DMStyles::Label());
    DrawLabelText(renderer,
                  "Choose Skip or Rename.",
                  drop_conflict_modal_.modal_rect.x + 16,
                  drop_conflict_modal_.modal_rect.y + 46,
                  DMStyles::Label());
    if (drop_conflict_modal_.skip_button) drop_conflict_modal_.skip_button->render(renderer);
    if (drop_conflict_modal_.rename_button) drop_conflict_modal_.rename_button->render(renderer);
}

void DevControls::render_drop_error_popup(SDL_Renderer* renderer) {
    if (!renderer || !drop_error_popup_.visible) return;
    layout_drop_error_popup();
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    dm_draw::DrawBeveledRect(renderer,
                             drop_error_popup_.modal_rect,
                             DMStyles::CornerRadius(),
                             DMStyles::BevelDepth(),
                             DMStyles::PanelBG(),
                             DMStyles::HighlightColor(),
                             DMStyles::ShadowColor(),
                             false,
                             DMStyles::HighlightIntensity(),
                             DMStyles::ShadowIntensity());
    DrawLabelText(renderer,
                  drop_error_popup_.message,
                  drop_error_popup_.modal_rect.x + 16,
                  drop_error_popup_.modal_rect.y + 24,
                  DMStyles::Label());
    if (drop_error_popup_.ok_button) drop_error_popup_.ok_button->render(renderer);
}

void DevControls::render_import_busy_overlay(SDL_Renderer* renderer) {
    if (!renderer || !import_busy_.active) return;
    SDL_BlendMode prev = SDL_BLENDMODE_NONE;
    SDL_GetRenderDrawBlendMode(renderer, &prev);
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    SDL_Color bg{15, 15, 20, 180};
    SDL_SetRenderDrawColor(renderer, bg.r, bg.g, bg.b, bg.a);
    SDL_Rect full{0, 0, screen_w_, screen_h_};
    sdl_render::FillRect(renderer, &full);

    const int spinner_size = 42;
    const int center_x = screen_w_ / 2;
    const int center_y = screen_h_ / 2;
    const Uint64 elapsed = SDL_GetTicks() - import_busy_.started_ms;
    constexpr double kTwoPi = 6.28318530717958647692;
    const double angle = static_cast<double>(elapsed % 2000) / 2000.0 * kTwoPi;
    SDL_Color fg{255, 255, 255, 235};
    SDL_SetRenderDrawColor(renderer, fg.r, fg.g, fg.b, fg.a);
    const int line_len = spinner_size;
    int x2 = center_x + static_cast<int>(std::lround(std::cos(angle) * line_len));
    int y2 = center_y + static_cast<int>(std::lround(std::sin(angle) * line_len));
    SDL_RenderLine(renderer, center_x, center_y, x2, y2);

    DrawLabelText(renderer,
                  import_busy_.message,
                  center_x - 140,
                  center_y + spinner_size + 12,
                  DMStyles::Label());

    SDL_SetRenderDrawBlendMode(renderer, prev);
}

bool DevControls::create_drop_asset(const std::string& asset_name,
                                   const std::vector<std::filesystem::path>& files,
                                   const DropImportRequest& request,
                                   bool open_editor_and_spawn,
                                   std::string& error_out) {
    ImportBusyScope busy(this, "Importing assets...");
    if (assets_) {
        if (SDL_Renderer* r = assets_->renderer()) {
            render_import_busy_overlay(r);
        }
    }

    error_out.clear();
    const std::string sanitized = sanitize_asset_name_local(devmode::utils::trim_whitespace_copy(asset_name));
    if (sanitized.empty()) {
        error_out = "Please enter a name (letters, numbers, underscore).";
        return false;
    }
    if (manifest_store_.resolve_asset_name(sanitized) || (assets_ && assets_->library().get(sanitized))) {
        error_out = "An asset with that name already exists.";
        return false;
    }

    std::vector<std::filesystem::path> validate_targets;
    if (request.kind == DropContentKind::PngFolder && !request.folder.empty()) {
        validate_targets.push_back(request.folder);
    } else {
        validate_targets = files;
    }
    DropValidationResult validation = validate_drop_items(validate_targets);
    if (!validation.valid) {
        error_out = validation.reason.empty() ? "Dropped content is invalid." : validation.reason;
        return false;
    }

    devmode::animation_import::CreateAssetRequest import_request;
    import_request.asset_name = sanitized;
    import_request.input_paths = validation.files;
    import_request.manifest_store = &manifest_store_;
    import_request.assets = assets_;
    devmode::animation_import::ImportResult import_result =
        devmode::animation_import::create_asset_from_frames(import_request);

    for (const auto& warning : import_result.warnings) {
        SDL_Log("[DevControls] Frame import warning for '%s': %s", sanitized.c_str(), warning.c_str());
    }

    if (!import_result.success) {
        error_out = import_result.message.empty() ? "No frames were imported." : import_result.message;
        return false;
    }

    std::shared_ptr<AssetInfo> info;
    if (assets_) {
        info = assets_->library().get(sanitized);
    }

    if (open_editor_and_spawn && assets_ && info) {
        Asset* spawned = nullptr;
        SDL_Point world = drop_world_from_screen(request.drop_screen);
        spawned = assets_->spawn_asset(sanitized, world);
        if (room_editor_ && spawned) {
            room_editor_->finalize_asset_drag(spawned, info);
        }
        assets_->open_asset_info_editor(info);
        assets_->show_dev_notice("Created asset '" + sanitized + "'", 1800);
    }

    return true;
}

bool DevControls::finalize_drop_creation(const std::string& desired_name) {
    std::string error;
    if (!create_drop_asset(desired_name, drop_modal_.request.files, drop_modal_.request, true, error)) {
        drop_modal_.error = error;
        return false;
    }
    return true;
}

void DevControls::handle_sdl_event(const SDL_Event& event) {
    if (!enabled_) return;

    if (import_busy_.active &&
        !drop_modal_.visible &&
        !drop_choice_modal_.visible &&
        !drop_conflict_modal_.visible &&
        !drop_error_popup_.visible) {
        if (input_) {
            input_->consumeEvent(event);
        }
        return;
    }

    if (drop_error_popup_.visible) {
        handle_drop_error_popup_event(event);
        if (input_) {
            input_->consumeEvent(event);
        }
        return;
    }

    if (drop_conflict_modal_.visible) {
        handle_drop_conflict_modal_event(event);
        if (input_) {
            input_->consumeEvent(event);
        }
        return;
    }

    if (drop_choice_modal_.visible) {
        handle_drop_choice_modal_event(event);
        if (input_) {
            input_->consumeEvent(event);
        }
        return;
    }

    if (drop_modal_.visible) {
        handle_drop_modal_event(event);
        if (input_) {
            input_->consumeEvent(event);
        }
        return;
    }

    if (handle_drop_event(event)) {
        return;
    }

    if (handle_misc_options_panel_event(event)) {
        if (input_) {
            input_->consumeEvent(event);
        }
        return;
    }

    other_settings_.ensure_layout();
    SDL_Rect header_rect{0, 0, 0, 0};
    SDL_Rect layout_rect = other_settings_.layout_bounds();
    SDL_Rect footer_rect{0, 0, 0, 0};
    std::vector<SDL_Rect> sliding_rects;
    if (map_mode_ui_) {
        map_mode_ui_->collect_sliding_container_rects(sliding_rects);
    }
    if (layout_rect.w > 0 && layout_rect.h > 0) {
        sliding_rects.push_back(layout_rect);
    }
    const bool modal_hide_pre = is_modal_blocking_panels();
    const bool layers_panel_open_pre = map_mode_ui_ && map_mode_ui_->is_layers_panel_visible();
    const bool frame_editor_active = frame_editor_session_ && frame_editor_session_->is_active();
    const bool asset_editor_tab_active = room_editor_ && room_editor_->is_asset_editor_tab_scope_active();
    if (map_mode_ui_) {
        DevFooterBar* footer = map_mode_ui_->get_footer_bar();
        if (footer && footer->visible()) {
            footer_rect = footer->rect();
        }
    }
    const bool hide_headers_pre =
        modal_hide_pre || sliding_headers_hidden_ || layers_panel_open_pre ||
        frame_editor_active || asset_editor_tab_active || shift_block_headers_footers_;
    header_rect = hide_headers_pre ? SDL_Rect{0, 0, 0, 0} : other_settings_.header_rect();
    SDL_Rect usable_rect = DockManager::instance().computeUsableRect(
        SDL_Rect{0, 0, screen_w_, screen_h_},
        header_rect,
        footer_rect,
        sliding_rects);
    if (map_mode_ui_) {
        map_mode_ui_->set_sliding_area_bounds(usable_rect);
    }

    const bool pointer_event = is_pointer_event(event);
    const bool wheel_event = (event.type == SDL_EVENT_MOUSE_WHEEL);
    const bool pointer_relevant = pointer_event || wheel_event;
    const bool keyboard_like_event =
        (event.type == SDL_EVENT_KEY_DOWN || event.type == SDL_EVENT_KEY_UP || event.type == SDL_EVENT_TEXT_INPUT);
    SDL_Point pointer{0, 0};
    if (pointer_relevant) {
        pointer = event_point(event);
    }

    const bool modal_hide = is_modal_blocking_panels();
    const bool layers_panel_open = map_mode_ui_ && map_mode_ui_->is_layers_panel_visible();
    modal_headers_hidden_ = modal_hide;
    const bool hide_headers =
        modal_hide || sliding_headers_hidden_ || layers_panel_open ||
        frame_editor_active || asset_editor_tab_active || shift_block_headers_footers_;
    other_settings_.set_enabled(enabled_);
    other_settings_.set_header_suppressed(hide_headers);
    apply_header_suppression();

    auto consume_if_handled = [&](bool handled, bool pointer_inside) {
        if (handled && input_) {
            if (!pointer_relevant || pointer_inside) {
                input_->consumeEvent(event);
            }
        }
        return handled;
};

    auto handle_floating_panels = [&]() -> bool {
        auto floating = DockManager::instance().open_panels();
        if (floating.empty()) {
            return false;
        }
        SDL_Point wheel_point{0, 0};
        bool wheel_point_valid = false;
        for (auto it = floating.rbegin(); it != floating.rend(); ++it) {
            DockableCollapsible* panel = *it;
            if (!panel || !panel->is_visible()) {
                continue;
            }
            bool pointer_inside = false;
            if (pointer_relevant) {
                SDL_Point probe = pointer;
                if (!pointer_event) {
                    if (!wheel_point_valid) {
                        sdl_mouse_util::GetMouseState(&wheel_point.x, &wheel_point.y);
                        wheel_point_valid = true;
                    }
                    probe = wheel_point;
                }
                pointer_inside = panel->is_point_inside(probe.x, probe.y);
            }
            if (consume_if_handled(panel->handle_event(event), pointer_inside)) {
                return true;
            }
            if (pointer_relevant && pointer_inside) {
                return true;
            }
        }
        return false;
};

    if (handle_floating_panels()) {
        return;
    }

    if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_ESCAPE) {
        if (layers_panel_open && map_mode_ui_) {
            const bool was_visible = layers_panel_open;
            map_mode_ui_->toggle_layers_panel();
            const bool now_visible = map_mode_ui_->is_layers_panel_visible();
            if (assets_ && was_visible != now_visible) {
                assets_->show_dev_notice("Layers panel closed", 1200);
            }
            if (input_) {
                input_->consumeEvent(event);
            }
            return;
        }
    }

    if (!other_settings_.header_suppressed()) {
        const bool pointer_inside_header = pointer_relevant && enabled_ && other_settings_.contains_point(pointer.x, pointer.y);
        if (!asset_editor_tab_active) {
            if (pointer_event && consume_if_handled(other_settings_.handle_event(event), pointer_inside_header)) {
                return;
            }
        }
        if (pointer_inside_header) {
            return;
        }
    }

    

    if (consume_modal_event(boundary_assets_modal_.get(), event, pointer, pointer_relevant, input_)) {
        return;
    }
    if (consume_modal_event(regenerate_popup_.get(), event, pointer, pointer_relevant, input_)) {
        return;
    }

    if (frame_editor_active) {
        if (frame_editor_session_ && consume_if_handled(frame_editor_session_->handle_event(event), pointer_relevant)) {
            return;
        }
        return;
    }

    if (map_mode_ui_) {
        if (DevFooterBar* footer = map_mode_ui_->get_footer_bar()) {
            if (footer->visible() && !frame_editor_active) {
                const bool pointer_in_footer = pointer_relevant && footer->contains(pointer.x, pointer.y);
                if (consume_if_handled(footer->handle_event(event), pointer_in_footer)) {
                    return;
                }
                if (pointer_in_footer) {
                    return;
                }
            }
        }
    }

    const bool room_editor_active = can_use_room_editor_ui();
    const bool spawn_panel_visible = room_editor_ && room_editor_->is_spawn_group_panel_visible();
    const bool can_route_room_editor = room_editor_ && (room_editor_active || spawn_panel_visible);
    const bool pointer_over_room_ui = can_route_room_editor && pointer_relevant &&
                                      room_editor_->is_room_ui_blocking_point(pointer.x, pointer.y);

    if (pointer_over_room_ui) {
        const bool handled = room_editor_->handle_sdl_event(event);
        if (handled && input_) {
            input_->consumeEvent(event);
        }
        return;
    }

    bool pointer_event_inside_camera = false;
    if (camera_panel_ && camera_panel_->is_visible()) {
        switch (event.type) {
        case SDL_EVENT_MOUSE_MOTION:
            pointer_event_inside_camera = camera_panel_->is_point_inside(event.motion.x, event.motion.y);
            break;
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_BUTTON_UP:
            pointer_event_inside_camera = camera_panel_->is_point_inside(event.button.x, event.button.y);
            break;
        case SDL_EVENT_MOUSE_WHEEL: {
            int mx = 0;
            int my = 0;
            sdl_mouse_util::GetMouseState(&mx, &my);
            pointer_event_inside_camera = camera_panel_->is_point_inside(mx, my);
            break;
        }
        default:
            break;
        }
    }
    if (camera_panel_ && camera_panel_->is_visible()) {
        if (consume_if_handled(camera_panel_->handle_event(event), pointer_event_inside_camera)) {
            return;
        }
    }

    if (frame_editor_session_ && frame_editor_session_->is_active()) {
        if (consume_if_handled(frame_editor_session_->handle_event(event), pointer_relevant)) {
            return;
        }
    }

    bool block_for_camera = pointer_event_inside_camera;
    if (keyboard_like_event && pointer_over_camera_panel_) {
        block_for_camera = true;
    }
    if (block_for_camera) {
        if (!pointer_relevant && input_) {
            input_->consumeEvent(event);
        }
        return;
    }

    const bool depth_guides_enabled = camera_panel_ && camera_panel_->is_visible() &&
                                      camera_panel_->is_debug_section_expanded() && assets_;
    auto clear_depth_guide_selection = [&]() {
        depth_guide_selection_ = DepthGuideSelection::None;
        depth_guide_drag_active_ = false;
        depth_guide_preview_active_ = false;
    };
    if (!depth_guides_enabled) {
        clear_depth_guide_selection();
    } else if (pointer_relevant && assets_) {
        WarpedScreenGrid& cam = assets_->getView();
        const auto settings = depth_guide_preview_active_ ? depth_guide_preview_settings_ : cam.get_settings();
        struct Hit { DepthGuideSelection sel; float y; int prio; };
        std::vector<Hit> hits;
        if (const auto red_y = project_depth_guide_screen_y(cam, settings.max_cull_depth, screen_h_)) {
            hits.push_back({DepthGuideSelection::RedCull, *red_y, 3});
        }
        if (const auto orange_y = project_depth_guide_screen_y(
                cam,
                settings.dynamic_renderer_depth_efficiency_depth,
                screen_h_)) {
            hits.push_back({DepthGuideSelection::OrangeEfficiency, *orange_y, 2});
        }
        if (depth_guide_selection_ == DepthGuideSelection::None || depth_guide_selection_ == DepthGuideSelection::BlueLayer) {
            const double max_cull = std::max(1.0, static_cast<double>(settings.max_cull_depth));
            const double interval = std::max(1.0, static_cast<double>(settings.layer_depth_interval));
            const double curve = std::max(0.0, static_cast<double>(settings.layer_depth_curve));
            for (double edge : render_depth::build_background_depth_edges(max_cull, interval, curve)) {
                if (edge <= 0.0) continue;
                if (const auto y = project_depth_guide_screen_y(cam, static_cast<float>(edge), screen_h_)) {
                    hits.push_back({DepthGuideSelection::BlueLayer, *y, 1});
                }
            }
        }
        const float mx = static_cast<float>(pointer.x);
        const float my = static_cast<float>(pointer.y);
        std::optional<Hit> best;
        for (const auto& h : hits) {
            const float dy = std::fabs(my - h.y);
            if (dy > 8.0f) continue;
            if (!best || h.prio > best->prio || (h.prio == best->prio && dy < std::fabs(my - best->y))) {
                best = h;
            }
        }

        if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN && event.button.button == SDL_BUTTON_LEFT && best) {
            depth_guide_selection_ = best->sel;
            depth_guide_drag_active_ = true;
            depth_guide_drag_start_y_ = pointer.y;
            depth_guide_preview_settings_ = cam.get_settings();
            depth_guide_preview_active_ = true;
            if (input_) input_->consumeEvent(event);
            return;
        }
        if (event.type == SDL_EVENT_MOUSE_MOTION && depth_guide_drag_active_ && depth_guide_preview_active_) {
            const int dy = depth_guide_drag_start_y_ - pointer.y;
            auto preview = depth_guide_preview_settings_;
            if (depth_guide_selection_ == DepthGuideSelection::RedCull) {
                preview.max_cull_depth = std::max(1.0f, preview.max_cull_depth + static_cast<float>(dy));
                preview.dynamic_renderer_depth_efficiency_depth =
                    std::clamp(preview.dynamic_renderer_depth_efficiency_depth, 0.0f, preview.max_cull_depth);
            } else if (depth_guide_selection_ == DepthGuideSelection::OrangeEfficiency) {
                preview.dynamic_renderer_depth_efficiency_depth =
                    std::clamp(preview.dynamic_renderer_depth_efficiency_depth + static_cast<float>(dy), 0.0f, preview.max_cull_depth);
            } else if (depth_guide_selection_ == DepthGuideSelection::BlueLayer) {
                preview.layer_depth_interval = std::clamp(preview.layer_depth_interval + static_cast<float>(dy), 1.0f, 100000.0f);
            }
            depth_guide_drag_start_y_ = pointer.y;
            depth_guide_preview_settings_ = preview;
            cam.set_realism_settings(preview);
            if (camera_panel_) {
                camera_panel_->sync_debug_controls_from_settings(preview);
            }
            if (input_) input_->consumeEvent(event);
            return;
        }
        if (event.type == SDL_EVENT_MOUSE_WHEEL && depth_guide_selection_ == DepthGuideSelection::BlueLayer) {
            auto preview = depth_guide_preview_active_ ? depth_guide_preview_settings_ : cam.get_settings();
            preview.layer_depth_curve = std::clamp(preview.layer_depth_curve + static_cast<float>(event.wheel.y) * 0.05f, 0.0f, 200.0f);
            depth_guide_preview_settings_ = preview;
            depth_guide_preview_active_ = true;
            cam.set_realism_settings(preview);
            if (camera_panel_) {
                camera_panel_->sync_debug_controls_from_settings(preview);
            }
            depth_guide_blue_wheel_last_change_ms_ = SDL_GetTicks();
            if (input_) input_->consumeEvent(event);
            return;
        }
        if (event.type == SDL_EVENT_MOUSE_BUTTON_UP && event.button.button == SDL_BUTTON_LEFT && depth_guide_drag_active_) {
            depth_guide_drag_active_ = false;
            if (depth_guide_preview_active_) {
                cam.set_realism_settings(depth_guide_preview_settings_);
                if (camera_panel_) {
                    camera_panel_->sync_debug_controls_from_settings(depth_guide_preview_settings_);
                }
                assets_->on_camera_settings_changed();
                mark_map_dirty();
                depth_guide_preview_active_ = false;
            }
            if (input_) input_->consumeEvent(event);
            return;
        }
        if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN &&
            ((event.button.button == SDL_BUTTON_RIGHT && !best) ||
             (event.button.button == SDL_BUTTON_LEFT && !best))) {
            clear_depth_guide_selection();
        }
    }
    if (!pointer_over_room_ui && map_mode_ui_) {
        if (depth_guide_selection_ != DepthGuideSelection::None && pointer_relevant) {
            if (input_) input_->consumeEvent(event);
            return;
        }
        const bool pointer_inside_map_mode = pointer_relevant && map_mode_ui_->is_point_inside(pointer.x, pointer.y);
        if (consume_if_handled(map_mode_ui_->handle_event(event), pointer_inside_map_mode)) {
            return;
        }
        if (pointer_inside_map_mode) {
            return;
        }
    }

    if (!(frame_editor_session_ && frame_editor_session_->is_active()) && can_route_room_editor) {
        const bool handled = room_editor_ && room_editor_->handle_sdl_event(event);
        if (handled && input_) {
            const bool pointer_inside_room_ui = pointer_relevant && room_editor_ && room_editor_->is_room_ui_blocking_point(pointer.x, pointer.y);
            if (!pointer_relevant || pointer_inside_room_ui) {
                input_->consumeEvent(event);
            }
        }
        if (handled) {
            return;
        }
    }
}

void DevControls::render_overlays(SDL_Renderer* renderer) {
    if (!enabled_) return;

    const bool layers_panel_open = map_mode_ui_ && map_mode_ui_->is_layers_panel_visible();
    const bool frame_editor_active = frame_editor_session_ && frame_editor_session_->is_active();
    const bool asset_editor_tab_active = room_editor_ && room_editor_->is_asset_editor_tab_scope_active();

    const bool hide_headers = modal_headers_hidden_ || sliding_headers_hidden_ || layers_panel_open ||
                              frame_editor_active || asset_editor_tab_active;
    other_settings_.set_header_suppressed(hide_headers);

    if (!renderer) {
        return;
    }

    if (frame_editor_active) {
        if (frame_editor_session_) {
            frame_editor_session_->render(renderer);
        }
        return;
    }

    auto try_floor_warped_screen_position = [&](const WarpedScreenGrid& c, SDL_Point w, SDL_FPoint& out) -> bool {
        SDL_FPoint linear{};
        SDL_FPoint floor_xy{static_cast<float>(w.x), 0.0f};
        const float depth_z = static_cast<float>(w.y);
        if (!c.project_world_point(floor_xy, depth_z, linear)) {
            return false;
        }
        float warped_y = c.warp_floor_screen_y(0.0f, linear.y);
        if (!std::isfinite(linear.x) || !std::isfinite(warped_y)) {
            return false;
        }
        out = SDL_FPoint{linear.x, warped_y};
        return true;
    };

    const bool show_depth_guides = camera_panel_ && camera_panel_->is_visible() && camera_panel_->is_debug_section_expanded();
    const bool show_grid_overlay = false; // Grid is now rendered by the OpenGL runtime renderer.
    std::optional<float> horizon_screen_y;
    std::optional<std::string> parallax_probe_label;
    const WarpedScreenGrid* cam = assets_ ? &assets_->getView() : nullptr;
    std::optional<WarpedScreenGrid::FloorDepthParams> floor_depth_params;
    if (cam) {
        floor_depth_params = cam->compute_floor_depth_params();
    }

    const bool need_grid_helpers = cam && (show_grid_overlay || show_depth_guides);
    if (renderer && need_grid_helpers) {
        const WarpedScreenGrid& view_cam = *cam;
        const WarpedScreenGrid::FloorDepthParams& depth_params = *floor_depth_params;
            world::WorldGrid& grid = assets_->world_grid();

            auto parallax_offset = [&](SDL_Point w) { return 0.0f; };

        SDL_BlendMode prev_mode = SDL_BLENDMODE_NONE;
        Uint8 pr = 0, pg = 0, pb = 0, pa = 0;
        if (show_grid_overlay) {
            SDL_GetRenderDrawBlendMode(renderer, &prev_mode);
            SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
            SDL_GetRenderDrawColor(renderer, &pr, &pg, &pb, &pa);
        }

        SDL_Color minor{0, 255, 255, 48};
        SDL_Color major{0, 255, 255, 80};

        auto [view_min_x, view_min_z, view_max_x, view_max_z] = view_cam.get_current_view().get_bounds();
        SDL_FPoint top_left_world{static_cast<float>(view_min_x), static_cast<float>(view_min_z)};
        SDL_FPoint bottom_right_world{static_cast<float>(view_max_x), static_cast<float>(view_max_z)};
        const float cam_scale = std::max(0.0001f, static_cast<float>(view_cam.get_scale()));

        int cell = std::max(1, grid_cell_size_px_);
        if (cell > 0) {
            const float world_padding = static_cast<float>(cell) * 4.0f;
            const float depth_world_padding = cam_scale * std::max(0.0f, view_cam.current_depth_offset_px());
            const float min_world_x = std::min(top_left_world.x, bottom_right_world.x) - world_padding;
            const float max_world_x = std::max(top_left_world.x, bottom_right_world.x) + world_padding;
            const float min_world_z = std::min(top_left_world.y, bottom_right_world.y) - world_padding - depth_world_padding * 0.5f;
            const float max_world_z = std::max(top_left_world.y, bottom_right_world.y) + world_padding + depth_world_padding;

            if (depth_params.enabled) {
                horizon_screen_y = static_cast<float>(depth_params.horizon_screen_y);
            }

            const int major_interval = 8;
            const int samples_per_line = 32;
            const float mid_world_x = (min_world_x + max_world_x) * 0.5f;

            float start_x = std::floor(min_world_x / cell) * cell;
            bool have_horizon_x = false;
            float best_horizon_x = 0.0f;
            const float screen_center_x = static_cast<float>(screen_w_) * 0.5f;
            auto draw_grid_polyline = [&](const std::vector<SDL_Point>& polyline, float line_value) {
                if (!show_grid_overlay || polyline.size() < 2) {
                    return;
                }
                const bool is_major = (static_cast<long long>(std::llround(line_value)) % (cell * major_interval) == 0);
                SDL_Color c = is_major ? major : minor;
                SDL_SetRenderDrawColor(renderer, c.r, c.g, c.b, c.a);
                sdl_render::Lines(renderer, polyline.data(), static_cast<int>(polyline.size()));
            };

            auto update_horizon_for_polyline = [&](const std::vector<SDL_Point>& polyline) {
                if (!depth_params.enabled || !horizon_screen_y || polyline.size() < 2) {
                    return;
                }
                const float hy = *horizon_screen_y;
                for (size_t i = 1; i < polyline.size(); ++i) {
                    const float y0 = static_cast<float>(polyline[i - 1].y);
                    const float y1 = static_cast<float>(polyline[i].y);
                    if ((y0 <= hy && hy <= y1) || (y1 <= hy && hy <= y0)) {
                        const float x0 = static_cast<float>(polyline[i - 1].x);
                        const float x1 = static_cast<float>(polyline[i].x);
                        if (std::fabs(y1 - y0) > 1e-6f) {
                            const float t = (hy - y0) / (y1 - y0);
                            const float ix = x0 + t * (x1 - x0);
                            const float dist = std::fabs(ix - screen_center_x);
                            if (!have_horizon_x || dist < std::fabs(best_horizon_x - screen_center_x)) {
                                have_horizon_x = true;
                                best_horizon_x = ix;
                            }
                        }
                    }
                }
            };

            for (float x = start_x; x <= max_world_x + cell; x += cell) {
                std::vector<SDL_Point> polyline;
                polyline.reserve(static_cast<std::size_t>(samples_per_line + 1));
                auto flush_polyline = [&]() {
                    draw_grid_polyline(polyline, x);
                    update_horizon_for_polyline(polyline);
                    polyline.clear();
                };

                for (int s = 0; s <= samples_per_line; ++s) {
                    const float t = static_cast<float>(s) / static_cast<float>(samples_per_line);
                    const float world_z = min_world_z + (max_world_z - min_world_z) * t;
                    SDL_Point world_point{
                        static_cast<int>(std::lround(x)), static_cast<int>(std::lround(world_z)) };
                    SDL_FPoint screen{};
                    if (!try_floor_warped_screen_position(view_cam, world_point, screen)) {
                        flush_polyline();
                        continue;
                    }
                    polyline.push_back(SDL_Point{
                        static_cast<int>(std::lround(screen.x)),
                        static_cast<int>(std::lround(screen.y))
                    });
                }
                flush_polyline();
            }

            if (have_horizon_x) {
                const int xi = static_cast<int>(std::lround(best_horizon_x));
                SDL_BlendMode prev_mode2 = SDL_BLENDMODE_NONE;
                SDL_GetRenderDrawBlendMode(renderer, &prev_mode2);
                SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
                SDL_SetRenderDrawColor(renderer, 255, 140, 0, 220);
                SDL_RenderLine(renderer, xi, 0, xi, screen_h_);
                SDL_SetRenderDrawBlendMode(renderer, prev_mode2);
            }

            float start_z = std::floor(max_world_z / cell) * cell;
            float highest_horizontal_screen_y = std::numeric_limits<float>::infinity();
            for (float z = start_z; z >= min_world_z - cell; z -= cell) {
                SDL_Point sample_world{
                    static_cast<int>(std::lround(mid_world_x)), static_cast<int>(std::lround(z)) };
                SDL_FPoint sample_screen{};
                if (try_floor_warped_screen_position(view_cam, sample_world, sample_screen)) {
                    const float screen_y = sample_screen.y;
                    if (std::isfinite(screen_y)) {
                        highest_horizontal_screen_y = std::min(highest_horizontal_screen_y, screen_y);
                    }
                }

                std::vector<SDL_Point> polyline;
                polyline.reserve(static_cast<std::size_t>(samples_per_line + 1));
                auto flush_polyline = [&]() {
                    draw_grid_polyline(polyline, z);
                    polyline.clear();
                };
                for (int s = 0; s <= samples_per_line; ++s) {
                    const float t = static_cast<float>(s) / static_cast<float>(samples_per_line);
                    const float wx = min_world_x + (max_world_x - min_world_x) * t;
                    SDL_Point world_point{
                        static_cast<int>(std::lround(wx)), static_cast<int>(std::lround(z)) };
                    SDL_FPoint screen{};
                    if (!try_floor_warped_screen_position(view_cam, world_point, screen)) {
                        flush_polyline();
                        continue;
                    }
                    polyline.push_back(SDL_Point{
                        static_cast<int>(std::lround(screen.x)),
                        static_cast<int>(std::lround(screen.y))
                    });
                }
                flush_polyline();
            }

            if (show_grid_overlay && horizon_screen_y) {
                const float hy = *horizon_screen_y;
                const bool already_at_horizon =
                    std::isfinite(highest_horizontal_screen_y) && std::fabs(highest_horizontal_screen_y - hy) < 0.5f;
                if (!already_at_horizon) {
                    SDL_Color c = major;
                    SDL_SetRenderDrawColor(renderer, c.r, c.g, c.b, c.a);
                    const int yi = static_cast<int>(std::lround(hy));
                    SDL_RenderLine(renderer, 0, yi, screen_w_, yi);
                }
            }

            if (depth_params.enabled) {

                horizon_screen_y = static_cast<float>(depth_params.horizon_screen_y);
            }


        }

        if (show_grid_overlay) {
            SDL_SetRenderDrawColor(renderer, pr, pg, pb, pa);
            SDL_SetRenderDrawBlendMode(renderer, prev_mode);
        }
    }

    if (renderer && show_grid_overlay && parallax_probe_label) {
        DMLabelStyle style = DMStyles::Label();
        const int text_x = DMSpacing::panel_padding();
        const int text_y = screen_h_ - style.font_size - DMSpacing::panel_padding();
        DrawLabelText(renderer, *parallax_probe_label, text_x, text_y, style);
    }

    if (renderer && show_depth_guides) {
        SDL_BlendMode prev_mode = SDL_BLENDMODE_NONE;
        SDL_GetRenderDrawBlendMode(renderer, &prev_mode);
        SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
        Uint8 pr = 0, pg = 0, pb = 0, pa = 0;
        SDL_GetRenderDrawColor(renderer, &pr, &pg, &pb, &pa);

        if (cam && floor_depth_params && floor_depth_params->enabled) {
            const auto settings = depth_guide_preview_active_ ? depth_guide_preview_settings_ : cam->get_settings();
            auto draw_guide = [&](float depth, SDL_Color color, const char* label, bool selected) {
                const auto y = project_depth_guide_screen_y(*cam, depth, screen_h_);
                if (!y.has_value()) return;
                const int line_y = static_cast<int>(std::lround(*y));
                const Uint8 alpha = static_cast<Uint8>(selected ? 255 : color.a);
                SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, alpha);
                SDL_RenderLine(renderer, 0, line_y, screen_w_, line_y);
                DMLabelStyle style = DMStyles::Label();
                style.color = SDL_Color{color.r, color.g, color.b, alpha};
                DrawLabelText(renderer, label, (screen_w_ / 2) + 12, std::max(0, line_y - style.font_size - 2), style);
            };
            const bool blue_mode = depth_guide_selection_ == DepthGuideSelection::BlueLayer;
            const bool show_red = !blue_mode && (depth_guide_selection_ == DepthGuideSelection::None || depth_guide_selection_ == DepthGuideSelection::RedCull);
            const bool show_orange = !blue_mode && (depth_guide_selection_ == DepthGuideSelection::None || depth_guide_selection_ == DepthGuideSelection::OrangeEfficiency);
            if (show_red) {
                draw_guide(settings.max_cull_depth, SDL_Color{255, 48, 48, 220}, "Max Cull Depth", depth_guide_selection_ == DepthGuideSelection::RedCull);
            }
            if (show_orange) {
                draw_guide(settings.dynamic_renderer_depth_efficiency_depth,
                           SDL_Color{255, 165, 0, 220},
                           "Dynamic Efficiency Depth",
                           depth_guide_selection_ == DepthGuideSelection::OrangeEfficiency);
            }
            if (depth_guide_selection_ == DepthGuideSelection::None || blue_mode) {
                const double max_cull = std::max(1.0, static_cast<double>(settings.max_cull_depth));
                const double interval = std::max(1.0, static_cast<double>(settings.layer_depth_interval));
                const double curve = std::max(0.0, static_cast<double>(settings.layer_depth_curve));
                for (double edge : render_depth::build_background_depth_edges(max_cull, interval, curve)) {
                    if (edge <= 0.0) continue;
                    draw_guide(static_cast<float>(edge),
                               SDL_Color{80, 160, 255, static_cast<Uint8>(blue_mode ? 220 : 120)},
                               "",
                               false);
                }
            }
        }

        SDL_SetRenderDrawColor(renderer, pr, pg, pb, pa);
        SDL_SetRenderDrawBlendMode(renderer, prev_mode);
    }

    if (renderer && room_editor_) {
        room_editor_->render_overlays(renderer);

        if (frame_editor_session_ && frame_editor_session_->is_active()) {
            frame_editor_session_->render(renderer);
        }
    }
    if (renderer && camera_panel_ && camera_panel_->is_visible() && assets_) {
        const WarpedScreenGrid& cam = assets_->getView();
        SDL_FPoint center_world_f = cam.get_view_center_f();
        SDL_FPoint center_screen_f = cam.map_to_screen_f(center_world_f);
        const int cx = static_cast<int>(std::lround(center_screen_f.x));
        const int cy = static_cast<int>(std::lround(center_screen_f.y));

        SDL_BlendMode prev_mode = SDL_BLENDMODE_NONE;
        SDL_GetRenderDrawBlendMode(renderer, &prev_mode);
        Uint8 pr = 0, pg = 0, pb = 0, pa = 0;
        SDL_GetRenderDrawColor(renderer, &pr, &pg, &pb, &pa);
        SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);

        const SDL_Color c = DMStyles::AccentButton().hover_bg;
        SDL_SetRenderDrawColor(renderer, c.r, c.g, c.b, 230);

        constexpr int arm = 8;
        constexpr int thickness = 3;
        const int offset_start = -thickness / 2;
        const int offset_end   =  thickness / 2;
        for (int o = offset_start; o <= offset_end; ++o) {
            SDL_RenderLine(renderer, cx - arm, cy + o, cx + arm, cy + o);
            SDL_RenderLine(renderer, cx + o, cy - arm, cx + o, cy + arm);
        }

        SDL_SetRenderDrawColor(renderer, pr, pg, pb, pa);
        SDL_SetRenderDrawBlendMode(renderer, prev_mode);
    }

    if (renderer && map_mode_ui_) map_mode_ui_->render(renderer);
    render_misc_options_panel(renderer);
    if (renderer && boundary_assets_modal_ && boundary_assets_modal_->visible()) {
        boundary_assets_modal_->render(renderer);
    }
    
    if (frame_editor_session_ && frame_editor_session_->is_active()) {

    }
    if (renderer && camera_panel_ && camera_panel_->is_visible()) {
        camera_panel_->render(renderer);
    }
    if (renderer && regenerate_popup_ && regenerate_popup_->visible()) {
        regenerate_popup_->render(renderer);
    }
    if (renderer && !hide_headers && !is_modal_blocking_panels()) {

        other_settings_.set_right_accessory_width(0);
        other_settings_.render(renderer);
    }

    render_drop_overlay(renderer);
    render_import_busy_overlay(renderer);
    render_drop_modal(renderer);
    render_drop_choice_modal(renderer);
    render_drop_conflict_modal(renderer);
    render_drop_error_popup(renderer);
}

void DevControls::begin_frame_editor_session(Asset* asset,
                                             std::shared_ptr<animation_editor::AnimationDocument> document,
                                             std::shared_ptr<animation_editor::PreviewProvider> preview,
                                             const std::string& animation_id,
                                             FrameEditorLaunchMode launch_mode,
                                             std::function<void(const std::string&)> on_host_closed) {
    if (!asset || !assets_ || animation_id.empty()) return;
    if (!frame_editor_session_) frame_editor_session_ = std::make_unique<FrameEditorSession>();
    frame_editor_session_->set_snap_resolution(grid_overlay_resolution_r_);

    frame_editor_prev_grid_overlay_ = grid_overlay_enabled_;
    sync_grid_overlay_enabled(true, true);
    other_settings_.set_setting_value(OtherSettingsAndControls::kShowGridSettingId, grid_overlay_enabled_);

    frame_editor_prev_asset_info_open_ = false;
    frame_editor_asset_for_reopen_ = nullptr;
    const bool launched_from_animation_editor = static_cast<bool>(on_host_closed);
    bool asset_info_was_open = false;
    if (room_editor_) {
        asset_info_was_open = room_editor_->is_asset_info_editor_open();
        if (asset_info_was_open) {
            room_editor_->close_asset_info_editor();
        }
    }
    frame_editor_prev_asset_info_open_ = asset_info_was_open || launched_from_animation_editor;
    if (frame_editor_prev_asset_info_open_) {
        frame_editor_asset_for_reopen_ = asset;
    }
    frame_editor_session_->begin(assets_,
                                 asset,
                                 std::move(document),
                                 std::move(preview),
                                 animation_id,
                                 launch_mode,
                                 parent_window_,
                                 std::move(on_host_closed),
                                 [this]() {

        this->sync_grid_overlay_enabled(this->frame_editor_prev_grid_overlay_, true);
        other_settings_.set_setting_value(OtherSettingsAndControls::kShowGridSettingId, this->grid_overlay_enabled_);

        if (this->frame_editor_prev_asset_info_open_ && this->room_editor_ && this->frame_editor_asset_for_reopen_) {
            this->room_editor_->open_asset_info_editor_for_asset(this->frame_editor_asset_for_reopen_);
        }
        this->frame_editor_prev_asset_info_open_ = false;
        this->frame_editor_asset_for_reopen_ = nullptr;
        this->apply_header_suppression();
    },
    [this]() {
        this->persist_map_info_to_disk();
    });
    apply_header_suppression();
}

void DevControls::end_frame_editor_session() {
    if (frame_editor_session_) {
        frame_editor_session_->end();
    }
    sync_grid_overlay_enabled(frame_editor_prev_grid_overlay_, true);
    other_settings_.set_setting_value(OtherSettingsAndControls::kShowGridSettingId, grid_overlay_enabled_);
    apply_header_suppression();
}

bool DevControls::is_frame_editor_session_active() const {
    return frame_editor_session_ && frame_editor_session_->is_active();
}

bool DevControls::is_runtime_light_editor_active() const {
    if (!room_editor_) {
        return false;
    }
    return room_editor_->is_light_edit_mode_active();
}

Assets::DevGridOverlayContext DevControls::dev_grid_overlay_context() const {
    if (!room_editor_ || !room_editor_->is_enabled()) {
        return {};
    }
    return room_editor_->dev_grid_overlay_context();
}

std::vector<Assets::DevFloorProjectionMarker> DevControls::floor_projection_markers_for_floor_pass() {
    if (!room_editor_ || !room_editor_->is_enabled()) {
        return {};
    }
    return room_editor_->floor_projection_markers_for_floor_pass();
}

const Asset* DevControls::frame_editor_target() const {
    if (!frame_editor_session_ || !frame_editor_session_->is_active()) {
        return nullptr;
    }
    return frame_editor_session_->target_asset();
}

void DevControls::toggle_asset_library() {
    if (!can_use_room_editor_ui()) return;
    room_editor_->toggle_asset_library();
    sync_header_button_states();
}

void DevControls::open_asset_library() {
    if (!can_use_room_editor_ui()) return;
    room_editor_->open_asset_library();
    sync_header_button_states();
}

void DevControls::close_asset_library() {
    if (room_editor_) room_editor_->close_asset_library();
    sync_header_button_states();
}

bool DevControls::is_asset_library_open() const {
    if (!room_editor_) return false;
    return room_editor_->is_asset_library_open();
}

std::shared_ptr<AssetInfo> DevControls::consume_selected_asset_from_library() {
    if (!can_use_room_editor_ui()) return nullptr;
    return room_editor_->consume_selected_asset_from_library();
}

void DevControls::open_asset_info_editor(const std::shared_ptr<AssetInfo>& info) {
    if (!can_use_room_editor_ui()) return;
    room_editor_->open_asset_info_editor(info);
}

void DevControls::open_asset_info_editor_for_asset(Asset* asset) {
    if (!can_use_room_editor_ui()) return;
    room_editor_->open_asset_info_editor_for_asset(asset);
}

void DevControls::open_animation_editor_for_asset(const std::shared_ptr<AssetInfo>& info) {
    if (!can_use_room_editor_ui()) return;
    room_editor_->open_animation_editor_for_asset(info);
}

void DevControls::close_asset_info_editor() {
    if (room_editor_) room_editor_->close_asset_info_editor();

    end_frame_editor_session();
}

bool DevControls::consume_escape_for_asset_editor_stack() {
    if (!can_use_room_editor_ui() || !room_editor_) {
        return false;
    }
    const bool consumed = room_editor_->consume_escape_for_asset_editor_stack();
    if (consumed) {
        end_frame_editor_session();
    }
    return consumed;
}

bool DevControls::is_asset_info_editor_open() const {
    if (!room_editor_) return false;
    return room_editor_->is_asset_info_editor_open();
}

std::uint64_t DevControls::other_settings_state_version() const {
    return other_settings_.state_version();
}

void DevControls::finalize_asset_drag(Asset* asset, const std::shared_ptr<AssetInfo>& info) {
    if (!can_use_room_editor_ui()) return;
    room_editor_->finalize_asset_drag(asset, info);
}

void DevControls::toggle_room_config() {
    if (!can_use_room_editor_ui()) return;
    if (room_editor_) {
        room_editor_->toggle_room_config();
    }
    sync_header_button_states();
}

void DevControls::close_room_config() {
    if (room_editor_) {
        room_editor_->close_room_config();
    }
    sync_header_button_states();
}

bool DevControls::is_room_config_open() const {
    return room_editor_ && room_editor_->is_room_config_open();
}

void DevControls::focus_camera_on_asset(Asset* asset, double height_factor, int duration_steps) {
    if (!room_editor_) return;
    room_editor_->focus_camera_on_asset(asset, height_factor, duration_steps);
}

void DevControls::reset_click_state() {
    if (room_editor_) room_editor_->reset_click_state();
}

void DevControls::clear_selection() {
    if (room_editor_) room_editor_->clear_selection();
}

void DevControls::purge_asset(Asset* asset) {
    if (!room_editor_) return;
    room_editor_->purge_asset(asset);
}

void DevControls::set_world_mutation_in_progress(bool in_progress) {
    world_mutation_in_progress_ = in_progress;
    if (!room_editor_) {
        return;
    }
    room_editor_->set_pointer_queries_suspended(in_progress);
    if (!in_progress) {
        pending_selection_sync_refresh_ = true;
    }
}

void DevControls::notify_spawn_group_config_changed(const nlohmann::json& entry) {
    if (room_editor_) {
        room_editor_->handle_spawn_config_change(entry);
    }
}

void DevControls::notify_spawn_group_removed(const std::string& spawn_id) {
    remove_spawn_group_assets(spawn_id);

    Asset::ClearFlipOverrideForSpawnId(spawn_id);
}

void DevControls::mark_map_dirty(devmode::core::DevSaveCoordinator::Priority priority) {
    if (assets_) {
        assets_->mark_map_data_dirty();
    }
    map_dirty_ = true;
    map_info_dirty_ = true;
    if (priority == devmode::core::DevSaveCoordinator::Priority::Immediate) {
        save_manager_.save_dirty_with_reason(priority,
                                             devmode::core::SaveOrchestrator::Reason::StateChange,
                                             "Immediate map change");
    }
}

const std::vector<Asset*>& DevControls::get_selected_assets() const {
    static std::vector<Asset*> empty;
    if (world_mutation_in_progress_ || !can_use_room_editor_ui()) return empty;
    return room_editor_->get_selected_assets();
}

const std::vector<Asset*>& DevControls::get_highlighted_assets() const {
    static std::vector<Asset*> empty;
    if (world_mutation_in_progress_ || !can_use_room_editor_ui()) return empty;
    return room_editor_->get_highlighted_assets();
}

Asset* DevControls::get_hovered_asset() const {
    if (world_mutation_in_progress_ || !can_use_room_editor_ui()) return nullptr;
    return room_editor_->get_hovered_asset();
}

void DevControls::set_height_scale_factor(double factor) {
    if (room_editor_) room_editor_->set_height_scale_factor(factor);
}

double DevControls::get_height_scale_factor() const {
    if (!room_editor_) return 1.0;
    return room_editor_->get_height_scale_factor();
}

void DevControls::configure_header_button_sets() {
    if (!map_mode_ui_) return;

    auto make_camera_button = [this]() {
        MapModeUI::HeaderButtonConfig camera_btn;
        camera_btn.id = "camera";
        camera_btn.label = "Camera";
        camera_btn.active = camera_panel_ && camera_panel_->is_visible();
        camera_btn.group = FooterButtonGroup::Primary;
        camera_btn.style_override = &DMStyles::HeaderButton();
        camera_btn.active_style_override = &DMStyles::AccentButton();
        camera_btn.on_toggle = [this](bool active) {
            if (room_editor_) {
                room_editor_->close_room_config();
            }
            if (active) {
                close_misc_options_panel();
            }
            if (!camera_panel_) {
                sync_header_button_states();
                return;
            }
            camera_panel_->set_assets(assets_);
            if (camera_panel_->is_visible() != active) {
                toggle_camera_panel();
            } else {
                sync_header_button_states();
            }
};
        return camera_btn;
};

    auto make_layers_button = [this]() {
        MapModeUI::HeaderButtonConfig layers_btn;
        layers_btn.id = "layers";
        layers_btn.label = "Layers";
        const bool layers_visible = map_mode_ui_ && map_mode_ui_->is_layers_panel_visible();
        layers_btn.active = layers_visible;
        layers_btn.group = FooterButtonGroup::Primary;
        layers_btn.style_override = &DMStyles::HeaderButton();
        layers_btn.active_style_override = &DMStyles::AccentButton();
        layers_btn.on_toggle = [this](bool active) {
            if (room_editor_) {
                room_editor_->close_room_config();
            }
            if (active) {
                close_misc_options_panel();
            }
            if (!map_mode_ui_) {
                sync_header_button_states();
                return;
            }
            const bool currently_open = map_mode_ui_->is_layers_panel_visible();
            if (active != currently_open) {
                if (active && !currently_open && is_modal_blocking_panels()) {
                    pulse_modal_header();
                    sync_header_button_states();
                    return;
                }
                if (active) {
                    map_mode_ui_->open_layers_panel();
                } else {
                    map_mode_ui_->toggle_layers_panel();
                }
            } else if (active) {
                map_mode_ui_->open_layers_panel();
            }
            sync_header_button_states();
};
        return layers_btn;
    };

    std::vector<MapModeUI::HeaderButtonConfig> room_buttons;

    room_buttons.push_back(make_camera_button());
    room_buttons.push_back(make_layers_button());

    {
        MapModeUI::HeaderButtonConfig boundary_btn;
        boundary_btn.id = "map_boundary";
        boundary_btn.label = "Live Dynamic Spawns";
        boundary_btn.active = (boundary_assets_modal_ && boundary_assets_modal_->visible());
        boundary_btn.group = FooterButtonGroup::Panels;
        boundary_btn.style_override = &DMStyles::ListButton();
        boundary_btn.active_style_override = &DMStyles::AccentButton();
        boundary_btn.on_toggle = [this](bool active) {
            if (active) {
                close_misc_options_panel();
                toggle_boundary_assets_modal();
            } else {
                if (room_editor_) room_editor_->clear_selection();
                if (boundary_assets_modal_) boundary_assets_modal_->close();
            }
            sync_header_button_states();
};
        room_buttons.push_back(std::move(boundary_btn));
    }

    MapModeUI::HeaderButtonConfig room_config_btn;
    room_config_btn.id = "room_config";
    room_config_btn.label = "Room Config";
    room_config_btn.active = false;
    room_config_btn.group = FooterButtonGroup::Panels;
    room_config_btn.style_override = &DMStyles::ListButton();
    room_config_btn.active_style_override = &DMStyles::AccentButton();
    room_config_btn.on_toggle = [this](bool active) {
        if (!room_editor_) {
            sync_header_button_states();
            return;
        }
        if (active) {
            close_misc_options_panel();
            room_editor_->open_room_config();
        } else {
            room_editor_->close_room_config();
        }
        sync_header_button_states();
};
    room_buttons.push_back(std::move(room_config_btn));

    MapModeUI::HeaderButtonConfig create_room_btn;
    create_room_btn.id = "create_room";
    create_room_btn.label = "Create Room";
    create_room_btn.momentary = true;
    create_room_btn.group = FooterButtonGroup::Actions;
    create_room_btn.style_override = &DMStyles::CreateButton();
    create_room_btn.active_style_override = &DMStyles::AccentButton();
    create_room_btn.on_toggle = [this](bool) {
        if (room_editor_) {
            room_editor_->create_room_from_footer();
            room_editor_->open_room_config();
        }
        sync_header_button_states();
    };
    room_buttons.push_back(std::move(create_room_btn));

    MapModeUI::HeaderButtonConfig create_trail_btn;
    create_trail_btn.id = "create_trail";
    create_trail_btn.label = "Create Trail";
    create_trail_btn.momentary = true;
    create_trail_btn.group = FooterButtonGroup::Actions;
    create_trail_btn.style_override = &DMStyles::CreateButton();
    create_trail_btn.active_style_override = &DMStyles::AccentButton();
    create_trail_btn.on_toggle = [this](bool) {
        if (room_editor_) {
            room_editor_->create_trail_from_footer();
            room_editor_->open_room_config();
        }
        sync_header_button_states();
    };
    room_buttons.push_back(std::move(create_trail_btn));

    MapModeUI::HeaderButtonConfig misc_options_btn;
    misc_options_btn.id = "misc_options";
    misc_options_btn.label = "Misc Options";
    misc_options_btn.active = is_misc_options_panel_open();
    misc_options_btn.group = FooterButtonGroup::Panels;
    misc_options_btn.style_override = &DMStyles::ListButton();
    misc_options_btn.active_style_override = &DMStyles::AccentButton();
    misc_options_btn.on_toggle = [this](bool active) {
        if (active) {
            open_misc_options_panel();
        } else {
            close_misc_options_panel();
        }
        sync_header_button_states();
    };
    room_buttons.push_back(std::move(misc_options_btn));

    MapModeUI::HeaderButtonConfig library_btn;
    library_btn.id = "asset_library";
    library_btn.label = "Asset Library";
    library_btn.active = room_editor_ && room_editor_->is_asset_library_open();
    library_btn.group = FooterButtonGroup::Panels;
    library_btn.style_override = &DMStyles::ListButton();
    library_btn.active_style_override = &DMStyles::AccentButton();
    library_btn.on_toggle = [this](bool active) {
        if (!room_editor_) return;
        room_editor_->close_room_config();
        if (active) {
            close_misc_options_panel();
            room_editor_->open_asset_library();
        } else {
            room_editor_->close_asset_library();
        }
        sync_header_button_states();
};
    room_buttons.push_back(std::move(library_btn));

    MapModeUI::HeaderButtonConfig regenerate_btn;
    regenerate_btn.id = "regenerate";
    regenerate_btn.label = "regen";
    regenerate_btn.momentary = true;
    regenerate_btn.style_override = &DMStyles::DeleteButton();
    regenerate_btn.group = FooterButtonGroup::Actions;
    regenerate_btn.active_style_override = &DMStyles::AccentButton();
    regenerate_btn.on_toggle = [this](bool) {
        if (!room_editor_) {
            sync_header_button_states();
            return;
        }
        room_editor_->close_room_config();
        if (is_modal_blocking_panels()) {
            pulse_modal_header();
            sync_header_button_states();
            return;
        }
        close_misc_options_panel();
        open_regenerate_room_popup();
        sync_header_button_states();
};
    room_buttons.push_back(std::move(regenerate_btn));

    MapModeUI::HeaderButtonConfig explorer_btn;
    explorer_btn.id = "open_explorer";
    explorer_btn.label = "Open Explorer";
    explorer_btn.momentary = true;
    explorer_btn.group = FooterButtonGroup::Utilities;
    explorer_btn.style_override = &DMStyles::SecondaryButton();
    explorer_btn.active_style_override = &DMStyles::AccentButton();
    explorer_btn.on_toggle = [this](bool) {
        open_repo_root_in_file_explorer();
        sync_header_button_states();
    };
    room_buttons.push_back(std::move(explorer_btn));

    map_mode_ui_->set_mode_button_sets({}, std::move(room_buttons));
    other_settings_.ensure_layout();
    sync_header_button_states();
}

void DevControls::sync_header_button_states() {
    if (!map_mode_ui_) {
        if (room_editor_) {
            room_editor_->set_room_trail_nav_visibility(false);
        }
        return;
    }
    const bool room_config_open = room_editor_ && room_editor_->is_room_config_open();
    map_mode_ui_->set_button_state(MapModeUI::HeaderMode::Room, "room_config", room_config_open);
    map_mode_ui_->set_button_state(MapModeUI::HeaderMode::Room, "misc_options", is_misc_options_panel_open());
    const bool library_open = room_editor_ && room_editor_->is_asset_library_open();
    map_mode_ui_->set_button_state(MapModeUI::HeaderMode::Room, "asset_library", library_open);
    const bool camera_open = camera_panel_ && camera_panel_->is_visible();
    map_mode_ui_->set_button_state(MapModeUI::HeaderMode::Room, "camera", camera_open);
    const bool layers_open = map_mode_ui_->is_layers_panel_visible();
    map_mode_ui_->set_button_state(MapModeUI::HeaderMode::Room, "layers", layers_open);
    map_mode_ui_->set_button_state(MapModeUI::HeaderMode::Room, "regenerate", false);
    map_mode_ui_->set_button_state(MapModeUI::HeaderMode::Room, "create_room", false);
    map_mode_ui_->set_button_state(MapModeUI::HeaderMode::Room, "create_trail", false);

    const bool boundary_open = boundary_assets_modal_ && boundary_assets_modal_->visible();
    map_mode_ui_->set_button_state(MapModeUI::HeaderMode::Room, "map_boundary", boundary_open);
    

    if (room_editor_) {
        room_editor_->set_blocking_panel_visible(RoomEditor::BlockingPanel::Camera, camera_open);
        room_editor_->set_blocking_panel_visible(RoomEditor::BlockingPanel::AssetLibrary, library_open);
        room_editor_->set_blocking_panel_visible(RoomEditor::BlockingPanel::MapLayers, layers_open);

        const bool map_ui_panels_open = map_mode_ui_ && map_mode_ui_->is_any_panel_visible();
        const bool room_editor_active =
            enabled_ && mode_ == Mode::RoomEditor && room_editor_->is_enabled();
        const bool external_selection_blocked =
            !room_editor_active ||
            camera_open ||
            layers_open ||
            boundary_open ||
            misc_options_panel_open_ ||
            (regenerate_popup_ && regenerate_popup_->visible()) ||
            map_ui_panels_open ||
            is_modal_blocking_panels();
        room_editor_->set_external_asset_selection_blocked(external_selection_blocked);

        const bool any_modal_open =
            room_config_open ||
            room_editor_->is_asset_info_editor_open() ||
            (regenerate_popup_ && regenerate_popup_->visible()) ||
            false ||
            boundary_open ||
            camera_open ||
            misc_options_panel_open_ ||
            map_ui_panels_open;
        const bool nav_buttons_enabled =
            enabled_ &&
            mode_ == Mode::RoomEditor &&
            room_editor_->is_enabled() &&
            !room_editor_->is_asset_stack_editor_active() &&
            !room_editor_->is_asset_editor_tab_scope_active() &&
            !room_editor_->any_blocking_panel_visible() &&
            !any_modal_open;
        room_editor_->set_room_trail_nav_visibility(nav_buttons_enabled);
    }

}

void DevControls::close_all_floating_panels() {
    if (room_editor_) {
        room_editor_->close_asset_library();
        room_editor_->close_asset_info_editor();
    }
    if (camera_panel_) {
        camera_panel_->close();
    }
    if (map_mode_ui_) {
        map_mode_ui_->close_all_panels();
    }
    if (boundary_assets_modal_) {
        if (room_editor_) room_editor_->clear_selection();
        boundary_assets_modal_->close();
    }
    
    close_misc_options_panel();
    
    if (regenerate_popup_) {
        regenerate_popup_->close();
    }
    sync_header_button_states();
}

void DevControls::maybe_update_mode_from_height() {}

bool DevControls::is_modal_blocking_panels() const {
    const bool room_modal = room_editor_ && room_editor_->has_active_modal();
    return room_modal;
}

void DevControls::pulse_modal_header() {
    if (room_editor_) {
        room_editor_->pulse_active_modal_header();
    }
}

void DevControls::apply_header_suppression() {
    if (map_mode_ui_) {

        const bool frame_editor_active = frame_editor_session_ && frame_editor_session_->is_active();
        const bool asset_editor_tab_active = room_editor_ && room_editor_->is_asset_editor_tab_scope_active();
        const bool modal_hide = is_modal_blocking_panels() || frame_editor_active || asset_editor_tab_active;
        const bool header_block = modal_hide || shift_block_headers_footers_;
        map_mode_ui_->set_headers_suppressed(header_block);
        map_mode_ui_->set_dev_sliding_headers_hidden(sliding_headers_hidden_);
        if (auto* footer = map_mode_ui_->get_footer_bar()) {
            const bool footer_enabled = !frame_editor_active && !shift_block_headers_footers_;
            footer->set_visible(footer_enabled);
            footer->set_input_enabled(footer_enabled);
        }
    }
}

void DevControls::mark_dirty(std::uint32_t flags) {
    dirty_flags_ |= flags;
}

bool DevControls::has_dirty(std::uint32_t flags) const {
    return (dirty_flags_ & flags) != 0;
}

void DevControls::clear_dirty(std::uint32_t flags) {
    dirty_flags_ &= ~flags;
}

void DevControls::mark_layout_dirty() {
    mark_dirty(kDirtyLayout);
    layout_cache_.valid = false;
}

void DevControls::update_header_and_footer_bounds() {
    const bool frame_editor_active = frame_editor_session_ && frame_editor_session_->is_active();
    const bool asset_editor_tab_active = room_editor_ && room_editor_->is_asset_editor_tab_scope_active();
    const bool modal_hide = is_modal_blocking_panels() || frame_editor_active || asset_editor_tab_active;
    modal_headers_hidden_ = modal_hide;
    const bool layers_panel_open = map_mode_ui_ && map_mode_ui_->is_layers_panel_visible();
    const bool hide_headers = modal_hide || sliding_headers_hidden_ || layers_panel_open || shift_block_headers_footers_;
    if (hide_headers) {
        last_header_rect_ = SDL_Rect{0, 0, 0, 0};
    } else {
        last_header_rect_ = other_settings_.header_rect();
    }
    if (map_mode_ui_) {
        if (auto* footer = map_mode_ui_->get_footer_bar()) {
            if (footer->visible() && !shift_block_headers_footers_) {
                last_footer_rect_ = footer->rect();
            } else {
                last_footer_rect_ = SDL_Rect{0, 0, 0, 0};
            }
        } else {
            last_footer_rect_ = SDL_Rect{0, 0, 0, 0};
        }
    } else {
        last_footer_rect_ = SDL_Rect{0, 0, 0, 0};
    }
}

void DevControls::rebuild_layout_state() {
    SDL_Rect layout_rect = other_settings_.layout_bounds();

    std::vector<SDL_Rect> sliding_rects;
    if (map_mode_ui_) {
        map_mode_ui_->collect_sliding_container_rects(sliding_rects);
    }
    if (layout_rect.w > 0 && layout_rect.h > 0) {
        sliding_rects.push_back(layout_rect);
    }

    SDL_Rect bounds{0, 0, screen_w_, screen_h_};
    SDL_Rect usable = DockManager::instance().computeUsableRect(
        bounds,
        last_header_rect_,
        last_footer_rect_,
        sliding_rects);
    layout_cache_.usable_rect = usable;
    layout_cache_.valid = true;
    if (map_mode_ui_) {
        map_mode_ui_->set_sliding_area_bounds(usable);
    }
}

void DevControls::ensure_layout_cache() {
    const bool needs_rebuild = !layout_cache_.valid || has_dirty(kDirtyLayout);
    if (needs_rebuild) {
        other_settings_.ensure_layout();
        update_header_and_footer_bounds();
        rebuild_layout_state();
        clear_dirty(kDirtyLayout);
    } else {
        update_header_and_footer_bounds();
    }
}

int DevControls::map_radius_or_default() const {
    if (!assets_) {
        return 1000;
    }
    int radius = 0;
    try {
        const nlohmann::json& map_json = assets_->map_info_json();
        if (map_json.is_object()) {
            const double computed = map_layers::map_radius_from_map_info(map_json);
            if (computed > 0.0) {
                radius = static_cast<int>(std::lround(computed));
            }
        }
    } catch (...) {
        radius = 0;
    }
    if (radius <= 0) {
        const auto& rooms = assets_->rooms();
        for (Room* room : rooms) {
            if (!room || !room->room_area) {
                continue;
            }
            auto [minx, miny, maxx, maxy] = room->room_area->get_bounds();
            int extent = 0;
            extent = std::max(extent, std::abs(minx));
            extent = std::max(extent, std::abs(miny));
            extent = std::max(extent, std::abs(maxx));
            extent = std::max(extent, std::abs(maxy));
            radius = std::max(radius, extent);
        }
    }
    if (radius <= 0) {
        radius = 1000;
    }
    return radius;
}

void DevControls::remove_spawn_group_assets(const std::string& spawn_id) {
    if (!assets_ || spawn_id.empty()) {
        return;
    }

    auto batch = assets_->begin_world_mutation_batch();
    std::vector<Asset*> candidates;
    candidates.reserve(assets_->all.size());
    for (Asset* asset : assets_->all) {
        if (!asset || asset->dead || asset == assets_->player) {
            continue;
        }
        if (asset->spawn_id != spawn_id) {
            continue;
        }
        candidates.push_back(asset);
    }

    for (Asset* asset : candidates) {
        purge_asset(asset);
        batch.mark_for_deletion(asset);
    }

    batch.set_pre_commit_save([this]() {
        mark_map_dirty(devmode::core::DevSaveCoordinator::Priority::Debounced);
        return true;
    });

    const bool committed = batch.commit();
    if (!committed) {
        return;
    }
    if (pending_selection_sync_refresh_) {
        pending_selection_sync_refresh_ = false;
        if (room_editor_) {
            room_editor_->clear_highlighted_assets();
        }
    }
}

void DevControls::integrate_spawned_assets(std::vector<std::unique_ptr<Asset>>& spawned) {
    if (!assets_ || spawned.empty()) {
        return;
    }
    for (auto& uptr : spawned) {
        if (!uptr) {
            continue;
        }
        Asset* raw = uptr.get();
        set_camera_recursive(raw, &assets_->getView());
        set_assets_owner_recursive(raw, assets_);
        raw->finalize_setup();
        raw = assets_->world_grid().create_asset_at_point(std::move(uptr));
        if (raw) {
            assets_->all.push_back(raw);
        }
    }
    spawned.clear();
    const SDL_Point center_px = assets_->getView().get_screen_center();
    const world::GridPoint center_point = world::GridPoint::make_virtual(
        center_px.x, 0, center_px.y, assets_->world_grid().max_resolution_layers());
    assets_->initialize_active_assets(center_point);
    assets_->refresh_active_asset_lists();
    refresh_active_asset_filters();
}

void DevControls::regenerate_boundary_spawn_group(const nlohmann::json& entry) {
    (void)entry;
    if (assets_) {
        assets_->rebuild_dynamic_spawn_runtime_from_map();
    }
}

void DevControls::apply_camera_area_render_flag() {
    WarpedScreenGrid* cam_ptr = nullptr;
    if (camera_override_for_testing_) {
        cam_ptr = camera_override_for_testing_;
    } else if (assets_) {
        cam_ptr = &assets_->getView();
    }

    if (!cam_ptr) {
        return;
    }

    cam_ptr->set_render_areas_enabled(false);
}

void DevControls::set_mode(Mode new_mode) {
    if (new_mode == Mode::MapEditor) {
        new_mode = Mode::RoomEditor;
    }
    if (mode_ == new_mode) {
        return;
    }
    mode_ = new_mode;
    other_settings_.set_active_mode(kModeIdRoom);
    apply_camera_area_render_flag();

    mark_layout_dirty();
    update_movement_debug_visibility();
}

void DevControls::update_movement_debug_visibility() {
    (void)enabled_;
    (void)movement_debug_enabled_;
    if (assets_) {
        bool suppress_impass_floor_debug = false;
        if (room_editor_) {
            suppress_impass_floor_debug = room_editor_->impassable_box_mode_active();
        }
        assets_->set_impass_floor_debug_visible(!suppress_impass_floor_debug);
    }
}

void DevControls::restore_filter_hidden_assets() const {
    for (auto& kv : filter_hidden_assets_) {
        if (Asset* asset = kv.first) {
            asset->set_hidden(kv.second.hidden);
            asset->active = kv.second.active;
        }
    }
    filter_hidden_assets_.clear();
    previous_filtered_membership_.clear();
}

void DevControls::ensure_boundary_assets_modal_open() {
    if (!assets_) return;
    if (!boundary_assets_modal_) {
        boundary_assets_modal_ = std::make_unique<BoundarySpawnGroupModal>();
        boundary_assets_modal_->set_screen_dimensions(screen_w_, screen_h_);
        boundary_assets_modal_->set_floating_stack_key("live_dynamic_spawns_modal");
    } else {
        boundary_assets_modal_->set_screen_dimensions(screen_w_, screen_h_);
    }
    boundary_assets_modal_->set_manifest_store(&manifest_store_);
    boundary_assets_modal_->set_assets(assets_);
    boundary_assets_modal_->set_on_close([this]() {
        if (room_editor_) room_editor_->clear_selection();
        this->sync_header_button_states();
    });
    auto save = [this]() { mark_map_dirty(devmode::core::DevSaveCoordinator::Priority::Debounced); return true; };
    auto regen = [this](const nlohmann::json& entry) { this->regenerate_boundary_spawn_group(entry); };
    auto& map_json = assets_->map_info_json();
    SDL_Color color{255, 200, 120, 255};
    boundary_assets_modal_->open(map_json, "live_dynamic_spawns", "batch_map_boundary", "Boundary Area", color, save, regen);
}

void DevControls::open_boundary_assets_modal() {
    if (boundary_assets_modal_ && boundary_assets_modal_->visible()) {
        boundary_assets_modal_->set_screen_dimensions(screen_w_, screen_h_);
    } else {
        ensure_boundary_assets_modal_open();
    }
    sync_header_button_states();
}

void DevControls::toggle_boundary_assets_modal() {
    if (boundary_assets_modal_ && boundary_assets_modal_->visible()) {
        if (room_editor_) room_editor_->clear_selection();
        boundary_assets_modal_->close();
    } else {
        ensure_boundary_assets_modal_open();
    }
    sync_header_button_states();
}

void DevControls::create_trail_template() {
    if (assets_) assets_->show_dev_notice("Trail creation from this panel is disabled", 1400);
    sync_header_button_states();
}

void DevControls::open_regenerate_room_popup() {
    if (!can_use_room_editor_ui()) return;
    if (!room_editor_ || !current_room_) {
        if (regenerate_popup_) regenerate_popup_->close();
        return;
    }

    if (is_trail_room(current_room_)) {
        if (assets_) {
            assets_->show_dev_notice("Regen is disabled for trail rooms", 1600);
        }
        if (regenerate_popup_) regenerate_popup_->close();
        return;
    }

    if (!assets_) {
        if (regenerate_popup_) regenerate_popup_->close();
        return;
    }

    nlohmann::json& map_info = assets_->map_info_json();
    nlohmann::json& rooms_data = map_info["rooms_data"];
    if (!rooms_data.is_object()) {
        if (assets_) {
            assets_->show_dev_notice("No room templates found in rooms_data", 1800);
        }
        if (regenerate_popup_) regenerate_popup_->close();
        return;
    }

    std::vector<RegenerateRoomPopup::TemplateEntry> templates;
    templates.reserve(rooms_data.size());
    for (auto it = rooms_data.begin(); it != rooms_data.end(); ++it) {
        if (!it.value().is_object()) {
            continue;
        }
        const std::string template_key = it.key();
        if (template_key.empty()) {
            continue;
        }
        const std::string template_type = to_lower_copy(it.value().value("type", std::string("room")));
        if (template_type == "trail") {
            continue;
        }
        RegenerateRoomPopup::TemplateEntry entry;
        entry.key = template_key;
        entry.label = template_key;
        templates.push_back(std::move(entry));
    }

    if (templates.empty()) {
        if (regenerate_popup_) regenerate_popup_->close();
        if (assets_) {
            assets_->show_dev_notice("No room templates available for regen", 1800);
        }
        return;
    }

    if (!regenerate_popup_) {
        regenerate_popup_ = std::make_unique<RegenerateRoomPopup>();
    }

    regenerate_popup_->open(templates,
                            [this](const std::string& selected_template_key) {
                                if (!room_editor_) return;
                                room_editor_->regenerate_room_from_template(selected_template_key);
                                if (regenerate_popup_) regenerate_popup_->close();
                                sync_header_button_states();
                            },
                            screen_w_,
                            screen_h_);
}

void DevControls::toggle_camera_panel() {
    if (!camera_panel_) {
        return;
    }
    camera_panel_->set_assets(assets_);
    if (camera_panel_->is_visible()) {
        camera_panel_->close();
    } else {
        if (is_modal_blocking_panels()) {
            pulse_modal_header();
            sync_header_button_states();
            return;
        }
        camera_panel_->open();
    }
    sync_header_button_states();
}

void DevControls::close_camera_panel() {
    if (camera_panel_) {
        camera_panel_->close();
    }
}

bool DevControls::can_use_room_editor_ui() const {
    return enabled_ && room_editor_ && room_editor_->is_enabled();
}

void DevControls::enter_map_editor_mode() {
    set_mode(Mode::RoomEditor);
    sync_header_button_states();
}

void DevControls::exit_map_editor_mode(bool focus_player, bool restore_previous_state) {
    (void)focus_player;
    (void)restore_previous_state;
    set_mode(Mode::RoomEditor);
    sync_header_button_states();
}

void DevControls::handle_map_selection() {
    // Room selection is now handled exclusively by RoomEditor room/trail nav badges.
}

Room* DevControls::find_spawn_room() const {
    if (assets_) {
        const std::vector<Room*> layer_zero_rooms = assets_->game_context().rooms_in_layer(0);
        if (!layer_zero_rooms.empty() && layer_zero_rooms.front()) {
            return layer_zero_rooms.front();
        }
    }
    if (!rooms_) return nullptr;
    for (Room* room : *rooms_) {
        if (room && room->room_area) {
            return room;
        }
    }
    return nullptr;
}

Room* DevControls::choose_room(Room* preferred) const {
    if (preferred) {
        return preferred;
    }
    if (Room* spawn = find_spawn_room()) {
        return spawn;
    }
    if (!rooms_) {
        return nullptr;
    }
    for (Room* room : *rooms_) {
        if (room && room->room_area) {
            return room;
        }
    }
    return nullptr;
}

void DevControls::filter_active_assets(std::vector<Asset*>& /*assets*/) const {
    if (!enabled_) {
        restore_filter_hidden_assets();
        return;
    }
    if (!assets_) {
        return;
    }

    // Visible set as computed by Assets::update_filtered_active_assets().
    const auto& visible = assets_->filtered_active_asset_membership();
    const auto& all_assets = assets_->getActive();

    std::unordered_set<Asset*> active_set;
    active_set.reserve(all_assets.size());
    std::unordered_set<Asset*> to_hide;
    to_hide.reserve(all_assets.size());

    for (Asset* asset : all_assets) {
        if (!asset) {
            continue;
        }
        active_set.insert(asset);
        if (visible.find(asset) == visible.end()) {
            to_hide.insert(asset);
        }
    }

    // Restore anything we previously hid that is now visible (or no longer active).
    for (auto it = filter_hidden_assets_.begin(); it != filter_hidden_assets_.end();) {
        Asset* asset = it->first;
        const bool still_active = active_set.find(asset) != active_set.end();
        const bool should_hide = to_hide.find(asset) != to_hide.end();
        if (!still_active || !should_hide) {
            if (asset) {
                asset->set_hidden(it->second.hidden);
                asset->active = it->second.active;
            }
            it = filter_hidden_assets_.erase(it);
        } else {
            ++it;
        }
    }

    // Hide assets that failed the filters and make sure they cannot be interacted with.
    for (Asset* asset : to_hide) {
        if (!asset) {
            continue;
        }
        auto [entry, inserted] = filter_hidden_assets_.emplace(
            asset, FilterHiddenAssetState{asset->is_hidden(), asset->active});
        asset->set_hidden(true);
        asset->active = false;
        asset->set_highlighted(false);
        asset->set_selected(false);
        (void)inserted;
    }

    // Track the currently hidden set so we can compare on the next update.
    previous_filtered_membership_ = std::move(to_hide);
}

void DevControls::refresh_active_asset_filters() {
    if (!assets_ || !enabled_) {
        return;
    }
    assets_->refresh_filtered_active_assets();
    auto& filtered = assets_->mutable_filtered_active_assets();
    set_active_assets(filtered, assets_->dev_active_state_version());
    if (room_editor_) {
        room_editor_->clear_highlighted_assets();
    }
}

bool DevControls::should_hide_assets_for_map_mode() const {
    return false;
}

void DevControls::reset_asset_filters() {
    other_settings_.reset();
    restore_filter_hidden_assets();
    refresh_active_asset_filters();
}

bool DevControls::passes_asset_filters(Asset* asset) const {
    if (!asset) {
        return false;
    }
    if (should_hide_assets_for_map_mode()) {
        return false;
    }
    return other_settings_.passes(*asset);
}

bool DevControls::fog_visible() const {
    return false;
}

bool DevControls::live_dynamic_assets_visible() const {
    if (!enabled_) {
        return true;
    }
    return other_settings_.is_type_filter_enabled("dynamic");
}


bool DevControls::persist_map_info_to_disk() {
    if (!assets_) {
        std::cerr << "[DevControls] Cannot persist map info: assets manager not set\n";
        return false;
    }
    const std::string map_id = assets_->map_id();
    if (map_id.empty()) {
        std::cerr << "[DevControls] Cannot persist map info: map id empty\n";
        return false;
    }
    mark_map_dirty(devmode::core::DevSaveCoordinator::Priority::Immediate);
    return true;
}
