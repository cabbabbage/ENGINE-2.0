#include "asset_spawn_planner.hpp"
#include <random>
#include <algorithm>
#include <cctype>
#include <iomanip>

#include "room_relative_size_resolver.hpp"
#include "assets/asset/Asset.hpp"
#include "spawn_group_codec.hpp"

AssetSpawnPlanner::AssetSpawnPlanner(const std::vector<nlohmann::json>& json_sources,
                                     const Area& area,
                                     AssetLibrary& asset_library,
                                     const std::vector<SourceContext>& source_contexts)
: asset_library_(&asset_library) {
    source_jsons_ = json_sources;
    source_contexts_ = source_contexts;
    if (source_contexts_.size() < source_jsons_.size()) {
        source_contexts_.resize(source_jsons_.size());
    }
    source_changed_.assign(source_jsons_.size(), false);

    nlohmann::json merged;
    merged["spawn_groups"] = nlohmann::json::array();
    for (size_t si = 0; si < source_jsons_.size(); ++si) {
        try {
            nlohmann::json js = source_jsons_[si];
            auto& groups = vibble::spawn_group_codec::ensure_spawn_groups_array(js);
            if (!groups.is_array()) continue;
            for (size_t ai = 0; ai < groups.size(); ++ai) {
                merged["spawn_groups"].push_back(groups[ai]);
                SourceRef ref;
                ref.source_index = static_cast<int>(si);
                ref.entry_index  = static_cast<int>(ai);
                ref.key = "spawn_groups";
                assets_provenance_.push_back(std::move(ref));
            }
        } catch (...) {

            continue;
        }
    }

    root_json_ = std::move(merged);
    parse_asset_spawns(area);
    sort_spawn_queue();
    persist_sources();
}

const std::vector<SpawnInfo>& AssetSpawnPlanner::get_spawn_queue() const {
    return spawn_queue_;
}

