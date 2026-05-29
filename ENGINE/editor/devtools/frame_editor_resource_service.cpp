#include "frame_editor_resource_service.hpp"

#include <SDL3/SDL.h>
#include <SDL3_image/SDL_image.h>

#include <algorithm>
#include <chrono>
#include <limits>
#include <set>
#include <system_error>
#include <unordered_set>

namespace devmode::frame_editor {
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

std::string source_kind_for(const nlohmann::json& payload) {
    if (!payload.is_object()) {
        return {};
    }
    const auto source_it = payload.find("source");
    if (source_it == payload.end() || !source_it->is_object()) {
        return {};
    }
    const auto kind_it = source_it->find("kind");
    if (kind_it == source_it->end() || !kind_it->is_string()) {
        return {};
    }
    return kind_it->get<std::string>();
}

std::string source_path_for(const nlohmann::json& payload, const std::string& animation_id) {
    const auto source_it = payload.find("source");
    if (source_it == payload.end() || !source_it->is_object()) {
        return animation_id;
    }
    const auto path_it = source_it->find("path");
    if (path_it == source_it->end() || !path_it->is_string()) {
        return animation_id;
    }
    const std::string path = path_it->get<std::string>();
    return path.empty() ? animation_id : path;
}

bool validate_image_file(const std::filesystem::path& path, std::string& error_message) {
    std::error_code ec;
    if (!std::filesystem::exists(path, ec) || ec || !std::filesystem::is_regular_file(path, ec) || ec) {
        error_message = "Image file is missing: '" + path_string(path) + "'";
        return false;
    }
    SDL_Surface* loaded = IMG_Load(path.string().c_str());
    if (!loaded) {
        error_message = "Image file is not readable: '" + path_string(path) + "': " + SDL_GetError();
        return false;
    }
    const bool valid = loaded->w > 0 && loaded->h > 0;
    SDL_DestroySurface(loaded);
    if (!valid) {
        error_message = "Image file has invalid dimensions: '" + path_string(path) + "'";
    }
    return valid;
}

bool write_image_as_png(const std::filesystem::path& source,
                        const std::filesystem::path& destination,
                        std::string& error_message) {
    SDL_Surface* loaded = IMG_Load(source.string().c_str());
    if (!loaded) {
        error_message = "Failed to decode image '" + path_string(source) + "': " + SDL_GetError();
        return false;
    }

    SDL_Surface* converted = SDL_ConvertSurface(loaded, SDL_PIXELFORMAT_RGBA32);
    SDL_DestroySurface(loaded);
    if (!converted) {
        error_message = "Failed to convert image '" + path_string(source) + "': " + SDL_GetError();
        return false;
    }

    const bool wrote = IMG_SavePNG(converted, destination.string().c_str());
    SDL_DestroySurface(converted);
    if (!wrote) {
        error_message = "Failed to write staged PNG '" + path_string(destination) + "': " + SDL_GetError();
        return false;
    }
    return validate_image_file(destination, error_message);
}

bool validate_frame_sequence(const std::filesystem::path& folder,
                             int frame_count,
                             std::string& error_message) {
    if (frame_count <= 0) {
        error_message = "Animation must keep at least one frame.";
        return false;
    }
    for (int i = 0; i < frame_count; ++i) {
        const std::filesystem::path frame = folder / (std::to_string(i) + ".png");
        if (!validate_image_file(frame, error_message)) {
            return false;
        }
    }
    return true;
}

std::filesystem::path make_temp_path(const std::filesystem::path& folder,
                                     const std::string& prefix,
                                     int index) {
    const auto now = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    return folder / (".frame_editor_" + prefix + "_" + std::to_string(now) + "_" + std::to_string(index) + ".png");
}

std::vector<int> sanitize_indices(const std::vector<int>& indices, int frame_count) {
    std::set<int> ordered;
    for (int index : indices) {
        if (index >= 0 && index < frame_count) {
            ordered.insert(index);
        }
    }
    return std::vector<int>(ordered.begin(), ordered.end());
}

nlohmann::json default_frame_json_for_key(const std::string& key) {
    if (key == "movement") {
        return nlohmann::json{{"dx", 0}, {"dy", 0}, {"dz", 0}};
    }
    return nlohmann::json::array();
}

nlohmann::json remap_frame_array(const nlohmann::json& source,
                                 const std::vector<std::optional<int>>& source_indices,
                                 const std::string& key) {
    nlohmann::json remapped = nlohmann::json::array();
    for (const std::optional<int>& old_index : source_indices) {
        if (old_index && *old_index >= 0 && source.is_array() && static_cast<std::size_t>(*old_index) < source.size()) {
            remapped.push_back(source[static_cast<std::size_t>(*old_index)]);
        } else {
            remapped.push_back(default_frame_json_for_key(key));
        }
    }
    return remapped;
}

void remap_known_frame_json(nlohmann::json& payload,
                            const std::vector<std::optional<int>>& source_indices) {
    static constexpr const char* kFrameArrayKeys[] = {
        "anchor_points",
        "hit_boxes",
        "attack_boxes",
        "movement",
    };

    for (const char* raw_key : kFrameArrayKeys) {
        const std::string key(raw_key);
        auto it = payload.find(key);
        if (it != payload.end() && it->is_array()) {
            *it = remap_frame_array(*it, source_indices, key);
        }
    }

    auto movement_paths_it = payload.find("movement_paths");
    if (movement_paths_it != payload.end() && movement_paths_it->is_array()) {
        nlohmann::json remapped_paths = nlohmann::json::array();
        for (const nlohmann::json& path : *movement_paths_it) {
            if (path.is_array()) {
                remapped_paths.push_back(remap_frame_array(path, source_indices, "movement"));
            } else {
                remapped_paths.push_back(path);
            }
        }
        *movement_paths_it = std::move(remapped_paths);
    }

    payload["number_of_frames"] = static_cast<int>(source_indices.size());
}

} // namespace

