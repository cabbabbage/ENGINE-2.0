#include "default_controller.hpp"
#include "assets/asset/Asset.hpp"
#include "assets/asset/animation.hpp"
#include "assets/asset/asset_info.hpp"
#include "animation/animation_update.hpp"

#include <string>


default_controller::default_controller(Asset* self)
    : CustomAssetController(self) {}

void default_controller::on_update(const Input& ) {
    Asset* self = self_ptr();
    if (!self || !self->info || !self->anim_) {
        return;
    }

    const std::string default_anim{ animation_update::detail::kDefaultAnimation };

    auto it = self->info->animations.find(default_anim);
    if (it == self->info->animations.end() || !it->second.has_frames()) {
        return;
    }

    if (self->current_animation != default_anim || self->current_frame == nullptr) {
        self->anim_->move(SDL_Point{ 0, 0 }, default_anim);
        return;
    }

}

void default_controller::on_process_pending_attacks(Asset& ) {
    Asset* self = self_ptr();
    if (!self || !self->info || !self->anim_) {
        return;
    }
    if (self->current_animation == "damaged" && self->info->animations.count("destroyed")) {
        self->anim_->set_animation("destroy");
    }
    if (self->info->animations.count("damaged")) {
        self->anim_->set_animation("damaged");
    }
}
