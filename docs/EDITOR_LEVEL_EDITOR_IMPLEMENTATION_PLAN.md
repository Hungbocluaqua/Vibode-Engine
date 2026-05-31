# Near-Clone Level Editor Implementation Plan

## Purpose

This plan turns the current Vulkan renderer editor into a near-clone of the supplied dark Minitech/Unreal-style editor references and expands it into a real persistent level editor. It is written to be implementation-ready and should be read together with `docs/LEVEL_EDITOR_FEATURE_CHECKLIST.md` and `docs/EDITOR_REFERENCE_SCREENSHOT_INSPECTION.md`.

The main outcome is an editor that looks like the references, keeps the current renderer/debug tooling intact, and behaves like a level editor rather than a scene-loader UI.

## Implementation Status

Last updated: 2026-05-31

Completed in the current implementation pass:

- Step 1 - Rename Current glTF Replacement To Import Scene As New Scene.
- Step 3 - Explicit Scene File Requests.
- Step 4 - Near-Clone Shell, Menus, Dock Layout, And Theme.
- Step 6 - Project Manager MVP.
- Step 7 - Project Asset Registry Skeleton.

Partially completed in the current implementation pass:

- Step 2 - Dirty Scene State, Scene Name, And Replacement Prompt: editor-origin New Scene, Open Scene, Import Scene as New Scene, dropped glTF, Exit, and Project Close now use Save / Do Not Save / Cancel protection. Scene-tab close remains pending because closable scene tabs are not implemented yet.
- Step 5 - Viewport Toolbar, Status Strip, And Shortcuts: the viewport is renamed to `Scene`, the HUD is now a compact status strip, and Q/W/E/R/G shortcuts plus compact toolbar controls are present. Frame-selected and F1-F8 debug-view registry work remains pending the command/keybinding milestone.

Not completed yet:

- Project settings details UI, prefab assets, import-as-asset, import-and-place, drag/drop prefab placement, merge scene, and GUID-backed `.rtlevel` migration.
- Autosave/recovery, full command registry, keybinding registry, persistent Log/Console models, and Timeline keyframe persistence.

## Non-Negotiable Constraints

- Preserve editor mode, headless mode, profile JSON export, render graph dumps, debug view export, debug package export, validation suite, fixed-seed runs, and RenderDoc capture support.
- Do not require RenderDoc for normal execution.
- Do not add unconditional `vkDeviceWaitIdle` calls except shutdown or unavoidable full teardown.
- Preserve existing `.rtlevel` loading while adding GUID-backed asset references.
- Keep current renderer request/application safety patterns instead of directly mutating renderer state from UI panels.
- Treat the current glTF load behavior as `Import Scene as New Scene` until real asset import exists.

## Target Experience

The editor should visually and functionally match the screenshots as closely as ImGui allows:

- Dark docked shell with dense panels, compact tabs, flat surfaces, small radii, blue selected rows, and restrained separators.
- Top menu bar: `File | Create | Engine | Window | Render | Layout`.
- Active scene name shown in the top bar between separators, with a dirty marker when the scene has unsaved changes.
- Compact top-right performance readout with FPS and frame time in milliseconds.
- Center `Scene` viewport with rendered image, compact toolbar, and compact status readout.
- Right side: `Hierarchy` and `Render World Settings` tabs above `Inspector`.
- Bottom: `Content`, `Timeline`, and `Log` tabs.
- `Content` browser uses a folder tree, file/asset list, and details/preview pane with compact breadcrumb navigation.
- `Inspector` uses component-specific property layouts for Transform, Light, Sun Light, Camera, camera post-process, mesh/material, and authored world settings.
- Technical tools remain available but do not dominate the default level-editing workspace.

Reference interpretation rules:

- Use `docs/EDITOR_REFERENCE_SCREENSHOT_INSPECTION.md` as the concrete visual reference for density, panel structure, status placement, Content browser layout, and component inspector groupings.
- Keep the canonical menu order as `File | Create | Engine | Window | Render | Layout` even though one reference screenshot shows `Render` before `Window`.
- Treat the screenshots as workflow references, not as a requirement to preserve local absolute paths or Minitech-specific asset names.
- If hierarchy highlight and Inspector active entity appear to disagree, treat that as a selection-state risk to fix or explicitly model through separate hovered, selected, and active-editable entities.

## Phase 1 - Project Manager MVP

The editor should support projects before full game/runtime features. The project manager gives the editor a stable workspace for scenes, assets, cache files, autosaves, logs, editor config, and the asset registry. This is editor infrastructure only; it does not imply play mode, game packaging, scripting, or runtime systems yet.

### Project File

Add a `.rtproject` project file:

```json
{
  "version": 1,
  "projectGuid": "generated-guid",
  "name": "MyProject",
  "engineVersion": "0.1",
  "startupScene": "Scenes/Main.rtlevel",
  "contentRoot": "Content",
  "scenesRoot": "Scenes",
  "cacheRoot": "Cache",
  "savedRoot": "Saved",
  "configRoot": "Config",
  "assetRegistry": "Content/AssetRegistry.json",
  "defaultRenderPreset": "Editor",
  "autosaveEnabled": true,
  "autosaveIntervalMinutes": 5
}
```

### Project File Versioning

- `.rtproject` has a `version` field.
- Older project files load through migration.
- Save migrated project files as the latest version.
- Keep a backup before overwriting migrated project settings.

### Project Folder Layout

A new project should create:

```text
MyProject/
  MyProject.rtproject
  Content/
    Models/
    Materials/
    Textures/
    HDRI/
    Prefabs/
    VDB/
  Scenes/
    Main.rtlevel
  Cache/
    Meshes/
    Textures/
    Shaders/
    BLAS/
    Thumbnails/
  Saved/
    Autosaves/
    Logs/
    Backups/
  Config/
    EditorConfig.json
    Layout.json
    Keybindings.json
  Build/
```

`Build/` may exist in V1, but cook/build/package actions stay disabled until a later runtime workflow phase.

### Project Manager Window

On startup, show a Project Manager window if no project is open. Required actions are New Project, Open Project, Recent Projects, Browse, Remove from Recent, and Open Last Project.

New Project fields are project name, project location, template, Create Default Scene, and Create default Content folders.

Templates V1: Empty, Path-Traced Level Editor Default, Lighting Test Scene.

### Project Templates

- Empty: creates folders and `.rtproject` only; no default scene unless Create Default Scene is enabled.
- Path-Traced Level Editor Default: creates `Main.rtlevel`; adds Camera, Sun Light, Environment Light, Sky Atmosphere, and `WorldSettings`; uses default editor render preset.
- Lighting Test Scene: creates `Main.rtlevel`; adds Camera, Sun Light, Environment Light, test primitives, area light, and Post Process Volume; useful for lighting/material import validation.

### Recent Projects

Store recent projects in editor config:

```json
{
  "recentProjects": [
    "D:/Projects/MyProject/MyProject.rtproject",
    "D:/Projects/SponzaEditor/SponzaEditor.rtproject"
  ],
  "lastOpenedProject": "D:/Projects/MyProject/MyProject.rtproject"
}
```

Missing projects show a warning badge, can be removed, and opening a project validates required folders. If folders are missing, offer to recreate them.

### Project State And Requests

```cpp
struct ProjectContext {
    Guid projectGuid;
    std::string name;
    std::filesystem::path projectFile;
    std::filesystem::path projectRoot;
    std::filesystem::path contentRoot;
    std::filesystem::path scenesRoot;
    std::filesystem::path cacheRoot;
    std::filesystem::path savedRoot;
    std::filesystem::path configRoot;
    std::filesystem::path buildRoot;
    std::filesystem::path assetRegistryPath;
    std::filesystem::path startupScene;
    std::string defaultRenderPreset;
};

struct CreateProjectRequest {
    std::string name;
    std::filesystem::path location;
    std::string templateName;
    bool createDefaultScene = true;
};

struct OpenProjectRequest {
    std::filesystem::path projectFile;
};
```

Add `createProject`, `openProject`, `closeProject`, and `saveProjectSettings` to `EditorRequests`.

### Project Settings And Paths

`Project Settings...` includes Project, Paths, Startup, Autosave, and Import Defaults sections.

When a project is open, Import Asset writes into `Project/Content/`, cache data into `Project/Cache/`, autosaves into `Project/Saved/Autosaves/`, logs into `Project/Saved/Logs/`, the asset registry into `Project/Content/AssetRegistry.json`, and scenes default to `Project/Scenes/`.

Store project-relative paths inside `.rtproject`, `.rtlevel`, and `AssetRegistry` whenever possible. Normalize path separators internally, preserve original source paths as debug/import metadata only, and support moved project folders as long as relative paths remain valid.

### Startup And Close Flow

- If Open Last Project is enabled and the project exists, open it; otherwise show Project Manager.
- If the user opens `.rtlevel` directly, enter no-project compatibility mode.
- If project `startupScene` exists, load it; if missing, show a warning and open an empty scene.
- Close Project flow: prompt for dirty scene, save dirty asset registry or show error, save editor layout/config, clear loaded scene or return to Project Manager, clear `ProjectContext`, and clear Content Browser state.

### Asset Registry Integration

- Project opening loads `Project/Content/AssetRegistry.json`.
- Project closing saves the asset registry if dirty.
- Track `AssetRegistryState { bool dirty; std::filesystem::path path; }` and dirty reasons: AssetImported, AssetReimported, AssetDeleted, AssetRenamed, AssetMoved, AssetDependencyChanged.

### No-Project Mode Final Rule

- Old `.rtlevel` files can open without a project.
- Content Browser uses a temporary compatibility workspace.
- Import Asset strongly prompts the user to create/open a project.
- If the user refuses, store asset registry data next to the scene as `SceneName.assets.json`.
- No-project mode is compatibility-only, not the recommended workflow.

## Phase 2 - Editor Shell Near-Clone

### Theme

- Update the global ImGui style used by the editor: very dark background/panels, compact padding/spacing, small tab/frame rounding, blue accent selection, and subtle borders.
- Avoid a one-color palette. Keep the UI mostly neutral dark with blue accents and small amber/red/green status colors.
- Match the reference density: low vertical padding, compact rows, thin separators, icon-first controls, and restrained contrast between panel backgrounds.

### Dock Layout

- Rebuild the default dock layout: center `Scene`, right top `Hierarchy` and `Render World Settings`, right bottom `Inspector`, bottom `Content`, `Timeline`, and `Log`.
- Keep `Render Settings`, `Debug / Profiler`, `Scene Stats`, `GPU Diagnostics`, `Material Editor`, and `Console` available from menus.
- Save/load/reset layout behavior must continue to work.
- Keep `Content`, `Timeline`, and `Log` as bottom tabs even when Timeline and Log are initially thin shells.
- The bottom `Content` panel should be tall enough for a three-pane browser and should not collapse into a single file picker layout.

### Panel Renames

- Rename `Viewport` to `Scene`.
- Rename `Scene Hierarchy` to `Hierarchy`.
- Rename `Inspector / Properties` to `Inspector`.
- Rename `Asset Browser` to `Content`.
- Add visibility flags for `timeline`, `log`, `console`, and `renderWorldSettings` if it cannot cleanly reuse the existing render settings panel.

### Menu Bar

- Replace current menus with `File`, `Create`, `Engine`, `Window`, `Render`, and `Layout`.
- `File` exposes New Scene, Open Scene, Save Scene, Save Scene As, Import Asset, Import and Place, Import Scene as New Scene, Merge Scene into Current, Import HDRI, and Exit.
- `Create` exposes implemented creation first: Empty Entity, Camera, Point Light, Primary Sun. Mesh From Asset, Prefab, Volume, and Post Process entries may appear disabled until implemented.
- `Engine` exposes Play, Simulate, Pause, and Stop as disabled shells initially, plus the existing Reload Shaders action and diagnostics shortcuts.
- `Window` toggles all panels.
- `Render` contains reset accumulation, denoiser toggle, debug view controls, view mode, quality preset, and technical render settings access.
- `Layout` contains save layout, reset layout, workspaces, UI scale shell, and theme shell.
- Show the active scene title in the top bar beside the menu strip, matching the reference style of `| Untitled Scene |`, `| sponza-intel |`, or the current scene name.
- Add or preserve a compact window-level FPS/frame-time readout at the far right of the title/menu area.

## Phase 3 - Viewport UX

### Viewport Window

- Rename the viewport window to `Scene`.
- Preserve existing renderer image sizing, desired render extent, viewport mouse state, picking, selected instance ID, accumulation reset, and ImGuizmo behavior.
- Keep the viewport primarily image-driven; debug and status overlays must be compact enough not to obscure path-traced content.

### Toolbar

- Add a compact top-left toolbar inside the viewport with Select, Move, Rotate, Scale, Local/World, Snap, Grid, Axes, View Settings, Stats, and Draw Debug.
- Use existing transform gizmo mode, local/world mode, snap settings, grid toggle, and axes toggle state.
- Use short icon-like text labels if no icon font is available. Tooltips must describe each button.
- Keep light and transform gizmos visible on top of rendered content, including sun/area-light handles when those entities are selected.

### Status Strip

- Replace the large top-left HUD with a compact top-right status strip showing CPU frame ms, GPU frame ms, samples, accumulation limit, debug/view mode, render/display resolution, render scale, denoiser/TAA state, last accumulation reset reason, and camera speed.
- Keep warning colors for slow GPU frames and accumulation resets.
- Preserve a compact path tracing progress string similar to `pt 219/256 27.890` from the references.
- Keep `View Settings`, `Stats`, and `Draw Debug` as always-visible compact viewport actions.

### Shortcuts

- Preserve existing shortcuts and add missing editor-standard shortcuts where they do not conflict: `Q` select, `W` move, `E` rotate, `R` scale, `G` grid, `F` frame selected, and `F1-F8` debug views.
- If conflicts exist with existing renderer shortcuts, prefer existing behavior until a keybinding registry can resolve precedence.

## Phase 4 - Hierarchy And Inspector

### Hierarchy

- Restyle `Hierarchy` as a dense tree with search, create/filter controls, visibility and lock columns, component badges, and blue selected rows.
- Preserve existing hierarchy behavior: select, multi-select, range select, reveal selected, rename, duplicate, delete, create child, detach parent, drag-drop reparent, material drop, and focus in viewport.
- Keep entity visibility and lock as compact icon-like controls with tooltips.
- Keep parent/child hierarchy editing backed by existing scene operations.
- Support large imported roots and repeated imported names without losing readability; examples from the references include Sponza roots, Quixel canyon roots, repeated `exterior1` entries, and generated glTF roots.
- Keep hierarchy selection synchronized with the viewport and Inspector active entity, or explicitly distinguish hover, selection, and active editable entity in UI state.

