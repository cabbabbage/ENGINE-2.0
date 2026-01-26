#pragma once

#include "AssetToolkit.hpp"
#include <vector>
#include <cmath>
#include <algorithm>

namespace vibble {

// Image processing constants
constexpr float PI = 3.14159265358979323846f;
constexpr float EPSILON = 1e-6f;

// Color space conversion functions
namespace colorspace {

// Convert RGB to HSV color space
void rgbToHsv(float r, float g, float b, float& h, float& s, float& v);

// Convert HSV to RGB color space
void hsvToRgb(float h, float s, float v, float& r, float& g, float& b);

// Clamp value to range [0, 1]
inline float clamp01(float value) {
    return std::max(0.0f, std::min(1.0f, value));
}

// Clamp value to range [min, max]
template<typename T>
inline T clamp(T value, T min, T max) {
    return std::max(min, std::min(max, value));
}

} // namespace colorspace

// Image processing algorithms
class ImageProcessor {
public:
    // Apply brightness adjustment to image data
    static void applyBrightness(std::vector<uint8_t>& pixels, float brightness);

    // Apply contrast adjustment to image data
    static void applyContrast(std::vector<uint8_t>& pixels, float contrast);

    // Apply saturation adjustment to image data
    static void applySaturation(std::vector<uint8_t>& pixels, float sat_r, float sat_g, float sat_b);

    // Apply hue rotation to image data
    static void applyHue(std::vector<uint8_t>& pixels, float hue_degrees);

    // Apply Gaussian blur to image data
    static void applyGaussianBlur(std::vector<uint8_t>& pixels, int width, int height, float radius);

    // Apply sharpening to image data
    static void applySharpen(std::vector<uint8_t>& pixels, int width, int height, float strength);

    // Apply effects to image data
    static void applyEffects(ImageData& image, const EffectParams& params);

    // Apply blur or sharpen based on blur value
    static void applyBlurOrSharpen(ImageData& image, float blur_value, bool is_foreground);

private:
    // Helper function for Gaussian blur
    static std::vector<float> createGaussianKernel(float radius, float sigma = 1.0f);

    // Helper function for color adjustments
    static void applyColorAdjustments(ImageData& image, const EffectParams& params);
};

} // namespace vibble
