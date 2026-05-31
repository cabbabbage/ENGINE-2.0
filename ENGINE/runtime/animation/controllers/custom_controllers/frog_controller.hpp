#ifndef FROG_CONTROLLER_HPP
#define FROG_CONTROLLER_HPP

#include "animation/controllers/custom_controller.hpp"

#include <SDL3/SDL.h>

#include <random>

class Asset;
class Input;

class frog_controller : public custom_controller_api::CustomControllerBase {

public:

    explicit frog_controller(Asset* self);

    ~frog_controller() override = default;

protected:
    void on_update(const Input& in) override;
    void on_process_pending_attacks(Asset& self) override;

private:
    int sample_idle_frames();
    void hop_away_from(const Asset& player);
    void random_wander_away_bias(const Asset& player);

    std::mt19937 rng_;
    int idle_frames_remaining_ = 0;
    bool flee_until_safe_ = false;
};

#endif
