#pragma once

#include <memory>
#include <string>
#include <vector>

#include <SDL3/SDL.h>

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

    std::unique_ptr<GpuRenderDevice> device_;
    GpuFrameGraph frame_graph_{};
    ShaderPackageLibrary shader_packages_{};
    ShaderPipelineCache pipeline_cache_{};
};
