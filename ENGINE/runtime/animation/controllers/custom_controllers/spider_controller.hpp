#ifndef spider_CONTROLLER_HPP
#define spider_CONTROLLER_HPP

#include "animation/controllers/shared/custom_controller_api.hpp"
#include <chrono>
#include <unordered_map>

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
    bool dispatch_weighted_attack_if_ready(Asset& self, Asset& player);
    std::chrono::steady_clock::time_point next_maul_time_{};
    std::chrono::steady_clock::time_point next_retarget_time_{};
    std::unordered_map<std::string, std::chrono::steady_clock::time_point> payload_recharge_ready_at_{};
};

#endif
