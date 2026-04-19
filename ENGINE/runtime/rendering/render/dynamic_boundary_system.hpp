#pragma once

#include <SDL3/SDL.h>
#include <nlohmann/json.hpp>

#include "rendering/render/scaling_logic.hpp"
#include "gameplay/spawn/runtime_candidates.hpp"

#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class Room;
class Area;
class WarpedScreenGrid;
class AssetLibrary;
class Assets;
class AssetInfo;
namespace world {
class WorldGrid;
}

class DynamicBoundarySystem {
public:
    struct BoundaryFrameVariant {
        SDL_Texture* texture = nullptr;
        int width = 0;
        int height = 0;
    };

    struct BoundaryFrame {
        std::vector<BoundaryFrameVariant> variants;
        float duration_ms = 41.67f;  // ~24 fps default
    };

    struct BoundarySprite {
        SDL_Texture* texture = nullptr;
        std::string  spawn_id;
        std::string  asset_name;
        SDL_FPoint   world_pos{0.0f, 0.0f};
        SDL_FPoint   screen_pos{0.0f, 0.0f};
        float        asset_scale = 1.0f;
        float        world_width = 0.0f;
        float        world_height = 0.0f;
        int          world_z = 0;
        int          texture_w = 0;
        int          texture_h = 0;

        int   boundary_type_index = 0;
        int   candidate_index = 0;
        int   current_frame_index = 0;
        float frame_elapsed_ms = 0.0f;
        int   total_frames = 1;
        float frame_duration_ms = 41.67f;
    };

    struct BoundaryAssetRuntime {
        std::vector<BoundaryFrame> frames;
        std::shared_ptr<AssetInfo> info;
        bool is_null = false;
        render_pipeline::ScalingLogic::HysteresisState hysteresis_state;
    };

    struct BoundaryType {
        std::string                 spawn_id;
        std::string                 display_name;
        int                         grid_resolution = 5;
        int                         jitter = 0;
        vibble::spawn::RuntimeCandidates candidates;
        std::unordered_map<std::string, BoundaryAssetRuntime> candidate_runtime_by_asset;
        std::unordered_set<int> room_trail_excluded_candidate_indices;
        bool room_trail_exclusions_ready = false;
    };

    struct BoundaryKey {
        int group = 0;
        int region_domain = 0;  // 0 = boundary, 1 = room/trail
        int resolution_layer = 0;
        int grid_x = 0;
        int grid_y = 0;
        int world_z = 0;

        bool operator==(const BoundaryKey& other) const noexcept {
            return group == other.group &&
                   region_domain == other.region_domain &&
                   resolution_layer == other.resolution_layer &&
                   grid_x == other.grid_x &&
                   grid_y == other.grid_y &&
                   world_z == other.world_z;
        }
    };

    struct BoundaryKeyHash {
        std::size_t operator()(const BoundaryKey& key) const noexcept;
    };

    struct BoundaryAssignment {
        int candidate_entry_index = -1;
        std::string resolved_asset_name;
        bool is_null = true;
        float size_variation_sample = 0.0f;
    };

    struct FrameState {
        int   frame_index = 0;
        float elapsed_ms = 0.0f;
    };

    DynamicBoundarySystem();
    ~DynamicBoundarySystem();

    bool initialize(SDL_Renderer* renderer, AssetLibrary* asset_library);
    void update(const WarpedScreenGrid& cam, world::WorldGrid& grid, Assets* assets, float delta_ms);

    const std::vector<BoundarySprite>& get_boundary_sprites() const { return active_boundary_sprites_; }

    static constexpr float kDefaultGridSpacingMultiplier = 1.0f;
    static constexpr float kDefaultBaseSizeScale        = 1.0f;
    static constexpr float kDefaultVerticalOffset       = 0.0f;
    static constexpr int   kDefaultResolutionLayer      = 5;