void AssetSpawnPlanner::parse_asset_spawns(const Area& area) {
    std::mt19937 rng(std::random_device{}());
    if (!root_json_.contains("spawn_groups") || !root_json_["spawn_groups"].is_array()) return;

    auto get_opt_str = [](const nlohmann::json& j, const char* k) -> std::string {
        return (j.contains(k) && j[k].is_string()) ? j[k].get<std::string>() : std::string{};
};

    auto mark_source_changed = [&](const SourceRef& ref) {
        if (ref.source_index >= 0 && static_cast<size_t>(ref.source_index) < source_changed_.size()) {
            source_changed_[static_cast<size_t>(ref.source_index)] = true;
        }
    };

    for (size_t idx = 0; idx < root_json_["spawn_groups"].size(); ++idx) {
        auto& entry = root_json_["spawn_groups"][idx];
        if (!entry.is_object()) continue;

        std::string fallback_display_name = get_opt_str(entry, "display_name");
        if (fallback_display_name.empty()) {
            fallback_display_name = get_opt_str(entry, "name");
        }
        if (fallback_display_name.empty()) {
            fallback_display_name = "Spawn";
        }

        vibble::spawn_group_codec::EntryDefaults defaults{};
        defaults.display_name = fallback_display_name;
        const std::string fallback_candidate_name = get_opt_str(entry, "name");
        defaults.default_candidate.name = fallback_candidate_name.empty() ? std::string("null") : fallback_candidate_name;
        defaults.default_candidate.chance = fallback_candidate_name.empty() ? 0.0 : 100.0;

        if (vibble::spawn_group_codec::ensure_spawn_group_entry_defaults(entry, defaults) &&
            idx < assets_provenance_.size()) {
            const auto& ref = assets_provenance_[idx];
            if (auto* src = get_source_entry(ref.source_index, ref.entry_index, ref.key)) {
                *src = entry;
                mark_source_changed(ref);
            }
        }

        nlohmann::json asset = entry;
        std::string spawn_id = get_opt_str(asset, "spawn_id");

        int priority = -1;
        if (asset.contains("priority")) {
            priority = vibble::spawn_group_codec::read_int_field(asset, "priority", -1);
        }
        if (priority < 0) {
            priority = static_cast<int>(idx);
            entry["priority"] = priority;
            if (idx < assets_provenance_.size()) {
                const auto& ref = assets_provenance_[idx];
                if (auto* src = get_source_entry(ref.source_index, ref.entry_index, ref.key)) {
                    (*src)["priority"] = priority;
                    mark_source_changed(ref);
                }
            }
        }

        std::string position = vibble::spawn_group_codec::normalize_method_from_entry(asset);

        std::string display_name = get_opt_str(asset, "display_name");
        if (display_name.empty()) display_name = get_opt_str(asset, "name");
        if (display_name.empty()) display_name = spawn_id;

        const auto to_lower = [](std::string value) {
            std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
                return static_cast<char>(std::tolower(c));
            });
            return value;
        };

        SpawnInfo::ExecutionMode execution_mode = SpawnInfo::ExecutionMode::Standard;
        std::string execution_mode_text = get_opt_str(asset, "execution_mode");
        if (execution_mode_text.empty()) {
            execution_mode_text = get_opt_str(asset, "spawn_mode");
        }
        const std::string lowered_execution_mode = to_lower(execution_mode_text);
        if (lowered_execution_mode == "batch_grid" || lowered_execution_mode == "batch") {
            execution_mode = SpawnInfo::ExecutionMode::BatchGrid;
        } else {
            const std::string lowered_display = to_lower(display_name);
            const std::string lowered_name = to_lower(get_opt_str(asset, "name"));
            if (lowered_display == "batch_map_assets" || lowered_name == "batch_map_assets") {
                execution_mode = SpawnInfo::ExecutionMode::BatchGrid;
            }
        }
        if (execution_mode == SpawnInfo::ExecutionMode::BatchGrid) {
            bool updated = false;
            if (!entry.contains("execution_mode") ||
                !entry["execution_mode"].is_string() ||
                entry["execution_mode"].get<std::string>() != "batch_grid") {
                entry["execution_mode"] = "batch_grid";
                updated = true;
            }
            if (!asset.contains("execution_mode") ||
                !asset["execution_mode"].is_string() ||
                asset["execution_mode"].get<std::string>() != "batch_grid") {
                asset["execution_mode"] = "batch_grid";
                updated = true;
            }
            if (updated && idx < assets_provenance_.size()) {
                const auto& ref = assets_provenance_[idx];
                if (auto* src = get_source_entry(ref.source_index, ref.entry_index, ref.key)) {
                    (*src)["execution_mode"] = "batch_grid";
                    mark_source_changed(ref);
                }
            }
        }

        std::string link_name = get_opt_str(asset, "link");

        const bool force_single_quantity = (position == "Exact");
        const bool default_geometry = vibble::spawn_group_codec::uses_geometry_resolution_by_default(position);
        const bool resolve_geometry =
            vibble::spawn_group_codec::read_bool_field(asset, "resolve_geometry_to_room_size", default_geometry);
        const bool resolve_quantity =
            vibble::spawn_group_codec::read_bool_field(asset, "resolve_quantity_to_room_size", false);

        auto [minx, miny, maxx, maxy] = area.get_bounds();
        const int curr_w = std::max(1, maxx - minx);
        const int curr_h = std::max(1, maxy - miny);

        int min_num = vibble::spawn_group_codec::read_int_field(asset, "min_number", 1);
        int max_num = vibble::spawn_group_codec::read_int_field(asset, "max_number", min_num);
        if (min_num < 0) min_num = 0;
        if (max_num < 0) max_num = 0;
        if (max_num < min_num) std::swap(max_num, min_num);

        int orig_w = vibble::spawn_group_codec::read_int_field(asset, "origional_width", curr_w);
        int orig_h = vibble::spawn_group_codec::read_int_field(asset, "origional_height", curr_h);

        const bool need_orig = default_geometry || resolve_geometry || resolve_quantity;
        if (need_orig) {
            bool set_wh = false;
            if (!asset.contains("origional_width") || !asset["origional_width"].is_number_integer()) {
                entry["origional_width"] = curr_w;
                asset["origional_width"] = curr_w;
                orig_w = curr_w;
                set_wh = true;
            } else {
                orig_w = vibble::spawn_group_codec::read_int_field(asset, "origional_width", curr_w);
            }
            if (!asset.contains("origional_height") || !asset["origional_height"].is_number_integer()) {
                entry["origional_height"] = curr_h;
                asset["origional_height"] = curr_h;
                orig_h = curr_h;
                set_wh = true;
            } else {
                orig_h = vibble::spawn_group_codec::read_int_field(asset, "origional_height", curr_h);
            }
            if (set_wh && idx < assets_provenance_.size()) {
                const auto& ref = assets_provenance_[idx];
                if (auto* src = get_source_entry(ref.source_index, ref.entry_index, ref.key)) {
                    (*src)["origional_width"] = entry["origional_width"];
                    (*src)["origional_height"] = entry["origional_height"];
                    mark_source_changed(ref);
                }
            }
        }

        RoomRelativeSizeResolver scaler(orig_w, orig_h, curr_w, curr_h);
        if (resolve_quantity && !force_single_quantity) {
            auto scaled_range = scaler.scale_count_range(min_num, max_num);
            min_num = scaled_range.first;
            max_num = scaled_range.second;
        }

        int quantity = std::uniform_int_distribution<int>(min_num, max_num)(rng);
        if (force_single_quantity) {
            quantity = 1;
        }

        const bool explicit_flip =
            vibble::spawn_group_codec::read_bool_field(asset, "explicit_flip", false);
        const bool force_flipped =
            vibble::spawn_group_codec::read_bool_field(asset, "force_flipped", false);
        Asset::SetFlipOverrideForSpawnId(spawn_id, explicit_flip, force_flipped);

        if (!asset.contains("candidates") || !asset["candidates"].is_array()) {
            continue;
        }
        vibble::spawn::RuntimeCandidates candidates =
            vibble::spawn::RuntimeCandidates::from_json(asset["candidates"]);
        if (candidates.empty()) {
            continue;
        }

        auto average_range = [&](const std::string& lo_key, const std::string& hi_key, int fallback) {
            int lo = asset.value(lo_key, fallback);
            int hi = asset.value(hi_key, fallback);
            if (lo == fallback && hi != fallback) return hi;
            if (hi == fallback && lo != fallback) return lo;
            return (lo + hi) / 2;
};

        SpawnInfo s{};
        s.name     = display_name;
        s.position = position;
        s.spawn_id = spawn_id;
        s.quantity = quantity;
        s.priority = priority;
        s.grid_resolution = entry.value("grid_resolution", 0);
        s.adjust_geometry_to_room = resolve_geometry;
        s.execution_mode = execution_mode;
        if (!link_name.empty()) {
            s.link_area_name = link_name;
        }

        s.check_min_spacing = asset.value("enforce_spacing", false);

        s.exact_offset.x = asset.value("dx", 0);
        s.exact_offset.y = asset.value("dz", 0);
        if (resolve_geometry) {
            s.exact_origin_w = orig_w;
            s.exact_origin_h = orig_h;
        } else {
            s.exact_origin_w = curr_w;
            s.exact_origin_h = curr_h;
        }
        s.exact_point.x  = asset.value("ep_x", average_range("ep_x_min", "ep_x_max", -1));
        s.exact_point.y  = asset.value("ep_y", average_range("ep_y_min", "ep_y_max", -1));

        if (position == "Perimeter") {
            int base_radius = asset.value("radius", asset.value("perimeter_radius", 0));
            s.perimeter_radius = resolve_geometry ? scaler.scale_length(base_radius) : base_radius;
        } else if (position == "Edge") {
            int inset = asset.value("edge_inset_percent", asset.value("boundary_inset", 100));
            inset = std::clamp(inset, 0, 200);
            s.edge_inset_percent = inset;
        }

        s.candidates = std::move(candidates);
        spawn_queue_.push_back(std::move(s));
    }
}