### Inspector

- Restyle `Inspector` as a component/property grid with an entity header, collapsible component sections, consistent label/value columns, and reset buttons for vector rows.
- Component sections: Transform, Mesh Renderer, Material Slots, Material, Light, Primary Sun, Camera, and Add Component.
- Preserve current edit requests for transforms, cameras, lights, sun, mesh renderer flags, material assignments, material edits, add component, duplicate, delete, and clear selection.
- Do not bypass undo-backed scene operations where they already exist.
- Transform rows should use compact three-column vector fields with reset/revert controls.
- Light inspector layouts must be component-specific: area lights should expose shape/type, lumen units, IES profile, radius/cone controls, material source, visible-to-camera, and shadow toggles; sun lights should expose sun type, lux units, azimuth/elevation, color temperature, intensity, softness, and shadow bounce controls.
- Camera inspector should expose physical camera and exposure controls such as near clip, exposure mode, aperture, ISO, shutter speed, film size, focal length, and grouped post-process controls.
- Camera/post-process controls should use collapsible groups such as DOF, Bloom, Color correction, Vignetting, and Film grain.

## Phase 5 - Content Browser And Asset Database

### Content Browser

- Replace the current load-oriented `Asset Browser` with a real `Content` browser.
- Required controls: Add/Import, search, breadcrumb, back, forward, refresh, folder tree, asset list/grid, and details panel.
- Keep existing favorites and recent files, but present them as Content browser sections.
- Keep current texture thumbnails where available.
- Use the reference three-pane layout: left folder tree, middle file/asset list, right details/preview panel.
- The header should include add/import controls, a compact filter field, navigation buttons, refresh, and breadcrumb path.
- The details pane should clearly state when no supported file or asset is selected.
- Browser rows should support folders, `.rtlevel` scene files, glTF/GLB, OBJ/MTL, texture files, HDR/EXR, material records, mesh records, and prefab records as the relevant milestones land.
- Project-relative paths should be used for project assets, but the breadcrumb interaction should remain similar to the references.
- Double-click/open behavior:
  - `.rtlevel` opens a scene.
  - `.gltf` and `.glb` use `Import Scene as New Scene` until import mode selection is implemented.
  - `.hdr` and `.exr` load/apply the environment.

### Asset Database

- Add a project-local asset registry, initially JSON for debuggability.
- Add core concepts: `AssetGuid`, `AssetType`, `AssetRecord`, `AssetImportSettings`, `AssetDependency`, and `AssetImportStatus`.
- Store GUID, type, display name, source path, imported/cache path, thumbnail path, dependencies, references, last modified timestamp, import settings, and missing/reimport status.
- V1 asset types: Mesh, Material, Texture, HDRI, Scene, and Prefab.

### Drag And Drop

- Mesh or prefab dragged to the viewport creates an entity in the current scene.
- Material dragged to a hierarchy or viewport mesh assigns the material.
- HDRI dragged to world settings applies the environment.
- Texture dragged to a material slot assigns that texture once material texture-slot editing exists.

## Phase 6 - Correct Import Architecture

The editor must separate scene loading from asset importing.

### Required Import Modes

- `Open Scene`: replace the current scene with a saved `.rtlevel`.
- `Import Asset`: import reusable assets into Content only; do not modify the current scene.
- `Import and Place`: import reusable assets, generate a prefab if needed, and instantiate it in the current scene.
- `Import Scene as New Scene`: current glTF replacement behavior, renamed honestly.
- `Merge Scene into Current`: load an external scene/model hierarchy and append it to the current scene.
- `Reimport Selected`: reimport source assets while preserving GUIDs and scene references.

### Import Settings Dialog

Model import must show a user-visible settings dialog before execution:

- Import mode: Asset, Import and Place, New Scene, or Merge.
- Destination Content folder.
- Preserve hierarchy.
- Generate prefab.
- Import materials.
- Import textures.
- Import cameras.
- Import lights.
- Generate tangents.
- Build BLAS cache.
- Unit scale.
- Coordinate conversion.

### Request/API Changes

- Extend `EditorRequests` with explicit requests: `openScene`, `saveScene`, `saveSceneAs`, `importAsset`, `importAndPlace`, `importSceneAsNewScene`, `mergeScene`, `reimportAsset`, and `placeAsset`.
- Keep `loadGltf` temporarily as an internal compatibility path or migrate call sites to `importSceneAsNewScene`.
- Do not let `Import Asset` call the existing whole-scene replacement path.

### glTF/GLB Import As Asset

- Reuse `GltfLoader` and `SceneCache` where practical.
- Generate asset records for textures, materials, meshes, and prefab hierarchy.
- Imported asset output should resemble `Content/Models/Name/Name.prefab`, with `Meshes`, `Materials`, and `Textures` subfolders.
- Preserve material/texture relationships and node hierarchy from glTF.

### Merge Scene

- Load source scene/model into temporary import data.
- Append entities into the current `SceneDocument` instead of replacing it.
- Remap entity IDs and stable UUIDs.
- Register or reuse imported assets by GUID/source path.
- Mark topology dirty and rebuild the GPU scene through existing update paths.

## Phase 7 - Prefabs And Persistent Scenes

### Prefab Asset

- Add a `PrefabAsset` concept with GUID, display name, root nodes, entity/component data, asset references by GUID, default transform, and source import record.
- Generate prefabs from imported glTF, GLB, and later OBJ files.
- Add `Create Prefab From Selection`.
- Add `Place Prefab` through Content drag/drop and the Create menu.

### Scene Persistence

- Extend `.rtlevel` persistence to reference assets by GUID.
- Keep backward compatibility with existing files using `sourceGltf` and index-based mesh/material references.
- Save and load scene name, scene GUID, hierarchy, stable entity UUIDs, transforms, mesh renderer references, material slots, camera/light/sun components, render settings, world/environment settings, active camera, primary sun, and bookmarks.
- Add scene dirty state and an unsaved marker.
- Add autosave and recovery after core save/load is stable.

## Phase 8 - Lighting And Post-Processing Components

- Add scene-editable lighting components, post-process components, world-setting entities, inspector UI, save/load, undo/redo, and renderer dirty-flag integration.
- Keep artistic lighting/post-process controls in scene data, not only in `Render Settings`.
- Use the detailed component model in `Lighting And Post-Processing Component Details` below as the implementation contract.

## Phase 9 - Undo, Timeline, Log, Console

### Undo/Redo

- Expand undo/redo coverage to include entity create/delete/duplicate, transform edit, reparent, component add/remove, property edit, material assignment, asset placement, and prefab placement.
- Group drag transforms into a single transaction.
- Keep existing `UndoStack` and `SceneOperations` patterns.

### Timeline

- Add `Timeline` panel with play, pause, stop, frame number, start frame, end frame, scrubber, and keyframe button.
- First supported track type: transform keyframes.
- Defer full sequencer, curves, camera tracks, material tracks, and export until persistent scene editing is complete.

### Log

- Add `Log` panel with info/warning/error/import/render filters, search, clear, copy, and open log file controls.
- Integrate with existing notifications first.
- Add persistent log file integration after the panel shell works.

### Console

- Add `Console` panel with command input, command history, and basic scene/render commands.
- Defer autocomplete until command registration is stable.

## Phase 10 - Render World Settings And Technical Render Settings

### Render World Settings

- Add or split an artist-facing `Render World Settings` panel.
- Include environment HDRI, intensity, rotation, background intensity/visibility, primary sun summary/creation, sky/atmosphere controls, exposure controls, post process summary, and GI summary.
- Keep this panel docked with `Hierarchy` by default.

### Technical Render Settings

- Keep `Render Settings` for technical controls: render preset, debug view, samples, bounces, resolution scale, ReSTIR, denoiser, TAA, OMM, SER, artifact controls, and reset accumulation.
- Continue routing changes through `requestSettings` and existing safe renderer settings application.

## Phase 11 - Autosave, Recovery, And Polish

- Add autosave scheduling after Project Manager, persistent scenes, and asset registry dirty state are stable.
- Store autosaves under `Project/Saved/Autosaves/` when a project is open.
- Store backup copies under `Project/Saved/Backups/` before migration saves and destructive scene operations.
- Add recovery prompts on startup after crash/interrupted save detection.
- Keep polish work scoped to editor workflows: workspace presets, layout versioning, UI scale, theme, and final panel-density cleanup.

## MVP Persistent Level Editor

The first complete milestone must be small enough to finish without spreading into the whole roadmap:

1. Scene tab with dirty marker.
2. Current glTF scene replacement renamed to `Import Scene as New Scene`.
3. `Import Asset` creates Content assets and does not modify the current scene.
4. Asset registry JSON.
5. glTF import creates a `PrefabAsset`.
6. Drag prefab into viewport to create a placed instance.
7. Save/reopen scene with asset GUID references.
8. Transform gizmo and undo for placed prefab instances.
9. Merge glTF into the current scene.
10. Existing headless/profile/debug tools still pass.

## Required Architecture Details

### Asset Instancing Model

- `AssetRecord` is the reusable source/imported asset stored in the asset registry.
- `SceneEntity` is an entity instance placed in the level.
- `PrefabAsset` stores a reusable entity/component hierarchy template.
- `PrefabInstance` is a placed prefab with a prefab GUID plus local overrides.
- Scene entities reference mesh, material, texture, HDRI, scene, and prefab assets by GUID.
- Reimport updates source assets and generated prefab data without destroying placed scene instances.
- The same imported prefab can be placed many times, for example `Tree_01`, `Tree_02`, and `Tree_03` all referencing `Content/Models/Tree/Tree.prefab`.

### Prefab Overrides

- Support transform overrides, material slot overrides, component property overrides, added child entity overrides, removed child entity overrides, added component overrides, and removed component overrides.
- Add commands for Apply Overrides to Prefab, Revert Overrides, and Break Prefab Link.
- Save overrides with the `PrefabInstance`, not by mutating the source `PrefabAsset` unless Apply Overrides is explicitly invoked.
- Reimported prefab source data must preserve instance overrides where target entities/components can still be matched.

### Import Conflict And Hash Policy

- If source path plus import settings already exists, reuse the existing asset records.
- If the source changed on disk, mark the asset stale/reimport-needed instead of silently duplicating it.
- If the user intentionally imports a duplicate, create a new display name such as `Car_001` while assigning new GUIDs.
- Reuse textures/materials when content hash and import settings hash match.
- Name conflicts must not change stable GUIDs; rename only display names.
- Add `sourceHash`, `importedHash`, and `importSettingsHash` to `AssetRecord`.

### Asset Dependency Graph

- Track dependencies from prefab to mesh to material to texture.
- Track reverse references from texture to material to mesh/prefab/scene.
- Content Browser must support Find References and Show Dependencies.
- Prevent deleting referenced assets unless the user chooses force delete.
- Safe delete must show the scenes, prefabs, materials, or other assets that reference the target asset.
- Force delete replaces scene references with missing asset placeholders.

### Missing Asset Handling

- Missing mesh shows a red broken mesh icon/placeholder.
- Missing material uses a fallback magenta/error material.
- Missing texture uses a checker fallback.
- Missing HDRI uses the default environment.
- Content Browser shows missing and stale badges.
- Inspector asset fields expose `Locate Missing Asset...` and `Find in Content` actions.

### Import Report UI

- Show an import result window after imports.
- Include imported mesh/material/texture counts, generated prefab path, warnings, missing textures, unsupported extensions, alpha/double-sided warnings, coordinate/unit conversion warnings, and cache/reuse summary.
- Include actions for Open Imported Folder and Place Prefab Now.

### glTF Extension Policy

- V1 must support or explicitly warn for `KHR_lights_punctual`, `KHR_texture_transform`, `KHR_materials_emissive_strength`, `KHR_materials_transmission`, `KHR_materials_ior`, `KHR_materials_clearcoat`, and `KHR_materials_sheen`.
- `KHR_lights_punctual`, `KHR_texture_transform`, and `KHR_materials_emissive_strength` should be treated as first-priority support because they directly affect common scene appearance.
- Unsupported extensions must not fail silently: log a warning, add it to the Import Report, and continue with a fallback when possible.
- Unsupported material extensions should approximate to the closest available PBR material instead of dropping the material.

### Texture Color Space Rules

- Import base color and emissive textures as sRGB.
- Import normal, roughness, metallic, occlusion, transmission, alpha mask, and height textures as linear.
- Normal maps must not be tonemapped or gamma corrected.
- Generate tangent basis when missing and required by normal mapping.
- Texture import settings must record color space so reimport is deterministic.

### glTF Metallic-Roughness Rules

- For glTF metallic-roughness textures, G channel is roughness and B channel is metallic.
- R channel is usually unused for metallic-roughness and must not be treated as base color.
- Occlusion may be separate or packed; import metadata must preserve the source mapping.
- Material UI should expose factors separately from texture channel mapping.

### Coordinate System And Units

- Define and document the engine coordinate system before importer work begins.
- glTF input is right-handed, Y-up, and measured in meters.
- If the engine is already right-handed and Y-up, no axis conversion is required except matrix convention handling.
- If conversion is required, apply it consistently to nodes, cameras, lights, normals, tangents, and later animation data.
- Store the original import transform/settings in the `AssetRecord` so reimport uses the same conversion.

### Material Fallbacks

- Missing material uses a default gray PBR material.
- Missing base color texture uses the base color factor.
- Missing normal map uses a flat normal.
- Missing roughness uses the glTF roughness factor or `0.5` if no factor exists.
- Missing metallic uses the glTF metallic factor or `0.0` if no factor exists.
- Unsupported material extensions approximate to the closest available PBR material and generate an Import Report warning.

### Scene File Versioning And Migration

- Add an explicit scene header with `version` and `sceneGuid`.
- Version 1 is the legacy path/index-reference format.
- Version 2 is the GUID-backed asset-reference format.
- Load old scenes through a migration path.
- Save migrated scenes as the latest format.
- Keep a backup before overwriting a scene during migration save.

```cpp
struct RtLevelHeader {
    uint32_t version;
    Guid sceneGuid;
};
```

### Editor Command Registry

- Menus, toolbar buttons, shortcuts, and console commands must dispatch through shared command IDs instead of duplicating logic.
- V1 command IDs include `File.NewScene`, `File.OpenScene`, `File.SaveScene`, `Import.ImportAsset`, `Import.ImportAndPlace`, `Import.ImportSceneAsNewScene`, `Import.MergeScene`, `Edit.Undo`, `Edit.Redo`, `Entity.CreateEmpty`, `Entity.Delete`, `Entity.Duplicate`, `View.FrameSelected`, and `Render.ResetAccumulation`.
- Command handlers should produce `EditorRequests` or call existing editor operations; UI widgets should not directly implement action behavior.

