#pragma once

#include <SDL.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <unordered_map>
#include <vector>

#include "utils/light_source.hpp"
#include "utils/grid.hpp"
#include "world/chunk_manager.hpp"
#include "world/grid_point.hpp"

class Asset;
class WarpedScreenGrid;

namespace world {

struct GridKey {
    int x = 0;
    int y = 0;
    int z = 0;
    int layer = 0;

    bool operator==(const GridKey& other) const noexcept {
        return x == other.x && y == other.y && z == other.z && layer == other.layer;
    }

    bool operator!=(const GridKey& other) const noexcept {
        return !(*this == other);
    }
};

struct GridKeyHash {
    std::size_t operator()(const GridKey& key) const noexcept {
        std::size_t h1 = std::hash<int>{}(key.x);
        std::size_t h2 = std::hash<int>{}(key.y);
        std::size_t h3 = std::hash<int>{}(key.z);
        std::size_t h4 = std::hash<int>{}(key.layer);
        return ((h1 ^ (h2 << 1)) >> 1) ^ (h3 << 1) ^ (h4 << 3);
    }
};

class WorldGrid {
public:
    struct LightInstance {
        const GridPoint* point = nullptr;
        Asset* asset = nullptr;
        const LightSource* source = nullptr;
    };

    struct RegionMetrics {
        std::uint32_t nodes_visited = 0;
        std::uint32_t branches_skipped = 0;
    };

    // Ownership/lifetime: WorldGrid is the Map Grid owner for all GridPoints across
    // (x, y, world_z, resolution_layer). Nodes are stored here for the life of the
    // grid and are not transferred to Screen Grid; Screen Grid only holds non-owning
    // references during frame rebuilds. Legacy chunked rendering (ChunkManager) and
    // loaders remain intact during migration; avoid adding new permanent 2D-only paths.
    WorldGrid() : WorldGrid(SDL_Point{0, 0}, 0) {}
    WorldGrid(SDL_Point origin, int r_chunk);

    void set_chunk_resolution(int r);
    void set_grid_resolution(int r);
    int  grid_resolution() const;
    int  chunk_resolution() const { return r_chunk_; }
    SDL_Point origin() const { return origin_; }
    void set_origin(SDL_Point origin);

    Asset* create_asset_at_point(std::unique_ptr<Asset> a, int world_z = 0, int resolution_layer = -1);
    Asset* create_asset_at_point(Asset* a, int world_z = 0, int resolution_layer = -1);
    Asset* register_asset(std::unique_ptr<Asset> a, int world_z = 0, int resolution_layer = -1);
    Asset* register_asset(Asset* a, int world_z = 0, int resolution_layer = -1);
    Chunk* ensure_chunk_from_world(SDL_Point world_px);
    Chunk* chunk_from_world(SDL_Point world_px) const;
    Chunk* get_or_create_chunk_ij(int i, int j);
    std::vector<Chunk*> all_chunks() const {
        const auto& storage = chunks_.storage();
        std::vector<Chunk*> result;
        result.reserve(storage.size());
        for (const auto& chunk : storage) {
            if (chunk) {
                result.push_back(chunk.get());
            }
        }
        return result;
    }

    Asset* move_asset_to_point(Asset* a, SDL_Point old_pos, SDL_Point new_pos, int world_z = 0, int resolution_layer = -1);
    Asset* move_asset(Asset* a, SDL_Point old_pos, SDL_Point new_pos, int world_z = 0, int resolution_layer = -1);

    Asset* remove_asset(Asset* a);
    void unregister_asset(Asset* a);
    void rebuild_chunks();

    void update_active_chunks(const SDL_Rect& camera_world, int margin_px);

    const std::vector<Chunk*>& active_chunks() const;

    ChunkManager& chunks();
    const ChunkManager& chunks() const;
    std::vector<Asset*> all_assets() const;

