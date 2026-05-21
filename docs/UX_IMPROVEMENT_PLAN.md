<!-- open-code-summary-anchor -->
# UI/UX Improvement Plan

## Overview

This document outlines targeted improvements to the editor UI/UX, organized by effort and grouped into implementation phases. Each item references the exact source files and provides concrete implementation guidance.

**Key design principles:**
- Deferred requests (`EditorRequests`) instead of direct mutation inside panels — enables undo, thread safety, future command system
- Undo/redo deliberately deferred to Phase 3 until editor mutation paths are stable
- Per-project layout profiles for dock persistence
- Best-quality defaults for entity duplication (deep copy: shared mesh + cloned transform + cloned material instances)
- Polymorphic `ICommand` undo system (not enum+union) with **command batching** to avoid undo spam from gizmo drags
- **Multi-selection from Phase 1**: `EditorSelection` uses `std::vector<EntityId>` from the start, even before multi-select UI is fully built. This avoids rewriting selection state representation later.
- **Renderer phase gating**: Each editor phase depends on renderer stability. Phase 1 works alongside renderer Phase A (Foundation). Phase 2 requires renderer Phase B (Temporal Stability). Phase 3 requires renderer Phase C (Editor Responsiveness). See the [Build Policy](IMPROVEMENT_PLAN.md#build-policy) for phase transition gates.
- **Feature gravity awareness**: Each new system creates pressure for more systems (bookmarks → serialization → thumbnails → previews → metadata → scene states). Phase 4 polish must not begin until Phases 1–3 are verified stable under daily use. Feature freeze discipline is essential — stable scene interaction is the bottleneck, not professional presentation.

**Phases at a glance:**

| Phase | Focus | Renderer Gate |
|-------|-------|---------------|
| 1 — Quick Wins | Settings, tooltips, hierarchy, snapping, transform utils, context menus | Phase A (Foundation) |
| 2 — Core Workflow | Drag/drop, visibility, multi-select, selection outline | Phase B (Temporal) |
| 3 — Infrastructure | Undo/redo, stable entity IDs, scene versioning | Phase C (Editor) |
| 4 — Polish | Notifications, bookmarks, preferences, diagnostics, HUD | Phase C (Editor) |
| 5 — Large Systems | Asset browser v2, viewport overlays, add component | Phase C+ |

---

## Phase 1 — Quick Wins (immediate usability, no new data structures)

### 1.1 Collapse advanced Render Settings

**File:** `src/rtv/RenderSettingsPanel.cpp`

**Effort:** ~30 lines

Group the 40+ controls into collapsible sections. This is the single highest-value-per-line-changed item:

```cpp
// Keep at top (always visible):
//   Backend, Active Backend, Debug View
//   Max Bounces, Environment Samples, Resolution Scale
//   Path Tracing, Direct Lighting, Indirect Strength

ImGui::SeparatorText("Tone Mapping");
// tone mapper, exposure, auto-exposure toggle
if (ImGui::TreeNodeEx("Advanced", 0)) {
    // contrast, saturation, brightness, gamma, whitePoint
    // histogram min/max, low/high/target percentiles
    // min/max exposure, adaptation speed, target luminance
    ImGui::TreePop();
}

ImGui::SeparatorText("Lighting");
// sunlight toggle + intensity/size, sky intensity
// direct lighting toggle, indirect strength

ImGui::SeparatorText("Environment");
// enabled, intensity, rotation, background intensity

ImGui::SeparatorText("Denoiser");
// enabled, while-moving, iterations, strength
```

---

### 1.2 Tooltips on non-obvious controls

**Files:** `src/rtv/InspectorPanel.cpp`, `src/rtv/RenderSettingsPanel.cpp`, `src/rtv/MaterialEditorPanel.cpp`

**Effort:** ~40 lines

Use the delay-safe pattern (compatible with older ImGui):

```cpp
if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
    ImGui::SetTooltip("What this parameter does and typical range");
}
```

**Tooltip checklist** — only for non-obvious parameters:

| File | Control | Tooltip |
|------|---------|---------|
| `RenderSettingsPanel.cpp` | Max Bounces | "Number of ray bounces. Higher = more accurate, slower. 4-8 preview, 16+ final." |
| `RenderSettingsPanel.cpp` | Environment Samples | "Environment light samples per bounce. Higher reduces fireflies." |
| `RenderSettingsPanel.cpp` | Indirect Strength | "Multiplier for indirect lighting contribution." |
| `RenderSettingsPanel.cpp` | Exposure | "Overall brightness multiplier. Higher = brighter image." |
| `RenderSettingsPanel.cpp` | Histogram sliders | "Controls the auto-exposure metering region." |
| `RenderSettingsPanel.cpp` | A-trous Iterations | "Denoiser iterations. More = smoother but slower." |
| `RenderSettingsPanel.cpp` | Denoiser Strength | "Higher = more aggressive denoising, may lose detail." |
| `InspectorPanel.cpp` | Rotation (degrees) | "Euler angles in degrees." |
| `MaterialEditorPanel.cpp` | Metallic | "0 = dielectric, 1 = metal. Most materials are 0 or 1." |
| `MaterialEditorPanel.cpp` | Roughness | "0 = mirror, 1 = diffuse." |

For the most advanced controls, also add a `(?)` helper:

```cpp
ImGui::SameLine();
ImGui::TextDisabled("(?)");
if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip("Detailed explanation...");
}
```

---

### 1.3 Dock layout persistence (per-project profiles)

**Files:** `src/rtv/EditorDockspace.cpp`, `src/rtv/UiOverlay.cpp`

**Effort:** ~60 lines

**Current state:** ImGui already saves `rtv_editor.ini` globally. This works but mixes layout state across all projects.

**Goal:** Per-project layout profiles saved alongside the scene file, without globally disabling ImGui's ini persistence.

**Implementation:**

```cpp
// In EditorDockspace:
class EditorDockspace {
public:
    void setProfilePath(const std::filesystem::path& scenePath) {
        profilePath_ = scenePath;
        profilePath_.replace_extension(".layout.ini");
    }
    void saveLayout() {
        if (!profilePath_.empty())
            ImGui::SaveIniSettingsToDisk(profilePath_.string().c_str());
    }
    void loadLayout() {
        if (!profilePath_.empty() && std::filesystem::exists(profilePath_))
            ImGui::LoadIniSettingsFromDisk(profilePath_.string().c_str());
    }
    std::filesystem::path profilePath_;
};

// In File > Save Level / Load Level:
// Call saveLayout()/loadLayout() alongside the scene load/save.
// Do NOT globally set ImGui::GetIO().IniFilename = nullptr —
// let the global ini persist for non-project windows.
```

**Menu additions** (in File menu):
- "Save Layout" — saves current dock layout
- "Reset Layout" — resets to factory default

---

### 1.4 Delete confirmation (temporary scaffolding)

**File:** `src/rtv/InspectorPanel.cpp` (lines 240-252)

**Effort:** ~20 lines

Replace immediate delete with a popup:

```cpp
if (ImGui::Button("Delete Entity")) {
    ImGui::OpenPopup("Delete##confirm");
}
if (ImGui::BeginPopupModal("Delete##confirm", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
    ImGui::Text("Delete '%s'?", entity->name.c_str());
    if (ImGui::Button("Yes")) {
        // existing delete logic
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("No")) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}
```

**Note:** This is temporary. Once Undo exists in Phase 3, remove confirmation modals — modern editors rely on undo, not dialogs.

---

### 1.5 Dirty indicator in window title

**File:** `src/rtv/Application.cpp` (function `updateWindowTitle`, lines 900-919)

**Effort:** ~10 lines

```cpp
std::ostringstream title;
if (sceneDocument_.dirty()) {
    title << "[Modified] ";
}
title << (gltfPath_.has_value() ? gltfPath_->stem().string() : "Untitled")
      << " - Ray Tracing Engine"
      << " | samples " << pathTracer_->sampleCount()
      << " | GPU "
      << std::fixed << std::setprecision(2)
      << timings.pathTraceMs + timings.denoiserMs + timings.fullscreenMs << " ms";
```

---

### 1.6 Shortcut labels in menu items

**File:** `src/rtv/EditorDockspace.cpp` (lines 137-148)

**Effort:** ~10 lines

Append `\tKEY` for right-aligned shortcut display:

```cpp
if (ImGui::MenuItem("Reset Accumulation\tR"))
if (ImGui::MenuItem("Toggle Denoiser\tF2"))
if (ImGui::MenuItem("Cycle Debug View\tF1"))
```

Also rename "Toggle Debug View" → "Cycle Debug View".

---

### 1.7 Centralized keybindings + generated Help window

**File:** `src/rtv/EditorDockspace.cpp`, new `include/rtv/KeyBindings.h`

**Effort:** ~80 lines

**Create a keybinding table** so help and menus stay in sync:

```cpp
// include/rtv/KeyBindings.h
struct KeyBinding {
    std::string action;
    std::string key;
    std::string category;  // "Navigation", "Render", "Editing"
    std::string description;
};

const std::vector<KeyBinding>& allKeyBindings();
```

**Generate Help window** from this table:

```cpp
void EditorDockspace::drawHelpWindows() {
    // ...
    std::string currentCategory;
    for (const auto& kb : allKeyBindings()) {
        if (kb.category != currentCategory) {
            if (ImGui::TreeNodeEx(kb.category.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
                currentCategory = kb.category;
            } else {
                currentCategory.clear();
                continue;
            }
        }
        ImGui::BulletText("%s: %s", kb.key.c_str(), kb.description.c_str());
    }
}
```

This prevents help drift when shortcuts change.

---

### 1.8 Camera speed presets

**File:** `src/rtv/InspectorPanel.cpp` (Camera selection), `include/rtv/CameraController.h`

**Effort:** ~30 lines

Replace single speed slider with presets + custom:

```cpp
// In Inspector, when Camera selected:
const std::pair<const char*, float> presets[] = {
    {"Slow", 1.0f},
    {"Medium", 2.4f},
    {"Fast", 6.0f},
    {"Very Fast", 15.0f},
};

float speed = state.camera->moveSpeed();
ImGui::Text("Move Speed");

ImGui::PushItemWidth(-1);
for (auto& [label, val] : presets) {
    ImGui::SameLine();
    if (ImGui::RadioButton(label, std::abs(speed - val) < 0.01f)) {
        speed = val;
        requests.cameraMoveSpeed = speed;
    }
}
ImGui::PopItemWidth();

ImGui::SliderFloat("##speed_custom", &speed, 0.25f, 20.0f, "Custom: %.2f");
if (ImGui::IsItemDeactivatedAfterEdit()) {
    requests.cameraMoveSpeed = speed;
}
```

---

### 1.9 Right-click context menus in Scene Hierarchy

**File:** `src/rtv/SceneHierarchyPanel.cpp` (function `drawEntityNode`)

**Effort:** ~100 lines

```cpp
const bool open = ImGui::TreeNodeEx(label.c_str(), flags);

if (ImGui::BeginPopupContextItem()) {
    if (ImGui::MenuItem("Duplicate")) {
        // queue via EditorRequests
    }
    if (ImGui::MenuItem("Delete")) {
        // queue via EditorRequests
    }
    if (ImGui::MenuItem("Rename")) {
        ImGui::SetKeyboardFocusHere(-1); // focus name input
    }
    if (ImGui::BeginMenu("Create Child")) {
        if (ImGui::MenuItem("Empty")) { /* create child entity */ }
        if (ImGui::MenuItem("Camera")) { /* create child camera */ }
        if (ImGui::MenuItem("Light")) { /* create child light */ }
        ImGui::EndMenu();
    }
    if (entity->parent.valid() && ImGui::MenuItem("Detach Parent")) {
        // unlink from parent
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Focus in Viewport")) {
        // move camera to entity
    }
    ImGui::EndPopup();
}
```

**New `EditorRequests` fields:**
```cpp
std::optional<EntityId> duplicateEntity;
std::optional<EntityId> deleteEntity;
std::optional<EntityId> focusOnEntity;
std::optional<std::pair<EntityId, EntityId>> reparentEntity; // child, newParent
```

**Duplicate policy** (best quality):
- Mesh: shared reference (no GPU copy)
- Material instances: **cloned** (deep copy)
- Transform: copied
- Children: recursive deep copy
- If entity has a parent, place as sibling under same parent

Process these in `Application::applyEditorRequests()`.

---

### 1.10 Entity search/filter in Scene Hierarchy

**File:** `src/rtv/SceneHierarchyPanel.cpp`

**Effort:** ~60 lines

```cpp
// At top of draw(), before create buttons:
static char filterBuffer[128] = "";
ImGui::InputTextWithHint("##filter", "Filter entities...",
    filterBuffer, sizeof(filterBuffer));
ImGui::Separator();
```

In `drawEntityNode()`, skip invisible nodes:

```cpp
if (filterBuffer[0] != '\0') {
    std::string filter = filterBuffer;
    std::transform(filter.begin(), filter.end(), filter.begin(), ::tolower);
    std::string name = entity.name;
    std::transform(name.begin(), name.end(), name.begin(), ::tolower);
    if (name.find(filter) == std::string::npos) {
        bool childMatches = false;
        for (EntityId childId : entity.children) {
            if (entityContainsName(registry, childId, filter))
                { childMatches = true; break; }
        }
        if (!childMatches) return; // skip this node
    }
}
```

**Future optimization** (not needed yet): cache lowercase names, flatten hierarchy for faster recursive search.

---

### 1.11 Gizmo snapping

**File:** `src/rtv/ViewportPanel.cpp` (lines 328-343)

**Effort:** ~60 lines

Add snap state + UI:

```cpp
// In ViewportPanel class:
struct SnapSettings {
    bool enabled = false;
    float translation = 0.25f;  // grid units
    float rotation = 15.0f;     // degrees (ImGuizmo expects degrees for rotation snap)
    float scale = 0.1f;         // factor
};
SnapSettings snap_;

// In draw(), next to gizmo mode buttons:
ImGui::SameLine();
if (ImGui::Checkbox("Snap", &snap_.enabled)) { }

// In ImGuizmo::Manipulate call:
float snapValues[3] = {};
if (snap_.enabled) {
    switch (transformGizmoMode_) {
        case 0: // translate
            snapValues[0] = snapValues[1] = snapValues[2] = snap_.translation;
            break;
        case 1: // rotate
            snapValues[0] = snapValues[1] = snapValues[2] = snap_.rotation; // degrees
            break;
        case 2: // scale
            snapValues[0] = snapValues[1] = snapValues[2] = snap_.scale;
            break;
    }
}
const bool manipulated = ImGuizmo::Manipulate(
    glm::value_ptr(view),
    glm::value_ptr(projection),
    operation,
    localGizmoMode_ ? ImGuizmo::LOCAL : ImGuizmo::WORLD,
    glm::value_ptr(world),
    snap_.enabled ? snapValues : nullptr);  // <-- pass snap values
```

**Important:** ImGuizmo's rotation snap expects degrees (not radians), so pass the raw degree value.

#### Gizmo Interaction State Machine

Once snapping, multi-select, undo batching, local/world space, viewport capture, and locking all coexist, the gizmo needs an explicit interaction state machine to coordinate them:

```cpp
enum class GizmoInteractionState {
    Idle,
    Hovered,
    DraggingTranslate,
    DraggingRotate,
    DraggingScale,
};

GizmoInteractionState gizmoState_ = GizmoInteractionState::Idle;
```

**State transitions:**

| From | To | Trigger | Side effects |
|------|----|---------|--------------|
| Idle | Hovered | Mouse over gizmo handle | Update cursor, show tooltip |
| Hovered | Idle | Mouse leaves gizmo | Hide tooltip |
| Hovered | Dragging* | Mouse down on handle | Capture original transform (for undo), pause accumulation |
| Dragging* | Idle | Mouse release | Commit single undo command, resume accumulation |
| Any | Any | Entity locked | Force to Idle, disable gizmo |

**Why an explicit state machine is necessary:**

- **Undo batching**: Capture `originalTransform` on `Idle → Dragging` transition, not per mouse-delta. This guarantees exactly one undo step per drag, regardless of duration.
- **Accumulation pause**: Pause accumulation during `Dragging*` states. Resume on return to `Idle`. The `IsItemDeactivatedAfterEdit()` check in ImGui is not sufficient — it fires after the frame releases, not when the drag visually ends.
- **Locked entities**: If the selected entity has `locked = true`, the gizmo must not enter `Hovered` or `Dragging*` states. Check on every transition.
- **Multi-select transforms**: In multi-select mode, a drag translates/rotates/scales all selected entities relative to their individual origins. The state machine remains the same — only the apply step iterates over the selection.
- **Snapping override**: When snapping is enabled, each `Dragging*` state applies the snap grid to the accumulated delta before committing.

**Future extension**: Add `DraggingTranslateView` and `DraggingOrbit` states for camera manipulation (middle mouse, alt-LMB), keeping them separate from transform gizmo states to avoid conflicts.

---

### 1.12 Selection outline/highlight improvements

**File:** `shaders/selection_outline.comp`, `src/rtv/PathTracerRenderer.cpp`

**Effort:** ~150 lines (Phase 1 pass: ~40 lines for lightweight ID-buffer highlight)

Current selection highlight is a basic AABB overlay. The recommended primary strategy is **ID-buffer driven highlight** — use the existing entity ID pick buffer to drive a post-process highlight, avoiding a separate stencil pass.

**Phase 1 lightweight pass**: A simple fullscreen quad sampling the entity ID buffer and drawing a colored outline around selected IDs. No stencil, no extra render pass. This provides immediate visual feedback with minimal infrastructure.

Alternative approaches considered (deferred):
- **Stencil-based outline:** Render selected objects with a stencil buffer pass, then apply a fullscreen outline. More robust for complex geometry but adds a full render pass. Defer to later phase if ID-buffer is insufficient.
- **Emissive overlay:** Add a subtle emissive glow to selected objects in the path traced output. Requires shader modification and potentially recompilation.

**Recommendation:** Start with ID-buffer driven highlight (leverages existing infrastructure, minimal overhead). Fall back to stencil if ID-buffer sampling proves insufficient for thin geometry or overlapping selections.

---

### 1.13 Transform reset utilities

**File:** `src/rtv/InspectorPanel.cpp` (Inspector section)

**Effort:** ~40 lines

Add reset and copy/paste buttons to the Transform section:

```cpp
ImGui::SeparatorText("Transform");

// Reset row
if (ImGui::SmallButton("Reset Position")) {
    entity->transform.position = glm::vec3(0.0f);
    entity->transform.dirty = true;
    transformChanged = true;
}
ImGui::SameLine();
if (ImGui::SmallButton("Reset Rotation")) {
    entity->transform.rotationEuler = glm::vec3(0.0f);
    entity->transform.dirty = true;
    transformChanged = true;
}
ImGui::SameLine();
if (ImGui::SmallButton("Reset Scale")) {
    entity->transform.scale = glm::vec3(1.0f);
    entity->transform.dirty = true;
    transformChanged = true;
}

// Copy / Paste transform
static std::optional<Transform> copiedTransform;
if (ImGui::SmallButton("Copy")) {
    copiedTransform = entity->transform;
}
ImGui::SameLine();
ImGui::BeginDisabled(!copiedTransform.has_value());
if (ImGui::SmallButton("Paste")) {
    entity->transform = *copiedTransform;
    entity->transform.dirty = true;
    transformChanged = true;
}
ImGui::EndDisabled();

// Existing Position / Rotation / Scale drag inputs follow...
```

Add a `static std::optional<Transform>` at file scope in `InspectorPanel.cpp` (or in a persistent struct). The copy survives across entity selection changes.

---

## Phase 2 — Core Workflow (50-400 lines each)

### 2.1 Drag-and-drop workflows

**File:** `src/rtv/ViewportPanel.cpp`, `src/rtv/SceneHierarchyPanel.cpp`, `src/rtv/MaterialEditorPanel.cpp`

**Effort:** ~120 lines

ImGui drag-and-drop uses payload IDs. Three key workflows:

**a) Material onto entity** (in viewport or hierarchy)

