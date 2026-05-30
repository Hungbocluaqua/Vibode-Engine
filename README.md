# Ray Tracing Engine Vulkan Port

This directory contains the native Vulkan 1.3 / C++20 port of the WebGPU path tracing renderer. Rendering is handled by the Vulkan KHR hardware ray tracing path.

The port is operational: it opens a native window, runs a Vulkan KHR hardware ray tracing path tracer on capable devices, uses ReSTIR DI/GI, denoises temporally/spatially, resolves TAA, presents through compute tone mapping and a fullscreen pass, supports glTF/HDR inputs, exposes an ImGui editor, and includes structured GPU timing/debug output. It is now an editor-oriented renderer prototype with scene hierarchy, inspector, undo/redo, render settings persistence, incremental scene-update paths, headless diagnostics, RenderGraph dumps, optional RenderDoc capture, opacity micromaps, SER-capability reporting, and opt-in wavefront path tracing probes.

## Current Real-Time Defaults

- `Balanced` is the default render preset for interactive/game-style use.
- The real-time path is capped at effective `1 SPP` by default through `Limit to 1 SPP`; higher requested SPP is opt-in for stills, screenshots, references, or high-end budgets.
- ReSTIR DI and ReSTIR GI are enabled by default in the beauty path.
- Opacity micromaps and shader execution reordering are requested by default on capable hardware. Unsupported devices clamp those paths off and report the reason in profile JSON.
- The Balanced preset targets a 16.6 ms GPU frame budget using render scale `0.65`, max bounces `5`, one environment sample, three a-trous iterations, ReSTIR GI spatial/final passes, temporal denoising, and TAA.
- Motion stability is prioritized over raw per-frame sampling. Use ReSTIR reuse, denoiser history, moment tracking, and TAA before raising SPP.

## Implemented

