#pragma once

#include "room.hpp"
#include "utils/area.hpp"
#include "assets/asset/asset_library.hpp"
#include "map_layers_geometry.hpp"
#include <vector>
#include <string>
#include <memory>
#include <random>
#include <unordered_map>
#include <cstdint>
#include <limits>
#include <cmath>
#include <algorithm>
#include <SDL3/SDL.h>
#include <nlohmann/json.hpp>

#include "utils/map_grid_settings.hpp"

namespace devmode::core {
class ManifestStore;
}

struct RoomSpec {
        std::string name;
        int max_instances;
        std::vector<std::string> required_children;
};

struct LayerSpec {
        int level = 0;
        int max_rooms = 0;
        std::vector<RoomSpec> rooms;
};

namespace spatial_index_detail {
inline int div_floor(int value, int divisor) {
        if (divisor == 0) return 0;
        if (value >= 0) return value / divisor;
        return -static_cast<int>((static_cast<long long>(-value) + divisor - 1) / divisor);
}

inline std::int64_t bucket_key(int x, int y) {
        return (static_cast<std::int64_t>(x) << 32) ^ static_cast<std::int64_t>(static_cast<std::uint32_t>(y));
}
}

class RoomSpatialIndex {
public:
        explicit RoomSpatialIndex(const std::vector<std::unique_ptr<Room>>& rooms, int bucket_size = 2048, int max_radius = 8)
        : bucket_size_(std::max(1, bucket_size)), max_radius_(std::max(1, max_radius)) {
                entries_.reserve(rooms.size());
                for (const auto& room_ptr : rooms) {
                        Room* room = room_ptr.get();
                        if (!room || !room->room_area) continue;
                        SDL_Point center = room->room_area->get_center();
                        entries_.push_back(RoomEntry{room, center});
                        const int bx = spatial_index_detail::div_floor(center.x, bucket_size_);
                        const int by = spatial_index_detail::div_floor(center.y, bucket_size_);
                        buckets_[spatial_index_detail::bucket_key(bx, by)].push_back(&entries_.back());
                }
        }

        explicit RoomSpatialIndex(const std::vector<Room*>& rooms, int bucket_size = 2048, int max_radius = 8)
        : bucket_size_(std::max(1, bucket_size)), max_radius_(std::max(1, max_radius)) {
                entries_.reserve(rooms.size());
                for (Room* room : rooms) {
                        if (!room || !room->room_area) continue;
                        SDL_Point center = room->room_area->get_center();
                        entries_.push_back(RoomEntry{room, center});
                        const int bx = spatial_index_detail::div_floor(center.x, bucket_size_);
                        const int by = spatial_index_detail::div_floor(center.y, bucket_size_);
                        buckets_[spatial_index_detail::bucket_key(bx, by)].push_back(&entries_.back());
                }
        }

        Room* find_owner(SDL_Point pt) const {
                const RoomEntry* best = nullptr;
                double best_dist_sq = std::numeric_limits<double>::max();
                const int base_bx = spatial_index_detail::div_floor(pt.x, bucket_size_);
                const int base_by = spatial_index_detail::div_floor(pt.y, bucket_size_);

                auto consider_bucket = [&](int bx, int by) -> bool {
                        auto it = buckets_.find(spatial_index_detail::bucket_key(bx, by));
                        if (it == buckets_.end()) return false;
                        for (const RoomEntry* entry : it->second) {
                                if (!entry || !entry->room || !entry->room->room_area) continue;
                                if (entry->room->room_area->contains_point(pt)) {
                                        best = entry;
                                        best_dist_sq = 0.0;
                                        return true;
                                }
                                const double dx = static_cast<double>(pt.x - entry->center.x);
                                const double dy = static_cast<double>(pt.y - entry->center.y);
                                const double dist_sq = dx * dx + dy * dy;
                                if (dist_sq < best_dist_sq) {
                                        best_dist_sq = dist_sq;
                                        best = entry;
                                }
                        }
                        return false;
                };

                for (int radius = 0; radius <= max_radius_; ++radius) {
                        for (int by = base_by - radius; by <= base_by + radius; ++by) {
                                for (int bx = base_bx - radius; bx <= base_bx + radius; ++bx) {
                                        if (consider_bucket(bx, by) && best_dist_sq == 0.0) {
                                                return best ? best->room : nullptr;
                                        }
                                }
                        }
                        if (best) return best->room;
                }

                if (!best) {
                        for (const RoomEntry& entry : entries_) {
                                if (!entry.room || !entry.room->room_area) continue;
                                if (entry.room->room_area->contains_point(pt)) return entry.room;
                                const double dx = static_cast<double>(pt.x - entry.center.x);
                                const double dy = static_cast<double>(pt.y - entry.center.y);
                                const double dist_sq = dx * dx + dy * dy;
                                if (!best || dist_sq < best_dist_sq) {
                                        best = &entry;
                                        best_dist_sq = dist_sq;
                                }
                        }
                }
                return best ? best->room : nullptr;
        }

