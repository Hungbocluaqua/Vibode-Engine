# Editor Reference Screenshot Inspection

## Purpose

This document records observations from four editor reference screenshots. Use it as visual and workflow input for `docs/EDITOR_LEVEL_EDITOR_IMPLEMENTATION_PLAN.md` and `docs/LEVEL_EDITOR_FEATURE_CHECKLIST.md`.

The screenshots show a dark Minitech/Unreal-style editor shell with a path-traced `Scene` viewport, right-side hierarchy and inspector panels, and a bottom content/timeline/log area. They are useful because they show the intended density, panel naming, menu organization, property editing style, and content browser behavior in concrete examples.

## Source Images

- `C:\Users\HomePc\Pictures\Screenshots\Ảnh chụp màn hình 2026-05-31 001413.png`
- `C:\Users\HomePc\Pictures\Screenshots\Ảnh chụp màn hình 2026-05-31 003505.png`
- `C:\Users\HomePc\Pictures\Screenshots\Ảnh chụp màn hình 2026-05-31 001611.png`
- `C:\Users\HomePc\Pictures\Screenshots\Ảnh chụp màn hình 2026-05-31 001548.png`

## Shared Layout Observations

- The editor uses a very dark, compact docked shell with thin separators, flat panels, small tabs, and blue selection highlights.
- The top menu bar uses short menus: `File`, `Create`, `Engine`, `Window`, `Render`, and `Layout`. One screenshot orders `Render` before `Window`; the implementation plan should keep the canonical order `File | Create | Engine | Window | Render | Layout` unless there is a deliberate UX reason to reorder it.
- The active scene name appears in the top bar between vertical separators, for example `test-ccccccccccccscc1`, `Untitled Scene`, and `sponza-intel`.
- A compact performance readout appears at the top-right of the window, showing `fps` and frame time in milliseconds.
- The central dock is a `Scene` viewport tab, not `Viewport`.
- The `Scene` viewport has a compact top toolbar with transform/navigation icons and a separate compact status/action strip containing path tracing progress, `View Settings`, `Stats`, `Draw Debug`, and a camera-speed or numeric control.
- The right side is split vertically: upper tabs are `Hierarchy` and `Render World Settings`; lower tab is `Inspector`.
- The bottom dock uses tabs named `Content`, `Timeline`, and `Log`.
- The visual style favors dense rows, low vertical padding, icon-first controls, and subtle contrast between panel backgrounds.
- Blue highlights indicate selected hierarchy rows, selected content folders, and active UI state.

## Screenshot 001413

File: `Ảnh chụp màn hình 2026-05-31 001413.png`

### Visible Scene

- The viewport shows a dark volumetric or VDB-like effect with bright cyan, orange, red, and yellow emission/refraction over a reflective floor.
- A selected light gizmo is visible in the upper-right of the rendered scene.
- The scene demonstrates that the editor needs to support high-dynamic-range, path-traced preview content without the UI overwhelming the image.

### Menu And Viewport

- Top menu order shown: `File`, `Create`, `Engine`, `Render`, `Layout`, `Window`.
- Scene title shown in the title/menu strip: `test-ccccccccccccscc1`.
- Top-right window readout shows approximately `fps: 45 | Ms: 19`.
- Viewport tab is `Scene` with a close button.
- Viewport toolbar contains selection/transform/navigation icons.
- Viewport status strip includes `pt 1/256 18.563`, `View Settings`, `Stats`, `Draw Debug`, and a numeric value around `1.000`.

### Hierarchy

- The `Hierarchy` tab is active beside `Render World Settings`.
- A row of component/filter icons appears below the hierarchy tabs.
- Search field placeholder is `Search...`.
- Entity list includes `Camera`, `Environment Light`, `Floor`, `volumeHet`, `avent`, `sphere`, and `AreaQuad Light`.
- Visibility eye icons appear on the right side of hierarchy rows.
- Some rows have disclosure triangles for nested content.
- `AreaQuad Light` is selected with a blue row highlight.

### Inspector

