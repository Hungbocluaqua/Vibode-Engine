param(
    [string]$RepoRoot,
    [string]$Scene = 'scenes\validation\cornell.rtlevel',
    [string]$Project = 'Projects\MyProject\MyProject.vproject',
    [string]$OutDir = 'out\editor_ui_ux_audits',
    [switch]$FailOnIssues
)

Import-Module (Join-Path $PSScriptRoot 'EditorToolingCommon.psm1') -Force
$RepoRoot = Resolve-EditorToolRepoRoot $RepoRoot
$out = if ([System.IO.Path]::IsPathRooted($OutDir)) { $OutDir } else { Join-Path $RepoRoot $OutDir }
New-Item -ItemType Directory -Force $out | Out-Null

$tools = @(
    @{ name='layout'; script='editor_ui_layout_dump.ps1'; json=(Join-Path $out 'layout.json'); params=@{ RepoRoot=$RepoRoot; JsonOut=(Join-Path $out 'layout.json') } },
    @{ name='hierarchy'; script='hierarchy_icon_audit.ps1'; json=(Join-Path $out 'hierarchy.json'); params=@{ RepoRoot=$RepoRoot; Scene=$Scene; JsonOut=(Join-Path $out 'hierarchy.json') } },
    @{ name='inspector'; script='inspector_coverage_audit.ps1'; json=(Join-Path $out 'inspector.json'); params=@{ RepoRoot=$RepoRoot; Scene=$Scene; JsonOut=(Join-Path $out 'inspector.json') } },
    @{ name='content'; script='content_browser_audit.ps1'; json=(Join-Path $out 'content.json'); params=@{ RepoRoot=$RepoRoot; Project=$Project; JsonOut=(Join-Path $out 'content.json') } },
    @{ name='interactionParity'; script='interaction_parity_audit.ps1'; json=(Join-Path $out 'interaction_parity.json'); params=@{ RepoRoot=$RepoRoot; JsonOut=(Join-Path $out 'interaction_parity.json') } },
    @{ name='timeline'; script='timeline_interaction_audit.ps1'; json=(Join-Path $out 'timeline.json'); params=@{ RepoRoot=$RepoRoot; JsonOut=(Join-Path $out 'timeline.json') } },
    @{ name='commands'; script='command_menu_audit.ps1'; json=(Join-Path $out 'commands.json'); params=@{ RepoRoot=$RepoRoot; JsonOut=(Join-Path $out 'commands.json') } },
    @{ name='projectManager'; script='project_manager_fixture_audit.ps1'; json=(Join-Path $out 'project_manager.json'); params=@{ RepoRoot=$RepoRoot; JsonOut=(Join-Path $out 'project_manager.json') } },
    @{ name='renderWorkflow'; script='render_workflow_audit.ps1'; json=(Join-Path $out 'render_workflow.json'); params=@{ RepoRoot=$RepoRoot; JsonOut=(Join-Path $out 'render_workflow.json') } },
    @{ name='styleMetrics'; script='style_metrics_audit.ps1'; json=(Join-Path $out 'style_metrics.json'); params=@{ RepoRoot=$RepoRoot; JsonOut=(Join-Path $out 'style_metrics.json') } },
    @{ name='screenshotRegression'; script='editor_screenshot_regression.ps1'; json=(Join-Path $out 'screenshot_regression.json'); params=@{ Baseline=(Join-Path $RepoRoot 'docs'); JsonOut=(Join-Path $out 'screenshot_regression.json'); InventoryOnly=$true } },
    @{ name='editorStateBaselines'; script='editor_screenshot_regression.ps1'; json=(Join-Path $out 'editor_state_baselines.json'); params=@{ Baseline=(Join-Path $RepoRoot 'refs\editor'); JsonOut=(Join-Path $out 'editor_state_baselines.json'); InventoryOnly=$true; RequireEditorStateSet=$true } },
    @{ name='referenceSceneBaselines'; script='editor_screenshot_regression.ps1'; json=(Join-Path $out 'reference_scene_baselines.json'); params=@{ Baseline=(Join-Path $RepoRoot 'refs\editor_reference_scenes'); JsonOut=(Join-Path $out 'reference_scene_baselines.json'); InventoryOnly=$true; RequireReferenceSceneSet=$true } }
)

$summary = @()
foreach ($tool in $tools) {
    $scriptPath = Join-Path $PSScriptRoot $tool['script']
    Write-Host "Running $($tool['name'])..."
    [hashtable]$toolParams = $tool['params']
    & $scriptPath @toolParams
    $exit = $LASTEXITCODE
    if ($null -eq $exit) { $exit = 0 }
    $summary += [pscustomobject]@{ name=$tool['name']; script=$tool['script']; exitCode=$exit; json=$tool['json'] }
}

$summaryPath = Join-Path $out 'summary.json'
$summary | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $summaryPath -Encoding UTF8
Write-Host "Wrote editor UI/UX audit summary: $summaryPath"

if ($FailOnIssues -and @($summary | Where-Object { $_.exitCode -ne 0 }).Count -gt 0) { exit 1 }
