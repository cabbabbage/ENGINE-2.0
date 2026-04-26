#include "rendering/render/gpu_scene_renderer.hpp"

#include "rendering/render/render_diagnostics.hpp"
#include "utils/log.hpp"

GpuSceneRenderer::GpuSceneRenderer(std::unique_ptr<GpuRenderDevice> device)
    : device_(std::move(device)) {}

std::unique_ptr<GpuSceneRenderer> GpuSceneRenderer::Create(SDL_Renderer* renderer,
                                                           bool prefer_depth32,
                                                           std::string& out_error) {
    std::unique_ptr<GpuRenderDevice> device = GpuRenderDevice::Create(renderer, prefer_depth32, out_error);
    if (!device) {
        return nullptr;
    }
    return std::unique_ptr<GpuSceneRenderer>(new GpuSceneRenderer(std::move(device)));
}

bool GpuSceneRenderer::load_shader_packages(const std::string& manifest_path, std::string& out_error) {
    const bool ok = shader_packages_.load_from_manifest(manifest_path, out_error);
    if (!ok) {
        vibble::log::error("[GpuSceneRenderer] Shader package load failed: " + out_error);
        return false;
    }
    vibble::log::info("[GpuSceneRenderer] Shader packages loaded from: " + manifest_path);
    out_error.clear();
    return true;
}

bool GpuSceneRenderer::has_shader_variant(const std::string& shader_name) const {
    return shader_packages_.find(shader_name) != nullptr;
}

void GpuSceneRenderer::add_render_pass(std::string name, GpuFrameGraph::PassCallback callback) {
    frame_graph_.add_render_pass(std::move(name), std::move(callback));
}

void GpuSceneRenderer::add_copy_pass(std::string name, GpuFrameGraph::PassCallback callback) {
    frame_graph_.add_copy_pass(std::move(name), std::move(callback));
}

void GpuSceneRenderer::add_compute_pass(std::string name, GpuFrameGraph::PassCallback callback) {
    frame_graph_.add_compute_pass(std::move(name), std::move(callback));
}

void GpuSceneRenderer::begin_frame() {
    frame_graph_.reset();
    render_diagnostics::set_renderer_runtime_info("gpu", device_ ? device_->backend_name() : "unknown", device_ ? device_->present_mode() : "unknown");
}

void GpuSceneRenderer::end_frame() {
    const GpuFrameGraph::ExecutionStats graph_stats = frame_graph_.execute();
    vibble::log::debug("[GpuSceneRenderer] Pass graph executed: render=" +
                       std::to_string(graph_stats.render_pass_count) +
                       " copy=" + std::to_string(graph_stats.copy_pass_count) +
                       " compute=" + std::to_string(graph_stats.compute_pass_count));
    vibble::log::debug("[GpuSceneRenderer] Pipeline cache hit-rate=" +
                       std::to_string(pipeline_cache_.hit_rate()));
}
