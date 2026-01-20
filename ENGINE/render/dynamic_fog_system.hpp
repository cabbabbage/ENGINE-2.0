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
        float opacity = 1.0f;
        int world_z = 0;
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

private:
    static constexpr int kNumFogTextures = 20;
    static constexpr int kFogResolutionLayer = 4;  // Changed from 6 to 4 for sparser grid (729px vs 81px spacing)
    static constexpr float kBaseFogSize = 2048.0f *2;  // Changed from 512 to 2048 for much larger fog
    static constexpr float kNearFogDistance = 1200.0f;  // 0% opacity
    static constexpr float kFarFogDistance = 1800.0f;   // 100% opacity

    // Fog textures (10 base textures loaded once)
    std::vector<SDL_Texture*> fog_textures_;

    // Track which fog texture index assigned to each grid point
    // Key: world coordinates as hash, Value: fog texture index (0-9)
    std::unordered_map<std::uint64_t, int> fog_assignments_;

    // Per-frame fog render data
    std::vector<FogSprite> active_fog_sprites_;

    SDL_Renderer* renderer_ = nullptr;

    // Helper methods
    float calculate_fog_opacity(float distance_to_camera) const;
    std::uint64_t make_grid_point_hash(int world_x, int world_y, int world_z, int layer) const;
    int assign_fog_texture_for_point(int world_x, int world_y, int world_z, int layer);
};
