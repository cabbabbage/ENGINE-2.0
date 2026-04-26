#include "rendering/render/gpu_render_device.hpp"

#include "utils/log.hpp"

namespace {
std::string safe_string(const char* value) {
    return value ? std::string(value) : std::string();
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

    SDL_PropertiesID gpu_props = SDL_GetGPUDeviceProperties(gpu_device_);
    if (gpu_props) {
        backend_name_ = safe_string(SDL_GetStringProperty(gpu_props, SDL_PROP_GPU_DEVICE_NAME_STRING, nullptr));
    }

    if (!GpuFormatPolicyResolver::Resolve(gpu_device_, prefer_depth32, format_policy_, out_error)) {
        return false;
    }

    vibble::log::info("[GpuRenderDevice] SDL_GPU ready. backend=" + backend_name_ +
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
