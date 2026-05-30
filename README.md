# VIBBLE - 2D Game Engine (Departed Affairs and Co.)


## Installation
### Quick Start (Windows)
1. Clone the repository.
2. Run `setup.bat` from the project root if you want to build and compile.

The script installs build tools (Git, MSVC Build Tools, CMake, Ninja, vcpkg), pulls dependencies, configures a RelWithDebInfo build, compiles, and launches the engine. Windows 10/11 and an internet connection are required, and administrator permissions are recommended for installing the tools.


## Overview
- SDL3-based 2D engine; content is stored in external JSON-driven files for maps, assets, lighting, and animations.
- Manifest-driven loading keeps game data outside the executable so asset changes do not require recompilation.
- Built-in dev mode inside the engine for editing rooms, lighting, assets, and spawn points, with immediate writes back to the content files.
- Asset tools regenerate animation and lighting caches only on explicit request and when repairing missing files.
- Asset manifest launch-flag contract and floor-box schema: `ENGINE/runtime/core/manifest/ASSET_MANIFEST_SCHEMA.md`.

## Running
- Recommended: run `run.bat` to configure, build, and launch the engine.
- To build from scratch, run `compile_and_run.bat`; it launches the `release\engine.exe` produced by the build process.
- Repeated runs use the configured build; run `compile_and_run.bat` again after pulling dependency changes.

## Dev Mode
- Toggle with `Ctrl+D`, through the pause menu (`Esc`), or auto-enable when the map has no player.
- Reduces render quality for better responsiveness and exposes editors for maps, assets, lighting, and spawn points. Settings are saved in `dev_mode_settings.json`.

## Custom Controllers
- Add new controllers under `ENGINE/runtime/animation/controllers/custom_controllers/` and register them in `ControllerFactory::create_by_key`.
- Controllers are linked by asset name (for example, the `spider` asset maps to `spider_controller`).

## Tests

## License

MIT License - see `LICENSE`.


# Custom Controllers And Helpers

## Canonical API
- Include `ENGINE/runtime/animation/controllers/custom_controller.hpp`.
- Derive from `custom_controller_api::CustomControllerBase`.
- Use `custom_controller_api::MovementConfig` and `custom_controller_api::EnemyAgentConfig` for movement + enemy policy.

## Enemy Behavior Contract
- Runtime auto-attack is canonical for enemy assets with attack animations.
- `run_enemy_behavior(...)` drives one policy pipeline:
  - `Acquire` -> `Approach` -> `AttackWindow` -> `Recover` -> `ReturnHome`
- Range semantics are explicit:
  - `ranges.aggro_radius_px`: engagement radius.
  - `ranges.desired_standoff_px`: approach stop distance.
  - `ranges.attack_radius_px`: attack-window entry distance.
- Migration reference: `ENGINE/runtime/animation/controllers/ENEMY_AI_MIGRATION.md`.

## Public vs Internal
- Public: lifecycle hooks on `CustomControllerBase`, context helpers, movement helpers, combat helpers, and child/anchor helpers.
- Internal/private: runtime backend bookkeeping, movement/combat/behavior internals, low-level attack scan plumbing.

## Factory Linking
- Controllers are linked by normalized key `<asset>_controller`.
- Registration lives in `ENGINE/runtime/assets/asset/controller_factory.cpp`.
- Unknown keys fall back to `CustomControllerBase` generic behavior.

## Editor Scaffolding
- Devtools controller generation is implemented by:
  - `ENGINE/editor/devtools/asset_editor/animation_editor_window/CustomControllerService.cpp`
- Generated controllers now target `CustomControllerBase` directly.
