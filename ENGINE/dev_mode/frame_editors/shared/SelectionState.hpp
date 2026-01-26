#pragma once

#include <SDL.h>

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

    bool has_target() const { return target != SelectionTarget::None; }

    void reset() {
        target = SelectionTarget::None;
        axis = AdjustmentAxis::X;
        child_index = -1;
        attack_vector_index = -1;
        world_pos = SDL_FPoint{0.0f, 0.0f};
        world_z = 0.0f;
        screen_pos = SDL_Point{0, 0};
    }
};

}  // namespace devmode::frame_editors
