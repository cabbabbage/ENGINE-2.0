#include <doctest/doctest.h>

#include "rendering/render/depth_cue_overlay_math.hpp"

namespace {

void check_decision(const depth_cue::OverlayOpacityDecision& decision,
                    depth_cue::OverlayLayer expected_layer,
                    float expected_opacity) {
    CHECK(decision.layer == expected_layer);
    CHECK(decision.opacity == doctest::Approx(expected_opacity).epsilon(1e-6));
}

} // namespace

TEST_CASE("depth cue overlay math follows boundary and interval rules") {
    depth_cue::DepthCueSettings settings{};
    settings.center_depth_offset = 0.0f;
    settings.foreground_max_depth_offset = -600.0f;
    settings.background_max_depth_offset = 600.0f;

    check_decision(depth_cue::evaluate_overlay_opacity(-900.0f, settings),
                   depth_cue::OverlayLayer::Foreground,
                   1.0f);
    check_decision(depth_cue::evaluate_overlay_opacity(-600.0f, settings),
                   depth_cue::OverlayLayer::Foreground,
                   1.0f);
    check_decision(depth_cue::evaluate_overlay_opacity(-300.0f, settings),
                   depth_cue::OverlayLayer::Foreground,
                   0.5f);
    check_decision(depth_cue::evaluate_overlay_opacity(0.0f, settings),
                   depth_cue::OverlayLayer::None,
                   0.0f);
    check_decision(depth_cue::evaluate_overlay_opacity(300.0f, settings),
                   depth_cue::OverlayLayer::Background,
                   0.5f);
    check_decision(depth_cue::evaluate_overlay_opacity(600.0f, settings),
                   depth_cue::OverlayLayer::Background,
                   1.0f);
    check_decision(depth_cue::evaluate_overlay_opacity(900.0f, settings),
                   depth_cue::OverlayLayer::Background,
                   1.0f);
}

TEST_CASE("depth cue overlay math shifts interpolation windows with center offset") {
    depth_cue::DepthCueSettings settings{};
    settings.center_depth_offset = 200.0f;
    settings.foreground_max_depth_offset = -200.0f;
    settings.background_max_depth_offset = 1000.0f;

    check_decision(depth_cue::evaluate_overlay_opacity(200.0f, settings),
                   depth_cue::OverlayLayer::None,
                   0.0f);
    check_decision(depth_cue::evaluate_overlay_opacity(0.0f, settings),
                   depth_cue::OverlayLayer::Foreground,
                   0.5f);
    check_decision(depth_cue::evaluate_overlay_opacity(600.0f, settings),
                   depth_cue::OverlayLayer::Background,
                   0.5f);
}

TEST_CASE("depth cue overlay math responds to effective world-z offset input") {
    depth_cue::DepthCueSettings settings{};
    settings.center_depth_offset = 0.0f;
    settings.foreground_max_depth_offset = -200.0f;
    settings.background_max_depth_offset = 200.0f;

    const float anchor_world_z = 1000.0f;
    const float asset_world_z = 1100.0f;
    const float no_offset_signed_depth = asset_world_z - anchor_world_z;
    const float with_offset_signed_depth = (asset_world_z - 300.0f) - anchor_world_z;

    check_decision(depth_cue::evaluate_overlay_opacity(no_offset_signed_depth, settings),
                   depth_cue::OverlayLayer::Background,
                   0.5f);
    check_decision(depth_cue::evaluate_overlay_opacity(with_offset_signed_depth, settings),
                   depth_cue::OverlayLayer::Foreground,
                   1.0f);
}

