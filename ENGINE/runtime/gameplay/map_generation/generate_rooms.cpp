#include "generate_rooms.hpp"
#include "generate_trails.hpp"
#include "utils/display_color.hpp"
#include "utils/log.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>
#include <nlohmann/json.hpp>
#include <queue>
#include <random>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

using json = nlohmann::json;

namespace {
constexpr double kTau = 6.28318530717958647692;
constexpr int kLayoutAttemptLimit = 12;

template <typename ClockDuration>
double duration_ms(ClockDuration value) {
    return std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(value).count();
}

bool is_trail_room(const Room* room) {
    return room && room->type == "trail";
}

bool rooms_overlap(const Room& a, const Room& b) {
    if (!a.room_area || !b.room_area) {
        return false;
    }
    auto [a_min_x, a_min_y, a_max_x, a_max_y] = a.room_area->get_bounds();
    auto [b_min_x, b_min_y, b_max_x, b_max_y] = b.room_area->get_bounds();
    return !(a_max_x < b_min_x || b_max_x < a_min_x || a_max_y < b_min_y || b_max_y < a_min_y);
}

void shift_room(Room& room, int dx, int dy) {
    room.map_origin.first += dx;
    room.map_origin.second += dy;
    if (room.room_area) {
        room.room_area->apply_offset(dx, dy);
    }
}

void resolve_scratch_room_overlaps(std::vector<std::unique_ptr<Room>>& rooms, int center_x, int center_y, double min_edge_distance) {
    const int base_shift = std::max(64, static_cast<int>(std::ceil(std::max(0.0, min_edge_distance) * 0.5)));
    for (int iter = 0; iter < 8; ++iter) {
        bool changed = false;
        for (std::size_t i = 0; i < rooms.size(); ++i) {
            for (std::size_t j = i + 1; j < rooms.size(); ++j) {
                if (!rooms[i] || !rooms[j] || !rooms_overlap(*rooms[i], *rooms[j])) {
                    continue;
                }
                Room& room = *rooms[j];
                const SDL_Point room_center = room.room_area ? room.room_area->get_center()
                                                              : SDL_Point{room.map_origin.first, room.map_origin.second};
                double dx = static_cast<double>(room_center.x - center_x);
                double dy = static_cast<double>(room_center.y - center_y);
                double len = std::hypot(dx, dy);
                if (len <= 1.0) {
                    dx = 1.0;
                    dy = 0.0;
                    len = 1.0;
                }
                const int shift_x = static_cast<int>(std::lround((dx / len) * base_shift - (dy / len) * base_shift * 0.35));
                const int shift_y = static_cast<int>(std::lround((dy / len) * base_shift + (dx / len) * base_shift * 0.35));
                shift_room(room, shift_x == 0 ? base_shift : shift_x, shift_y);
                changed = true;
            }
        }
        if (!changed) {
            break;
        }
    }
}

bool graph_reaches_all_non_trail(Room* spawn, const std::vector<Room*>& rooms) {
    if (!spawn) {
        return false;
    }
    std::unordered_set<Room*> seen;
    std::queue<Room*> queue;
    seen.insert(spawn);
    queue.push(spawn);
    while (!queue.empty()) {
        Room* current = queue.front();
        queue.pop();
        if (!current) {
            continue;
        }
        for (Room* next : current->connected_rooms) {
            if (next && seen.insert(next).second) {
                queue.push(next);
            }
        }
    }
    for (Room* room : rooms) {
        if (!room || is_trail_room(room)) {
            continue;
        }
        if (seen.find(room) == seen.end()) {
            return false;
        }
    }
    return true;
}

int count_reachable_non_trail(Room* spawn, const std::vector<Room*>& rooms) {
    if (!spawn) {
        return 0;
    }
    std::unordered_set<Room*> seen;
    std::queue<Room*> queue;
    seen.insert(spawn);
    queue.push(spawn);
    while (!queue.empty()) {
        Room* current = queue.front();
        queue.pop();
        if (!current) {
            continue;
        }
        for (Room* next : current->connected_rooms) {
            if (next && seen.insert(next).second) {
                queue.push(next);
            }
        }
    }

    int count = 0;
    for (Room* room : rooms) {
        if (room && !is_trail_room(room) && seen.find(room) != seen.end()) {
            ++count;
        }
    }
    return count;
}

double bounds_radius_from_rooms(int center_x,
                                int center_y,
                                const std::vector<std::unique_ptr<Room>>& rooms,
                                const std::vector<std::unique_ptr<Room>>& trails) {
    double radius = 1.0;
    auto consider = [&](const Room* room) {
        if (!room || !room->room_area) {
            return;
        }
        auto [min_x, min_y, max_x, max_y] = room->room_area->get_bounds();
        radius = std::max(radius, std::abs(static_cast<double>(min_x - center_x)));
        radius = std::max(radius, std::abs(static_cast<double>(max_x - center_x)));
        radius = std::max(radius, std::abs(static_cast<double>(min_y - center_y)));
        radius = std::max(radius, std::abs(static_cast<double>(max_y - center_y)));
    };
    for (const auto& room : rooms) {
        consider(room.get());
    }
    for (const auto& trail : trails) {
        consider(trail.get());
    }
    return radius + map_layers::kMapRadiusOuterPadding;
}
}

