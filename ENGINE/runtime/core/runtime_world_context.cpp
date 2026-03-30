#include "runtime_world_context.hpp"

#include "gameplay/map_generation/room.hpp"

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
