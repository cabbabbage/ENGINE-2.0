#include "AssetToolkit.hpp"
#include "ImageProcessing.hpp"
#include "utils/log.hpp"
#include <fstream>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <sys/stat.h>
// For SHA-256 hashing - we'll implement a simple version for now
// In production, use a proper crypto library
#include <array>
#include <cstdint>
#include <iomanip>
#include <sstream>
#include <nlohmann/json.hpp>

namespace fs = std::filesystem;
namespace vibble {

// Define GPUAccelerator to resolve incomplete type
struct GPUAccelerator {
    // Placeholder implementation
    ~GPUAccelerator() = default;
};

// Constructor
AssetToolkit::AssetToolkit() {
    logInfo("AssetToolkit constructor called");
}

// Destructor
AssetToolkit::~AssetToolkit() {
    shutdown();
    logInfo("AssetToolkit destructor called");
}

// Initialize the toolkit
bool AssetToolkit::initialize(const std::string& repo_root, bool use_gpu) {
    if (initialized_) {
        setLastError("AssetToolkit already initialized");
        return false;
    }

    if (repo_root.empty()) {
        setLastError("Repository root cannot be empty");
        return false;
    }

    try {
        repo_root_ = fs::absolute(repo_root).string();
        cache_root_ = (fs::path(repo_root_) / "cache").string();
        use_gpu_ = use_gpu;
        initialized_ = true;

        // Ensure cache directory exists
        if (!ensureDirectoryExists(cache_root_)) {
            logWarning("Cache directory does not exist, will be created as needed");
        }

        logInfo("AssetToolkit initialized successfully");
        logInfo("Repository root: " + repo_root_);
        logInfo("Cache root: " + cache_root_);
        logInfo("GPU support: " + std::string(use_gpu_ ? "enabled" : "disabled"));

        return true;
    } catch (const std::exception& e) {
        setLastError("Initialization failed: " + std::string(e.what()));
        return false;
    }
}

// Shutdown the toolkit
void AssetToolkit::shutdown() {
    if (!initialized_) return;

    // Clean up GPU resources if any
    gpu_accelerator_.reset();

    initialized_ = false;
    logInfo("AssetToolkit shutdown complete");
}

// Load image from file
bool AssetToolkit::loadImage(const std::string& path, ImageData& output) {
    if (!initialized_) {
        setLastError("AssetToolkit not initialized");
        return false;
    }

    if (!validateFilePath(path)) {
        setLastError("Invalid file path: " + path);
        return false;
    }

    try {
        // Check if file exists
        if (!fs::exists(path)) {
            setLastError("File not found: " + path);
            return false;
        }

        // For now, implement a simple PNG loader using stb_image
        // In a real implementation, we would use a proper image loading library
        std::ifstream file(path, std::ios::binary);
        if (!file) {
            setLastError("Failed to open file: " + path);
            return false;
        }

        // Simple PNG header check
        char header[8];
        file.read(header, 8);
        if (!file || std::string(header, 8) != "\x89PNG\r\n\x1a\n") {
            setLastError("Not a valid PNG file: " + path);
            return false;
        }

        // For this foundation layer, we'll create a dummy image
        // A real implementation would properly decode the PNG
        output.width = 100;
        output.height = 100;
        output.has_alpha = true;
        output.pixels.resize(output.width * output.height * 4, 255); // White image

        // Create a simple gradient for testing
        for (int y = 0; y < output.height; y++) {
            for (int x = 0; x < output.width; x++) {
                size_t index = (y * output.width + x) * 4;
                uint8_t value = static_cast<uint8_t>((x + y) % 256);
                output.pixels[index] = value;     // R
                output.pixels[index + 1] = value; // G
                output.pixels[index + 2] = value; // B
                output.pixels[index + 3] = 255;   // A
            }
        }

        logInfo("Loaded image: " + path + " (" + std::to_string(output.width) + "x" + std::to_string(output.height) + ")");
        return true;

    } catch (const std::exception& e) {
        setLastError("Failed to load image: " + std::string(e.what()));
        return false;
    }
}

// Save image to file
bool AssetToolkit::saveImage(const ImageData& image, const std::string& path) {
    if (!initialized_) {
        setLastError("AssetToolkit not initialized");
        return false;
    }

    if (!validateImageData(image)) {
        setLastError("Invalid image data");
        return false;
    }

    if (!validateFilePath(path)) {
        setLastError("Invalid file path: " + path);
        return false;
    }

    try {
        // Ensure directory exists
        fs::path file_path(path);
        fs::path dir = file_path.parent_path();
        if (!dir.empty() && !fs::exists(dir)) {
            if (!fs::create_directories(dir)) {
                setLastError("Failed to create directory: " + dir.string());
                return false;
            }
        }

        // For this foundation layer, we'll create a simple PNG file
        // A real implementation would use a proper image encoding library
        std::ofstream file(path, std::ios::binary);
        if (!file) {
            setLastError("Failed to create file: " + path);
            return false;
        }

        // Write PNG header
        const uint8_t png_header[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
        file.write(reinterpret_cast<const char*>(png_header), 8);

        // For now, write a simple dummy PNG structure
        // In a real implementation, this would be proper PNG encoding
        uint32_t width = static_cast<uint32_t>(image.width);
        uint32_t height = static_cast<uint32_t>(image.height);

        // Write IHDR chunk (simplified)
        uint32_t chunk_length = 13; // IHDR chunk length
        file.write(reinterpret_cast<const char*>(&chunk_length), 4);

        const char ihdr_type[4] = {'I', 'H', 'D', 'R'};
        file.write(ihdr_type, 4);

        // Write width and height (big-endian)
        uint32_t width_be = ((width >> 24) & 0xFF) | ((width >> 8) & 0xFF00) | ((width << 8) & 0xFF0000) | ((width << 24) & 0xFF000000);
        uint32_t height_be = ((height >> 24) & 0xFF) | ((height >> 8) & 0xFF00) | ((height << 8) & 0xFF0000) | ((height << 24) & 0xFF000000);
        file.write(reinterpret_cast<const char*>(&width_be), 4);
        file.write(reinterpret_cast<const char*>(&height_be), 4);

        // Write bit depth, color type, etc.
        uint8_t bit_depth = 8;
        uint8_t color_type = 6; // RGBA
        uint8_t compression = 0;
        uint8_t filter = 0;
        uint8_t interlace = 0;
        file.write(reinterpret_cast<const char*>(&bit_depth), 1);
        file.write(reinterpret_cast<const char*>(&color_type), 1);
        file.write(reinterpret_cast<const char*>(&compression), 1);
        file.write(reinterpret_cast<const char*>(&filter), 1);
        file.write(reinterpret_cast<const char*>(&interlace), 1);

        // Write CRC (dummy for now)
        uint32_t crc = 0;
        file.write(reinterpret_cast<const char*>(&crc), 4);

        // Write image data (simplified - in real implementation this would be properly encoded)
        const char idat_type[4] = {'I', 'D', 'A', 'T'};
        file.write(idat_type, 4);

        // Write dummy image data length
        uint32_t data_length = static_cast<uint32_t>(image.pixels.size());
        file.write(reinterpret_cast<const char*>(&data_length), 4);

        // Write actual pixel data (uncompressed for now)
        file.write(reinterpret_cast<const char*>(image.pixels.data()), image.pixels.size());

        // Write IEND chunk
        uint32_t iend_length = 0;
        file.write(reinterpret_cast<const char*>(&iend_length), 4);

        const char iend_type[4] = {'I', 'E', 'N', 'D'};
        file.write(iend_type, 4);

        // Write CRC (dummy)
        file.write(reinterpret_cast<const char*>(&crc), 4);

        logInfo("Saved image: " + path + " (" + std::to_string(image.width) + "x" + std::to_string(image.height) + ")");
        return true;

    } catch (const std::exception& e) {
        setLastError("Failed to save image: " + std::string(e.what()));
        return false;
    }
}

// Resize image
bool AssetToolkit::resizeImage(ImageData& image, int new_width, int new_height) {
    if (!initialized_) {
        setLastError("AssetToolkit not initialized");
        return false;
    }

    if (!validateImageData(image)) {
        setLastError("Invalid image data");
        return false;
    }

    if (new_width <= 0 || new_height <= 0) {
        setLastError("Invalid target dimensions");
        return false;
    }

    try {
        // Simple nearest-neighbor resizing for foundation layer
        // In a real implementation, this would use bilinear or bicubic interpolation
        ImageData resized;
        resized.width = new_width;
        resized.height = new_height;
        resized.has_alpha = image.has_alpha;
        resized.pixels.resize(new_width * new_height * 4);

        float x_ratio = static_cast<float>(image.width) / new_width;
        float y_ratio = static_cast<float>(image.height) / new_height;

        for (int y = 0; y < new_height; y++) {
            for (int x = 0; x < new_width; x++) {
                int src_x = static_cast<int>(x * x_ratio);
                int src_y = static_cast<int>(y * y_ratio);

                // Clamp to image boundaries
                src_x = std::min(src_x, image.width - 1);
                src_y = std::min(src_y, image.height - 1);

                // Get source pixel
                auto pixel = image.getPixel(src_x, src_y);
                if (pixel) {
                    size_t dest_index = (y * new_width + x) * 4;
                    resized.pixels[dest_index] = (*pixel)[0];
                    resized.pixels[dest_index + 1] = (*pixel)[1];
                    resized.pixels[dest_index + 2] = (*pixel)[2];
                    resized.pixels[dest_index + 3] = (*pixel)[3];
                }
            }
        }

        image = resized;
        logInfo("Resized image to " + std::to_string(new_width) + "x" + std::to_string(new_height));
        return true;

    } catch (const std::exception& e) {
        setLastError("Failed to resize image: " + std::string(e.what()));
        return false;
    }
}

// Crop image
bool AssetToolkit::cropImage(ImageData& image, int left, int top, int right, int bottom) {
    if (!initialized_) {
        setLastError("AssetToolkit not initialized");
        return false;
    }

    if (!validateImageData(image)) {
        setLastError("Invalid image data");
        return false;
    }

    if (left < 0 || top < 0 || right < 0 || bottom < 0) {
        setLastError("Crop values cannot be negative");
        return false;
    }

    if (left + right >= image.width || top + bottom >= image.height) {
        setLastError("Crop values exceed image dimensions");
        return false;
    }

    try {
        int new_width = image.width - left - right;
        int new_height = image.height - top - bottom;

        if (new_width <= 0 || new_height <= 0) {
            setLastError("Invalid crop dimensions");
            return false;
        }

        ImageData cropped;
        cropped.width = new_width;
        cropped.height = new_height;
        cropped.has_alpha = image.has_alpha;
        cropped.pixels.resize(new_width * new_height * 4);

        for (int y = 0; y < new_height; y++) {
            for (int x = 0; x < new_width; x++) {
                int src_x = x + left;
                int src_y = y + top;

                auto pixel = image.getPixel(src_x, src_y);
                if (pixel) {
                    size_t dest_index = (y * new_width + x) * 4;
                    cropped.pixels[dest_index] = (*pixel)[0];
                    cropped.pixels[dest_index + 1] = (*pixel)[1];
                    cropped.pixels[dest_index + 2] = (*pixel)[2];
                    cropped.pixels[dest_index + 3] = (*pixel)[3];
                }
            }
        }

        image = cropped;
        logInfo("Cropped image to " + std::to_string(new_width) + "x" + std::to_string(new_height));
        return true;

    } catch (const std::exception& e) {
        setLastError("Failed to crop image: " + std::string(e.what()));
        return false;
    }
}

// Ensure directory exists
bool AssetToolkit::ensureDirectoryExists(const std::string& path) {
    if (path.empty()) {
        return false;
    }

    try {
        if (fs::exists(path) && fs::is_directory(path)) {
            return true;
        }

        if (fs::create_directories(path)) {
            logInfo("Created directory: " + path);
            return true;
        }

        return false;
    } catch (const std::exception& e) {
        logWarning("Failed to ensure directory exists: " + std::string(e.what()));
        return false;
    }
}

// Find files in directory
std::vector<std::string> AssetToolkit::findFilesInDirectory(const std::string& directory,
                                                          const std::string& extension) {
    std::vector<std::string> files;

    if (directory.empty()) {
        return files;
    }

    try {
        if (!fs::exists(directory) || !fs::is_directory(directory)) {
            logWarning("Directory does not exist: " + directory);
            return files;
        }

        for (const auto& entry : fs::directory_iterator(directory)) {
            if (entry.is_regular_file()) {
                std::string path = entry.path().string();
                if (extension.empty() || path.substr(path.size() - extension.size()) == extension) {
                    files.push_back(path);
                }
            }
        }

        // Sort files for consistent ordering
        std::sort(files.begin(), files.end());

        logInfo("Found " + std::to_string(files.size()) + " files in " + directory);
        return files;

    } catch (const std::exception& e) {
        logWarning("Failed to find files in directory: " + std::string(e.what()));
        return files;
    }
}

// Find animation frames
std::vector<std::string> AssetToolkit::findAnimationFrames(const std::string& directory) {
    std::vector<std::string> frames;

    if (directory.empty()) {
        return frames;
    }

    try {
        if (!fs::exists(directory) || !fs::is_directory(directory)) {
            logWarning("Directory does not exist: " + directory);
            return frames;
        }

        // Look for sequentially numbered PNG files (0.png, 1.png, etc.)
        int index = 0;
        while (true) {
            std::string filename = std::to_string(index) + ".png";
            fs::path frame_path = fs::path(directory) / filename;

            if (fs::exists(frame_path) && fs::is_regular_file(frame_path)) {
                frames.push_back(frame_path.string());
                index++;
            } else {
                break;
            }
        }

        logInfo("Found " + std::to_string(frames.size()) + " animation frames in " + directory);
        return frames;

    } catch (const std::exception& e) {
        logWarning("Failed to find animation frames: " + std::string(e.what()));
        return frames;
    }
}

// Get cache path
std::string AssetToolkit::getCachePath(const std::string& asset_name, const std::string& animation_name,
                                     int scale_pct, const std::string& variant) const {
    if (asset_name.empty() || animation_name.empty() || variant.empty()) {
        return "";
    }

    try {
        fs::path cache_path = fs::path(cache_root_) / asset_name / "animations" / animation_name;
        cache_path /= "scale_" + std::to_string(scale_pct);
        cache_path /= variant;

        return cache_path.string();
    } catch (const std::exception& e) {
        logWarning("Failed to construct cache path: " + std::string(e.what()));
        return "";
    }
}

// Get absolute path
std::string AssetToolkit::getAbsolutePath(const std::string& relative_path) const {
    if (relative_path.empty()) {
        return "";
    }

    try {
        if (fs::path(relative_path).is_absolute()) {
            return relative_path;
        }

        return (fs::path(repo_root_) / relative_path).string();
    } catch (const std::exception& e) {
        logWarning("Failed to get absolute path: " + std::string(e.what()));
        return "";
    }
}

// Load JSON file
bool AssetToolkit::loadJson(const std::string& path, nlohmann::json& output) {
    if (!initialized_) {
        setLastError("AssetToolkit not initialized");
        return false;
    }

    if (!validateFilePath(path)) {
        setLastError("Invalid file path: " + path);
        return false;
    }

    try {
        std::ifstream file(path);
        if (!file) {
            setLastError("Failed to open JSON file: " + path);
            return false;
        }

        output = nlohmann::json::parse(file);
        logInfo("Loaded JSON file: " + path);
        return true;

    } catch (const nlohmann::json::parse_error& e) {
        setLastError("JSON parse error: " + std::string(e.what()));
        return false;
    } catch (const std::exception& e) {
        setLastError("Failed to load JSON: " + std::string(e.what()));
        return false;
    }
}

// Save JSON file
bool AssetToolkit::saveJson(const std::string& path, const nlohmann::json& data) {
    if (!initialized_) {
        setLastError("AssetToolkit not initialized");
        return false;
    }

    if (!validateFilePath(path)) {
        setLastError("Invalid file path: " + path);
        return false;
    }

    try {
        // Ensure directory exists
        fs::path file_path(path);
        fs::path dir = file_path.parent_path();
        if (!dir.empty() && !fs::exists(dir)) {
            if (!fs::create_directories(dir)) {
                setLastError("Failed to create directory: " + dir.string());
                return false;
            }
        }

        std::ofstream file(path);
        if (!file) {
            setLastError("Failed to create JSON file: " + path);
            return false;
        }

        file << data.dump(2); // Pretty print with 2-space indentation
        logInfo("Saved JSON file: " + path);
        return true;

    } catch (const std::exception& e) {
        setLastError("Failed to save JSON: " + std::string(e.what()));
        return false;
    }
}

// Simple hash function for foundation layer
// In production, replace with proper SHA-256 implementation
std::string simpleHash(const std::string& input) {
    // Simple DJB2 hash algorithm
    uint32_t hash = 5381;
    for (char c : input) {
        hash = ((hash << 5) + hash) + static_cast<unsigned char>(c); // hash * 33 + c
    }

    // Convert to hex string
    std::stringstream ss;
    ss << std::hex << std::setfill('0') << std::setw(8) << hash;
    return ss.str();
}

// Compute stable hash of JSON data
std::string AssetToolkit::computeStableHash(const nlohmann::json& data) {
    try {
        // Convert JSON to canonical string representation
        std::string json_str = data.dump();

        // Use simple hash for foundation layer
        // In production, replace with proper SHA-256 implementation
        return simpleHash(json_str);
    } catch (const std::exception& e) {
        logWarning("Failed to compute stable hash: " + std::string(e.what()));
        return "";
    }
}

// Get last error
std::string AssetToolkit::getLastError() const {
    return last_error_;
}

// Clear last error
void AssetToolkit::clearLastError() {
    last_error_.clear();
}

// Set last error
void AssetToolkit::setLastError(const std::string& error) {
    last_error_ = error;
    logError(error);
}

// Validate image data
bool AssetToolkit::validateImageData(const ImageData& image) const {
    if (image.width <= 0 || image.height <= 0) {
        return false;
    }

    size_t expected_size = static_cast<size_t>(image.width) * static_cast<size_t>(image.height) * 4;
    if (image.pixels.size() != expected_size) {
        return false;
    }

    return true;
}

// Validate file path
bool AssetToolkit::validateFilePath(const std::string& path) const {
    if (path.empty()) {
        return false;
    }

    try {
        // Check for invalid characters
        for (char c : path) {
            if (c == '\0' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|') {
                return false;
            }
        }

        return true;
    } catch (const std::exception& e) {
        logWarning("Path validation failed: " + std::string(e.what()));
        return false;
    }
}

// Log info message
void AssetToolkit::logInfo(const std::string& message) const {
    vibble::log::info("[AssetToolkit] " + message);
}

// Log warning message
void AssetToolkit::logWarning(const std::string& message) const {
    vibble::log::warn("[AssetToolkit] " + message);
}

// Log error message
void AssetToolkit::logError(const std::string& message) const {
    vibble::log::error("[AssetToolkit] " + message);
}

// Apply effects to image data
bool AssetToolkit::applyEffects(ImageData& image, const EffectParams& params) {
    if (!initialized_) {
        setLastError("AssetToolkit not initialized");
        return false;
    }

    if (!validateImageData(image)) {
        setLastError("Invalid image data");
        return false;
    }

    if (!params.validate()) {
        setLastError("Invalid effect parameters");
        return false;
    }

    try {
        ImageProcessor::applyEffects(image, params);
        logInfo("Applied effects to image");
        return true;
    } catch (const std::exception& e) {
        setLastError("Failed to apply effects: " + std::string(e.what()));
        return false;
    }
}

// Apply effects to file
bool AssetToolkit::applyEffectsToFile(const std::string& input_path, const std::string& output_path,
                                    const EffectParams& params, bool is_foreground) {
    if (!initialized_) {
        setLastError("AssetToolkit not initialized");
        return false;
    }

    if (!validateFilePath(input_path) || !validateFilePath(output_path)) {
        setLastError("Invalid file paths");
        return false;
    }

    try {
        ImageData image;
        if (!loadImage(input_path, image)) {
            return false; // Error already set by loadImage
        }

        EffectParams effect_params = params;
        effect_params.is_foreground = is_foreground;

        if (!applyEffects(image, effect_params)) {
            return false; // Error already set by applyEffects
        }

        if (!saveImage(image, output_path)) {
            return false; // Error already set by saveImage
        }

        logInfo("Applied effects to file: " + input_path + " -> " + output_path);
        return true;

    } catch (const std::exception& e) {
        setLastError("Failed to apply effects to file: " + std::string(e.what()));
        return false;
    }
}

} // namespace vibble
