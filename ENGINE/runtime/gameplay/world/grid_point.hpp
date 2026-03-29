#pragma once

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "core/axis_convention.hpp"

class Asset;
class Room;

// Forward declarations to keep grid_point lightweight
namespace world {
class WorldGrid;
}

namespace world {

struct Chunk;

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

struct GridCoord {
    int x = 0;
    int z = 0; // ground depth index (Z axis depth)
};

struct GridKeyHash {
    std::size_t operator()(const GridKey& key) const noexcept;
};

// Parameters needed for world→screen projection, extracted from CameraState
struct CameraProjectionParams {
    // Camera position in meters
    double position_x = 0.0, position_y = 0.0, position_z = 0.0;
    // Camera basis vectors
    double forward_x = 0.0, forward_y = 0.0, forward_z = 1.0;
    double right_x = 1.0, right_y = 0.0, right_z = 0.0;
    double up_x = 0.0, up_y = 1.0, up_z = 0.0;
    // World anchor point (pixels) on the ground plane (X/Z) plus optional height offset.
    double anchor_world_x = 0.0; // ground-plane X anchor
    double anchor_world_y = 0.0; // world height anchor (Y)
    double anchor_world_z = 0.0; // ground-plane depth anchor (Z)
    // Conversion factor: world pixels to meters
    double meters_scale = 0.01;
    // Field of view tangents
    double tan_half_fov_x = 1.0, tan_half_fov_y = 1.0;
    // Clipping planes
    double near_plane = 0.1, far_plane = 10000.0;
    // Zoom and pan
    double screen_zoom = 1.0, screen_pan_y_px = 0.0;
    // Horizon and pitch
    double horizon_screen_y = 0.0, pitch_radians = 0.0;
    // Fade parameters
    float horizon_band_px = 100.0f;
    float near_camera_max_perspective_scale = 4.0f;
    // Screen dimensions
    int screen_width = 0, screen_height = 0;
    // Version for cache invalidation
    std::uint64_t state_version = 0;
};

using GridId = std::uint64_t;

struct GridPoint {
    struct ProjectionCache {
        SDL_FPoint screen = SDL_FPoint{0.0f, 0.0f};
        float parallax_dx = 0.0f;
        float vertical_scale = 1.0f;
        float horizon_fade_alpha = 1.0f;
        float perspective_scale = 1.0f;
        float distance_to_camera = 0.0f;
        float tilt_radians = 0.0f;
        bool on_screen = false;
        std::uint64_t screen_data_frame_updated = 0;
        bool screen_data_valid = false;
        std::uint64_t last_camera_state_version = 0;

        bool needs_projection_update(std::uint64_t current_frame,
                                     std::uint64_t camera_version) const {
            return !screen_data_valid ||
                   screen_data_frame_updated != current_frame ||
                   last_camera_state_version != camera_version;
        }

        void reset(std::uint64_t frame_stamp = 0) {
            screen = SDL_FPoint{0.0f, 0.0f};
            parallax_dx = 0.0f;
            vertical_scale = 1.0f;
            horizon_fade_alpha = 1.0f;
            perspective_scale = 1.0f;
            distance_to_camera = 0.0f;
            tilt_radians = 0.0f;
            on_screen = false;
            screen_data_frame_updated = frame_stamp;
            screen_data_valid = false;
        }

        void invalidate() {
            screen_data_valid = false;
        }

        void mark_updated(std::uint64_t frame) {
            screen_data_frame_updated = frame;
            screen_data_valid = true;
        }

        bool has_valid_data(std::uint64_t current_frame) const {
            return screen_data_valid && screen_data_frame_updated == current_frame;
        }
    };

    GridPoint() = delete;
    GridPoint(int world_x,
              int world_y,
              int world_z,
              int resolution_layer,
              GridCoord grid_idx,
              GridCoord chunk_idx,
              GridId legacy_id,
              Chunk* owning_chunk,
              GridPoint* parent_point = nullptr,
              bool is_virtual_point = false);

    GridPoint(const GridPoint& other);
    ~GridPoint();
    GridPoint(GridPoint&&) noexcept;
    GridPoint& operator=(const GridPoint&) = delete;
    GridPoint& operator=(GridPoint&& other) noexcept;

