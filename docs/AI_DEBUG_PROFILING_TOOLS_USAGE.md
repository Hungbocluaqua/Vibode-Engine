# AI Debug & Profiling Tools — Usage Guide

## Overview

Current interactive defaults are tuned for a real-time path-tracing workflow: the `Balanced` render preset is the default, ReSTIR DI and ReSTIR GI are enabled for beauty rendering, opacity micromaps and SER are requested by default on capable hardware, effective SPP is capped at `1` by the SPP limiter, and stability comes from ReSTIR reuse plus temporal denoising/TAA rather than raising per-frame samples. Higher SPP is available through `--spp <N> --spp-limit off` or the editor slider for stills, screenshots, reference captures, and high-end budgets.

The AI Debug & Profiling Tools add a headless diagnostic CLI and structured output layer to the ray tracing engine. AI agents, CI systems, and automated debuggers can run the engine, collect profiling data, export debug views, and produce self-contained bug reports — all without the editor UI.

---

## Quick Start

```bat
rtvulkan.exe --headless ^
  --scene scenes/validation/cornell.rtlevel ^
  --warmup-frames 30 ^
  --frames 120 ^
  --fixed-seed 1 ^
  --profile ^
  --profile-json out/profile.json ^
  --dump-rendergraph out/rendergraph.json ^
  --save-debug-views out/debug_views ^
  --make-debug-package out/debug_package
```

This single command renders 120 frames (30 warmup + 90 profiled), exports all profiling data and debug view images, and assembles a complete diagnostic package in `out/debug_package/`.

## Scene Selection

Use `scenes/validation/cornell.rtlevel` as the default diagnostic smoke scene. It is intentionally fast and stable, and it should remain the canonical scene for verifying profile JSON, render graph dumps, debug views, and debug packages.

Use `scenes/validation/closeup_cornell.rtlevel` as the close-camera performance regression scene. It should be run when investigating path tracing cost, camera-proximity slowdowns, ReSTIR GI debug/final output, or changes that may alter ray depth, hit distance, or in-box traversal behavior.

```bat
rtvulkan.exe --headless ^
  --scene scenes/validation/closeup_cornell.rtlevel ^
  --warmup-frames 30 ^
  --frames 120 ^
  --fixed-seed 1 ^
  --debug-view restir-gi-final ^
  --profile ^
  --profile-json out/closeup_profile.json
```

Use the lightweight Sponza glTF scene for broader scene-loading, material, texture, RenderGraph, and debug-view validation without the cost of the heavy Sponza assets:

```bat
rtvulkan.exe --headless ^
  --gltf Sponza/glTF/Sponza.gltf ^
  --warmup-frames 30 ^
  --frames 120 ^
  --fixed-seed 1 ^
  --profile ^
  --profile-json out/sponza_gltf_profile.json ^
  --save-debug-views out/sponza_gltf_debug_views
```

Use the Khronos `AttenuationTest` glTF sample copied under `scenes/validation/gltf_sample_assets/AttenuationTest/glTF/` for Phase 81 transmissive caustic-visibility probes:

```bat
rtvulkan.exe --headless ^
  --gltf scenes/validation/gltf_sample_assets/AttenuationTest/glTF/AttenuationTest.gltf ^
  --warmup-frames 4 ^
  --frames 12 ^
  --fixed-seed 1 ^
  --caustics on ^
  --profile --profile-json out/phase81_attenuation/profile.json ^
  --save-debug-views out/phase81_attenuation/debug_views
```

Reserve the heavy/full Sponza assets under `main_sponza/` for performance profiling, GPU stress testing, memory pressure, async compute validation, denoiser stability, and large-scene benchmarking.

For alpha-tested foliage shadow-ray coverage in the wavefront shadow probe, use the built-in lightweight Sponza diagnostic camera with a low preset:

```bat
rtvulkan.exe --headless ^
  --gltf Sponza/glTF/Sponza.gltf ^
  --camera sponza-foliage ^
  --render-preset low ^
  --restir classic --restir-gi off ^
  --wavefront-shadow-trace on ^
  --profile --profile-json out/sponza_foliage_wavefront/profile.json
```

The default Sponza glTF camera remains useful for broad startup/material validation, but it may not intersect the alpha-tested foliage cluster or queue shadow rays.

---

## CLI Flags Reference

### New Diagnostic Flags

