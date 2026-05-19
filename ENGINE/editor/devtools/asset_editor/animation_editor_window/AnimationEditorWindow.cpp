#include "AnimationEditorWindow.hpp"
#include "utils/sdl_render_conversions.hpp"
#include "utils/sdl_mouse_utils.hpp"
#include "utils/ttf_render_utils.hpp"

#include <SDL3/SDL.h>
#include <SDL3_ttf/SDL_ttf.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <optional>
#include <regex>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <tuple>
#include <unordered_set>
#include <vector>

#include <nlohmann/json.hpp>

#include "AnimationDocument.hpp"
#include "AnimationInspectorPanel.hpp"
#include "AnimationListContextMenu.hpp"
#include "AnimationListPanel.hpp"
#include "CustomControllerService.hpp"
#include "EditorUIPrimitives.hpp"
#include "AsyncTaskQueue.hpp"
#include "AudioImporter.hpp"
#include "core/manifest/manifest_loader.hpp"
#include "PreviewProvider.hpp"
#include "string_utils.hpp"
#include "utils/input.hpp"

#include "assets/asset/asset_info.hpp"
#include "devtools/animation_runtime_refresh.hpp"
#include "devtools/animation_frame_import_service.hpp"
#include "devtools/core/manifest_store.hpp"
#include "devtools/frame_importer.hpp"
#include "devtools/dm_styles.hpp"
#include "devtools/draw_utils.hpp"
#include "devtools/widgets.hpp"
#include "core/AssetsManager.hpp"
#include "devtools/asset_paths.hpp"
#include "devtools/dev_mode_utils.hpp"
#include "devtools/sdl_modal_dialog.hpp"
#include "core/AssetsManager.hpp"

namespace {

using animation_editor::AnimationEditorWindow;
namespace fs = std::filesystem;
namespace asset_paths = devmode::asset_paths;

constexpr int kAutoSaveDelayFrames = 12;

class ScopedRendererState {
public:
    explicit ScopedRendererState(SDL_Renderer* renderer)
        : renderer_(renderer),
          target_(renderer ? SDL_GetRenderTarget(renderer) : nullptr),
          clip_enabled_(renderer ? SDL_RenderClipEnabled(renderer) : false) {
        if (!renderer_) {
            return;
        }
        if (clip_enabled_) {
            SDL_GetRenderClipRect(renderer_, &clip_rect_);
        }
        SDL_GetRenderDrawBlendMode(renderer_, &blend_mode_);
    }

    ScopedRendererState(const ScopedRendererState&) = delete;
    ScopedRendererState& operator=(const ScopedRendererState&) = delete;

    ~ScopedRendererState() {
        restore(nullptr);
    }

    void restore(std::string* target_debug) {
        if (!renderer_ || restored_) {
            return;
        }

        SDL_Texture* target_after = SDL_GetRenderTarget(renderer_);
        const bool target_changed = (target_ != target_after);
        if (target_debug) {
            *target_debug = target_changed ? "changed" : "unchanged";
        }
        if (target_changed) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "[AnimationEditor] Render target changed during render; restoring previous target.");
            SDL_SetRenderTarget(renderer_, target_);
        }

        if (clip_enabled_) {
            SDL_SetRenderClipRect(renderer_, &clip_rect_);
        } else {
            SDL_SetRenderClipRect(renderer_, nullptr);
        }
        SDL_SetRenderDrawBlendMode(renderer_, blend_mode_);
        restored_ = true;
    }

private:
    SDL_Renderer* renderer_ = nullptr;
    SDL_Texture* target_ = nullptr;
    bool clip_enabled_ = false;
    SDL_Rect clip_rect_{0, 0, 0, 0};
    SDL_BlendMode blend_mode_ = SDL_BLENDMODE_NONE;
    bool restored_ = false;
};

fs::path preferred_asset_folder(const std::string& asset_name) {
    if (asset_name.empty()) {
        return asset_paths::assets_root_path();
    }
    return (asset_paths::assets_root_path() / asset_name).lexically_normal();
}

bool path_has_prefix(fs::path path, fs::path prefix) {
    path = path.lexically_normal();
    prefix = prefix.lexically_normal();
    if (prefix.empty()) {
        return false;
    }
    auto pit = prefix.begin();
    auto it = path.begin();
    for (; pit != prefix.end(); ++pit, ++it) {
        if (it == path.end() || *it != *pit) {
            return false;
        }
    }
    return true;
}

bool is_inside_assets_root(const fs::path& path) {
    return path_has_prefix(path, asset_paths::assets_root_path());
}

bool is_inside_resources_root(const fs::path& path) {
    return path_has_prefix(path, fs::path("resources"));
}

void copy_directory_contents(const fs::path& source, const fs::path& destination, const std::string& asset_name) {
    std::error_code ec;
    if (source.empty() || destination.empty()) {
        return;
    }
    if (!fs::exists(source, ec) || !fs::is_directory(source, ec)) {
        return;
    }
    fs::create_directories(destination, ec);
    if (ec) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "[AnimationEditor] Failed to prepare destination '%s' for '%s': %s", destination.generic_string().c_str(), asset_name.c_str(), ec.message().c_str());
        return;
    }
    for (fs::directory_iterator it(source, ec); !ec && it != fs::directory_iterator(); ++it) {
        const fs::path target = destination / it->path().filename();
        fs::copy(it->path(), target, fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
        if (ec) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "[AnimationEditor] Failed to copy '%s' to '%s' for '%s': %s", it->path().generic_string().c_str(), target.generic_string().c_str(), asset_name.c_str(), ec.message().c_str());
            ec.clear();
        }
    }
}

fs::path ensure_assets_storage(const fs::path& candidate, const AssetInfo& info) {
    const std::string asset_name = info.name;
    if (asset_name.empty()) {
        return candidate.lexically_normal();
    }

    const fs::path preferred = preferred_asset_folder(asset_name);
    fs::path normalized_candidate = candidate.lexically_normal();

    if (normalized_candidate.empty()) {
        normalized_candidate = preferred;
    }

    if (is_inside_assets_root(normalized_candidate)) {
        return normalized_candidate;
    }

    if (!normalized_candidate.empty() && !is_inside_resources_root(normalized_candidate)) {
        return normalized_candidate;
    }

    std::error_code ec;
    const bool preferred_exists = fs::exists(preferred, ec);
    ec.clear();
    const bool candidate_exists = !normalized_candidate.empty() && fs::exists(normalized_candidate, ec);
    ec.clear();

    if (!preferred_exists && candidate_exists) {
        const fs::path source = normalized_candidate;
        if (!source.empty() && source != preferred) {
            copy_directory_contents(source, preferred, asset_name);
        }
    }

    fs::create_directories(preferred, ec);
    if (ec) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "[AnimationEditor] Failed to create assets directory '%s' for '%s': %s", preferred.generic_string().c_str(), asset_name.c_str(), ec.message().c_str());
        return normalized_candidate.empty() ? preferred : normalized_candidate;
    }

    return preferred;
}

void render_label(SDL_Renderer* renderer, const std::string& text, int x, int y) {
    if (!renderer || text.empty()) return;

    const DMLabelStyle& style = DMStyles::Label();
    TTF_Font* font = style.open_font();
    if (!font) return;

    SDL_Surface* surf = ttf_util::RenderTextBlended(font, text.c_str(), style.color);
    if (!surf) {
        TTF_CloseFont(font);
        return;
    }

    SDL_Texture* tex = SDL_CreateTextureFromSurface(renderer, surf);
    if (tex) {
        SDL_Rect dst{x, y, surf->w, surf->h};
        sdl_render::Texture(renderer, tex, nullptr, &dst);
        SDL_DestroyTexture(tex);
    }
    SDL_DestroySurface(surf);
    TTF_CloseFont(font);
}

std::vector<std::filesystem::path> split_paths(const std::string& raw) {
    std::vector<std::filesystem::path> paths;
    size_t start = 0;
    while (start < raw.size()) {
        size_t pos = raw.find('|', start);
        std::string token = raw.substr(start, pos == std::string::npos ? std::string::npos : pos - start);
        token = animation_editor::strings::trim_copy(token);
        if (!token.empty()) {
            paths.emplace_back(token);
        }
        if (pos == std::string::npos) break;
        start = pos + 1;
    }
    return paths;
}

std::string lowercase_copy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

bool has_extension_ci(const std::filesystem::path& path, std::string_view extension) {
    return lowercase_copy(path.extension().string()) == lowercase_copy(std::string(extension));
}

std::vector<std::filesystem::path> normalize_sequence_paths(const std::vector<std::filesystem::path>& files) {
    std::vector<std::filesystem::path> normalized = files;
    auto numeric_key = [](const std::filesystem::path& path) {
        std::string stem = path.stem().string();
        try {
            return std::make_tuple(0, std::stoi(stem), lowercase_copy(stem), lowercase_copy(path.filename().string()));
        } catch (...) {
            std::smatch match;
            static const std::regex number_regex("(\\d+)", std::regex::icase);
            if (std::regex_search(stem, match, number_regex)) {
                try {
                    return std::make_tuple(0, std::stoi(match.str(1)), lowercase_copy(stem), lowercase_copy(path.filename().string()));
                } catch (...) {
                }
            }
        }
        return std::make_tuple(1, 0, lowercase_copy(stem), lowercase_copy(path.filename().string()));
    };
    std::sort(normalized.begin(), normalized.end(), [&](const std::filesystem::path& lhs, const std::filesystem::path& rhs) {
        return numeric_key(lhs) < numeric_key(rhs);
    });
    return normalized;
}

nlohmann::json build_movement_sequence(int frame_count, int total_dx, int total_dy, int total_dz) {
    nlohmann::json movement = nlohmann::json::array();
    const int safe_frames = std::max(frame_count, 1);
    movement.get_ref<nlohmann::json::array_t&>().reserve(static_cast<std::size_t>(safe_frames));

    auto distribute_component = [safe_frames](int total) {
        std::vector<int> values(static_cast<std::size_t>(safe_frames), 0);
        if (safe_frames <= 0) {
            return values;
        }
        const int base = total / safe_frames;
        int remainder = total % safe_frames;
        for (int i = 0; i < safe_frames; ++i) {
            values[static_cast<std::size_t>(i)] = base;
            if (remainder > 0) {
                ++values[static_cast<std::size_t>(i)];
                --remainder;
            } else if (remainder < 0) {
                --values[static_cast<std::size_t>(i)];
                ++remainder;
            }
        }
        return values;
    };

    const std::vector<int> dx_frames = distribute_component(total_dx);
    const std::vector<int> dy_frames = distribute_component(total_dy);
    const std::vector<int> dz_frames = distribute_component(total_dz);
    for (int i = 0; i < safe_frames; ++i) {
        const int frame_dx = dx_frames[static_cast<std::size_t>(i)];
        const int frame_dy = dy_frames[static_cast<std::size_t>(i)];
        const int frame_dz = dz_frames[static_cast<std::size_t>(i)];
        movement.push_back(nlohmann::json::array({frame_dx, frame_dy, frame_dz}));
    }
    return movement;
}

nlohmann::json build_empty_geometry_frames(int frame_count) {
    nlohmann::json frames = nlohmann::json::array();
    const int safe_frames = std::max(frame_count, 1);
    frames.get_ref<nlohmann::json::array_t&>().reserve(static_cast<std::size_t>(safe_frames));
    for (int i = 0; i < safe_frames; ++i) {
        frames.push_back(nlohmann::json::array());
    }
    return frames;
}

nlohmann::json build_movement_total(const nlohmann::json& movement) {
    int total_dx = 0;
    int total_dy = 0;
    int total_dz = 0;
    float total_dr = 0.0f;
    if (movement.is_array()) {
        for (std::size_t i = 0; i < movement.size(); ++i) {
            const auto& entry = movement[i];
            if (!entry.is_array()) {
                continue;
            }
            if (entry.size() > 0 && entry[0].is_number()) {
                total_dx += static_cast<int>(std::lround(entry[0].get<double>()));
            }
            if (entry.size() > 1 && entry[1].is_number()) {
                total_dy += static_cast<int>(std::lround(entry[1].get<double>()));
            }
            if (entry.size() > 2 && entry[2].is_number()) {
                total_dz += static_cast<int>(std::lround(entry[2].get<double>()));
            }
            if (entry.size() > 3 && entry[3].is_number()) {
                total_dr += static_cast<float>(entry[3].get<double>());
            }
        }
    }
    return nlohmann::json::object({
        {"dx", total_dx},
        {"dy", total_dy},
        {"dz", total_dz},
        {"dr", total_dr},
    });
}

struct DefaultAnimationSpec {
    std::string id;
    int dx = 0;
    int dy = 0;
    int dz = 0;
    std::string source_id;
    bool folder_sourced = false;
    bool invert_frames_horizontal = false;
    bool invert_frames_vertical = false;
};

std::vector<std::string> default_direction_tags(const std::string& animation_id,
                                                int dx,
                                                int dy,
                                                int dz) {
    std::vector<std::string> tags;
    std::unordered_set<std::string> seen;
    auto append = [&](std::string tag) {
        tag = animation_editor::strings::trim_copy(tag);
        std::transform(tag.begin(), tag.end(), tag.begin(), [](unsigned char ch) {
            return static_cast<char>(std::tolower(ch));
        });
        if (!tag.empty() && seen.insert(tag).second) {
            tags.push_back(std::move(tag));
        }
    };

    append("movement");
    append("directional");

    std::stringstream id_stream(animation_id);
    std::string part;
    while (std::getline(id_stream, part, '_')) {
        append(part);
    }

    if (dx < 0) {
        append("left");
        append("world_x_negative");
    } else if (dx > 0) {
        append("right");
        append("world_x_positive");
    }

    if (dy < 0) {
        append("down");
        append("world_y_negative");
    } else if (dy > 0) {
        append("up");
        append("world_y_positive");
    }

    if (dz < 0) {
        append("up");
        append("forward");
        append("world_z_negative");
    } else if (dz > 0) {
        append("down");
        append("backward");
        append("world_z_positive");
    }

    if (dy != 0) {
        append("elevation");
    }

    return tags;
}

std::string default_audio_subdir() { return "audio"; }

std::string manifest_key_fallback(const AssetInfo& info) {
    if (!info.name.empty()) {
        return info.name;
    }
    try {
        std::filesystem::path dir = info.asset_dir_path();
        if (dir.has_filename()) {
            std::string stem = dir.filename().string();
            if (!stem.empty()) {
                return stem;
            }
        }
        if (!dir.empty()) {
            std::string normalized = dir.lexically_normal().generic_string();
            if (!normalized.empty()) {
                return normalized;
            }
        }
    } catch (...) {
    }
    return {};
}

bool has_animation_entries(const nlohmann::json& asset_json) {
    if (!asset_json.is_object()) {
        return false;
    }
    auto animations_it = asset_json.find("animations");
    if (animations_it == asset_json.end() || !animations_it->is_object()) {
        return false;
    }
    if (animations_it->contains("animations") && (*animations_it)["animations"].is_object()) {
        return !(*animations_it)["animations"].empty();
    }
    return !animations_it->empty();
}

nlohmann::json build_folder_payload(const std::filesystem::path& folder) {
    try {
        if (folder.empty() || !std::filesystem::exists(folder) || !std::filesystem::is_directory(folder)) {
            return {};
        }
        int frame_count = 0;
        for (const auto& entry : std::filesystem::directory_iterator(folder)) {
            if (!entry.is_regular_file()) {
                continue;
            }
            std::string ext = entry.path().extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char ch) {
                return static_cast<char>(std::tolower(ch));
            });
            if (ext == ".png" || ext == ".gif") {
                ++frame_count;
            }
        }
        if (frame_count == 0) {
            return {};
        }
        nlohmann::json payload = {
            {"locked", false},
            {"reverse_source", false},
            {"invert_x", false},
            {"invert_y", false},
            {"invert_z", false},
            {"rnd_start", false},
            {"on_end", "default"},
            {"source",
                {
                    {"kind", "folder"},
                    {"path", folder.generic_string()},

                    {"name", ""},
                }},
};
        payload["number_of_frames"] = frame_count;
        return payload;
    } catch (...) {
        return {};
    }
}

nlohmann::json snapshot_from_asset_folders(const AssetInfo& info, const std::filesystem::path& asset_root) {
    nlohmann::json snapshot = nlohmann::json::object();
    if (!info.name.empty()) {
        snapshot["asset_name"] = info.name;
    }
    if (!info.type.empty()) {
        snapshot["asset_type"] = info.type;
    }
    if (!asset_root.empty()) {
        snapshot["asset_directory"] = asset_root.generic_string();
    }

    nlohmann::json animations = nlohmann::json::object();
    try {
        if (!asset_root.empty() && std::filesystem::exists(asset_root) && std::filesystem::is_directory(asset_root)) {

            for (const auto& entry : std::filesystem::directory_iterator(asset_root)) {
                if (!entry.is_directory()) {
                    continue;
                }
                std::string anim_id = entry.path().filename().string();
                if (anim_id.empty()) {
                    continue;
                }
                nlohmann::json payload = build_folder_payload(entry.path());
                if (!payload.is_object() || payload.empty()) {
                    continue;
                }
                animations[anim_id] = std::move(payload);
            }

            nlohmann::json root_payload = build_folder_payload(asset_root);
            if (root_payload.is_object() && !root_payload.empty()) {

                std::string preferred_id = "default";
                if (animations.contains(preferred_id)) {

                    preferred_id = "root";
                    if (animations.contains(preferred_id)) {
                        preferred_id = info.name.empty() ? std::string{"main"} : info.name;
                        if (preferred_id.empty()) preferred_id = "main";
                    }
                }
                animations[preferred_id] = std::move(root_payload);
            }
        }
    } catch (...) {
        animations = nlohmann::json::object();
    }

    if (!animations.empty()) {
        snapshot["animations"] = std::move(animations);
        std::string start_id = info.start_animation;
        if (start_id.empty()) {

            if (snapshot["animations"].contains("default")) {
                start_id = "default";
            } else {
                const auto& anims = snapshot["animations"];
                auto it = anims.begin();
                if (it != anims.end()) {
                    start_id = it.key();
                }
            }
        }
        if (!start_id.empty()) {
            snapshot["start"] = start_id;
        }
    }

    return snapshot;
}

nlohmann::json snapshot_from_asset_info(const AssetInfo& info) {
    nlohmann::json snapshot = nlohmann::json::object();
    if (!info.name.empty()) {
        snapshot["asset_name"] = info.name;
    }
    if (!info.type.empty()) {
        snapshot["asset_type"] = info.type;
    }
    try {
        std::filesystem::path dir = info.asset_dir_path();
        if (!dir.empty()) {
            snapshot["asset_directory"] = dir.generic_string();
        }
    } catch (...) {
    }

    nlohmann::json animations = nlohmann::json::object();
    try {
        auto names = info.animation_names();
        for (const auto& anim_id : names) {
            nlohmann::json payload = info.animation_payload(anim_id);
            if (payload.is_object() && !payload.empty()) {
                animations[anim_id] = std::move(payload);
            }
        }
    } catch (...) {
        animations = nlohmann::json::object();
    }

    if (!animations.empty()) {
        snapshot["animations"] = std::move(animations);
        if (!info.start_animation.empty()) {
            snapshot["start"] = info.start_animation;
        }
    }

    return snapshot;
}

}

namespace animation_editor {

AnimationEditorWindow::AnimationEditorWindow() {
    document_ = std::make_shared<AnimationDocument>();
    document_->set_on_saved_callback([this]() { this->handle_document_saved(); });
    document_->set_on_structure_changed_callback([this](const AnimationDocument::StructureChangeEvent&) {
        std::string error;
        if (!this->invalidate_asset_cache_now(error) && !error.empty()) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "[AnimationEditor] %s", error.c_str());
        }
    });
    preview_provider_ = std::make_shared<PreviewProvider>();
    preview_provider_->set_document(document_);
    task_queue_ = std::make_shared<AsyncTaskQueue>();
    audio_importer_ = std::make_shared<AudioImporter>();
    list_panel_ = std::make_unique<AnimationListPanel>();
    list_panel_->set_document(document_);
    list_panel_->set_preview_provider(preview_provider_);
    configure_list_panel();
    inspector_panel_ = std::make_unique<AnimationInspectorPanel>();
    inspector_panel_->set_document(document_);
    inspector_panel_->set_preview_provider(preview_provider_);
    configure_inspector_panel();
    list_context_menu_ = std::make_unique<AnimationListContextMenu>();
    custom_controller_service_ = std::make_unique<CustomControllerService>();

    load_details_toggle_button_ = std::make_unique<DMButton>("Load Details", &DMStyles::HeaderButton(), 140, DMButton::height());
    load_details_copy_button_ = std::make_unique<DMButton>("Copy", &DMStyles::HeaderButton(), 90, DMButton::height());
    ensure_defaults_modal_widgets();
    layout_dirty_ = true;
}

