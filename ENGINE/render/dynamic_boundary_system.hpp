#pragma once

#include <SDL.h>
#include <nlohmann/json.hpp>
#include <vector>
#include <unordered_map>
#include <string>
#include <memory>
#include <functional>

class WarpedScreenGrid;
class AssetLibrary;
class Assets;
namespace world {
    class WorldGrid;
}

class DynamicBoundarySystem {
public:
    struct BoundarySprite {
        SDL_Texture* texture = nullptr;
        SDL_FPoint world_pos{0.0f, 0.0f};
        SDL_FPoint screen_pos{0.0f, 0.0f};
        float scale = 1.0f;
        int world_z = 0;
        int texture_w = 0;
        int texture_h = 0;

        // Animation state
        int boundary_type_index = 0;
        int candidate_index = 0;
        int current_frame_index = 0;
        float frame_elapsed_ms = 0.0f;
        int total_frames = 1;
        float frame_duration_ms = 41.67f;  // 24 fps default
    };

    struct BoundaryCandidate {
        std::string asset_name;
        int chance = 0;
        SDL_Texture* texture = nullptr;
        int texture_w = 0;
        int texture_h = 0;
    };

    struct BoundaryType {
        std::string spawn_id;
        std::string display_name;
        int grid_resolution = 5;
        std::vector<BoundaryCandidate> candidates;
        int total_chance = 0;
    };

    DynamicBoundarySystem();
    ~DynamicBoundarySystem();

    // Initialize with asset library (for texture lookups)
    bool initialize(SDL_Renderer* renderer, AssetLibrary* asset_library);

    // Update boundary sprites for current frame - reads config LIVE from assets
    void update(const WarpedScreenGrid& cam, const world::WorldGrid& grid, Assets* assets, float delta_ms);

    // Get sprites for depth-sorted rendering
    const std::vector<BoundarySprite>& get_boundary_sprites() const { return active_boundary_sprites_; }

    // Configuration - similar to fog system
    static constexpr float kDefaultGridSpacingMultiplier = 1.0f;
    static constexpr float kDefaultBaseSizeScale = 1.0f;
    static constexpr float kDefaultVerticalOffset = 0.0f;

    static void set_grid_spacing_multiplier(float multiplier);
    static float grid_spacing_multiplier();
    static void set_base_size_scale(float scale);
    static float base_size_scale();
    static void set_vertical_offset(float offset);
    static float vertical_offset();
    static void set_max_random_jitter(float jitter);
    static float max_random_jitter();

    bool is_initialized() const { return initialized_; }

    // Force re-parse of boundary config on next update
    void invalidate_config() { config_dirty_ = true; }

private:
    bool initialized_ = false;
    bool config_dirty_ = true;
    SDL_Renderer* renderer_ = nullptr;
    AssetLibrary* asset_library_ = nullptr;

    // Cached boundary types (re-parsed when config_dirty_)
    std::vector<BoundaryType> boundary_types_;

    // Per-grid-point assignment: hash -> (boundary_type_index, candidate_index)
    // Keyed by (world_x, world_y, world_z, resolution_layer)
    std::unordered_map<std::uint64_t, std::pair<int, int>> boundary_assignments_;

    // Animation state per grid point: hash -> (frame_index, elapsed_ms)
    std::unordered_map<std::uint64_t, std::pair<int, float>> animation_states_;

    // Active sprites for this frame
    std::vector<BoundarySprite> active_boundary_sprites_;

    // Parse boundary config from map_info_json
    void parse_boundary_config(const nlohmann::json& map_info);

    // Helper methods
    std::uint64_t make_grid_point_hash(int world_x, int world_y, int world_z, int layer) const;
    std::pair<int, int> assign_boundary_for_point(int world_x, int world_y, int world_z, int layer);
    SDL_FPoint sample_jitter_offset(int world_x, int world_y, int world_z, int layer, float max_jitter) const;

    struct BoundaryConfig {
        float grid_spacing_multiplier = kDefaultGridSpacingMultiplier;
        float base_size_scale = kDefaultBaseSizeScale;
        float vertical_offset = kDefaultVerticalOffset;
        float max_random_jitter = 0.0f;
    };
    static BoundaryConfig& config();
};
