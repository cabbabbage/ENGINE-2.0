#pragma once

#include "rendering/render/layer_effect_processor.hpp"
#include "rendering/render/render.hpp"

namespace render_internal {

class LightingSystemV2 {
public:
    static float attenuate_for_layer(const LayerEffectProcessor::RuntimeLight& light,
                                     const DepthInterval& layer_depth,
                                     const ScreenAabb& layer_bounds);
};

} // namespace render_internal