AnimationEditorWindow::~AnimationEditorWindow() {
    if (document_) {
        document_->set_on_saved_callback(nullptr);
        document_->set_on_structure_changed_callback(nullptr);
    }
}

void AnimationEditorWindow::set_visible(bool visible, bool process_close) {
    const bool was_visible = visible_;
    const bool becoming_hidden = (!visible && was_visible);
    const bool becoming_visible = (visible && !was_visible);
    const bool notify_closed = (becoming_hidden && process_close);

    if (becoming_hidden) {
        // Always clear transient overlay/interaction state when hiding, even for
        // programmatic visibility transitions that skip close processing.
        close_defaults_modal();
        defaults_modal_lifecycle_ = ModalLifecycleState::Closed;
        pending_external_action_.clear();
        if (list_context_menu_) {
            list_context_menu_->close();
        }
        if (load_details_toggle_button_) {
            load_details_toggle_button_->cancel_interaction();
        }
        if (load_details_copy_button_) {
            load_details_copy_button_->cancel_interaction();
        }

        if (process_close) {
            if (document_ && document_->consume_dirty_flag()) {
                auto_save_pending_ = true;
                auto_save_timer_frames_ = 0;
            }
            auto_save_timer_frames_ = 0;
            process_auto_save();

            if (using_manifest_store_ && manifest_transaction_) {

                nlohmann::json dummy;
                persist_manifest_payload(dummy, true);
            }
        }
    }

    visible_ = visible;
    if (becoming_visible) {
        first_render_completed_ = false;
        log_ui_transition("visibility", "opened");
        ensure_layout();
        ensure_selection_valid();
        refresh_panels_after_load();
    } else if (becoming_hidden) {
        log_ui_transition("visibility", "closed");
    }
    if (notify_closed && on_closed_) {
        on_closed_();
    }
}

void AnimationEditorWindow::toggle_visible() { set_visible(!visible_); }

void AnimationEditorWindow::set_bounds(const SDL_Rect& bounds) {
    if (bounds_.x == bounds.x && bounds_.y == bounds.y &&
        bounds_.w == bounds.w && bounds_.h == bounds.h) {
        return;
    }
    const bool was_valid = bounds_.w > 0 && bounds_.h > 0;
    const bool now_valid = bounds.w > 0 && bounds.h > 0;
    if (was_valid != now_valid) {
        log_ui_transition("bounds",
                          now_valid ? "became_valid" : "became_invalid");
    }
    bounds_ = bounds;
    first_render_completed_ = false;
    layout_dirty_ = true;
    layout_children();
}

bool AnimationEditorWindow::is_layout_valid() const {
    return layout_state_.bounds_valid && layout_state_.list_valid && layout_state_.inspector_valid;
}

bool AnimationEditorWindow::is_ready_for_action_execution() const {
    return visible_ && is_layout_valid() && document_ && first_render_completed_;
}

int AnimationEditorWindow::status_height() const {
    const int padding = DMSpacing::panel_padding();
    int height = DMButton::height() + padding * 2;
    if (!load_details_expanded_) {
        return height;
    }

    const int line_height = DMStyles::Label().font_size + 4;
    constexpr int kDiagnosticLineCount = 7;
    height += DMSpacing::small_gap() + kDiagnosticLineCount * line_height + DMSpacing::small_gap();
    return height;
}

void AnimationEditorWindow::refresh_panels_after_load() {
    if (list_panel_) {
        list_panel_->update();
        list_panel_->set_selected_animation_id(selected_animation_id_);
    }
    if (inspector_panel_) {
        if (selected_animation_id_) {
            inspector_panel_->set_animation_id(*selected_animation_id_);
            inspector_panel_->update();
        }
    }
}

void AnimationEditorWindow::set_info(const std::shared_ptr<AssetInfo>& info) {
    close_manifest_transaction();
    info_ = info;
    pending_external_action_.clear();
    first_render_completed_ = false;
    load_diagnostics_ = {};

    if (!document_) {
        document_ = std::make_shared<AnimationDocument>();
        document_->set_on_saved_callback([this]() { this->handle_document_saved(); });
        document_->set_on_structure_changed_callback([this](const AnimationDocument::StructureChangeEvent&) {
            std::string error;
            if (!this->invalidate_asset_cache_now(error) && !error.empty()) {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "[AnimationEditor] %s", error.c_str());
            }
        });
    }

    if (!info) {
        clear_info();
        return;
    }

    asset_root_path_.clear();
    try {
        std::filesystem::path candidate = info->asset_dir_path();
        if (!candidate.empty()) {
            asset_root_path_ = candidate;
        }
    } catch (...) {
        asset_root_path_.clear();
    }
    asset_root_path_ = ensure_assets_storage(asset_root_path_, *info);
    devmode::frame_importer::cleanup_stale_import_folders(asset_root_path_);
    if (custom_controller_service_) {
        custom_controller_service_->set_asset_root(asset_root_path_);
    }

    process_auto_save();

    using_manifest_store_ = false;
    manifest_asset_key_.clear();
    manifest_transaction_ = {};
    if (document_) {
        document_->set_manifest_asset_key_debug({});
    }

    enum class SnapshotRecoverySource { None, AssetMetadata, AssetFolders, Manifest, RuntimeAssetMetadata };
    SnapshotRecoverySource recovery_source = SnapshotRecoverySource::None;

    auto build_folder_snapshot = [&]() -> nlohmann::json { return snapshot_from_asset_folders(*info, asset_root_path_); };
    auto build_info_snapshot   = [&]() -> nlohmann::json { return snapshot_from_asset_info(*info); };

    nlohmann::json snapshot = nlohmann::json::object();
    std::function<bool(const nlohmann::json&)> persist_callback;
    bool seed_transaction_with_recovery = false;

    nlohmann::json info_snapshot = build_info_snapshot();
    auto attach_manifest_transaction = [&](const std::string& key, bool log_creation) {
        if (!manifest_store_ || key.empty()) {
            return;
        }
        manifest_asset_key_ = key;
        manifest_transaction_ = manifest_store_->begin_asset_transaction(manifest_asset_key_, true);
        if (manifest_transaction_) {
            using_manifest_store_ = true;
            load_diagnostics_.manifest_transaction_open_result = "opened";
            if (document_) {
                document_->set_manifest_asset_key_debug(manifest_asset_key_);
            }
            persist_callback = [this](const nlohmann::json& payload) { return this->persist_manifest_payload(payload); };
            if (log_creation) {
                std::cerr << "[AnimationEditor] Created manifest entry for '" << info->name << "' as '" << manifest_asset_key_ << "'\n";
            }
        } else {
            load_diagnostics_.manifest_transaction_open_result = "failed";
            std::cerr << "[AnimationEditor] Failed to open manifest transaction for '" << manifest_asset_key_ << "'\n";
            manifest_asset_key_.clear();
            manifest_transaction_ = {};
            if (document_) {
                document_->set_manifest_asset_key_debug({});
            }
        }
    };

    if (manifest_store_) {
        if (auto key = resolve_manifest_key(*info)) {
            load_diagnostics_.manifest_key_resolution_result = "resolved:" + *key;
            attach_manifest_transaction(*key, false);
        } else {
            std::cerr << "[AnimationEditor] Unable to resolve manifest key for '" << info->name << "'\n";
            const std::string fallback_key = manifest_key_fallback(*info);
            if (!fallback_key.empty()) {
                load_diagnostics_.manifest_key_resolution_result = "fallback:" + fallback_key;
                attach_manifest_transaction(fallback_key, true);
            } else {
                load_diagnostics_.manifest_key_resolution_result = "failed";
            }
        }
    } else {
        load_diagnostics_.manifest_key_resolution_result = "manifest_store_unavailable";
        load_diagnostics_.manifest_transaction_open_result = "not_attempted";
        std::cerr << "[AnimationEditor] Manifest store unavailable; animations will not persist for '" << info->name << "'\n";
    }
    if (custom_controller_service_) {
        custom_controller_service_->set_manifest_store(manifest_store_);
        custom_controller_service_->set_manifest_asset_key(manifest_asset_key_);
    }

    if (manifest_transaction_) {
        nlohmann::json manifest_data = manifest_transaction_.data();
        if (has_animation_entries(manifest_data)) {
            snapshot = std::move(manifest_data);
            recovery_source = SnapshotRecoverySource::Manifest;
            std::cerr << "[AnimationEditor] Loaded animations from manifest for '" << info->name << "'\n";
        }
    }

    if (!has_animation_entries(snapshot) && has_animation_entries(info_snapshot)) {
        snapshot = std::move(info_snapshot);
        recovery_source = SnapshotRecoverySource::AssetMetadata;
        seed_transaction_with_recovery = true;
        std::cerr << "[AnimationEditor] Using animations from AssetInfo for '" << info->name << "'\n";
    }

    if (!has_animation_entries(snapshot)) {
        nlohmann::json folder_snapshot = build_folder_snapshot();
        if (has_animation_entries(folder_snapshot)) {
            snapshot = std::move(folder_snapshot);
            recovery_source = SnapshotRecoverySource::AssetFolders;
            seed_transaction_with_recovery = true;
            std::cerr << "[AnimationEditor] Recovered animations by scanning folders for '" << info->name << "'\n";
        } else {
            snapshot = nlohmann::json::object();
            std::cerr << "[AnimationEditor] No animations found for '" << info->name << "' (manifest/metadata/folders)\n";
        }
    }

    auto apply_snapshot = [&](const nlohmann::json& payload, SnapshotRecoverySource source) {
        document_->load_from_manifest(payload, asset_root_path_, persist_callback);
        recovery_source = source;
        if (using_manifest_store_ && has_animation_entries(payload)) {
            persist_manifest_payload(payload);
        }
};

    const bool snapshot_was_empty = !has_animation_entries(snapshot);
    document_->load_from_manifest(snapshot, asset_root_path_, persist_callback);
    if (seed_transaction_with_recovery) {
        persist_manifest_payload(snapshot);
    }

    if (document_->animation_ids().empty()) {
        bool recovered = false;

        nlohmann::json metadata_snapshot2 = snapshot_from_asset_info(*info);
        if (has_animation_entries(metadata_snapshot2)) {
            apply_snapshot(metadata_snapshot2, SnapshotRecoverySource::AssetMetadata);
            recovered = true;
        } else {
            nlohmann::json folder_snapshot2 = snapshot_from_asset_folders(*info, asset_root_path_);
            if (has_animation_entries(folder_snapshot2)) {
                apply_snapshot(folder_snapshot2, SnapshotRecoverySource::AssetFolders);
                recovered = true;
            }
        }
        if (!recovered) {

            if (target_asset_ && target_asset_->info) {
                nlohmann::json runtime_snapshot = snapshot_from_asset_info(*target_asset_->info);
                if (has_animation_entries(runtime_snapshot)) {
                    apply_snapshot(runtime_snapshot, SnapshotRecoverySource::AssetMetadata);
                    recovery_source = SnapshotRecoverySource::RuntimeAssetMetadata;
                    recovered = true;
                    std::cerr << "[AnimationEditor] Fallback to runtime asset info for '" << info->name << "'\n";
                }
            }
            if (!recovered) {
                recovery_source = SnapshotRecoverySource::None;
            }
        }
    }

    const bool seeded_default = snapshot_was_empty && recovery_source == SnapshotRecoverySource::None &&
                                document_ && document_->animation_ids().size() == 1 && document_->animation_ids().front() == "default";
    load_diagnostics_.seeded_default_applied = seeded_default;

    if (seeded_default) {
        orchestrated_save(devmode::core::SaveOrchestrator::Reason::HotReload,
                          manifest_asset_key_.empty() ? std::string("animation-editor") : manifest_asset_key_,
                          [this]() { document_->save_to_file(); return true; });
    } else if (document_) {
        document_->consume_dirty_flag();
    }
    preview_provider_->set_document(document_);
    configure_list_panel();
    configure_inspector_panel();
    if (list_panel_) list_panel_->set_preview_provider(preview_provider_);
    if (inspector_panel_) inspector_panel_->set_preview_provider(preview_provider_);
    if (audio_importer_) {
        std::filesystem::path audio_root = asset_root_path_.empty() ? std::filesystem::path{}
                                                                   : asset_root_path_ / default_audio_subdir();
        audio_importer_->set_asset_root(audio_root);
    }
    if (list_panel_) list_panel_->set_document(document_);
    if (inspector_panel_) inspector_panel_->set_document(document_);
    ensure_selection_valid();
    refresh_panels_after_load();
    update_controller_button_label();
    std::string asset_label = info->name.empty() ? std::string("asset") : info->name;
    const bool has_any_animations = !document_->animation_ids().empty();
    if (seeded_default) {
        set_status_message("Created default animation for " + asset_label + ".", 300);
    } else {
        switch (recovery_source) {
            case SnapshotRecoverySource::AssetMetadata:
                load_diagnostics_.snapshot_source_chosen = "asset_metadata";
                set_status_message("Recovered animations from asset metadata for " + asset_label + ".", 300);
                break;
            case SnapshotRecoverySource::AssetFolders:
                load_diagnostics_.snapshot_source_chosen = "asset_folders";
                set_status_message("Recovered animations from asset folders for " + asset_label + ".", 300);
                break;
            case SnapshotRecoverySource::Manifest:
                load_diagnostics_.snapshot_source_chosen = "manifest";
                if (has_any_animations) {
                    set_status_message("Loaded " + asset_label, 240);
                } else {
                    set_status_message("No animations found for " + asset_label + ".", 240);
                }
                break;
            case SnapshotRecoverySource::RuntimeAssetMetadata:
                load_diagnostics_.snapshot_source_chosen = "runtime_asset_metadata";
                set_status_message("Recovered animations from runtime asset metadata for " + asset_label + ".", 300);
                break;
            default:
                load_diagnostics_.snapshot_source_chosen = "none";
                if (has_any_animations) {
                    set_status_message("Loaded " + asset_label, 240);
                } else {
                    set_status_message("No animations found for " + asset_label + ".", 240);
                }
                break;
        }
    }
    if (seeded_default) {
        load_diagnostics_.snapshot_source_chosen = "none";
    }
    load_diagnostics_.animation_count_loaded = static_cast<int>(document_->animation_ids().size());
    if (document_) {
        const auto& report = document_->last_load_report();
        load_diagnostics_.parse_failures = report.parse_failures;
        load_diagnostics_.normalization_failures = report.normalization_failures;
    }
    layout_dirty_ = true;
    auto_save_pending_ = false;
    auto_save_timer_frames_ = 0;
}

void AnimationEditorWindow::clear_info() {
    info_.reset();
    asset_root_path_.clear();
    load_diagnostics_ = {};
    close_manifest_transaction();
    close_create_animation_modal();
    close_defaults_modal();
    create_animation_modal_lifecycle_ = ModalLifecycleState::Closed;
    defaults_modal_lifecycle_ = ModalLifecycleState::Closed;
    pending_external_action_.clear();
    first_render_completed_ = false;
    document_->load_from_manifest(nlohmann::json::object(), std::filesystem::path{}, {});
    document_->consume_dirty_flag();
    preview_provider_->invalidate_all();
    if (list_panel_) list_panel_->set_preview_provider(preview_provider_);
    if (list_panel_) list_panel_->set_document(document_);
    if (inspector_panel_) inspector_panel_->set_preview_provider(preview_provider_);
    if (inspector_panel_) inspector_panel_->set_document(document_);
    configure_list_panel();
    configure_inspector_panel();
    select_animation(std::nullopt, false);
    refresh_panels_after_load();
    set_status_message("Select an asset to configure animations.", 240);
    auto_save_pending_ = false;
    auto_save_timer_frames_ = 0;
}

void AnimationEditorWindow::layout_children() {
    layout_dirty_ = false;
    apply_layout_state(compute_layout_state());
    layout_create_animation_modal();
    layout_defaults_modal();
    sync_modal_lifecycle_with_layout();
}

AnimationEditorWindow::LayoutState AnimationEditorWindow::compute_layout_state() const {
    LayoutState state{};
    const int padding = DMSpacing::panel_padding();
    const int header_gap = DMSpacing::small_gap();
    const int button_gap = DMSpacing::small_gap();
    state.bounds_valid = bounds_.w > 0 && bounds_.h > 0;

    const int status_padding = DMSpacing::panel_padding();
    const int status_h = status_height();
    if (load_details_toggle_button_) {
        load_details_toggle_button_->set_text(load_details_expanded_ ? "Hide Load Details" : "Load Details");
    }
    if (load_details_copy_button_) {
        load_details_copy_button_->set_text("Copy");
    }
    state.status_rect = SDL_Rect{bounds_.x, bounds_.y + bounds_.h - status_h, bounds_.w, status_h};
    state.status_rect.h = status_h;
    state.status_valid = state.status_rect.w > 0 && state.status_rect.h > 0;

    const int footer_y = state.status_rect.y + status_padding;
    const int footer_right = state.status_rect.x + state.status_rect.w - status_padding;
    int footer_cursor_x = footer_right;
    if (load_details_copy_button_) {
        const int w = std::max(load_details_copy_button_->rect().w, load_details_copy_button_->preferred_width() + 20);
        footer_cursor_x -= w;
        load_details_copy_button_->set_rect(SDL_Rect{footer_cursor_x, footer_y, w, DMButton::height()});
        footer_cursor_x -= button_gap;
    }
    if (load_details_toggle_button_) {
        const int w = std::max(load_details_toggle_button_->rect().w, load_details_toggle_button_->preferred_width() + 20);
        footer_cursor_x -= w;
        load_details_toggle_button_->set_rect(SDL_Rect{footer_cursor_x, footer_y, w, DMButton::height()});
    }
    if (load_details_expanded_ && state.status_valid) {
        const int details_y = footer_y + DMButton::height() + DMSpacing::small_gap();
        const int details_h = std::max(0, state.status_rect.y + state.status_rect.h - details_y - status_padding);
        state.load_details_rect = SDL_Rect{state.status_rect.x + status_padding,
                                           details_y,
                                           std::max(0, state.status_rect.w - status_padding * 2),
                                           details_h};
    } else {
        state.load_details_rect = SDL_Rect{0, 0, 0, 0};
    }

    int content_top = bounds_.y + padding;
    int content_bottom = state.status_rect.y - header_gap;
    int content_height = std::max(0, content_bottom - content_top);
    int available_width = std::max(0, bounds_.w - padding * 2);
    bool stack_vertical = available_width < 640;

    if (stack_vertical) {
        int gap = DMSpacing::panel_padding();
        if (content_height < gap * 2) {
            gap = DMSpacing::small_gap();
        }
        int inspector_height = content_height / 2;
        int list_height = std::max(0, content_height - inspector_height - gap);
        inspector_height = std::max(0, content_height - list_height - gap);

        state.list_rect = SDL_Rect{bounds_.x + padding, content_top, available_width, list_height};
        state.inspector_rect = SDL_Rect{bounds_.x + padding,
                                        state.list_rect.y + state.list_rect.h + gap,
                                        available_width,
                                        inspector_height};
    } else {
        int sidebar_width = std::clamp(available_width / 3, 260, 420);
        int inspector_gap = DMSpacing::panel_padding();
        if (available_width < sidebar_width + inspector_gap + 320) {
            inspector_gap = DMSpacing::small_gap();
        }
        state.list_rect = SDL_Rect{bounds_.x + padding, content_top, sidebar_width, content_height};
        int inspector_x = state.list_rect.x + state.list_rect.w + inspector_gap;
        int inspector_w = std::max(0, bounds_.x + bounds_.w - padding - inspector_x);
        state.inspector_rect = SDL_Rect{inspector_x, content_top, inspector_w, content_height};
    }
    state.list_valid = state.list_rect.w > 0 && state.list_rect.h > 0;
    state.inspector_valid = state.inspector_rect.w > 0 && state.inspector_rect.h > 0;
    return state;
}