    // Canonical constructors/converters
    static GridPoint& from_world(int x, int y, int z, int layer, WorldGrid& grid);
    static GridPoint& from_world(const axis::WorldPos& pos, int layer, WorldGrid& grid);
    static GridPoint& from_world(const GridKey& key, WorldGrid& grid);
    static GridPoint* from_screen(const SDL_FPoint& screen,
                                  float world_y,
                                  const CameraProjectionParams& proj,
                                  WorldGrid& grid);
    static GridPoint make_virtual(const axis::WorldPos& world_pos, int resolution_layer = 0);
    static GridPoint make_virtual(int world_x,
                                  int world_y,
                                  int world_z = 0,
                                  int resolution_layer = 0);

    GridKey key() const;
    GridId  hash_key() const;
    static GridId hash_key(const GridKey& key);

    axis::WorldPos world_position() const { return world_pos_; }
    int world_x() const { return world_pos_.x; }
    int world_y() const { return world_pos_.y; }
    int world_z() const { return world_pos_.z; }
    int resolution_layer() const { return resolution_layer_; }
    GridPoint* parent() const { return parent_; }
    bool is_virtual() const { return is_virtual_; }

    // True when this grid point represents the ground (floor) for its XZ.
    bool is_floor = false;

    // Renderer/UI boundary helpers (explicit conversions)
    SDL_Point to_sdl_point() const {
        const axis::WorldPos canonical = world_position();
        return SDL_Point{canonical.x, canonical.z};
    }
    SDL_FPoint to_sdl_fpoint() const {
        const axis::WorldPos canonical = world_position();
        return SDL_FPoint{static_cast<float>(canonical.x), static_cast<float>(canonical.z)};
    }

    enum class ChildDirection {
        XNeg = 0,
        XPos,
        YNeg,
        YPos,
        ZNeg,
        ZPos
    };

    GridPoint* child(ChildDirection dir) const {
        switch (dir) {
        case ChildDirection::XNeg: return x_child_neg_;
        case ChildDirection::XPos: return x_child_pos_;
        case ChildDirection::YNeg: return y_child_neg_;
        case ChildDirection::YPos: return y_child_pos_;
        case ChildDirection::ZNeg: return z_child_neg_;
        case ChildDirection::ZPos: return z_child_pos_;
        default: return nullptr;
        }
    }

    void set_child(ChildDirection dir, GridPoint* child_ptr) {
        // Map Grid owns nodes; these pointers are non-owning links in the hierarchy.
        switch (dir) {
        case ChildDirection::XNeg: x_child_neg_ = child_ptr; break;
        case ChildDirection::XPos: x_child_pos_ = child_ptr; break;
        case ChildDirection::YNeg: y_child_neg_ = child_ptr; break;
        case ChildDirection::YPos: y_child_pos_ = child_ptr; break;
        case ChildDirection::ZNeg: z_child_neg_ = child_ptr; break;
        case ChildDirection::ZPos: z_child_pos_ = child_ptr; break;
        default: break;
        }
    }

    const GridId     id           = 0;
    const GridCoord  grid_index   = GridCoord{0, 0};
    GridCoord        chunk_index  = GridCoord{0, 0};
    Chunk*           chunk        = nullptr;

    // Per-frame camera projection and screen cache data.
    const ProjectionCache& projection_cache() const { return projection_; }
    ProjectionCache& mutable_projection_cache() { return projection_; }
    SDL_FPoint screen_position() const { return projection_.screen; }
    float perspective_scale() const { return projection_.perspective_scale; }
    float vertical_scale() const { return projection_.vertical_scale; }
    float horizon_fade_alpha() const { return projection_.horizon_fade_alpha; }
    float distance_to_camera() const { return projection_.distance_to_camera; }
    float tilt_radians() const { return projection_.tilt_radians; }
    bool is_on_screen() const { return projection_.on_screen; }

    // Smart caching: returns true if projection calculation is needed
    bool needs_projection_update(std::uint64_t current_frame,
                                 std::uint64_t camera_version) const {
        return projection_.needs_projection_update(current_frame, camera_version);
    }

    // Self-contained projection: GridPoint calculates its own screen position
    void project_to_screen(const CameraProjectionParams& params);

