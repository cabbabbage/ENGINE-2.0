// cache_validator_cli.cpp
//
// CLI tool to validate cache integrity and mark missing frames for rebuild
// Replaces cache_validator.py
// Checks that all expected cache files exist for each animation

#include "image_cache_generator.hpp"
#include "cache_helper.hpp"

#include <iostream>
#include <string>
#include <filesystem>

#include <nlohmann/json.hpp>

namespace fs = std::filesystem;
using json = nlohmann::ordered_json;

constexpr int ANIMATION_SCALE_PCTS[] = {75, 50, 25, 10};
constexpr const char* VARIANTS[] = {"normal", "foreground", "background"};

// Helper to navigate animations (handles both nested and flat structures)
json* find_animations_object(json& manifest, const std::string& asset_name) {
    if (!manifest.contains("assets") || !manifest["assets"].is_object()) {
        return nullptr;
    }

    auto& assets = manifest["assets"];
    if (!assets.contains(asset_name) || !assets[asset_name].is_object()) {
        return nullptr;
    }

    auto& asset = assets[asset_name];
    if (!asset.contains("animations") || !asset["animations"].is_object()) {
        return nullptr;
    }

    auto& animations = asset["animations"];

    // Check for nested structure: animations["animations"]
    if (animations.contains("animations") && animations["animations"].is_object()) {
        return &animations["animations"];
    }

    // Flat structure: animations itself
    return &animations;
}

// Normalize frames array to match frame_count
void normalize_frames(json& anim, int frame_count) {
    if (!anim.is_object()) {
        return;
    }

    if (frame_count < 0) {
        frame_count = 0;
    }

    json frames;
    if (anim.contains("frames") && anim["frames"].is_array()) {
        frames = anim["frames"];
    } else {
        frames = json::array();
    }

    // Extend frames array if needed
    while (static_cast<int>(frames.size()) < frame_count) {
        json frame_obj = json::object();
        frame_obj["needs_rebuild"] = false;
        frames.push_back(frame_obj);
    }

    // Truncate if too many
    if (static_cast<int>(frames.size()) > frame_count) {
        json trimmed = json::array();
        const int limit = std::max(0, frame_count);
        for (int i = 0; i < limit && i < static_cast<int>(frames.size()); ++i) {
            trimmed.push_back(frames[i]);
        }
        frames = std::move(trimmed);
    }

    anim["frames"] = frames;
}

// Get frame count from animation metadata
int get_frame_count(const json& anim) {
    if (anim.is_object() && anim.contains("number_of_frames") && anim["number_of_frames"].is_number_integer()) {
        return std::max(0, anim["number_of_frames"].get<int>());
    }
    return 0;
}

// Get speed multiplier from animation metadata
float get_speed_multiplier(const json& anim) {
    if (!anim.is_object()) {
        return 1.0f;
    }

    // Check speed_multiplier first, then speed_factor
    if (anim.contains("speed_multiplier") && anim["speed_multiplier"].is_number()) {
        return anim["speed_multiplier"].get<float>();
    }
    if (anim.contains("speed_factor") && anim["speed_factor"].is_number()) {
        return anim["speed_factor"].get<float>();
    }

    return 1.0f;
}

// Check if all cache files exist for a given output frame
bool animation_output_exists(const fs::path& anim_cache_root, int output_idx) {
    for (int scale_pct : ANIMATION_SCALE_PCTS) {
        fs::path scale_dir = anim_cache_root / ("scale_" + std::to_string(scale_pct));
        if (!fs::is_directory(scale_dir)) {
            return false;
        }

        for (const char* variant : VARIANTS) {
            fs::path frame_path = scale_dir / variant / (std::to_string(output_idx) + ".png");
            if (!fs::is_regular_file(frame_path)) {
                return false;
            }
        }
    }
    return true;
}

// Mark all frames as needing rebuild
bool mark_all_frames_missing(json& frames) {
    bool changed = false;
    if (!frames.is_array()) {
        return false;
    }

    for (auto& frame : frames) {
        if (frame.is_object()) {
            // Only mark if not already marked
            bool already_marked = frame.contains("needs_rebuild") &&
                                 frame["needs_rebuild"].is_boolean() &&
                                 frame["needs_rebuild"].get<bool>();
            if (!already_marked) {
                frame["needs_rebuild"] = true;
                changed = true;
            }
        }
    }
    return changed;
}

