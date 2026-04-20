#include "animation/attack_validation.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "assets/asset/Asset.hpp"
#include "assets/asset/animation.hpp"
#include "assets/asset/animation_frame.hpp"
#include "animation_update.hpp"

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

struct FrameAttackAabb {
    float min_x = 0.0f;
    float min_y = 0.0f;
    float max_x = 0.0f;
    float max_y = 0.0f;
    bool valid = false;
};

struct AnimationAttackFrameMeta {
    FrameAttackAabb local_attack_aabb{};
    axis::WorldPos cumulative_displacement{0, 0, 0};
};

struct AnimationAttackMetadata {
    std::string animation_id;
    std::size_t path_index = 0;
    std::vector<AnimationAttackFrameMeta> frames;
};

std::mutex g_attack_metadata_mutex;
std::unordered_map<std::string, AnimationAttackMetadata> g_attack_metadata_cache;

std::string stable_asset_id(const Asset& asset);
std::string stable_asset_name(const Asset& asset);

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

bool aabb_overlaps_with_padding(const AABB3& lhs, const AABB3& rhs, float padding) {
    if (!lhs.valid || !rhs.valid) {
        return false;
    }
    return !(lhs.max.x + padding < rhs.min.x || lhs.min.x - padding > rhs.max.x ||
             lhs.max.y + padding < rhs.min.y || lhs.min.y - padding > rhs.max.y ||
             lhs.max.z + padding < rhs.min.z || lhs.min.z - padding > rhs.max.z);
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

AABB3 translate_aabb(const AABB3& in, const axis::WorldPos& delta) {
    if (!in.valid) {
        return in;
    }
    AABB3 out = in;
    out.min.x += static_cast<float>(delta.x);
    out.min.y += static_cast<float>(delta.y);
    out.min.z += static_cast<float>(delta.z);
    out.max.x += static_cast<float>(delta.x);
    out.max.y += static_cast<float>(delta.y);
    out.max.z += static_cast<float>(delta.z);
    return out;
}

Asset::RuntimeBoxVolume translate_volume(const Asset::RuntimeBoxVolume& in, const axis::WorldPos& delta) {
    Asset::RuntimeBoxVolume out = in;
    out.centroid.x += delta.x;
    out.centroid.y += delta.y;
    out.centroid.z += delta.z;
    for (auto& point : out.world_points) {
        point.x += delta.x;
        point.y += delta.y;
        point.z += delta.z;
    }
    return out;
}

std::string make_attack_cache_key(const Asset& asset,
                                  const std::string& animation_id,
                                  std::size_t path_index) {
    std::string key = stable_asset_name(asset);
    key.push_back('#');
    key.append(animation_id);
    key.push_back('#');
    key.append(std::to_string(path_index));
    return key;
}

AnimationAttackMetadata build_attack_metadata(const Animation& animation,
                                             const std::string& animation_id,
                                             std::size_t path_index) {
    AnimationAttackMetadata out{};
    out.animation_id = animation_id;
    out.path_index = path_index;
    const auto& path = animation.movement_path(path_index);
    out.frames.reserve(path.size());
    axis::WorldPos cumulative{0, 0, 0};
    for (const AnimationFrame& frame : path) {
        AnimationAttackFrameMeta frame_meta{};
        frame_meta.cumulative_displacement = cumulative;
        for (const auto& attack_box : frame.attack_boxes.boxes) {
            if (!attack_box.enabled) {
                continue;
            }
            const auto points = attack_box.to_runtime_clockwise_points();
            for (const auto& point : points) {
                if (!frame_meta.local_attack_aabb.valid) {
                    frame_meta.local_attack_aabb.min_x = static_cast<float>(point.texture_x);
                    frame_meta.local_attack_aabb.max_x = static_cast<float>(point.texture_x);
                    frame_meta.local_attack_aabb.min_y = static_cast<float>(point.texture_y);
                    frame_meta.local_attack_aabb.max_y = static_cast<float>(point.texture_y);
                    frame_meta.local_attack_aabb.valid = true;
                } else {
                    frame_meta.local_attack_aabb.min_x = std::min(frame_meta.local_attack_aabb.min_x, static_cast<float>(point.texture_x));
                    frame_meta.local_attack_aabb.max_x = std::max(frame_meta.local_attack_aabb.max_x, static_cast<float>(point.texture_x));
                    frame_meta.local_attack_aabb.min_y = std::min(frame_meta.local_attack_aabb.min_y, static_cast<float>(point.texture_y));
                    frame_meta.local_attack_aabb.max_y = std::max(frame_meta.local_attack_aabb.max_y, static_cast<float>(point.texture_y));
                }
            }
        }
        out.frames.push_back(frame_meta);
        cumulative.x += frame.dx;
        cumulative.y += frame.dy;
        cumulative.z += frame.dz;
    }
    return out;
}

const AnimationAttackMetadata* get_or_build_attack_metadata(const Asset& attacker,
                                                            const std::string& animation_id,
                                                            std::size_t path_index) {
    if (!attacker.info) {
        return nullptr;
    }
    const auto animation_it = attacker.info->animations.find(animation_id);
    if (animation_it == attacker.info->animations.end()) {
        return nullptr;
    }
    const Animation& animation = animation_it->second;
    path_index = animation.clamp_path_index(path_index);
    const std::string key = make_attack_cache_key(attacker, animation_id, path_index);
    {
        std::lock_guard<std::mutex> lock(g_attack_metadata_mutex);
        auto it = g_attack_metadata_cache.find(key);
        if (it != g_attack_metadata_cache.end()) {
            return &it->second;
        }
    }
    AnimationAttackMetadata built = build_attack_metadata(animation, animation_id, path_index);
    std::lock_guard<std::mutex> lock(g_attack_metadata_mutex);
    auto [it, inserted] = g_attack_metadata_cache.emplace(key, std::move(built));
    (void)inserted;
    return &it->second;
}

axis::WorldPos estimate_target_frame_velocity(const Asset& target) {
    if (!target.current_animation_frame()) {
        return axis::WorldPos{0, 0, 0};
    }
    return animation_update::detail::frame_world_delta_3d(
        *target.current_animation_frame(),
        target,
        vibble::grid::global_grid());
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
    if (!attacker.isAttackBoxEnabled() || !target.isHitboxEnabled()) {
        return std::nullopt;
    }
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
            attack.attack_type = attack_volume.type.empty() ? std::string{"attack_box"} : attack_volume.type;
            attack.payload = attack_volume.payload;
            if (attack.payload.payload_id.empty()) {
                attack.payload.payload_id =
                    attack_volume.payload_id.empty() ? attack_volume.id : attack_volume.payload_id;
            }
            attack.payload.damage_amount = std::max(0, attack.payload.damage_amount);
            attack.attack_payload_id = attack.payload.payload_id;
            attack.damage_amount = attack.payload.damage_amount;
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

AttackValidation::AttackWindowEvaluation AttackValidation::evaluate_attack_window(
    const Asset& attacker,
    const Asset& target,
    const std::string& attack_animation_id,
    int horizon_frames) {
    AttackWindowEvaluation evaluation{};
    if (!attacker.info || !target.info || !attacker.isAttackBoxEnabled() || !target.isHitboxEnabled()) {
        return evaluation;
    }
    const auto* metadata = get_or_build_attack_metadata(attacker, attack_animation_id, 0);
    if (!metadata || metadata->frames.empty()) {
        return evaluation;
    }
    const auto& attack_volumes = attacker.current_attack_box_volumes();
    const auto& hit_volumes = target.current_hit_box_volumes();
    if (attack_volumes.empty() || hit_volumes.empty()) {
        return evaluation;
    }

    const int current_frame_index =
        attacker.current_animation_frame() ? std::max(0, attacker.current_animation_frame()->frame_index) : 0;
    const int clamped_horizon = std::max(1, horizon_frames);
    const int terminal_index = std::min<int>(
        static_cast<int>(metadata->frames.size()) - 1,
        current_frame_index + clamped_horizon);
    const int sampled_mid_index = std::max(current_frame_index, (current_frame_index + terminal_index) / 2);

    const axis::WorldPos attacker_terminal_delta{
        metadata->frames[terminal_index].cumulative_displacement.x -
            metadata->frames[std::min<int>(current_frame_index, static_cast<int>(metadata->frames.size()) - 1)]
                .cumulative_displacement.x,
        metadata->frames[terminal_index].cumulative_displacement.y -
            metadata->frames[std::min<int>(current_frame_index, static_cast<int>(metadata->frames.size()) - 1)]
                .cumulative_displacement.y,
        metadata->frames[terminal_index].cumulative_displacement.z -
            metadata->frames[std::min<int>(current_frame_index, static_cast<int>(metadata->frames.size()) - 1)]
                .cumulative_displacement.z};
    const axis::WorldPos attacker_mid_delta{
        metadata->frames[sampled_mid_index].cumulative_displacement.x -
            metadata->frames[std::min<int>(current_frame_index, static_cast<int>(metadata->frames.size()) - 1)]
                .cumulative_displacement.x,
        metadata->frames[sampled_mid_index].cumulative_displacement.y -
            metadata->frames[std::min<int>(current_frame_index, static_cast<int>(metadata->frames.size()) - 1)]
                .cumulative_displacement.y,
        metadata->frames[sampled_mid_index].cumulative_displacement.z -
            metadata->frames[std::min<int>(current_frame_index, static_cast<int>(metadata->frames.size()) - 1)]
                .cumulative_displacement.z};

    const axis::WorldPos target_velocity = estimate_target_frame_velocity(target);
    const axis::WorldPos target_terminal_delta{
        target_velocity.x * clamped_horizon,
        target_velocity.y * clamped_horizon,
        target_velocity.z * clamped_horizon};
    const axis::WorldPos target_mid_delta{
        target_velocity.x * ((clamped_horizon + 1) / 2),
        target_velocity.y * ((clamped_horizon + 1) / 2),
        target_velocity.z * ((clamped_horizon + 1) / 2)};

    AABB3 coarse_attack_sweep{};
    for (const auto& volume : attack_volumes) {
        if (!volume.valid) {
            continue;
        }
        const AABB3 now = compute_aabb(volume);
        const AABB3 projected = translate_aabb(now, attacker_terminal_delta);
        if (now.valid) {
            extend_aabb(coarse_attack_sweep, now.min);
            extend_aabb(coarse_attack_sweep, now.max);
        }
        if (projected.valid) {
            extend_aabb(coarse_attack_sweep, projected.min);
            extend_aabb(coarse_attack_sweep, projected.max);
        }
    }

    AABB3 coarse_target_sweep{};
    for (const auto& volume : hit_volumes) {
        if (!volume.valid) {
            continue;
        }
        const AABB3 now = compute_aabb(volume);
        const AABB3 projected = translate_aabb(now, target_terminal_delta);
        if (now.valid) {
            extend_aabb(coarse_target_sweep, now.min);
            extend_aabb(coarse_target_sweep, now.max);
        }
        if (projected.valid) {
            extend_aabb(coarse_target_sweep, projected.min);
            extend_aabb(coarse_target_sweep, projected.max);
        }
    }

    if (!aabb_overlaps_with_padding(coarse_attack_sweep, coarse_target_sweep, 20.0f)) {
        evaluation.score = AttackWindowScore::Miss;
        return evaluation;
    }

    for (const auto& attack_volume : attack_volumes) {
        if (!attack_volume.valid) {
            continue;
        }
        const Asset::RuntimeBoxVolume attack_mid = translate_volume(attack_volume, attacker_mid_delta);
        const Asset::RuntimeBoxVolume attack_terminal = translate_volume(attack_volume, attacker_terminal_delta);
        const AABB3 attack_mid_aabb = compute_aabb(attack_mid);
        const AABB3 attack_terminal_aabb = compute_aabb(attack_terminal);
        for (const auto& hit_volume : hit_volumes) {
            if (!hit_volume.valid) {
                continue;
            }
            const Asset::RuntimeBoxVolume hit_mid = translate_volume(hit_volume, target_mid_delta);
            const Asset::RuntimeBoxVolume hit_terminal = translate_volume(hit_volume, target_terminal_delta);
            const AABB3 hit_mid_aabb = compute_aabb(hit_mid);
            const AABB3 hit_terminal_aabb = compute_aabb(hit_terminal);
            if (aabb_overlaps(attack_mid_aabb, hit_mid_aabb) && sat_intersects(attack_mid, hit_mid)) {
                evaluation.score = AttackWindowScore::ClearHit;
            } else if (aabb_overlaps(attack_terminal_aabb, hit_terminal_aabb) &&
                       sat_intersects(attack_terminal, hit_terminal)) {
                evaluation.score = AttackWindowScore::ClearHit;
            } else if (aabb_overlaps_with_padding(attack_terminal_aabb, hit_terminal_aabb, 18.0f)) {
                if (evaluation.score == AttackWindowScore::Miss) {
                    evaluation.score = AttackWindowScore::NearHit;
                }
            }

            if (evaluation.score == AttackWindowScore::ClearHit) {
                Attack attack{};
                attack.attacker_asset_id = stable_asset_id(attacker);
                attack.attacker_asset_name = stable_asset_name(attacker);
                attack.target_asset_id = stable_asset_id(target);
                attack.target_asset_name = stable_asset_name(target);
                attack.attack_type = attack_volume.type.empty() ? std::string{"attack_box"} : attack_volume.type;
                attack.payload = attack_volume.payload;
                if (attack.payload.payload_id.empty()) {
                    attack.payload.payload_id =
                        attack_volume.payload_id.empty() ? attack_volume.id : attack_volume.payload_id;
                }
                attack.payload.damage_amount = std::max(0, attack.payload.damage_amount);
                attack.attack_payload_id = attack.payload.payload_id;
                attack.damage_amount = attack.payload.damage_amount;
                attack.hit_x = (attack_terminal.centroid.x + hit_terminal.centroid.x) * 0.5f;
                attack.hit_y = (attack_terminal.centroid.y + hit_terminal.centroid.y) * 0.5f;
                attack.hit_z = (attack_terminal.centroid.z + hit_terminal.centroid.z) * 0.5f;
                attack.source_frame_index =
                    attacker.current_animation_frame() ? attacker.current_animation_frame()->frame_index : -1;
                evaluation.attack = std::move(attack);
                return evaluation;
            }
        }
    }

    return evaluation;
}

}  // namespace animation_update
