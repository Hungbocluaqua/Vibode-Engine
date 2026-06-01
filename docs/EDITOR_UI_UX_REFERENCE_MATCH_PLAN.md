# Editor UI/UX Reference Match Plan

## Purpose

This document defines the step-by-step work needed to make the current editor UI/UX match the provided reference editor screenshots as closely as possible.

The target is not only visual similarity. The target is the same interaction model:

- a scene-first viewport
- an object-centric hierarchy
- a component-driven inspector
- a clean content browser
- a docked render settings workflow
- compact icon-first editor controls
- consistent dark editor styling

This plan intentionally does not implement code. It is a detailed execution plan for future implementation.

## Implementation Status

Status markers are maintained as the implementation lands:

- `[done]` completed in code and validated by the relevant audit/build where available.
- `[partial]` meaningful implementation exists, but the phase still has known gaps against the reference target.
- `[todo]` not yet implemented beyond older baseline behavior.

Current status:

| Area | Status | Evidence / remaining work |
|---|---|---|
| Phase 0: Project Manager Startup | [done] | `Vibode Engine` naming/window title, `%USERPROFILE%/Documents/Vibode Projects` default, `.vproject` creation/opening, duplicate/name/read-only/location validation, left-rail Project Manager navigation, load-on-startup selector, recent project cards with preview blocks, saved `Thumbnail.png`/`ProjectThumbnail.png` raster mosaic loading, generated `Saved/Thumbnail.png` capture from the renderer after project startup scene load, thumbnail-availability badges and drawn project glyph fallbacks, packaged sample `.vproject` manifests for Cornell Validation, Lightweight Sponza, and Cinematic Lighting, a dedicated `scenes/validation/lightweight_sponza.rtlevel` that references `Sponza/glTF/Sponza.gltf` and includes a mesh-renderer entity for imported mesh `0`, sample cards that prefer opening those projects with scene fallback actions and drawn scene/model glyphs, six-template grid with drawn template glyph preview cards, folder picker for new-project location, renderer-deferred Project Manager-only interactive startup when no explicit scene/project or open-last preference is active, explicit `--scene`/`--gltf` startup bypass of the Project Manager gate, deterministic `RTV_EDITOR_STARTUP_PROJECT` capture startup override with recovery-prompt suppression for screenshot runs, PM-only overlay rendering before `PathTracerRenderer` creation, fallback renderer creation on `Continue Without Project`, minimal Project Manager startup scene, and panel suppression while the Project Manager gate is active are implemented. `project_manager_fixture_audit.ps1 -FailOnIssues` verifies source readiness, raster thumbnail loading, deferred renderer startup, explicit scene bypass, deterministic capture startup override, Lightweight Sponza startup-scene wiring/renderable mesh entity, and generated thumbnail capture. |
| Final Target Layout | [done] | Default dock uses `Scene`, `Hierarchy`, unified `Render Settings`, `Inspector`, `Content`, `Timeline`, and `Log`; the deterministic default editor capture explicitly activates `Content`; `Debug / Profiler` is hidden by default; the active scene tab in the main menu bar now uses custom tab chrome with drawn scene and close glyphs; Windows builds request a dark native title frame to match the full-frame references; shared base theme colors, dock tab colors/rounding, default splitter ratios, stable `###` dock title IDs, foreground native dock-tab glyph overlays, shared splitter colors, and foreground dock-panel border/accent overlays live in `EditorUiStyle`/`EditorDockspace`; the multi-state harness captures Project Manager home, default editor, Content-active, Timeline-active, Render Settings-active, selected-camera, and selected-light screenshots, with all seven blessed under `refs/editor`; the deeper Lightweight Sponza reference-scene harness captures default, Content-active, Timeline-active, and selected-sun states with four blessed baselines under `refs/editor_reference_scenes`; both screenshot sets compare within threshold after the reference-style theme/chrome refresh. |
| Phase 1: Baseline UI Style Constants | [done] | Compact reference-style dark theme exists, and shared `EditorUiStyle` metrics/colors now cover base ImGui theme colors, common padding/rounding/sidebar/detail widths, Project Manager card, Content grid/preview, registry progress, hierarchy row height/icon size/indent/right-fade metrics, content/inspector row heights, inspector label width, Inspector component header/action sizing, viewport overlay padding/rounding/colors, dock split ratios, dock tab rounding/borders, dock panel border/accent sizing, dock splitter/panel border colors, scene-tab chrome metrics/colors, Timeline control/ruler/track/key-editor metrics/colors, and shared selected/hover/active row colors. `style_metrics_audit.ps1 -FailOnIssues` verifies the shared base theme, row, dock, scene-tab, Timeline, disabled, and viewport/Inspector style surfaces. |
| Phase 2: Remove Content Panel Clutter | [done] | Removed `Scene Import / Compatibility`, HDRI path, and scene document forms from `Content`; `content_browser_audit.ps1 -FailOnIssues` passes. |
| Phase 3: Redesign Content Browser | [done] | Three-pane browser, custom-drawn `+`/navigation/view toolbar glyphs, shared drawn file-type glyphs in grid/list/preview/tree surfaces, CPU-sampled raster thumbnails for image/HDR files and registry `thumbnailPath` previews, GPU-backed preview presentation for resident imported scene/material textures through an editor UI texture-provider bridge, standalone GPU upload/cache presentation for not-yet-resident raster image/HDR previews keyed by source path/stamp/size through the same editor texture-provider bridge, generated non-image source preview cards for folders, models, scenes, projects, materials, IES profiles, and VDB volumes with in-memory metadata plus persistent DDC JSON disk cache keyed by source path/stamp/size, standalone generated GPU preview textures for those non-image preview cards, icon+text details actions with hover descriptions, project/workspace mount shortcuts, project-root folder tree with Content/Scenes/Saved/DDC access, asset list, details pane preview/actions, folder/file context menus, copy-path, Explorer reveal, import/open routing, async scene import/open progress display, registry status display, visible import/reimport operation queue, per-asset registry progress bars, reimport requests, and Application-owned worker-thread import/reimport staging are implemented. Successful completed startup/import load messages no longer render as prominent progress banners in the Content browser, so completed project startup does not obscure the reference-style three-pane surface; active loads and failures still draw the visible status row. The empty details pane now stays quiet with `No supported files selected` instead of showing project/root metadata when nothing is selected, matching the reference Content screenshots. `content_browser_audit.ps1 -FailOnIssues` verifies icon actions, CPU raster thumbnails, GPU resident scene texture thumbnails, standalone GPU raster/HDR preview cache, generated non-image GPU preview cache with persistent metadata disk cache, visible operation queue, completed-load banner collapse, quiet empty details state, and async import worker coverage; `capture_editor_screenshot.ps1 -FocusWindow Content -ClickX 270 -ClickY 648 -FailOnIssues` verifies the runtime generated GPU folder preview path. |
| Phase 4: Combine Render World Settings And Render Settings | [done] | Default layout docks only `Render Settings`; `editor_ui_layout_dump.ps1` reports `noRenderWorldSettingsTab=true`. |
| Phase 5: Hierarchy Match | [done] | Every row has an icon; scene entity, imported glTF, and fallback renderer hierarchy rows use shared custom-drawn glyphs, search is `Search...`, the root creation strip uses shared icon+text button chrome, the compact type-filter toolbar is active rather than a passive legend and supports mesh/camera/light/world/atmosphere/effects filters with blue active tint, visibility/lock controls are right-aligned on entity rows with shared custom-drawn eye/lock glyph buttons, hierarchy rows use shared row height, selected/hover/active colors, explicit shared indentation guide rails, muted hidden/locked text/icon treatment, and a right-edge row fade behind lock/visibility controls; parent-match filtering keeps descendants visible, empty-space creation context menus exist, richer entity context menus exist, selected-camera/light and Lightweight Sponza captures exercise selected and deeper hierarchy states, and `hierarchy_icon_audit.ps1 -FailOnMissing` plus refreshed screenshot comparisons verify icon coverage, type-filter toolbar, create toolbar glyph chrome, context glyph rows, row chrome, and hierarchy-state thresholds. |
| Phase 6: Inspector Match | [done] | Camera physical/post-process controls, sun-specific controls, area-light reference fields, component action menus, entity action menu, transform/camera/light/sun row reset controls, and component coverage are implemented; inspector reset/action affordances use shared custom-drawn glyph buttons instead of raw `R`/ellipsis text buttons; shared two-column Inspector property-row helpers are used across visible Transform, Camera, Light, Sun, Mesh Renderer, Environment/Atmosphere/Fog/Cloud, Post Process Volume, and Camera Post Process controls; empty, invalid, and multi-selection states render compact shared-glyph state cards instead of raw disabled text; Transform/Camera/Light/Sun/Mesh/Material/world/post-process component sections use shared icon header chrome with integrated action popups instead of raw separator/remove-button rows; locked entities show a compact lock-state banner before disabled controls; Add Component actions use shared icon+text button chrome; selected-camera, selected-light, and Sponza selected-sun screenshots exercise the Inspector component-header and property-row chrome; authored-but-not-yet-renderer-owned light/sun/camera post-process fields such as IES profile, visibility/shadow toggles, bloom, vignetting, and film grain persist through scene serialization. `inspector_coverage_audit.ps1 -FailOnMissing` and refreshed screenshot comparisons verify component coverage, property-row helper usage, state-card chrome, component header/action chrome, locked-state banner, icon add-component controls, raw component remove-button removal, unsupported-field persistence, and per-widget alignment against blessed baselines. |
| Phase 7: Viewport Toolbar And Scene Controls | [done] | Existing `Scene` viewport toolbar/status strip, transform gizmo, grid/axes, selection overlay, and light gizmos remain. The transform toolbar uses shared custom-drawn icon buttons with command/shortcut tooltips, and the top-right `View Settings`, `Stats`, and `Draw Debug` viewport strip uses custom-drawn icon-plus-label buttons backed by real popovers with preview/debug-view settings, camera-speed readout/editing, frame/pass stats, and draw-layer toggles; both viewport overlay strips draw shared translucent backdrop/border chrome so controls and status readouts remain legible over bright render content. Selected-camera, selected-light, and Sponza selected-sun state captures verify selected viewport states and overlay chrome against blessed thresholds, and broader icon/chrome coverage is enforced through the style/icon audits. |
| Phase 8: Timeline Match | [done] | Compact custom-drawn transport controls, persistent sequence FPS/range metadata, range duration readout, range start/end jumps, fit-range-to-keys action, direct Render Sequence action, ruler/scrubber with labeled ticks, current-frame playhead, explicit range text, shared Timeline ruler/lane/key/playhead chrome, key markers, transform track rows, lane playhead, track context menu, drawn key/save/clear/delete action buttons, key deletion, per-key frame/transform editing, stable key IDs, Ctrl/Shift multi-select, selected-key deletion, lane drag-to-frame key editing, and sequence render output modal/status wiring are implemented. `timeline_interaction_audit.ps1 -FailOnIssues` verifies stable key identity, multi-select controls, selected deletion, drag editing, and sequence workflow controls; `style_metrics_audit.ps1 -FailOnIssues` verifies shared Timeline chrome metrics/colors and usage; `render_workflow_audit.ps1 -FailOnIssues` verifies render job state, manifest output, modal progress, sequence frame readout, Stop Render, and Open Output actions; refreshed `timeline_active.png` and `sponza_timeline_active.png` baselines compare within threshold. |
| Phase 8.5: Notifications, Commands, And Layout Persistence | [done] | Notifications exist with action buttons, notification actions can open Log/Content/Render Settings/Project Manager/output folder, command registry exists, command palette opens from the effective shortcut, shortcut conflicts are surfaced in the palette, editable shortcut overrides persist in editor preferences and feed menu/palette/viewport/global runtime shortcut checks, project-specific layout files are saved under `Saved/Editor/layout.ini`, and render workflow commands now exist. |
| Phase 9: Top Menus Match | [done] | Required top menus and command registry entries exist; render workflow command audit passes and render commands write project render history, per-job render manifests, output-folder notifications, and a visible Render Output modal. Menus include search fields, section labels, File/Create/Engine/Window/Render/Layout reference groups, drawn command/toggle glyph rows, visible disabled future items, disabled-state row chrome, and disabled-state tooltips with context-specific reasons for pending features. `command_menu_audit.ps1 -FailOnIssues` verifies required menus/sections/rows, drawn glyph rows, scene-tab drawn close chrome, disabled future-row reasons, and absence of legacy bracket icon fallbacks. |
| Phase 10: Right-Side Dock Details | [done] | Right dock structure matches the high-level target, the active scene tab uses drawn close/tab chrome, and native dock tab colors/rounding plus right/bottom split ratios are centralized and tuned through `EditorUiStyle`; native dock tabs reserve stable-title padding and draw shared glyph overlays for Scene, Hierarchy, Render Settings, Inspector, Material Editor, Content, Timeline, and Log without changing docking IDs; dock splitter colors use shared style helpers and known docked panels receive foreground border/accent overlays. Default, selected-camera, selected-light, Content-active, Timeline-active, Render Settings-active, and Sponza selected-sun captures exercise the right dock split, bottom dock tabs, Inspector, and Render Settings states, and refreshed baseline comparisons pass. |
| Phase 11: Icon System | [done] | Shared `EditorUiStyle` glyph helpers drive hierarchy rows, imported nodes, fallback hierarchy rows, hierarchy root creation controls, hierarchy visibility/lock controls, Content file rows/previews, Project Manager cards, top-menu command/toggle rows, native dock-tab overlays, Inspector component headers/add-component controls, Timeline transport/key/action buttons, and viewport toolbar/strip controls with hover descriptions plus shared overlay backdrops. A custom ImGui draw-icon source exists for core viewport controls, top-right viewport actions, camera-speed readout, all hierarchy row/control icon paths, native dock tabs, Content file-type glyphs, Content toolbar glyphs, Project Manager recent/template/sample card glyphs, Inspector reset/action/header/add-component buttons, Timeline transport/key/action buttons, and top-menu command/toggle icons. Legacy bracket fallback constants and string icon helpers have been removed, and screenshot-level icon/chrome polish is covered by refreshed editor/reference-scene baselines plus command/content/hierarchy/inspector/style audits. |
| Phase 12: Visual Polish Pass | [done] | Shared base theme colors, selected/hover/active row colors, and row-height metrics now apply across the editor; hierarchy row indentation, icon sizing, hidden/locked muted treatment, and right control fade are centralized; shared Inspector property-row metrics/helpers cover visible Transform, Camera, Light, Sun, Mesh Renderer, world/environment, Post Process Volume, and Camera Post Process rows, plus shared-glyph empty/multi-selection state cards, icon component headers, integrated header action popups, a compact locked-state banner, and icon add-component buttons; viewport overlay strips have shared translucent backdrop/border chrome; dock tab colors/rounding, icon padding, stable title IDs, foreground native-tab glyph overlays, foreground dock-panel borders/active accents, shared splitter colors, and default split ratios are centralized; Timeline ruler/lane/key/playhead colors and control/key-editor metrics are centralized; Content details action buttons and Hierarchy/Content/Inspector/Viewport context menu rows use shared glyph chrome with disabled-state tinting; disabled menu and context rows now use shared disabled text/icon colors plus a subtle disabled row accent instead of raw ImGui defaults; completed-load status banners are hidden from the settled Content browser surface after successful project startup; the empty Content details pane avoids non-reference project/root metadata when nothing is selected; resident imported scene/material textures can render as GPU-backed Content previews, standalone image/HDR previews can upload to and reuse GPU preview textures, generated non-image cards can upload to and reuse GPU preview textures, saved Project Manager thumbnails render raster thumbnail mosaics instead of only glyph placeholders, and generated non-image preview metadata is reused through persistent DDC disk cache. `style_metrics_audit.ps1 -FailOnIssues` verifies shared base theme, row, Inspector header, viewport overlay, dock panel-border/splitter, dock tab chrome, native dock-tab glyph coverage, Timeline chrome, and shared disabled styling helpers/usage; `content_browser_audit.ps1`, `hierarchy_icon_audit.ps1`, `inspector_coverage_audit.ps1`, and `project_manager_fixture_audit.ps1` verify the corresponding visual polish surfaces. |
| Phase 13: Interaction Parity | [done] | Hierarchy/viewport/inspector selection, prefab drag-drop placement from Content/registry into the viewport or hierarchy, automatic selection of placed/created/duplicated root entities so the hierarchy and Inspector follow the changed entity, viewport popovers, a viewport right-click context menu for focus, duplicate, delete, reset transform, create-here, and prefab-drop guidance, Content browser folder/file context menus, icon action buttons, visible import/reimport operation queue, hierarchy creation/entity context menus with glyph rows, inspector entity/component action menus with glyph rows, editable/deletable timeline transform keys with Ctrl/Shift multi-select and drag-to-frame editing, timeline range/FPS/fit/jump controls plus direct Render Sequence queuing, Project Manager startup gating, clickable Layout workspace presets for Default Editor, Content Editing, Lighting, Animation / Timeline, and Debug / Profiling, docked Render Settings preview actions for Reset Accumulation, Debug View, Intermediate targets, and Denoiser toggling, and visible render-output modal flow with progress, cancel, manifest, history, and output-folder actions are implemented. `content_browser_audit.ps1 -FailOnIssues` verifies prefab drag/drop placement targets and the placement-selection handoff; `interaction_parity_audit.ps1 -FailOnIssues` verifies placed/created/duplicated entity selection handoff, viewport context menu parity controls, and hierarchy/content/inspector/component context menu coverage. |
| Phase 14: Regression And Validation | [done] | UI/UX audits run; completed targeted gates pass; Debug build passes; required headless diagnostic smoke with `scenes/validation/cornell.rtlevel` produced `out/profile.json`, `out/rendergraph.json`, `out/debug_views/`, and `out/debug_package/`; command-menu audit now verifies required reference top-menu sections/rows, drawn top-menu glyph rows, scene-tab drawn close chrome, and absence of legacy bracket icon fallbacks; style metrics audit now verifies shared base theme colors, row-selection, Inspector header metrics, viewport overlay chrome metrics, dock panel-border/splitter metrics, dock chrome style coverage, scene-tab chrome, Timeline chrome, shared disabled styling, and native dock-tab glyph overlay source coverage; content browser audit verifies icon details actions, CPU raster thumbnail preview support, GPU resident scene texture thumbnail support, standalone GPU raster/HDR and generated non-image preview caching, generated non-image preview disk caching, import operation queue support, completed-load banner collapse, quiet empty details state, Application-owned async import worker coverage, prefab drag/drop placement targets, and placed-prefab selection handoff; interaction parity audit verifies placed/created/duplicated entity selection handoff, viewport context menu parity controls, and hierarchy/content/inspector/component context menu coverage; render workflow audit verifies docked Render Settings preview actions, menu/diagnostic fallback coverage, editor render job state, manifest output, visible Render Output modal progress, sequence frame readout, Stop Render, and Open Output actions; inspector audit verifies component coverage, broad shared property-row helper usage, state-card chrome, component header/action chrome, locked-state banner, icon add-component controls, raw component remove-button removal, and authored unsupported-field persistence; timeline audit verifies stable key identity, multi-select, selected deletion, drag editing, and sequence workflow controls; project-manager audit verifies saved project thumbnail loading, generated thumbnail capture, deferred renderer startup, explicit scene startup bypass, deterministic project-startup screenshot override/recovery suppression, and Lightweight Sponza startup-scene wiring plus renderable mesh entity; `rtlevel_schema_validator.ps1 -Path scenes\validation\lightweight_sponza.rtlevel -FailOnInvalid` validates the Sponza scene document; `capture_editor_screenshot.ps1 -UseProjectStartup -Project Samples\LightweightSponza\LightweightSponza.vproject -FailOnIssues` opens the full editor without the Project Manager or recovery modal; `capture_editor_screenshot.ps1 -FocusWindow Content -ClickX 270 -ClickY 648 -FailOnIssues` verifies the runtime generated GPU folder preview path; screenshot regression tooling supports reference inventory, optional current-vs-baseline comparison, explicit `-UpdateBaseline` acceptance, required editor-state-set checks, required deeper reference-scene state checks, deterministic dock-window focus, explicit glTF capture, deterministic project startup capture, explicit `-WindowWidth`/`-WindowHeight` reference-size captures, and temporary preference isolation so `openLastProject` cannot hijack standalone scene captures; `capture_editor_ui_states.ps1 -FailOnIssues` captures Project Manager home, default editor with Content active, Content-active, Timeline-active, Render Settings-active, selected-camera, and selected-light screenshots under `out/editor_ui_ux_audits/states/`; `refs/editor` contains the blessed seven-state baseline set; `capture_editor_reference_scene_states.ps1 -FailOnIssues` captures Sponza default, Content-active, Timeline-active, and selected-sun screenshots under `out/editor_ui_ux_audits/reference_scenes/`; `refs/editor_reference_scenes` contains the blessed four-state deeper reference-scene baseline set; `editor_screenshot_regression.ps1 -Baseline refs\editor -Current out\editor_ui_ux_audits\states -RequireEditorStateSet -FailOnDifference` compares all seven Cornell/editor state screenshots within thresholds; and `editor_screenshot_regression.ps1 -Baseline refs\editor_reference_scenes -Current out\editor_ui_ux_audits\reference_scenes -RequireReferenceSceneSet -FailOnDifference` compares all four deeper reference-scene screenshots within thresholds. |

