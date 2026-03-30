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

void AnimationUpdate::set_animation(const std::string&) {}