- GLFW window with Vulkan surface creation
- Vulkan 1.3 instance/device setup with validation debug messenger
- Physical-device selection requiring swapchain support, dynamic rendering, Synchronization2, and required descriptor features
- Logical device, graphics/present queues, and VMA allocator
- Swapchain creation, image views, resize handling, and per-swapchain-image present semaphores
- Per-frame command pools, command buffers, semaphores, and fences
- Dynamic rendering submitted through `vkQueueSubmit2`
- RAII `Buffer` abstraction for GPU-only, upload, readback, and persistently mapped buffers
- RAII `Image` abstraction for sampled textures, storage images, render targets, mip-capable images, image views, and descriptor metadata
- Synchronization2 image and buffer barrier helpers using `vkCmdPipelineBarrier2`
- Fence-backed `UploadContext` and `BufferUploader` for staging buffer, texture, and scene uploads
- `TextureLoader` and HDR environment upload paths
- Descriptor layout cache, descriptor allocator, descriptor writer, and descriptor set wrapper
- Per-frame descriptor arenas and transient CPU-visible uniform ring buffers
- GLSL to SPIR-V shader compilation through `glslangValidator`
- SPIR-V reflection for descriptor bindings, descriptor types, stage visibility, and push constants
- Pipeline cache, compute pipeline wrapper, graphics pipeline wrapper, ray tracing pipeline wrapper, and dynamic-rendering fullscreen pipeline
- Compact auxiliary buffers for variance, depth/normal, world position, and temporal history
- Temporal denoiser with reprojection, disocclusion rejection, luminance clipping, reactive masking, moving-camera preview support, and multi-scale a-trous filtering
- Temporal anti-aliasing with independent temporal frame tracking, TAA-only camera jitter, depth/world-position validation, and configurable sharpening
- Render presets (`Low`, `Balanced`, `Ultra`, `Custom`) that tune path tracing, ReSTIR, denoiser, TAA, SPP limiting, and render scale together
- True per-frame SPP control with a real-time `1 SPP` limiter, editor slider, CLI overrides, and profile JSON reporting
- Cornell-box scene and optional glTF/GLB import through `--gltf`
- glTF camera import into editable camera entities
- Radiance HDR environment loading through `--hdr`
- Environment row/column CDF generation and shader-side importance sampling
- Environment direct lighting / next-event estimation at every non-delta bounce, with a configurable per-hit environment sample count
- Separate lighting controls for procedural sky intensity, loaded HDR environment intensity, and camera-visible background intensity
- CPU BVH construction with Morton ordering, binned SAH, BVH4-style packed upload data, and rope traversal
- Scene buffers for materials, primitives, mesh records, instance records, light records, local mesh data, local BVHs, and TLAS nodes
- glTF texture residency through a fixed sampled texture array
- Shader-side base-color, normal, metallic-roughness, and emissive texture sampling
- Direct/emissive/environment lighting debug views plus PDF/MIS diagnostics
- ReSTIR DI and ReSTIR GI reservoir reuse with GI spatial/final beauty-path passes, debug views, and profile/render-graph reporting
- RenderGraph JSON/DOT/lifetime diagnostics for pass/resource/barrier inspection
- Headless diagnostics with profile JSON, debug-view export, debug packages, memory dumps, frame timelines, resource lifetimes, shader reports, baseline checks, and validation suites
- GPU timestamp profiling for path tracing, denoising, and fullscreen passes
- ImGui editor with viewport, scene hierarchy, inspector, asset browser, render settings, debug/profiler, optional material editor, sample count, reset reason, resolution, and pass timings
- Scene hierarchy selection sync, selected-instance outline, transform gizmo preview/commit, active scene-camera piloting, and undo/redo-backed editor operations
- Inspector-backed transform, camera, light, mesh visibility, material assignment, cast-shadow, and visible-to-camera edits
- WASD/mouse camera controls, `Ctrl+L` sun rotation drag, pointer release on focus loss, and `F11` borderless fullscreen
- Primary Vulkan hardware ray tracing backend using `VK_KHR_acceleration_structure` and `VK_KHR_ray_tracing_pipeline`
- BLAS/TLAS construction for fallback Cornell-box and imported glTF triangle meshes
- Hardware RT path loop with raygen-owned multi-bounce tracing, direct/emissive/environment lighting, shadow rays, MIS/PDF diagnostics, denoiser auxiliary outputs, and alpha-aware any-hit shaders
- Hardware RT per-triangle material metadata so closest-hit and any-hit shaders use direct material lookup instead of scanning primitive records
- Hardware RT reduced ray payload: hit shaders return IDs, UVs, normals, and tangent basis; raygen performs material decode/texture evaluation after a hit is accepted
- Hardware RT optimized any-hit path that samples only alpha data when alpha is relevant and terminates shadow rays immediately for accepted opaque hits
- Conservative hardware RT opaque traversal fast path using `VK_GEOMETRY_OPAQUE_BIT_KHR` for meshes known to be alpha-free and double-sided
- Instance visibility flags for hidden, camera-hidden, and non-shadow-casting objects in ray queries
- AgX, ACES, PBR Neutral, Reinhard, Reinhard White, and Linear tone mappers
- Hardware RT debug/profiler stats for BLAS count, instance count, acceleration-structure memory, and SBT size
- Hardware opacity micromap support for eligible alpha-tested geometry when `VK_EXT_opacity_micromap` is available
- Shader execution reordering support for opt-in wavefront trace probes when `VK_NV_ray_tracing_invocation_reorder` is available
- Ray traced motion blur, thin-lens depth of field, homogeneous volumes, and a guarded caustic-visibility probe behind explicit settings/CLI flags
- Opt-in wavefront queue, trace, shade, shadow, compaction, sorting, validation, debug-view, and final-output paths used for architecture-v2 validation

## Still In Progress

