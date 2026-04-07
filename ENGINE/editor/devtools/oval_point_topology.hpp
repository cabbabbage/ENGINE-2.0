#pragma once

#include <algorithm>
#include <cmath>
#include <vector>

#include "assets/asset/asset_info.hpp"
#include "utils/oval_anchor_math.hpp"

namespace devmode::oval_point_topology {

inline std::vector<AssetInfo::OvalAnchorPoint> sort_points_by_angle(
    const std::vector<AssetInfo::OvalAnchorPoint>& points) {
    std::vector<AssetInfo::OvalAnchorPoint> sorted = points;
    std::sort(sorted.begin(),
              sorted.end(),
              [](const AssetInfo::OvalAnchorPoint& lhs, const AssetInfo::OvalAnchorPoint& rhs) {
                  return oval_anchor_math::normalize_angle_degrees(lhs.angle_degrees) <
                         oval_anchor_math::normalize_angle_degrees(rhs.angle_degrees);
              });
    return sorted;
}

inline bool points_evenly_spaced_by_angle(const std::vector<AssetInfo::OvalAnchorPoint>& sorted_points) {
    if (sorted_points.size() < 2) {
        return false;
    }
    const float expected_step = 360.0f / static_cast<float>(sorted_points.size());
    constexpr float kSpacingToleranceDegrees = 0.75f;
    for (std::size_t i = 0; i < sorted_points.size(); ++i) {
        const float current = oval_anchor_math::normalize_angle_degrees(sorted_points[i].angle_degrees);
        float next = oval_anchor_math::normalize_angle_degrees(sorted_points[(i + 1) % sorted_points.size()].angle_degrees);
        if (next <= current) {
            next += 360.0f;
        }
        const float step = next - current;
        if (std::fabs(step - expected_step) > kSpacingToleranceDegrees) {
            return false;
        }
    }
    return true;
}

inline std::vector<AssetInfo::OvalAnchorPoint> double_point_count(
    const std::vector<AssetInfo::OvalAnchorPoint>& points) {
    if (points.size() < 2) {
        return points;
    }

    const std::vector<AssetInfo::OvalAnchorPoint> sorted_points = sort_points_by_angle(points);
    std::vector<AssetInfo::OvalAnchorPoint> midpoint_batch;
    midpoint_batch.reserve(sorted_points.size());
    for (std::size_t i = 0; i < sorted_points.size(); ++i) {
        const AssetInfo::OvalAnchorPoint& current = sorted_points[i];
        const AssetInfo::OvalAnchorPoint& next = sorted_points[(i + 1) % sorted_points.size()];
        const float a = oval_anchor_math::normalize_angle_degrees(current.angle_degrees);
        float b = oval_anchor_math::normalize_angle_degrees(next.angle_degrees);
        if (b <= a) {
            b += 360.0f;
        }

        AssetInfo::OvalAnchorPoint midpoint = current;
        midpoint.angle_degrees = oval_anchor_math::normalize_angle_degrees((a + b) * 0.5f);
        midpoint_batch.push_back(midpoint);
    }

    std::vector<AssetInfo::OvalAnchorPoint> doubled = sorted_points;
    doubled.insert(doubled.end(), midpoint_batch.begin(), midpoint_batch.end());
    return doubled;
}

inline std::vector<AssetInfo::OvalAnchorPoint> halve_point_count(
    const std::vector<AssetInfo::OvalAnchorPoint>& points) {
    if (points.size() <= 4) {
        return points;
    }

    const std::size_t current_count = points.size();
    const std::size_t target_count = std::max<std::size_t>(4, (current_count + 1) / 2);

    std::vector<AssetInfo::OvalAnchorPoint> next_points;
    if ((current_count % 2) == 0 && (current_count / 2) >= 4) {
        const std::vector<AssetInfo::OvalAnchorPoint> sorted_points = sort_points_by_angle(points);
        if (points_evenly_spaced_by_angle(sorted_points)) {
            next_points.reserve(current_count / 2);
            for (std::size_t i = 0; i < sorted_points.size(); i += 2) {
                next_points.push_back(sorted_points[i]);
            }
        }
    }

    if (next_points.empty()) {
        next_points = points;
        if (next_points.size() > target_count) {
            next_points.resize(target_count);
        }
    }

    if (next_points.size() < 4 || next_points.size() >= current_count) {
        return points;
    }
    return next_points;
}

}  // namespace devmode::oval_point_topology

