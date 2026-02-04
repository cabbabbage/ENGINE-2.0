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
//   variant in: normal, foreground, background
//   pct is integer like 75, 50, 25, 10
//   out_index is integer frame index of the speed-expanded output sequence
//
// This tool will live next to the existing Python scripts, so it must follow the same
// manifest discovery and default path resolution rules (repo root search for manifest.json).

#include <cstdint>
#include <filesystem>
#include <optional>
#include <set>
#include <string>
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
    Normal = 0,
    Foreground,
    Background
};

struct CropBounds {
    // Mirrors Python compute_crop_bounds storage:
    // margins plus original dimensions.
    int left = 0;
    int top = 0;
    int right_margin = 0;
    int bottom_margin = 0;
    int src_w = 0;
    int src_h = 0;

    bool valid() const { return src_w > 0 && src_h > 0; }
};

struct FrameMeta {
    // Mirrors manifest frames[*].needs_rebuild
    bool needs_rebuild = false;
};

struct EffectsParams {
    // Mirrors effects.py / apply_color_effects.py semantics.
    // Keep stable and map directly from manifest fields.
    float hue_shift = 0.0f;
    float saturation = 0.0f;
    float brightness = 0.0f;
    float contrast = 0.0f;

    float blur_radius = 0.0f;
    float sharpen_amount = 0.0f;

    // Optional per-channel saturation if present in your Python config.
    float saturation_r = 0.0f;
    float saturation_g = 0.0f;
    float saturation_b = 0.0f;
};

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
    static constexpr const char* kForegroundDirName = "foreground";
    static constexpr const char* kBackgroundDirName = "background";

    static inline std::string scale_dir_name(int pct) {
        return "scale_" + std::to_string(pct);
    }

    static inline const char* variant_dir_name(Variant v) {
        switch (v) {
            case Variant::Normal: return kNormalDirName;
            case Variant::Foreground: return kForegroundDirName;
            case Variant::Background: return kBackgroundDirName;
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

    // If non-empty: only rebuild these source frame indices (pre speed expansion).
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
    bool dry_run = false;

    // Must remain true for safety unless you intentionally diverge from Python.
    bool clear_needs_rebuild_on_success_only = true;

    // Default scales (Python parity): 75, 50, 25, 10
    // If empty: use defaults
    std::vector<int> scale_percents;

    // If 0: generator chooses (hardware_concurrency - 1, minimum 1)
    std::uint32_t worker_count_override = 0;

    GeneratorFilters filters;
};

// -----------------------------
// Internal planning structures
// -----------------------------
struct AnimPayload {
    std::string asset_name;
    std::string anim_name;

    fs::path src_anim_dir;
    std::vector<fs::path> src_frames;

    std::vector<FrameMeta> frames_meta;

    bool crop_frames_enabled = false;
    std::optional<CropBounds> crop_bounds;

    float speed_multiplier = 1.0f;
    std::vector<int> out_to_src; // output index -> source index mapping

    EffectsParams fx_foreground;
    EffectsParams fx_background;
};

struct WorkItem {
    std::string asset_name;
    std::string anim_name;

    int src_frame_index = 0;
    std::vector<int> out_indices; // all output indices that map to this source index

    int scale_pct = 100;
    float scale_factor = 1.0f;

    std::optional<CropBounds> crop_bounds_scaled;

    fs::path src_png_path;

    fs::path out_normal_dir;
    fs::path out_foreground_dir;
    fs::path out_background_dir;

    EffectsParams fx_foreground;
    EffectsParams fx_background;
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

    bool rebuild_queue_written = false;
    GenStats stats;
};

// -----------------------------
// Main generator class
// -----------------------------
class ImageCacheGenerator final {
public:
    // Entry point.
    // Must reproduce Python behavior:
    // - Manifest parsing (for global effects) and asset iteration
    // - Animation discovery (subdirs vs default)
    // - Source frame enumeration: 0.png..N.png stopping at first missing
    // - Speed multiplier expansion mapping output frames to source frames
    // - Crop bounds union behavior when enabled
    // - Rebuild decision: needs_rebuild true OR output missing, plus force option
    // - Write output PNGs into the exact cache structure
    // - Clear rebuild queue flags only after full successful generation
    // - Never write the rebuild queue if any frame fails
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

    // -------------------------
    // Manifest parsing and planning
    // -------------------------
    // Ensure frames_meta is at least source_frame_count long (default needs_rebuild=false).
    static void EnsureFrameMetadata(std::vector<FrameMeta>& frames_meta, int source_frame_count);

    // Build output mapping for speed multiplier:
    // returns out_to_src where index is output frame index and value is source frame index.
    static std::vector<int> BuildSpeedFrameSequence(int source_frame_count, float speed_multiplier);

    // Crop bounds:
    // Compute union alpha bbox across all frames if enabled.
    // If any source frame size mismatch, return nullopt (skip cropping).
    static std::optional<CropBounds> ComputeCropBoundsForAnimation(const std::vector<fs::path>& src_frames,
                                                                  ILogger& log);

    // Scale bounds using Python rounding rules.
    static CropBounds ScaleCropBounds(const CropBounds& b, float scale_factor);

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
    static std::optional<ImageRGBA> ApplyCrop(const ImageRGBA& src, const CropBounds& b, std::string& err);

    // Normal variant is unchanged (just scaled/cropped).
    // Foreground/background apply effects.
    static std::optional<ImageRGBA> ApplyEffects(const ImageRGBA& src, const EffectsParams& fx, std::string& err);

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