| Flag | Type | Default | Description |
|------|------|---------|-------------|
| `--headless` | bool | false | Run without window, swapchain, or ImGui. Requires `--scene` or `--gltf`. |
| `--warmup-frames <N>` | uint32 | 0 | Frames to render before profiling starts. Excluded from min/avg/max computation. |
| `--fixed-seed <N>` | uint32 | — | Deterministic random seed for reproducible output across runs and machines. |
| `--profile` | bool | false | Enable profiling mode. Collects per-frame CPU and GPU timings. |
| `--profile-json <path>` | string | — | Write JSON profile report after the run completes. |
| `--dump-rendergraph <path>` | string | — | Write render graph JSON after the first frame. |
| `--dump-rendergraph-dot <path>` | string | — | Write render graph as Graphviz DOT file. |
| `--save-debug-views <dir>` | string | — | Export debug view PNG images to directory. |
| `--save-frame-sequence <dir>` | string | — | Export selected debug views as per-frame PNG sequences after warmup. Requires `--headless`. |
| `--sequence-views <csv>` | string | `beauty` | Comma-separated debug views for sequence export or sequence comparison, such as `beauty,motion-vectors,reprojection-confidence`. |
| `--sequence-start-frame <N>` | uint32 | `0` | Skip this many post-warmup frames before exporting a sequence. |
| `--sequence-frame-count <N>` | uint32 | profiled frame count | Number of frames to export per selected view. |
| `--sequence-step <N>` | uint32 | `1` | Export every Nth post-warmup frame. |
| `--capture-renderdoc <path>` | string | — | Output `.rdc` RenderDoc capture path. Requires `RENDERDOC_SDK_DIR` at build time. |
| `--capture-frame <N>` | uint32 | 60 | Frame number to trigger RenderDoc capture. |
| `--renderdoc-dll <path>` | string | — | Optional runtime path to `renderdoc.dll`; otherwise the app tries `RENDERDOC_DLL_PATH`, `RENDERDOC_DIR`, `RENDERDOC_SDK_DIR`, the current directory, PATH, and standard RenderDoc install directories. |
| `--make-debug-package <dir>` | string | — | Assemble complete diagnostic package directory. |
| `--disable-async-compute` | bool | false | Force single-queue execution. |
| `--single-queue-fallback` | bool | false | Use graphics queue for all work. |
| `--disable-resource-aliasing` | bool | false | Disable transient resource aliasing. |
| `--run-validation-suite` | bool | false | Run all validation scenes and output pass/fail results. |
| `--validation-output <dir>` | string | — | Output directory for validation suite results. |
| `--compare-profile <old> <new>` | paths | — | Offline profile JSON comparison for CPU/GPU timings, per-pass timings, memory, and regression percentages. |
| `--compare-image <baseline> <current> --out <diff>` | paths | — | Offline PNG comparison. Prints MSE, PSNR, SSIM, max error, and changed pixel percentage; optionally writes an amplified diff PNG. |
| `--compare-image-sequence <baseline_dir> <current_dir> --out <dir>` | paths | — | Offline sequence comparison. Writes per-frame diffs, contact sheets, and `motion_stability_report.json`. |
| `--update-baseline` | bool | false | Writes profile, render graph, and beauty image baselines for the current scene. |
| `--check-baseline` | bool | false | Compares current profile, render graph, and beauty image against known-good baselines. |
| `--baseline-dir <dir>` | string | `baselines` | Root directory for `--update-baseline` and `--check-baseline`. |
| `--dump-memory <path>` | string | — | Writes texture, buffer, acceleration structure, temporal, ReSTIR, persistent, transient, and total memory JSON. |
| `--dump-frame-timeline <path>` | string | — | Writes CPU frame summary, GPU passes, queue submits, barriers, semaphore list, and presentation state JSON. |
| `--dump-resource-lifetimes <path>` | string | — | Writes per-resource create/read/write/destroy/alias lifetime JSON derived from the RenderGraph. |
| `--dump-shader-report <path>` | string | — | Writes shader source, SPIR-V path, hash, SPIR-V size, defines, entry point, and pipeline name JSON. |
| `--dump-bindings <path>` | string | — | Writes pass resource inputs/outputs/formats/extents and known descriptor binding names JSON. |
| `--crash-dump-package <dir>` | string | — | Writes scene, settings, log, validation messages, render graph, last profile, debug views, and last frame output. |
| `--disable-pass <name>` | repeatable string | — | Disables supported passes from CLI (`Denoiser`, `TAA`, `ReSTIR`, `ReSTIRDI`, `ReSTIRGI`, `AutoExposure`). |
| `--frame-index <N>` | uint32 | — | Uses `N` as the deterministic fixed seed when `--fixed-seed` is not supplied. |
| `--camera <name>` | string | — | Selects a scene camera by entity name or a built-in diagnostic camera such as `sponza-foliage` for lightweight Sponza alpha-tested foliage validation. |
| `--render-preset <name>` | string | `balanced` | Selects a tuned quality/performance preset: `low`, `balanced`, `ultra`, or `custom`. |
| `--restir-gi <on/off>` | bool | `on` | Overrides ReSTIR GI reservoir/final pass usage. |
| `--opacity-micromaps` / `--omm <on/off>` | bool | `on` on capable hardware | Enables hardware opacity micromap BLAS integration when the device supports `VK_EXT_opacity_micromap`; unsupported devices fall back to the alpha any-hit path and report the reason in profile JSON. Use `--omm off` for explicit A/B fallback profiling. |
| `--omm-subdivision` / `--opacity-micromap-subdivision <N>` | uint32 | `2` | Overrides CPU OMM alpha preprocessing subdivision for diagnostic foliage profiling. Values above `5` are clamped to bound CPU preprocessing memory; the selected level is reported in profile JSON. |
| `--wavefront-queues <on/off>` | bool | `off` | Allocates the Phase 61 wavefront queue buffers and runs the queue-header clear validation pass while keeping the megakernel renderer as the default reference path. Reports `wavefront_queues` in profile JSON. |
| `--wavefront-primary-generate` / `--wavefront-generate <on/off>` | bool | `off` | Enables the Phase 62 opt-in compute primary-ray generation pass. This implies wavefront queue allocation, initializes per-pixel state, writes primary rays into the ray queue, and reports primary generation count/parity validation in `wavefront_queues`. The megakernel renderer remains the default output path. |
| `--wavefront-trace` / `--wavefront-trace-wrapper <on/off>` | bool | `off` | Enables the Phase 63 opt-in ray trace wrapper. This implies primary generation and queue allocation, traces generated primary rays into the hit queue, keeps the megakernel as the rendered reference output, and reports hit/depth/normal parity validation in `wavefront_queues`. |
| `--wavefront-shade` / `--wavefront-shade-compute <on/off>` | bool | `off` | Enables the Phase 64 opt-in shade compute probe. This implies wavefront trace, decodes/material-shades first-hit records, enqueues first shadow and secondary rays where supported, keeps the megakernel as the rendered reference output, and reports shade queue counters in `wavefront_queues`. Use `--debug-view wavefront-direct-lighting` when you need a shadow-resolved first-bounce direct-light image from the wavefront pixel-state path. |
| `--wavefront-shadow-trace` / `--wavefront-shadow <on/off>` | bool | `off` | Enables the Phase 65 opt-in shadow trace probe. This implies wavefront shade, traces queued shadow rays through the shadow ray tracing path, applies visible direct-light contribution into pixel state using split light-category accumulation, keeps the megakernel as the rendered reference output, and reports shadow visibility, occupancy, cost, and direct-light parity diagnostics in `wavefront_queues` plus `per_pass_gpu_ms.wavefront_shadow_trace`. |
| `--wavefront-compact` / `--wavefront-queue-compact <on/off>` | bool | `off` | Enables the Phase 66 opt-in queue compaction probe. This implies wavefront shade, atomically appends live secondary rays into a separate compacted queue, keeps the megakernel as the rendered reference output, and reports compact input/live/output counts, invalid/drop/overflow/mapping counters, occupancy, survival ratio, and `per_pass_gpu_ms.wavefront_compact`. When sort is disabled, Phase 71 also runs the direct unsorted secondary trace/shade baseline and reports `wavefront_queues.secondary_shade_*` plus `per_pass_gpu_ms.wavefront_secondary_shade`. |
| `--wavefront-sort` / `--wavefront-ray-sort <on/off>` | bool | `off` | Enables the Phase 71 opt-in secondary-ray bucket sort probe. This implies wavefront compaction, sorts the compacted secondary-ray queue into a separate sorted queue by event/material bucket, keeps the megakernel as the rendered reference output, and reports sort input/output/verified counts, active bucket count, ordering/invalid/overflow counters, and `per_pass_gpu_ms.wavefront_sort`. |
| `--wavefront-final-output` / `--wavefront-renderer <on/off>` | bool | `off` | Enables the opt-in wavefront-owned final-output path. This implies queues, primary generation, trace, shade, compaction, and shadow trace; skips `path_trace_rt`; iterates compacted secondary trace/shade up to the active bounce depth; resolves queued shadow rays once; writes wavefront pixel-state radiance to HDR; and reports `settings.wavefront_final_output_enabled` plus `wavefront_queues.final_output_enabled`. |
| `--ser` / `--shader-execution-reordering <on/off>` | bool | `on` on capable hardware | Enables the Phase 76 SER raygen variant for wavefront trace probes on devices that support `VK_NV_ray_tracing_invocation_reorder`. Unsupported devices clamp the setting off. Combine with `--wavefront-validation`, `--wavefront-trace`, or `--wavefront-sort`; the default megakernel path remains unchanged unless a wavefront trace path is enabled. Use `--ser off` for explicit A/B fallback profiling. |
| `--dof-aperture-radius <value>` | float | `0` | Enables Phase 78 thin-lens depth of field when greater than zero. The value is in scene units and affects both megakernel and wavefront primary ray generation. |
| `--dof-focus-distance` / `--focus-distance <value>` | float | `10` | Sets the camera-space distance to the sharp focus plane for Phase 78 depth of field. |
| `--dof-blades` / `--dof-blade-count <N>` | uint32 | `0` | Sets polygonal aperture blades for bokeh. `0` keeps a circular aperture; nonzero values clamp to `3..16`. |
| `--dof-bokeh-rotation` / `--bokeh-rotation <radians>` | float | `0` | Rotates the polygonal aperture shape used by Phase 78 bokeh sampling. |
| `--validation-object-motion` | flag | off | Moves the first mesh entity deterministically through the `TransformOnly` scene update path during headless validation. Use with `scenes/validation/motion_blur_instances.rtlevel` to prove Phase 79 moving/static TLAS motion instances. |
| `--motion-blur` / `--rt-motion-blur <on/off>` | bool | `off` | Enables Phase 79 ray traced motion blur when `VK_NV_ray_tracing_motion_blur` is supported. Unsupported devices clamp the setting off and report the reason in profile JSON. |
| `--motion-blur-shutter-open <value>` | float | `0` | Sets the normalized shutter-open ray time used by the motion raygen variant. Values clamp to `0..1`. |
| `--motion-blur-shutter-close <value>` | float | `1` | Sets the normalized shutter-close ray time used by the motion raygen variant. Values clamp to `0..1` and are ordered against shutter open. |
| `--homogeneous-volume` / `--volume <on/off>` | bool | `off` | Enables Phase 80 path-traced global homogeneous medium sampling in the megakernel path. |
| `--volume-scattering` / `--homogeneous-volume-scattering <value>` | float | `0` | Sets the homogeneous medium scattering coefficient in scene units. Supplying this flag enables the medium unless `--volume off` is also supplied. |
| `--volume-absorption` / `--homogeneous-volume-absorption <value>` | float | `0` | Sets the homogeneous medium absorption coefficient in scene units. |
| `--volume-anisotropy` / `--homogeneous-volume-anisotropy <value>` | float | `0` | Sets the Henyey-Greenstein phase anisotropy, clamped to `-0.95..0.95`. |
| `--mnee-caustics` / `--caustics <on/off>` | bool | `off` | Enables the Phase 81 partial caustic-visibility probe. Direct-light shadow rays may pass through up to two delta transmissive interfaces before testing final visibility; the full MNEE solver remains incomplete. Profile JSON reports `mnee_caustics_enabled`. |
| `--wavefront-validation` | bool | false | Enables the Phase 74 side-by-side validation probe. It keeps the megakernel as the rendered reference output, switches to classic direct lighting with ReSTIR GI off for strict parity, enables wavefront shade/compact/secondary-shade/shadow-trace diagnostics, and writes aggregate `wavefront_validation` pass/fail/timing fields to profile JSON. |
| `--taa-motion-feedback <value>` | float | preset | Overrides moving-camera TAA current-frame blend for scripted stability sweeps. |
| `--taa-reactive-feedback <value>` | float | preset | Overrides moving-camera reactive/disocclusion TAA current-frame blend. |
| `--spp` / `--samples-per-pixel <N>` | uint32 | preset | Overrides requested path samples per pixel per frame. Values are clamped to 1-8. |
| `--spp-limit` / `--limit-spp <on/off>` | bool | preset | Enables or disables the real-time 1 SPP limiter. With the limiter on, effective SPP is forced to 1. |
| `--validate-gpu-labels` | bool | false | Checks RenderGraph passes/resources for useful names. |
| `--check-budget <path>` | string | — | Fails if profile GPU timings, validation errors, or memory exceed a budget JSON. |
| `--descriptor-lifetime-stress <path>` | string | — | Headless-only stress report that repeatedly toggles renderer resource sizes, updates material descriptors when scene assets exist, reloads the `.rtlevel` when available, recreates the path tracer through the shader-reload path, drains retired renderers, and writes descriptor/VMA snapshots. Exits nonzero if descriptor allocation failures increase or retired renderers remain queued. |
| `--descriptor-lifetime-stress-cycles <N>` | uint32 | 12 | Number of descriptor lifetime stress cycles to run. |
| `--descriptor-lifetime-stress-frames <N>` | uint32 | 2 | Frames rendered at each stress step before sampling descriptor and memory state. |
| `--shader-hot-reload-report` | bool | false | Writes a shader report to the current diagnostics artifact directory. |

