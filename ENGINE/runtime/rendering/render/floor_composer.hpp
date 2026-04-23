#pragma once

#include <unordered_map>
#include <vector>

#include <SDL3/SDL.h>

#include "rendering/render/render_pipeline_types.hpp"

class Assets;
class GeometryBatcher;
class WarpedScreenGrid;
namespace world { class WorldGrid; }

class GridTileRenderer {
public:
    explicit GridTileRenderer(Assets* assets);

    void render(SDL_Renderer* renderer);
    void render(SDL_Renderer* renderer, const WarpedScreenGrid& cam, const world::WorldGrid& grid);
    void render(SDL_Renderer* renderer,
                const WarpedScreenGrid& cam,
                const world::WorldGrid& grid,
                GeometryBatcher* batcher);

    void invalidate_texture_cache();

private:
    bool fetch_texture_size(SDL_Texture* texture, SDL_FPoint& out_size);

    Assets* assets_ = nullptr;
    std::unordered_map<SDL_Texture*, SDL_FPoint> texture_size_cache_;
};

class FloorComposer {
public:
    FloorComposer(SDL_Renderer* renderer, Assets* assets);
    ~FloorComposer();

    FloorComposer(const FloorComposer&) = delete;
    FloorComposer& operator=(const FloorComposer&) = delete;

    void set_output_dimensions(int screen_width, int screen_height);

    SDL_Texture* compose(const WarpedScreenGrid& cam,
                         const world::WorldGrid& grid,
                         const std::vector<render_pipeline::RuntimeLight>& runtime_lights,
                         bool runtime_lighting_enabled,
                         double max_cull_depth,
                         SDL_Color clear_color,
                         bool render_floor_tiles);
    SDL_Texture* floor_dark_mask_texture() const { return has_floor_dark_mask_ ? floor_light_mask_texture_ : nullptr; }

private:
    bool ensure_sized_target(SDL_Texture*& texture);
    SDL_Texture* ensure_floor_light_falloff_texture();
    float compute_horizon_screen_y(const WarpedScreenGrid& cam) const;
    void clear_target(SDL_Texture* texture);
    void destroy_owned_textures();

    SDL_Renderer* renderer_ = nullptr;
    GridTileRenderer tile_renderer_;
    int screen_width_ = 1;
    int screen_height_ = 1;

    SDL_Texture* floor_base_texture_ = nullptr;
    SDL_Texture* floor_light_mask_texture_ = nullptr;
    SDL_Texture* floor_light_falloff_texture_ = nullptr;
    bool has_floor_dark_mask_ = false;
};
