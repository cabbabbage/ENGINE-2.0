#include "custom_asset_controller.hpp"

#include <SDL3/SDL.h>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_set>

#include "assets/asset/Asset.hpp"
#include "assets/asset/animation.hpp"
#include "assets/asset/asset_library.hpp"
#include "assets/asset/asset_info.hpp"
#include "animation/animation_update.hpp"
#include "animation/controllers/shared/attack_detection_helper.hpp"
#include "core/AssetsManager.hpp"
#include "gameplay/spawn/runtime_candidates.hpp"

namespace animation_update::custom_controllers {}
namespace custom_controllers = animation_update::custom_controllers;

namespace {

constexpr std::uint64_t kGoldenRatio = 0x9e3779b97f4a7c15ULL;
constexpr int kAnchorCandidateSpawnRetryLimit = 60;

std::uint64_t mix_hash(std::uint64_t seed, std::uint64_t value) {
    seed ^= value + kGoldenRatio + (seed << 6) + (seed >> 2);
    return seed;
}

std::uint64_t hash_signed_int(int value) {
    return static_cast<std::uint64_t>(static_cast<std::int64_t>(value));
}

} // namespace

CustomAssetController::CustomAssetController(Asset* self)
    : self_(self) {
    if (self_ && !surface_child_.has_value()) {
        surface_child_.emplace(*self_, "#surface");
        surface_child_->bind("surface");
    }
    initialize_anchor_candidate_children();
}

CustomAssetController::~CustomAssetController() = default;

void CustomAssetController::update(const Input& in) {
    tick_anchor_candidate_attachments();
    on_update(in);
}

void CustomAssetController::process_pending_attacks(Asset& self) {
    on_process_pending_attacks(self);
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
    for (const auto& candidate_entry : self_->info->anchor_point_child_candidates) {
        if (candidate_entry.anchor_point_name.empty()) {
            continue;
        }
        if (!seen_anchor_names.insert(candidate_entry.anchor_point_name).second) {
            continue;
        }
        const nlohmann::json normalized_candidate_entry =
            self_->info->anchor_point_child_candidate_candidates(candidate_entry.anchor_point_name);
        const auto candidates_it = normalized_candidate_entry.find("candidates");
        if (candidates_it == normalized_candidate_entry.end() || !candidates_it->is_array()) {
            continue;
        }

        const vibble::spawn::RuntimeCandidates runtime_candidates =
            vibble::spawn::RuntimeCandidates::from_json(*candidates_it);
        if (runtime_candidates.empty()) {
            continue;
        }

        const auto resolved = runtime_candidates.pick_hashed(
            anchor_candidate_hash(candidate_entry.anchor_point_name),
            catalog,
            vibble::spawn::ZeroWeightPolicy::NoSelection);
        if (!resolved || resolved->is_null || resolved->resolved_asset_name.empty()) {
            continue;
        }

        AnchorCandidateAttachment attachment{};
        attachment.anchor_name = candidate_entry.anchor_point_name;
        attachment.resolved_asset_name = resolved->resolved_asset_name;
        attachment.remaining_spawn_retries = kAnchorCandidateSpawnRetryLimit;
        attachment.exhausted = false;
        attachment.child.emplace(*self_, attachment.resolved_asset_name);
        attachment.child->bind(attachment.anchor_name);
        anchor_candidate_children_.push_back(std::move(attachment));
    }
}

void CustomAssetController::tick_anchor_candidate_attachments() {
    for (auto& attachment : anchor_candidate_children_) {
        if (!attachment.child.has_value()) {
            continue;
        }

        if (attachment.child->get_asset()) {
            (void)attachment.child->update();
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

    if (self && !self->current_attack_box_volumes().empty()) {
        custom_controllers::AttackDetectionHelper::send_attacks_to_active_targets(self, assets());
    }
}

void CustomAssetController::on_process_pending_attacks(Asset& self) {
    custom_controllers::AttackDetectionHelper::process_pending_attacks_default(&self);
}