```cpp
// In Material Editor:
ImGui::Button("Drag material");
if (ImGui::BeginDragDropSource()) {
    EditorMaterialDragPayload payload{materialId};
    ImGui::SetDragDropPayload("MATERIAL", &payload, sizeof(payload));
    ImGui::Text("Material %u", materialId);
    ImGui::EndDragDropSource();
}

// In Scene Hierarchy (accept):
if (ImGui::BeginDragDropTarget()) {
    if (const auto* payload = ImGui::AcceptDragDropPayload("MATERIAL")) {
        auto* data = static_cast<EditorMaterialDragPayload*>(payload->Data);
        // assign material to selected entity
    }
    ImGui::EndDragDropTarget();
}
```

**b) Entity reparenting via drag**

In `drawEntityNode()`, add drag-drop target that moves the dragged entity under the target:

```cpp
if (ImGui::BeginDragDropTarget()) {
    if (const auto* payload = ImGui::AcceptDragDropPayload("ENTITY")) {
        auto* data = static_cast<EntityId*>(payload->Data);
        requests.reparentEntity = {*data, entity.id};
    }
    ImGui::EndDragDropTarget();
}
```

**Cycle prevention:** Before accepting a reparent, validate the hierarchy:
```cpp
bool wouldCreateCycle(const SceneRegistry& registry, EntityId child, EntityId newParent) {
    if (child == newParent) return true;
    // Walk up newParent's ancestor chain; if child appears, cycle exists
    EntityId ancestor = newParent;
    while (ancestor.valid()) {
        if (ancestor == child) return true;
        const Entity* e = registry.entity(ancestor);
        ancestor = e ? e->parent : EntityId{};
    }
    return false;
}
```

