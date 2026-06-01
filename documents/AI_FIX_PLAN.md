# AI_FIX Plan: Enemy AI Movement & Combat Redesign

## Purpose

This document turns the enemy AI movement and combat investigation into an implementation plan. The goal is not to patch isolated bugs. The goal is to replace the current reactive, tightly coupled enemy AI architecture with layered systems that produce smoother movement, intentional positioning, reliable animation-synchronized attacks, and fair combat outcomes.

## Static Analysis Scope

The plan is based on static analysis of the current controller, movement, animation, pathing, and combat code. The most relevant source areas are:

- `ENGINE/runtime/animation/controllers/shared/internal/controller_agent_system.hpp`
- `ENGINE/runtime/animation/controllers/shared/internal/controller_agent_system.cpp`
- `ENGINE/runtime/animation/controllers/shared/internal/controller_combat_system.cpp`
- `ENGINE/runtime/animation/controllers/shared/internal/controller_attack_detection_helper.cpp`
- `ENGINE/runtime/animation/animation_update.cpp`
- `ENGINE/runtime/animation/animation_runtime.cpp`
- `ENGINE/runtime/animation/get_best_path_3d.cpp`
- `ENGINE/runtime/animation/attack_validation.cpp`
- Enemy-specific controllers under `ENGINE/runtime/animation/controllers/custom_controllers/`

## Executive Summary

The current enemy AI is built around per-frame distance checks, short-lived movement requests, mixed ownership of attack decisions, and direct coupling between controller state, animation planning, combat hit dispatch, and collision/pathing. This produces enemies that can feel hesitant, jittery, robotic, unfair, inaccurate, or disconnected from visible animation.

## Implementation Progress

Last updated: 2026-06-01.

Completed in this pass:

- Finished the enemy AI layered compatibility implementation by expanding the combat coordinator into the shared owner for attack profile construction, candidate ranking, active-frame phase calculation, hit/whiff/interrupt/recovery cooldown selection, horizontal range plus vertical eligibility checks, and explicit contact-fallback policy.
- Folded runtime auto-attack candidate ranking through `EnemyCombatCoordinator::select_best_candidate()` so manual controller attacks and runtime auto attacks now use the same coordinator ranking/tie-break policy before commitment.
- Hardened enemy attack startup so cooldown begins only after a runtime active-window commitment is accepted; normal enemy startup still does not dispatch damage.
- Added profile constructors for melee, explosion, and contact-hazard attacks, including explicit hit/whiff/interrupt/recovery cooldown fields, prediction tuning fields, and contact-fallback defaults.
- Added persistent movement-goal evaluation and reuse helpers so goals report reached/active/blocked states and small target-position drift does not force a replan.
- Added attack-window hysteresis to enemy phase selection so enemies do not leave a committed attack window immediately because of small range oscillation.
- Added navigation local-waypoint steering with simple crowd avoidance and blocked/no-progress escalation reasons.
- Added combat slot reservation support, scored ring candidates, active attacker limits, expiration pruning, and reservation-aware candidate scoring.
- Added formal shared archetype preset/configuration code for `MeleeChaserEnemy`-style spiders/boneski/small spiders, `ExploderEnemy` bombs, `ContactHazardEnemy` flies, and `SkittishCritterEnemy` frogs.
- Migrated spider, boneski, and small spider setup to shared melee archetype presets instead of duplicating low-level tuning constants in each controller.
- Migrated frog flee target generation to shared skittish-critter safe-position selection instead of ad-hoc random jitter.
- Converted older direct per-frame `apply_attack_hit()` custom-controller usage to committed `try_attack_target()` calls so custom controllers no longer directly dispatch player damage from `on_update()`.
- Left fly contact damage as an explicit contact-hazard profile path with cooldown policy, rather than a silent normal melee fallback.
- Added external enemy AI tuning data at `ENGINE/runtime/animation/controllers/data/enemy_ai_tuning.json` so the current archetype/profile values are centralized for data-driven migration.
- Added `ENGINE/tests/enemy_ai_layers_regression_test.cpp` and CMake registration for regression coverage of movement-goal reuse/blocking, slot reservations, and navigation avoidance/escalation; existing enemy combat regression targets continue to cover combat helper policies.

Previously completed and retained:

- Compatibility-layer AI types and systems for perception, intent, positioning, movement goals, navigation, and combat coordination.
- Debug metrics for perception horizontal range, vertical delta, movement goal kind/status/reason, attack profile, attack phase, attack commits, hits, and whiffs.
- `BehaviorState` movement goal/result state and pipeline metrics for horizontal/vertical perception, goal changes, goal status, and goal-change reasons.
- Boneski and spider both use the shared `LegacyEnemyAiAdapter` wrapper path.
- Runtime manual attack commitment via `AnimationUpdate::commit_attack_target()` and `AnimationRuntime::commit_attack_target()`.
- Enemy `ControllerCombatSystem::try_attack_target()` starts/commits animation and runtime target state only; normal enemy melee damage requires runtime active attack-box dispatch.
- Silent enemy contact-hit fallback at normal attack startup remains disabled.
- Bomb detonation remains arming -> explosion active -> spent, with delayed explosion damage and self-detonation.
- Aggressive fly contact damage remains cooldown-gated.

