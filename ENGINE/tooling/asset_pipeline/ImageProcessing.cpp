#include "ImageProcessing.hpp"
#include "AssetToolkit.hpp"
#include "utils/log.hpp"
#include <vector>
#include <cmath>
#include <algorithm>
#include <numeric>

// Image processing constants
constexpr float PI = 3.14159265358979323846f;
constexpr float EPSILON = 1e-6f;

namespace vibble {

// Helper functions for clamping
inline float clamp01(float value) {
    return std::max(0.0f, std::min(1.0f, value));
}

template<typename T>
inline T clamp(T value, T min, T max) {
    return std::max(min, std::min(max, value));
}

namespace colorspace {

// Convert RGB to HSV color space
void colorspace::rgbToHsv(float r, float g, float b, float& h, float& s, float& v) {
    // Clamp input values to [0, 1] range
    r = clamp01(r);
    g = clamp01(g);
    b = clamp01(b);

    float max = std::max({r, g, b});
    float min = std::min({r, g, b});
    float delta = max - min;

    // Value (brightness)
    v = max;

    // Saturation
    if (max > 0.0f) {
        s = delta / max;
    } else {
        s = 0.0f;
        h = 0.0f; // Undefined, but set to 0
        return;
    }

    // Hue
    if (delta < EPSILON) {
        h = 0.0f; // Undefined, but set to 0
    } else {
        if (max == r) {
            h = 60.0f * std::fmod((g - b) / delta, 6.0f);
        } else if (max == g) {
            h = 60.0f * (((b - r) / delta) + 2.0f);
        } else { // max == b
            h = 60.0f * (((r - g) / delta) + 4.0f);
        }

        if (h < 0.0f) {
            h += 360.0f;
        }
    }

    // Normalize hue to [0, 1] range
    h /= 360.0f;
    }
} // namespace colorspace

// Convert HSV to RGB color space
void colorspace::hsvToRgb(float h, float s, float v, float& r, float& g, float& b) {
    // Clamp input values
    h = clamp01(h);
    s = clamp01(s);
    v = clamp01(v);

    if (s < EPSILON) {
        // Gray scale
        r = g = b = v;
        return;
    }

    // Convert hue to sector [0, 6)
    float h6 = h * 6.0f;
    if (h6 >= 6.0f) {
        h6 = 0.0f;
    }

    int sector = static_cast<int>(h6);
    float fractional = h6 - sector;

    float p = v * (1.0f - s);
    float q = v * (1.0f - s * fractional);
    float t = v * (1.0f - s * (1.0f - fractional));

    switch (sector) {
        case 0: r = v; g = t; b = p; break;
        case 1: r = q; g = v; b = p; break;
        case 2: r = p; g = v; b = t; break;
        case 3: r = p; g = q; b = v; break;
        case 4: r = t; g = p; b = v; break;
        default: r = v; g = p; b = q; break;
    }
}

// Apply brightness adjustment to image data
void ImageProcessor::applyBrightness(std::vector<uint8_t>& pixels, float brightness) {
    if (std::abs(brightness) < EPSILON) {
        return;
    }

    // Convert brightness from [-1, 1] to adjustment factor
    float adjustment = brightness;

    for (size_t i = 0; i < pixels.size(); i += 4) {
        // Skip alpha channel (index 3)
        for (int c = 0; c < 3; c++) {
            if (i + c >= pixels.size()) continue;

            float normalized = static_cast<float>(pixels[i + c]) / 255.0f;
            normalized += adjustment;
            normalized = clamp01(normalized);
            pixels[i + c] = static_cast<uint8_t>(normalized * 255.0f);
        }
    }
}

// Apply contrast adjustment to image data
void ImageProcessor::applyContrast(std::vector<uint8_t>& pixels, float contrast) {
    if (std::abs(contrast) < EPSILON) {
        return;
    }

    // Convert contrast from [-1, 1] to multiplier
    float factor = 1.0f + contrast;

    for (size_t i = 0; i < pixels.size(); i += 4) {
        // Skip alpha channel (index 3)
        for (int c = 0; c < 3; c++) {
            if (i + c >= pixels.size()) continue;

            float normalized = static_cast<float>(pixels[i + c]) / 255.0f;
            normalized = (normalized - 0.5f) * factor + 0.5f;
            normalized = clamp01(normalized);
            pixels[i + c] = static_cast<uint8_t>(normalized * 255.0f);
        }
    }
}

// Apply saturation adjustment to image data
void ImageProcessor::applySaturation(std::vector<uint8_t>& pixels, float sat_r, float sat_g, float sat_b) {
    if (std::abs(sat_r) < EPSILON && std::abs(sat_g) < EPSILON && std::abs(sat_b) < EPSILON) {
        return;
    }

    for (size_t i = 0; i < pixels.size(); i += 4) {
        if (i + 3 >= pixels.size()) continue;

        // Get RGB values
        float r = static_cast<float>(pixels[i]) / 255.0f;
        float g = static_cast<float>(pixels[i + 1]) / 255.0f;
        float b = static_cast<float>(pixels[i + 2]) / 255.0f;
        float a = static_cast<float>(pixels[i + 3]) / 255.0f;

        // Convert to grayscale (luminance)
        float gray = 0.299f * r + 0.587f * g + 0.114f * b;

        // Apply saturation factors
        auto applySaturationChannel = [](float channel, float gray, float sat_factor) {
            float factor = 1.0f + 2.0f * sat_factor;
            factor = clamp(factor, 0.0f, 3.0f);
            return gray + (channel - gray) * factor;
        };

        if (std::abs(sat_r) > EPSILON) {
            r = applySaturationChannel(r, gray, sat_r);
        }
        if (std::abs(sat_g) > EPSILON) {
            g = applySaturationChannel(g, gray, sat_g);
        }
        if (std::abs(sat_b) > EPSILON) {
            b = applySaturationChannel(b, gray, sat_b);
        }

        // Clamp and convert back to 8-bit
        pixels[i] = static_cast<uint8_t>(clamp01(r) * 255.0f);
        pixels[i + 1] = static_cast<uint8_t>(clamp01(g) * 255.0f);
        pixels[i + 2] = static_cast<uint8_t>(clamp01(b) * 255.0f);
        pixels[i + 3] = static_cast<uint8_t>(clamp01(a) * 255.0f);
    }
}

// Apply hue rotation to image data
void ImageProcessor::applyHue(std::vector<uint8_t>& pixels, float hue_degrees) {
    if (std::abs(hue_degrees) < EPSILON) {
        return;
    }

    // Convert hue from degrees to normalized value [-180, 180] -> [-0.5, 0.5]
    float hue_offset = hue_degrees / 360.0f;

    for (size_t i = 0; i < pixels.size(); i += 4) {
        if (i + 3 >= pixels.size()) continue;

        // Get RGB values and convert to [0, 1] range
        float r = static_cast<float>(pixels[i]) / 255.0f;
        float g = static_cast<float>(pixels[i + 1]) / 255.0f;
        float b = static_cast<float>(pixels[i + 2]) / 255.0f;

        // Convert RGB to HSV
        float h, s, v;
        colorspace::rgbToHsv(r, g, b, h, s, v);

        // Apply hue rotation
        h += hue_offset;
        h = std::fmod(h, 1.0f);
        if (h < 0.0f) {
            h += 1.0f;
        }

        // Convert back to RGB
        colorspace::hsvToRgb(h, s, v, r, g, b);

        // Clamp and convert back to 8-bit
        pixels[i] = static_cast<uint8_t>(clamp01(r) * 255.0f);
        pixels[i + 1] = static_cast<uint8_t>(clamp01(g) * 255.0f);
        pixels[i + 2] = static_cast<uint8_t>(clamp01(b) * 255.0f);
    }
}

// Create Gaussian kernel for blur operations
std::vector<float> ImageProcessor::createGaussianKernel(float radius, float sigma) {
    // Calculate kernel size based on radius
    int kernel_size = static_cast<int>(radius * 2) + 1;
    if (kernel_size < 3) {
        kernel_size = 3;
    }

    std::vector<float> kernel(kernel_size);
    float sum = 0.0f;

    int center = kernel_size / 2;
    for (int i = 0; i < kernel_size; i++) {
        float x = static_cast<float>(i - center);
        kernel[i] = std::exp(-(x * x) / (2.0f * sigma * sigma));
        sum += kernel[i];
    }

    // Normalize kernel
    if (sum > 0.0f) {
        for (float& value : kernel) {
            value /= sum;
        }
    }

    return kernel;
}

// Apply Gaussian blur to image data
void ImageProcessor::applyGaussianBlur(std::vector<uint8_t>& pixels, int width, int height, float radius) {
    if (radius < EPSILON || width <= 0 || height <= 0) {
        return;
    }

    // Create temporary buffer
    std::vector<uint8_t> temp_pixels = pixels;

    // Create Gaussian kernel
    std::vector<float> kernel = createGaussianKernel(radius);

    // Apply horizontal blur
    int kernel_size = static_cast<int>(kernel.size());
    int kernel_radius = kernel_size / 2;

    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            float r = 0.0f, g = 0.0f, b = 0.0f, a = 0.0f;

            for (int k = 0; k < kernel_size; k++) {
                int x_offset = x + (k - kernel_radius);
                x_offset = clamp(x_offset, 0, width - 1);

                size_t src_index = (y * width + x_offset) * 4;
                if (src_index + 3 < pixels.size()) {
                    r += pixels[src_index] * kernel[k];
                    g += pixels[src_index + 1] * kernel[k];
                    b += pixels[src_index + 2] * kernel[k];
                    a += pixels[src_index + 3] * kernel[k];
                }
            }

            size_t dest_index = (y * width + x) * 4;
            if (dest_index + 3 < temp_pixels.size()) {
                temp_pixels[dest_index] = static_cast<uint8_t>(clamp01(r) * 255.0f);
                temp_pixels[dest_index + 1] = static_cast<uint8_t>(clamp01(g) * 255.0f);
                temp_pixels[dest_index + 2] = static_cast<uint8_t>(clamp01(b) * 255.0f);
                temp_pixels[dest_index + 3] = static_cast<uint8_t>(clamp01(a) * 255.0f);
            }
        }
    }

    // Apply vertical blur
    pixels = temp_pixels;

    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            float r = 0.0f, g = 0.0f, b = 0.0f, a = 0.0f;

            for (int k = 0; k < kernel_size; k++) {
                int y_offset = y + (k - kernel_radius);
                y_offset = clamp(y_offset, 0, height - 1);

                size_t src_index = (y_offset * width + x) * 4;
                if (src_index + 3 < temp_pixels.size()) {
                    r += temp_pixels[src_index] * kernel[k];
                    g += temp_pixels[src_index + 1] * kernel[k];
                    b += temp_pixels[src_index + 2] * kernel[k];
                    a += temp_pixels[src_index + 3] * kernel[k];
                }
            }

            size_t dest_index = (y * width + x) * 4;
            if (dest_index + 3 < pixels.size()) {
                pixels[dest_index] = static_cast<uint8_t>(clamp01(r) * 255.0f);
                pixels[dest_index + 1] = static_cast<uint8_t>(clamp01(g) * 255.0f);
                pixels[dest_index + 2] = static_cast<uint8_t>(clamp01(b) * 255.0f);
                pixels[dest_index + 3] = static_cast<uint8_t>(clamp01(a) * 255.0f);
            }
        }
    }
}