    static void  set_grid_spacing_multiplier(float multiplier);
    static float grid_spacing_multiplier();
    static void  set_base_size_scale(float scale);
    static float base_size_scale();
    static void  set_vertical_offset(float offset);
    static float vertical_offset();
    static void  set_max_random_jitter(float jitter);
    static float max_random_jitter();
    static float sample_size_variation_from_hash(std::uint64_t key_hash);
    static float compute_effective_base_scale(const AssetInfo& info, float size_variation_sample);

    bool is_initialized() const { return initialized_; }
    void invalidate_config();

private:
    bool initialized_ = false;
    bool config_dirty_ = true;
    SDL_Renderer* renderer_ = nullptr;
    AssetLibrary* asset_library_ = nullptr;

    std::uint64_t  config_revision_ = 0;
    std::uint64_t  map_boundary_revision_ = 0;
    std::size_t    map_boundary_signature_ = 0;
    std::uint64_t  warning_revision_ = 0;
    std::uint64_t  boundary_regen_seed_ = 0;

    std::vector<BoundaryType> boundary_types_;
    std::unordered_map<std::string, std::shared_ptr<AssetInfo>> room_trail_catalog_storage_;
    std::size_t room_trail_catalog_signature_ = std::numeric_limits<std::size_t>::max();
    std::unordered_map<BoundaryKey, BoundaryAssignment, BoundaryKeyHash> boundary_assignments_;
    std::unordered_map<BoundaryKey, FrameState, BoundaryKeyHash> animation_states_;
    std::vector<BoundarySprite> active_boundary_sprites_;
    struct StaticCellAssignment {
        BoundaryKey key;
        BoundaryAssignment assignment;
        int boundary_type_index = -1;
        SDL_FPoint world_pos{0.0f, 0.0f};
        int world_z = 0;
    };
    struct StaticAssignmentFingerprint {
        std::uint64_t config_revision = 0;
        std::size_t rooms_hash = 0;
        int origin_x = 0;
        int origin_z = 0;
        int origin_layer = 0;
        int grid_resolution = 0;
        int min_grid_x = 0;
        int max_grid_x = 0;
        int min_grid_y = 0;
        int max_grid_y = 0;
        float spacing_multiplier = kDefaultGridSpacingMultiplier;

        bool operator==(const StaticAssignmentFingerprint& other) const noexcept {
            return config_revision == other.config_revision &&
                   rooms_hash == other.rooms_hash &&
                   origin_x == other.origin_x &&
                   origin_z == other.origin_z &&
                   origin_layer == other.origin_layer &&
                   grid_resolution == other.grid_resolution &&
                   min_grid_x == other.min_grid_x &&
                   max_grid_x == other.max_grid_x &&
                   min_grid_y == other.min_grid_y &&
                   max_grid_y == other.max_grid_y &&
                   spacing_multiplier == other.spacing_multiplier;
        }
        bool operator!=(const StaticAssignmentFingerprint& other) const noexcept {
            return !(*this == other);
        }
    };
    std::vector<StaticCellAssignment> static_assignments_;
    StaticAssignmentFingerprint static_assignment_fingerprint_{};
    std::unordered_set<int> dense_type_warnings_;
    struct RegionCacheKey {
        int x = 0;
        int y = 0;
        bool operator==(const RegionCacheKey& other) const noexcept {
            return x == other.x && y == other.y;
        }
    };
    struct RegionCacheKeyHash {
        std::size_t operator()(const RegionCacheKey& key) const noexcept {
            const std::uint64_t a = static_cast<std::uint32_t>(key.x);
            const std::uint64_t b = static_cast<std::uint32_t>(key.y);
            std::uint64_t combined = (a << 32) | b;
            combined ^= combined >> 33;
            combined *= 0xff51afd7ed558ccdULL;
            combined ^= combined >> 33;
            return static_cast<std::size_t>(combined);
        }
    };
    enum class RegionKind : int {
        Boundary = 0,
        Room,
        Trail
    };

