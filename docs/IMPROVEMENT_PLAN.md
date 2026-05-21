<!-- open-code-summary-anchor -->
# Renderer Improvement Plan (Revision 4)

## Overview

This revision incorporates architectural review feedback on sequencing and subsystem coupling. The progression is now:

```
foundation stability
→ temporal infrastructure
→ editor responsiveness
→ atmosphere + lighting
→ scalability
→ architecture v2
```

### Key Changes from Revision 3

- **Bindless** moved from Phase 2 to Phase 6A — deferred until scalability stage, after temporal/atmosphere/ReSTIR
- **Shared Temporal Layer** added as Phases 2B/2C — formalizes reprojection, confidence, history validation, variance estimation as shared infrastructure consumed by TAA, denoiser, atmosphere, and ReSTIR
- **GPU Debugging Foundation** promoted from long-term to Phase 1B — pass timestamps, resource state viz, velocity/history debug overlays before any temporal work
- **SceneUpdateRouter** added as Phase 1C — replaces `FullSceneRebuild` fallthrough with precise per-mutation routing; `FullSceneRebuild` is an error
- **Render Graph** Stages B (transient) and C (async) moved to Stages 6D and 7A — deferred until temporal + atmosphere pipelines are stable
- **Editor** split into foundation (Stage 0B, parallel with AS refit) and full (Stage 3, after temporal stability)
- **Atmosphere** gated on stable TAA + denoiser + shared temporal layer
- **SER / GPU-driven rendering** added as explicit Phase 7D
- **Acceptance criteria** added per phase — each phase must pass specific, testable gates before the next begins

---

## Build Policy

**No new subsystem begins until the previous subsystem meets ALL of these gates:**

- **Debugged** — no known correctness bugs in common cases. All debug views render expected output. All validation scenes produce consistent results.
- **Profiler-verified** — GPU cost is within 1.5× of the expected budget (specified per phase). No unexpected pipeline stages or hidden barriers.
- **Validation-scene-passing** — all 5 validation scenes (see Phase 1B) render without artifacts, without validation errors, and converge to a stable result.
- **Accumulation-stable** — no observable flicker, ghosting, or temporal instability across 120+ consecutive frames of camera motion.
- **GPU-capture-clean** — no Vulkan validation errors, no barrier warnings, no descriptor validation warnings in a clean GPU capture (RenderDoc or similar).

These gates are checked at each phase transition. If gates are not met, the next phase does not begin. The only exception is bug fixes within the current phase — those are allowed because they're part of getting the gates to pass.

The Build Policy applies to:
- All renderer phases (Stages 0–7 below)
- All editor phases (UX Plan Phases 1–3)
- All atmosphere phases (Sky System Stages A–F)

The UX and atmosphere plans are both dependency-gated by this policy. Do not begin UX Phase 2 until the renderer's Phase A gates pass. Do not begin atmosphere Stage B until atmosphere Stage A gates pass.

---

## Foundation Completion Policy

The single biggest execution risk is building too many foundational systems simultaneously. The correct strategy:

**Finish each foundation completely before starting the next.**

```
Render graph (Phase 1A)
→ stable, used everywhere, debugged
→ THEN temporal foundation (Stage 2)
→ THEN editor responsiveness (Stage 3)
→ THEN atmosphere (Stage 4)
```

Do NOT partially implement all phases simultaneously. Each foundation must be:
- **Stable**: no known correctness bugs in common cases
- **Integrated**: used by all downstream consumers, no dead code paths
- **Debugged**: visualization tools / validation layers pass cleanly

The most dangerous pattern is:

```
Render graph 40% done
+ temporal layer 30% done
+ atmosphere 20% done
+ ReSTIR 10% done
= permanent instability
```

This is how renderer projects become permanently unstable. Resist the temptation to parallelize temporal infrastructure work. The dependencies exist for real engineering reasons, not scheduling convenience.

**Implementation quality > feature count**: From this point onward, prefer stable debug tools, validation, visualization, profiling, and correctness over faster feature accumulation.

**Acceptance criteria**: Each phase below includes concrete gates. Do not begin Phase N+1 until Phase N's acceptance criteria are met.

---

## Stage 0 — Parallel Foundation (Phase A: Foundation)

These phases are independent of each other and can proceed in parallel.

### Phase 0: Acceleration Structure Refit (Effort: Medium, Impact: High)

**Problem**: `PathTracerRenderer::updateSceneTransforms()` calls `vkDeviceWaitIdle` then fully recreates `RayTracingScene`, rebuilding all BLAS + TLAS from scratch. This stalls the pipeline for 100ms+ on transform edits.

**Architecture prerequisite**: Geometry classification by update mode.

```cpp
enum class AccelUpdateMode { Static, RefitTransform, RefitDeform, Rebuild };
```

| Geometry type | Strategy |
|---|---|
| Static (world-space static) | Build once, never touch |
| Transform-only (rigid animated) | TLAS refit, BLAS unchanged |
| Skinned/deforming (skeletal animation) | BLAS refit + TLAS refit |
| Topology-changing (cloth, destruction) | Full rebuild |

**Files to modify:**
- `include/rtv/BindlessResources.h`
- `src/rtv/BindlessResources.cpp`
- `include/rtv/GpuScene.h`
- `src/rtv/GpuScene.cpp`
- `include/rtv/PathTracerRenderer.h`
- `src/rtv/PathTracerRenderer.cpp`

**Descriptor fragmentation**: Long-running editor workflows need a `FreeListAllocator<uint32_t>` for descriptor indices. Without it, hot-reload and scene edits cause descriptor exhaustion, sparse holes, and stale handles. The bindless manager must track free indices and compact when fragmentation grows too high.

**Descriptor cache locality**: Large bindless systems do not have free indexing — random-access divergence and descriptor cache misses become bottlenecks with texture-heavy scenes and wavefront divergence. Long-term, material/texture locality sorting may be needed to group shaders by their descriptor access patterns.

**Shader permutation pressure**: Large bindless + optional features (textures, closures, material flags) create shader permutation explosion. Feature masks or specialization constants may matter more than descriptor management for controlling shader variant count. Plan the shader compilation pipeline to support specialization-based feature selection alongside bindless descriptors.

**Steps:**

1. **Add update flag to `AccelerationStructure`**:
   - `AccelerationStructureDesc { ..., bool allowUpdate }`
   - Pass `VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR` when true.
   - Store a `Buffer` for scratch update source.

2. **Transient scratch arena**:
   - Do NOT keep per-BLAS persistently-sized scratch buffers.
   - Create a single `Buffer` pool sized to the largest scratch need.
   - Align with `minAccelerationStructureScratchOffsetAlignment`.
   - Reuse across all BLAS + TLAS builds within a frame.

3. **RayTracingScene refactor**:
   - Store `std::vector<AccelUpdateMode> perMeshMode_`.
   - Separate `build()` (initial) and `refitTransforms()` (transform-only update).
   - `refitTransforms()`:
     - Update instance buffer with new transforms.
     - For BLAS that are `RefitDeform`: build with `VK_BUILD_ACCELERATION_STRUCTURE_MODE_REFIT_KHR`.
     - For TLAS: always refit if all instances are `Static` or `RefitTransform`; rebuild if any instance is `RefitDeform`.
     - Use scratch arena for temporary memory.

4. **Modify `PathTracerRenderer::updateSceneTransforms()`**:
   ```cpp
   bool PathTracerRenderer::updateSceneTransforms(...) {
       const bool updated = scene_.updateInstanceTransforms(...);
       if (!updated) return false;
       if (activeBackend_ == RendererBackend::HardwareRayTracing) {
           if (!rayTracingScene_->refitTransforms(context_, allocator_, uploader_, scene_)) {
               rayTracingScene_ = std::make_unique<RayTracingScene>(...);
           }
       }
       resetAccumulation(AccumulationResetReason::SceneChanged);
       return true;
   }
   ```
   **Critically**: remove `vkDeviceWaitIdle`. The refit runs on the upload context timeline which already synchronizes via fences.

5. **GpuScene augmentation**:
   - Add `AccelUpdateMode meshUpdateMode(size_t meshIndex)` to `RayTracingMeshBuildInput` (or a parallel array).
   - Pass through from `SceneToGpuSceneBuilder` based on node transform animation state.

6. **Refit quality heuristics** (future):
   - BLAS refit quality degrades over time for skinned/deforming geometry — bounds inflation accumulates across frames.
   - Eventually need: track SAH degradation per BLAS, trigger full rebuild after N refits, abort refit if bounds inflation exceeds a threshold.
   - Without this, traversal quality gradually collapses on heavily deforming assets.

7. **Hybrid partial rebuilds** (future):
   - Refit vs rebuild is not binary — engines evolve toward incremental partial rebuilds (rebuild only high-deformation BLAS leaves, stagger across frames) to avoid full-rebuild frame spikes.
   - Consider supporting per-subtree rebuild granularity once the refit system is stable.

**Acceptance criteria:**
1. Transform edit in editor no longer calls `vkDeviceWaitIdle`
2. BLAS refit completes within 1ms for Sponza scene
3. TLAS refit completes within 0.5ms for Sponza scene
4. Accumulation resets correctly on transform change
5. GPU profiler shows refit time in debug panel

---

### Phase 0B: Editor Quick Wins (Effort: Small, Impact: Medium)

These editor improvements have no renderer dependency and can proceed immediately in parallel with Phase 0.

See `docs/UX_IMPROVEMENT_PLAN.md` Phase 1 — Quick Wins for full implementation details:

- Render Settings collapsing (highest value per line changed)
- Tooltips on non-obvious controls
- Dock layout persistence (per-project `.layout.ini`)
- Delete confirmation modal
- Dirty indicator in window title
- Shortcut labels in menu items
- Centralized keybinding table + Help window
- Camera speed presets

**Acceptance criteria:**
1. Render Settings panel has collapsible sections
2. Tooltips appear on all non-obvious controls
3. Per-project dock layout saves/loads alongside scene file
4. Delete confirmation popup works
5. Window title shows "[Modified]" when scene has unsaved changes

---

## Stage 1 — Engine Infrastructure (Phase A: Foundation)

### Phase 1A: Render Graph — Dependency Graph + Barriers Only (Effort: Very Large, Impact: Architectural)

**Problem**: Manual barrier management (`PathTracerRenderer.cpp` has ~40+ explicit `cmdTransitionImage`/`cmdBufferBarrier` calls per frame) is fragile, non-reorderable, and blocks async compute.

**Target**: A Granite-style render graph -- a DAG of render/compute passes that declares each pass's inputs/outputs and lets the graph automatically insert barriers.

**Critical**: This phase implements Stage A only. Do NOT attempt Stages B or C.

**ONLY**:
- Pass ordering
- Automatic barriers
- Resource state tracking (`ResourceState` enum as public API)

**NO**:
- Transient aliasing
- Heap packing
- Async partitioning

This alone already provides enormous value.

**ResourceState design**: The `ResourceState` enum is the frontend API for pass declarations. Internally the graph compiler resolves each state to the exact Vulkan synchronization primitives needed:

```cpp
struct ResourceAccess {
    VkPipelineStageFlags2 stage;
    VkAccessFlags2 access;
    VkImageLayout layout;
};
```

This prevents edge-case ambiguities where an abstract state like `ShaderRead` could map to different access masks/layouts depending on context.

**Dead-pass elimination**: The graph compiler should cull passes whose output is never consumed:

```cpp
// If denoiser is disabled:
if (!enableDenoiser)
    graph.removePass("denoise");
// If fog is disabled:
if (!enableFog)
    graph.removePass("fog_integrate");
```

This is valuable sooner than expected — skip denoiser, fog, histogram, or debug visualization passes without code changes.

```cpp
enum class ResourceState {
    Undefined,
    PreRasterization,
    ColorAttachment,
    DepthStencilAttachment,
    ShaderRead,
    ShaderStorage,
    UniformBuffer,
    RayTracing,
    ComputeShaderRead,
    ComputeShaderStorage,
    TransferSource,
    TransferDest,
    Present,
};
```

**API decoupling from Vulkan types**: The public API should use abstract enums, not raw Vulkan types. Direct exposure of `VkPipelineStageFlags2` in pass declarations couples the graph frontend to Vulkan synchronization internals, harming portability and making declarations verbose. Instead:

```cpp
enum class PassAccess { Read, Write, ReadWrite };
enum class PipelineDomain { Graphics, Compute, RayTracing, Transfer };

// Internally compiled to:
struct ResourceAccess {
    VkPipelineStageFlags2 stage;
    VkAccessFlags2 access;
    VkImageLayout layout;
};
```

