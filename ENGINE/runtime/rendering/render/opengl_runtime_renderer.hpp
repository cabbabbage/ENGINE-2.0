#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <deque>
#include <string>
#include <unordered_map>
#include <vector>

#include <SDL3/SDL.h>

#include "rendering/render/opengl_scene_frame_data.hpp"
#include "rendering/render/projected_sprite_frame.hpp"
#include "rendering/render/dof_blur_chain.hpp"

class Assets;
class Asset;
class WarpedScreenGrid;

namespace world {
struct Chunk;
}


namespace opengl_runtime_renderer_detail {

// Draw ordering contract (must match OpenGLRuntimeRenderer):
bool draw_packet_sort_predicate_floor(const GpuSpriteDrawPacket& lhs,
                                      const GpuSpriteDrawPacket& rhs);
bool draw_packet_sort_predicate_xy(const GpuSpriteDrawPacket& lhs,
                                   const GpuSpriteDrawPacket& rhs);

// Floor-pass compatibility tags are parsed for diagnostics only.
// They do not change pass routing under strict floor (XZ) / XY sprite separation.
bool info_requests_floor_pass_tag_for_diagnostics(const std::string& type,
                                                  const std::vector<std::string>& tags);
bool info_is_xy_sprite_pass_eligible(bool tillable);

bool build_floor_tile_draw_packets(const WarpedScreenGrid& camera,
                                   const std::vector<world::Chunk*>& chunks,
                                   std::uint32_t target_width,
                                   std::uint32_t target_height,
                                   std::vector<GpuSpriteDrawPacket>& out_packets);
bool build_xy_sprite_draw_packets(const WarpedScreenGrid& camera,
                                  const std::vector<Asset*>& visible_assets,
                                  std::uint32_t target_width,
                                  std::uint32_t target_height,
                                  std::vector<GpuSpriteDrawPacket>& out_xy_sprite_draws,
                                  std::string& out_error,
                                  const std::vector<double>* cached_depth_edges = nullptr);
bool build_sink_clipped_sprite_packet(const render_projection::ProjectedSpriteFrame& projected,
                                      float u0,
                                      float v0,
                                      float u1,
                                      float v1,
                                      float sink_height_offset_px,
                                      std::uint32_t target_width,
                                      std::uint32_t target_height,
                                      GpuSpriteDrawPacket& out_packet);
int classify_depth_layer_for_asset(const WarpedScreenGrid& camera,
                                   const Asset& asset,
                                   const std::vector<double>* cached_depth_edges = nullptr);
float far_background_bottom_screen_y(const WarpedScreenGrid& camera, std::uint32_t target_height);
const std::vector<Asset*>& select_visible_assets_for_gpu_frame(bool dev_mode,
                                                               bool focus_filter_active,
                                                               const std::vector<Asset*>& active_assets,
                                                               const std::vector<Asset*>& filtered_active_assets,
                                                               bool& out_used_active_fallback);

} // namespace opengl_runtime_renderer_detail

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

    bool render_frame(std::string& out_error,
                      SDL_Texture* ui_overlay_texture = nullptr,
                      double ui_overlay_prepare_ms = 0.0,
                      bool ui_overlay_active = false,
                      bool ui_overlay_redrawn = false);
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
                                    std::string& out_error,
                                    bool allow_dof_depth_layers = true) const;
    bool render_packet_batch(const std::vector<GpuSpriteDrawPacket>& packets,
                            std::uint32_t target_width,
                            std::uint32_t target_height,
                            std::string& out_error);
    bool ensure_depth_layer_targets(const GpuSceneFrameData& frame_data, std::string& out_error);
    void destroy_depth_layer_targets();
    bool ensure_far_background_textures();
    bool process_creation_queue(const GpuSceneFrameData& frame_data, std::string& out_error);
    void clear_creation_queue();
    void destroy_far_background_textures();
    bool render_far_background(const WarpedScreenGrid& camera,
                               std::uint32_t target_width,
                               std::uint32_t target_height,
                               std::string& out_error);
    std::vector<world::Chunk*> runtime_floor_chunks() const;
    SDL_Color resolve_runtime_floor_clear_color() const;

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
                                   std::array<SDL_Vertex, render_sink::kMaxClippedVertices>& out_vertices);

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
    std::uint32_t hold_after_target_resize_frames_remaining_ = 0;
    std::uint32_t hold_empty_scene_frames_remaining_ = 1;
    bool startup_scene_submission_established_ = false;
    std::uint32_t post_startup_empty_scene_frame_count_ = 0;
    int output_target_width_ = 1;
    int output_target_height_ = 1;
    SDL_Texture* floor_target_ = nullptr;
    SDL_Texture* xy_sprite_target_ = nullptr;
    SDL_Texture* composite_target_ = nullptr;
    SDL_Texture* far_background_sky_texture_ = nullptr;
    SDL_Texture* far_background_mountains_texture_ = nullptr;
    std::vector<int> cached_depth_layer_ids_{};
    std::unordered_map<int, SDL_Texture*> depth_layer_targets_{};
    mutable std::vector<GpuSpriteDrawPacket> scratch_floor_grid_overlay_draws_{};
    mutable std::vector<GpuSpriteDrawPacket> scratch_floor_marker_draws_{};
    mutable std::unordered_map<int, std::vector<GpuSpriteDrawPacket>> scratch_depth_xy_sprite_packets_{};
    mutable std::vector<int> scratch_depth_layer_ids_{};
    std::vector<int> scratch_active_layer_ids_{};
    std::vector<SDL_Vertex> scratch_batch_vertices_{};
    std::vector<int> scratch_batch_indices_{};
    dof_blur_chain::Renderer dof_blur_chain_{};
    double last_dof_path_ms_ = 0.0;
    struct CreationBudgetConfig {
        std::uint32_t max_creations_per_frame = 3;
        double max_creation_ms_per_frame = 2.5;
        std::uint32_t max_retry_count = 2;
    };
    struct DeferredCreationJob {
        enum class Type { MainTarget, DepthLayerTarget };
        Type type = Type::MainTarget;
        int layer_id = 0;
        std::string label;
        std::uint64_t enqueue_frame = 0;
        std::uint32_t retries = 0;
        std::uint64_t sequence = 0;
    };
    CreationBudgetConfig creation_budget_config_{};
    std::deque<DeferredCreationJob> deferred_creation_queue_{};
    std::uint64_t creation_job_sequence_ = 0;
};
