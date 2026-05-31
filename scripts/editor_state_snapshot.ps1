param(
    [string]$RepoRoot,
    [string]$Scene,
    [string]$Project,
    [string]$ExePath,
    [ValidateSet('Debug','Release')][string]$Config = 'Release',
    [string]$JsonOut = 'out\editor_state_snapshot.json'
)

Import-Module (Join-Path $PSScriptRoot 'EditorToolingCommon.psm1') -Force
$RepoRoot = Resolve-EditorToolRepoRoot $RepoRoot
$exe = $null
try { $exe = Find-RtvulkanExe -RepoRoot $RepoRoot -Config $Config -ExePath $ExePath } catch { }
$editorPanels = Get-Content -Raw -LiteralPath (Join-Path $RepoRoot 'include\rtv\EditorPanels.h')
$dockspace = Get-Content -Raw -LiteralPath (Join-Path $RepoRoot 'src\rtv\EditorDockspace.cpp')
$snapshot = [ordered]@{
    generatedAt = (Get-Date).ToString('o')
    repoRoot = $RepoRoot
    executable = $exe
    sourceState = [ordered]@{
        hasProjectContext = ((Get-TextMatches -RepoRoot $RepoRoot -Pattern 'ProjectContext' -Paths @('include','src')).Count -gt 0)
        hasAssetRegistry = ((Get-TextMatches -RepoRoot $RepoRoot -Pattern 'AssetRegistry|AssetRecord' -Paths @('include','src')).Count -gt 0)
        hasImportAssetRequest = ($editorPanels -match 'importAsset')
        hasImportSceneAsNewSceneRequest = ($editorPanels -match 'importSceneAsNewScene')
        hasPrefabSupport = ((Get-TextMatches -RepoRoot $RepoRoot -Pattern 'PrefabAsset|PrefabInstance' -Paths @('include','src')).Count -gt 0)
        hasTargetMenus = ($dockspace -match 'BeginMenu\("Create"\)' -and $dockspace -match 'BeginMenu\("Engine"\)' -and $dockspace -match 'BeginMenu\("Layout"\)')
    }
}
if (![string]::IsNullOrWhiteSpace($Scene)) {
    $scenePath = if ([System.IO.Path]::IsPathRooted($Scene)) { $Scene } else { Join-Path $RepoRoot $Scene }
    if (Test-Path $scenePath) {
        $sceneJson = Read-EditorJson -Path $scenePath
        $snapshot.scene = [ordered]@{ path=$scenePath; entityCount=@($sceneJson.entities).Count; version=$sceneJson.version; sourceGltf=$sceneJson.sourceGltf; dirty=$null }
    }
}
if (![string]::IsNullOrWhiteSpace($Project)) {
    $projectPath = if ([System.IO.Path]::IsPathRooted($Project)) { $Project } else { Join-Path $RepoRoot $Project }
    if (Test-Path $projectPath) {
        $projectJson = Read-EditorJson -Path $projectPath
        $snapshot.project = [ordered]@{ path=$projectPath; name=$projectJson.name; guid=$projectJson.projectGuid; startupScene=$projectJson.startupScene; assetRegistry=$projectJson.assetRegistry }
    }
}
$out = if ([System.IO.Path]::IsPathRooted($JsonOut)) { $JsonOut } else { Join-Path $RepoRoot $JsonOut }
New-Item -ItemType Directory -Force (Split-Path -Parent $out) | Out-Null
$snapshot | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath $out -Encoding UTF8
Write-Host "Wrote editor state snapshot: $out"
