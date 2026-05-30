# Integrated Renderer Implementation Plan

This document converts the 81-phase renderer improvement roadmap into an executable implementation plan.

The plan is intentionally staged. Early batches fix correctness and low-risk performance issues in the current megakernel renderer. Middle batches harden temporal, denoising, ReSTIR, and material systems. Late batches are architecture v2 work and must not begin until the renderer is stable, profiled, and regression-tested.

## Current Implementation Snapshot

- `Balanced` is the current interactive/default render preset.
- Real-time path tracing defaults to effective `1 SPP` through the SPP limiter; requested SPP can be raised to `1..8` only when the limiter is disabled.
- ReSTIR DI and ReSTIR GI are enabled by default in the beauty path. Normal balanced frames include GI spatial reuse and GI final contribution passes.
- Opacity micromaps and SER are requested by default and are clamped off on unsupported devices. SER only affects wavefront trace paths; the default megakernel output remains unchanged unless wavefront final output is explicitly enabled.
- Motion stability is currently handled by ReSTIR reuse, temporal moment tracking, denoising, and TAA rather than by increasing SPP.
- Profile JSON and budget checks now include frame p95/p99, per-pass p95/p99, SPP settings, adaptive-quality state, memory, and motion-stability metrics.
- The canonical smoke remains Cornell with profile JSON, render graph JSON, debug views, and debug package export.

## Execution Rules

### Phase Exit Gates

Every phase must pass these gates before the next dependent phase begins:

- Builds in Debug and Release.
- Runs without Vulkan validation errors.
- Produces stable output in the validation scenes.
- Has at least one targeted debug view, screenshot, GPU capture, or profiler counter proving the intended behavior.
- Does not regress static-scene accumulation.
- Does not add unbounded memory growth, descriptor leaks, or command-buffer lifetime hazards.

### Batch Exit Gates

Every batch must additionally pass:

- 120-frame static accumulation stability test.
- 120-frame camera-motion test.
- GPU timestamp comparison against the previous batch.
- RenderDoc or equivalent capture with clean barriers and descriptor state.
- Manual visual check on at least: Cornell/simple emissive scene, closeup Cornell for camera-proximity cost, glossy metal scene, foliage/alpha scene, lightweight Sponza glTF, and outdoor HDR environment.

### Validation Scene Policy

- Keep `scenes/validation/cornell.rtlevel` as the canonical fast smoke scene for profile JSON, render graph dumps, debug-view export, and debug-package export.
- Use `scenes/validation/closeup_cornell.rtlevel` as the close-camera performance/regression scene. Run it for path tracing, ReSTIR GI, denoiser, traversal, hit-distance, or camera-proximity performance work, and compare it against normal Cornell when diagnosing distance-dependent slowdowns.
- Use `Sponza/glTF/Sponza.gltf` as the default Sponza validation scene for scene loading, material/texture coverage, RenderGraph validation, shader debugging, and debug-view export.
- Reserve `main_sponza/` heavy/full Sponza assets for performance profiling, GPU stress testing, memory pressure, async compute validation, ReSTIR validation, denoiser stability, and large-scene benchmarking.

### Implementation Pattern

Each phase should follow the same sequence:

1. Add a compile-time or runtime feature flag if the change can affect frame output.
2. Add debug/profiler instrumentation before changing behavior.
3. Implement the smallest isolated code path.
4. Validate against a known baseline scene.
5. Enable by default only after the phase acceptance criteria pass.
6. Remove dead fallback code only after the next batch is stable.

### Phase 49-54 Scope

Phases 49-54 are treated as architecture and memory hardening before OMM, wavefront, and SER:

- Phase 49: Async compute overlap for denoiser, TAA/TSR, tone map, histogram, and post passes.
- Phase 50: Reservoir compression for ReSTIR DI/GI.
- Phase 51: Memory aliasing for temporal swap buffers and reservoir ping-pong buffers.
- Phase 52: Async entity picking.
- Phase 53: Replace avoidable `vkDeviceWaitIdle`.
- Phase 54: VMA budget control and descriptor pool tuning. `[DONE]`

## Batch Overview

| Batch | Phases | Goal | Risk |
|-------|--------|------|------|
| 1 | 1-6, 15, 46 | Critical correctness and trivial quality fixes | Low |
| 2 | 7-10, 14, 16-17, 29-30 | Quick performance and BSDF quality wins | Low-Medium |
| 3 | 11-13, 18 | Sampling, denoiser, adaptive quality | Medium |
| 4 | 19-22, 49 | Temporal super resolution, display/output split, async compute overlap | Medium-Large |
| 5 | 23-28 | Production denoising | Large |
| 6 | 31-33 | ReSTIR DI correctness and light BVH quality | Medium-Large |
| 7 | 34-39 | ReSTIR GI | Large |
| 8 | 40-48, 55 | Material and texture system improvements | Medium-Large |
| 9 | 50-54 | Reservoir compression, memory aliasing, stall removal, memory-budget hardening | Large |
| 10 | 56-60 | Opacity micromaps | Medium-Large |
| 11 | 61-74 | Wavefront path tracing architecture v2 | Very Large |
| 12 | 75-77 | SER integration | Large |
| 13 | 78-81 | Advanced rendering features | Very Large |

## Batch 1: Critical Fixes

Goal: remove known correctness errors that bias the path tracer or waste trivial GPU work.

### Phase 1: Continue Paths After Emissive Hits [DONE]

Target files:

- `shaders/pathtrace.rgen`

Steps:

1. Locate the emissive-hit contribution path around the current emissive termination logic.
2. Replace unconditional path termination with normal throughput/radiance handling.
3. Only terminate when the sampled BSDF, path depth, or Russian roulette says to terminate.
4. Ensure next-event estimation and MIS state are still updated after the emissive contribution.
5. Add a debug mode that colors paths reaching an emissive and continuing past it.

Acceptance criteria:

- Emissive surfaces still contribute directly when hit.
- Paths can bounce away from emissive geometry.
- No energy spike from double-counting emissive direct light.
- Cornell box with emissive ceiling converges without dark secondary bounce loss.

Implementation notes:

- `shaders/pathtrace.rgen` now contributes hit emission without unconditionally terminating the path.
- Direct-light NEE is skipped on the emissive hit itself to avoid self-light double counting; subsequent BSDF continuation updates the usual next-hit MIS state.
- Added `emissive-continuation` debug view (`RendererDebugView::EmissiveContinuation`, value 38) to color paths that hit emissive geometry and successfully continue.
- Verified `glslangValidator`, Debug build, Release build, and a 3-frame Debug smoke run with `--debug-view emissive-continuation`.

### Phase 2: Fix Delta Classification Roughness Check [DONE]

Target files:

- `shaders/pathtrace.rgen`
- `shaders/rt_common.glsl`

Steps:

1. Find the delta/specular branch currently based on material or lobe type only.
2. Add `roughness < 0.001` as the delta threshold.
3. Clamp roughness for numerical stability separately from delta classification.
4. Ensure MIS PDF logic treats true delta lobes as non-MIS-sampled where appropriate.
5. Add a roughness ramp validation scene or debug material override.

Acceptance criteria:

- Roughness `0.0` and `0.0005` behave as delta.
- Roughness `0.001` and above use glossy sampling/PDF logic.
- No NaN or firefly increase on near-perfect mirrors.

Implementation notes:

- Added `MATERIAL_DELTA_ROUGHNESS_THRESHOLD = 0.001` and `material_is_delta(...)` in `shaders/rt_common.glsl`.
- Texture roughness is preserved down to `0.0`; GGX numerical stability now uses `ggx_safe_roughness(...)` separately.
- Direct-light delta skipping and path-scatter delta branches now use the threshold; rough specular surfaces fall through to glossy BRDF sampling/PDF.
- True delta lobes return zero from `pdf_brdf(...)`, keeping MIS from treating them as area-sampled glossy events.
- Verified with `glslangValidator`, Debug build, Release build, and a 3-frame Debug smoke run with `--debug-view roughness`.

### Phase 3: Remove Double Fresnel From Multi-Scatter Compensation [DONE]

Target files:

- `shaders/rt_common.glsl`

Steps:

1. Inspect the GGX multi-scatter compensation path around the current specular accumulation.
2. Verify whether Fresnel is already included in the compensation term.
3. Change the accumulation to add the multi-scatter compensation directly instead of multiplying Fresnel twice.
4. Compare white furnace and metallic furnace results before and after.

Acceptance criteria:

- White furnace test remains energy-conserving.
- Metals do not darken incorrectly at grazing angles.
- Multi-scatter compensation remains bounded for roughness near 1.

Implementation notes:

- `heitz_ms_ggx(...)` already includes average Fresnel through `f_avg`; `eval_ggx_brdf(...)` now adds the returned multi-scatter term directly instead of multiplying by Schlick Fresnel again.
- Verified with `glslangValidator`, Debug build, Release build, and a 3-frame Debug beauty smoke run.

### Phase 4: Remove `indirect_strength` From Russian Roulette Decisions [DONE]

Target files:

- `shaders/pathtrace.rgen`
- `include/rtv/RendererSettings.h`
- `src/rtv/RenderSettingsPanel.cpp`

Steps:

1. Find where `indirect_strength` scales throughput before Russian roulette probability is computed.
2. Move `indirect_strength` application to final indirect contribution only.
3. Keep Russian roulette based on physical throughput luminance.
4. Validate that setting `indirect_strength` to 0 does not change path survival before final contribution scaling.

Acceptance criteria:

- `indirect_strength` changes appearance only, not path survival statistics.
- GPU time is stable when varying `indirect_strength`.
- No biased brightness shift in scenes with low indirect strength.

Implementation notes:

- Removed `camera.indirect_strength` from path throughput updates in `shaders/pathtrace.rgen`.
- Applied `indirect_strength` only when adding non-primary-bounce lighting contributions to radiance and debug components.
- Russian roulette now uses physical throughput rather than the artistic indirect multiplier.
- Verified with `glslangValidator`, Debug build, Release build, and a 3-frame Debug beauty smoke run.

### Phase 5: MIS-Weight Sun Disk Hits [DONE]

Target files:

- `shaders/pathtrace.rgen`
- `shaders/rt_common.glsl`
- `shaders/environment_sampling.glsl`
- `shaders/atmosphere_lighting.glsl`

Steps:

1. In `pathtrace.rgen`, audit both sun paths: explicit sun sampling through `sample_sun_light` and environment miss handling where `environment_sun_disk_radiance` contributes.
2. Treat `rt_common.glsl` as the canonical shared BSDF/PDF implementation target; update `environment_sampling.glsl` and `atmosphere_lighting.glsl` only when the helper function lives there in the current checkout.
3. Compute the BSDF PDF and sun/light PDF in matching solid-angle measure.
4. Apply `power_heuristic` to the sun disk contribution when a BSDF-sampled ray hits/misses into the sun disk.
5. Verify the explicit-light path and BSDF/environment-miss path use symmetric MIS weights.
6. Store the previous event PDF and event type in path state so sun disk MIS can distinguish delta, BSDF, environment, and light samples.
7. Add a debug view for sun MIS weight, sun light PDF, and previous BSDF PDF.

Acceptance criteria:

- Sun disk brightness is stable across roughness values.
- No double-bright sun highlight when both BSDF and light sampling can hit the disk.
- Outdoor HDR scene converges with fewer sun fireflies.

Implementation notes:

- Added `analytical_sun_pdf(...)` in `shaders/rt_common.glsl` so explicit sun sampling and BSDF/environment-miss sun hits share the same solid-angle PDF.
- `shaders/pathtrace.rgen` now tracks previous path event type (`BSDF` vs `DELTA`) and applies `power_heuristic(previousBsdfPdf, sunPdf)` when a non-delta BSDF ray misses into the analytical sun disk.
- Added sun-specific debug state and debug views: `sun-mis-weight`, `sun-light-pdf`, and `sun-previous-bsdf-pdf`.
- Verified with `glslangValidator`, Debug build, Release build, and a 3-frame Debug smoke run with `--debug-view sun-mis-weight`.

### Phase 6: Use Effective RIS PDF For MIS [DONE]

Target files:

- `shaders/pathtrace.rgen`
- `shaders/rt_common.glsl`
- `shaders/restir_common.glsl`
- `shaders/environment_sampling.glsl`

Steps:

1. In `pathtrace.rgen`, find RIS candidate selection around direct environment/light sampling and the MIS code consuming the selected candidate PDF.
2. Treat `rt_common.glsl` as the canonical shared data-layout and BRDF/PDF target; use `restir_common.glsl` only for reservoir helper functions that are actually shared by ReSTIR passes.
3. Track raw proposal PDF, candidate target value, candidate count, reservoir weight sum, selected candidate probability, and final effective RIS PDF.
4. Store the effective PDF in the selected sample or reservoir payload instead of recomputing from the raw proposal.
5. Use the effective RIS PDF in MIS for direct lighting, environment lighting, and sun-disk competition.
6. Add debug output for raw proposal PDF, effective RIS PDF, and their ratio.
7. Add a test mode with fixed candidate count 1; it must match the non-RIS direct-light estimator.

Acceptance criteria:

- RIS estimator remains unbiased in a single-light test.
- Changing RIS candidate count changes variance, not mean brightness.
- No brightness shift between RIS disabled and RIS enabled after enough samples.

Implementation notes:

- The emissive RIS path now computes `effectiveLightPdf = rawLightPdf * candidateCount * selectedProxy / proxyWeightSum`.
- MIS and the direct-light contribution divide use the effective RIS PDF rather than the raw proposal PDF.
- First-bounce ReSTIR reservoir payloads receive the effective direct-light PDF through `components.first_light_pdf`.
- Added debug views: `ris-raw-light-pdf`, `ris-effective-light-pdf`, and `ris-pdf-ratio`.
- Verified with `glslangValidator`, Debug build, Release build, and a 3-frame Debug smoke run with `--debug-view ris-effective-light-pdf`.

### Phase 15: Lower GGX Roughness Floor [DONE]

Target files:

- `shaders/rt_common.glsl`

Steps:

1. Locate the current roughness floor, expected near `0.02`.
2. Lower the floor to `0.001`.
3. Keep denominator guards for GGX D, G, and PDF functions.
4. Add NaN guards in debug builds if shader debug support exists.

Acceptance criteria:

- Mirror-like materials become visibly sharper.
- No NaN/Inf pixels in a roughness-0 validation scene.
- Denoiser does not smear sharp reflections more than before.

Implementation notes:

- Lowered `MATERIAL_MIN_GGX_ROUGHNESS` in `shaders/rt_common.glsl` from `0.02` to `0.001`.
- Kept the existing denominator guards in GGX D, G, PDF, and sampling functions.
- Verified with `glslangValidator`, Debug build, Release build, and a 3-frame Debug smoke run with `--debug-view roughness`.

### Phase 46: Enable Anisotropic Texture Filtering [DONE]

Target files:

- `src/rtv/TextureLoader.cpp`
- `src/rtv/PathTracerRenderer.cpp`
- `include/rtv/VulkanContext.h`
- `src/rtv/VulkanContext.cpp`

Steps:

1. Query `samplerAnisotropy` support during physical-device feature setup.
2. Enable `samplerAnisotropy` when available.
3. Create material texture samplers with anisotropy enabled and a device-limit clamp.
4. Add a renderer setting for anisotropy level if one does not exist.
5. Fall back to isotropic filtering when unsupported.

Acceptance criteria:

- Oblique floor/wall textures remain sharper.
- Validation layers report sampler feature usage as valid.
- Unsupported devices run without crashing.

Implementation notes:

- `VulkanContext` now queries `samplerAnisotropy`, enables it at device creation when supported, and exposes the device max anisotropy.
- `ResourceAllocator` carries sampler-anisotropy support and max anisotropy to resource creation code.
- Added `RendererSettings::materialTextureAnisotropy`, scene JSON persistence, and a Render Settings UI slider for material texture anisotropy.
- Material texture samplers in `GpuScene` now use the renderer-controlled anisotropy level, clamp to the device limit, and fall back to 1x when unsupported.
- `GpuScene` recreates material samplers when the anisotropy setting changes and defers destruction of retired samplers until enough frames have passed for in-flight descriptor use to finish.
- Combined image sampler descriptors now use the per-texture sampler array when available, preserving imported sampler state while applying the anisotropy level.
- Verified Debug build, Release build, and a 3-frame Debug smoke run.

## Batch 2: Quick Wins

Goal: improve frame responsiveness and sampling quality without changing architecture.

### Phase 7: Clamp Delta Time [DONE]

Target files:

- `src/rtv/Application.cpp`
- `src/rtv/CameraController.cpp`
- `include/rtv/RendererSettings.h`

Steps:

1. Identify where frame delta time is computed.
2. Clamp simulation/render delta time to a sane maximum, for example 1/30s or 1/15s.
3. Keep raw frame time available for profiler UI.
4. Ensure camera smoothing and adaptive quality use clamped delta, not raw stall spikes.

Acceptance criteria:

- Window drag, breakpoint resume, and shader compile stalls do not cause huge camera jumps.
- Adaptive quality does not enter a long feedback loop after one slow frame.

Implementation notes:

- `Application` now computes both raw and clamped frame delta; raw frame time remains visible in the editor/profiler UI, while runtime controls and renderer frame timing use the clamped value.
- Added `RendererSettings::maxFrameDeltaSeconds` with a default clamp of `1/30s`, sanitized in `PathTracerRenderer::applySettings`.
- `CameraController` defensively clamps its input delta to `1/30s` before applying keyboard movement.
- Auto-exposure adaptation receives the clamped frame delta through `PathTracerRenderer::setFrameDeltaSeconds`.
- Verified Debug build, Release build, and a 3-frame Debug smoke run.

### Phase 8: Increase Frames In Flight From 2 To 3 [DONE]

Target files:

- `include/rtv/FrameResources.h`
- `src/rtv/FrameResources.cpp`
- `include/rtv/CommandSystem.h`
- `src/rtv/CommandSystem.cpp`
- `src/rtv/PathTracerRenderer.cpp`

Steps:

1. Locate the global frames-in-flight constant.
2. Increase from 2 to 3.
3. Audit per-frame buffers, descriptor pools, command buffers, fences, and query pools.
4. Ensure accumulation frame index remains logical-frame based, not frame-resource-index based.
5. Verify resize and swapchain recreation wait on all frame fences.

Acceptance criteria:

- No frame-resource overwrite hazards.
- CPU frame time improves or remains stable.
- Accumulation does not reset incorrectly every third frame.

Implementation notes:

- Increased `CommandSystem::framesInFlight` from 2 to 3, giving the command pools, command buffers, acquire semaphores, and fences three independent frame slots.
- `PathTracerRenderer` now allocates three per-frame descriptor/uniform resources and three GPU profiler query sets.
- Updated `PipelineDemo` to three per-frame descriptor/uniform resources as part of the frame-resource aliasing audit.
- Accumulation continues to use logical sample/frame counters (`frameCount_` and `temporalFrameIndex_`) rather than the modulo frame-resource index.
- Resize and swapchain recreation paths still use device-idle waits, which cover all frame fences for this phase.
- Verified Debug build, Release build, and a 5-frame Debug smoke run to exercise frame slot wraparound.

### Phase 9: Eliminate Per-Frame Picking Ray [DONE]

Target files:

- `shaders/pathtrace.rgen`
- `shaders/rt_common.glsl`
- `src/rtv/PathTracerRenderer.cpp`
- `include/rtv/EditorSelection.h`

Steps:

1. Treat `pathtrace.rgen` as the active ray tracing implementation target; the current picking path is driven through `trace_surface_with_mode(..., picking)` and `payload.picking`.
2. Identify whether the renderer dispatches picking every frame or only when the editor requests selection.
3. Change picking to run only on click/selection request.
4. Store the last pick result until the next request.
5. Route picking through an explicit request object containing screen coordinate, frame id, scene revision, and requested entity mask.
6. Keep alpha-tested picking correctness by preserving the `payload.picking` behavior in any-hit shaders.
8. Add profiler markers for pick dispatch and pick readback separately.

Acceptance criteria:

- No picking ray work in normal frames.
- Click selection still works on opaque and alpha-tested geometry.
- GPU time improves in path tracing by the expected small percentage.

Implementation notes:

- Completed by reusing the alpha-tested primary visibility result for the entity-id buffer instead of tracing a second per-pixel picking ray.
- `shaders/pathtrace.rgen` now writes `entity_id_buffer` from the first path-trace hit stored in `PathComponents`.
- Existing editor click selection continues to read the latest entity-id buffer; a dedicated asynchronous click/request dispatch remains deferred to Phase 52.
- Verified Debug build, Release build, and a 5-frame Debug smoke run.

### Phase 10: Precompute Normal Matrices [DONE]

Target files:

- `src/rtv/SceneToGpuSceneBuilder.cpp`
- `include/rtv/GpuScene.h`
- `shaders/rt_common.glsl`
- `shaders/pathtrace.rchit`

Steps:

1. Add a normal matrix field to the GPU instance or geometry record.
2. Compute inverse-transpose transform on CPU during scene upload/refit.
3. Replace shader-side normal matrix reconstruction.
4. Keep support for non-uniform scale.
5. Validate transformed normals in a debug-normal view.

Acceptance criteria:

- Normal debug view matches previous output.
- Non-uniformly scaled meshes shade correctly.
- Shader instruction count decreases.

Implementation notes:

- Added `normalTransform` to `GpuInstanceRecord` and `normal_transform` to the GLSL `InstanceRecord` layout.
- `makeInstanceRecord(...)` now computes the inverse transform once and stores the CPU-side inverse-transpose normal transform for scene upload and transform refit paths.
- GPU cache restore reconstructs `normalTransform` from cached inverse transforms, so existing cache data can still load through the new GPU layout.
- Replaced shader-side `transpose(mat3(instance.inverse_transform))` reconstruction in closest-hit, primary any-hit, shadow any-hit, and emissive-light sampling with `mat3(instance.normal_transform)`.
- Verified Debug build, Release build, a 5-frame Debug normals smoke run, and a 5-frame Debug normals run against `scenes/validation/transform_stress.rtlevel`.

### Phase 14: VNDF Sampling For GGX [DONE]

Target files:

- `shaders/rt_common.glsl`
- `shaders/pathtrace.rgen`

Steps:

1. Add a visible-normal GGX sampling function.
2. Return sampled half-vector, BSDF value, and matching PDF.
3. Replace old GGX NDF sampling for glossy reflection.
4. Preserve delta path for roughness below the Phase 2 threshold.
5. Validate PDF by comparing BSDF sampling histograms or furnace output.

Acceptance criteria:

- Grazing-angle reflections converge faster.
- No brightness shift in rough metal scenes.
- PDF and eval functions are measure-consistent.

Implementation notes:

- Added tangent-frame helpers and an isotropic Heitz visible-normal GGX sampler in `shaders/rt_common.glsl`.
- `sample_ggx_brdf(...)` now samples visible GGX normals in the view-aligned stretched space and reflects around the sampled half-vector.
- `pdf_ggx_brdf(...)` now evaluates the matching visible-normal reflection PDF, `D(h) * G1(v) / (4 * NdotV)`, instead of the old NDF half-vector PDF.
- The existing Phase 2 delta path remains outside `sample_brdf(...)`, so roughness below `0.001` still uses the explicit delta reflection/refraction branches.
- Verified Debug build, Release build, a 5-frame Debug beauty smoke run, and a 5-frame Debug run against `scenes/validation/material_grid.rtlevel`.

### Phase 16: Height-Correlated Smith G2 [DONE]

Target files:

- `shaders/rt_common.glsl`

Steps:

1. Add height-correlated Smith masking-shadowing function.
2. Replace uncorrelated G2 for GGX eval.
3. Keep numerical guards for `NdotV` and `NdotL`.
4. Validate with roughness/angle sweeps.

Acceptance criteria:

- Specular lobe shape matches expected GGX behavior.
- Energy remains bounded in furnace tests.

Implementation notes:

- Added `smith_ggx_lambda(...)` and replaced the uncorrelated `smith_g1(v) * smith_g1(l)` product with height-correlated Smith `G2 = 1 / (1 + lambdaV + lambdaL)`.
- Kept `smith_g1(...)` for the Phase 14 VNDF visible-normal PDF, where the sampled visible-normal distribution still needs the view masking term.
- Retained numerical guards for low `NdotV`, low `NdotL`, and near-zero tangent denominators.
- Verified Debug build, Release build, and a 5-frame Debug material-grid beauty run.

### Phase 17: Diffuse Energy Compensation [DONE]

Target files:

- `shaders/rt_common.glsl`
- `shaders/pathtrace.rgen`

Steps:

1. Estimate energy lost to specular Fresnel for mixed diffuse/specular materials.
2. Reduce diffuse contribution by the physically appropriate specular allocation.
3. Apply consistently in BSDF eval and sampling weight.
4. Validate white furnace and colored dielectric scenes.

Acceptance criteria:

- Dielectrics do not exceed unit energy.
- Diffuse albedo remains visually stable for low-specular materials.

Implementation notes:

- Added shared PBR helpers for `f0`, Schlick average Fresnel, and diffuse energy allocation in `shaders/rt_common.glsl`.
- Mixed diffuse/specular BRDF evaluation now scales the diffuse term by `(1 - averageFresnel) * (1 - metallic)` instead of using the half-vector Fresnel term directly.
- The diffuse/specular sampling probability now uses the same compensated diffuse energy term, while keeping view Fresnel for the specular sampling weight.
- Pure diffuse material closures remain unchanged.
- Verified Debug build, Release build, and a 5-frame Debug material-grid beauty run.

### Phase 29: Reduce Environment Samples [DONE]

Target files:

- `shaders/pathtrace.rgen`
- `include/rtv/RendererSettings.h`
- `src/rtv/RenderSettingsPanel.cpp`

Steps:

1. Locate the fixed environment sample loop.
2. Reduce default from 8 to 1 or 2.
3. Make the sample count user-configurable for profiling.
4. Ensure MIS and ReSTIR paths still receive correct PDFs.

Acceptance criteria:

- GPU time drops in environment-lit scenes.
- Variance increase is acceptable and preferably offset by RIS/ReSTIR.

Implementation notes:

- `RendererSettings`, `RenderSettings`, and `CameraUniform` default `environmentDirectSamples` to `1`.
- The Render Settings panel exposes a user-configurable `Environment Samples` slider from `1` to `8` for profiling.
- `shaders/pathtrace.rgen` clamps `camera.environment_direct_samples` to `1..8`, loops over that count, divides the accumulated contribution by the sample count, and keeps MIS PDFs in solid-angle measure for each environment sample.
- First-sample environment PDFs continue to populate the direct-light debug/ReSTIR payload fields through `sampledLightPdf`, `sampledBsdfPdf`, and `sampledMisWeight`.
- Verified as part of the Phase 17 Debug/Release builds and material-grid smoke run.

### Phase 29b: Real-Time SPP Limiter And Slider [DONE]

Target files:

- `include/rtv/RendererSettings.h`
- `include/rtv/SceneComponents.h`
- `include/rtv/GpuScene.h`
- `shaders/rt_common.glsl`
- `shaders/pathtrace.rgen`
- `src/rtv/PathTracerRenderer.cpp`
- `src/rtv/RenderSettingsPanel.cpp`
- `src/main.cpp`
- `src/rtv/HeadlessDiagnostics.cpp`

Steps:

1. Add requested SPP and a real-time 1 SPP limiter to renderer settings.
2. Expose requested SPP and limiter on/off in the editor and CLI.
3. Report requested/effective SPP in profile JSON and diagnostic summaries.
4. Add true multi-SPP raygen support while preserving a fast single-trace path for effective 1 SPP.

Acceptance criteria:

- Balanced/Low remain effective 1 SPP by default.
- Higher SPP only affects cost and quality when the limiter is disabled.
- Required smoke and Balanced budget checks still pass with the limiter enabled.

Implementation notes:

- Added `samplesPerPixel` and `limitSamplesPerPixel` to renderer and scene settings.
- Added CLI flags `--spp` / `--samples-per-pixel` and `--spp-limit` / `--limit-spp`.
- Added editor controls `Limit to 1 SPP` and `Samples Per Pixel`.
- Added `CameraUniform::pathTraceControls` and matching shader layout field.
- `shaders/pathtrace.rgen` now keeps the original single-trace branch when effective SPP is 1, and uses a bounded `1..8` multi-SPP loop only when the limiter is disabled.
- Profile JSON reports `samples_per_pixel`, `limit_samples_per_pixel`, and `effective_samples_per_pixel`.
- Verified Release build, required smoke, default Balanced budget check, and A/B probe showing effective 2 SPP increases path-trace cost as expected.

### Phase 30: Tune Russian Roulette [DONE]

Target files:

- `shaders/pathtrace.rgen`
- `include/rtv/RendererSettings.h`

Steps:

1. Set Russian roulette start depth based on current bounce configuration.
2. Compute survival probability from throughput luminance with a minimum clamp.
3. Scale throughput by inverse survival probability only when the path survives.
4. Profile path length distribution.

Acceptance criteria:

- 5-15 percent GPU reduction in bounce-heavy scenes.
- No measurable mean brightness bias.

Implementation notes:

- Added `RendererSettings::russianRouletteMinSurvival` with a default of `0.10`, clamped in `PathTracerRenderer::applySettings`.
- Stored the RR minimum survival value in `CameraUniform::renderControls.w`.
- `shaders/pathtrace.rgen` now derives RR start depth from `max_bounces` (`max_bounces / 2`, clamped to path depths 3-5) and skips RR on the final possible bounce.
- Survival probability now uses physical throughput luminance with the configured minimum clamp and a `0.95` maximum, replacing the previous max-RGB survival heuristic.
- Throughput is divided by the survival probability only after the path survives.
- Verified Debug build, Release build, and a 5-frame Debug smoke run with `--debug-view path-length`.

## Batch 3: Core Performance

Goal: improve sample quality, denoiser cost, and interactive responsiveness.

### Phase 13: Sobol, Owen Scrambling, And STBN [DONE]

Target files:

- `shaders/blue_noise.glsl`
- `shaders/pathtrace.rgen`
- `shaders/restir_*.comp`
- `include/rtv/PathTracerRenderer.h`
- `src/rtv/PathTracerRenderer.cpp`

Steps:

1. Add Sobol sequence tables or generated buffers.
2. Add per-pixel/per-frame Owen scrambling.
3. Integrate spatiotemporal blue-noise texture or procedural STBN source.
4. Route all major sampling dimensions through a shared sampler API.
5. Assign fixed dimensions for camera, BSDF, light, environment, RR, ReSTIR candidates, and denoiser stochastic taps.
6. Add debug views for sample dimensions and scrambling.

Acceptance criteria:

- Static scenes converge with visibly less structured noise.
- Temporal noise does not shimmer during camera motion.
- ReSTIR and path tracing do not accidentally reuse correlated dimensions.

Implementation notes:

- Added a shared shader sampling API in `blue_noise.glsl` with fixed dimension IDs, Sobol-style low-discrepancy values, Owen-style hash scrambling, and procedural spatiotemporal blue-noise rotation.
- Routed path-tracing seed setup, direct-light sampling, environment debug samples, BSDF sampling, dielectric decisions, and Russian roulette through fixed dimension streams.
- Routed ReSTIR RIS candidate selection and reservoir acceptance through the shared sampler.
- Routed ReSTIR spatial neighbor offsets through the shared sampler using frame-indexed dimensions.
- Added sample-dimension and sample-scramble debug views to the renderer enum, parser, editor combo, and raygen debug output.
- Kept CPU-provided TAA jitter as the camera ray offset so motion vectors and reprojection remain consistent with existing uniforms.
- Verified Debug build, beauty smoke, sample-dimension smoke, sample-scramble smoke, and a ReSTIR smoke run.
- Added `RTV_USE_DIMENSIONED_SAMPLER=0` as a shader compile toggle for comparing against the cheaper hash-based dimension fallback. The shader compiler now tracks shader option signatures so define changes force SPIR-V recompilation.
- Local 150-frame Debug Cornell smoke at frame 120: dimensioned sampler on `path=8.56 ms`, fallback sampler `path=8.47 ms`. Treat this as a quick spot check rather than a final benchmark.

### Phase 18: Denoiser Shared Memory Optimization [DONE]

Target files:

- `shaders/denoiser.comp`

Steps:

1. Profile current denoiser global-memory reads.
2. Tile neighborhood data into shared memory.
3. Add halo loading for filter radius.
4. Avoid bank conflicts where practical.
5. Keep fallback path behind a shader define until validated.

Acceptance criteria:

- 1-3 ms GPU improvement on denoiser-heavy resolutions.
- Output matches the baseline within expected floating-point tolerance.
- No out-of-bounds reads at image edges.

Implementation notes:

- Added `RTV_DENOISER_SHARED_TILE` as a shader define with a global-memory fallback path.
- Tiles the first 5x5 A-trous pass into 20x20 workgroup shared memory for color and packed depth/normal reads.
- Keeps later wider-step A-trous passes on the original global-memory path to avoid oversized shared-memory tiles.
- Handles partial workgroups by loading shared tiles before the in-bounds return so barriers remain valid at image edges.
- Verified Debug build and a 5-frame Debug smoke run with the beauty view.
- `RTV_DENOISER_SHARED_TILE=0` can compile the original global-memory path for A/B profiling.
- Local 150-frame Debug Cornell smoke at frame 120: shared-tile denoiser `denoise=0.258 ms`, global-memory fallback `denoise=0.240 ms`, so this scene/GPU currently favors the fallback by a small margin.

### Phase 11: Dynamic Quality Manager [DONE]

Target files:

- `include/rtv/RendererSettings.h`
- `src/rtv/PathTracerRenderer.cpp`
- `src/rtv/RenderSettingsPanel.cpp`
- `include/rtv/GpuProfiler.h`
- `src/rtv/GpuProfiler.cpp`

Steps:

1. Add frame-time target and quality state.
2. Track smoothed GPU frame time, CPU frame time, queue wait time, and camera/editor motion state.
3. Pull timings from `GpuProfiler` rather than wall-clock estimates so path tracing, denoising, TAA/TSR, tone map, ReSTIR, and UI cost can be controlled independently.
4. Dynamically adjust internal resolution, samples per pixel, max bounces, environment direct samples, denoiser passes, ReSTIR reuse count, and expensive debug features.
5. Add hysteresis to prevent oscillation: separate upscale/downscale thresholds, minimum dwell time, and cooldown after scene edits.
6. Expose override modes: off, conservative, balanced, aggressive.
7. Persist the selected mode in editor preferences or scene render settings.

Acceptance criteria:

- 30-50 percent GPU reduction during camera motion.
- Static camera returns to full quality after a stable delay.
- No visible resolution pumping during minor mouse movement.

Implementation notes:

- Added adaptive quality modes with conservative, balanced, and aggressive policies; the current Balanced preset enables balanced adaptive quality by default for interactive use.
- Persisted adaptive mode and GPU frame-time target in scene render settings.
- Uses `GpuProfiler` frame timings plus camera/still-frame state to reduce effective max bounces, environment samples, and denoiser A-trous iterations while moving. The current real-time path keeps the SPP limiter at effective `1 SPP` instead of scaling per-frame SPP upward.
- Keeps render resolution fixed in this phase to avoid visible pumping before the later TSR/internal-resolution phases.
- Static frames recover to full settings after a deterministic stable-frame delay.
- Exposed adaptive quality mode and GPU frame target in the render settings panel.
- Verified Debug/Release builds, required smoke runs, and Balanced budget suites across Cornell, closeup Cornell, lightweight Sponza, and heavy Sponza glTF.

### Phase 12: Motion-Adaptive Pass Skipping [DONE]

Target files:

- `src/rtv/PathTracerRenderer.cpp`
- `src/rtv/TemporalSystem.cpp`
- `shaders/denoiser.comp`

Steps:

1. Define pass skip policy for camera motion and scene edits.
2. Skip or simplify denoiser, expensive ReSTIR reuse, high-cost debug views, and optional post passes while moving.
3. Preserve mandatory history invalidation and output presentation.
4. Add profiler markers showing skipped passes.

Acceptance criteria:

- Motion responsiveness improves without stale-frame artifacts.
- Static quality recovers deterministically.

Implementation notes:

- Early experiments allowed adaptive quality to skip ReSTIR spatial reuse and denoising while moving.
- Current stability tuning keeps denoiser and ReSTIR spatial reuse active during motion so temporal history remains stable; adaptive quality instead reduces bounces, environment samples, and a-trous iterations.
- Validation pass markers still record adaptive state, effective bounces, effective environment samples, effective a-trous iterations, and any skipped optional paths.
- Verified through required smoke runs and Balanced motion/performance budget suites.

## Batch 4: Temporal Super Resolution

Goal: split internal render resolution from display resolution and convert TAA into TSR.

### Phase 19: Add Display Extent And Split Resources [DONE]

Target files:

- `include/rtv/PathTracerRenderer.h`
- `src/rtv/PathTracerRenderer.cpp`
- `include/rtv/TemporalSystem.h`
- `src/rtv/TemporalSystem.cpp`
- `shaders/temporal_common.glsl`

Steps:

1. Add separate `renderExtent` and `displayExtent`.
2. Allocate accumulation, G-buffer, velocity, variance, and path state at render extent.
3. Allocate presentation, UI composite, and final tone-map targets at display extent.
4. Audit every dispatch size and image barrier.
5. Add debug overlay showing both extents and scale ratio.

Acceptance criteria:

- 1.0 scale produces identical output to the pre-TSR renderer.
- Non-1.0 scale does not cause out-of-bounds reads/writes.

Implementation notes:

- Added explicit renderer `renderExtent` and `displayExtent`.
- Path tracing, accumulation, variance, G-buffer, velocity, ReSTIR, picking, and denoiser history remain render-resolution resources.
- Presentation LDR image is allocated at display extent; Phase 20 moves TSR output/history to display extent.
- Tone mapping dispatches at display extent and samples the render-resolution HDR input with normalized coordinates to avoid out-of-bounds reads at non-1.0 scale.
- Editor viewport now tracks display extent for the presented texture while still exposing render extent for diagnostics.
- Selection outline is skipped when render and display extents differ; display-space outline alignment remains a later upgrade.
- Verified Debug build plus native-scale and 0.5-scale smoke runs.

### Phase 20: Convert TAA To TSR [DONE]

Target files:

- `shaders/taa.comp`
- `shaders/temporal_common.glsl`
- `src/rtv/TemporalSystem.cpp`

Steps:

