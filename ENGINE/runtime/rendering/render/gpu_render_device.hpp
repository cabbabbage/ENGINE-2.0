#pragma once

#include <SDL3/SDL.h>
#include <SDL3/SDL_gpu.h>

#include <memory>
#include <cstdint>
#include <string>

#include "rendering/render/gpu_format_policy.hpp"

class GpuRenderDevice {
public:
    struct FrameState {
        SDL_GPUCommandBuffer* command_buffer = nullptr;
        SDL_GPUTexture* swapchain_texture = nullptr;
        Uint32 swapchain_width = 0;
        Uint32 swapchain_height = 0;
    };

    static std::unique_ptr<GpuRenderDevice> Create(SDL_Renderer* renderer,
                                                   bool prefer_depth32,
                                                   std::string& out_error);

    SDL_GPUDevice* gpu_device() const { return gpu_device_; }
    SDL_Renderer* renderer() const { return renderer_; }
    const RuntimeGpuFormatPolicy& format_policy() const { return format_policy_; }
    const std::string& backend_name() const { return backend_name_; }
    const std::string& present_mode() const { return present_mode_; }
    const FrameState& frame_state() const { return frame_state_; }

    SDL_GPUCommandBuffer* begin_command_buffer() const;
    bool submit(SDL_GPUCommandBuffer* command_buffer) const;
    bool begin_frame(std::string& out_error);
    bool end_frame(bool submit_commands, std::string& out_error);
    bool query_texture_memory_usage(std::uint64_t& out_bytes) const;

private:
    explicit GpuRenderDevice(SDL_Renderer* renderer);

    bool initialize(bool prefer_depth32, std::string& out_error);

    SDL_Renderer* renderer_ = nullptr;
    SDL_Window* window_ = nullptr;
    SDL_GPUDevice* gpu_device_ = nullptr;
    RuntimeGpuFormatPolicy format_policy_{};
    std::string backend_name_ = "unknown";
    std::string present_mode_ = "vsync";
    FrameState frame_state_{};
};