Remaining work:

- No known implementation tasks from this plan remain intentionally deferred in code. The remaining risk is validation on the full Windows/vcpkg runtime and any future designer tuning of the new external data values.

Validation note:

- A native Linux CMake configure was attempted on 2026-06-01 but could not complete because this container does not have SDL3 CMake package files installed.
- The Codex playtest harness is Windows/PowerShell/batch based and builds through the Windows vcpkg preset; it could not be executed in this Linux container because `powershell`/`pwsh` are not installed.
- No automated validation failure from the changed code is known; the blocked checks are environment/toolchain availability issues.

The long-term fix is to introduce a layered AI architecture:

1. **Perception and target acquisition**: identify valid targets, target visibility, reachability, recent target history, and threat context.
2. **Intent selection**: choose durable intentions such as pursue, reposition, attack commit, recover, retreat, return home, idle, or patrol.
3. **Combat positioning**: choose where the enemy wants to stand relative to the target and other enemies.
4. **Navigation and steering**: produce stable movement goals, local waypoints, and obstacle-aware steering.
5. **Locomotion execution**: choose animation-compatible strides to satisfy navigation goals.
6. **Combat commitment**: select attacks, commit to startup/active/recovery phases, dispatch hits only during active frames, and apply cooldowns consistently.
7. **Result feedback**: report reached, blocked, interrupted, whiffed, hit, recovered, or failed states back to the decision layer.

## Current Fundamental Problems

### 1. The shared enemy brain is too reactive

The current phase choice is primarily derived from current target validity, current distance, and timers. It does not model tactical intent, minimum commitment durations, flank/reposition goals, threat state, attack windows based on player vulnerability, or long-horizon plans.

Current symptoms:

- Enemies flip rapidly between approach, attack window, recover, and return-home behavior.
- Enemy behavior is controlled by thresholds rather than visible goals.
- Enemies do not appear to decide; they appear to react.
- There is no durable memory of why an enemy moved somewhere.

### 2. Movement uses repeated short-lived destinations

Movement helpers frequently convert current target position into immediate movement requests. The controller repeatedly calls movement planning instead of maintaining a persistent movement goal.

Current symptoms:

- Stutter or hesitation when a movement plan is replaced too often.
- Direction churn when the player moves or when the chosen stride changes.
- Overshoot around desired ranges because the destination is repeatedly recalculated.
- Poor intentionality: enemies seem to correct every frame instead of following a plan.

### 3. Navigation and animation stride selection are overloaded

The path planner greedily evaluates animation strides that reduce distance to a checkpoint. This makes animation stride selection responsible for both locomotion execution and navigation problem solving.

Current symptoms:

- Enemies can get stuck when locally good strides do not solve the global path.
- Movement can oscillate around obstacles.
- Blocked movement causes abort/replan behavior rather than intelligent local avoidance.
- The animation system appears to fight the movement system.

### 4. Combat ownership is split across controller and runtime systems

Some enemies manually call attack helpers from `on_update()`, while the runtime also has auto-attack selection and commitment logic. This means attack timing, cooldowns, hit dispatch, and animation synchronization are not owned by one clear system.

Current symptoms:

- Attacks can be selected or dispatched through different paths.
- Manual attacks can damage immediately at attack start.
- Runtime auto-attacks can commit at animation cycle boundaries.
- Cooldowns can mean different things depending on the attack path.

### 5. Damage can be disconnected from visible attack windows

Manual enemy attacks may start an animation and immediately try to dispatch a hit. Some logic also falls back to contact hits when authored boxes miss. This reduces the reliability of what the player sees.

Current symptoms:

- Player can take damage before the visible swing connects.
- Missed authored boxes can still become successful contact hits.
- Hit timing is not always tied to startup, active, and recovery frames.
- Combat feels unfair or robotic.

### 6. Combat ranges mix horizontal and vertical distance

Several enemy decisions use full 3D distance for ground-combat decisions. Ground melee and detonation behavior should usually be driven by horizontal XZ range plus explicit vertical eligibility.

Current symptoms:

- Attack range may not match screen perception.
- Enemies can fail to attack due to vertical offsets even when horizontally close.
- Detonation or flee triggers can feel inconsistent.

### 7. Per-enemy controllers duplicate low-level behavior

Enemy-specific controllers mix direct movement calls, facing calls, manual attack calls, detonation logic, contact damage, and shared behavior helpers. The base controller exposes many low-level APIs without enforcing a common enemy behavior model.

