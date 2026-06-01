param(
    [string]$RepoRoot,
    [string]$Project = 'Projects\MyProject\MyProject.vproject',
    [string]$ContentPath,
    [string]$JsonOut,
    [switch]$FailOnIssues
)

Import-Module (Join-Path $PSScriptRoot 'EditorToolingCommon.psm1') -Force
$RepoRoot = Resolve-EditorToolRepoRoot $RepoRoot

function Get-RelativePathCompat {
    param([string]$BasePath, [string]$FullPath)
    $base = [System.Uri]::new((Join-Path (Resolve-Path -LiteralPath $BasePath).Path '.'))
    $full = [System.Uri]::new((Resolve-Path -LiteralPath $FullPath).Path)
    return [System.Uri]::UnescapeDataString($base.MakeRelativeUri($full).ToString()).Replace('/', '\')
}

$iconMap = [ordered]@{
    '.rtlevel'='scene'
    '.mscene'='scene'
    '.vproject'='project'
    '.rtproject'='project'
    '.gltf'='model'
    '.glb'='model'
    '.obj'='model'
    '.fbx'='model'
    '.mtl'='material'
    '.png'='texture'
    '.jpg'='texture'
    '.jpeg'='texture'
    '.tga'='texture'
    '.bmp'='texture'
    '.hdr'='hdr'
    '.exr'='hdr'
    '.ies'='ies'
    '.vdb'='volume'
    '.glsl'='shader'
    '.hlsl'='shader'
    '.spv'='shader'
    '.json'='data'
    '.ini'='data'
    '.toml'='data'
    '.yaml'='data'
}

$projectPath = if ([System.IO.Path]::IsPathRooted($Project)) { $Project } else { Join-Path $RepoRoot $Project }
$contentRoot = $null
if (![string]::IsNullOrWhiteSpace($ContentPath)) {
    $contentRoot = if ([System.IO.Path]::IsPathRooted($ContentPath)) { $ContentPath } else { Join-Path $RepoRoot $ContentPath }
} elseif (Test-Path -LiteralPath $projectPath) {
    $projectJson = Read-EditorJson -Path $projectPath
    $contentRoot = Join-Path (Split-Path -Parent $projectPath) ([string]$projectJson.contentRoot)
} else {
    $contentRoot = Join-Path $RepoRoot 'Projects\MyProject\Content'
}

$files = @()
if (Test-Path -LiteralPath $contentRoot) {
    $files = @(Get-ChildItem -LiteralPath $contentRoot -Recurse -File | ForEach-Object {
        $ext = $_.Extension.ToLowerInvariant()
        [pscustomobject]@{
            path = $_.FullName
            relative = Get-RelativePathCompat -BasePath $contentRoot -FullPath $_.FullName
            extension = $ext
            icon = if ($iconMap.Contains($ext)) { $iconMap[$ext] } else { 'unsupported-file' }
            supported = $iconMap.Contains($ext)
        }
    })
}

$source = (Get-Content -Raw -LiteralPath (Join-Path $RepoRoot 'src\rtv\EditorLayer.cpp')) + "`n" +
    (Get-Content -Raw -LiteralPath (Join-Path $RepoRoot 'src\rtv\AssetBrowserPanel.cpp')) + "`n" +
    (Get-Content -Raw -LiteralPath (Join-Path $RepoRoot 'src\rtv\ViewportPanel.cpp')) + "`n" +
    (Get-Content -Raw -LiteralPath (Join-Path $RepoRoot 'src\rtv\SceneHierarchyPanel.cpp')) + "`n" +
    (Get-Content -Raw -LiteralPath (Join-Path $RepoRoot 'src\rtv\UiOverlay.cpp')) + "`n" +
    (Get-Content -Raw -LiteralPath (Join-Path $RepoRoot 'include\rtv\EditorPanels.h'))
$applicationSource = Get-Content -Raw -LiteralPath (Join-Path $RepoRoot 'src\rtv\Application.cpp')
$sourceChecks = [ordered]@{
    hasContentWindow = ($source -match 'Content')
    hasFilterPlaceholder = ($source -match 'Filter in selected folder')
    hasPlusButton = ($source -match 'SmallButton\("\+"|Button\("\+"|editorIconButton\("ContentAdd",\s*EditorGlyphIcon::Add')
    hasRemovedCompatibilityText = -not ($source -match 'Scene Import / Compatibility')
    hasNoMiddleActionColumn = -not ($source -match 'Action"')
    hasIconTextDetailsActions = ($source -match 'contentActionButton' -and $source -match 'editorIconTextButton')
    hasRasterThumbnailCache = ($source -match 'thumbnailForPath' -and $source -match 'drawRasterThumbnail' -and $source -match 'stbi_load')
    hasGpuSceneTextureThumbnails = ($source -match 'EditorUiTextureProvider' -and $source -match 'acquireEditorTexture' -and $source -match 'drawGpuSceneTextureThumbnail' -and $source -match 'materialCombinedDescriptors' -and $source -match 'AddImage')
    hasStandaloneGpuPreviewCache = ($source -match 'acquireAssetPreview' -and $source -match 'acquireEditorAssetPreviewTexture' -and $source -match 'assetPreviewTextures_' -and $source -match 'drawStandaloneGpuAssetPreview' -and $source -match 'uploadToImage2D')
    hasGeneratedNonImageGpuPreviewCache = ($source -match 'generateStandalonePreviewPixels' -and $source -match '\.gltf' -and $source -match '\.rtlevel' -and $source -match '\.vproject' -and $source -match '\.ies' -and $source -match '\.vdb')
    hasStandaloneGpuPreviewInvalidation = ($source -match 'invalidateAssetPreviewTextures' -and $source -match 'pathWriteStampForPreview' -and $source -match 'pathSizeForPreview')
    hasRegistryThumbnailPathPreview = ($source -match 'thumbnailPath' -and $source -match 'AssetRecordPreview')
    hasImportOperationQueue = ($source -match 'ImportOperation' -and $source -match 'recordImportOperation' -and $source -match 'drawImportOperations' -and $source -match 'ContentImportOperations')
    hasCompletedLoadBannerCollapse = ($source -match 'sceneLoadStatusIsSuccessfulCompletion' -and $source -match 'sceneLoadCompleted' -and $source -match 'showSceneLoadBanner')
    hasQuietEmptyDetailsPane = ($source -match 'hasDetailsSelection' -and $source -match 'No supported files selected' -and $source -match 'if \(hasDetailsSelection\)')
    hasApplicationImportWorker = ($applicationSource -match 'queueAssetImportNonMutating' -and $applicationSource -match 'queueAssetReimport' -and $applicationSource -match 'startNextAssetImportWorker' -and $applicationSource -match 'pollAssetImportWorker')
    hasAsyncImportStaging = ($applicationSource -match 'std::async\(std::launch::async' -and $applicationSource -match 'stagePlaceholderAssetImport\(request, workspace\)' -and $applicationSource -match 'applyCompletedAssetImport')
    hasImportAndPlaceCompletion = ($applicationSource -match 'placeAfterImport' -and $applicationSource -match 'placePrefabAsset\(importedGuid\)')
    hasPrefabDragDropPlacement = ($source -match 'SetDragDropPayload\("PREFAB_ASSET"' -and $source -match 'AcceptDragDropPayload\("PREFAB_ASSET"' -and $source -match 'requests\.placeAsset')
    hasPlacementSelectionHandoff = ($source -match 'struct\s+EditorPlacementStatus' -and $source -match 'const\s+EditorPlacementStatus\*\s+placement' -and $applicationSource -match 'editorPlacement_\.entity\s*=\s*instance\.instanceRoot' -and $applicationSource -match 'Prefab placed and selected' -and $source -match 'selection_\.selectEntity\(state\.placement->entity\)')
    hasGlyphContextMenus = ($source -match 'drawPathContextMenu' -and $source -match 'editorGlyphMenuItem\(EditorGlyphIcon::Import' -and $source -match 'editorGlyphMenuItem\(EditorGlyphIcon::Refresh' -and $source -match 'editorGlyphMenuItem\(EditorGlyphIcon::Trash')
    hasGeneratedSourcePreview = ($source -match 'SourcePreview' -and $source -match 'sourcePreviewForPath' -and $source -match 'drawGeneratedSourcePreview' -and $source -match 'sourcePreviewCache_')
    hasGeneratedPreviewDiskCache = ($source -match 'generatedPreviewCachePath' -and $source -match 'loadGeneratedPreviewDiskCache' -and $source -match 'saveGeneratedPreviewDiskCache' -and $source -match 'vibode\.generatedPreview\.v1' -and $source -match 'GeneratedPreviews')
    hasNonImagePreviewMetadata = ($source -match 'jsonArraySize\(\*json, "meshes"\)' -and $source -match 'Mesh renderers' -and $source -match 'IES photometric profile' -and $source -match 'OpenVDB volume container')
}

$unsupportedKnown = @($files | Where-Object { -not $_.supported })
$results = @(
    New-ToolResult -Name 'Content root exists' -Passed (Test-Path -LiteralPath $contentRoot) -Message $contentRoot
    New-ToolResult -Name 'Content source reference-match readiness' -Passed ($sourceChecks.hasContentWindow -and $sourceChecks.hasRemovedCompatibilityText) -Message ("plus={0}; filter={1}" -f $sourceChecks.hasPlusButton, $sourceChecks.hasFilterPlaceholder) -Details $sourceChecks
    New-ToolResult -Name 'Content details icon actions present' -Passed $sourceChecks.hasIconTextDetailsActions -Message ("iconTextActions={0}" -f $sourceChecks.hasIconTextDetailsActions) -Details $sourceChecks
    New-ToolResult -Name 'Content raster thumbnail preview present' -Passed ($sourceChecks.hasRasterThumbnailCache -and $sourceChecks.hasRegistryThumbnailPathPreview) -Message ("cache={0}; registryThumbnail={1}" -f $sourceChecks.hasRasterThumbnailCache, $sourceChecks.hasRegistryThumbnailPathPreview) -Details $sourceChecks
    New-ToolResult -Name 'Content GPU scene texture thumbnails present' -Passed $sourceChecks.hasGpuSceneTextureThumbnails -Message ("gpuSceneThumbnails={0}" -f $sourceChecks.hasGpuSceneTextureThumbnails) -Details $sourceChecks
    New-ToolResult -Name 'Content standalone GPU preview cache present' -Passed ($sourceChecks.hasStandaloneGpuPreviewCache -and $sourceChecks.hasStandaloneGpuPreviewInvalidation -and $sourceChecks.hasGeneratedNonImageGpuPreviewCache) -Message ("gpuPreviewCache={0}; generatedNonImage={1}; invalidation={2}" -f $sourceChecks.hasStandaloneGpuPreviewCache, $sourceChecks.hasGeneratedNonImageGpuPreviewCache, $sourceChecks.hasStandaloneGpuPreviewInvalidation) -Details $sourceChecks
    New-ToolResult -Name 'Content generated non-image preview present' -Passed ($sourceChecks.hasGeneratedSourcePreview -and $sourceChecks.hasNonImagePreviewMetadata -and $sourceChecks.hasGeneratedPreviewDiskCache) -Message ("generated={0}; metadata={1}; diskCache={2}" -f $sourceChecks.hasGeneratedSourcePreview, $sourceChecks.hasNonImagePreviewMetadata, $sourceChecks.hasGeneratedPreviewDiskCache) -Details $sourceChecks
    New-ToolResult -Name 'Content import operation queue present' -Passed $sourceChecks.hasImportOperationQueue -Message ("operationQueue={0}" -f $sourceChecks.hasImportOperationQueue) -Details $sourceChecks
    New-ToolResult -Name 'Content completed load banner collapsed' -Passed $sourceChecks.hasCompletedLoadBannerCollapse -Message ("completedLoadCollapse={0}" -f $sourceChecks.hasCompletedLoadBannerCollapse) -Details $sourceChecks
    New-ToolResult -Name 'Content empty details pane is quiet' -Passed $sourceChecks.hasQuietEmptyDetailsPane -Message ("quietEmptyDetails={0}" -f $sourceChecks.hasQuietEmptyDetailsPane) -Details $sourceChecks
    New-ToolResult -Name 'Content import worker execution present' -Passed ($sourceChecks.hasApplicationImportWorker -and $sourceChecks.hasAsyncImportStaging -and $sourceChecks.hasImportAndPlaceCompletion) -Message ("worker={0}; asyncStage={1}; placeOnCompletion={2}" -f $sourceChecks.hasApplicationImportWorker, $sourceChecks.hasAsyncImportStaging, $sourceChecks.hasImportAndPlaceCompletion) -Details $sourceChecks
    New-ToolResult -Name 'Content asset placement selection flow present' -Passed ($sourceChecks.hasPrefabDragDropPlacement -and $sourceChecks.hasPlacementSelectionHandoff) -Message ("dragDrop={0}; selectionHandoff={1}" -f $sourceChecks.hasPrefabDragDropPlacement, $sourceChecks.hasPlacementSelectionHandoff) -Details $sourceChecks
    New-ToolResult -Name 'Content context menu glyph chrome present' -Passed $sourceChecks.hasGlyphContextMenus -Message ("glyphContextMenus={0}" -f $sourceChecks.hasGlyphContextMenus) -Details $sourceChecks
    New-ToolResult -Name 'Asset file icon mapping inventory' -Passed $true -Message ("files={0}; unsupported={1}" -f $files.Count, $unsupportedKnown.Count) -Details $files
)
$passed = Write-ToolResults -Results $results -JsonOut $JsonOut
if ($FailOnIssues -and -not $passed) { exit 1 }