- Hardware RT material, lighting, temporal stability, and traversal stabilization
- Hardware RT acceleration-structure update/refit paths for transform-heavy editing
- Finer hardware RT geometry splitting so opaque single-sided and alpha-tested primitives can use different traversal/hit-group policies within the same mesh
- Further rough dielectric/specular sampling and MIS tuning
- Fully bindless material texture residency
- Broader glTF material extension support
- More complete scene instancing and transform update paths
- Fuller RenderGraph execution ownership beyond the current diagnostic/planning path
- More denoiser/reprojection validation on the hardware RT output
- Continued editor workflow polish around component creation, camera activation, hierarchy reveal, and inspector edge cases
- Wavefront final-output parity, full wavefront-owned debug-view coverage, and sorting/SER performance acceptance before wavefront becomes a default renderer path
- Full MNEE caustics beyond the current guarded caustic-visibility probe

## Build

Requirements:

- Windows
- Visual Studio 2022 with C++ tools
- CMake
- Vulkan SDK
- vcpkg dependencies for this project

Configure and build from the repository root:

```powershell
cmake -S native/vulkan -B native/vulkan/build -G "Visual Studio 17 2022" -A x64 -DCMAKE_TOOLCHAIN_FILE=C:/Users/HomePc/vcpkg/scripts/buildsystems/vcpkg.cmake
cmake --build native/vulkan/build --config Debug
```

If the terminal does not see `VULKAN_SDK`, pass the SDK paths explicitly:

```powershell
$env:VULKAN_SDK='C:\VulkanSDK\1.4.350.0'
cmake -S native/vulkan -B native/vulkan/build -G "Visual Studio 17 2022" -A x64 -DCMAKE_TOOLCHAIN_FILE=C:/Users/HomePc/vcpkg/scripts/buildsystems/vcpkg.cmake -DVulkan_INCLUDE_DIR=C:/VulkanSDK/1.4.350.0/Include -DVulkan_LIBRARY=C:/VulkanSDK/1.4.350.0/Lib/vulkan-1.lib
cmake --build native/vulkan/build --config Debug
```

Run:

```powershell
native\vulkan\build\Debug\rtvulkan.exe
```

Smoke test:

```powershell
native\vulkan\build\Debug\rtvulkan.exe --frames 6
```

## Runtime Examples

```powershell
native\vulkan\build\Debug\rtvulkan.exe --debug-view direct
native\vulkan\build\Debug\rtvulkan.exe --debug-view indirect
native\vulkan\build\Debug\rtvulkan.exe --debug-view mis-weight
native\vulkan\build\Debug\rtvulkan.exe --gltf path\to\scene.glb
native\vulkan\build\Debug\rtvulkan.exe --hdr path\to\environment.hdr
native\vulkan\build\Debug\rtvulkan.exe --gltf path\to\scene.glb --hdr path\to\environment.hdr
```

Hardware RT requires `VK_KHR_acceleration_structure`, `VK_KHR_ray_tracing_pipeline`, `VK_KHR_deferred_host_operations`, `VK_KHR_buffer_device_address`, `VK_KHR_spirv_1_4`, and `VK_KHR_shader_float_controls`.
In hardware RT mode, raygen owns the path loop and launches primary, bounce, and shadow rays explicitly while keeping pipeline recursion depth at `1`.
Hardware RT uses a correctness-first material policy: alpha and single-sided material rules are enforced by any-hit where needed. Meshes that are known to be alpha-free and double-sided are marked opaque so Vulkan can skip any-hit traversal for that geometry.

## Runtime Controls

- Click captures mouse look.
- `Esc`, `Alt+Tab`, or focus loss releases mouse look.
- `WASD` moves the camera.
- `Q/E` or `Ctrl/Space` moves vertically.
- `Shift` increases camera speed.
- `F11` toggles borderless fullscreen.
- Hold `Ctrl+L` and drag to rotate the scene Primary Sun. Quick tapping `Ctrl+L` keeps the open-level shortcut behavior.
- `F1` cycles debug views.
- `0` returns to beauty view.
- `R` resets accumulation.
- `F2` toggles denoising.
- `F3` toggles denoising while the camera is moving.
- `F4` toggles the scene Primary Sun.
- `F5` toggles environment lighting.
- `F6` toggles direct lighting.
- `+/-` adjusts exposure.
- `</>` adjusts environment intensity.
- `[`/`]` rotates the environment.
- `PageUp/PageDown` changes max bounces.
- `Home/End` changes a-trous denoiser iterations.

