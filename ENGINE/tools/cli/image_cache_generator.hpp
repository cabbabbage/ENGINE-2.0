#pragma once

// image_cache_generator.hpp
//
// Standalone OFFLINE image generation + cache writer.
// Generates one canonical alpha-cropped PNG per animation source frame.
//
// New cache output layout (single texture per frame, no scale variants):
//   <cache_root>/<asset>/animations/<anim>/<frame_index>.png
//
// The old multi-variant scale_* folder layout has been removed.
// GPU mipmaps and hardware scaling handle runtime size changes.

#include <cstdint>
#include <filesystem>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "cache_helper.hpp"

namespace imgcache {

namespace fs = std::filesystem;

// -----------------------------
// Core enums and simple structs
// -----------------------------
inline constexpr std::uint8_t kTextureLayerMaskNone = 0u;
inline constexpr std::uint8_t kTextureLayerMaskBase = 1u << 0;
inline constexpr std::uint8_t kTextureLayerMaskAll = kTextureLayerMaskBase;

struct ImageRGBA {
    int w = 0;
    int h = 0;
    // RGBA8 row-major, size = w*h*4
    std::vector<std::uint8_t> pixels;

    bool valid() const { return w > 0 && h > 0 && (int)pixels.size() == w * h * 4; }
};

// Logger interface so the tool can do per-task logging without being tied to iostreams.
struct ILogger {
    virtual ~ILogger() = default;
    virtual void info(const std::string& msg) = 0;
    virtual void warn(const std::string& msg) = 0;
    virtual void error(const std::string& msg) = 0;
};

// -----------------------------
// Output path builder (single canonical frame per animation)
// -----------------------------
struct CachePaths final {
    static constexpr const char* kCacheDirName = "cache";
    static constexpr const char* kAnimationsDirName = "animations";

    // <cache_root>/<asset>/animations/<anim>
    static inline fs::path anim_root(const fs::path& cache_root,
                                     const std::string& asset_name,
                                     const std::string& anim_name) {
        return cache_root / asset_name / kAnimationsDirName / anim_name;
    }

    // <cache_root>/<asset>/animations/<anim>/<frame_index>.png
    static inline fs::path frame_png_path(const fs::path& cache_root,
                                          const std::string& asset_name,
                                          const std::string& anim_name,
                                          int out_index) {
        return anim_root(cache_root, asset_name, anim_name) / (std::to_string(out_index) + ".png");
    }
};

// -----------------------------
// Filters and options
// -----------------------------
struct GeneratorFilters {
    // Empty means "no filter"
    std::unordered_set<std::string> assets;
    std::unordered_set<std::string> animations;

    // If non-empty: only rebuild these source frame indices.
    std::set<int> source_frames;

    bool matches_asset(const std::string& a) const {
        return assets.empty() || assets.count(a) > 0;
    }
    bool matches_anim(const std::string& a) const {
        return animations.empty() || animations.count(a) > 0;
    }
    bool matches_source_frame(int idx) const {
        return source_frames.empty() || source_frames.count(idx) > 0;
    }
};

struct GeneratorOptions {
    // If empty: generator searches upward from current working dir for "manifest.json"
    fs::path manifest_path;

    // If empty: cache_root = <repo_root>/cache
    fs::path cache_root_override;

    bool force_rebuild = false;
    bool missing_only = false;
    bool dry_run = false;

    struct AnimationRebuildRequest {
        std::string asset_name;
        std::string animation_name;
        std::uint8_t all_frames_layer_mask = kTextureLayerMaskNone;
        std::unordered_map<int, std::uint8_t> frame_layer_masks;
    };
    std::vector<AnimationRebuildRequest> explicit_rebuild_requests;

    // If 0: generator chooses (hardware_concurrency - 1, minimum 1)
    std::uint32_t worker_count_override = 0;

    // Reduce per-task logging noise/IO overhead by default.
    bool quiet_task_logs = true;

    GeneratorFilters filters;

    bool has_explicit_rebuild_requests() const { return !explicit_rebuild_requests.empty(); }
};

// -----------------------------
// Internal planning structures
// -----------------------------
struct AnimPayload {
    std::string asset_name;
    std::string anim_name;

    fs::path src_anim_dir;
    std::vector<fs::path> src_frames;

};

struct WorkItem {
    std::string asset_name;
    std::string anim_name;

    int src_frame_index = 0;
    std::vector<int> out_indices; // all output indices that map to this source index

    fs::path src_png_path;

