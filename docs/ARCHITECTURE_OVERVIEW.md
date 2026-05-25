# Architecture Overview: Native Vulkan Ray Tracing Engine

**Project**: `rtvulkan` — C++20 / Vulkan 1.3 hardware-accelerated path tracing renderer and editor  
**Platform**: Windows (Visual Studio 2022)  
**Source**: 85 C++ files (`src/rtv/`), 94 headers (`include/rtv/`), ~40 shaders, 9 docs

---

## 1. High-Level Data Flow

```
CLI / Editor Input → Application → SceneDocument / AssetManager
  → SceneToGpuSceneBuilder → GpuScene → PathTracerRenderer
    → Hardware RT Backend (primary) or Compute Backend (legacy)
      → ReSTIR Spatial (optional)
        → Height Fog (optional)
          → Atmosphere LUTs (optional)
            → Denoiser (temporal/spatial)
              → TAA (optional)
                → Auto-Exposure (optional)
                  → Tone Map (compute)
                    → Selection Outline (optional)
                      → Fullscreen Present (graphics)
                        → ImGui Editor Overlay
```

---

## 2. System Architecture Layers

```
┌──────────────────────────────────────────────────────────────────────────┐
│                        Application Layer                                 │
│  Application (GLFW window, event loop, frame orchestration)             │
├──────────────────────┬─────────────────────────┬────────────────────────┤
│  Editor Layer        │  Renderer Layer          │  Scene Layer           │
│  EditorLayer         │  PathTracerRenderer      │  SceneDocument         │
│  ├─ ViewportPanel    │  ├─ GpuScene              │  ├─ SceneRegistry      │
│  ├─ InspectorPanel   │  ├─ Shader/Pipeline       │  ├─ SceneComponents    │
│  ├─ SceneHierarchy   │  ├─ RT Backend            │  ├─ SceneOperations    │
│  ├─ AssetBrowser     │  ├─ Compute Backend       │  ├─ SceneEventBus      │
│  ├─ RenderSettings   │  ├─ Denoiser/TAA          │  ├─ SceneUpdateRouter  │
│  ├─ Debug/Profiler   │  ├─ ToneMap/AutoExp       │  └─ UndoStack          │
│  └─ MaterialEditor   │  ├─ Atmosphere/Sky        │                       │
│                      │  ├─ ReSTIR                │                       │
│                      │  └─ TemporalSystem        │                       │
├──────────────────────┴─────────────────────────┴────────────────────────┤
│                       Resource Layer                                    │
│  Buffer, Image, ResourceAllocator (VMA), UploadContext, ImageBarrier   │
├──────────────────────────────────────────────────────────────────────────┤
│                     Descriptor & Pipeline Layer                          │
│  DescriptorLayoutCache, DescriptorAllocator, DescriptorWriter,          │
│  FrameResources (per-frame arenas), ShaderReflection,                   │
│  ComputePipeline, GraphicsPipeline, RayTracingPipeline, PipelineCache  │
├──────────────────────────────────────────────────────────────────────────┤
│                    Vulkan Core Layer                                    │
│  VulkanContext (instance, device, queues, RT/bindless caps)             │
│  Swapchain (images, resize, present mode), CommandSystem (2 in-flight) │
├──────────────────────────────────────────────────────────────────────────┤
│                    Hardware Layer                                       │
│  GLFW ← → Volk (Vulkan Loader) ← → GPU Driver                         │
└──────────────────────────────────────────────────────────────────────────┘
```

---

## 3. Layer Details

### 3.1 Entry Point — `src/main.cpp`

Minimal CLI parser supporting:

| Flag | Description |
|------|-------------|
| `--frames <n>` | Run for N frames (smoke testing) |
| `--debug-view <name>` | Start in a debug view mode |
| `--gltf <path>` | Load glTF/GLB scene |
| `--hdr <path>` | Load HDR environment map |
| `--scene <path>` | Load `.rtlevel` scene |
| `--backend auto\|rt\|compute` | Select renderer backend |
| `--denoiser on\|off` | Override denoiser state |
| `--restir classic\|restir\|hybrid` | Set ReSTIR mode |
| `--validation-camera-motion` | Enable validation camera path |

Constructs `rtv::Application` and calls `run(maxFrames)`.

---

### 3.2 Application Layer — `Application`

**File**: `include/rtv/Application.h`, `src/rtv/Application.cpp`

Top-level runtime owner. Lifecycle:

1. Create GLFW window
2. Initialize Vulkan through `VulkanContext`
3. Create VMA `ResourceAllocator` + upload helpers
4. Create `Swapchain` + `CommandSystem`
5. Load glTF scene or initialize fallback (Cornell box)
6. Convert `SceneDocument` → `GpuScene` via `SceneToGpuSceneBuilder`
7. Create `PathTracerRenderer`
8. Apply active camera
9. Create `UiOverlay` (ImGui)
10. Enter main loop

**Main loop** per frame:

1. Poll GLFW events
2. Compute delta time
3. Begin ImGui frame
4. Process keyboard/mouse runtime controls
5. Build editor panels → produce `EditorRequests`
6. Apply **pre-frame** editor requests (safe, no rebuild needed)
7. `CommandSystem::drawFrame()` — records + submits all GPU work
8. Apply **post-frame** editor requests (may require GPU resource rebuilds)
9. Poll async scene loading
10. Update window title