- Inspector header shows the selected entity name `AreaQuad Light` with a `Name` field area.
- Transform section is expanded with `Position`, `Rotation`, `Scale`, and `Show World Matrix`.
- Numeric fields are compact and arranged in three columns for vector values.
- Reset/revert icons appear beside editable transform rows.
- Light section is expanded.
- Light type appears as `Area Disk`.
- Light units appear as `LuminousPower (lumen)`.
- Light controls include IES profile selection, color temperature toggle, RGB color fields, light color swatch, light intensity, exposure multiplier, cone angle, cone penumbra, radius, visible-to-camera toggle, material source selection, save material button, cast surface shadows, and cast volumetric shadows.

### Bottom Dock

- Tabs are `Content`, `Timeline`, and `Log`.
- `Timeline` appears active.
- Timeline has transport/frame controls, `Frame`, `Start`, `End`, and a ruler with frame ticks.
- The timeline range appears to span `0 - 270`.

### Implementation Implications

- Area light inspector support should include light shape/type, physical units, color temperature, IES profile, material source, visibility, and shadow toggles.
- The bottom `Timeline` can be implemented as a functional shell early, but it should match the dense ruler/transport layout from the screenshot.
- The selected light gizmo confirms that light entities should have visible viewport handles/icons.

## Screenshot 003505

File: `Ảnh chụp màn hình 2026-05-31 003505.png`

### Visible Scene

- The viewport shows a large outdoor rocky canyon or plateau scene with distant terrain, a small settlement, and water near the bottom.
- The rendered view is bright and path traced, with visible progressive sampling noise.
- A transform gizmo is visible near the upper center of the viewport.

### Menu And Viewport

- Top menu order shown: `File`, `Create`, `Engine`, `Window`, `Render`, `Layout`.
- Scene title is `Untitled Scene`.
- Top-right readout shows approximately `fps: 67 | Ms: 27`.
- Viewport status strip includes `pt 227/256 15.160`, `View Settings`, `Stats`, `Draw Debug`, and a numeric value around `33.000`.

### Hierarchy

- The hierarchy contains top-level items such as `Camera`, `Environment Light`, `Floor`, multiple `test-volhom-planetextpreesMb...` objects, and `test-quixel-canyon55`.
- `test-quixel-canyon55` is expanded and contains `Environment Light`, `Camera`, `Sun Light`, `Actor`, `scene`, and `BatMobile`.
- Multiple `exterior1` entries appear below, each with blue cube-like icons.
- `Sun Light` is selected.
- Visibility eye icons are present for each row.

### Inspector

- Inspector shows `Sun Light` selected.
- Transform section is expanded with position, rotation, scale, and world matrix toggle.
- Only transform fields are visible in the captured lower panel area; the light section may be below the fold.

### Content Browser

- `Content` tab is active.
- Header includes add/import button, search/filter field, navigation buttons, and breadcrumb path.
- Breadcrumb path reads approximately `C: > Source > 1 > minitech > Engine > Content > bistro`.
- Left pane is a folder tree with folders such as `alfa_romeo_33_stradale_wwc`, `AnimatedModels`, `arch1`, `Batwing`, `bigsphere`, and `bistro`.
- Selected folder `bistro` is highlighted blue.
- Middle pane lists subfolders `Materials`, `Objects`, `Textures` and files `exterior1.mtl`, `exterior1.obj`, `interior.mtl`, `interior.obj`.
- Right/details pane says `No supported files selected`.

### Implementation Implications

- The content browser should use a three-pane layout: folder tree, file/asset list, and details/preview panel.
- Breadcrumb navigation is important and should stay compact.
- Hierarchy must support deep imported scene roots and repeated asset/entity names without losing selection clarity.
- `Sun Light` should be editable as a first-class scene entity even when it came from an imported scene root.

## Screenshot 001611

File: `Ảnh chụp màn hình 2026-05-31 001611.png`

### Visible Scene

- The viewport shows a forested canyon scene with trees, rocky terrain, and a small roofed structure.
- The camera appears positioned above the scene looking down.
- The lighting is bright daylight with foliage detail and strong terrain shadows.

### Menu And Viewport

- Top menu order shown: `File`, `Create`, `Engine`, `Window`, `Render`, `Layout`.
- Scene title is `Untitled Scene`.
- Top-right readout shows approximately `fps: 42 | Ms: 24`.
- Viewport status strip includes `pt 82/256 17.372`, `View Settings`, `Stats`, `Draw Debug`, and a numeric value around `33.000`.

### Hierarchy

