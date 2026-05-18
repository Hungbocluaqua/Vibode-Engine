# Detailed Plan: Fully Implement Vulkan Hardware Ray Tracing

## Objective

Implement an optional Vulkan hardware ray tracing backend for the native Vulkan renderer. The new backend must use `VK_KHR_acceleration_structure` and `VK_KHR_ray_tracing_pipeline` for mesh traversal, preserve the existing compute BVH renderer as a fallback/reference, reuse the existing denoiser and fullscreen presentation path, and expose backend selection through CLI and UI.

The final result should support:

- `--backend compute`: existing compute BVH renderer.
- `--backend rt`: hardware ray tracing renderer, with a clear error if unavailable.
- `--backend auto`: hardware RT when available, compute otherwise.
- Existing glTF/HDR loading, material texture sampling, emissive/environment/direct lighting, debug views where applicable, accumulation, denoising, and editor integration.

## Non-Goals For The First Complete Version

- Do not remove or rewrite the compute renderer.
- Do not require hardware RT for startup unless explicitly requested by `--backend rt`.
- Do not implement AS compaction as a blocker; add it only after baseline RT rendering is correct.
- Do not implement advanced animated mesh deformation; static mesh BLAS plus TLAS rebuild/refit is enough initially.
- Do not replace the denoiser or presentation pass.
- Do not introduce a render graph rewrite as part of this work.

## Current Repository Context

### Vulkan Setup

- `VulkanContext` currently enables Vulkan 1.3, dynamic rendering, synchronization2, descriptor indexing as available, and swapchain.
- Required device extensions currently only include `VK_KHR_swapchain`.
- Physical-device scoring does not query ray tracing support.
- VMA allocator is created without `VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT`.
- Volk is already used, so KHR function pointers should be available after enabling extensions and calling `volkLoadDevice`.

### Renderer Flow

- `PathTracerRenderer::recordPathTrace()` currently:
  - transitions `rawImage_` to `VK_IMAGE_LAYOUT_GENERAL`.
  - allocates/writes one compute descriptor set.
  - binds `pathTracePipeline_`.
  - dispatches `shaders/pathtrace.comp`.
  - runs denoiser/history copy or skips denoiser.
- `recordDenoiser()` and fullscreen presentation are backend-independent and should remain shared.
- `GpuProfiler` names the path-trace timing generically enough to reuse initially.

### Scene Data

- `GpuScene` already uploads:
  - `GpuLocalVertex` local mesh vertices.
  - local mesh indices.
  - `GpuMeshRecord` records.
  - `GpuPrimitiveRecord` records.
  - `GpuInstanceRecord` records.
  - material data buffer.
  - texture arrays/samplers.
  - light records.
  - environment image/CDF buffers.
  - CPU-built BVHs/TLAS for compute traversal.
- Imported scenes use triangle meshes and instances.
- Fallback Cornell scene includes triangle walls/light plus three analytic spheres stored in `scene_spheres`; hardware triangle AS cannot intersect those spheres directly.

### Shader Data Layout

- `pathtrace.comp` uses set 0 bindings 0-32.
- Materials are packed as 5 `vec4` values per material.
- Textures are split sampled image array at binding 19 and sampler array at binding 23.
- Debug/auxiliary outputs are already encoded for denoiser/fullscreen compatibility.

## Target Architecture

### Backend Model

Add a backend enum that represents the requested backend and the active backend separately:

```cpp
enum class RendererBackend : uint32_t {
    Auto,
    Compute,
    HardwareRayTracing,
};
```

Recommended terms:

- `requestedBackend`: value from CLI/UI/settings.
- `activeBackend`: resolved backend after checking hardware support.

Resolution rules:

| Requested | RT Supported | Result |
| --- | --- | --- |
| Compute | either | Compute |
| HardwareRayTracing | yes | HardwareRayTracing |
| HardwareRayTracing | no | fatal error / UI rejected |
| Auto | yes | HardwareRayTracing |
| Auto | no | Compute |

### Renderer Split

Keep `PathTracerRenderer` as the owner/orchestrator and add backend-specific helpers:

- Existing compute path remains in `PathTracerRenderer`.
- New `RayTracingScene` owns BLAS/TLAS resources.
- New `RayTracingPipeline` owns RT pipeline and SBT.
- New `AccelerationStructure` owns one Vulkan AS handle plus backing buffer.

High-level frame flow after implementation:

1. `beginFrame()` updates frame resources, resolution resources, camera buffers.
2. `recordPathTrace()` performs shared prologue.
3. If active backend is compute, dispatch `pathtrace.comp` exactly as today.
4. If active backend is RT, bind RT pipeline/descriptors/SBT and call `vkCmdTraceRaysKHR`.
5. Shared denoiser/history/presentation continues unchanged.

## Phase 0: Baseline Safety And Acceptance Criteria

### Tasks

- Build and run current compute renderer before invasive changes when implementation begins.
- Capture current smoke commands and output expectations.
- Treat the compute renderer as the regression reference after every major phase.

### Baseline Checks

- `cmake --build native/vulkan/build --config Debug`
- `native\vulkan\build\Debug\rtvulkan.exe --frames 6 --backend compute` after CLI support exists.
- Before CLI support exists, use existing `--frames 6` smoke test.

### Done When

- Current compute backend still builds and renders after each phase.
- Non-RT hardware can still launch in compute/auto mode.

## Phase 1: Capability Discovery And Backend Selection

### Files

- `include/rtv/VulkanContext.h`
- `src/rtv/VulkanContext.cpp`
- `include/rtv/PathTracerRenderer.h`
- `include/rtv/SceneComponents.h`
- `src/rtv/Application.cpp`
- `include/rtv/Application.h`
- `src/main.cpp`
- `src/rtv/RenderSettingsPanel.cpp`
- `README.md` later after functionality is real

### Add RT Capability Types

Add structures similar to:

```cpp
struct RayTracingCapabilities {
    bool accelerationStructure = false;
    bool rayTracingPipeline = false;
    bool bufferDeviceAddress = false;
    bool deferredHostOperations = false;
    bool spirv14 = false;
    bool shaderFloatControls = false;
    bool supported = false;
    std::vector<std::string> missing;
};

struct RayTracingDeviceInfo {
    RayTracingCapabilities capabilities;
    VkPhysicalDeviceAccelerationStructurePropertiesKHR accelerationStructureProperties{};
    VkPhysicalDeviceRayTracingPipelinePropertiesKHR rayTracingPipelineProperties{};
};
```

Expose accessors from `VulkanContext`:

- `const RayTracingDeviceInfo& rayTracingInfo() const`
- `bool supportsHardwareRayTracing() const`

### Extension Querying

Refactor device extension support into two lists:

- Always required:
  - `VK_KHR_SWAPCHAIN_EXTENSION_NAME`
- Optional RT set:
  - `VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME`
  - `VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME`
  - `VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME`
  - `VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME`
  - `VK_KHR_SPIRV_1_4_EXTENSION_NAME`
  - `VK_KHR_SHADER_FLOAT_CONTROLS_EXTENSION_NAME`

Implementation detail:

- Keep `deviceSupportsRequiredExtensions()` checking only swapchain.
- Add `queryRayTracingCapabilities(VkPhysicalDevice)` to report optional RT support and missing names.
- Add RT extensions to `VkDeviceCreateInfo` only if all RT requirements are supported.

### Feature Querying

Use a `vkGetPhysicalDeviceFeatures2` chain:

```cpp
VkPhysicalDeviceAccelerationStructureFeaturesKHR asFeatures{};
VkPhysicalDeviceRayTracingPipelineFeaturesKHR rtFeatures{};
VkPhysicalDeviceBufferDeviceAddressFeatures bdaFeatures{};
VkPhysicalDeviceDescriptorIndexingFeatures descriptorIndexing{};
VkPhysicalDeviceVulkan13Features features13{};
```

Required true values for RT support:

- `asFeatures.accelerationStructure == VK_TRUE`
- `rtFeatures.rayTracingPipeline == VK_TRUE`
- `bdaFeatures.bufferDeviceAddress == VK_TRUE`

Optional features to consider enabling only if supported:

- `rtFeatures.rayTraversalPrimitiveCulling`
- descriptor indexing features already used by material textures

### Property Querying

Use a `vkGetPhysicalDeviceProperties2` chain:

```cpp
VkPhysicalDeviceAccelerationStructurePropertiesKHR asProps{};
VkPhysicalDeviceRayTracingPipelinePropertiesKHR rtProps{};
```

Store at least:

- `maxRayRecursionDepth`
- `shaderGroupHandleSize`
- `shaderGroupHandleAlignment`
- `shaderGroupBaseAlignment`
- `maxShaderGroupStride`
- `minAccelerationStructureScratchOffsetAlignment`

### Device Creation Chain

Current chain is:

`VkPhysicalDeviceFeatures2 -> VkPhysicalDeviceDescriptorIndexingFeatures -> VkPhysicalDeviceVulkan13Features`

Update to append RT feature structs only when RT is supported:

`Features2 -> DescriptorIndexing -> BufferDeviceAddress -> AccelerationStructure -> RayTracingPipeline -> Vulkan13`

or another valid pNext order. Ensure all structs have stable lifetime until `vkCreateDevice` returns.

### Backend Settings

Add `RendererBackend requestedBackend = RendererBackend::Auto;` to:

- `RendererSettings`
- `RenderSettings` in `SceneComponents.h`

Add helpers:

- `RendererBackend parseRendererBackend(std::string_view)`
- `const char* rendererBackendName(RendererBackend)`
- possibly in a new `RendererBackend.h/.cpp` or in `RendererDebug.*` if keeping scope small.

### CLI

Update `main.cpp` parsing:

- `--backend compute`
- `--backend rt`
- `--backend hardware-ray-tracing`
- `--backend auto`

Pass requested backend into `Application` constructor.

Update `Application` fields/constructor:

- store requested backend.
- initialize startup settings with requested backend.

### UI

In `RenderSettingsPanel`:

- Add combo/dropdown under Rendering:
  - Auto
  - Compute
  - Hardware Ray Tracing
- Show a read-only line for active backend and RT availability later once renderer exposes it.
- If user selects RT on unsupported hardware, either:
  - reject and restore prior value with an ImGui text warning, or
  - allow requested setting but renderer resolves/fails safely.

Preferred UI behavior:

- In interactive UI, do not crash if RT unavailable. Disable the RT option or show `Unavailable`.
- CLI `--backend rt` should fail clearly because it is explicit.

### Startup Logs

Log once after device creation:

- Vulkan device name.
- Hardware RT: available/unavailable.
- Missing RT requirements if unavailable.
- Key RT limits if available.

Example:

```text
Hardware ray tracing: available (max recursion depth 31, group handle size 32, group base alignment 64)
```

or:

```text
Hardware ray tracing: unavailable (missing VK_KHR_ray_tracing_pipeline, bufferDeviceAddress feature)
```

### Done When

- Backend settings propagate from CLI and UI to renderer settings.
- Auto/compute startup behaves exactly like current renderer.
- Logs accurately report RT support without requiring RT-capable hardware.
- No RT-specific objects are created yet.

## Phase 2: Buffer Device Address And Resource Usage

### Files

- `include/rtv/ResourceAllocator.h`
- `src/rtv/ResourceAllocator.cpp`
- `include/rtv/Buffer.h`
- `src/rtv/Buffer.cpp`
- `src/rtv/GpuScene.cpp`
- `include/rtv/GpuScene.h`
- `src/rtv/BatchUploader.cpp` if alignment improvements are needed

### VMA Allocator

When `context.supportsBufferDeviceAddress()` or `context.supportsHardwareRayTracing()` is true, create VMA with:

```cpp
createInfo.flags |= VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT;
```

This requires passing either the context or a boolean into `ResourceAllocator` and retaining whether BDA is available.

### Buffer Device Address API

Add to `Buffer`:

```cpp
[[nodiscard]] VkDeviceAddress deviceAddress() const;
[[nodiscard]] bool supportsDeviceAddress() const;
```

Implementation:

- Check `buffer_ != VK_NULL_HANDLE`.
- Check `(desc_.usage & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT) != 0`.
- Use `VkBufferDeviceAddressInfo` and `vkGetBufferDeviceAddress`.
- Throw with a clear error if requested for a buffer without the usage bit.

### Required Usage Flags

Add `VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT` to buffers used by RT shaders or AS builds.

For existing uploaded scene buffers, add the flag conditionally only when RT is active/supported, or always if BDA is enabled. Minimal safe approach:

- Add a helper in `GpuScene` upload path:

```cpp
VkBufferUsageFlags sceneStorageUsage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
if (allocator.supportsDeviceAddress()) {
    sceneStorageUsage |= VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
}
```

- Use this for:
  - `localVertices_`
  - `localIndices_`
  - `meshRecords_`
  - `primitiveRecords_`
  - `instanceRecords_`
  - `materials_`
  - `lightRecords_`
  - environment CDF buffers if accessed by device address in RT shaders (can remain descriptors initially)

For AS build inputs:

- vertex/index buffers need:
  - `VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR`
  - `VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT`
  - current storage/transfer bits as needed

For AS storage buffers:

- `VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR`
- `VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT` optional but useful

For scratch buffers:

- `VK_BUFFER_USAGE_STORAGE_BUFFER_BIT`
- `VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT`

For SBT buffers:

- `VK_BUFFER_USAGE_SHADER_BINDING_TABLE_BIT_KHR`
- `VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT`
- `VK_BUFFER_USAGE_TRANSFER_DST_BIT`

### Alignment Helpers

Add a generic helper if not already enough:

- `Buffer::alignUp(value, alignment)` exists; use it for scratch and SBT.
- Scratch buffer size can be exact, but scratch device address must satisfy `minAccelerationStructureScratchOffsetAlignment`; VMA normally aligns allocations enough, but validate or over-allocate if needed.

### Done When

- Any buffer created for RT can return a valid device address.
- Existing compute renderer still reads all buffers through descriptors.
- No validation warnings from adding BDA usage.

## Phase 3: Descriptor And Utility Support For Acceleration Structures

### Files

