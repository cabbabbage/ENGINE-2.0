#include <doctest/doctest.h>

#include <string>

#include "rendering/render/gpu_frame_graph.hpp"

TEST_CASE("GpuFrameGraph strict validation fails on read-before-write dependency") {
    GpuFrameGraph graph;
    graph.add_render_pass("read_uninitialized",
                          []() {},
                          {GpuFrameGraph::ResourceDependency{"scene.output", false}});

    GpuFrameGraph::ExecuteOptions options{};
    options.strict_resource_validation = true;
    options.fail_on_validation_error = true;
    const GpuFrameGraph::ExecutionStats stats = graph.execute(options);
    CHECK_FALSE(stats.success);
    CHECK(stats.dependency_error_count == 1);
    CHECK(stats.error_message.find("reads resource") != std::string::npos);
}

TEST_CASE("GpuFrameGraph non-strict validation warns and continues") {
    GpuFrameGraph graph;
    graph.add_render_pass("read_uninitialized",
                          []() {},
                          {GpuFrameGraph::ResourceDependency{"scene.output", false}});
    graph.add_render_pass("write_after_read",
                          []() {},
                          {GpuFrameGraph::ResourceDependency{"scene.output", true}});

    GpuFrameGraph::ExecuteOptions options{};
    options.strict_resource_validation = false;
    options.fail_on_validation_error = false;
    const GpuFrameGraph::ExecutionStats stats = graph.execute(options);
    CHECK(stats.success);
    CHECK(stats.render_pass_count == 2);
    CHECK(stats.dependency_warning_count == 1);
    CHECK(stats.dependency_error_count == 0);
}
