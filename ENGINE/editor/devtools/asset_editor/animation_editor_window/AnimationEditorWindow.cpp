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
#include "ui/tinyfiledialogs.h"
#ifdef _WIN32
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#  include <shobjidl.h>
#  include <shlwapi.h>
#endif
#include "utils/input.hpp"

#include "assets/asset/asset_info.hpp"
#include "devtools/core/manifest_store.hpp"
#include "devtools/dm_styles.hpp"
#include "devtools/draw_utils.hpp"
#include "devtools/widgets.hpp"
#include "core/AssetsManager.hpp"
#include "devtools/asset_paths.hpp"
#include "devtools/dev_mode_utils.hpp"
#include "core/AssetsManager.hpp"

namespace {

using animation_editor::AnimationEditorWindow;
namespace fs = std::filesystem;
namespace asset_paths = devmode::asset_paths;

constexpr int kAutoSaveDelayFrames = 12;

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

    add_button_ = std::make_unique<DMButton>("Add Animation", &DMStyles::CreateButton(), 160, DMButton::height());
    controller_button_ = std::make_unique<DMButton>("Add Controller", &DMStyles::CreateButton(), 140, DMButton::height());
    create_defaults_button_ = std::make_unique<DMButton>("Create Defaults", &DMStyles::CreateButton(), 170, DMButton::height());
    ensure_defaults_modal_widgets();
    layout_dirty_ = true;
}

AnimationEditorWindow::~AnimationEditorWindow() {
    if (document_) {
        document_->set_on_saved_callback(nullptr);
    }
    invalidate_inspector_background_cache();
}

void AnimationEditorWindow::set_visible(bool visible, bool process_close) {
    const bool notify_closed = (!visible && visible_ && process_close);
    if (!visible && visible_ && process_close) {
        close_defaults_modal();

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

        if (list_context_menu_) {
            list_context_menu_->close();
        }
    }
    visible_ = visible;
    if (notify_closed && on_closed_) {
        on_closed_();
    }
}

void AnimationEditorWindow::toggle_visible() { set_visible(!visible_); }

void AnimationEditorWindow::set_bounds(const SDL_Rect& bounds) {
    bounds_ = bounds;
    layout_dirty_ = true;
    layout_children();
}

void AnimationEditorWindow::set_info(const std::shared_ptr<AssetInfo>& info) {
    close_manifest_transaction();
    info_ = info;

    if (!document_) {
        document_ = std::make_shared<AnimationDocument>();
        document_->set_on_saved_callback([this]() { this->handle_document_saved(); });
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

    enum class SnapshotRecoverySource { None, AssetMetadata, AssetFolders, Manifest };
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
            if (document_) {
                document_->set_manifest_asset_key_debug(manifest_asset_key_);
            }
            persist_callback = [this](const nlohmann::json& payload) { return this->persist_manifest_payload(payload); };
            if (log_creation) {
                std::cerr << "[AnimationEditor] Created manifest entry for '" << info->name << "' as '" << manifest_asset_key_ << "'\n";
            }
        } else {
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
            attach_manifest_transaction(*key, false);
        } else {
            std::cerr << "[AnimationEditor] Unable to resolve manifest key for '" << info->name << "'\n";
            const std::string fallback_key = manifest_key_fallback(*info);
            if (!fallback_key.empty()) {
                attach_manifest_transaction(fallback_key, true);
            }
        }
    } else {
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
    update_controller_button_label();
    std::string asset_label = info->name.empty() ? std::string("asset") : info->name;
    const bool has_any_animations = !document_->animation_ids().empty();
    if (seeded_default) {
        set_status_message("Created default animation for " + asset_label + ".", 300);
    } else {
        switch (recovery_source) {
            case SnapshotRecoverySource::AssetMetadata:
                set_status_message("Recovered animations from asset metadata for " + asset_label + ".", 300);
                break;
            case SnapshotRecoverySource::AssetFolders:
                set_status_message("Recovered animations from asset folders for " + asset_label + ".", 300);
                break;
            default:
                if (has_any_animations) {
                    set_status_message("Loaded " + asset_label, 240);
                } else {
                    set_status_message("No animations found for " + asset_label + ".", 240);
                }
                break;
        }
    }
    auto_save_pending_ = false;
    auto_save_timer_frames_ = 0;
}

void AnimationEditorWindow::clear_info() {
    info_.reset();
    asset_root_path_.clear();
    close_manifest_transaction();
    close_defaults_modal();
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
    set_status_message("Select an asset to configure animations.", 240);
    auto_save_pending_ = false;
    auto_save_timer_frames_ = 0;
}

