#pragma once

#include <SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

class Asset;

namespace world {

struct Chunk;

// Parameters needed for world→screen projection, extracted from CameraState
struct CameraProjectionParams {
    // Camera position in meters
    double position_x = 0.0, position_y = 0.0, position_z = 0.0;
    // Camera basis vectors
    double forward_x = 0.0, forward_y = 1.0, forward_z = 0.0;
    double right_x = 1.0, right_y = 0.0, right_z = 0.0;
    double up_x = 0.0, up_y = 0.0, up_z = 1.0;
    // World anchor point (pixels)
    double anchor_world_x = 0.0, anchor_world_y = 0.0;
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
    float offscreen_fade_amount_px = 200.0f;
    // Screen dimensions
    int screen_width = 0, screen_height = 0;
    // Version for cache invalidation
    std::uint64_t state_version = 0;
};

using GridId = std::uint64_t;

struct GridPoint {
    GridPoint() = delete;
    GridPoint(int world_x,
              int world_y,
              int world_z,
              int resolution_layer,
              SDL_Point grid_idx,
              SDL_Point chunk_idx,
              GridId legacy_id,
              Chunk* owning_chunk,
              GridPoint* parent_point = nullptr)
        : id(legacy_id)
        , world_x_(world_x)
        , world_y_(world_y)
        , world_z_(world_z)
        , resolution_layer_(resolution_layer)
        , parent_(parent_point)
        , world(SDL_Point{world_x, world_y})
        , grid_index(grid_idx)
        , chunk_index(chunk_idx)
        , chunk(owning_chunk) {}

    GridPoint(const GridPoint&) = default;
    GridPoint(GridPoint&&) = default;
    GridPoint& operator=(const GridPoint&) = delete;
    GridPoint& operator=(GridPoint&& other) noexcept;

    int world_x() const { return world_x_; }
    int world_y() const { return world_y_; }
    int world_z() const { return world_z_; }
    int resolution_layer() const { return resolution_layer_; }
    GridPoint* parent() const { return parent_; }

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

    // Phase 1 (3d_refactor_plan.md): canonical 3D identity is immutable and must
    // match the legacy 2D fields until Cleanup.
    // Legacy fields stay present for migration; avoid mutating them outside WorldGrid wiring.
    const GridId     id           = 0;
    const SDL_Point  world        = SDL_Point{0, 0};
    const SDL_Point  grid_index   = SDL_Point{0, 0};
    SDL_Point        chunk_index  = SDL_Point{0, 0};
    Chunk*           chunk        = nullptr;

    // Per-frame camera fields; WarpedScreenGrid must refresh these each rebuild.
    SDL_FPoint screen             = SDL_FPoint{0.0f, 0.0f};
    float      parallax_dx        = 0.0f;
    float      vertical_scale     = 1.0f;
    float      horizon_fade_alpha = 1.0f;
    float      near_camera_fade_alpha = 1.0f;
    float      perspective_scale  = 1.0f;
    float      distance_to_camera = 0.0f;
    float      tilt_radians       = 0.0f;
    bool       on_screen          = false;

    mutable std::uint64_t screen_data_frame_updated = 0;
    mutable bool          screen_data_valid         = false;
    mutable std::uint64_t last_camera_state_version_ = 0;

    // Smart caching: returns true if projection calculation is needed
    bool needs_projection_update(std::uint64_t current_frame, std::uint64_t camera_version) const {
        return !screen_data_valid ||
               screen_data_frame_updated != current_frame ||
               last_camera_state_version_ != camera_version;
    }

    // Self-contained projection: GridPoint calculates its own screen position
    void project_to_screen(const CameraProjectionParams& params);

    // Update world position (controlled mutation for movement)
    void update_world_position(int new_x, int new_y, int new_z = 0);

    // Swap mutable data between points (for efficient grid reorganization)
    friend void swap(GridPoint& a, GridPoint& b) noexcept;

    void reset_frame_state(std::uint64_t frame_stamp = 0) {
        screen             = SDL_FPoint{0.0f, 0.0f};
        parallax_dx        = 0.0f;
        vertical_scale     = 1.0f;
        horizon_fade_alpha = 1.0f;
        near_camera_fade_alpha = 1.0f;
        perspective_scale  = 1.0f;
        distance_to_camera = 0.0f;
        tilt_radians       = 0.0f;
        on_screen          = false;
        screen_data_frame_updated = frame_stamp;
        screen_data_valid  = false;
    }

    void invalidate_screen_data() {
        screen_data_valid = false;
    }

    void mark_screen_data_updated(std::uint64_t frame) {
        screen_data_frame_updated = frame;
        screen_data_valid = true;
    }

    bool has_valid_screen_data(std::uint64_t current_frame) const {
        return screen_data_valid && screen_data_frame_updated == current_frame;
    }

    // Asset/branch tracking: occupants remain the canonical list; branch bits mark active 3D children.
    static constexpr std::uint8_t BRANCH_X_NEG = 1 << 0;
    static constexpr std::uint8_t BRANCH_X_POS = 1 << 1;
    static constexpr std::uint8_t BRANCH_Y_NEG = 1 << 2;
    static constexpr std::uint8_t BRANCH_Y_POS = 1 << 3;
    static constexpr std::uint8_t BRANCH_Z_NEG = 1 << 4;
    static constexpr std::uint8_t BRANCH_Z_POS = 1 << 5;
    // Legacy aliases for in-flight Phase 3/4 code paths.
    static constexpr std::uint8_t kBranchXNeg = BRANCH_X_NEG;
    static constexpr std::uint8_t kBranchXPos = BRANCH_X_POS;
    static constexpr std::uint8_t kBranchYNeg = BRANCH_Y_NEG;
    static constexpr std::uint8_t kBranchYPos = BRANCH_Y_POS;
    static constexpr std::uint8_t kBranchZNeg = BRANCH_Z_NEG;
    static constexpr std::uint8_t kBranchZPos = BRANCH_Z_POS;

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

    std::string debug_identity_and_mask() const {
        // Compact identity + branch state for dev tools / logging.
        return "id=" + std::to_string(id) +
               " world=(" + std::to_string(world_x_) + "," + std::to_string(world_y_) + "," + std::to_string(world_z_) + ")" +
               " layer=" + std::to_string(resolution_layer_) +
               " grid_index=(" + std::to_string(grid_index.x) + "," + std::to_string(grid_index.y) + ")" +
               " chunk_index=(" + std::to_string(chunk_index.x) + "," + std::to_string(chunk_index.y) + ")" +
               " assets=" + std::to_string(occupants.size()) +
               " children_with_assets=" + std::to_string(children_with_assets) +
               " mask=0x" + to_hex(active_child_mask);
    }

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

    int world_x_          = 0;
    int world_y_          = 0;
    int world_z_          = 0;
    int resolution_layer_ = 0;
    GridPoint* parent_    = nullptr;

    // Non-owning hierarchy links; Map Grid owns nodes, Screen Grid only references.
    GridPoint* x_child_neg_ = nullptr;
    GridPoint* x_child_pos_ = nullptr;
    GridPoint* y_child_neg_ = nullptr;
    GridPoint* y_child_pos_ = nullptr;
    GridPoint* z_child_neg_ = nullptr;
    GridPoint* z_child_pos_ = nullptr;
};

}
