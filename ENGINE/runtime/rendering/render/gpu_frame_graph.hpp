#pragma once

#include <functional>
#include <string>
#include <vector>

class GpuFrameGraph {
public:
    using PassCallback = std::function<void()>;

    void reset();
    void add_render_pass(std::string name, PassCallback callback);
    void add_copy_pass(std::string name, PassCallback callback);
    void add_compute_pass(std::string name, PassCallback callback);
    void execute() const;

private:
    struct Pass {
        std::string name;
        PassCallback callback;
    };

    std::vector<Pass> render_passes_;
    std::vector<Pass> copy_passes_;
    std::vector<Pass> compute_passes_;
};
