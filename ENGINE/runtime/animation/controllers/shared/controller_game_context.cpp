#include "animation/controllers/shared/controller_game_context.hpp"

#include "assets/asset/Asset.hpp"
#include "core/AssetsManager.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <string>

namespace animation_update::custom_controllers {

namespace {

constexpr std::uint32_t kOrbitRefreshMinFrames = 1000;
constexpr std::uint32_t kOrbitRefreshMaxFrames = 3000;
constexpr std::uint32_t kOrbitRefreshMinFramesMad = 10;
constexpr std::uint32_t kOrbitRefreshMaxFramesMad = 20;

constexpr int kOrbitHeightMinOffset = 100;
constexpr int kOrbitHeightMaxOffset = 5000;
constexpr float kOrbitPointChangeEpsilon = 4.0f;
constexpr int kOrbitRadiusMin = 20;
constexpr int kOrbitRadiusMax = 40;
constexpr double kTwoPi = 6.28318530717958647692;
constexpr std::uint64_t kGoldenRatio64 = 0x9e3779b97f4a7c15ULL;

const runtime::config::RuntimeGameConfig& default_runtime_game_config() {
    static const runtime::config::RuntimeGameConfig default_config{};
    return default_config;
}

bool is_valid_player_target(const Asset* self, const Asset* player) {
    if (!self || !player || self == player) {
        return false;
    }
    if (player->dead || !player->active) {
        return false;
    }
    return true;
}

bool room_name_empty(const Asset* asset) {
    return !asset || asset->owning_room_name().empty();
}

std::uint64_t splitmix64(std::uint64_t x) {
    x += kGoldenRatio64;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
    return x ^ (x >> 31);
}

std::uint64_t mix_hash(std::uint64_t seed, std::uint64_t value) {
    seed ^= value + kGoldenRatio64 + (seed << 6) + (seed >> 2);
    return seed;
}

std::uint64_t asset_runtime_seed(const Asset* self) {
    std::uint64_t seed = 0;
    if (!self) {
        return splitmix64(seed);
    }

    if (!self->spawn_id.empty()) {
        seed = mix_hash(seed, std::hash<std::string>{}(self->spawn_id));
    } else if (self->info && !self->info->name.empty()) {
        seed = mix_hash(seed, std::hash<std::string>{}(self->info->name));
    } else {
        seed = mix_hash(seed, reinterpret_cast<std::uint64_t>(self));
    }

    seed = mix_hash(seed, static_cast<std::uint64_t>(static_cast<std::int64_t>(self->world_x())));
    seed = mix_hash(seed, static_cast<std::uint64_t>(static_cast<std::int64_t>(self->world_z())));
    seed = mix_hash(seed, static_cast<std::uint64_t>(static_cast<std::int64_t>(self->grid_resolution)));
    return splitmix64(seed);
}

std::uint32_t orbit_refresh_interval_frames(const Asset* self,
                                            std::uint64_t target_id,
                                            std::uint32_t target_version,
                                            bool mad) {
    std::uint64_t seed = asset_runtime_seed(self);
    seed = mix_hash(seed, target_id);
    seed = mix_hash(seed, static_cast<std::uint64_t>(target_version));
    seed = mix_hash(seed, mad ? 1ULL : 0ULL);

    const std::uint64_t random_value = splitmix64(seed);

    const std::uint32_t min_frames = mad ? kOrbitRefreshMinFramesMad : kOrbitRefreshMinFrames;
    const std::uint32_t max_frames = mad ? kOrbitRefreshMaxFramesMad : kOrbitRefreshMaxFrames;
    const std::uint32_t span = (max_frames - min_frames) + 1u;

    return min_frames + static_cast<std::uint32_t>(random_value % span);
}

std::uint32_t schedule_next_refresh_frame(std::uint32_t frame, std::uint32_t interval) {
    const std::uint64_t next = static_cast<std::uint64_t>(frame) + interval;
    return static_cast<std::uint32_t>(
        std::min<std::uint64_t>(next, std::numeric_limits<std::uint32_t>::max()));
}

std::uint64_t orbit_target_id(const Room* room, const FlyOrbitTargetSnapshot& target) {
    std::uint64_t seed = 0;
    if (room) {
        seed = mix_hash(seed, std::hash<std::string>{}(room->room_name));
    }
    seed = mix_hash(seed, static_cast<std::uint64_t>(static_cast<std::int64_t>(target.world_xz.x)));
    seed = mix_hash(seed, static_cast<std::uint64_t>(static_cast<std::int64_t>(target.world_y)));
    seed = mix_hash(seed, static_cast<std::uint64_t>(static_cast<std::int64_t>(target.world_xz.y)));
    seed = mix_hash(seed, static_cast<std::uint64_t>(static_cast<std::int64_t>(target.grid_resolution)));
    return splitmix64(seed);
}

bool fly_orbit_point_changed(const FlyOrbitTargetSnapshot& previous,
                             const FlyOrbitTargetSnapshot& current) {
    if (!previous.valid) {
        return current.valid;
    }
    if (previous.grid_resolution != current.grid_resolution) {
        return true;
    }

    const float dx = static_cast<float>(current.world_xz.x - previous.world_xz.x);
    const float dz = static_cast<float>(current.world_xz.y - previous.world_xz.y);
    const float dy = static_cast<float>(current.world_y - previous.world_y);
    const float dist_sq = dx * dx + dy * dy + dz * dz;

    return dist_sq > (kOrbitPointChangeEpsilon * kOrbitPointChangeEpsilon);
}

bool refresh_due(std::uint32_t frame_id, const FlyOrbitTargetSnapshot& previous) {
    if (!previous.valid) {
        return true;
    }
    return frame_id >= previous.next_refresh_frame;
}

SDL_Point room_fallback_point(const Room& room) {
    if (room.room_area) {
        const auto& points = room.room_area->get_points();
        if (!points.empty()) {
            return points.front();
        }
    }
    return SDL_Point{0, 0};
}

#if !defined(ENGINE_WORLD_TESTS)
const Room* resolve_orbit_room(const ControllerGameContext& context) {
    if (!context.assets) {
        return nullptr;
    }

    if (context.self && !context.self->owning_room_name().empty()) {
        const std::string& self_room_name = context.self->owning_room_name();
        for (const Room* room : context.assets->rooms()) {
            if (!room || !room->room_area) {
                continue;
            }
            if (room->room_name == self_room_name) {
                return room;
            }
        }
    }

    if (context.current_room && context.current_room->room_area) {
        return context.current_room;
    }

    return nullptr;
}

FlyOrbitTargetSnapshot resolve_fly_orbit_target(const ControllerGameContext& context,
                                                const FlyOrbitTargetSnapshot& previous) {
    FlyOrbitTargetSnapshot snapshot = previous;
    const Room* orbit_room = resolve_orbit_room(context);

    if (!context.self || !context.assets || !orbit_room || !orbit_room->room_area) {
        snapshot.valid = false;
        snapshot.next_refresh_frame = schedule_next_refresh_frame(
            context.frame_id,
            orbit_refresh_interval_frames(context.self, 0, previous.target_version, context.Flies_mad));
        return snapshot;
    }

    const bool needs_refresh =
        refresh_due(context.frame_id, previous) ||
        !orbit_room->room_area->contains_point(previous.world_xz);

    if (!needs_refresh) {
        return snapshot;
    }

    const bool use_player_anchor =
        context.player_is_valid() &&
        context.resolved_player &&
        !context.resolved_player->owning_room_name().empty() &&
        context.resolved_player->owning_room_name() == orbit_room->room_name;

    const SDL_Point anchor = use_player_anchor
        ? SDL_Point{context.resolved_player->world_x(), context.resolved_player->world_z()}
        : context.self_world_xz;

    SDL_Point candidate{};

    if (!context.Flies_mad) {
        std::uint64_t seed = asset_runtime_seed(context.self);
        seed = mix_hash(seed, static_cast<std::uint64_t>(context.frame_id));
        seed = mix_hash(seed, static_cast<std::uint64_t>(previous.target_version));

        const std::uint64_t radius_rand = splitmix64(seed);
        const std::uint64_t angle_rand = splitmix64(radius_rand);

        const int radius_span = (kOrbitRadiusMax - kOrbitRadiusMin) + 1;
        const int radius = kOrbitRadiusMin +
            static_cast<int>(radius_rand % static_cast<std::uint64_t>(radius_span));
        const double angle =
            (static_cast<double>(angle_rand & 0xFFFFFFFFULL) / 4294967295.0) * kTwoPi;

        candidate = SDL_Point{
            anchor.x + static_cast<int>(std::lround(std::cos(angle) * static_cast<double>(radius))),
            anchor.y + static_cast<int>(std::lround(std::sin(angle) * static_cast<double>(radius)))
        };
    } else {
        candidate = SDL_Point{
            context.player->world_x() + static_cast<int>(std::lround(std::cos(context.frame_id * 0.1) * 50.0)),
            context.player->world_z() + static_cast<int>(std::lround(std::sin(context.frame_id * 0.1) * 50.0))
        };
    }

    if (!orbit_room->room_area->contains_point(candidate)) {
        candidate = context.self_world_xz;
    }
    if (!orbit_room->room_area->contains_point(candidate)) {
        candidate = orbit_room->room_area->get_center();
    }
    if (!orbit_room->room_area->contains_point(candidate)) {
        candidate = room_fallback_point(*orbit_room);
    }
    if (!orbit_room->room_area->contains_point(candidate)) {
        snapshot.valid = false;
        snapshot.next_refresh_frame = schedule_next_refresh_frame(
            context.frame_id,
            orbit_refresh_interval_frames(context.self, 0, previous.target_version, context.Flies_mad));
        return snapshot;
    }

    const world::GridPoint floor_point =
        context.assets->resolve_floor_world_point(candidate, context.self_grid_resolution);
    const int min_world_y = floor_point.world_y() + kOrbitHeightMinOffset;
    const int max_world_y = floor_point.world_y() + kOrbitHeightMaxOffset;
    const int desired_world_y = context.self ? context.self->world_y() : min_world_y;

    snapshot.valid = true;
    snapshot.world_xz = SDL_Point{floor_point.world_x(), floor_point.world_z()};
    snapshot.world_y = std::clamp(desired_world_y, min_world_y, max_world_y);
    snapshot.grid_resolution = floor_point.resolution_layer();
    snapshot.last_update_frame = context.frame_id;
    snapshot.target_id = orbit_target_id(orbit_room, snapshot);

    const bool changed = fly_orbit_point_changed(previous, snapshot);
    snapshot.target_version = changed ? (previous.target_version + 1u) : previous.target_version;
    snapshot.next_refresh_frame = schedule_next_refresh_frame(
        context.frame_id,
        orbit_refresh_interval_frames(
            context.self,
            snapshot.target_id,
            snapshot.target_version,
            context.Flies_mad));

    return snapshot;
}
#endif

} // namespace

bool ControllerGameContext::has_self() const {
    return self != nullptr;
}

bool ControllerGameContext::has_assets() const {
    return assets != nullptr;
}

bool ControllerGameContext::has_player() const {
    return player != nullptr;
}

bool ControllerGameContext::player_is_valid() const {
    return player_valid && resolved_player != nullptr;
}

bool ControllerGameContext::self_is_player() const {
    return self != nullptr && player != nullptr && self == player;
}

bool ControllerGameContext::has_current_room() const {
    return current_room != nullptr;
}

bool ControllerGameContext::self_has_room_assignment() const {
    return !room_name_empty(self);
}

bool ControllerGameContext::player_has_room_assignment() const {
    return !room_name_empty(player);
}

bool ControllerGameContext::self_in_current_room() const {
    if (!self || !current_room || room_name_empty(self)) {
        return false;
    }
    return self->owning_room_name() == current_room->room_name;
}

bool ControllerGameContext::player_in_current_room() const {
    if (!player || !current_room || room_name_empty(player)) {
        return false;
    }
    return player->owning_room_name() == current_room->room_name;
}

bool ControllerGameContext::self_and_player_share_room() const {
    if (!self || !player || room_name_empty(self) || room_name_empty(player)) {
        return false;
    }
    return self->owning_room_name() == player->owning_room_name();
}

void ControllerGameContext::set_flies_mad() const {
    const_cast<ControllerGameContext*>(this)->Flies_mad = true;
}

bool ControllerGameContext::has_runtime_config() const {
    return runtime_config != nullptr;
}

const runtime::config::RuntimeGameConfig& ControllerGameContext::runtime_game_config() const {
    return runtime_config ? *runtime_config : default_runtime_game_config();
}

const runtime::config::RandomOrbit3DControllerBehaviorConfig&
ControllerGameContext::fly_orbit_behavior_config() const {
    return runtime_game_config().fly_orbit_behavior;
}

ControllerGameContext build_controller_game_context(Asset* self,
                                                    Assets* assets,
                                                    const FlyOrbitTargetSnapshot* previous_orbit_target) {
    ControllerGameContext context{};
    context.self = self;
    context.assets = assets;
    context.player = assets ? assets->player : nullptr;
    context.resolved_player = is_valid_player_target(self, context.player) ? context.player : nullptr;
    context.player_valid = context.resolved_player != nullptr;
    context.runtime_config = assets ? &assets->runtime_game_config() : nullptr;

    if (previous_orbit_target) {
        context.fly_orbit_target = *previous_orbit_target;
        context.fly_orbit_point.valid = previous_orbit_target->valid;
        context.fly_orbit_point.world_xz = previous_orbit_target->world_xz;
        context.fly_orbit_point.world_y = previous_orbit_target->world_y;
        context.fly_orbit_point.grid_resolution = previous_orbit_target->grid_resolution;
    }

    if (self) {
        context.self_world_xz = SDL_Point{self->world_x(), self->world_z()};
        context.self_world_y = self->world_y();
        context.self_grid_resolution = self->grid_resolution;
    }

    if (!assets) {
        return context;
    }

#if !defined(ENGINE_WORLD_TESTS)
    context.frame_id = assets->frame_id();
    context.delta_seconds = assets->frame_delta_seconds_clamped();
    context.camera_view = &assets->getView();
    context.current_room = assets->current_room();

    const FlyOrbitTargetSnapshot previous =
        previous_orbit_target ? *previous_orbit_target : FlyOrbitTargetSnapshot{};
    const FlyOrbitTargetSnapshot resolved = resolve_fly_orbit_target(context, previous);

    context.fly_orbit_target_changed = fly_orbit_point_changed(previous, resolved);
    context.fly_orbit_target = resolved;
    context.fly_orbit_point.valid = resolved.valid;
    context.fly_orbit_point.world_xz = resolved.world_xz;
    context.fly_orbit_point.world_y = resolved.world_y;
    context.fly_orbit_point.grid_resolution = resolved.grid_resolution;
#endif

    return context;
}

} // namespace animation_update::custom_controllers