The ImGui editor exposes the same core controls plus scene hierarchy, inspector editing, asset browsing, render settings, HDR path loading, profiler timing, hardware RT AS/SBT stats, accumulated sample count, requested/effective SPP controls, resolution, debug view selection, accumulation reset reason, TAA sharpening, tone mapper selection, and environment lighting controls.

SPP control behavior:

- `Limit to 1 SPP` is enabled by default for real-time presets. It forces effective path samples per pixel per frame to `1`, even if the requested SPP slider is higher.
- `Samples Per Pixel` requests `1..8` path samples per pixel per frame when the limiter is disabled.
- CLI equivalents are `--spp <N>` / `--samples-per-pixel <N>` and `--spp-limit on|off` / `--limit-spp on|off`.
- Profile JSON records `samples_per_pixel`, `limit_samples_per_pixel`, and `effective_samples_per_pixel` under `settings`.

Environment control behavior:

- `Sky Intensity` controls the procedural fallback sky only.
- `Environment Intensity` controls loaded HDR environment lighting only.
- `Background Intensity` scales the camera-visible sky/background, so HDR lighting can be raised without immediately blowing out the visible sky.
- `Environment Samples` controls direct environment-light samples per surface hit. `1` is the interactive default; higher values reduce sky-light noise at a proportional performance cost.
- The procedural sky is uploaded through the same environment texture path as HDRs, but it is flagged separately on the GPU so it uses `Sky Intensity` instead of `Environment Intensity`.
- The scene-owned Primary Sun component is the source of truth for sun direction, illuminance, angular radius, and color temperature. Legacy directional sun lights are migrated on load.

## Debug Views

Supported debug views include:

- `beauty`
- `variance`
- `normals`
- `depth`
- `roughness`
- `reprojection-confidence`
- `denoiser-rejection`
- `direct-lighting`
- `indirect-lighting`
- `environment-contribution`
- `emissive-contribution`
- `instance-id`
- `mesh-id`
- `light-pdf`
- `bsdf-pdf`
- `mis-weight`
- `direct-sample-type`
- `albedo`
- `clay-material`
- `first-bounce-throughput`
- `secondary-environment-miss`
- `bounce-count`
- `secondary-environment-radiance`
- `white-environment-transport`
- `motion-vectors`
- `atmosphere-sky-view`
- `atmosphere-transmittance`
- `atmosphere-aerial-perspective`
- `atmosphere-multi-scatter`
- `temporal-reactive-mask`
- `temporal-history-weight`
- `restir-reservoir-age`
- `restir-reservoir-confidence`
- `restir-reservoir-m`

Some parse-compatible low-level traversal views remain available for saved settings and CLI compatibility, but the editor filters out placeholder/no-op views from normal cycling.

## Renderer Architecture Overview

The renderer is organized around a few ownership boundaries. `Application` owns runtime mode and editor/headless orchestration. `SceneDocument` and `AssetManager` own editable CPU state. `SceneToGpuSceneBuilder` converts editor state into renderer-facing scene data. `GpuScene` owns shader-visible buffers, textures, light data, environment sampling data, and hardware RT build inputs. `PathTracerRenderer` owns render targets, temporal resources, pipelines, pass recording, debug views, validation hooks, and profiling hooks. `CommandSystem` owns frame submission and presentation synchronization.

The high-level data flow is:

```text
CLI / editor / headless diagnostics
    -> Application
    -> SceneDocument + SceneRegistry + AssetManager
    -> SceneUpdateRouter / SceneToGpuSceneBuilder
    -> GpuScene
    -> PathTracerRenderer
    -> RenderGraph plan + Vulkan command recording
    -> KHR hardware ray tracing path trace
    -> ReSTIR / atmosphere / fog / denoiser / TAA / tone map
    -> fullscreen presentation or headless artifact export
```

### Application And Runtime Modes

