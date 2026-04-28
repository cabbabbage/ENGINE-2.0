#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>
#include <vector>

#include "gameplay/map_generation/room.hpp"
#include "rendering/render/layer_submission_builder.hpp"
#include "rendering/render/render.hpp"
#include "rendering/render/warped_screen_grid.hpp"

namespace {

Area make_starting_area() {
    std::vector<Area::Point> corners{
        Area::Point{-1600, -1200},
        Area::Point{1600, -1200},
        Area::Point{1600, 1200},
        Area::Point{-1600, 1200}
    };
    return Area("layer_submission_packet_test_area", corners, 0);
}

void set_quad_vertices(SDL_Vertex (&vertices)[4], float x, float y, float w, float h) {
    const SDL_FColor white{1.0f, 1.0f, 1.0f, 1.0f};
    vertices[0] = SDL_Vertex{SDL_FPoint{x, y}, white, SDL_FPoint{0.0f, 0.0f}};
    vertices[1] = SDL_Vertex{SDL_FPoint{x + w, y}, white, SDL_FPoint{1.0f, 0.0f}};
    vertices[2] = SDL_Vertex{SDL_FPoint{x + w, y + h}, white, SDL_FPoint{1.0f, 1.0f}};
    vertices[3] = SDL_Vertex{SDL_FPoint{x, y + h}, white, SDL_FPoint{0.0f, 1.0f}};
}

} // namespace

TEST_CASE("Layer submission builds compact packet buffers without per-layer draw copies") {
    WarpedScreenGrid grid(1280, 720, make_starting_area());
    grid.set_screen_center(SDL_Point{0, 350}, true);

    GeometryBatcher geometry_batcher(nullptr);
    LayerSubmissionBuilder builder;
    constexpr double kMaxCullDepth = 500.0;
    static constexpr int kQuadIndices[6] = {0, 1, 2, 0, 2, 3};
    SDL_Texture* fake_texture = reinterpret_cast<SDL_Texture*>(static_cast<std::uintptr_t>(0x1));

    SDL_Vertex quad_a[4]{};
    SDL_Vertex quad_b[4]{};
    SDL_Vertex quad_c[4]{};
    set_quad_vertices(quad_a, 10.0f, 10.0f, 24.0f, 24.0f);
    set_quad_vertices(quad_b, 40.0f, 10.0f, 24.0f, 24.0f);
    set_quad_vertices(quad_c, 70.0f, 10.0f, 24.0f, 24.0f);
    geometry_batcher.addQuad(fake_texture, quad_a, kQuadIndices, SDL_BLENDMODE_BLEND, -120.0);
    geometry_batcher.addQuad(fake_texture, quad_b, kQuadIndices, SDL_BLENDMODE_BLEND, 0.0);
    geometry_batcher.addQuad(fake_texture, quad_c, kQuadIndices, SDL_BLENDMODE_BLEND, 180.0);

    const render_pipeline::LayerBuildResult build =
        builder.build(geometry_batcher, grid, grid.anchor_world_z(), kMaxCullDepth);

    REQUIRE(build.valid);
    CHECK(build.packets.size() == 3);
    CHECK(build.gpu_packets.size() == build.packets.size());
    CHECK(build.materials.size() == 1);
    CHECK(build.packed_vertices.size() == 12);
    CHECK(build.packed_indices.size() == 18);

    std::size_t packet_index_total = 0;
    for (int layer_index : build.non_empty_layers) {
        REQUIRE(layer_index >= 0);
        REQUIRE(layer_index < build.layer_count);
        const render_pipeline::LayerSubmission& layer = build.layers[static_cast<std::size_t>(layer_index)];
        CHECK(layer.draws.empty());
        CHECK(!layer.packet_indices.empty());
        CHECK(!layer.command_ranges.empty());
        packet_index_total += layer.packet_indices.size();
        for (const render_pipeline::DrawCommandRange& range : layer.command_ranges) {
            CHECK(range.index_count > 0);
            CHECK(range.packet_count > 0);
            CHECK(range.material_index < build.materials.size());
            CHECK(range.index_offset + range.index_count <= build.packed_indices.size());
        }
    }
    CHECK(packet_index_total == build.packets.size());

    for (const render_pipeline::DrawPacket& packet : build.packets) {
        CHECK(packet.vertex_count == 4);
        CHECK(packet.index_count == 6);
        CHECK(packet.material_index < build.materials.size());
        CHECK(packet.layer_index < static_cast<std::uint32_t>(build.layer_count));
        CHECK(packet.light_cluster_index == packet.layer_index);
        CHECK(packet.vertex_offset + packet.vertex_count <= build.packed_vertices.size());
        CHECK(packet.index_offset + packet.index_count <= build.packed_indices.size());
        CHECK(std::isfinite(packet.depth));
    }
}

