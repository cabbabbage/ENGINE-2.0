#ifndef bomb_CONTROLLER_HPP
#define bomb_CONTROLLER_HPP

#include "animation/controllers/shared/custom_controller_api.hpp"
#include "animation/controllers/shared/enemy_combat_steering.hpp"
#include <string>

class Asset;
class Input;

class bomb_controller : public CustomAssetController {

public:
    explicit bomb_controller(Asset* self);
    ~bomb_controller() override = default;

protected:
    void on_update(const Input&) override;
    void on_death() override;
    void on_process_pending_attacks(Asset& self) override;

private:
    enum class State {
        Chasing,
        Detonating
    };

    void begin_detonation(Asset* player, const std::string& animation_id = "die");
    void dispatch_explosion_once(Asset* player);

    custom_controller_api::EnemyCombatSteering steering_;
    State state_ = State::Chasing;
    bool explosion_dispatched_ = false;
};

#endif