// Validate cache for a single animation
bool validate_animation_cache(
    const std::string& asset_name,
    const std::string& anim_name,
    json& anim_meta,
    const fs::path& cache_root
) {
    // Get frame count
    int frame_count = get_frame_count(anim_meta);
    normalize_frames(anim_meta, frame_count);

    if (frame_count <= 0) {
        return false;
    }

    if (!anim_meta.contains("frames") || !anim_meta["frames"].is_array()) {
        return false;
    }

    auto& frames = anim_meta["frames"];

    // Check if animation cache directory exists
    fs::path anim_cache_root = cache_root / asset_name / "animations" / anim_name;
    if (!fs::is_directory(anim_cache_root)) {
        return mark_all_frames_missing(frames);
    }

    // Get speed multiplier and build output sequence
    float speed_multiplier = get_speed_multiplier(anim_meta);
    std::vector<int> frame_sequence = imgcache::ImageCacheGenerator::BuildSpeedFrameSequence(frame_count, speed_multiplier);

    if (frame_sequence.empty()) {
        return false;
    }

    // Check each output frame
    bool changed = false;
    for (size_t output_idx = 0; output_idx < frame_sequence.size(); ++output_idx) {
        int source_idx = frame_sequence[output_idx];

        if (source_idx < 0 || source_idx >= static_cast<int>(frames.size())) {
            continue;
        }

        auto& frame_entry = frames[source_idx];
        if (!frame_entry.is_object()) {
            continue;
        }

        // Skip if already marked for rebuild
        bool already_marked = frame_entry.contains("needs_rebuild") &&
                             frame_entry["needs_rebuild"].is_boolean() &&
                             frame_entry["needs_rebuild"].get<bool>();
        if (already_marked) {
            continue;
        }

        // Check if output exists
        if (!animation_output_exists(anim_cache_root, static_cast<int>(output_idx))) {
            frame_entry["needs_rebuild"] = true;
            changed = true;
        }
    }

    return changed;
}

// Iterate all animations and validate
template<typename Func>
void for_each_animation(json& manifest, Func callback) {
    if (!manifest.contains("assets") || !manifest["assets"].is_object()) {
        return;
    }

    auto& assets = manifest["assets"];
    for (auto& [asset_name, asset_meta] : assets.items()) {
        if (!asset_meta.is_object() || !asset_meta.contains("animations")) {
            continue;
        }

        auto& animations = asset_meta["animations"];
        if (!animations.is_object()) {
            continue;
        }

        // Handle nested structure: animations["animations"]
        json* anims_obj = &animations;
        if (animations.contains("animations") && animations["animations"].is_object()) {
            anims_obj = &animations["animations"];
        }

        for (auto& [anim_name, anim_meta] : anims_obj->items()) {
            if (anim_meta.is_object()) {
                callback(asset_name, anim_name, anim_meta);
            }
        }
    }
}

void print_usage(const char* prog_name) {
    std::cout << "Usage: " << prog_name << " [--manifest <path>]\n\n";
    std::cout << "OPTIONS:\n";
    std::cout << "  --manifest <path>    Path to manifest.json (default: ../manifest.json)\n\n";
    std::cout << "Validates cache integrity and marks frames with missing cache files for rebuild.\n";
}

int main(int argc, char** argv) {
    fs::path manifest_path;

    // Parse arguments
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--manifest") {
            if (i + 1 >= argc) {
                std::cerr << "Error: --manifest requires a path argument\n";
                return 2;
            }
            manifest_path = argv[++i];
        }
        else if (arg == "--help" || arg == "-h") {
            print_usage(argv[0]);
            return 0;
        }
        else {
            std::cerr << "Error: unknown argument: " << arg << "\n";
            print_usage(argv[0]);
            return 2;
        }
    }

    // Default manifest path if not specified
    if (manifest_path.empty()) {
        manifest_path = fs::path(argv[0]).parent_path().parent_path() / "manifest.json";
    }

    // Load manifest
    std::cout << "Loading manifest: " << manifest_path.string() << "\n";
    auto load_result = imgcache::CacheHelper::LoadJsonFile(manifest_path.string());
    if (!load_result.ok) {
        std::cerr << "Error: failed to load manifest: " << load_result.error << "\n";
        return 3;
    }

    json manifest = load_result.value;

    // Determine cache root
    fs::path cache_root = manifest_path.parent_path() / "cache";
    std::cout << "Cache root: " << cache_root.string() << "\n";

    // Validate cache
    bool changed = false;
    for_each_animation(manifest, [&](const std::string& asset_name, const std::string& anim_name, json& anim_meta) {
        if (validate_animation_cache(asset_name, anim_name, anim_meta, cache_root)) {
            changed = true;
        }
    });

    // Write manifest if changed
    if (changed) {
        std::cout << "Detected missing cache files; marking entries for rebuild.\n";
        auto write_result = imgcache::CacheHelper::WriteJsonFile(manifest_path.string(), manifest);
        if (!write_result.ok) {
            std::cerr << "Error: failed to write manifest: " << write_result.error << "\n";
            return 3;
        }
        std::cout << "Manifest updated successfully.\n";
    }
    else {
        std::cout << "Cache validation passed; no missing files detected.\n";
    }

    return 0;
}