struct FrameEditorResourceService::ResolvedAnimation {
    nlohmann::json* payload = nullptr;
    std::filesystem::path asset_root;
    std::filesystem::path raw_folder;
    int frame_count = 0;
};

struct FrameEditorResourceService::FramePlanEntry {
    std::optional<int> old_index;
    std::optional<int> external_index;
};

FrameEditorResourceService::FrameEditorResourceService(FrameEditorResourceContext context)
    : context_(std::move(context)) {}

nlohmann::json* FrameEditorResourceService::payload() const {
    if (context_.animation_payload) {
        return context_.animation_payload;
    }
    if (context_.animation_payload_provider) {
        return context_.animation_payload_provider();
    }
    return nullptr;
}

FrameEditorResourceResult FrameEditorResourceService::fail(std::string message) const {
    FrameEditorResourceResult result;
    result.message = std::move(message);
    return result;
}

std::optional<FrameEditorResourceService::ResolvedAnimation>
FrameEditorResourceService::resolve_editable_animation(FrameEditorResourceResult& result) const {
    nlohmann::json* animation_payload = payload();
    if (!animation_payload || !animation_payload->is_object()) {
        result = fail("Animation payload is unavailable.");
        return std::nullopt;
    }
    if (context_.animation_id.empty()) {
        result = fail("Animation id is empty.");
        return std::nullopt;
    }

    const std::string kind = source_kind_for(*animation_payload);
    if (kind == "animation") {
        result = fail("Inherited animation '" + context_.animation_id + "' is not directly editable.");
        return std::nullopt;
    }
    if (kind != "folder") {
        result = fail("Animation '" + context_.animation_id + "' must use source.kind == 'folder'.");
        return std::nullopt;
    }

    const auto frame_count_it = animation_payload->find("number_of_frames");
    if (frame_count_it == animation_payload->end() ||
        !(frame_count_it->is_number_integer() || frame_count_it->is_number_unsigned())) {
        result = fail("Animation '" + context_.animation_id + "' is missing number_of_frames.");
        return std::nullopt;
    }
    const int frame_count = frame_count_it->get<int>();
    if (frame_count <= 0) {
        result = fail("Animation '" + context_.animation_id + "' has no editable frames.");
        return std::nullopt;
    }

    std::filesystem::path asset_root = context_.asset_root.empty() ? context_.asset_directory : context_.asset_root;
    if (asset_root.empty()) {
        result = fail("Asset directory is unavailable.");
        return std::nullopt;
    }
    asset_root = asset_root.lexically_normal();

    const std::filesystem::path source_path = source_path_for(*animation_payload, context_.animation_id);
    const std::filesystem::path raw_folder = (asset_root / source_path).lexically_normal();
    if (!path_has_prefix(raw_folder, asset_root)) {
        result = fail("Refusing to edit animation folder outside asset directory: '" + path_string(raw_folder) + "'");
        return std::nullopt;
    }

    std::error_code ec;
    if (!std::filesystem::exists(raw_folder, ec) || ec || !std::filesystem::is_directory(raw_folder, ec) || ec) {
        result = fail("Animation folder is missing: '" + path_string(raw_folder) + "'");
        return std::nullopt;
    }

    std::string validation_error;
    if (!validate_frame_sequence(raw_folder, frame_count, validation_error)) {
        result = fail(validation_error);
        return std::nullopt;
    }

    return ResolvedAnimation{animation_payload, asset_root, raw_folder, frame_count};
}

