// set_rebuild_cli.cpp
//
// CLI tool to mark frames for rebuild in manifest.json
// Replaces set_rebuild_values.py
// Supports modes: all, asset, animation, frame

#include "cache_helper.hpp"
#include "image_cache_generator.hpp"

#include <iostream>
#include <string>
#include <cstring>
#include <filesystem>
#include <optional>

#include <nlohmann/json.hpp>

namespace fs = std::filesystem;
using json = nlohmann::ordered_json;

// Helper to navigate animations (handles both nested and flat structures)
json* find_animation(json& manifest, const std::string& asset_name, const std::string& anim_name) {
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

    // Check for nested structure: animations["animations"][anim_name]
    if (animations.contains("animations") && animations["animations"].is_object()) {
        auto& nested = animations["animations"];
        if (nested.contains(anim_name)) {
            return &nested[anim_name];
        }
    }

    // Check for flat structure: animations[anim_name]
    if (animations.contains(anim_name)) {
        return &animations[anim_name];
    }

    return nullptr;
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

// Iterate all animations and apply callback
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

// Mark all frames for rebuild
void mark_all_for_rebuild(json& manifest) {
    for_each_animation(manifest, [](const std::string& asset_name, const std::string& anim_name, json& anim) {
        int count = get_frame_count(anim);
        normalize_frames(anim, count);

        if (anim.contains("frames") && anim["frames"].is_array()) {
            for (auto& frame : anim["frames"]) {
                if (frame.is_object()) {
                    frame["needs_rebuild"] = true;
                }
            }
        }
    });
}

// Mark entire asset for rebuild
void mark_asset_for_rebuild(json& manifest, const std::string& target_asset) {
    for_each_animation(manifest, [&target_asset](const std::string& asset_name, const std::string& anim_name, json& anim) {
        if (asset_name == target_asset) {
            int count = get_frame_count(anim);
            normalize_frames(anim, count);

            if (anim.contains("frames") && anim["frames"].is_array()) {
                for (auto& frame : anim["frames"]) {
                    if (frame.is_object()) {
                        frame["needs_rebuild"] = true;
                    }
                }
            }
        }
    });
}

// Mark specific animation for rebuild
void mark_animation_for_rebuild(json& manifest, const std::string& target_asset, const std::string& target_anim) {
    json* anim = find_animation(manifest, target_asset, target_anim);
    if (!anim || !anim->is_object()) {
        std::cerr << "Warning: animation not found: " << target_asset << "/" << target_anim << "\n";
        return;
    }

    int count = get_frame_count(*anim);
    normalize_frames(*anim, count);

    if (anim->contains("frames") && (*anim)["frames"].is_array()) {
        for (auto& frame : (*anim)["frames"]) {
            if (frame.is_object()) {
                frame["needs_rebuild"] = true;
            }
        }
    }
}

// Mark specific frame for rebuild
void mark_frame_for_rebuild(json& manifest, const std::string& target_asset, const std::string& target_anim, int frame_idx) {
    if (frame_idx < 0) {
        std::cerr << "Error: frame index must be non-negative\n";
        return;
    }

    json* anim = find_animation(manifest, target_asset, target_anim);
    if (!anim || !anim->is_object()) {
        std::cerr << "Warning: animation not found: " << target_asset << "/" << target_anim << "\n";
        return;
    }

    int count = get_frame_count(*anim);
    // Ensure frames array is large enough to hold the target frame
    normalize_frames(*anim, std::max(count, frame_idx + 1));

    if (anim->contains("frames") && (*anim)["frames"].is_array()) {
        auto& frames = (*anim)["frames"];
        if (frame_idx < static_cast<int>(frames.size())) {
            if (frames[frame_idx].is_object()) {
                frames[frame_idx]["needs_rebuild"] = true;
            }
        }
    }
}

static std::optional<fs::path> resolve_manifest(const fs::path& explicit_path) {
    if (!explicit_path.empty()) return explicit_path;

    imgcache::GeneratorOptions opts;
    auto discovered = imgcache::ImageCacheGenerator::ResolveManifestPath(opts);
    if (discovered) return *discovered;
    return std::nullopt;
}

void print_usage(const char* prog_name) {
    std::cout << "Usage: " << prog_name << " <mode> [args...] [--manifest <path>]\n\n";
    std::cout << "MODES:\n";
    std::cout << "  all                              Mark all frames for rebuild\n";
    std::cout << "  asset <name>                     Mark all frames in an asset\n";
    std::cout << "  animation <asset> <animation>    Mark all frames in an animation\n";
    std::cout << "  frame <asset> <animation> <idx>  Mark a specific frame\n\n";
    std::cout << "OPTIONS:\n";
    std::cout << "  --manifest <path>                Path to manifest.json (default: ../manifest.json)\n\n";
    std::cout << "Examples:\n";
    std::cout << "  " << prog_name << " all\n";
    std::cout << "  " << prog_name << " asset player\n";
    std::cout << "  " << prog_name << " animation player idle\n";
    std::cout << "  " << prog_name << " frame player idle 5\n";
}

int main(int argc, char** argv) {
    if (argc < 2) {
        print_usage(argv[0]);
        return 2;
    }

    std::string mode = argv[1];
    std::string asset_name;
    std::string animation_name;
    int frame_idx = -1;
    fs::path manifest_path;

    // Parse arguments
    int arg_idx = 2;
    if (mode == "asset") {
        if (argc < 3) {
            std::cerr << "Error: 'asset' mode requires asset name\n";
            return 2;
        }
        asset_name = argv[arg_idx++];
    }
    else if (mode == "animation") {
        if (argc < 4) {
            std::cerr << "Error: 'animation' mode requires asset and animation names\n";
            return 2;
        }
        asset_name = argv[arg_idx++];
        animation_name = argv[arg_idx++];
    }
    else if (mode == "frame") {
        if (argc < 5) {
            std::cerr << "Error: 'frame' mode requires asset, animation, and frame index\n";
            return 2;
        }
        asset_name = argv[arg_idx++];
        animation_name = argv[arg_idx++];
        try {
            frame_idx = std::stoi(argv[arg_idx++]);
        }
        catch (const std::exception& e) {
            std::cerr << "Error: invalid frame index: " << e.what() << "\n";
            return 2;
        }
    }
    else if (mode != "all") {
        std::cerr << "Error: unknown mode '" << mode << "'\n";
        print_usage(argv[0]);
        return 2;
    }

    // Parse optional --manifest argument
    for (int i = arg_idx; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--manifest") {
            if (i + 1 >= argc) {
                std::cerr << "Error: --manifest requires a path argument\n";
                return 2;
            }
            manifest_path = argv[++i];
        }
    }

    // Default manifest path if not specified
    auto resolved = resolve_manifest(manifest_path);
    if (!resolved) {
        std::cerr << "Error: could not locate manifest.json (searched upward from CWD). Use --manifest <path>.\n";
        return 3;
    }
    manifest_path = *resolved;

    // Load manifest
    std::cout << "Loading manifest: " << manifest_path.string() << "\n";
    auto load_result = imgcache::CacheHelper::LoadJsonFile(manifest_path.string());
    if (!load_result.ok) {
        std::cerr << "Error: failed to load manifest: " << load_result.error << "\n";
        return 3;
    }

    json manifest = load_result.value;

    // Apply rebuild marking based on mode
    if (mode == "all") {
        std::cout << "Marking all frames for rebuild...\n";
        mark_all_for_rebuild(manifest);
    }
    else if (mode == "asset") {
        std::cout << "Marking asset '" << asset_name << "' for rebuild...\n";
        mark_asset_for_rebuild(manifest, asset_name);
    }
    else if (mode == "animation") {
        std::cout << "Marking animation '" << asset_name << "/" << animation_name << "' for rebuild...\n";
        mark_animation_for_rebuild(manifest, asset_name, animation_name);
    }
    else if (mode == "frame") {
        std::cout << "Marking frame " << asset_name << "/" << animation_name << "/" << frame_idx << " for rebuild...\n";
        mark_frame_for_rebuild(manifest, asset_name, animation_name, frame_idx);
    }

    // Write manifest back
    auto write_result = imgcache::CacheHelper::WriteJsonFile(manifest_path.string(), manifest);
    if (!write_result.ok) {
        std::cerr << "Error: failed to write manifest: " << write_result.error << "\n";
        return 3;
    }

    std::cout << "Manifest updated successfully.\n";
    return 0;
}
