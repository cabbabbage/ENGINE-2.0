#include "grid_point.hpp"
#include "assets/asset/Asset.hpp"
#include "gameplay/world/world_grid.hpp"

#include <algorithm>
#include <cmath>
#include <optional>

namespace world {

namespace {
    // Vec3-like operations for projection math
    struct Vec3 {
        double x = 0.0, y = 0.0, z = 0.0;
    };

    inline Vec3 operator-(const Vec3& a, const Vec3& b) {
        return {a.x - b.x, a.y - b.y, a.z - b.z};
    }

    inline double dot(const Vec3& a, const Vec3& b) {
        return a.x * b.x + a.y * b.y + a.z * b.z;
    }

    inline double length(const Vec3& v) {
        return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
    }
} // namespace

std::size_t GridKeyHash::operator()(const GridKey& key) const noexcept {
    std::size_t h1 = std::hash<int>{}(key.x);
    std::size_t h2 = std::hash<int>{}(key.y);
    std::size_t h3 = std::hash<int>{}(key.z);
    std::size_t h4 = std::hash<int>{}(key.layer);
    return ((h1 ^ (h2 << 1)) >> 1) ^ (h3 << 1) ^ (h4 << 3);
}

GridPoint::GridPoint(int world_x,
                     int world_y,
                     int world_z,
                     int resolution_layer,
                     GridCoord grid_idx,
                     GridCoord chunk_idx,
                     GridId legacy_id,
                     Chunk* owning_chunk,
                     GridPoint* parent_point,
                     bool is_virtual_point)
    : id(legacy_id)
    , world_pos_{world_x, world_y, world_z}
    , resolution_layer_(resolution_layer)
    , parent_(parent_point)
    , is_virtual_(is_virtual_point)
    , is_floor(world_y == 0)
    , grid_index(grid_idx)
    , chunk_index(chunk_idx)
    , chunk(owning_chunk) {}

GridPoint::GridPoint(const GridPoint& other)
    : id(other.id)
    , world_pos_(other.world_pos_)
    , resolution_layer_(other.resolution_layer_)
    , parent_(other.parent_)
    , is_virtual_(other.is_virtual_)
    , is_floor(other.is_floor)
    , grid_index(other.grid_index)
    , chunk_index(other.chunk_index)
    , chunk(other.chunk)
    , projection_(other.projection_)
    , occupants()  // Leave empty - unique_ptrs cannot be copied
    , children_with_assets(0)  // Reset - no assets copied
    , active_child_mask(0)  // Reset - no child state copied
    , x_child_neg_(other.x_child_neg_)
    , x_child_pos_(other.x_child_pos_)
    , y_child_neg_(other.y_child_neg_)
    , y_child_pos_(other.y_child_pos_)
    , z_child_neg_(other.z_child_neg_)
    , z_child_pos_(other.z_child_pos_)
{
    // Note: occupants vector is intentionally left empty
    // Virtual points (used in GridBounds) don't have assets
    // Real points with assets should use move semantics, not copy
}

GridPoint::GridPoint(GridPoint&&) noexcept = default;

GridPoint::~GridPoint() = default;

std::string GridPoint::debug_identity_and_mask() const {
    // Compact identity + branch state for dev tools / logging.
    const axis::WorldPos canonical = world_position();
    return "id=" + std::to_string(id) +
           " world=(" + std::to_string(canonical.x) + "," + std::to_string(canonical.y) + "," + std::to_string(canonical.z) + ")" +
           " layer=" + std::to_string(resolution_layer_) +
           " grid_index=(" + std::to_string(grid_index.x) + "," + std::to_string(grid_index.z) + ")" +
           " chunk_index=(" + std::to_string(chunk_index.x) + "," + std::to_string(chunk_index.z) + ")" +
           (is_virtual_ ? " virtual=1" : " virtual=0") +
           " assets=" + std::to_string(occupants.size()) +
           " children_with_assets=" + std::to_string(children_with_assets) +
           " mask=0x" + to_hex(active_child_mask);
}

void GridPoint::project_to_screen(const CameraProjectionParams& params) {
    // Track which camera state we're using
    projection_.last_camera_state_version = params.state_version;

    // Convert world position to meters
    const axis::WorldPos canonical_world = world_position();
    const double safe_scale = std::max(1e-6, params.meters_scale);
    const Vec3 world_meters{
        (static_cast<double>(canonical_world.x) - params.anchor_world_x) * safe_scale,
        (static_cast<double>(canonical_world.y) - params.anchor_world_y) * safe_scale,
        (static_cast<double>(canonical_world.z) - params.anchor_world_z) * safe_scale
    };

    // Camera vectors
    const Vec3 cam_pos{params.position_x, params.position_y, params.position_z};
    const Vec3 cam_forward{params.forward_x, params.forward_y, params.forward_z};
    const Vec3 cam_right{params.right_x, params.right_y, params.right_z};
    const Vec3 cam_up{params.up_x, params.up_y, params.up_z};

    // Transform to camera space
    const Vec3 to_point = world_meters - cam_pos;
    const double depth_along_forward = dot(to_point, cam_forward);
    const double distance_sq = dot(to_point, to_point);

    // Early rejection for points behind or outside clipping planes (using squared distances to avoid sqrt)
    const double near_plane_sq = params.near_plane * params.near_plane;
    const double far_plane_sq = params.far_plane * params.far_plane;
    if (depth_along_forward <= params.near_plane ||
        distance_sq < near_plane_sq ||
        distance_sq > far_plane_sq ||
        !std::isfinite(distance_sq)) {
        projection_.screen = SDL_FPoint{0.0f, 0.0f};
        projection_.parallax_dx = 0.0f;
        projection_.on_screen = false;
        projection_.screen_data_valid = true;
        return;
    }

    // Compute actual distance only after rejection checks pass
    const double distance = std::sqrt(distance_sq);

    // Project to camera space
    const double cam_x = dot(to_point, cam_right);
    const double cam_y = dot(to_point, cam_up);

    // Project to NDC
    const double tan_fov_x = std::max(1e-6, params.tan_half_fov_x);
    const double tan_fov_y = std::max(1e-6, params.tan_half_fov_y);
    const double ndc_x = (cam_x / depth_along_forward) / tan_fov_x;
    const double ndc_y = (cam_y / depth_along_forward) / tan_fov_y;

    if (!std::isfinite(ndc_x) || !std::isfinite(ndc_y)) {
        projection_.screen = SDL_FPoint{0.0f, 0.0f};
        projection_.parallax_dx = 0.0f;
        projection_.on_screen = false;
        projection_.screen_data_valid = true;
        return;
    }

    // NDC to screen (with zoom)
    const double scaled_x = ndc_x * params.screen_zoom;
    const double scaled_y = ndc_y * params.screen_zoom;
    const double screen_x = (scaled_x * 0.5 + 0.5) * static_cast<double>(params.screen_width);
    const double screen_y = (0.5 - scaled_y * 0.5) * static_cast<double>(params.screen_height) + params.screen_pan_y_px;

    // Calculate perspective scale
    const double safe_width = static_cast<double>(std::max(1, params.screen_width));
    const double pixels_per_meter = (safe_width * 0.5) * params.screen_zoom / tan_fov_x;
    double scale = params.meters_scale * pixels_per_meter / depth_along_forward;

    // Calculate vertical scale by projecting unit vectors
    float vert_scale = 1.0f;
    const double unit_meters = std::max(1e-6, params.meters_scale);
    const Vec3 unit_world_x{unit_meters, 0.0, 0.0};
    const Vec3 unit_world_y{0.0, unit_meters, 0.0};

    auto screen_distance_for_delta = [&](const Vec3& unit_world) -> std::optional<double> {
        const Vec3 delta_cam{
            dot(unit_world, cam_right),
            dot(unit_world, cam_up),
            dot(unit_world, cam_forward)
        };
        const double depth2 = depth_along_forward + delta_cam.z;
        if (!std::isfinite(depth2) || depth2 <= params.near_plane) {
            return std::nullopt;
        }
        const double ndc_x2 = ((cam_x + delta_cam.x) / depth2) / tan_fov_x;
        const double ndc_y2 = ((cam_y + delta_cam.y) / depth2) / tan_fov_y;
        if (!std::isfinite(ndc_x2) || !std::isfinite(ndc_y2)) {
            return std::nullopt;
        }
        const double sx2 = (ndc_x2 * params.screen_zoom * 0.5 + 0.5) * safe_width;
        const double sy2 = (0.5 - ndc_y2 * params.screen_zoom * 0.5) * params.screen_height + params.screen_pan_y_px;
        const double dx = sx2 - (scaled_x * 0.5 + 0.5) * safe_width;
        const double dy = sy2 - screen_y;
        const double dist = std::sqrt(dx * dx + dy * dy);
        if (!std::isfinite(dist) || dist <= 1e-6) {
            return std::nullopt;
        }
        return dist;
    };

    if (const auto scale_x = screen_distance_for_delta(unit_world_x)) {
        scale = *scale_x;
    }
    if (const auto scale_y = screen_distance_for_delta(unit_world_y)) {
        if (std::isfinite(scale) && scale > 1e-6) {
            vert_scale = static_cast<float>(*scale_y / scale);
        }
    }

    // Clamp perspective scale
    if (params.near_camera_max_perspective_scale > 0.0f) {
        scale = std::min(scale, static_cast<double>(params.near_camera_max_perspective_scale));
    }
    if (!std::isfinite(scale) || scale <= 0.0) {
        scale = 1.0;
    }
    if (!std::isfinite(vert_scale) || vert_scale <= 0.0f) {
        vert_scale = 1.0f;
    }

    // Calculate horizon fade
    const float zoom_scale = std::isfinite(params.screen_zoom) && params.screen_zoom > 0.0
        ? static_cast<float>(params.screen_zoom) : 1.0f;
    const float effective_horizon_band = params.horizon_band_px * zoom_scale;

    float h_fade = 1.0f;
    if (effective_horizon_band > 0.0f) {
        const float dist_from_horizon = static_cast<float>(screen_y - params.horizon_screen_y);
        if (dist_from_horizon <= 0.0f) {
            h_fade = 0.0f;
        } else if (dist_from_horizon < effective_horizon_band) {
            const float t = dist_from_horizon / effective_horizon_band;
            h_fade = std::clamp(t * t * t, 0.0f, 1.0f);
        }
    }

    // Calculate near-camera fade
    float near_fade = 1.0f;
    if (std::isfinite(screen_y) && params.screen_height > 0) {
        const float screen_h = static_cast<float>(params.screen_height);
        const float offscreen_amount = std::max(0.0f, params.offscreen_fade_amount_px);
        if (static_cast<float>(screen_y) >= screen_h) {
            if (offscreen_amount <= 0.0f) {
                near_fade = 0.0f;
            } else {
                const float t = (static_cast<float>(screen_y) - screen_h) / offscreen_amount;
                near_fade = std::clamp(1.0f - t, 0.0f, 1.0f);
            }
        }
    }

    // Store results
    projection_.screen = SDL_FPoint{static_cast<float>(screen_x), static_cast<float>(screen_y)};
    projection_.parallax_dx = 0.0f;
    projection_.vertical_scale = vert_scale;
    projection_.perspective_scale = static_cast<float>(std::max(0.0001, scale));
    projection_.horizon_fade_alpha = h_fade;
    projection_.near_camera_fade_alpha = near_fade;
    projection_.distance_to_camera = static_cast<float>(distance);
    projection_.tilt_radians = static_cast<float>(params.pitch_radians);
    projection_.on_screen = std::isfinite(screen_x) && std::isfinite(screen_y);
    projection_.screen_data_valid = true;
}

GridPoint& GridPoint::operator=(GridPoint&& other) noexcept {
    if (this == &other) return *this;

    // Transfer projection cache state.
    projection_ = other.projection_;
    is_floor = other.is_floor;

    // Transfer occupants (ownership)
    occupants = std::move(other.occupants);

    // Transfer hierarchy tracking
    children_with_assets = other.children_with_assets;
    active_child_mask = other.active_child_mask;

    // Transfer child pointers
    x_child_neg_ = other.x_child_neg_;
    x_child_pos_ = other.x_child_pos_;
    y_child_neg_ = other.y_child_neg_;
    y_child_pos_ = other.y_child_pos_;
    z_child_neg_ = other.z_child_neg_;
    z_child_pos_ = other.z_child_pos_;

    // Clear source child pointers
    other.x_child_neg_ = nullptr;
    other.x_child_pos_ = nullptr;
    other.y_child_neg_ = nullptr;
    other.y_child_pos_ = nullptr;
    other.z_child_neg_ = nullptr;
    other.z_child_pos_ = nullptr;
    other.children_with_assets = 0;
    other.active_child_mask = 0;
    other.is_floor = false;
    other.projection_.reset();

    return *this;
}

GridKey GridPoint::key() const {
    const axis::WorldPos canonical = world_position();
    return GridKey{canonical.x, canonical.y, canonical.z, resolution_layer_};
}

GridId GridPoint::hash_key() const {
    return hash_key(key());
}

GridId GridPoint::hash_key(const GridKey& key) {
    return static_cast<GridId>(GridKeyHash{}(key));
}

GridPoint& GridPoint::from_world(int x, int y, int z, int layer, WorldGrid& grid) {
    return from_world(axis::WorldPos{x, y, z}, layer, grid);
}

GridPoint& GridPoint::from_world(const axis::WorldPos& pos, int layer, WorldGrid& grid) {
    const GridPoint world_point = GridPoint::make_virtual(pos, layer);
    const GridKey key = grid.grid_key_from_world(world_point, layer);
    Chunk* owning_chunk = grid.ensure_chunk_from_world(world_point);
    GridPoint& gp = grid.find_or_create_grid_point(key, owning_chunk, nullptr);
    gp.is_floor = (gp.world_y() == 0);
    return gp;
}

GridPoint& GridPoint::from_world(const GridKey& key, WorldGrid& grid) {
    const axis::WorldPos world_pos{key.x, key.y, key.z};
    const GridPoint world_point = GridPoint::make_virtual(world_pos, key.layer);
    Chunk* owning_chunk = grid.ensure_chunk_from_world(world_point);
    GridPoint& gp = grid.find_or_create_grid_point(key, owning_chunk, nullptr);
    gp.is_floor = (gp.world_y() == 0);
    return gp;
}

GridPoint* GridPoint::from_screen(const SDL_FPoint& screen,
                                  float world_y_target,
                                  const CameraProjectionParams& params,
                                  WorldGrid& grid) {
    if (params.screen_width <= 0 || params.screen_height <= 0) {
        return nullptr;
    }

    const double zoom = std::max(1e-6, params.screen_zoom);
    const double ndc_x = (static_cast<double>(screen.x) / static_cast<double>(params.screen_width) * 2.0 - 1.0) / zoom;
    const double ndc_y = (1.0 - (static_cast<double>(screen.y) - params.screen_pan_y_px) / static_cast<double>(params.screen_height) * 2.0) / zoom;

    const double tan_fov_x = std::max(1e-6, params.tan_half_fov_x);
    const double tan_fov_y = std::max(1e-6, params.tan_half_fov_y);

    // Solve for depth so that resulting world_y matches the requested height (ground plane assumption).
    const Vec3 cam_pos{params.position_x, params.position_y, params.position_z};
    const Vec3 cam_forward{params.forward_x, params.forward_y, params.forward_z};
    const Vec3 cam_right{params.right_x, params.right_y, params.right_z};
    const Vec3 cam_up{params.up_x, params.up_y, params.up_z};

    const double denom = cam_forward.y + ndc_x * tan_fov_x * cam_right.y + ndc_y * tan_fov_y * cam_up.y;
    const double target_y_meters = static_cast<double>(world_y_target) * std::max(1e-6, params.meters_scale);
    if (std::abs(denom) <= 1e-9) {
        return nullptr;
    }
    const double depth = (target_y_meters - cam_pos.y) / denom;
    if (!std::isfinite(depth) || depth <= params.near_plane || depth >= params.far_plane) {
        return nullptr;
    }

    const double cam_x = depth * ndc_x * tan_fov_x;
    const double cam_y = depth * ndc_y * tan_fov_y;
    const Vec3 world_meters{
        cam_pos.x + cam_forward.x * depth + cam_right.x * cam_x + cam_up.x * cam_y,
        cam_pos.y + cam_forward.y * depth + cam_right.y * cam_x + cam_up.y * cam_y,
        cam_pos.z + cam_forward.z * depth + cam_right.z * cam_x + cam_up.z * cam_y
    };

    const double safe_scale = std::max(1e-6, params.meters_scale);
    const int world_x_px = static_cast<int>(std::lround(world_meters.x / safe_scale + params.anchor_world_x));
    const int world_y_px = static_cast<int>(std::lround(world_meters.y / safe_scale + params.anchor_world_y));
    const int world_z_px = static_cast<int>(std::lround(world_meters.z / safe_scale + params.anchor_world_z));

    const GridPoint world_point = GridPoint::make_virtual(axis::WorldPos{world_x_px, world_y_px, world_z_px}, -1);
    const GridKey key = grid.grid_key_from_world(world_point, -1);
    Chunk* owning_chunk = grid.ensure_chunk_from_world(world_point);
    GridPoint& gp = grid.find_or_create_grid_point(key, owning_chunk, nullptr);
    gp.is_floor = (gp.world_y() == 0);
    return &gp;
}

GridPoint GridPoint::make_virtual(const axis::WorldPos& world_pos,
                                  int resolution_layer) {
    const GridKey key{world_pos.x, world_pos.y, world_pos.z, resolution_layer};
    const GridId virtual_id = hash_key(key);
    return GridPoint(world_pos.x,
                     world_pos.y,
                     world_pos.z,
                     resolution_layer,
                     GridCoord{0, 0},
                     GridCoord{0, 0},
                     virtual_id,
                     nullptr,
                     nullptr,
                     true);
}

GridPoint GridPoint::make_virtual(int world_x,
                                  int world_y,
                                  int world_z,
                                  int resolution_layer) {
    return make_virtual(axis::WorldPos{world_x, world_y, world_z}, resolution_layer);
}

void GridPoint::update_world_position(const axis::WorldPos& new_pos) {
    world_pos_ = new_pos;
    is_floor = (new_pos.y == 0);
    // Invalidate screen data since position changed
    projection_.invalidate();
}

void GridPoint::update_world_position(int new_x, int new_y, int new_z) {
    update_world_position(axis::WorldPos{new_x, new_y, new_z});
}

void GridPoint::set_world_z(int new_z) {
    axis::WorldPos next_pos = world_position();
    next_pos.z = new_z;
    update_world_position(next_pos);
}

void swap(GridPoint& a, GridPoint& b) noexcept {
    using std::swap;

    // Swap projection cache.
    swap(a.projection_, b.projection_);
    swap(a.is_floor, b.is_floor);

    // Swap occupants
    swap(a.occupants, b.occupants);

    // Swap hierarchy tracking
    swap(a.children_with_assets, b.children_with_assets);
    swap(a.active_child_mask, b.active_child_mask);

    // Swap child pointers
    swap(a.x_child_neg_, b.x_child_neg_);
    swap(a.x_child_pos_, b.x_child_pos_);
    swap(a.y_child_neg_, b.y_child_neg_);
    swap(a.y_child_pos_, b.y_child_pos_);
    swap(a.z_child_neg_, b.z_child_neg_);
    swap(a.z_child_pos_, b.z_child_pos_);
}

GridBounds GridBounds::from_xywh(int x, int z, int w, int h, int world_y, int layer) {
    const int max_x = x + std::max(0, w - 1);
    const int max_z = z + std::max(0, h - 1);
    const axis::WorldPos min_pos{x, world_y, z};
    const axis::WorldPos max_pos{max_x, world_y, max_z};
    GridPoint min = GridPoint::make_virtual(min_pos, layer);
    GridPoint max = GridPoint::make_virtual(max_pos, layer);
    return GridBounds(min, max);
}

GridBounds GridBounds::from_min_max(const GridPoint& min_pt, const GridPoint& max_pt) {
    return GridBounds(min_pt, max_pt);
}

bool GridBounds::contains(const GridPoint& pt) const {
    return pt.world_x() >= min.world_x() && pt.world_x() <= max.world_x() &&
           pt.world_z() >= min.world_z() && pt.world_z() <= max.world_z();
}

GridBounds GridBounds::expanded(int margin) const {
    const int m = std::max(0, margin);
    const axis::WorldPos expanded_min{min.world_x() - m, min.world_y(), min.world_z() - m};
    const axis::WorldPos expanded_max{max.world_x() + m, max.world_y(), max.world_z() + m};
    GridPoint new_min = GridPoint::make_virtual(expanded_min, min.resolution_layer());
    GridPoint new_max = GridPoint::make_virtual(expanded_max, max.resolution_layer());
    return GridBounds(new_min, new_max);
}

SDL_Rect GridBounds::to_sdl_rect() const {
    return SDL_Rect{
        min.world_x(),
        min.world_z(),
        std::max(0, max.world_x() - min.world_x() + 1),
        std::max(0, max.world_z() - min.world_z() + 1)
    };
}

SDL_FRect GridBounds::to_sdl_frect() const {
    return SDL_FRect{
        static_cast<float>(min.world_x()),
        static_cast<float>(min.world_z()),
        static_cast<float>(std::max(0, max.world_x() - min.world_x() + 1)),
        static_cast<float>(std::max(0, max.world_z() - min.world_z() + 1))
    };
}

} // namespace world
