#pragma once

#include <SDL3/SDL_gpu.h>

#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

class GpuFrameGraph {
public:
    struct ExecuteContext;

    enum class PassType {
        Render,
        Copy,
        Compute
    };

    struct ResourceDependency {
        enum class Source {
            FrameGraph,
            ExternalInput
        };

        std::string name;
        bool write = false;
        Source source = Source::FrameGraph;

        static ResourceDependency read(std::string resource_name) {
            return ResourceDependency{std::move(resource_name), false, Source::FrameGraph};
        }

        static ResourceDependency write_resource(std::string resource_name) {
            return ResourceDependency{std::move(resource_name), true, Source::FrameGraph};
        }

        static ResourceDependency imported_read(std::string resource_name) {
            return ResourceDependency{std::move(resource_name), false, Source::ExternalInput};
        }
    };

    struct RenderPassPayload {
        struct SampledTextureBinding {
            std::string texture;
            std::string sampler;
        };

        std::string pipeline_id;
        std::uint32_t render_state_key = 0;
        std::string color_target;
        bool use_swapchain_target = false;
        SDL_FColor clear_color{0.0f, 0.0f, 0.0f, 0.0f};
        SDL_GPULoadOp load_op = SDL_GPU_LOADOP_CLEAR;
        SDL_GPUStoreOp store_op = SDL_GPU_STOREOP_STORE;
        std::uint32_t vertex_count = 3;
        std::uint32_t instance_count = 1;
        std::vector<SampledTextureBinding> fragment_sampled_textures{};
        std::function<bool(const ExecuteContext&,
                           SDL_GPURenderPass*,
                           std::string&)> custom_render{};
        bool execute_default_draw = true;
    };

    struct ComputePassPayload {
        std::string pipeline_id;
        std::uint32_t render_state_key = 0;
        std::vector<std::string> rw_buffer_bindings{};
        std::uint32_t dispatch_x = 1;
        std::uint32_t dispatch_y = 1;
        std::uint32_t dispatch_z = 1;
    };

    struct BlitPassPayload {
        std::string source_texture;
        std::string destination_texture;
        bool use_swapchain_destination = false;
        SDL_GPULoadOp load_op = SDL_GPU_LOADOP_DONT_CARE;
        SDL_GPUFilter filter = SDL_GPU_FILTER_LINEAR;
        std::uint32_t width = 0;
        std::uint32_t height = 0;
    };

    struct PassDescriptor {
        PassType type = PassType::Render;
        std::string name{};
        std::vector<ResourceDependency> resources{};
        RenderPassPayload render{};
        ComputePassPayload compute{};
        BlitPassPayload blit{};
    };

    struct ExecuteContext {
        SDL_GPUCommandBuffer* command_buffer = nullptr;
        SDL_GPUTexture* swapchain_texture = nullptr;
        std::uint32_t swapchain_width = 0;
        std::uint32_t swapchain_height = 0;
        std::function<SDL_GPUTexture*(const std::string&)> resolve_texture{};
        std::function<SDL_GPUSampler*(const std::string&)> resolve_sampler{};
        std::function<SDL_GPUGraphicsPipeline*(const std::string&, std::uint32_t)> resolve_graphics_pipeline{};
        std::function<SDL_GPUComputePipeline*(const std::string&, std::uint32_t)> resolve_compute_pipeline{};
        std::function<SDL_GPUBuffer*(const std::string&)> resolve_buffer{};
    };

    struct ExecuteOptions {
        bool strict_resource_validation = false;
        bool fail_on_validation_error = false;
        bool fail_on_missing_resource = true;
        bool fail_on_missing_pipeline = true;
        bool dry_run = false;
    };

    struct ExecutionStats {
        bool success = true;
        std::string error_message{};
        std::uint32_t render_pass_count = 0;
        std::uint32_t copy_pass_count = 0;
        std::uint32_t compute_pass_count = 0;
        std::uint32_t draw_call_count = 0;
        std::uint32_t dependency_warning_count = 0;
        std::uint32_t dependency_error_count = 0;
    };

    void reset();
    void add_pass(PassDescriptor pass);
    ExecutionStats execute(const ExecuteContext& context,
                           const ExecuteOptions& options = ExecuteOptions{}) const;
    const ExecutionStats& last_execution_stats() const { return last_execution_stats_; }

private:
    bool fail_or_warn_missing(const std::string& message,
                              bool fail,
                              bool is_validation_error,
                              ExecutionStats& stats) const;

    std::vector<PassDescriptor> passes_{};
    mutable ExecutionStats last_execution_stats_{};
};
