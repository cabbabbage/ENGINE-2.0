#ifndef VIBBLE_CONTROLLER_HPP
#define VIBBLE_CONTROLLER_HPP

#include "animation/controllers/shared/custom_controller_api.hpp"
#include "animation/controllers/shared/child_asset.hpp"
#include <SDL3/SDL.h>
#include <chrono>
#include <optional>
#include <string>
#include <vector>

class Asset;
class Input;

class vibble_controller : public CustomAssetController {

public:
    vibble_controller(Asset* player);
    ~vibble_controller() override;

protected:
    void on_update(const Input& in) override;
    custom_controller_api::AttackProcessingConfig attack_processing_config() const override;
    void on_process_pending_attacks(Asset& self) override;

private:
    struct CardinalVector {
        int x = 0;
        int y = 0;
    };

    struct FacingSelection {
        std::string animation_id;
        CardinalVector vector{};
        bool valid = false;
    };

    void movement(const Input& input);
    std::string animation_for_direction(int screen_x, int screen_y) const;
    FacingSelection facing_from_mouse(const Asset& player,
                                      const Input& input,
                                      const std::string& fallback_animation) const;
    CardinalVector movement_cardinal_vector(int world_x, int world_y) const;
    static CardinalVector cardinal_vector_for_animation(const std::string& animation_id);
    void apply_idle_facing(const std::string& animation_id);
    void start_dash();

    static constexpr float kWalkSpeed        = 300.0f;
    static constexpr float kSprintMultiplier = 2.0f;

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
    std::string last_facing_animation_ = "default";
};

#endif
