#include "rendering/render/terrain_field.hpp"

#include "gameplay/world/grid_point.hpp"
#include "gameplay/world/world_grid.hpp"
#include "gameplay/map_generation/room.hpp"
#include "utils/area.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>
#include <tuple>

namespace {
inline std::uint64_t mix_uint64(std::uint64_t seed, std::uint64_t value) {
    seed ^= value + 0x9e3779b97f4a7c15ULL + (seed << 6) + (seed >> 2);
    return seed;
}

inline bool is_trail_string(const std::string& text) {
    if (text.size() != 5) return false;
    return std::tolower(static_cast<unsigned char>(text[0])) == 't' &&
           std::tolower(static_cast<unsigned char>(text[1])) == 'r' &&
           std::tolower(static_cast<unsigned char>(text[2])) == 'a' &&
           std::tolower(static_cast<unsigned char>(text[3])) == 'i' &&
           std::tolower(static_cast<unsigned char>(text[4])) == 'l';
}

std::size_t compute_rooms_hash(const std::vector<Room*>& rooms) {
    std::uint64_t h = 0;
    for (const Room* room : rooms) {
        if (!room) {
            h = mix_uint64(h, 0x9e3779b97f4a7c15ULL);
            continue;
        }
        h = mix_uint64(h, reinterpret_cast<std::uint64_t>(room));
        h = mix_uint64(h, static_cast<std::uint64_t>(room->map_origin.first));
        h = mix_uint64(h, static_cast<std::uint64_t>(room->map_origin.second));
        h = mix_uint64(h, static_cast<std::uint64_t>(room->areas.size()));
        h = mix_uint64(h, static_cast<std::uint64_t>(room->type.size()));
    }
    return static_cast<std::size_t>(h);
}

inline float lerp_unclamped(float a, float b, float t) {
    return a + (b - a) * t;
}

inline float point_segment_distance_sq(const SDL_Point& p, const SDL_Point& a, const SDL_Point& b) {
    const float px = static_cast<float>(p.x);
    const float py = static_cast<float>(p.y);
    const float ax = static_cast<float>(a.x);
    const float ay = static_cast<float>(a.y);
    const float bx = static_cast<float>(b.x);
    const float by = static_cast<float>(b.y);
    const float abx = bx - ax;
    const float aby = by - ay;
    const float len_sq = abx * abx + aby * aby;
    if (len_sq < 1e-6f) {
        const float dx = px - ax;
        const float dy = py - ay;
        return dx * dx + dy * dy;
    }
    const float t = std::clamp(((px - ax) * abx + (py - ay) * aby) / len_sq, 0.0f, 1.0f);
    const float proj_x = ax + t * abx;
    const float proj_y = ay + t * aby;
    const float dx = px - proj_x;
    const float dy = py - proj_y;
    return dx * dx + dy * dy;
}
}

TerrainField::TerrainField() = default;

std::size_t TerrainField::CellKeyHash::operator()(const CellKey& key) const noexcept {
    const std::uint64_t a = static_cast<std::uint32_t>(key.x);
    const std::uint64_t b = static_cast<std::uint32_t>(key.y);
    std::uint64_t combined = (a << 32) | b;
    combined ^= combined >> 33;
    combined *= 0xff51afd7ed558ccdULL;
    combined ^= combined >> 33;
    return static_cast<std::size_t>(combined);
}

void TerrainField::reset_cache_if_needed(std::uint64_t frame_id) {
    if (frame_id != last_frame_id_ || cached_revision_ != runtime_revision_) {
        frame_cache_.clear();
        last_frame_id_ = frame_id;
        cached_revision_ = runtime_revision_;
    }
}

void TerrainField::sync_runtime_state(const TerrainRuntimeState& runtime_state,
                                      const std::vector<Room*>& rooms) {
    if (!has_runtime_state_ || runtime_state.revision != runtime_revision_) {
        cached_settings_ = runtime_state.settings;
        cached_settings_.clamp();
        base_seed_ = runtime_state.session_seed;
        runtime_revision_ = runtime_state.revision;
        cached_revision_ = 0;
        last_frame_id_ = 0;
        frame_cache_.clear();
        region_index_.clear();
        region_index_hash_ = 0;
        has_runtime_state_ = true;
    }
    ensure_region_index(rooms);
}