## Reference Inputs

Use these screenshots as the visual target:

- `2026-05-31 001413`: volumetric/light scene, AreaQuad Light selected, Timeline active.
- `2026-05-31 003505`: canyon scene, Sun Light selected, Content active.
- `2026-05-31 001611`: forest scene, Camera selected, Content active.
- `2026-05-31 001548`: Sponza scene, Sun Light selected, texture folder open.

Also use:

- `docs/EDITOR_REFERENCE_SCREENSHOT_INSPECTION.md`
- `docs/LEVEL_EDITOR_FEATURE_CHECKLIST.md`

## Non-Negotiable Requirements

1. Match the reference layout before adding new visual experiments.
2. Make the editor object-centric, not renderer-debug-centric.
3. Remove the default floating `Render Settings` window from the viewport.
4. Combine `Render World Settings` and `Render Settings` into one right-side dock tab.
5. Remove `Scene Import / Compatibility / Scene Document` controls from the `Content` panel.
6. Keep the `Content` panel usable without vertical scrolling in its default state.
7. Hide the current engine `Debug / Profile` panel from the default layout.
8. Preserve editor mode, headless mode, profile export, RenderGraph dumps, debug views, debug packages, validation scenes, fixed-seed behavior, and RenderDoc capture support.
9. Do not make RenderDoc required for normal editor execution.
10. Do not remove required debug/profiling workflows; redistribute the current debug/profile functions into `View Settings`, `Stats`, or `Draw Debug` based on the kind of data/function, with export/batch commands still available through menus or advanced panels.
11. Show the Project Manager first on normal interactive startup; create/open/select a project before opening the full editor.
12. Default new project folders to the Windows Documents directory, using an Unreal-style project root folder.
13. Every hierarchy row must have an icon. No entity, folder, component, imported root, or world object should render as a plain text row without an icon.
14. Template-created lighting/atmosphere/world actors must be functional, not decorative placeholders.
15. Use `Vibode Engine` as the product/editor name and `.vproject` as the project file extension.

