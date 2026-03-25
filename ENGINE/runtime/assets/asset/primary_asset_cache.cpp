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
#include <initializer_list>
#include <limits>

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

    // Keep path in the signature to preserve differentiation for similarly-sized files.
    hash = fnv1a64(path.generic_string(), hash);

    const auto size = fs::file_size(path, ec);
    if (!ec) {
        hash = fnv1a64(&size, sizeof(size), hash);
    }

    auto hash_window = [&](std::ifstream& in,
                           std::uintmax_t offset,
                           std::size_t byte_count,
                           std::uint64_t& target_hash) -> bool {
        if (byte_count == 0) {
            return false;
        }
        in.clear();
        in.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
        if (!in.good()) {
            return false;
        }

        std::array<char, 4096> buffer{};
        std::size_t remaining = byte_count;
        bool hashed_any = false;
        while (remaining > 0 && in.good()) {
            const std::size_t want = std::min(buffer.size(), remaining);
            in.read(buffer.data(), static_cast<std::streamsize>(want));
            const std::streamsize got = in.gcount();
            if (got <= 0) {
                break;
            }
            target_hash = fnv1a64(buffer.data(), static_cast<std::size_t>(got), target_hash);
            remaining -= static_cast<std::size_t>(got);
            hashed_any = true;
            if (static_cast<std::size_t>(got) < want) {
                break;
            }
        }
        return hashed_any;
    };

    // Use sampled content signatures instead of full-byte scans so
    // cache rewrites with identical bytes keep the same hash without
    // forcing large synchronous disk reads during map load.
    std::ifstream in(path, std::ios::binary);
    if (in) {
        constexpr std::uintmax_t kFullReadLimitBytes = 32 * 1024;
        constexpr std::uintmax_t kWindowBytes = 4 * 1024;

        bool hashed = false;
        if (!ec && size <= kFullReadLimitBytes) {
            const std::size_t bytes_to_hash =
                static_cast<std::size_t>(std::min<std::uintmax_t>(size, std::numeric_limits<std::size_t>::max()));
            hashed = hash_window(in, 0, bytes_to_hash, hash);
        } else if (!ec) {
            const std::uintmax_t head_offset = 0;
            const std::uintmax_t mid_center = size / 2;
            const std::uintmax_t mid_offset =
                (mid_center > (kWindowBytes / 2)) ? (mid_center - (kWindowBytes / 2)) : 0;
            const std::uintmax_t tail_offset = (size > kWindowBytes) ? (size - kWindowBytes) : 0;

            const std::array<std::uintmax_t, 3> offsets{head_offset, mid_offset, tail_offset};
            std::array<std::uintmax_t, 3> lengths{
                std::min<std::uintmax_t>(kWindowBytes, size),
                0,
                0,
            };
            lengths[1] = (mid_offset < size) ? std::min<std::uintmax_t>(kWindowBytes, size - mid_offset) : 0;
            lengths[2] = (tail_offset < size) ? std::min<std::uintmax_t>(kWindowBytes, size - tail_offset) : 0;

            std::array<std::uintmax_t, 3> seen_offsets{std::numeric_limits<std::uintmax_t>::max(),
                                                       std::numeric_limits<std::uintmax_t>::max(),
                                                       std::numeric_limits<std::uintmax_t>::max()};
            std::size_t seen_count = 0;
            for (std::size_t idx = 0; idx < offsets.size(); ++idx) {
                const std::uintmax_t off = offsets[idx];
                if (lengths[idx] == 0 || off >= size) {
                    continue;
                }
                bool duplicate = false;
                for (std::size_t s = 0; s < seen_count; ++s) {
                    if (seen_offsets[s] == off) {
                        duplicate = true;
                        break;
                    }
                }
                if (duplicate) {
                    continue;
                }
                seen_offsets[seen_count++] = off;

                const std::size_t bytes_to_hash = static_cast<std::size_t>(
                    std::min<std::uintmax_t>(lengths[idx], std::numeric_limits<std::size_t>::max()));
                hashed = hash_window(in, off, bytes_to_hash, hash) || hashed;
            }
        }
        if (hashed && !in.bad()) {
            return;
        }
    }

    // Fallback only if content read failed.
    ec.clear();
    const auto ftime = fs::last_write_time(path, ec);
    if (!ec) {
        const auto stamp = ftime.time_since_epoch().count();
        hash = fnv1a64(&stamp, sizeof(stamp), hash);
    }
}

