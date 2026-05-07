#include <doctest/doctest.h>

#include <string>
#include <utility>

#include "rendering/render/gpu_frame_graph.hpp"

namespace {

GpuFrameGraph::PassDescriptor make_render_write_pass(std::string name,
                                                     std::string resource,
                                                     std::string pipeline_id) {
    GpuFrameGraph::PassDescriptor pass{};
    pass.type = GpuFrameGraph::PassType::Render;
    pass.name = std::move(name);
    pass.resources = {GpuFrameGraph::ResourceDependency::write_resource(resource)};
    pass.render.pipeline_id = std::move(pipeline_id);
    pass.render.color_target = pass.resources.front().name;
    pass.render.load_op = SDL_GPU_LOADOP_CLEAR;
    pass.render.store_op = SDL_GPU_STOREOP_STORE;
    pass.render.clear_color = SDL_FColor{0.0f, 0.0f, 0.0f, 1.0f};
    return pass;
}

GpuFrameGraph::PassDescriptor make_final_swapchain_pass(std::string name,
                                                         std::string source_resource) {
    GpuFrameGraph::PassDescriptor pass{};
    pass.type = GpuFrameGraph::PassType::Render;
    pass.name = std::move(name);
    pass.resources = {
        GpuFrameGraph::ResourceDependency::read(source_resource),
        GpuFrameGraph::ResourceDependency::write_resource("scene.swapchain"),
    };
    pass.render.use_swapchain_target = true;
    pass.render.pipeline_id = "sprite_textured";
    pass.render.load_op = SDL_GPU_LOADOP_CLEAR;
    pass.render.store_op = SDL_GPU_STOREOP_STORE;
    pass.render.clear_color = SDL_FColor{0.0f, 0.0f, 0.0f, 1.0f};
    pass.render.fragment_sampled_textures = {
        GpuFrameGraph::RenderPassPayload::SampledTextureBinding{std::move(source_resource), "linear_clamp"},
    };
    return pass;
}

} // namespace

