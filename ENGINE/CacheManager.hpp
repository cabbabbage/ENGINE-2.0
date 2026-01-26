#pragma once

#include "AssetToolkit.hpp"
#include "EffectsParser.hpp"
#include <string>
#include <vector>
#include <filesystem>

namespace vibble {

// Cache file information
struct CacheFileInfo {
    std::string file_path;
    std::filesystem::file_time_type last_modified;
    bool exists;
    bool is_valid;
    std::string error_message;
};

// Cache validation report
struct CacheValidationReport {
    CacheValidationResult result;
    std::string asset_name;
    std::string animation_name;
    std::vector<CacheFileInfo> missing_files;
    std::vector<CacheFileInfo> invalid_files;
    int total_files_checked;
    int files_valid;
    int files_invalid;
};

// Cache manager for asset processing
class CacheManager {
public:
    // Constructor and destructor
    CacheManager(AssetToolkit& toolkit);
    ~CacheManager();

    // Validate entire cache integrity
    CacheValidationResult validateCacheIntegrity(const std::string& cache_root,
                                              std::vector<CacheValidationReport>& reports);

    // Validate single animation cache
    CacheValidationResult validateAnimationCache(const std::string& asset_name,
                                              const std::string& animation_name,
                                              const std::string& cache_root,
                                              CacheValidationReport& report);

    // Mark frames for rebuild
    bool markFramesForRebuild(const std::string& manifest_path,
                             const std::string& asset_name,
                             const std::string& animation_name,
                             const std::vector<int>& frame_indices);

    // Mark all frames in animation for rebuild
    bool markAllFramesForRebuild(const std::string& manifest_path,
                                const std::string& asset_name,
                                const std::string& animation_name);

    // Clear rebuild flags for frames
    bool clearRebuildFlags(const std::string& manifest_path,
                          const std::string& asset_name,
                          const std::string& animation_name,
                          const std::vector<int>& frame_indices);

    // Cleanup stale cache files
    bool cleanupStaleCacheFiles(const std::string& cache_root,
                               const std::string& asset_name = "",
                               const std::string& animation_name = "");

    // Rebuild missing cache files
    bool rebuildMissingCacheFiles(const std::string& manifest_path,
                                 const std::string& cache_root);

    // Get statistics
    struct CacheStatistics {
        int total_files_checked;
        int valid_files;
        int missing_files;
        int invalid_files;
        int files_rebuilt;
        int errors_encountered;
    };

    CacheStatistics getStatistics() const { return stats_; }

    // Get last error
    std::string getLastError() const { return last_error_; }

private:
    AssetToolkit& toolkit_;
    CacheStatistics stats_;
    std::string last_error_;

    // Helper methods
    bool checkCacheFileExists(const std::string& file_path);
    bool checkCacheFileValid(const std::string& file_path);
    std::filesystem::file_time_type getFileLastModified(const std::string& file_path);
    bool compareFileTimestamps(const std::string& source_path, const std::string& cache_path);

    bool loadManifest(const std::string& manifest_path, nlohmann::json& manifest);
    bool saveManifest(const std::string& manifest_path, const nlohmann::json& manifest);

    bool findAnimationFrames(const std::string& animation_dir, std::vector<std::string>& frame_paths);
    bool getFrameMetadata(nlohmann::json& manifest,
                         const std::string& asset_name,
                         const std::string& animation_name,
                         std::vector<nlohmann::json>& frame_metadata);

    bool updateFrameRebuildFlags(nlohmann::json& manifest,
                                const std::string& asset_name,
                                const std::string& animation_name,
                                const std::vector<int>& frame_indices,
                                bool needs_rebuild);

    void setLastError(const std::string& error);
    void clearLastError();

    bool isOrphanedCacheFile(const fs::path& cache_file);
};

// Complete asset processing pipeline
class AssetProcessingPipeline {
public:
    AssetProcessingPipeline(AssetToolkit& toolkit);
    ~AssetProcessingPipeline();

    // Main processing entry point
    bool processAllAssetsWithCacheManagement(const std::string& manifest_path);

