#pragma once

#include <unordered_map>
#include <string>
#include <vector>
#include <filesystem>

#include <SDL3/SDL.h>
#include <nlohmann/json.hpp>

#include "animation.hpp"
#include "utils/cache_manager.hpp"

class PrimaryAssetCache {
public:
    explicit PrimaryAssetCache(SDL_Renderer* renderer);

    bool load_or_build(class AssetInfo& info,
                       std::unordered_map<std::string, PrebuiltAnimationFrames>& out_frames,
                       CacheManager::BundleData& raw_bundle);

    bool save_current(const class AssetInfo& info);

private:
    SDL_Renderer* renderer_ = nullptr;

    std::uint64_t compute_hash(const AssetInfo& info) const;
    bool build_bundle_from_sources(const AssetInfo& info, CacheManager::BundleData& out_data);
    bool populate_runtime_frames(const AssetInfo& info,
                                 const CacheManager::BundleData& bundle,
                                 std::unordered_map<std::string, PrebuiltAnimationFrames>& out_frames);
    bool build_variant_atlases(CacheManager::BundleAnimation& animation,
                               const std::filesystem::path& cache_root) const;
};