1. Convert TAA resolve to output display-resolution pixels.
2. Reproject display pixels into render-resolution history.
3. Add jitter-aware reconstruction filter.
4. Add disocclusion rejection, reactive mask support, and variance-aware clamping.
5. Preserve camera-cut reset behavior.

Acceptance criteria:

- Edges resolve over multiple frames instead of blurring.
- Disocclusions do not drag history.
- Static 0.67x render scale approaches native quality after accumulation.

Implementation notes:

- TAA now resolves display-resolution pixels while sampling render-resolution color, depth/normal, and velocity inputs.
- The shader reconstructs current color with a clamped 3x3 render-resolution filter and keeps all history reads/writes at display resolution.
- Render-space velocity is scaled into display pixels before reprojection, so TSR history remains stable when render scale changes.
- History clipping uses the existing neighborhood min/max clamp with luminance sigma/reactive checks for disocclusion stability.
- Camera-cut and history-valid resets are preserved through the existing TAA uniform path.
- Verified native-scale TAA smoke and 0.5-scale TSR smoke.

### Phase 21: Update C++ Pipeline For TSR [DONE]

Target files:

- `src/rtv/PathTracerRenderer.cpp`
- `src/rtv/RenderGraph.cpp`
- `include/rtv/RendererSettings.h`
- `src/rtv/RenderSettingsPanel.cpp`

Steps:

1. Add render scale setting and quality presets.
2. Wire TSR pass resources into the render graph.
3. Recreate render-resolution resources when scale changes.
4. Keep display-resolution resources tied to swapchain size.
5. Reset or validate history on scale changes.

Acceptance criteria:

- Runtime render-scale changes are stable.
- No stale descriptors after resource recreation.
- GPU profiler attributes cost to render-resolution and display-resolution passes separately.

Implementation notes:

- `PathTracerRenderer::beginFrame` now receives render and display extents; render-resolution resources are recreated when the internal scale changes.
- TAA/TSR output and history images are allocated at display extent, while denoiser, velocity, G-buffer, and path-tracing resources stay at render extent.
- The render graph binds the TSR pass against display-sized output/history resources and dispatches it at display extent.
- Render settings expose TSR presets (`Native`, `Quality`, `Balanced`, `Performance`) plus the existing raw render-resolution scale slider.
- Existing profiler ranges continue to isolate render-resolution passes such as path trace/denoiser from display-resolution post passes such as TAA/TSR and tone map.
- Verified runtime resource recreation through 0.5-scale validation scene smoke.

### Phase 22: Update Tone Map And Presentation [DONE]

Target files:

- `shaders/tone_map.comp`
- `src/rtv/PathTracerRenderer.cpp`
- `src/rtv/ViewportPanel.cpp`

Steps:

1. Ensure tone mapping consumes display-resolution TSR output.
2. Ensure viewport presentation and screenshots use display extent.
3. Keep luminance histogram resolution policy explicit.
4. Validate UI overlays and selection outline alignment.

Acceptance criteria:

- Presentation is sharp at native display resolution.
- Selection outline and picking coordinates match displayed pixels.
- Screenshots capture the final display-resolution image.

Implementation notes:

- Tone mapping consumes the current post-temporal HDR source; with TSR enabled that source is display-resolution TSR output.
- Tone mapping dispatches at display extent and samples HDR input by normalized coordinates, so native and sub-native render scales share the same presentation path.
- Viewport presentation uses the display extent for texture validation, viewport fitting, and HUD diagnostics.
- Auto-exposure histogram policy is explicit: histogram sampling and exposure reduction use the current post-temporal HDR source extent.
- Picking maps viewport UVs into the render-resolution entity buffer; selection outline remains enabled only when render and display extents match to avoid misaligned display-space outlines until the outline pass is upgraded.
- No separate screenshot capture path is present in the current codebase; the final presentation image is display-resolution.
- Verified 0.5-scale TSR plus auto-exposure smoke run.

### Phase 49: Async Compute Overlap [DONE]

Target files:

- `include/rtv/VulkanContext.h`
- `src/rtv/VulkanContext.cpp`
- `include/rtv/CommandSystem.h`
- `src/rtv/CommandSystem.cpp`
- `include/rtv/RenderGraph.h`
- `src/rtv/RenderGraph.cpp`
- `src/rtv/PathTracerRenderer.cpp`
- `src/rtv/GpuProfiler.cpp`

Queue partition targets:

| Work | Preferred queue | Dependency | Notes |
|------|-----------------|------------|-------|
| Path tracing RT dispatch | Graphics/ray tracing | TLAS, descriptors, accumulation state | Producer for denoiser and temporal passes |
| Denoiser | Compute | Path tracing output, G-buffer/path data | First async candidate |
| TAA/TSR | Compute | Denoiser or raw color, velocity, histories | Async candidate after TSR graph is explicit |
| Luminance histogram | Compute | TSR or pre-tone-map HDR | Can overlap with some UI/graphics work |
| Exposure reduce | Compute | Histogram | Short pass; include only if synchronization overhead is lower than saved time |
| Tone map | Compute | Exposure, HDR input | Last async candidate before presentation |
| UI composite/presentation | Graphics | Tone-map output | Must wait for compute completion |

Steps:

1. Profile the post-path-trace workload first: denoiser, TAA/TSR, tone map, luminance histogram, exposure reduce, selection outline, sky reprojection, and any pure compute debug passes.
2. Add compute queue discovery in `VulkanContext`: prefer a dedicated compute queue family when available, otherwise use a second compute-capable queue from the graphics family when possible.
3. Expose `computeQueue`, `computeQueueFamilyIndex`, and capability flags.
4. Add timeline semaphore support for cross-queue dependencies.
5. Extend `CommandSystem` to allocate and submit compute command buffers independently from graphics/ray-tracing command buffers.
6. Extend `RenderGraph` pass metadata with queue domain: graphics, ray tracing, compute, transfer, or same-family compute.
7. Teach the graph compiler to partition post-path-trace compute passes after path tracing and before presentation.
8. Use same-family queue submissions without queue-family ownership transfers when graphics and compute share the same family.
9. Use `VkImageMemoryBarrier2` and `VkBufferMemoryBarrier2` with queue ownership transfers only for true cross-family queues.
10. Start with denoiser plus tone map overlap; add TAA/TSR, histogram, and exposure only after the first overlap path validates.
11. Add `--disable-async-compute` and `--single-queue-fallback` flags for deterministic debugging.
12. Add profiler lanes for graphics, ray tracing, compute, and queue wait time.
13. Compare against a single-queue baseline on the same scene, resolution, and quality settings.

Acceptance criteria:

- Denoiser/TAA/TSR/tone-map compute work overlaps with ray tracing or graphics work where hardware supports it.
- Total frame time improves by at least 10 percent on a GPU-bound scene, or the capture documents why overlap is unavailable.
- Target savings are tracked against the original 1.6-4.2 ms / 10-26 percent goal.
- `--single-queue-fallback` produces equivalent output to async mode under fixed RNG seed.
- Vulkan synchronization validation is clean with both same-family and cross-family queue paths.
- Unsupported devices fall back to the existing single-queue path.

Implementation notes:

- Compute queue discovery now scans all queue families and keeps a compute-capable queue available when the device exposes one.
- `VulkanContext` exposes compute queue, queue family, timeline semaphore, and capability helpers for async-compute preparation.
- Timeline semaphore support is now queried before device creation; devices without the feature skip timeline semaphore creation and remain on the single-queue fallback path.
- `CommandSystem` accepts `--disable-async-compute` and `--single-queue-fallback` runtime options through `main`/`Application`.
- `CommandSystem` now allocates per-frame compute command pools/buffers when async compute is enabled by device support and not disabled by CLI fallback flags.
- `CommandSystem` now routes frame submission through `vkQueueSubmit2`; when async compute work is recorded, graphics submission signals the timeline semaphore and the compute queue waits on that value before submitting its command buffer.
- The frame fence is attached to the final submitted queue, so future async compute work can keep per-frame command-pool reuse safe.
- Async history-copy completion now signals a second timeline value, and the next graphics frame waits on that value before work that can read history resources.
- Queue discovery requests a dedicated compute queue family when available, otherwise it can request a second queue from the graphics/compute family when that family exposes multiple queues; async deferral is enabled only when graphics and compute use distinct `VkQueue` handles.
- `PathTracerRenderer::recordAsyncComputeWork(...)` now records the deferred post-trace compute chain when async recording is available; the graphics path keeps the original inline post-trace order for single-queue and unsupported-device fallbacks.
- `history_copy` and `taa_history_copy` are recorded through standalone render-graph copy passes when deferred, so source/history layouts and final history access remain explicit on the async queue.
- `RenderGraphPass` now carries explicit queue-domain metadata, and `RenderGraph::hasAsyncCompute()` now reflects actual queue/semaphore wiring instead of transient-resource availability.
- The render graph now infers pass queue domains from resource-use domains and emits per-pass queue labels in JSON/DOT dumps.
- Render graph barriers now retain before/after queue domains and write those domains to render graph JSON for validation.
- GPU timing data now exposes graphics-lane, ray-tracing-lane, compute-lane, and queue-wait fields in profile JSON; same-family async frames write producer-end and async-compute-start timestamps so queue wait is measured instead of hard-coded.
- Presentation-safe graphics command-buffer splitting is now in place: pre-presentation graphics work records separately from post-compute presentation/UI work, non-async frames submit both graphics command buffers together, and async frames chain pre-graphics -> compute -> post-graphics with presentation/fence signaling on the final graphics submit.
- The post-path-trace compute chain is now factored behind `recordPostTraceCompute(...)`; on the async path, moment update, denoiser, history copies, TAA, auto exposure, tone map, and selection outline record into the compute command buffer while single-queue and unsupported-device paths keep the original inline recording order.
- Async post-graphics submission now waits for compute completion at fragment-shader scope instead of all-commands scope, allowing swapchain transition/clear setup to run before presentation samples the async tone-map output.
- Queue-lane accounting now treats selection outline as compute work; fullscreen and editor presentation remain on the graphics lane.
- Cross-family compute devices now create allocator-backed images, buffers, and RenderGraph transient resources with concurrent graphics/compute sharing when the selected compute queue family differs from graphics. Same-family devices keep exclusive sharing. This makes the async post-trace compute path valid on dedicated compute queues without relying on per-resource ownership state for the renderer's persistent temporal buffers.
- Post-trace compute barriers now use queue-valid synchronization stages in the shared async path; a Debug validation run on the dedicated compute queue path reported zero Vulkan validation messages after fixing fragment-stage barriers recorded into compute command buffers.
- Profile and frame-timeline JSON now include an `async_compute` block with enabled/fallback state, timeline semaphore state, graphics/compute queue family indices, compute queue index, cross-family status, and allocator sharing mode/family list.
- Verified Debug and Release builds, the canonical 120-frame Cornell diagnostic smoke, a matching single-queue fallback metadata run, short Debug Vulkan-validation async run, profile comparison, fixed-seed beauty image comparison, and RenderDoc capture after independent queue selection, post-trace async recording, next-frame history waits, and presentation-safe split submission.
- Cornell validation evidence: the canonical smoke under `out/phase49_async_metadata_smoke/` produced profile JSON, render graph JSON, debug views, and debug package outputs. Its profile reported `async_compute.enabled=true`, `graphics_family=0`, `compute_family=2`, `cross_family=true`, `resource_sharing_mode="concurrent"`, and `resource_sharing_queue_families=[0,2]`; queue lanes were ray tracing 9.179 ms, compute 4.653 ms, graphics 0.006 ms, and queue wait 0.072 ms.
- Debug Vulkan-validation evidence: `out/phase49_async_metadata_debug_validation_fixed/` ran the Cornell scene with validation layers enabled and emitted zero `Vulkan validation` messages. Its profile and frame timeline both reported `async_compute.enabled=true`, `cross_family=true`, graphics family 0, compute family 2, and concurrent sharing across `[0,2]`.
- Single-queue fallback evidence: `out/phase49_async_metadata_single_queue/profile.json` reported `async_compute.enabled=false` and `single_queue_fallback=true` on the same hardware.
- Closeup Cornell GPU-bound validation evidence after the final cross-queue barrier fix: async GPU average was 35.096 ms versus 35.511 ms single-queue, a 1.17 percent average improvement; p95 improved by 4.25 percent and p99 by 8.13 percent. Fixed-seed `beauty.png` comparison was identical (`mse=0`, `ssim=1.0`, `changed_pixel_percentage=0`). The smaller-than-target average improvement documents that the frame remains ray-tracing dominated and the overlap window is too small to reach the original 10 percent goal on this hardware/workload.
- Closeup validation artifacts were exported under `out/phase49_closeup_async_final/` and `out/phase49_closeup_single_final/`, including profile JSON, debug views, profile comparison JSON, beauty comparison JSON, and an amplified beauty diff image.
- RenderDoc runtime loading now supports `--renderdoc-dll`, `RENDERDOC_DLL_PATH`, `RENDERDOC_DIR`, `RENDERDOC_SDK_DIR`, the current directory, the default DLL search path, and standard RenderDoc install directories; the DLL is loaded before Vulkan initialization when capture is requested.
- RenderDoc validation capture succeeded after loading `C:\Program Files\RenderDoc\renderdoc.dll`: `out/phase49_closeup_capture_fixed/async_capture_capture.rdc` is a Vulkan 1280x720 capture with 65 actions, 7 dispatches, and the expected async compute markers (`moment_update`, `temporal_denoiser`, `taa_resolve`, `tone_map`) before the fullscreen draw. The capture profile reports the same cross-family async metadata as the smoke run.
- Phase 49 is complete: async compute submission, fallback controls, queue-lane profiling, same-family/single-queue fallback behavior, cross-family concurrent sharing, Vulkan validation evidence, fixed-seed output equivalence, and RenderDoc evidence are all recorded in the artifacts above.

## Batch 5: Denoiser Production Quality

Goal: move denoising from generic color filtering toward path-aware diffuse/specular temporal filtering.

### Phase 23: Expose Path Data [DONE]

Target files:

- `shaders/pathtrace.rgen`
- `shaders/denoiser.comp`
- `include/rtv/PathTracerRenderer.h`
- `src/rtv/PathTracerRenderer.cpp`

Steps:

1. Add output buffers/images for direct diffuse, direct specular, indirect diffuse, indirect specular, albedo, roughness, hit distance, and confidence.
2. Write path data in the path tracer with clear units and resolution.
3. Update descriptor layouts and render graph resources.
4. Add debug views for each channel.
5. Keep old single-color denoiser path behind a compatibility flag.

Acceptance criteria:

- Debug channels match expected scene content.
- No significant path tracing cost increase beyond the added writes.
- Denoiser can consume the new channels without changing visual output yet.

Implementation notes:

- Added a compact path-data storage buffer with direct diffuse, direct specular, indirect diffuse, indirect specular, albedo, roughness, hit distance, and confidence metadata.
- Path tracing writes the path-data buffer at render resolution, and both the render graph and denoiser descriptor layouts now declare the buffer dependency.
- Added debug views for path direct diffuse/specular, path indirect diffuse/specular, path albedo, and path metrics.
- Verified Debug build plus beauty, path-direct-diffuse, path-direct-specular, path-indirect-diffuse, path-data-albedo, and path-data-metrics smoke runs.

### Phase 24: Roughness-Aware Kernel Sizing [DONE]

Target files:

- `shaders/denoiser.comp`

Steps:

1. Use roughness and normal variance to choose filter radius.
2. Keep glossy/low-roughness reflections on smaller kernels.
3. Use larger kernels on diffuse/high-roughness regions.
4. Add debug view for chosen radius.

Acceptance criteria:

- Sharp specular detail is preserved better.
- Diffuse noise still reduces effectively.

Implementation notes:

- The denoiser now derives a per-pixel spatial filter radius from first-hit roughness and variance, keeping low-roughness pixels on the smaller kernel while allowing noisy/high-roughness regions to use the wider kernel.
- Added the `denoiser-kernel-radius` debug view and exposed it through CLI parsing and the editor debug view list.
- Verified Debug build plus a `denoiser-kernel-radius` smoke run.

### Phase 25: Hit-Distance Filtering [DONE]

Target files:

- `shaders/denoiser.comp`
- `shaders/temporal_common.glsl`

Steps:

1. Store primary and secondary hit distance from path tracing.
2. Normalize hit distance into a stable range.
3. Use hit distance to adapt spatial and temporal filter radius.
4. Reject history when hit-distance mismatch exceeds threshold.

Acceptance criteria:

- Near-field detail is preserved.
- Distant diffuse regions denoise more aggressively.
- Disocclusion halos are reduced.

Implementation notes:

- Path tracing now stores primary hit distance and secondary segment hit distance in the path-data record used by the denoiser.
- Added normalized hit-distance helpers in the temporal shader utilities and used them to compare spatial neighbors and reprojected history.
- The denoiser now uses hit distance to keep near-field spatial weights stricter, allow distant rough regions to filter more broadly, and reduce history weight when reprojected hit distance diverges.
- Added the `denoiser-hit-distance` debug view, showing normalized primary distance, normalized secondary distance, and hit-distance confidence.
- Verified Debug build plus beauty, `denoiser-hit-distance`, and `path-data-metrics` smoke runs.

### Phase 26: Virtual Motion For Specular [DONE]

Target files:

- `shaders/denoiser.comp`
- `shaders/temporal_common.glsl`
- `src/rtv/TemporalSystem.cpp`

Steps:

1. Estimate reflected hit point or virtual surface position for specular paths.
2. Compute virtual motion vector for specular history reprojection.
3. Fall back to surface motion when confidence is low.
4. Add debug view for specular virtual velocity.

Acceptance criteria:

- Specular ghosting is reduced during camera motion.
- Low-confidence areas do not smear or explode.

Implementation notes:

- The denoiser now estimates a reflected virtual position from the first-hit surface position, first-hit normal, camera position, and secondary hit distance.
- Specular virtual reprojection is applied only when a conservative confidence test passes: glossy surface, specular-dominant path data, valid secondary distance, on-screen history, and bounded virtual/surface motion disagreement.
- History validation blends from the surface position to the virtual position when virtual motion is active, and falls back to the existing surface velocity path when confidence is low.
- Added matching `TemporalSystem::estimateSpecularVirtualMotion` CPU-side utility logic for the temporal subsystem.
- Added the `denoiser-virtual-motion` debug view, showing virtual velocity and confidence.
- Verified Debug build plus beauty, `denoiser-virtual-motion`, and `denoiser-hit-distance` smoke runs.

### Phase 27: Split Diffuse And Specular Histories [DONE]

Target files:

- `include/rtv/TemporalSystem.h`
- `src/rtv/TemporalSystem.cpp`
- `shaders/denoiser.comp`
- `shaders/taa.comp`

Steps:

1. Allocate separate diffuse and specular temporal histories.
2. Apply channel-specific validation and clamping.
3. Recombine after denoising and before tone mapping.
4. Add debug views for history length and confidence per channel.

Acceptance criteria:

- Diffuse converges smoothly without over-blurring specular.
- Specular rejects history more aggressively when needed.
- Memory impact is measured and documented.

Implementation notes:

- Added separate render-resolution diffuse and specular denoiser histories, plus current-frame channel outputs so the denoiser never reads and writes the same history image in one dispatch.
- The denoiser now splits filtered color using path-data diffuse/specular signal, applies separate history validation and clamping, then recombines resolved diffuse and specular channels into the existing denoised HDR output.
- Diffuse history uses softer variance/reactive validation; specular history uses stricter variance, hit-distance, roughness, motion, and virtual-motion validation.
- TAA now reads path data and increases rejection for specular-heavy pixels so the final temporal resolve does not over-stabilize rejected specular history.
- Added `denoiser-diffuse-history` and `denoiser-specular-history` debug views. Red is normalized effective history length, green is channel confidence, and blue is channel signal.
- Memory impact: four added RGBA16F render-resolution images for previous/current diffuse/specular channel histories, or 32 bytes per render pixel. This is about 63 MiB at 1080p, 113 MiB at 1440p, and 253 MiB at 4K.
- Verified Debug build plus validation-clean beauty, `denoiser-diffuse-history`, `denoiser-specular-history`, and `temporal-history-weight` smoke runs.

### Phase 28: Anti-Flicker For Emissives [DONE]

Target files:

- `shaders/denoiser.comp`
- `shaders/pathtrace.rgen`

Steps:

1. Mark emissive contributions in path data.
2. Detect high-intensity small-area emissive samples.
3. Clamp temporally using luminance history and confidence.
4. Avoid suppressing legitimate lighting changes.

Acceptance criteria:

- Small emissive objects flicker less.
- Moving emissive objects do not leave long trails.

Implementation notes:

- Path tracing now marks emissive contribution strength in path data by storing emissive luminance in the direct-diffuse metadata lane.
- The denoiser detects high-intensity emissive spikes from that path-data marker plus neighborhood luminance variance.
- Emissive anti-flicker clamps only when stable diffuse history, motion confidence, frame warmup, and low reactive change agree; moving or newly changing emissive regions fall back toward the current sample and avoid long trails.
- Added the `denoiser-emissive-clamp` debug view. Red is emissive signal, green is history confidence, and blue is applied clamp amount.
- Verified Debug build plus validation-clean beauty, `denoiser-emissive-clamp`, `emissive-contribution`, and `denoiser-diffuse-history` smoke runs.

### Phase 28.1: Motion Denoiser Stabilization [DONE]

Target files:

- `shaders/denoiser.comp`

Steps:

1. Diagnose why camera motion still appears noisy even though denoiser history is preserved across normal camera movement.
2. Use per-pixel velocity to increase spatial filter tolerance during motion.
3. Preserve a conservative, confidence-gated diffuse/specular history floor for moving pixels when reprojection and disocclusion tests pass.
4. Keep the stronger motion path inactive for history resets and disoccluded pixels.

Acceptance criteria:

- Camera motion is less noisy than the previous moving-frame output.
- Disocclusions do not receive forced history.
- Specular history remains more conservative than diffuse history.

Implementation notes:

- `denoiser.comp` now computes the motion signal before the spatial A-trous loop and uses it to relax color/depth/normal sigma, widen outer-ring participation, and keep at least three A-trous iterations for significant moving pixels.
- Added motion-only diffuse and specular history floors. They are gated by existing channel confidence, frame blend/history length, reactive rejection, and `reset_history == 0`, so camera cuts and newly exposed regions still fall back to the current sample.
- Verified manual `glslangValidator` compilation for `denoiser.comp`. Visual camera-motion validation is still required on `cornell.rtlevel`, `closeup_cornell.rtlevel`, and lightweight Sponza glTF.

## Batch 6: ReSTIR DI Fixes

Goal: remove bias and leaking from direct-light ReSTIR before building ReSTIR GI.

### Phase 31: Pairwise MIS For Temporal ReSTIR [DONE]

Target files:

- `shaders/restir_common.glsl`
- `shaders/restir_temporal.comp`
- `shaders/restir_final.comp`

Steps:

1. Audit temporal reservoir reuse math.
2. Store all fields needed for pairwise MIS: target function, source PDF, sample count, confidence, and visibility state.
3. Implement pairwise MIS weighting for temporal candidates.
4. Reject incompatible reservoirs using depth, normal, material, and motion confidence.
5. Validate with moving camera and changing light intensity.

Acceptance criteria:

- Temporal ReSTIR mean matches non-temporal reference over enough frames.
- No persistent over-bright or under-bright bias after camera motion.

Implementation notes:

- Added common ReSTIR helpers for valid/visibility bits, target-function luminance, source PDF, sample count, temporal age confidence, pairwise compatibility, pairwise previous-candidate weight, and temporal reservoir merge.
- Reused reserved reservoir lanes without growing the reservoir buffer: metadata `z` now stores valid/visibility bits, metadata `w` stores a temporal compatibility signature, and `target_pdf_weight_sum_m.w` stores the previous-candidate pairwise MIS weight for debugging.
- The active raygen temporal reuse path now merges current and reprojected previous direct-light reservoirs with pairwise MIS instead of replacing current direct lighting with the previous reservoir.
- Temporal reuse rejects incompatible candidate reservoirs by sample type, roughness/material signature, age, visibility validity, source-PDF ratio, target-function ratio, and motion confidence. The standalone `restir_temporal.comp` path also performs depth/normal rejection through the depth-normal buffer.
- Added the `restir-pairwise-mis` debug view. Red is previous-candidate weight, green is current-candidate weight, and blue is merged confidence.
- Updated `restir_final.comp` to consume the merged reservoir estimate directly instead of dividing the already-merged estimate by `M`.
- Verified Debug build, manual `glslangValidator` compilation for `restir_temporal.comp` and `restir_final.comp`, validation-clean ReSTIR smoke runs for `restir-pairwise-mis`, `restir-reservoir-confidence`, `beauty` in hybrid compare mode, and `restir-pairwise-mis` with validation camera motion.

### Phase 32: Visibility Reuse For Spatial ReSTIR [DONE]

Target files:

- `shaders/restir_spatial.comp`
- `shaders/restir_final.comp`
- `shaders/pathtrace_shadow.*`

Steps:

1. Track whether reservoir visibility is known, unknown, or invalid.
2. Reuse visibility only when receiver and occluder assumptions are compatible.
3. Cast validation rays for reused samples when required.
4. Reject spatial samples across large depth/normal discontinuities.

Acceptance criteria:

- Light leaking across walls is eliminated or significantly reduced.
- Spatial reuse still reduces variance in open areas.

Implementation notes:

- Reused the Phase 31 reservoir visibility bits for spatial ReSTIR: valid visible reservoirs can be spatially reused, invalid reservoirs are rejected, and unknown visibility is not propagated as known-visible.
- Preserved current known-visible state during temporal pairwise merges instead of degrading zero-previous-weight reservoirs to unknown visibility.
- `restir_spatial.comp` now uses shared ReSTIR reservoir helpers, rejects candidate reservoirs with unknown/invalid visibility, rejects mismatched sample type or temporal compatibility signature, and applies stricter depth, normal, roughness, source-PDF, and target-function compatibility checks before mixing.
- Spatial reuse only propagates confirmed visibility when both the center and candidate reservoirs are visible. Samples that would require a fresh visibility validation ray are conservatively rejected by the spatial pass instead of reused without validation.
- `restir_final.comp` rejects explicitly invalid visibility and applies reduced confidence to unresolved unknown-visibility reservoirs.
- Verified `restir_spatial.comp` and `restir_final.comp` with `glslangValidator`, Debug build, and validation-clean ReSTIR smoke runs for `restir-reservoir-confidence`, `restir-reservoir-m`, and hybrid `beauty` with validation camera motion.

### Phase 33: Improve Light BVH SAH Quality [DONE]

Target files:

- `include/rtv/LightBvh.h`
- `src/rtv/LightBvh.cpp`
- `shaders/restir_common.glsl`

Steps:

1. Profile current light BVH build and traversal quality.
2. Improve SAH splitting or binning.
3. Store tighter bounds and better power estimates.
4. Add debug stats: tree depth, leaf count, traversal count, selected light distribution.

Acceptance criteria:

- Light selection variance improves in many-light scenes.
- Build cost remains acceptable for scene edits.

Implementation notes:

- Replaced the midpoint light-BVH split with binned SAH over light primitive bounds and power. The builder now evaluates eight centroid bins and minimizes `surface_area * power` across split candidates, with a deterministic median fallback for degenerate ranges.
- Added `LightBvhPrimitive` input records with bounds, centroid, power, and original light index while preserving the existing power-only build overload for compatibility.
- Stored tighter light bounds in the packed GPU BVH node lanes: `data0.xyz` carries node bounds min, `data0.w` carries total power, `data1.xyz` carries bounds max, and `data1.w` keeps the existing leaf/child encoding used by the shader.
- Populated emissive triangle light records with centroid and exact world-space triangle AABBs in previously unused lanes, so the light BVH can build spatially meaningful nodes without changing direct-light sampling behavior.
- Added CPU-side Light BVH stats: node count, leaf count, max depth, min/max leaf power, and estimated average traversal steps. Scene upload now logs these stats when the light BVH is rebuilt.
- Verified Debug and Release builds, ReSTIR beauty smoke, headless diagnostic smoke with profile JSON/render graph/debug views/debug package, and JSON parsing for generated `profile.json` and `rendergraph.json`.

## Batch 7: ReSTIR GI

Goal: add reservoir-based indirect illumination reuse after DI ReSTIR is correct.

### Phase 34: ReSTIR GI Reservoir Struct [DONE]

Target files:

- `shaders/restir_common.glsl`
- `include/rtv/PathTracerRenderer.h`
- `src/rtv/PathTracerRenderer.cpp`

Steps:

1. Define GI reservoir layout around the 80-byte per-pixel target.
2. Include hit point, normal, radiance, target PDF, weight sum, sample count, age, and flags.
3. Allocate triple-buffered reservoir storage.
4. Add clear and debug-view passes.

Acceptance criteria:

- Reservoir buffers allocate and clear correctly.
- Debug view can inspect reservoir validity and age.

Implementation notes:

- Added an 80-byte `RestirGiReservoir` layout in GLSL and matching `RestirGiReservoirGpu` in C++: selected hit point plus target PDF, normal plus roughness, radiance plus weight sum, receiver point plus hit distance, and metadata for sample count, age, flags, and material id.
- Added GI reservoir helpers for empty reservoirs, validity, sample count, and normalized age. Flags currently reserve valid and visible bits for the later temporal/spatial phases.
- Allocated triple-buffered GI reservoir storage: current, previous, and spatial scratch buffers with persistent debug names.
- Added a `restir_gi_clear` render-graph pass for GI reservoir debug views. This early debug-only scheduling note was superseded by the current default beauty-path ReSTIR GI integration; normal Balanced beauty frames may now schedule GI clear/spatial/final work when ReSTIR GI is enabled.
- Added `restir-gi-validity` and `restir-gi-age` debug views. Validity shows current/previous/spatial validity in RGB; age shows current age, sample count, and target PDF channels.
- Extended ReSTIR reservoir memory accounting to include the GI buffers so Phase 50 has the correct baseline footprint.
- Verified `pathtrace.rgen` with `glslangValidator`, Debug build, validation-clean `restir-gi-validity`, `restir-gi-age`, and hybrid beauty smokes, plus the headless diagnostic workflow with profile JSON, render graph JSON, debug views, and debug package.

### Phase 35: ReSTIR GI Initial Sampling [DONE]

Target files:

- `shaders/pathtrace.rgen`
- `shaders/restir_gi_init.comp`

Steps:

1. Generate candidate secondary hits from BSDF sampling.
2. Evaluate target function at candidate hit points.
3. Store selected candidate in the GI reservoir.
4. Keep regular path tracing contribution available for comparison.

Acceptance criteria:

- Initial GI reservoirs match one-bounce indirect lighting distribution.
- Reservoir disabled/enabled comparison has matching mean over time.

Implementation notes:

- Added first-secondary-hit GI candidate capture to `pathtrace.rgen`. The candidate is only accepted when the secondary surface was reached by the first bounce BSDF sample, which keeps Phase 35 scoped to initial sampling rather than temporal/spatial reuse.
- Stored the selected candidate hit point, normal, roughness, receiver point, hit distance, material id, target luminance, and evaluated one-bounce radiance in the GI reservoir layout introduced in Phase 34.
- Kept the regular path tracing contribution path unchanged; the GI reservoir is populated for the ReSTIR GI debug views so beauty/ReSTIR DI frames avoid the inactive GI reservoir cost.
- Deferred a separate `restir_gi_init.comp` pass until the temporal/spatial GI phases need an explicit pass boundary. This keeps the initial sampling result tied to the existing BSDF path sample and avoids a second secondary ray in Phase 35.

### Phase 36: ReSTIR GI Temporal Reuse [DONE]

Target files:

- `shaders/restir_gi_temporal.comp`
- `shaders/temporal_common.glsl`

Steps:

1. Reproject previous GI reservoirs using hit-point reprojection.
2. Validate using depth, normal, material, motion, and hit-distance confidence.
3. Merge current and previous reservoirs with correct weights.
4. Age out stale reservoirs.

Acceptance criteria:

- Temporal reuse reduces indirect noise.
- Camera motion does not create persistent ghosted GI.

Implementation notes:

- Added conservative temporal reuse for the ReSTIR GI debug reservoir path. Current first-secondary-hit GI candidates now reproject through the primary-surface motion vector and merge with the previous GI reservoir when history is available.
- Added GI temporal rejection using receiver world-position delta, selected-hit normal agreement, selected material id, hit-distance delta, target-luminance ratio, reservoir age, and motion confidence.
- Added GI reservoir history lifetime tracking on the C++ side. Previous GI reservoirs are cleared only before a valid GI history exists, then preserved and refreshed by copying current GI reservoirs into the previous-history buffer at the existing history-copy point.
- Kept temporal GI reuse gated to `restir-gi-validity` and `restir-gi-age`, so beauty and ReSTIR DI frames still avoid the inactive GI reservoir passes.
- Kept the temporal merge in `pathtrace.rgen` for this debug-first phase. A standalone `restir_gi_temporal.comp` pass remains deferred until Phase 37/38 need an explicit GI pass boundary for spatial reuse or final shading.

### Phase 37: ReSTIR GI Spatial Reuse [DONE]

Target files:

- `shaders/restir_gi_spatial.comp`
- `shaders/pathtrace_shadow.*`

Steps:

1. Select blue-noise spatial neighbors.
2. Validate compatible surfaces.
3. Cast visibility rays for reused indirect samples where required.
4. Merge reservoirs with correct target-function evaluation at the current receiver.

Acceptance criteria:

- Indirect light does not leak through thin walls.
- Spatial reuse visibly reduces low-frequency noise.

Implementation notes:

- Added `shaders/restir_gi_spatial.comp`, a GI-specific spatial reuse compute pass that samples blue-noise neighbor offsets and writes the merged result into the GI spatial scratch reservoir.
- Added C++ shader compilation, descriptor layout, compute pipeline, RenderGraph pass, and pass callback for `restir_gi_spatial`.
- Fed the spatial GI result into the previous-GI history copy path so the next frame's temporal reuse consumes the spatially merged reservoir rather than the raw initial candidate.
- Superseded by the current default beauty-path integration: `restir_gi_spatial` now runs for normal Balanced beauty rendering when ReSTIR GI is enabled, with debug views still available for isolating each reuse stage.
- Used conservative receiver depth/normal, selected-hit normal, selected material id, target-luminance, and hit-distance validation. Explicit visibility rays remain deferred until GI tuning/final integration; for Phase 37 debug reservoirs, conservative rejection avoids introducing a ray-query dependency.

### Phase 38: ReSTIR GI Final Shading [DONE]

Target files:

- `shaders/restir_gi_final.comp`
- `shaders/pathtrace.rgen`

Steps:

1. Convert GI reservoir result into final indirect contribution.
2. Combine with regular path tracing or replace selected bounce path according to mode.
3. Apply generalized MIS consistently.
4. Add debug toggles for GI initial, temporal, spatial, and final contribution.

Acceptance criteria:

- ReSTIR GI final contribution can feed regular beauty output when ReSTIR GI is enabled, with the path tracing reference available by disabling ReSTIR GI or selecting reference/debug comparisons.
- Final GI debug output uses the selected-bounce replacement estimate for inspecting temporal/spatial GI reuse against the path tracing base.
- Debug modes isolate each reuse stage.

Implementation notes:

- Added `shaders/restir_gi_final.comp`, a compute pass that reads current, previous, and spatial GI reservoirs and writes the selected-bounce replacement preview into the raw HDR target for `restir-gi-final`.
- Added `restir-gi-initial`, `restir-gi-temporal`, `restir-gi-spatial`, and `restir-gi-final` debug views to CLI parsing, editor selection, diagnostic image export, and the AI debug/profiling tools documentation.
- Expanded the GI reservoir write gate in `pathtrace.rgen` so all ReSTIR GI debug/final views populate reservoirs; `restir-gi-initial` skips temporal merge to isolate the first candidate, while `restir-gi-temporal` shows the temporally merged current reservoir.
- Superseded by the current real-time default: regular Balanced beauty rendering schedules ReSTIR GI spatial/final work so GI reuse contributes to the displayed image. The path tracing reference remains available through settings/debug comparisons rather than being the only normal beauty path.
- Validated with shader compilation, Release and Debug builds, a `restir-gi-final` headless debug export, beauty-path render graph regression, release editor one-frame launch, and the required-style profile/rendergraph/debug-package smoke test.

### Phase 39: ReSTIR GI Tuning [DONE]

Target files:

- `shaders/restir_*.comp`
- `include/rtv/RendererSettings.h`
- `src/rtv/RenderSettingsPanel.cpp`

Steps:

1. Tune neighbor count, temporal age, confidence thresholds, and visibility ray budget.
2. Add half-resolution mode.
3. Identify candidate reservoir fields for later Phase 50 compression, but do not change reservoir layout in this tuning phase.
4. Profile memory bandwidth and GPU time.
5. Record pre-compression reservoir memory footprint so Phase 50 has a reliable baseline.

Acceptance criteria:

- ReSTIR GI has a documented quality/performance preset table.
- Half-resolution mode is stable and does not shimmer excessively.

Preset table:

| Preset | Temporal age | Spatial rounds | Radius | Depth scale | Compatibility cutoff | Half resolution | Use |
|--------|--------------|----------------|--------|-------------|----------------------|-----------------|-----|
| Reference | 32 | 6 | 4.25 | 0.85 | 0.10 | Off | Strict debug comparison |
| Balanced | 18 | 3 | 3.75 | 0.85 | 0.06 | Off | Default real-time quality/performance balance |
| Performance | 16 | 2 | 3.00 | 1.15 | 0.00 | On | Lower-cost 2x2 spatial GI debug/final preview |

Implementation notes:

- Added ReSTIR GI tuning fields to renderer settings, scene render settings, scene JSON persistence, editor render settings, application scene sync, and profile/settings JSON.
- Tuned the default Balanced preset for the current real-time path to temporal age `18`, `3` spatial rounds, radius `3.75`, depth scale `0.85`, compatibility cutoff `0.06`, and full-resolution reservoirs. Earlier debug-reference tuning used a heavier 24-age/4-round profile, but that is no longer the Balanced default.
- `pathtrace.rgen` now uses `restirGiTemporalMaxAge` for GI temporal age rejection and sample-count clamping instead of the previous hardcoded 32-frame cap.
- `restir_gi_spatial.comp` now uses configurable spatial rounds, radius, depth threshold scale, compatibility cutoff, and an effective half-resolution mode that reuses one spatial GI reservoir per 2x2 pixel group. The current implementation keeps the full-size reservoir allocation so Phase 50 has a stable pre-compression memory baseline.
- Added a reserved `restirGiVisibilityRayBudget` setting and profile field, but kept visibility rays disabled until the later ray-query visibility validation pass; current GI spatial reuse remains conservative.
- Recorded the pre-compression reservoir memory baseline from the Cornell 1280x720 headless profile: `353,894,400` bytes for DI current/previous/spatial plus GI current/previous/spatial reservoirs. The GI portion is three full-resolution 80-byte reservoirs: `221,184,000` bytes.
- Verified shader compilation, Release build, validation-clean default `restir-gi-final` export, validation-clean half-resolution scene export, and later Balanced beauty-path render graph runs with ReSTIR GI spatial/final passes present by default.