FrameEditorResourceResult FrameEditorResourceService::apply_plan(
    const ResolvedAnimation& resolved,
    const std::vector<FramePlanEntry>& plan,
    const std::vector<std::filesystem::path>& external_images) {
    if (plan.empty()) {
        return fail("Animation must keep at least one frame.");
    }
    if (plan.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return fail("Animation frame count is too large.");
    }

    std::error_code ec;
    std::vector<std::filesystem::path> staged_existing(static_cast<std::size_t>(resolved.frame_count));
    std::vector<std::filesystem::path> staged_external(external_images.size());
    std::vector<std::filesystem::path> written_finals;

    auto cleanup_temps = [&]() {
        std::error_code cleanup_ec;
        for (const auto& path : staged_existing) {
            if (!path.empty()) {
                std::filesystem::remove(path, cleanup_ec);
                cleanup_ec.clear();
            }
        }
        for (const auto& path : staged_external) {
            if (!path.empty()) {
                std::filesystem::remove(path, cleanup_ec);
                cleanup_ec.clear();
            }
        }
    };

    auto rollback_existing = [&]() {
        std::error_code rollback_ec;
        for (const auto& final_path : written_finals) {
            std::filesystem::remove(final_path, rollback_ec);
            rollback_ec.clear();
        }
        for (int i = 0; i < resolved.frame_count; ++i) {
            const std::filesystem::path original = resolved.raw_folder / (std::to_string(i) + ".png");
            if (!staged_existing[static_cast<std::size_t>(i)].empty() &&
                std::filesystem::exists(staged_existing[static_cast<std::size_t>(i)], rollback_ec) && !rollback_ec) {
                std::filesystem::rename(staged_existing[static_cast<std::size_t>(i)], original, rollback_ec);
                rollback_ec.clear();
            }
        }
        cleanup_temps();
    };

    for (std::size_t i = 0; i < external_images.size(); ++i) {
        staged_external[i] = make_temp_path(resolved.raw_folder, "new", static_cast<int>(i));
        std::string error;
        if (!write_image_as_png(external_images[i], staged_external[i], error)) {
            cleanup_temps();
            return fail(error);
        }
    }

    for (int i = 0; i < resolved.frame_count; ++i) {
        const std::filesystem::path original = resolved.raw_folder / (std::to_string(i) + ".png");
        const std::filesystem::path staged = make_temp_path(resolved.raw_folder, "old", i);
        std::filesystem::rename(original, staged, ec);
        if (ec) {
            rollback_existing();
            return fail("Failed to stage raw frame '" + path_string(original) + "': " + ec.message());
        }
        staged_existing[static_cast<std::size_t>(i)] = staged;
    }

    for (std::size_t i = 0; i < plan.size(); ++i) {
        const FramePlanEntry& entry = plan[i];
        std::filesystem::path source;
        if (entry.external_index) {
            if (*entry.external_index < 0 || static_cast<std::size_t>(*entry.external_index) >= staged_external.size()) {
                rollback_existing();
                return fail("Frame plan referenced an invalid staged image.");
            }
            source = staged_external[static_cast<std::size_t>(*entry.external_index)];
        } else if (entry.old_index) {
            if (*entry.old_index < 0 || *entry.old_index >= resolved.frame_count) {
                rollback_existing();
                return fail("Frame plan referenced an invalid source frame.");
            }
            source = staged_existing[static_cast<std::size_t>(*entry.old_index)];
        } else {
            rollback_existing();
            return fail("Frame plan contained an empty source.");
        }

        const std::filesystem::path destination = resolved.raw_folder / (std::to_string(i) + ".png");
        std::filesystem::copy_file(source, destination, std::filesystem::copy_options::overwrite_existing, ec);
        if (ec) {
            rollback_existing();
            return fail("Failed to write raw frame '" + path_string(destination) + "': " + ec.message());
        }
        written_finals.push_back(destination);
    }

    const int final_count = static_cast<int>(plan.size());
    std::string validation_error;
    if (!validate_frame_sequence(resolved.raw_folder, final_count, validation_error)) {
        rollback_existing();
        return fail(validation_error);
    }

    std::vector<std::optional<int>> json_sources;
    json_sources.reserve(plan.size());
    for (const FramePlanEntry& entry : plan) {
        json_sources.push_back(entry.old_index);
    }
    remap_known_frame_json(*resolved.payload, json_sources);

    if (context_.save_manifest && !context_.save_manifest()) {
        FrameEditorResourceResult result = fail("Failed to save manifest after raw frame resource update.");
        result.raw_folder = resolved.raw_folder;
        result.frame_count = final_count;
        cleanup_temps();
        return result;
    }

    cleanup_temps();
    const std::string asset_name = context_.asset_name.empty() ? std::string("<unknown>") : context_.asset_name;
    SDL_Log("[FrameEditor] Raw frame resources changed for %s/%s; cache rebuild required next run.",
            asset_name.c_str(),
            context_.animation_id.c_str());

    FrameEditorResourceResult result;
    result.success = true;
    result.frame_count = final_count;
    result.raw_folder = resolved.raw_folder;
    result.message = "Raw frame resources updated.";
    return result;
}