**c) HDR / glTF drag onto viewport** — already works via GLFW drop callback. Add a visual drop-zone indicator when dragging a file over the viewport:

```cpp
if (ImGui::BeginDragDropTarget()) {
    // Accept file payload from OS
    // Show a "Load HDR / Load glTF" overlay
    ImGui::EndDragDropTarget();
}
```

---

### 2.2 Visibility / lock toggles in Scene Hierarchy

**File:** `src/rtv/SceneHierarchyPanel.cpp`, `include/rtv/Entity.h`

**Effort:** ~60 lines

Add eye (visibility) and lock (prevent selection/edit) toggles to each entity row. These are core scene management primitives — essential once scenes grow.

**Entity changes** (`include/rtv/Entity.h`):
```cpp
struct Entity {
    // ... existing fields ...
    bool visible = true;
    bool locked = false;
};
```

**Hierarchy rendering** (`SceneHierarchyPanel.cpp`, in `drawEntityNode()`):
```cpp
// Before TreeNodeEx label:
ImGui::PushID(static_cast<int>(entity.id.index));

// Visibility toggle
bool vis = entity.visible;
if (ImGui::Checkbox("##vis", &vis)) {
    entity.visible = vis;
    // Visibility changes should NOT set transform.dirty —
    // that would trigger unnecessary TLAS/accumulation resets.
    // Use a dedicated update kind (see invalidation granularity section).
}
ImGui::SameLine();

// Lock toggle
bool locked = entity.locked;
if (ImGui::Checkbox("##lock", &locked)) {
    entity.locked = locked;
}
ImGui::SameLine();

// Existing TreeNodeEx label...
if (entity.locked) {
    ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(140, 140, 140, 255));
}
const bool open = ImGui::TreeNodeEx(label.c_str(), flags);
if (entity.locked) {
    ImGui::PopStyleColor();
}
```

**Hierarchical visibility propagation:**

An entity's effective visibility cascades from its ancestors:

```cpp
bool effectiveVisible(const Entity& entity, const SceneRegistry& registry) {
    if (!entity.visible) return false;
    if (entity.parent.valid()) {
        const Entity* parent = registry.entity(entity.parent);
        if (parent != nullptr && !effectiveVisible(*parent, registry))
            return false;
    }
    return true;
}
```

**Consequences of this rule:**
- Hiding a parent recursively hides all descendants in the viewport
- Children of a hidden parent are not pickable (ray intersection skipped via instance mask)
- Gizmo is disabled for hidden entities
- The hierarchy tree still shows hidden children (visually dimmed) — they can still be selected for editing, so toggling visibility on them is discoverable
- The Inspector shows a "Hidden by parent" notice when a child is individually visible but parent is hidden

**Behavior when locked:**
- Click selection in hierarchy is ignored
- Click selection in viewport is ignored
- Transform gizmo is disabled
- Inspector shows a "This entity is locked" notice instead of editable fields

**Solo mode** (stretch): Alt+click on the eye icon to isolate — hide all other entities. Requires storing a visibility snapshot to restore.

---

### 2.3 Multi-select in Scene Hierarchy

**Files:** `include/rtv/EditorSelection.h`, `src/rtv/SceneHierarchyPanel.cpp`

**Effort:** ~200 lines

**Update `EditorSelection`:**

```cpp
class EditorSelection {
public:
    void toggleEntity(EntityId id);                    // Ctrl+click
    void selectRange(EntityId from, EntityId to);     // Shift+click
    void clear();
    bool isSelected(EntityId id) const;
    const std::vector<EntityId>& selectedEntities() const;
    size_t selectionCount() const;
private:
    EditorSelectionId primary_{};
    std::vector<EntityId> multiSelected_;
    EntityId lastClickedId_; // for range selection
};
```

**Caveat:** `selectRange()` requires a **flattened visible hierarchy** for consistent behavior. Before processing shift-click, collect all visible entities in display order into a vector, then select the range between the previously clicked entity and the current click.

> **Complexity callout — Multi-select interactions compound non-linearly:**
> The ~200 line estimate covers the selection logic itself. The real cost is in every system that interacts with selection:
> - **Gizmo**: Must operate on all selected entities, not just the primary. Translate applies the same delta to all. Rotate/scale needs pivot mode choice.
> - **Inspector**: Must show "multi-edit" mode where unchanged fields display "(multiple values)".
> - **Delete**: Must handle batch deletion with a single undo command (composite command).
> - **Visibility/lock**: Batch operations over selection set.
> - **Drag-and-drop**: Must distinguish "drag one" vs "drag many".
> - **Selection outline**: Must render N outlines, not just one.
> - **Scene serialization**: Copy/paste of multi-selection.
>
> Each interaction doubles the implementation surface. Plan for ~800–1000 total lines spread across all consumers, even though the core selection container is ~200 lines.

**Click handler in `drawEntityNode()`:**

```cpp
if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
    if (ImGui::GetIO().KeyCtrl) {
        selection.toggleEntity(entity.id);
    } else if (ImGui::GetIO().KeyShift) {
        selection.selectRange(lastSelectedId, entity.id);
    } else {
        selection.selectEntity(entity.id);
    }
    lastSelectedId = entity.id;
}
```

**Inspector behavior:**
- Single selection → existing behavior
- Multi selection → show "{N} entities selected" header + batch delete button
- Transform editing in multi-select applies deltas relative to each entity's **individual origin** (not a computed median pivot — simpler and more predictable for now)
- **Future pivot modes:** individual origin → median pivot → active object pivot → bounding box center (like Blender/Maya/Unity). Not needed until multi-select transform editing becomes frequent.

---

## Phase 3 — Infrastructure

### 3.1 Centralized Scene Mutation Layer

**New files:** `include/rtv/SceneOperations.h`, `src/rtv/SceneOperations.cpp`

**Effort:** ~120 lines

Before undo can exist, ALL scene mutation paths must route through a single authoritative layer. The current pattern of "panels partially understand scene structure" becomes dangerous once undo is active — mixing direct mutation with command mutation creates state desynchronization bugs that are nearly impossible to debug.

**Problem**: Currently, scene mutations are scattered:
- `InspectorPanel` modifies transform/light/camera directly on `Entity`
- `SceneHierarchyPanel` deletes entities directly
- `MaterialEditorPanel` modifies materials via `MaterialAsset`
- `ViewportPanel` modifies transforms via gizmo
- Context menus duplicate/delete/reparent entities

Each of these paths would need its own undo command, and they all share the same `SceneRegistry`. If one path bypasses the undo stack (e.g., an editor internal operation), the undo state and the actual scene state diverge silently.

**Solution — SceneOperations:**

```cpp
// include/rtv/SceneOperations.h
class SceneOperations {
public:
    explicit SceneOperations(SceneDocument& document);

    // All mutation paths go through these methods:
    EntityId createEntity(const std::string& name, EntityId parent = {});
    void deleteEntity(EntityId id);
    EntityId duplicateEntity(EntityId id);
    void reparentEntity(EntityId child, EntityId newParent);

    void setTransform(EntityId id, const Transform& transform);
    void setVisibility(EntityId id, bool visible);
    void setLocked(EntityId id, bool locked);

    void addComponent(EntityId id, ComponentType type);
    void removeComponent(EntityId id, ComponentType type);

    void setMaterial(uint32_t materialId, const MaterialAsset& material);

    // Undo support:
    void setUndoStack(UndoStack* stack);

private:
    SceneDocument* document_;
    UndoStack* undoStack_ = nullptr;

    // Internal helpers capture old state for undo:
    Entity snapshotEntity(EntityId id) const;
    void validateOperation(EntityId id) const;
};
```

**Design rules:**

1. **No direct mutation outside SceneOperations**: All panels, gizmos, context menus, and internal operations must call `SceneOperations` methods. The only exception is camera state (view/projection), which is editor-only and has no undo.

2. **Undo is optional, but routing is mandatory**: `SceneOperations` works correctly even when `undoStack_` is null (Phase 1–2 behavior). When undo is plugged in (Phase 3), `SceneOperations` wraps each mutation in a command automatically. This means panels do NOT need to know whether undo is active — they always call the same API.

3. **Snapshot capture**: Each mutation captures the old state before applying the new state. For delete, this means a full entity snapshot (children, transform, components). For transform, just the previous transform.

4. **Validation**: `validateOperation()` checks entity existence, locked state, cycle prevention (reparent), and returns error codes that the caller can surface via `NotificationManager`.

**Integration with EditorRequests:**

