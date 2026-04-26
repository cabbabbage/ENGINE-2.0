#include "rendering/render/gpu_scene_renderer.hpp"

#include "rendering/render/render_diagnostics.hpp"
#include "utils/log.hpp"

#include <algorithm>
#include <array>
#include <cctype>

namespace {

std::string lowercase_ascii(std::string value) {
    std::transform(value.begin(),
                   value.end(),
                   value.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return value;
}

bool prefer_dxil_backend(const std::string& backend_name) {
    const std::string lowered = lowercase_ascii(backend_name);
    return lowered.find("d3d") != std::string::npos ||
           lowered.find("direct3d") != std::string::npos;
}

} // namespace

GpuSceneRenderer::GpuSceneRenderer(std::unique_ptr<GpuRenderDevice> device)
    : device_(std::move(device)) {}

GpuSceneRenderer::~GpuSceneRenderer() {
    pipeline_cache_.clear(device_ ? device_->gpu_device() : nullptr);
}

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

    static constexpr std::array<const char*, 7> kRequiredVariants = {
        "sprite_textured",
        "sprite_batched",
        "light_eval",
        "floor_compose",
        "dark_mask",
        "final_compose",
        "compute_light_binning",
    };
    const bool use_dxil = prefer_dxil_backend(device_ ? device_->backend_name() : std::string{});
    for (const char* required : kRequiredVariants) {
        const ShaderPackageLibrary::ShaderVariantPath* variant = shader_packages_.find(required);
        if (!variant) {
            out_error = std::string("Missing required shader variant: ") + required;
            vibble::log::error("[GpuSceneRenderer] " + out_error);
            return false;
        }

        const bool backend_variant_available =
            use_dxil ? variant->dxil.available : variant->spirv.available;
        if (!backend_variant_available) {
            out_error = std::string("Missing ") + (use_dxil ? "DXIL" : "SPIR-V") +
                        " binary for required variant: " + required;
            vibble::log::error("[GpuSceneRenderer] " + out_error);
            return false;
        }
    }

    for (const char* required : kRequiredVariants) {
        ShaderPipelineKey key{};
        key.shader_id = required;
        key.variant = use_dxil ? "dxil" : "spirv";
        key.color_format = device_ ? device_->format_policy().albedo_format : SDL_GPU_TEXTUREFORMAT_INVALID;
        key.depth_format = device_ ? device_->format_policy().depth_format : SDL_GPU_TEXTUREFORMAT_INVALID;
        pipeline_cache_.register_miss(key);
    }

    vibble::log::info("[GpuSceneRenderer] Shader packages loaded from: " + manifest_path);
    vibble::log::info("[GpuSceneRenderer] Shader manifest version=" +
                      std::to_string(shader_packages_.manifest_version()) +
                      " variants=" + std::to_string(shader_packages_.variant_count()) +
                      " backend_variant=" + std::string(use_dxil ? "dxil" : "spirv"));
    out_error.clear();
    return true;
}

bool GpuSceneRenderer::has_shader_variant(const std::string& shader_name) const {
    return shader_packages_.find(shader_name) != nullptr;
}

void GpuSceneRenderer::add_render_pass(std::string name,
                                       GpuFrameGraph::PassCallback callback,
                                       std::vector<GpuFrameGraph::ResourceDependency> resources) {
    frame_graph_.add_render_pass(std::move(name), std::move(callback), std::move(resources));
}

void GpuSceneRenderer::add_copy_pass(std::string name,
                                     GpuFrameGraph::PassCallback callback,
                                     std::vector<GpuFrameGraph::ResourceDependency> resources) {
    frame_graph_.add_copy_pass(std::move(name), std::move(callback), std::move(resources));
}

void GpuSceneRenderer::add_compute_pass(std::string name,
                                        GpuFrameGraph::PassCallback callback,
                                        std::vector<GpuFrameGraph::ResourceDependency> resources) {
    frame_graph_.add_compute_pass(std::move(name), std::move(callback), std::move(resources));
}

void GpuSceneRenderer::begin_frame() {
    std::string frame_error;
    if (device_ && !device_->begin_frame(frame_error)) {
        vibble::log::error("[GpuSceneRenderer] begin_frame failed: " + frame_error);
    }
    frame_graph_.reset();
    render_diagnostics::set_renderer_runtime_info("gpu",
                                                  device_ ? device_->backend_name() : "unknown",
                                                  device_ ? device_->present_mode() : "unknown");
    std::uint64_t texture_memory_bytes = 0;
    const bool texture_memory_known =
        device_ ? device_->query_texture_memory_usage(texture_memory_bytes) : false;
    render_diagnostics::set_texture_memory_usage(texture_memory_bytes, texture_memory_known);
}

void GpuSceneRenderer::end_frame() {
    const GpuFrameGraph::ExecutionStats graph_stats = frame_graph_.execute();
    std::string frame_error;
    if (device_ && !device_->end_frame(true, frame_error)) {
        vibble::log::error("[GpuSceneRenderer] end_frame submit failed: " + frame_error);
    }
    vibble::log::debug("[GpuSceneRenderer] Pass graph executed: render=" +
                       std::to_string(graph_stats.render_pass_count) +
                       " copy=" + std::to_string(graph_stats.copy_pass_count) +
                       " compute=" + std::to_string(graph_stats.compute_pass_count) +
                       " dependency_warnings=" + std::to_string(graph_stats.dependency_warning_count));
    vibble::log::debug("[GpuSceneRenderer] Pipeline cache hit-rate=" +
                       std::to_string(pipeline_cache_.hit_rate()));
}
