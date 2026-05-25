#ifndef spider_CONTROLLER_HPP
#define spider_CONTROLLER_HPP

#include "animation/controllers/shared/custom_controller_api.hpp"
#include "animation/controllers/shared/enemy_combat_steering.hpp"
#include <chrono>
#include <optional>
#include <string>

class Asset;
class Input;

class spider_controller : public CustomAssetController {

public:
    explicit spider_controller(Asset* self);
    ~spider_controller() override = default;

protected:
    void on_update(const Input&) override;
    void on_process_pending_attacks(Asset& self) override;

private:
    enum class State {
        Approach,
        Attack,
        Evade
    };

    void tick_approach(Asset& self, Asset& player);
    void tick_attack(Asset& self, Asset& player);
    void tick_evade(Asset& self, Asset& player);
    std::optional<std::string> resolve_attack_animation_id(Asset& self, const Asset& player) const;
    bool attack_window_is_hittable(const Asset& self,
                                   const Asset& player,
                                   const std::string& attack_animation_id) const;
    bool trigger_attack(Asset& self, const Asset& player, const std::string& attack_animation_id);
    bool dispatch_attack_frame_once(Asset& self, Asset& player);
    void enter_evade();

    custom_controller_api::EnemyCombatSteering steering_;
    State state_ = State::Approach;
    std::chrono::steady_clock::time_point next_attack_time_{};
    std::chrono::steady_clock::time_point evade_until_{};
    int last_dispatched_attack_frame_ = -1;
    std::string last_dispatched_payload_id_{};
    bool attack_dispatched_this_cycle_ = false;
    bool await_fresh_window_after_evade_ = false;
    bool prior_window_hittable_ = false;
};

#endif