GenerateRooms::GenerateRooms(const std::vector<LayerSpec>& layers,
                             int map_cx,
                             int map_cz,
                             const std::string& map_id,
                             nlohmann::json& map_manifest,
                             double min_edge_distance,
                             devmode::core::ManifestStore* manifest_store,
                             Room::ManifestWriter manifest_writer)
: map_layers_(layers),
  map_center_x_(map_cx),
  map_center_z_(map_cz),
  map_id_(map_id),
  map_manifest_(&map_manifest),
  manifest_store_(manifest_store),
  manifest_writer_(std::move(manifest_writer)),
  rng_(std::random_device{}()),
  min_edge_distance_(std::max(0.0, min_edge_distance))
{}

SDL_Point GenerateRooms::polar_to_cartesian(int cx, int cz, double radius, float angle_rad) {
    const double x = static_cast<double>(cx) + std::cos(angle_rad) * radius;
    const double z = static_cast<double>(cz) + std::sin(angle_rad) * radius;
    return SDL_Point{static_cast<int>(std::lround(x)), static_cast<int>(std::lround(z))};
}

std::vector<RoomSpec> GenerateRooms::get_children_from_layer(const LayerSpec& layer) {
    std::vector<RoomSpec> result;
    const int target = std::max(0, layer.max_rooms);

    if (testing) {
        vibble::log::debug(
            std::string("[GenerateRooms] Building layer ") +
            std::to_string(layer.level) +
            " targeting " + std::to_string(target) + " rooms");
    }

    if (target == 0) {
        return result;
    }

    std::vector<RoomSpec> candidates;
    for (const auto& r : layer.rooms) {
        const int max_instances = std::max(0, r.max_instances);
        if (testing) {
            vibble::log::debug(
                std::string("[GenerateRooms] Room type: ") +
                r.name + " count: " + std::to_string(max_instances));
        }
        for (int i = 0; i < max_instances; ++i) {
            candidates.push_back(r);
        }
    }

    if (candidates.empty()) {
        return result;
    }

    std::shuffle(candidates.begin(), candidates.end(), rng_);
    if (static_cast<int>(candidates.size()) <= target) {
        return candidates;
    }

    result.insert(result.end(), candidates.begin(), candidates.begin() + target);
    return result;
}