void TerrainField::set_runtime_state(const TerrainRuntimeState& runtime_state,
                                     const std::vector<Room*>& rooms) {
    sync_runtime_state(runtime_state, rooms);
}

void TerrainField::begin_frame(std::uint64_t frame_id,
                               const TerrainRuntimeState& runtime_state,
                               const std::vector<Room*>& rooms) {
    sync_runtime_state(runtime_state, rooms);
    reset_cache_if_needed(frame_id);
}

void TerrainField::ensure_region_index(const std::vector<Room*>& rooms) {
    const std::size_t hash = compute_rooms_hash(rooms);
    if (hash == region_index_hash_) {
        return;
    }
    region_index_hash_ = hash;
    region_index_.clear();

    for (const Room* room : rooms) {
        if (!room) {
            continue;
        }
        const bool room_is_trail = is_trail_string(room->type);
        add_indexed_area(room->room_area.get(), room, room_is_trail);
        for (const auto& named : room->areas) {
            if (!named.area) {
                continue;
            }
            if (!(is_trail_string(named.type) || is_trail_string(named.kind) || is_trail_string(named.name))) {
                continue;
            }
            add_indexed_area(named.area.get(), room, true);
        }
    }
    frame_cache_.clear();
}

void TerrainField::add_indexed_area(const Area* area, const Room* owner, bool is_trail) {
    if (!area || !owner) {
        return;
    }
    int minx = 0;
    int miny = 0;
    int maxx = 0;
    int maxy = 0;
    try {
        std::tie(minx, miny, maxx, maxy) = area->get_bounds();
    } catch (...) {
        return;
    }
    const int width = maxx - minx;
    const int height = maxy - miny;
    if (width <= 0 || height <= 0) {
        return;
    }

    RegionArea indexed{};
    indexed.area = area;
    indexed.owner = owner;
    indexed.bounds = SDL_Rect{minx, miny, width, height};
    indexed.is_trail = is_trail;

    const int start_x = static_cast<int>(std::floor(static_cast<double>(minx) / kRegionIndexCellSize));
    const int end_x   = static_cast<int>(std::floor(static_cast<double>(maxx) / kRegionIndexCellSize));
    const int start_y = static_cast<int>(std::floor(static_cast<double>(miny) / kRegionIndexCellSize));
    const int end_y   = static_cast<int>(std::floor(static_cast<double>(maxy) / kRegionIndexCellSize));

    for (int gx = start_x; gx <= end_x; ++gx) {
        for (int gy = start_y; gy <= end_y; ++gy) {
            region_index_[CellKey{gx, gy}].push_back(indexed);
        }
    }
}

TerrainField::RegionQueryResult TerrainField::query_region(const SDL_Point& pt, float falloff_radius) {
    RegionQueryResult result{};
    if (region_index_.empty()) {
        return result;
    }

    const int cell_x = static_cast<int>(std::floor(static_cast<double>(pt.x) / kRegionIndexCellSize));
    const int cell_y = static_cast<int>(std::floor(static_cast<double>(pt.y) / kRegionIndexCellSize));
    const int search_radius = std::max(0, static_cast<int>(std::ceil(std::max(0.0f, falloff_radius) / static_cast<float>(kRegionIndexCellSize))));

    float best_distance = std::numeric_limits<float>::infinity();

    for (int gx = cell_x - search_radius; gx <= cell_x + search_radius; ++gx) {
        for (int gy = cell_y - search_radius; gy <= cell_y + search_radius; ++gy) {
            auto it = region_index_.find(CellKey{gx, gy});
            if (it == region_index_.end()) {
                continue;
            }
            for (const RegionArea& indexed : it->second) {
                bool inside = false;
                const float dist = distance_to_area(pt, indexed.area, &inside);
                if (inside) {
                    result.inside = true;
                    result.distance = 0.0f;
                    return result;
                }
                if (dist < best_distance) {
                    best_distance = dist;
                }
            }
        }
    }

    result.distance = best_distance;
    return result;
}

