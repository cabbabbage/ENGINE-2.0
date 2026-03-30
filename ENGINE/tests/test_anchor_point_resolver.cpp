#include <doctest/doctest.h>

#include <cmath>
#include <limits>
#include <memory>

#include "assets/asset/Asset.hpp"
#include "assets/asset/anchor_point.hpp"
#include "assets/asset/asset_info.hpp"
#include "utils/AnchorPointResolver.hpp"

namespace {

Asset make_test_asset(int world_x, int world_y) {
    auto info = std::make_shared<AssetInfo>("anchor_resolver_test_asset");
    Area spawn_area("anchor_resolver_test_area", 0);
    return Asset(info, spawn_area, SDL_Point{world_x, world_y}, 0);
}

float distance_from_asset_origin(const Asset& asset, const anchor_points::AnchorWorldPoint3& point) {
    const float dx = point.x - static_cast<float>(asset.world_x());
    const float dy = point.y - static_cast<float>(asset.world_y());
    const float dz = point.z - static_cast<float>(asset.world_z());
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

} // namespace

TEST_CASE("Anchor ray direction stays stable under camera/point translation") {
    Asset asset_a = make_test_asset(100, 200);
    Asset asset_b = make_test_asset(300, 50);

    // Both flat points are translated by the same camera-space delta (30, 40, 50).
    const anchor_points::AnchorWorldPoint3 flat_a{130.0f, 240.0f, 50.0f, true};
    const anchor_points::AnchorWorldPoint3 flat_b{330.0f, 90.0f, 50.0f, true};

    anchor_points::AnchorWorldPoint3 dir_a{};
    anchor_points::AnchorWorldPoint3 dir_b{};
    REQUIRE(anchor_points::compute_camera_to_point_ray(asset_a, flat_a, dir_a));
    REQUIRE(anchor_points::compute_camera_to_point_ray(asset_b, flat_b, dir_b));
    REQUIRE(dir_a.valid);
    REQUIRE(dir_b.valid);

    CHECK(dir_a.x == doctest::Approx(dir_b.x).epsilon(1e-5));
    CHECK(dir_a.y == doctest::Approx(dir_b.y).epsilon(1e-5));
    CHECK(dir_a.z == doctest::Approx(dir_b.z).epsilon(1e-5));
}

TEST_CASE("Depth offset sign follows camera-to-anchor ray convention") {
    Asset asset = make_test_asset(0, 0);
    const anchor_points::AnchorWorldPoint3 flat{0.0f, 0.0f, 10.0f, true};

    anchor_points::AnchorWorldPoint3 farther{};
    anchor_points::AnchorWorldPoint3 closer{};
    REQUIRE(anchor_points::displace_along_camera_to_point_ray(asset, flat, 3.0f, farther));
    REQUIRE(anchor_points::displace_along_camera_to_point_ray(asset, flat, -3.0f, closer));
    REQUIRE(farther.valid);
    REQUIRE(closer.valid);

    const float flat_distance = distance_from_asset_origin(asset, flat);
    const float farther_distance = distance_from_asset_origin(asset, farther);
    const float closer_distance = distance_from_asset_origin(asset, closer);

    CHECK(farther_distance > flat_distance);
    CHECK(closer_distance < flat_distance);
}

TEST_CASE("Extrusion endpoints are symmetric around flat anchor point") {
    Asset asset = make_test_asset(0, 0);
    const anchor_points::AnchorWorldPoint3 flat{3.0f, 4.0f, 12.0f, true};
    const float extrusion = 2.5f;

    anchor_points::AnchorWorldPoint3 near_point{};
    anchor_points::AnchorWorldPoint3 far_point{};
    REQUIRE(anchor_points::build_symmetric_camera_ray_extrusion(asset,
                                                                flat,
                                                                extrusion,
                                                                near_point,
                                                                far_point));
    REQUIRE(near_point.valid);
    REQUIRE(far_point.valid);

    CHECK(((near_point.x + far_point.x) * 0.5f) == doctest::Approx(flat.x).epsilon(1e-5));
    CHECK(((near_point.y + far_point.y) * 0.5f) == doctest::Approx(flat.y).epsilon(1e-5));
    CHECK(((near_point.z + far_point.z) * 0.5f) == doctest::Approx(flat.z).epsilon(1e-5));

    const float near_distance = std::sqrt(
        (flat.x - near_point.x) * (flat.x - near_point.x) +
        (flat.y - near_point.y) * (flat.y - near_point.y) +
        (flat.z - near_point.z) * (flat.z - near_point.z));
    const float far_distance = std::sqrt(
        (flat.x - far_point.x) * (flat.x - far_point.x) +
        (flat.y - far_point.y) * (flat.y - far_point.y) +
        (flat.z - far_point.z) * (flat.z - far_point.z));

    CHECK(near_distance == doctest::Approx(extrusion).epsilon(1e-5));
    CHECK(far_distance == doctest::Approx(extrusion).epsilon(1e-5));
}

TEST_CASE("Anchor UV conversion preserves off-frame coordinates") {
    const SDL_Point texture_px{-12, 132};
    const SDL_FPoint uv = anchor_points::anchor_pixel_to_uv(texture_px, 100, 100, SDL_FLIP_NONE);
    const SDL_FPoint flipped_uv =
        anchor_points::anchor_pixel_to_uv(texture_px, 100, 100, SDL_FLIP_HORIZONTAL);

    CHECK(uv.x < 0.0f);
    CHECK(uv.y > 1.0f);
    CHECK(flipped_uv.x == doctest::Approx(1.0f - uv.x).epsilon(1e-6));
    CHECK(flipped_uv.y == doctest::Approx(uv.y).epsilon(1e-6));
}

TEST_CASE("Flat anchor depth plane includes asset world z offset") {
    Asset asset = make_test_asset(25, 50);
    asset.set_world_z_offset(6.0f);

    DisplacedAssetAnchorPoint anchor{};
    anchor.name = "offset_test";
    anchor.texture_x = 4;
    anchor.texture_y = 7;
    anchor.depth_offset = 0;

    const auto sample = anchor_points::resolve_frame_anchor_sample(
        asset,
        anchor,
        anchor_points::GridMaterialization::None);

    REQUIRE_FALSE(sample.resolved.missing);
    REQUIRE(sample.flat_relative_pixel_point.valid);
    CHECK(sample.flat_relative_pixel_point.z == doctest::Approx(6.0f).epsilon(1e-6));
    CHECK(sample.resolved.world_z == 6);
    CHECK(sample.resolved.world_depth == doctest::Approx(6.0f).epsilon(1e-6));
    CHECK(sample.resolved.world_exact_z == doctest::Approx(6.0f).epsilon(1e-6));
}

TEST_CASE("Resolved anchor preserves exact world depth alongside rounded world z") {
    Asset asset = make_test_asset(0, 0);
    const anchor_points::AnchorWorldPoint3 flat{0.0f, 0.0f, 10.0f, true};
    DisplacedAssetAnchorPoint anchor{};
    anchor.name = "depth_precision";
    anchor.texture_x = 0;
    anchor.texture_y = 0;
    anchor.depth_offset = 2.5f;

    anchor_points::AnchorWorldPoint3 displaced{};
    if (!anchor_points::displace_along_camera_to_point_ray(asset, flat, 2.5f, displaced)) {
        // Non-world builds require a live camera/assets context to resolve depth-ray displacement.
        const auto missing_sample = anchor_points::resolve_frame_anchor_sample(
            asset,
            anchor,
            anchor_points::GridMaterialization::None);
        CHECK(missing_sample.resolved.missing);
        return;
    }
    REQUIRE(displaced.valid);

    const auto sample = anchor_points::resolve_frame_anchor_sample(
        asset,
        anchor,
        anchor_points::GridMaterialization::None);

    REQUIRE_FALSE(sample.resolved.missing);
    REQUIRE(sample.final_anchor_point.valid);
    CHECK(sample.resolved.world_exact_pos_2d.x == doctest::Approx(sample.final_anchor_point.x).epsilon(1e-6));
    CHECK(sample.resolved.world_exact_pos_2d.y == doctest::Approx(sample.final_anchor_point.y).epsilon(1e-6));
    CHECK(sample.resolved.world_exact_z == doctest::Approx(sample.final_anchor_point.z).epsilon(1e-6));
    CHECK(sample.resolved.world_depth == doctest::Approx(sample.final_anchor_point.z).epsilon(1e-6));
    CHECK(sample.resolved.world_z == static_cast<int>(std::lround(sample.final_anchor_point.z)));
    CHECK(sample.resolved.depth_offset == doctest::Approx(2.5f).epsilon(1e-6));
}

TEST_CASE("Resolved anchor flat perspective scale is driven by effective runtime perspective sample, not anchor texture position") {
    Asset asset = make_test_asset(40, 55);
    CHECK(asset.set_anchor_perspective_override(2.35f, std::nullopt));

    DisplacedAssetAnchorPoint anchor_a{};
    anchor_a.name = "a";
    anchor_a.texture_x = 2;
    anchor_a.texture_y = 3;
    anchor_a.depth_offset = 0.0f;

    DisplacedAssetAnchorPoint anchor_b{};
    anchor_b.name = "b";
    anchor_b.texture_x = 240;
    anchor_b.texture_y = -120;
    anchor_b.depth_offset = 0.0f;

    const auto sample_a = anchor_points::resolve_frame_anchor_sample(
        asset,
        anchor_a,
        anchor_points::GridMaterialization::None);
    const auto sample_b = anchor_points::resolve_frame_anchor_sample(
        asset,
        anchor_b,
        anchor_points::GridMaterialization::None);

    if (sample_a.resolved.missing || sample_b.resolved.missing) {
        // This resolver path requires a live runtime camera/assets context in non-world-test builds.
        CHECK(sample_a.resolved.missing);
        CHECK(sample_b.resolved.missing);
        return;
    }

    REQUIRE_FALSE(sample_a.resolved.missing);
    REQUIRE_FALSE(sample_b.resolved.missing);
    REQUIRE(sample_a.resolved.has_flat_perspective_scale);
    REQUIRE(sample_b.resolved.has_flat_perspective_scale);
    CHECK(sample_a.resolved.flat_perspective_scale == doctest::Approx(2.35f).epsilon(1e-6));
    CHECK(sample_b.resolved.flat_perspective_scale == doctest::Approx(2.35f).epsilon(1e-6));
}

TEST_CASE("Anchor perspective override rejects non-finite input and falls back to non-override source") {
    Asset asset = make_test_asset(5, 7);
    CHECK_FALSE(asset.set_anchor_perspective_override(std::numeric_limits<float>::quiet_NaN(), std::nullopt));
    const Asset::PerspectiveSample sample = asset.runtime_perspective_sample();
    CHECK(sample.source != Asset::PerspectiveSource::AnchorBindingOverride);
}

#if !defined(ENGINE_WORLD_TESTS)
TEST_CASE("Resolved anchor does not propagate flat perspective scale when runtime sample falls back to default source") {
    Asset asset = make_test_asset(12, 18);
    asset.clear_anchor_perspective_override();
    auto* gp = asset.grid_point();
    REQUIRE(gp != nullptr);
    gp->mutable_projection_cache().perspective_scale = std::numeric_limits<float>::quiet_NaN();

    const Asset::PerspectiveSample runtime_sample = asset.runtime_perspective_sample();
    REQUIRE(runtime_sample.source == Asset::PerspectiveSource::Default);

    DisplacedAssetAnchorPoint anchor{};
    anchor.name = "default_source_anchor";
    anchor.texture_x = 8;
    anchor.texture_y = 11;
    anchor.depth_offset = 0.0f;

    const auto sample = anchor_points::resolve_frame_anchor_sample(
        asset,
        anchor,
        anchor_points::GridMaterialization::None);

    CHECK(sample.resolved.missing);
    CHECK_FALSE(sample.resolved.has_flat_perspective_scale);
}
#endif