void AnimationEditorWindow::layout_children() {
    layout_dirty_ = false;
    const int padding = DMSpacing::panel_padding();
    const int header_gap = DMSpacing::small_gap();
    const int button_gap = DMSpacing::small_gap();
    const int header_control_height = DMButton::height();
    const int header_height = header_control_height + header_gap * 2;
    header_rect_ = SDL_Rect{bounds_.x, bounds_.y, bounds_.w, header_height};

    int y = header_rect_.y + header_gap;
    int left_x = header_rect_.x + padding;

    if (add_button_) {
        add_button_->set_rect(SDL_Rect{left_x, y, add_button_->rect().w, DMButton::height()});
        left_x += add_button_->rect().w + button_gap;
    }

    if (controller_button_) {
        controller_button_->set_rect(SDL_Rect{left_x, y, controller_button_->rect().w, DMButton::height()});
        left_x += controller_button_->rect().w + button_gap;
    }

    if (create_defaults_button_) {
        create_defaults_button_->set_rect(SDL_Rect{left_x, y, create_defaults_button_->rect().w, DMButton::height()});
        left_x += create_defaults_button_->rect().w + button_gap;
    }

    const int status_padding = DMSpacing::panel_padding();
    int status_height = DMStyles::Label().font_size + status_padding * 2;
    status_rect_ = SDL_Rect{bounds_.x, bounds_.y + bounds_.h - status_height, bounds_.w, status_height};

    int content_top = header_rect_.y + header_rect_.h + header_gap;
    int content_bottom = status_rect_.y - header_gap;
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

        list_rect_ = SDL_Rect{bounds_.x + padding, content_top, available_width, list_height};
        inspector_rect_ = SDL_Rect{bounds_.x + padding,
                                   list_rect_.y + list_rect_.h + gap,
                                   available_width,
                                   inspector_height};
    } else {
        int sidebar_width = std::clamp(available_width / 3, 260, 420);
        int inspector_gap = DMSpacing::panel_padding();
        if (available_width < sidebar_width + inspector_gap + 320) {
            inspector_gap = DMSpacing::small_gap();
        }
        list_rect_ = SDL_Rect{bounds_.x + padding, content_top, sidebar_width, content_height};
        int inspector_x = list_rect_.x + list_rect_.w + inspector_gap;
        int inspector_w = std::max(0, bounds_.x + bounds_.w - padding - inspector_x);
        inspector_rect_ = SDL_Rect{inspector_x, content_top, inspector_w, content_height};
    }
    if (list_panel_) list_panel_->set_bounds(list_rect_);
    if (inspector_panel_) inspector_panel_->set_bounds(inspector_rect_);

    invalidate_inspector_background_cache();
    layout_defaults_modal();

}

void AnimationEditorWindow::invalidate_inspector_background_cache() {
    if (inspector_background_cache_) {
        SDL_DestroyTexture(inspector_background_cache_);
        inspector_background_cache_ = nullptr;
    }
    inspector_background_dirty_ = true;
}

