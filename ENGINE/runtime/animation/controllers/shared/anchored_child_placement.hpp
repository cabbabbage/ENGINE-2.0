#pragma once

#include <SDL3/SDL.h>

#include "utils/AnchorPointResolver.hpp"

class WarpedScreenGrid;

namespace anchored_child_placement {

struct ParentTransformInput {
    float world_x = 0.0f;
    float world_y = 0.0f;
    float world_z = 0.0f;
    int resolution_layer = 0;
};

struct AnchorDefinitionInput {
    AnchorPoint anchor{};
};

struct ChildOffsetInput {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

struct SpriteTransformInput {
    bool mirror_x = false;
    bool mirror_y = false;
    float scale_x = 1.0f;
    float scale_y = 1.0f;
    float rotation_degrees = 0.0f;
};

struct CameraStateInput {
    const WarpedScreenGrid* camera = nullptr;
};

struct PlacementInput {
    ParentTransformInput parent{};
    AnchorDefinitionInput anchor_definition{};
    ChildOffsetInput anchor_world_displacement{};
    ChildOffsetInput child_offset{};
    SpriteTransformInput sprite_transform{};
    CameraStateInput camera_state{};
};

struct PlacementOutput {
    anchor_points::AnchorWorldPoint3 anchor_world{};
    anchor_points::AnchorWorldPoint3 child_world{};
    SDL_Point child_world_quantized{0, 0};
    int child_world_quantized_z = 0;
    int resolution_layer = 0;
    SDL_FPoint anchor_screen_px{0.0f, 0.0f};
    bool has_anchor_screen_px = false;
    SDL_FPoint child_screen_px{0.0f, 0.0f};
    bool has_child_screen_px = false;
};

// Shared child-placement contract used by runtime and dev tools:
// Input = parent transform + anchor definition + child offset + camera state.
// Output = resolved world transform (exact + quantized) and optional screen transform.
bool resolve_child_placement(const PlacementInput& input, PlacementOutput& output);

bool project_child_pivot_screen(const WarpedScreenGrid& cam,
                                float world_x,
                                float world_y,
                                float world_z,
                                SDL_FPoint& out_screen);

} // namespace anchored_child_placement
