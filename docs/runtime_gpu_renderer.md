# SDL3 Runtime GPU Renderer

## Modes
- Runtime gameplay renderer is GPU-only.
- Strict-startup: missing GPU capabilities, unsupported required formats, invalid shader packages, or invalid GPU pass graph startup probe fail at startup with a hard error.
- Runtime uses one authoritative SDL_GPU frame graph path. SDL texture bridge interop is not part of runtime presentation.

## Startup Policy
- Format probing uses SDL3 GPU capability checks at startup only:
  - `SDL_GPUTextureSupportsFormat`
  - `SDL_GPUTextureSupportsSampleCount`
- Fallback tables are immutable at startup:
  - Albedo/color: `R8G8B8A8_UNORM_SRGB -> R8G8B8A8_UNORM`
  - Light accumulation: `R16G16B16A16_FLOAT -> R11G11B10_UFLOAT`
  - Masks: `R8_UNORM`
  - Depth: `D24_UNORM_S8_UINT -> D32_FLOAT` (if needed)

## Shader Packaging
- Runtime shader manifest: [runtime_shaders.json](/C:/Users/cal_m/OneDrive/Documents/GitHub/ENGINE-2.0/ENGINE/runtime/rendering/shaders/runtime_shaders.json)
- Packaging script: [build_runtime_shader_packages.py](/C:/Users/cal_m/OneDrive/Documents/GitHub/ENGINE-2.0/ENGINE/tools/shaders/build_runtime_shader_packages.py)
- Shader sources:
  - HLSL: `ENGINE/runtime/rendering/shaders/src/hlsl`
  - GLSL: `ENGINE/runtime/rendering/shaders/src/glsl`
- Required variants:
  - `fullscreen_vertex`
  - `sprite_textured`
  - `sprite_batched`
  - `light_eval`
  - `floor_compose`
  - `dark_mask`
  - `final_compose`
  - `compute_light_binning`
- Manifest validation is strict in GPU mode:
  - JSON parse/version checks
  - required variant/backend presence checks
  - file existence/size checks
  - hash checks (`fnv1a64`)
  - binary magic checks (`DXBC` for DXIL containers, SPIR-V magic for `.spv`)

## Pipeline Cache
- Pipeline cache key includes:
  - shader id
  - backend variant (`dxil`/`spirv`)
  - color/depth format
  - sample count
  - render-state key
- Required graphics + compute pipelines are warmed up at startup in GPU mode.
- Cache hit rate is logged after frame execution.

## Diagnostics Fields
- Runtime logs include:
  - `frame_cpu_ms`
  - `render_thread_cpu_ms`
  - `draw_submission_ms`
  - `pass_count`, `copy_pass_count`, `compute_pass_count`
  - `draw_calls`
  - `rt_switches`
  - `tex_create`, `tex_destroy`
  - `cpu_light_gather_ms`, `cpu_light_mask_ms`
  - renderer path/backend/present mode
  - texture memory usage when backend reports it (`texture_mem_mb`)
  - SDL renderer call counters (`sdl_renderer_target_call_count`, `sdl_renderer_draw_call_count`) for GPU-path regression detection

## Preflight
- Non-strict:
  - `cmake --build <build_dir> --target runtime_gpu_preflight`
- Strict toolchain (requires shader compiler binaries):
  - `cmake --build <build_dir> --target runtime_gpu_preflight_strict`
- Rebuild shader packages from sources (fail if compilers are missing):
  - `python ENGINE/tools/shaders/build_runtime_shader_packages.py --input ENGINE/runtime/rendering/shaders --output <out_dir> --require-compile`
