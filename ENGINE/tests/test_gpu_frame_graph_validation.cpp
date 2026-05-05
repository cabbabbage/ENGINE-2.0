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

TEST_CASE("GpuFrameGraph strict validation accepts identical startup and runtime authoritative topology") {
    const auto execute_authoritative_sequence = [](const std::string& prefix) {
        GpuFrameGraph graph;

        GpuFrameGraph::PassDescriptor floor_pass{};
        floor_pass.type = GpuFrameGraph::PassType::Render;
        floor_pass.name = prefix + "_render_floor";
        floor_pass.resources = {GpuFrameGraph::ResourceDependency::write_resource("scene.floor")};
        floor_pass.render.pipeline_id = "floor_compose";
        graph.add_pass(std::move(floor_pass));

        GpuFrameGraph::PassDescriptor layers_pass{};
        layers_pass.type = GpuFrameGraph::PassType::Render;
        layers_pass.name = prefix + "_render_layers";
        layers_pass.resources = {GpuFrameGraph::ResourceDependency::write_resource("scene.layers")};
        layers_pass.render.pipeline_id = "sprite_textured";
        graph.add_pass(std::move(layers_pass));

        GpuFrameGraph::PassDescriptor blur_bg_pass{};
        blur_bg_pass.type = GpuFrameGraph::PassType::Render;
        blur_bg_pass.name = prefix + "_render_blur_background";
        blur_bg_pass.resources = {
            GpuFrameGraph::ResourceDependency::read("scene.layers"),
            GpuFrameGraph::ResourceDependency::write_resource("scene.blur_background"),
        };
        blur_bg_pass.render.pipeline_id = "dark_mask";
        blur_bg_pass.render.fragment_sampled_textures = {
            GpuFrameGraph::RenderPassPayload::SampledTextureBinding{"scene.layers", "linear_clamp"},
        };
        graph.add_pass(std::move(blur_bg_pass));

        GpuFrameGraph::PassDescriptor blur_fg_pass{};
        blur_fg_pass.type = GpuFrameGraph::PassType::Render;
        blur_fg_pass.name = prefix + "_render_blur_foreground";
        blur_fg_pass.resources = {
            GpuFrameGraph::ResourceDependency::read("scene.layers"),
            GpuFrameGraph::ResourceDependency::write_resource("scene.blur_foreground"),
        };
        blur_fg_pass.render.pipeline_id = "light_eval";
        blur_fg_pass.render.fragment_sampled_textures = {
            GpuFrameGraph::RenderPassPayload::SampledTextureBinding{"scene.layers", "linear_clamp"},
        };
        graph.add_pass(std::move(blur_fg_pass));

        GpuFrameGraph::PassDescriptor composite_pass{};
        composite_pass.type = GpuFrameGraph::PassType::Render;
        composite_pass.name = prefix + "_render_scene_composite";
        composite_pass.resources = {
            GpuFrameGraph::ResourceDependency::read("scene.floor"),
            GpuFrameGraph::ResourceDependency::read("scene.layers"),
            GpuFrameGraph::ResourceDependency::read("scene.blur_background"),
            GpuFrameGraph::ResourceDependency::read("scene.blur_foreground"),
            GpuFrameGraph::ResourceDependency::write_resource("scene.composite"),
        };
        composite_pass.render.pipeline_id = "final_compose";
        composite_pass.render.fragment_sampled_textures = {
            GpuFrameGraph::RenderPassPayload::SampledTextureBinding{"scene.floor", "linear_clamp"},
            GpuFrameGraph::RenderPassPayload::SampledTextureBinding{"scene.layers", "linear_clamp"},
            GpuFrameGraph::RenderPassPayload::SampledTextureBinding{"scene.blur_background", "linear_clamp"},
            GpuFrameGraph::RenderPassPayload::SampledTextureBinding{"scene.blur_foreground", "linear_clamp"},
        };
        graph.add_pass(std::move(composite_pass));

        GpuFrameGraph::PassDescriptor present_pass{};
        present_pass.type = GpuFrameGraph::PassType::Copy;
        present_pass.name = prefix + "_present_scene_composite";
        present_pass.resources = {
            GpuFrameGraph::ResourceDependency::read("scene.composite"),
            GpuFrameGraph::ResourceDependency::write_resource("scene.swapchain"),
        };
        present_pass.blit.source_texture = "scene.composite";
        present_pass.blit.destination_texture = "scene.swapchain";
        graph.add_pass(std::move(present_pass));

        GpuFrameGraph::ExecuteContext context{};
        GpuFrameGraph::ExecuteOptions options{};
        options.strict_resource_validation = true;
        options.fail_on_validation_error = true;
        options.dry_run = true;
        return graph.execute(context, options);
    };

    const GpuFrameGraph::ExecutionStats startup_stats =
        execute_authoritative_sequence("startup_probe");
    const GpuFrameGraph::ExecutionStats runtime_stats =
        execute_authoritative_sequence("runtime");

    CHECK(startup_stats.success);
    CHECK(runtime_stats.success);
    CHECK(startup_stats.dependency_error_count == 0);
    CHECK(runtime_stats.dependency_error_count == 0);
    CHECK(startup_stats.dependency_warning_count == runtime_stats.dependency_warning_count);
    CHECK(startup_stats.render_pass_count == runtime_stats.render_pass_count);
    CHECK(startup_stats.copy_pass_count == runtime_stats.copy_pass_count);
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

TEST_CASE("GpuFrameGraph strict validation fails immediately when scene.composite is read without write in startup sequence") {
    GpuFrameGraph graph;

    GpuFrameGraph::PassDescriptor copy_pass{};
    copy_pass.type = GpuFrameGraph::PassType::Copy;
    copy_pass.name = "startup_probe_copy_scene_composite";
    copy_pass.resources = {
        GpuFrameGraph::ResourceDependency::read("scene.composite"),
        GpuFrameGraph::ResourceDependency::write_resource("scene.frame_graph_copy")
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
    CHECK_FALSE(stats.success);
    CHECK(stats.dependency_error_count == 1);
    CHECK(stats.render_pass_count == 0);
    CHECK(stats.copy_pass_count == 1);
}

TEST_CASE("GpuFrameGraph strict validation fails immediately when runtime scene.composite writer is missing") {
    GpuFrameGraph graph;

    GpuFrameGraph::PassDescriptor write_other{};
    write_other.type = GpuFrameGraph::PassType::Render;
    write_other.name = "runtime_validation_write_other";
    write_other.resources = {GpuFrameGraph::ResourceDependency::write_resource("scene.other")};
    graph.add_pass(std::move(write_other));

    GpuFrameGraph::PassDescriptor copy_pass{};
    copy_pass.type = GpuFrameGraph::PassType::Copy;
    copy_pass.name = "copy_scene_composite";
    copy_pass.resources = {
        GpuFrameGraph::ResourceDependency::read("scene.composite"),
        GpuFrameGraph::ResourceDependency::write_resource("scene.frame_graph_copy")
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
    CHECK_FALSE(stats.success);
    CHECK(stats.dependency_error_count == 1);
}

TEST_CASE("GpuFrameGraph sampled texture contract requires declared read dependency") {
    GpuFrameGraph graph;

    GpuFrameGraph::PassDescriptor pass{};
    pass.type = GpuFrameGraph::PassType::Render;
    pass.name = "missing_read_dependency_for_sample";
    pass.resources = {
        GpuFrameGraph::ResourceDependency::write_resource("scene.composite"),
    };
    pass.render.pipeline_id = "final_compose";
    pass.render.fragment_sampled_textures = {
        GpuFrameGraph::RenderPassPayload::SampledTextureBinding{"scene.layers", "linear_clamp"},
    };
    graph.add_pass(std::move(pass));

    GpuFrameGraph::ExecuteContext context{};
    GpuFrameGraph::ExecuteOptions options{};
    options.strict_resource_validation = true;
    options.fail_on_validation_error = true;
    options.dry_run = true;

    const GpuFrameGraph::ExecutionStats stats = graph.execute(context, options);
    CHECK_FALSE(stats.success);
    CHECK(stats.dependency_error_count == 1);
    CHECK(stats.error_message.find("without declaring it as a read dependency") != std::string::npos);
}

TEST_CASE("GpuFrameGraph sampled texture contract rejects empty sampler names") {
    GpuFrameGraph graph;

    GpuFrameGraph::PassDescriptor write_input{};
    write_input.type = GpuFrameGraph::PassType::Render;
    write_input.name = "write_scene_layers";
    write_input.resources = {
        GpuFrameGraph::ResourceDependency::write_resource("scene.layers"),
    };
    write_input.render.pipeline_id = "sprite_textured";
    graph.add_pass(std::move(write_input));

    GpuFrameGraph::PassDescriptor pass{};
    pass.type = GpuFrameGraph::PassType::Render;
    pass.name = "empty_sampler_binding";
    pass.resources = {
        GpuFrameGraph::ResourceDependency::read("scene.layers"),
        GpuFrameGraph::ResourceDependency::write_resource("scene.composite"),
    };
    pass.render.pipeline_id = "final_compose";
    pass.render.fragment_sampled_textures = {
        GpuFrameGraph::RenderPassPayload::SampledTextureBinding{"scene.layers", ""},
    };
    graph.add_pass(std::move(pass));

    GpuFrameGraph::ExecuteContext context{};
    GpuFrameGraph::ExecuteOptions options{};
    options.strict_resource_validation = true;
    options.fail_on_validation_error = true;
    options.dry_run = true;

    const GpuFrameGraph::ExecutionStats stats = graph.execute(context, options);
    CHECK_FALSE(stats.success);
    CHECK(stats.dependency_error_count == 1);
    CHECK(stats.error_message.find("empty sampler name") != std::string::npos);
}