**Key owned objects**:
- `VulkanContext* context_`
- `ResourceAllocator* allocator_`
- `UploadContext* uploadContext_` + `BufferUploader* uploader_`
- `Swapchain* swapchain_`
- `CommandSystem* commandSystem_`
- `PathTracerRenderer* pathTracer_`
- `UiOverlay* uiOverlay_`
- `SceneDocument sceneDocument_`
- `AssetManager assets_`
- `CameraController cameraController_`
- `UndoStack undoStack_`
- `SceneEventBus sceneEventBus_`
- `NotificationManager notifications_`
- `SceneToGpuSceneBuilder sceneBuilder_`

---

### 3.3 Vulkan Core Layer

#### `VulkanContext` (`include/rtv/VulkanContext.h`)

Owns instance, debug messenger, surface, physical device selection/scoring, logical device, queues.

**Capability discovery**:
- Bindless: `VK_EXT_descriptor_indexing` — queries `maxSampledImages`, `descriptorIndexing`, `runtimeDescriptorArray`, `partiallyBound`, `updateAfterBind`
- Ray tracing: `VK_KHR_acceleration_structure`, `VK_KHR_ray_tracing_pipeline`, `VK_KHR_deferred_host_operations`, `VK_KHR_buffer_device_address`, `VK_KHR_spirv_1_4`, `VK_KHR_shader_float_controls`
- Timeline semaphore support
- Shader execution reordering (SER) support

Exposes `graphicsQueue_`, `presentQueue_`, `computeQueue_` (may be same queue).

#### `Swapchain` (`include/rtv/Swapchain.h`)

Owns `VkSwapchainKHR`, swapchain images/views, format/extent, surface queries, present mode selection, resize recreation.

#### `CommandSystem` (`include/rtv/CommandSystem.h`)

- **2 frames in flight** — per-frame `VkCommandPool`, `VkCommandBuffer`, `VkFence`, `VkSemaphore` (image available)
- Additional `VkSemaphore` per swapchain image (render finished)
- `drawFrame()` orchestrates: wait fence → acquire image → record → submit → present
- Delegates actual rendering to:
  - `PathTracerRenderer::recordPathTrace(commandBuffer)`
  - `PathTracerRenderer::recordFullscreen(commandBuffer, extent)`
  - `UiOverlay` render

---

### 3.4 Resource Layer

#### `ResourceAllocator` (`include/rtv/ResourceAllocator.h`)
Wraps VMA allocator. Stores device/physical device handles.

#### `Buffer` (`include/rtv/Buffer.h`)
RAII wrapper for `VkBuffer` + `VmaAllocation`. Supports:
- GPU-only, upload, readback, persistently mapped
- Optional buffer device address (for hardware RT)
- Descriptor metadata (`VkDescriptorBufferInfo`)
- Writes/flushes/invalidates, resize

#### `Image` (`include/rtv/Image.h`)
RAII wrapper for `VkImage` + `VmaAllocation` + `VkImageView`. Supports:
- Sampled/storage descriptors
- Mipmap generation
- Tracked image layout (for barrier transitions)
- Resize

#### `UploadContext` + `BufferUploader` + `BatchUploader`
Staging upload paths. Fence-backed, transfer-queue compatible. `BufferUploader` handles single uploads; `BatchUploader` handles bulk scene data.

#### `ImageBarrier` (`include/rtv/ImageBarrier.h`)
Synchronization2 helpers — only `vkCmdPipelineBarrier2`, no legacy barriers.

---

### 3.5 Descriptor & Pipeline Layer

#### Descriptor Management

| Component | Role |
|-----------|------|
| `DescriptorLayoutCache` | Caches `VkDescriptorSetLayout` keyed by binding/type/count/stage |
| `DescriptorAllocator` | Resettable descriptor pools (per-frame arenas) |
| `DescriptorSet` | Lightweight wrapper |
| `DescriptorWriter` | Batch writes for buffers, images, samplers, acceleration structures |

**Strategy**: Per-frame descriptor arenas in `FrameResources`. The command system waits the in-flight fence before reusing a frame index, then `FrameResources::beginFrame()` resets descriptor pools. A descriptor set is never reset while a command buffer referencing it executes.

#### Shader Compilation & Reflection

| Component | Role |
|-----------|------|
| `ShaderCompiler` | Wraps `glslangValidator` — recompiles GLSL when sources newer than SPIR-V outputs |
| `ShaderReflection` | SPIRV-Reflect — extracts descriptor bindings (set, binding, type, count, stage) and push constants. Merges per set; throws on incompatible cross-stage bindings |
| `ShaderModule` | Owns compiled SPIR-V `VkShaderModule` |

#### Pipeline Types

| Type | Use |
|------|-----|
| `ComputePipeline` | Path tracing (legacy), denoiser, TAA, tonemap, auto-exposure, atmosphere LUTs, ReSTIR, selection outline, fog |
| `GraphicsPipeline` | Fullscreen dynamic-rendering presentation (no vertex buffers, fullscreen triangle) |
| `RayTracingPipeline` | Hardware RT — raygen + miss + hit groups, shader binding tables |
| `PipelineCache` | Vulkan pipeline cache (memory-only) |

---

### 3.6 Scene & Asset Model

