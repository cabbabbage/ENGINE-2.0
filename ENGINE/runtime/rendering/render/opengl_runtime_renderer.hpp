#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <SDL3/SDL.h>

#include "rendering/render/runtime_gpu_renderer.hpp"

class Assets;
class Asset;
class WarpedScreenGrid;

namespace world {
struct Chunk;
}

class OpenGLRuntimeRenderer {
public:
    static std::unique_ptr<OpenGLRuntimeRenderer> Create(SDL_Renderer* renderer,
                                                         Assets* assets,
                                                         int screen_width,
                                                         int screen_height,
                                                         std::string& out_error);

    ~OpenGLRuntimeRenderer();

    OpenGLRuntimeRenderer(const OpenGLRuntimeRenderer&) = delete;
    OpenGLRuntimeRenderer& operator=(const OpenGLRuntimeRenderer&) = delete;

    void set_output_dimensions(int screen_width, int screen_height);

    bool render_frame(std::string& out_error, SDL_Texture* ui_overlay_texture = nullptr);
    std::optional<SDL_Point> scene_target_size() const;

    bool ready() const { return renderer_ != nullptr; }
    const std::string& present_mode() const;
    const std::string& backend_name() const;

private:
    struct RenderTargetLifecycleManager {
        int requested_width = 1;
        int requested_height = 1;
        int active_width = 1;
        int active_height = 1;

        void set_requested_size(int screen_width, int screen_height);
        bool synchronize_to_output(int width, int height, std::string& out_error);
        std::optional<SDL_Point> current_size() const;
    };

    OpenGLRuntimeRenderer(SDL_Renderer* renderer,
                          Assets* assets,
                          int screen_width,
                          int screen_height);

    bool initialize(std::string& out_error);
    void destroy_render_targets();
    bool ensure_render_targets(const GpuSceneFrameData& frame_data, std::string& out_error);
    bool build_gpu_scene_frame_data(std::uint32_t target_width,
                                    std::uint32_t target_height,
                                    GpuSceneFrameData& out_data,
                                    std::string& out_error) const;
    bool render_packet_batch(const std::vector<GpuSpriteDrawPacket>& packets,
                            std::uint32_t target_width,
                            std::uint32_t target_height,
                            std::string& out_error);
    std::vector<world::Chunk*> runtime_floor_chunks() const;

    static SDL_Texture* create_render_target(SDL_Renderer* renderer,
                                             int width,
                                             int height,
                                             const std::string& label,
                                             std::string& out_error);
    static void configure_render_target(SDL_Texture* texture);
    static SDL_FPoint clip_to_screen(float clip_x, float clip_y, float target_width, float target_height);
    static void packet_to_vertices(const GpuSpriteDrawPacket& packet,
                                   std::uint32_t target_width,
                                   std::uint32_t target_height,
                                   std::array<SDL_Vertex, 6>& out_vertices);

    SDL_Renderer* renderer_ = nullptr;
    Assets* assets_ = nullptr;
    int screen_width_ = 1;
    int screen_height_ = 1;
    std::string renderer_name_ = "unknown";
    std::string present_mode_name_ = "unknown";
    RenderTargetLifecycleManager render_target_manager_{};
    std::optional<GpuSceneFrameData> last_complete_scene_frame_data_{};
    std::uint32_t last_complete_scene_width_ = 0;
    std::uint32_t last_complete_scene_height_ = 0;
    std::uint32_t consecutive_held_incomplete_scene_frames_ = 0;
    int output_target_width_ = 1;
    int output_target_height_ = 1;
    SDL_Texture* floor_target_ = nullptr;
    SDL_Texture* composite_target_ = nullptr;
    std::vector<int> cached_depth_layer_ids_{};
    std::unordered_map<int, SDL_Texture*> depth_layer_targets_{};
};
