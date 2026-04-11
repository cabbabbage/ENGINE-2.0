// CONTROLLER_META_BEGIN
// Controller: fly_controller
// Asset: fly (type: object)
// Available animations [1]:
//   - default
// Generated: 2026-04-10 02:16:39
// CONTROLLER_META_END



#include "fly_controller.hpp"

#include "assets/asset/Asset.hpp"
#include "assets/asset/animation.hpp"
#include "assets/asset/asset_info.hpp"
#include "animation/animation_update.hpp"
#include "utils/input.hpp"
#include <string>

fly_controller::fly_controller(Asset* self)
    : CustomAssetController(self) {}

void fly_controller::on_update(const Input& ) {
    Asset* self = self_ptr();
    if (!self || !self->info || !self->anim_) return;

    const std::string default_anim{ animation_update::detail::kDefaultAnimation };
    auto it = self->info->animations.find(default_anim);
    if (it != self->info->animations.end() && it->second.has_frames()) {
        if (self->current_animation != default_anim || self->current_frame == nullptr) {
            self->anim_->move(SDL_Point{0, 0}, default_anim);
        }
        return;
    }

    if (!self->info->animations.empty()) {
        const auto& first = *self->info->animations.begin();
        if (self->current_animation != first.first || self->current_frame == nullptr) {
            self->anim_->move(SDL_Point{0, 0}, first.first);
        }
    }
}

void fly_controller::on_process_pending_attacks(Asset& self_ref) {
    (void)self_ref;
    Asset* self = self_ptr();
    if (!self || !self->info || !self->anim_) return;
    // TODO: implement attack handling if this asset uses attack queues.
}
