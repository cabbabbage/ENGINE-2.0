#include "runtime_map_graph.hpp"

#include <algorithm>
#include <cctype>

#include "gameplay/map_generation/room.hpp"

namespace runtime::mapgraph {
namespace {

bool is_trail_room(const Room* room) {
    if (!room) return false;
    std::string lowered = room->type;
    std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return lowered == "trail";
}

} // namespace

void RuntimeMapGraph::clear() {
    nodes_.clear();
    edges_.clear();
    room_to_node_id_.clear();
    room_name_to_node_id_.clear();
    current_room_node_id_ = -1;
}

void RuntimeMapGraph::build_from_rooms(const std::vector<Room*>& rooms) {
    clear();

    for (Room* room : rooms) {
        if (!room || is_trail_room(room)) {
            continue;
        }
        RuntimeMapGraphNode node;
        node.node_id = static_cast<int>(nodes_.size());
        node.room_name = room->room_name;
        node.layer = room->layer;
        node.room = room;
        nodes_.push_back(std::move(node));
        room_to_node_id_[room] = nodes_.back().node_id;
        room_name_to_node_id_[room->room_name] = nodes_.back().node_id;
    }

    for (RuntimeMapGraphNode& node : nodes_) {
        if (!node.room) continue;
        if (node.room->parent) {
            auto it = room_to_node_id_.find(node.room->parent);
            if (it != room_to_node_id_.end()) {
                node.parent_node_id = it->second;
                nodes_[it->second].child_node_ids.push_back(node.node_id);
                edges_.push_back(RuntimeMapGraphEdge{it->second, node.node_id, nullptr, true});
            }
        }
        for (Room* neighbor : node.room->connected_rooms) {
            auto it = room_to_node_id_.find(neighbor);
            if (it == room_to_node_id_.end()) {
                continue;
            }
            if (std::find(node.neighbor_node_ids.begin(), node.neighbor_node_ids.end(), it->second) ==
                node.neighbor_node_ids.end()) {
                node.neighbor_node_ids.push_back(it->second);
            }
        }
    }
}

int RuntimeMapGraph::node_id_for_room(Room* room) const {
    if (!room) return -1;
    auto it = room_to_node_id_.find(room);
    if (it != room_to_node_id_.end()) {
        return it->second;
    }
    auto by_name = room_name_to_node_id_.find(room->room_name);
    if (by_name != room_name_to_node_id_.end()) {
        return by_name->second;
    }
    return -1;
}

void RuntimeMapGraph::set_current_room(Room* room) {
    current_room_node_id_ = node_id_for_room(room);
}

Room* RuntimeMapGraph::current_room() const {
    if (current_room_node_id_ < 0 || current_room_node_id_ >= static_cast<int>(nodes_.size())) {
        return nullptr;
    }
    return nodes_[current_room_node_id_].room;
}

std::vector<Room*> RuntimeMapGraph::connected_rooms(Room* room) const {
    std::vector<Room*> out;
    const int id = node_id_for_room(room);
    if (id < 0 || id >= static_cast<int>(nodes_.size())) return out;
    const RuntimeMapGraphNode& node = nodes_[id];
    out.reserve(node.neighbor_node_ids.size() + node.child_node_ids.size() + (node.parent_node_id >= 0 ? 1 : 0));
    for (int neighbor_id : node.neighbor_node_ids) {
        if (neighbor_id >= 0 && neighbor_id < static_cast<int>(nodes_.size()) && nodes_[neighbor_id].room) {
            out.push_back(nodes_[neighbor_id].room);
        }
    }
    if (node.parent_node_id >= 0 && node.parent_node_id < static_cast<int>(nodes_.size()) &&
        nodes_[node.parent_node_id].room) {
        out.push_back(nodes_[node.parent_node_id].room);
    }
    for (int child_id : node.child_node_ids) {
        if (child_id >= 0 && child_id < static_cast<int>(nodes_.size()) && nodes_[child_id].room) {
            out.push_back(nodes_[child_id].room);
        }
    }
    return out;
}

Room* RuntimeMapGraph::parent_room(Room* room) const {
    const int id = node_id_for_room(room);
    if (id < 0 || id >= static_cast<int>(nodes_.size())) return nullptr;
    const int parent_id = nodes_[id].parent_node_id;
    if (parent_id < 0 || parent_id >= static_cast<int>(nodes_.size())) return nullptr;
    return nodes_[parent_id].room;
}

std::vector<Room*> RuntimeMapGraph::child_rooms(Room* room) const {
    std::vector<Room*> out;
    const int id = node_id_for_room(room);
    if (id < 0 || id >= static_cast<int>(nodes_.size())) return out;
    for (int child_id : nodes_[id].child_node_ids) {
        if (child_id >= 0 && child_id < static_cast<int>(nodes_.size()) && nodes_[child_id].room) {
            out.push_back(nodes_[child_id].room);
        }
    }
    return out;
}

std::vector<Room*> RuntimeMapGraph::rooms_in_layer(int layer) const {
    std::vector<Room*> out;
    for (const RuntimeMapGraphNode& node : nodes_) {
        if (node.layer == layer && node.room) {
            out.push_back(node.room);
        }
    }
    return out;
}

bool RuntimeMapGraph::are_connected(Room* a, Room* b) const {
    const int ia = node_id_for_room(a);
    const int ib = node_id_for_room(b);
    if (ia < 0 || ib < 0) return false;
    if (ia == ib) return true;
    const RuntimeMapGraphNode& node = nodes_[ia];
    if (node.parent_node_id == ib) return true;
    if (std::find(node.child_node_ids.begin(), node.child_node_ids.end(), ib) != node.child_node_ids.end()) return true;
    if (std::find(node.neighbor_node_ids.begin(), node.neighbor_node_ids.end(), ib) != node.neighbor_node_ids.end()) return true;
    return false;
}

Room* RuntimeMapGraph::trail_between(Room* a, Room* b) const {
    if (!a || !b) return nullptr;
    for (Room* connected : a->connected_rooms) {
        if (!connected || !is_trail_room(connected)) continue;
        for (Room* trail_connected : connected->connected_rooms) {
            if (trail_connected == b) {
                return connected;
            }
        }
    }
    return nullptr;
}

} // namespace runtime::mapgraph
