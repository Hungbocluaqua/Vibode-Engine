param(
    [string]$RepoRoot,
    [string]$Exe = 'build\Debug\rtvulkan.exe',
    [string]$Project = 'Samples\LightweightSponza\LightweightSponza.vproject',
    [string]$OutDir = 'out\editor_ui_ux_audits\reference_scenes',
    [int]$WaitSeconds = 25,
    [int]$CaptureDelaySeconds = 10,
    [int]$WindowWidth = 0,
    [int]$WindowHeight = 0,
    [switch]$FailOnIssues
)

Import-Module (Join-Path $PSScriptRoot 'EditorToolingCommon.psm1') -Force
$RepoRoot = Resolve-EditorToolRepoRoot $RepoRoot

function Resolve-StatePath {
    param([string]$Path)
    if ([System.IO.Path]::IsPathRooted($Path)) { return $Path }
    return Join-Path $RepoRoot $Path
}

$resolvedOutDir = Resolve-StatePath $OutDir
New-Item -ItemType Directory -Force $resolvedOutDir | Out-Null

$states = @(
    [pscustomobject]@{ name='sponza-default'; row=-1; focusWindow=''; file='sponza_default.png' },
    [pscustomobject]@{ name='sponza-content-active'; row=-1; focusWindow='Content'; file='sponza_content_active.png' },
    [pscustomobject]@{ name='sponza-timeline-active'; row=-1; focusWindow='Timeline'; file='sponza_timeline_active.png' },
    [pscustomobject]@{ name='sponza-selected-sun'; row=2; focusWindow=''; file='sponza_selected_sun.png' }
)

$results = @()
foreach ($state in $states) {
    $imagePath = Join-Path $resolvedOutDir $state.file
    $jsonPath = Join-Path $resolvedOutDir ($state.name + '.json')
    $captureParams = @{
        RepoRoot = $RepoRoot
        Exe = $Exe
        Project = $Project
        Out = $imagePath
        JsonOut = $jsonPath
        WaitSeconds = $WaitSeconds
        CaptureDelaySeconds = $CaptureDelaySeconds
        WindowWidth = $WindowWidth
        WindowHeight = $WindowHeight
        StateName = $state.name
        UseProjectStartup = $true
        FailOnIssues = $true
    }
    if ($state.row -ge 0) {
        $captureParams.SelectHierarchyRow = $state.row
    }
    if (![string]::IsNullOrWhiteSpace($state.focusWindow)) {
        $captureParams.FocusWindow = $state.focusWindow
    }

    & (Join-Path $PSScriptRoot 'capture_editor_screenshot.ps1') @captureParams
    $passed = (Test-Path -LiteralPath $imagePath) -and (Test-Path -LiteralPath $jsonPath)
    $results += New-ToolResult -Name ("Capture reference scene state: {0}" -f $state.name) -Passed $passed -Message $imagePath -Details ([pscustomobject]@{
        state = $state.name
        selectHierarchyRow = $state.row
        requestedWindowWidth = $WindowWidth
        requestedWindowHeight = $WindowHeight
        focusWindow = $state.focusWindow
        image = $imagePath
        json = $jsonPath
    })
}

$summaryPath = Join-Path $resolvedOutDir 'summary.json'
$passedAll = Write-ToolResults -Results $results -JsonOut $summaryPath
if ($FailOnIssues -and -not $passedAll) { exit 1 }