void AnimationEditorWindow::apply_layout_state(const LayoutState& state) {
    layout_state_ = state;
    list_rect_ = state.list_rect;
    inspector_rect_ = state.inspector_rect;
    status_rect_ = state.status_rect;
    load_details_rect_ = state.load_details_rect;
    if (list_panel_) list_panel_->set_bounds(list_rect_);
    if (inspector_panel_) inspector_panel_->set_bounds(inspector_rect_);
}

void AnimationEditorWindow::sync_modal_lifecycle_with_layout() {
    if (!visible_) {
        create_animation_modal_lifecycle_ = ModalLifecycleState::Closed;
        create_animation_modal_visible_ = false;
        defaults_modal_lifecycle_ = ModalLifecycleState::Closed;
        defaults_modal_visible_ = false;
        return;
    }
    if (create_animation_modal_lifecycle_ == ModalLifecycleState::OpenReady && !create_animation_modal_bounds_valid()) {
        create_animation_modal_lifecycle_ = ModalLifecycleState::OpenPendingLayout;
        create_animation_modal_visible_ = false;
        create_animation_modal_open_deferred_ = true;
        set_status_message("Create Animation waiting for valid editor layout.", 180);
    } else if (create_animation_modal_lifecycle_ == ModalLifecycleState::OpenPendingLayout &&
               create_animation_modal_bounds_valid()) {
        create_animation_modal_lifecycle_ = ModalLifecycleState::OpenReady;
        create_animation_modal_visible_ = true;
        create_animation_modal_open_deferred_ = false;
        layout_create_animation_modal();
    } else if (create_animation_modal_lifecycle_ == ModalLifecycleState::Closing) {
        create_animation_modal_lifecycle_ = ModalLifecycleState::Closed;
        create_animation_modal_visible_ = false;
    }
    if (defaults_modal_lifecycle_ == ModalLifecycleState::OpenReady && !defaults_modal_bounds_valid()) {
        defaults_modal_lifecycle_ = ModalLifecycleState::OpenPendingLayout;
        defaults_modal_visible_ = false;
        defaults_modal_open_deferred_ = true;
        set_status_message("Create Defaults waiting for valid editor layout.", 180);
    } else if (defaults_modal_lifecycle_ == ModalLifecycleState::OpenPendingLayout &&
               defaults_modal_bounds_valid()) {
        defaults_modal_lifecycle_ = ModalLifecycleState::OpenReady;
        defaults_modal_visible_ = true;
        defaults_modal_open_deferred_ = false;
        layout_defaults_modal();
    } else if (defaults_modal_lifecycle_ == ModalLifecycleState::Closing) {
        defaults_modal_lifecycle_ = ModalLifecycleState::Closed;
        defaults_modal_visible_ = false;
    }
}

void AnimationEditorWindow::invalidate_inspector_background_cache() {
    // Inspector background caching is intentionally disabled. It previously
    // changed render targets during UI overlay rendering and could leave later
    // editor drawing on the wrong target.
}

void AnimationEditorWindow::configure_list_panel() {
    if (!list_panel_) return;
    list_panel_->set_document(document_);
    list_panel_->set_preview_provider(preview_provider_);
    list_panel_->set_on_selection_changed([this](const std::optional<std::string>& animation_id) {
        this->log_ui_transition("list_selection",
                                animation_id ? ("selected:" + *animation_id) : std::string("selected:<none>"));
        this->select_animation(animation_id, true);
    });
    list_panel_->set_on_context_menu([this](const std::optional<std::string>& animation_id, const SDL_Point& location) {
        this->handle_list_context_menu(animation_id, location);
    });
    list_panel_->set_on_delete_animation([this](const std::string& animation_id) {
        this->delete_animation_with_confirmation(animation_id);
    });
    list_panel_->set_selected_animation_id(selected_animation_id_);
}

void AnimationEditorWindow::configure_inspector_panel() {
    if (!inspector_panel_) return;
    inspector_panel_->set_document(document_);
    inspector_panel_->set_preview_provider(preview_provider_);
    inspector_panel_->set_task_queue(task_queue_);
    inspector_panel_->set_source_folder_picker([this]() { return this->pick_folder(); });
    inspector_panel_->set_source_animation_picker([this]() { return this->pick_animation_reference(); });
    inspector_panel_->set_source_gif_picker([this]() { return this->pick_gif(); });
    inspector_panel_->set_source_png_sequence_picker([this]() { return this->pick_png_sequence(); });
    inspector_panel_->set_source_frame_import_handler([this](const std::string& animation_id,
                                                             const std::vector<std::filesystem::path>& frames) {
        SourceFrameImportOutcome outcome = this->import_source_frames_for_animation(animation_id, frames);
        if (!outcome.success) {
            auto stage_name = [&]() -> const char* {
                switch (outcome.failure_stage) {
                    case SourceFrameImportOutcome::FailureStage::Copy: return "copy";
                    case SourceFrameImportOutcome::FailureStage::Payload: return "payload";
                    case SourceFrameImportOutcome::FailureStage::Save: return "save";
                    case SourceFrameImportOutcome::FailureStage::RuntimeVerify: return "runtime_verify";
                    case SourceFrameImportOutcome::FailureStage::None:
                    default: return "unknown";
                }
            };
            if (outcome.message.empty()) {
                outcome.message = std::string{"Frame import failed at stage: "} + stage_name();
            } else {
                outcome.message += " (stage: " + std::string(stage_name()) + ")";
            }
        }
        return SourceConfigPanel::FrameImportResult{outcome.success, outcome.frames_written, outcome.message};
    });
    inspector_panel_->set_source_status_callback([this](const std::string& message) { this->set_status_message(message); });
    inspector_panel_->set_source_changed_callback([this](const SourceConfigPanel::SourceChangeEvent&) {
        std::string error;
        if (!this->invalidate_asset_cache_now(error) && !error.empty()) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "[AnimationEditor] %s", error.c_str());
        }
    });
    inspector_panel_->set_frame_edit_callback({});
    inspector_panel_->set_frame_mode_edit_callback({});
    inspector_panel_->set_navigate_to_animation_callback([this](const std::string& id) {
        this->select_animation(std::optional<std::string>{id}, true);
    });
    inspector_panel_->set_audio_importer(audio_importer_);
    inspector_panel_->set_audio_file_picker([this]() { return this->pick_audio_file(); });
    inspector_panel_->set_manifest_store(manifest_store_);
    refresh_inspector_animation_callback();
    if (selected_animation_id_) {
        inspector_panel_->set_animation_id(*selected_animation_id_);
    }
}

void AnimationEditorWindow::select_animation(const std::optional<std::string>& animation_id, bool from_user) {
    const bool changed = selected_animation_id_ != animation_id;
    selected_animation_id_ = animation_id;
    if (list_panel_) {
        list_panel_->set_selected_animation_id(selected_animation_id_);
    }
    if (inspector_panel_ && selected_animation_id_) {
        inspector_panel_->set_animation_id(*selected_animation_id_);
        inspector_panel_->update();
    }

    if (from_user) {
        if (selected_animation_id_) {
            set_status_message(std::string(changed ? "Selected" : "Activated") +
                                   " animation '" + *selected_animation_id_ + "'.",
                               150);
            log_ui_transition("selection", std::string(changed ? "selected:" : "activated:") + *selected_animation_id_);
        } else {
            set_status_message("No animation selected.", 120);
            log_ui_transition("selection", "none");
        }
    }
}

void AnimationEditorWindow::ensure_selection_valid() {
    if (!document_) {
        if (selected_animation_id_) {
            select_animation(std::nullopt, false);
        }
        return;
    }

    auto ids = document_->animation_ids();
    if (ids.empty()) {
        select_animation(std::nullopt, false);
        return;
    }

    if (selected_animation_id_) {
        if (std::find(ids.begin(), ids.end(), *selected_animation_id_) != ids.end()) {
            if (list_panel_) {
                list_panel_->set_selected_animation_id(selected_animation_id_);
            }
            return;
        }
    }

    std::optional<std::string> candidate;
    if (auto start = document_->start_animation()) {
        if (std::find(ids.begin(), ids.end(), *start) != ids.end()) {
            candidate = *start;
        }
    }
    if (!candidate) {
        candidate = ids.front();
    }
    select_animation(candidate, false);
}

void AnimationEditorWindow::handle_list_context_menu(const std::optional<std::string>& animation_id, const SDL_Point& location) {
    if (!document_) {
        return;
    }
    if (!list_context_menu_) {
        list_context_menu_ = std::make_unique<AnimationListContextMenu>();
    }

    std::vector<AnimationListContextMenu::Option> options;
    options.push_back(AnimationListContextMenu::Option{
        "Create New Animation",
        [this]() { this->open_create_animation_modal(); },
    });
    options.push_back(AnimationListContextMenu::Option{
        "Create Default Animations",
        [this]() { this->open_defaults_modal(); },
    });

    if (animation_id) {
        select_animation(animation_id, false);
        const std::string row_id = *animation_id;
        options.push_back(AnimationListContextMenu::Option{
            "Rename...",
            [this, row_id]() { this->prompt_rename_animation(row_id); },
        });
        options.push_back(AnimationListContextMenu::Option{
            "Set as start",
            [this, row_id]() { this->set_animation_as_start(row_id); },
        });
        options.push_back(AnimationListContextMenu::Option{
            "Duplicate",
            [this, row_id]() { this->duplicate_animation(row_id); },
        });
        options.push_back(AnimationListContextMenu::Option{
            "Delete",
            [this, row_id]() { this->delete_animation_with_confirmation(row_id); },
        });
    }

    list_context_menu_->open(bounds_, location, std::move(options));
    set_status_message(animation_id ? ("Context menu for '" + *animation_id + "'.") : "Animation list actions.", 90);
}

void AnimationEditorWindow::update(const Input& input, int screen_w, int screen_h) {
    if (!visible_) return;
    ++frame_counter_;

    int mouse_x, mouse_y;
    sdl_mouse_util::GetMouseState(&mouse_x, &mouse_y);
    if (mouse_x >= bounds_.x && mouse_x < bounds_.x + bounds_.w &&
        mouse_y >= bounds_.y && mouse_y < bounds_.y + bounds_.h) {
        auto& mutable_input = const_cast<Input&>(input);
        mutable_input.consumeAllMouseButtons();
        mutable_input.consumeMotion();
        mutable_input.consumeScroll();
    }

    ensure_layout();
    maybe_retry_deferred_create_animation_modal();
    maybe_retry_deferred_defaults_modal();
    (void)flush_pending_external_action();

    if (task_queue_) task_queue_->update();
    if (list_panel_) list_panel_->update();
    ensure_selection_valid();
    if (inspector_panel_) {
        if (selected_animation_id_) {
            inspector_panel_->update();
        }
    }
    if (document_ && document_->consume_dirty_flag()) {
        auto_save_pending_ = true;
        auto_save_timer_frames_ = kAutoSaveDelayFrames;
    }

    process_auto_save();

    if (status_timer_frames_ > 0) {
        --status_timer_frames_;
        if (status_timer_frames_ == 0) {
            status_message_.clear();
        }
    }
}

void AnimationEditorWindow::render(SDL_Renderer* renderer) const {
    if (!visible_ || !renderer) return;

    ensure_layout();
    ScopedRendererState renderer_state(renderer);

    render_background(renderer);
    if (list_panel_) list_panel_->render(renderer);
    render_inspector(renderer);
    render_status(renderer);
    if (list_context_menu_ && list_context_menu_->is_open()) {
        list_context_menu_->render(renderer);
    }

    DMDropdown::render_active_options(renderer);
    if (create_animation_modal_actionable()) {
        render_create_animation_modal(renderer);
    }
    if (defaults_modal_actionable()) {
        render_defaults_modal(renderer);
    }

    renderer_state.restore(&last_debug_render_target_);
    first_render_completed_ = true;
}

bool AnimationEditorWindow::handle_event(const SDL_Event& e) {
    if (!visible_) return false;

    ensure_layout();

    if (create_animation_modal_lifecycle_ == ModalLifecycleState::OpenReady && !create_animation_modal_bounds_valid()) {
        create_animation_modal_lifecycle_ = ModalLifecycleState::OpenPendingLayout;
        create_animation_modal_visible_ = false;
        create_animation_modal_open_deferred_ = true;
    }
    if (defaults_modal_lifecycle_ == ModalLifecycleState::OpenReady && !defaults_modal_bounds_valid()) {
        defaults_modal_lifecycle_ = ModalLifecycleState::OpenPendingLayout;
        defaults_modal_visible_ = false;
        defaults_modal_open_deferred_ = true;
    }

    const bool pointer_or_wheel_event =
        (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN ||
         e.type == SDL_EVENT_MOUSE_BUTTON_UP ||
         e.type == SDL_EVENT_MOUSE_MOTION ||
         e.type == SDL_EVENT_MOUSE_WHEEL);
    SDL_Point mp{0, 0};
    bool has_pointer = false;
    if (pointer_or_wheel_event) {
        has_pointer = true;
        if (e.type == SDL_EVENT_MOUSE_MOTION) {
            mp.x = static_cast<int>(std::lround(e.motion.x));
            mp.y = static_cast<int>(std::lround(e.motion.y));
        } else if (e.type == SDL_EVENT_MOUSE_WHEEL) {
            sdl_mouse_util::GetMouseState(&mp.x, &mp.y);
        } else {
            mp.x = static_cast<int>(std::lround(e.button.x));
            mp.y = static_cast<int>(std::lround(e.button.y));
        }
    }
    const bool pointer_in_list = has_pointer && SDL_PointInRect(&mp, &list_rect_);
    const bool pointer_in_inspector = has_pointer && SDL_PointInRect(&mp, &inspector_rect_);
    const bool pointer_in_window = has_pointer && SDL_PointInRect(&mp, &bounds_);

    // 1) Active actionable create-animation modal always wins.
    if (create_animation_modal_actionable()) {
        last_event_route_ = "create_animation_modal";
        if (handle_create_animation_modal_event(e)) {
            return true;
        }
        if (e.type == SDL_EVENT_KEY_DOWN || e.type == SDL_EVENT_TEXT_INPUT || pointer_or_wheel_event) {
            return true;
        }
    }

    // 2) Active actionable defaults modal wins after create-animation modal.
    if (defaults_modal_actionable()) {
        last_event_route_ = "modal";
        if (handle_defaults_modal_event(e)) {
            return true;
        }
        if (e.type == SDL_EVENT_KEY_DOWN || e.type == SDL_EVENT_TEXT_INPUT || pointer_or_wheel_event) {
            return true;
        }
    }

    // 3) List context menu owns input while open.
    if (list_context_menu_ && list_context_menu_->is_open()) {
        last_event_route_ = "list_context_menu";
        if (list_context_menu_->handle_event(e)) {
            return true;
        }
        if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
            SDL_Point p = sdl_mouse_util::ButtonPoint(e.button);
            SDL_Rect menu_bounds = list_context_menu_->bounds();
            if (!SDL_PointInRect(&p, &menu_bounds)) {
                list_context_menu_->close();
            }
            return true;
        }
        if (e.type == SDL_EVENT_KEY_DOWN && e.key.key == SDLK_ESCAPE) {
            list_context_menu_->close();
            return true;
        }
        if (pointer_or_wheel_event || e.type == SDL_EVENT_KEY_DOWN || e.type == SDL_EVENT_TEXT_INPUT) {
            return true;
        }
    }

    // 4) Route pointer events to list first when pointer is inside list.
    if (list_panel_ && pointer_in_list) {
        last_event_route_ = "list";
        if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN && e.button.button == SDL_BUTTON_LEFT) {
            log_ui_transition("event_route", "pointer_to_list");
        }
        if (list_panel_->handle_event(e)) {
            return true;
        }
        if (pointer_or_wheel_event) {
            return true;
        }
    }

    // 5) Inspector handles pointer events only while pointer is inside inspector.
    if (inspector_panel_ && selected_animation_id_) {
        if (pointer_in_inspector && e.type == SDL_EVENT_MOUSE_BUTTON_DOWN && e.button.button == SDL_BUTTON_LEFT) {
            last_event_route_ = "inspector";
            log_ui_transition("event_route", "pointer_to_inspector");
        }
        if (pointer_in_inspector && inspector_panel_->handle_event(e)) {
            return true;
        }
        if (!has_pointer && inspector_panel_->handle_event(e)) {
            return true;
        }
    }

    // 6) Non-pointer list events (e.g. keyboard navigation in future).
    if (list_panel_ && !has_pointer && list_panel_->handle_event(e)) {
        last_event_route_ = "list_non_pointer";
        return true;
    }

    if (auto* active_dd = DMDropdown::active_dropdown()) {
        if (active_dd->handle_event(e)) {
            last_event_route_ = "active_dropdown";
            if (inspector_panel_) {
                inspector_panel_->apply_dropdown_selections();
            }
            return true;
        }
    }

    if (handle_status_event(e)) {
        last_event_route_ = "status";
        return true;
    }

    if (e.type == SDL_EVENT_KEY_DOWN && e.key.key == SDLK_ESCAPE) {
        last_event_route_ = "escape";
        set_visible(false);
        return true;
    }

    if (pointer_or_wheel_event) {
        last_event_route_ = pointer_in_window ? "window_empty" : "outside";
        return pointer_in_window;
    }

    last_event_route_ = "ignored";
    return false;
}

bool AnimationEditorWindow::trigger_add_animation_action() {
    queue_external_action(ExternalActionType::AddAnimation);
    return flush_pending_external_action();
}

bool AnimationEditorWindow::trigger_controller_action() {
    queue_external_action(ExternalActionType::Controller);
    return flush_pending_external_action();
}

bool AnimationEditorWindow::trigger_create_defaults_action() {
    queue_external_action(ExternalActionType::CreateDefaults);
    return flush_pending_external_action();
}

std::string AnimationEditorWindow::controller_action_label() const {
    return does_controller_exist() ? "Open Controller" : "Add Controller";
}

void AnimationEditorWindow::focus_animation(const std::string& animation_id) {
    if (animation_id.empty()) return;
    if (!document_) return;
    auto ids = document_->animation_ids();
    if (std::find(ids.begin(), ids.end(), animation_id) == ids.end()) {
        return;
    }
    select_animation(std::make_optional(animation_id), true);
}

void AnimationEditorWindow::queue_external_action(ExternalActionType type) {
    if (type == ExternalActionType::None) {
        pending_external_action_.clear();
        return;
    }
    pending_external_action_.type = type;
    pending_external_action_.request_revision = ++next_external_action_revision_;
    pending_external_action_.first_seen_frame = frame_counter_;
    std::string detail = "queued:";
    switch (type) {
        case ExternalActionType::AddAnimation: detail += "add_animation"; break;
        case ExternalActionType::Controller: detail += "controller"; break;
        case ExternalActionType::CreateDefaults: detail += "create_defaults"; break;
        case ExternalActionType::None: detail += "none"; break;
    }
    log_ui_transition("action", detail);
}

bool AnimationEditorWindow::execute_external_action(ExternalActionType type) {
    if (!is_ready_for_action_execution()) {
        return false;
    }
    switch (type) {
        case ExternalActionType::AddAnimation:
            if (!document_) {
                return false;
            }
            create_animation_via_prompt();
            return true;
        case ExternalActionType::Controller:
            handle_controller_button_click();
            return true;
        case ExternalActionType::CreateDefaults:
            open_defaults_modal();
            return defaults_modal_actionable();
        case ExternalActionType::None:
        default:
            return false;
    }
}

bool AnimationEditorWindow::flush_pending_external_action() {
    if (!pending_external_action_.active()) {
        return false;
    }
    if (!is_ready_for_action_execution()) {
        set_status_message("Animation editor action deferred until layout is ready.", 120);
        return false;
    }
    if (!execute_external_action(pending_external_action_.type)) {
        set_status_message("Animation editor action deferred; execution failed.", 120);
        return false;
    }
    pending_external_action_.clear();
    return true;
}

