#include "primary_asset_cache.hpp"

#include "asset_info.hpp"
#include "rendering/render/scaling_logic.hpp"
#include "utils/log.hpp"

#include <SDL3_image/SDL_image.h>
#include <filesystem>
#include <fstream>
#include <unordered_set>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <optional>

namespace fs = std::filesystem;

namespace {

std::uint64_t fnv1a64(const void* data, std::size_t len, std::uint64_t seed = 14695981039346656037ull) {
    const auto* bytes = static_cast<const unsigned char*>(data);
    std::uint64_t hash = seed;
    for (std::size_t i = 0; i < len; ++i) {
        hash ^= static_cast<std::uint64_t>(bytes[i]);
        hash *= 1099511628211ull;
    }
    return hash;
}

std::uint64_t fnv1a64(const std::string& text, std::uint64_t seed = 14695981039346656037ull) {
    return fnv1a64(text.data(), text.size(), seed);
}

void hash_file_signature(const fs::path& path, std::uint64_t& hash) {
    std::error_code ec;
    if (!fs::exists(path, ec) || ec) {
        return;
    }

    hash = fnv1a64(path.generic_string(), hash);
    const auto ftime = fs::last_write_time(path, ec);
    if (!ec) {
        const auto stamp = ftime.time_since_epoch().count();
        hash = fnv1a64(&stamp, sizeof(stamp), hash);
    }

    ec.clear();
    const auto size = fs::file_size(path, ec);
    if (!ec) {
        hash = fnv1a64(&size, sizeof(size), hash);
    }
}

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
    SDL_Surface* converted = SDL_ConvertSurface(loaded, SDL_PIXELFORMAT_RGBA8888);
    SDL_DestroySurface(loaded);
    return converted;
}

CacheManager::BundleFrameLayer make_layer(SDL_Surface* surface) {
    CacheManager::BundleFrameLayer layer;
    if (!surface) {
        return layer;
    }
    SDL_Surface* rgba = surface;
    if (surface->format != SDL_PIXELFORMAT_RGBA8888) {
        rgba = SDL_ConvertSurface(surface, SDL_PIXELFORMAT_RGBA8888);
        SDL_DestroySurface(surface);
    }
    layer.width = rgba->w;
    layer.height = rgba->h;
    layer.pitch = rgba->pitch;
    layer.format = rgba ? rgba->format : SDL_PIXELFORMAT_RGBA8888;
    const std::size_t byte_count = static_cast<std::size_t>(layer.pitch) * static_cast<std::size_t>(layer.height);
    layer.pixels.resize(byte_count);
    std::memcpy(layer.pixels.data(), rgba->pixels, byte_count);
    if (rgba != surface) {
        SDL_DestroySurface(rgba);
    }
    return layer;
}

SDL_Surface* surface_from_layer(const CacheManager::BundleFrameLayer& layer) {
    if (layer.empty()) {
        return nullptr;
    }
    return SDL_CreateSurfaceFrom(layer.width,
                                 layer.height,
                                 static_cast<SDL_PixelFormat>(layer.format),
                                 const_cast<std::uint8_t*>(layer.pixels.data()),
                                 layer.pitch);
}

std::vector<float> normalized_variant_steps(const AssetInfo& info) {
    std::vector<float> steps = info.scale_variants;
    if (steps.empty()) {
        render_pipeline::ScalingLogic::NormalizeVariantSteps(steps);
    }
    if (std::find_if(steps.begin(), steps.end(), [](float v) { return std::fabs(v - 1.0f) < 1e-4f; }) == steps.end()) {
        steps.insert(steps.begin(), 1.0f);
    }
    steps.erase(std::remove_if(steps.begin(), steps.end(), [](float v) { return !(v > 0.0f) || !std::isfinite(v); }), steps.end());
    std::sort(steps.begin(), steps.end(), std::greater<float>());
    steps.erase(std::unique(steps.begin(), steps.end(), [](float a, float b) { return std::fabs(a - b) < 1e-4f; }), steps.end());
    if (steps.empty()) {
        steps.push_back(1.0f);
    }
    return steps;
}

