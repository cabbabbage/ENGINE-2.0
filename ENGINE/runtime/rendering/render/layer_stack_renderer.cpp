#include "rendering/render/layer_stack_renderer.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <utility>
#include <vector>

#include "rendering/render/render_diagnostics.hpp"
#include "rendering/render/lighting_system_v2.hpp"

namespace {

constexpr std::size_t kMinGpuBufferBytes = 4096;
constexpr std::size_t kUploadAlignmentBytes = 16;

void destroy_texture(SDL_Texture*& texture) {
    render_diagnostics::destroy_texture(texture);
}

SDL_BlendMode safe_layer_blend_mode(SDL_BlendMode blend_mode) {
    if (blend_mode == SDL_BLENDMODE_MOD || blend_mode == SDL_BLENDMODE_MUL) {
        return SDL_BLENDMODE_BLEND;
    }
    return blend_mode;
}

std::size_t align_up(std::size_t value, std::size_t alignment) {
    if (alignment == 0) {
        return value;
    }
    const std::size_t mask = alignment - 1;
    return (value + mask) & ~mask;
}

std::size_t next_capacity(std::size_t current, std::size_t required) {
    std::size_t capacity = std::max<std::size_t>(kMinGpuBufferBytes, current);
    while (capacity < required) {
        if (capacity > (std::numeric_limits<std::size_t>::max() / 2u)) {
            return required;
        }
        capacity *= 2u;
    }
    return capacity;
}

std::uint32_t pack_rgba(SDL_Color color) {
    return static_cast<std::uint32_t>(color.r) |
           (static_cast<std::uint32_t>(color.g) << 8u) |
           (static_cast<std::uint32_t>(color.b) << 16u) |
           (static_cast<std::uint32_t>(color.a) << 24u);
}

int clamp_tile_index(int value, int max_value_inclusive) {
    if (max_value_inclusive < 0) {
        return 0;
    }
    return std::clamp(value, 0, max_value_inclusive);
}

} // namespace

LayerStackRenderer::LayerStackRenderer(SDL_Renderer* renderer)
    : renderer_(renderer),
      layer_effect_processor_(renderer) {}

LayerStackRenderer::~LayerStackRenderer() {
    reset_targets();
    reset_gpu_upload();
}

void LayerStackRenderer::set_output_dimensions(int screen_width, int screen_height) {
    const int safe_w = std::max(1, screen_width);
    const int safe_h = std::max(1, screen_height);
    if (screen_width_ == safe_w && screen_height_ == safe_h) {
        return;
    }
    screen_width_ = safe_w;
    screen_height_ = safe_h;
    reset_targets();
}

void LayerStackRenderer::reset_targets() {
    for (TextureSet& set : layer_targets_) {
        destroy_texture(set.base);
        destroy_texture(set.dark_mask);
        destroy_texture(set.dark_mask_merged);
        destroy_texture(set.lit);
        set.dark_mask_history_write_index = 0;
        set.valid_dark_mask_history_count = 0;
    }
    layer_targets_.clear();
    destroy_texture(gpu_compact_geometry_);
    destroy_texture(gpu_compact_light_);
    destroy_texture(gpu_compact_final_);
}

bool LayerStackRenderer::ensure_target(SDL_Texture*& texture) const {
    if (!renderer_ || screen_width_ <= 0 || screen_height_ <= 0) {
        return false;
    }

    int tex_w = 0;
    int tex_h = 0;
    if (texture) {
        float w = 0.0f;
        float h = 0.0f;
        if (SDL_GetTextureSize(texture, &w, &h)) {
            tex_w = static_cast<int>(std::lround(w));
            tex_h = static_cast<int>(std::lround(h));
        }
        if (tex_w != screen_width_ || tex_h != screen_height_) {
            destroy_texture(texture);
        }
    }

    if (!texture) {
        texture = render_diagnostics::create_texture(renderer_,
                                                     SDL_PIXELFORMAT_RGBA8888,
                                                     SDL_TEXTUREACCESS_TARGET,
                                                     screen_width_,
                                                     screen_height_);
        if (!texture) {
            return false;
        }
        SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
    }

    return true;
}

bool LayerStackRenderer::ensure_layer_capacity(int layer_count) {
    if (layer_count < 0) {
        return false;
    }

    if (static_cast<int>(layer_targets_.size()) != layer_count) {
        reset_targets();
        layer_targets_.resize(static_cast<std::size_t>(layer_count));
    }

    for (TextureSet& set : layer_targets_) {
        if (!ensure_target(set.base) ||
            !ensure_target(set.dark_mask) ||
            !ensure_target(set.dark_mask_merged) ||
            !ensure_target(set.lit)) {
            return false;
        }
    }

    return true;
}

bool LayerStackRenderer::copy_texture(SDL_Texture* src, SDL_Texture* dst) const {
    if (!renderer_ || !src || !dst) {
        return false;
    }

    clear_target(dst);
    if (!render_diagnostics::set_render_target(renderer_, dst)) {
        return false;
    }
    SDL_SetTextureBlendMode(src, SDL_BLENDMODE_BLEND);
    SDL_SetTextureAlphaMod(src, 255);
    SDL_SetTextureColorMod(src, 255, 255, 255);
    render_diagnostics::render_texture(renderer_, src, nullptr, nullptr);
    return true;
}

void LayerStackRenderer::clear_target(SDL_Texture* texture) const {
    if (!renderer_ || !texture) {
        return;
    }
    if (!render_diagnostics::set_render_target(renderer_, texture)) {
        return;
    }
    SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 0);
    SDL_RenderClear(renderer_);
}

