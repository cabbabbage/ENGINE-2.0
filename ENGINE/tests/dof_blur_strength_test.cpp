#include "rendering/render/layer_depth_bins.hpp"

#include <cassert>
#include <cmath>

namespace {

bool nearly_equal(float lhs, float rhs) {
    return std::fabs(lhs - rhs) <= 1.0e-6f;
}

} // namespace

int main() {
    constexpr int focus = 7;
    constexpr int max_distance_from_focus = 4;

    const float at_focus = render_depth::dof_blur_strength_for_layer_distance(
        focus,
        focus,
        max_distance_from_focus);
    const float near_background = render_depth::dof_blur_strength_for_layer_distance(
        focus + 1,
        focus,
        max_distance_from_focus);
    const float near_foreground = render_depth::dof_blur_strength_for_layer_distance(
        focus - 1,
        focus,
        max_distance_from_focus);
    const float mid_background = render_depth::dof_blur_strength_for_layer_distance(
        focus + 2,
        focus,
        max_distance_from_focus);
    const float mid_foreground = render_depth::dof_blur_strength_for_layer_distance(
        focus - 2,
        focus,
        max_distance_from_focus);
    const float far_background = render_depth::dof_blur_strength_for_layer_distance(
        focus + 3,
        focus,
        max_distance_from_focus);
    const float far_foreground = render_depth::dof_blur_strength_for_layer_distance(
        focus - 3,
        focus,
        max_distance_from_focus);
    const float edge_background = render_depth::dof_blur_strength_for_layer_distance(
        focus + 4,
        focus,
        max_distance_from_focus);
    const float edge_foreground = render_depth::dof_blur_strength_for_layer_distance(
        focus - 4,
        focus,
        max_distance_from_focus);
    const float beyond_background = render_depth::dof_blur_strength_for_layer_distance(
        focus + 50,
        focus,
        max_distance_from_focus);
    const float beyond_foreground = render_depth::dof_blur_strength_for_layer_distance(
        focus - 50,
        focus,
        max_distance_from_focus);

    assert(nearly_equal(at_focus, 0.0f));
    assert(nearly_equal(near_background, near_foreground));
    assert(nearly_equal(mid_background, mid_foreground));
    assert(nearly_equal(far_background, far_foreground));
    assert(nearly_equal(edge_background, edge_foreground));
    assert(nearly_equal(beyond_background, beyond_foreground));
    assert(at_focus < near_background);
    assert(near_background < mid_background);
    assert(mid_background < far_background);
    assert(far_background < edge_background);
    assert(near_background < 0.25f);
    assert(nearly_equal(mid_background, 0.5f));
    assert(nearly_equal(edge_background, 1.0f));
    assert(nearly_equal(beyond_background, 1.0f));

    return 0;
}
