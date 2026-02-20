#pragma once

#include <SDL3/SDL.h>
#include <cmath>
#include <cstdint>
#include <vector>

#include "gameplay/world/grid_point.hpp"

namespace world { class WorldGrid; }
class AssetInfo;
class Assets;

// Minimal stub for tests targeting WorldGrid only.
// Provides just enough API used by world_grid.cpp
class Asset {
 public:
    ~Asset();

    SDL_Point pos{0, 0};
    int grid_resolution = 0;
    int pos_z = 0;
    AssetInfo* info = nullptr;

    int world_x() const { return pos_ ? pos_->world_x() : pos.x; }
    int world_y() const { return pos_ ? pos_->world_y() : pos.y; }
    int world_z() const { return pos_ ? pos_->world_z() : pos_z; }
    SDL_Point world_point() const { return SDL_Point{world_x(), world_y()}; }
    world::GridPoint* grid_point() const { return pos_; }

    void set_grid_point(world::GridPoint* gp) { pos_ = gp; }

    int height() const { return height_px_; }
    void set_height_px(int h) { height_px_ = h; }

    float runtime_height_px() const {
        if (!(height_px_ > 0)) {
            return 0.0f;
        }
        float remainder = (std::isfinite(remainder_scale_) && remainder_scale_ > 0.0f)
            ? remainder_scale_
            : 1.0f;
        return static_cast<float>(height_px_) * remainder;
    }
    void set_remainder_scale(float r) { remainder_scale_ = r; }

    SDL_Texture* get_current_frame() const { return current_frame_; }
    void set_current_frame(SDL_Texture* tex) { current_frame_ = tex; }

    Assets* get_assets() const { return assets_; }
    void set_assets(Assets* assets) { assets_ = assets; }

    void set_grid_id(std::uint64_t id) { grid_id_ = id; }
    std::uint64_t grid_id() const { return grid_id_; }
    void clear_grid_id();

private:
    friend class world::WorldGrid;
    world::GridPoint* pos_ = nullptr;
    std::uint64_t grid_id_ = 0;
    int height_px_ = 0;
    float remainder_scale_ = 1.0f;
    SDL_Texture* current_frame_ = nullptr;
    Assets* assets_ = nullptr;
};