CacheManager::BundleFrameLayer scale_layer(const CacheManager::BundleFrameLayer& src, float scale) {
    if (src.empty() || !(scale > 0.0f)) {
        return CacheManager::BundleFrameLayer{};
    }
    SDL_Surface* base = surface_from_layer(src);
    if (!base) {
        return CacheManager::BundleFrameLayer{};
    }
    SDL_Surface* scaled = render_pipeline::CreateScaledSurface(base, scale);
    SDL_DestroySurface(base);
    if (!scaled) {
        return CacheManager::BundleFrameLayer{};
    }
    auto layer = make_layer(scaled);
    SDL_DestroySurface(scaled);
    return layer;
}

struct UniformCropMargins {
    int left = 0;
    int top = 0;
    int right = 0;
    int bottom = 0;
};

bool compute_visible_margins(SDL_Surface* surface, UniformCropMargins& out) {
    if (!surface || surface->w <= 0 || surface->h <= 0 || !surface->pixels) {
        return false;
    }

    const bool locked = SDL_MUSTLOCK(surface);
    if (locked && !SDL_LockSurface(surface)) {
        return false;
    }

    const int width = surface->w;
    const int height = surface->h;
    const int pitch = surface->pitch;
    const auto* pixels = static_cast<const std::uint8_t*>(surface->pixels);

    int left = width;
    int top = height;
    int right = -1;
    int bottom = -1;

    for (int y = 0; y < height; ++y) {
        const std::uint8_t* row = pixels + static_cast<std::size_t>(y) * static_cast<std::size_t>(pitch);
        for (int x = 0; x < width; ++x) {
            const std::uint8_t alpha = row[static_cast<std::size_t>(x) * 4u + 3u];
            if (alpha == 0) {
                continue;
            }
            if (x < left) left = x;
            if (y < top) top = y;
            if (x > right) right = x;
            if (y > bottom) bottom = y;
        }
    }

    if (locked) {
        SDL_UnlockSurface(surface);
    }

    if (right < left || bottom < top) {
        return false;
    }

    out.left = left;
    out.top = top;
    out.right = std::max(0, width - (right + 1));
    out.bottom = std::max(0, height - (bottom + 1));
    return true;
}

SDL_Surface* crop_surface_with_margins(SDL_Surface* surface, const UniformCropMargins& margins) {
    if (!surface || surface->w <= 0 || surface->h <= 0) {
        return surface;
    }

    const int src_w = surface->w;
    const int src_h = surface->h;

    const int left = std::clamp(margins.left, 0, std::max(0, src_w - 1));
    const int top = std::clamp(margins.top, 0, std::max(0, src_h - 1));

    const int max_right = std::max(0, src_w - left - 1);
    const int max_bottom = std::max(0, src_h - top - 1);

    const int right = std::clamp(margins.right, 0, max_right);
    const int bottom = std::clamp(margins.bottom, 0, max_bottom);

    const int crop_w = src_w - left - right;
    const int crop_h = src_h - top - bottom;
    if (crop_w <= 0 || crop_h <= 0) {
        return surface;
    }
    if (left == 0 && top == 0 && crop_w == src_w && crop_h == src_h) {
        return surface;
    }

    SDL_Surface* cropped = SDL_CreateSurface(crop_w, crop_h, SDL_PIXELFORMAT_RGBA8888);
    if (!cropped) {
        return surface;
    }

    SDL_Rect src_rect{left, top, crop_w, crop_h};
    SDL_Rect dst_rect{0, 0, crop_w, crop_h};
    if (!SDL_BlitSurface(surface, &src_rect, cropped, &dst_rect)) {
        SDL_DestroySurface(cropped);
        return surface;
    }

    SDL_DestroySurface(surface);
    return cropped;
}

