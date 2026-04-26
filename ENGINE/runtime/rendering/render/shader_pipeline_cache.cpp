#include "rendering/render/shader_pipeline_cache.hpp"

#include <functional>

std::size_t ShaderPipelineKeyHash::operator()(const ShaderPipelineKey& key) const {
    const std::size_t shader_hash = std::hash<std::string>{}(key.shader_id);
    const std::size_t variant_hash = std::hash<std::string>{}(key.variant);
    const std::size_t color_hash = std::hash<int>{}(static_cast<int>(key.color_format));
    const std::size_t depth_hash = std::hash<int>{}(static_cast<int>(key.depth_format));
    return shader_hash ^ (variant_hash << 1) ^ (color_hash << 2) ^ (depth_hash << 3);
}

void ShaderPipelineCache::register_hit(const ShaderPipelineKey& key) {
    ++hits_[key];
}

void ShaderPipelineCache::register_miss(const ShaderPipelineKey& key) {
    ++misses_[key];
}

double ShaderPipelineCache::hit_rate() const {
    std::uint64_t hit_count = 0;
    std::uint64_t miss_count = 0;
    for (const auto& entry : hits_) {
        hit_count += entry.second;
    }
    for (const auto& entry : misses_) {
        miss_count += entry.second;
    }
    const std::uint64_t total = hit_count + miss_count;
    if (total == 0) {
        return 1.0;
    }
    return static_cast<double>(hit_count) / static_cast<double>(total);
}