        std::vector<std::pair<double, Room*>> find_k_nearest(SDL_Point pt, int k) const {
                std::vector<std::pair<double, Room*>> candidates;
                const int base_bx = spatial_index_detail::div_floor(pt.x, bucket_size_);
                const int base_by = spatial_index_detail::div_floor(pt.y, bucket_size_);

                for (int radius = 0; radius <= max_radius_ && static_cast<int>(candidates.size()) < k * 2; ++radius) {
                        for (int by = base_by - radius; by <= base_by + radius; ++by) {
                                for (int bx = base_bx - radius; bx <= base_bx + radius; ++bx) {
                                        if (radius > 0 && std::abs(bx - base_bx) < radius && std::abs(by - base_by) < radius)
                                                continue;
                                        auto it = buckets_.find(spatial_index_detail::bucket_key(bx, by));
                                        if (it == buckets_.end()) continue;
                                        for (const RoomEntry* entry : it->second) {
                                                if (!entry || !entry->room) continue;
                                                double dx = pt.x - entry->center.x;
                                                double dy = pt.y - entry->center.y;
                                                candidates.emplace_back(dx * dx + dy * dy, entry->room);
                                        }
                                }
                        }
                }

                if (static_cast<int>(candidates.size()) > k) {
                        std::partial_sort(candidates.begin(), candidates.begin() + k, candidates.end(),
                                [](const auto& a, const auto& b) { return a.first < b.first; });
                        candidates.resize(k);
                } else {
                        std::sort(candidates.begin(), candidates.end(),
                                [](const auto& a, const auto& b) { return a.first < b.first; });
                }
                return candidates;
        }

private:
        struct RoomEntry {
                Room* room = nullptr;
                SDL_Point center{0, 0};
        };

        int bucket_size_ = 2048;
        int max_radius_ = 8;
        std::vector<RoomEntry> entries_;
        mutable std::unordered_map<std::int64_t, std::vector<const RoomEntry*>> buckets_;
};

class GenerateRooms {

	public:
    using Point = SDL_Point;
    GenerateRooms(const std::vector<LayerSpec>& layers,
                  int map_cx,
                  int map_cz,
                  const std::string& map_id,
                  nlohmann::json& map_manifest,
                  double min_edge_distance,
                  devmode::core::ManifestStore* manifest_store = nullptr,
                  Room::ManifestWriter manifest_writer = {});
    std::vector<std::unique_ptr<Room>> build(AssetLibrary* asset_lib, double map_radius, const std::vector<double>& layer_radii, const nlohmann::json& boundary_data, nlohmann::json& rooms_data, nlohmann::json& trails_data, nlohmann::json& map_assets_data, const MapGridSettings& grid_settings);
    bool testing = false;

	private:
    struct Sector {
    Room* room;
    float start_angle;
    float span_angle;
};
    SDL_Point polar_to_cartesian(int cx, int cy, double radius, float angle_rad);
    std::vector<RoomSpec> get_children_from_layer(const LayerSpec& layer);
    std::vector<LayerSpec> map_layers_;
    int map_center_x_;
    int map_center_z_;
    std::string map_id_;
    nlohmann::json* map_manifest_ = nullptr;
    devmode::core::ManifestStore* manifest_store_ = nullptr;
    Room::ManifestWriter manifest_writer_{};
    std::mt19937 rng_;
    double min_edge_distance_ = static_cast<double>(map_layers::kDefaultMinEdgeDistance);
};
