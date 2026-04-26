#include "custom_asset_controller.hpp"

#include <SDL3/SDL.h>
#include <algorithm>
#include <cstdint>
#include <cmath>
#include <functional>
#include <string>
#include <unordered_set>

#include "assets/asset/Asset.hpp"
#include "assets/asset/animation.hpp"
#include "assets/asset/asset_library.hpp"
#include "assets/asset/asset_info.hpp"
#include "assets/asset/asset_types.hpp"
#include "animation/animation_update.hpp"
#include "animation/animation_tag_utils.hpp"
#include "animation/controllers/shared/attack_detection_helper.hpp"
#include "animation/controllers/shared/attack_processing_helper.hpp"
#include "core/AssetsManager.hpp"
#include "gameplay/spawn/runtime_candidates.hpp"
#include "utils/log.hpp"
#include "utils/string_utils.hpp"

namespace animation_update::custom_controllers {}
namespace custom_controllers = animation_update::custom_controllers;

namespace {

constexpr std::uint64_t kGoldenRatio = 0x9e3779b97f4a7c15ULL;
constexpr int kAnchorCandidateSpawnRetryLimit = 60;
constexpr double kOrphanGravityUnitsPerSec2 = 2400.0;
constexpr double kOrphanMaxDtSeconds = 1.0 / 20.0;
constexpr double kOrphanMinBounceSpeed = 60.0;
constexpr double kOrphanRestitutionMax = 0.75;
constexpr double kOrphanSettleSpeed = 20.0;

std::uint64_t mix_hash(std::uint64_t seed, std::uint64_t value) {
    seed ^= value + kGoldenRatio + (seed << 6) + (seed >> 2);
    return seed;
}

std::uint64_t hash_signed_int(int value) {
    return static_cast<std::uint64_t>(static_cast<std::int64_t>(value));
}

bool should_use_auto_move_attack_dispatch(const Asset* self) {
    if (!self || !self->info) {
        return false;
    }
    if (asset_types::canonicalize(self->info->type) != asset_types::enemy) {
        return false;
    }
    for (const auto& [animation_id, animation] : self->info->animations) {
        (void)animation_id;
        if (animation_update::tag_utils::has_normalized_tag(animation.tags, "attack")) {
            return true;
        }
    }
    return false;
}

} // namespace

CustomAssetController::CustomAssetController(Asset* self)
    : self_(self) {
    initialize_anchor_candidate_children();
}

CustomAssetController::~CustomAssetController() = default;

void CustomAssetController::update(const Input& in) {
    game_context_ = custom_controllers::build_controller_game_context(self_, assets(), &fly_orbit_target_state_);
    fly_orbit_target_state_ = game_context_.fly_orbit_target;
    tick_anchor_candidate_attachments();
    tick_orphan_fall_state();
    on_update(in);
}

void CustomAssetController::process_pending_attacks(Asset& self) {
    on_process_pending_attacks(self);
}

void CustomAssetController::on_pre_delete(Asset& self) {
    on_parent_pre_delete(self);
}

void CustomAssetController::on_orphaned(Asset& self, Asset* former_parent) {
    on_child_orphaned(self, former_parent);
}

Assets* CustomAssetController::assets() const {
    return self_ ? self_->get_assets() : nullptr;
}

