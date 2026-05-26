#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <SDL3/SDL.h>
#include <nlohmann/json.hpp>

#include "gameplay/world/grid_point.hpp"
#include "gameplay/spawn/dynamic_spawn_geometry.hpp"

class Asset;
class AssetInfo;
class AssetLibrary;
class Assets;
class Room;
class Area;

namespace world {
struct GridBounds;
}

namespace dynamic_spawn {

struct DynamicSpawnDiagnostics {
    std::size_t active = 0;
    std::size_t suspended = 0;
    std::size_t planned_cells = 0;
    std::size_t spawned = 0;
    std::size_t reused = 0;
    std::size_t suspended_this_sync = 0;
    std::size_t deleted = 0;
    std::size_t cells_processed_this_frame = 0;
    std::size_t deferred_cells_remaining = 0;
    bool movement_throttling_applied = false;
    double sync_ms = 0.0;
    double compile_ms = 0.0;
    double plan_ms = 0.0;
    double activate_ms = 0.0;
    double suspend_ms = 0.0;
};

class DynamicSpawnRuntime {
public:
    explicit DynamicSpawnRuntime(Assets& assets);
    ~DynamicSpawnRuntime();

    DynamicSpawnRuntime(const DynamicSpawnRuntime&) = delete;
    DynamicSpawnRuntime& operator=(const DynamicSpawnRuntime&) = delete;

    void compile_from_map();
    void refresh_distance_to_edge();
    void sync(const world::GridBounds& work_bounds, std::size_t max_cells_per_sync = 0, bool movement_throttled = false);
    void clear();
    void forget_asset(Asset* asset);
    std::size_t delete_for_spawn_group(const std::string& spawn_id);
    std::size_t delete_for_spawn_groups(const std::vector<std::string>& spawn_ids);

    const DynamicSpawnDiagnostics& diagnostics() const { return diagnostics_; }

private:
    enum class Mode {
        BoundaryArea = 0,
        InheritedMap = 1,
        FogBoundaryLane = 2,
    };

    struct Candidate {
        std::string asset_name;
        std::shared_ptr<AssetInfo> info;
        double weight = 0.0;
        bool is_null = false;
    };

    struct Selector {
        std::uint32_t id = 0;
        Mode mode = Mode::BoundaryArea;
        int grid_resolution = 0;
        int jitter_px = 0;
        std::string spawn_id;
        std::string display_name;
        std::uint64_t jitter_seed = 0;
        std::uint64_t candidate_seed = 0;
        std::vector<Candidate> candidates;
        std::vector<double> cumulative_weights;
        double total_weight = 0.0;
    };

    struct CellKey {
        Mode mode = Mode::BoundaryArea;
        std::uint32_t selector_id = 0;
        int grid_resolution = 0;
        int grid_x = 0;
        int grid_z = 0;

        bool operator==(const CellKey& other) const {
            return mode == other.mode &&
                   selector_id == other.selector_id &&
                   grid_resolution == other.grid_resolution &&
                   grid_x == other.grid_x &&
                   grid_z == other.grid_z;
        }
    };

    struct CellKeyHash {
        std::size_t operator()(const CellKey& key) const;
    };

    struct ChunkKey {
        int i = 0;
        int j = 0;

        bool operator==(const ChunkKey& other) const {
            return i == other.i && j == other.j;
        }
    };

    struct ChunkKeyHash {
        std::size_t operator()(const ChunkKey& key) const;
    };

    struct PlannedCell {
        CellKey key;
        ChunkKey chunk;
        int owner_anchor_world_x = 0;
        int owner_anchor_world_z = 0;
        int world_x = 0;
        int world_z = 0;
        std::string owner_name;
        std::string selector_spawn_id;
        std::string asset_name;
        std::shared_ptr<AssetInfo> info;
    };

    using PlanByChunk = std::unordered_map<ChunkKey, std::vector<PlannedCell>, ChunkKeyHash>;

    struct SelectorCompiler;
    struct SpawnPlanner;
    struct SpawnActivationRuntime;

    void clear_active_instances(bool delete_assets);
    void parse_selectors();
    void build_plan();
    void build_plan_into(PlanByChunk& plan, int threshold_px) const;
    void add_selector_cells(const Selector& selector,
                            const geometry::AreaGeometry& geometry,
                            int threshold_px,
                            PlanByChunk& plan) const;
    void add_selector_cells_in_distance_band(const Selector& selector,
                                             const geometry::AreaGeometry& geometry,
                                             int previous_threshold_px,
                                             int next_threshold_px,
                                             PlanByChunk& plan,
                                             std::unordered_set<CellKey, CellKeyHash>& known_keys) const;
    void add_planned_cell(const Selector& selector,
                          const CellKey& key,
                          int owner_anchor_world_x,
                          int owner_anchor_world_z,
                          const std::string& owner_name,
                          PlanByChunk& plan) const;
    bool resolve_cell_owner(const Selector& selector,
                            const geometry::AreaGeometry& geometry,
                            SDL_Point owner_anchor,
                            int threshold_px,
                            std::string& owner_name) const;
    Asset* activate_cell(const PlannedCell& cell);
    std::unique_ptr<Asset> create_asset_for_cell(const PlannedCell& cell) const;
    void suspend_cell(const CellKey& key, Asset* asset);
    void suspend_outside_keep_chunks(const std::unordered_set<ChunkKey, ChunkKeyHash>& keep_chunks);
    void activate_chunk(const ChunkKey& chunk);
    std::size_t activate_chunk_budgeted(const ChunkKey& chunk, std::size_t& remaining_budget);
    ChunkKey chunk_key_for_world(int world_x, int world_z) const;
    std::unordered_set<ChunkKey, ChunkKeyHash> chunk_keys_for_bounds(const world::GridBounds& bounds) const;
    world::GridBounds expanded_bounds(const world::GridBounds& bounds, int margin_px) const;
    bool room_contains_dynamic_area(const Room* room, SDL_Point point) const;
    Room* inherited_room_for_point(SDL_Point point) const;
    const Candidate* pick_candidate(const Selector& selector, const CellKey& key) const;
    SDL_Point jittered_world_point(const Selector& selector, const CellKey& key, SDL_Point base_point) const;
    bool info_allowed(const AssetInfo* info, Mode mode) const;
    int max_spawn_from_room_px() const;
    int fog_render_boundary_spacing_px() const;
    int preload_margin_px() const;
    int despawn_margin_px() const;

    Assets& assets_;
    std::vector<Selector> selectors_;
    std::unordered_map<std::uint64_t, const Selector*> selector_index_;
    PlanByChunk cells_by_chunk_;
    std::unordered_map<CellKey, Asset*, CellKeyHash> active_;
    std::unordered_map<CellKey, std::unique_ptr<Asset>, CellKeyHash> suspended_;
    std::unordered_map<Asset*, CellKey> asset_to_key_;
    std::unordered_set<ChunkKey, ChunkKeyHash> active_chunks_;
    std::vector<ChunkKey> pending_activation_chunks_;
    std::unordered_set<ChunkKey, ChunkKeyHash> pending_activation_set_;
    std::unordered_map<ChunkKey, std::size_t, ChunkKeyHash> chunk_activation_cursor_;
    DynamicSpawnDiagnostics diagnostics_{};
    std::uint32_t next_selector_id_ = 1;
    int planned_max_spawn_from_room_px_ = 128;
    std::uint64_t selector_config_hash_ = 0;
    std::uint64_t geometry_hash_ = 0;
    std::uint64_t plan_hash_ = 0;
};

} // namespace dynamic_spawn
