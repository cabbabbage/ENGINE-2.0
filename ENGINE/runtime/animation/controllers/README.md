# Custom Controller Authoring (`CustomControllerBase`)

This directory now exposes one public authoring surface for runtime controllers:

- `animation/controllers/custom_controller.hpp`
- `animation_update::custom_controllers::CustomControllerBase`

Custom controller authors should inherit from `CustomControllerBase` and use its helper APIs. Legacy split helpers (`wander`, `enemy steering`, `auto combat`, path utilities, etc.) were removed.

## Public vs Internal

Safe/public for controller authors:

- Lifecycle hooks (`on_init`, `on_update`, `on_attack`, `on_hit`, `on_death`, `on_no_pending_attacks`, `on_after_attack`, `on_process_pending_attacks`, `on_pre_delete_hook`, `on_orphaned_hook`, `on_interact_hook`)
- Context/target helpers (`controller_self`, `controller_assets`, `controller_game_context`, `controller_runtime_game_context`, `resolve_target_player`, `is_target_valid`)
- Movement helpers (`move_3d`, `move_toward`, `move_away`, `seek_target`, `chase_target`, `retreat_from_target`, `patrol`, `idle_wander`, `return_home`, `face_target`, `face_direction`)
- Combat helpers (`start_attack`, `is_target_in_range`, `cooldown_ready`, `start_cooldown`, `try_attack_target`, `apply_attack_hit`, `apply_attack_hits_to_active_targets`, `is_hit_window_open`)
- Animation helpers (`play_animation`, `play_animation_by_tags`, `play_default_idle`, reverse-animation helpers)
- Child/anchor helpers (`spawn_bind_child`, `child_helper`, `child_asset`, `orphan_child`, `destroy_child`, `clear_all_children`, `notify_anchor_changed`)

Internal/private (do not couple custom controllers to these directly):

- Backend runtime bookkeeping (`shared/internal/controller_runtime_backend.*`)
- Internal movement/combat/behavior systems (`shared/internal/controller_*_system.*`)
- Attack resolution internals, hitbox scans, orphan-fall simulation, anchor candidate spawn bookkeeping

## Lifecycle Hooks

Override only what you need:

- `on_init`: controller initialization.
- `on_update`: per-frame behavior logic.
- `on_attack`: raw attack event received.
- `on_hit`: called when attack processing applies damage.
- `on_death`: called when processed damage kills this asset.
- `on_no_pending_attacks`: called when no pending attacks are queued.
- `on_after_attack`: optional post-attack behavior.
- `attack_processing_config`: per-controller hit/death/knockback config.
- `on_process_pending_attacks`: post-processing hook after queued attacks are handled.
- `on_pre_delete_hook`: cleanup before delete.
- `on_orphaned_hook`: orphan callback with optional impulse.
- `on_interact_hook`: interaction callback.

## 3D Movement Model

Movement helpers operate in world-space `(x, y, z)`:

- `x`: horizontal world axis
- `y`: height/elevation axis
- `z`: depth axis

Use:

- `move_3d(delta, animation)` for direct per-frame movement deltas.
- `move_toward(target, step_px, config)` / `move_away(...)` for 3D steering.
- `seek_target`/`chase_target`/`retreat_from_target` for target-relative movement.
- `patrol(points, config)` for checkpoint loops.
- `idle_wander(min_delta, max_delta, config)` for random idle motion.
- `return_home(threshold_px, config)` to rejoin spawn/home location.

`MovementConfig` controls visit threshold, optional resolution layer override, lock behavior, and combat auto-move overrides.

## Combat, Targeting, Cooldowns, Hit Windows

Typical attack flow:

1. Resolve target: `Asset* target = resolve_target_player();`
2. Keep spacing with movement helpers.
3. Trigger attack intent with `try_attack_target(...)` or explicit `start_attack(...)` + `apply_attack_hit(...)`.
4. Use cooldown helpers (`cooldown_ready`, `start_cooldown`) for pacing.
5. Optional animation-frame windows with `is_hit_window_open(start, end)`.

