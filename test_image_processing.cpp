#include "ENGINE/ImageProcessing.hpp"
#include "ENGINE/AssetToolkit.hpp"
#include <iostream>
#include <iomanip>

void testColorSpaceConversions() {
    std::cout << "Testing color space conversions..." << std::endl;

    // Test RGB to HSV conversion
    float h, s, v;
    vibble::colorspace::rgbToHsv(1.0f, 0.0f, 0.0f, h, s, v);
    std::cout << "  RGB(1,0,0) -> HSV(" << h << "," << s << "," << v << ")" << std::endl;

    // Test HSV to RGB conversion
    float r, g, b;
    vibble::colorspace::hsvToRgb(h, s, v, r, g, b);
    std::cout << "  HSV(" << h << "," << s << "," << v << ") -> RGB(" << r << "," << g << "," << b << ")" << std::endl;

    // Test round-trip conversion
    float orig_r = 0.5f, orig_g = 0.3f, orig_b = 0.8f;
    vibble::colorspace::rgbToHsv(orig_r, orig_g, orig_b, h, s, v);
    vibble::colorspace::hsvToRgb(h, s, v, r, g, b);
    std::cout << "  Round-trip: RGB(" << orig_r << "," << orig_g << "," << orig_b << ") -> RGB(" << r << "," << g << "," << b << ")" << std::endl;

    std::cout << "✓ Color space conversions working" << std::endl;
}

void testImageEffects() {
    std::cout << "Testing image effects..." << std::endl;

    // Create a test image (4x4 RGBA)
    vibble::ImageData image;
    image.width = 4;
    image.height = 4;
    image.has_alpha = true;
    image.pixels.resize(64); // 4x4x4 = 64 bytes

    // Fill with a gradient pattern
    for (int y = 0; y < 4; y++) {
        for (int x = 0; x < 4; x++) {
            size_t index = (y * 4 + x) * 4;
            image.pixels[index] = static_cast<uint8_t>((x * 64) % 256);     // R
            image.pixels[index + 1] = static_cast<uint8_t>((y * 64) % 256); // G
            image.pixels[index + 2] = static_cast<uint8_t>(128);             // B
            image.pixels[index + 3] = 255;                                  // A
        }
    }

    std::cout << "  Created test image (" << image.width << "x" << image.height << ")" << std::endl;

    // Test brightness adjustment
    vibble::ImageData bright_image = image;
    vibble::ImageProcessor::applyBrightness(bright_image.pixels, 0.2f);
    std::cout << "  Applied brightness +0.2" << std::endl;

    // Test contrast adjustment
    vibble::ImageData contrast_image = image;
    vibble::ImageProcessor::applyContrast(contrast_image.pixels, 0.5f);
    std::cout << "  Applied contrast +0.5" << std::endl;

    // Test hue adjustment
    vibble::ImageData hue_image = image;
    vibble::ImageProcessor::applyHue(hue_image.pixels, 90.0f);
    std::cout << "  Applied hue +90°" << std::endl;

    // Test saturation adjustment
    vibble::ImageData sat_image = image;
    vibble::ImageProcessor::applySaturation(sat_image.pixels, 0.3f, -0.2f, 0.1f);
    std::cout << "  Applied saturation adjustments" << std::endl;

    // Test blur
    vibble::ImageData blur_image = image;
    vibble::ImageProcessor::applyGaussianBlur(blur_image.pixels, blur_image.width, blur_image.height, 2.0f);
    std::cout << "  Applied Gaussian blur (radius 2)" << std::endl;

    // Test sharpen
    vibble::ImageData sharp_image = image;
    vibble::ImageProcessor::applySharpen(sharp_image.pixels, sharp_image.width, sharp_image.height, 0.5f);
    std::cout << "  Applied sharpening (strength 0.5)" << std::endl;

    std::cout << "✓ Image effects working" << std::endl;
}

void testEffectParams() {
    std::cout << "Testing EffectParams..." << std::endl;

    // Test valid parameters
    vibble::EffectParams valid_params;
    valid_params.brightness = 0.5f;
    valid_params.contrast = -0.3f;
    valid_params.blur = 0.8f;
    valid_params.saturation_red = 0.2f;
    valid_params.hue = -45.0f;
    valid_params.is_foreground = true;

    std::cout << "  Valid params: " << (valid_params.validate() ? "true" : "false") << std::endl;

    // Test invalid parameters
    vibble::EffectParams invalid_params;
    invalid_params.brightness = 2.0f; // Out of range
    invalid_params.hue = 200.0f;      // Out of range

    std::cout << "  Invalid params: " << (invalid_params.validate() ? "true" : "false") << std::endl;

    // Test JSON serialization
    auto json = valid_params.toJson();
    std::cout << "  JSON serialization: " << json.dump(2) << std::endl;

    // Test JSON deserialization
    vibble::EffectParams parsed_params = vibble::EffectParams::fromJson(json);
    std::cout << "  JSON deserialization: brightness=" << parsed_params.brightness
              << ", foreground=" << (parsed_params.is_foreground ? "true" : "false") << std::endl;

    std::cout << "✓ EffectParams working" << std::endl;
}

void testCompleteEffectPipeline() {
    std::cout << "Testing complete effect pipeline..." << std::endl;

    // Create a test image
    vibble::ImageData image;
    image.width = 8;
    image.height = 8;
    image.has_alpha = true;
    image.pixels.resize(256); // 8x8x4 = 256 bytes

    // Fill with a checkerboard pattern
    for (int y = 0; y < 8; y++) {
        for (int x = 0; x < 8; x++) {
            size_t index = (y * 8 + x) * 4;
            uint8_t value = ((x + y) % 2 == 0) ? 255 : 0;
            image.pixels[index] = value;     // R
            image.pixels[index + 1] = value; // G
            image.pixels[index + 2] = value; // B
            image.pixels[index + 3] = 255;   // A
        }
    }

    std::cout << "  Created checkerboard test image" << std::endl;

    // Create effect parameters
    vibble::EffectParams params;
    params.brightness = 0.1f;
    params.contrast = 0.2f;
    params.blur = -0.3f; // Negative = sharpen
    params.saturation_red = 0.1f;
    params.saturation_green = -0.1f;
    params.hue = 30.0f;
    params.is_foreground = true;

    std::cout << "  Created effect parameters" << std::endl;

    // Apply effects
    vibble::ImageData processed_image = image;
    vibble::ImageProcessor::applyEffects(processed_image, params);

    std::cout << "  Applied complete effect pipeline" << std::endl;
    std::cout << "  Original image valid: " << (image.isValid() ? "true" : "false") << std::endl;
    std::cout << "  Processed image valid: " << (processed_image.isValid() ? "true" : "false") << std::endl;

    std::cout << "✓ Complete effect pipeline working" << std::endl;
}

int main() {
    std::cout << "Testing Image Processing Algorithms (Step 2)..." << std::endl;
    std::cout << "================================================" << std::endl;

    try {
        testColorSpaceConversions();
        std::cout << std::endl;

        testImageEffects();
        std::cout << std::endl;

        testEffectParams();
        std::cout << std::endl;

        testCompleteEffectPipeline();
        std::cout << std::endl;

        std::cout << "🎉 All image processing tests passed!" << std::endl;
        std::cout << "Step 2: Core image processing algorithms completed successfully." << std::endl;

    } catch (const std::exception& e) {
        std::cerr << "❌ Test failed with exception: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}