void AnimationEditorWindow::configure_list_panel() {
    if (!list_panel_) return;
    list_panel_->set_document(document_);
    list_panel_->set_preview_provider(preview_provider_);
    list_panel_->set_on_selection_changed([this](const std::optional<std::string>& animation_id) {
        this->select_animation(animation_id, true);
    });
    list_panel_->set_on_context_menu([this](const std::string& animation_id, const SDL_Point& location) {
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
    inspector_panel_->set_source_status_callback([this](const std::string& message) { this->set_status_message(message); });
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
    if (selected_animation_id_ == animation_id) {
        if (list_panel_) {
            list_panel_->set_selected_animation_id(selected_animation_id_);
        }
        if (inspector_panel_ && selected_animation_id_) {
            inspector_panel_->set_animation_id(*selected_animation_id_);
        }
        return;
    }

    selected_animation_id_ = animation_id;
    if (list_panel_) {
        list_panel_->set_selected_animation_id(selected_animation_id_);
    }
    if (inspector_panel_ && selected_animation_id_) {
        inspector_panel_->set_animation_id(*selected_animation_id_);
    }


    if (from_user) {
        if (selected_animation_id_) {
            set_status_message("Selected animation '" + *selected_animation_id_ + "'.", 150);
        } else {
            set_status_message("No animation selected.", 120);
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

void AnimationEditorWindow::handle_list_context_menu(const std::string& animation_id, const SDL_Point& location) {
    if (!document_) {
        return;
    }
    if (!list_context_menu_) {
        list_context_menu_ = std::make_unique<AnimationListContextMenu>();
    }

    select_animation(std::make_optional(animation_id), false);
    std::vector<AnimationListContextMenu::Option> options;
    options.push_back(AnimationListContextMenu::Option{
        "Rename...",
        [this, animation_id]() { this->prompt_rename_animation(animation_id); },
    });
    options.push_back(AnimationListContextMenu::Option{
        "Set as start",
        [this, animation_id]() { this->set_animation_as_start(animation_id); },
    });
    options.push_back(AnimationListContextMenu::Option{
        "Duplicate",
        [this, animation_id]() { this->duplicate_animation(animation_id); },
    });
    options.push_back(AnimationListContextMenu::Option{
        "Delete",
        [this, animation_id]() { this->delete_animation_with_confirmation(animation_id); },
    });

    list_context_menu_->open(bounds_, location, std::move(options));
    set_status_message("Context menu for '" + animation_id + "'.", 90);
}

void AnimationEditorWindow::update(const Input& input, int screen_w, int screen_h) {
    if (!visible_) return;

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

    render_background(renderer);
    render_header(renderer);
    if (list_panel_) list_panel_->render(renderer);
    render_inspector(renderer);
    render_status(renderer);
    if (list_context_menu_ && list_context_menu_->is_open()) {
        list_context_menu_->render(renderer);
    }

    DMDropdown::render_active_options(renderer);
    if (defaults_modal_visible_) {
        render_defaults_modal(renderer);
    }
}

bool AnimationEditorWindow::handle_event(const SDL_Event& e) {
    if (!visible_) return false;

    ensure_layout();

    if (defaults_modal_visible_) {
        if (handle_defaults_modal_event(e)) {
            return true;
        }
        if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN || e.type == SDL_EVENT_MOUSE_BUTTON_UP ||
            e.type == SDL_EVENT_MOUSE_MOTION || e.type == SDL_EVENT_MOUSE_WHEEL ||
            e.type == SDL_EVENT_KEY_DOWN || e.type == SDL_EVENT_TEXT_INPUT) {
            return true;
        }
    }

    if (auto* active_dd = DMDropdown::active_dropdown()) {
        if (active_dd->handle_event(e)) {

            if (inspector_panel_) {
                inspector_panel_->apply_dropdown_selections();
            }
            return true;
        }
    }

    if (list_context_menu_ && list_context_menu_->is_open()) {
        if (list_context_menu_->handle_event(e)) {
            return true;
        }

        if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
            SDL_Point p = sdl_mouse_util::ButtonPoint(e.button);
            SDL_Rect menu_bounds = list_context_menu_->bounds();
            if (!SDL_PointInRect(&p, &menu_bounds)) {
                list_context_menu_->close();
            }
        }

        if (e.type == SDL_EVENT_KEY_DOWN && e.key.key == SDLK_ESCAPE) {
            list_context_menu_->close();
            return true;
        }
    }

    if (inspector_panel_ && selected_animation_id_) {
        if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN || e.type == SDL_EVENT_MOUSE_BUTTON_UP ||
            e.type == SDL_EVENT_MOUSE_MOTION || e.type == SDL_EVENT_MOUSE_WHEEL) {
            int mx = 0, my = 0;
            if (e.type == SDL_EVENT_MOUSE_MOTION) { mx = e.motion.x; my = e.motion.y; }
            else if (e.type == SDL_EVENT_MOUSE_WHEEL) { sdl_mouse_util::GetMouseState(&mx, &my); }
            else { mx = e.button.x; my = e.button.y; }
            SDL_Point mp{mx, my};
            if (SDL_PointInRect(&mp, &inspector_rect_)) {

                (void)inspector_panel_->handle_event(e);
                return true;
            }
        }

        if (inspector_panel_->handle_event(e)) {
            return true;
        }
    }

    if (handle_header_event(e)) {
        return true;
    }

    if (list_panel_ && list_panel_->handle_event(e)) {
        return true;
    }

    if (e.type == SDL_EVENT_KEY_DOWN) {
        if (e.key.key == SDLK_ESCAPE) {
            set_visible(false);
            return true;
        }
    }

    if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN || e.type == SDL_EVENT_MOUSE_BUTTON_UP || e.type == SDL_EVENT_MOUSE_MOTION) {
        SDL_Point p;
        if (e.type == SDL_EVENT_MOUSE_MOTION) { p.x = e.motion.x; p.y = e.motion.y; }
        else { p.x = e.button.x; p.y = e.button.y; }

        if (list_context_menu_ && e.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
            list_context_menu_->close();
        }

        if (SDL_PointInRect(&p, &bounds_)) {
            return true;
        } else {

            return false;
        }
    }

    if (e.type == SDL_EVENT_MOUSE_WHEEL) {
        int mx = 0;
        int my = 0;
        sdl_mouse_util::GetMouseState(&mx, &my);
        SDL_Point p{mx, my};

        if (inspector_panel_ && selected_animation_id_ && SDL_PointInRect(&p, &inspector_rect_)) {
            return inspector_panel_->handle_event(e);
        }

        return false;
    }

    return false;
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

std::string AnimationEditorWindow::normalize_animation_name(std::string_view raw) const {
    std::string normalized = animation_editor::strings::trim_copy(raw);
    return animation_editor::strings::to_lower_copy(normalized);
}

void AnimationEditorWindow::prompt_rename_animation(const std::string& animation_id) {
    if (!document_) return;

    const char* input = tinyfd_inputBox("Rename Animation", "Enter new animation identifier", animation_id.c_str());
    if (!input) {
        set_status_message("Rename cancelled.", 120);
        return;
    }

    std::string desired = normalize_animation_name(input);
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

    auto before_ids = document_->animation_ids();
    document_->create_animation(animation_id);
    auto after_ids = document_->animation_ids();

    std::optional<std::string> created_id;
    for (const auto& id : after_ids) {
        if (std::find(before_ids.begin(), before_ids.end(), id) == before_ids.end()) {
            created_id = id;
            break;
        }
    }

    if (created_id) {
        if (auto payload = document_->animation_payload(animation_id)) {
            document_->replace_animation_payload(*created_id, *payload);
            preview_provider_->invalidate(*created_id);
        }
        select_animation(created_id, false);
        set_status_message("Duplicated animation to '" + *created_id + "'.", 240);
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
    int result = tinyfd_messageBox("Delete Animation", message.c_str(), "yesno", "warning", 0);
    if (result != 1) {
        set_status_message("Deletion cancelled.", 120);
        if (list_context_menu_) {
            list_context_menu_->close();
        }
        return;
    }

    std::string source_delete_error;
    std::string cache_delete_error;
    const bool removed_source_folder = remove_animation_source_folder(animation_id, source_delete_error);
    const bool removed_cache_folder = remove_animation_cache_folder(animation_id, cache_delete_error);

    if (!removed_source_folder && !source_delete_error.empty()) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[AnimationEditor] %s",
                    source_delete_error.c_str());
    }
    if (!removed_cache_folder && !cache_delete_error.empty()) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[AnimationEditor] %s",
                    cache_delete_error.c_str());
    }

    document_->delete_animation(animation_id);
    preview_provider_->invalidate(animation_id);
    if (!removed_source_folder || !removed_cache_folder) {
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

void AnimationEditorWindow::render_header(SDL_Renderer* renderer) const {
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    dm_draw::DrawBeveledRect( renderer, header_rect_, DMStyles::CornerRadius(), DMStyles::BevelDepth(), DMStyles::PanelHeader(), DMStyles::HighlightColor(), DMStyles::ShadowColor(), false, DMStyles::HighlightIntensity(), DMStyles::ShadowIntensity());

    std::string title = "Animation Editor";
    if (auto info_ptr = info_.lock()) {
        std::string name = info_ptr->name;
        if (name.empty()) {
            name = asset_root_path_.filename().string();
        }
        if (!name.empty()) {
            title += " - ";
            title += name;
        }
    } else if (!asset_root_path_.empty()) {
        title += " - ";
        title += asset_root_path_.filename().string();
    }

    if (add_button_) add_button_->render(renderer);
    if (controller_button_) controller_button_->render(renderer);
    if (create_defaults_button_) create_defaults_button_->render(renderer);

    int label_x = header_rect_.x + DMSpacing::panel_padding();
    if (add_button_) {
        label_x = std::max(label_x, add_button_->rect().x + add_button_->rect().w + DMSpacing::small_gap());
    }
    if (controller_button_) {
        label_x = std::max(label_x, controller_button_->rect().x + controller_button_->rect().w + DMSpacing::small_gap());
    }
    if (create_defaults_button_) {
        label_x = std::max(label_x, create_defaults_button_->rect().x + create_defaults_button_->rect().w + DMSpacing::small_gap());
    }
    render_label(renderer, title, label_x, header_rect_.y + DMSpacing::small_gap());
}

void AnimationEditorWindow::render_status(SDL_Renderer* renderer) const {
    if (status_message_.empty()) return;

    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    animation_editor::ui::draw_panel_background(renderer, status_rect_);

    render_label(renderer, status_message_, status_rect_.x + DMSpacing::panel_padding(), status_rect_.y + DMSpacing::panel_padding());
}

void AnimationEditorWindow::render_inspector(SDL_Renderer* renderer) const {
    if (!renderer) return;
    if (inspector_rect_.w <= 0 || inspector_rect_.h <= 0) {
        return;
    }

    render_inspector_background(renderer);

    if (inspector_panel_ && selected_animation_id_) {
        inspector_panel_->render(renderer);
        return;
    }

    std::string message = "Select an animation to edit.";
    int text_x = inspector_rect_.x + DMSpacing::panel_padding();
    int text_y = inspector_rect_.y + DMSpacing::panel_padding();
    render_label(renderer, message, text_x, text_y);
}

void AnimationEditorWindow::render_inspector_background(SDL_Renderer* renderer) const {
    if (!renderer) return;
    if (inspector_rect_.w <= 0 || inspector_rect_.h <= 0) {
        return;
    }

    if (!inspector_background_dirty_ &&
        inspector_background_cache_ &&
        inspector_background_cache_rect_.w == inspector_rect_.w &&
        inspector_background_cache_rect_.h == inspector_rect_.h) {
        sdl_render::Texture(renderer, inspector_background_cache_, nullptr, &inspector_rect_);
        return;
    }

    if (inspector_background_cache_) {
        SDL_DestroyTexture(inspector_background_cache_);
        inspector_background_cache_ = nullptr;
    }

    SDL_Texture* cache = SDL_CreateTexture(renderer, static_cast<SDL_PixelFormat>(SDL_PIXELFORMAT_RGBA8888), SDL_TEXTUREACCESS_TARGET, inspector_rect_.w, inspector_rect_.h);
    if (!cache) {
        return;
    }
    SDL_SetRenderTarget(renderer, cache);
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    SDL_Rect local_rect{0, 0, inspector_rect_.w, inspector_rect_.h};
    animation_editor::ui::draw_panel_background(renderer, local_rect);
    SDL_SetRenderTarget(renderer, nullptr);
    inspector_background_cache_ = cache;
    inspector_background_cache_rect_ = SDL_Rect{inspector_rect_.x, inspector_rect_.y, inspector_rect_.w, inspector_rect_.h};
    inspector_background_dirty_ = false;
    sdl_render::Texture(renderer, cache, nullptr, &inspector_rect_);
}

bool AnimationEditorWindow::handle_header_event(const SDL_Event& e) {
    bool consumed = false;
    auto handle_button = [&](const std::unique_ptr<DMButton>& button, auto&& callback) {
        if (!button) return;
        bool activated = button->handle_event(e);
        if (!activated) return;

        if (e.type == SDL_EVENT_MOUSE_BUTTON_UP && e.button.button == SDL_BUTTON_LEFT) {
            callback();
        }
        consumed = true;
};

    handle_button(add_button_, [this]() { create_animation_via_prompt(); });
    handle_button(controller_button_, [this]() { handle_controller_button_click(); });
    handle_button(create_defaults_button_, [this]() { open_defaults_modal(); });

    return consumed;
}

void AnimationEditorWindow::set_status_message(const std::string& message, int frames) {
    status_message_ = message;
    status_timer_frames_ = std::max(frames, 0);
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

void AnimationEditorWindow::open_defaults_modal() {
    ensure_defaults_modal_widgets();
    defaults_modal_visible_ = true;
    if (list_context_menu_) {
        list_context_menu_->close();
    }
    layout_defaults_modal();
}

void AnimationEditorWindow::close_defaults_modal() {
    defaults_modal_visible_ = false;
    defaults_modal_rect_ = SDL_Rect{0, 0, 0, 0};
}

void AnimationEditorWindow::layout_defaults_modal() {
    if (!defaults_modal_visible_) {
        defaults_modal_rect_ = SDL_Rect{0, 0, 0, 0};
        return;
    }

    ensure_defaults_modal_widgets();

    const int modal_width = std::clamp(bounds_.w - 120, 460, 720);
    const int modal_height = 430;
    defaults_modal_rect_ = SDL_Rect{
        bounds_.x + std::max(0, (bounds_.w - modal_width) / 2),
        bounds_.y + std::max(0, (bounds_.h - modal_height) / 2),
        modal_width,
        modal_height
    };

    const int padding = DMSpacing::panel_padding();
    const int row_gap = DMSpacing::small_gap();
    const int content_x = defaults_modal_rect_.x + padding;
    const int content_w = std::max(0, defaults_modal_rect_.w - padding * 2);
    int y = defaults_modal_rect_.y + padding + DMStyles::Label().font_size + row_gap + 6;

    if (defaults_basic_movement_checkbox_) {
        defaults_basic_movement_checkbox_->set_rect(SDL_Rect{content_x, y, content_w, DMCheckbox::height()});
        y += DMCheckbox::height() + row_gap;
    }
    if (defaults_diagonals_checkbox_) {
        defaults_diagonals_checkbox_->set_rect(SDL_Rect{content_x, y, content_w, DMCheckbox::height()});
        y += DMCheckbox::height() + row_gap;
    }
    if (defaults_elevation_checkbox_) {
        defaults_elevation_checkbox_->set_rect(SDL_Rect{content_x, y, content_w, DMCheckbox::height()});
        y += DMCheckbox::height() + row_gap;
    }
    if (defaults_3d_diagonals_checkbox_) {
        defaults_3d_diagonals_checkbox_->set_rect(SDL_Rect{content_x, y, content_w, DMCheckbox::height()});
        y += DMCheckbox::height() + row_gap;
    }
    if (defaults_distance_box_) {
        const int distance_h = defaults_distance_box_->preferred_height(content_w);
        defaults_distance_box_->set_rect(SDL_Rect{content_x, y, content_w, distance_h});
        y += distance_h + row_gap;
    }

    if (defaults_base_frames_button_) {
        if (defaults_base_frame_paths_.empty()) {
            defaults_base_frames_button_->set_text("Add Base Movement Animation");
        } else {
            defaults_base_frames_button_->set_text("Base Frames (" + std::to_string(defaults_base_frame_paths_.size()) + ")");
        }
        const int button_w = std::min(content_w, std::max(280, defaults_base_frames_button_->preferred_width() + 24));
        defaults_base_frames_button_->set_rect(SDL_Rect{content_x, y, button_w, DMButton::height()});
    }

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
    if (!defaults_modal_visible_) {
        return false;
    }

    ensure_defaults_modal_widgets();
    layout_defaults_modal();

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
        if (SDL_PointInRect(&point, &defaults_modal_rect_)) {
            return true;
        }
        if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN || e.type == SDL_EVENT_MOUSE_BUTTON_UP) {
            return true;
        }
    }

    return consumed;
}

