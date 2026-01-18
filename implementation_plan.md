# Implementation Plan: C++ AssetToolkit Class

## Overview

Replace all Python tools in ENGINE\tools with a single comprehensive C++ class called `AssetToolkit` that provides equivalent functionality with better performance and seamless C++ integration.

## Scope and Context

The current system uses multiple Python scripts for asset processing, image effects, cache management, and utility functions. These are called from C++ using `std::system()` calls, which is inefficient and creates maintenance challenges. The new `AssetToolkit` class will consolidate all this functionality into native C++ code.

## Types

### Core Data Structures

```cpp
// Image data structure
struct ImageData {
    std::vector<uint8_t> pixels;  // RGBA format
    int width;
    int height;
    bool has_alpha;
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
};

// Animation frame data
struct AnimationFrame {
    ImageData image;
    std::string output_path;
    int frame_index;
    bool needs_rebuild;
};

// Asset information
struct AssetInfo {
    std::string name;
    std::string source_directory;
    std::vector<std::string> animations;
    std::vector<int> size_variants;
    std::string type;
};

// Cache validation result
enum class CacheValidationResult {
    VALID,
    MISSING_FILES,
    INVALID_STRUCTURE,
    UPDATED
};
```

## Files

### New Files to Create

1. **ENGINE/AssetToolkit.hpp** - Main header file with class declaration
2. **ENGINE/AssetToolkit.cpp** - Main implementation file
3. **ENGINE/AssetToolkitGPU.cu** - CUDA GPU acceleration implementation
4. **ENGINE/AssetToolkitOpenCL.cpp** - OpenCL GPU acceleration implementation
5. **ENGINE/ImageProcessing.hpp/cpp** - Core image processing algorithms
6. **ENGINE/EffectSystem.hpp/cpp** - Effect parsing and application
7. **ENGINE/CacheManager.hpp/cpp** - Cache management and validation
8. **ENGINE/AnimationProcessor.hpp/cpp** - Animation processing utilities

### Files to Modify

1. **ENGINE/utils/rebuild_queue.cpp/hpp** - Replace Python calls with native C++ calls
2. **ENGINE/main.cpp** - Update initialization and usage
3. **CMakeLists.txt** - Add new source files and GPU dependencies

### Files to Remove

