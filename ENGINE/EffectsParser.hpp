#pragma once

#include "AssetToolkit.hpp"
#include <string>
#include <utility>
#include <vector>

namespace vibble {

// Effects parser for manifest files
class EffectsParser {
public:
    // Constructor and destructor
    EffectsParser(const std::string& manifest_path, const std::string& cache_path = "");
    ~EffectsParser();

    // Parse effects from manifest and cache
    // Returns: (foreground_effects, background_effects, unchanged)
    std::tuple<EffectParams, EffectParams, bool> parse();

    // Save effects to cache
    bool saveEffectsCache(const EffectParams& fg_effects, const EffectParams& bg_effects);

    // Load effects from cache
    bool loadEffectsCache(EffectParams& fg_effects, EffectParams& bg_effects);

    // Check if effects have changed
    bool effectsChanged(const EffectParams& fg_effects, const EffectParams& bg_effects);

    // Get the cache path being used
    std::string getCachePath() const { return cache_path_; }

    // Get last error
    std::string getLastError() const { return last_error_; }

private:
    std::string manifest_path_;
    std::string cache_path_;
    nlohmann::json manifest_data_;
    nlohmann::json cache_data_;
    mutable std::string last_error_;

    // Helper methods
    bool loadManifest();
    bool parseManifestEffects();
    EffectParams extractEffectBlock(const std::string& block_name);
    std::string getDefaultCachePath() const;
    void setLastError(const std::string& error) const;
    void clearLastError() const;
};

// Animation frame information
struct AnimationFrameInfo {
    std::string source_path;
    std::string output_path_normal;
    std::string output_path_fg;
    std::string output_path_bg;
    int frame_index;
    bool needs_rebuild;
    int source_width;
    int source_height;
};

// Animation processing result
struct AnimationProcessingResult {
    std::string animation_name;
    int total_frames;
    int processed_frames;
    int failed_frames;
    std::vector<std::string> error_messages;
    bool success;
};

// Asset processing manager
class AssetProcessor {
public:
    AssetProcessor(AssetToolkit& toolkit);
    ~AssetProcessor();

    // Process all assets in manifest
    bool processAllAssets(const std::string& manifest_path);

    // Process single asset
    bool processAsset(const std::string& asset_name, const std::string& asset_source_dir);

    // Process single animation
    bool processAnimation(const std::string& asset_name,
                         const std::string& animation_name,
                         const std::string& animation_dir,
                         const EffectParams& fg_effects,
                         const EffectParams& bg_effects);

    // Get processing statistics
    int getTotalProcessedFrames() const { return total_processed_frames_; }
    int getTotalFailedFrames() const { return total_failed_frames_; }

    // Find animation directories (made public for AssetProcessingPipeline)
    std::vector<std::string> findAnimationDirectories(const std::string& asset_dir);

private:
    AssetToolkit& toolkit_;
    int total_processed_frames_ = 0;
    int total_failed_frames_ = 0;
    std::string last_error_;

    // Helper methods
    bool loadManifest(const std::string& manifest_path, nlohmann::json& manifest);
    std::vector<AnimationFrameInfo> collectAnimationFrames(const std::string& animation_dir,
                                                         const std::string& cache_dir,
                                                         const std::vector<int>& scale_variants);
    bool processAnimationFrame(const AnimationFrameInfo& frame_info,
                             const ImageData& source_image,
                             const EffectParams& fg_effects,
                             const EffectParams& bg_effects);
    void setLastError(const std::string& error);
    std::string getLastError() const;
    void clearLastError();
};

} // namespace vibble