// Apply sharpening to image data
void ImageProcessor::applySharpen(std::vector<uint8_t>& pixels, int width, int height, float strength) {
    if (strength < EPSILON || width <= 0 || height <= 0) {
        return;
    }

    // Create temporary buffer
    std::vector<uint8_t> temp_pixels = pixels;

    // Strength should be in range [0, 1], but we'll clamp it
    strength = clamp01(strength);

    // Apply unsharp mask algorithm
    for (int y = 1; y < height - 1; y++) {
        for (int x = 1; x < width - 1; x++) {
            // Get current pixel
            size_t center_index = (y * width + x) * 4;
            if (center_index + 3 >= pixels.size()) continue;

            float r_center = static_cast<float>(pixels[center_index]);
            float g_center = static_cast<float>(pixels[center_index + 1]);
            float b_center = static_cast<float>(pixels[center_index + 2]);

            // Get average of neighboring pixels (3x3 box blur)
            float r_avg = 0.0f, g_avg = 0.0f, b_avg = 0.0f;
            int count = 0;

            for (int dy = -1; dy <= 1; dy++) {
                for (int dx = -1; dx <= 1; dx++) {
                    if (dx == 0 && dy == 0) continue;

                    int nx = x + dx;
                    int ny = y + dy;

                    if (nx >= 0 && nx < width && ny >= 0 && ny < height) {
                        size_t neighbor_index = (ny * width + nx) * 4;
                        if (neighbor_index + 3 < pixels.size()) {
                            r_avg += pixels[neighbor_index];
                            g_avg += pixels[neighbor_index + 1];
                            b_avg += pixels[neighbor_index + 2];
                            count++;
                        }
                    }
                }
            }

            if (count > 0) {
                r_avg /= count;
                g_avg /= count;
                b_avg /= count;

                // Apply unsharp mask: original + strength * (original - average)
                float r_result = r_center + strength * (r_center - r_avg);
                float g_result = g_center + strength * (g_center - g_avg);
                float b_result = b_center + strength * (b_center - b_avg);

                // Clamp and store result
                temp_pixels[center_index] = static_cast<uint8_t>(clamp01(r_result / 255.0f) * 255.0f);
                temp_pixels[center_index + 1] = static_cast<uint8_t>(clamp01(g_result / 255.0f) * 255.0f);
                temp_pixels[center_index + 2] = static_cast<uint8_t>(clamp01(b_result / 255.0f) * 255.0f);
            }
        }
    }

    pixels = temp_pixels;
}

