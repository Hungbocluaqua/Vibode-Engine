param(
    [string]$RepoRoot,
    [string]$Scene = 'scenes\validation\cornell.rtlevel',
    [string[]]$Assets = @('Sponza\glTF\Sponza.gltf'),
    [string]$JsonOut,
    [switch]$ExpectImplemented,
    [switch]$FailOnRegression
)

Import-Module (Join-Path $PSScriptRoot 'EditorToolingCommon.psm1') -Force
$RepoRoot = Resolve-EditorToolRepoRoot $RepoRoot
$scenePath = if ([System.IO.Path]::IsPathRooted($Scene)) { $Scene } else { Join-Path $RepoRoot $Scene }
$beforeCount = Get-EntityCountFromRtLevel -Path $scenePath
$requestAuditJson = if ($JsonOut) { [System.IO.Path]::ChangeExtension($JsonOut, '.requests.json') } else { $null }
& (Join-Path $PSScriptRoot 'editor_request_flow_report.ps1') -RepoRoot $RepoRoot -JsonOut $requestAuditJson | Out-Host
$editorPanels = Get-Content -Raw -LiteralPath (Join-Path $RepoRoot 'include\rtv\EditorPanels.h')
$hasImportAsset = $editorPanels -match 'importAsset'
$hasImportAndPlace = $editorPanels -match 'importAndPlace'
$hasReimportAsset = $editorPanels -match 'reimportAsset'
$hasPlaceAsset = $editorPanels -match 'placeAsset'
$application = Get-Content -Raw -LiteralPath (Join-Path $RepoRoot 'src\rtv\Application.cpp')
$assetImport = Get-Content -Raw -LiteralPath (Join-Path $RepoRoot 'src\rtv\AssetImport.cpp')
$hasNonMutatingImport = $application -match 'importAssetNonMutating' -and $application -match 'stagePlaceholderAssetImport'
$importHandlerCallsSceneReplacement = $application -match 'if \(requests\.importAsset\.has_value\(\)\) \{\s*requestGltfSceneLoad'
$hasGltfAssetImport = $assetImport -match 'GltfLoader' -and $assetImport -match 'ImportedGltfTexture' -and $assetImport -match 'ImportedGltfMaterial' -and $assetImport -match 'ImportedGltfMesh'
$hasDeterministicImportGuids = $assetImport -match 'importedAssetGuidFor' -and $assetImport -match 'sourceHash' -and $assetImport -match 'importSettingsHash'
$hasPrefabPlacement = (Get-TextMatches -RepoRoot $RepoRoot -Pattern 'PrefabAsset|PrefabInstance|PrefabOverride|placePrefab|PREFAB_ASSET' -Paths @('include','src')).Count -gt 0
$hasImportAndPlaceHandler = $application -match 'requests\.importAndPlace\.has_value' -and $application -match 'placePrefabAsset'
$hasMergeSceneHandler = $application -match 'requests\.mergeScene\.has_value' -and $application -match 'mergeSceneIntoCurrent' -and $application -notmatch 'Merge Scene is not implemented yet'
$hasMergeSceneAppend = (Get-TextMatches -RepoRoot $RepoRoot -Pattern 'mergeSceneAsset|Merged Scene|appendImportedAssets|remapSceneAssetHandles' -Paths @('include','src')).Count -gt 0
$assetChecks = @()
foreach ($asset in $Assets) {
    $full = if ([System.IO.Path]::IsPathRooted($asset)) { $asset } else { Join-Path $RepoRoot $asset }
    $assetChecks += [pscustomobject]@{ path=$full; exists=(Test-Path $full) }
}
$results = @(
    (New-ToolResult -Name 'Baseline scene entity count captured' -Passed $true -Message "entities=$beforeCount" -Details @{ scene=$scenePath; entityCount=$beforeCount }),
    (New-ToolResult -Name 'Import Asset request exists' -Passed $hasImportAsset -Message "implemented=$hasImportAsset"),
    (New-ToolResult -Name 'Import and Place request exists' -Passed $hasImportAndPlace -Message "implemented=$hasImportAndPlace"),
    (New-ToolResult -Name 'Reimport Asset request exists' -Passed $hasReimportAsset -Message "implemented=$hasReimportAsset"),
    (New-ToolResult -Name 'Place Asset request exists' -Passed $hasPlaceAsset -Message "implemented=$hasPlaceAsset"),
    (New-ToolResult -Name 'Non-mutating import skeleton present' -Passed $hasNonMutatingImport -Message "implemented=$hasNonMutatingImport"),
    (New-ToolResult -Name 'Import Asset avoids scene replacement path' -Passed (-not $importHandlerCallsSceneReplacement) -Message "callsSceneReplacement=$importHandlerCallsSceneReplacement"),
    (New-ToolResult -Name 'glTF asset import metadata present' -Passed $hasGltfAssetImport -Message "implemented=$hasGltfAssetImport"),
    (New-ToolResult -Name 'Import dedupe metadata present' -Passed $hasDeterministicImportGuids -Message "implemented=$hasDeterministicImportGuids"),
    (New-ToolResult -Name 'Prefab placement support present' -Passed $hasPrefabPlacement -Message "implemented=$hasPrefabPlacement"),
    (New-ToolResult -Name 'Import and Place handler present' -Passed $hasImportAndPlaceHandler -Message "implemented=$hasImportAndPlaceHandler"),
    (New-ToolResult -Name 'Merge Scene handler present' -Passed $hasMergeSceneHandler -Message "implemented=$hasMergeSceneHandler"),
    (New-ToolResult -Name 'Merge Scene append support present' -Passed $hasMergeSceneAppend -Message "implemented=$hasMergeSceneAppend"),
    (New-ToolResult -Name 'Import regression assets exist' -Passed (@($assetChecks | Where-Object { -not $_.exists }).Count -eq 0) -Details $assetChecks)
)
if (!$hasImportAsset -and !$ExpectImplemented) {
    $results += New-ToolResult -Name 'Harness status' -Passed $true -Message 'Import Asset is not implemented yet; static readiness checks only.'
}
$passed = Write-ToolResults -Results $results -JsonOut $JsonOut
if (($FailOnRegression -or $ExpectImplemented) -and -not $passed) { exit 1 }