```cpp
// In Application::applyEditorRequests():
if (requests.deleteEntity.has_value()) {
    sceneOps_->deleteEntity(*requests.deleteEntity);
}
if (requests.duplicateEntity.has_value()) {
    sceneOps_->duplicateEntity(*requests.duplicateEntity);
}
// ... etc for all mutation request types
```

This keeps `EditorRequests` as the UI-side request bus and `SceneOperations` as the authoritative mutation layer. No panel ever holds a direct pointer to `SceneRegistry`.

> **Side note — SceneOperations as god object:** Unified scene mutation is correct for v1. Splitting into `SceneGraphOperations`, `MaterialOperations`, and `LightOperations` would increase interface surface without measurable benefit until undo commands need different serialization or the codebase exceeds ~50k LOC. Revisit when there are 6+ mutation methods or when command capture requires subsystem-specific snapshot logic.

**Pre-undo dependency**: This class must exist and ALL mutation paths must route through it BEFORE undo is implemented. Otherwise, commands end up duplicating scene logic, and the direct-mutation paths that inevitably remain create impossible-to-diagnose desync bugs.

> **Future separation (not urgent)**: As SceneOperations grows, it may accumulate serialization, hierarchy, material mutation, and renderer sync concerns. If it becomes unwieldy, split into `SceneGraphOperations` (create/delete/reparent), `MaterialOperations` (material mutation), and `RendererSceneSync` (TLAS/descriptor/accumulation routing). The unified class is correct for v1.

---

### 3.2 SceneEvent System

**New files:** `include/rtv/SceneEventBus.h`, `src/rtv/SceneEventBus.cpp`

**Effort:** ~100 lines

SceneOperations mutates the scene, but renderer, hierarchy, selection, inspector, undo, notifications, and asset systems all need synchronization. Wiring each consumer individually creates scattered subscription logic that is impossible to audit.

**Solution — typed event bus:**

```cpp
// include/rtv/SceneEventBus.h
enum class SceneEventType {
    EntityCreated,
    EntityDeleted,
    EntityReparented,
    TransformChanged,
    VisibilityChanged,
    LockChanged,
    MaterialChanged,
    ComponentAdded,
    ComponentRemoved,
    SelectionChanged,
    SceneLoaded,
    SceneCleared,
};

struct SceneEvent {
    SceneEventType type;
    EntityId entity;
    EntityId oldParent;      // for reparent
    EntityId newParent;      // for reparent
    ComponentType componentType; // for add/remove
    bool oldVisibility;      // for visibility toggle
    bool oldLocked;          // for lock toggle
};
```

**Event bus:**

```cpp
class SceneEventBus {
public:
    using Handler = std::function<void(const SceneEvent&)>;
    void subscribe(Handler handler);
    void publish(const SceneEvent& event);
    void clear();
private:
    std::vector<Handler> handlers_;
};
```

**Integration with SceneOperations:**

Each `SceneOperations` method publishes the appropriate event after mutation:

```cpp
EntityId SceneOperations::createEntity(const std::string& name, EntityId parent) {
    EntityId id = document_->registry().createEntity(name, parent);
    if (eventBus_) {
        eventBus_->publish({SceneEventType::EntityCreated, id});
    }
    return id;
}

void SceneOperations::deleteEntity(EntityId id) {
    auto snapshot = snapshotEntity(id);  // capture for undo
    document_->registry().destroyEntity(id);
    if (eventBus_) {
        eventBus_->publish({SceneEventType::EntityDeleted, id});
    }
}
```

**Consumers:**

| Consumer | Subscribes to | Action |
|----------|--------------|--------|
| Renderer sync (SceneUpdateRouter) | TransformChanged, VisibilityChanged, MaterialChanged, EntityDeleted, ComponentAdded/Removed | Routes to appropriate `SceneUpdateKind` |
| SceneHierarchyPanel | EntityCreated, EntityDeleted, EntityReparented | Refreshes tree structure |
| UndoStack | EntityCreated, EntityDeleted | Enables Undo menu item, refreshes labels |
| SelectionManager | EntityDeleted | Clears deleted entity from selection |
| NotificationManager | EntityCreated, EntityDeleted | Shows toast ("Entity created", "Entity deleted") |
| InspectorPanel | SelectionChanged | Refreshes displayed entity |
| MaterialEditorPanel | MaterialChanged | Refreshes displayed material |
| CameraBookmarkManager | EntityDeleted | Invalidates bookmarks referencing deleted entity |

**Design rules:**

