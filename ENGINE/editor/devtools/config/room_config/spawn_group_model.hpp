#pragma once

#include <algorithm>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <nlohmann/json.hpp>

#include "gameplay/spawn/spawn_group_codec.hpp"

namespace vibble::dev_mode::room_config::model {

using SpawnMethodId = std::string;

struct Candidate {
    std::string asset_id;
    float weight = 1.0f;
};

struct MethodConfig {
    struct None {
    };

    struct Random {
    };

    struct Perimeter {
        int min_number = 2;
        int max_number = 2;
    };

    struct Edge {
        int min_number = 1;
        int max_number = 1;
        int inset_percent = 100;
    };

    struct Exact {
        int quantity = 1;
    };

    using Variant = std::variant<None, Random, Perimeter, Edge, Exact>;

    MethodConfig() = default;
    explicit MethodConfig(Variant data) : data(std::move(data)) {}

    static MethodConfig make_none() { return MethodConfig{Variant{None{}}}; }

    static MethodConfig make_random() { return MethodConfig{Variant{Random{}}}; }

    static MethodConfig make_perimeter(int min_number = 2, int max_number = 2) {
        if (max_number < min_number) {
            max_number = min_number;
        }
        return MethodConfig{Variant{Perimeter{min_number, max_number}}};
    }

    static MethodConfig make_edge(int min_number = 1, int max_number = 1, int inset_percent = 100) {
        if (min_number < 1) {
            min_number = 1;
        }
        if (max_number < min_number) {
            max_number = min_number;
        }
        if (inset_percent < 0) inset_percent = 0;
        if (inset_percent > 200) inset_percent = 200;
        return MethodConfig{Variant{Edge{min_number, max_number, inset_percent}}};
    }

    static MethodConfig make_exact(int quantity = 1) {
        return MethodConfig{Variant{Exact{quantity}}};
    }

    None* as_none() { return std::get_if<None>(&data); }
    const None* as_none() const { return std::get_if<None>(&data); }

    Random* as_random() { return std::get_if<Random>(&data); }
    const Random* as_random() const { return std::get_if<Random>(&data); }

    Perimeter* as_perimeter() { return std::get_if<Perimeter>(&data); }
    const Perimeter* as_perimeter() const { return std::get_if<Perimeter>(&data); }

    Edge* as_edge() { return std::get_if<Edge>(&data); }
    const Edge* as_edge() const { return std::get_if<Edge>(&data); }

    Exact* as_exact() { return std::get_if<Exact>(&data); }
    const Exact* as_exact() const { return std::get_if<Exact>(&data); }

