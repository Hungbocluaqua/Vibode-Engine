# Project Overview

This document summarizes the current structure and architecture of the native Vulkan renderer in `native/vulkan`.

The project is a C++20 / Vulkan 1.3 port of a path-tracing renderer. It builds a native Windows executable named `rtvulkan` and currently supports compute path tracing, optional Vulkan KHR hardware ray tracing, glTF/GLB scene loading, HDR environment maps, progressive accumulation, temporal/spatial denoising, ImGui editor panels, GPU timing, and renderer debug views.

The renderer is operational, but it is still in migration and stabilization. The README and docs describe remaining work around compute-vs-hardware-RT visual parity, bindless material residency, glTF material coverage, scene transform updates, hardware RT refit paths, render graph/state tracking, and denoiser tuning.

## Repository Shape

Important top-level files and directories:

- `CMakeLists.txt`: CMake build definition for the `rtvulkan` executable.
- `README.md`: user-facing build/run instructions and current feature status.
- `HardwareRT_Implementation_plan.md`: detailed phased plan for Vulkan hardware ray tracing.
- `docs/`: architecture and migration notes.
- `include/rtv/`: public/internal C++ headers for renderer modules.
- `src/rtv/`: C++ implementation files.
- `src/main.cpp`: executable entry point and CLI parsing.
- `shaders/`: GLSL compute, fullscreen, and ray tracing shaders.
- `build/`: local CMake build directory.
- `Sponza/`: local sample scene assets.
- `citrus_orchard_road_puresky_4k.hdr`: local HDR environment asset.
- `rtv_editor.ini`: ImGui/editor layout state.
- `run_rtvulkan.bat`, `run_rtvulkan_debug.bat`: local launcher scripts.

At the time of inspection, the project contained roughly:

- 65 C++ source files.
- 71 header files.
- 12 shader files.
- Several migration/stabilization documents.

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
- stb.
- tinygltf.
- SPIRV-Reflect.
- ImGui docking sources.

The CMake file currently includes local fallback paths for the Vulkan SDK, vcpkg, and ImGui source tree. That makes the project convenient on the current machine, but not fully portable without path cleanup.

## Program Entry

`src/main.cpp` is intentionally small. It parses CLI options and starts the application.

Supported options include:

- `--frames <count>`: run for a fixed number of frames, useful for smoke tests.
- `--debug-view <name>`: start in a specific renderer debug view.
- `--gltf <path>`: load a glTF/GLB scene.
- `--hdr <path>`: load an HDR environment map.
- `--backend compute|auto|rt`: choose the renderer backend.

After parsing, `main()` constructs `rtv::Application` and calls `run()`.

## Application Layer

`Application` is the top-level owner of the runtime. It is declared in `include/rtv/Application.h` and implemented in `src/rtv/Application.cpp`.

It owns:

- The GLFW window.
- Runtime input handling.
- Camera controller.
- Scene loading and async glTF reloads.
- Asset manager.
- Scene document.
- Vulkan context.
- Resource allocator.
- Upload context and buffer uploader.
- Swapchain.
- Command system.
- ImGui overlay.
- Path tracer renderer.

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

## Vulkan Core

### `VulkanContext`

`VulkanContext` owns the Vulkan instance and device-level setup.

Responsibilities:

- Instance creation.
- Validation layer/debug messenger setup.
- GLFW surface creation.
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
- `ShaderCompiler`: invokes `glslangValidator` and recompiles GLSL when needed.
- `ShaderReflection`: uses SPIRV-Reflect to discover descriptor bindings and push constants.
- `ShaderModule`: owns compiled SPIR-V modules.
- `PipelineCache`: owns Vulkan pipeline cache state.
- `ComputePipeline`: owns compute pipeline state.
- `GraphicsPipeline`: owns dynamic-rendering fullscreen graphics pipeline state.
- `RayTracingPipeline`: owns Vulkan ray tracing pipeline and shader binding tables.

The descriptor and pipeline architecture is documented further in `docs/DESCRIPTOR_PIPELINE_SYSTEM.md`.

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
- `FullSceneRebuild`.

### Scene Conversion

`SceneToGpuSceneBuilder` converts a `SceneDocument` into renderer-facing data. It returns:

- A generated `SceneAsset`.
- Instance entity mapping.
- Renderer settings.
- Accumulation reset reason.
- Whether a renderer rebuild is required.

