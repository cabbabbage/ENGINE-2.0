#include "rendering/render/gpu_scene_renderer.hpp"

#include "rendering/render/render_diagnostics.hpp"
#include "utils/log.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <utility>

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

bool texture_spec_matches(const GpuSceneRenderer::TextureResourceSpec& lhs,
                          const GpuSceneRenderer::TextureResourceSpec& rhs) {
    return lhs.width == rhs.width &&
           lhs.height == rhs.height &&
           lhs.format == rhs.format &&
           lhs.usage == rhs.usage &&
           lhs.layer_count_or_depth == rhs.layer_count_or_depth &&
           lhs.num_levels == rhs.num_levels &&
           lhs.sample_count == rhs.sample_count;
}

bool buffer_spec_matches(const GpuSceneRenderer::BufferResourceSpec& lhs,
                         const GpuSceneRenderer::BufferResourceSpec& rhs) {
    return lhs.size_bytes == rhs.size_bytes &&
           lhs.usage == rhs.usage;
}

std::uint64_t estimate_gpu_texture_bytes(const GpuSceneRenderer::TextureResourceSpec& spec) {
    const std::uint64_t width = std::max<std::uint64_t>(1u, spec.width);
    const std::uint64_t height = std::max<std::uint64_t>(1u, spec.height);
    const std::uint64_t layers = std::max<std::uint64_t>(1u, spec.layer_count_or_depth);
    const std::uint64_t levels = std::max<std::uint64_t>(1u, spec.num_levels);
    const std::uint64_t samples = std::max<std::uint64_t>(1u, static_cast<std::uint64_t>(spec.sample_count));
    std::uint64_t bpp = 4;
    switch (spec.format) {
    case SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT:
        bpp = 8;
        break;
    case SDL_GPU_TEXTUREFORMAT_R11G11B10_UFLOAT:
    case SDL_GPU_TEXTUREFORMAT_D24_UNORM_S8_UINT:
    case SDL_GPU_TEXTUREFORMAT_D32_FLOAT:
    case SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM:
    case SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM_SRGB:
        bpp = 4;
        break;
    case SDL_GPU_TEXTUREFORMAT_R8_UNORM:
        bpp = 1;
        break;
    default:
        bpp = 4;
        break;
    }
    return width * height * layers * levels * samples * bpp;
}

} // namespace

GpuSceneRenderer::GpuSceneRenderer(std::unique_ptr<GpuRenderDevice> device)
    : device_(std::move(device)) {}

