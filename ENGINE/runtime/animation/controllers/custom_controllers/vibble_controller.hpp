#ifndef VIBBLE_CONTROLLER_HPP
#define VIBBLE_CONTROLLER_HPP

#include "assets/asset_controller.hpp"
#include <SDL3/SDL.h>
#include <string>
#include <chrono>
#include <thread>
#include <memory>

#include "anchor_bound_asset_helper.hpp"

class Asset;
class Input;

class vibble_controller : public AssetController {

public:
    vibble_controller(Asset* player);
    ~vibble_controller() override;
    void update(const Input& in) override;
    void process_pending_attacks(Asset& self) override;
    int get_dx() const;
    int get_dy() const;

private:
    void movement(const Input& input);
    float frame_dt() const;
    std::string animation_for_direction(int raw_x, int raw_y) const;
    void Dash();
    void spawn_eyes_follower();

    static constexpr float kWalkSpeed        = 300.0f;
    static constexpr float kSprintMultiplier = 2.0f;

    Asset* player_ = nullptr;
    AnchorBoundAssetHelper::Handle eyes_follower_{};
    std::unique_ptr<AnchorBoundAssetHelper> anchor_helper_;
    std::string eyes_spawn_last_failure_;
    bool eyes_spawn_success_logged_ = false;
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
