#include "rendering/render/gpu_scene_renderer.hpp"

#include "rendering/render/render_diagnostics.hpp"
#include "utils/cache_manager.hpp"
#include "utils/log.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cmath>
#include <functional>
#include <limits>
#include <optional>
#include <sstream>
#include <string_view>
#include <utility>
#include <unordered_set>

namespace {

constexpr std::array<const char*, 2> kRequiredGraphicsPipelines = {
    "sprite_textured",
    "layer_blur",
};
constexpr const char* kSpriteBatchVertexVariant = "sprite_batch_vertex";

struct ShaderResourceCounts {
    std::uint32_t samplers = 0;
    std::uint32_t storage_textures = 0;
    std::uint32_t storage_buffers = 0;
    std::uint32_t uniform_buffers = 0;
};

struct GraphicsPipelineShaderSpec {
    const char* vertex_variant = kSpriteBatchVertexVariant;
    const char* fragment_variant = "sprite_textured";
    ShaderResourceCounts vertex_resources{};
    ShaderResourceCounts fragment_resources{};
    bool alpha_blend = false;
};

const GraphicsPipelineShaderSpec* graphics_pipeline_spec_for_name(const std::string& name) {
    static const GraphicsPipelineShaderSpec kSpriteTextured{
        kSpriteBatchVertexVariant,
        "sprite_textured",
        ShaderResourceCounts{0u, 0u, 0u, 1u},
        ShaderResourceCounts{1u, 0u, 0u, 0u},
        true};
    static const GraphicsPipelineShaderSpec kLayerBlur{
        kSpriteBatchVertexVariant,
        "layer_blur",
        ShaderResourceCounts{0u, 0u, 0u, 1u},
        ShaderResourceCounts{1u, 0u, 0u, 0u},
        true};
    if (name == "sprite_textured") {
        return &kSpriteTextured;
    }
    if (name == "layer_blur") {
        return &kLayerBlur;
    }
    return nullptr;
}

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

bool sampler_spec_matches(const GpuSceneRenderer::SamplerResourceSpec& lhs,
                          const GpuSceneRenderer::SamplerResourceSpec& rhs) {
    return lhs.min_filter == rhs.min_filter &&
           lhs.mag_filter == rhs.mag_filter &&
           lhs.mipmap_mode == rhs.mipmap_mode &&
           lhs.address_mode_u == rhs.address_mode_u &&
           lhs.address_mode_v == rhs.address_mode_v &&
           lhs.address_mode_w == rhs.address_mode_w &&
           lhs.mip_lod_bias == rhs.mip_lod_bias &&
           lhs.max_anisotropy == rhs.max_anisotropy &&
           lhs.compare_op == rhs.compare_op &&
           lhs.min_lod == rhs.min_lod &&
           lhs.max_lod == rhs.max_lod &&
           lhs.enable_anisotropy == rhs.enable_anisotropy &&
           lhs.enable_compare == rhs.enable_compare;
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

struct ResolvedGpuSpriteDraw {
    SDL_GPUTexture* source_texture = nullptr;
    std::array<GpuSpriteVertex, 6> vertices{};
    SDL_FColor modulate{1.0f, 1.0f, 1.0f, 1.0f};
};

std::string describe_draw_packet_source(const GpuSpriteDrawPacket& draw);

bool resolve_sprite_draws(GpuSceneRenderer& renderer,
                          const std::vector<GpuSpriteDrawPacket>& source_draws,
                          std::string_view pass_name,
                          std::vector<ResolvedGpuSpriteDraw>& out_draws,
                          std::string& out_error) {
    out_draws.clear();
    out_draws.reserve(source_draws.size());
    out_error.clear();

    for (const GpuSpriteDrawPacket& draw : source_draws) {
        std::string texture_error;
        SDL_GPUTexture* gpu_texture =
            renderer.resolve_gpu_texture_for_sdl_texture(draw.source_texture, texture_error);
        if (!gpu_texture) {
            out_error = "Failed to resolve draw texture for pass '" + std::string(pass_name) + "': " +
                        (texture_error.empty() ? "unknown SDL->GPU texture import failure." : texture_error);
            return false;
        }

        ResolvedGpuSpriteDraw resolved{};
        resolved.source_texture = gpu_texture;
        resolved.vertices = draw.vertices;
        resolved.modulate = draw.modulate;
        out_draws.push_back(resolved);
    }
    return true;
}

struct SpriteBatchVertexUniformData {
    SDL_FColor vertex_uv[6]{};
    SDL_FColor modulate{1.0f, 1.0f, 1.0f, 1.0f};
};

bool render_resolved_sprite_draw_batch(const GpuFrameGraph::ExecuteContext& context,
                                       SDL_GPURenderPass* render_pass,
                                       const std::vector<ResolvedGpuSpriteDraw>& draws,
                                       std::string_view pass_name,
                                       std::string& out_error) {
    out_error.clear();
    if (draws.empty()) {
        return true;
    }
    if (!render_pass) {
        out_error = "Render pass handle was null.";
        return false;
    }
    if (!context.command_buffer) {
        out_error = "Command buffer was null.";
        return false;
    }
    if (!context.resolve_sampler) {
        out_error = "Sampler resolver unavailable.";
        return false;
    }

    SDL_GPUSampler* linear_sampler = context.resolve_sampler("linear_clamp");
    if (!linear_sampler) {
        out_error = "Sampler 'linear_clamp' not available.";
        return false;
    }

    for (const ResolvedGpuSpriteDraw& draw : draws) {
        SDL_GPUTextureSamplerBinding sampler_binding{};
        sampler_binding.texture = draw.source_texture;
        sampler_binding.sampler = linear_sampler;
        SDL_BindGPUFragmentSamplers(render_pass, 0u, &sampler_binding, 1u);

        SpriteBatchVertexUniformData uniform_data{};
        for (std::size_t i = 0; i < draw.vertices.size(); ++i) {
            const GpuSpriteVertex& vertex = draw.vertices[i];
            uniform_data.vertex_uv[i] = SDL_FColor{vertex.clip_x, vertex.clip_y, vertex.uv_x, vertex.uv_y};
        }
        uniform_data.modulate = draw.modulate;
        SDL_PushGPUVertexUniformData(context.command_buffer,
                                     0u,
                                     &uniform_data,
                                     static_cast<Uint32>(sizeof(uniform_data)));

        SDL_DrawGPUPrimitives(render_pass, 6u, 1u, 0u, 0u);
        render_diagnostics::add_draw_call_count();
    }

    (void)pass_name;
    return true;
}

std::function<bool(const GpuFrameGraph::ExecuteContext&, SDL_GPURenderPass*, std::string&)>
make_scene_draw_callback(std::vector<ResolvedGpuSpriteDraw> floor_draws,
                         std::vector<ResolvedGpuSpriteDraw> world_draws,
                         std::string pass_name) {
    return [floor_draws = std::move(floor_draws),
            world_draws = std::move(world_draws),
            pass_name = std::move(pass_name)](
               const GpuFrameGraph::ExecuteContext& context,
               SDL_GPURenderPass* render_pass,
               std::string& out_error) -> bool {
        if (!render_resolved_sprite_draw_batch(context, render_pass, floor_draws, pass_name + ".floor", out_error)) {
            return false;
        }
        if (!render_resolved_sprite_draw_batch(context, render_pass, world_draws, pass_name + ".world", out_error)) {
            return false;
        }
        return true;
    };
}

bool render_sprite_draw_batch(GpuSceneRenderer& renderer,
                              SDL_GPURenderPass* render_pass,
                              const std::vector<GpuSpriteDrawPacket>& draws,
                              const char* pipeline_name,
                              std::uint32_t render_state_key,
                              SDL_GPUTextureFormat color_target_format,
                              std::string_view pass_name,
                              std::string& out_error) {
    out_error.clear();
    if (draws.empty()) {
        return true;
    }
    if (!render_pass) {
        out_error = "Render pass handle was null.";
        return false;
    }
    if (!renderer.device() || !renderer.device()->gpu_device()) {
        out_error = "GPU device unavailable.";
        return false;
    }

    SDL_GPUGraphicsPipeline* pipeline =
        renderer.resolve_graphics_pipeline(pipeline_name, render_state_key, color_target_format);
    if (!pipeline) {
        out_error = "Failed to resolve graphics pipeline '" +
                    std::string(pipeline_name ? pipeline_name : "<null>") + "'.";
        return false;
    }

    SDL_GPUSampler* linear_sampler = renderer.find_sampler_resource("linear_clamp");
    if (!linear_sampler) {
        out_error = "Sampler 'linear_clamp' not available.";
        return false;
    }

    SDL_BindGPUGraphicsPipeline(render_pass, pipeline);
    for (const GpuSpriteDrawPacket& draw : draws) {
        SDL_GPUTexture* gpu_texture = draw.source_gpu_texture
            ? draw.source_gpu_texture
            : renderer.find_gpu_texture_for_sdl_texture(draw.source_texture);
        if (!gpu_texture) {
            out_error = "FATAL missing GPU-backed texture; " + describe_draw_packet_source(draw) +
                        " renderer='GpuSceneRenderer' frame_pass='" + std::string(pass_name) + "'";
            render_diagnostics::add_skipped_texture_count();
            render_diagnostics::set_failed_texture_names(describe_draw_packet_source(draw));
            return false;
        }

        SDL_GPUTextureSamplerBinding sampler_binding{};
        sampler_binding.texture = gpu_texture;
        sampler_binding.sampler = linear_sampler;
        SDL_BindGPUFragmentSamplers(render_pass, 0u, &sampler_binding, 1u);

        SpriteBatchVertexUniformData uniform_data{};
        for (std::size_t i = 0; i < draw.vertices.size(); ++i) {
            const GpuSpriteVertex& vertex = draw.vertices[i];
            uniform_data.vertex_uv[i] = SDL_FColor{vertex.clip_x, vertex.clip_y, vertex.uv_x, vertex.uv_y};
        }
        uniform_data.modulate = draw.modulate;
        SDL_PushGPUVertexUniformData(renderer.device()->frame_state().command_buffer,
                                     0u,
                                     &uniform_data,
                                     static_cast<Uint32>(sizeof(uniform_data)));

        SDL_DrawGPUPrimitives(render_pass, 6u, 1u, 0u, 0u);
        render_diagnostics::add_draw_call_count();
    }

    return true;
}

float to_clip_x(float screen_x, float target_width) {
    if (!std::isfinite(screen_x) || !std::isfinite(target_width) || target_width <= 0.0f) {
        return 0.0f;
    }
    return (screen_x / target_width) * 2.0f - 1.0f;
}

float to_clip_y(float screen_y, float target_height) {
    if (!std::isfinite(screen_y) || !std::isfinite(target_height) || target_height <= 0.0f) {
        return 0.0f;
    }
    return 1.0f - (screen_y / target_height) * 2.0f;
}

void fill_fullscreen_packet_vertices(std::uint32_t target_width,
                                     std::uint32_t target_height,
                                     GpuSpriteDrawPacket& out_packet) {
    const float output_w = static_cast<float>(std::max<std::uint32_t>(1u, target_width));
    const float output_h = static_cast<float>(std::max<std::uint32_t>(1u, target_height));

    const SDL_FPoint tl{0.0f, 0.0f};
    const SDL_FPoint tr{output_w, 0.0f};
    const SDL_FPoint br{output_w, output_h};
    const SDL_FPoint bl{0.0f, output_h};

    auto make_vertex = [output_w, output_h](const SDL_FPoint& point, float u, float v) {
        GpuSpriteVertex vertex{};
        vertex.clip_x = to_clip_x(point.x, output_w);
        vertex.clip_y = to_clip_y(point.y, output_h);
        vertex.uv_x = u;
        vertex.uv_y = v;
        return vertex;
    };

    out_packet.vertices[0] = make_vertex(tl, 0.0f, 0.0f);
    out_packet.vertices[1] = make_vertex(tr, 1.0f, 0.0f);
    out_packet.vertices[2] = make_vertex(br, 1.0f, 1.0f);
    out_packet.vertices[3] = make_vertex(tl, 0.0f, 0.0f);
    out_packet.vertices[4] = make_vertex(br, 1.0f, 1.0f);
    out_packet.vertices[5] = make_vertex(bl, 0.0f, 1.0f);
}

GpuSpriteDrawPacket make_fullscreen_draw_packet(SDL_GPUTexture* texture,
                                                std::uint32_t target_width,
                                                std::uint32_t target_height,
                                                const SDL_FColor& modulate = SDL_FColor{1.0f, 1.0f, 1.0f, 1.0f}) {
    GpuSpriteDrawPacket packet{};
    packet.source_texture = nullptr;
    packet.source_gpu_texture = texture;
    packet.modulate = modulate;
    packet.is_floor_packet = false;
    packet.depth_layer = 0;
    fill_fullscreen_packet_vertices(target_width, target_height, packet);
    return packet;
}

std::string describe_draw_packet_source(const GpuSpriteDrawPacket& draw) {
    return "asset='" + draw.source_asset_name +
           "' animation='" + (draw.source_animation_name.empty()
               ? std::string{"<none>"}
               : draw.source_animation_name) +
           "' frame=" + std::to_string(draw.source_frame_index) +
           " variant=" + std::to_string(draw.source_variant_index) +
           " texture_id='" + draw.source_texture_id +
           "' floor=" + std::string(draw.is_floor_packet ? "true" : "false") +
           " depth_layer=" + std::to_string(draw.depth_layer) +
           " stable_sort_id=" + std::to_string(static_cast<std::uint64_t>(draw.stable_sort_id));
}

std::string runtime_floor_target_name() {
    return "runtime.scene.floor";
}

std::string runtime_layer_target_name(int depth_layer) {
    return "runtime.scene.layer." + std::to_string(depth_layer);
}

std::string runtime_processed_layer_target_name(int depth_layer) {
    return "runtime.scene.processed_layer." + std::to_string(depth_layer);
}

std::string runtime_blur_target_a_name() {
    return "runtime.scene.blur.a";
}

std::string runtime_blur_target_b_name() {
    return "runtime.scene.blur.b";
}

SDL_FColor blur_modulate(float direction_x, float direction_y, float blur_strength_px) {
    return SDL_FColor{direction_x, direction_y, blur_strength_px, 1.0f};
}

struct RenderTargetPassContext {
    SDL_GPUCommandBuffer* command_buffer = nullptr;
    SDL_GPUTexture* target_texture = nullptr;
    bool cycle_target = true;
};

bool begin_target_render_pass(const RenderTargetPassContext& context,
                              const SDL_FColor& clear_color,
                              SDL_GPURenderPass*& out_render_pass) {
    out_render_pass = nullptr;
    if (!context.command_buffer) {
        return false;
    }
    if (!context.target_texture) {
        return false;
    }

    SDL_GPUColorTargetInfo target_info{};
    target_info.texture = context.target_texture;
    target_info.mip_level = 0;
    target_info.layer_or_depth_plane = 0;
    target_info.clear_color = clear_color;
    target_info.load_op = SDL_GPU_LOADOP_CLEAR;
    target_info.store_op = SDL_GPU_STOREOP_STORE;
    target_info.resolve_texture = nullptr;
    target_info.resolve_mip_level = 0;
    target_info.resolve_layer = 0;
    target_info.cycle = context.cycle_target && target_info.load_op != SDL_GPU_LOADOP_LOAD;
    target_info.cycle_resolve_texture = false;

    out_render_pass = SDL_BeginGPURenderPass(context.command_buffer, &target_info, 1, nullptr);
    return out_render_pass != nullptr;
}

bool render_packet_batch_to_target(GpuSceneRenderer& renderer,
                                   SDL_GPUCommandBuffer* command_buffer,
                                   SDL_GPUTexture* target_texture,
                                   const std::vector<GpuSpriteDrawPacket>& packets,
                                   const char* pipeline_name,
                                   std::uint32_t render_state_key,
                                   const SDL_FColor& clear_color,
                                   std::string_view pass_name,
                                   std::string& out_error,
                                   bool cycle_target = true,
                                   SDL_GPUTextureFormat color_target_format = SDL_GPU_TEXTUREFORMAT_INVALID) {
    out_error.clear();
    if (!command_buffer) {
        out_error = "Command buffer was null.";
        return false;
    }
    if (!target_texture) {
        out_error = "Target texture was null.";
        return false;
    }

    SDL_GPURenderPass* render_pass = nullptr;
    if (!begin_target_render_pass(RenderTargetPassContext{command_buffer, target_texture, cycle_target},
                                  clear_color,
                                  render_pass)) {
        out_error = "SDL_BeginGPURenderPass failed for pass '" + std::string(pass_name) +
                    "': " + SDL_GetError();
        return false;
    }
    render_diagnostics::add_render_pass();

    const bool rendered =
        render_sprite_draw_batch(renderer,
                                 render_pass,
                                 packets,
                                 pipeline_name,
                                 render_state_key,
                                 color_target_format,
                                 pass_name,
                                 out_error);
    SDL_EndGPURenderPass(render_pass);
    return rendered;
}

struct ActiveDepthLayerRuntimeState {
    int depth_layer = 0;
    float blur_strength_px = 0.0f;
    SDL_GPUTexture* layer_target = nullptr;
    SDL_GPUTexture* processed_texture = nullptr;
    std::size_t packet_count = 0;
};

bool render_depth_layer_blur_chain(GpuSceneRenderer& renderer,
                                   SDL_GPUCommandBuffer* command_buffer,
                                   SDL_GPUTexture* source_texture,
                                   SDL_GPUTexture* resolve_texture,
                                   SDL_GPUTexture* blur_target_a,
                                   SDL_GPUTexture* blur_target_b,
                                   std::uint32_t target_width,
                                   std::uint32_t target_height,
                                   float blur_strength_px,
                                   int depth_layer,
                                   std::string& out_error) {
    out_error.clear();
    if (blur_strength_px <= 0.0f) {
        return true;
    }
    if (!source_texture || !resolve_texture || !blur_target_a || !blur_target_b) {
        out_error = "Blur target resources were unavailable for depth layer " + std::to_string(depth_layer) + ".";
        return false;
    }

    std::vector<GpuSpriteDrawPacket> horizontal_packets{};
    horizontal_packets.push_back(make_fullscreen_draw_packet(
        source_texture,
        target_width,
        target_height,
        blur_modulate(1.0f, 0.0f, blur_strength_px)));
    if (!render_packet_batch_to_target(renderer,
                                       command_buffer,
                                       blur_target_a,
                                       horizontal_packets,
                                       "layer_blur",
                                       0x2201u,
                                       SDL_FColor{0.0f, 0.0f, 0.0f, 0.0f},
                                       "runtime_scene.blur.horizontal",
                                       out_error)) {
        return false;
    }

    std::vector<GpuSpriteDrawPacket> vertical_packets{};
    vertical_packets.push_back(make_fullscreen_draw_packet(
        blur_target_a,
        target_width,
        target_height,
        blur_modulate(0.0f, 1.0f, blur_strength_px)));
    if (!render_packet_batch_to_target(renderer,
                                       command_buffer,
                                       blur_target_b,
                                       vertical_packets,
                                       "layer_blur",
                                       0x2201u,
                                       SDL_FColor{0.0f, 0.0f, 0.0f, 0.0f},
                                       "runtime_scene.blur.vertical",
                                       out_error)) {
        return false;
    }

    std::vector<GpuSpriteDrawPacket> copy_packets{};
    copy_packets.push_back(make_fullscreen_draw_packet(blur_target_b,
                                                       target_width,
                                                       target_height));
    if (!render_packet_batch_to_target(renderer,
                                       command_buffer,
                                       resolve_texture,
                                       copy_packets,
                                       "sprite_textured",
                                       0x2200u,
                                       SDL_FColor{0.0f, 0.0f, 0.0f, 0.0f},
                                       "runtime_scene.blur.resolve",
                                       out_error)) {
        return false;
    }

    return true;
}

std::string join_packets_per_layer_summary(const std::vector<GpuDepthLayerDrawPackets>& depth_layers) {
    std::ostringstream stream;
    bool first = true;
    for (const GpuDepthLayerDrawPackets& layer : depth_layers) {
        if (!first) {
            stream << ", ";
        }
        first = false;
        stream << layer.depth_layer << '=' << layer.packets.size();
    }
    return stream.str();
}

std::string join_blur_strength_summary(const std::vector<GpuDepthLayerDrawPackets>& depth_layers) {
    std::ostringstream stream;
    bool first = true;
    for (const GpuDepthLayerDrawPackets& layer : depth_layers) {
        if (!first) {
            stream << ", ";
        }
        first = false;
        stream << layer.depth_layer << '=' << layer.blur_strength_px;
    }
    return stream.str();
}

std::string join_composite_summary(const std::vector<ActiveDepthLayerRuntimeState>& depth_layers,
                                   bool includes_ui_overlay) {
    std::ostringstream stream;
    stream << "floor";
    for (const ActiveDepthLayerRuntimeState& layer : depth_layers) {
        stream << " -> " << layer.depth_layer;
    }
    if (includes_ui_overlay) {
        stream << " -> ui";
    }
    return stream.str();
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
                                                       std::uint32_t render_state_key,
                                                       SDL_GPUTextureFormat color_target_format) const {
    ShaderPipelineKey key{};
    key.shader_id = shader_name;
    key.variant = backend_shader_variant_;
    key.color_format = color_target_format != SDL_GPU_TEXTUREFORMAT_INVALID
        ? color_target_format
        : (device_ ? device_->format_policy().albedo_format : SDL_GPU_TEXTUREFORMAT_INVALID);
    // Runtime frame graph uses a dedicated present state key for the swapchain pass.
    // Ensure that pipeline targets the real swapchain format when available.
    if (device_ &&
        (render_state_key == 0x1006u || render_state_key == 0x2005u || render_state_key == 0x2100u) &&
        device_->swapchain_format() != SDL_GPU_TEXTUREFORMAT_INVALID) {
        key.color_format = device_->swapchain_format();
    }
    key.depth_format = device_ ? device_->format_policy().depth_format : SDL_GPU_TEXTUREFORMAT_INVALID;
    // Runtime scene resources are single-sample; keep pipeline sample count aligned.
    key.sample_count = SDL_GPU_SAMPLECOUNT_1;
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
                                               std::uint32_t num_samplers,
                                               std::uint32_t num_storage_textures,
                                               std::uint32_t num_storage_buffers,
                                               std::uint32_t num_uniform_buffers,
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
    create_info.num_samplers = num_samplers;
    create_info.num_storage_textures = num_storage_textures;
    create_info.num_storage_buffers = num_storage_buffers;
    create_info.num_uniform_buffers = num_uniform_buffers;
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
                                                                    SDL_GPUTextureFormat color_target_format,
                                                                    std::string& out_error) const {
    if (!device_ || !device_->gpu_device()) {
        out_error = "GPU device unavailable while creating graphics pipeline";
        return nullptr;
    }

    const GraphicsPipelineShaderSpec* pipeline_spec = graphics_pipeline_spec_for_name(pipeline_name);
    if (!pipeline_spec) {
        out_error = "Missing graphics pipeline shader spec for pipeline: " + pipeline_name;
        return nullptr;
    }

    const ShaderPackageLibrary::ShaderVariantPath* fragment_variant = shader_packages_.find(pipeline_spec->fragment_variant);
    if (!fragment_variant) {
        out_error = "Missing fragment shader variant: " + std::string(pipeline_spec->fragment_variant);
        return nullptr;
    }
    const ShaderPackageLibrary::ShaderVariantPath* vertex_variant = shader_packages_.find(pipeline_spec->vertex_variant);
    if (!vertex_variant) {
        out_error = "Missing vertex shader variant: " + std::string(pipeline_spec->vertex_variant);
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
    SDL_GPUShader* vertex_shader = create_shader(*vertex,
                                                 SDL_GPU_SHADERSTAGE_VERTEX,
                                                 pipeline_spec->vertex_resources.samplers,
                                                 pipeline_spec->vertex_resources.storage_textures,
                                                 pipeline_spec->vertex_resources.storage_buffers,
                                                 pipeline_spec->vertex_resources.uniform_buffers,
                                                 shader_error);
    if (!vertex_shader) {
        out_error = shader_error;
        return nullptr;
    }
    SDL_GPUShader* fragment_shader = create_shader(*fragment,
                                                   SDL_GPU_SHADERSTAGE_FRAGMENT,
                                                   pipeline_spec->fragment_resources.samplers,
                                                   pipeline_spec->fragment_resources.storage_textures,
                                                   pipeline_spec->fragment_resources.storage_buffers,
                                                   pipeline_spec->fragment_resources.uniform_buffers,
                                                   shader_error);
    if (!fragment_shader) {
        SDL_ReleaseGPUShader(device_->gpu_device(), vertex_shader);
        out_error = shader_error;
        return nullptr;
    }

    SDL_GPUColorTargetBlendState blend_state{};
    blend_state.src_color_blendfactor = pipeline_spec->alpha_blend
        ? SDL_GPU_BLENDFACTOR_SRC_ALPHA
        : SDL_GPU_BLENDFACTOR_ONE;
    blend_state.dst_color_blendfactor = pipeline_spec->alpha_blend
        ? SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA
        : SDL_GPU_BLENDFACTOR_ZERO;
    blend_state.color_blend_op = SDL_GPU_BLENDOP_ADD;
    blend_state.src_alpha_blendfactor = pipeline_spec->alpha_blend
        ? SDL_GPU_BLENDFACTOR_ONE
        : SDL_GPU_BLENDFACTOR_ONE;
    blend_state.dst_alpha_blendfactor = pipeline_spec->alpha_blend
        ? SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA
        : SDL_GPU_BLENDFACTOR_ZERO;
    blend_state.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
    blend_state.color_write_mask =
        SDL_GPU_COLORCOMPONENT_R |
        SDL_GPU_COLORCOMPONENT_G |
        SDL_GPU_COLORCOMPONENT_B |
        SDL_GPU_COLORCOMPONENT_A;
    blend_state.enable_blend = pipeline_spec->alpha_blend;
    blend_state.enable_color_write_mask = true;

    SDL_GPUColorTargetDescription color_target{};
    color_target.format = (color_target_format != SDL_GPU_TEXTUREFORMAT_INVALID)
        ? color_target_format
        : device_->format_policy().albedo_format;
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
    create_info.multisample_state.sample_count = SDL_GPU_SAMPLECOUNT_1;
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

    const auto ensure_vertex_variant = [this, &out_error](const char* variant_name) -> bool {
        const ShaderPackageLibrary::ShaderVariantPath* vertex_variant = shader_packages_.find(variant_name);
        if (!vertex_variant) {
            out_error = std::string("Missing required vertex variant: ") + variant_name;
            return false;
        }
        const ShaderPackageLibrary::ShaderBinaryDescriptor* vertex_desc = select_backend_binary(*vertex_variant);
        if (!vertex_desc) {
            out_error = std::string("Missing backend binary for required vertex variant: ") + variant_name;
            return false;
        }
        if (!stage_matches(vertex_desc->stage, "vertex")) {
            out_error = "Required vertex variant has invalid stage metadata: " + vertex_desc->stage;
            return false;
        }
        return true;
    };

    if (!ensure_vertex_variant(kSpriteBatchVertexVariant)) {
        return false;
    }

    std::string pipeline_error;
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
            [&]() { return create_graphics_pipeline(pipeline_name, key.color_format, pipeline_error); });
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

bool GpuSceneRenderer::render_active_frame(const GpuSceneFrameData& frame_data, std::string& out_error) {
    out_error.clear();
    if (!device_ || !device_->gpu_device()) {
        out_error = "GPU device unavailable while rendering scene frame";
        return false;
    }

    const GpuRenderDevice::FrameState& frame_state = device_->frame_state();
    if (!frame_state.command_buffer) {
        out_error = "GPU command buffer unavailable while rendering scene frame";
        return false;
    }
    if (!frame_state.swapchain_texture || frame_state.swapchain_width == 0 || frame_state.swapchain_height == 0) {
        out_error = "GPU swapchain texture unavailable while rendering scene frame";
        return false;
    }

    const RuntimeGpuFormatPolicy& format_policy = device_->format_policy();
    if (format_policy.albedo_format == SDL_GPU_TEXTUREFORMAT_INVALID) {
        out_error = "GPU albedo format is invalid";
        return false;
    }

    GpuSceneRenderer::SamplerResourceSpec sampler_spec{};
    if (!ensure_sampler_resource("linear_clamp", sampler_spec, out_error)) {
        return false;
    }

    const std::uint32_t target_width = frame_state.swapchain_width;
    const std::uint32_t target_height = frame_state.swapchain_height;
    const SDL_FColor clear_color{0.0f, 0.0f, 0.0f, 0.0f};
    const GpuSceneRenderer::TextureResourceSpec layer_spec{
        target_width,
        target_height,
        format_policy.albedo_format,
        static_cast<SDL_GPUTextureUsageFlags>(SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER),
        1u,
        1u,
        SDL_GPU_SAMPLECOUNT_1,
    };

    render_diagnostics::set_floor_target_dimensions(target_width, target_height);
    render_diagnostics::set_packet_counts(frame_data.floor_draw_count, frame_data.layer_sprite_draw_count);
    render_diagnostics::set_active_depth_layer_count(frame_data.active_depth_layer_count);
    render_diagnostics::set_packets_per_depth_layer(join_packets_per_layer_summary(frame_data.depth_layers));
    render_diagnostics::set_blur_strength_per_layer(join_blur_strength_summary(frame_data.depth_layers));
    render_diagnostics::set_clear_executed(true);
    const bool debug_passes = std::getenv("VIBBLE_GPU_RENDER_DEBUG") != nullptr;
    const auto debug_log = [debug_passes](const std::string& message) {
        if (debug_passes) {
            vibble::log::info("[GpuSceneRenderer] " + message);
        }
    };

    debug_log("ensure floor target");
    if (!ensure_texture_resource(runtime_floor_target_name(), layer_spec, out_error)) {
        return false;
    }
    SDL_GPUTexture* floor_target = find_texture_resource(runtime_floor_target_name());
    if (!floor_target) {
        out_error = "Floor render target was unavailable.";
        return false;
    }

    std::vector<ActiveDepthLayerRuntimeState> active_layers{};
    active_layers.reserve(frame_data.depth_layers.size());
    std::unordered_set<std::string> retained_layer_targets{};
    retained_layer_targets.reserve(frame_data.depth_layers.size());
    std::unordered_set<std::string> retained_processed_layer_targets{};
    retained_processed_layer_targets.reserve(frame_data.depth_layers.size());

    bool blur_chain_required = false;
    for (const GpuDepthLayerDrawPackets& depth_layer : frame_data.depth_layers) {
        const std::string target_name = runtime_layer_target_name(depth_layer.depth_layer);
        retained_layer_targets.insert(target_name);
        if (!ensure_texture_resource(target_name, layer_spec, out_error)) {
            return false;
        }
        SDL_GPUTexture* layer_target = find_texture_resource(target_name);
        if (!layer_target) {
            out_error = "Depth-layer render target was unavailable for layer " +
                        std::to_string(depth_layer.depth_layer) + ".";
            return false;
        }

        ActiveDepthLayerRuntimeState state{};
        state.depth_layer = depth_layer.depth_layer;
        state.blur_strength_px = depth_layer.blur_strength_px;
        state.layer_target = layer_target;
        state.processed_texture = layer_target;
        state.packet_count = depth_layer.packets.size();
        active_layers.push_back(state);
        blur_chain_required = blur_chain_required || (depth_layer.blur_strength_px > 0.0f);
    }

    const std::string blur_target_a_name = runtime_blur_target_a_name();
    const std::string blur_target_b_name = runtime_blur_target_b_name();
    if (blur_chain_required) {
        debug_log("ensure blur targets");
        if (!ensure_texture_resource(blur_target_a_name, layer_spec, out_error)) {
            return false;
        }
        if (!ensure_texture_resource(blur_target_b_name, layer_spec, out_error)) {
            return false;
        }
    } else {
        (void)release_texture_resource(blur_target_a_name);
        (void)release_texture_resource(blur_target_b_name);
    }

    debug_log("render floor pass");
    if (!render_packet_batch_to_target(*this,
                                       frame_state.command_buffer,
                                       floor_target,
                                       frame_data.floor_draws,
                                       "sprite_textured",
                                       0x2200u,
                                       clear_color,
                                       "runtime_scene.floor",
                                       out_error)) {
        return false;
    }
    debug_log("floor pass complete");

    std::uint32_t blur_pass_count = 0;
    for (std::size_t i = 0; i < active_layers.size(); ++i) {
        const GpuDepthLayerDrawPackets& source_layer = frame_data.depth_layers[i];
        ActiveDepthLayerRuntimeState& state = active_layers[i];

        debug_log("render layer pass " + std::to_string(state.depth_layer));
        if (!render_packet_batch_to_target(*this,
                                           frame_state.command_buffer,
                                           state.layer_target,
                                           source_layer.packets,
                                           "sprite_textured",
                                           0x2200u,
                                           clear_color,
                                           "runtime_scene.layer." + std::to_string(state.depth_layer),
                                           out_error)) {
            return false;
        }
        debug_log("layer pass complete " + std::to_string(state.depth_layer));

        if (state.blur_strength_px > 0.0f && state.depth_layer != 0) {
            SDL_GPUTexture* blur_a = find_texture_resource(blur_target_a_name);
            SDL_GPUTexture* blur_b = find_texture_resource(blur_target_b_name);
            const std::string processed_target_name = runtime_processed_layer_target_name(state.depth_layer);
            retained_processed_layer_targets.insert(processed_target_name);
            if (!ensure_texture_resource(processed_target_name, layer_spec, out_error)) {
                return false;
            }
            SDL_GPUTexture* processed_target = find_texture_resource(processed_target_name);
            if (!processed_target) {
                out_error = "Processed depth-layer target was unavailable for layer " +
                            std::to_string(state.depth_layer) + ".";
                return false;
            }
            debug_log("render blur chain " + std::to_string(state.depth_layer));
            if (!render_depth_layer_blur_chain(*this,
                                               frame_state.command_buffer,
                                               state.layer_target,
                                               processed_target,
                                               blur_a,
                                               blur_b,
                                               target_width,
                                               target_height,
                                               state.blur_strength_px,
                                               state.depth_layer,
                                               out_error)) {
                return false;
            }
            state.processed_texture = processed_target;
            debug_log("blur chain complete " + std::to_string(state.depth_layer));
            state.blur_strength_px = std::max(0.0f, state.blur_strength_px);
            blur_pass_count += 3;
        }
    }

    render_diagnostics::set_blur_pass_count(blur_pass_count);
    release_texture_resources_with_prefix("runtime.scene.layer.", retained_layer_targets);
    release_texture_resources_with_prefix("runtime.scene.processed_layer.", retained_processed_layer_targets);

    const bool has_ui_overlay = frame_data.ui_overlay_gpu_texture != nullptr;
    std::vector<GpuSpriteDrawPacket> composite_packets{};
    composite_packets.reserve(active_layers.size() + (has_ui_overlay ? 2u : 1u));
    composite_packets.push_back(make_fullscreen_draw_packet(floor_target, target_width, target_height));
    for (const ActiveDepthLayerRuntimeState& state : active_layers) {
        if (!state.processed_texture) {
            out_error = "Processed depth-layer texture was unavailable for layer " +
                        std::to_string(state.depth_layer) + ".";
            return false;
        }
        composite_packets.push_back(make_fullscreen_draw_packet(state.processed_texture, target_width, target_height));
    }
    if (has_ui_overlay) {
        composite_packets.push_back(make_fullscreen_draw_packet(frame_data.ui_overlay_gpu_texture,
                                                                target_width,
                                                                target_height));
    }

    render_diagnostics::set_composite_layers_submitted(join_composite_summary(active_layers, has_ui_overlay));

    debug_log("render present pass");
    if (!render_packet_batch_to_target(*this,
                                       frame_state.command_buffer,
                                       frame_state.swapchain_texture,
                                       composite_packets,
                                       "sprite_textured",
                                       0x2100u,
                                       clear_color,
                                       "runtime_scene.present",
                                       out_error,
                                       false,
                                       device_->swapchain_format())) {
        return false;
    }
    debug_log("present pass complete");

    return true;
}

bool GpuSceneRenderer::render_frame(const GpuSceneFrameData& frame_data, std::string& out_error) {
    out_error.clear();
    if (!device_ || !device_->gpu_device()) {
        out_error = "GPU device unavailable while rendering scene frame";
        return false;
    }

    const std::uint64_t pipeline_hits_before = pipeline_cache_.total_hits();
    const std::uint64_t pipeline_misses_before = pipeline_cache_.total_misses();

    std::string frame_error;
    if (!begin_frame(&frame_error)) {
        out_error = frame_error.empty() ? "GpuSceneRenderer::begin_frame failed." : frame_error;
        return false;
    }

    const GpuRenderDevice::FrameState& frame_state = device_->frame_state();
    render_diagnostics::set_command_buffer_acquired(frame_state.command_buffer != nullptr);
    render_diagnostics::set_swapchain_acquired(frame_state.swapchain_texture != nullptr);
    render_diagnostics::set_swapchain_dimensions(frame_state.swapchain_width, frame_state.swapchain_height);

    if (!render_active_frame(frame_data, out_error)) {
        if (!device_->end_frame(false, frame_error) && !frame_error.empty()) {
            vibble::log::warn("[GpuSceneRenderer] Failed to cancel GPU frame cleanly: " + frame_error);
        }
        render_diagnostics::set_submit_result(false);
        return false;
    }

    if (std::getenv("VIBBLE_GPU_RENDER_DEBUG")) {
        vibble::log::info("[GpuSceneRenderer] submitting frame");
    }
    if (!device_->end_frame(true, frame_error)) {
        render_diagnostics::set_submit_result(false);
        out_error = "Failed to submit GPU frame: " + frame_error;
        return false;
    }
    if (std::getenv("VIBBLE_GPU_RENDER_DEBUG")) {
        vibble::log::info("[GpuSceneRenderer] submit complete");
    }
    render_diagnostics::set_submit_result(true);

    const std::uint64_t pipeline_hits_after = pipeline_cache_.total_hits();
    const std::uint64_t pipeline_misses_after = pipeline_cache_.total_misses();
    const std::uint64_t frame_hits = (pipeline_hits_after >= pipeline_hits_before)
        ? (pipeline_hits_after - pipeline_hits_before)
        : pipeline_hits_after;
    const std::uint64_t frame_misses = (pipeline_misses_after >= pipeline_misses_before)
        ? (pipeline_misses_after - pipeline_misses_before)
        : pipeline_misses_after;
    const double frame_hit_rate = (frame_hits + frame_misses) == 0
        ? 1.0
        : static_cast<double>(frame_hits) / static_cast<double>(frame_hits + frame_misses);
    render_diagnostics::set_gpu_pipeline_cache_stats(frame_hits, frame_misses, frame_hit_rate);

    return true;
}

SDL_GPUGraphicsPipeline* GpuSceneRenderer::get_graphics_pipeline(const std::string& pipeline_name,
                                                                 std::uint32_t render_state_key,
                                                                 SDL_GPUTextureFormat color_target_format) {
    std::string pipeline_error;
    const ShaderPipelineKey key = make_pipeline_key(pipeline_name, render_state_key, color_target_format);
    SDL_GPUGraphicsPipeline* pipeline = pipeline_cache_.get_or_create_graphics_pipeline(
        key,
        [&]() { return create_graphics_pipeline(pipeline_name, key.color_format, pipeline_error); });
    if (!pipeline && !pipeline_error.empty()) {
        vibble::log::error("[GpuSceneRenderer] Graphics pipeline resolve failed for '" +
                           pipeline_name + "': " + pipeline_error);
    }
    return pipeline;
}

SDL_GPUGraphicsPipeline* GpuSceneRenderer::resolve_graphics_pipeline(const std::string& pipeline_name,
                                                                     std::uint32_t render_state_key,
                                                                     SDL_GPUTextureFormat color_target_format) {
    return get_graphics_pipeline(pipeline_name, render_state_key, color_target_format);
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

void GpuSceneRenderer::reset_frame_graph() {
    frame_graph_.reset();
}

bool GpuSceneRenderer::begin_frame(std::string* out_error, bool reset_frame_graph_before_begin) {
    if (reset_frame_graph_before_begin) {
        frame_graph_.reset();
    }
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
    execute_context.swapchain_format = device_ ? device_->swapchain_format() : SDL_GPU_TEXTUREFORMAT_INVALID;
    execute_context.swapchain_width = frame_state.swapchain_width;
    execute_context.swapchain_height = frame_state.swapchain_height;
    execute_context.resolve_texture = [this](const std::string& name) {
        return find_texture_resource(name);
    };
    execute_context.resolve_sampler = [this](const std::string& name) {
        return find_sampler_resource(name);
    };
    execute_context.resolve_buffer = [this](const std::string& name) {
        return find_buffer_resource(name);
    };
    execute_context.resolve_graphics_pipeline =
        [this](const std::string& name, std::uint32_t state_key, SDL_GPUTextureFormat color_target_format) {
            return get_graphics_pipeline(name, state_key, color_target_format);
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
    vibble::log::debug("[GpuSceneRenderer] Scene frame executed: render=" +
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

void GpuSceneRenderer::abort_frame() {
    frame_graph_.reset();
    if (!device_) {
        return;
    }
    std::string cancel_error;
    if (!device_->end_frame(false, cancel_error) && !cancel_error.empty()) {
        vibble::log::warn("[GpuSceneRenderer] Failed to abort GPU frame cleanly: " + cancel_error);
    }
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

bool GpuSceneRenderer::register_external_texture_resource(const std::string& logical_name, SDL_GPUTexture* texture) {
    if (logical_name.empty() || !texture) {
        return false;
    }
    external_texture_resources_[logical_name] = texture;
    return true;
}

void GpuSceneRenderer::clear_external_texture_resources() {
    external_texture_resources_.clear();
}

bool GpuSceneRenderer::release_texture_resource(const std::string& logical_name) {
    if (!device_ || !device_->gpu_device() || logical_name.empty()) {
        return false;
    }
    const auto it = texture_resources_.find(logical_name);
    if (it == texture_resources_.end()) {
        return false;
    }
    if (it->second.texture) {
        SDL_ReleaseGPUTexture(device_->gpu_device(), it->second.texture);
        it->second.texture = nullptr;
        render_diagnostics::add_texture_destroy_count();
    }
    texture_resources_.erase(it);
    return true;
}

void GpuSceneRenderer::release_texture_resources_with_prefix(
    const std::string& prefix,
    const std::unordered_set<std::string>& retained_logical_names) {
    if (!device_ || !device_->gpu_device() || prefix.empty()) {
        return;
    }

    for (auto it = texture_resources_.begin(); it != texture_resources_.end();) {
        const bool matches_prefix = it->first.rfind(prefix, 0) == 0;
        const bool retained = retained_logical_names.find(it->first) != retained_logical_names.end();
        if (matches_prefix && !retained) {
            if (it->second.texture) {
                SDL_ReleaseGPUTexture(device_->gpu_device(), it->second.texture);
                it->second.texture = nullptr;
                render_diagnostics::add_texture_destroy_count();
            }
            it = texture_resources_.erase(it);
            continue;
        }
        ++it;
    }
}

SDL_GPUTexture* GpuSceneRenderer::find_texture_resource(const std::string& logical_name) const {
    const auto it = texture_resources_.find(logical_name);
    if (it != texture_resources_.end()) {
        return it->second.texture;
    }
    const auto external_it = external_texture_resources_.find(logical_name);
    return (external_it != external_texture_resources_.end()) ? external_it->second : nullptr;
}

SDL_GPUTexture* GpuSceneRenderer::find_gpu_texture_for_sdl_texture(SDL_Texture* texture) const {
    if (!texture) {
        return nullptr;
    }
    const auto it = imported_sdl_texture_resources_.find(texture);
    if (it == imported_sdl_texture_resources_.end()) {
        return nullptr;
    }
    return it->second.gpu_texture;
}

SDL_GPUTexture* GpuSceneRenderer::resolve_gpu_texture_for_sdl_texture(SDL_Texture* texture, std::string& out_error) {
    out_error.clear();
    if (!texture) {
        out_error = "Cannot resolve GPU texture from null SDL texture.";
        return nullptr;
    }
    if (!device_ || !device_->gpu_device()) {
        out_error = "GPU device unavailable while resolving SDL texture.";
        return nullptr;
    }

    const SDL_PropertiesID texture_props = SDL_GetTextureProperties(texture);
    if (!texture_props) {
        out_error = "SDL texture properties unavailable while resolving GPU texture bridge.";
        return nullptr;
    }

    SDL_GPUTexture* bridged_gpu_texture = static_cast<SDL_GPUTexture*>(
        SDL_GetPointerProperty(texture_props, SDL_PROP_TEXTURE_GPU_TEXTURE_POINTER, nullptr));
    bool owns_imported_gpu_texture = false;
    if (!bridged_gpu_texture) {
        const CacheManager::PreparedGpuTextureUpload* prepared =
            CacheManager::prepared_gpu_upload_for_texture(texture);
        if (!prepared) {
            out_error = "SDL texture does not expose SDL_PROP_TEXTURE_GPU_TEXTURE_POINTER and has no prepared GPU upload payload; readback fallback is disabled.";
            return nullptr;
        }

        const auto cached_it = imported_sdl_texture_resources_.find(texture);
        if (cached_it != imported_sdl_texture_resources_.end() &&
            cached_it->second.gpu_texture &&
            cached_it->second.owns_gpu_texture &&
            cached_it->second.width == static_cast<Uint32>(std::max(1, prepared->width)) &&
            cached_it->second.height == static_cast<Uint32>(std::max(1, prepared->height))) {
            return cached_it->second.gpu_texture;
        }

        std::string upload_error;
        bridged_gpu_texture = CacheManager::upload_prepared_texture_to_gpu(device_->gpu_device(),
                                                                           *prepared,
                                                                           upload_error);
        if (!bridged_gpu_texture) {
            out_error = "Prepared GPU texture upload failed: " + upload_error;
            return nullptr;
        }
        owns_imported_gpu_texture = true;
        vibble::log::info("[GpuSceneRenderer] Created backend GPU texture from prepared asset payload "
                          "for SDL texture without renderer bridge.");
    }

    float src_wf = 0.0f;
    float src_hf = 0.0f;
    if (!SDL_GetTextureSize(texture, &src_wf, &src_hf)) {
        out_error = "SDL_GetTextureSize failed for texture bridge resolve: " + std::string(SDL_GetError());
        if (owns_imported_gpu_texture && bridged_gpu_texture) {
            SDL_ReleaseGPUTexture(device_->gpu_device(), bridged_gpu_texture);
        }
        return nullptr;
    }
    const Uint32 src_w = static_cast<Uint32>(std::max(1, static_cast<int>(std::lround(src_wf))));
    const Uint32 src_h = static_cast<Uint32>(std::max(1, static_cast<int>(std::lround(src_hf))));
    const std::uintptr_t revision = reinterpret_cast<std::uintptr_t>(bridged_gpu_texture);

    const auto cached_it = imported_sdl_texture_resources_.find(texture);
    if (cached_it != imported_sdl_texture_resources_.end()) {
        const ImportedSdlTextureResource& cached = cached_it->second;
        if (cached.gpu_texture == bridged_gpu_texture &&
            cached.revision == revision &&
            cached.width == src_w &&
            cached.height == src_h) {
            return cached.gpu_texture;
        }
        if (cached.gpu_texture && cached.owns_gpu_texture &&
            cached.gpu_texture != bridged_gpu_texture) {
            SDL_ReleaseGPUTexture(device_->gpu_device(), cached.gpu_texture);
            render_diagnostics::add_texture_destroy_count();
        }
    }

    ImportedSdlTextureResource imported{};
    imported.source_texture = texture;
    imported.gpu_texture = bridged_gpu_texture;
    imported.owns_gpu_texture = owns_imported_gpu_texture;
    imported.revision = revision;
    imported.width = src_w;
    imported.height = src_h;
    imported_sdl_texture_resources_[texture] = imported;
    if (owns_imported_gpu_texture) {
        render_diagnostics::add_texture_create_count();
    }
    return bridged_gpu_texture;
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

bool GpuSceneRenderer::ensure_sampler_resource(const std::string& logical_name,
                                               const SamplerResourceSpec& spec,
                                               std::string& out_error) {
    if (!device_ || !device_->gpu_device()) {
        out_error = "GPU device unavailable while creating sampler resource '" + logical_name + "'";
        return false;
    }
    if (logical_name.empty()) {
        out_error = "Sampler resource name cannot be empty";
        return false;
    }

    const auto it = sampler_resources_.find(logical_name);
    if (it != sampler_resources_.end() &&
        it->second.sampler &&
        sampler_spec_matches(it->second.spec, spec)) {
        out_error.clear();
        return true;
    }

    if (it != sampler_resources_.end() && it->second.sampler) {
        SDL_ReleaseGPUSampler(device_->gpu_device(), it->second.sampler);
        it->second.sampler = nullptr;
    }

    SDL_GPUSamplerCreateInfo create_info{};
    create_info.min_filter = spec.min_filter;
    create_info.mag_filter = spec.mag_filter;
    create_info.mipmap_mode = spec.mipmap_mode;
    create_info.address_mode_u = spec.address_mode_u;
    create_info.address_mode_v = spec.address_mode_v;
    create_info.address_mode_w = spec.address_mode_w;
    create_info.mip_lod_bias = spec.mip_lod_bias;
    create_info.max_anisotropy = spec.max_anisotropy;
    create_info.compare_op = spec.compare_op;
    create_info.min_lod = spec.min_lod;
    create_info.max_lod = spec.max_lod;
    create_info.enable_anisotropy = spec.enable_anisotropy;
    create_info.enable_compare = spec.enable_compare;
    create_info.props = 0;

    SDL_GPUSampler* sampler = SDL_CreateGPUSampler(device_->gpu_device(), &create_info);
    if (!sampler) {
        out_error = "SDL_CreateGPUSampler failed for '" + logical_name + "': " + SDL_GetError();
        return false;
    }

    RuntimeSamplerResource resource{};
    resource.sampler = sampler;
    resource.spec = spec;
    sampler_resources_[logical_name] = resource;
    out_error.clear();
    return true;
}

SDL_GPUSampler* GpuSceneRenderer::find_sampler_resource(const std::string& logical_name) const {
    const auto it = sampler_resources_.find(logical_name);
    return (it != sampler_resources_.end()) ? it->second.sampler : nullptr;
}

void GpuSceneRenderer::release_runtime_resources() {
    if (!device_ || !device_->gpu_device()) {
        texture_resources_.clear();
        external_texture_resources_.clear();
        imported_sdl_texture_resources_.clear();
        buffer_resources_.clear();
        sampler_resources_.clear();
        return;
    }
    SDL_WaitForGPUIdle(device_->gpu_device());
    for (auto& entry : texture_resources_) {
        if (entry.second.texture) {
            SDL_ReleaseGPUTexture(device_->gpu_device(), entry.second.texture);
            entry.second.texture = nullptr;
            render_diagnostics::add_texture_destroy_count();
        }
    }
    external_texture_resources_.clear();
    for (auto& entry : imported_sdl_texture_resources_) {
        if (entry.second.gpu_texture && entry.second.owns_gpu_texture) {
            SDL_ReleaseGPUTexture(device_->gpu_device(), entry.second.gpu_texture);
            entry.second.gpu_texture = nullptr;
            render_diagnostics::add_texture_destroy_count();
        }
    }
    imported_sdl_texture_resources_.clear();
    for (auto& entry : buffer_resources_) {
        if (entry.second.buffer) {
            SDL_ReleaseGPUBuffer(device_->gpu_device(), entry.second.buffer);
            entry.second.buffer = nullptr;
            render_diagnostics::add_gpu_buffer_destroy_count();
        }
    }
    for (auto& entry : sampler_resources_) {
        if (entry.second.sampler) {
            SDL_ReleaseGPUSampler(device_->gpu_device(), entry.second.sampler);
            entry.second.sampler = nullptr;
        }
    }
    texture_resources_.clear();
    imported_sdl_texture_resources_.clear();
    buffer_resources_.clear();
    sampler_resources_.clear();
}
