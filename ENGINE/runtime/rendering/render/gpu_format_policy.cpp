#include "rendering/render/gpu_format_policy.hpp"

#include <array>

namespace {
bool supports_format(SDL_GPUDevice* device,
                     SDL_GPUTextureFormat format,
                     SDL_GPUTextureUsageFlags usage = SDL_GPU_TEXTUREUSAGE_SAMPLER) {
    return SDL_GPUTextureSupportsFormat(device,
                                        format,
                                        SDL_GPU_TEXTURETYPE_2D,
                                        usage);
}

SDL_GPUTextureFormat choose_supported(SDL_GPUDevice* device,
                                      const std::array<SDL_GPUTextureFormat, 2>& candidates,
                                      SDL_GPUTextureUsageFlags usage) {
    for (SDL_GPUTextureFormat candidate : candidates) {
        if (supports_format(device, candidate, usage)) {
            return candidate;
        }
    }
    return SDL_GPU_TEXTUREFORMAT_INVALID;
}
} // namespace

bool GpuFormatPolicyResolver::Resolve(SDL_GPUDevice* device,
                                      bool prefer_depth32,
                                      RuntimeGpuFormatPolicy& out_policy,
                                      std::string& out_error) {
    if (!device) {
        out_error = "SDL_GPUDevice is null";
        return false;
    }

    const SDL_GPUTextureFormat albedo =
        choose_supported(device,
                         {SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM_SRGB,
                          SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM},
                         static_cast<SDL_GPUTextureUsageFlags>(SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET));
    if (albedo == SDL_GPU_TEXTUREFORMAT_INVALID) {
        out_error = "No supported albedo format (R8G8B8A8_UNORM_SRGB / R8G8B8A8_UNORM)";
        return false;
    }

    const SDL_GPUTextureFormat light =
        choose_supported(device,
                         {SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT,
                          SDL_GPU_TEXTUREFORMAT_R11G11B10_UFLOAT},
                         static_cast<SDL_GPUTextureUsageFlags>(SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET));
    if (light == SDL_GPU_TEXTUREFORMAT_INVALID) {
        out_error = "No supported light accumulation format (R16G16B16A16_FLOAT / R11G11B10_UFLOAT)";
        return false;
    }

    if (!supports_format(device,
                         SDL_GPU_TEXTUREFORMAT_R8_UNORM,
                         static_cast<SDL_GPUTextureUsageFlags>(SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET))) {
        out_error = "R8_UNORM mask format is unsupported";
        return false;
    }

    SDL_GPUTextureFormat depth_format = SDL_GPU_TEXTUREFORMAT_D24_UNORM_S8_UINT;
    if (prefer_depth32 || !supports_format(device,
                                           depth_format,
                                           SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET)) {
        if (supports_format(device,
                            SDL_GPU_TEXTUREFORMAT_D32_FLOAT,
                            SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET)) {
            depth_format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
        } else if (!supports_format(device,
                                    depth_format,
                                    SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET)) {
            out_error = "No supported depth format (D24_UNORM_S8_UINT / D32_FLOAT)";
            return false;
        }
    }

    SDL_GPUSampleCount sample_count = SDL_GPU_SAMPLECOUNT_1;
    if (SDL_GPUTextureSupportsSampleCount(device, albedo, SDL_GPU_SAMPLECOUNT_4)) {
        sample_count = SDL_GPU_SAMPLECOUNT_4;
    } else if (SDL_GPUTextureSupportsSampleCount(device, albedo, SDL_GPU_SAMPLECOUNT_2)) {
        sample_count = SDL_GPU_SAMPLECOUNT_2;
    }

    out_policy.albedo_format = albedo;
    out_policy.light_accumulation_format = light;
    out_policy.mask_format = SDL_GPU_TEXTUREFORMAT_R8_UNORM;
    out_policy.depth_format = depth_format;
    out_policy.depth_uses_d32 = depth_format == SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
    out_policy.sample_count = sample_count;
    out_error.clear();
    return true;
}
