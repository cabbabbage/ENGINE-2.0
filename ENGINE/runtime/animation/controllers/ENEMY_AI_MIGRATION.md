# Enemy AI Migration (Controlled Break)

This migration defines the canonical enemy controller contract after the 2026 cleanup pass.

## Public Controller API

- Include: `animation/controllers/custom_controller.hpp`
- Base class: `custom_controller_api::CustomControllerBase`
- Enemy policy type: `custom_controller_api::EnemyAgentConfig`
- Movement type: `custom_controller_api::MovementConfig`

## Range Semantics (Old -> New)

- `chase_range_px` -> `ranges.desired_standoff_px`
  - Old behavior used this as stop distance, not detection range.
- `attack_range_px` -> `ranges.attack_radius_px`
- New field: `ranges.aggro_radius_px`
  - Explicit detection/engagement radius.

## Enemy Phase Model

`run_enemy_behavior(...)` now drives explicit phases:

1. `Acquire`
2. `Approach`
3. `AttackWindow`
4. `Recover`
5. `ReturnHome`

`kamikaze=true` uses `Approach -> AttackWindow` without recover retreat.

## Movement Contract

- Grounded enemies should keep `MovementConfig::allow_vertical_movement = false`.
- Vertical movement is opt-in and explicit.
- Ground-lock now resolves floor `y` for auto-move when vertical movement is disabled.

## Combat Contract

- `apply_attack_hit(...)` returns `true` only when a hit is dispatched.
- `apply_attack_hits_to_active_targets(...)` returns `true` only when at least one hit is dispatched.
- `try_attack_target(...)` starts cooldown only on actual hit.

## Affected Controllers (Migrated)

- `spider_controller`
- `small_spider_controller`
- `bomb_controller`

## Additional Content Alignment

- Manifest enemy classification normalized:
  - `bomb.asset_type = "enemy"`
  - `small_spider.asset_type = "enemy"`

