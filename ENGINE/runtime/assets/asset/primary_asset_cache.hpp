#pragma once

#include <unordered_map>
#include <unordered_set>
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
                       CacheManager::BundleData& raw_bundle,
                       const std::unordered_set<std::string>* animation_filter = nullptr);

    bool save_current(const class AssetInfo& info);

private:
    SDL_Renderer* renderer_ = nullptr;

    bool repair_missing_cache_files(AssetInfo& info,
                                    const std::unordered_set<std::string>* animation_filter = nullptr) const;
    bool build_bundle_from_sources(const AssetInfo& info,
                                   CacheManager::BundleData& out_data,
                                   const std::unordered_set<std::string>* animation_filter = nullptr);
    bool populate_runtime_frames(const AssetInfo& info,
                                 const CacheManager::BundleData& bundle,
                                 std::unordered_map<std::string, PrebuiltAnimationFrames>& out_frames,
                                 const std::unordered_set<std::string>* animation_filter);
    bool build_variant_atlases(CacheManager::BundleAnimation& animation,
                               const std::filesystem::path& cache_root) const;
};