std::vector<std::unique_ptr<Room>> GenerateRooms::build(AssetLibrary* asset_lib,
                                                        double map_radius,
                                                        const std::vector<double>& layer_radii,
                                                        const nlohmann::json& live_dynamic_spawns_data,
                                                        nlohmann::json& rooms_data,
                                                        nlohmann::json& trails_data,
                                                        const MapGridSettings& grid_settings) {
    (void)live_dynamic_spawns_data;

    using clock = std::chrono::steady_clock;
    const auto build_start = clock::now();

    vibble::log::info(
        std::string("[GenerateRooms] Starting build for map '") +
        map_id_ + "' with " + std::to_string(map_layers_.size()) + " layers");

    std::vector<std::unique_ptr<Room>> empty_result;
    if (map_layers_.empty()) {
        vibble::log::warn("[GenerateRooms] No layers to process, returning empty");
        return empty_result;
    }

    if (map_layers_[0].rooms.empty()) {
        vibble::log::error("[GenerateRooms] Layer 0 has no center-room spec. Aborting generation.");
        return empty_result;
    }

    const auto& root_spec = map_layers_[0].rooms[0];
    vibble::log::info(
        std::string("[GenerateRooms] Creating root room: ") + root_spec.name);

    if (!rooms_data.is_object()) {
        rooms_data = nlohmann::json::object();
        vibble::log::warn("[GenerateRooms] rooms_data was not an object. Reinitialized to empty object");
    }

    if (!rooms_data.contains(root_spec.name) || !rooms_data[root_spec.name].is_object()) {
        vibble::log::error(
            std::string("[GenerateRooms] Root room data missing for center-room '") +
            root_spec.name + "'. Aborting generation.");
        return empty_result;
    }

    std::vector<SDL_Color> room_colors = utils::display_color::collect(rooms_data);

    auto get_room_data = [&](const std::string& name) -> nlohmann::json* {
        if (!rooms_data.is_object()) return nullptr;
        nlohmann::json& entry = rooms_data[name];
        bool mutated = false;
        utils::display_color::ensure(entry, room_colors, &mutated);
        if (mutated && testing) {
            vibble::log::debug(
                std::string("[GenerateRooms] Assigned display color to room template '") +
                name + "'");
        }
        return &entry;
    };

    const nlohmann::json* rooms_data_lookup = rooms_data.is_object() ? &rooms_data : nullptr;
    auto room_extent_lookup = [&](const std::string& room_name) {
        double extent = map_layers::room_extent_from_rooms_data(rooms_data_lookup, room_name);
        return (extent > 0.0) ? extent : 1.0;
    };

    auto append_sectors_from_angles = [](const std::vector<Room*>& rooms,
                                         const std::vector<double>& angles,
                                         std::vector<Sector>& out) {
        if (rooms.empty() || rooms.size() != angles.size()) {
            return;
        }
        if (rooms.size() == 1) {
            out.push_back({rooms[0], 0.0f, static_cast<float>(kTau)});
            return;
        }
        for (std::size_t idx = 0; idx < rooms.size(); ++idx) {
            const double current = angles[idx];
            const double prev = (idx == 0) ? angles.back() - kTau : angles[idx - 1];
            const double next = (idx + 1 == angles.size()) ? angles.front() + kTau : angles[idx + 1];
            const double prev_gap = current - prev;
            const double next_gap = next - current;
            const double start = current - prev_gap * 0.5;
            const double span = (prev_gap + next_gap) * 0.5;
            out.push_back({rooms[idx], static_cast<float>(start), static_cast<float>(span)});
        }
    };

    struct ScratchBuild {
        std::vector<std::unique_ptr<Room>> rooms;
        std::vector<std::pair<Room*, Room*>> required_connections;
    };

    struct FallbackBuild {
        ScratchBuild scratch;
        TrailGenerationResult trail_result;
        bool has = false;
        int attempt_index = -1;
        int required_successes = -1;
        int reachable_non_trail = -1;
        std::size_t trail_count = 0;
    };

    FallbackBuild best_fallback;

    auto remember_fallback = [&](ScratchBuild&& scratch,
                                 TrailGenerationResult&& trail_result,
                                 int attempt_index,
                                 int required_successes,
                                 int reachable_non_trail) {
        const std::size_t trail_count = trail_result.trail_rooms.size();
        bool better = !best_fallback.has;
        if (!better && required_successes != best_fallback.required_successes) {
            better = required_successes > best_fallback.required_successes;
        }
        if (!better && reachable_non_trail != best_fallback.reachable_non_trail) {
            better = reachable_non_trail > best_fallback.reachable_non_trail;
        }
        if (!better && trail_count != best_fallback.trail_count) {
            better = trail_count > best_fallback.trail_count;
        }
        if (!better && scratch.rooms.size() != best_fallback.scratch.rooms.size()) {
            better = scratch.rooms.size() > best_fallback.scratch.rooms.size();
        }
        if (!better) {
            return;
        }

        trail_result.all_required_connected = false;
        best_fallback.scratch = std::move(scratch);
        best_fallback.trail_result = std::move(trail_result);
        best_fallback.has = true;
        best_fallback.attempt_index = attempt_index;
        best_fallback.required_successes = required_successes;
        best_fallback.reachable_non_trail = reachable_non_trail;
        best_fallback.trail_count = trail_count;
    };

    auto make_scratch_room = [&](const std::string& name,
                                 SDL_Point pos,
                                 Room* parent,
                                 int layer,
                                 std::vector<std::unique_ptr<Room>>& rooms) -> Room* {
        auto room = std::make_unique<Room>(
            Room::Point{pos.x, pos.y},
            "room",
            name,
            parent,
            map_id_,
            nullptr,
            nullptr,
            get_room_data(name),
            grid_settings,
            map_radius,
            "rooms_data",
            nullptr,
            nullptr,
            std::string{},
            Room::ManifestWriter{},
            false);
        room->layer = layer;
        Room* raw = room.get();
        rooms.push_back(std::move(room));
        return raw;
    };

    std::unordered_set<std::string> failed_route_room_names;

    auto build_scratch = [&](int attempt_index) {
        ScratchBuild scratch;
        scratch.rooms.reserve(32);

        Room* root = make_scratch_room(root_spec.name,
                                       SDL_Point{map_center_x_, map_center_z_},
                                       nullptr,
                                       0,
                                       scratch.rooms);
        std::vector<Room*> current_parents = {root};
        std::vector<Sector> current_sectors = {{root, 0.0f, static_cast<float>(kTau)}};

        const double retry_outward = static_cast<double>(attempt_index) * std::max(128.0, min_edge_distance_ * 0.5);
        const double retry_angle = static_cast<double>(attempt_index) * 0.173;

        for (size_t li = 1; li < map_layers_.size(); ++li) {
            const LayerSpec& layer = map_layers_[li];
            const double base_radius = (li < layer_radii.size()) ? layer_radii[li] : 0.0;
            const double radius = base_radius + retry_outward;
            auto children_specs = get_children_from_layer(layer);

            vibble::log::info(
                std::string("[GenerateRooms] Draft layer ") +
                std::to_string(li) +
                " level=" + std::to_string(layer.level) +
                " radius=" + std::to_string(radius) +
                " children=" + std::to_string(children_specs.size()) +
                " attempt=" + std::to_string(attempt_index + 1));

            std::vector<Sector> next_sectors;
            std::vector<Room*> next_parents;

            auto place_ordered_specs = [&](const std::vector<RoomSpec>& ordered_specs,
                                           const std::vector<Room*>& ordered_parents) {
                if (ordered_specs.empty()) {
                    return;
                }

                std::vector<double> extents;
                extents.reserve(ordered_specs.size());
                for (const auto& spec : ordered_specs) {
                    extents.push_back(room_extent_lookup(spec.name));
                }

                std::uniform_real_distribution<double> start_angle_dist(0.0, kTau);
                map_layers::RadialLayout layout =
                    map_layers::compute_radial_layout(radius, extents, min_edge_distance_, start_angle_dist(rng_) + retry_angle);

                std::vector<double> angles = layout.angles;
                if (angles.size() != ordered_specs.size()) {
                    angles.resize(ordered_specs.size());
                    const double step = kTau / static_cast<double>(ordered_specs.size());
                    for (std::size_t idx = 0; idx < angles.size(); ++idx) {
                        angles[idx] = step * static_cast<double>(idx) + retry_angle;
                    }
                    vibble::log::warn(
                        std::string("[GenerateRooms] Layer ") + std::to_string(li) +
                        " radial layout angle count mismatch. Used fallback evenly spaced angles");
                }

                const double used_radius = layout.radius;
                std::vector<double> placed_angles;
                placed_angles.reserve(ordered_specs.size());

                for (std::size_t idx = 0; idx < ordered_specs.size(); ++idx) {
                    Room* parent = ordered_parents[idx];
                    double angle = angles[idx];
                    if (failed_route_room_names.find(ordered_specs[idx].name) != failed_route_room_names.end()) {
                        const double direction = (idx % 2 == 0) ? 1.0 : -1.0;
                        const double tangential_cap =
                            std::min(0.45, (room_extent_lookup(ordered_specs[idx].name) + min_edge_distance_) /
                                           std::max(1.0, used_radius));
                        angle += direction * tangential_cap * static_cast<double>(attempt_index + 1);
                    }
                    const double extra_radius =
                        failed_route_room_names.find(ordered_specs[idx].name) != failed_route_room_names.end()
                            ? std::min(std::max(128.0, min_edge_distance_), retry_outward + min_edge_distance_)
                            : 0.0;
                    SDL_Point pos = polar_to_cartesian(
                        map_center_x_, map_center_z_, used_radius + extra_radius, static_cast<float>(angle));

                    Room* child = make_scratch_room(ordered_specs[idx].name, pos, parent, layer.level, scratch.rooms);
                    if (!next_parents.empty()) {
                        next_parents.back()->set_sibling_right(child);
                        child->set_sibling_left(next_parents.back());
                    }
                    if (parent) {
                        parent->children.push_back(child);
                        scratch.required_connections.emplace_back(parent, child);
                    }
                    next_parents.push_back(child);
                    placed_angles.push_back(angle);
                }

                append_sectors_from_angles(next_parents, placed_angles, next_sectors);
            };

            if (li == 1) {
                if (!children_specs.empty()) {
                    std::shuffle(children_specs.begin(), children_specs.end(), rng_);
                    std::vector<Room*> ordered_parents(children_specs.size(), current_parents.empty() ? nullptr : current_parents[0]);
                    place_ordered_specs(children_specs, ordered_parents);
                }
            } else {
                std::unordered_map<Room*, std::vector<RoomSpec>> assignments;

                for (const auto& sec : current_sectors) {
                    for (const auto& rs : map_layers_[li - 1].rooms) {
                        if (sec.room->room_name == rs.name) {
                            for (const auto& cname : rs.required_children) {
                                assignments[sec.room].push_back({cname, 1, {}});
                            }
                        }
                    }
                }

                std::vector<Room*> parent_order;
                for (auto& sec : current_sectors) {
                    parent_order.push_back(sec.room);
                }

                std::vector<int> counts(parent_order.size(), 0);
                for (auto& rs : children_specs) {
                    if (parent_order.empty()) {
                        break;
                    }
                    auto it = std::min_element(counts.begin(), counts.end());
                    int idx = static_cast<int>(std::distance(counts.begin(), it));
                    assignments[parent_order[idx]].push_back(rs);
                    counts[idx]++;
                }

                std::vector<RoomSpec> ordered_specs;
                std::vector<Room*> ordered_parents;
                for (auto& sec : current_sectors) {
                    Room* parent = sec.room;
                    auto& kids = assignments[parent];
                    if (kids.empty()) {
                        continue;
                    }
                    std::shuffle(kids.begin(), kids.end(), rng_);
                    for (auto& spec : kids) {
                        ordered_specs.push_back(spec);
                        ordered_parents.push_back(parent);
                    }
                }
                place_ordered_specs(ordered_specs, ordered_parents);
            }

            current_parents = next_parents;
            current_sectors = next_sectors;
        }

        resolve_scratch_room_overlaps(scratch.rooms, map_center_x_, map_center_z_, min_edge_distance_);
        return scratch;
    };

    auto commit_from_scratch = [&](ScratchBuild& scratch, TrailGenerationResult& trail_result) {
        const double final_map_radius =
            std::max(map_radius, bounds_radius_from_rooms(map_center_x_, map_center_z_, scratch.rooms, trail_result.trail_rooms));
        std::vector<std::unique_ptr<Room>> committed;
        committed.reserve(scratch.rooms.size() + trail_result.trail_rooms.size());
        std::unordered_map<Room*, Room*> remap;
        remap.reserve(scratch.rooms.size());

        for (const auto& scratch_room : scratch.rooms) {
            if (!scratch_room || !scratch_room->room_area) {
                continue;
            }
            Room* final_parent = nullptr;
            if (scratch_room->parent) {
                auto parent_it = remap.find(scratch_room->parent);
                final_parent = parent_it == remap.end() ? nullptr : parent_it->second;
            }
            Area precomputed(scratch_room->room_name, scratch_room->room_area->get_points(), scratch_room->room_area->resolution());
            auto room = std::make_unique<Room>(
                scratch_room->map_origin,
                "room",
                scratch_room->room_name,
                final_parent,
                map_id_,
                asset_lib,
                &precomputed,
                get_room_data(scratch_room->room_name),
                grid_settings,
                final_map_radius,
                "rooms_data",
                map_manifest_,
                manifest_store_,
                map_id_,
                manifest_writer_);
            room->layer = scratch_room->layer;
            remap[scratch_room.get()] = room.get();
            committed.push_back(std::move(room));
        }

        for (const auto& scratch_room : scratch.rooms) {
            auto room_it = remap.find(scratch_room.get());
            if (room_it == remap.end()) {
                continue;
            }
            Room* final_room = room_it->second;
            final_room->children.clear();
            for (Room* child : scratch_room->children) {
                auto child_it = remap.find(child);
                if (child_it != remap.end()) {
                    final_room->children.push_back(child_it->second);
                }
            }
            if (scratch_room->left_sibling) {
                auto left_it = remap.find(scratch_room->left_sibling);
                final_room->left_sibling = left_it == remap.end() ? nullptr : left_it->second;
            }
            if (scratch_room->right_sibling) {
                auto right_it = remap.find(scratch_room->right_sibling);
                final_room->right_sibling = right_it == remap.end() ? nullptr : right_it->second;
            }
        }

        auto find_trail_data = [&](const std::string& name) -> nlohmann::json* {
            if (trails_data.is_object()) {
                auto it = trails_data.find(name);
                if (it != trails_data.end() && it->is_object()) {
                    return &(*it);
                }
                for (auto fallback = trails_data.begin(); fallback != trails_data.end(); ++fallback) {
                    if (fallback->is_object()) {
                        return &(*fallback);
                    }
                }
            }
            return nullptr;
        };

        for (const auto& scratch_trail : trail_result.trail_rooms) {
            if (!scratch_trail || !scratch_trail->room_area) {
                continue;
            }
            std::vector<Room*> endpoints;
            for (Room* connected : scratch_trail->connected_rooms) {
                auto it = remap.find(connected);
                if (it != remap.end() && it->second) {
                    endpoints.push_back(it->second);
                }
            }
            if (endpoints.size() < 2) {
                continue;
            }

            Area precomputed(scratch_trail->room_name,
                             scratch_trail->room_area->get_points(),
                             scratch_trail->room_area->resolution());
            nlohmann::json* trail_data = find_trail_data(scratch_trail->room_name);
            auto trail = std::make_unique<Room>(
                endpoints[0]->map_origin,
                "trail",
                scratch_trail->room_name,
                nullptr,
                map_id_,
                asset_lib,
                &precomputed,
                trail_data,
                MapGridSettings::defaults(),
                final_map_radius,
                "trails_data",
                map_manifest_,
                manifest_store_,
                map_id_,
                manifest_writer_);
            trail->camera_height_px = scratch_trail->camera_height_px;
            trail->camera_tilt_deg = scratch_trail->camera_tilt_deg;
            trail->camera_zoom_percent = scratch_trail->camera_zoom_percent;
            trail->camera_center_dx = scratch_trail->camera_center_dx;
            trail->camera_center_dz = scratch_trail->camera_center_dz;

            endpoints[0]->add_connecting_room(trail.get());
            endpoints[1]->add_connecting_room(trail.get());
            trail->add_connecting_room(endpoints[0]);
            trail->add_connecting_room(endpoints[1]);
            committed.push_back(std::move(trail));
        }

        return committed;
    };

    for (int attempt = 0; attempt < kLayoutAttemptLimit; ++attempt) {
        const auto attempt_start = clock::now();
        ScratchBuild scratch = build_scratch(attempt);

        vibble::log::info(
            std::string("[GenerateRooms] Draft layout built") +
            " attempt=" + std::to_string(attempt + 1) + "/" + std::to_string(kLayoutAttemptLimit) +
            " rooms=" + std::to_string(scratch.rooms.size()) +
            " required_connections=" + std::to_string(scratch.required_connections.size()));

        if (scratch.rooms.size() <= 1) {
            TrailGenerationResult empty_trails;
            empty_trails.all_required_connected = true;
            auto committed = commit_from_scratch(scratch, empty_trails);
            vibble::log::info("[GenerateRooms] Build completed with a single room");
            return committed;
        }

        TrailGenerationResult trail_result;
        try {
            GenerateTrails trailgen(trails_data, room_colors);
            std::vector<Room*> room_refs;
            room_refs.reserve(scratch.rooms.size());
            for (auto& room_ptr : scratch.rooms) {
                room_refs.push_back(room_ptr.get());
            }
            trailgen.set_all_rooms_reference(room_refs);
            trail_result = trailgen.generate_trails(
                scratch.required_connections,
                map_id_,
                nullptr,
                map_radius,
                nullptr,
                nullptr,
                Room::ManifestWriter{});
        } catch (const std::exception& ex) {
            vibble::log::error(std::string("[GenerateRooms] Trail generation setup failed: ") + ex.what());
            TrailGenerationResult empty_trails;
            auto committed = commit_from_scratch(scratch, empty_trails);
            if (!committed.empty()) {
                vibble::log::warn(
                    std::string("[GenerateRooms] Committed degraded room layout without trails after setup failure") +
                    " rooms=" + std::to_string(committed.size()));
                return committed;
            }
            return empty_result;
        }

        std::vector<Room*> connectivity_refs;
        connectivity_refs.reserve(scratch.rooms.size() + trail_result.trail_rooms.size());
        for (auto& room_ptr : scratch.rooms) {
            connectivity_refs.push_back(room_ptr.get());
        }
        for (auto& trail_ptr : trail_result.trail_rooms) {
            connectivity_refs.push_back(trail_ptr.get());
        }

        const int reachable_non_trail = count_reachable_non_trail(scratch.rooms.front().get(), connectivity_refs);
        const int required_successes = std::max(
            0,
            static_cast<int>(scratch.required_connections.size()) -
                static_cast<int>(trail_result.required_failures.size()));
        const bool connected = trail_result.required_failures.empty() &&
                               graph_reaches_all_non_trail(scratch.rooms.front().get(), connectivity_refs);
        trail_result.all_required_connected = connected;

        if (connected) {
            auto committed = commit_from_scratch(scratch, trail_result);
            const auto build_end = clock::now();
            vibble::log::info(
                std::string("[GenerateRooms] Build completed successfully") +
                " attempt=" + std::to_string(attempt + 1) +
                " total_rooms=" + std::to_string(committed.size()) +
                " optional_skips=" + std::to_string(trail_result.optional_skips.size()) +
                " total_ms=" + std::to_string(duration_ms(build_end - build_start)));
            return committed;
        }

        failed_route_room_names.clear();
        for (const TrailConnectionFailure& failure : trail_result.required_failures) {
            if (failure.b) {
                failed_route_room_names.insert(failure.b->room_name);
            }
            vibble::log::warn(
                std::string("[GenerateRooms] Required trail failed") +
                " attempt=" + std::to_string(attempt + 1) +
                " rooms=" + (failure.a ? failure.a->room_name : std::string("<null>")) +
                "<->" + (failure.b ? failure.b->room_name : std::string("<null>")) +
                " reason=" + failure.reason);
        }
        if (failed_route_room_names.empty()) {
            for (const auto& connection : scratch.required_connections) {
                if (connection.second) {
                    failed_route_room_names.insert(connection.second->room_name);
                    break;
                }
            }
        }

        vibble::log::warn(
            std::string("[GenerateRooms] Draft layout rejected") +
            " attempt=" + std::to_string(attempt + 1) +
            " connected=" + std::string(connected ? "true" : "false") +
            " required_successes=" + std::to_string(required_successes) +
            " reachable_non_trail=" + std::to_string(reachable_non_trail) +
            " attempt_ms=" + std::to_string(duration_ms(clock::now() - attempt_start)));

        remember_fallback(std::move(scratch),
                          std::move(trail_result),
                          attempt,
                          required_successes,
                          reachable_non_trail);
    }

    if (best_fallback.has) {
        auto committed = commit_from_scratch(best_fallback.scratch, best_fallback.trail_result);
        if (!committed.empty()) {
            const auto build_end = clock::now();
            vibble::log::warn(
                std::string("[GenerateRooms] Failed to produce a fully connected map after bounded layout attempts; committed degraded layout") +
                " attempt=" + std::to_string(best_fallback.attempt_index + 1) +
                " total_rooms=" + std::to_string(committed.size()) +
                " required_successes=" + std::to_string(best_fallback.required_successes) +
                " reachable_non_trail=" + std::to_string(best_fallback.reachable_non_trail) +
                " trails=" + std::to_string(best_fallback.trail_count) +
                " total_ms=" + std::to_string(duration_ms(build_end - build_start)));
            return committed;
        }
    }

    vibble::log::error("[GenerateRooms] Failed to produce a fully connected map after bounded layout attempts");
    return empty_result;
}
