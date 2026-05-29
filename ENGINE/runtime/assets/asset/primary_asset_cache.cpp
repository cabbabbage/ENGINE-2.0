#include "primary_asset_cache.hpp"

#include "asset_info.hpp"
#include "rendering/render/scaling_logic.hpp"
#include "image_cache_generator.hpp"
#include "utils/log.hpp"

#include <SDL3_image/SDL_image.h>
#include <filesystem>
#include <unordered_set>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <unordered_map>
#include <optional>
#include <sstream>
#include <system_error>
#include <utility>

namespace fs = std::filesystem;

namespace {

constexpr SDL_PixelFormat kRuntimeRgbaPixelFormat = SDL_PIXELFORMAT_RGBA32;

class GeneratorLogBridge final : public imgcache::ILogger {
public:
    void info(const std::string& msg) override {
        vibble::log::info("[PrimaryAssetCache] " + msg);
    }

    void warn(const std::string& msg) override {
        vibble::log::warn("[PrimaryAssetCache] " + msg);
    }

    void error(const std::string& msg) override {
        vibble::log::warn("[PrimaryAssetCache] " + msg);
    }
};

int count_sequential_png(const fs::path& folder) {
    int count = 0;
    std::error_code ec;
    while (true) {
        fs::path frame_path = folder / (std::to_string(count) + ".png");
        if (!fs::exists(frame_path, ec) || ec) {
            break;
        }
        ++count;
    }
    return count;
}

SDL_Surface* load_rgba_surface(const fs::path& path) {
    SDL_Surface* loaded = CacheManager::load_surface(path.generic_string());
    if (!loaded) return nullptr;
    SDL_Surface* converted = SDL_ConvertSurface(loaded, kRuntimeRgbaPixelFormat);
    SDL_DestroySurface(loaded);
    return converted;
}

CacheManager::BundleFrameLayer make_layer(SDL_Surface* surface) {
    CacheManager::BundleFrameLayer layer;
    if (!surface) {
        return layer;
    }
    SDL_Surface* rgba = surface;
    if (surface->format != kRuntimeRgbaPixelFormat) {
        rgba = SDL_ConvertSurface(surface, kRuntimeRgbaPixelFormat);
        SDL_DestroySurface(surface);
    }
    if (!rgba) {
        return layer;
    }
    layer.width = rgba->w;
    layer.height = rgba->h;
    layer.pitch = rgba->pitch;
    layer.format = rgba ? rgba->format : static_cast<std::uint32_t>(kRuntimeRgbaPixelFormat);
    const std::size_t byte_count = static_cast<std::size_t>(layer.pitch) * static_cast<std::size_t>(layer.height);
    layer.pixels.resize(byte_count);
    std::memcpy(layer.pixels.data(), rgba->pixels, byte_count);
    if (rgba != surface) {
        SDL_DestroySurface(rgba);
    }
    return layer;
}

CacheManager::BundleFrameLayer make_transparent_layer(int width, int height) {
    CacheManager::BundleFrameLayer layer;
    layer.width = std::max(1, width);
    layer.height = std::max(1, height);
    layer.format = kRuntimeRgbaPixelFormat;
    layer.pitch = layer.width * 4;
    const std::size_t byte_count =
        static_cast<std::size_t>(layer.pitch) * static_cast<std::size_t>(layer.height);
    layer.pixels.assign(byte_count, static_cast<std::uint8_t>(0));
    return layer;
}

std::uint8_t glyph_row(char ch, int row) {
    if (row < 0 || row >= 7) { return 0; }
    switch (static_cast<char>(std::toupper(static_cast<unsigned char>(ch)))) {
    case 'A': { static constexpr std::uint8_t rows[7] = {0b01110, 0b10001, 0b10001, 0b11111, 0b10001, 0b10001, 0b10001}; return rows[row]; }
    case 'B': { static constexpr std::uint8_t rows[7] = {0b11110, 0b10001, 0b10001, 0b11110, 0b10001, 0b10001, 0b11110}; return rows[row]; }
    case 'D': { static constexpr std::uint8_t rows[7] = {0b11110, 0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b11110}; return rows[row]; }
    case 'E': { static constexpr std::uint8_t rows[7] = {0b11111, 0b10000, 0b10000, 0b11110, 0b10000, 0b10000, 0b11111}; return rows[row]; }
    case 'S': { static constexpr std::uint8_t rows[7] = {0b01111, 0b10000, 0b10000, 0b01110, 0b00001, 0b00001, 0b11110}; return rows[row]; }
    case 'T': { static constexpr std::uint8_t rows[7] = {0b11111, 0b00100, 0b00100, 0b00100, 0b00100, 0b00100, 0b00100}; return rows[row]; }
    default: return 0;
    }
}

int bitmap_text_width(const std::string& text, int scale) {
    if (text.empty()) return 0;
    return static_cast<int>(text.size()) * 5 * scale + static_cast<int>(text.size() - 1) * scale;
}

void draw_bitmap_text(SDL_Surface* surface, const std::string& text, int x, int y, int scale, Uint32 color) {
    if (!surface || text.empty() || scale <= 0) return;
    int cursor_x = x;
    for (char ch : text) {
        if (ch != ' ') {
            for (int row = 0; row < 7; ++row) {
                const std::uint8_t bits = glyph_row(ch, row);
                for (int col = 0; col < 5; ++col) {
                    if ((bits & (1u << (4 - col))) == 0) continue;
                    SDL_Rect rect{cursor_x + col * scale, y + row * scale, scale, scale};
                    SDL_FillSurfaceRect(surface, &rect, color);
                }
            }
        }
        cursor_x += 6 * scale;
    }
}

CacheManager::BundleFrameLayer make_bad_asset_layer() {
    constexpr int width = 96, height = 48, scale = 3;
    SDL_Surface* surface = SDL_CreateSurface(width, height, kRuntimeRgbaPixelFormat);
    if (!surface) return make_transparent_layer(1, 1);
    const SDL_PixelFormatDetails* fmt = SDL_GetPixelFormatDetails(surface->format);
    SDL_Palette* palette = SDL_GetSurfacePalette(surface);
    const Uint32 background = fmt ? SDL_MapRGBA(fmt, palette, 255, 0, 255, 255) : 0;
    const Uint32 border = fmt ? SDL_MapRGBA(fmt, palette, 0, 0, 0, 255) : 0;
    const Uint32 text = fmt ? SDL_MapRGBA(fmt, palette, 255, 255, 255, 255) : 0;
    SDL_FillSurfaceRect(surface, nullptr, background);
    SDL_Rect top{0, 0, width, 2}, bottom{0, height - 2, width, 2}, left{0, 0, 2, height}, right{width - 2, 0, 2, height};
    SDL_FillSurfaceRect(surface, &top, border);
    SDL_FillSurfaceRect(surface, &bottom, border);
    SDL_FillSurfaceRect(surface, &left, border);
    SDL_FillSurfaceRect(surface, &right, border);
    draw_bitmap_text(surface, "BAD", (width - bitmap_text_width("BAD", scale)) / 2, 2, scale, text);
    draw_bitmap_text(surface, "ASSET", (width - bitmap_text_width("ASSET", scale)) / 2, 25, scale, text);
    CacheManager::BundleFrameLayer layer = make_layer(surface);
    SDL_DestroySurface(surface);
    return layer.empty() ? make_transparent_layer(1, 1) : layer;
}

CacheManager::BundleFrame make_placeholder_frame() {
    CacheManager::BundleFrame frame;
    frame.base_layer = make_bad_asset_layer();
    return frame;
}

SDL_Surface* surface_from_layer(const CacheManager::BundleFrameLayer& layer) {
    if (layer.empty()) return nullptr;
    return SDL_CreateSurfaceFrom(layer.width, layer.height,
                                 static_cast<SDL_PixelFormat>(layer.format),
                                 const_cast<std::uint8_t*>(layer.pixels.data()), layer.pitch);
}

CacheManager::BundleFrameLayer load_layer_from_candidates(std::initializer_list<fs::path> candidates) {
    for (const auto& candidate : candidates) {
        std::error_code ec;
        if (!fs::exists(candidate, ec) || ec) continue;
        if (SDL_Surface* surface = load_rgba_surface(candidate)) {
            auto layer = make_layer(surface);
            SDL_DestroySurface(surface);
            return layer;
        }
    }
    return {};
}

bool bundle_frames_are_valid(const CacheManager::BundleData& bundle) {
    for (const auto& animation : bundle.animations) {
        for (const auto& frame : animation.frames) {
            if (frame.base_layer.empty()) return false;
            if (frame.base_layer.format != static_cast<std::uint32_t>(kRuntimeRgbaPixelFormat)) return false;
        }
    }
    return true;
}

const CacheManager::BundleAnimation* find_bundle_animation(const CacheManager::BundleData& bundle,
                                                           const std::string& animation_name) {
    for (const auto& animation : bundle.animations) {
        if (animation.name == animation_name) return &animation;
    }
    return nullptr;
}

struct CacheManifestBundleSnapshot {
    bool present = false;
    int schema_version = 0;
    std::string digest;
};

CacheManifestBundleSnapshot read_bundle_cache_manifest_snapshot(const CacheManager::BundleData& bundle) {
    CacheManifestBundleSnapshot snapshot{};
    if (!bundle.metadata_snapshot.is_object()) return snapshot;
    auto it = bundle.metadata_snapshot.find("_cache_manifest");
    if (it == bundle.metadata_snapshot.end() || !it->is_object()) return snapshot;
    snapshot.present = true;
    snapshot.schema_version = it->value("schema_version", 0);
    snapshot.digest = it->value("digest", std::string{});
    return snapshot;
}

struct CurrentCacheManifestSnapshot {
    bool required = false;
    bool present = false;
    int schema_version = 0;
    std::string digest;
    std::string reason;
};

CurrentCacheManifestSnapshot read_current_cache_manifest_snapshot(
    const std::string& asset_name) {
    CurrentCacheManifestSnapshot snapshot{};
    snapshot.required = true;
    imgcache::CacheManifest manifest;
    std::string err;
    const fs::path manifest_path = imgcache::ImageCacheGenerator::CacheManifestPath(fs::path("cache"), asset_name);
    if (!imgcache::ImageCacheGenerator::ReadCacheManifest(manifest_path, manifest, err)) {
        snapshot.reason = "cache_manifest_missing: " + err;
        return snapshot;
    }
    if (manifest.digest.empty()) {
        snapshot.reason = "cache_manifest_missing: digest is empty";
        return snapshot;
    }
    snapshot.present = true;
    snapshot.schema_version = manifest.schema_version;
    snapshot.digest = manifest.digest;
    return snapshot;
}

bool write_cache_manifest_snapshot_to_bundle(CacheManager::BundleData& bundle, const std::string& asset_name) {
    const CurrentCacheManifestSnapshot current = read_current_cache_manifest_snapshot(asset_name);
    if (!current.present) return false;
    if (!bundle.metadata_snapshot.is_object()) bundle.metadata_snapshot = nlohmann::json::object();
    bundle.metadata_snapshot["_cache_manifest"] = nlohmann::json{
        {"schema_version", current.schema_version},
        {"digest", current.digest}
    };
    return true;
}

struct BundleValidationResult {
    bool frames_valid = true;
    bool cache_manifest_ok = true;
    std::string cache_manifest_reason;

