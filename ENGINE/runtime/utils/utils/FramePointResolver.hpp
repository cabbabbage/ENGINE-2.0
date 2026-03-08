#pragma once
#include "gameplay/world/grid_point.hpp"
#include "gameplay/world/world_grid.hpp"
#include <SDL3/SDL.h>

class Asset;
class Assets;

namespace devmode::frame_editors {

/**
 * Utility for converting stored frame point data (x/y offsets + z percentage)
 * into usable world-space coordinates relative to an asset.
 *
 * Z is stored as a percentage of the parent asset's current height:
 *   0.0 -> on the floor, 1.0 -> top of asset's vertical bounds.
 *
 * The resolver derives anchor, base world Z, and height from the asset so
 * callers only need to supply their offset data.
 */
class FramePointResolver {
  public:


    struct Displacement_percent_vals {
        // Percent offsets relative to the parent's height:
        float right_percent = 0.0f;
        float height_percent = 0.0f;
        float depth_percent = 0.0f;
    };

    explicit FramePointResolver(const Asset* asset) : asset_(asset) {}

    // Anchor = bottom-middle of the asset in world space.
    SDL_Point anchor_world() const;

    // Get parent asset's height in pixels (for percent calculations)
    float parent_height_px() const;

    // Get the base world height (Y axis) of the asset
    float base_world_height() const;
    float base_world_z() const { return base_world_height(); }

    // Convert world height coordinate to percent of parent height
    // 0.0 = on the floor, 1.0 = top of asset's vertical bounds
    float to_percent(float world_height) const;
    float to_percent_height(float world_height) const;

    // Convert percent of parent height to world height coordinate
    float to_world_height(float height_percent) const;
    float to_world_z(float height_percent) const { return to_world_height(height_percent); }

    // Convert world X/Y coordinate to percent of parent height
    float to_percent_xy(float world_coord) const;

    // Convert percent of parent height to world X/Y coordinate
    float to_world_xy(float coord_percent) const;

    Displacement_percent_vals to_percent_displacement(int x, int y, int z, const Asset* source) const;

    world::GridPoint* to_grid_point_displacement(Displacement_percent_vals vals, const Asset* source_asset) const;



    private:
    const Asset* asset_ = nullptr;



    

};

}  // namespace devmode::frame_editors