## Phase 0: Project Manager Startup

Normal interactive startup should open a Project Manager before the full engine/editor scene is created.

### Step 0.1: Startup flow

Default launch flow:

```text
Start executable
  Project Manager opens first
    New Project
    My Projects
    Recent Projects
    Sample Projects
    Open Project
    Load on Startup option
  User selects or creates project
  Engine/editor opens selected project
  Scene/editor layout loads
```

The full editor should not immediately create an untitled scene and start as if a project already exists unless the user has selected:

- load most recent project on startup
- load a specific project on startup
- command-line project path
- explicit developer/debug bypass

### Step 0.2: Project Manager layout

Target Project Manager should be close to the Unreal-style reference:

- dark standalone window
- left navigation rail
- app logo/name area using `Vibode Engine`
- window title using `Vibode Engine`
- `New Project`
- `My Projects`
- `Home`
- `News` optional
- `Getting Started` optional
- `Sample Projects`
- resources/community links optional
- `Load on Startup` dropdown
- central `Recent Projects` section
- project cards with thumbnails
- `See all projects` button
- project search/filter where useful

### Step 0.3: New project defaults

Default Windows project root:

```text
%USERPROFILE%\Documents\Vibode Projects
```

If the product name changes, the folder name may change, but the default location should remain under:

```text
%USERPROFILE%\Documents
```

New project layout should follow a predictable structure:

```text
MyProject
  Config
  Content
  Scenes
  DerivedDataCache
  Intermediate
  Saved
  MyProject.vproject
```

The default project path must be editable before project creation.

### Step 0.4: Recent projects

Recent project behavior:

- show recent projects before opening the editor
- show thumbnail if available
- show project name
- show current/recent marker where relevant
- double-click opens project
- missing projects are shown as unavailable or removable
- `Open Current Project Directory` opens the project folder in Windows Explorer after a project is selected

### Step 0.5: Project templates

Project creation should offer templates that decide which default scene/world actors are created.

Example templates:

- Empty
- Basic Lit
- Outdoor / Atmosphere
- Interior
- Path Tracing Validation
- Cinematic

Template defaults should be deterministic and documented.

### Step 0.6: New Project template grid

The Project Manager needs a proper new-project creation surface, not only a button.

Target layout:

- left navigation rail remains visible
- central template grid
- template cards with icon/thumbnail
- template name
- short template description
- selected template blue outline/highlight
- project name field
- project location field
- browse button
- create button
- cancel/back button
- optional starter content toggle
- optional create default scene toggle

Required validation:

- project name cannot be empty
- project name cannot contain invalid Windows path characters
- project folder cannot already contain a conflicting `.vproject`
- duplicate project name in the same location requires rename or confirmation
- unavailable location shows a clear error
- read-only location shows a clear error
- missing parent folder can be created after confirmation
- project file path preview is shown before create

### Step 0.7: Project thumbnails

Project cards should use thumbnails.

Sources in priority order:

1. saved project thumbnail from the last editor session
2. startup scene thumbnail
3. template thumbnail
4. generic Vibode project icon fallback

Acceptance criteria:

- Recent projects and template cards have visual thumbnails/icons.
- Missing thumbnails do not produce blank cards.
- The selected/current project is visually marked.

Acceptance criteria:

- Normal interactive startup opens Project Manager first.
- A user can create a project before the full editor opens.
- A user can select a recent project before the full editor opens.
- New projects default to `%USERPROFILE%\Documents\Vibode Projects`.
- The default path can be changed.
- The editor can still be launched directly with an explicit project path for developer/testing workflows.

## Final Target Layout

The final editor should use this default layout:

```text
Vibode Engine Window
  Top menu bar
    File | Create | Engine | Window | Render | Layout | scene title | fps/ms

  Main dock area
    Left/main
      Scene viewport tab
        viewport toolbar
        viewport status/actions strip
        rendered scene

    Right dock
      Upper tabs
        Hierarchy
        Render Settings

      Lower tabs
        Inspector

    Bottom dock
      Content
      Timeline
      Log

    Hidden by default / opened only when requested
      Debug / Profile
```

Important target behavior:

- The viewport should be the visual focus.
- Right-side tabs should not steal space from the viewport more than necessary.
- Bottom tabs should not force the content browser to scroll just to access basic browser controls.
- The default bottom dock should show only `Content`, `Timeline`, and `Log`.
- `Debug / Profile` should be hidden by default and opened from `Window`, `Render`, or an advanced/debug command.
- The inspector should update based on the selected object.
- The unified `Render Settings` tab should replace both the floating render window and the old `Render World Settings` tab.

## Reference 001611 Gap Audit

The `2026-05-31 001611` reference image adds several exact-match details that are easy to miss if the plan only describes high-level panels.

### Dock and tab chrome

Missing details to implement:

- panel tabs include small icons before labels
- active tabs use a slightly brighter dark fill
- tab labels use compact text and tight padding
- tabs can show a close `X` where the reference shows one
- dock regions have thin separators and splitters
- panel headers use small collapse/dropdown affordances on the left where shown
- right-side panel tab strip includes a close `X` at the far right
- bottom tabs use icons: `Content`, `Timeline`, and `Log`
- `Scene` viewport tab includes an icon and close `X`
- top title text uses vertical separators around the scene name, for example `| Untitled Scene |`
- window title uses the product name, for example `Untitled - Vibode Engine`
- dirty/unsaved scene state should be visible in the title/tab, for example `Untitled Scene*`

### Hierarchy toolbar

Missing details to implement:

- a compact type-filter toolbar below the `Hierarchy` tab row
- filter icons for common entity classes such as object/mesh, camera, light, settings/world, effects/atmosphere, and cloud/volume
- active filter icon uses the same blue accent as selected rows
- hierarchy search remains below the filter toolbar
- hierarchy rows keep the visibility eye aligned to the far right

### Content browser exact behavior

Missing details to implement:

- content search placeholder should read like the reference: `Filter in selected folder...`
- the bottom `Content` panel itself should not vertically scroll
- each content pane may scroll independently: folder tree, asset list, and details/preview
- the folder tree should show project/root folders, not only `Content` and `Scenes`
- the default tree can include current project root, engine root, current workspace root, and mounted project roots
- the default tree should still avoid showing the whole computer unless the user enters filesystem/import mode
- folder tree remains folder-only
- middle asset list remains file/asset-only for the selected folder
- right details pane shows `No supported files selected` when selection has no preview/action

### Inspector exact behavior

Missing details to implement:

- inspector tab has icon plus `Inspector` label plus close `X`
- inspector has its own internal vertical scrollbar
- component headers use gray bars and compact disclosure arrows
- property rows use right-aligned labels and compact numeric fields
- dropdown fields show a visible arrow on the right
- reset/revert icons appear on transform rows
- disabled checkboxes and unsupported controls use muted styling

### Viewport exact behavior

Missing details to implement:

- active viewport toolbar icon uses blue highlight
- toolbar icons are tiny and flat, with no large button boxes
- viewport status strip text is low-contrast and aligned to the top-right
- viewport should remain unobstructed by default
- transform/light/camera gizmos should draw over the rendered scene without opening editor panels

Acceptance criteria:

- These small chrome details are tracked as required UI work, not optional polish.
- The reference frame can be compared against the editor and each item above can be checked visually.

## Phase 1: Baseline UI Style Constants

Create one shared editor style layer before changing individual panels.

### Step 1.1: Define style constants

Add or centralize constants for:

- editor background color
- panel background color
- panel header color
- tab active color
- tab inactive color
- row background color
- hovered row color
- selected row blue
- disabled row color
- text color
- muted text color
- separator color
- warning text color
- icon color
- active icon color
- scrollbar color

### Step 1.2: Define layout metrics

Add constants for:

- top menu height
- tab height
- panel header height
- toolbar height
- hierarchy row height
- content row height
- inspector row height
- component header height
- icon size
- visibility icon size
- tree indentation width
- splitter thickness
- property label column width
- property value column width
- vector field width
- slider width
- checkbox size
- scrollbar width

### Step 1.3: Define text sizing

Use a compact but readable editor font scheme:

- menu text
- tab text
- panel header text
- row text
- property label text
- property value text
- muted metadata text

Acceptance criteria:

- UI sizes are controlled by shared constants.
- No panel hardcodes a different row height unless it has a specific reason.
- Selected row blue is consistent in hierarchy and content browser.
- The style is dark gray, not pure black-heavy debug UI.

## Phase 2: Remove Content Panel Clutter

The `Content` panel must become a clean asset browser. It should not contain scene import forms, compatibility forms, scene document forms, or long warning blocks.

### Step 2.1: Remove visible import forms from Content

Remove these from the default `Content` tab:

- `Scene Import / Compatibility`
- `Import Scene as New Scene` path field
- `Environment` HDRI path field
- `Scene Document` path field
- `Import Scene as New` button
- `Import HDRI` button
- `Save Scene` button
- `Open Scene` button
- compatibility warning block
- project-manager warning block

These controls should not appear at the bottom of `Content`.

### Step 2.2: Move scene file actions to top menus

Move scene document actions to:

```text
File
  New Scene
  Open Scene...
  Save Scene
  Save Scene As...
  Recent Scenes
```

### Step 2.3: Move import actions to menu commands

Move import actions to:

```text
File
  Import
    Import Asset...
    Import Model as Asset...
    Import and Place...
    Import Scene as New Scene...
    Merge Scene into Current...
    Import Texture...
    Import HDRI...
    Import VDB...
    Import IES Profile...
```

The `Content` toolbar may still have a compact add/import icon that opens the same import menu, but the large form must be gone.

### Step 2.4: Replace compatibility warning with status feedback

Move compatibility/project warnings to one of these:

- bottom status bar
- non-blocking toast
- modal shown only when a user attempts a blocked action
- details panel message when relevant

Do not reserve permanent vertical space in the content browser for compatibility text.

### Step 2.5: Keep Content default state scroll-free

The default `Content` tab should fit without vertical scrolling at common editor window sizes.

Target default visible areas:

```text
Content toolbar
  + add/create/import dropdown
  search field with placeholder: Filter in selected folder...
  back/forward/up buttons
  refresh
  grid/list toggle
  hide details toggle
  breadcrumb path

Content body
  left section: folder tree only
  middle section: file/asset list
  right section: details/preview/actions panel
```

Acceptance criteria:

- Opening `Content` shows the browser immediately.
- There is no bottom import form.
- There is no `Scene Import / Compatibility` section.
- There is no `Scene Document` section.
- There is no forced vertical scroll just to access the main asset list.
- The `Content` panel itself does not vertically scroll; folder tree, asset list, and details/preview panes scroll independently when needed.
- The browser body is split into exactly three visible sections: folder tree, file/asset list, and details/preview/actions.
- Scene open/save/import actions still exist through top menus or compact toolbar menus.

### Step 2.6: Content plus button

The first control in the `Content` toolbar should be a compact `+` button with a small dropdown arrow, matching the reference.

Target behavior:

- clicking `+` opens a compact create/import menu
- clicking the arrow opens the same menu or a submenu variant
- the button is icon-first, not a wide text button
- actions affect the current selected folder when relevant
- unavailable actions are dimmed instead of hidden

Target menu items:

- New Folder
- New Scene
- New Material
- Import Asset...
- Import Into Scene...
- Import Texture...
- Import HDRI...
- Import IES Profile...
- Browse Filesystem...

Acceptance criteria:

- The `+` button is the first visible control in the Content toolbar.
- The middle asset list does not contain per-row action buttons.
- Create/import actions are available through the `+` dropdown and top menus.
- The `+` dropdown does not reintroduce the removed import/compatibility forms.

## Phase 3: Redesign Content Browser To Match Reference

### Step 3.1: Three-pane browser

Use the reference layout:

```text
Left pane: folder tree only
Middle pane: selected folder file/asset list
Right pane: details/preview/actions
```

This split is required, not optional. The default `Content` tab should always read as three sections:

1. left navigation section
2. middle file/asset selection section
3. right details/preview/actions section

The section splitters can be resizable, and the right details/preview section can be hidden with `Hide Details`, but the default reference-match layout should show all three sections.

### Step 3.1.1: Project/content-scoped folder tree

The folder tree should not show the entire computer by default.

Default folder roots should be limited to editor/project/workspace asset locations:

- current project root
- current project `Content`
- current project `Scenes`
- current engine/workspace root when useful
- engine `Content`
- engine `Scenes` when useful
- mounted asset libraries
- user-configured asset roots
- recent project/content roots

Full computer browsing should be a separate explicit mode or action:

- `File > Import > Import Asset...`
- `Content` toolbar import/add menu
- `Browse Filesystem...`
- OS-native file picker
- optional temporary `Filesystem` browser mode for advanced import workflows

Rationale:

- A full computer tree is noisy and slow in the default editor browser.
- It exposes unrelated system folders inside a project asset panel.
- It makes search/filter behavior ambiguous.
- It weakens the reference-style project/content browser UX.

The left tree still remains folder-only:

- no loose files
- no asset rows
- no import action rows
- no open/save scene document rows
- no preview/action controls

Acceptance criteria:

- The default left tree shows project/editor content roots, not the whole computer.
- The default left tree can show the current project/workspace folder hierarchy, including folders outside `Content` when they are inside the project/workspace root.
- The left tree displays folders only.
- Files/assets appear only in the middle file/asset list for the selected folder.
- Project/editor content roots are pinned or easy to reach.
- Users can still import from anywhere through explicit import/browse commands.
- Adding custom asset roots is supported through project settings or a mount command.

### Step 3.2: Folder tree behavior

The folder tree needs:

- disclosure arrows
- folder icons
- folders only; no loose files, assets, import action rows, or document action rows
- project/content root entries only by default
- selected folder blue highlight
- muted rows for unavailable folders
- compact indentation
- stable row height
- mouse hover state
- keyboard navigation

### Step 3.3: Asset list behavior

The asset list needs:

- file/asset rows for the selected folder
- a type icon on every row
- model icons
- material icons
- texture icons
- scene icons
- script/config icons where relevant
- selected asset highlight
- double-click open behavior
- right-click context menu
- drag-to-viewport behavior for placeable assets
- no per-row action buttons in the middle list by default
- no import/open/save scene forms in the middle list

The middle pane is for browsing and selecting files/assets. Actions for the selected file belong in the right details/preview pane.

Required file/asset icons:

- folder
- scene file: `.rtlevel`, `.mscene`
- project file: `.vproject`
- model file: `.gltf`, `.glb`, `.obj`, `.fbx`
- material file: `.mtl`, MaterialX, engine material assets
- texture/image file: `.png`, `.jpg`, `.jpeg`, `.tga`, `.bmp`
- HDR/image lighting file: `.hdr`, `.exr`
- IES light profile file: `.ies`
- volume file: `.vdb`
- shader file: `.glsl`, `.hlsl`, `.spv`, shader include files
- script/tool file where shown
- config/data file: `.json`, `.ini`, `.toml`, `.yaml`
- unknown/unsupported file fallback

Acceptance criteria:

- No visible asset/file row appears without a type icon.
- Unsupported files still get a generic file icon and can show details in the right panel.
- File actions remain in the right details/preview/actions panel, not in the middle list.

### Step 3.4: Breadcrumb behavior

Breadcrumb should match the reference:

```text
C: > Source > 1 > minitech > Engine > Content > bistro
```

Requirements:

- compact text
- clickable segments
- horizontal overflow handling
- back/forward/up navigation buttons

### Step 3.5: Details/preview panel

The right details/preview/actions panel should show:

- `No supported files selected` when nothing useful is selected
- thumbnail or preview when available
- type
- path
- dimensions for textures
- material slot summary for models
- scene/entity count for scene files when known

Selected-file actions belong here, not in the middle asset list.

Target actions by selected file type:

- scene file: open scene, merge scene, inspect metadata
- model file: import as asset, import and place, reimport, inspect mesh/material summary
- texture file: preview texture, import texture, reimport, inspect dimensions/format
- material file: open material, assign to selection when applicable, reimport
- HDRI file: import HDRI, set as environment when applicable
- IES file: import IES profile, assign to selected light when applicable
- unsupported file: show path/type only, no noisy action buttons

The middle asset list should remain a clean selectable list with icons, names, and lightweight metadata only.

Acceptance criteria:

- The content browser visually matches the reference structure.
- It is a browser, not a table-first debug panel.
- The right details panel can be hidden with `Hide Details`.
- File actions are shown in the right details/preview/actions panel, not as per-row buttons in the middle asset list.
- The browser remains usable at the same bottom-panel height shown in the screenshots.

### Step 3.6: Asset thumbnails and previews

The middle asset list uses type icons by default. The right details/preview panel provides richer previews.

Required preview behavior:

- texture/image files show thumbnail preview, dimensions, format, color space when known
- HDR/EXR files show a tone-mapped preview and HDR metadata when available
- material files show a material preview sphere or swatch when available
- model files show a generated thumbnail or simple preview with mesh/material summary
- scene files show a scene thumbnail and entity count when available
- project files show project thumbnail/name/location metadata
- IES files show a small profile/curve preview when possible
- VDB/volume files show metadata and a fallback volume icon if no preview exists
- unsupported files show a generic icon and clear unsupported reason

Thumbnail generation rules:

- generated thumbnails should be cached under the project `Saved` or cache area
- thumbnail generation must not block editor interaction
- missing/corrupt thumbnails should fall back gracefully
- thumbnails should not replace the clean list layout unless grid mode is enabled

### Step 3.7: Import and reimport UX

Import/reimport needs clear state feedback.

Required behavior:

- importing shows progress in a modal, toast, or status area depending on duration
- long imports can be cancelled when safe
- missing textures are reported with actionable paths
- unsupported files show a reason in the right details/preview panel
- reimport shows source path, destination asset path, and last import status
- successful import shows created/updated assets
- import failures are written to `Log` and shown as a non-blocking error notification
- conversion output path is visible before import when applicable
- imported assets select or reveal themselves in the Content browser after completion

Acceptance criteria:

- Import/reimport work never fails silently.
- The user can see what was created, skipped, or failed.
- File action buttons stay in the right details/preview/actions panel.

Implementation evidence:

- `Application` queues import/reimport jobs, runs `stagePlaceholderAssetImport` on a worker thread, polls completion each frame, and applies registry/save/notification/runtime prefab refresh on the main thread.
- `Import and Place` now places the imported prefab after the worker result commits, preserving the synchronous user flow without blocking the editor during staging.
- `content_browser_audit.ps1 -FailOnIssues` checks the operation queue and worker-thread staging hooks.

## Phase 4: Combine Render World Settings And Render Settings

The current `Render World Settings` and `Render Settings` should become one unified right-side dock tab named `Render Settings`.

### Step 4.1: Remove separate Render World Settings tab

Remove the standalone right-side `Render World Settings` tab from the default layout.

### Step 4.2: Remove default floating Render Settings window

The large floating `Render Settings` window should not appear by default.

Allowed behavior:

- advanced users may open a floating debug window from `Window > Debug > Floating Render Controls`
- the default workflow uses the docked right-side `Render Settings` tab

### Step 4.3: Create unified Render Settings sections

The unified `Render Settings` tab should use collapsible sections:

```text
Render Settings
  Quality / Preset
  Path Tracing
  ReSTIR
  Denoiser / TAA
  Tone Mapping
  Environment
  Sun / Lighting
  Sky / Atmosphere
  Post Process / GI
  Caustics
  Advanced
  Debug
```

### Step 4.4: Move current floating render controls

Move the existing floating controls into these sections:

`Quality / Preset`:

- render preset
- debug view
- TSR preset
- render resolution scale
- adaptive quality
- adaptive GPU target

`Path Tracing`:

- path tracing enable
- samples per pixel
- limit to 1 SPP
- max bounces
- environment samples
- direct lighting
- indirect strength
- material anisotropy
- specular AA
- opacity micromaps
- SER controls

`ReSTIR`:

- ReSTIR mode
- ReSTIR DI enable/settings
- ReSTIR GI enable/settings
- ReSTIR GI tuning

`Denoiser / TAA`:

- denoiser enable
- denoise while moving
- A-trous iterations
- TAA jitter
- motion-related stability controls

`Tone Mapping`:

- tone mapper
- exposure
- auto exposure
- physical camera link
- target luminance
- min exposure
- max exposure
- adaptation speed
- contrast
- saturation
- brightness
- gamma
- white point
- histogram controls

`Caustics`:

- MNEE caustics
- caustic-specific controls

### Step 4.5: Move Render World Settings controls

Move current world settings into these sections:

`Environment`:

- HDRI source
- import HDRI command
- reset accumulation
- environment enabled
- intensity
- background intensity
- rotation

`Sun / Lighting`:

- primary sun assignment
- create primary sun
- sun enabled
- illuminance
- direction/elevation/azimuth
- high fog/post process volume links if currently shown

`Sky / Atmosphere`:

- atmosphere enabled
- sky intensity
- Rayleigh scale
- Mie scale
- Mie anisotropy
- ground albedo

`Post Process / GI`:

- denoiser status
- TAA status
- ReSTIR GI status
- post process enabled
- exposure
- saturation
- contrast
- indirect strength

