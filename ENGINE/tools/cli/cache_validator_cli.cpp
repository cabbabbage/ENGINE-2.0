// cache_validator_cli.cpp
//
// CLI tool to validate cache integrity and mark missing frames for rebuild queue
// Replaces cache_validator.py
// Checks that all expected cache files exist for each animation

#include "asset_metadata.hpp"
#include "image_cache_generator.hpp"
#include "rebuild_queue.hpp"

#include <iostream>
#include <fstream>
#include <string>
#include <filesystem>
#include <optional>
#include <system_error>
#include <cmath>
#include <algorithm>
#include <numeric>

#include <nlohmann/json.hpp>

namespace fs = std::filesystem;
using json = nlohmann::ordered_json;
using namespace imgcache;

constexpr int ANIMATION_SCALE_PCTS[] = {75, 50, 25, 10};
constexpr const char* VARIANTS[] = {"normal", "foreground", "background"};

static inline fs::file_time_type safe_last_write_time(const fs::path& p) {
    std::error_code ec;
    auto t = fs::last_write_time(p, ec);
    if (ec) return fs::file_time_type::min();
    return t;
}

static inline double parse_float_like(const json& v, double fallback) {
    if (v.is_number()) return v.get<double>();
    if (v.is_string()) {
        try {
            return std::stod(v.get<std::string>());
        } catch (...) {
            return fallback;
        }
    }
    return fallback;
}

static json build_effects_snippet(const json& manifest) {
    json snippet = json::object();
    auto block_to_dict = [&](const char* name) {
        json out = json::object();
        if (manifest.is_object() && manifest.contains("image_effects") && manifest["image_effects"].is_object()) {
            const json& ie = manifest["image_effects"];
            if (ie.contains(name) && ie[name].is_object()) {
                out = ie[name];
            }
        }
        const char* keys[] = {
            "brightness", "contrast", "blur",
            "saturation_red", "saturation_green", "saturation_blue",
            "hue"
        };
        for (const char* k : keys) {
            if (!out.contains(k)) {
                out[k] = 0.0;
            } else {
                out[k] = parse_float_like(out[k], 0.0);
            }
        }
        return out;
    };

    snippet["foreground"] = block_to_dict("foreground");
    snippet["background"] = block_to_dict("background");
    return snippet;
}

static std::optional<json> read_json_file(const fs::path& path) {
    std::ifstream in(path);
    if (!in.is_open()) return std::nullopt;
    json data;
    try {
        in >> data;
    } catch (...) {
        return std::nullopt;
    }
    return data;
}

static inline bool file_missing_or_older(const fs::path& p,
                                         fs::file_time_type baseline,
                                         std::chrono::nanoseconds slack = std::chrono::nanoseconds{0}) {
    std::error_code ec;
    if (!fs::exists(p, ec) || ec) return true;
    auto t = fs::last_write_time(p, ec);
    if (ec) return true;
    if (baseline != fs::file_time_type::min()) {
        // Allow a small slack so freshly written cache files (which are created
        // just before manifest.json is rewritten) are not treated as stale.
        if (slack.count() > 0) {
            auto slack_ft = std::chrono::duration_cast<fs::file_time_type::duration>(slack);
            // Avoid underflow if slack exceeds epoch distance
            if (slack_ft < baseline.time_since_epoch()) {
                baseline -= slack_ft;
            } else {
                baseline = fs::file_time_type::min();
            }
        }
        if (t < baseline) return true;
    }
    uintmax_t sz = fs::file_size(p, ec);
    if (ec) return true;
    return sz < 32; // heuristically treat tiny files as invalid
}

static inline int source_frame_count(const std::vector<fs::path>& frames) {
    return static_cast<int>(frames.size());
}

// Check if any expected output is missing or older than baseline
bool animation_output_missing_or_stale(const fs::path& anim_cache_root,
                                       int output_idx,
                                       fs::file_time_type baseline_time,
                                       std::chrono::nanoseconds slack) {
    for (int scale_pct : ANIMATION_SCALE_PCTS) {
        fs::path scale_dir = anim_cache_root / ("scale_" + std::to_string(scale_pct));
        for (const char* variant : VARIANTS) {
            fs::path frame_path = scale_dir / variant / (std::to_string(output_idx) + ".png");
            if (file_missing_or_older(frame_path, baseline_time, slack)) {
                return true;
            }
        }
    }
    return false;
}

// Mark all frames for rebuild in queue
bool mark_all_frames(json& queue, const std::string& asset_name, const std::string& anim_name, int frame_count) {
    json* anim_entry = FindAnimEntry(queue, asset_name, anim_name, true);
    if (!anim_entry) {
        return false;
    }
    MarkAllFrames(*anim_entry, frame_count, true);
    return true;
}

void print_usage(const char* prog_name) {
    std::cout << "Usage: " << prog_name << " [--manifest <path>] [--cache-root <path>]\n\n";
    std::cout << "OPTIONS:\n";
    std::cout << "  --manifest <path>    Path to manifest.json (default: ../manifest.json)\n";
    std::cout << "  --cache-root <path>  Path to cache root (default: <repo>/cache)\n\n";
    std::cout << "Validates cache integrity and marks frames with missing cache files for rebuild.\n";
}

