# Current Runtime Renderer Pipeline (Authoritative)

This document describes how rendering currently works in `ENGINE-2.0` as implemented in code today.
It intentionally replaces older renderer planning notes and focuses on the active runtime path.

## 1. High-Level Architecture

The runtime path is GPU-only and strict-fail:

1. `EngineRenderer` creates an SDL GPU renderer at startup.
2. `Assets` owns a `SceneRenderer`.
3. `SceneRenderer` owns `RuntimeGpuRenderer`.
4. `RuntimeGpuRenderer` builds frame data and drives a `GpuRuntimePipeline`.
5. `GpuRuntimePipeline` enqueues an explicit pass graph into `GpuSceneRenderer`.
6. `GpuSceneRenderer` executes that graph through `GpuFrameGraph` + `GpuRenderDevice` using `SDL_GPU` command buffers and swapchain presentation.

There is no runtime fallback to a software/legacy renderer path.
If required GPU setup fails, startup fails.
If a runtime GPU frame fails, the frame is treated as fatal.

## 2. Startup Path and Ownership Chain

### 2.1 Renderer bootstrap

`EngineRenderer::Create(...)`:

- Enumerates GPU drivers and probes adapter context.
- Attempts GPU renderer creation via SDL renderer properties (`SDL_GPU_RENDERER`).
- Prefers high-performance GPU usage.
- Can retry backend hints (for example direct3d12/vulkan on Windows) if the first result appears to miss a preferred dedicated GPU.
- Rejects startup if no valid GPU renderer is available.

### 2.2 Format policy resolution

`GpuFormatPolicyResolver::Resolve(...)` runs strict format capability checks against `SDL_GPUDevice`:

- Albedo: prefers `R8G8B8A8_UNORM_SRGB`, falls back to `R8G8B8A8_UNORM`.
- Light accumulation: prefers `R16G16B16A16_FLOAT`, falls back to `R11G11B10_UFLOAT`.
- Mask: requires `R8_UNORM`.
- Depth: prefers `D24_UNORM_S8_UINT`, falls back to `D32_FLOAT` when needed.
- Sample count probe logs 4x/2x support; policy stores selected sample count.

If required formats are unsupported, startup fails.

### 2.3 Runtime scene renderer creation

`Assets` constructor creates `SceneRenderer` (if SDL renderer exists).

`SceneRenderer` constructor:

- Verifies prerequisites (`SDL_Renderer*`, `Assets*`).
- Creates `RuntimeGpuRenderer`.
- Treats failure as fatal (`throw std::runtime_error`).

### 2.4 Runtime GPU renderer initialization

`RuntimeGpuRenderer::initialize(...)`:

1. Creates `GpuSceneRenderer` (which wraps `GpuRenderDevice`).
2. Resolves shader manifest path from:
   - `VIBBLE_GPU_SHADER_MANIFEST` (SDL env), then
   - `VIBBLE_GPU_SHADER_MANIFEST` (process env), then
   - default: `ENGINE/runtime/rendering/shaders/runtime_shaders.json`.
3. Requires manifest file to exist.
4. Loads shader packages through `ShaderPackageLibrary` (strict validation).
5. Allocates scene graph resources (`scene.floor`, `scene.layers`, `scene.composite`) and shared sampler (`linear_clamp`).
6. Runs startup probe frame by enqueuing and executing the full runtime graph once (`startup_probe` prefix).

Any failure in this chain fails initialization.

## 3. Shader Package and Pipeline Strictness

## 3.1 Manifest and payload validation

`ShaderPackageLibrary::load_from_manifest(...)` enforces:

- Valid JSON and `manifest_version > 0`.
- `variants` object presence.
- Per-variant backend descriptor parse.
- Payload file existence and non-empty content.
- Placeholder text rejection.
- Binary magic checks:
  - DXIL container header (`DXBC`).
  - SPIR-V magic (`0x07230203`).
- Optional size/hash metadata validation (`file_size_bytes`, `hash_fnv1a64`).

### 3.2 Backend selection

`GpuSceneRenderer` chooses backend shader format from device support:

- Prefers `DXIL` if available.
- Else `SPIR-V`.
- Else fail.

### 3.3 Required variants and warmup

Current required runtime graphics variants are:

- `fullscreen_vertex`
- `sprite_batch_vertex`
- `sprite_textured`
- `sprite_batched`
- `floor_compose`
- `final_compose`

`GpuSceneRenderer::warmup_required_pipelines(...)` validates stages and creates required graphics pipelines up front through cache keys, so first runtime frames do not discover missing critical pipelines late.

## 4. Per-Frame Runtime Flow

### 4.1 Main loop stage placement

`Assets::update(...)` runs staged frame logic:

1. World update stage.
2. Visibility/traversal rebuild stage.
3. Runtime effects convergence stage.
4. Render stage (`render_runtime_frame()`).

Render stage calls `scene->render()` when rendering is not suppressed.

### 4.2 SceneRenderer frame contract

`SceneRenderer::render()`:

- Starts diagnostics frame (`render_diagnostics::begin_frame`).
- Calls `RuntimeGpuRenderer::render_frame(...)`.
- On GPU frame failure:
  - marks diagnostics failure,
  - logs error,
  - throws fatal runtime error.
- On success:
  - records canonical runtime path `scene_composite_present_graph`,
  - writes renderer backend/present info,
  - ends diagnostics frame.

## 5. RuntimeGpuRenderer Frame Construction

`RuntimeGpuRenderer::render_frame(...)` executes two major steps:

1. `ensure_scene_target(...)`
2. `execute_gpu_frame_graph(...)`

### 5.1 Resource ensuring and resize behavior

`ensure_scene_target(...)`:

- Clamps render dimensions to renderer max texture size.
- Updates scene composite spec to current output dimensions.
- Uses albedo format from `GpuRenderDevice` policy.
- Ensures authoritative resources and shared sampler every frame (idempotent).

### 5.2 Scene packet build (CPU side)

`build_gpu_scene_frame_data(...)` assembles draw packets:

- Pulls visible assets from `Assets`:
  - dev mode: filtered active assets,
  - runtime: active assets.
- Pulls camera from `Assets::getView()`.
- Builds map floor tile packets from active chunks/world grid.
- For each visible asset:
  - builds `RenderObject` via `render_build::build_direct_asset_render_object`,
  - projects through `render_projection::build_render_object_projected_frame`,
  - computes UVs (including atlas rect + flip handling),
  - emits `GpuSpriteDrawPacket` with clip-space triangle vertices, color modulation, sort key, stable id,
  - classifies packet into floor or layer list.
- Stable-sorts floor and layer sprite packets by sort key then stable id.
- Computes count fields and `has_valid_composite_source`.

Safety guard:

- If visible assets exist but no drawable packets and no floor packets, frame data build fails.

## 6. Authoritative Pass Graph (What Actually Renders)

`GpuRuntimePipeline::enqueue_frame_graph(...)` currently enqueues six render passes in this order:

1. `render_floor_base`
   - target: `scene.floor`
   - pipeline: `floor_compose`
   - clear to opaque black
2. `render_floor_tiles`
   - target: `scene.floor` (LOAD)
   - pipeline: `sprite_batched`
   - custom sprite draw callback for map floor tile packets
3. `render_floor_sprites`
   - target: `scene.floor` (LOAD)
   - pipeline: `sprite_batched`
   - custom sprite draw callback for floor-tagged asset sprites
4. `render_layers`
   - target: `scene.layers`
   - pipeline: `sprite_batched`
   - clear transparent
   - custom sprite draw callback for non-floor layer sprites
5. `render_scene_composite`
   - target: `scene.composite`
   - pipeline: `final_compose`
   - samples `scene.floor` and `scene.layers` with `linear_clamp`
6. `present_scene_composite`
   - target: swapchain
   - pipeline: `sprite_textured`
   - samples `scene.composite` and presents

No copy/blit swapchain presentation path is allowed for the runtime graph.
Presentation is an explicit final render pass.

## 7. GpuFrameGraph Validation and Execution Rules

`GpuFrameGraph::execute(...)` enforces strict validation:

- Named passes required.
- Render passes must have valid targets.
- Swapchain pass must:
  - have sampled source,
  - use `load_op=CLEAR`,
  - use `store_op=STORE`,
  - declare write dependency on `scene.swapchain`.
- Read-before-write dependency violations fail under strict options.
- Same-pass read/write of sampled texture is rejected.
- Exactly one swapchain render pass is required.
- Swapchain pass must be the last pass.

