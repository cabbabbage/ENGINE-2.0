// set_rebuild_cli.cpp
//
// CLI tool to mark frames for rebuild in cache/rebuild_queue.json
// Replaces set_rebuild_values.py
// Supports modes: all, asset, animation, frame

#include "asset_metadata.hpp"
#include "image_cache_generator.hpp"
#include "rebuild_queue.hpp"

#include <iostream>
#include <string>
#include <cstring>
#include <filesystem>
#include <optional>

#include <nlohmann/json.hpp>

namespace fs = std::filesystem;
using json = nlohmann::ordered_json;
using namespace imgcache;

// Mark all frames for rebuild
void mark_all_for_rebuild(json& queue,
                          const fs::path& manifest_dir,
                          const fs::path& repo_root,
                          const fs::path& cache_root) {
    const fs::path assets_root = repo_root / "resources" / "assets";
    const auto assets = DiscoverAssetNames(assets_root);
    for (const auto& asset_name : assets) {
        AssetRecord asset = BuildAssetRecord(manifest_dir, repo_root, cache_root, asset_name);
        for (const auto& anim_name : asset.anim_names) {
            ordered_json anim_meta = ordered_json::object();
            const ordered_json* anims = AnimationsObject(asset.meta);
            if (anims && anims->contains(anim_name) && (*anims)[anim_name].is_object()) {
                anim_meta = (*anims)[anim_name];
            }
            const fs::path anim_dir = ResolveAnimDir(asset.source_dir, anim_name, anim_meta, asset.discovered_anims);
            const auto frames = ImageCacheGenerator::EnumerateSourceFrames(anim_dir);
            if (frames.empty()) {
                continue;
            }
            ordered_json* anim_entry = FindAnimEntry(queue, asset_name, anim_name, true);
            if (!anim_entry) {
                continue;
            }
            MarkAllFrames(*anim_entry, static_cast<int>(frames.size()), true);
        }
    }
}

// Mark entire asset for rebuild
void mark_asset_for_rebuild(json& queue,
                            const fs::path& manifest_dir,
                            const fs::path& repo_root,
                            const fs::path& cache_root,
                            const std::string& target_asset) {
    AssetRecord asset = BuildAssetRecord(manifest_dir, repo_root, cache_root, target_asset);
    for (const auto& anim_name : asset.anim_names) {
        ordered_json anim_meta = ordered_json::object();
        const ordered_json* anims = AnimationsObject(asset.meta);
        if (anims && anims->contains(anim_name) && (*anims)[anim_name].is_object()) {
            anim_meta = (*anims)[anim_name];
        }
        const fs::path anim_dir = ResolveAnimDir(asset.source_dir, anim_name, anim_meta, asset.discovered_anims);
        const auto frames = ImageCacheGenerator::EnumerateSourceFrames(anim_dir);
        if (frames.empty()) {
            continue;
        }
        ordered_json* anim_entry = FindAnimEntry(queue, target_asset, anim_name, true);
        if (!anim_entry) {
            continue;
        }
        MarkAllFrames(*anim_entry, static_cast<int>(frames.size()), true);
    }
}

// Mark specific animation for rebuild
void mark_animation_for_rebuild(json& queue,
                                const fs::path& manifest_dir,
                                const fs::path& repo_root,
                                const fs::path& cache_root,
                                const std::string& target_asset,
                                const std::string& target_anim) {
    AssetRecord asset = BuildAssetRecord(manifest_dir, repo_root, cache_root, target_asset);
    ordered_json anim_meta = ordered_json::object();
    const ordered_json* anims = AnimationsObject(asset.meta);
    if (anims && anims->contains(target_anim) && (*anims)[target_anim].is_object()) {
        anim_meta = (*anims)[target_anim];
    }
    const fs::path anim_dir = ResolveAnimDir(asset.source_dir, target_anim, anim_meta, asset.discovered_anims);
    const auto frames = ImageCacheGenerator::EnumerateSourceFrames(anim_dir);
    if (frames.empty()) {
        std::cerr << "Warning: animation not found or has no frames: " << target_asset << "/" << target_anim << "\n";
        return;
    }
    ordered_json* anim_entry = FindAnimEntry(queue, target_asset, target_anim, true);
    if (!anim_entry) {
        return;
    }
    MarkAllFrames(*anim_entry, static_cast<int>(frames.size()), true);
}

