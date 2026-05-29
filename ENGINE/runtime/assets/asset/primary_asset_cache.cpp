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

std::unordered_map<std::string, std::vector<fs::path>> group_written_files_by_asset(
    const std::vector<fs::path>& written_files) {
    std::unordered_map<std::string, std::vector<fs::path>> grouped;
    for (const auto& file_path : written_files) {
        const fs::path normalized = file_path.lexically_normal();
        std::vector<std::string> parts;
        parts.reserve(8);
        for (const auto& part : normalized) {
            parts.push_back(part.string());
        }
        auto cache_it = std::find(parts.begin(), parts.end(), "cache");
        if (cache_it == parts.end()) {
            continue;
        }
        const auto asset_it = std::next(cache_it);
        if (asset_it == parts.end() || asset_it->empty()) {
            continue;
        }
        grouped[*asset_it].push_back(file_path);
    }
    return grouped;
}

PrimaryAssetCache::WarmupOutcome warmup_outcome_from_bundle_state(bool bundle_loaded) {
    return bundle_loaded ? PrimaryAssetCache::WarmupOutcome::Rebuilt
                         : PrimaryAssetCache::WarmupOutcome::Created;
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
    if (row < 0 || row >= 7) {
        return 0;
    }

    switch (static_cast<char>(std::toupper(static_cast<unsigned char>(ch)))) {
    case 'A': {
        static constexpr std::uint8_t rows[7] = {0b01110, 0b10001, 0b10001, 0b11111, 0b10001, 0b10001, 0b10001};
        return rows[row];
    }
    case 'B': {
        static constexpr std::uint8_t rows[7] = {0b11110, 0b10001, 0b10001, 0b11110, 0b10001, 0b10001, 0b11110};
        return rows[row];
    }
    case 'D': {
        static constexpr std::uint8_t rows[7] = {0b11110, 0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b11110};
        return rows[row];
    }
    case 'E': {
        static constexpr std::uint8_t rows[7] = {0b11111, 0b10000, 0b10000, 0b11110, 0b10000, 0b10000, 0b11111};
        return rows[row];
    }
    case 'S': {
        static constexpr std::uint8_t rows[7] = {0b01111, 0b10000, 0b10000, 0b01110, 0b00001, 0b00001, 0b11110};
        return rows[row];
    }
    case 'T': {
        static constexpr std::uint8_t rows[7] = {0b11111, 0b00100, 0b00100, 0b00100, 0b00100, 0b00100, 0b00100};
        return rows[row];
    }
    default:
        return 0;
    }
}

int bitmap_text_width(const std::string& text, int scale) {
    if (text.empty()) {
        return 0;
    }
    return static_cast<int>(text.size()) * 5 * scale +
           static_cast<int>(text.size() - 1) * scale;
}

void draw_bitmap_text(SDL_Surface* surface,
                      const std::string& text,
                      int x,
                      int y,
                      int scale,
                      Uint32 color) {
    if (!surface || text.empty() || scale <= 0) {
        return;
    }

    int cursor_x = x;
    for (char ch : text) {
        if (ch != ' ') {
            for (int row = 0; row < 7; ++row) {
                const std::uint8_t bits = glyph_row(ch, row);
                for (int col = 0; col < 5; ++col) {
                    if ((bits & (1u << (4 - col))) == 0) {
                        continue;
                    }
                    SDL_Rect rect{cursor_x + col * scale, y + row * scale, scale, scale};
                    SDL_FillSurfaceRect(surface, &rect, color);
                }
            }
        }
        cursor_x += 6 * scale;
    }
}

