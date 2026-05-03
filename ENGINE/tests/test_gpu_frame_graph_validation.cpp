#include <doctest/doctest.h>

#include <string>
#include <utility>

#include "rendering/render/gpu_frame_graph.hpp"

TEST_CASE("GpuFrameGraph strict validation fails on read-before-write dependency") {
    GpuFrameGraph graph;
    GpuFrameGraph::PassDescriptor read_pass{};
    read_pass.type = GpuFrameGraph::PassType::Render;
    read_pass.name = "read_uninitialized";
    read_pass.resources = {GpuFrameGraph::ResourceDependency{"scene.output", false}};
    graph.add_pass(std::move(read_pass));

    GpuFrameGraph::ExecuteContext context{};
    GpuFrameGraph::ExecuteOptions options{};
    options.strict_resource_validation = true;
    options.fail_on_validation_error = true;
    options.dry_run = true;
    const GpuFrameGraph::ExecutionStats stats = graph.execute(context, options);
    CHECK_FALSE(stats.success);
    CHECK(stats.dependency_error_count == 1);
    CHECK(stats.error_message.find("reads resource") != std::string::npos);
}

TEST_CASE("GpuFrameGraph non-strict validation warns and continues") {
    GpuFrameGraph graph;
    GpuFrameGraph::PassDescriptor read_pass{};
    read_pass.type = GpuFrameGraph::PassType::Render;
    read_pass.name = "read_uninitialized";
    read_pass.resources = {GpuFrameGraph::ResourceDependency{"scene.output", false}};
    graph.add_pass(std::move(read_pass));
    GpuFrameGraph::PassDescriptor write_pass{};
    write_pass.type = GpuFrameGraph::PassType::Render;
    write_pass.name = "write_after_read";
    write_pass.resources = {GpuFrameGraph::ResourceDependency{"scene.output", true}};
    graph.add_pass(std::move(write_pass));

    GpuFrameGraph::ExecuteContext context{};
    GpuFrameGraph::ExecuteOptions options{};
    options.strict_resource_validation = false;
    options.fail_on_validation_error = false;
    options.dry_run = true;
    const GpuFrameGraph::ExecutionStats stats = graph.execute(context, options);
    CHECK(stats.success);
    CHECK(stats.render_pass_count == 2);
    CHECK(stats.dependency_warning_count == 1);
    CHECK(stats.dependency_error_count == 0);
}

TEST_CASE("GpuFrameGraph strict validation accepts startup probe write-then-copy ordering") {
    GpuFrameGraph graph;

    GpuFrameGraph::PassDescriptor write_pass{};
    write_pass.type = GpuFrameGraph::PassType::Render;
    write_pass.name = "startup_probe_write_scene_composite";
    write_pass.resources = {GpuFrameGraph::ResourceDependency{"scene.composite", true}};
    graph.add_pass(std::move(write_pass));

    GpuFrameGraph::PassDescriptor copy_pass{};
    copy_pass.type = GpuFrameGraph::PassType::Copy;
    copy_pass.name = "startup_probe_copy_scene_composite";
    copy_pass.resources = {
        GpuFrameGraph::ResourceDependency{"scene.composite", false},
        GpuFrameGraph::ResourceDependency{"scene.frame_graph_copy", true}
    };
    copy_pass.blit.source_texture = "scene.composite";
    copy_pass.blit.destination_texture = "scene.frame_graph_copy";
    graph.add_pass(std::move(copy_pass));

    GpuFrameGraph::ExecuteContext context{};
    GpuFrameGraph::ExecuteOptions options{};
    options.strict_resource_validation = true;
    options.fail_on_validation_error = true;
    options.dry_run = true;
    const GpuFrameGraph::ExecutionStats stats = graph.execute(context, options);
    CHECK(stats.success);
    CHECK(stats.dependency_error_count == 0);
}
