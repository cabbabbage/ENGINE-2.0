#include "FrameEditState.hpp"

#include <algorithm>
#include <cmath>

namespace devmode::frame_editors {

MovementFrame clamp_frame(const MovementFrame& in) {
    MovementFrame f = in;
    if (!std::isfinite(f.dx)) f.dx = 0.0f;
    if (!std::isfinite(f.dy)) f.dy = 0.0f;
    if (!std::isfinite(f.dz)) f.dz = 0.0f;
    if (!std::isfinite(f.rotation_degrees)) f.rotation_degrees = 0.0f;
    return f;
}

std::vector<MovementFrame> parse_frames_from_payload(const std::string& payload_json) {
    nlohmann::json payload = nlohmann::json::parse(payload_json, nullptr, false);
    return parse_frames_from_payload(payload);
}

namespace {

std::vector<MovementFrame> parse_frame_sequence(const nlohmann::json& movement) {
    std::vector<MovementFrame> frames;
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
                // Canonical arrays are [dx, y(height), z(depth)].
                // Legacy arrays may only include [dx, depth].
                if (entry.size() > 2 && entry[2].is_number()) {
                    f.dy = static_cast<float>(entry[1].get<double>());
                } else {
                    f.dy = 0.0f;
                    f.dz = static_cast<float>(entry[1].get<double>());
                }
            }
            if (entry.size() > 2 && entry[2].is_number()) {
                f.dz = static_cast<float>(entry[2].get<double>());
            }
            if (entry.size() > 3 && entry[3].is_number()) {
                f.rotation_degrees = static_cast<float>(entry[3].get<double>());
            }
            for (std::size_t i = 3; i < entry.size(); ++i) {
                if (entry[i].is_boolean()) {
                    f.resort_z = entry[i].get<bool>();
                    break;
                }
            }
        } else if (entry.is_object()) {
            auto read_number = [&](const char* key, float fallback) -> float {
                if (!entry.contains(key) || !entry[key].is_number()) {
                    return fallback;
                }
                return static_cast<float>(entry[key].get<double>());
            };
            f.dx = entry.contains("dx") ? read_number("dx", 0.0f) : read_number("x", 0.0f);

            const bool has_depth_key = entry.contains("dz") || entry.contains("z");
            const bool has_height_key = entry.contains("dy") || entry.contains("y");
            float parsed_height = entry.contains("dy") ? read_number("dy", 0.0f) : read_number("y", 0.0f);
            float parsed_depth = entry.contains("dz") ? read_number("dz", 0.0f) : read_number("z", 0.0f);

            const bool has_xy_keys = entry.contains("x") || entry.contains("y") || entry.contains("z");
            const bool has_dxyz_keys = entry.contains("dx") || entry.contains("dy") || entry.contains("dz");
            const bool legacy_depth_in_height =
                (!has_depth_key && has_height_key) ||
                (has_depth_key && has_height_key && has_xy_keys && has_dxyz_keys &&
                 std::abs(parsed_depth) < 1e-5f && std::abs(parsed_height) > 1e-5f);
            if (legacy_depth_in_height) {
                parsed_depth = parsed_height;
                parsed_height = 0.0f;
            }
            f.dy = parsed_height;
            f.dz = parsed_depth;
            f.rotation_degrees = read_number("rotation_degrees", 0.0f);
            f.resort_z = entry.value("resort_z", false);
        }
        frames.push_back(clamp_frame(f));
    }

    if (frames.empty()) {
        frames.push_back(MovementFrame{});
    }
    return frames;
}

struct MovementTotals {
    int dx = 0;
    int dy = 0;
    double dz = 0.0;
    double dr = 0.0;
};

