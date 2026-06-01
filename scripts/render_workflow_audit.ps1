param(
    [string]$RepoRoot,
    [string]$JsonOut,
    [switch]$FailOnIssues
)

Import-Module (Join-Path $PSScriptRoot 'EditorToolingCommon.psm1') -Force
$RepoRoot = Resolve-EditorToolRepoRoot $RepoRoot

$commands = Get-Content -Raw -LiteralPath (Join-Path $RepoRoot 'include\rtv\EditorCommands.h')
$dockspace = Get-Content -Raw -LiteralPath (Join-Path $RepoRoot 'src\rtv\EditorDockspace.cpp')
$renderSettingsPanel = Get-Content -Raw -LiteralPath (Join-Path $RepoRoot 'src\rtv\RenderSettingsPanel.cpp')
$requests = Get-Content -Raw -LiteralPath (Join-Path $RepoRoot 'include\rtv\EditorPanels.h')
$applicationHeader = Get-Content -Raw -LiteralPath (Join-Path $RepoRoot 'include\rtv\Application.h')
$applicationSource = Get-Content -Raw -LiteralPath (Join-Path $RepoRoot 'src\rtv\Application.cpp')
$editorHeader = Get-Content -Raw -LiteralPath (Join-Path $RepoRoot 'include\rtv\EditorLayer.h')
$editorSource = Get-Content -Raw -LiteralPath (Join-Path $RepoRoot 'src\rtv\EditorLayer.cpp')
$uiOverlayHeader = Get-Content -Raw -LiteralPath (Join-Path $RepoRoot 'include\rtv\UiOverlay.h')
$uiOverlaySource = Get-Content -Raw -LiteralPath (Join-Path $RepoRoot 'src\rtv\UiOverlay.cpp')

$required = @(
    'RenderCurrentViewport',
    'RenderImage',
    'RenderSequence',
    'StopRender',
    'OpenOutputFolder'
)
$presence = @($required | ForEach-Object {
    [pscustomobject]@{
        name=$_
        commandEnum=($commands -match $_)
        menuText=($dockspace -match ($_ -replace '([A-Z])',' $1').Trim() -or $dockspace -match $_)
        request=($requests -match $_)
    }
})

$existingFallback = [ordered]@{
    hasRenderMenu = ($dockspace -match 'BeginMenu\("Render"\)')
    hasResetAccumulation = ($dockspace -match 'Reset Accumulation')
    hasDockedRenderSettingsActions = ($renderSettingsPanel -match 'Preview Actions' -and $renderSettingsPanel -match 'RenderSettingsResetAccumulation' -and $renderSettingsPanel -match 'RenderSettingsCycleDebugView' -and $renderSettingsPanel -match 'RenderSettingsCycleIntermediate' -and $renderSettingsPanel -match 'RenderSettingsToggleDenoiser')
    hasScreenshotHeadlessFlags = ((Get-TextMatches -RepoRoot $RepoRoot -Pattern 'save-debug-views|save-frame-sequence|profile-json' -Paths @('src','include','docs')).Count -gt 0)
}

$renderJobState = [ordered]@{
    hasRuntimeStatus = ($requests -match 'struct\s+EditorRenderJobStatus' -and $requests -match 'enum class\s+EditorRenderJobKind' -and $requests -match 'const\s+EditorRenderJobStatus\*\s+renderJob')
    exposedThroughOverlay = ($uiOverlayHeader -match 'const\s+EditorRenderJobStatus\*\s+renderJob' -and $uiOverlaySource -match '\.renderJob\s*=\s*renderJob')
    applicationOwnsJob = ($applicationHeader -match 'EditorRenderJobStatus\s+editorRenderJob_' -and $applicationSource -match 'updateEditorRenderJob\(deltaSeconds\)' -and $applicationSource -match 'startEditorRenderJob\(EditorRenderJobKind::Sequence')
    writesManifest = ($applicationSource -match 'render_manifest\.json' -and $applicationSource -match 'writeEditorRenderJobManifest' -and $applicationSource -match 'manifest\["renderer"\]')
    supportsCancel = ($applicationSource -match 'cancelEditorRenderJob' -and $applicationSource -match 'StopRender' -and $applicationSource -match 'No active render job to stop')
}

$renderJobModal = [ordered]@{
    hasModal = ($editorHeader -match 'drawRenderJobModal' -and $editorSource -match 'BeginPopupModal\("Render Output"')
    hasProgress = ($editorSource -match 'Render Output' -and $editorSource -match 'ProgressBar\(std::clamp\(job->progress')
    hasOpenOutputAction = ($editorSource -match 'RenderJobOpenOutput' -and $editorSource -match 'requests\.openOutputFolder\s*=\s*true')
    opensActiveJobFolder = ($applicationSource -match 'editorRenderJob_\.outputRoot' -and $applicationSource -match 'Open render output folder:')
    hasStopAction = ($editorSource -match 'RenderJobStop' -and $editorSource -match 'requests\.stopRender\s*=\s*true')
    hasSequenceFrameReadout = ($editorSource -match 'Frames: %d / %d' -and $applicationSource -match 'timeline\.endFrame - timeline\.startFrame \+ 1')
}

$missing = @($presence | Where-Object { -not ($_.commandEnum -or $_.menuText -or $_.request) })
$results = @(
    New-ToolResult -Name 'Render workflow explicit command readiness' -Passed ($missing.Count -eq 0) -Message ("missing={0}" -f ((@($missing | ForEach-Object { $_.name })) -join ', ')) -Details $presence
    New-ToolResult -Name 'Existing render diagnostic fallback' -Passed ($existingFallback.hasRenderMenu -and $existingFallback.hasDockedRenderSettingsActions -and $existingFallback.hasScreenshotHeadlessFlags) -Message ("renderMenu={0}; dockedActions={1}" -f $existingFallback.hasRenderMenu, $existingFallback.hasDockedRenderSettingsActions) -Details $existingFallback
    New-ToolResult -Name 'Editor render job output state' -Passed ($renderJobState.hasRuntimeStatus -and $renderJobState.exposedThroughOverlay -and $renderJobState.applicationOwnsJob -and $renderJobState.writesManifest -and $renderJobState.supportsCancel) -Message ("state={0}; manifest={1}; cancel={2}" -f $renderJobState.hasRuntimeStatus, $renderJobState.writesManifest, $renderJobState.supportsCancel) -Details $renderJobState
    New-ToolResult -Name 'Editor render output modal controls' -Passed ($renderJobModal.hasModal -and $renderJobModal.hasProgress -and $renderJobModal.hasOpenOutputAction -and $renderJobModal.opensActiveJobFolder -and $renderJobModal.hasStopAction -and $renderJobModal.hasSequenceFrameReadout) -Message ("modal={0}; progress={1}; stop={2}; open={3}" -f $renderJobModal.hasModal, $renderJobModal.hasProgress, $renderJobModal.hasStopAction, $renderJobModal.hasOpenOutputAction) -Details $renderJobModal
)
$passed = Write-ToolResults -Results $results -JsonOut $JsonOut
if ($FailOnIssues -and -not $passed) { exit 1 }
