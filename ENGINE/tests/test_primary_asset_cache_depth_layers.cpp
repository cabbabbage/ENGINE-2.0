#include <doctest/doctest.h>

#include <SDL3/SDL.h>
#include <SDL3_image/SDL_image.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <string>

#include "assets/asset/asset_info.hpp"
#include "assets/asset/primary_asset_cache.hpp"
#include "rendering/render/scaling_logic.hpp"
#include "utils/cache_manager.hpp"

namespace {

class ScopedSdlVideo {
public:
    ScopedSdlVideo() : initialized_(SDL_InitSubSystem(SDL_INIT_VIDEO)) {}
    ~ScopedSdlVideo() {
        if (initialized_) {
            SDL_QuitSubSystem(SDL_INIT_VIDEO);
        }
    }

    bool initialized() const { return initialized_; }

private:
    bool initialized_ = false;
};

class ScopedRenderer {
public:
    ScopedRenderer() {
        if (!video_.initialized()) {
            return;
        }
        window_ = SDL_CreateWindow("primary_asset_cache_depth_layer_tests", 32, 32, SDL_WINDOW_HIDDEN);
        if (!window_) {
            return;
        }
        renderer_ = SDL_CreateRenderer(window_, nullptr);
        if (!renderer_) {
            renderer_ = SDL_CreateRenderer(window_, SDL_SOFTWARE_RENDERER);
        }
    }

    ~ScopedRenderer() {
        if (renderer_) {
            SDL_DestroyRenderer(renderer_);
            renderer_ = nullptr;
        }
        if (window_) {
            SDL_DestroyWindow(window_);
            window_ = nullptr;
        }
    }

    SDL_Renderer* get() const { return renderer_; }
    bool ready() const { return renderer_ != nullptr; }

private:
    ScopedSdlVideo video_{};
    SDL_Window* window_ = nullptr;
    SDL_Renderer* renderer_ = nullptr;
};

std::string unique_asset_name(const std::string& prefix) {
    static std::uint64_t counter = 0;
    ++counter;
    return prefix + "_" + std::to_string(SDL_GetTicks()) + "_" + std::to_string(counter);
}

bool write_png(const std::filesystem::path& path, SDL_Color color) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) {
        return false;
    }

    SDL_Surface* surface = SDL_CreateSurface(1, 1, SDL_PIXELFORMAT_RGBA8888);
    if (!surface) {
        return false;
    }

    const SDL_PixelFormatDetails* fmt = SDL_GetPixelFormatDetails(surface->format);
    SDL_Palette* palette = SDL_GetSurfacePalette(surface);
    if (!fmt) {
        SDL_DestroySurface(surface);
        return false;
    }
    const Uint32 packed = SDL_MapRGBA(fmt, palette, color.r, color.g, color.b, color.a);
    if (!SDL_FillSurfaceRect(surface, nullptr, packed)) {
        SDL_DestroySurface(surface);
        return false;
    }

    const bool saved = IMG_SavePNG(surface, path.generic_string().c_str());
    SDL_DestroySurface(surface);
    return saved;
}

CacheManager::BundleFrameLayer make_bundle_layer(SDL_Color color) {
    CacheManager::BundleFrameLayer layer;
    layer.width = 1;
    layer.height = 1;
    layer.format = SDL_PIXELFORMAT_RGBA8888;
    layer.pitch = 4;
    layer.pixels = {color.r, color.g, color.b, color.a};
    return layer;
}

nlohmann::json make_folder_source_metadata(const std::string& asset_name) {
    return nlohmann::json{
        {"asset_name", asset_name},
        {"asset_type", "object"},
        {"start", "default"},
        {"animations",
         {
             {"default",
              {
                  {"source", {{"kind", "folder"}, {"path", "default"}}},
              }},
         }},
    };
}

CacheManager::BundleData make_bundle_data(const std::string& animation_name,
                                          bool include_foreground,
                                          bool include_background) {
    CacheManager::BundleData data;
    data.version = 1;

    CacheManager::BundleAnimation animation;
    animation.name = animation_name;
    animation.variant_steps = render_pipeline::ScalingLogic::DefaultScaleSteps();

    CacheManager::BundleFrame frame;
    frame.variants.reserve(animation.variant_steps.size());
    for (std::size_t idx = 0; idx < animation.variant_steps.size(); ++idx) {
        CacheManager::BundleFrameVariant variant;
        variant.base = make_bundle_layer(SDL_Color{255, 255, 255, 255});
        if (include_foreground) {
            variant.foreground = make_bundle_layer(SDL_Color{0, 255, 0, 255});
        }
        if (include_background) {
            variant.background = make_bundle_layer(SDL_Color{0, 0, 255, 255});
        }
        frame.variants.push_back(std::move(variant));
    }
    animation.frames.push_back(std::move(frame));
    data.animations.push_back(std::move(animation));
    return data;
}