std::string AnimationEditorWindow::normalize_animation_name(std::string_view raw) const {
    std::string normalized = animation_editor::strings::trim_copy(raw);
    return animation_editor::strings::to_lower_copy(normalized);
}

void AnimationEditorWindow::prompt_rename_animation(const std::string& animation_id) {
    if (!document_) return;

    std::optional<std::string> input;
    if (text_prompt_override_) {
        input = text_prompt_override_("Rename Animation", "Enter new animation identifier", animation_id);
    } else {
        input = devmode::dialogs::prompt_text(parent_window_, "Rename Animation", "Enter new animation identifier", animation_id);
    }
    if (!input) {
        set_status_message("Rename cancelled.", 120);
        return;
    }

    std::string desired = normalize_animation_name(*input);
    if (desired.empty()) {
        set_status_message("Animation name cannot be empty.", 180);
        return;
    }
    desired = animation_editor::strings::to_lower_copy(desired);

    auto before_ids = document_->animation_ids();
    document_->rename_animation(animation_id, desired);
    auto after_ids = document_->animation_ids();

    std::string new_id = animation_id;
    for (const auto& id : after_ids) {
        if (std::find(before_ids.begin(), before_ids.end(), id) == before_ids.end()) {
            new_id = id;
            break;
        }
    }

    preview_provider_->invalidate(animation_id);
    if (new_id != animation_id) {
        preview_provider_->invalidate(new_id);
    }

    select_animation(std::make_optional(new_id), false);
    set_status_message("Renamed animation to '" + new_id + "'.", 240);
    if (list_context_menu_) {
        list_context_menu_->close();
    }
}

void AnimationEditorWindow::set_animation_as_start(const std::string& animation_id) {
    if (!document_) return;
    document_->set_start_animation(animation_id);
    set_status_message("Set '" + animation_id + "' as start animation.", 180);
    if (list_context_menu_) {
        list_context_menu_->close();
    }
}

void AnimationEditorWindow::duplicate_animation(const std::string& animation_id) {
    if (!document_) return;

    std::string base = normalize_animation_name(animation_id);
    if (base.empty()) {
        base = "animation";
    }
    std::string candidate = base;
    int suffix = 2;
    auto ids = document_->animation_ids();
    while (std::find(ids.begin(), ids.end(), candidate) != ids.end()) {
        candidate = base + "_" + std::to_string(suffix++);
    }
    const auto created = document_->create_animation(candidate);

    if (created == AnimationDocument::CreateAnimationResult::Created) {
        if (auto payload = document_->animation_payload(animation_id)) {
            document_->replace_animation_payload(candidate, *payload);
            preview_provider_->invalidate(candidate);
        }
        select_animation(candidate, false);
        set_status_message("Duplicated animation to '" + candidate + "'.", 240);
    } else {
        set_status_message("Failed to duplicate animation.", 180);
    }

    if (list_context_menu_) {
        list_context_menu_->close();
    }
}

void AnimationEditorWindow::delete_animation_with_confirmation(const std::string& animation_id) {
    if (!document_) return;

    std::string message = "Delete animation '" + animation_id + "'? This cannot be undone.";
    bool confirmed = false;
    if (choice_prompt_override_) {
        auto result = choice_prompt_override_("Delete Animation", message, {0, 1});
        confirmed = result && *result == 1;
    } else {
        confirmed = devmode::dialogs::confirm(parent_window_, "Delete Animation", message, "Delete", "Cancel", true);
    }
    if (!confirmed) {
        set_status_message("Deletion cancelled.", 120);
        if (list_context_menu_) {
            list_context_menu_->close();
        }
        return;
    }

    std::string source_delete_error;
    const bool removed_source_folder = remove_animation_source_folder(animation_id, source_delete_error);

    if (!removed_source_folder && !source_delete_error.empty()) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[AnimationEditor] %s",
                    source_delete_error.c_str());
    }

    document_->delete_animation(animation_id);
    preview_provider_->invalidate(animation_id);
    if (!removed_source_folder) {
        set_status_message("Deleted animation '" + animation_id + "' (some files could not be removed).", 300);
    } else {
        set_status_message("Deleted animation '" + animation_id + "'.", 240);
    }
    if (list_context_menu_) {
        list_context_menu_->close();
    }
    ensure_selection_valid();
}

void AnimationEditorWindow::set_on_document_saved(std::function<void()> callback) {
    on_document_saved_ = std::move(callback);
}

void AnimationEditorWindow::set_on_closed(std::function<void()> callback) {
    on_closed_ = std::move(callback);
}

void AnimationEditorWindow::set_on_animation_properties_changed(std::function<void(const std::string&, const nlohmann::json&)> callback) {
    external_animation_properties_changed_ = std::move(callback);
    refresh_inspector_animation_callback();
}

void AnimationEditorWindow::handle_document_saved() {
    if (on_document_saved_) {
        on_document_saved_();
    }
}

void AnimationEditorWindow::refresh_inspector_animation_callback() {
    if (!inspector_panel_) {
        return;
    }
    inspector_panel_->set_on_animation_properties_changed([this](const std::string& animation_id, const nlohmann::json& payload) {
        if (preview_provider_ && !animation_id.empty()) {
            preview_provider_->invalidate(animation_id);
        }
        if (external_animation_properties_changed_) {
            external_animation_properties_changed_(animation_id, payload);
            return;
        }
        if (document_) {
            const std::string manifest_key = document_->manifest_asset_key_debug();
            if (!orchestrated_save(devmode::core::SaveOrchestrator::Reason::StateChange,
                                   manifest_key.empty() ? std::string("animation-editor") : manifest_key,
                                   [this]() { return document_->save_to_file_checked(true); })) {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                            "AnimationEditorWindow: fallback save failed for animation '%s' (manifest key: '%s').",
                            animation_id.c_str(),
                            manifest_key.empty() ? "<unknown>" : manifest_key.c_str());
            }
        }
    });
}

void AnimationEditorWindow::ensure_layout() const {
    if (layout_dirty_) {
        const_cast<AnimationEditorWindow*>(this)->layout_children();
    }
}

void AnimationEditorWindow::render_background(SDL_Renderer* renderer) const {
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    animation_editor::ui::draw_panel_background(renderer, bounds_);
}

void AnimationEditorWindow::render_status(SDL_Renderer* renderer) const {
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    animation_editor::ui::draw_panel_background(renderer, status_rect_);
    const int padding = DMSpacing::panel_padding();
    const int status_text_y = status_rect_.y + padding + std::max(0, (DMButton::height() - DMStyles::Label().font_size) / 2);
    const std::string message = status_message_.empty() ? std::string{"Ready."} : status_message_;
    render_label(renderer, message, status_rect_.x + padding, status_text_y);
    if (load_details_toggle_button_) {
        load_details_toggle_button_->render(renderer);
    }
    if (load_details_copy_button_) {
        load_details_copy_button_->render(renderer);
    }
    if (load_details_expanded_) {
        render_load_diagnostics(renderer);
    }
}

void AnimationEditorWindow::render_debug_overlay(SDL_Renderer* renderer) const {
    if (!renderer || bounds_.w <= 0 || bounds_.h <= 0) {
        return;
    }

    const int row_count = list_panel_ ? list_panel_->debug_row_count() : 0;
    const std::string hit = list_panel_ ? list_panel_->debug_last_hit_result() : std::string{"none"};
    std::ostringstream ss;
    ss << "AnimEditor debug"
       << " selected=" << (selected_animation_id_ ? *selected_animation_id_ : std::string{"<none>"})
       << " count=" << (document_ ? document_->animation_ids().size() : 0)
       << " rows=" << row_count
       << " list=" << list_rect_.x << "," << list_rect_.y << " " << list_rect_.w << "x" << list_rect_.h
       << " inspector=" << inspector_rect_.x << "," << inspector_rect_.y << " " << inspector_rect_.w << "x" << inspector_rect_.h
       << " route=" << last_event_route_
       << " hit={" << hit << "}"
       << " target=" << last_debug_render_target_;

    const int padding = DMSpacing::small_gap();
    SDL_Rect bg{bounds_.x + padding, bounds_.y + padding, std::max(0, bounds_.w - padding * 2), DMStyles::Label().font_size + padding * 2};
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 190);
    sdl_render::FillRect(renderer, &bg);
    render_label(renderer, ss.str(), bg.x + padding, bg.y + padding);
}

void AnimationEditorWindow::render_load_diagnostics(SDL_Renderer* renderer) const {
    if (!renderer || !load_details_expanded_ || load_details_rect_.w <= 0 || load_details_rect_.h <= 0) {
        return;
    }
    const int line_height = DMStyles::Label().font_size + 4;
    int y = load_details_rect_.y;
    const int x = load_details_rect_.x;
    render_label(renderer, "Manifest Key: " + load_diagnostics_.manifest_key_resolution_result, x, y);
    y += line_height;
    render_label(renderer, "Transaction: " + load_diagnostics_.manifest_transaction_open_result, x, y);
    y += line_height;
    render_label(renderer, "Snapshot Source: " + load_diagnostics_.snapshot_source_chosen, x, y);
    y += line_height;
    render_label(renderer, "Animation Count: " + std::to_string(load_diagnostics_.animation_count_loaded), x, y);
    y += line_height;
    render_label(renderer, std::string("Seeded Default: ") + (load_diagnostics_.seeded_default_applied ? "yes" : "no"), x, y);
    y += line_height;
    render_label(renderer, "Parse Failures: " + std::to_string(load_diagnostics_.parse_failures), x, y);
    y += line_height;
    render_label(renderer, "Normalization Failures: " + std::to_string(load_diagnostics_.normalization_failures), x, y);
}

std::string AnimationEditorWindow::format_load_diagnostics_json() const {
    nlohmann::json payload = nlohmann::json::object({
        {"manifest_key_resolution_result", load_diagnostics_.manifest_key_resolution_result},
        {"manifest_transaction_open_result", load_diagnostics_.manifest_transaction_open_result},
        {"snapshot_source_chosen", load_diagnostics_.snapshot_source_chosen},
        {"animation_count_loaded", load_diagnostics_.animation_count_loaded},
        {"seeded_default_applied", load_diagnostics_.seeded_default_applied},
        {"parse_failures", load_diagnostics_.parse_failures},
        {"normalization_failures", load_diagnostics_.normalization_failures},
    });
    return payload.dump(2);
}

void AnimationEditorWindow::render_inspector(SDL_Renderer* renderer) const {
    if (!renderer) return;
    if (inspector_rect_.w <= 0 || inspector_rect_.h <= 0) {
        return;
    }

    animation_editor::ui::draw_panel_background(renderer, inspector_rect_);

    if (inspector_panel_ && selected_animation_id_) {
        inspector_panel_->render(renderer);
        return;
    }

    std::string message = "Select an animation to edit.";
    int text_x = inspector_rect_.x + DMSpacing::panel_padding();
    int text_y = inspector_rect_.y + DMSpacing::panel_padding();
    render_label(renderer, message, text_x, text_y);
}

bool AnimationEditorWindow::handle_status_event(const SDL_Event& e) {
    auto inside_status = [&](int x, int y) {
        SDL_Point p{x, y};
        return SDL_PointInRect(&p, &status_rect_) != 0;
    };
    const bool status_mouse_event =
        e.type == SDL_EVENT_MOUSE_MOTION || e.type == SDL_EVENT_MOUSE_BUTTON_DOWN || e.type == SDL_EVENT_MOUSE_BUTTON_UP;

    bool handled = false;
    if (load_details_toggle_button_ && load_details_toggle_button_->handle_event(e)) {
        handled = true;
        if (e.type == SDL_EVENT_MOUSE_BUTTON_UP && e.button.button == SDL_BUTTON_LEFT) {
            load_details_expanded_ = !load_details_expanded_;
            layout_dirty_ = true;
        }
    }
    if (load_details_copy_button_ && load_details_copy_button_->handle_event(e)) {
        handled = true;
        if (e.type == SDL_EVENT_MOUSE_BUTTON_UP && e.button.button == SDL_BUTTON_LEFT) {
            const std::string payload = format_load_diagnostics_json();
            if (SDL_SetClipboardText(payload.c_str())) {
                set_status_message("Copied load diagnostics to clipboard.", 180);
            } else {
                set_status_message(std::string("Failed to copy diagnostics: ") + SDL_GetError(), 240);
            }
        }
    }
    if (handled) {
        return true;
    }

    if (!status_mouse_event) {
        return false;
    }
    int x = 0;
    int y = 0;
    if (e.type == SDL_EVENT_MOUSE_MOTION) {
        x = e.motion.x;
        y = e.motion.y;
    } else {
        x = e.button.x;
        y = e.button.y;
    }
    return inside_status(x, y);
}

void AnimationEditorWindow::set_status_message(const std::string& message, int frames) {
    status_message_ = message;
    status_timer_frames_ = std::max(frames, 0);
}

void AnimationEditorWindow::log_ui_transition(const char* tag, const std::string& detail) const {
    SDL_Log("[AnimationEditor][%s] %s", tag ? tag : "event", detail.c_str());
}

void AnimationEditorWindow::ensure_create_animation_modal_widgets() {
    if (!create_animation_name_box_) {
        create_animation_name_box_ = std::make_unique<DMTextBox>("Animation id", "animation");
    }
    if (!create_animation_create_button_) {
        create_animation_create_button_ = std::make_unique<DMButton>("Create", &DMStyles::CreateButton(), 120, DMButton::height());
    }
    if (!create_animation_cancel_button_) {
        create_animation_cancel_button_ = std::make_unique<DMButton>("Cancel", &DMStyles::HeaderButton(), 120, DMButton::height());
    }
}

bool AnimationEditorWindow::create_animation_modal_bounds_valid() const {
    return is_layout_valid() &&
           bounds_.w > 0 && bounds_.h > 0 &&
           create_animation_modal_rect_.w > 0 && create_animation_modal_rect_.h > 0;
}

bool AnimationEditorWindow::create_animation_modal_actionable() const {
    return visible_ &&
           create_animation_modal_lifecycle_ == ModalLifecycleState::OpenReady &&
           create_animation_modal_visible_ &&
           create_animation_modal_bounds_valid();
}

bool AnimationEditorWindow::try_open_create_animation_modal(bool from_retry) {
    ensure_create_animation_modal_widgets();
    if (list_context_menu_) {
        list_context_menu_->close();
    }
    close_defaults_modal();
    if (create_animation_name_box_ && !from_retry) {
        create_animation_name_box_->set_value("animation");
        create_animation_name_box_->start_editing();
    }
    create_animation_modal_lifecycle_ = ModalLifecycleState::OpenReady;
    create_animation_modal_visible_ = true;
    layout_create_animation_modal();
    if (create_animation_modal_actionable()) {
        create_animation_modal_open_deferred_ = false;
        create_animation_modal_lifecycle_ = ModalLifecycleState::OpenReady;
        log_ui_transition("create_animation_modal", from_retry ? "opened_after_retry" : "opened");
        return true;
    }

    create_animation_modal_open_deferred_ = true;
    create_animation_modal_visible_ = false;
    create_animation_modal_lifecycle_ = ModalLifecycleState::OpenPendingLayout;
    if (!from_retry) {
        if (!create_animation_modal_warning_.empty()) {
            set_status_message(create_animation_modal_warning_, 240);
        } else {
            set_status_message("Create Animation is waiting for valid editor bounds.", 240);
        }
        log_ui_transition("create_animation_modal", "deferred_open_initial_failed");
    }
    return false;
}

void AnimationEditorWindow::maybe_retry_deferred_create_animation_modal() {
    if (!create_animation_modal_open_deferred_ &&
        create_animation_modal_lifecycle_ != ModalLifecycleState::OpenPendingLayout) {
        return;
    }
    if (!is_layout_valid()) {
        return;
    }
    (void)try_open_create_animation_modal(true);
}

void AnimationEditorWindow::open_create_animation_modal() {
    (void)try_open_create_animation_modal(false);
}

void AnimationEditorWindow::close_create_animation_modal() {
    if (create_animation_create_button_) {
        create_animation_create_button_->cancel_interaction();
    }
    if (create_animation_cancel_button_) {
        create_animation_cancel_button_->cancel_interaction();
    }
    if (create_animation_name_box_) {
        create_animation_name_box_->stop_editing();
    }
    if (create_animation_modal_visible_ || create_animation_modal_open_deferred_ ||
        create_animation_modal_lifecycle_ == ModalLifecycleState::OpenReady ||
        create_animation_modal_lifecycle_ == ModalLifecycleState::OpenPendingLayout) {
        log_ui_transition("create_animation_modal", "closed");
    }
    create_animation_modal_visible_ = false;
    create_animation_modal_open_deferred_ = false;
    create_animation_modal_lifecycle_ = ModalLifecycleState::Closed;
    create_animation_modal_rect_ = SDL_Rect{0, 0, 0, 0};
    create_animation_modal_warning_.clear();
}

void AnimationEditorWindow::layout_create_animation_modal() {
    if (create_animation_modal_lifecycle_ != ModalLifecycleState::OpenReady || !create_animation_modal_visible_) {
        create_animation_modal_rect_ = SDL_Rect{0, 0, 0, 0};
        return;
    }

    ensure_create_animation_modal_widgets();
    if (!is_layout_valid() || bounds_.w <= 0 || bounds_.h <= 0) {
        create_animation_modal_visible_ = false;
        create_animation_modal_rect_ = SDL_Rect{0, 0, 0, 0};
        create_animation_modal_open_deferred_ = true;
        create_animation_modal_lifecycle_ = ModalLifecycleState::OpenPendingLayout;
        create_animation_modal_warning_ = "Create Animation cannot open while panel bounds are collapsed.";
        return;
    }
    create_animation_modal_warning_.clear();

    const int modal_width = std::clamp(bounds_.w - 120, 320, 520);
    const int padding = DMSpacing::panel_padding();
    const int row_gap = DMSpacing::small_gap();
    const int input_h = create_animation_name_box_ ? create_animation_name_box_->preferred_height(modal_width - padding * 2) : DMTextBox::height();
    const int modal_height = padding * 3 + DMStyles::Label().font_size * 2 + row_gap * 2 + input_h + DMButton::height();
    create_animation_modal_rect_ = SDL_Rect{
        bounds_.x + std::max(0, (bounds_.w - modal_width) / 2),
        bounds_.y + std::max(0, (bounds_.h - modal_height) / 2),
        modal_width,
        modal_height,
    };

    const int content_x = create_animation_modal_rect_.x + padding;
    const int content_w = std::max(0, create_animation_modal_rect_.w - padding * 2);
    int y = create_animation_modal_rect_.y + padding + DMStyles::Label().font_size * 2 + row_gap * 2;
    if (create_animation_name_box_) {
        create_animation_name_box_->set_rect(SDL_Rect{content_x, y, content_w, input_h});
    }

    const int footer_y = create_animation_modal_rect_.y + create_animation_modal_rect_.h - padding - DMButton::height();
    if (create_animation_cancel_button_ && create_animation_create_button_) {
        const int button_gap = DMSpacing::small_gap();
        const int cancel_w = 120;
        const int create_w = 130;
        const int create_x = create_animation_modal_rect_.x + create_animation_modal_rect_.w - padding - create_w;
        const int cancel_x = create_x - button_gap - cancel_w;
        create_animation_cancel_button_->set_rect(SDL_Rect{cancel_x, footer_y, cancel_w, DMButton::height()});
        create_animation_create_button_->set_rect(SDL_Rect{create_x, footer_y, create_w, DMButton::height()});
    }
}

