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


## Shared Enemy AI Pipeline

New archetype migrations should route through `shared/enemy_ai/enemy_ai_pipeline` before custom controller-specific attack code. The pipeline deliberately separates each frame into independently inspectable stages:

1. **Perception** - target validity, room eligibility, distance, self/home positions, and frame time.
2. **Intent selection** - phase choice plus attack-window enter/exit transitions.
3. **Positioning** - whether the enemy should seek standoff, face, retreat, or return home.
4. **Navigation** - route type and movement config/combat overrides for the chosen positioning request.
5. **Locomotion animation** - whether movement is allowed and whether movement may coexist with attacking.
6. **Attack commitment** - whether the controller is committed to a target and may fire a manual attack.
7. **Result feedback** - movement result, no-progress counters, attack-window counters, recovery, and fallback state.

`LegacyEnemyAiAdapter` keeps existing controller behavior intact by applying the separated frame through the current `ControllerAgentSystem` movement helpers. This allows one archetype to migrate at a time while older controllers continue to call `run_enemy_behavior(...)` unchanged.

Current first migrated archetype: `spider_controller`.