### Keybinding Registry V1

- Add a small keybinding registry with command name, default key, active context, and conflict detection.
- Contexts: global, viewport, hierarchy, inspector, content browser, and console.
- Save user overrides to editor config later; V1 may use built-in defaults only.
- Do not add Q/W/E/R/F/G/F1-F8 shortcuts as hard-coded one-offs outside this registry.

### Selection Model

- Track primary selected entity, selected entity set, hovered entity, active editable entity, selection source, and selection changed event.
- Selection sources: viewport, hierarchy, content browser, inspector, and console command.
- Inspector edits the active editable entity; gizmo uses the selected entity set with the primary selection as pivot/default.
- Hierarchy, viewport outline, and inspector must observe the same selection state.

### Gizmo Transaction Rules

- On mouse down over gizmo, begin a transform transaction.
- During drag, update live transforms and mark renderer transform dirty.
- On mouse release, commit one undo command.
- On Escape/cancel, revert to original transforms.
- Multi-select stores the original transform for every selected entity.

### Scene Dirty Reasons

- Store dirty reasons, not only a boolean dirty state.
- Required reasons: EntityCreated, EntityDeleted, TransformChanged, MaterialChanged, AssetPlaced, AssetReimported, WorldSettingsChanged, and RenderSettingsChanged.
- Dirty reasons feed autosave, status strip, logging, and renderer update routing.

### World Settings Ownership

- Use entities for authored world actors: Sun, Atmosphere, Fog, Post Process Volume, Environment Light, and related future components.
- Use scene-level `WorldSettings` only for defaults and active bindings.
- Scene-level world settings should group `EnvironmentSettings`, `AtmosphereSettings`, `FogSettings`, `ExposureSettings`, `PostProcessSettings`, and `GISettings`.
- `Render World Settings` edits scene-level defaults and links to the active world-setting entities.

## Lighting And Post-Processing Component Details

Lighting and post-processing must be editable scene data, not only renderer settings. Artist-facing lighting and post-process controls should exist as scene entities/components with transform, hierarchy presence, inspector UI, save/load, undo/redo, and renderer dirty-flag integration. Technical renderer controls remain in `Render Settings`.

### Lighting Components

Required lighting components:

- DirectionalLightComponent / SunLightComponent
- PointLightComponent
- SpotLightComponent
- RectAreaLightComponent
- DiskAreaLightComponent
- SphereLightComponent
- EnvironmentLightComponent
- EmissiveMeshLightComponent
- SkyAtmosphereComponent
- HeightFogComponent
- VolumetricCloudComponent
- VolumeLight / ParticipatingMediaComponent later
- ReflectionCapture / RadianceProbe later

### Light Units Policy

- Directional/Sun light uses lux or a documented radiance multiplier.
- Point light uses lumens or candela.
- Spot light uses lumens or candela.
- Area light uses luminance/radiance or total lumens.
- UI may expose artist-friendly units, but the renderer receives normalized physical values.
- Unit conversion must be deterministic, documented, and shared by manually created lights, imported glTF lights, and renderer-side path tracing.

### glTF Light Import Mapping

- glTF directional light imports as `SunLightComponent` or `DirectionalLightComponent`.
- glTF point light imports as `PointLightComponent`.
- glTF spot light imports as `SpotLightComponent`.
- glTF light color maps to component color.
- glTF intensity maps to imported intensity with an explicit conversion note in the Import Report.
- glTF spot inner/outer cone values map to `SpotLightComponent` cone angles.
- Imported lights appear under the generated prefab hierarchy or imported scene root.

### glTF Camera Import Mapping

- glTF perspective cameras import as `CameraComponent`.
- FOV, aspect, near plane, and far plane import when available.
- Imported cameras appear as scene entities.
- When importing as a new scene, if no active camera exists, the first imported camera may become active.
- When importing as an asset/prefab, cameras remain part of the prefab unless the user disables camera import.

### Light Linking Later

- Add include/exclude object lists per light.
- Add include/exclude light lists per mesh renderer.
- Add light groups.
- Add per-object receive shadows and cast shadows toggles.
- Add per-object visible to camera, visible in reflections, and visible in GI toggles.

### Directional Light / Sun Component

Properties:

- enabled
- color
- color temperature
- intensity
- intensity unit: lux / radiance multiplier
- direction from transform
- angular diameter
- cast shadows
- cast volumetric shadows
- atmosphere sun index
- affect atmosphere
- affect fog
- affect clouds
- visible sun disk
- shadow softness
- cloud shadow strength later
- azimuth/elevation editor-facing controls derived from transform direction
- optional shadow bounce and volumetric shadow bounce controls when supported by renderer settings

Editor behavior:

- Appears in Hierarchy as `Sun Light`.
- Has a sun direction viewport gizmo.
- Supports Ctrl+L style sun direction control later.
- Can be assigned as the scene primary sun.
- Direction/intensity changes reset accumulation.
- Sun/atmosphere binding changes rebuild atmosphere lighting when needed.
- Inspector layout follows the reference Sun Light UI: `Sun` light type, `Illuminance (lux)` units, azimuth, elevation, color temperature, temperature, intensity, exposure multiplier, softness, cast surface shadows, cast volumetric shadows, and bounce controls where implemented.

### Point Light Component

Properties:

- enabled
- color
- color temperature
- intensity
- intensity unit: lumen / candela / renderer unit
- radius
- attenuation range
- cast shadows
- cast volumetric shadows
- visible in reflections
- visible to camera optional
- importance sampling weight

Viewport gizmo:

- light icon
- radius sphere
- selected bounds

### Spot Light Component

Properties:

- enabled
- color
- color temperature
- intensity
- inner cone angle
- outer cone angle
- radius
- range
- cast shadows
- cast volumetric shadows
- IES profile optional
- barn doors later

Viewport gizmo:

- spot cone
- radius handle
- direction arrow

### Area Light Component

Supported types:

- rectangle
- disk
- sphere

Properties:

- enabled
- shape
- color
- color temperature
- intensity
- intensity unit
- width
- height
- radius
- two-sided
- cast shadows
- visible to camera
- visible in reflections
- importance sampling weight
- material source for emissive/visible fixture representation
- IES profile slot when the selected shape or renderer path supports it

Renderer behavior:

- Updates light buffer.
- Rebuilds light sampling table when topology or area-light count changes.
- Resets accumulation after meaningful light edits.

Editor behavior:

- Inspector layout follows the reference area-light UI: shape/type selector such as `Area Disk`, physical units such as `LuminousPower (lumen)`, IES profile field, color temperature toggle, RGB/color swatch controls, intensity, exposure multiplier, cone/radius controls when applicable, visible-to-camera toggle, material source field, and shadow toggles.
- Selected area lights show a visible viewport gizmo/handle over the rendered image.

### Environment Light Component

Properties:

- HDRI asset GUID
- intensity
- rotation
- background visible
- background intensity
- affect diffuse
- affect specular
- importance sampling enabled
- environment CDF status

Renderer behavior:

- HDRI change rebuilds environment CDF.
- Intensity/rotation change resets accumulation.
- HDRI asset reference is saved by GUID.

### Sky Atmosphere Component

Properties:

- enabled
- planet radius
- atmosphere height
- Rayleigh scattering
- Rayleigh scale height
- Mie scattering
- Mie scale height
- Mie anisotropy
- ozone absorption
- ground albedo
- multi-scattering
- transmittance LUT settings
- sky-view LUT settings
- aerial perspective LUT settings
- bound sun light

Renderer behavior:

- Atmosphere parameter changes rebuild LUTs.
- Sun binding changes update atmosphere sun direction.
- Atmosphere exists as an entity in the hierarchy.

### Height Fog Component

Properties:

- enabled
- density
- height falloff
- base height
- color
- inscattering intensity
- anisotropy
- max distance
- affect sky
- affect transparent objects later
- volumetric enabled

Renderer behavior:

- Fog edits reset accumulation.
- Fog participates in path tracing/volume integration when enabled.

### Volumetric Cloud Component

Properties:

- enabled
- cloud asset/noise asset
- coverage
- density
- height range
- wind direction
- wind speed
- phase anisotropy
- shadow strength
- sample count
- temporal accumulation enabled

This component can be deferred, but the component slot should be reserved because the target editor needs world/atmosphere-style scene controls.

### Post Processing Components

Post-processing must be split into camera exposure/lens properties, global post-process defaults, and `PostProcessVolumeComponent` overrides with blending.

Required components/data:

- PostProcessVolumeComponent
- CameraPostProcessComponent or CameraComponent post-process section
- GlobalPostProcessSettings as scene-level defaults

Camera inspector reference layout:

- Camera section exposes near clip, exposure mode, aperture size, ISO, shutter speed, film size, and focal length.
- Camera/post-process settings are grouped into collapsible sections such as DOF, Bloom, Color correction, Vignetting, and Film grain.
- Motion blur, DOF, Bloom, and Film grain use enable checkboxes before detailed controls.
- Physical camera controls and post-process controls may initially live on `CameraComponent`, but the ownership must be clear enough to migrate to `CameraPostProcessComponent` without changing saved scene meaning.

### PostProcessVolumeComponent

Core properties:

- enabled
- priority
- blend radius
- blend weight
- infinite extent / unbound
- volume shape: box/sphere
- affects editor camera
- affects game camera
- override flags per property

Override data model:

```cpp
template <typename T>
struct PostProcessOverride {
    bool overrideEnabled = false;
    T value{};
};

struct PostProcessVolumeComponent {
    bool enabled = true;
    float priority = 0.0f;
    float blendRadius = 0.0f;
    float blendWeight = 1.0f;
    bool unbound = false;

    PostProcessOverride<float> exposureCompensation;
    PostProcessOverride<float> bloomIntensity;
    PostProcessOverride<float> saturation;
    PostProcessOverride<float> vignetteIntensity;
};
```

Exposure properties:

- exposure mode: manual / auto
- EV compensation
- ISO
- shutter speed
- aperture
- min EV
- max EV
- adaptation speed up
- adaptation speed down
- metering mode later

Tonemapping properties:

- tonemapper type: ACES / AgX / Filmic / custom
- exposure
- contrast
- shoulder
- toe
- saturation
- white point
- display gamma
- output color space

Bloom properties:

- enabled
- threshold
- intensity
- radius
- dirt texture later
- dirt intensity later

Color grading properties:

- temperature
- tint
- contrast
- saturation
- gamma
- gain
- lift
- offset
- shadows/midtones/highlights later
- LUT texture later

Depth of field properties:

- enabled
- focus distance
- aperture / f-stop
- focal length
- sensor size
- near blur
- far blur
- blade count later

Motion blur properties:

- enabled
- shutter angle
- sample count
- max velocity

Vignette / film properties:

- vignette intensity
- vignette radius
- film grain intensity
- chromatic aberration optional
- sharpen optional

Renderer behavior:

- Post-process changes must not rebuild TLAS or BLAS.
- Most post-process changes do not need accumulation reset unless exposure participates before accumulation.
- Exposure and tonemap changes should apply after path tracing when possible.
- Camera-specific settings override global scene defaults.
- PostProcessVolume blends by priority and camera position.

### Lighting/Post-Process Data Ownership

- Scene entities own editable lighting and world objects: Sun Light, Point Light, Spot Light, Area Light, Environment Light, Sky Atmosphere, Height Fog, Volumetric Cloud, and Post Process Volume.
- Scene `WorldSettings` stores defaults and active bindings: active environment, primary sun, active sky atmosphere, default exposure, default tonemapper, and default GI/render quality.
- `Render Settings` stores technical renderer options: samples, bounces, debug view, denoiser, TAA, ReSTIR, OMM/SER, and resolution scale.
- Lighting and post-processing should not live only in `Render Settings`; they must be scene-editable components with transform, hierarchy presence, inspector UI, save/load, undo/redo, and renderer dirty-flag integration.

### Component Enable/Disable

- Disabled `MeshRenderer` is not rendered and is not included in TLAS.
- Disabled lights are not included in light buffers or the light sampling table.
- Disabled atmosphere, fog, environment, cloud, and post-process components do not affect rendering.
- Disabling a component marks the correct dirty flags and resets accumulation if visible output changes.

### Default Scene Template

New Scene creates:

- Root entity
- Editor camera or scene camera
- Sun Light
- Environment Light
- Sky Atmosphere optional
- Height Fog disabled by default
- Global `WorldSettings`
- Default post-process settings

### Renderer Sync Contract

- UI never directly mutates GPU resources.
- UI modifies `SceneDocument` through `SceneOperations` or explicit editor requests.
- Scene changes emit dirty flags and dirty reasons.
- The scene-to-renderer builder converts dirty scene state into renderer update requests.
- The renderer applies requests at a safe frame boundary.

### Renderer Rebuild Granularity

- Camera moved: reset accumulation only.
- Object transform changed: update instance transform and refit/rebuild TLAS as needed.
- Material value changed: update material buffer and reset accumulation.
- Texture changed: update texture resource/descriptors and reset accumulation.
- Mesh imported: build BLAS.
- Mesh assigned: update entity mesh reference and TLAS.
- Light changed: update light buffer and reset accumulation.
- HDRI changed: rebuild environment CDF and reset accumulation.
- Atmosphere changed: rebuild atmosphere LUTs and reset accumulation.

### Content Root Rules

- Imported assets must live under `Content/`.
- Cache/generated files must live under `Cache/`.
- Scenes should live under `Scenes/` or `Content/Scenes/`.
- `.rtlevel` files should store relative asset GUID references and project-relative paths, not absolute paths except fallback/debug metadata.
- If no project is open, use a temporary/default Content root.

### No-Project Mode

- Editor can still open old `.rtlevel` scenes.
- Content browser uses a default workspace folder.
- `Import Asset` asks for a destination folder.
- Asset registry is stored next to the scene or in the default workspace.
- Project manager later migrates no-project data into the project structure.

### OBJ Import V1

- Add OBJ import after glTF asset import works.
- V1 support: geometry, MTL materials, diffuse texture, normal map if present, split by material, generated normals/tangents, and generated prefab.

### Thumbnail V1

- Use type icons first.
- Generate texture thumbnails from source images.
- Store thumbnail path in `AssetRecord`.
- Add material thumbnails using a preview sphere later.
- Add mesh/prefab thumbnails later through offscreen preview rendering.

### Safe Delete