- `include/rtv/DescriptorWriter.h`
- `src/rtv/DescriptorWriter.cpp`
- `include/rtv/DescriptorAllocator.h`
- `src/rtv/DescriptorAllocator.cpp`
- `include/rtv/DescriptorLayoutCache.h` if manual layouts need helper additions

### Descriptor Pool

Add pool size:

```cpp
{VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, setsPerPool_}
```

Keep `VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT` only if still needed by bindless texture descriptors. It is already enabled globally.

### Descriptor Writer

Add method:

```cpp
DescriptorWriter& writeAccelerationStructure(uint32_t binding, VkAccelerationStructureKHR accelerationStructure);
```

Important lifetime rule:

- `VkWriteDescriptorSetAccelerationStructureKHR` must remain alive until `vkUpdateDescriptorSets` returns.
- `pAccelerationStructures` must point to stable storage.

Recommended implementation:

- Add vectors:
  - `std::vector<VkAccelerationStructureKHR> accelerationStructures_;`
  - `std::vector<VkWriteDescriptorSetAccelerationStructureKHR> accelerationStructureInfos_;`
- Pending write records need either an enum kind or pNext index.
- In `update()`, set `write.pNext = &accelerationStructureInfos_[index]` and `descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR`.

### Manual RT Layout Option

Reflection may or may not handle acceleration structure descriptors. Be ready to manually create RT layout bindings:

- Either add a helper that builds `std::vector<VkDescriptorSetLayoutBinding>` for the RT pipeline.
- Or use reflection for non-AS shaders and append the AS binding manually.

Manual layout is preferred for first RT implementation because it avoids reflection surprises.

### Done When

- A descriptor set layout can include one AS binding.
- A descriptor set can be updated with TLAS.
- Compute descriptor writing still works unchanged.

## Phase 4: AccelerationStructure RAII Wrapper

### New Files

- `include/rtv/AccelerationStructure.h`
- `src/rtv/AccelerationStructure.cpp`

### Class Responsibilities

`AccelerationStructure` should own:

- `VkDevice device_`
- `VkAccelerationStructureKHR handle_`
- backing `Buffer buffer_`
- `VkAccelerationStructureTypeKHR type_`
- `VkDeviceAddress deviceAddress_`
- `VkDeviceSize size_`

### Public API

Suggested interface:

```cpp
struct AccelerationStructureDesc {
    VkAccelerationStructureTypeKHR type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
    VkDeviceSize size = 0;
    const char* debugName = nullptr;
};

class AccelerationStructure final : private NonCopyable {
public:
    AccelerationStructure() = default;
    AccelerationStructure(VkDevice device, ResourceAllocator& allocator, const AccelerationStructureDesc& desc);
    ~AccelerationStructure();

    AccelerationStructure(AccelerationStructure&&) noexcept;
    AccelerationStructure& operator=(AccelerationStructure&&) noexcept;

    void create(VkDevice device, ResourceAllocator& allocator, const AccelerationStructureDesc& desc);
    void destroy();

    [[nodiscard]] VkAccelerationStructureKHR handle() const;
    [[nodiscard]] VkDeviceAddress deviceAddress() const;
    [[nodiscard]] VkDeviceSize size() const;
    [[nodiscard]] VkAccelerationStructureTypeKHR type() const;
};
```

### Creation Details

- Allocate backing buffer with:
  - `VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR`
  - `VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT`
- Create AS with `vkCreateAccelerationStructureKHR`.
- Query device address with `vkGetAccelerationStructureDeviceAddressKHR`.
- Set debug name for AS handle if debug utils are available.

### Destruction Details

- Destroy AS handle before backing buffer.
- Move operations must null out source handle.

### Done When

- A BLAS/TLAS-sized AS can be allocated and destroyed cleanly.
- Validation layers report no object lifetime issues.

## Phase 5: RayTracingScene Build Inputs From GpuScene

### Files

- `include/rtv/GpuScene.h`
- `src/rtv/GpuScene.cpp`
- new `include/rtv/RayTracingScene.h`
- new `src/rtv/RayTracingScene.cpp`

### Need From GpuScene

Expose compact immutable build data without duplicating all scene CPU data.

Recommended new structs:

```cpp
struct RayTracingMeshBuildInput {
    uint32_t meshIndex = 0;
    uint32_t firstVertex = 0;
    uint32_t vertexCount = 0;
    uint32_t firstIndex = 0;
    uint32_t indexCount = 0;
    uint32_t primitiveOffset = 0;
    uint32_t primitiveCount = 0;
    bool containsAlphaTestedGeometry = false;
};

struct RayTracingInstanceBuildInput {
    uint32_t instanceIndex = 0;
    uint32_t meshIndex = 0;
    glm::mat4 transform{1.0f};
    uint32_t flags = 0;
    bool visible = true;
};
```

Expose:

```cpp
[[nodiscard]] const std::vector<RayTracingMeshBuildInput>& rayTracingMeshes() const;
[[nodiscard]] const std::vector<RayTracingInstanceBuildInput>& rayTracingInstances() const;
```

### Populate Build Inputs

In `GpuScene::createCornellBox()`:

- Add one mesh build input for the Cornell triangle mesh.
- Add one instance build input for the identity instance.
- Address fallback analytic spheres separately (see Phase 13).

In `GpuScene::createImportedScene()`:

- Add one mesh build input per `GpuMeshRecord`.
- Add one instance build input per `GpuInstanceRecord`.
- Determine `containsAlphaTestedGeometry` by scanning primitive materials:
  - material alpha mode mask/blend.
  - base-color texture with alpha if conservative.

In `createImportedSceneFromCache()`:

- Populate the same build input vectors from cached records.

### Buffer Accessors

RT scene needs device addresses from:

- `scene.localVertices()`
- `scene.localIndices()`
- possibly `scene.meshRecords()` and other buffers for shader descriptors only, not AS build.

Use existing accessors.

### Avoid Stale Inputs

Clear RT build input vectors whenever `GpuScene` rebuilds.

### Done When

- `GpuScene` can report mesh/instance counts matching existing logs.
- Build inputs match `GpuMeshRecord` offsets and `GpuInstanceRecord` transforms.
- Imported and cached scene paths both produce equivalent RT inputs.

## Phase 6: BLAS Build Path

### New Class: RayTracingScene

`RayTracingScene` owns:

- `std::vector<AccelerationStructure> blases_`
- `AccelerationStructure tlas_`
- instance upload/build buffer
- scratch buffers, or temporary scratch allocations during build
- counts/statistics for UI/logging

### Public API

```cpp
struct RayTracingSceneStats {
    uint32_t blasCount = 0;
    uint32_t instanceCount = 0;
    VkDeviceSize blasBytes = 0;
    VkDeviceSize tlasBytes = 0;
    VkDeviceSize scratchBytes = 0;
};

class RayTracingScene final : private NonCopyable {
public:
    RayTracingScene(const VulkanContext& context, ResourceAllocator& allocator, UploadContext& uploadContext, const GpuScene& scene);
    ~RayTracingScene();

    void rebuild(const GpuScene& scene);
    void rebuildTlas(const GpuScene& scene);

    [[nodiscard]] VkAccelerationStructureKHR tlas() const;
    [[nodiscard]] const RayTracingSceneStats& stats() const;
};
```

