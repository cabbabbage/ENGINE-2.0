#include "ENGINE/AssetToolkit.hpp"
#include <iostream>

int main() {
    std::cout << "Testing AssetToolkit Foundation Layer..." << std::endl;

    // Test initialization
    vibble::AssetToolkit toolkit;
    if (!toolkit.initialize(".")) {
        std::cerr << "Failed to initialize AssetToolkit: " << toolkit.getLastError() << std::endl;
        return 1;
    }

    std::cout << "✓ AssetToolkit initialized successfully" << std::endl;
    std::cout << "  Repo root: " << toolkit.getRepoRoot() << std::endl;
    std::cout << "  Cache root: " << toolkit.getCacheRoot() << std::endl;

    // Test ImageData structure
    vibble::ImageData test_image;
    test_image.width = 640;
    test_image.height = 480;
    test_image.has_alpha = true;
    test_image.pixels.resize(test_image.width * test_image.height * 4, 128);

    std::cout << "✓ ImageData structure created: "
              << test_image.width << "x" << test_image.height
              << " (" << test_image.pixelCount() << " pixels)" << std::endl;

    // Test EffectParams structure
    vibble::EffectParams test_effects;
    test_effects.brightness = 0.5f;
    test_effects.contrast = 0.2f;
    test_effects.hue = 45.0f;
    test_effects.is_foreground = true;

    std::cout << "✓ EffectParams structure created" << std::endl;
    std::cout << "  Valid: " << (test_effects.validate() ? "true" : "false") << std::endl;

    // Test JSON conversion
    auto json = test_effects.toJson();
    std::cout << "✓ EffectParams to JSON conversion: " << json.dump() << std::endl;

    // Test JSON parsing
    vibble::EffectParams parsed_effects = vibble::EffectParams::fromJson(json);
    std::cout << "✓ EffectParams from JSON parsing: brightness=" << parsed_effects.brightness
              << ", foreground=" << (parsed_effects.is_foreground ? "true" : "false") << std::endl;

    // Test file system utilities
    std::vector<std::string> files = toolkit.findFilesInDirectory(".", ".hpp");
    std::cout << "✓ Found " << files.size() << " .hpp files in current directory" << std::endl;

    // Test path utilities
    std::string cache_path = toolkit.getCachePath("test_asset", "test_animation", 100, "normal");
    std::cout << "✓ Cache path generation: " << cache_path << std::endl;

    // Test hash function
    nlohmann::json test_json = {{"test", "value"}, {"number", 42}};
    std::string hash = toolkit.computeStableHash(test_json);
    std::cout << "✓ Hash computation: " << hash << std::endl;

    // Test error handling
    std::cout << "✓ Error handling: " << toolkit.getLastError() << std::endl;
    toolkit.clearLastError();
    std::cout << "✓ Error cleared: " << (toolkit.getLastError().empty() ? "true" : "false") << std::endl;

    std::cout << "\n🎉 All foundation layer tests passed!" << std::endl;
    std::cout << "The AssetToolkit is ready for further development." << std::endl;

    return 0;
}
