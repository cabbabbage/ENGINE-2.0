#pragma once

#include <SDL3/SDL_gpu.h>

#include <string>
#include <unordered_map>

struct ShaderPipelineKey {
    std::string shader_id;
    std::string variant;
    SDL_GPUTextureFormat color_format = SDL_GPU_TEXTUREFORMAT_INVALID;
    SDL_GPUTextureFormat depth_format = SDL_GPU_TEXTUREFORMAT_INVALID;

    bool operator==(const ShaderPipelineKey& other) const {
        return shader_id == other.shader_id &&
               variant == other.variant &&
               color_format == other.color_format &&
               depth_format == other.depth_format;
    }
};

struct ShaderPipelineKeyHash {
    std::size_t operator()(const ShaderPipelineKey& key) const;
};

class ShaderPipelineCache {
public:
    void register_hit(const ShaderPipelineKey& key);
    void register_miss(const ShaderPipelineKey& key);
    double hit_rate() const;

private:
    std::unordered_map<ShaderPipelineKey, std::uint32_t, ShaderPipelineKeyHash> hits_;
    std::unordered_map<ShaderPipelineKey, std::uint32_t, ShaderPipelineKeyHash> misses_;
};