bool path_newer_than(const fs::path& path, fs::file_time_type baseline) {
    std::error_code ec;
    if (!fs::exists(path, ec) || ec) {
        return false;
    }
    const auto t = fs::last_write_time(path, ec);
    if (ec) {
        return false;
    }
    return t > baseline;
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
    if (!rgba) {
        return layer;
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

CacheManager::BundleFrameLayer make_transparent_layer(int width, int height) {
    CacheManager::BundleFrameLayer layer;
    layer.width = std::max(1, width);
    layer.height = std::max(1, height);
    layer.format = SDL_PIXELFORMAT_RGBA8888;
    layer.pitch = layer.width * 4;
    const std::size_t byte_count =
        static_cast<std::size_t>(layer.pitch) * static_cast<std::size_t>(layer.height);
    layer.pixels.assign(byte_count, static_cast<std::uint8_t>(0));
    return layer;
}

CacheManager::BundleFrame make_placeholder_frame(const std::vector<float>& variant_steps) {
    CacheManager::BundleFrame frame;
    frame.variants.reserve(variant_steps.empty() ? 1u : variant_steps.size());
    const std::size_t variant_count = variant_steps.empty() ? 1u : variant_steps.size();
    for (std::size_t idx = 0; idx < variant_count; ++idx) {
        CacheManager::BundleFrameVariant variant;
        variant.base = make_transparent_layer(1, 1);
        frame.variants.push_back(std::move(variant));
    }
    return frame;
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

std::vector<float> normalized_variant_steps(const AssetInfo& /*info*/) {
    std::vector<float> steps = render_pipeline::ScalingLogic::DefaultScaleSteps();
    steps.erase(std::remove_if(steps.begin(), steps.end(), [](float v) { return !(v > 0.0f) || !std::isfinite(v); }), steps.end());
    std::sort(steps.begin(), steps.end(), std::greater<float>());
    steps.erase(std::unique(steps.begin(), steps.end(), [](float a, float b) { return std::fabs(a - b) < 1e-4f; }), steps.end());
    if (steps.empty()) {
        steps = render_pipeline::ScalingLogic::DefaultScaleSteps();
    }
    return steps;
}

bool steps_match_canonical(const std::vector<float>& steps) {
    const auto& canonical = render_pipeline::ScalingLogic::DefaultScaleSteps();
    if (steps.size() != canonical.size()) {
        return false;
    }
    for (std::size_t idx = 0; idx < canonical.size(); ++idx) {
        if (!std::isfinite(steps[idx])) {
            return false;
        }
        if (std::fabs(steps[idx] - canonical[idx]) > 1e-4f) {
            return false;
        }
    }
    return true;
}

bool bundle_variant_layout_is_valid(const CacheManager::BundleData& bundle) {
    const std::size_t expected_variant_count = render_pipeline::ScalingLogic::DefaultScaleSteps().size();
    for (const auto& animation : bundle.animations) {
        if (animation.frames.empty()) {
            continue;
        }
        if (!steps_match_canonical(animation.variant_steps)) {
            return false;
        }
        for (const auto& frame : animation.frames) {
            if (frame.variants.size() != expected_variant_count) {
                return false;
            }
        }
    }
    return true;
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

CacheManager::BundleFrameLayer load_layer_from_candidates(std::initializer_list<fs::path> candidates) {
    for (const auto& candidate : candidates) {
        std::error_code ec;
        if (!fs::exists(candidate, ec) || ec) {
            continue;
        }
        if (SDL_Surface* surface = load_rgba_surface(candidate)) {
            auto layer = make_layer(surface);
            SDL_DestroySurface(surface);
            return layer;
        }
    }
    return CacheManager::BundleFrameLayer{};
}

fs::path cache_variant_root(const fs::path& animation_cache_root,
                            const std::vector<float>& variant_steps,
                            std::size_t variant_idx) {
    return fs::path(render_pipeline::ScalingLogic::VariantFolder(
        animation_cache_root.string(),
        variant_steps,
        variant_idx));
}

bool animation_is_selected(const std::string& animation_name,
                           const std::unordered_set<std::string>* animation_filter) {
    if (!animation_filter || animation_filter->empty()) {
        return true;
    }
    return animation_filter->find(animation_name) != animation_filter->end();
}

} // namespace

PrimaryAssetCache::PrimaryAssetCache(SDL_Renderer* renderer) : renderer_(renderer) {}

std::uint64_t PrimaryAssetCache::compute_hash(const AssetInfo& info,
                                              const std::unordered_set<std::string>* animation_filter) const {
    std::uint64_t hash = fnv1a64(info.info_json_.dump());
    const auto canonical_steps = render_pipeline::ScalingLogic::DefaultScaleSteps();

    if (info.anims_json_.is_object()) {
        for (auto it = info.anims_json_.begin(); it != info.anims_json_.end(); ++it) {
            if (!it.value().is_object()) continue;
            if (!animation_is_selected(it.key(), animation_filter)) continue;
            const nlohmann::json& anim_json = it.value();
            if (!anim_json.contains("source") || !anim_json["source"].is_object()) {
                continue;
            }
            const auto& source = anim_json["source"];
            if (source.value("kind", std::string{}) != "folder") {
                continue;
            }
            fs::path folder = fs::path(info.asset_dir_path()) / source.value("path", it.key());
            const fs::path cache_anim_root = fs::path("cache") / info.name / "animations" / it.key();
            const fs::path cache_scale_100 = cache_anim_root / "scale_100";
            const fs::path cache_normal_100 = cache_scale_100 / "normal";
            const fs::path cache_foreground_100 = cache_scale_100 / "foreground";
            const fs::path cache_background_100 = cache_scale_100 / "background";
            const int source_frame_count = count_sequential_png(folder);
            const int cached_frame_count = count_sequential_png(cache_normal_100);
            const int frame_count = std::max(source_frame_count, cached_frame_count);
            for (int i = 0; i < frame_count; ++i) {
                fs::path frame_path = folder / (std::to_string(i) + ".png");
                hash_file_signature(frame_path, hash);

                const std::string frame_name = std::to_string(i) + ".png";
                hash_file_signature(folder / "foreground" / frame_name, hash);
                hash_file_signature(folder / "background" / frame_name, hash);
                hash_file_signature(cache_normal_100 / frame_name, hash);
                hash_file_signature(cache_foreground_100 / frame_name, hash);
                hash_file_signature(cache_background_100 / frame_name, hash);

                for (std::size_t variant_idx = 0; variant_idx < canonical_steps.size(); ++variant_idx) {
                    const fs::path variant_root = cache_variant_root(cache_anim_root,
                                                                     canonical_steps,
                                                                     variant_idx);
                    hash_file_signature(variant_root / "normal" / frame_name, hash);
                    hash_file_signature(variant_root / "foreground" / frame_name, hash);
                    hash_file_signature(variant_root / "background" / frame_name, hash);
                }
            }
        }
    }
    return hash;
}

bool PrimaryAssetCache::inputs_newer_than_bundle(const AssetInfo& info,
                                                 const fs::path& bundle_path,
                                                 const std::unordered_set<std::string>* animation_filter) const {
    std::error_code ec;
    const auto bundle_time = fs::last_write_time(bundle_path, ec);
    if (ec) {
        return true;
    }

    const auto canonical_steps = render_pipeline::ScalingLogic::DefaultScaleSteps();
    if (!info.anims_json_.is_object()) {
        return false;
    }

    for (auto it = info.anims_json_.begin(); it != info.anims_json_.end(); ++it) {
        if (!it.value().is_object()) continue;
        if (!animation_is_selected(it.key(), animation_filter)) continue;
        const nlohmann::json& anim_json = it.value();
        if (!anim_json.contains("source") || !anim_json["source"].is_object()) {
            continue;
        }
        const auto& source = anim_json["source"];
        if (source.value("kind", std::string{}) != "folder") {
            continue;
        }

        const fs::path folder = fs::path(info.asset_dir_path()) / source.value("path", it.key());
        const fs::path cache_anim_root = fs::path("cache") / info.name / "animations" / it.key();
        const fs::path cache_scale_100 = cache_anim_root / "scale_100";
        const fs::path cache_normal_100 = cache_scale_100 / "normal";
        const fs::path cache_foreground_100 = cache_scale_100 / "foreground";
        const fs::path cache_background_100 = cache_scale_100 / "background";

        const int source_frame_count = count_sequential_png(folder);
        const int cached_frame_count = count_sequential_png(cache_normal_100);
        const int frame_count = std::max(source_frame_count, cached_frame_count);
        for (int i = 0; i < frame_count; ++i) {
            const std::string frame_name = std::to_string(i) + ".png";
            if (path_newer_than(folder / frame_name, bundle_time)) return true;
            if (path_newer_than(folder / "foreground" / frame_name, bundle_time)) return true;
            if (path_newer_than(folder / "background" / frame_name, bundle_time)) return true;
            if (path_newer_than(cache_normal_100 / frame_name, bundle_time)) return true;
            if (path_newer_than(cache_foreground_100 / frame_name, bundle_time)) return true;
            if (path_newer_than(cache_background_100 / frame_name, bundle_time)) return true;

            for (std::size_t variant_idx = 0; variant_idx < canonical_steps.size(); ++variant_idx) {
                const fs::path variant_root = cache_variant_root(cache_anim_root,
                                                                 canonical_steps,
                                                                 variant_idx);
                if (path_newer_than(variant_root / "normal" / frame_name, bundle_time)) return true;
                if (path_newer_than(variant_root / "foreground" / frame_name, bundle_time)) return true;
                if (path_newer_than(variant_root / "background" / frame_name, bundle_time)) return true;
            }
        }
    }

    return false;
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

bool PrimaryAssetCache::build_bundle_from_sources(const AssetInfo& info,
                                                  CacheManager::BundleData& out_data,
                                                  const std::unordered_set<std::string>* animation_filter) {
    out_data = CacheManager::BundleData{};
    out_data.version = 1;
    out_data.metadata_snapshot = info.info_json_;

    std::vector<float> variant_steps = normalized_variant_steps(info);
    if (info.anims_json_.is_object()) {
        for (auto it = info.anims_json_.begin(); it != info.anims_json_.end(); ++it) {
            if (!it.value().is_object()) continue;
            if (!animation_is_selected(it.key(), animation_filter)) continue;
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
            const fs::path cache_anim_root = fs::path("cache") / info.name / "animations" / bundle_anim.name;
            const fs::path cache_scale_100 = cache_anim_root / "scale_100";
            const fs::path cache_normal_100 = cache_scale_100 / "normal";
            const fs::path cache_foreground_100 = cache_scale_100 / "foreground";
            const fs::path cache_background_100 = cache_scale_100 / "background";
            const int source_frame_count = count_sequential_png(folder);
            const int cached_frame_count = count_sequential_png(cache_normal_100);
            const int frame_count = std::max(source_frame_count, cached_frame_count);
            if (frame_count <= 0) {
                vibble::log::warn("[PrimaryAssetCache] No source frames found for " + info.name +
                                  "::" + bundle_anim.name + " in '" + folder.generic_string() +
                                  "'; injecting a transparent placeholder frame.");
                bundle_anim.frames.push_back(make_placeholder_frame(bundle_anim.variant_steps));
                out_data.animations.push_back(std::move(bundle_anim));
                continue;
            }

            const fs::path fg_folder = folder / "foreground";
            const fs::path bg_folder = folder / "background";

            bundle_anim.frames.reserve(static_cast<std::size_t>(frame_count));
            bool warned_missing_base_frame = false;

            for (int frame_idx = 0; frame_idx < frame_count; ++frame_idx) {
                CacheManager::BundleFrame frame;
                frame.variants.reserve(bundle_anim.variant_steps.size());
                const std::string frame_name = std::to_string(frame_idx) + ".png";

                CacheManager::BundleFrameLayer base_layer = load_layer_from_candidates({
                    cache_normal_100 / frame_name,
                    folder / frame_name,
                });
                if (base_layer.empty()) {
                    if (!warned_missing_base_frame) {
                        vibble::log::warn("[PrimaryAssetCache] Missing base frame data for " + info.name +
                                          "::" + bundle_anim.name +
                                          "; injecting transparent fallback for unavailable frame(s).");
                        warned_missing_base_frame = true;
                    }
                    base_layer = make_transparent_layer(1, 1);
                }
                CacheManager::BundleFrameLayer fg_layer = load_layer_from_candidates({
                    cache_foreground_100 / frame_name,
                    fg_folder / frame_name,
                });
                CacheManager::BundleFrameLayer bg_layer = load_layer_from_candidates({
                    cache_background_100 / frame_name,
                    bg_folder / frame_name,
                });

                for (std::size_t variant_idx = 0; variant_idx < bundle_anim.variant_steps.size(); ++variant_idx) {
                    const float step = bundle_anim.variant_steps[variant_idx];
                    CacheManager::BundleFrameVariant variant;
                    const fs::path variant_root = cache_variant_root(cache_anim_root,
                                                                     bundle_anim.variant_steps,
                                                                     variant_idx);
                    variant.base = load_layer_from_candidates({
                        variant_root / "normal" / frame_name,
                    });
                    if (variant.base.empty()) {
                        variant.base = scale_layer(base_layer, step);
                    }
                    if (variant.base.empty()) {
                        variant.base = make_transparent_layer(1, 1);
                    }

                    variant.foreground = load_layer_from_candidates({
                        variant_root / "foreground" / frame_name,
                    });
                    if (variant.foreground.empty() && !fg_layer.empty()) {
                        variant.foreground = scale_layer(fg_layer, step);
                    }

                    variant.background = load_layer_from_candidates({
                        variant_root / "background" / frame_name,
                    });
                    if (variant.background.empty() && !bg_layer.empty()) {
                        variant.background = scale_layer(bg_layer, step);
                    }
                    frame.variants.push_back(std::move(variant));
                }

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
                                                std::unordered_map<std::string, PrebuiltAnimationFrames>& out_frames,
                                                const std::unordered_set<std::string>* animation_filter) {
    if (!renderer_) {
        return false;
    }
    auto apply_texture_scale_mode = [&info](SDL_Texture* texture) {
        if (!texture) {
            return;
        }
        SDL_SetTextureScaleMode(texture, info.smooth_scaling ? SDL_SCALEMODE_LINEAR : SDL_SCALEMODE_NEAREST);
    };

    out_frames.clear();
    for (const auto& anim : bundle.animations) {
        if (animation_filter && !animation_filter->empty() &&
            animation_filter->find(anim.name) == animation_filter->end()) {
            continue;
        }
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
                    apply_texture_scale_mode(atlas_tex);
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
                    apply_texture_scale_mode(tex);
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
                    apply_texture_scale_mode(cache_entry.foreground_textures[variant_idx]);
                    SDL_DestroySurface(fg_surface);
                }

                if (!variant.background.empty()) {
                    SDL_Surface* bg_surface = surface_from_layer(variant.background);
                    cache_entry.background_textures[variant_idx] = CacheManager::surface_to_texture(renderer_, bg_surface);
                    apply_texture_scale_mode(cache_entry.background_textures[variant_idx]);
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
                                      CacheManager::BundleData& raw_bundle,
                                      const std::unordered_set<std::string>* animation_filter) {
    const fs::path bundle_path = fs::path("cache") / info.name / "bundle.bin";
    const bool has_animation_filter = animation_filter && !animation_filter->empty();
    bool has_expected_hash = false;
    std::uint64_t expected_hash = 0;
    auto get_expected_hash = [&]() -> std::uint64_t {
        if (!has_expected_hash) {
            expected_hash = compute_hash(info, animation_filter);
            has_expected_hash = true;
        }
        return expected_hash;
    };
    bool has_full_expected_hash = false;
    std::uint64_t full_expected_hash = 0;
    auto get_full_expected_hash = [&]() -> std::uint64_t {
        if (!has_full_expected_hash) {
            full_expected_hash = compute_hash(info, nullptr);
            has_full_expected_hash = true;
        }
        return full_expected_hash;
    };

    auto try_populate = [&](const CacheManager::BundleData& bundle) {
        out_frames.clear();
        return populate_runtime_frames(info, bundle, out_frames, animation_filter);
    };
    auto bundle_contains_required_folder_animations = [&](const CacheManager::BundleData& bundle) {
        if (!info.anims_json_.is_object()) {
            return true;
        }

        std::unordered_set<std::string> bundle_animation_names;
        bundle_animation_names.reserve(bundle.animations.size());
        for (const auto& anim : bundle.animations) {
            if (anim.name.empty() || anim.frames.empty()) {
                continue;
            }
            bundle_animation_names.insert(anim.name);
        }

        for (auto it = info.anims_json_.begin(); it != info.anims_json_.end(); ++it) {
            if (!it.value().is_object()) continue;
            if (!animation_is_selected(it.key(), animation_filter)) continue;

            const nlohmann::json& anim_json = it.value();
            if (!anim_json.contains("source") || !anim_json["source"].is_object()) {
                continue;
            }
            const auto& source = anim_json["source"];
            if (source.value("kind", std::string{}) != "folder") {
                continue;
            }
            if (bundle_animation_names.find(it.key()) == bundle_animation_names.end()) {
                return false;
            }
        }

        return true;
    };

    CacheManager::BundleData bundle;
    const bool bundle_loaded = CacheManager::load_bundle(bundle_path.generic_string(), bundle);
    if (bundle_loaded) {
        const bool covers_required_animations =
            bundle_contains_required_folder_animations(bundle);
        const bool variant_layout_ok = bundle_variant_layout_is_valid(bundle);
        const bool populated = try_populate(bundle);
        const bool reusable_bundle = populated && variant_layout_ok && covers_required_animations;
        bool hash_ok = false;
        if (bundle.content_hash != 0) {
            const std::uint64_t expected = get_expected_hash();
            hash_ok = (bundle.content_hash == expected);
            if (!hash_ok && has_animation_filter) {
                // A filtered request can reuse a fully-baked bundle.
                hash_ok = (bundle.content_hash == get_full_expected_hash());
            }
        }

        if (reusable_bundle && hash_ok) {
            raw_bundle = bundle;
            return true;
        }

        if (reusable_bundle && !hash_ok) {
            const bool inputs_are_newer = inputs_newer_than_bundle(info, bundle_path, animation_filter);
            if (!inputs_are_newer) {
                if (!has_animation_filter) {
                    const std::uint64_t canonical_hash = get_expected_hash();
                    if (bundle.content_hash != canonical_hash) {
                        if (!CacheManager::update_bundle_content_hash(bundle_path.generic_string(), canonical_hash)) {
                            vibble::log::warn("[PrimaryAssetCache] Failed to refresh bundle hash for " + info.name + ".");
                        } else {
                            bundle.content_hash = canonical_hash;
                        }
                    }
                }
                raw_bundle = bundle;
                return true;
            }
        }

        if (!covers_required_animations) {
            vibble::log::info("[PrimaryAssetCache] Rebuilding stale bundle cache for " + info.name +
                              " (missing required folder animation entries).");
        } else if (!hash_ok) {
            vibble::log::info("[PrimaryAssetCache] Rebuilding stale bundle cache for " + info.name +
                              " (content hash mismatch).");
        } else if (!variant_layout_ok) {
            vibble::log::info("[PrimaryAssetCache] Rebuilding stale bundle cache for " + info.name +
                              " (missing full-resolution or inconsistent variant metadata).");
        } else {
            vibble::log::warn("[PrimaryAssetCache] Cached bundle for " + info.name +
                              " could not populate requested runtime frames; rebuilding from source.");
        }
    } else {
        vibble::log::info("[PrimaryAssetCache] Missing cached bundle for " + info.name +
                          "; building cache from source frames.");
    }

    CacheManager::BundleData rebuilt;
    if (!build_bundle_from_sources(info, rebuilt, animation_filter)) {
        vibble::log::warn("[PrimaryAssetCache] Failed to build bundle cache from source for " + info.name + ".");
        return false;
    }

    rebuilt.content_hash = get_expected_hash();
    // Never persist filtered bundle builds; they contain only a subset of animations.
    const bool should_persist_rebuilt_bundle = !has_animation_filter;
    if (should_persist_rebuilt_bundle) {
        if (!CacheManager::save_bundle(bundle_path.generic_string(), rebuilt)) {
            vibble::log::warn("[PrimaryAssetCache] Failed to save rebuilt bundle cache for " + info.name + ".");
        }
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