    struct RegionCacheEntry {
        RegionKind region_kind = RegionKind::Boundary;
        const Room* owner = nullptr;
        bool blocked = false;
    };
    struct RegionCacheFingerprint {
        std::uint64_t config_revision = 0;
        std::size_t map_info_hash = 0;
        std::size_t rooms_hash = 0;
        int origin_x = 0;
        int origin_z = 0;
        int origin_layer = 0;
        int grid_resolution = 0;
        float spacing_multiplier = kDefaultGridSpacingMultiplier;

        bool operator==(const RegionCacheFingerprint& other) const noexcept {
            return config_revision == other.config_revision &&
                   map_info_hash == other.map_info_hash &&
                   rooms_hash == other.rooms_hash &&
                   origin_x == other.origin_x &&
                   origin_z == other.origin_z &&
                   origin_layer == other.origin_layer &&
                   grid_resolution == other.grid_resolution &&
                   spacing_multiplier == other.spacing_multiplier;
        }
        bool operator!=(const RegionCacheFingerprint& other) const noexcept {
            return !(*this == other);
        }
    };
    std::unordered_map<RegionCacheKey, RegionCacheEntry, RegionCacheKeyHash> region_cache_;
    struct IndexedArea {
        const Area* area = nullptr;
        RegionKind region_kind = RegionKind::Boundary;
        const Room* owner = nullptr;
    };
    static constexpr int kRegionIndexCellSize = 512;
    std::unordered_map<RegionCacheKey, std::vector<IndexedArea>, RegionCacheKeyHash> region_area_index_;
    RegionCacheFingerprint region_cache_fingerprint_;
    mutable std::size_t cached_rooms_generation_ = std::numeric_limits<std::size_t>::max();
    mutable std::size_t cached_rooms_topology_hash_ = 0;

    void parse_boundary_config(const nlohmann::json& boundary_data);
    void build_candidate_frames(BoundaryAssetRuntime& candidate_runtime,
                                const std::string& asset_name);
    BoundaryAssetRuntime* ensure_candidate_runtime(BoundaryType& type,
                                                   const std::string& asset_name);
    bool refresh_boundary_config_revision(const nlohmann::json& map_info);
    void clear_runtime_caches();

    BoundaryKey  make_key(int group_idx,
                          int region_domain,
                          int resolution_layer,
                          int grid_x,
                          int grid_y,
                          int world_z) const;
    std::uint64_t hash_key(const BoundaryKey& key) const;
    const BoundaryAssignment& select_candidate_for_key(
        const BoundaryKey& key,
        BoundaryType& btype,
        const vibble::spawn::RuntimeCandidates::AssetCatalogView& catalog,
        const std::unordered_set<int>* excluded_entry_indices = nullptr);
    SDL_FPoint sample_jitter_offset(const BoundaryKey& key, float max_jitter) const;
    void ensure_region_cache_valid(const world::WorldGrid& grid,
                                   const std::vector<Room*>& rooms,
                                   std::size_t rooms_hash,
                                   float spacing_multiplier);
    const RegionCacheEntry& resolve_region_cache(const SDL_Point& world_pt,
                                                 const std::vector<Room*>& rooms);
    RegionCacheEntry classify_region_point(const SDL_Point& world_pt) const;
    void rebuild_region_area_index(const std::vector<Room*>& rooms);
    void add_indexed_area(const Area* area, RegionKind kind, const Room* owner);
    std::size_t compute_rooms_topology_hash(const Assets* assets) const;

    struct BoundaryConfig {
        float grid_spacing_multiplier = kDefaultGridSpacingMultiplier;
        float base_size_scale = kDefaultBaseSizeScale;
        float vertical_offset = kDefaultVerticalOffset;
        float max_random_jitter = 0.0f;
    };
    static BoundaryConfig& config();
};