TEST_CASE("GpuFrameGraph strict validation fails on read-before-write dependency") {
    GpuFrameGraph graph;
    GpuFrameGraph::PassDescriptor read_pass{};
    read_pass.type = GpuFrameGraph::PassType::Copy;
    read_pass.name = "read_uninitialized";
    read_pass.resources = {
        GpuFrameGraph::ResourceDependency::read("scene.output"),
        GpuFrameGraph::ResourceDependency::write_resource("scene.temp")
    };
    graph.add_pass(std::move(read_pass));

    GpuFrameGraph::PassDescriptor write_present_source{};
    write_present_source.type = GpuFrameGraph::PassType::Copy;
    write_present_source.name = "write_present_source";
    write_present_source.resources = {
        GpuFrameGraph::ResourceDependency::imported_read("history.external_color"),
        GpuFrameGraph::ResourceDependency::write_resource("scene.present_source")
    };
    graph.add_pass(std::move(write_present_source));
    graph.add_pass(make_final_swapchain_pass("present_scene", "scene.present_source"));

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
    read_pass.type = GpuFrameGraph::PassType::Copy;
    read_pass.name = "read_uninitialized";
    read_pass.resources = {
        GpuFrameGraph::ResourceDependency::read("scene.output"),
        GpuFrameGraph::ResourceDependency::write_resource("scene.temp")
    };
    graph.add_pass(std::move(read_pass));
    GpuFrameGraph::PassDescriptor write_pass{};
    write_pass.type = GpuFrameGraph::PassType::Copy;
    write_pass.name = "write_after_read";
    write_pass.resources = {
        GpuFrameGraph::ResourceDependency::imported_read("history.external_color"),
        GpuFrameGraph::ResourceDependency::write_resource("scene.output")
    };
    graph.add_pass(std::move(write_pass));
    graph.add_pass(make_final_swapchain_pass("present_scene", "scene.output"));

    GpuFrameGraph::ExecuteContext context{};
    GpuFrameGraph::ExecuteOptions options{};
    options.strict_resource_validation = false;
    options.fail_on_validation_error = false;
    options.dry_run = true;
    const GpuFrameGraph::ExecutionStats stats = graph.execute(context, options);
    CHECK(stats.success);
    CHECK(stats.render_pass_count == 1);
    CHECK(stats.copy_pass_count == 2);
    CHECK(stats.dependency_warning_count == 1);
    CHECK(stats.dependency_error_count == 0);
}

    TEST_CASE("GpuFrameGraph strict validation accepts identical startup and runtime authoritative topology") {
    const auto execute_authoritative_sequence = [](const std::string& prefix) {
        GpuFrameGraph graph;

        GpuFrameGraph::PassDescriptor floor_base_pass{};
        floor_base_pass.type = GpuFrameGraph::PassType::Render;
        floor_base_pass.name = prefix + "_render_floor_base";
        floor_base_pass.resources = {GpuFrameGraph::ResourceDependency::write_resource("scene.floor")};
        floor_base_pass.render.pipeline_id = "floor_compose";
        floor_base_pass.render.color_target = "scene.floor";
        floor_base_pass.render.load_op = SDL_GPU_LOADOP_CLEAR;
        floor_base_pass.render.store_op = SDL_GPU_STOREOP_STORE;
        graph.add_pass(std::move(floor_base_pass));

        GpuFrameGraph::PassDescriptor floor_tiles_pass{};
        floor_tiles_pass.type = GpuFrameGraph::PassType::Render;
        floor_tiles_pass.name = prefix + "_render_floor_tiles";
        floor_tiles_pass.resources = {GpuFrameGraph::ResourceDependency::write_resource("scene.floor")};
        floor_tiles_pass.render.pipeline_id = "sprite_batched";
        floor_tiles_pass.render.color_target = "scene.floor";
        floor_tiles_pass.render.load_op = SDL_GPU_LOADOP_LOAD;
        floor_tiles_pass.render.store_op = SDL_GPU_STOREOP_STORE;
        floor_tiles_pass.render.execute_default_draw = false;
        graph.add_pass(std::move(floor_tiles_pass));

        GpuFrameGraph::PassDescriptor floor_sprites_pass{};
        floor_sprites_pass.type = GpuFrameGraph::PassType::Render;
        floor_sprites_pass.name = prefix + "_render_floor_sprites";
        floor_sprites_pass.resources = {GpuFrameGraph::ResourceDependency::write_resource("scene.floor")};
        floor_sprites_pass.render.pipeline_id = "sprite_batched";
        floor_sprites_pass.render.color_target = "scene.floor";
        floor_sprites_pass.render.load_op = SDL_GPU_LOADOP_LOAD;
        floor_sprites_pass.render.store_op = SDL_GPU_STOREOP_STORE;
        floor_sprites_pass.render.execute_default_draw = false;
        graph.add_pass(std::move(floor_sprites_pass));

        GpuFrameGraph::PassDescriptor layers_pass{};
        layers_pass.type = GpuFrameGraph::PassType::Render;
        layers_pass.name = prefix + "_render_layers";
        layers_pass.resources = {GpuFrameGraph::ResourceDependency::write_resource("scene.layers")};
        layers_pass.render.pipeline_id = "sprite_batched";
        layers_pass.render.color_target = "scene.layers";
        layers_pass.render.load_op = SDL_GPU_LOADOP_CLEAR;
        layers_pass.render.store_op = SDL_GPU_STOREOP_STORE;
        layers_pass.render.execute_default_draw = false;
        graph.add_pass(std::move(layers_pass));

        GpuFrameGraph::PassDescriptor composite_pass{};
        composite_pass.type = GpuFrameGraph::PassType::Render;
        composite_pass.name = prefix + "_render_scene_composite";
        composite_pass.resources = {
            GpuFrameGraph::ResourceDependency::read("scene.floor"),
            GpuFrameGraph::ResourceDependency::read("scene.layers"),
            GpuFrameGraph::ResourceDependency::write_resource("scene.composite"),
        };
        composite_pass.render.pipeline_id = "final_compose";
        composite_pass.render.color_target = "scene.composite";
        composite_pass.render.load_op = SDL_GPU_LOADOP_CLEAR;
        composite_pass.render.store_op = SDL_GPU_STOREOP_STORE;
        composite_pass.render.fragment_sampled_textures = {
            GpuFrameGraph::RenderPassPayload::SampledTextureBinding{"scene.floor", "linear_clamp"},
            GpuFrameGraph::RenderPassPayload::SampledTextureBinding{"scene.layers", "linear_clamp"},
        };
        graph.add_pass(std::move(composite_pass));

        GpuFrameGraph::PassDescriptor present_pass{};
        present_pass.type = GpuFrameGraph::PassType::Render;
        present_pass.name = prefix + "_present_scene_composite";
        present_pass.resources = {
            GpuFrameGraph::ResourceDependency::read("scene.composite"),
            GpuFrameGraph::ResourceDependency::write_resource("scene.swapchain"),
        };
        present_pass.render.use_swapchain_target = true;
        present_pass.render.load_op = SDL_GPU_LOADOP_CLEAR;
        present_pass.render.store_op = SDL_GPU_STOREOP_STORE;
        present_pass.render.pipeline_id = "sprite_textured";
        present_pass.render.fragment_sampled_textures = {
            GpuFrameGraph::RenderPassPayload::SampledTextureBinding{"scene.composite", "linear_clamp"},
        };
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
    graph.add_pass(make_final_swapchain_pass("present_scene", "scene.composite"));

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

    graph.add_pass(make_render_write_pass("runtime_validation_write_other",
                                          "scene.other",
                                          "sprite_textured"));

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

TEST_CASE("GpuFrameGraph rejects read/write texture alias in same pass") {
    GpuFrameGraph graph;

    GpuFrameGraph::PassDescriptor pass{};
    pass.type = GpuFrameGraph::PassType::Render;
    pass.name = "alias_read_write_same_texture";
    pass.resources = {
        GpuFrameGraph::ResourceDependency::read("scene.composite"),
        GpuFrameGraph::ResourceDependency::write_resource("scene.composite"),
    };
    pass.render.pipeline_id = "final_compose";
    pass.render.color_target = "scene.composite";
    pass.render.fragment_sampled_textures = {
        GpuFrameGraph::RenderPassPayload::SampledTextureBinding{"scene.composite", "linear_clamp"},
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
    CHECK(stats.error_message.find("reads and writes texture") != std::string::npos);
}

TEST_CASE("GpuFrameGraph requires final swapchain pass contract") {
    GpuFrameGraph graph;

    GpuFrameGraph::PassDescriptor floor_pass{};
    floor_pass.type = GpuFrameGraph::PassType::Render;
    floor_pass.name = "render_floor";
    floor_pass.resources = {GpuFrameGraph::ResourceDependency::write_resource("scene.floor")};
    floor_pass.render.pipeline_id = "floor_compose";
    floor_pass.render.color_target = "scene.floor";
    graph.add_pass(std::move(floor_pass));

    GpuFrameGraph::PassDescriptor present_pass{};
    present_pass.type = GpuFrameGraph::PassType::Render;
    present_pass.name = "present_scene";
    present_pass.resources = {
        GpuFrameGraph::ResourceDependency::read("scene.floor"),
    };
    present_pass.render.use_swapchain_target = true;
    present_pass.render.pipeline_id = "sprite_textured";
    present_pass.render.fragment_sampled_textures = {
        GpuFrameGraph::RenderPassPayload::SampledTextureBinding{"scene.floor", "linear_clamp"},
    };
    graph.add_pass(std::move(present_pass));

    GpuFrameGraph::ExecuteContext context{};
    GpuFrameGraph::ExecuteOptions options{};
    options.strict_resource_validation = true;
    options.fail_on_validation_error = true;
    options.dry_run = true;

    const GpuFrameGraph::ExecutionStats stats = graph.execute(context, options);
    CHECK_FALSE(stats.success);
    CHECK(stats.dependency_error_count == 1);
    CHECK(stats.error_message.find("write dependency on 'scene.swapchain'") != std::string::npos);
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
    pass.render.color_target = "scene.composite";
    pass.render.load_op = SDL_GPU_LOADOP_CLEAR;
    pass.render.store_op = SDL_GPU_STOREOP_STORE;
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

    graph.add_pass(make_render_write_pass("write_scene_layers",
                                          "scene.layers",
                                          "sprite_textured"));

    GpuFrameGraph::PassDescriptor pass{};
    pass.type = GpuFrameGraph::PassType::Render;
    pass.name = "empty_sampler_binding";
    pass.resources = {
        GpuFrameGraph::ResourceDependency::read("scene.layers"),
        GpuFrameGraph::ResourceDependency::write_resource("scene.composite"),
    };
    pass.render.pipeline_id = "final_compose";
    pass.render.color_target = "scene.composite";
    pass.render.load_op = SDL_GPU_LOADOP_CLEAR;
    pass.render.store_op = SDL_GPU_STOREOP_STORE;
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

TEST_CASE("GpuFrameGraph requires exactly one final swapchain render pass") {
    GpuFrameGraph graph;

    graph.add_pass(make_render_write_pass("write_scene_floor", "scene.floor", "floor_compose"));
    graph.add_pass(make_render_write_pass("write_scene_layers", "scene.layers", "sprite_batched"));

    GpuFrameGraph::PassDescriptor compose_pass{};
    compose_pass.type = GpuFrameGraph::PassType::Render;
    compose_pass.name = "compose_scene";
    compose_pass.resources = {
        GpuFrameGraph::ResourceDependency::read("scene.floor"),
        GpuFrameGraph::ResourceDependency::read("scene.layers"),
        GpuFrameGraph::ResourceDependency::write_resource("scene.composite"),
    };
    compose_pass.render.pipeline_id = "final_compose";
    compose_pass.render.color_target = "scene.composite";
    compose_pass.render.fragment_sampled_textures = {
        GpuFrameGraph::RenderPassPayload::SampledTextureBinding{"scene.floor", "linear_clamp"},
        GpuFrameGraph::RenderPassPayload::SampledTextureBinding{"scene.layers", "linear_clamp"},
    };
    graph.add_pass(std::move(compose_pass));

    graph.add_pass(make_final_swapchain_pass("present_scene_once", "scene.composite"));
    graph.add_pass(make_final_swapchain_pass("present_scene_twice", "scene.composite"));

    GpuFrameGraph::ExecuteContext context{};
    GpuFrameGraph::ExecuteOptions options{};
    options.strict_resource_validation = true;
    options.fail_on_validation_error = true;
    options.dry_run = true;

    const GpuFrameGraph::ExecutionStats stats = graph.execute(context, options);
    CHECK_FALSE(stats.success);
    CHECK(stats.error_message.find("Expected exactly one final swapchain render pass") != std::string::npos);
}