int main(int argc, char** argv) {
    fs::path manifest_path;
    fs::path cache_root_override;

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
        else if (arg == "--cache-root") {
            if (i + 1 >= argc) {
                std::cerr << "Error: --cache-root requires a path argument\n";
                return 2;
            }
            cache_root_override = argv[++i];
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

    fs::path cache_root = cache_root_override.empty()
                              ? imgcache::ImageCacheGenerator::ResolveCacheRoot(repo_root, generator_opts)
                              : cache_root_override;

    std::cout << "Cache root: " << cache_root.string() << "\n";

    const fs::path effects_cache_path = cache_root / "effects_cache.json";
    const fs::file_time_type effects_cache_mtime = safe_last_write_time(effects_cache_path);

    bool effects_mismatch = false;
    if (!manifest_path.empty()) {
        const auto manifest_json = read_json_file(manifest_path);
        const auto effects_cache_json = read_json_file(effects_cache_path);
        if (!manifest_json || !effects_cache_json) {
            effects_mismatch = true;
        } else {
            effects_mismatch = (build_effects_snippet(*manifest_json) != *effects_cache_json);
        }
    }

    const fs::path rebuild_queue_path = ResolveRebuildQueuePath(cache_root);
    json rebuild_queue = LoadRebuildQueue(rebuild_queue_path);
    bool changed = false;

    json metadata_cache = LoadAssetMetadataCache(cache_root);

    const fs::path assets_root = repo_root / "resources" / "assets";
    std::vector<std::string> asset_names = DiscoverAssetNames(assets_root);
    if (asset_names.empty() && rebuild_queue.contains("assets") && rebuild_queue["assets"].is_object()) {
        for (auto it = rebuild_queue["assets"].begin(); it != rebuild_queue["assets"].end(); ++it) {
            asset_names.push_back(it.key());
        }
    }

    for (const auto& asset_name : asset_names) {
        imgcache::AssetRecord asset = BuildAssetRecord(manifest_dir, repo_root, cache_root, asset_name);
        const json* anim_payloads = AnimationsObject(asset.meta);
        const std::string current_meta_hash = BuildImageMetadataHash(asset.meta);
        const std::string cached_meta_hash = CachedImageMetadataHash(metadata_cache, asset_name);
        const bool asset_meta_changed = !current_meta_hash.empty() && current_meta_hash != cached_meta_hash;

        for (const auto& anim_name : asset.anim_names) {
            json anim_meta = json::object();
            if (anim_payloads && anim_payloads->contains(anim_name) && (*anim_payloads)[anim_name].is_object()) {
                anim_meta = (*anim_payloads)[anim_name];
            }

            const fs::path anim_dir = ResolveAnimDir(asset.source_dir, anim_name, anim_meta, asset.discovered_anims);
            std::vector<fs::path> source_frames = imgcache::ImageCacheGenerator::EnumerateSourceFrames(anim_dir);
            const int frame_count = source_frame_count(source_frames);
            if (frame_count <= 0) {
                continue;
            }

            fs::path anim_cache_root = cache_root / asset_name / "animations" / anim_name;
            if (effects_mismatch || asset_meta_changed || !fs::is_directory(anim_cache_root)) {
                if (mark_all_frames(rebuild_queue, asset_name, anim_name, frame_count)) {
                    changed = true;
                }
                continue;
            }

            json* anim_entry = FindAnimEntry(rebuild_queue, asset_name, anim_name, true);
            if (!anim_entry) {
                continue;
            }
            EnsureFramesArray(*anim_entry, frame_count);
            auto& frames = (*anim_entry)["frames"];

            std::vector<int> frame_sequence(static_cast<size_t>(frame_count));
            std::iota(frame_sequence.begin(), frame_sequence.end(), 0);
            if (frame_sequence.empty()) {
                continue;
            }

            constexpr auto kTimestampSlack = std::chrono::seconds(2);
            for (size_t output_idx = 0; output_idx < frame_sequence.size(); ++output_idx) {
                const int source_idx = frame_sequence[output_idx];
                if (source_idx < 0 || source_idx >= frame_count) {
                    continue;
                }

                auto& frame_entry = frames[source_idx];
                bool already_marked = false;
                if (frame_entry.is_boolean()) {
                    already_marked = frame_entry.get<bool>();
                } else if (frame_entry.is_object() && frame_entry.contains("needs_rebuild") &&
                           frame_entry["needs_rebuild"].is_boolean()) {
                    already_marked = frame_entry["needs_rebuild"].get<bool>();
                }
                if (already_marked) {
                    continue;
                }

                fs::file_time_type baseline = effects_cache_mtime;
                if (source_idx >= 0 && source_idx < static_cast<int>(source_frames.size())) {
                    baseline = std::max(baseline, safe_last_write_time(source_frames[source_idx]));
                }

                if (animation_output_missing_or_stale(anim_cache_root,
                                                      static_cast<int>(output_idx),
                                                      baseline,
                                                      kTimestampSlack)) {
                    if (!frame_entry.is_object()) {
                        frame_entry = json::object();
                    }
                    frame_entry["needs_rebuild"] = true;
                    changed = true;
                }
            }
        }
    }

    if (changed) {
        std::cout << "Detected missing cache files; updating rebuild queue.\n";
        if (!SaveRebuildQueue(rebuild_queue_path, rebuild_queue)) {
            std::cerr << "Error: failed to write rebuild queue: " << rebuild_queue_path.string() << "\n";
            return 3;
        }
        std::cout << "Rebuild queue updated successfully.\n";
    } else {
        std::cout << "Cache validation passed; no missing files detected.\n";
    }

    return 0;
}
