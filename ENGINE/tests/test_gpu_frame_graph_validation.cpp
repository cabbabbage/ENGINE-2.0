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

TEST_CASE("GpuFrameGraph strict validation fails when resource is allocated but never written this execution") {
    GpuFrameGraph graph;
    GpuFrameGraph::PassDescriptor read_pass{};
    read_pass.type = GpuFrameGraph::PassType::Copy;
    read_pass.name = "read_allocated_never_written";
    read_pass.resources = {
        GpuFrameGraph::ResourceDependency::read("scene.allocated_only"),
        GpuFrameGraph::ResourceDependency::write_resource("scene.copy_destination")
    };
    graph.add_pass(std::move(read_pass));

    GpuFrameGraph::ExecuteContext context{};
    GpuFrameGraph::ExecuteOptions options{};
    options.strict_resource_validation = true;
    options.fail_on_validation_error = true;
    options.dry_run = true;
    const GpuFrameGraph::ExecutionStats stats = graph.execute(context, options);
    CHECK_FALSE(stats.success);
    CHECK(stats.dependency_error_count == 1);
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

TEST_CASE("GpuFrameGraph strict validation allows explicitly imported external reads") {
    GpuFrameGraph graph;

    GpuFrameGraph::PassDescriptor imported_read_pass{};
    imported_read_pass.type = GpuFrameGraph::PassType::Copy;
    imported_read_pass.name = "copy_from_external_input";
    imported_read_pass.resources = {
        GpuFrameGraph::ResourceDependency::imported_read("history.external_color"),
        GpuFrameGraph::ResourceDependency::write_resource("scene.composite")
    };
    graph.add_pass(std::move(imported_read_pass));

    GpuFrameGraph::ExecuteContext context{};
    GpuFrameGraph::ExecuteOptions options{};
    options.strict_resource_validation = true;
    options.fail_on_validation_error = true;
    options.dry_run = true;
    const GpuFrameGraph::ExecutionStats stats = graph.execute(context, options);
    CHECK(stats.success);
    CHECK(stats.dependency_error_count == 0);
    CHECK(stats.dependency_warning_count == 0);
}
