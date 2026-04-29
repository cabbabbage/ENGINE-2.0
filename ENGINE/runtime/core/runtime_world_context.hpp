#pragma once

#include "runtime_game_config.hpp"

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
    bool remove_room(Room* target);
    bool replace_room(Room* target, std::unique_ptr<Room> replacement);
    void append_rooms(std::vector<std::unique_ptr<Room>> rooms);

    std::vector<Room*>& rooms() { return rooms_view_; }
    const std::vector<Room*>& rooms() const { return rooms_view_; }

    std::vector<std::unique_ptr<Room>>& owned_rooms() { return owned_rooms_; }
    const std::vector<std::unique_ptr<Room>>& owned_rooms() const { return owned_rooms_; }

    std::size_t topology_generation() const { return topology_generation_; }
    runtime::config::RuntimeGameConfig& game_config() { return game_config_; }
    const runtime::config::RuntimeGameConfig& game_config() const { return game_config_; }

private:
    std::vector<std::unique_ptr<Room>> owned_rooms_;
    std::vector<Room*> rooms_view_;
    std::size_t topology_generation_ = 0;
    runtime::config::RuntimeGameConfig game_config_{};
};