void AnimationEditorWindow::handle_create_animation_modal_submit() {
    if (!document_ || !create_animation_name_box_) {
        set_status_message("Animation document is unavailable.", 180);
        return;
    }

    const std::string name = normalize_animation_name(create_animation_name_box_->value());
    if (name.empty()) {
        set_status_message("Animation name is invalid after normalization.", 240);
        return;
    }
    if (animation_editor::strings::is_reserved_animation_name(name)) {
        set_status_message("Animation name '" + name + "' is reserved.", 240);
        return;
    }

    const auto before_ids = document_->animation_ids();
    if (std::find(before_ids.begin(), before_ids.end(), name) != before_ids.end()) {
        select_animation(std::make_optional(name), false);
        refresh_panels_after_load();
        set_status_message("Animation '" + name + "' already exists; selected existing animation.", 240);
        return;
    }

    const auto create_result = document_->create_animation(name);
    const auto after_ids = document_->animation_ids();
    const bool inserted = std::find(after_ids.begin(), after_ids.end(), name) != after_ids.end();
    if (create_result == AnimationDocument::CreateAnimationResult::Created && inserted) {
        if (preview_provider_) {
            preview_provider_->invalidate_all();
        }
        select_animation(std::make_optional(name), false);
        refresh_panels_after_load();
        close_create_animation_modal();
        set_status_message("Created animation '" + name + "'.", 240);
    } else if (create_result == AnimationDocument::CreateAnimationResult::AlreadyExists) {
        set_status_message("Animation '" + name + "' already exists.", 240);
    } else if (create_result == AnimationDocument::CreateAnimationResult::InvalidName) {
        set_status_message("Animation name '" + name + "' is invalid.", 240);
    } else if (create_result == AnimationDocument::CreateAnimationResult::Created && !inserted) {
        set_status_message("Animation create failed verification for '" + name + "'.", 260);
    } else {
        set_status_message("Failed to create animation '" + name + "'.", 240);
    }
}

bool AnimationEditorWindow::handle_create_animation_modal_event(const SDL_Event& e) {
    if (!create_animation_modal_actionable()) {
        return false;
    }

    ensure_create_animation_modal_widgets();
    layout_create_animation_modal();
    if (!create_animation_modal_actionable()) {
        return false;
    }

    bool consumed = false;
    if (create_animation_name_box_ && create_animation_name_box_->handle_event(e)) {
        consumed = true;
    }

    auto handle_button = [&](const std::unique_ptr<DMButton>& button, auto&& callback) {
        if (!button) return;
        if (!button->handle_event(e)) return;
        consumed = true;
        if (e.type == SDL_EVENT_MOUSE_BUTTON_UP && e.button.button == SDL_BUTTON_LEFT) {
            callback();
        }
    };

    handle_button(create_animation_create_button_, [this]() { handle_create_animation_modal_submit(); });
    handle_button(create_animation_cancel_button_, [this]() { close_create_animation_modal(); });

    if (e.type == SDL_EVENT_KEY_DOWN) {
        if (e.key.key == SDLK_ESCAPE) {
            close_create_animation_modal();
            return true;
        }
        if (e.key.key == SDLK_RETURN) {
            handle_create_animation_modal_submit();
            return true;
        }
        return true;
    }
    if (e.type == SDL_EVENT_TEXT_INPUT) {
        return true;
    }

    SDL_Point point{0, 0};
    bool pointer_event = false;
    if (e.type == SDL_EVENT_MOUSE_MOTION) {
        point = SDL_Point{static_cast<int>(std::lround(e.motion.x)), static_cast<int>(std::lround(e.motion.y))};
        pointer_event = true;
    } else if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN || e.type == SDL_EVENT_MOUSE_BUTTON_UP) {
        point = SDL_Point{static_cast<int>(std::lround(e.button.x)), static_cast<int>(std::lround(e.button.y))};
        pointer_event = true;
    } else if (e.type == SDL_EVENT_MOUSE_WHEEL) {
        int mx = 0;
        int my = 0;
        sdl_mouse_util::GetMouseState(&mx, &my);
        point = SDL_Point{mx, my};
        pointer_event = true;
    }

    if (pointer_event) {
        const bool inside_modal = SDL_PointInRect(&point, &create_animation_modal_rect_);
        if (inside_modal) {
            return true;
        }
        if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN && e.button.button == SDL_BUTTON_LEFT) {
            close_create_animation_modal();
            return true;
        }
        if (e.type == SDL_EVENT_MOUSE_BUTTON_UP || e.type == SDL_EVENT_MOUSE_WHEEL) {
            return true;
        }
    }

    return consumed;
}

void AnimationEditorWindow::render_create_animation_modal(SDL_Renderer* renderer) const {
    if (!renderer || !create_animation_modal_actionable()) {
        return;
    }

    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 150);
    SDL_Rect scrim = bounds_;
    sdl_render::FillRect(renderer, &scrim);

    dm_draw::DrawBeveledRect(renderer,
                             create_animation_modal_rect_,
                             DMStyles::CornerRadius(),
                             DMStyles::BevelDepth(),
                             DMStyles::PanelBG(),
                             DMStyles::HighlightColor(),
                             DMStyles::ShadowColor(),
                             false,
                             DMStyles::HighlightIntensity(),
                             DMStyles::ShadowIntensity());
    dm_draw::DrawRoundedOutline(renderer, create_animation_modal_rect_, DMStyles::CornerRadius(), 1, DMStyles::Border());

    const int padding = DMSpacing::panel_padding();
    const int title_x = create_animation_modal_rect_.x + padding;
    int title_y = create_animation_modal_rect_.y + padding;
    render_label(renderer, "Create Animation", title_x, title_y);
    title_y += DMStyles::Label().font_size + DMSpacing::small_gap();
    render_label(renderer, "Enter a unique animation id.", title_x, title_y);

    if (create_animation_name_box_) create_animation_name_box_->render(renderer);
    if (create_animation_create_button_) create_animation_create_button_->render(renderer);
    if (create_animation_cancel_button_) create_animation_cancel_button_->render(renderer);
}

void AnimationEditorWindow::ensure_defaults_modal_widgets() {
    if (!defaults_diagonals_checkbox_) {
        defaults_diagonals_checkbox_ = std::make_unique<DMCheckbox>("Ground Diagonals", true);
    }
    if (!defaults_basic_movement_checkbox_) {
        defaults_basic_movement_checkbox_ = std::make_unique<DMCheckbox>("Basic Movement", true);
    }
    if (!defaults_elevation_checkbox_) {
        defaults_elevation_checkbox_ = std::make_unique<DMCheckbox>("Elevation", false);
    }
    if (!defaults_3d_diagonals_checkbox_) {
        defaults_3d_diagonals_checkbox_ = std::make_unique<DMCheckbox>("3D Diagonals (8-Way)", false);
    }
    if (!defaults_base_faces_right_checkbox_) {
        defaults_base_faces_right_checkbox_ = std::make_unique<DMCheckbox>("Base Frames Face Right", false);
    }
    if (!defaults_distance_box_) {
        defaults_distance_box_ = std::make_unique<DMTextBox>("Total movement per animation", "5");
    }
    if (!defaults_base_frames_button_) {
        defaults_base_frames_button_ = std::make_unique<DMButton>("Add Base Movement Animation", &DMStyles::AccentButton(), 260, DMButton::height());
    }
    if (!defaults_create_button_) {
        defaults_create_button_ = std::make_unique<DMButton>("Create", &DMStyles::CreateButton(), 120, DMButton::height());
    }
    if (!defaults_cancel_button_) {
        defaults_cancel_button_ = std::make_unique<DMButton>("Cancel", &DMStyles::HeaderButton(), 120, DMButton::height());
    }
}

bool AnimationEditorWindow::defaults_modal_bounds_valid() const {
    return is_layout_valid() &&
           bounds_.w > 0 && bounds_.h > 0 &&
           defaults_modal_rect_.w > 0 && defaults_modal_rect_.h > 0;
}

bool AnimationEditorWindow::defaults_modal_actionable() const {
    return visible_ &&
           defaults_modal_lifecycle_ == ModalLifecycleState::OpenReady &&
           defaults_modal_visible_ &&
           defaults_modal_bounds_valid();
}

bool AnimationEditorWindow::try_open_defaults_modal(bool from_retry) {
    ensure_defaults_modal_widgets();
    if (list_context_menu_) {
        list_context_menu_->close();
    }
    close_create_animation_modal();
    defaults_modal_lifecycle_ = ModalLifecycleState::OpenReady;
    defaults_modal_visible_ = true;
    layout_defaults_modal();
    if (defaults_modal_actionable()) {
        defaults_modal_open_deferred_ = false;
        defaults_modal_lifecycle_ = ModalLifecycleState::OpenReady;
        log_ui_transition("defaults_modal", from_retry ? "opened_after_retry" : "opened");
        return true;
    }

    defaults_modal_open_deferred_ = true;
    defaults_modal_visible_ = false;
    defaults_modal_lifecycle_ = ModalLifecycleState::OpenPendingLayout;
    if (!from_retry) {
        if (!defaults_modal_open_warning_.empty()) {
            set_status_message(defaults_modal_open_warning_, 240);
        } else {
            set_status_message("Create Defaults is waiting for valid editor bounds.", 240);
        }
    }
    if (!from_retry) {
        log_ui_transition("defaults_modal", "deferred_open_initial_failed");
    }
    return false;
}

void AnimationEditorWindow::maybe_retry_deferred_defaults_modal() {
    if (!defaults_modal_open_deferred_ && defaults_modal_lifecycle_ != ModalLifecycleState::OpenPendingLayout) {
        return;
    }
    if (!is_layout_valid()) {
        return;
    }
    (void)try_open_defaults_modal(true);
}

void AnimationEditorWindow::open_defaults_modal() {
    (void)try_open_defaults_modal(false);
}

void AnimationEditorWindow::close_defaults_modal() {
    if (defaults_base_frames_button_) {
        defaults_base_frames_button_->cancel_interaction();
    }
    if (defaults_create_button_) {
        defaults_create_button_->cancel_interaction();
    }
    if (defaults_cancel_button_) {
        defaults_cancel_button_->cancel_interaction();
    }
    if (defaults_distance_box_) {
        defaults_distance_box_->stop_editing();
    }
    if (defaults_modal_visible_ || defaults_modal_open_deferred_ ||
        defaults_modal_lifecycle_ == ModalLifecycleState::OpenReady ||
        defaults_modal_lifecycle_ == ModalLifecycleState::OpenPendingLayout) {
        log_ui_transition("defaults_modal", "closed");
    }
    defaults_modal_visible_ = false;
    defaults_modal_open_deferred_ = false;
    defaults_modal_lifecycle_ = ModalLifecycleState::Closed;
    defaults_modal_rect_ = SDL_Rect{0, 0, 0, 0};
    defaults_modal_scroll_rect_ = SDL_Rect{0, 0, 0, 0};
    defaults_modal_scroll_offset_ = 0;
    defaults_modal_scroll_max_ = 0;
}


void AnimationEditorWindow::layout_defaults_modal() {
    if (defaults_modal_lifecycle_ != ModalLifecycleState::OpenReady || !defaults_modal_visible_) {
        defaults_modal_rect_ = SDL_Rect{0, 0, 0, 0};
        defaults_modal_scroll_rect_ = SDL_Rect{0, 0, 0, 0};
        defaults_modal_scroll_offset_ = 0;
        defaults_modal_scroll_max_ = 0;
        return;
    }

    ensure_defaults_modal_widgets();

    if (!is_layout_valid() || bounds_.w <= 0 || bounds_.h <= 0) {
        defaults_modal_visible_ = false;
        defaults_modal_rect_ = SDL_Rect{0, 0, 0, 0};
        defaults_modal_scroll_rect_ = SDL_Rect{0, 0, 0, 0};
        defaults_modal_open_deferred_ = true;
        defaults_modal_lifecycle_ = ModalLifecycleState::OpenPendingLayout;
        defaults_modal_open_warning_ = "Create Defaults cannot open while panel bounds are collapsed.";
        return;
    }
    defaults_modal_open_warning_.clear();

    const int modal_width = std::clamp(bounds_.w - 120, 320, 720);
    const int modal_height = std::clamp(455, 180, std::max(180, bounds_.h - 20));
    defaults_modal_rect_ = SDL_Rect{bounds_.x + std::max(0, (bounds_.w - modal_width) / 2), bounds_.y + std::max(0, (bounds_.h - modal_height) / 2), modal_width, modal_height};

    const int padding = DMSpacing::panel_padding();
    const int row_gap = DMSpacing::small_gap();
    const int content_x = defaults_modal_rect_.x + padding;
    const int content_w = std::max(0, defaults_modal_rect_.w - padding * 2);
    const int title_y = defaults_modal_rect_.y + padding;
    const int title_block_h = DMStyles::Label().font_size * 2 + row_gap + 6;
    const int scroll_top = title_y + title_block_h;
    const int footer_h = DMButton::height();
    const int info_h = DMStyles::Label().font_size;
    const int scroll_bottom = defaults_modal_rect_.y + defaults_modal_rect_.h - padding - footer_h - row_gap - info_h - row_gap;
    const int scroll_h = std::max(0, scroll_bottom - scroll_top);
    defaults_modal_scroll_rect_ = SDL_Rect{content_x, scroll_top, content_w, scroll_h};
    int y = scroll_top - defaults_modal_scroll_offset_;

    if (defaults_basic_movement_checkbox_) { defaults_basic_movement_checkbox_->set_rect(SDL_Rect{content_x, y, content_w, DMCheckbox::height()}); y += DMCheckbox::height() + row_gap; }
    if (defaults_diagonals_checkbox_) { defaults_diagonals_checkbox_->set_rect(SDL_Rect{content_x, y, content_w, DMCheckbox::height()}); y += DMCheckbox::height() + row_gap; }
    if (defaults_elevation_checkbox_) { defaults_elevation_checkbox_->set_rect(SDL_Rect{content_x, y, content_w, DMCheckbox::height()}); y += DMCheckbox::height() + row_gap; }
    if (defaults_3d_diagonals_checkbox_) { defaults_3d_diagonals_checkbox_->set_rect(SDL_Rect{content_x, y, content_w, DMCheckbox::height()}); y += DMCheckbox::height() + row_gap; }
    if (defaults_base_faces_right_checkbox_) { defaults_base_faces_right_checkbox_->set_rect(SDL_Rect{content_x, y, content_w, DMCheckbox::height()}); y += DMCheckbox::height() + row_gap; }
    if (defaults_distance_box_) { const int distance_h = defaults_distance_box_->preferred_height(content_w); defaults_distance_box_->set_rect(SDL_Rect{content_x, y, content_w, distance_h}); y += distance_h + row_gap; }
    if (defaults_base_frames_button_) {
        defaults_base_frames_button_->set_text(defaults_base_frame_paths_.empty() ? "Add Base Movement Animation" : "Base Frames (" + std::to_string(defaults_base_frame_paths_.size()) + ")");
        const int button_w = std::min(content_w, std::max(280, defaults_base_frames_button_->preferred_width() + 24));
        defaults_base_frames_button_->set_rect(SDL_Rect{content_x, y, button_w, DMButton::height()});
        y += DMButton::height() + row_gap;
    }

    const int content_total_height = (y + defaults_modal_scroll_offset_) - scroll_top;
    defaults_modal_scroll_max_ = std::max(0, content_total_height - scroll_h);
    defaults_modal_scroll_offset_ = std::clamp(defaults_modal_scroll_offset_, 0, defaults_modal_scroll_max_);

    const int footer_y = defaults_modal_rect_.y + defaults_modal_rect_.h - padding - DMButton::height();
    if (defaults_cancel_button_ && defaults_create_button_) {
        const int button_gap = DMSpacing::small_gap();
        const int cancel_w = 120;
        const int create_w = 130;
        const int create_x = defaults_modal_rect_.x + defaults_modal_rect_.w - padding - create_w;
        const int cancel_x = create_x - button_gap - cancel_w;
        defaults_cancel_button_->set_rect(SDL_Rect{cancel_x, footer_y, cancel_w, DMButton::height()});
        defaults_create_button_->set_rect(SDL_Rect{create_x, footer_y, create_w, DMButton::height()});
    }
}

bool AnimationEditorWindow::handle_defaults_modal_event(const SDL_Event& e) {
    if (!defaults_modal_actionable()) {
        return false;
    }

    ensure_defaults_modal_widgets();
    layout_defaults_modal();
    if (!defaults_modal_actionable()) {
        return false;
    }

    bool consumed = false;
    if (defaults_basic_movement_checkbox_ && defaults_basic_movement_checkbox_->handle_event(e)) {
        consumed = true;
    }
    if (defaults_diagonals_checkbox_ && defaults_diagonals_checkbox_->handle_event(e)) {
        consumed = true;
    }
    if (defaults_elevation_checkbox_ && defaults_elevation_checkbox_->handle_event(e)) {
        consumed = true;
    }
    if (defaults_3d_diagonals_checkbox_ && defaults_3d_diagonals_checkbox_->handle_event(e)) {
        consumed = true;
    }
    if (defaults_base_faces_right_checkbox_ && defaults_base_faces_right_checkbox_->handle_event(e)) {
        consumed = true;
    }
    if (defaults_distance_box_ && defaults_distance_box_->handle_event(e)) {
        consumed = true;
    }

    auto handle_button = [&](const std::unique_ptr<DMButton>& button, auto&& callback) {
        if (!button) return;
        if (!button->handle_event(e)) return;
        consumed = true;
        if (e.type == SDL_EVENT_MOUSE_BUTTON_UP && e.button.button == SDL_BUTTON_LEFT) {
            callback();
        }
    };

    handle_button(defaults_base_frames_button_, [this]() { handle_pick_defaults_base_frames(); });
    handle_button(defaults_create_button_, [this]() { handle_create_defaults(); });
    handle_button(defaults_cancel_button_, [this]() { close_defaults_modal(); });

    if (e.type == SDL_EVENT_KEY_DOWN) {
        if (e.key.key == SDLK_ESCAPE) {
            close_defaults_modal();
            return true;
        }
        if (e.key.key == SDLK_RETURN && (!defaults_distance_box_ || !defaults_distance_box_->is_editing())) {
            handle_create_defaults();
            return true;
        }
        return true;
    }
    if (e.type == SDL_EVENT_TEXT_INPUT) {
        return true;
    }
    if (e.type == SDL_EVENT_MOUSE_WHEEL && defaults_modal_scroll_rect_.h > 0) {
        int mx = 0;
        int my = 0;
        sdl_mouse_util::GetMouseState(&mx, &my);
        SDL_Point mp{mx, my};
        if (SDL_PointInRect(&mp, &defaults_modal_scroll_rect_) && defaults_modal_scroll_max_ > 0) {
            const int delta = static_cast<int>(std::lround(e.wheel.y)) * 24;
            defaults_modal_scroll_offset_ = std::clamp(defaults_modal_scroll_offset_ - delta, 0, defaults_modal_scroll_max_);
            layout_defaults_modal();
            return true;
        }
    }

    SDL_Point point{0, 0};
    bool pointer_event = false;
    if (e.type == SDL_EVENT_MOUSE_MOTION) {
        point = SDL_Point{static_cast<int>(std::lround(e.motion.x)), static_cast<int>(std::lround(e.motion.y))};
        pointer_event = true;
    } else if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN || e.type == SDL_EVENT_MOUSE_BUTTON_UP) {
        point = SDL_Point{static_cast<int>(std::lround(e.button.x)), static_cast<int>(std::lround(e.button.y))};
        pointer_event = true;
    } else if (e.type == SDL_EVENT_MOUSE_WHEEL) {
        int mx = 0;
        int my = 0;
        sdl_mouse_util::GetMouseState(&mx, &my);
        point = SDL_Point{mx, my};
        pointer_event = true;
    }

    if (pointer_event) {
        const bool inside_modal = SDL_PointInRect(&point, &defaults_modal_rect_);
        if (inside_modal) {
            return true;
        }
        if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN && e.button.button == SDL_BUTTON_LEFT) {
            close_defaults_modal();
            return true;
        }
        if (e.type == SDL_EVENT_MOUSE_BUTTON_UP || e.type == SDL_EVENT_MOUSE_WHEEL) {
            return true;
        }
    }

    return consumed;
}

