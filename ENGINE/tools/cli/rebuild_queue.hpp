#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "cache_helper.hpp"

namespace imgcache {

namespace fs = std::filesystem;
using ordered_json = nlohmann::ordered_json;

inline ordered_json DefaultRebuildQueue() {
    ordered_json queue = ordered_json::object();
    queue["version"] = 1;
    queue["assets"] = ordered_json::object();
    return queue;
}

inline fs::path ResolveRebuildQueuePath(const fs::path& cache_root) {
    return cache_root / "rebuild_queue.json";
}

inline ordered_json LoadRebuildQueue(const fs::path& queue_path) {
    auto result = CacheHelper::LoadJsonFile(queue_path.string());
    if (!result.ok || !result.value.is_object()) {
        return DefaultRebuildQueue();
    }
    ordered_json queue = result.value;
    if (!queue.contains("assets") || !queue["assets"].is_object()) {
        queue["assets"] = ordered_json::object();
    }
    if (!queue.contains("version") || !queue["version"].is_number_integer()) {
        queue["version"] = 1;
    }
    return queue;
}

inline bool SaveRebuildQueue(const fs::path& queue_path, const ordered_json& queue) {
    auto result = CacheHelper::WriteJsonFile(queue_path.string(), nlohmann::json(queue));
    return result.ok;
}

inline ordered_json* FindAnimEntry(ordered_json& queue,
                                   const std::string& asset_name,
                                   const std::string& anim_name,
                                   bool create_if_missing) {
    if (!queue.is_object()) {
        return nullptr;
    }
    if (!queue.contains("assets") || !queue["assets"].is_object()) {
        if (!create_if_missing) {
            return nullptr;
        }
        queue["assets"] = ordered_json::object();
    }
    auto& assets = queue["assets"];
    if (!assets.contains(asset_name) || !assets[asset_name].is_object()) {
        if (!create_if_missing) {
            return nullptr;
        }
        assets[asset_name] = ordered_json::object();
    }
    auto& asset = assets[asset_name];
    if (!asset.contains("animations") || !asset["animations"].is_object()) {
        if (!create_if_missing) {
            return nullptr;
        }
        asset["animations"] = ordered_json::object();
    }
    auto& anims = asset["animations"];
    if (!anims.contains(anim_name) || !anims[anim_name].is_object()) {
        if (!create_if_missing) {
            return nullptr;
        }
        anims[anim_name] = ordered_json::object();
    }
    return &anims[anim_name];
}

inline void EnsureFramesArray(ordered_json& anim_entry, int frame_count) {
    if (!anim_entry.is_object()) {
        anim_entry = ordered_json::object();
    }
    ordered_json frames = ordered_json::array();
    if (anim_entry.contains("frames") && anim_entry["frames"].is_array()) {
        frames = anim_entry["frames"];
    }
    if (frame_count < 0) frame_count = 0;
    while (static_cast<int>(frames.size()) < frame_count) {
        ordered_json entry = ordered_json::object();
        entry["needs_rebuild"] = false;
        frames.push_back(entry);
    }
    anim_entry["frames"] = frames;
}

inline std::vector<int> FlaggedFrames(const ordered_json& anim_entry) {
    std::vector<int> out;
    if (!anim_entry.is_object() || !anim_entry.contains("frames") || !anim_entry["frames"].is_array()) {
        return out;
    }
    const auto& frames = anim_entry["frames"];
    for (size_t i = 0; i < frames.size(); ++i) {
        const auto& entry = frames[i];
        if (entry.is_boolean()) {
            if (entry.get<bool>()) {
                out.push_back(static_cast<int>(i));
            }
            continue;
        }
        if (entry.is_object() && entry.contains("needs_rebuild") &&
            entry["needs_rebuild"].is_boolean() && entry["needs_rebuild"].get<bool>()) {
            out.push_back(static_cast<int>(i));
        }
    }
    return out;
}

inline void SetFrameFlag(ordered_json& anim_entry, int frame_idx, bool value) {
    if (!anim_entry.is_object()) {
        anim_entry = ordered_json::object();
    }
    if (!anim_entry.contains("frames") || !anim_entry["frames"].is_array()) {
        anim_entry["frames"] = ordered_json::array();
    }
    auto& frames = anim_entry["frames"];
    if (frame_idx < 0) {
        return;
    }
    if (static_cast<int>(frames.size()) <= frame_idx) {
        EnsureFramesArray(anim_entry, frame_idx + 1);
    }
    auto& entry = frames[frame_idx];
    if (!entry.is_object()) {
        entry = ordered_json::object();
    }
    entry["needs_rebuild"] = value;
}

inline void MarkAllFrames(ordered_json& anim_entry, int frame_count, bool value) {
    EnsureFramesArray(anim_entry, frame_count);
    auto& frames = anim_entry["frames"];
    for (auto& entry : frames) {
        if (!entry.is_object()) {
            entry = ordered_json::object();
        }
        entry["needs_rebuild"] = value;
    }
}

} // namespace imgcache
