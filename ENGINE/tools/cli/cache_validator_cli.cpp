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
#include <optional>
#include <system_error>
#include <cmath>
#include <algorithm>

#include <nlohmann/json.hpp>

namespace fs = std::filesystem;
using json = nlohmann::ordered_json;

constexpr int ANIMATION_SCALE_PCTS[] = {75, 50, 25, 10};
constexpr const char* VARIANTS[] = {"normal", "foreground", "background"};

static std::optional<fs::path> resolve_manifest(const fs::path& explicit_path) {
    if (!explicit_path.empty()) return explicit_path;

    imgcache::GeneratorOptions opts;
    auto discovered = imgcache::ImageCacheGenerator::ResolveManifestPath(opts);
    if (discovered) return *discovered;
    return std::nullopt;
}

static inline fs::file_time_type safe_last_write_time(const fs::path& p) {
    std::error_code ec;
    auto t = fs::last_write_time(p, ec);
    if (ec) return fs::file_time_type::min();
    return t;
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

static inline int manifest_frame_count(const json& anim) {
    if (anim.is_object() && anim.contains("frames") && anim["frames"].is_array()) {
        return static_cast<int>(anim["frames"].size());
    }
    if (anim.is_object() && anim.contains("number_of_frames") && anim["number_of_frames"].is_number_integer()) {
        return std::max(0, anim["number_of_frames"].get<int>());
    }
    return 0;
}

static inline float normalize_speed_multiplier(float raw) {
    static constexpr float kSpeedMultipliers[] = {0.25f, 0.5f, 1.0f, 2.0f, 4.0f};
    if (!std::isfinite(raw) || raw <= 0.0f) return 1.0f;
    float best = kSpeedMultipliers[0];
    float best_diff = std::fabs(best - raw);
    for (float c : kSpeedMultipliers) {
        float d = std::fabs(c - raw);
        if (d < best_diff) {
            best_diff = d;
            best = c;
        }
    }
    return best;
}

static inline float get_speed_multiplier(const json& anim) {
    if (!anim.is_object()) return 1.0f;
    float raw = 1.0f;
    if (anim.contains("speed_multiplier") && anim["speed_multiplier"].is_number()) {
        raw = anim["speed_multiplier"].get<float>();
    } else if (anim.contains("speed_factor") && anim["speed_factor"].is_number()) {
        raw = anim["speed_factor"].get<float>();
    }
    return normalize_speed_multiplier(raw);
}

static std::vector<fs::path> enumerate_source_frames(const fs::path& anim_dir) {
    std::vector<fs::path> frames;
    int idx = 0;
    for (;;) {
        fs::path candidate = anim_dir / (std::to_string(idx) + ".png");
        std::error_code ec;
        if (!fs::exists(candidate, ec) || ec) break;
        frames.push_back(candidate);
        ++idx;
    }
    return frames;
}

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
    const json& asset_meta,
    const fs::path& cache_root,
    const fs::path& manifest_dir,
    fs::file_time_type manifest_mtime
) {
    int frame_count = manifest_frame_count(anim_meta);

    fs::path asset_src_dir = imgcache::ImageCacheGenerator::ResolveAssetSourceDir(
        manifest_dir, manifest_dir, asset_name, nlohmann::json(asset_meta));

    // Discover animation directory matching python rules
    fs::path anim_src_dir = asset_src_dir;
    auto discovered = imgcache::ImageCacheGenerator::DiscoverAnimations(asset_src_dir);
    for (const auto& pair : discovered) {
        if (pair.first == anim_name) {
            anim_src_dir = pair.second;
            break;
        }
    }
    if (anim_name != "default" && anim_src_dir == asset_src_dir) {
        fs::path candidate = asset_src_dir / anim_name;
        if (fs::exists(candidate)) anim_src_dir = candidate;
    }

    std::vector<fs::path> source_frames = enumerate_source_frames(anim_src_dir);
    if (!source_frames.empty()) {
        frame_count = std::max(frame_count, static_cast<int>(source_frames.size()));
    }

    normalize_frames(anim_meta, frame_count);
    if (!anim_meta.contains("frames") || !anim_meta["frames"].is_array() || frame_count <= 0) {
        return false;
    }

    auto& frames = anim_meta["frames"];

    fs::path anim_cache_root = cache_root / asset_name / "animations" / anim_name;
    if (!fs::is_directory(anim_cache_root)) {
        return mark_all_frames_missing(frames);
    }

    float speed_multiplier = get_speed_multiplier(anim_meta);
    std::vector<int> frame_sequence = imgcache::ImageCacheGenerator::BuildSpeedFrameSequence(frame_count, speed_multiplier);
    if (frame_sequence.empty()) return false;

    constexpr auto kTimestampSlack = std::chrono::seconds(2);

    bool changed = false;
    for (size_t output_idx = 0; output_idx < frame_sequence.size(); ++output_idx) {
        int source_idx = frame_sequence[output_idx];
        if (source_idx < 0 || source_idx >= static_cast<int>(frames.size())) continue;

        auto& frame_entry = frames[source_idx];
        if (!frame_entry.is_object()) frame_entry = json::object();

        bool already_marked = frame_entry.contains("needs_rebuild") &&
                              frame_entry["needs_rebuild"].is_boolean() &&
                              frame_entry["needs_rebuild"].get<bool>();
        if (already_marked) continue;

        fs::file_time_type baseline = manifest_mtime;
        if (source_idx >= 0 && source_idx < static_cast<int>(source_frames.size())) {
            baseline = std::max(baseline, safe_last_write_time(source_frames[source_idx]));
        }

        if (animation_output_missing_or_stale(anim_cache_root,
                                              static_cast<int>(output_idx),
                                              baseline,
                                              kTimestampSlack)) {
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
                callback(asset_name, asset_meta, anim_name, anim_meta);
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
    auto resolved = resolve_manifest(manifest_path);
    if (!resolved) {
        std::cerr << "Error: could not locate manifest.json (searched upward from CWD). Use --manifest <path>.\n";
        return 3;
    }
    manifest_path = *resolved;
    fs::path manifest_dir = manifest_path.parent_path();
    fs::file_time_type manifest_mtime = safe_last_write_time(manifest_path);

    imgcache::GeneratorOptions generator_opts;
    generator_opts.manifest_path = manifest_path;
    fs::path cache_root = imgcache::ImageCacheGenerator::ResolveCacheRoot(manifest_dir, generator_opts);

    // Load manifest
    std::cout << "Loading manifest: " << manifest_path.string() << "\n";
    auto load_result = imgcache::CacheHelper::LoadJsonFile(manifest_path.string());
    if (!load_result.ok) {
        std::cerr << "Error: failed to load manifest: " << load_result.error << "\n";
        return 3;
    }

    json manifest = load_result.value;

    std::cout << "Cache root: " << cache_root.string() << "\n";

    // Validate cache
    bool changed = false;
    for_each_animation(manifest, [&](const std::string& asset_name,
                                     const json& asset_meta,
                                     const std::string& anim_name,
                                     json& anim_meta) {
        if (validate_animation_cache(asset_name, anim_name, anim_meta, asset_meta, cache_root, manifest_dir, manifest_mtime)) {
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