### Step 4.6: Preserve machine-readable debug workflow

Do not remove access to:

- profile JSON export
- render graph JSON/DOT dump
- debug view export
- debug package export
- validation suite
- fixed-seed controls
- RenderDoc capture support

Machine-readable export and automation commands can live under:

```text
Render Settings
  Debug

Window > Debug / Profile

Optional hidden Debug / Profile panel

Render menu
```

Interactive viewport-facing debug data should not be buried in the hidden panel. Route it through the viewport strip:

```text
View Settings = choose viewport/debug image mode or preview mode
Stats         = inspect numeric counters, timings, samples, memory, pass costs
Draw Debug    = toggle visual overlays, gizmos, bounds, debug draw passes
```

Acceptance criteria:

- Right-side upper tabs include `Hierarchy` and `Render Settings`.
- There is no separate `Render World Settings` tab.
- The floating render settings window is closed by default.
- All previous render/world controls still exist in the unified tab.
- The unified tab is grouped, searchable or collapsible enough to avoid a wall of controls.

## Phase 5: Hierarchy Match

The hierarchy should match the reference as a scene outliner.

### Step 5.1: Row structure

Each row should contain:

```text
disclosure arrow | object type icon | object name | visibility eye
```

This is mandatory for every row. There should be no iconless hierarchy item.

Rows that require icons include:

- camera
- sun/directional light
- environment light
- sky atmosphere
- height fog
- volumetric cloud
- post process volume
- floor/default geometry
- imported scene root
- imported mesh/object
- actor/group/folder
- material/prefab references where shown
- hidden/disabled objects
- unknown/custom entities

Unknown/custom entities should use a generic object icon rather than leaving the icon slot empty.

### Step 5.1.1: Hierarchy type-filter toolbar

The hierarchy should include a compact icon toolbar below the tab strip and above search.

Target toolbar contents:

- all/entities filter, cube icon
- camera filter, camera icon
- light filter, light bulb icon
- world/settings filter, gear icon
- effects/post-process filter, effects icon
- atmosphere/cloud/volume filter, cloud icon

Behavior:

- clicking an icon filters or highlights matching entity classes
- buttons are icon-only, no text labels
- icon order matches the reference as closely as possible: object/cube, camera, light, settings, effects, cloud/volume
- the icon strip sits directly below the `Hierarchy | Render Settings` tab row
- the hierarchy search field sits directly below the icon strip
- active icon uses the selected-row blue accent
- inactive icons use muted gray
- hover state slightly brightens the icon and row hit area
- each icon has a tooltip
- the filter state can be cleared by clicking the active filter again or using the all/entities icon

Acceptance criteria:

- The hierarchy matches the reference top area: tab row, icon filter row, search field, then entity rows.
- The filter toolbar does not replace hierarchy search.
- The filter toolbar is made of compact icon buttons, not text buttons.

### Step 5.2: Object icons

Add icons for:

- camera
- environment light
- sun/directional light
- point light
- spot light
- area light
- mesh/static object
- imported scene root
- folder/group
- volume
- sky/atmosphere
- height fog
- volumetric cloud
- post process volume
- unknown/custom entity fallback
- material/prefab where relevant

### Step 5.3: Visibility state

Add reference-style eye toggles:

- visible eye
- hidden crossed/disabled eye
- inherited hidden state if parent is hidden

Exact behavior:

- every hierarchy row has an eye button aligned to the far right
- the eye column is stable and does not shift when names are long
- visible rows show a normal eye icon
- hidden rows show a crossed/muted eye icon
- inherited-hidden rows use a muted inherited state
- clicking the eye toggles entity visibility without changing selection unless the row was already selected by that click behavior
- hovering a row makes the eye target clear enough to click
- eye buttons are icon-only with tooltips
- disabled/unsupported entities keep the eye visible but dimmed when visibility cannot be changed

Acceptance criteria:

- No hierarchy row is missing the eye affordance.
- Visibility can be toggled directly from the hierarchy for every supported entity.
- Eye icons remain right-aligned like the reference screenshot.

### Step 5.4: Selection state

Selection behavior:

- clicked row becomes blue
- selected object name appears in inspector
- selected object gets viewport gizmo
- selection remains stable when expanding/collapsing parents
- clicking empty hierarchy space clears selection only if intended

### Step 5.5: Search behavior

Search field:

- placeholder `Search...`
- filters matching rows
- keeps parents visible for matching children
- highlights or preserves selection when possible

### Step 5.6: Nested scene roots

Imported scene roots should expand like:

```text
test-quixel-canyon55
  Environment Light
  Camera
  Sun Light
  Actor
  scene
  BatMobile
```

Acceptance criteria:

- The hierarchy looks like the reference, not a checkbox debug list.
- Object type, visibility, nesting, and selection are clear at a glance.
- Search and visibility controls are visible without opening context menus.
- Every visible hierarchy row has an icon, including lighting, atmosphere, volume, post process, imported, unknown, and disabled rows.

### Step 5.7: Template-created world actors

Project templates should create default project/scene-level world actors when appropriate.

Example default actors by template:

`Empty`:

- Camera only, or no actors if explicitly selected

`Basic Lit`:

- Camera
- Sun Light
- Environment Light
- Post Process Volume

`Outdoor / Atmosphere`:

- Camera
- Sun Light
- Environment Light
- Sky Atmosphere
- Height Fog
- Volumetric Cloud when supported
- Post Process Volume

`Interior`:

- Camera
- Environment Light
- Post Process Volume
- optional area/rect light

`Path Tracing Validation`:

- Camera
- Sun Light or area light depending on validation scene
- Environment Light
- Post Process Volume
- required validation scene geometry

These actors should appear at scene/project root level unless the template deliberately groups them.

### Step 5.8: Lighting/atmosphere actors must be functional

The current visible lighting/atmosphere-style entries should not be inert list items.

Required behavior:

- `Environment Light` controls the active HDRI/procedural environment, background intensity, environment intensity, and rotation.
- `Sky Atmosphere` controls sky scattering parameters and feeds the renderer's atmosphere/sky path.
- `Height Fog` controls fog density/falloff/color/height and affects viewport rendering when enabled.
- `Volumetric Cloud` controls cloud/volume source and affects rendering when the template or renderer supports it.
- `Post Process Volume` controls exposure, tone mapping, color correction, bloom, vignette, film grain, and related post-process settings within its scope.
- `Sun Light` remains the directional/sun light and should interact with sky/atmosphere when those actors are present.

If a renderer feature is unavailable, the actor should show a disabled or unsupported state in the inspector with a clear reason. It should not silently appear to do nothing.

Acceptance criteria:

- Template-created lighting/world actors visibly affect rendering when edited.
- Unsupported world actors are clearly marked as unsupported.
- These actors have hierarchy icons, inspector components, visibility/enabled state, and real render-setting bindings.

## Phase 6: Inspector Match

The inspector is the most important UX difference. It must become component-driven.

### Step 6.1: Inspector header

Header layout:

```text
Selected Object Name                                 Name
```

Requirements:

- selected object name field
- optional lock/pin support later
- clear empty state when nothing is selected

### Step 6.2: Component header style

Each component uses:

```text
disclosure arrow | component icon | component name | optional enable checkbox | optional remove/menu button
```

### Step 6.3: Transform component

Every scene object should expose:

- Position vector3
- Rotation vector3
- Scale vector3
- Show World Matrix toggle
- reset/revert icons per row
- optional link scale icon

Match the reference compact three-column numeric layout.

### Step 6.4: Camera component

Camera inspector should expose:

- near clip
- exposure mode
- aperture size
- ISO
- shutter speed
- film size
- focal length
- enable motion blur
- camera-specific post process sections

Camera post-process groups:

- DOF
- Bloom
- Color correction
- Vignetting
- Film grain

### Step 6.5: Light component

Light inspector should expose:

- light type
- light units
- IES profile
- color temperature toggle
- RGB values
- color swatch
- light intensity
- exposure multiplier
- cone angle
- cone penumbra
- radius
- visible to camera
- material source
- save material
- cast surface shadows
- cast volumetric shadows
- shadow bounce controls where supported

Sun/directional light should expose:

- illuminance
- azimuth
- elevation
- color temperature
- intensity
- exposure multiplier
- softness
- cast surface shadows
- cast volumetric shadows

### Step 6.6: Mesh/material component

Mesh objects should expose:

- mesh asset reference
- material slots
- material assignment controls
- visibility
- shadow casting
- ray tracing participation
- optional LOD information
- bounds summary

### Step 6.7: Environment and volume components

Environment/volume objects should expose:

- enabled toggle
- source asset
- density/intensity controls
- scattering/absorption where supported
- sky or atmosphere linkage where relevant

### Step 6.8: Empty inspector state

When nothing is selected:

- show a small muted `No selection` message
- do not show global render controls
- do not show large empty forms

Acceptance criteria:

- Selecting Camera, Sun Light, AreaQuad Light, mesh object, and environment object changes the inspector.
- The inspector resembles the reference screenshots in grouping and density.
- Global render settings are not mixed into selected-object inspector controls.

## Phase 7: Viewport Toolbar And Scene Controls

### Step 7.1: Replace text buttons with icons

Replace default text controls:

- `Q`
- `W`
- `E`
- `R`
- `World`
- `Snap`
- `Grid`
- `Axes`

With compact icon-style controls:

- select
- move
- rotate
- scale
- world/local space
- snap
- grid
- axis/gizmo
- camera/view mode

Text can remain in tooltips, not as primary toolbar labels.

Implementation status: the `Scene` viewport transform toolbar now uses shared custom-drawn ImGui icon buttons for select/move/rotate/scale, world/local, snap, grid, axes, and frame-selected controls, with command names and shortcuts kept in tooltips. The top-right strip also shows the camera navigation speed next to the viewport action buttons.

### Step 7.2: Match viewport top strip

Viewport should show:

```text
pt current/target gpu_ms | View Settings | Stats | Draw Debug | camera speed/value
```

The target visual style is a compact top-right viewport strip:

```text
pt 82/256 17.372   [monitor icon] View Settings   [bars icon] Stats   [bug icon] Draw Debug   [camera icon] 33.000
```

Requirements:

- low-contrast overlay text
- compact icon plus label buttons
- no heavy button boxes unless hovered or active
- active states use the same blue accent as selected rows
- each item opens a small popover or dropdown, not a large blocking window
- each icon-only or icon-plus-label item has a tooltip

### Step 7.3: View Settings popover

`View Settings` should control how the viewport is viewed, not global renderer quality.

Target contents:

- viewport shading/debug view mode
- debug buffer/view selector for image-like outputs
- beauty/base color/normal/depth/roughness/metallic/emissive preview modes when available
- path tracing, ReSTIR, denoiser, TAA, and post-process debug image modes when they replace the viewport image
- camera/view selection
- exposure preview override
- tonemapper preview override when useful
- resolution scale preview if editor-only
- show/hide grid
- show/hide axis gizmo
- show/hide transform gizmos
- show/hide selection outlines
- show/hide light icons
- show/hide camera icons
- show/hide bounds
- show/hide safe frame or camera frame
- viewport navigation speed shortcut

Implementation status: viewport camera navigation speed is visible in the top-right strip and editable from the `View Settings` popover.

Behavior:

- opens as a compact anchored popover below the `View Settings` button
- closes when clicking outside
- does not change persistent render presets unless the option explicitly says it does
- keeps editor-only visibility controls separate from scene object visibility

Acceptance criteria:

- `View Settings` is visible in the top-right viewport strip.
- The popover is compact and does not cover the center of the viewport.
- View options are not mixed into the object inspector.
- Image-like debug views from the old debug/profile UI are available here.

### Step 7.4: Stats popover

`Stats` should expose performance and rendering counters in a compact viewport-local panel.

Target contents:

- fps
- frame time in ms
- GPU frame time
- CPU frame time if available
- path tracing sample progress
- current SPP / effective SPP
- accumulation state
- selected render preset
- resolution scale
- VRAM/resource summary if available
- top expensive render passes
- per-pass GPU timing summary
- frame avg/min/max/p95/p99 when available
- memory/resource lifetime summary when available
- ReSTIR enabled/status
- denoiser/TAA enabled/status
- validation/profile status flags when available

Behavior:

- the default viewport strip only shows the short readout
- clicking `Stats` opens a compact stats popover
- the popover can optionally pin open
- detailed profiler output remains in the hidden/optional `Debug / Profile` panel, not in the viewport by default

Acceptance criteria:

- `Stats` feels like the reference compact status tool.
- It gives immediate performance context without replacing the profiler panel.
- Numeric debug/profile data from the old debug/profile UI is available here.
- Detailed profiler output remains in the hidden/optional `Debug / Profile` panel.
- It does not permanently consume viewport space.

### Step 7.5: Draw Debug popover

`Draw Debug` should control scene and renderer debug overlays.

Target contents:

- draw transform gizmos
- draw light gizmos/icons
- draw camera gizmos/icons
- draw object bounds
- draw selected bounds only
- draw mesh wireframe overlay
- draw normals/tangents overlay
- draw collision/proxy bounds if available
- draw BVH/TLAS/BLAS debug overlays if available
- draw radiance cache/probe debug overlays if available
- draw ReSTIR DI/GI debug overlays
- draw denoiser history/motion debug overlays
- draw RenderGraph pass/debug overlay if available
- draw any old debug/profile visual overlay that does not replace the viewport image

Behavior:

- opens as a compact anchored popover
- overlay toggles should be checkboxes
- expensive overlays should show a warning or muted note
- debug view export stays available through `Render` menu, `Render Settings > Debug`, or the hidden/optional `Debug / Profile` panel

Acceptance criteria:

- `Draw Debug` is a viewport overlay controller, not a replacement for the debug package workflow.
- Overlay-style debug functions from the old debug/profile UI are available here.
- Toggling overlays does not alter scene data.
- The popover matches the small reference control style.

### Step 7.6: Route current debug/profile data and functions

The existing engine debug/profile UI should be hidden from the default layout, but its useful data and functions must remain accessible through the reference-style viewport controls.

Routing rules:

| Current data/function type | New default location |
|---|---|
| Debug image mode that replaces the viewport image | `View Settings` |
| Viewport preview mode, camera/view mode, grid, gizmo visibility | `View Settings` |
| Performance counters, frame timing, SPP, accumulation state | `Stats` |
| GPU/CPU timing summaries and per-pass timing summaries | `Stats` |
| Memory/resource summaries | `Stats` |
| ReSTIR, denoiser, TAA status text/counters | `Stats` |
| Object bounds, light/camera icons, transform gizmos | `Draw Debug` |
| Wireframe, normals, tangents, collision, BVH, probes, ReSTIR overlays | `Draw Debug` |
| Debug export, debug package, profile JSON, RenderGraph dump | `Render` menu or `Render Settings > Debug` |
| Full legacy/debug table view for engineering use | hidden optional `Debug / Profile` panel |

Acceptance criteria:

- Hiding `Debug / Profile` does not remove its useful data/functions.
- Every current debug/profile item has an assigned new home.
- User-facing viewport debug tools are primarily `View Settings`, `Stats`, and `Draw Debug`.
- Export and automation workflows remain accessible but do not clutter the default layout.

### Step 7.7: Camera speed/value control

The numeric value at the far right of the viewport strip should behave like a compact viewport navigation speed control.

Target behavior:

- displays the current viewport camera speed/value
- clicking opens a small slider or preset list
- mouse wheel over the value adjusts speed
- value persists in editor preferences
- does not affect render exposure or scene camera focal settings

Acceptance criteria:

- The control visually matches the reference small numeric item.
- It is clearly viewport navigation state, not render quality state.

### Step 7.8: Gizmo behavior

Selected objects need viewport gizmos:

- transform gizmo for mesh/camera/light
- light-specific icon or handle
- camera icon/handle
- disabled objects use muted gizmo color

### Step 7.9: Keep viewport unobstructed

Default viewport must not be blocked by:

- floating render settings
- import forms
- large debug windows

Acceptance criteria:

- The viewport top bar visually resembles the reference.
- Primary toolbar controls are icon-first.
- `View Settings`, `Stats`, and `Draw Debug` are compact viewport popovers.
- The camera speed/value control is present at the far right of the strip.
- The rendered scene remains the dominant visual surface.

## Phase 8: Timeline Match

### Step 8.1: Timeline top controls

Timeline should include:

- play/pause
- stop
- current frame
- start frame
- end frame
- optional record/add key controls

### Step 8.2: Timeline ruler

Add:

- frame tick marks
- labeled frame intervals
- current frame playhead
- selected range highlight
- range text such as `0 - 270`

### Step 8.3: Track area

Add or prepare rows for:

- transform keys
- camera keys
- light keys
- material keys

Acceptance criteria:

- Timeline looks like a compact sequencer, not a plain table.
- It matches the reference bottom panel density.
- It can be active without hiding or breaking the content/log tabs.

## Phase 8.5: Notifications, Commands, And Layout Persistence

### Step 8.5.1: Status bar and notifications

Because import/compatibility/debug messages are removed from the default Content panel, the editor needs a clear notification system.

Required surfaces:

- bottom status text area for short persistent state
- transient toast notifications for save/import/render completion
- non-blocking error notifications for failed actions
- click-through action such as `Open Log`, `Show Details`, or `Open Output Folder`
- warning badges on affected panels when relevant

Notification types:

- info
- success
- warning
- error
- progress

Acceptance criteria:

- save/import/render/debug failures are visible without opening `Debug / Profile`
- notifications do not block normal editing unless the action requires confirmation
- every notification with details links to `Log` or the relevant panel

### Step 8.5.2: Command palette and shortcut map

Add a central command registry so menu items, shortcuts, toolbar buttons, and command search call the same commands.

Required behavior:

- command palette opens from a shortcut such as `Ctrl+Shift+P`
- command palette searches all available commands
- commands show current shortcut when assigned
- disabled commands are shown with a reason
- shortcut map is editable or at least documented in preferences
- shortcuts are conflict-checked
- command execution writes errors to `Log`

Minimum command categories:

- File
- Create
- Render
- View
- Window
- Layout
- Debug
- Content
- Selection

### Step 8.5.3: Layout persistence

The editor must save and restore layout state.

Persist:

- dock split sizes
- selected tabs
- hidden/visible panels
- panel order
- bottom panel height
- right dock width
- right dock vertical split
- content browser column/split widths
- open project layout preference
- active layout preset

Behavior:

- project-specific layout overrides global defaults when present
- global default layout is used for new projects
- `Reset Layout` returns to the reference-match layout
- corrupted layout config falls back to default and reports a warning

Acceptance criteria:

- closing and reopening the editor restores the user's layout.
- reset layout reliably returns to the reference default.
- layout persistence does not break headless mode.

## Phase 9: Top Menus Match

### Step 9.1: Canonical menu order

Use:

```text
File | Create | Engine | Window | Render | Layout
```

### Step 9.2: File menu

The `File` menu should match the reference dropdown style:

- dark vertical dropdown
- search field at the top
- compact row height
- left icon per command
- right-aligned shortcuts
- muted section labels
- thin separators between sections
- disabled rows are visible but dimmed
- submenus use a right arrow

Target structure:

```text
File
  Search field: Start typing to search

  OPEN
    New Scene...                         Ctrl+N
    Open Scene...                        Ctrl+O
    Favorite Scenes
    Open Asset...                        Ctrl+P

  SAVE
    Save Current Scene                   Ctrl+S
    Save Current Scene As...             Ctrl+Alt+S
    Save All                             Ctrl+Shift+S
    Choose Files to Save...              Ctrl+Alt+Shift+S

  IMPORT / EXPORT
    Import Into Scene...
    Export All...
    Export Selected...                   disabled when no selection

  PROJECT
    New Project...
    Open Project...
    Zip Project
    Open Current Project Directory
    Recent Projects                      submenu

  EXIT
    Exit
```

Notes:

- Use `Scene` naming in this engine, even when the reference screenshot says `Level`.
- `Open Asset...` should open an asset picker or focus the content browser search.
- `Import Into Scene...` is for placing external content into the current scene.
- `Export Selected...` should be disabled when there is no selected entity or asset.
- Scene/import/document controls removed from the `Content` panel should live here or under `Render` where appropriate.

Acceptance criteria:

- The File dropdown visually resembles the reference menu screenshot.
- Commands are grouped by `OPEN`, `SAVE`, `IMPORT / EXPORT`, `PROJECT`, and `EXIT`.
- Search is available at the top of the menu.
- Shortcuts are right-aligned.
- Disabled command styling is implemented.

### Step 9.3: Create menu

Create menu should own scene object creation:

Target structure:

```text
Create
  Search field: Start typing to search

  ENTITY
    Empty Entity
    Folder / Group

  3D OBJECT
    Cube
    Sphere
    Plane
    Cylinder
    Cone
    Mesh From Asset...

  LIGHT
    Directional Light / Sun
    Point Light
    Spot Light
    Rect Area Light
    Disk Area Light
    Sphere Light
    Emissive Mesh Light

  CAMERA
    Camera
    Cine Camera

  ENVIRONMENT
    Environment Light
    Sky Atmosphere
    Height Fog
    Volumetric Cloud
    Post Process Volume

  ASSET
    Material
    Material Instance
    Prefab From Selection
```

Acceptance criteria:

- Create dropdown uses icons, grouped section labels, and disabled states like the File menu.
- Created entities become selected and appear in the hierarchy with icons.
- Unsupported create commands are visible but dimmed with tooltips.

### Step 9.3.1: Engine menu

The `Engine` menu should own engine/editor-level operations.

Target structure:

```text
Engine
  Project Settings...
  Editor Preferences...
  Engine Settings...
  Asset Registry
    Rebuild Asset Registry
    Validate Asset References
  Cache
    Clear Derived Data Cache...
    Open Cache Directory
  Validation
    Run Validation Suite
    Run Current Scene Checks
  Diagnostics
    Open Log Folder
    Open Debug Package Folder
    Copy System Info
```

Acceptance criteria:

- Engine menu does not duplicate object creation or render output commands.
- Long-running commands show progress and write to Log.

### Step 9.4: Render menu

Render menu should own rendering/debug commands:

- Render current viewport
- Render image...
- Render sequence...
- Stop render
- Pause/resume render when applicable
- Reset accumulation
- Render settings
- Screenshot
- High resolution render
- Capture RenderDoc
- Export debug views
- Export debug package
- Dump RenderGraph
- Profile current scene

