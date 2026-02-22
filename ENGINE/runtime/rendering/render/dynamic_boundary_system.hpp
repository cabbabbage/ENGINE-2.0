#pragma once

#include <SDL3/SDL.h>
#include <nlohmann/json.hpp>

#include "rendering/render/scaling_logic.hpp"
#include "gameplay/world/grid_point.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

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

    struct BoundaryCandidate {
        std::string asset_name;
        int         chance = 0;
        bool        is_null = false;
        std::vector<BoundaryFrame> frames;
        std::shared_ptr<AssetInfo> info;
        render_pipeline::ScalingLogic::HysteresisState hysteresis_state;
    };

    struct BoundaryType {
        std::string                 spawn_id;
        std::string                 display_name;
        int                         grid_resolution = 5;
        int                         jitter = 0;
        std::vector<BoundaryCandidate> candidates;
        int                         total_chance = 0;
    };

    struct BoundaryKey {
        int group = 0;
        int resolution_layer = 0;
        int grid_x = 0;
        int grid_y = 0;
        int world_z = 0;

        bool operator==(const BoundaryKey& other) const noexcept {
            return group == other.group &&
                   resolution_layer == other.resolution_layer &&
                   grid_x == other.grid_x &&
                   grid_y == other.grid_y &&
                   world_z == other.world_z;
        }
    };

    struct BoundaryKeyHash {
        std::size_t operator()(const BoundaryKey& key) const noexcept;
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

    bool is_initialized() const { return initialized_; }
    void invalidate_config();

private:
    bool initialized_ = false;
    bool config_dirty_ = true;
    SDL_Renderer* renderer_ = nullptr;
    AssetLibrary* asset_library_ = nullptr;

    nlohmann::json last_boundary_json_;
    std::uint64_t  config_revision_ = 0;
    std::uint64_t  warning_revision_ = 0;
    std::uint64_t  boundary_regen_seed_ = 0;

    std::vector<BoundaryType> boundary_types_;
    std::unordered_map<BoundaryKey, int, BoundaryKeyHash> boundary_assignments_;
    std::unordered_map<BoundaryKey, FrameState, BoundaryKeyHash> animation_states_;
    std::vector<BoundarySprite> active_boundary_sprites_;
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
    struct RegionCacheEntry {
        world::GridPoint::RegionKind region_kind = world::GridPoint::RegionKind::Boundary;
        const Room* owner = nullptr;
        bool blocked = false;
    };
    struct RegionCacheFingerprint {
        std::uint64_t config_revision = 0;
        std::size_t map_info_hash = 0;
        std::size_t rooms_hash = 0;
        int origin_x = 0;
        int origin_y = 0;
        int origin_layer = 0;
        int grid_resolution = 0;
        float spacing_multiplier = kDefaultGridSpacingMultiplier;

        bool operator==(const RegionCacheFingerprint& other) const noexcept {
            return config_revision == other.config_revision &&
                   map_info_hash == other.map_info_hash &&
                   rooms_hash == other.rooms_hash &&
                   origin_x == other.origin_x &&
                   origin_y == other.origin_y &&
                   origin_layer == other.origin_layer &&
                   grid_resolution == other.grid_resolution &&
                   spacing_multiplier == other.spacing_multiplier;
        }
        bool operator!=(const RegionCacheFingerprint& other) const noexcept {
            return !(*this == other);
        }
    };
    std::unordered_map<RegionCacheKey, RegionCacheEntry, RegionCacheKeyHash> region_cache_;
    RegionCacheFingerprint region_cache_fingerprint_;

    void parse_boundary_config(const nlohmann::json& map_info);
    void build_candidate_frames(BoundaryCandidate& candidate);
    bool needs_reparse(const nlohmann::json& map_info) const;
    void clear_runtime_caches();

    BoundaryKey  make_key(int group_idx, int resolution_layer, int grid_x, int grid_y, int world_z) const;
    std::uint64_t hash_key(const BoundaryKey& key) const;
    int select_candidate_for_key(const BoundaryKey& key, const BoundaryType& btype);
    SDL_FPoint sample_jitter_offset(const BoundaryKey& key, float max_jitter) const;
    void ensure_region_cache_valid(const world::WorldGrid& grid,
                                   std::size_t map_info_hash,
                                   std::size_t rooms_hash,
                                   float spacing_multiplier);
    const RegionCacheEntry& resolve_region_cache(const SDL_Point& world_pt,
                                                 const std::vector<Room*>& rooms);
    RegionCacheEntry classify_region_point(const SDL_Point& world_pt,
                                           const std::vector<Room*>& rooms) const;
    std::size_t compute_map_info_hash(const nlohmann::json& map_info) const;
    std::size_t compute_rooms_topology_hash(const Assets* assets) const;

    struct BoundaryConfig {
        float grid_spacing_multiplier = kDefaultGridSpacingMultiplier;
        float base_size_scale = kDefaultBaseSizeScale;
        float vertical_offset = kDefaultVerticalOffset;
        float max_random_jitter = 0.0f;
    };
    static BoundaryConfig& config();
};