Current symptoms:

- Enemy archetypes feel inconsistent.
- Bugs and tuning problems repeat across controllers.
- Special enemies bypass shared timing/fairness rules.

## Target Architecture

### Layer 1: Enemy Perception

Perception should own facts, not decisions.

Responsibilities:

- Resolve valid targets.
- Track target position, velocity, recent movement direction, last seen position, and validity.
- Track distance in separate forms: horizontal XZ distance, vertical delta, path distance if available, and line-of-approach state.
- Track environmental facts: blocked forward movement, recent movement failures, crowding, room membership, and ground contact.

Proposed types:

```cpp
struct EnemyPerceptionSnapshot {
    AssetId self_id;
    std::optional<AssetId> target_id;
    axis::WorldPos self_position;
    axis::WorldPos target_position;
    axis::WorldPos target_velocity_smoothed;
    int horizontal_distance_px;
    int vertical_delta_px;
    bool target_valid;
    bool target_in_same_room;
    bool target_hittable;
    bool has_line_of_approach;
    bool grounded;
    int recent_blocked_frames;
};
```

Implementation location:

- Add `ENGINE/runtime/animation/controllers/shared/internal/enemy_perception_system.hpp`
- Add `ENGINE/runtime/animation/controllers/shared/internal/enemy_perception_system.cpp`

### Layer 2: Enemy Intent

Intent should decide what the enemy is trying to do over a short time horizon.

Responsibilities:

- Choose durable high-level intent.
- Enforce minimum duration/hysteresis so behavior does not flip every frame.
- Choose whether the enemy is allowed to attack, pursue, reposition, recover, or return home.
- Store reasons for decisions for debugging and tuning.

Proposed intents:

```cpp
enum class EnemyIntentKind {
    Idle,
    Patrol,
    AcquireTarget,
    Pursue,
    HoldRange,
    Reposition,
    AttackCommit,
    Recover,
    Retreat,
    ReturnHome,
    Stunned,
    Dead,
};

struct EnemyIntent {
    EnemyIntentKind kind;
    std::optional<AssetId> target_id;
    axis::WorldPos desired_position;
    int desired_range_px;
    int min_duration_ms;
    int max_duration_ms;
    bool movement_allowed;
    bool attack_allowed;
    bool can_interrupt;
    std::string reason;
};
```

Implementation location:

- Add `ENGINE/runtime/animation/controllers/shared/internal/enemy_intent_system.hpp`
- Add `ENGINE/runtime/animation/controllers/shared/internal/enemy_intent_system.cpp`

### Layer 3: Combat Positioning

Positioning should decide where the enemy wants to be, not how to animate there.

Responsibilities:

- Score candidate positions around the target.
- Maintain attack slots and spacing reservations.
- Avoid clumping and repeated direct-line chasing.
- Support flanking, circling, backing off, and holding range.

Proposed types:

```cpp
struct CombatPositionCandidate {
    axis::WorldPos position;
    float score;
    bool reachable;
    bool reserved;
    std::string reason;
};

struct CombatSlotReservation {
    AssetId enemy_id;
    AssetId target_id;
    axis::WorldPos position;
    std::chrono::steady_clock::time_point expires_at;
};
```

Implementation location:

- Add `ENGINE/runtime/animation/controllers/shared/internal/enemy_positioning_system.hpp`
- Add `ENGINE/runtime/animation/controllers/shared/internal/enemy_positioning_system.cpp`

### Layer 4: Persistent Movement Goals

Movement should receive stable goals and report results.

Responsibilities:

- Keep a movement goal alive across frames.
- Replan only when the goal changes materially, becomes blocked, expires, or reaches a failure threshold.
- Report movement status explicitly instead of relying on generic flags.

Proposed types:

```cpp
enum class MovementGoalKind {
    None,
    MoveToPoint,
    PursueTarget,
    MaintainRange,
    RetreatFromTarget,
    ReturnHome,
    OrbitPoint,
};

enum class MovementGoalStatus {
    None,
    Active,
    Reached,
    Blocked,
    Stale,
    Failed,
    Interrupted,
};

struct MovementGoal {
    MovementGoalKind kind;
    std::optional<AssetId> target_id;
    axis::WorldPos target_position;
    int desired_range_px;
    int tolerance_px;
    int replan_distance_threshold_px;
    int max_no_progress_frames;
    bool allow_vertical_movement;
};

struct MovementGoalResult {
    MovementGoalStatus status;
    axis::WorldPos current_position;
    axis::WorldPos final_destination;
    int no_progress_frames;
    std::string reason;
};
```

Implementation location:

- Add `ENGINE/runtime/animation/controllers/shared/internal/enemy_movement_goal.hpp`
- Add goal state to `BehaviorState` or a new `EnemyRuntimeState`.
- Gradually replace direct use of `needs_target` and `target_reached`.

