#include "map_graph.hpp"

#include <algorithm>
#include <cctype>
#include <unordered_set>

namespace map_graph {
namespace {

constexpr const char* kDefaultSpawnName = "Spawn";

std::string normalize_tag_value(const std::string& raw) {
    std::string out;
    out.reserve(raw.size());
    bool last_space = false;
    for (unsigned char ch : raw) {
        if (std::isalnum(ch) || ch == '_' || ch == '-' || ch == '.' || ch == '#') {
            out.push_back(static_cast<char>(std::tolower(ch)));
            last_space = false;
            continue;
        }
        if (std::isspace(ch)) {
            if (!out.empty() && !last_space) {
                out.push_back(' ');
                last_space = true;
            }
        }
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

void append_unique(std::vector<std::string>& values, const std::string& value) {
    if (value.empty()) return;
    if (std::find(values.begin(), values.end(), value) == values.end()) {
        values.push_back(value);
    }
}

std::vector<std::string> collect_room_tags(const nlohmann::json& room_entry) {
    std::vector<std::string> tags;
    if (!room_entry.is_object()) {
        return tags;
    }

    auto add_from_array = [&](const nlohmann::json& arr) {
        if (!arr.is_array()) return;
        for (const auto& entry : arr) {
            if (!entry.is_string()) continue;
            const std::string normalized = normalize_tag_value(entry.get<std::string>());
            append_unique(tags, normalized);
        }
    };

    if (room_entry.contains("room_tags")) {
        add_from_array(room_entry["room_tags"]);
    }
    if (room_entry.contains("tags")) {
        const auto& section = room_entry["tags"];
        if (section.is_array()) {
            add_from_array(section);
        } else if (section.is_object()) {
            if (section.contains("include")) add_from_array(section["include"]);
            if (section.contains("tags")) add_from_array(section["tags"]);
        }
    }
    return tags;
}

void normalize_candidate_entry(nlohmann::json& candidate) {
    if (!candidate.is_object()) {
        candidate = nlohmann::json::object();
    }

    std::string source_type = candidate.value("source_type", std::string());
    if (source_type != "room_name" && source_type != "room_tag") {
        source_type = "room_name";
    }

    std::string value = candidate.value("value", std::string());
    if (value.empty() && candidate.contains("name") && candidate["name"].is_string()) {
        value = candidate["name"].get<std::string>();
    }
    if (source_type == "room_tag") {
        value = normalize_tag_value(value);
    }

    int min_instances = 0;
    int max_instances = 1;
    if (candidate.contains("min_instances") && candidate["min_instances"].is_number_integer()) {
        min_instances = std::max(0, candidate["min_instances"].get<int>());
    }
    if (candidate.contains("max_instances") && candidate["max_instances"].is_number_integer()) {
        max_instances = std::max(0, candidate["max_instances"].get<int>());
    }
    if (max_instances < min_instances) {
        max_instances = min_instances;
    }

    if (!candidate.contains("required_children") || !candidate["required_children"].is_array()) {
        candidate["required_children"] = nlohmann::json::array();
    }
    candidate["source_type"] = source_type;
    candidate["value"] = value;
    candidate["name"] = value;
    candidate["min_instances"] = min_instances;
    candidate["max_instances"] = max_instances;
}

void ensure_room_entry_exists(nlohmann::json& rooms_data, const std::string& room_name) {
    if (!rooms_data.contains(room_name) || !rooms_data[room_name].is_object()) {
        nlohmann::json room = nlohmann::json::object();
        room["name"] = room_name;
        room["geometry"] = "Circle";
        room["min_width"] = 3000;
        room["max_width"] = 3000;
        room["min_height"] = 3000;
        room["max_height"] = 3000;
        room["edge_smoothness"] = 2;
        room["inherits_map_assets"] = false;
        room["is_boss"] = false;
        room["spawn_groups"] = nlohmann::json::array();
        room["room_tags"] = nlohmann::json::array();
        rooms_data[room_name] = std::move(room);
        return;
    }
    rooms_data[room_name]["name"] = room_name;
    if (!rooms_data[room_name].contains("spawn_groups") || !rooms_data[room_name]["spawn_groups"].is_array()) {
        rooms_data[room_name]["spawn_groups"] = nlohmann::json::array();
    }
    if (!rooms_data[room_name].contains("room_tags") || !rooms_data[room_name]["room_tags"].is_array()) {
        rooms_data[room_name]["room_tags"] = nlohmann::json::array();
    }
}

} // namespace

MapGraphPlan build_map_graph_plan(nlohmann::json* map_manifest) {
    MapGraphPlan plan;
    if (!map_manifest || !map_manifest->is_object()) {
        plan.diagnostics.push_back("error: map manifest is missing or invalid.");
        return plan;
    }

    nlohmann::json& root = *map_manifest;
    if (!root.contains("rooms_data") || !root["rooms_data"].is_object()) {
        root["rooms_data"] = nlohmann::json::object();
    }
    nlohmann::json& rooms_data = root["rooms_data"];

    if (!root.contains("map_layers") || !root["map_layers"].is_array()) {
        root["map_layers"] = nlohmann::json::array();
    }
    nlohmann::json& layers = root["map_layers"];
    if (layers.empty()) {
        layers.push_back(nlohmann::json::object());
    }
    for (std::size_t i = 0; i < layers.size(); ++i) {
        if (!layers[i].is_object()) {
            layers[i] = nlohmann::json::object();
        }
        layers[i]["level"] = static_cast<int>(i);
        if (!layers[i].contains("name") || !layers[i]["name"].is_string()) {
            layers[i]["name"] = std::string("layer_") + std::to_string(i);
        }
        if (!layers[i].contains("rooms") || !layers[i]["rooms"].is_array()) {
            layers[i]["rooms"] = nlohmann::json::array();
        }
    }

    nlohmann::json& layer0 = layers[0];
    nlohmann::json& layer0_rooms = layer0["rooms"];
    for (auto& entry : layer0_rooms) {
        normalize_candidate_entry(entry);
    }
    if (layer0_rooms.empty() || !layer0_rooms[0].is_object() ||
        layer0_rooms[0].value("source_type", std::string()) != "room_name" ||
        layer0_rooms[0].value("value", std::string()).empty()) {
        nlohmann::json spawn_candidate = {
            {"source_type", "room_name"},
            {"value", std::string(kDefaultSpawnName)},
            {"name", std::string(kDefaultSpawnName)},
            {"min_instances", 1},
            {"max_instances", 1},
            {"required_children", nlohmann::json::array()}
        };
        layer0_rooms = nlohmann::json::array({std::move(spawn_candidate)});
    }
    if (layer0_rooms.size() > 1) {
        layer0_rooms.erase(layer0_rooms.begin() + 1, layer0_rooms.end());
    }
    normalize_candidate_entry(layer0_rooms[0]);
    layer0_rooms[0]["source_type"] = "room_name";
    layer0_rooms[0]["min_instances"] = 1;
    layer0_rooms[0]["max_instances"] = 1;
    layer0["min_rooms"] = 1;
    layer0["max_rooms"] = 1;

    plan.spawn_room_name = layer0_rooms[0].value("value", std::string(kDefaultSpawnName));
    if (plan.spawn_room_name.empty()) {
        plan.spawn_room_name = kDefaultSpawnName;
        layer0_rooms[0]["value"] = plan.spawn_room_name;
        layer0_rooms[0]["name"] = plan.spawn_room_name;
    }
    ensure_room_entry_exists(rooms_data, plan.spawn_room_name);

    std::unordered_map<std::string, std::vector<std::string>> tag_index;
    for (auto it = rooms_data.begin(); it != rooms_data.end(); ++it) {
        if (!it.value().is_object()) continue;
        const std::string room_name = it.key();
        ensure_room_entry_exists(rooms_data, room_name);
        it.value().erase("is_spawn");
        const std::vector<std::string> room_tags = collect_room_tags(it.value());
        for (const std::string& room_tag : room_tags) {
            tag_index[room_tag].push_back(room_name);
        }
        it.value()["room_tags"] = room_tags;
    }
    for (auto& [_, names] : tag_index) {
        std::sort(names.begin(), names.end());
    }

    std::unordered_map<std::string, int> node_by_layer_and_name;
    auto node_key = [](int layer, const std::string& name) {
        return std::to_string(layer) + "|" + name;
    };

    plan.resolved_layers.clear();
    plan.nodes.clear();
    plan.edges.clear();
    int next_node_id = 0;

    for (std::size_t layer_index = 0; layer_index < layers.size(); ++layer_index) {
        nlohmann::json& layer_entry = layers[layer_index];
        nlohmann::json& candidates = layer_entry["rooms"];
        LayerSpec layer_spec;
        layer_spec.level = static_cast<int>(layer_index);
        layer_spec.max_rooms = std::max(0, layer_entry.value("max_rooms", 0));

        std::unordered_map<std::string, RoomSpec> by_name;
        for (auto& candidate_entry : candidates) {
            normalize_candidate_entry(candidate_entry);
            const std::string source_type = candidate_entry.value("source_type", std::string("room_name"));
            const std::string value = candidate_entry.value("value", std::string());
            const int min_instances = std::max(0, candidate_entry.value("min_instances", 0));
            const int max_instances = std::max(min_instances, candidate_entry.value("max_instances", 1));

            std::vector<std::string> resolved_rooms;
            if (source_type == "room_name") {
                if (value.empty()) {
                    plan.diagnostics.push_back("error: layer " + std::to_string(layer_index) + " has empty room_name candidate.");
                } else if (!rooms_data.contains(value) || !rooms_data[value].is_object()) {
                    plan.diagnostics.push_back("error: layer " + std::to_string(layer_index) + " references missing room '" + value + "'.");
                } else {
                    resolved_rooms.push_back(value);
                }
            } else {
                const auto tag_it = tag_index.find(value);
                if (value.empty() || tag_it == tag_index.end() || tag_it->second.empty()) {
                    plan.diagnostics.push_back("error: layer " + std::to_string(layer_index) + " tag candidate '" + value + "' resolved to zero rooms.");
                } else {
                    resolved_rooms = tag_it->second;
                }
            }

            std::vector<std::string> required_children;
            const auto required_it = candidate_entry.find("required_children");
            if (required_it != candidate_entry.end() && required_it->is_array()) {
                for (const auto& child_entry : *required_it) {
                    if (!child_entry.is_string()) continue;
                    const std::string child_name = child_entry.get<std::string>();
                    append_unique(required_children, child_name);
                }
            }

            for (const std::string& resolved_room_name : resolved_rooms) {
                RoomSpec& spec = by_name[resolved_room_name];
                if (spec.name.empty()) {
                    spec.name = resolved_room_name;
                    spec.max_instances = 0;
                }
                spec.max_instances += max_instances;
                for (const std::string& child_name : required_children) {
                    append_unique(spec.required_children, child_name);
                }
            }
        }

        for (auto& [room_name, spec] : by_name) {
            if (spec.max_instances < 0) spec.max_instances = 0;
            if (spec.max_instances == 0 && layer_index == 0) {
                spec.max_instances = 1;
            }
            layer_spec.rooms.push_back(spec);
        }
        std::sort(layer_spec.rooms.begin(), layer_spec.rooms.end(), [](const RoomSpec& a, const RoomSpec& b) {
            return a.name < b.name;
        });

        if (layer_spec.max_rooms <= 0) {
            int sum = 0;
            for (const auto& rs : layer_spec.rooms) {
                sum += std::max(0, rs.max_instances);
            }
            layer_spec.max_rooms = sum;
        }
        if (layer_index == 0) {
            layer_spec.max_rooms = 1;
            if (layer_spec.rooms.empty()) {
                RoomSpec root;
                root.name = plan.spawn_room_name;
                root.max_instances = 1;
                layer_spec.rooms.push_back(root);
            }
            if (layer_spec.rooms.size() > 1) {
                layer_spec.rooms.resize(1);
            }
            layer_spec.rooms[0].max_instances = 1;
        }

        plan.resolved_layers.push_back(layer_spec);

        for (const RoomSpec& room_spec : layer_spec.rooms) {
            PlanNode node;
            node.id = next_node_id++;
            node.layer = layer_spec.level;
            node.room_name = room_spec.name;
            node.max_instances = room_spec.max_instances;
            node_by_layer_and_name[node_key(node.layer, node.room_name)] = node.id;
            plan.nodes.push_back(std::move(node));
        }
    }

    for (std::size_t layer_index = 0; layer_index + 1 < plan.resolved_layers.size(); ++layer_index) {
        const LayerSpec& layer_spec = plan.resolved_layers[layer_index];
        for (const RoomSpec& room_spec : layer_spec.rooms) {
            const auto from_it = node_by_layer_and_name.find(node_key(static_cast<int>(layer_index), room_spec.name));
            if (from_it == node_by_layer_and_name.end()) continue;
            const int from_id = from_it->second;
            for (const std::string& child_name : room_spec.required_children) {
                const auto to_it = node_by_layer_and_name.find(node_key(static_cast<int>(layer_index + 1), child_name));
                if (to_it == node_by_layer_and_name.end()) {
                    plan.diagnostics.push_back(
                        "error: required child '" + child_name + "' from room '" + room_spec.name +
                        "' is missing on next layer.");
                    continue;
                }
                const int to_id = to_it->second;
                bool edge_exists = std::any_of(plan.edges.begin(), plan.edges.end(), [&](const PlanEdge& edge) {
                    return edge.from_id == from_id && edge.to_id == to_id;
                });
                if (!edge_exists) {
                    plan.edges.push_back(PlanEdge{from_id, to_id, true});
                }
            }
        }
    }

    for (const PlanEdge& edge : plan.edges) {
        if (edge.from_id < 0 || edge.from_id >= static_cast<int>(plan.nodes.size()) ||
            edge.to_id < 0 || edge.to_id >= static_cast<int>(plan.nodes.size())) {
            continue;
        }
        plan.nodes[edge.from_id].child_ids.push_back(edge.to_id);
        plan.nodes[edge.to_id].parent_ids.push_back(edge.from_id);
    }

    plan.valid = true;
    for (const std::string& diagnostic : plan.diagnostics) {
        if (diagnostic.rfind("error:", 0) == 0) {
            plan.valid = false;
            break;
        }
    }
    if (plan.resolved_layers.empty()) {
        plan.valid = false;
        plan.diagnostics.push_back("error: no layers resolved in map graph plan.");
    }

    return plan;
}

} // namespace map_graph
