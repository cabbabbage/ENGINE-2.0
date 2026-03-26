#pragma once

#include <algorithm>
#include <cstdint>
#include <cstddef>
#include <optional>
#include <unordered_map>
#include <vector>

namespace anchor_binding_order {

struct Node {
    std::uintptr_t id = 0;
    std::optional<std::uintptr_t> depends_on{};
    std::uint64_t sort_key = 0;
};

struct Result {
    std::vector<std::uintptr_t> ordered_ids;
    bool has_cycle = false;
    std::size_t cycle_nodes = 0;
};

inline Result compute(const std::vector<Node>& nodes) {
    Result result{};
    if (nodes.empty()) {
        return result;
    }

    struct GraphNode {
        std::size_t indegree = 0;
        std::vector<std::uintptr_t> outgoing;
    };

    std::unordered_map<std::uintptr_t, GraphNode> graph;
    graph.reserve(nodes.size());

    std::unordered_map<std::uintptr_t, std::uint64_t> sort_keys;
    sort_keys.reserve(nodes.size());

    for (const Node& node : nodes) {
        graph.emplace(node.id, GraphNode{});
        sort_keys[node.id] = node.sort_key;
    }

    for (const Node& node : nodes) {
        if (!node.depends_on.has_value()) {
            continue;
        }
        const auto parent_it = graph.find(*node.depends_on);
        const auto child_it = graph.find(node.id);
        if (parent_it == graph.end() || child_it == graph.end()) {
            continue;
        }
        parent_it->second.outgoing.push_back(node.id);
        ++child_it->second.indegree;
    }

    auto compare_ids = [&](std::uintptr_t lhs, std::uintptr_t rhs) {
        const std::uint64_t lhs_sort = sort_keys.count(lhs) ? sort_keys[lhs] : 0;
        const std::uint64_t rhs_sort = sort_keys.count(rhs) ? sort_keys[rhs] : 0;
        if (lhs_sort != rhs_sort) {
            return lhs_sort < rhs_sort;
        }
        return lhs < rhs;
    };

    std::vector<std::uintptr_t> ready;
    ready.reserve(nodes.size());
    auto push_ready = [&](std::uintptr_t id) {
        const auto pos = std::lower_bound(ready.begin(), ready.end(), id, compare_ids);
        ready.insert(pos, id);
    };

    for (const auto& [id, graph_node] : graph) {
        if (graph_node.indegree == 0) {
            push_ready(id);
        }
    }

    result.ordered_ids.reserve(nodes.size());
    while (!ready.empty()) {
        const std::uintptr_t id = ready.front();
        ready.erase(ready.begin());
        result.ordered_ids.push_back(id);

        auto graph_it = graph.find(id);
        if (graph_it == graph.end()) {
            continue;
        }

        std::vector<std::uintptr_t> outgoing = graph_it->second.outgoing;
        std::sort(outgoing.begin(), outgoing.end(), compare_ids);
        outgoing.erase(std::unique(outgoing.begin(), outgoing.end()), outgoing.end());
        for (const std::uintptr_t child_id : outgoing) {
            auto child_it = graph.find(child_id);
            if (child_it == graph.end() || child_it->second.indegree == 0) {
                continue;
            }
            --child_it->second.indegree;
            if (child_it->second.indegree == 0) {
                push_ready(child_id);
            }
        }
    }

    if (result.ordered_ids.size() == nodes.size()) {
        return result;
    }

    result.has_cycle = true;
    std::unordered_map<std::uintptr_t, bool> seen;
    seen.reserve(result.ordered_ids.size());
    for (const std::uintptr_t id : result.ordered_ids) {
        seen[id] = true;
    }

    std::vector<std::uintptr_t> cycle_nodes;
    cycle_nodes.reserve(nodes.size() - result.ordered_ids.size());
    for (const Node& node : nodes) {
        if (seen.count(node.id) == 0) {
            cycle_nodes.push_back(node.id);
        }
    }
    std::sort(cycle_nodes.begin(), cycle_nodes.end(), compare_ids);
    result.cycle_nodes = cycle_nodes.size();
    result.ordered_ids.insert(result.ordered_ids.end(), cycle_nodes.begin(), cycle_nodes.end());
    return result;
}

} // namespace anchor_binding_order