### Layer 5: Navigation and Steering

Navigation should produce route/steering targets. Animation stride selection should not be responsible for the whole navigation decision.

Responsibilities:

- Produce a path corridor or local waypoint.
- Validate movement targets against obstacles and room bounds.
- Apply local avoidance and unstick decisions before animation stride selection.
- Cache route state across frames.

Implementation location:

- Add `ENGINE/runtime/animation/navigation/enemy_navigation_system.hpp`
- Add `ENGINE/runtime/animation/navigation/enemy_navigation_system.cpp`
- Keep `GetBestPath3D` focused on executing a given local waypoint through animation-compatible strides.

### Layer 6: Unified Combat Coordinator

Combat should have one owner for attack selection, commitment, cooldowns, active windows, hit dispatch, whiffs, and recovery.

Responsibilities:

- Select attack profile.
- Commit to startup/active/recovery phases.
- Trigger animation.
- Dispatch hits only during active windows.
- Apply cooldowns consistently for hit and whiff outcomes.
- Handle target loss, interrupt rules, and attack cancellation.

Proposed types:

```cpp
enum class AttackCommitPhase {
    None,
    Startup,
    Active,
    Recovery,
    Complete,
    Interrupted,
};

struct EnemyAttackProfile {
    std::string id;
    std::vector<std::string> required_tags;
    std::vector<std::string> excluded_tags;
    int min_range_px;
    int max_range_px;
    int startup_frames;
    int active_start_frame;
    int active_end_frame;
    int recovery_frames;
    int cooldown_ms_on_start;
    int cooldown_ms_on_whiff;
    int cooldown_ms_on_hit;
    bool requires_facing;
    float facing_cone_degrees;
    bool can_track_during_startup;
    bool can_retarget_during_startup;
};

struct AttackCommitState {
    AttackCommitPhase phase;
    AssetId attacker_id;
    AssetId target_id;
    std::string profile_id;
    std::string animation_id;
    std::size_t path_index;
    int frame_started;
    bool hit_dispatched;
    bool whiffed;
};
```

Implementation location:

- Add `ENGINE/runtime/animation/controllers/shared/internal/enemy_combat_coordinator.hpp`
- Add `ENGINE/runtime/animation/controllers/shared/internal/enemy_combat_coordinator.cpp`
- Refactor `ControllerCombatSystem::try_attack_target()` into intent/commit requests.
- Refactor runtime auto-attack code to call the coordinator instead of independently owning attack commitment.

### Layer 7: Enemy Archetypes

Enemy-specific controllers should configure archetypes instead of assembling low-level behavior every frame.

Proposed archetypes:

- `MeleeChaserEnemy`: spiders, boneski-like enemies.
- `ContactHazardEnemy`: fly-like enemies.
- `ExploderEnemy`: bomb-like enemies.
- `SkittishCritterEnemy`: frog-like enemies.
- `SpawnerEnemy`: egg-like enemies.

Implementation location:

- Add `ENGINE/runtime/animation/controllers/shared/enemy_archetype_controller.hpp`
- Add `ENGINE/runtime/animation/controllers/shared/enemy_archetype_controller.cpp`
- Migrate enemies one archetype at a time.

## Detailed Implementation Plan

## Phase 0: Instrumentation and Safety Rails

### Task 0.1: Add AI debug state dumps

What should change:

- Add structured debug output for enemy perception, intent, movement goal, combat commit phase, cooldown state, and movement result.
- Expose debug names for all AI states.
- Record per-frame metrics for movement goal changes, attack commits, whiffs, blocked movement, and intent transitions.

Why it improves gameplay:

- Makes it possible to distinguish jitter from plan churn, path blockage, animation locks, or combat state transitions.
- Enables future playtest reports to identify root causes instead of symptoms.

Difficulty:

- Medium.

Expected impact:

- Medium immediately; high as a debugging multiplier.

Acceptance criteria:

- Every enemy can report current intent, movement goal, and attack phase.
- Logs show why an intent changed.
- Metrics distinguish attack hit, whiff, cancelled, interrupted, and cooldown outcomes.

### Task 0.2: Add compatibility wrappers around current behavior

What should change:

- Keep current enemy controllers functioning.
- Introduce new APIs behind wrappers so existing calls can be migrated gradually.

Why it improves gameplay:

- Reduces risk during refactor.
- Allows one enemy archetype to migrate at a time.

Difficulty:

- Medium.

Expected impact:

- Medium; mostly reduces implementation risk.

Acceptance criteria:

- Existing controllers compile after wrapper introduction.
- No immediate behavior changes unless explicitly enabled by config.

## Phase 1: Combat Reliability First

