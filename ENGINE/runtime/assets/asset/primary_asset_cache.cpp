#include "primary_asset_cache.hpp"

#include "asset_info.hpp"
#include "rendering/render/scaling_logic.hpp"
#include "image_cache_generator.hpp"
#include "utils/log.hpp"

#include <SDL3_image/SDL_image.h>
#include <filesystem>
#include <unordered_set>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <initializer_list>
#include <limits>
#include <unordered_map>
#include <optional>

namespace fs = std::filesystem;

namespace {

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

std::optional<int> parse_frame_index_from_filename(const fs::path& path) {
    try {
        return std::stoi(path.stem().string());
    } catch (...) {
        return std::nullopt;
    }
}

std::uint8_t variant_mask_for_directory(const std::string& variant_dir) {
    if (variant_dir == "normal") {
        return AssetInfo::kTextureVariantNormal;
    }
    if (variant_dir == "foreground") {
        return AssetInfo::kTextureVariantForeground;
    }
    if (variant_dir == "background") {
        return AssetInfo::kTextureVariantBackground;
    }
    return AssetInfo::kTextureVariantNone;
}

void record_load_repairs_from_written_files(AssetInfo& info, const std::vector<fs::path>& written_files) {
    const fs::path cache_root = fs::path("cache");
    const fs::path asset_root = cache_root / info.name / "animations";
    for (const auto& file_path : written_files) {
        std::error_code ec;
        if (!fs::exists(file_path, ec) || ec) {
            continue;
        }

        const fs::path normalized = file_path.lexically_normal();
        auto rel = normalized.lexically_relative(asset_root);
        const std::string rel_text = rel.generic_string();
        if (rel.empty() || rel_text == ".." || rel_text.rfind("../", 0) == 0) {
            continue;
        }

        std::vector<std::string> parts;
        parts.reserve(8);
        for (const auto& part : rel) {
            parts.push_back(part.string());
        }
        if (parts.size() < 4) {
            continue;
        }

        const std::string animation_name = parts[0];
        const std::string variant_name = parts[2];
        const auto frame_index = parse_frame_index_from_filename(parts.back());
        const std::uint8_t variants = variant_mask_for_directory(variant_name);
        if (animation_name.empty() || !frame_index.has_value() || variants == AssetInfo::kTextureVariantNone) {
            continue;
        }
        info.mark_texture_frame_rebuild_on_load(animation_name, *frame_index, variants);
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

bool PrimaryAssetCache::repair_missing_cache_files(
    AssetInfo& info,
    const std::unordered_set<std::string>* animation_filter) const {
    if (info.name.empty()) {
        return true;
    }

    imgcache::GeneratorOptions options;
#if defined(PROJECT_ROOT)
    const fs::path manifest_path = fs::path(PROJECT_ROOT) / "manifest.json";
    std::error_code manifest_ec;
    if (fs::exists(manifest_path, manifest_ec) && !manifest_ec) {
        options.manifest_path = manifest_path;
    }
#endif
    options.missing_only = true;
    options.force_rebuild = false;
    options.quiet_task_logs = true;
    options.effects_backend = imgcache::EffectsBackend::Cpu;
    options.filters.assets.insert(info.name);
    if (animation_filter && !animation_filter->empty()) {
        options.filters.animations = *animation_filter;
    }

    GeneratorLogBridge logger;
    auto result = imgcache::ImageCacheGenerator::Run(options, logger);
    if (!result.ok) {
        vibble::log::warn("[PrimaryAssetCache] Missing-file cache repair failed for " + info.name +
                          ": " + result.error);
        return false;
    }

    if (!result.written_files.empty()) {
        record_load_repairs_from_written_files(info, result.written_files);
        info.consume_pending_texture_rebuild_on_load();
    }
    return true;
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

    // Load-time repair only fills missing files and does not compare cache freshness.
    repair_missing_cache_files(info, animation_filter);

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
        if (reusable_bundle) {
            raw_bundle = bundle;
            return true;
        }

        if (!covers_required_animations) {
            vibble::log::info("[PrimaryAssetCache] Rebuilding bundle cache for " + info.name +
                              " (missing required folder animation entries).");
        } else if (!variant_layout_ok) {
            vibble::log::info("[PrimaryAssetCache] Rebuilding bundle cache for " + info.name +
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

    // Content hash is preserved as a compatibility-reserved field only.
    rebuilt.content_hash = 0;
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
    bundle.content_hash = 0;
    const fs::path bundle_path = fs::path("cache") / info.name / "bundle.bin";
    return CacheManager::save_bundle(bundle_path.generic_string(), bundle);
}