1. **Events are fire-and-forget**: Consumers should not mutate the scene in response to events (that is `SceneOperations`'s job). Events are notifications, not requests.
2. **Events are synchronous within a frame**: All subscribers are notified before the frame ends. No deferred delivery.
3. **No guaranteed delivery order**: Consumers must not depend on other consumers processing the event first. If ordering matters (e.g., selection must clear before hierarchy refreshes), that is the editor tick's responsibility.

The `SceneEventBus` is owned by `Application` and passed to `SceneOperations`, renderer sync, and all subscribing panels at construction time.

---

### 3.3 Undo/Redo system

**⚠️ Critical prerequisite**: Before implementing undo, ALL scene mutation paths must route through `SceneOperations` (3.1) and the `SceneEventBus` (3.2) must be operational. If any code path mutates the scene directly while undo is active, the undo stack and actual scene state will diverge silently — creating bugs that are nearly impossible to reproduce or diagnose. Once undo exists, there is no "temporary direct mutation" — every create, delete, transform, material, visibility, and lock change must go through the command layer.

**New files:** `include/rtv/UndoStack.h`, `src/rtv/UndoStack.cpp`

**Effort:** ~400 lines

**Design — polymorphic commands:**

```cpp
class ICommand {
public:
    virtual ~ICommand() = default;
    virtual void undo(SceneDocument& document) = 0;
    virtual void redo(SceneDocument& document) = 0;
    virtual std::string label() const = 0;
};

// Concrete examples:
class TransformCommand : public ICommand {
    EntityId entity_;
    Transform oldTransform_;
    Transform newTransform_;
    // ...
};

class DeleteEntityCommand : public ICommand {
    Entity entitySnapshot_;  // full copy before deletion
    EntityId parentId_;
    size_t siblingIndex_;
    // ...
};

class CreateEntityCommand : public ICommand {
    EntityId createdId_;
    std::string name_;
    // ...
};

class MaterialEditCommand : public ICommand {
    uint32_t materialId_;
    MaterialAsset oldMaterial_;
    MaterialAsset newMaterial_;
    // ...
};
```

**Command batching / transactions** — essential to prevent gizmo drags from generating hundreds of tiny undo steps:

```cpp
class CommandTransaction {
public:
    explicit CommandTransaction(UndoStack& stack, const std::string& label)
        : stack_(&stack), label_(label) {}

    ~CommandTransaction() {
        if (active_) cancel();
    }

    void append(std::unique_ptr<ICommand> cmd) {
        pending_.push_back(std::move(cmd));
    }

    void commit() {
        if (!pending_.empty()) {
            // Wrap as a single macro command
            struct MacroCommand : ICommand {
                std::vector<std::unique_ptr<ICommand>> commands;
                std::string label_;
                void undo(SceneDocument& doc) override {
                    for (auto it = commands.rbegin(); it != commands.rend(); ++it)
                        (*it)->undo(doc);
                }
                void redo(SceneDocument& doc) override {
                    for (auto& cmd : commands) cmd->redo(doc);
                }
                std::string label() const override { return label_; }
            };
            auto macro = std::make_unique<MacroCommand>();
            macro->commands = std::move(pending_);
            macro->label_ = label_;
            stack_->pushCommand(std::move(macro));
        }
        active_ = false;
    }

    void cancel() { pending_.clear(); active_ = false; }

private:
    UndoStack* stack_;
    std::string label_;
    std::vector<std::unique_ptr<ICommand>> pending_;
    bool active_ = true;
};

// Usage in ViewportPanel:
// mouse down → capture original transform
// drag updates → mutate live only (no commands)
// mouse release → create ONE TransformCommand(old, final) and commit

**Optimization note:** For continuous gizmo motion, do NOT allocate per-delta commands even inside a transaction. Capture old state on mouse-down and create a single `TransformCommand(oldTransform, finalTransform)` on mouse-up. The `CommandTransaction` pattern is better suited for batch operations (multi-object edits, grouped changes) rather than real-time dragging.
```

**UndoStack:**

```cpp
class UndoStack {
public:
    void pushCommand(std::unique_ptr<ICommand> cmd);
    bool canUndo() const;
    bool canRedo() const;
    const std::string& undoLabel() const;
    const std::string& redoLabel() const;
    void undo(SceneDocument& document);
    void redo(SceneDocument& document);
    void clear();
private:
    std::vector<std::unique_ptr<ICommand>> commands_;
    size_t currentIndex_ = 0;
    static constexpr size_t maxCommands_ = 256;
};
```

**Integration:**
1. Store `UndoStack` in `Application`.
2. Pass to `SceneOperations` via `setUndoStack()`.
3. Ctrl+Z / Ctrl+Shift+Z handled in `processRuntimeControls()` → `EditorRequests` bools.
4. After undo/redo, `SceneUpdateKind` is set appropriately.

**Process for each command type:**

| Operation | During mutation | On undo | On redo |
|-----------|----------------|---------|---------|
| Transform change | `CommandTransaction` commits one `MacroCommand` | Restore old transform | Restore new transform |
| Entity create | Push `CreateEntityCommand(id)` | Destroy entity | Re-create from snapshot |
| Entity delete | Push `DeleteEntityCommand(snapshot)` | Re-insert entity | Destroy entity |
| Material edit | Push `MaterialEditCommand(id, old, new)` | Restore old material | Restore new material |

**Scene serialization:** Add `"version": 1` to `SceneDocument::saveJson()` immediately so that future schema migrations (bookmarks, layout paths, editor metadata) can be handled cleanly.

---

### 3.4 Stable Entity Identities

**Files:** `include/rtv/Entity.h`, `src/rtv/SceneRegistry.cpp`, `include/rtv/EditorSelection.h`

**Effort:** ~100 lines

Current entity IDs are index-based and are invalidated when entities are added or removed (shifting indices). This causes problems for undo/redo, bookmarks, and multi-select persistence.

**Requirement:** Entity identity must survive insertion and deletion of other entities. This is a prerequisite for:
- Undo/redo (restoring a deleted entity requires a stable ID)
- Camera bookmarks (referencing specific entities)
- Multi-select ranges that persist across scene mutations

**Design — UUID-based identity:**

```cpp
// include/rtv/Entity.h
struct EntityId {
    uint64_t id; // random UUID (use incremental counter or std::mt19937_64)
};

// Alternative: layered approach
// Internal: u32 index + u32 generation (like ECS handles)
// External: u64 UUID that maps to internal (index, gen)
struct EntityHandle {
    uint64_t uuid;      // persistent, never changes
    uint32_t index;     // current slot in registry (recalculated on scene load)
    uint32_t generation; // incremented on reuse
};
```

**Option 1 (Recommended): Simple UUID**

Add a `uint64_t uuid` field to `Entity`. The registry uses a hashmap `std::unordered_map<uint64_t, Entity>` instead of a flat vector of slots. Entity references stored in `EditorSelection`, bookmarks, and undo commands use the `uuid` field.

**Option 2 (Layered): Index + Generation**

Keep the flat array for performance but add a generation counter to detect stale handles. On scene load, rebuild index→uuid and uuid→index mappings.

**Migration strategy:**
1. Add `uint64_t Entity::uuid` field, initialize during entity construction with a monotonic counter
2. Store `EntityId` as `uint64_t uuid` everywhere external persistence is needed (undo commands, bookmarks, serialized scene)
3. Keep `uint32_t index` as a hot cache for the registry lookup (recalculated from registry on scene load)
4. Add `SceneRegistry::entityByUuid(uint64_t uuid)` for persistence paths
5. EditorSelection switches from storing `uint32_t` index to `uint64_t` uuid

**Serialization impact:**
- Scene file: add `"uuid": <uint64_t>` to each entity
- Bookmark references: switch from entity name to entity uuid
- Undo commands: store uuid, not index

**Avoid:** Do NOT use string-based GUIDs (too heavy). Do NOT reuse UUIDs within a session. Reset UUID counter on new scene.

---

## Phase 4 — Polish

### 4.1 Toast/Notification system

**New files:** `include/rtv/NotificationManager.h`, `src/rtv/NotificationManager.cpp`

**Effort:** ~150 lines

```cpp
// include/rtv/NotificationManager.h
#pragma once
#include <string>
#include <deque>

namespace rtv {

enum class NotificationType { Info, Warning, Error, Success };

struct Notification {
    std::string message;
    double startSeconds;     // from engine timing, not wall clock
    float duration;
    NotificationType type;

    // Optional action (e.g. "View Log", "Locate File")
    std::function<void()> action;
    std::string actionLabel;
};

class NotificationManager {
public:
    void push(std::string message,
              NotificationType type = NotificationType::Info,
              float duration = 3.0f);
    void draw(double nowSeconds);

private:
    std::deque<Notification> active_;
    static constexpr size_t maxVisible_ = 5;
    static constexpr float fadeDuration_ = 0.3f;
    static constexpr ImVec2 toastSize_{400.0f, 0.0f}; // width, auto height
    float toastPadding_ = 8.0f;

    void coalesce(const std::string& message, NotificationType type,
                  float duration);
    static ImU32 colorForType(NotificationType type);
};

} // namespace rtv
```

**Draw behavior:**
- Stack from top-right, newest at bottom
- Each toast: rounded rect background + colored left border + icon + text
- Opacity fades from 0→1 over `fadeDuration_` at start, and 1→0 over `fadeDuration_` at end
- If more than `maxVisible_` active, oldest toasts are hidden (not removed — they'll still expire)
- Identical consecutive messages are coalesced: extend duration + re-trigger fade

**Integration:**
1. Add `NotificationManager` to `Application`.
2. Pass pointer through `EditorRuntimeState`.
3. Draw in `EditorLayer::draw()` after all panels (so they render on top).
4. Use `ImGui::GetForegroundDrawList()` for top-right positioning.
5. Trigger on: scene loaded, HDR loaded, entity deleted, material updated, settings applied, error messages, scene rebuild.

---

### 4.2 Camera bookmarks

**New file:** `include/rtv/CameraBookmark.h`

**Effort:** ~80 lines

```cpp
struct CameraBookmark {
    std::string name;
    glm::vec3 position;
    glm::vec3 forward;
    float fovY;

    // Extensible: optional rendering state capture
    std::optional<float> exposure;
    std::optional<float> envRotation;
    std::optional<uint32_t> debugView;
};

class CameraBookmarkManager {
public:
    void saveBookmark(const CameraController& camera, const std::string& name,
                      const RendererSettings* settings = nullptr);
    void loadBookmark(const CameraBookmark& bookmark,
                      CameraController& camera, PathTracerRenderer& renderer,
                      RendererSettings* settings = nullptr) const;
    void deleteBookmark(size_t index);
    const std::vector<CameraBookmark>& bookmarks() const;
    void serialize(SceneDocument& document) const;
    void deserialize(const SceneDocument& document);
private:
    std::vector<CameraBookmark> bookmarks_;
};
```

**Integration:**
- Add to `EditorRequests`: `std::optional<std::string> saveCameraBookmark`, `std::optional<size_t> loadCameraBookmark`
- UI: In Inspector when a Camera entity is selected, or in a "Bookmarks" panel
- Persisted in the scene `.rtlevel` JSON via `serialize`/`deserialize`
- Scene version: add `"version": 1` early to `SceneDocument::saveJson()` for forward migration

---

### 4.3 Renderer progress feedback

**File:** `src/rtv/ViewportPanel.cpp` (HUD section), `include/rtv/PathTracerRenderer.h`

**Effort:** ~40 lines

Add feedback indicators to the HUD:

```cpp
// Accumulation progress (if accumulation limit is set)
if (settings.accumulationLimit > 0) {
    const float progress = static_cast<float>(state.renderer.sampleCount())
                         / static_cast<float>(settings.accumulationLimit);
    dl->AddText(..., ("Accumulation: " + std::to_string(int(progress * 100)) + "%").c_str());
}

// Moving indicator
if (state.viewport.mouseCaptureActive) {
    dl->AddText(..., IM_COL32(255, 200, 60, 255), "⚡ Moving — accumulation paused");
}

// Denoiser active
if (settings.denoiserEnabled) {
    dl->AddText(..., IM_COL32(150, 200, 255, 200), "Denoiser: ON");
}

// Scene rebuild spinner (via NotificationManager)
```

---

### 4.4 Editor preferences system

**New files:** `include/rtv/EditorPreferences.h`, `src/rtv/EditorPreferences.cpp`

**Effort:** ~100 lines

Consolidate scattered editor settings into a single `editor_preferences.json` file saved alongside (or adjacent to) the global `rtv_editor.ini`.

```cpp
// include/rtv/EditorPreferences.h
struct EditorPreferences {
    // Camera
    float cameraMoveSpeed = 2.4f;
    float cameraFastMoveSpeed = 7.5f;

    // Grid
    bool gridVisible = true;

    // Viewport
    bool showHud = true;
    float hudScale = 1.0f;

    // General
    bool confirmDelete = true;
    std::vector<std::string> recentFiles;
    static constexpr size_t maxRecentFiles = 10;

    void save(const std::filesystem::path& path) const;
    void load(const std::filesystem::path& path);
    static std::filesystem::path defaultPath();
};
```

**Integration:**
- Store `EditorPreferences` in `Application`.
- Load on startup from `defaultPath()` (e.g. `editor_preferences.json` in the working directory).
- Save whenever preferences change (camera speed slider, grid toggle, etc.).
- Pass relevant fields through `EditorRuntimeState` to panels.
- The `recentFiles` list replaces the one in `AssetBrowserPanel`, keeping a single source of truth.

**Avoid fragmentation:** Do NOT create separate pref files for each system. One file, one `save()`/`load()` pair.

---

### 4.5 Scene statistics panel

**New file:** `src/rtv/SceneStatsPanel.cpp`, `include/rtv/SceneStatsPanel.h`

**Effort:** ~60 lines

Add a lightweight statistics panel that surfaces scene and renderer metrics currently buried in the Debug/Profiler panel:

```cpp
void SceneStatsPanel::draw(const EditorRuntimeState& state) {
    if (!ImGui::Begin("Scene Stats")) return;

    const MeshParamsUniform& meshParams = state.renderer.scene().meshParams();
    const GpuScene& gpuScene = state.renderer.scene();

    ImGui::SeparatorText("Scene");
    ImGui::Text("Entities: %zu", state.sceneDocument != nullptr
        ? state.sceneDocument->registry().liveCount() : 0);
    ImGui::Text("Meshes: %u  Primitives: %u",
        meshParams.meshCount, meshParams.primitiveCount);
    ImGui::Text("Triangles: %u  Vertices: %u",
        meshParams.triangleCount, meshParams.vertexCount);
    ImGui::Text("Instances: %u", meshParams.instanceCount);
    ImGui::Text("Materials: %u  Textures: %zu",
        meshParams.materialCount,
        gpuScene.materialTextureDescriptors().size());

    ImGui::SeparatorText("Rendering");
    ImGui::Text("Lights: %u  Spheres: %u",
        meshParams.lightCount, meshParams.sphereCount);
    ImGui::Text("Emissive area: %.3f", meshParams.emissiveTotalArea);
    ImGui::Text("BVH nodes: %u  Local BVH: %u",
        meshParams.bvhNodeCount, meshParams.localBvhNodeCount);
    ImGui::Text("TLAS nodes: %u  TLAS indices: %u",
        meshParams.tlasNodeCount, meshParams.tlasInstanceIndexCount);

    const RayTracingRendererStats rt = state.renderer.rayTracingStats();
    if (rt.active) {
        ImGui::SeparatorText("Hardware RT");
        ImGui::Text("BLAS: %u  Instances: %u", rt.blasCount, rt.instanceCount);
        ImGui::Text("AS memory: %.2f MB",
            static_cast<double>(rt.accelerationStructureBytes) / (1024.0 * 1024.0));
    }

    ImGui::End();
}
```

**Integration:**
- Add `EditorPanelVisibility::sceneStats = false` (hidden by default)
- Add to `EditorLayer::draw()` when visible
- Hook into View menu toggle

---

### 4.6 GPU diagnostics panel

**New file:** `src/rtv/GpuDiagnosticsPanel.cpp`, `include/rtv/GpuDiagnosticsPanel.h`

**Effort:** ~80 lines

Add a diagnostics panel focused on GPU metrics useful for renderer development:

```cpp
void GpuDiagnosticsPanel::draw(const EditorRuntimeState& state) {
    if (!ImGui::Begin("GPU Diagnostics")) return;

    const GpuFrameTimings& timings = state.renderer.timings();
    constexpr double mb = 1024.0 * 1024.0;

    ImGui::SeparatorText("Frame Timing");
    ImGui::Text("Path trace:  %.2f ms", timings.pathTraceMs);
    ImGui::Text("Denoiser:    %.2f ms", timings.denoiserMs);
    ImGui::Text("Fullscreen:  %.2f ms", timings.fullscreenMs);
    ImGui::Text("Total GPU:   %.2f ms",
        timings.pathTraceMs + timings.denoiserMs + timings.fullscreenMs);

    ImGui::SeparatorText("Rays");
    const VkExtent2D extent = state.renderer.renderExtent();
    const uint64_t raysPerFrame = static_cast<uint64_t>(extent.width)
        * static_cast<uint64_t>(extent.height)
        * state.renderer.settings().maxBounces;
    const float gpuSec = (timings.pathTraceMs + timings.denoiserMs + timings.fullscreenMs) / 1000.0f;
    ImGui::Text("Rays/frame:  %llu", static_cast<unsigned long long>(raysPerFrame));
    ImGui::Text("Rays/sec:    %.2f M", gpuSec > 0.0f
        ? (static_cast<double>(raysPerFrame) / gpuSec) / 1.0e6 : 0.0);

    ImGui::SeparatorText("Memory");
    const RayTracingRendererStats rt = state.renderer.rayTracingStats();
    if (rt.active) {
        ImGui::Text("AS memory:   %.2f MB",
            static_cast<double>(rt.accelerationStructureBytes) / mb);
        ImGui::Text("SBT memory:  %.2f KB",
            static_cast<double>(rt.sbtBytes) / 1024.0);
    }

    ImGui::SeparatorText("Accumulation");
    ImGui::Text("Sample:      %u", state.renderer.sampleCount());
    ImGui::Text("Last reset:  %s",
        accumulationResetReasonName(state.renderer.lastAccumulationResetReason()));

    ImGui::End();
}
```

**Integration:**
- Add `EditorPanelVisibility::gpuDiagnostics = false` (hidden by default)
- Add to `EditorLayer::draw()` when visible
- Hook into View menu toggle

---

### 4.7 Styled viewport HUD

**File:** `src/rtv/ViewportPanel.cpp` (lines 286-301)

**Effort:** ~80 lines

Replace plain white text with a semi-transparent panel + color-coded values:

```cpp
ImDrawList* dl = ImGui::GetWindowDrawList();
const ImVec2 hudPos(imagePos.x + 12.0f, imagePos.y + 12.0f);
const ImVec2 hudSize(350.0f, 140.0f);

// Background
dl->AddRectFilled(hudPos,
    ImVec2(hudPos.x + hudSize.x, hudPos.y + hudSize.y),
    IM_COL32(0, 0, 0, 160), 6.0f);

// Per-pass breakdown
std::ostringstream fmt;
fmt << std::fixed << std::setprecision(2);
fmt << "GPU: " << (timings.pathTraceMs + timings.denoiserMs + timings.fullscreenMs) << " ms";
fmt << "  (trace " << timings.pathTraceMs
    << "  denoise " << timings.denoiserMs
    << "  present " << timings.fullscreenMs << ")";

const float gpuTotal = timings.pathTraceMs + timings.denoiserMs + timings.fullscreenMs;
const ImU32 perfColor = gpuTotal < 16.0f ? IM_COL32(120, 220, 120, 255)
                      : gpuTotal < 33.0f ? IM_COL32(240, 220, 100, 255)
                      : IM_COL32(240, 100, 100, 255);

float y = hudPos.y + 8;
dl->AddText(ImVec2(hudPos.x + 8, y), perfColor, fmt.str().c_str());

y += 22;
dl->AddText(ImVec2(hudPos.x + 8, y), IM_COL32(200, 200, 200, 255),
    ("Samples: " + std::to_string(state.renderer.sampleCount())).c_str());

y += 22;
dl->AddText(ImVec2(hudPos.x + 8, y), IM_COL32(180, 200, 230, 255),
    rendererDebugViewName(settings.debugView));

y += 22;
dl->AddText(ImVec2(hudPos.x + 8, y), IM_COL32(180, 180, 180, 200),
    (std::to_string(extent.width) + "x" + std::to_string(extent.height)).c_str());

// Moving / denoiser / accumulation indicators from 4.3
```

---

## Phase 5 — Large Systems (200-1000+ lines each)

### 5.1 Asset Browser v2 — thumbnails, directory tree, favorites

**File:** `src/rtv/AssetBrowserPanel.cpp`

**Effort:** ~1000+ lines (this system grows toward an asset database)

Current Asset Browser is a flat list with text-only entries. v2 adds:

**a) Texture thumbnails**

For each loaded texture, render a small GPU thumbnail:

```cpp
// In AssetManager, expose image view:
VkImageView textureThumbnailView(TextureAssetHandle handle);

// In AssetBrowserPanel:
struct ThumbnailCache {
    VkDescriptorSet descriptor;
    VkExtent2D size;
};

std::unordered_map<uint32_t, ThumbnailCache> thumbnailCache_;

void ensureThumbnail(uint32_t textureIndex) {
    if (thumbnailCache_.count(textureIndex)) return;
    VkImageView view = assets->textureThumbnailView({textureIndex});
    if (view != VK_NULL_HANDLE) {
        auto desc = ImGui_ImplVulkan_AddTexture(view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        thumbnailCache_[textureIndex] = {desc, /* size */};
    }
}

// In draw:
constexpr float thumbSize = 64.0f;
for (uint32_t i = 0; i < textures.size(); ++i) {
    ensureThumbnail(i);
    if (auto it = thumbnailCache_.find(i); it != thumbnailCache_.end()) {
        ImGui::Image((ImTextureID)(uintptr_t)it->second.descriptor,
                     ImVec2(thumbSize, thumbSize));
        ImGui::SameLine();
    }
    // existing selectable + info
}
```

**Descriptor lifetime management** is critical — textures can be reloaded or evicted. Store `thumbnailCache_` in `AssetBrowserPanel` and invalidate on scene reload via an `EditorRequests::invalidateAssetBrowser` flag.

**b) Favorites / Recent paths**

