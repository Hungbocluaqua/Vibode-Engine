# AGENTS.md — Vulkan Real-Time Path Tracing Engine

This repository is a Vulkan real-time path tracing engine with editor tooling, RenderGraph, GPU profiling, ReSTIR, denoising, TSR/TAA, and AI-friendly debug/profiling tools.

## Required Context

Before modifying renderer startup, CLI parsing, RenderGraph, profiler, debug views, validation scenes, RenderDoc capture, or diagnostic output code, read:

- `docs/AI_DEBUG_PROFILING_TOOLS_USAGE.md`

That document is the canonical reference for the engine's AI debug/profiling workflow.

## Debug Level Asset Rule

The repository contains two Sponza scene variants:

- Heavy/full Sponza:
  - `native/vulkan/main_sponza`
- Lightweight/debug Sponza:
  - `native/vulkan/Sponza/glTF`

Guidelines:

1. Prefer the lightweight Sponza glTF scene for:
   - rapid iteration
   - renderer debugging
   - validation runs
   - smoke tests
   - CI/regression checks
   - shader debugging
   - RenderGraph validation
   - debug view generation

2. Prefer the heavy/full Sponza scene for:
   - performance profiling
   - GPU stress testing
   - memory pressure testing
   - async compute validation
   - ReSTIR validation
   - denoiser stability testing
   - large-scene benchmarking

3. When creating `.rtlevel` validation/debug levels:
   - generate them from the lightweight glTF Sponza by default
   - place generated debug levels under:
     - `scenes/validation/`
     - or another existing validation/debug scene directory

4. Do not modify the original Sponza assets unless explicitly requested.

## Core Rule

Do not remove or break these tools:

- headless mode
- profile JSON export
- render graph JSON/DOT dump
- debug view export
- debug package export
- validation suite
- deterministic fixed-seed runs
- RenderDoc capture support
- editor mode

## Required Smoke Test

After renderer, RenderGraph, profiler, CLI, or debug-view changes, this command should still work:

```powershell
rtvulkan.exe --headless --scene scenes/validation/cornell.rtlevel ^
  --warmup-frames 30 --frames 120 --fixed-seed 1 ^
  --profile --profile-json out/profile.json ^
  --dump-rendergraph out/rendergraph.json ^
  --save-debug-views out/debug_views ^
  --make-debug-package out/debug_package
```

Expected outputs:

- `out/profile.json`
- `out/rendergraph.json`
- `out/debug_views/`
- `out/debug_package/`

Keep `scenes/validation/cornell.rtlevel` as the default smoke scene because it is fast, deterministic, and exercises the required diagnostic outputs. Do not replace it with `closeup_cornell.rtlevel` as the canonical smoke unless explicitly requested.

## Targeted Validation Scenes

Use these scenes in addition to the required smoke when the task touches renderer behavior, performance, materials, ReSTIR, denoising, or scene loading:

- `scenes/validation/closeup_cornell.rtlevel`
  - close-camera path tracing stress/regression scene
  - use for camera-proximity performance regressions, ReSTIR GI debug/final checks, and path-trace cost investigations
  - compare against `scenes/validation/cornell.rtlevel` when diagnosing performance that changes with camera distance
- `Sponza/glTF/Sponza.gltf`
  - lightweight Sponza glTF scene for broader scene-loading, material, texture, RenderGraph, debug-view, and validation coverage
  - prefer this before heavy Sponza for iteration and CI-style checks
- `main_sponza/NewSponza_Main_glTF_003.gltf` and related `main_sponza` assets
  - heavy/full Sponza stress scenes
  - reserve for performance profiling, memory pressure, async compute validation, denoiser stability, and large-scene benchmarking

## Implementation Rules

1. Keep editor mode working.
2. Keep headless mode working.
3. Do not require RenderDoc for normal execution.
4. RenderDoc capture failure must be a warning, not a fatal error.
5. `--fixed-seed` must keep output deterministic where practical.
6. Warmup frames must not be included in final profile averages.
7. Prefer machine-readable JSON diagnostics over UI-only debugging.
8. Do not introduce Vulkan validation errors.
9. Avoid `vkDeviceWaitIdle` except shutdown or unavoidable full teardown.
10. Add or update debug/profiler instrumentation before changing renderer behavior.

Before optimizing renderer code, run or preserve:

- profile comparison
- image diff
- baseline regression check
- frame timeline dump
- memory/resource lifetime report

## Current Renderer Priority

Priority order:

1. Maintain AI debug/profiling tools.
2. Continue the current implementation plan only with profiler/debug validation.
3. Finish async compute only with profiler lanes and validation.
4. Continue ReSTIR DI/GI only with debug views and JSON reports.
5. Avoid large architecture changes without regression outputs.

## Current Defaults And Diagnostics

- `Balanced` is the default interactive/game preset.
- Real-time presets keep effective path tracing at `1 SPP` through the SPP limiter by default. Higher requested SPP is opt-in through the editor or `--spp` plus `--spp-limit off`.
- ReSTIR DI and ReSTIR GI are enabled by default in the beauty path. ReSTIR GI spatial/final passes are expected in the normal Balanced render graph unless explicitly disabled by settings.
- Adaptive quality should preserve denoiser and ReSTIR spatial stability during motion; avoid optimization paths that skip these passes solely because the camera is moving.
- Profile JSON should continue to report `samples_per_pixel`, `limit_samples_per_pixel`, `effective_samples_per_pixel`, frame avg/min/max/p95/p99, `per_pass_gpu_ms`, `per_pass_gpu_ms_p95`, `per_pass_gpu_ms_p99`, `adaptive_quality`, and the `moment_update` pass when active.
- Use `budgets/balanced_game_16ms.json` as the default Balanced real-time gate. It includes frame-time, memory, per-pass tail-latency, and motion-stability limits.

## Code Review Checklist

Before finishing a renderer task, check:

- Debug build works.
- Release build works.
- Vulkan validation is clean.
- Headless mode still runs.
- Editor mode still opens.
- `profile.json` is valid JSON.
- `rendergraph.json` is valid JSON.
- Debug package contains the expected files.
- No new unconditional `vkDeviceWaitIdle` was added.
- New GPU resources have useful debug names.