Runtime executes with strict options enabled (`fail_on_validation_error`, `fail_on_missing_resource`, `fail_on_missing_pipeline`).
Validation or execution failure aborts frame submit.

## 8. GPU Device and Submission Layer

`GpuRenderDevice` handles low-level frame state:

- Acquires command buffer (with retries).
- Acquires swapchain texture (`SDL_WaitAndAcquireGPUSwapchainTexture`).
- Exposes per-frame command buffer + swapchain handles.
- Submits or cancels command buffers on end frame.

Present mode behavior:

- Runtime probes window support for vsync/mailbox/immediate.
- Env `VIBBLE_GPU_PRESENT_MODE` can request mode (`auto`, `mailbox`, `immediate`).
- Falls back to vsync when request cannot be applied.

## 9. Pipeline Cache Behavior

`ShaderPipelineCache` key fields:

- shader id
- backend variant (`dxil`/`spirv`)
- color/depth format
- sample count
- render state key

Runtime usage details:

- Pipelines are warmed on load for required variants.
- `GpuSceneRenderer` uses cache get-or-create during pass execution.
- Per-frame hit/miss deltas are recorded into diagnostics.

## 10. SDL Texture to GPU Texture Bridge

Runtime sprite passes consume `SDL_Texture*` from assets/chunks.
For each draw packet, runtime resolves `SDL_GPUTexture*` via
`SDL_PROP_TEXTURE_GPU_TEXTURE_POINTER`.

Important current constraints:

- If texture does not expose GPU bridge pointer, resolve fails.
- Readback fallback is intentionally disabled.
- This makes texture provenance strict: runtime expects GPU-backed SDL textures.

## 11. Diagnostics and Telemetry

`render_diagnostics` records:

- pass counts, draw calls, render target switches,
- texture and GPU buffer create/destroy counts,
- frame CPU timings,
- present pacing (block + interval),
- renderer backend/present mode,
- texture memory (when backend exposes counters),
- GPU pipeline cache hit/miss/hit-rate,
- GPU scene packet stats,
- SDL renderer call counters.

Key invariant checked by tests:

- Runtime GPU graph path should execute with zero SDL render target/draw calls for scene rendering passes (`sdl_renderer_target_call_count == 0`, `sdl_renderer_draw_call_count == 0`).

## 12. Runtime Safety / Startup Throttling Interactions

`AssetsManager` has startup safety controls (frame skipping/batching) that can throttle update or render cadence during early frames via environment-driven knobs.
This can reduce how often `scene->render()` is called temporarily, but does not change the renderer architecture or pass graph design.

## 13. Environment Controls Used by Rendering Path

Known environment knobs directly used in current rendering chain:

- `VIBBLE_GPU_SHADER_MANIFEST` (manifest override)
- `VIBBLE_GPU_PRESENT_MODE` (`auto`/`mailbox`/`immediate`)

Related startup safety knobs in `AssetsManager` can indirectly affect render cadence (skip/interval behavior), but not pass topology.

## 14. Test and Preflight Coverage

### 14.1 Runtime tests

Current tests exercise:

- manifest default/override selection,
- pipeline cache behavior,
- full runtime pass topology smoke,
- no SDL renderer draw/target calls in GPU path,
- floor packet generation and classification,
- composite validity when one source pass is empty,
- rejection of non-bridged textures.

### 14.2 Preflight script

`ENGINE/tools/rendering/runtime_gpu_preflight.py` checks:

- shader manifest presence and required variants,
- required payload files,
- SDL package presence,
- toolchain availability (`cmake`, optional `dxc`, optional `glslc`),
- absence of forbidden legacy bridge tokens in `render.cpp`.

## 15. What Is Not In The Active Runtime Graph

As of current code:

- No CPU/legacy fallback renderer path.
- No swapchain copy/blit present pass.
- No runtime compute pass currently enqueued by `GpuRuntimePipeline`.
- No readback fallback when SDL texture GPU bridge pointer is missing.

## 16. Practical Mental Model

The runtime renderer is an explicit, strict GPU frame graph:

- Build draw packets from current world/camera state.
- Resolve packets to GPU textures.
- Render floor and layer targets.
- Compose to scene composite.
- Present composite through a mandatory final swapchain render pass.
- Fail fast on capability, shader, dependency, resource, or pipeline violations.