```cpp
struct FavoritePath {
    std::filesystem::path path;
    std::string label;   // user-assigned or filename
};

class AssetBrowserPanel {
    std::vector<FavoritePath> favorites_;
    std::vector<std::filesystem::path> recentPaths_;
    static constexpr size_t maxRecent_ = 10;

    void drawFavorites() {
        ImGui::SeparatorText("Favorites");
        for (auto& fav : favorites_) {
            if (ImGui::Selectable(fav.label.c_str())) {
                // load fav.path
            }
        }
        // "Add Current" button saves current scene/HDR path
        // Remove via right-click context menu
    }

    void drawRecentPaths() {
        ImGui::SeparatorText("Recent");
        for (auto& path : recentPaths_) {
            if (ImGui::Selectable(path.filename().string().c_str())) {
                // load path
            }
        }
    }
};
```

**c) Directory tree browser** (stretch goal)

A simple filesystem tree that lets users navigate directories without going through the OS file dialog every time. Show `.gltf`, `.glb`, `.hdr` files with click-to-load.

> **⚠️ Scope trap — Asset Browser v2 is a black hole**: Thumbnails create gravity toward async loading → caching → metadata → previews → import pipeline → tags → search → dependency tracking. Each of these is its own system. The 1000-line estimate covers only the visible UI — the real cost is the asset management platform it inevitably pulls in.
>
> **Explicit dependency gate — do NOT begin Asset Browser v2 until ALL of the following are stable:**
> - Stable undo/redo (Phases 3.1–3.3)
> - Stable multi-select (Phase 2.3)
> - Stable renderer invalidation (Phase 1C SceneUpdateRouter)
> - Stable viewport interactions (gizmo, snapping, selection outline)
> - Stable scene serialization (scene versioning, UUID persistence)
>
> Asset systems absorb an outsized fraction of engine development time. Rushing them before the editor itself is stable will stall everything.

**d) Async loading note** — Large glTF files already load in a background thread (`std::async`). Extend this pattern to HDRs and textures. The thumbnail cache should show a loading state until the texture GPU upload completes.

---

### 5.2 Viewport axes indicator

**File:** `src/rtv/ViewportPanel.cpp`

**Effort:** ~60 lines

Add a 3D axes indicator in the bottom-left corner:

```cpp
void drawAxesIndicator(const EditorRuntimeState& state,
                       const CameraController& camera) {
    const float size = 60.0f;
    const float margin = 15.0f;
    ImVec2 origin(
        state.viewport.imageOrigin.x + margin,
        state.viewport.imageOrigin.y + state.viewport.imageSize.y - margin - size);

    // Derive camera orientation from quaternion (avoids view matrix extraction issues)
    glm::vec3 forward = glm::normalize(camera.direction());
    glm::vec3 worldUp(0, 1, 0);
    glm::vec3 right = glm::normalize(glm::cross(forward, worldUp));
    glm::vec3 up = glm::normalize(glm::cross(right, forward));

    // Project world axes to indicator space
    auto project = [&](glm::vec3 dir) {
        return ImVec2(
            origin.x + (dir.x * right.x + dir.y * up.x - dir.z * forward.x) * size * 0.8f,
            origin.y - (dir.x * right.y + dir.y * up.y - dir.z * forward.y) * size * 0.8f
        );
    };

    ImDrawList* dl = ImGui::GetWindowDrawList();
    auto drawAxis = [&](glm::vec3 dir, ImU32 color, const char* label) {
        ImVec2 tip = project(dir);
        dl->AddLine(origin, tip, color, 2.5f);
        dl->AddText(ImVec2(tip.x + 3, tip.y - 6), color, label);
    };

    drawAxis(glm::vec3(1,0,0), IM_COL32(255,80,80,255), "X");
    drawAxis(glm::vec3(0,1,0), IM_COL32(80,255,80,255), "Y");
    drawAxis(glm::vec3(0,0,1), IM_COL32(80,80,255,255), "Z");
}
```