#### `AssetManager` (`include/rtv/AssetManager.h`)
CPU-side asset owner. Stores vectors of:
- `TextureAsset` — metadata, sampler, RGBA8 pixels, residency
- `MaterialAsset` — PBR params (base color, emissive, metallic, roughness, alpha, textures)
- `MeshAsset` — vertices, indices, primitives, per-primitive material handles

Assets referenced by lightweight handles: `TextureAssetHandle`, `MaterialAssetHandle`, `MeshAssetHandle`.

#### `SceneDocument` (`include/rtv/SceneDocument.h`)
Editable scene representation. Owns:
- `SceneRegistry` — ECS with generational `EntityId`
- `Environment` settings (HDR path, intensity, rotation, background)
- `RenderSettings` (local, distinct from renderer)
- Active camera `EntityId`
- Source glTF/HDR paths
- Dirty state + `SceneUpdateKind` tracking

#### Entity Components (`include/rtv/SceneComponents.h`)

| Component | Fields |
|-----------|--------|
| `Transform` | position, rotationEuler, scale, dirty flag |
| `MeshRenderer` | mesh handle, material slots, visibility (visible, castShadow, visibleToCamera) |
| `Light` | type (Directional/Point/Area), color, intensity, size/radius, enabled |
| `Camera` | verticalFOV, near/far plane, active flag |

#### `SceneRegistry` (`include/rtv/SceneRegistry.h`)
Generational entity storage. Entities stored in slots with `uint32_t generation` for handle validation. Supports:
- Create/destroy/rename entities
- Add/remove/query components (Transform, MeshRenderer, Light, Camera)
- UUID-based lookup
- Dirty tracking via `SceneUpdateKind`

#### `SceneOperations` (`include/rtv/SceneOperations.h`)
High-level mutation API with undo support:
- Entity create/duplicate/delete/reparent
- Component add (light, camera, mesh renderer)
- Property set (transform, light, camera, visibility)
- Transform gizmo drag commit
- Sun drag commit

#### `SceneUpdateRouter` (`include/rtv/SceneUpdateRouter.h`)
Maps `SceneUpdateKind` to a `SceneUpdateRoute`:

```cpp
struct SceneUpdateRoute {
    SceneUpdateKind kind;
    SceneUpdateGpuAction action;   // UpdateMaterials, UpdateTransforms, etc.
    AccumulationResetReason resetReason;
    bool requiresGpuSceneBuild;
    bool requiresRendererRebuild;
    bool resetsAccumulation;
};
```

This creates a **declarative routing table** preventing logic duplication.

#### Scene Update Kinds

| Kind | When | GPU Action |
|------|------|------------|
| `MaterialOnly` | Material parameter edit | UpdateMaterials |
| `TransformOnly` | Gizmo drag / property edit | UpdateTransforms |
| `LightOnly` | Light parameters | UpdateLights |
| `EnvironmentOnly` | HDR/sky settings | UpdateEnvironment |
| `CameraOnly` | Camera params | UpdateCamera |
| `VisibilityOnly` | Hide/show entities | UpdateVisibility |
| `TopologyChanged` | Entity create/delete/reparent | RebuildTopology |
| `RendererSettingsOnly` | Settings panel changes | ApplyRendererSettings |

#### `EditorRequests` (`include/rtv/EditorPanels.h`)
Editor → Application communication struct. Contains optional fields for every possible editor operation. `Application::applyEditorRequests()` consumes these at safe frame boundaries (pre-frame for safe changes, post-frame for rebuild-requiring changes).

#### `EditorSelection` (`include/rtv/EditorSelection.h`)
Tracks current selection kind (Object, Material, Asset, Camera, Light) with multi-select support.

---

### 3.7 GPU Scene — `GpuScene`

**File**: `include/rtv/GpuScene.h`, `src/rtv/GpuScene.cpp`

The bridge between CPU scene data and shader-visible GPU buffers. Owns:

#### Buffers

| Buffer | Contents |
|--------|----------|
| `vertices_` | Packed vertex data |
| `indices_` | Triangle index data |
| `bvhNodes_` | CPU BVH4 nodes (compute backend) |
| `triangles_` | Triangle records |
| `materials_` | Packed material params (5 `vec4` per material) |
| `spheres_` | Analytic sphere data |
| `meshRecords_` | Per-mesh metadata (vertex/index offset, BVH offset, flags) |
| `primitiveRecords_` | Per-primitive metadata (index offset, material, flags) |
| `instanceRecords_` | Per-instance transforms + metadata |
| `rtTriangleMaterialIds_` | Per-triangle material index (hardware RT fast lookup) |
| `lightRecords_` | Per-light metadata + parameters |
| `localVertices_` | Local mesh vertex data (for instancing) |
| `localIndices_` | Local mesh index data |
| `instanceBounds_` | AABB per instance |
| `localBvhNodes_` | Per-mesh BVH nodes (compute backend) |
| `localTriangles_` | Per-mesh triangle records |
| `tlasNodes_` | TLAS build output (compute backend) |
| `tlasInstanceIndices_` | TLAS instance index mapping |
| `envRows_` | Environment importance sampling row CDF |
| `envCols_` | Environment importance sampling column CDF |
| `meshParamsBuffer_` | `MeshParamsUniform` (counts + flags) |
| `envParamsBuffer_` | `EnvParamsUniform` (enabled, intensity, rotation) |
| `lightBvhNodes_` | Light BVH for importance sampling |