CacheManager::BundleFrameLayer make_bad_asset_layer() {
    constexpr int width = 96;
    constexpr int height = 48;
    constexpr int scale = 3;

    SDL_Surface* surface = SDL_CreateSurface(width, height, kRuntimeRgbaPixelFormat);
    if (!surface) {
        return make_transparent_layer(1, 1);
    }

    const SDL_PixelFormatDetails* fmt = SDL_GetPixelFormatDetails(surface->format);
    SDL_Palette* palette = SDL_GetSurfacePalette(surface);
    const Uint32 background = fmt ? SDL_MapRGBA(fmt, palette, 255, 0, 255, 255) : 0;
    const Uint32 border = fmt ? SDL_MapRGBA(fmt, palette, 0, 0, 0, 255) : 0;
    const Uint32 text = fmt ? SDL_MapRGBA(fmt, palette, 255, 255, 255, 255) : 0;

    SDL_FillSurfaceRect(surface, nullptr, background);
    SDL_Rect top{0, 0, width, 2};
    SDL_Rect bottom{0, height - 2, width, 2};
    SDL_Rect left{0, 0, 2, height};
    SDL_Rect right{width - 2, 0, 2, height};
    SDL_FillSurfaceRect(surface, &top, border);
    SDL_FillSurfaceRect(surface, &bottom, border);
    SDL_FillSurfaceRect(surface, &left, border);
    SDL_FillSurfaceRect(surface, &right, border);

    const std::string line1 = "BAD";
    const std::string line2 = "ASSET";
    draw_bitmap_text(surface, line1, (width - bitmap_text_width(line1, scale)) / 2, 2, scale, text);
    draw_bitmap_text(surface, line2, (width - bitmap_text_width(line2, scale)) / 2, 25, scale, text);

    CacheManager::BundleFrameLayer layer = make_layer(surface);
    SDL_DestroySurface(surface);
    if (layer.empty()) {
        return make_transparent_layer(1, 1);
    }
    return layer;
}

