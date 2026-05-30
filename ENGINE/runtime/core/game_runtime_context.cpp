#include "core/game_runtime_context.hpp"

#include <algorithm>
#include <cmath>

#include "core/AssetsManager.hpp"
#include "gameplay/map_generation/room.hpp"

namespace runtime::context {

namespace {

constexpr float kDefaultDeltaSeconds = 1.0f / 60.0f;
constexpr float kMinimumAggressionDurationSeconds = 0.1f;

float sanitize_delta_seconds(float dt) {
    if (!std::isfinite(dt) || dt < 0.0f) {
        return kDefaultDeltaSeconds;
    }
    return dt;
}

} // namespace

void GameRuntimeContext::begin_frame(Assets* assets,
                                     std::uint32_t frame_id,
                                     float delta_seconds,
                                     Room* current_room,
                                     Asset* player,
                                     const WarpedScreenGrid* camera_view,
                                     const runtime::config::RuntimeGameConfig* runtime_config) {
    assets_ = assets;
    frame_id_ = frame_id;
    delta_seconds_ = sanitize_delta_seconds(delta_seconds);
    elapsed_seconds_ += delta_seconds_;
    current_room_ = current_room;
    player_ = player;
    camera_view_ = camera_view;
    runtime_config_ = runtime_config;
    player_motion_disturbance_.active = false;
    player_motion_disturbance_.is_dashing = false;
    player_motion_disturbance_.is_sprinting = false;
    aim_assist_overlay_.enabled = false;
    aim_assist_overlay_.points.clear();
    map_graph_.set_current_room(current_room_);
    prune_expired_room_fly_aggression();
}

void GameRuntimeContext::set_room_fly_aggression(const std::string& room_name, float duration_seconds) {
    if (room_name.empty()) {
        return;
    }

    const float clamped_duration =
        std::max(duration_seconds, kMinimumAggressionDurationSeconds);

    RoomFlyAggressionState& state = room_fly_aggression_[room_name];
    state.aggressive = true;
    state.last_trigger_frame = frame_id_;
    state.expires_at_seconds = elapsed_seconds_ + clamped_duration;
}

bool GameRuntimeContext::is_room_fly_aggressive(std::string_view room_name) const {
    if (room_name.empty()) {
        return false;
    }
    auto it = room_fly_aggression_.find(std::string(room_name));
    if (it == room_fly_aggression_.end()) {
        return false;
    }
    return it->second.aggressive && elapsed_seconds_ < it->second.expires_at_seconds;
}

bool GameRuntimeContext::is_current_room_fly_aggressive() const {
    if (!current_room_) {
        return false;
    }
    return is_room_fly_aggressive(current_room_->room_name);
}

const RoomFlyAggressionState* GameRuntimeContext::room_fly_aggression_state(
    std::string_view room_name) const {
    if (room_name.empty()) {
        return nullptr;
    }
    auto it = room_fly_aggression_.find(std::string(room_name));
    if (it == room_fly_aggression_.end()) {
        return nullptr;
    }
    return &it->second;
}

void GameRuntimeContext::rebuild_runtime_map_graph(const std::vector<Room*>& rooms) {
    // Skip null pointers so runtime connectivity stays deterministic across editor/runtime rebuild passes.
    std::vector<Room*> filtered;
    filtered.reserve(rooms.size());
    for (Room* room : rooms) {
        if (room) {
            filtered.push_back(room);
        }
    }
    map_graph_.build_from_rooms(filtered);
    map_graph_.set_current_room(current_room_);
}

std::vector<Room*> GameRuntimeContext::connected_rooms(Room* room) const {
    return map_graph_.connected_rooms(room);
}

Room* GameRuntimeContext::parent_room(Room* room) const {
    return map_graph_.parent_room(room);
}

std::vector<Room*> GameRuntimeContext::child_rooms(Room* room) const {
    return map_graph_.child_rooms(room);
}

std::vector<Room*> GameRuntimeContext::rooms_in_layer(int layer) const {
    return map_graph_.rooms_in_layer(layer);
}

bool GameRuntimeContext::are_rooms_connected(Room* a, Room* b) const {
    return map_graph_.are_connected(a, b);
}

Room* GameRuntimeContext::trail_between(Room* a, Room* b) const {
    return map_graph_.trail_between(a, b);
}

void GameRuntimeContext::set_player_motion_disturbance(bool active,
                                                       bool is_sprinting,
                                                       bool is_dashing) {
    player_motion_disturbance_.active = active;
    player_motion_disturbance_.is_sprinting = is_sprinting;
    player_motion_disturbance_.is_dashing = is_dashing;
    if (!active) {
        return;
    }
    ++player_motion_disturbance_.pulse_id;
    player_motion_disturbance_.pulse_frame = frame_id_;
    player_motion_disturbance_.pulse_time_seconds = elapsed_seconds_;
}

void GameRuntimeContext::emit_player_damage_pulse(int damage_amount,
                                                  int health_after,
                                                  int starting_health) {
    const int sanitized_damage = std::max(0, damage_amount);
    if (sanitized_damage <= 0) {
        return;
    }

    const int safe_starting_health = std::max(1, starting_health);
    const int safe_health_after = std::max(0, health_after);
    const float ratio_after = std::clamp(
        static_cast<float>(safe_health_after) / static_cast<float>(safe_starting_health),
        0.0f,
        1.0f);

    ++player_damage_pulse_.pulse_id;
    player_damage_pulse_.pulse_time_seconds = elapsed_seconds_;
    player_damage_pulse_.damage_amount = sanitized_damage;
    player_damage_pulse_.health_after = safe_health_after;
    player_damage_pulse_.health_ratio_after = ratio_after;
}

void GameRuntimeContext::prune_expired_room_fly_aggression() {
    for (auto it = room_fly_aggression_.begin(); it != room_fly_aggression_.end();) {
        if (!it->second.aggressive || elapsed_seconds_ >= it->second.expires_at_seconds) {
            it = room_fly_aggression_.erase(it);
            continue;
        }
        ++it;
    }
}

} // namespace runtime::context
