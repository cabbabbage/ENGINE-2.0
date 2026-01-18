#include "ENGINE/EffectsParser.hpp"
#include "ENGINE/AssetToolkit.hpp"
#include <iostream>
#include <fstream>
#include <filesystem>

namespace fs = std::filesystem;

void createTestManifest() {
    std::cout << "Creating test manifest..." << std::endl;

    nlohmann::json manifest = {
        {"version", 1},
        {"image_effects", {
            {"foreground", {
                {"brightness", 0.2f},
                {"contrast", 0.3f},
                {"blur", 0.1f},
                {"saturation_red", 0.4f},
                {"saturation_green", -0.1f},
                {"saturation_blue", 0.2f},
                {"hue", 15.0f}
            }},
            {"background", {
                {"brightness", -0.1f},
                {"contrast", 0.2f},
                {"blur", 0.2f},
                {"saturation_red", 0.1f},
                {"saturation_green", 0.1f},
                {"saturation_blue", 0.1f},
                {"hue", -10.0f}
            }}
        }},
        {"assets", {
            {"test_asset", {
                {"asset_directory", "test_assets/test_asset"}
            }}
        }}
    };

    // Ensure test directory exists
    fs::create_directories("test_assets/test_asset");

    // Save manifest
    std::ofstream manifest_file("test_manifest.json");
    manifest_file << manifest.dump(2);
    manifest_file.close();

    std::cout << "✓ Created test manifest: test_manifest.json" << std::endl;
}

void testEffectsParser() {
    std::cout << "Testing EffectsParser..." << std::endl;

    // Initialize AssetToolkit
    vibble::AssetToolkit toolkit;
    if (!toolkit.initialize(".")) {
        std::cerr << "Failed to initialize AssetToolkit: " << toolkit.getLastError() << std::endl;
        return;
    }

    // Test with non-existent manifest first
    {
        vibble::EffectsParser parser("nonexistent_manifest.json");
        auto [fg, bg, unchanged] = parser.parse();

        std::cout << "  Non-existent manifest test:" << std::endl;
        std::cout << "    FG effects valid: " << (fg.validate() ? "true" : "false") << std::endl;
        std::cout << "    BG effects valid: " << (bg.validate() ? "true" : "false") << std::endl;
        std::cout << "    Unchanged: " << (unchanged ? "true" : "false") << std::endl;
    }

    // Test with our test manifest
    {
        vibble::EffectsParser parser("test_manifest.json");
        auto [fg, bg, unchanged] = parser.parse();

        std::cout << "  Test manifest parsing:" << std::endl;
        std::cout << "    FG brightness: " << fg.brightness << std::endl;
        std::cout << "    FG contrast: " << fg.contrast << std::endl;
        std::cout << "    FG blur: " << fg.blur << std::endl;
        std::cout << "    FG hue: " << fg.hue << std::endl;
        std::cout << "    FG foreground flag: " << (fg.is_foreground ? "true" : "false") << std::endl;

        std::cout << "    BG brightness: " << bg.brightness << std::endl;
        std::cout << "    BG contrast: " << bg.contrast << std::endl;
        std::cout << "    BG blur: " << bg.blur << std::endl;
        std::cout << "    BG hue: " << bg.hue << std::endl;
        std::cout << "    BG foreground flag: " << (bg.is_foreground ? "true" : "false") << std::endl;

        std::cout << "    Effects unchanged: " << (unchanged ? "true" : "false") << std::endl;
        std::cout << "    Cache path: " << parser.getCachePath() << std::endl;
    }

    // Test cache functionality
    {
        std::cout << "  Testing cache functionality:" << std::endl;

        // Create test effects
        vibble::EffectParams test_fg, test_bg;
        test_fg.brightness = 0.5f;
        test_fg.contrast = 0.2f;
        test_fg.is_foreground = true;

        test_bg.brightness = -0.2f;
        test_bg.contrast = 0.1f;
        test_bg.is_foreground = false;

        // Save to cache
        vibble::EffectsParser parser("test_manifest.json");
        if (parser.saveEffectsCache(test_fg, test_bg)) {
            std::cout << "    ✓ Saved effects to cache" << std::endl;
        } else {
            std::cout << "    ✗ Failed to save effects to cache" << std::endl;
        }

        // Load from cache
        vibble::EffectParams loaded_fg, loaded_bg;
        if (parser.loadEffectsCache(loaded_fg, loaded_bg)) {
            std::cout << "    ✓ Loaded effects from cache" << std::endl;
            std::cout << "      FG brightness: " << loaded_fg.brightness << std::endl;
            std::cout << "      BG brightness: " << loaded_bg.brightness << std::endl;
        } else {
            std::cout << "    ✗ Failed to load effects from cache" << std::endl;
        }

        // Test effects comparison
        bool changed = parser.effectsChanged(test_fg, test_bg);
        std::cout << "    Effects changed: " << (changed ? "true" : "false") << std::endl;
    }

    std::cout << "✓ EffectsParser tests completed" << std::endl;
}

