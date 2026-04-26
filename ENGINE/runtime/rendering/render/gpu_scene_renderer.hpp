#pragma once

#include <memory>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include <SDL3/SDL.h>
#include <SDL3/SDL_gpu.h>

#include "rendering/render/gpu_frame_graph.hpp"
#include "rendering/render/gpu_render_device.hpp"
#include "rendering/render/shader_package_library.hpp"
#include "rendering/render/shader_pipeline_cache.hpp"

class GpuSceneRenderer {
public:
    struct TextureResourceSpec {
        Uint32 width = 1;
        Uint32 height = 1;
        SDL_GPUTextureFormat format = SDL_GPU_TEXTUREFORMAT_INVALID;
        SDL_GPUTextureUsageFlags usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
        Uint32 layer_count_or_depth = 1;
        Uint32 num_levels = 1;
        SDL_GPUSampleCount sample_count = SDL_GPU_SAMPLECOUNT_1;
    };

    struct BufferResourceSpec {
        Uint32 size_bytes = 0;
        SDL_GPUBufferUsageFlags usage = SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ;
    };

    static std::unique_ptr<GpuSceneRenderer> Create(SDL_Renderer* renderer,
                                                    bool prefer_depth32,
                                                    std::string& out_error);
    ~GpuSceneRenderer();

    bool ready() const { return device_ != nullptr; }
    const GpuRenderDevice* device() const { return device_.get(); }
    ShaderPipelineCache& pipeline_cache() { return pipeline_cache_; }
    const ShaderPipelineCache& pipeline_cache() const { return pipeline_cache_; }

    bool load_shader_packages(const std::string& manifest_path, std::string& out_error);
    bool has_shader_variant(const std::string& shader_name) const;
    const std::string& backend_shader_variant() const { return backend_shader_variant_; }
    void add_pass(GpuFrameGraph::PassDescriptor pass);

    bool begin_frame(std::string* out_error = nullptr);
    bool end_frame(std::string* out_error = nullptr);

    bool ensure_texture_resource(const std::string& logical_name,
                                 const TextureResourceSpec& spec,
                                 std::string& out_error);
    SDL_GPUTexture* find_texture_resource(const std::string& logical_name) const;
    bool ensure_buffer_resource(const std::string& logical_name,
                                const BufferResourceSpec& spec,
                                std::string& out_error);
    SDL_GPUBuffer* find_buffer_resource(const std::string& logical_name) const;
    void release_runtime_resources();

private:
    struct RuntimeTextureResource {
        SDL_GPUTexture* texture = nullptr;
        TextureResourceSpec spec{};
        std::uint64_t estimated_bytes = 0;
    };

    struct RuntimeBufferResource {
        SDL_GPUBuffer* buffer = nullptr;
        BufferResourceSpec spec{};
    };

    explicit GpuSceneRenderer(std::unique_ptr<GpuRenderDevice> device);
    ShaderPipelineKey make_pipeline_key(const std::string& shader_name,
                                        std::uint32_t render_state_key = 0) const;
    const ShaderPackageLibrary::ShaderBinaryDescriptor* select_backend_binary(
        const ShaderPackageLibrary::ShaderVariantPath& variant) const;
    bool warmup_required_pipelines(std::string& out_error);
    SDL_GPUGraphicsPipeline* create_graphics_pipeline(const std::string& pipeline_name,
                                                      std::string& out_error) const;
    SDL_GPUComputePipeline* create_compute_pipeline(const std::string& pipeline_name,
                                                    std::string& out_error) const;
    SDL_GPUShader* create_shader(const ShaderPackageLibrary::ShaderBinaryDescriptor& descriptor,
                                 SDL_GPUShaderStage stage,
                                 std::string& out_error) const;
    SDL_GPUGraphicsPipeline* get_graphics_pipeline(const std::string& pipeline_name,
                                                   std::uint32_t render_state_key);
    SDL_GPUComputePipeline* get_compute_pipeline(const std::string& pipeline_name,
                                                 std::uint32_t render_state_key);

    std::unique_ptr<GpuRenderDevice> device_;
    GpuFrameGraph frame_graph_{};
    ShaderPackageLibrary shader_packages_{};
    ShaderPipelineCache pipeline_cache_{};
    std::string backend_shader_variant_ = "unknown";
    SDL_GPUShaderFormat backend_shader_format_ = SDL_GPU_SHADERFORMAT_INVALID;
    std::unordered_map<std::string, RuntimeTextureResource> texture_resources_{};
    std::unordered_map<std::string, RuntimeBufferResource> buffer_resources_{};
    std::uint64_t last_pipeline_hit_total_ = 0;
    std::uint64_t last_pipeline_miss_total_ = 0;
};