std::optional<UniformCropMargins> compute_uniform_crop_margins(const AssetInfo& info, const nlohmann::json& anims_json) {
    if (!info.crop_frames || !anims_json.is_object()) {
        return std::nullopt;
    }

    bool have_union = false;
    UniformCropMargins union_margins{};

    for (auto it = anims_json.begin(); it != anims_json.end(); ++it) {
        if (!it.value().is_object()) {
            continue;
        }
        const nlohmann::json& anim_json = it.value();
        if (!anim_json.contains("source") || !anim_json["source"].is_object()) {
            continue;
        }

        const auto& source = anim_json["source"];
        const std::string kind = source.value("kind", std::string{});
        if (kind != "folder") {
            continue;
        }

        const fs::path folder = fs::path(info.asset_dir_path()) / source.value("path", it.key());
        const fs::path fg_folder = folder / "foreground";
        const fs::path bg_folder = folder / "background";
        const int frame_count = count_sequential_png(folder);
        if (frame_count <= 0) {
            continue;
        }

        for (int frame_idx = 0; frame_idx < frame_count; ++frame_idx) {
            const std::string frame_name = std::to_string(frame_idx) + ".png";
            const std::array<fs::path, 3> layer_paths = {
                folder / frame_name,
                fg_folder / frame_name,
                bg_folder / frame_name
            };

            for (const fs::path& layer_path : layer_paths) {
                std::error_code ec;
                if (!fs::exists(layer_path, ec) || ec) {
                    continue;
                }

                SDL_Surface* layer_surface = load_rgba_surface(layer_path);
                if (!layer_surface) {
                    continue;
                }

                UniformCropMargins local{};
                const bool has_visible = compute_visible_margins(layer_surface, local);
                SDL_DestroySurface(layer_surface);
                if (!has_visible) {
                    continue;
                }

                if (!have_union) {
                    union_margins = local;
                    have_union = true;
                } else {
                    union_margins.left = std::min(union_margins.left, local.left);
                    union_margins.top = std::min(union_margins.top, local.top);
                    union_margins.right = std::min(union_margins.right, local.right);
                    union_margins.bottom = std::min(union_margins.bottom, local.bottom);
                }
            }
        }
    }

    if (!have_union) {
        return std::nullopt;
    }
    return union_margins;
}

} // namespace

PrimaryAssetCache::PrimaryAssetCache(SDL_Renderer* renderer) : renderer_(renderer) {}

std::uint64_t PrimaryAssetCache::compute_hash(const AssetInfo& info) const {
    std::uint64_t hash = fnv1a64(info.info_json_.dump());

    if (info.anims_json_.is_object()) {
        for (auto it = info.anims_json_.begin(); it != info.anims_json_.end(); ++it) {
            if (!it.value().is_object()) continue;
            const nlohmann::json& anim_json = it.value();
            if (!anim_json.contains("source") || !anim_json["source"].is_object()) {
                continue;
            }
            const auto& source = anim_json["source"];
            if (source.value("kind", std::string{}) != "folder") {
                continue;
            }
            fs::path folder = fs::path(info.asset_dir_path()) / source.value("path", it.key());
            const int frame_count = count_sequential_png(folder);
            for (int i = 0; i < frame_count; ++i) {
                fs::path frame_path = folder / (std::to_string(i) + ".png");
                hash_file_signature(frame_path, hash);

                const std::string frame_name = std::to_string(i) + ".png";
                hash_file_signature(folder / "foreground" / frame_name, hash);
                hash_file_signature(folder / "background" / frame_name, hash);
            }
        }
    }
    return hash;
}