Combat reliability is the highest-priority gameplay improvement because unfair or visually disconnected damage is more noticeable than imperfect movement.

### Task 1.1: Create enemy attack profiles

What should change:

- Define `EnemyAttackProfile` data for each enemy attack.
- Include range, startup frames, active frame range, recovery, cooldowns, facing requirements, target prediction policy, and tracking rules.
- Add defaults inferred from existing attack animations only as a temporary compatibility path.

Why it improves gameplay:

- Makes attack timing explicit and tunable.
- Gives designers and programmers a single place to reason about attacks.

Difficulty:

- Medium.

Expected impact:

- High.

Acceptance criteria:

- Spider and boneski primary attacks have explicit attack profiles.
- Attack profile data can be logged during attack selection.
- Profiles support both hit and whiff cooldown policies.

### Task 1.2: Build a unified enemy combat coordinator

What should change:

- Add `EnemyCombatCoordinator` to own attack requests, attack selection, attack commitment, cooldown, active hit dispatch, whiff handling, and recovery.
- Controllers should request an attack but should not dispatch damage directly.
- Runtime auto-attack logic should ask the coordinator whether an attack can start or continue.

Why it improves gameplay:

- Prevents duplicate combat ownership.
- Makes combat timing consistent across enemies.

Difficulty:

- High.

Expected impact:

- Very high.

Acceptance criteria:

- One system owns enemy attack phase transitions.
- Manual controller attack paths and runtime auto-attack paths use the same coordinator.
- Debug logs identify selected profile, target, animation, active frames, cooldown result, and whiff/hit outcome.

### Task 1.3: Remove immediate enemy hit dispatch from attack startup

What should change:

- Refactor `ControllerCombatSystem::try_attack_target()` so it starts or requests an attack commit only.
- Remove immediate calls to `apply_attack_hit()` for enemy attackers.
- Remove or gate fallback contact-hit behavior behind explicit attack profile settings.

Why it improves gameplay:

- Damage occurs only when visible attack frames are active.
- Player trust improves because hit feedback aligns with animation.

Difficulty:

- High.

Expected impact:

- Very high.

Acceptance criteria:

- No enemy melee damage is dispatched on attack startup.
- Damage dispatch requires an active attack window.
- Fallback contact damage is disabled by default.

### Task 1.4: Centralize cooldown policy

What should change:

- Move cooldown decisions into the combat coordinator.
- Support separate cooldowns for start, whiff, hit, interrupt, and recovery complete.
- Remove per-controller cooldown inconsistencies where possible.

Why it improves gameplay:

- Prevents enemies from retrying instantly after whiffs.
- Makes attack cadence predictable and tunable.

Difficulty:

- Medium-high.

Expected impact:

- High.

Acceptance criteria:

- Attack cooldown state is visible in debug output.
- Whiff cooldown and hit cooldown can differ.
- Existing spider and boneski cooldown behavior is migrated to profile-driven policy.

### Task 1.5: Convert fly and bomb combat into committed attacks

What should change:

- Fly aggressive contact damage becomes a profile with tick rate, active window, and cooldown.
- Bomb detonation becomes arming, committed, explosion active, and spent states.
- Bomb damage dispatch moves to explosion active timing instead of immediate trigger timing.

Why it improves gameplay:

- Special enemies become readable and fair.
- Damage no longer appears disconnected from visual telegraphs.

Difficulty:

- Medium.

Expected impact:

- High.

Acceptance criteria:

- Fly damage cannot apply every frame without cooldown/profile gating.
- Bomb explosion damage occurs on active explosion timing, not on trigger entry.
- Bomb has a visible response window unless explicitly configured as instant.

## Phase 2: Movement Goal Stability

### Task 2.1: Add persistent movement goals

What should change:

- Add `MovementGoal` and `MovementGoalResult` types.
- Store the active movement goal in enemy runtime state.
- Only update/replan when the goal changes materially or fails.

Why it improves gameplay:

- Reduces jitter and plan churn.
- Enemies appear to pursue an objective rather than recalculate a target every frame.

Difficulty:

- High.

Expected impact:

- Very high.

Acceptance criteria:

- Repeated pursuit of the same target does not regenerate a plan every frame.
- Movement goal changes are logged with reasons.
- Movement goals report reached, blocked, stale, failed, or interrupted.

### Task 2.2: Replace direct `needs_target` / `target_reached` coupling

What should change:

- Add typed movement status and completion reason.
- Keep legacy `needs_target` and `target_reached` as derived compatibility fields during migration.
- Remove direct business logic dependence on these flags from new AI code.

Why it improves gameplay:

- Reduces hidden state conflicts between AI, animation, and movement planning.

Difficulty:

- Medium-high.

Expected impact:

- High.

Acceptance criteria:

- New enemy AI code reads movement result objects instead of raw flags.
- Legacy flags are written in one compatibility layer.
- Debug output shows which system requested a new target and why.

