#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "utils/grid.hpp"
#include "gameplay/world/chunk_manager.hpp"
#include "gameplay/world/grid_point.hpp"

class Asset;
class WarpedScreenGrid;

namespace world {

class WorldGrid {
public:
    struct RegionMetrics {
        std::uint32_t nodes_visited = 0;
        std::uint32_t branches_skipped = 0;
    };

    // Ownership/lifetime: WorldGrid is the Map Grid owner for all GridPoints across
    // (x, y, world_z, resolution_layer). Nodes are stored here for the life of the
    // grid and are not transferred to Screen Grid; Screen Grid only holds non-owning
    // references during frame rebuilds. Legacy chunked rendering (ChunkManager) and
    // loaders remain intact during migration; avoid adding new permanent 2D-only paths.
    WorldGrid() : WorldGrid(GridPoint::make_virtual(0, 0, 0, 0), 0) {}
    WorldGrid(const GridPoint& origin, int r_chunk);

    void set_grid_resolution(int r);
    int  grid_resolution() const;
    GridPoint origin() const { return origin_; }
    void set_origin(const GridPoint& origin);

    Asset* create_asset_at_point(std::unique_ptr<Asset> a, int world_z = 0, int resolution_layer = -1);
    Asset* register_asset(std::unique_ptr<Asset> a, int world_z = 0, int resolution_layer = -1);
    Chunk* ensure_chunk_from_world(const GridPoint& world_px);
    Chunk* chunk_from_world(const GridPoint& world_px) const;
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

    Asset* move_asset(Asset* a, const GridPoint& old_pos, const GridPoint& new_pos);

    Asset* remove_asset(Asset* a);
    void unregister_asset(Asset* a);
    void rebuild_chunks();
    std::size_t flush_deferred_empty_points(std::size_t max_points = std::numeric_limits<std::size_t>::max());
    bool is_occupied_at_xz(int world_x, int world_z, int resolution_layer) const;
    std::uint32_t empty_point_skips() const { return empty_point_skips_; }
    std::uint32_t empty_point_invalidations() const { return empty_point_invalidations_; }

    void update_active_chunks(const GridBounds& camera_world, int margin_px);

    const std::vector<Chunk*>& active_chunks() const;

    ChunkManager& chunks();
    const ChunkManager& chunks() const;
    std::vector<Asset*> all_assets() const;

    GridKey grid_key_from_world(const GridPoint& world_point, int layer = -1) const;
    const std::unordered_map<GridId, GridPoint>& points() const { return points_; }
    std::unordered_map<GridId, GridPoint>& points() { return points_; }
    GridPoint* point_for_id(GridId id);
    const GridPoint* point_for_id(GridId id) const;
    GridPoint* point_for_asset(const Asset* asset);
    const GridPoint* point_for_asset(const Asset* asset) const;

    // Detach/attach helpers for suspending and reactivating assets without destroying them.
    std::unique_ptr<Asset> extract_asset(Asset* a);
    Asset* attach_asset(std::unique_ptr<Asset> a, int world_z = 0, int resolution_layer = -1);

    GridPoint* find_grid_point(const GridKey& key);
    const GridPoint* find_grid_point(const GridKey& key) const;
    GridPoint* find_grid_point_strict(const GridKey& key);
    const GridPoint* find_grid_point_strict(const GridKey& key) const;
    GridPoint& find_or_create_grid_point(const GridKey& key, Chunk* owning_chunk = nullptr, GridPoint* parent = nullptr);
    GridPoint& ensure_child(GridPoint& parent, GridPoint::ChildDirection dir, const GridKey& child_key, Chunk* owning_chunk = nullptr);
    void attach_asset_to_hierarchy(GridPoint& point);
    void detach_asset_from_hierarchy(GridPoint& point);
    static std::size_t hash_key(const GridKey& key);
    void debug_validate_keys_and_masks() const;
    void invalidate_projection_cache();
    std::vector<GridPoint*> query_region(const GridBounds& bounds,
                                         int min_layer,
                                         int max_layer,
                                         int min_world_depth,
                                         int max_world_depth,
                                         bool skip_inactive_branches,
                                         bool include_empty_nodes,
                                         RegionMetrics* metrics = nullptr);
    std::vector<const GridPoint*> query_region(const GridBounds& bounds,
                                               int min_layer,
                                               int max_layer,
                                               int min_world_depth,
                                               int max_world_depth,
                                               bool skip_inactive_branches,
                                               bool include_empty_nodes,
                                               RegionMetrics* metrics = nullptr) const;
    int max_resolution_layers() const;
    int default_resolution_layer() const;
    int grid_spacing_for_layer(int layer) const;

private:
    struct Residency {
        Chunk*      chunk = nullptr;
        std::size_t index_in_chunk_assets = 0;
    };