float TerrainField::distance_to_area(const SDL_Point& pt, const Area* area, bool* inside) const {
    if (inside) {
        *inside = false;
    }
    if (!area) {
        return std::numeric_limits<float>::infinity();
    }

    int minx = 0;
    int miny = 0;
    int maxx = 0;
    int maxy = 0;
    try {
        std::tie(minx, miny, maxx, maxy) = area->get_bounds();
    } catch (...) {
        return std::numeric_limits<float>::infinity();
    }

    const float px = static_cast<float>(pt.x);
    const float py = static_cast<float>(pt.y);
    const float dx_box = (px < minx) ? static_cast<float>(minx) - px : (px > maxx ? px - static_cast<float>(maxx) : 0.0f);
    const float dy_box = (py < miny) ? static_cast<float>(miny) - py : (py > maxy ? py - static_cast<float>(maxy) : 0.0f);
    float best_dist_sq = dx_box * dx_box + dy_box * dy_box;
    if (best_dist_sq == 0.0f) {
        try {
            if (area->contains_point(pt)) {
                if (inside) {
                    *inside = true;
                }
                return 0.0f;
            }
        } catch (...) {
        }
        best_dist_sq = std::numeric_limits<float>::infinity();
    }

    const auto& points = area->get_points();
    if (points.empty()) {
        return std::sqrt(best_dist_sq);
    }

    if (points.size() == 1) {
        const float dx = px - static_cast<float>(points[0].x);
        const float dy = py - static_cast<float>(points[0].y);
        best_dist_sq = std::min(best_dist_sq, dx * dx + dy * dy);
        return std::sqrt(best_dist_sq);
    }

    const std::size_t count = points.size();
    for (std::size_t i = 0; i < count; ++i) {
        const SDL_Point& a = points[i];
        const SDL_Point& b = points[(i + 1) % count];
        const float dist_sq = point_segment_distance_sq(pt, a, b);
        if (dist_sq < best_dist_sq) {
            best_dist_sq = dist_sq;
            if (best_dist_sq <= 0.0f) {
                return 0.0f;
            }
        }
    }

    return std::sqrt(best_dist_sq);
}

float TerrainField::edge_falloff(float distance, const TerrainSettings& settings) const {
    if (distance <= 0.0f) {
        return 0.0f;
    }
    if (settings.edge_falloff_distance_world <= 0.0f) {
        return 1.0f;
    }
    float t = distance / settings.edge_falloff_distance_world;
    if (t >= 1.0f) {
        return 1.0f;
    }
    t = std::clamp(t, 0.0f, 1.0f);
    float smooth = t * t * (3.0f - 2.0f * t); // smoothstep
    if (settings.blend_strength > 0.0f) {
        const float blend = std::clamp(settings.blend_strength, 0.0f, 1.0f);
        const float softer = smooth * smooth;
        smooth = (1.0f - blend) * smooth + blend * softer;
    }
    return smooth;
}

float TerrainField::hash01(int x, int y, std::uint32_t seed) const {
    std::uint64_t h = static_cast<std::uint64_t>(seed);
    h ^= static_cast<std::uint64_t>(x) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
    h ^= static_cast<std::uint64_t>(y) + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
    h ^= h >> 33;
    h *= 0xff51afd7ed558ccdULL;
    h ^= h >> 33;
    const std::uint32_t upper = static_cast<std::uint32_t>((h >> 32) & 0xffffffffu);
    return static_cast<float>(upper) / static_cast<float>(std::numeric_limits<std::uint32_t>::max());
}

