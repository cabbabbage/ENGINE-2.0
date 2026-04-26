#pragma once

#include <SDL3/SDL_gpu.h>

#include <string>

struct RuntimeGpuFormatPolicy {
    SDL_GPUTextureFormat albedo_format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    SDL_GPUTextureFormat light_accumulation_format = SDL_GPU_TEXTUREFORMAT_R11G11B10_UFLOAT;
    SDL_GPUTextureFormat mask_format = SDL_GPU_TEXTUREFORMAT_R8_UNORM;
    SDL_GPUTextureFormat depth_format = SDL_GPU_TEXTUREFORMAT_D24_UNORM_S8_UINT;
    bool depth_uses_d32 = false;
    SDL_GPUSampleCount sample_count = SDL_GPU_SAMPLECOUNT_1;
};

class GpuFormatPolicyResolver {
public:
    static bool Resolve(SDL_GPUDevice* device,
                        bool prefer_depth32,
                        RuntimeGpuFormatPolicy& out_policy,
                        std::string& out_error);
};
