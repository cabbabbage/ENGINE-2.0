#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <string>

#include <SDL3/SDL.h>
#include <nlohmann/json.hpp>

#include "assets/asset/asset_info.hpp"

namespace {

namespace fs = std::filesystem;

class ScopedSdlRenderer {
public:
    ScopedSdlRenderer() {
        video_initialized_ = SDL_InitSubSystem(SDL_INIT_VIDEO);
        if (!video_initialized_) {
            return;
        }
        window_ = SDL_CreateWindow("asset_animation_preload_failure", 32, 32, SDL_WINDOW_HIDDEN);
        if (!window_) {
            return;
        }
        renderer_ = SDL_CreateRenderer(window_, SDL_SOFTWARE_RENDERER);
        if (!renderer_) {
            renderer_ = SDL_CreateRenderer(window_, nullptr);
        }
    }

    ~ScopedSdlRenderer() {
        if (renderer_) {
            SDL_DestroyRenderer(renderer_);
        }
        if (window_) {
            SDL_DestroyWindow(window_);
        }
        if (video_initialized_) {
            SDL_QuitSubSystem(SDL_INIT_VIDEO);
        }
    }

    SDL_Renderer* get() const { return renderer_; }

private:
    bool video_initialized_ = false;
    SDL_Window* window_ = nullptr;
    SDL_Renderer* renderer_ = nullptr;
};

fs::path unique_test_dir(const std::string& suffix) {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    return fs::temp_directory_path() / ("engine_asset_preload_" + suffix + "_" + std::to_string(stamp));
}

bool contains_name(const std::vector<std::string>& names, const std::string& expected) {
    return std::find(names.begin(), names.end(), expected) != names.end();
}

} // namespace

TEST_CASE("cached-only folder animation preload reports missing runtime frames instead of appearing loaded") {
    ScopedSdlRenderer renderer;
    REQUIRE(renderer.get() != nullptr);

    const std::string asset_name = "empty_invalid_bundle_preload_asset";
    const fs::path asset_root = unique_test_dir("asset_root");
    const fs::path empty_animation_folder = asset_root / "idle";
    fs::create_directories(empty_animation_folder);
    fs::remove_all(fs::path("cache") / asset_name);

    nlohmann::json metadata = {
        {"asset_name", asset_name},
        {"asset_directory", asset_root.generic_string()},
        {"start_animation", "idle"},
        {"scale_variants", nlohmann::json::array({1.0})},
        {"animations", {
            {"idle", {
                {"source", {
                    {"kind", "folder"},
                    {"path", "idle"}
                }}
            }}
        }}
    };

    AssetInfo info(asset_name, metadata);
    const auto result = info.loadAnimationsDetailed(renderer.get(), true, true, false);

    CHECK(result.attempted);
    CHECK_FALSE(result.ok());
    CHECK_FALSE(result.used_placeholder_fallback);
    CHECK(contains_name(result.missing_runtime_frame_animations, "idle"));

    auto anim_it = info.animations.find("idle");
    REQUIRE(anim_it != info.animations.end());
    CHECK(anim_it->second.cached_frame_count() == 0);

    fs::remove_all(fs::path("cache") / asset_name);
    fs::remove_all(asset_root);
}

TEST_CASE("folder animation preload can use generated bad asset fallback texture") {
    ScopedSdlRenderer renderer;
    REQUIRE(renderer.get() != nullptr);

    const std::string asset_name = "empty_bad_asset_fallback_preload_asset";
    const fs::path asset_root = unique_test_dir("bad_asset_fallback_root");
    const fs::path empty_animation_folder = asset_root / "default";
    fs::create_directories(empty_animation_folder);
    fs::remove_all(fs::path("cache") / asset_name);

    nlohmann::json metadata = {
        {"asset_name", asset_name},
        {"asset_directory", asset_root.generic_string()},
        {"start_animation", "default"},
        {"scale_variants", nlohmann::json::array({1.0})},
        {"animations", {
            {"default", {
                {"source", {
                    {"kind", "folder"},
                    {"path", "default"}
                }}
            }}
        }}
    };

    AssetInfo info(asset_name, metadata);
    const auto result = info.loadAnimationsDetailed(renderer.get(), true, true, true);

    CHECK(result.attempted);
    CHECK(result.ok());
    CHECK(result.cache_ready);
    CHECK(result.missing_runtime_frame_animations.empty());

    auto anim_it = info.animations.find("default");
    REQUIRE(anim_it != info.animations.end());
    REQUIRE(anim_it->second.cached_frame_count() == 1);

    bool found_texture = false;
    for (const auto& frame : anim_it->second.cached_frames()) {
        for (SDL_Texture* texture : frame.textures) {
            found_texture = found_texture || texture != nullptr;
        }
    }
    CHECK(found_texture);
    CHECK(anim_it->second.has_frames());

    fs::remove_all(fs::path("cache") / asset_name);
    fs::remove_all(asset_root);
}
