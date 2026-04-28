#include "rendering/render/shader_pipeline_cache.hpp"

#include <functional>

std::size_t ShaderPipelineKeyHash::operator()(const ShaderPipelineKey& key) const {
    const std::size_t shader_hash = std::hash<std::string>{}(key.shader_id);
    const std::size_t variant_hash = std::hash<std::string>{}(key.variant);
    const std::size_t color_hash = std::hash<int>{}(static_cast<int>(key.color_format));
    const std::size_t depth_hash = std::hash<int>{}(static_cast<int>(key.depth_format));
    const std::size_t sample_hash = std::hash<int>{}(static_cast<int>(key.sample_count));
    const std::size_t state_hash = std::hash<std::uint32_t>{}(key.render_state_key);
    return shader_hash ^
           (variant_hash << 1) ^
           (color_hash << 2) ^
           (depth_hash << 3) ^
           (sample_hash << 4) ^
           (state_hash << 5);
}

void ShaderPipelineCache::register_hit(const ShaderPipelineKey& key) {
    ++hits_[key];
}

void ShaderPipelineCache::register_miss(const ShaderPipelineKey& key) {
    ++misses_[key];
}

SDL_GPUGraphicsPipeline* ShaderPipelineCache::get_or_create_graphics_pipeline(
    const ShaderPipelineKey& key,
    const GraphicsFactory& factory) {
    const auto existing = graphics_pipelines_.find(key);
    if (existing != graphics_pipelines_.end() && existing->second) {
        register_hit(key);
        return existing->second;
    }

    register_miss(key);
    if (!factory) {
        return nullptr;
    }
    SDL_GPUGraphicsPipeline* pipeline = factory();
    if (!pipeline) {
        return nullptr;
    }
    graphics_pipelines_[key] = pipeline;
    return pipeline;
}

SDL_GPUComputePipeline* ShaderPipelineCache::get_or_create_compute_pipeline(
    const ShaderPipelineKey& key,
    const ComputeFactory& factory) {
    const auto existing = compute_pipelines_.find(key);
    if (existing != compute_pipelines_.end() && existing->second) {
        register_hit(key);
        return existing->second;
    }

    register_miss(key);
    if (!factory) {
        return nullptr;
    }
    SDL_GPUComputePipeline* pipeline = factory();
    if (!pipeline) {
        return nullptr;
    }
    compute_pipelines_[key] = pipeline;
    return pipeline;
}

void ShaderPipelineCache::clear(SDL_GPUDevice* device) {
    if (device) {
        for (const auto& entry : graphics_pipelines_) {
            if (entry.second) {
                SDL_ReleaseGPUGraphicsPipeline(device, entry.second);
            }
        }
        for (const auto& entry : compute_pipelines_) {
            if (entry.second) {
                SDL_ReleaseGPUComputePipeline(device, entry.second);
            }
        }
    }
    graphics_pipelines_.clear();
    compute_pipelines_.clear();
    hits_.clear();
    misses_.clear();
}

double ShaderPipelineCache::hit_rate() const {
    const std::uint64_t hit_count = total_hits();
    const std::uint64_t miss_count = total_misses();
    const std::uint64_t total = hit_count + miss_count;
    if (total == 0) {
        return 1.0;
    }
    return static_cast<double>(hit_count) / static_cast<double>(total);
}

std::uint64_t ShaderPipelineCache::total_hits() const {
    std::uint64_t hit_count = 0;
    for (const auto& entry : hits_) {
        hit_count += entry.second;
    }
    return hit_count;
}

std::uint64_t ShaderPipelineCache::total_misses() const {
    std::uint64_t miss_count = 0;
    for (const auto& entry : misses_) {
        miss_count += entry.second;
    }
    return miss_count;
}
