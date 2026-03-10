#include "animation/attack_validation.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <string>
#include <vector>

#include "assets/asset/Asset.hpp"

namespace animation_update {

namespace {

constexpr float kAxisEpsilon = 1e-6f;

struct Vec3 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

struct AABB3 {
    Vec3 min{};
    Vec3 max{};
    bool valid = false;
};

Vec3 to_vec3(const Asset::RuntimeBoxPoint3& p) {
    return Vec3{p.x, p.y, p.z};
}

Vec3 subtract(const Vec3& a, const Vec3& b) {
    return Vec3{a.x - b.x, a.y - b.y, a.z - b.z};
}

Vec3 cross(const Vec3& a, const Vec3& b) {
    return Vec3{
        a.y * b.z - a.z * b.y,
        a.z * b.x - a.x * b.z,
        a.x * b.y - a.y * b.x};
}

float dot(const Vec3& a, const Vec3& b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

float length_sq(const Vec3& v) {
    return dot(v, v);
}

bool normalize_axis(Vec3& axis) {
    const float len_sq = length_sq(axis);
    if (len_sq <= kAxisEpsilon * kAxisEpsilon || !std::isfinite(len_sq)) {
        return false;
    }
    const float inv_len = 1.0f / std::sqrt(len_sq);
    axis.x *= inv_len;
    axis.y *= inv_len;
    axis.z *= inv_len;
    return std::isfinite(axis.x) && std::isfinite(axis.y) && std::isfinite(axis.z);
}

void extend_aabb(AABB3& aabb, const Vec3& point) {
    if (!aabb.valid) {
        aabb.min = point;
        aabb.max = point;
        aabb.valid = true;
        return;
    }
    aabb.min.x = std::min(aabb.min.x, point.x);
    aabb.min.y = std::min(aabb.min.y, point.y);
    aabb.min.z = std::min(aabb.min.z, point.z);
    aabb.max.x = std::max(aabb.max.x, point.x);
    aabb.max.y = std::max(aabb.max.y, point.y);
    aabb.max.z = std::max(aabb.max.z, point.z);
}

AABB3 compute_aabb(const Asset::RuntimeBoxVolume& volume) {
    AABB3 out{};
    for (const auto& point : volume.world_points) {
        extend_aabb(out, to_vec3(point));
    }
    return out;
}

bool aabb_overlaps(const AABB3& lhs, const AABB3& rhs) {
    if (!lhs.valid || !rhs.valid) {
        return false;
    }
    return !(lhs.max.x < rhs.min.x || lhs.min.x > rhs.max.x ||
             lhs.max.y < rhs.min.y || lhs.min.y > rhs.max.y ||
             lhs.max.z < rhs.min.z || lhs.min.z > rhs.max.z);
}

constexpr std::array<std::array<int, 4>, 6> kVolumeFaces{{
    {{0, 1, 2, 3}},
    {{4, 5, 6, 7}},
    {{0, 1, 5, 4}},
    {{1, 2, 6, 5}},
    {{2, 3, 7, 6}},
    {{3, 0, 4, 7}},
}};

constexpr std::array<std::array<int, 2>, 12> kVolumeEdges{{
    {{0, 1}}, {{1, 2}}, {{2, 3}}, {{3, 0}},
    {{4, 5}}, {{5, 6}}, {{6, 7}}, {{7, 4}},
    {{0, 4}}, {{1, 5}}, {{2, 6}}, {{3, 7}},
}};

Vec3 face_normal(const Asset::RuntimeBoxVolume& volume, const std::array<int, 4>& face) {
    const Vec3 p0 = to_vec3(volume.world_points[static_cast<std::size_t>(face[0])]);
    const Vec3 p1 = to_vec3(volume.world_points[static_cast<std::size_t>(face[1])]);
    const Vec3 p2 = to_vec3(volume.world_points[static_cast<std::size_t>(face[2])]);
    return cross(subtract(p1, p0), subtract(p2, p0));
}

Vec3 edge_vector(const Asset::RuntimeBoxVolume& volume, const std::array<int, 2>& edge) {
    const Vec3 a = to_vec3(volume.world_points[static_cast<std::size_t>(edge[0])]);
    const Vec3 b = to_vec3(volume.world_points[static_cast<std::size_t>(edge[1])]);
    return subtract(b, a);
}

void project_on_axis(const Asset::RuntimeBoxVolume& volume,
                     const Vec3& axis,
                     float& out_min,
                     float& out_max) {
    out_min = std::numeric_limits<float>::infinity();
    out_max = -std::numeric_limits<float>::infinity();
    for (const auto& point : volume.world_points) {
        const float value = dot(to_vec3(point), axis);
        out_min = std::min(out_min, value);
        out_max = std::max(out_max, value);
    }
}

bool intervals_overlap(float min_a, float max_a, float min_b, float max_b) {
    return !(max_a < min_b || max_b < min_a);
}

bool sat_intersects(const Asset::RuntimeBoxVolume& attack_volume,
                    const Asset::RuntimeBoxVolume& hit_volume) {
    std::vector<Vec3> candidate_axes;
    candidate_axes.reserve(kVolumeFaces.size() * 2 + kVolumeEdges.size() * kVolumeEdges.size());

    for (const auto& face : kVolumeFaces) {
        Vec3 axis = face_normal(attack_volume, face);
        if (normalize_axis(axis)) {
            candidate_axes.push_back(axis);
        }
    }
    for (const auto& face : kVolumeFaces) {
        Vec3 axis = face_normal(hit_volume, face);
        if (normalize_axis(axis)) {
            candidate_axes.push_back(axis);
        }
    }

    for (const auto& edge_a : kVolumeEdges) {
        const Vec3 vec_a = edge_vector(attack_volume, edge_a);
        for (const auto& edge_b : kVolumeEdges) {
            const Vec3 vec_b = edge_vector(hit_volume, edge_b);
            Vec3 axis = cross(vec_a, vec_b);
            if (normalize_axis(axis)) {
                candidate_axes.push_back(axis);
            }
        }
    }

    if (candidate_axes.empty()) {
        return false;
    }

    for (const Vec3& axis : candidate_axes) {
        float attack_min = 0.0f;
        float attack_max = 0.0f;
        float hit_min = 0.0f;
        float hit_max = 0.0f;
        project_on_axis(attack_volume, axis, attack_min, attack_max);
        project_on_axis(hit_volume, axis, hit_min, hit_max);
        if (!intervals_overlap(attack_min, attack_max, hit_min, hit_max)) {
            return false;
        }
    }
    return true;
}

std::string stable_asset_id(const Asset& asset) {
    if (!asset.spawn_id.empty()) {
        return asset.spawn_id;
    }
    if (asset.info) {
        return asset.info->name;
    }
    return std::string{};
}

std::string stable_asset_name(const Asset& asset) {
    if (asset.info) {
        return asset.info->name;
    }
    return std::string{};
}

}  // namespace

std::optional<Attack> AttackValidation::compute_attack_if_hit(const Asset& attacker,
                                                              const Asset& target) {
    const auto& attack_volumes = attacker.current_attack_box_volumes();
    const auto& hit_volumes = target.current_hit_box_volumes();
    if (attack_volumes.empty() || hit_volumes.empty()) {
        return std::nullopt;
    }

    for (const auto& attack_volume : attack_volumes) {
        if (!attack_volume.valid) {
            continue;
        }
        const AABB3 attack_aabb = compute_aabb(attack_volume);
        for (const auto& hit_volume : hit_volumes) {
            if (!hit_volume.valid) {
                continue;
            }
            const AABB3 hit_aabb = compute_aabb(hit_volume);
            if (!aabb_overlaps(attack_aabb, hit_aabb)) {
                continue;
            }
            if (!sat_intersects(attack_volume, hit_volume)) {
                continue;
            }

            Attack attack{};
            attack.attacker_asset_id = stable_asset_id(attacker);
            attack.attacker_asset_name = stable_asset_name(attacker);
            attack.target_asset_id = stable_asset_id(target);
            attack.target_asset_name = stable_asset_name(target);
            attack.damage_amount = attack_volume.damage_amount;
            attack.hit_x = (attack_volume.centroid.x + hit_volume.centroid.x) * 0.5f;
            attack.hit_y = (attack_volume.centroid.y + hit_volume.centroid.y) * 0.5f;
            attack.hit_z = (attack_volume.centroid.z + hit_volume.centroid.z) * 0.5f;
            attack.source_frame_index =
                attacker.current_animation_frame() ? attacker.current_animation_frame()->frame_index : -1;
            return attack;
        }
    }

    return std::nullopt;
}

}  // namespace animation_update