- Check asset references before delete.
- Show the scenes, prefabs, materials, or other assets using the target.
- Allow cancel.
- Allow force delete, replacing references with missing asset placeholders.

### Navigation Helpers

- From Inspector asset fields: Find in Content.
- From Content assets: Find References and Show Dependencies.
- From Hierarchy entities: Focus in Viewport.
- From selected mesh/material: Show Source Asset.

### Dirty Scene Prompt

- Prompt before destructive scene replacement actions when the current scene is dirty.
- Required choices: Save, Do Not Save, Cancel.
- Required for New Scene, Open Scene, Import Scene as New Scene, and Exit.

## Async Scene Loading

Scene loading should be asynchronous for large `.rtlevel`, glTF, and GLB scenes so the editor UI remains responsive.

### Rule

Async scene loading produces a staged CPU-side scene result. It must not mutate the active scene or renderer directly.

### Supported V1 Operations

- Open `.rtlevel`
- Import Scene as New Scene
- Merge Scene into Current
- Load project startup scene
- Later: Import Asset
- Later: Reimport Asset

### Worker Thread Responsibilities

Allowed:

- File IO
- `.rtlevel` JSON parsing
- glTF/GLB parsing
- CPU-side mesh/material/texture metadata extraction
- Staged `SceneDocument` creation
- Import report generation
- Project-relative path resolving from a copied `ProjectContext`
- Asset GUID resolving from a read-only asset registry snapshot

Not allowed:

- Mutating active `SceneDocument`
- Mutating editor selection
- Mutating undo/redo stack
- Calling ImGui
- Creating/destroying Vulkan resources
- Touching `PathTracerRenderer`
- Touching `CommandSystem`
- Calling `vkDeviceWaitIdle`

### Main Thread Apply

Completed async results are applied only by `Application`.

Open Scene / Import Scene as New Scene:

- Prompt dirty scene before starting the load.
- Load staged scene on worker.
- Swap active `SceneDocument` on main thread.
- Clear selection.
- Clear undo stack.
- Rebuild renderer through existing renderer recreation/update route.
- Reset accumulation.
- Mark scene clean for opened `.rtlevel`.
- Mark scene dirty for imported unsaved scene.

Merge Scene into Current:

- Load staged scene on worker.
- Append staged entities under a new import root.
- Remap entity IDs and stable UUIDs.
- Preserve existing scene entities.
- Mark scene dirty.
- Rebuild/update renderer through existing scene update route.
- Reset accumulation.

### Async Scene Loading Types

```cpp
enum class SceneLoadMode {
    OpenRtLevel,
    ImportSceneAsNewScene,
    MergeSceneIntoCurrent,
    LoadProjectStartupScene,
};

enum class SceneLoadStatus {
    Idle,
    Queued,
    Loading,
    WaitingForApply,
    Applying,
    Completed,
    Failed,
    Cancelled,
};

struct SceneLoadRequest {
    SceneLoadMode mode;
    std::filesystem::path sourcePath;
    ProjectContext projectSnapshot;
    bool preserveHierarchy = true;
    bool importMaterials = true;
    bool importTextures = true;
    bool importLights = true;
    bool importCameras = true;
};

struct SceneLoadResult {
    SceneLoadMode mode;
    std::filesystem::path sourcePath;
    bool success = false;
    std::string errorMessage;
    std::unique_ptr<SceneDocument> stagedScene;
    ImportReport report;
    bool needsRendererRebuild = true;
    bool needsGpuSceneRebuild = true;
    bool needsTlasRebuild = true;
    bool needsEnvironmentCdfRebuild = false;
    bool needsAtmosphereLutRebuild = false;
};

class AsyncSceneLoader {
public:
    bool start(SceneLoadRequest request);
    void requestCancel();
    bool isRunning() const;
    SceneLoadStatus status() const;
    float progress() const;
    std::string stage() const;
    bool hasCompletedResult() const;
    SceneLoadResult takeCompletedResult();

private:
    std::future<SceneLoadResult> future_;
    std::atomic<bool> running_ = false;
    std::atomic<bool> cancelRequested_ = false;
    std::atomic<float> progress_ = 0.0f;
    std::atomic<SceneLoadStatus> status_ = SceneLoadStatus::Idle;
};
```

V1 limitation: only one scene load job may run at a time.

### Loading Overlay

Show a modal or viewport overlay while loading with operation name, source path, current stage, progress percent, and Cancel button.

Stages: Reading file, Parsing scene, Resolving assets, Loading materials, Loading textures, Building CPU scene, Waiting for main thread, Applying scene, Rebuilding renderer, Done.

### Cancellation

Cancellation is cooperative. Check the cancel flag after file read, after JSON/glTF parse, between meshes, between texture decodes, and before final staged scene creation. Cancelled loads leave the current scene unchanged.

### Async Project Startup Scene Loading

- Opening a project loads `.rtproject` synchronously because it is small.
- Loading the startup scene may run through `AsyncSceneLoader`.
- If startup scene loading fails, show Project Manager or open an empty scene with an error in Log.
- If the user cancels startup scene loading, keep the project open but no active scene loaded.

### Import Jobs Later

- Large imports should eventually run as background jobs.
- Import operations can run asynchronously and show progress.
- Import can be cancelled before GPU upload/cache finalization.
- Import logs stream into the Log panel.
- Import Report is produced after completion.
- CPU import may happen on worker threads.
- GPU upload and renderer integration happen on safe main-thread/frame-boundary paths.

### Asset Registry Thread Safety

- Worker threads may read from a copied/snapshot asset registry.
- Worker threads must not mutate the live asset registry.
- Asset imports/reimports produce staged asset registry changes.
- Main thread applies staged registry changes.
- Applying registry changes marks `AssetRegistryState.dirty`.

## Material Editing Ownership

- Editing a `MaterialAsset` changes every object using that material asset.
- Editing a `MaterialInstance` changes only users of that material instance.
- Per-entity material overrides are stored on the scene entity or prefab instance.
- Inspector must clearly show whether the user is editing source material asset, material instance, or selected entity override.
- Imported glTF materials become `MaterialAsset`s.
- Users can create a `MaterialInstance` from an imported material before making local edits.
- Dragging a material onto an object assigns the material reference.
- Editing a material slot override must not mutate the source imported material unless explicitly requested.

## Editor ID Buffer

- Renderer outputs stable entity/object IDs for viewport picking.
- Selection outline is based on selected entity UUIDs, not only transient instance index.
- Imported prefab children receive stable entity UUIDs.
- Object ID, Material ID, and Instance ID debug views use the same stable ID mapping.
- Reimport should preserve ID mapping where possible.
- If an entity cannot be matched after reimport, assign a new UUID and record a warning in the Import Report.

## Scene Tabs V1

- V1 supports one open scene at a time.
- The scene tab displays current scene name and dirty marker.
- Closing the scene tab triggers dirty scene prompt.
- Multiple scene tabs are deferred.
- Opening another scene replaces the current scene only after Save / Do Not Save / Cancel.

## Editor Config

Stored in `Config/EditorConfig.json`:

- open last project enabled
- last opened project
- last opened scene
- recent projects
- recent scenes
- default import options
- UI scale
- theme
- layout version
- viewport camera speed
- autosave preferences
- last selected workspace
- Content Browser view mode

## Current Implementation Baseline

This plan starts from the current editor state, not from a blank slate. Treat these systems as existing foundations:

- `SceneDocument` owns editable scene data and `.rtlevel` JSON save/load.
- `SceneRegistry`, `SceneOperations`, and `UndoStack` already support entity create, duplicate, delete, reparent, visibility, lock, transform, camera, light, sun, and mesh-renderer component edits.
- `EditorRequests` currently routes `loadGltf`, `loadHdr`, `saveSceneJson`, `loadSceneJson`, renderer settings, component edits, layout/editor commands, undo, redo, and shader reload.
- `Application::requestGltfSceneLoad` already performs async CPU glTF loading, while `commitLoadedGltfScene` applies the completed replacement scene and rebuilds renderer resources on the main thread.
- `EditorDockspace` currently exposes `File`, `Edit`, `View`, and `Render` menus and docks `Viewport`, `Scene Hierarchy`, `Inspector / Properties`, `Asset Browser`, `Render Settings`, and `Debug / Profiler`.
- `AssetBrowserPanel` is currently a load-oriented panel with glTF/HDR/level fields, favorites, recent files, texture thumbnails, mesh list, material list, and material drag/drop.

Treat everything else as unimplemented until a later step adds it. In particular, there is no current `.rtproject`, `ProjectContext`, project asset registry, GUID-backed asset reference model, prefab asset model, `Import Asset`, `Import and Place`, `Merge Scene`, `Render World Settings`, Timeline panel, Log panel, Console panel, or autosave/recovery flow.

## Detailed Step-By-Step Implementation Plan

Each step must land as a working vertical slice. Do not move to the next step until the current step builds, editor mode still opens, headless mode still runs when touched, and the listed validation gate passes. Keep old request names as compatibility aliases until all call sites have migrated.

For every implementation step, record the exact verification evidence in the change notes or PR description. The minimum evidence is the build command used, the editor smoke or manual editor checks that were run, and the JSON outputs from any applicable scripts under `out/editor_tools/`. If a gate is not run because the local environment cannot build or launch the editor, record the reason and the highest-value static tools that were run instead.

Use the editor tooling documented in `docs/AI_DEBUG_PROFILING_TOOLS_USAGE.md` as milestone gates, not as optional cleanup. The most important mapping is:

- UI label, panel, dock, menu, and request-routing work: `imgui_panel_audit.ps1`, `editor_request_flow_report.ps1`, and `editor_state_snapshot.ps1`.
- Project manager, project context, asset registry, import request, prefab, command registry, async loading, and autosave/recovery work: `editor_plan_audit.ps1` plus the relevant targeted validator below.
- `.rtlevel` schema, compatibility, save/load, GUID migration, and backup work: `rtlevel_schema_validator.ps1`, `rtlevel_compat_test.ps1`, and `migration_backup_test.ps1`.
- Project fixtures and asset registry work: `project_fixture_generator.ps1` and `asset_registry_validator.ps1`.
- Import, import-and-place, merge, and non-mutating import work: `scene_entity_count_probe.ps1` and `import_regression_harness.ps1`.
- Shared editor request handling, scene loading, renderer synchronization, or diagnostic output paths: `editor_smoke.ps1` and the canonical Cornell headless diagnostic smoke.

Treat failing static audits as useful progress signals during early milestones. Only enable failure switches such as `-FailOnInvalid` or `-ExpectImplemented` after the corresponding feature is intended to be complete for that milestone.

### Step 0 - Baseline And Safety Checks

Goal: capture the current behavior before changing editor workflows.

Implementation tasks:

1. Build Debug and Release if the local environment is available.
2. Run the required Cornell headless diagnostic smoke when renderer/shared request code will be touched.
3. Manually launch editor mode and verify viewport rendering, hierarchy selection, inspector edits, asset browser loads, save/load `.rtlevel`, glTF replacement, undo/redo, and shader reload.
4. Record any pre-existing failures before making editor changes.

Primary files to inspect: `include/rtv/EditorPanels.h`, `src/rtv/EditorDockspace.cpp`, `src/rtv/Application.cpp`, `src/rtv/SceneDocument.cpp`, `src/rtv/AssetBrowserPanel.cpp`.

Recommended tooling:

```powershell
.\scripts\imgui_panel_audit.ps1 -JsonOut out\editor_tools\baseline_imgui_audit.json
.\scripts\editor_request_flow_report.ps1 -JsonOut out\editor_tools\baseline_request_flow.json
.\scripts\editor_state_snapshot.ps1 `
  -Scene scenes\validation\cornell.rtlevel `
  -JsonOut out\editor_tools\baseline_state_snapshot.json
.\scripts\rtlevel_schema_validator.ps1 `
  -Path scenes\validation\cornell.rtlevel `
  -JsonOut out\editor_tools\baseline_rtlevel_schema.json
```

Validation gate: no behavior changes; baseline is known; pre-existing audit warnings are recorded before implementation starts.

### Step 1 - Rename Current glTF Replacement To Import Scene As New Scene - Completed 2026-05-31

Goal: make the existing destructive glTF workflow honest before adding real asset import.

Implementation tasks:

1. Add `importSceneAsNewScene` to `EditorRequests` while keeping `loadGltf` as an internal compatibility field.
2. Rename visible UI actions from `Open glTF` / `Load glTF` to `Import Scene as New Scene`.
3. Route `importSceneAsNewScene` to the existing `requestGltfSceneLoad` path in `Application`.
4. Update status strings, notifications, and logs to say the active scene will be replaced.
5. Keep recent/favorite file behavior unchanged.

Primary files: `EditorPanels.h`, `EditorDockspace.cpp`, `AssetBrowserPanel.cpp`, `Application.cpp`.

Recommended tooling:

```powershell
.\scripts\imgui_panel_audit.ps1 -JsonOut out\editor_tools\step01_imgui_audit.json
.\scripts\editor_request_flow_report.ps1 -JsonOut out\editor_tools\step01_request_flow.json
```

Validation gate: glTF still replaces the active scene exactly as before; no asset registry files are created; headless `--gltf` is unchanged; visible stale labels such as `Open glTF` and `Load glTF` are removed or documented as compatibility-only internal labels.

Completion notes 2026-05-31:

- Added `EditorRequests::importSceneAsNewScene` and routed it to the existing async glTF scene replacement path while preserving `loadGltf` as a compatibility alias.
- Renamed visible UI actions/status strings from generic glTF loading to `Import Scene as New Scene`.
- Kept headless `--gltf` untouched and did not create asset registry records from this replacement path.

### Step 2 - Dirty Scene State, Scene Name, And Replacement Prompt - Partially Completed 2026-05-31

Goal: prevent accidental scene loss before project/import workflows grow.

Implementation tasks:

1. Track current scene path and display name separately from `sourceGltfPath`.
2. Use `SceneDocument::dirty`, `markDirty`, and a new dirty reason list as the first dirty-state source of truth.
3. Add a Save / Do Not Save / Cancel modal for New Scene, Open Scene, Import Scene as New Scene, dropped glTF, Exit, and project close.
4. Add a single scene tab or viewport header showing scene name and `*` when dirty.
5. Ensure Cancel does not queue async loading and does not change selection, undo stack, renderer, or scene data.
6. Clear dirty state only after successful save or successful load of a clean scene.

Primary files: `SceneDocument.h`, `SceneDocument.cpp`, `Application.h`, `Application.cpp`, `ViewportPanel.cpp`, `EditorDockspace.cpp`.

Recommended tooling:

```powershell
.\scripts\editor_request_flow_report.ps1 -JsonOut out\editor_tools\step02_request_flow.json
.\scripts\editor_state_snapshot.ps1 `
  -Scene scenes\validation\cornell.rtlevel `
  -JsonOut out\editor_tools\step02_state_snapshot.json
```

Validation gate: replacement operations prompt when dirty; Cancel leaves the current scene, selection, undo stack, pending async load, renderer resources, and current request state untouched; Save writes before continuing; Do Not Save continues without writing; the dirty marker clears only after a successful save or clean scene load.

Partial completion notes 2026-05-31:

- Added an editor dirty-scene protection flow for destructive editor-origin actions: New Scene, Open Scene, Import Scene as New Scene, dropped glTF/GLB, Exit, and Project Close.
- Prompt choices are Save, Do Not Save, and Cancel. Save writes the current scene or opens Save Scene As for untitled scenes before continuing; Cancel leaves the active scene and pending async load state unchanged.
- Added an editor-level unsaved scene flag so the dirty marker and destructive-action prompt are not lost when `SceneDocument::dirty()` is cleared by renderer update routing.
- Active scene name and dirty marker remain visible in the top menu/title strip.
- Remaining work: apply the same prompt to scene-tab close after closable scene tabs exist; expand dirty reasons into a persistent reason list.

### Step 3 - Explicit Scene File Requests - Completed 2026-05-31

Goal: separate scene file commands from import commands before adding projects.

Implementation tasks:

1. Add explicit requests: `newScene`, `openScene`, `saveScene`, `saveSceneAs`, and `importSceneAsNewScene`.
2. Keep `loadSceneJson` and `saveSceneJson` as temporary aliases while call sites migrate.
3. Add `New Scene` behavior that creates an empty scene with default camera, primary sun, environment, and world settings placeholder.
4. Add consistent scene command result reporting for success, warning, and error messages.
5. Keep direct `.rtlevel` compatibility mode working without a project.

Primary files: `EditorPanels.h`, `EditorDockspace.cpp`, `Application.cpp`, `SceneOperations.cpp`.

Recommended tooling:

```powershell
.\scripts\editor_request_flow_report.ps1 -JsonOut out\editor_tools\step03_request_flow.json
.\scripts\rtlevel_schema_validator.ps1 `
  -Path scenes\validation `
  -JsonOut out\editor_tools\step03_rtlevel_schema.json
.\scripts\rtlevel_compat_test.ps1 `
  -Path scenes\validation `
  -JsonOut out\editor_tools\step03_rtlevel_compat.json
```

Validation gate: Open Scene loads `.rtlevel`; Save Scene writes current scene path; Save Scene As updates current scene path; New Scene works after dirty prompt approval; direct no-project `.rtlevel` compatibility remains intact.

Completion notes 2026-05-31:

- Added explicit request fields for `newScene`, `openScene`, `saveScene`, `saveSceneAs`, `importAsset`, `importAndPlace`, `importSceneAsNewScene`, and `mergeScene`.
- Routed Open Scene, Save Scene, Save Scene As, and New Scene through `Application` while keeping legacy request aliases during migration.
- Added non-mutating skeleton notifications for Import Asset, Import and Place, and Merge Scene; these do not modify the active scene yet.
- New Scene creates the current default editable scene scaffold and is gated by dirty-scene confirmation when needed.

### Step 4 - Near-Clone Shell, Menus, Dock Layout, And Theme - Completed 2026-05-31

Goal: align the editor shell with the target layout without changing scene data behavior.

Implementation tasks:

1. Rename visible panels: `Viewport` -> `Scene`, `Scene Hierarchy` -> `Hierarchy`, `Inspector / Properties` -> `Inspector`, `Asset Browser` -> `Content`.
2. Add visibility flags for `renderWorldSettings`, `timeline`, `log`, and `console`.
3. Add functional placeholder panels for `Render World Settings`, `Timeline`, `Log`, and `Console`.
4. Rebuild default dock layout: center `Scene`, right top `Hierarchy` plus `Render World Settings`, right bottom `Inspector`, bottom `Content` plus `Timeline` plus `Log`.
5. Replace menus with `File`, `Create`, `Engine`, `Window`, `Render`, and `Layout`.
6. Move existing actions into the new menu groups without dropping debug/profiler access.
7. Apply the dense dark theme and add layout version reset for renamed panels.

Primary files: `EditorPanels.h`, `EditorDockspace.h`, `EditorDockspace.cpp`, `EditorLayer.cpp`, new placeholder panel files if needed.

Recommended tooling:

```powershell
.\scripts\imgui_panel_audit.ps1 -JsonOut out\editor_tools\step04_imgui_audit.json
.\scripts\editor_state_snapshot.ps1 -JsonOut out\editor_tools\step04_state_snapshot.json
```

Validation gate: Reset Layout restores target layout; existing renderer/debug/profiler panels remain reachable; viewport render extent still follows panel size; old dock labels are handled by layout version reset rather than leaving duplicate stale panels.

Completion notes 2026-05-31:

- Renamed visible panels to `Scene`, `Hierarchy`, `Inspector`, and `Content`.
- Added `Render World Settings`, `Timeline`, `Log`, and `Console` visibility flags and functional shell panels.
- Rebuilt the default dock layout around center `Scene`, right-side `Hierarchy` / `Render World Settings` plus `Inspector`, and bottom `Content` / `Timeline` / `Log` tabs.
- Replaced the main menus with `File`, `Create`, `Engine`, `Window`, `Render`, and `Layout`, while keeping technical render/debug/profiler tools reachable.
- Applied a denser dark ImGui theme with restrained blue accents and compact spacing.

### Step 5 - Viewport Toolbar, Status Strip, And Shortcuts - Partially Completed 2026-05-31

Goal: make the viewport usable as the primary level editing surface.

Implementation tasks:

1. Preserve renderer image sizing, mouse state, picking, selected instance ID, accumulation reset, and ImGuizmo behavior.
2. Add compact toolbar controls for Select, Move, Rotate, Scale, Local/World, Snap, Grid, Axes, View Settings, Stats, and Draw Debug.
3. Route toolbar state to existing transform gizmo, local/world mode, snap, grid, and axes state.
4. Replace the large HUD with compact top-right status: CPU ms, GPU ms, samples, accumulation limit, debug/view mode, render/display resolution, render scale, denoiser/TAA state, reset reason, and camera speed.
5. Add non-conflicting shortcuts: `Q/W/E/R` tool modes when not navigating, `G` grid, `F` frame selected, and `F1-F8` debug views.

Primary files: `ViewportPanel.cpp`, `ViewportPanel.h`, `KeyBindings.cpp`.

Recommended tooling:

```powershell
.\scripts\imgui_panel_audit.ps1 -JsonOut out\editor_tools\step05_imgui_audit.json
.\scripts\editor_state_snapshot.ps1 `
  -Scene scenes\validation\cornell.rtlevel `
  -JsonOut out\editor_tools\step05_state_snapshot.json
```

Validation gate: picking still selects; gizmo edits commit one undoable transform command; navigation shortcuts still work; status values match renderer state.

Partial completion notes 2026-05-31:

- Renamed the viewport window to `Scene` and preserved renderer image sizing, picking, selected instance ID, accumulation reset state, and ImGuizmo path.
- Replaced the larger HUD with a compact top-right status strip showing CPU/GPU frame timing, samples, accumulation limit, debug view, render/display resolution, render scale, denoiser/TAA state, reset reason, and camera speed.
- Added compact toolbar controls for Select, Move, Rotate, Scale, Local/World, Snap, Grid, and Axes using short labels/tooltips.
- Added Q/W/E/R/G shortcut behavior for tool/grid switching while preserving legacy T/R/S/L shortcuts.
- Remaining work: frame-selected shortcut, F1-F8 debug-view registry, and shared command/keybinding routing remain pending Step 14.

### Step 6 - Project Manager MVP - Completed 2026-05-31

Goal: add project roots without breaking standalone `.rtlevel` editing.

Implementation tasks:

1. Add `ProjectContext`, `CreateProjectRequest`, `OpenProjectRequest`, and project settings serialization.
2. Add `.rtproject` load/save with `version`, `projectGuid`, paths, `startupScene`, default preset, and autosave fields.
3. Create and validate `Content`, `Scenes`, `Cache`, `Saved`, `Config`, `Build`, and asset registry paths.
4. Add Project Manager startup window with New Project, Open Project, Recent Projects, Browse, Remove from Recent, and Open Last Project.
5. Store recent projects and last opened project in global editor config first.
6. Add close project flow: dirty scene prompt, dirty registry placeholder, layout/config save, scene clear or return to Project Manager, and Content state clear.
7. Keep no-project compatibility mode for old `.rtlevel` files.

Primary files: new project files, `EditorPanels.h`, `EditorDockspace.cpp`, `EditorLayer.cpp`, `Application.cpp`, `EditorPreferences.h`.

Recommended tooling:

```powershell
.\scripts\editor_plan_audit.ps1 -JsonOut out\editor_tools\step06_plan_audit.json
.\scripts\project_fixture_generator.ps1 `
  -Name ToolingSmoke `
  -Template PathTracedDefault `
  -Force `
  -JsonOut out\editor_tools\step06_fixture.json
```

Validation gate: new project creates the expected folders; opening a project sets all roots; recent projects survive restart; old `.rtlevel` opens without a project; Project Manager failures do not block headless execution.

Completion notes 2026-05-31:

- Added `ProjectContext`, `CreateProjectRequest`, `OpenProjectRequest`, `.rtproject` load/save, and project GUID generation.
- New projects create the expected `Content`, `Scenes`, `Cache`, `Saved`, `Config`, `Build`, and initial `Content/AssetRegistry.json` paths.
- Project Manager UI supports New Project, Open Project, Browse, Recent Projects, Remove from Recent, Open Last Project, and Continue Without Project compatibility mode.
- Recent projects, last opened project, and Open Last Project preference are stored in global editor preferences.
- Application can create, open, auto-open-last, save settings, and close projects while preserving no-project `.rtlevel` compatibility.
- Project close is gated by the dirty-scene prompt and returns to no-project fallback scene state.
- Remaining follow-up: detailed Project Settings sections are still deferred to the project settings/polish work.

### Step 7 - Project Asset Registry Skeleton - Completed 2026-05-31

Goal: create stable project asset identity before importing reusable assets.

Implementation tasks:

1. Add `AssetGuid`, `AssetType`, `AssetRecord`, `AssetImportSettings`, `AssetDependency`, `AssetImportStatus`, and dirty reason types.
2. Store registry JSON at `Project/Content/AssetRegistry.json`.
3. Include GUID, type, display name, source path, imported/cache path, thumbnail path, dependencies, references, source/import hashes, import settings hash, timestamp, missing/stale flags, and status.
4. Load or create registry on project open.
5. Save registry on project close and explicit project settings save.
6. Display registry records in `Content` before thumbnails and drag/drop are implemented.

Primary files: new asset registry files, `Application.cpp`, Content panel files, project context files.

Recommended tooling:

```powershell
.\scripts\editor_plan_audit.ps1 -JsonOut out\editor_tools\step07_plan_audit.json
.\scripts\asset_registry_validator.ps1 `
  -RegistryPath out\project_fixtures\ToolingSmoke\Content\AssetRegistry.json `
  -JsonOut out\editor_tools\step07_asset_registry.json
```

Validation gate: registry survives restart; dirty state changes on record mutation; displaying registry records never mutates active scene; registry paths remain project-relative and normalized.

Completion notes 2026-05-31:

- Added `AssetGuid`, `AssetType`, `AssetRecord`, `AssetImportSettings`, `AssetDependency`, `AssetImportStatus`, registry dirty reasons, and `AssetRegistryState`.
- Added project-local asset registry JSON load/save at `Project/Content/AssetRegistry.json`, including GUID, type, display name, source path, imported/cache/thumbnail paths, dependencies, references, hashes, timestamp, import settings, status, missing, and stale flags.
- Project opening loads or creates the registry; project close and explicit project settings save persist dirty registry state.
- `Content` displays project registry path, dirty marker, and registry records without mutating the active scene.
- Registry validator passes for the generated ToolingSmoke fixture.

### Step 8 - Content Browser Project Integration

Goal: turn `Content` into a project browser instead of a file loader.

Implementation tasks:

1. Root `Content` at `Project/Content/` when a project is open.
2. In no-project mode, show compatibility state and prompt users to create/open a project for asset import.
3. Add folder tree, breadcrumb, back, forward, refresh, search, list/grid toggle, details panel, favorites, and recents.
4. Display registry metadata: type, GUID, source path, imported path, dependency count, reference count, missing/stale status.
5. Define double-click behavior: `.rtlevel` opens scene, `.gltf/.glb` imports scene as new scene until import mode selection exists, `.hdr/.exr` applies environment, registry records open details.

Primary files: Content panel files, asset registry files, `EditorLayer.cpp`, `Application.cpp`.

Recommended tooling:

```powershell
.\scripts\editor_plan_audit.ps1 -JsonOut out\editor_tools\step08_plan_audit.json
.\scripts\asset_registry_validator.ps1 `
  -RegistryPath out\project_fixtures\ToolingSmoke\Content\AssetRegistry.json `
  -JsonOut out\editor_tools\step08_asset_registry.json
```

Validation gate: project-relative content paths display correctly; moving the project folder preserves relative references; external files do not silently become project assets.

### Step 9 - Non-Mutating Import Asset Skeleton

Goal: prove `Import Asset` does not replace or edit the active scene.

Implementation tasks:

1. Add requests: `importAsset`, `importAndPlace`, `reimportAsset`, and `placeAsset`, but implement only `importAsset` first.
2. Add Import Settings dialog with mode, destination folder, hierarchy/material/texture/camera/light options, tangents, BLAS cache, unit scale, and coordinate conversion.
3. Add staged import result containing registry mutations, generated files, warnings, errors, and Import Report data.
4. For V1 skeleton, create registry records and placeholder imported/cache files only; do not create renderer resources or scene entities.
5. Apply staged registry changes on the main thread.
6. In no-project mode, strongly prompt to create/open project; if refused, use scene-adjacent `SceneName.assets.json` only as compatibility mode.

Primary files: `EditorPanels.h`, `Application.cpp`, Content panel files, asset registry/import files.

Recommended tooling:

```powershell
.\scripts\scene_entity_count_probe.ps1 `
  -Before scenes\validation\cornell.rtlevel `
  -JsonOut out\editor_tools\step09_entity_count.json
.\scripts\import_regression_harness.ps1 -JsonOut out\editor_tools\step09_import_harness.json
```

