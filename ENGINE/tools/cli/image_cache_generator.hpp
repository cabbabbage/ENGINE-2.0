#pragma once

// image_cache_generator.hpp
//
// Standalone OFFLINE image generation + cache writer that mirrors the Python image pipeline output layout.
// This is NOT an engine renderer. It is a build-time cache generator.
//
// This header defines the full public surface for the generator and includes the critical
// path and naming helpers that guarantee the same directory structure and filenames.
//
// Expected cache output layout (must match Python exactly):
//   <cache_root>/<asset>/animations/<anim>/scale_<pct>/<variant>/<out_index>.png
// Where:
//   cache_root default: <repo_root>/cache
//   variant in: normal
//   pct is one of 100, 90, 80, 70, 60, 50, 40, 30, 20, 10
//   out_index is integer frame index of the source sequence
//
// This tool will live next to the existing Python scripts, so it must follow the same
// manifest discovery and default path resolution rules (repo root search for manifest.json).

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
enum class Variant : std::uint8_t {
    Normal = 0
};

inline constexpr std::uint8_t kTextureVariantMaskNone = 0u;
inline constexpr std::uint8_t kTextureVariantMaskNormal = 1u << 0;
inline constexpr std::uint8_t kTextureVariantMaskAll = kTextureVariantMaskNormal;

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
// Output path builder (must match Python exactly)
// -----------------------------
struct CachePaths final {
    static constexpr const char* kCacheDirName = "cache";
    static constexpr const char* kAnimationsDirName = "animations";
    static constexpr const char* kNormalDirName = "normal";

    static inline std::string scale_dir_name(int pct) {
        return "scale_" + std::to_string(pct);
    }

    static inline const char* variant_dir_name(Variant v) {
        switch (v) {
            case Variant::Normal: return kNormalDirName;
        }
        return kNormalDirName;
    }

    // <cache_root>/<asset>/animations/<anim>
    static inline fs::path anim_root(const fs::path& cache_root,
                                    const std::string& asset_name,
                                    const std::string& anim_name) {
        return cache_root / asset_name / kAnimationsDirName / anim_name;
    }

    // <cache_root>/<asset>/animations/<anim>/scale_<pct>/<variant>
    static inline fs::path variant_dir(const fs::path& cache_root,
                                       const std::string& asset_name,
                                       const std::string& anim_name,
                                       int scale_pct,
                                       Variant variant) {
        return anim_root(cache_root, asset_name, anim_name)
             / scale_dir_name(scale_pct)
             / variant_dir_name(variant);
    }

    // <cache_root>/<asset>/animations/<anim>/scale_<pct>/<variant>/<out_index>.png
    static inline fs::path frame_png_path(const fs::path& cache_root,
                                          const std::string& asset_name,
                                          const std::string& anim_name,
                                          int scale_pct,
                                          Variant variant,
                                          int out_index) {
        return variant_dir(cache_root, asset_name, anim_name, scale_pct, variant)
             / (std::to_string(out_index) + ".png");
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
        std::uint8_t all_frames_variant_mask = kTextureVariantMaskNone;
        std::unordered_map<int, std::uint8_t> frame_variant_masks;
    };
    std::vector<AnimationRebuildRequest> explicit_rebuild_requests;

    // Legacy compatibility option. Runtime generation enforces canonical
    // scales: 100, 90, 80, 70, 60, 50, 40, 30, 20, 10.
    std::vector<int> scale_percents;

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

    int scale_pct = 100;
    float scale_factor = 1.0f;

    fs::path src_png_path;

    fs::path out_normal_dir;
    bool write_normal = true;
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
// Main generator class
// -----------------------------
class ImageCacheGenerator final {
public:
    // Entry point.
    // Runtime behavior:
    // - explicit_rebuild_requests: rebuild only requested animations/frames/variants
    // - missing_only: rebuild only missing output files in selected scope
    // - no queue persistence and no metadata-hash stale checks
    static GenResult Run(const GeneratorOptions& opt, ILogger& log);

    // -------------------------
    // Path resolution (Python parity)
    // -------------------------
    static std::optional<fs::path> FindRepoRootFrom(const fs::path& start_dir);
    static std::optional<fs::path> ResolveManifestPath(const GeneratorOptions& opt);
    static fs::path ResolveCacheRoot(const fs::path& repo_root, const GeneratorOptions& opt);

    // Resolve asset source directory:
    // default: <repo_root>/resources/assets/<asset_name>
    // override: manifest asset_directory (relative to manifest dir unless absolute)
    static fs::path ResolveAssetSourceDir(const fs::path& manifest_dir,
                                          const fs::path& repo_root,
                                          const std::string& asset_name,
                                          const nlohmann::json& asset_obj);

    // Animation discovery:
    // if asset dir has subdirs: each subdir is an animation name
    // else: single animation named "default"
    static std::vector<std::pair<std::string, fs::path>> DiscoverAnimations(const fs::path& asset_src_dir);

    // Source frames in animation dir:
    // Only numeric enumeration 0.png.. until first missing.
    static std::vector<fs::path> EnumerateSourceFrames(const fs::path& anim_src_dir);

    // Decide whether output should be built:
    // - if force_rebuild -> true
    // - else if any expected output file is missing -> true
    // - else -> false
    static bool OutputMissingAnyVariant(const fs::path& cache_root,
                                        const std::string& asset_name,
                                        const std::string& anim_name,
                                        int scale_pct,
                                        int out_index);

    // -------------------------
    // Image IO and transforms (implemented in .cpp)
    // -------------------------
    static std::optional<ImageRGBA> LoadPngRGBA(const fs::path& path, std::string& err);
    static bool SavePngRGBA(const fs::path& path, const ImageRGBA& img, std::string& err);

    static std::optional<ImageRGBA> ResizeRGBA(const ImageRGBA& src, int dst_w, int dst_h, std::string& err);

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