    // Process single asset with cache validation
    bool processAssetWithValidation(const std::string& asset_name,
                                  const std::string& asset_source_dir);

    // Process only assets that need rebuilding
    bool processAssetsNeedingRebuild(const std::string& manifest_path);

    // Get processing statistics
    struct ProcessingStatistics {
        int total_assets_checked;
        int assets_processed;
        int assets_skipped;
        int total_animations_checked;
        int animations_processed;
        int animations_skipped;
        int total_frames_checked;
        int frames_processed;
        int frames_skipped;
        int frames_rebuilt;
        int errors_encountered;
        std::vector<std::string> error_messages;
    };

    ProcessingStatistics getStatistics() const { return stats_; }

    // Get last error
    std::string getLastError() const { return last_error_; }

private:
    AssetToolkit& toolkit_;
    CacheManager cache_manager_;
    EffectsParser effects_parser_{"", ""};
    AssetProcessor asset_processor_;
    ProcessingStatistics stats_;
    std::string last_error_;

    // Helper methods
    bool loadAndValidateManifest(const std::string& manifest_path, nlohmann::json& manifest);
    bool shouldProcessAsset(const nlohmann::json& asset_data);
    bool shouldProcessAnimation(const nlohmann::json& animation_data);
    bool validateAssetCache(const nlohmann::json& manifest,
                           const std::string& asset_name,
                           const std::string& asset_source_dir);
    bool processAssetIfNeeded(const nlohmann::json& manifest,
                             const std::string& asset_name,
                             const std::string& asset_source_dir);
    bool processAnimationIfNeeded(const nlohmann::json& manifest,
                                 const std::string& asset_name,
                                 const std::string& animation_name,
                                 const std::string& animation_dir,
                                 const EffectParams& fg_effects,
                                 const EffectParams& bg_effects);
    bool updateManifestAfterProcessing(const std::string& manifest_path,
                                     const std::string& asset_name,
                                     const std::string& animation_name,
                                     const std::vector<int>& processed_frames,
                                     bool success);

    void setLastError(const std::string& error);
    void clearLastError();
};

// Rebuild management utilities
class RebuildManager {
public:
    RebuildManager(AssetToolkit& toolkit);
    ~RebuildManager();

    // Mark assets for complete rebuild
    bool markAllAssetsForRebuild(const std::string& manifest_path);
    bool markAssetForRebuild(const std::string& manifest_path, const std::string& asset_name);
    bool markAnimationForRebuild(const std::string& manifest_path,
                                const std::string& asset_name,
                                const std::string& animation_name);
    bool markFrameForRebuild(const std::string& manifest_path,
                           const std::string& asset_name,
                           const std::string& animation_name,
                           int frame_index);

    // Check rebuild status
    bool needsRebuild(const std::string& manifest_path);
    bool assetNeedsRebuild(const std::string& manifest_path, const std::string& asset_name);
    bool animationNeedsRebuild(const std::string& manifest_path,
                              const std::string& asset_name,
                              const std::string& animation_name);
    bool frameNeedsRebuild(const std::string& manifest_path,
                          const std::string& asset_name,
                          const std::string& animation_name,
                          int frame_index);

    // Get rebuild statistics
    struct RebuildStatistics {
        int total_assets;
        int assets_needing_rebuild;
        int total_animations;
        int animations_needing_rebuild;
        int total_frames;
        int frames_needing_rebuild;
    };

    RebuildStatistics getRebuildStatistics(const std::string& manifest_path);

    // Get last error
    std::string getLastError() const { return last_error_; }

private:
    AssetToolkit& toolkit_;
    std::string last_error_;

    // Helper methods
    bool loadManifest(const std::string& manifest_path, nlohmann::json& manifest);
    bool saveManifest(const std::string& manifest_path, const nlohmann::json& manifest);
    bool ensureFrameMetadataExists(nlohmann::json& manifest,
                                  const std::string& asset_name,
                                  const std::string& animation_name,
                                  int frame_count);
    void setLastError(const std::string& error);
};

} // namespace vibble
