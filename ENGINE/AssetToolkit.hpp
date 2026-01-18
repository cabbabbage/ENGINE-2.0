#pragma once

#include <vector>
#include <string>
#include <memory>
#include <optional>
#include <filesystem>
#include <nlohmann/json.hpp>

namespace fs = std::filesystem;

namespace vibble {

// Forward declarations
class GPUAccelerator;
class AnimationProcessor;
class CacheManager;

// Image data structure
struct ImageData {
    std::vector<uint8_t> pixels;  // RGBA format
    int width = 0;
    int height = 0;
    bool has_alpha = false;

    // Clear image data
    void clear() {
        pixels.clear();
        width = 0;
        height = 0;
        has_alpha = false;
    }

    // Check if image is valid
    bool isValid() const {
        return width > 0 && height > 0 && !pixels.empty();
    }

    // Get pixel count
    size_t pixelCount() const {
        return static_cast<size_t>(width) * static_cast<size_t>(height);
    }

    // Get pixel at position (x, y)
    std::optional<std::vector<uint8_t>> getPixel(int x, int y) const {
        if (x < 0 || x >= width || y < 0 || y >= height) {
            return std::nullopt;
        }
        size_t index = (static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x)) * 4;
        if (index + 3 >= pixels.size()) {
            return std::nullopt;
        }
        return std::vector<uint8_t>{pixels[index], pixels[index + 1], pixels[index + 2], pixels[index + 3]};
    }

    // Set pixel at position (x, y)
    bool setPixel(int x, int y, const std::vector<uint8_t>& color) {
        if (x < 0 || x >= width || y < 0 || y >= height || color.size() < 4) {
            return false;
        }
        size_t index = (static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x)) * 4;
        if (index + 3 >= pixels.size()) {
            return false;
        }
        pixels[index] = color[0];
        pixels[index + 1] = color[1];
        pixels[index + 2] = color[2];
        pixels[index + 3] = color[3];
        return true;
    }
};

// Effect parameters
struct EffectParams {
    float brightness = 0.0f;      // [-1.0, 1.0]
    float contrast = 0.0f;        // [-1.0, 1.0]
    float blur = 0.0f;            // [-1.0, 1.0]
    float saturation_red = 0.0f;  // [-1.0, 1.0]
    float saturation_green = 0.0f;// [-1.0, 1.0]
    float saturation_blue = 0.0f; // [-1.0, 1.0]
    float hue = 0.0f;             // [-180.0, 180.0] degrees
    bool is_foreground = false;

    // Validate parameters
    bool validate() const {
        return brightness >= -1.0f && brightness <= 1.0f &&
               contrast >= -1.0f && contrast <= 1.0f &&
               blur >= -1.0f && blur <= 1.0f &&
               saturation_red >= -1.0f && saturation_red <= 1.0f &&
               saturation_green >= -1.0f && saturation_green <= 1.0f &&
               saturation_blue >= -1.0f && saturation_blue <= 1.0f &&
               hue >= -180.0f && hue <= 180.0f;
    }

    // Convert to JSON
    nlohmann::json toJson() const {
        return {
            {"brightness", brightness},
            {"contrast", contrast},
            {"blur", blur},
            {"saturation_red", saturation_red},
            {"saturation_green", saturation_green},
            {"saturation_blue", saturation_blue},
            {"hue", hue},
            {"foreground", is_foreground}
        };
    }

    // Create from JSON
    static EffectParams fromJson(const nlohmann::json& json) {
        EffectParams params;
        if (json.contains("brightness")) params.brightness = json["brightness"].get<float>();
        if (json.contains("contrast")) params.contrast = json["contrast"].get<float>();
        if (json.contains("blur")) params.blur = json["blur"].get<float>();
        if (json.contains("saturation_red")) params.saturation_red = json["saturation_red"].get<float>();
        if (json.contains("saturation_green")) params.saturation_green = json["saturation_green"].get<float>();
        if (json.contains("saturation_blue")) params.saturation_blue = json["saturation_blue"].get<float>();
        if (json.contains("hue")) params.hue = json["hue"].get<float>();
        if (json.contains("foreground")) params.is_foreground = json["foreground"].get<bool>();
        return params;
    }
};

// Animation frame data
struct AnimationFrame {
    ImageData image;
    std::string output_path;
    int frame_index = -1;
    bool needs_rebuild = false;
};

// Asset information
struct AssetInfo {
    std::string name;
    std::string source_directory;
    std::vector<std::string> animations;
    std::vector<int> size_variants;
    std::string type;

    // Check if asset info is valid
    bool isValid() const {
        return !name.empty() && !source_directory.empty();
    }
};

// Cache validation result
enum class CacheValidationResult {
    VALID,
    MISSING_FILES,
    INVALID_STRUCTURE,
    UPDATED,
    ERROR
};

// Main AssetToolkit class
class AssetToolkit {
public:
    // Constructor and destructor
    AssetToolkit();
    ~AssetToolkit();

    // Initialize the toolkit
    bool initialize(const std::string& repo_root, bool use_gpu = true);

    // Shutdown the toolkit
    void shutdown();

    // Image I/O operations
    bool loadImage(const std::string& path, ImageData& output);
    bool saveImage(const ImageData& image, const std::string& path);

    // Basic image manipulation
    bool resizeImage(ImageData& image, int new_width, int new_height);
    bool cropImage(ImageData& image, int left, int top, int right, int bottom);

    // File system utilities
    bool ensureDirectoryExists(const std::string& path);
    std::vector<std::string> findFilesInDirectory(const std::string& directory,
                                                 const std::string& extension = ".png");
    std::vector<std::string> findAnimationFrames(const std::string& directory);

    // Path utilities
    std::string getCachePath(const std::string& asset_name, const std::string& animation_name,
                            int scale_pct, const std::string& variant) const;
    std::string getAbsolutePath(const std::string& relative_path) const;

    // JSON utilities
    bool loadJson(const std::string& path, nlohmann::json& output);
    bool saveJson(const std::string& path, const nlohmann::json& data);
    std::string computeStableHash(const nlohmann::json& data);

    // Error handling
    std::string getLastError() const;
    void clearLastError();

    // Getters
    const std::string& getRepoRoot() const { return repo_root_; }
    const std::string& getCacheRoot() const { return cache_root_; }
    bool isInitialized() const { return initialized_; }

private:
    // Member variables
    std::string repo_root_;
    std::string cache_root_;
    bool initialized_ = false;
    bool use_gpu_ = false;
    std::string last_error_;

    // GPU resources
    std::unique_ptr<GPUAccelerator> gpu_accelerator_;

    // Helper methods
    void setLastError(const std::string& error);
    bool validateImageData(const ImageData& image) const;
    bool validateFilePath(const std::string& path) const;

    // Logging
    void logInfo(const std::string& message) const;
    void logWarning(const std::string& message) const;
    void logError(const std::string& message) const;
};

} // namespace vibble
