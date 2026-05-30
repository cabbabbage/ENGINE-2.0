#include "rendering/render/layer_depth_bins.hpp"

#include <cassert>
#include <cmath>

namespace {

bool nearly_equal(float lhs, float rhs) {
    return std::fabs(lhs - rhs) <= 1.0e-5f;
}

} // namespace

int main() {
    render_depth::LensBlurDepthSettings settings{};
    settings.aperture = 1.0f;
    settings.focus_falloff_acceleration = 1.4f;
    settings.max_near_blur_px = 8.0f;
    settings.max_far_blur_px = 24.0f;
    settings.near_far_blur_bias = 0.0f;
    settings.focus_dead_zone = 0.05f;

    constexpr float focus_depth = 7.0f;

    const float at_focus = render_depth::dof_blur_amount_for_layer_depth(focus_depth, focus_depth, settings);
    const float near_one = render_depth::dof_blur_amount_for_layer_depth(focus_depth - 1.0f, focus_depth, settings);
    const float near_two = render_depth::dof_blur_amount_for_layer_depth(focus_depth - 2.0f, focus_depth, settings);
    const float near_three = render_depth::dof_blur_amount_for_layer_depth(focus_depth - 3.0f, focus_depth, settings);
    const float far_one = render_depth::dof_blur_amount_for_layer_depth(focus_depth + 1.0f, focus_depth, settings);
    const float far_two = render_depth::dof_blur_amount_for_layer_depth(focus_depth + 2.0f, focus_depth, settings);
    const float far_three = render_depth::dof_blur_amount_for_layer_depth(focus_depth + 3.0f, focus_depth, settings);

    assert(nearly_equal(at_focus, 0.0f));
    assert(near_one > at_focus);
    assert(near_two > near_one);
    assert(near_three > near_two);
    assert(far_one > at_focus);
    assert(far_two > far_one);
    assert(far_three > far_two);

    render_depth::LensBlurDepthSettings far_heavy = settings;
    far_heavy.max_near_blur_px = 4.0f;
    far_heavy.max_far_blur_px = 16.0f;
    const float capped_near = render_depth::dof_blur_amount_for_layer_depth(focus_depth - 10.0f, focus_depth, far_heavy);
    const float capped_far = render_depth::dof_blur_amount_for_layer_depth(focus_depth + 10.0f, focus_depth, far_heavy);
    assert(nearly_equal(capped_near, 4.0f));
    assert(nearly_equal(capped_far, 16.0f));
    assert(capped_far > capped_near);

    render_depth::LensBlurDepthSettings faster_aperture = settings;
    faster_aperture.aperture = 2.0f;
    assert(render_depth::dof_blur_amount_for_layer_depth(focus_depth + 1.0f, focus_depth, faster_aperture) > far_one);

    render_depth::LensBlurDepthSettings faster_falloff = settings;
    faster_falloff.focus_falloff_acceleration = 2.0f;
    assert(render_depth::dof_blur_amount_for_layer_depth(focus_depth + 2.0f, focus_depth, faster_falloff) > far_two);

    render_depth::LensBlurDepthSettings biased = settings;
    biased.near_far_blur_bias = 0.5f;
    const float biased_near = render_depth::dof_blur_amount_for_layer_depth(focus_depth - 20.0f, focus_depth, biased);
    const float biased_far = render_depth::dof_blur_amount_for_layer_depth(focus_depth + 20.0f, focus_depth, biased);
    assert(nearly_equal(biased_near, settings.max_near_blur_px * 0.5f));
    assert(nearly_equal(biased_far, settings.max_far_blur_px * 1.5f));

    return 0;
}
