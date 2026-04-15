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
#include <random>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

using json = nlohmann::json;

namespace {
constexpr double kTau = 6.28318530717958647692;

template <typename ClockDuration>
double duration_ms(ClockDuration value) {
    return std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(value).count();
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
                                                        const nlohmann::json& boundary_data,
                                                        nlohmann::json& rooms_data,
                                                        nlohmann::json& trails_data,
                                                        const MapGridSettings& grid_settings) {
    (void)boundary_data;

    using clock = std::chrono::steady_clock;
    const auto build_start = clock::now();

    vibble::log::info(
        std::string("[GenerateRooms] Starting build for map '") +
        map_id_ + "' with " + std::to_string(map_layers_.size()) + " layers");

    std::vector<std::unique_ptr<Room>> all_rooms;
    if (map_layers_.empty()) {
        vibble::log::warn("[GenerateRooms] No layers to process, returning empty");
        return all_rooms;
    }

    if (map_layers_[0].rooms.empty()) {
        std::string fallback_name = "spawn";
        if (rooms_data.is_object()) {
            for (auto it = rooms_data.begin(); it != rooms_data.end(); ++it) {
                if (it.value().is_object() && it.value().value("is_spawn", false)) {
                    fallback_name = it.key();
                    break;
                }
            }
        }
        RoomSpec rs;
        rs.name = fallback_name;
        rs.max_instances = 1;
        map_layers_[0].rooms.push_back(rs);

        vibble::log::warn(
            std::string("[GenerateRooms] Layer 0 had no room spec. Using fallback root '") +
            fallback_name + "'");
    }

    const auto& root_spec = map_layers_[0].rooms[0];
    vibble::log::info(
        std::string("[GenerateRooms] Creating root room: ") + root_spec.name);

    if (!rooms_data.is_object()) {
        rooms_data = nlohmann::json::object();
        vibble::log::warn("[GenerateRooms] rooms_data was not an object. Reinitialized to empty object");
    }

    if (!rooms_data.contains(root_spec.name) || !rooms_data[root_spec.name].is_object()) {
        constexpr int kSpawnRadius = 1500;
        const int diameter = kSpawnRadius * 2;
        nlohmann::json entry = nlohmann::json::object();
        entry["name"] = root_spec.name;
        entry["geometry"] = "Circle";
        entry["min_width"] = diameter;
        entry["max_width"] = diameter;
        entry["min_height"] = diameter;
        entry["max_height"] = diameter;
        entry["edge_smoothness"] = 2;
        entry["is_spawn"] = true;
        entry["is_boss"] = false;
        entry["inherits_map_assets"] = false;
        entry["spawn_groups"] = nlohmann::json::array();
        rooms_data[root_spec.name] = std::move(entry);

        vibble::log::warn(
            std::string("[GenerateRooms] Root room data missing. Injected default spawn room for '") +
            root_spec.name + "'");
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

    const auto root_start = clock::now();
    auto root = std::make_unique<Room>(
        Room::Point{map_center_x_, map_center_z_},
        "room",
        root_spec.name,
        nullptr,
        map_id_,
        asset_lib,
        nullptr,
        get_room_data(root_spec.name),
        grid_settings,
        map_radius,
        "rooms_data",
        map_manifest_,
        manifest_store_,
        map_id_,
        manifest_writer_);
    root->layer = 0;
    all_rooms.push_back(std::move(root));
    const auto root_end = clock::now();

    vibble::log::info(
        std::string("[GenerateRooms] Root room created successfully in ") +
        std::to_string(duration_ms(root_end - root_start)) + "ms");

    std::vector<Room*> current_parents = {all_rooms[0].get()};
    std::vector<Sector> current_sectors = {{current_parents[0], 0.0f, static_cast<float>(kTau)}};

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

    for (size_t li = 1; li < map_layers_.size(); ++li) {
        const auto layer_start = clock::now();

        const LayerSpec& layer = map_layers_[li];
        const double radius = (li < layer_radii.size()) ? layer_radii[li] : 0.0;
        auto children_specs = get_children_from_layer(layer);

        vibble::log::info(
            std::string("[GenerateRooms] Processing layer ") +
            std::to_string(li) +
            " level=" + std::to_string(layer.level) +
            " radius=" + std::to_string(radius) +
            " children=" + std::to_string(children_specs.size()));

        if (testing) {
            vibble::log::debug(
                std::string("[GenerateRooms] Layer detail level=") +
                std::to_string(layer.level) +
                " radius=" + std::to_string(radius) +
                " children=" + std::to_string(children_specs.size()));
        }

        std::vector<Sector> next_sectors;
        std::vector<Room*> next_parents;

        if (li == 1) {
            if (!children_specs.empty()) {
                std::shuffle(children_specs.begin(), children_specs.end(), rng_);

                std::vector<double> extents;
                extents.reserve(children_specs.size());
                for (const auto& spec : children_specs) {
                    extents.push_back(room_extent_lookup(spec.name));
                }

                std::uniform_real_distribution<double> start_angle_dist(0.0, kTau);
                map_layers::RadialLayout layout =
                    map_layers::compute_radial_layout(radius, extents, min_edge_distance_, start_angle_dist(rng_));

                std::vector<double> angles = layout.angles;
                if (angles.size() != children_specs.size()) {
                    angles.resize(children_specs.size());
                    const double step = kTau / static_cast<double>(children_specs.size());
                    for (std::size_t idx = 0; idx < angles.size(); ++idx) {
                        angles[idx] = step * static_cast<double>(idx);
                    }
                    vibble::log::warn(
                        std::string("[GenerateRooms] Layer ") + std::to_string(li) +
                        " radial layout angle count mismatch. Used fallback evenly spaced angles");
                }

                const double used_radius = layout.radius;
                std::vector<double> placed_angles;
                placed_angles.reserve(children_specs.size());

                for (std::size_t i = 0; i < children_specs.size(); ++i) {
                    const double angle = angles[i];
                    SDL_Point pos = polar_to_cartesian(map_center_x_, map_center_z_, used_radius, static_cast<float>(angle));

                    if (testing) {
                        vibble::log::debug(
                            std::string("[GenerateRooms] Placing layer-1 child ") +
                            children_specs[i].name +
                            " at angle " + std::to_string(angle) +
                            " -> (" + std::to_string(pos.x) + ", " + std::to_string(pos.y) + ")");
                    }

                    auto child = std::make_unique<Room>(
                        Room::Point{pos.x, pos.y},
                        "room",
                        children_specs[i].name,
                        current_parents[0],
                        map_id_,
                        asset_lib,
                        nullptr,
                        get_room_data(children_specs[i].name),
                        grid_settings,
                        map_radius,
                        "rooms_data",
                        map_manifest_,
                        manifest_store_,
                        map_id_,
                        manifest_writer_);

                    child->layer = layer.level;
                    if (!next_parents.empty()) {
                        next_parents.back()->set_sibling_right(child.get());
                        child->set_sibling_left(next_parents.back());
                    }
                    current_parents[0]->children.push_back(child.get());
                    next_parents.push_back(child.get());
                    placed_angles.push_back(angle);
                    all_rooms.push_back(std::move(child));
                }

                append_sectors_from_angles(next_parents, placed_angles, next_sectors);
            }
        } else {
            std::unordered_map<Room*, std::vector<RoomSpec>> assignments;

            for (const auto& sec : current_sectors) {
                for (const auto& rs : map_layers_[li - 1].rooms) {
                    if (sec.room->room_name == rs.name) {
                        for (const auto& cname : rs.required_children) {
                            if (testing) {
                                vibble::log::debug(
                                    std::string("[GenerateRooms] Adding required child ") +
                                    cname + " for parent " + rs.name);
                            }
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
                auto it = std::min_element(counts.begin(), counts.end());
                int idx = static_cast<int>(std::distance(counts.begin(), it));
                assignments[parent_order[idx]].push_back(rs);
                counts[idx]++;
            }

            std::vector<RoomSpec> ordered_specs;
            std::vector<Room*> ordered_parents;
            ordered_specs.reserve(children_specs.size());
            ordered_parents.reserve(children_specs.size());

            for (auto& sec : current_sectors) {
                Room* parent = sec.room;
                auto& kids = assignments[parent];
                if (kids.empty()) continue;
                std::shuffle(kids.begin(), kids.end(), rng_);
                for (auto& spec : kids) {
                    ordered_specs.push_back(spec);
                    ordered_parents.push_back(parent);
                }
            }

            if (!ordered_specs.empty()) {
                std::vector<double> extents;
                extents.reserve(ordered_specs.size());
                for (const auto& spec : ordered_specs) {
                    extents.push_back(room_extent_lookup(spec.name));
                }

                std::uniform_real_distribution<double> start_angle_dist(0.0, kTau);
                map_layers::RadialLayout layout =
                    map_layers::compute_radial_layout(radius, extents, min_edge_distance_, start_angle_dist(rng_));

                std::vector<double> angles = layout.angles;
                if (angles.size() != ordered_specs.size()) {
                    angles.resize(ordered_specs.size());
                    const double step = kTau / static_cast<double>(ordered_specs.size());
                    for (std::size_t idx = 0; idx < angles.size(); ++idx) {
                        angles[idx] = step * static_cast<double>(idx);
                    }
                    vibble::log::warn(
                        std::string("[GenerateRooms] Layer ") + std::to_string(li) +
                        " ordered radial layout angle count mismatch. Used fallback evenly spaced angles");
                }

                const double used_radius = layout.radius;
                std::vector<double> placed_angles;
                placed_angles.reserve(ordered_specs.size());

                for (std::size_t idx = 0; idx < ordered_specs.size(); ++idx) {
                    Room* parent = ordered_parents[idx];
                    const double angle = angles[idx];
                    SDL_Point pos = polar_to_cartesian(map_center_x_, map_center_z_, used_radius, static_cast<float>(angle));

                    if (testing) {
                        vibble::log::debug(
                            std::string("[GenerateRooms] Placing child ") +
                            ordered_specs[idx].name +
                            " under parent " + parent->room_name +
                            " at angle " + std::to_string(angle) +
                            " -> (" + std::to_string(pos.x) + ", " + std::to_string(pos.y) + ")");
                    }

                    auto child = std::make_unique<Room>(
                        Room::Point{pos.x, pos.y},
                        "room",
                        ordered_specs[idx].name,
                        parent,
                        map_id_,
                        asset_lib,
                        nullptr,
                        get_room_data(ordered_specs[idx].name),
                        grid_settings,
                        map_radius,
                        "rooms_data",
                        map_manifest_,
                        manifest_store_,
                        map_id_,
                        manifest_writer_);

                    child->layer = layer.level;
                    if (!next_parents.empty()) {
                        next_parents.back()->set_sibling_right(child.get());
                        child->set_sibling_left(next_parents.back());
                    }
                    parent->children.push_back(child.get());
                    next_parents.push_back(child.get());
                    placed_angles.push_back(angle);
                    all_rooms.push_back(std::move(child));
                }

                append_sectors_from_angles(next_parents, placed_angles, next_sectors);
            }
        }

        current_parents = next_parents;
        current_sectors = next_sectors;

        const auto layer_end = clock::now();
        vibble::log::info(
            std::string("[GenerateRooms] Layer ") +
            std::to_string(li) +
            " completed total_rooms=" + std::to_string(all_rooms.size()) +
            " layer_ms=" + std::to_string(duration_ms(layer_end - layer_start)));
    }

    const auto connection_build_start = clock::now();
    std::vector<std::pair<Room*, Room*>> connections;
    for (auto& rp : all_rooms) {
        for (Room* c : rp->children) {
            connections.emplace_back(rp.get(), c);
        }
    }
    const auto connection_build_end = clock::now();

    vibble::log::info(
        std::string("[GenerateRooms] Parent-child connections established: ") +
        std::to_string(connections.size()) +
        " build_ms=" + std::to_string(duration_ms(connection_build_end - connection_build_start)));

    vibble::log::debug(
        std::string("[GenerateRooms] Existing room references prepared for trail generation total_rooms_pre_trail=") +
        std::to_string(all_rooms.size()));

    const auto trail_start = clock::now();
    vibble::log::info("[GenerateRooms] Beginning trail generation");

    if (all_rooms.size() > 1) {
        GenerateTrails trailgen(trails_data, room_colors);

        std::vector<Room*> room_refs;
        room_refs.reserve(all_rooms.size());
        for (auto& room_ptr : all_rooms) {
            room_refs.push_back(room_ptr.get());
        }

        vibble::log::debug(
            std::string("[GenerateRooms] Passing ") +
            std::to_string(room_refs.size()) +
            " room refs into GenerateTrails");

        trailgen.set_all_rooms_reference(room_refs);
        auto trail_objects = trailgen.generate_trails(
            connections,
            map_id_,
            asset_lib,
            map_radius,
            map_manifest_,
            manifest_store_,
            manifest_writer_);

        vibble::log::info(
            std::string("[GenerateRooms] Trail generation returned ") +
            std::to_string(trail_objects.size()) + " trail rooms");

        for (auto& t : trail_objects) {
            all_rooms.push_back(std::move(t));
        }
    } else {
        vibble::log::warn("[GenerateRooms] Skipping trail generation because there is only one room");
    }

    const auto trail_end = clock::now();
    vibble::log::info(
        std::string("[GenerateRooms] Trail generation complete total_rooms=") +
        std::to_string(all_rooms.size()) +
        " trail_ms=" + std::to_string(duration_ms(trail_end - trail_start)));

    const auto build_end = clock::now();
    vibble::log::info(
        std::string("[GenerateRooms] Build completed successfully total_rooms=") +
        std::to_string(all_rooms.size()) +
        " total_ms=" + std::to_string(duration_ms(build_end - build_start)));

    return all_rooms;
}