- Hierarchy includes `Camera`, `Environment Light`, `Floor`, and a collapsed imported object named approximately `test-volhom-planetextpreesMb`.
- This screenshot has a simpler hierarchy state than the previous canyon shot.

### Inspector

- `Camera` is selected.
- Transform section is expanded.
- Camera section is expanded with camera and exposure/post-process controls.
- Visible camera controls include near clip, exposure mode, aperture size, ISO, shutter speed, film size, and focal length.
- Exposure mode shows `Manual (SBS)`.
- Post-process groups include `DOF`, `Bloom`, `Color correction`, `Vignetting`, and `Film grain`.
- There are enable checkboxes for motion blur, DOF, bloom, and film grain.
- Numeric sliders and compact input rows are used for camera/post-process values.

### Content Browser

- `Content` tab is active.
- Breadcrumb path reads approximately `C: > Source > 1 > minitech > Engine > Scenes`.
- Left pane folder tree shows `minitech`, `.git`, `Binaries`, `Development`, `Engine`, and engine subfolders like `Config`, `Content`, `Fonts`, `MaterialX`, and `Scenes`.
- Selected folder is `Scenes`.
- Middle pane lists multiple `.mscene` files such as `test-volhom-planetexptrees1.mscene`, `test-volume-hetperf.mscene`, and related variants.
- Details pane says `No supported files selected`.

### Implementation Implications

- Camera inspector should include exposure and lens controls, not only transform and FOV.
- Camera/post-process controls need grouped collapsible sections with enable checkboxes.
- The content browser should handle scene files as first-class browser entries and should clearly distinguish supported from unsupported selections.
- The plan's `CameraPostProcessComponent` and `GlobalPostProcessSettings` sections should use this screenshot as a reference for UI grouping.

## Screenshot 001548

File: `Ảnh chụp màn hình 2026-05-31 001548.png`

### Visible Scene

- The viewport shows an interior Sponza-like corridor with stone columns, walls, tiled floor, bright sun patches, and a glossy bust/statue in the foreground.
- The scene demonstrates high-contrast path-traced lighting, glossy reflection, and detailed imported architecture.

### Menu And Viewport

- Top menu order shown: `File`, `Create`, `Engine`, `Window`, `Render`, `Layout`.
- Scene title is `sponza-intel`.
- Top-right readout shows approximately `fps: 28 | Ms: 37`.
- Viewport status strip includes `pt 219/256 27.890`, `View Settings`, `Stats`, `Draw Debug`, and a numeric value around `1.000`.

### Hierarchy

- Hierarchy includes `Camera`, `Environment Light`, `NewSponza_Curtains_glTF`, `WorldRadianceCache`, `NewSponza_IvyGrowth_glTF`, `Sun Light`, `NewSponza_Main_glTF_002`, and `ajax`.
- `Sun Light` appears selected in the Inspector, while `ajax` appears highlighted blue in the hierarchy. This may indicate a selection mismatch in the captured frame, a hover highlight, or stale inspector selection.
- Multiple imported glTF roots are visible, indicating the hierarchy must remain readable with large scene imports.

### Inspector

- Inspector header shows `Sun Light`.
- Transform section is expanded.
- Light section is expanded.
- Light type is `Sun`.
- Light units are `Illuminance (lux)`.
- Controls include azimuth, elevation, color temperature toggle, temperature, light intensity in lux, exposure multiplier, softness, cast surface shadows, cast volumetric shadows, number of shadow bounces, and number of volumetric shadow bounces.
- Sun light controls are more specialized than the generic/area light controls in screenshot `001413`.

### Content Browser

- `Content` tab is active.
- Breadcrumb path reads approximately `C: > Source > 1 > minitech > Engine > Content > intel_sponza > Main_1_Sponza > textures`.
- Left pane folder tree contains asset folders such as `Head`, `headTen24`, `IES`, `intel_sponza`, `lamborghini-aventador-pbribl-obj`, `Lego`, and `lego_bricks`.
- Selected folder is `textures` under `intel_sponza > Main_1_Sponza`.
- Middle pane lists texture files such as base color, normal, roughness, and metalness PNGs.
- Details pane says `No supported files selected`.

### Implementation Implications

