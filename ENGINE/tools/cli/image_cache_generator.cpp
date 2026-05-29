#include "image_cache_generator.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <chrono>
#include <fstream>
#include <ios>
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

bool is_png_file(const fs::path& path) {
    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return ext == ".png";
}

constexpr int kCacheManifestSchemaVersion = 2;
constexpr const char* kCacheManifestGeneratorVersion = "image_cache_generator.v4";
constexpr const char* kCacheManifestFileName = "cache_manifest.json";
constexpr std::uint64_t kFnvOffset = 14695981039346656037ull;
constexpr std::uint64_t kFnvPrime = 1099511628211ull;

void fnv1a_append(std::uint64_t& hash, const void* data, std::size_t size) {
    const auto* bytes = static_cast<const unsigned char*>(data);
    for (std::size_t idx = 0; idx < size; ++idx) {
        hash ^= static_cast<std::uint64_t>(bytes[idx]);
        hash *= kFnvPrime;
    }
}

std::string hex_u64(std::uint64_t value) {
    std::ostringstream out;
    out << std::hex << std::setfill('0') << std::setw(16) << value;
    return out.str();
}

std::optional<std::string> HashFileFnv1a(const fs::path& path, std::string& err) {
    err.clear();
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) {
        err = "failed to open source frame for hashing: " + path.string();
        return std::nullopt;
    }

    std::uint64_t hash = kFnvOffset;
    std::array<char, 64 * 1024> buffer{};
    while (in.good()) {
        in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const std::streamsize got = in.gcount();
        if (got > 0) {
            fnv1a_append(hash, buffer.data(), static_cast<std::size_t>(got));
        }
    }
    if (!in.eof()) {
        err = "failed while reading source frame for hashing: " + path.string();
        return std::nullopt;
    }
    return hex_u64(hash);
}

nlohmann::json SourceFrameToJson(const CacheManifestSourceFrame& frame) {
    return nlohmann::json{{"filename", frame.filename},
                          {"order", frame.order},
                          {"hash", frame.hash},
                          {"width", frame.width},
                          {"height", frame.height}};
}

nlohmann::json AnimationToJson(const CacheManifestAnimation& animation) {
    nlohmann::json frames = nlohmann::json::array();
    for (const auto& frame : animation.source_frames) {
        frames.push_back(SourceFrameToJson(frame));
    }
    return nlohmann::json{{"name", animation.name}, {"source_frames", frames}};
}

