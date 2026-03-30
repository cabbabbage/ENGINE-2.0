#pragma once

#include <cstddef>
#include <memory>
#include <vector>

class Room;

class RuntimeWorldContext {
public:
    RuntimeWorldContext() = default;
    explicit RuntimeWorldContext(std::vector<std::unique_ptr<Room>> rooms) {
        adopt_rooms(std::move(rooms));
    }

    void adopt_rooms(std::vector<std::unique_ptr<Room>> rooms);
    void rebuild_room_view();
    void notify_topology_changed();

    std::vector<Room*>& rooms() { return rooms_view_; }
    const std::vector<Room*>& rooms() const { return rooms_view_; }

    std::vector<std::unique_ptr<Room>>& owned_rooms() { return owned_rooms_; }
    const std::vector<std::unique_ptr<Room>>& owned_rooms() const { return owned_rooms_; }

    std::size_t topology_generation() const { return topology_generation_; }

private:
    std::vector<std::unique_ptr<Room>> owned_rooms_;
    std::vector<Room*> rooms_view_;
    std::size_t topology_generation_ = 0;
};