This keeps editor edits separate from the lower-level GPU scene representation.

Current limitation: this conversion path is partial. Mesh entities and render/environment settings are the primary data that reach the renderer. Editor `Light` components and some camera/editor properties exist in the `SceneDocument`, but they are not fully exported into GPU lighting or consistently applied to the active renderer camera on every edit.

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

The compute and hardware RT backends share much of this scene data. That is important because the hardware RT backend is intended to match the compute backend visually rather than evolve into a separate renderer.

Lighting note: GPU light records are currently built from emissive mesh and sphere geometry, plus the renderer's sun/environment paths. Newly created editor `Light` components are not yet converted into `GpuLightRecord` data, so creating or editing a point/directional/area light in the editor does not currently affect rendered lighting.

## Path Tracer Renderer

`PathTracerRenderer` is the main rendering engine.

It owns:

- The active `GpuScene`.
- Render resolution state.
- Accumulation state.
- Camera uniforms.
- Renderer settings.
- Debug parameters.
- Raw render image.
- Denoised image.
- History image.
- Accumulation buffer.
- Variance buffer.
- Depth/normal buffer.
- World position buffers.
- Uniform buffers.
- Descriptor layout cache.
- Pipeline cache.
- Shader modules.
- Compute path tracing pipeline.
- Denoiser compute pipeline.
- Fullscreen graphics pipeline.
- Optional ray tracing pipeline.
- Optional ray tracing scene.
- Per-frame descriptor arenas.
- GPU profilers.
- Renderer validation log.

Main frame flow:

1. Begin the frame and update camera/settings uniforms.
2. Transition the raw output image for shader writes.
3. Run path tracing through the selected backend.
4. Barrier path tracing outputs for denoiser reads.
5. Run temporal/spatial denoising if enabled.
6. Copy history resources for the next frame.
7. Transition the selected output for fullscreen sampling.
8. Render fullscreen presentation.
9. Render the editor overlay.

Accumulation resets when relevant state changes:

- Startup.
- Resize.
- Camera movement.
- Manual reset.
- Render settings change.
- Lighting change.
- Environment change.
- Denoiser setting change.
- Debug view change.
- Scene change.
- Material change.
- Shader reload.
- Backend change.

## Renderer Backends

The renderer supports three backend modes:

- `Auto`: use hardware RT when available, otherwise compute.
- `Compute`: always use compute path tracing.
- `HardwareRayTracing`: require Vulkan KHR hardware RT and fail clearly if unsupported.

### Compute Backend

The compute backend uses `shaders/pathtrace.comp`.

It performs:

- Primary ray generation.
- BVH/TLAS traversal.
- Material evaluation.
- Texture sampling.
- Direct lighting.
- Emissive lighting.
- Environment lighting.
- Environment importance sampling.
- Multiple bounces.
- MIS/PDF diagnostics.
- Progressive accumulation.
- Debug view output.
- Denoiser auxiliary buffer output.

It binds scene data directly as storage buffers, sampled images, samplers, and uniforms.

### Hardware Ray Tracing Backend

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

`RayTracingPipeline` builds:

- Ray generation group.
- Miss groups.
- Hit groups.
- Shader binding tables.

The ray generation shader owns the multi-bounce path loop. Hit shaders return compact hit information such as IDs, UVs, normals, and tangent basis. Material decoding and texture evaluation happen after hits are accepted.

The hardware RT descriptor set includes the TLAS and an RT triangle-material-ID buffer in addition to the shared scene/material/environment buffers.

## Shaders

Shader files:

- `demo_compute.comp`: simple compute demo.
- `pathtrace.comp`: compute path tracer.
- `denoiser.comp`: temporal/spatial denoiser.
- `fullscreen.vert`: fullscreen triangle vertex shader.
- `fullscreen.frag`: fullscreen presentation/debug fragment shader.
- `pathtrace.rgen`: ray generation shader for hardware RT.
- `pathtrace.rchit`: closest-hit shader.
- `pathtrace.rahit`: primary any-hit shader.
- `pathtrace.rmiss`: primary miss shader.
- `pathtrace_shadow.rahit`: shadow any-hit shader.
- `pathtrace_shadow.rmiss`: shadow miss shader.
- `rt_common.glsl`: shared hardware RT shader declarations and helpers.