bool LayerStackRenderer::initialize_gpu_upload() {
    if (gpu_upload_.device) {
        gpu_upload_.active = true;
        return true;
    }
    if (!renderer_) {
        gpu_upload_.active = false;
        return false;
    }

    SDL_PropertiesID renderer_props = SDL_GetRendererProperties(renderer_);
    if (!renderer_props) {
        gpu_upload_.active = false;
        return false;
    }

    gpu_upload_.device = static_cast<SDL_GPUDevice*>(
        SDL_GetPointerProperty(renderer_props, SDL_PROP_RENDERER_GPU_DEVICE_POINTER, nullptr));
    gpu_upload_.active = (gpu_upload_.device != nullptr);
    return gpu_upload_.active;
}

void LayerStackRenderer::reset_gpu_upload() {
    if (gpu_upload_.device) {
        auto release_buffer = [&](SDL_GPUBuffer*& buffer) {
            if (buffer) {
                SDL_ReleaseGPUBuffer(gpu_upload_.device, buffer);
                buffer = nullptr;
                ++gpu_upload_.buffer_destroy_count;
                render_diagnostics::add_gpu_buffer_destroy_count();
            }
        };
        release_buffer(gpu_upload_.vertex_buffer);
        release_buffer(gpu_upload_.index_buffer);
        release_buffer(gpu_upload_.material_buffer);
        release_buffer(gpu_upload_.light_buffer);
        release_buffer(gpu_upload_.packet_buffer);
        if (gpu_upload_.transfer_buffer) {
            SDL_ReleaseGPUTransferBuffer(gpu_upload_.device, gpu_upload_.transfer_buffer);
            gpu_upload_.transfer_buffer = nullptr;
        }
    }
    gpu_upload_.vertex_capacity_bytes = 0;
    gpu_upload_.index_capacity_bytes = 0;
    gpu_upload_.material_capacity_bytes = 0;
    gpu_upload_.light_capacity_bytes = 0;
    gpu_upload_.packet_capacity_bytes = 0;
    gpu_upload_.transfer_capacity_bytes = 0;
    gpu_upload_.device = nullptr;
    gpu_upload_.active = false;
}

bool LayerStackRenderer::ensure_gpu_buffer_capacity(SDL_GPUBuffer*& buffer,
                                                    std::size_t& capacity_bytes,
                                                    std::size_t required_bytes,
                                                    SDL_GPUBufferUsageFlags usage) {
    if (required_bytes == 0) {
        return true;
    }
    if (!initialize_gpu_upload() || !gpu_upload_.device) {
        return false;
    }
    if (required_bytes > static_cast<std::size_t>(std::numeric_limits<Uint32>::max())) {
        return false;
    }
    if (buffer && capacity_bytes >= required_bytes) {
        return true;
    }

    const std::size_t target_capacity = next_capacity(capacity_bytes, required_bytes);
    if (target_capacity > static_cast<std::size_t>(std::numeric_limits<Uint32>::max())) {
        return false;
    }

    if (buffer) {
        SDL_ReleaseGPUBuffer(gpu_upload_.device, buffer);
        buffer = nullptr;
        ++gpu_upload_.buffer_destroy_count;
        render_diagnostics::add_gpu_buffer_destroy_count();
        capacity_bytes = 0;
    }

    SDL_GPUBufferCreateInfo create_info{};
    create_info.usage = usage;
    create_info.size = static_cast<Uint32>(target_capacity);
    create_info.props = 0;
    buffer = SDL_CreateGPUBuffer(gpu_upload_.device, &create_info);
    if (!buffer) {
        return false;
    }
    capacity_bytes = target_capacity;
    ++gpu_upload_.buffer_create_count;
    render_diagnostics::add_gpu_buffer_create_count();
    return true;
}

bool LayerStackRenderer::ensure_transfer_capacity(std::size_t required_bytes) {
    if (required_bytes == 0) {
        return true;
    }
    if (!initialize_gpu_upload() || !gpu_upload_.device) {
        return false;
    }
    if (required_bytes > static_cast<std::size_t>(std::numeric_limits<Uint32>::max())) {
        return false;
    }
    if (gpu_upload_.transfer_buffer && gpu_upload_.transfer_capacity_bytes >= required_bytes) {
        return true;
    }

    const std::size_t target_capacity = next_capacity(gpu_upload_.transfer_capacity_bytes, required_bytes);
    if (target_capacity > static_cast<std::size_t>(std::numeric_limits<Uint32>::max())) {
        return false;
    }
    if (gpu_upload_.transfer_buffer) {
        SDL_ReleaseGPUTransferBuffer(gpu_upload_.device, gpu_upload_.transfer_buffer);
        gpu_upload_.transfer_buffer = nullptr;
        gpu_upload_.transfer_capacity_bytes = 0;
    }

    SDL_GPUTransferBufferCreateInfo create_info{};
    create_info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    create_info.size = static_cast<Uint32>(target_capacity);
    create_info.props = 0;
    gpu_upload_.transfer_buffer = SDL_CreateGPUTransferBuffer(gpu_upload_.device, &create_info);
    if (!gpu_upload_.transfer_buffer) {
        return false;
    }
    gpu_upload_.transfer_capacity_bytes = target_capacity;
    return true;
}