void AnimationEditorWindow::render_defaults_modal(SDL_Renderer* renderer) const {
    if (!renderer || !defaults_modal_actionable()) {
        return;
    }

    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 150);
    SDL_Rect scrim = bounds_;
    sdl_render::FillRect(renderer, &scrim);

    dm_draw::DrawBeveledRect(renderer,
                             defaults_modal_rect_,
                             DMStyles::CornerRadius(),
                             DMStyles::BevelDepth(),
                             DMStyles::PanelBG(),
                             DMStyles::HighlightColor(),
                             DMStyles::ShadowColor(),
                             false,
                             DMStyles::HighlightIntensity(),
                             DMStyles::ShadowIntensity());
    dm_draw::DrawRoundedOutline(renderer, defaults_modal_rect_, DMStyles::CornerRadius(), 1, DMStyles::Border());

    const int padding = DMSpacing::panel_padding();
    const int title_x = defaults_modal_rect_.x + padding;
    int title_y = defaults_modal_rect_.y + padding;
    render_label(renderer, "Create Defaults", title_x, title_y);
    title_y += DMStyles::Label().font_size + DMSpacing::small_gap();
    render_label(renderer, "Generate canonical [dx, dy, dz] data (per-frame = total / frames).", title_x, title_y);

    SDL_SetRenderClipRect(renderer, &defaults_modal_scroll_rect_);
    if (defaults_basic_movement_checkbox_) defaults_basic_movement_checkbox_->render(renderer);
    if (defaults_diagonals_checkbox_) defaults_diagonals_checkbox_->render(renderer);
    if (defaults_elevation_checkbox_) defaults_elevation_checkbox_->render(renderer);
    if (defaults_3d_diagonals_checkbox_) defaults_3d_diagonals_checkbox_->render(renderer);
    if (defaults_base_faces_right_checkbox_) defaults_base_faces_right_checkbox_->render(renderer);
    if (defaults_distance_box_) defaults_distance_box_->render(renderer);
    if (defaults_base_frames_button_) defaults_base_frames_button_->render(renderer);
    SDL_SetRenderClipRect(renderer, nullptr);
    if (defaults_create_button_) defaults_create_button_->render(renderer);
    if (defaults_cancel_button_) defaults_cancel_button_->render(renderer);

    const int info_x = defaults_modal_rect_.x + padding;
    const int info_y = defaults_modal_rect_.y + defaults_modal_rect_.h - padding - DMButton::height() - DMSpacing::small_gap() - DMStyles::Label().font_size;
    if (defaults_base_frame_paths_.empty()) {
        render_label(renderer, "Base frames: none selected.", info_x, info_y);
    } else {
        std::string summary = "Base frames: " + std::to_string(defaults_base_frame_paths_.size()) +
                              " selected (" + defaults_base_frame_paths_.front().filename().string();
        if (defaults_base_frame_paths_.size() > 1) {
            summary += " ...";
        }
        summary += ")";
        render_label(renderer, summary, info_x, info_y);
    }
}

void AnimationEditorWindow::handle_pick_defaults_base_frames() {
    auto pick_result = pick_png_sequence();
    if (pick_result.status != devmode::dialogs::FileDialogStatus::Selected) {
        set_status_message("Base frame selection was not completed.", 120);
        return;
    }
    if (pick_result.paths.empty()) {
        set_status_message("Base frame selection returned no files.", 180);
        return;
    }

    std::vector<std::filesystem::path> filtered;
    filtered.reserve(pick_result.paths.size());
    for (const auto& path : pick_result.paths) {
        if (devmode::frame_importer::is_supported_image_file(path) &&
            !devmode::frame_importer::is_gif_file(path)) {
            filtered.push_back(path);
        }
    }
    if (filtered.empty()) {
        set_status_message("No supported image frames selected.", 180);
        return;
    }

    defaults_base_frame_paths_ = normalize_sequence_paths(filtered);
    layout_defaults_modal();
    set_status_message("Selected " + std::to_string(defaults_base_frame_paths_.size()) + " base frame(s).", 180);
}

std::optional<int> AnimationEditorWindow::parse_defaults_total_movement() const {
    if (!defaults_distance_box_) {
        return std::nullopt;
    }
    std::string raw = animation_editor::strings::trim_copy(defaults_distance_box_->value());
    if (raw.empty()) {
        return std::nullopt;
    }

    std::size_t index = 0;
    int value = 0;
    try {
        value = std::stoi(raw, &index);
    } catch (...) {
        return std::nullopt;
    }
    if (index != raw.size() || value <= 0) {
        return std::nullopt;
    }
    return value;
}

bool AnimationEditorWindow::defaults_base_faces_right() const {
    return defaults_base_faces_right_checkbox_ && defaults_base_faces_right_checkbox_->value();
}

bool AnimationEditorWindow::copy_frames_to_animation_folder(const std::string& animation_id,
                                                            const std::vector<std::filesystem::path>& frames) {
    const SourceFrameImportOutcome outcome = import_source_frames_for_animation(animation_id, frames);
    return outcome.success;
}

// QA checklist invariants for transactional frame imports:
// 1) Imported source folder must contain canonical frame filenames: 0..n-1.png
// 2) Payload source.path + payload number_of_frames must match imported folder state
// 3) Runtime cache must report the same frame count as payload/source folder
// 4) Runtime texture presence must be verified for every expected frame
AnimationEditorWindow::SourceFrameImportOutcome AnimationEditorWindow::import_source_frames_for_animation(
    const std::string& animation_id,
    const std::vector<std::filesystem::path>& frames) {
    SourceFrameImportOutcome outcome;
    auto fail = [&](SourceFrameImportOutcome::FailureStage stage, const std::string& message) {
        outcome.success = false;
        outcome.failure_stage = stage;
        outcome.message = message;
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[AnimationEditor][frame_import] failed animation='%s' source_count=%zu stage=%d target='%s': %s",
                     animation_id.c_str(),
                     frames.size(),
                     static_cast<int>(stage),
                     asset_root_path_.generic_string().c_str(),
                     message.c_str());
        return outcome;
    };

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[AnimationEditor][frame_import] start animation='%s' source_count=%zu asset_root='%s'",
                animation_id.c_str(),
                frames.size(),
                asset_root_path_.generic_string().c_str());

    if (!document_) {
        return fail(SourceFrameImportOutcome::FailureStage::Payload, "Animation document is unavailable.");
    }
    if (animation_id.empty()) {
        return fail(SourceFrameImportOutcome::FailureStage::Payload, "Animation id is empty.");
    }
    if (frames.empty()) {
        return fail(SourceFrameImportOutcome::FailureStage::Copy, "No image files were provided.");
    }
    if (asset_root_path_.empty()) {
        return fail(SourceFrameImportOutcome::FailureStage::Copy, "Asset directory is unavailable.");
    }

    std::error_code ec;
    std::filesystem::create_directories(asset_root_path_, ec);
    if (ec) {
        return fail(SourceFrameImportOutcome::FailureStage::Copy, "Failed to create asset folder '" + asset_root_path_.generic_string() + "': " + ec.message());
    }

    const std::filesystem::path output_dir = asset_root_path_ / animation_id;
    const std::optional<nlohmann::json> previous_payload = document_->animation_payload_json(animation_id);
    const bool animation_existed = previous_payload.has_value();

    std::filesystem::path rollback_backup;
    const bool output_existed = std::filesystem::exists(output_dir, ec);
    if (ec) {
        return fail(SourceFrameImportOutcome::FailureStage::Copy, "Failed to inspect existing frame folder '" + output_dir.generic_string() + "': " + ec.message());
    }
    if (output_existed) {
        const auto now = std::chrono::high_resolution_clock::now().time_since_epoch().count();
        bool allocated = false;
        for (int attempt = 0; attempt < 64; ++attempt) {
            rollback_backup = asset_root_path_ /
                (".frame_import_editor_backup_" + animation_id + "_" +
                 std::to_string(static_cast<long long>(now)) + "_" + std::to_string(attempt));
            std::error_code exists_ec;
            if (!std::filesystem::exists(rollback_backup, exists_ec) && !exists_ec) {
                allocated = true;
                break;
            }
        }
        if (!allocated) {
            return fail(SourceFrameImportOutcome::FailureStage::Copy, "Failed to allocate rollback backup for '" + output_dir.generic_string() + "'.");
        }
        std::filesystem::rename(output_dir, rollback_backup, ec);
        if (ec) {
            return fail(SourceFrameImportOutcome::FailureStage::Copy, "Failed to backup existing frame folder '" + output_dir.generic_string() + "': " + ec.message());
        }
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "[AnimationEditor][frame_import] backed up '%s' -> '%s'",
                    output_dir.generic_string().c_str(),
                    rollback_backup.generic_string().c_str());
    }

    auto restore_folder_backup = [&](const std::string& rollback_reason) {
        if (rollback_backup.empty()) {
            return;
        }
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[AnimationEditor][frame_import] rollback animation='%s' reason='%s' restoring_backup='%s'",
                    animation_id.c_str(),
                    rollback_reason.c_str(),
                    rollback_backup.generic_string().c_str());
        std::error_code restore_ec;
        if (std::filesystem::exists(output_dir, restore_ec) && !restore_ec) {
            std::filesystem::remove_all(output_dir, restore_ec);
        }
        restore_ec.clear();
        if (std::filesystem::exists(rollback_backup, restore_ec) && !restore_ec) {
            std::filesystem::rename(rollback_backup, output_dir, restore_ec);
            if (restore_ec) {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                             "[AnimationEditor][frame_import] failed to restore backup '%s' -> '%s': %s",
                             rollback_backup.generic_string().c_str(),
                             output_dir.generic_string().c_str(),
                             restore_ec.message().c_str());
            }
        }
    };

    auto restore_payload = [&]() {
        if (animation_existed && previous_payload.has_value()) {
            (void)document_->update_animation_payload(animation_id, *previous_payload);
        } else {
            document_->delete_animation(animation_id);
        }
    };

    devmode::animation_import::ImportResult import_result;
    try {
        import_result = devmode::animation_import::import_frames_to_animation_folder(asset_root_path_,
                                                                                    animation_id,
                                                                                    frames);
    } catch (const std::exception& ex) {
        restore_folder_backup("copy_exception");
        return fail(SourceFrameImportOutcome::FailureStage::Copy, std::string("Frame import threw exception: ") + ex.what());
    } catch (...) {
        restore_folder_backup("copy_exception_unknown");
        return fail(SourceFrameImportOutcome::FailureStage::Copy, "Frame import threw an unknown exception.");
    }

    if (!import_result.success) {
        restore_folder_backup("copy_failed");
        const std::string detail = import_result.message.empty()
            ? std::string{"No frames were imported."}
            : import_result.message;
        return fail(SourceFrameImportOutcome::FailureStage::Copy, detail);
    }

    for (const auto& warning : import_result.warnings) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[AnimationEditor][frame_import] warning animation='%s': %s",
                    animation_id.c_str(),
                    warning.c_str());
    }

    if (!ensure_animation_exists(animation_id)) {
        restore_folder_backup("payload_create_failed");
        return fail(SourceFrameImportOutcome::FailureStage::Payload, "Failed to create animation payload entry.");
    }

    nlohmann::json payload = previous_payload.value_or(nlohmann::json::object());
    if (!payload.is_object()) {
        payload = nlohmann::json::object();
    }
    const nlohmann::json folder_payload =
        devmode::animation_import::build_folder_animation_payload(animation_id, import_result.frames_written);
    payload["source"] = folder_payload["source"];
    payload["number_of_frames"] = import_result.frames_written;
    if (!payload.contains("on_end")) {
        payload["on_end"] = "default";
    }

    if (!document_->update_animation_payload(animation_id, payload) &&
        !document_->animation_payload(animation_id).has_value()) {
        restore_folder_backup("payload_update_failed");
        restore_payload();
        return fail(SourceFrameImportOutcome::FailureStage::Payload, "Failed to update animation payload.");
    }

    const std::string document_id = manifest_asset_key_.empty()
        ? std::string{"animation-editor"}
        : manifest_asset_key_;
    const bool saved = orchestrated_save(devmode::core::SaveOrchestrator::Reason::StateChange,
                                         document_id,
                                         [this]() { return document_->save_to_file_checked(true); });
    if (!saved) {
        restore_payload();
        restore_folder_backup("manifest_save_failed");
        return fail(SourceFrameImportOutcome::FailureStage::Save, "Manifest save failed after installing frames; restored previous animation state.");
    }

    if (!rollback_backup.empty()) {
        std::error_code remove_ec;
        std::filesystem::remove_all(rollback_backup, remove_ec);
        if (remove_ec) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "[AnimationEditor][frame_import] failed to remove rollback backup '%s': %s",
                        rollback_backup.generic_string().c_str(),
                        remove_ec.message().c_str());
        }
    }

    auto_save_pending_ = false;
    auto_save_timer_frames_ = 0;
    if (preview_provider_) {
        preview_provider_->invalidate(animation_id);
        preview_provider_->invalidate_all();
    }
    std::string runtime_asset_key = manifest_asset_key_;
    if (runtime_asset_key.empty()) {
        if (auto info_ptr = info_.lock()) {
            runtime_asset_key = info_ptr->name;
        }
    }
    devmode::animation_import::ImportResult runtime_result =
        devmode::animation_import::reload_and_verify_runtime(assets_,
                                                             runtime_asset_key,
                                                             animation_id,
                                                             import_result.frames_written);
    if (!runtime_result.success) {
        restore_payload();
        restore_folder_backup("runtime_verify_failed");
        (void)orchestrated_save(devmode::core::SaveOrchestrator::Reason::StateChange,
                                document_id,
                                [this]() { return document_->save_to_file_checked(true); });
        std::string ignored_cache_error;
        (void)devmode::animation_import::delete_asset_cache(runtime_asset_key, ignored_cache_error);
        return fail(SourceFrameImportOutcome::FailureStage::RuntimeVerify,
                    runtime_result.message.empty()
                        ? std::string{"Runtime verification failed after importing frames."}
                        : runtime_result.message);
    }
    if (auto info_ptr = info_.lock()) {
        devmode::refresh_loaded_animation_instances(assets_, info_ptr);
    }

    outcome.success = true;
    outcome.failure_stage = SourceFrameImportOutcome::FailureStage::None;
    outcome.frames_written = import_result.frames_written;
    outcome.message = "Imported " + std::to_string(import_result.frames_written) +
                      " frame(s) to " + output_dir.generic_string();
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[AnimationEditor][frame_import] success animation='%s' source_count=%zu written=%d target='%s'",
                animation_id.c_str(),
                frames.size(),
                import_result.frames_written,
                output_dir.generic_string().c_str());
    return outcome;
}

bool AnimationEditorWindow::remove_animation_source_folder(const std::string& animation_id,
                                                           std::string& error_message) {
    error_message.clear();
    if (asset_root_path_.empty() || animation_id.empty()) {
        return true;
    }

    const std::filesystem::path source_root = asset_root_path_.lexically_normal();
    const std::filesystem::path target_folder = (source_root / animation_id).lexically_normal();
    if (!path_has_prefix(target_folder, source_root)) {
        error_message = "Refusing to delete animation source folder outside asset root: '" + target_folder.generic_string() + "'";
        return false;
    }

    std::error_code ec;
    if (!std::filesystem::exists(target_folder, ec)) {
        return true;
    }
    ec.clear();
    std::filesystem::remove_all(target_folder, ec);
    if (ec) {
        error_message = "Failed to delete animation source folder '" + target_folder.generic_string() + "': " + ec.message();
        return false;
    }
    return true;
}

bool AnimationEditorWindow::invalidate_asset_cache_now(std::string& error_message) {
    error_message.clear();
    std::string asset_cache_key;
    std::shared_ptr<AssetInfo> info_ptr = info_.lock();
    if (info_ptr) {
        asset_cache_key = info_ptr->name;
    }
    if (asset_cache_key.empty()) {
        asset_cache_key = asset_root_path_.filename().string();
    }
    if (asset_cache_key.empty()) {
        return true;
    }

    const std::filesystem::path cache_root = std::filesystem::path("cache").lexically_normal();
    const std::filesystem::path target_folder = (cache_root / asset_cache_key).lexically_normal();
    if (!path_has_prefix(target_folder, cache_root)) {
        error_message = "Refusing to delete asset cache folder outside cache root: '" + target_folder.generic_string() + "'";
        return false;
    }

    std::error_code ec;
    if (std::filesystem::exists(target_folder, ec)) {
        ec.clear();
        std::filesystem::remove_all(target_folder, ec);
        if (ec) {
            error_message = "Failed to delete asset cache folder '" + target_folder.generic_string() + "': " + ec.message();
            return false;
        }
    }

    if (info_ptr) {
        (void)info_ptr->consume_pending_texture_rebuild_on_close();
    }

    return true;
}

nlohmann::json AnimationEditorWindow::build_file_sourced_movement_payload(const std::string& animation_id,
                                                                          int frame_count,
                                                                          int dx,
                                                                          int dy,
                                                                          int dz,
                                                                          const std::vector<std::string>& tags) const {
    const int safe_frames = std::max(frame_count, 1);
    nlohmann::json payload = nlohmann::json::object();
    payload["source"] = nlohmann::json::object({
        {"kind", "folder"},
        {"path", animation_id},
        {"name", ""}
    });
    payload["number_of_frames"] = safe_frames;
    payload["movement"] = build_movement_sequence(safe_frames, dx, dy, dz);
    payload["movement_total"] = build_movement_total(payload["movement"]);
    payload["anchor_points"] = build_empty_geometry_frames(safe_frames);
    payload["locked"] = false;
    payload["reverse_source"] = false;
    payload["invert_x"] = false;
    payload["invert_y"] = false;
    payload["invert_z"] = false;
    payload["rnd_start"] = false;
    payload["on_end"] = "default";
    payload["tags"] = tags;
    return payload;
}

nlohmann::json AnimationEditorWindow::build_derived_movement_payload(const std::string& animation_id,
                                                                     const std::string& source_animation_id,
                                                                     int frame_count,
                                                                     bool invert_x,
                                                                     bool invert_y,
                                                                     bool invert_z,
                                                                     bool invert_frames_horizontal,
                                                                     bool invert_frames_vertical,
                                                                     const std::vector<std::string>& tags) const {
    (void)animation_id;
    const int safe_frames = std::max(frame_count, 1);
    nlohmann::json payload = nlohmann::json::object();
    payload["source"] = nlohmann::json::object({
        {"kind", "animation"},
        {"path", ""},
        {"name", source_animation_id}
    });
    payload["number_of_frames"] = safe_frames;
    payload["inherit_data"] = true;
    payload["reverse_source"] = false;
    payload["invert_x"] = invert_x;
    payload["invert_y"] = invert_y;
    payload["invert_z"] = invert_z;
    payload["invert_frames_horizontal"] = invert_frames_horizontal;
    payload["invert_frames_vertical"] = invert_frames_vertical;
    payload["on_end"] = "default";
    payload["tags"] = tags;
    payload["derived_modifiers"] = nlohmann::json::object({
        {"reverse", false}
    });
    return payload;
}

bool AnimationEditorWindow::ensure_animation_exists(const std::string& animation_id) {
    if (!document_ || animation_id.empty()) {
        return false;
    }
    auto ids = document_->animation_ids();
    if (std::find(ids.begin(), ids.end(), animation_id) != ids.end()) {
        return true;
    }

    const auto result = document_->create_animation(animation_id);
    if (result != AnimationDocument::CreateAnimationResult::Created &&
        result != AnimationDocument::CreateAnimationResult::AlreadyExists) {
        return false;
    }
    ids = document_->animation_ids();
    return std::find(ids.begin(), ids.end(), animation_id) != ids.end();
}

bool AnimationEditorWindow::create_or_replace_animation_payload(const std::string& animation_id,
                                                                const nlohmann::json& payload) {
    if (!document_ || animation_id.empty() || !payload.is_object()) {
        return false;
    }
    if (!ensure_animation_exists(animation_id)) {
        return false;
    }

    bool changed = document_->update_animation_payload(animation_id, payload);
    if (changed) {
        return true;
    }
    return document_->animation_payload(animation_id).has_value();
}