#### Images + Samplers

- `environmentImage_` + `environmentSampler_` — HDR or procedural sky
- `materialTextureTable_` — `BindlessTextureTable` for material textures
- `materialSampler_` — shared sampler

#### CPU Cached Data

- `meshParams_` / `envParams_` — last uploaded uniforms
- `rayTracingMeshes_` / `rayTracingInstances_` — build inputs for hardware RT
- `instanceRecordCpu_` — CPU copy of instance records for refit

#### Uniform Structs

**`CameraUniform`** (std140, shader binding 1):
- pos, forward, right, up, frameCount, temporalFrameIndex, jitter, FOV, sun params, exposure, bounces, lighting toggles, render controls (ray bias, firefly clamp)

**`MeshParamsUniform`** (std140, shader binding 11):
- vertexCount, triangleCount, bvhNodeCount, materialCount, enabled, sphereCount, primitiveCount, instanceCount, lightCount, meshCount, local counts, TLAS counts

**`EnvParamsUniform`** (std140, shader binding 16):
- enabled, intensity, rotation, width/height, backgroundIntensity, procedural flag, CDF dimensions, invTotalLum

---

### 3.8 Path Tracer Renderer — `PathTracerRenderer`

**File**: `include/rtv/PathTracerRenderer.h`, `src/rtv/PathTracerRenderer.cpp`

The central rendering engine. Owns all GPU resources for rendering:

#### Owned Resources

| Category | Items |
|----------|-------|
| Render targets | `rawImage_`, `denoisedImage_`, `presentationImage_`, `taaImage_`, `taaHistoryImage_`, `historyImage_` |
| Auxiliary buffers | `accumulationBuffer_`, `varianceBuffer_`, `depthNormalBuffer_`, `worldPositionBuffer_`, `previousWorldPositionBuffer_`, `velocityBuffer_`, `entityIdBuffer_` |
| ReSTIR | `restirReservoirBuffer_`, `previousRestirReservoirBuffer_`, `restirSpatialReservoirBuffer_` |
| Uniforms | `cameraBuffer_`, `denoiserParamsBuffer_`, `prevCameraBuffer_`, `debugParamsBuffer_`, `selectionParamsBuffer_`, `histogramBuffer_`, `exposureBuffer_` |
| Pipelines | `denoiserPipeline_`, `taaPipeline_`, `toneMapPipeline_`, `graphicsPipeline_`, `rayTracingPipeline_`, `selectionOutlinePipeline_`, `luminanceHistogramPipeline_`, `exposureReducePipeline_`, `restirSpatialPipeline_`, `fogPipeline_` |
| Shader modules | All ~16 shader objects (RT, denoiser, TAA, tonemap, atmosphere, selection, etc.) |
| Backend | `rayTracingScene_` (BLAS/TLAS), `atmosphereLutSystem_`, `temporalSystem_` |
| Per-frame | `frames_` (vector of `FrameResources`), `profilers_` (vector of `GpuProfiler`) |
| Meta | `layoutCache_`, `pipelineCache_`, `shaderCompiler_`, `validationLog_`, `physicalCamera_` |

#### Frame Pipeline (in order)

1. **`beginFrame(frameIndex, extent)`** — update camera/settings uniforms, transition output images
2. **`recordPathTrace(commandBuffer)`** — dispatch selected backend
3. **Barrier** — path trace outputs → denoiser readable
4. **`recordRestirSpatial(commandBuffer)`** — optional ReSTIR spatial reuse
5. **`recordHeightFog(commandBuffer)`** — optional volumetric fog integration
6. **Atmosphere LUTs** — transmittance, multi-scatter, sky-view, aerial perspective (via `SkySystem`)
7. **`recordDenoiser(commandBuffer)`** — temporal reprojection + a-trous spatial filtering
8. **`recordTaa(commandBuffer)`** — optional temporal anti-aliasing
9. **`copyHistoryResources(commandBuffer)`** — reprojection buffers for next frame
10. **`recordAutoExposure(commandBuffer)`** — optional histogram-based auto-exposure
11. **`recordToneMap(commandBuffer)`** — HDR → SDR with tone mapping
12. **`recordSelectionOutline(commandBuffer)`** — optional selected-instance outline
13. **Barrier** — presentation image → shader readable
14. **`recordFullscreen(commandBuffer, extent)`** — fullscreen triangle draw
15. **`recordEditorPresentationStart/End`** — ImGui overlay

#### Accumulation Reset Reasons (13 triggers)

```
Startup, Resize, CameraMoved, Manual, RenderSettingsChanged,
LightingChanged, EnvironmentChanged, DenoiserSettingsChanged,
DebugViewChanged, SceneChanged, MaterialChanged, ShaderReloaded, BackendChange
```

#### Backend Selection

```cpp
enum Backend { HardwareRayTracing, Auto, Compute };
```

- `HardwareRayTracing` — requires `VK_KHR_acceleration_structure + ray_tracing_pipeline + deferred_host_operations + buffer_device_address + spirv_1_4 + shader_float_controls`
- `Auto` — hardware RT when available, else compute
- `Compute` — always legacy compute

---

### 3.9 Ray Tracing Backend

