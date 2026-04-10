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
        if (runtime_candidates.empty()) {
            vibble::log::warn(std::string("[CustomAssetController] Empty explicit anchor candidates for anchor '") +
                              anchor_name + "' on asset '" + self_->info->name + "'");
            continue;
        }

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
    custom_controllers::AttackProcessingHelper::process_pending_attacks(self);
}