void AnimationEditorWindow::render_defaults_modal(SDL_Renderer* renderer) const {
    if (!defaults_modal_visible_ || !renderer || defaults_modal_rect_.w <= 0 || defaults_modal_rect_.h <= 0) {
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

    if (defaults_basic_movement_checkbox_) defaults_basic_movement_checkbox_->render(renderer);
    if (defaults_diagonals_checkbox_) defaults_diagonals_checkbox_->render(renderer);
    if (defaults_elevation_checkbox_) defaults_elevation_checkbox_->render(renderer);
    if (defaults_3d_diagonals_checkbox_) defaults_3d_diagonals_checkbox_->render(renderer);
    if (defaults_distance_box_) defaults_distance_box_->render(renderer);
    if (defaults_base_frames_button_) defaults_base_frames_button_->render(renderer);
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
    std::vector<std::filesystem::path> picked = pick_png_sequence();
    if (picked.empty()) {
        set_status_message("Base frame selection cancelled.", 120);
        return;
    }

    std::vector<std::filesystem::path> filtered;
    filtered.reserve(picked.size());
    for (const auto& path : picked) {
        if (has_extension_ci(path, ".png")) {
            filtered.push_back(path);
        }
    }
    if (filtered.empty()) {
        set_status_message("No PNG frames selected.", 180);
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

bool AnimationEditorWindow::copy_frames_to_animation_folder(const std::string& animation_id,
                                                            const std::vector<std::filesystem::path>& frames) {
    if (asset_root_path_.empty() || animation_id.empty() || frames.empty()) {
        return false;
    }

    std::filesystem::path output_dir = asset_root_path_ / animation_id;
    std::error_code ec;
    std::filesystem::create_directories(output_dir, ec);
    if (ec) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[AnimationEditor] Failed to prepare output folder '%s': %s",
                     output_dir.generic_string().c_str(),
                     ec.message().c_str());
        return false;
    }

    for (const auto& entry : std::filesystem::directory_iterator(output_dir, ec)) {
        if (ec) break;
        if (!entry.is_regular_file()) continue;
        if (!has_extension_ci(entry.path(), ".png")) continue;
        std::error_code remove_ec;
        std::filesystem::remove(entry.path(), remove_ec);
    }
    ec.clear();

    int copied = 0;
    for (const auto& source : frames) {
        if (!std::filesystem::exists(source, ec) || !std::filesystem::is_regular_file(source, ec)) {
            ec.clear();
            continue;
        }
        if (!has_extension_ci(source, ".png")) {
            continue;
        }
        std::filesystem::path destination = output_dir / (std::to_string(copied) + ".png");
        std::filesystem::copy_file(source, destination, std::filesystem::copy_options::overwrite_existing, ec);
        if (!ec) {
            ++copied;
        } else {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "[AnimationEditor] Failed to copy '%s' -> '%s': %s",
                        source.generic_string().c_str(),
                        destination.generic_string().c_str(),
                        ec.message().c_str());
            ec.clear();
        }
    }

    return copied > 0;
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

bool AnimationEditorWindow::remove_animation_cache_folder(const std::string& animation_id,
                                                          std::string& error_message) {
    error_message.clear();
    if (animation_id.empty()) {
        return true;
    }

    std::string asset_cache_key;
    if (auto info_ptr = info_.lock()) {
        asset_cache_key = info_ptr->name;
    }
    if (asset_cache_key.empty()) {
        asset_cache_key = asset_root_path_.filename().string();
    }
    if (asset_cache_key.empty()) {
        return true;
    }

    const std::filesystem::path cache_root = std::filesystem::path("cache").lexically_normal();
    const std::filesystem::path cache_animations_root = (cache_root / asset_cache_key / "animations").lexically_normal();
    const std::filesystem::path target_folder = (cache_animations_root / animation_id).lexically_normal();
    if (!path_has_prefix(target_folder, cache_animations_root)) {
        error_message = "Refusing to delete animation cache folder outside cache root: '" + target_folder.generic_string() + "'";
        return false;
    }

    std::error_code ec;
    if (std::filesystem::exists(target_folder, ec)) {
        ec.clear();
        std::filesystem::remove_all(target_folder, ec);
        if (ec) {
            error_message = "Failed to delete animation cache folder '" + target_folder.generic_string() + "': " + ec.message();
            return false;
        }
    }

    const std::filesystem::path bundle_path = (cache_root / asset_cache_key / "bundle.bin").lexically_normal();
    if (path_has_prefix(bundle_path, cache_root) && std::filesystem::exists(bundle_path, ec)) {
        ec.clear();
        std::filesystem::remove(bundle_path, ec);
        if (ec) {
            error_message = "Deleted animation cache folder but failed to delete bundle cache '" + bundle_path.generic_string() + "': " + ec.message();
            return false;
        }
    }

    return true;
}

nlohmann::json AnimationEditorWindow::build_file_sourced_movement_payload(const std::string& animation_id,
                                                                          int frame_count,
                                                                          int dx,
                                                                          int dy,
                                                                          int dz) const {
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
    return payload;
}

nlohmann::json AnimationEditorWindow::build_derived_movement_payload(const std::string& animation_id,
                                                                     const std::string& source_animation_id,
                                                                     int frame_count,
                                                                     int dx,
                                                                     int dy,
                                                                     int dz,
                                                                     bool invert_frames_horizontal) const {
    (void)animation_id;
    const int safe_frames = std::max(frame_count, 1);
    nlohmann::json payload = nlohmann::json::object();
    payload["source"] = nlohmann::json::object({
        {"kind", "animation"},
        {"path", ""},
        {"name", source_animation_id}
    });
    payload["number_of_frames"] = safe_frames;
    payload["inherit_data"] = false;
    payload["movement"] = build_movement_sequence(safe_frames, dx, dy, dz);
    payload["movement_total"] = build_movement_total(payload["movement"]);
    payload["anchor_points"] = build_empty_geometry_frames(safe_frames);
    payload["locked"] = false;
    payload["reverse_source"] = false;
    payload["invert_x"] = false;
    payload["invert_y"] = false;
    payload["invert_z"] = false;
    payload["invert_frames_horizontal"] = invert_frames_horizontal;
    payload["invert_frames_vertical"] = false;
    payload["rnd_start"] = false;
    payload["on_end"] = "default";
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

    document_->create_animation(animation_id);
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

    std::vector<std::filesystem::path> base_frames;
    base_frames.reserve(defaults_base_frame_paths_.size());
    for (const auto& path : defaults_base_frame_paths_) {
        if (has_extension_ci(path, ".png")) {
            base_frames.push_back(path);
        }
    }
    if (base_frames.empty()) {
        set_status_message("Select at least one base PNG frame.", 180);
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
    bool ok = true;
    std::vector<std::string> created_ids;

    auto create_file_based = [&](const std::string& id, int dx, int dy, int dz) {
        if (!ok) return;
        if (!copy_frames_to_animation_folder(id, base_frames)) {
            ok = false;
            return;
        }
        nlohmann::json payload = build_file_sourced_movement_payload(id, frame_count, dx, dy, dz);
        if (!create_or_replace_animation_payload(id, payload)) {
            ok = false;
            return;
        }
        created_ids.push_back(id);
    };

    auto create_derived = [&](const std::string& id,
                              const std::string& source,
                              int dx,
                              int dy,
                              int dz,
                              bool invert_frames_horizontal) {
        if (!ok) return;
        nlohmann::json payload =
            build_derived_movement_payload(id, source, frame_count, dx, dy, dz, invert_frames_horizontal);
        if (!create_or_replace_animation_payload(id, payload)) {
            ok = false;
            return;
        }
        created_ids.push_back(id);
    };

    if (create_basic) {
        create_file_based("left", -d, 0, 0);
        create_file_based("right", d, 0, 0);
        create_file_based("forward", 0, 0, -d);
        create_file_based("backward", 0, 0, d);
    }

    if (create_diagonals) {
        create_file_based("forward_left", -d, 0, -d);
        create_file_based("forward_right", d, 0, -d);
        create_file_based("backward_left", -d, 0, d);
        create_file_based("backward_right", d, 0, d);
    }

    if (create_elevation) {
        create_file_based("up", 0, d, 0);
        create_file_based("down", 0, -d, 0);
    }

    if (create_3d_diagonals) {
        create_file_based("up_forward_left", -d, d, -d);
        create_derived("up_forward_right", "up_forward_left", d, d, -d, true);
        create_derived("up_backward_left", "up_forward_left", -d, d, d, false);
        create_derived("up_backward_right", "up_forward_left", d, d, d, true);

        create_file_based("down_forward_left", -d, -d, -d);
        create_derived("down_forward_right", "down_forward_left", d, -d, -d, true);
        create_derived("down_backward_left", "down_forward_left", -d, -d, d, false);
        create_derived("down_backward_right", "down_forward_left", d, -d, d, true);
    }

    if (!ok) {
        set_status_message("Failed to create one or more default animations.", 240);
        return;
    }

    // Movement defaults are intentional authored movement data.
    if (manifest_store_ && !manifest_asset_key_.empty()) {
        (void)persist_manifest_payload(nlohmann::json{{"movement_enabled", true}}, false);
    }

    if (preview_provider_) {
        preview_provider_->invalidate_all();
    }
    if (!created_ids.empty()) {
        select_animation(std::optional<std::string>{created_ids.front()}, false);
    } else {
        ensure_selection_valid();
    }
    set_status_message("Created default movement animations.", 300);
    close_defaults_modal();
}

void AnimationEditorWindow::create_animation_via_prompt() {
    const char* input = tinyfd_inputBox("Create Animation", "Enter new animation identifier", "animation");
    if (!input) return;
    std::string name = normalize_animation_name(input);

    if (name.empty()) {
        return;
    }
    if (animation_editor::strings::is_reserved_animation_name(name)) {
        set_status_message("Animation name '" + name + "' is reserved.", 240);
        return;
    }
    document_->create_animation(name);
    preview_provider_->invalidate_all();
    select_animation(std::make_optional(name), false);
    set_status_message("Created animation '" + name + "'.", 240);
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
    std::string default_path = asset_root_path_.empty() ? std::string{} : asset_root_path_.string();
#ifdef _WIN32
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    IFileDialog* pfd = nullptr;
    std::optional<std::filesystem::path> picked;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&pfd)))) {
        DWORD options = 0;
        if (SUCCEEDED(pfd->GetOptions(&options))) {
            options |= FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM;
            pfd->SetOptions(options);
        }
        pfd->SetTitle(L"Upload Folder");
        if (!default_path.empty()) {
            IShellItem* psi = nullptr;
            std::wstring wpath(default_path.begin(), default_path.end());
            if (SUCCEEDED(SHCreateItemFromParsingName(wpath.c_str(), nullptr, IID_PPV_ARGS(&psi)))) {
                pfd->SetFolder(psi);
                psi->Release();
            }
        }
        if (SUCCEEDED(pfd->Show(nullptr))) {
            IShellItem* item = nullptr;
            if (SUCCEEDED(pfd->GetResult(&item))) {
                PWSTR psz = nullptr;
                if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &psz)) && psz) {
                    picked = std::filesystem::path(std::wstring(psz));
                    CoTaskMemFree(psz);
                }
                item->Release();
            }
        }
        pfd->Release();
    }
    if (SUCCEEDED(hr)) CoUninitialize();
    return picked;