Validation gate: with Scene A open, importing model B does not change Scene A entity count; cancelling import leaves scene, registry, and project files unchanged except temp files; `Import Asset` never calls the whole-scene replacement path.

### Step 10 - glTF/GLB Import-As-Asset

Goal: turn glTF input into reusable project assets.

Implementation tasks:

1. Reuse `GltfLoader` and `SceneCache` where practical, but keep output staged and project-local.
2. Create records for textures, materials, meshes, and a source model/prefab root.
3. Write outputs under project `Content/Models/<Name>/...` and caches under `Project/Cache/...`.
4. Apply color-space rules: baseColor/emissive as sRGB, normal/roughness/metallic/occlusion as linear.
5. Apply metallic-roughness channel rules and warn for unsupported or lossy conversions.
6. Preserve source node hierarchy for prefab generation.
7. Deduplicate by source path, source hash, and import settings hash.

Primary files: glTF loader integration files, asset registry/import files, `SceneCache` integration files, Content panel files.

Recommended tooling:

```powershell
.\scripts\editor_plan_audit.ps1 -JsonOut out\editor_tools\step10_plan_audit.json
.\scripts\asset_registry_validator.ps1 `
  -RegistryPath out\project_fixtures\ToolingSmoke\Content\AssetRegistry.json `
  -JsonOut out\editor_tools\step10_asset_registry.json
.\scripts\import_regression_harness.ps1 `
  -ExpectImplemented `
  -JsonOut out\editor_tools\step10_import_harness.json
```

Validation gate: embedded textures, external textures, multiple materials, alpha mask, double-sided metadata, and node hierarchy import or warn deterministically.

### Step 11 - Prefab Generation, Placement, And Import And Place

Goal: place imported asset hierarchies into scenes using stable asset IDs.

Implementation tasks:

1. Add `PrefabAsset`, `PrefabInstance`, and `PrefabOverride` types.
2. Generate prefab data from imported glTF/GLB hierarchy with mesh/material asset GUID references.
3. Add `placeAsset` request that creates scene entities through `SceneOperations`.
4. Store prefab GUID, instance root, local overrides, and generated entity UUIDs.
5. Add drag/drop from Content to viewport and hierarchy.
6. Implement `Import and Place` as Import Asset plus prefab generation plus one placement.
7. Ensure material drag/drop assigns material references without mutating the source material asset.

Primary files: new prefab files, `SceneDocument`, `SceneOperations`, Content panel, `ViewportPanel`, `Application`.

Recommended tooling:

```powershell
.\scripts\scene_entity_count_probe.ps1 `
  -Before scenes\validation\cornell.rtlevel `
  -JsonOut out\editor_tools\step11_entity_count.json
.\scripts\import_regression_harness.ps1 `
  -ExpectImplemented `
  -JsonOut out\editor_tools\step11_import_harness.json
```

Validation gate: Import and Place increases entity count only by the placed prefab hierarchy; duplicate placements share source assets; per-instance edits do not mutate source prefab or material assets unless an explicit apply-to-source command is used.

### Step 12 - GUID-Backed `.rtlevel` Save/Load And Migration

Goal: make scenes durable across project moves and asset reimports.

Implementation tasks:

1. Add `RtLevelHeader` with format version, scene GUID, engine version, and project-relative metadata.
2. Add GUID-backed references for mesh, material, texture, prefab, environment, and source records.
3. Keep legacy `sourceGltf` and index-handle load support.
4. Save latest format when project asset registry is available.
5. Create backups before migrated scene overwrite.
6. Save/load world settings, active camera, primary sun, bookmarks, prefab instances, overrides, and dirty reasons.

Primary files: `SceneDocument.h`, `SceneDocument.cpp`, asset registry files, prefab files, project context files.

Recommended tooling:

```powershell
.\scripts\rtlevel_schema_validator.ps1 `
  -Path scenes\validation `
  -JsonOut out\editor_tools\step12_rtlevel_schema.json
.\scripts\rtlevel_compat_test.ps1 `
  -Path scenes\validation `
  -JsonOut out\editor_tools\step12_rtlevel_compat.json
.\scripts\migration_backup_test.ps1 `
  -Scene scenes\validation\cornell.rtlevel `
  -JsonOut out\editor_tools\step12_migration_backup.json
```

Validation gate: existing validation `.rtlevel` files load; migrated saves create backups; moved project folders preserve project-relative references; missing assets warn instead of crashing; migration never overwrites a legacy scene without a recoverable backup.

### Step 13 - Merge Scene Into Current

Goal: add append behavior as a separate workflow from replacement and import.

Implementation tasks:

1. Add `mergeScene` request and UI entry.
2. Load source data into a staged CPU result with explicit load mode metadata.
3. Append entities under a new import root.
4. Remap `EntityId`, stable UUIDs, prefab instance IDs, and asset references.
5. Register or reuse imported assets by GUID/source/import settings.
6. Wrap the append in one undo transaction.
7. Rebuild renderer resources only after successful main-thread apply.

Primary files: `Application`, `SceneDocument`, `SceneOperations`, asset registry/import files.

Recommended tooling:

```powershell
.\scripts\scene_entity_count_probe.ps1 `
  -Before scenes\validation\cornell.rtlevel `
  -JsonOut out\editor_tools\step13_entity_count.json
.\scripts\import_regression_harness.ps1 `
  -ExpectImplemented `
  -JsonOut out\editor_tools\step13_import_harness.json
```

Validation gate: existing scene entities remain unchanged; merge failure leaves scene and renderer unchanged; undo removes the merged root; the renderer rebuild is triggered only after the staged merge is applied on the main thread.

### Step 14 - Command Registry And Keybinding Registry

Goal: stop menus, toolbar buttons, shortcuts, and console commands from growing separate action paths.

Implementation tasks:

1. Add `EditorCommandId`, `EditorCommand`, `EditorKeybinding`, and command context types.
2. Register commands for scene, project, import, create, transform, viewport, render/debug, layout, and tool actions.
3. Route menus, toolbar buttons, shortcuts, and later console commands through the registry.
4. Add conflict detection and context precedence: text input, modal, viewport navigation, scene editing, global.
5. Keep existing shortcuts as compatibility registrations.

Primary files: new command/keybinding files, `EditorDockspace`, `ViewportPanel`, `EditorLayer`, `KeyBindings.cpp`.

Recommended tooling:

```powershell
.\scripts\imgui_panel_audit.ps1 -JsonOut out\editor_tools\step14_imgui_audit.json
.\scripts\editor_request_flow_report.ps1 -JsonOut out\editor_tools\step14_request_flow.json
.\scripts\editor_plan_audit.ps1 -JsonOut out\editor_tools\step14_plan_audit.json
```

Validation gate: existing shortcuts work; menu and toolbar actions trigger the same commands; viewport navigation keeps priority during mouse capture.

### Step 15 - Undo/Redo Coverage Expansion

Goal: make all authored scene changes undoable and visible.

Implementation tasks:

1. Keep `SceneOperations` as the mutation gateway.
2. Add command transactions for create, delete, duplicate, reparent, transform, component add/remove, property edit, material assignment, asset placement, prefab placement, and merge.
3. Group gizmo drag preview into one commit command.
4. Avoid pushing cancelled or no-op commands.
5. Surface undo/redo labels in menus and Log panel.

Primary files: `SceneOperations`, `UndoStack`, `ViewportPanel`, `InspectorPanel`, `SceneHierarchyPanel`.

Recommended tooling:

```powershell
.\scripts\editor_state_snapshot.ps1 `
  -Scene scenes\validation\cornell.rtlevel `
  -JsonOut out\editor_tools\step15_state_snapshot.json
.\scripts\scene_entity_count_probe.ps1 `
  -Before scenes\validation\cornell.rtlevel `
  -JsonOut out\editor_tools\step15_entity_count.json
```

Validation gate: undo/redo restores scene data and renderer-visible state; cancelled operations never enter the undo stack.

### Step 16 - Render World Settings And Lighting/Post-Process Components

Goal: separate artist-facing world controls from technical render settings.

Implementation tasks:

1. Add `WorldSettings` with active environment, primary sun, atmosphere, fog, and post-process references.
2. Add scene components for Sun, Point, Spot, Area, Environment Light, Sky Atmosphere, Height Fog, Volumetric Cloud shell, Post Process Volume, and Camera Post Process.
3. Extend Create menu, Hierarchy badges, and Inspector sections.
4. Add `Render World Settings` panel for HDRI, intensity, rotation, background, primary sun, sky/atmosphere, exposure, post-process summary, and GI summary.
5. Route renderer updates through scene dirty kinds and the rebuild granularity table.
6. Save/load and undo/redo all new component state.

Primary files: `SceneComponents`, `SceneDocument`, `SceneToGpuSceneBuilder`, `SceneUpdateRouter`, `InspectorPanel`, new `RenderWorldSettingsPanel`, `RenderSettingsPanel`.

Recommended tooling:

```powershell
.\scripts\imgui_panel_audit.ps1 -JsonOut out\editor_tools\step16_imgui_audit.json
.\scripts\editor_state_snapshot.ps1 `
  -Scene scenes\validation\cornell.rtlevel `
  -JsonOut out\editor_tools\step16_state_snapshot.json
.\scripts\rtlevel_schema_validator.ps1 `
  -Path scenes\validation `
  -JsonOut out\editor_tools\step16_rtlevel_schema.json
```

Validation gate: created lights appear in Hierarchy and Inspector; editing light/world settings updates renderer safely and resets accumulation when needed.

### Step 17 - General Async Scene Loading

Goal: replace the glTF-only async helper with a reusable scene loading model.

Implementation tasks:

1. Add `SceneLoadMode`, `SceneLoadStatus`, `SceneLoadRequest`, `SceneLoadResult`, and `AsyncSceneLoader`.
2. Support Open Scene, Import Scene as New Scene, Merge Scene, and Project Startup Scene.
3. Keep workers limited to CPU file IO, JSON/glTF parsing, dependency discovery, CPU import transforms, and staged result creation.
4. Forbid workers from touching active `SceneDocument`, ImGui, Vulkan, renderer, `AssetManager`, undo stack, or selection.
5. Apply completed results on the main thread at a safe frame boundary.
6. Add progress, loading overlay, cancellation, and Log reporting.
7. Preserve headless loading behavior.

Primary files: new `AsyncSceneLoader` files, `Application`, import/registry files, loading overlay UI.

Recommended tooling:

```powershell
.\scripts\editor_request_flow_report.ps1 -JsonOut out\editor_tools\step17_request_flow.json
.\scripts\rtlevel_compat_test.ps1 `
  -Path scenes\validation `
  -JsonOut out\editor_tools\step17_rtlevel_compat.json
.\scripts\editor_smoke.ps1 `
  -BuildDebug `
  -BuildRelease `
  -OutDir out\editor_smoke_step17
```

Validation gate: large scene open keeps UI responsive; cancelled/failed loads leave current scene unchanged; renderer rebuild happens only after main-thread apply.

### Step 18 - Log, Console, Timeline V1, Autosave, And Recovery

Goal: finish the editor infrastructure after project, registry, import, and scene persistence are stable.

Implementation tasks:

1. Add Log model and panel with info/warning/error/import/render/project/scene/command categories, filters, search, clear, copy, and open log file controls.
2. Route notifications, import reports, scene load failures, save/load errors, undo labels, and renderer warnings into Log.
3. Add Console panel with command input/history and commands routed through the command registry.
4. Add Timeline V1 with play, pause, stop, frame range, scrubber, selected-entity transform keyframes, save/load, and undo/redo.
5. Add autosave scheduling using project settings and write autosaves under `Project/Saved/Autosaves/`.
6. Add atomic save helpers, backups before migrations/destructive saves/reimports, crash markers, and recovery prompts.
7. Finish polish: layout versioning, UI scale, theme presets, workspace presets, and final density cleanup.

Primary files: new log/console/timeline/recovery files, `NotificationManager`, `Application`, `SceneDocument`, asset registry, project settings, `EditorDockspace`, `EditorLayer`.

Recommended tooling:

```powershell
.\scripts\editor_plan_audit.ps1 -JsonOut out\editor_tools\step18_plan_audit.json
.\scripts\migration_backup_test.ps1 `
  -Scene scenes\validation\cornell.rtlevel `
  -JsonOut out\editor_tools\step18_migration_backup.json
.\scripts\editor_smoke.ps1 `
  -BuildDebug `
  -BuildRelease `
  -OutDir out\editor_smoke_step18
```

Validation gate: Log captures workflow failures; Console routes through command registry; transform keyframes save/reload; autosaves and recovery work; required diagnostic smoke outputs still work.

## Step Dependencies And Gates

- Step 1 must precede real import so the current replacement path is labeled honestly.
- Step 2 must precede project close, scene replacement, async merge, and exit workflows.
- Step 6 must precede registry/import work because Content, Cache, Saved, Config, and AssetRegistry roots are project-relative.
- Step 7 must precede GUID-backed scenes and prefabs because persistence needs stable asset identity.
- Step 9 must precede Step 10 and Step 11 because Import Asset must first prove it does not mutate the active scene.
- Step 12 must precede serious reimport work because placed instances need GUID-backed references.
- Step 14 should precede large menu, toolbar, console, and shortcut expansion.
- Step 18 should wait until project settings, registry dirty state, scene dirty state, and logging are stable.

For every step that touches renderer startup, CLI parsing, RenderGraph, profiler, debug views, validation scenes, RenderDoc capture, diagnostic output, scene loading, or renderer synchronization, re-read `docs/AI_DEBUG_PROFILING_TOOLS_USAGE.md`, build, and run the required Cornell diagnostic smoke when shared renderer/request behavior changed.

For every step that changes editor UI labels, menus, dock layout, panel visibility, or action routing, run the static editor audits and keep their JSON output:

```powershell
.\scripts\imgui_panel_audit.ps1 -JsonOut out\editor_tools\imgui_audit.json
.\scripts\editor_request_flow_report.ps1 -JsonOut out\editor_tools\request_flow.json
.\scripts\editor_state_snapshot.ps1 `
  -Scene scenes\validation\cornell.rtlevel `
  -JsonOut out\editor_tools\state_snapshot.json
```

For every step that changes project files, registry files, import state, prefab state, migration, or scene persistence, run the applicable targeted validators:

```powershell
.\scripts\editor_plan_audit.ps1 -JsonOut out\editor_tools\plan_audit.json
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
.\scripts\scene_entity_count_probe.ps1 `
  -Before scenes\validation\cornell.rtlevel `
  -JsonOut out\editor_tools\entity_count.json
.\scripts\import_regression_harness.ps1 -JsonOut out\editor_tools\import_harness.json
.\scripts\migration_backup_test.ps1 `
  -Scene scenes\validation\cornell.rtlevel `
  -JsonOut out\editor_tools\migration_backup.json
```

