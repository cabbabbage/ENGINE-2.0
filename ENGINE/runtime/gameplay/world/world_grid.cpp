#include "gameplay/world/world_grid.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <string>
#include <utility>

#include "assets/Asset.hpp"
#include "rendering/render/warped_screen_grid.hpp"
#include "utils/log.hpp"
#include "gameplay/world/chunk.hpp"
#include "gameplay/world/chunk_manager.hpp"

namespace world {

namespace {

int grid_floor_div(int numerator, int denominator) {
    if (denominator == 0) {
        return 0;
    }
    return static_cast<int>(std::floor(static_cast<double>(numerator) / denominator));
}

constexpr float kParallaxEpsilon = 1e-3f;
constexpr int   kDefaultWorldZ   = 0;
constexpr int   kDefaultMaxLayers = 10;

GridPoint world_point_for_asset(const Asset* asset, int resolution_layer = -1) {
    if (!asset) {
        return GridPoint::make_virtual(0, 0, 0, 0);
    }
    const int resolved_layer = (resolution_layer >= 0) ? resolution_layer : asset->grid_resolution;
    return GridPoint::make_virtual(asset->world_x(), asset->world_y(), asset->world_z(), resolved_layer);
}

template <typename PointPtr>
inline bool world_point_in_rect(const PointPtr* gp, const GridBounds& rect) {
    if (!gp) return false;
    return rect.contains(*gp);
}

std::optional<GridPoint::ChildDirection> direction_from_parent(const GridPoint* parent, const GridPoint* child) {
    if (!parent) {
        return std::nullopt;
    }
    return parent->direction_for_child(child);
}

}

int WorldGrid::power_of_three(int exponent) {
    if (exponent <= 0) {
        return 1;
    }
    int result = 1;
    for (int i = 0; i < exponent; ++i) {
        if (result > std::numeric_limits<int>::max() / 3) {
            return std::numeric_limits<int>::max();
        }
        result *= 3;
    }
    return result;
}

int WorldGrid::max_resolution_layers() const {
    return (max_resolution_layers_ > 0) ? max_resolution_layers_ : kDefaultMaxLayers;
}

int WorldGrid::distance_for_layer(int layer) const {
    const int max_layers = max_resolution_layers();
    const int clamped_layer = std::clamp(layer, 0, max_layers);
    const int exponent = std::max(0, max_layers - clamped_layer);
    const int dist = power_of_three(exponent);
    return dist;
}

int WorldGrid::grid_spacing_for_layer(int layer) const {
    const int dist = distance_for_layer(layer);
    return dist;
}

WorldGrid::WorldGrid(const GridPoint& origin, int r_chunk)
    : origin_(origin)
    , r_chunk_(std::clamp(r_chunk, 0, vibble::grid::kMaxResolution))
    , grid_resolution_(r_chunk_)
    , max_resolution_layers_(kDefaultMaxLayers) {
    // Map Grid ownership: all GridPoints created through ensure_point/ensure_child
    // live inside this container. Screen Grid rebuilds receive non-owning pointers
    // only; do not transfer ownership out of WorldGrid. ChunkManager remains the
    // legacy compatibility path for tiles during migration.
    invalidate_active_cache();
}

void WorldGrid::set_chunk_resolution(int r) {
    const int clamped = std::clamp(r, 0, vibble::grid::kMaxResolution);
    if (clamped != r) {
        vibble::log::warn(std::string{"[WorldGrid] Requested chunk resolution "} +
                          std::to_string(r) +
                          " clamped to " + std::to_string(clamped) +
                          " (max=" + std::to_string(vibble::grid::kMaxResolution) + ")");
    }
    if (clamped == r_chunk_) {
        return;
    }
    r_chunk_ = clamped;
    invalidate_active_cache();
}

void WorldGrid::set_origin(const GridPoint& origin) {
    origin_ = GridPoint::make_virtual(origin.world_x(), origin.world_y(), origin.world_z(), origin.resolution_layer());
    invalidate_active_cache();
}

void WorldGrid::invalidate_active_cache() {
    chunks_.clear_active();
}

std::uint64_t WorldGrid::next_traversal_stamp() const {
    const std::uint64_t next = traversal_stamp_ + 1;
    traversal_stamp_ = (next == 0) ? 1 : next;
    return traversal_stamp_;
}

const ChunkManager& WorldGrid::chunks() const {
    return chunks_;
}

ChunkManager& WorldGrid::chunks() {
    return chunks_;
}

GridId WorldGrid::make_point_id(int i, int j) const {
    const std::uint32_t ux = static_cast<std::uint32_t>(i);
    const std::uint32_t uy = static_cast<std::uint32_t>(j);
    return (static_cast<GridId>(ux) << 32) | static_cast<GridId>(uy);
}

GridKey WorldGrid::grid_key_from_world(const GridPoint& world, int world_z, int layer) const {
    const int resolution_layer = (layer >= 0) ? layer : world.resolution_layer();
    return GridKey{world.world_x(), world.world_y(), world_z, resolution_layer};
}

GridKey WorldGrid::grid_key_from_world(const GridPoint& world_point) const {
    return GridKey{world_point.world_x(), world_point.world_y(), world_point.world_z(), world_point.resolution_layer()};
}

std::size_t WorldGrid::hash_key(const GridKey& key) {
    return GridKeyHash{}(key);
}

GridPoint* WorldGrid::point_for_id(GridId id) {
    auto it = points_.find(id);
    if (it == points_.end()) {
        return nullptr;
    }
    return &it->second;
}

const GridPoint* WorldGrid::point_for_id(GridId id) const {
    auto it = points_.find(id);
    if (it == points_.end()) {
        return nullptr;
    }
    return &it->second;
}

GridPoint* WorldGrid::point_for_asset(const Asset* asset) {
    if (!asset) {
        return nullptr;
    }
    auto key_it = asset_to_key_.find(const_cast<Asset*>(asset));
    if (key_it == asset_to_key_.end()) {
        return nullptr;
    }
    return find_grid_point_strict(key_it->second);
}

const GridPoint* WorldGrid::point_for_asset(const Asset* asset) const {
    if (!asset) {
        return nullptr;
    }
    auto key_it = asset_to_key_.find(const_cast<Asset*>(asset));
    if (key_it == asset_to_key_.end()) {
        return nullptr;
    }
    return find_grid_point_strict(key_it->second);
}

Asset* WorldGrid::create_asset_at_point(std::unique_ptr<Asset> a, int world_z, int resolution_layer) {
    return register_asset(std::move(a), world_z, resolution_layer);
}

Asset* WorldGrid::create_asset_at_point(Asset* a, int world_z, int resolution_layer) {
    return register_asset(std::unique_ptr<Asset>(a), world_z, resolution_layer);
}

Asset* WorldGrid::move_asset_to_point(Asset* a, const GridPoint& old_pos, const GridPoint& new_pos) {
    return move_asset(a, old_pos, new_pos);
}

Asset* WorldGrid::remove_asset(Asset* a) {
    if (!a) {
        return nullptr;
    }

    bool removed_from_point = false;
    auto key_lookup = asset_to_key_.find(a);
    if (key_lookup != asset_to_key_.end()) {
        if (GridPoint* gp = find_grid_point_strict(key_lookup->second)) {
            remove_asset_from_point(a, *gp);
            removed_from_point = true;
        }
        asset_to_key_.erase(key_lookup);
    }

    if (!removed_from_point) {
        for (auto& entry : points_) {
            auto& point = entry.second;
            auto it = std::find_if(point.occupants.begin(), point.occupants.end(),
                [a](const std::unique_ptr<Asset>& up) { return up.get() == a; });
            if (it != point.occupants.end()) {
                remove_asset_from_point(a, point);
                removed_from_point = true;
                break;
            }
        }
    }

    auto it = residency_.find(a);
    if (it != residency_.end()) {
        remove_from_chunk(a, it->second);
        residency_.erase(it);
    }

    prune_empty_points();

    return a;
}

std::vector<Asset*> WorldGrid::all_assets() const {
    std::vector<Asset*> out;
    out.reserve(asset_to_key_.size());
    for (const auto& entry : asset_to_key_) {
        out.push_back(entry.first);
    }
    return out;
}

void WorldGrid::remove_asset_from_point(Asset* a, GridPoint& point) {
    if (!a) {
        return;
    }
    (void)detach_asset_from_grid_point(a, point, true);
}

std::unique_ptr<Asset> WorldGrid::detach_asset_from_grid_point(Asset* a, GridPoint& point, bool clear_mapping) {
    if (!a) {
        return nullptr;
    }
    const bool had_assets_before = point.has_assets_or_active_children();
    auto it = std::find_if(point.occupants.begin(), point.occupants.end(),
        [a](const std::unique_ptr<Asset>& up) { return up.get() == a; });
    if (it == point.occupants.end()) {
        return nullptr;
    }
    std::unique_ptr<Asset> owned = std::move(*it);
    point.occupants.erase(it);
    point.invalidate_screen_data();
    if (clear_mapping) {
        asset_to_key_.erase(a);
    }
    a->pos_ = nullptr;
    a->clear_grid_id();
    SDL_assert(point.occupants.empty() || point.has_assets_or_active_children());
    const bool has_after = point.has_assets_or_active_children();
    if (!has_after) {
        SDL_assert(point.children_with_assets == 0);
        SDL_assert(point.active_child_mask == 0);
    }
    if (had_assets_before && !has_after) {
        propagate_branch_inactive(&point);
    }
    return owned;
}

void WorldGrid::attach_asset_to_grid_point(std::unique_ptr<Asset> owned, Asset* raw, GridPoint& point) {
    Asset* target = raw ? raw : owned.get();
    if (!target && !owned) {
        return;
    }
    const bool had_assets_before = point.has_assets_or_active_children();
    if (owned) {
        point.occupants.push_back(std::move(owned));
    } else {
        point.occupants.push_back(std::unique_ptr<Asset>(target));
    }
    bind_asset_to_point(target, point);
    point.invalidate_screen_data();
    SDL_assert(!point.occupants.empty());
    SDL_assert(point.has_assets_or_active_children());
    if (!had_assets_before && point.has_assets_or_active_children()) {
        propagate_branch_active(&point);
    }
}

GridPoint& WorldGrid::ensure_point(GridCoord grid_index, GridCoord chunk_index, Chunk* owning_chunk, GridPoint* parent, int world_z, int resolution_layer_override) {
    const GridId id = make_point_id(grid_index.x, grid_index.y);
    const int resolution_layer = (resolution_layer_override >= 0) ? resolution_layer_override : default_resolution_layer();
    const int spacing = grid_spacing_for_layer(resolution_layer);
    const int canonical_world_x = origin_.world_x() + grid_index.x * spacing;
    const int canonical_world_y = origin_.world_y() + grid_index.y * spacing;
    const GridKey canonical_key{canonical_world_x, canonical_world_y, world_z, resolution_layer};

    auto [it, inserted] = points_.try_emplace(
        id,
        canonical_world_x,
        canonical_world_y,
        world_z,
        resolution_layer,
        grid_index,
        chunk_index,
        id,
        owning_chunk,
        parent);

    GridPoint& point = it->second;
    if (inserted && parent == nullptr) {
        add_root_id(id);
    }
    key_to_id_[canonical_key] = id;
    return point;
}

GridPoint& WorldGrid::ensure_child(GridPoint& parent, GridPoint::ChildDirection dir, const GridKey& child_key, Chunk* owning_chunk) {
    GridPoint& child = find_or_create_grid_point(child_key, owning_chunk, &parent);
    if (child.parent() != &parent && child.parent() != nullptr) {
        vibble::log::warn("[WorldGrid] ensure_child parent mismatch; Map Grid must keep hierarchy links consistent during migration (Phase 2 - 3d_refactor_plan.md).");
    }
    if (child.parent() != &parent) {
        // Re-rooting should be avoided; warn and set only if null.
        if (child.parent() == nullptr) {
            child.set_child(GridPoint::ChildDirection::XNeg, nullptr); // placeholder no-op to keep API symmetry
        }
    }
    parent.set_child(dir, &child);
    if (child.has_assets_or_active_children()) {
        parent.set_branch_bit_for_child(&child);
    }
    return child;
}

GridKey WorldGrid::grid_key_from_legacy(GridPoint grid_index, int world_z, int layer) const {
    const int resolution_layer = (layer >= 0) ? layer : default_resolution_layer();
    const int spacing = grid_spacing_for_layer(resolution_layer);
    const int wx = origin_.world_x() + grid_index.world_x() * spacing;
    const int wy = origin_.world_y() + grid_index.world_y() * spacing;
    return GridKey{wx, wy, world_z, resolution_layer};
}

GridPoint* WorldGrid::find_grid_point(const GridKey& key) {
    auto it = key_to_id_.find(key);
    if (it != key_to_id_.end()) {
        return point_for_id(it->second);
    }
    return nullptr;
}

const GridPoint* WorldGrid::find_grid_point(const GridKey& key) const {
    auto it = key_to_id_.find(key);
    if (it != key_to_id_.end()) {
        return point_for_id(it->second);
    }
    return nullptr;
}

GridPoint* WorldGrid::find_grid_point_strict(const GridKey& key) {
    auto it = key_to_id_.find(key);
    if (it == key_to_id_.end()) {
        return nullptr;
    }
    return point_for_id(it->second);
}

const GridPoint* WorldGrid::find_grid_point_strict(const GridKey& key) const {
    auto it = key_to_id_.find(key);
    if (it == key_to_id_.end()) {
        return nullptr;
    }
    return point_for_id(it->second);
}

GridPoint& WorldGrid::find_or_create_grid_point(const GridKey& key, Chunk* owning_chunk, GridPoint* parent) {
    if (GridPoint* existing = find_grid_point(key)) {
        return *existing;
    }

    const GridPoint world = GridPoint::make_virtual(key.x, key.y, key.z, key.layer);
    const GridCoord grid_idx = grid_index_from_world(world, key.layer);
    const int chunk_step = 1 << r_chunk_;
    GridCoord chunk_idx{0, 0};
    if (chunk_step > 0) {
        chunk_idx.x = grid_floor_div(world.world_x() - origin_.world_x(), chunk_step);
        chunk_idx.y = grid_floor_div(world.world_y() - origin_.world_y(), chunk_step);
    }
    GridPoint& point = ensure_point(grid_idx, chunk_idx, owning_chunk, parent, key.z, key.layer);

    const bool identity_mismatch = (point.world_z() != key.z) || (point.resolution_layer() != key.layer);
    if (identity_mismatch) {
        vibble::log::warn("[WorldGrid] find_or_create_grid_point created point with legacy identity that differs from requested 3D key; migration path only (Phase 2 - 3d_refactor_plan.md).");
    }
    key_to_id_[key] = point.id;
    return point;
}

void WorldGrid::debug_validate_keys_and_masks() const {
    for (const auto& entry : points_) {
        const GridPoint& gp = entry.second;
        GridKey expected_key{gp.world_x(), gp.world_y(), gp.world_z(), gp.resolution_layer()};
        auto key_it = key_to_id_.find(expected_key);
        if (key_it == key_to_id_.end() || key_it->second != gp.id) {
            vibble::log::warn("[WorldGrid] Key mismatch for point id=" + std::to_string(gp.id) +
                              " identity=" + gp.debug_identity_and_mask());
        }
    }
}

namespace {
template <typename PointPtr>
std::vector<PointPtr*> gather_roots(const std::unordered_map<GridId, GridPoint>& points,
                                    const std::vector<GridId>& root_ids) {
    std::vector<PointPtr*> out;
    if (!root_ids.empty()) {
        out.reserve(root_ids.size());
        for (GridId id : root_ids) {
            auto it = points.find(id);
            if (it != points.end()) {
                out.push_back(const_cast<PointPtr*>(&it->second));
            }
        }
    } else {
        out.reserve(points.size());
        for (const auto& entry : points) {
            if (entry.second.parent() == nullptr) {
                out.push_back(const_cast<PointPtr*>(&entry.second));
            }
        }
    }
    return out;
}
}

std::vector<GridPoint*> WorldGrid::query_region(const GridBounds& world_bounds,
                                                int min_layer,
                                                int max_layer,
                                                int min_world_z,
                                                int max_world_z,
                                                bool skip_inactive_branches,
                                                bool include_empty_nodes,
                                                RegionMetrics* metrics) {
    const int safe_min_layer = std::max(0, std::min(min_layer, max_layer));
    const int safe_max_layer = std::max(safe_min_layer, max_layer);
    const int safe_min_z = std::min(min_world_z, max_world_z);
    const int safe_max_z = std::max(min_world_z, max_world_z);

    std::vector<GridPoint*> result;
    const std::uint64_t visit_stamp = next_traversal_stamp();
    std::vector<GridPoint*> stack = gather_roots<GridPoint>(points_, roots_);

    auto push_child = [&](GridPoint* parent, GridPoint::ChildDirection dir) {
        GridPoint* child = parent ? parent->child(dir) : nullptr;
        if (!child) {
            return;
        }
        if (skip_inactive_branches && !parent->child_active(dir)) {
            if (metrics) ++metrics->branches_skipped;
            return;
        }
        stack.push_back(child);
    };

    while (!stack.empty()) {
        GridPoint* node = stack.back();
        stack.pop_back();
        if (!node) continue;
        if (node->last_region_query_stamp == visit_stamp) {
            continue;
        }
        node->last_region_query_stamp = visit_stamp;

        if (skip_inactive_branches && !node->has_assets_or_active_children()) {
            if (metrics) ++metrics->branches_skipped;
            continue;
        }
        if (metrics) ++metrics->nodes_visited;

        const bool in_layer = node->resolution_layer() >= safe_min_layer && node->resolution_layer() <= safe_max_layer;
        const bool in_z     = node->world_z() >= safe_min_z && node->world_z() <= safe_max_z;
        const bool in_bounds = world_point_in_rect(node, world_bounds);
        const bool include_node = in_layer && in_z && in_bounds && (include_empty_nodes || !node->occupants.empty());
        if (include_node) {
            result.push_back(node);
        }

        push_child(node, GridPoint::ChildDirection::XNeg);
        push_child(node, GridPoint::ChildDirection::XPos);
        push_child(node, GridPoint::ChildDirection::YNeg);
        push_child(node, GridPoint::ChildDirection::YPos);
        push_child(node, GridPoint::ChildDirection::ZNeg);
        push_child(node, GridPoint::ChildDirection::ZPos);
    }

    return result;
}

std::vector<const GridPoint*> WorldGrid::query_region(const GridBounds& world_bounds,
                                                      int min_layer,
                                                      int max_layer,
                                                      int min_world_z,
                                                      int max_world_z,
                                                      bool skip_inactive_branches,
                                                      bool include_empty_nodes,
                                                      RegionMetrics* metrics) const {
    const int safe_min_layer = std::max(0, std::min(min_layer, max_layer));
    const int safe_max_layer = std::max(safe_min_layer, max_layer);
    const int safe_min_z = std::min(min_world_z, max_world_z);
    const int safe_max_z = std::max(min_world_z, max_world_z);

    std::vector<const GridPoint*> result;
    const std::uint64_t visit_stamp = next_traversal_stamp();
    std::vector<const GridPoint*> stack = gather_roots<const GridPoint>(points_, roots_);

    auto push_child = [&](const GridPoint* parent, GridPoint::ChildDirection dir) {
        const GridPoint* child = parent ? parent->child(dir) : nullptr;
        if (!child) {
            return;
        }
        if (skip_inactive_branches && !parent->child_active(dir)) {
            if (metrics) ++metrics->branches_skipped;
            return;
        }
        stack.push_back(child);
    };

    while (!stack.empty()) {
        const GridPoint* node = stack.back();
        stack.pop_back();
        if (!node) continue;
        if (node->last_region_query_stamp == visit_stamp) {
            continue;
        }
        node->last_region_query_stamp = visit_stamp;

        if (skip_inactive_branches && !node->has_assets_or_active_children()) {
            if (metrics) ++metrics->branches_skipped;
            continue;
        }
        if (metrics) ++metrics->nodes_visited;

        const bool in_layer = node->resolution_layer() >= safe_min_layer && node->resolution_layer() <= safe_max_layer;
        const bool in_z     = node->world_z() >= safe_min_z && node->world_z() <= safe_max_z;
        const bool in_bounds = world_point_in_rect(node, world_bounds);
        const bool include_node = in_layer && in_z && in_bounds && (include_empty_nodes || !node->occupants.empty());
        if (include_node) {
            result.push_back(node);
        }

        push_child(node, GridPoint::ChildDirection::XNeg);
        push_child(node, GridPoint::ChildDirection::XPos);
        push_child(node, GridPoint::ChildDirection::YNeg);
        push_child(node, GridPoint::ChildDirection::YPos);
        push_child(node, GridPoint::ChildDirection::ZNeg);
        push_child(node, GridPoint::ChildDirection::ZPos);
    }

    return result;
}

std::unique_ptr<Asset> WorldGrid::extract_from_point(Asset* a, GridPoint& point) {
    if (!a) {
        return nullptr;
    }
    auto it = std::find_if(point.occupants.begin(), point.occupants.end(),
        [a](const std::unique_ptr<Asset>& up) { return up.get() == a; });
    if (it == point.occupants.end()) {
        return nullptr;
    }
    std::unique_ptr<Asset> owned = std::move(*it);
    point.occupants.erase(it);
    if (owned) {
        owned->clear_grid_id();
    }
    return owned;
}

void WorldGrid::bind_asset_to_point(Asset* a, GridPoint& point) {
    if (!a) {
        return;
    }
    a->pos_ = &point;
    GridKey key{point.world_x(), point.world_y(), point.world_z(), point.resolution_layer()};
    asset_to_key_[a] = key;
    a->clear_grid_id();
}

void WorldGrid::prune_empty_points() {
    for (auto it = points_.begin(); it != points_.end(); ) {
        if (!it->second.has_assets_or_active_children()) {
            GridPoint& gp = it->second;
            GridKey key{gp.world_x(), gp.world_y(), gp.world_z(), gp.resolution_layer()};
            key_to_id_.erase(key);
            remove_root_id(it->first);
            it = points_.erase(it);
        } else {
            ++it;
        }
    }
}

void WorldGrid::add_root_id(GridId id) {
    if (std::find(roots_.begin(), roots_.end(), id) == roots_.end()) {
        roots_.push_back(id);
    }
}

void WorldGrid::remove_root_id(GridId id) {
    auto it = std::remove(roots_.begin(), roots_.end(), id);
    if (it != roots_.end()) {
        roots_.erase(it, roots_.end());
    }
}

Asset* WorldGrid::register_asset(std::unique_ptr<Asset> a, int world_z, int resolution_layer) {
    if (!a) {
        return nullptr;
    }
    Asset* raw = a.get();
    const int chunk_step = 1 << r_chunk_;
    if (chunk_step <= 0) {
        return raw;
    }

    const int resolved_layer = (resolution_layer >= 0) ? resolution_layer : default_resolution_layer();
    const GridPoint world_pos = world_point_for_asset(raw, resolved_layer);
    const GridCoord grid_index = grid_index_from_world(world_pos, resolved_layer);
    const GridKey new_key = grid_key_from_world(world_pos, world_z, resolved_layer);

    auto existing_key_it = asset_to_key_.find(raw);
    if (existing_key_it != asset_to_key_.end() && existing_key_it->second != new_key) {
        if (GridPoint* existing_point = find_grid_point_strict(existing_key_it->second)) {
            remove_asset_from_point(raw, *existing_point);
        }
        asset_to_key_.erase(existing_key_it);
        prune_empty_points();
    }

    const int i = grid_floor_div(world_pos.world_x() - origin_.world_x(), chunk_step);
    const int j = grid_floor_div(world_pos.world_y() - origin_.world_y(), chunk_step);
    const GridCoord chunk_index{i, j};
    Chunk& chunk = chunks_.ensure(chunk_index.x, chunk_index.y, r_chunk_, origin_);

    auto ensure_asset_in_chunk = [&]() {
        auto it = std::find(chunk.assets.begin(), chunk.assets.end(), raw);
        if (it == chunk.assets.end()) {
            chunk.assets.push_back(raw);
        }
};

    auto existing = residency_.find(raw);
    if (existing != residency_.end()) {
        Chunk* previous = existing->second;
        if (previous == &chunk) {
            ensure_asset_in_chunk();
            return raw;
        }
        remove_from_chunk(raw, previous);
        existing->second = &chunk;
    } else {
        residency_[raw] = &chunk;
    }
    ensure_asset_in_chunk();

    GridPoint& point = ensure_point(grid_index, chunk_index, &chunk, nullptr, new_key.z, new_key.layer);
    attach_asset_to_grid_point(std::move(a), raw, point);
    return raw;
}

Asset* WorldGrid::register_asset(Asset* a, int world_z, int resolution_layer) {
    return register_asset(std::unique_ptr<Asset>(a), world_z, resolution_layer);
}

Chunk* WorldGrid::ensure_chunk_from_world(const GridPoint& world_px) {
    const int chunk_step = 1 << r_chunk_;
    if (chunk_step <= 0) {
        return nullptr;
    }
    const int i = grid_floor_div(world_px.world_x() - origin_.world_x(), chunk_step);
    const int j = grid_floor_div(world_px.world_y() - origin_.world_y(), chunk_step);
    return get_or_create_chunk_ij(i, j);
}

Chunk* WorldGrid::chunk_from_world(const GridPoint& world_px) const {
    const int chunk_step = 1 << r_chunk_;
    if (chunk_step <= 0) {
        return nullptr;
    }
    const int i = grid_floor_div(world_px.world_x() - origin_.world_x(), chunk_step);
    const int j = grid_floor_div(world_px.world_y() - origin_.world_y(), chunk_step);
    return chunks_.find(i, j);
}

Chunk* WorldGrid::get_or_create_chunk_ij(int i, int j) {
    return &chunks_.ensure(i, j, r_chunk_, origin_);
}

void WorldGrid::remove_from_chunk(Asset* a, Chunk* c) {
    if (!a || !c) {
        return;
    }
    auto it = std::find(c->assets.begin(), c->assets.end(), a);
    if (it != c->assets.end()) {
        c->assets.erase(it);
    }
}

Asset* WorldGrid::move_asset(Asset* a, const GridPoint& old_pos, const GridPoint& new_pos) {
    if (!a) {
        return nullptr;
    }
    const int resolved_layer = new_pos.resolution_layer();
    const GridCoord old_index = grid_index_from_world(old_pos, old_pos.resolution_layer());
    const GridCoord new_index = grid_index_from_world(new_pos, resolved_layer);
    const int chunk_step = 1 << r_chunk_;
    if (chunk_step <= 0) {
        return nullptr;
    }
    const int old_i = grid_floor_div(old_pos.world_x() - origin_.world_x(), chunk_step);
    const int old_j = grid_floor_div(old_pos.world_y() - origin_.world_y(), chunk_step);
    const int new_i = grid_floor_div(new_pos.world_x() - origin_.world_x(), chunk_step);
    const int new_j = grid_floor_div(new_pos.world_y() - origin_.world_y(), chunk_step);
    const GridCoord chunk_index{new_i, new_j};

    Chunk* previous = nullptr;
    auto existing = residency_.find(a);
    if (existing != residency_.end()) {
        previous = existing->second;
    } else {
        previous = chunks_.find(old_i, old_j);
    }
    Chunk& target = chunks_.ensure(new_i, new_j, r_chunk_, origin_);

    if (previous != &target) {
        if (previous) {
            remove_from_chunk(a, previous);
        }
        if (std::find(target.assets.begin(), target.assets.end(), a) == target.assets.end()) {
            target.assets.push_back(a);
        }
        residency_[a] = &target;
    }

    // PRESERVE perspective scale before detaching from old point
    float preserved_perspective = 1.0f;
    if (a->pos_ && a->pos_->perspective_scale > 0.0001f) {
        preserved_perspective = a->pos_->perspective_scale;
    }

    std::unique_ptr<Asset> owned;
    const bool point_changed = (old_pos.world_x() != new_pos.world_x()) || (old_pos.world_y() != new_pos.world_y());
    if (point_changed) {
        if (GridPoint* existing_point = point_for_asset(a)) {
            owned = detach_asset_from_grid_point(a, *existing_point, true);
        }
    }

    const GridKey new_key = grid_key_from_world(new_pos, new_pos.world_z(), resolved_layer);
    GridPoint& point = ensure_point(new_index, chunk_index, &target, nullptr, new_key.z, new_key.layer);

    if (point_changed) {
        if (owned) {
            attach_asset_to_grid_point(std::move(owned), nullptr, point);
        } else {
            attach_asset_to_grid_point(nullptr, a, point);
        }
        // Initialize new point's perspective_scale if not yet calculated
        // This prevents a single frame of wrong scaling during movement
        if (point.perspective_scale <= 0.0001f || !point.screen_data_valid) {
            point.perspective_scale = preserved_perspective;
        }
    } else {
        point.invalidate_screen_data();
    }
    prune_empty_points();

    return a;
}

void WorldGrid::unregister_asset(Asset* a) {
    (void)remove_asset(a);
}

void WorldGrid::rebuild_chunks() {
    std::vector<std::unique_ptr<Asset>> owned_assets;
    for (auto& entry : points_) {
        for (auto& occ : entry.second.occupants) {
            if (occ) {
                owned_assets.push_back(std::move(occ));
            }
        }
        entry.second.occupants.clear();
    }
    points_.clear();
    residency_.clear();
    asset_to_key_.clear();
    roots_.clear();
    chunks_.reset();
    invalidate_active_cache();

    for (auto& uptr : owned_assets) {
        register_asset(std::move(uptr));
    }
}

const std::vector<Chunk*>& WorldGrid::active_chunks() const {
    return chunks_.active();
}

void WorldGrid::update_active_chunks(const GridBounds& camera_world, int margin_px) {
    const int margin = std::max(0, margin_px);
    GridBounds expanded = camera_world.expanded(margin);
    const int min_x = std::min(expanded.min.world_x(), expanded.max.world_x());
    const int max_x = std::max(expanded.min.world_x(), expanded.max.world_x());
    const int min_y = std::min(expanded.min.world_y(), expanded.max.world_y());
    const int max_y = std::max(expanded.min.world_y(), expanded.max.world_y());

    const bool needs_update = !has_cached_camera_rect_ ||
        last_margin_px_ != margin_px ||
        last_chunk_resolution_ != r_chunk_ ||
        last_expanded_camera_.min.world_x() != min_x ||
        last_expanded_camera_.min.world_y() != min_y ||
        last_expanded_camera_.max.world_x() != max_x ||
        last_expanded_camera_.max.world_y() != max_y;

    if (!needs_update) {
        return;
    }

    chunks_.clear_active();
    auto& active = chunks_.active();
    const int chunk_step = 1 << r_chunk_;
    if (chunk_step > 0 && max_x >= min_x && max_y >= min_y) {
        const int min_i = grid_floor_div(min_x - origin_.world_x(), chunk_step);
        const int min_j = grid_floor_div(min_y - origin_.world_y(), chunk_step);
        const int max_i = grid_floor_div(max_x - origin_.world_x(), chunk_step);
        const int max_j = grid_floor_div(max_y - origin_.world_y(), chunk_step);
        if (max_i >= min_i && max_j >= min_j) {
            for (int i = min_i; i <= max_i; ++i) {
                for (int j = min_j; j <= max_j; ++j) {
                    if (Chunk* chunk = chunks_.find(i, j)) {
                        active.push_back(chunk);
                    }
                }
            }
        }
    }

    last_expanded_camera_ = std::move(expanded);
    last_margin_px_ = margin_px;
    last_chunk_resolution_ = r_chunk_;
    has_cached_camera_rect_ = true;
}

int WorldGrid::default_resolution_layer() const {
    // Finest available layer by default; grid_resolution_ is tile-only.
    return max_resolution_layers();
}

void WorldGrid::set_grid_resolution(int r) {
    const int clamped = std::clamp(r, 0, vibble::grid::kMaxResolution);
    grid_resolution_ = clamped;
}

int WorldGrid::grid_resolution() const {
    return grid_resolution_;
}

GridCoord WorldGrid::grid_index_from_world(const GridPoint& world_point, int layer_override) const {
    const int resolution_layer = (layer_override >= 0) ? layer_override : world_point.resolution_layer();
    const int spacing = grid_spacing_for_layer(resolution_layer);
    if (spacing <= 0) {
        return GridCoord{0, 0};
    }
    const int i = grid_floor_div(world_point.world_x() - origin_.world_x(), spacing);
    const int j = grid_floor_div(world_point.world_y() - origin_.world_y(), spacing);
    return GridCoord{i, j};
}

void WorldGrid::propagate_branch_active(GridPoint* node) {
    GridPoint* child = node;
    SDL_assert(!child || child->has_assets_or_active_children());
    GridPoint* parent = child ? child->parent() : nullptr;
    while (parent) {
        const bool parent_was_active = parent->has_assets_or_active_children();
        const auto dir = direction_from_parent(parent, child);
        SDL_assert(dir.has_value());
        if (!dir.has_value()) {
            break;
        }
        const bool bit_changed = parent->set_branch_bit_for_child(child);
        if (!bit_changed && !parent_was_active) {
            break;
        }
        if (parent_was_active) {
            break;
        }
        child = parent;
        parent = parent->parent();
    }
}

void WorldGrid::propagate_branch_inactive(GridPoint* node) {
    GridPoint* child = node;
    SDL_assert(!child || !child->has_assets_or_active_children());
    GridPoint* parent = child ? child->parent() : nullptr;
    while (parent) {
        const bool parent_was_active = parent->has_assets_or_active_children();
        const auto dir = direction_from_parent(parent, child);
        if (!dir.has_value()) {
            break;
        }
        const bool bit_cleared = parent->clear_branch_bit_for_child(child);
        if (child && child->occupants.empty() && child->active_child_mask == 0) {
            SDL_assert(!parent->child_active(*dir));
        }
        if (!bit_cleared && !parent_was_active) {
            break;
        }
        if (parent->has_assets_or_active_children()) {
            break;
        }
        SDL_assert(parent->children_with_assets == 0);
        child = parent;
        parent = parent->parent();
    }
}

void WorldGrid::attach_asset_to_hierarchy(GridPoint& point) {
    propagate_branch_active(&point);
}

void WorldGrid::detach_asset_from_hierarchy(GridPoint& point) {
    if (!point.has_assets_or_active_children()) {
        propagate_branch_inactive(&point);
    }
}

} // namespace world
