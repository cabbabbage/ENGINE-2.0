#pragma once

#include <SDL3/SDL.h>
#include <SDL3/SDL_gpu.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "rendering/render/layer_effect_processor.hpp"
#include "rendering/render/render.hpp"
#include "rendering/render/render_pipeline_types.hpp"

class LayerStackRenderer {
public:
    explicit LayerStackRenderer(SDL_Renderer* renderer);
    ~LayerStackRenderer();

    LayerStackRenderer(const LayerStackRenderer&) = delete;
    LayerStackRenderer& operator=(const LayerStackRenderer&) = delete;

    void set_output_dimensions(int screen_width, int screen_height);

    render_pipeline::LayerRenderResult render(
        const render_pipeline::LayerBuildResult& build,
        const std::vector<LayerEffectProcessor::RuntimeLight>& runtime_lights,
        bool runtime_lighting_enabled,
        float front_layer_light_strength_multiplier,
        float behind_layer_light_strength_multiplier);
    render_pipeline::GpuCompactRenderStats build_gpu_tiled_light_bins(
        const render_pipeline::LayerBuildResult& build,
        const std::vector<LayerEffectProcessor::RuntimeLight>& runtime_lights);
    render_pipeline::CompactLayerRenderResult render_gpu_compact(
        const render_pipeline::LayerBuildResult& build,
        const std::vector<LayerEffectProcessor::RuntimeLight>& runtime_lights,
        bool runtime_lighting_enabled,
        float front_layer_light_strength_multiplier,
        float behind_layer_light_strength_multiplier);
    const render_pipeline::GpuCompactRenderStats& gpu_tiled_light_bin_stats() const {
        return gpu_compact_stats_;
    }

private:
    struct FrameScratchArena {
        struct LayerMetadata {
            int layer_index = -1;
            render_internal::DepthInterval depth_interval{};
            render_internal::ScreenAabb screen_bounds{};
        };
        struct LightMetadata {
            render_internal::DepthInterval depth_interval{};
            render_internal::ScreenAabb screen_bounds{};
        };
        std::vector<std::vector<std::uint32_t>> per_layer_light_indices;
        std::vector<LayerEffectProcessor::RuntimeLight> layer_light_buffer;
        std::vector<LayerEffectProcessor::RuntimeLight> owner_light_buffer;
        std::vector<LayerMetadata> layer_metadata;
        std::vector<LightMetadata> light_metadata;
        std::vector<std::size_t> layer_order_by_depth_start;

        void clear_for_frame(std::size_t layer_count) {
            if (per_layer_light_indices.size() < layer_count) {
                per_layer_light_indices.resize(layer_count);
            }
            for (std::size_t i = 0; i < layer_count; ++i) {
                per_layer_light_indices[i].clear();
            }
            layer_light_buffer.clear();
            owner_light_buffer.clear();
            layer_metadata.assign(layer_count, LayerMetadata{});
            light_metadata.clear();
            layer_order_by_depth_start.clear();
        }
    };

    struct TextureSet {
        SDL_Texture* base = nullptr;
        SDL_Texture* dark_mask = nullptr;
        SDL_Texture* dark_mask_merged = nullptr;
        SDL_Texture* lit = nullptr;
        std::uint8_t dark_mask_history_write_index = 0;
        std::uint8_t valid_dark_mask_history_count = 0;
    };

    struct GpuUploadResources {
        SDL_GPUDevice* device = nullptr;
        SDL_GPUBuffer* vertex_buffer = nullptr;
        SDL_GPUBuffer* index_buffer = nullptr;
        SDL_GPUBuffer* material_buffer = nullptr;
        SDL_GPUBuffer* light_buffer = nullptr;
        SDL_GPUBuffer* packet_buffer = nullptr;
        SDL_GPUTransferBuffer* transfer_buffer = nullptr;
        std::size_t vertex_capacity_bytes = 0;
        std::size_t index_capacity_bytes = 0;
        std::size_t material_capacity_bytes = 0;
        std::size_t light_capacity_bytes = 0;
        std::size_t packet_capacity_bytes = 0;
        std::size_t transfer_capacity_bytes = 0;
        std::uint64_t buffer_create_count = 0;
        std::uint64_t buffer_destroy_count = 0;
        bool active = false;
    };

    struct GpuTiledLightBins {
        int tile_size_px = 16;
        int tile_count_x = 0;
        int tile_count_y = 0;
        std::vector<std::vector<std::uint32_t>> bins;
        std::vector<std::uint32_t> dedupe_stamps;
        std::uint32_t dedupe_generation = 1;
        std::size_t source_light_count = 0;
    };

    bool ensure_target(SDL_Texture*& texture) const;
    bool ensure_layer_capacity(int layer_count);
    void reset_targets();
    void clear_target(SDL_Texture* texture) const;
    bool copy_texture(SDL_Texture* src, SDL_Texture* dst) const;
    bool initialize_gpu_upload();
    void reset_gpu_upload();
    bool ensure_gpu_buffer_capacity(SDL_GPUBuffer*& buffer,
                                    std::size_t& capacity_bytes,
                                    std::size_t required_bytes,
                                    SDL_GPUBufferUsageFlags usage);
    bool ensure_transfer_capacity(std::size_t required_bytes);
    bool upload_frame_submission_buffers(
        const render_pipeline::LayerBuildResult& build,
        const std::vector<LayerEffectProcessor::RuntimeLight>& runtime_lights);
    render_pipeline::GpuSubmissionStats current_gpu_submission_stats() const;
    bool ensure_gpu_compact_targets();
    bool query_gpu_tiled_light_candidates(const render_internal::ScreenAabb& bounds,
                                          std::vector<std::uint32_t>& out_candidates);
    void draw_layer_geometry(const render_pipeline::LayerBuildResult& build,
                             const render_pipeline::LayerSubmission& layer) const;

    void render_layer_base(const render_pipeline::LayerBuildResult& build,
                           const render_pipeline::LayerSubmission& layer,
                           SDL_Texture* target) const;

    SDL_Renderer* renderer_ = nullptr;
    int screen_width_ = 0;
    int screen_height_ = 0;
    LayerEffectProcessor layer_effect_processor_;
    std::vector<TextureSet> layer_targets_;
    FrameScratchArena frame_scratch_;
    GpuUploadResources gpu_upload_;
    SDL_Texture* gpu_compact_geometry_ = nullptr;
    SDL_Texture* gpu_compact_light_ = nullptr;
    SDL_Texture* gpu_compact_final_ = nullptr;
    GpuTiledLightBins gpu_tiled_light_bins_{};
    render_pipeline::GpuCompactRenderStats gpu_compact_stats_{};
};