When profiling is enabled, `gpu_debug_counters.ray_tracing_any_hit` also includes Phase 81 caustic-visibility counters: `caustic_shadow_attempts`, `caustic_transmissive_hits`, `caustic_transmissive_visible`, and `caustic_shadow_blocked`. These counters should remain zero when `--caustics off` and become nonzero on transmissive probes such as `AttenuationTest` when `--caustics on`.

The `caustic-visibility` debug view visualizes the same Phase 81 path per pixel and automatically enables the guarded caustic-visibility probe while selected. Red marks blocked caustic shadow probes, green marks transmissive paths that reached the light/environment, and blue marks transmissive-interface hits. It is included in `--save-debug-views` exports.

### Existing Flags (unchanged)

| Flag | Description |
|------|-------------|
| `--frames <N>` | Total frames to render before exit. |
| `--scene <path>` / `--rtlevel <path>` | Load `.rtlevel` scene JSON. |
| `--gltf <path>` | Load glTF/GLB scene directly. |
| `--hdr <path>` | Load HDR environment map. |
| `--debug-view <name>` | Set initial debug view (e.g. `beauty`, `normals`, `depth`). |
| `--denoiser <on/off>` | Override denoiser on/off. |
| `--restir <mode>` | Override ReSTIR mode: `classic`, `restir`, `hybrid`. |

---

Wavefront queue memory is allocated from per-frame transient arenas when any wavefront queue probe is enabled. Profile JSON reports `wavefront_queues.transient_arena_used_bytes`, `transient_arena_high_water_bytes`, and `transient_arena_capacity_bytes` so queue memory high-water and arena reuse are visible in diagnostics. RenderGraph JSON reports buffer `buffer_offset` and `size_bytes` for buffer resources and barriers, which is required for validating barriers over aliased wavefront arena subranges.

When ReSTIR DI is enabled with the opt-in wavefront shade probe, `wavefront_queues` also reports `shade_restir_reservoir_write_count`, `shade_restir_valid_candidate_count`, `shade_restir_temporal_merge_count`, and `shade_restir_invalid_candidate_count`. These counters prove that shade-stage first-bounce direct-light candidates are being written into the wavefront DI diagnostic reservoir and that previous-frame DI history is being read for temporal merge diagnostics. When ReSTIR GI is enabled with secondary/sorted wavefront shade, `shade_restir_gi_reservoir_write_count`, `shade_restir_gi_valid_candidate_count`, `shade_restir_gi_temporal_merge_count`, and `shade_restir_gi_invalid_candidate_count` report second-bounce GI candidate generation into the separate wavefront GI diagnostic reservoir. The diagnostic DI/GI reservoirs are intentionally separate from the production ReSTIR reservoirs so the default megakernel beauty path remains the reference output until the later wavefront write/debug-view phases are complete.

Phase 70 adds wavefront-owned debug output views: `wavefront-queue-occupancy`, `wavefront-path-depth`, `wavefront-live-rays`, `wavefront-terminated-rays`, `wavefront-material-bucket`, `wavefront-restir-di`, `wavefront-restir-gi`, and `wavefront-direct-lighting`. Selecting one of these views automatically enables the wavefront shade probe and writes a diagnostic visualization from wavefront queue/pixel-state buffers into the HDR debug output while keeping the normal default path unchanged. `wavefront-restir-gi` also enables queue compaction and secondary shade so the GI candidate reservoir is populated. `wavefront-direct-lighting` enables the shadow-trace probe so the image reflects shadow-resolved direct lighting. The editor debug-view combo groups these under `Wavefront Queue`; existing non-wavefront debug views are labeled as reference-path views until wavefront final output owns the corresponding core debug buffers.

The opt-in `--wavefront-final-output on` mode makes the wavefront path own beauty output for validation. It does not alter default rendering. In this mode the runtime graph omits `path_trace_rt`, runs primary trace/shade, repeats compacted secondary trace/shade for the active bounce count, resolves accumulated shadow rays, optionally feeds production ReSTIR GI spatial/final debug passes, and writes `wavefront_final_write` into the raw HDR target. Denoiser, TAA, and height fog are bypassed for this mode until their full wavefront-owned inputs are available.

Phase 65 direct-light parity is scoped to classic/raw NEE first-bounce direct lighting. Use `--restir classic --restir-gi off` for shadow-trace parity acceptance. Opaque scenes require zero direct-light mismatches; alpha-tested scenes reuse the trace-validation edge tolerance because the megakernel and wavefront probes run separate alpha any-hit ray tracing passes. Default ReSTIR direct-light parity is intentionally not a Phase 65 acceptance gate and remains part of Phase 69 wavefront ReSTIR integration.

Phase 71 adds an opt-in `wavefront_sort` compute pass after queue compaction. It reads `wavefront compacted ray queue`, writes `wavefront sorted ray queue`, and reports `wavefront_queues.sort_*` counters plus `per_pass_gpu_ms.wavefront_sort` so sorting overhead and ordering correctness can be tracked. When enabled, the graph also runs `wavefront_sorted_hit_queue_clear`, `wavefront_sorted_trace_rt`, and `wavefront_sorted_shade` after primary trace validation to prove the sorted secondary queue is consumable by the RT trace and shade stages without corrupting primary trace/shade/compact validation. Sorted shade reports `wavefront_queues.sorted_shade_*` counters and `per_pass_gpu_ms.wavefront_sorted_shade`. When compaction is enabled without sorting, the graph runs the direct unsorted secondary trace/shade baseline and reports `wavefront_queues.secondary_shade_*` counters plus `per_pass_gpu_ms.wavefront_secondary_shade`; use this for direct A/B against sorted shade. Profile JSON also reports `secondary_path_cost_ms`, `sorted_path_cost_ms`, `*_path_microseconds_per_ray`, and conservative `sort_net_benefit_*` fields. The net-benefit flag only passes when a profile contains validated direct and sorted measurements over the same ray count; otherwise use paired fixed-seed profiles for the A/B. Current Phase 71 validation keeps the phase open because the direct A/B shows the sorted path is correct but not yet faster on Cornell or material-grid. The default smoke path keeps all wavefront probes disabled; required smoke validation should continue to show no `wavefront_*` graph passes and `wavefront_queues.sort_enabled=false` unless a probe is explicitly requested.

The sort pass writes a GPU dispatch-indirect argument from the compacted ray count and uses it for count/scatter/verify dispatches. Secondary rays carry the material bucket in the path flags written by `wavefront_shade.comp`, so `wavefront_sort.comp` can build event/material sort keys from the ray record without loading per-ray pixel-state material data. The count phase accumulates bucket counts in a workgroup-local histogram before merging into the global bucket counters, reducing global atomic pressure while preserving the same validation counters. `per_pass_gpu_ms.wavefront_sort` ends before the validation-only order scan, while `wavefront_queues.sort_validation_passed` still depends on `sort_verified_ray_count`, `sort_order_violation_count`, invalid-pixel count, and overflow count. Treat the timing as queue-producing sort cost and the validation counters as diagnostic correctness evidence.

`wavefront_shade.comp` uses a 1D queue/pixel dispatch for both primary and secondary shade probes. Primary shade still derives the 2D pixel coordinate from the linear invocation index, while queue-indexed secondary and sorted shade recover the original pixel coordinate from the hit record. The direct unsorted secondary shade baseline uses dispatch-indirect arguments written by compaction so it only launches groups for the compacted secondary-ray count; sorted shade keeps the full dispatch path because the indirect variant measured slower on the current material-grid probe. This keeps RNG/debug validation stable while reducing wasted dispatch shape overhead in secondary shade timing probes.

