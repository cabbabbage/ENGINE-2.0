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

## What a custom controller is
Custom controllers are this engine's equivalent of Unity scripts/C++ blueprints for asset behavior.
Each asset can map to a controller by asset name (for example `frog` -> `frog_controller`).

## Canonical files
- Runtime base + facade:
  - `ENGINE/runtime/animation/controllers/shared/custom_asset_controller.hpp`
  - `ENGINE/runtime/animation/controllers/shared/custom_controller_api.hpp`
- Built-in custom controllers:
  - `ENGINE/runtime/animation/controllers/custom_controllers/`
- Factory mapping:
  - `ENGINE/runtime/assets/asset/controller_factory.cpp`
- Dev-mode generator/open flow:
  - `ENGINE/editor/devtools/asset_editor/animation_editor_window/CustomControllerService.cpp`

## Authoring include and namespace
Use one include in custom controllers:

```cpp
#include "animation/controllers/shared/custom_controller_api.hpp"
```

Use the short API namespace:

```cpp
custom_controller_api::resolve_valid_player_target(ctx);
```

## Lifecycle hooks (author surface)
Custom controllers derive from `CustomAssetController` and can override:
- `on_init()`: constructor-time setup hook.
- `on_update(const Input&)`: per-frame behavior.
- `on_attack(const animation_update::Attack&)`: called for each pending incoming attack before processing.
- `on_hit(const animation_update::Attack&)`: called when damage was applied and asset survived.
- `on_death()`: called when pending attack processing results in death.
- `on_no_pending_attacks()`: called when there were no pending attacks that tick.
- `on_process_pending_attacks(Asset&)`: post-processing hook (keep terminology; do not duplicate with separate damage hook).
- `on_orphaned_hook(Asset&, Asset*)`: called when orphaned.
- `on_pre_delete_hook(Asset&)`: called before delete.

Default recommendation: call `CustomAssetController::<same_hook>(...)` first in overrides, then custom logic.

## Runtime context access
- Read-only frame context:
  - `game_context()`
- Access current asset and assets manager:
  - `self_ptr()`
  - `assets()`
- Controlled mutable runtime game context:
  - `mutable_runtime_game_context()`

Use mutable context intentionally for runtime gameplay state updates.

## Common helper cookbook
- Resolve valid player target:
  - `custom_controller_api::resolve_valid_player_target(game_context())`
- Contact attack dispatch:
  - `custom_controller_api::dispatch_contact_attack(game_context())`
- Reverse animation helpers:
  - `begin_reverse_current_animation_until_stop(...)`
  - `begin_reverse_current_animation_to_default(...)`
  - `stop_reverse_current_animation(...)`
- Attack processing defaults:
  - provided by `CustomAssetController` pending-attack pipeline.

## Factory linking behavior
- Controllers are linked by normalized asset name key with `_controller` suffix.
- New generated controllers are inserted into `controller_factory.cpp` include and registry markers.
- Unknown keys fall back to base `CustomAssetController`.

## Dev-mode behavior (create/open)
- **Add Controller**:
  - uses `CustomControllerService` only.
  - creates `.hpp/.cpp` scaffold under `ENGINE/runtime/animation/controllers/custom_controllers/`.
  - auto-registers in controller factory.
- **Open Controller**:
  - opens existing header/source files.
  - **does not mutate files** (no metadata/factory/manifest/source rewrite on open).

## Troubleshooting
- Controller button says Add but you expect Open:
  - verify both `.hpp` and `.cpp` exist in `custom_controllers/`.
- Controller not running in runtime:
  - verify factory include + registry entry exist for your key.
  - verify asset name normalizes to your controller key (`<asset>_controller`).
- Behavior not receiving attack callbacks:
  - ensure asset can receive attacks (hitbox enabled, active, alive).
