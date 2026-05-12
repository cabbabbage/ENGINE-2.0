#pragma once

#include <SDL3/SDL.h>

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "rendering/render/gpu_scene_renderer.hpp"

class Assets;
class Asset;
class WarpedScreenGrid;
namespace world {
struct Chunk;
}

namespace runtime_gpu_renderer_detail {

// Draw ordering contract (must match RuntimeGpuRenderer and OpenGLRuntimeRenderer):
// - Pass classification: packets resolve to floor pass only when explicit floor intent is
//   present (manifest render-pass floor tags) and floor boxes are enabled, otherwise layer pass.
// - Depth-layer grouping: only layer pass packets are grouped by packet.depth_layer and consumed
//   from highest layer id to lowest layer id.
// - In-layer ordering: packets are ordered by (sort_key with epsilon grouping, depth_metric,
//   stable_sort_id), where stable_sort_id is the final deterministic tie-breaker.
bool draw_packets_share_sort_key(float lhs, float rhs);
bool draw_packet_sort_predicate(const GpuSpriteDrawPacket& lhs,
                                const GpuSpriteDrawPacket& rhs);

bool build_floor_tile_draw_packets(const WarpedScreenGrid& camera,
                                   const std::vector<world::Chunk*>& chunks,
                                   std::uint32_t target_width,
                                   std::uint32_t target_height,
                                   std::vector<GpuSpriteDrawPacket>& out_packets);
bool build_floor_sprite_draw_packets(const WarpedScreenGrid& camera,
                                     const std::vector<Asset*>& visible_assets,
                                     std::uint32_t target_width,
                                     std::uint32_t target_height,
                                     std::vector<GpuSpriteDrawPacket>& out_floor_draws,
                                     std::vector<GpuSpriteDrawPacket>& out_layer_draws,
                                     std::string& out_error);
int classify_depth_layer_for_asset(const WarpedScreenGrid& camera, const Asset& asset);
void append_classified_sprite_draw_packet(bool floor_tagged,
                                          const GpuSpriteDrawPacket& packet,
                                          std::vector<GpuSpriteDrawPacket>& out_floor_draws,
                                          std::vector<GpuSpriteDrawPacket>& out_layer_draws);
const std::vector<Asset*>& select_visible_assets_for_gpu_frame(bool dev_mode,
                                                               bool focus_filter_active,
                                                               const std::vector<Asset*>& active_assets,
                                                               const std::vector<Asset*>& filtered_active_assets,
                                                               bool& out_used_active_fallback);

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

    bool render_frame(std::string& out_error, SDL_Texture* ui_overlay_texture = nullptr);
    std::optional<SDL_Point> scene_target_size() const;

    bool ready() const { return backend_owner_.ready(); }
    const std::string& present_mode() const;
    const std::string& backend_name() const;

private:
    struct FrameStats {
        std::uint32_t render_pass_count = 0;
        std::uint32_t floor_draw_count = 0;
        std::uint32_t layer_sprite_draw_count = 0;
        std::uint32_t debug_overlay_draw_count = 0;
        std::uint32_t draw_call_count = 0;
    };

    struct FrameContext {
        SDL_GPUCommandBuffer* command_buffer = nullptr;
        SDL_GPUTexture* swapchain_texture = nullptr;
        Uint32 swapchain_width = 0;
        Uint32 swapchain_height = 0;
        FrameStats stats{};
    };

    struct RenderTargetLifecycleManager {
        int requested_width = 1;
        int requested_height = 1;
        int active_width = 1;
        int active_height = 1;

        void set_requested_size(int screen_width, int screen_height);
        bool synchronize_to_swapchain(Uint32 swapchain_width,
                                      Uint32 swapchain_height,
                                      std::string& out_error);
        std::optional<SDL_Point> current_size() const;
    };

    struct GpuBackendOwner {
        std::unique_ptr<GpuSceneRenderer> scene_renderer;

        GpuSceneRenderer* get() const { return scene_renderer.get(); }
        bool ready() const { return scene_renderer != nullptr; }
    };

    RuntimeGpuRenderer(SDL_Renderer* renderer,
                       Assets* assets,
                       int screen_width,
                       int screen_height);

    bool initialize(std::string& out_error);
    bool ensure_scene_target(std::string& out_error);
    bool validate_preloaded_sprite_textures(std::string& out_error) const;
    bool build_gpu_scene_frame_data(std::uint32_t target_width,
                                    std::uint32_t target_height,
                                    GpuSceneFrameData& out_data,
                                    std::string& out_error) const;
    bool render_draw_packet_batch(SDL_GPURenderPass* render_pass,
                                  const std::vector<GpuSpriteDrawPacket>& packets,
                                  const char* pass_label,
                                  std::string& out_error);
    std::vector<world::Chunk*> runtime_floor_chunks() const;

    SDL_Renderer* renderer_ = nullptr;
    Assets* assets_ = nullptr;
    int screen_width_ = 1;
    int screen_height_ = 1;
    GpuBackendOwner backend_owner_{};
    FrameContext frame_context_{};
    RenderTargetLifecycleManager render_target_manager_{};
    std::optional<GpuSceneFrameData> last_complete_scene_frame_data_{};
    std::uint32_t last_complete_scene_width_ = 0;
    std::uint32_t last_complete_scene_height_ = 0;
    std::uint32_t consecutive_held_incomplete_scene_frames_ = 0;
};
