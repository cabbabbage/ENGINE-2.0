#include <doctest/doctest.h>

#include <cmath>
#include <memory>

#include "assets/asset/Asset.hpp"
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
