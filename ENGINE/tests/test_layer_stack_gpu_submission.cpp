#include <doctest/doctest.h>

#include <SDL3/SDL.h>

#include <cstdint>
#include <vector>

#include "rendering/render/layer_stack_renderer.hpp"

namespace {

class ScopedSdlVideo {
public:
    ScopedSdlVideo() : initialized_(SDL_InitSubSystem(SDL_INIT_VIDEO)) {}
    ~ScopedSdlVideo() {
        if (initialized_) {
            SDL_QuitSubSystem(SDL_INIT_VIDEO);
        }
    }

    bool initialized() const { return initialized_; }

private:
    bool initialized_ = false;
};

class ScopedRenderer {
public:
    ScopedRenderer() {
        if (!video_.initialized()) {
            return;
        }
        window_ = SDL_CreateWindow("layer_stack_gpu_submission_tests", 64, 64, SDL_WINDOW_HIDDEN);
        if (!window_) {
            return;
        }
        renderer_ = SDL_CreateRenderer(window_, nullptr);
        if (!renderer_) {
            renderer_ = SDL_CreateRenderer(window_, SDL_SOFTWARE_RENDERER);
        }
    }

    ~ScopedRenderer() {
        if (renderer_) {
            SDL_DestroyRenderer(renderer_);
            renderer_ = nullptr;
        }
        if (window_) {
            SDL_DestroyWindow(window_);
            window_ = nullptr;
        }
    }

    SDL_Renderer* get() const { return renderer_; }
    bool ready() const { return renderer_ != nullptr; }

private:
    ScopedSdlVideo video_{};
    SDL_Window* window_ = nullptr;
    SDL_Renderer* renderer_ = nullptr;
};

SDL_Texture* create_solid_texture(SDL_Renderer* renderer,
                                  int width,
                                  int height,
                                  SDL_Color color) {
    if (!renderer || width <= 0 || height <= 0) {
        return nullptr;
    }
    SDL_Texture* texture = SDL_CreateTexture(renderer,
                                             SDL_PIXELFORMAT_RGBA8888,
                                             SDL_TEXTUREACCESS_TARGET,
                                             width,
                                             height);
    if (!texture) {
        return nullptr;
    }
    SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
    SDL_Texture* previous_target = SDL_GetRenderTarget(renderer);
    if (!SDL_SetRenderTarget(renderer, texture)) {
        SDL_DestroyTexture(texture);
        return nullptr;
    }
    SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
    SDL_RenderClear(renderer);
    SDL_SetRenderTarget(renderer, previous_target);
    return texture;
}

render_pipeline::LayerBuildResult make_single_quad_build(SDL_Texture* texture) {
    render_pipeline::LayerBuildResult build{};
    build.valid = (texture != nullptr);
    build.layer_count = 1;
    build.player_layer_index = 0;
    build.non_empty_layers = {0};
    build.layers.resize(1);

    render_pipeline::LayerSubmission& layer = build.layers[0];
    layer.representative_depth = 0.0;
    layer.depth_min = 0.0;
    layer.depth_max = 0.0;
    layer.bounds_min_x = 0.0f;
    layer.bounds_min_y = 0.0f;
    layer.bounds_max_x = 63.0f;
    layer.bounds_max_y = 63.0f;

    build.materials.push_back(render_pipeline::DrawMaterial{texture, SDL_BLENDMODE_BLEND});
    const SDL_FColor white{1.0f, 1.0f, 1.0f, 1.0f};
    build.packed_vertices = {
        SDL_Vertex{SDL_FPoint{0.0f, 0.0f}, white, SDL_FPoint{0.0f, 0.0f}},
        SDL_Vertex{SDL_FPoint{63.0f, 0.0f}, white, SDL_FPoint{1.0f, 0.0f}},
        SDL_Vertex{SDL_FPoint{63.0f, 63.0f}, white, SDL_FPoint{1.0f, 1.0f}},
        SDL_Vertex{SDL_FPoint{0.0f, 63.0f}, white, SDL_FPoint{0.0f, 1.0f}}};
    build.packed_indices = {0, 1, 2, 0, 2, 3};

    render_pipeline::DrawPacket packet{};
    packet.vertex_offset = 0;
    packet.vertex_count = 4;
    packet.index_offset = 0;
    packet.index_count = 6;
    packet.material_index = 0;
    packet.layer_index = 0;
    packet.light_cluster_index = 0;
    packet.depth = 0.0f;
    build.packets.push_back(packet);

    build.gpu_packets.push_back(render_pipeline::GpuDrawPacketRecord{
        packet.index_offset,
        packet.index_count,
        packet.vertex_offset,
        packet.vertex_count,
        packet.material_index,
        packet.layer_index,
        packet.light_cluster_index,
        packet.depth});

    layer.packet_indices.push_back(0);
    layer.command_ranges.push_back(render_pipeline::DrawCommandRange{
        packet.material_index,
        packet.index_offset,
        packet.index_count,
        0,
        1});

    return build;
}

} // namespace

TEST_CASE("LayerStackRenderer keeps GPU submission buffers steady in stable frames") {
    ScopedRenderer renderer_scope;
    REQUIRE(renderer_scope.ready());
    SDL_Renderer* renderer = renderer_scope.get();
    REQUIRE(renderer != nullptr);

    SDL_Texture* sprite = create_solid_texture(renderer, 4, 4, SDL_Color{255, 255, 255, 255});
    REQUIRE(sprite != nullptr);

    LayerStackRenderer stack(renderer);
    stack.set_output_dimensions(64, 64);

    const render_pipeline::LayerBuildResult build = make_single_quad_build(sprite);
    REQUIRE(build.valid);

    const render_pipeline::LayerRenderResult first = stack.render(
        build,
        std::vector<LayerEffectProcessor::RuntimeLight>{},
        false,
        1.0f,
        1.0f);
    REQUIRE(first.valid);

    // Skip on platforms where SDL_Renderer does not expose an SDL_GPUDevice.
    if (!first.gpu_submission.active || first.gpu_submission.buffer_create_count < 5) {
        SDL_DestroyTexture(sprite);
        return;
    }

    const render_pipeline::LayerRenderResult second = stack.render(
        build,
        std::vector<LayerEffectProcessor::RuntimeLight>{},
        false,
        1.0f,
        1.0f);
    REQUIRE(second.valid);
    CHECK(second.gpu_submission.active);
    CHECK(second.gpu_submission.buffer_create_count == first.gpu_submission.buffer_create_count);
    CHECK(second.gpu_submission.buffer_destroy_count == first.gpu_submission.buffer_destroy_count);
    CHECK(second.gpu_submission.vertex_capacity_bytes == first.gpu_submission.vertex_capacity_bytes);
    CHECK(second.gpu_submission.index_capacity_bytes == first.gpu_submission.index_capacity_bytes);
    CHECK(second.gpu_submission.material_capacity_bytes == first.gpu_submission.material_capacity_bytes);
    CHECK(second.gpu_submission.light_capacity_bytes == first.gpu_submission.light_capacity_bytes);
    CHECK(second.gpu_submission.packet_capacity_bytes == first.gpu_submission.packet_capacity_bytes);

    SDL_DestroyTexture(sprite);
}

