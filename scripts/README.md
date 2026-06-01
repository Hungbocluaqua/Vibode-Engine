# Editor Level Editor Tooling

These scripts support `docs/EDITOR_LEVEL_EDITOR_IMPLEMENTATION_PLAN.md`. They are intentionally small and composable so each implementation step can add stricter checks without changing the renderer.

## Static audits

```powershell
.\scripts\editor_plan_audit.ps1
.\scripts\imgui_panel_audit.ps1
.\scripts\editor_request_flow_report.ps1
.\scripts\editor_state_snapshot.ps1 -Scene scenes\validation\cornell.rtlevel
.\scripts\editor_ui_ux_audit_all.ps1
```

## UI/UX reference-match audits

These tools support `docs/EDITOR_UI_UX_REFERENCE_MATCH_PLAN.md`. They are safe by default: they report readiness and write JSON, but only fail the process when a `-FailOnIssues`, `-FailOnMissing`, or `-FailOnDifference` switch is used.

```powershell
.\scripts\editor_ui_layout_dump.ps1
.\scripts\hierarchy_icon_audit.ps1 -Scene scenes\validation\cornell.rtlevel
.\scripts\inspector_coverage_audit.ps1 -Scene scenes\validation\cornell.rtlevel
.\scripts\content_browser_audit.ps1 -Project Projects\MyProject\MyProject.rtproject
.\scripts\interaction_parity_audit.ps1
.\scripts\command_menu_audit.ps1
.\scripts\project_manager_fixture_audit.ps1
.\scripts\render_workflow_audit.ps1
.\scripts\style_metrics_audit.ps1
```

Screenshot comparison can compare one image pair or matching files under two directories:

```powershell
.\scripts\editor_screenshot_regression.ps1 -Baseline refs\editor -Current out\editor_screens
.\scripts\capture_editor_reference_scene_states.ps1 -OutDir out\editor_ui_ux_audits\reference_scenes
.\scripts\editor_screenshot_regression.ps1 -Baseline refs\editor_reference_scenes -Current out\editor_ui_ux_audits\reference_scenes -RequireReferenceSceneSet -FailOnDifference
```

Use `-WindowWidth` and `-WindowHeight` on the capture scripts when checking against the supplied full-size reference screenshots. Leaving them at `0` preserves the default editor-window size used by the committed baselines.

The aggregate runner writes per-tool JSON under `out\editor_ui_ux_audits`:

```powershell
.\scripts\editor_ui_ux_audit_all.ps1
```

## Validation and smoke

```powershell
.\scripts\rtlevel_schema_validator.ps1 -Path scenes\validation -FailOnInvalid
.\scripts\rtlevel_compat_test.ps1 -Path scenes\validation
.\scripts\editor_smoke.ps1 -BuildDebug -BuildRelease
```

Use `-RunHeadless` with `rtlevel_compat_test.ps1` to launch `rtvulkan.exe` for each scene.

## Project and asset fixtures

```powershell
.\scripts\project_fixture_generator.ps1 -Name ToolingSmoke -Template PathTracedDefault -Force
.\scripts\asset_registry_validator.ps1 -RegistryPath out\project_fixtures\ToolingSmoke\Content\AssetRegistry.json
```

## Import and migration probes

```powershell
.\scripts\scene_entity_count_probe.ps1 -Before scenes\validation\cornell.rtlevel
.\scripts\import_regression_harness.ps1
.\scripts\migration_backup_test.ps1 -Scene scenes\validation\cornell.rtlevel
```

`import_regression_harness.ps1` is a readiness harness until the editor exposes real `Import Asset` requests. Use `-ExpectImplemented` once that step lands.
