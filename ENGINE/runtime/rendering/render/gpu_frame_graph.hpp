#pragma once

#include <functional>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

class GpuFrameGraph {
public:
    using PassCallback = std::function<void()>;

    struct ResourceDependency {
        std::string name;
        bool write = false;
    };

    struct ExecutionStats {
        std::uint32_t render_pass_count = 0;
        std::uint32_t copy_pass_count = 0;
        std::uint32_t compute_pass_count = 0;
        std::uint32_t dependency_warning_count = 0;
    };

    void reset();
    void add_render_pass(std::string name,
                         PassCallback callback,
                         std::vector<ResourceDependency> resources = {});
    void add_copy_pass(std::string name,
                       PassCallback callback,
                       std::vector<ResourceDependency> resources = {});
    void add_compute_pass(std::string name,
                          PassCallback callback,
                          std::vector<ResourceDependency> resources = {});
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
        std::vector<ResourceDependency> resources;
    };

    std::vector<Pass> passes_;
    mutable ExecutionStats last_execution_stats_{};
};
