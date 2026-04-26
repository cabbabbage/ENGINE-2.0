#include "rendering/render/gpu_frame_graph.hpp"

#include "rendering/render/render_diagnostics.hpp"
#include "utils/log.hpp"

#include <unordered_map>
#include <utility>

void GpuFrameGraph::reset() {
    passes_.clear();
    last_execution_stats_ = ExecutionStats{};
}

void GpuFrameGraph::add_pass(PassDescriptor pass) {
    passes_.push_back(std::move(pass));
}

void GpuFrameGraph::add_render_pass(std::string name,
                                    PassCallback callback,
                                    std::vector<ResourceDependency> resources) {
    add_pass(PassDescriptor{
        PassType::Render,
        std::move(name),
        std::move(callback),
        std::move(resources),
        {},
        0,
        1,
        1,
        1});
}

void GpuFrameGraph::add_copy_pass(std::string name,
                                  PassCallback callback,
                                  std::vector<ResourceDependency> resources) {
    add_pass(PassDescriptor{
        PassType::Copy,
        std::move(name),
        std::move(callback),
        std::move(resources),
        {},
        0,
        1,
        1,
        1});
}

void GpuFrameGraph::add_compute_pass(std::string name,
                                     PassCallback callback,
                                     std::vector<ResourceDependency> resources) {
    add_pass(PassDescriptor{
        PassType::Compute,
        std::move(name),
        std::move(callback),
        std::move(resources),
        {},
        0,
        1,
        1,
        1});
}

GpuFrameGraph::ExecutionStats GpuFrameGraph::execute(const ExecuteOptions& options) const {
    ExecutionStats stats{};
    std::unordered_map<std::string, std::size_t> resource_last_writer{};
    for (const PassDescriptor& pass : passes_) {
        switch (pass.type) {
        case PassType::Render:
            render_diagnostics::add_render_pass();
            ++stats.render_pass_count;
            break;
        case PassType::Copy:
            render_diagnostics::add_copy_pass();
            ++stats.copy_pass_count;
            break;
        case PassType::Compute:
            render_diagnostics::add_compute_pass();
            ++stats.compute_pass_count;
            break;
        }

        for (const ResourceDependency& dep : pass.resources) {
            if (dep.name.empty()) {
                continue;
            }
            if (!dep.write && resource_last_writer.find(dep.name) == resource_last_writer.end()) {
                const std::string message = "[GpuFrameGraph] Pass '" + pass.name +
                                            "' reads resource '" + dep.name +
                                            "' before any writer pass.";
                if (options.strict_resource_validation) {
                    ++stats.dependency_error_count;
                    vibble::log::error(message);
                    if (options.fail_on_validation_error) {
                        stats.success = false;
                        stats.error_message = message;
                        last_execution_stats_ = stats;
                        return stats;
                    }
                } else {
                    ++stats.dependency_warning_count;
                    vibble::log::warn(message);
                }
            }
            if (dep.write) {
                resource_last_writer[dep.name] = static_cast<std::size_t>(&pass - passes_.data());
            }
        }

        if (pass.callback) {
            pass.callback();
        }
    }
    stats.success = true;
    last_execution_stats_ = stats;
    return stats;
}
