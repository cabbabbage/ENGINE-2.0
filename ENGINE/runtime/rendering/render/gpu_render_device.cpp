#include "rendering/render/gpu_render_device.hpp"

#include "utils/log.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>

namespace {
std::string safe_string(const char* value) {
    return value ? std::string(value) : std::string();
}

std::string to_lower_copy(const std::string& value) {
    std::string lowered;
    lowered.reserve(value.size());
    for (const unsigned char ch : value) {
        lowered.push_back(static_cast<char>(std::tolower(ch)));
    }
    return lowered;
}

const char* present_mode_name(SDL_GPUPresentMode mode) {
    switch (mode) {
    case SDL_GPU_PRESENTMODE_IMMEDIATE:
        return "immediate";
    case SDL_GPU_PRESENTMODE_MAILBOX:
        return "mailbox";
    case SDL_GPU_PRESENTMODE_VSYNC:
    default:
        return "vsync";
    }
}

SDL_GPUPresentMode select_present_mode(bool supports_mailbox,
                                       bool supports_immediate) {
    const char* requested = std::getenv("VIBBLE_GPU_PRESENT_MODE");
    std::string mode = requested ? to_lower_copy(requested) : std::string();
    if (mode.empty() || mode == "auto") {
        return SDL_GPU_PRESENTMODE_VSYNC;
    }
    if (mode == "mailbox") {
        return supports_mailbox ? SDL_GPU_PRESENTMODE_MAILBOX : SDL_GPU_PRESENTMODE_VSYNC;
    }
    if (mode == "immediate") {
        if (supports_immediate) {
            return SDL_GPU_PRESENTMODE_IMMEDIATE;
        }
        return SDL_GPU_PRESENTMODE_VSYNC;
    }
    return SDL_GPU_PRESENTMODE_VSYNC;
}
} // namespace

GpuRenderDevice::GpuRenderDevice(SDL_Renderer* renderer)
    : renderer_(renderer) {}

std::unique_ptr<GpuRenderDevice> GpuRenderDevice::Create(SDL_Renderer* renderer,
                                                         bool prefer_depth32,
                                                         std::string& out_error) {
    auto device = std::unique_ptr<GpuRenderDevice>(new GpuRenderDevice(renderer));
    if (!device->initialize(prefer_depth32, out_error)) {
        return nullptr;
    }
    return device;
}

bool GpuRenderDevice::initialize(bool prefer_depth32, std::string& out_error) {
    if (!renderer_) {
        out_error = "Renderer is null";
        return false;
    }

    SDL_PropertiesID props = SDL_GetRendererProperties(renderer_);
    if (!props) {
        out_error = "Renderer properties unavailable";
        return false;
    }

    gpu_device_ = static_cast<SDL_GPUDevice*>(
        SDL_GetPointerProperty(props, SDL_PROP_RENDERER_GPU_DEVICE_POINTER, nullptr));
    if (!gpu_device_) {
        out_error = "Renderer has no SDL_GPUDevice";
        return false;
    }

    window_ = static_cast<SDL_Window*>(
        SDL_GetPointerProperty(props, SDL_PROP_RENDERER_WINDOW_POINTER, nullptr));

    SDL_PropertiesID gpu_props = SDL_GetGPUDeviceProperties(gpu_device_);
    const std::string gpu_driver_name = safe_string(SDL_GetGPUDeviceDriver(gpu_device_));
    if (gpu_props) {
        const std::string backend_prop = safe_string(
            SDL_GetStringProperty(gpu_props, SDL_PROP_GPU_DEVICE_DRIVER_NAME_STRING, nullptr));
        const std::string device_name = safe_string(
            SDL_GetStringProperty(gpu_props, SDL_PROP_GPU_DEVICE_NAME_STRING, nullptr));
        backend_name_ = !backend_prop.empty() ? backend_prop : (!gpu_driver_name.empty() ? gpu_driver_name : "unknown");
        if (backend_name_.empty()) {
            backend_name_ = "unknown";
        }
        if (!device_name.empty()) {
            vibble::log::info("[GpuRenderDevice] Using GPU adapter: " + device_name);
        }
    } else if (!gpu_driver_name.empty()) {
        backend_name_ = gpu_driver_name;
    }

    if (window_) {
        const bool supports_vsync =
            SDL_WindowSupportsGPUPresentMode(gpu_device_, window_, SDL_GPU_PRESENTMODE_VSYNC);
        const bool supports_mailbox =
            SDL_WindowSupportsGPUPresentMode(gpu_device_, window_, SDL_GPU_PRESENTMODE_MAILBOX);
        const bool supports_immediate =
            SDL_WindowSupportsGPUPresentMode(gpu_device_, window_, SDL_GPU_PRESENTMODE_IMMEDIATE);

        SDL_GPUPresentMode selected_mode =
            select_present_mode(supports_mailbox, supports_immediate);
        if (!SDL_SetGPUSwapchainParameters(gpu_device_,
                                           window_,
                                           SDL_GPU_SWAPCHAINCOMPOSITION_SDR,
                                           selected_mode)) {
            vibble::log::warn("[GpuRenderDevice] Failed to set requested present mode '" +
                              std::string(present_mode_name(selected_mode)) +
                              "': " + safe_string(SDL_GetError()) +
                              ". Falling back to vsync.");
            selected_mode = SDL_GPU_PRESENTMODE_VSYNC;
            if (!SDL_SetGPUSwapchainParameters(gpu_device_,
                                               window_,
                                               SDL_GPU_SWAPCHAINCOMPOSITION_SDR,
                                               selected_mode)) {
                vibble::log::warn("[GpuRenderDevice] Failed to apply VSYNC fallback present mode: " +
                                  safe_string(SDL_GetError()));
            }
        }
        present_mode_ = present_mode_name(selected_mode);
        swapchain_format_ = SDL_GetGPUSwapchainTextureFormat(gpu_device_, window_);
        if (swapchain_format_ == SDL_GPU_TEXTUREFORMAT_INVALID) {
            vibble::log::warn("[GpuRenderDevice] Failed to query swapchain texture format.");
        }

        vibble::log::info("[GpuRenderDevice] Present mode probe: mailbox=" +
                          std::string(supports_mailbox ? "1" : "0") +
                          " vsync=" + std::string(supports_vsync ? "1" : "0") +
                          " immediate=" + std::string(supports_immediate ? "1" : "0") +
                          " selected=" + present_mode_);
    } else {
        present_mode_ = "vsync";
        swapchain_format_ = SDL_GPU_TEXTUREFORMAT_INVALID;
        vibble::log::warn("[GpuRenderDevice] Renderer window handle unavailable; present mode probe skipped.");
    }

    if (!GpuFormatPolicyResolver::Resolve(gpu_device_, prefer_depth32, format_policy_, out_error)) {
        return false;
    }

    vibble::log::info("[GpuRenderDevice] SDL_GPU ready. backend=" + backend_name_ +
                      " present=" + present_mode_ +
                      " swapchain=" + std::to_string(static_cast<int>(swapchain_format_)) +
                      " albedo=" + std::to_string(static_cast<int>(format_policy_.albedo_format)) +
                      " light=" + std::to_string(static_cast<int>(format_policy_.light_accumulation_format)) +
                      " mask=" + std::to_string(static_cast<int>(format_policy_.mask_format)) +
                      " depth=" + std::to_string(static_cast<int>(format_policy_.depth_format)) +
                      " samples=" + std::to_string(static_cast<int>(format_policy_.sample_count)));

    out_error.clear();
    return true;
}

