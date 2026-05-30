# Project Overview

This document summarizes the current structure and architecture of the native Vulkan renderer in `native/vulkan`.

The project is a C++20 / Vulkan 1.3 path-tracing renderer and editor. It builds a native Windows executable named `rtvulkan` and currently supports a Vulkan KHR hardware ray tracing path, glTF/GLB scene loading, HDR environment maps, ReSTIR DI/GI reuse, progressive accumulation, temporal/spatial denoising, TAA, histogram auto exposure, compute tone mapping, selection outlines, ImGui editor panels, GPU timing, and renderer debug views.

The renderer is operational, but it is still in stabilization. Current development is focused on real-time path tracing stability/performance, ReSTIR DI/GI integration, denoiser/TAA motion quality, the hardware ray tracing backend, and editor-scene synchronization.

## Current Real-Time Defaults

- `Balanced` is the default interactive/game preset.
- Real-time presets use an effective `1 SPP` path-tracing budget by default through the SPP limiter. Higher requested SPP is available through the editor or CLI when the limiter is disabled.
- ReSTIR DI and ReSTIR GI are enabled by default for beauty rendering. Balanced frames include GI spatial reuse and GI final contribution passes unless settings disable them.
- Stability while moving comes from ReSTIR reuse, temporal moment tracking, denoising, and TAA rather than from raising per-frame SPP.
- Profile and budget diagnostics include frame p95/p99, per-pass p95/p99, SPP settings, adaptive-quality state, memory, and motion-stability metrics.

## Repository Shape

Important top-level files and directories:

- `CMakeLists.txt`: CMake build definition for the `rtvulkan` executable.
- `README.md`: user-facing build/run instructions and current feature status.
- `docs/`: architecture, migration, debug/profiling, and implementation planning notes.
- `include/rtv/`: public/internal C++ headers for renderer modules.
- `src/rtv/`: C++ implementation files.
- `src/main.cpp`: executable entry point and CLI parsing.
- `shaders/`: GLSL compute post-process, fullscreen, and hardware ray tracing shaders.
- `build/`: local CMake build directory.
- `Sponza/`: local sample scene assets.
- `citrus_orchard_road_puresky_4k.hdr`: local HDR environment asset.
- `rtv_editor.ini`: ImGui/editor layout state.
- `run_rtvulkan.bat`, `run_rtvulkan_debug.bat`: local launcher scripts.

At the time of inspection, the project contained roughly:

- 91 C++ source files.
- 99 header files.
- 40 shader files, including ray tracing, denoising, tone mapping, TAA, atmosphere, sky, fog, and ReSTIR-related compute shaders.
- Validation `.rtlevel` scenes and debug/regression automation assets.
- Architecture, implementation-plan, and AI debug/profiling documents.

## Build System

`CMakeLists.txt` defines a single executable:

```text
rtvulkan
```

The project requires:

- Windows.
- Visual Studio 2022 C++ tools.
- CMake.
- Vulkan SDK.
- vcpkg-provided dependencies.

Main dependencies:

- Vulkan SDK.
- Volk.
- GLFW.
- GLM.
- Vulkan Memory Allocator.
- nlohmann/json.
- stb.
- tinygltf.
- SPIRV-Reflect.
- KTX.
- ImGuizmo.
- ImGui docking sources.
- Optional RenderDoc SDK headers for CLI-triggered captures.

The CMake file currently expects `VULKAN_SDK`, uses vcpkg packages for most third-party dependencies, discovers ImGui source from a cache variable or local vcpkg buildtree, and enables RenderDoc support only when `RENDERDOC_SDK_DIR` contains `renderdoc_app.h`.

## Program Entry

`src/main.cpp` is intentionally small. It parses CLI options and starts the application.

Supported interactive options include:

- `--frames <count>`: run for a fixed number of frames, useful for smoke tests.
- `--debug-view <name>`: start in a specific renderer debug view.
- `--gltf <path>`: load a glTF/GLB scene.
- `--hdr <path>`: load an HDR environment map.
- `--scene <path>` / `--rtlevel <path>`: load a serialized `.rtlevel` scene.
- `--denoiser <on/off>`: override denoiser state.
- `--restir <mode>`: override ReSTIR mode.
- `--restir-di <on/off>` and `--restir-gi <on/off>`: override default ReSTIR DI/GI usage.
- `--spp <N>` / `--samples-per-pixel <N>`: request 1-8 path samples per pixel per frame.
- `--spp-limit <on/off>` / `--limit-spp <on/off>`: cap effective real-time SPP to 1 when enabled.

Supported diagnostic and automation options include:

- `--headless`: run without window, swapchain presentation, or ImGui; requires a scene.
- `--warmup-frames <count>`: render warmup frames that are excluded from profile averages.
- `--fixed-seed <seed>`: use deterministic sampling/jitter seed input where practical.
- `--profile` and `--profile-json <path>`: collect and export JSON timing, memory, settings, and validation data.
- `--dump-rendergraph <path>` and `--dump-rendergraph-dot <path>`: export render graph structure and barriers.
- `--save-debug-views <dir>`: export renderer debug views as PNGs.
- `--make-debug-package <dir>`: assemble profile, logs, render graph, scene copy, settings, validation log, and debug views.
- `--capture-renderdoc <path>` and `--capture-frame <n>`: trigger optional RenderDoc capture when RenderDoc is available.
- `--run-validation-suite` and `--validation-output <dir>`: run validation scenes and write a summary package.
- Offline comparison/baseline commands such as `--compare-profile`, `--compare-image`, `--update-baseline`, and `--check-baseline`.

After parsing, `main()` constructs `rtv::Application` and calls `run()`.

## Application Layer

`Application` is the top-level owner of the runtime. It is declared in `include/rtv/Application.h` and implemented in `src/rtv/Application.cpp`.

It owns:

- The GLFW window.
- Optional headless execution state.
- Runtime input handling.
- Camera controller.
- Scene loading and async glTF reloads.
- Asset manager.
- Scene document.
- Scene event bus.
- Notification manager.
- Undo stack.
- Vulkan context.
- Resource allocator.
- Upload context and buffer uploader.
- Swapchain in windowed/editor mode.
- Command system.
- ImGui overlay in windowed/editor mode.
- Path tracer renderer.
- Frame timing arrays used by headless diagnostics.

Startup flow:

1. Create the GLFW window.
2. Initialize Vulkan through `VulkanContext`.
3. Create VMA resource allocation and upload helpers.
4. Create the swapchain and command system.
5. Load a glTF scene if provided; otherwise initialize the fallback scene.
6. Convert the editor scene document into a GPU scene asset.
7. Create `PathTracerRenderer`.
8. Apply the active scene camera.
9. Create the ImGui overlay.
10. Enter the main frame loop.

Headless startup follows the same renderer/device path but skips GLFW window creation, swapchain presentation, and ImGui setup. It renders into offscreen presentation images and is the preferred path for deterministic diagnostics, validation, and CI-style runs.

The main loop:

1. Polls GLFW events.
2. Computes frame delta time.
3. Starts an ImGui frame.
4. Processes runtime keyboard/mouse controls.
5. Builds editor panels.
6. Applies safe pre-frame editor requests.
7. Records and submits the frame through `CommandSystem`.
8. Applies post-frame requests that may require resource rebuilds.
9. Polls async scene loading.
10. Updates the window title.

Headless execution calls `runHeadless()` / `renderFrames()` instead of the editor loop. Warmup frames and profiled frames are tracked separately so timing reports can exclude startup/transient accumulation cost.

## Vulkan Core

### `VulkanContext`

`VulkanContext` owns the Vulkan instance and device-level setup.

Responsibilities:

- Instance creation.
- Validation layer/debug messenger setup.
- GLFW surface creation in windowed mode.
- Surface/swapchain-free initialization in headless mode.
- Physical-device selection.
- Required extension and feature checks.
- Logical device creation.
- Graphics/present queue retrieval.
- Bindless capability discovery.
- Hardware ray tracing capability discovery.
- VMA-compatible device state.

Hardware RT capability tracking includes:

- `VK_KHR_acceleration_structure`.
- `VK_KHR_ray_tracing_pipeline`.
- `VK_KHR_deferred_host_operations`.
- `VK_KHR_buffer_device_address`.
- `VK_KHR_spirv_1_4`.
- `VK_KHR_shader_float_controls`.

### `Swapchain`

`Swapchain` owns:

- `VkSwapchainKHR`.
- Swapchain images.
- Image views.
- Swapchain format.
- Swapchain extent.
- Surface support queries.
- Surface format selection.
- Present mode selection.
- Resize/recreation.

The swapchain is not created in headless mode. Offscreen images are used instead so diagnostic runs do not depend on a desktop surface or presentation path.

### `CommandSystem`

`CommandSystem` owns frame submission.

It uses two frames in flight and manages:

- Command pools.
- Command buffers.
- Per-frame fences.
- Image-available semaphores.
- Render-finished semaphores.
- Swapchain recreation.
- Frame recording.
- Queue submission.
- Presentation.

It delegates actual renderer work to `PathTracerRenderer` and UI rendering to `UiOverlay`.