    // Update world position (controlled mutation for movement)
    void update_world_position(const axis::WorldPos& new_pos);
    void update_world_position(int new_x, int new_y, int new_z = 0);
    // Convenience helper to change only world Z without touching XY.
    void set_world_z(int new_z);

    // Swap mutable data between points (for efficient grid reorganization)
    friend void swap(GridPoint& a, GridPoint& b) noexcept;

    void reset_frame_state(std::uint64_t frame_stamp = 0) {
        is_floor           = (world_position().y == 0);
        projection_.reset(frame_stamp);
    }

    void invalidate_screen_data() {
        projection_.invalidate();
    }

    void mark_screen_data_updated(std::uint64_t frame) {
        projection_.mark_updated(frame);
    }

    bool has_valid_screen_data(std::uint64_t current_frame) const {
        return projection_.has_valid_data(current_frame);
    }

    // Asset/branch tracking: occupants remain the canonical list; branch bits mark active 3D children.
    static constexpr std::uint8_t BRANCH_X_NEG = 1 << 0;
    static constexpr std::uint8_t BRANCH_X_POS = 1 << 1;
    static constexpr std::uint8_t BRANCH_Y_NEG = 1 << 2;
    static constexpr std::uint8_t BRANCH_Y_POS = 1 << 3;
    static constexpr std::uint8_t BRANCH_Z_NEG = 1 << 4;
    static constexpr std::uint8_t BRANCH_Z_POS = 1 << 5;

    std::vector<std::unique_ptr<Asset>>& assets_here() { return occupants; }
    const std::vector<std::unique_ptr<Asset>>& assets_here() const { return occupants; }

    void set_child_active(ChildDirection dir) {
        active_child_mask |= child_bit(dir);
    }

    void clear_child_active(ChildDirection dir) {
        active_child_mask &= static_cast<std::uint8_t>(~child_bit(dir));
    }

    void set_child_with_assets(ChildDirection dir) {
        const std::uint8_t bit = child_bit(dir);
        const bool was_active = (active_child_mask & bit) != 0;
        active_child_mask |= bit;
        if (!was_active) {
            ++children_with_assets;
        }
    }

    void clear_child_with_assets(ChildDirection dir) {
        const std::uint8_t bit = child_bit(dir);
        const bool was_active = (active_child_mask & bit) != 0;
        active_child_mask &= static_cast<std::uint8_t>(~bit);
        if (was_active) {
            children_with_assets = std::max(0, children_with_assets - 1);
        }
    }

    bool child_active(ChildDirection dir) const {
        return (active_child_mask & child_bit(dir)) != 0;
    }

    std::optional<ChildDirection> direction_for_child(const GridPoint* child_ptr) const {
        if (!child_ptr) {
            return std::nullopt;
        }
        if (child_ptr == x_child_neg_) return ChildDirection::XNeg;
        if (child_ptr == x_child_pos_) return ChildDirection::XPos;
        if (child_ptr == y_child_neg_) return ChildDirection::YNeg;
        if (child_ptr == y_child_pos_) return ChildDirection::YPos;
        if (child_ptr == z_child_neg_) return ChildDirection::ZNeg;
        if (child_ptr == z_child_pos_) return ChildDirection::ZPos;
        return std::nullopt;
    }

    bool set_branch_bit_for_child(GridPoint* child_ptr) {
        const auto dir = direction_for_child(child_ptr);
        if (!dir.has_value()) {
            return false;
        }
        const std::uint8_t bit = child_bit(*dir);
        const bool was_active = (active_child_mask & bit) != 0;
        active_child_mask |= bit;
        if (!was_active) {
            ++children_with_assets;
            return true;
        }
        return false;
    }

    bool clear_branch_bit_for_child(GridPoint* child_ptr) {
        const auto dir = direction_for_child(child_ptr);
        if (!dir.has_value()) {
            return false;
        }
        const std::uint8_t bit = child_bit(*dir);
        const bool was_active = (active_child_mask & bit) != 0;
        active_child_mask &= static_cast<std::uint8_t>(~bit);
        if (was_active) {
            children_with_assets = std::max(0, children_with_assets - 1);
            return true;
        }
        return false;
    }