#### `AccelerationStructure` (`include/rtv/AccelerationStructure.h`)
RAII wrapper for `VkAccelerationStructureKHR` + scratch buffer. Builds from geometry inputs.

#### `RayTracingScene` (`include/rtv/RayTracingScene.h`)
Builds:
- **BLAS**: One per unique mesh from `GpuScene::rayTracingMeshes()`
- **TLAS**: From `GpuScene::rayTracingInstances()`

Supports **TLAS refit** (`refitTransforms()`) for transform-only updates.

#### `RayTracingPipeline` (`include/rtv/RayTracingPipeline.h`)
Builds ray tracing pipeline from:
- 1 raygen group (`pathtrace.rgen`)
- 1 primary miss group (`pathtrace.rmiss`)
- 1 shadow miss group (`pathtrace_shadow.rmiss`)
- 1 primary hit group (`pathtrace.rchit` + `pathtrace.rahit`)
- 1 shadow hit group (`pathtrace_shadow.rahit`)

Shader binding tables for each group. Pipeline recursion depth = 1 (raygen owns multi-bounce via explicit `traceRayEXT` calls).

#### Hardware RT Shader Architecture

```
pathtrace.rgen (813 lines)
  ├── Primary ray generation (camera rays)
  ├── Multi-bounce path loop
  │   ├── Trace primary ray → closest hit / any hit / miss
  │   ├── Material decode + texture evaluation (after hit)
  │   ├── Direct lighting (Next Event Estimation)
  │   │   ├── Light BVH sampling
  │   │   ├── Environment importance sampling
  │   │   └── Analytical sun
  │   ├── BSDF sampling → next bounce ray
  │   ├── MIS weight computation
  │   └── ReSTIR reservoir update
  └── Output buffer writes (accumulation, variance, depth/normal, world position, velocity, entity ID)
      └── Shadow rays → pathtrace_shadow.rahit / pathtrace_shadow.rmiss

pathtrace.rchit (75 lines)
  └── Barycentric interpolation → payload: material ID, world pos, normal, UV, tangent, IDs

pathtrace.rahit (60 lines)
  └── Alpha testing: opaque (pass), mask (compare), blend (stochastic)

pathtrace_shadow.rahit (58 lines)
  └── Early terminate on opaque, alpha-masked shadow check

pathtrace.rmiss (22 lines) / pathtrace_shadow.rmiss (9 lines)
  └── Default payload / shadow_occluded = 0
```

---

### 3.10 Compute Backend (Legacy)

**Shader**: `shaders/pathtrace.comp`

Uses CPU-built BVH4:
- `BvhBuilder` — Morton-code-based construction
- `SahBuilder` — binned SAH optimization
- `BvhCollapse` — BVH4 packing
- `ParallelBvhBuilder` — multi-threaded
- `MortonCodes` — 30-bit Morton code generation

Traversal via storage buffers (no acceleration structures). Maintained for reference and comparison but **not** active development target.

---

### 3.11 Atmosphere & Sky System

#### Architecture: 4 Subsystems + Orchestrator

```
PathTracerRenderer
  └── SkySystem (thin orchestrator)
       ├── AtmosphereModel           — pure math, no GPU (phase functions, transmittance computation)
       ├── AtmosphereLutSystem       — GPU LUT generation + resource ownership (set 1)
       ├── AtmosphereTemporalSystem  — DirtyDAG scheduling, reprojection, history
       └── AtmosphereSamplingSystem  — CDF construction, MIS, environment sampling
```

#### Single Descriptor Set 1

All atmosphere resources in one set:

| Binding | Resource |
|---------|----------|
| 0 | Atmosphere UBO (std140) |
| 1 | Transmittance LUT (2D texture) |
| 2 | Multi-scatter LUT (2D texture) |
| 3 | Sky-view LUT (2D texture) |
| 4 | Aerial perspective LUT (3D texture) |
| 5 | Sampler |
| 6 | Sky CDF rows buffer |
| 7 | Sky CDF columns buffer |

#### LUT Specifications

| LUT | Format | Resolution | VRAM | Update |
|-----|--------|------------|------|--------|
| Transmittance | RGBA16F | 256×64 | 128 KB | On parameter change |
| Multi-scatter | RGBA16F | 32×32 | 8 KB | On parameter change |
| Sky-view | RGBA16F | 256×144 | 288 KB | Every frame (temporal reproject) |
| Aerial perspective | RGBA16F | 96×96×48 | 2.6 MB | On parameter change |

#### Sun Policy
- Sun is **purely analytical** — NOT in sky LUT or CDF
- Miss shader: `sample_sky_view_lut(dir) + analytical_sun_disk_radiance(dir)`
- Direct lighting: `analytical_sun_center_radiance() * sun_transmittance(hitPos)`
- Importance CDF built from diffuse sky LUT only (no sun component)

#### DirtyDAG (AtmosphereTemporalSystem)
Dependency graph for LUT update scheduling:
```
Rayleigh|Mie|Ozone → Transmittance LUT + Multi-scatter LUT
  → SunDirection|Scatter|Quality → Sky-view LUT
    → CDF rebuild
```

---

### 3.12 Shaders Complete Inventory

#### Hardware RT (6 files)