The command system is the boundary between the application frame loop and renderer pass recording. It owns synchronization and presentation concerns; renderer modules record rendering work into the provided command buffer without owning acquire/present state.

## Resource Layer

The project wraps Vulkan resources in small RAII-style C++ types.

### `ResourceAllocator`

`ResourceAllocator` wraps Vulkan Memory Allocator and stores device/physical-device state. It also exposes debug naming and whether buffer device address is supported.

### `Buffer`

`Buffer` wraps:

- `VkBuffer`.
- `VmaAllocation`.
- Optional persistent mapping.

It supports:

- GPU-only memory.
- Upload memory.
- Readback memory.
- Descriptor metadata.
- Writes/flushes/invalidates.
- Buffer resizing.
- Device address queries for hardware RT.

### `Image`

`Image` wraps:

- `VkImage`.
- `VmaAllocation`.
- Image view.
- Current tracked layout.

It supports:

- Sampled image descriptors.
- Storage image descriptors.
- Full subresource ranges.
- Resizing.
- Mipmap generation.

### Upload Helpers

`UploadContext`, `BufferUploader`, and `BatchUploader` provide staging upload paths for buffers, textures, and scene data. Upload work is fence-backed and designed to remain compatible with a future transfer-queue path.

### Barriers

`ImageBarrier` provides Synchronization2 helpers around `vkCmdPipelineBarrier2`. The renderer avoids legacy `vkCmdPipelineBarrier` and keeps image/buffer transitions explicit.

## Descriptor And Pipeline Layer

The renderer has custom helpers for descriptor and pipeline management.

Key modules:

- `DescriptorLayoutCache`: caches reusable descriptor set layouts.
- `DescriptorAllocator`: owns resettable descriptor pools.
- `DescriptorSet`: lightweight descriptor set wrapper.
- `DescriptorWriter`: batches writes for buffers, images, image arrays, samplers, and acceleration structures.
- `BindlessResources`: centralizes bindless-style resource table work that is being phased in.
- `ShaderCompiler`: invokes `glslangValidator` and recompiles GLSL when needed.
- `ShaderReflection`: uses SPIRV-Reflect to discover descriptor bindings and push constants.
- `ShaderModule`: owns compiled SPIR-V modules.
- `PipelineCache`: owns Vulkan pipeline cache state.
- `ComputePipeline`: owns compute pipeline state.
- `GraphicsPipeline`: owns dynamic-rendering fullscreen graphics pipeline state.
- `RayTracingPipeline`: owns Vulkan ray tracing pipeline and shader binding tables.

The descriptor and pipeline architecture is summarized here because the current `docs/` directory is focused on renderer planning and AI/debug workflows.

## Render Graph Layer

`RenderGraph`, `RenderGraphPass`, `RenderGraphResource`, and `RenderGraphDump` describe and serialize renderer pass structure. The current renderer still records many passes directly in `PathTracerRenderer`, but it also builds a graph plan for validation and diagnostics.

The render graph tracks:

- Pass names and domains.
- Texture and buffer resources.
- Read/write access for pass inputs and outputs.
- Compiled pass order.
- Synchronization2-style resource barriers.
- Transient resource lifetimes and alias groups.
- Optional async-compute metadata and timeline semaphore state.
- JSON and Graphviz DOT diagnostic dumps.

`PathTracerRenderer::recordRenderGraphPlan()` emits the frame plan and exports it when `--dump-rendergraph` or `--dump-rendergraph-dot` is enabled. This makes pass order and resource transitions machine-readable even while the renderer is still moving toward fuller graph-driven execution.

The intended architecture is:

```text
Renderer pass intent
    -> RenderGraph pass/resource declarations
    -> compile pass order and barriers
    -> execute/record Vulkan commands
    -> dump JSON/DOT/lifetime diagnostics
```

Render graph output is part of the AI debug/profiling workflow and should stay stable enough for automated comparison.

## Scene And Asset Model

The CPU-side asset model is handle-based and lives mainly in:

- `AssetManager`.
- `TextureAsset.h`.
- `MeshAsset.h`.
- `GltfLoader`.
- `SceneDocument`.
- `SceneRegistry`.
- `SceneComponents`.

### `AssetManager`

`AssetManager` owns vectors of:

- `TextureAsset`.
- `MaterialAsset`.
- `MeshAsset`.

Assets are referenced by lightweight handles:

- `TextureAssetHandle`.
- `MaterialAssetHandle`.
- `MeshAssetHandle`.

### Asset Types

`TextureAsset` stores texture metadata, sampler description, source path, residency state, and RGBA8 pixel data.