SDL_GPUCommandBuffer* GpuRenderDevice::begin_command_buffer() const {
    if (!gpu_device_) {
        return nullptr;
    }
    return SDL_AcquireGPUCommandBuffer(gpu_device_);
}

bool GpuRenderDevice::submit(SDL_GPUCommandBuffer* command_buffer) const {
    if (!gpu_device_ || !command_buffer) {
        return false;
    }
    return SDL_SubmitGPUCommandBuffer(command_buffer);
}

bool GpuRenderDevice::begin_frame(std::string& out_error) {
    if (!gpu_device_) {
        out_error = "GPU device is unavailable";
        return false;
    }
    if (frame_state_.command_buffer) {
        out_error = "Frame is already active";
        return false;
    }

    static constexpr int kAcquireCommandBufferRetries = 3;
    for (int attempt = 0; attempt < kAcquireCommandBufferRetries; ++attempt) {
        frame_state_.command_buffer = SDL_AcquireGPUCommandBuffer(gpu_device_);
        if (frame_state_.command_buffer) {
            break;
        }
        if (attempt + 1 < kAcquireCommandBufferRetries) {
            SDL_Delay(1);
        }
    }
    if (!frame_state_.command_buffer) {
        out_error = "Failed to acquire GPU command buffer: " + safe_string(SDL_GetError());
        return false;
    }

    if (window_) {
        Uint32 swapchain_width = 0;
        Uint32 swapchain_height = 0;
        SDL_GPUTexture* swapchain_texture = nullptr;
        if (!SDL_WaitAndAcquireGPUSwapchainTexture(frame_state_.command_buffer,
                                                   window_,
                                                   &swapchain_texture,
                                                   &swapchain_width,
                                                   &swapchain_height)) {
            SDL_CancelGPUCommandBuffer(frame_state_.command_buffer);
            frame_state_ = FrameState{};
            out_error = "Failed to acquire GPU swapchain texture: " + safe_string(SDL_GetError());
            return false;
        }
        frame_state_.swapchain_texture = swapchain_texture;
        frame_state_.swapchain_width = swapchain_width;
        frame_state_.swapchain_height = swapchain_height;
    }

    out_error.clear();
    return true;
}

bool GpuRenderDevice::end_frame(bool submit_commands, std::string& out_error) {
    if (!frame_state_.command_buffer) {
        out_error = "No active frame command buffer";
        return false;
    }

    bool ok = false;
    if (submit_commands) {
        ok = SDL_SubmitGPUCommandBuffer(frame_state_.command_buffer);
        if (!ok) {
            out_error = "Failed to submit GPU command buffer: " + safe_string(SDL_GetError());
        }
    } else {
        ok = SDL_CancelGPUCommandBuffer(frame_state_.command_buffer);
        if (!ok) {
            out_error = "Failed to cancel GPU command buffer: " + safe_string(SDL_GetError());
        }
    }

    frame_state_ = FrameState{};
    if (ok) {
        out_error.clear();
    }
    return ok;
}

bool GpuRenderDevice::query_texture_memory_usage(std::uint64_t& out_bytes) const {
    out_bytes = 0;
    if (!gpu_device_) {
        return false;
    }

    SDL_PropertiesID props = SDL_GetGPUDeviceProperties(gpu_device_);
    if (!props) {
        return false;
    }

    // SDL does not currently expose a stable cross-backend texture-memory usage
    // property. Keep probing for vendor-specific extensions by name.
    static constexpr const char* kCandidateProps[] = {
        "SDL.gpu.device.memory.textures.used",
        "SDL.gpu.device.memory.texture_bytes",
        "SDL.gpu.device.memory.used_texture_bytes",
    };

    for (const char* prop_name : kCandidateProps) {
        const Sint64 value = SDL_GetNumberProperty(props, prop_name, -1);
        if (value >= 0) {
            out_bytes = static_cast<std::uint64_t>(value);
            return true;
        }
    }
    return false;
}