### Task 2.3: Add movement hysteresis and range bands

What should change:

- Replace single thresholds with enter/exit thresholds.
- Use desired range bands for approach, hold range, attack prep, and retreat.
- Prevent immediate mode flips near range boundaries.

Why it improves gameplay:

- Reduces jitter around attack radius and desired standoff distance.

Difficulty:

- Medium.

Expected impact:

- High.

Acceptance criteria:

- Enemies do not alternate approach/attack/recover on adjacent frames due only to small distance changes.
- Range band values are configurable by archetype/profile.

## Phase 3: Intent and Decision Architecture

### Task 3.1: Introduce enemy perception snapshots

What should change:

- Build `EnemyPerceptionSystem`.
- Provide target validity, room membership, horizontal range, vertical delta, target velocity, ground contact, and recent movement failure data.

Why it improves gameplay:

- Decisions become based on explicit facts instead of ad hoc controller checks.

Difficulty:

- Medium.

Expected impact:

- High.

Acceptance criteria:

- Spider, boneski, bomb, fly, and frog can all consume a perception snapshot.
- Room membership and target validity checks are centralized.
- Horizontal and vertical distances are reported separately.

### Task 3.2: Introduce durable enemy intents

What should change:

- Build `EnemyIntentSystem`.
- Replace direct distance-to-phase decisions with durable intents.
- Add minimum duration, maximum duration, interrupt rules, and transition reasons.

Why it improves gameplay:

- Enemies commit to behavior long enough to look purposeful.

Difficulty:

- High.

Expected impact:

- Very high.

Acceptance criteria:

- Intent transitions are logged and reasoned.
- Each intent has movement and combat permissions.
- Attack intent cannot be interrupted by trivial range noise unless configured.

### Task 3.3: Add utility scoring for tactical choices

What should change:

- Add utility scores for pursue, attack, reposition, recover, retreat, and return-home.
- Include health, target distance, target velocity, attack readiness, crowding, blocked movement, and recent failures.

Why it improves gameplay:

- Allows more nuanced behavior than hard-coded if/else thresholds.

Difficulty:

- High.

Expected impact:

- High.

Acceptance criteria:

- Utility scores can be dumped in debug mode.
- Enemies can prefer reposition over attack when blocked or crowded.
- Recover duration can vary by tactical context.

## Phase 4: Navigation and Steering Redesign

### Task 4.1: Add navigation waypoints before animation stride planning

What should change:

- Create an enemy navigation layer that chooses a local waypoint or path corridor.
- Keep `GetBestPath3D` focused on choosing animation-compatible strides toward the current waypoint.

Why it improves gameplay:

- Decouples route choice from animation stride selection.
- Reduces local minima and path jitter.

Difficulty:

- Very high.

Expected impact:

- Very high.

Acceptance criteria:

- Movement can keep the same route across multiple frames.
- Replanning reason is explicit: blocked, stale, target moved too far, or goal changed.
- Animation stride planner no longer owns tactical destination choice.

### Task 4.2: Add local avoidance and unstick escalation

What should change:

- Add local steering around blockers before aborting movement.
- Escalate repeated no-progress frames to reposition, fallback route, or intent change.

Why it improves gameplay:

- Enemies should not simply stop, jitter, or return home after repeated blocked movement.

Difficulty:

- High.

Expected impact:

- High.

Acceptance criteria:

- Repeated blocked movement produces a clear reposition or route-change action.
- No-progress counters feed into intent decisions.
- Enemies avoid obvious blockers when a nearby alternative exists.

### Task 4.3: Add steering behaviors for pursuit, orbit, retreat, and hold range

What should change:

- Implement steering outputs for pursue target, maintain range, orbit, flank, retreat, and return home.
- Feed steering into movement goals and local waypoints.

Why it improves gameplay:

- Enemies gain movement styles beyond direct chase and fixed retreat.

Difficulty:

- High.

Expected impact:

- High.

Acceptance criteria:

- Melee enemies can approach on arcs rather than only direct lines.
- Recover can pick safe reposition points instead of blind retreat.
- Flying enemies can orbit with collision-aware steering.

## Phase 5: Combat Positioning and Group Coordination

### Task 5.1: Add combat slot reservations

What should change:

- Add shared reservations around the player/target.
- Limit active attackers and assign waiting/repositioning enemies to slots.

Why it improves gameplay:

- Reduces clumping and unfair dogpiling.

Difficulty:

- High.

Expected impact:

- High.

Acceptance criteria:

- Multiple enemies do not all select the same attack position.
- Active attacker count is configurable.
- Non-attacking enemies reposition or hold range instead of stacking.

### Task 5.2: Add scored combat position candidates

What should change:

- Generate candidate positions around the target.
- Score by distance band, angle, reachability, obstacle clearance, reservation status, and recent failures.