`MaterialAsset` stores:

- Base color factor.
- Emissive factor.
- Metallic factor.
- Roughness factor.
- Alpha cutoff.
- Alpha mode.
- Double-sided flag.
- Base color texture handle.
- Normal texture handle.
- Metallic-roughness texture handle.
- Emissive texture handle.

Imported metallic-roughness materials are uploaded as PBR/GGX materials even when `metallic == 0`, so dielectric roughness now affects stone, plastic, painted, and other non-metal surfaces. The shaders use a diffuse/specular mixture PDF for those materials instead of sampling only the GGX lobe.

`MeshAsset` stores:

- Vertices.
- Indices.
- Mesh primitives.
- Per-primitive material handles.

`SceneAsset` stores:

- Source path.
- Texture handles.
- Material handles.
- Mesh handles.
- Scene nodes.
- Scene lights.
- Root nodes.

### Editor Scene

`SceneDocument` is the editable scene representation. It owns:

- `SceneRegistry`.
- Environment settings.
- Render settings.
- Active camera.
- Source glTF/HDR paths.
- Dirty/update state.

`SceneRegistry` stores entities using generational `EntityId`s.

Entity components include:

- `Transform`.
- `MeshRenderer`.
- `Light`.
- `Camera`.

Scene changes are categorized by `SceneUpdateKind`:

- `None`.
- `MaterialOnly`.
- `TransformOnly`.
- `LightOnly`.
- `EnvironmentOnly`.
- `CameraOnly`.
- `VisibilityOnly`.
- `TopologyChanged`.
- `RendererSettingsOnly`.

### Scene Conversion

`SceneToGpuSceneBuilder` converts a `SceneDocument` into renderer-facing data. It returns:

- A generated `SceneAsset`.
- Instance entity mapping.
- Renderer settings.
- Accumulation reset reason.
- Whether a renderer rebuild is required.

This keeps editor edits separate from the lower-level GPU scene representation.

Current limitation: this conversion path is still evolving. Mesh entities, lights, render settings, visibility flags, environment settings, and active camera state reach the renderer, but topology-changing editor operations still require careful rebuild/refit routing.

### Scene Update Routing

`SceneUpdateRouter` maps `SceneUpdateKind` values to renderer-facing GPU actions. This keeps scene editing from blindly forcing full renderer rebuilds.

Update routes include:

- Material buffer updates.
- Transform/TLAS update or rebuild paths.
- Light buffer updates.
- Environment updates.
- Camera updates.
- Visibility updates.
- Topology rebuilds.
- Renderer settings application.

Each route also records the accumulation reset reason, whether a GPU scene rebuild is required, whether the renderer must be rebuilt, and whether accumulation should reset. Route counters are surfaced through validation logs and diagnostic packages, which helps distinguish incremental editor updates from costly topology changes.

## GPU Scene Representation

`GpuScene` is the main bridge between CPU scene data and shader-visible GPU buffers.

It owns buffers for:

- Vertices.
- Indices.
- BVH nodes.
- Triangle records.
- Materials.
- Analytic spheres.
- Mesh records.
- Primitive records.
- Instance records.
- Hardware RT triangle material IDs.
- Light records.
- Local mesh vertices.
- Local mesh indices.
- Instance bounds.
- Local BVH nodes.
- Local triangle data.
- TLAS nodes.
- TLAS instance indices.
- Environment row CDF.
- Environment column CDF.
- Mesh parameters uniform.
- Environment parameters uniform.

It also owns:

- Environment image.
- Environment sampler.
- Material texture table.
- Material texture samplers.
- Hardware ray tracing mesh build inputs.
- Hardware ray tracing instance build inputs.

This scene data feeds the hardware RT renderer and the post-processing passes.

Lighting note: GPU light records are built from emissive mesh/sphere geometry and authored scene lights. Directional, point, and area lights are merged into the same light-record selection table as emissive geometry; sun and environment lighting remain separate shader paths.

## Path Tracer Renderer

`PathTracerRenderer` is the main rendering engine.

It owns:

- The active `GpuScene`.
- Render resolution state.
- Display resolution state.
- Accumulation state.
- Temporal frame state.
- Adaptive quality state.
- Camera uniforms.
- Renderer settings.
- Debug parameters.
- Raw render image.
- Denoised image.
- Diffuse/specular resolved and history images.
- Presentation image.
- History image.
- TAA history and parameter state.
- Accumulation buffer.
- Variance buffer.
- Depth/normal buffer.
- World position buffers.
- Velocity buffer.
- Entity ID buffer for picking/selection.
- Path data buffer for lighting/MIS/debug outputs.
- ReSTIR DI and GI reservoir buffers.
- Uniform buffers.
- Descriptor layout cache.
- Pipeline cache.
- Shader modules.
- Denoiser compute pipeline.
- ReSTIR spatial and ReSTIR GI compute pipelines.
- Height fog compute pipeline.
- Atmosphere LUT system.
- Fullscreen graphics pipeline.
- Hardware ray tracing pipeline.
- Hardware ray tracing scene.
- Auto-exposure histogram/reduce pipelines.
- Tone-map compute pipeline.
- TAA compute pipeline.
- Selection-outline compute pipeline.
- Per-frame descriptor arenas.
- GPU profilers.
- Renderer validation log.

Main frame flow:

1. Begin the frame and update camera/settings uniforms.
2. Transition the raw output image for shader writes.
3. Run path tracing through hardware RT.
4. Run ReSTIR DI/GI spatial reuse/finalization passes when enabled by the active preset/settings.
5. Optionally integrate height fog and atmosphere contributions.
6. Barrier path tracing outputs for denoiser reads.
7. Run temporal/spatial denoising if enabled.
8. Run TAA if enabled and update temporal history.
9. Copy history resources for the next frame.
10. Optionally build/reduce the luminance histogram for auto exposure.
11. Tone map into the presentation image.
12. Optionally run the selection outline compute pass.
13. Render fullscreen presentation.
14. Render the editor overlay in windowed/editor mode.

Accumulation resets when relevant state changes:

- Startup.
- Resize.
- Camera movement.
- Manual reset.
- Render settings change.
- Lighting change.
- Environment change.
- Denoiser setting change.
- TAA setting change.
- Debug view change.
- Scene change.
- Material change.
- Shader reload.

`PathTracerRenderer` also owns the diagnostic surface for the frame. It records pass events, barrier events, accumulation invalidations, scene update routes, hardware RT statistics, memory estimates, pipeline statistics, and render graph dumps. Headless diagnostics consume these APIs directly instead of scraping UI state.

## Renderer Backend

The renderer requires Vulkan KHR hardware ray tracing and fails clearly during startup when the selected device lacks the required extensions or features.

The hardware RT backend uses:

- `RayTracingScene`.
- `AccelerationStructure`.
- `RayTracingPipeline`.
- `shaders/pathtrace.rgen`.
- `shaders/pathtrace.rchit`.
- `shaders/pathtrace.rahit`.
- `shaders/pathtrace.rmiss`.
- `shaders/pathtrace_shadow.rahit`.
- `shaders/pathtrace_shadow.rmiss`.
- `shaders/rt_common.glsl`.

`RayTracingScene` builds:

- One or more BLAS objects from mesh geometry.
- A TLAS from scene instances.
- Hardware RT instance records that mirror renderer visibility and transform state.
- Acceleration-structure memory and count statistics for diagnostics.

`RayTracingPipeline` builds:

- Ray generation group.
- Miss groups.
- Hit groups.
- Shader binding tables.

The ray generation shader owns the multi-bounce path loop. Hit shaders return compact hit information such as IDs, UVs, normals, and tangent basis. Material decoding and texture evaluation happen after hits are accepted.

The hardware RT descriptor set includes the TLAS and an RT triangle-material-ID buffer in addition to the shared scene/material/environment buffers. New material, lighting, traversal, scene-update, and performance work should be implemented here first.

Traversal policy is deliberately conservative. Opaque, alpha-tested, shadow-casting, visible-to-camera, and double-sided material rules are represented in scene/instance metadata and enforced by the hardware RT shaders or geometry flags. Meshes known to be alpha-free and double-sided can use `VK_GEOMETRY_OPAQUE_BIT_KHR`; mixed-material meshes still need finer splitting before every triangle can take the fastest path.

## Shaders

Shader files:

- `demo_compute.comp`: simple compute demo.
- `denoiser.comp`: temporal/spatial denoiser.
- `taa.comp`: temporal anti-aliasing resolve and configurable sharpening.
- `luminance_histogram.comp`: auto-exposure histogram builder.
- `exposure_reduce.comp`: auto-exposure percentile reducer/adaptation pass.
- `tone_map.comp`: HDR exposure, tone mapping, color grading, and output encoding.
- `selection_outline.comp`: selected-instance outline overlay.
- `restir_*.comp` and `restir_common.glsl`: ReSTIR DI/GI reservoir initialization, temporal/spatial reuse, and finalization helpers.
- `transmittance_lut.comp`, `multi_scatter_lut.comp`, `sky_view_lut.comp`, `aerial_perspective_lut.comp`, `fog_integrate.comp`, and atmosphere shared headers: atmosphere and fog lookup generation/integration.
- `sky_cdf.comp`, `sky_debug_views.comp`, and `sky_reproject.comp`: sky sampling/debug/temporal support.
- `temporal_common.glsl`, `environment_sampling.glsl`, `atmosphere_lighting.glsl`, `blue_noise.glsl`, and `wavefront_common.glsl`: shared shader utilities.
- `fullscreen.vert`: fullscreen triangle vertex shader.
- `fullscreen.frag`: fullscreen presentation copy shader.
- `pathtrace.rgen`: ray generation shader for hardware RT.
- `pathtrace.rchit`: closest-hit shader.
- `pathtrace.rahit`: primary any-hit shader.
- `pathtrace.rmiss`: primary miss shader.
- `pathtrace_shadow.rahit`: shadow any-hit shader.
- `pathtrace_shadow.rmiss`: shadow miss shader.
- `rt_common.glsl`: shared hardware RT shader declarations and helpers.

The hardware RT shader set is the renderer path.

## Denoising

The denoiser is implemented in `shaders/denoiser.comp`.

It reads:

- Raw color.
- Packed depth/normal.
- Variance.
- History color.
- Current world position.
- Previous world position.
- Previous/current camera data.

It writes:

- Denoised color.

The denoiser performs temporal reprojection and spatial filtering. It can be disabled, run only when the camera is still, or run while moving depending on settings.

## Temporal, ReSTIR, And Atmosphere Systems

`TemporalSystem` keeps temporal state separate from path-tracing accumulation. This lets the renderer reset accumulation for correctness while still making deliberate decisions about TAA and denoiser history validity.

Temporal resources include:

- Current and previous world position buffers.
- Velocity buffer.
- Denoiser history.
- TAA history.
- Diffuse/specular history buffers.
- Previous camera matrices and jitter state.

ReSTIR support is split across shader reservoirs and renderer-owned storage buffers:

- ReSTIR DI reservoir buffers for direct-light reuse.
- ReSTIR GI current, previous, and spatial reservoirs.
- Spatial reuse and GI final compute passes.
- Debug views for reservoir age, confidence, sample count, pairwise MIS, and GI validity/contribution.

The default Balanced path uses ReSTIR DI/GI in the beauty output. Disabling ReSTIR GI or selecting specific debug/reference views is the way to compare against the path tracing reference path.

Atmosphere and sky support is organized through `AtmosphereModel`, `AtmosphereLutSystem`, `AtmosphereSamplingSystem`, and `SkySystem`. LUT compute passes generate transmittance, multi-scatter, sky-view, aerial-perspective, and sky CDF data that the path tracer and fog pass can consume. The editor and diagnostics expose atmosphere statistics and debug views so sky/fog regressions are not limited to final-image inspection.

## Presentation

Presentation is split between compute and graphics passes.

`tone_map.comp` reads the raw, denoised, or TAA-resolved HDR renderer output, applies manual or auto exposure, runs the selected tone mapper, applies color grading, and writes the SDR presentation image. Tone mappers currently include Linear, Reinhard, Reinhard White, ACES, PBR Neutral, and AgX.

Auto exposure is scene-wide and histogram-based. `luminance_histogram.comp` bins log luminance from the denoised image, and `exposure_reduce.comp` chooses a configured percentile luminance, clamps the exposure target, and temporally adapts the current exposure.

`fullscreen.vert` emits a fullscreen triangle.

`fullscreen.frag` samples the already tone-mapped presentation image and writes it to the active render target.

`selection_outline.comp` can run after tone mapping to draw the selected-instance outline using the entity/instance ID buffer and packed depth/normal data.

After the fullscreen pass, the ImGui editor overlay is rendered.

## Editor And UI

The editor is ImGui-based and organized into panels:

- Viewport.
- Scene hierarchy.
- Inspector.
- Asset browser.
- Material editor.
- Render settings.
- Debug/profiler panel.
- Dockspace.

The editor builds `EditorRequests` rather than directly mutating every renderer subsystem. `Application` applies those requests at safe points in the frame.

Editor requests include:

- Renderer setting changes.
- Accumulation reset.
- glTF load.
- HDR load.
- Scene JSON save/load.
- Material update.
- Scene update.
- Entity creation/deletion/duplication/reparenting.
- Component creation for light, camera, and mesh renderer.
- Light, camera, transform, visibility, cast-shadow, and visible-to-camera edits.
- Transform gizmo live preview and undoable commit.
- Camera speed change.
- Camera reset.
- Shader reload.
- Layout reset.
- Denoiser toggle.
- Debug view toggle.
- Exit.

