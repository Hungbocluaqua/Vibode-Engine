param(
    [string]$RepoRoot,
    [string]$JsonOut,
    [switch]$FailOnIssues
)

Import-Module (Join-Path $PSScriptRoot 'EditorToolingCommon.psm1') -Force
$RepoRoot = Resolve-EditorToolRepoRoot $RepoRoot

$editorPanels = Get-Content -Raw -LiteralPath (Join-Path $RepoRoot 'include\rtv\EditorPanels.h')
$applicationSource = Get-Content -Raw -LiteralPath (Join-Path $RepoRoot 'src\rtv\Application.cpp')
$editorSource = Get-Content -Raw -LiteralPath (Join-Path $RepoRoot 'src\rtv\EditorLayer.cpp')
$viewportSource = Get-Content -Raw -LiteralPath (Join-Path $RepoRoot 'src\rtv\ViewportPanel.cpp')
$cameraSource = Get-Content -Raw -LiteralPath (Join-Path $RepoRoot 'src\rtv\CameraController.cpp')
$contentSource = Get-Content -Raw -LiteralPath (Join-Path $RepoRoot 'src\rtv\AssetBrowserPanel.cpp')
$hierarchySource = Get-Content -Raw -LiteralPath (Join-Path $RepoRoot 'src\rtv\SceneHierarchyPanel.cpp')
$inspectorSource = Get-Content -Raw -LiteralPath (Join-Path $RepoRoot 'src\rtv\InspectorPanel.cpp')

$placement = [ordered]@{
    hasPlacementStatus = ($editorPanels -match 'struct\s+EditorPlacementStatus' -and $editorPanels -match 'const\s+EditorPlacementStatus\*\s+placement')
    selectsPlacedEntity = ($editorSource -match 'selection_\.selectEntity\(state\.placement->entity\)' -and $editorSource -match 'observedPlacementSerial_')
    recordsPlacedPrefabRoot = ($applicationSource -match 'editorPlacement_\.entity\s*=\s*instance\.instanceRoot' -and $applicationSource -match 'Prefab placed and selected')
    recordsCreatedEntity = ($applicationSource -match 'editorPlacement_\.entity\s*=\s*created' -and $applicationSource -match 'Created entity')
    recordsDuplicatedEntity = ($applicationSource -match 'editorPlacement_\.entity\s*=\s*duplicate' -and $applicationSource -match 'Duplicated entity')
    prefabDragDrop = ($contentSource -match 'SetDragDropPayload\("PREFAB_ASSET"' -and $viewportSource -match 'AcceptDragDropPayload\("PREFAB_ASSET"' -and $hierarchySource -match 'AcceptDragDropPayload\("PREFAB_ASSET"')
}

$viewportContext = [ordered]@{
    hasViewportContextMenu = ($viewportSource -match 'ViewportContextMenu' -and $viewportSource -match 'OpenPopup\("ViewportContextMenu"')
    focusSelected = ($viewportSource -match 'Focus Selected' -and $viewportSource -match 'requests\.focusOnEntity')
    duplicateSelected = ($viewportSource -match 'Duplicate' -and $viewportSource -match 'requests\.duplicateEntity')
    deleteSelected = ($viewportSource -match 'Delete' -and $viewportSource -match 'requests\.deleteEntity')
    resetTransform = ($viewportSource -match 'Reset Transform' -and $viewportSource -match 'requests\.setEntityTransform')
    createHere = ($viewportSource -match 'Create Empty Here' -and $viewportSource -match 'Create Camera Here' -and $viewportSource -match 'Create Light Here')
    disabledDropHint = ($viewportSource -match 'Drop prefab here' -and $viewportSource -match 'Drag a prefab from Content')
    usesGlyphRows = ($viewportSource -match 'editorGlyphMenuItem\(EditorGlyphIcon::Frame' -and $viewportSource -match 'editorGlyphMenuItem\(EditorGlyphIcon::Trash')
    suppressesNavigationHoldMenu = ($viewportSource -match 'rightMouseContextCandidate_' -and $viewportSource -match 'viewportContextTapMaxSeconds' -and $viewportSource -match 'releasedMouseCaptureDurationSeconds' -and $viewportSource -match 'releasedMouseCaptureMoved' -and $viewportSource -match '!releasedNavigationGesture' -and $viewportSource -match 'mouseCaptureMoved' -and $cameraSource -match 'mouseCaptureDurationSeconds_')
}

$contextMenus = [ordered]@{
    hierarchyContext = ($hierarchySource -match 'BeginPopupContextItem' -and $hierarchySource -match 'Duplicate' -and $hierarchySource -match 'Delete' -and ($hierarchySource -match 'Focus in Viewport' -or $hierarchySource -match 'Frame Selection'))
    hierarchyEmptyContext = ($hierarchySource -match 'HierarchyEmptyContext' -and $hierarchySource -match 'Empty Entity' -and $hierarchySource -match 'Sky Atmosphere')
    contentContext = ($contentSource -match 'drawPathContextMenu' -and $contentSource -match 'Import Here' -and $contentSource -match 'Show In Explorer' -and $contentSource -match 'Copy Path')
    inspectorActions = ($inspectorSource -match 'EntityActionsPopup' -and $inspectorSource -match 'Duplicate Entity' -and $inspectorSource -match 'Delete Entity' -and $inspectorSource -match 'Add Component')
    componentActions = ($inspectorSource -match 'Reset to Defaults' -and $inspectorSource -match 'Copy Component' -and $inspectorSource -match 'Paste Values' -and $inspectorSource -match 'Remove Component')
}

$results = @(
    New-ToolResult -Name 'Asset placement selects new entity' -Passed ($placement.hasPlacementStatus -and $placement.selectsPlacedEntity -and $placement.recordsPlacedPrefabRoot -and $placement.recordsCreatedEntity -and $placement.recordsDuplicatedEntity -and $placement.prefabDragDrop) -Message ("prefab={0}; created={1}; duplicated={2}; select={3}" -f $placement.recordsPlacedPrefabRoot, $placement.recordsCreatedEntity, $placement.recordsDuplicatedEntity, $placement.selectsPlacedEntity) -Details $placement
    New-ToolResult -Name 'Viewport context menu parity controls' -Passed ($viewportContext.hasViewportContextMenu -and $viewportContext.focusSelected -and $viewportContext.duplicateSelected -and $viewportContext.deleteSelected -and $viewportContext.resetTransform -and $viewportContext.createHere -and $viewportContext.disabledDropHint -and $viewportContext.usesGlyphRows -and $viewportContext.suppressesNavigationHoldMenu) -Message ("menu={0}; actions={1}/{2}/{3}; create={4}; suppressHold={5}" -f $viewportContext.hasViewportContextMenu, $viewportContext.focusSelected, $viewportContext.duplicateSelected, $viewportContext.deleteSelected, $viewportContext.createHere, $viewportContext.suppressesNavigationHoldMenu) -Details $viewportContext
    New-ToolResult -Name 'Editor context menu coverage' -Passed ($contextMenus.hierarchyContext -and $contextMenus.hierarchyEmptyContext -and $contextMenus.contentContext -and $contextMenus.inspectorActions -and $contextMenus.componentActions) -Message ("hierarchy={0}; content={1}; inspector={2}; components={3}" -f $contextMenus.hierarchyContext, $contextMenus.contentContext, $contextMenus.inspectorActions, $contextMenus.componentActions) -Details $contextMenus
)

$passed = Write-ToolResults -Results $results -JsonOut $JsonOut
if ($FailOnIssues -and -not $passed) { exit 1 }
