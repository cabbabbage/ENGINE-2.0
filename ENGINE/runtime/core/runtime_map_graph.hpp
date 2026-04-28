#pragma once

#include <string>
#include <unordered_map>
#include <vector>

class Room;

namespace runtime::mapgraph {

struct RuntimeMapGraphNode {
    int node_id = -1;
    std::string room_name;
    int layer = -1;
    Room* room = nullptr;
    int parent_node_id = -1;
    std::vector<int> child_node_ids;
    std::vector<int> neighbor_node_ids;
};

struct RuntimeMapGraphEdge {
    int from_node_id = -1;
    int to_node_id = -1;
    Room* trail_room = nullptr;
    bool required = false;
};

class RuntimeMapGraph {
public:
    void clear();
    void build_from_rooms(const std::vector<Room*>& rooms);

    void set_current_room(Room* room);
    Room* current_room() const;

    std::vector<Room*> connected_rooms(Room* room) const;
    Room* parent_room(Room* room) const;
    std::vector<Room*> child_rooms(Room* room) const;
    std::vector<Room*> rooms_in_layer(int layer) const;
    bool are_connected(Room* a, Room* b) const;
    Room* trail_between(Room* a, Room* b) const;

    const std::vector<RuntimeMapGraphNode>& nodes() const { return nodes_; }
    const std::vector<RuntimeMapGraphEdge>& edges() const { return edges_; }

private:
    int node_id_for_room(Room* room) const;

    std::vector<RuntimeMapGraphNode> nodes_;
    std::vector<RuntimeMapGraphEdge> edges_;
    std::unordered_map<Room*, int> room_to_node_id_;
    std::unordered_map<std::string, int> room_name_to_node_id_;
    int current_room_node_id_ = -1;
};

} // namespace runtime::mapgraph