Prefer constructing after `GpuScene` has uploaded buffers.

### BLAS Geometry Setup

For each `RayTracingMeshBuildInput`:

```cpp
VkAccelerationStructureGeometryTrianglesDataKHR triangles{};
triangles.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
triangles.vertexData.deviceAddress = localVertices.deviceAddress()
    + firstVertex * sizeof(GpuLocalVertex)
    + offsetof(GpuLocalVertex, positionUvX);
triangles.vertexStride = sizeof(GpuLocalVertex);
triangles.maxVertex = vertexCount - 1;
triangles.indexType = VK_INDEX_TYPE_UINT32;
triangles.indexData.deviceAddress = localIndices.deviceAddress() + firstIndex * sizeof(uint32_t);
```

Geometry wrapper:

```cpp
VkAccelerationStructureGeometryKHR geometry{};
geometry.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
geometry.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
geometry.flags = containsAlpha ? 0 : VK_GEOMETRY_OPAQUE_BIT_KHR;
geometry.geometry.triangles = triangles;
```

Build range:

```cpp
VkAccelerationStructureBuildRangeInfoKHR range{};
range.primitiveCount = indexCount / 3;
range.primitiveOffset = 0;
range.firstVertex = 0;
range.transformOffset = 0;
```

### Build Size Query

For each BLAS:

```cpp
VkAccelerationStructureBuildGeometryInfoKHR buildInfo{};
buildInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
buildInfo.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
buildInfo.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
buildInfo.geometryCount = 1;
buildInfo.pGeometries = &geometry;

uint32_t primitiveCount = indexCount / 3;
VkAccelerationStructureBuildSizesInfoKHR sizes{};
vkGetAccelerationStructureBuildSizesKHR(
    device,
    VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR,
    &buildInfo,
    &primitiveCount,
    &sizes);
```

### Scratch Strategy

Simplest robust option:

- Compute max scratch size across all BLAS builds.
- Allocate one scratch buffer of max scratch size.
- Build BLAS sequentially in one command buffer.
- Barrier after each build or after all builds before TLAS.

More performant option later:

- Batch multiple BLAS builds with separate scratch offsets respecting alignment.

### Command Recording

Use the existing `UploadContext`:

- Begin one-time command buffer.
- For each BLAS:
  - set `buildInfo.dstAccelerationStructure`.
  - set `buildInfo.scratchData.deviceAddress`.
  - call `vkCmdBuildAccelerationStructuresKHR`.
  - issue AS build barrier before scratch reuse.
- Submit and wait.

Barrier after each BLAS when reusing scratch:

```cpp
VkMemoryBarrier2 barrier{};
barrier.srcStageMask = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
barrier.srcAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
barrier.dstStageMask = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
barrier.dstAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR | VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
```

### Empty Mesh Handling

Skip meshes with:

- `indexCount < 3`
- `vertexCount == 0`
- device addresses unavailable

If no BLAS can be built:

- Active RT backend should fail clearly or resolve to compute if requested backend is auto.

### Done When

- BLAS objects are built for all triangle meshes.
- Logs show BLAS count and memory usage.
- Validation layers have no AS build warnings.

## Phase 7: TLAS Build And Instance Updates

### Instance Buffer

Create a vector of `VkAccelerationStructureInstanceKHR` from `GpuScene` inputs.

Transform conversion:

```cpp
VkTransformMatrixKHR toVkTransform(const glm::mat4& m) {
    VkTransformMatrixKHR out{};
    out.matrix[0][0] = m[0][0]; out.matrix[0][1] = m[1][0]; out.matrix[0][2] = m[2][0]; out.matrix[0][3] = m[3][0];
    out.matrix[1][0] = m[0][1]; out.matrix[1][1] = m[1][1]; out.matrix[1][2] = m[2][1]; out.matrix[1][3] = m[3][1];
    out.matrix[2][0] = m[0][2]; out.matrix[2][1] = m[1][2]; out.matrix[2][2] = m[2][2]; out.matrix[2][3] = m[3][2];
    return out;
}
```

Vulkan expects a 3x4 row-major transform; GLM stores columns, so transpose carefully as shown.

Instance fields:

```cpp
instance.transform = toVkTransform(transform);
instance.instanceCustomIndex = sceneInstanceIndex;
instance.mask = 0xff;
instance.instanceShaderBindingTableRecordOffset = containsAlpha ? alphaHitGroupOffset : opaqueHitGroupOffset;
instance.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR; // initially safest
instance.accelerationStructureReference = blas.deviceAddress();
```

### TLAS Build

- Upload instances to a GPU buffer with:
  - `VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR`
  - `VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT`
  - `VK_BUFFER_USAGE_TRANSFER_DST_BIT`
- Build TLAS using `VK_GEOMETRY_TYPE_INSTANCES_KHR`.
- Use `VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR` initially.
- If transform updates are common, add `VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR` and later use `VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR`.

### Synchronization

After TLAS build, issue barrier from AS build write to ray tracing shader read:

```cpp
srcStage = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR
srcAccess = VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR
dstStage = VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR
dstAccess = VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR
```

### Update Strategy

Initial complete version can rebuild TLAS on:

- full scene reload.
- transform-only scene update.
- visibility changes if supported.

Do not rebuild BLAS for transform-only updates.

### Done When

- TLAS builds from all visible scene instances.
- TLAS handle can be bound in a descriptor set.
- Transform-only rebuild path does not recreate all static BLAS resources.

## Phase 8: Shader Compiler Support For RT Stages

### Files

- `include/rtv/ShaderCompiler.h`
- `src/rtv/ShaderCompiler.cpp`

### Extension-Based Stage Inference

Add recognized shader extensions:

| Extension | Stage |
| --- | --- |
| `.rgen` | raygen |
| `.rmiss` | miss |
| `.rchit` | closest-hit |
| `.rahit` | any-hit |
| `.rcall` | callable if ever needed |
| `.comp` | compute |
| `.vert` | vertex |
| `.frag` | fragment |

`glslangValidator` can infer from file extension, but explicit `-S` is safer if extension handling is uncertain.

Suggested compile command:

```text
glslangValidator -V --target-env vulkan1.3 -o output.spv source
```

or:

```text
glslangValidator -V --target-env vulkan1.3 -S rgen -o output.spv source
```

### Output Naming

Current output is `source.filename() + ".spv"`, e.g. `pathtrace.comp.spv`. Keep this convention for RT files:

- `pathtrace.rgen.spv`
- `pathtrace.rmiss.spv`
- `pathtrace.rchit.spv`
- `pathtrace.rahit.spv`

### Include Dependency Tracking

Current `dependenciesFor()` supports direct `#include "..."`. Improve if needed:

- Keep direct include support.
- Optional: recursively parse includes so changes to shared includes recompile all dependent shaders.
- Avoid a broad shader build-system rewrite.

### Done When

- RT shader files compile to SPIR-V.
- Existing compute/graphics shaders still compile as before.
- Shared include edits trigger recompilation of RT shaders at least for direct includes.

## Phase 9: Ray Tracing Shader Set And Shared Shader Code

### New Shader Files

Recommended layout:

```text
shaders/rt/pathtrace.rgen
shaders/rt/pathtrace.rmiss
shaders/rt/shadow.rmiss
shaders/rt/pathtrace.rchit
shaders/rt/pathtrace.rahit
shaders/rt/pathtrace_rt_common.glsl
shaders/pathtrace_common.glsl
```

Alternative: put all files directly under `shaders/` if compiler/output code expects a flat directory. The nested directory is cleaner but requires compile path handling.

### Required GLSL Extensions

In RT shaders:

```glsl
#version 460
#extension GL_EXT_ray_tracing : require
#extension GL_EXT_nonuniform_qualifier : enable
```

If keeping `#version 450` works with the SDK, that is acceptable, but `#version 460` is common for RT shader examples. Use what `glslangValidator` accepts consistently.

### Payloads

Use a small hit payload. Avoid recursive ray tracing; keep path loop in raygen and set pipeline max recursion depth to 1 or 2.

Suggested payloads:

```glsl
struct RtHitPayload {
    uint hit;
    uint instanceId;
    uint meshId;
    uint materialId;
    float t;
    vec3 position;
    vec3 geometricNormal;
    vec3 shadingNormal;
    vec2 uv;
    vec4 tangent;
};

layout(location = 0) rayPayloadEXT RtHitPayload hitPayload;
layout(location = 1) rayPayloadEXT bool shadowOccluded;
layout(location = 2) hitAttributeEXT vec2 attribs;
```

If payload size is too large, store compact values and reconstruct more in raygen or closest-hit.

### Descriptor Bindings

Keep set 0 mostly compatible with compute. Add TLAS at binding 33:

| Binding | Descriptor | Notes |
| --- | --- | --- |
| 0 | storage buffer | accumulation |
| 1 | uniform buffer | camera |
| 2 | storage buffer | variance |
| 3 | storage image | raw output |
| 4 | storage buffer | depth/normal |
| 5 | storage buffer | world position |
| 10 | storage buffer | materials |
| 11 | uniform buffer | mesh params |
| 12 | sampled image | environment map |
| 13 | sampler | environment sampler |
| 14 | storage buffer | env rows |
| 15 | storage buffer | env cols |
| 16 | uniform buffer | env params |
| 18 | uniform buffer | debug params |
| 19 | sampled image array | material textures |
| 21 | storage buffer | primitive records |
| 22 | storage buffer | instance records |
| 23 | sampler array | material samplers |
| 24 | storage buffer | light records |
| 25 | storage buffer | mesh records |
| 26 | storage buffer | local vertices |
| 27 | storage buffer | local indices |
| 28 | storage buffer | instance bounds if needed |
| 33 | acceleration structure | TLAS |

Bindings used only by compute BVH traversal (8, 9, 29, 30, 31, 32) can be omitted from the RT layout if shaders do not reference them.

### Ray Generation Shader

Responsibilities:

1. Compute pixel coordinates and bounds check.
2. Seed RNG exactly like compute path.
3. Generate camera ray using camera vectors/FOV/render extent.
4. Loop bounces from 0 to `camera.max_bounces - 1`.
5. Call `traceRayEXT` for each surface ray.
6. On miss, sample environment/sky and terminate.
7. On hit, run material evaluation/direct lighting/BSDF sampling logic.
8. Trace shadow rays for direct/emissive/environment/sun visibility.
9. Accumulate direct/indirect/emissive/environment components for debug output.
10. Write:
    - raw image
    - accumulation buffer
    - variance buffer
    - depth/normal buffer
    - world position buffer

### Closest-Hit Shader

Responsibilities:

1. Read `gl_InstanceCustomIndexEXT`, `gl_PrimitiveID`, and hit barycentrics.
2. Use instance record to find mesh record.
3. Use primitive records to resolve material for the hit triangle.
4. Fetch indices and `GpuLocalVertex` data.
5. Interpolate:
   - local/world position.
   - geometric normal.
   - vertex normal.
   - UV.
   - tangent/bitangent.
6. Decode and apply material textures.
7. Fill payload.

Primitive/material mapping detail:

- `gl_PrimitiveID` is local to the BLAS geometry. If one BLAS geometry covers the full mesh index range, map triangle id to primitive by scanning that mesh's primitive range.
- For performance later, add a triangle-to-material buffer. For initial correctness, scan `primitiveCount` because glTF meshes usually have small primitive counts.
- If primitive scan becomes expensive for large meshes with many primitives, create a `triangleMaterialIds` buffer in `GpuScene` and use direct lookup.

### Any-Hit Shader

Initial implementation options:

1. Omit any-hit and mark all geometry opaque for first visible RT pixels.
2. Add any-hit immediately for alpha support.

Recommended staged approach:

- Milestone A: opaque only; any-hit file exists but not used.
- Milestone B: use separate opaque/alpha hit groups; alpha group includes any-hit.

Any-hit responsibilities:

- Resolve material as in closest-hit enough to sample alpha.
- Apply base color texture alpha and alpha factor.
- For `ALPHA_MODE_MASK`, call `ignoreIntersectionEXT` if below cutoff.
- For `ALPHA_MODE_BLEND`, either deterministic hash threshold or match compute stochastic alpha with RNG. Prefer deterministic hash initially to avoid unstable visibility.

### Miss Shaders

Primary miss:

- Set payload hit to false.
- Optionally set environment radiance fields if payload includes them, or let raygen compute environment from ray direction.

Shadow miss:

- Set `shadowOccluded = false`.

Shadow any-hit/closest-hit:

- Simplest: trace with `gl_RayFlagsTerminateOnFirstHitEXT` and an opaque/alpha-aware hit group.
- For alpha cutouts, any-hit must ignore transparent intersections.

### Shared Shader Refactor

Extract from `pathtrace.comp` in small patches:

1. constants and data structs.
2. RNG helpers.
3. pack/unpack debug/depth/normal/world position helpers.
4. material decoding and texture sampling.
5. environment mapping/CDF sampling.
6. BSDF and MIS helpers.

Keep compute-specific traversal functions in `pathtrace.comp`:

- `intersect_aabb`
- `intersect_triangle`
- `hit_bvh`
- `hit_instance_mesh`
- `hit_instance_scene`
- `hit_sphere`

### Done When

- Minimal RT shaders compile.
- RT raygen can output a deterministic debug image.
- Then full path tracing logic reaches parity with compute for core scenes.

## Phase 10: RayTracingPipeline And Shader Binding Table

### New Files

- `include/rtv/RayTracingPipeline.h`
- `src/rtv/RayTracingPipeline.cpp`

### Class Responsibilities

`RayTracingPipeline` owns:

- `VkDevice device_`
- `VkPipelineLayout layout_`
- `VkPipeline pipeline_`
- `Buffer raygenSbt_`
- `Buffer missSbt_`
- `Buffer hitSbt_`
- SBT region structs
- shader group metadata

### Shader Groups

Minimum group layout:

| Group Index | Type | Shader |
| --- | --- | --- |
| 0 | general | raygen |
| 1 | general | primary miss |
| 2 | general | shadow miss |
| 3 | triangles hit group | closest-hit opaque |
| 4 | triangles hit group | closest-hit + any-hit alpha |

If deferring alpha, use only groups 0-3 initially.

