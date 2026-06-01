param(
    [string]$RepoRoot,
    [string]$Exe = 'build\Debug\rtvulkan.exe',
    [string]$Project = 'Samples\CornellValidation\CornellValidation.vproject',
    [string]$OutDir = 'out\editor_ui_ux_audits\states',
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
    [pscustomobject]@{ name='project-manager-home'; row=-1; clickX=-1; clickY=-1; focusWindow=''; projectManager=$true; file='project_manager_home.png' },
    [pscustomobject]@{ name='default'; row=-1; clickX=-1; clickY=-1; focusWindow='Content'; projectManager=$false; file='default_editor.png' },
    [pscustomobject]@{ name='content-active'; row=-1; clickX=-1; clickY=-1; focusWindow='Content'; projectManager=$false; file='content_active.png' },
    [pscustomobject]@{ name='timeline-active'; row=-1; clickX=-1; clickY=-1; focusWindow='Timeline'; projectManager=$false; file='timeline_active.png' },
    [pscustomobject]@{ name='render-settings-active'; row=-1; clickX=-1; clickY=-1; focusWindow='Render Settings'; projectManager=$false; file='render_settings_active.png' },
    [pscustomobject]@{ name='selected-camera'; row=0; clickX=-1; clickY=-1; focusWindow=''; projectManager=$false; file='selected_camera.png' },
    [pscustomobject]@{ name='selected-light'; row=1; clickX=-1; clickY=-1; focusWindow=''; projectManager=$false; file='selected_light.png' }
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
        FailOnIssues = $true
    }
    if ($state.projectManager) {
        $captureParams.ProjectManagerStartup = $true
    } else {
        $captureParams.UseProjectStartup = $true
    }
    if ($state.row -ge 0) {
        $captureParams.SelectHierarchyRow = $state.row
    }
    if ($state.clickX -ge 0 -and $state.clickY -ge 0) {
        $captureParams.ClickX = $state.clickX
        $captureParams.ClickY = $state.clickY
    }
    if (![string]::IsNullOrWhiteSpace($state.focusWindow)) {
        $captureParams.FocusWindow = $state.focusWindow
    }

    & (Join-Path $PSScriptRoot 'capture_editor_screenshot.ps1') @captureParams
    $passed = (Test-Path -LiteralPath $imagePath) -and (Test-Path -LiteralPath $jsonPath)
    $results += New-ToolResult -Name ("Capture editor state: {0}" -f $state.name) -Passed $passed -Message $imagePath -Details ([pscustomobject]@{
        state = $state.name
        selectHierarchyRow = $state.row
        clickX = $state.clickX
        clickY = $state.clickY
        requestedWindowWidth = $WindowWidth
        requestedWindowHeight = $WindowHeight
        focusWindow = $state.focusWindow
        projectManager = $state.projectManager
        image = $imagePath
        json = $jsonPath
    })
}

$summaryPath = Join-Path $resolvedOutDir 'summary.json'
$passedAll = Write-ToolResults -Results $results -JsonOut $summaryPath
if ($FailOnIssues -and -not $passedAll) { exit 1 }
