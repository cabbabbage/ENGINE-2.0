#pragma once
#include "animation/controllers/custom_controllers/custom_asset_controller.hpp"

#include <random>
#include <SDL3/SDL.h>

class Asset;
class Input;

class bartender_controller : public CustomAssetController {
public:
    explicit bartender_controller(Asset* self);
    ~bartender_controller() override = default;


protected:
    void on_update(const Input& in) override;
    void on_process_pending_attacks(Asset& self) override;

private:
    SDL_Point get_random_point_in_room();

    std::mt19937 rng_;
    std::uniform_int_distribution<int> idle_range_{};
};