    Variant data{None{}};
};

struct SpawnGroup {
    std::string id;
    std::string display_name;
    std::string area_name;
    SpawnMethodId method;
    MethodConfig method_config;
    std::vector<Candidate> candidates;
};

inline void switch_method(SpawnGroup& group, SpawnMethodId method) {
    group.method = vibble::spawn_group_codec::normalize_method(std::move(method));
    if (group.method == "Random") {
        group.method_config = MethodConfig::make_random();
    } else if (group.method == "Perimeter") {
        group.method_config = MethodConfig::make_perimeter();
    } else if (group.method == "Edge") {
        group.method_config = MethodConfig::make_edge();
    } else if (group.method == "Exact") {
        group.method_config = MethodConfig::make_exact();
    } else {
        group.method_config = MethodConfig::make_none();
    }
}

inline float read_candidate_weight(const nlohmann::json& candidate) {
    return static_cast<float>(vibble::spawn_group_codec::read_candidate_chance(candidate, 0.0));
}

inline SpawnGroup spawn_group_from_json(const nlohmann::json& entry) {
    SpawnGroup group{};

    nlohmann::json normalized = entry;
    vibble::spawn_group_codec::EntryDefaults defaults{};
    defaults.display_name = vibble::spawn_group_codec::read_string_field(entry, "display_name");
    if (defaults.display_name.empty()) {
        defaults.display_name = vibble::spawn_group_codec::read_string_field(entry, "name", "Spawn Group");
    }
    vibble::spawn_group_codec::ensure_spawn_group_entry_defaults(normalized, defaults);

    group.id = vibble::spawn_group_codec::read_string_field(normalized, "spawn_id");
    group.display_name = vibble::spawn_group_codec::read_string_field(normalized, "display_name");
    group.area_name = vibble::spawn_group_codec::read_string_field(normalized, "area");
    switch_method(group, vibble::spawn_group_codec::normalize_method_from_entry(normalized));

    if (auto* perimeter = group.method_config.as_perimeter()) {
        perimeter->min_number =
            vibble::spawn_group_codec::read_int_field(normalized, "min_number", perimeter->min_number);
        perimeter->max_number =
            vibble::spawn_group_codec::read_int_field(normalized, "max_number", perimeter->max_number);
    } else if (auto* edge = group.method_config.as_edge()) {
        edge->min_number =
            vibble::spawn_group_codec::read_int_field(normalized, "min_number", edge->min_number);
        edge->max_number =
            vibble::spawn_group_codec::read_int_field(normalized, "max_number", edge->max_number);
        edge->inset_percent = vibble::spawn_group_codec::read_int_field(
            normalized, "edge_inset_percent", edge->inset_percent);
    } else if (auto* exact = group.method_config.as_exact()) {
        exact->quantity = vibble::spawn_group_codec::read_int_field(normalized, "quantity", exact->quantity);
    }

    group.candidates.clear();
    const auto it = normalized.find("candidates");
    if (it != normalized.end() && it->is_array()) {
        for (const auto& candidate : *it) {
            if (!candidate.is_object()) continue;
            Candidate parsed{};
            parsed.asset_id = vibble::spawn_group_codec::read_string_field(candidate, "name");
            parsed.weight = read_candidate_weight(candidate);
            if (!parsed.asset_id.empty() || parsed.weight != 0.0f) {
                group.candidates.push_back(std::move(parsed));
            }
        }
    }

    return group;
}

inline void apply_spawn_group_to_json(const SpawnGroup& group, nlohmann::json& entry) {
    if (!entry.is_object()) {
        entry = nlohmann::json::object();
    }

    entry["spawn_id"] = group.id;
    entry["display_name"] = group.display_name;

    if (!group.area_name.empty()) {
        entry["area"] = group.area_name;
    } else {
        entry.erase("area");
    }

    const std::string method =
        vibble::spawn_group_codec::normalize_method(group.method.empty() ? std::string{"Random"} : group.method);
    entry["position"] = method;
    entry.erase("randomize_y");
    entry.erase("position_y");
    entry.erase("min_y");
    entry.erase("max_y");

    if (const auto* perimeter = group.method_config.as_perimeter()) {
        entry["min_number"] = perimeter->min_number;
        entry["max_number"] = perimeter->max_number;
        entry.erase("quantity");
        entry.erase("edge_inset_percent");
    } else if (const auto* edge = group.method_config.as_edge()) {
        entry["min_number"] = edge->min_number;
        entry["max_number"] = edge->max_number;
        entry["edge_inset_percent"] = edge->inset_percent;
        entry.erase("quantity");
    } else if (const auto* exact = group.method_config.as_exact()) {
        entry["quantity"] = exact->quantity;
        entry["min_number"] = exact->quantity;
        entry["max_number"] = exact->quantity;
        entry.erase("edge_inset_percent");
    } else {
        entry.erase("quantity");
        entry.erase("edge_inset_percent");
    }

    nlohmann::json candidates = nlohmann::json::array();
    for (const auto& candidate : group.candidates) {
        nlohmann::json candidate_json = nlohmann::json::object();
        candidate_json["name"] = candidate.asset_id;
        candidate_json["chance"] = candidate.weight;
        candidates.push_back(std::move(candidate_json));
    }
    entry["candidates"] = std::move(candidates);

    vibble::spawn_group_codec::EntryDefaults defaults{};
    defaults.display_name = group.display_name.empty() ? std::string("New Spawn") : group.display_name;
    vibble::spawn_group_codec::ensure_spawn_group_entry_defaults(entry, defaults);
}

} // namespace vibble::dev_mode::room_config::model