#else
    const char* result = tinyfd_selectFolderDialog("Select Animation Folder", default_path.empty() ? nullptr : default_path.c_str());
    if (!result || std::string(result).empty()) {
        return std::nullopt;
    }
    return std::filesystem::path(result);
#endif
}

void AnimationEditorWindow::handle_controller_button_click() {
    if (does_controller_exist()) {
        open_controller();
    } else {
        add_controller();
    }
}

void AnimationEditorWindow::update_controller_button_label() {
    if (!controller_button_) return;
    if (does_controller_exist()) {
        controller_button_->set_text("Open Controller");
    } else {
        controller_button_->set_text("Add Controller");
    }
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
    std::string default_path = asset_root_path_.empty() ? std::string{} : asset_root_path_.string();
#ifdef _WIN32
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    IFileDialog* pfd = nullptr;
    std::optional<std::filesystem::path> picked;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&pfd)))) {
        DWORD options = 0;
        if (SUCCEEDED(pfd->GetOptions(&options))) {
            options |= FOS_FORCEFILESYSTEM;
            pfd->SetOptions(options);
        }
        pfd->SetTitle(L"Upload GIF");
        COMDLG_FILTERSPEC filters[] = { {L"GIF Image", L"*.gif"} };
        pfd->SetFileTypes(1, filters);
        pfd->SetDefaultExtension(L"gif");
        if (!default_path.empty()) {
            IShellItem* psi = nullptr;
            std::wstring wpath(default_path.begin(), default_path.end());
            if (SUCCEEDED(SHCreateItemFromParsingName(wpath.c_str(), nullptr, IID_PPV_ARGS(&psi)))) {
                pfd->SetFolder(psi);
                psi->Release();
            }
        }
        if (SUCCEEDED(pfd->Show(nullptr))) {
            IShellItem* item = nullptr;
            if (SUCCEEDED(pfd->GetResult(&item))) {
                PWSTR psz = nullptr;
                if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &psz)) && psz) {
                    picked = std::filesystem::path(std::wstring(psz));
                    CoTaskMemFree(psz);
                }
                item->Release();
            }
        }
        pfd->Release();
    }
    if (SUCCEEDED(hr)) CoUninitialize();
    return picked;