    bool ready() const { return frames_valid && cache_manifest_ok; }
};

BundleValidationResult validate_loaded_bundle(const CacheManager::BundleData& bundle,
                                              const std::string& asset_name) {
    BundleValidationResult result{};
    result.frames_valid = bundle_frames_are_valid(bundle);

    const CurrentCacheManifestSnapshot current_manifest = read_current_cache_manifest_snapshot(asset_name);
    if (current_manifest.required) {
        if (!current_manifest.present) {
            result.cache_manifest_ok = false;
            result.cache_manifest_reason = current_manifest.reason.empty()
                ? std::string("cache_manifest_missing") : current_manifest.reason;
        } else {
            const CacheManifestBundleSnapshot bundle_manifest = read_bundle_cache_manifest_snapshot(bundle);
            if (!bundle_manifest.present || bundle_manifest.digest.empty()) {
                result.cache_manifest_ok = false;
                result.cache_manifest_reason = "bundle metadata missing cache manifest digest";
            } else if (bundle_manifest.schema_version != current_manifest.schema_version) {
                result.cache_manifest_ok = false;
                result.cache_manifest_reason = "schema old=" + std::to_string(bundle_manifest.schema_version) +
                    " new=" + std::to_string(current_manifest.schema_version);
            } else if (bundle_manifest.digest != current_manifest.digest) {
                result.cache_manifest_ok = false;
                result.cache_manifest_reason = "manifest digest changed";
            }
        }
    }
    return result;
}

std::string describe_validation_failure(const BundleValidationResult& validation) {
    if (!validation.cache_manifest_ok)
        return validation.cache_manifest_reason.empty() ? "cache manifest failed" : validation.cache_manifest_reason;
    if (!validation.frames_valid) return "invalid frame data in bundle";
    return "unknown";
}

} // namespace

