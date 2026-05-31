# Editor Level Editor Tooling

These scripts support `docs/EDITOR_LEVEL_EDITOR_IMPLEMENTATION_PLAN.md`. They are intentionally small and composable so each implementation step can add stricter checks without changing the renderer.

## Static audits

```powershell
.\scripts\editor_plan_audit.ps1
.\scripts\imgui_panel_audit.ps1
.\scripts\editor_request_flow_report.ps1
.\scripts\editor_state_snapshot.ps1 -Scene scenes\validation\cornell.rtlevel
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
