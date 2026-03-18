// apply_effects_cli.cpp
//
// CLI tool to apply color effects to images for preview generation
// Replaces apply_color_effects.py
// Supports foreground and background layer types with different blur behaviors

#include "image_cache_generator.hpp"

#include <iostream>
#include <string>
#include <cstring>
#include <filesystem>

namespace fs = std::filesystem;

void print_usage(const char* prog_name) {
    std::cout << "Usage: " << prog_name << " <input> <output> <layer_type> <contrast> <brightness> <blur> <sat_r> <sat_g> <sat_b> <hue>\n\n";
    std::cout << "ARGUMENTS:\n";
    std::cout << "  <input>       Input PNG image path\n";
    std::cout << "  <output>      Output PNG image path\n";
    std::cout << "  <layer_type>  Layer type: 'foreground' or 'background'\n";
    std::cout << "  <contrast>    Contrast adjustment (-1.0 to 1.0)\n";
    std::cout << "  <brightness>  Brightness adjustment (-1.0 to 1.0)\n";
    std::cout << "  <blur>        Blur radius (-1.0 to 1.0, negative=sharpen, positive=blur)\n";
    std::cout << "  <sat_r>       Red saturation (-1.0 to 1.0)\n";
    std::cout << "  <sat_g>       Green saturation (-1.0 to 1.0)\n";
    std::cout << "  <sat_b>       Blue saturation (-1.0 to 1.0)\n";
    std::cout << "  <hue>         Hue shift in degrees (-180.0 to 180.0)\n\n";
    std::cout << "Examples:\n";
    std::cout << "  " << prog_name << " input.png output.png foreground 0.1 0.2 0.0 0.0 0.0 0.0 0.0\n";
    std::cout << "  " << prog_name << " input.png output.png background 0.0 0.1 0.5 0.0 0.0 0.0 0.0\n";
}

int main(int argc, char** argv) {
    if (argc < 11) {
        std::cerr << "Error: insufficient arguments\n\n";
        print_usage(argv[0]);
        return 2;
    }

    fs::path input_path(argv[1]);
    fs::path output_path(argv[2]);
    std::string layer_type(argv[3]);

    // Parse effect parameters
    imgcache::EffectsParams fx;
    try {
        fx.contrast = std::stof(argv[4]);
        fx.brightness = std::stof(argv[5]);
        float blur_value = std::stof(argv[6]);
        fx.saturation_r = std::stof(argv[7]);
        fx.saturation_g = std::stof(argv[8]);
        fx.saturation_b = std::stof(argv[9]);
        fx.hue_shift = std::stof(argv[10]);

        // Handle blur vs sharpen
        // Python uses negative blur for sharpen, positive for blur
        if (blur_value < 0.0f) {
            fx.sharpen_amount = -blur_value;
            fx.blur_radius = 0.0f;
        } else {
            fx.blur_radius = blur_value;
            fx.sharpen_amount = 0.0f;
        }
    }
    catch (const std::exception& e) {
        std::cerr << "Error: invalid numeric argument: " << e.what() << "\n";
        return 2;
    }

    // Validate layer type
    bool is_foreground = (layer_type == "foreground");
    bool is_background = (layer_type == "background");
    if (!is_foreground && !is_background) {
        std::cerr << "Error: layer_type must be 'foreground' or 'background', got: " << layer_type << "\n";
        return 2;
    }
    const imgcache::EffectLayerMode layer_mode =
        is_foreground ? imgcache::EffectLayerMode::Foreground
                      : imgcache::EffectLayerMode::Background;

    // Check input file exists
    if (!fs::exists(input_path)) {
        std::cerr << "Error: input file does not exist: " << input_path.string() << "\n";
        return 3;
    }

    // Create output directory if needed
    fs::path output_dir = output_path.parent_path();
    if (!output_dir.empty() && !fs::exists(output_dir)) {
        std::error_code ec;
        fs::create_directories(output_dir, ec);
        if (ec) {
            std::cerr << "Error: failed to create output directory: " << ec.message() << "\n";
            return 3;
        }
    }

    // Load image
    std::string err;
    auto img_opt = imgcache::ImageCacheGenerator::LoadPngRGBA(input_path, err);
    if (!img_opt) {
        std::cerr << "Error: failed to load input image: " << err << "\n";
        return 3;
    }

    std::cout << "Loaded image: " << input_path.string()
              << " (" << img_opt->w << "x" << img_opt->h << ")\n";

    // Apply effects
    auto result_opt = imgcache::ImageCacheGenerator::ApplyEffects(*img_opt, fx, layer_mode, err);
    if (!result_opt) {
        std::cerr << "Error: failed to apply effects: " << err << "\n";
        return 1;
    }

    // Save output
    if (!imgcache::ImageCacheGenerator::SavePngRGBA(output_path, *result_opt, err)) {
        std::cerr << "Error: failed to save output image: " << err << "\n";
        return 3;
    }

    std::cout << "Preview image generated: " << output_path.string() << "\n";
    std::cout << "  Layer type: " << layer_type << "\n";
    std::cout << "  Effects: contrast=" << fx.contrast
              << " brightness=" << fx.brightness
              << " blur=" << fx.blur_radius
              << " sharpen=" << fx.sharpen_amount << "\n";
    std::cout << "  Saturation: R=" << fx.saturation_r
              << " G=" << fx.saturation_g
              << " B=" << fx.saturation_b << "\n";
    std::cout << "  Hue shift: " << fx.hue_shift << " degrees\n";

    return 0;
}
