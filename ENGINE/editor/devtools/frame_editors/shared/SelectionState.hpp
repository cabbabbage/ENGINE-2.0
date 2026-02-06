#pragma once

#include <SDL3/SDL.h>

namespace devmode::frame_editors {

enum class AdjustmentAxis { X, Y, Z };

enum class SelectionTarget {
    None,
    MovementPoint,
    ChildPoint,
    AttackStart,
    AttackControl,
    AttackEnd,
    HitboxCenter,
};

struct SelectionState {
    SelectionTarget target = SelectionTarget::None;
    AdjustmentAxis axis = AdjustmentAxis::X;
    int child_index = -1;
    int attack_vector_index = -1;
    SDL_FPoint world_pos{0.0f, 0.0f};
    float world_z = 0.0f;
    SDL_Point screen_pos{0, 0};
    SDL_FPoint anchor_world_pos{0.0f, 0.0f};
    float anchor_world_z = 0.0f;
    bool anchor_valid = false;

    bool has_target() const { return target != SelectionTarget::None; }

    bool has_anchor() const { return anchor_valid; }

    SDL_FPoint relative_world_pos() const {
        if (anchor_valid) {
            return SDL_FPoint{world_pos.x - anchor_world_pos.x,
                              world_pos.y - anchor_world_pos.y};
        }
        return world_pos;
    }

    float relative_world_z() const {
        return anchor_valid ? (world_z - anchor_world_z) : world_z;
    }

    SDL_FPoint anchor_point() const { return anchor_world_pos; }

    float anchor_z_point() const { return anchor_world_z; }

    void set_anchor_world(SDL_Point anchor, float anchor_z) {
        anchor_world_pos = SDL_FPoint{static_cast<float>(anchor.x), static_cast<float>(anchor.y)};
        anchor_world_z = anchor_z;
        anchor_valid = true;
    }

    void set_anchor_world(SDL_FPoint anchor, float anchor_z) {
        anchor_world_pos = anchor;
        anchor_world_z = anchor_z;
        anchor_valid = true;
    }

    void clear_anchor_world() {
        anchor_valid = false;
        anchor_world_pos = SDL_FPoint{0.0f, 0.0f};
        anchor_world_z = 0.0f;
    }

    void reset() {
        target = SelectionTarget::None;
        axis = AdjustmentAxis::X;
        child_index = -1;
        attack_vector_index = -1;
        world_pos = SDL_FPoint{0.0f, 0.0f};
        world_z = 0.0f;
        screen_pos = SDL_Point{0, 0};
        clear_anchor_world();
    }
};

}  // namespace devmode::frame_editors

