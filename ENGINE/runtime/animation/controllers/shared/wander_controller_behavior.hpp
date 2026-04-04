#pragma once

#include <SDL3/SDL.h>
#include <random>

class Input;
class CustomAssetController;

namespace animation_update::custom_controllers {

struct WanderControllerBehaviorConfig {
    int idle_radius_px = 500;
    int random_range_min = -500;
    int random_range_max = 500;
    bool debug_enabled = false;
};

class WanderControllerBehavior {
public:
    WanderControllerBehavior(CustomAssetController* controller, WanderControllerBehaviorConfig config);
    void tick(const Input& in);

private:
    SDL_Point get_random_target();

    CustomAssetController* controller_;
    WanderControllerBehaviorConfig config_;
    std::mt19937 rng_;
    std::uniform_int_distribution<int> random_range_;
};

} // namespace animation_update::custom_controllers
