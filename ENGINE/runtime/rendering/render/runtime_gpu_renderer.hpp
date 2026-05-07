#pragma once

#include <SDL3/SDL.h>

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "rendering/render/gpu_scene_renderer.hpp"

class Assets;
class WarpedScreenGrid;
namespace world {
struct Chunk;
}

namespace runtime_gpu_renderer_detail {

bool build_map_floor_tile_draw_packets(const WarpedScreenGrid& camera,
                                       const std::vector<world::Chunk*>& chunks,
                                       std::uint32_t target_width,
                                       std::uint32_t target_height,
                                       std::vector<GpuSpriteDrawPacket>& out_packets);
void append_classified_sprite_draw_packet(bool floor_tagged,
                                          const GpuSpriteDrawPacket& packet,
                                          GpuSceneFrameData& in_out_frame_data);

} // namespace runtime_gpu_renderer_detail

class RuntimeGpuRenderer {
public:
    static std::unique_ptr<RuntimeGpuRenderer> Create(SDL_Renderer* renderer,
                                                      Assets* assets,
                                                      int screen_width,
                                                      int screen_height,
                                                      std::string& out_error);

    ~RuntimeGpuRenderer();

    RuntimeGpuRenderer(const RuntimeGpuRenderer&) = delete;
    RuntimeGpuRenderer& operator=(const RuntimeGpuRenderer&) = delete;

    void set_output_dimensions(int screen_width, int screen_height);

    bool render_frame(std::string& out_error);
    std::optional<SDL_Point> scene_target_size() const;

    bool ready() const { return gpu_scene_renderer_ != nullptr; }
    const std::string& present_mode() const;
    const std::string& backend_name() const;

private:
    RuntimeGpuRenderer(SDL_Renderer* renderer,
                       Assets* assets,
                       int screen_width,
                       int screen_height);

    bool initialize(std::string& out_error);
    bool ensure_scene_target(std::string& out_error);
    bool build_gpu_scene_frame_data(GpuSceneFrameData& out_data, std::string& out_error) const;
    std::vector<world::Chunk*> runtime_floor_chunks() const;

    SDL_Renderer* renderer_ = nullptr;
    Assets* assets_ = nullptr;
    int screen_width_ = 1;
    int screen_height_ = 1;


    std::unique_ptr<GpuSceneRenderer> gpu_scene_renderer_;
};