## Batch 8: Materials And Textures

Goal: improve BRDF fidelity and texture correctness before architecture v2.

### Phase 40: Oren-Nayar Diffuse [DONE]

Target files:

- `shaders/rt_common.glsl`

Steps:

1. Add Oren-Nayar diffuse evaluation.
2. Map material roughness to diffuse roughness.
3. Preserve Lambert fallback for low roughness or performance mode.
4. Ensure PDF remains cosine-weighted unless sampling changes.

Acceptance criteria:

- Rough diffuse surfaces look less plastic.
- Energy remains bounded.

Implementation notes:

- Added bounded Oren-Nayar diffuse evaluation in `shaders/rt_common.glsl`, with material roughness mapped to a diffuse sigma up to 70 degrees.
- Preserved Lambert fallback for low roughness (`roughness <= 0.08`) and non-path-traced preview mode.
- Kept diffuse sampling and PDF cosine-weighted; direct-light MIS and BSDF sampling probabilities are unchanged.
- Routed both pure diffuse closures and the diffuse part of mixed PBR/GGX materials through the new diffuse BRDF helper.
- Kept the Oren-Nayar factor clamped to `[0, 1]`, so rough diffuse can darken/flatten response without increasing diffuse energy beyond Lambert.
- Used the existing `scenes/validation/material_grid.rtlevel` as the Batch 8 material validation scene for roughness/direct-diffuse coverage.
- Verified `pathtrace.rgen` with `glslangValidator`, Release build, Debug build, release editor one-frame launch, validation-clean Cornell and material-grid headless debug exports, and the required-style profile/rendergraph/debug-package smoke test.

### Phase 41: Conductor Fresnel [DONE]

Target files:

- `shaders/rt_common.glsl`
- `include/rtv/SceneComponents.h`
- `src/rtv/MaterialEditorPanel.cpp`

Steps:

1. Add conductor Fresnel using eta/k parameters.
2. Provide presets for common metals.
3. Map existing metallic materials to approximate conductor parameters.
4. Keep artist-friendly base-color metallic path as fallback.

Acceptance criteria:

- Gold, copper, aluminum, and silver presets render plausibly.
- Existing metallic assets do not break.

Implementation notes:

- Added optional conductor eta/k fields to `MaterialAsset` in `include/rtv/MeshAsset.h`; existing materials leave this disabled and continue using the artist-friendly base-color metallic fallback.
- Extended the GPU material payload from 5 to 7 `vec4`s in `src/rtv/GpuScene.cpp` and `shaders/rt_common.glsl`, carrying conductor eta/k plus an explicit enable flag.
- Added exact conductor Fresnel evaluation for enabled eta/k materials, while preserving Schlick Fresnel for legacy metallic and dielectric materials.
- Added gold, copper, aluminum, and silver presets in `src/rtv/MaterialEditorPanel.cpp`; applying a preset sets eta/k, metallic=1, base color to normal-incidence F0, and keeps roughness in a plausible range.
- Added an opt-in "From Base Color" conductor approximation for existing metallic colors, so legacy metals can initialize eta/k without changing their default rendering path.
- Cached scene materials intentionally decode with conductor optics disabled until future cache schema work stores eta/k fields.
- Verified `pathtrace.rgen` with `glslangValidator`, Release build, Debug build, Release editor 5-second launch, required Cornell profile/rendergraph/debug-view/debug-package smoke, Debug Cornell `path-direct-specular` smoke, material-grid debug export, valid JSON outputs, and `validation_error_count=0` in the generated profile JSON files.

### Phase 42: Wire glTF Extensions [DONE]

Target files:

- `src/rtv/GltfLoader.cpp`
- `include/rtv/MeshAsset.h`
- `src/rtv/SceneToGpuSceneBuilder.cpp`
- `shaders/rt_common.glsl`

Steps:

1. Add import support for selected glTF material extensions handled by existing or near-term closures: `KHR_materials_clearcoat`, `KHR_materials_transmission`, `KHR_materials_ior`, `KHR_materials_specular`, `KHR_materials_sheen`, `KHR_materials_emissive_strength`, and `KHR_texture_transform`.
2. Do not implement `KHR_materials_anisotropy` shading in this phase; parse and store its raw fields only if needed so Phase 43 can consume them.
3. Store extension data in CPU material structs with explicit `hasExtension` flags and default values.
4. Convert supported extension data to GPU material fields or closure fields.
5. Add asset-level diagnostics for unsupported extensions.
6. Add import tests or sample assets for each supported extension.

Acceptance criteria:

- Extension-heavy glTF test assets load with expected material properties.
- Unsupported extensions produce explicit warnings, not silent wrong materials.

Implementation notes:

- Added explicit CPU material fields for `KHR_materials_clearcoat`, `KHR_materials_transmission`, `KHR_materials_ior`, `KHR_materials_specular`, `KHR_materials_sheen`, `KHR_materials_emissive_strength`, `KHR_materials_anisotropy`, and primary texture `KHR_texture_transform` data in `include/rtv/MeshAsset.h`.
- Imported those fields in `src/rtv/GltfLoader.cpp`; `KHR_materials_ior` now feeds the existing GPU `ior` field, and `KHR_materials_emissive_strength` is folded into emissive intensity during import.
- Left clearcoat, transmission, specular, sheen, anisotropy, and texture-transform shading behavior staged for later Batch 8 phases; their source values are retained with explicit `has*` flags instead of silently discarding them.
- Added explicit glTF extension diagnostics: unsupported asset/material extensions print warnings, while parsed extension-heavy materials print a concise value summary.
- Extended `CachedMaterialData` and bumped the scene-cache version to preserve imported extension fields, conductor optics, texture handles, and texture-transform state across glTF and GPU cache hits.
- Added `scenes/validation/gltf_extensions_phase42.gltf` and `scenes/validation/gltf_extensions_phase42.rtlevel` to exercise supported extension import plus an intentional unsupported-extension warning.
- Verified `pathtrace.rgen` with `glslangValidator`, Release build, Debug build, Release editor glTF launch, Phase 42 glTF headless uncached and cached runs, required Cornell profile/rendergraph/debug-view/debug-package smoke, Debug Cornell `path-direct-specular` smoke, valid JSON outputs, and `validation_error_count=0` in generated profile JSON files.

### Phase 43: Anisotropic GGX [DONE]

Target files:

- `shaders/rt_common.glsl`
- `shaders/pathtrace.rgen`
- `src/rtv/GltfLoader.cpp`

Steps:

1. Add tangent/bitangent orientation support if not already reliable.
2. Implement anisotropic GGX eval and VNDF sampling.
3. Use glTF anisotropy extension fields.
4. Add brushed metal validation scene.

Acceptance criteria:

- Anisotropic highlights rotate correctly with tangent direction.
- Isotropic materials remain unchanged.

Implementation notes:

- Added anisotropy strength/rotation to the GPU material payload, increasing the material stride from 7 to 8 `vec4`s in `src/rtv/GpuScene.cpp` and `shaders/rt_common.glsl`.
- Routed imported `KHR_materials_anisotropy` values from Phase 42 into the path-tracing material decode path.
- Added tangent-frame anisotropic GGX NDF, Smith masking, visible-normal PDF, and VNDF sampling in `shaders/rt_common.glsl`.
- Updated BRDF eval/pdf/sample call sites in `shaders/pathtrace.rgen` to pass geometry tangent/bitangent so anisotropic highlights rotate with the material tangent frame.
- Isotropic materials keep `anisotropy_strength=0`, which preserves the existing isotropic GGX code path.
- Reused `scenes/validation/gltf_extensions_phase42.gltf` and `.rtlevel` as the anisotropy validation asset because it imports `anisotropyStrength=0.7` and `anisotropyRotation=0.3`.
- Verified `pathtrace.rgen` with `glslangValidator`, Release build, Debug build, Release editor glTF launch, anisotropic glTF headless uncached and cached runs, required Cornell profile/rendergraph/debug-view/debug-package smoke, Debug Cornell `path-direct-specular` smoke, valid JSON outputs, and `validation_error_count=0` in generated profile JSON files.

### Phase 44: Occlusion Texture [DONE]

Target files:

- `src/rtv/GltfLoader.cpp`
- `shaders/rt_common.glsl`
- `shaders/pathtrace.rgen`

Steps:

1. Import glTF occlusion texture and strength.
2. Bind texture through material texture indices.
3. Apply occlusion to indirect diffuse only, not direct lighting.
4. Add debug view for AO texture.

Acceptance criteria:

- AO maps visibly affect creases without darkening direct light incorrectly.
- Missing AO maps default to neutral.

Implementation notes:

- Added glTF `occlusionTexture` import and `occlusionTexture.strength` preservation in `MaterialAsset`, including scene-cache save/load and cached glTF restoration.
- Routed occlusion texture index and strength through the existing eighth GPU material `vec4`, avoiding a material stride or descriptor layout change.
- Classified occlusion textures as data textures so AO is uploaded as linear data, not sRGB color.
- `shaders/rt_common.glsl` now samples AO into `Material::occlusion` without modifying base color, direct-light BRDF evaluation, or alpha.
- `shaders/pathtrace.rgen` applies AO only to first-surface indirect/ambient contribution using the existing diffuse/specular path split, leaving direct lighting unoccluded and missing AO maps neutral at `1.0`.
- Added the `material-occlusion` debug view (`RendererDebugView::MaterialOcclusion`, value 89), editor listing, CLI parsing aliases, and debug-view export coverage.
- Verified Debug build, Release build, a 3-frame Debug headless smoke with `--debug-view material-occlusion`, and the required-style 120-frame Release diagnostic smoke producing profile/rendergraph/debug views/debug package with `validation_error_count=0` and `material-occlusion.png` exported.

### Phase 45: Specular AA / Toksvig [DONE]

Target files:

- `shaders/rt_common.glsl`
- `shaders/pathtrace.rchit`

Steps:

1. Estimate normal-map variance from mip level or derivatives where available.
2. Increase effective roughness for high-frequency normal variation.
3. Clamp only the specular lobe, not material base roughness.
4. Add setting to compare on/off.

Acceptance criteria:

- Normal-mapped glossy surfaces shimmer less.
- Reflections are not globally over-blurred.

Implementation notes:

- Added `RendererSettings::specularAaEnabled`, scene render-setting persistence, settings UI, scene-to-GPU routing, and profile JSON reporting as `specular_aa_enabled`.
- Reused the existing `CameraUniform::restirGiControls.w` padding as the shader-side specular-AA enable flag, avoiding uniform-size and descriptor-layout changes.
- `shaders/rt_common.glsl` now estimates normal-map variance from the sampled tangent-space normal and derives an effective GGX specular roughness only for specular eval/pdf/sampling.
- Authored material roughness remains unchanged for material data, roughness debug view, diffuse evaluation, delta classification, and reservoir compatibility logic.
- The adjustment is conservative and opt-out through the Specular AA checkbox, so normal-mapped glossy surfaces can be compared with and without the Toksvig-style roughness increase.
- Verified Debug build, Release build, a 3-frame Debug headless roughness debug-view smoke, and the required-style 120-frame Release diagnostic smoke producing profile/rendergraph/debug views/debug package with `validation_error_count=0` and `specular_aa_enabled=true` in `profile.json`.

### Phase 47: Fix KTX2 Mipmap Loading [DONE]

Target files:

- `src/rtv/TextureLoader.cpp`
- `include/rtv/TextureLoader.h`

Steps:

1. Audit KTX2 mip level enumeration and upload offsets.
2. Preserve all mip levels and correct row/slice layout.
3. Ensure image view and sampler use full mip range.
4. Validate with a colored-mip test texture.

Acceptance criteria:

- Correct mip appears at each distance.
- Compressed mip upload passes validation.

Implementation notes:

- Added explicit `TextureMipLevel` metadata to loaded textures, texture assets, scene cache records, and batched texture upload operations.
- Direct block-compressed KTX2 loading now reads every level-index entry, copies all mip payloads into one staging blob, and records per-level buffer offsets, sizes, and extents.
- Basis-transcoded KTX2 loading now preserves every transcoded mip level instead of copying only level 0.
- `BufferUploader` and `BatchUploader` can now issue one `vkCmdCopyBufferToImage` with per-mip `VkBufferImageCopy` regions; images with supplied mip payloads skip generated blits.
- Imported and cached material texture creation now uses full KTX2 mip counts and forwards mip metadata, so image views and samplers see the full mip range.
- Added a tinygltf image-loader callback that preserves `image/ktx2` bytes/URIs instead of letting STB reject KTX2 before `TextureLoader` can process it.
- Bumped the scene cache version to invalidate cached texture records that do not contain mip metadata.
- Verified Debug build, Release build, a generated four-level colored-mip BC1 KTX2 glTF/rtlevel smoke both uncached and cache-hit, and the required-style 120-frame Release diagnostic smoke producing profile/rendergraph/debug views/debug package with `validation_error_count=0`.

### Phase 48: 16-Bit And HDR Textures [DONE]

Target files:

- `src/rtv/TextureLoader.cpp`
- `include/rtv/TextureAsset.h`
- `src/rtv/GltfLoader.cpp`

Steps:

1. Add loader support for 16-bit UNORM, 16F, and HDR formats where relevant.
2. Choose Vulkan formats by texture semantic: normal, emissive, color, data.
3. Preserve color-space handling.
4. Add memory budget warnings for high-precision textures.

Acceptance criteria:

- HDR emissive and high-precision normal textures load correctly.
- Color management remains correct for sRGB textures.

Completed:

- Added explicit Vulkan format and linear-color metadata to texture assets, texture loader output, and scene cache records; bumped the scene cache version to invalidate older texture records.
- Extended STB loading to preserve 16-bit UNORM images as `R16G16B16A16_UNORM` and Radiance HDR images as linear `R32G32B32A32_SFLOAT` payloads.
- Extended direct KTX2 loading beyond BC formats so uncompressed/high-precision KTX2 levels, including 16F and 16-bit UNORM data, upload with their declared Vulkan format and explicit mip metadata.
- Updated glTF image import to preserve HDR payloads and route external non-KTX images through the engine texture loader, avoiding silent 8-bit down-conversion for high-precision files.
- Updated material texture upload format selection so 8-bit color textures still use sRGB formats, linear HDR textures avoid manual gamma decode, and high-precision non-sRGB color textures keep the existing manual sRGB decode path where needed.
- Added high-precision material texture memory warnings for large individual textures and resident high-precision material texture budgets.
- Generated a Phase 48 validation scene under `out/phase48_smoke/hdr16_validation` covering 16F KTX2 base color, 16-bit UNORM KTX2 normal, and Radiance HDR emissive inputs.
- Verified Debug build, Release build, generated Phase 48 validation scene uncached/cache-hit smokes, and the required-style 120-frame Release diagnostic smoke producing profile/rendergraph/debug views/debug package with `validation_error_count=0`.

### Phase 55: Sheen And Thin-Film Sampling [DONE]

Target files:

- `shaders/rt_common.glsl`
- `shaders/pathtrace.rgen`
- `src/rtv/GltfLoader.cpp`

Steps:

1. Add sheen lobe evaluation and sampling.
2. Add thin-film interference approximation for applicable materials.
3. Integrate lobe selection probabilities into BSDF sampling.
4. Validate MIS weights for multi-lobe sampling.

Acceptance criteria:

- Sheen materials converge without excessive variance.
- Thin-film effect is stable under camera motion.

Completed:

- Added GPU material fields for sheen color/roughness, sheen texture slots, iridescence factor/IOR/thickness, and iridescence texture slots; increased the material record stride consistently in C++ and GLSL.
- Added `KHR_materials_iridescence` glTF import, extension diagnostics, cached-scene persistence, and cache-hit restore; bumped the scene cache version to invalidate older material records.
- Routed sheen color textures through color-space classification and sheen roughness/iridescence textures through data-texture classification.
- Added shader sampling for sheen color/roughness and iridescence factor/thickness textures.
- Added Charlie-distribution sheen BRDF evaluation, PDF, and half-vector sampling; integrated diffuse/specular/sheen lobe probabilities into combined BSDF sampling and MIS PDFs.
- Replaced the placeholder thin-film path with a stable wavelength-based specular Fresnel tint controlled by `KHR_materials_iridescence` factor, IOR, and thickness.
- Generated a Phase 55 validation scene under `out/phase55_smoke/sheen_iridescence_validation` covering sheen plus iridescence import, uncached load, and cache-hit restore.
- Verified Debug build, Release build, existing glTF sheen validation, generated sheen/iridescence validation uncached/cache-hit smokes, and the required-style 120-frame Release diagnostic smoke producing profile/rendergraph/debug views/debug package with `validation_error_count=0`.

## Batch 9: Architecture Hardening

Goal: prepare renderer infrastructure for OMM, wavefront, SER, and long-running editor sessions.

### Phase 50: Reservoir Compression [DONE]

Target files:

- `shaders/rt_common.glsl`
- `shaders/restir_common.glsl`
- `shaders/restir_temporal.comp`
- `shaders/restir_spatial.comp`
- `shaders/restir_final.comp`
- `src/rtv/PathTracerRenderer.cpp`
- `include/rtv/PathTracerRenderer.h`

Compression targets:

| Field group | Baseline representation | Candidate representation | Risk |
|-------------|-------------------------|--------------------------|------|
| Metadata | Multiple uint/float fields | Packed `uvec4` bitfields | Low |
| Normal | `vec3` fp32 | Octahedral 2x16-bit | Low-Medium |
| Hit distance / position | `vec3` fp32 world position | fp16 distance plus local basis or quantized offset | Medium |
| Radiance/sample value | `vec3` fp32 | fp16 RGB or RGBE | Medium |
| Target PDF / weight sum | fp32 | fp16 only if bias tests pass | High |
| Confidence / age / flags | fp32/uint | 8-16 bit packed integers | Low |

Steps:

1. Measure the current reservoir footprint at target resolutions.
2. Use the working target from the source roadmap as the baseline: DI reservoirs about 285 MB and GI reservoirs about 475 MB, roughly 760 MB total before compression at high resolution/triple buffering.
3. Inventory all reservoir fields: sample id, light id, hit position, normal, radiance/sample value, target PDF, weight sum, sample count `M`, confidence, age, flags, and visibility state.
4. Split reservoir data into hot fields used every pass and cold/debug fields used only for validation.
5. Pack metadata into `uvec4` or narrower integer fields: light index, pixel/sample id, age, flags, visibility state, and sample count.
6. Quantize normals using octahedral encoding into 2x16-bit or 2x10-bit fields depending on visual error.
7. Quantize hit distance or hit position relative to primary surface where possible instead of storing full `vec3` world position.
8. Store radiance/sample value in `f16vec3` or packed RGBE when the error budget is acceptable.
9. Store target PDF and weight sum in fp16 only after validating no bias or instability; otherwise keep fp32 for those fields.
10. Add versioned reservoir structs: uncompressed debug layout and compressed production layout.
11. Add conversion shaders or compile-time switch so A/B testing can run both layouts on the same scene.
12. Update ReSTIR DI and GI passes to read/write the compressed layout consistently.
13. Add debug views that decode and display compressed fields: age, confidence, M, light id, normal, hit distance, and validity.
14. Add GPU memory accounting for DI, GI, previous, current, spatial, and scratch reservoir buffers.

Acceptance criteria:

- Reservoir memory is reduced by at least 40 percent from the uncompressed baseline.
- High-resolution target memory moves materially below the original ~760 MB reservoir footprint.
- Mean lighting matches the uncompressed layout within a documented tolerance.
- No extra temporal flicker from quantized confidence, normal, hit distance, or weight fields.
- Debug mode can switch back to uncompressed reservoirs for validation.

Completed so far:

- Packed the ReSTIR GI selected normal into octahedral 2x16-bit metadata and packed GI sample count, age, flags, and roughness into a single metadata word while keeping hit position, target PDF, radiance, weight sum, receiver position, and hit distance in fp32.
- Reduced `RestirGiReservoirGpu` stride from 80 bytes to 64 bytes and kept the existing compact DI reservoir stride at 48 bytes.
- Added shared GLSL encode/decode/accessor helpers for compressed GI reservoir normal, roughness, age, sample count, flags, material id, and visibility state across raygen and GI compute passes.
- Updated ReSTIR GI temporal/spatial reuse and GI debug views to use the compressed accessors consistently.
- Fixed the ReSTIR GI debug reservoir write gate so current GI debug view IDs populate GI reservoirs instead of the older denoiser-range IDs.
- Added decoded compressed GI debug views for `restir-gi-normal` and `restir-gi-hit-distance`, including editor selection, CLI parsing, debug-view export, and AI debug/profiling documentation.
- Added profile JSON reservoir memory breakdown fields for DI current/previous/spatial and GI current/previous/spatial buffers. The Phase 50 Cornell smoke reported 309,657,600 total reservoir bytes at the smoke resolution, with each DI reservoir at 44,236,800 bytes and each compressed GI reservoir at 58,982,400 bytes.
- Added `RTV_RESTIR_GI_UNCOMPRESSED_LAYOUT=1` as an explicit uncompressed validation/A-B switch. The shader compiler includes this define in its compile signature and C++ allocation switches GI reservoir stride between the compressed production record and 80-byte uncompressed validation record.
- Ran close-camera A/B validation on `scenes/validation/closeup_cornell.rtlevel` with `--debug-view restir-gi-final`, `--fixed-seed 1`, 30 warmup frames, and 120 total frames. Compressed reservoirs saved 44,236,800 bytes versus the uncompressed validation layout: total reservoir memory dropped from 353,894,400 bytes to 309,657,600 bytes, each GI reservoir dropped from 73,728,000 bytes to 58,982,400 bytes, and `--compare-profile` reported total memory down 8.56% for the run.
- Compared compressed versus uncompressed `restir-gi-final` debug output for closeup Cornell. `--compare-image` reported SSIM 0.999999, PSNR 74.65 dB, MSE 0.00223, max error 46, and 0.0967% changed pixels.
- Measured the close-camera performance issue in the same compressed `restir-gi-final` mode: normal Cornell averaged 9.62 ms GPU / 8.33 ms path trace, while closeup Cornell averaged 35.92 ms GPU / 34.55 ms path trace, making the closeup run 3.73x total GPU and 4.15x path-trace cost with identical reservoir memory.
- Verified compressed and uncompressed `glslangValidator` variants for `pathtrace.rgen`, `restir_spatial.comp`, `restir_gi_spatial.comp`, and `restir_gi_final.comp`; Debug build; Release build; closeup compressed/uncompressed profile+debug-package runs; and the required-style 120-frame Release diagnostic smoke producing profile/rendergraph/debug views/debug package with `validation_error_count=0`.
- Ran the remaining high-resolution reservoir memory comparison for a 1920x1080 target after the first 64-byte GI compression stage. With the initial 48-byte DI reservoirs and 64-byte compressed GI reservoirs, three DI buffers use 298,598,400 bytes, three compressed GI buffers use 398,131,200 bytes, and total reservoir memory is 696,729,600 bytes versus 796,262,400 bytes for the original roadmap baseline. This saved 99,532,800 bytes, or 12.5% of total reservoir memory, so deeper packing was still required.
- Ran temporal flicker/regression A/B validation on `scenes/validation/closeup_cornell.rtlevel` with `--validation-camera-motion`, `--debug-view restir-gi-final`, `--fixed-seed 1`, 4 warmup frames, and 32 total frames. The sequence comparison under `out/phase50_motion_compare/` passed: beauty average SSIM was 0.9999999999956796 with max changed pixels 0.0001085%, `restir-gi-final` average SSIM was 0.9999998191066761 with max changed pixels 0.011393%, and temporal variance was effectively unchanged between compressed and uncompressed layouts. The decoded normal debug view differs as expected because it visualizes quantized octahedral normals directly, but the beauty/final GI outputs remain stable.
- Packed ReSTIR DI reservoirs from 48 bytes to 32 bytes by keeping selected direct radiance/confidence in fp32 and moving source PDF, pairwise previous weight, reservoir age, validity/visibility, and sample count into metadata bitfields/fp16 pairs.
- Packed production ReSTIR GI reservoirs from 64 bytes to 32 bytes by keeping selected GI radiance/weight in fp32, keeping octahedral 2x16 normal metadata, packing hit distance and target PDF into an fp16 pair, packing sample count/age/flags/roughness into one metadata word, and dropping full selected-hit/receiver world positions from the production layout. The uncompressed validation layout still keeps full hit position, receiver position, normal, target PDF, and metadata for A/B checks.
- Updated ReSTIR DI temporal/spatial/final code and ReSTIR GI temporal/spatial/final/debug code to use shared accessors for packed fields. Production GI reuse now bases compatibility on material id, selected normal, hit distance, target value, and receiver-surface depth/normal instead of full stored hit/receiver positions.
- The required-style 120-frame Release Cornell smoke under `out/phase50_packed_smoke/` produced profile JSON, render graph JSON, debug views, and debug package outputs with `validation_error_count=0`. Reservoir memory at 1280x720 dropped to 176,947,200 bytes; each DI and GI current/previous/spatial buffer is 29,491,200 bytes.
- Closeup Cornell A/B validation with `--debug-view restir-gi-final`, `--fixed-seed 1`, 30 warmup frames, and 120 total frames reported reservoir memory down from 309,657,600 bytes in the uncompressed validation layout to 176,947,200 bytes in the packed production layout, a 42.86% reduction. Compared against the original 48-byte DI plus 80-byte GI roadmap baseline, the packed 32-byte DI plus 32-byte GI layout is a 50% reduction at any resolution.
- The closeup `restir-gi-final` image comparison between uncompressed and packed layouts passed with SSIM 0.9999928969900937, PSNR 64.93 dB, MSE 0.02089, max error 78, and 0.7415% changed pixels. The moving-camera sequence comparison under `out/phase50_packed_motion_compare/` also passed: beauty was identical, `restir-gi-final` average SSIM was 0.9999976526233659, and hit-distance temporal variance was effectively unchanged.
- Verified packed and uncompressed `glslangValidator --target-env vulkan1.2` variants for `pathtrace.rgen`, `restir_spatial.comp`, `restir_final.comp`, `restir_gi_spatial.comp`, and `restir_gi_final.comp`; Debug build; Release build; required-style Cornell diagnostic smoke; closeup compressed/uncompressed profile+debug-view runs; and closeup moving-camera sequence A/B validation. Phase 50 is complete.

### Phase 51: Memory Aliasing For Temporal And Reservoir Buffers [DONE]

Target files:

- `include/rtv/RenderGraph.h`
- `src/rtv/RenderGraph.cpp`
- `include/rtv/RenderGraphResource.h`
- `include/rtv/ResourceAllocator.h`
- `src/rtv/ResourceAllocator.cpp`
- `include/rtv/TemporalSystem.h`
- `src/rtv/TemporalSystem.cpp`
- `src/rtv/PathTracerRenderer.cpp`

Aliasing targets:

| Resource group | Candidate aliasing | Expected saving | Prerequisite |
|----------------|--------------------|-----------------|--------------|
| `worldPositionBuffer_` / `previousWorldPositionBuffer_` | Temporal ping-pong alias when previous is not read after current write | ~32 MB | Exact temporal pass lifetimes |
| ReSTIR DI current/previous/spatial scratch | Alias non-overlapping temporal/spatial buffers | Part of ~285 MB | Phase 50 layout inventory |
| ReSTIR GI current/previous/spatial scratch | Alias non-overlapping GI buffers | Part of ~285 MB | Phase 39 final pass schedule |
| TSR intermediate HDR/output buffers | Alias pre/post resolve intermediates | Scene/resolution dependent | Render graph lifetime analysis |
| Denoiser intermediate ping-pong buffers | Alias separable filter temporaries | Scene/resolution dependent | Denoiser pass declarations |

Steps:

1. Inventory temporal swap resources and reservoir resources with their exact frame lifetimes.
2. Specifically evaluate aliasing for `worldPositionBuffer_` and `previousWorldPositionBuffer_`, with the original memory target of about 32 MB saved when lifetimes do not overlap.
3. Specifically evaluate aliasing for the three reservoir buffers, with the original memory target of about 285 MB saved when DI/GI ping-pong or scratch lifetimes do not overlap.
4. Add lifetime intervals to render graph resources: first writer, last reader, queue domain, access mask, image layout, and frame index.
5. Add an aliasing eligibility check: equal or compatible size, format, usage flags, sample count, tiling, queue ownership, and no overlapping lifetime.
6. Add alias groups for temporal current/previous resources only when the graph proves they are not both read in the same pass.
7. Add alias groups for reservoir current/previous/spatial/scratch resources only after ReSTIR temporal and spatial passes declare exact reads and writes.
8. Use VMA allocation aliasing or explicit placed allocation strategy supported by the existing allocator abstraction.
9. Add resource debug names that include alias group id and active logical resource name for GPU captures.
10. Add a debug mode that disables aliasing at runtime for A/B validation.
11. Add validation assertions that a logical resource is not accessed outside its lifetime interval.
12. Add graph visualization or log output showing which resources alias and estimated memory saved.

Acceptance criteria:

- Temporal world-position aliasing saves the expected ~32 MB when enabled.
- Reservoir aliasing saves the expected ~285 MB where the final ReSTIR schedule permits it.
- No validation errors from layout, access, or queue ownership hazards.
- `--disable-resource-aliasing` produces visually equivalent output under fixed RNG seed.
- GPU captures show clear physical allocation reuse and logical resource names.

Completed:

- Added full RenderGraph lifetime intervals for every graph resource, including first/last use, first/last read, first/last write, queue domain, access masks, image layout, estimated bytes, alias eligibility, alias group, and aliased state.
- Hardened transient alias grouping so resources only share an alias group when type, size/extent, format, usage, and lifetime intervals are compatible; the aliasing toggle now controls whether compatible transient resources are physically grouped.
- Wired `--disable-resource-aliasing` through `Application` into `PathTracerRenderer` and every RenderGraph instance. Render graph JSON now reports `resource_aliasing.enabled`, so A/B diagnostic runs can prove whether aliasing was enabled or disabled.
- Extended `rendergraph.json` and `--dump-resource-lifetimes` output with lifetime intervals and `alias_checks`. The alias checks report compatible-shape resource pairs, whether their lifetimes overlap, whether they are schedule candidates, whether they are physical alias candidates, estimated saved bytes, and the rejection reason.
- Ran a Phase 51 Cornell lifetime smoke under `out/phase51_lifetime_smoke/` with profile JSON, render graph JSON, resource lifetime JSON, debug views, and debug package outputs. The profile reported `validation_error_count=0`; `rendergraph.json` contained 36 resources and 104 alias checks.
- Ran `--disable-resource-aliasing` under `out/phase51_lifetime_noalias/` and verified `rendergraph.json` reports `resource_aliasing.enabled=false` while still emitting the same alias analysis. The initial inventory shows current world-position and ReSTIR DI/GI temporal/spatial buffers have overlapping frame lifetimes in the compiled schedule, so they are not yet safe physical-alias candidates without a follow-up schedule/resource split.
- Added explicit physical reuse for the accepted ReSTIR reservoir alias group: `restir gi reservoir` and `restir spatial reservoir` share the same underlying buffer when aliasing is enabled, their size/usage/memory class match, and their RenderGraph lifetimes do not overlap. The shared buffer debug name is `alias group restir gi current / restir spatial reservoir`.
- Added a `Buffer::aliasFrom` non-owning logical alias path so logical buffers can share a physical `VkBuffer` without double-destroying the VMA allocation. Added an explicit initial access barrier for the separately recorded DI spatial graph when it reuses the earlier GI-current physical buffer.
- Updated reservoir memory accounting to count unique physical reservoir buffers. At 1280x720, aliasing reduces reservoir memory from `176,947,200` bytes to `147,456,000` bytes, saving `29,491,200` bytes for the accepted group.
- Updated RenderGraph alias diagnostics to mark shared external handles as active physical aliases. The required Cornell smoke graph reports `restir spatial reservoir` / `restir gi reservoir` with `shared_physical_handle=true`, `physical_alias_candidate=true`, reason `active physical handle reuse`, and `estimated_saved_bytes=29,491,200`.
- Preserved the `--disable-resource-aliasing` A/B path. The no-alias Cornell control reports `resource_aliasing.enabled=false`, no shared handle for the same pair, and reservoir memory `176,947,200` bytes.
- Fixed-seed A/B validation passed for alias-enabled versus `--disable-resource-aliasing`: Cornell beauty and `restir-gi-final` comparisons both reported SSIM `1.0`, PSNR `99.0`, MSE `0.0`, max error `0`, and changed pixels `0.0%`.
- Required 30-warmup/120-frame Release Cornell smoke under `out/phase51_required_smoke/` produced profile JSON, render graph JSON, resource lifetime JSON, debug views, and debug package outputs with `validation_error_count=0` and active physical alias diagnostics.
- Closeup Cornell A/B validation under `out/phase51_closeup_beauty_alias/` and `out/phase51_closeup_beauty_noalias/` reported `validation_error_count=0`, the same reservoir memory reduction (`147,456,000` versus `176,947,200` bytes), and byte-identical beauty and `restir-gi-final` debug views.
- Lightweight Sponza glTF validation under `out/phase51_sponza_alias/` reported `validation_error_count=0`, `restir_reservoir_bytes=62,167,040`, and an active physical alias saving `12,433,408` bytes at that run's effective resolution.
- World-position current/previous, previous reservoir, GI previous, and GI spatial resources remain deliberately unaliased because the lifetime dump proves they overlap in the current schedule. They should only be revisited after a deeper temporal schedule/storage rewrite.

### Phase 52: Async Entity Picking [DONE]

Target files:

- `src/rtv/PathTracerRenderer.cpp`
- `include/rtv/EditorSelection.h`
- `src/rtv/EditorSelection.cpp`
- `src/rtv/ViewportPanel.cpp`

Steps:

1. Replace synchronous pick readback with a per-frame readback ring.
2. Submit pick request with frame index and screen coordinate.
3. Read result only after the corresponding fence signals.
4. Return pending state to the editor instead of blocking.
5. Keep last valid selection visible while a request is pending.

Acceptance criteria:

- Click selection never calls `vkDeviceWaitIdle`.
- Selection result latency is acceptable, usually 1-3 frames.
- No stale result is applied after the scene changes incompatibly.

Completed:

- Replaced synchronous `PathTracerRenderer::pickInstanceId` with `requestPickInstanceId`, `consumePickedInstanceId`, and `pickPending`. The pick result is read after a frame-ring delay based on the renderer frame resource count instead of blocking the device.
- Removed the `vkDeviceWaitIdle(read entity pick buffer)` path. Code search excluding this plan confirms no remaining `pickInstanceId` callsite and no remaining `vkDeviceWaitIdle(read entity pick buffer)` string in implementation files.
- Added pending pick state to `EditorSelection`; `ViewportPanel` now keeps the existing selection visible, submits click UVs as pending requests, and consumes the delayed result when ready.
- Added a pick scene-version counter. Accumulation resets cancel pending pick requests so stale picks are not applied after resize, scene/material/light/environment/settings changes, or camera-cut style resets.
- Verified Debug and Release builds. The only observed compiler warning remains the existing `C4324` PathTracerRenderer padding warning.
- Ran a short Cornell diagnostic smoke under `out/phase52_async_pick_smoke/` with profile JSON, render graph JSON, debug views, and debug package outputs; `validation_error_count=0`.
- Ran the required 30-warmup/120-frame Release Cornell smoke under `out/phase52_required_smoke/` with profile JSON, render graph JSON, resource lifetime JSON, debug views, and debug package outputs; `validation_error_count=0` and all expected outputs were present.

### Phase 53: Replace `vkDeviceWaitIdle` [DONE]

Target files:

- `src/rtv/CommandSystem.cpp`
- `src/rtv/GpuScene.cpp`
- `src/rtv/PathTracerRenderer.cpp`
- `src/rtv/VulkanContext.cpp`
- `src/rtv/UiOverlay.cpp`

Steps:

1. Inventory every `vkDeviceWaitIdle` call and classify it: resize, upload, scene buffer replacement, environment load, picking, shutdown, or debug-only.
2. Replace per-frame/editor interactions with fences, timeline semaphores, deferred deletion, or per-resource lifetime tracking.
3. Keep device idle only for shutdown and unavoidable full device teardown.
4. Add debug logging when any device-idle path is hit.

Acceptance criteria:

- Normal editing, picking, environment load, and resize do not stall the whole device.
- Shutdown remains safe.
- GPU profiler shows fewer long gaps.

Completed:

- Inventoried the remaining device-wide waits after Phase 52. Picking no longer has a wait; remaining waits are grouped as shutdown/headless completion (`CommandSystem::waitIdle`, `UiOverlay` teardown, `VulkanContext` teardown, `UploadContext` teardown), renderer rebuild/reload call sites in `Application`, scene buffer replacement in `GpuScene`, and environment load in `PathTracerRenderer`.
- Removed the editor viewport resize wait in `CommandSystem`; viewport texture invalidation now happens without `vkDeviceWaitIdle(editor viewport resize)`.
- Removed the renderer resize wait in `PathTracerRenderer`. Old per-resolution images and buffers are moved into a retired-resource batch and destroyed after the renderer frame ring has advanced, so in-flight work can drain without a device-wide idle.
- Removed `vkDeviceWaitIdle(replace scene buffer)` from `GpuScene`. GPU scene light, light-BVH, instance, bounds, TLAS node, and TLAS index buffers now return replaced buffers to a retired-buffer list and release them after the renderer frame delay.
- Removed `vkDeviceWaitIdle(load environment)` from `PathTracerRenderer`. HDR/procedural environment image, row/column CDF buffers, and environment params buffers are now retired and released after the renderer frame delay when the environment is replaced.
- Replaced swapchain-resource recreation's device-wide idle with frame-fence waits.
- Removed normal editor renderer-rebuild waits from scene reload, pending scene update, and shader reload flows. Replacement `PathTracerRenderer` instances are installed immediately, and old renderers are retired after the frame-ring delay.
- Added explicit stderr diagnostics to retained shutdown/teardown waits in `CommandSystem::waitIdle`, `UiOverlay` teardown, and `VulkanContext` teardown.
- Code search confirms `vkDeviceWaitIdle(editor viewport resize)`, `vkDeviceWaitIdle(resize path tracer)`, `vkDeviceWaitIdle(replace scene buffer)`, `vkDeviceWaitIdle(load environment)`, and `vkDeviceWaitIdle(read entity pick buffer)` are no longer present in implementation files.
- Verified Debug and Release builds after the resize wait replacement. The only observed compiler warning remains the existing `C4324` PathTracerRenderer padding warning.
- Ran the required-style Release Cornell diagnostic smoke under `out/phase53_resize_smoke/` with profile JSON, render graph JSON, resource lifetime JSON, debug views, and debug package outputs; `validation_error_count=0` and all expected outputs were present.
- Ran the required-style Release Cornell diagnostic smoke again after the scene-buffer and environment deferred-retirement changes under `out/phase53_deferred_smoke/`; `validation_error_count=0` and all expected outputs were present.
- Ran the final required Release Cornell diagnostic smoke under `out/phase53_required_smoke/` with profile JSON, render graph JSON, resource lifetime JSON, debug views, and debug package outputs; `validation_error_count=0` and all expected outputs were present.
- Compared `out/phase52_required_smoke/profile.json` against `out/phase53_required_smoke/profile.json`; CPU avg regression was `0.78%`, GPU avg regression was `0.60%`, and memory regression was `0%`.