Current editor integration limits:

- Camera updates apply through the active-camera path and scene-camera piloting, but camera/editor workflows still need more manual validation.
- Creating or editing lights and cameras is routed through editor requests and scene operations; topology-changing edits require renderer rebuild/refit paths.
- Several Inspector branches for legacy/imported/fallback selections still show placeholder controls or temporary local values. Those controls can move in the UI without changing renderer state.
- The Inspector is the main editing surface for selected entities. The standalone Material Editor exists but is hidden by default.
- Material edits update `AssetManager` material data and attempt a GPU material-buffer update through `PathTracerRenderer::updateMaterials`.
- `Ctrl+L` rotates the scene-owned Primary Sun, but there is intentionally no separate sun drag indicator overlay at the moment.

## Runtime Controls

The app supports keyboard/mouse controls for:

- Camera movement.
- Mouse look.
- `Ctrl+L` Primary Sun drag rotation.
- Fullscreen toggle.
- Debug view cycling.
- Accumulation reset.
- Denoiser toggles.
- Sun/environment/direct-light toggles.
- Exposure adjustment.
- Environment intensity/rotation.
- Bounce count.
- A-trous denoiser iterations.

Files can be dropped onto the window:

- `.hdr` files load as environment maps.
- `.gltf` and `.glb` files trigger scene reload.

## Debugging And Profiling

`RendererDebugView` defines many parse-compatible debug views. The editor exposes a filtered set of useful/implemented views:

- Beauty.
- Variance.
- Normals.
- Reprojection confidence.
- Denoiser rejection.
- Depth.
- Roughness.
- Direct lighting.
- Indirect lighting.
- Emissive contribution.
- Environment contribution.
- Traversal steps.
- BVH depth.
- Instance ID.
- Mesh ID.
- TLAS steps.
- Traversal mismatch.
- Light PDF.
- BSDF PDF.
- MIS weight.
- Direct sample type.
- Albedo.
- Clay material.
- First-bounce throughput.
- Secondary environment miss.
- Bounce count.
- Secondary environment radiance.
- White environment transport.
- Motion vectors.
- Atmosphere sky view.
- Atmosphere transmittance.
- Atmosphere aerial perspective.
- Atmosphere multi-scatter.
- Temporal reactive mask.
- Temporal history weight.
- ReSTIR reservoir age.
- ReSTIR reservoir confidence.
- ReSTIR reservoir M.

`GpuProfiler` records GPU timings for major passes:

- Path tracing.
- ReSTIR clears, spatial reuse, copy, GI spatial reuse, and GI final shading.
- Atmosphere LUT generation, including transmittance, multi-scatter, sky-view, sky reprojection, sky CDF, and aerial perspective.
- Height fog.
- Denoising and denoiser bypass copies.
- Main history copy and TAA history copy.
- TAA resolve.
- Auto-exposure histogram clear, histogram build, and reduce/adaptation.
- Tone mapping.
- Selection outline.
- Fullscreen and editor presentation.

The renderer validation log records finer pass names, including hardware RT tracing, auto-exposure histogram/reduce, tone map compute, selection outline, history copy, and editor viewport presentation.

The editor also exposes hardware RT stats:

- Active/inactive status.
- BLAS count.
- Instance count.
- Acceleration-structure memory.
- Shader binding table size.

## Headless Diagnostics And Validation

`HeadlessDiagnostics`, `DiagnosticTools`, `DiagnosticImageExport`, `GpuValidation`, `RenderGraphDump`, and `ValidationSceneSuite` form the automation-facing diagnostic layer. The canonical usage guide is `docs/AI_DEBUG_PROFILING_TOOLS_USAGE.md`.

Headless diagnostics are designed around structured artifacts:

- `profile.json` for CPU/GPU timings, per-pass timings, pipeline stats, memory estimates, settings, validation counts, warnings, GPU/driver metadata, and warmup/profiled frame counts.
- `rendergraph.json` and `rendergraph.dot` for pass/resource topology and barriers.
- Debug view PNGs and an export manifest.
- `validation.txt` for pass events, barrier events, accumulation invalidations, scene update routes, and resource states.
- Optional memory, frame timeline, resource lifetime, shader, binding, crash-dump, and baseline/regression outputs.
- Optional RenderDoc capture paths when the engine is built with `RTV_HAS_RENDERDOC` and RenderDoc is actually injected/loaded.

The diagnostic architecture intentionally treats RenderDoc as optional. Capture failures are warnings; normal headless/editor execution must continue without RenderDoc.

