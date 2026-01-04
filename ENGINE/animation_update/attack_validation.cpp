#include "animation_update/attack_validation.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <unordered_map>

#include <SDL.h>

namespace animation_update {

namespace {

constexpr float kDegToRad = 0.017453292519943295f;
constexpr float kIntersectionEpsilon = 1e-6f;
constexpr float kAttackRangeMargin = 8.0f;

struct AABB {
    float min_x = 0.0f;
    float min_y = 0.0f;
    float max_x = 0.0f;
    float max_y = 0.0f;
    bool valid = false;
};

inline void extend_aabb(AABB& box, const SDL_FPoint& point) {
    if (!box.valid) {
        box.min_x = point.x;
        box.max_x = point.x;
        box.min_y = point.y;
        box.max_y = point.y;
        box.valid = true;
        return;
    }
    box.min_x = std::min(box.min_x, point.x);
    box.max_x = std::max(box.max_x, point.x);
    box.min_y = std::min(box.min_y, point.y);
    box.max_y = std::max(box.max_y, point.y);
}

inline bool aabb_overlaps(const AABB& lhs, const AABB& rhs) {
    if (!lhs.valid || !rhs.valid) {
        return false;
    }
    if (lhs.max_x < rhs.min_x || lhs.min_x > rhs.max_x) {
        return false;
    }
    if (lhs.max_y < rhs.min_y || lhs.min_y > rhs.max_y) {
        return false;
    }
    return true;
}

inline float plane_coordinate(float y, float z, CombatPlane plane) {
    return (plane == CombatPlane::XY) ? y : z;
}

inline float safe_scale(float scale) {
    return (scale > 0.0f) ? scale : 1.0f;
}

inline SDL_FPoint local_to_world(float local_x, float local_plane, const GeometryContext& ctx) {
    const float scale = safe_scale(ctx.scale);
    const float mirrored_x = ctx.flipped ? -local_x : local_x;
    return SDL_FPoint{ ctx.anchor.x + mirrored_x * scale, ctx.anchor.y - local_plane * scale };
}

double cross(const SDL_FPoint& a, const SDL_FPoint& b, const SDL_FPoint& c) {
    return (static_cast<double>(b.x) - a.x) * (static_cast<double>(c.y) - a.y) -
           (static_cast<double>(b.y) - a.y) * (static_cast<double>(c.x) - a.x);
}

bool on_segment(const SDL_FPoint& a, const SDL_FPoint& b, const SDL_FPoint& p) {
    const float min_x = std::min(a.x, b.x);
    const float max_x = std::max(a.x, b.x);
    const float min_y = std::min(a.y, b.y);
    const float max_y = std::max(a.y, b.y);
    return p.x >= min_x - kIntersectionEpsilon && p.x <= max_x + kIntersectionEpsilon &&
           p.y >= min_y - kIntersectionEpsilon && p.y <= max_y + kIntersectionEpsilon;
}

bool segments_intersect(const SDL_FPoint& a1,
                        const SDL_FPoint& a2,
                        const SDL_FPoint& b1,
                        const SDL_FPoint& b2) {
    const double o1 = cross(a1, a2, b1);
    const double o2 = cross(a1, a2, b2);
    const double o3 = cross(b1, b2, a1);
    const double o4 = cross(b1, b2, a2);

    if (std::fabs(o1) < kIntersectionEpsilon && on_segment(a1, a2, b1)) return true;
    if (std::fabs(o2) < kIntersectionEpsilon && on_segment(a1, a2, b2)) return true;
    if (std::fabs(o3) < kIntersectionEpsilon && on_segment(b1, b2, a1)) return true;
    if (std::fabs(o4) < kIntersectionEpsilon && on_segment(b1, b2, a2)) return true;

    return (o1 * o2 < 0.0) && (o3 * o4 < 0.0);
}

std::optional<SDL_FPoint> segment_intersection_point(const SDL_FPoint& a1,
                                                     const SDL_FPoint& a2,
                                                     const SDL_FPoint& b1,
                                                     const SDL_FPoint& b2) {
    const double denom =
        (static_cast<double>(a1.x) - a2.x) * (static_cast<double>(b1.y) - b2.y) -
        (static_cast<double>(a1.y) - a2.y) * (static_cast<double>(b1.x) - b2.x);
    if (std::fabs(denom) < kIntersectionEpsilon) {
        return std::nullopt;
    }

    const double a = static_cast<double>(a1.x) * a2.y - static_cast<double>(a1.y) * a2.x;
    const double b = static_cast<double>(b1.x) * b2.y - static_cast<double>(b1.y) * b2.x;

    const double x = (a * (b1.x - b2.x) - (a1.x - a2.x) * b) / denom;
    const double y = (a * (b1.y - b2.y) - (a1.y - a2.y) * b) / denom;
    return SDL_FPoint{ static_cast<float>(x), static_cast<float>(y) };
}

std::optional<SDL_FPoint> find_path_polygon_intersection(const std::vector<SDL_FPoint>& path,
                                                         const std::vector<SDL_FPoint>& polygon) {
    if (path.size() < 2 || polygon.size() < 3) {
        return std::nullopt;
    }

    auto point_inside = [&](const SDL_FPoint& p) {
        bool inside = false;
        const std::size_t count = polygon.size();
        for (std::size_t i = 0, j = count - 1; i < count; j = i++) {
            const float xi = polygon[i].x;
            const float yi = polygon[i].y;
            const float xj = polygon[j].x;
            const float yj = polygon[j].y;
            const bool intersect = ((yi > p.y) != (yj > p.y)) &&
                                   (p.x < (xj - xi) * (p.y - yi) / (yj - yi) + xi);
            if (intersect) {
                inside = !inside;
            }
        }
        return inside;
    };

    for (std::size_t i = 0; i + 1 < path.size(); ++i) {
        const SDL_FPoint& seg_start = path[i];
        const SDL_FPoint& seg_end = path[i + 1];
        if (point_inside(seg_start)) {
            return seg_start;
        }
        if (point_inside(seg_end)) {
            return seg_end;
        }
        for (std::size_t j = 0, k = polygon.size() - 1; j < polygon.size(); k = j++) {
            const SDL_FPoint& poly_start = polygon[k];
            const SDL_FPoint& poly_end = polygon[j];
            if (!segments_intersect(seg_start, seg_end, poly_start, poly_end)) {
                continue;
            }
            auto point = segment_intersection_point(seg_start, seg_end, poly_start, poly_end);
            if (point.has_value()) {
                return point;
            }
        }
    }
    return std::nullopt;
}

} // namespace

std::vector<SDL_FPoint> AttackValidation::attack_vector_path(const FrameAttackGeometry::Vector& vector,
                                                             const GeometryContext& context,
                                                             std::size_t segments) {
    if (segments == 0) {
        segments = 1;
    }
    const float start_plane = plane_coordinate(vector.start_y, vector.start_z, context.plane);
    const float control_plane = plane_coordinate(vector.control_y, vector.control_z, context.plane);
    const float end_plane = plane_coordinate(vector.end_y, vector.end_z, context.plane);

    std::vector<SDL_FPoint> path;
    path.reserve(segments + 1);
    for (std::size_t idx = 0; idx <= segments; ++idx) {
        const float t = static_cast<float>(idx) / static_cast<float>(segments);
        const float u = 1.0f - t;
        const float local_x =
            u * u * vector.start_x + 2.0f * u * t * vector.control_x + t * t * vector.end_x;
        const float local_plane =
            u * u * start_plane + 2.0f * u * t * control_plane + t * t * end_plane;
        path.push_back(local_to_world(local_x, local_plane, context));
    }
    return path;
}

std::vector<SDL_FPoint> AttackValidation::hitbox_polygon(const FrameHitGeometry::HitBox& box,
                                                        const GeometryContext& context) {
    if (box.is_empty()) {
        return {};
    }

    const float plane_center = plane_coordinate(box.center_y, box.center_z, context.plane);
    const float cos_r = std::cos(box.rotation_degrees * kDegToRad);
    const float sin_r = std::sin(box.rotation_degrees * kDegToRad);
    const std::array<SDL_FPoint, 4> offsets = {
        SDL_FPoint{ -box.half_width,  box.half_height },
        SDL_FPoint{  box.half_width,  box.half_height },
        SDL_FPoint{  box.half_width, -box.half_height },
        SDL_FPoint{ -box.half_width, -box.half_height },
    };

    std::vector<SDL_FPoint> corners;
    corners.reserve(offsets.size());
    for (const auto& offset : offsets) {
        const float rotated_x = offset.x * cos_r - offset.y * sin_r;
        const float rotated_plane = offset.x * sin_r + offset.y * cos_r;
        const float local_x = box.center_x + rotated_x;
        const float local_plane = plane_center + rotated_plane;
        corners.push_back(local_to_world(local_x, local_plane, context));
    }
    return corners;
}

std::optional<Attack> AttackValidation::compute_attack_if_hit(
    const CombatantSnapshot& attacker,
    const CombatantSnapshot& target,
    const std::vector<ChildAttachmentSnapshot>& child_frames,
    std::size_t path_segments) {

    if (!attacker.frame || !target.frame) {
        return std::nullopt;
    }

    if (!(target.transform.scale > 0.0f)) {
        return std::nullopt;
    }

    std::unordered_map<std::string, std::vector<std::vector<SDL_FPoint>>> hitbox_map;
    hitbox_map.reserve(target.frame->hit_geometry.boxes.size());
    AABB target_aabb{};
    for (const auto& box : target.frame->hit_geometry.boxes) {
        if (box.is_empty()) {
            continue;
        }
        const auto corners = hitbox_polygon(box, target.transform);
        if (corners.size() < 4) {
            continue;
        }
        for (const auto& corner : corners) {
            extend_aabb(target_aabb, corner);
        }
        hitbox_map[box.type].push_back(corners);
    }
    if (hitbox_map.empty() || !target_aabb.valid) {
        return std::nullopt;
    }

    struct Source {
        const AnimationFrame* frame = nullptr;
        GeometryContext transform{};
    };

    std::vector<Source> sources;
    sources.reserve(1 + child_frames.size());
    if (!attacker.frame->attack_geometry.vectors.empty()) {
        sources.push_back({ attacker.frame, attacker.transform });
    }
    for (const auto& child : child_frames) {
        if (!child.frame || child.frame->attack_geometry.vectors.empty()) {
            continue;
        }
        sources.push_back({ child.frame, child.transform });
    }
    if (sources.empty()) {
        return std::nullopt;
    }

    AABB attack_aabb{};
    for (const auto& source : sources) {
        for (const auto& vector : source.frame->attack_geometry.vectors) {
            const float start_plane =
                plane_coordinate(vector.start_y, vector.start_z, source.transform.plane);
            const float control_plane =
                plane_coordinate(vector.control_y, vector.control_z, source.transform.plane);
            const float end_plane =
                plane_coordinate(vector.end_y, vector.end_z, source.transform.plane);
            extend_aabb(attack_aabb,
                        local_to_world(vector.start_x, start_plane, source.transform));
            extend_aabb(attack_aabb,
                        local_to_world(vector.control_x, control_plane, source.transform));
            extend_aabb(attack_aabb,
                        local_to_world(vector.end_x, end_plane, source.transform));
        }
    }
    if (!attack_aabb.valid) {
        return std::nullopt;
    }

    attack_aabb.min_x -= kAttackRangeMargin;
    attack_aabb.min_y -= kAttackRangeMargin;
    attack_aabb.max_x += kAttackRangeMargin;
    attack_aabb.max_y += kAttackRangeMargin;
    if (!aabb_overlaps(attack_aabb, target_aabb)) {
        return std::nullopt;
    }

    for (const auto& source : sources) {
        for (const auto& vector : source.frame->attack_geometry.vectors) {
            auto it = hitbox_map.find(vector.type);
            if (it == hitbox_map.end()) {
                continue;
            }
            const auto path = attack_vector_path(vector, source.transform,
                                                 path_segments == 0 ? kDefaultAttackSegments : path_segments);
            if (path.size() < 2) {
                continue;
            }
            for (const auto& polygon : it->second) {
                const auto intersection = find_path_polygon_intersection(path, polygon);
                if (!intersection.has_value()) {
                    continue;
                }
                Attack attack;
                attack.attacker_asset_id = attacker.asset_id;
                attack.attacker_asset_name = attacker.asset_name;
                attack.target_asset_id = target.asset_id;
                attack.target_asset_name = target.asset_name;
                attack.damage_amount = vector.damage;
                attack.damage_type = vector.type;
                attack.hit_x = intersection->x;
                const float plane_value =
                    (target.transform.anchor.y - intersection->y) / target.transform.scale;
                if (target.transform.plane == CombatPlane::XY) {
                    attack.hit_y = plane_value;
                    attack.hit_z = 0.0f;
                } else {
                    attack.hit_y = 0.0f;
                    attack.hit_z = plane_value;
                }
                attack.source_frame_index = source.frame ? source.frame->frame_index : -1;
                attack.vectors.clear();
                attack.vectors.push_back(vector);
                return attack;
            }
        }
    }

    return std::nullopt;
}

} // namespace animation_update
