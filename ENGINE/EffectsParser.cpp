#include "EffectsParser.hpp"
#include "utils/log.hpp"
#include <fstream>
#include <algorithm>

namespace vibble {

// Constructor
EffectsParser::EffectsParser(const std::string& manifest_path, const std::string& cache_path)
    : manifest_path_(manifest_path) {
    if (cache_path.empty()) {
        cache_path_ = getDefaultCachePath();
    } else {
        cache_path_ = cache_path;
    }
}

// Destructor
EffectsParser::~EffectsParser() {
    // Clean up any resources
}

// Parse effects from manifest and cache
std::tuple<EffectParams, EffectParams, bool> EffectsParser::parse() {
    clearLastError();

    // Try to load manifest
    if (!loadManifest()) {
        // If manifest loading fails, return default effects
        EffectParams default_fg, default_bg;
        default_fg.is_foreground = true;
        default_bg.is_foreground = false;
        return std::make_tuple(default_fg, default_bg, false);
    }

    // Try to load cache
    EffectParams cached_fg, cached_bg;
    bool cache_loaded = loadEffectsCache(cached_fg, cached_bg);

    // Parse effects from manifest
    EffectParams manifest_fg = extractEffectBlock("foreground");
    EffectParams manifest_bg = extractEffectBlock("background");

    // Mark foreground/background appropriately
    manifest_fg.is_foreground = true;
    manifest_bg.is_foreground = false;

    // Check if effects have changed
    bool unchanged = cache_loaded && !effectsChanged(manifest_fg, manifest_bg);

    // If effects have changed, save new cache
    if (!unchanged) {
        saveEffectsCache(manifest_fg, manifest_bg);
    }

    return std::make_tuple(manifest_fg, manifest_bg, unchanged);
}

// Save effects to cache
bool EffectsParser::saveEffectsCache(const EffectParams& fg_effects, const EffectParams& bg_effects) {
    try {
        nlohmann::json cache_data;
        cache_data["version"] = 1;
        cache_data["generated_at"] = std::time(nullptr);
        cache_data["foreground"] = fg_effects.toJson();
        cache_data["background"] = bg_effects.toJson();

        // Ensure directory exists
        std::filesystem::path cache_path(cache_path_);
        std::filesystem::path cache_dir = cache_path.parent_path();

        if (!cache_dir.empty() && !std::filesystem::exists(cache_dir)) {
            std::filesystem::create_directories(cache_dir);
        }

        std::ofstream cache_file(cache_path_);
        if (!cache_file) {
            setLastError("Failed to create cache file: " + cache_path_);
            return false;
        }

        cache_file << cache_data.dump(2);
        vibble::log::info("[EffectsParser] Saved effects cache to: " + cache_path_);
        return true;

    } catch (const std::exception& e) {
        setLastError("Failed to save effects cache: " + std::string(e.what()));
        return false;
    }
}

// Load effects from cache
bool EffectsParser::loadEffectsCache(EffectParams& fg_effects, EffectParams& bg_effects) {
    try {
        if (!std::filesystem::exists(cache_path_)) {
            return false;
        }

        std::ifstream cache_file(cache_path_);
        if (!cache_file) {
            setLastError("Failed to open cache file: " + cache_path_);
            return false;
        }

        nlohmann::json cache_data = nlohmann::json::parse(cache_file);

        if (cache_data.contains("foreground")) {
            fg_effects = EffectParams::fromJson(cache_data["foreground"]);
        }

        if (cache_data.contains("background")) {
            bg_effects = EffectParams::fromJson(cache_data["background"]);
        }

        vibble::log::info("[EffectsParser] Loaded effects cache from: " + cache_path_);
        return true;

    } catch (const nlohmann::json::parse_error& e) {
        setLastError("JSON parse error in cache: " + std::string(e.what()));
        return false;
    } catch (const std::exception& e) {
        setLastError("Failed to load effects cache: " + std::string(e.what()));
        return false;
    }
}

// Check if effects have changed
bool EffectsParser::effectsChanged(const EffectParams& fg_effects, const EffectParams& bg_effects) {
    try {
        // Load current cache
        EffectParams cached_fg, cached_bg;
        if (!loadEffectsCache(cached_fg, cached_bg)) {
            // No cache exists, so effects have "changed"
            return true;
        }

        // Compare foreground effects
        bool fg_changed = (std::abs(fg_effects.brightness - cached_fg.brightness) > 1e-6f) ||
                         (std::abs(fg_effects.contrast - cached_fg.contrast) > 1e-6f) ||
                         (std::abs(fg_effects.blur - cached_fg.blur) > 1e-6f) ||
                         (std::abs(fg_effects.saturation_red - cached_fg.saturation_red) > 1e-6f) ||
                         (std::abs(fg_effects.saturation_green - cached_fg.saturation_green) > 1e-6f) ||
                         (std::abs(fg_effects.saturation_blue - cached_fg.saturation_blue) > 1e-6f) ||
                         (std::abs(fg_effects.hue - cached_fg.hue) > 1e-6f);

        // Compare background effects
        bool bg_changed = (std::abs(bg_effects.brightness - cached_bg.brightness) > 1e-6f) ||
                         (std::abs(bg_effects.contrast - cached_bg.contrast) > 1e-6f) ||
                         (std::abs(bg_effects.blur - cached_bg.blur) > 1e-6f) ||
                         (std::abs(bg_effects.saturation_red - cached_bg.saturation_red) > 1e-6f) ||
                         (std::abs(bg_effects.saturation_green - cached_bg.saturation_green) > 1e-6f) ||
                         (std::abs(bg_effects.saturation_blue - cached_bg.saturation_blue) > 1e-6f) ||
                         (std::abs(bg_effects.hue - cached_bg.hue) > 1e-6f);

        return fg_changed || bg_changed;

    } catch (const std::exception& e) {
        setLastError("Failed to compare effects: " + std::string(e.what()));
        return true; // Assume changed if comparison fails
    }
}

// Load manifest file
bool EffectsParser::loadManifest() {
    try {
        if (!std::filesystem::exists(manifest_path_)) {
            setLastError("Manifest file not found: " + manifest_path_);
            return false;
        }

        std::ifstream manifest_file(manifest_path_);
        if (!manifest_file) {
            setLastError("Failed to open manifest file: " + manifest_path_);
            return false;
        }

        manifest_data_ = nlohmann::json::parse(manifest_file);
        vibble::log::info("[EffectsParser] Loaded manifest from: " + manifest_path_);
        return true;

    } catch (const nlohmann::json::parse_error& e) {
        setLastError("JSON parse error in manifest: " + std::string(e.what()));
        return false;
    } catch (const std::exception& e) {
        setLastError("Failed to load manifest: " + std::string(e.what()));
        return false;
    }
}

// Extract effect block from manifest
EffectParams EffectsParser::extractEffectBlock(const std::string& block_name) {
    EffectParams params;

    try {
        if (!manifest_data_.contains("image_effects")) {
            vibble::log::warn("[EffectsParser] No 'image_effects' section in manifest");
            return params;
        }

        const auto& image_effects = manifest_data_["image_effects"];
        if (!image_effects.contains(block_name)) {
            vibble::log::warn("[EffectsParser] No '" + block_name + "' block in image_effects");
            return params;
        }

        const auto& effect_block = image_effects[block_name];

        // Extract parameters with validation
        params.brightness = effect_block.value("brightness", 0.0f);
        params.contrast = effect_block.value("contrast", 0.0f);
        params.blur = effect_block.value("blur", 0.0f);
        params.saturation_red = effect_block.value("saturation_red", 0.0f);
        params.saturation_green = effect_block.value("saturation_green", 0.0f);
        params.saturation_blue = effect_block.value("saturation_blue", 0.0f);
        params.hue = effect_block.value("hue", 0.0f);

        // Validate parameters
        if (!params.validate()) {
            vibble::log::warn("[EffectsParser] Invalid parameters in " + block_name + " block");
            // Reset to defaults if invalid
            params = EffectParams();
        }

        return params;

    } catch (const std::exception& e) {
        setLastError("Failed to extract " + block_name + " effects: " + std::string(e.what()));
        return EffectParams();
    }
}

// Get default cache path
std::string EffectsParser::getDefaultCachePath() const {
    try {
        std::filesystem::path manifest_path(manifest_path_);
        std::filesystem::path manifest_dir = manifest_path.parent_path();
        return (manifest_dir / ".image_effects_cache.json").string();
    } catch (const std::exception& e) {
        setLastError("Failed to determine default cache path: " + std::string(e.what()));
        return "";
    }
}

// Set last error
void EffectsParser::setLastError(const std::string& error) {
    last_error_ = error;
    vibble::log::error("[EffectsParser] " + error);
}

// Get last error
std::string EffectsParser::getLastError() const {
    return last_error_;
}

// Clear last error
void EffectsParser::clearLastError() {
    last_error_.clear();
}

// AssetProcessor implementation
AssetProcessor::AssetProcessor(AssetToolkit& toolkit) : toolkit_(toolkit) {
    vibble::log::info("[AssetProcessor] Initialized");
}

// Destructor
AssetProcessor::~AssetProcessor() {
    vibble::log::info("[AssetProcessor] Shutdown");
}

// Process all assets in manifest
bool AssetProcessor::processAllAssets(const std::string& manifest_path) {
    try {
        nlohmann::json manifest;
        if (!loadManifest(manifest_path, manifest)) {
            return false;
        }

        // Parse effects
        EffectsParser effects_parser(manifest_path);
        auto [fg_effects, bg_effects, unchanged] = effects_parser.parse();

        vibble::log::info("[AssetProcessor] Effects unchanged: " + std::string(unchanged ? "true" : "false"));

        // Process each asset
        if (manifest.contains("assets") && manifest["assets"].is_object()) {
            for (auto& [asset_name, asset_data] : manifest["assets"].items()) {
                if (!asset_data.is_object()) {
                    vibble::log::warn("[AssetProcessor] Invalid asset data for: " + asset_name);
                    continue;
                }

                std::string source_dir;
                if (asset_data.contains("asset_directory")) {
                    source_dir = asset_data["asset_directory"].get<std::string>();
                } else {
                    // Default source directory structure
                    std::filesystem::path manifest_dir = std::filesystem::path(manifest_path).parent_path();
                    source_dir = (manifest_dir / "SRC" / "assets" / asset_name).string();
                }

                if (!processAsset(asset_name, source_dir)) {
                    vibble::log::error("[AssetProcessor] Failed to process asset: " + asset_name);
                    // Continue with other assets
                }
            }
        }

        vibble::log::info("[AssetProcessor] Processed all assets");
        vibble::log::info("[AssetProcessor] Total frames: " + std::to_string(total_processed_frames_));
        vibble::log::info("[AssetProcessor] Failed frames: " + std::to_string(total_failed_frames_));

        return true;

    } catch (const std::exception& e) {
        setLastError("Failed to process all assets: " + std::string(e.what()));
        return false;
    }
}

// Process single asset
bool AssetProcessor::processAsset(const std::string& asset_name, const std::string& asset_source_dir) {
    try {
        vibble::log::info("[AssetProcessor] Processing asset: " + asset_name);

        if (!std::filesystem::exists(asset_source_dir) || !std::filesystem::is_directory(asset_source_dir)) {
            vibble::log::warn("[AssetProcessor] Asset source directory not found: " + asset_source_dir);
            return false;
        }

        // Find animation directories
        auto animation_dirs = findAnimationDirectories(asset_source_dir);

        if (animation_dirs.empty()) {
            vibble::log::warn("[AssetProcessor] No animation directories found for asset: " + asset_name);
            return false;
        }

        // Parse effects for this asset
        std::filesystem::path manifest_path = std::filesystem::path(asset_source_dir) / ".." / ".." / ".." / "manifest.json";
        EffectsParser effects_parser(manifest_path.string());
        auto [fg_effects, bg_effects, unchanged] = effects_parser.parse();

        // Process each animation
        for (const auto& anim_dir : animation_dirs) {
            std::filesystem::path anim_path(anim_dir);
            std::string animation_name = anim_path.filename().string();

            if (!processAnimation(asset_name, animation_name, anim_dir, fg_effects, bg_effects)) {
                vibble::log::error("[AssetProcessor] Failed to process animation: " + animation_name);
                // Continue with other animations
            }
        }

        return true;

    } catch (const std::exception& e) {
        setLastError("Failed to process asset " + asset_name + ": " + std::string(e.what()));
        return false;
    }
}

// Process single animation
bool AssetProcessor::processAnimation(const std::string& asset_name,
                                    const std::string& animation_name,
                                    const std::string& animation_dir,
                                    const EffectParams& fg_effects,
                                    const EffectParams& bg_effects) {
    try {
        vibble::log::info("[AssetProcessor] Processing animation: " + animation_name);

        // Get cache directory
        std::filesystem::path cache_root = toolkit_.getCacheRoot();
        std::filesystem::path animation_cache_dir = cache_root / asset_name / "animations" / animation_name;

        // Standard scale variants: 75%, 50%, 25%, 10%
        std::vector<int> scale_variants = {75, 50, 25, 10};

        // Collect all frames to process
        auto frames = collectAnimationFrames(animation_dir, animation_cache_dir.string(), scale_variants);

        if (frames.empty()) {
            vibble::log::warn("[AssetProcessor] No frames found for animation: " + animation_name);
            return false;
        }

        // Process each frame
        for (const auto& frame_info : frames) {
            // Load source image
            ImageData source_image;
            if (!toolkit_.loadImage(frame_info.source_path, source_image)) {
                vibble::log::error("[AssetProcessor] Failed to load source image: " + frame_info.source_path);
                total_failed_frames_++;
                continue;
            }

            // Process the frame
            if (processAnimationFrame(frame_info, source_image, fg_effects, bg_effects)) {
                total_processed_frames_++;
            } else {
                total_failed_frames_++;
            }
        }

        vibble::log::info("[AssetProcessor] Processed animation: " + animation_name +
                          " (" + std::to_string(frames.size()) + " frames)");

        return true;

    } catch (const std::exception& e) {
        setLastError("Failed to process animation " + animation_name + ": " + std::string(e.what()));
        return false;
    }
}

// Load manifest file
bool AssetProcessor::loadManifest(const std::string& manifest_path, nlohmann::json& manifest) {
    try {
        if (!std::filesystem::exists(manifest_path)) {
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

// Find animation directories in asset directory
std::vector<std::string> AssetProcessor::findAnimationDirectories(const std::string& asset_dir) {
    std::vector<std::string> directories;

    try {
        if (!std::filesystem::exists(asset_dir) || !std::filesystem::is_directory(asset_dir)) {
            return directories;
        }

        // Look for subdirectories (animations)
        for (const auto& entry : std::filesystem::directory_iterator(asset_dir)) {
            if (entry.is_directory()) {
                directories.push_back(entry.path().string());
            }
        }

        // If no subdirectories found, check if this directory contains frames
        if (directories.empty()) {
            // Check for frame files (0.png, 1.png, etc.)
            int frame_count = 0;
            while (true) {
                std::string frame_path = (std::filesystem::path(asset_dir) / (std::to_string(frame_count) + ".png")).string();
                if (!std::filesystem::exists(frame_path)) {
                    break;
                }
                frame_count++;
            }

            if (frame_count > 0) {
                directories.push_back(asset_dir);
            }
        }

        return directories;

    } catch (const std::exception& e) {
        setLastError("Failed to find animation directories: " + std::string(e.what()));
        return directories;
    }
}

// Collect animation frames for processing
std::vector<AnimationFrameInfo> AssetProcessor::collectAnimationFrames(const std::string& animation_dir,
                                                                    const std::string& cache_dir,
                                                                    const std::vector<int>& scale_variants) {
    std::vector<AnimationFrameInfo> frames;

    try {
        // Find all source frames (0.png, 1.png, etc.)
        int frame_index = 0;
        while (true) {
            std::string frame_path = (std::filesystem::path(animation_dir) / (std::to_string(frame_index) + ".png")).string();
            if (!std::filesystem::exists(frame_path)) {
                break;
            }

            // Load source image to get dimensions
            ImageData source_image;
            if (!toolkit_.loadImage(frame_path, source_image)) {
                frame_index++;
                continue;
            }

            // Create frame info for each scale variant
            for (int scale_pct : scale_variants) {
                AnimationFrameInfo frame_info;
                frame_info.source_path = frame_path;
                frame_info.frame_index = frame_index;
                frame_info.needs_rebuild = true; // For now, always rebuild
                frame_info.source_width = source_image.width;
                frame_info.source_height = source_image.height;

                // Calculate target dimensions
                int target_width = static_cast<int>(source_image.width * scale_pct / 100.0f);
                int target_height = static_cast<int>(source_image.height * scale_pct / 100.0f);

                // Create cache paths
                std::filesystem::path scale_cache_dir = std::filesystem::path(cache_dir) /
                    ("scale_" + std::to_string(scale_pct));

                frame_info.output_path_normal = (scale_cache_dir / "normal" / (std::to_string(frame_index) + ".png")).string();
                frame_info.output_path_fg = (scale_cache_dir / "foreground" / (std::to_string(frame_index) + ".png")).string();
                frame_info.output_path_bg = (scale_cache_dir / "background" / (std::to_string(frame_index) + ".png")).string();

                frames.push_back(frame_info);
            }

            frame_index++;
        }

        vibble::log::info("[AssetProcessor] Found " + std::to_string(frame_index) + " source frames");
        vibble::log::info("[AssetProcessor] Generated " + std::to_string(frames.size()) + " frame variants");

        return frames;

    } catch (const std::exception& e) {
        setLastError("Failed to collect animation frames: " + std::string(e.what()));
        return frames;
    }
}

// Process single animation frame
bool AssetProcessor::processAnimationFrame(const AnimationFrameInfo& frame_info,
                                         const ImageData& source_image,
                                         const EffectParams& fg_effects,
                                         const EffectParams& bg_effects) {
    try {
        // Ensure output directories exist
        std::vector<std::string> output_dirs = {
            std::filesystem::path(frame_info.output_path_normal).parent_path().string(),
            std::filesystem::path(frame_info.output_path_fg).parent_path().string(),
            std::filesystem::path(frame_info.output_path_bg).parent_path().string()
        };

        for (const auto& dir : output_dirs) {
            if (!std::filesystem::exists(dir)) {
                std::filesystem::create_directories(dir);
            }
        }

        // Resize image if needed
        ImageData processed_image = source_image;
        int target_width = static_cast<int>(source_image.width * 100 / 100.0f); // Keep original size for now
        int target_height = static_cast<int>(source_image.height * 100 / 100.0f);

        if (processed_image.width != target_width || processed_image.height != target_height) {
            if (!toolkit_.resizeImage(processed_image, target_width, target_height)) {
                vibble::log::error("[AssetProcessor] Failed to resize image for frame " +
                                  std::to_string(frame_info.frame_index));
                return false;
            }
        }

        // Save normal (unprocessed) version
        if (!toolkit_.saveImage(processed_image, frame_info.output_path_normal)) {
            vibble::log::error("[AssetProcessor] Failed to save normal frame " +
                              std::to_string(frame_info.frame_index));
            return false;
        }

        // Apply foreground effects
        ImageData fg_image = processed_image;
        EffectParams fg_params = fg_effects;
        fg_params.is_foreground = true;

        if (!toolkit_.applyEffects(fg_image, fg_params)) {
            vibble::log::error("[AssetProcessor] Failed to apply foreground effects to frame " +
                              std::to_string(frame_info.frame_index));
            return false;
        }

        // Save foreground version
        if (!toolkit_.saveImage(fg_image, frame_info.output_path_fg)) {
            vibble::log::error("[AssetProcessor] Failed to save foreground frame " +
                              std::to_string(frame_info.frame_index));
            return false;
        }

        // Apply background effects
        ImageData bg_image = processed_image;
        EffectParams bg_params = bg_effects;
        bg_params.is_foreground = false;

        if (!toolkit_.applyEffects(bg_image, bg_params)) {
            vibble::log::error("[AssetProcessor] Failed to apply background effects to frame " +
                              std::to_string(frame_info.frame_index));
            return false;
        }

        // Save background version
        if (!toolkit_.saveImage(bg_image, frame_info.output_path_bg)) {
            vibble::log::error("[AssetProcessor] Failed to save background frame " +
                              std::to_string(frame_info.frame_index));
            return false;
        }

        vibble::log::info("[AssetProcessor] Processed frame " + std::to_string(frame_info.frame_index) +
                          " -> " + std::to_string(frame_info.source_width) + "x" +
                          std::to_string(frame_info.source_height));

        return true;

    } catch (const std::exception& e) {
        setLastError("Failed to process frame " + std::to_string(frame_info.frame_index) + ": " +
                    std::string(e.what()));
        return false;
    }
}

// Set last error
void AssetProcessor::setLastError(const std::string& error) {
    last_error_ = error;
    vibble::log::error("[AssetProcessor] " + error);
}

// Get last error
std::string AssetProcessor::getLastError() const {
    return last_error_;
}

// Clear last error
void AssetProcessor::clearLastError() {
    last_error_.clear();
}

} // namespace vibble