bool LayerStackRenderer::upload_frame_submission_buffers(
    const render_pipeline::LayerBuildResult& build,
    const std::vector<LayerEffectProcessor::RuntimeLight>& runtime_lights) {
    if (!initialize_gpu_upload() || !gpu_upload_.device) {
        gpu_upload_.active = false;
        return false;
    }
    gpu_upload_.active = true;

    std::vector<render_pipeline::GpuMaterialRecord> material_records;
    material_records.reserve(build.materials.size());
    for (const render_pipeline::DrawMaterial& material : build.materials) {
        render_pipeline::GpuMaterialRecord record{};
        record.texture_token = static_cast<std::uint64_t>(
            reinterpret_cast<std::uintptr_t>(material.texture));
        record.blend_mode = static_cast<std::uint32_t>(material.blend_mode);
        material_records.push_back(record);
    }

    std::vector<render_pipeline::GpuRuntimeLightRecord> light_records;
    light_records.reserve(runtime_lights.size());
    for (const LayerEffectProcessor::RuntimeLight& light : runtime_lights) {
        render_pipeline::GpuRuntimeLightRecord record{};
        record.screen_center_x = light.screen_center.x;
        record.screen_center_y = light.screen_center.y;
        record.radius_px = light.radius_px;
        record.radius_world = light.radius_world;
        record.world_z = light.world_z;
        record.intensity = light.intensity;
        record.falloff = light.falloff;
        record.color_rgba = pack_rgba(light.color);
        light_records.push_back(record);
    }

    const std::size_t vertex_bytes = build.packed_vertices.size() * sizeof(SDL_Vertex);
    const std::size_t index_bytes = build.packed_indices.size() * sizeof(int);
    const std::size_t material_bytes = material_records.size() * sizeof(render_pipeline::GpuMaterialRecord);
    const std::size_t light_bytes = light_records.size() * sizeof(render_pipeline::GpuRuntimeLightRecord);
    const std::size_t packet_bytes = build.gpu_packets.size() * sizeof(render_pipeline::GpuDrawPacketRecord);

    if (!ensure_gpu_buffer_capacity(gpu_upload_.vertex_buffer,
                                    gpu_upload_.vertex_capacity_bytes,
                                    vertex_bytes,
                                    SDL_GPU_BUFFERUSAGE_VERTEX)) {
        return false;
    }
    if (!ensure_gpu_buffer_capacity(gpu_upload_.index_buffer,
                                    gpu_upload_.index_capacity_bytes,
                                    index_bytes,
                                    SDL_GPU_BUFFERUSAGE_INDEX)) {
        return false;
    }
    if (!ensure_gpu_buffer_capacity(gpu_upload_.material_buffer,
                                    gpu_upload_.material_capacity_bytes,
                                    material_bytes,
                                    SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ)) {
        return false;
    }
    if (!ensure_gpu_buffer_capacity(gpu_upload_.light_buffer,
                                    gpu_upload_.light_capacity_bytes,
                                    light_bytes,
                                    SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ |
                                        SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ)) {
        return false;
    }
    if (!ensure_gpu_buffer_capacity(gpu_upload_.packet_buffer,
                                    gpu_upload_.packet_capacity_bytes,
                                    packet_bytes,
                                    SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ |
                                        SDL_GPU_BUFFERUSAGE_INDIRECT)) {
        return false;
    }

    std::size_t total_upload_bytes = 0;
    auto append_bytes = [&](std::size_t bytes) {
        if (bytes == 0) {
            return;
        }
        total_upload_bytes = align_up(total_upload_bytes, kUploadAlignmentBytes);
        total_upload_bytes += bytes;
    };
    append_bytes(vertex_bytes);
    append_bytes(index_bytes);
    append_bytes(material_bytes);
    append_bytes(light_bytes);
    append_bytes(packet_bytes);

    if (total_upload_bytes == 0) {
        return true;
    }
    if (!ensure_transfer_capacity(total_upload_bytes)) {
        return false;
    }

    std::uint8_t* mapped = static_cast<std::uint8_t*>(
        SDL_MapGPUTransferBuffer(gpu_upload_.device, gpu_upload_.transfer_buffer, true));
    if (!mapped) {
        return false;
    }

    struct UploadChunk {
        Uint32 offset = 0;
        Uint32 size = 0;
    };
    UploadChunk vertex_chunk{};
    UploadChunk index_chunk{};
    UploadChunk material_chunk{};
    UploadChunk light_chunk{};
    UploadChunk packet_chunk{};

    std::size_t cursor = 0;
    auto write_chunk = [&](const void* src, std::size_t size_bytes, UploadChunk& out_chunk) {
        if (!src || size_bytes == 0) {
            out_chunk = UploadChunk{};
            return;
        }
        cursor = align_up(cursor, kUploadAlignmentBytes);
        out_chunk.offset = static_cast<Uint32>(cursor);
        out_chunk.size = static_cast<Uint32>(size_bytes);
        std::memcpy(mapped + cursor, src, size_bytes);
        cursor += size_bytes;
    };
    write_chunk(build.packed_vertices.data(), vertex_bytes, vertex_chunk);
    write_chunk(build.packed_indices.data(), index_bytes, index_chunk);
    write_chunk(material_records.data(), material_bytes, material_chunk);
    write_chunk(light_records.data(), light_bytes, light_chunk);
    write_chunk(build.gpu_packets.data(), packet_bytes, packet_chunk);
    SDL_UnmapGPUTransferBuffer(gpu_upload_.device, gpu_upload_.transfer_buffer);

    SDL_GPUCommandBuffer* command_buffer = SDL_AcquireGPUCommandBuffer(gpu_upload_.device);
    if (!command_buffer) {
        return false;
    }
    SDL_GPUCopyPass* copy_pass = SDL_BeginGPUCopyPass(command_buffer);
    if (!copy_pass) {
        SDL_CancelGPUCommandBuffer(command_buffer);
        return false;
    }

    auto upload_chunk = [&](const UploadChunk& chunk, SDL_GPUBuffer* dst_buffer) {
        if (!dst_buffer || chunk.size == 0) {
            return;
        }
        SDL_GPUTransferBufferLocation src{};
        src.transfer_buffer = gpu_upload_.transfer_buffer;
        src.offset = chunk.offset;
        SDL_GPUBufferRegion dst{};
        dst.buffer = dst_buffer;
        dst.offset = 0;
        dst.size = chunk.size;
        SDL_UploadToGPUBuffer(copy_pass, &src, &dst, false);
    };

    upload_chunk(vertex_chunk, gpu_upload_.vertex_buffer);
    upload_chunk(index_chunk, gpu_upload_.index_buffer);
    upload_chunk(material_chunk, gpu_upload_.material_buffer);
    upload_chunk(light_chunk, gpu_upload_.light_buffer);
    upload_chunk(packet_chunk, gpu_upload_.packet_buffer);
    SDL_EndGPUCopyPass(copy_pass);

    const bool submitted = SDL_SubmitGPUCommandBuffer(command_buffer);
    if (!submitted) {
        SDL_CancelGPUCommandBuffer(command_buffer);
    }
    return submitted;
}

