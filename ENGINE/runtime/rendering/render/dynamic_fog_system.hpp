#pragma once

#include <SDL.h>
#include <vector>
#include <unordered_map>

class WarpedScreenGrid;
namespace world {
    class WorldGrid;
    struct GridKey;
    struct GridKeyHash;
}

class DynamicFogSystem {
public:
    struct FogSprite {
        SDL_Texture* texture = nullptr;
        SDL_FPoint world_pos{0.0f, 0.0f};
        SDL_FPoint screen_pos{0.0f, 0.0f};
        float scale = 1.0f;
        int world_z = 0;
        int texture_w = 0;
        int texture_h = 0;
    };

    DynamicFogSystem();
    ~DynamicFogSystem();

    // Initialize fog system - loads textures from fog/ directory
    bool initialize(SDL_Renderer* renderer);

    // Update fog sprites for current frame based on camera position
    void update(const WarpedScreenGrid& cam, const world::WorldGrid& grid);

    // Render all active fog sprites
    void render(SDL_Renderer* renderer, const WarpedScreenGrid& cam);

    // Get fog sprites for rendering in z-order with assets
    const std::vector<FogSprite>& get_fog_sprites() const { return active_fog_sprites_; }

    // ========================================================================
    // FOG PLACEMENT SETTINGS - Adjust grid spacing here
    // ========================================================================
    // Grid spacing in world pixels (how far apart fog sprites are placed)
    // Smaller = denser fog, Larger = sparser fog
    // Examples: 243 (dense), 729 (medium), 2187 (sparse)
    static constexpr float kDefaultGridSpacingMultiplier = 2.0f;

    // ========================================================================
    // FOG SIZE SETTINGS - Adjust fog size here
    // ========================================================================
    // Base size multiplier as a percentage of original texture size
    // This value multiplies the texture dimensions to get world size
    // Examples: 2.0f (200% = 2x texture size), 4.0f (400% = 4x texture size)
    static constexpr float kDefaultBaseSizeScale = 3.0f;

    // ========================================================================
    // FOG VERTICAL OFFSET SETTINGS - Adjust fog vertical position here
    // ========================================================================
    // Vertical offset in pixels to shift fog rendering up (-) or down (+)
    static constexpr float kDefaultVerticalOffset = 0.0f;

    // Resolution layer for fog texture assignment (affects hashing for random but consistent placement)
    static constexpr int kFogResolutionLayer = 4;

    // Runtime configuration
    static void set_grid_spacing_multiplier(float multiplier);
    static float grid_spacing_multiplier();
    static void set_base_size_scale(float scale);
    static float base_size_scale();
    static void set_vertical_offset(float offset);
    static float vertical_offset();
    static void set_max_random_jitter(float jitter);
    static float max_random_jitter();

private:
    static constexpr int kNumFogTextures = 10;
    static constexpr float kMinRandomJitter = 0.0f;
    static constexpr float kMaxRandomJitter = 500.0f;

    struct FogTexture {
        SDL_Texture* texture = nullptr;
        int width = 0;
        int height = 0;
    };

    // Fog textures (10 base textures loaded once, dimensions cached at load)
    std::vector<FogTexture> fog_textures_;

    // Track which fog texture index assigned to each grid point
    // Key: world coordinates as hash, Value: fog texture index (0-9)
    std::unordered_map<std::uint64_t, int> fog_assignments_;

    // Per-frame fog render data
    std::vector<FogSprite> active_fog_sprites_;

    SDL_Renderer* renderer_ = nullptr;

    // Helper methods
    std::uint64_t make_grid_point_hash(int world_x, int world_y, int world_z, int layer) const;
    int assign_fog_texture_for_point(int world_x, int world_y, int world_z, int layer);

    struct FogConfig {
        float grid_spacing_multiplier = kDefaultGridSpacingMultiplier;
        float base_size_scale = kDefaultBaseSizeScale;
        float vertical_offset = kDefaultVerticalOffset;
        float max_random_jitter = 0.0f;
    };
    static FogConfig& config();

    SDL_FPoint sample_jitter_offset(int world_x, int world_y, int world_z, int layer, float max_jitter) const;
};
