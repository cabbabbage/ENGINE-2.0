#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

#include "generate_rooms.hpp"

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

MapGraphPlan build_map_graph_plan(nlohmann::json* map_manifest);

} // namespace map_graph