**Future:** clickable orientation cube (like many DCC tools).

---

### 5.3 Entity "Add Component" support

**File:** `src/rtv/InspectorPanel.cpp`

**Effort:** ~80 lines

Add buttons to attach components that don't exist yet:

```cpp
ImGui::SeparatorText("Add Component");
ImGui::BeginDisabled(entity->light.has_value());
if (ImGui::Button("Light")) {
    document.registry().addLight(entity->id, Light{});
    document.markDirty(SceneUpdateKind::LightOnly);
    requests.sceneUpdate = SceneUpdateKind::LightOnly;
}
ImGui::EndDisabled();
ImGui::SameLine();
ImGui::BeginDisabled(entity->camera.has_value());
if (ImGui::Button("Camera")) {
    document.registry().addCamera(entity->id, Camera{});
    document.markDirty(SceneUpdateKind::CameraOnly);
    requests.sceneUpdate = SceneUpdateKind::CameraOnly;
}
ImGui::EndDisabled();
ImGui::SameLine();
ImGui::BeginDisabled(entity->meshRenderer.has_value());
if (ImGui::Button("Mesh Renderer")) {
    document.registry().addMeshRenderer(entity->id, MeshRenderer{});
    document.markDirty(SceneUpdateKind::FullSceneRebuild);
    requests.sceneUpdate = SceneUpdateKind::FullSceneRebuild;
}
ImGui::EndDisabled();
```

Also add a small remove button (`-`) next to each component header label that calls `removeCamera()`, `removeLight()`, or `removeMeshRenderer()`.

**Architecture note:** The current `std::optional<Component>` storage limits each entity to one component per type. This is fine for now, but future ECS-style systems may support multiple components of the same type (multi-light entities, etc.). When that day comes, this UI logic should use reflection/generic registration rather than per-type hardcoding.

---

### 5.4 Grid / ground plane overlay

**File:** `src/rtv/ViewportPanel.cpp`

**Effort:** ~80 lines

Add a perspective grid on the ground plane (y=0) for spatial orientation, especially in empty scenes:

```cpp
void drawGrid(const EditorRuntimeState& state, const CameraController& camera) {
    constexpr float gridSize = 50.0f;
    constexpr int gridDivisions = 50;
    constexpr float gridStep = gridSize / static_cast<float>(gridDivisions);

    glm::mat4 view = editorViewMatrix(camera);
    glm::mat4 proj = editorProjectionMatrix(
        activeCameraFov(*state.sceneDocument),
        state.viewport.imageSize.x / state.viewport.imageSize.y);
    glm::mat4 viewProj = proj * view;

    // Convert image coords to NDC for 2D→3D
    auto worldToScreen = [&](glm::vec3 worldPos) -> std::optional<ImVec2> {
        glm::vec4 clip = viewProj * glm::vec4(worldPos, 1.0f);
        if (clip.w <= 0.0f) return {};
        glm::vec3 ndc = glm::vec3(clip) / clip.w;
        return ImVec2(
            state.viewport.imageOrigin.x + (ndc.x * 0.5f + 0.5f) * state.viewport.imageSize.x,
            state.viewport.imageOrigin.y + (1.0f - (ndc.y * 0.5f + 0.5f)) * state.viewport.imageSize.y);
    };

    ImDrawList* dl = ImGui::GetWindowDrawList();

    // Major lines every 10 divisions
    for (int i = -gridDivisions; i <= gridDivisions; ++i) {
        float pos = static_cast<float>(i) * gridStep;
        auto p1 = worldToScreen(glm::vec3(pos, 0.0f, -gridSize));
        auto p2 = worldToScreen(glm::vec3(pos, 0.0f,  gridSize));
        auto p3 = worldToScreen(glm::vec3(-gridSize, 0.0f, pos));
        auto p4 = worldToScreen(glm::vec3( gridSize, 0.0f, pos));
        bool major = (i % 10) == 0;
        ImU32 color = major ? IM_COL32(160, 160, 160, 180) : IM_COL32(100, 100, 100, 80);
        float thickness = major ? 1.5f : 0.8f;
        if (p1 && p2) dl->AddLine(*p1, *p2, color, thickness);
        if (p3 && p4) dl->AddLine(*p3, *p4, color, thickness);
    }
}
```

**Toggling:** Add a `G` key shortcut and an `EditorRequests::toggleGrid` field. Store `bool gridVisible_` in `Application`. Persist in `EditorPreferences` (not layout ini — grid visibility is a personal preference, not a per-project layout concern).

---

## Additional Considerations

### Renderer invalidation granularity

**This is now formalized as Phase 1C (SceneUpdateRouter) in the renderer improvement plan.** The `SceneUpdateRouter` class routes each change type to precise update paths and asserts on `FullSceneRebuild` fallthrough.

The goals from this UX plan are directly implemented by Phase 1C:

| Update Kind | SceneUpdateRouter Action | Depends on |
|-------------|-------------------------|------------|
| Transform | TLAS refit + instance buffer update | Phase 0 (AS Refit) |
| Material | Per-material descriptor update | Phase 6A (Full Bindless) — partial bindless sufficient earlier |
| Light | Light buffer update | Already partial |
| Camera | View matrix update only | Already partial |
| Visibility | Instance mask update only | Phase 1C |

**Editor dependency on renderer phases:**

| UX Phase | Features | Renderer Prerequisite |
|----------|----------|----------------------|
| Phase 1 (Quick Wins) | Settings, tooltips, dock, shortcuts, context menus, search, snapping, transform utils, selection outline | None — parallel with renderer Stage 0 |
| Phase 2 (Core Workflow) | Drag/drop, visibility/lock, multi-select | Phase 1C (SceneUpdateRouter) + Phase 0 (AS Refit) |
| Phase 3 (Infrastructure) | Scene mutation layer, undo/redo, stable IDs, scene versioning | Phase C (Editor Responsiveness) — stable viewport required for undo feedback |
| Phase 4 (Polish) | Notifications, bookmarks, preferences, diagnostics, HUD | Phase C (Editor Responsiveness) — stable foundation before visual polish |
| Phase 5 (Large Systems) | Asset Browser v2, axes, grid, add component | Stage 2+ (Temporal Foundation) — stable accumulation before complex overlays |

### SceneUpdateKind — Formal Invalidation Classification

As the editor gains mutation paths (visibility, lock, multi-select, undo), the engine needs an explicit invalidation classification to avoid unnecessary TLAS rebuilds, accumulation resets, and descriptor updates.