| File | Lines | Purpose |
|------|-------|---------|
| `pathtrace.rgen` | 813 | Raygen — multi-bounce path loop, lighting, MIS, ReSTIR |
| `pathtrace.rchit` | 75 | Closest-hit — barycentric interpolation, payload packing |
| `pathtrace.rahit` | 60 | Any-hit — alpha testing (opaque/mask/blend) |
| `pathtrace.rmiss` | 22 | Miss — default payload |
| `pathtrace_shadow.rahit` | 58 | Shadow any-hit — early termination, alpha mask |
| `pathtrace_shadow.rmiss` | 9 | Shadow miss — occluded=0 |

#### GLSL Includes (10 files)

| File | Lines | Purpose |
|------|-------|---------|
| `rt_common.glsl` | 1208 | Shared declarations, material decode, BSDF, env sampling, structs |
| `atmosphere_phase.glsl` | 93 | Rayleigh/Mie phase functions, ray-sphere intersection |
| `atmosphere_lighting.glsl` | 183 | Analytical sun, sun disk, horizon visibility |
| `atmosphere_lut_sampling.glsl` | 61 | LUT sampling accessors |
| `environment_sampling.glsl` | 57 | Environment PDF + importance sampling |
| `temporal_common.glsl` | 89 | Velocity unpacking, reprojection, confidence |
| `restir_common.glsl` | 24 | ReSTIR reservoir struct |
| `wavefront_common.glsl` | 32 | Wavefront path tracing structs |
| `blue_noise.glsl` | 35 | Interleaved gradient noise |
| `atmosphere_common.glsl` | 8 | Atmosphere include wrapper |

#### Compute Shaders (20 files)

| Shader | Purpose |
|--------|---------|
| `denoiser.comp` | Spatio-temporal denoiser (a-trous wavelet) |
| `taa.comp` | Temporal anti-aliasing |
| `tone_map.comp` | Tone mapping (6 mappers: ACES, AgX, Reinhard, etc.) |
| `luminance_histogram.comp` | Auto-exposure histogram builder |
| `exposure_reduce.comp` | Auto-exposure percentile reducer/adaptation |
| `fog_integrate.comp` | Volumetric fog integration |
| `selection_outline.comp` | Selection outline overlay |
| `transmittance_lut.comp` | Atmosphere transmittance LUT |
| `multi_scatter_lut.comp` | Atmosphere multi-scattering LUT |
| `sky_view_lut.comp` | Sky dome LUT generation |
| `sky_cdf.comp` | Sky importance CDF construction |
| `sky_reproject.comp` | Sky temporal reprojection |
| `sky_debug_views.comp` | Atmosphere debug views |
| `aerial_perspective_lut.comp` | Aerial perspective 3D LUT |
| `restir_init.comp` | ReSTIR initial sampling |
| `restir_ris_init.comp` | ReSTIR reservoir importance sampling |
| `restir_temporal.comp` | ReSTIR temporal reuse |
| `restir_spatial.comp` | ReSTIR spatial reuse |
| `restir_final.comp` | ReSTIR final resolve |
| `demo_compute.comp` | Compute pipeline demo |

#### Fullscreen (2 files)

| Shader | Lines | Purpose |
|--------|-------|---------|
| `fullscreen.vert` | 15 | Fullscreen triangle |
| `fullscreen.frag` | 12 | Final swapchain write |

---

### 3.13 Editor & UI

**Framework**: ImGui (docking branch) + ImGuizmo

#### Panels

| Panel | File | Function |
|-------|------|----------|
| Viewport | `ViewportPanel.cpp` | Rendered image display, mouse capture, pick query |
| Scene Hierarchy | `SceneHierarchyPanel.cpp` | Entity tree, selection, visibility/lock toggles, right-click menu |
| Inspector | `InspectorPanel.cpp` | Transform gizmo, camera params, light params, mesh renderer, material slot edit |
| Asset Browser | `AssetBrowserPanel.cpp` | File-system browsing, thumbnail generation, drag-drop |
| Material Editor | `MaterialEditorPanel.cpp` | PBR material parameter editing (hidden by default) |
| Render Settings | `RenderSettingsPanel.cpp` | Tone mapping, exposure, denoising, TAA, RT backend, sun/sky/env controls |
| Debug/Profiler | `DebugProfilerPanel.cpp` | Debug view selection, GPU timings, pipeline stats, RT stats |
| Scene Stats | `SceneStatsPanel.cpp` | Entity counts, memory stats |
| GPU Diagnostics | `GpuDiagnosticsPanel.cpp` | Hardware RT BLAS/instance/SBT info |

#### Editor Request Pattern

Editor panels produce `EditorRequests` — a struct with optional fields for every operation. `Application` applies them in two stages:

**Pre-frame** (safe):
- Renderer settings changes
- Accumulation reset
- Denoiser toggle
- Debug view change
- Camera speed

**Post-frame** (may require rebuild):
- Scene topology changes (create/delete/reparent entities)
- Material updates
- Light/camera edits
- Transform commits
- glTF/HDR file loads
- Shader reload
- Undo/redo

#### Undo/Redo

`UndoStack` + `SceneOperations` provide undoable:
- Entity create/delete/duplicate/reparent
- Component add (light, camera, mesh renderer)
- Property changes (transform, light, camera, visibility)
- Transform gizmo commit
- Sun drag commit

#### EditorPreferences

Persisted via JSON:
- Recent files (Sponza.gltf paths)
- Camera speeds
- Grid/HUD visibility
- Panel visibility toggles