void CustomAssetController::initialize_anchor_candidate_children() {
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
        false
    };

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
            vibble::log::warn(std::string("[CustomAssetController] Invalid explicit anchor candidate payload for anchor '") +
                              anchor_name + "' on asset '" + self_->info->name + "'");
            continue;
        }

        const vibble::spawn::RuntimeCandidates runtime_candidates =
            vibble::spawn::RuntimeCandidates::from_json(*candidates_it);


        const auto resolved = runtime_candidates.pick_hashed(
            anchor_candidate_hash(anchor_name),
            catalog,
            vibble::spawn::ZeroWeightPolicy::NoSelection);
        if (!resolved || resolved->is_null || resolved->resolved_asset_name.empty()) {
            vibble::log::warn(std::string("[CustomAssetController] Unable to resolve explicit anchor candidates for anchor '") +
                              anchor_name + "' on asset '" + self_->info->name + "'");
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
            vibble::log::warn(std::string("[CustomAssetController] Skipping oval fallback with invalid mapping name on asset '") +
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
            vibble::log::warn(std::string("[CustomAssetController] Skipping oval fallback for anchor '") +
                              anchor_name + "' on asset '" + self_->info->name +
                              "' because mapping asset_name is empty");
            continue;
        }
        if (fallback_asset_name == self_->info->name) {
            // Avoid self-recursive attachment defaults; explicit candidates remain authoritative.
            continue;
        }
        if (!owner_assets->library().get(fallback_asset_name)) {
            vibble::log::warn(std::string("[CustomAssetController] Missing oval fallback asset '") +
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

void CustomAssetController::tick_anchor_candidate_attachments() {
    for (auto& attachment : anchor_candidate_children_) {
        if (attachment.orphaned) {
            continue;
        }
        if (!attachment.child.has_value()) {
            continue;
        }

        if (attachment.child->get_asset()) {
            // Live anchor-bound children are synchronized via queued anchor flush.
            // Avoid eager per-controller updates here so parent movement is committed first.
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

void CustomAssetController::orphan_eligible_children(Asset& owner) {
    for (auto& attachment : anchor_candidate_children_) {
        if (attachment.orphaned || !attachment.orphan_on_end || !attachment.child.has_value()) {
            continue;
        }

        Asset* orphaned_child = attachment.child->orphan();
        if (!orphaned_child) {
            continue;
        }
        attachment.orphaned = true;
        orphaned_child->notify_orphaned(&owner);
    }
}

void CustomAssetController::tick_orphan_fall_state() {
    if (!orphan_fall_state_.active || !self_ || self_->dead) {
        return;
    }

    const double dt = std::clamp(
        static_cast<double>(self_->frame_delta_seconds_clamped()),
        1.0 / 240.0,
        kOrphanMaxDtSeconds);

    orphan_fall_state_.velocity_y -= kOrphanGravityUnitsPerSec2 * dt;
    orphan_fall_state_.world_y += orphan_fall_state_.velocity_y * dt;

    const double floor_y = orphan_fall_state_.floor_y;
    if (orphan_fall_state_.world_y <= floor_y) {
        orphan_fall_state_.world_y = floor_y;
        const double impact_speed = std::abs(orphan_fall_state_.velocity_y);
        const double rebound_speed = impact_speed * orphan_fall_state_.restitution;
        if (rebound_speed > kOrphanMinBounceSpeed) {
            orphan_fall_state_.velocity_y = rebound_speed;
        } else {
            orphan_fall_state_.velocity_y = 0.0;
            orphan_fall_state_.active = false;
        }
    }

    if (!orphan_fall_state_.active &&
        std::abs(orphan_fall_state_.world_y - floor_y) <= 0.5 &&
        std::abs(orphan_fall_state_.velocity_y) <= kOrphanSettleSpeed) {
        orphan_fall_state_.world_y = floor_y;
    }

    self_->move_to_world_position(orphan_fall_state_.world_x,
                                  static_cast<int>(std::lround(orphan_fall_state_.world_y)),
                                  orphan_fall_state_.world_z,
                                  orphan_fall_state_.resolution_layer);
}

std::uint64_t CustomAssetController::anchor_candidate_hash(const std::string& anchor_name) const {
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

std::string CustomAssetController::owner_identity_for_anchor_candidates() const {
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

void CustomAssetController::on_update(const Input&) {
    Asset* self = self_ptr();
    if (self && self->info && self->anim_ && self->default_controller_animation_enforced()) {
        const std::string default_anim{ animation_update::detail::kDefaultAnimation };

        auto it = self->info->animations.find(default_anim);
        if (it != self->info->animations.end() && it->second.has_frames()) {
            if (self->current_animation != default_anim || self->current_frame == nullptr) {
                self->anim_->move(SDL_Point{ 0, 0 }, default_anim);
            }
        }
    }

    if (self &&
        !self->current_attack_box_volumes().empty() &&
        !should_use_auto_move_attack_dispatch(self)) {
        custom_controllers::AttackDetectionHelper::send_attacks_to_active_targets(self, assets());
    }
}

void CustomAssetController::on_process_pending_attacks(Asset& self) {
    if (self.has_pending_attacks()) {
        orphan_eligible_children(self);
    }
    custom_controllers::AttackProcessingHelper::process_pending_attacks(self);
}

void CustomAssetController::on_parent_pre_delete(Asset& self) {
    orphan_eligible_children(self);
}

void CustomAssetController::on_child_orphaned(Asset& self, Asset* former_parent) {
    (void)former_parent;
    Assets* owner_assets = self.get_assets();
    if (!owner_assets) {
        return;
    }

    const world::GridPoint floor_point =
        owner_assets->resolve_floor_world_point(SDL_Point{self.world_x(), self.world_z()}, self.grid_resolution);

    orphan_fall_state_.world_x = self.world_x();
    orphan_fall_state_.world_z = self.world_z();
    orphan_fall_state_.resolution_layer = self.grid_resolution;
    orphan_fall_state_.world_y = static_cast<double>(self.world_y());
    orphan_fall_state_.floor_y = static_cast<double>(floor_point.world_y());
    orphan_fall_state_.velocity_y = 0.0;

    const int bounce_amount = self.info ? std::clamp(self.info->bounce_amount, 0, 100) : 0;
    orphan_fall_state_.restitution =
        (static_cast<double>(bounce_amount) / 100.0) * kOrphanRestitutionMax;
    orphan_fall_state_.active = orphan_fall_state_.world_y > orphan_fall_state_.floor_y + 0.5;

    if (!orphan_fall_state_.active) {
        orphan_fall_state_.world_y = orphan_fall_state_.floor_y;
        self.move_to_world_position(orphan_fall_state_.world_x,
                                    static_cast<int>(std::lround(orphan_fall_state_.world_y)),
                                    orphan_fall_state_.world_z,
                                    orphan_fall_state_.resolution_layer);
    }
}