render_pipeline::GpuSubmissionStats LayerStackRenderer::current_gpu_submission_stats() const {
    render_pipeline::GpuSubmissionStats stats{};
    stats.active = gpu_upload_.active;
    stats.vertex_capacity_bytes = gpu_upload_.vertex_capacity_bytes;
    stats.index_capacity_bytes = gpu_upload_.index_capacity_bytes;
    stats.material_capacity_bytes = gpu_upload_.material_capacity_bytes;
    stats.light_capacity_bytes = gpu_upload_.light_capacity_bytes;
    stats.packet_capacity_bytes = gpu_upload_.packet_capacity_bytes;
    stats.buffer_create_count = gpu_upload_.buffer_create_count;
    stats.buffer_destroy_count = gpu_upload_.buffer_destroy_count;
    return stats;
}

bool LayerStackRenderer::ensure_gpu_compact_targets() {
    return ensure_target(gpu_compact_geometry_) &&
           ensure_target(gpu_compact_light_) &&
           ensure_target(gpu_compact_final_);
}

render_pipeline::GpuCompactRenderStats LayerStackRenderer::build_gpu_tiled_light_bins(
    const render_pipeline::LayerBuildResult& build,
    const std::vector<LayerEffectProcessor::RuntimeLight>& runtime_lights) {
    gpu_compact_stats_ = render_pipeline::GpuCompactRenderStats{};
    const int tile_size = std::max(16, gpu_tiled_light_bins_.tile_size_px);
    const int tile_count_x = std::max(1, (screen_width_ + tile_size - 1) / tile_size);
    const int tile_count_y = std::max(1, (screen_height_ + tile_size - 1) / tile_size);

    gpu_tiled_light_bins_.tile_size_px = tile_size;
    gpu_tiled_light_bins_.tile_count_x = tile_count_x;
    gpu_tiled_light_bins_.tile_count_y = tile_count_y;
    gpu_tiled_light_bins_.source_light_count = runtime_lights.size();
    gpu_tiled_light_bins_.bins.assign(static_cast<std::size_t>(tile_count_x * tile_count_y), {});
    gpu_tiled_light_bins_.dedupe_stamps.assign(runtime_lights.size(), 0u);
    gpu_tiled_light_bins_.dedupe_generation = 1u;

    std::uint32_t assignment_count = 0;
    for (std::uint32_t light_index = 0; light_index < runtime_lights.size(); ++light_index) {
        const LayerEffectProcessor::RuntimeLight& light = runtime_lights[light_index];
        if (!std::isfinite(light.screen_center.x) ||
            !std::isfinite(light.screen_center.y) ||
            !std::isfinite(light.radius_px) ||
            light.radius_px <= 0.5f) {
            continue;
        }
        const float min_x = light.screen_center.x - light.radius_px;
        const float max_x = light.screen_center.x + light.radius_px;
        const float min_y = light.screen_center.y - light.radius_px;
        const float max_y = light.screen_center.y + light.radius_px;
        const int tile_min_x = clamp_tile_index(static_cast<int>(std::floor(min_x / static_cast<float>(tile_size))),
                                                tile_count_x - 1);
        const int tile_max_x = clamp_tile_index(static_cast<int>(std::floor(max_x / static_cast<float>(tile_size))),
                                                tile_count_x - 1);
        const int tile_min_y = clamp_tile_index(static_cast<int>(std::floor(min_y / static_cast<float>(tile_size))),
                                                tile_count_y - 1);
        const int tile_max_y = clamp_tile_index(static_cast<int>(std::floor(max_y / static_cast<float>(tile_size))),
                                                tile_count_y - 1);
        for (int ty = tile_min_y; ty <= tile_max_y; ++ty) {
            for (int tx = tile_min_x; tx <= tile_max_x; ++tx) {
                const std::size_t tile_index = static_cast<std::size_t>(ty * tile_count_x + tx);
                gpu_tiled_light_bins_.bins[tile_index].push_back(light_index);
                ++assignment_count;
            }
        }
    }

    const std::uint32_t layer_count = static_cast<std::uint32_t>(std::max<std::size_t>(1u, build.non_empty_layers.size()));
    gpu_compact_stats_.tile_count_x = static_cast<std::uint32_t>(tile_count_x);
    gpu_compact_stats_.tile_count_y = static_cast<std::uint32_t>(tile_count_y);
    gpu_compact_stats_.tile_size_px = static_cast<std::uint32_t>(tile_size);
    gpu_compact_stats_.tile_light_assignment_count = assignment_count;
    gpu_compact_stats_.naive_light_evaluations = static_cast<std::uint32_t>(runtime_lights.size()) * layer_count;
    return gpu_compact_stats_;
}

