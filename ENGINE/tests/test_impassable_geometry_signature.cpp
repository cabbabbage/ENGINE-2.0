#include <doctest/doctest.h>

#include <memory>
#include <string>
#include <vector>

#include "assets/asset/Asset.hpp"

namespace {

std::unique_ptr<Asset> make_asset_for_impassable_signature() {
    auto info = std::make_shared<AssetInfo>("impassable_signature_asset");
    info->type = "object";
    info->impassable_enabled = true;
    Area spawn_area("impassable_signature_test_area", 0);
    return std::make_unique<Asset>(info,
                                   spawn_area,
                                   SDL_Point{0, 0},
                                   0,
                                   std::string{},
                                   std::string{},
                                   0);
}

Asset::RuntimeImpassableShape make_runtime_shape(std::vector<SDL_FPoint> points) {
    Asset::RuntimeImpassableShape shape{};
    shape.id = "shape_a";
    shape.name = "Shape A";
    shape.enabled = true;
    shape.valid = true;
    shape.floor_points = std::move(points);
    shape.bottom_ring.reserve(shape.floor_points.size());
    shape.top_ring.reserve(shape.floor_points.size());

    float sum_x = 0.0f;
    float sum_z = 0.0f;
    for (const SDL_FPoint& point : shape.floor_points) {
        shape.bottom_ring.push_back(Asset::RuntimeBoxPoint3{point.x, 0.0f, point.y});
        shape.top_ring.push_back(Asset::RuntimeBoxPoint3{point.x, 100.0f, point.y});
        sum_x += point.x;
        sum_z += point.y;
    }

    if (!shape.floor_points.empty()) {
        const float inv_count = 1.0f / static_cast<float>(shape.floor_points.size());
        shape.centroid = Asset::RuntimeBoxPoint3{sum_x * inv_count, 50.0f, sum_z * inv_count};
    }
    return shape;
}

} // namespace

TEST_CASE("runtime impassable geometry signature is stable for unchanged shapes") {
    auto asset = make_asset_for_impassable_signature();
    REQUIRE(asset != nullptr);

    asset->test_set_current_impassable_shapes({
        make_runtime_shape({
            SDL_FPoint{0.0f, 0.0f},
            SDL_FPoint{64.0f, 0.0f},
            SDL_FPoint{32.0f, 64.0f},
        })
    });

    const auto first = asset->runtime_impassable_geometry_signature();
    const auto second = asset->runtime_impassable_geometry_signature();

    CHECK(first == second);
}

TEST_CASE("runtime impassable geometry signature changes when floor geometry changes") {
    auto asset = make_asset_for_impassable_signature();
    REQUIRE(asset != nullptr);

    asset->test_set_current_impassable_shapes({
        make_runtime_shape({
            SDL_FPoint{0.0f, 0.0f},
            SDL_FPoint{64.0f, 0.0f},
            SDL_FPoint{32.0f, 64.0f},
        })
    });
    const auto baseline = asset->runtime_impassable_geometry_signature();

    asset->test_set_current_impassable_shapes({
        make_runtime_shape({
            SDL_FPoint{12.0f, 8.0f},
            SDL_FPoint{76.0f, 8.0f},
            SDL_FPoint{44.0f, 72.0f},
        })
    });
    const auto moved = asset->runtime_impassable_geometry_signature();

    CHECK(moved != baseline);
}
