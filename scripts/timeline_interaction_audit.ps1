param(
    [string]$RepoRoot,
    [string]$JsonOut,
    [switch]$FailOnIssues
)

Import-Module (Join-Path $PSScriptRoot 'EditorToolingCommon.psm1') -Force
$RepoRoot = Resolve-EditorToolRepoRoot $RepoRoot

$timelineHeader = Get-Content -Raw -LiteralPath (Join-Path $RepoRoot 'include\rtv\EditorTimeline.h')
$timelineSource = Get-Content -Raw -LiteralPath (Join-Path $RepoRoot 'src\rtv\EditorTimeline.cpp')
$editorHeader = Get-Content -Raw -LiteralPath (Join-Path $RepoRoot 'include\rtv\EditorLayer.h')
$editorSource = Get-Content -Raw -LiteralPath (Join-Path $RepoRoot 'src\rtv\EditorLayer.cpp')

$checks = [ordered]@{
    stableKeyIds = ($timelineHeader -match 'uint64_t id' -and $timelineHeader -match 'nextKeyId_' -and $timelineSource -match '"id"')
    keyFrameUpdateById = ($timelineHeader -match 'updateTransformKeyFrame' -and $timelineSource -match 'updateTransformKeyFrame\(uint64_t keyId')
    keyRemovalById = ($timelineHeader -match 'removeTransformKeyById' -and $timelineSource -match 'removeTransformKeyById\(uint64_t keyId')
    selectionState = ($editorHeader -match 'timelineSelectedKeyIds_' -and $editorSource -match 'Selected keys')
    ctrlShiftSelection = ($editorSource -match 'KeyCtrl' -and $editorSource -match 'KeyShift' -and $editorSource -match 'selectTimelineKey')
    dragEditing = ($editorHeader -match 'timelineDraggingKeys_' -and $editorSource -match 'beginTimelineKeyDrag' -and $editorSource -match 'IsMouseDragging' -and $editorSource -match 'framesPerPixel')
    selectedDelete = ($editorSource -match 'TimelineDeleteSelectedKeys' -and $editorSource -match 'removeTransformKeyById')
    sequenceFrameRate = ($timelineHeader -match 'frameRate' -and $timelineSource -match '"frameRate"' -and $timelineSource -match 'std::clamp\(frameRate')
    sequenceRangeActions = ($editorSource -match 'TimelineRangeStart' -and $editorSource -match 'TimelineRangeEnd' -and $editorSource -match 'TimelineFitRangeToKeys')
    sequenceRenderAction = ($editorSource -match 'TimelineRenderSequence' -and $editorSource -match 'requests\.renderSequence = true')
}

$results = @(
    New-ToolResult -Name 'Timeline stable key identity' -Passed ($checks.stableKeyIds -and $checks.keyFrameUpdateById -and $checks.keyRemovalById) -Message ("ids={0}; update={1}; remove={2}" -f $checks.stableKeyIds, $checks.keyFrameUpdateById, $checks.keyRemovalById) -Details $checks
    New-ToolResult -Name 'Timeline multi-select controls' -Passed ($checks.selectionState -and $checks.ctrlShiftSelection -and $checks.selectedDelete) -Message ("state={0}; ctrlShift={1}; delete={2}" -f $checks.selectionState, $checks.ctrlShiftSelection, $checks.selectedDelete) -Details $checks
    New-ToolResult -Name 'Timeline drag key editing' -Passed $checks.dragEditing -Message ("drag={0}" -f $checks.dragEditing) -Details $checks
    New-ToolResult -Name 'Timeline sequence workflow controls' -Passed ($checks.sequenceFrameRate -and $checks.sequenceRangeActions -and $checks.sequenceRenderAction) -Message ("fps={0}; rangeActions={1}; renderSequence={2}" -f $checks.sequenceFrameRate, $checks.sequenceRangeActions, $checks.sequenceRenderAction) -Details $checks
)

$passed = Write-ToolResults -Results $results -JsonOut $JsonOut
if ($FailOnIssues -and -not $passed) { exit 1 }
