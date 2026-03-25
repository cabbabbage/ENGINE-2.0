#include "default_controller.hpp"
#include "assets/asset/Asset.hpp"
#include "assets/asset/animation.hpp"
#include "assets/asset/asset_info.hpp"
#include "animation/animation_update.hpp"

#include <string>

default_controller::default_controller(Asset* self)
    : self_(self) {}

void default_controller::update(const Input& ) {
    if (!self_ || !self_->info || !self_->anim_) {
        return;
    }

    const std::string default_anim{ animation_update::detail::kDefaultAnimation };

    auto it = self_->info->animations.find(default_anim);
    if (it == self_->info->animations.end() || !it->second.has_frames()) {
        return;
    }

    if (self_->current_animation != default_anim || self_->current_frame == nullptr) {
        self_->anim_->move(SDL_Point{ 0, 0 }, default_anim);
        return;
    }

}

void default_controller::process_pending_attacks(Asset& ) {
    if (!self_ || !self_->info || !self_->anim_) {
        return;
    }
    if (self_->current_animation == "damaged" && self_->info->animations.count("destroyed")) {
        self_->anim_->set_animation("destroy");
    }
    if (self_->info->animations.count("damaged")) {
        self_->anim_->set_animation("damaged");
    }
}
