#pragma once

#include <SDL3/SDL.h>

#include <cstdint>
#include <limits>
#include <unordered_map>
#include <vector>

#include "rendering/render/terrain_runtime_state.hpp"

namespace world {
struct GridKey;
class WorldGrid;
}
class Room;
class Area;

// Deterministic terrain height sampler with region-aware flattening and per-frame caching.
class TerrainField {
public:
    TerrainField();

    // Prepare caches for the current frame and refresh region indices when rooms change.
    void begin_frame(std::uint64_t frame_id,
                     const TerrainRuntimeState& runtime_state,
                     const std::vector<Room*>& rooms);

    // Update runtime state immediately (clears caches/indices as needed).
    void set_runtime_state(const TerrainRuntimeState& runtime_state,
                           const std::vector<Room*>& rooms);

    // Sample elevation (>=0) for a world grid key at the active grid resolution.
    // Returns 0 when disabled or inside a room/trail region.
    float sample_elevation(const world::GridKey& key,
                           const world::WorldGrid& grid,
                           const std::vector<Room*>& rooms,
                           const TerrainRuntimeState& runtime_state,
                           std::uint64_t frame_id);

    std::uint64_t runtime_revision() const { return runtime_revision_; }

private:
    struct RegionArea {
        const Area* area = nullptr;
        const Room* owner = nullptr;
        SDL_Rect bounds{0, 0, 0, 0};
        bool is_trail = false;
    };

    struct CellKey {
        int x = 0;
        int y = 0;
        bool operator==(const CellKey& other) const noexcept {
            return x == other.x && y == other.y;
        }
    };

    struct CellKeyHash {
        std::size_t operator()(const CellKey& key) const noexcept;
    };

    struct RegionQueryResult {
        bool inside = false;
        float distance = std::numeric_limits<float>::infinity();
    };

    void ensure_region_index(const std::vector<Room*>& rooms);
    void add_indexed_area(const Area* area, const Room* owner, bool is_trail);
    RegionQueryResult query_region(const SDL_Point& pt, float falloff_radius);
    float distance_to_area(const SDL_Point& pt, const Area* area, bool* inside) const;

    float edge_falloff(float distance, const TerrainSettings& settings) const;
    float fractal_noise(float nx, float ny, const TerrainSettings& settings, std::uint32_t base_seed) const;
    float value_noise(float x, float y, std::uint32_t seed) const;
    float hash01(int x, int y, std::uint32_t seed) const;

    void reset_cache_if_needed(std::uint64_t frame_id);
    void sync_runtime_state(const TerrainRuntimeState& runtime_state,
                            const std::vector<Room*>& rooms);

    static constexpr int kRegionIndexCellSize = 512;

    std::unordered_map<CellKey, std::vector<RegionArea>, CellKeyHash> region_index_;
    std::unordered_map<std::uint64_t, float> frame_cache_;
    std::size_t region_index_hash_ = 0;
    std::uint64_t runtime_revision_ = 0;
    std::uint64_t cached_revision_ = 0;
    std::uint64_t last_frame_id_ = 0;
    std::uint64_t base_seed_ = 0;
    TerrainSettings cached_settings_{};
    bool has_runtime_state_ = false;
};
