# Codex Playtest Harness

This repo has a Codex-only playtest harness for reproducing normal game mode stalls with logging, frame stats, automatic map selection, and automated player input.

## Human Instructions

Ask Codex to use this exact document when you want an automated playtest. Good prompts:

- `Read docs/CODEX_PLAYTEST.md, run the Codex playtest harness on forrest, and report freeze or frame-stat issues.`
- `Using docs/CODEX_PLAYTEST.md, run a 30 second Codex playtest and give me a fix plan for any normal mode stall clues.`
- `Read docs/CODEX_PLAYTEST.md, run codex_playtest.bat, then inspect the report and frame stats for movement-related freezes.`

Useful overrides before running:

```bat
set CODEX_PLAYTEST_SECONDS=30
set CODEX_PLAYTEST_FRAME_LIMIT=1800
set CODEX_PLAYTEST_MAP=forrest
codex_playtest.bat
```

Default behavior is map `forrest`, about 60 seconds, and a frame limit based on 60 FPS. The harness writes `codex_playtest_report.md`, `log.txt`, `runtime_frame_stats.csv`, and Codex playtest stdout/stderr logs.

## Codex Instructions

When the user references this file, run the harness from the repo root:

```bat
codex_playtest.bat
```

For fast validation, use:

```bat
set CODEX_PLAYTEST_SECONDS=5
set CODEX_PLAYTEST_FRAME_LIMIT=300
codex_playtest.bat
```

The harness sets `VIBBLE_AUTOSTART_MAP=forrest`, `VIBBLE_RUNTIME_FRAME_LIMIT`, and `VIBBLE_CODEX_PLAYTEST_INPUT=1`. The in-engine input driver uses deterministic pseudo-random movement segments: short bursts, medium holds, and long single-direction holds. Long holds are required because normal mode stalls often show up while the player keeps moving in one direction.

After the run, inspect:

- `codex_playtest_report.md` for build/run status, freeze signals, worst frames, warning/error summaries, and likely investigation points.
- `runtime_frame_stats.csv` for spikes in `main.frame_total_ms`, `main.assets_update_ms`, `assets.world_ms`, `assets.visibility_ms`, `assets.runtime_effects_ms`, `assets.render_ms`, and movement/path-blocking metrics.
- `codex_playtest.input_driver`, `codex_playtest.segment_kind`, `codex_playtest.segment_length_frames`, `movement.player_has_intent`, `movement.player_input_x`, and `movement.player_input_y` to confirm the player was being driven.
- `log.txt` or `codex_playtest_stdout.log` for startup, map selection, runtime warnings, and the last log line before a timeout or stall.

If the user asks for a diagnosis, summarize the observed data first, then identify the most likely subsystem. If the user asks for a fix, produce a concrete implementation plan or implement the fix if appropriate. If the run times out or has a forced kill, treat it as a freeze reproduction and prioritize the last log lines plus the final frame-stat rows before the stall.
