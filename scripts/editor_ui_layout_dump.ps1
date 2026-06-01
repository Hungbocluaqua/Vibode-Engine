param(
    [string]$RepoRoot,
    [string]$IniPath = 'rtv_editor.ini',
    [string]$JsonOut = 'out\editor_ui_layout_dump.json'
)

Import-Module (Join-Path $PSScriptRoot 'EditorToolingCommon.psm1') -Force
$RepoRoot = Resolve-EditorToolRepoRoot $RepoRoot

$dockspacePath = Join-Path $RepoRoot 'src\rtv\EditorDockspace.cpp'
$panelsPath = Join-Path $RepoRoot 'include\rtv\EditorPanels.h'
$dockspace = Get-Content -Raw -LiteralPath $dockspacePath
$panels = Get-Content -Raw -LiteralPath $panelsPath

$dockedWindows = @()
foreach ($m in [regex]::Matches($dockspace, 'DockBuilderDockWindow\("([^"]+)"\s*,\s*([^)]+)\)')) {
    $dockedWindows += [pscustomobject]@{ label=$m.Groups[1].Value; target=$m.Groups[2].Value.Trim() }
}

$splitNodes = @()
foreach ($m in [regex]::Matches($dockspace, 'DockBuilderSplitNode\([^,]+,\s*(ImGuiDir_[A-Za-z]+),\s*([0-9.]+)f')) {
    $splitNodes += [pscustomobject]@{ direction=$m.Groups[1].Value; ratio=[double]$m.Groups[2].Value }
}

$visibilityDefaults = @()
foreach ($m in [regex]::Matches($panels, 'bool\s+([A-Za-z0-9_]+)\s*=\s*(true|false)')) {
    $visibilityDefaults += [pscustomobject]@{ panel=$m.Groups[1].Value; visible=($m.Groups[2].Value -eq 'true') }
}

$iniResolved = if ([System.IO.Path]::IsPathRooted($IniPath)) { $IniPath } else { Join-Path $RepoRoot $IniPath }
$iniWindows = @()
if (Test-Path -LiteralPath $iniResolved) {
    foreach ($m in [regex]::Matches((Get-Content -Raw -LiteralPath $iniResolved), '\[Window\]\[([^\]]+)\]')) {
        $iniWindows += $m.Groups[1].Value
    }
}

$targetDefaultBottom = @('Content','Timeline','Log')
$hiddenByDefault = @('Debug / Profiler','Debug / Profile')
$dump = [ordered]@{
    generatedAt = (Get-Date).ToString('o')
    repoRoot = $RepoRoot
    source = [ordered]@{
        dockedWindows = $dockedWindows
        splitNodes = $splitNodes
        visibilityDefaults = $visibilityDefaults
    }
    ini = [ordered]@{
        path = $iniResolved
        exists = (Test-Path -LiteralPath $iniResolved)
        windows = @($iniWindows | Select-Object -Unique)
    }
    readiness = [ordered]@{
        hasScene = @($dockedWindows | Where-Object label -eq 'Scene').Count -gt 0
        hasHierarchy = @($dockedWindows | Where-Object label -eq 'Hierarchy').Count -gt 0
        hasInspector = @($dockedWindows | Where-Object label -eq 'Inspector').Count -gt 0
        hasContentTimelineLog = @($targetDefaultBottom | Where-Object { $label = $_; @($dockedWindows | Where-Object label -eq $label).Count -eq 0 }).Count -eq 0
        debugProfilerHiddenByDefault = @($visibilityDefaults | Where-Object { $_.panel -eq 'debugProfiler' -and -not $_.visible }).Count -gt 0
        noRenderWorldSettingsTab = @($dockedWindows | Where-Object label -eq 'Render World Settings').Count -eq 0
    }
}

$out = if ([System.IO.Path]::IsPathRooted($JsonOut)) { $JsonOut } else { Join-Path $RepoRoot $JsonOut }
New-Item -ItemType Directory -Force (Split-Path -Parent $out) | Out-Null
$dump | ConvertTo-Json -Depth 16 | Set-Content -LiteralPath $out -Encoding UTF8
Write-Host "Wrote editor UI layout dump: $out"