nlohmann::json build_movement_sequence_json(const std::vector<MovementFrame>& frames,
                                            const nlohmann::json& existing_sequence,
                                            MovementTotals& totals) {
    const std::size_t frame_count = frames.size();
    nlohmann::json movement = existing_sequence.is_array() ? existing_sequence : nlohmann::json::array();
    if (movement.size() < frame_count) {
        movement.get_ref<nlohmann::json::array_t&>().resize(frame_count, nlohmann::json::array({0, 0, 0}));
    } else if (movement.size() > frame_count) {
        movement.erase(movement.begin() + static_cast<std::ptrdiff_t>(frame_count), movement.end());
    }

    for (std::size_t i = 0; i < frame_count; ++i) {
        const MovementFrame& f = frames[i];
        nlohmann::json entry = movement[static_cast<nlohmann::json::array_t::size_type>(i)];
        if (!entry.is_array()) {
            entry = nlohmann::json::array();
        }
        if (entry.size() < 3) {
            entry = nlohmann::json::array({0, 0, 0});
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
        entry.push_back(f.rotation_degrees);
        if (f.resort_z || had_resort) {
            entry.push_back(f.resort_z);
        }
        if (has_color) {
            entry.push_back(std::move(preserved_color));
        }
        movement[static_cast<nlohmann::json::array_t::size_type>(i)] = std::move(entry);
    }

    for (std::size_t i = 0; i < movement.size(); ++i) {
        const auto& entry = movement[i];
        if (!entry.is_array()) {
            continue;
        }
        if (!entry.empty() && entry[0].is_number_integer()) {
            totals.dx += entry[0].get<int>();
        } else if (!entry.empty() && entry[0].is_number()) {
            totals.dx += static_cast<int>(std::lround(entry[0].get<double>()));
        }
        if (entry.size() > 1 && entry[1].is_number_integer()) {
            totals.dy += entry[1].get<int>();
        } else if (entry.size() > 1 && entry[1].is_number()) {
            totals.dy += static_cast<int>(std::lround(entry[1].get<double>()));
        }
        if (entry.size() > 2 && entry[2].is_number()) {
            totals.dz += entry[2].get<double>();
        }
        if (entry.size() > 3 && entry[3].is_number()) {
            totals.dr += entry[3].get<double>();
        }
    }

    if (movement.empty()) {
        movement.push_back(nlohmann::json::array({0, 0, 0}));
    }
    return movement;
}

nlohmann::json totals_json(const MovementTotals& totals) {
    return nlohmann::json{{"dx", totals.dx}, {"dy", totals.dy}, {"dz", totals.dz}, {"dr", totals.dr}};
}

}  // namespace

std::vector<MovementFrame> parse_frames_from_payload(const nlohmann::json& payload) {
    std::vector<std::vector<MovementFrame>> paths = parse_movement_paths_from_payload(payload);
    if (paths.empty()) {
        paths.emplace_back();
        paths.back().push_back(MovementFrame{});
    }
    if (paths.front().empty()) {
        paths.front().push_back(MovementFrame{});
    }
    return paths.front();
}

std::vector<std::vector<MovementFrame>> parse_movement_paths_from_payload(const nlohmann::json& payload) {
    std::vector<std::vector<MovementFrame>> paths;
    if (!payload.is_object()) {
        paths.push_back(std::vector<MovementFrame>{MovementFrame{}});
        return paths;
    }

    if (payload.contains("movement_paths") && payload["movement_paths"].is_array()) {
        for (const auto& path : payload["movement_paths"]) {
            if (path.is_array()) {
                paths.push_back(parse_frame_sequence(path));
            }
        }
    }

    if (paths.empty() && payload.contains("movement") && payload["movement"].is_array()) {
        paths.push_back(parse_frame_sequence(payload["movement"]));
    }

    if (paths.empty()) {
        paths.push_back(std::vector<MovementFrame>{MovementFrame{}});
    }
    for (auto& path : paths) {
        if (path.empty()) {
            path.push_back(MovementFrame{});
        }
    }
    return paths;
}

nlohmann::json build_payload_from_frames(const std::vector<MovementFrame>& frames,
                                         const nlohmann::json& existing_payload) {
    return build_payload_from_movement_paths(std::vector<std::vector<MovementFrame>>{frames}, existing_payload);
}

nlohmann::json build_payload_from_movement_paths(const std::vector<std::vector<MovementFrame>>& paths,
                                                 const nlohmann::json& existing_payload) {
    nlohmann::json payload = existing_payload;
    if (!payload.is_object()) {
        payload = nlohmann::json::object();
    }

    std::vector<std::vector<MovementFrame>> safe_paths = paths;
    if (safe_paths.empty()) {
        safe_paths.push_back(std::vector<MovementFrame>{MovementFrame{}});
    }

    const nlohmann::json existing_paths =
        (payload.contains("movement_paths") && payload["movement_paths"].is_array())
            ? payload["movement_paths"]
            : nlohmann::json::array();

    nlohmann::json movement_paths = nlohmann::json::array();
    nlohmann::json movement_totals = nlohmann::json::array();
    MovementTotals primary_totals{};

    for (std::size_t path_index = 0; path_index < safe_paths.size(); ++path_index) {
        if (safe_paths[path_index].empty()) {
            safe_paths[path_index].push_back(MovementFrame{});
        }
        const auto& path_frames = safe_paths[path_index];
        MovementTotals path_totals{};
        const nlohmann::json existing_sequence =
            (path_index < existing_paths.size() && existing_paths[path_index].is_array())
                ? existing_paths[path_index]
                : nlohmann::json::array();
        movement_paths.push_back(build_movement_sequence_json(path_frames, existing_sequence, path_totals));
        movement_totals.push_back(totals_json(path_totals));
        if (path_index == 0) {
            primary_totals = path_totals;
        }
    }

    payload["movement_paths"] = std::move(movement_paths);
    payload["movement_total"] = totals_json(primary_totals);
    payload["movement_totals"] = std::move(movement_totals);
    payload.erase("movement");

    return payload;
}

}  // namespace devmode::frame_editors