class ScopedCacheFixture {
public:
    explicit ScopedCacheFixture(const std::string& prefix)
        : asset_name_(unique_asset_name(prefix)),
          info_(asset_name_, make_folder_source_metadata(asset_name_)) {
        cache_animation_scale_root_ =
            std::filesystem::path("cache") / asset_name_ / "animations" / "default" / "scale_100";
        cache_bundle_path_ = std::filesystem::path("cache") / asset_name_ / "bundle.bin";

        const bool wrote_base =
            write_png(cache_animation_scale_root_ / "normal" / "0.png", SDL_Color{255, 255, 255, 255});
        const bool wrote_fg =
            write_png(cache_animation_scale_root_ / "foreground" / "0.png", SDL_Color{0, 255, 0, 255});
        const bool wrote_bg =
            write_png(cache_animation_scale_root_ / "background" / "0.png", SDL_Color{0, 0, 255, 255});
        CHECK(wrote_base);
        CHECK(wrote_fg);
        CHECK(wrote_bg);
    }

    ~ScopedCacheFixture() {
        std::error_code ec;
        std::filesystem::remove_all(std::filesystem::path("cache") / asset_name_, ec);
        (void)ec;
    }

    const std::string& asset_name() const { return asset_name_; }
    AssetInfo& info() { return info_; }
    const std::filesystem::path& bundle_path() const { return cache_bundle_path_; }
    const std::filesystem::path& scale_root() const { return cache_animation_scale_root_; }

private:
    std::string asset_name_;
    AssetInfo info_;
    std::filesystem::path cache_animation_scale_root_;
    std::filesystem::path cache_bundle_path_;
};

bool bundle_has_foreground_or_background(const CacheManager::BundleData& data) {
    for (const auto& animation : data.animations) {
        for (const auto& frame : animation.frames) {
            for (const auto& variant : frame.variants) {
                if (!variant.foreground.empty() || !variant.background.empty()) {
                    return true;
                }
            }
        }
    }
    return false;
}

} // namespace

TEST_CASE("PrimaryAssetCache rebuilds stale bundle missing depth layers") {
    ScopedRenderer renderer_scope;
    REQUIRE(renderer_scope.ready());

    ScopedCacheFixture fixture("depth_layer_integrity");
    const CacheManager::BundleData stale_bundle = make_bundle_data("default", false, false);
    REQUIRE(CacheManager::save_bundle(fixture.bundle_path().generic_string(), stale_bundle));

    PrimaryAssetCache cache(renderer_scope.get());
    CacheManager::BundleData rebuilt_bundle;
    PrimaryAssetCache::WarmupOutcome outcome = PrimaryAssetCache::WarmupOutcome::Failed;

    REQUIRE(cache.ensure_cache_ready(fixture.info(), &rebuilt_bundle, nullptr, &outcome));
    CHECK(outcome == PrimaryAssetCache::WarmupOutcome::Rebuilt);
    CHECK(bundle_has_foreground_or_background(rebuilt_bundle));
}

TEST_CASE("PrimaryAssetCache rebuilds bundle when overlay cache files are newer than bundle") {
    ScopedRenderer renderer_scope;
    REQUIRE(renderer_scope.ready());

    ScopedCacheFixture fixture("depth_layer_freshness");
    const CacheManager::BundleData initial_bundle = make_bundle_data("default", true, true);
    REQUIRE(CacheManager::save_bundle(fixture.bundle_path().generic_string(), initial_bundle));

    PrimaryAssetCache cache(renderer_scope.get());
    CacheManager::BundleData cached_bundle;
    PrimaryAssetCache::WarmupOutcome initial_outcome = PrimaryAssetCache::WarmupOutcome::Failed;
    REQUIRE(cache.ensure_cache_ready(fixture.info(), &cached_bundle, nullptr, &initial_outcome));
    CHECK(initial_outcome == PrimaryAssetCache::WarmupOutcome::Reused);

    std::error_code ec;
    const auto bundle_time = std::filesystem::last_write_time(fixture.bundle_path(), ec);
    REQUIRE_FALSE(ec);
    const auto newer_time = bundle_time + std::chrono::seconds(5);
    std::filesystem::last_write_time(fixture.scale_root() / "foreground" / "0.png", newer_time, ec);
    REQUIRE_FALSE(ec);
    std::filesystem::last_write_time(fixture.scale_root() / "background" / "0.png", newer_time, ec);
    REQUIRE_FALSE(ec);

    CacheManager::BundleData rebuilt_bundle;
    PrimaryAssetCache::WarmupOutcome rebuilt_outcome = PrimaryAssetCache::WarmupOutcome::Failed;
    REQUIRE(cache.ensure_cache_ready(fixture.info(), &rebuilt_bundle, nullptr, &rebuilt_outcome));
    CHECK(rebuilt_outcome == PrimaryAssetCache::WarmupOutcome::Rebuilt);
    CHECK(bundle_has_foreground_or_background(rebuilt_bundle));
}

