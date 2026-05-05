#pragma once

#include <cstdint>
#include <string>
#include <string_view>

class GpuSceneRenderer;

class GpuRuntimePipeline {
public:
    bool ensure_resources(GpuSceneRenderer& renderer,
                          std::uint32_t width,
                          std::uint32_t height,
                          std::string& out_error) const;
    bool ensure_shared_resources(GpuSceneRenderer& renderer, std::string& out_error) const;
    void enqueue_frame_graph(GpuSceneRenderer& renderer,
                             std::string_view pass_name_prefix,
                             std::uint32_t width,
                             std::uint32_t height) const;
};