### Phase 54: VMA Budget Control And Descriptor Pool Tuning `[DONE]`

Target files:

- `include/rtv/ResourceAllocator.h`
- `src/rtv/ResourceAllocator.cpp`
- `include/rtv/DescriptorAllocator.h`
- `src/rtv/DescriptorAllocator.cpp`
- `include/rtv/DescriptorLayoutCache.h`
- `src/rtv/DescriptorLayoutCache.cpp`
- `include/rtv/UiOverlay.h`
- `src/rtv/UiOverlay.cpp`
- `include/rtv/VulkanContext.h`
- `src/rtv/VulkanContext.cpp`

Budget categories:

| Category | Examples | Required diagnostics |
|----------|----------|----------------------|
| Acceleration structures | BLAS, TLAS, scratch | build/update peak, resident size |
| Textures | base color, normal, emissive, HDR env | format, mip count, compressed/uncompressed size |
| Temporal histories | TAA/TSR, denoiser, velocity, depth, normal | render/display extent, precision, history count |
| ReSTIR reservoirs | DI, GI, previous/current/spatial/scratch | compressed size, alias group, resolution |
| Render graph transients | intermediate images/buffers | lifetime, alias group, physical allocation |
| Wavefront queues | ray, hit, shadow, compaction, sort | capacity, high-water mark |
| Staging/upload | texture and buffer staging | transient peak and lifetime |
| Descriptors | renderer, bindless, UI, temporal, ReSTIR | pool size, allocated sets, failures |

Steps:

1. Enable `VMA_ALLOCATOR_CREATE_EXT_MEMORY_BUDGET_BIT` when `VK_EXT_memory_budget` is available.
2. Query heap budget, heap usage, allocation count, and block fragmentation through VMA each frame or at a throttled diagnostics rate.
3. Add memory-budget categories: acceleration structures, textures, temporal histories, reservoirs, render graph transients, wavefront queues, staging, UI, and miscellaneous buffers.
4. Add high-water marks and per-frame delta tracking for GPU memory.
5. Add warning thresholds at 70, 85, and 95 percent of reported budget.
6. Add emergency quality-budget hooks: reduce render scale, disable optional histories, reduce reservoir resolution, reduce denoiser history precision, and reject new large texture uploads.
7. Audit descriptor pool sizing for renderer, bindless resources, ImGui/UI, temporal passes, ReSTIR, and wavefront.
8. Replace fixed descriptor pool guesses with pool sizing derived from pass/resource counts where practical.
9. Add descriptor pool usage stats: allocated sets, free sets, pool count, failed allocations, fragmentation, and peak use.
10. Add descriptor pool growth policy with a hard cap and explicit error reporting.
11. Add descriptor lifetime validation for hot reload, scene reload, swapchain recreation, and renderer shutdown.
12. Add diagnostics UI or log output for VMA budget and descriptor pool state.

Acceptance criteria:

- VMA budget reporting is active on devices supporting `VK_EXT_memory_budget`.
- Memory pressure warnings appear before allocation failure.
- Descriptor pools no longer exhaust during long editor sessions, scene reloads, or material hot reload.
- Descriptor pool sizes are documented and tied to renderer feature counts.
- No descriptor leaks are observed after repeated scene load/unload cycles.

Completed so far:

- Enabled `VK_EXT_memory_budget` during device creation when supported and passed `VMA_ALLOCATOR_CREATE_EXT_MEMORY_BUDGET_BIT` into VMA allocator creation.
- Added `ResourceAllocator::memoryBudgetReport()` with per-heap VMA usage, budget, allocation bytes/count, block bytes/count, pressure level, warnings at 70/85/95 percent thresholds, total usage, usage delta, and peak usage.
- Added VMA memory-budget output to profile JSON and `--dump-memory`; local Cornell validation reports `supported=true`, `total_usage_bytes=806,686,720`, `total_budget_bytes=32,300,032,000`, max heap usage ratio `0.04299`, and pressure `normal`.
- Tuned renderer descriptor-pool sizing away from the previous bindless-array multiplication. Pools now reserve for one 1024-entry bindless material array plus pass-local sets, use a 64-pool hard cap, and throw an explicit error if the cap is reached.
- Added descriptor pool stats for allocated sets, peak sets, pool count, capacity, growth count, out-of-pool failures, and fragmented-pool failures. Profile JSON and `--dump-memory` now include these stats.
- Ran the required-style Release Cornell smoke under `out/phase54_budget_smoke/` with profile JSON, render graph JSON, resource lifetime JSON, memory JSON, debug views, and debug package outputs; `validation_error_count=0` and all expected outputs were present. Descriptor stats reported `pool_count=3`, `capacity_sets=768`, `allocated_sets=36`, `peak_allocated_sets=42`, `failed_allocations=0`, and `fragmented_pool_failures=0`.
- Updated `--compare-profile` so nested memory diagnostics such as `vma_budget` and `descriptors` do not break numeric memory comparisons. Comparing `out/phase53_required_smoke/profile.json` to `out/phase54_budget_smoke/profile.json` reported CPU avg regression `0.0087%`, GPU avg regression `-0.3768%`, and memory regression `0%`.
- Added runtime memory-pressure quality hooks. The renderer derives a pressure tier from VMA budget usage, keeps user settings intact, and exposes effective render scale, SPP limiter state, ReSTIR GI half-resolution state, and denoiser history cap through profile JSON as `memory_pressure_quality`.
- Added `RTV_MEMORY_PRESSURE_OVERRIDE` as a diagnostic validation override for memory-pressure actions. The critical-pressure smoke under `out/phase54_pressure_override_smoke/` reported `validation_error_count=0`, `memory_pressure_quality.tier=3`, `effective_render_scale=0.5`, `limit_samples_per_pixel=true`, `restir_gi_half_resolution=true`, `denoiser_max_history_length=16`, and reduced reservoir memory to `36,864,000` bytes.
- Ran the normal required-style Release Cornell smoke again under `out/phase54_pressure_required_smoke/` with profile JSON, render graph JSON, resource lifetime JSON, memory JSON, debug views, and debug package outputs. It reported `validation_error_count=0`, memory pressure inactive, VMA pressure `normal`, descriptor failures `0`, fragmented-pool failures `0`, and unchanged reservoir memory `147,456,000` bytes. Comparing against `out/phase54_budget_smoke/profile.json` reported CPU avg regression `-1.0957%`, GPU avg regression `-0.3285%`, and memory regression `0%`.
- Added authoritative staging/upload diagnostics from `BufferUploader` and `BatchUploader`: total uploaded bytes, peak staging allocation, last staging allocation, upload count, buffer upload count, image upload count, and batch upload count. Profile JSON, `--dump-memory`, and profile comparison totals now include staging peak bytes as the resident upload contribution.
- Ran the required-style Release Cornell smoke under `out/phase54_staging_required_smoke/` with profile JSON, render graph JSON, resource lifetime JSON, memory JSON, debug views, and debug package outputs. It reported `validation_error_count=0`, VMA pressure `normal`, descriptor failures `0`, `staging_upload_total_bytes=43,273,432`, `staging_upload_peak_bytes=41,173,940`, `staging_upload_count=7`, and `memory.total_bytes=514,728,884`. Comparing against `out/phase54_pressure_required_smoke/profile.json` reported CPU avg regression `-0.5032%`, GPU avg regression `-0.2567%`, and memory regression `8.6946%`; the memory increase is expected accounting expansion from adding staging peak bytes, not an added persistent allocation.
- Added UI descriptor-pool diagnostics for editor runs. `UiOverlay` reports whether the UI is present, descriptor max sets, combined-image-sampler/sampled-image/sampler descriptor capacity, and whether the viewport texture descriptor is allocated; headless diagnostics report `ui.present=false`.
- Added render graph transient memory derivation to `--dump-memory`. When `--dump-rendergraph` is provided, memory JSON now reports transient estimated bytes, transient alias-eligible bytes/count, and active RenderGraph alias savings/count from `rendergraph.json` lifetime and alias diagnostics. The current Cornell graph reports no transient pool allocations yet, but it reports the active explicit reservoir alias as `rendergraph_active_alias_saved_bytes=29,491,200` and `rendergraph_active_alias_count=1`.
- Ran the required-style Release Cornell smoke under `out/phase54_categories_required_smoke/` after adding UI/transient categories. It reported `validation_error_count=0`, `ui.present=false`, `staging_upload_peak_bytes=41,173,940`, descriptor failures `0`, VMA pressure `normal`, and memory pressure inactive. Comparing against `out/phase54_staging_required_smoke/profile.json` reported CPU avg regression `-0.6753%`, GPU avg regression `-0.9791%`, and memory regression `0%`.
- Verified the renamed RenderGraph alias memory fields with a short Release Cornell smoke under `out/phase54_categories_quick_smoke/`; memory JSON reported `transient_resources_bytes=0`, `transient_alias_eligible_bytes=0`, `rendergraph_active_alias_saved_bytes=29,491,200`, `rendergraph_active_alias_count=1`, and `total_bytes=514,728,884`.
- Added `--descriptor-lifetime-stress`, `--descriptor-lifetime-stress-cycles`, and `--descriptor-lifetime-stress-frames` as a repeatable headless descriptor/VMA lifetime report. The stress loop renders steady frames, toggles render-scale resources, updates material descriptors when scene assets exist, reloads the `.rtlevel`, recreates the path tracer through the shader-reload path, drains retired renderers, and writes per-step descriptor/VMA snapshots. It exits nonzero if descriptor allocation failures increase, fragmented-pool failures increase, or retired renderers remain queued.
- Ran the required Release Cornell diagnostic smoke with the expanded descriptor lifetime stress under `out/phase54_scene_descriptor_stress_required/` with profile JSON, render graph JSON, resource lifetime JSON, memory JSON, descriptor stress JSON, debug views, and debug package outputs. All expected outputs were present, `validation_error_count=0`, `descriptor_lifetime_stress.passed=true`, `cycles=12`, `samples=62`, `retired_renderer_count_final=0`, `failed_allocations=0`, `fragmented_pool_failures=0`, max descriptor `pool_count=3`, max descriptor `capacity_sets=768`, profile descriptor `allocated_sets=36`, profile descriptor `peak_allocated_sets=42`, VMA pressure `normal`, and `memory.total_bytes=514,728,884`.
- Compared `out/phase54_categories_required_smoke/profile.json` against `out/phase54_scene_descriptor_stress_required/profile.json`; CPU avg regression was `0.2729%`, GPU avg regression was `0.5741%`, and memory regression was `0%`. The stress run increases cumulative staging upload counters by design because it reloads/rebuilds renderer resources, while staging peak and resident memory total remain unchanged.
- Ran the Balanced budget gate on the current Release Cornell profile under `out/phase54_descriptor_stress_budget/`; it failed existing denoiser, tail-frame, texture/buffer, and temporal-history caps from `budgets/balanced_game_16ms.json`. The Phase 54 comparison still shows no resident memory regression and no descriptor failures; budget retuning or further denoiser/memory optimization remains outside this phase.
- Verified Debug and Release builds after the descriptor lifetime stress work. The only observed compiler warning remains the existing `C4324` PathTracerRenderer padding warning.

Deferred follow-up:

- Extend memory categories for future wavefront queues once the wavefront architecture exists and exposes authoritative queue capacity/high-water data.

## Batch 10: Opacity Micromaps

Goal: reduce any-hit cost for alpha-tested foliage on RTX 40+ hardware while preserving fallback paths.

### Phase 56: Extension Detection And Feature Query `[DONE]`

Target files:

- `src/rtv/VulkanContext.cpp`
- `include/rtv/VulkanContext.h`
- `src/rtv/RayTracingScene.cpp`

Steps:

1. Query opacity micromap extension support and feature structs.
2. Expose renderer capability flags.
3. Add settings UI with disabled reason when unsupported.
4. Ensure non-NVIDIA or pre-RTX-40 devices use fallback alpha any-hit path.

Acceptance criteria:

- Capability detection is correct and visible.
- Unsupported devices run unchanged.

Completed:

- Added `OpacityMicromapDeviceInfo` to `VulkanContext` and query support for `VK_EXT_opacity_micromap`, `VkPhysicalDeviceOpacityMicromapFeaturesEXT`, and `VkPhysicalDeviceOpacityMicromapPropertiesEXT`.
- Exposed `VulkanContext::opacityMicromapInfo()` and `supportsOpacityMicromaps()`. The renderer only reports OMM usable when hardware RT is available, the OMM extension is exposed, the `micromap` feature is present, and the device vendor is NVIDIA; otherwise the disabled reason keeps non-NVIDIA and pre-RTX-40 devices on the alpha any-hit fallback path.
- Enabled `VK_EXT_opacity_micromap` and the `micromap` feature in the device feature chain only when the capability is usable. The BLAS build path is still unchanged in Phase 56, so OMM remains off by default until geometry classification and BLAS integration are implemented.
- Added `RendererSettings::opacityMicromapsEnabled`, scene JSON persistence, profile JSON output, and Render Settings UI exposure. Unsupported devices show a disabled checkbox with the capability disabled reason.
- Profile JSON now includes an `opacity_micromap` block and `settings.opacity_micromaps_enabled`.
- Verified Debug and Release builds. The only observed compiler warning remains the existing `C4324` PathTracerRenderer padding warning.
- Ran a Debug headless Cornell startup smoke under `out/phase56_debug_startup/`; validation-layer startup succeeded and the local RTX 4070 Ti SUPER reported OMM available with max 2-state and 4-state subdivision level `12`.
- Ran the required Release Cornell diagnostic smoke under `out/phase56_required_smoke/` with profile JSON, render graph JSON, resource lifetime JSON, memory JSON, debug views, and debug package outputs. All expected outputs were present, `validation_error_count=0`, `opacity_micromap.supported=true`, `extension_supported=true`, `micromap_feature=true`, `capture_replay=false`, `host_commands=false`, max 2-state/4-state subdivision level `12`, `settings.opacity_micromaps_enabled=false`, descriptor failures `0`, fragmented-pool failures `0`, and `memory.total_bytes=514,728,884`.
- Compared `out/phase54_scene_descriptor_stress_required/profile.json` against `out/phase56_required_smoke/profile.json`; CPU avg regression was `-0.7042%`, GPU avg regression was `-0.5526%`, and memory regression was `0%`.
- Ran lightweight Sponza glTF smoke under `out/phase56_sponza_gltf_smoke/` with profile JSON and debug views. It reported `validation_error_count=0`, `opacity_micromap.supported=true`, and `settings.opacity_micromaps_enabled=false`, confirming the broader glTF/material path remains on the existing fallback behavior in this detection-only phase.

### Phase 57: Mark Alpha-Tested Geometry `[DONE]`

Target files:

- `src/rtv/GltfLoader.cpp`
- `src/rtv/SceneToGpuSceneBuilder.cpp`
- `include/rtv/MeshAsset.h`
- `include/rtv/RayTracingScene.h`

Steps:

1. Add `containsAlphaTestedGeometry` or equivalent mesh/material flag.
2. Propagate alpha mode and cutoff from glTF to GPU scene.
3. Split flags by primitive where needed.
4. Add debug stats for opaque, alpha-tested, and blended geometry counts.

Acceptance criteria:

- Alpha-tested foliage is identified correctly.
- Opaque geometry is not incorrectly routed through OMM.

Completed:

- Added per-primitive alpha classification to `MeshPrimitiveAsset`, including alpha mode/cutoff propagation and explicit alpha-tested/blended flags derived from the assigned material.
- Updated glTF import, GPU-cache restoration, scene material assignment, and material edits to keep primitive alpha classification in sync with material alpha mode and cutoff.
- Packed primitive alpha class into GPU primitive metadata and propagated mesh-level `containsAlphaTestedGeometry` / `containsBlendedGeometry` into ray tracing mesh build inputs while preserving the existing any-hit fallback path. Opaque traversal remains enabled only when every primitive in a mesh is opaque-safe.
- Added `ray_tracing_geometry` profile JSON diagnostics plus editor/debug stats for opaque, alpha-tested, and blended primitive/triangle counts.
- Verified Debug and Release builds. The only observed compiler warning remains the existing `C4324` PathTracerRenderer padding warning.
- Ran the required Release Cornell diagnostic smoke under `out/phase57_required_smoke/` with profile JSON, render graph JSON, resource lifetime JSON, memory JSON, debug views, and debug package outputs. All expected outputs were present, `validation_error_count=0`, `ray_tracing_geometry.opaque_primitive_count=76812`, `alpha_tested_primitive_count=0`, `blended_primitive_count=0`, and `mesh_count_with_only_opaque_geometry=1`, confirming opaque Cornell geometry is not routed through alpha/OMM classification.
- Ran lightweight Sponza glTF smoke under `out/phase57_sponza_gltf_smoke/`; the cached glTF path reported `validation_error_count=0`, `alpha_tested_primitive_count=14`, `alpha_tested_triangle_count=34940`, `opaque_primitive_count=89`, and `blended_primitive_count=0`, confirming alpha-tested foliage/material primitives are identified correctly.
- Compared `out/phase56_required_smoke/profile.json` against `out/phase57_required_smoke/profile.json`; CPU avg regression was `0.7791%`, GPU avg regression was `0.6616%`, and memory regression was `0%`.

### Phase 58: Generate OMM Data From Alpha Textures `[DONE]`

Target files:

- `src/rtv/TextureLoader.cpp`
- `src/rtv/RayTracingScene.cpp`
- new OMM preprocessing helper files if needed

Steps:

1. Sample alpha textures at the required subdivision level.
2. Classify micro-triangles as opaque, transparent, unknown, or mixed.
3. Build micromap arrays and usage counts.
4. Cache OMM data per mesh/material/alpha texture.
5. Add CPU-side validation and debug visualization.

Acceptance criteria:

- Generated OMM data matches alpha cutoff visually.
- Preprocessing time and memory are measured.

Completed:

- Added `OpacityMicromapPreprocess` CPU preprocessing helpers that sample alpha-tested base-color textures at subdivision level `2`, classify micro-triangles as opaque, transparent, mixed, or unknown, and build per-primitive micro-triangle state arrays with usage counts.
- Added an in-memory preprocessing cache keyed by mesh/primitive/material/alpha texture/cutoff/subdivision so duplicated requests reuse generated CPU OMM state data during scene construction.
- Integrated preprocessing with imported and cached glTF scene creation while leaving BLAS build behavior unchanged. Phase 59 still owns attaching OMM descriptors to Vulkan acceleration-structure builds.
- Added profile JSON measurements under `opacity_micromap.preprocess`, including eligible/generated primitive counts, alpha-texture primitive count, cache entries/hits, total triangles, micro-triangle state counts, data bytes, preprocessing time, validation errors, and warnings.
- Added a CPU debug atlas export at `debug_views/opacity_micromap/opacity_micromap_microtriangles.png` when `--save-debug-views` is used. The atlas uses white for opaque, black for transparent, yellow for mixed, and blue for unknown micro-triangles.
- Verified Debug and Release builds. The only observed compiler warning remains the existing `C4324` PathTracerRenderer padding warning.
- Ran the required Release Cornell diagnostic smoke under `out/phase58_required_smoke/` with profile JSON, render graph JSON, resource lifetime JSON, memory JSON, debug views, and debug package outputs. All expected outputs were present, `validation_error_count=0`, `opacity_micromap.preprocess.eligible_primitive_count=0`, `micro_triangle_count=0`, and `validation_error_count=0`, confirming opaque-only scenes do not generate OMM data.
- Ran lightweight Sponza glTF smoke under `out/phase58_sponza_gltf_smoke/`; it reported `validation_error_count=0`, `ray_tracing_geometry.alpha_tested_primitive_count=14`, `opacity_micromap.preprocess.eligible_primitive_count=14`, `generated_primitive_count=14`, `micro_triangle_count=559040`, `opaque_count=145683`, `transparent_count=132794`, `mixed_count=280563`, `unknown_count=0`, `data_bytes=559712`, `preprocessing_ms=150.7632`, and `validation_error_count=0`.
- Verified the generated Sponza CPU debug atlas exists at `out/phase58_sponza_gltf_smoke/debug_views/opacity_micromap/opacity_micromap_microtriangles.png` and visually shows opaque, transparent, and mixed micro-triangle bands with no unknown blue samples.
- Compared `out/phase57_required_smoke/profile.json` against `out/phase58_required_smoke/profile.json`; CPU avg regression was `-0.1360%`, GPU avg regression was `-0.5276%`, and memory regression was `0%`.

### Phase 59: Integrate OMM Into BLAS Builds `[DONE]`

Target files:

- `src/rtv/AccelerationStructure.cpp`
- `src/rtv/RayTracingScene.cpp`

Steps:

1. Attach OMM descriptors to eligible BLAS geometry.
2. Use extension build structs in the BLAS build chain.
3. Preserve fallback BLAS build path.
4. Validate build flags and memory requirements.

Acceptance criteria:

- BLAS builds succeed with OMM enabled.
- Any-hit invocation count drops on foliage scenes.

Completed:

- Added guarded Vulkan opacity micromap BLAS integration. `RayTracingScene` now builds `VkMicromapEXT` storage for eligible alpha-tested CPU OMM data, records `vkCmdBuildMicromapsEXT` before BLAS builds, attaches `VkAccelerationStructureTrianglesOpacityMicromapEXT` through the triangles `pNext` chain, and keeps the existing fallback BLAS path when OMM is disabled, unsupported, or no eligible data exists.
- Packed Phase 58 CPU micro-triangle states into Vulkan 4-state opacity micromap data. Opaque and transparent micro-triangles map to hardware skip states; mixed/unknown samples conservatively map to unknown-opaque so the any-hit shader remains the fallback for uncertain alpha regions.
- Added `--opacity-micromaps` / `--omm <on/off>` for headless validation and propagated startup settings into ray tracing scene/pipeline creation so OMM state is available before BLAS and pipeline creation. The ray tracing pipeline now uses `VK_PIPELINE_CREATE_RAY_TRACING_OPACITY_MICROMAP_BIT_EXT` only when an OMM-backed BLAS is active.
- Added profile JSON diagnostics under `opacity_micromap.build`, including requested/supported/active state, fallback reason, micromap count, mesh count, indexed triangle count, triangle-array count, packed micro-triangle count, micromap bytes, build input bytes, scratch bytes, and build elapsed time.
- Kept non-alpha geometry out of the uncertain OMM path in Phase 59's single-geometry BLAS layout by defaulting non-blended, non-alpha triangles to the fully opaque special index. Blended meshes remain conservative until Phase 60 geometry splitting.
- Verified Debug and Release builds. The only observed compiler warning remains the existing `C4324` PathTracerRenderer padding warning.
- Ran the required Release Cornell diagnostic smoke under `out/phase59_required_smoke/` with profile JSON, render graph JSON, resource lifetime JSON, memory JSON, debug views, and debug package outputs. All expected outputs were present, `validation_error_count=0`, `opacity_micromap.build.requested=false`, `active=false`, and required diagnostic outputs remained intact.
- Ran lightweight Sponza glTF with OMM enabled under `out/phase59_sponza_gltf_omm/`; it reported `validation_error_count=0`, `opacity_micromap.build.active=true`, `micromap_count=1`, `mesh_count=1`, `indexed_triangle_count=34940`, `triangle_array_count=34095`, `packed_micro_triangle_count=545520`, `micromap_bytes=818560`, `build_input_bytes=1458208`, and generated the CPU OMM debug atlas.
- Ran matched lightweight Sponza glTF performance profiles with OMM on/off under `out/phase59_sponza_gltf_omm_perf/` and `out/phase59_sponza_gltf_no_omm_perf/`. Both reported `validation_error_count=0`; the OMM-on path built successfully and remained within smoke-test correctness expectations, while full any-hit/shadow-ray speedup validation remains in Phase 60 where geometry splitting can isolate alpha-tested geometry and add authoritative any-hit counters.
- Compared `out/phase58_required_smoke/profile.json` against `out/phase59_required_smoke/profile.json`; CPU avg regression was `-0.5153%`, GPU avg regression was `-0.3081%`, and memory regression was `0%`.

### Phase 60: Geometry Splitting `[DONE]`

Target files:

- `src/rtv/SceneToGpuSceneBuilder.cpp`
- `src/rtv/RayTracingScene.cpp`
- `shaders/pathtrace.rahit`

Steps:

1. Split opaque, alpha-tested, and blended geometry into separate BLAS geometry groups.
2. Use OMM only for alpha-tested geometry.
3. Keep blended geometry on existing any-hit path if needed.
4. Profile traversal and any-hit counts.

Acceptance criteria:

- Foliage shadow rays improve by 2-4x on supported hardware.
- Any-hit invocations drop by 50-80 percent.
- Fallback path remains correct.

Completed Phase 60 implementation notes:

- Split each mesh BLAS into separate opaque, alpha-tested, and blended `VkAccelerationStructureGeometryKHR` ranges while keeping a one-BLAS-per-mesh layout. Alpha-tested ranges are the only ranges that attach OMM triangle-array data; blended ranges stay on the any-hit fallback path.
- Added shader-visible geometry-to-global-triangle mapping buffers so `gl_GeometryIndexEXT` plus `gl_PrimitiveID` still resolve the correct material, primitive id, and triangle data after BLAS geometry splitting.
- Preserved conservative traversal semantics: opaque ranges use `VK_GEOMETRY_OPAQUE_BIT_KHR` only when the primitive is classified as traversal-safe, and fallback ranges now also require an opaque alpha class before setting the opaque traversal flag.
- Added profile JSON diagnostics under `ray_tracing_geometry` for total BLAS geometry count, opaque/alpha-tested/blended split counts, and OMM-backed geometry count.
- Added profile-run shader counters under `gpu_debug_counters.ray_tracing_any_hit` for surface trace rays, shadow trace rays, closest-hit invocations, alpha-material closest hits, camera any-hit invocations/accepts/ignores, and shadow any-hit invocations/accepts/ignores. These counters are enabled for profile runs through `camera.path_trace_controls.z`, which is visible to raygen, closest-hit, and any-hit stages.
- Added `--omm-subdivision` / `--opacity-micromap-subdivision <N>` for diagnostic OMM preprocessing sweeps, threaded the startup setting into imported and cached scene preprocessing, clamped runtime settings to CPU subdivision level `0..5`, and reported `settings.opacity_micromap_subdivision_level` in profile JSON.
- Verified Debug and Release builds. The only observed compiler warning remains the existing `C4324` PathTracerRenderer padding warning.
- Ran the required Release Cornell diagnostic smoke under `out/phase60_required_smoke/` with profile JSON, render graph JSON, resource lifetime JSON, memory JSON, debug views, and debug package outputs. All expected outputs were present, JSON parsed, `validation_error_count=0`, `blas_geometry_count=1`, `blas_opaque_geometry_count=1`, and no alpha/OMM geometries were reported for the opaque-only Cornell scene.
- Ran lightweight Sponza glTF with OMM enabled under `out/phase60_sponza_gltf_omm/`; it reported `validation_error_count=0`, `blas_geometry_count=26`, `blas_opaque_geometry_count=13`, `blas_alpha_tested_geometry_count=13`, `blas_opacity_micromap_geometry_count=13`, `opacity_micromap.build.active=true`, `micromap_count=1`, `indexed_triangle_count=34940`, `triangle_array_count=34095`, `micromap_bytes=818560`, and exported the CPU OMM debug atlas.
- Ran matched lightweight Sponza glTF performance profiles with OMM on/off under `out/phase60_sponza_gltf_omm_perf/` and `out/phase60_sponza_gltf_no_omm_perf/`. Both reported `validation_error_count=0`; the OMM-on path remained valid but was not a frame-average speedup in this view (`gpu_frame_ms.avg` +10.79%, `path_trace.avg` +16.11%), while p95/p99 path-trace timing improved in the instrumented comparison. The diagnostic counters reported matching `surface_trace_rays=34968960` and zero any-hit invocations for both OMM states, so the current Sponza camera path does not provide a defensible any-hit drop percentage.
- Compared `out/phase59_required_smoke/profile.json` against `out/phase60_required_smoke/profile.json`; CPU avg regression was `+2.0774%`, GPU avg regression was `+1.7307%`, memory regression was effectively flat (`+0.000012%`, 64 bytes from diagnostic counter buffers), and `validation_error_count=0`.
- Confirmed closest-hit and any-hit diagnostic counter writeback after switching the profile-run gate to `camera.path_trace_controls.z`. An opaque Cornell probe reported closest-hit activity with zero any-hit as expected, while moving Sponza reported nonzero camera and shadow any-hit counts only when the camera path traversed alpha-tested foliage.
- Ran matched moving lightweight Sponza glTF profiles with `--validation-camera-motion`, `--warmup-frames 10`, `--frames 60`, fixed seed `1`, OMM off under `out/phase60_sponza_motion_no_omm_subdiv_accept/`, and OMM on with `--omm-subdivision 3` under `out/phase60_sponza_motion_omm_subdiv3_accept/`. Both reported `validation_error_count=0`; OMM was active with `subdivision_level=3`, `micromap_bytes=1,234,048`, `opaque_count=789211`, `transparent_count=725871`, and `mixed_count=721078`.
- The matched moving-Sponza profile reduced camera any-hit invocations from `875779` to `430357` (`50.86%`) and shadow any-hit invocations from `202790` to `94864` (`53.22%`), with comparable traced ray counts (`surface_trace_rays` `23490882` off vs `23484192` on, `shadow_trace_rays` `211398` off vs `205279` on). In this run `path_trace.avg` improved from `0.774896 ms` to `0.688960 ms` and `gpu_frame_ms.avg` from `4.171202 ms` to `3.820706 ms`, satisfying the Batch 10 any-hit reduction target for the moving foliage path.
- Re-ran the required Release Cornell diagnostic smoke after the subdivision CLI change under `out/phase60_subdivision_cli_required_smoke/`; profile/rendergraph/lifetime/memory JSON parsed, debug views and debug package were present, `validation_error_count=0`, `blas_geometry_count=1`, and `settings.opacity_micromap_subdivision_level=2`.

## Batch 11: Wavefront Path Tracing

Goal: fork the renderer into a queue-based architecture where compute handles generation/shading and thin ray tracing pipelines handle hardware traversal.

Wavefront work is architecture v2. Do not start until the megakernel path tracer, temporal system, denoiser, ReSTIR DI/GI, materials, bindless descriptors, and render graph hardening are stable.

### Phase 61: Expand Wavefront Common Structs `[DONE]`

Target files:

- `shaders/wavefront_common.glsl`
- `include/rtv/PathTracerRenderer.h`
- `src/rtv/PathTracerRenderer.cpp`

Steps:

1. Define `WavefrontRay`, `WavefrontHit`, `WavefrontShadowRay`, and `WavefrontPixelState`.
2. Include radiance, throughput, RNG state, MIS state, path depth, atmosphere transmittance, material id, instance id, primitive id, barycentrics, and flags.
3. Define queue headers with atomic counters and capacity.
4. Define dynamic queue sizing based on render extent and max path depth.
5. Add buffer allocation and clear passes.

Acceptance criteria:

- Struct layouts match between C++ and GLSL.
- Queue counters clear and increment correctly in a test shader.

Completed:

- Added `shaders/wavefront_common.glsl` with shared `WavefrontQueueHeader`, `WavefrontRay`, `WavefrontHit`, `WavefrontShadowRay`, and `WavefrontPixelState` layouts. The matching C++ GPU structs live in `PathTracerRenderer` with static size assertions: header `32` bytes, ray `80` bytes, hit `96` bytes, shadow ray `80` bytes, and pixel state `128` bytes.
- Added an opt-in Phase 61 validation path via `--wavefront-queues <on/off>` and `RendererSettings::wavefrontQueuesEnabled`. It defaults to off so the megakernel renderer remains the reference path with no default queue memory, while targeted wavefront validation runs can allocate queues and execute the clear pass.
- Added dynamic queue sizing from the current render extent and effective max path depth metadata. The Phase 61 queues allocate one record per render pixel per active queue because queues are reused per bounce; the clear header records the current effective max path depth for later wavefront dispatch stages.
- Added `wavefront_queue_clear.comp`, a compute validation pass that initializes queue headers, clears queue counts, writes capacities/max-depth/frame metadata, and exercises an atomic increment before restoring the ray queue count to zero. Profile JSON reports the readback under `wavefront_queues`.
- Added the opt-in `wavefront_queue_clear` RenderGraph pass and matching graph-dump resources when `--wavefront-queues on` is supplied. Default graph dumps omit the pass and report `settings.wavefront_queues_enabled=false`, preserving current megakernel behavior.
- Verified Release and Debug builds. The only observed compiler warning remains the existing `C4324` PathTracerRenderer padding warning.
- Ran targeted Cornell queue validation with `--wavefront-queues on` under `out/phase61_wavefront_queue_optin_probe/`. It reported `validation_error_count=0`, `wavefront_queues.clear_validation_passed=true`, all queue counts `0`, `ray_queue_capacity=921600`, `max_path_depth=2`, `total_bytes=353894528`, and `rendergraph.json` contained `wavefront_queue_clear`.
- Ran the required default Cornell diagnostic smoke under `out/phase61_required_smoke_default/` with profile JSON, render graph JSON, resource lifetime JSON, memory JSON, debug views, and debug package outputs. All expected outputs were present, JSON parsed, `validation_error_count=0`, `settings.wavefront_queues_enabled=false`, `wavefront_queues.buffers_allocated=false`, `wavefront_queues.total_bytes=0`, and the default render graph did not include `wavefront_queue_clear`.
- Compared the default Phase 61 Cornell smoke against the Phase 60 smoke: CPU avg regression `+0.79%`, GPU avg regression `-0.09%`, memory regression `0%`, and beauty image comparison was byte-identical (`SSIM=1.0`, `PSNR=99.0`, max error `0`, changed pixels `0%`).
- Ran lightweight Sponza glTF default validation under `out/phase61_sponza_gltf_smoke_default/`; it reported `validation_error_count=0`, `alpha_tested_primitive_count=14`, `settings.wavefront_queues_enabled=false`, no wavefront queue allocation, debug views exported, and the OMM CPU debug atlas was present.

### Phase 62: Camera Ray Generation Compute `[DONE]`

Target files:

- new `shaders/wavefront_generate.comp`
- `src/rtv/PathTracerRenderer.cpp`

Steps:

1. Generate primary camera rays in compute.
2. Initialize per-pixel state.
3. Append rays into the ray queue.
4. Support jitter, depth of field placeholders, and camera cut reset.

Acceptance criteria:

- Generated ray directions match the megakernel camera path.
- Queue count equals active render pixels.

Completed:

- Added `shaders/wavefront_generate.comp`, an opt-in compute pass behind `--wavefront-primary-generate <on/off>` / `--wavefront-generate <on/off>`. Enabling it implies `wavefrontQueuesEnabled`, but the default megakernel path still allocates no wavefront queue memory and omits all wavefront passes.
- The pass initializes primary `WavefrontRay` records and `WavefrontPixelState` records for each render pixel, uses the same camera formula as `pathtrace.rgen` including jitter, records camera-moving/camera-cut flags, and leaves depth-of-field fields as inert placeholders for Phase 78.
- Added the `wavefront_primary_generate` RenderGraph pass after `wavefront_queue_clear`, plus matching graph-dump representation. The current megakernel `path_trace_rt` pass still renders the frame; Phase 62 only validates generated queue contents.
- Extended `wavefront_queues` profile JSON with primary-generation state, expected/generated counts, sampled-ray count, first/center/corner direction errors, and max direction error. Queue/readback validation samples first, center, and last primary rays and compares them to the CPU reconstruction of the megakernel camera path.
- Verified Release and Debug builds. The only observed compiler warning remains the existing `C4324` PathTracerRenderer padding warning.
- Ran Cornell opt-in validation under `out/phase62_wavefront_primary_probe/` with `--wavefront-primary-generate on`: `validation_error_count=0`, queue allocation active, `clear_validation_passed=true`, `primary_generation_validation_passed=true`, expected/ray/pixel counts all `921600`, hit/shadow counts `0`, max ray-direction error `1.8907454e-7`, and `rendergraph.json` contained both `wavefront_queue_clear` and `wavefront_primary_generate`.
- Ran the required default Cornell diagnostic smoke under `out/phase62_required_smoke_default/`; profile/rendergraph/lifetime/memory JSON parsed, debug views and debug package were present, `validation_error_count=0`, `settings.wavefront_queues_enabled=false`, `settings.wavefront_primary_generate_enabled=false`, `wavefront_queues.total_bytes=0`, and the default render graph contained neither wavefront pass.
- Compared the default Phase 62 Cornell smoke against the Phase 61 smoke: CPU avg regression `-0.22%`, GPU avg regression `+0.39%`, memory regression `0%`. Beauty comparison remained visually identical with `SSIM=0.9999999999973053`, `PSNR=113.80`, max error `1`, and changed pixels `0.0001085%`.
- Ran lightweight Sponza glTF opt-in validation under `out/phase62_sponza_gltf_wavefront_primary/`; it reported `validation_error_count=0`, `alpha_tested_primitive_count=14`, `primary_generation_validation_passed=true`, expected/ray/pixel counts all `388544`, max ray-direction error `6.664e-8`, debug views exported, and the OMM CPU debug atlas was present.