float TerrainField::value_noise(float x, float y, std::uint32_t seed) const {
    const int xi = static_cast<int>(std::floor(x));
    const int yi = static_cast<int>(std::floor(y));
    const float xf = x - static_cast<float>(xi);
    const float yf = y - static_cast<float>(yi);

    const float v00 = hash01(xi, yi, seed);
    const float v10 = hash01(xi + 1, yi, seed);
    const float v01 = hash01(xi, yi + 1, seed);
    const float v11 = hash01(xi + 1, yi + 1, seed);

    const float u = xf * xf * (3.0f - 2.0f * xf);
    const float v = yf * yf * (3.0f - 2.0f * yf);

    const float x1 = lerp_unclamped(v00, v10, u);
    const float x2 = lerp_unclamped(v01, v11, u);
    return lerp_unclamped(x1, x2, v);
}

float TerrainField::fractal_noise(float nx, float ny, const TerrainSettings& settings, std::uint32_t base_seed) const {
    const int octave_count = std::clamp(2 + static_cast<int>(std::round(settings.noise_variation * 3.0f)), 2, 6);
    const float lacunarity = 1.6f + settings.noise_variation * 0.9f;
    const float persistence = std::clamp(settings.roughness, 0.05f, 1.5f);
    float freq = 1.0f / std::max(0.001f, 1.0f + settings.smoothness * 2.5f);
    float amp = 1.0f;
    float sum = 0.0f;
    float weight = 0.0f;

    for (int i = 0; i < octave_count; ++i) {
        const std::uint32_t octave_seed = static_cast<std::uint32_t>(base_seed + static_cast<std::uint32_t>(i) * 0x9e3779u);
        const float sample = value_noise(nx * freq, ny * freq, octave_seed);
        sum += sample * amp;
        weight += amp;
        freq *= lacunarity;
        amp *= persistence;
    }

    if (weight <= 0.0f) {
        return 0.0f;
    }

    float normalized = sum / weight; // 0..1
    if (settings.blend_strength > 0.0f) {
        const float blend = std::clamp(settings.blend_strength * 0.65f, 0.0f, 1.0f);
        const float softened = normalized * normalized;
        normalized = (1.0f - blend) * normalized + blend * softened;
    }
    return std::clamp(normalized, 0.0f, 1.0f);
}

float TerrainField::sample_elevation(const world::GridKey& key,
                                     const world::WorldGrid& grid,
                                     const std::vector<Room*>& rooms,
                                     const TerrainRuntimeState& runtime_state,
                                     std::uint64_t frame_id) {
    sync_runtime_state(runtime_state, rooms);
    reset_cache_if_needed(frame_id);

    if (!cached_settings_.enabled || cached_settings_.max_elevation_world <= 0.0f) {
        return 0.0f;
    }

    std::uint64_t hash_key = world::GridPoint::hash_key(key);
    auto cache_it = frame_cache_.find(hash_key);
    if (cache_it != frame_cache_.end()) {
        return cache_it->second;
    }

    int layer = key.layer;
    if (layer < 0) {
        layer = grid.default_resolution_layer();
    }
    layer = std::clamp(layer, 0, grid.max_resolution_layers());
    int spacing = grid.grid_spacing_for_layer(layer);
    if (spacing <= 0) {
        spacing = 1;
    }

    std::uint64_t seed64 = base_seed_;
    if (cached_settings_.light.lock_seed_to_world) {
        seed64 = mix_uint64(seed64, static_cast<std::uint64_t>(key.x));
        seed64 = mix_uint64(seed64, static_cast<std::uint64_t>(key.y));
    }
    const std::uint32_t seed32 = static_cast<std::uint32_t>(seed64 & 0xffffffffu);

    const float base_cell = std::max(1.0f, static_cast<float>(spacing) * cached_settings_.resolution_density_scale);
    const float nx = static_cast<float>(key.x) / base_cell;
    const float ny = static_cast<float>(key.y) / base_cell;

    float height = fractal_noise(nx, ny, cached_settings_, seed32) * cached_settings_.max_elevation_world;

    const RegionQueryResult region = query_region(SDL_Point{key.x, key.y}, cached_settings_.edge_falloff_distance_world);
    if (region.inside) {
        height = 0.0f;
    } else {
        const float fade = edge_falloff(region.distance, cached_settings_);
        height *= fade;
    }

    frame_cache_.emplace(hash_key, height);
    return height;
}
