#pragma once

#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <SDL3/SDL.h>
#include <nlohmann/json.hpp>

#include "generate_rooms.hpp"

class Room;

namespace map_graph {

struct PlanNode {
    int id = -1;
    int layer = -1;
    std::string room_name;
    int max_instances = 0;
    std::vector<int> parent_ids;
    std::vector<int> child_ids;
};

struct PlanEdge {
    int from_id = -1;
    int to_id = -1;
    bool required = true;
};

struct MapGraphPlan {
    bool valid = false;
    std::string spawn_room_name;
    int trail_endpoint_containment_safety_px = 12;
    int trail_room_margin_px = 3000;
    std::vector<LayerSpec> resolved_layers;
    std::vector<PlanNode> nodes;
    std::vector<PlanEdge> edges;
    std::vector<std::string> diagnostics;
};

struct RoomRegenSnapshot {
    bool valid = false;
    Room* old_room = nullptr;
    SDL_Point old_center{0, 0};

    Room* parent = nullptr;
    std::vector<Room*> children;
    Room* left_sibling = nullptr;
    Room* right_sibling = nullptr;

    std::vector<Room*> connected_trails;
    std::vector<Room*> connected_neighbors;
    std::unordered_map<Room*, int> trail_pair_counts;
    std::vector<std::string> diagnostics;
};

struct RoomRegenPlan {
    bool valid = false;
    std::string selected_template_key;
    SDL_Point replacement_center{0, 0};
    std::vector<std::pair<Room*, Room*>> planned_trail_pairs;
    std::vector<std::string> diagnostics;
};

MapGraphPlan build_map_graph_plan(nlohmann::json* map_manifest);
RoomRegenSnapshot capture_room_regen_snapshot(Room* room);
RoomRegenPlan build_room_regen_plan(const RoomRegenSnapshot& snapshot,
                                    const std::string& selected_template_key);

} // namespace map_graph
