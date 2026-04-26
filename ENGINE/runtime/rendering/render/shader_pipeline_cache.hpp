#pragma once

#include <SDL3/SDL_gpu.h>

#include <functional>
#include <cstdint>
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
    using GraphicsFactory = std::function<SDL_GPUGraphicsPipeline*()>;
    using ComputeFactory = std::function<SDL_GPUComputePipeline*()>;

    void register_hit(const ShaderPipelineKey& key);
    void register_miss(const ShaderPipelineKey& key);
    SDL_GPUGraphicsPipeline* get_or_create_graphics_pipeline(const ShaderPipelineKey& key,
                                                             const GraphicsFactory& factory);
    SDL_GPUComputePipeline* get_or_create_compute_pipeline(const ShaderPipelineKey& key,
                                                           const ComputeFactory& factory);
    void clear(SDL_GPUDevice* device);
    double hit_rate() const;
    std::size_t graphics_pipeline_count() const { return graphics_pipelines_.size(); }
    std::size_t compute_pipeline_count() const { return compute_pipelines_.size(); }

private:
    std::unordered_map<ShaderPipelineKey, std::uint32_t, ShaderPipelineKeyHash> hits_;
    std::unordered_map<ShaderPipelineKey, std::uint32_t, ShaderPipelineKeyHash> misses_;
    std::unordered_map<ShaderPipelineKey, SDL_GPUGraphicsPipeline*, ShaderPipelineKeyHash> graphics_pipelines_;
    std::unordered_map<ShaderPipelineKey, SDL_GPUComputePipeline*, ShaderPipelineKeyHash> compute_pipelines_;
};