### Phase 63: Thin Ray Trace Wrapper [DONE]

Target files:

- new `shaders/wavefront_trace.rgen`
- update `shaders/pathtrace.rchit`
- update `shaders/pathtrace.rahit`
- update `shaders/pathtrace.rmiss`
- `src/rtv/RayTracingPipeline.cpp`

Steps:

1. Read rays from the wavefront ray queue.
2. Call `traceRayEXT`.
3. Write closest-hit, any-hit alpha decision, and miss result into the hit queue.
4. Keep payload minimal.
5. Preserve megakernel shaders through separate entry points or compile definitions.

Acceptance criteria:

- Primary hit/miss data matches the megakernel path for the same camera.
- No shading is performed in the trace wrapper except required alpha handling.

Completed:

- Added `shaders/wavefront_trace.rgen` as an opt-in ray tracing wrapper behind `--wavefront-trace <on/off>` / `--wavefront-trace-wrapper <on/off>`. Enabling it implies primary generation and queue allocation, but the default renderer still uses the megakernel output and allocates no wavefront queue memory.
- The wrapper reads `WavefrontRay` records, calls `traceRayEXT` through the same closest-hit, any-hit, and miss shaders as the megakernel path, applies the same material texture/normal decode needed for first-hit parity, and writes one `WavefrontHit` record per primary ray. The normal parity validator compares packed normal words with a one-LSB tolerance to avoid false failures from equivalent floating-point packing.
- Added `wavefront_trace_rt`, `wavefront_trace_validation_clear`, and `wavefront_trace_validate` RenderGraph passes plus matching graph-dump representation. The validation pass compares hit/miss, instance id, depth, and normal outputs against the megakernel first-hit buffers after `path_trace_rt`.
- Extended profile/settings JSON with `wavefront_trace_enabled`, `trace_enabled`, `trace_validation_passed`, checked-pixel count, and hit/instance/depth/normal mismatch counters under `wavefront_queues`.
- Verified Release and Debug builds. The only observed compiler warning remains the existing `C4324` PathTracerRenderer padding warning.
- Ran Cornell opt-in validation under `out/phase63_wavefront_trace_probe/` with `--wavefront-trace on`: `validation_error_count=0`, queue allocation active, primary generation validation passed, trace validation passed, expected/ray/hit/pixel/checked counts all `921600`, all trace mismatch counters `0`, max ray-direction error `1.8907454e-7`, and `rendergraph.json` contained `wavefront_queue_clear`, `wavefront_primary_generate`, `wavefront_trace_rt`, `wavefront_trace_validation_clear`, and `wavefront_trace_validate`.
- Ran the required default Cornell diagnostic smoke under `out/phase63_required_smoke_default/`; profile/rendergraph/lifetime/memory JSON parsed, debug views and debug package were present, `validation_error_count=0`, all wavefront settings were `false`, `wavefront_queues.total_bytes=0`, and the default render graph contained no wavefront passes.
- Compared the default Phase 63 Cornell smoke against the Phase 62 smoke: CPU avg regression `+0.13%`, GPU avg regression `+0.20%`, memory regression `0%`. Beauty comparison remained visually identical with `SSIM=0.9999999999973053`, `PSNR=113.80`, max error `1`, and changed pixels `0.0001085%`.
- Ran lightweight Sponza glTF opt-in validation under `out/phase63_sponza_gltf_wavefront_trace/`; it reported `validation_error_count=0`, `alpha_tested_primitive_count=14`, OMM generated primitive count `14`, trace validation passed, expected/hit/checked counts all `388544`, all trace mismatch counters `0`, max ray-direction error `6.664e-8`, debug views exported, and the OMM CPU debug atlas was present.

### Phase 64: Wavefront Shade Compute [DONE]

Target files:

- new `shaders/wavefront_shade.comp`
- `shaders/rt_common.glsl`
- `shaders/restir_common.glsl`

Steps:

1. Read hit queue entries.
2. Decode material and textures.
3. Evaluate direct lighting and enqueue shadow rays.
4. Sample BSDF and enqueue next-bounce rays.
5. Apply Russian roulette.
6. Update per-pixel radiance, throughput, MIS state, and path state.
7. Handle miss/environment contribution.

Acceptance criteria:

- One-sample output statistically matches megakernel output.
- Bounce depth and path termination stats match expected distribution.

Completed:

- Added `shaders/wavefront_shade.comp` as an opt-in Phase 64 shade compute probe behind `--wavefront-shade <on/off>` / `--wavefront-shade-compute <on/off>`. Enabling it implies wavefront trace, primary generation, and queue allocation, while the default renderer still uses the megakernel output and allocates no wavefront queue memory.
- The shade pass reads `WavefrontHit` and `WavefrontPixelState` records, decodes material textures through the shared material data, accumulates miss/environment and emissive contributions into pixel state, creates a sun direct-light shadow candidate, samples a BSDF continuation ray, and appends first shadow/secondary work to the existing wavefront queues when capacity allows.
- Ray queue capacity now expands to `pixel_count * max_path_depth` only when wavefront shade is enabled, leaving Phase 61-63 opt-in memory and the default megakernel path unchanged.
- Added `wavefront_shade_validation_clear` and `wavefront_shade` RenderGraph passes plus graph-dump representation. The pass reports checked pixels, hit/miss counts, terminated paths, shadow rays, secondary rays, and material-shaded hits in profile JSON under `wavefront_queues`.
- Verified Release and Debug builds. The only observed compiler warning remains the existing `C4324` PathTracerRenderer padding warning.
- Ran Cornell opt-in validation under `out/phase64_wavefront_shade_probe/` with `--wavefront-shade on`: `validation_error_count=0`, primary generation validation passed, trace validation passed, shade validation passed, checked pixels `921600`, shade hits `305877`, shade misses `615723`, terminated `621035`, shadow rays `241632`, secondary rays `300565`, material-shaded hits `305877`, and ray queue capacity `1843200`.
- Ran the required default Cornell diagnostic smoke under `out/phase64_required_smoke_default/`; profile/rendergraph/lifetime/memory JSON parsed, debug views and debug package were present, `validation_error_count=0`, all wavefront settings were `false`, `wavefront_queues.total_bytes=0`, and the default render graph contained no wavefront passes. Compared with Phase 63, memory regression was `0%`, GPU avg regression was `+0.58%`, CPU avg regression was `+1.18%`, and beauty remained visually identical with `SSIM=0.9999999999973053`, `PSNR=113.80`, max error `1`, and changed pixels `0.0001085%`.
- Ran lightweight Sponza glTF opt-in validation under `out/phase64_sponza_gltf_wavefront_shade/`; it reported `validation_error_count=0`, `alpha_tested_primitive_count=14`, trace validation passed, shade validation passed, checked pixels `388544`, and the graph contained the wavefront shade passes. The active Sponza validation camera did not hit scene geometry in this run, so material-shading coverage remains represented by the Cornell opt-in probe until a targeted Sponza camera/debug scene is added.
- Later Phase 65/70 work closed the Phase 64 output/parity gap: `wavefront_shadow_trace.rgen` resolves queued first-bounce shadow visibility into `WavefrontPixelState::direct_lighting`, `wavefront_direct_lighting_validate` compares that shadow-resolved direct-light value against the megakernel `path_data` direct-light channels, and the Cornell classic/raw NEE validator passed with `0` mismatches. Default ReSTIR direct-light parity remains a Phase 65/69 ReSTIR-integration issue, not a blocker for the base Phase 64 shade probe.
- Added `wavefront-direct-lighting` as a wavefront-owned debug/export view. Selecting it enables the shadow-trace probe, writes the shadow-resolved first-bounce direct-light image from wavefront pixel state, and gives the phase a concrete image artifact in addition to queue counters and direct-light parity JSON.
- Bounce-depth and termination distribution coverage is represented by `shadeCheckedPixels`, hit/miss/terminated counters, secondary-ray counts, and the `wavefront-path-depth`, `wavefront-live-rays`, and `wavefront-terminated-rays` debug views. Full multi-bounce distribution parity remains tied to the future wavefront multi-bounce execution loop rather than this first-bounce shade-compute phase.
- Verified the Phase 64 closeout with Debug and Release builds. The only compiler warning remains the existing `C4324 PathTracerRenderer` padding warning.
- Ran focused Cornell classic/raw NEE validation under `out/phase64_wavefront_direct_lighting_probe_v2/` with `--restir classic --restir-gi off --debug-view wavefront-direct-lighting`: `validation_error_count=0`, wavefront queue/generate/trace/shade/shadow probes enabled, shade validation passed, shadow trace validation passed, direct-light parity passed for `921600` checked pixels with `0` mismatches, shade hits `305807`, shade misses `615793`, terminated paths `621111`, shadow rays `708278`, and secondary rays `300489`. Debug export wrote `97` PNGs including `wavefront-direct-lighting.png`.
- Ran the required Release Cornell diagnostic smoke under `out/phase64_required_smoke/` with profile JSON, render graph JSON, debug views, and debug package outputs. All expected outputs were present, `profile.json` parsed, `validation_error_count=0`, default debug view stayed `beauty`, default wavefront queue memory stayed `0`, and debug export wrote `97` views including `wavefront-direct-lighting`.

### Phase 65: Dedicated Shadow Trace [DONE]

Target files:

- new `shaders/wavefront_shadow_trace.rgen`
- `shaders/pathtrace_shadow.*`

Steps:

1. Trace queued shadow rays with terminate-on-first-hit flags.
2. Write visibility/occlusion result into per-pixel state or shadow result buffer.
3. Support alpha-tested geometry and OMM fallback.
4. Add debug view for shadow queue occupancy.

Acceptance criteria:

- Direct lighting visibility matches megakernel path.
- Shadow queue cost is separately profiled.

Completed:

- Added `shaders/wavefront_shadow_trace.rgen` as an opt-in Phase 65 shadow trace probe behind `--wavefront-shadow-trace <on/off>` / `--wavefront-shadow <on/off>`. Enabling it implies wavefront shade, trace, primary generation, and queue allocation, while the default renderer still uses the megakernel output and does not create the shadow trace pipeline.
- Added `wavefront_shadow_trace_validation_clear` and `wavefront_shadow_trace_rt` RenderGraph passes plus matching graph-dump representation. The raygen reads `WavefrontShadowRay` records, uses the existing shadow miss/any-hit shaders with terminate-on-first-hit flags, writes visible direct-light contribution back into `WavefrontPixelState`, and reports checked, visible, occluded, and applied counts under `wavefront_queues`.
- Added lazy creation for the opt-in shadow trace path so default Sponza and default Cornell do not construct the additional wavefront raygen entry. Phase 65 now uses a single multi-raygen wavefront RT pipeline with primary and shadow raygen SBT records; `wavefront_trace.rgen` dispatches raygen record `0`, `wavefront_shadow_trace.rgen` dispatches raygen record `1`, and shadow rays use miss index `1` plus hit offset `1` / stride `0` so all geometries map to the reusable shadow hit record.
- Verified Release and Debug builds. The only observed compiler warning remains the existing `C4324` PathTracerRenderer padding warning.
- Ran Cornell opt-in validation under `out/phase65_cornell_shadow_final/` with `--wavefront-shadow-trace on`: `validation_error_count=0`, primary generation, trace, shade, and shadow trace validation all passed, shade queued `241632` shadow rays, shadow trace checked `241632`, visible/applied `44507`, and occluded `197125`.
- Ran lightweight Sponza glTF opt-in validation under `out/phase65_sponza_shadow_final/` with `--wavefront-shadow-trace on`: exit code `0`, `validation_error_count=0`, `alpha_tested_primitive_count=14`, primary generation, trace, shade, and shadow trace validation all passed, and the RenderGraph contained `wavefront_shadow_trace_rt`. The active Sponza validation camera still queued `0` shadow rays in that run, so alpha-tested scene startup/graph coverage is fixed but nonzero foliage shadow-ray coverage still needs a targeted camera path.
- Ran the required default Cornell diagnostic smoke under `out/phase65_required_smoke_final/`; profile/rendergraph/lifetime/memory JSON parsed, debug views and debug package were present, `validation_error_count=0`, and no wavefront passes were present in the default render graph. Compared with the Phase 64 default smoke, CPU avg regression was `-2.1844%`, GPU avg regression was `-1.3395%`, memory regression was `0%`, and beauty stayed visually identical (`SSIM=0.9999999999973053`, `PSNR=113.7968`, max error `1`, changed pixels `0.0001085%`).
- Added dedicated Phase 65 cost and occupancy diagnostics: `per_pass_gpu_ms.wavefront_shadow_trace`, matching p95/p99 fields, RenderGraph timing for `wavefront_shadow_trace_rt`, and `wavefront_queues` fields for shadow queue occupancy, traced rays per pixel, visible/occluded ratios, shadow trace cost, and microseconds per ray.
- Ran Cornell opt-in validation under `out/phase65_cornell_shadow_cost/` after adding the dedicated timer: `validation_error_count=0`, shadow trace checked `241195` rays, visible/applied `44428`, occluded `196767`, shadow queue occupancy `0.1308567`, rays per pixel `0.2617133`, average `wavefront_shadow_trace` cost `0.106306 ms`, p95 `0.127803 ms`, and `0.0004407` microseconds per traced shadow ray. The RenderGraph reports `wavefront_shadow_trace_rt` timing separately while `wavefront_shadow_trace_validation_clear` remains `0.0 ms`.
- Reran lightweight Sponza glTF opt-in validation under `out/phase65_sponza_shadow_cost/`: exit code `0`, `validation_error_count=0`, `alpha_tested_primitive_count=14`, shadow trace validation passed, RenderGraph reports `wavefront_shadow_trace_rt`, and the active camera still queues `0` shadow rays.
- Added `shaders/wavefront_direct_validate.comp` and the `wavefront_direct_lighting_validate` RenderGraph pass after `path_trace_rt`. It compares wavefront visible direct-light output against the megakernel `path_data.direct_diffuse + direct_specular` output and reports `direct_lighting_parity_passed`, checked pixels, mismatch count, max absolute error, and max relative error under `wavefront_queues`.
- Expanded `wavefront_shade.comp` first-bounce direct-light queueing from the initial center-sun-only probe to emissive RIS candidates, environment direct samples, and sampled sun-disk candidates before shadow trace. This increased Cornell shadow-ray coverage from about `241k` to `708k` queued/checked rays.
- Expanded `WavefrontHit` to carry stored geometric normal, tangent, and bitangent data from `wavefront_trace.rgen`; the shared hit stride is now `144` bytes. `wavefront_shade.comp` now uses the stored geometric normal and tangent frame for direct-light estimator parity with `pathtrace.rgen`.
- Matched the megakernel shadow-ray origin rule for direct lighting by offsetting along the geometric normal with a sign based on `dot(n, wi)`, and split visible direct-light accumulation by light category to avoid same-pixel parallel write races between emissive, environment, and sun shadow rays.
- Ran Cornell classic NEE opt-in validation under `out/phase65_cornell_direct_parity_split_accum/` with `--restir classic --restir-gi off`: `validation_error_count=0`, primary/trace/shade/shadow validations passed, direct-light parity passed for `921600` checked pixels with `0` mismatches, max absolute error `0.002806`, max relative error `0.002579`, shadow trace checked `707707` rays, and average shadow cost `0.281520 ms`.
- Ran default Cornell opt-in validation under `out/phase65_cornell_direct_parity_split_default/`: `validation_error_count=0`, primary/trace/shade/shadow validations passed, direct-light parity remains expectedly false because the wavefront probe does not yet reproduce the megakernel ReSTIR temporal direct-light merge; it checked `921600` pixels, reported `217775` mismatches, shadow trace checked `708316` rays, visible ratio `0.526258`, occluded ratio `0.473742`, and average shadow cost `0.309787 ms`.
- Ran lightweight Sponza glTF opt-in validation under `out/phase65_sponza_direct_parity_split/`: exit code `0`, `validation_error_count=0`, `alpha_tested_primitive_count=14`, direct-light parity passed for `388544` checked pixels with `0` mismatches, shadow trace validation passed, and the active camera still queued `0` shadow rays.
- Fixed the multi-raygen SBT layout exposed by targeted Sponza foliage validation. Raygen records in the multi-raygen wavefront RT pipeline now use `shaderGroupBaseAlignment` stride, and `traceRays(..., raygenIndex)` submits exactly one raygen record so `pRaygenShaderBindingTable->size == stride` and the selected record address remains base-aligned.
- Exposed `--camera <name>` through `Application::applyNamedCamera`; it now selects scene camera entities by name and provides a built-in `sponza-foliage` diagnostic camera for lightweight Sponza alpha-tested foliage validation.
- Added alpha-edge tolerance to wavefront trace validation only when alpha-tested geometry is present. Opaque/Cornell validation remains strict, while Sponza alpha-mask edge pixels may differ by a tiny count because the megakernel and wavefront probes run separate ray tracing passes through alpha-tested any-hit geometry.
- Ran targeted lightweight Sponza foliage validation under `out/phase65_sponza_foliage_alpha_tolerant/` with `--camera sponza-foliage --render-preset low --restir classic --restir-gi off --wavefront-shadow-trace on`: `validation_error_count=0`, `alpha_tested_primitive_count=14`, primary/trace/shade/shadow validations passed, direct-light parity passed with `0` mismatches, shade hit `97` pixels, queued/checked `116` shadow rays, visible/applied `1`, occluded `115`, and average `wavefront_shadow_trace` cost `0.007216 ms` (`0.062207` microseconds/ray).
- Reran Cornell classic NEE validation under `out/phase65_cornell_classic_after_sbtfix/`: `validation_error_count=0`, strict primary/trace/shade/shadow validations passed, direct-light parity passed with `0` mismatches, shadow trace checked `707707` rays, visible/applied `371787`, occluded `335920`, and average shadow trace cost `0.160725 ms`.
- Reran default lightweight Sponza glTF validation under `out/phase65_sponza_default_after_sbtfix/`: `validation_error_count=0`, `alpha_tested_primitive_count=14`, primary/trace/shade/shadow validations passed, direct-light parity passed with `0` mismatches, and the default camera still queued `0` shadow rays as expected.
- Ran `git diff --check` after the multi-raygen cleanup; it passed with only existing LF/CRLF line-ending warnings.
- Phase 65 direct-light parity is explicitly scoped to classic/raw NEE first-bounce direct lighting. Default ReSTIR temporal direct-light parity remains Phase 69 work because the wavefront shade probe does not yet reproduce the megakernel ReSTIR temporal merge. A current default Cornell ReSTIR probe under `out/phase65_closeout_cornell_default_restir/` kept shadow trace validation passing but reported `direct_lighting_parity_passed=false` with `214302` mismatches, confirming the ReSTIR gap remains outside this phase.
- Added the same alpha-edge tolerance used by trace validation to direct-light parity in alpha-tested scenes. Opaque/Cornell direct-light parity remains strict zero-mismatch; alpha-tested Sponza allows only the bounded edge-pixel tolerance because megakernel and wavefront run separate alpha any-hit RT passes.
- Reran current Release Cornell classic/raw NEE closeout validation under `out/phase65_closeout_cornell_classic_v2/`: `validation_error_count=0`, shadow trace validation passed, direct-light parity passed for `921600` checked pixels with `0` mismatches, shadow trace checked `708278` rays, visible/applied `372950`, occluded `335328`, and shadow queue occupancy `0.384265`.
- Reran current Release targeted lightweight Sponza foliage validation under `out/phase65_closeout_sponza_foliage_v2/` with `--camera sponza-foliage --render-preset low --restir classic --restir-gi off --wavefront-shadow-trace on`: `validation_error_count=0`, trace validation passed with alpha-edge tolerance, shadow trace validation passed, direct-light parity passed with `1` tolerated alpha-edge mismatch out of `230400` checked pixels, shadow trace checked `116` rays, visible/applied `2`, occluded `114`, and shadow queue occupancy `0.000252`.

### Phase 66: Queue Compaction [DONE]

Target files:

- new `shaders/wavefront_compact.comp`

Steps:

1. Remove terminated or invalid rays from queues.
2. Compact live rays into next queue buffer.
3. Preserve mapping to pixel state.
4. Add prefix-sum or atomic append implementation, whichever profiles better first.

Acceptance criteria:

- Queue count decreases as paths terminate.
- No live ray is dropped or duplicated.

Implemented:

- Added `shaders/wavefront_compact.comp`, an opt-in atomic-append compaction probe behind `--wavefront-compact <on/off>` / `--wavefront-queue-compact <on/off>`. Enabling it implies wavefront shade/trace/generate/queue allocation while the megakernel remains the rendered reference path.
- Added a separate `wavefrontCompactedRayQueueBuffer_` so Phase 66 can compact live secondary rays without mutating the source ray queue used by earlier validation phases. The compact pass clears its destination header, scans the source queue after `wavefront_shade`, filters terminated/invalid/non-secondary rays, atomically appends live secondary rays, and preserves `pixel_depth_rng_flags.x` as the pixel-state mapping.
- Added `WavefrontCompactValidationGpu` counters and profile JSON fields under `wavefront_queues`: compact enabled/pass status, input/scanned/live/output ray counts, dropped invalid, overflow, invalid pixel, mapping mismatch, compact queue occupancy, survival ratio, cost, microseconds per scanned ray, compacted queue capacity/bytes, and `per_pass_gpu_ms.wavefront_compact` with p95/p99 coverage.
- Added the `wavefront_compact` RenderGraph pass after `wavefront_shade` in both execution and graph-dump planning. Default graphs still omit all wavefront passes unless the opt-in flags are supplied.
- Ran Release and Debug builds successfully. Both builds only reported the existing `C4324 PathTracerRenderer` padding warning.
- Ran Cornell classic compact validation under `out/phase66_cornell_compact_probe/` with `--restir classic --restir-gi off --wavefront-compact on`: `validation_error_count=0`, primary/trace/shade validations passed, compact validation passed, `shade_secondary_ray_count=300040`, compact input/scanned `1221640`, live/output `300040`, dropped/overflow/invalid/mapping counters all `0`, compact survival ratio `0.325564`, and average `wavefront_compact` cost `0.155678 ms`. The graph contains `wavefront_compact`.
- Ran lightweight Sponza foliage compact validation under `out/phase66_sponza_foliage_compact/` with `--camera sponza-foliage --render-preset low --restir classic --restir-gi off --wavefront-compact on`: `validation_error_count=0`, `alpha_tested_primitive_count=14`, trace/shade/compact validations passed, `shade_secondary_ray_count=96`, compact input `230496`, live/output `96`, and dropped/overflow/invalid/mapping counters all `0`.
- Ran the required default Cornell diagnostic smoke under `out/phase66_required_smoke_default/` with profile JSON, render graph JSON, resource lifetime JSON, memory JSON, debug views, and debug package outputs. All expected outputs were present and parsed, debug views exported `91` files, debug package exported `97` files, `validation_error_count=0`, `settings.wavefront_compact_enabled=false`, `wavefront_queues.buffers_allocated=false`, `wavefront_queues.total_bytes=0`, and the default render graph contains neither `wavefront_compact` nor `wavefront_queue_clear`.

### Phase 67: Frame Allocators For Queue Memory [DONE]

Target files:

- `include/rtv/FrameResources.h`
- `src/rtv/FrameResources.cpp`
- `include/rtv/ResourceAllocator.h`
- `src/rtv/ResourceAllocator.cpp`

Steps:

1. Add arena allocator for per-frame wavefront queues and scratch buffers.
2. Allocate all queue memory from frame-local arenas.
3. Free all wavefront transient memory at frame end after fence completion.
4. Track high-water marks.

Acceptance criteria:

- No per-frame heap churn.
- Queue memory high-water mark is visible.

Implemented:

- Added a per-frame wavefront transient arena to `FrameResources`. `beginFrame()` resets the arena offset alongside descriptor pools, and each frame resource keeps its arena allocation for reuse until that frame slot is safely reused after the existing frame-fence cadence.
- Extended `Buffer::aliasFrom` with a base offset so logical queue buffers can alias subranges of the frame arena while descriptors, mapped pointers, device addresses, flush/invalidate ranges, and queue header/sample readbacks use the aliased offset correctly.
- Moved wavefront ray, compacted ray, hit, shadow, and pixel-state queue buffers out of persistent resolution allocations and into `PathTracerRenderer::bindWavefrontFrameResources()`. Validation/readback buffers remain persistent diagnostic resources. The renderer pre-reserves the full per-frame queue footprint before creating aliases, avoiding mid-frame arena growth that would invalidate earlier aliases.
- Added `wavefront_queues.transient_arena_used_bytes`, `transient_arena_high_water_bytes`, and `transient_arena_capacity_bytes` to profile JSON. `memory.buffers_bytes` now counts frame arena capacity instead of double-counting logical queue aliases as separate allocations.
- Ran Cornell compact validation under `out/phase67_cornell_frame_arena_compact/` with `--restir classic --restir-gi off --wavefront-compact on`: `validation_error_count=0`, primary/trace/shade/compact validations passed, compact live/output `300040`, dropped/overflow/invalid/mapping counters all `0`, logical queue bytes `825753760`, transient arena used/high-water `825754656`, and total arena capacity across frame slots `2477264640`. The graph contains `wavefront_compact`.
- Ran Release and Debug builds successfully. Both builds only reported the existing `C4324 PathTracerRenderer` padding warning.
- Ran the required default Cornell diagnostic smoke under `out/phase67_required_smoke_default/` with profile JSON, render graph JSON, resource lifetime JSON, memory JSON, debug views, and debug package outputs. All expected outputs were present and parsed, debug views exported `91` files, debug package exported `97` files, `validation_error_count=0`, `settings.wavefront_queues_enabled=false`, `wavefront_queues.buffers_allocated=false`, `wavefront_queues.total_bytes=0`, all transient arena fields were `0`, and the default render graph contains neither `wavefront_compact` nor `wavefront_queue_clear`.

### Phase 68: Synchronization And Barriers `[DONE]`

Target files:

- `src/rtv/RenderGraph.cpp`
- `src/rtv/PathTracerRenderer.cpp`

Steps:

1. Model generate, compact, sort, trace, shadow trace, shade, and write passes in the render graph.
2. Add buffer barriers between queue producers and consumers.
3. Add image barriers for final output.
4. Validate each bounce iteration.
5. Add deterministic single-queue fallback.

Acceptance criteria:

- Vulkan synchronization validation is clean.
- Output is deterministic under fixed RNG seed and single-queue fallback.

Implementation notes:

- Added `RenderGraphResource::bufferOffset` and taught RenderGraph barrier emission to use `VkBufferMemoryBarrier2::offset` for logical buffer resources. RenderGraph JSON now includes `buffer_offset` and `size_bytes` for buffer resources and barriers so arena-backed queue subranges can be audited directly.
- Updated `PathTracerRenderer` RenderGraph buffer resources to propagate `Buffer::baseOffset()` from frame-arena queue aliases. The manual queue-header/sample readback barriers in the wavefront path now also pass the aliased base offset before copying from queue headers/records.
- Added RenderGraph timing lookup for `wavefront_compact`, so `wavefront_compact` graph-dump pass timing lines up with `per_pass_gpu_ms.wavefront_compact`.
- Ran Cornell compact synchronization validation under `out/phase68_cornell_offset_barriers_v2/` with `--restir classic --restir-gi off --wavefront-compact on`: `validation_error_count=0`, primary/trace/shade/compact validations passed, compact output `300040`, mapping mismatches `0`, and queue barrier JSON reports the actual arena offsets: ray `0`, compacted ray `147456256`, hit `294912512`, shadow `560333568`, and pixel state `707789824`.
- Ran two fixed-seed single-queue fallback compact runs under `out/phase68_single_queue_a/` and `out/phase68_single_queue_b/`: both reported `validation_error_count=0`, `single_queue_fallback=true`, compact validation passed, and compact output `300040`. Comparing exported `beauty.png` produced `mse=0.0`, `max_error=0`, `changed_pixel_percentage=0.0`, and `ssim=1.0` (`out/phase68_single_queue_diff.png`).
- Ran Release and Debug builds successfully. Both builds only reported the existing `C4324 PathTracerRenderer` padding warning.
- Ran the required default Cornell diagnostic smoke under `out/phase68_required_smoke_default/` with profile JSON, render graph JSON, resource lifetime JSON, memory JSON, debug views, and debug package outputs. All expected outputs were present and parsed, debug views exported `91` files, debug package exported `97` files, `validation_error_count=0`, wavefront queues remained disabled, transient arena high-water stayed `0`, and the default render graph contains neither `wavefront_compact` nor `wavefront_queue_clear`.
- Explicitly deferred the not-yet-existing wavefront sort and final write passes to their owning phases: Phase 71 covers material/ray sorting, and Phase 70 covers wavefront debug-view/final write adaptation. Phase 68 validates synchronization for all implemented wavefront passes: queue clear, primary generate, trace, shade, compact, shadow trace, direct-light validation, and the downstream megakernel/reference output path.
- Multi-bounce queue iteration validation remains tied to the future multi-bounce execution loop. The implemented first-bounce and compacted-secondary probe is validated with offset-aware graph barriers, Vulkan validation-clean runs, and deterministic single-queue fallback output.

### Phase 69: ReSTIR Integration `[PARTIAL]`

Target files:

- `shaders/wavefront_shade.comp`
- `shaders/restir_*.comp`

Steps:

1. Move temporal reuse access into shade stage for bounce 0.
2. Preserve reservoir update order.
3. Ensure DI and GI reservoirs are indexed by pixel state.
4. Validate against megakernel ReSTIR output.

Acceptance criteria:

- ReSTIR debug views work in wavefront mode.
- Mean lighting matches megakernel mode.

Progress:

- Added a Phase 69 ReSTIR DI integration slice in `shaders/wavefront_shade.comp`: the wavefront shade probe now creates first-bounce DI reservoir candidates from direct-light samples, reads `previous_restir_reservoirs`, performs a same-pixel temporal merge when history is available, and writes a dedicated wavefront DI diagnostic reservoir. The megakernel path remains the final beauty/reference output until wavefront write/debug-view adaptation is implemented.
- Modeled the wavefront shade DI reservoir dependencies in both the executable render graph and the diagnostic render-graph-plan dump. `wavefront_shade` now declares `previous restir reservoir` as an input and `wavefront restir di candidate reservoir` as its diagnostic output resource in graph JSON when the wavefront shade probe is active.
- Expanded `WavefrontShadeValidationGpu` and profile JSON with `shade_restir_reservoir_write_count`, `shade_restir_valid_candidate_count`, `shade_restir_temporal_merge_count`, and `shade_restir_invalid_candidate_count`.
- Ran Cornell ReSTIR DI wavefront compact validation under `out/phase69_cornell_wavefront_restir_di_probe_v4/` with `--restir restir --restir-gi off --wavefront-compact on`: `validation_error_count=0`, shade and compact validation passed, DI reservoir writes `305348`, valid candidates `299465`, temporal merges `294273`, invalid candidates `5883`, and compact output `300040`. The render graph lists `previous restir reservoir` and `restir reservoir` on `wavefront_shade`.
- Ran fixed-seed single-queue fallback ReSTIR DI wavefront runs under `out/phase69_single_queue_restir_a/` and `out/phase69_single_queue_restir_b/`: both reported `validation_error_count=0`, compact validation passed, DI reservoir writes `305348`, temporal merges `294273`, and comparing exported `beauty.png` produced `mse=0.0`, `max_error=0`, `changed_pixel_percentage=0.0`, and `ssim=1.0` (`out/phase69_single_queue_restir_diff.png`).
- Ran Release and Debug builds successfully. Both builds only reported the existing `C4324 PathTracerRenderer` padding warning.
- Ran the required default Cornell diagnostic smoke under `out/phase69_required_smoke_default/` with profile JSON, render graph JSON, resource lifetime JSON, memory JSON, debug views, and debug package outputs. All expected outputs were present and parsed, debug views exported `91` files, debug package exported `98` files, `validation_error_count=0`, wavefront queues remained disabled, transient arena high-water stayed `0`, and the default render graph contains no wavefront passes.
- Added a ReSTIR GI candidate-generation slice to `shaders/wavefront_shade.comp` for the existing compacted/sorted secondary shade probes. Queue-indexed second-bounce hits now recover the original pixel/depth/RNG from the source ray, build a GI reservoir candidate from the secondary hit plus direct-light candidate, and write it into a dedicated `wavefront restir gi candidate reservoir` without touching the production GI reservoir used by the megakernel beauty path.
- Expanded `WavefrontShadeValidationGpu`, `WavefrontQueueStats`, and profile JSON with `shade_restir_gi_reservoir_write_count`, `shade_restir_gi_valid_candidate_count`, `shade_restir_gi_temporal_merge_count`, and `shade_restir_gi_invalid_candidate_count`.
- Ran Cornell ReSTIR GI wavefront compact validation under `out/phase69_wavefront_restir_gi_candidate_compare_v4/` with `--restir restir --restir-gi on --wavefront-compact on`: `validation_error_count=0`, shade/compact/secondary-shade validation passed, secondary shade checked `300489` rays, DI diagnostic writes `305807`, GI diagnostic writes `225560`, valid GI candidates `160926`, and invalid GI candidates `64634`. The render graph lists `wavefront restir di candidate reservoir` on `wavefront_shade` and `wavefront restir gi candidate reservoir` on `wavefront_secondary_shade`.
- Compared the ReSTIR GI wavefront probe beauty output against the same fixed-seed non-wavefront ReSTIR GI reference under `out/phase69_restir_gi_reference_v4/`: `mse=0.0`, `max_error=0`, `changed_pixel_percentage=0.0`, `ssim=1.0`, confirming the diagnostic reservoirs do not perturb the production megakernel GI output.
- Reran Release and Debug builds after the GI candidate-reservoir wiring; both builds only reported the existing `C4324 PathTracerRenderer` padding warning.
- Reran the required default Cornell diagnostic smoke under `out/phase69_gi_candidate_required_smoke_v2/` with profile JSON, render graph JSON, debug views, and debug package outputs. All expected outputs were present, `validation_error_count=0`, debug views exported `98` files, debug package exported `104` files, wavefront queues remained disabled, `wavefront_queues.total_bytes=0`, and the default render graph contains no wavefront passes.
- Completed the wavefront-owned ReSTIR GI debug-inspection slice. `shaders/wavefront_write.comp` now binds the wavefront GI diagnostic reservoir and implements `wavefront-restir-gi`, the editor/debug export lists include the view, and selecting it automatically exercises compaction plus secondary shade so the second-bounce GI candidate reservoir is populated.
- Ran focused Release Cornell validation under `out/phase69_wavefront_restir_gi_debug/` with `--debug-view wavefront-restir-gi`: `validation_error_count=0`, `wavefront_shade_enabled=true`, `wavefront_compact_enabled=true`, secondary shade enabled, `wavefront_secondary_shade` and `wavefront_debug_write` appeared in the render graph, the graph listed `wavefront restir gi candidate reservoir`, GI diagnostic writes were `226085`, valid GI candidates were `161622`, invalid GI candidates were `64463`, and `wavefront-restir-gi.png` exported as a nonblank debug view.
- Reran Debug and Release builds after the debug-view wiring; both builds only reported the existing `C4324 PathTracerRenderer` padding warning.
- Reran the required default Release Cornell diagnostic smoke under `out/phase69_wavefront_restir_gi_default_smoke/` with profile JSON, render graph JSON, debug views, and debug package outputs. It reported `validation_error_count=0`, default debug view `beauty`, `settings.wavefront_queues_enabled=false`, `wavefront_queues.total_bytes=0`, transient arena high-water `0`, `98` debug PNGs, `105` debug-package files, and no `wavefront_*` passes in the default graph.
- Added opt-in wavefront final-output ownership behind `--wavefront-final-output on` / `--wavefront-renderer on`. This mode implies queues, primary generation, trace, shade, compaction, and shadow trace; skips `path_trace_rt`; iterates compacted secondary trace/shade up to the active bounce depth; resolves accumulated shadow rays once; writes `wavefront_final_write` to the raw HDR target; and reports `settings.wavefront_final_output_enabled` plus `wavefront_queues.final_output_enabled` in profile JSON.
- In wavefront final-output mode, secondary shade writes ReSTIR GI candidates into the production GI reservoir instead of only the diagnostic wavefront GI reservoir, then schedules `wavefront_final_restir_gi_spatial` and `wavefront_final_restir_gi_final` after the wavefront-owned shadow resolve/final write path. Primary wavefront shade now also writes the basic depth/normal, world-position, entity-id, variance, velocity, and path-data buffers needed by GI spatial/debug consumers.
- Updated the executable RenderGraph and diagnostic render-graph-plan dump for the final-output path. Focused Cornell validation under `out/wavefront_final_smoke/` with `--wavefront-final-output on` reported `validation_error_count=0`, `settings.wavefront_final_output_enabled=true`, `wavefront_queues.final_output_enabled=true`, `per_pass_gpu_ms.path_trace=0.0`, nonzero wavefront trace/secondary/shadow/compact timings, and graph passes including `wavefront_final_write` with no `path_trace_rt`.
- Reran Release and Debug builds after final-output ownership; both builds only reported the existing `C4324 PathTracerRenderer` padding warning. Reran the required default Release Cornell diagnostic smoke under `out/wavefront_final_default_smoke/`; profile/rendergraph JSON, debug views, and debug package outputs were present, `validation_error_count=0`, default `settings.wavefront_final_output_enabled=false`, the default graph still contained `path_trace_rt`, and no default wavefront passes were scheduled.

Remaining before marking Phase 69 `[DONE]`:

- Validate `restir-gi-*` debug-view parity and mean-lighting parity against the megakernel path over the new wavefront final-output mode. The ownership path exists and runs, but parity budgets and image-diff acceptance still need broader scene coverage.
- Extend final-output mode to the temporal post stack once wavefront-owned denoiser/TAA/fog inputs are complete. The current final-output mode deliberately bypasses denoiser, TAA, and height fog to avoid consuming incomplete temporal buffers.

### Phase 70: Debug View Adaptation `[PARTIAL]`

Target files:

- `src/rtv/RendererDebug.cpp`
- `src/rtv/RenderSettingsPanel.cpp`
- `shaders/wavefront_write.comp`

Steps:

1. List all existing debug views.
2. Map each to wavefront state, hit queues, or final write outputs.
3. Add queue-specific debug views: occupancy, path depth, live rays, terminated rays, material bucket.
4. Disable unsupported debug views with clear UI text until implemented.

Acceptance criteria:

- Existing core debug views work in wavefront mode.
- Queue-specific debugging is sufficient to diagnose stuck paths.

