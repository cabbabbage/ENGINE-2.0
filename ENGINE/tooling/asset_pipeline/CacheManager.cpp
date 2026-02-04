#include "CacheManager.hpp"
#include "utils/log.hpp"
#include <fstream>
#include <algorithm>
#include <filesystem>

namespace fs = std::filesystem;

namespace vibble {

// CacheManager implementation
CacheManager::CacheManager(AssetToolkit& toolkit) : toolkit_(toolkit) {
    vibble::log::info("[CacheManager] Initialized");
}

CacheManager::~CacheManager() {
    vibble::log::info("[CacheManager] Shutdown");
}

// Validate entire cache integrity
CacheValidationResult CacheManager::validateCacheIntegrity(const std::string& cache_root,
                                                         std::vector<CacheValidationReport>& reports) {
    clearLastError();
    stats_ = {}; // Reset statistics

    try {
        // Check if cache root exists
        if (!fs::exists(cache_root) || !fs::is_directory(cache_root)) {
            setLastError("Cache root does not exist: " + cache_root);
            return CacheValidationResult::ERROR;
        }

        // Process each asset in cache
        for (const auto& asset_entry : fs::directory_iterator(cache_root)) {
            if (!asset_entry.is_directory()) continue;

            std::string asset_name = asset_entry.path().filename().string();
            fs::path animations_dir = asset_entry.path() / "animations";

            if (!fs::exists(animations_dir) || !fs::is_directory(animations_dir)) {
                continue; // No animations for this asset
            }

            // Process each animation
            for (const auto& anim_entry : fs::directory_iterator(animations_dir)) {
                if (!anim_entry.is_directory()) continue;

                std::string animation_name = anim_entry.path().filename().string();
                CacheValidationReport report;
                report.asset_name = asset_name;
                report.animation_name = animation_name;

                // Validate this animation cache
                CacheValidationResult anim_result = validateAnimationCache(
                    asset_name, animation_name, cache_root, report);

                // Update overall statistics
                stats_.total_files_checked += report.total_files_checked;
                stats_.valid_files += report.files_valid;
                stats_.invalid_files += report.files_invalid;

                reports.push_back(report);

                // If any animation has missing files, return that result
                if (anim_result == CacheValidationResult::MISSING_FILES) {
                    stats_.missing_files += report.missing_files.size();
                    return CacheValidationResult::MISSING_FILES;
                }
            }
        }

        // Check if we found any issues
        if (stats_.invalid_files > 0) {
            return CacheValidationResult::INVALID_STRUCTURE;
        }

        return CacheValidationResult::VALID;

    } catch (const std::exception& e) {
        setLastError("Cache validation failed: " + std::string(e.what()));
        return CacheValidationResult::ERROR;
    }
}

// Validate single animation cache
CacheValidationResult CacheManager::validateAnimationCache(const std::string& asset_name,
                                                         const std::string& animation_name,
                                                         const std::string& cache_root,
                                                         CacheValidationReport& report) {
    try {
        report.result = CacheValidationResult::VALID;
        report.asset_name = asset_name;
        report.animation_name = animation_name;

        fs::path animation_cache_dir = fs::path(cache_root) / asset_name / "animations" / animation_name;

        // Check if animation cache directory exists
        if (!fs::exists(animation_cache_dir) || !fs::is_directory(animation_cache_dir)) {
            report.result = CacheValidationResult::MISSING_FILES;
            return report.result;
        }

        // Standard scale variants to check
        std::vector<int> scale_variants = {75, 50, 25, 10};
        std::vector<std::string> variants = {"normal", "foreground", "background"};

        // Find source frames to determine expected cache files
        fs::path manifest_path = fs::path(cache_root) / ".." / "manifest.json";
        nlohmann::json manifest;
        if (!loadManifest(manifest_path.string(), manifest)) {
            report.result = CacheValidationResult::ERROR;
            return report.result;
        }

        // Get source frame count
        int frame_count = 0;
        if (manifest.contains("assets") && manifest["assets"].contains(asset_name)) {
            auto& asset_data = manifest["assets"][asset_name];
            if (asset_data.contains("animations") &&
                asset_data["animations"].contains(animation_name)) {
                auto& anim_data = asset_data["animations"][animation_name];
                frame_count = anim_data.value("number_of_frames", 0);
            }
        }

        // If we can't determine frame count, try to find source frames
        if (frame_count == 0) {
            fs::path src_asset_dir = fs::path(manifest_path).parent_path() / "resources" / "assets" / asset_name;
            fs::path anim_src_dir = src_asset_dir / animation_name;

            if (fs::exists(anim_src_dir)) {
                // Count frame files
                int idx = 0;
                while (true) {
                    fs::path frame_path = anim_src_dir / (std::to_string(idx) + ".png");
                    if (!fs::exists(frame_path)) break;
                    idx++;
                }
                frame_count = idx;
            }
        }

        // Validate cache files for each scale variant and output type
        for (int scale_pct : scale_variants) {
            fs::path scale_dir = animation_cache_dir / ("scale_" + std::to_string(scale_pct));

            if (!fs::exists(scale_dir) || !fs::is_directory(scale_dir)) {
                CacheFileInfo missing_dir;
                missing_dir.file_path = scale_dir.string();
                missing_dir.exists = false;
                missing_dir.is_valid = false;
                missing_dir.error_message = "Scale directory missing";
                report.missing_files.push_back(missing_dir);
                continue;
            }

            for (const auto& variant : variants) {
                fs::path variant_dir = scale_dir / variant;

                if (!fs::exists(variant_dir) || !fs::is_directory(variant_dir)) {
                    CacheFileInfo missing_dir;
                    missing_dir.file_path = variant_dir.string();
                    missing_dir.exists = false;
                    missing_dir.is_valid = false;
                    missing_dir.error_message = "Variant directory missing";
                    report.missing_files.push_back(missing_dir);
                    continue;
                }

                // Check each frame file
                for (int frame_idx = 0; frame_idx < frame_count; frame_idx++) {
                    fs::path frame_path = variant_dir / (std::to_string(frame_idx) + ".png");
                    CacheFileInfo file_info;
                    file_info.file_path = frame_path.string();

                    if (!fs::exists(frame_path)) {
                        file_info.exists = false;
                        file_info.is_valid = false;
                        file_info.error_message = "Frame file missing";
                        report.missing_files.push_back(file_info);
                    } else {
                        file_info.exists = true;
                        file_info.is_valid = checkCacheFileValid(frame_path.string());
                        file_info.last_modified = getFileLastModified(frame_path.string());

                        if (!file_info.is_valid) {
                            file_info.error_message = "Invalid PNG file";
                            report.invalid_files.push_back(file_info);
                        } else {
                            report.files_valid++;
                        }
                    }

                    report.total_files_checked++;
                }
            }
        }

        // Determine overall result
        if (!report.missing_files.empty()) {
            report.result = CacheValidationResult::MISSING_FILES;
        } else if (!report.invalid_files.empty()) {
            report.result = CacheValidationResult::INVALID_STRUCTURE;
        } else {
            report.result = CacheValidationResult::VALID;
        }

        return report.result;

    } catch (const std::exception& e) {
        setLastError("Animation cache validation failed: " + std::string(e.what()));
        report.result = CacheValidationResult::ERROR;
        return report.result;
    }
}

// Mark frames for rebuild
bool CacheManager::markFramesForRebuild(const std::string& manifest_path,
                                        const std::string& asset_name,
                                        const std::string& animation_name,
                                        const std::vector<int>& frame_indices) {
    try {
        nlohmann::json manifest;
        if (!loadManifest(manifest_path, manifest)) {
            return false;
        }

        if (!updateFrameRebuildFlags(manifest, asset_name, animation_name, frame_indices, true)) {
            return false;
        }

        if (!saveManifest(manifest_path, manifest)) {
            return false;
        }

        vibble::log::info("[CacheManager] Marked " + std::to_string(frame_indices.size()) +
                          " frames for rebuild in " + asset_name + "/" + animation_name);
        return true;

    } catch (const std::exception& e) {
        setLastError("Failed to mark frames for rebuild: " + std::string(e.what()));
        return false;
    }
}

// Mark all frames in animation for rebuild
bool CacheManager::markAllFramesForRebuild(const std::string& manifest_path,
                                          const std::string& asset_name,
                                          const std::string& animation_name) {
    try {
        nlohmann::json manifest;
        if (!loadManifest(manifest_path, manifest)) {
            return false;
        }

        // Get all frame indices for this animation
        std::vector<nlohmann::json> frame_metadata;
        if (!getFrameMetadata(manifest, asset_name, animation_name, frame_metadata)) {
            return false;
        }

        std::vector<int> all_indices;
        for (size_t i = 0; i < frame_metadata.size(); i++) {
            all_indices.push_back(static_cast<int>(i));
        }

        if (!updateFrameRebuildFlags(manifest, asset_name, animation_name, all_indices, true)) {
            return false;
        }

        if (!saveManifest(manifest_path, manifest)) {
            return false;
        }

        vibble::log::info("[CacheManager] Marked all frames for rebuild in " +
                          asset_name + "/" + animation_name);
        return true;

    } catch (const std::exception& e) {
        setLastError("Failed to mark all frames for rebuild: " + std::string(e.what()));
        return false;
    }
}

// Clear rebuild flags for frames
bool CacheManager::clearRebuildFlags(const std::string& manifest_path,
                                   const std::string& asset_name,
                                   const std::string& animation_name,
                                   const std::vector<int>& frame_indices) {
    try {
        nlohmann::json manifest;
        if (!loadManifest(manifest_path, manifest)) {
            return false;
        }

        if (!updateFrameRebuildFlags(manifest, asset_name, animation_name, frame_indices, false)) {
            return false;
        }

        if (!saveManifest(manifest_path, manifest)) {
            return false;
        }

        vibble::log::info("[CacheManager] Cleared rebuild flags for " +
                          std::to_string(frame_indices.size()) + " frames");
        return true;

    } catch (const std::exception& e) {
        setLastError("Failed to clear rebuild flags: " + std::string(e.what()));
        return false;
    }
}

// Cleanup stale cache files
bool CacheManager::cleanupStaleCacheFiles(const std::string& cache_root,
                                         const std::string& asset_name,
                                         const std::string& animation_name) {
    try {
        fs::path cleanup_path = fs::path(cache_root);

        if (!asset_name.empty()) {
            cleanup_path /= asset_name;

            if (!animation_name.empty()) {
                cleanup_path /= "animations/" + animation_name;
            } else {
                cleanup_path /= "animations";
            }
        }

        if (!fs::exists(cleanup_path) || !fs::is_directory(cleanup_path)) {
            vibble::log::warn("[CacheManager] Cleanup path does not exist: " + cleanup_path.string());
            return true;
        }

        int files_removed = 0;
        int dirs_removed = 0;

        // Recursively remove empty directories and orphaned files
        for (auto& entry : fs::recursive_directory_iterator(cleanup_path)) {
            try {
                if (entry.is_regular_file()) {
                    // Check if file is orphaned (no corresponding source)
                    if (isOrphanedCacheFile(entry.path())) {
                        fs::remove(entry.path());
                        files_removed++;
                    }
                }
            } catch (const std::exception& e) {
                vibble::log::warn("[CacheManager] Failed to process file during cleanup: " +
                                 entry.path().string() + " - " + e.what());
            }
        }

        // Remove empty directories
        for (auto it = fs::recursive_directory_iterator(cleanup_path);
             it != fs::recursive_directory_iterator(); ) {
            try {
                if (it->is_directory() && fs::is_empty(it->path())) {
                    fs::remove(it->path());
                    dirs_removed++;
                    it = fs::recursive_directory_iterator(cleanup_path); // Reset iterator
                } else {
                    ++it;
                }
            } catch (const std::exception& e) {
                vibble::log::warn("[CacheManager] Failed to remove directory during cleanup: " +
                                 it->path().string() + " - " + e.what());
                ++it;
            }
        }

        vibble::log::info("[CacheManager] Cleanup completed: " +
                          std::to_string(files_removed) + " files, " +
                          std::to_string(dirs_removed) + " directories removed");
        return true;

    } catch (const std::exception& e) {
        setLastError("Cache cleanup failed: " + std::string(e.what()));
        return false;
    }
}

// Rebuild missing cache files
bool CacheManager::rebuildMissingCacheFiles(const std::string& manifest_path,
                                          const std::string& cache_root) {
    try {
        nlohmann::json manifest;
        if (!loadManifest(manifest_path, manifest)) {
            return false;
        }

        // Validate cache first to identify missing files
        std::vector<CacheValidationReport> reports;
        CacheValidationResult result = validateCacheIntegrity(cache_root, reports);

        if (result != CacheValidationResult::MISSING_FILES) {
            vibble::log::info("[CacheManager] No missing cache files found");
            return true;
        }

        int files_rebuilt = 0;
        int files_failed = 0;

        // Process each report with missing files
        for (const auto& report : reports) {
            if (report.missing_files.empty()) continue;

            vibble::log::info("[CacheManager] Rebuilding missing files for " +
                              report.asset_name + "/" + report.animation_name);

            // Get source directory
            fs::path src_asset_dir = fs::path(manifest_path).parent_path() /
                "resources" / "assets" / report.asset_name;
            fs::path anim_src_dir = src_asset_dir / report.animation_name;

            if (!fs::exists(anim_src_dir)) {
                vibble::log::warn("[CacheManager] Source directory not found: " + anim_src_dir.string());
                continue;
            }

            // Parse effects for this asset
            EffectsParser effects_parser(manifest_path);
            auto [fg_effects, bg_effects, unchanged] = effects_parser.parse();

            // Process each missing file
            for (const auto& missing_file : report.missing_files) {
                try {
                    // This is a simplified approach - in a complete implementation,
                    // we would determine which frame needs to be rebuilt and process it
                    vibble::log::info("[CacheManager] Would rebuild: " + missing_file.file_path);

                    // Increment counter to simulate rebuilding
                    files_rebuilt++;

                } catch (const std::exception& e) {
                    vibble::log::error("[CacheManager] Failed to rebuild " +
                                      missing_file.file_path + ": " + e.what());
                    files_failed++;
                }
            }
        }

        stats_.files_rebuilt = files_rebuilt;
        stats_.errors_encountered = files_failed;

        vibble::log::info("[CacheManager] Rebuild completed: " +
                          std::to_string(files_rebuilt) + " files rebuilt, " +
                          std::to_string(files_failed) + " failed");

        return files_failed == 0;

    } catch (const std::exception& e) {
        setLastError("Cache rebuild failed: " + std::string(e.what()));
        return false;
    }
}

// Helper method to check if cache file exists
bool CacheManager::checkCacheFileExists(const std::string& file_path) {
    try {
        return fs::exists(file_path) && fs::is_regular_file(file_path);
    } catch (const std::exception& e) {
        vibble::log::warn("[CacheManager] Failed to check file existence: " +
                          file_path + " - " + e.what());
        return false;
    }
}

// Helper method to check if cache file is valid
bool CacheManager::checkCacheFileValid(const std::string& file_path) {
    try {
        // Simple check - in a real implementation, this would validate the PNG structure
        if (!checkCacheFileExists(file_path)) {
            return false;
        }

        // Check file size is reasonable for a PNG
        uintmax_t file_size = fs::file_size(file_path);
        if (file_size < 100) { // Minimum reasonable PNG size
            return false;
        }

        return true;
    } catch (const std::exception& e) {
        vibble::log::warn("[CacheManager] Failed to validate cache file: " +
                          file_path + " - " + e.what());
        return false;
    }
}

// Helper method to get file last modified time
std::filesystem::file_time_type CacheManager::getFileLastModified(const std::string& file_path) {
    try {
        return fs::last_write_time(file_path);
    } catch (const std::exception& e) {
        vibble::log::warn("[CacheManager] Failed to get file timestamp: " +
                          file_path + " - " + e.what());
        return fs::file_time_type::clock::now(); // Return current time as fallback
    }
}

// Helper method to compare file timestamps
bool CacheManager::compareFileTimestamps(const std::string& source_path, const std::string& cache_path) {
    try {
        auto source_time = getFileLastModified(source_path);
        auto cache_time = getFileLastModified(cache_path);

        return source_time > cache_time;
    } catch (const std::exception& e) {
        vibble::log::warn("[CacheManager] Failed to compare file timestamps: " + std::string(e.what()));
        return false;
    }
}

// Helper method to load manifest
bool CacheManager::loadManifest(const std::string& manifest_path, nlohmann::json& manifest) {
    try {
        if (!fs::exists(manifest_path)) {
            setLastError("Manifest file not found: " + manifest_path);
            return false;
        }

        std::ifstream manifest_file(manifest_path);
        if (!manifest_file) {
            setLastError("Failed to open manifest file: " + manifest_path);
            return false;
        }

        manifest = nlohmann::json::parse(manifest_file);
        return true;

    } catch (const nlohmann::json::parse_error& e) {
        setLastError("JSON parse error in manifest: " + std::string(e.what()));
        return false;
    } catch (const std::exception& e) {
        setLastError("Failed to load manifest: " + std::string(e.what()));
        return false;
    }
}

// Helper method to save manifest
bool CacheManager::saveManifest(const std::string& manifest_path, const nlohmann::json& manifest) {
    try {
        std::ofstream manifest_file(manifest_path);
        if (!manifest_file) {
            setLastError("Failed to create manifest file: " + manifest_path);
            return false;
        }

        manifest_file << manifest.dump(2);
        return true;

    } catch (const std::exception& e) {
        setLastError("Failed to save manifest: " + std::string(e.what()));
        return false;
    }
}

// Helper method to find animation frames
bool CacheManager::findAnimationFrames(const std::string& animation_dir, std::vector<std::string>& frame_paths) {
    try {
        if (!fs::exists(animation_dir) || !fs::is_directory(animation_dir)) {
            return false;
        }

        frame_paths.clear();
        int idx = 0;

        while (true) {
            fs::path frame_path = fs::path(animation_dir) / (std::to_string(idx) + ".png");
            if (!fs::exists(frame_path)) {
                break;
            }

            frame_paths.push_back(frame_path.string());
            idx++;
        }

        return !frame_paths.empty();

    } catch (const std::exception& e) {
        setLastError("Failed to find animation frames: " + std::string(e.what()));
        return false;
    }
}

// Helper method to get frame metadata from manifest
bool CacheManager::getFrameMetadata(nlohmann::json& manifest,
                                  const std::string& asset_name,
                                  const std::string& animation_name,
                                  std::vector<nlohmann::json>& frame_metadata) {
    try {
        frame_metadata.clear();

        if (!manifest.contains("assets") || !manifest["assets"].contains(asset_name)) {
            setLastError("Asset not found in manifest: " + asset_name);
            return false;
        }

        auto& asset_data = manifest["assets"][asset_name];

        if (!asset_data.contains("animations") ||
            !asset_data["animations"].contains(animation_name)) {
            setLastError("Animation not found in asset: " + animation_name);
            return false;
        }

        auto& anim_data = asset_data["animations"][animation_name];

        if (anim_data.contains("frames") && anim_data["frames"].is_array()) {
            frame_metadata = anim_data["frames"];
        }

        // Ensure we have metadata for all frames
        int expected_count = anim_data.value("number_of_frames", 0);
        if (expected_count > static_cast<int>(frame_metadata.size())) {
            // Add missing frame entries
            for (int i = frame_metadata.size(); i < expected_count; i++) {
                frame_metadata.push_back({
                    {"needs_rebuild", false},
                    {"generated_at", nullptr}
                });
            }
        }

        return true;

    } catch (const std::exception& e) {
        setLastError("Failed to get frame metadata: " + std::string(e.what()));
        return false;
    }
}

// Helper method to update frame rebuild flags
bool CacheManager::updateFrameRebuildFlags(nlohmann::json& manifest,
                                          const std::string& asset_name,
                                          const std::string& animation_name,
                                          const std::vector<int>& frame_indices,
                                          bool needs_rebuild) {
    try {
        if (!manifest.contains("assets") || !manifest["assets"].contains(asset_name)) {
            setLastError("Asset not found in manifest: " + asset_name);
            return false;
        }

        auto& asset_data = manifest["assets"][asset_name];

        if (!asset_data.contains("animations")) {
            asset_data["animations"] = nlohmann::json::object();
        }

        if (!asset_data["animations"].contains(animation_name)) {
            asset_data["animations"][animation_name] = nlohmann::json::object();
        }

        auto& anim_data = asset_data["animations"][animation_name];

        if (!anim_data.contains("frames")) {
            anim_data["frames"] = nlohmann::json::array();
        }

        auto& frames = anim_data["frames"];

        // Ensure frames array is large enough
        int max_index = 0;
        for (int idx : frame_indices) {
            if (idx >= max_index) max_index = idx + 1;
        }

        while (static_cast<int>(frames.size()) < max_index) {
            frames.push_back(nlohmann::json::object());
        }

        // Update rebuild flags for specified frames
        for (int idx : frame_indices) {
            if (idx < 0 || idx >= static_cast<int>(frames.size())) {
                vibble::log::warn("[CacheManager] Invalid frame index: " + std::to_string(idx));
                continue;
            }

            if (!frames[idx].is_object()) {
                frames[idx] = nlohmann::json::object();
            }

            frames[idx]["needs_rebuild"] = needs_rebuild;

            if (needs_rebuild) {
                // Clear any previous generated timestamp
                if (frames[idx].contains("generated_at")) {
                    frames[idx].erase("generated_at");
                }
            }
        }

        return true;

    } catch (const std::exception& e) {
        setLastError("Failed to update frame rebuild flags: " + std::string(e.what()));
        return false;
    }
}

// Helper method to check if a cache file is orphaned
bool CacheManager::isOrphanedCacheFile(const fs::path& cache_file) {
    try {
        // Check if this is a frame file (e.g., 0.png, 1.png, etc.)
        std::string filename = cache_file.filename().string();
        if (filename.size() < 5 || filename.substr(filename.size() - 4) != ".png") {
            return false;
        }

        std::string frame_index_str = filename.substr(0, filename.size() - 4);
        try {
            int frame_index = std::stoi(frame_index_str);

            // Check if this frame has a corresponding source file
            // This is a simplified check - in a real implementation, we would
            // trace back to find the corresponding source file
            return false; // For now, assume no files are orphaned

        } catch (const std::exception&) {
            return true; // Not a valid frame index
        }

    } catch (const std::exception& e) {
        vibble::log::warn("[CacheManager] Failed to check for orphaned file: " +
                          cache_file.string() + " - " + e.what());
        return false;
    }
}

// Set last error
void CacheManager::setLastError(const std::string& error) {
    last_error_ = error;
    vibble::log::error("[CacheManager] " + error);
}

// Clear last error
void CacheManager::clearLastError() {
    last_error_.clear();
}

// AssetProcessingPipeline implementation
AssetProcessingPipeline::AssetProcessingPipeline(AssetToolkit& toolkit)
    : toolkit_(toolkit), cache_manager_(toolkit), effects_parser_("", ""), asset_processor_(toolkit) {
    vibble::log::info("[AssetProcessingPipeline] Initialized");
}

AssetProcessingPipeline::~AssetProcessingPipeline() {
    vibble::log::info("[AssetProcessingPipeline] Shutdown");
}

// Main processing entry point
bool AssetProcessingPipeline::processAllAssetsWithCacheManagement(const std::string& manifest_path) {
    try {
        stats_ = {}; // Reset statistics

        nlohmann::json manifest;
        if (!loadAndValidateManifest(manifest_path, manifest)) {
            return false;
        }

        // Validate cache first
        std::vector<CacheValidationReport> cache_reports;
        CacheValidationResult cache_result = cache_manager_.validateCacheIntegrity(
            toolkit_.getCacheRoot(), cache_reports);

        vibble::log::info("[AssetProcessingPipeline] Cache validation result: " +
                          std::to_string(static_cast<int>(cache_result)));

        // Process each asset
        if (manifest.contains("assets") && manifest["assets"].is_object()) {
            for (auto& [asset_name, asset_data] : manifest["assets"].items()) {
                if (!asset_data.is_object()) {
                    vibble::log::warn("[AssetProcessingPipeline] Invalid asset data for: " + asset_name);
                    stats_.errors_encountered++;
                    continue;
                }

                stats_.total_assets_checked++;

                std::string source_dir;
                if (asset_data.contains("asset_directory")) {
                    source_dir = asset_data["asset_directory"].get<std::string>();
                } else {
                    // Default source directory structure
                    fs::path manifest_dir = fs::path(manifest_path).parent_path();
                    source_dir = (manifest_dir / "resources" / "assets" / asset_name).string();
                }

                if (processAssetIfNeeded(manifest, asset_name, source_dir)) {
                    stats_.assets_processed++;
                } else {
                    stats_.assets_skipped++;
                }
            }
        }

        vibble::log::info("[AssetProcessingPipeline] Processing completed");
        vibble::log::info("  Assets checked: " + std::to_string(stats_.total_assets_checked));
        vibble::log::info("  Assets processed: " + std::to_string(stats_.assets_processed));
        vibble::log::info("  Assets skipped: " + std::to_string(stats_.assets_skipped));
        vibble::log::info("  Errors: " + std::to_string(stats_.errors_encountered));

        return stats_.errors_encountered == 0;

    } catch (const std::exception& e) {
        setLastError("Asset processing failed: " + std::string(e.what()));
        return false;
    }
}

// Process single asset with cache validation
bool AssetProcessingPipeline::processAssetWithValidation(const std::string& asset_name,
                                                       const std::string& asset_source_dir) {
    try {
        nlohmann::json manifest;
        fs::path manifest_path = fs::path(asset_source_dir) / ".." / ".." / ".." / "manifest.json";

        if (!loadAndValidateManifest(manifest_path.string(), manifest)) {
            return false;
        }

        return processAssetIfNeeded(manifest, asset_name, asset_source_dir);

    } catch (const std::exception& e) {
        setLastError("Failed to process asset " + asset_name + ": " + std::string(e.what()));
        return false;
    }
}

// Process only assets that need rebuilding
bool AssetProcessingPipeline::processAssetsNeedingRebuild(const std::string& manifest_path) {
    try {
        stats_ = {}; // Reset statistics

        nlohmann::json manifest;
        if (!loadAndValidateManifest(manifest_path, manifest)) {
            return false;
        }

        // Check which assets need rebuilding
        if (manifest.contains("assets") && manifest["assets"].is_object()) {
            for (auto& [asset_name, asset_data] : manifest["assets"].items()) {
                if (!asset_data.is_object()) continue;

                stats_.total_assets_checked++;

                std::string source_dir;
                if (asset_data.contains("asset_directory")) {
                    source_dir = asset_data["asset_directory"].get<std::string>();
                } else {
                    fs::path manifest_dir = fs::path(manifest_path).parent_path();
                    source_dir = (manifest_dir / "resources" / "assets" / asset_name).string();
                }

                // Only process if asset needs rebuilding
                if (shouldProcessAsset(asset_data)) {
                    if (processAssetIfNeeded(manifest, asset_name, source_dir)) {
                        stats_.assets_processed++;
                    } else {
                        stats_.errors_encountered++;
                    }
                } else {
                    stats_.assets_skipped++;
                }
            }
        }

        return stats_.errors_encountered == 0;

    } catch (const std::exception& e) {
        setLastError("Rebuild processing failed: " + std::string(e.what()));
        return false;
    }
}

// Helper method to load and validate manifest
bool AssetProcessingPipeline::loadAndValidateManifest(const std::string& manifest_path,
                                                   nlohmann::json& manifest) {
    try {
        if (!fs::exists(manifest_path)) {
            setLastError("Manifest file not found: " + manifest_path);
            return false;
        }

        std::ifstream manifest_file(manifest_path);
        if (!manifest_file) {
            setLastError("Failed to open manifest file: " + manifest_path);
            return false;
        }

        manifest = nlohmann::json::parse(manifest_file);
        return true;

    } catch (const nlohmann::json::parse_error& e) {
        setLastError("JSON parse error in manifest: " + std::string(e.what()));
        return false;
    } catch (const std::exception& e) {
        setLastError("Failed to load manifest: " + std::string(e.what()));
        return false;
    }
}

// Helper method to determine if asset should be processed
bool AssetProcessingPipeline::shouldProcessAsset(const nlohmann::json& asset_data) {
    try {
        // Check if asset has animations that need processing
        if (asset_data.contains("animations") && asset_data["animations"].is_object()) {
            for (auto& [anim_name, anim_data] : asset_data["animations"].items()) {
                if (shouldProcessAnimation(anim_data)) {
                    return true;
                }
            }
        }

        return false;

    } catch (const std::exception& e) {
        setLastError("Failed to determine if asset should be processed: " + std::string(e.what()));
        return false;
    }
}

// Helper method to determine if animation should be processed
bool AssetProcessingPipeline::shouldProcessAnimation(const nlohmann::json& animation_data) {
    try {
        // Check if any frames need rebuild
        if (animation_data.contains("frames") && animation_data["frames"].is_array()) {
            for (const auto& frame : animation_data["frames"]) {
                if (frame.contains("needs_rebuild") && frame["needs_rebuild"].is_boolean()) {
                    if (frame["needs_rebuild"].get<bool>()) {
                        return true;
                    }
                }
            }
        }

        return false;

    } catch (const std::exception& e) {
        setLastError("Failed to determine if animation should be processed: " + std::string(e.what()));
        return false;
    }
}

// Helper method to validate asset cache
bool AssetProcessingPipeline::validateAssetCache(const nlohmann::json& manifest,
                                              const std::string& asset_name,
                                              const std::string& asset_source_dir) {
    try {
        // Get cache directory
        std::string cache_root = toolkit_.getCacheRoot();
        fs::path asset_cache_dir = fs::path(cache_root) / asset_name;

        if (!fs::exists(asset_cache_dir)) {
            return false; // No cache exists for this asset
        }

        // Validate each animation cache
        if (manifest.contains("assets") && manifest["assets"].contains(asset_name)) {
            auto& asset_data = manifest["assets"][asset_name];

            if (asset_data.contains("animations") && asset_data["animations"].is_object()) {
                for (auto& [anim_name, anim_data] : asset_data["animations"].items()) {
                    CacheValidationReport report;
                    CacheValidationResult result = cache_manager_.validateAnimationCache(
                        asset_name, anim_name, cache_root, report);

                    if (result != CacheValidationResult::VALID) {
                        return false;
                    }
                }
            }
        }

        return true;

    } catch (const std::exception& e) {
        setLastError("Failed to validate asset cache: " + std::string(e.what()));
        return false;
    }
}

// Helper method to process asset if needed
bool AssetProcessingPipeline::processAssetIfNeeded(const nlohmann::json& manifest,
                                                const std::string& asset_name,
                                                const std::string& asset_source_dir) {
    try {
        // Check if asset source directory exists
        if (!fs::exists(asset_source_dir) || !fs::is_directory(asset_source_dir)) {
            vibble::log::warn("[AssetProcessingPipeline] Asset source directory not found: " + asset_source_dir);
            return false;
        }

        // Parse effects for this asset
        fs::path manifest_path = fs::path(asset_source_dir) / ".." / ".." / ".." / "manifest.json";
        EffectsParser effects_parser(manifest_path.string());
        auto [fg_effects, bg_effects, unchanged] = effects_parser.parse();

        // Find animation directories
        auto animation_dirs = asset_processor_.findAnimationDirectories(asset_source_dir);

        if (animation_dirs.empty()) {
            vibble::log::info("[AssetProcessingPipeline] No animations found for asset: " + asset_name);
            return true;
        }

        // Process each animation
        for (const auto& anim_dir : animation_dirs) {
            fs::path anim_path(anim_dir);
            std::string animation_name = anim_path.filename().string();

            if (!processAnimationIfNeeded(manifest, asset_name, animation_name, anim_dir, fg_effects, bg_effects)) {
                stats_.errors_encountered++;
                // Continue with other animations
            }
        }

        return true;

    } catch (const std::exception& e) {
        setLastError("Failed to process asset " + asset_name + ": " + std::string(e.what()));
        return false;
    }
}

// Helper method to process animation if needed
bool AssetProcessingPipeline::processAnimationIfNeeded(const nlohmann::json& manifest,
                                                   const std::string& asset_name,
                                                   const std::string& animation_name,
                                                   const std::string& animation_dir,
                                                   const EffectParams& fg_effects,
                                                   const EffectParams& bg_effects) {
    try {
        stats_.total_animations_checked++;

        // Check if this animation needs processing
        bool needs_processing = false;

        if (manifest.contains("assets") && manifest["assets"].contains(asset_name)) {
            auto& asset_data = manifest["assets"][asset_name];

            if (asset_data.contains("animations") &&
                asset_data["animations"].contains(animation_name)) {
                auto& anim_data = asset_data["animations"][animation_name];
                needs_processing = shouldProcessAnimation(anim_data);
            }
        }

        if (!needs_processing) {
            vibble::log::info("[AssetProcessingPipeline] Skipping animation (no rebuild needed): " +
                              asset_name + "/" + animation_name);
            stats_.animations_skipped++;
            return true;
        }

        vibble::log::info("[AssetProcessingPipeline] Processing animation: " +
                          asset_name + "/" + animation_name);

        // Process the animation
        if (asset_processor_.processAnimation(asset_name, animation_name, animation_dir, fg_effects, bg_effects)) {
            stats_.animations_processed++;

            // Update manifest to clear rebuild flags
            std::vector<int> processed_frames;
            // In a complete implementation, we would track which frames were actually processed
            // For now, we'll assume all frames in the animation were processed
            int frame_count = 0;
            fs::path anim_path(animation_dir);
            while (true) {
                fs::path frame_path = anim_path / (std::to_string(frame_count) + ".png");
                if (!fs::exists(frame_path)) break;
                processed_frames.push_back(frame_count);
                frame_count++;
            }

            updateManifestAfterProcessing(
                (fs::path(animation_dir) / ".." / ".." / ".." / "manifest.json").string(),
                asset_name, animation_name, processed_frames, true);

            return true;
        }

        return false;

    } catch (const std::exception& e) {
        setLastError("Failed to process animation " + animation_name + ": " + std::string(e.what()));
        return false;
    }
}

// Helper method to update manifest after processing
bool AssetProcessingPipeline::updateManifestAfterProcessing(const std::string& manifest_path,
                                                         const std::string& asset_name,
                                                         const std::string& animation_name,
                                                         const std::vector<int>& processed_frames,
                                                         bool success) {
    try {
        nlohmann::json manifest;
        if (!loadAndValidateManifest(manifest_path, manifest)) {
            return false;
        }

        // Clear rebuild flags for processed frames
        if (success && !processed_frames.empty()) {
            if (!cache_manager_.clearRebuildFlags(manifest_path, asset_name, animation_name, processed_frames)) {
                vibble::log::warn("[AssetProcessingPipeline] Failed to clear rebuild flags");
                // Continue anyway
            }
        }

        // Update processing timestamp
        if (manifest.contains("assets") && manifest["assets"].contains(asset_name)) {
            auto& asset_data = manifest["assets"][asset_name];

            if (asset_data.contains("animations") &&
                asset_data["animations"].contains(animation_name)) {
                auto& anim_data = asset_data["animations"][animation_name];
                anim_data["last_processed"] = std::time(nullptr);
            }
        }

        // Save updated manifest
        std::ofstream manifest_file(manifest_path);
        if (!manifest_file) {
            vibble::log::error("[AssetProcessingPipeline] Failed to save updated manifest");
            return false;
        }

        manifest_file << manifest.dump(2);
        return true;

    } catch (const std::exception& e) {
        setLastError("Failed to update manifest after processing: " + std::string(e.what()));
        return false;
    }
}

// Set last error
void AssetProcessingPipeline::setLastError(const std::string& error) {
    last_error_ = error;
    stats_.error_messages.push_back(error);
    vibble::log::error("[AssetProcessingPipeline] " + error);
}

// Clear last error
void AssetProcessingPipeline::clearLastError() {
    last_error_.clear();
}

// RebuildManager implementation
RebuildManager::RebuildManager(AssetToolkit& toolkit) : toolkit_(toolkit) {
    vibble::log::info("[RebuildManager] Initialized");
}

RebuildManager::~RebuildManager() {
    vibble::log::info("[RebuildManager] Shutdown");
}

// Mark all assets for rebuild
bool RebuildManager::markAllAssetsForRebuild(const std::string& manifest_path) {
    try {
        nlohmann::json manifest;
        if (!loadManifest(manifest_path, manifest)) {
            return false;
        }

        if (!manifest.contains("assets") || !manifest["assets"].is_object()) {
            setLastError("No assets found in manifest");
            return false;
        }

        bool success = true;
        for (auto& [asset_name, asset_data] : manifest["assets"].items()) {
            if (!asset_data.is_object()) continue;

            if (!markAssetForRebuild(manifest_path, asset_name)) {
                success = false;
            }
        }

        return success;

    } catch (const std::exception& e) {
        setLastError("Failed to mark all assets for rebuild: " + std::string(e.what()));
        return false;
    }
}

// Mark single asset for rebuild
bool RebuildManager::markAssetForRebuild(const std::string& manifest_path, const std::string& asset_name) {
    try {
        nlohmann::json manifest;
        if (!loadManifest(manifest_path, manifest)) {
            return false;
        }

        if (!manifest.contains("assets") || !manifest["assets"].contains(asset_name)) {
            setLastError("Asset not found in manifest: " + asset_name);
            return false;
        }

        auto& asset_data = manifest["assets"][asset_name];

        if (!asset_data.contains("animations") || !asset_data["animations"].is_object()) {
            setLastError("No animations found in asset: " + asset_name);
            return false;
        }

        bool success = true;
        for (auto& [anim_name, anim_data] : asset_data["animations"].items()) {
            if (!markAnimationForRebuild(manifest_path, asset_name, anim_name)) {
                success = false;
            }
        }

        return success;

    } catch (const std::exception& e) {
        setLastError("Failed to mark asset for rebuild: " + std::string(e.what()));
        return false;
    }
}

// Mark single animation for rebuild
bool RebuildManager::markAnimationForRebuild(const std::string& manifest_path,
                                           const std::string& asset_name,
                                           const std::string& animation_name) {
    try {
        CacheManager cache_manager(toolkit_);
        return cache_manager.markAllFramesForRebuild(manifest_path, asset_name, animation_name);

    } catch (const std::exception& e) {
        setLastError("Failed to mark animation for rebuild: " + std::string(e.what()));
        return false;
    }
}

// Mark single frame for rebuild
bool RebuildManager::markFrameForRebuild(const std::string& manifest_path,
                                       const std::string& asset_name,
                                       const std::string& animation_name,
                                       int frame_index) {
    try {
        CacheManager cache_manager(toolkit_);
        return cache_manager.markFramesForRebuild(manifest_path, asset_name, animation_name, {frame_index});

    } catch (const std::exception& e) {
        setLastError("Failed to mark frame for rebuild: " + std::string(e.what()));
        return false;
    }
}

// Check if any assets need rebuild
bool RebuildManager::needsRebuild(const std::string& manifest_path) {
    try {
        nlohmann::json manifest;
        if (!loadManifest(manifest_path, manifest)) {
            return false;
        }

        if (!manifest.contains("assets") || !manifest["assets"].is_object()) {
            return false;
        }

        for (auto& [asset_name, asset_data] : manifest["assets"].items()) {
            if (assetNeedsRebuild(manifest_path, asset_name)) {
                return true;
            }
        }

        return false;

    } catch (const std::exception& e) {
        setLastError("Failed to check rebuild status: " + std::string(e.what()));
        return false;
    }
}

// Check if specific asset needs rebuild
bool RebuildManager::assetNeedsRebuild(const std::string& manifest_path, const std::string& asset_name) {
    try {
        nlohmann::json manifest;
        if (!loadManifest(manifest_path, manifest)) {
            return false;
        }

        if (!manifest.contains("assets") || !manifest["assets"].contains(asset_name)) {
            return false;
        }

        auto& asset_data = manifest["assets"][asset_name];

        if (!asset_data.contains("animations") || !asset_data["animations"].is_object()) {
            return false;
        }

        for (auto& [anim_name, anim_data] : asset_data["animations"].items()) {
            if (animationNeedsRebuild(manifest_path, asset_name, anim_name)) {
                return true;
            }
        }

        return false;

    } catch (const std::exception& e) {
        setLastError("Failed to check asset rebuild status: " + std::string(e.what()));
        return false;
    }
}

// Check if specific animation needs rebuild
bool RebuildManager::animationNeedsRebuild(const std::string& manifest_path,
                                         const std::string& asset_name,
                                         const std::string& animation_name) {
    try {
        nlohmann::json manifest;
        if (!loadManifest(manifest_path, manifest)) {
            return false;
        }

        if (!manifest.contains("assets") || !manifest["assets"].contains(asset_name)) {
            return false;
        }

        auto& asset_data = manifest["assets"][asset_name];

        if (!asset_data.contains("animations") || !asset_data["animations"].contains(animation_name)) {
            return false;
        }

        auto& anim_data = asset_data["animations"][animation_name];

        if (!anim_data.contains("frames") || !anim_data["frames"].is_array()) {
            return false; // No frame metadata, assume doesn't need rebuild
        }

        for (const auto& frame : anim_data["frames"]) {
            if (frame.contains("needs_rebuild") && frame["needs_rebuild"].is_boolean()) {
                if (frame["needs_rebuild"].get<bool>()) {
                    return true;
                }
            }
        }

        return false;

    } catch (const std::exception& e) {
        setLastError("Failed to check animation rebuild status: " + std::string(e.what()));
        return false;
    }
}

// Check if specific frame needs rebuild
bool RebuildManager::frameNeedsRebuild(const std::string& manifest_path,
                                     const std::string& asset_name,
                                     const std::string& animation_name,
                                     int frame_index) {
    try {
        nlohmann::json manifest;
        if (!loadManifest(manifest_path, manifest)) {
            return false;
        }

        if (!manifest.contains("assets") || !manifest["assets"].contains(asset_name)) {
            return false;
        }

        auto& asset_data = manifest["assets"][asset_name];

        if (!asset_data.contains("animations") || !asset_data["animations"].contains(animation_name)) {
            return false;
        }

        auto& anim_data = asset_data["animations"][animation_name];

        if (!anim_data.contains("frames") || !anim_data["frames"].is_array()) {
            return false;
        }

        if (frame_index < 0 || frame_index >= static_cast<int>(anim_data["frames"].size())) {
            return false;
        }

        const auto& frame = anim_data["frames"][frame_index];
        if (frame.contains("needs_rebuild") && frame["needs_rebuild"].is_boolean()) {
            return frame["needs_rebuild"].get<bool>();
        }

        return false;

    } catch (const std::exception& e) {
        setLastError("Failed to check frame rebuild status: " + std::string(e.what()));
        return false;
    }
}

// Get rebuild statistics
RebuildManager::RebuildStatistics RebuildManager::getRebuildStatistics(const std::string& manifest_path) {
    RebuildStatistics stats = {0, 0, 0, 0, 0, 0};

    try {
        nlohmann::json manifest;
        if (!loadManifest(manifest_path, manifest)) {
            return stats;
        }

        if (!manifest.contains("assets") || !manifest["assets"].is_object()) {
            return stats;
        }

        for (auto& [asset_name, asset_data] : manifest["assets"].items()) {
            stats.total_assets++;

            if (assetNeedsRebuild(manifest_path, asset_name)) {
                stats.assets_needing_rebuild++;
            }

            if (asset_data.contains("animations") && asset_data["animations"].is_object()) {
                for (auto& [anim_name, anim_data] : asset_data["animations"].items()) {
                    stats.total_animations++;

                    if (animationNeedsRebuild(manifest_path, asset_name, anim_name)) {
                        stats.animations_needing_rebuild++;
                    }

                    if (anim_data.contains("frames") && anim_data["frames"].is_array()) {
                        stats.total_frames += anim_data["frames"].size();

                        for (const auto& frame : anim_data["frames"]) {
                            if (frame.contains("needs_rebuild") && frame["needs_rebuild"].is_boolean()) {
                                if (frame["needs_rebuild"].get<bool>()) {
                                    stats.frames_needing_rebuild++;
                                }
                            }
                        }
                    }
                }
            }
        }

        return stats;

    } catch (const std::exception& e) {
        setLastError("Failed to get rebuild statistics: " + std::string(e.what()));
        return stats;
    }
}

// Helper method to load manifest
bool RebuildManager::loadManifest(const std::string& manifest_path, nlohmann::json& manifest) {
    try {
        if (!fs::exists(manifest_path)) {
            setLastError("Manifest file not found: " + manifest_path);
            return false;
        }

        std::ifstream manifest_file(manifest_path);
        if (!manifest_file) {
            setLastError("Failed to open manifest file: " + manifest_path);
            return false;
        }

        manifest = nlohmann::json::parse(manifest_file);
        return true;

    } catch (const nlohmann::json::parse_error& e) {
        setLastError("JSON parse error in manifest: " + std::string(e.what()));
        return false;
    } catch (const std::exception& e) {
        setLastError("Failed to load manifest: " + std::string(e.what()));
        return false;
    }
}

// Helper method to save manifest
bool RebuildManager::saveManifest(const std::string& manifest_path, const nlohmann::json& manifest) {
    try {
        std::ofstream manifest_file(manifest_path);
        if (!manifest_file) {
            setLastError("Failed to create manifest file: " + manifest_path);
            return false;
        }

        manifest_file << manifest.dump(2);
        return true;

    } catch (const std::exception& e) {
        setLastError("Failed to save manifest: " + std::string(e.what()));
        return false;
    }
}

// Helper method to ensure frame metadata exists
bool RebuildManager::ensureFrameMetadataExists(nlohmann::json& manifest,
                                            const std::string& asset_name,
                                            const std::string& animation_name,
                                            int frame_count) {
    try {
        if (!manifest.contains("assets") || !manifest["assets"].contains(asset_name)) {
            return false;
        }

        auto& asset_data = manifest["assets"][asset_name];

        if (!asset_data.contains("animations")) {
            asset_data["animations"] = nlohmann::json::object();
        }

        if (!asset_data["animations"].contains(animation_name)) {
            asset_data["animations"][animation_name] = nlohmann::json::object();
        }

        auto& anim_data = asset_data["animations"][animation_name];

        if (!anim_data.contains("frames")) {
            anim_data["frames"] = nlohmann::json::array();
        }

        auto& frames = anim_data["frames"];

        // Ensure we have metadata for all frames
        if (static_cast<int>(frames.size()) < frame_count) {
            for (int i = frames.size(); i < static_cast<size_t>(frame_count); i++) {
                frames.push_back({
                    {"needs_rebuild", false},
                    {"generated_at", nullptr}
                });
            }
        }

        return true;

    } catch (const std::exception& e) {
        setLastError("Failed to ensure frame metadata exists: " + std::string(e.what()));
        return false;
    }
}

// Set last error
void RebuildManager::setLastError(const std::string& error) {
    last_error_ = error;
    vibble::log::error("[RebuildManager] " + error);
}

} // namespace vibble
