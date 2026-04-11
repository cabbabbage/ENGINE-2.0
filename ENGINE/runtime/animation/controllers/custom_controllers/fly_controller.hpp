// CONTROLLER_META_BEGIN
// Controller: fly_controller
// Asset: fly (type: object)
// Available animations [1]:
//   - default
// Generated: 2026-04-10 02:16:39
// CONTROLLER_META_END



#pragma once
#include "animation/controllers/shared/custom_asset_controller.hpp"

class Asset;
class Input;

class fly_controller : public CustomAssetController {
public:
    explicit fly_controller(Asset* self);
    ~fly_controller() override = default;

protected:
    void on_update(const Input& in) override;
    void on_process_pending_attacks(Asset& self) override;
};
