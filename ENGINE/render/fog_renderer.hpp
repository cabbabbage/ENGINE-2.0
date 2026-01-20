#pragma once

#include <SDL.h>
#include <string>
#include <vector>
#include <array>

/**
 * FogRenderer manages fog texture loading, variant creation, and rendering calculations.
 *
 * Fog textures provide atmospheric depth cueing by overlaying semi-transparent fog images
 * on assets based on their distance from the camera.
 *
 * - 10 fog texture images (fog_1.png to fog_10.png)
 * - Each fog image has 10 size variants (10%, 20%, ..., 100% of base size)
 * - Total: 100 textures loaded and cached
 * - Opacity interpolates linearly from 0% at 1200 units to 100% at 1800 units
 */
class FogRenderer {
public:
    // Constants for fog opacity calculation
    static constexpr float kFogForegroundDistance = 1200.0f;  // 0% opacity
    static constexpr float kFogBackgroundDistance = 1800.0f;  // 100% opacity

    // Number of fog texture images
    static constexpr int kNumFogTextures = 10;

    // Number of scale variants per fog texture (10%, 20%, ..., 100%)
    static constexpr int kNumFogVariants = 10;

    /**
     * Fog texture variant set for a single fog image.
     * Contains 10 scale variants from 10% to 100%.
     */
    struct FogVariantSet {
        std::array<SDL_Texture*, kNumFogVariants> variants{};
        int base_width = 0;
        int base_height = 0;

        FogVariantSet() {
            variants.fill(nullptr);
        }
    };

    FogRenderer(SDL_Renderer* renderer);
    ~FogRenderer();

    // Non-copyable
    FogRenderer(const FogRenderer&) = delete;
    FogRenderer& operator=(const FogRenderer&) = delete;

    /**
     * Load all fog textures from the specified directory.
     * Expects files named fog_1.png through fog_10.png.
     * Creates 10 scale variants for each fog image.
     *
     * @param fog_dir Directory containing fog texture images
     * @return true if successful, false otherwise
     */
    bool load_fog_textures(const std::string& fog_dir);

    /**
     * Get a specific fog texture variant.
     *
     * @param fog_index Index of fog texture (0-9)
     * @param target_size Target size in pixels (used to select best variant)
     * @return SDL_Texture* for the appropriate variant, or nullptr if not loaded
     */
    SDL_Texture* get_fog_variant(int fog_index, int target_size) const;

    /**
     * Calculate fog opacity based on distance from camera.
     * Uses linear interpolation:
     * - distance <= 1200: 0% opacity (no fog)
     * - distance >= 1800: 100% opacity (full fog)
     * - 1200 < distance < 1800: linear interpolation
     *
     * @param distance_from_camera Distance in screen pixels
     * @return Opacity value [0.0, 1.0]
     */
    static float calculate_fog_opacity(float distance_from_camera);

    /**
     * Check if fog textures are loaded and ready.
     */
    bool is_loaded() const { return loaded_; }

    /**
     * Get the number of loaded fog texture sets.
     */
    int num_loaded_textures() const { return static_cast<int>(fog_textures_.size()); }

private:
    SDL_Renderer* renderer_;
    std::vector<FogVariantSet> fog_textures_;
    bool loaded_ = false;

    /**
     * Create scale variants for a base fog texture.
     * Generates 10 variants at 10%, 20%, ..., 100% of base size.
     */
    bool create_fog_variants(SDL_Texture* base_texture, FogVariantSet& variant_set);

    /**
     * Clean up all loaded fog textures.
     */
    void cleanup();
};