**Resource versioning**: Passes often produce modified versions of the same logical resource (e.g., raw → denoised → TAA'd → tonemapped). Rather than mutating one resource through many states, the graph should internally track logical resource versions (`rawImage_v0`, `rawImage_v1`, etc.) to simplify lifetime analysis and barrier inference.

**Subresource-level synchronization**: `RenderGraphResourceId` alone becomes too coarse once mip-level transitions, array-slice ownership, partial UAV overlaps, or layered rendering are needed. Without subresource granularity, barriers become overly conservative and aliasing opportunities shrink. Long-term, prefer:

```cpp
struct ResourceView {
    RenderGraphResourceId resource;
    uint32_t mip;
    uint32_t mipCount;
    uint32_t layer;
    uint32_t layerCount;
};
```

Not urgent for initial implementation, but the API should allow future extension to subresource granularity.

```cpp
struct RenderGraphResourceId { uint32_t index; };

struct RenderGraphResource {
    enum Type { Texture, Buffer };
    enum class Lifetime { Transient, Persistent, Temporal };
    // Transient:   freed after last use within frame (scrach buffers, intermediate targets)
    // Persistent:  exists for entire application lifetime (swapchain, persistent buffers)
    // Temporal:    lives across frames (TAA history, denoiser history, ReSTIR reservoirs)
    //              — needs special handling: not freed per-frame, not persistent forever
    Type type;
    Lifetime lifetime;
    VkFormat format;
    VkExtent3D extent;
    VkDeviceSize size;
    VkImageUsageFlags usage;
    VkBufferUsageFlags bufferUsage;
    bool transient;           // freed after last use (Stage B)
    bool external;            // owned outside graph (swapchain, persistent)
    const char* debugName;
};

class RenderGraphPass {
public:
    RenderGraphPass& addColorAttachment(RenderGraphResourceId id, ...);
    RenderGraphPass& addInputAttachment(RenderGraphResourceId id, ...);
    RenderGraphPass& addStorageRead(RenderGraphResourceId id, PassAccess access, PipelineDomain domain);
    RenderGraphPass& addStorageWrite(RenderGraphResourceId id, PassAccess access, PipelineDomain domain);
    RenderGraphPass& addUniformBuffer(RenderGraphResourceId id, ...);
    RenderGraphPass& setExecuteCallback(std::function<void(FrameGraphContext&, VkCommandBuffer)> fn);
};

/** Frame ownership context — passed to every pass execute callback.
 *  Prevents implicit resource capture that breaks under async scheduling,
 *  transient aliasing, and graph caching. */
struct FrameGraphContext {
    uint64_t frameIndex;
    RenderGraph& graph;
    TemporalSystem& temporal;
    DescriptorArena& descriptors;
    TransientAllocator& transientAllocator;
};

class RenderGraph {
public:
    RenderGraphResourceId createTexture(const RenderGraphResource& desc);
    RenderGraphResourceId createBuffer(const RenderGraphResource& desc);
    RenderGraphPass& addPass(const char* name);

    void compile(VkDevice device);  // produces immutable compiled graph; builder can be reset
    void execute(VkCommandBuffer cmd);

    void reset(); // per-frame — reuses compiled graph, only updates external resource handles


private:
    // Runtime graph (compiled) is immutable after compile(); builder graph is separate
    // This separation is critical once async scheduling, transient allocation,
    // and pass culling grow in sophistication.
};
```

**Graph compilation cost**: As pass counts grow and transient aliasing + async scheduling mature, full graph recompilation every frame becomes expensive — especially in an editor context. Long-term, incremental graph compilation or cached compiled graph variants (keyed by which passes are enabled) may be needed to avoid per-frame stalls.

**Barrier insertion algorithm (Stage A):**
1. Topologically sort the DAG.
2. For each pass-to-pass edge, determine the image/buffer final layout from producer, expected layout for consumer.
3. Insert `VkImageMemoryBarrier2` / `VkBufferMemoryBarrier2` between passes.
4. Batch barriers per pipeline stage for efficiency.

**Migration strategy**: Do NOT rewrite `PathTracerRenderer` in one shot. Convert one pass at a time (denoiser first, simplest). Once all passes are graph-based, remove the old barrier code.

**Files to modify:**
- `include/rtv/RenderGraph.h` (NEW)
- `src/rtv/RenderGraph.cpp` (NEW)
- `include/rtv/RenderGraphPass.h` (NEW)
- `include/rtv/RenderGraphResource.h` (NEW)
- `include/rtv/PathTracerRenderer.h`
- `src/rtv/PathTracerRenderer.cpp` — substantially refactored
- `CMakeLists.txt`

**Acceptance criteria:**
1. All existing passes (path trace, denoise, histogram, tone map, selection outline, fullscreen) declared in render graph
2. Manual barrier calls removed from `PathTracerRenderer.cpp`
3. Dead-pass elimination works (disabling denoiser removes denoise pass from graph)
4. GPU profiler shows per-pass timestamps within graph
5. Pass reordering in graph does not change visual output
6. All existing debug views work correctly through the graph

---

### Phase 1B: GPU Debugging Foundation (Effort: Medium, Impact: High)

Before any temporal rendering or async compute work, the engine needs basic GPU observability. Without these tools, debugging TAA ghosting, denoiser artifacts, and velocity errors becomes guesswork.

**Minimum tooling:**

- **Pass timestamps**: Per-pass GPU timestamp queries exposed in the debug profiler panel and graph DAG view
- **Intermediate texture viewer**: Ability to inspect any render graph pass output as a debug overlay (cycle through with keyboard shortcut)
- **Resource state visualization**: Color-code current image layout / access mask per resource in the graph view
- **GPU crash markers**: `vkCmdInsertDebugUtilsLabelEXT` region markers for all graph passes
- **Accumulation reset visualization**: On-screen indicator showing reset reason and frame count since last reset
- **Velocity buffer debug view**: Visualize raw velocity buffer as color-coded overlay (direction = hue, magnitude = value)
- **History confidence overlay**: Show per-pixel confidence values for temporal accumulation

**Validation scene suite** — Permanent internal test scenes co-developed with the debugging tools:

| Scene | Tests | Used By |
|-------|-------|---------|
| Temporal Stability | Thin geometry, rotating object, camera pan, emissive flicker, disocclusion | TAA, denoiser, velocity |
| MIS Validation | Emissive quads, glossy metals, rough dielectrics, HDR environment | MIS, ReSTIR, specular |
| Atmosphere Validation | Horizon, sun elevation, aerial perspective, fog density, exposure transitions | Atmosphere |
| AS Stress | Many instances, transform edits, partial rebuilds, visibility toggles | AS refit |
| Divergence | Many materials, roughness extremes, transmissive objects | Wavefront, SER, occupancy |

Each scene has a reference configuration file + expected output checksum for regression detection.

**Files to modify:**
- `include/rtv/PathTracerRenderer.h`
- `src/rtv/PathTracerRenderer.cpp`
- `include/rtv/DebugProfilerPanel.h` (or new `GpuDebugPanel`)
- `shaders/debug_overlay.comp` (NEW)
- `include/rtv/RenderGraph.h`
- `scenes/validation/` (NEW — validation scene directory)

**Future expansion** (not needed in this phase):
- Barrier visualization in graph DAG
- Transient allocation inspection
- Reservoir debug visualization
- Per-pixel reservoir inspection

These will be added in their respective feature phases.

**Acceptance criteria:**
1. Per-pass GPU timestamps displayed in debug profiler
2. Any intermediate buffer viewable as full-screen debug overlay via keyboard shortcut
3. Resource state (layout/access) visible per pass in the graph
4. Debug markers visible in RenderDoc / GPU capture tools
5. Accumulation reset reason + frame count displayed on HUD
6. Velocity buffer visualization as color overlay (north = green, east = red, magnitude = brightness)
7. History confidence per-pixel as grayscale overlay

---

### Phase 1C: SceneUpdateRouter (Effort: Medium, Impact: Architectural)

**Problem**: `SceneUpdateKind::FullSceneRebuild` appears frequently and degrades responsiveness. Many mutations (material edit, light change, visibility toggle) force unnecessary full rebuilds when only a targeted update is needed.

**Solution**: Replace manual `if/then` invalidation with a routing layer that maps each change type to precise update paths.

```cpp
enum class SceneUpdateKind {
    // Precise kinds — each maps to minimal GPU work
    TransformOnly,         // TLAS refit + instance buffer update + accumulation reset
    MaterialOnly,          // Per-material descriptor update + accumulation reset
    LightOnly,             // Light buffer update only
    CameraOnly,            // View matrix update only (no rebuild)
    VisibilityOnly,        // Instance mask update only (no accumulation reset)
    AddRemoveEntity,       // Partial scene rebuild (affected BLAS only)
    AddRemoveLight,        // Light buffer rebuild
    EnvironmentOnly,       // Environment map + CDF rebuild

    // Error state — never triggered by production code
    FullSceneRebuild,      // Triggers assertion in debug; only valid during initial load
};
```

**SceneUpdateRouter class:**

```cpp
class SceneUpdateRouter {
public:
    void route(SceneUpdateKind kind, GpuScene& gpuScene, RayTracingScene& rtScene,
               PathTracerRenderer& renderer);

    // Debug tracking
    const char* lastUpdateKindName() const;
    SceneUpdateKind lastUpdateKind() const;
    uint64_t routeCount(SceneUpdateKind kind) const;

private:
    uint64_t routeCounts_[/* kind count */] = {};
    SceneUpdateKind lastKind_ = SceneUpdateKind::FullSceneRebuild;
};
```

**Routing table:**

| Update Kind | Action | Accumulation Reset? | TLAS Impact |
|---|---|---|---|
| TransformOnly | Update instance buffer, TLAS refit | Yes | Refit |
| MaterialOnly | Update material descriptor + buffer | Yes | None |
| LightOnly | Update light buffer | Yes | None |
| CameraOnly | Update view/proj uniforms | No | None |
| VisibilityOnly | Update instance mask bits | No | None |
| AddRemoveEntity | Rebuild affected BLAS + TLAS refit | Yes | Partial rebuild |
| AddRemoveLight | Rebuild light buffer | Yes | None |
| EnvironmentOnly | Upload env map + rebuild CDF | Yes | None |
| FullSceneRebuild | Full rebuild | Yes | Full rebuild |

**Integration:**

- `EditorRequests` gains `SceneUpdateKind sceneUpdate` field
- `Application::applyEditorRequests()` calls `SceneUpdateRouter::route()` instead of blanket `rebuildScene()`
- `SceneUpdateKind` in `SceneDocument::markDirty()` feeds directly into the router
- Assert / error if `FullSceneRebuild` is triggered outside initial scene load or explicit user request

**Acceptance criteria:**
1. Material edit triggers descriptor update only — no TLAS rebuild
2. Visibility toggle triggers instance mask update only — no accumulation reset
3. Transform edit triggers TLAS refit only — no BLAS rebuild
4. Light edit triggers light buffer update only — no BLAS/TLAS rebuild
5. `FullSceneRebuild` never appears in route count after initial scene load
6. Debug panel shows route count histogram per kind

---

## Stage 2 — Temporal Foundation (Phase B: Temporal Stability)

### Phase 2A: Per-Instance Velocity Buffers + Camera Jitter (Effort: Large, Impact: High)

**Problem**: The denoiser uses world-position history for reprojection, which is imprecise and doesn't handle rotating objects or camera jitter. There is no velocity buffer for temporal techniques.

**Design:**

```cpp
struct PerInstanceVelocity {
    glm::mat4 prevModelMatrix;  // previous frame's world transform
    glm::mat4 curModelMatrix;   // current frame's world transform
};
```

**Stored in instance record** (add `prevTransform` alongside `transform`).

**GPU velocity computation** (new compute pass or integrated into raygen):

```glsl
// CORRECT: prevTransform * localPos vs curTransform * localPos
// NOT: prevTransform * inverseTransform (fails for non-uniform scaling)
vec4 curWorldPos   = instance.transform * vec4(localPos, 1.0);
vec4 prevWorldPos  = instance.prevTransform * vec4(localPos, 1.0);

// Remove jitter from both clip positions
vec4 curClip  = removeJitter(camera.viewProj, camera.jitter)       * curWorldPos;
vec4 prevClip = removeJitter(camera.prevViewProj, camera.prevJitter) * prevWorldPos;

vec2 velocity = (curClip.xy / curClip.w) - (prevClip.xy / prevClip.w);
// velocity is in NDC space [-1,1]; convert to render-resolution UV space
```

**Key math:**
- `prevTransform * inverseTransform` is incorrect for non-uniform transforms
- Both current and previous jitter must be removed before computing velocity
- Velocity must be in **render-resolution UV space**, not screen pixels — otherwise TSR/dynamic resolution/DLSS-style scaling breaks reprojection
- Otherwise TAA instability and reprojection drift occur

**Camera jitter**: Apply sub-pixel jitter to the projection matrix each frame (Halton sequence), store as `jitterOffset` in camera uniform. Used for temporal AA/ReSTIR.

**Camera-cut reset**: All temporal systems (TAA, denoiser, ReSTIR) need a `bool resetHistory` signal for:
- Teleports / camera cuts
- FOV changes
- Aspect ratio changes
- First frame after scene load

Without this, temporal systems accumulate ghost history from the wrong camera state.

**Files to modify:**
- `include/rtv/GpuScene.h`
- `src/rtv/GpuScene.cpp`
- `include/rtv/PathTracerRenderer.h`
- `src/rtv/PathTracerRenderer.cpp`
- `shaders/pathtrace.rgen`
- `shaders/rt_common.glsl`
- `shaders/denoiser.comp`
- `include/rtv/CameraController.h`
- `src/rtv/CameraController.cpp`

**Steps:**

1. **Add prev transform to instance records**:
   ```cpp
   struct GpuInstanceRecord {
       glm::mat4 transform{1.0f};
       glm::mat4 inverseTransform{1.0f};
       glm::mat4 prevTransform{1.0f};  // NEW
       glm::uvec4 metadata{};
   };
   ```
   On transform update, copy `transform` to `prevTransform` before writing new values.

2. **Upload velocity uniform**:
   ```cpp
   struct PrevCameraUniform {
       glm::mat4 viewProj;
       glm::mat4 invViewProj;
       glm::mat4 prevViewProj;
       glm::vec4 currentPos;
       glm::vec4 prevPos;
       glm::vec2 jitter;       // NEW: current frame jitter
       glm::vec2 prevJitter;   // NEW: previous frame jitter
   };
   ```

3. **Camera jitter generation**:
   - Halton(2, frame) for x, Halton(3, frame) for y.
   - Apply as sub-pixel offset to projection matrix.
   - Only jitter when accumulation is running (not in preview mode).
   - Store both current and previous jitter for reprojection.

4. **Shader velocity output**:
   - In `pathtrace.rgen`, write `velocity_buffer[pixel_index]` as packed `vec2`.
   - Separate velocity buffer (RG16F) or pack into existing auxiliary output.

5. **Skinned mesh motion vectors** (future extension):
   - Rigid transforms are covered by `prevTransform` per instance.
   - Skinned meshes need prev-frame bone matrices to compute per-vertex motion.
   - Approach: store `prevBoneMatrices` buffer alongside `boneMatrices`; compute skinned prev position in vertex shader.
   - Not urgent (requires skeletal animation support first), but keep in the velocity pipeline design.

6. **Dedicated velocity pass** (future scalability):
   - Generating velocity inside `pathtrace.rgen` doesn't scale once denoisers, TAA, editor gizmos, raster overlays, and debug visualizations all need motion vectors.
   - Eventually a dedicated geometry velocity pass (or gbuffer-lite visibility pass) becomes preferable — it runs once per frame and produces a single authoritative velocity buffer consumed by all temporal systems.
   - Not urgent, but the architecture should allow swapping the velocity source without rewriting every consumer.

7. **Camera-relative precision** (large worlds):
   - Large world coordinates cause floating-point precision degradation in velocity computation — reprojection jitter and velocity inaccuracies grow with distance from origin.
   - Long-term, camera-relative rendering or origin rebasing may be necessary for scenes with atmosphere, huge environments, or editor tools.
   - Not urgent unless world size exceeds ~10km from origin.

8. **Dilated velocity fields** (disocclusion boundaries):
   - Even with correct motion vectors, thin geometry, alpha masking, and subpixel motion cause invalid reprojection at disocclusion boundaries.
   - Long-term, dilated velocity fields or closest-depth velocity resolve can extend valid velocity coverage to edges.
   - Not urgent initially, but the velocity buffer format should allow for dilation as a post-process.

**Acceptance criteria:**
1. Velocity buffer renders as correct color overlay (debug view F-key)
2. Static camera shows zero velocity everywhere
3. Camera orbit shows smooth velocity gradient
4. Object rotation produces correct rotational velocity in buffer
5. Camera jitter enabled/disabled toggle works
6. Camera-cut detection triggers within 1 frame of teleport
7. Accumulation resets on camera cut

---

### Phase 2B: Shared Temporal Layer — Core (Effort: Medium, Impact: Architectural)

**Problem**: TAA, denoiser, atmosphere reprojection, and ReSTIR each implement their own history validation, reprojection, and camera-cut detection. This duplicates logic and creates inconsistent rejection behavior across temporal systems.

**Solution**: A shared temporal infrastructure owned by `TemporalSystem` that provides core reprojection and history management consumed by all temporal techniques.

**TemporalSystem — Core responsibilities:**

- **Reprojection API**: Given current UV and velocity, compute previous-frame UV with bounds checking
- **History ownership**: Per-resource history slot management (create, update, invalidate)
- **Camera-cut detection**: Single authoritative signal consumed by all temporal systems
- **Accumulation reset routing**: Centralized reset reason tracking, per-resource reset granularity

```cpp
class TemporalSystem {
public:
    // Reprojection
    struct ReprojectResult {
        vec2 prevUV;
        bool valid;           // false if UV out of bounds
        float confidence;     // 0 = disoccluded, 1 = perfect reprojection
    };
    ReprojectResult reproject(vec2 uv, vec2 velocity, vec2 renderSize);

    // History management
    enum class TemporalResidency {
        Persistent,           // Always kept (TAA main history)
        Evictable,            // Can be freed under memory pressure (denoiser moments)
        HalfResolution,       // Stored at half resolution (variance history, confidence)
        DynamicResolution,    // Resolution adapts to viewport size (sky reprojection)
    };

    struct HistorySlot {
        std::unique_ptr<Image> image;
        bool valid;
        uint64_t lastWrittenFrame;
        TemporalResidency residency;
        float memoryBudgetWeight;   // 0.0 = always evict first, 1.0 = keep as long as possible
    };
    HistorySlot& createHistorySlot(const char* name, VkFormat format, VkExtent2D extent,
                                   TemporalResidency residency = TemporalResidency::Persistent);
    void invalidateSlot(const char* name);
    void invalidateAll();
    void evict();                     // called when VRAM pressure triggers — evicts lowest-weight slots

    // Camera-cut
    bool isCameraCut() const;
    void setCameraCut(bool cut);
    AccumulationResetReason lastResetReason() const;

    // Per-frame
    void beginFrame(uint64_t frameIndex);
    void endFrame();

private:
    std::unordered_map<std::string, HistorySlot> historySlots_;
    bool cameraCut_ = true;           // start as cut on first frame
    AccumulationResetReason resetReason_;
    uint64_t frameIndex_ = 0;
    uint64_t totalHistoryMemory_ = 0; // bytes — for budget enforcement
    static constexpr uint64_t maxHistoryMemory_ = 512 * 1024 * 1024; // 512 MB budget
};
```

**Camera-cut integration:**
- Camera move, FOV change, aspect ratio change, scene load → `temporalSystem.setCameraCut(true)`
- All temporal systems check `isCameraCut()` at start of frame
- On cut: reset history, set all confidence to 0, skip temporal reuse
- After cut: exponential blend-down over N frames

**History memory pressure**: TAA + denoiser + ReSTIR + future volumetrics all accumulate per-pixel temporal history. Plan the history layout to allow future compression, half-resolution fallback, or selective persistence.

**Multi-resolution accumulation**: Design history slot dimensions to cleanly support half-resolution fallback for future scalability.

**Acceptance criteria:**
1. `TemporalSystem::reproject()` produces identical UVs to manual reprojection for all tested cases
2. Camera-cut signal propagates to TAA, denoiser within 1 frame
3. History slot creation/destruction shows zero leaks in GPU memory tracker
4. `invalidateAll()` called on scene load clears all temporal state
5. Frame index counter increments correctly across temporal techniques

---

### Phase 2C: Shared Temporal Layer — Extensions (Effort: Medium, Impact: High)

Extensions build on the core layer and provide composable confidence heuristics, variance estimation, and rejection logic.

**Components:**

**1. Variance Estimation**

```cpp
struct VarianceEstimator {
    // Per-pixel luminance variance over recent frames
    // Used for: stable clamping thresholds, adaptive filtering strength
    void update(Image& currentFrame, Image& historyVariance, uint64_t frameIndex);
    float perPixelVariance(vec2 uv) const;
};
```

Track per-pixel luminance variance using an exponential moving average:
```glsl
float variance = lerp(prevVariance, abs(currentLum - prevLum), alpha);
```

**2. History Confidence**

```cpp
struct ConfidenceEvaluator {
    float evaluate(float lumVariance, float depthDisocclusion,
                   float normalDisocclusion) const {
        return exp(-lumVariance * 2.0) *
               exp(-depthDisocclusion * 8.0) *
               exp(-normalDisocclusion * 4.0);
    }

    // Reactive mask for specular/emissive high-frequency regions
    float reactiveWeight(float localContrast, float luminance) const;
};
```

**3. Variance Clipping**

```cpp
struct VarianceClipper {
    // Clamp history sample to neighborhood variance range
    // Tighter clamp = less ghosting, more flicker
    // Looser clamp = more ghosting, less flicker

    // YCoCg color space for clamping (preserves chroma better than RGB)
    vec3 clampHistory(vec3 history, vec3 current, float variance,
                      float strength) const;
};
```

RGB-space clamps desaturate highlights and smear chroma. YCoCg preserves chroma better during clamping.

**4. Disocclusion Detection**

```cpp
struct DisocclusionDetector {
    // Track neighborhood min/max depth history
    // Detect disocclusion when current depth falls outside previous-frame
    // depth range at reprojected position

    bool isDisoccluded(vec2 uv, float currentDepth,
                       const Image& depthHistory) const;

    // Apply exponential blend-down when confidence is low
    float blendFactor(float confidence) const;
};
```

**5. Accumulation Reset Routing**

Extend `TemporalSystem::invalidateSlot()` to support per-resource reset:

```cpp
// A material change may only need to reset the denoiser history,
// not the TAA history or ReSTIR reservoirs.
temporalSystem.invalidateSlot("denoiser_history");
```

**Integration with temporal consumers:**

| Consumer | Uses Core | Uses Extensions |
|----------|-----------|-----------------|
| TAA | reproject, camera-cut, history | variance clipping, confidence |
| Denoiser | reproject, history | variance, confidence, disocclusion, reactive mask |
| Atmosphere sky reprojection | reproject, camera-cut | confidence, variance clipping |
| ReSTIR | reproject, camera-cut, history slot | confidence, disocclusion |

**Acceptance criteria:**
1. Variance estimator converges within 8 frames on static scene
2. Confidence evaluator produces 0.0 for disoccluded pixels, 1.0 for stable pixels
3. YCoCg clamping preserves chroma better than RGB (visible in debug overlay)
4. Disocclusion detector correctly identifies moving-object edges
5. Per-slot invalidation works (denoiser resets independently of TAA)
6. All temporal layers share a single camera-cut signal with no frame of delay

---

### Phase 2D: TAA / Temporal Super Resolution (Effort: Medium, Impact: High)

**Jitter without TAA is incomplete. ReSTIR stability benefits heavily and denoiser quality improves.**

**Files:**
- `shaders/taa.comp` (NEW)
- `include/rtv/PathTracerRenderer.h`
- `src/rtv/PathTracerRenderer.cpp`
- `CMakeLists.txt`

**Render order**: TAA must run **before** tonemapping, in HDR space. The correct frame pipeline is:

```
HDR lighting output
→ TAA (accumulate + reproject + anti-ghost in HDR)
→ tone map (exposure + color grading)
→ sharpening (optional)
→ UI overlay
→ present
```

TAA after tonemapping causes bloom instability, highlight clipping, and incorrect blending of bright regions.

**Implementation:**
- Accumulate history with velocity-based reprojection (Phase 2A) through shared temporal layer (Phase 2B)
- Anti-ghosting with AABB neighborhood clamp in **YCoCg perceptual color space**, not RGB
  - RGB-space clamps desaturate highlights and smear chroma
  - YCoCg preserves chroma better during clamping
- Variance clipping: clamp history sample to local variance range, not just AABB (uses Phase 2C)
- **Sharpening after TAA**: Weak sharpening produces blur; aggressive sharpening creates temporal shimmer. Evaluate CAS (Contrast Adaptive Sharpening) or RCAS (Robust Contrast Adaptive Sharpening). Eventually, temporal-aware sharpening that accounts for motion may be needed.
- Write to HDR intermediate buffer

**Integration with jitter:**
- Jitter is applied in the projection matrix during path tracing
- TAA removes jitter by blending current frame with history
- Without TAA, jittered output flickers visibly

**Important: Ship TAA before TSR**: Temporal Super Resolution (reconstruction + upsampling + disocclusion + sharpening) is substantially harder than TAA. TSR involves resolution-independent reprojection, adaptive sharpening, and stability across upsampling ratios. Ship standard TAA first as a separate phase, then add TSR as a later extension. Do not attempt both simultaneously.

**Reactive masks for translucency/emissives**: Standard single-history TAA struggles with particles, emissives, and UI overlays. Long-term, the TAA architecture should support per-layer reactivity — separate accumulation rates for foreground vs background, reduced blending for high-frequency detail. Similar to TSR reactive masks, but achievable in standard TAA with careful confidence weighting.

**Acceptance criteria:**
1. Jittered output is visually stable after TAA (no visible flicker)
2. Static scene converges to sharp image within 16 frames
3. Moving camera shows no ghosting on stationary objects
4. Object motion shows minimal ghosting (trail < 4 pixels)
5. TAA off shows visible jitter; TAA on removes it
6. History confidence overlay shows stable values in static regions
7. Variance clipping prevents ghosting on high-contrast edges

---

### Phase 2E: Denoiser Hardening (Effort: Medium, Impact: High)

**Problem**: The denoiser has basic temporal accumulation but lacks proper motion vector reprojection, disocclusion detection, history confidence, reactive masks, and variance estimation.

**Now consumes shared temporal layer** (Phases 2C/2D) instead of implementing its own reprojection and confidence logic.

**Files to modify:**
- `shaders/denoiser.comp`
- `shaders/pathtrace.rgen`
- `include/rtv/PathTracerRenderer.h`
- `src/rtv/PathTracerRenderer.cpp`

**Steps:**

1. **Replace world-position reprojection with shared temporal reprojection** (Phase 2B):
   ```glsl
   TemporalSystem::ReprojectResult result = temporalSystem.reproject(
       vec2(coords), velocity, renderSize);
   vec2 historyPos = result.prevUV;
   float confidence = result.confidence;
   ```

2. **Use shared variance estimation** (Phase 2C):
   - Track per-pixel luminance variance over recent frames
   - Use variance for: stable clamping thresholds, adaptive a-trous filtering strength
   - Without variance estimation, clamping is either too aggressive (flicker) or too weak (ghosting)

3. **Use shared confidence evaluation** (Phase 2C):
   ```glsl
   float historyConfidence = confidenceEvaluator.evaluate(
       lumVariance, depthDisocclusion, normalDisocclusion);
   ```

4. **Use shared disocclusion detection** (Phase 2C):
   - Track neighborhood min/max depth history.
   - Detect disocclusion when current depth falls outside previous-frame depth range at reprojected position.
   - Apply exponential blend-down when confidence is low.

5. **Use shared reactive mask** (Phase 2C):
   - Detect high-frequency detail regions (specular highlights, emissive edges).
   - Reduce temporal blending in those regions to avoid ghosting.
   - Store `reactiveMask` as a per-pixel value.

6. **Anti-flicker for emissive/light sources**:
   - Clamp history luminance more aggressively for pixels with high variance.
   - Use per-pixel adaptive clamping based on local contrast.

**Future evolution — split histories**:
- The current architecture uses single-history temporal accumulation.
- Modern denoisers decouple diffuse / glossy / reflection / GI histories because they converge at different rates.
- Not needed immediately, but important to keep in mind — split histories will be easier to add once the shared temporal infrastructure (Phases 2B/2C) is stable.

**Confidence-aware filtering**: Once ReSTIR, RIS, and adaptive sampling coexist, variance behavior becomes more chaotic than standard path tracing. Long-term, sample classification or confidence-aware filtering (weighting filter taps by local variance confidence rather than spatial distance alone) may be needed to avoid over-blurring in regions with unstable variance.

**Acceptance criteria:**
1. Denoiser uses `TemporalSystem::reproject()` — no independent reprojection code
2. Denoiser uses shared confidence evaluator — no independent confidence logic
3. Variance estimation visible in debug overlay
4. Disocclusion correctly detected at motion boundaries
5. Reactive mask prevents ghosting on emissive/specular highlights
6. Denoiser quality matches or exceeds pre-hardening baseline
7. Camera-cut correctly flushes denoiser history

---

## Stage 3 — Editor Responsiveness (Phase C: Editor UX)

At this point the engine has:
- AS refit (no `vkDeviceWaitIdle` on transform edits)
- Render graph with pass-level barriers
- SceneUpdateRouter (no FullSceneRebuild fallthrough)
- Stable temporal infrastructure

These prerequisites mean editor operations no longer cause GPU stalls, making the following features practical and responsive.

See `docs/UX_IMPROVEMENT_PLAN.md` Phase 2 and Phase 3 for full implementation details.

**Features (in priority order):**

### Phase 3A: Visibility/Lock Toggles
- Eye icon for per-entity visibility (hierarchical propagation)
- Lock icon for selection prevention
- Instance mask update only — no accumulation reset (via SceneUpdateRouter)

### Phase 3B: Undo/Redo System
- Polymorphic `ICommand` hierarchy
- `CommandTransaction` for gizmo drag batching
- `UndoStack` with 256-entry limit
- Integration with `EditorRequests` and `SceneUpdateRouter`

### Phase 3C: Snapping, Notifications, Progress
- Gizmo snapping (translation, rotation, scale)
- Toast notification manager
- Renderer progress feedback (accumulation %, moving indicator)

### Phase 3D: Transform Utilities, Stats, Diagnostics
- Transform reset/copy/paste
- Scene statistics panel (entities, meshes, triangles, BVH nodes, AS memory)
- GPU diagnostics panel (per-pass timing, rays/sec, memory usage)
- Entity search/filter in hierarchy
- Context menus (duplicate, delete, rename, create child)

**Acceptance criteria (overall Stage 3):**
1. Transform edit completes within 2ms (AS refit + instance buffer update only)
2. Material edit completes within 1ms (descriptor update only)
3. Visibility toggle completes within 0.5ms (instance mask only)
4. Undo/redo of transform edit correctly restores previous state
5. Notification appears on scene load, material edit, settings change
6. Scene stats panel shows correct counts matching known scene data

---

## Stage 4 — Atmosphere System (Phase D: Atmosphere)

### Phase 4A: Atmosphere + Height Fog (Effort: Medium, Impact: High)

**Prerequisites**: Stable HDR accumulation (Phase 2D), stable temporal reprojection (Phase 2B/2C), stable tone mapping (existing), render graph (Phase 1A), debugging tools (Phase 1B).

**Note**: Participating media (heterogeneous volumetric integration) is NOT included here. It is deferred to the long-term section.

See `docs/SKY_ATMOSPHERE_SYSTEM.md` for the full detailed design. This phase references the key implementation steps.

**New files:**
- `shaders/atmosphere.comp` (precomputed scattering LUT)
- `shaders/fog_integrate.comp` (screen-space fog)

**Files to modify:**
- `include/rtv/PathTracerRenderer.h`
- `src/rtv/PathTracerRenderer.cpp`
- `shaders/pathtrace.rgen`
- `CMakeLists.txt`

**Steps:**

1. **Precomputed atmosphere LUT** (Bruneton-style):
   - Do NOT use a single large 3D volumetric texture — expensive for scattering order and precision.
   - Instead use the Eric Bruneton precomputed atmosphere model:
     - **Transmittance LUT** (1D/2D): optical depth along view ray
     - **Multi-scattering LUT** (3D): accumulated higher-order scattering
     - **Sky-view LUT** (2D): final sky color per view direction
   - These smaller, specialized LUTs are cheaper to update and more accurate.
   - Use atmosphere parameter hashing for LUT cache invalidation — sun direction is not the only parameter that invalidates LUTs. Weather, turbidity, ozone concentration, and altitude changes also require recomputation.

2. **Sun/LUT separation**:
   - The sun is **purely analytical** — removed from the sky-view LUT.
   - Sky LUT stores only diffuse atmospheric scattering (sky dome color without the solar disk).
   - Sun handled entirely through analytical functions.
   - This avoids double-counting solar energy and keeps MIS clean.

3. **Temporal reprojection for sky-view LUT**:
   - Consumes `TemporalSystem::reproject()` and `ConfidenceEvaluator` from Phase 2B/2C.
   - Sky-view LUT reprojection uses velocity-based camera-motion reprojection.
   - History confidence and variance clipping prevent ghosting on sun/horizon.

4. **Height fog** (exponential density):
   - After path tracing, run `fog_integrate.comp` that ray-marches through fog density field.
   - Integrate inscatter + extinction along view ray using the atmosphere LUT.
   - Blend onto the raw output image.

5. **Path tracing integration**:
   - Atmosphere transmittance applied per-bounce (extinction along ray segments).
   - Sky visibility sampled via environment map with atmosphere attenuation.
   - Environment importance sampling from sky-view LUT luminance only (no sun in CDF).

6. **Physical camera exposure**:
   - Atmosphere only produces correct output with physically based exposure.
   - EV100-based exposure system integrated with tone mapper.
   - AgX or UE5-style filmic tone mapping for atmosphere's high dynamic range.

**Long-term: lighting through fog volumes**:
- Post-process fog (blend after path tracing) is acceptable initially, but eventually fog participates in lighting — god rays, emissive fog, shadowed fog, and local volumetrics cannot remain purely post-process.
- Direction: froxel grids or volumetric lighting injection for in-scatter from light sources through participating media.
- Not urgent, but keep the fog architecture extensible toward volume integration.

**Acceptance criteria:**
1. Sky-view LUT produces visually correct sky dome (blue zenith, warm horizon, solar corona)
2. Analytical sun disk renders correctly without LUT contamination
3. Transmittance LUT produces correct extinction on distant objects
4. Temporal reprojection of sky-view LUT shows no ghosting on camera rotation
5. Height fog correctly attenuates distant objects exponentially
6. Physical exposure produces correct sky brightness range (10³–10⁶ cd/m²)
7. Atmosphere parameters (turbidity, ozone, albedo) produce visibly different sky states
8. LUT update is < 0.5ms total across all passes
9. Accumulation resets correctly on sun direction change (not on camera orbit)

---

## Stage 5 — Lighting Improvements (Phase E: Lighting Scalability)

### Phase 5A: Specular MIS + BSDF Improvements (Effort: Medium, Impact: Medium)

**Problem**: Rough specular/glossy reflections lack proper BSDF-vs-light MIS. Glossy NEE is noisy because it doesn't importance-sample the BSDF lobe. Rough metals darken incorrectly due to energy loss.

**Files to modify:**
- `shaders/pathtrace.rgen`
- `shaders/rt_common.glsl`

**Steps:**

1. **Always combine light + BSDF sampling with power heuristic**:
   - Do NOT switch strategies based on roughness (remove `roughness < 0.3` branch).
   - Sample light, trace shadow, evaluate BSDF, weight with `power_heuristic(lightPdf, bsdfPdf)`.
   - Sample BSDF, trace bounce ray, if hits light, weight with `power_heuristic(bsdfPdf, lightPdf)`.

2. **Multiple-scattering GGX compensation**:
   - Rough metals darken incorrectly without it (energy loss from single-scattering assumption).
   - Implement Heitz multiple-scattering GGX compensation:
     ```glsl
     vec3 msCompensation = compute_ms_ggx(roughness, metallic, F0, NdotV);
     brdf *= (1.0 + msCompensation);
     ```
   - Precompute energy compensation LUT or use analytic approximation.

3. **Extend MIS to emissive triangle hits**:
   - When a BSDF bounce ray hits an emissive triangle, compute `emissive_hit_pdf()` and apply MIS between `bsdfPdf` and `emissivePdf`.
   - Currently this only works for environment light misses; extend to emissive geometry.

4. **Fix `pbr_specular_sample_probability`**:
   - Replace luminance heuristic with actual Fresnel-weighted blend:
   ```glsl
   float specularWeight = clamp(
       brdf_luminance(f0) / max(brdf_luminance(f0) + brdf_luminance(kd * color / PI), 1e-6),
       0.05, 0.95
   );
   ```

**Future direction — lobe selection MIS**:
- The current roadmap assumes single-lobe GGX-centric BSDF evaluation.
- Once closures expand to include sheen, transmission, thin film, and anisotropy, lobe selection MIS becomes necessary — each bounce must choose which lobe to sample and weight contributions correctly.
- Not urgent now, but the closure iteration loop (`for i < 4`) must support per-lobe MIS weights from the start.

**Spectral rendering** (very long-term):
- Thin film, dispersion, and transmissive materials are physically limited in RGB space.
- If physical accuracy becomes a priority later, spectral rendering (sampling wavelengths instead of RGB) may become relevant.
- Not urgent — acceptable to remain RGB-only for the foreseeable future.

**Transmission / refraction MIS**:
- Glass, thin film, and layered transmission make path selection substantially harder — each interface requires BSDF sampling for both reflection and transmission lobes.
- Once transmissive materials are added, the MIS system must handle: reflection vs transmission probability, dielectric Fresnel, and total internal reflection.
- Not urgent now, but the MIS architecture should generalize to N lobes per interaction.

**Acceptance criteria:**
1. Rough metallic spheres show correct energy-conserving brightness (compare with reference)
2. Glossy NEE noise reduced by > 50% on dielectric spheres
3. Emissive triangle MIS removes fireflies from BSDF-scattered emissive hits
4. Power heuristic used for all light/BSDF combinations (no roughness branches)

---

### Phase 5B: ReSTIR DI (Effort: Very Large, Impact: High)

**Dependencies**: Requires Phase 2A (velocity), Phase 2B/2C (shared temporal layer), Phase 2D (TAA working), Phase 2E (denoiser + variance), Phase 5A (specular MIS stability).

**Problem**: 1 environment sample + 1 emissive sample per hit is noisy for complex lighting. Spatial reuse can reduce noise 10x at minimal cost.

**Design**: Hybrid ReSTIR mode — classic NEE remains as a fallback and comparison path. ReSTIR replaces direct lighting NEE only when enabled, with full debug isolation.

### Hybrid Mode Strategy

During development, ReSTIR operates alongside classic NEE:

| Mode | Behavior | Use Case |
|------|----------|----------|
| **Classic NEE** (default) | Existing per-hit light sampling | Baseline reference, no ReSTIR code runs |
| **ReSTIR Only** | Full ReSTIR pipeline, NEE disabled | Primary mode once stable |
| **Hybrid Compare** | Split-screen or A/B toggle at runtime | Debug — verify ReSTIR matches NEE within tolerance |

**Split-screen debug modes:**
- Left/right split: NEE vs ReSTIR on same frame
- Per-pixel difference overlay: `abs(nee_direct - restir_direct)` as heat map
- Reservoir debug overlay: age, confidence, M, sample count per pixel

This isolation is critical because ReSTIR bugs (temporal reuse contamination, spatial bias, PDF errors) are notoriously difficult to diagnose without a clean reference path.

### Architecture

```
Per pixel each frame:
1. Initial sampling: sample 1 light, evaluate BSDF + visibility -> reservoir
2. Temporal reuse: reproject previous reservoir -> merge via pairwise MIS
3. Spatial reuse: 3-4 rounds of neighbor reservoir merging (blue-noise offsets)
4. Final shading: use reservoir for direct lighting contribution
```

**New files:**
- `shaders/restir_init.comp`
- `shaders/restir_temporal.comp`
- `shaders/restir_spatial.comp`
- `shaders/restir_final.comp`

**Files to modify:**
- `include/rtv/PathTracerRenderer.h`
- `src/rtv/PathTracerRenderer.cpp`
- `shaders/pathtrace.rgen`
- `CMakeLists.txt`

**GPU data structures:**

```cpp
struct RestirReservoir {
    uint lightIndex;      // index into light list (UINT32_MAX = environment)
    float targetPdf;      // p_hat(q)
    float weightSum;      // sum of w(x_i, q)
    float M;              // sample count
    vec3 sampleValue;     // L_i * f * cos / p(q)
    float confidence;     // from shared temporal layer
    uint age;             // reservoir age for stale rejection
    uint metadata;        // packed: env flag, validity, etc.
};
```

**Critical: Blue-noise neighbor offsets** — do NOT use deterministic ring radii (1, 2, 4, 8). They create structured artifacts. Instead:
- Precompute blue-noise sampling masks
- Use stochastic neighbor selection per pixel
- Reject neighbors with: depth difference > threshold, normal dot < threshold

**Visibility reuse complexity**: Glossy surfaces, thin geometry, and alpha-masked materials make reservoir visibility validation significantly harder than it first appears. A reservoir from a neighbor pixel may have a valid light sample that is occluded for the current pixel. This requires careful neighbor rejection heuristics and will need iterative tuning.

**Reservoir normalization safeguards**: Without safeguards, reservoirs can destabilize from:
- Huge weight values from low-PDF samples
- Fireflies from invalid PDF ratios
- Accumulated FP error over many temporal frames

Use **robust luminance clamping** (not hard arbitrary max values):
- Hard clamping of `sampleValue` biases lighting and kills energy conservation
- Prefer percentile-based clamping: track luminance distribution across the frame and clamp outliers to e.g. 99.9th percentile
- Alternatively, clamp the reservoir contribution weight rather than the sample value itself

**Specular-aware reuse rejection**: Diffuse reuse is relatively manageable; glossy/specular reuse is far harder because visibility changes rapidly, PDFs spike, and reprojection fails more often. Eventually, specular-aware neighbor rejection heuristics will be needed (separate confidence for specular reservoirs, stricter depth/normal thresholds for glossy pixels).

**Bias correctness — biased vs unbiased reservoir strategy**: The pairwise MIS merge described above is intentionally simplified. ReSTIR bias correctness is extremely subtle once temporal reuse, spatial reuse, RIS, and visibility reuse all interact. RTXDI-style systems often accept controlled bias for stability. Future work must decide: accept bias for visual stability, or maintain unbiasedness at the cost of noisier results? This is an explicit architectural choice, not an implementation detail.

**Compressed reservoir storage**: Reservoirs + histories + velocities + denoiser moments + TAA buffers create severe memory bandwidth pressure before wavefront even arrives. RTXDI-style implementations aggressively compress reservoir storage: packed PDFs, packed confidence, quantized luminance. Design the reservoir struct to allow future packing — separate hot fields (accessed every temporal reuse) from cold fields (only needed for debug visualization).

**Key implementation details:**

1. **Initial sampling** (`restir_init.comp`):
   - Per pixel, sample 1 light from the light list (uniform + environment).
   - Trace shadow ray for visibility.
   - Evaluate BSDF * cos / pdf to get `sampleValue`.
   - Store as reservoir with M=1, age=0, confidence=1.0.

2. **Temporal reuse** (`restir_temporal.comp`):
   - Reproject current pixel to previous frame using shared temporal layer (Phase 2B).
   - Load previous reservoir from `prevReservoirBuffer`.
   - Compute pairwise MIS weight: `w = p_hat(new) / (p_hat(new) + p_hat(old))`.
   - Merge: `reservoir += oldReservoir * w_old`.
   - Decrement confidence for disoccluded pixels; kill reservoir if age > maxAge.
   - Increment `age` for surviving reservoirs.

3. **Spatial reuse** (`restir_spatial.comp`):
   - 3-4 passes with blue-noise stochastic neighbor offsets.
   - Reject neighbors with: depth difference > threshold, normal dot < threshold.
   - Merge via pairwise MIS: `w = p_hat(current) / (p_hat(current) + p_hat(neighbor))`.

4. **Final shading** (`restir_final.comp`):
   - Unpack reservoir, compute final direct lighting contribution.
   - Write to direct light buffer consumed by path tracing.

5. **Integration into path tracer**:
   - In `pathtrace.rgen`, when ReSTIR is active:
     - Read pre-computed direct lighting from `directLightBuffer`.
     - Skip per-hit NEE entirely (avoid double-counting).
     - Still trace indirect bounces as before.

**Acceptance criteria:**
1. ReSTIR reduces direct lighting noise by > 5x vs 1-sample NEE
2. Temporal reuse shows no ghosting on moving objects (uses shared temporal layer)
3. Spatial reuse shows no structured artifacts (blue-noise verification)
4. Camera-cut correctly flushes all reservoirs
5. Reservoir confidence decreases at disocclusion boundaries
6. Robust luminance clamping prevents fireflies
7. Per-pixel reservoir debug visualization works (age, confidence, M)
8. Specular-aware rejection prevents glossy ghosting

---

### Phase 5C: Light BVH / RIS Acceleration (Effort: Large, Impact: High)

**Problem**: Once ReSTIR is active, the flat light list (including many emissive triangles + environment map) becomes expensive. Emissive triangle clustering and importance sampling are needed.

**Design:**
- Light BVH for efficient many-light importance sampling
- Alias tables for environment map sampling
- RIS (Resampled Importance Sampling) pre-sampling light candidates

**New files:**
- `shaders/light_bvh_build.comp`
- `shaders/light_bvh_sample.glsl`

**Files to modify:**
- `include/rtv/GpuScene.h`
- `src/rtv/GpuScene.cpp`
- `shaders/rt_common.glsl`
- `shaders/restir_init.comp`

**Steps:**

1. **Emissive triangle clustering**:
   - Group emissive triangles by spatial proximity.
   - Build a BVH over emissive clusters.
   - Each cluster stores: bounding box, total emitted power, triangle count.
   - **Power-weighted clustering**: Spatial proximity alone is insufficient. Power-weighted spatial clustering (or solid-angle-aware clustering) matters more for tiny bright emitters, HDR emissives, and distant lights. A dim cluster near a bright emitter should not merge them.

2. **Light BVH traversal in ReSTIR init**:
   - Replace uniform light sampling with BVH-guided importance sampling.
   - Sample cluster by power, then sample triangle within cluster uniformly.

3. **Alias table for environment map**:
   - Build alias table from environment map PDF.
   - Sample environment map in O(1) with alias method.

4. **RIS (Resampled Importance Sampling)**:
   - Pre-sample a set of candidate lights using a cheap proxy PDF.
   - Resample candidates using target BSDF*cos/pdf for better sample quality.
   - Integrate with ReSTIR initial sampling pass.

**Unified light abstraction** (long-term):
- Environment maps remain special-cased. Eventually, unify emissive triangles, directional lights, environment lights, and analytic lights into a single sampling system.
- A unified `LightSample` struct with a common PDF evaluation interface simplifies both ReSTIR integration and future light types.

**GPU-built light hierarchies**: RIS candidate generation may eventually become CPU bottlenecked if preprocessing grows complex. Long-term, GPU-built light BVHs (built and traversed entirely on GPU) may be preferable over CPU preprocessed hierarchies — especially once the renderer pushes toward GPU-driven rendering.

**Acceptance criteria:**
1. Light BVH sampling reduces ReSTIR init variance on many-light scenes
2. Alias table enables O(1) environment map sampling
3. RIS pre-sampling improves initial reservoir quality vs uniform sampling
4. BVH build completes within 1ms on CPU for typical scene
5. Power-weighted clustering prevents bright emitter contamination

---

## Research Track (Deferred — Architecture V2 Forks)

**Do NOT begin Research Track phases until Phases A–E (Stages 0–5) are stable and passing all Build Policy gates.**

These phases are architecture v2 forks. They change scheduling, memory ownership, queue structure, material execution, and descriptor management. Implementing any of them while the megakernel renderer is still in flux guarantees permanent instability.

### Phase 6A: Full Bindless + Descriptor Handle Abstractions (Effort: Large, Impact: High)

**Problem**: Fixed 128-texture array (`material_textures[128]`). Partial bindless -- only textures, not materials/geometry/lights.

**Scope**: All descriptor arrays become dynamically sized:
- `material_textures[]` + combined `sampler2D`
- material buffer (indexed via handles)
- light data
- geometry instances

**New abstraction layer:**

```cpp
struct TextureHandle  { uint32_t index; };
struct MaterialHandle { uint32_t index; };
struct MeshHandle     { uint32_t index; };

class BindlessDescriptorManager {
public:
    TextureHandle registerTexture(Image& image, VkSampler sampler);
    MaterialHandle registerMaterial(const GpuMaterial& material);
    MeshHandle registerMesh(const GpuMeshRecord& mesh);

    void updateDescriptorSet(VkDescriptorSet set); // bulk update

    VkDescriptorSetLayout layout();
    VkDescriptorPool pool();

private:
    std::vector<VkDescriptorImageInfo> textureArray_;
    Buffer materialBuffer_;        // GPU-side array
    Buffer meshBuffer_;            // GPU-side array
};
```

**Shader changes:**

```glsl
// Instead of:
layout(set = 0, binding = 19) uniform texture2D material_textures[128];
layout(set = 0, binding = 23) uniform sampler material_samplers[128];

// Use:
layout(set = 0, binding = 19) uniform sampler2D material_textures[];

// Material access via handle:
Material decode_material(MaterialHandle handle) {
    uint idx = handle.index * MATERIAL_STRIDE;
    // ... existing decode logic from material buffer
}
```

**Files to modify:**
- `shaders/rt_common.glsl`
- `shaders/pathtrace.rgen`
- `shaders/pathtrace.rahit`
- `shaders/pathtrace_shadow.rahit`
- `shaders/pathtrace.comp`
- `include/rtv/BindlessResources.h`
- `src/rtv/BindlessResources.cpp`
- `include/rtv/GpuScene.h`
- `src/rtv/GpuScene.cpp`
- `include/rtv/PathTracerRenderer.h`
- `src/rtv/PathTracerRenderer.cpp`

**Acceptance criteria:**
1. No hardcoded array sizes in shader descriptor bindings
2. Material handles work correctly with at least 256 unique materials
3. Mesh handles work correctly with at least 1024 unique meshes
4. Descriptor updates within frame budget (< 0.1ms)
5. Material editor hot-reload works without descriptor exhaustion
6. `FreeListAllocator<uint32_t>` reports fragmentation stats in debug panel

---

### Phase 6B: Compressed Textures via KTX2/Basis Universal (Effort: Medium, Impact: Medium)

**Problem**: Only RGBA8/16F textures. VRAM usage is 4-8x higher than necessary and loading is slow.

**Approach**: Use KTX2 + Basis Universal, not runtime BC7 encoding (slow and complex). GPU-decompressed, not `vkCmdBlitImage`.

**Files to modify:**
- `include/rtv/TextureLoader.h`
- `src/rtv/TextureLoader.cpp`
- `src/rtv/GpuScene.cpp`
- `CMakeLists.txt`

**Steps:**

1. **Integrate Basis Universal / KTX2**: Add `libktx` as a dependency (or embed a minimal transcoder).
   - KTX2 containers hold Basis-compressed textures.
   - Transcoder converts Basis to BC7/BC5/ETC2 at load time on the CPU (~2ms per 2K texture).

2. **Load path**:
   - Detect KTX2 by magic bytes or `.ktx2` extension.
   - Transcode to GPU format.
   - Upload to staging buffer, then GPU.

3. **Format selection**:
   - `VK_FORMAT_BC7_UNORM` for color maps.
   - `VK_FORMAT_BC5_UNORM` for normal maps.
   - `VK_FORMAT_BC3_UNORM` for combined maps (RGBA).

4. **Fallback**: Standard RGBA8 loading for non-KTX2 assets.

**Acceptance criteria:**
1. KTX2 textures load and display correctly
2. VRAM usage reduced by > 50% for texture-heavy scenes
3. Load time for KTX2 is competitive with RGBA8 (within 2x)
4. Fallback to RGBA8 works for non-KTX2 assets

---

### Phase 6C: Closure Material System (Effort: Large, Impact: Medium)

**Problem**: Material type enum (`mat_type = 0..5`) leads to uber-shader explosion. Scene instancing and transform updates are ad-hoc.

**Approach**: Move to composable closure-based materials with a fixed-size closure array as an intermediate step. A compact material DAG / indirect indexing can replace this in the long-term architecture v2.

```glsl
struct MaterialClosure {
    uint flags;          // bitfield: HAS_DIFFUSE, HAS_SPECULAR, HAS_SSS, HAS_TRANSMISSION, HAS_CLEARCOAT
    float weight;        // blend weight for layered closures
    vec3 color;
    float roughness;
    float metallic;
    float ior;
};

vec3 eval_brdf(MaterialClosure closures[4], vec3 wo, vec3 wi, vec3 n) {
    vec3 result = vec3(0.0);
    for (int i = 0; i < 4; ++i) {
        if (hasFlag(closures[i].flags, HAS_DIFFUSE))
            result += closures[i].weight * eval_lambert(closures[i], wo, wi, n);
        if (hasFlag(closures[i].flags, HAS_SPECULAR))
            result += closures[i].weight * eval_ggx(closures[i], wo, wi, n);
    }
    return result;
}
```

**Important — megakernel compatibility**: The closure iteration loop (`for i < 4`) and fixed-size closure array are designed to work with both megakernel (current) and wavefront (future) architectures. Do NOT introduce execution-dependent control flow or GPU-side dynamic allocation that would require wavefront scheduling. The closure dispatch path must allow per-material specialization later without changing the shader interface.

**Files to modify:**
- `shaders/rt_common.glsl`
- `shaders/pathtrace.rgen`
- `include/rtv/MeshAsset.h`
- `src/rtv/SceneToGpuSceneBuilder.cpp`

**Steps:**

1. Define bitfield flags for material features:
   - `MAT_DIFFUSE`, `MAT_SPECULAR`, `MAT_METAL`, `MAT_SSS`, `MAT_TRANSMISSION`, `MAT_CLEARCOAT`, `MAT_SHEEN`, `MAT_THIN_FILM`

2. Store material as a flat closure array (up to N=4 layers) instead of `mat_type`.

3. Update scene import pipeline to map glTF materials to closure representation.

4. Remove `mat_type` switch in shaders; replace with closure iteration.

**Future direction**: Fixed-size closure arrays are an intermediate step. The long-term direction is:
- Compact material DAG / material node graphs
- Indirect indexing per material
- Compressed feature masks

**Material classification buckets**: Closure iteration (`for i < 4`) interacts badly with SER, wavefront sorting, and occupancy once materials become heterogeneous. Long-term, material classification buckets (grouping similar materials for coherent execution) may outperform generic closure traversal. Not urgent, but the closure dispatch path should allow per-material specialization later.

**Lobe-oriented dispatch (future wavefront/SER compatibility)**: The current dispatch is material-ordered (iterate closures per material). Long-term, wavefront and SER benefit from lobe-ordered batching — group all diffuse closures together, all glossy closures together, etc. The closure storage layout must support this:
- Store closures as a flat array of `(flags, params)` tuples, NOT nested inside material structs
- Allow GPU-side sorting by `flags & LOBE_MASK` without changing the storage format
- Keep `eval_brdf()` as a pure function over individual closures, not material-aware

This does not change Phase 6C's implementation; it only constrains the data layout to prevent a future rewrite.

**Acceptance criteria:**
1. All existing materials render identically before and after closure conversion
2. Closure array iteration produces same results as `mat_type` switch
3. Material editor shows closure flags per layer
4. glTF import correctly maps PBR materials to closure representation

---

### Phase 6D: Render Graph — Transient Resources (Effort: Medium, Impact: High)

Adds memory optimization on top of the stable Stage 1A graph:

- Lifetime analysis (first-use and last-use pass index per resource)
- Transient image pooling (sub-allocate from large `VkDeviceMemory` block)
- Buffer aliasing when lifetimes don't overlap

**Acceptance criteria:**
1. Transient resources correctly aliased (no overlap in lifetimes)
2. Memory usage reduced for intermediate targets
3. No new synchronization bugs from aliasing
4. Memory pool fragmentation stats visible in debug panel

---

### Phase 7A: Render Graph — Async Scheduling (Effort: Medium, Impact: Medium)

**Do NOT begin until Phases A–E are stable.**

Partition graph into compute vs graphics sub-graphs with timeline semaphore dependencies.

- The graph compiler automatically splits the submission
- Same-family queues preferred before cross-family transfers
- `--single-queue-fallback` flag for deterministic debugging

**Acceptance criteria:**
1. Graph correctly partitions into compute/graphics sub-graphs
2. Timeline semaphore synchronization correct (no hazards)
3. Same-family queue preferred when available
4. Fallback mode produces identical output to non-async mode

---

### Phase 7B: Async Compute Overlap (Effort: Large, Impact: Medium)

**Problem**: All passes run sequentially on the graphics queue. Denoising + tone-mapping + histogram are pure compute and could overlap.

**Dependency**: Requires Phase 7A (render graph async partitioning).

**Important**: Prefer same-family async compute first before cross-family queue transfers. If the graphics queue and compute queue share the same queue family, ownership transfer may be unnecessary and much cheaper.

**Files to modify:**
- `include/rtv/VulkanContext.h`
- `src/rtv/VulkanContext.cpp`
- `include/rtv/RenderGraph.h`
- `src/rtv/RenderGraph.cpp`
- `include/rtv/CommandSystem.h`
- `src/rtv/CommandSystem.cpp`

**Steps:**

1. **Create a compute queue** in `VulkanContext`:
   - Request `VK_QUEUE_COMPUTE_BIT` with a separate queue family or from the same family.
   - Prefer same-family queues to avoid ownership transfer overhead.
   - Expose `VkQueue computeQueue()` and `uint32_t computeQueueFamilyIndex()`.

2. **Timeline semaphore for cross-queue sync** (only needed for cross-family):
   - Create `VkSemaphore timelineSemaphore_` with `VK_SEMAPHORE_TYPE_TIMELINE`.
   - Graphics queue signals timeline value N after path tracing.
   - Compute queue waits on timeline value N, then runs denoise + tone map, signals N+1.
   - Graphics queue waits on timeline value N+1 before presentation.

3. **Render graph integration**:
   - The graph partitions into two sub-graphs: graphics (path trace) and compute (denoise + post).
   - Edges between sub-graphs carry timeline semaphore dependencies.
   - The graph compiler automatically splits the submission.

4. **Buffer ownership management** (cross-family only):
   - Raw path trace output transitions via queue ownership transfer.
   - `VkImageMemoryBarrier2` with `srcQueueFamilyIndex` / `dstQueueFamilyIndex`.

**Important caveat**: Profile first with `GpuProfiler` to confirm the GPU has idle compute capacity during path tracing. Add pipeline statistics queries before implementing async.

**Deterministic replay / debug mode**: Async scheduling bugs become extremely nondeterministic once queue overlap, transient aliasing, and resource hazards interact. Ensure `--disable-async`, `--single-queue-fallback` exist for deterministic repro.

**Acceptance criteria:**
1. Compute queue creation succeeds with fallback to graphics-only
2. GPU profiler shows overlapping compute during path trace
3. No synchronization hazards (validated with Vulkan validation layers)
4. Total frame time reduced by at least 10% on GPU-bound scenes
5. `--single-queue-fallback` produces identical output to async mode

---

### Phase 7C: Wavefront Path Tracing (Effort: Very Large, Impact: High)

**Problem**: Monolithic raygen kernel wastes occupancy on divergence. Not recommended until all temporal + scene infrastructure is stable.

**Strategy**: Start with megakernel split before full queue-based wavefront.

### Megakernel Split

1. Primary ray generation + trace (compute)
2. Shade + accumulate + decide bounce type (compute)
3. Bounce trace (compute + RT)
4. Loop 2-3 until convergence

### Full Wavefront

```
Stage buffers: rayQueue (atomic counter), hitData, shadowQueue
1. generate:    write primary rays to rayQueue
2. trace:       for each ray in rayQueue, traceRayEXT, write hitData
3. shade:       for each hit, evaluate material, accumulate, maybe enqueue bounce
4. continue:    loop 2-3 until all rays terminate or max bounces
```

**New files:**
- `shaders/wavefront_generate.comp`
- `shaders/wavefront_trace.comp`
- `shaders/wavefront_shade.comp`

**Note**: Hybrid compute+RT wavefronts can become synchronization-heavy. Hardware RT intersections with software queue orchestration may be preferable long-term over fully splitting every stage.

**Architecture fork — not a feature**: Wavefront fundamentally changes scheduling, memory ownership, debugging, queue structure, material execution, synchronization, and occupancy strategy. Treat it as an architecture v2 fork, not an incremental feature. Verify that all prior phases avoided hard-coded megakernel assumptions:

- **Material dispatch** (Phase 6C): closure iteration loop is wavefront-compatible
- **BSDF evaluation** (Phase 5A): no execution-dependent divergence assumptions
- **Ray payload layout**: fixed-size, no dynamic allocation
- **Queue ownership** (Phase 7B): already supports async queues
- **Shader interfaces**: bindless descriptors (Phase 6A) already dynamic

**Queue memory bandwidth**: Wavefront renderers often become memory movement systems rather than compute systems. Ray queues, hit queues, shadow queues, and reservoir buffers all compete for bandwidth. Scalable wavefront rendering requires queue compaction, sorting, ray bucketing, and material bucketing to prevent memory traffic from dominating frame time. Budget implementation time accordingly — queue management is the real difficulty, not kernel splitting.

**Wavefront scheduling complexity**: Beyond queue memory, wavefront renderers face queue starvation, stage imbalance (generate vs trace vs shade run at different speeds), divergent bounce depth, and shadow queue amplification. Dynamic work stealing, queue balancing, and occupancy feedback loops are needed to keep all stages fed. This is substantially harder than the initial megakernel split suggests.

**Frame allocators for queue memory**: Wavefront systems create many transient queues, staging buffers, and synchronization metadata per frame. General-purpose allocation leads to severe allocator fragmentation. Frame allocators or arena-based queue allocators (where all per-frame queue memory is freed in one batch at the end of the frame) become necessary for stable performance.

**Acceptance criteria:**
1. Megakernel split produces identical output to monolithic raygen
2. Wavefront mode compiles and runs without synchronization crashes
3. Occupancy improves by > 20% on divergent scenes
4. Queue memory bandwidth is within 2x of megakernel mode
5. Frame allocators show no fragmentation growth over 1000 frames

---

### Phase 7D: SER / GPU-Driven Rendering (Effort: Very Large, Impact: High)

**Shader Execution Reordering (SER)**:
- `VK_NV_ray_tracing_invocation_reorder` or vendor equivalents can substantially improve occupancy for:
  - Glossy rays (high divergence)
  - Secondary bounces
  - ReSTIR workloads (divergent shadow rays)
- Integrate after wavefront is stable and occupancy bottlenecks are identified through profiling.

**GPU-Driven Rendering**:
- The renderer is currently CPU-orchestrated (CPU loops over instances, CPU dispatch calls).
- Long-term direction: move toward GPU-driven pipeline:
  - **GPU scene compaction**: dead-instance removal on GPU
  - **Indirect dispatch generation**: GPU computes dispatch sizes
  - **GPU visibility**: frustum/occlusion culling on GPU
  - **GPU-driven material sorting**: sort shader permutations on GPU
  - **GPU-generated ray queues**: wavefront path tracing queues managed entirely on GPU
- Fits naturally after wavefront path tracing (Phase 7C), as wavefront already pushes toward GPU work management.

**Acceptance criteria:**
1. SER improves occupancy by measurable amount on glossy-heavy scenes
2. GPU-driven indirect dispatch works for at least one render pass
3. CPU submission overhead reduced by moving dispatch size computation to GPU

---

## Automated Regression Testing

As the renderer becomes temporally accumulated, async, nondeterministic, and GPU-driven, visual regressions become subtle — often appearing weeks later only on specific GPUs or after unrelated optimizations.

### Approach

- **Image diff tests**: Compare render output against reference images per scene, per configuration. Use perceptual diff (SSIM/LPIPS) not per-pixel RGB. SSIM catches structural degradation; LPIPS catches perceptual shifts that SSIM misses.
- **Reference image management**: Reference images are stored in the repository under `tests/references/`. Each reference is tied to a specific commit hash + renderer configuration. When a deliberate visual change is made (new feature, quality improvement), references are regenerated via a CI-managed process.
- **Deterministic replay captures**: Record frame inputs + graph topology as a binary capture file. Replay deterministically for regression detection. Uses `RendererDebugCapture` (see below) as the serialization format.
- **Temporal stability tests**: Measure per-pixel variance across 120+ consecutive frames. Regressions in temporal systems (TAA, denoiser, ReSTIR) show as increased flicker, which SSIM across time windows detects.
- **Performance regression tracking**: GPU timestamp history per pass, tracked across builds. Alert when any pass exceeds 1.5× its baseline.

### When Tests Run

- **Per-phase gates**: Full regression suite runs when a phase's acceptance criteria are evaluated. This is NOT per-commit — it's too expensive. Per-commit: compile + unit tests only.
- **Pre-release**: Full suite runs before any tagged release.
- **Triggered by**: Phase transition evaluation, major refactoring, upstream dependency changes.

### Integration

The validation scene suite (from Phase 1B) serves as the test corpus. Each scene encodes a specific feature test:
- Scene 1: Basic geometry + lighting (regression catch-all)
- Scene 2: Temporal reprojection (velocity, disocclusion)
- Scene 3: Atmosphere + aerial perspective
- Scene 4: ReSTIR + many-light
- Scene 5: Denoiser + TAA stability

---

## RendererDebugCapture

Debugging temporal systems — TAA ghosting, ReSTIR instability, denoiser failure, atmosphere flicker — requires deterministic replay. A `RendererDebugCapture` object serializes all mutable renderer state at a given frame.

### Capture Contents

```cpp
struct RendererDebugCapture {
    // Identity
    uint32_t magic = 0x52445443;  // "RDTC"
    uint32_t version = 1;
    char scenePath[256];
    uint64_t frameIndex;

    // Camera
    glm::vec3 cameraPosition;
    glm::vec3 cameraTarget;
    glm::vec3 cameraUp;
    float cameraFov;

    // Renderer settings (all enums, bools, floats)
    uint32_t maxBounces;
    uint32_t environmentSamples;
    float resolutionScale;
    bool pathTracingEnabled;
    bool directLightingEnabled;
    float indirectStrength;
    // ... all other settings from RendererSettings ...

    // Temporal state
    uint32_t accumulationCount;
    uint32_t jitterPhase;
    uint32_t temporalHistoryAge;
    glm::vec2 jitterOffset;

    // Atmosphere
    AtmosphereParams atmosphereParams;
    float sunAzimuth;
    float sunElevation;
    AtmosphereQuality atmosphereQuality;

    // Random seeds (for deterministic RNG replay)
    uint32_t frameSeed;
    uint32_t blueNoiseTextureIndex;

    // Graph topology (compact)
    // Array of pass IDs + barrier state at capture time
};
```

### Usage

- **Manual capture**: Press a hotkey (e.g., F11) to dump the current frame's `RendererDebugCapture` to disk.
- **Auto-capture on error**: If a validation error or GPU hang is detected, dump the last N frames' captures.
- **Replay**: Load a capture file, bypass the normal scene loading, and replay the frame's graph with the exact same inputs. The renderer's RNG is seeded from `frameSeed` and `blueNoiseTextureIndex`, producing identical sampling patterns.
- **Bug report attachment**: Users can attach a capture file to bug reports. The developer replays locally with identical output.

### Ownership

`RendererDebugCapture` is a debug tool, not a renderer feature. It lives in the debug utility layer (same as GPU debug visualization, profiler panels). It has zero runtime cost when capture is not active.

---

## Long-term Items

### Participating Media
- Heterogeneous volume integration with delta-tracking
- Woodcock tracking for free-path sampling
- Phase function MIS integration
- Requires atmosphere (Stage 4) and material system (Phase 6C) first
- **Complexity escalates rapidly** — phase functions change renderer architecture

### ReSTIR GI
- Extends ReSTIR from direct to indirect lighting
- Requires stable temporal infrastructure (Stage 2) and ReSTIR DI (Phase 5B)
- Reservoir-based indirect sample reuse

### Compact Material DAG
- Replace fixed-size closure arrays with compact material graphs
- Indirect indexing per material, compressed feature masks
- More scalable long-term than closure[4]

### Disk Pipeline Cache
- On creation, read `.pipeline_cache` file from build directory.
- On destruction, write it back.
- Low effort, incremental improvement.

### Hot Shader Reload
- Track `std::filesystem::file_time_type` per shader source.
- Each frame, check modification times of shader files.
- If any changed, call `reloadShadersFromEditor()`.

### Profiler Pipeline Statistics
- Add `VK_QUERY_TYPE_PIPELINE_STATISTICS` query pool.
- Track: ray count, triangle intersection tests, BVH traversal steps.
- Display in `DebugProfilerPanel`.

### Crash Recovery / Validation Infrastructure

Beyond debugging visualization, the renderer needs failure isolation:

- **GPU validation toggles**: Enable/disable individual validation layers at runtime
- **Fallback synchronization modes**: `--safe-barriers` (full barriers everywhere) for isolating sync bugs
- **Replay capture**: Capture a frame's graph topology + resource state to a file for deterministic replay
- **Crash repro dumps**: On GPU hang/crash, dump the current graph state, last N timestamps, and pending barriers
- **Pass isolation modes**: Run any single pass in isolation (bypass graph, use pre-initialized inputs) for repro

As async compute + transient aliasing + wavefront grow, bugs become extremely difficult to reproduce. This infrastructure is the long-term safety net.

### Regression Testing Infrastructure

As the renderer becomes temporally accumulated, async, nondeterministic, and GPU-driven, visual regressions become subtle — often appearing weeks later only on specific GPUs or after unrelated optimizations. The renderer needs:

- **Image diff tests**: Compare render output against reference images per scene, per configuration
- **Deterministic replay captures**: Record frame inputs + graph topology, replay deterministically for regression detection
- **Temporal stability tests**: Measure per-pixel variance across consecutive frames — regressions in temporal systems show as increased flicker
- **Performance regression tracking**: GPU timestamp history per pass across builds
- **Validation scenes**: Small synthetic scenes that exercise specific features (MIS, ReSTIR, velocity, TAA) in isolation

This is where mature renderer projects invest heavily — without it, every optimization risks undetectable visual degradation.

---

## Acceptance Criteria Standard

Every phase must meet its acceptance criteria before the next phase begins. The following framework applies across all phases:

**Correctness gates:**
- No regression in existing features (all debug views still work, all scenes load)
- No new validation layer errors or warnings
- Accumulation converges to stable result within expected sample count

**Performance gates:**
- Frame time does not regress by more than 5% for existing features
- New GPU passes complete within their budget (specified per phase)
- VRAM usage does not regress

**Debugging gates:**
- Each phase ships at least one debug visualization for its core data
- GPU profiler tracks all new passes
- Validation markers exist for all new GPU work

**Integration gates:**
- New system is consumed by all downstream systems (no dead code paths)
- Old code paths are removed (not ifdef-gated)
- Documentation is updated to reflect new architecture

---

## Final Priority Order

| Phase | Stage | Item | Effort | Impact | Why At This Position |
|-------|-------|------|--------|--------|---------------------|
| A | 0 | AS Refit | Medium | High | Unblocks interactive editing immediately |
| A | 0B | Editor Quick Wins | Small | Medium | No renderer dependency, immediate value |
| A | 1A | Render Graph — Barriers | Very Large | Architectural | Prerequisite for all scheduling work |
| A | 1B | GPU Debugging + Validation Scenes | Medium | High | Observability before temporal work |
| A | 1C | SceneUpdateRouter | Medium | Architectural | Prevents FullSceneRebuild fallthrough |
| B | 2A | Velocity + Camera Jitter | Large | High | Foundation for all temporal systems |
| B | 2B | Temporal Layer — Core | Medium | Architectural | Shared reprojection, history, camera-cut |
| B | 2C | Temporal Layer — Extensions | Medium | High | Shared confidence, variance, rejection |
| B | 2D | TAA | Medium | High | Jitter without TAA is incomplete |
| B | 2E | Denoiser Hardening | Medium | High | Consumes shared temporal layer |
| C | 3A-D | Editor Responsiveness | Medium | High | Now safe — partial updates exist |
| D | 4A | Atmosphere + Height Fog | Medium | High | Requires stable temporal + HDR |
| E | 5A | Specular MIS | Medium | Medium | Fixes noisy glossy + energy loss |
| E | 5B | ReSTIR DI (Hybrid) | Very Large | High | Depends on temporal infra + TAA + denoiser |
| E | 5C | Light BVH / RIS | Large | High | Enables many-light ReSTIR scaling |
| R | 6A | Full Bindless Descriptors | Large | High | Scalability — not needed for temporal/atmosphere |
| R | 6B | Compressed Textures | Medium | Medium | Clear VRAM/load win, independent |
| R | 6C | Material Closure System | Large | Medium | Prevents uber-shader explosion |
| R | 6D | Render Graph — Transient | Medium | High | Memory optimization on stable graph |
| R | 7A | Render Graph — Async | Medium | Medium | Gated on profiling data |
| R | 7B | Async Compute | Large | Medium | Gated on render graph async |
| R | 7C | Wavefront Path Tracing | Very Large | High | Last — architecture v2 fork |
| R | 7D | SER / GPU-Driven | Very Large | High | After wavefront is stable |

**Phase key**: A = Foundation, B = Temporal, C = Editor, D = Atmosphere, E = Lighting, R = Research Track

---

## Effort Summary

| Effort | Phases | Total Estimated Work |
|--------|--------|---------------------|
| Small | 0B | ~1 week |
| Medium | 0, 1B, 1C, 2B, 2C, 2D, 2E, 3A-D, 4A, 5A, 6B, 6D, 7A | ~10-14 weeks |
| Large | 2A, 5C, 6A, 6C, 7B | ~8-12 weeks |
| Very Large | 1A, 5B, 7C, 7D | ~14-22 weeks |
| **Total** | **24 items across 5 phases + research track** | **~33-49 weeks** |