#### CameraBookmarkManager

Save/load camera positions by name. Bookmarks persist in scene JSON.

---

### 3.14 Temporal System

**File**: `include/rtv/TemporalSystem.h`, `src/rtv/TemporalSystem.cpp`

Manages temporal history for denoising and TAA:

- History slots with name, format, extent, residency policy
- Camera cut detection
- Memory budgeting (evictable slots)
- Reprojection: UV + velocity → previous UV
- Confidence evaluation: luminance variance, depth disocclusion, normal disocclusion
- History clamping in YCoCg space
- Reactive mask generation

Separate `frameCount_` (accumulation) and `temporalFrameIndex_` (TAA/denoiser) prevent temporal history resets on every camera movement.

---

### 3.15 RendererDebug

**File**: `include/rtv/RendererDebug.h`, `src/rtv/RendererDebug.cpp`

38 parse-compatible debug views:

| Category | Views |
|----------|-------|
| Core | Beauty, Variance, Normals, Depth, Roughness, Albedo, Motion Vectors |
| Lighting | Direct, Indirect, Emissive, Environment, First-bounce throughput, Secondary env radiance |
| PDF/MIS | Light PDF, BSDF PDF, MIS Weight, Direct Sample Type |
| Traversal | Instance ID, Mesh ID, TLAS Steps, BVH Depth, Traversal Mismatch, Bounce Count |
| Temporal | Reprojection Confidence, Denoiser Rejection, Reactive Mask, History Weight |
| Atmosphere | Sky View, Transmittance, Aerial Perspective, Multi-scatter |
| ReSTIR | Reservoir Age, Reservoir Confidence, Reservoir M |
| Other | Clay Material, Secondary Environment Miss, White Environment Transport |

Selected via `--debug-view` CLI, F1 cycling, or Render Settings panel.

---

### 3.16 GpuProfiler

**File**: `include/rtv/GpuProfiler.h`, `src/rtv/GpuProfiler.cpp`

22 timestamp queries per frame (11 start/end pairs):

```
PathTrace, RestirSpatial, FogIntegrate, Atmosphere, Denoiser,
HistoryCopy, Taa, AutoExposure, ToneMap, SelectionOutline, Fullscreen
```

Also supports `VK_QUERY_TYPE_PIPELINE_STATISTICS` (ray invocations, triangle hits, AABB hits).

---

### 3.17 GpuValidation

**File**: `include/rtv/GpuValidation.h`, `src/rtv/GpuValidation.cpp`

`RendererValidationLog` records:
- Barrier events (label, src/dst stage/access)
- Resource state transitions (image name, before/after layout, stage, access)
- Accumulation invalidations (reason, frame)
- Scene update route events (kind, action)
- Pass names

Inspected via GpuDiagnosticsPanel for debugging barrier correctness.

---

## 4. Build System

### CMakeLists.txt

- **CMake** 3.27+, **C++20**
- Single target: `rtvulkan` executable
- 85 source files + 6 ImGui files

### Dependencies

| Dependency | Source | Role |
|-----------|--------|------|
| Vulkan SDK 1.4.350+ | System | Core Vulkan + glslangValidator |
| Volk | Bundled in SDK | Vulkan loader (no prototypes) |
| GLFW | vcpkg | Window + surface + input |
| GLM | vcpkg | Mathematics |
| VMA | vcpkg | GPU memory allocation |
| stb | vcpkg | Image loading |
| tinygltf | Header include | glTF/GLB loading |
| nlohmann_json | vcpkg | JSON serialization |
| SPIRV-Reflect | vcpkg | Shader reflection |
| ImGuizmo | vcpkg | 3D transform gizmo |
| KTX | vcpkg | Texture loading |
| ImGui (docking) | Local source tree | Editor UI |

### Compile Defines

```
VK_NO_PROTOTYPES, GLFW_INCLUDE_NONE, NOMINMAX,
IMGUI_IMPL_VULKAN_USE_VOLK, IMGUI_IMPL_VULKAN_VOLK_FILENAME=<Volk/volk.h>
```

### Shader Compilation

`ShaderCompiler` runs `glslangValidator` at startup, recompiling only when GLSL sources are newer than generated SPIR-V.

---

## 5. Architecture Strengths

| Strength | Evidence |
|----------|----------|
| Clean separation of concerns | Editor requests vs immediate mutation, scene doc vs GPU scene, path tracer vs backend |
| Indirection at every boundary | `SceneUpdateRouter`, `EditorRequests`, `SceneToGpuSceneBuilder` all decouple producer from consumer |
| RAII resource wrappers | `Buffer`, `Image`, `AccelerationStructure` own their Vulkan handles with proper destruction |
| Explicit synchronization | Only Synchronization2 barriers, no legacy `vkCmdPipelineBarrier`, tracked image layouts |
| Shader reflection validation | SPIRV-Reflect cross-validates bindings across stages before pipeline creation |
| Progressive accumulation with separate temporal tracking | Decouples path tracing convergence from temporal filtering stability |
| Comprehensive debug/profiling | 38 debug views, GPU timestamps, pipeline statistics, validation logging |
| Editor request throttling | Pre-frame/post-frame split prevents GPU race conditions |
| Two-descriptor-set architecture | Only 2 descriptor set switches per frame (scene + atmosphere) |
| Deferred mutation | Editor produces request structs, application applies at safe points |

