# Codex Playtest Report

Generated: 2026-05-29 18:51:44 -07:00

## Result
- Build exit code: 0
- Run exit code: 0
- Duration: 1.00 seconds
- Timed out: False
- Forced kill: False
- Requested map: forrest
- Playtest profile: spider_slow
- Map selected: True
- Game loop started: True
- Spider attack observed on vibble: False
- Frame limit: 9000

## Frame Stats
- Rows captured: 0
- Primary frame metric: main.frame_total_ms
- Average frame: n/a
- P95 frame: n/a
- Max frame: n/a
- Frames > 33 ms: 0
- Frames > 50 ms: 0
- Frames > 100 ms: 0

### Player Input
- Codex driver frames: 0
- Movement-intent frames: 0
- Long-hold frames: 0
- Burst frames: 0

### Suspicious Metrics
- main.raw_frame_total_ms: n/a
- main.frame_total_ms: n/a
- main.frame_begin_interval_ms: n/a
- main.assets_update_ms: n/a
- main.present_ms: n/a
- main.gameplay_active: n/a
- main.idle_wait_used: n/a
- assets.world_ms: n/a
- assets.visibility_ms: n/a
- assets.runtime_effects_ms: n/a
- assets.render_ms: n/a
- render.draw_submission_ms: n/a
- render.submit_unaccounted_ms: n/a
- render.target_sync_ms: n/a
- render.first_ensure_targets_ms: n/a
- render.final_ensure_targets_ms: n/a
- render.sdl_render_target_ms: n/a
- render.sdl_render_texture_ms: n/a
- render.sdl_render_geometry_ms: n/a
- render.diagnostics_stale: n/a
- movement.player_path_blocked_ms: n/a
- movement.player_path_blocked_checks: n/a
- enemy_ai.phase: n/a
- enemy_ai.no_progress_frames: n/a
- enemy_ai.return_home_fallback_count: n/a
- enemy_ai.approach_attempted: n/a
- enemy_ai.approach_moved: n/a
- enemy_ai.recover_active: n/a
- enemy_ai.attack_window_enter_count: n/a
- enemy_ai.attack_window_exit_count: n/a
- enemy_ai.hit_dispatch_attempt_count: n/a
- enemy_ai.hit_dispatch_success_count: n/a
- enemy_ai.active_target_scan_hits: n/a
- enemy_ai.movement_attack_conflict_flag: n/a
- combat.vibble_last_attacker: n/a
- dynamic_spawn.sync_ms: n/a
- camera.update_ms: n/a
- camera.visible_grid_ms: n/a
- asset_runtime_animation_load.attempted: n/a
- asset_runtime_animation_load.ms: n/a
- asset_runtime_animation_load.deferred: n/a
- assets.active_count: n/a
- assets.filtered_active_count: n/a
- assets.active_fail_open_frames: n/a
- assets.startup_safety_active: n/a

### Worst Frames
- No frame rows with `main.frame_total_ms` were available.

### Top Frames By Metric
- main.raw_frame_total_ms: n/a
- main.frame_begin_interval_ms: n/a
- main.present_ms: n/a
- assets.render_ms: n/a
- render.submit_unaccounted_ms: n/a

### Movement Cause Summary
- Movement/path, dynamic spawn, visibility, and camera/grid metrics did not exceed hitch thresholds in this run.

## Freeze Signals
- No frame-stat rows were captured.

## Log Issues
- Warnings: 115
- Errors/Fatal: 0
- Final log line: [INFO] +97.348s: [MenuUI] Runtime frame limit reached; exiting automation run.

### Recent Warnings
- [WARN] +82.071s: [Assets] Recoverable OpenGL runtime frame deferral detected. Skipping this frame and allowing deferred target creation to complete.
- [WARN] +82.205s: [Assets] Recoverable OpenGL runtime frame deferral detected. Skipping this frame and allowing deferred target creation to complete.
- [WARN] +82.371s: [Assets] Recoverable OpenGL runtime frame deferral detected. Skipping this frame and allowing deferred target creation to complete.
- [WARN] +82.837s: [Assets] Recoverable OpenGL runtime frame deferral detected. Skipping this frame and allowing deferred target creation to complete.
- [WARN] +82.870s: [Assets] Recoverable OpenGL runtime frame deferral detected. Skipping this frame and allowing deferred target creation to complete.
- [WARN] +83.145s: [Assets] Recoverable OpenGL runtime frame deferral detected. Skipping this frame and allowing deferred target creation to complete.
- [WARN] +83.411s: [Assets] Recoverable OpenGL runtime frame deferral detected. Skipping this frame and allowing deferred target creation to complete.
- [WARN] +83.686s: [Assets] Recoverable OpenGL runtime frame deferral detected. Skipping this frame and allowing deferred target creation to complete.
- [WARN] +83.770s: [Assets] Recoverable OpenGL runtime frame deferral detected. Skipping this frame and allowing deferred target creation to complete.
- [WARN] +83.869s: [Assets] Recoverable OpenGL runtime frame deferral detected. Skipping this frame and allowing deferred target creation to complete.
- [WARN] +84.004s: [Assets] Recoverable OpenGL runtime frame deferral detected. Skipping this frame and allowing deferred target creation to complete.
- [WARN] +85.203s: [Assets] Recoverable OpenGL runtime frame deferral detected. Skipping this frame and allowing deferred target creation to complete.

## Likely Investigation Points
- No single dominant freeze signal was detected; compare worst frames against the suspicious metric table.
- Spider attack verification failed: no spider-origin hit on vibble was observed.

## Files
- Runtime log: C:\Users\cal_m\OneDrive\Documents\GitHub\ENGINE-2.0\log.txt
- Frame stats: C:\Users\cal_m\OneDrive\Documents\GitHub\ENGINE-2.0\runtime_frame_stats.csv
- Launcher log: codex_playtest_launcher.log
- stdout: codex_playtest_stdout.log
- stderr: codex_playtest_stderr.log

## Verification Failure
- spider_slow profile requires at least one spider-origin hit on vibble, but none were observed.
