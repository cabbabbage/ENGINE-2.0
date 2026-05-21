#include "rendering/render/render_diagnostics.hpp"
#include "utils/log.hpp"

#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace {
RenderFrameStats g_frame_stats{};
std::uint64_t g_frame_begin_counter = 0;
std::uint64_t g_perf_frequency = 0;
SDL_Texture* g_last_render_target = nullptr;
double g_last_present_block_ms = 0.0;
double g_last_present_interval_ms = 0.0;
bool g_last_present_interval_known = false;
bool g_has_present_sample = false;
std::unordered_map<SDL_Texture*, std::uint64_t> g_texture_size_bytes{};
std::uint64_t g_tracked_texture_bytes = 0;

std::uint64_t estimate_texture_bytes(SDL_Texture* texture) {
    if (!texture) {
        return 0;
    }

    float width = 0.0f;
    float height = 0.0f;
    if (!SDL_GetTextureSize(texture, &width, &height)) {
        return 0;
    }
    const std::uint64_t w = static_cast<std::uint64_t>(std::max(0.0, static_cast<double>(std::ceil(width))));
    const std::uint64_t h = static_cast<std::uint64_t>(std::max(0.0, static_cast<double>(std::ceil(height))));
    if (w == 0 || h == 0) {
        return 0;
    }

    SDL_PixelFormat format = SDL_PIXELFORMAT_RGBA32;
    if (SDL_PropertiesID props = SDL_GetTextureProperties(texture)) {
        format = static_cast<SDL_PixelFormat>(
            SDL_GetNumberProperty(props, SDL_PROP_TEXTURE_FORMAT_NUMBER, static_cast<Sint64>(format)));
    }
    const SDL_PixelFormatDetails* format_details = SDL_GetPixelFormatDetails(format);
    const std::uint64_t bytes_per_pixel =
        (format_details && format_details->bytes_per_pixel > 0)
            ? static_cast<std::uint64_t>(format_details->bytes_per_pixel)
            : 4ull;

    return w * h * bytes_per_pixel;
}

void track_texture_create(SDL_Texture* texture) {
    if (!texture || g_texture_size_bytes.find(texture) != g_texture_size_bytes.end()) {
        return;
    }
    const std::uint64_t bytes = estimate_texture_bytes(texture);
    g_texture_size_bytes.emplace(texture, bytes);
    g_tracked_texture_bytes += bytes;
}

void track_texture_destroy(SDL_Texture* texture) {
    if (!texture) {
        return;
    }
    const auto it = g_texture_size_bytes.find(texture);
    if (it == g_texture_size_bytes.end()) {
        return;
    }
    const std::uint64_t bytes = it->second;
    g_tracked_texture_bytes = (g_tracked_texture_bytes >= bytes) ? (g_tracked_texture_bytes - bytes) : 0;
    g_texture_size_bytes.erase(it);
}

void ensure_perf_frequency() {
    if (g_perf_frequency == 0) {
        g_perf_frequency = SDL_GetPerformanceFrequency();
    }
}

double elapsed_ms(std::uint64_t begin_counter, std::uint64_t end_counter) {
    ensure_perf_frequency();
    if (g_perf_frequency == 0 || end_counter <= begin_counter) {
        return 0.0;
    }
    return (static_cast<double>(end_counter - begin_counter) * 1000.0) /
           static_cast<double>(g_perf_frequency);
}
} // namespace