#else
    const char* filters[] = {"*.gif"};
    const char* result = tinyfd_openFileDialog("Import GIF", default_path.c_str(), 1, filters, "GIF Image", 0);
    if (!result || std::string(result).empty()) {
        return std::nullopt;
    }
    return std::filesystem::path(result);
#endif
}

std::vector<std::filesystem::path> AnimationEditorWindow::pick_png_sequence() const {
    std::string default_path = asset_root_path_.empty() ? std::string{} : asset_root_path_.string();
#ifdef _WIN32
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    IFileOpenDialog* pfd = nullptr;
    std::vector<std::filesystem::path> picked;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&pfd)))) {
        DWORD options = 0;
        if (SUCCEEDED(pfd->GetOptions(&options))) {
            options |= FOS_FORCEFILESYSTEM | FOS_ALLOWMULTISELECT;
            pfd->SetOptions(options);
        }
        pfd->SetTitle(L"Upload PNG");
        COMDLG_FILTERSPEC filters[] = { {L"PNG Images", L"*.png"} };
        pfd->SetFileTypes(1, filters);
        pfd->SetDefaultExtension(L"png");
        if (!default_path.empty()) {
            IShellItem* psi = nullptr;
            std::wstring wpath(default_path.begin(), default_path.end());
            if (SUCCEEDED(SHCreateItemFromParsingName(wpath.c_str(), nullptr, IID_PPV_ARGS(&psi)))) {
                pfd->SetFolder(psi);
                psi->Release();
            }
        }
        if (SUCCEEDED(pfd->Show(nullptr))) {
            IShellItemArray* items = nullptr;
            if (SUCCEEDED(pfd->GetResults(&items)) && items) {
                DWORD count = 0;
                if (SUCCEEDED(items->GetCount(&count))) {
                    for (DWORD i = 0; i < count; ++i) {
                        IShellItem* item = nullptr;
                        if (!SUCCEEDED(items->GetItemAt(i, &item)) || !item) {
                            continue;
                        }
                        PWSTR psz = nullptr;
                        if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &psz)) && psz) {
                            picked.emplace_back(std::wstring(psz));
                            CoTaskMemFree(psz);
                        }
                        item->Release();
                    }
                }
                items->Release();
            } else {
                IShellItem* item = nullptr;
                if (SUCCEEDED(pfd->GetResult(&item)) && item) {
                    PWSTR psz = nullptr;
                    if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &psz)) && psz) {
                        picked.emplace_back(std::wstring(psz));
                        CoTaskMemFree(psz);
                    }
                    item->Release();
                }
            }
        }
        pfd->Release();
    }
    if (SUCCEEDED(hr)) CoUninitialize();
    return picked;