void AnimationEditorWindow::handle_create_defaults() {
    enum class DefaultsStageError { None, SourceDependency, CopyFailure, PayloadWriteFailure, RuntimeVerificationFailure };
    struct PlannedDefaultWrite {
        DefaultAnimationSpec spec;
        std::filesystem::path folder_path;
        nlohmann::json payload;
    };
    struct FolderBackup {
        std::string animation_id;
        std::filesystem::path original_path;
        std::filesystem::path backup_path;
    };

    if (!document_) {
        set_status_message("Animation document is unavailable.", 180);
        return;
    }
    if (asset_root_path_.empty()) {
        set_status_message("Asset directory is unavailable.", 180);
        return;
    }

    const bool create_basic = defaults_basic_movement_checkbox_ && defaults_basic_movement_checkbox_->value();
    const bool create_diagonals = defaults_diagonals_checkbox_ && defaults_diagonals_checkbox_->value();
    const bool create_elevation = defaults_elevation_checkbox_ && defaults_elevation_checkbox_->value();
    const bool create_3d_diagonals = defaults_3d_diagonals_checkbox_ && defaults_3d_diagonals_checkbox_->value();
    if (!create_basic && !create_diagonals && !create_elevation && !create_3d_diagonals) {
        set_status_message("Select at least one defaults group.", 180);
        return;
    }

    if (defaults_base_frame_paths_.empty()) {
        handle_pick_defaults_base_frames();
        if (defaults_base_frame_paths_.empty()) {
            devmode::dialogs::show_message(parent_window_, "Create Defaults", "Select at least one base image frame before creating default animations.", devmode::dialogs::MessageIcon::Warning);
            return;
        }
    }

    std::vector<std::filesystem::path> base_frames;
    for (const auto& path : defaults_base_frame_paths_) {
        if (devmode::frame_importer::is_supported_image_file(path) &&
            !devmode::frame_importer::is_gif_file(path)) {
            base_frames.push_back(path);
        }
    }
    if (base_frames.empty()) {
        set_status_message("Select at least one base image frame.", 180);
        devmode::dialogs::show_message(parent_window_, "Create Defaults", "Select at least one base image frame before creating default animations.", devmode::dialogs::MessageIcon::Warning);
        return;
    }
    base_frames = normalize_sequence_paths(base_frames);

    std::optional<int> total_movement_per_animation = parse_defaults_total_movement();
    if (!total_movement_per_animation.has_value()) {
        set_status_message("Total movement per animation must be a positive integer.", 180);
        return;
    }

    const int d = *total_movement_per_animation;
    const int frame_count = static_cast<int>(base_frames.size());
    const bool base_faces_right = defaults_base_faces_right();
    const std::vector<std::string> ids_before_defaults = document_->animation_ids();
    const std::unordered_set<std::string> ids_before_defaults_set(
        ids_before_defaults.begin(), ids_before_defaults.end());
    std::unordered_map<std::string, nlohmann::json> payload_snapshot_before_defaults;
    std::vector<std::string> created_ids;
    std::vector<std::filesystem::path> created_folders;
    std::vector<FolderBackup> source_folder_backups;
    std::unordered_set<std::string> backed_up_source_folder_ids;
    std::vector<DefaultAnimationSpec> specs;

    auto add_seed=[&](const std::string& id,int dx,int dy,int dz){specs.push_back(DefaultAnimationSpec{id,dx,dy,dz,{},true,false,false});};
    auto add_derived=[&](const std::string& id,const std::string& source_id,int dx,int dy,int dz,bool h,bool v){specs.push_back(DefaultAnimationSpec{id,dx,dy,dz,source_id,false,h,v});};
    const int x_seed_dx = base_faces_right ? d : -d;
    const std::string x_seed_id = base_faces_right ? "right" : "left";
    const std::string x_opposite_id = base_faces_right ? "left" : "right";
    const int x_opposite_dx = -x_seed_dx;
    if (create_basic) { add_seed(x_seed_id,x_seed_dx,0,0); add_derived(x_opposite_id,x_seed_id,x_opposite_dx,0,0,true,false); add_seed("up",0,0,-d); add_derived("forward","up",0,0,-d,false,false); add_derived("down","up",0,0,d,false,true); add_derived("backward","up",0,0,d,false,true);}    
    if (create_diagonals) { const std::string seed_id=base_faces_right?"forward_right":"forward_left"; const std::string opposite_forward_id=base_faces_right?"forward_left":"forward_right"; const std::string same_backward_id=base_faces_right?"backward_right":"backward_left"; const std::string opposite_backward_id=base_faces_right?"backward_left":"backward_right"; add_seed(seed_id,x_seed_dx,0,-d); add_derived(opposite_forward_id,seed_id,x_opposite_dx,0,-d,true,false); add_derived(same_backward_id,seed_id,x_seed_dx,0,d,false,true); add_derived(opposite_backward_id,seed_id,x_opposite_dx,0,d,true,true);}    
    if (create_elevation) { add_seed("elevation_up",0,d,0); add_derived("elevation_down","elevation_up",0,-d,0,false,true);}    
    if (create_3d_diagonals) { const std::string seed_id=base_faces_right?"up_forward_right":"up_forward_left"; add_seed(seed_id,x_seed_dx,d,-d); const std::array<int,2> y{1,-1},z{-1,1},x{-1,1}; for(int ys:y) for(int zs:z) for(int xs:x){ const int dx=xs*d,dy=ys*d,dz=zs*d; const std::string id=std::string(ys>0?"up":"down")+"_"+(zs<0?"forward":"backward")+"_"+(xs<0?"left":"right"); if(id==seed_id) continue; add_derived(id,seed_id,dx,dy,dz,dx!=x_seed_dx,dy!=d||dz!=-d);} }

    auto find_source=[&](const std::string& id)->const DefaultAnimationSpec*{for(const auto& spec:specs) if(spec.id==id) return &spec; return nullptr;};
    std::vector<PlannedDefaultWrite> plan; plan.reserve(specs.size());
    DefaultsStageError error=DefaultsStageError::None;
    if (defaults_force_source_dependency_failure_) error = DefaultsStageError::SourceDependency;
    for (const auto& spec : specs) {
        PlannedDefaultWrite write{spec, asset_root_path_ / spec.id, nlohmann::json::object()};
        if (ids_before_defaults_set.count(spec.id) > 0) {
            if (payload_snapshot_before_defaults.count(spec.id) == 0) {
                auto payload = document_->animation_payload_json(spec.id);
                if (payload.has_value() && payload->is_object()) {
                    payload_snapshot_before_defaults.emplace(spec.id, std::move(*payload));
                }
            }
        }
        if (spec.folder_sourced) write.payload = build_file_sourced_movement_payload(spec.id, frame_count, spec.dx, spec.dy, spec.dz, default_direction_tags(spec.id, spec.dx, spec.dy, spec.dz));
        else {
            const DefaultAnimationSpec* source = find_source(spec.source_id);
            if (!source) { error = DefaultsStageError::SourceDependency; break; }
            write.payload = build_derived_movement_payload(spec.id,spec.source_id,frame_count,source->dx!=0&&source->dx!=spec.dx,source->dy!=0&&source->dy!=spec.dy,source->dz!=0&&source->dz!=spec.dz,spec.invert_frames_horizontal,spec.invert_frames_vertical,default_direction_tags(spec.id, spec.dx, spec.dy, spec.dz));
        }
        plan.push_back(std::move(write));
    }

    auto backup_existing_source_folder = [&](const PlannedDefaultWrite& write) -> bool {
        if (!write.spec.folder_sourced || backed_up_source_folder_ids.count(write.spec.id) > 0) {
            return true;
        }
        std::error_code ec;
        const bool existed = std::filesystem::exists(write.folder_path, ec);
        if (ec) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "[AnimationEditor] Failed to inspect source folder '%s': %s",
                         write.folder_path.generic_string().c_str(),
                         ec.message().c_str());
            return false;
        }
        if (!existed) {
            return true;
        }

        std::filesystem::path backup_path;
        bool found_backup_slot = false;
        for (int attempt = 0; attempt < 64; ++attempt) {
            const std::string suffix =
                ".defaults_backup_" + write.spec.id + "_" +
                std::to_string(static_cast<long long>(
                    std::chrono::high_resolution_clock::now().time_since_epoch().count())) +
                "_" + std::to_string(attempt);
            backup_path = asset_root_path_ / suffix;
            std::error_code exists_ec;
            if (!std::filesystem::exists(backup_path, exists_ec) && !exists_ec) {
                found_backup_slot = true;
                break;
            }
        }

        if (!found_backup_slot) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "[AnimationEditor] Failed to allocate defaults backup folder for '%s'.",
                         write.spec.id.c_str());
            return false;
        }

        std::filesystem::rename(write.folder_path, backup_path, ec);
        if (ec) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "[AnimationEditor] Failed to backup source folder '%s': %s",
                         write.folder_path.generic_string().c_str(),
                         ec.message().c_str());
            return false;
        }

        source_folder_backups.push_back(FolderBackup{write.spec.id, write.folder_path, backup_path});
        backed_up_source_folder_ids.insert(write.spec.id);
        return true;
    };

    for (const auto& write : plan) {
        if (error != DefaultsStageError::None) break;
        if (write.spec.folder_sourced) {
            if (!backup_existing_source_folder(write)) {
                error = DefaultsStageError::CopyFailure;
                break;
            }
            const bool copied = defaults_copy_frames_override_ ? defaults_copy_frames_override_(write.spec.id, base_frames) : copy_frames_to_animation_folder(write.spec.id, base_frames);
            if (!copied) { error = DefaultsStageError::CopyFailure; break; }
            if (backed_up_source_folder_ids.count(write.spec.id) == 0) {
                created_folders.push_back(write.folder_path);
            }
        }
        if (defaults_force_payload_write_failure_ || !create_or_replace_animation_payload(write.spec.id, write.payload)) { error = DefaultsStageError::PayloadWriteFailure; break; }
        created_ids.push_back(write.spec.id);
    }

    if (error == DefaultsStageError::None && manifest_store_ && !manifest_asset_key_.empty()) {
        const bool persisted = defaults_persist_manifest_override_ ? defaults_persist_manifest_override_(nlohmann::json{{"movement_enabled", true}}, false) : persist_manifest_payload(nlohmann::json{{"movement_enabled", true}}, false);
        if (!persisted) error = DefaultsStageError::PayloadWriteFailure;
    }

    if (error == DefaultsStageError::None && assets_) {
        std::string runtime_asset_key = manifest_asset_key_;
        if (runtime_asset_key.empty()) {
            if (auto info_ptr = info_.lock()) {
                runtime_asset_key = info_ptr->name;
            }
        }
        bool loaded_runtime = false;
        for (const auto& write : plan) {
            if (!write.spec.folder_sourced) {
                continue;
            }
            if (!loaded_runtime) {
                auto runtime = devmode::animation_import::reload_and_verify_runtime(assets_,
                                                                                    runtime_asset_key,
                                                                                    write.spec.id,
                                                                                    frame_count);
                if (!runtime.success) {
                    error = DefaultsStageError::RuntimeVerificationFailure;
                    break;
                }
                loaded_runtime = true;
            } else {
                std::string verify_error;
                if (!devmode::animation_import::verify_runtime_textures(assets_->library().get(runtime_asset_key),
                                                                        write.spec.id,
                                                                        frame_count,
                                                                        verify_error)) {
                    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                                 "[AnimationEditor] Defaults runtime verification failed for '%s': %s",
                                 write.spec.id.c_str(),
                                 verify_error.c_str());
                    error = DefaultsStageError::RuntimeVerificationFailure;
                    break;
                }
            }
        }
    }

    if (error != DefaultsStageError::None) {
        for (const auto& id : created_ids) {
            if (ids_before_defaults_set.count(id) == 0) {
                document_->delete_animation(id);
            }
        }
        for (const auto& [id, payload] : payload_snapshot_before_defaults) {
            (void)create_or_replace_animation_payload(id, payload);
        }
        for (const auto& folder : created_folders) { std::error_code ec; std::filesystem::remove_all(folder, ec); }
        for (auto it = source_folder_backups.rbegin(); it != source_folder_backups.rend(); ++it) {
            std::error_code ec;
            if (std::filesystem::exists(it->original_path, ec) && !ec) {
                ec.clear();
                std::filesystem::remove_all(it->original_path, ec);
            }
            ec.clear();
            std::filesystem::rename(it->backup_path, it->original_path, ec);
            if (ec) {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                            "[AnimationEditor] Failed to restore source folder backup '%s' -> '%s': %s",
                            it->backup_path.generic_string().c_str(),
                            it->original_path.generic_string().c_str(),
                            ec.message().c_str());
            }
        }
        if (document_) {
            (void)orchestrated_save(devmode::core::SaveOrchestrator::Reason::StateChange,
                                    manifest_asset_key_.empty() ? std::string("animation-editor") : manifest_asset_key_,
                                    [this]() { return document_->save_to_file_checked(true); });
        }
        std::string runtime_asset_key = manifest_asset_key_;
        if (runtime_asset_key.empty()) {
            if (auto info_ptr = info_.lock()) {
                runtime_asset_key = info_ptr->name;
            }
        }
        std::string ignored_cache_error;
        (void)devmode::animation_import::delete_asset_cache(runtime_asset_key, ignored_cache_error);
        if (error == DefaultsStageError::CopyFailure) set_status_message("Create defaults failed while importing source frames. Check source images and write permissions.", 300);
        else if (error == DefaultsStageError::PayloadWriteFailure) set_status_message("Create defaults failed while writing animation payloads. No partial defaults were kept.", 300);
        else if (error == DefaultsStageError::RuntimeVerificationFailure) set_status_message("Create defaults failed while loading imported textures. No partial defaults were kept.", 300);
        else set_status_message("Create defaults failed: a source dependency was missing.", 300);
        return;
    }

    for (const auto& backup : source_folder_backups) {
        std::error_code ec;
        std::filesystem::remove_all(backup.backup_path, ec);
        if (ec) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "[AnimationEditor] Failed to remove source folder backup '%s': %s",
                        backup.backup_path.generic_string().c_str(),
                        ec.message().c_str());
        }
    }

    if (preview_provider_) preview_provider_->invalidate_all();
    if (!created_ids.empty()) select_animation(std::optional<std::string>{created_ids.front()}, false); else ensure_selection_valid();
    set_status_message("Created default movement animations.", 300);
    close_defaults_modal();
}

void AnimationEditorWindow::create_animation_via_prompt() {
    if (!document_) {
        set_status_message("Animation document is unavailable.", 180);
        return;
    }

    auto prompt_name = [&](const std::string& default_value) -> std::optional<std::string> {
        if (text_prompt_override_) {
            return text_prompt_override_("Create Animation", "Enter new animation identifier", default_value);
        }
        return devmode::dialogs::prompt_text(parent_window_, "Create Animation", "Enter new animation identifier", default_value);
    };

    std::optional<std::string> input = prompt_name("animation");
    if (!input) {
        return;
    }

    for (;;) {
        std::string name = normalize_animation_name(*input);
        if (name.empty()) {
            set_status_message("Animation name is invalid after normalization.", 240);
            return;
        }
        if (animation_editor::strings::is_reserved_animation_name(name)) {
            set_status_message("Animation name '" + name + "' is reserved.", 240);
            return;
        }

        const auto before_ids = document_->animation_ids();
        const bool conflict =
            std::find(before_ids.begin(), before_ids.end(), name) != before_ids.end();
        if (conflict) {
            std::optional<int> choice;
            if (choice_prompt_override_) {
                choice = choice_prompt_override_("Animation Exists",
                                                 "Animation '" + name + "' already exists.",
                                                 {0, 1, 2});
            } else {
                choice = devmode::dialogs::show_choice(
                    parent_window_,
                    "Animation Exists",
                    "Animation '" + name + "' already exists.",
                    {
                        devmode::dialogs::DialogButton{0, "Rename", true, false, false},
                        devmode::dialogs::DialogButton{1, "Cancel", false, true, false},
                        devmode::dialogs::DialogButton{2, "Open Existing", false, false, false},
                    },
                    devmode::dialogs::MessageIcon::Warning);
            }
            if (!choice || *choice == 1) {
                set_status_message("Create animation cancelled.", 150);
                return;
            }
            if (*choice == 2) {
                select_animation(std::make_optional(name), false);
                refresh_panels_after_load();
                set_status_message("Opened existing animation '" + name + "'.", 240);
                return;
            }
            input = prompt_name(name + "_2");
            if (!input) {
                set_status_message("Create animation cancelled.", 150);
                return;
            }
            continue;
        }

        const auto create_result = document_->create_animation(name);
        const auto after_ids = document_->animation_ids();
        const bool inserted =
            std::find(after_ids.begin(), after_ids.end(), name) != after_ids.end();
        if (create_result == AnimationDocument::CreateAnimationResult::Created && inserted) {
            preview_provider_->invalidate_all();
            select_animation(std::make_optional(name), false);
            refresh_panels_after_load();
            set_status_message("Created animation '" + name + "'.", 240);
        } else if (create_result == AnimationDocument::CreateAnimationResult::AlreadyExists) {
            set_status_message("Animation '" + name + "' already exists.", 240);
        } else if (create_result == AnimationDocument::CreateAnimationResult::InvalidName) {
            set_status_message("Animation name '" + name + "' is invalid.", 240);
        } else if (create_result == AnimationDocument::CreateAnimationResult::Created && !inserted) {
            set_status_message("Animation create failed verification for '" + name + "'.", 260);
        } else {
            set_status_message("Failed to create animation '" + name + "'.", 240);
        }
        return;
    }
}

void AnimationEditorWindow::reload_document() {
    auto info_ptr = info_.lock();
    bool snapshot_was_empty = true;
    if (!info_ptr || !manifest_store_) {
        close_manifest_transaction();
        document_->load_from_manifest(nlohmann::json::object(), asset_root_path_, {});
        using_manifest_store_ = false;
    } else {
        close_manifest_transaction();
        auto attach_manifest_transaction = [&](const std::string& key, bool log_creation) -> bool {
            if (!manifest_store_ || key.empty()) {
                return false;
            }
            manifest_asset_key_ = key;
            manifest_transaction_ = manifest_store_->begin_asset_transaction(manifest_asset_key_, true);
            if (!manifest_transaction_) {
                std::cerr << "[AnimationEditor] Failed to reopen manifest transaction for '"
                          << manifest_asset_key_ << "'\n";
                manifest_asset_key_.clear();
                manifest_transaction_ = {};
                if (document_) {
                    document_->set_manifest_asset_key_debug({});
                }
                return false;
            }
            if (document_) {
                document_->set_manifest_asset_key_debug(manifest_asset_key_);
            }
            if (log_creation) {
                std::cerr << "[AnimationEditor] Created manifest entry for '" << info_ptr->name
                          << "' as '" << manifest_asset_key_ << "' during reload\n";
            }
            using_manifest_store_ = true;
            return true;
        };

        bool attached = false;
        if (auto key = resolve_manifest_key(*info_ptr)) {
            attached = attach_manifest_transaction(*key, false);
        } else {
            std::cerr << "[AnimationEditor] Unable to resolve manifest key during reload\n";
            const std::string fallback_key = manifest_key_fallback(*info_ptr);
            if (!fallback_key.empty()) {
                attached = attach_manifest_transaction(fallback_key, true);
            }
        }

        if (attached) {
            nlohmann::json snapshot = manifest_transaction_.data();
            snapshot_was_empty = !has_animation_entries(snapshot);
            document_->load_from_manifest(snapshot,
                                          asset_root_path_,
                                          [this](const nlohmann::json& payload) {
                                              return this->persist_manifest_payload(payload);
                                          });
        } else {
            document_->load_from_manifest(nlohmann::json::object(), asset_root_path_, {});
            using_manifest_store_ = false;
        }
    }

    const bool seeded_default = snapshot_was_empty && document_ && document_->animation_ids().size() == 1 && document_->animation_ids().front() == "default";

    if (seeded_default) {
        orchestrated_save(devmode::core::SaveOrchestrator::Reason::HotReload,
                          manifest_asset_key_.empty() ? std::string("animation-editor") : manifest_asset_key_,
                          [this]() { document_->save_to_file(); return true; });
    } else if (document_) {
        document_->consume_dirty_flag();
    }
    preview_provider_->invalidate_all();
    if (list_panel_) list_panel_->set_document(document_);
    if (inspector_panel_) inspector_panel_->set_document(document_);
    configure_list_panel();
    configure_inspector_panel();
    ensure_selection_valid();
    refresh_panels_after_load();
    if (document_) {
        const auto& report = document_->last_load_report();
        load_diagnostics_.animation_count_loaded = static_cast<int>(document_->animation_ids().size());
        load_diagnostics_.parse_failures = report.parse_failures;
        load_diagnostics_.normalization_failures = report.normalization_failures;
    }
    if (seeded_default) {
        set_status_message("Created default animation.", 240);
    } else {
        set_status_message("Reloaded animations.", 240);
    }
    auto_save_pending_ = false;
    auto_save_timer_frames_ = 0;
}

