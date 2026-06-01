param(
    [string]$RepoRoot,
    [string]$Scene = 'scenes\validation\cornell.rtlevel',
    [string]$JsonOut,
    [switch]$FailOnMissing
)

Import-Module (Join-Path $PSScriptRoot 'EditorToolingCommon.psm1') -Force
$RepoRoot = Resolve-EditorToolRepoRoot $RepoRoot

function Get-EntityType {
    param([object]$Entity)
    if ($null -ne $Entity.camera) { return 'camera' }
    if ($null -ne $Entity.sun) { return 'sun' }
    if ($null -ne $Entity.light) { return 'light' }
    if ($null -ne $Entity.environmentLight) { return 'environmentLight' }
    if ($null -ne $Entity.skyAtmosphere) { return 'skyAtmosphere' }
    if ($null -ne $Entity.heightFog) { return 'heightFog' }
    if ($null -ne $Entity.volumetricCloud) { return 'volumetricCloud' }
    if ($null -ne $Entity.postProcessVolume) { return 'postProcessVolume' }
    if ($null -ne $Entity.meshRenderer -or $null -ne $Entity.mesh) { return 'mesh' }
    if (@($Entity.children).Count -gt 0) { return 'group' }
    return 'empty'
}

$iconMap = @{
    camera='camera'
    sun='sun'
    light='light'
    environmentLight='environment-light'
    skyAtmosphere='sky-atmosphere'
    heightFog='height-fog'
    volumetricCloud='volumetric-cloud'
    postProcessVolume='post-process-volume'
    mesh='mesh-cube'
    group='actor-group'
    empty='unknown-entity'
}

$scenePath = if ([System.IO.Path]::IsPathRooted($Scene)) { $Scene } else { Join-Path $RepoRoot $Scene }
$rows = @()
if (Test-Path -LiteralPath $scenePath) {
    $json = Read-EditorJson -Path $scenePath
    foreach ($entity in @($json.entities)) {
        $type = Get-EntityType $entity
        $rows += [pscustomobject]@{
            name = [string]$entity.name
            type = $type
            icon = $iconMap[$type]
            hasIcon = -not [string]::IsNullOrWhiteSpace($iconMap[$type])
            hasVisibilityEye = $true
            parent = $entity.parent
            childCount = @($entity.children).Count
        }
    }
}

$editorSource = (Get-Content -Raw -LiteralPath (Join-Path $RepoRoot 'src\rtv\EditorLayer.cpp')) + "`n" +
    (Get-Content -Raw -LiteralPath (Join-Path $RepoRoot 'src\rtv\SceneHierarchyPanel.cpp')) + "`n" +
    (Get-Content -Raw -LiteralPath (Join-Path $RepoRoot 'include\rtv\EditorUiStyle.h'))
$sourceChecks = [ordered]@{
    hasHierarchyWindow = ($editorSource -match 'Hierarchy')
    hasVisibilityToggleRequest = ($editorSource -match 'setEntityVisibility|visibility')
    hasIconLiteralOrHelper = ($editorSource -match 'editorGlyphForEntity|editorDrawIconGlyph|icon|Icon|ICON|cube|camera|eye')
    hasTypeFilterToolbar = ($editorSource -match 'HierarchyTypeFilterMesh' -and $editorSource -match 'hierarchyTypeFilterButton' -and $editorSource -match 'typeFilterMask_')
    hasActiveFilterTint = ($editorSource -match 'editorIconTint\(active\)' -and $editorSource -match 'editorSelectedRowColor\(\)')
    hasGlyphContextMenus = ($editorSource -match 'BeginPopupContextItem' -and $editorSource -match 'editorGlyphMenuItem\(EditorGlyphIcon::Trash' -and $editorSource -match 'editorGlyphMenuItem\(EditorGlyphIcon::Frame')
    hasReferenceRowChrome = ($editorSource -match 'drawHierarchyIndentGuides' -and $editorSource -match 'drawHierarchyRightFade' -and $editorSource -match 'hierarchyIndentSpacing' -and $editorSource -match 'hierarchyRowRightFadeWidth')
    hasGlyphCreateToolbar = ($editorSource -match 'HierarchyCreateEmpty' -and $editorSource -match 'HierarchyCreateCamera' -and $editorSource -match 'HierarchyCreateLight' -and $editorSource -match 'editorIconTextButton')
}

$missingIcons = @($rows | Where-Object { -not $_.hasIcon })
$results = @(
    New-ToolResult -Name 'Hierarchy rows discovered' -Passed ($rows.Count -gt 0) -Message ("rows={0}" -f $rows.Count) -Details $rows
    New-ToolResult -Name 'Hierarchy icon mapping complete for scene' -Passed ($missingIcons.Count -eq 0) -Message ("missing={0}" -f $missingIcons.Count) -Details $missingIcons
    New-ToolResult -Name 'Hierarchy source visibility/icon readiness' -Passed ($sourceChecks.hasHierarchyWindow -and $sourceChecks.hasVisibilityToggleRequest) -Message ("iconSource={0}" -f $sourceChecks.hasIconLiteralOrHelper) -Details $sourceChecks
    New-ToolResult -Name 'Hierarchy type filter toolbar present' -Passed ($sourceChecks.hasTypeFilterToolbar -and $sourceChecks.hasActiveFilterTint) -Message ("toolbar={0}; activeTint={1}" -f $sourceChecks.hasTypeFilterToolbar, $sourceChecks.hasActiveFilterTint) -Details $sourceChecks
    New-ToolResult -Name 'Hierarchy context menu glyph chrome present' -Passed $sourceChecks.hasGlyphContextMenus -Message ("glyphContextMenus={0}" -f $sourceChecks.hasGlyphContextMenus) -Details $sourceChecks
    New-ToolResult -Name 'Hierarchy reference row chrome present' -Passed $sourceChecks.hasReferenceRowChrome -Message ("rowChrome={0}" -f $sourceChecks.hasReferenceRowChrome) -Details $sourceChecks
    New-ToolResult -Name 'Hierarchy create toolbar glyph chrome present' -Passed $sourceChecks.hasGlyphCreateToolbar -Message ("createToolbar={0}" -f $sourceChecks.hasGlyphCreateToolbar) -Details $sourceChecks
)
$passed = Write-ToolResults -Results $results -JsonOut $JsonOut
if ($FailOnMissing -and -not $passed) { exit 1 }