void AssetSpawnPlanner::sort_spawn_queue() {

    std::stable_sort(spawn_queue_.begin(), spawn_queue_.end(),
                     [](const SpawnInfo& a, const SpawnInfo& b){ return a.priority < b.priority; });
}

void AssetSpawnPlanner::persist_sources() {
    for (size_t i = 0; i < source_jsons_.size(); ++i) {
        if (i >= source_changed_.size() || !source_changed_[i]) continue;
        if (i >= source_contexts_.size()) continue;
        auto& ctx = source_contexts_[i];
        if (ctx.json_ref) {
            *ctx.json_ref = source_jsons_[i];
        }
        if (ctx.persist) {
            ctx.persist(source_jsons_[i]);
        }
    }
}

nlohmann::json* AssetSpawnPlanner::get_source_entry(int source_index, int entry_index, const std::string& key) {
    if (source_index < 0 || entry_index < 0) return nullptr;
    size_t si = static_cast<size_t>(source_index);
    size_t ei = static_cast<size_t>(entry_index);
    if (si >= source_jsons_.size()) return nullptr;
    try {
        auto& src = source_jsons_[si];
        if (!src.contains(key) || !src[key].is_array()) return nullptr;
        auto& arr = src[key];
        if (ei >= arr.size()) return nullptr;
        return &arr[ei];
    } catch (...) {
        return nullptr;
    }
}
