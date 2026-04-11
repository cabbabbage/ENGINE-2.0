#pragma once

#include <SDL3/SDL.h>

#include <cstdint>

class Asset;
class Assets;
class Room;
class WarpedScreenGrid;

namespace animation_update::custom_controllers {

struct FlyOrbitTargetSnapshot {
    bool valid = false;
    SDL_Point world_xz{0, 0};
    int world_y = 0;
    int grid_resolution = 0;
    std::uint32_t last_update_frame = 0;
    std::uint32_t next_refresh_frame = 0;
    std::uint64_t target_id = 0;
    std::uint32_t target_version = 0;
};

struct FlyOrbitPoint3D {
    bool valid = false;
    SDL_Point world_xz{0, 0};
    int world_y = 0;
    int grid_resolution = 0;
};

struct ControllerGameContext {
    Asset* self = nullptr;
    Assets* assets = nullptr;
    Asset* player = nullptr;
    Asset* resolved_player = nullptr;
    bool player_valid = false;

    std::uint32_t frame_id = 0;
    float delta_seconds = 1.0f / 60.0f;

    const WarpedScreenGrid* camera_view = nullptr;
    Room* current_room = nullptr;

    SDL_Point self_world_xz{0, 0};
    int self_world_y = 0;
    int self_grid_resolution = 0;
    FlyOrbitPoint3D fly_orbit_point{};
    FlyOrbitTargetSnapshot fly_orbit_target{};
    bool fly_orbit_target_changed = false;

    bool has_self() const;
    bool has_assets() const;
    bool has_player() const;
    bool player_is_valid() const;
    bool self_is_player() const;

    bool has_current_room() const;
    bool self_has_room_assignment() const;
    bool player_has_room_assignment() const;
    bool self_in_current_room() const;
    bool player_in_current_room() const;
    bool self_and_player_share_room() const;
};

ControllerGameContext build_controller_game_context(
    Asset* self,
    Assets* assets,
    const FlyOrbitTargetSnapshot* previous_orbit_target = nullptr);

} // namespace animation_update::custom_controllers
