#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "cache_helper.hpp"

namespace imgcache {

namespace fs = std::filesystem;
using ordered_json = nlohmann::ordered_json;

inline constexpr std::uint8_t kVariantNormal = 1u << 0;
inline constexpr std::uint8_t kVariantForeground = 1u << 1;
inline constexpr std::uint8_t kVariantBackground = 1u << 2;
inline constexpr std::uint8_t kVariantAll = kVariantNormal | kVariantForeground | kVariantBackground;

inline std::string VariantNameForMaskBit(std::uint8_t bit) {
    switch (bit) {
    case kVariantNormal:
        return "normal";
    case kVariantForeground:
        return "foreground";
    case kVariantBackground:
        return "background";
    default:
        return {};
    }
}

inline std::uint8_t VariantMaskBitForName(const std::string& name) {
    if (name == "normal") {
        return kVariantNormal;
    }
    if (name == "foreground") {
        return kVariantForeground;
    }
    if (name == "background") {
        return kVariantBackground;
    }
    return 0;
}

inline ordered_json VariantArrayForMask(std::uint8_t mask) {
    ordered_json variants = ordered_json::array();
    if ((mask & kVariantNormal) != 0u) {
        variants.push_back("normal");
    }
    if ((mask & kVariantForeground) != 0u) {
        variants.push_back("foreground");
    }
    if ((mask & kVariantBackground) != 0u) {
        variants.push_back("background");
    }
    return variants;
}

inline std::optional<std::uint8_t> ParseVariantMask(const ordered_json& entry) {
    if (!entry.is_object()) {
        return std::nullopt;
    }
    if (!entry.contains("variants") || !entry["variants"].is_array()) {
        return std::nullopt;
    }

    std::uint8_t mask = 0;
    for (const auto& variant : entry["variants"]) {
        if (!variant.is_string()) {
            continue;
        }
        mask = static_cast<std::uint8_t>(mask | VariantMaskBitForName(variant.get<std::string>()));
    }

    if (mask == 0u) {
        return std::nullopt;
    }
    return mask;
}

inline bool FrameEntryNeedsRebuild(const ordered_json& entry) {
    if (entry.is_boolean()) {
        return entry.get<bool>();
    }
    if (!entry.is_object()) {
        return false;
    }
    auto it = entry.find("needs_rebuild");
    return it != entry.end() && it->is_boolean() && it->get<bool>();
}

struct FlaggedFrame {
    int index = -1;
    bool has_variant_mask = false;
    std::uint8_t variants = kVariantAll;
};

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
        if (FrameEntryNeedsRebuild(frames[i])) {
            out.push_back(static_cast<int>(i));
        }
    }
    return out;
}

inline std::vector<FlaggedFrame> FlaggedFramesDetailed(const ordered_json& anim_entry) {
    std::vector<FlaggedFrame> out;
    if (!anim_entry.is_object() || !anim_entry.contains("frames") || !anim_entry["frames"].is_array()) {
        return out;
    }
    const auto& frames = anim_entry["frames"];
    for (size_t i = 0; i < frames.size(); ++i) {
        const auto& entry = frames[i];
        if (!FrameEntryNeedsRebuild(entry)) {
            continue;
        }
        FlaggedFrame flagged;
        flagged.index = static_cast<int>(i);
        const std::optional<std::uint8_t> mask = ParseVariantMask(entry);
        if (mask.has_value()) {
            flagged.has_variant_mask = true;
            flagged.variants = *mask;
        }
        out.push_back(flagged);
    }
    return out;
}

inline void SetFrameFlag(ordered_json& anim_entry,
                         int frame_idx,
                         bool value,
                         std::optional<std::uint8_t> variant_mask = std::nullopt) {
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

    if (!value) {
        entry["needs_rebuild"] = false;
        entry.erase("variants");
        return;
    }

    const bool had_rebuild_flag = FrameEntryNeedsRebuild(entry);
    std::optional<std::uint8_t> existing_mask = ParseVariantMask(entry);
    std::uint8_t requested_mask = variant_mask.value_or(kVariantAll);
    requested_mask = static_cast<std::uint8_t>(requested_mask & kVariantAll);
    if (requested_mask == 0u) {
        requested_mask = kVariantAll;
    }

    std::uint8_t combined_mask = requested_mask;
    if (had_rebuild_flag) {
        if (existing_mask.has_value()) {
            combined_mask = static_cast<std::uint8_t>(combined_mask | *existing_mask);
        } else {
            combined_mask = kVariantAll;
        }
    }

    entry["needs_rebuild"] = true;
    if (combined_mask == kVariantAll) {
        entry.erase("variants");
    } else {
        entry["variants"] = VariantArrayForMask(combined_mask);
    }
}

inline void MarkAllFrames(ordered_json& anim_entry,
                          int frame_count,
                          bool value,
                          std::optional<std::uint8_t> variant_mask = std::nullopt) {
    EnsureFramesArray(anim_entry, frame_count);
    auto& frames = anim_entry["frames"];
    for (int i = 0; i < static_cast<int>(frames.size()); ++i) {
        SetFrameFlag(anim_entry, i, value, variant_mask);
    }
}

inline void ClearFrameFlag(ordered_json& anim_entry,
                           int frame_idx,
                           std::optional<std::uint8_t> cleared_mask = std::nullopt) {
    if (!anim_entry.is_object() || !anim_entry.contains("frames") || !anim_entry["frames"].is_array()) {
        return;
    }
    auto& frames = anim_entry["frames"];
    if (frame_idx < 0 || frame_idx >= static_cast<int>(frames.size())) {
        return;
    }
    auto& entry = frames[frame_idx];
    if (!entry.is_object()) {
        entry = ordered_json::object();
    }

    if (!FrameEntryNeedsRebuild(entry)) {
        entry["needs_rebuild"] = false;
        entry.erase("variants");
        return;
    }

    if (!cleared_mask.has_value()) {
        entry["needs_rebuild"] = false;
        entry.erase("variants");
        return;
    }

    const std::uint8_t clear_mask = static_cast<std::uint8_t>(*cleared_mask & kVariantAll);
    if (clear_mask == 0u) {
        return;
    }

    const std::optional<std::uint8_t> current_mask_opt = ParseVariantMask(entry);
    const std::uint8_t current_mask = current_mask_opt.value_or(kVariantAll);
    const std::uint8_t remaining = static_cast<std::uint8_t>(current_mask & ~clear_mask);

    if (remaining == 0u) {
        entry["needs_rebuild"] = false;
        entry.erase("variants");
        return;
    }

    entry["needs_rebuild"] = true;
    if (remaining == kVariantAll) {
        entry.erase("variants");
    } else {
        entry["variants"] = VariantArrayForMask(remaining);
    }
}

} // namespace imgcache