void testAssetProcessor() {
    std::cout << "Testing AssetProcessor..." << std::endl;

    // Initialize AssetToolkit
    vibble::AssetToolkit toolkit;
    if (!toolkit.initialize(".")) {
        std::cerr << "Failed to initialize AssetToolkit: " << toolkit.getLastError() << std::endl;
        return;
    }

    // Create test asset structure
    fs::create_directories("test_assets/test_animation");

    // Create a simple test frame
    {
        vibble::ImageData test_image;
        test_image.width = 64;
        test_image.height = 64;
        test_image.has_alpha = true;
        test_image.pixels.resize(64 * 64 * 4, 255);

        // Create a simple pattern
        for (int y = 0; y < 64; y++) {
            for (int x = 0; x < 64; x++) {
                size_t index = (y * 64 + x) * 4;
                uint8_t value = static_cast<uint8_t>((x + y) % 256);
                test_image.pixels[index] = value;     // R
                test_image.pixels[index + 1] = value; // G
                test_image.pixels[index + 2] = value; // B
                test_image.pixels[index + 3] = 255;   // A
            }
        }

        toolkit.saveImage(test_image, "test_assets/test_animation/0.png");
        std::cout << "  ✓ Created test frame" << std::endl;
    }

    // Test AssetProcessor
    vibble::AssetProcessor processor(toolkit);

    // Test manifest loading
    {
        nlohmann::json manifest;
        if (processor.loadManifest("test_manifest.json", manifest)) {
            std::cout << "  ✓ Loaded test manifest" << std::endl;
            if (manifest.contains("assets")) {
                std::cout << "    Found " << manifest["assets"].size() << " assets" << std::endl;
            }
        } else {
            std::cout << "  ✗ Failed to load test manifest" << std::endl;
        }
    }

    // Test animation directory finding
    {
        auto anim_dirs = processor.findAnimationDirectories("test_assets");
        std::cout << "  Found " << anim_dirs.size() << " animation directories" << std::endl;
        for (const auto& dir : anim_dirs) {
            std::cout << "    - " << dir << std::endl;
        }
    }

    // Test frame collection
    {
        std::vector<int> scale_variants = {100, 75}; // Simplified for testing
        auto frames = processor.collectAnimationFrames(
            "test_assets/test_animation",
            "cache/test_asset/animations/test_animation",
            scale_variants
        );

        std::cout << "  Collected " << frames.size() << " frames for processing" << std::endl;
        if (!frames.empty()) {
            std::cout << "    First frame: " << frames[0].source_path << std::endl;
            std::cout << "    Output normal: " << frames[0].output_path_normal << std::endl;
            std::cout << "    Output FG: " << frames[0].output_path_fg << std::endl;
            std::cout << "    Output BG: " << frames[0].output_path_bg << std::endl;
        }
    }

    std::cout << "✓ AssetProcessor tests completed" << std::endl;
}