### Pipeline Creation

- Load `ShaderModule` objects for rgen/rmiss/rchit/rahit.
- Create `VkPipelineShaderStageCreateInfo` for each module.
- Create `VkRayTracingShaderGroupCreateInfoKHR` entries.
- Create pipeline layout from RT descriptor set layout and push constants if any.
- Set `maxPipelineRayRecursionDepth`:
  - Prefer 1 if only raygen traces and hit/miss do not recursively trace.
  - Use 2 if closest-hit traces shadow rays, but recommended design traces shadows from raygen, so 1 should suffice.
  - Clamp to device `maxRayRecursionDepth`.

### SBT Building

Inputs:

- `shaderGroupHandleSize`
- `shaderGroupHandleAlignment`
- `shaderGroupBaseAlignment`

Compute:

```cpp
handleSizeAligned = alignUp(handleSize, shaderGroupHandleAlignment);
raygenStride = alignUp(handleSizeAligned, shaderGroupBaseAlignment);
missStride = alignUp(handleSizeAligned, shaderGroupHandleAlignment);
missSize = alignUp(missStride * missGroupCount, shaderGroupBaseAlignment);
hitStride = alignUp(handleSizeAligned, shaderGroupHandleAlignment);
hitSize = alignUp(hitStride * hitGroupCount, shaderGroupBaseAlignment);
```

Get handles:

```cpp
vkGetRayTracingShaderGroupHandlesKHR(device, pipeline, 0, groupCount, dataSize, data.data());
```

Upload each section into an SBT buffer.

SBT region setup:

```cpp
raygenRegion.deviceAddress = raygenSbt.deviceAddress();
raygenRegion.stride = raygenStride;
raygenRegion.size = raygenStride;

missRegion.deviceAddress = missSbt.deviceAddress();
missRegion.stride = missStride;
missRegion.size = missSize;

hitRegion.deviceAddress = hitSbt.deviceAddress();
hitRegion.stride = hitStride;
hitRegion.size = hitSize;
```

### Public API

```cpp
void bind(VkCommandBuffer commandBuffer) const;
void traceRays(VkCommandBuffer commandBuffer, uint32_t width, uint32_t height) const;
VkPipelineLayout layout() const;
```

### Done When

- RT pipeline and SBT are created without validation errors.
- `vkCmdTraceRaysKHR` can be recorded.

## Phase 11: PathTracerRenderer Integration

### Files

- `include/rtv/PathTracerRenderer.h`
- `src/rtv/PathTracerRenderer.cpp`
- `include/rtv/GpuProfiler.h` only if label changes are needed

### Constructor Changes

Add requested backend input:

```cpp
RendererBackend requestedBackend = RendererBackend::Auto
```

Resolve active backend in constructor after `scene_` is created:

```cpp
activeBackend_ = resolveBackend(requestedBackend, context_.rayTracingInfo(), strictMode);
```

Where strict mode is true for CLI `--backend rt`, false for UI auto fallback. It may be simpler to resolve in `Application` before constructing renderer.

### Members

Add:

```cpp
RendererBackend requestedBackend_ = RendererBackend::Auto;
RendererBackend activeBackend_ = RendererBackend::Compute;
std::unique_ptr<RayTracingScene> rayTracingScene_;
std::unique_ptr<RayTracingPipeline> rayTracingPipeline_;
std::unique_ptr<ShaderModule> raygenShader_;
std::unique_ptr<ShaderModule> missShader_;
std::unique_ptr<ShaderModule> shadowMissShader_;
std::unique_ptr<ShaderModule> closestHitShader_;
std::unique_ptr<ShaderModule> anyHitShader_;
VkDescriptorSetLayout rayTracingSetLayout_ = VK_NULL_HANDLE;
```

Expose:

```cpp
RendererBackend activeBackend() const;
RendererBackend requestedBackend() const;
bool hardwareRayTracingAvailable() const;
```

### Pipeline Creation

If active backend is RT:

1. Compile RT shaders.
2. Create RT descriptor set layout manually.
3. Create `RayTracingPipeline`.
4. Build `RayTracingScene` from `scene_`.

If active backend is compute:

- Do not compile RT shaders.
- Do not build AS.

### Descriptor Writing

Create helper methods to avoid duplicating shared bindings:

```cpp
DescriptorWriter PathTracerRenderer::writeCommonPathTraceDescriptors(DescriptorWriter writer, Image& outputImage);
```

Or explicit methods:

- `writeComputePathTraceDescriptors(DescriptorWriter&)`
- `writeRayTracingDescriptors(DescriptorWriter&)`

RT descriptor writer must include TLAS:

```cpp
.writeAccelerationStructure(33, rayTracingScene_->tlas())
```

### recordPathTrace Refactor

Split current function:

- `recordPathTracePrologue(commandBuffer)`
- `recordComputePathTrace(commandBuffer)`
- `recordHardwarePathTrace(commandBuffer)`
- existing denoiser epilogue stays in `recordPathTrace()`

Pseudocode:

```cpp
void PathTracerRenderer::recordPathTrace(VkCommandBuffer cmd) {
    validationLog_.recordPass(activeBackend_ == RT ? "path tracing rt" : "path tracing compute");
    beginPathTraceOutput(cmd);

    if (activeBackend_ == RendererBackend::HardwareRayTracing) {
        recordHardwareRayTrace(cmd);
    } else {
        recordComputeRayTrace(cmd);
    }

    currentProfiler_->write(cmd, GpuProfiler::PathTraceEnd, backendStage());
    if (shouldRunDenoiser()) { ... }
}
```

Use RT stage flags for RT backend:

- profiler stage: `VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR`
- barriers after trace rays:
  - source stage `VK_PIPELINE_STAGE_2_RAY_TRACING_SHADER_BIT_KHR`
  - source access `VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT`

Denoiser currently assumes compute wrote raw/depth/variance/world buffers. Update barriers to choose source stage based on active backend.

### Backend Switching At Runtime

When UI changes backend:

- simplest safe approach: `Application` waits idle and recreates `PathTracerRenderer` with previous settings.
- apply active camera after recreation.
- reset accumulation with `RenderSettingsChanged` or new `BackendChanged` reason.

Add `AccumulationResetReason::BackendChanged` if useful.

### Resize Handling

- Resolution resources remain backend independent.
- Do not rebuild AS on resize.
- Do not recreate RT pipeline on resize.

### Done When

- Active RT backend reaches `vkCmdTraceRaysKHR` and writes output.
- Compute backend path still uses current descriptor layout/pipeline unchanged.
- Backend switch causes safe renderer recreation and accumulation reset.

## Phase 12: Material, Lighting, And Debug Parity

### Material Parity Checklist

RT closest-hit/any-hit must match compute shader for:

- base color factor.
- base color texture with optional manual sRGB conversion flag.
- alpha factor/cutoff/mode.
- normal texture tangent-space mapping.
- metallic-roughness texture.
- emissive texture/factor.
- double-sided material behavior.
- material types currently used by compute:
  - diffuse/lambertian.
  - rough metal.
  - dielectric/specular types already present.

### Lighting Parity Checklist

Raygen path loop must match compute shader for:

- sun lighting toggle/intensity/angular radius.
- direct lighting toggle.
- environment toggle/intensity/rotation.
- HDR environment radiance sampling.
- environment CDF importance sampling.
- emissive triangle light sampling.
- MIS weights and debug outputs.
- indirect strength.
- max bounces.
- Russian roulette if compute shader uses it; otherwise do not add it yet.

### Debug View Parity

Must support or clearly substitute:

- Beauty.
- Raw/accumulation through existing fullscreen/debug controls.
- Variance.
- Normal.
- Depth.
- World position.
- Direct/indirect/environment/emissive.
- Light PDF / BSDF PDF / MIS weight.
- Instance id / mesh id.

Compute-only debug views:

- traversal steps.
- BVH depth.
- TLAS steps.
- traversal mismatch.

For RT backend:

- Return 0 or an RT-specific proxy for traversal metrics.
- For traversal mismatch, display a neutral/black image or a backend-unavailable message in UI.
- Do not attempt to compare RT traversal to compute BVH per pixel as part of first implementation.

### Done When

- Cornell triangle surfaces and imported glTF scenes visually match compute for core views.
- Material textures and normal maps show correctly.
- Environment and emissive lighting work in RT backend.

## Phase 13: Default Scene And Analytic Spheres

### Problem

The fallback Cornell scene has analytic spheres in `scene_spheres`, intersected manually in compute shader. Hardware triangle AS will not include them.

### Preferred Solution: Tessellate Fallback Spheres

When building `GpuScene::createCornellBox()`:

- Replace or augment analytic spheres with tessellated sphere meshes.
- Use enough segments for visual parity, e.g. 32 longitude x 16 latitude.
- Assign equivalent materials:
  - glass sphere.
  - metallic sphere.
  - rough dielectric/specular sphere.
- Include tessellated spheres in:
  - local vertex/index buffers.
  - mesh records / primitive records.
  - light records only if emissive (currently not emissive).
  - RT build inputs.

Options:

1. Use tessellated spheres for both compute and RT, removing analytic sphere dependence from fallback scene over time.
2. Keep compute analytic spheres and add tessellated RT-only sphere meshes.

Recommended complete approach:

- Add tessellated spheres into normal `GpuScene` mesh data and set `sphereCount = 0` for fallback after migration.
- This simplifies visual parity and removes a special case.
- If this is too invasive, keep analytic spheres for compute but add a `RayTracingMeshBuildInput` path for generated sphere meshes used only by RT.

### Done When

- Default startup scene has visible spheres in hardware RT backend.
- Compute backend remains visually acceptable.

## Phase 14: Scene Updates, Materials, And Editor Integration

### Material Updates

Current `GpuScene::updateImportedMaterials()` updates the material buffer without rebuilding scene geometry.

For RT backend:

- Material-only updates do not require BLAS/TLAS rebuild if opacity class does not change.
- If alpha mode changes opaque <-> alpha-tested/blend, hit group/geometry flags may need BLAS rebuild.

Initial policy:

- If material update changes only colors/roughness/emissive/texture factors, upload material buffer and reset accumulation.
- If alpha mode changes, wait idle and rebuild RT scene or full renderer.

### Transform Updates

If scene transform updates are supported by the editor:

- Re-upload instance records.
- Rebuild TLAS only.
- Reset accumulation.

If current app recreates renderer for full scene rebuild, keep that path initially and add TLAS-only optimization later.

### Environment Updates

No AS changes needed.

- Existing environment image/CDF upload remains shared.
- Reset accumulation.

### Backend UI State

`RenderSettingsPanel` should show:

- requested backend.
- active backend.
- hardware RT availability.
- AS stats in debug/profiler panel if easy:
  - BLAS count.
  - instance count.
  - AS memory.

### Done When

- Backend changes from UI are safe.
- Material/environment/camera changes reset accumulation consistently.
- Transform changes do not leave stale TLAS data.

## Phase 15: Validation, Diagnostics, And Error Handling

### Validation Layer Requirements

Run with validation enabled and fix warnings for:

- missing BDA usage on AS build input buffers.
- AS scratch alignment.
- incorrect SBT alignment/size/stride.
- descriptor layout mismatch.
- using AS before build barrier.
- invalid shader group indices.
- max recursion depth exceeding device limit.

### Runtime Error Messages

Add clear errors for:

- `--backend rt` on unsupported device.
- RT supported but no buildable geometry.
- shader compilation failure for RT shader stage.
- SBT group handle query failure.
- TLAS descriptor update with null handle.

### Logging

At renderer creation:

```text
Renderer backend requested: auto
Renderer backend active: hardware-ray-tracing
RT scene: BLAS=42 instances=137 BLAS memory=96.2 MB TLAS memory=1.4 MB SBT=512 B
```

At fallback:

```text
Renderer backend requested: auto
Hardware ray tracing unavailable; using compute backend.
```

### Done When

- Failure cases are understandable without attaching a debugger.
- Validation is clean in smoke tests.

## Phase 16: Documentation

### README Updates

Update `README.md` only after implementation is working.

Add:

- Hardware RT requirements.
- Backend CLI options.
- Examples:
  - `--backend compute`
  - `--backend auto`
  - `--backend rt`
- Explain fallback behavior.
- Explain compute-only debug views.
- Add RT smoke test commands.

### In-Code Comments

Add comments sparingly for non-obvious Vulkan details:

- GLM-to-`VkTransformMatrixKHR` conversion.
- SBT alignment math.
- descriptor write pNext lifetime for acceleration structures.

## Detailed File Change Map

### New Files

- `include/rtv/AccelerationStructure.h`
- `src/rtv/AccelerationStructure.cpp`
- `include/rtv/RayTracingScene.h`
- `src/rtv/RayTracingScene.cpp`
- `include/rtv/RayTracingPipeline.h`
- `src/rtv/RayTracingPipeline.cpp`
- Optional `include/rtv/RendererBackend.h`
- Optional `src/rtv/RendererBackend.cpp`
- `shaders/rt/pathtrace.rgen`
- `shaders/rt/pathtrace.rmiss`
- `shaders/rt/shadow.rmiss`
- `shaders/rt/pathtrace.rchit`
- `shaders/rt/pathtrace.rahit`
- `shaders/rt/pathtrace_rt_common.glsl`
- `shaders/pathtrace_common.glsl` if shared with compute

### Modified Files

- `CMakeLists.txt`
  - Add new `.cpp` files.
- `include/rtv/VulkanContext.h`
  - Capability structs/accessors.
- `src/rtv/VulkanContext.cpp`
  - Extension/feature/property querying and optional enabling.
- `include/rtv/ResourceAllocator.h`
  - Track BDA support.
- `src/rtv/ResourceAllocator.cpp`
  - VMA BDA flag.
- `include/rtv/Buffer.h`
  - Device address API.
- `src/rtv/Buffer.cpp`
  - `vkGetBufferDeviceAddress` wrapper.
- `include/rtv/DescriptorWriter.h`
  - AS descriptor writer support.
- `src/rtv/DescriptorWriter.cpp`
  - `pNext` write handling.
- `src/rtv/DescriptorAllocator.cpp`
  - AS descriptor pool size.
- `include/rtv/GpuScene.h`
  - RT build input structs/accessors.