// Apply color adjustments to image
void ImageProcessor::applyColorAdjustments(ImageData& image, const EffectParams& params) {
    if (!image.isValid() || !params.validate()) {
        return;
    }

    // Apply brightness
    if (std::abs(params.brightness) > EPSILON) {
        applyBrightness(image.pixels, params.brightness);
    }

    // Apply contrast
    if (std::abs(params.contrast) > EPSILON) {
        applyContrast(image.pixels, params.contrast);
    }

    // Apply saturation
    if (std::abs(params.saturation_red) > EPSILON ||
        std::abs(params.saturation_green) > EPSILON ||
        std::abs(params.saturation_blue) > EPSILON) {
        applySaturation(image.pixels, params.saturation_red, params.saturation_green, params.saturation_blue);
    }

    // Apply hue
    if (std::abs(params.hue) > EPSILON) {
        applyHue(image.pixels, params.hue);
    }
}

// Apply effects to image data
void ImageProcessor::applyEffects(ImageData& image, const EffectParams& params) {
    if (!image.isValid() || !params.validate()) {
        return;
    }

    // Apply color adjustments first
    applyColorAdjustments(image, params);

    // Apply blur/sharpen based on blur parameter
    if (std::abs(params.blur) > EPSILON) {
        applyBlurOrSharpen(image, params.blur, params.is_foreground);
    }
}

// Apply blur or sharpen based on blur value
void ImageProcessor::applyBlurOrSharpen(ImageData& image, float blur_value, bool is_foreground) {
    if (!image.isValid() || std::abs(blur_value) < EPSILON) {
        return;
    }

    // Clamp blur value to [-1, 1] range
    blur_value = clamp(blur_value, -1.0f, 1.0f);

    if (blur_value > 0.0f) {
        // Apply blur (positive values)
        float max_radius = 20.0f;
        float base_radius = blur_value * max_radius;

        if (base_radius < 1.0f) {
            base_radius = 1.0f;
        }

        // Different blur for foreground vs background
        float final_radius = is_foreground ? base_radius * 2.0f : base_radius * 1.3f;
        applyGaussianBlur(image.pixels, image.width, image.height, final_radius);
    } else {
        // Apply sharpen (negative values)
        float strength = -blur_value; // Convert to positive strength
        applySharpen(image.pixels, image.width, image.height, strength);
    }
}

} // namespace vibble
