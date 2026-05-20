#pragma once

#include <SDL3/SDL.h>

#include <cstdint>
#include <string>

struct RenderFrameStats {
    std::uint64_t frame_index = 0;
    double frame_cpu_ms = 0.0;
    double render_thread_cpu_ms = 0.0;
    std::uint32_t render_pass_count = 0;
    std::uint32_t copy_pass_count = 0;
    std::uint32_t compute_pass_count = 0;
    std::uint32_t draw_call_count = 0;
    std::uint32_t texture_create_count = 0;
    std::uint32_t texture_destroy_count = 0;
    std::uint32_t gpu_buffer_create_count = 0;
    std::uint32_t gpu_buffer_destroy_count = 0;
    std::uint32_t render_target_switch_count = 0;
    double cpu_light_gather_ms = 0.0;
    double cpu_light_mask_generation_ms = 0.0;
    double draw_submission_cpu_ms = 0.0;
    double draw_submission_packet_build_sort_ms = 0.0;
    double draw_submission_resource_create_ms = 0.0;
    double draw_submission_pipeline_bind_ms = 0.0;
    double draw_submission_submit_present_handoff_ms = 0.0;
    std::uint32_t draw_submission_packet_build_count = 0;
    std::uint32_t draw_submission_resource_create_count = 0;
    std::uint32_t draw_submission_pipeline_bind_count = 0;
    std::uint32_t draw_submission_submit_handoff_count = 0;
    double ui_overlay_prepare_ms = 0.0;
    double present_block_ms = 0.0;
    double present_interval_ms = 0.0;
    bool present_interval_known = false;
    std::string renderer_path;
    std::string backend_name;
    std::string present_mode;
    std::uint64_t texture_memory_bytes = 0;
    bool texture_memory_known = false;
    std::uint32_t gpu_light_tile_assignments = 0;
    std::uint32_t gpu_light_naive_evaluations = 0;
    std::uint32_t gpu_light_tiled_evaluations = 0;
    std::uint64_t gpu_pipeline_cache_hits = 0;
    std::uint64_t gpu_pipeline_cache_misses = 0;
    double gpu_pipeline_cache_hit_rate = 1.0;
    std::uint32_t sdl_renderer_target_call_count = 0;
    std::uint32_t sdl_renderer_draw_call_count = 0;
    std::uint32_t present_call_count = 0;
    std::uint32_t gpu_failed_frame_count = 0;
    std::uint32_t gpu_scene_floor_draw_count = 0;
    std::uint32_t gpu_scene_xy_sprite_draw_count = 0;
    bool gpu_scene_composite_source_ready = false;
    bool command_buffer_acquired = false;
    bool swapchain_acquired = false;
    std::uint32_t swapchain_width = 0;
    std::uint32_t swapchain_height = 0;
    std::uint32_t floor_pass_target_width = 0;
    std::uint32_t floor_pass_target_height = 0;
    std::uint32_t xy_sprite_pass_target_width = 0;
    std::uint32_t xy_sprite_pass_target_height = 0;
    bool clear_executed = false;
    std::uint32_t floor_packet_count = 0;
    std::uint32_t xy_sprite_packet_count = 0;
    std::uint32_t active_depth_layer_count = 0;
    std::uint32_t blur_pass_count = 0;
    std::uint32_t skipped_texture_count = 0;
    std::string failed_texture_names;
    std::string packets_per_depth_layer;
    std::string blur_strength_per_layer;
    std::string composite_layers_submitted;
    std::string render_stage_timings;
    bool ui_overlay_active = false;
    bool ui_overlay_redrawn = false;
    bool submit_succeeded = false;
    std::uint32_t projection_calls_total = 0;
    std::uint32_t projection_calls_saved_early = 0;
    std::uint32_t assets_stageA_reject = 0;
    std::uint32_t assets_stageC_entered = 0;
    std::uint32_t projection_recompute_budget = 0;
    std::uint32_t projection_points_deferred = 0;
    std::uint32_t projection_points_updated = 0;
};