**This is already formalized as Phase 1C (SceneUpdateRouter) in the [renderer improvement plan](IMPROVEMENT_PLAN.md#phase-1c-sceneupdaterouter-effort-medium-impact-architectural).** The enum and routing table live there. This section documents the UX-facing policy decisions that the router implements:

```cpp
enum class SceneUpdateKind : uint8_t {
    None,              // no scene change
    Transform,         // TLAS refit + instance buffer update
    Material,          // per-material descriptor update
    Visibility,        // instance mask update only
    Topology,          // entity add/remove → full scene rebuild
    Lighting,          // light buffer update
    Camera,            // view matrix update only
    RendererSettings,  // shader recompilation or pipeline change
};
```

**Impact matrix:**

| Update Kind | TLAS | Descriptors | Accumulation | Shaders |
|-------------|------|-------------|--------------|---------|
| Transform | Refit | — | Reset | — |
| Material | — | Update | Reset | — |
| Visibility | — | — | — | — |
| Topology | Rebuild | Rebind | Reset | — |
| Lighting | Refit | Update | Reset | — |
| Camera | — | — | Reset | — |
| RendererSettings | — | — | Reset | Recompile |

**Key policy decisions:**
- **Visibility changes do NOT reset accumulation** — only the instance mask changes; the rendered image is identical. This is critical for viewport responsiveness during solo/lock operations.
- **Camera changes DO reset accumulation** — the viewport content changes even though the scene is the same.
- **Topology changes require full TLAS rebuild** — not just refit, because the instance count changed.
- **RendererSettings changes may require shader recompilation** — this is the most expensive path and should be gated behind an explicit user action.

This enum should be set by `SceneOperations` (3.1) after each mutation, and consumed by the renderer's update pipeline to select the minimal update path.

**UX action → SceneUpdateKind mapping:**

| UX Action | SceneUpdateKind | Accumulation Reset? | TLAS Impact |
|-----------|----------------|---------------------|-------------|
| Transform drag (gizmo) | Transform | Yes | Refit |
| Material edit (inspector) | Material | Yes | None |
| Visibility toggle | Visibility | No | None |
| Lock toggle | None (editor-only) | No | None |
| Entity create | Topology | Yes | Rebuild |
| Entity delete | Topology | Yes | Rebuild |
| Entity duplicate | Topology | Yes | Rebuild |
| Camera move (orbit/pan/zoom) | Camera | No | None |
| Light parameter edit | Lighting | Yes | Refit |
| Environment change | Lighting | Yes | Refit |
| Sun direction change | Lighting | Yes | Refit |
| Renderer setting change | RendererSettings | Yes | None |

### Editor Frame Tick Ordering

With deferred requests, SceneOperations, SceneEventBus, undo, gizmo state, renderer invalidation, and selection all active, the editor needs an explicit per-frame execution order to prevent one-frame lag, stale state, and incorrect accumulation resets.

```cpp
// Per-frame execution order — strictly sequenced:
//
//   1. Input processing (mouse, keyboard, shortcuts)
//   2. Selection update (viewport pick, hierarchy click, modifier keys)
//   3. Gizmo update (hover detection, drag state machine → EditorRequests)
//   4. Panel updates (inspector, hierarchy, material editor → populate EditorRequests)
//   5. EditorRequests dispatch (Application::applyEditorRequests())
//   6. SceneOperations execution (mutations + SceneEventBus::publish)
//   7. SceneEvent delivery to all subscribers (sync consumers)
//   8. Renderer sync (SceneUpdateRouter::route() → GPU updates)
//   9. Render (path tracing, denoising, compositing, present)
//  10. Undo stack refresh (menu enable/disable, label update)
```

**Why this order:**

- **Selection before gizmo**: The gizmo needs to know what is selected before it can render handles. Processing selection first avoids a one-frame lag on gizmo visibility.
- **Gizmo before panels**: Gizmo drag produces EditorRequests (transform change) that must be consumed in the same frame. Panels that display the current transform should read the updated value, not the pre-drag value.
- **Requests before SceneOperations**: EditorRequests is the UI bus. SceneOperations is the execution layer. Processing requests before operations ensures all UI intent is captured in a single batch.
- **SceneOperations before SceneEvents**: Mutations must complete before notifications fire. Otherwise, consumers see stale state.
- **SceneEvents before RendererSync**: Events may trigger additional scene queries (e.g., selection clears on delete). The renderer must see the final state.
- **RendererSync before Render**: The GPU must see updated descriptors, TLAS, and accumulation state before the next frame renders.
- **Undo refresh last**: The undo stack's menu state depends on whether the current operation added a command. Refreshing it first would show stale undo labels.

**Critical invariant**: Within a single frame, steps 1–10 execute exactly once, in order. No step re-enters a previous step. If a panel responding to a SceneEvent needs to mutate the scene, it must queue an EditorRequests field (which is consumed next frame). This prevents infinite loops.

### Layout Persistence Fragility

Per-project layout persistence is the correct direction. However, layouts can become incompatible as the editor evolves:

- **Renamed panels**: A saved layout references a panel name that no longer exists. Fallback: ImGui ignores unknown dock nodes.
- **Removed windows**: Dock space refers to a window type that was removed. Fallback: The window simply doesn't open; the layout structure remains intact.
- **Invalid saved states**: Corrupted `.layout.ini` files (truncated writes, disk errors). Fallback: Detect parse failure and reset to default layout.

**Mitigations (not urgent):**

1. **Version the layout format**: Add a `layout_version` integer to the layout file. On mismatch, offer to reset or migrate.
2. **Validate on load**: After `ImGui::LoadIniSettingsFromDisk()`, check that expected dock nodes exist. If key nodes are missing, log a warning and offer reset.
3. **Backup on save**: Before overwriting the layout file, rename the previous version to `*.layout.ini.bak`. This gives a manual recovery path.
4. **DPI-aware layout hints**: Store a DPI scale factor alongside the layout so that layouts from high-DPI monitors don't render tiny on standard-DPI monitors (and vice versa).

These are not urgent — the global `rtv_editor.ini` already handles most layout recovery. Versioning becomes important when saved layouts are shared across projects or team members.

### SelectionManager — Interaction System Ownership

Selection logic is currently implicit between hierarchy, viewport, outline, gizmo, and inspector. As multi-select, box-select, and locked/hidden filtering grow, selection needs to become a proper interaction system rather than a data container:

```cpp
class SelectionManager {
public:
    // Selection mutation
    void select(EntityId id);           // single select
    void toggle(EntityId id);            // Ctrl+click
    void selectRange(EntityId from, EntityId to);  // Shift+click
    void clear();

    // Hover state (transient — does not change selection)
    void setHovered(EntityId id);        // mouse over entity
    EntityId hovered() const;
    bool isHovered(EntityId id) const;

    // Queries
    EntityId primary() const;
    const std::vector<EntityId>& selection() const;
    bool isSelected(EntityId id) const;
    size_t count() const;

    // Filtering
    bool isSelectable(EntityId id) const;         // checks locked + hidden
    std::vector<EntityId> filteredSelection() const;  // excludes locked/hidden

    // Event wiring
    void setEventBus(SceneEventBus* bus);

private:
    EntityId primary_;
    std::vector<EntityId> selection_;
    EntityId lastClicked_;  // for range selection
    EntityId hovered_;      // transient — reset each frame
    SceneEventBus* eventBus_ = nullptr;

    void publishSelectionChanged();
};
```

**Hover state** is a transient per-frame value: the entity under the mouse cursor. It is reset at the start of each frame and updated during viewport/hierarchy rendering. Gizmo and outline use it for visual feedback (highlight on hover) without changing the active selection. This prevents the confusing behavior where moving the mouse over an entity changes your selection.

**Event publishing**: When selection changes (select, toggle, clear, range), `SelectionManager` publishes a `SceneEventType::SelectionChanged` event. The inspector subscribes to this to refresh the displayed entity. Hierarchy panel subscribes to update its visual selection state. This replaces the current pattern of each consumer polling selection state.

**Ownership boundaries:**

| Observer | Reads from | Writes to | Notes |
|----------|-----------|-----------|-------|
| Scene Hierarchy | `SelectionManager` | `EditorRequests::selectEntity` | Click → request, not direct select |
| Viewport picking | `SelectionManager` | `EditorRequests::selectEntity` | Ray-pick → request |
| Gizmo | `SelectionManager::primary()` | — | Operates on primary, transforms affect all selected |
| Inspector | `SelectionManager::primary()` | — | Shows primary entity |
| Selection outline | `SelectionManager::selection()` | — | Highlights all selected entities |
| Multi-select modifier keys | — | — | Ctrl=∈toggle, Shift=∈range, plain=∈single |

**Design rules:**
1. Selection is editor-only state — it NEVER feeds into the scene or renderer.
2. Selection changes NEVER trigger accumulation reset (selection is a UI overlay).
3. `isSelectable()` centralizes the locked/hidden filter so all consumers behave consistently.
4. The `EditorRequests::selectEntity` field is a `std::variant<EntityId, SelectRange>` for simplicity — the application layer converts to the appropriate `SelectionManager` call.

**Locked entity interaction:** When a locked entity is clicked, `SelectionManager` ignores the click and optionally surfaces a notification. This prevents the confusing UX of clicking a lock entity and having nothing visibly happen (the selection stayed on whatever was previously selected).

### Accumulation reset policy

For a path-traced editor, avoiding unnecessary accumulation resets is critical for responsiveness. Define which actions trigger a reset:

| Action | Reset accumulation? | Reason |
|--------|-------------------|--------|
| Camera move | Yes | New viewport content |
| Transform change | Yes | Geometry moves |
| Material change | Yes | Surface appearance changes |
| Lighting change | Yes | Illumination changes |
| Environment change | Yes | Background changes |
| Visibility toggle | Yes | Content appears/disappears |
| Lock toggle | No | Editor-only state |
| Selection change | No | Selection is a UI overlay |
| Gizmo hover | No | No scene content change |
| Panel resize | No | Just re-projection |
| HUD toggle | No | HUD is a UI overlay |
| Grid toggle | No | Grid is a UI overlay |
| Denoiser toggle | Yes | Denoiser state affects output |

**Optimization goal:** Eventually, visibility toggles and transform changes should be skippable (no reset) when the denoiser can temporally adapt, or when only editor overlays change.

### Async asset loading roadmap

| Asset type | Current | Goal |
|------------|---------|------|
| glTF scenes | Background thread (`std::async`) | Good — keep |
| HDR environments | Synchronous `loadEnvironment()` | Background load + notification on completion |
| Textures (via glTF) | Loaded during scene import | Individual async loading with progress |
| All assets | — | Unified job queue with cancellation |

---

## Implementation Order (Final)

| Priority | Feature | Phase | Why |
|----------|---------|-------|-----|
| **1** | Render Settings collapsing | P1 | Highest impact per line changed |
| **2** | Tooltips | P1 | Eliminates guesswork |
| **3** | Dock persistence | P1 | Usability for multi-project workflows |
| **4** | Context menus | P1 | Eliminates toolbar hunting |
| **5** | Search/filter | P1 | Scales to complex scenes |
| **6** | Gizmo snapping | P1 | Precision editing |
| **7** | Transform reset utilities | P1 | High usability per line |
| **8** | Delete confirmation | P1 | Safety net |
| **9** | Dirty indicator | P1 | Immediate feedback |
| **10** | Shortcut labels + keybindings | P1 | UX consistency |
| **11** | Camera speed presets | P1 | Convenience |
| **12** | Selection outline | P1 | Selection readability is fundamental interaction quality |
| **13** | Visibility/lock toggles | P2 | Core scene management |
| **14** | Drag-and-drop | P2 | Central UX infrastructure |
| **15** | Multi-select | P2 | Batch operations |
| **16** | Scene event system | P3 | Synchronization layer for all mutation consumers |
| **17** | Undo/Redo | P3 | Foundation for all editing workflows |
| **18** | Stable entity IDs | P3 | Prerequisite for undo/bookmarks |
| **19** | Scene mutation layer | P3 | All mutation paths must unify before undo |
| **20** | Editor preferences | P4 | Prevents setting fragmentation |
| **21** | Notifications | P4 | Replaces silent status text |
| **22** | Progress feedback | P4 | Offline render visibility |
| **23** | Scene stats panel | P4 | Developer debugging insight |
| **24** | GPU diagnostics | P4 | Renderer development feedback |
| **25** | Camera bookmarks | P4 | Viewpoint iteration |
| **26** | Styled HUD | P4 | Visual polish |
| **27** | Asset Browser v2 | P5 | Asset workflow foundation |
| **28** | Grid/ground plane | P5 | Spatial orientation |
| **29** | Axes indicator | P5 | Spatial orientation |
| **30** | Add component | P5 | Workflow completeness |

---

## Verification

After each item:

1. Build: `cmake --build build --config Release`
2. Launch: `run_rtvulkan_debug.bat` (Debug) or `run_rtvulkan.bat` (Release)
3. Load the Sponza scene
4. Exercise the changed panel(s)
5. Check console for warnings/errors
6. Verify no ImGui assertion failures
7. For Phase 1 items specifically: verify the existing hotkeys (F1-F6, WASD, R, etc.) still work unchanged