Phase 72 begins queue bandwidth reduction by removing unused or reconstructible payload fields from wavefront queues. `WavefrontRay` no longer carries unused throughput/MIS fields, `WavefrontHit` no longer carries unused throughput or the stored bitangent basis vector, `WavefrontShadowRay` no longer carries the unused transmittance/distance vec4 because `direction_tmax.w` already stores the trace distance, and `WavefrontPixelState` no longer carries the unused barycentrics/MIS slot. Wavefront shade reconstructs the bitangent from the stored tangent plus shading normal before BRDF evaluation. Shadow-ray visible-light contribution is packed as half precision RGB/PDF plus exact pixel/light metadata, so the ray record is now `48` bytes, the hit record is now `112` bytes, the shadow ray record is now `48` bytes, and the pixel-state record is now `112` bytes. `wavefront_queue_clear.comp` validates those strides at runtime before queue probes are accepted. Profile JSON `wavefront_queues.*_bytes` fields should be used to verify the expected queue memory drop whenever this layout changes.

Phase 73 adds explicit queue-balance diagnostics to `profile.json`. When a wavefront probe is enabled, `wavefront_queues` reports `primary_queue_occupancy`, `trace_hit_queue_occupancy`, `shade_secondary_queue_occupancy`, `shadow_queue_occupancy`, `compact_queue_occupancy`, `sort_output_queue_occupancy`, secondary/sorted shade output queue occupancy, `queue_overflow_count`, `queue_starvation_detected`, and `queue_balance_validation_passed`. Use these fields with the existing per-pass timings and checked-ray counts before changing dispatch sizing or adding persistent/work-stealing kernels. The default megakernel smoke keeps wavefront probes disabled, so `queue_balance_validation_passed` remains false there because no wavefront queue balance was exercised.

The compact pass now dispatches only the secondary-ray suffix appended after the primary ray records. `compact_input_ray_count` still reports the full source ray queue count for validation, while `compact_scanned_ray_count` reports the actually dispatched suffix count and should match `shade_secondary_ray_count` for the current one-bounce probe. `compact_microseconds_per_ray`, `per_pass_gpu_ms.wavefront_compact`, and the per-pass p95/p99 fields are the timing gates for this dispatch-sizing optimization.

SER profile JSON includes conservative closeout fields: `shader_execution_reordering.performance_target_min_percent`, `performance_target_max_percent`, `performance_evidence_available`, `performance_target_passed`, and `observed_improvement_percent`. A single profile cannot prove the Phase 76 20-40 percent target by itself, so these fields remain evidence-unavailable until a comparison workflow supplies a baseline and current run.

When `rayTracingPipelineTraceRaysIndirect` is available, the direct unsorted secondary trace pass uses a queue-sized RT indirect launch from the compacted secondary-ray count. Devices without that feature fall back to the full-frame trace launch and shader-side queue-count early return. `wavefront_queues.trace_rays_indirect_supported` records the device capability, and `wavefront_queues.secondary_trace_indirect_enabled` records whether the retained direct secondary path can use the queue-sized launch. The sorted secondary trace path intentionally remains on the full-frame launch until sorted queue-sized tracing proves stable p95/p99 behavior.

SER capability is reported in profile JSON under `shader_execution_reordering`. The block includes `supported`, `enabled`, `extension_supported`, `invocation_reorder_feature`, `dedicated_ser_pipeline`, `pipeline_create_flag_required`, `reordering_hint`, `max_invocation_reorder_depth_reported`, `max_ray_tracing_invocation_reorder_depth`, and `disabled_reason`. Current Vulkan SDK headers expose the `VK_NV_ray_tracing_invocation_reorder` feature and reordering hint but do not expose a max invocation reorder depth property, so that depth field remains `0` with `max_invocation_reorder_depth_reported=false` until a target SDK/extension reports it. Phase 76 adds a SER wavefront trace raygen variant compiled with `RTV_SER_ENABLED=1`; it calls `reorderThreadNV()` before `traceRayEXT()` using a hint derived from ray type, encoded material bucket, and distance bucket. Profile JSON reports direct wavefront RT timing as `per_pass_gpu_ms.wavefront_trace`, `wavefront_secondary_trace`, and `wavefront_sorted_trace`; use those fields for SER A/B rather than inferring trace cost from shade timings. Phase 77 was revised to remove the obsolete roadmap dependency on a non-public `CAPTURE_REORDER` pipeline flag; SER pipeline creation now uses the standard ray tracing pipeline flags plus the enabled invocation-reorder feature and SER shader variant, so `pipeline_create_flag_required=false`.

Use `--wavefront-validation` for a single-command Phase 74 side-by-side sanity check. It reports `wavefront_validation.all_required_passed`, the individual primary/trace/shade/compact/secondary/shadow/direct-light parity gates, checked pixel/ray counts, direct-light error maxima, and aggregate wavefront probe GPU cost. This mode intentionally forces classic direct lighting and disables ReSTIR GI so first-bounce direct-light parity is strict; use separate ReSTIR DI/GI wavefront debug probes when validating reservoir diagnostics. The aggregate trace gate accepts the lower-level `wavefront_queues.trace_validation_passed` result, so alpha-edge hit/normal differences use the renderer's existing alpha-edge tolerance, or the broad Phase 74 one-pixel parity tolerance; raw mismatch counters remain visible in profile JSON.

Combining `--run-validation-suite --wavefront-validation` runs the validation manifest scenes in the same side-by-side wavefront validation mode and writes per-scene `wavefront_validation` summaries into `summary.json`. The suite keeps the raw strict `wavefront_queues.*_validation_passed` counters in each scene `profile.json`, while the top-level `wavefront_validation` gate allows a one-pixel parity tolerance for broad smoke coverage across validation scenes.

---

## Features

### 1. Headless Mode

Run the engine without any window, ImGui, or GLFW. Pure Vulkan rendering on the GPU with no display output.

```bat
rtvulkan.exe --headless --scene scenes/my_scene.rtlevel --frames 120
```

**How it works:**
- Creates a Vulkan instance and device without requiring a GLFW surface or `VK_KHR_swapchain` extension.
- Allocates 2 offscreen `VkImage` objects (B8G8R8A8_UNORM) as the presentation target.
- Uses a flip counter instead of `vkAcquireNextImageKHR` and skips `vkQueuePresentKHR`.
- Skips `initWindow()`, `UiOverlay` creation, and the GLFW event loop entirely.

**Deterministic output:** Use `--fixed-seed N` to make the ray generation shader use `N` as the base seed instead of the frame counter, and use `N` as the camera jitter Halton sequence index. This makes output reproducible across runs.

**Requirements:** Must be paired with `--scene`/`--rtlevel` or `--gltf`. Without a scene or glTF path, the engine will throw an error.

---

### 2. JSON Profile Report

Export machine-readable profiling data with stable field names for diffing between commits.

```bat
rtvulkan.exe --headless ^
  --scene scenes/cornell.rtlevel ^
  --warmup-frames 30 ^
  --frames 120 ^
  --profile ^
  --profile-json out/profile.json
```

**JSON schema:**