void AnimationEditorWindow::process_auto_save() {
    if (!auto_save_pending_ || !document_) {
        return;
    }

    if (auto_save_timer_frames_ > 0) {
        --auto_save_timer_frames_;
        return;
    }

    (void)orchestrated_save(devmode::core::SaveOrchestrator::Reason::AutoSave,
                            manifest_asset_key_.empty() ? std::string("animation-editor") : manifest_asset_key_,
                            [this]() { return document_->save_to_file_checked(true); });
    if (using_manifest_store_) {
        set_status_message("Animations auto-saved.", 180);
    }
    auto_save_pending_ = false;
    auto_save_timer_frames_ = 0;
}

void AnimationEditorWindow::set_manifest_store(devmode::core::ManifestStore* store) {
    if (manifest_store_ == store) {
        return;
    }
    close_manifest_transaction();
    manifest_store_ = store;
    if (custom_controller_service_) {
        custom_controller_service_->set_manifest_store(store);
    }
    if (inspector_panel_) {
        inspector_panel_->set_manifest_store(store);
    }
    if (auto info_ptr = info_.lock()) {
        set_info(info_ptr);
    }
}

void AnimationEditorWindow::close_manifest_transaction() {
    if (manifest_transaction_) {
        manifest_transaction_.cancel();
        manifest_transaction_ = {};
    }
    manifest_asset_key_.clear();
    using_manifest_store_ = false;
    if (document_) {
        document_->set_manifest_asset_key_debug({});
    }
}

bool AnimationEditorWindow::persist_manifest_payload(const nlohmann::json& payload, bool finalize) {
    if (!manifest_store_ || manifest_asset_key_.empty()) {
        return false;
    }
    if (!manifest_transaction_) {
        manifest_transaction_ = manifest_store_->begin_asset_transaction(manifest_asset_key_, true);
        if (!manifest_transaction_) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "[AnimationEditor] Failed to open manifest transaction for key '%s'.",
                         manifest_asset_key_.c_str());
            set_status_message("Failed to save animations for '" + manifest_asset_key_ + "'.", 240);
            return false;
        }
        using_manifest_store_ = true;
    }

    auto refresh_draft_from_store = [&]() {
        auto view = manifest_store_->get_asset(manifest_asset_key_);
        if (view && view.data && view.data->is_object()) {
            manifest_transaction_.data() = *view.data;
        }
    };

    // Always rebase on the latest manifest entry so external live edits (e.g., frame editor)
    // cannot be overwritten by a stale transaction snapshot when finalizing.
    refresh_draft_from_store();

    nlohmann::json& draft = manifest_transaction_.data();
    if (!draft.is_object()) {
        draft = nlohmann::json::object();
    }
    auto ensure_enable_flag = [&](const char* key) {
        if (!draft.contains(key) || !draft[key].is_boolean()) {
            draft[key] = false;
        }
    };
    ensure_enable_flag("movement_enabled");
    ensure_enable_flag("attack_box_enabled");
    ensure_enable_flag("hitbox_enabled");
    ensure_enable_flag("floor_boxes_enabled");
    if (payload.is_object()) {
        if (!draft.is_object()) {
            draft = nlohmann::json::object();
        }

        for (auto it = payload.begin(); it != payload.end(); ++it) {
            draft[it.key()] = it.value();
        }
    } else if (!payload.is_null()) {

        draft = payload;
        if (!draft.is_object()) {
            draft = nlohmann::json::object();
        }
    }

    ensure_enable_flag("movement_enabled");
    ensure_enable_flag("attack_box_enabled");
    ensure_enable_flag("hitbox_enabled");
    ensure_enable_flag("floor_boxes_enabled");

    const nlohmann::json merged_payload = draft;

    auto commit_fn = [this, finalize, merged_payload]() -> bool {
        auto commit_current_transaction = [this, finalize]() -> bool {
            if (!manifest_transaction_) {
                return false;
            }
            const bool ok = finalize ? manifest_transaction_.finalize() : manifest_transaction_.save();
            if (ok && finalize) {
                manifest_transaction_ = {};
                manifest_asset_key_.clear();
                using_manifest_store_ = false;
                if (document_) {
                    document_->set_manifest_asset_key_debug({});
                }
            }
            return ok;
        };

        if (commit_current_transaction()) {
            return true;
        }

        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[AnimationEditor] Manifest commit failed for key '%s'; recreating transaction and retrying.",
                    manifest_asset_key_.c_str());

        manifest_transaction_ = {};
        manifest_transaction_ = manifest_store_->begin_asset_transaction(manifest_asset_key_, true);
        if (!manifest_transaction_) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "[AnimationEditor] Failed to recreate manifest transaction for key '%s' after commit failure.",
                         manifest_asset_key_.c_str());
            set_status_message("Failed to save animations for '" + manifest_asset_key_ + "'.", 240);
            return false;
        }
        using_manifest_store_ = true;
        manifest_transaction_.data() = merged_payload;

        if (commit_current_transaction()) {
            return true;
        }

        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[AnimationEditor] Manifest commit retry failed for key '%s'.",
                     manifest_asset_key_.c_str());
        set_status_message("Failed to save animations for '" + manifest_asset_key_ + "'.", 240);
        return false;
    };

    auto on_success = [this, finalize]() {
        if (finalize) {
            handle_document_saved();
        }
        set_status_message(finalize ? "Animations saved." : "Animations updated.", finalize ? 200 : 120);
    };

    if (save_coordinator_) {
        if (!finalize) {
            // Keep debounced preview-state writes on the caller thread to avoid racing
            // the mutable manifest transaction against async coordinator execution.
            const bool committed = commit_fn();
            if (committed) {
                on_success();
            }
            return committed;
        }

        save_coordinator_->enqueue_custom(devmode::core::DevSaveCoordinator::IntentKind::ManifestAsset,
                                          std::string("asset:") + manifest_asset_key_,
                                          [commit_fn](devmode::core::ManifestStore&) { return commit_fn(); },
                                          devmode::core::DevSaveCoordinator::Priority::Immediate,
                                          "Animation manifest",
                                          on_success);
        save_coordinator_->flush_now("Animation save");
        return true;
    }

    bool committed = commit_fn();
    if (committed) {
        on_success();
    }
    return committed;
}


bool AnimationEditorWindow::orchestrated_save(devmode::core::SaveOrchestrator::Reason reason,
                                              const std::string& document_id,
                                              const std::function<bool()>& write) {
    devmode::core::SaveOrchestrator::Request request;
    request.document_id = document_id.empty() ? std::string("animation-editor") : document_id;
    request.reason = reason;
    request.atomic_write = write;
    request.disk_available_check = []() { return true; };
    request.checksum = []() { return std::size_t{0}; };
    const auto result = save_orchestrator_.save(request);
    if (!result.success && result.conflict) {
        set_status_message("Conflict detected", 240);
    }
    return result.success;
}

std::optional<std::string> AnimationEditorWindow::resolve_manifest_key(const AssetInfo& info) const {
    if (!manifest_store_) {
        return std::nullopt;
    }

    std::vector<std::string> candidates;
    if (!info.name.empty()) {
        candidates.push_back(info.name);
    }
    try {
        std::filesystem::path dir = info.asset_dir_path();
        if (!dir.empty()) {
            candidates.push_back(dir.filename().string());
            candidates.push_back(dir.lexically_normal().generic_string());
        }
    } catch (...) {
    }

    auto to_lower = [](std::string value) {
        std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        return value;
};

    std::unordered_set<std::string> seen;
    for (const auto& candidate : candidates) {
        if (candidate.empty()) continue;
        if (!seen.insert(candidate).second) continue;
        if (auto resolved = manifest_store_->resolve_asset_name(candidate)) {
            return resolved;
        }
    }

    std::string desired_dir;
    try {
        std::filesystem::path dir = info.asset_dir_path();
        if (!dir.empty()) {
            desired_dir = dir.lexically_normal().generic_string();
        }
    } catch (...) {
        desired_dir.clear();
    }

    std::string desired_name_lower = to_lower(info.name);
    for (const auto& view : manifest_store_->assets()) {
        if (!view || !view.data || !view.data->is_object()) {
            continue;
        }
        const auto& asset_json = *view.data;
        auto dir_it = asset_json.find("asset_directory");
        if (dir_it != asset_json.end() && dir_it->is_string()) {
            try {
                std::filesystem::path dir = dir_it->get<std::string>();
                if (!desired_dir.empty() && dir.lexically_normal().generic_string() == desired_dir) {
                    return view.name;
                }
            } catch (...) {
            }
        }
        if (!desired_name_lower.empty()) {
            std::string manifest_name = asset_json.value("asset_name", view.name);
            if (!manifest_name.empty() && to_lower(manifest_name) == desired_name_lower) {
                return view.name;
            }
        }
    }

    return std::nullopt;
}

std::optional<std::filesystem::path> AnimationEditorWindow::pick_folder() const {
    return devmode::dialogs::open_folder(parent_window_,
                                         "Select Animation Folder",
                                         asset_root_path_.empty() ? std::filesystem::path{} : asset_root_path_);
}

void AnimationEditorWindow::handle_controller_button_click() {
    if (does_controller_exist()) {
        open_controller();
    } else {
        add_controller();
    }
}

void AnimationEditorWindow::update_controller_button_label() {
    // No-op: action label is now surfaced through controller_action_label()
    // and rendered by AssetInfoUI controls.
}

bool AnimationEditorWindow::does_controller_exist() const {
    auto info_ptr = info_.lock();
    if (!info_ptr || asset_root_path_.empty()) return false;
    std::string sanitized = sanitize_asset_name(info_ptr->name);
    if (sanitized.empty()) return false;
    std::string key = generate_controller_key(sanitized);
    std::filesystem::path controller_dir = "ENGINE/runtime/animation/controllers/custom_controllers";
    std::filesystem::path hpp_path = controller_dir / (key + ".hpp");
    std::filesystem::path cpp_path = controller_dir / (key + ".cpp");
    return std::filesystem::exists(hpp_path) && std::filesystem::exists(cpp_path);
}

std::string AnimationEditorWindow::sanitize_asset_name(const std::string& name) const {
    return devmode::utils::normalize_asset_name(name);
}

std::string AnimationEditorWindow::generate_controller_key(const std::string& asset_name) const {
    return asset_name + "_controller";
}

std::string AnimationEditorWindow::generate_class_name(const std::string& asset_name) const {
    if (asset_name.empty()) return "";
    return asset_name + "_controller";
}

std::vector<std::string> AnimationEditorWindow::collect_available_animation_ids() const {
    std::vector<std::string> ids;
    if (document_) {
        ids = document_->animation_ids();
    } else if (auto info_ptr = info_.lock()) {
        ids.reserve(info_ptr->animations.size());
        for (const auto& entry : info_ptr->animations) {
            ids.push_back(entry.first);
        }
    }
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    return ids;
}

std::string AnimationEditorWindow::build_controller_metadata(const std::string& controller_key) const {
    auto info_ptr = info_.lock();
    std::ostringstream oss;
    oss << "// CONTROLLER_META_BEGIN\n";
    oss << "// Controller: " << controller_key << "\n";
    if (info_ptr) {
        oss << "// Asset: " << info_ptr->name;
        if (!info_ptr->type.empty()) {
            oss << " (type: " << info_ptr->type << ")";
        }
        oss << "\n";
    }
    const auto ids = collect_available_animation_ids();
    oss << "// Available animations [" << ids.size() << "]:";
    if (ids.empty()) {
        oss << " <none>\n";
    } else {
        oss << "\n";
        for (const auto& id : ids) {
            oss << "//   - " << id << "\n";
        }
    }
    const auto now = std::chrono::system_clock::now();
    const std::time_t now_c = std::chrono::system_clock::to_time_t(now);
    oss << "// Generated: " << std::put_time(std::localtime(&now_c), "%Y-%m-%d %H:%M:%S") << "\n";
    oss << "// CONTROLLER_META_END\n\n";
    return oss.str();
}

bool AnimationEditorWindow::write_or_update_controller_metadata(const std::filesystem::path& path,
                                                                 const std::string& metadata) const {
    if (path.empty()) return false;
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        return false;
    }

    std::ifstream in(path);
    if (!in.is_open()) return false;
    std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();

    const std::string begin_tag = "// CONTROLLER_META_BEGIN";
    const std::string end_tag = "// CONTROLLER_META_END";
    std::size_t begin_pos = content.find(begin_tag);
    std::size_t end_pos = content.find(end_tag);

    std::string new_content;
    if (begin_pos != std::string::npos && end_pos != std::string::npos && end_pos > begin_pos) {
        end_pos = content.find('\n', end_pos);
        if (end_pos == std::string::npos) end_pos = content.size();
        new_content = content.substr(0, begin_pos) + metadata + content.substr(end_pos);
    } else {
        new_content = metadata + content;
    }

    if (new_content == content) return true;

    std::ofstream out(path, std::ios::trunc);
    if (!out.is_open()) return false;
    out << new_content;
    return true;
}

void AnimationEditorWindow::ensure_controller_factory_registration(const std::string& key,
                                                                   const std::string& class_name) const {
    const std::filesystem::path factory_path = "ENGINE/runtime/assets/asset/controller_factory.cpp";
    std::error_code ec;
    if (!std::filesystem::exists(factory_path, ec)) {
        return;
    }

    std::ifstream in(factory_path);
    if (!in.is_open()) return;
    std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();

    bool modified = false;
    const std::string include_line = "#include \"animation/controllers/custom_controllers/" + key + ".hpp\"";
    if (content.find(include_line) == std::string::npos) {
        const std::string include_marker = "// <<CUSTOM_CONTROLLER_INCLUDE_INSERT_POINT>>";
        auto include_pos = content.find(include_marker);
        if (include_pos != std::string::npos) {
            content.insert(include_pos, include_line + "\n");
        } else {
            include_pos = content.find("#include \"animation/controllers/custom_controllers/default_controller.hpp\"");
            if (include_pos != std::string::npos) {
                content.insert(include_pos, include_line + "\n");
            } else {
                content = include_line + "\n" + content;
            }
        }
        modified = true;
    }

    const std::string entry = "        { \"" + key + "\", [](Asset* asset) {\n"
                              "                return std::make_unique<" + class_name + ">(asset);\n"
                              "        } },\n";

    const std::string marker = "// <<CUSTOM_CONTROLLER_FACTORY_INSERT_POINT>>";
    auto marker_pos = content.find(marker);
    if (marker_pos != std::string::npos) {
        auto insert_pos = content.find('\n', marker_pos);
        if (insert_pos != std::string::npos) {
            insert_pos += 1;
            if (content.find(entry, marker_pos) == std::string::npos) {
                content.insert(insert_pos, entry);
                modified = true;
            }
        }
    }

    if (!modified) return;

    std::ofstream out(factory_path, std::ios::trunc);
    if (!out.is_open()) return;
    out << content;
}

void AnimationEditorWindow::add_controller() {
    auto info_ptr = info_.lock();
    if (!info_ptr || !custom_controller_service_) {
        set_status_message("No asset selected.", 180);
        return;
    }

    try {
        custom_controller_service_->set_asset_root(asset_root_path_);
        custom_controller_service_->set_manifest_store(manifest_store_);
        custom_controller_service_->set_manifest_asset_key(manifest_asset_key_);
        custom_controller_service_->create_new_controller(generate_controller_key(sanitize_asset_name(info_ptr->name)));
        set_status_message("Controller created.", 240);
        update_controller_button_label();
    } catch (const std::exception& ex) {
        set_status_message(std::string("Failed to create controller: ") + ex.what(), 240);
    }
}

void AnimationEditorWindow::open_controller() {
    auto info_ptr = info_.lock();
    if (!info_ptr || !custom_controller_service_) {
        set_status_message("No asset selected.", 180);
        return;
    }

    try {
        custom_controller_service_->set_asset_root(asset_root_path_);
        custom_controller_service_->open_existing_controller(generate_controller_key(sanitize_asset_name(info_ptr->name)));
        set_status_message("Opened controller file.", 120);
    } catch (const std::exception& ex) {
        set_status_message(std::string("Failed to open controller: ") + ex.what(), 240);
    }
}

std::optional<std::filesystem::path> AnimationEditorWindow::pick_gif() const {
    return devmode::dialogs::open_file(parent_window_,
                                       "Import GIF",
                                       asset_root_path_.empty() ? std::filesystem::path{} : asset_root_path_,
                                       {devmode::dialogs::FileDialogFilter{"GIF Image", "gif"}});
}

devmode::dialogs::FileDialogResult AnimationEditorWindow::pick_png_sequence() const {
    if (png_sequence_picker_override_) {
        return png_sequence_picker_override_();
    }
    return devmode::dialogs::open_files(parent_window_,
                                        "Upload Images",
                                        asset_root_path_.empty() ? std::filesystem::path{} : asset_root_path_,
                                        {devmode::dialogs::FileDialogFilter{"Image Files", "png;jpg;jpeg;bmp;webp;gif"}},
                                        true);
}

std::optional<std::string> AnimationEditorWindow::pick_animation_reference() const {
    if (!document_) return std::nullopt;
    auto ids = document_->animation_ids();
    std::vector<std::string> selectable;
    selectable.reserve(ids.size());
    for (const auto& id : ids) {
        if (selected_animation_id_ && id == *selected_animation_id_) {
            continue;
        }
        auto payload_text = document_->animation_payload(id);
        if (!payload_text.has_value()) {
            continue;
        }
        nlohmann::json payload = nlohmann::json::parse(*payload_text, nullptr, false);
        if (payload.is_discarded() || !payload.is_object()) {
            continue;
        }
        std::string kind = "folder";
        if (payload.contains("source") && payload["source"].is_object()) {
            kind = payload["source"].value("kind", std::string{"folder"});
        }
        const bool sourced_from_animation =
            animation_editor::strings::to_lower_copy(kind) == std::string{"animation"};
        bool inherits_data = false;
        if (payload.contains("inherit_data")) {
            inherits_data = payload.value("inherit_data", false);
        } else {
            inherits_data = payload.value("inherit_source_geometry", false);
        }
        if (!sourced_from_animation || !inherits_data) {
        selectable.push_back(id);
    }
    }

    if (selectable.empty()) return std::nullopt;

    std::ostringstream oss;
    oss << "Animations with editable geometry:\n";
    for (const auto& id : selectable) {
        oss << " - " << id << "\n";
    }

    std::optional<std::string> result;
    if (text_prompt_override_) {
        result = text_prompt_override_("Select Animation", oss.str(), selectable.front());
    } else {
        result = devmode::dialogs::prompt_text(parent_window_, "Select Animation", oss.str(), selectable.front());
    }
    if (!result) return std::nullopt;
    std::string choice = animation_editor::strings::trim_copy(*result);
    if (choice.empty()) return std::nullopt;

    auto match_it = std::find(selectable.begin(), selectable.end(), choice);
    if (match_it == selectable.end()) {
        std::string lowered = animation_editor::strings::to_lower_copy(choice);
        match_it = std::find_if(selectable.begin(), selectable.end(), [&](const std::string& value) {
            return animation_editor::strings::to_lower_copy(value) == lowered;
        });
        if (match_it == selectable.end()) {
            return std::nullopt;
        }
        choice = *match_it;
    }
    return choice;
}

std::optional<std::filesystem::path> AnimationEditorWindow::pick_audio_file() const {
    std::filesystem::path default_path;
    if (!asset_root_path_.empty()) {
        default_path = asset_root_path_ / default_audio_subdir();
    }
    return devmode::dialogs::open_file(parent_window_,
                                       "Select Audio Clip",
                                       default_path,
                                       {
                                           devmode::dialogs::FileDialogFilter{"Audio Files", "wav;ogg;mp3"},
                                           devmode::dialogs::FileDialogFilter{"WAV", "wav"},
                                           devmode::dialogs::FileDialogFilter{"OGG", "ogg"},
                                           devmode::dialogs::FileDialogFilter{"MP3", "mp3"},
                                       });
}

}