bool LayerStackRenderer::query_gpu_tiled_light_candidates(const render_internal::ScreenAabb& bounds,
                                                          std::vector<std::uint32_t>& out_candidates) {
    out_candidates.clear();
    if (gpu_tiled_light_bins_.bins.empty() ||
        gpu_tiled_light_bins_.tile_count_x <= 0 ||
        gpu_tiled_light_bins_.tile_count_y <= 0) {
        return false;
    }
    if (!std::isfinite(bounds.min_x) || !std::isfinite(bounds.min_y) ||
        !std::isfinite(bounds.max_x) || !std::isfinite(bounds.max_y)) {
        return false;
    }
    const int tile_size = std::max(1, gpu_tiled_light_bins_.tile_size_px);
    const int min_tile_x = clamp_tile_index(static_cast<int>(std::floor(bounds.min_x / static_cast<float>(tile_size))),
                                            gpu_tiled_light_bins_.tile_count_x - 1);
    const int max_tile_x = clamp_tile_index(static_cast<int>(std::floor(bounds.max_x / static_cast<float>(tile_size))),
                                            gpu_tiled_light_bins_.tile_count_x - 1);
    const int min_tile_y = clamp_tile_index(static_cast<int>(std::floor(bounds.min_y / static_cast<float>(tile_size))),
                                            gpu_tiled_light_bins_.tile_count_y - 1);
    const int max_tile_y = clamp_tile_index(static_cast<int>(std::floor(bounds.max_y / static_cast<float>(tile_size))),
                                            gpu_tiled_light_bins_.tile_count_y - 1);

    std::uint32_t generation = ++gpu_tiled_light_bins_.dedupe_generation;
    if (generation == 0u) {
        std::fill(gpu_tiled_light_bins_.dedupe_stamps.begin(), gpu_tiled_light_bins_.dedupe_stamps.end(), 0u);
        generation = 1u;
        gpu_tiled_light_bins_.dedupe_generation = generation;
    }

    for (int ty = min_tile_y; ty <= max_tile_y; ++ty) {
        for (int tx = min_tile_x; tx <= max_tile_x; ++tx) {
            const std::size_t tile_index = static_cast<std::size_t>(ty * gpu_tiled_light_bins_.tile_count_x + tx);
            const std::vector<std::uint32_t>& tile_lights = gpu_tiled_light_bins_.bins[tile_index];
            for (const std::uint32_t light_index : tile_lights) {
                if (light_index >= gpu_tiled_light_bins_.dedupe_stamps.size()) {
                    continue;
                }
                if (gpu_tiled_light_bins_.dedupe_stamps[light_index] == generation) {
                    continue;
                }
                gpu_tiled_light_bins_.dedupe_stamps[light_index] = generation;
                out_candidates.push_back(light_index);
            }
        }
    }
    return true;
}

void LayerStackRenderer::draw_layer_geometry(const render_pipeline::LayerBuildResult& build,
                                             const render_pipeline::LayerSubmission& layer) const {
    if (!renderer_) {
        return;
    }

    if (!layer.command_ranges.empty() &&
        !build.materials.empty() &&
        !build.packed_vertices.empty() &&
        !build.packed_indices.empty()) {
        const int total_vertices = static_cast<int>(build.packed_vertices.size());
        for (const render_pipeline::DrawCommandRange& range : layer.command_ranges) {
            if (range.material_index >= build.materials.size()) {
                continue;
            }
            if (range.index_count == 0 ||
                range.index_offset >= build.packed_indices.size() ||
                range.index_offset + range.index_count > build.packed_indices.size()) {
                continue;
            }
            const render_pipeline::DrawMaterial& material =
                build.materials[static_cast<std::size_t>(range.material_index)];
            if (!material.texture) {
                continue;
            }
            SDL_SetTextureBlendMode(material.texture, safe_layer_blend_mode(material.blend_mode));
            render_diagnostics::render_geometry(renderer_,
                                               material.texture,
                                               build.packed_vertices.data(),
                                               total_vertices,
                                               build.packed_indices.data() + range.index_offset,
                                               static_cast<int>(range.index_count));
        }
        return;
    }

    static constexpr int kQuadIndices[6] = {0, 1, 2, 0, 2, 3};
    for (const render_pipeline::GeometryLayerDrawItem& draw : layer.draws) {
        if (!draw.texture) {
            continue;
        }
        SDL_SetTextureBlendMode(draw.texture, safe_layer_blend_mode(draw.blend_mode));
        render_diagnostics::render_geometry(renderer_, draw.texture, draw.vertices.data(), 4, kQuadIndices, 6);
    }
}

void LayerStackRenderer::render_layer_base(const render_pipeline::LayerBuildResult& build,
                                           const render_pipeline::LayerSubmission& layer,
                                           SDL_Texture* target) const {
    if (!renderer_ || !target) {
        return;
    }

    clear_target(target);
    if (!render_diagnostics::set_render_target(renderer_, target)) {
        return;
    }
    draw_layer_geometry(build, layer);
}

