#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
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
};

} // namespace runtime::context