```json
{
  "engine_version": "0.1.0",
  "git_commit": "abc1234",
  "gpu_name": "NVIDIA GeForce RTX 4090",
  "driver_version": "551.86",
  "vulkan_version": "1.3.280",
  "resolution": {
    "render_extent": { "width": 1920, "height": 1080 },
    "display_extent": { "width": 1920, "height": 1080 },
    "render_scale": 1.0
  },
  "frame_count": 120,
  "warmup_frames": 30,
  "profiled_frames": 90,
  "cpu_frame_ms": { "min": 2.1, "avg": 3.4, "max": 8.7, "p95": 4.1, "p99": 7.2 },
  "gpu_frame_ms": { "min": 4.0, "avg": 4.2, "max": 5.1, "p95": 4.6, "p99": 5.0 },
  "per_pass_gpu_ms": {
    "path_trace": 2.1,
    "restir_history_clear": 0.0,
    "restir_gi_clear": 0.0,
    "restir_spatial": 0.3,
    "restir_spatial_copy": 0.02,
    "restir_gi_spatial": 0.0,
    "restir_gi_final": 0.0,
    "fog_integrate": 0.05,
    "atmosphere": 0.1,
    "atmosphere_transmittance": 0.01,
    "atmosphere_multi_scatter": 0.01,
    "atmosphere_sky_view": 0.03,
    "atmosphere_sky_reproject": 0.02,
    "atmosphere_sky_cdf": 0.01,
    "atmosphere_aerial_perspective": 0.02,
    "denoiser": 0.4,
    "history_copy": 0.08,
    "skip_denoiser_copy": 0.0,
    "taa": 0.15,
    "taa_history_copy": 0.03,
    "auto_exposure_histogram_clear": 0.005,
    "auto_exposure_histogram": 0.02,
    "auto_exposure_reduce": 0.01,
    "tone_map": 0.06,
    "selection_outline": 0.0,
    "fullscreen": 0.03,
    "editor_presentation": 0.0
  },
  "per_pass_gpu_ms_p95": {
    "path_trace": 2.4,
    "denoiser": 0.5,
    "moment_update": 0.12,
    "history_copy": 0.11,
    "taa": 0.18,
    "restir_gi_final": 0.08
  },
  "per_pass_gpu_ms_p99": {
    "path_trace": 2.7,
    "denoiser": 0.6,
    "moment_update": 0.14,
    "history_copy": 0.13,
    "taa": 0.21,
    "restir_gi_final": 0.10
  },
  "pipeline_statistics": {
    "ray_invocations": 123456789,
    "triangle_hits": 98765432,
    "aabb_hits": 0
  },
  "ray_tracing_geometry": {
    "opaque_primitive_count": 42,
    "alpha_tested_primitive_count": 6,
    "blended_primitive_count": 2,
    "opaque_triangle_count": 120000,
    "alpha_tested_triangle_count": 18000,
    "blended_triangle_count": 900,
    "mesh_count_with_only_opaque_geometry": 12,
    "mesh_count_with_alpha_tested_geometry": 3,
    "mesh_count_with_blended_geometry": 1
  },
  "opacity_micromap": {
    "supported": true,
    "extension_supported": true,
    "micromap_feature": true,
    "capture_replay": false,
    "host_commands": false,
    "max_opacity_2_state_subdivision_level": 12,
    "max_opacity_4_state_subdivision_level": 12,
    "disabled_reason": "",
    "preprocess": {
      "subdivision_level": 2,
      "eligible_primitive_count": 6,
      "generated_primitive_count": 6,
      "alpha_texture_primitive_count": 6,
      "constant_alpha_primitive_count": 0,
      "cache_entry_count": 6,
      "cache_hit_count": 0,
      "total_triangle_count": 18000,
      "micro_triangle_count": 72000,
      "opaque_count": 42000,
      "transparent_count": 18000,
      "unknown_count": 0,
      "mixed_count": 12000,
      "data_bytes": 72384,
      "preprocessing_ms": 3.2,
      "validation_error_count": 0,
      "warnings": []
    }
  },
  "memory": {
    "textures_bytes": 524288000,
    "buffers_bytes": 134217728,
    "acceleration_structure_bytes": 67108864,
    "temporal_history_bytes": 33554432,
    "restir_reservoir_bytes": 16777216
  },
  "validation_error_count": 0,
  "warnings": [],
  "settings": {
    "path_tracing_enabled": true,
    "denoiser_enabled": true,
    "max_bounces": 8,
    "restir_mode": "classic-nee",
    "tone_mapper": "aces",
    "exposure": 2.0,
    "debug_view": "beauty",
    "specular_aa_enabled": true,
    "opacity_micromaps_enabled": false,
    "render_resolution_scale": 1.0
  }
}
```

**Timing collection:** CPU timings are measured per-frame with `std::chrono`. GPU timings are read from the `GpuProfiler` smoothed exponential moving average. Frame min/avg/max/p95/p99 and per-pass avg/p95/p99 are computed over profiled frames (warmup frames excluded). `per_pass_gpu_ms` remains the average per-pass object for compatibility; `per_pass_gpu_ms_p95` and `per_pass_gpu_ms_p99` expose tail costs for spike budgets.

**Diffing between commits:** Run the same scene with `--fixed-seed 1` on both commits, then diff the JSON files. The stable field names make automated comparison trivial.

---

### 3. Render Graph Dump

Serialize the render graph structure to JSON or Graphviz DOT format.

**JSON:**
```bat
rtvulkan.exe --headless --scene scenes/cornell.rtlevel --frames 1 ^
  --dump-rendergraph out/rendergraph.json
```

**DOT (for visualization):**
```bat
rtvulkan.exe --headless --scene scenes/cornell.rtlevel --frames 1 ^
  --dump-rendergraph-dot out/rendergraph.dot
dot -Tpng out/rendergraph.dot -o rendergraph.png
```

**JSON structure:**

```json
{
  "passes": [
    {
      "name": "path_trace_rt",
      "queue": "raytracing",
      "inputs": ["previous restir reservoir"],
      "outputs": ["raw hdr", "entity ids", "accumulation", "restir reservoir"],
      "resource_formats": { "raw hdr": "R16G16B16A16_SFLOAT" },
      "extents": { "raw hdr": { "width": 1920, "height": 1080 } },
      "barriers": [
        {
          "resource": "raw hdr",
          "before_pass": "<external>",
          "after_pass": "path_trace_rt",
          "before": { "stage": "NONE", "access": "NONE", "layout": "undefined" },
          "after": { "stage": "RAY_TRACING_SHADER", "access": "SHADER_WRITE", "layout": "general" }
        }
      ],
      "gpu_ms": 2.1,
      "skipped": false
    }
  ],
  "resources": [
    {
      "name": "raw hdr",
      "type": "Texture",
      "lifetime": "Persistent",
      "format": 97,
      "extent": { "width": 1920, "height": 1080 }
    }
  ],
  "barriers": [
    {
      "resource": "raw hdr",
      "before_pass": "<external>",
      "after_pass": "path_trace_rt",
      "before": { "stage": "NONE", "access": "NONE", "layout": "undefined" },
      "after": { "stage": "RAY_TRACING_SHADER", "access": "SHADER_WRITE", "layout": "general" }
    }
  ]
}
```

**DOT output:** Produces a Graphviz directed graph with domain-colored nodes (lightblue for RayTracing/Graphics, lightgreen for Compute, lightyellow for Transfer) and edges labeled with the shared resource name.

**Integration:** The dump is triggered from `PathTracerRenderer::recordRenderGraphPlan()` after `graph.compile()` on the first frame, then the path is cleared so it only dumps once.

---

### 4. Debug View Export

Export every `RendererDebugView` enum value as 8-bit PNG images. Each view gets its own warmup cycle for clean output.

```bat
rtvulkan.exe --headless ^
  --scene scenes/cornell.rtlevel ^
  --warmup-frames 4 ^
  --frames 1 ^
  --fixed-seed 1 ^
  --save-debug-views out/debug_views
```

**Exported views:**

| Category | View Name | Description |
|----------|-----------|-------------|
| Geometry | `beauty` | Tone-mapped final output |
| Geometry | `albedo` | Base color |
| Geometry | `material-occlusion` | Material ambient-occlusion texture/strength |
| Geometry | `normals` | World-space normals |
| Geometry | `depth` | Linear depth |
| Geometry | `roughness` | Surface roughness |
| Lighting | `path-direct-diffuse` | Direct diffuse lighting |
| Lighting | `path-direct-specular` | Direct specular lighting |
| Lighting | `path-indirect-diffuse` | Indirect diffuse |
| Lighting | `path-indirect-specular` | Indirect specular |
| Denoiser | `denoiser-kernel-radius` | Denoiser filter radius |
| Denoiser | `denoiser-hit-distance` | Hit distance filter |
| Denoiser | `denoiser-virtual-motion` | Specular virtual motion |
| Denoiser | `denoiser-diffuse-history` | Diffuse history confidence |
| Denoiser | `denoiser-specular-history` | Specular history confidence |
| Temporal | `temporal-history-weight` | Temporal blend weight |
| Temporal | `reprojection-confidence` | Reprojection confidence |
| Temporal | `motion-vectors` | Screen-space velocity |
| ReSTIR | `restir-reservoir-age` | ReSTIR reservoir age |
| ReSTIR | `restir-reservoir-confidence` | ReSTIR confidence |
| ReSTIR | `restir-reservoir-m` | ReSTIR reservoir sample count |
| ReSTIR | `restir-pairwise-mis` | Pairwise MIS weight |
| ReSTIR GI | `restir-gi-validity` | Current/previous/spatial GI reservoir validity |
| ReSTIR GI | `restir-gi-age` | GI reservoir age |
| ReSTIR GI | `restir-gi-initial` | Initial GI candidate contribution |
| ReSTIR GI | `restir-gi-temporal` | Temporal GI reservoir contribution |
| ReSTIR GI | `restir-gi-spatial` | Spatial GI reservoir contribution |
| ReSTIR GI | `restir-gi-final` | Final selected-bounce GI replacement preview |
| ReSTIR GI | `restir-gi-normal` | Decoded compressed GI reservoir normal |
| ReSTIR GI | `restir-gi-hit-distance` | Decoded compressed GI hit distance |
| MIS | `sun-mis-weight` | Sun MIS weight |
| MIS | `ris-effective-light-pdf` | RIS effective PDF |
| Other | `path-data-metrics` | Path hit metrics |
| Other | `variance` | Variance estimate |

When alpha-tested geometry is present, `--save-debug-views` also writes a CPU-side opacity micromap atlas to `debug_views/opacity_micromap/opacity_micromap_microtriangles.png`. White micro-triangles are opaque, black are transparent, yellow are mixed, and blue are unknown/not CPU-readable.