Validation scenes live under `scenes/validation/` and cover Cornell, material grids, MIS, atmosphere, temporal stability, transform stress, and glTF extension cases. Lightweight validation scenes are preferred for smoke/regression work, while the heavy Sponza assets are reserved for performance and stress testing.

The required smoke-test shape is:

```text
rtvulkan.exe --headless --scene scenes/validation/cornell.rtlevel
    --warmup-frames 30 --frames 120 --fixed-seed 1
    --profile --profile-json out/profile.json
    --dump-rendergraph out/rendergraph.json
    --save-debug-views out/debug_views
    --make-debug-package out/debug_package
```

This workflow must keep producing machine-readable artifacts without requiring the editor.

## Documentation

Existing docs:

- `docs/AI_DEBUG_PROFILING_TOOLS_USAGE.md`: canonical AI/headless profiling, debug view, RenderDoc, validation, and baseline workflow.
- `docs/INTEGRATED_RENDERER_IMPLEMENTATION_PLAN.md`: current integrated renderer implementation plan.

Older migration/stabilization notes referenced by previous versions of this overview are not present in the current checkout. Treat `README.md`, this overview, the integrated implementation plan, and the AI debug/profiling guide as the active documentation set.

## Current Development State

The renderer is functional but not finished. The strongest parts of the architecture are:

- Clear application/runtime ownership.
- RAII resource wrappers.
- Explicit Synchronization2 barriers.
- Shared GPU scene representation.
- Hardware RT backend.
- Shader reflection-driven descriptor layout construction.
- Editor request flow separated from immediate renderer mutation.
- Undo/redo command stack for editor operations.
- Scene-document-backed renderer settings and JSON persistence.
- Rich debug and profiling support.
- Headless diagnostics with structured artifacts.
- Render graph dump/validation path.
- Validation-scene suite and baseline comparison workflows.
- Optional RenderDoc capture integration that does not affect normal execution.

Main active or future work areas:

- Continued real-time stability and performance tuning for default Balanced rendering.
- Hardware RT acceleration structure updates/refits.
- Hardening editor camera propagation and active-camera edge cases.
- Hardening authored light updates, GPU light-record weighting, and light editing workflows.
- Continuing to replace placeholder Inspector controls with controls backed by actual scene/renderer state.
- Finer geometry splitting for opaque, alpha-tested, and single-sided traversal paths.
- Further rough dielectric/specular sampling and MIS tuning in the hardware RT path.
- Fully bindless material texture residency.
- Broader glTF material extension support.
- More complete scene instancing and transform update paths.
- Render graph resource-state tracking and barrier validation.
- Denoiser, TAA, and reprojection validation against hardware RT output and the WebGPU reference.
- Async compute validation and profiler-lane reporting.
- ReSTIR DI/GI validation through beauty-path output, debug views, JSON reports, and Balanced budget gates.
- More complete atmosphere/fog regression coverage.

## Important Architectural Takeaway

The project is not just a minimal Vulkan sample. It is a staged renderer/editor migration with a real scene pipeline, GPU scene abstraction, hardware ray tracing backend, denoiser, debug tooling, and editor integration.

The central data flow is:

```text
CLI / editor / headless diagnostics
    -> Application
    -> SceneDocument + SceneRegistry + AssetManager
    -> SceneUpdateRouter / SceneToGpuSceneBuilder
    -> GpuScene
    -> PathTracerRenderer
    -> RenderGraph plan + Vulkan command recording
    -> hardware RT path trace
    -> ReSTIR / atmosphere / fog / denoiser / TAA / tone map
    -> fullscreen or offscreen presentation
    -> ImGui editor overlay and/or diagnostic artifacts
```

Important ownership boundaries:

- `Application` owns runtime mode, scene/editor orchestration, frame loop, and safe request application.
- `SceneDocument` and `AssetManager` own editable/imported CPU state.
- `SceneToGpuSceneBuilder` translates CPU scene state into renderer-facing GPU scene inputs.
- `GpuScene` owns shader-visible scene buffers, textures, light data, environment sampling data, and hardware RT build inputs.
- `PathTracerRenderer` owns render targets, temporal resources, pipelines, pass recording, debug views, and validation/profiling hooks.
- `RenderGraph` describes pass/resource/barrier intent for dumps and validation, with fuller execution ownership still evolving.
- `HeadlessDiagnostics` consumes renderer/application state and writes stable artifacts for automation.

This separation lets the project continue improving editor features, scene import, GPU data layout, renderer tracing, RenderGraph validation, and AI diagnostic workflows without rewriting the full application shell.
