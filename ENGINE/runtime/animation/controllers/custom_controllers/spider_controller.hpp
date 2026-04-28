#ifndef spider_CONTROLLER_HPP
#define spider_CONTROLLER_HPP

#include "animation/controllers/shared/custom_controller_api.hpp"

class Asset;
class Input;

class spider_controller : public CustomAssetController {

public:
    explicit spider_controller(Asset* self);
    ~spider_controller() override = default;

protected:
    void on_update(const Input&) override;
    void on_process_pending_attacks(Asset& self) override;
};

#endif
