#include "animation/animation_update.hpp"
#include "assets/asset/Asset.hpp"

AnimationUpdate::AnimationUpdate(Asset* self, Assets* assets)
    : self_(self), assets_owner_(assets) {
}

void AnimationUpdate::move(SDL_Point, const std::string&, bool, bool) {
    if (self_) {
        self_->needs_target = true;
    }
}

void AnimationUpdate::move_3d(const axis::WorldPos&, const std::string&, bool, bool) {
    if (self_) {
        self_->needs_target = true;
    }
}

void AnimationUpdate::set_animation(const std::string&) {}

void AnimationUpdate::auto_move(const std::vector<SDL_Point>&, int, std::optional<int>, bool) {}
void AnimationUpdate::auto_move(SDL_Point, int, std::optional<int>, bool) {}
void AnimationUpdate::auto_move(Asset*, int, bool) {}
void AnimationUpdate::auto_move_3d(axis::WorldPos, int, std::optional<int>, bool) {}
void AnimationUpdate::auto_move_3d_relative(axis::WorldPos, int, std::optional<int>, bool) {}
void AnimationUpdate::auto_move_3d(const std::vector<axis::WorldPos>&, bool, int, std::optional<int>, bool) {}

void AnimationUpdate::begin_reverse_current_animation_until_stop() {}
void AnimationUpdate::begin_reverse_current_animation_to_default() {}
void AnimationUpdate::stop_reverse_current_animation() {}
void AnimationUpdate::cancel_all_movement() {}
void AnimationUpdate::clear_movement_plan() {}
std::size_t AnimationUpdate::path_index_for(const std::string&) const { return 0; }
AnimationUpdate::MoveRequest AnimationUpdate::consume_move_request() { return {}; }
AnimationUpdate::MoveRequest3D AnimationUpdate::consume_move_request_3d() { return {}; }
bool AnimationUpdate::consume_input_event() { return false; }
void AnimationUpdate::set_debug_enabled(bool) {}
bool AnimationUpdate::debug_enabled() const { return false; }
vibble::grid::Grid& AnimationUpdate::grid() const { return vibble::grid::global_grid(); }
int AnimationUpdate::effective_grid_resolution(std::optional<int>) const { return 0; }