1. **ENGINE/tools/*.py** - All Python tool files (after successful migration)

## Functions

### New Functions to Implement

#### Core AssetToolkit Class Methods

```cpp
class AssetToolkit {
public:
    // Constructor/Destructor
    AssetToolkit();
    ~AssetToolkit();

    // Initialization
    bool initialize(const std::string& repo_root, bool use_gpu = true);
    void shutdown();

    // Image Processing
    bool loadImage(const std::string& path, ImageData& output);
    bool saveImage(const ImageData& image, const std::string& path);
    bool applyEffects(ImageData& image, const EffectParams& params);
    bool applyEffectsToFile(const std::string& input_path, const std::string& output_path,
                          const EffectParams& params, bool is_foreground);

    // Asset Processing
    bool processAssetAnimations(const std::string& asset_name, const std::string& cache_root);
    bool processAllAssets(const std::string& cache_root);
    bool generateAnimationCache(const std::string& asset_name, const std::string& src_dir,
                              const std::string& cache_root);

    // Effect Management
    EffectParams parseEffectParams(const nlohmann::json& effect_json, bool is_foreground);
    std::pair<EffectParams, EffectParams> getForegroundBackgroundEffects(const std::string& manifest_path);

    // Cache Management
    CacheValidationResult validateCache(const std::string& manifest_path, const std::string& cache_root);
    bool updateCacheManifest(const std::string& manifest_path);
    std::string computeStableHash(const nlohmann::json& data);

    // Utility Functions
    bool convertImageToPNG(const std::string& input_path, const std::string& output_path);
    bool retimeAnimation(const std::string& asset_name, const std::string& animation_name,
                        const std::string& mode);
    bool setRebuildFlags(const std::string& manifest_path, const std::string& mode,
                        const std::string& asset_name = "", const std::string& animation_name = "",
                        int frame_index = -1);

    // GPU Management
    bool hasGPUSupport() const;
    std::string getGPUInfo() const;
    void setUseGPU(bool use_gpu);

private:
    // Internal processing methods
    bool applyEffectsCPU(ImageData& image, const EffectParams& params);
    bool applyEffectsGPU(ImageData& image, const EffectParams& params);
    bool applyBlur(ImageData& image, float blur_value, bool is_foreground);
    bool applyColorAdjustments(ImageData& image, const EffectParams& params);

    // Helper methods
    bool rgbToHsv(const std::vector<float>& rgb, std::vector<float>& hsv);
    bool hsvToRgb(const std::vector<float>& hsv, std::vector<float>& rgb);
    bool resizeImage(ImageData& image, int new_width, int new_height);
    bool cropImage(ImageData& image, int left, int top, int right, int bottom);

    // File system utilities
    std::vector<std::string> findAnimationFrames(const std::string& directory);
    bool ensureDirectoryExists(const std::string& path);
    std::string getCachePath(const std::string& asset_name, const std::string& animation_name,
                            int scale_pct, const std::string& variant);

    // Member variables
    std::string repo_root_;
    std::string cache_root_;
    bool use_gpu_;
    bool gpu_available_;

    // GPU resources
    void* gpu_context_;  // Platform-specific GPU context
    void* gpu_resources_; // GPU memory and kernels
};
```

#### Image Processing Functions

```cpp
// Color space conversions
bool rgbToHsvCPU(const uint8_t* rgb_data, float* hsv_data, size_t pixel_count);
bool hsvToRgbCPU(const float* hsv_data, uint8_t* rgb_data, size_t pixel_count);

// Effect applications
void applyBrightness(uint8_t* pixels, size_t count, float brightness);
void applyContrast(uint8_t* pixels, size_t count, float contrast);
void applySaturation(uint8_t* pixels, size_t count,
                    float sat_r, float sat_g, float sat_b);
void applyHue(uint8_t* pixels, size_t count, float hue_degrees);

// Blur algorithms
void applyGaussianBlur(uint8_t* pixels, int width, int height, float radius);
void applySharpen(uint8_t* pixels, int width, int height, float strength);
```

#### Cache Management Functions

```cpp
// JSON utilities
bool compareJson(const nlohmann::json& a, const nlohmann::json& b);
nlohmann::json normalizeJson(const nlohmann::json& data);
std::string computeJsonHash(const nlohmann::json& data);

// Manifest operations
bool loadManifest(const std::string& path, nlohmann::json& output);
bool saveManifest(const std::string& path, const nlohmann::json& data);
bool updateManifestEffects(const std::string& manifest_path,
                          const EffectParams& fg_effects,
                          const EffectParams& bg_effects);
```

### Modified Functions

#### RebuildQueueCoordinator Updates

```cpp
// Replace Python calls with native C++ calls
class RebuildQueueCoordinator {
    // ... existing members ...

    // Replace these methods:
    bool run_asset_tool(const std::string& command_prefix) const;
    void mark_all_frames_for_rebuild() const;
    void mark_asset_for_rebuild(const std::string& asset_name) const;
    void mark_animation_for_rebuild(const std::string& asset_name, const std::string& animation) const;
    void mark_frame_for_rebuild(const std::string& asset_name, const std::string& animation, int frame_index) const;
    bool validate_manifest_cache(const std::string& command_prefix) const;

    // Add AssetToolkit member
    std::unique_ptr<AssetToolkit> asset_toolkit_;
};
```

## Classes

### New Classes to Implement

```cpp
// GPU Acceleration Interface
class GPUAccelerator {
public:
    virtual ~GPUAccelerator() = default;
    virtual bool initialize() = 0;
    virtual void shutdown() = 0;
    virtual bool isAvailable() const = 0;
    virtual std::string getDeviceInfo() const = 0;
    virtual bool applyEffects(const uint8_t* input, uint8_t* output,
                            int width, int height, const EffectParams& params) = 0;
};

// CUDA Implementation
class CUDAGPUAccelerator : public GPUAccelerator {
    // CUDA-specific implementation
};

// OpenCL Implementation
class OpenCLGPUAccelerator : public GPUAccelerator {
    // OpenCL-specific implementation
};

// Animation Processor
class AnimationProcessor {
public:
    bool processAnimation(const std::string& asset_name, const std::string& animation_name,
                         const std::string& src_dir, const std::string& cache_root,
                         const EffectParams& fg_effects, const EffectParams& bg_effects,
                         AssetToolkit& toolkit);

    bool retimeAnimation(const std::string& asset_name, const std::string& animation_name,
                        const std::string& mode, const std::string& manifest_path);

private:
    std::vector<std::string> buildFrameSequence(int frame_count, float speed_multiplier);
    bool computeCropBounds(const std::vector<std::string>& frame_paths,
                          std::vector<int>& bounds);
};

// Cache Manager
class CacheManager {
public:
    CacheValidationResult validateAnimationCache(const std::string& asset_name,
                                               const std::string& animation_name,
                                               const std::string& manifest_path,
                                               const std::string& cache_root);

    bool updateCacheFiles(const std::string& manifest_path);

private:
    bool checkCacheFileExists(const std::string& path);
    bool markMissingFrames(nlohmann::json& manifest, const std::string& asset_name,
                          const std::string& animation_name);
};
```

### Modified Classes

```cpp
// Updated RebuildQueueCoordinator
class RebuildQueueCoordinator {
    // ... existing interface ...

private:
    std::unique_ptr<AssetToolkit> asset_toolkit_;

    // New implementation methods
    bool run_asset_tool_native(const std::string& command_prefix) const;
    void mark_rebuild_flags_native(const std::string& mode, const std::string& asset_name = "",
                                  const std::string& animation_name = "", int frame_index = -1) const;
};
```

## Dependencies

### New Dependencies

1. **GPU Libraries**:
   - CUDA Toolkit (for NVIDIA GPU support)
   - OpenCL (for cross-platform GPU support)

2. **Image Processing**:
   - stb_image.h (header-only image loading)
   - stb_image_write.h (header-only image writing)

3. **JSON Processing**:
   - nlohmann/json (already used in project)

4. **Threading**:
   - C++17 parallel algorithms
   - Thread pool implementation

### Modified Dependencies

1. **CMakeLists.txt**:
   - Add CUDA/OpenCL find packages
   - Add new source files
   - Update include directories

2. **Project Structure**:
   - Remove Python tool dependencies
   - Update build configuration

## Testing

### Test Strategy

1. **Unit Tests**:
   - Image processing algorithms
   - Effect application
   - Color space conversions
   - Cache management

2. **Integration Tests**:
   - Full asset processing pipeline
   - GPU/CPU fallback behavior
   - Manifest updating

3. **Performance Tests**:
   - Benchmark against Python tools
   - GPU vs CPU performance
   - Memory usage analysis

4. **Regression Tests**:
   - Output comparison with Python tools
   - Visual inspection of processed images
   - Cache validation consistency

### Test Implementation

```cpp
// Test cases to implement
class AssetToolkitTests {
    void testImageLoading();
    void testEffectApplication();
    void testColorConversions();
    void testBlurAlgorithms();
    void testCacheValidation();
    void testAnimationProcessing();
    void testGPUAcceleration();
    void testManifestOperations();
};
```

## Implementation Order

1. **Foundation Layer**:
   - Create basic ImageData structures
   - Implement file I/O operations
   - Set up error handling and logging

2. **Core Image Processing**:
   - Implement RGB/HSV conversions
   - Create basic effect application
   - Add CPU-based processing

3. **GPU Acceleration**:
   - Implement CUDA accelerator
   - Implement OpenCL accelerator
   - Add GPU detection and fallback

4. **Effect System**:
   - Parse effect parameters from JSON
   - Implement foreground/background effects
   - Add effect validation

5. **Asset Processing**:
   - Implement animation frame processing
   - Add multi-threaded processing
   - Create cache management

6. **Utility Functions**:
   - Image format conversion
   - Animation retiming
   - Rebuild flag management

7. **Integration**:
   - Update RebuildQueueCoordinator
   - Replace Python calls
   - Add comprehensive error handling

8. **Testing and Optimization**:
   - Implement unit tests
   - Performance benchmarking
   - Memory optimization
   - Final validation

## Navigation Commands

```bash
# Read Overview section
sed -n '/## Overview/,/## Scope/ p' implementation_plan.md

# Read Types section
sed -n '/## Types/,/## Files/ p' implementation_plan.md

# Read Files section
sed -n '/## Files/,/## Functions/ p' implementation_plan.md

# Read Functions section
sed -n '/## Functions/,/## Classes/ p' implementation_plan.md

# Read Classes section
sed -n '/## Classes/,/## Dependencies/ p' implementation_plan.md

# Read Dependencies section
sed -n '/## Dependencies/,/## Testing/ p' implementation_plan.md

# Read Testing section
sed -n '/## Testing/,/## Implementation Order/ p' implementation_plan.md

# Read Implementation Order section
sed -n '/## Implementation Order/,$ p' implementation_plan.md
```

## Additional Notes

- All functionality must produce identical output to Python tools
- Maintain backward compatibility with existing manifest formats
- Preserve all logging and error reporting behavior
- Implement comprehensive error handling
- Ensure thread safety for multi-threaded operations
- Optimize for both performance and memory usage
