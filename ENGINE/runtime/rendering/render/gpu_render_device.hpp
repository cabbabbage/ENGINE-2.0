#pragma once

#include <SDL3/SDL.h>
#include <SDL3/SDL_gpu.h>

#include <memory>
#include <string>

#include "rendering/render/gpu_format_policy.hpp"

class GpuRenderDevice {
public:
    static std::unique_ptr<GpuRenderDevice> Create(SDL_Renderer* renderer,
                                                   bool prefer_depth32,
                                                   std::string& out_error);

    SDL_GPUDevice* gpu_device() const { return gpu_device_; }
    const RuntimeGpuFormatPolicy& format_policy() const { return format_policy_; }
    const std::string& backend_name() const { return backend_name_; }
    const std::string& present_mode() const { return present_mode_; }

    SDL_GPUCommandBuffer* begin_command_buffer() const;
    bool submit(SDL_GPUCommandBuffer* command_buffer) const;

private:
    explicit GpuRenderDevice(SDL_Renderer* renderer);

    bool initialize(bool prefer_depth32, std::string& out_error);

    SDL_Renderer* renderer_ = nullptr;
    SDL_GPUDevice* gpu_device_ = nullptr;
    RuntimeGpuFormatPolicy format_policy_{};
    std::string backend_name_ = "unknown";
    std::string present_mode_ = "vsync";
};