bool PrimaryAssetCache::build_variant_atlases(CacheManager::BundleAnimation& animation,
                                              const fs::path& cache_root) const {
    if (animation.frames.empty() || animation.variant_steps.empty()) {
        return false;
    }

    std::error_code ec;
    fs::create_directories(cache_root, ec);

    const int padding = 2;
    const int max_width = 4096;
    animation.atlas_paths.assign(animation.variant_steps.size(), fs::path{});

    for (std::size_t variant_idx = 0; variant_idx < animation.variant_steps.size(); ++variant_idx) {
        int shelf_x = 0;
        int shelf_y = 0;
        int shelf_h = 0;
        int used_w = 0;
        std::vector<SDL_Rect> rects(animation.frames.size(), SDL_Rect{0, 0, 0, 0});

        for (std::size_t frame_idx = 0; frame_idx < animation.frames.size(); ++frame_idx) {
            if (variant_idx >= animation.frames[frame_idx].variants.size()) {
                continue;
            }
            const auto& layer = animation.frames[frame_idx].variants[variant_idx].base;
            if (layer.empty()) continue;
            if (shelf_x + layer.width + padding > max_width) {
                shelf_y += shelf_h + padding;
                shelf_x = 0;
                shelf_h = 0;
            }
            SDL_Rect rect{ shelf_x, shelf_y, layer.width, layer.height };
            rects[frame_idx] = rect;
            shelf_x += layer.width + padding;
            shelf_h = std::max(shelf_h, layer.height);
            used_w = std::max(used_w, rect.x + rect.w);
        }
        const int atlas_h = shelf_y + shelf_h;
        if (used_w <= 0 || atlas_h <= 0) {
            continue;
        }

        SDL_Surface* atlas = SDL_CreateSurface(used_w, atlas_h, SDL_PIXELFORMAT_RGBA8888);
        if (!atlas) {
            continue;
        }
        const SDL_PixelFormatDetails* atlas_fmt = SDL_GetPixelFormatDetails(atlas->format);
        SDL_Palette* atlas_palette = SDL_GetSurfacePalette(atlas);
        const Uint32 clear_color = atlas_fmt ? SDL_MapRGBA(atlas_fmt, atlas_palette, 0, 0, 0, 0) : 0;
        SDL_FillSurfaceRect(atlas, nullptr, clear_color);

        for (std::size_t frame_idx = 0; frame_idx < animation.frames.size(); ++frame_idx) {
            if (variant_idx >= animation.frames[frame_idx].variants.size()) continue;
            const auto& variant = animation.frames[frame_idx].variants[variant_idx];
            if (variant.base.empty()) continue;
            SDL_Surface* surf = surface_from_layer(variant.base);
            if (!surf) continue;
            SDL_Rect dest = rects[frame_idx];
            SDL_BlitSurface(surf, nullptr, atlas, &dest);
            SDL_DestroySurface(surf);
        }

        const fs::path atlas_path = cache_root / ("atlas_" + std::to_string(variant_idx) + ".png");
        if (IMG_SavePNG(atlas, atlas_path.generic_string().c_str()) == 0) {
            animation.atlas_paths[variant_idx] = atlas_path;
            animation.uses_atlas = true;
            for (std::size_t frame_idx = 0; frame_idx < animation.frames.size(); ++frame_idx) {
                if (variant_idx >= animation.frames[frame_idx].variants.size()) continue;
                animation.frames[frame_idx].variants[variant_idx].use_atlas = true;
                animation.frames[frame_idx].variants[variant_idx].atlas_rect = rects[frame_idx];
            }
        }
        SDL_DestroySurface(atlas);
    }

    return animation.uses_atlas;
}

bool PrimaryAssetCache::build_bundle_from_sources(const AssetInfo& info, CacheManager::BundleData& out_data) {
    out_data = CacheManager::BundleData{};
    out_data.version = 1;
    out_data.metadata_snapshot = info.info_json_;

    std::vector<float> variant_steps = normalized_variant_steps(info);
    const std::optional<UniformCropMargins> uniform_crop = compute_uniform_crop_margins(info, info.anims_json_);

    if (info.anims_json_.is_object()) {
        for (auto it = info.anims_json_.begin(); it != info.anims_json_.end(); ++it) {
            if (!it.value().is_object()) continue;
            const nlohmann::json& anim_json = it.value();
            if (!anim_json.contains("source") || !anim_json["source"].is_object()) {
                continue;
            }
            const auto& source = anim_json["source"];
            const std::string source_kind = source.value("kind", std::string{});
            CacheManager::BundleAnimation bundle_anim;
            bundle_anim.name = it.key();
            bundle_anim.variant_steps = variant_steps;

            if (source_kind != "folder") {
                out_data.animations.push_back(std::move(bundle_anim));
                continue;
            }

            const fs::path folder = fs::path(info.asset_dir_path()) / source.value("path", it.key());
            const int frame_count = count_sequential_png(folder);
            if (frame_count <= 0) {
                out_data.animations.push_back(std::move(bundle_anim));
                continue;
            }

            const fs::path fg_folder = folder / "foreground";
            const fs::path bg_folder = folder / "background";

            bundle_anim.frames.reserve(static_cast<std::size_t>(frame_count));

            for (int frame_idx = 0; frame_idx < frame_count; ++frame_idx) {
                CacheManager::BundleFrame frame;
                frame.variants.reserve(bundle_anim.variant_steps.size());

                const fs::path base_path = folder / (std::to_string(frame_idx) + ".png");
                SDL_Surface* base_surface = load_rgba_surface(base_path);

                SDL_Surface* fg_surface = nullptr;
                SDL_Surface* bg_surface = nullptr;
                if (fs::exists(fg_folder / (std::to_string(frame_idx) + ".png"))) {
                    fg_surface = load_rgba_surface(fg_folder / (std::to_string(frame_idx) + ".png"));
                }
                if (fs::exists(bg_folder / (std::to_string(frame_idx) + ".png"))) {
                    bg_surface = load_rgba_surface(bg_folder / (std::to_string(frame_idx) + ".png"));
                }

                if (uniform_crop.has_value()) {
                    base_surface = crop_surface_with_margins(base_surface, *uniform_crop);
                    fg_surface = crop_surface_with_margins(fg_surface, *uniform_crop);
                    bg_surface = crop_surface_with_margins(bg_surface, *uniform_crop);
                }

                CacheManager::BundleFrameLayer base_layer = make_layer(base_surface);
                CacheManager::BundleFrameLayer fg_layer = make_layer(fg_surface);
                CacheManager::BundleFrameLayer bg_layer = make_layer(bg_surface);

                for (float step : bundle_anim.variant_steps) {
                    CacheManager::BundleFrameVariant variant;
                    variant.base = scale_layer(base_layer, step);
                    if (!fg_layer.empty()) {
                        variant.foreground = scale_layer(fg_layer, step);
                    }
                    if (!bg_layer.empty()) {
                        variant.background = scale_layer(bg_layer, step);
                    }
                    frame.variants.push_back(std::move(variant));
                }

                if (base_surface) SDL_DestroySurface(base_surface);
                if (fg_surface) SDL_DestroySurface(fg_surface);
                if (bg_surface) SDL_DestroySurface(bg_surface);

                bundle_anim.frames.push_back(std::move(frame));
            }

            const bool atlas_requested = info.info_json_.value("cache_atlas", false);
            if (atlas_requested) {
                const fs::path cache_root = fs::path("cache") / info.name;
                build_variant_atlases(bundle_anim, cache_root);
            }

            out_data.animations.push_back(std::move(bundle_anim));
        }
    }

    return true;
}