- Sun light inspector needs a specialized physical-light UI with azimuth/elevation, lux units, color temperature, softness, and bounce controls.
- Imported glTF roots should remain distinguishable in hierarchy and should support visibility toggles.
- Texture browsing needs readable file rows with texture-type icons and should eventually support texture previews/details.
- Selection state should be audited so the hierarchy selected row and inspector entity cannot diverge unless there is a deliberate hover/active-edit distinction.

## Consolidated UI Requirements From The Screenshots

### Shell And Docking

- Use a compact dark theme with low padding, small tabs, restrained borders, and blue selection highlights.
- Default docking should be center `Scene`, right `Hierarchy` and `Render World Settings`, lower-right `Inspector`, and bottom `Content`, `Timeline`, and `Log`.
- Keep the scene name visible in the top bar, with dirty-state marker support.
- Keep global performance readout visible at the top-right.

### Viewport

- The viewport tab label should be `Scene`.
- The viewport should include compact transform/navigation controls at the top-left.
- The viewport should include a compact status/action strip with path tracing progress, `View Settings`, `Stats`, `Draw Debug`, and camera speed or equivalent numeric state.
- Selection gizmos for lights and transforms must remain visible on top of rendered content.
- Path-traced progressive sample status should remain visible but not dominate the viewport.

### Hierarchy

- Use dense rows with icons, disclosure arrows, search, and visibility eye toggles.
- Imported scene roots and repeated imported object names must remain readable.
- Selection should be blue and unambiguous.
- Hierarchy state should stay synchronized with Inspector state.

### Inspector

- Use collapsible component sections with dense property rows.
- Transform rows should use three compact numeric columns and reset/revert controls.
- Light inspectors should be component-specific.
- Area lights need shape/type, lumen units, IES profile, radius/cone controls, material source, visibility, and shadow toggles.
- Sun lights need sun type, lux units, azimuth/elevation, color temperature, intensity, softness, and shadow bounce controls.
- Camera inspector should include physical camera controls, exposure mode, ISO, shutter speed, film size, focal length, near clip, and post-process groups.
- Post-process controls should be grouped into collapsible sections such as DOF, Bloom, Color correction, Vignetting, and Film grain.

### Content Browser

- Use a multi-pane layout with folder tree, file/asset list, and details/preview panel.
- Include add/import controls, filter field, back/forward navigation, refresh, and breadcrumb path.
- Support folders, scene files, OBJ/MTL files, and texture files in the browser.
- Details pane should clearly indicate unsupported or unselected assets.
- Project-relative paths should eventually replace absolute local engine paths for project assets, but the breadcrumb behavior shown here is the desired interaction model.

### Timeline And Log

- `Timeline` should exist as a bottom tab with compact transport/frame controls and a ruler.
- `Log` should exist as a bottom tab even when not active.
- The timeline can initially be a shell, but its layout should reserve space for frame range, scrubber, and keyframe controls.

## Plan Updates Suggested By These References

- Keep the canonical menu order as `File | Create | Engine | Window | Render | Layout`, even though one screenshot shows `Render` before `Window`.
- Add a selection-state validation item to ensure `Hierarchy`, viewport selection, and `Inspector` active entity do not drift unexpectedly.
- Make camera/post-process inspector work part of the lighting/post-process milestone, not an afterthought.
- Make sun light and area light inspector layouts separate component-specific UIs rather than one generic light editor.
- Prioritize the three-pane Content browser because it appears in three of the four screenshots and is central to the target workflow.
- Preserve the compact path tracing progress/status strip in the viewport during UI redesign.
- Treat imported scene roots as first-class hierarchy groups with visibility toggles and stable names.

## Validation Notes

When implementing UI to match these references, run:

```powershell
.\scripts\imgui_panel_audit.ps1 -JsonOut out\editor_tools\reference_imgui_audit.json
.\scripts\editor_state_snapshot.ps1 `
  -Scene scenes\validation\cornell.rtlevel `
  -JsonOut out\editor_tools\reference_state_snapshot.json
```

For Content browser and import-related work, also run:

```powershell
.\scripts\editor_plan_audit.ps1 -JsonOut out\editor_tools\reference_plan_audit.json
.\scripts\scene_entity_count_probe.ps1 `
  -Before scenes\validation\cornell.rtlevel `
  -JsonOut out\editor_tools\reference_entity_count.json
```
