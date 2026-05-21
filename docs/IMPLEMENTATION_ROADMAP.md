# Implementation Roadmap

This file ties together three plans — [Renderer](IMPROVEMENT_PLAN.md), [Atmosphere](SKY_ATMOSPHERE_SYSTEM.md), and [Editor UX](UX_IMPROVEMENT_PLAN.md) — into a single executable sequence. Each phase below maps tasks from all three documents behind a single gate.

## Build Policy

Every phase transition must pass the [Build Policy](IMPROVEMENT_PLAN.md#build-policy) gates:

- **Debugged**: no known correctness bugs in common cases
- **Profiler-verified**: within 1.5x expected GPU cost
- **Validation-scene-passing**: all scenes render without artifacts
- **Accumulation-stable**: no flicker/ghosting across 120+ frames
- **GPU-capture-clean**: no validation errors or barrier warnings

No phase begins until the previous phase passes all five gates.

---

## Phase A — Renderer Foundation

**Goal**: Stable rendering core with interactive editing, observability, and correct scene mutation.

**Plans involved**: Renderer Stage 0 + Stage 1, UX Phase 1

### Tasks

| Plan | Tasks | Effort |
|------|-------|--------|
| Renderer | Stage 0: AS Refit (BLAS/TLAS refit, geometry classification, EditorRequests) | Medium |
| Renderer | Stage 1A: Render Graph — dependency graph + barriers only | Very Large |
| Renderer | Stage 1B: GPU Debugging Foundation + 5 validation scenes | Medium |
| Renderer | Stage 1C: SceneUpdateRouter (replace FullSceneRebuild) | Medium |
| UX | Phase 1: Collapse render settings, tooltips, dock persistence, dirty title, shortcut labels | Small |

### Acceptance Criteria

- AS refit: transform edits update TLAS without `vkDeviceWaitIdle`, accumulation preserved
- Render graph: all existing passes migrated, barriers correct, no new validation errors
- GPU debug: all 5 validation scenes render cleanly, pass timestamps visible, resource state viz works
- SceneUpdateRouter: `FullSceneRebuild` is unreachable (compile error if hit)
- UX: settings panel is navigable, dock layout persists across launches

### Dependencies

- **Internal**: AS refit can parallelize with UX Phase 1 (no renderer dependency)
- **To Phase B**: Render graph + SceneUpdateRouter + GPU debug must pass Build Policy gates

---

## Phase B — Temporal Stability

**Goal**: Per-frame velocity, shared temporal layer, TAA, hardened denoiser. Enables temporal accumulation that does not flicker.

**Plans involved**: Renderer Stage 2, UX Phase 1 (continues)

### Tasks

| Plan | Tasks | Effort |
|------|-------|--------|
| Renderer | Stage 2A: Per-instance velocity buffers + camera jitter | Large |
| Renderer | Stage 2B: Shared Temporal Layer — core (reprojection, history, camera-cut) | Medium |
| Renderer | Stage 2C: Shared Temporal Layer — extensions (confidence, variance, rejection) | Medium |
| Renderer | Stage 2D: TAA / Temporal Super Resolution | Medium |
| Renderer | Stage 2E: Denoiser hardening (uses shared temporal layer) | Medium |

### Acceptance Criteria

- Velocity: per-instance motion vectors correct in all 5 validation scenes, no off-by-one in temporal reprojection
- Temporal core: history is valid after camera cut, reprojection sub-pixel accurate
- TAA: no ghosting on static geometry, no jitter shimmer, edges resolve within 8 frames
- Denoiser: no over-blurring on moving geometry, no temporal instability during camera motion
- **Temporal stability test** (120-frame variance): per-pixel variance < 0.5% for static scenes

### Dependencies

- **From Phase A**: Render graph, SceneUpdateRouter, GPU debugging must be stable
- **To Phase C**: Temporal stability gates must pass

---

## Phase C — Editor Responsiveness

**Goal**: Usable editor with visibility, snapping, undo/redo, notifications, diagnostics.

**Plans involved**: Renderer Stage 3, UX Phase 2

### Tasks

| Plan | Tasks | Effort |
|------|-------|--------|
| Renderer | Stage 3A: Visibility/lock toggles routed through SceneUpdateRouter | Medium |
| Renderer | Stage 3B: Undo/redo system (ICommand, batching for gizmo drags) | Large |
| Renderer | Stage 3C: Snapping, notifications, progress indicators | Medium |
| Renderer | Stage 3D: Transform utilities, stats overlay, diagnostics panel | Medium |
| UX | Phase 2: Hierarchy context menus, search/filter, selection stats, notification center | Medium |

### Acceptance Criteria

- Visibility: toggling visibility does not reset accumulation or rebuild TLAS
- Undo: gizmo drags are undoable as a single batched command, undo stack survives scene mutations
- Snapping: grid snap, rotation snap, scale snap all correct with world/ local modes
- Notifications: non-blocking, auto-dismiss, no frame stalls
- UX: hierarchy search returns results in < 1 ms for 10,000 entities

### Dependencies

- **From Phase B**: Temporal stability must be passing
- **To Phase D**: Editor mutation paths must be correct and debugged

---

## Phase D — Atmosphere

**Goal**: Physically based sky with transmittance, sky-view LUT, analytical sun, exposure calibration, and aerial perspective fog.

**Plans involved**: Renderer Stage 4, Sky System Stages A–G

### Tasks

| Plan | Tasks | Effort |
|------|-------|--------|
| Sky | Stage A: Core (params, transmittance LUT, sky-view LUT, analytical sun, miss shader, exposure) | Medium |
| Sky | Stage B: Quality (multi-scatter LUT, tone map improvements, sunset validation) | Medium |
| Sky | Stage C: Direct lighting (sun transmittance, delta-light, shadows) | Medium |
| Sky | Stage D: Depth (analytic fog first, then aerial perspective LUT) | Medium |
| Sky | Stage E: Environment importance sampling (CDF, MIS — no blue noise) | Medium |
| Sky | Stage F: Temporal stability (simple reprojection + lerp blend) | Medium |
| Sky | Stage G: Future systems (ray tiers, fast path, ground bounce, blue noise, classification) | Small |
| Renderer | Stage 4A: Atmosphere integration into path tracer + exposure system | Medium |

### Acceptance Criteria

- Stage A: sky renders with correct Rayleigh/Mie, sun disk sharp, no double-counting
- Stage B: multi-scatter brightens sky correctly, sunset horizon warm, no double-counting
- Stage C: sun light attenuated through atmosphere at sunset, shadows show correct tint
- Stage D: analytic fog works first, then LUT matches analytic visually — validates parameterization
- Stage E: environment importance converges faster than uniform, MIS weights correct
- Stage F: sky stable under camera rotation, no ghosting
- Stage G: ray quality tiers produce no visible seams, fast path energy matches LUT path within 5%

### Dependencies

- **From Phase C**: Render graph + temporal stability + editor mutations must be stable
- Sky Stages are strictly ordered per the [Sky System Implementation Order](SKY_ATMOSPHERE_SYSTEM.md#implementation-order)
- **To Phase E**: Atmosphere must pass Build Policy gates

---

## Phase E — Lighting Scalability

**Goal**: Correct specular MIS, ReSTIR direct illumination, light BVH for many-light support.

**Plans involved**: Renderer Stage 5

### Tasks

| Plan | Tasks | Effort |
|------|-------|--------|
| Renderer | Stage 5A: Specular MIS + BSDF improvements | Medium |
| Renderer | Stage 5B: ReSTIR DI (hybrid mode — classic NEE fallback + split-screen debug) | Very Large |
| Renderer | Stage 5C: Light BVH / RIS acceleration | Large |

### Acceptance Criteria

- Specular MIS: glossy reflections match reference (no energy loss at BSDF/environment boundary)
- ReSTIR: hybrid mode produces visually identical results to classic NEE within 2% RMS
- Light BVH: many-light scenes (500+ lights) render within 2x frame time of single-light scenes
- All validation scenes continue to pass

### Dependencies

- **From Phase D**: Atmosphere + temporal infrastructure must be passing Build Policy gates
- **To Research Track**: Phase E is the last required phase before architecture v2 forks

---

## Research Track (Deferred)

**Do NOT begin until Phases A–E pass all Build Policy gates.**

| Plan | Tasks | Effort | Risk |
|------|-------|--------|------|
| Renderer | Stage 6A: Full bindless descriptors | Large | High — changes descriptor management across all shaders |
| Renderer | Stage 6B: Compressed textures (KTX2/Basis) | Medium | Low — independent subsystem |
| Renderer | Stage 6C: Closure material system | Large | High — changes material execution model |
| Renderer | Stage 6D: Render graph — transient resources | Medium | Medium — requires stable graph topology |
| Renderer | Stage 7A: Render graph — async scheduling | Medium | High — changes queue management |
| Renderer | Stage 7B: Async compute overlap | Large | High — GPU scheduling complexity |
| Renderer | Stage 7C: Wavefront path tracing | Very Large | Very High — architecture v2, changes scheduling/memory/queues |
| Renderer | Stage 7D: SER / GPU-driven rendering | Very Large | Very High — after wavefront is stable |
| Sky | Future: Volumetric clouds | Very Large | High — subsystem equal to atmosphere in scope |

These phases are architecture v2 forks. They change scheduling, memory ownership, queue structure, material execution, and descriptor management. Implementing any of them while the megakernel renderer is still in flux guarantees permanent instability.

---

## Cross-Plan Dependency Graph

```
Phase A (Foundation)
  ├── Renderer Stage 0  ──►  UX Phase 1 (independent)
  └── Renderer Stage 1  ──►  UX Phase 1 (parallel)
        ↓
Phase B (Temporal)
  └── Renderer Stage 2
        ↓
Phase C (Editor)
  ├── Renderer Stage 3
  └── UX Phase 2
        ↓
Phase D (Atmosphere)
  ├── Renderer Stage 4
  └── Sky System Stages A–G
        ↓
Phase E (Lighting)
  └── Renderer Stage 5
        ↓
Research Track (deferred)
  ├── Renderer Stage 6
  ├── Renderer Stage 7
  └── Sky: Volumetric clouds
```

Each arrow means "gated on Build Policy acceptance criteria of the upstream phase."

---

## Effort Summary

| Phase | Total Estimated Work | Notes |
|-------|---------------------|-------|
| A — Foundation | ~8-12 weeks | Largest single phase (render graph) |
| B — Temporal | ~6-10 weeks | Shared temporal layer reduces later work |
| C — Editor | ~4-6 weeks | Only after scene mutation is correct |
| D — Atmosphere | ~6-8 weeks | 7 sub-stages (A–G), each independently verifiable |
| E — Lighting | ~8-14 weeks | ReSTIR dominates |
| Research Track | ~14-22 weeks | Do not start until A–E are stable |
| **Total** | **~46-72 weeks** | Not sequential — overlap within phases is allowed |

**Acceptance Criteria Standard**: See [IMPROVEMENT_PLAN.md](IMPROVEMENT_PLAN.md#acceptance-criteria-standard) for the framework that applies across all phases and plans.