### Step 9.4.1: Render button/function

There should be a clear render command that users can invoke without knowing debug/profiling tools.

Target behavior:

- `Render` appears as a top menu item in the main menu bar.
- `Render current viewport` starts a render using the current viewport camera and active render settings.
- `Render image...` opens a compact modal for output path, resolution, samples/frames, format, and whether to use current viewport or selected camera.
- `Render sequence...` uses timeline frame range and active/selected camera.
- while rendering, the command changes to `Stop render` or shows a stop command nearby.
- progress is visible through the viewport status strip or a small status/progress notification.
- completed renders provide an `Open Output Folder` action.
- render failures go to `Log` and show a non-blocking error notification.

Acceptance criteria:

- A normal user can find and run a render from the top menu.
- Render output does not require opening `Debug / Profile`.
- Render progress and completion are visible without blocking the editor.

### Step 9.4.2: Render output history and queue

Renders that take longer than a quick screenshot need visible history/queue state.

Required behavior:

- active render job appears in a compact progress notification or render status popover
- queued render jobs are listed when more than one exists
- completed render jobs are available in a recent render output list
- each completed item offers `Open Image`, `Open Output Folder`, and `Copy Path`
- failed render jobs show error state and link to `Log`
- render history persists for the current project
- stopping a render leaves a cancelled entry, not a silent disappearance

Acceptance criteria:

- render output is discoverable after completion
- long renders can be monitored without opening debug/profiling panels
- failures and cancellations are visible and actionable

### Step 9.5: Window menu

Window menu should open/close panels:

- Hierarchy
- Inspector
- Content
- Timeline
- Log
- Debug / Profile hidden advanced panel
- Render Settings
- Floating Render Controls
- Reset Layout
- Save Layout
- Load Layout

Target structure:

```text
Window
  PANELS
    Hierarchy
    Inspector
    Content
    Timeline
    Log
    Render Settings

  DEBUG / ADVANCED
    Debug / Profile
    Floating Render Controls

  LAYOUT
    Save Layout
    Reset Layout
    Load Layout...
```

Acceptance criteria:

- Scene/import/document controls are in menus, not permanently in `Content`.
- Advanced windows can be opened intentionally without cluttering the default layout.
- `Debug / Profile` is not part of the default bottom tab set; it appears only after the user opens it.

### Step 9.6: Layout menu

The `Layout` menu should own workspace presets and layout persistence.

Target structure:

```text
Layout
  Default Editor
  Content Editing
  Lighting
  Animation / Timeline
  Debug / Profiling
  Save Current Layout
  Reset To Default Layout
  Manage Layouts...
```

Acceptance criteria:

- Layout presets restore dock positions, panel visibility, and selected tabs.
- Debug/profiling layout can show the hidden `Debug / Profile` panel intentionally.
- Reset returns to the reference-match default layout.

## Phase 10: Right-Side Dock Details

### Step 10.1: Upper-right tabs

Target tabs:

```text
Hierarchy | Render Settings
```

Remove:

```text
Render World Settings
```

### Step 10.2: Lower-right tabs

Target tabs:

```text
Inspector
```

Optional future tabs:

- Properties
- Details

Only add these if needed. The reference uses `Inspector`.

### Step 10.3: Panel split

Right-side vertical split:

- upper: hierarchy/render settings
- lower: inspector

The split should keep enough height for a usable inspector when a camera/light is selected.

Acceptance criteria:

- Right side matches the reference arrangement.
- `Render Settings` is docked, not floating by default.
- Inspector has enough room to show component headers without excessive scrolling.

## Phase 11: Icon System

### Step 11.1: Define icon source

Choose one icon approach:

- existing embedded icon font if already present
- small texture atlas
- custom ImGui draw icons

Keep icons visually consistent.

### Step 11.2: Required icons

Minimum icon set:

- select
- move
- rotate
- scale
- world/local
- search
- add
- add dropdown arrow
- refresh
- grid
- list
- folder
- file
- texture
- HDR/EXR image
- model
- material
- scene
- project file
- IES profile
- VDB volume file
- shader file
- config/data file
- unsupported file fallback
- camera
- light
- hierarchy all/entities filter
- hierarchy settings/world filter
- hierarchy effects/post-process filter
- sun
- environment light
- sky atmosphere
- height fog
- volumetric cloud
- post process volume
- mesh cube
- volume
- imported scene root
- actor/group
- unknown entity fallback
- eye visible
- eye hidden
- eye inherited hidden
- eye disabled
- lock
- reset/revert
- play
- pause
- stop
- timeline key
- stats
- draw debug

### Step 11.3: Tooltips

Every icon-only button needs a tooltip.

Acceptance criteria:

- The UI no longer relies on cryptic text buttons for core tools.
- Icons are consistent between toolbar, hierarchy, content, and inspector.

Implementation status: `EditorUiStyle` now defines a custom ImGui draw-icon source for the viewport transform toolbar, viewport action strip, camera-speed readout, all hierarchy row/control icon paths, Content browser file/toolbar glyphs, Project Manager card glyphs, Inspector reset/action controls, Timeline transport/key/action controls, and top-menu command/toggle rows. Top menus reserve row-leading space and draw glyphs over it while preserving `ImGui::MenuItem` shortcuts, disabled rows, and toggle state.

## Phase 12: Visual Polish Pass

### Step 12.1: Reduce raw debug appearance

Remove or restyle:

- overly bright blue panel borders
- dense black-only panels
- permanent warning text in browser panels
- raw table headers where asset-browser rows are better
- tiny text buttons for primary tools

### Step 12.2: Match row spacing

Apply consistent row heights:

- hierarchy rows
- content folder rows
- content asset rows
- inspector property rows
- render settings property rows

### Step 12.3: Match selection blue

Use the same selected row color for:

- hierarchy selected entity
- content selected folder
- content selected asset
- active timeline key/range if relevant

### Step 12.4: Match disabled state

Hidden or disabled hierarchy objects should use:

- muted text
- muted icon
- hidden eye icon

### Step 12.5: Align property rows

Inspector and render settings should use:

```text
label column | value/control column
```

Vector rows should use:

```text
X field | Y field | Z field | reset icon
```

Acceptance criteria:

- The editor looks like one coherent product.
- Panels no longer look like independent debug widgets.
- The UI matches the reference density without becoming unreadable.

## Phase 13: Interaction Parity

### Step 13.1: Selection flow

Required flow:

1. Select object in hierarchy.
2. Row turns blue.
3. Viewport gizmo appears.
4. Inspector shows object-specific components.
5. Editing transform updates scene.
6. Visibility eye toggles object visibility.

### Step 13.1.1: Viewport selection behavior

Viewport selection should match editor expectations.

Required behavior:

- clicking an object in the viewport selects it
- selected object row highlights in hierarchy
- selected object shows transform/light/camera gizmo
- selected object can show outline, bounds, or selection marker depending on `View Settings`
- `F` or equivalent focuses/frames selected object
- delete removes selected object after confirmation when needed
- duplicate creates a copy and selects it
- multi-select works with Shift/Ctrl where supported
- marquee/box select may be added later, but behavior should be planned
- clicking empty viewport clears selection only if current tool mode allows it
- locked objects cannot be selected or edited unless unlocked

Acceptance criteria:

- hierarchy selection and viewport selection stay synchronized.
- selection state is obvious in both viewport and hierarchy.
- selected-object operations do not require raw debug panels.

### Step 13.1.2: Context menus

Context menus are required for editor parity.

Hierarchy row context menu:

- Rename
- Duplicate
- Delete
- Create Child
- Focus / Frame Selected
- Show / Hide
- Lock / Unlock
- Move To Group
- Convert To Prefab when supported
- Open Source Asset when available

Content folder context menu:

- New Folder
- Rename
- Delete
- Show In Explorer
- Import Here
- Refresh

Content asset context menu:

- Open
- Preview
- Import / Reimport
- Place In Scene when supported
- Show In Explorer
- Copy Path
- Delete

Viewport selection context menu:

- Focus Selected
- Duplicate
- Delete
- Create Here
- Place Asset Here when dragging/selected
- Reset Transform

Inspector component context menu:

- Reset Component
- Copy Component Values
- Paste Component Values
- Remove Component where allowed
- Collapse Others
- Pin Inspector

Acceptance criteria:

- right-click behavior exists for hierarchy, content, viewport, and inspector
- menu items are disabled with reasons when unavailable
- destructive actions require confirmation where appropriate

### Step 13.2: Asset placement flow

Required flow:

1. Open Content.
2. Select folder in tree.
3. See assets in middle pane.
4. Drag model/prefab into viewport.
5. New entity appears in hierarchy.
6. Entity becomes selected.
7. Inspector shows mesh/material components.

### Step 13.3: Render settings flow

Required flow:

1. Open right-side `Render Settings`.
2. Adjust environment/sun/sky/path tracing settings.
3. Viewport updates.
4. Reset accumulation is available.
5. Debug/profiling tools remain accessible.

### Step 13.4: Scene file flow

Required flow:

1. Use `File > Open Scene`.
2. Open dialog appears.
3. Current scene is replaced.
4. Scene title updates.
5. Hierarchy updates.
6. Content browser remains a browser, not a document form.

### Step 13.5: Project startup flow

Required flow:

1. Start the interactive application.
2. Project Manager appears before the full editor.
3. User selects `New Project`, `Open Project`, or a recent project.
4. New projects default to `%USERPROFILE%\Documents\Vibode Projects`.
5. Selected template creates the expected default scene/world actors.
6. Full editor opens only after a project is selected or created.

### Step 13.6: Template world actor flow

Required flow:

1. Create a project from `Outdoor / Atmosphere` or similar template.
2. Hierarchy includes Camera, Sun Light, Environment Light, Sky Atmosphere, Height Fog, Volumetric Cloud when supported, and Post Process Volume.
3. Every row has a type icon.
4. Selecting each actor shows a meaningful inspector component.
5. Editing each supported actor changes the viewport/render state.
6. Unsupported actor/features show a clear disabled/unsupported state.

Acceptance criteria:

- Common workflows are available without opening raw debug windows.
- The user can understand the scene, selected object, and render state from the layout.
- Startup project selection is clear before entering the full editor.
- Template-created lighting/atmosphere/world actors are functional or explicitly marked unsupported.

## Phase 14: Regression And Validation

If implementation touches renderer startup, CLI parsing, RenderGraph, profiler, debug views, validation scenes, RenderDoc capture, or diagnostic output code, read `docs/AI_DEBUG_PROFILING_TOOLS_USAGE.md` before coding.

### Step 14.0: UI/UX audit tools

Use the editor UI/UX audit tools in `scripts/` while implementing this plan.

Aggregate runner:

```powershell
.\scripts\editor_ui_ux_audit_all.ps1
```

The aggregate runner writes JSON reports under:

```text
out/editor_ui_ux_audits/
```

Individual tools:

```powershell
.\scripts\editor_ui_layout_dump.ps1
.\scripts\hierarchy_icon_audit.ps1 -Scene scenes\validation\cornell.rtlevel
.\scripts\inspector_coverage_audit.ps1 -Scene scenes\validation\cornell.rtlevel
.\scripts\content_browser_audit.ps1 -Project Projects\MyProject\MyProject.rtproject
.\scripts\command_menu_audit.ps1
.\scripts\project_manager_fixture_audit.ps1
.\scripts\render_workflow_audit.ps1
.\scripts\style_metrics_audit.ps1
```

Strict gate examples:

```powershell
.\scripts\hierarchy_icon_audit.ps1 -Scene scenes\validation\cornell.rtlevel -FailOnMissing
.\scripts\inspector_coverage_audit.ps1 -Scene scenes\validation\cornell.rtlevel -FailOnIssues
.\scripts\content_browser_audit.ps1 -Project Projects\MyProject\MyProject.rtproject -FailOnIssues
.\scripts\command_menu_audit.ps1 -FailOnIssues
.\scripts\project_manager_fixture_audit.ps1 -FailOnIssues
.\scripts\render_workflow_audit.ps1 -FailOnIssues
```

Screenshot comparison:

```powershell
.\scripts\editor_screenshot_regression.ps1 -Baseline refs\editor -Current out\editor_screens
```

Tool responsibilities:

- `editor_screenshot_regression.ps1`: compares reference/current screenshots or screenshot directories.
- `editor_ui_layout_dump.ps1`: exports docked windows, split ratios, visibility defaults, and layout readiness.
- `hierarchy_icon_audit.ps1`: checks scene hierarchy type/icon mapping and visibility readiness.
- `inspector_coverage_audit.ps1`: checks source coverage for required inspector components.
- `content_browser_audit.ps1`: checks content roots, source readiness, and file/asset icon mapping.
- `command_menu_audit.ps1`: checks command registry and top menu readiness.
- `project_manager_fixture_audit.ps1`: creates project-manager fixtures and validates default project-root assumptions.
- `render_workflow_audit.ps1`: checks explicit render workflow command readiness.
- `style_metrics_audit.ps1`: inventories UI style constants and metrics.

These tools are safe by default. They report readiness and write JSON. Use fail switches such as `-FailOnIssues`, `-FailOnMissing`, or `-FailOnDifference` when turning a check into a CI gate.

Current expected pre-implementation failures include:

- missing explicit render workflow commands
- incomplete camera post-process inspector coverage
- incomplete Content toolbar reference-match controls

These failures should disappear as the corresponding implementation phases are completed.

Tool-to-phase gates:

- Project Manager and `.vproject` work: run `project_manager_fixture_audit.ps1`.
- Default layout and hidden debug/profile work: run `editor_ui_layout_dump.ps1`.
- Hierarchy icons, nesting, selection, and eye buttons: run `hierarchy_icon_audit.ps1`.
- Inspector component coverage: run `inspector_coverage_audit.ps1`.
- Content browser layout, roots, `+` menu, file-type icons, and action routing: run `content_browser_audit.ps1`.
- File/Create/Engine/Window/Render/Layout menu coverage: run `command_menu_audit.ps1`.
- Render button/function, progress, output, and history/queue: run `render_workflow_audit.ps1`.
- Visual style constants, panel metrics, and spacing: run `style_metrics_audit.ps1`.
- Final reference-match pass: run `editor_ui_ux_audit_all.ps1` and `editor_screenshot_regression.ps1`.

### Step 14.1: Editor smoke checks

Verify:

- `.\scripts\editor_ui_ux_audit_all.ps1` runs and writes reports
- `.\scripts\editor_ui_layout_dump.ps1` reports the expected default layout
- Project Manager opens first in normal interactive startup
- new project default path is under `%USERPROFILE%\Documents\Vibode Projects`
- recent project selection opens the full editor
- project template grid works
- project name/path validation works
- project thumbnails or fallback icons display
- editor launches
- default layout loads
- layout restore works
- reset layout returns to default
- hierarchy visible
- inspector visible
- content browser visible
- timeline visible
- log visible
- unified render settings visible
- no floating render settings by default
- no content import/compatibility forms by default

### Step 14.2: Scene interaction checks

Verify:

- `.\scripts\hierarchy_icon_audit.ps1` reports no missing icon mappings for target scenes
- `.\scripts\inspector_coverage_audit.ps1` reports required inspector coverage
- selecting camera updates inspector
- selecting sun light updates inspector
- selecting environment light updates inspector
- selecting sky atmosphere updates inspector
- selecting height fog updates inspector
- selecting volumetric cloud updates inspector when supported
- selecting post process volume updates inspector
- selecting mesh updates inspector
- viewport click selection updates hierarchy and inspector
- focus/frame selected works
- duplicate/delete selected works
- context menus work for hierarchy, content, viewport, and inspector
- hierarchy search works
- visibility eye toggles work
- transform fields edit selected object
- viewport gizmo appears for selection
- every hierarchy row has an icon
- template-created lighting/atmosphere/world actors affect rendering or show unsupported state

### Step 14.3: Content checks

Verify:

- `.\scripts\content_browser_audit.ps1` reports file/asset icon mappings
- content browser opens without vertical scrolling
- folder tree works
- breadcrumb works
- asset list works
- every visible asset/file row has a type icon
- texture/material/model/scene previews or fallbacks work
- details panel works
- import/reimport progress and failure states work
- import actions still exist in menus
- scene open/save actions still exist in menus

### Step 14.4: Notification, command, and layout checks

Verify:

- `.\scripts\command_menu_audit.ps1` reports target menus and commands
- `.\scripts\style_metrics_audit.ps1` reports required style metrics
- save/import/render notifications appear
- errors link to `Log`
- command palette opens and runs commands
- shortcut conflicts are reported
- layout save/restore/reset works
- hidden `Debug / Profile` can be opened intentionally

### Step 14.5: Render settings checks

Verify:

- environment controls exist
- sky/atmosphere controls exist
- height fog controls exist when the actor is present
- volumetric cloud controls exist when the actor is present
- post process volume controls exist when the actor is present
- sun controls exist
- path tracing controls exist
- tone mapping controls exist
- denoiser/TAA controls exist
- ReSTIR controls exist
- debug/profiling controls exist
- reset accumulation works

### Step 14.6: Render output checks

Verify:

- `.\scripts\render_workflow_audit.ps1` reports explicit render workflow readiness
- Render current viewport works
- Render image modal works
- Render sequence uses timeline range
- render progress is visible
- stop/cancel render works
- completed render exposes `Open Output Folder`
- render failures are logged and notified

### Step 14.7: Required smoke command

After renderer, RenderGraph, profiler, CLI, or debug-view changes, preserve this command:

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

### Step 14.8: Screenshot reference checks

After major visual UI work, capture editor screenshots for the same reference states and compare them with:

```powershell
.\scripts\editor_screenshot_regression.ps1 -Baseline refs\editor -Current out\editor_screens
```

Reference states should include:

- Project Manager home/recent projects
- default editor layout with `Content` active
- hierarchy selection with Camera selected
- hierarchy selection with Sun Light selected
- Content browser folder with model/material/texture files
- Timeline active
- Render Settings active

Acceptance criteria:

- screenshot differences are reviewed intentionally
- false positives are documented
- accepted baseline updates are explicit, not accidental

## Implementation Order

Use this order to reduce churn:

1. Run `.\scripts\editor_ui_ux_audit_all.ps1` once to capture the current baseline reports under `out/editor_ui_ux_audits/`.
2. Add Project Manager startup flow and default project location, then run `project_manager_fixture_audit.ps1`.
3. Add Project Manager template grid, validation, and project thumbnails.
4. Add project templates and template-created world actor defaults.
5. Create shared style constants, then run `style_metrics_audit.ps1`.
6. Add command registry, shortcut map, notifications, and layout persistence, then run `command_menu_audit.ps1`.
7. Remove content import/compatibility/scene document forms.
8. Move scene/import actions into menus.
9. Complete File/Create/Engine/Window/Render/Layout menu specs.
10. Combine `Render World Settings` and `Render Settings`.
11. Disable default floating render settings window, then run `editor_ui_layout_dump.ps1`.
12. Rework hierarchy rows with icons, visibility, selection, and search, then run `hierarchy_icon_audit.ps1`.
13. Build selected-object component inspector, then run `inspector_coverage_audit.ps1`.
14. Bind lighting/atmosphere/world actors to real renderer settings or explicit unsupported states.
15. Upgrade content browser visual behavior.
16. Add asset file-type icons, previews, import/reimport progress, and file action routing, then run `content_browser_audit.ps1`.
17. Upgrade viewport toolbar and status strip.
18. Add viewport selection behavior, context menus, and gizmo polish.
19. Add render button/function, render output modal, and render history/queue, then run `render_workflow_audit.ps1`.
20. Upgrade timeline ruler and controls.
21. Apply final visual polish pass.
22. Run `.\scripts\editor_ui_ux_audit_all.ps1` and fix newly introduced audit failures.
23. Run strict targeted audits for each completed phase with `-FailOnIssues`, `-FailOnMissing`, or `-FailOnDifference`.
24. Run screenshot regression checks for completed visual phases.
25. Run editor smoke checks.
26. Run renderer/debug regression checks if renderer-adjacent code changed.

## Definition Of Done

The UI/UX match is done when:

- normal interactive startup opens Project Manager before the full editor
- new projects default to `%USERPROFILE%\Documents\Vibode Projects`
- recent projects can be selected before the full editor opens
- Project Manager template grid, project validation, and thumbnails work
- templates create the correct default project/scene-level actors
- default layout visually matches the reference screenshots
- layout save/restore/reset works
- command palette and shortcut map exist
- notification/status system covers save/import/render errors and completion
- `Content` is a clean asset browser with no import/compatibility/document forms
- `Content` does not need scrolling in the default state
- `Content` is split into three visible sections by default: folder tree, asset list, and details/preview
- every visible Content asset/file row has a type icon
- asset previews/thumbnails and import/reimport states are available through the details/preview/actions panel
- right-side upper tabs are `Hierarchy` and `Render Settings`
- `Render World Settings` no longer exists as a separate tab
- the floating render settings window is not open by default
- `Debug / Profile` is hidden from the default layout and available only through intentional advanced/debug commands
- selected objects drive the inspector
- viewport selection synchronizes with hierarchy and inspector
- hierarchy/content/viewport/inspector context menus exist
- camera inspector resembles the reference camera inspector
- light inspector resembles the reference light inspector
- every hierarchy row has an icon, plus nesting, selection, and visibility eyes
- Environment Light, Sky Atmosphere, Height Fog, Volumetric Cloud, and Post Process Volume are functional where supported or clearly marked unsupported
- viewport toolbar is icon-first
- current debug/profile data and functions are routed into `View Settings`, `Stats`, or `Draw Debug` based on type
- Render menu has clear render commands, progress, output actions, and render history/queue
- timeline has transport controls and a visual frame ruler
- top menus own scene, import, render, layout, window, engine, and create actions
- debug/profiling workflows remain available
- UI/UX audit scripts are documented in `scripts/README.md`
- `.\scripts\editor_ui_ux_audit_all.ps1` runs and all relevant implemented-phase checks pass
- targeted audit tools pass in strict mode for their completed feature areas
- screenshot regression is available for reference-match visual checks
- required headless smoke command still works when renderer-adjacent code is touched
