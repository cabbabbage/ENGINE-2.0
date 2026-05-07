#include "image_cache_generator.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <limits>
#include <optional>
#include <string>
#include <unordered_set>
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
                                     const std::string& animation_name) {
    if (!opt.filters.matches_asset(asset_name) || !opt.filters.matches_anim(animation_name)) {
        return false;
    }

    if (!opt.has_explicit_rebuild_requests()) {
        return true;
    }

    for (const auto& request : opt.explicit_rebuild_requests) {
        if (request.asset_name == asset_name && request.animation_name == animation_name) {
            return true;
        }
    }
    return false;
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

    std::vector<float> scale_steps = opt.scale_percents.empty() ? canonical_scale_steps() : std::vector<float>{};
    if (!opt.scale_percents.empty()) {
        scale_steps.reserve(opt.scale_percents.size());
        for (int pct : opt.scale_percents) {
            if (pct <= 0) {
                continue;
            }
            scale_steps.push_back(std::max(0.01f, static_cast<float>(pct) * 0.01f));
        }
        std::sort(scale_steps.begin(), scale_steps.end(), std::greater<float>());
        scale_steps.erase(std::unique(scale_steps.begin(), scale_steps.end(), [](float a, float b) {
            return std::fabs(a - b) < 1e-6f;
        }), scale_steps.end());
        if (scale_steps.empty()) {
            scale_steps = canonical_scale_steps();
        }
    }

    const auto& assets = manifest["assets"];
    for (auto it = assets.begin(); it != assets.end(); ++it) {
        const std::string asset_name = it.key();
        if (!opt.filters.matches_asset(asset_name)) {
            continue;
        }
        if (!it.value().is_object()) {
            continue;
        }

        const fs::path asset_src_dir = ResolveAssetSourceDir(manifest_dir, repo_root, asset_name, it.value());
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

        for (const auto& animation_entry : animations) {
            const std::string& animation_name = animation_entry.first;
            const fs::path& animation_src_dir = animation_entry.second;
            if (!opt.filters.matches_anim(animation_name)) {
                continue;
            }

            const auto frames = EnumerateSourceFrames(animation_src_dir);
            if (frames.empty()) {
                continue;
            }

            const GeneratorOptions::AnimationRebuildRequest* explicit_request = nullptr;
            if (opt.has_explicit_rebuild_requests()) {
                for (const auto& request : opt.explicit_rebuild_requests) {
                    if (request.asset_name == asset_name && request.animation_name == animation_name) {
                        explicit_request = &request;
                        break;
                    }
                }
                if (!explicit_request) {
                    continue;
                }
            }

            bool touched_animation = false;
            for (std::size_t frame_index = 0; frame_index < frames.size(); ++frame_index) {
                const int frame_idx = static_cast<int>(frame_index);
                bool frame_requested = opt.filters.matches_source_frame(frame_idx);
                bool explicit_frame_requested = false;
                bool explicit_all_requested = false;
                if (explicit_request) {
                    explicit_all_requested = has_normal_variant_mask(explicit_request->all_frames_variant_mask);
                    auto frame_it = explicit_request->frame_variant_masks.find(frame_idx);
                    if (frame_it != explicit_request->frame_variant_masks.end()) {
                        explicit_frame_requested = has_normal_variant_mask(frame_it->second);
                    }
                    frame_requested = frame_requested || explicit_all_requested || explicit_frame_requested;
                }

                if (!frame_requested) {
                    continue;
                }

                std::string load_err;
                std::optional<ImageRGBA> src_opt = LoadPngRGBA(frames[frame_index], load_err);
                if (!src_opt.has_value()) {
                    result.error = "Failed to load source frame '" + frames[frame_index].string() + "': " + load_err;
                    return result;
                }
                const ImageRGBA& src = src_opt.value();

                for (float step : scale_steps) {
                    const int pct = scale_percent(step);
                    const fs::path out_path = CachePaths::frame_png_path(cache_root,
                                                                         asset_name,
                                                                         animation_name,
                                                                         pct,
                                                                         Variant::Normal,
                                                                         frame_idx);

                    bool should_write = opt.force_rebuild;
                    if (!should_write && explicit_request) {
                        should_write = explicit_all_requested || explicit_frame_requested;
                    }
                    if (!should_write) {
                        std::error_code ec;
                        should_write = !fs::exists(out_path, ec) || ec;
                    }

                    if (!should_write) {
                        ++result.stats.pngs_skipped_existing;
                        continue;
                    }

                    ++result.stats.tasks_total;
                    touched_animation = true;

                    if (opt.dry_run) {
                        ++result.stats.tasks_succeeded;
                        continue;
                    }

                    const int dst_w = scale_dimension(src.w, step);
                    const int dst_h = scale_dimension(src.h, step);
                    std::optional<ImageRGBA> scaled = ResizeRGBA(src, dst_w, dst_h, load_err);
                    if (!scaled.has_value()) {
                        ++result.stats.tasks_failed;
                        result.error = "Failed to resize frame '" + frames[frame_index].string() + "': " + load_err;
                        return result;
                    }

                    const std::optional<AlphaBounds> scaled_bounds = FindAlphaBounds(scaled.value());
                    int shared_w = scale_dimension(shared_crop_size->width, step);
                    int shared_h = scale_dimension(shared_crop_size->height, step);
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
                        return result;
                    }

                    std::error_code ec;
                    fs::create_directories(out_path.parent_path(), ec);
                    if (ec) {
                        ++result.stats.tasks_failed;
                        result.error = "Failed creating cache directory: " + out_path.parent_path().string();
                        return result;
                    }

                    std::string save_err;
                    if (!SavePngRGBA(out_path, cropped.value(), save_err)) {
                        ++result.stats.tasks_failed;
                        result.error = "Failed writing frame '" + out_path.string() + "': " + save_err;
                        return result;
                    }

                    ++result.stats.tasks_succeeded;
                    ++result.stats.pngs_written;
                    result.written_files.push_back(out_path);
                }
            }

            if (touched_animation) {
                ++result.stats.animations_touched;
                result.touched_animations.push_back(asset_name + "::" + animation_name);
            }
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

    if (result.stats.tasks_total == 0) {
        log.info("No cache work required.");
    } else {
        log.info("Generated " + std::to_string(result.stats.pngs_written) + " alpha-cropped normal texture files.");
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
