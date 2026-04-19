#pragma once

#include "rendering/render/render_pipeline_types.hpp"

class GeometryBatcher;
class WarpedScreenGrid;

class LayerSubmissionBuilder {
public:
    LayerSubmissionBuilder() = default;

    render_pipeline::LayerBuildResult build(const GeometryBatcher& geometry_batcher,
                                            const WarpedScreenGrid& cam,
                                            double player_split_world_z,
                                            double max_cull_depth) const;
};
