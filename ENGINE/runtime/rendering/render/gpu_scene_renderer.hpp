#pragma once

#include <memory>
#include <array>
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

struct GpuSpriteVertex {
    float clip_x = 0.0f;
    float clip_y = 0.0f;
    float uv_x = 0.0f;
    float uv_y = 0.0f;
};

struct GpuSpriteDrawPacket {
    SDL_Texture* source_texture = nullptr;
    SDL_GPUTexture* source_gpu_texture = nullptr;
    std::string source_asset_name{};
    std::string source_texture_id{};
    std::array<GpuSpriteVertex, 6> vertices{};
    SDL_FColor modulate{1.0f, 1.0f, 1.0f, 1.0f};
    std::uint8_t sort_group = 0;
    float sort_key = 0.0f;
    std::uintptr_t stable_sort_id = 0u;
};

struct GpuSceneFrameData {
    std::vector<GpuSpriteDrawPacket> floor_draws{};
    std::vector<GpuSpriteDrawPacket> layer_draws{};
    std::uint32_t floor_draw_count = 0;
    std::uint32_t layer_sprite_draw_count = 0;
    std::uint32_t debug_overlay_draw_count = 0;
    bool has_valid_composite_source = false;
};

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

    struct SamplerResourceSpec {
        SDL_GPUFilter min_filter = SDL_GPU_FILTER_LINEAR;
        SDL_GPUFilter mag_filter = SDL_GPU_FILTER_LINEAR;
        SDL_GPUSamplerMipmapMode mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_LINEAR;
        SDL_GPUSamplerAddressMode address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        SDL_GPUSamplerAddressMode address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        SDL_GPUSamplerAddressMode address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        float mip_lod_bias = 0.0f;
        float max_anisotropy = 1.0f;
        SDL_GPUCompareOp compare_op = SDL_GPU_COMPAREOP_ALWAYS;
        float min_lod = 0.0f;
        float max_lod = 16.0f;
        bool enable_anisotropy = false;
        bool enable_compare = false;
    };

    static std::unique_ptr<GpuSceneRenderer> Create(SDL_Renderer* renderer,
                                                    bool prefer_depth32,
                                                    std::string& out_error);
    ~GpuSceneRenderer();

    bool ready() const { return device_ != nullptr; }
    GpuRenderDevice* device() { return device_.get(); }
    const GpuRenderDevice* device() const { return device_.get(); }
    ShaderPipelineCache& pipeline_cache() { return pipeline_cache_; }
    const ShaderPipelineCache& pipeline_cache() const { return pipeline_cache_; }

    bool load_shader_packages(const std::string& manifest_path, std::string& out_error);
    bool has_shader_variant(const std::string& shader_name) const;
    const std::string& backend_shader_variant() const { return backend_shader_variant_; }
    bool render_frame(const GpuSceneFrameData& frame_data, std::string& out_error);
    void reset_frame_graph();
    void add_pass(GpuFrameGraph::PassDescriptor pass);

    bool begin_frame(std::string* out_error = nullptr, bool reset_frame_graph = true);
    bool end_frame(std::string* out_error = nullptr);
    void abort_frame();

    bool ensure_texture_resource(const std::string& logical_name,
                                 const TextureResourceSpec& spec,
                                 std::string& out_error);
    bool register_external_texture_resource(const std::string& logical_name, SDL_GPUTexture* texture);
    void clear_external_texture_resources();
    SDL_GPUTexture* find_texture_resource(const std::string& logical_name) const;
    SDL_GPUTexture* resolve_gpu_texture_for_sdl_texture(SDL_Texture* texture, std::string& out_error);
    SDL_GPUTexture* find_gpu_texture_for_sdl_texture(SDL_Texture* texture) const;
    bool ensure_buffer_resource(const std::string& logical_name,
                                const BufferResourceSpec& spec,
                                std::string& out_error);
    SDL_GPUBuffer* find_buffer_resource(const std::string& logical_name) const;
    SDL_GPUGraphicsPipeline* resolve_graphics_pipeline(const std::string& pipeline_name,
                                                       std::uint32_t render_state_key);
    bool ensure_sampler_resource(const std::string& logical_name,
                                 const SamplerResourceSpec& spec,
                                 std::string& out_error);
    SDL_GPUSampler* find_sampler_resource(const std::string& logical_name) const;
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

    struct RuntimeSamplerResource {
        SDL_GPUSampler* sampler = nullptr;
        SamplerResourceSpec spec{};
    };

    struct ImportedSdlTextureResource {
        SDL_Texture* source_texture = nullptr;
        SDL_GPUTexture* gpu_texture = nullptr;
        std::uintptr_t revision = 0;
        Uint32 width = 0;
        Uint32 height = 0;
    };

    explicit GpuSceneRenderer(std::unique_ptr<GpuRenderDevice> device);
    ShaderPipelineKey make_pipeline_key(const std::string& shader_name,
                                        std::uint32_t render_state_key = 0) const;
    const ShaderPackageLibrary::ShaderBinaryDescriptor* select_backend_binary(
        const ShaderPackageLibrary::ShaderVariantPath& variant) const;
    bool warmup_required_pipelines(std::string& out_error);
    SDL_GPUGraphicsPipeline* create_graphics_pipeline(const std::string& pipeline_name,
                                                      SDL_GPUTextureFormat color_target_format,
                                                      std::string& out_error) const;
    SDL_GPUComputePipeline* create_compute_pipeline(const std::string& pipeline_name,
                                                    std::string& out_error) const;
    SDL_GPUShader* create_shader(const ShaderPackageLibrary::ShaderBinaryDescriptor& descriptor,
                                 SDL_GPUShaderStage stage,
                                 std::uint32_t num_samplers,
                                 std::uint32_t num_storage_textures,
                                 std::uint32_t num_storage_buffers,
                                 std::uint32_t num_uniform_buffers,
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
    std::unordered_map<std::string, SDL_GPUTexture*> external_texture_resources_{};
    std::unordered_map<SDL_Texture*, ImportedSdlTextureResource> imported_sdl_texture_resources_{};
    std::unordered_map<std::string, RuntimeBufferResource> buffer_resources_{};
    std::unordered_map<std::string, RuntimeSamplerResource> sampler_resources_{};
    std::uint64_t last_pipeline_hit_total_ = 0;
    std::uint64_t last_pipeline_miss_total_ = 0;
};