PrimaryAssetCache::PrimaryAssetCache(SDL_Renderer* renderer) : renderer_(renderer) {}

bool PrimaryAssetCache::ensure_cache_ready(AssetInfo& info,
                                           CacheManager::BundleData* out_bundle,
                                           const std::unordered_set<std::string>* animation_filter,
                                           WarmupOutcome* out_outcome,
                                           bool allow_placeholder_fallback) {
    const fs::path bundle_path = fs::path("cache") / info.name / "bundle.bin";
    const bool has_animation_filter = animation_filter && !animation_filter->empty();

    if (out_outcome) *out_outcome = WarmupOutcome::Failed;

    // Generate image cache if needed
    {
        std::string generation_error;
        imgcache::GeneratorOptions options;
        options.missing_only = true;
        options.force_rebuild = false;
        options.dry_run = false;
        options.quiet_task_logs = true;
        options.filters.assets.insert(info.name);
        if (animation_filter && !animation_filter->empty()) {
            options.filters.animations = *animation_filter;
        }
        GeneratorLogBridge logger;
        const imgcache::GenResult result = imgcache::ImageCacheGenerator::Run(options, logger);
        if (!result.ok) {
            vibble::log::warn("[PrimaryAssetCache] Failed to refresh image cache for " + info.name +
                              ": " + result.error);
            return false;
        }
    }

    CacheManager::BundleData bundle;
    const bool bundle_loaded = CacheManager::load_bundle(bundle_path.generic_string(), bundle);
    BundleValidationResult validation{};
    const bool bundle_ready = bundle_loaded
        ? (validation = validate_loaded_bundle(bundle, info.name), validation.ready())
        : false;

    if (bundle_ready) {
        if (out_outcome) *out_outcome = WarmupOutcome::Reused;
        if (out_bundle) *out_bundle = std::move(bundle);
        return true;
    }

    if (bundle_loaded) {
        vibble::log::info("[PrimaryAssetCache] Rebuilding bundle for " + info.name +
                          " (" + describe_validation_failure(validation) + ").");
    } else {
        vibble::log::info("[PrimaryAssetCache] Missing bundle for " + info.name + "; building from cache frames.");
    }

    // Build bundle from cache PNGs
    CacheManager::BundleData rebuilt;
    rebuilt.version = 3;
    rebuilt.metadata_snapshot = info.info_json_;
    write_cache_manifest_snapshot_to_bundle(rebuilt, info.name);

    if (info.anims_json_.is_object()) {
        for (auto it = info.anims_json_.begin(); it != info.anims_json_.end(); ++it) {
            if (!it.value().is_object()) continue;
            if (animation_filter && !animation_filter->empty() &&
                animation_filter->find(it.key()) == animation_filter->end()) continue;
            const nlohmann::json& anim_json = it.value();
            if (!anim_json.contains("source") || !anim_json["source"].is_object()) continue;
            const auto& source = anim_json["source"];
            if (source.value("kind", std::string{}) != "folder") continue;

            const fs::path folder = fs::path(info.asset_dir_path()) / source.value("path", it.key());
            const fs::path cache_anim_root = fs::path("cache") / info.name / "animations" / it.key();
            const int source_frame_count = count_sequential_png(folder);
            const int cached_frame_count = count_sequential_png(cache_anim_root);
            const int frame_count = std::max(source_frame_count, cached_frame_count);

            CacheManager::BundleAnimation bundle_anim;
            bundle_anim.name = it.key();

            if (frame_count <= 0) {
                if (!allow_placeholder_fallback) {
                    vibble::log::warn("[PrimaryAssetCache] No frames for " + info.name + "::" + it.key());
                    return false;
                }
                bundle_anim.frames.push_back(make_placeholder_frame());
                rebuilt.animations.push_back(std::move(bundle_anim));
                continue;
            }

            bundle_anim.frames.reserve(static_cast<std::size_t>(frame_count));
            for (int frame_idx = 0; frame_idx < frame_count; ++frame_idx) {
                CacheManager::BundleFrame frame;
                const std::string frame_name = std::to_string(frame_idx) + ".png";

                // Load from new cache layout: cache/<asset>/animations/<anim>/<frame>.png
                // Fallback to source folder
                frame.base_layer = load_layer_from_candidates({
                    cache_anim_root / frame_name,
                    folder / frame_name,
                });

                if (frame.base_layer.empty()) {
                    if (!allow_placeholder_fallback) {
                        vibble::log::warn("[PrimaryAssetCache] Missing frame " + frame_name + " for " +
                                          info.name + "::" + it.key());
                        return false;
                    }
                    frame.base_layer = make_bad_asset_layer();
                }
                bundle_anim.frames.push_back(std::move(frame));
            }
            rebuilt.animations.push_back(std::move(bundle_anim));
        }
    }

    if (!has_animation_filter) {
        if (!CacheManager::save_bundle(bundle_path.generic_string(), rebuilt)) {
            vibble::log::warn("[PrimaryAssetCache] Failed to save bundle for " + info.name + ".");
        }
    }

    if (out_bundle) *out_bundle = std::move(rebuilt);
    if (out_outcome) *out_outcome = bundle_loaded ? WarmupOutcome::Rebuilt : WarmupOutcome::Created;
    return true;
}

