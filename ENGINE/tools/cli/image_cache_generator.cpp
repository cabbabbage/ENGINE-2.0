#include "image_cache_generator.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <limits>
#include <optional>
#include <string>
#include <unordered_set>
#include <sstream>
#include <vector>

#include "utils/stb_image.h"
#include "utils/stb_image_write.h"

namespace imgcache {

namespace {

struct AlphaBounds {
    int left = 0;
    int top = 0;
    int right = 0;  // exclusive
    int bottom = 0; // exclusive

    [[nodiscard]] int width() const {
        return std::max(0, right - left);
    }

    [[nodiscard]] int height() const {
        return std::max(0, bottom - top);
    }

    [[nodiscard]] bool valid() const {
        return right > left && bottom > top;
    }
};

struct SharedCropSize {
    int width = 0;
    int height = 0;

    [[nodiscard]] bool valid() const {
        return width > 0 && height > 0;
    }
};

std::vector<float> canonical_scale_steps() {
    return {1.0f, 0.9f, 0.8f, 0.7f, 0.6f, 0.5f, 0.4f, 0.3f, 0.2f, 0.1f};
}

bool is_png_file(const fs::path& path) {
    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return ext == ".png";
}

int scale_percent(float step) {
    return std::max(1, static_cast<int>(std::lround(static_cast<double>(step) * 100.0)));
}

int scale_dimension(int value, float step) {
    return std::max(1, static_cast<int>(std::lround(static_cast<double>(value) * static_cast<double>(step))));
}

bool has_normal_variant_mask(std::uint8_t mask) {
    return (mask & kTextureVariantMaskNormal) != 0;
}

std::vector<fs::path> EnumerateSourceFramesForAnalysis(const fs::path& anim_src_dir) {
    std::vector<fs::path> frames;
    std::error_code ec;
    for (int idx = 0; idx < 100000; ++idx) {
        fs::path frame_path = anim_src_dir / (std::to_string(idx) + ".png");
        if (!fs::exists(frame_path, ec) || ec) {
            break;
        }
        frames.push_back(frame_path);
    }
    return frames;
}

std::optional<ImageRGBA> LoadPngRGBAForAnalysis(const fs::path& path, std::string& err) {
    err.clear();
    int w = 0;
    int h = 0;
    int channels = 0;
    stbi_uc* pixels = stbi_load(path.string().c_str(), &w, &h, &channels, 4);
    if (!pixels || w <= 0 || h <= 0) {
        err = "stbi_load failed";
        if (pixels) {
            stbi_image_free(pixels);
        }
        return std::nullopt;
    }

    ImageRGBA image;
    image.w = w;
    image.h = h;
    image.pixels.assign(pixels, pixels + static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4u);
    stbi_image_free(pixels);
    return image;
}

std::optional<AlphaBounds> FindAlphaBounds(const ImageRGBA& image) {
    if (!image.valid()) {
        return std::nullopt;
    }

    int left = image.w;
    int top = image.h;
    int right = -1;
    int bottom = -1;

    for (int y = 0; y < image.h; ++y) {
        for (int x = 0; x < image.w; ++x) {
            const std::size_t idx = (static_cast<std::size_t>(y) * static_cast<std::size_t>(image.w) +
                                     static_cast<std::size_t>(x)) * 4u;
            const std::uint8_t alpha = image.pixels[idx + 3];
            if (alpha == 0) {
                continue;
            }

            left = std::min(left, x);
            top = std::min(top, y);
            right = std::max(right, x + 1);
            bottom = std::max(bottom, y + 1);
        }
    }

    if (right < 0 || bottom < 0) {
        return std::nullopt;
    }

    AlphaBounds bounds;
    bounds.left = left;
    bounds.top = top;
    bounds.right = right;
    bounds.bottom = bottom;
    return bounds;
}

std::optional<ImageRGBA> CropToSharedCanvas(const ImageRGBA& src,
                                            const std::optional<AlphaBounds>& bounds,
                                            int shared_width,
                                            int shared_height,
                                            std::string& err) {
    err.clear();

    if (!src.valid()) {
        err = "invalid source image";
        return std::nullopt;
    }
    if (shared_width <= 0 || shared_height <= 0) {
        err = "invalid shared crop size";
        return std::nullopt;
    }

    ImageRGBA out;
    out.w = shared_width;
    out.h = shared_height;
    out.pixels.assign(static_cast<std::size_t>(shared_width) * static_cast<std::size_t>(shared_height) * 4u, 0u);

    if (!bounds.has_value()) {
        return out;
    }

    const AlphaBounds b = bounds.value();
    if (!b.valid()) {
        return out;
    }
    if (b.left < 0 || b.top < 0 || b.right > src.w || b.bottom > src.h) {
        err = "alpha bounds exceed source image dimensions";
        return std::nullopt;
    }
    if (b.width() > shared_width || b.height() > shared_height) {
        err = "frame alpha bounds exceed shared crop canvas";
        return std::nullopt;
    }

    const int paste_x = (shared_width - b.width()) / 2;
    const int paste_y = shared_height - b.height();

    for (int y = 0; y < b.height(); ++y) {
        const int src_y = b.top + y;
        const int dst_y = paste_y + y;
        for (int x = 0; x < b.width(); ++x) {
            const int src_x = b.left + x;
            const int dst_x = paste_x + x;

            const std::size_t src_idx = (static_cast<std::size_t>(src_y) * static_cast<std::size_t>(src.w) +
                                         static_cast<std::size_t>(src_x)) * 4u;
            const std::size_t dst_idx = (static_cast<std::size_t>(dst_y) * static_cast<std::size_t>(out.w) +
                                         static_cast<std::size_t>(dst_x)) * 4u;

            out.pixels[dst_idx + 0] = src.pixels[src_idx + 0];
            out.pixels[dst_idx + 1] = src.pixels[src_idx + 1];
            out.pixels[dst_idx + 2] = src.pixels[src_idx + 2];
            out.pixels[dst_idx + 3] = src.pixels[src_idx + 3];
        }
    }

    return out;
}

bool AnimationRequestedForGeneration(const GeneratorOptions& opt,
                                     const std::string& asset_name,
                                     const std::string& /*animation_name*/) {
    // Smart texture-cache rebuilds are asset-wide: all animations and frames
    // must share one per-asset set of scale percentages. Asset filters still
    // narrow the selected assets, but animation/frame filters are intentionally
    // ignored once an asset is selected for generation.
    return opt.filters.matches_asset(asset_name);
}

std::optional<SharedCropSize> AnalyzeSharedCropSizeForAsset(
    const GeneratorOptions& opt,
    const std::string& asset_name,
    const std::vector<std::pair<std::string, fs::path>>& animations,
    std::string& err) {
    err.clear();

    SharedCropSize shared;
    bool found_frame = false;
    bool found_visible_pixels = false;

    for (const auto& animation_entry : animations) {
        const std::string& animation_name = animation_entry.first;
        const fs::path& animation_src_dir = animation_entry.second;

        if (!AnimationRequestedForGeneration(opt, asset_name, animation_name)) {
            continue;
        }

        const auto frames = EnumerateSourceFramesForAnalysis(animation_src_dir);
        for (const fs::path& frame_path : frames) {
            found_frame = true;

            std::string load_err;
            std::optional<ImageRGBA> src_opt = LoadPngRGBAForAnalysis(frame_path, load_err);
            if (!src_opt.has_value()) {
                err = "Failed to load source frame '" + frame_path.string() + "' while analyzing crop bounds: " + load_err;
                return std::nullopt;
            }

            const std::optional<AlphaBounds> bounds = FindAlphaBounds(src_opt.value());
            if (!bounds.has_value()) {
                continue;
            }

            found_visible_pixels = true;
            shared.width = std::max(shared.width, bounds->width());
            shared.height = std::max(shared.height, bounds->height());
        }
    }

    if (!found_frame) {
        err = "No source frames found while analyzing shared alpha crop bounds for asset '" + asset_name + "'";
        return std::nullopt;
    }

    if (!found_visible_pixels || !shared.valid()) {
        err = "No visible alpha pixels found while analyzing shared alpha crop bounds for asset '" + asset_name + "'";
        return std::nullopt;
    }

    return shared;
}

struct CameraCacheSettings {
    int min_height_px = 100;
    int max_height_px = 2000;
    float base_height_px = 1000.0f;
    float min_visible_screen_ratio = 0.003f;
    float boundary_min_visible_screen_ratio = 0.015f;
};

struct SmartVariantPlan {
    std::vector<int> percents;
    std::vector<float> steps;
    float authored_scale = 1.0f;
    float max_camera_scale = 1.0f;
    float min_camera_scale = 0.1f;
    int scale100_w = 1;
    int scale100_h = 1;
    int min_w = 1;
    int min_h = 1;
    float min_visible_ratio = 0.003f;
    bool boundary_ratio = false;
};

float read_float_json(const nlohmann::json& node, const char* key, float fallback) {
    auto it = node.find(key);
    if (it == node.end() || !it->is_number()) {
        return fallback;
    }
    const float value = static_cast<float>(it->get<double>());
    return std::isfinite(value) ? value : fallback;
}

int read_int_json(const nlohmann::json& node, const char* key, int fallback) {
    auto it = node.find(key);
    if (it == node.end() || !it->is_number()) {
        return fallback;
    }
    return std::max(1, static_cast<int>(std::lround(it->get<double>())));
}

CameraCacheSettings ResolveCameraCacheSettings(const nlohmann::json& manifest) {
    CameraCacheSettings settings;
    if (!manifest.contains("maps") || !manifest["maps"].is_object()) {
        return settings;
    }
    for (auto it = manifest["maps"].begin(); it != manifest["maps"].end(); ++it) {
        if (!it.value().is_object()) {
            continue;
        }
        auto camera_it = it.value().find("camera_settings");
        if (camera_it == it.value().end() || !camera_it->is_object()) {
            continue;
        }
        const nlohmann::json& camera = *camera_it;
        settings.min_height_px = read_int_json(camera, "camera_height_min_px", settings.min_height_px);
        settings.max_height_px = std::max(settings.min_height_px,
                                          read_int_json(camera, "camera_height_max_px", settings.max_height_px));
        settings.base_height_px = std::max(1.0f, read_float_json(camera, "base_height_px", settings.base_height_px));
        settings.min_visible_screen_ratio = std::clamp(read_float_json(camera,
                                                                       "min_visible_screen_ratio",
                                                                       settings.min_visible_screen_ratio),
                                                       0.0f,
                                                       0.5f);
        settings.boundary_min_visible_screen_ratio = std::clamp(read_float_json(camera,
                                                                                "boundary_min_visible_screen_ratio",
                                                                                settings.boundary_min_visible_screen_ratio),
                                                                0.0f,
                                                                0.5f);
        return settings;
    }
    return settings;
}

float ReadAuthoredScale(const nlohmann::json& asset_obj) {
    if (!asset_obj.is_object()) {
        return 1.0f;
    }
    auto ss_it = asset_obj.find("size_settings");
    if (ss_it == asset_obj.end() || !ss_it->is_object()) {
        return 1.0f;
    }
    const float percent = read_float_json(*ss_it, "scale_percentage", 100.0f);
    if (!std::isfinite(percent) || percent <= 0.0f) {
        return 1.0f;
    }
    return std::max(0.01f, percent * 0.01f);
}

bool IsBoundaryAsset(const nlohmann::json& asset_obj) {
    if (!asset_obj.is_object()) {
        return false;
    }
    auto type_it = asset_obj.find("asset_type");
    if (type_it == asset_obj.end() || !type_it->is_string()) {
        type_it = asset_obj.find("type");
    }
    if (type_it == asset_obj.end() || !type_it->is_string()) {
        return false;
    }
    std::string type = type_it->get<std::string>();
    std::transform(type.begin(), type.end(), type.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return type == "boundary";
}

std::vector<int> BuildVariantPercents(float min_step) {
    min_step = std::clamp(min_step, 0.01f, 0.91f);
    const int min_pct = std::clamp(static_cast<int>(std::ceil(static_cast<double>(min_step) * 100.0)), 1, 91);
    std::vector<int> percents;
    percents.reserve(10);
    for (int idx = 0; idx < 10; ++idx) {
        const double t = static_cast<double>(idx) / 9.0;
        int pct = static_cast<int>(std::lround(100.0 + (static_cast<double>(min_pct) - 100.0) * t));
        if (!percents.empty()) {
            pct = std::min(pct, percents.back() - 1);
        }
        pct = std::max(min_pct, pct);
        percents.push_back(pct);
    }
    percents.front() = 100;
    percents.back() = min_pct;
    for (std::size_t idx = 1; idx < percents.size(); ++idx) {
        if (percents[idx] >= percents[idx - 1]) {
            percents[idx] = percents[idx - 1] - 1;
        }
    }
    percents.back() = std::min(percents.back(), percents[percents.size() - 2] - 1);
    percents.back() = std::max(1, percents.back());
    return percents;
}

SmartVariantPlan BuildSmartVariantPlan(const nlohmann::json& asset_obj,
                                       const CameraCacheSettings& camera,
                                       const SharedCropSize& shared_crop) {
    SmartVariantPlan plan;
    plan.authored_scale = ReadAuthoredScale(asset_obj);
    plan.boundary_ratio = IsBoundaryAsset(asset_obj);
    plan.min_visible_ratio = plan.boundary_ratio
        ? camera.boundary_min_visible_screen_ratio
        : camera.min_visible_screen_ratio;

    plan.max_camera_scale = std::max(0.01f, camera.base_height_px / static_cast<float>(std::max(1, camera.min_height_px)));
    plan.min_camera_scale = std::max(0.01f, camera.base_height_px / static_cast<float>(std::max(1, camera.max_height_px)));

    const float scale100_factor = std::max(0.01f, plan.authored_scale * plan.max_camera_scale);
    plan.scale100_w = scale_dimension(shared_crop.width, scale100_factor);
    plan.scale100_h = scale_dimension(shared_crop.height, scale100_factor);

    const float horizon_factor = std::max(0.01f, plan.authored_scale * plan.min_camera_scale);
    const int horizon_w = scale_dimension(shared_crop.width, horizon_factor);
    const int horizon_h = scale_dimension(shared_crop.height, horizon_factor);
    const float min_visible_px = std::max(1.0f,
        std::clamp(plan.min_visible_ratio, 0.0f, 0.5f) * camera.base_height_px * 1.1f);
    const float aspect = static_cast<float>(std::max(1, shared_crop.width)) /
                         static_cast<float>(std::max(1, shared_crop.height));
    const int configured_min_h = std::max(1, static_cast<int>(std::lround(min_visible_px)));
    const int configured_min_w = std::max(1, static_cast<int>(std::lround(min_visible_px * aspect)));
    if (std::max(horizon_w, horizon_h) >= static_cast<int>(std::lround(min_visible_px))) {
        plan.min_w = horizon_w;
        plan.min_h = horizon_h;
    } else {
        plan.min_w = configured_min_w;
        plan.min_h = configured_min_h;
    }

    const float min_step_w = static_cast<float>(plan.min_w) / static_cast<float>(std::max(1, plan.scale100_w));
    const float min_step_h = static_cast<float>(plan.min_h) / static_cast<float>(std::max(1, plan.scale100_h));
    const float min_step = std::clamp(std::max(min_step_w, min_step_h), 0.01f, 0.91f);
    plan.percents = BuildVariantPercents(min_step);
    plan.steps.reserve(plan.percents.size());
    for (int pct : plan.percents) {
        plan.steps.push_back(static_cast<float>(pct) * 0.01f);
    }
    return plan;
}

std::string FormatFloat(float value) {
    std::ostringstream out;
    out << std::fixed << std::setprecision(3) << value;
    return out.str();
}

void LogSmartVariantPlan(ILogger& log,
                         const std::string& asset_name,
                         const ImageRGBA& source_image,
                         const SmartVariantPlan& plan,
                         const CameraCacheSettings& camera) {
    log.info("[ImageCacheGenerator] Asset: " + asset_name +
             "\nSource Size: " + std::to_string(source_image.w) + "x" + std::to_string(source_image.h) +
             "\nAuthored Scale %: " + FormatFloat(plan.authored_scale * 100.0f) +
             "\nCamera Min Height: " + std::to_string(camera.min_height_px) +
             "\nCamera Max Height: " + std::to_string(camera.max_height_px) +
             "\nCamera Min Zoom: " + FormatFloat(plan.max_camera_scale) +
             "\nCamera Max Zoom: " + FormatFloat(plan.min_camera_scale) +
             "\nCalculated scale_100 Size: " + std::to_string(plan.scale100_w) + "x" + std::to_string(plan.scale100_h) +
             "\nMinimum Visible Size: " + std::to_string(plan.min_w) + "x" + std::to_string(plan.min_h) +
             "\nMin Visible Ratio Source: " + std::string(plan.boundary_ratio ? "boundary" : "normal"));
    for (std::size_t idx = 0; idx < plan.percents.size(); ++idx) {
        const int out_w = scale_dimension(plan.scale100_w, plan.steps[idx]);
        const int out_h = scale_dimension(plan.scale100_h, plan.steps[idx]);
        log.info("Variant " + std::to_string(idx + 1) + "/" + std::to_string(plan.percents.size()) +
                 " -> scale_" + std::to_string(plan.percents[idx]) +
                 " -> " + std::to_string(out_w) + "x" + std::to_string(out_h));
    }
}

bool AssetHasScalingProfile(const nlohmann::json& asset_obj) {
    auto it = asset_obj.find("scaling_profile");
    return it != asset_obj.end() && it->is_object();
}

bool AssetCacheMissingAnySmartVariant(const fs::path& cache_root,
                                      const std::string& asset_name,
                                      const std::vector<std::pair<std::string, fs::path>>& animations,
                                      const SmartVariantPlan& plan) {
    std::error_code ec;
    if (!fs::exists(cache_root / asset_name, ec) || ec) {
        return true;
    }
    for (const auto& animation_entry : animations) {
        const auto frames = ImageCacheGenerator::EnumerateSourceFrames(animation_entry.second);
        for (std::size_t frame_index = 0; frame_index < frames.size(); ++frame_index) {
            for (int pct : plan.percents) {
                const fs::path out_path = CachePaths::frame_png_path(cache_root,
                                                                     asset_name,
                                                                     animation_entry.first,
                                                                     pct,
                                                                     Variant::Normal,
                                                                     static_cast<int>(frame_index));
                if (!fs::exists(out_path, ec) || ec) {
                    return true;
                }
            }
        }
    }
    return false;
}

fs::path BuildTempCacheRoot(const fs::path& cache_root, const std::string& asset_name) {
    const auto ticks = std::chrono::steady_clock::now().time_since_epoch().count();
    return cache_root / (".tmp_" + asset_name + "_" + std::to_string(ticks));
}

bool ReplaceAssetCacheAtomically(const fs::path& cache_root,
                                 const fs::path& temp_root,
                                 const std::string& asset_name,
                                 std::string& err) {
    err.clear();
    std::error_code ec;
    const fs::path temp_asset = temp_root / asset_name;
    const fs::path final_asset = cache_root / asset_name;
    if (!fs::exists(temp_asset, ec) || ec) {
        err = "temporary asset cache was not generated: " + temp_asset.string();
        return false;
    }
    fs::create_directories(cache_root, ec);
    if (ec) {
        err = "failed creating cache root: " + cache_root.string();
        return false;
    }
    fs::remove_all(final_asset, ec);
    if (ec) {
        err = "failed removing old asset cache: " + final_asset.string() + ": " + ec.message();
        return false;
    }
    ec.clear();
    fs::rename(temp_asset, final_asset, ec);
    if (ec) {
        err = "failed moving rebuilt asset cache into place: " + final_asset.string() + ": " + ec.message();
        return false;
    }
    fs::remove_all(temp_root, ec);
    return true;
}

} // namespace

GenResult ImageCacheGenerator::Run(const GeneratorOptions& opt, ILogger& log) {
    GenResult result{};

    const auto manifest_path_opt = ResolveManifestPath(opt);
    if (!manifest_path_opt.has_value()) {
        result.error = "Unable to locate manifest.json";
        return result;
    }
    const fs::path manifest_path = manifest_path_opt.value();

    std::ifstream in(manifest_path);
    if (!in.is_open()) {
        result.error = "Failed to open manifest: " + manifest_path.string();
        return result;
    }

    nlohmann::json manifest;
    try {
        in >> manifest;
    } catch (const std::exception& e) {
        result.error = std::string("Failed to parse manifest: ") + e.what();
        return result;
    }

    if (!manifest.is_object() || !manifest.contains("assets") || !manifest["assets"].is_object()) {
        result.error = "Manifest does not contain an object 'assets' section";
        return result;
    }

    const fs::path repo_root = manifest_path.parent_path();
    const fs::path manifest_dir = manifest_path.parent_path();
    const fs::path cache_root = ResolveCacheRoot(repo_root, opt);
    const CameraCacheSettings camera_settings = ResolveCameraCacheSettings(manifest);

    bool manifest_modified = false;
    auto& assets = manifest["assets"];
    for (auto it = assets.begin(); it != assets.end(); ++it) {
        const std::string asset_name = it.key();
        if (!opt.filters.matches_asset(asset_name)) {
            continue;
        }
        if (!it.value().is_object()) {
            continue;
        }
        nlohmann::json& asset_obj = it.value();

        const fs::path asset_src_dir = ResolveAssetSourceDir(manifest_dir, repo_root, asset_name, asset_obj);
        const auto animations = DiscoverAnimations(asset_src_dir);
        if (animations.empty()) {
            continue;
        }

        std::string crop_err;
        std::optional<SharedCropSize> shared_crop_size = AnalyzeSharedCropSizeForAsset(opt,
                                                                                       asset_name,
                                                                                       animations,
                                                                                       crop_err);
        if (!shared_crop_size.has_value()) {
            result.error = crop_err;
            return result;
        }

        fs::path first_frame_path;
        for (const auto& animation_entry : animations) {
            if (!AnimationRequestedForGeneration(opt, asset_name, animation_entry.first)) {
                continue;
            }
            const auto frames = EnumerateSourceFrames(animation_entry.second);
            if (!frames.empty()) {
                first_frame_path = frames.front();
                break;
            }
        }
        if (first_frame_path.empty()) {
            continue;
        }
        std::string load_err;
        std::optional<ImageRGBA> first_source = LoadPngRGBA(first_frame_path, load_err);
        if (!first_source.has_value()) {
            result.error = "Failed to load source frame '" + first_frame_path.string() + "': " + load_err;
            return result;
        }

        const SmartVariantPlan plan = BuildSmartVariantPlan(asset_obj, camera_settings, *shared_crop_size);
        const bool should_generate = opt.force_rebuild ||
                                     !AssetHasScalingProfile(asset_obj) ||
                                     ReadAuthoredScale(asset_obj) != 1.0f ||
                                     AssetCacheMissingAnySmartVariant(cache_root, asset_name, animations, plan);
        if (!should_generate) {
            continue;
        }

        LogSmartVariantPlan(log, asset_name, first_source.value(), plan, camera_settings);

        const fs::path write_cache_root = opt.dry_run ? cache_root : BuildTempCacheRoot(cache_root, asset_name);
        if (!opt.dry_run) {
            std::error_code ec;
            fs::remove_all(write_cache_root, ec);
            ec.clear();
            fs::create_directories(write_cache_root, ec);
            if (ec) {
                result.error = "Failed creating temporary cache root: " + write_cache_root.string();
                return result;
            }
        }

        bool touched_asset = false;
        for (const auto& animation_entry : animations) {
            const std::string& animation_name = animation_entry.first;
            const fs::path& animation_src_dir = animation_entry.second;
            if (!AnimationRequestedForGeneration(opt, asset_name, animation_name)) {
                continue;
            }

            const auto frames = EnumerateSourceFrames(animation_src_dir);
            if (frames.empty()) {
                continue;
            }

            bool touched_animation = false;
            for (std::size_t frame_index = 0; frame_index < frames.size(); ++frame_index) {
                const int frame_idx = static_cast<int>(frame_index);
                std::optional<ImageRGBA> src_opt = LoadPngRGBA(frames[frame_index], load_err);
                if (!src_opt.has_value()) {
                    result.error = "Failed to load source frame '" + frames[frame_index].string() + "': " + load_err;
                    if (!opt.dry_run) {
                        std::error_code ignored;
                        fs::remove_all(write_cache_root, ignored);
                    }
                    return result;
                }
                const ImageRGBA& src = src_opt.value();

                for (std::size_t step_index = 0; step_index < plan.steps.size(); ++step_index) {
                    const float step = plan.steps[step_index];
                    const int pct = plan.percents[step_index];
                    const fs::path out_path = CachePaths::frame_png_path(write_cache_root,
                                                                         asset_name,
                                                                         animation_name,
                                                                         pct,
                                                                         Variant::Normal,
                                                                         frame_idx);

                    ++result.stats.tasks_total;
                    touched_animation = true;
                    touched_asset = true;

                    if (opt.dry_run) {
                        ++result.stats.tasks_succeeded;
                        continue;
                    }

                    const float resize_factor = std::max(0.01f, plan.authored_scale * plan.max_camera_scale * step);
                    const int dst_w = scale_dimension(src.w, resize_factor);
                    const int dst_h = scale_dimension(src.h, resize_factor);
                    std::optional<ImageRGBA> scaled = ResizeRGBA(src, dst_w, dst_h, load_err);
                    if (!scaled.has_value()) {
                        ++result.stats.tasks_failed;
                        result.error = "Failed to resize frame '" + frames[frame_index].string() + "': " + load_err;
                        std::error_code ignored;
                        fs::remove_all(write_cache_root, ignored);
                        return result;
                    }

                    const std::optional<AlphaBounds> scaled_bounds = FindAlphaBounds(scaled.value());
                    int shared_w = scale_dimension(shared_crop_size->width, resize_factor);
                    int shared_h = scale_dimension(shared_crop_size->height, resize_factor);
                    if (scaled_bounds.has_value()) {
                        shared_w = std::max(shared_w, scaled_bounds->width());
                        shared_h = std::max(shared_h, scaled_bounds->height());
                    }

                    std::optional<ImageRGBA> cropped = CropToSharedCanvas(scaled.value(),
                                                                          scaled_bounds,
                                                                          shared_w,
                                                                          shared_h,
                                                                          load_err);
                    if (!cropped.has_value()) {
                        ++result.stats.tasks_failed;
                        result.error = "Failed to alpha crop frame '" + frames[frame_index].string() + "': " + load_err;
                        std::error_code ignored;
                        fs::remove_all(write_cache_root, ignored);
                        return result;
                    }

                    std::error_code ec;
                    fs::create_directories(out_path.parent_path(), ec);
                    if (ec) {
                        ++result.stats.tasks_failed;
                        result.error = "Failed creating cache directory: " + out_path.parent_path().string();
                        fs::remove_all(write_cache_root, ec);
                        return result;
                    }

                    std::string save_err;
                    if (!SavePngRGBA(out_path, cropped.value(), save_err)) {
                        ++result.stats.tasks_failed;
                        result.error = "Failed writing frame '" + out_path.string() + "': " + save_err;
                        std::error_code ignored;
                        fs::remove_all(write_cache_root, ignored);
                        return result;
                    }

                    ++result.stats.tasks_succeeded;
                    ++result.stats.pngs_written;
                    result.written_files.push_back(CachePaths::frame_png_path(cache_root,
                                                                              asset_name,
                                                                              animation_name,
                                                                              pct,
                                                                              Variant::Normal,
                                                                              frame_idx));
                }
            }

            if (touched_animation) {
                ++result.stats.animations_touched;
                result.touched_animations.push_back(asset_name + "::" + animation_name);
            }
        }

        if (touched_asset && !opt.dry_run) {
            std::string replace_err;
            if (!ReplaceAssetCacheAtomically(cache_root, write_cache_root, asset_name, replace_err)) {
                ++result.stats.tasks_failed;
                result.error = replace_err;
                std::error_code ignored;
                fs::remove_all(write_cache_root, ignored);
                return result;
            }

            if (!asset_obj.contains("size_settings") || !asset_obj["size_settings"].is_object()) {
                asset_obj["size_settings"] = nlohmann::json::object();
            }
            asset_obj["size_settings"]["scale_percentage"] = 100.0;

            nlohmann::json percentages = nlohmann::json::array();
            nlohmann::json steps = nlohmann::json::array();
            for (std::size_t idx = 0; idx < plan.percents.size(); ++idx) {
                percentages.push_back(plan.percents[idx]);
                steps.push_back(plan.steps[idx]);
            }
            const std::uint64_t revision = static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count());
            asset_obj["scaling_profile"] = nlohmann::json{
                {"revision", revision},
                {"percentages", percentages},
                {"steps", steps},
                {"min_scale", plan.steps.empty() ? 1.0f : plan.steps.back()},
                {"max_scale", plan.max_camera_scale},
                {"baked_authored_scale", plan.authored_scale},
                {"scale100_width", plan.scale100_w},
                {"scale100_height", plan.scale100_h}
            };
            manifest_modified = true;
        }
    }