    SDL_Point grid_index_from_world(SDL_Point world) const;
    GridKey grid_key_from_world(SDL_Point world, int world_z = 0, int layer = -1) const;
    const std::unordered_map<GridId, GridPoint>& points() const { return points_; }
    std::unordered_map<GridId, GridPoint>& points() { return points_; }
    GridPoint* point_for_id(GridId id);
    const GridPoint* point_for_id(GridId id) const;
    GridPoint* point_for_asset(const Asset* asset);
    const GridPoint* point_for_asset(const Asset* asset) const;
    GridPoint& ensure_child(GridPoint& parent, SDL_Point grid_index, SDL_Point chunk_index, Chunk* owning_chunk = nullptr);
    GridPoint* find_grid_point(const GridKey& key);
    const GridPoint* find_grid_point(const GridKey& key) const;
    GridPoint* find_grid_point_strict(const GridKey& key);
    const GridPoint* find_grid_point_strict(const GridKey& key) const;
    GridPoint& find_or_create_grid_point(const GridKey& key, Chunk* owning_chunk = nullptr, GridPoint* parent = nullptr);
    GridKey grid_key_from_legacy(SDL_Point grid_index, int world_z = 0, int layer = -1) const;
    GridPoint& ensure_child(GridPoint& parent, GridPoint::ChildDirection dir, const GridKey& child_key, Chunk* owning_chunk = nullptr);
    void attach_asset_to_hierarchy(GridPoint& point);
    void detach_asset_from_hierarchy(GridPoint& point);
    static std::size_t hash_key(const GridKey& key);
    void debug_validate_keys_and_masks() const;
    std::vector<GridPoint*> query_region(const SDL_FRect& world_bounds,
                                         int min_layer,
                                         int max_layer,
                                         int min_world_z,
                                         int max_world_z,
                                         bool skip_inactive_branches,
                                         bool include_empty_nodes,
                                         RegionMetrics* metrics = nullptr);
    std::vector<const GridPoint*> query_region(const SDL_FRect& world_bounds,
                                               int min_layer,
                                               int max_layer,
                                               int min_world_z,
                                               int max_world_z,
                                               bool skip_inactive_branches,
                                               bool include_empty_nodes,
                                               RegionMetrics* metrics = nullptr) const;
    std::vector<LightInstance> query_lights(const SDL_FRect& world_bounds,
                                            int min_world_z,
                                            int max_world_z,
                                            bool skip_inactive_branches);
    std::vector<LightInstance> query_lights(const SDL_FRect& world_bounds,
                                            int min_world_z,
                                            int max_world_z,
                                            bool skip_inactive_branches) const;
    int max_resolution_layers() const;

private:
    void remove_from_chunk(Asset* a, Chunk* c);
    void invalidate_active_cache();
    GridId make_point_id(int i, int j) const;
    void remove_asset_from_point(Asset* a, GridPoint& point);
    std::unique_ptr<Asset> detach_asset_from_grid_point(Asset* a, GridPoint& point, bool clear_mapping);
    void attach_asset_to_grid_point(std::unique_ptr<Asset> owned, Asset* raw, GridPoint& point);
    GridPoint& ensure_point(SDL_Point grid_index, SDL_Point chunk_index, Chunk* owning_chunk, GridPoint* parent = nullptr, int world_z = 0, int resolution_layer = -1);
    void bind_asset_to_point(Asset* a, GridPoint& point);
    void propagate_branch_active(GridPoint* node);
    void propagate_branch_inactive(GridPoint* node);
    void prune_empty_points();
    std::unique_ptr<Asset> extract_from_point(Asset* a, GridPoint& point);
    int default_resolution_layer() const;
    int grid_spacing_for_layer(int layer) const;
    int distance_for_layer(int layer) const;
    static int power_of_three(int exponent);

    SDL_Point origin_{0, 0};
    int       r_chunk_ = 0;
    int       grid_resolution_ = 0;
    int       max_resolution_layers_ = 0;

    ChunkManager chunks_;
    std::unordered_map<Asset*, Chunk*> residency_;

    bool     has_cached_camera_rect_ = false;
    SDL_Rect last_expanded_camera_{0, 0, 0, 0};
    int      last_margin_px_         = -1;
    int      last_chunk_resolution_  = -1;

    std::unordered_map<GridId, GridPoint> points_;
    std::unordered_map<Asset*, GridKey> asset_to_key_;
    std::unordered_map<GridKey, GridId, GridKeyHash> key_to_id_;
    std::vector<GridId> roots_;

    void add_root_id(GridId id);
    void remove_root_id(GridId id);
};

}
