#include "animation_update.hpp"

#include <algorithm>
#include <optional>
#include <random>
#include <string>
#include <unordered_set>

#include "animation_runtime.hpp"
#include "assets/asset/Asset.hpp"
#include "assets/asset/asset_info.hpp"
#include "utils/utils/string_utils.hpp"

namespace {
std::vector<std::string> normalize_tag_list(const std::vector<std::string>& input) {
    std::vector<std::string> normalized;
    std::unordered_set<std::string> seen;
    for (const std::string& raw : input) {
        std::string canonical = vibble::strings::to_lower_copy(vibble::strings::trim_copy(raw));
        if (!canonical.empty() && seen.insert(canonical).second) normalized.push_back(std::move(canonical));
    }
    return normalized;
}
std::mt19937& tag_selection_rng() { static std::mt19937 rng{std::random_device{}()}; return rng; }
}

void AnimationUpdate::begin_reverse_current_animation_until_stop() { pending_reverse_command_ = ReversePlaybackCommand::ReverseUntilStopCurrentAnimation; input_event_ = true; if (!move_pending_ && !move_pending_3d_ && runtime_) { runtime_->begin_reverse_current_animation_until_stop(); pending_reverse_command_ = ReversePlaybackCommand::None; } }
void AnimationUpdate::begin_reverse_current_animation_to_default() { pending_reverse_command_ = ReversePlaybackCommand::ReverseToDefaultAtStart; input_event_ = true; if (!move_pending_ && !move_pending_3d_ && runtime_) { runtime_->begin_reverse_current_animation_to_default(); pending_reverse_command_ = ReversePlaybackCommand::None; } }
void AnimationUpdate::stop_reverse_current_animation() { pending_reverse_command_ = ReversePlaybackCommand::Stop; input_event_ = true; if (!move_pending_ && !move_pending_3d_ && runtime_) { runtime_->stop_reverse_current_animation(); pending_reverse_command_ = ReversePlaybackCommand::None; } }
std::size_t AnimationUpdate::path_index_for(const std::string& anim_id) const { return runtime_ ? runtime_->path_index_for(anim_id) : 0; }
AnimationUpdate::MoveRequest AnimationUpdate::consume_move_request() { move_pending_ = false; pending_move_.reverse_command = pending_reverse_command_; pending_reverse_command_ = ReversePlaybackCommand::None; return pending_move_; }
AnimationUpdate::MoveRequest3D AnimationUpdate::consume_move_request_3d() { move_pending_3d_ = false; return pending_move_3d_; }
bool AnimationUpdate::consume_input_event() { const bool had = input_event_; input_event_ = false; return had; }
void AnimationUpdate::set_animation(const std::string& animation_id) {
    if (!self_ || !self_->info || !runtime_) {
        return;
    }
    auto it = self_->info->animations.find(animation_id);
    if (it == self_->info->animations.end()) {
        return;
    }
    runtime_->switch_to(animation_id, path_index_for(animation_id));
}

static std::vector<std::string> matching_animation_ids_by_tags(const Asset* self,
                                                               const std::vector<std::string>& required_tags,
                                                               const std::vector<std::string>& excluded_tags) {
    std::vector<std::string> candidates;
    if (!self || !self->info) {
        return candidates;
    }

    const auto required = normalize_tag_list(required_tags);
    const auto excluded = normalize_tag_list(excluded_tags);
    for (const auto& [animation_id, animation] : self->info->animations) {
        if (!animation.has_frames()) {
            continue;
        }
        const auto normalized_tags = normalize_tag_list(animation.tags);
        bool excluded_match = false;
        for (const auto& tag : excluded) {
            if (std::find(normalized_tags.begin(), normalized_tags.end(), tag) != normalized_tags.end()) {
                excluded_match = true;
                break;
            }
        }
        if (excluded_match) {
            continue;
        }
        bool required_match = true;
        for (const auto& tag : required) {
            if (std::find(normalized_tags.begin(), normalized_tags.end(), tag) == normalized_tags.end()) {
                required_match = false;
                break;
            }
        }
        if (required_match) {
            candidates.push_back(animation_id);
        }
    }
    return candidates;
}

std::optional<std::string> AnimationUpdate::resolve_animation_by_tags(
    const std::vector<std::string>& required_tags,
    const std::vector<std::string>& excluded_tags) const {
    auto candidates = matching_animation_ids_by_tags(self_, required_tags, excluded_tags);
    if (candidates.empty()) {
        return std::nullopt;
    }
    std::uniform_int_distribution<std::size_t> pick(0, candidates.size() - 1);
    return candidates[pick(tag_selection_rng())];
}

std::optional<std::string> AnimationUpdate::resolve_animation_by_tags_deterministic(
    const std::vector<std::string>& required_tags,
    const std::vector<std::string>& excluded_tags) const {
    auto candidates = matching_animation_ids_by_tags(self_, required_tags, excluded_tags);
    if (candidates.empty()) {
        return std::nullopt;
    }
    std::sort(candidates.begin(), candidates.end());
    return candidates.front();
}

bool AnimationUpdate::set_animation_by_tags(const std::vector<std::string>& required_tags,
                                            const std::vector<std::string>& excluded_tags) {
    if (!runtime_) {
        return false;
    }
    const auto resolved = resolve_animation_by_tags(required_tags, excluded_tags);
    if (!resolved.has_value()) {
        return false;
    }
    set_animation(*resolved);
    return true;
}

bool AnimationUpdate::set_animation_by_tags_deterministic(const std::vector<std::string>& required_tags,
                                                          const std::vector<std::string>& excluded_tags) {
    if (!runtime_) {
        return false;
    }
    const auto resolved = resolve_animation_by_tags_deterministic(required_tags, excluded_tags);
    if (!resolved.has_value()) {
        return false;
    }
    set_animation(*resolved);
    return true;
}
