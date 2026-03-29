#include "custom_asset_controller.hpp"

#include <SDL3/SDL.h>
#include <string>

#include "assets/asset/Asset.hpp"
#include "assets/asset/animation.hpp"
#include "assets/asset/asset_info.hpp"
#include "animation/animation_update.hpp"
#include "core/AssetsManager.hpp"

CustomAssetController::CustomAssetController(Asset* self)
    : self_(self) {
    if (self_ && !surface_child_.has_value()) {
        surface_child_.emplace(*self_, "#surface");
        surface_child_->bind("surface");
    }
}

CustomAssetController::~CustomAssetController() = default;

void CustomAssetController::update(const Input& in) {
    on_update(in);
}

void CustomAssetController::process_pending_attacks(Asset& self) {
    on_process_pending_attacks(self);
}

Assets* CustomAssetController::assets() const {
    return self_ ? self_->get_assets() : nullptr;
}

void CustomAssetController::on_update(const Input&) {
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
    }
}

void CustomAssetController::on_process_pending_attacks(Asset&) {
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