bool PrimaryAssetCache::load_cached_only(AssetInfo& info,
                                         std::unordered_map<std::string, PrebuiltAnimationFrames>& out_frames,
                                         CacheManager::BundleData& raw_bundle,
                                         const std::unordered_set<std::string>* animation_filter) {
    const fs::path bundle_path = fs::path("cache") / info.name / "bundle.bin";
    CacheManager::BundleData bundle;
    if (!CacheManager::load_bundle(bundle_path.generic_string(), bundle)) return false;

    const BundleValidationResult validation = validate_loaded_bundle(bundle, info.name);
    if (!validation.ready()) {
        vibble::log::info("[PrimaryAssetCache] Bundle rejected for " + info.name +
                          " (" + describe_validation_failure(validation) + ").");
        return false;
    }
    if (!populate_runtime_frames(info, bundle, out_frames, animation_filter)) return false;
    raw_bundle = std::move(bundle);
    return true;
}

bool PrimaryAssetCache::load_or_build(AssetInfo& info,
                                      std::unordered_map<std::string, PrebuiltAnimationFrames>& out_frames,
                                      CacheManager::BundleData& raw_bundle,
                                      const std::unordered_set<std::string>* animation_filter,
                                      bool allow_placeholder_fallback) {
    if (!ensure_cache_ready(info, &raw_bundle, animation_filter, nullptr, allow_placeholder_fallback))
        return false;
    out_frames.clear();
    return populate_runtime_frames(info, raw_bundle, out_frames, animation_filter);
}