bool PrimaryAssetCache::populate_runtime_frames(const AssetInfo& info,
                                                const CacheManager::BundleData& bundle,
                                                std::unordered_map<std::string, PrebuiltAnimationFrames>& out_frames) {
    if (!renderer_) {
        return false;
    }
    out_frames.clear();
    for (const auto& anim : bundle.animations) {
        if (anim.frames.empty()) {
            continue;
        }

        PrebuiltAnimationFrames prepared;
        prepared.uses_atlas = anim.uses_atlas;
        prepared.variant_steps = anim.variant_steps;
        if (prepared.variant_steps.empty()) {
            prepared.variant_steps = normalized_variant_steps(info);
        }
        const std::size_t frame_count = anim.frames.size();
        prepared.frames.reserve(frame_count);

        std::unordered_map<std::size_t, SDL_Texture*> atlas_by_variant;
        std::unordered_map<std::size_t, SDL_Point> atlas_sizes;
        if (anim.uses_atlas) {
            for (std::size_t idx = 0; idx < anim.atlas_paths.size(); ++idx) {
                if (anim.atlas_paths[idx].empty()) continue;
                SDL_Surface* atlas_surface = CacheManager::load_surface(anim.atlas_paths[idx].generic_string());
                if (!atlas_surface) continue;
                SDL_Texture* atlas_tex = CacheManager::surface_to_texture(renderer_, atlas_surface);
                if (atlas_tex) {
                    atlas_by_variant[idx] = atlas_tex;
                    atlas_sizes[idx] = SDL_Point{atlas_surface->w, atlas_surface->h};
                }
                SDL_DestroySurface(atlas_surface);
            }
        }

        for (std::size_t frame_idx = 0; frame_idx < frame_count; ++frame_idx) {
            Animation::FrameCache cache_entry;
            const auto& frame = anim.frames[frame_idx];
            const std::size_t variant_count = prepared.variant_steps.size();
            cache_entry.resize(variant_count);

            for (std::size_t variant_idx = 0; variant_idx < variant_count && variant_idx < frame.variants.size(); ++variant_idx) {
                const auto& variant = frame.variants[variant_idx];

                if (variant.use_atlas && atlas_by_variant.count(variant_idx)) {
                    cache_entry.textures[variant_idx] = atlas_by_variant[variant_idx];
                    cache_entry.widths[variant_idx] = variant.atlas_rect.w;
                    cache_entry.heights[variant_idx] = variant.atlas_rect.h;
                    cache_entry.uses_atlas[variant_idx] = true;
                    cache_entry.source_rects[variant_idx] = variant.atlas_rect;
                } else if (!variant.base.empty()) {
                    SDL_Surface* base_surface = surface_from_layer(variant.base);
                    SDL_Texture* tex = CacheManager::surface_to_texture(renderer_, base_surface);
                    cache_entry.textures[variant_idx] = tex;
                    cache_entry.widths[variant_idx] = variant.base.width;
                    cache_entry.heights[variant_idx] = variant.base.height;
                    cache_entry.source_rects[variant_idx] = SDL_Rect{0, 0, variant.base.width, variant.base.height};
                    cache_entry.uses_atlas[variant_idx] = false;
                    SDL_DestroySurface(base_surface);
                }

                if (!variant.foreground.empty()) {
                    SDL_Surface* fg_surface = surface_from_layer(variant.foreground);
                    cache_entry.foreground_textures[variant_idx] = CacheManager::surface_to_texture(renderer_, fg_surface);
                    SDL_DestroySurface(fg_surface);
                }

                if (!variant.background.empty()) {
                    SDL_Surface* bg_surface = surface_from_layer(variant.background);
                    cache_entry.background_textures[variant_idx] = CacheManager::surface_to_texture(renderer_, bg_surface);
                    SDL_DestroySurface(bg_surface);
                }
            }

            prepared.frames.push_back(std::move(cache_entry));
        }

        if (!prepared.frames.empty() && !prepared.frames[0].widths.empty()) {
            prepared.canvas_width = prepared.frames[0].widths[0];
            prepared.canvas_height = prepared.frames[0].heights[0];
        }

        out_frames[anim.name] = std::move(prepared);
    }
    return !out_frames.empty();
}