`Application` is the executable runtime shell. It creates the GLFW window in editor mode, initializes Vulkan, owns the camera controller, loads `.rtlevel`, glTF/GLB, and HDR files, applies CLI overrides, processes editor requests, runs async scene loading, and decides whether a request can be applied incrementally or needs a renderer rebuild.

Interactive mode creates the window, swapchain, ImGui overlay, editor panels, and viewport presentation path. Headless mode uses the same Vulkan device, scene conversion, renderer, shader, and diagnostic paths but skips the desktop window, editor UI, and swapchain presentation. Headless runs are the preferred path for reproducible profiling because warmup frames, fixed seeds, profile windows, debug-view export, RenderGraph dumps, and debug packages are all controlled from the CLI.

`CommandSystem` is the frame-submission boundary. It owns per-frame command pools, command buffers, semaphores, fences, swapchain acquisition, queue submission, and presentation. The renderer records work into command buffers supplied by `CommandSystem`; it does not own window acquire/present state.

### Vulkan Device And Resource Core

`VulkanContext` owns instance and device setup. It discovers and reports descriptor indexing, memory budget support, hardware ray tracing features, opacity micromap support, shader execution reordering support, ray tracing motion blur support, queue families, timeline semaphore support, and validation/debug messenger state.

The resource layer is deliberately explicit:

- `ResourceAllocator` wraps VMA and debug naming.
- `Buffer` wraps `VkBuffer`, `VmaAllocation`, mapping, flushing, invalidation, descriptor metadata, resizing, and device-address queries.
- `Image` wraps `VkImage`, `VmaAllocation`, image views, sampled/storage descriptors, layout tracking, and mip-capable texture/render-target usage.
- `UploadContext`, `BufferUploader`, and `BatchUploader` stage CPU data into GPU buffers and images.
- Synchronization uses Vulkan Synchronization2 helpers and avoids hidden legacy barriers.

This layer keeps memory ownership, descriptor metadata, and barrier transitions visible enough for RenderGraph dumps, validation logs, and memory reports.

### Descriptors, Shaders, And Pipelines

Descriptor and pipeline objects are built from small reusable helpers:

- `DescriptorLayoutCache` caches `VkDescriptorSetLayout` objects.
- `DescriptorAllocator` provides per-frame descriptor arenas and growth stats.
- `DescriptorWriter` batches storage/uniform/image/sampler/acceleration-structure writes.
- `ShaderCompiler` invokes `glslangValidator`, emits SPIR-V, supports shader variants, and feeds shader reports.
- `ShaderReflection` uses SPIRV-Reflect to derive descriptor layouts and push constants.
- `PipelineCache`, `ComputePipeline`, `GraphicsPipeline`, and `RayTracingPipeline` own the Vulkan pipeline objects.

The renderer uses many compute pipelines for post/temporal work, one fullscreen graphics pipeline for presentation, and KHR ray tracing pipelines for the megakernel and opt-in wavefront trace paths. SER uses a separate wavefront raygen variant compiled with `RTV_SER_ENABLED=1` and is selected only when the device supports `VK_NV_ray_tracing_invocation_reorder` and a wavefront trace path is active.

### Scene And Asset Model

The CPU scene model separates imported assets from editable entities. `AssetManager` owns textures, materials, and meshes. `GltfLoader` imports glTF/GLB data, caches imported scenes, stores material extension data, and preserves texture/material handles. `SceneDocument` owns the editable scene: entities, transforms, cameras, lights, mesh renderers, environment settings, render settings, source paths, active camera, dirty state, and JSON serialization.

`SceneUpdateRouter` maps document dirty flags to renderer actions: material update, transform/TLAS update, light update, environment update, camera update, visibility update, renderer settings update, or topology rebuild. This keeps routine editor changes from forcing a full renderer rebuild when an incremental path exists.

`SceneToGpuSceneBuilder` turns the document into a renderer-facing `SceneAsset` plus renderer settings and instance-entity mappings. `GpuScene` then uploads shader-visible buffers and image descriptors for vertices, indices, materials, primitives, mesh records, instance records, light records, local mesh/BVH data, hardware RT material IDs, environment CDFs, and material texture tables.