std::optional<CacheManifestSourceFrame> BuildSourceFrameManifest(const fs::path& frame_path,
                                                                 int order,
                                                                 std::string& err) {
    err.clear();
    auto hash = HashFileFnv1a(frame_path, err);
    if (!hash.has_value()) {
        return std::nullopt;
    }

    int width = 0;
    int height = 0;
    int channels = 0;
    if (stbi_info(frame_path.string().c_str(), &width, &height, &channels) == 0 || width <= 0 || height <= 0) {
        err = "failed reading source frame dimensions: " + frame_path.string();
        return std::nullopt;
    }

    CacheManifestSourceFrame frame;
    frame.filename = frame_path.filename().string();
    frame.order = order;
    frame.hash = hash.value();
    frame.width = width;
    frame.height = height;
    return frame;
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

std::optional<ImageRGBA> ResizeAlphaCropToSharedCanvasFast(const ImageRGBA& src,
                                                           const AlphaBounds& bounds,
                                                           int shared_width,
                                                           int shared_height,
                                                           int visible_width,
                                                           int visible_height,
                                                           std::string& err) {
    err.clear();

    if (!src.valid()) {
        err = "invalid source image";
        return std::nullopt;
    }
    if (!bounds.valid()) {
        err = "invalid alpha bounds";
        return std::nullopt;
    }
    if (bounds.left < 0 || bounds.top < 0 || bounds.right > src.w || bounds.bottom > src.h) {
        err = "alpha bounds exceed source image dimensions";
        return std::nullopt;
    }
    if (shared_width <= 0 || shared_height <= 0 || visible_width <= 0 || visible_height <= 0) {
        err = "invalid output size";
        return std::nullopt;
    }
    if (visible_width > shared_width || visible_height > shared_height) {
        err = "visible crop exceeds shared crop canvas";
        return std::nullopt;
    }

    ImageRGBA out;
    out.w = shared_width;
    out.h = shared_height;
    out.pixels.assign(static_cast<std::size_t>(shared_width) * static_cast<std::size_t>(shared_height) * 4u, 0u);

    const int paste_x = (shared_width - visible_width) / 2;
    const int paste_y = shared_height - visible_height;
    const int src_w = bounds.width();
    const int src_h = bounds.height();

    for (int y = 0; y < visible_height; ++y) {
        const int src_y = bounds.top + std::min(src_h - 1, (y * src_h) / visible_height);
        const std::size_t dst_row = static_cast<std::size_t>(paste_y + y) * static_cast<std::size_t>(out.w);
        const std::size_t src_row = static_cast<std::size_t>(src_y) * static_cast<std::size_t>(src.w);

        for (int x = 0; x < visible_width; ++x) {
            const int src_x = bounds.left + std::min(src_w - 1, (x * src_w) / visible_width);
            const std::size_t src_idx = (src_row + static_cast<std::size_t>(src_x)) * 4u;
            const std::size_t dst_idx = (dst_row + static_cast<std::size_t>(paste_x + x)) * 4u;

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

float read_float_json(const nlohmann::json& node, const char* key, float fallback) {
    auto it = node.find(key);
    if (it == node.end() || !it->is_number()) {
        return fallback;
    }
    const float value = static_cast<float>(it->get<double>());
    return std::isfinite(value) ? value : fallback;
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


std::optional<CacheManifest> BuildCurrentCacheManifest(const std::string& asset_name,
                                                       const std::vector<std::pair<std::string, fs::path>>& animations,
                                                       float authored_scale,
                                                       const SharedCropSize& shared_crop,
                                                       std::string& err) {
    err.clear();
    CacheManifest manifest;
    manifest.schema_version = kCacheManifestSchemaVersion;
    manifest.generator_version = kCacheManifestGeneratorVersion;
    manifest.asset_name = asset_name;
    manifest.authored_scale_percentage = authored_scale * 100.0f;
    manifest.crop_canvas.shared_width = shared_crop.width;
    manifest.crop_canvas.shared_height = shared_crop.height;

    for (const auto& animation_entry : animations) {
        CacheManifestAnimation animation;
        animation.name = animation_entry.first;
        const auto frames = ImageCacheGenerator::EnumerateSourceFrames(animation_entry.second);
        for (std::size_t frame_index = 0; frame_index < frames.size(); ++frame_index) {
            auto frame = BuildSourceFrameManifest(frames[frame_index], static_cast<int>(frame_index), err);
            if (!frame.has_value()) {
                return std::nullopt;
            }
            animation.source_frames.push_back(frame.value());
        }
        manifest.animations.push_back(animation);
    }

    manifest.digest = ImageCacheGenerator::GenerateManifestDigest(manifest);
    return manifest;
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


fs::path ImageCacheGenerator::CacheManifestPath(const fs::path& cache_root, const std::string& asset_name) {
    return cache_root / asset_name / kCacheManifestFileName;
}

nlohmann::json ImageCacheGenerator::CacheManifestToJson(const CacheManifest& manifest, bool include_digest) {
    nlohmann::json animations = nlohmann::json::array();
    for (const auto& animation : manifest.animations) {
        animations.push_back(AnimationToJson(animation));
    }

    nlohmann::json json{{"schema_version", manifest.schema_version},
                        {"generator_version", manifest.generator_version},
                        {"asset_name", manifest.asset_name},
                        {"authored_scale_percentage", manifest.authored_scale_percentage},
                        {"crop_canvas", nlohmann::json{{"shared_width", manifest.crop_canvas.shared_width},
                                                        {"shared_height", manifest.crop_canvas.shared_height}}},
                        {"animations", animations}};
    if (include_digest) {
        json["digest"] = manifest.digest;
    }
    return json;
}

std::optional<CacheManifest> ImageCacheGenerator::CacheManifestFromJson(const nlohmann::json& json, std::string& err) {
    err.clear();
    try {
        if (!json.is_object()) {
            err = "cache manifest root is not an object";
            return std::nullopt;
        }
        CacheManifest manifest;
        manifest.schema_version = json.value("schema_version", 0);
        manifest.generator_version = json.value("generator_version", std::string{});
        manifest.asset_name = json.value("asset_name", std::string{});
        manifest.digest = json.value("digest", std::string{});
        manifest.authored_scale_percentage = json.value("authored_scale_percentage", 100.0f);

        const auto& crop_canvas = json.at("crop_canvas");
        manifest.crop_canvas.shared_width = crop_canvas.value("shared_width", 0);
        manifest.crop_canvas.shared_height = crop_canvas.value("shared_height", 0);

        for (const auto& animation_json : json.at("animations")) {
            CacheManifestAnimation animation;
            animation.name = animation_json.value("name", std::string{});
            for (const auto& frame_json : animation_json.at("source_frames")) {
                CacheManifestSourceFrame frame;
                frame.filename = frame_json.value("filename", std::string{});
                frame.order = frame_json.value("order", 0);
                frame.hash = frame_json.value("hash", std::string{});
                frame.width = frame_json.value("width", 0);
                frame.height = frame_json.value("height", 0);
                animation.source_frames.push_back(frame);
            }
            manifest.animations.push_back(animation);
        }
        return manifest;
    } catch (const std::exception& e) {
        err = std::string("failed parsing cache manifest: ") + e.what();
        return std::nullopt;
    }
}

std::string ImageCacheGenerator::GenerateManifestDigest(const CacheManifest& manifest) {
    const std::string canonical = CacheManifestToJson(manifest, false).dump();
    std::uint64_t hash = kFnvOffset;
    fnv1a_append(hash, canonical.data(), canonical.size());
    return hex_u64(hash);
}

bool ImageCacheGenerator::ReadCacheManifest(const fs::path& path, CacheManifest& manifest, std::string& err) {
    err.clear();
    std::ifstream in(path);
    if (!in.is_open()) {
        err = "cache manifest does not exist: " + path.string();
        return false;
    }
    nlohmann::json json;
    try {
        in >> json;
    } catch (const std::exception& e) {
        err = std::string("failed reading cache manifest: ") + e.what();
        return false;
    }
    auto parsed = CacheManifestFromJson(json, err);
    if (!parsed.has_value()) {
        return false;
    }
    manifest = parsed.value();
    return true;
}

bool ImageCacheGenerator::WriteCacheManifest(const fs::path& path, const CacheManifest& manifest, std::string& err) {
    err.clear();
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    if (ec) {
        err = "failed creating cache manifest directory: " + path.parent_path().string();
        return false;
    }
    std::ofstream out(path, std::ios::trunc);
    if (!out.is_open()) {
        err = "failed opening cache manifest for writing: " + path.string();
        return false;
    }
    out << CacheManifestToJson(manifest, true).dump(2) << '\n';
    if (!out.good()) {
        err = "failed writing cache manifest: " + path.string();
        return false;
    }
    return true;
}

bool ImageCacheGenerator::CacheManifestsExactlyMatch(const CacheManifest& existing,
                                                     const CacheManifest& current,
                                                     std::string& reason) {
    reason.clear();
    if (existing.schema_version != current.schema_version) {
        reason = "cache manifest schema version mismatch: old=" +
                 std::to_string(existing.schema_version) + " new=" +
                 std::to_string(current.schema_version);
    } else if (existing.digest != current.digest) {
        reason = "cache manifest digest differs";
    } else if (CacheManifestToJson(existing, true) != CacheManifestToJson(current, true)) {
        reason = "cache manifest JSON differs despite matching digest";
    }
    return reason.empty();
}

bool ImageCacheGenerator::OutputMissingAnyFrame(const fs::path& cache_root,
                                                const std::string& asset_name,
                                                const std::string& anim_name,
                                                int out_index) {
    std::error_code ec;
    const fs::path output = CachePaths::frame_png_path(cache_root, asset_name, anim_name, out_index);
    return !fs::exists(output, ec) || ec;
}

void ImageCacheGenerator::DeleteOldMultiVariantCache(const fs::path& cache_root,
                                                      const std::string& asset_name,
                                                      ILogger& log) {
    std::error_code ec;
    const fs::path asset_cache_dir = cache_root / asset_name / CachePaths::kAnimationsDirName;
    if (!fs::exists(asset_cache_dir, ec) || ec) {
        return;
    }

    bool found_old = false;
    std::vector<fs::path> stale_scale_dirs;
    for (fs::recursive_directory_iterator it(asset_cache_dir, ec), end; it != end && !ec; it.increment(ec)) {
        if (!it->is_directory(ec) || ec) {
            continue;
        }
        const std::string dirname = it->path().filename().string();
        if (dirname.rfind("scale_", 0) != 0) {
            continue;
        }
        if (!found_old) {
            log.info("[ImageCacheGenerator] Detected old multi-variant cache in " + asset_cache_dir.string() +
                     "; deleting stale scale_* directories before rebuild.");
            found_old = true;
        }
        stale_scale_dirs.push_back(it->path());
        it.disable_recursion_pending();
    }
    for (const auto& stale_dir : stale_scale_dirs) {
        std::error_code rm_ec;
        fs::remove_all(stale_dir, rm_ec);
    }
}

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

        const float authored_scale = ReadAuthoredScale(asset_obj);

        std::string manifest_err;
        std::optional<CacheManifest> current_manifest_opt = BuildCurrentCacheManifest(asset_name,
                                                                                       animations,
                                                                                       authored_scale,
                                                                                       *shared_crop_size,
                                                                                       manifest_err);
        if (!current_manifest_opt.has_value()) {
            result.error = manifest_err;
            return result;
        }
        const CacheManifest current_cache_manifest = current_manifest_opt.value();

        std::vector<std::string> stale_reasons;
        if (opt.force_rebuild) {
            stale_reasons.emplace_back("force rebuild requested");
        }

        CacheManifest existing_cache_manifest;
        const fs::path cache_manifest_path = CacheManifestPath(cache_root, asset_name);
        std::string existing_manifest_err;
        if (!ReadCacheManifest(cache_manifest_path, existing_cache_manifest, existing_manifest_err)) {
            stale_reasons.push_back(existing_manifest_err);
        } else {
            std::string compare_reason;
            if (!CacheManifestsExactlyMatch(existing_cache_manifest, current_cache_manifest, compare_reason)) {
                stale_reasons.push_back(compare_reason);
            }
        }

        // Check for missing output frames
        if (stale_reasons.empty() && !opt.force_rebuild) {
            for (const auto& animation : current_cache_manifest.animations) {
                for (const auto& frame : animation.source_frames) {
                    if (OutputMissingAnyFrame(cache_root, asset_name, animation.name, frame.order)) {
                        stale_reasons.push_back("missing output frame: " + animation.name + "/" + std::to_string(frame.order));
                        break;
                    }
                }
                if (!stale_reasons.empty()) break;
            }
        }

        if (stale_reasons.empty()) {
            continue;
        }

        log.info("[ImageCacheGenerator] Rebuilding asset '" + asset_name + "': " + stale_reasons.front());
        for (std::size_t reason_index = 1; reason_index < stale_reasons.size(); ++reason_index) {
            log.info("[ImageCacheGenerator] Additional stale reason for '" + asset_name + "': " + stale_reasons[reason_index]);
        }

        // Delete old multi-variant cache directories if they exist
        DeleteOldMultiVariantCache(cache_root, asset_name, log);

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
            std::string load_err;

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

                const std::optional<AlphaBounds> src_bounds = FindAlphaBounds(src_opt.value());
                if (!src_bounds.has_value()) {
                    result.error = "Source frame has no visible alpha pixels: " + frames[frame_index].string();
                    if (!opt.dry_run) {
                        std::error_code ignored;
                        fs::remove_all(write_cache_root, ignored);
                    }
                    return result;
                }

                const fs::path out_path = CachePaths::frame_png_path(write_cache_root,
                                                                     asset_name,
                                                                     animation_name,
                                                                     frame_idx);

                ++result.stats.tasks_total;
                touched_animation = true;
                touched_asset = true;

                if (opt.dry_run) {
                    ++result.stats.tasks_succeeded;
                    continue;
                }

                // Crop to shared canvas at original source resolution
                // No multi-variant scaling — just crop to the shared canvas
                std::optional<ImageRGBA> cropped = CropToSharedCanvas(src_opt.value(),
                                                                       src_bounds,
                                                                       shared_crop_size->width,
                                                                       shared_crop_size->height,
                                                                       load_err);
                if (!cropped.has_value()) {
                    ++result.stats.tasks_failed;
                    result.error = "Failed to crop frame '" + frames[frame_index].string() + "': " + load_err;
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
                    src_opt.reset();
                    return result;
                }

                std::string save_err;
                const bool saved = SavePngRGBA(out_path, cropped.value(), save_err);
                cropped.reset();

                if (!saved) {
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
                                                                          frame_idx));

                src_opt.reset();
            }

            if (touched_animation) {
                ++result.stats.animations_touched;
                result.touched_animations.push_back(asset_name + "::" + animation_name);
            }
        }

        if (touched_asset && !opt.dry_run) {
            std::string cache_manifest_write_err;
            if (!WriteCacheManifest(CacheManifestPath(write_cache_root, asset_name),
                                    current_cache_manifest,
                                    cache_manifest_write_err)) {
                ++result.stats.tasks_failed;
                result.error = cache_manifest_write_err;
                std::error_code ignored;
                fs::remove_all(write_cache_root, ignored);
                return result;
            }

            std::string replace_err;
            if (!ReplaceAssetCacheAtomically(cache_root, write_cache_root, asset_name, replace_err)) {
                ++result.stats.tasks_failed;
                result.error = replace_err;
                return result;
            }

            log.info("[ImageCacheGenerator] Generated single-frame cache for asset '" + asset_name + "'" +
                     (!stale_reasons.empty() ? " (" + stale_reasons.front() + ")" : ""));
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
        log.info("Generated " + std::to_string(result.stats.pngs_written) + " single-frame cache files.");
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
    // Favor generation speed over maximum PNG compression.
    stbi_write_png_compression_level = 1;

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