Why it improves gameplay:

- Enemies look like they are trying to create openings rather than simply chasing.

Difficulty:

- High.

Expected impact:

- High.

Acceptance criteria:

- Position choices include a logged score and reason.
- Enemies can choose flank/reposition over direct approach.
- Failed positions are temporarily penalized.

## Phase 6: Enemy Archetype Migration

### Task 6.1: Build `MeleeChaserEnemy` archetype

What should change:

- Encapsulate spider and boneski shared behavior.
- Configure ranges, movement style, attack profiles, recovery, and slot behavior through data.

Why it improves gameplay:

- Fixes core melee behavior once instead of separately per enemy.

Difficulty:

- High.

Expected impact:

- Very high.

Acceptance criteria:

- Spider and boneski no longer manually call low-level attack dispatch from `on_update()`.
- Both use shared intent, movement goal, positioning, and combat coordinator systems.

### Task 6.2: Build `ExploderEnemy` archetype

What should change:

- Encapsulate bomb trigger, arming, detonation, explosion active, and self-destruction flow.

Why it improves gameplay:

- Makes explosive enemies fair and telegraphed.

Difficulty:

- Medium.

Expected impact:

- High.

Acceptance criteria:

- Bomb has configurable arming and explosion active windows.
- Explosion damage is synchronized with animation/profile timing.

### Task 6.3: Build `ContactHazardEnemy` archetype

What should change:

- Encapsulate fly orbit, aggression, contact damage cadence, and cooldowns.

Why it improves gameplay:

- Contact damage becomes predictable instead of frame-overlap dependent.

Difficulty:

- Medium.

Expected impact:

- Medium-high.

Acceptance criteria:

- Fly damage applies at configured cadence.
- Fly aggressive state has telegraph/debug information.

### Task 6.4: Build `SkittishCritterEnemy` archetype

What should change:

- Encapsulate frog flee, idle, wander, and safe-distance behavior.
- Replace random wandering with valid safe-position selection.

Why it improves gameplay:

- Passive enemies look intentional rather than random.

Difficulty:

- Medium.

Expected impact:

- Medium.

Acceptance criteria:

- Frog picks validated flee/idle positions.
- Frog does not repeatedly choose blocked or unsafe random destinations.

## Phase 7: Attack Prediction and Hit Detection Quality

### Task 7.1: Make attack prediction profile-driven

What should change:

- Replace hard-coded prediction horizon and padding values with attack profile data.
- Use target velocity smoothing.
- Clamp prediction based on attack tracking rules.

Why it improves gameplay:

- Attacks feel more accurate and less robotic.

Difficulty:

- Medium-high.

Expected impact:

- High.

Acceptance criteria:

- Each attack profile controls prediction horizon and padding.
- Fast attacks and slow attacks can use different prediction logic.
- Debug output explains why an attack candidate was accepted or rejected.

### Task 7.2: Add visible fairness constraints

What should change:

- Add optional line-of-sight, facing cone, and active-frame-only hit checks.
- Require current visual attack geometry unless an attack profile explicitly enables forgiving contact logic.

Why it improves gameplay:

- Reduces hits that do not match what the player sees.

Difficulty:

- Medium.

Expected impact:

- High.

Acceptance criteria:

- Forgiving hit logic is profile-configured, not a silent fallback.
- Attack rejections identify whether range, facing, active frame, or hitbox caused the rejection.

## Phase 8: Data and Tooling

### Task 8.1: Add enemy AI tuning data

What should change:

- Move ranges, cooldowns, intent durations, movement tolerances, slot settings, and attack profiles into data/config where practical.

Why it improves gameplay:

- Makes enemy feel tunable without recompiling logic.

Difficulty:

- Medium.

Expected impact:

- High for iteration speed.

Acceptance criteria:

- Spider and boneski core tuning values are no longer hard-coded only in constructors.
- Attack profiles can be adjusted from data or a centralized config structure.

### Task 8.2: Add editor/debug visualization hooks

What should change:

- Visualize current intent, movement goal, desired position, active slot, attack range, active hit window, and predicted target position.

Why it improves gameplay:

- Designers and developers can see why enemies behave the way they do.

Difficulty:

- Medium-high.

Expected impact:

- High for iteration and debugging.

Acceptance criteria:

- Debug overlay can show active AI state per enemy.
- Attack windows and movement goals are visible during playtest/debug mode.

## Suggested Migration Order

