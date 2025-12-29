#pragma once

#include <string_view>

#include "transform_smoothing.hpp"

namespace transform_smoothing {

const TransformSmoothingParams& asset_alpha_params();

void set_asset_alpha_params(const TransformSmoothingParams& params);

void reload_from_settings();

}