For broad active hitbox dispatch, call `apply_attack_hits_to_active_targets()`.

Damage/hit/death resolution for incoming queued attacks is handled centrally in `process_pending_attacks`, using your `attack_processing_config()`.

## Auto Behavior Helpers

Use high-level behavior wrappers for common AI loops:

- `run_enemy_behavior(target, config, chase_move, retreat_move)`
  - Supports chase/attack/recover/return and optional kamikaze mode.
- `run_wander_behavior(target, idle_radius_px, min_wander_delta_px, max_wander_delta_px, config)`
  - Handles simple idle/wander behavior away from or around targets.

These helpers keep behavior state internal to the base class and avoid duplicating FSM logic per controller.

## Child / Anchor / Oval / Bound Asset Control

For controller-owned children:

- Spawn + bind: `spawn_bind_child(key, asset_name, anchor_name, hidden)`
- Access helper: `child_helper(key)` / `child_asset(key)`
- Orphan with impulse: `orphan_child(key, impulse)`
- Cleanup: `destroy_child(key)` / `clear_all_children()`

When facing/orientation changes should propagate to anchor-bound visuals, call:

- `notify_anchor_changed(anchor_name)`

`face_target`/`face_direction` also update directional heading/pitch and issue anchor notifications.

## Minimal Example

```cpp
#include "animation/controllers/custom_controller.hpp"

class simple_enemy_controller : public custom_controller_api::CustomControllerBase {
public:
    explicit simple_enemy_controller(Asset* self)
        : custom_controller_api::CustomControllerBase(self) {}

protected:
    void on_update(const Input&) override {
        Asset* self = controller_self();
        Asset* target = resolve_target_player();
        if (!self || !target) {
            return;
        }

        custom_controller_api::MovementConfig move_cfg{};
        move_cfg.visit_threshold_px = 10;
        move_cfg.override_non_locked = false;

        chase_target(*target, move_cfg);
        try_attack_target(*target, "bite", 0.35f, 64, "attack");
    }
};
```

## Advanced Enemy Example

```cpp
#include "animation/controllers/custom_controller.hpp"

class advanced_skirmisher_controller : public custom_controller_api::CustomControllerBase {
public:
    explicit advanced_skirmisher_controller(Asset* self)
        : custom_controller_api::CustomControllerBase(self) {
        behavior_cfg_.kamikaze = false;
        behavior_cfg_.chase_range_px = 220;
        behavior_cfg_.attack_range_px = 72;
        behavior_cfg_.retreat_distance_px = 260;
        behavior_cfg_.recover_ms = 450;
        behavior_cfg_.return_home_threshold_px = 96;

        chase_cfg_.visit_threshold_px = 10;
        retreat_cfg_.visit_threshold_px = 12;
        chase_cfg_.override_non_locked = false;
        retreat_cfg_.override_non_locked = false;
    }

protected:
    void on_update(const Input&) override {
        Asset* target = resolve_target_player();
        run_enemy_behavior(target, behavior_cfg_, chase_cfg_, retreat_cfg_);

        if (target && is_target_in_range(*target, behavior_cfg_.attack_range_px)) {
            // gated hit window for animation frames 2..4
            if (start_attack("attack") && is_hit_window_open(2, 4)) {
                apply_attack_hit(*target);
            }
        }
    }

private:
    custom_controller_api::EnemyBehaviorConfig behavior_cfg_{};
    custom_controller_api::MovementConfig chase_cfg_{};
    custom_controller_api::MovementConfig retreat_cfg_{};
};
```

## Notes

- Keep custom code on `CustomControllerBase` helpers; avoid including internal backend headers.
- If you need new behavior primitives, add them to `CustomControllerBase` so all controllers share one toolkit.