**ReSTIR GI tuning fields in profile/settings JSON:**

| Field | Default | Description |
|-------|---------|-------------|
| `restir_gi_temporal_max_age` | `24` | Maximum temporal GI reservoir age before rejection |
| `restir_gi_spatial_rounds` | `4` | Spatial GI neighbor rounds |
| `restir_gi_spatial_radius` | `4.25` | Blue-noise spatial GI search radius |
| `restir_gi_depth_threshold_scale` | `1.0` | Depth rejection threshold scale for spatial GI reuse |
| `restir_gi_spatial_compatibility_threshold` | `0.05` | Minimum compatibility score for accepting a spatial GI neighbor |
| `restir_gi_half_resolution` | `false` | Reuses one spatial GI reservoir per 2x2 pixel group in GI debug/final paths |
| `restir_gi_visibility_ray_budget` | `0` | Reserved for future ray-query visibility validation; current GI spatial reuse remains conservative |

**TAA motion stability fields in profile/settings JSON:**

| Field | Balanced Default | Description |
|-------|------------------|-------------|
| `taa_feedback` | `0.06` | Base current-frame blend. Lower values keep more validated history. |
| `taa_motion_feedback` | `0.90` | Current-frame blend target while the camera is moving. Lower values stabilize noisy motion; higher values react faster. |
| `taa_reactive_feedback` | `0.98` | Current-frame blend for strong reactive/disocclusion cases while moving. |
| `taa_sharpening_strength` | `0.05` | TAA resolve sharpening. Automatically damped while moving or reactive. |

**SPP fields in profile/settings JSON:**

Real-time path tracing should normally run at an effective 1 spp and spend the remaining budget on ReSTIR reuse, temporal denoising, TAA, and stable frame pacing. Use higher SPP for still captures, reference comparisons, or high-end offline-style previews.

| Field | Balanced Default | Description |
|-------|------------------|-------------|
| `samples_per_pixel` | `1` | Requested path samples per pixel per frame. |
| `limit_samples_per_pixel` | `true` | Forces effective SPP to 1 for real-time/game-style rendering. |
| `effective_samples_per_pixel` | `1` | Actual path samples per pixel per frame after the limiter is applied. |

**ReSTIR GI reservoir layout validation:**

By default, ReSTIR GI debug reservoirs use the compressed production layout and profile JSON reports `restir_gi_layout: "compressed"`. Set `RTV_RESTIR_GI_UNCOMPRESSED_LAYOUT=1` before launching `rtvulkan.exe` to compile shaders and allocate buffers with the uncompressed validation layout for A/B comparisons. Use the same scene, frame count, fixed seed, and debug view, then compare the resulting `profile.json` files with `--compare-profile` and `restir-gi-final.png` with `--compare-image`.

**Output structure:**

```
debug_views/
  beauty.png
  albedo.png
  normals.png
  depth.png
  ...
  export_manifest.json
```

**`export_manifest.json`:**

```json
{
  "exported": ["beauty", "albedo", "normals", "depth", ...],
  "missing_debug_views": ["metallic"],
  "resolution": { "width": 1920, "height": 1080 }
}
```

**Diagnosing bugs from debug views:**

| Symptom | View to Check |
|---------|---------------|
| Black image | `depth.png` — camera might be inside geometry |
| Noisy image | `variance.png` — sampling issue |
| Wrong colors | `albedo.png` and `normals.png` — material/geometry issue |
| Denoiser artifacts | `denoiser-kernel-radius.png` and `denoiser-hit-distance.png` |
| Temporal ghosting | `temporal-history-weight.png` and `reprojection-confidence.png` |
| ReSTIR issues | `restir-reservoir-confidence.png` and `restir-pairwise-mis.png` |

---

### 5. RenderDoc Capture

Trigger a RenderDoc `.rdc` capture from the CLI. This is compile-time optional behind `#ifdef RTV_HAS_RENDERDOC` and runtime optional: normal engine execution must not require RenderDoc.

**Build setup:**
```bat
cmake -S . -B build -DRENDERDOC_SDK_DIR="C:/Users/HomePc/renderdoc/renderdoc/api/app"
cmake --build build --config Release
```

`RENDERDOC_SDK_DIR` must point to the directory that contains `renderdoc_app.h`, not to the `.h` file itself. For a standard RenderDoc install this may also be a path such as `C:/Program Files/RenderDoc`.

**Direct runtime usage:**
```bat
build\Release\rtvulkan.exe --headless ^
  --scene scenes/validation/cornell.rtlevel ^
  --frames 3 ^
  --renderdoc-dll "C:\Program Files\RenderDoc\renderdoc.dll" ^
  --capture-renderdoc out/capture.rdc ^
  --capture-frame 1
```

When running the executable directly, the engine tries to load `renderdoc.dll` before creating Vulkan objects. It checks `--renderdoc-dll`, `RENDERDOC_DLL_PATH`, `RENDERDOC_DIR`, `RENDERDOC_SDK_DIR`, the current directory, the normal DLL search path, and standard RenderDoc install directories. If the DLL cannot be loaded, the engine prints:

```text
Warning: RenderDoc DLL not loaded. Set --renderdoc-dll <path>, RENDERDOC_DLL_PATH, or install RenderDoc.
```

That warning is expected and non-fatal; the run continues without a capture.

**Injected capture with `renderdoccmd`:**
```powershell
& "C:\Program Files\RenderDoc\renderdoccmd.exe" capture -w `
  -d "C:\Users\HomePc\ray-tracing-engine\native\vulkan" `
  -c "C:\Users\HomePc\ray-tracing-engine\native\vulkan\out\renderdoc_capture\renderdoccmd_capture" `
  "C:\Users\HomePc\ray-tracing-engine\native\vulkan\build\Release\rtvulkan.exe" `
  --headless --scene scenes/validation/cornell.rtlevel `
  --frames 3 --fixed-seed 1 `
  --capture-renderdoc out/renderdoc_capture/engine_capture.rdc `
  --capture-frame 1
```

`--capture-frame` is 1-based and defaults to frame 60. The run must render at least that many frames before shutdown. The `--capture-renderdoc` value is passed to RenderDoc as a capture file path template, so RenderDoc may append a suffix such as `_capture` or a frame number. For example, `engine_capture.rdc` may be written as `engine_capture_capture.rdc`.

**How it works:**
1. At build time, if `RENDERDOC_SDK_DIR` contains `renderdoc_app.h`, the `RTV_HAS_RENDERDOC` compile definition is set.
2. At runtime, the engine first checks for an already-loaded `renderdoc.dll`, then attempts `LoadLibraryA()` from the configured/common locations before Vulkan initialization.
3. If loaded, `RENDERDOC_GetAPI` is called to obtain the RenderDoc API v1.6.0.
4. If the API is available, `SetCaptureFilePathTemplate()` is called before the render loop.
5. On the requested frame, `StartFrameCapture()` is called before the frame draw and `EndFrameCapture()` is called after the draw.
6. If RenderDoc is not loaded or the API cannot be obtained, a warning is printed and execution continues normally.

**RenderDoc MCP/Python setup:** The MCP server uses the RenderDoc Python module and native DLL, which are separate from `RENDERDOC_SDK_DIR`.

```powershell
$env:RENDERDOC_MODULE_PATH = "C:\Users\HomePc\renderdoc\x64\Release\pymodules"
$env:PATH = "C:\Users\HomePc\renderdoc\x64\Release;$env:PATH"
```

The module path must contain `renderdoc.pyd`. The native DLL directory must contain `renderdoc.dll`, either on `PATH` or added by the MCP server with `os.add_dll_directory()`.

**Key constraints:** RenderDoc is NOT required for normal engine execution. If the DLL is not loaded at runtime, the feature is silently disabled with a warning.

---

### 6. Debug Package Export

`--make-debug-package <dir>` creates a self-contained diagnostic directory with everything needed to file a bug report or compare between commits.

```bat
rtvulkan.exe --headless ^
  --scene scenes/cornell.rtlevel ^
  --warmup-frames 30 ^
  --frames 120 ^
  --fixed-seed 1 ^
  --profile ^
  --profile-json out/profile.json ^
  --save-debug-views out/debug_views ^
  --make-debug-package out/debug_package
```

**Package contents:**

| File | Description |
|------|-------------|
| `profile.json` | Full profiling report with timings, memory, settings |
| `rendergraph.json` | Render graph structure (if `--dump-rendergraph` also specified) |
| `log.txt` | stdout and stderr captured during the run |
| `validation.txt` | Renderer validation log dump |
| `settings.json` | Full renderer settings snapshot |
| `scene_copy.rtlevel` | Copy of the input scene file |
| `debug_views/` | All exported debug view PNGs (if `--save-debug-views` also specified) |
| `capture.rdc` | RenderDoc capture (if `--capture-renderdoc` also specified and RenderDoc available) |

**`validation.txt` sections:**

```
=== Validation Log ===

