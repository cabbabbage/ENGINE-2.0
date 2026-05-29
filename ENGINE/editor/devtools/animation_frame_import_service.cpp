#include "animation_frame_import_service.hpp"

#include "asset_paths.hpp"
#include "dev_mode_utils.hpp"
#include "devtools/core/manifest_store.hpp"
#include "devtools/frame_importer.hpp"
#include "assets/asset/animation.hpp"
#include "assets/asset/asset_info.hpp"
#include "assets/asset/asset_library.hpp"
#include "core/AssetsManager.hpp"
#include "utils/weighted_range.hpp"

#include <SDL3/SDL.h>
#include <SDL3_image/SDL_image.h>

#include <algorithm>
#include <system_error>

namespace devmode::animation_import {
namespace {

std::string path_string(const std::filesystem::path& path) {
    return path.lexically_normal().generic_string();
}

bool path_has_prefix(std::filesystem::path path, std::filesystem::path prefix) {
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

void remove_tree(const std::filesystem::path& path) {
    if (path.empty()) {
        return;
    }
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
}

bool png_readable(const std::filesystem::path& path, std::string& error_message) {
    SDL_Surface* surface = IMG_Load(path.string().c_str());
    if (!surface) {
        error_message = "Frame is not readable as PNG '" + path_string(path) + "': " + SDL_GetError();
        return false;
    }
    const bool valid = surface->w > 0 && surface->h > 0;
    SDL_DestroySurface(surface);
    if (!valid) {
        error_message = "Frame has invalid dimensions: '" + path_string(path) + "'";
    }
    return valid;
}

} // namespace

nlohmann::json build_folder_animation_payload(const std::string& animation_id,
                                              int frame_count) {
    const int safe_frames = std::max(1, frame_count);
    return nlohmann::json{
        {"on_end", "default"},
        {"locked", false},
        {"reverse_source", false},
        {"invert_x", false},
        {"invert_y", false},
        {"invert_z", false},
        {"rnd_start", false},
        {"source", nlohmann::json{{"kind", "folder"}, {"path", animation_id}, {"name", ""}}},
        {"number_of_frames", safe_frames}
    };
}

nlohmann::json build_default_asset_manifest(const std::string& asset_name,
                                            const std::filesystem::path& asset_dir,
                                            int frame_count) {
    nlohmann::json manifest_entry = {
        {"asset_name", asset_name},
        {"asset_type", "Object"},
        {"movement_enabled", false},
        {"attack_box_enabled", false},
        {"hitbox_enabled", false},
        {"impassable_enabled", false},
        {"floor_boxes_enabled", false},
        {"animations", nlohmann::json{{"default", build_folder_animation_payload("default", frame_count)}}},
        {"start", "default"},
        {"asset_directory", asset_dir.lexically_normal().generic_string()},
        {"tags", nlohmann::json::array()},
        {"anti_tags", nlohmann::json::array()},
        {"neighbor_search_distance", 500},
        {"render_radius", 0},
        {"update_radius", 0},
        {"min_same_type_distance", 0},
        {"min_distance_all", 0},
        {"can_invert", false},
        {"tilt_range", vibble::weighted_range::to_json(vibble::weighted_range::make_flat(0))},
        {"y_position_range", vibble::weighted_range::to_json(vibble::weighted_range::make_flat(0))},
        {"size_settings", {
            {"scale_percentage", 100.0},
            {"size_variation", vibble::weighted_range::to_json(vibble::weighted_range::make_flat(0))}
        }}
    };
    return manifest_entry;
}

ImportResult import_frames_to_animation_folder(const std::filesystem::path& asset_root,
                                               const std::string& animation_id,
                                               const std::vector<std::filesystem::path>& input_paths) {
    ImportResult out;
    out.animation_ids.push_back(animation_id);
    if (asset_root.empty()) {
        out.message = "Asset directory is unavailable.";
        return out;
    }
    if (animation_id.empty()) {
        out.message = "Animation id is empty.";
        return out;
    }
    if (input_paths.empty()) {
        out.message = "No supported image/vector files were provided.";
        return out;
    }

    std::error_code ec;
    std::filesystem::create_directories(asset_root, ec);
    if (ec) {
        out.message = "Failed to create asset folder '" + path_string(asset_root) + "': " + ec.message();
        return out;
    }

    const std::filesystem::path output_dir = asset_root / animation_id;
    auto imported = devmode::frame_importer::import_frames_to_directory(input_paths, output_dir);
    out.frames_written = imported.frames_written;
    out.warnings = imported.warnings;
    if (!imported.success()) {
        out.message = imported.error_message.empty() ? "No supported image/vector frames were imported." : imported.error_message;
        if (!imported.failed_stage.empty()) {
            out.message = "Import failed during " + imported.failed_stage + ": " + out.message;
        }
        return out;
    }

    std::string verify_error;
    if (!verify_animation_files(asset_root, animation_id, imported.frames_written, verify_error)) {
        out.message = verify_error;
        return out;
    }

    out.success = true;
    out.message = "Imported " + std::to_string(imported.frames_written) + " frame(s).";
    return out;
}

bool delete_asset_cache(const std::string& asset_key, std::string& error_message) {
    error_message.clear();
    if (asset_key.empty()) {
        return true;
    }
    const std::filesystem::path cache_root = std::filesystem::path("cache").lexically_normal();
    const std::filesystem::path target = (cache_root / asset_key).lexically_normal();
    if (!path_has_prefix(target, cache_root)) {
        error_message = "Refusing to delete cache outside cache root: '" + path_string(target) + "'";
        return false;
    }
    std::error_code ec;
    if (std::filesystem::exists(target, ec)) {
        ec.clear();
        std::filesystem::remove_all(target, ec);
        if (ec) {
            error_message = "Failed to delete asset cache '" + path_string(target) + "': " + ec.message();
            return false;
        }
    }
    return true;
}

bool verify_animation_files(const std::filesystem::path& asset_root,
                            const std::string& animation_id,
                            int expected_frames,
                            std::string& error_message) {
    error_message.clear();
    if (expected_frames <= 0) {
        error_message = "Imported animation has no frames.";
        return false;
    }
    const std::filesystem::path folder = asset_root / animation_id;
    std::error_code ec;
    if (!std::filesystem::exists(folder, ec) || ec || !std::filesystem::is_directory(folder, ec) || ec) {
        error_message = "Animation folder is missing: '" + path_string(folder) + "'";
        return false;
    }
    for (int i = 0; i < expected_frames; ++i) {
        const std::filesystem::path frame = folder / (std::to_string(i) + ".png");
        if (!std::filesystem::exists(frame, ec) || ec || !std::filesystem::is_regular_file(frame, ec) || ec) {
            error_message = "Missing imported frame '" + path_string(frame) + "'";
            return false;
        }
        if (!png_readable(frame, error_message)) {
            return false;
        }
    }
    const std::filesystem::path unexpected = folder / (std::to_string(expected_frames) + ".png");
    if (std::filesystem::exists(unexpected, ec) && !ec) {
        error_message = "Imported animation has non-canonical extra frame '" + path_string(unexpected) + "'";
        return false;
    }
    return true;
}


bool verify_runtime_textures(const std::shared_ptr<AssetInfo>& info,
                             const std::string& animation_id,
                             int expected_frames,
                             std::string& error_message) {
    error_message.clear();

    if (!info) {
        error_message = "Runtime asset info is unavailable.";
        return false;
    }

    auto anim_it = info->animations.find(animation_id);
    if (anim_it == info->animations.end()) {
        error_message = "Runtime animation '" + animation_id + "' is missing.";
        return false;
    }

    Animation& animation = anim_it->second;
    const std::size_t cached_count = animation.cached_frame_count();
    if (cached_count != static_cast<std::size_t>(expected_frames)) {
        error_message = "Runtime animation '" + animation_id + "' has " +
                        std::to_string(cached_count) + " frame(s), expected " +
                        std::to_string(expected_frames) + ".";
        return false;
    }

    AnimationFrame* frame = animation.get_first_frame();
    for (int i = 0; i < expected_frames; ++i) {
        if (!frame) {
            error_message = "Runtime animation '" + animation_id + "' frame " +
                            std::to_string(i) + " is missing from the frame chain.";
            return false;
        }

        // Do not inspect Animation::FrameCache internals here. FrameCache layout
        // has changed, and AnimationFrame already owns the stable texture accessor.
        if (!frame->get_base_texture(0)) {
            error_message = "Runtime animation '" + animation_id + "' frame " +
                            std::to_string(i) + " has no usable base texture.";
            return false;
        }

        frame = frame->next;
    }

    return true;
}



ImportResult reload_and_verify_runtime(Assets* assets,
                                       const std::string& asset_key,
                                       const std::string& animation_id,
                                       int expected_frames) {
    ImportResult out;
    out.asset_key = asset_key;
    out.animation_ids.push_back(animation_id);
    out.frames_written = expected_frames;
    if (!assets) {
        out.success = true;
        out.message = "Imported frames; runtime verification skipped because no runtime context is available.";
        return out;
    }

    std::string cache_error;
    if (!delete_asset_cache(asset_key, cache_error)) {
        out.message = cache_error;
        return out;
    }

    auto info = assets->library().get(asset_key);
    if (!info) {
        out.message = "Runtime asset '" + asset_key + "' is unavailable.";
        return out;
    }
    if (!info->reload_animations_from_disk()) {
        out.message = "Failed to reload animation metadata for '" + asset_key + "'.";
        return out;
    }
    if (SDL_Renderer* renderer = assets->renderer()) {
        auto load_result = info->loadAnimationsDetailed(renderer, true, false, false);
        if (!load_result.ok()) {
            out.message = "Runtime texture load failed for imported animation.";
            return out;
        }
    }
    std::string verify_error;
    if (!verify_runtime_textures(info, animation_id, expected_frames, verify_error)) {
        out.message = verify_error;
        return out;
    }
    assets->mark_active_assets_dirty();
    out.success = true;
    out.message = "Imported and verified " + std::to_string(expected_frames) + " runtime frame(s).";
    return out;
}

ImportResult create_asset_from_frames(const CreateAssetRequest& request) {
    ImportResult out;
    const std::string sanitized =
        devmode::utils::normalize_asset_name(devmode::utils::trim_whitespace_copy(request.asset_name));
    out.asset_key = sanitized;
    out.animation_ids.push_back("default");

    if (sanitized.empty()) {
        out.message = "Please enter a name (letters, numbers, underscore).";
        return out;
    }
    if (!request.manifest_store) {
        out.message = "Manifest store is unavailable.";
        return out;
    }
    if (request.manifest_store->resolve_asset_name(sanitized) ||
        (request.assets && request.assets->library().get(sanitized))) {
        out.message = "An asset with that name already exists.";
        return out;
    }

    const std::filesystem::path asset_dir = devmode::asset_paths::asset_folder_path(sanitized);
    if (std::filesystem::exists(asset_dir)) {
        out.message = "Asset folder already exists on disk.";
        return out;
    }

    auto cleanup = [&]() {
        request.manifest_store->remove_asset(sanitized);
        request.manifest_store->flush();
        if (request.assets) {
            request.assets->library().remove(sanitized);
        }
        remove_tree(asset_dir);
        std::string ignored;
        (void)delete_asset_cache(sanitized, ignored);
    };

    ImportResult import_result = import_frames_to_animation_folder(asset_dir, "default", request.input_paths);
    out.warnings = import_result.warnings;
    if (!import_result.success) {
        out.message = import_result.message;
        cleanup();
        return out;
    }
    out.frames_written = import_result.frames_written;

    const nlohmann::json manifest_entry =
        build_default_asset_manifest(sanitized, asset_dir, import_result.frames_written);
    auto session = request.manifest_store->begin_asset_edit(sanitized, true);
    if (!session || !session.is_new_asset()) {
        out.message = "Manifest entry already exists.";
        cleanup();
        return out;
    }
    session.data() = manifest_entry;
    if (!session.commit()) {
        out.message = "Failed to write manifest entry.";
        cleanup();
        return out;
    }
    request.manifest_store->flush();

    if (request.assets) {
        request.assets->library().add_asset(sanitized, manifest_entry);
        ImportResult runtime = reload_and_verify_runtime(request.assets, sanitized, "default", import_result.frames_written);
        if (!runtime.success) {
            out.message = runtime.message.empty() ? "Runtime verification failed." : runtime.message;
            cleanup();
            return out;
        }
    }

    out.success = true;
    out.message = "Created asset '" + sanitized + "' with " +
                  std::to_string(import_result.frames_written) + " frame(s).";
    return out;
}

} // namespace devmode::animation_import
