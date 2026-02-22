#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <vector>
#include <unordered_map>

#include "rendering/render/composite_asset_renderer.hpp"
#include "rendering/render/scaling_logic.hpp"
#include "rendering/render/dynamic_fog_system.hpp"
#include "rendering/render/dynamic_boundary_system.hpp"
#include "rendering/render/TextureLoadQueue.hpp"
#include <SDL3/SDL.h>

#include <nlohmann/json.hpp>

class Assets;
class WarpedScreenGrid;
class AssetLibrary;
namespace world { class WorldGrid; }

// Geometry batching system for reducing draw calls
class GeometryBatcher {
public:
    explicit GeometryBatcher(SDL_Renderer* renderer);

    // Add a quad to the batch with depth for sorting
    void addQuad(SDL_Texture* texture, const SDL_Vertex vertices[4], const int indices[6],
                 SDL_BlendMode blend_mode, double depth);

    // Flush all batches to the renderer
    void flush();

    // Clear all batches (call at start of frame)
    void clear();

    // Get statistics for profiling
    size_t getBatchCount() const;
    size_t getDrawCallCount() const { return draw_call_count_; }
    size_t getTotalVertices() const { return total_vertices_; }

private:
    struct QuadData {
        SDL_Vertex vertices[4];
        double depth;
    };

    struct BatchKey {
        SDL_Texture* texture;
        SDL_BlendMode blend_mode;

        bool operator==(const BatchKey& other) const {
            return texture == other.texture && blend_mode == other.blend_mode;
        }
    };

    struct BatchKeyHash {
        size_t operator()(const BatchKey& key) const {
            return reinterpret_cast<size_t>(key.texture) ^ static_cast<size_t>(key.blend_mode);
        }
    };

    struct Batch {
        SDL_Texture* texture = nullptr;
        SDL_BlendMode blend_mode = SDL_BLENDMODE_BLEND;
        std::vector<QuadData> quads;

        void reserve(size_t count) {
            quads.reserve(count);
        }
    };

    SDL_Renderer* renderer_;
    std::unordered_map<BatchKey, Batch, BatchKeyHash> batches_;
    size_t draw_call_count_ = 0;
    size_t total_vertices_ = 0;

    // Reusable buffers to avoid allocations
    std::vector<SDL_Vertex> vertex_buffer_;
    std::vector<int> index_buffer_;
};

class GridTileRenderer {
public:
    explicit GridTileRenderer(Assets* assets) : assets_(assets) {}

    void render(SDL_Renderer* renderer);

    void render(SDL_Renderer* renderer, const WarpedScreenGrid& cam, const world::WorldGrid& grid);

    void render(SDL_Renderer* renderer, const WarpedScreenGrid& cam, const world::WorldGrid& grid, GeometryBatcher* batcher);

private:
    Assets* assets_ = nullptr;
};

class SceneRenderer {
public:
    SceneRenderer(SDL_Renderer* renderer, Assets* assets, int screen_width, int screen_height, const nlohmann::json& map_manifest, const std::string& map_id);
    ~SceneRenderer();

    void invalidate_dynamic_boundary_system();

    static inline bool prerequisites_ready(SDL_Renderer* renderer, Assets* assets, std::string* reason = nullptr) {
        if (!renderer) {
            if (reason) { *reason = "SDL_Renderer pointer is null."; }
            return false;
        }
        if (!assets) {
            if (reason) { *reason = "Assets pointer is null."; }
            return false;
        }
        if (reason) { reason->clear(); }
        return true;
    }

    void render();
    SDL_Renderer* get_renderer() const;
    texture_loading::TextureLoadQueue* get_texture_load_queue() const;

    void set_movement_debug_enabled(bool enabled);
    bool movement_debug_enabled() const { return debug_auto_paths_; }
    void set_movement_debug_visible(bool visible);
    bool movement_debug_visible() const { return movement_debug_visible_; }
    void set_map_clear_color(SDL_Color color) { map_clear_color_ = color; }
    SDL_Color map_clear_color() const { return map_clear_color_; }

private:
    struct PrevalidatedTag {};

    SceneRenderer(PrevalidatedTag, SDL_Renderer* renderer, Assets* assets, int screen_width, int screen_height, const nlohmann::json& map_manifest, const std::string& map_id);
    static PrevalidatedTag require_prerequisites(SDL_Renderer* renderer, Assets* assets);

    bool ensure_sky_texture();
    void destroy_sky_texture();
    void render_sky_layer(const WarpedScreenGrid& cam, bool depth_effects_enabled);

    SDL_Renderer*  renderer_;
    Assets*        assets_;
    int            screen_width_;
    int            screen_height_;

    std::unique_ptr<GridTileRenderer> tile_renderer_;
    std::unique_ptr<GeometryBatcher> geometry_batcher_;
    std::unique_ptr<texture_loading::TextureLoadQueue> texture_load_queue_;

    bool           debugging = false;
    bool           low_quality_rendering_ = false;

    std::uint64_t frame_counter_ = 0;

    SDL_Color    map_clear_color_{0, 128, 0, 255};
    bool         debug_auto_paths_ = true;
    bool         movement_debug_visible_ = true;

    CompositeAssetRenderer composite_renderer_;
    std::unique_ptr<DynamicFogSystem> dynamic_fog_system_;
    std::unique_ptr<DynamicBoundarySystem> dynamic_boundary_system_;

    std::uint32_t depthcue_warmup_frames_ = 8;

    SDL_Texture* scene_composite_tex_ = nullptr;
    SDL_Texture* postprocess_tex_     = nullptr;
    SDL_Texture* blur_tex_            = nullptr;
    std::filesystem::path sky_texture_path_;
    double                map_radius_world_ = 0.0;
    SDL_Texture*          sky_texture_       = nullptr;
    int                   sky_texture_width_ = 0;
    int                   sky_texture_height_ = 0;
    bool                  sky_texture_failed_ = false;

};
