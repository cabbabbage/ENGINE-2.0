#include "runtime_world_context.hpp"

#include "gameplay/map_generation/room.hpp"
#include <algorithm>

void RuntimeWorldContext::adopt_rooms(std::vector<std::unique_ptr<Room>> rooms) {
    owned_rooms_ = std::move(rooms);
    rebuild_room_view();
    notify_topology_changed();
}

void RuntimeWorldContext::rebuild_room_view() {
    rooms_view_.clear();
    rooms_view_.reserve(owned_rooms_.size());
    for (const auto& room : owned_rooms_) {
        rooms_view_.push_back(room.get());
    }
}

void RuntimeWorldContext::notify_topology_changed() {
    ++topology_generation_;
}

bool RuntimeWorldContext::remove_room(Room* target) {
    if (!target) {
        return false;
    }
    const auto it = std::find_if(owned_rooms_.begin(), owned_rooms_.end(),
                                 [target](const std::unique_ptr<Room>& room) {
                                     return room.get() == target;
                                 });
    if (it == owned_rooms_.end()) {
        return false;
    }
    owned_rooms_.erase(it);
    rebuild_room_view();
    notify_topology_changed();
    return true;
}

bool RuntimeWorldContext::replace_room(Room* target, std::unique_ptr<Room> replacement) {
    if (!target || !replacement) {
        return false;
    }
    const auto it = std::find_if(owned_rooms_.begin(), owned_rooms_.end(),
                                 [target](const std::unique_ptr<Room>& room) {
                                     return room.get() == target;
                                 });
    if (it == owned_rooms_.end()) {
        return false;
    }
    *it = std::move(replacement);
    rebuild_room_view();
    notify_topology_changed();
    return true;
}

void RuntimeWorldContext::append_rooms(std::vector<std::unique_ptr<Room>> rooms) {
    if (rooms.empty()) {
        return;
    }
    for (auto& room : rooms) {
        if (!room) {
            continue;
        }
        owned_rooms_.push_back(std::move(room));
    }
    rebuild_room_view();
    notify_topology_changed();
}