bool PrimaryAssetCache::load_or_build(AssetInfo& info,
                                      std::unordered_map<std::string, PrebuiltAnimationFrames>& out_frames,
                                      CacheManager::BundleData& raw_bundle) {
    const fs::path bundle_path = fs::path("cache") / info.name / "bundle.bin";
    const std::uint64_t expected_hash = compute_hash(info);

    auto try_populate = [&](const CacheManager::BundleData& bundle) {
        out_frames.clear();
        return populate_runtime_frames(info, bundle, out_frames);
    };

    CacheManager::BundleData bundle;
    const bool bundle_loaded = CacheManager::load_bundle(bundle_path.generic_string(), bundle);
    if (bundle_loaded) {
        const bool hash_matches = bundle.content_hash == expected_hash;
        const bool populated = try_populate(bundle);
        if (hash_matches && populated) {
            raw_bundle = bundle;
            return true;
        }

        if (!hash_matches) {
            vibble::log::info("[PrimaryAssetCache] Rebuilding stale bundle cache for " + info.name + ".");
        } else {
            vibble::log::warn("[PrimaryAssetCache] Cached bundle for " + info.name +
                              " could not populate runtime frames; rebuilding from source.");
        }
    } else {
        vibble::log::info("[PrimaryAssetCache] Missing cached bundle for " + info.name +
                          "; building cache from source frames.");
    }

    CacheManager::BundleData rebuilt;
    if (!build_bundle_from_sources(info, rebuilt)) {
        vibble::log::warn("[PrimaryAssetCache] Failed to build bundle cache from source for " + info.name + ".");
        return false;
    }

    rebuilt.content_hash = expected_hash;
    if (!CacheManager::save_bundle(bundle_path.generic_string(), rebuilt)) {
        vibble::log::warn("[PrimaryAssetCache] Failed to save rebuilt bundle cache for " + info.name + ".");
    }

    const bool populated = try_populate(rebuilt);
    raw_bundle = std::move(rebuilt);
    if (!populated) {
        vibble::log::warn("[PrimaryAssetCache] Rebuilt bundle cache for " + info.name +
                          " did not produce runtime frames.");
    }
    return populated;
}

bool PrimaryAssetCache::save_current(const AssetInfo& info) {
    CacheManager::BundleData bundle;
    if (!build_bundle_from_sources(info, bundle)) {
        return false;
    }
    bundle.content_hash = compute_hash(info);
    const fs::path bundle_path = fs::path("cache") / info.name / "bundle.bin";
    return CacheManager::save_bundle(bundle_path.generic_string(), bundle);
}
