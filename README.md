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
- Asset tools regenerate animation and lighting caches from explicit requests and missing-file repair.

## Running
- Preferred: run `run.bat` to configure, build, and start the engine.
- To build fresh run `compile_and_run.bat`; it launches `release\engine.exe` produced by the build.
- Repeat runs reuse the configured build; rerun `compile_and_run.bat` after pulling dependency changes.

## Dev Mode
- Toggle with `Ctrl+D` or through the pause menu (`Esc`), or it auto-enables when a map lacks a player.
- Reduces render quality for responsiveness and exposes editors for maps, assets, lighting, and spawns. Settings persist in `dev_mode_settings.json`.

## Custom Controllers
- Add new controllers under `ENGINE/runtime/animation/controllers/custom_controllers/` and register them in `ControllerFactory::create_by_key`.
- Controllers are linked by asset name (for example, asset `spider` maps to `spider_controller`).

## Testing
- Build tests: `cmake --build --preset windows-vcpkg-release --target engine_tests`
- Run tests: `ENGINE/engine_tests.exe`

## License

MIT License - see `LICENSE`.
