#pragma once
#include "asset/asset_controller.hpp"

#include <random>
#include <SDL.h>

class Assets;
class Asset;
class Input;

class GaryController : public AssetController {
public:
    GaryController(Assets* assets, Asset* self);
    ~GaryController() override = default;
    

    void update(const Input& in) override;

private:
    SDL_Point get_random_point_in_room();

    Assets* assets_ = nullptr;
    Asset*  self_   = nullptr;

    std::mt19937 rng_;
    std::uniform_int_distribution<int> idle_range_{};
};