namespace render_diagnostics {

void begin_frame();
void end_frame();
const RenderFrameStats& current_frame_stats();

void add_render_pass(std::uint32_t count = 1);
void add_copy_pass(std::uint32_t count = 1);
void add_compute_pass(std::uint32_t count = 1);
void add_draw_call_count(std::uint32_t count = 1);
void add_texture_create_count(std::uint32_t count = 1);
void add_texture_destroy_count(std::uint32_t count = 1);
void add_gpu_buffer_create_count(std::uint32_t count = 1);
void add_gpu_buffer_destroy_count(std::uint32_t count = 1);
void add_render_target_switch_count(std::uint32_t count = 1);
void add_cpu_light_gather_ms(double elapsed_ms);
void add_cpu_light_mask_generation_ms(double elapsed_ms);
void add_draw_submission_ms(double elapsed_ms);
void add_draw_submission_packet_build_sort_ms(double elapsed_ms, std::uint32_t packet_count = 0);
void add_draw_submission_resource_create_ms(double elapsed_ms, std::uint32_t create_count = 0);
void add_draw_submission_pipeline_bind_ms(double elapsed_ms, std::uint32_t bind_count = 0);
void add_draw_submission_submit_present_handoff_ms(double elapsed_ms, std::uint32_t handoff_count = 0);
void set_ui_overlay_stats(bool active, bool redrawn, double prepare_ms);
void set_present_pacing(double present_block_ms,
                        double present_interval_ms,
                        bool interval_known);
void set_renderer_runtime_info(const std::string& renderer_path,
                               const std::string& backend_name,
                               const std::string& present_mode);
void set_texture_memory_usage(std::uint64_t bytes, bool known);
void set_render_thread_cpu_ms(double elapsed_ms);
void set_gpu_light_culling_stats(std::uint32_t tile_assignments,
                                 std::uint32_t naive_evaluations,
                                 std::uint32_t tiled_evaluations);
void set_gpu_pipeline_cache_stats(std::uint64_t hits,
                                  std::uint64_t misses,
                                  double hit_rate);
void note_present_call(std::uint32_t count = 1);
void note_gpu_frame_skipped_due_to_failure(std::uint32_t count = 1);
void set_gpu_scene_packet_stats(std::uint32_t floor_draw_count,
                                std::uint32_t xy_sprite_draw_count,
                                bool composite_source_ready);
void set_command_buffer_acquired(bool acquired);
void set_swapchain_acquired(bool acquired);
void set_swapchain_dimensions(std::uint32_t width, std::uint32_t height);
void set_floor_pass_target_dimensions(std::uint32_t width, std::uint32_t height);
void set_xy_sprite_pass_target_dimensions(std::uint32_t width, std::uint32_t height);
void set_clear_executed(bool executed);
void set_pass_packet_counts(std::uint32_t floor_packets, std::uint32_t xy_sprite_packets);
void set_active_depth_layer_count(std::uint32_t count);
void set_blur_pass_count(std::uint32_t count);
void set_packets_per_depth_layer(const std::string& summary);
void set_blur_strength_per_layer(const std::string& summary);
void set_composite_layers_submitted(const std::string& summary);
void set_render_stage_timings(const std::string& summary);
void add_skipped_texture_count(std::uint32_t count = 1);
void set_failed_texture_names(const std::string& names);
void set_submit_result(bool succeeded);
void set_visibility_projection_stats(std::uint32_t projection_calls_total,
                                    std::uint32_t projection_calls_saved_early,
                                    std::uint32_t assets_stageA_reject,
                                    std::uint32_t assets_stageC_entered,
                                    std::uint32_t projection_recompute_budget,
                                    std::uint32_t projection_points_deferred,
                                    std::uint32_t projection_points_updated);
void note_texture_created(SDL_Texture* texture);
void note_texture_destroyed(SDL_Texture* texture);
void destroy_texture(SDL_Texture*& texture);
std::uint64_t tracked_texture_bytes();

SDL_Texture* create_texture(SDL_Renderer* renderer,
                            SDL_PixelFormat format,
                            SDL_TextureAccess access,
                            int w,
                            int h);
SDL_Texture* create_frame_graph_texture(SDL_Renderer* renderer,
                                        const std::string& resource_name,
                                        SDL_PixelFormat format,
                                        SDL_TextureAccess access,
                                        int w,
                                        int h);
bool set_render_target(SDL_Renderer* renderer, SDL_Texture* texture);
bool render_texture(SDL_Renderer* renderer,
                    SDL_Texture* texture,
                    const SDL_FRect* srcrect,
                    const SDL_FRect* dstrect);
bool render_geometry(SDL_Renderer* renderer,
                     SDL_Texture* texture,
                     const SDL_Vertex* vertices,
                     int num_vertices,
                     const int* indices,
                     int num_indices);

enum class CpuTimerMetric {
    LightGather,
    LightMaskGeneration
};

class ScopedCpuTimer {
public:
    explicit ScopedCpuTimer(CpuTimerMetric metric);
    ~ScopedCpuTimer();

    ScopedCpuTimer(const ScopedCpuTimer&) = delete;
    ScopedCpuTimer& operator=(const ScopedCpuTimer&) = delete;

private:
    CpuTimerMetric metric_;
    std::uint64_t begin_counter_ = 0;
};

} // namespace render_diagnostics
