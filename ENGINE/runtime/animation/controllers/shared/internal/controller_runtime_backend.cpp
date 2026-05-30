#include "animation/controllers/shared/internal/controller_runtime_backend.hpp"
#include "animation/attack.hpp"
#include "animation/controllers/shared/internal/throw_physics.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <string>
#include <unordered_set>

#include <nlohmann/json.hpp>

#include "assets/asset/Asset.hpp"
#include "assets/asset/asset_info.hpp"
#include "core/AssetsManager.hpp"
#include "gameplay/spawn/runtime_candidates.hpp"
#include "utils/log.hpp"
#include "utils/string_utils.hpp"

namespace animation_update::custom_controllers::internal {

namespace {

constexpr std::uint64_t kGoldenRatio = 0x9e3779b97f4a7c15ULL;
constexpr int kAnchorCandidateSpawnRetryLimit = 60;
constexpr double kOrphanRestitutionMax = 0.75;
constexpr double kOrphanMinImpulseDirection = 1e-4;

std::uint64_t mix_hash(std::uint64_t seed, std::uint64_t value) {
    seed ^= value + kGoldenRatio + (seed << 6) + (seed >> 2);
    return seed;
}

std::uint64_t hash_signed_int(int value) {
    return static_cast<std::uint64_t>(static_cast<std::int64_t>(value));
}

} // namespace

ControllerRuntimeBackend::ControllerRuntimeBackend(Asset* self, bool generic_fallback)
    : self_(self),
      generic_fallback_(generic_fallback) {
    initialize_anchor_candidate_children();
}

bool ControllerRuntimeBackend::requires_runtime_update() const {
    if (!generic_fallback_) {
        return true;
    }
    return orphan_fall_state_.active || !anchor_candidate_children_.empty();
}

void ControllerRuntimeBackend::begin_frame_update() {
    game_context_ = build_controller_game_context(self_, assets(), &fly_orbit_target_state_);
    fly_orbit_target_state_ = game_context_.fly_orbit_target;
    tick_anchor_candidate_attachments();
    tick_orphan_fall_state();
}

Assets* ControllerRuntimeBackend::assets() const {
    return self_ ? self_->get_assets() : nullptr;
}

runtime::context::GameRuntimeContext* ControllerRuntimeBackend::mutable_runtime_game_context() const {
    Assets* owner_assets = assets();
    return owner_assets ? &owner_assets->mutable_game_context() : nullptr;
}

void ControllerRuntimeBackend::initialize_anchor_candidate_children() {
    anchor_candidate_children_.clear();
    if (!self_ || !self_->info) {
        return;
    }

    Assets* owner_assets = assets();
    if (!owner_assets) {
        return;
    }

    const vibble::spawn::RuntimeCandidates::AssetCatalogView catalog{
        &owner_assets->library().all(),
        false};

    std::unordered_set<std::string> seen_anchor_names;
    std::unordered_set<std::string> explicit_anchor_names;

    for (const auto& candidate_entry : self_->info->anchor_point_child_candidates) {
        const std::string anchor_name = vibble::strings::trim_copy(candidate_entry.anchor_point_name);
        if (anchor_name.empty()) {
            continue;
        }
        explicit_anchor_names.insert(anchor_name);
        if (!seen_anchor_names.insert(anchor_name).second) {
            continue;
        }
        const nlohmann::json normalized_candidate_entry =
            self_->info->anchor_point_child_candidate_candidates(anchor_name);
        const auto candidates_it = normalized_candidate_entry.find("candidates");
        if (candidates_it == normalized_candidate_entry.end() || !candidates_it->is_array()) {
            vibble::log::warn(std::string("[CustomControllerBase] Invalid explicit anchor candidate payload for anchor '") +
                              anchor_name + "' on asset '" + self_->info->name + "'");
            continue;
        }

        const vibble::spawn::RuntimeCandidates runtime_candidates =
            vibble::spawn::RuntimeCandidates::from_json(*candidates_it);

        const auto resolved = runtime_candidates.pick_hashed(
            anchor_candidate_hash(anchor_name),
            catalog,
            vibble::spawn::ZeroWeightPolicy::NoSelection);
        if (!resolved.has_value() || resolved->is_null || resolved->resolved_asset_name.empty()) {
            continue;
        }

        AnchorCandidateAttachment attachment{};
        attachment.anchor_name = anchor_name;
        attachment.resolved_asset_name = resolved->resolved_asset_name;
        attachment.remaining_spawn_retries = kAnchorCandidateSpawnRetryLimit;
        attachment.exhausted = false;
        attachment.orphan_on_end = self_->info->anchor_point_child_candidate_orphan_on_end(anchor_name);
        attachment.orphaned = false;
        attachment.child.emplace(*self_, attachment.resolved_asset_name);
        attachment.child->bind(attachment.anchor_name);
        anchor_candidate_children_.push_back(std::move(attachment));
    }

    for (const auto& mapping : self_->info->oval_anchor_mappings) {
        const std::string anchor_name = vibble::strings::trim_copy(mapping.name);
        if (anchor_name.empty()) {
            vibble::log::warn(std::string("[CustomControllerBase] Skipping oval fallback with invalid mapping name on asset '") +
                              self_->info->name + "'");
            continue;
        }
        if (explicit_anchor_names.find(anchor_name) != explicit_anchor_names.end()) {
            continue;
        }
        if (!seen_anchor_names.insert(anchor_name).second) {
            continue;
        }

        const std::string fallback_asset_name = vibble::strings::trim_copy(mapping.asset_name);
        if (fallback_asset_name.empty()) {
            vibble::log::warn(std::string("[CustomControllerBase] Skipping oval fallback for anchor '") +
                              anchor_name + "' on asset '" + self_->info->name +
                              "' because mapping asset_name is empty");
            continue;
        }
        if (fallback_asset_name == self_->info->name) {
            continue;
        }
        if (!owner_assets->library().get(fallback_asset_name)) {
            vibble::log::warn(std::string("[CustomControllerBase] Missing oval fallback asset '") +
                              fallback_asset_name + "' for anchor '" + anchor_name +
                              "' on asset '" + self_->info->name + "'");
            continue;
        }

        AnchorCandidateAttachment attachment{};
        attachment.anchor_name = anchor_name;
        attachment.resolved_asset_name = fallback_asset_name;
        attachment.remaining_spawn_retries = kAnchorCandidateSpawnRetryLimit;
        attachment.exhausted = false;
        attachment.orphan_on_end = true;
        attachment.orphaned = false;
        attachment.child.emplace(*self_, attachment.resolved_asset_name);
        attachment.child->bind(attachment.anchor_name);
        anchor_candidate_children_.push_back(std::move(attachment));
    }
}

void ControllerRuntimeBackend::tick_anchor_candidate_attachments() {
    for (auto& attachment : anchor_candidate_children_) {
        if (attachment.orphaned || !attachment.child.has_value()) {
            continue;
        }

        if (attachment.child->get_asset()) {
            continue;
        }

        if (attachment.exhausted) {
            continue;
        }

        (void)attachment.child->update();
        if (attachment.child->get_asset()) {
            continue;
        }

        if (attachment.remaining_spawn_retries > 0) {
            --attachment.remaining_spawn_retries;
        }
        if (attachment.remaining_spawn_retries <= 0) {
            attachment.exhausted = true;
        }
    }
}

void ControllerRuntimeBackend::orphan_eligible_children(Asset& owner) {
    for (auto& attachment : anchor_candidate_children_) {
        if (attachment.orphaned || !attachment.orphan_on_end || !attachment.child.has_value()) {
            continue;
        }

        Asset* orphaned_child = attachment.child->orphan();
        if (!orphaned_child) {
            continue;
        }
        attachment.orphaned = true;
    }
}

void ControllerRuntimeBackend::on_pre_delete(Asset& owner) {
    orphan_eligible_children(owner);
}

void ControllerRuntimeBackend::tick_orphan_fall_state() {
    if (!orphan_fall_state_.active || !self_ || self_->dead) {
        return;
    }

    throw_physics::StepState step_state{};
    step_state.world_x = orphan_fall_state_.world_x;
    step_state.world_z = orphan_fall_state_.world_z;
    step_state.world_y = orphan_fall_state_.world_y;
    step_state.floor_y = orphan_fall_state_.floor_y;
    step_state.velocity_x = orphan_fall_state_.velocity_x;
    step_state.velocity_z = orphan_fall_state_.velocity_z;
    step_state.velocity_y = orphan_fall_state_.velocity_y;
    step_state.restitution = orphan_fall_state_.restitution;
    step_state.active = orphan_fall_state_.active;

    const auto step_result = throw_physics::step(
        step_state,
        static_cast<double>(self_->frame_delta_seconds_clamped()),
        throw_physics::default_tuning());

    orphan_fall_state_.world_x = step_state.world_x;
    orphan_fall_state_.world_z = step_state.world_z;
    orphan_fall_state_.world_y = step_state.world_y;
    orphan_fall_state_.velocity_x = step_state.velocity_x;
    orphan_fall_state_.velocity_z = step_state.velocity_z;
    orphan_fall_state_.velocity_y = step_state.velocity_y;
    orphan_fall_state_.active = step_state.active;

    if (step_result.hit_floor) {
        resolve_throw_impact_damage(step_result.impact_speed, true);
    } else {
        const double impact_like_speed = std::hypot(
            std::hypot(orphan_fall_state_.velocity_x, orphan_fall_state_.velocity_z),
            orphan_fall_state_.velocity_y);
        resolve_throw_impact_damage(impact_like_speed, false);
    }

    self_->move_to_world_position(static_cast<int>(std::lround(orphan_fall_state_.world_x)),
                                  static_cast<int>(std::lround(orphan_fall_state_.world_y)),
                                  static_cast<int>(std::lround(orphan_fall_state_.world_z)),
                                  orphan_fall_state_.resolution_layer);
}

std::uint64_t ControllerRuntimeBackend::anchor_candidate_hash(const std::string& anchor_name) const {
    std::uint64_t hash = mix_hash(std::hash<std::string>{}(owner_identity_for_anchor_candidates()),
                                  std::hash<std::string>{}(anchor_name));
    if (!self_) {
        return hash;
    }
    hash = mix_hash(hash, hash_signed_int(self_->world_x()));
    hash = mix_hash(hash, hash_signed_int(self_->world_y()));
    hash = mix_hash(hash, hash_signed_int(self_->world_z()));
    return hash;
}

std::string ControllerRuntimeBackend::owner_identity_for_anchor_candidates() const {
    if (!self_) {
        return {};
    }
    if (!self_->spawn_id.empty()) {
        return self_->spawn_id;
    }
    if (self_->info && !self_->info->name.empty()) {
        return self_->info->name;
    }
    return "asset";
}

void ControllerRuntimeBackend::on_orphaned(Asset& self,
                                           Asset* former_parent,
                                           std::optional<OrphanImpulse> impulse) {
    (void)former_parent;
    Assets* owner_assets = self.get_assets();
    if (!owner_assets) {
        return;
    }

    const world::GridPoint floor_point =
        owner_assets->resolve_floor_world_point(SDL_Point{self.world_x(), self.world_z()}, self.grid_resolution);

    orphan_fall_state_.world_x = static_cast<double>(self.world_x());
    orphan_fall_state_.world_z = static_cast<double>(self.world_z());
    orphan_fall_state_.resolution_layer = self.grid_resolution;
    orphan_fall_state_.world_y = static_cast<double>(self.world_y());
    orphan_fall_state_.floor_y = static_cast<double>(floor_point.world_y());
    orphan_fall_state_.velocity_x = 0.0;
    orphan_fall_state_.velocity_z = 0.0;
    orphan_fall_state_.velocity_y = 0.0;
    orphan_fall_state_.source_mass_kg = 0.0;
    orphan_fall_state_.can_deal_throw_damage = false;
    orphan_fall_state_.throw_damage_applied = false;
    orphan_fall_state_.instigator_asset_id.clear();
    orphan_fall_state_.instigator_asset_name.clear();
    if (impulse.has_value()) {
        const double impulse_force = std::max(0.0, static_cast<double>(impulse->force));
        const double raw_x = static_cast<double>(impulse->direction_x);
        const double raw_z = static_cast<double>(impulse->direction_z);
        const double length = std::hypot(raw_x, raw_z);
        if (length > kOrphanMinImpulseDirection) {
            orphan_fall_state_.velocity_x = (raw_x / length) * impulse_force;
            orphan_fall_state_.velocity_z = (raw_z / length) * impulse_force;
        }
        orphan_fall_state_.velocity_y = static_cast<double>(impulse->upward_force);
        orphan_fall_state_.source_mass_kg = std::max(0.0, static_cast<double>(impulse->source_mass_kg));
        orphan_fall_state_.can_deal_throw_damage = impulse->can_deal_throw_damage;
        orphan_fall_state_.instigator_asset_id = impulse->instigator_asset_id;
        orphan_fall_state_.instigator_asset_name = impulse->instigator_asset_name;
    }

    const int bounce_amount = self.info ? std::clamp(self.info->bounce_amount, 0, 100) : 0;
    orphan_fall_state_.restitution =
        (static_cast<double>(bounce_amount) / 100.0) * kOrphanRestitutionMax;
    const throw_physics::Tuning tuning = throw_physics::default_tuning();
    orphan_fall_state_.active =
        orphan_fall_state_.world_y > orphan_fall_state_.floor_y + 0.5 ||
        std::abs(orphan_fall_state_.velocity_y) > tuning.settle_vertical_speed ||
        std::hypot(orphan_fall_state_.velocity_x, orphan_fall_state_.velocity_z) >
            tuning.settle_horizontal_speed;

    if (!orphan_fall_state_.active) {
        orphan_fall_state_.world_y = orphan_fall_state_.floor_y;
        self.move_to_world_position(static_cast<int>(std::lround(orphan_fall_state_.world_x)),
                                    static_cast<int>(std::lround(orphan_fall_state_.world_y)),
                                    static_cast<int>(std::lround(orphan_fall_state_.world_z)),
                                    orphan_fall_state_.resolution_layer);
    }
}

void ControllerRuntimeBackend::resolve_throw_impact_damage(double impact_speed, bool floor_impact) {
    if (!self_ || self_->dead || orphan_fall_state_.throw_damage_applied || !orphan_fall_state_.can_deal_throw_damage) {
        return;
    }
    if (!std::isfinite(impact_speed) || impact_speed <= 0.0) {
        return;
    }
    const int damage = throw_physics::damage_from_impact_energy(orphan_fall_state_.source_mass_kg, impact_speed);
    if (damage <= 0) {
        return;
    }

    Asset* hit_target = nullptr;
    Assets* owner_assets = assets();
    if (owner_assets) {
        const SDL_Point impact_point{
            static_cast<int>(std::lround(orphan_fall_state_.world_x)),
            static_cast<int>(std::lround(orphan_fall_state_.world_z))};
        std::int64_t best_dist_sq = std::numeric_limits<std::int64_t>::max();
        for (Asset* candidate : owner_assets->getActive()) {
            if (!candidate || candidate == self_ || candidate->dead || !candidate->active || !candidate->isHitboxEnabled()) {
                continue;
            }
            const SDL_Point candidate_point{candidate->world_x(), candidate->world_z()};
            const std::int64_t dx = static_cast<std::int64_t>(candidate_point.x) - static_cast<std::int64_t>(impact_point.x);
            const std::int64_t dz = static_cast<std::int64_t>(candidate_point.y) - static_cast<std::int64_t>(impact_point.y);
            const std::int64_t dist_sq = dx * dx + dz * dz;
            if (dist_sq < best_dist_sq && dist_sq <= static_cast<std::int64_t>(90) * 90) {
                best_dist_sq = dist_sq;
                hit_target = candidate;
            }
        }
    }

    auto make_attack = [&](Asset* target) {
        animation_update::Attack attack{};
        attack.attacker_asset_id = orphan_fall_state_.instigator_asset_id;
        attack.attacker_asset_name = orphan_fall_state_.instigator_asset_name.empty()
            ? (self_->info ? self_->info->name : std::string("thrown_object"))
            : orphan_fall_state_.instigator_asset_name;
        attack.target_asset_id = target ? target->spawn_id : std::string{};
        attack.target_asset_name = (target && target->info) ? target->info->name : std::string{};
        attack.attack_type = floor_impact ? "throw_impact_floor" : "throw_impact";
        attack.damage_amount = damage;
        attack.payload.damage_amount = damage;
        attack.payload.hitback_enabled = false;
        attack.hit_x = static_cast<float>(orphan_fall_state_.world_x);
        attack.hit_y = static_cast<float>(orphan_fall_state_.world_y);
        attack.hit_z = static_cast<float>(orphan_fall_state_.world_z);
        attack.tick = owner_assets ? static_cast<std::uint64_t>(owner_assets->frame_id()) : 0ull;
        return attack;
    };

    bool impassable_contact = false;
    if (!floor_impact && !hit_target && owner_assets) {
        std::vector<const Assets::FrameCollisionEntry*> entries;
        owner_assets->query_impassable_entries(*self_, 96, entries);
        const SDL_Point impact_point{
            static_cast<int>(std::lround(orphan_fall_state_.world_x)),
            static_cast<int>(std::lround(orphan_fall_state_.world_z))};
        for (const auto* entry : entries) {
            if (!entry || !entry->asset || entry->asset == self_) {
                continue;
            }
            const bool is_impass = entry->canonical_type == "impassable" || entry->canonical_type == "impassable_area";
            if (!is_impass) {
                continue;
            }
            if (entry->area.contains_point(impact_point)) {
                impassable_contact = true;
                break;
            }
        }
    }

    if (!floor_impact && !hit_target && !impassable_contact) {
        return;
    }

    self_->send_attack(make_attack(self_));
    if (hit_target) {
        hit_target->send_attack(make_attack(hit_target));
    }
    orphan_fall_state_.throw_damage_applied = true;
}

} // namespace animation_update::custom_controllers::internal
