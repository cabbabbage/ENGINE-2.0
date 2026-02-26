#pragma once
#include "assets/asset_controller.hpp"

#include <random>
#include <SDL3/SDL.h>

class Assets;
class Asset;
class Input;

class bartender_controller : public AssetController {
public:
    bartender_controller(Assets* assets, Asset* self);
    ~bartender_controller() override = default;


    void update(const Input& in) override;
    void process_pending_attacks(Asset& self) override;

private:
    SDL_Point get_random_point_in_room();

    Assets* assets_ = nullptr;
    Asset*  self_   = nullptr;

    std::mt19937 rng_;
    std::uniform_int_distribution<int> idle_range_{};
};