#else
    const char* filters[] = {"*.png"};
    const char* result = tinyfd_openFileDialog("Upload PNG", default_path.c_str(), 1, filters, "PNG Images", 1);
    if (!result || std::string(result).empty()) {
        return {};
    }
    return split_paths(result);
#endif
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

    const char* result = tinyfd_inputBox("Select Animation", oss.str().c_str(), selectable.front().c_str());
    if (!result) return std::nullopt;
    std::string choice = animation_editor::strings::trim_copy(result);
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
    std::string default_path;
    if (!asset_root_path_.empty()) {
        default_path = (asset_root_path_ / default_audio_subdir()).string();
    }
#ifdef _WIN32
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    IFileDialog* pfd = nullptr;
    std::optional<std::filesystem::path> picked;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&pfd)))) {
        DWORD options = 0;
        if (SUCCEEDED(pfd->GetOptions(&options))) {
            options |= FOS_FORCEFILESYSTEM;
            pfd->SetOptions(options);
        }
        pfd->SetTitle(L"Select Audio Clip");
        COMDLG_FILTERSPEC filters[] = {
            {L"Audio Files", L"*.wav;*.ogg;*.mp3"},
            {L"WAV", L"*.wav"},
            {L"OGG", L"*.ogg"},
            {L"MP3", L"*.mp3"}
};
        pfd->SetFileTypes(4, filters);
        pfd->SetDefaultExtension(L"wav");
        if (!default_path.empty()) {
            IShellItem* psi = nullptr;
            std::wstring wpath(default_path.begin(), default_path.end());
            if (SUCCEEDED(SHCreateItemFromParsingName(wpath.c_str(), nullptr, IID_PPV_ARGS(&psi)))) {
                pfd->SetFolder(psi);
                psi->Release();
            }
        }
        if (SUCCEEDED(pfd->Show(nullptr))) {
            IShellItem* item = nullptr;
            if (SUCCEEDED(pfd->GetResult(&item))) {
                PWSTR psz = nullptr;
                if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &psz)) && psz) {
                    picked = std::filesystem::path(std::wstring(psz));
                    CoTaskMemFree(psz);
                }
                item->Release();
            }
        }
        pfd->Release();
    }
    if (SUCCEEDED(hr)) CoUninitialize();
    return picked;
#else
    const char* filters[] = {"*.wav", "*.ogg", "*.mp3"};
    const char* result = tinyfd_openFileDialog("Select Audio Clip", default_path.c_str(), 3, filters, "Audio Files", 0);
    if (!result || std::string(result).empty()) {
        return std::nullopt;
    }
    return std::filesystem::path(result);
#endif
}

}




