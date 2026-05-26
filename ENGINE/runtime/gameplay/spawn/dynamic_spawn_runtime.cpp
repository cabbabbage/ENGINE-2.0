#include "gameplay/spawn/dynamic_spawn_runtime.hpp"
#include "gameplay/spawn/dynamic_spawn_geometry.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_set>
#include <utility>

#include "assets/asset/Asset.hpp"
#include "assets/asset/asset_info.hpp"
#include "assets/asset/asset_library.hpp"
#include "assets/asset/asset_types.hpp"
#include "core/AssetsManager.hpp"
#include "gameplay/map_generation/room.hpp"
#include "gameplay/spawn/trail_classification.hpp"
#include "gameplay/world/world_grid.hpp"
#include "utils/area.hpp"
#include "utils/grid.hpp"
#include "utils/integer_grid_math.hpp"
#include "utils/log.hpp"
#include "utils/string_utils.hpp"

namespace dynamic_spawn {
namespace {

std::uint64_t mix_u64(std::uint64_t seed, std::uint64_t value) {
    seed ^= value + 0x9e3779b97f4a7c15ULL + (seed << 6) + (seed >> 2);
    return seed;
}

double unit_interval(std::uint64_t value) {
    value ^= value >> 33;
    value *= 0xff51afd7ed558ccdULL;
    value ^= value >> 33;
    value *= 0xc4ceb9fe1a85ec53ULL;
    value ^= value >> 33;
    constexpr double kInv = 1.0 / static_cast<double>(std::numeric_limits<std::uint64_t>::max());
    return static_cast<double>(value) * kInv;
}

double candidate_weight(const nlohmann::json& candidate) {
    if (!candidate.is_object()) {
        return 0.0;
    }
    auto read_weight = [](const nlohmann::json& value) -> double {
        try {
            if (value.is_number()) {
                const double parsed = value.get<double>();
                return std::isfinite(parsed) ? std::max(0.0, parsed) : 0.0;
            }
            if (value.is_string()) {
                const double parsed = std::stod(value.get<std::string>());
                return std::isfinite(parsed) ? std::max(0.0, parsed) : 0.0;
            }
        } catch (...) {
        }
        return 0.0;
    };
    if (auto it = candidate.find("chance"); it != candidate.end()) {
        return read_weight(*it);
    }
    if (auto it = candidate.find("weight"); it != candidate.end()) {
        return read_weight(*it);
    }
    return 0.0;
}

std::string candidate_name(const nlohmann::json& candidate, bool& is_tag, bool& is_null) {
    is_tag = false;
    is_null = false;
    if (!candidate.is_object()) {
        return {};
    }
    std::string name;
    if (auto it = candidate.find("name"); it != candidate.end() && it->is_string()) {
        name = it->get<std::string>();
    } else if (auto it = candidate.find("asset"); it != candidate.end() && it->is_string()) {
        name = it->get<std::string>();
    } else if (auto it = candidate.find("tag"); it != candidate.end() && it->is_string()) {
        name = it->get<std::string>();
        is_tag = true;
    }
    name = vibble::strings::trim_copy(name);
    if (!name.empty() && name.front() == '#') {
        name.erase(name.begin());
        is_tag = true;
    }
    std::string lower = name;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    is_null = lower.empty() || lower == "null" || lower == "none";
    return name;
}

bool type_is_boundary(const AssetInfo* info) {
    return info && asset_types::canonicalize(info->type) == asset_types::boundary;
}

bool named_area_is_trail(const Room::NamedArea& area) {
    return is_trail_area_label(area.type) ||
           is_trail_area_label(area.kind) ||
           is_trail_area_label(area.name);
}

int read_int_setting(const nlohmann::json& object, const char* key, int fallback, int min_value, int max_value) {
    if (!object.is_object()) {
        return std::clamp(fallback, min_value, max_value);
    }
    auto it = object.find(key);
    if (it == object.end() || !it->is_number()) {
        return std::clamp(fallback, min_value, max_value);
    }
    try {
        const double raw = it->get<double>();
        if (!std::isfinite(raw)) {
            return std::clamp(fallback, min_value, max_value);
        }
        return std::clamp(static_cast<int>(std::llround(raw)), min_value, max_value);
    } catch (...) {
        return std::clamp(fallback, min_value, max_value);
    }
}

} // namespace

std::size_t DynamicSpawnRuntime::CellKeyHash::operator()(const CellKey& key) const {
    std::size_t seed = std::hash<int>{}(static_cast<int>(key.mode));
    auto mix = [&seed](std::size_t value) {
        seed ^= value + 0x9e3779b9u + (seed << 6) + (seed >> 2);
    };
    mix(std::hash<std::uint32_t>{}(key.selector_id));
    mix(std::hash<int>{}(key.grid_resolution));
    mix(std::hash<int>{}(key.grid_x));
    mix(std::hash<int>{}(key.grid_z));
    return seed;
}

std::size_t DynamicSpawnRuntime::ChunkKeyHash::operator()(const ChunkKey& key) const {
    std::size_t seed = std::hash<int>{}(key.i);
    seed ^= std::hash<int>{}(key.j) + 0x9e3779b9u + (seed << 6) + (seed >> 2);
    return seed;
}

DynamicSpawnRuntime::DynamicSpawnRuntime(Assets& assets) : assets_(assets) {}

DynamicSpawnRuntime::~DynamicSpawnRuntime() = default;

void DynamicSpawnRuntime::compile_from_map() {
    clear_active_instances(true);
    selectors_.clear();
    cells_by_chunk_.clear();
    active_chunks_.clear();
    next_selector_id_ = 1;
    diagnostics_ = {};

    planned_max_spawn_from_room_px_ = max_spawn_from_room_px();
    parse_selectors();
    build_plan();
    for (const auto& entry : cells_by_chunk_) {
        diagnostics_.planned_cells += entry.second.size();
    }
}

void DynamicSpawnRuntime::clear() {
    clear_active_instances(true);
    selectors_.clear();
    cells_by_chunk_.clear();
    active_chunks_.clear();
    diagnostics_ = {};
    planned_max_spawn_from_room_px_ = 128;
}

void DynamicSpawnRuntime::clear_active_instances(bool delete_assets) {
    std::vector<Asset*> active_assets;
    active_assets.reserve(active_.size());
    for (const auto& [key, asset] : active_) {
        (void)key;
        if (asset) {
            active_assets.push_back(asset);
        }
    }
    active_.clear();
    asset_to_key_.clear();
    active_chunks_.clear();

    if (delete_assets && !active_assets.empty()) {
        diagnostics_.deleted += assets_.delete_assets_runtime(active_assets);
    } else {
        for (Asset* asset : active_assets) {
            if (!asset || asset->dead) {
                continue;
            }
            (void)assets_.extract_asset(asset);
        }
    }
    suspended_.clear();
}

void DynamicSpawnRuntime::forget_asset(Asset* asset) {
    if (!asset) {
        return;
    }
    auto it = asset_to_key_.find(asset);
    if (it == asset_to_key_.end()) {
        return;
    }
    active_.erase(it->second);
    asset_to_key_.erase(it);
}

std::size_t DynamicSpawnRuntime::delete_for_spawn_group(const std::string& spawn_id) {
    if (spawn_id.empty()) {
        return 0;
    }
    std::vector<Asset*> active_assets;
    for (const auto& [key, asset] : active_) {
        (void)key;
        if (!asset || asset->dead) {
            continue;
        }
        const std::string prefix = std::string("live_dynamic:") + spawn_id + ":";
        if (asset->spawn_id.rfind(prefix, 0) == 0) {
            active_assets.push_back(asset);
        }
    }

    std::size_t removed = 0;
    if (!active_assets.empty()) {
        removed += assets_.delete_assets_runtime(active_assets);
    }

    for (auto it = suspended_.begin(); it != suspended_.end();) {
        const std::string prefix = std::string("live_dynamic:") + spawn_id + ":";
        Asset* asset = it->second.get();
        if (asset && asset->spawn_id.rfind(prefix, 0) == 0) {
            it = suspended_.erase(it);
            ++removed;
        } else {
            ++it;
        }
    }

    if (removed > 0) {
        for (auto chunk_it = cells_by_chunk_.begin(); chunk_it != cells_by_chunk_.end(); ++chunk_it) {
            auto& cells = chunk_it->second;
            cells.erase(std::remove_if(cells.begin(), cells.end(), [&](const PlannedCell& cell) {
                return cell.selector_spawn_id == spawn_id;
            }), cells.end());
        }
    }
    diagnostics_.deleted += removed;
    return removed;
}

std::size_t DynamicSpawnRuntime::delete_for_spawn_groups(const std::vector<std::string>& spawn_ids) {
    std::size_t removed = 0;
    for (const std::string& spawn_id : spawn_ids) {
        removed += delete_for_spawn_group(spawn_id);
    }
    return removed;
}

void DynamicSpawnRuntime::parse_selectors() {
    const nlohmann::json& map = assets_.map_info_json();
    if (!map.is_object()) {
        return;
    }
    auto live_it = map.find("live_dynamic_spawns");
    if (live_it == map.end() || !live_it->is_object()) {
        return;
    }
    auto selectors_it = live_it->find("boundary_area_selectors");
    if (selectors_it == live_it->end() || !selectors_it->is_array()) {
        return;
    }

    const auto catalog = assets_.library().all();
    auto append_selector = [&](const nlohmann::json& source, Mode mode) {
        if (!source.is_object()) {
            return;
        }
        auto candidates_it = source.find("candidates");
        if (candidates_it == source.end() || !candidates_it->is_array() || candidates_it->empty()) {
            return;
        }

        Selector selector;
        selector.id = next_selector_id_++;
        selector.mode = mode;
        selector.spawn_id = source.value("spawn_id", std::string{});
        if (selector.spawn_id.empty()) {
            selector.spawn_id = source.value("name", std::string{});
        }
        if (selector.spawn_id.empty()) {
            selector.spawn_id = std::string("live_dynamic_") + std::to_string(selector.id);
        }
        selector.display_name = source.value("display_name", selector.spawn_id);
        const int raw_resolution = source.contains("grid_resolution")
            ? source.value("grid_resolution", assets_.map_grid_settings().grid_resolution)
            : source.value("resolution", assets_.map_grid_settings().grid_resolution);
        selector.grid_resolution = vibble::grid::clamp_resolution(raw_resolution);
        selector.jitter_px = std::clamp(source.value("jitter", assets_.map_grid_settings().position_jitter_px), 0, 2048);
        const std::uint64_t spawn_hash = static_cast<std::uint64_t>(std::hash<std::string>{}(selector.spawn_id));
        selector.jitter_seed = mix_u64(static_cast<std::uint64_t>(std::hash<std::string>{}(assets_.map_id())), spawn_hash);
        selector.candidate_seed = mix_u64(selector.jitter_seed, 0x9e3779b97f4a7c15ULL);

        for (const nlohmann::json& candidate_json : *candidates_it) {
            const double weight = candidate_weight(candidate_json);
            if (weight <= 0.0) {
                continue;
            }
            bool is_tag = false;
            bool is_null = false;
            const std::string name = candidate_name(candidate_json, is_tag, is_null);
            if (is_null) {
                selector.candidates.push_back(Candidate{{}, nullptr, weight, true});
                continue;
            }
            if (is_tag) {
                for (const auto& [asset_name, info] : catalog) {
                    if (info && info->has_tag(name) && info_allowed(info.get(), mode)) {
                        selector.candidates.push_back(Candidate{asset_name, info, weight, false});
                    }
                }
                continue;
            }
            std::shared_ptr<AssetInfo> info = assets_.library().get(name);
            if (info && info_allowed(info.get(), mode)) {
                selector.candidates.push_back(Candidate{name, info, weight, false});
            }
        }

        if (selector.candidates.empty()) {
            return;
        }
        double cumulative = 0.0;
        selector.cumulative_weights.reserve(selector.candidates.size());
        for (const Candidate& candidate : selector.candidates) {
            cumulative += std::max(0.0, candidate.weight);
            selector.cumulative_weights.push_back(cumulative);
        }
        selector.total_weight = cumulative;
        if (selector.total_weight > 0.0) {
            selectors_.push_back(std::move(selector));
        }
    };

    for (const nlohmann::json& selector_json : *selectors_it) {
        append_selector(selector_json, Mode::BoundaryArea);
        append_selector(selector_json, Mode::InheritedMap);
    }
    selector_index_.clear();
    for (const Selector& s : selectors_) { selector_index_[ (static_cast<std::uint64_t>(s.id) << 32) | static_cast<std::uint64_t>(s.mode)] = &s; }
}

void DynamicSpawnRuntime::build_plan() {
    cells_by_chunk_.clear();
    build_plan_into(cells_by_chunk_, planned_max_spawn_from_room_px_);
}

void DynamicSpawnRuntime::build_plan_into(PlanByChunk& plan, int threshold_px) const {
    const auto geometry = dynamic_spawn::geometry::collect_area_geometry(assets_);
    if (!geometry.valid || selectors_.empty()) {
        return;
    }
    for (const Selector& selector : selectors_) {
        add_selector_cells(selector, geometry, threshold_px, plan);
    }
}

void DynamicSpawnRuntime::refresh_distance_to_edge() {
    diagnostics_.spawned = 0;
    diagnostics_.reused = 0;
    diagnostics_.suspended_this_sync = 0;
    diagnostics_.sync_ms = 0.0;
    const std::uint64_t freq = SDL_GetPerformanceFrequency();
    const std::uint64_t begin = SDL_GetPerformanceCounter();

    if (selectors_.empty()) {
        diagnostics_.planned_cells = 0;
        diagnostics_.active = active_.size();
        diagnostics_.suspended = suspended_.size();
        return;
    }

    const int previous_threshold = planned_max_spawn_from_room_px_;
    const int next_threshold = max_spawn_from_room_px();
    if (previous_threshold == next_threshold) {
        std::size_t planned_count = 0;
        for (const auto& [chunk, cells] : cells_by_chunk_) {
            (void)chunk;
            planned_count += cells.size();
        }
        diagnostics_.planned_cells = planned_count;
        diagnostics_.active = active_.size();
        diagnostics_.suspended = suspended_.size();
        return;
    }

    const auto geometry = dynamic_spawn::geometry::collect_area_geometry(assets_);
    if (!geometry.valid) {
        clear_active_instances(true);
        cells_by_chunk_.clear();
        planned_max_spawn_from_room_px_ = next_threshold;
        diagnostics_.active = active_.size();
        diagnostics_.suspended = suspended_.size();
        return;
    }

    std::unordered_set<CellKey, CellKeyHash> obsolete_cells;
    if (next_threshold < previous_threshold) {
        for (auto& [chunk, cells] : cells_by_chunk_) {
            (void)chunk;
            for (const PlannedCell& cell : cells) {
                const std::uint64_t selector_key = (static_cast<std::uint64_t>(cell.key.selector_id) << 32) | static_cast<std::uint64_t>(cell.key.mode);
                auto sel_it = selector_index_.find(selector_key);
                const Selector* selector = sel_it == selector_index_.end() ? nullptr : sel_it->second;
                std::string owner_name;
                if (!selector ||
                    !resolve_cell_owner(*selector,
                                        geometry,
                                        SDL_Point{cell.owner_anchor_world_x, cell.owner_anchor_world_z},
                                        next_threshold,
                                        owner_name)) {
                    obsolete_cells.insert(cell.key);
                }
            }
        }
    } else {
        std::unordered_set<CellKey, CellKeyHash> known_keys;
        for (const auto& [chunk, cells] : cells_by_chunk_) {
            (void)chunk;
            for (const PlannedCell& cell : cells) {
                known_keys.insert(cell.key);
            }
        }
        for (const Selector& selector : selectors_) {
            add_selector_cells_in_distance_band(selector,
                                                geometry,
                                                previous_threshold,
                                                next_threshold,
                                                cells_by_chunk_,
                                                known_keys);
        }
    }

    if (!obsolete_cells.empty()) {
        for (auto& [chunk, cells] : cells_by_chunk_) {
            (void)chunk;
            cells.erase(std::remove_if(cells.begin(), cells.end(), [&](const PlannedCell& cell) {
                return obsolete_cells.find(cell.key) != obsolete_cells.end();
            }), cells.end());
        }
    }

    std::size_t planned_count = 0;
    for (const auto& [chunk, cells] : cells_by_chunk_) {
        (void)chunk;
        planned_count += cells.size();
    }

    std::vector<std::pair<CellKey, Asset*>> assets_to_suspend;
    if (!obsolete_cells.empty()) {
        for (const auto& [key, asset] : active_) {
            if (obsolete_cells.find(key) != obsolete_cells.end()) {
                assets_to_suspend.push_back({key, asset});
            }
        }
    }

    for (const auto& [key, asset] : assets_to_suspend) {
        suspend_cell(key, asset);
    }

    planned_max_spawn_from_room_px_ = next_threshold;
    diagnostics_.planned_cells = planned_count;

    for (const ChunkKey& chunk : active_chunks_) {
        activate_chunk(chunk);
    }

    if (freq != 0) {
        const std::uint64_t end = SDL_GetPerformanceCounter();
        diagnostics_.sync_ms = static_cast<double>(end - begin) * 1000.0 / static_cast<double>(freq);
    }
    diagnostics_.active = active_.size();
    diagnostics_.suspended = suspended_.size();
}

void DynamicSpawnRuntime::add_selector_cells(const Selector& selector,
                                             const dynamic_spawn::geometry::AreaGeometry& geometry,
                                             int threshold_px,
                                             PlanByChunk& plan) const {
    const int threshold = std::max(0, threshold_px);
    const int expanded_min_x = geometry.min_x - threshold;
    const int expanded_min_z = geometry.min_z - threshold;
    const int expanded_max_x = geometry.max_x + threshold;
    const int expanded_max_z = geometry.max_z + threshold;
    const int resolution = vibble::grid::clamp_resolution(selector.grid_resolution);
    const SDL_Point min_index = vibble::grid::global_grid().world_to_index(
        SDL_Point{expanded_min_x, expanded_min_z}, resolution);
    const SDL_Point max_index = vibble::grid::global_grid().world_to_index(
        SDL_Point{expanded_max_x, expanded_max_z}, resolution);
    const int min_gx = std::min(min_index.x, max_index.x);
    const int max_gx = std::max(min_index.x, max_index.x);
    const int min_gz = std::min(min_index.y, max_index.y);
    const int max_gz = std::max(min_index.y, max_index.y);

    for (int gx = min_gx; gx <= max_gx; ++gx) {
        for (int gz = min_gz; gz <= max_gz; ++gz) {
            const SDL_Point owner_anchor = vibble::grid::global_grid().index_to_world(gx, gz, resolution);
            std::string owner_name;
            if (!resolve_cell_owner(selector, geometry, owner_anchor, threshold, owner_name)) {
                continue;
            }

            CellKey key{selector.mode, selector.id, resolution, gx, gz};
            add_planned_cell(selector, key, owner_anchor.x, owner_anchor.y, owner_name, plan);
        }
    }
}

void DynamicSpawnRuntime::add_selector_cells_in_distance_band(
    const Selector& selector,
    const dynamic_spawn::geometry::AreaGeometry& geometry,
    int previous_threshold_px,
    int next_threshold_px,
    PlanByChunk& plan,
    std::unordered_set<CellKey, CellKeyHash>& known_keys) const {
    const int previous_threshold = std::max(0, previous_threshold_px);
    const int next_threshold = std::max(previous_threshold, next_threshold_px);
    const int expanded_min_x = geometry.min_x - next_threshold;
    const int expanded_min_z = geometry.min_z - next_threshold;
    const int expanded_max_x = geometry.max_x + next_threshold;
    const int expanded_max_z = geometry.max_z + next_threshold;
    const int resolution = vibble::grid::clamp_resolution(selector.grid_resolution);
    const SDL_Point min_index = vibble::grid::global_grid().world_to_index(
        SDL_Point{expanded_min_x, expanded_min_z}, resolution);
    const SDL_Point max_index = vibble::grid::global_grid().world_to_index(
        SDL_Point{expanded_max_x, expanded_max_z}, resolution);
    const int min_gx = std::min(min_index.x, max_index.x);
    const int max_gx = std::max(min_index.x, max_index.x);
    const int min_gz = std::min(min_index.y, max_index.y);
    const int max_gz = std::max(min_index.y, max_index.y);

    for (int gx = min_gx; gx <= max_gx; ++gx) {
        for (int gz = min_gz; gz <= max_gz; ++gz) {
            CellKey key{selector.mode, selector.id, resolution, gx, gz};
            if (known_keys.find(key) != known_keys.end()) {
                continue;
            }
            const SDL_Point owner_anchor = vibble::grid::global_grid().index_to_world(gx, gz, resolution);
            if (dynamic_spawn::geometry::point_near_geometry(owner_anchor, geometry, previous_threshold)) {
                continue;
            }
            std::string owner_name;
            if (!resolve_cell_owner(selector, geometry, owner_anchor, next_threshold, owner_name)) {
                continue;
            }
            add_planned_cell(selector, key, owner_anchor.x, owner_anchor.y, owner_name, plan);
            known_keys.insert(key);
        }
    }
}

bool DynamicSpawnRuntime::resolve_cell_owner(const Selector& selector,
                                             const dynamic_spawn::geometry::AreaGeometry& geometry,
                                             SDL_Point owner_anchor,
                                             int threshold_px,
                                             std::string& owner_name) const {
    if (!dynamic_spawn::geometry::point_near_geometry(owner_anchor, geometry, threshold_px)) {
        return false;
    }

    if (selector.mode == Mode::InheritedMap) {
        Room* owner = inherited_room_for_point(owner_anchor);
        if (!owner) {
            return false;
        }
        owner_name = owner->room_name;
        return true;
    }

    if (dynamic_spawn::geometry::point_inside_any_area(owner_anchor, geometry)) {
        return false;
    }
    owner_name = assets_.map_id();
    return true;
}

void DynamicSpawnRuntime::add_planned_cell(const Selector& selector,
                                           const CellKey& key,
                                           int owner_anchor_world_x,
                                           int owner_anchor_world_z,
                                           const std::string& owner_name,
                                           PlanByChunk& plan) const {
    const Candidate* candidate = pick_candidate(selector, key);
    if (!candidate || candidate->is_null || !candidate->info || candidate->asset_name.empty()) {
        return;
    }
    const SDL_Point jittered = jittered_world_point(selector, key, SDL_Point{owner_anchor_world_x, owner_anchor_world_z});
    PlannedCell cell;
    cell.key = key;
    cell.owner_anchor_world_x = owner_anchor_world_x;
    cell.owner_anchor_world_z = owner_anchor_world_z;
    cell.world_x = jittered.x;
    cell.world_z = jittered.y;
    cell.owner_name = owner_name;
    cell.selector_spawn_id = selector.spawn_id;
    cell.asset_name = candidate->asset_name;
    cell.info = candidate->info;
    cell.chunk = chunk_key_for_world(owner_anchor_world_x, owner_anchor_world_z);
    plan[cell.chunk].push_back(std::move(cell));
}

void DynamicSpawnRuntime::sync(const world::GridBounds& work_bounds, std::size_t max_cells_per_sync, bool movement_throttled) {
    diagnostics_.spawned = 0;
    diagnostics_.reused = 0;
    diagnostics_.suspended_this_sync = 0;
    diagnostics_.cells_processed_this_frame = 0;
    diagnostics_.deferred_cells_remaining = 0;
    diagnostics_.movement_throttling_applied = movement_throttled;
    diagnostics_.sync_ms = 0.0;
    const std::uint64_t freq = SDL_GetPerformanceFrequency();
    const std::uint64_t begin = SDL_GetPerformanceCounter();

    if (!assets_.live_dynamic_assets_visible()) {
        suspend_outside_keep_chunks({});
        active_chunks_.clear();
        pending_activation_chunks_.clear();
        pending_activation_set_.clear();
        chunk_activation_cursor_.clear();
        diagnostics_.active = active_.size();
        diagnostics_.suspended = suspended_.size();
        return;
    }

    auto spawn_chunks = chunk_keys_for_bounds(expanded_bounds(work_bounds, preload_margin_px()));
    auto keep_chunks = chunk_keys_for_bounds(expanded_bounds(work_bounds, despawn_margin_px()));
    suspend_outside_keep_chunks(keep_chunks);

    std::vector<ChunkKey> ordered_spawn_chunks(spawn_chunks.begin(), spawn_chunks.end());
    std::sort(ordered_spawn_chunks.begin(), ordered_spawn_chunks.end(), [](const ChunkKey& lhs, const ChunkKey& rhs) {
        if (lhs.i != rhs.i) return lhs.i < rhs.i;
        return lhs.j < rhs.j;
    });
    for (const ChunkKey& chunk : ordered_spawn_chunks) {
        if (active_chunks_.find(chunk) != active_chunks_.end() || pending_activation_set_.find(chunk) != pending_activation_set_.end()) {
            continue;
        }
        pending_activation_chunks_.push_back(chunk);
        pending_activation_set_.insert(chunk);
    }

    for (auto it = active_chunks_.begin(); it != active_chunks_.end();) {
        if (keep_chunks.find(*it) == keep_chunks.end()) {
            pending_activation_set_.erase(*it);
            chunk_activation_cursor_.erase(*it);
            it = active_chunks_.erase(it);
        } else {
            ++it;
        }
    }

    std::vector<ChunkKey> retained_pending;
    retained_pending.reserve(pending_activation_chunks_.size());
    for (const ChunkKey& chunk : pending_activation_chunks_) {
        if (keep_chunks.find(chunk) != keep_chunks.end()) {
            retained_pending.push_back(chunk);
        } else {
            pending_activation_set_.erase(chunk);
            chunk_activation_cursor_.erase(chunk);
        }
    }
    pending_activation_chunks_.swap(retained_pending);
    std::size_t remaining_budget = max_cells_per_sync == 0 ? std::numeric_limits<std::size_t>::max() : max_cells_per_sync;
    std::vector<ChunkKey> next_pending;
    next_pending.reserve(pending_activation_chunks_.size());
    for (const ChunkKey& chunk : pending_activation_chunks_) {
        if (remaining_budget == 0) {
            next_pending.push_back(chunk);
            continue;
        }
        diagnostics_.cells_processed_this_frame += activate_chunk_budgeted(chunk, remaining_budget);
        auto cursor_it = chunk_activation_cursor_.find(chunk);
        if (cursor_it == chunk_activation_cursor_.end()) {
            active_chunks_.insert(chunk);
            pending_activation_set_.erase(chunk);
        } else {
            next_pending.push_back(chunk);
        }
    }
    pending_activation_chunks_.swap(next_pending);
    for (const ChunkKey& chunk : pending_activation_chunks_) {
        auto cursor_it = chunk_activation_cursor_.find(chunk);
        auto chunk_it = cells_by_chunk_.find(chunk);
        if (cursor_it != chunk_activation_cursor_.end() && chunk_it != cells_by_chunk_.end()) {
            diagnostics_.deferred_cells_remaining += (chunk_it->second.size() - std::min(cursor_it->second, chunk_it->second.size()));
        }
    }
    if (freq != 0) {
        const std::uint64_t end = SDL_GetPerformanceCounter();
        diagnostics_.sync_ms = static_cast<double>(end - begin) * 1000.0 / static_cast<double>(freq);
    }
    diagnostics_.active = active_.size();
    diagnostics_.suspended = suspended_.size();
}

void DynamicSpawnRuntime::activate_chunk(const ChunkKey& chunk) {
    std::size_t unlimited_budget = std::numeric_limits<std::size_t>::max();
    (void)activate_chunk_budgeted(chunk, unlimited_budget);
}

std::size_t DynamicSpawnRuntime::activate_chunk_budgeted(const ChunkKey& chunk, std::size_t& remaining_budget) {
    auto it = cells_by_chunk_.find(chunk);
    if (it == cells_by_chunk_.end()) {
        chunk_activation_cursor_.erase(chunk);
        return 0;
    }
    std::size_t& cursor = chunk_activation_cursor_[chunk];
    std::size_t processed = 0;
    for (; cursor < it->second.size() && remaining_budget > 0; ++cursor, --remaining_budget, ++processed) {
        const PlannedCell& cell = it->second[cursor];
        if (active_.find(cell.key) != active_.end()) {
            continue;
        }
        if (assets_.world_grid().is_occupied_at_xz(cell.owner_anchor_world_x,
                                                   cell.owner_anchor_world_z,
                                                   cell.key.grid_resolution)) {
            continue;
        }
        (void)activate_cell(cell);
    }
    if (cursor >= it->second.size()) {
        chunk_activation_cursor_.erase(chunk);
    }
    return processed;
}

Asset* DynamicSpawnRuntime::activate_cell(const PlannedCell& cell) {
    auto suspended_it = suspended_.find(cell.key);
    std::unique_ptr<Asset> asset;
    bool reused = false;
    if (suspended_it != suspended_.end()) {
        asset = std::move(suspended_it->second);
        suspended_.erase(suspended_it);
        reused = true;
        asset->resample_spawn_y_position();
    } else {
        asset = create_asset_for_cell(cell);
    }
    if (!asset) {
        return nullptr;
    }

    Asset* raw = assets_.attach_asset(std::move(asset), cell.world_z, cell.key.grid_resolution);
    if (!raw) {
        return nullptr;
    }
    raw->set_dynamic_spawned_asset(true);
    active_[cell.key] = raw;
    asset_to_key_[raw] = cell.key;
    if (reused) {
        ++diagnostics_.reused;
    } else {
        ++diagnostics_.spawned;
    }
    return raw;
}

std::unique_ptr<Asset> DynamicSpawnRuntime::create_asset_for_cell(const PlannedCell& cell) const {
    if (!cell.info) {
        return nullptr;
    }
    Area spawn_area(cell.owner_name.empty() ? std::string("dynamic_spawn") : cell.owner_name,
                    cell.key.grid_resolution);
    std::string stable_spawn_id =
        std::string("live_dynamic:") + cell.selector_spawn_id +
        ":" + std::to_string(cell.key.grid_resolution) +
        ":" + std::to_string(cell.key.grid_x) +
        ":" + std::to_string(cell.key.grid_z);
    auto asset = std::make_unique<Asset>(cell.info,
                                         spawn_area,
                                         SDL_Point{cell.world_x, cell.world_z},
                                         0,
                                         stable_spawn_id,
                                         std::string("live_dynamic"),
                                         cell.key.grid_resolution);
    asset->set_assets(&assets_);
    asset->set_camera(&assets_.getView());
    asset->set_owning_room_name(cell.owner_name);
    asset->set_dynamic_spawned_asset(true);
    asset->finalize_setup();
    return asset;
}

void DynamicSpawnRuntime::suspend_cell(const CellKey& key, Asset* asset) {
    if (!asset || asset->dead) {
        active_.erase(key);
        asset_to_key_.erase(asset);
        return;
    }
    std::unique_ptr<Asset> detached = assets_.extract_asset(asset);
    active_.erase(key);
    asset_to_key_.erase(asset);
    if (detached) {
        detached->set_dynamic_spawned_asset(true);
        suspended_[key] = std::move(detached);
        ++diagnostics_.suspended_this_sync;
    }
}

void DynamicSpawnRuntime::suspend_outside_keep_chunks(const std::unordered_set<ChunkKey, ChunkKeyHash>& keep_chunks) {
    std::vector<std::pair<CellKey, Asset*>> to_suspend;
    to_suspend.reserve(active_.size());
    for (const auto& [key, asset] : active_) {
        if (!asset || asset->dead) {
            to_suspend.push_back({key, asset});
            continue;
        }
        const ChunkKey chunk = chunk_key_for_world(asset->world_x(), asset->world_z());
        if (keep_chunks.find(chunk) == keep_chunks.end()) {
            to_suspend.push_back({key, asset});
        }
    }
    for (const auto& [key, asset] : to_suspend) {
        suspend_cell(key, asset);
    }
}

DynamicSpawnRuntime::ChunkKey DynamicSpawnRuntime::chunk_key_for_world(int world_x, int world_z) const {
    const int layer = std::max(0, assets_.world_grid().default_resolution_layer());
    const int step = std::max(1, 1 << layer);
    const world::GridPoint origin = assets_.world_grid().origin();
    return ChunkKey{
        vibble::math::floor_div(world_x - origin.world_x(), step),
        vibble::math::floor_div(world_z - origin.world_z(), step)};
}

std::unordered_set<DynamicSpawnRuntime::ChunkKey, DynamicSpawnRuntime::ChunkKeyHash>
DynamicSpawnRuntime::chunk_keys_for_bounds(const world::GridBounds& bounds) const {
    const int min_x = std::min(bounds.min.world_x(), bounds.max.world_x());
    const int max_x = std::max(bounds.min.world_x(), bounds.max.world_x());
    const int min_z = std::min(bounds.min.world_z(), bounds.max.world_z());
    const int max_z = std::max(bounds.min.world_z(), bounds.max.world_z());
    std::unordered_set<ChunkKey, ChunkKeyHash> result;
    if (min_x > max_x || min_z > max_z) {
        return result;
    }
    const ChunkKey min_chunk = chunk_key_for_world(min_x, min_z);
    const ChunkKey max_chunk = chunk_key_for_world(max_x, max_z);
    for (int i = std::min(min_chunk.i, max_chunk.i); i <= std::max(min_chunk.i, max_chunk.i); ++i) {
        for (int j = std::min(min_chunk.j, max_chunk.j); j <= std::max(min_chunk.j, max_chunk.j); ++j) {
            result.insert(ChunkKey{i, j});
        }
    }
    return result;
}

world::GridBounds DynamicSpawnRuntime::expanded_bounds(const world::GridBounds& bounds, int margin_px) const {
    return bounds.expanded(std::max(0, margin_px));
}

bool DynamicSpawnRuntime::room_contains_dynamic_area(const Room* room, SDL_Point point) const {
    if (!room) {
        return false;
    }
    if (room->room_area && room->room_area->contains_point(point)) {
        return true;
    }
    for (const Room::NamedArea& area : room->areas) {
        if (named_area_is_trail(area) && area.area && area.area->contains_point(point)) {
            return true;
        }
    }
    return false;
}

Room* DynamicSpawnRuntime::inherited_room_for_point(SDL_Point point) const {
    for (Room* room : assets_.rooms()) {
        if (room && room->inherits_map_assets() && room_contains_dynamic_area(room, point)) {
            return room;
        }
    }
    return nullptr;
}

const DynamicSpawnRuntime::Candidate*
DynamicSpawnRuntime::pick_candidate(const Selector& selector, const CellKey& key) const {
    if (selector.total_weight <= 0.0 || selector.candidates.empty()) {
        return nullptr;
    }
    std::uint64_t hash = selector.candidate_seed;
    hash = mix_u64(hash, key.selector_id);
    hash = mix_u64(hash, static_cast<std::uint32_t>(key.grid_resolution));
    hash = mix_u64(hash, static_cast<std::uint32_t>(key.grid_x));
    hash = mix_u64(hash, static_cast<std::uint32_t>(key.grid_z));
    hash = mix_u64(hash, static_cast<std::uint64_t>(key.mode));
    const double roll = unit_interval(hash) * selector.total_weight;
    for (std::size_t i = 0; i < selector.cumulative_weights.size(); ++i) {
        if (roll < selector.cumulative_weights[i]) {
            return &selector.candidates[i];
        }
    }
    return &selector.candidates.back();
}

SDL_Point DynamicSpawnRuntime::jittered_world_point(const Selector& selector,
                                                    const CellKey& key,
                                                    SDL_Point base_point) const {
    if (selector.jitter_px <= 0) {
        return base_point;
    }
    std::uint64_t seed = selector.jitter_seed;
    seed = mix_u64(seed, key.selector_id);
    seed = mix_u64(seed, static_cast<std::uint32_t>(key.grid_x));
    seed = mix_u64(seed, static_cast<std::uint32_t>(key.grid_z));
    seed = mix_u64(seed, static_cast<std::uint64_t>(key.mode));
    const double u0 = unit_interval(seed);
    const double u1 = unit_interval(mix_u64(seed, 0x9e3779b97f4a7c15ULL));
    const int jitter = selector.jitter_px;
    return SDL_Point{
        base_point.x + static_cast<int>(std::llround((u0 * 2.0 - 1.0) * static_cast<double>(jitter))),
        base_point.y + static_cast<int>(std::llround((u1 * 2.0 - 1.0) * static_cast<double>(jitter)))};
}

bool DynamicSpawnRuntime::info_allowed(const AssetInfo* info, Mode mode) const {
    const bool boundary = type_is_boundary(info);
    return mode == Mode::BoundaryArea ? boundary : !boundary;
}

int DynamicSpawnRuntime::max_spawn_from_room_px() const {
    const nlohmann::json& map = assets_.map_info_json();
    if (!map.is_object()) {
        return 128;
    }
    auto live_it = map.find("live_dynamic_spawns");
    if (live_it == map.end()) {
        return 128;
    }
    return read_int_setting(*live_it, "max_spawn_from_room", 128, 0, 20000);
}



int DynamicSpawnRuntime::preload_margin_px() const {
    const nlohmann::json& map = assets_.map_info_json();
    const nlohmann::json* camera = nullptr;
    if (map.is_object()) {
        auto it = map.find("camera_settings");
        if (it != map.end() && it->is_object()) {
            camera = &(*it);
        }
    }
    return camera ? read_int_setting(*camera, "live_dynamic_preload_margin_world_px", 192, 0, 100000) : 192;
}

int DynamicSpawnRuntime::despawn_margin_px() const {
    const nlohmann::json& map = assets_.map_info_json();
    const nlohmann::json* camera = nullptr;
    if (map.is_object()) {
        auto it = map.find("camera_settings");
        if (it != map.end() && it->is_object()) {
            camera = &(*it);
        }
    }
    const int preload = preload_margin_px();
    return camera ? std::max(preload, read_int_setting(*camera, "live_dynamic_despawn_margin_world_px", 256, 0, 100000))
                  : 256;
}

} // namespace dynamic_spawn
