#include <doctest/doctest.h>

#include <filesystem>
#include <string>

#include <nlohmann/json.hpp>

#include "assets/Asset.hpp"
#include "core/manifest/map_data.hpp"
#include "core/manifest/manifest_loader.hpp"
#include "devtools/core/manifest_store.hpp"
#include "gameplay/world/chunk.hpp"
#include "gameplay/world/grid_point.hpp"
#include "gameplay/world/world_grid.hpp"

namespace {

struct FakeManifestBackend {
    FakeManifestBackend()
        : data(nlohmann::json::object({
              {"version", 2},
              {"assets", nlohmann::json::object()},
              {"maps", nlohmann::json::object()}
          })) {}

    nlohmann::json data;
    bool flushed = false;
};

manifest::ManifestData make_manifest_data_from(const nlohmann::json& snapshot) {
    manifest::ManifestData manifest_data;
    manifest_data.raw = snapshot;
    manifest_data.assets = manifest_data.raw["assets"];
    manifest_data.maps = manifest_data.raw["maps"];
    return manifest_data;
}

} // namespace

TEST_CASE("GridPoint projection round-trips via camera params") {
    world::WorldGrid grid;
    const axis::WorldPos original_pos{32, 5, 64};
    world::GridPoint original = world::GridPoint::make_virtual(original_pos, 1);

    world::CameraProjectionParams params;
    params.position_x = 0.0;
    params.position_y = 3.5;
    params.position_z = -10.0;
    params.forward_x = 0.0;
    params.forward_y = 0.0;
    params.forward_z = 1.0;
    params.right_x = 1.0;
    params.right_y = 0.0;
    params.right_z = 0.0;
    params.up_x = 0.0;
    params.up_y = 1.0;
    params.up_z = 0.0;
    params.screen_width = 1280;
    params.screen_height = 720;
    params.tan_half_fov_x = 0.7;
    params.tan_half_fov_y = 0.7;
    params.meters_scale = 1.0;
    params.screen_zoom = 1.0;
    params.anchor_world_x = 0.0;
    params.anchor_world_y = 0.0;
    params.anchor_world_z = 0.0;
    params.state_version = 1;

    original.project_to_screen(params);
    SDL_FPoint screen_pos = original.screen;
    REQUIRE(std::isfinite(screen_pos.x));
    REQUIRE(std::isfinite(screen_pos.y));

    world::GridPoint* round_trip = world::GridPoint::from_screen(screen_pos,
                                                                 static_cast<float>(original.world_y()),
                                                                 params,
                                                                 grid);
    REQUIRE(round_trip != nullptr);
    CHECK(round_trip->world_x() == original.world_x());
    CHECK(round_trip->world_y() == original.world_y());
    CHECK(round_trip->world_z() == original.world_z());
}

TEST_CASE("WorldGrid keys stay stable across repeated requests") {
    world::WorldGrid grid;
    const axis::WorldPos sample_pos{15, 2, 31};
    world::GridPoint sample = world::GridPoint::make_virtual(sample_pos, 3);
    const world::GridKey first_key = grid.grid_key_from_world(sample);
    const world::GridKey second_key = grid.grid_key_from_world(sample);
    CHECK(first_key == second_key);
    CHECK(first_key.x == sample_pos.x);
    CHECK(first_key.y == sample_pos.y);
    CHECK(first_key.z == sample_pos.z);

    world::Chunk* chunk = grid.ensure_chunk_from_world(sample);
    REQUIRE(chunk != nullptr);
    world::GridPoint& stored = grid.find_or_create_grid_point(first_key, chunk, nullptr);
    CHECK(stored.hash_key() == grid.hash_key(first_key));
    CHECK(first_key.layer == stored.resolution_layer());
}

TEST_CASE("ManifestStore map entry round-trip honors writes") {
    FakeManifestBackend storage;
    auto loader = [&storage]() {
        return make_manifest_data_from(storage.data);
    };

    auto submit = [&storage](const std::filesystem::path&, const nlohmann::json& json, int) {
        storage.data = json;
    };

    auto flush = [&storage]() {
        storage.flushed = true;
    };

    devmode::core::ManifestStore store("manifest.json", loader, submit, flush, 2);
    const nlohmann::json map_entry = {
        {"schema_version", manifest::kMapSchemaVersion},
        {"rooms_data", nlohmann::json::object()}
    };

    REQUIRE(store.update_map_entry("map_alpha", map_entry));
    CHECK(storage.data["maps"]["map_alpha"] == map_entry);

    store.flush();
    CHECK(storage.flushed);

    store.reload();
    const nlohmann::json* persisted = store.find_map_entry("map_alpha");
    REQUIRE(persisted != nullptr);
    CHECK(*persisted == map_entry);
}