render_pipeline::LayerRenderResult LayerStackRenderer::render(
    const render_pipeline::LayerBuildResult& build,
    const std::vector<LayerEffectProcessor::RuntimeLight>& runtime_lights,
    bool runtime_lighting_enabled) {
    render_pipeline::LayerRenderResult out{};
    out.layer_count = build.layer_count;
    out.player_layer_index = build.player_layer_index;
    out.non_empty_layers = build.non_empty_layers;

    if (!renderer_ ||
        !build.valid ||
        build.layer_count <= 0 ||
        static_cast<int>(build.layers.size()) != build.layer_count ||
        !ensure_layer_capacity(build.layer_count)) {
        out.gpu_submission = current_gpu_submission_stats();
        return out;
    }

    (void)upload_frame_submission_buffers(build, runtime_lights);
    out.gpu_submission = current_gpu_submission_stats();

    out.final_layer_textures.assign(static_cast<std::size_t>(build.layer_count), nullptr);
    out.owning_body_lights.resize(static_cast<std::size_t>(build.layer_count));
    frame_scratch_.clear_for_frame(static_cast<std::size_t>(build.layer_count));
    frame_scratch_.light_metadata.resize(runtime_lights.size());

    for (int layer_index : build.non_empty_layers) {
        if (layer_index < 0 || layer_index >= build.layer_count) {
            continue;
        }
        const render_pipeline::LayerSubmission& layer = build.layers[static_cast<std::size_t>(layer_index)];
        const std::size_t li = static_cast<std::size_t>(layer_index);
        const double depth_min = std::isfinite(layer.depth_min) ? layer.depth_min : layer.representative_depth;
        const double depth_max = std::isfinite(layer.depth_max) ? layer.depth_max : layer.representative_depth;
        frame_scratch_.layer_metadata[li].layer_index = layer_index;
        frame_scratch_.layer_metadata[li].depth_interval =
            render_internal::make_sorted_depth_interval(depth_min, depth_max);
        frame_scratch_.layer_metadata[li].screen_bounds = render_internal::ScreenAabb{
            layer.bounds_min_x,
            layer.bounds_min_y,
            layer.bounds_max_x,
            layer.bounds_max_y};
        frame_scratch_.layer_order_by_depth_start.push_back(li);
    }

    const std::size_t non_empty_layer_count = frame_scratch_.layer_order_by_depth_start.size();
    const std::size_t expected_lights_per_layer =
        (non_empty_layer_count > 0) ? ((runtime_lights.size() / non_empty_layer_count) + 2) : 0;
    for (std::size_t li : frame_scratch_.layer_order_by_depth_start) {
        frame_scratch_.per_layer_light_indices[li].reserve(
            std::max(frame_scratch_.per_layer_light_indices[li].capacity(), expected_lights_per_layer));
    }

    std::sort(frame_scratch_.layer_order_by_depth_start.begin(),
              frame_scratch_.layer_order_by_depth_start.end(),
              [this](std::size_t lhs, std::size_t rhs) {
                  return frame_scratch_.layer_metadata[lhs].depth_interval.min <
                         frame_scratch_.layer_metadata[rhs].depth_interval.min;
              });

    for (std::uint32_t light_index = 0; light_index < runtime_lights.size(); ++light_index) {
        const LayerEffectProcessor::RuntimeLight& light = runtime_lights[light_index];
        const std::size_t light_meta_index = static_cast<std::size_t>(light_index);
        frame_scratch_.light_metadata[light_meta_index].depth_interval = render_internal::light_depth_interval(light);
        frame_scratch_.light_metadata[light_meta_index].screen_bounds = render_internal::ScreenAabb{
            light.screen_center.x - light.radius_px,
            light.screen_center.y - light.radius_px,
            light.screen_center.x + light.radius_px,
            light.screen_center.y + light.radius_px};

        if (!std::isfinite(light.screen_center.x) || !std::isfinite(light.screen_center.y)) {
            continue;
        }

        for (const std::size_t li : frame_scratch_.layer_order_by_depth_start) {
            const FrameScratchArena::LayerMetadata& layer_meta = frame_scratch_.layer_metadata[li];
            const bool bounds_overlap =
                render_internal::screen_aabb_overlaps(frame_scratch_.light_metadata[light_meta_index].screen_bounds,
                                                      layer_meta.screen_bounds);
            const bool center_inside =
                light.screen_center.x >= layer_meta.screen_bounds.min_x &&
                light.screen_center.x <= layer_meta.screen_bounds.max_x &&
                light.screen_center.y >= layer_meta.screen_bounds.min_y &&
                light.screen_center.y <= layer_meta.screen_bounds.max_y;
            if (!bounds_overlap && !center_inside) {
                continue;
            }
            frame_scratch_.per_layer_light_indices[li].push_back(light_index);
        }
    }

    (void)runtime_lighting_enabled;

    for (int layer_index : build.non_empty_layers) {
        if (layer_index < 0 || layer_index >= build.layer_count) {
            continue;
        }

        const render_pipeline::LayerSubmission& layer = build.layers[static_cast<std::size_t>(layer_index)];
        TextureSet& targets = layer_targets_[static_cast<std::size_t>(layer_index)];
        render_layer_base(build, layer, targets.base);

        const std::size_t li = static_cast<std::size_t>(layer_index);
        out.owning_body_lights[li].clear();
        out.final_layer_textures[static_cast<std::size_t>(layer_index)] = targets.base;
    }

    out.valid = true;
    return out;
}