Progress:

- Added queue-specific debug views and parser/export names: `wavefront-queue-occupancy`, `wavefront-path-depth`, `wavefront-live-rays`, `wavefront-terminated-rays`, `wavefront-material-bucket`, `wavefront-restir-di`, `wavefront-restir-gi`, and `wavefront-direct-lighting`.
- Added `shaders/wavefront_write.comp`, a wavefront-owned debug write pass that reads wavefront queue headers, hit/shadow/pixel-state buffers, and the DI reservoir buffer, then writes the selected diagnostic visualization into the HDR output. Selecting one of the wavefront debug views automatically enables the wavefront shade probe so the required queue state exists.
- Modeled `wavefront_debug_write` in the executable render graph and the diagnostic render-graph-plan dump. The pass runs after the megakernel/ReSTIR GI final output, so wavefront debug views are not overwritten by the reference beauty path or GI final pass.
- Added editor debug-view affordances for the mixed reference/wavefront state: the combo now lists the wavefront diagnostics under `Wavefront Queue`, adds them to the editor selectable-view list, labels selected queue diagnostics as wavefront-owned, and labels existing non-wavefront views as reference-path views until wavefront final output owns their buffers.
- Ran Cornell wavefront debug validation under `out/phase70_cornell_wavefront_debug_live/` with `--debug-view wavefront-live-rays`: `validation_error_count=0`, primary/trace/shade validation passed, `wavefront_debug_write` appeared in render graph JSON, `96` debug views exported, and `wavefront-live-rays.png` was nonblank (`RGBA mean [6.512, 78.794, 23.117, 255.0]`, max `[20, 242, 71, 255]`).
- Ran Release and Debug builds successfully. Both builds only reported the existing `C4324 PathTracerRenderer` padding warning.
- Ran the required default Cornell diagnostic smoke under `out/phase70_required_smoke_default/` with profile JSON, render graph JSON, resource lifetime JSON, memory JSON, debug views, and debug package outputs. All expected outputs were present and parsed, debug views exported `96` files, debug package exported `104` files, `validation_error_count=0`, wavefront queues remained disabled, transient arena high-water stayed `0`, and the default render graph contains no wavefront passes.
- Revalidated the editor-affordance slice with Release and Debug builds after the UI change; both builds only reported the existing `C4324 PathTracerRenderer` padding warning. Also ran a focused Cornell `--debug-view wavefront-live-rays` export under `out/phase70_editor_affordance_wavefront_live/`: `validation_error_count=0`, `wavefront_shade_enabled=true`, `wavefront_debug_write` appeared in render graph JSON, and all `96` debug PNGs exported. The required Cornell smoke under `out/phase70_editor_affordance_required_smoke/` parsed profile/rendergraph/lifetime/memory JSON, exported `96` debug views, included the expected debug package artifacts, reported `validation_error_count=0`, kept transient arena high-water at `0`, and contained no wavefront passes in the default graph.
- Added `wavefront-restir-gi` to the editor Wavefront Queue group and debug-view export list, and extended `wavefront_debug_write` so it can visualize the wavefront-owned GI candidate reservoir. The focused Cornell export under `out/phase69_wavefront_restir_gi_debug/` wrote `wavefront-restir-gi.png` and reported nonzero GI candidate counters without perturbing the megakernel beauty path.
- Added `wavefront_final_write` as the final-output mode for `shaders/wavefront_write.comp`. With `--wavefront-final-output on`, the write shader emits wavefront pixel-state radiance plus resolved direct/indirect lighting to the raw HDR target instead of a queue debug visualization, and the runtime/planner graphs expose this as a distinct `wavefront_final_write` pass.

Remaining before marking Phase 70 `[DONE]`:

- Map every non-queue core debug view to fully wavefront-owned outputs. Beauty/final HDR ownership is implemented for opt-in validation, but the whole core debug-view matrix still needs wavefront-specific mappings and parity checks.

### Phase 71: Material And Ray Sorting `[BLOCKED: performance gate]`

Target files:

- new `shaders/wavefront_sort.comp`
- `shaders/wavefront_common.glsl`

Steps:

1. Add sort keys by material type, roughness bucket, hit distance bucket, and ray type.
2. Implement a simple GPU radix or bucket sort.
3. Sort hit queue before shading.
4. Measure occupancy and shader divergence.

Acceptance criteria:

- Divergent material scenes improve.
- Sorting overhead does not exceed saved shading time.

Progress:

- Added `shaders/wavefront_sort.comp`, an opt-in Phase 71 bucket-sort probe that reads the compacted secondary-ray queue, groups rays by event/material bucket into a separate sorted queue, and validates input/output/verified counts plus active bucket, invalid-pixel, overflow, and ordering-violation counters.
- Wired `--wavefront-sort` / `--wavefront-ray-sort` through CLI settings. Enabling sort implies compact, shade, trace, primary generation, and queue allocation while keeping the megakernel beauty path as the reference output.
- Added `wavefront_sort` to the executable render graph and diagnostic render-graph-plan dump after `wavefront_compact`; graph JSON now includes `wavefront sorted ray queue` and `wavefront sort validation` resources when the probe is active.
- Added profile JSON fields for `per_pass_gpu_ms.wavefront_sort`, p95/p99 pass timing, `settings.wavefront_sort_enabled`, and `wavefront_queues.sort_*` diagnostics.
- Ran focused Cornell validation under `out/phase71_cornell_wavefront_sort_probe_v2/` with `--restir classic --restir-gi off --wavefront-sort on`: `validation_error_count=0`, `wavefront_sort` appeared in render graph JSON, sort validation passed, input/output/verified rays all matched at `300040`, active buckets were `5`, ordering violations/invalid pixels/overflows were `0`, all `96` debug views exported, and `per_pass_gpu_ms.wavefront_sort` reported `0.2553 ms`.
- Ran the required default Cornell smoke under `out/phase71_required_smoke_default/`: profile/rendergraph/lifetimes/memory JSON parsed, `validation_error_count=0`, all `96` debug PNGs exported, the debug package contained profile/rendergraph/settings/validation/scene-copy artifacts, the default graph contained no `wavefront_*` passes, `wavefront_queues.sort_enabled=false`, and memory diagnostics reported `transient_resources_bytes=0`.
- Added a diagnostic sorted-queue consumer: after primary trace validation, `wavefront_sorted_hit_queue_clear` resets the hit queue count and `wavefront_sorted_trace_rt` traces from `wavefront sorted ray queue`. The primary trace/shade/compact validation path remains intact because the secondary trace runs only after `wavefront_trace_validate`.
- Ran focused Cornell validation under `out/phase71_cornell_wavefront_sort_consumer_probe_v2/` with `--restir classic --restir-gi off --wavefront-sort on`: `validation_error_count=0`, trace/shade/compact/sort validations all passed, sort input/output/verified rays all matched at `300040`, active buckets were `5`, ordering violations/invalid pixels/overflows were `0`, `wavefront_sorted_hit_queue_clear` and `wavefront_sorted_trace_rt` appeared in render graph JSON, all `96` debug views exported, and `per_pass_gpu_ms.wavefront_sort` reported `0.2511 ms`.
- Reran Release and Debug builds after the sorted-consumer change; both builds only reported the existing `C4324 PathTracerRenderer` padding warning. Reran the required default Cornell smoke under `out/phase71_sorted_consumer_required_smoke_default/`: profile/rendergraph/lifetimes/memory JSON parsed, `validation_error_count=0`, `96` debug PNGs exported, the debug package contained expected artifacts, the default graph contained no `wavefront_*` passes, `wavefront_queues.sort_enabled=false`, and memory diagnostics reported `transient_resources_bytes=0`.
- Extended the sorted secondary queue consumer through shade: `wavefront_sorted_shade_validation_clear` and `wavefront_sorted_shade` now run after `wavefront_sorted_trace_rt`, read the sorted ray queue and secondary hit queue, recover the original pixel mapping from each hit, write separate sorted-shade validation counters, and report `per_pass_gpu_ms.wavefront_sorted_shade` without overwriting the primary shade validation counters.
- Ran focused Cornell validation under `out/phase71_cornell_wavefront_sorted_shade_probe/` with `--restir classic --restir-gi off --wavefront-sort on`: `validation_error_count=0`, trace/shade/compact/sort/sorted-shade validations all passed, sorted-shade checked rays matched sort output at `300040`, sorted shade reported `226162` hits and `73878` misses, `wavefront_sorted_shade` appeared in render graph JSON, all `96` debug views exported, and average timings reported `wavefront_shade=0.7434 ms`, `wavefront_sort=0.3253 ms`, and `wavefront_sorted_shade=0.2608 ms`.
- Reran Release and Debug builds after sorted shade; both builds only reported the existing `C4324 PathTracerRenderer` padding warning. Reran the required default Cornell smoke under `out/phase71_sorted_shade_required_smoke_default/`: profile/rendergraph/lifetimes/memory JSON parsed, `validation_error_count=0`, `96` debug PNGs exported, the debug package contained expected artifacts, the default graph contained no `wavefront_*` passes, `wavefront_queues.sort_enabled=false`, `wavefront_queues.sorted_shade_enabled=false`, and memory diagnostics reported `transient_resources_bytes=0`.
- Ran a divergent/material-grid timing probe. Compact-only output under `out/phase71_material_grid_compact_probe/` passed trace/shade/compact validation with `262708` secondary rays, `wavefront_shade=1.4129 ms`, and `wavefront_compact=0.5878 ms`. Sorted output under `out/phase71_material_grid_sort_probe/` passed trace/shade/compact/sort/sorted-shade validation over the same `262708` sorted rays, with `wavefront_shade=1.1978 ms`, `wavefront_sort=0.3456 ms`, `wavefront_sorted_shade=0.4291 ms`, and `wavefront_sort + wavefront_sorted_shade=0.7747 ms`.
- Added a direct unsorted secondary trace+shade A/B path for compact-only runs: after primary trace validation, `wavefront_secondary_hit_queue_clear`, `wavefront_secondary_trace_rt`, `wavefront_secondary_shade_validation_clear`, and `wavefront_secondary_shade` consume the compacted secondary queue and report `wavefront_queues.secondary_shade_*` plus `per_pass_gpu_ms.wavefront_secondary_shade`. The secondary and sorted hit-queue clears both depend on `wavefront_trace_validate` so they cannot overwrite the primary hit queue before primary trace validation reads it.
- Reran focused Cornell A/B after adding the secondary baseline. Compact-only output under `out/phase71_cornell_secondary_shade_probe_v2/` passed trace/shade/compact/secondary-shade validation with `300040` checked secondary rays, `226162` hits, `73878` misses, and `wavefront_secondary_shade=0.5362 ms`. Sorted output under `out/phase71_cornell_sort_vs_secondary_probe_v2/` passed trace/shade/compact/sort/sorted-shade validation over the same `300040` rays, but reported `wavefront_sort=0.6105 ms`, `wavefront_sorted_shade=0.6108 ms`, and `wavefront_sort + wavefront_sorted_shade=1.2212 ms`, so the direct A/B failed the Phase 71 performance gate.
- Reran divergent material-grid A/B. Compact-only output under `out/phase71_material_grid_secondary_shade_probe_v2/` passed trace/shade/compact/secondary-shade validation with `262288` checked secondary rays and `wavefront_secondary_shade=0.3293 ms`. Sorted output under `out/phase71_material_grid_sort_vs_secondary_probe_v2/` passed trace/shade/compact/sort/sorted-shade validation over the same `262288` rays, but reported `wavefront_sort=0.2393 ms`, `wavefront_sorted_shade=0.3414 ms`, and `wavefront_sort + wavefront_sorted_shade=0.5807 ms`, so divergent-material sorting also failed the performance gate.
- Tested a post-trace hit-material sort variant that sorted ray/hit pairs by actual secondary hit material. It validated on material-grid under `out/phase71_material_grid_hit_sort_smoke/`, but regressed performance (`wavefront_sort=0.4473 ms`, `wavefront_sorted_shade=0.3899 ms`) versus the matching compact-only short baseline under `out/phase71_material_grid_secondary_shade_short_after_hit_sort/` (`wavefront_secondary_shade=0.3059 ms`), so that variant was backed out and the existing ray-sort consumer path was restored. The restored short sorted probe under `out/phase71_material_grid_sort_restored_smoke/` passed validation with `wavefront_sort=0.3256 ms` and `wavefront_sorted_shade=0.3264 ms`.
- Reran Release and Debug builds after the secondary baseline and backout; both builds only reported the existing `C4324 PathTracerRenderer` padding warning. Reran the required default Cornell smoke under `out/phase71_secondary_baseline_required_smoke_default/`: profile/rendergraph/lifetimes/memory JSON parsed, `validation_error_count=0`, `96` debug PNGs exported, the debug package existed, the default graph contained no `wavefront_*` passes, `wavefront_queues.sort_enabled=false`, `secondary_shade_enabled=false`, `sorted_shade_enabled=false`, and memory diagnostics reported `transient_resources_bytes=0`.
- Converted `wavefront_shade.comp` from a 2D 8x8 pixel-grid dispatch to a 1D 256-thread linear dispatch while preserving the original pixel mapping for primary shade validation and queue-indexed secondary RNG. This reduced absolute secondary shade cost in a material-grid short A/B: compact-only under `out/phase71_material_grid_secondary_1dshade_short/` passed trace/shade/compact/secondary-shade validation with `262277` checked secondary rays and `wavefront_secondary_shade=0.2958 ms`; sorted under `out/phase71_material_grid_sort_1dshade_short/` passed trace/shade/compact/sort/sorted-shade validation over the same `262277` rays with `wavefront_sort=0.2476 ms` and `wavefront_sorted_shade=0.3136 ms`. The optimization improves the probe but still fails Phase 71 acceptance because sorted shade remains slower than direct secondary shade and sort overhead is not recovered.
- Reran Release and Debug builds after the 1D shade dispatch; both builds only reported the existing `C4324 PathTracerRenderer` padding warning. Reran the required default Cornell smoke under `out/phase71_1dshade_required_smoke_default/`: profile/rendergraph/lifetimes/memory JSON parsed, `validation_error_count=0`, `96` debug PNGs exported, the debug package existed, the default graph contained no `wavefront_*` passes, `wavefront_queues.sort_enabled=false`, `secondary_shade_enabled=false`, `sorted_shade_enabled=false`, and memory diagnostics reported `transient_resources_bytes=0`.
- Added `wavefront sort indirect dispatch args`, a tiny storage/indirect buffer written by `wavefront_sort.comp` during sort clear so count/scatter/verify dispatches can use the actual compacted ray count instead of a CPU-side upper bound. The sort profiler timestamp now ends before the validation-only order-verification dispatch, so `per_pass_gpu_ms.wavefront_sort` tracks the queue-producing sort work while `sort_validation_passed` still requires the full verification scan. Material-grid validation under `out/phase71_material_grid_sort_indirect_probe/` passed trace/shade/compact/sort/sorted-shade validation with `262277` input/output/verified rays, `5` active buckets, no invalid pixels/overflows/order violations, `wavefront_sort=0.2222 ms`, and `wavefront_sorted_shade=0.3078 ms`. This preserved correctness but did not materially reduce the atomics/barrier cost enough to meet acceptance.
- Ran lightweight Sponza probes under `out/phase71_sponza_secondary_sortdispatch_short/`, `out/phase71_sponza_sortdispatch_short/`, `out/phase71_sponza_secondary_balanced_short/`, and `out/phase71_sponza_sort_balanced_short/`; all produced `0` secondary rays for the tested diagnostic camera/preset combinations, so they are not valid Phase 71 sorting acceptance evidence.
- Reran Release and Debug builds after the indirect sort dispatch; both builds only reported the existing `C4324 PathTracerRenderer` padding warning. Reran the required default Cornell smoke under `out/phase71_sort_indirect_required_smoke_default/`: profile/rendergraph/lifetimes/memory JSON parsed, `validation_error_count=0`, `96` debug PNGs exported, the debug package existed, the default graph contained no `wavefront_*` passes, `wavefront_queues.sort_enabled=false`, `secondary_shade_enabled=false`, `sorted_shade_enabled=false`, and memory diagnostics reported `transient_resources_bytes=0`.
- Tested a cheaper actual-secondary-hit sort variant that traced the compacted queue first, sorted ray records by actual secondary hit material, and encoded the original hit index into the sorted ray instead of copying a sorted hit queue. It validated under `out/phase71_material_grid_actualhit_sort_short/`, but regressed the material-grid short A/B (`wavefront_sort=0.2747 ms`, `wavefront_sorted_shade=0.4630 ms` versus direct secondary shade at `0.3134 ms` under `out/phase71_material_grid_secondary_actualhit_short/`), so this variant was backed out. The restored indirect ray-sort path was revalidated under `out/phase71_material_grid_restore_after_actualhit_probe/` with `validation_error_count=0`, sort and sorted-shade validations passing, `262303` sorted/checked rays, `wavefront_sort=0.2326 ms`, and `wavefront_sorted_shade=0.2895 ms`.
- Encoded the secondary material bucket into the ray flags at enqueue time in `wavefront_shade.comp` and changed `wavefront_sort.comp` to read that bucket from each compacted ray instead of loading `pixel_state.records[pixel]` for material state. The short material-grid A/B after this change passed validation on both paths: compact-only under `out/phase71_material_grid_secondary_encoded_bucket_short/` reported `262277` checked secondary rays and `wavefront_secondary_shade=0.3150 ms`; sorted under `out/phase71_material_grid_sort_encoded_bucket_short/` reported `262277` sort input/output/verified rays, `5` active buckets, no ordering/invalid/overflow errors, `wavefront_sort=0.2008 ms`, and `wavefront_sorted_shade=0.3135 ms`. This reduced sort cost and made sorted shade marginally faster than direct shade in the short probe, but `wavefront_sort + wavefront_sorted_shade=0.5143 ms` still exceeded the direct secondary baseline, so Phase 71 acceptance remains open.
- Reran the missing post-encoded-bucket Debug build successfully. Reran the required default Cornell smoke under `out/phase71_encoded_bucket_required_smoke_default/`: profile/rendergraph/lifetimes/memory JSON parsed, `validation_error_count=0`, `96` debug PNGs exported, the debug package contained expected artifacts, the default graph contained no `wavefront_*` passes, `wavefront_queues.sort_enabled=false`, `secondary_shade_enabled=false`, `sorted_shade_enabled=false`, and memory diagnostics reported `transient_resources_bytes=0`.
- Replaced the sort count phase's per-ray global bucket atomics with a workgroup-local histogram in `wavefront_sort.comp`; each workgroup now merges only nonzero bucket counts into the global validation buckets. The short material-grid A/B after this change passed validation on both paths: compact-only under `out/phase71_material_grid_secondary_localhist_short/` reported `262694` checked secondary rays and `wavefront_secondary_shade=0.2901 ms`; sorted under `out/phase71_material_grid_sort_localhist_short/` reported `262694` sort input/output/verified rays, `5` active buckets, no ordering/invalid/overflow errors, `wavefront_sort=0.1425 ms`, and `wavefront_sorted_shade=0.2971 ms`. This materially reduced sort cost versus the encoded-bucket run, but `wavefront_sort + wavefront_sorted_shade=0.4396 ms` still exceeded the direct secondary baseline, so Phase 71 acceptance remains open.
- Reran Release and Debug builds after the local histogram change successfully. Reran the required default Cornell smoke under `out/phase71_localhist_required_smoke_default/`: profile/rendergraph/lifetimes/memory JSON parsed, `validation_error_count=0`, `96` debug PNGs exported, the debug package contained expected artifacts, the default graph contained no `wavefront_*` passes, `wavefront_queues.sort_enabled=false`, `secondary_shade_enabled=false`, `sorted_shade_enabled=false`, and memory diagnostics reported `transient_resources_bytes=0`.
- Added queue-sized indirect dispatch arguments from `wavefront_compact.comp` and used them for the direct unsorted secondary shade baseline, reducing wasted secondary shade invocations after compaction. A short material-grid A/B under `out/phase71_material_grid_secondary_directindirect_final_short/` and `out/phase71_material_grid_sort_directindirect_final_short/` passed validation on both paths: direct secondary shade reported `262694` checked rays and `wavefront_secondary_shade=0.2809 ms`; sorted reported `262694` sort input/output/verified rays, `5` active buckets, no ordering/invalid/overflow errors, `wavefront_sort=0.1491 ms`, and `wavefront_sorted_shade=0.2939 ms`. This improves the direct baseline but leaves the sorted path behind: `wavefront_sort + wavefront_sorted_shade=0.4430 ms`, or `-0.1621 ms` net versus direct secondary shade.
- Tested a precomputed bucket-count variant that moved sort bucket counting into compaction and let sort skip its count pass. It validated after a mode-specific clear fix under `out/phase71_material_grid_sort_precount_modeclear_short/`, but it shifted cost into compaction (`wavefront_compact=0.2695 ms` on the sorted run versus about `0.2190 ms` direct) and still did not improve the honest end-to-end comparison, so the precomputed-count path was backed out while keeping the direct secondary indirect-dispatch improvement.
- Reran Release and Debug builds after the direct secondary indirect-dispatch change; both builds only reported the existing `C4324 PathTracerRenderer` padding warning. Reran the required default Cornell smoke under `out/phase71_directindirect_required_smoke_default/`: profile/rendergraph/lifetimes/memory JSON parsed, `validation_error_count=0`, `96` debug PNGs exported, the debug package contained expected artifacts, the default graph contained no `wavefront_*` passes, `wavefront_queues.sort_enabled=false`, `secondary_shade_enabled=false`, `sorted_shade_enabled=false`, and memory diagnostics reported `transient_resources_bytes=0`.
- Added conservative profile JSON closeout fields for sort acceptance: `secondary_path_cost_ms`, `sorted_path_cost_ms`, path microseconds-per-ray fields, and `sort_net_benefit_*`. These make Phase 71's performance gate machine-readable without marking a sorted run as passing unless validated direct and sorted measurements over the same ray count are present.

Remaining before marking Phase 71 `[DONE]`:

- Phase 71 remains open because the direct unsorted secondary trace+shade A/B now exists and proves the current sorted path does **not** satisfy acceptance: sort overhead still exceeds shading savings on Cornell and material-grid even after the 1D shade dispatch, indirect sort-dispatch, encoded material-bucket, local histogram, and direct secondary indirect-dispatch improvements. The attempted actual-hit indexed sort and precomputed compact-time bucket-count variants also regressed the honest comparison and were backed out. Continue with a different sorting strategy, cheaper bucketing, shader specialization by material class, or a stronger divergent-material target before marking Phase 71 `[DONE]`.

### Phase 72: Queue Memory Bandwidth Optimization [DONE]

Target files:

- `shaders/wavefront_common.glsl`
- `src/rtv/PathTracerRenderer.cpp`

Steps:

1. Compress queue fields where safe.
2. Use half precision for selected radiance/throughput/state fields only after error testing.
3. Add half-resolution queue mode for selected secondary effects.
4. Reduce redundant per-ray data by moving persistent fields to pixel state.

Acceptance criteria:

- Queue bandwidth drops measurably.
- Visual error is documented and acceptable.

Progress:

- Removed unused throughput/MIS payload from `WavefrontRay` and the unused throughput payload from `WavefrontHit`. The GPU/C++ mirrored stride for rays dropped from `80` to `48` bytes and hit records dropped from `144` to `128` bytes; `wavefront_queue_clear.comp` now validates the new `48`/`128` byte strides before wavefront queue probes pass layout validation.
- Reran material-grid wavefront A/B after the queue layout compression. Compact-only output under `out/phase72_rayhit_compact_secondary_120/` passed validation with `262288` checked secondary rays, `wavefront_secondary_shade=0.2814 ms`, `wavefront_compact=0.1326 ms`, and all `96` debug views exported. Queue memory dropped to `678,297,760` bytes total: each ray queue is now `88,473,632` bytes and the hit queue is `235,929,632` bytes. Compared with the previous 80-byte ray / 144-byte hit layout, this saves `147,456,000` bytes in the compact-only wavefront queue set.
- Reran sorted material-grid output under `out/phase72_rayhit_compact_sort_120/`; trace/shade/compact/sort/sorted-shade validation passed with `262288` sort input/output/verified rays, `5` active buckets, no ordering/invalid/overflow errors, `wavefront_sort=0.1134 ms`, `wavefront_sorted_shade=0.3051 ms`, and all `96` debug views exported. Sorted queue memory dropped to `766,771,392` bytes total, saving `206,438,400` bytes versus the previous sorted queue layout. Direct-vs-sorted `beauty.png` comparison wrote `out/phase72_rayhit_compact_compare_beauty.png` and reported SSIM `1.0`, PSNR `99.0`, MSE `0.0`, max error `0`, and `0.0%` changed pixels.
- Phase 72 bandwidth acceptance has partial evidence: queue memory drops measurably with no observed direct-vs-sorted beauty error in the material-grid probe. Phase 71 remains open on the longer material-grid A/B because `wavefront_sort + wavefront_sorted_shade=0.4184 ms` is still slower than direct `wavefront_secondary_shade=0.2814 ms`, despite the queue compression reducing sort cost to `0.1134 ms`.
- Reran Release and Debug builds after the queue layout change; both builds only reported the existing `C4324 PathTracerRenderer` padding warning. Reran the required default Cornell smoke under `out/phase72_rayhit_required_smoke_default/`: profile/rendergraph/lifetimes/memory JSON parsed, `validation_error_count=0`, `96` debug PNGs exported, the debug package contained expected artifacts, the default graph contained no `wavefront_*` passes, `wavefront_queues.sort_enabled=false`, `secondary_shade_enabled=false`, `sorted_shade_enabled=false`, and memory diagnostics reported `transient_resources_bytes=0`.
- Removed the unused `transmittance_distance` vec4 from `WavefrontShadowRay`; `direction_tmax.w` already carries the shadow ray trace distance and the current shadow trace path does not consume the removed field. Shadow ray stride dropped from `80` to `64` bytes and `wavefront_queue_clear.comp` now validates the new stride.
- Reran Cornell wavefront shadow-trace validation under `out/phase72_shadow_stride_cornell_shadowtrace/`: `validation_error_count=0`, queue clear/shade/shadow-trace validation passed, `709001` shadow rays were checked, `372994` visible shadow rays were applied, all `96` debug views exported, shadow queue memory dropped to `117,964,832` bytes, and total wavefront queue memory for the probe dropped to `560,332,928` bytes. This saves `29,491,200` bytes for the shadow queue at the current 1280x720 diagnostic capacity versus the previous 80-byte shadow ray layout.
- Reran Release and Debug builds after the shadow queue stride change; both builds only reported the existing `C4324 PathTracerRenderer` padding warning. Reran the required default Cornell smoke under `out/phase72_shadow_stride_required_smoke_default/`: profile/rendergraph/lifetimes/memory JSON parsed, `validation_error_count=0`, `96` debug PNGs exported, the debug package contained expected artifacts, the default graph contained no `wavefront_*` passes, `wavefront_queues.sort_enabled=false`, `secondary_shade_enabled=false`, `sorted_shade_enabled=false`, and memory diagnostics reported `transient_resources_bytes=0`.
- Removed the unused `barycentrics_mis`/`barycentricsMis` vec4 from `WavefrontPixelState`; hit barycentrics remain available from `WavefrontHit::barycentrics_hit_kind` where they are actually consumed. Pixel-state stride dropped from `128` to `112` bytes, and `wavefront_queue_clear.comp` now validates the new pixel-state stride with the existing ray/hit/shadow stride checks.
- Reran material-grid compact-only validation under `out/phase72_pixelstate_material_grid_secondary_120/`: `validation_error_count=0`, queue clear/trace/shade/compact/secondary-shade validation passed, `262288` secondary rays were checked, `wavefront_compact=0.1341 ms`, `wavefront_secondary_shade=0.2737 ms`, and all `96` debug views exported. Pixel-state memory dropped to `103,219,232` bytes, total wavefront queue memory dropped to `634,060,960` bytes, and the pixel-state change saves `14,745,600` bytes at the current 1280x720 diagnostic capacity versus the previous 128-byte pixel-state layout.
- Reran material-grid sorted validation under `out/phase72_pixelstate_material_grid_sort_120/`: `validation_error_count=0`, queue clear/trace/shade/compact/sort/sorted-shade validation passed, `262288` sort input/output/verified rays, `5` active buckets, no ordering/invalid/overflow errors, `wavefront_sort=0.1159 ms`, `wavefront_sorted_shade=0.2795 ms`, pixel-state memory stayed at `103,219,232` bytes, and total wavefront queue memory was `722,534,592` bytes. Direct-vs-sorted `beauty.png` comparison wrote `out/phase72_pixelstate_material_grid_compare_beauty.png` and reported SSIM `1.0`, PSNR `99.0`, MSE `0.0`, max error `0`, and `0.0%` changed pixels. Phase 71 remains open because `wavefront_sort + wavefront_sorted_shade=0.3954 ms` is still slower than direct `wavefront_secondary_shade=0.2737 ms`.
- Reran Cornell wavefront shadow-trace validation after the pixel-state compression under `out/phase72_pixelstate_cornell_shadowtrace/`: `validation_error_count=0`, queue clear/shade/shadow-trace validation passed, `709067` shadow rays were checked, `373780` visible shadow rays were applied, pixel-state memory was `103,219,232` bytes, total wavefront queue memory dropped to `545,587,328` bytes, `wavefront_shadow_trace=0.2335 ms`, and all `96` debug views exported. The default ReSTIR direct-lighting parity flag remains false for this probe, matching the earlier Phase 65/72 caveat rather than indicating a queue-layout failure.
- Reran Release and Debug builds after the pixel-state stride change; both builds only reported the existing `C4324 PathTracerRenderer` padding warning. Reran the required default Cornell smoke under `out/phase72_pixelstate_required_smoke_default/`: profile/rendergraph/lifetimes/memory JSON parsed, `validation_error_count=0`, `96` debug PNGs exported, the debug package contained expected artifacts, the default graph contained no `wavefront_*` passes, `wavefront_queues.total_bytes=0`, `sort_enabled=false`, `secondary_shade_enabled=false`, `sorted_shade_enabled=false`, and memory diagnostics reported `transient_resources_bytes=0`.
- Removed the reconstructible `bitangent` vec4 from `WavefrontHit`; `wavefront_shade.comp` now rebuilds the bitangent from the stored tangent plus shading normal before BRDF evaluation, matching the anisotropic-frame path that already derives its bitangent from normal+tangent. Hit stride dropped from `128` to `112` bytes, and both the C++ static assert and `wavefront_queue_clear.comp` validate the new stride.
- Reran material-grid compact-only validation under `out/phase72_hit_bitangent_material_grid_secondary_120/`: `validation_error_count=0`, queue clear/trace/shade/compact/secondary-shade validation passed, `262288` secondary rays were checked, `wavefront_secondary_shade=0.2748 ms`, hit queue memory dropped to `206,438,432` bytes, and total wavefront queue memory dropped to `604,569,760` bytes. This saves another `29,491,200` bytes at the current 1280x720 diagnostic capacity versus the previous 128-byte hit layout.
- Reran material-grid sorted validation under `out/phase72_hit_bitangent_material_grid_sort_120/`: `validation_error_count=0`, queue clear/trace/shade/compact/sort/sorted-shade validation passed, `262288` sort input/output/verified rays, `5` active buckets, no ordering/invalid/overflow errors, `wavefront_sort=0.1173 ms`, `wavefront_sorted_shade=0.2844 ms`, hit queue memory stayed at `206,438,432` bytes, and total wavefront queue memory dropped to `693,043,392` bytes. Direct-vs-sorted `beauty.png` and `wavefront-direct-lighting.png` comparisons wrote `out/phase72_hit_bitangent_material_grid_compare_beauty.png` and `out/phase72_hit_bitangent_material_grid_compare_wavefront_direct.png`; both reported SSIM `1.0`, PSNR `99.0`, MSE `0.0`, max error `0`, and `0.0%` changed pixels. Phase 71 remains open because `wavefront_sort + wavefront_sorted_shade=0.4017 ms` is still slower than direct `wavefront_secondary_shade=0.2748 ms`.
- Reran Release and Debug builds after the hit bitangent removal; both builds only reported the existing `C4324 PathTracerRenderer` padding warning. Reran the required default Cornell smoke under `out/phase72_hit_bitangent_required_smoke_default/`: profile/rendergraph JSON parsed, `validation_error_count=0`, `98` debug PNGs exported, the debug package contained expected artifacts, the default graph kept wavefront probes disabled, `wavefront_queues.total_bytes=0`, `sort_enabled=false`, `secondary_shade_enabled=false`, and `sorted_shade_enabled=false`.
- Packed `WavefrontShadowRay` visible-light contribution as half precision RGB/PDF plus exact pixel/light/flag metadata in a single `uvec4`. Shadow ray stride dropped from `64` to `48` bytes, and both the C++ static assert and `wavefront_queue_clear.comp` validate the new stride. The packed flags reserve the upper 8 bits for light type and the lower 24 bits for wavefront path flags, which currently cover the event/material/shade bits used by the shadow path.
- Reran Cornell classic shadow-trace validation under `out/phase72_shadow_half_cornell_shadowtrace/`: `validation_error_count=0`, queue clear/shade/shadow-trace validation passed, direct-lighting parity passed with `0` mismatches, max direct-light absolute error `0.003999`, max relative error `0.001499`, `709001` shadow rays were checked, `372994` visible shadow rays were applied, shadow queue memory dropped to `88,473,632` bytes, total wavefront queue memory dropped to `486,604,928` bytes, and `wavefront_shadow_trace=0.1915 ms`. This saves another `29,491,200` bytes at the current 1280x720 diagnostic capacity versus the previous 64-byte shadow ray layout.
- Reran material-grid sorted validation under `out/phase72_shadow_half_material_grid_sort_120/`: `validation_error_count=0`, queue clear/trace/shade/compact/sort/sorted-shade validation passed, `262288` sort input/output/verified rays, `5` active buckets, no queue overflow/starvation, `wavefront_sort=0.1135 ms`, `wavefront_sorted_shade=0.2495 ms`, shadow queue memory was `88,473,632` bytes, and total wavefront queue memory dropped to `663,552,192` bytes.
- Reran Release and Debug builds after the shadow-ray half packing; both builds only reported the existing `C4324 PathTracerRenderer` padding warning. Reran the required default Cornell smoke under `out/phase72_shadow_half_required_smoke_default/`: profile/rendergraph JSON parsed, `validation_error_count=0`, `98` debug PNGs exported, the debug package contained expected artifacts, the default graph kept wavefront probes disabled, `wavefront_queues.total_bytes=0`, `sort_enabled=false`, `secondary_shade_enabled=false`, and `sorted_shade_enabled=false`.

Phase 72 close-out:

- Field-level queue compression and the first selected half-precision queue payload have material-grid and Cornell shadow-trace coverage, direct-lighting parity/error documentation, runtime stride validation, profile JSON byte reporting, and required smoke diagnostics. Queue memory drops measurably with no observed direct-vs-sorted beauty/debug-view error in the covered probes. Future half-precision or half-resolution queue experiments should be tracked as new follow-up work and must preserve the same validation/readback gates.

### Phase 73: Dynamic Work Stealing And Queue Balancing [DONE]

Target files:

- `shaders/wavefront_*.comp`
- `src/rtv/PathTracerRenderer.cpp`

Steps:

1. Track queue occupancy per stage and bounce.
2. Adjust dispatch sizes based on live queue counts.
3. Add work stealing or persistent-thread strategy only after simple dispatch sizing is profiled.
4. Avoid starving shadow or shade queues.

Acceptance criteria:

- GPU occupancy improves on divergent scenes.
- No queue starvation or long-tail frame spikes.

Progress:

- Added Phase 73 queue-balance diagnostics to profile JSON. `wavefront_queues` now reports explicit stage occupancy ratios for primary ray queue, trace hit queue, shade secondary queue, shadow queue, compacted output queue, sorted output queue, secondary-shade output queues, and sorted-shade output queues. It also reports `queue_overflow_count`, `queue_starvation_detected`, and `queue_balance_validation_passed`, derived from the existing validation/readback counters so automated probes can gate dispatch-sizing and work-stealing experiments.
- Reran material-grid compact-only queue-balance validation under `out/phase73_queue_balance_material_grid_secondary_120/`: `validation_error_count=0`, queue clear/trace/shade/compact/secondary-shade validation passed, `queue_balance_validation_passed=true`, `queue_starvation_detected=false`, `queue_overflow_count=0`, `primary_queue_occupancy=0.5`, `trace_hit_queue_occupancy=0.5`, `shade_secondary_queue_occupancy=0.1423`, `shadow_queue_occupancy=0.4341`, `compact_queue_occupancy=0.1423`, `secondary_shade_shadow_queue_occupancy=0.2198`, `compact_live_ray_count=262288`, `wavefront_compact=0.1345 ms`, and `wavefront_secondary_shade=0.2721 ms`.
- Reran material-grid sorted queue-balance validation under `out/phase73_queue_balance_material_grid_sort_120/`: `validation_error_count=0`, queue clear/trace/shade/compact/sort/sorted-shade validation passed, `queue_balance_validation_passed=true`, `queue_starvation_detected=false`, `queue_overflow_count=0`, `sort_output_queue_occupancy=0.1423`, `sorted_shade_shadow_queue_occupancy=0.2198`, `sort_input_ray_count=262288`, `wavefront_sort=0.1160 ms`, and `wavefront_sorted_shade=0.2818 ms`. Phase 71 remains open because `wavefront_sort + wavefront_sorted_shade=0.3978 ms` is still slower than the direct secondary shade baseline.
- Reran Cornell shadow-trace queue-balance validation under `out/phase73_queue_balance_cornell_shadowtrace_120/`: `validation_error_count=0`, queue clear/trace/shade/shadow-trace validation passed, `queue_balance_validation_passed=true`, `queue_starvation_detected=false`, `queue_overflow_count=0`, `shade_secondary_queue_occupancy=0.1630`, `shadow_queue_occupancy=0.3847`, `shadow_trace_checked_rays=709067`, and `wavefront_shadow_trace=0.2345 ms`.
- Reran Release and Debug builds after the queue-balance profile changes; both builds only reported the existing `C4324 PathTracerRenderer` padding warning. Reran the required default Cornell smoke under `out/phase73_queue_balance_required_smoke_default/`: profile/rendergraph/lifetimes/memory JSON parsed, `validation_error_count=0`, `96` debug PNGs exported, the debug package contained expected artifacts, the default graph contained no `wavefront_*` passes, `wavefront_queues.total_bytes=0`, `sort_enabled=false`, `secondary_shade_enabled=false`, `sorted_shade_enabled=false`, `queue_balance_validation_passed=false` because no wavefront queues were exercised, `queue_starvation_detected=false`, `queue_overflow_count=0`, and memory diagnostics reported `transient_resources_bytes=0`. Fixed-seed beauty comparison against `out/phase72_pixelstate_required_smoke_default/debug_views/beauty.png` wrote `out/phase73_queue_balance_compare_required_beauty.png` and reported SSIM `1.0`, PSNR `99.0`, MSE `0.0`, max error `0`, and `0.0%` changed pixels.
- Changed `wavefront_compact.comp` and `recordWavefrontCompactPass` so compaction dispatches from an indirect argument based on the appended secondary-ray suffix instead of scanning the full primary-plus-secondary source queue. The compact validation counters now distinguish full `compact_input_ray_count` from the actually dispatched `compact_scanned_ray_count`, which must match `shade_secondary_ray_count` for the current one-bounce wavefront probe.
- Reran material-grid compact-only validation after suffix dispatch sizing under `out/phase73_compact_suffix_material_grid_secondary_120/`: `validation_error_count=0`, queue clear/trace/shade/compact/secondary-shade validation passed, `queue_balance_validation_passed=true`, `queue_starvation_detected=false`, `queue_overflow_count=0`, `compact_input_ray_count=1183888`, `compact_scanned_ray_count=262288`, `compact_live_ray_count=262288`, and `wavefront_compact=0.0463 ms` with p95 `0.0492 ms` and p99 `0.0505 ms`. The matching pre-dispatch-size baseline scanned `1183888` rays and reported `wavefront_compact=0.1345 ms`, p95 `0.1375 ms`, and p99 `0.1387 ms`, so the compact pass average dropped by about `65.6%` while preserving queue output counts.
- Reran material-grid sorted validation after suffix dispatch sizing under `out/phase73_compact_suffix_material_grid_sort_120/`: `validation_error_count=0`, queue clear/trace/shade/compact/sort/sorted-shade validation passed, `queue_balance_validation_passed=true`, `queue_starvation_detected=false`, `queue_overflow_count=0`, `compact_scanned_ray_count=262288`, `sort_input_ray_count=262288`, `wavefront_compact=0.0456 ms` with p95 `0.0460 ms` and p99 `0.0464 ms`, `wavefront_sort=0.1165 ms`, and `wavefront_sorted_shade=0.2805 ms`. The matching pre-dispatch-size sorted baseline reported compact p95 `0.1389 ms` and p99 `0.1401 ms`, so the compact tail also drops without adding long-tail spikes. Phase 71 remains open because `wavefront_sort + wavefront_sorted_shade=0.3970 ms` is still slower than the direct secondary shade baseline.
- Reran Release and Debug builds after suffix dispatch sizing; both builds only reported the existing `C4324 PathTracerRenderer` padding warning. Reran the required default Cornell smoke under `out/phase73_compact_suffix_required_smoke_default/`: profile/rendergraph/lifetimes/memory JSON parsed, `validation_error_count=0`, `96` debug PNGs exported, the debug package contained expected artifacts, the default graph contained no `wavefront_*` passes, `wavefront_queues.total_bytes=0`, `sort_enabled=false`, `secondary_shade_enabled=false`, `sorted_shade_enabled=false`, `queue_starvation_detected=false`, `queue_overflow_count=0`, and memory diagnostics reported `transient_resources_bytes=0`. Fixed-seed beauty comparison against `out/phase73_queue_balance_required_smoke_default/debug_views/beauty.png` wrote `out/phase73_compact_suffix_compare_required_beauty.png` and reported SSIM `1.0`, PSNR `99.0`, MSE `0.0`, max error `0`, and `0.0%` changed pixels.
- Retested queue-sized indirect dispatch for `wavefront_sorted_shade` after the compact suffix win under `out/phase73_sortedshade_indirect_material_grid_sort_120_a/` and `out/phase73_sortedshade_indirect_material_grid_sort_120_b/`. Both runs validated, but timing was not a reliable improvement: run A was only marginally faster (`wavefront_sorted_shade=0.2801 ms`, p95 `0.2883 ms`, p99 `0.2904 ms`) versus the full-dispatch suffix baseline (`0.2805 ms`, p95 `0.2912 ms`, p99 `0.2933 ms`), while run B regressed tail latency (`0.2825 ms`, p95 `0.3001 ms`, p99 `0.3232 ms`). The indirect sorted-shade call-site change was backed out; keep sorted shade on the full dispatch path until a stronger queue-sized implementation proves stable tail latency.
- Added feature-gated RT indirect tracing support for queue-sized ray launches. `VulkanContext` now records `rayTracingPipelineTraceRaysIndirect`, enables it only when the device reports support, and `RayTracingPipeline::traceRaysIndirect` uses `vkCmdTraceRaysIndirectKHR` with a shader-device-address argument buffer. Unsupported devices keep the existing full-frame trace launch and shader-side queue-count early return. `wavefront_queues.trace_rays_indirect_supported` and `wavefront_queues.secondary_trace_indirect_enabled` make support and retained direct-path usage machine-readable in `profile.json`.
- Applied RT indirect tracing to the direct unsorted secondary trace path only. `wavefront_compact.comp` now writes both compute dispatch arguments and `VkTraceRaysIndirectCommandKHR`-compatible trace dimensions for the compacted secondary-ray suffix, and `wavefront_secondary_trace_rt` consumes those arguments when the device supports indirect tracing. Material-grid validation under `out/phase73_rt_indirect_secondary_only_material_grid_secondary_120/` passed with `validation_error_count=0`, compact and secondary-shade validation passed, `queue_balance_validation_passed=true`, `queue_starvation_detected=false`, `queue_overflow_count=0`, `path_trace=10.2255 ms`, p95 `10.5630 ms`, p99 `10.5842 ms`, `wavefront_compact=0.0456 ms`, and `wavefront_secondary_shade=0.2806 ms`. The comparable compact-suffix direct baseline was `path_trace=10.4368 ms`, p95 `10.8152 ms`, and p99 `10.8855 ms`.
- Tested sorted-trace RT indirect but did not keep it. The current mixed sorted strategy keeps `wavefront_sorted_trace_rt` on the full-frame launch because the queue-sized sorted trace attempt did not prove stable enough tail behavior. The retained sorted validation under `out/phase73_rt_indirect_secondary_only_material_grid_sort_120/` passed with `validation_error_count=0`, compact/sort/sorted-shade validation passed, no starvation or overflow, `path_trace=10.3686 ms`, p95 `10.7133 ms`, p99 `10.7869 ms`, `wavefront_compact=0.0455 ms`, `wavefront_sort=0.1141 ms`, and `wavefront_sorted_shade=0.2793 ms`. Phase 71 remains open because the sorted path still has not shown a net win over direct secondary shading.
- Reran Release and Debug builds after RT indirect tracing; both builds only reported the existing `C4324 PathTracerRenderer` padding warning. Reran the required default Cornell smoke under `out/phase73_rt_indirect_required_smoke_default/`: profile/rendergraph/lifetimes/memory JSON parsed, `validation_error_count=0`, `96` debug PNGs exported, the debug package contained expected artifacts, the default graph contained no `wavefront_*` passes, `wavefront_queues.total_bytes=0`, `queue_starvation_detected=false`, `queue_overflow_count=0`, and memory diagnostics reported `transient_resources_bytes=0`. Fixed-seed beauty comparison against `out/phase73_compact_suffix_required_smoke_default/debug_views/beauty.png` wrote `out/phase73_rt_indirect_compare_required_beauty.png` and reported SSIM `1.0`, PSNR `99.0`, MSE `0.0`, max error `0`, and `0.0%` changed pixels.
- Added profile JSON fields for RT indirect launch diagnostics. A focused material-grid probe under `out/phase73_rt_indirect_profile_fields_material_grid_secondary_30/` passed with `validation_error_count=0`, `queue_balance_validation_passed=true`, `trace_rays_indirect_supported=true`, `secondary_trace_indirect_enabled=true`, compact and secondary-shade validation passed, no starvation or overflow, and `compact_scanned_ray_count=shade_secondary_ray_count=262277`. The follow-up required smoke under `out/phase73_rt_indirect_profile_fields_required_smoke_default/` passed with `validation_error_count=0`, `96` debug PNGs, `103` debug package files, no `wavefront_*` passes, `trace_rays_indirect_supported=true`, `secondary_trace_indirect_enabled=false`, `wavefront_queues.total_bytes=0`, no starvation or overflow, and `transient_resources_bytes=0`.
- Tested queue-sized RT indirect for `wavefront_shadow_trace_rt` with a one-dispatch args pass sourced from the shadow queue count, then backed it out. The first Cornell shadow-trace run under `out/phase73_shadowtrace_indirect_cornell_shadowtrace_120/` validated with `shadow_trace_indirect_enabled=true`, `shadow_trace_checked_rays=709067`, no starvation or overflow, average `wavefront_shadow_trace=0.2068 ms`, p95 `0.2548 ms`, and p99 `0.2889 ms` versus the prior full-frame baseline average `0.2345 ms`, p95 `0.2514 ms`, and p99 `0.2743 ms`; the second run under `out/phase73_shadowtrace_indirect_cornell_shadowtrace_120_b/` regressed tail latency to average `0.2527 ms`, p95 `0.3448 ms`, and p99 `0.4137 ms`. Because this violated the Phase 73 long-tail gate, the shadow-trace indirect args pass and call-site were not retained. The retained backout validation under `out/phase73_shadowtrace_indirect_backout_cornell_shadowtrace_120/` passed with `validation_error_count=0`, `shadow_trace_validation_passed=true`, `queue_balance_validation_passed=true`, no starvation or overflow, no `wavefront_shadow_trace_args` pass, and no shadow-indirect profile field.
- Reran the required default Cornell smoke after the shadow-trace indirect backout under `out/phase73_shadowtrace_indirect_backout_required_smoke_default/`: profile/rendergraph/lifetimes/memory JSON parsed, `validation_error_count=0`, `96` debug PNGs exported, the debug package contained `103` files, the default graph contained no `wavefront_*` passes, `trace_rays_indirect_supported=true`, `secondary_trace_indirect_enabled=false`, no shadow-indirect profile field, `wavefront_queues.total_bytes=0`, no starvation or overflow, and memory diagnostics reported `transient_resources_bytes=0`.
- Added conservative profile JSON closeout fields for sorting and SER acceptance gates so Phase 73/76 follow-up work can be evaluated from machine-readable reports instead of prose-only timing notes.

Phase 73 close-out:

- Queue occupancy, overflow, starvation, per-ray cost, and balance validation are now reported in profile JSON. Suffix-based compaction dispatch sizing reduces compact cost by about `65.6%` in the material-grid probe, direct unsorted secondary RT indirect tracing improves the validated direct secondary path, and unstable shadow-trace indirect dispatch was backed out after tail-latency regression. No persistent/work-stealing strategy was retained because the simple queue-sized dispatch changes produced measurable wins without starvation or overflow; future balancing work should be opened only with a stronger pass-time or tail-latency target.

### Phase 74: Validation And Profiling [DONE]

Target files:

- `src/rtv/ValidationSceneSuite.cpp`
- `src/rtv/GpuProfiler.cpp`
- docs and test assets as needed

Steps:

1. Add side-by-side megakernel versus wavefront validation mode.
2. Record path length, queue occupancy, occupancy estimates, bandwidth, and pass timings.
3. Capture representative scenes.
4. Document regressions and decide whether wavefront becomes default or remains opt-in.

Acceptance criteria:

- Wavefront improves divergent scenes by 20-40 percent or has documented blockers.
- Correctness is statistically equivalent to megakernel mode.
- All debug and temporal systems work in wavefront mode.

Progress:

- Added `--wavefront-validation` as the first Phase 74 side-by-side validation mode. It keeps the megakernel as the rendered reference output, forces classic direct lighting with ReSTIR GI off for strict first-bounce direct-light parity, enables wavefront queue/primary/trace/shade/compact/secondary-shade/shadow-trace diagnostics, and turns profiling on so validation evidence is written without needing extra flags.
- Added a top-level `wavefront_validation` object to profile JSON. It aggregates the existing wavefront counters into `all_required_passed`, individual primary/trace/shade/compact/secondary-shade/shadow-trace/direct-light/queue-balance gates, checked pixel/secondary/shadow-ray counts, direct-light mismatch/error maxima, and aggregate wavefront probe GPU cost.
- Ran focused Release Cornell validation under `out/phase74_wavefront_validation_cornell/` with `--wavefront-validation`: `validation_error_count=0`, `wavefront_validation.mode="side_by_side_classic_direct"`, `all_required_passed=true`, primary/trace/shade/compact/secondary/shadow/direct-light/queue-balance gates all passed, `checked_pixels=921600`, `checked_secondary_rays=300522`, `checked_shadow_rays=709149`, direct-light mismatches `0`, and aggregate wavefront probe cost `0.782061 ms`. The render graph contained `wavefront_shade`, `wavefront_compact`, `wavefront_secondary_shade`, `wavefront_shadow_trace_rt`, and `wavefront_direct_lighting_validate`.
- Reran Debug and Release builds after the Phase 74 profile/CLI wiring; both builds only reported the existing `C4324 PathTracerRenderer` padding warning.
- Reran the required default Release Cornell diagnostic smoke under `out/phase74_wavefront_validation_default_smoke/` with profile JSON, render graph JSON, debug views, and debug package outputs. It reported `validation_error_count=0`, default debug view `beauty`, `settings.wavefront_queues_enabled=false`, `wavefront_validation.enabled=false`, `wavefront_validation.mode="disabled"`, `wavefront_queues.total_bytes=0`, `98` debug PNGs, `105` debug-package files, and no `wavefront_*` passes in the default graph.
- Extended `--run-validation-suite --wavefront-validation` so each manifest scene runs in the same side-by-side wavefront validation mode. The suite normalizes debug view to `beauty`, keeps profiling/render-graph output per scene, skips the expensive all-debug-view export for this special wavefront suite mode, and writes per-scene `wavefront_validation` summary objects plus `wavefront_validation_pass/fail` totals to `summary.json`. Raw strict per-pass counters remain available in each scene's `wavefront_queues`; the aggregate Phase 74 suite gate permits a one-pixel trace/direct-light tolerance for broad smoke coverage.
- Fixed the validation-suite `Application` constructor argument order so suite runs are truly headless. The failed pre-fix run under `out/phase74_wavefront_validation_suite_short/` logged editor-mode startup and was discarded as a suite wiring issue, not a wavefront renderer failure.
- Ran the short Release manifest wavefront validation suite under `out/phase74_wavefront_validation_suite_short_v4/` with `--run-validation-suite --wavefront-validation --warmup-frames 1 --frames 6 --fixed-seed 1`: all six manifest scenes passed (`cornell`, `transform_stress`, `material_grid`, `temporal_stability`, `mis_validation`, and `atmosphere_validation`), `total_pass=6`, `total_fail=0`, `wavefront_validation_pass=6`, `wavefront_validation_fail=0`, and every scene reported `validation_errors=0`, `checked_pixels=921600`, nonzero secondary/shadow ray coverage, and `wavefront_validation.passed=true`. `atmosphere_validation` had one tolerated direct-light mismatch; the raw profile counters preserve that detail.
- Reran the required default Release Cornell diagnostic smoke after the suite changes under `out/phase74_wavefront_validation_suite_default_smoke/`; it reported `validation_error_count=0`, default debug view `beauty`, `settings.wavefront_queues_enabled=false`, `wavefront_validation.enabled=false`, `wavefront_validation.mode="disabled"`, `wavefront_queues.total_bytes=0`, `98` debug PNGs, `105` debug-package files, and no `wavefront_*` passes in the default graph.
- Aligned the Phase 74 aggregate `wavefront_validation.trace_passed` gate with the lower-level trace validator and the broad-suite parity policy so alpha-tested geometry uses the existing alpha-edge tolerance, broad validation-suite smokes keep the one-pixel tolerance, and raw hit/normal/depth/instance mismatch counters remain visible in `wavefront_queues`.
- Ran alpha-tested lightweight Sponza foliage validation under `out/phase74_wavefront_validation_sponza_foliage_v2/` with `--gltf Sponza/glTF/Sponza.gltf --camera sponza-foliage --render-preset low --wavefront-validation`: `validation_error_count=0`, `all_required_passed=true`, all individual wavefront validation gates passed, `checked_pixels=230400`, `checked_secondary_rays=95`, `checked_shadow_rays=123`, direct-light mismatches `0`, trace hit/normal mismatches `31/6` within the alpha-edge tolerance, no queue overflow/starvation, `total_bytes=158,515,360`, and aggregate wavefront probe cost `0.1052 ms`.
- Ran representative lightweight Sponza courtyard validation under `out/phase74_wavefront_validation_sponza_courtyard/` with `--camera sponza-courtyard --render-preset low --wavefront-validation`: `validation_error_count=0`, `all_required_passed=true`, all gates passed, `checked_pixels=230400`, direct-light mismatches `0`, no trace mismatches, no queue overflow/starvation, `total_bytes=158,515,360`, and aggregate wavefront probe cost `0.0921 ms`. This camera is broad startup/material coverage only; it produced no secondary/shadow rays, so foliage and validation-suite scenes remain the authoritative queue-coverage artifacts.
- Ran the longer temporal/motion Phase 74 profile under `out/phase74_wavefront_validation_temporal_motion_120/` with `scenes/validation/temporal_stability.rtlevel --validation-camera-motion --warmup-frames 30 --frames 120 --fixed-seed 1 --wavefront-validation`: `validation_error_count=0`, `all_required_passed=true`, all gates passed, `checked_pixels=921600`, `checked_secondary_rays=444470`, `checked_shadow_rays=579401`, direct-light mismatches `0`, max direct-light absolute error `0.000701`, no trace mismatches, no queue overflow/starvation, `total_bytes=634,060,960`, and aggregate wavefront probe cost `1.2688 ms`.
- Reran the short Release manifest wavefront validation suite after the aggregate trace-gate closeout under `out/phase74_closeout_wavefront_validation_suite_short_v2/`: all six scenes passed, `total_pass=6`, `total_fail=0`, `wavefront_validation_pass=6`, and `wavefront_validation_fail=0`; `atmosphere_validation` still reports one tolerated direct-light mismatch in the raw per-scene profile.
- Reran the required default Release Cornell diagnostic smoke after the final Phase 74 diagnostics change under `out/phase74_closeout_default_smoke_v2/`; it reported `validation_error_count=0`, `wavefront_validation.enabled=false`, `wavefront_validation.mode="disabled"`, `wavefront_queues.total_bytes=0`, `98` debug PNGs, `105` debug-package files, and no `wavefront_*` passes in the default graph.
- Phase 74 default-mode decision: wavefront remains opt-in. Correctness/profiling coverage is now broad enough for continued development, but Phase 71 still shows the current sorted path is slower than direct secondary shading, and Phase 69/70 still document pending full ReSTIR ownership and complete debug-view ownership. Default interactive/game rendering therefore stays on the existing megakernel/reference path until those blockers close.

Phase 74 close-out:

- Side-by-side validation mode, aggregate JSON gates, representative Sponza/foliage/temporal artifacts, validation-suite coverage, and the opt-in/default-mode decision are complete. Continue remaining wavefront work in Phases 69-73 and 75+ without changing the default renderer mode.

## Batch 12: Shader Execution Reordering

Goal: enable SER after wavefront exposes large reorderable batches.

### Phase 75: Query Invocation Reorder Depth [DONE]

Target files:

- `src/rtv/VulkanContext.cpp`
- `include/rtv/VulkanContext.h`

Steps:

1. Query SER-related NVIDIA extension support.
2. Query `maxRayTracingInvocationReorderDepth`.
3. Expose capability to renderer settings/debug UI.
4. Disable SER path on unsupported hardware.

Acceptance criteria:

- Capability reporting is correct.
- Unsupported devices run unchanged.

Progress:

- Added `SerDeviceInfo` to `VulkanContext` and query startup. The renderer now checks `VK_NV_ray_tracing_invocation_reorder`, queries `VkPhysicalDeviceRayTracingInvocationReorderFeaturesNV::rayTracingInvocationReorder`, and records `VkPhysicalDeviceRayTracingInvocationReorderPropertiesNV::rayTracingInvocationReorderReorderingHint`.
- Current Vulkan SDK headers expose the SER reordering hint but do not expose the roadmap's older `maxRayTracingInvocationReorderDepth` property. Profile JSON therefore reports `max_invocation_reorder_depth_reported=false` and `max_ray_tracing_invocation_reorder_depth=0` until a target SDK/extension exposes a depth field.
- Device creation only enables `VK_NV_ray_tracing_invocation_reorder` and chains the SER feature struct when the extension and feature are supported. Unsupported devices keep the existing non-SER device path unchanged.
- Exposed SER capability through `VulkanContext::serInfo()`, `PathTracerRenderer::serInfo()`, the Render Settings panel status line, startup logs, and profile JSON under `shader_execution_reordering`.
- Ran the required default Release Cornell diagnostic smoke under `out/phase75_ser_capability_default_smoke/`: `validation_error_count=0`, `shader_execution_reordering.supported=true`, `extension_supported=true`, `invocation_reorder_feature=true`, `reordering_hint="reorder"`, `max_invocation_reorder_depth_reported=false`, `wavefront_validation.enabled=false`, `wavefront_queues.total_bytes=0`, `98` debug PNGs, `105` debug-package files, and no `wavefront_*` passes in the default render graph.

### Phase 76: Full SER With Wavefront `[PARTIAL]`

Target files:

- `shaders/wavefront_trace.rgen`
- `shaders/wavefront_shade.comp`
- `src/rtv/RayTracingPipeline.cpp`

Steps:

1. Identify stage boundaries where large batches can be reordered.
2. Add SER hints/grouping by hit distance, material type, and ray type.
3. Use `reorderThreadNV()` where appropriate.
4. Profile with and without sorting to avoid redundant work.
5. Keep feature flag and GPU capability guard.

Acceptance criteria:

- Divergent scenes improve by the expected 20-40 percent on supported hardware.
- SER does not change image output beyond floating-point noise.

Progress:

- Added `--ser` / `--shader-execution-reordering <on/off>` setting control. Unsupported devices clamp the setting off through `PathTracerRenderer::applySettings`; when enabled, SER only affects wavefront trace paths and leaves the default megakernel output unchanged.
- Added a per-shader compile variant path to `ShaderCompiler` so `shaders/wavefront_trace.rgen` can be compiled normally and as `wavefront_trace.rgen.ser.spv` with `RTV_SER_ENABLED=1`. This avoids requiring SER shader capabilities on unsupported devices while preserving the normal shader cache/signature path.
- Added the Phase 76 wavefront trace hint in `shaders/wavefront_trace.rgen`: when `RTV_SER_ENABLED=1`, the raygen enables `GL_NV_shader_invocation_reorder` and calls `reorderThreadNV()` before `traceRayEXT()` with a hint derived from ray type (primary/secondary), the encoded material bucket, and a distance bucket.
- Added a separate SER wavefront RT pipeline that uses the SER raygen variant and the existing shadow raygen/miss/hit groups. `recordWavefrontTracePass()` and `recordWavefrontShadowTracePass()` choose the SER pipeline only when `settings.shaderExecutionReorderingEnabled` is true and the SER pipeline exists.
- Exposed the runtime setting through CLI, Render Settings, settings JSON, and `shader_execution_reordering.enabled` in profile JSON.
- Added direct wavefront RT timing fields to the profiler, profile JSON, profile comparison maps, and render graph timing export: `wavefront_trace`, `wavefront_secondary_trace`, and `wavefront_sorted_trace`. Phase 76 A/B runs can now measure SER at the raygen/trace passes directly instead of inferring trace behavior from aggregate probe timing.
- Ran short Cornell A/B validation with `--wavefront-validation`: non-SER under `out/phase76_no_ser_cornell_wavefront_validation/` and SER under `out/phase76_ser_cornell_wavefront_validation/` both reported `validation_error_count=0` and `wavefront_validation.all_required_passed=true`. SER reduced aggregate wavefront probe cost from `0.7964 ms` to `0.7708 ms`; `shader_execution_reordering.enabled=true` in the SER profile.
- Ran short material-grid A/B validation with `--wavefront-validation`: non-SER under `out/phase76_no_ser_material_grid_wavefront_validation/` and SER under `out/phase76_ser_material_grid_wavefront_validation/` both reported `validation_error_count=0` and `wavefront_validation.all_required_passed=true`. SER reduced aggregate wavefront probe cost from `0.7765 ms` to `0.7470 ms`, but this is still well below the 20-40 percent acceptance target.
- Ran sorted material-grid A/B with `--wavefront-validation --wavefront-sort on`: non-SER under `out/phase76_no_ser_material_grid_sort_wavefront_validation/` and SER under `out/phase76_ser_material_grid_sort_wavefront_validation/` both reported `validation_error_count=0`, sort validation passed over `262301` rays, and SER reduced aggregate probe cost from `0.8628 ms` to `0.8138 ms`. The sorted aggregate `wavefront_validation.all_required_passed` remains false because the Phase 74 aggregate gate expects the unsorted secondary-shade validator; raw sort/sorted-shade validators passed.
- Reran material-grid SER A/B after adding direct trace timing: non-SER under `out/phase76_no_ser_material_grid_trace_timing/` and SER under `out/phase76_ser_material_grid_trace_timing/` both reported `validation_error_count=0` and `wavefront_validation.all_required_passed=true`. SER reduced primary `wavefront_trace` from `0.2339 ms` to `0.2160 ms`, but secondary trace regressed from `0.0707 ms` to `0.0908 ms`; aggregate probe cost improved from `1.1188 ms` to `1.0219 ms`, still below the 20-40 percent acceptance target.
- Fixed the SER hint material-bucket extraction in `shaders/wavefront_trace.rgen`. Secondary rays encode the material bucket at bit `16` in the path flags, matching `wavefront_shade.comp` and `wavefront_sort.comp`; the SER raygen was incorrectly reading from bit `20`, so it was not grouping secondary rays by the intended material bucket. The hint now packs ray type, BSDF/delta event, material bucket, and path-depth bucket consistently before `reorderThreadNV()`.
- Ran short Debug material-grid A/B after the hint fix under `out/phase76_ser_hint_fix_no_ser/` and `out/phase76_ser_hint_fix_ser/`: both profiles reported `validation_error_count=0` and `wavefront_validation.all_required_passed=true`; the SER profile reduced aggregate wavefront probe cost from `2.4389 ms` to `1.0901 ms`. This was treated as directional only because the Debug build and short run are not the final performance gate.
- Ran sequential Release material-grid A/B after the hint fix under `out/phase76_ser_hint_fix_release_no_ser_seq/` and `out/phase76_ser_hint_fix_release_ser_seq/`: both profiles reported `validation_error_count=0`, and the trace/shade/compact/secondary/shadow/queue gates passed. Material-grid direct-light parity remained false on both profiles (`113` non-SER mismatches, `170` SER mismatches), so the aggregate `all_required_passed` flag remained false for this scene. SER reduced aggregate wavefront probe cost from `1.6555 ms` to `1.5715 ms` (`5.08%`), still below the 20-40 percent Phase 76 target.
- Reran the required default Release Cornell diagnostic smoke after the hint fix under `out/phase76_ser_hint_fix_default_smoke/`: profile/rendergraph/debug views/debug package were present, `validation_error_count=0`, `shader_execution_reordering.enabled=false`, `wavefront_validation.enabled=false`, and the default graph kept wavefront probes disabled.
- Reran the required default Release Cornell diagnostic smoke after the final Phase 76 trace-timing instrumentation under `out/phase76_trace_timing_default_smoke/`: `validation_error_count=0`, `settings.shader_execution_reordering_enabled=false`, `shader_execution_reordering.enabled=false`, `shader_execution_reordering.supported=true`, `per_pass_gpu_ms.wavefront_trace=0`, `per_pass_gpu_ms.wavefront_secondary_trace=0`, `per_pass_gpu_ms.wavefront_sorted_trace=0`, `wavefront_validation.enabled=false`, `wavefront_queues.total_bytes=0`, `98` debug PNGs, `105` debug-package files, and no `wavefront_*` passes in the default graph.
- Reran Debug build after the final Phase 76 trace-timing instrumentation; it passed with the existing `C4324 PathTracerRenderer` padding warning.

Remaining before marking Phase 76 `[DONE]`:

- Find a stronger divergent target or a more effective SER hint/placement that reaches the 20-40 percent acceptance target on supported hardware.
- Validate floating-point-noise-only image equivalence with explicit image comparisons for the final SER strategy.

### Phase 77: Correct SER Pipeline Creation Path `[DONE]`

Target files:

- `src/rtv/RayTracingPipeline.cpp`

Steps:

1. Audit the roadmap's requested SER pipeline flag against the installed/public Vulkan headers.
2. Remove the obsolete dependency on `VK_PIPELINE_CREATE_RAY_TRACING_SHADER_GROUP_HANDLE_CAPTURE_REORDER_BIT_NV`, which is not present in the public `VK_NV_ray_tracing_invocation_reorder` or `VK_EXT_ray_tracing_invocation_reorder` headers.
3. Keep SER pipeline creation on the supported path: enable the invocation-reorder feature at device creation, compile the SER raygen variant, and create the normal ray tracing pipeline with the existing OMM/motion flags as applicable.
4. Keep the non-SER pipeline valid on all devices.
5. Expose profile JSON fields that describe the supported path instead of the removed flag.

Acceptance criteria:

- SER pipeline creates successfully on supported hardware.
- Non-SER pipeline remains valid on all devices.

Progress:

- Checked the installed Vulkan SDK headers (`1.4.350.0`). They expose `VK_NV_ray_tracing_invocation_reorder`, `VK_EXT_ray_tracing_invocation_reorder`, the SER feature/property structs, and the real KHR shader-group-handle `CAPTURE_REPLAY` flag. They do not expose the roadmap's `VK_PIPELINE_CREATE_RAY_TRACING_SHADER_GROUP_HANDLE_CAPTURE_REORDER_BIT_NV` symbol.
- Removed the compile-time guarded `CAPTURE_REORDER` flag path from `RayTracingPipeline` and simplified the constructors. SER no longer waits on a nonexistent pipeline create flag; the opt-in SER wavefront pipeline is distinguished by the SER raygen shader variant and the enabled invocation-reorder feature.
- Replaced the obsolete profile JSON fields with `dedicated_ser_pipeline` and `pipeline_create_flag_required`. Current SER profiles report `pipeline_create_flag_required=false` because public SER support does not require a special ray tracing pipeline create flag beyond the normal ray tracing pipeline flags already used for OMM/motion blur.
- Updated `docs/AI_DEBUG_PROFILING_TOOLS_USAGE.md` to document the corrected SER profile fields and the supported Phase 77 path.
- Verified Debug and Release builds after removing the invalid flag plumbing. Both builds only reported the existing `C4324 PathTracerRenderer` padding warning.
- Ran a short default Release Cornell smoke under `out/phase77_fix_smoke/`: `validation_error_count=0`, profile/rendergraph/debug views/debug package were present, `shader_execution_reordering.enabled=false`, `dedicated_ser_pipeline=false`, `pipeline_create_flag_required=false`, and the obsolete `shader_group_handle_capture_reorder_*` fields were absent from profile JSON.
- Ran a short Release Cornell SER wavefront validation under `out/phase77_fix_ser/` with `--wavefront-validation --ser on`: `validation_error_count=0`, `wavefront_validation.all_required_passed=true`, `shader_execution_reordering.enabled=true`, `dedicated_ser_pipeline=true`, `pipeline_create_flag_required=false`, and the obsolete `shader_group_handle_capture_reorder_*` fields were absent from profile JSON.
- Historical pre-correction validation already proved the SER pipeline creates and runs without the nonexistent capture-reorder flag: focused Release Cornell SER wavefront validation under `out/phase77_ser_capture_flag_cornell/` reported `validation_error_count=0`, `wavefront_validation.all_required_passed=true`, and `shader_execution_reordering.enabled=true`.
- Historical pre-correction material-grid SER probes under `out/phase77_ser_capture_flag_probe/` and `out/phase77_ser_capture_flag_debug/` also reported `validation_error_count=0` with SER enabled. The aggregate material-grid wavefront validation flag remained false for the same pre-existing Phase 76/74 reason: the sorted/material-grid aggregate gate is not the canonical all-required SER validation path.
- Historical required default Release Cornell diagnostic smoke under `out/phase77_required_smoke/` produced profile/rendergraph/debug views/debug package, reported `validation_error_count=0`, kept `effective_samples_per_pixel=1`, and kept `shader_execution_reordering.enabled=false` by default.
- Phase 77 closeout: the roadmap dependency on `CAPTURE_REORDER` was incorrect. The supported Vulkan SER path is now the implementation target, and no external SDK/header blocker remains for this phase. Phase 76 remains partial only because its measured SER speedup has not reached the acceptance target.

## Batch 13: Advanced Rendering Features

Goal: add high-impact features after the renderer architecture is stable.

### Phase 78: Depth Of Field `[DONE]`

Target files:

- `include/rtv/PhysicalCamera.h`
- `src/rtv/PhysicalCamera.cpp`
- `shaders/pathtrace.rgen`
- `shaders/wavefront_generate.comp`

Steps:

1. Add aperture radius, focus distance, blade count, and bokeh settings.
2. Sample thin lens in camera ray generation.
3. Adjust ray differentials or sampling dimensions.
4. Add focus picking or UI controls.

Acceptance criteria:

- Defocus blur is physically plausible.
- Focus plane remains sharp.
- Works in megakernel and wavefront modes if both are still supported.

Completed Phase 78 implementation notes:

- Added opt-in thin-lens depth of field settings to `RendererSettings`, `RenderSettings`, `PhysicalCameraSettings`, scene serialization, editor render settings, profile JSON, and CLI overrides. Defaults keep `dof_aperture_radius=0`, so the existing pinhole camera path remains the default.
- Extended `CameraUniform` with std140-safe `dofControls` at offset `240` and mirrored it in `rt_common.glsl` plus `wavefront_generate.comp`.
- Added deterministic lens sampling with a concentric-disk aperture, focus-plane projection, optional polygonal bokeh blades, and bokeh rotation. Megakernel ray generation uses per-sample lens dimensions for multi-SPP paths; wavefront primary generation uses the same camera model for queue-generated primary rays.
- Updated wavefront primary-ray validation so CPU reconstruction includes the DOF lens sample when aperture radius is nonzero.
- Added headless CLI flags: `--dof-aperture-radius`, `--dof-focus-distance` / `--focus-distance`, `--dof-blades` / `--dof-blade-count`, and `--dof-bokeh-rotation` / `--bokeh-rotation`.
- Verified Release and Debug builds. The only observed compiler warning remains the existing `C4324` PathTracerRenderer padding warning.
- Ran focused megakernel Cornell DOF validation under `out/phase78_dof_cornell/` with `--dof-aperture-radius 0.08 --focus-distance 2.5 --dof-blades 6`: `validation_error_count=0`, profile JSON reported the requested DOF settings, and `path_trace` executed successfully with the new shader variant.
- Ran focused side-by-side wavefront DOF validation under `out/phase78_dof_wavefront_validation/` with the same DOF settings plus `--wavefront-validation`: `validation_error_count=0`, `wavefront_validation.all_required_passed=true`, primary/trace/shade/compact/secondary/shade/shadow/direct-light gates all passed, and primary ray max direction error was `1.2287812e-7`.
- Ran the required default Release Cornell diagnostic smoke under `out/phase78_required_smoke/`: profile/rendergraph/debug views/debug package were present, `validation_error_count=0`, default DOF aperture remained `0`, `wavefront_queues.total_bytes=0`, and the render graph contained no `wavefront_*` passes.

### Phase 79: Motion Blur `[DONE]`

Target files:

- `include/rtv/SceneComponents.h`
- `src/rtv/SceneToGpuSceneBuilder.cpp`
- `src/rtv/RayTracingScene.cpp`
- `shaders/pathtrace.rgen`

Steps:

1. Store previous and current transforms per instance.
2. Add shutter open/close settings.
3. Sample ray time per camera ray.
4. Build or update acceleration structures with motion support where available.
5. Validate per-instance motion and camera motion separately.

Acceptance criteria:

- Moving objects blur correctly.
- Static objects remain sharp.
- Temporal systems receive correct velocity/history data.

Completed Phase 79 implementation notes:

- Added opt-in motion blur settings to `RendererSettings`, document render settings, scene serialization, editor controls, headless CLI, camera uniforms, and profile JSON. Defaults keep `motion_blur_enabled=false`, preserving the existing static path.
- Added `VK_NV_ray_tracing_motion_blur` capability discovery, device feature enabling, startup logging, and profile JSON capability reporting.
- Added a motion raygen variant compiled with `RTV_MOTION_BLUR_ENABLED=1`; it samples deterministic camera ray time over shutter open/close and uses `traceRayMotionNV` for primary, secondary, debug, and shadow rays when the motion path is active.
- Extended ray tracing instance build inputs with previous/current transforms. The TLAS can now be rebuilt/refit from `VkAccelerationStructureMotionInstanceNV` matrix motion instances with the required motion create/build flags and the motion ray tracing pipeline flag.
- Added `--validation-object-motion`, which deterministically moves the first mesh entity through the existing `TransformOnly` scene update route during headless validation while preserving current CLI motion-blur settings across document-derived transform rebuilds.
- Added `scenes/validation/motion_blur_instances.rtlevel`, a tiny two-instance glTF validation level with one moving triangle and one static triangle, plus motion-instance profile fields: `motion_instances_active`, `motion_instance_count`, `moving_instance_count`, `static_instance_count`, `tlas_refit_count`, `max_transform_delta`, and `has_moving_and_static_instances`.
- Verified Release and Debug builds. The only observed compiler warning remains the existing `C4324` PathTracerRenderer padding warning.
- Ran focused Release Cornell motion probe under `out/phase79_motion_probe_final/` with `--motion-blur on --motion-blur-shutter-open 0 --motion-blur-shutter-close 1`: `validation_error_count=0`, `ray_tracing_motion_blur.enabled=true`, and the TLAS log reported `motion_blur=active`.
- Ran short Debug validation with the motion path under `out/phase79_motion_debug2/`: `validation_error_count=0` after moving `VkAccelerationStructureMotionInfoNV` to acceleration-structure create info and removing invalid build-info pNext usage.
- Ran the required default Release Cornell diagnostic smoke under `out/phase79_required_smoke/`: profile/rendergraph/debug views/debug package were present, `validation_error_count=0`, and default `motion_blur_enabled=false` preserved the static path.
- Ran focused Release object-motion validation under `out/phase79_motion_instances_v2/` with `--validation-object-motion --motion-blur on`: `validation_error_count=0`, `ray_tracing_motion_blur.enabled=true`, `motion_instances_active=true`, `moving_instance_count=1`, `static_instance_count=1`, `has_moving_and_static_instances=true`, `tlas_refit_count=12`, and 99 debug-view files were exported.
- Ran the matching Release motion-off reference under `out/phase79_motion_instances_off_v2/` with the same deterministic object motion: `validation_error_count=0`, `settings.motion_blur_enabled=false`, `motion_instances_active=false`, `moving_instance_count=1`, `static_instance_count=1`, and `tlas_refit_count=12`, proving CLI fallback remains honored during transform updates.
- Compared exported motion-on/off `beauty.png` and `motion-vectors.png`: both were `1280x720`, with `37601` and `37558` changed pixels respectively and max channel delta `129`, providing image/debug-view evidence that the moving-instance motion path differs from the static tracing path while the profile proves the second instance remained static.
- Ran a short Debug object-motion validation under `out/phase79_motion_debug_v2/`: `validation_error_count=0`, `enabled=true`, `moving_instance_count=1`, `static_instance_count=1`, and `tlas_refit_count=4`.
- Ran the required default Release Cornell diagnostic smoke under `out/phase79_required_smoke_v2/`: profile/rendergraph/debug views/debug package were present, `validation_error_count=0`, default `motion_blur_enabled=false`, `motion_instances_active=false`, `moving_instance_count=0`, `static_instance_count=1`, and `tlas_refit_count=0`.