### Hardware Ray Tracing Backend

The production path is the Vulkan KHR hardware ray tracing backend. `RayTracingScene` builds BLAS objects from imported/fallback triangle geometry and a TLAS from visible scene instances. `RayTracingPipeline` owns the KHR ray tracing pipeline, shader groups, and shader binding table.

The megakernel raygen shader owns the path loop. It traces primary, shadow, and bounce rays with pipeline recursion depth kept at `1`. Hit shaders return compact hit records: primitive/material IDs, UVs, normals, tangent basis, and hit metadata. Raygen performs material decoding, texture sampling, BSDF evaluation, direct lighting, environment lighting, emissive contribution, MIS accounting, ReSTIR candidate generation, denoiser auxiliary writes, and debug-view data writes after a hit is accepted.

Traversal policy is conservative and material-correct. Opaque geometry can use `VK_GEOMETRY_OPAQUE_BIT_KHR`; alpha-tested and single-sided cases use any-hit paths where needed. Opacity micromaps are requested by default on capable devices and used for eligible alpha-tested geometry; unsupported devices or ineligible scenes fall back to the alpha any-hit path and report the reason in profile JSON.

### Main Frame Pipeline

`PathTracerRenderer` is the central pass recorder. A normal Balanced frame follows this shape:

1. Clamp/adapt settings for memory pressure and adaptive quality.
2. Update camera, previous-camera, renderer, debug, ReSTIR, atmosphere, and post-process uniforms.
3. Transition writable HDR, auxiliary, and history resources.
4. Run KHR hardware ray tracing into the raw HDR output plus path data, variance, depth/normal, world-position, velocity, entity-ID, and ReSTIR reservoir outputs.
5. Run ReSTIR DI/GI spatial reuse and GI final contribution when enabled.
6. Run atmosphere/fog integration when the relevant path is active.
7. Update temporal moments and dispatch the temporal/a-trous denoiser, or copy/bypass when denoising is disabled.
8. Resolve TAA and update TAA history.
9. Update/copy denoiser, temporal, world-position, diffuse, and specular histories for the next frame.
10. Build/reduce auto-exposure histograms when auto exposure is enabled.
11. Tone map into the presentation image.
12. Draw selection outline when an entity is selected.
13. Present through the fullscreen graphics pass and render ImGui in editor mode.

Path tracing accumulation and temporal history are related but not identical. Accumulation resets on camera, resize, scene, material, light, environment, render setting, denoiser/TAA, debug view, and shader changes. Temporal resources track camera cuts, motion, reprojection validity, reactive masks, and history confidence so TAA/denoising can remain stable without pretending the path-tracing sample count is still accumulating.

### ReSTIR, Denoising, TAA, And Atmosphere

ReSTIR DI and ReSTIR GI are part of the default beauty path. DI/GI reservoir buffers are owned by `PathTracerRenderer`, populated from path tracing/shader passes, copied or aged across frames, and consumed by spatial/final compute passes. Debug views and profile JSON expose reservoir age, confidence, M values, GI validity, GI initial/temporal/spatial/final states, and pass timings.

The denoiser is temporal first and spatial second. It reads raw lighting/path data, depth/normal, variance, velocity, world positions, previous camera data, and history images. It performs reprojection, disocclusion checks, luminance/moment validation, reactive masking, and a-trous filtering. TAA runs after denoising and owns its own history so display stability can be tuned separately from path-tracing accumulation.

Atmosphere and sky are split into LUT generation, sampling data, and integration passes. Transmittance, multi-scatter, sky-view, aerial perspective, sky CDF, and fog integration resources are generated through compute passes and then consumed by the path tracer, fog pass, debug views, and profile reports.

### RenderGraph And Diagnostics

The RenderGraph layer currently acts as both a planning model and a diagnostic contract. `PathTracerRenderer` still records most Vulkan commands directly, but it also emits a RenderGraph plan describing pass order, resource reads/writes, queue domains, barrier intent, logical buffer offsets, transient lifetimes, and alias groups.

