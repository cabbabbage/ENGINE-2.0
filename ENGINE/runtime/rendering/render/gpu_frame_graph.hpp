#pragma once

#include <functional>
#include <cstdint>
#include <string>
#include <vector>

class GpuFrameGraph {
public:
    using PassCallback = std::function<void()>;

    struct ExecutionStats {
        std::uint32_t render_pass_count = 0;
        std::uint32_t copy_pass_count = 0;
        std::uint32_t compute_pass_count = 0;
    };

    void reset();
    void add_render_pass(std::string name, PassCallback callback);
    void add_copy_pass(std::string name, PassCallback callback);
    void add_compute_pass(std::string name, PassCallback callback);
    ExecutionStats execute() const;
    const ExecutionStats& last_execution_stats() const { return last_execution_stats_; }

private:
    enum class PassType {
        Render,
        Copy,
        Compute
    };

    struct Pass {
        PassType type = PassType::Render;
        std::string name;
        PassCallback callback;
    };

    std::vector<Pass> passes_;
    mutable ExecutionStats last_execution_stats_{};
};
