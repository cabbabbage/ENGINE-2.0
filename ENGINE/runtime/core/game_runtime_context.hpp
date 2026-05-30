#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "runtime_map_graph.hpp"

class Asset;
class Assets;
class Room;
class WarpedScreenGrid;

namespace runtime::config {
struct RuntimeGameConfig;
} // namespace runtime::config

namespace runtime::context {

struct RoomFlyAggressionState {
    bool aggressive = false;
    float expires_at_seconds = 0.0f;
    std::uint32_t last_trigger_frame = 0;
};

struct PlayerMotionDisturbanceState {
    bool active = false;
    bool is_dashing = false;
    bool is_sprinting = false;
    std::uint64_t pulse_id = 0;
    std::uint32_t pulse_frame = 0;
    float pulse_time_seconds = 0.0f;
};

struct PlayerDamagePulseState {
    std::uint64_t pulse_id = 0;
    float pulse_time_seconds = 0.0f;
    int damage_amount = 0;
    int health_after = 0;
    float health_ratio_after = 1.0f;
};

struct AimAssistOverlayPoint {
    float world_x = 0.0f;
    float world_z = 0.0f;
};

struct AimAssistOverlayState {
    bool enabled = false;
    bool is_throw_arc = false;
    std::vector<AimAssistOverlayPoint> points;
    AimAssistOverlayPoint nub{};
};

class GameRuntimeContext {
public:
    void begin_frame(Assets* assets,
                     std::uint32_t frame_id,
                     float delta_seconds,
                     Room* current_room,
                     Asset* player,
                     const WarpedScreenGrid* camera_view,
                     const runtime::config::RuntimeGameConfig* runtime_config);

    Assets* assets() const { return assets_; }
    std::uint32_t frame_id() const { return frame_id_; }
    float delta_seconds() const { return delta_seconds_; }
    float elapsed_seconds() const { return elapsed_seconds_; }
    Room* current_room() const { return current_room_; }
    Asset* player() const { return player_; }
    const WarpedScreenGrid* camera_view() const { return camera_view_; }
    const runtime::config::RuntimeGameConfig* runtime_config() const { return runtime_config_; }
    const runtime::mapgraph::RuntimeMapGraph& map_graph() const { return map_graph_; }
    runtime::mapgraph::RuntimeMapGraph& mutable_map_graph() { return map_graph_; }

    void set_room_fly_aggression(const std::string& room_name, float duration_seconds = 20.0f);
    bool is_room_fly_aggressive(std::string_view room_name) const;
    bool is_current_room_fly_aggressive() const;
    const RoomFlyAggressionState* room_fly_aggression_state(std::string_view room_name) const;
    void rebuild_runtime_map_graph(const std::vector<Room*>& rooms);
    std::vector<Room*> connected_rooms(Room* room) const;
    Room* parent_room(Room* room) const;
    std::vector<Room*> child_rooms(Room* room) const;
    std::vector<Room*> rooms_in_layer(int layer) const;
    bool are_rooms_connected(Room* a, Room* b) const;
    Room* trail_between(Room* a, Room* b) const;
    void set_player_motion_disturbance(bool active, bool is_sprinting, bool is_dashing);
    const PlayerMotionDisturbanceState& player_motion_disturbance() const { return player_motion_disturbance_; }
    void emit_player_damage_pulse(int damage_amount, int health_after, int starting_health);
    const PlayerDamagePulseState& player_damage_pulse() const { return player_damage_pulse_; }
    void set_aim_assist_overlay(AimAssistOverlayState state) { aim_assist_overlay_ = std::move(state); }
    const AimAssistOverlayState& aim_assist_overlay() const { return aim_assist_overlay_; }

private:
    void prune_expired_room_fly_aggression();

    Assets* assets_ = nullptr;
    std::uint32_t frame_id_ = 0;
    float delta_seconds_ = 1.0f / 60.0f;
    float elapsed_seconds_ = 0.0f;
    Room* current_room_ = nullptr;
    Asset* player_ = nullptr;
    const WarpedScreenGrid* camera_view_ = nullptr;
    const runtime::config::RuntimeGameConfig* runtime_config_ = nullptr;
    runtime::mapgraph::RuntimeMapGraph map_graph_{};
    std::unordered_map<std::string, RoomFlyAggressionState> room_fly_aggression_;
    PlayerMotionDisturbanceState player_motion_disturbance_{};
    PlayerDamagePulseState player_damage_pulse_{};
    AimAssistOverlayState aim_assist_overlay_{};
};

} // namespace runtime::context