--- Pass Events (42) ---
  atmosphere lut update
  render graph compiled pass count=11
  render graph pass: path_trace_rt
  ...

--- Barrier Events (23) ---
  render graph barrier raw hdr <external> -> path_trace_rt
  ...

--- Accumulation Invalidations (1) ---
  frame=0 reason=Startup

--- Scene Update Routes (2) ---
  RendererSettingsOnly: full (count=1)
  ...

--- Resource States (23) ---
  raw hdr <external> -> path_trace_rt [layout: 0 -> 65, stage: 0 -> 4096, access: 0 -> 2]
  ...
```

---

### 7. Validation Suite

`--run-validation-suite` runs a set of core validation scenes and outputs pass/fail results.

```bat
rtvulkan.exe --run-validation-suite --validation-output validation_output/
```

**Validation scenes:**

| Scene | File | Category | Frames | Warmup |
|-------|------|----------|--------|--------|
| Cornell | `cornell.rtlevel` | Basic | 120 | 30 |
| Glossy Metal | `glossy_metal.rtlevel` | Materials | 120 | 30 |
| Foliage Alpha | `foliage_alpha.rtlevel` | Materials | 120 | 30 |
| Sponza | `sponza.rtlevel` | Stress | 60 | 20 |
| Outdoor HDR | `outdoor_hdr.rtlevel` | Atmosphere | 120 | 30 |
| Material Grid | `material_grid.rtlevel` | Debug Views | 120 | 30 |
| Transform Stress | `transform_stress.rtlevel` | AS Refit | 120 | 30 |

**Output structure:**

```
validation_output/
  summary.json
  cornell/
    profile.json
    debug_views/
    validation.txt
    log.txt
  glossy_metal/
    ...
```

**`summary.json`:**

```json
{
  "scenes": [
    {
      "name": "cornell",
      "status": "pass",
      "gpu_ms_total": 4.2,
      "validation_errors": 0,
      "frames_rendered": 120
    },
    {
      "name": "glossy_metal",
      "status": "pass",
      "gpu_ms_total": 3.8,
      "validation_errors": 0,
      "frames_rendered": 120
    }
  ],
  "total_pass": 7,
  "total_fail": 0
}
```

**Automation script (`run_validation.ps1`):**

```powershell
.\run_validation.ps1 -BuildDir "build\Release"
```

The script runs each scene, captures stdout to `log.txt`, writes `summary.json`, and exits with code 1 if any scene fails. Suitable for CI pipelines.

---

## Typical Workflows

### CI Regression Testing

```bat
rtvulkan.exe --headless ^
  --scene scenes/validation/cornell.rtlevel ^
  --fixed-seed 1 ^
  --frames 120 ^
  --warmup-frames 30 ^
  --profile ^
  --profile-json ci/profile.json ^
  --make-debug-package ci/debug_package
```

- Compare `profile.json` between commits for performance regressions.
- Compare `beauty.png` pixel-by-pixel (PSNR/SSIM) for visual regressions.
- Compare per-pass GPU timings to detect which pass regressed.

### Bug Report Generation

```bat
rtvulkan.exe --headless ^
  --scene scenes/problem_scene.rtlevel ^
  --make-debug-package bug_report/
```

Attach the `bug_report/` directory to the issue. Contains everything needed to reproduce and diagnose.

### Debug View Inspection

```bat
rtvulkan.exe --headless ^
  --scene scenes/cornell.rtlevel ^
  --fixed-seed 1 ^
  --warmup-frames 4 ^
  --save-debug-views out/views
```

Open the PNGs to inspect individual rendering passes and data channels.

### Render Graph Visualization

```bat
rtvulkan.exe --headless ^
  --scene scenes/cornell.rtlevel ^
  --frames 1 ^
  --dump-rendergraph-dot out/graph.dot
dot -Tpng out/graph.dot -o graph.png
```

### Full Diagnostic Run

```bat
rtvulkan.exe --headless ^
  --scene scenes/validation/cornell.rtlevel ^
  --warmup-frames 30 ^
  --frames 120 ^
  --fixed-seed 1 ^
  --profile ^
  --profile-json out/profile.json ^
  --dump-rendergraph out/rendergraph.json ^
  --dump-rendergraph-dot out/rendergraph.dot ^
  --save-debug-views out/debug_views ^
  --capture-renderdoc out/capture.rdc ^
  --make-debug-package out/debug_package
```

This produces the complete set of diagnostic outputs in a single run.

---

## Editor Level-Editor Tooling

The repository includes PowerShell tools under `scripts/` to support `docs/EDITOR_LEVEL_EDITOR_IMPLEMENTATION_PLAN.md`. These tools complement the renderer diagnostics: they audit editor UI/request state, validate scene/project files, generate fixtures, and provide small regression probes for project, import, prefab, and migration work.

Use these tools before and after editor roadmap steps. Several audits are expected to fail while a planned feature is not implemented yet; use `-FailOn...` switches only after the corresponding milestone has landed.

### Static Editor Audits

```powershell
.\scripts\editor_plan_audit.ps1 -JsonOut out\editor_tools\plan_audit.json
.\scripts\imgui_panel_audit.ps1 -JsonOut out\editor_tools\imgui_audit.json
.\scripts\editor_request_flow_report.ps1 -JsonOut out\editor_tools\request_flow.json
.\scripts\editor_state_snapshot.ps1 `
  -Scene scenes\validation\cornell.rtlevel `
  -JsonOut out\editor_tools\state_snapshot.json
```

Purpose:

- `editor_plan_audit.ps1` reports whether planned systems exist in code, including `.rtproject`, `ProjectContext`, asset registry, import requests, prefabs, command registry, async scene loading, and autosave/recovery.
- `imgui_panel_audit.ps1` finds ImGui window/menu/dock labels and flags stale labels such as `Viewport`, `Scene Hierarchy`, `Inspector / Properties`, `Asset Browser`, `Open glTF`, and `Load glTF`.
- `editor_request_flow_report.ps1` lists `EditorRequests` fields and whether each has UI and `Application` usage.
- `editor_state_snapshot.ps1` writes a compact JSON summary of current source-level editor capabilities plus optional scene/project metadata.

Recommended use by roadmap step:

- Steps 1-4: run `imgui_panel_audit.ps1` and `editor_request_flow_report.ps1` after every UI/request migration.
- Steps 6-9: run `editor_plan_audit.ps1` after adding project, registry, and import request symbols.
- Step 14: run `editor_request_flow_report.ps1` after command-registry routing to catch stale direct request paths.

### Scene, Project, And Registry Validators

```powershell
.\scripts\rtlevel_schema_validator.ps1 `
  -Path scenes\validation `
  -JsonOut out\editor_tools\rtlevel_schema.json

.\scripts\rtlevel_compat_test.ps1 `
  -Path scenes\validation `
  -JsonOut out\editor_tools\rtlevel_compat.json

.\scripts\project_fixture_generator.ps1 `
  -Name ToolingSmoke `
  -Template PathTracedDefault `
  -Force `
  -JsonOut out\editor_tools\fixture.json

.\scripts\asset_registry_validator.ps1 `
  -RegistryPath out\project_fixtures\ToolingSmoke\Content\AssetRegistry.json `
  -JsonOut out\editor_tools\asset_registry.json
```

Purpose:

- `rtlevel_schema_validator.ps1` statically checks `.rtlevel` JSON shape, entity UUID/stable IDs, transform presence, and duplicate IDs.
- `rtlevel_compat_test.ps1` runs the schema validator and can optionally launch `rtvulkan.exe` for each scene with `-RunHeadless`.
- `project_fixture_generator.ps1` creates deterministic `.rtproject` fixtures with the expected `Content`, `Scenes`, `Cache`, `Saved`, `Config`, and `Build` layout.
- `asset_registry_validator.ps1` validates GUID uniqueness, project-relative paths, path normalization, and dependency references in `AssetRegistry.json`.

Use `-FailOnInvalid` once the checked format is part of the active milestone gate.

### Import, Merge, And Migration Probes

```powershell
.\scripts\scene_entity_count_probe.ps1 `
  -Before scenes\validation\cornell.rtlevel `
  -JsonOut out\editor_tools\entity_count.json

.\scripts\import_regression_harness.ps1 `
  -JsonOut out\editor_tools\import_harness.json

.\scripts\migration_backup_test.ps1 `
  -Scene scenes\validation\cornell.rtlevel `
  -JsonOut out\editor_tools\migration_backup.json
```

Purpose:

- `scene_entity_count_probe.ps1` records or compares entity counts. Use it to verify that `Import Asset` does not mutate the active scene, `Import and Place` adds exactly one prefab hierarchy, and `Merge Scene` appends without removing existing entities.
- `import_regression_harness.ps1` checks readiness for `Import Asset` and `Import and Place`. It is a static readiness harness until those requests exist; pass `-ExpectImplemented` after the import milestone lands.
- `migration_backup_test.ps1` creates copied scene and backup fixtures for migration tests and verifies the copies remain valid JSON with the same entity count.

### Editor Diagnostic Smoke

```powershell
.\scripts\editor_smoke.ps1 `
  -BuildDebug `
  -BuildRelease `
  -OutDir out\editor_smoke
```

`editor_smoke.ps1` can build Debug/Release and run the canonical Cornell diagnostic smoke, verifying `profile.json`, `rendergraph.json`, debug views, and debug package output. Use it after changes to shared editor request handling, scene loading, renderer synchronization, or diagnostic output paths.

For a faster static-only pass during UI work, run:

```powershell
.\scripts\imgui_panel_audit.ps1
.\scripts\editor_request_flow_report.ps1
.\scripts\rtlevel_schema_validator.ps1 -Path scenes\validation\cornell.rtlevel
```

---

## CMake Build Options

### RenderDoc Integration

```cmake
set(RENDERDOC_SDK_DIR "" CACHE PATH "Path to RenderDoc SDK directory")
if(EXISTS "${RENDERDOC_SDK_DIR}/renderdoc_app.h")
    target_compile_definitions(rtvulkan PRIVATE RTV_HAS_RENDERDOC=1)
    target_include_directories(rtvulkan PRIVATE "${RENDERDOC_SDK_DIR}")
endif()
```

Pass `-DRENDERDOC_SDK_DIR=<path>` when configuring CMake to enable RenderDoc capture support. The path must be the directory containing `renderdoc_app.h`; for the local source build use `C:/Users/HomePc/renderdoc/renderdoc/api/app`. If not specified, the feature is silently disabled.

---

## How AI Agents Should Use These Outputs

### Diagnosing a Render Bug

1. Run the engine with `--make-debug-package out/debug_package`.
2. Read `out/debug_package/profile.json` to check GPU timings, memory usage, and settings.
3. Read `out/debug_package/rendergraph.json` to verify pass order and barriers.
4. Read `out/debug_package/validation.txt` for validation errors or warnings.
5. Inspect `out/debug_package/debug_views/beauty.png` for the final output.
6. Inspect specific debug views to isolate the issue using the symptom-to-view table above.

### Comparing Between Commits

1. Run the same scene with `--fixed-seed 1` on both commits.
2. Diff the `profile.json` files — stable field names make this trivial.
3. Compare `beauty.png` pixel-by-pixel (PSNR/SSIM) to detect visual regressions.
4. Compare per-pass GPU timings to detect performance regressions.

### Built-in Profile and Image Comparison

```bat
rtvulkan.exe --compare-profile old/profile.json new/profile.json
rtvulkan.exe --compare-image baseline/beauty.png current/beauty.png --out out/diff.png
```

`--compare-profile` prints JSON deltas and regression percentages for CPU frame time, GPU frame time, per-pass GPU timings, and memory. Frame timing includes `p95` and `p99` so tail-latency regressions are visible even when averages are stable. `--compare-image` prints MSE, PSNR, SSIM, max error, and changed pixel percentage, and writes an amplified absolute-difference PNG when `--out` is provided.

Budget checks accept `gpu_frame_ms` or `gpu_frame_ms_avg` for average frame cost, plus `gpu_frame_ms_p95`, `gpu_frame_ms_p99`, and `gpu_frame_ms_max` for frame-time spike gates. Use the percentile keys for game-facing budgets because one-off startup spikes should usually be separated from steady-state tail latency. They also accept `per_pass_gpu_ms`, `per_pass_gpu_ms_p95`, and `per_pass_gpu_ms_p99` objects for named pass caps, `validation_error_count`, `memory_total_bytes`, `memory_total_mb`, and a `memory` object with byte caps for `textures_bytes`, `buffers_bytes`, `acceleration_structure_bytes`, `temporal_history_bytes`, `restir_reservoir_bytes`, `restir_di_current_bytes`, `restir_di_previous_bytes`, `restir_di_spatial_bytes`, `restir_gi_current_bytes`, `restir_gi_previous_bytes`, `restir_gi_spatial_bytes`, or `total_bytes`.

`run_game_stability_diagnostics.ps1 -IncludeMotion -BudgetPath <path>` also honors a `motion_stability` object after it generates `motion_stability_report.json`. Supported keys are `beauty_temporal_variance_score`, `uncorrelated_temporal_noise_score`, `denoiser_residual_noise_score`, and `strongest_temporal_diagnostic_correlation_min`. This makes moving-camera denoiser/ReSTIR stability part of the same pass/fail gate as frame time and memory.

The repo includes `budgets/balanced_game_16ms.json` as the default Balanced preset gate. It targets a 16.6 ms average and p95 GPU frame, a 17.0 ms p99, and an 18.0 ms max for steady-state diagnostic runs while also requiring zero validation errors, keeping persistent renderer memory under 640 MB, capping motion stability noise/correlation metrics, and capping average/p95/p99 costs for the main path-trace, moment-update, denoiser, history-copy, TAA, and ReSTIR GI final passes.

### Motion Stability Sequence Comparison

Use `--validation-camera-motion` with `--save-frame-sequence` to export deterministic moving-camera image sequences. Export `beauty` plus temporal debug views when diagnosing shimmer, ghosting, disocclusion, or unstable denoising.

```bat
rtvulkan.exe --headless ^
  --scene scenes/validation/cornell.rtlevel ^
  --validation-camera-motion ^
  --warmup-frames 4 --frames 32 --fixed-seed 1 ^
  --save-frame-sequence out/motion_baseline/frame_sequence ^
  --sequence-views beauty,motion-vectors,reprojection-confidence,temporal-history-weight,variance

rtvulkan.exe --headless ^
  --scene scenes/validation/cornell.rtlevel ^
  --validation-camera-motion ^
  --warmup-frames 4 --frames 32 --fixed-seed 1 ^
  --save-frame-sequence out/motion_current/frame_sequence ^
  --sequence-views beauty,motion-vectors,reprojection-confidence,temporal-history-weight,variance

rtvulkan.exe --compare-image-sequence ^
  out/motion_baseline/frame_sequence ^
  out/motion_current/frame_sequence ^
  --sequence-views beauty,motion-vectors,reprojection-confidence,temporal-history-weight,variance ^
  --out out/motion_compare
```

The sequence comparator writes `motion_stability_report.json`, amplified per-frame diffs under `diffs/<view>/`, `contact_sheet_<view>.png`, and worst-frame shortcuts such as `worst_beauty_diff.png`. The report includes baseline-vs-current SSIM/PSNR/MSE summaries, frame-to-frame temporal metrics, temporal debug correlations, and a `stability_summary`. `uncorrelated_temporal_noise_score` estimates how much beauty instability is not explained by motion vectors, reprojection confidence, temporal history weight, variance, or ReSTIR GI debug views; track it across runs when tuning motion denoising. `denoiser_residual_noise_score` narrows that estimate to beauty instability not explained by reprojection confidence, temporal history weight, or variance, so it is a better gate for residual shimmer after TAA/denoiser changes.

### Baseline Regression Checks

```bat
rtvulkan.exe --headless --scene scenes/validation/cornell.rtlevel ^
  --warmup-frames 30 --frames 120 --fixed-seed 1 ^
  --update-baseline --baseline-dir baselines

rtvulkan.exe --headless --scene scenes/validation/cornell.rtlevel ^
  --warmup-frames 30 --frames 120 --fixed-seed 1 ^
  --check-baseline --baseline-dir baselines
```

Baseline update stores `profile.json`, `rendergraph.json`, and `beauty.png` under `baselines/<scene>/`. Baseline check compares those files against a fresh run and exits nonzero on image, profile, or render graph regressions.

### Additional JSON Dumps

```bat
rtvulkan.exe --headless --scene scenes/validation/cornell.rtlevel ^
  --frames 120 --fixed-seed 1 ^
  --dump-memory out/memory.json ^
  --dump-frame-timeline out/timeline.json ^
  --dump-resource-lifetimes out/lifetimes.json ^
  --dump-bindings out/bindings.json ^
  --dump-shader-report out/shaders.json
```

These reports are machine-readable and intended for automated debugging. GPU debug counters are included in `profile.json`; current hardware pipeline statistics provide ray/hit/miss counts when available, while shader-atomic counters such as ReSTIR/TAA/denoiser accept/reject counts are reported as `null` until shader instrumentation is added.

### Automated Regression Testing

1. Run `--run-validation-suite --validation-output validation_output/`.
2. Read `validation_output/summary.json` for pass/fail status.
3. Each scene directory contains the full diagnostic package.
4. CI can upload these as artifacts for manual inspection.