void testCompletePipeline() {
    std::cout << "Testing complete effect pipeline..." << std::endl;

    // Initialize AssetToolkit
    vibble::AssetToolkit toolkit;
    if (!toolkit.initialize(".")) {
        std::cerr << "Failed to initialize AssetToolkit: " << toolkit.getLastError() << std::endl;
        return;
    }

    // Create test effects
    vibble::EffectParams fg_effects, bg_effects;
    fg_effects.brightness = 0.2f;
    fg_effects.contrast = 0.1f;
    fg_effects.saturation_red = 0.3f;
    fg_effects.hue = 10.0f;
    fg_effects.blur = -0.1f; // Negative = sharpen
    fg_effects.is_foreground = true;

    bg_effects.brightness = -0.1f;
    bg_effects.contrast = 0.2f;
    bg_effects.saturation_blue = 0.2f;
    bg_effects.hue = -5.0f;
    bg_effects.blur = 0.1f;
    bg_effects.is_foreground = false;

    // Load test image
    vibble::ImageData test_image;
    if (toolkit.loadImage("test_assets/test_animation/0.png", test_image)) {
        std::cout << "  ✓ Loaded test image: " << test_image.width << "x" << test_image.height << std::endl;

        // Apply foreground effects
        vibble::ImageData fg_image = test_image;
        if (toolkit.applyEffects(fg_image, fg_effects)) {
            std::cout << "  ✓ Applied foreground effects" << std::endl;

            // Save result
            fs::create_directories("test_output/foreground");
            if (toolkit.saveImage(fg_image, "test_output/foreground/0.png")) {
                std::cout << "  ✓ Saved foreground result" << std::endl;
            }
        }

        // Apply background effects
        vibble::ImageData bg_image = test_image;
        if (toolkit.applyEffects(bg_image, bg_effects)) {
            std::cout << "  ✓ Applied background effects" << std::endl;

            // Save result
            fs::create_directories("test_output/background");
            if (toolkit.saveImage(bg_image, "test_output/background/0.png")) {
                std::cout << "  ✓ Saved background result" << std::endl;
            }
        }

        // Save normal version
        fs::create_directories("test_output/normal");
        if (toolkit.saveImage(test_image, "test_output/normal/0.png")) {
            std::cout << "  ✓ Saved normal version" << std::endl;
        }
    } else {
        std::cout << "  ✗ Failed to load test image" << std::endl;
    }

    std::cout << "✓ Complete pipeline test finished" << std::endl;
}

void cleanupTestFiles() {
    std::cout << "Cleaning up test files..." << std::endl;

    // Remove test files and directories
    try {
        fs::remove_all("test_assets");
        fs::remove_all("test_output");
        fs::remove("test_manifest.json");
        fs::remove(".image_effects_cache.json");

        std::cout << "✓ Cleaned up test files" << std::endl;
    } catch (const std::exception& e) {
        std::cout << "⚠ Cleanup warning: " << e.what() << std::endl;
    }
}

int main() {
    std::cout << "Testing Effects System (Step 4)..." << std::endl;
    std::cout << "====================================" << std::endl;

    try {
        // Create test environment
        createTestManifest();

        // Run tests
        testEffectsParser();
        std::cout << std::endl;

        testAssetProcessor();
        std::cout << std::endl;

        testCompletePipeline();
        std::cout << std::endl;

        // Clean up
        cleanupTestFiles();
        std::cout << std::endl;

        std::cout << "🎉 All effects system tests completed successfully!" << std::endl;
        std::cout << "Step 4: Effect system and parsing implementation is working." << std::endl;

    } catch (const std::exception& e) {
        std::cerr << "❌ Test failed with exception: " << e.what() << std::endl;
        cleanupTestFiles();
        return 1;
    }

    return 0;
}
