#include "rendering/render/lighting_system_v2.hpp"

#include <algorithm>
#include <cmath>

namespace render_internal {

float LightingSystemV2::attenuate_for_layer(const LayerEffectProcessor::RuntimeLight& light,
                                            const DepthInterval& layer_depth,
                                            const ScreenAabb& layer_bounds) {
    if (!(std::isfinite(light.intensity) && light.intensity > 0.0f)) {
        return 0.0f;
    }
    if (!std::isfinite(light.screen_center.x) || !std::isfinite(light.screen_center.y)) {
        return 0.0f;
    }
    const float cx = 0.5f * (layer_bounds.min_x + layer_bounds.max_x);
    const float cy = 0.5f * (layer_bounds.min_y + layer_bounds.max_y);
    const float dx = light.screen_center.x - cx;
    const float dy = light.screen_center.y - cy;
    const float screen_distance = std::sqrt(dx * dx + dy * dy);
    const float screen_radius = std::max(1.0f, std::isfinite(light.radius_px) ? light.radius_px : 1.0f);
    const float screen_term = 1.0f / (1.0f + (screen_distance / screen_radius));

    const double layer_center = 0.5 * (layer_depth.min + layer_depth.max);
    const double depth_delta = std::abs(static_cast<double>(light.world_z) - layer_center);
    const double depth_radius = std::max(1.0, static_cast<double>(
        std::isfinite(light.radius_world) && light.radius_world > 0.0f ? light.radius_world : screen_radius));
    const float depth_term = static_cast<float>(1.0 / (1.0 + (depth_delta / depth_radius)));

    const float attenuation = std::clamp(screen_term * depth_term, 0.0f, 1.0f);
    return light.intensity * attenuation;
}

} // namespace render_internal