    std::sort(result.touched_animations.begin(), result.touched_animations.end());
    result.touched_animations.erase(std::unique(result.touched_animations.begin(), result.touched_animations.end()),
                                    result.touched_animations.end());

    {
        std::unordered_set<std::string> touched_assets;
        for (const auto& full_name : result.touched_animations) {
            const std::size_t split = full_name.find("::");
            touched_assets.insert(full_name.substr(0, split));
        }
        result.touched_assets.assign(touched_assets.begin(), touched_assets.end());
        std::sort(result.touched_assets.begin(), result.touched_assets.end());
    }
    result.stats.assets_touched = static_cast<std::uint64_t>(result.touched_assets.size());

    if (manifest_modified) {
        std::ofstream out(manifest_path, std::ios::trunc);
        if (!out.is_open()) {
            result.error = "Failed to open manifest for writing: " + manifest_path.string();
            return result;
        }
        out << manifest.dump(2) << '\n';
        if (!out.good()) {
            result.error = "Failed writing manifest: " + manifest_path.string();
            return result;
        }
    }

    if (result.stats.tasks_total == 0) {
        log.info("No cache work required.");
    } else {
        log.info("Generated " + std::to_string(result.stats.pngs_written) + " camera-aware texture cache files.");
    }

    result.ok = true;
    return result;
}

std::optional<fs::path> ImageCacheGenerator::FindRepoRootFrom(const fs::path& start_dir) {
    std::error_code ec;
    fs::path current = fs::absolute(start_dir, ec);
    if (ec) {
        current = start_dir;
    }

    for (int i = 0; i < 16; ++i) {
        if (current.empty()) {
            break;
        }
        if (fs::exists(current / "manifest.json", ec) && !ec) {
            return current;
        }
        if (!current.has_parent_path() || current.parent_path() == current) {
            break;
        }
        current = current.parent_path();
    }
    return std::nullopt;
}

std::optional<fs::path> ImageCacheGenerator::ResolveManifestPath(const GeneratorOptions& opt) {
    if (!opt.manifest_path.empty()) {
        return opt.manifest_path;
    }
    const auto repo_root = FindRepoRootFrom(fs::current_path());
    if (!repo_root.has_value()) {
        return std::nullopt;
    }
    return repo_root.value() / "manifest.json";
}

fs::path ImageCacheGenerator::ResolveCacheRoot(const fs::path& repo_root, const GeneratorOptions& opt) {
    if (!opt.cache_root_override.empty()) {
        return opt.cache_root_override;
    }
    return repo_root / CachePaths::kCacheDirName;
}

fs::path ImageCacheGenerator::ResolveAssetSourceDir(const fs::path& manifest_dir,
                                                    const fs::path& repo_root,
                                                    const std::string& asset_name,
                                                    const nlohmann::json& asset_obj) {
    fs::path asset_dir = repo_root / "resources" / "assets" / asset_name;
    if (asset_obj.is_object()) {
        auto it = asset_obj.find("asset_directory");
        if (it != asset_obj.end() && it->is_string()) {
            fs::path configured = it->get<std::string>();
            if (configured.is_relative()) {
                configured = manifest_dir / configured;
            }
            asset_dir = configured;
        }
    }
    return asset_dir.lexically_normal();
}

std::vector<std::pair<std::string, fs::path>> ImageCacheGenerator::DiscoverAnimations(const fs::path& asset_src_dir) {
    std::vector<std::pair<std::string, fs::path>> result;
    std::error_code ec;
    if (!fs::exists(asset_src_dir, ec) || ec || !fs::is_directory(asset_src_dir, ec)) {
        return result;
    }

    std::vector<std::pair<std::string, fs::path>> subdirs;
    bool has_root_png = false;
    for (const auto& entry : fs::directory_iterator(asset_src_dir, ec)) {
        if (ec) {
            break;
        }
        if (entry.is_directory(ec) && !ec) {
            subdirs.emplace_back(entry.path().filename().string(), entry.path());
            continue;
        }
        if (entry.is_regular_file(ec) && !ec && is_png_file(entry.path())) {
            has_root_png = true;
        }
    }

    std::sort(subdirs.begin(), subdirs.end(), [](const auto& a, const auto& b) {
        return a.first < b.first;
    });

    if (!subdirs.empty()) {
        return subdirs;
    }
    if (has_root_png) {
        result.emplace_back("default", asset_src_dir);
    }
    return result;
}

std::vector<fs::path> ImageCacheGenerator::EnumerateSourceFrames(const fs::path& anim_src_dir) {
    std::vector<fs::path> frames;
    std::error_code ec;
    for (int idx = 0; idx < 100000; ++idx) {
        fs::path frame_path = anim_src_dir / (std::to_string(idx) + ".png");
        if (!fs::exists(frame_path, ec) || ec) {
            break;
        }
        frames.push_back(frame_path);
    }
    return frames;
}

bool ImageCacheGenerator::OutputMissingAnyVariant(const fs::path& cache_root,
                                                  const std::string& asset_name,
                                                  const std::string& anim_name,
                                                  int scale_pct,
                                                  int out_index) {
    std::error_code ec;
    const fs::path output = CachePaths::frame_png_path(cache_root,
                                                       asset_name,
                                                       anim_name,
                                                       scale_pct,
                                                       Variant::Normal,
                                                       out_index);
    return !fs::exists(output, ec) || ec;
}

std::optional<ImageRGBA> ImageCacheGenerator::LoadPngRGBA(const fs::path& path, std::string& err) {
    err.clear();
    int w = 0;
    int h = 0;
    int channels = 0;
    stbi_uc* pixels = stbi_load(path.string().c_str(), &w, &h, &channels, 4);
    if (!pixels || w <= 0 || h <= 0) {
        err = "stbi_load failed";
        if (pixels) {
            stbi_image_free(pixels);
        }
        return std::nullopt;
    }

    ImageRGBA image;
    image.w = w;
    image.h = h;
    image.pixels.assign(pixels, pixels + static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4u);
    stbi_image_free(pixels);
    return image;
}

bool ImageCacheGenerator::SavePngRGBA(const fs::path& path, const ImageRGBA& img, std::string& err) {
    err.clear();
    if (!img.valid()) {
        err = "invalid RGBA image";
        return false;
    }
    if (stbi_write_png(path.string().c_str(),
                       img.w,
                       img.h,
                       4,
                       img.pixels.data(),
                       img.w * 4) == 0) {
        err = "stbi_write_png failed";
        return false;
    }
    return true;
}

std::optional<ImageRGBA> ImageCacheGenerator::ResizeRGBA(const ImageRGBA& src,
                                                         int dst_w,
                                                         int dst_h,
                                                         std::string& err) {
    err.clear();
    if (!src.valid()) {
        err = "invalid source image";
        return std::nullopt;
    }
    if (dst_w <= 0 || dst_h <= 0) {
        err = "invalid destination size";
        return std::nullopt;
    }

    if (dst_w == src.w && dst_h == src.h) {
        return src;
    }

    ImageRGBA out;
    out.w = dst_w;
    out.h = dst_h;
    out.pixels.resize(static_cast<std::size_t>(dst_w) * static_cast<std::size_t>(dst_h) * 4u);

    for (int y = 0; y < dst_h; ++y) {
        const int src_y = std::min(src.h - 1, (y * src.h) / dst_h);
        for (int x = 0; x < dst_w; ++x) {
            const int src_x = std::min(src.w - 1, (x * src.w) / dst_w);
            const std::size_t src_index = (static_cast<std::size_t>(src_y) * static_cast<std::size_t>(src.w) +
                                           static_cast<std::size_t>(src_x)) * 4u;
            const std::size_t dst_index = (static_cast<std::size_t>(y) * static_cast<std::size_t>(dst_w) +
                                           static_cast<std::size_t>(x)) * 4u;
            out.pixels[dst_index + 0] = src.pixels[src_index + 0];
            out.pixels[dst_index + 1] = src.pixels[src_index + 1];
            out.pixels[dst_index + 2] = src.pixels[src_index + 2];
            out.pixels[dst_index + 3] = src.pixels[src_index + 3];
        }
    }

    return out;
}

} // namespace imgcache