Use failure-enforcing switches only when the feature is expected to be implemented for the current step. Before that point, preserve the JSON output and track which expected gaps remain.

## Legacy Milestone Summary Superseded By Step-By-Step Plan

1. Rename current glTF replacement behavior to `Import Scene as New Scene`.
2. Add dirty scene prompt.
3. Add scene tab with dirty marker.
4. Add Project Manager MVP: `.rtproject`, project folders, recent projects, project context, and project settings.
5. Add asset registry skeleton inside the project.
6. Add `Import Asset` skeleton that writes to project `Content/` and does not modify the scene.
7. UI shell near-clone: theme, dock layout, menus, panel renames.
8. Content browser uses project `Content/`.
9. glTF/GLB import-as-asset and generated prefab.
10. Import-and-place and prefab drag/drop.
11. Merge scene into current scene.
12. GUID-backed `.rtlevel` save/load.
13. Undo/redo expansion.
14. Lighting/post-process components.
15. Autosave/recovery.

This order keeps the editor foundation stable: Project Manager -> Content root -> Asset Registry -> Import Asset -> Persistent Scene.

## Legacy Detailed Milestone Breakdown

Each milestone must land as a working vertical slice with UI, data model, request routing, persistence behavior, and validation. Avoid partial systems that only work through one UI entry point.

### 1. glTF Scene Replacement Rename

Deliverables:

- Rename visible UI actions from generic `Open glTF` / `Load glTF` to `Import Scene as New Scene` wherever the current whole-scene replacement path is used.
- Keep the current request path internally compatible while adding a new explicit request name for future cleanup.
- Update status/log text so users understand the current scene will be replaced.
- Add dirty-scene prompt before replacement starts.

Validation:

- Loading a glTF still replaces the current scene exactly as before.
- No asset registry records are created by this path unless explicitly added later.
- Existing recent/favorite file behavior still works.

### 2. Dirty Scene Prompt And Scene Tab

Deliverables:

- Add a single scene tab above or inside the viewport area.
- Display scene name and dirty marker.
- Add Save / Do Not Save / Cancel prompt for New Scene, Open Scene, Import Scene as New Scene, Exit, and closing the scene tab.
- Keep V1 to one open scene at a time.

Validation:

- Cancel leaves scene, selection, undo stack, and renderer untouched.
- Save writes the current scene before continuing.
- Do Not Save continues without writing.

### 3. Project Manager MVP

Deliverables:

- Project Manager startup window.
- New Project and Open Project flows.
- `.rtproject` JSON read/write.
- Project folder creation and validation.
- Recent Projects list and missing-project badges.
- `ProjectContext` lifetime in the editor/application layer.
- Project Settings panel for project paths, startup scene, autosave, and import defaults.

Validation:

- Opening a project makes Content, Scenes, Cache, Saved, Config, Build, and AssetRegistry roots available to all editor panels.
- Closing a project clears project-scoped UI state and returns to Project Manager or no-project compatibility state.

### 4. Asset Registry Skeleton

Deliverables:

- JSON asset registry at `Project/Content/AssetRegistry.json`.
- `AssetGuid`, `AssetRecord`, `AssetType`, dependency lists, reverse-reference lists, source/import hashes, and dirty state.
- Load/save registry on project open/close.
- Content Browser can display registry-backed assets before thumbnails exist.

Validation:

- Creating a project creates or initializes the registry.
- Registry dirty state is set when records are added, renamed, deleted, moved, imported, or reimported.
- Registry survives editor restart.

### 5. Import Asset Skeleton

Deliverables:

- Import Settings dialog with import mode, destination, hierarchy/material/texture/camera/light options, tangents, BLAS cache, unit scale, and coordinate conversion.
- `Import Asset` creates registry records and files under `Project/Content/` without modifying the active scene.
- Import Report window.
- Staged registry changes applied on the main thread.

Validation:

- With Scene A open, importing model B does not change Scene A entity count.
- Imported asset appears in Content Browser.
- Cancelling import leaves registry and scene unchanged.

### 6. Near-Clone UI Shell

Deliverables:

- Dark dense theme.
- Dock layout matching the screenshots.
- Menu names and panel names aligned with the target editor.
- `Scene`, `Hierarchy`, `Inspector`, `Content`, `Timeline`, `Log`, `Render World Settings`, and technical panels wired to visibility flags.

Validation:

- Reset layout restores the target layout.
- Existing renderer/debug/profiler panels remain reachable.
- Viewport render target sizing still follows panel size.

### 7. Content Browser Project Integration

Deliverables:

- Content Browser root is project `Content/`.
- External files enter through Import Asset, not arbitrary browsing as project content.
- Breadcrumbs, folder tree, list/grid view, details panel, refresh, search, and registry metadata display.
- Find References, Show Dependencies, Find in Content, and Show Source Asset actions.

Validation:

- Content Browser shows project-relative paths.
- Moving the project folder keeps project-relative assets resolvable.
- Missing/stale assets show badges.

### 8. glTF/GLB Import-As-Asset And Prefab Generation

Deliverables:

- glTF/GLB import creates texture, material, mesh, and prefab records.
- Preserve node hierarchy in `PrefabAsset`.
- Import glTF lights and cameras using the mapping rules.
- Apply texture color-space and metallic-roughness channel rules.
- Produce Import Report warnings for unsupported extensions and fallbacks.

Validation:

- Embedded and external textures import.
- Multiple materials, alpha mask, double-sided, texture transform, lights, and cameras import or warn deterministically.
- Reimport preserves GUIDs where possible.

### 9. Import And Place / Prefab Drag-Drop

Deliverables:

- Drag prefab from Content to viewport creates a `PrefabInstance` and scene entities.
- Import and Place imports assets, generates prefab, and places one instance.
- Placed instances store prefab GUID and overrides.
- Material drag/drop assigns material references without mutating the source material asset.

Validation:

- Import and Place increases entity count only by the placed prefab hierarchy.
- Duplicate placements share source assets.
- Per-instance overrides survive save/reload.

### 10. Merge Scene Into Current

Deliverables:

- Async load staged scene data.
- Append entities under a new import root.
- Remap entity IDs and stable UUIDs.
- Preserve current scene entities, selection rules, and undo safety.

Validation:

- Existing entities remain untouched.
- Renderer updates only after main-thread apply.
- Merge failure leaves current scene unchanged.

### 11. GUID-Backed `.rtlevel` Save/Load

Deliverables:

- Scene file header with version and scene GUID.
- GUID-backed asset references.
- Migration path for legacy source-path/index scenes.
- Backup before migration save.
- Save/load for components, prefab instances, overrides, world settings, active bindings, and scene dirty reasons.

Validation:

- Existing validation `.rtlevel` files load.
- Saving migrated scenes writes latest format.
- Missing assets produce placeholders and warnings, not crashes.

### 12. Undo/Redo Expansion

Deliverables:

- Command transactions for create, delete, duplicate, reparent, transform, component add/remove, property edit, material assignment, asset placement, prefab placement, and merge.
- Gizmo drag commits one undo command.
- Undo labels surface in UI/Log.

Validation:

- Undo/redo restores scene state and renderer-visible state.
- Cancelled operations do not enter the undo stack.

### 13. Lighting/Post-Process Components

Deliverables:

- First-class lighting/post-process components in scene data.
- Create menu entries for Sun, Point, Spot, Area, Environment, Sky Atmosphere, Height Fog, and Post Process Volume.
- Inspector sections and renderer dirty routing.
- Save/load and undo/redo integration.

Validation:

- Artistic lighting controls do not live only in `Render Settings`.
- Renderer update granularity follows the rebuild table.

### 14. Autosave, Recovery, And Polish

Deliverables:

- Autosave scheduling using project settings.
- Recovery prompt on startup.
- Backups before migrations/destructive saves.
- Layout versioning, UI scale, theme, workspace presets, and final density polish.

Validation:

- Autosaves appear under `Project/Saved/Autosaves/`.
- Recovery can restore a scene after interrupted save/crash.

## Implementation Ownership Map

- `Application` owns request application, async load apply, project open/close, renderer synchronization, dirty prompts, and headless/editor compatibility.
- `EditorLayer` owns panel orchestration, shared selection state, command dispatch, and visible editor requests.
- `EditorDockspace` owns top menus, dock defaults, workspace/layout reset, and Project Manager visibility entry points.
- `SceneDocument` owns persistent scene data, world settings, components, scene GUID, dirty reasons, and `.rtlevel` serialization/migration.
- `SceneOperations` owns undoable scene mutations.
- `AssetManager` continues to own runtime mesh/material/texture data.
- The new asset registry owns project asset identity, GUIDs, dependencies, import settings, and import metadata.
- Content Browser owns project-rooted asset presentation and operations; it must not directly mutate the renderer.
- `AsyncSceneLoader` owns worker-thread CPU loading and returns staged results only.
- `PathTracerRenderer` remains renderer-owned; editor systems reach it through existing safe request/apply paths.

## Data Flow Details

### Project Open

1. User selects `.rtproject` from Project Manager or recent list.
2. Application reads project JSON and validates/migrates it.
3. Application builds `ProjectContext` and creates missing folders if approved.
4. Asset registry loads from project Content root.
5. Editor config/layout/keybindings load from project Config root.
6. Startup scene load is queued through `AsyncSceneLoader` when available.

### Import Asset

1. User chooses Import Asset from File menu or Content Browser.
2. Import Settings dialog captures mode, destination, geometry/material/texture/camera/light options, unit scale, and conversion settings.
3. CPU import runs synchronously for V1 skeleton or as a future background import job.
4. Import produces staged asset files, registry changes, and Import Report.
5. Main thread applies registry changes and marks registry dirty.
6. Content Browser refreshes and shows new asset records.
7. Active scene remains unchanged unless mode is Import and Place.

### Scene Save

1. UI emits Save Scene request.
2. Application serializes `SceneDocument` with scene header/version and project-relative asset GUID references.
3. Camera bookmarks, world settings, prefab instances, overrides, and component data are serialized.
4. Scene dirty state clears only after successful write.
5. Save failure leaves scene dirty and logs an error.

### Renderer Update

1. UI edits scene data through `SceneOperations` or editor requests.
2. Scene marks dirty reason and update kind.
3. Application routes changes to scene/GPU builders at safe frame boundary.
4. Renderer updates only required resources according to rebuild granularity.
5. Accumulation resets only when visible output or sampling state requires it.

## Definition Of Done Gates

- UI gate: target panels, menus, and dock layout are visible and resettable.
- Project gate: project create/open/close, recent projects, settings, folders, and registry path work.
- Import gate: Import Asset never mutates active scene; Import and Place mutates only by placement.
- Persistence gate: save/reopen preserves GUID asset references, prefab instances, overrides, world settings, and components.
- Async gate: worker threads never touch active scene, ImGui, Vulkan, renderer, or undo stack.
- Renderer gate: no new validation errors and no new unconditional `vkDeviceWaitIdle` outside allowed teardown.
- Compatibility gate: legacy `.rtlevel` and current glTF replacement path continue working.
- Diagnostics gate: required headless/profile/rendergraph/debug-view smoke outputs still work after shared request/scene changes.

## Public Interface Changes

### Editor Panel Visibility

Add panel visibility fields:

```cpp
bool renderWorldSettings = true;
bool timeline = true;
bool log = true;
bool console = false;
```

### Editor Requests

Add explicit request fields:

```cpp
std::optional<std::filesystem::path> openScene;
std::optional<std::filesystem::path> saveScene;
std::optional<std::filesystem::path> saveSceneAs;
std::optional<std::filesystem::path> importAsset;
std::optional<std::filesystem::path> importAndPlace;
std::optional<std::filesystem::path> importSceneAsNewScene;
std::optional<std::filesystem::path> mergeScene;
std::optional<AssetGuid> reimportAsset;
std::optional<AssetGuid> placeAsset;
std::optional<CreateProjectRequest> createProject;
std::optional<OpenProjectRequest> openProject;
bool closeProject = false;
bool saveProjectSettings = false;
```

Existing `loadGltf` may remain temporarily as an internal compatibility field, but visible UI must stop presenting it as generic import.

### Project Context

Add project-facing editor types:

```cpp
struct ProjectContext;
struct CreateProjectRequest;
struct OpenProjectRequest;
struct AssetRegistryState;
enum class AssetRegistryDirtyReason;
```

Project context owns project-relative roots for Content, Scenes, Cache, Saved, Config, Build, and AssetRegistry.

### Async Scene Loading

Add async scene loading types:

```cpp
enum class SceneLoadMode;
enum class SceneLoadStatus;
struct SceneLoadRequest;
struct SceneLoadResult;
class AsyncSceneLoader;
```

Scene load workers produce staged CPU-side results only; `Application` applies completed results on the main thread at a safe frame boundary.

### Asset Database

Add core data types:

```cpp
struct AssetGuid;
enum class AssetType;
struct AssetRecord;
struct AssetImportSettings;
struct AssetDependency;
struct ImportReport;
struct PrefabAsset;
struct PrefabInstance;
struct PrefabOverride;
struct RtLevelHeader;
enum class SceneDirtyReason;
```

The first registry format should be JSON and project-local.

`AssetRecord` must include source/import hashes and import settings hash so duplicate imports and stale assets can be detected deterministically.

### Editor Commands And Keybindings

Add command and keybinding types:

```cpp
struct EditorCommandId;
struct EditorCommand;
struct EditorKeybinding;
enum class EditorCommandContext;
```

Menus, toolbar buttons, shortcuts, and console commands must dispatch through the command registry.

### Lighting And Post-Process Components

Add first-class scene component/data types for artist-facing lighting and post-processing:

```cpp
struct SunLightComponent;
struct PointLightComponent;
struct SpotLightComponent;
struct AreaLightComponent;
struct EnvironmentLightComponent;
struct EmissiveMeshLightComponent;
struct SkyAtmosphereComponent;
struct HeightFogComponent;
struct VolumetricCloudComponent;
struct PostProcessVolumeComponent;
struct CameraPostProcessComponent;
struct GlobalPostProcessSettings;
struct WorldSettings;
template <typename T> struct PostProcessOverride;
enum class LightIntensityUnit;
struct ModelImportSettings;
```

These components must participate in hierarchy display, inspector editing, scene save/load, undo/redo, dirty reasons, and renderer synchronization.

### Selection State