1. Add instrumentation and compatibility wrappers.
2. Add attack profiles for spider and boneski.
3. Build combat coordinator.
4. Remove immediate hit dispatch from manual enemy attacks.
5. Convert spider to coordinator-driven attacks.
6. Convert boneski to coordinator-driven attacks.
7. Add persistent movement goals.
8. Convert spider and boneski movement to persistent goals.
9. Add intent system with hysteresis.
10. Replace `decide_enemy_phase()` usage for spider and boneski.
11. Add combat positioning slots for melee enemies.
12. Convert bomb to telegraphed `ExploderEnemy` behavior.
13. Convert fly to cooldown/profile-based `ContactHazardEnemy` behavior.
14. Convert frog to `SkittishCritterEnemy` with safe-position selection.
15. Split navigation waypoints from animation stride planning.
16. Add steering and local avoidance improvements.
17. Move tuning into data/config and add visualization tools.

## Validation Plan

### Static validation

- Confirm enemy controllers no longer dispatch direct damage during `on_update()`.
- Confirm attack dispatch occurs only from active attack windows.
- Confirm movement goals are persistent and not recreated every frame for unchanged pursuit.
- Confirm new AI state objects have explicit debug names and transition reasons.

### Unit and regression tests

Add tests for:

- Intent hysteresis near attack radius.
- Movement goal reuse when the target moves only slightly.
- Movement goal invalidation when blocked repeatedly.
- Attack profile selection by range and facing.
- Attack startup does not damage.
- Active-frame attack does damage when boxes overlap.
- Whiff cooldown applies after a missed active window.
- Bomb arming does not immediately damage.
- Fly contact damage respects cooldown/tick policy.

Suggested test files:

- `ENGINE/tests/enemy_intent_regression_test.cpp`
- `ENGINE/tests/enemy_movement_goal_regression_test.cpp`
- `ENGINE/tests/enemy_combat_coordinator_regression_test.cpp`
- `ENGINE/tests/enemy_attack_window_regression_test.cpp`
- `ENGINE/tests/enemy_special_archetype_regression_test.cpp`

### Playtest validation

Track metrics:

- Intent transitions per enemy per second.
- Movement replans per enemy per second.
- Movement blocked/no-progress frames.
- Attack commits, active windows, hits, whiffs, interrupts, and cooldown starts.
- Damage events outside active windows; expected value should be zero except explicit hazards.
- Number of active attackers around the player.
- Enemy overlap/crowding near the player.

Qualitative checks:

- Spider approaches smoothly and commits attacks visibly.
- Boneski does not instantly damage on attack start.
- Bomb gives a readable arming/explosion sequence.
- Fly damage feels periodic and readable, not continuous/random.
- Frog flees to plausible safe positions instead of jittering randomly.
- Groups avoid clumping and do not all attack simultaneously unless configured.

## Major Redesign Decisions

### Decision 1: Do not keep patching direct attack dispatch

Direct damage dispatch from controller `on_update()` should be considered legacy behavior. It should be replaced by attack commitment and active-window dispatch.

### Decision 2: Do not make animation stride planning solve navigation alone

Animation stride selection should execute movement style. It should not be the only pathfinding/steering intelligence.

### Decision 3: Do not use one distance threshold as a state machine

Use range bands, hysteresis, utility scores, and durable intents.

### Decision 4: Do not let every enemy controller own low-level movement and combat

Enemy controllers should configure archetypes and profiles. Shared systems should own behavior mechanics.

## Risks and Mitigations

### Risk: Large refactor causes behavior regressions

Mitigation:

- Introduce systems behind compatibility wrappers.
- Migrate one archetype at a time.
- Keep old behavior toggles temporarily.
- Add regression tests before switching defaults.

### Risk: Attack windows require asset data cleanup

Mitigation:

- Build temporary inference tools for attack profile defaults.
- Add debug visualization for active hitboxes and active frames.
- Allow profiles to override imperfect animation metadata while assets are cleaned up.

### Risk: Navigation rewrite is expensive

Mitigation:

- First stabilize movement goals without replacing `GetBestPath3D`.
- Then introduce navigation waypoints as an intermediate layer.
- Only later add richer steering/local avoidance.

### Risk: Enemy behavior becomes too complex to tune

Mitigation:

- Keep archetype presets simple.
- Add debug score dumps.
- Centralize data and avoid scattering constants across controllers.

## Definition of Done

The AI_FIX work should be considered complete when:

- Enemy attacks are selected, committed, dispatched, recovered, and cooled down by one unified combat path.
- Damage for normal enemy attacks occurs only during active attack windows.
- Enemies use persistent movement goals instead of recreating micro-plans every frame.
- Enemy decisions use durable intents with hysteresis and transition reasons.
- Ground combat range uses horizontal distance plus vertical eligibility.
- Melee enemies can position, approach, attack, recover, and re-engage without obvious jitter or clumping.
- Special enemies such as bombs and flies use readable attack profiles.
- Low-level direct combat/movement calls in enemy-specific controllers are replaced by archetype configuration.
- Debug metrics and tests can identify whether a poor AI moment came from intent, positioning, navigation, animation, collision, or combat validation.
