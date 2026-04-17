#pragma once

#include <cmath>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

#include "assets/asset/Asset.hpp"

namespace oval_anchor_heading {

inline constexpr float kPi = 3.14159265358979323846f;

inline void append_unique_center_anchor_name(std::vector<std::string>& out_names,
                                             std::unordered_set<std::string>& seen_names,
                                             const std::string& source_name) {
    const std::string center_name = AssetInfo::oval_center_anchor_name_for_mapping(source_name);
    if (center_name.empty()) {
        return;
    }
    if (seen_names.insert(center_name).second) {
        out_names.push_back(center_name);
    }
}

inline std::optional<float> resolve_effective_oval_heading_radians(Asset* owner_asset,
                                                                    const AnchorPoint& parent_anchor) {
    if (!owner_asset || !owner_asset->info || parent_anchor.name.empty()) {
        return std::nullopt;
    }

    const AssetInfo::OvalAnchorMapping* mapping =
        owner_asset->info->find_oval_anchor_mapping(parent_anchor.name, true);
    if (!mapping) {
        return std::nullopt;
    }

    std::vector<std::string> center_anchor_candidates;
    std::unordered_set<std::string> seen_center_names;
    append_unique_center_anchor_name(center_anchor_candidates, seen_center_names, mapping->name);
    append_unique_center_anchor_name(center_anchor_candidates, seen_center_names, parent_anchor.name);
    for (const auto& alias : mapping->legacy_names) {
        append_unique_center_anchor_name(center_anchor_candidates, seen_center_names, alias);
    }

    std::optional<AnchorPoint> center_anchor;
    for (const auto& center_name : center_anchor_candidates) {
        const auto resolved = owner_asset->anchor_state(center_name,
                                                        anchor_points::GridMaterialization::None,
                                                        Asset::AnchorResolveMode::ForceRecompute);
        if (!resolved.has_value() || !resolved->is_active()) {
            continue;
        }
        center_anchor = resolved;
        break;
    }

    const float center_x = center_anchor.has_value()
        ? center_anchor->world_exact_pos_2d.x
        : static_cast<float>(owner_asset->world_x());
    const float center_z = center_anchor.has_value()
        ? center_anchor->world_exact_z
        : (static_cast<float>(owner_asset->world_z()) + owner_asset->world_z_offset());

    float base_heading_radians = 0.0f;
    bool has_base_heading = false;
    if (owner_asset->has_directional_target_world_xz()) {
        const float target_x = owner_asset->directional_target_world_x();
        const float target_z = owner_asset->directional_target_world_z();
        const float heading_dx = target_x - center_x;
        const float heading_dz = target_z - center_z;
        const float heading_len_sq = heading_dx * heading_dx + heading_dz * heading_dz;
        if (std::isfinite(heading_dx) &&
            std::isfinite(heading_dz) &&
            heading_len_sq > 1e-6f) {
            base_heading_radians = std::atan2(heading_dz, heading_dx);
            has_base_heading = true;
        }
    }
    if (!has_base_heading && owner_asset->has_directional_heading_radians()) {
        const float heading_radians = owner_asset->directional_heading_radians();
        if (std::isfinite(heading_radians)) {
            base_heading_radians = heading_radians;
            has_base_heading = true;
        }
    }
    if (!has_base_heading) {
        return std::nullopt;
    }

    const float offset_degrees = std::isfinite(mapping->radius_offset_degrees)
        ? mapping->radius_offset_degrees
        : 0.0f;
    const float offset_radians = offset_degrees * (kPi / 180.0f);
    const float effective_heading_radians = base_heading_radians + offset_radians;
    if (!std::isfinite(effective_heading_radians)) {
        return std::nullopt;
    }
    return effective_heading_radians;
}

inline void rotate_xz_about_world_y(float angle_radians, float& x, float& z) {
    if (!std::isfinite(angle_radians) || !std::isfinite(x) || !std::isfinite(z)) {
        return;
    }
    const float cos_theta = std::cos(angle_radians);
    const float sin_theta = std::sin(angle_radians);
    const float rotated_x = x * cos_theta - z * sin_theta;
    const float rotated_z = x * sin_theta + z * cos_theta;
    x = rotated_x;
    z = rotated_z;
}

} // namespace oval_anchor_heading