GpuSceneRenderer::~GpuSceneRenderer() {
    release_runtime_resources();
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

SDL_GPUGraphicsPipeline* GpuSceneRenderer::get_graphics_pipeline(const std::string& pipeline_name,
                                                                 std::uint32_t render_state_key) {
    std::string pipeline_error;
    const ShaderPipelineKey key = make_pipeline_key(pipeline_name, render_state_key);
    SDL_GPUGraphicsPipeline* pipeline = pipeline_cache_.get_or_create_graphics_pipeline(
        key,
        [&]() { return create_graphics_pipeline(pipeline_name, pipeline_error); });
    if (!pipeline && !pipeline_error.empty()) {
        vibble::log::error("[GpuSceneRenderer] Graphics pipeline resolve failed for '" +
                           pipeline_name + "': " + pipeline_error);
    }
    return pipeline;
}

SDL_GPUComputePipeline* GpuSceneRenderer::get_compute_pipeline(const std::string& pipeline_name,
                                                               std::uint32_t render_state_key) {
    std::string pipeline_error;
    const ShaderPipelineKey key = make_pipeline_key(pipeline_name, render_state_key);
    SDL_GPUComputePipeline* pipeline = pipeline_cache_.get_or_create_compute_pipeline(
        key,
        [&]() { return create_compute_pipeline(pipeline_name, pipeline_error); });
    if (!pipeline && !pipeline_error.empty()) {
        vibble::log::error("[GpuSceneRenderer] Compute pipeline resolve failed for '" +
                           pipeline_name + "': " + pipeline_error);
    }
    return pipeline;
}

void GpuSceneRenderer::add_pass(GpuFrameGraph::PassDescriptor pass) {
    frame_graph_.add_pass(std::move(pass));
}

bool GpuSceneRenderer::begin_frame(std::string* out_error) {
    std::string frame_error;
    if (device_ && !device_->begin_frame(frame_error)) {
        vibble::log::error("[GpuSceneRenderer] begin_frame failed: " + frame_error);
        if (out_error) {
            *out_error = frame_error;
        }
        return false;
    }
    if (!device_) {
        if (out_error) {
            *out_error = "GPU device is unavailable";
        }
        return false;
    }
    frame_graph_.reset();
    render_diagnostics::set_renderer_runtime_info("gpu",
                                                  device_ ? device_->backend_name() : "unknown",
                                                  device_ ? device_->present_mode() : "unknown");
    std::uint64_t texture_memory_bytes = 0;
    const bool texture_memory_known =
        device_ ? device_->query_texture_memory_usage(texture_memory_bytes) : false;
    render_diagnostics::set_texture_memory_usage(texture_memory_bytes, texture_memory_known);
    last_pipeline_hit_total_ = pipeline_cache_.total_hits();
    last_pipeline_miss_total_ = pipeline_cache_.total_misses();
    if (out_error) {
        out_error->clear();
    }
    return true;
}

bool GpuSceneRenderer::end_frame(std::string* out_error) {
    const GpuRenderDevice::FrameState& frame_state = device_ ? device_->frame_state() : GpuRenderDevice::FrameState{};
    GpuFrameGraph::ExecuteOptions execute_options{};
    execute_options.strict_resource_validation = true;
    execute_options.fail_on_validation_error = true;
    execute_options.fail_on_missing_resource = true;
    execute_options.fail_on_missing_pipeline = true;
    GpuFrameGraph::ExecuteContext execute_context{};
    execute_context.command_buffer = frame_state.command_buffer;
    execute_context.swapchain_texture = frame_state.swapchain_texture;
    execute_context.swapchain_width = frame_state.swapchain_width;
    execute_context.swapchain_height = frame_state.swapchain_height;
    execute_context.resolve_texture = [this](const std::string& name) {
        return find_texture_resource(name);
    };
    execute_context.resolve_buffer = [this](const std::string& name) {
        return find_buffer_resource(name);
    };
    execute_context.resolve_graphics_pipeline = [this](const std::string& name, std::uint32_t state_key) {
        return get_graphics_pipeline(name, state_key);
    };
    execute_context.resolve_compute_pipeline = [this](const std::string& name, std::uint32_t state_key) {
        return get_compute_pipeline(name, state_key);
    };
    const GpuFrameGraph::ExecutionStats graph_stats = frame_graph_.execute(execute_context, execute_options);
    if (!graph_stats.success) {
        std::string frame_error = graph_stats.error_message.empty()
            ? "Frame graph dependency validation failed"
            : graph_stats.error_message;
        if (device_) {
            std::string cancel_error;
            (void)device_->end_frame(false, cancel_error);
        }
        if (out_error) {
            *out_error = frame_error;
        }
        vibble::log::error("[GpuSceneRenderer] Frame graph execution failed: " + frame_error);
        return false;
    }
    std::string frame_error;
    if (device_ && !device_->end_frame(true, frame_error)) {
        vibble::log::error("[GpuSceneRenderer] end_frame submit failed: " + frame_error);
        if (out_error) {
            *out_error = frame_error;
        }
        return false;
    }
    const std::uint64_t total_hits = pipeline_cache_.total_hits();
    const std::uint64_t total_misses = pipeline_cache_.total_misses();
    const std::uint64_t frame_hits = (total_hits >= last_pipeline_hit_total_)
        ? (total_hits - last_pipeline_hit_total_) : total_hits;
    const std::uint64_t frame_misses = (total_misses >= last_pipeline_miss_total_)
        ? (total_misses - last_pipeline_miss_total_) : total_misses;
    const double frame_hit_rate = (frame_hits + frame_misses) == 0
        ? 1.0
        : static_cast<double>(frame_hits) /
            static_cast<double>(frame_hits + frame_misses);
    render_diagnostics::set_gpu_pipeline_cache_stats(frame_hits, frame_misses, frame_hit_rate);
    vibble::log::debug("[GpuSceneRenderer] Pass graph executed: render=" +
                       std::to_string(graph_stats.render_pass_count) +
                       " copy=" + std::to_string(graph_stats.copy_pass_count) +
                       " compute=" + std::to_string(graph_stats.compute_pass_count) +
                       " dependency_warnings=" + std::to_string(graph_stats.dependency_warning_count) +
                       " dependency_errors=" + std::to_string(graph_stats.dependency_error_count));
    vibble::log::debug("[GpuSceneRenderer] Pipeline cache hit-rate=" +
                       std::to_string(frame_hit_rate) +
                       " frame_hits=" + std::to_string(frame_hits) +
                       " frame_misses=" + std::to_string(frame_misses) +
                       " graphics=" + std::to_string(pipeline_cache_.graphics_pipeline_count()) +
                       " compute=" + std::to_string(pipeline_cache_.compute_pipeline_count()));
    if (out_error) {
        out_error->clear();
    }
    return true;
}

bool GpuSceneRenderer::ensure_texture_resource(const std::string& logical_name,
                                               const TextureResourceSpec& spec,
                                               std::string& out_error) {
    if (!device_ || !device_->gpu_device()) {
        out_error = "GPU device unavailable while creating texture resource '" + logical_name + "'";
        return false;
    }
    if (logical_name.empty()) {
        out_error = "Texture resource name cannot be empty";
        return false;
    }
    if (spec.width == 0 || spec.height == 0) {
        out_error = "Texture resource '" + logical_name + "' has invalid dimensions";
        return false;
    }
    if (spec.format == SDL_GPU_TEXTUREFORMAT_INVALID) {
        out_error = "Texture resource '" + logical_name + "' has invalid format";
        return false;
    }
    const auto it = texture_resources_.find(logical_name);
    if (it != texture_resources_.end() &&
        it->second.texture &&
        texture_spec_matches(it->second.spec, spec)) {
        out_error.clear();
        return true;
    }

    if (it != texture_resources_.end() && it->second.texture) {
        SDL_ReleaseGPUTexture(device_->gpu_device(), it->second.texture);
        it->second.texture = nullptr;
        render_diagnostics::add_texture_destroy_count();
    }

    SDL_GPUTextureCreateInfo create_info{};
    create_info.type = SDL_GPU_TEXTURETYPE_2D;
    create_info.format = spec.format;
    create_info.usage = spec.usage;
    create_info.width = spec.width;
    create_info.height = spec.height;
    create_info.layer_count_or_depth = spec.layer_count_or_depth;
    create_info.num_levels = spec.num_levels;
    create_info.sample_count = spec.sample_count;
    create_info.props = 0;
    SDL_GPUTexture* texture = SDL_CreateGPUTexture(device_->gpu_device(), &create_info);
    if (!texture) {
        out_error = "SDL_CreateGPUTexture failed for '" + logical_name + "': " + SDL_GetError();
        return false;
    }

    RuntimeTextureResource resource{};
    resource.texture = texture;
    resource.spec = spec;
    resource.estimated_bytes = estimate_gpu_texture_bytes(spec);
    texture_resources_[logical_name] = resource;
    render_diagnostics::add_texture_create_count();
    out_error.clear();
    return true;
}

SDL_GPUTexture* GpuSceneRenderer::find_texture_resource(const std::string& logical_name) const {
    const auto it = texture_resources_.find(logical_name);
    return (it != texture_resources_.end()) ? it->second.texture : nullptr;
}

bool GpuSceneRenderer::ensure_buffer_resource(const std::string& logical_name,
                                              const BufferResourceSpec& spec,
                                              std::string& out_error) {
    if (!device_ || !device_->gpu_device()) {
        out_error = "GPU device unavailable while creating buffer resource '" + logical_name + "'";
        return false;
    }
    if (logical_name.empty()) {
        out_error = "Buffer resource name cannot be empty";
        return false;
    }
    if (spec.size_bytes == 0) {
        out_error = "Buffer resource '" + logical_name + "' has zero byte size";
        return false;
    }

    const auto it = buffer_resources_.find(logical_name);
    if (it != buffer_resources_.end() &&
        it->second.buffer &&
        buffer_spec_matches(it->second.spec, spec)) {
        out_error.clear();
        return true;
    }

    if (it != buffer_resources_.end() && it->second.buffer) {
        SDL_ReleaseGPUBuffer(device_->gpu_device(), it->second.buffer);
        it->second.buffer = nullptr;
        render_diagnostics::add_gpu_buffer_destroy_count();
    }

    SDL_GPUBufferCreateInfo create_info{};
    create_info.usage = spec.usage;
    create_info.size = spec.size_bytes;
    create_info.props = 0;
    SDL_GPUBuffer* buffer = SDL_CreateGPUBuffer(device_->gpu_device(), &create_info);
    if (!buffer) {
        out_error = "SDL_CreateGPUBuffer failed for '" + logical_name + "': " + SDL_GetError();
        return false;
    }

    RuntimeBufferResource resource{};
    resource.buffer = buffer;
    resource.spec = spec;
    buffer_resources_[logical_name] = resource;
    render_diagnostics::add_gpu_buffer_create_count();
    out_error.clear();
    return true;
}

SDL_GPUBuffer* GpuSceneRenderer::find_buffer_resource(const std::string& logical_name) const {
    const auto it = buffer_resources_.find(logical_name);
    return (it != buffer_resources_.end()) ? it->second.buffer : nullptr;
}

void GpuSceneRenderer::release_runtime_resources() {
    if (!device_ || !device_->gpu_device()) {
        texture_resources_.clear();
        buffer_resources_.clear();
        return;
    }
    for (auto& entry : texture_resources_) {
        if (entry.second.texture) {
            SDL_ReleaseGPUTexture(device_->gpu_device(), entry.second.texture);
            entry.second.texture = nullptr;
            render_diagnostics::add_texture_destroy_count();
        }
    }
    for (auto& entry : buffer_resources_) {
        if (entry.second.buffer) {
            SDL_ReleaseGPUBuffer(device_->gpu_device(), entry.second.buffer);
            entry.second.buffer = nullptr;
            render_diagnostics::add_gpu_buffer_destroy_count();
        }
    }
    texture_resources_.clear();
    buffer_resources_.clear();
}