    bool has_assets_or_active_children() const {
        return !occupants.empty() || children_with_assets > 0 || active_child_mask != 0;
    }

    std::string debug_identity_and_mask() const;

    std::vector<std::unique_ptr<Asset>> occupants;
    int children_with_assets = 0;       // Count of direct children marked active (assets or active sub-branches).
    std::uint8_t active_child_mask = 0; // Branch activity bits for quick traversal across ±X/±Y/±Z.

private:
    static constexpr std::uint8_t child_bit(ChildDirection dir) {
        switch (dir) {
        case ChildDirection::XNeg: return BRANCH_X_NEG;
        case ChildDirection::XPos: return BRANCH_X_POS;
        case ChildDirection::YNeg: return BRANCH_Y_NEG;
        case ChildDirection::YPos: return BRANCH_Y_POS;
        case ChildDirection::ZNeg: return BRANCH_Z_NEG;
        case ChildDirection::ZPos: return BRANCH_Z_POS;
        default: return 0;
        }
    }

    static std::string to_hex(std::uint8_t value) {
        const char* digits = "0123456789ABCDEF";
        std::string out;
        out.reserve(2);
        out.push_back(digits[(value >> 4) & 0xF]);
        out.push_back(digits[value & 0xF]);
        return out;
    }

    axis::WorldPos world_pos_{};
    int resolution_layer_ = 0;
    GridPoint* parent_    = nullptr;
    bool is_virtual_      = false;
    ProjectionCache projection_{};

    // Non-owning hierarchy links; Map Grid owns nodes, Screen Grid only references.
    GridPoint* x_child_neg_ = nullptr;
    GridPoint* x_child_pos_ = nullptr;
    GridPoint* y_child_neg_ = nullptr;
    GridPoint* y_child_pos_ = nullptr;
    GridPoint* z_child_neg_ = nullptr;
    GridPoint* z_child_pos_ = nullptr;
};

struct GridBounds {
    GridPoint min;
    GridPoint max;

    GridBounds()
        : min(GridPoint::make_virtual(0, 0, 0, 0))
        , max(GridPoint::make_virtual(0, 0, 0, 0)) {}

    GridBounds(const GridPoint& min_pt, const GridPoint& max_pt)
        : min(min_pt)
        , max(max_pt) {}

    // Rectangle on the X/Z ground plane; world_y is the height (Y axis).
    static GridBounds from_xywh(int x, int z, int w, int h, int world_y = 0, int layer = 0);
    static GridBounds from_min_max(const GridPoint& min_pt, const GridPoint& max_pt);

    bool contains(const GridPoint& pt) const;
    GridBounds expanded(int margin) const;

    // Renderer-only escape hatches
    SDL_Rect  to_sdl_rect() const;
    SDL_FRect to_sdl_frect() const;
};

namespace grid_math {
// Lightweight helpers to keep spatial math in the GridPoint domain and avoid
// ad-hoc SDL_Point arithmetic sprinkled across systems that aren't render UI.
inline GridPoint from_sdl(const SDL_Point& pt,
                          int world_y = 0,
                          int resolution_layer = 0) {
    // SDL y maps to world depth (Z) on the ground plane; height supplied separately.
    return GridPoint::make_virtual(axis::WorldPos{pt.x, world_y, pt.y}, resolution_layer);
}

inline SDL_Point to_sdl(const GridPoint& gp) { return gp.to_sdl_point(); }

inline GridPoint offset(const GridPoint& base, int dx, int dz) {
    const axis::WorldPos next_pos{base.world_x() + dx, base.world_y(), base.world_z() + dz};
    return GridPoint::make_virtual(next_pos, base.resolution_layer());
}

inline GridPoint offset(const GridPoint& base, const SDL_Point& delta) {
    return offset(base, delta.x, delta.y);
}

inline int distance_sq(const GridPoint& a, const GridPoint& b) {
    const int dx = a.world_x() - b.world_x();
    const int dz = a.world_z() - b.world_z();
    return dx * dx + dz * dz;
}

inline int manhattan(const GridPoint& a, const GridPoint& b) {
    return std::abs(a.world_x() - b.world_x()) + std::abs(a.world_z() - b.world_z());
}
} // namespace grid_math

}
