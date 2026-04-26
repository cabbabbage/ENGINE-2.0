#include "rendering/render/gpu_frame_graph.hpp"

#include "rendering/render/render_diagnostics.hpp"

void GpuFrameGraph::reset() {
    passes_.clear();
    last_execution_stats_ = ExecutionStats{};
}

void GpuFrameGraph::add_render_pass(std::string name, PassCallback callback) {
    passes_.push_back(Pass{PassType::Render, std::move(name), std::move(callback)});
}

void GpuFrameGraph::add_copy_pass(std::string name, PassCallback callback) {
    passes_.push_back(Pass{PassType::Copy, std::move(name), std::move(callback)});
}

void GpuFrameGraph::add_compute_pass(std::string name, PassCallback callback) {
    passes_.push_back(Pass{PassType::Compute, std::move(name), std::move(callback)});
}

GpuFrameGraph::ExecutionStats GpuFrameGraph::execute() const {
    ExecutionStats stats{};
    for (const Pass& pass : passes_) {
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
        if (pass.callback) {
            pass.callback();
        }
    }
    last_execution_stats_ = stats;
    return stats;
}