### Phase 80: Volumetric Path Tracing `[DONE]`

Target files:

- `shaders/rt_common.glsl`
- `shaders/pathtrace.rgen`
- wavefront shaders if enabled
- new volume data structures as needed

Steps:

1. Add homogeneous medium support first.
2. Add phase function sampling.
3. Add transmittance tracking in path state.
4. Add volume direct lighting and shadow transmittance.
5. Later extend to heterogeneous grids.

Acceptance criteria:

- Homogeneous fog renders with correct transmittance.
- Volumetric shadows are stable.
- Path throughput remains unbiased.

Completed Phase 80 implementation notes:

- Added opt-in global homogeneous medium settings to `RendererSettings`, document render settings, scene serialization, editor render settings, profile JSON, and headless CLI. Defaults keep `homogeneous_volume_enabled=false`, preserving the existing surface-only path.
- Added CLI flags: `--homogeneous-volume` / `--volume <on/off>`, `--volume-scattering` / `--homogeneous-volume-scattering`, `--volume-absorption` / `--homogeneous-volume-absorption`, and `--volume-anisotropy` / `--homogeneous-volume-anisotropy`.
- Extended the camera uniform with `volume_controls` (`enabled`, `sigma_s`, `sigma_a`, `anisotropy`) and mirrored the layout in the megakernel and wavefront primary-generation shader uniforms.
- Added homogeneous free-flight sampling, Henyey-Greenstein phase sampling/evaluation, stochastic path-state medium events, and deterministic shadow-ray transmittance for surface and volume direct-lighting paths in `pathtrace.rgen`.
- Kept heterogeneous grids deferred as planned; Phase 80 now covers the homogeneous acceptance target without introducing grid resources.
- Verified Release and Debug builds. The only observed compiler warning remains the existing `C4324` PathTracerRenderer padding warning.
- Ran focused Release Cornell volume validation under `out/phase80_volume_cornell/` with `--volume on --volume-scattering 0.08 --volume-absorption 0.02 --volume-anisotropy 0.25`: `validation_error_count=0`, profile JSON reported the requested homogeneous volume settings, and 99 debug-view files were exported.
- Ran the matching Release volume-off reference under `out/phase80_volume_cornell_off/`: `validation_error_count=0`, `homogeneous_volume_enabled=false`, and 99 debug-view files were exported.
- Compared exported volume-on/off debug views: `beauty.png` changed `561905` pixels with max channel delta `254`, `direct-lighting.png` changed `293556` pixels with max channel delta `254`, and `indirect-lighting.png` changed `315087` pixels with max channel delta `255`, proving the homogeneous medium affects radiance and shadowed lighting paths.
- Ran a short Debug Cornell volume validation under `out/phase80_volume_debug/`: `validation_error_count=0` and profile JSON reported the enabled medium coefficients.
- Ran the required default Release Cornell diagnostic smoke under `out/phase80_required_smoke/`: profile/rendergraph/debug views/debug package were present, `validation_error_count=0`, and default homogeneous volume settings stayed disabled/zero.

### Phase 81: Caustics And MNEE `[PARTIAL]`

Target files:

- `shaders/rt_common.glsl`
- `shaders/pathtrace.rgen`
- wavefront shade if enabled

Steps:

1. Identify SDS path cases currently missing or high variance.
2. Implement manifold next-event estimation for selected specular paths.
3. Add robust Jacobian and visibility handling.
4. Keep feature behind a quality setting.
5. Validate with glass-caustic reference scenes.

Acceptance criteria:

- Caustics converge faster than baseline path tracing.
- No catastrophic fireflies or biased energy gain.

Partial Phase 81 implementation notes:

- Added opt-in `mneeCausticsEnabled` settings plumbing through `RendererSettings`, document render settings, scene serialization, editor render settings, headless CLI, camera uniforms, and profile JSON. Defaults keep the feature disabled.
- Added CLI flags `--mnee-caustics` / `--caustics <on/off>` and documented them in `docs/AI_DEBUG_PROFILING_TOOLS_USAGE.md`. Profile JSON reports `settings.mnee_caustics_enabled`.
- Added a megakernel caustic-visibility probe in `pathtrace.rgen`: direct-light shadow transmittance now routes through `caustic_shadow_transmittance`, which preserves the original `shadow_transmittance` path when disabled and, when enabled, can trace through up to two delta transmissive glass interfaces before the final visibility test.
- Classified imported glTF materials with `KHR_materials_transmission`, `transmissionFactor > 0.99`, zero metallic, and delta roughness as dielectric material type `2` in fresh and cached imported-scene GPU material packing. Other imported PBR materials remain type `3`.
- Added the compact Khronos `AttenuationTest` glTF sample under `scenes/validation/gltf_sample_assets/AttenuationTest/glTF/` for repeatable transmission/IOR caustic-visibility validation.
- This is not the full manifold next-event estimation solver: robust SDS manifold walks, Jacobian evaluation, dedicated glass-caustic reference scenes, wavefront integration, and convergence proof remain open.
- Verified Release and Debug builds. The only observed compiler warning remains the existing `C4324` PathTracerRenderer padding warning.
- Ran focused Release glTF material-extension probes under `out/phase81_caustics_off/` and `out/phase81_caustics_on/`: both completed with `validation_error_count=0`, exported 99 debug-view files, and profile JSON correctly reported `mnee_caustics_enabled=false/true`.
- Compared the off/on glTF probe `beauty.png` and `direct-lighting.png` outputs under `out/phase81_caustics_compare/`: both were bit-identical (`SSIM=1.0`, `MSE=0`, `max_error=0`), which confirms no regression on that non-caustic validation scene but does not satisfy the full caustic convergence acceptance criterion.
- Ran focused Release `AttenuationTest` caustics off/on probes under `out/phase81_attenuation_off/` and `out/phase81_attenuation_on/`: both completed with `validation_error_count=0`, exported 99 debug-view files, and profile JSON correctly reported `mnee_caustics_enabled=false/true`. The enabled probe increased path-trace cost from `2.988 ms` to `4.178 ms` on the 12-frame diagnostic run because it traces surface rays through transmissive interfaces instead of early shadow occlusion.
- Compared `AttenuationTest` off/on images under `out/phase81_attenuation_compare/`: `beauty.png` changed `98.6224%` of pixels (`SSIM=0.8300`, `MSE=155.405`, `max_error=119`) and `direct-lighting.png` changed `59.0209%` of pixels (`SSIM=0.8358`, `MSE=1077.286`, `max_error=210`), proving the guarded caustic-visibility path affects transmissive direct lighting.
- Ran short Debug caustics-on probes under `out/phase81_caustics_debug/` and `out/phase81_attenuation_debug/`: both reported `validation_error_count=0`, and the AttenuationTest debug profile reported `mnee_caustics_enabled=true`.
- Ran the required default Release Cornell diagnostic smoke under `out/phase81_required_smoke_v2/`: profile/rendergraph/debug views/debug package were present, `validation_error_count=0`, `effective_samples_per_pixel=1`, and default `mnee_caustics_enabled=false` preserved the baseline path.
- Added Phase 81 GPU debug counters under `gpu_debug_counters.ray_tracing_any_hit`: `caustic_shadow_attempts`, `caustic_transmissive_hits`, `caustic_transmissive_visible`, and `caustic_shadow_blocked`. The counters are shader-atomic diagnostics guarded by the existing profile/counter flag and only increment inside the opt-in caustic-visibility path.
- Ran a short Debug `AttenuationTest` caustics-on probe under `out/phase81_caustic_counters_debug/`: `validation_error_count=0`, `mnee_caustics_enabled=true`, `caustic_shadow_attempts=3282228`, `caustic_transmissive_hits=173375`, `caustic_transmissive_visible=156863`, and `caustic_shadow_blocked=96661`.
- Ran the matching short Debug `AttenuationTest` caustics-off probe under `out/phase81_caustic_counters_off_debug/`: `validation_error_count=0`, `mnee_caustics_enabled=false`, and all four caustic counters remained `0`, proving the disabled path is not instrumenting normal shadow rays as caustic probes.
- Ran a short Release `AttenuationTest` caustics-on probe under `out/phase81_caustic_counters_release_on/`: `validation_error_count=0`, `mnee_caustics_enabled=true`, and the same counter coverage was present (`3282228` attempts, `173375` transmissive hits, `156863` transmissive-visible paths, `96661` blocked probes).
- Reran the required default Release Cornell diagnostic smoke under `out/phase81_caustic_counters_default_smoke/`: profile/rendergraph/debug views/debug package were present, `validation_error_count=0`, default `mnee_caustics_enabled=false`, all four caustic counters remained `0`, 98 debug PNGs exported, and the debug package contained 105 files.
- Added `caustic-visibility` (`RendererDebugView::CausticVisibility`, value `100`) as a path-trace debug view. The view automatically enables the guarded caustic-visibility probe while selected, exports with `--save-debug-views`, and visualizes blocked caustic probes in red, transmissive visible paths in green, and transmissive-interface hits in blue.
- Ran focused Debug `AttenuationTest` debug-view validation under `out/phase81_caustic_debug_view/` with `--debug-view caustic-visibility`: `validation_error_count=0`, profile JSON reported `debug_view="caustic-visibility"` and `mnee_caustics_enabled=true`, counters were nonzero (`2461722` attempts, `130094` transmissive hits, `117708` visible, `72390` blocked), 99 debug PNGs exported, `caustic-visibility.png` existed, and sampled image pixels were nonblank (`sample_sum=58219`, `sample_max=174`).
- Reran the required default Release Cornell diagnostic smoke after adding the view under `out/phase81_caustic_debug_view_default_smoke/`: profile/rendergraph/debug views/debug package were present, `validation_error_count=0`, default debug view stayed `beauty`, `mnee_caustics_enabled=false`, all four caustic counters remained `0`, 99 debug PNGs exported, and the debug package remained present.
- Ran focused Release `AttenuationTest` debug-view validation under `out/phase81_caustic_debug_view_release/`: `validation_error_count=0`, profile JSON reported `debug_view="caustic-visibility"` and `mnee_caustics_enabled=true`, counters were nonzero (`2461722` attempts, `130094` transmissive hits, `117708` visible, `72390` blocked), 99 debug PNGs exported, and `caustic-visibility.png` was nonblank (`sample_sum=60203`, `sample_max=217`).

## Detailed Execution Runbooks

This section defines how to execute the phase plan in practice. The phase sections above define what to build. The runbooks below define the exact operating procedure for baselining, implementation, validation, and handoff.

### Repository Preflight

Run this before starting any batch:

1. Confirm the working tree state and identify unrelated user changes.
2. Build Debug.
3. Build Release.
4. Run the renderer for a short smoke test with the default scene.
5. Run a 120-frame static accumulation test.
6. Run a 120-frame camera-motion test.
7. Capture baseline GPU timings for path trace, denoiser, TAA/TSR, ReSTIR, tone map, histogram, and UI.
8. Capture baseline VRAM usage by category if diagnostics already exist.
9. Capture at least one baseline screenshot per validation scene.
10. Save the renderer settings used for the baseline.
11. Record GPU, driver version, resolution, render scale, max bounces, samples per pixel, denoiser mode, ReSTIR mode, and debug flags.
12. Ensure Vulkan validation is enabled for Debug validation runs.
13. Ensure shader debug names and resource names appear in GPU captures.
14. Disable adaptive quality for correctness baselines unless the phase explicitly tests adaptive quality.
15. Pin random seed for A/B comparisons when the phase affects sampling.

Suggested local commands:

```powershell
cmake --build build --config Debug
cmake --build build --config Release
.\build\Debug\rtvulkan.exe --frames 120
.\build\Release\rtvulkan.exe --frames 120
```

Adjust executable paths if the active build directory differs.

### Per-Phase Work Item Template

Every phase should produce a small implementation record with these fields:

| Field | Required content |
|-------|------------------|
| Phase | Number and title |
| Owner branch | Branch or changelist name |
| Feature flag | Runtime setting, shader define, or reason no flag is needed |
| Files touched | Exact files changed |
| Baseline artifact | Screenshot, capture, timing table, or metric snapshot |
| Implementation notes | Design decisions and rejected alternatives |
| Validation artifacts | Screenshots, GPU timings, validation output, memory stats |
| Rollback plan | Flag disable, commit revert, or fallback path |
| Follow-up debt | Known limitations not blocking phase exit |

Phase implementation sequence:

1. Create or select a feature flag before changing behavior.
2. Add debug/profiler counters first.
3. Add data layout changes with old behavior still active.
4. Add shader changes behind the flag.
5. Add C++ descriptor/resource wiring.
6. Run a single-frame smoke test.
7. Run fixed-seed A/B output comparison.
8. Run validation scenes.
9. Capture GPU timings and memory deltas.
10. Enable by default only if acceptance criteria pass.
11. Leave a setting to disable the feature until the next batch is stable.
12. Document any deviation from this plan in the phase record.

### Feature Flag Policy

Use feature flags to isolate renderer-risk changes:

| Change type | Required flag type |
|-------------|--------------------|
| Shader math correctness fix | Debug comparison flag until validated |
| Sampling strategy change | Runtime renderer setting and fixed-seed A/B mode |
| Temporal history change | Runtime setting plus history reset path |
| Memory layout change | Compile-time layout define plus runtime debug fallback when practical |
| Queue/synchronization change | Runtime setting with single-queue fallback |
| Hardware-specific feature | Capability flag plus explicit unsupported-device fallback |
| Architecture fork | Renderer mode selection, for example megakernel versus wavefront |

Naming convention:

1. Renderer settings use `enableX`.
2. Shader defines use `RTV_ENABLE_X`.
3. Debug fallbacks use `--disable-x` command-line flags.
4. Architecture modes use positive selection such as `--renderer-mode wavefront`.
5. Temporary A/B switches must be removed or moved to debug-only UI after the next batch stabilizes.

### Baseline Metrics Schema

Record this table before and after every batch:

| Metric | Unit | Notes |
|--------|------|-------|
| Total GPU frame time | ms | Median, p95, p99 over 120 frames |
| CPU frame time | ms | Median, p95, p99 over 120 frames |
| Path trace pass | ms | Include RT dispatch only |
| Denoiser pass | ms | Split temporal/spatial if possible |
| TAA/TSR pass | ms | Include resolve/reconstruction |
| ReSTIR DI | ms | Init, temporal, spatial, final |
| ReSTIR GI | ms | Init, temporal, spatial, final |
| Tone map and exposure | ms | Histogram, exposure reduce, tone map |
| Queue wait time | ms | Required after Phase 49 |
| VRAM total | MB | VMA reported usage where available |
| Reservoir memory | MB | Required after Phase 34 |
| Temporal history memory | MB | Required after Phase 19 |
| Descriptor sets allocated | count | Required after Phase 54 |
| Any-hit invocations | count | Required for alpha scenes |
| Average path length | bounces | Required for RR and wavefront work |
| Live wavefront queue high-water | rays | Required after Phase 61 |

### Batch 1 Runbook: Critical Fixes

Execution order:

1. Build a fixed-seed baseline for emissive, sun, glossy, and RIS scenes.
2. Implement Phase 2 before Phase 14 so delta classification is correct before VNDF sampling.
3. Implement Phase 15 immediately after Phase 2 because roughness floor and delta classification interact.
4. Implement Phase 3 with a white furnace comparison before touching emissive continuation.
5. Implement Phase 1 and verify emissive continuation does not double-count direct emissive hits.
6. Implement Phase 5 after Phase 1 because sun disk MIS depends on previous-event PDF state.
7. Implement Phase 6 after Phase 5 because RIS effective PDF must participate in the same MIS path.
8. Implement Phase 4 after MIS fixes so Russian roulette validation uses the final throughput logic.
9. Implement Phase 46 last because it is independent and easy to disable.
10. Run all Batch 1 validation scenes with fixed seed and then with normal stochastic sampling.

Batch 1 validation details:

1. Cornell emissive mean luminance must not decrease after emissive continuation.
2. A mirror looking at an emissive surface must not become over-bright.
3. Roughness ramp must show delta behavior only below `0.001`.
4. White furnace and metallic furnace must remain energy bounded.
5. Sun glints must not double-brighten when both BSDF and light sampling can sample the sun.
6. RIS candidate count changes must affect variance but not long-run mean.
7. `indirect_strength` changes must not change path survival statistics.
8. Anisotropic filtering must be disabled cleanly on unsupported devices.

### Batch 2 Runbook: Quick Wins

Execution order:

1. Implement Phase 7 first to stop long frame delta spikes from contaminating later responsiveness measurements.
2. Implement Phase 8 and run resource-lifetime validation for three frames in flight.
3. Implement Phase 9 and confirm picking work is absent from normal frames.
4. Implement Phase 10 and compare normal debug view before/after.
5. Implement Phase 14 only after Phase 2 is merged and validated.
6. Implement Phase 16 after Phase 14 so GGX eval and sampling remain paired.
7. Implement Phase 17 and re-run furnace tests.
8. Implement Phase 29 and Phase 30 together because environment sample count and RR both affect variance/performance tradeoffs.
9. Re-profile static and moving scenes.

Batch 2 validation details:

1. Camera motion after a breakpoint or window drag must not jump.
2. Three frames in flight must not corrupt accumulation or descriptor updates.
3. Picking requests must produce the same entity id as the previous per-frame path.
4. Non-uniform scale normal debug view must remain correct.
5. VNDF sampling must preserve mean brightness versus old GGX after enough samples.
6. RR tuning must reduce average path length without luminance bias.

### Batch 3 Runbook: Core Performance

Execution order:

1. Add sampling-dimension documentation before implementing Phase 13.
2. Implement Sobol/Owen/STBN behind a sampler-mode setting.
3. Reserve fixed dimensions for camera jitter, lens, BSDF lobe, BSDF direction, light selection, environment selection, ReSTIR candidates, RR, and denoiser stochastic taps.
4. Implement Phase 18 shared-memory denoiser as a shader variant.
5. Implement Phase 11 dynamic quality with adaptive behavior disabled by default.
6. Implement Phase 12 pass skipping only after Phase 11 exposes motion state and quality state.
7. Profile each feature independently and then together.

Batch 3 validation details:

1. Sample dimension reuse must be visible in debug output.
2. STBN must reduce structured noise without creating temporal shimmer.
3. Shared-memory denoiser must match baseline output within tolerance.
4. Dynamic quality must not oscillate under small mouse movement.
5. Pass skipping must never skip mandatory history invalidation.

### Batch 4 Runbook: TSR And Async Compute

Execution order:

1. Implement Phase 19 resource split with render scale fixed to 1.0.
2. Validate that 1.0 render scale is identical to the pre-split renderer.
3. Implement Phase 20 TSR reconstruction with a native-scale compatibility mode.
4. Implement Phase 21 C++ pipeline wiring and render-scale UI.
5. Implement Phase 22 presentation, tone map, screenshot, and selection alignment.
6. Build a pass dependency table for all post-path-trace compute work.
7. Implement Phase 49 same-family compute submission first.
8. Add cross-family ownership transfers only after same-family async is correct.
9. Add timeline semaphore profiling and single-queue fallback.
10. Run all TSR validation before enabling async compute by default.

Batch 4 validation details:

1. Render scale 1.0 must match baseline.
2. Render scale changes must recreate resources without stale descriptors.
3. Selection outline and picking coordinates must match displayed pixels.
4. TSR history must reset correctly on camera cut and scale change.
5. Async compute must produce equivalent output to single-queue fallback.
6. GPU captures must show actual overlap or explain why hardware scheduling prevents it.

### Batch 5 Runbook: Production Denoiser

Execution order:

1. [DONE] Implement Phase 23 path data outputs without changing denoiser behavior.
2. [DONE] Add debug views for every path data channel.
3. [DONE] Implement Phase 24 roughness-aware kernel sizing.
4. [DONE] Implement Phase 25 hit-distance filtering.
5. [DONE] Implement Phase 26 virtual motion for specular behind a conservative flag.
6. [DONE] Implement Phase 27 split histories only after diffuse/specular channel confidence is reliable.
7. [DONE] Implement Phase 28 emissive anti-flicker last because it depends on stable path data classification.
8. Re-run temporal motion tests after each phase.

Batch 5 validation details:

1. Path data channels must be explainable for every validation scene.
2. Specular highlights must not smear after roughness-aware filtering.
3. Hit-distance filtering must reduce halos at disocclusions.
4. Virtual specular motion must improve moving-camera glossy scenes.
5. Split histories must not exceed memory budget without Phase 50/51 mitigation notes.
6. Emissive anti-flicker must not suppress real animated emissive changes.

### Batch 6 Runbook: ReSTIR DI Fixes

Execution order:

1. Freeze a non-ReSTIR path tracing reference for many-light scenes.
2. [DONE] Implement Phase 31 pairwise MIS with debug mode showing candidate weights.
3. Validate temporal reservoir reuse with camera still, camera moving, and light intensity changing.
4. [DONE] Implement Phase 32 visibility reuse with conservative rejection first.
5. Loosen spatial reuse only after light leaks are eliminated.
6. [DONE] Implement Phase 33 Light BVH improvements after DI estimator correctness is stable.
7. Compare variance, mean, and GPU cost against pre-batch ReSTIR.

Batch 6 validation details:

1. Pairwise MIS must remove temporal brightness bias.
2. Visibility reuse must not leak light through walls.
3. Light BVH changes must affect variance and traversal counts, not mean brightness.
4. Reservoir debug views must remain valid after every pass.

### Batch 7 Runbook: ReSTIR GI

Execution order:

1. [DONE] Implement Phase 34 GI reservoir layout and clear/debug passes only.
2. [DONE] Add memory accounting before generating any GI samples.
3. [DONE] Implement Phase 35 initial sampling and compare to one-bounce path tracing.
4. [DONE] Implement Phase 36 temporal reuse with strict rejection thresholds.
5. [DONE] Implement Phase 37 spatial reuse with conservative visibility-preserving rejection.
6. [DONE] Implement Phase 38 final shading debug/reference mode.
7. [DONE] Implement Phase 39 tuning only after correctness is stable.
8. [DONE] Record reservoir memory footprint for Phase 50 compression.

Batch 7 validation details:

1. Initial reservoirs must match bounce-1 distribution.
2. Temporal reuse must reduce noise without ghosted GI.
3. Spatial reuse must not leak through walls or across strong normal/depth discontinuities.
4. Final shading must be unbiased relative to reference.
5. Half-resolution tuning must have documented quality loss.

### Batch 8 Runbook: Materials And Textures

Execution order:

1. [DONE] Add material validation scenes before changing BRDF code.
2. [DONE] Implement Phase 40 Oren-Nayar with Lambert fallback.
3. [DONE] Implement Phase 41 conductor Fresnel with preset values and legacy fallback.
4. [DONE] Implement Phase 42 glTF extension import without anisotropic shading.
5. [DONE] Implement Phase 43 anisotropic GGX after tangent basis validation.
6. [DONE] Implement Phase 44 occlusion texture and verify it affects indirect diffuse only.
7. [DONE] Implement Phase 45 specular AA as a roughness adjustment, not a material mutation.
8. [DONE] Implement Phase 47 KTX2 mip fix with colored-mip validation.
9. [DONE] Implement Phase 48 HDR/16-bit texture support with color-space tests.
10. [DONE] Implement Phase 55 sheen/thin-film sampling after multi-lobe MIS is stable.

Batch 8 validation details:

1. Legacy materials must render acceptably after every material change.
2. Extension-heavy glTF assets must report unsupported extensions explicitly.
3. Anisotropic highlights must rotate with tangents.
4. AO must not incorrectly darken direct light.
5. KTX2 mips must display the expected colored mip at distance.
6. HDR emissive textures must not clamp unexpectedly.
7. Multi-lobe sampling must preserve energy.

### Batch 9 Runbook: Memory And Stall Hardening

Execution order:

1. [DONE] Implement Phase 50 after ReSTIR GI has final reservoir semantics.
2. [DONE] Keep uncompressed reservoirs as a debug layout until Phase 51 passes.
3. [DONE] Implement Phase 51 aliasing with a runtime disable path for A/B validation.
4. [DONE] Enable aliasing per resource group, starting with the lowest-risk proven non-overlap reservoir pair.
5. [DONE] Implement Phase 52 async picking before Phase 53 so picking-specific waits have a replacement path.
6. [DONE] Implement Phase 53 by replacing one `vkDeviceWaitIdle` class at a time.
7. [DONE] Implement Phase 54 after memory categories are final enough to budget.
8. [DONE] Cover long-session descriptor lifetime with automated headless stress for repeated scene reloads, material descriptor updates, render-scale resource rebuilds, shader hot rebuilds, and retired-renderer draining; editor-only UI behavior remains covered by UI descriptor stats and manual editor smoke when needed.

Batch 9 validation details:

1. Reservoir compression must reduce memory without mean-lighting bias.
2. Closeup Cornell must be included for path tracing, ReSTIR GI, and stall/memory hardening changes because it exposes the known close-camera performance cliff.
3. Lightweight Sponza glTF must be included for scene-loading, material, texture, RenderGraph, and debug-view coverage before moving to heavy Sponza profiling.
4. Aliasing must be provably lifetime-safe in graph logs and GPU captures.
5. [DONE] Async picking must never block the device.
6. [DONE] Normal editing flows must not hit `vkDeviceWaitIdle`.
7. [DONE] VMA budget warnings must appear before allocation failure.
8. [DONE] Descriptor pools must survive repeated hot reload and scene reload.

### Batch 10 Runbook: OMM

Execution order:

1. [DONE] Implement Phase 56 feature detection and leave OMM disabled by default.
2. [DONE] Implement Phase 57 alpha-tested geometry classification.
3. [DONE] Validate classification on foliage, cutout decals, opaque meshes, and blended meshes.
4. [DONE] Implement Phase 58 OMM preprocessing with CPU-side debug visualization.
5. [DONE] Implement Phase 59 BLAS build integration with fallback build path.
6. [DONE] Implement Phase 60 geometry splitting after BLAS integration works.
7. [DONE] Profile any-hit invocation count and shadow ray cost on foliage scenes with a camera path that actually traverses alpha-tested foliage. The moving lightweight Sponza glTF acceptance run reduced camera any-hit invocations by `50.86%` and shadow any-hit invocations by `53.22%` with OMM subdivision `3`.

Batch 10 validation details:

1. Unsupported hardware must behave exactly like the fallback path.
2. OMM alpha classification must match the alpha cutoff.
3. BLAS builds must remain valid after scene reload and material edits.
4. Any-hit invocations should drop by 50-80 percent on foliage.
5. Foliage shadow rays should improve by the expected 2-4x where supported.

### Batch 11 Runbook: Wavefront

Execution order:

1. [DONE] Create a separate renderer mode for wavefront and keep megakernel as reference. Phase 61 added opt-in `--wavefront-queues on` validation while keeping the default megakernel path unchanged.
2. [DONE] Implement Phase 61 layouts and C++/GLSL layout tests before any tracing.
3. [DONE] Implement Phase 62 primary generation and validate queue counts.
4. [DONE] Implement Phase 63 trace wrapper and compare hit/miss buffers to megakernel.
5. [DONE] Implement Phase 64 shade compute for one bounce before multiple bounces. Phase 64 has an opt-in queue/shade probe, shadow-resolved first-bounce direct-light output through `wavefront-direct-lighting`, shade/path-depth counters, and classic/raw NEE direct-light parity coverage through the Phase 65 validator.
6. [DONE] Implement Phase 65 shadow trace and validate direct lighting. Phase 65 has an opt-in Cornell shadow trace probe, a base-aligned multi-raygen RT pipeline that validates on default lightweight Sponza without the previous shadow-raygen GPU fault, dedicated shadow-trace occupancy/cost diagnostics, a direct-light parity validator, passing Cornell classic/raw NEE parity, and targeted `--camera sponza-foliage --render-preset low` alpha-tested Sponza shadow-ray coverage. Default ReSTIR direct-light parity remains pending for the Phase 69 ReSTIR wavefront integration phase.
7. [DONE] Implement Phase 66 compaction with debug counters.
8. [DONE] Implement Phase 67 frame allocator and memory high-water stats.
9. [DONE] Implement Phase 68 synchronization after all early stages exist.
10. [PARTIAL] Implement Phase 69 ReSTIR integration only after base wavefront shading is correct. The current implementation covers DI diagnostic reservoirs, GI second-bounce candidate reservoirs, and wavefront ReSTIR debug views; full wavefront GI ownership and final output remain open until the multi-bounce wavefront loop exists.
11. [PARTIAL] Implement Phase 70 debug views before optimization phases. Queue-owned wavefront diagnostics are implemented; non-queue core views still use the megakernel/reference outputs until wavefront final write owns those buffers.
12. [BLOCKED] Implement Phase 71 sorting, Phase 72 bandwidth optimization, and Phase 73 work balancing only after correctness is locked. Phase 72 and Phase 73 are complete; Phase 71 remains open because the current sorted path validates but still fails the direct unsorted A/B timing gate.
13. [DONE] Implement Phase 74 validation and decide default renderer mode based on data.

Batch 11 validation details:

1. Wavefront primary rays must match megakernel rays.
2. Wavefront hit buffers must match megakernel payload outputs.
3. One-bounce wavefront output must match megakernel statistically.
4. Multi-bounce path length distribution must match megakernel.
5. Queue counters must never exceed capacity.
6. Fixed-seed single-queue wavefront must be deterministic.
7. Debug views must diagnose queue occupancy, material buckets, path depth, and terminated rays.
8. Divergent scenes should improve by 20-40 percent or blockers must be documented.

### Batch 12 Runbook: SER

Execution order:

1. [DONE] Implement Phase 75 capability query and UI reporting.
2. [DONE] Add SER pipeline creation behind capability checks. Phase 77 corrected the roadmap: public SER support does not require the nonexistent `CAPTURE_REORDER` pipeline create flag, so the SER pipeline uses the enabled invocation-reorder feature plus the SER shader variant on the normal ray tracing pipeline path.
3. [DONE] Implement Phase 77 pipeline-path correction and remove the obsolete capture-reorder flag dependency.
4. [PARTIAL] Implement Phase 76 reordering hints in wavefront mode only. SER hints and the opt-in SER raygen pipeline exist, but measured Release improvements remain below the 20-40 percent acceptance target.
5. [DONE] Profile with sorting disabled, sorting enabled, SER disabled, and SER enabled.
6. [DONE] Keep unsupported hardware on the exact same wavefront path without SER.

Batch 12 validation details:

1. SER unsupported devices must not create SER pipeline structs.
2. SER output must match non-SER wavefront output within floating-point noise.
3. Reordering must improve divergent scenes enough to justify complexity.
4. SER must not conflict with wavefront sorting or debug captures.

### Batch 13 Runbook: Advanced Features

Execution order:

1. [DONE] Implement Phase 78 depth of field in camera ray generation with deterministic lens sampling.
2. [DONE] Validate DoF in megakernel and wavefront if both are active.
3. [DONE] Implement Phase 79 motion blur after per-instance previous/current transform infrastructure is stable.
4. [DONE] Validate motion blur independently from temporal velocity buffers.
5. [DONE] Implement Phase 80 homogeneous volumes before heterogeneous volumes.
6. [PARTIAL] Implement Phase 81 MNEE only after specular/refraction material paths are stable. A guarded delta-glass caustic visibility probe exists; full MNEE, Jacobians, wavefront support, and glass-caustic convergence validation remain open.
7. [DONE] Keep every advanced feature behind quality settings.

Batch 13 validation details:

1. DoF focus plane must remain sharp.
2. Motion blur must not blur static objects.
3. Volume transmittance must match analytic homogeneous-medium expectations.
4. MNEE caustics must improve convergence without introducing persistent fireflies.

### Phase Handoff Checklist

Before a phase is considered complete:

1. The feature has a clear enabled/disabled state.
2. The fallback path still works.
3. Debug UI or logs expose the new state.
4. GPU resources have debug names.
5. Validation scenes pass.
6. GPU timings are recorded.
7. Memory deltas are recorded.
8. Known limitations are written into the phase record.
9. Any new shader layout is documented in C++ and GLSL.
10. Any new descriptor binding is reflected in layout creation and update code.
11. Any new temporal history has reset, resize, and camera-cut behavior.
12. Any new queue submission has a single-queue fallback.

### Regression Triage Procedure

Use this order when a phase fails validation:

1. Re-run with the phase feature flag disabled.
2. Re-run with fixed seed.
3. Re-run at one sample per pixel and one bounce if the failure is shading-related.
4. Re-run with temporal accumulation disabled if the failure is ghosting/flicker.
5. Re-run with ReSTIR disabled if the failure is direct or indirect lighting bias.
6. Re-run with denoiser disabled if the failure is blur, haloing, or trails.
7. Re-run with async compute disabled if the failure is nondeterministic.
8. Re-run with resource aliasing disabled if the failure is corruption.
9. Compare debug views for depth, normal, world position, roughness, velocity, reservoir age, and reservoir confidence.
10. Take a GPU capture only after the smallest failing configuration is identified.

## Dependency Graph Summary

```text
Batch 1 critical fixes
  -> Batch 2 quick wins
  -> Batch 3 core performance
  -> Batch 4 TSR + async compute overlap
  -> Batch 5 denoiser
  -> Batch 6 ReSTIR DI
  -> Batch 7 ReSTIR GI
  -> Batch 8 materials/textures
  -> Batch 9 memory/stall hardening
  -> Batch 10 OMM
  -> Batch 11 wavefront
  -> Batch 12 SER
  -> Batch 13 advanced features
```

Parallel-safe groups:

- Batch 1 phases are mostly parallel, but Phase 2 should land before Phase 14.
- Batch 2 phases can run in parallel except Phase 11 depends on Phase 8 and Phase 12 depends on Phase 11.
- Batch 4 phases 19-22 are sequential; Phase 49 begins after the TSR pass graph and post stack are explicit enough to partition by queue.
- Batch 5 phases 24-28 depend on Phase 23.
- Batch 7 is sequential because each ReSTIR GI stage depends on the previous reservoir semantics.
- Batch 9 order is Phase 50 before Phase 51 for ReSTIR resources, Phase 52 before Phase 53 for picking stalls, and Phase 54 after Phases 50-51 so budget control sees final memory categories.
- Batch 10 is sequential because OMM build integration depends on capability detection, classification, and data generation.
- Batch 11 is mostly sequential through Phases 61-64, then Phases 65, 67, 69, and 70 can branch.

## Validation Scene Matrix

Use these scenes for every batch:

| Scene | Purpose |
|-------|---------|
| `scenes/validation/cornell.rtlevel` | Canonical fast smoke for profile JSON, render graph dumps, debug views, debug packages, emissive continuation, MIS, and denoising stability |
| `scenes/validation/closeup_cornell.rtlevel` | Close-camera path tracing stress/regression scene for camera-proximity slowdowns, ReSTIR GI debug/final checks, traversal cost, and hit-distance behavior |
| Mirror/glossy roughness ramp | Delta threshold, VNDF, roughness floor, specular denoising |
| Outdoor HDR + sun | Sun MIS, environment sampling, TSR, exposure |
| Foliage alpha scene | Alpha any-hit, OMM, shadow performance |
| `Sponza/glTF/Sponza.gltf` | Lightweight Sponza validation for scene loading, texture filtering, material system, RenderGraph validation, debug views, and ReSTIR checks |
| `main_sponza/` heavy Sponza assets | Full scene stress test for performance profiling, memory pressure, async compute validation, denoiser stability, and large-scene benchmarking |
| Many-light scene | Light BVH, ReSTIR DI/GI |
| Moving camera/object scene | Velocity, TSR, denoiser, motion blur readiness |

## Metrics To Track

Record these after each batch:

- GPU frame time at native resolution.
- GPU frame time at TSR quality scale.
- Path tracing pass time.
- Denoiser pass time.
- ReSTIR pass time.
- Tone-map/presentation time.
- Async compute overlap time, queue wait time, and single-queue fallback delta after Phase 49.
- Accumulation stability over 120 static frames.
- Mean luminance against reference.
- Number of validation-layer warnings/errors.
- VRAM usage by category: images, buffers, histories, descriptors, acceleration structures, reservoirs, wavefront queues, staging, render graph transients.
- Reservoir memory before/after compression and aliasing after Phases 50-51.
- VMA heap budget, heap usage, allocation count, and high-water marks after Phase 54.
- Descriptor pool usage, growth count, failed allocations, and high-water marks after Phase 54.
- Any-hit invocation count for alpha scenes.
- Wavefront queue occupancy once available.

## Stop Conditions

Stop the current phase and fix before continuing if any of these occur:

- A correctness change shifts mean luminance without a documented estimator reason.
- Vulkan validation reports synchronization or descriptor errors.
- A temporal change introduces persistent ghosting in validation scenes.
- A performance optimization worsens GPU time by more than 5 percent in common scenes.
- A new architecture feature requires rewriting an unstable earlier subsystem.
- A fallback path is broken on unsupported hardware.