`--dump-rendergraph` and `--dump-rendergraph-dot` export this plan as JSON and DOT. `--dump-resource-lifetimes`, `--dump-frame-timeline`, `--dump-memory`, `--dump-shader-report`, and `--dump-bindings` expose related diagnostics. The long-term direction is fuller RenderGraph execution ownership, but the current dumps are already used as regression artifacts and should remain machine-readable.

`HeadlessDiagnostics` writes `profile.json`, validation logs, debug-view PNGs, debug packages, baseline comparisons, budget checks, frame sequences, and optional RenderDoc captures. RenderDoc support is optional by design; capture failure is a warning, not a renderer startup failure.

### Editor Integration

The editor is request-driven. Panels build `EditorRequests` instead of mutating every renderer object directly. `Application` applies those requests at safe points before or after command submission depending on whether the change can be incremental or requires resource rebuilds.

Editor state includes scene hierarchy, inspector, asset browser, material editor, render settings, GPU diagnostics, debug/profiler panel, viewport texture descriptors, selection outline, active scene-camera piloting, transform gizmo preview/commit, undo/redo, and scene JSON save/load. Renderer settings are synchronized back to `SceneDocument` so `.rtlevel` files preserve user-visible render settings while hardware-gated options still clamp to the effective renderer state at startup.

### Wavefront Architecture V2 Path

The wavefront renderer is implemented as an opt-in architecture-v2 validation path, not the default game renderer. It allocates queue buffers, generates primary rays, traces queues through KHR RT, shades hits in compute, traces shadow queues, compacts secondary rays, optionally sorts by event/material buckets, validates parity against the megakernel path, and can write wavefront-owned final output through `--wavefront-final-output on`.

Wavefront diagnostics report queue capacity, occupancy, overflow/starvation, compaction counts, secondary trace/shade timings, sorted trace/shade timings, direct-light parity, ReSTIR DI/GI candidate counters, SER timing, and queue memory. The default megakernel smoke keeps wavefront queues disabled, so wavefront memory should be zero unless a wavefront probe or wavefront debug view is explicitly selected.

Current wavefront status is intentionally conservative: queue/shade/shadow/compact/final-output paths exist, but full wavefront ReSTIR/debug-view ownership, sorted-path net performance, and SER performance acceptance remain validation gates before wavefront can be considered a default renderer path.

### Advanced Feature Flags

Several features are implemented but remain opt-in or hardware-gated:

- OMM is requested by default on capable hardware and falls back when unsupported or when no eligible alpha-tested geometry exists.
- SER is requested by default on capable hardware but only affects active wavefront trace paths.
- Ray traced motion blur requires `VK_NV_ray_tracing_motion_blur` and `--motion-blur on`.
- Thin-lens depth of field is enabled by `--dof-aperture-radius > 0`.
- Homogeneous volumes are enabled by `--volume on` or nonzero volume coefficients.
- MNEE caustics are currently a guarded caustic-visibility probe, enabled by `--caustics on` or the `caustic-visibility` debug view.

These flags keep the default real-time path focused on predictable 1-SPP ReSTIR/denoise/TAA behavior while still allowing targeted validation of advanced renderer features.

## Development Direction

Recommended next work:

1. Stabilize the hardware RT path across Cornell, Sponza, HDR, alpha-cutout, emissive, and normal-map scenes.
2. Continue rough dielectric/specular BSDF, PDF, and MIS tuning in the hardware RT shaders.
3. Add hardware RT TLAS/BLAS refit or rebuild paths for transform-heavy editing.
4. Split mixed-material hardware RT meshes into opaque, alpha, and single-sided geometry classes so more triangles can use fast opaque traversal without losing glTF material correctness.
5. Move fixed texture arrays toward a full bindless descriptor model.
6. Harden render graph state tracking and barrier validation.
7. Expand glTF material support and tune temporal reprojection/denoising against hardware RT output.