Formalize selection state around primary entity, selected entity set, hovered entity, active editable entity, selection source, and selection changed event.

Reference-driven selection requirements:

- `Hierarchy`, viewport gizmos, and `Inspector` must normally agree on the active selected entity.
- If a row is only hovered or focused for keyboard navigation, the UI must make that visually distinct from the selected row.
- `Inspector` edits the active editable entity, not the last hovered row.
- Imported scene roots, repeated imported entity names, and glTF-generated roots must still produce stable selection IDs and readable labels.
- Selection diagnostics should catch mismatches where the hierarchy highlight and Inspector entity name diverge unexpectedly.

## Acceptance Criteria

### Visual/UX

- Editor opens directly into the near-clone layout.
- Menus match `File | Create | Engine | Window | Render | Layout`.
- Active scene name and dirty marker are visible in the top bar.
- FPS and frame-time readout remain visible at the top-right.
- The viewport panel is named `Scene` and has compact toolbar/status UI.
- Viewport status preserves compact path tracing progress plus `View Settings`, `Stats`, and `Draw Debug` actions.
- Right and bottom panels match the reference structure.
- `Content` uses a three-pane folder tree, asset/file list, and details/preview layout with breadcrumb navigation.
- Existing debug/profiler panels remain accessible.

### Editing

- Entity create, rename, duplicate, delete, lock, hide, reparent, and focus still work.
- Viewport picking still selects entities.
- Transform gizmo still edits selected entities.
- Inspector edits transform, light, sun, camera, mesh renderer, and material values.
- Hierarchy selected row, viewport selection/gizmo, and Inspector active entity stay synchronized unless the UI explicitly shows hover or active-edit state separately.
- Undo/redo works for implemented scene operations.

### Import/Persistence

- `Import Scene as New Scene` preserves current glTF replacement behavior.
- `Import Asset` does not replace the current scene.
- Imported model assets appear in Content.
- Generated prefab can be placed into the current scene.
- Save/reopen preserves placed asset references.
- Existing `.rtlevel` scenes still load.

### Import Fix

- With Scene A open, Import Asset of model B must not change entity count in Scene A.
- Import and Place of model B must increase entity count only by the placed prefab hierarchy.
- Import Scene as New Scene must prompt to save dirty Scene A before replacing it.
- Merge Scene must preserve all existing Scene A entities and append Scene B under a new root.
- Reimport model B must preserve GUIDs and update all placed instances that reference it.
- Reimport must preserve local prefab instance overrides whenever matching source nodes/components still exist.

### Project Manager

- Editor can create a new project folder.
- New project contains `.rtproject`, `Content`, `Scenes`, `Cache`, `Saved`, `Config`, and `Build` folders.
- Editor can open an existing `.rtproject`.
- Recent Projects list works and marks missing projects.
- Project Settings shows project paths and startup scene.
- Import Asset defaults to the project Content folder.
- Save Scene defaults to the project Scenes folder.
- Asset registry loads/saves inside the project.
- Autosaves/logs write into the project Saved folder.
- Closing project prompts to save dirty scene and dirty asset registry.
- Existing standalone `.rtlevel` loading still works without a project.

### Lighting/Post-Processing

- Create > Light can create Sun, Point, Spot, and Area lights.
- Created lights appear in the Hierarchy and Inspector.
- Light transform/gizmo affects renderer lighting.
- Editing light color, intensity, radius, cone, area shape, or shadow state updates the renderer safely and resets accumulation when needed.
- Sun Light Inspector exposes sun-specific physical controls: lux units, azimuth/elevation, color temperature, intensity, softness, and shadow options.
- Area Light Inspector exposes area-specific controls: shape/type, lumen units, IES/material source when supported, color controls, radius/cone controls, visible-to-camera, and shadow options.
- Camera Inspector exposes physical camera controls and grouped post-process controls for DOF, Bloom, Color correction, Vignetting, and Film grain.
- Create > Environment can create Environment Light, Sky Atmosphere, Height Fog, and Post Process Volume entities.
- Environment, atmosphere, fog, cloud, and post-process components save and reload in `.rtlevel`.
- World Settings can assign primary sun, active environment, active atmosphere, and default post-process.
- Render Settings remains technical and does not become the only place for artistic lighting controls.

### Async Scene Loading

- Opening a large `.rtlevel` does not freeze the editor UI.
- Import Scene as New Scene shows progress and can be cancelled.
- Merge Scene into Current shows progress and does not replace existing entities.
- Dirty scene prompt appears before replacement loading starts.
- Worker thread never mutates active `SceneDocument`.
- Worker thread never creates Vulkan resources.
- Completed scene result is applied only on the main/editor thread at a safe frame boundary.
- Cancelled scene load leaves the current scene unchanged.
- Failed scene load leaves the current scene unchanged and writes an error to Log.
- Renderer rebuild happens only after main-thread apply.
- Existing headless loading path still works.

### Renderer Safety

- No new Vulkan validation errors.
- No regression to headless diagnostic tools.
- No renderer debug/profiling outputs are removed or renamed incompatibly.

## Test Plan

### Build

Build Debug:

```powershell
cmake --build build --config Debug
```

Build Release:

```powershell
cmake --build build --config Release
```

### Required Smoke Test

Run after renderer/shared request changes:

```powershell
rtvulkan.exe --headless --scene scenes/validation/cornell.rtlevel ^
  --warmup-frames 30 --frames 120 --fixed-seed 1 ^
  --profile --profile-json out/profile.json ^
  --dump-rendergraph out/rendergraph.json ^
  --save-debug-views out/debug_views ^
  --make-debug-package out/debug_package
```

Expected outputs:

- `out/profile.json`
- `out/rendergraph.json`
- `out/debug_views/`
- `out/debug_package/`

### Editor Tooling Smoke

Run after shared editor request handling, scene loading, renderer synchronization, or diagnostic output path changes:

```powershell
.\scripts\editor_smoke.ps1 `
  -BuildDebug `
  -BuildRelease `
  -OutDir out\editor_smoke
```

If build time is too high for the current iteration, run the static-only tooling pass and record that the full smoke was deferred:

```powershell
.\scripts\imgui_panel_audit.ps1 -JsonOut out\editor_tools\imgui_audit.json
.\scripts\editor_request_flow_report.ps1 -JsonOut out\editor_tools\request_flow.json
.\scripts\rtlevel_schema_validator.ps1 `
  -Path scenes\validation\cornell.rtlevel `
  -JsonOut out\editor_tools\rtlevel_schema_cornell.json
```

### Static Editor Audits

Run after UI shell, panel label, menu, toolbar, shortcut, command registry, or request-routing changes:

```powershell
.\scripts\imgui_panel_audit.ps1 -JsonOut out\editor_tools\imgui_audit.json
.\scripts\editor_request_flow_report.ps1 -JsonOut out\editor_tools\request_flow.json
.\scripts\editor_state_snapshot.ps1 `
  -Scene scenes\validation\cornell.rtlevel `
  -JsonOut out\editor_tools\state_snapshot.json
```

Expected checks:

- `imgui_panel_audit.ps1` shows the intended panel/menu names and no stale labels for completed rename milestones.
- `editor_request_flow_report.ps1` shows each completed request has both UI and `Application` usage.
- `editor_state_snapshot.ps1` captures the expected source-level capabilities for the current milestone.

### Reference Screenshot Match Checks

Run these manual checks after UI shell, viewport, hierarchy, inspector, Content browser, lighting, or camera/post-process work. Use `docs/EDITOR_REFERENCE_SCREENSHOT_INSPECTION.md` as the detailed reference.

- Confirm the default dock layout matches the references: center `Scene`, right `Hierarchy`/`Render World Settings`, lower-right `Inspector`, bottom `Content`/`Timeline`/`Log`.
- Confirm menu order is the canonical `File | Create | Engine | Window | Render | Layout`.
- Confirm active scene title and dirty marker are visible in the top bar.
- Confirm FPS and frame-time readout are visible at the top-right.
- Confirm viewport status includes compact path tracing progress, `View Settings`, `Stats`, and `Draw Debug`.
- Confirm transform and light gizmos remain visible over rendered content.
- Confirm hierarchy rows use dense icons, disclosure arrows, search, blue selection, and visibility toggles.
- Confirm hierarchy selection, viewport gizmo, and Inspector entity name stay synchronized.
- Confirm Content uses folder tree, file/asset list, details/preview pane, filter, navigation buttons, refresh, and breadcrumb.
- Confirm unsupported or empty Content selection reports a clear details-pane message.
- Confirm `Sun Light` Inspector uses sun-specific controls rather than the generic light layout.
- Confirm area light Inspector uses area-specific controls rather than the generic light layout.
- Confirm camera Inspector includes physical camera, exposure, and post-process groups.

### Scene, Project, And Registry Validation

Run after `.rtlevel`, `.rtproject`, project fixture, asset registry, scene save/load, or migration changes:

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

Expected checks:

- Validation `.rtlevel` files remain valid and compatible.
- Generated projects have the expected `Content`, `Scenes`, `Cache`, `Saved`, `Config`, and `Build` layout.
- Asset registry files keep unique GUIDs, project-relative normalized paths, and valid dependency references.

### Import, Merge, And Migration Probes

Run after import mode, asset placement, prefab placement, merge, scene migration, or backup changes:

```powershell
.\scripts\scene_entity_count_probe.ps1 `
  -Before scenes\validation\cornell.rtlevel `
  -JsonOut out\editor_tools\entity_count.json

.\scripts\import_regression_harness.ps1 -JsonOut out\editor_tools\import_harness.json

.\scripts\migration_backup_test.ps1 `
  -Scene scenes\validation\cornell.rtlevel `
  -JsonOut out\editor_tools\migration_backup.json
```

Expected checks:

- `Import Asset` does not mutate the active scene entity count.
- `Import and Place` adds only the placed prefab hierarchy.
- `Merge Scene` appends without removing existing entities.
- Migration backups preserve valid JSON and matching entity counts.

### Manual Editor Tests

- Launch editor with a lightweight validation scene.
- Verify default dock layout and visual style.
- Toggle all panels from `Window`.
- Reset and save layout.
- Select from hierarchy and viewport.
- Create empty entity, camera, and light.
- Rename, duplicate, delete, lock, hide, and reparent entities.
- Edit transform with inspector and viewport gizmo.
- Edit light, sun, camera, and material values.
- Save `.rtlevel`, close, reopen, and verify state.
- Load current glTF path as `Import Scene as New Scene`.
- Import glTF as asset and confirm current scene is not replaced.
- Drag generated prefab into the viewport.
- Merge an external scene into the current scene.
- Assign material by drag/drop.
- Apply HDRI from Content to world settings.
- Check undo/redo for create, delete, transform, material assignment, and prefab placement.

### Project Manager Tests

- Create a new project.
- Close editor and reopen project from Recent Projects.
- Import glTF as asset and verify files appear under `Content/`.
- Save scene and verify it appears under `Scenes/`.
- Restart editor and verify asset registry reloads.
- Move/delete a project folder and verify Recent Projects shows missing status.
- Open old `.rtlevel` without a project and verify compatibility.

### Async Scene Loading Tests

- Open a large `.rtlevel` and verify the editor UI remains responsive.
- Import Scene as New Scene and cancel during loading; verify the current scene is unchanged.
- Merge a large glTF scene and verify existing entities remain after apply.
- Fail a scene load intentionally and verify the error appears in Log.
- Verify worker-side load does not create Vulkan resources.
- Verify renderer rebuild happens only after main-thread apply.
- Open a project with a startup scene and verify startup scene loading can run through `AsyncSceneLoader`.

### Import Regression Tests

- Import GLB with embedded textures.
- Import glTF with external textures.
- Import glTF with multiple materials.
- Import glTF with node hierarchy.
- Import glTF with `KHR_lights_punctual`.
- Import glTF with alpha mask.
- Import glTF with double-sided material.
- Import same asset twice and verify deduplication/reuse policy.
- Reimport changed texture and verify scene instances update.
- Import Asset and verify the current scene is not replaced.
- Import and Place and verify only one prefab hierarchy is added.
- Merge Scene and verify existing scene entities remain.
- Verify baseColor/emissive textures import as sRGB.
- Verify normal/roughness/metallic/occlusion textures import as linear.
- Verify glTF metallic-roughness G/B channel mapping.

### Component Tests

- Add `PointLightComponent` to an empty entity.
- Remove `PointLightComponent` and verify renderer light count updates.
- Disable/enable light and verify output changes.
- Add `PostProcessVolumeComponent` and verify Inspector shows override groups.
- Remove `MeshRenderer` and verify the entity remains but is no longer rendered.
- Add Sun, Point, Spot, and Area lights from Create > Light and verify each appears in Hierarchy and Inspector.
- Create Environment Light, Sky Atmosphere, Height Fog, and Post Process Volume from Create > Environment.

### Post Process Volume Tests

- Unbound post-process volume affects the editor camera.
- Higher priority volume overrides lower priority volume.
- Blend radius smoothly interpolates values.
- Disabled volume has no effect.
- Camera component post-process overrides global defaults.
- Scene save/reload preserves volume priority, blend radius, blend weight, and override flags.

### Regression Scenes

- `scenes/validation/cornell.rtlevel`
- `scenes/validation/closeup_cornell.rtlevel`
- `Sponza/glTF/Sponza.gltf`
- Heavy Sponza only for performance/stress validation after functionality is stable.

## Risks And Mitigations

- Risk: UI rename breaks saved docking layouts.
  - Mitigation: force/reset default layout on version change and keep reset layout easy to access.
- Risk: glTF import-as-asset duplicates materials/textures repeatedly.
  - Mitigation: asset registry should deduplicate by GUID/source path/import settings.
- Risk: old `.rtlevel` files reference mesh/material indices.
  - Mitigation: keep legacy load path and migrate to GUID references on save.
- Risk: import pipeline rebuilds too much renderer state.
  - Mitigation: use existing scene update kinds and resource rebuild paths first, then optimize after profiling.
- Risk: toolbar shortcuts conflict with existing renderer shortcuts.
  - Mitigation: preserve current behavior until a centralized keybinding registry resolves precedence.

## Assumptions

- Near-clone means matching layout, density, labels, panel behavior, and workflow as closely as ImGui allows, not copying proprietary art assets.
- The first UI implementation may use text/icon-like buttons until an icon font is available.
- Asset database persistence starts as JSON.
- glTF/GLB asset import comes before OBJ/FBX/USD support.
- Full advanced project workflow, play mode, cook/build/package, node material editor, and advanced sequencer come after persistent level editing and asset import are stable. Project Manager MVP is part of the editor foundation.
