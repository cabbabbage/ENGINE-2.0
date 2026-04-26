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
    std::uint32_t render_target_switch_count = 0;
    double cpu_light_gather_ms = 0.0;
    double cpu_light_mask_generation_ms = 0.0;
    double draw_submission_cpu_ms = 0.0;
    std::string renderer_path;
    std::string backend_name;
    std::string present_mode;
    std::uint64_t texture_memory_bytes = 0;
    bool texture_memory_known = false;
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
void add_render_target_switch_count(std::uint32_t count = 1);
void add_cpu_light_gather_ms(double elapsed_ms);
void add_cpu_light_mask_generation_ms(double elapsed_ms);
void add_draw_submission_ms(double elapsed_ms);
void set_renderer_runtime_info(const std::string& renderer_path,
                               const std::string& backend_name,
                               const std::string& present_mode);
void set_texture_memory_usage(std::uint64_t bytes, bool known);
void set_render_thread_cpu_ms(double elapsed_ms);

SDL_Texture* create_texture(SDL_Renderer* renderer,
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
