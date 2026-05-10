#include "rendering/render/gpu_format_policy.hpp"

#include <array>
#include <string>

#include "utils/log.hpp"

namespace {
void assign_default_policy(RuntimeGpuFormatPolicy& out_policy, bool prefer_depth32) {
    out_policy.albedo_format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM_SRGB;
    out_policy.light_accumulation_format = SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT;
    out_policy.mask_format = SDL_GPU_TEXTUREFORMAT_R8_UNORM;
    out_policy.depth_format = prefer_depth32
        ? SDL_GPU_TEXTUREFORMAT_D32_FLOAT
        : SDL_GPU_TEXTUREFORMAT_D24_UNORM_S8_UINT;
    out_policy.depth_uses_d32 = out_policy.depth_format == SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
    out_policy.sample_count = SDL_GPU_SAMPLECOUNT_1;
}

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

const char* format_name(SDL_GPUTextureFormat format) {
    switch (format) {
    case SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM_SRGB: return "R8G8B8A8_UNORM_SRGB";
    case SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM: return "R8G8B8A8_UNORM";
    case SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT: return "R16G16B16A16_FLOAT";
    case SDL_GPU_TEXTUREFORMAT_R11G11B10_UFLOAT: return "R11G11B10_UFLOAT";
    case SDL_GPU_TEXTUREFORMAT_R8_UNORM: return "R8_UNORM";
    case SDL_GPU_TEXTUREFORMAT_D24_UNORM_S8_UINT: return "D24_UNORM_S8_UINT";
    case SDL_GPU_TEXTUREFORMAT_D32_FLOAT: return "D32_FLOAT";
    default: return "UNKNOWN";
    }
}

const char* sample_count_name(SDL_GPUSampleCount sample_count) {
    switch (sample_count) {
    case SDL_GPU_SAMPLECOUNT_1: return "1x";
    case SDL_GPU_SAMPLECOUNT_2: return "2x";
    case SDL_GPU_SAMPLECOUNT_4: return "4x";
    case SDL_GPU_SAMPLECOUNT_8: return "8x";
    default: return "unknown";
    }
}
} // namespace

bool GpuFormatPolicyResolver::Resolve(SDL_GPUDevice* device,
                                      bool prefer_depth32,
                                      RuntimeGpuFormatPolicy& out_policy,
                                      std::string& out_error) {
    if (!device) {
        assign_default_policy(out_policy, prefer_depth32);
        vibble::log::info("[GpuFormatPolicy] Using default renderer-compatible format policy "
                          "(no SDL_GPUDevice available).");
        out_error.clear();
        return true;
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
    if (albedo != SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM_SRGB) {
        vibble::log::warn("[GpuFormatPolicy] Falling back albedo format to " + std::string(format_name(albedo)));
    } else {
        vibble::log::info("[GpuFormatPolicy] Using albedo format " + std::string(format_name(albedo)));
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
    if (light != SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT) {
        vibble::log::warn("[GpuFormatPolicy] Falling back light accumulation format to " +
                          std::string(format_name(light)));
    } else {
        vibble::log::info("[GpuFormatPolicy] Using light accumulation format " + std::string(format_name(light)));
    }

    if (!supports_format(device,
                         SDL_GPU_TEXTUREFORMAT_R8_UNORM,
                         static_cast<SDL_GPUTextureUsageFlags>(SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET))) {
        out_error = "R8_UNORM mask format is unsupported";
        return false;
    }
    vibble::log::info("[GpuFormatPolicy] Using mask format R8_UNORM");

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
    vibble::log::info("[GpuFormatPolicy] Using depth format " + std::string(format_name(depth_format)));

    SDL_GPUSampleCount sample_count = SDL_GPU_SAMPLECOUNT_1;
    const bool supports_4x = SDL_GPUTextureSupportsSampleCount(device, albedo, SDL_GPU_SAMPLECOUNT_4);
    const bool supports_2x = SDL_GPUTextureSupportsSampleCount(device, albedo, SDL_GPU_SAMPLECOUNT_2);
    if (supports_4x) {
        sample_count = SDL_GPU_SAMPLECOUNT_4;
    } else if (supports_2x) {
        sample_count = SDL_GPU_SAMPLECOUNT_2;
    }
    vibble::log::info("[GpuFormatPolicy] Sample-count probe for " + std::string(format_name(albedo)) +
                      ": supports_4x=" + (supports_4x ? std::string("1") : std::string("0")) +
                      " supports_2x=" + (supports_2x ? std::string("1") : std::string("0")) +
                      " selected=" + sample_count_name(sample_count));

    out_policy.albedo_format = albedo;
    out_policy.light_accumulation_format = light;
    out_policy.mask_format = SDL_GPU_TEXTUREFORMAT_R8_UNORM;
    out_policy.depth_format = depth_format;
    out_policy.depth_uses_d32 = depth_format == SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
    out_policy.sample_count = sample_count;
    out_error.clear();
    return true;
}
