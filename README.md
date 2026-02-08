# VIBBLE - 2D Game Engine (Departed Affairs and Co.)


## Installation
### Quick start (Windows)
1. Clone the repo.
2. Run `setup.bat` from the project root if you wish to build and compile compile.

The script installs build tools (Git, MSVC build tools, CMake, Ninja, vcpkg), fetches dependencies, configures a RelWithDebInfo build, compiles, and launches the engine. Requires Windows 10/11, internet, and admin rights are recommended for tool installs.


## Overview
- SDL3-based 2D engine; content lives in external JSON-driven files for maps, assets, lighting, and animations.
- Manifest-driven loading keeps game data out of the executable so asset changes do not require recompiles.
- Dev Mode ships in-engine for editing rooms, lighting, assets, and spawns with immediate write-through to content files.
- Asset tools regenerate animation and lighting caches from manifest flags to keep generated art in sync.

## Project Layout
- `ENGINE/runtime/`: Runtime source for assets, controllers, rendering, UI, and shared utilities.
- `ENGINE/editor/devtools/`: In-engine editors for assets, maps, lighting, and spawn tooling.
- `ENGINE/tooling/asset_pipeline/`: Offline pipelines (asset toolkit, cache manager, effect parsers) that feed the runtime cache.
- `resources/`: Source art, icons, loading screens, fog textures, and other media consumed at runtime.
- `cache/`: Generated texture/cache exports produced by the asset pipeline and tooling.
- `MAPS/`: Map layouts, rooms, and spawn data referenced by the manifest.
- `content/`: Runtime content packs (e.g., `content/test`, `content/forrest`).
- `engine/tools/`: C++ cache utilities like `asset_tool_cli`, `set_rebuild_cli`, and `apply_effects_cli`.
- `TESTS/`: Unit tests for core systems.
- `vcpkg/`, `external/`: Dependency management and bundled libs.

## Running
- Preferred: run `run.bat` to configure, build, and start the engine.
- To build fresh run `compile_and_run.bat`; it launches `release\engine.exe` produced by the build.
- Repeat runs reuse the configured build; rerun `compile_and_run.bat` after pulling dependency changes.

## Dev Mode
- Toggle with `Ctrl+D` or through the pause menu (`Esc`), or it auto-enables when a map lacks a player.
- Reduces render quality for responsiveness and exposes editors for maps, assets, lighting, and spawns. Settings persist in `dev_mode_settings.json`.

## Custom Controllers
- Add new controllers under `ENGINE/runtime/animation/controllers/custom_controllers/` and register them in `ControllerFactory::create_by_key`.
- Link from content with `"custom_controller_key": "YourController"` in an asset `info.json` or via the Dev Mode editors.

## Testing
- Build tests: `cmake --build --preset windows-vcpkg-release --target engine_tests`
- Run tests: `ENGINE/engine_tests.exe`

## License

MIT License - see `LICENSE`.