    fs::path out_dir;
    bool write_frame = true;
};

// -----------------------------
// Result model
// -----------------------------
struct GenStats {
    std::uint64_t tasks_total = 0;
    std::uint64_t tasks_succeeded = 0;
    std::uint64_t tasks_failed = 0;

    std::uint64_t pngs_written = 0;
    std::uint64_t pngs_skipped_existing = 0;

    std::uint64_t animations_touched = 0;
    std::uint64_t assets_touched = 0;
};

struct GenResult {
    bool ok = false;
    std::string error;

    GenStats stats;
    std::vector<std::string> touched_assets;
    std::vector<std::string> touched_animations;
    std::vector<fs::path> written_files;
};


// -----------------------------
// Cache manifest model (single texture per frame)
// -----------------------------
struct CacheManifestSourceFrame {
    std::string filename;
    int order = 0;
    std::string hash;
    int width = 0;
    int height = 0;
};

struct CacheManifestAnimation {
    std::string name;
    std::vector<CacheManifestSourceFrame> source_frames;
};

struct CacheManifestCropCanvas {
    int shared_width = 0;
    int shared_height = 0;
};

struct CacheManifest {
    int schema_version = 2;  // bumped from 1: removed legacy_variant_profiles, camera_inputs, camera_derived
    std::string generator_version;
    std::string asset_name;
    std::string digest;
    float authored_scale_percentage = 100.0f;
    CacheManifestCropCanvas crop_canvas;
    std::vector<CacheManifestAnimation> animations;
    // legacy_variant_profiles, camera_inputs, camera_derived removed
};

// -----------------------------
// Main generator class
// -----------------------------
class ImageCacheGenerator final {
public:
    // Entry point.
    // Generates one cache PNG per source animation frame, cropped to shared canvas.
    static GenResult Run(const GeneratorOptions& opt, ILogger& log);

    // -------------------------
    // Path resolution
    // -------------------------
    static std::optional<fs::path> FindRepoRootFrom(const fs::path& start_dir);
    static std::optional<fs::path> ResolveManifestPath(const GeneratorOptions& opt);
    static fs::path ResolveCacheRoot(const fs::path& repo_root, const GeneratorOptions& opt);

    static fs::path ResolveAssetSourceDir(const fs::path& manifest_dir,
                                          const fs::path& repo_root,
                                          const std::string& asset_name,
                                          const nlohmann::json& asset_obj);

    static std::vector<std::pair<std::string, fs::path>> DiscoverAnimations(const fs::path& asset_src_dir);

    static std::vector<fs::path> EnumerateSourceFrames(const fs::path& anim_src_dir);

    static fs::path CacheManifestPath(const fs::path& cache_root, const std::string& asset_name);
    static nlohmann::json CacheManifestToJson(const CacheManifest& manifest, bool include_digest = true);
    static std::optional<CacheManifest> CacheManifestFromJson(const nlohmann::json& json, std::string& err);
    static std::string GenerateManifestDigest(const CacheManifest& manifest);
    static bool ReadCacheManifest(const fs::path& path, CacheManifest& manifest, std::string& err);
    static bool WriteCacheManifest(const fs::path& path, const CacheManifest& manifest, std::string& err);
    static bool CacheManifestsExactlyMatch(const CacheManifest& existing,
                                           const CacheManifest& current,
                                           std::string& reason);

    // Check if a cached frame is missing
    static bool OutputMissingAnyFrame(const fs::path& cache_root,
                                      const std::string& asset_name,
                                      const std::string& anim_name,
                                      int out_index);

    // -------------------------
    // Image IO and transforms
    // -------------------------
    static std::optional<ImageRGBA> LoadPngRGBA(const fs::path& path, std::string& err);
    static bool SavePngRGBA(const fs::path& path, const ImageRGBA& img, std::string& err);

    static std::optional<ImageRGBA> ResizeRGBA(const ImageRGBA& src, int dst_w, int dst_h, std::string& err);

    // Delete old multi-variant scale_* cache directories for an asset
    static void DeleteOldMultiVariantCache(const fs::path& cache_root, const std::string& asset_name, ILogger& log);

private:
    ImageCacheGenerator() = delete;
};

// Convenience: use an explicit manifest path when provided, otherwise discover it by walking upward.
inline std::optional<fs::path> ResolveManifestOrExplicit(const fs::path& explicit_path) {
    if (!explicit_path.empty()) {
        return explicit_path;
    }
    GeneratorOptions opts;
    return ImageCacheGenerator::ResolveManifestPath(opts);
}

} // namespace imgcache