// CONTROLLER_META_BEGIN
// Controller: vibble_controller
// Asset: vibble (type: object)
// Available animations [1]:
//   - default
// Generated: 2026-03-17 23:48:57
// CONTROLLER_META_END

#ifndef VIBBLE_CONTROLLER_HPP
#define VIBBLE_CONTROLLER_HPP

#include "animation/controllers/custom_controllers/custom_asset_controller.hpp"
#include "animation/controllers/custom_controllers/child_asset.hpp"
#include <SDL3/SDL.h>
#include <chrono>
#include <optional>
#include <string>

class Asset;
class Input;

class vibble_controller : public CustomAssetController {

public:
    vibble_controller(Asset* player);
    ~vibble_controller() override;
    int get_dx() const;
    int get_dy() const;

protected:
    void on_update(const Input& in) override;
    void on_process_pending_attacks(Asset& self) override;

private:
    void ensure_eyes_child();
    void movement(const Input& input);
    float frame_dt() const;
    std::string animation_for_direction(int screen_x, int screen_y) const;
    void Dash();

    static constexpr float kWalkSpeed        = 300.0f;
    static constexpr float kSprintMultiplier = 2.0f;

    std::optional<ChildAsset> eyes_child_;
    int    dx_ = 0;
    int    dy_ = 0;

    bool canDash    = true;
    bool isDashing  = false;
    float dashingPower = 10.0f;
    float dashingTime = 0.05f;
    float dashingCooldown = 1.0f;
    std::chrono::steady_clock::time_point dashEndTime;
    std::chrono::steady_clock::time_point cooldownEndTime;

    bool canMelee = true;
    bool isMeleeing = false;
    float meleeCooldown = 0.5f;
    std::chrono::steady_clock::time_point meleeCooldownEndTime;

    float subpixel_x_ = 0.0f;
    float subpixel_y_ = 0.0f;
};

#endif