// Mark specific frame for rebuild
void mark_frame_for_rebuild(json& queue,
                            const fs::path& manifest_dir,
                            const fs::path& repo_root,
                            const fs::path& cache_root,
                            const std::string& target_asset,
                            const std::string& target_anim,
                            int frame_idx) {
    if (frame_idx < 0) {
        std::cerr << "Error: frame index must be non-negative\n";
        return;
    }
    AssetRecord asset = BuildAssetRecord(manifest_dir, repo_root, cache_root, target_asset);
    ordered_json anim_meta = ordered_json::object();
    const ordered_json* anims = AnimationsObject(asset.meta);
    if (anims && anims->contains(target_anim) && (*anims)[target_anim].is_object()) {
        anim_meta = (*anims)[target_anim];
    }
    const fs::path anim_dir = ResolveAnimDir(asset.source_dir, target_anim, anim_meta, asset.discovered_anims);
    const auto frames = ImageCacheGenerator::EnumerateSourceFrames(anim_dir);
    if (frames.empty()) {
        std::cerr << "Warning: animation not found or has no frames: " << target_asset << "/" << target_anim << "\n";
        return;
    }
    ordered_json* anim_entry = FindAnimEntry(queue, target_asset, target_anim, true);
    if (!anim_entry) {
        return;
    }
    EnsureFramesArray(*anim_entry, std::max(static_cast<int>(frames.size()), frame_idx + 1));
    SetFrameFlag(*anim_entry, frame_idx, true);
}

void print_usage(const char* prog_name) {
    std::cout << "Usage: " << prog_name << " <mode> [args...] [--manifest <path>] [--cache-root <path>]\n\n";
    std::cout << "MODES:\n";
    std::cout << "  all                              Mark all frames for rebuild\n";
    std::cout << "  asset <name>                     Mark all frames in an asset\n";
    std::cout << "  animation <asset> <animation>    Mark all frames in an animation\n";
    std::cout << "  frame <asset> <animation> <idx>  Mark a specific frame\n\n";
    std::cout << "OPTIONS:\n";
    std::cout << "  --manifest <path>                Path to manifest.json (default: ../manifest.json)\n";
    std::cout << "  --cache-root <path>              Path to cache root (default: <repo>/cache)\n\n";
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
    fs::path cache_root_override;

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

    // Parse optional --manifest/--cache-root arguments
    for (int i = arg_idx; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--manifest") {
            if (i + 1 >= argc) {
                std::cerr << "Error: --manifest requires a path argument\n";
                return 2;
            }
            manifest_path = argv[++i];
        } else if (arg == "--cache-root") {
            if (i + 1 >= argc) {
                std::cerr << "Error: --cache-root requires a path argument\n";
                return 2;
            }
            cache_root_override = argv[++i];
        }
    }

    fs::path repo_root;
    fs::path manifest_dir;
    auto resolved = ResolveManifestOrExplicit(manifest_path);
    if (resolved) {
        manifest_path = *resolved;
        manifest_dir = manifest_path.parent_path();
        repo_root = manifest_dir;
        std::cout << "Using manifest: " << manifest_path.string() << "\n";
    } else if (cache_root_override.empty()) {
        std::cerr << "Error: could not locate manifest.json (searched upward from CWD). Use --manifest <path> or --cache-root.\n";
        return 3;
    }

    if (repo_root.empty() && !cache_root_override.empty()) {
        repo_root = cache_root_override.parent_path();
        manifest_dir = repo_root;
    }

    imgcache::GeneratorOptions generator_opts;
    if (!manifest_path.empty()) {
        generator_opts.manifest_path = manifest_path;
    }

    const fs::path cache_root = cache_root_override.empty()
                                    ? imgcache::ImageCacheGenerator::ResolveCacheRoot(repo_root, generator_opts)
                                    : cache_root_override;

    const fs::path rebuild_queue_path = ResolveRebuildQueuePath(cache_root);
    json rebuild_queue = LoadRebuildQueue(rebuild_queue_path);

    // Apply rebuild marking based on mode
    if (mode == "all") {
        std::cout << "Marking all frames for rebuild...\n";
        mark_all_for_rebuild(rebuild_queue, manifest_dir, repo_root, cache_root);
    }
    else if (mode == "asset") {
        std::cout << "Marking asset '" << asset_name << "' for rebuild...\n";
        mark_asset_for_rebuild(rebuild_queue, manifest_dir, repo_root, cache_root, asset_name);
    }
    else if (mode == "animation") {
        std::cout << "Marking animation '" << asset_name << "/" << animation_name << "' for rebuild...\n";
        mark_animation_for_rebuild(rebuild_queue, manifest_dir, repo_root, cache_root, asset_name, animation_name);
    }
    else if (mode == "frame") {
        std::cout << "Marking frame " << asset_name << "/" << animation_name << "/" << frame_idx << " for rebuild...\n";
        mark_frame_for_rebuild(rebuild_queue, manifest_dir, repo_root, cache_root, asset_name, animation_name, frame_idx);
    }

    if (!SaveRebuildQueue(rebuild_queue_path, rebuild_queue)) {
        std::cerr << "Error: failed to write rebuild queue: " << rebuild_queue_path.string() << "\n";
        return 3;
    }

    std::cout << "Rebuild queue updated successfully.\n";
    return 0;
}
