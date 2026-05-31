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
$assetChecks = @()
foreach ($asset in $Assets) {
    $full = if ([System.IO.Path]::IsPathRooted($asset)) { $asset } else { Join-Path $RepoRoot $asset }
    $assetChecks += [pscustomobject]@{ path=$full; exists=(Test-Path $full) }
}
$results = @(
    (New-ToolResult -Name 'Baseline scene entity count captured' -Passed $true -Message "entities=$beforeCount" -Details @{ scene=$scenePath; entityCount=$beforeCount }),
    (New-ToolResult -Name 'Import Asset request exists' -Passed $hasImportAsset -Message "implemented=$hasImportAsset"),
    (New-ToolResult -Name 'Import and Place request exists' -Passed $hasImportAndPlace -Message "implemented=$hasImportAndPlace"),
    (New-ToolResult -Name 'Import regression assets exist' -Passed (@($assetChecks | Where-Object { -not $_.exists }).Count -eq 0) -Details $assetChecks)
)
if (!$hasImportAsset -and !$ExpectImplemented) {
    $results += New-ToolResult -Name 'Harness status' -Passed $true -Message 'Import Asset is not implemented yet; static readiness checks only.'
}
$passed = Write-ToolResults -Results $results -JsonOut $JsonOut
if (($FailOnRegression -or $ExpectImplemented) -and -not $passed) { exit 1 }