bool PrimaryAssetCache::save_current(const AssetInfo& info) {
    CacheManager::BundleData bundle;
    bundle.version = 3;
    bundle.metadata_snapshot = info.info_json_;
    write_cache_manifest_snapshot_to_bundle(bundle, info.name);

    if (info.anims_json_.is_object()) {
        for (auto it = info.anims_json_.begin(); it != info.anims_json_.end(); ++it) {
            if (!it.value().is_object()) continue;
            const auto& source = it.value()["source"];
            if (source.value("kind", std::string{}) != "folder") continue;
            const fs::path folder = fs::path(info.asset_dir_path()) / source.value("path", it.key());
            const fs::path cache_anim_root = fs::path("cache") / info.name / "animations" / it.key();
            const int frame_count = std::max(count_sequential_png(folder), count_sequential_png(cache_anim_root));
            CacheManager::BundleAnimation bundle_anim;
            bundle_anim.name = it.key();
            for (int idx = 0; idx < frame_count; ++idx) {
                CacheManager::BundleFrame frame;
                frame.base_layer = load_layer_from_candidates({
                    cache_anim_root / (std::to_string(idx) + ".png"),
                    folder / (std::to_string(idx) + ".png"),
                });
                if (frame.base_layer.empty()) frame.base_layer = make_bad_asset_layer();
                bundle_anim.frames.push_back(std::move(frame));
            }
            bundle.animations.push_back(std::move(bundle_anim));
        }
    }
    const fs::path bundle_path = fs::path("cache") / info.name / "bundle.bin";
    return CacheManager::save_bundle(bundle_path.generic_string(), bundle);
}