---

## 6. Architecture Weaknesses & Risks

| Weakness | Impact | Location |
|----------|--------|----------|
| Monolithic `rt_common.glsl` (1208 lines) | Every change recompiles all RT shaders; hard to reason about dependencies | `shaders/rt_common.glsl` |
| Monolithic `PathTracerRenderer` | Single class owns all rendering state; violates single responsibility | `include/rtv/PathTracerRenderer.h` |
| No render graph fully deployed | Barriers are manually placed; no automatic state tracking | `RenderGraph.cpp` exists but is nascent |
| Single graphics queue | No async compute; atmosphere LUT gen blocks path tracing | `VulkanContext.h` |
| Hardcoded ImGui path | CMake requires local ImGui source directory; fragile portability | `CMakeLists.txt:29-40` |
| No incremental SPIR-V cache | Shaders compile at every launch | `ShaderCompiler` |
| Manual accumulation reset (13 points) | Must keep in sync with every state change | `PathTracerRenderer.h:51-64` |
| Float precision for planetary coordinates | Risk of precision artifacts at 6,360,000m scale | `atmosphere_phase.glsl` |
| No full bindless textures | Material textures use fixed array with `nonuniformEXT` | `rt_common.glsl:109` |
| Editor tied to GLFW | No headless/remote rendering | `Application.h` owns `GLFWwindow*` |

---

## 7. Code Quality Metrics

| Metric | Value |
|--------|-------|
| C++ source files | 85 (84 `src/rtv/` + `src/main.cpp`) |
| C++ header files | 94 (`include/rtv/`) |
| Shader source files | 40 |
| Documentation | 9 `.md` files (~7,000+ lines) |
| Validation scenes | 5 `.rtlevel` + manifest |
| GPU profiler queries | 22 per frame |
| Debug view modes | 38 parse-compatible, ~34 exposed |
| Tone mappers | 6 (ACES, AgX, PBR Neutral, Reinhard, Reinhard White, Linear) |
| CLI options | 8 |

---

## 8. Source File Dependency Graph

```
main.cpp
  └── Application
        ├── VulkanContext → GLFW window
        ├── Swapchain
        ├── CommandSystem → PathTracerRenderer, UiOverlay
        ├── ResourceAllocator (VMA)
        │     ├── Buffer, Image, UploadContext, BufferUploader
        │     └── DescriptorLayoutCache, DescriptorAllocator
        ├── SceneDocument
        │     ├── SceneRegistry → Entity, EntityId
        │     ├── SceneComponents (Transform, MeshRenderer, Light, Camera)
        │     └── SceneToGpuSceneBuilder → GpuScene
        ├── AssetManager
        │     ├── GltfLoader
        │     ├── TextureLoader, HdrLoader
        │     └── MeshAsset, TextureAsset, MaterialAsset
        └── PathTracerRenderer
              ├── GpuScene (scene GPU buffers)
              ├── RayTracingScene (BLAS/TLAS)
              ├── RayTracingPipeline (SBT)
              ├── ComputePipeline (denoiser, TAA, tonemap, etc.)
              ├── GraphicsPipeline (fullscreen)
              ├── SkySystem
              │     ├── AtmosphereModel (pure math)
              │     ├── AtmosphereLutSystem (GPU LUT gen)
              │     ├── AtmosphereTemporalSystem (DirtyDAG)
              │     └── AtmosphereSamplingSystem (CDF)
              ├── TemporalSystem (history management)
              └── FrameResources (per-frame arenas)
```

---

## 9. Project File Inventory

| Directory | Contents |
|-----------|----------|
| `src/main.cpp` | Entry point, CLI parsing |
| `src/rtv/` | 84 implementation files |
| `include/rtv/` | 94 header files |
| `shaders/` | 40 shader files (6 RT, 20 compute, 10 GLSL includes, 2 fullscreen, 2 SPIR-V) |
| `docs/` | 9 markdown documentation files |
| `scenes/validation/` | 5 `.rtlevel` validation scenes + manifest |
| `Sponza/` | Crytek Sponza Palace (glTF, 68 textures) |
| `main_sponza/` | New Sponza (glTF, USD, FBX, 137 textures) |
| Root | `citrus_orchard_road_puresky_4k.hdr` (HDR env map) |

---

## 10. Key Areas for Future Work

| Area | Priority | Details |
|------|----------|---------|
| RT acceleration structure refit | High | `RayTracingScene::refitTransforms()` exists but editor transform workflows need hardening |
| Bindless textures | High | `BindlessResources` + `BindlessTextureTable` exist but material textures still use fixed array |
| Render graph | Medium | `RenderGraph.h/cpp` exists but not fully integrated for automatic barrier management |
| Denoiser/TAA validation | Medium | Temporal reprojection needs validation against hardware RT output |
| Scene instancing | Medium | Instance transform update paths need hardening |
| glTF material coverage | Low | Extensions (clearcoat, sheen, transmission) not fully supported |
| Async compute | Low | Atmosphere LUT generation could run on dedicated compute queue |
| Editor camera workflows | Medium | Active camera propagation and piloting need hardening |
| Separate opaque/alpha geometry | Medium | Geometry type splitting for optimized hit groups |
| Portable build | Low | Hardcoded vcpkg/VulkanSDK/ImGui paths need cleanup |
