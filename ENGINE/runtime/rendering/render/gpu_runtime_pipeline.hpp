#pragma once

#include <SDL3/SDL.h>

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

class GpuSceneRenderer;

struct GpuSpriteVertex {
    float clip_x = 0.0f;
    float clip_y = 0.0f;
    float uv_x = 0.0f;
    float uv_y = 0.0f;
};

struct GpuSpriteDrawPacket {
    SDL_Texture* source_texture = nullptr;
    std::array<GpuSpriteVertex, 6> vertices{};
    SDL_FColor modulate{1.0f, 1.0f, 1.0f, 1.0f};
};

struct GpuSceneFrameData {
    std::vector<GpuSpriteDrawPacket> floor_draws{};
    std::vector<GpuSpriteDrawPacket> layer_draws{};
    std::uint32_t floor_draw_count = 0;
    std::uint32_t layer_sprite_draw_count = 0;
    std::uint32_t debug_overlay_draw_count = 0;
    bool has_valid_composite_source = false;
};

class GpuRuntimePipeline {
public:
    bool ensure_resources(GpuSceneRenderer& renderer,
                          std::uint32_t width,
                          std::uint32_t height,
                          std::string& out_error) const;
    bool ensure_shared_resources(GpuSceneRenderer& renderer, std::string& out_error) const;
    bool enqueue_frame_graph(GpuSceneRenderer& renderer,
                             const GpuSceneFrameData& frame_data,
                             std::string_view pass_name_prefix,
                             std::uint32_t width,
                             std::uint32_t height,
                             std::string& out_error) const;
};
