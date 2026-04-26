#include "rendering/render/gpu_scene_renderer.hpp"

#include "rendering/render/render_diagnostics.hpp"
#include "utils/log.hpp"

#include <algorithm>
#include <array>
#include <cctype>

namespace {

constexpr std::uint32_t kComputeLightBinningStateKey = 0x0000C011u;
constexpr std::array<const char*, 6> kRequiredGraphicsPipelines = {
    "sprite_textured",
    "sprite_batched",
    "light_eval",
    "floor_compose",
    "dark_mask",
    "final_compose",
};
constexpr const char* kRequiredComputePipeline = "compute_light_binning";
constexpr const char* kFullscreenVertexVariant = "fullscreen_vertex";

std::string lowercase_ascii(std::string value) {
    std::transform(value.begin(),
                   value.end(),
                   value.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return value;
}

bool stage_matches(const std::string& stage_name, const char* expected) {
    const std::string lowered = lowercase_ascii(stage_name);
    return lowered == "auto" || lowered == expected;
}

bool choose_backend_shader_format(SDL_GPUDevice* device,
                                  std::string& out_variant,
                                  SDL_GPUShaderFormat& out_format,
                                  std::string& out_error) {
    out_variant = "unknown";
    out_format = SDL_GPU_SHADERFORMAT_INVALID;
    if (!device) {
        out_error = "SDL_GPUDevice is null";
        return false;
    }

    const SDL_GPUShaderFormat available = SDL_GetGPUShaderFormats(device);
    if ((available & SDL_GPU_SHADERFORMAT_DXIL) != 0) {
        out_variant = "dxil";
        out_format = SDL_GPU_SHADERFORMAT_DXIL;
        out_error.clear();
        return true;
    }
    if ((available & SDL_GPU_SHADERFORMAT_SPIRV) != 0) {
        out_variant = "spirv";
        out_format = SDL_GPU_SHADERFORMAT_SPIRV;
        out_error.clear();
        return true;
    }
    out_error = "No supported shader package backend for this GPU device "
                "(expected DXIL or SPIR-V support)";
    return false;
}

std::uint32_t graphics_state_key_from_index(std::size_t index) {
    return 0x1000u + static_cast<std::uint32_t>(index);
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

ShaderPipelineKey GpuSceneRenderer::make_pipeline_key(const std::string& shader_name,
                                                      std::uint32_t render_state_key) const {
    ShaderPipelineKey key{};
    key.shader_id = shader_name;
    key.variant = backend_shader_variant_;
    key.color_format = device_ ? device_->format_policy().albedo_format : SDL_GPU_TEXTUREFORMAT_INVALID;
    key.depth_format = device_ ? device_->format_policy().depth_format : SDL_GPU_TEXTUREFORMAT_INVALID;
    key.sample_count = device_ ? device_->format_policy().sample_count : SDL_GPU_SAMPLECOUNT_1;
    key.render_state_key = render_state_key;
    return key;
}

const ShaderPackageLibrary::ShaderBinaryDescriptor* GpuSceneRenderer::select_backend_binary(
    const ShaderPackageLibrary::ShaderVariantPath& variant) const {
    if (backend_shader_variant_ == "dxil") {
        return variant.dxil.available ? &variant.dxil : nullptr;
    }
    if (backend_shader_variant_ == "spirv") {
        return variant.spirv.available ? &variant.spirv : nullptr;
    }
    return nullptr;
}

SDL_GPUShader* GpuSceneRenderer::create_shader(const ShaderPackageLibrary::ShaderBinaryDescriptor& descriptor,
                                               SDL_GPUShaderStage stage,
                                               std::string& out_error) const {
    if (!device_ || !device_->gpu_device()) {
        out_error = "GPU device unavailable while creating shader";
        return nullptr;
    }
    if (backend_shader_format_ == SDL_GPU_SHADERFORMAT_INVALID) {
        out_error = "Backend shader format is invalid";
        return nullptr;
    }
    if (descriptor.payload.empty()) {
        out_error = "Shader payload is empty for " + descriptor.path.string();
        return nullptr;
    }

    SDL_GPUShaderCreateInfo create_info{};
    create_info.code_size = descriptor.payload.size();
    create_info.code = descriptor.payload.data();
    create_info.entrypoint = descriptor.entrypoint.empty() ? "main" : descriptor.entrypoint.c_str();
    create_info.format = backend_shader_format_;
    create_info.stage = stage;
    create_info.num_samplers = 0;
    create_info.num_storage_textures = 0;
    create_info.num_storage_buffers = 0;
    create_info.num_uniform_buffers = 0;
    create_info.props = 0;

    SDL_GPUShader* shader = SDL_CreateGPUShader(device_->gpu_device(), &create_info);
    if (!shader) {
        out_error = "SDL_CreateGPUShader failed for '" + descriptor.path.string() +
                    "': " + SDL_GetError();
        return nullptr;
    }

    out_error.clear();
    return shader;
}

SDL_GPUGraphicsPipeline* GpuSceneRenderer::create_graphics_pipeline(const std::string& pipeline_name,
                                                                    std::string& out_error) const {
    if (!device_ || !device_->gpu_device()) {
        out_error = "GPU device unavailable while creating graphics pipeline";
        return nullptr;
    }

    const ShaderPackageLibrary::ShaderVariantPath* fragment_variant = shader_packages_.find(pipeline_name);
    if (!fragment_variant) {
        out_error = "Missing fragment shader variant: " + pipeline_name;
        return nullptr;
    }
    const ShaderPackageLibrary::ShaderVariantPath* vertex_variant = shader_packages_.find(kFullscreenVertexVariant);
    if (!vertex_variant) {
        out_error = std::string("Missing shared vertex shader variant: ") + kFullscreenVertexVariant;
        return nullptr;
    }

    const ShaderPackageLibrary::ShaderBinaryDescriptor* fragment = select_backend_binary(*fragment_variant);
    const ShaderPackageLibrary::ShaderBinaryDescriptor* vertex = select_backend_binary(*vertex_variant);
    if (!fragment || !vertex) {
        out_error = "Missing backend shader binary for graphics pipeline: " + pipeline_name;
        return nullptr;
    }

    if (!stage_matches(vertex->stage, "vertex")) {
        out_error = "Vertex shader stage metadata mismatch for: " + vertex->path.string();
        return nullptr;
    }
    if (!stage_matches(fragment->stage, "fragment")) {
        out_error = "Fragment shader stage metadata mismatch for: " + fragment->path.string();
        return nullptr;
    }

    std::string shader_error;
    SDL_GPUShader* vertex_shader = create_shader(*vertex, SDL_GPU_SHADERSTAGE_VERTEX, shader_error);
    if (!vertex_shader) {
        out_error = shader_error;
        return nullptr;
    }
    SDL_GPUShader* fragment_shader = create_shader(*fragment, SDL_GPU_SHADERSTAGE_FRAGMENT, shader_error);
    if (!fragment_shader) {
        SDL_ReleaseGPUShader(device_->gpu_device(), vertex_shader);
        out_error = shader_error;
        return nullptr;
    }

    SDL_GPUColorTargetBlendState blend_state{};
    blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
    blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ZERO;
    blend_state.color_blend_op = SDL_GPU_BLENDOP_ADD;
    blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
    blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ZERO;
    blend_state.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
    blend_state.color_write_mask =
        SDL_GPU_COLORCOMPONENT_R |
        SDL_GPU_COLORCOMPONENT_G |
        SDL_GPU_COLORCOMPONENT_B |
        SDL_GPU_COLORCOMPONENT_A;
    blend_state.enable_blend = false;
    blend_state.enable_color_write_mask = true;

    SDL_GPUColorTargetDescription color_target{};
    color_target.format = device_->format_policy().albedo_format;
    color_target.blend_state = blend_state;

    SDL_GPUGraphicsPipelineCreateInfo create_info{};
    create_info.vertex_shader = vertex_shader;
    create_info.fragment_shader = fragment_shader;
    create_info.vertex_input_state = SDL_GPUVertexInputState{};
    create_info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    create_info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
    create_info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
    create_info.rasterizer_state.front_face = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;
    create_info.rasterizer_state.enable_depth_clip = true;
    create_info.multisample_state.sample_count = device_->format_policy().sample_count;
    create_info.multisample_state.sample_mask = 0;
    create_info.multisample_state.enable_mask = false;
    create_info.multisample_state.enable_alpha_to_coverage = false;
    create_info.depth_stencil_state.compare_op = SDL_GPU_COMPAREOP_ALWAYS;
    create_info.depth_stencil_state.enable_depth_test = false;
    create_info.depth_stencil_state.enable_depth_write = false;
    create_info.depth_stencil_state.enable_stencil_test = false;
    create_info.target_info.color_target_descriptions = &color_target;
    create_info.target_info.num_color_targets = 1;
    create_info.target_info.has_depth_stencil_target = false;
    create_info.target_info.depth_stencil_format = SDL_GPU_TEXTUREFORMAT_INVALID;
    create_info.props = 0;

    SDL_GPUGraphicsPipeline* pipeline = SDL_CreateGPUGraphicsPipeline(device_->gpu_device(), &create_info);
    SDL_ReleaseGPUShader(device_->gpu_device(), fragment_shader);
    SDL_ReleaseGPUShader(device_->gpu_device(), vertex_shader);

    if (!pipeline) {
        out_error = "SDL_CreateGPUGraphicsPipeline failed for '" + pipeline_name +
                    "': " + SDL_GetError();
        return nullptr;
    }
    out_error.clear();
    return pipeline;
}

SDL_GPUComputePipeline* GpuSceneRenderer::create_compute_pipeline(const std::string& pipeline_name,
                                                                  std::string& out_error) const {
    if (!device_ || !device_->gpu_device()) {
        out_error = "GPU device unavailable while creating compute pipeline";
        return nullptr;
    }

    const ShaderPackageLibrary::ShaderVariantPath* variant = shader_packages_.find(pipeline_name);
    if (!variant) {
        out_error = "Missing compute shader variant: " + pipeline_name;
        return nullptr;
    }

    const ShaderPackageLibrary::ShaderBinaryDescriptor* compute = select_backend_binary(*variant);
    if (!compute) {
        out_error = "Missing backend binary for compute shader variant: " + pipeline_name;
        return nullptr;
    }
    if (!stage_matches(compute->stage, "compute")) {
        out_error = "Compute shader stage metadata mismatch for: " + compute->path.string();
        return nullptr;
    }
    if (compute->payload.empty()) {
        out_error = "Compute shader payload is empty for: " + compute->path.string();
        return nullptr;
    }

    SDL_GPUComputePipelineCreateInfo create_info{};
    create_info.code_size = compute->payload.size();
    create_info.code = compute->payload.data();
    create_info.entrypoint = compute->entrypoint.empty() ? "main" : compute->entrypoint.c_str();
    create_info.format = backend_shader_format_;
    create_info.num_samplers = 0;
    create_info.num_readonly_storage_textures = 0;
    create_info.num_readonly_storage_buffers = 0;
    create_info.num_readwrite_storage_textures = 0;
    create_info.num_readwrite_storage_buffers = 0;
    create_info.num_uniform_buffers = 0;
    create_info.threadcount_x = 8;
    create_info.threadcount_y = 8;
    create_info.threadcount_z = 1;
    create_info.props = 0;

    SDL_GPUComputePipeline* pipeline = SDL_CreateGPUComputePipeline(device_->gpu_device(), &create_info);
    if (!pipeline) {
        out_error = "SDL_CreateGPUComputePipeline failed for '" + pipeline_name +
                    "': " + SDL_GetError();
        return nullptr;
    }
    out_error.clear();
    return pipeline;
}

bool GpuSceneRenderer::warmup_required_pipelines(std::string& out_error) {
    if (!device_ || !device_->gpu_device()) {
        out_error = "GPU device unavailable during pipeline warmup";
        return false;
    }

    const ShaderPackageLibrary::ShaderVariantPath* fullscreen_vs = shader_packages_.find(kFullscreenVertexVariant);
    if (!fullscreen_vs) {
        out_error = std::string("Missing required vertex variant: ") + kFullscreenVertexVariant;
        return false;
    }
    const ShaderPackageLibrary::ShaderBinaryDescriptor* fullscreen_desc = select_backend_binary(*fullscreen_vs);
    if (!fullscreen_desc) {
        out_error = std::string("Missing backend binary for required vertex variant: ") + kFullscreenVertexVariant;
        return false;
    }
    if (!stage_matches(fullscreen_desc->stage, "vertex")) {
        out_error = "Required vertex variant has invalid stage metadata: " + fullscreen_desc->stage;
        return false;
    }

    std::string pipeline_error;
    {
        const ShaderPipelineKey key = make_pipeline_key(kRequiredComputePipeline, kComputeLightBinningStateKey);
        SDL_GPUComputePipeline* pipeline = pipeline_cache_.get_or_create_compute_pipeline(
            key,
            [&]() { return create_compute_pipeline(kRequiredComputePipeline, pipeline_error); });
        if (!pipeline) {
            out_error = "Failed to warmup compute pipeline '" +
                        std::string(kRequiredComputePipeline) + "': " + pipeline_error;
            return false;
        }
    }

    for (std::size_t i = 0; i < kRequiredGraphicsPipelines.size(); ++i) {
        const std::string pipeline_name = kRequiredGraphicsPipelines[i];
        const ShaderPackageLibrary::ShaderVariantPath* variant = shader_packages_.find(pipeline_name);
        if (!variant) {
            out_error = "Missing required graphics variant: " + pipeline_name;
            return false;
        }
        const ShaderPackageLibrary::ShaderBinaryDescriptor* descriptor = select_backend_binary(*variant);
        if (!descriptor) {
            out_error = "Missing backend binary for required graphics variant: " + pipeline_name;
            return false;
        }
        if (!stage_matches(descriptor->stage, "fragment")) {
            out_error = "Required graphics variant has invalid stage metadata: " +
                        pipeline_name + " stage=" + descriptor->stage;
            return false;
        }

        const ShaderPipelineKey key = make_pipeline_key(pipeline_name, graphics_state_key_from_index(i));
        SDL_GPUGraphicsPipeline* pipeline = pipeline_cache_.get_or_create_graphics_pipeline(
            key,
            [&]() { return create_graphics_pipeline(pipeline_name, pipeline_error); });
        if (!pipeline) {
            out_error = "Failed to warmup graphics pipeline '" + pipeline_name + "': " + pipeline_error;
            return false;
        }
    }

    out_error.clear();
    return true;
}

bool GpuSceneRenderer::load_shader_packages(const std::string& manifest_path, std::string& out_error) {
    const bool ok = shader_packages_.load_from_manifest(manifest_path, out_error);
    if (!ok) {
        vibble::log::error("[GpuSceneRenderer] Shader package load failed: " + out_error);
        return false;
    }

    if (!choose_backend_shader_format(device_ ? device_->gpu_device() : nullptr,
                                      backend_shader_variant_,
                                      backend_shader_format_,
                                      out_error)) {
        vibble::log::error("[GpuSceneRenderer] " + out_error);
        return false;
    }

    const ShaderPackageLibrary::ShaderVariantPath* compute_variant = shader_packages_.find(kRequiredComputePipeline);
    if (!compute_variant || !select_backend_binary(*compute_variant)) {
        out_error = std::string("Missing ") + backend_shader_variant_ +
                    " binary for required variant: " + kRequiredComputePipeline;
        vibble::log::error("[GpuSceneRenderer] " + out_error);
        return false;
    }

    for (const char* required : kRequiredGraphicsPipelines) {
        const ShaderPackageLibrary::ShaderVariantPath* variant = shader_packages_.find(required);
        if (!variant || !select_backend_binary(*variant)) {
            out_error = std::string("Missing ") + backend_shader_variant_ +
                        " binary for required variant: " + required;
            vibble::log::error("[GpuSceneRenderer] " + out_error);
            return false;
        }
    }

    if (!warmup_required_pipelines(out_error)) {
        vibble::log::error("[GpuSceneRenderer] " + out_error);
        return false;
    }

    vibble::log::info("[GpuSceneRenderer] Shader packages loaded from: " + manifest_path);
    vibble::log::info("[GpuSceneRenderer] Shader manifest version=" +
                      std::to_string(shader_packages_.manifest_version()) +
                      " variants=" + std::to_string(shader_packages_.variant_count()) +
                      " backend_variant=" + backend_shader_variant_ +
                      " graphics_pipelines=" + std::to_string(pipeline_cache_.graphics_pipeline_count()) +
                      " compute_pipelines=" + std::to_string(pipeline_cache_.compute_pipeline_count()));
    out_error.clear();
    return true;
}

bool GpuSceneRenderer::has_shader_variant(const std::string& shader_name) const {
    return shader_packages_.find(shader_name) != nullptr;
}

void GpuSceneRenderer::touch_graphics_pipeline(const std::string& shader_name,
                                               std::uint32_t render_state_key) {
    const ShaderPipelineKey key = make_pipeline_key(shader_name, render_state_key);
    (void)pipeline_cache_.get_or_create_graphics_pipeline(
        key,
        []() -> SDL_GPUGraphicsPipeline* { return nullptr; });
}

bool GpuSceneRenderer::dispatch_compute_light_binning(Uint32 group_count_x,
                                                      Uint32 group_count_y,
                                                      std::string* out_error) {
    if (!device_ || !device_->gpu_device()) {
        if (out_error) {
            *out_error = "GPU device unavailable";
        }
        return false;
    }
    const GpuRenderDevice::FrameState& frame_state = device_->frame_state();
    if (!frame_state.command_buffer) {
        if (out_error) {
            *out_error = "No active GPU frame command buffer";
        }
        return false;
    }

    std::string pipeline_error;
    const ShaderPipelineKey key = make_pipeline_key(kRequiredComputePipeline, kComputeLightBinningStateKey);
    SDL_GPUComputePipeline* pipeline = pipeline_cache_.get_or_create_compute_pipeline(
        key,
        [&]() { return create_compute_pipeline(kRequiredComputePipeline, pipeline_error); });
    if (!pipeline) {
        if (out_error) {
            *out_error = pipeline_error.empty() ? "compute pipeline unavailable" : pipeline_error;
        }
        return false;
    }

    SDL_GPUComputePass* compute_pass = SDL_BeginGPUComputePass(frame_state.command_buffer, nullptr, 0, nullptr, 0);
    if (!compute_pass) {
        if (out_error) {
            *out_error = std::string("SDL_BeginGPUComputePass failed: ") + SDL_GetError();
        }
        return false;
    }

    SDL_BindGPUComputePipeline(compute_pass, pipeline);
    SDL_DispatchGPUCompute(compute_pass,
                           std::max<Uint32>(1, group_count_x),
                           std::max<Uint32>(1, group_count_y),
                           1);
    SDL_EndGPUComputePass(compute_pass);
    if (out_error) {
        out_error->clear();
    }
    return true;
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
                       std::to_string(pipeline_cache_.hit_rate()) +
                       " graphics=" + std::to_string(pipeline_cache_.graphics_pipeline_count()) +
                       " compute=" + std::to_string(pipeline_cache_.compute_pipeline_count()));
}
