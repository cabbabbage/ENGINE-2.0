#include "FrameEditState.hpp"

#include <algorithm>
#include <cmath>
#include <string>

namespace devmode::frame_editors {

MovementFrame clamp_frame(const MovementFrame& in) {
    MovementFrame f = in;
    if (!std::isfinite(f.dx)) f.dx = 0.0f;
    if (!std::isfinite(f.dy)) f.dy = 0.0f;
    if (!std::isfinite(f.dz)) f.dz = 0.0f;
    return f;
}

namespace {

float read_float(const nlohmann::json& v, float fallback = 0.0f) {
    if (v.is_number()) {
        try {
            return static_cast<float>(v.get<double>());
        } catch (...) {
        }
    }
    if (v.is_string()) {
        try {
            return std::stof(v.get<std::string>());
        } catch (...) {
        }
    }
    return fallback;
}

int read_int(const nlohmann::json& v, int fallback = 0) {
    if (v.is_number_integer()) {
        try {
            return v.get<int>();
        } catch (...) {
        }
    } else if (v.is_number()) {
        try {
            return static_cast<int>(v.get<double>());
        } catch (...) {
        }
    } else if (v.is_string()) {
        try {
            return std::stoi(v.get<std::string>());
        } catch (...) {
        }
    }
    return fallback;
}

void upsert_hit_box(MovementFrame& frame, const std::string& type, const nlohmann::json& node) {
    if (type.empty() || node.is_null()) {
        return;
    }
    animation_update::FrameHitGeometry::HitBox box;
    box.type = type;
    if (node.is_object()) {
        box.center_x = read_float(node.value("center_x", 0.0f));
        box.center_y = read_float(node.value("center_y", 0.0f));
        box.center_z = read_float(node.value("center_z", 0.0f));
        box.half_width = read_float(node.value("half_width", 0.0f));
        box.half_height = read_float(node.value("half_height", 0.0f));
        box.rotation_degrees = read_float(node.value("rotation", node.value("rotation_degrees", 0.0f)));
        if (node.contains("type") && node["type"].is_string()) {
            box.type = node["type"].get<std::string>();
        }
    } else if (node.is_array()) {
        const auto& arr = node;
        if (!arr.empty()) box.center_x = read_float(arr[0]);
        if (arr.size() > 1) box.center_y = read_float(arr[1]);
        if (arr.size() > 5 && arr[2].is_number() && arr[3].is_number() && arr[4].is_number()) {
            box.center_z = read_float(arr[2]);
            box.half_width = read_float(arr[3]);
            box.half_height = read_float(arr[4]);
            if (arr.size() > 5 && arr[5].is_number()) {
                box.rotation_degrees = read_float(arr[5]);
            }
        } else {
            if (arr.size() > 2) box.half_width = read_float(arr[2]);
            if (arr.size() > 3) box.half_height = read_float(arr[3]);
            if (arr.size() > 4 && arr[4].is_number()) {
                box.rotation_degrees = read_float(arr[4]);
            } else if (arr.size() > 5 && arr[5].is_number()) {
                box.rotation_degrees = read_float(arr[5]);
            }
        }
        if (arr.size() > 4 && arr[4].is_boolean() && !arr[4].get<bool>()) {
            return;
        }
    } else {
        return;
    }
    if (box.is_empty()) {
        return;
    }
    auto it = std::find_if(frame.hit.boxes.begin(), frame.hit.boxes.end(), [&](const auto& b) { return b.type == box.type; });
    if (it != frame.hit.boxes.end()) {
        *it = box;
    } else {
        frame.hit.boxes.push_back(box);
    }
}

void append_attack_vector(MovementFrame& frame, const std::string& type, const nlohmann::json& node) {
    if (type.empty() || node.is_null()) return;
    animation_update::FrameAttackGeometry::Vector vec;
    vec.type = type;
    if (node.is_object()) {
        vec.start_x = read_float(node.value("start_x", 0.0f));
        vec.start_y = read_float(node.value("start_y", 0.0f));
        vec.start_z = read_float(node.value("start_z", 0.0f));
        if (node.contains("control_x") || node.contains("control_y")) {
            vec.control_x = read_float(node.value("control_x", (vec.start_x)));
            vec.control_y = read_float(node.value("control_y", (vec.start_y)));
        } else {
            vec.control_x = (vec.start_x + read_float(node.value("end_x", 0.0f))) * 0.5f;
            vec.control_y = (vec.start_y + read_float(node.value("end_y", 0.0f))) * 0.5f;
        }
        vec.control_z = read_float(node.value("control_z", vec.start_z));
        vec.end_x = read_float(node.value("end_x", 0.0f));
        vec.end_y = read_float(node.value("end_y", 0.0f));
        vec.end_z = read_float(node.value("end_z", 0.0f));
        vec.damage = read_int(node.value("damage", 0));
        if (node.contains("type") && node["type"].is_string()) {
            vec.type = node["type"].get<std::string>();
        }
    } else if (node.is_array()) {
        const auto& arr = node;
        if (!arr.empty()) vec.start_x = read_float(arr[0]);
        if (arr.size() > 1) vec.start_y = read_float(arr[1]);
        if (arr.size() > 2) vec.end_x = read_float(arr[2]);
        if (arr.size() > 3) vec.end_y = read_float(arr[3]);
        vec.control_x = (vec.start_x + vec.end_x) * 0.5f;
        vec.control_y = (vec.start_y + vec.end_y) * 0.5f;
        if (arr.size() > 4) vec.damage = read_int(arr[4]);
    } else {
        return;
    }
    frame.attack.add_vector(vec);
}

}  // namespace

std::vector<MovementFrame> parse_frames_from_payload(const std::string& payload_json) {
    nlohmann::json payload = nlohmann::json::parse(payload_json, nullptr, false);
    return parse_frames_from_payload(payload);
}

std::vector<MovementFrame> parse_frames_from_payload(const nlohmann::json& payload) {
    std::vector<MovementFrame> frames;
    if (!payload.is_object()) {
        frames.push_back(MovementFrame{});
        return frames;
    }
    nlohmann::json movement = nlohmann::json::array();
    if (payload.contains("movement")) movement = payload["movement"];
    if (!movement.is_array() || movement.empty()) {
        frames.push_back(MovementFrame{});
        return frames;
    }

    nlohmann::json hit_geom = nlohmann::json::array();
    if (payload.contains("hit_geometry")) hit_geom = payload["hit_geometry"];
    if (!hit_geom.is_array()) hit_geom = nlohmann::json::array();

    nlohmann::json attack_geom = nlohmann::json::array();
    if (payload.contains("attack_geometry")) attack_geom = payload["attack_geometry"];
    if (!attack_geom.is_array()) attack_geom = nlohmann::json::array();

    std::size_t frame_index = 0;
    for (const auto& entry : movement) {
        MovementFrame f{};
        if (entry.is_array()) {
            if (!entry.empty() && entry[0].is_number()) f.dx = static_cast<float>(entry[0].get<double>());
            if (entry.size() > 1 && entry[1].is_number()) f.dy = static_cast<float>(entry[1].get<double>());
            if (entry.size() > 2 && entry[2].is_number()) {
                f.dz = static_cast<float>(entry[2].get<double>());
            }
            if (entry.size() > 2 && entry[2].is_boolean()) {
                f.resort_z = entry[2].get<bool>();
            } else if (entry.size() > 3 && entry[3].is_boolean()) {
                f.resort_z = entry[3].get<bool>();
            }

        } else if (entry.is_object()) {
            f.dx = static_cast<float>(entry.value("dx", 0.0));
            f.dy = static_cast<float>(entry.value("dy", 0.0));
            f.dz = static_cast<float>(entry.value("dz", 0.0));
            f.resort_z = entry.value("resort_z", false);
        }

        f.hit.boxes.clear();
        if (hit_geom.is_array() && frame_index < hit_geom.size()) {
            const auto& hit_entry = hit_geom[static_cast<nlohmann::json::size_type>(frame_index)];
            if (hit_entry.is_object()) {
                for (const char* type : kDamageTypeNames) {
                    auto it = hit_entry.find(type);
                    if (it != hit_entry.end()) {
                        upsert_hit_box(f, type, *it);
                    }
                }
            } else if (!hit_entry.is_null()) {
                upsert_hit_box(f, "melee", hit_entry);
            }
        }

        f.attack.vectors.clear();
        if (attack_geom.is_array() && frame_index < attack_geom.size()) {
            const auto& attack_entry = attack_geom[static_cast<nlohmann::json::size_type>(frame_index)];
            if (attack_entry.is_array()) {
                for (const auto& vec_node : attack_entry) {
                    append_attack_vector(f, "", vec_node);
                }
            } else if (attack_entry.is_object()) {
                // Legacy support for typed attacks
                for (const char* type : kDamageTypeNames) {
                    auto it = attack_entry.find(type);
                    if (it == attack_entry.end() || !it->is_array()) continue;
                    for (const auto& vec_node : *it) {
                        append_attack_vector(f, type, vec_node);
                    }
                }
            }
        }

        frames.push_back(clamp_frame(f));
        ++frame_index;
    }
    if (frames.empty()) frames.push_back(MovementFrame{});
    return frames;
}

nlohmann::json build_payload_from_frames(const std::vector<MovementFrame>& frames,
                                         const nlohmann::json& existing_payload) {
    nlohmann::json payload = existing_payload;
    if (!payload.is_object()) {
        payload = nlohmann::json::object();
    }

    const std::size_t frame_count = frames.size();

    nlohmann::json movement = (payload.contains("movement") && payload["movement"].is_array())
                                  ? payload["movement"]
                                  : nlohmann::json::array();
    if (movement.size() < frame_count) {
        movement.get_ref<nlohmann::json::array_t&>().resize(frame_count, nlohmann::json::array({0, 0}));
    } else if (movement.size() > frame_count) {
        movement.erase(movement.begin() + static_cast<std::ptrdiff_t>(frame_count), movement.end());
    }

    for (std::size_t i = 0; i < frame_count; ++i) {
        const MovementFrame& f = frames[i];
        nlohmann::json entry = movement[static_cast<nlohmann::json::array_t::size_type>(i)];
        if (!entry.is_array()) {
            entry = nlohmann::json::array();
        }
        if (entry.size() < 2) {
            entry = nlohmann::json::array({0, 0});
        }
        nlohmann::json preserved_color;
        bool has_color = false;
        bool had_resort = false;
        if (entry.is_array()) {
            for (const auto& node : entry) {
                if (node.is_boolean()) {
                    had_resort = true;
                }
                if (!node.is_array()) {
                    continue;
                }
                const bool looks_color = node.size() == 3 && node[0].is_number() && node[1].is_number() && node[2].is_number();
                if (looks_color && !has_color) {
                    preserved_color = node;
                    has_color = true;
                }
            }
        }
        entry = nlohmann::json::array();
        entry.push_back(static_cast<int>(std::lround(f.dx)));
        entry.push_back(static_cast<int>(std::lround(f.dy)));
        entry.push_back(static_cast<double>(f.dz));
        if (f.resort_z || had_resort) {
            entry.push_back(f.resort_z);
        }
        if (has_color) {
            entry.push_back(std::move(preserved_color));
        }
        movement[static_cast<nlohmann::json::array_t::size_type>(i)] = std::move(entry);
    }

    nlohmann::json hit_geometry = (payload.contains("hit_geometry") && payload["hit_geometry"].is_array())
                                      ? payload["hit_geometry"]
                                      : nlohmann::json::array();
    if (hit_geometry.size() < frame_count) {
        hit_geometry.get_ref<nlohmann::json::array_t&>().resize(frame_count, nlohmann::json::object());
    } else if (hit_geometry.size() > frame_count) {
        hit_geometry.erase(hit_geometry.begin() + static_cast<std::ptrdiff_t>(frame_count), hit_geometry.end());
    }
    for (std::size_t i = 0; i < frame_count; ++i) {
        const MovementFrame& f = frames[i];
        nlohmann::json existing = hit_geometry[static_cast<nlohmann::json::array_t::size_type>(i)];
        if (!existing.is_object()) existing = nlohmann::json::object();
        auto preserved_needs_rebuild = existing.contains("needs_rebuild") ? existing["needs_rebuild"] : nlohmann::json();

        for (const char* type : kDamageTypeNames) {
            auto it = std::find_if(f.hit.boxes.begin(), f.hit.boxes.end(), [&](const auto& b) { return b.type == type; });
            const auto* box = (it != f.hit.boxes.end()) ? &*it : nullptr;
            if (!box || box->is_empty() || !std::isfinite(box->center_x) || !std::isfinite(box->center_y) ||
                !std::isfinite(box->center_z) || !std::isfinite(box->half_width) ||
                !std::isfinite(box->half_height) || !std::isfinite(box->rotation_degrees)) {
                existing[type] = nullptr;
                continue;
            }
            existing[type] = nlohmann::json{
                {"center_x", box->center_x},
                {"center_y", box->center_y},
                {"center_z", box->center_z},
                {"half_width", box->half_width},
                {"half_height", box->half_height},
                {"rotation", box->rotation_degrees},
                {"type", type}
            };
        }
        if (preserved_needs_rebuild.is_boolean()) {
            existing["needs_rebuild"] = preserved_needs_rebuild;
        }
        hit_geometry[static_cast<nlohmann::json::array_t::size_type>(i)] = std::move(existing);
    }

    nlohmann::json attack_geometry = (payload.contains("attack_geometry") && payload["attack_geometry"].is_array())
                                         ? payload["attack_geometry"]
                                         : nlohmann::json::array();
    if (attack_geometry.size() < frame_count) {
        attack_geometry.get_ref<nlohmann::json::array_t&>().resize(frame_count, nlohmann::json::object());
    } else if (attack_geometry.size() > frame_count) {
        attack_geometry.erase(attack_geometry.begin() + static_cast<std::ptrdiff_t>(frame_count), attack_geometry.end());
    }
    for (std::size_t i = 0; i < frame_count; ++i) {
        const MovementFrame& f = frames[i];
        nlohmann::json attack_array = nlohmann::json::array();
        for (const auto& vec : f.attack.vectors) {
            if (!std::isfinite(vec.start_x) || !std::isfinite(vec.start_y) ||
                !std::isfinite(vec.end_x) || !std::isfinite(vec.end_y) ||
                !std::isfinite(vec.control_x) || !std::isfinite(vec.control_y)) {
                continue;
            }
            attack_array.push_back(nlohmann::json{
                {"start_x", vec.start_x},
                {"start_y", vec.start_y},
                {"start_z", vec.start_z},
                {"control_x", vec.control_x},
                {"control_y", vec.control_y},
                {"control_z", vec.control_z},
                {"end_x", vec.end_x},
                {"end_y", vec.end_y},
                {"end_z", vec.end_z},
                {"damage", vec.damage},
                {"type", vec.type}
            });
        }
        attack_geometry[static_cast<nlohmann::json::array_t::size_type>(i)] = std::move(attack_array);
    }

    int total_dx = 0;
    int total_dy = 0;
    double total_dz = 0.0;
    for (std::size_t i = 1; i < movement.size(); ++i) {
        const auto& entry = movement[i];
        if (entry.is_array()) {
            if (entry.size() > 0 && entry[0].is_number_integer()) total_dx += entry[0].get<int>();
            else if (entry.size() > 0 && entry[0].is_number()) total_dx += static_cast<int>(std::lround(entry[0].get<double>()));
            if (entry.size() > 1 && entry[1].is_number_integer()) total_dy += entry[1].get<int>();
            else if (entry.size() > 1 && entry[1].is_number()) total_dy += static_cast<int>(std::lround(entry[1].get<double>()));
            if (entry.size() > 2 && entry[2].is_number()) total_dz += entry[2].get<double>();
        }
    }

    if (movement.empty()) movement.push_back(nlohmann::json::array({0, 0}));
    payload["movement"] = std::move(movement);
    payload["movement_total"] = nlohmann::json{{"dx", total_dx}, {"dy", total_dy}, {"dz", total_dz}};
    payload["hit_geometry"] = std::move(hit_geometry);
    payload["attack_geometry"] = std::move(attack_geometry);
    return payload;
}

}  // namespace devmode::frame_editors
