#pragma once

#include <functional>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

class GpuFrameGraph {
public:
    using PassCallback = std::function<void()>;
    enum class PassType {
        Render,
        Copy,
        Compute
    };

    struct ResourceDependency {
        std::string name;
        bool write = false;
    };

    struct PassDescriptor {
        PassType type = PassType::Render;
        std::string name;
        PassCallback callback;
        std::vector<ResourceDependency> resources;
        std::string pipeline_id;
        std::uint32_t draw_call_count = 0;
        std::uint32_t dispatch_x = 1;
        std::uint32_t dispatch_y = 1;
        std::uint32_t dispatch_z = 1;
    };

    struct ExecuteOptions {
        bool strict_resource_validation = false;
        bool fail_on_validation_error = false;
    };

    struct ExecutionStats {
        bool success = true;
        std::string error_message;
        std::uint32_t render_pass_count = 0;
        std::uint32_t copy_pass_count = 0;
        std::uint32_t compute_pass_count = 0;
        std::uint32_t dependency_warning_count = 0;
        std::uint32_t dependency_error_count = 0;
    };

    void reset();
    void add_pass(PassDescriptor pass);
    void add_render_pass(std::string name,
                         PassCallback callback,
                         std::vector<ResourceDependency> resources = {});
    void add_copy_pass(std::string name,
                       PassCallback callback,
                       std::vector<ResourceDependency> resources = {});
    void add_compute_pass(std::string name,
                          PassCallback callback,
                          std::vector<ResourceDependency> resources = {});
    ExecutionStats execute(const ExecuteOptions& options = ExecuteOptions{}) const;
    const ExecutionStats& last_execution_stats() const { return last_execution_stats_; }

private:
    std::vector<PassDescriptor> passes_;
    mutable ExecutionStats last_execution_stats_{};
};