FrameEditorResourceResult FrameEditorResourceService::delete_frames(const std::vector<int>& selected_indices) {
    FrameEditorResourceResult result;
    auto resolved = resolve_editable_animation(result);
    if (!resolved) {
        return result;
    }

    const std::vector<int> selected = sanitize_indices(selected_indices, resolved->frame_count);
    if (selected.empty()) {
        return fail("No valid frames were selected.");
    }
    std::unordered_set<int> selected_set(selected.begin(), selected.end());
    std::vector<FramePlanEntry> plan;
    for (int i = 0; i < resolved->frame_count; ++i) {
        if (selected_set.find(i) == selected_set.end()) {
            plan.push_back(FramePlanEntry{i, std::nullopt});
        }
    }
    return apply_plan(*resolved, plan, {});
}

FrameEditorResourceResult FrameEditorResourceService::duplicate_frames(const std::vector<int>& selected_indices) {
    FrameEditorResourceResult result;
    auto resolved = resolve_editable_animation(result);
    if (!resolved) {
        return result;
    }

    const std::vector<int> selected = sanitize_indices(selected_indices, resolved->frame_count);
    if (selected.empty()) {
        return fail("No valid frames were selected.");
    }
    std::unordered_set<int> selected_set(selected.begin(), selected.end());
    std::vector<FramePlanEntry> plan;
    for (int i = 0; i < resolved->frame_count; ++i) {
        plan.push_back(FramePlanEntry{i, std::nullopt});
        if (selected_set.find(i) != selected_set.end()) {
            plan.push_back(FramePlanEntry{i, std::nullopt});
        }
    }
    return apply_plan(*resolved, plan, {});
}

