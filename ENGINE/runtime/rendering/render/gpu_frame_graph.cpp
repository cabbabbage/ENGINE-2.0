#include "rendering/render/gpu_frame_graph.hpp"

#include "rendering/render/render_diagnostics.hpp"

void GpuFrameGraph::reset() {
    render_passes_.clear();
    copy_passes_.clear();
    compute_passes_.clear();
}

void GpuFrameGraph::add_render_pass(std::string name, PassCallback callback) {
    render_passes_.push_back(Pass{std::move(name), std::move(callback)});
}

void GpuFrameGraph::add_copy_pass(std::string name, PassCallback callback) {
    copy_passes_.push_back(Pass{std::move(name), std::move(callback)});
}

void GpuFrameGraph::add_compute_pass(std::string name, PassCallback callback) {
    compute_passes_.push_back(Pass{std::move(name), std::move(callback)});
}

void GpuFrameGraph::execute() const {
    for (const Pass& pass : render_passes_) {
        render_diagnostics::add_render_pass();
        if (pass.callback) {
            pass.callback();
        }
    }
    for (const Pass& pass : copy_passes_) {
        render_diagnostics::add_copy_pass();
        if (pass.callback) {
            pass.callback();
        }
    }
    for (const Pass& pass : compute_passes_) {
        render_diagnostics::add_compute_pass();
        if (pass.callback) {
            pass.callback();
        }
    }
}