The compute shader is the largest shader and acts as the main visual reference. The hardware RT shaders share layout and lighting/material logic where possible.

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

## Presentation

Presentation uses a fullscreen dynamic-rendering graphics pass.

`fullscreen.vert` emits a fullscreen triangle.

`fullscreen.frag` samples either the raw or denoised renderer output and applies exposure/debug presentation logic.

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
- Camera speed change.
- Camera reset.
- Shader reload.
- Layout reset.
- Denoiser toggle.
- Debug view toggle.
- Exit.

Current editor integration limits:

- Creating a camera updates the editor scene and active-camera state, but the renderer does not consistently apply that new camera pose/FOV during the normal `CameraOnly` request path. The camera is reliably applied during renderer creation/rebuild paths.
- Creating or editing a light updates the editor scene, but editor light components are not uploaded into GPU light records yet.
- Several Inspector branches for legacy/imported/fallback selections still show placeholder controls or temporary local values. Those controls can move in the UI without changing renderer state.
- The Material Editor panel is controlled by panel visibility, not by selection type. It remains visible unless the user hides that panel, and it shows a disabled message when the current selection has no material.
- Material edits are more connected than light/camera authoring: they update `AssetManager` material data and attempt a GPU material-buffer update through `PathTracerRenderer::updateMaterials`.

## Runtime Controls

The app supports keyboard/mouse controls for:

- Camera movement.
- Mouse look.
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

`RendererDebugView` currently defines 28 debug views:

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

`GpuProfiler` records GPU timings for major passes:

- Path tracing.
- Denoising.
- Fullscreen presentation.

The editor also exposes hardware RT stats:

- Active/inactive status.
- BLAS count.
- Instance count.
- Acceleration-structure memory.
- Shader binding table size.

## Documentation

Existing docs:

- `docs/RESOURCE_SYSTEM.md`: Vulkan resource ownership and barrier rationale.
- `docs/DESCRIPTOR_PIPELINE_SYSTEM.md`: descriptor lifetime, reflection, and pipeline strategy.
- `docs/PATH_TRACER_MIGRATION.md`: GPU scene and path tracer migration details.
- `docs/RENDERER_STABILIZATION.md`: MIS, debug visualization, asset pipeline, bindless groundwork, and validation notes.
- `docs/MIGRATION_ROADMAP.md`: WebGPU-to-Vulkan migration roadmap.
- `HardwareRT_Implementation_plan.md`: detailed hardware RT implementation plan.

## Current Development State

The renderer is functional but not finished. The strongest parts of the architecture are:

- Clear application/runtime ownership.
- RAII resource wrappers.
- Explicit Synchronization2 barriers.
- Shared GPU scene representation.
- Compute and hardware RT backend abstraction.
- Shader reflection-driven descriptor layout construction.
- Editor request flow separated from immediate renderer mutation.
- Rich debug and profiling support.

Main active or future work areas:

- Compute-vs-hardware-RT visual parity.
- Hardware RT acceleration structure updates/refits.
- Completing editor camera propagation so created/selected active cameras immediately drive the renderer.
- Exporting editor light components into GPU light records or another renderer lighting path.
- Replacing placeholder Inspector controls with controls backed by actual scene/renderer state.
- Finer geometry splitting for opaque, alpha-tested, and single-sided traversal paths.
- Rough dielectric/specular sampling and MIS tuning.
- Fully bindless material texture residency.
- Broader glTF material extension support.
- More complete scene instancing and transform update paths.
- Render graph resource-state tracking and barrier validation.
- Denoiser and reprojection validation against the WebGPU reference.

## Important Architectural Takeaway

The project is not just a minimal Vulkan sample. It is a staged renderer/editor migration with a real scene pipeline, GPU scene abstraction, compute path tracing backend, hardware ray tracing backend, denoiser, debug tooling, and editor integration.

The central data flow is:

```text
CLI / editor input
    -> Application
    -> SceneDocument / AssetManager
    -> SceneToGpuSceneBuilder
    -> GpuScene
    -> PathTracerRenderer
    -> compute backend or hardware RT backend
    -> denoiser
    -> fullscreen presentation
    -> ImGui editor overlay
```

This separation lets the project continue improving editor features, scene import, GPU data layout, and backend-specific tracing without rewriting the full application shell.
