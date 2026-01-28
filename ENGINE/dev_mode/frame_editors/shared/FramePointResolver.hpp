#pragma once

#include <SDL.h>

class Asset;

namespace devmode::frame_editors {

/**
 * Utility for converting stored frame point data (x/y offsets + z percentage)
 * into usable world‑space coordinates relative to an asset.
 *
 * Z is stored as a percentage of the parent asset's current height:
 *   0.0 -> on the floor, 1.0 -> top of asset's vertical bounds.
 *
 * The resolver derives anchor, base world Z, and height from the asset so
 * callers only need to supply their offset data.
 */
class FramePointResolver {
  public:
    struct WorldPoint {
        SDL_FPoint xy{0.0f, 0.0f};
        float z = 0.0f;
    };

    explicit FramePointResolver(const Asset* asset);

    // Anchor = bottom‑middle of the asset in world space.
    SDL_Point anchor_world() const;

    // Base Z offset for the asset (floor level).
    float base_world_z() const;

    // Current parent height in pixels (already scaled); 0 if unknown.
    float parent_height_px() const;

    // Convert stored percentage to world Z (adds base_world_z()).
    float to_world_z(float z_percent) const;

    // Convert a world Z value back to stored percentage. Clamped to [0, 1] if height is known.
    float to_percent(float world_z) const;

    // Combine a precomputed world offset (relative to anchor) with a stored z percent.
    WorldPoint resolve_from_anchor_offset(const SDL_FPoint& offset_from_anchor,
                                          float z_percent) const;

  private:
    const Asset* asset_ = nullptr;
};

}  // namespace devmode::frame_editors