namespace render_diagnostics {

void begin_frame() {
    ++g_frame_stats.frame_index;
    g_frame_stats.frame_cpu_ms = 0.0;
    g_frame_stats.render_thread_cpu_ms = 0.0;
    g_frame_stats.render_pass_count = 0;
    g_frame_stats.copy_pass_count = 0;
    g_frame_stats.compute_pass_count = 0;
    g_frame_stats.draw_call_count = 0;
    g_frame_stats.texture_create_count = 0;
    g_frame_stats.texture_destroy_count = 0;
    g_frame_stats.gpu_buffer_create_count = 0;
    g_frame_stats.gpu_buffer_destroy_count = 0;
    g_frame_stats.render_target_switch_count = 0;
    g_frame_stats.cpu_light_gather_ms = 0.0;
    g_frame_stats.cpu_light_mask_generation_ms = 0.0;
    g_frame_stats.draw_submission_cpu_ms = 0.0;
    g_frame_stats.draw_submission_packet_build_sort_ms = 0.0;
    g_frame_stats.draw_submission_resource_create_ms = 0.0;
    g_frame_stats.draw_submission_pipeline_bind_ms = 0.0;
    g_frame_stats.draw_submission_submit_present_handoff_ms = 0.0;
    g_frame_stats.draw_submission_packet_build_count = 0;
    g_frame_stats.draw_submission_resource_create_count = 0;
    g_frame_stats.draw_submission_pipeline_bind_count = 0;
    g_frame_stats.draw_submission_submit_handoff_count = 0;
    g_frame_stats.ui_overlay_prepare_ms = 0.0;
    g_frame_stats.present_block_ms = g_has_present_sample ? g_last_present_block_ms : 0.0;
    g_frame_stats.present_interval_ms = g_has_present_sample ? g_last_present_interval_ms : 0.0;
    g_frame_stats.present_interval_known = g_has_present_sample && g_last_present_interval_known;
    g_frame_stats.texture_memory_bytes = 0;
    g_frame_stats.texture_memory_known = false;
    g_frame_stats.gpu_light_tile_assignments = 0;
    g_frame_stats.gpu_light_naive_evaluations = 0;
    g_frame_stats.gpu_light_tiled_evaluations = 0;
    g_frame_stats.gpu_pipeline_cache_hits = 0;
    g_frame_stats.gpu_pipeline_cache_misses = 0;
    g_frame_stats.gpu_pipeline_cache_hit_rate = 1.0;
    g_frame_stats.sdl_renderer_target_call_count = 0;
    g_frame_stats.sdl_renderer_draw_call_count = 0;
    g_frame_stats.present_call_count = 0;
    g_frame_stats.gpu_failed_frame_count = 0;
    g_frame_stats.gpu_scene_floor_draw_count = 0;
    g_frame_stats.gpu_scene_xy_sprite_draw_count = 0;
    g_frame_stats.gpu_scene_composite_source_ready = false;
    g_frame_stats.command_buffer_acquired = false;
    g_frame_stats.swapchain_acquired = false;
    g_frame_stats.swapchain_width = 0;
    g_frame_stats.swapchain_height = 0;
    g_frame_stats.floor_pass_target_width = 0;
    g_frame_stats.floor_pass_target_height = 0;
    g_frame_stats.xy_sprite_pass_target_width = 0;
    g_frame_stats.xy_sprite_pass_target_height = 0;
    g_frame_stats.clear_executed = false;
    g_frame_stats.floor_packet_count = 0;
    g_frame_stats.xy_sprite_packet_count = 0;
    g_frame_stats.active_depth_layer_count = 0;
    g_frame_stats.blur_pass_count = 0;
    g_frame_stats.skipped_texture_count = 0;
    g_frame_stats.failed_texture_names.clear();
    g_frame_stats.packets_per_depth_layer.clear();
    g_frame_stats.blur_strength_per_layer.clear();
    g_frame_stats.composite_layers_submitted.clear();
    g_frame_stats.render_stage_timings.clear();
    g_frame_stats.ui_overlay_active = false;
    g_frame_stats.ui_overlay_redrawn = false;
    g_frame_stats.submit_succeeded = false;
    g_frame_stats.projection_calls_total = 0;
    g_frame_stats.projection_calls_saved_early = 0;
    g_frame_stats.assets_stageA_reject = 0;
    g_frame_stats.assets_stageC_entered = 0;
    g_frame_stats.projection_recompute_budget = 0;
    g_frame_stats.projection_points_deferred = 0;
    g_frame_stats.projection_points_updated = 0;
    g_frame_stats.creation_budget_limit = 0;
    g_frame_stats.creation_budget_ms_limit = 0.0;
    g_frame_stats.creation_attempted_this_frame = 0;
    g_frame_stats.creation_executed_this_frame = 0;
    g_frame_stats.creation_deferred_count = 0;
    g_frame_stats.creation_queue_depth_start = 0;
    g_frame_stats.creation_queue_depth_end = 0;
    g_frame_stats.creation_queue_age_max = 0;
    g_frame_stats.creation_permanent_failures = 0;
    g_frame_stats.creation_retried_count = 0;
    g_frame_stats.held_scene_frame = false;
    g_frame_stats.held_scene_reason.clear();
    g_last_render_target = nullptr;
    g_frame_begin_counter = SDL_GetPerformanceCounter();
}

void end_frame() {
    const std::uint64_t frame_end = SDL_GetPerformanceCounter();
    g_frame_stats.frame_cpu_ms = elapsed_ms(g_frame_begin_counter, frame_end);
}

const RenderFrameStats& current_frame_stats() {
    return g_frame_stats;
}

void add_render_pass(std::uint32_t count) {
    g_frame_stats.render_pass_count += count;
}

void add_copy_pass(std::uint32_t count) {
    g_frame_stats.copy_pass_count += count;
}

void add_compute_pass(std::uint32_t count) {
    g_frame_stats.compute_pass_count += count;
}

void add_draw_call_count(std::uint32_t count) {
    g_frame_stats.draw_call_count += count;
}

void add_texture_create_count(std::uint32_t count) {
    g_frame_stats.texture_create_count += count;
}

void add_texture_destroy_count(std::uint32_t count) {
    g_frame_stats.texture_destroy_count += count;
}

void add_gpu_buffer_create_count(std::uint32_t count) {
    g_frame_stats.gpu_buffer_create_count += count;
}

void add_gpu_buffer_destroy_count(std::uint32_t count) {
    g_frame_stats.gpu_buffer_destroy_count += count;
}

void add_render_target_switch_count(std::uint32_t count) {
    g_frame_stats.render_target_switch_count += count;
}

void add_cpu_light_gather_ms(double elapsed_ms_value) {
    g_frame_stats.cpu_light_gather_ms += std::max(0.0, elapsed_ms_value);
}

void add_cpu_light_mask_generation_ms(double elapsed_ms_value) {
    g_frame_stats.cpu_light_mask_generation_ms += std::max(0.0, elapsed_ms_value);
}

void add_draw_submission_ms(double elapsed_ms_value) {
    g_frame_stats.draw_submission_cpu_ms += std::max(0.0, elapsed_ms_value);
}


void add_draw_submission_packet_build_sort_ms(double elapsed_ms_value, std::uint32_t packet_count) {
    g_frame_stats.draw_submission_packet_build_sort_ms += std::max(0.0, elapsed_ms_value);
    g_frame_stats.draw_submission_packet_build_count += packet_count;
}

void add_draw_submission_resource_create_ms(double elapsed_ms_value, std::uint32_t create_count) {
    g_frame_stats.draw_submission_resource_create_ms += std::max(0.0, elapsed_ms_value);
    g_frame_stats.draw_submission_resource_create_count += create_count;
}

void add_draw_submission_pipeline_bind_ms(double elapsed_ms_value, std::uint32_t bind_count) {
    g_frame_stats.draw_submission_pipeline_bind_ms += std::max(0.0, elapsed_ms_value);
    g_frame_stats.draw_submission_pipeline_bind_count += bind_count;
}

void add_draw_submission_submit_present_handoff_ms(double elapsed_ms_value, std::uint32_t handoff_count) {
    g_frame_stats.draw_submission_submit_present_handoff_ms += std::max(0.0, elapsed_ms_value);
    g_frame_stats.draw_submission_submit_handoff_count += handoff_count;
}

void set_ui_overlay_stats(bool active, bool redrawn, double prepare_ms) {
    g_frame_stats.ui_overlay_active = active;
    g_frame_stats.ui_overlay_redrawn = redrawn;
    g_frame_stats.ui_overlay_prepare_ms = std::max(0.0, prepare_ms);
}

void set_present_pacing(double present_block_ms_value,
                        double present_interval_ms_value,
                        bool interval_known) {
    g_frame_stats.present_block_ms = std::max(0.0, present_block_ms_value);
    g_frame_stats.present_interval_ms = std::max(0.0, present_interval_ms_value);
    g_frame_stats.present_interval_known = interval_known;
    g_last_present_block_ms = g_frame_stats.present_block_ms;
    g_last_present_interval_ms = g_frame_stats.present_interval_ms;
    g_last_present_interval_known = g_frame_stats.present_interval_known;
    g_has_present_sample = true;
}

void set_renderer_runtime_info(const std::string& renderer_path,
                               const std::string& backend_name,
                               const std::string& present_mode) {
    g_frame_stats.renderer_path = renderer_path;
    g_frame_stats.backend_name = backend_name;
    g_frame_stats.present_mode = present_mode;
}

void set_texture_memory_usage(std::uint64_t bytes, bool known) {
    g_frame_stats.texture_memory_bytes = bytes;
    g_frame_stats.texture_memory_known = known;
}

void set_render_thread_cpu_ms(double elapsed_ms_value) {
    g_frame_stats.render_thread_cpu_ms = std::max(0.0, elapsed_ms_value);
}

void set_gpu_light_culling_stats(std::uint32_t tile_assignments,
                                 std::uint32_t naive_evaluations,
                                 std::uint32_t tiled_evaluations) {
    g_frame_stats.gpu_light_tile_assignments = tile_assignments;
    g_frame_stats.gpu_light_naive_evaluations = naive_evaluations;
    g_frame_stats.gpu_light_tiled_evaluations = tiled_evaluations;
}

void set_gpu_pipeline_cache_stats(std::uint64_t hits,
                                  std::uint64_t misses,
                                  double hit_rate) {
    g_frame_stats.gpu_pipeline_cache_hits = hits;
    g_frame_stats.gpu_pipeline_cache_misses = misses;
    g_frame_stats.gpu_pipeline_cache_hit_rate = std::clamp(hit_rate, 0.0, 1.0);
}

void note_present_call(std::uint32_t count) {
    g_frame_stats.present_call_count += count;
}

void note_gpu_frame_skipped_due_to_failure(std::uint32_t count) {
    g_frame_stats.gpu_failed_frame_count += count;
}

void set_gpu_scene_packet_stats(std::uint32_t floor_draw_count,
                                std::uint32_t xy_sprite_draw_count,
                                bool composite_source_ready) {
    g_frame_stats.gpu_scene_floor_draw_count = floor_draw_count;
    g_frame_stats.gpu_scene_xy_sprite_draw_count = xy_sprite_draw_count;
    g_frame_stats.gpu_scene_composite_source_ready = composite_source_ready;
}
void set_command_buffer_acquired(bool acquired) { g_frame_stats.command_buffer_acquired = acquired; }
void set_swapchain_acquired(bool acquired) { g_frame_stats.swapchain_acquired = acquired; }
void set_swapchain_dimensions(std::uint32_t width, std::uint32_t height) {
    g_frame_stats.swapchain_width = width;
    g_frame_stats.swapchain_height = height;
}
void set_floor_pass_target_dimensions(std::uint32_t width, std::uint32_t height) {
    g_frame_stats.floor_pass_target_width = width;
    g_frame_stats.floor_pass_target_height = height;
}
void set_xy_sprite_pass_target_dimensions(std::uint32_t width, std::uint32_t height) {
    g_frame_stats.xy_sprite_pass_target_width = width;
    g_frame_stats.xy_sprite_pass_target_height = height;
}
void set_clear_executed(bool executed) { g_frame_stats.clear_executed = executed; }
void set_pass_packet_counts(std::uint32_t floor_packets, std::uint32_t xy_sprite_packets) {
    g_frame_stats.floor_packet_count = floor_packets;
    g_frame_stats.xy_sprite_packet_count = xy_sprite_packets;
}
void set_active_depth_layer_count(std::uint32_t count) { g_frame_stats.active_depth_layer_count = count; }
void set_blur_pass_count(std::uint32_t count) { g_frame_stats.blur_pass_count = count; }
void set_packets_per_depth_layer(const std::string& summary) { g_frame_stats.packets_per_depth_layer = summary; }
void set_blur_strength_per_layer(const std::string& summary) { g_frame_stats.blur_strength_per_layer = summary; }
void set_composite_layers_submitted(const std::string& summary) { g_frame_stats.composite_layers_submitted = summary; }
void set_render_stage_timings(const std::string& summary) { g_frame_stats.render_stage_timings = summary; }
void add_skipped_texture_count(std::uint32_t count) { g_frame_stats.skipped_texture_count += count; }
void set_failed_texture_names(const std::string& names) { g_frame_stats.failed_texture_names = names; }

void set_visibility_projection_stats(std::uint32_t projection_calls_total,
                                    std::uint32_t projection_calls_saved_early,
                                    std::uint32_t assets_stageA_reject,
                                    std::uint32_t assets_stageC_entered,
                                    std::uint32_t projection_recompute_budget,
                                    std::uint32_t projection_points_deferred,
                                    std::uint32_t projection_points_updated) {
    g_frame_stats.projection_calls_total = projection_calls_total;
    g_frame_stats.projection_calls_saved_early = projection_calls_saved_early;
    g_frame_stats.assets_stageA_reject = assets_stageA_reject;
    g_frame_stats.assets_stageC_entered = assets_stageC_entered;
    g_frame_stats.projection_recompute_budget = projection_recompute_budget;
    g_frame_stats.projection_points_deferred = projection_points_deferred;
    g_frame_stats.projection_points_updated = projection_points_updated;
}
void set_creation_budget_stats(std::uint32_t budget_limit,
                               double budget_ms_limit,
                               std::uint32_t attempted,
                               std::uint32_t executed,
                               std::uint32_t deferred,
                               std::uint32_t queue_depth_start,
                               std::uint32_t queue_depth_end,
                               std::uint32_t queue_age_max,
                               std::uint32_t retried_count,
                               std::uint32_t permanent_failures) {
    g_frame_stats.creation_budget_limit = budget_limit;
    g_frame_stats.creation_budget_ms_limit = std::max(0.0, budget_ms_limit);
    g_frame_stats.creation_attempted_this_frame = attempted;
    g_frame_stats.creation_executed_this_frame = executed;
    g_frame_stats.creation_deferred_count = deferred;
    g_frame_stats.creation_queue_depth_start = queue_depth_start;
    g_frame_stats.creation_queue_depth_end = queue_depth_end;
    g_frame_stats.creation_queue_age_max = queue_age_max;
    g_frame_stats.creation_retried_count = retried_count;
    g_frame_stats.creation_permanent_failures = permanent_failures;
}

void set_held_scene_frame(bool held, const std::string& reason) {
    g_frame_stats.held_scene_frame = held;
    g_frame_stats.held_scene_reason = held ? reason : std::string{};
}

void set_submit_result(bool succeeded) { g_frame_stats.submit_succeeded = succeeded; }

void note_texture_created(SDL_Texture* texture) {
    if (!texture) {
        return;
    }
    add_texture_create_count();
    track_texture_create(texture);
}

void note_texture_destroyed(SDL_Texture* texture) {
    if (!texture) {
        return;
    }
    add_texture_destroy_count();
    track_texture_destroy(texture);
}

void destroy_texture(SDL_Texture*& texture) {
    if (!texture) {
        return;
    }
    note_texture_destroyed(texture);
    SDL_DestroyTexture(texture);
    texture = nullptr;
}

std::uint64_t tracked_texture_bytes() {
    return g_tracked_texture_bytes;
}

SDL_Texture* create_texture(SDL_Renderer* renderer,
                            SDL_PixelFormat format,
                            SDL_TextureAccess access,
                            int w,
                            int h) {
    SDL_Texture* texture = SDL_CreateTexture(renderer, format, access, w, h);
    if (texture) {
        note_texture_created(texture);
    }
    return texture;
}

SDL_Texture* create_frame_graph_texture(SDL_Renderer* renderer,
                                        const std::string& resource_name,
                                        SDL_PixelFormat format,
                                        SDL_TextureAccess access,
                                        int w,
                                        int h) {
    SDL_Texture* texture = create_texture(renderer, format, access, w, h);
    if (!texture) {
        vibble::log::error("[RenderDiagnostics] Failed to create frame-graph-critical SDL texture for resource '" +
                           resource_name + "' size=" + std::to_string(w) + "x" + std::to_string(h) + ".");
        return nullptr;
    }

    void* gpu_ptr = nullptr;
    SDL_PropertiesID props = SDL_GetTextureProperties(texture);
    if (props) {
        gpu_ptr = SDL_GetPointerProperty(props, SDL_PROP_TEXTURE_GPU_TEXTURE_POINTER, nullptr);
    }
    if (!gpu_ptr) {
        vibble::log::warn("[RenderDiagnostics] Frame-graph texture '" + resource_name +
                          "' has no SDL GPU pointer bridge on this backend; continuing with graph-native resource binding.");
        return texture;
    }
    return texture;
}

bool set_render_target(SDL_Renderer* renderer, SDL_Texture* texture) {
    ++g_frame_stats.sdl_renderer_target_call_count;
    if (texture != g_last_render_target) {
        ++g_frame_stats.render_target_switch_count;
        g_last_render_target = texture;
    }
    const bool ok = SDL_SetRenderTarget(renderer, texture);
    if (ok) {
        add_render_pass();
    }
    return ok;
}

bool render_texture(SDL_Renderer* renderer,
                    SDL_Texture* texture,
                    const SDL_FRect* srcrect,
                    const SDL_FRect* dstrect) {
    const bool ok = SDL_RenderTexture(renderer, texture, srcrect, dstrect);
    if (ok) {
        ++g_frame_stats.sdl_renderer_draw_call_count;
        add_draw_call_count();
    }
    return ok;
}

bool render_geometry(SDL_Renderer* renderer,
                     SDL_Texture* texture,
                     const SDL_Vertex* vertices,
                     int num_vertices,
                     const int* indices,
                     int num_indices) {
    const bool ok = SDL_RenderGeometry(renderer, texture, vertices, num_vertices, indices, num_indices);
    if (ok) {
        ++g_frame_stats.sdl_renderer_draw_call_count;
        add_draw_call_count();
    }
    return ok;
}

ScopedCpuTimer::ScopedCpuTimer(CpuTimerMetric metric)
    : metric_(metric),
      begin_counter_(SDL_GetPerformanceCounter()) {}

ScopedCpuTimer::~ScopedCpuTimer() {
    const double metric_ms = elapsed_ms(begin_counter_, SDL_GetPerformanceCounter());
    if (metric_ == CpuTimerMetric::LightGather) {
        add_cpu_light_gather_ms(metric_ms);
    } else if (metric_ == CpuTimerMetric::LightMaskGeneration) {
        add_cpu_light_mask_generation_ms(metric_ms);
    }
}

} // namespace render_diagnostics