CacheManager::BundleFrame make_placeholder_frame(const std::vector<float>& variant_steps) {
    CacheManager::BundleFrame frame;
    frame.variants.reserve(variant_steps.empty() ? 1u : variant_steps.size());
    const std::size_t variant_count = variant_steps.empty() ? 1u : variant_steps.size();
    const CacheManager::BundleFrameLayer fallback = make_bad_asset_layer();
    for (std::size_t idx = 0; idx < variant_count; ++idx) {
        CacheManager::BundleFrameVariant variant;
        variant.base = fallback;
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

std::vector<float> normalized_variant_steps(const AssetInfo& info) {
    auto profile = render_pipeline::ScalingLogic::ProfileForAsset(info.name);
    std::vector<float> steps = profile.steps;
    render_pipeline::ScalingLogic::NormalizeVariantSteps(steps);
    if (steps.empty()) {
        steps = render_pipeline::ScalingLogic::DefaultScaleSteps();
    }
    return steps;
}

bool steps_match_canonical(const std::vector<float>& steps) {
    std::vector<float> normalized = steps;
    render_pipeline::ScalingLogic::NormalizeVariantSteps(normalized);
    return normalized.size() == steps.size() && !normalized.empty();
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

bool bundle_layer_formats_are_valid(const CacheManager::BundleData& bundle) {
    for (const auto& animation : bundle.animations) {
        for (const auto& frame : animation.frames) {
            for (const auto& variant : frame.variants) {
                if (!variant.base.empty() &&
                    variant.base.format != static_cast<std::uint32_t>(kRuntimeRgbaPixelFormat)) {
                    return false;
                }
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

std::uint64_t fnv1a64(const void* data, std::size_t size, std::uint64_t seed = 1469598103934665603ull) {
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    std::uint64_t hash = seed;
    for (std::size_t idx = 0; idx < size; ++idx) {
        hash ^= static_cast<std::uint64_t>(bytes[idx]);
        hash *= 1099511628211ull;
    }
    return hash;
}

std::uint64_t hash_layer_identity(const CacheManager::BundleFrameLayer& layer) {
    if (layer.empty()) {
        return 0;
    }
    std::uint64_t hash = 1469598103934665603ull;
    hash = fnv1a64(&layer.width, sizeof(layer.width), hash);
    hash = fnv1a64(&layer.height, sizeof(layer.height), hash);
    hash = fnv1a64(&layer.format, sizeof(layer.format), hash);
    hash = fnv1a64(&layer.pitch, sizeof(layer.pitch), hash);
    if (!layer.pixels.empty()) {
        hash = fnv1a64(layer.pixels.data(), layer.pixels.size(), hash);
    }
    return hash;
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

struct FolderAnimationCacheState {
    std::unordered_set<std::string> required_folder_animations;
};

const CacheManager::BundleAnimation* find_bundle_animation(const CacheManager::BundleData& bundle,
                                                           const std::string& animation_name) {
    for (const auto& animation : bundle.animations) {
        if (animation.name == animation_name) {
            return &animation;
        }
    }
    return nullptr;
}

FolderAnimationCacheState collect_folder_animation_cache_state(
    const AssetInfo& info,
    const std::unordered_set<std::string>* animation_filter) {
    FolderAnimationCacheState state{};
    const std::vector<std::string> animation_names = info.animation_names();
    const fs::path cache_animation_root = fs::path("cache") / info.name / "animations";
    for (const std::string& animation_name : animation_names) {
        if (!animation_is_selected(animation_name, animation_filter)) {
            continue;
        }

        const nlohmann::json anim_json = info.animation_payload(animation_name);
        if (!anim_json.is_object()) {
            continue;
        }
        if (!anim_json.contains("source") || !anim_json["source"].is_object()) {
            continue;
        }
        const auto& source = anim_json["source"];
        if (source.value("kind", std::string{}) != "folder") {
            continue;
        }

        state.required_folder_animations.insert(animation_name);
    }

    return state;
}

bool bundle_contains_required_folder_animations(const CacheManager::BundleData& bundle,
                                                const FolderAnimationCacheState& cache_state) {
    for (const auto& animation_name : cache_state.required_folder_animations) {
        const CacheManager::BundleAnimation* animation = find_bundle_animation(bundle, animation_name);
        if (!animation || animation->frames.empty()) {
            return false;
        }
    }
    return true;
}

bool is_transparent_placeholder_layer(const CacheManager::BundleFrameLayer& layer) {
    return layer.width == 1 &&
           layer.height == 1 &&
           layer.pitch == 4 &&
           layer.pixels.size() == 4 &&
           std::all_of(layer.pixels.begin(), layer.pixels.end(), [](std::uint8_t byte) {
               return byte == 0;
           });
}

bool bundle_contains_transparent_placeholder_for_required_folder_animations(
    const CacheManager::BundleData& bundle,
    const FolderAnimationCacheState& cache_state) {
    for (const auto& animation_name : cache_state.required_folder_animations) {
        const CacheManager::BundleAnimation* animation = find_bundle_animation(bundle, animation_name);
        if (!animation) {
            continue;
        }
        for (const auto& frame : animation->frames) {
            for (const auto& variant : frame.variants) {
                if (!variant.use_atlas && is_transparent_placeholder_layer(variant.base)) {
                    return true;
                }
            }
        }
    }
    return false;
}

struct BundleValidationResult {
    bool variant_layout_ok = true;
    bool required_animations_ok = true;
    bool placeholder_policy_ok = true;
    bool pixel_format_ok = true;

    bool ready() const {
        return variant_layout_ok &&
               required_animations_ok &&
               placeholder_policy_ok &&
               pixel_format_ok;
    }
};

BundleValidationResult validate_loaded_bundle(const CacheManager::BundleData& bundle,
                                              const fs::path& /*bundle_path*/,
                                              const FolderAnimationCacheState& cache_state,
                                              bool allow_placeholder_fallback) {
    BundleValidationResult result{};
    result.variant_layout_ok = bundle_variant_layout_is_valid(bundle);
    result.required_animations_ok = bundle_contains_required_folder_animations(bundle, cache_state);
    result.placeholder_policy_ok = allow_placeholder_fallback ||
        !bundle_contains_transparent_placeholder_for_required_folder_animations(bundle, cache_state);
    result.pixel_format_ok = bundle_layer_formats_are_valid(bundle);
    return result;
}

std::string describe_bundle_validation_failure(const BundleValidationResult& validation) {
    std::vector<std::string> reasons;
    if (!validation.required_animations_ok) {
        reasons.emplace_back("missing required folder animation entries");
    }
    if (!validation.variant_layout_ok) {
        reasons.emplace_back("missing full-resolution or inconsistent variant metadata");
    }
    if (!validation.placeholder_policy_ok) {
        reasons.emplace_back("transparent placeholder frame present without explicit fallback policy");
    }
    if (!validation.pixel_format_ok) {
        reasons.emplace_back("cached layers use legacy non-RGBA32 pixel format");
    }

    if (reasons.empty()) {
        return "unknown";
    }

    std::ostringstream out;
    for (std::size_t idx = 0; idx < reasons.size(); ++idx) {
        if (idx != 0) {
            out << "; ";
        }
        out << reasons[idx];
    }
    return out.str();
}

} // namespace

PrimaryAssetCache::PrimaryAssetCache(SDL_Renderer* renderer) : renderer_(renderer) {}

bool PrimaryAssetCache::load_cached_only(AssetInfo& info,
                                         std::unordered_map<std::string, PrebuiltAnimationFrames>& out_frames,
                                         CacheManager::BundleData& raw_bundle,
                                         const std::unordered_set<std::string>* animation_filter) {
    const fs::path bundle_path = fs::path("cache") / info.name / "bundle.bin";
    const FolderAnimationCacheState cache_state =
        collect_folder_animation_cache_state(info, animation_filter);

    auto try_populate = [&](const CacheManager::BundleData& bundle) {
        out_frames.clear();
        return populate_runtime_frames(info, bundle, out_frames, animation_filter);
    };

    CacheManager::BundleData bundle;
    const bool bundle_loaded = CacheManager::load_bundle(bundle_path.generic_string(), bundle);
    if (!bundle_loaded) {
        return false;
    }

    const BundleValidationResult validation =
        validate_loaded_bundle(bundle, bundle_path, cache_state, false);
    if (!validation.ready()) {
        vibble::log::info("[PrimaryAssetCache] Cached-only bundle rejected for " + info.name +
                          " (" + describe_bundle_validation_failure(validation) + ").");
        out_frames.clear();
        return false;
    }

    const bool populated = try_populate(bundle);
    if (!populated) {
        out_frames.clear();
        return false;
    }

    raw_bundle = std::move(bundle);
    return true;
}

bool PrimaryAssetCache::ensure_cache_ready(AssetInfo& info,
                                           CacheManager::BundleData* out_bundle,
                                           const std::unordered_set<std::string>* animation_filter,
                                           WarmupOutcome* out_outcome,
                                           bool allow_placeholder_fallback) {
    const fs::path bundle_path = fs::path("cache") / info.name / "bundle.bin";
    const bool has_animation_filter = animation_filter && !animation_filter->empty();
    const FolderAnimationCacheState cache_state =
        collect_folder_animation_cache_state(info, animation_filter);
    if (out_outcome) {
        *out_outcome = WarmupOutcome::Failed;
    }

    CacheManager::BundleData bundle;
    const bool bundle_loaded = CacheManager::load_bundle(bundle_path.generic_string(), bundle);
    BundleValidationResult validation{};
    const bool bundle_ready = bundle_loaded
        ? (validation = validate_loaded_bundle(bundle, bundle_path, cache_state, allow_placeholder_fallback), validation.ready())
        : false;
    if (bundle_ready) {
        if (out_outcome) {
            *out_outcome = WarmupOutcome::Reused;
        }
        if (out_bundle) {
            *out_bundle = std::move(bundle);
        }
        return true;
    }

    if (bundle_loaded) {
        vibble::log::info("[PrimaryAssetCache] Rebuilding bundle cache for " + info.name +
                          " (" + describe_bundle_validation_failure(validation) + ").");
    } else {
        vibble::log::info("[PrimaryAssetCache] Missing cached bundle for " + info.name +
                          "; building cache from source frames.");
    }

    CacheManager::BundleData rebuilt;
    if (!build_bundle_from_sources(info, rebuilt, animation_filter, allow_placeholder_fallback)) {
        vibble::log::warn("[PrimaryAssetCache] Failed to build bundle cache from source for " + info.name + ".");
        return false;
    }

    rebuilt.content_hash = 0;
    const bool should_persist_rebuilt_bundle = !has_animation_filter;
    if (should_persist_rebuilt_bundle) {
        if (!CacheManager::save_bundle(bundle_path.generic_string(), rebuilt)) {
            vibble::log::warn("[PrimaryAssetCache] Failed to save rebuilt bundle cache for " + info.name + ".");
        }
    }

    if (out_bundle) {
        *out_bundle = std::move(rebuilt);
    }
    if (out_outcome) {
        *out_outcome = warmup_outcome_from_bundle_state(bundle_loaded);
    }
    return true;
}

PrimaryAssetCache::BatchRepairResult PrimaryAssetCache::detect_missing_cache_files(
    const std::vector<AssetInfo*>& infos,
    const std::unordered_set<std::string>* animation_filter) const {
    return run_missing_cache_file_batch(infos, animation_filter, true);
}

PrimaryAssetCache::BatchRepairResult PrimaryAssetCache::repair_missing_cache_files(
    const std::vector<AssetInfo*>& infos,
    const std::unordered_set<std::string>* animation_filter) const {
    return run_missing_cache_file_batch(infos, animation_filter, false);
}

PrimaryAssetCache::BatchRepairResult PrimaryAssetCache::run_missing_cache_file_batch(
    const std::vector<AssetInfo*>& infos,
    const std::unordered_set<std::string>* animation_filter,
    bool dry_run) const {
    BatchRepairResult batch_result;
    batch_result.ok = true;

    std::vector<AssetInfo*> selected_infos;
    selected_infos.reserve(infos.size());
    for (AssetInfo* info : infos) {
        if (!info || info->name.empty()) {
            continue;
        }
        selected_infos.push_back(info);
    }
    if (selected_infos.empty()) {
        return batch_result;
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
    options.dry_run = dry_run;
    options.quiet_task_logs = true;
    for (const AssetInfo* info : selected_infos) {
        options.filters.assets.insert(info->name);
    }
    if (animation_filter && !animation_filter->empty()) {
        options.filters.animations = *animation_filter;
    }

    GeneratorLogBridge logger;
    auto result = imgcache::ImageCacheGenerator::Run(options, logger);
    if (!result.ok) {
        batch_result.ok = false;
        batch_result.error = result.error;
        return batch_result;
    }

    batch_result.touched_assets = std::move(result.touched_assets);
    if (dry_run || result.written_files.empty()) {
        return batch_result;
    }

    batch_result.written_files_by_asset = group_written_files_by_asset(result.written_files);
    render_pipeline::ScalingLogic::LoadPrecomputedProfiles(true);
    for (AssetInfo* info : selected_infos) {
        auto it = batch_result.written_files_by_asset.find(info->name);
        if (it == batch_result.written_files_by_asset.end() || it->second.empty()) {
            continue;
        }
        info->set_scale_percentage(100.0f);
        record_load_repairs_from_written_files(*info, it->second);
        info->consume_pending_texture_rebuild_on_load();
    }
    return batch_result;
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

        SDL_Surface* atlas = SDL_CreateSurface(used_w, atlas_h, kRuntimeRgbaPixelFormat);
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
                                                  const std::unordered_set<std::string>* animation_filter,
                                                  bool allow_placeholder_fallback) {
    out_data = CacheManager::BundleData{};
    out_data.version = 2;
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
            const int source_frame_count = count_sequential_png(folder);
            const int cached_frame_count = count_sequential_png(cache_normal_100);
            const int frame_count = std::max(source_frame_count, cached_frame_count);
            if (frame_count <= 0) {
                if (!allow_placeholder_fallback) {
                    vibble::log::warn("[PrimaryAssetCache] No source frames found for " + info.name +
                                      "::" + bundle_anim.name + " in '" + folder.generic_string() +
                                      "'; bad-asset placeholder fallback is disabled, so cache build fails.");
                    return false;
                }
                vibble::log::warn("[PrimaryAssetCache] Intentional fallback policy enabled for " + info.name +
                                  "::" + bundle_anim.name + "; injecting a readable BAD ASSET placeholder frame.");
                bundle_anim.frames.push_back(make_placeholder_frame(bundle_anim.variant_steps));
                out_data.animations.push_back(std::move(bundle_anim));
                continue;
            }

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
                    if (!allow_placeholder_fallback) {
                        vibble::log::warn("[PrimaryAssetCache] Missing base frame data for " + info.name +
                                          "::" + bundle_anim.name +
                                          "; bad-asset frame fallback is disabled, so cache build fails.");
                        return false;
                    }
                    if (!warned_missing_base_frame) {
                        vibble::log::warn("[PrimaryAssetCache] Intentional fallback policy enabled for " + info.name +
                                          "::" + bundle_anim.name +
                                          "; injecting BAD ASSET fallback for unavailable frame(s).");
                        warned_missing_base_frame = true;
                    }
                    base_layer = make_bad_asset_layer();
                }

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
                        variant.base = make_bad_asset_layer();
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
        std::unordered_map<std::uint64_t, SDL_Texture*> dedup_textures;

        std::unordered_map<std::size_t, SDL_Texture*> atlas_by_variant;
        if (anim.uses_atlas) {
            for (std::size_t idx = 0; idx < anim.atlas_paths.size(); ++idx) {
                if (anim.atlas_paths[idx].empty()) continue;
                SDL_Surface* atlas_surface = CacheManager::load_surface(anim.atlas_paths[idx].generic_string());
                if (!atlas_surface) continue;
                SDL_Texture* atlas_tex = CacheManager::surface_to_texture(renderer_, atlas_surface);
                if (atlas_tex) {
                    apply_texture_scale_mode(atlas_tex);
                    atlas_by_variant[idx] = atlas_tex;
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
                    SDL_Texture* tex = nullptr;
                    const std::uint64_t layer_hash = hash_layer_identity(variant.base);
                    if (layer_hash != 0) {
                        auto dedup_it = dedup_textures.find(layer_hash);
                        if (dedup_it != dedup_textures.end()) {
                            tex = dedup_it->second;
                        }
                    }

                    if (!tex) {
                        SDL_Surface* base_surface = surface_from_layer(variant.base);
                        CacheManager::TextureUploadOptions upload_options{};
                        upload_options.semantic = CacheManager::TextureSemantic::Color;
                        upload_options.enable_mipmaps =
                            variant.base.width >= 128 && variant.base.height >= 128;
                        tex = CacheManager::surface_to_texture(renderer_, base_surface, upload_options);
                        if (base_surface) {
                            SDL_DestroySurface(base_surface);
                        }
                        apply_texture_scale_mode(tex);
                        if (tex && layer_hash != 0) {
                            dedup_textures[layer_hash] = tex;
                        }
                    }

                    cache_entry.textures[variant_idx] = tex;
                    cache_entry.widths[variant_idx] = variant.base.width;
                    cache_entry.heights[variant_idx] = variant.base.height;
                    cache_entry.source_rects[variant_idx] = SDL_Rect{0, 0, variant.base.width, variant.base.height};
                    cache_entry.uses_atlas[variant_idx] = false;
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
                                      const std::unordered_set<std::string>* animation_filter,
                                      bool allow_placeholder_fallback) {
    if (!ensure_cache_ready(info, &raw_bundle, animation_filter, nullptr, allow_placeholder_fallback)) {
        return false;
    }

    out_frames.clear();
    const bool populated = populate_runtime_frames(info, raw_bundle, out_frames, animation_filter);
    if (!populated) {
        vibble::log::warn("[PrimaryAssetCache] Bundle cache for " + info.name +
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
