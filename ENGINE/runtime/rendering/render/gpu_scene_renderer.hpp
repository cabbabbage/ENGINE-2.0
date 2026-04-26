#pragma once

#include <memory>
#include <string>
#include <vector>

#include <SDL3/SDL.h>
#include <SDL3/SDL_gpu.h>

#include "rendering/render/gpu_frame_graph.hpp"
#include "rendering/render/gpu_render_device.hpp"
#include "rendering/render/shader_package_library.hpp"
#include "rendering/render/shader_pipeline_cache.hpp"

class GpuSceneRenderer {
public:
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
    bool dispatch_compute_light_binning(Uint32 group_count_x,
                                        Uint32 group_count_y,
                                        std::string* out_error = nullptr);
    void touch_graphics_pipeline(const std::string& shader_name,
                                 std::uint32_t render_state_key = 0);

    void add_render_pass(std::string name,
                         GpuFrameGraph::PassCallback callback,
                         std::vector<GpuFrameGraph::ResourceDependency> resources = {});
    void add_copy_pass(std::string name,
                       GpuFrameGraph::PassCallback callback,
                       std::vector<GpuFrameGraph::ResourceDependency> resources = {});
    void add_compute_pass(std::string name,
                          GpuFrameGraph::PassCallback callback,
                          std::vector<GpuFrameGraph::ResourceDependency> resources = {});

    void begin_frame();
    void end_frame();

private:
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

    std::unique_ptr<GpuRenderDevice> device_;
    GpuFrameGraph frame_graph_{};
    ShaderPackageLibrary shader_packages_{};
    ShaderPipelineCache pipeline_cache_{};
    std::string backend_shader_variant_ = "unknown";
    SDL_GPUShaderFormat backend_shader_format_ = SDL_GPU_SHADERFORMAT_INVALID;
};