    void remove_from_chunk(Asset* a, Chunk* c);
    void add_to_chunk(Asset* a, Chunk& c);
    void debug_assert_residency_integrity(Asset* a) const;
    void invalidate_active_cache();
    GridId make_point_id(int grid_x, int grid_depth, int world_y, int resolution_layer, std::uint32_t salt = 0) const;
    void remove_asset_from_point(Asset* a, GridPoint& point);
    std::unique_ptr<Asset> detach_asset_from_grid_point(Asset* a, GridPoint& point, bool clear_mapping);
    void attach_asset_to_grid_point(std::unique_ptr<Asset> owned, GridPoint& point);
    GridPoint& ensure_point(GridCoord grid_index, GridCoord chunk_index, Chunk* owning_chunk, GridPoint* parent = nullptr, int world_y = 0, int resolution_layer = -1);
    void bind_asset_to_point(Asset* a, GridPoint& point);
    void mark_point_for_deferred_prune(GridPoint* point);
    bool point_can_resolve_empty(const GridPoint& point) const;
    void update_sticky_empty_state(GridPoint& point);
    void invalidate_sticky_empty_state(GridPoint& point);
    void propagate_branch_active(GridPoint* node);
    void propagate_branch_inactive(GridPoint* node);
    void prune_empty_points();
    struct OccupancyXZKey {
        int world_x = 0;
        int world_z = 0;
        int layer = 0;

        bool operator==(const OccupancyXZKey& other) const noexcept {
            return world_x == other.world_x &&
                   world_z == other.world_z &&
                   layer == other.layer;
        }
    };
    struct OccupancyXZKeyHash {
        std::size_t operator()(const OccupancyXZKey& key) const noexcept {
            std::size_t seed = std::hash<int>{}(key.world_x);
            auto mix = [&seed](std::size_t value) {
                seed ^= value + 0x9e3779b9u + (seed << 6) + (seed >> 2);
            };
            mix(std::hash<int>{}(key.world_z));
            mix(std::hash<int>{}(key.layer));
            return seed;
        }
    };
    static OccupancyXZKey occupancy_key_for_point(const GridPoint& point);
    void increment_xz_occupancy(const GridPoint& point);
    void decrement_xz_occupancy(const GridPoint& point);
    std::unique_ptr<Asset> extract_from_point(Asset* a, GridPoint& point);
    int distance_for_layer(int layer) const;
    static int power_of_three(int exponent);
    GridCoord grid_index_from_world(const GridPoint& world_point, int layer_override = -1) const;

    GridPoint origin_ = GridPoint::make_virtual(0, 0, 0, 0);
    int       r_chunk_ = 0;
    int       max_resolution_layers_ = 0;

    ChunkManager chunks_;
    std::unordered_map<Asset*, Residency> residency_;

    bool     has_cached_camera_rect_ = false;
    GridBounds last_expanded_camera_{};
    int      last_margin_px_         = -1;
    int      last_chunk_resolution_  = -1;

    std::unordered_map<GridId, GridPoint> points_;
    std::unordered_map<Asset*, GridKey> asset_to_key_;
    std::unordered_map<GridKey, GridId, GridKeyHash> key_to_id_;
    std::vector<GridId> roots_;
    std::unordered_set<GridId> deferred_empty_point_ids_;
    std::uint64_t empty_state_revision_ = 1;
    std::uint32_t empty_point_skips_ = 0;
    std::uint32_t empty_point_invalidations_ = 0;
    std::unordered_map<OccupancyXZKey, std::uint32_t, OccupancyXZKeyHash> occupancy_xz_counts_;

    void add_root_id(GridId id);
    void remove_root_id(GridId id);
};

}