TEST_CASE("AssetInfo loadAnimations falls back from cached-only mode and restores overlays") {
    ScopedRenderer renderer_scope;
    REQUIRE(renderer_scope.ready());

    ScopedCacheFixture fixture("depth_layer_assume_cached");
    const CacheManager::BundleData stale_bundle = make_bundle_data("default", false, false);
    REQUIRE(CacheManager::save_bundle(fixture.bundle_path().generic_string(), stale_bundle));

    fixture.info().loadAnimations(renderer_scope.get(), true, true);

    auto animation_it = fixture.info().animations.find("default");
    REQUIRE(animation_it != fixture.info().animations.end());
    const Animation& animation = animation_it->second;
    REQUIRE(animation.cached_frame_count() > 0);
    const Animation::FrameCache& first_frame = animation.cached_frames().front();
    REQUIRE(first_frame.foreground_textures.size() > 0);
    REQUIRE(first_frame.background_textures.size() > 0);
    CHECK(first_frame.foreground_textures.front() != nullptr);
    CHECK(first_frame.background_textures.front() != nullptr);
}

TEST_CASE("PrimaryAssetCache cached-only load self-heals overlays from variant cache files") {
    ScopedRenderer renderer_scope;
    REQUIRE(renderer_scope.ready());

    ScopedCacheFixture fixture("depth_layer_self_heal");

    std::error_code ec;
    std::filesystem::remove(fixture.scale_root() / "foreground" / "0.png", ec);
    ec.clear();
    std::filesystem::remove(fixture.scale_root() / "background" / "0.png", ec);
    ec.clear();

    const std::filesystem::path scale_50_root = fixture.scale_root().parent_path() / "scale_50";
    REQUIRE(write_png(scale_50_root / "foreground" / "0.png", SDL_Color{0, 255, 0, 255}));
    REQUIRE(write_png(scale_50_root / "background" / "0.png", SDL_Color{0, 0, 255, 255}));

    const CacheManager::BundleData stale_bundle = make_bundle_data("default", false, false);
    REQUIRE(CacheManager::save_bundle(fixture.bundle_path().generic_string(), stale_bundle));

    PrimaryAssetCache cache(renderer_scope.get());
    std::unordered_map<std::string, PrebuiltAnimationFrames> prebuilt;
    CacheManager::BundleData raw_bundle;
    REQUIRE(cache.load_cached_only(fixture.info(), prebuilt, raw_bundle, nullptr));

    auto anim_it = prebuilt.find("default");
    REQUIRE(anim_it != prebuilt.end());
    REQUIRE_FALSE(anim_it->second.frames.empty());
    REQUIRE_FALSE(anim_it->second.variant_steps.empty());

    int variant_idx_50 = -1;
    for (std::size_t idx = 0; idx < anim_it->second.variant_steps.size(); ++idx) {
        if (std::fabs(anim_it->second.variant_steps[idx] - 0.5f) < 0.0001f) {
            variant_idx_50 = static_cast<int>(idx);
            break;
        }
    }
    REQUIRE(variant_idx_50 >= 0);

    const Animation::FrameCache& frame = anim_it->second.frames.front();
    REQUIRE(static_cast<std::size_t>(variant_idx_50) < frame.foreground_textures.size());
    REQUIRE(static_cast<std::size_t>(variant_idx_50) < frame.background_textures.size());
    CHECK(frame.foreground_textures[static_cast<std::size_t>(variant_idx_50)] != nullptr);
    CHECK(frame.background_textures[static_cast<std::size_t>(variant_idx_50)] != nullptr);
}