- `src/rtv/GpuScene.cpp`
  - Populate RT build inputs; add BDA/AS build usage flags; handle fallback spheres.
- `include/rtv/PathTracerRenderer.h`
  - Backend state and RT resources.
- `src/rtv/PathTracerRenderer.cpp`
  - Compile RT shaders, create RT pipeline/scene, branch dispatch.
- `include/rtv/SceneComponents.h`
  - Backend in render settings.
- `src/rtv/Application.cpp`
  - Propagate CLI/backend settings; safe renderer recreation on backend change.
- `include/rtv/Application.h`
  - Store requested backend.
- `src/main.cpp`
  - Parse `--backend`.
- `src/rtv/RenderSettingsPanel.cpp`
  - Backend UI control.
- `include/rtv/ShaderCompiler.h`
  - Optional stage-aware compile API.
- `src/rtv/ShaderCompiler.cpp`
  - RT shader stage support and possibly recursive includes.
- `README.md`
  - Document final behavior.

## Verification Plan

### Build Checks

Run after each major phase:

```powershell
cmake --build native/vulkan/build --config Debug
```

### Compute Regression Checks

```powershell
native\vulkan\build\Debug\rtvulkan.exe --frames 6 --backend compute
native\vulkan\build\Debug\rtvulkan.exe --frames 6 --backend compute --debug-view direct
native\vulkan\build\Debug\rtvulkan.exe --frames 6 --backend compute --gltf NewSponza_Main_glTF_003.gltf
```

### Hardware RT Smoke Checks

On RT-capable hardware:

```powershell
native\vulkan\build\Debug\rtvulkan.exe --frames 6 --backend auto
native\vulkan\build\Debug\rtvulkan.exe --frames 6 --backend rt
native\vulkan\build\Debug\rtvulkan.exe --frames 6 --backend rt --debug-view normal
native\vulkan\build\Debug\rtvulkan.exe --frames 6 --backend rt --debug-view direct
native\vulkan\build\Debug\rtvulkan.exe --frames 6 --backend rt --gltf NewSponza_Main_glTF_003.gltf
native\vulkan\build\Debug\rtvulkan.exe --frames 6 --backend rt --gltf NewSponza_Main_glTF_003.gltf --hdr path\to\environment.hdr
```

### Unsupported Hardware Checks

If available, or by temporarily disabling RT capability in code during testing:

```powershell
native\vulkan\build\Debug\rtvulkan.exe --frames 6 --backend auto
native\vulkan\build\Debug\rtvulkan.exe --frames 6 --backend compute
native\vulkan\build\Debug\rtvulkan.exe --frames 6 --backend rt
```

Expected:

- auto falls back to compute.
- compute works.
- rt fails with a clear message.

### Visual Checks

Compare compute vs RT for:

- default scene.
- imported Sponza scene.
- HDR environment.
- camera movement accumulation reset.
- denoiser on/off.
- material normal maps.
- alpha cutout scene if available.
- emissive surfaces.

### Debug View Checks

Check at least:

- `beauty`
- `normal`
- `depth`
- `world`
- `direct`
- `indirect`
- `environment`
- `emissive`
- `instance`
- `mesh`
- `light-pdf`
- `bsdf-pdf`
- `mis-weight`

For compute-only traversal views, verify RT backend does not crash.

## Implementation Milestones

### Milestone 1: Settings And Capability Plumbing

Deliverables:

- CLI/UI backend setting exists.
- RT capability logs exist.
- Auto/compute still run compute.

Validation:

- Build.
- Existing `--frames 6` smoke test.

### Milestone 2: BDA And Descriptor Infrastructure

Deliverables:

- Buffers can expose device addresses.
- AS descriptor writes are supported.
- Compute remains unchanged.

Validation:

- Build.
- Compute smoke test with validation.

### Milestone 3: BLAS/TLAS Construction

Deliverables:

- `RayTracingScene` builds BLAS/TLAS for imported/fallback triangle geometry.
- Logs AS stats.

Validation:

- Build.
- Launch `--backend auto` on RT-capable hardware and verify AS build validation is clean, even before tracing.

### Milestone 4: Minimal Trace Rays Output

Deliverables:

- RT pipeline and SBT created.
- `vkCmdTraceRaysKHR` writes simple color/normal/depth output.

Validation:

- `--backend rt --frames 6` shows non-black image.
- Denoiser/fullscreen path remains stable.

### Milestone 5: Full Path Tracing Parity

Deliverables:

- RT shaders implement material, lighting, bounces, accumulation, variance, and debug outputs.

Validation:

- Compute/RT visual comparison on default and glTF scenes.

### Milestone 6: Alpha, Default Spheres, Updates, Docs

Deliverables:

- Any-hit alpha support.
- Default scene spheres visible in RT.
- UI backend switching safe.
- README updated.

Validation:

- Full smoke matrix.
- Validation clean.

## Key Risks And Mitigations

### Risk: Shader Refactor Breaks Compute Renderer

Mitigation:

- Extract shared shader code in small pieces.
- Run compute smoke after each extraction.
- Avoid changing compute traversal code while adding RT shaders.

### Risk: Primitive ID To Material Mapping Is Wrong

Mitigation:

- Start with debug output for instance id, mesh id, primitive id/material id.
- Use a direct triangle-to-material buffer if primitive scanning is ambiguous or slow.

### Risk: SBT Alignment Bugs

Mitigation:

- Encapsulate SBT math in `RayTracingPipeline`.
- Assert all region addresses/sizes/strides meet device alignment.
- Log SBT values during development.

### Risk: Any-Hit Alpha Causes Divergence Or Instability

Mitigation:

- Implement opaque-only first.
- Add alpha hit group second.
- Prefer deterministic alpha threshold for cutout/blend validation.

### Risk: Default Scene RT Misses Analytic Spheres

Mitigation:

- Tessellate fallback spheres or convert fallback scene fully to mesh geometry.
- Do not call RT backend complete until default scene works.

### Risk: Non-RT Hardware Regression

Mitigation:

- Keep RT extensions optional.
- Never add RT requirements to physical-device scoring except for explicit `--backend rt` validation.
- Test forced compute and auto fallback.

### Risk: AS Build Memory Usage On Large Scenes

Mitigation:

- Build BLAS sequentially with reusable scratch.
- Log AS memory.
- Add fast-trace vs fast-build flags later if memory/perf demands it.

## Final Acceptance Criteria

- `--backend compute` works and matches current renderer behavior.
- `--backend auto` selects RT on supported GPUs and compute otherwise.
- `--backend rt` builds BLAS/TLAS, creates RT pipeline/SBT, traces rays, and renders a complete path-traced image on supported GPUs.
- RT backend supports imported glTF geometry, material textures, normal maps, emissive surfaces, HDR environment, sun/direct lighting, accumulation, denoising, and fullscreen presentation.
- Default startup scene renders meaningful geometry in RT mode, including the current sphere content or equivalent tessellated replacements.
- Runtime/UI backend switching is safe and resets accumulation.
- Validation layers are clean for build, AS construction, descriptor updates, SBT, and trace dispatch in smoke tests.
- README documents RT requirements, backend options, and smoke commands.