FrameEditorResourceResult FrameEditorResourceService::reorder_frames(const std::vector<int>& selected_indices,
                                                                      int insertion_index) {
    FrameEditorResourceResult result;
    auto resolved = resolve_editable_animation(result);
    if (!resolved) {
        return result;
    }

    const std::vector<int> selected = sanitize_indices(selected_indices, resolved->frame_count);
    if (selected.empty()) {
        return fail("No valid frames were selected.");
    }
    std::unordered_set<int> selected_set(selected.begin(), selected.end());
    insertion_index = std::clamp(insertion_index, 0, resolved->frame_count);
    int selected_before_insertion = 0;
    for (int index : selected) {
        if (index < insertion_index) {
            ++selected_before_insertion;
        }
    }
    int adjusted_insertion = insertion_index - selected_before_insertion;

    std::vector<FramePlanEntry> remaining;
    std::vector<FramePlanEntry> moving;
    for (int i = 0; i < resolved->frame_count; ++i) {
        if (selected_set.find(i) != selected_set.end()) {
            moving.push_back(FramePlanEntry{i, std::nullopt});
        } else {
            remaining.push_back(FramePlanEntry{i, std::nullopt});
        }
    }
    adjusted_insertion = std::clamp(adjusted_insertion, 0, static_cast<int>(remaining.size()));
    remaining.insert(remaining.begin() + adjusted_insertion, moving.begin(), moving.end());
    return apply_plan(*resolved, remaining, {});
}

FrameEditorResourceResult FrameEditorResourceService::insert_frame(int insertion_index,
                                                                    const std::filesystem::path& image_path) {
    FrameEditorResourceResult result;
    auto resolved = resolve_editable_animation(result);
    if (!resolved) {
        return result;
    }
    insertion_index = std::clamp(insertion_index, 0, resolved->frame_count);

    std::vector<FramePlanEntry> plan;
    for (int i = 0; i < resolved->frame_count; ++i) {
        if (i == insertion_index) {
            plan.push_back(FramePlanEntry{std::nullopt, 0});
        }
        plan.push_back(FramePlanEntry{i, std::nullopt});
    }
    if (insertion_index == resolved->frame_count) {
        plan.push_back(FramePlanEntry{std::nullopt, 0});
    }
    return apply_plan(*resolved, plan, {image_path});
}

FrameEditorResourceResult FrameEditorResourceService::replace_frame(int frame_index,
                                                                     const std::filesystem::path& image_path) {
    FrameEditorResourceResult result;
    auto resolved = resolve_editable_animation(result);
    if (!resolved) {
        return result;
    }
    if (frame_index < 0 || frame_index >= resolved->frame_count) {
        return fail("Frame index is out of range.");
    }

    std::vector<FramePlanEntry> plan;
    for (int i = 0; i < resolved->frame_count; ++i) {
        if (i == frame_index) {
            plan.push_back(FramePlanEntry{i, 0});
        } else {
            plan.push_back(FramePlanEntry{i, std::nullopt});
        }
    }
    return apply_plan(*resolved, plan, {image_path});
}

} // namespace devmode::frame_editor
