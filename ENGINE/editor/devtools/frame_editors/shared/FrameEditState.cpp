#include "FrameEditState.hpp"

#include <algorithm>
#include <cmath>

namespace devmode::frame_editors {

MovementFrame clamp_frame(const MovementFrame& in) {
    MovementFrame f = in;
    if (!std::isfinite(f.dx)) f.dx = 0.0f;
    if (!std::isfinite(f.dy)) f.dy = 0.0f;
    if (!std::isfinite(f.dz)) f.dz = 0.0f;
    return f;
}

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

    const nlohmann::json movement =
        (payload.contains("movement") && payload["movement"].is_array())
            ? payload["movement"]
            : nlohmann::json::array();
    if (movement.empty()) {
        frames.push_back(MovementFrame{});
        return frames;
    }

    for (const auto& entry : movement) {
        MovementFrame f{};
        if (entry.is_array()) {
            if (!entry.empty() && entry[0].is_number()) {
                f.dx = static_cast<float>(entry[0].get<double>());
            }
            if (entry.size() > 1 && entry[1].is_number()) {
                f.dy = static_cast<float>(entry[1].get<double>());
            }
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
        frames.push_back(clamp_frame(f));
    }

    if (frames.empty()) {
        frames.push_back(MovementFrame{});
    }
    return frames;
}

namespace {

nlohmann::json normalize_frame_array_key(const nlohmann::json& payload,
                                         const char* key,
                                         std::size_t frame_count) {
    nlohmann::json value = (payload.contains(key) && payload[key].is_array())
                               ? payload[key]
                               : nlohmann::json::array();
    if (value.size() < frame_count) {
        value.get_ref<nlohmann::json::array_t&>().resize(frame_count, nlohmann::json::array());
    } else if (value.size() > frame_count) {
        value.erase(value.begin() + static_cast<std::ptrdiff_t>(frame_count), value.end());
    }
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (!value[i].is_array()) {
            value[i] = nlohmann::json::array();
        }
    }
    return value;
}

}  // namespace

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
                const bool looks_color = node.size() == 3 &&
                                         node[0].is_number() &&
                                         node[1].is_number() &&
                                         node[2].is_number();
                if (looks_color && !has_color) {
                    preserved_color = node;
                    has_color = true;
                }
            }
        }

        entry = nlohmann::json::array();
        entry.push_back(static_cast<int>(std::lround(f.dx)));
        entry.push_back(static_cast<int>(std::lround(f.dy)));
        entry.push_back(static_cast<int>(std::lround(f.dz)));
        if (f.resort_z || had_resort) {
            entry.push_back(f.resort_z);
        }
        if (has_color) {
            entry.push_back(std::move(preserved_color));
        }
        movement[static_cast<nlohmann::json::array_t::size_type>(i)] = std::move(entry);
    }

    int total_dx = 0;
    int total_dy = 0;
    double total_dz = 0.0;
    for (std::size_t i = 1; i < movement.size(); ++i) {
        const auto& entry = movement[i];
        if (!entry.is_array()) {
            continue;
        }
        if (!entry.empty() && entry[0].is_number_integer()) {
            total_dx += entry[0].get<int>();
        } else if (!entry.empty() && entry[0].is_number()) {
            total_dx += static_cast<int>(std::lround(entry[0].get<double>()));
        }
        if (entry.size() > 1 && entry[1].is_number_integer()) {
            total_dy += entry[1].get<int>();
        } else if (entry.size() > 1 && entry[1].is_number()) {
            total_dy += static_cast<int>(std::lround(entry[1].get<double>()));
        }
        if (entry.size() > 2 && entry[2].is_number()) {
            total_dz += entry[2].get<double>();
        }
    }

    if (movement.empty()) {
        movement.push_back(nlohmann::json::array({0, 0}));
    }
    payload["movement"] = std::move(movement);
    payload["movement_total"] = nlohmann::json{{"dx", total_dx}, {"dy", total_dy}, {"dz", total_dz}};

    payload["anchor_points"] = normalize_frame_array_key(payload, "anchor_points", frame_count);
    payload["hit_boxes"] = normalize_frame_array_key(payload, "hit_boxes", frame_count);
    payload["attack_boxes"] = normalize_frame_array_key(payload, "attack_boxes", frame_count);
    payload.erase("hit_geometry");
    payload.erase("attack_geometry");

    return payload;
}

}  // namespace devmode::frame_editors