render_pipeline::CompactLayerRenderResult LayerStackRenderer::render_gpu_compact(
    const render_pipeline::LayerBuildResult& build,
    const std::vector<LayerEffectProcessor::RuntimeLight>& runtime_lights,
    bool runtime_lighting_enabled) {
    render_pipeline::CompactLayerRenderResult out{};
    out.gpu_submission = current_gpu_submission_stats();
    if (!renderer_ ||
        !build.valid ||
        build.layer_count <= 0 ||
        static_cast<int>(build.layers.size()) != build.layer_count ||
        !ensure_gpu_compact_targets()) {
        return out;
    }

    (void)upload_frame_submission_buffers(build, runtime_lights);
    out.gpu_submission = current_gpu_submission_stats();

    render_pipeline::GpuCompactRenderStats compact_stats = gpu_compact_stats_;
    const bool need_rebuild_tiled_bins =
        gpu_tiled_light_bins_.bins.empty() ||
        gpu_tiled_light_bins_.source_light_count != runtime_lights.size() ||
        gpu_tiled_light_bins_.tile_count_x <= 0 ||
        gpu_tiled_light_bins_.tile_count_y <= 0;
    if (need_rebuild_tiled_bins) {
        compact_stats = build_gpu_tiled_light_bins(build, runtime_lights);
    } else {
        const std::uint32_t layer_count = static_cast<std::uint32_t>(
            std::max<std::size_t>(1u, build.non_empty_layers.size()));
        compact_stats.naive_light_evaluations =
            static_cast<std::uint32_t>(runtime_lights.size()) * layer_count;
        compact_stats.tiled_light_evaluations = 0;
        compact_stats.aggregated_light_count = 0;
    }

    frame_scratch_.clear_for_frame(static_cast<std::size_t>(build.layer_count));
    frame_scratch_.light_metadata.resize(runtime_lights.size());
    for (int layer_index : build.non_empty_layers) {
        if (layer_index < 0 || layer_index >= build.layer_count) {
            continue;
        }
        const render_pipeline::LayerSubmission& layer = build.layers[static_cast<std::size_t>(layer_index)];
        const std::size_t li = static_cast<std::size_t>(layer_index);
        const double depth_min = std::isfinite(layer.depth_min) ? layer.depth_min : layer.representative_depth;
        const double depth_max = std::isfinite(layer.depth_max) ? layer.depth_max : layer.representative_depth;
        frame_scratch_.layer_metadata[li].layer_index = layer_index;
        frame_scratch_.layer_metadata[li].depth_interval =
            render_internal::make_sorted_depth_interval(depth_min, depth_max);
        frame_scratch_.layer_metadata[li].screen_bounds = render_internal::ScreenAabb{
            layer.bounds_min_x,
            layer.bounds_min_y,
            layer.bounds_max_x,
            layer.bounds_max_y};
        frame_scratch_.layer_order_by_depth_start.push_back(li);
    }

    for (std::uint32_t light_index = 0; light_index < runtime_lights.size(); ++light_index) {
        const LayerEffectProcessor::RuntimeLight& light = runtime_lights[light_index];
        frame_scratch_.light_metadata[static_cast<std::size_t>(light_index)].depth_interval =
            render_internal::light_depth_interval(light);
        frame_scratch_.light_metadata[static_cast<std::size_t>(light_index)].screen_bounds =
            render_internal::ScreenAabb{
                light.screen_center.x - light.radius_px,
                light.screen_center.y - light.radius_px,
                light.screen_center.x + light.radius_px,
                light.screen_center.y + light.radius_px};
    }

    constexpr std::size_t kMaxLightsPerCluster = 32;
    constexpr std::size_t kMaxLightsPerFrame = 256;
    constexpr std::size_t kFarTierDominantLightCount = 1;
    constexpr float kNearTierDepth = 12.0f;
    constexpr float kMidTierDepth = 42.0f;
    constexpr float kSmallFootprintPx = 24.0f;

    std::vector<float> effective_light_intensity(runtime_lights.size(), 0.0f);
    std::vector<std::uint32_t> tile_candidates{};
    for (const std::size_t li : frame_scratch_.layer_order_by_depth_start) {
        tile_candidates.clear();
        const render_internal::ScreenAabb layer_bounds = frame_scratch_.layer_metadata[li].screen_bounds;
        if (!query_gpu_tiled_light_candidates(layer_bounds, tile_candidates)) {
            continue;
        }
        std::sort(tile_candidates.begin(), tile_candidates.end());
        if (tile_candidates.size() > kMaxLightsPerCluster) {
            tile_candidates.resize(kMaxLightsPerCluster);
        }
        compact_stats.tiled_light_evaluations += static_cast<std::uint32_t>(tile_candidates.size());
        const render_internal::DepthInterval& layer_depth = frame_scratch_.layer_metadata[li].depth_interval;
        const float layer_depth_abs = static_cast<float>(std::max(std::fabs(layer_depth.min), std::fabs(layer_depth.max)));
        const float layer_w = std::max(0.0f, layer_bounds.max_x - layer_bounds.min_x);
        const float layer_h = std::max(0.0f, layer_bounds.max_y - layer_bounds.min_y);
        const float layer_footprint_px = std::max(layer_w, layer_h);
        enum class ClusterTier { Near, Mid, Far };
        ClusterTier tier = ClusterTier::Near;
        if (layer_depth_abs >= kMidTierDepth || layer_footprint_px <= kSmallFootprintPx) tier = ClusterTier::Far;
        else if (layer_depth_abs >= kNearTierDepth) tier = ClusterTier::Mid;

        std::size_t accepted_in_cluster = 0;
        for (const std::uint32_t light_index : tile_candidates) {
            if (light_index >= runtime_lights.size()) {
                continue;
            }
            const LayerEffectProcessor::RuntimeLight& light = runtime_lights[light_index];
            const render_internal::DepthInterval& light_depth =
                frame_scratch_.light_metadata[static_cast<std::size_t>(light_index)].depth_interval;
            const int signed_separation = render_internal::compare_depth_intervals_signed(light_depth, layer_depth);
            float adjusted_intensity = light.intensity;
            if (tier == ClusterTier::Mid && accepted_in_cluster >= (kMaxLightsPerCluster / 2)) {
                continue;
            }
            if (tier == ClusterTier::Far && accepted_in_cluster >= kFarTierDominantLightCount) {
                continue;
            }
            if (tier != ClusterTier::Far) {
                adjusted_intensity = render_internal::LightingSystemV2::attenuate_for_layer(
                    light,
                    layer_depth,
                    frame_scratch_.layer_metadata[li].screen_bounds);
            }
            if (adjusted_intensity > effective_light_intensity[light_index]) {
                effective_light_intensity[light_index] = adjusted_intensity;
                ++accepted_in_cluster;
            }
        }
    }

    std::vector<LayerEffectProcessor::GpuRadialLight> aggregated_lights;
    aggregated_lights.reserve(runtime_lights.size());
    for (std::size_t i = 0; i < runtime_lights.size(); ++i) {
        const LayerEffectProcessor::RuntimeLight& source = runtime_lights[i];
        const float effective_intensity = effective_light_intensity[i];
        if (!(std::isfinite(effective_intensity) && effective_intensity > 0.0005f)) {
            continue;
        }
        LayerEffectProcessor::GpuRadialLight light{};
        light.center = source.screen_center;
        light.color = source.color;
        light.radius_x_px = std::max(2.0f, source.radius_px);
        light.radius_y_px = std::max(2.0f, source.radius_px);
        light.intensity = effective_intensity;
        light.opacity = std::clamp(source.opacity > 0.0f ? source.opacity : 1.0f, 0.0f, 1.0f);
        light.falloff = std::max(0.05f, source.falloff);
        aggregated_lights.push_back(light);
    }
    std::stable_sort(aggregated_lights.begin(), aggregated_lights.end(), [](const auto& a, const auto& b){ return a.intensity > b.intensity; });
    if (aggregated_lights.size() > kMaxLightsPerFrame) aggregated_lights.resize(kMaxLightsPerFrame);
    compact_stats.aggregated_light_count = static_cast<std::uint32_t>(aggregated_lights.size());

    if (!render_diagnostics::set_render_target(renderer_, gpu_compact_geometry_)) {
        return out;
    }
    SDL_SetRenderViewport(renderer_, nullptr);
    SDL_SetRenderClipRect(renderer_, nullptr);
    SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 0);
    SDL_RenderClear(renderer_);
    for (auto it = build.non_empty_layers.rbegin(); it != build.non_empty_layers.rend(); ++it) {
        const int layer_index = *it;
        if (layer_index < 0 || layer_index >= build.layer_count) {
            continue;
        }
        draw_layer_geometry(build, build.layers[static_cast<std::size_t>(layer_index)]);
    }

    if (!runtime_lighting_enabled) {
        if (!copy_texture(gpu_compact_geometry_, gpu_compact_final_)) {
            return out;
        }
        out.valid = true;
        out.final_texture = gpu_compact_final_;
        out.compact_stats = compact_stats;
        gpu_compact_stats_ = compact_stats;
        render_diagnostics::set_gpu_light_culling_stats(compact_stats.tile_light_assignment_count,
                                                        compact_stats.naive_light_evaluations,
                                                        compact_stats.tiled_light_evaluations);
        return out;
    }

    layer_effect_processor_.set_renderer(renderer_);
    if (!layer_effect_processor_.render_gpu_light_field(gpu_compact_light_,
                                                        SDL_Color{18, 20, 24, 255},
                                                        aggregated_lights,
                                                        SDL_BLENDMODE_ADD)) {
        return out;
    }

    if (!render_diagnostics::set_render_target(renderer_, gpu_compact_final_)) {
        return out;
    }
    SDL_SetRenderViewport(renderer_, nullptr);
    SDL_SetRenderClipRect(renderer_, nullptr);
    SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 0);
    SDL_RenderClear(renderer_);
    SDL_SetTextureBlendMode(gpu_compact_geometry_, SDL_BLENDMODE_BLEND);
    SDL_SetTextureAlphaMod(gpu_compact_geometry_, 255);
    SDL_SetTextureColorMod(gpu_compact_geometry_, 255, 255, 255);
    render_diagnostics::render_texture(renderer_, gpu_compact_geometry_, nullptr, nullptr);
    SDL_SetTextureBlendMode(gpu_compact_light_, SDL_BLENDMODE_MOD);
    SDL_SetTextureAlphaMod(gpu_compact_light_, 255);
    SDL_SetTextureColorMod(gpu_compact_light_, 255, 255, 255);
    render_diagnostics::render_texture(renderer_, gpu_compact_light_, nullptr, nullptr);

    out.valid = true;
    out.final_texture = gpu_compact_final_;
    out.compact_stats = compact_stats;
    gpu_compact_stats_ = compact_stats;
    render_diagnostics::set_gpu_light_culling_stats(compact_stats.tile_light_assignment_count,
                                                    compact_stats.naive_light_evaluations,
                                                    compact_stats.tiled_light_evaluations);
    return out;
}