bool PrimaryAssetCache::populate_runtime_frames(const AssetInfo& info,
                                                const CacheManager::BundleData& bundle,
                                                std::unordered_map<std::string, PrebuiltAnimationFrames>& out_frames,
                                                const std::unordered_set<std::string>* animation_filter) {
    if (!renderer_) return false;
    out_frames.clear();

    for (const auto& anim : bundle.animations) {
        if (animation_filter && !animation_filter->empty() &&
            animation_filter->find(anim.name) == animation_filter->end())
            continue;
        if (anim.frames.empty()) continue;

        PrebuiltAnimationFrames prepared;
        prepared.frames.reserve(anim.frames.size());
        int canvas_width = 0, canvas_height = 0;

        for (std::size_t frame_idx = 0; frame_idx < anim.frames.size(); ++frame_idx) {
            Animation::FrameCache cache_entry{};
            const auto& frame = anim.frames[frame_idx];

            if (!frame.base_layer.empty()) {
                SDL_Surface* base_surface = surface_from_layer(frame.base_layer);
                CacheManager::TextureUploadOptions upload_opts;
                upload_opts.semantic = CacheManager::TextureSemantic::Color;
                upload_opts.enable_mipmaps = frame.base_layer.width >= 128 && frame.base_layer.height >= 128;
                cache_entry.texture = CacheManager::surface_to_texture(renderer_, base_surface, upload_opts);
                if (base_surface) SDL_DestroySurface(base_surface);
                if (!info.smooth_scaling && cache_entry.texture) {
                    SDL_SetTextureScaleMode(cache_entry.texture, SDL_SCALEMODE_NEAREST);
                }
                cache_entry.width = frame.base_layer.width;
                cache_entry.height = frame.base_layer.height;
                cache_entry.source_rect = {0, 0, frame.base_layer.width, frame.base_layer.height};

                if (frame_idx == 0) {
                    canvas_width = frame.base_layer.width;
                    canvas_height = frame.base_layer.height;
                }
            }
            prepared.frames.push_back(std::move(cache_entry));
        }

        prepared.canvas_width = canvas_width;
        prepared.canvas_height = canvas_height;
#if !defined(NDEBUG)
        vibble::log::debug("[PrimaryAssetCache] Lazily loaded " + std::to_string(prepared.frames.size()) +
                           " resident texture(s) for " + info.name + "::" + anim.name + ".");
#endif
        out_frames[anim.name] = std::move(prepared);
    }
    return !out_frames.empty();
}