param(
    [string]$RepoRoot,
    [Parameter(Mandatory=$true)][string]$Name,
    [string]$Location = 'out\project_fixtures',
    [ValidateSet('Empty','PathTracedDefault','LightingTest','BrokenMissingFolders')][string]$Template = 'Empty',
    [switch]$Force,
    [string]$JsonOut
)

Import-Module (Join-Path $PSScriptRoot 'EditorToolingCommon.psm1') -Force
$RepoRoot = Resolve-EditorToolRepoRoot $RepoRoot
$base = if ([System.IO.Path]::IsPathRooted($Location)) { $Location } else { Join-Path $RepoRoot $Location }
$projectRoot = Join-Path $base $Name
if ((Test-Path $projectRoot) -and -not $Force) { throw "Project fixture already exists: $projectRoot. Pass -Force to reuse." }
New-Item -ItemType Directory -Force $projectRoot | Out-Null
$folders = @('Content','Content\Models','Content\Materials','Content\Textures','Content\HDRI','Content\Prefabs','Content\VDB','Scenes','Cache','Cache\Meshes','Cache\Textures','Cache\Shaders','Cache\BLAS','Cache\Thumbnails','Saved','Saved\Autosaves','Saved\Logs','Saved\Backups','Config','Build')
foreach ($folder in $folders) { New-Item -ItemType Directory -Force (Join-Path $projectRoot $folder) | Out-Null }
if ($Template -eq 'BrokenMissingFolders') { Remove-Item -Recurse -Force (Join-Path $projectRoot 'Cache\BLAS') }
$project = [ordered]@{
    version = 1
    projectGuid = [guid]::NewGuid().ToString()
    name = $Name
    engineVersion = '0.1'
    startupScene = 'Scenes/Main.rtlevel'
    contentRoot = 'Content'
    scenesRoot = 'Scenes'
    cacheRoot = 'Cache'
    savedRoot = 'Saved'
    configRoot = 'Config'
    assetRegistry = 'Content/AssetRegistry.json'
    defaultRenderPreset = 'Editor'
    autosaveEnabled = $true
    autosaveIntervalMinutes = 5
}
$project | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $projectRoot "$Name.rtproject") -Encoding UTF8
@{ version = 1; assets = @() } | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $projectRoot 'Content\AssetRegistry.json') -Encoding UTF8
@{ recentProjects = @(); lastOpenedProject = '' } | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $projectRoot 'Config\EditorConfig.json') -Encoding UTF8
@{} | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $projectRoot 'Config\Layout.json') -Encoding UTF8
@{} | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $projectRoot 'Config\Keybindings.json') -Encoding UTF8
if ($Template -ne 'Empty') {
    $scene = [ordered]@{ version=2; sourceGltf=''; sourceHdr=''; activeCamera=1; primarySun=2; environment=@{ hdrPath=''; intensity=1.0; rotation=0.0; backgroundIntensity=0.35; enabled=$true }; renderSettings=@{}; entities=@() }
    $scene.entities += [ordered]@{ id=@{ uuid=1 }; name='Camera'; parent=0; transform=@{ position=@(0,1.5,4); rotationEuler=@(0,0,0); scale=@(1,1,1) }; camera=@{ verticalFovRadians=1.0472; nearPlane=0.01; farPlane=1000; active=$true; useRenderSettingsExposure=$true } }
    $scene.entities += [ordered]@{ id=@{ uuid=2 }; name='Primary Sun'; parent=0; transform=@{ position=@(0,0,0); rotationEuler=@(0.8,0.4,0); scale=@(1,1,1) }; sun=@{ enabled=$true; illuminanceLux=100000; angularRadiusRadians=0.00465; colorTemperatureKelvin=5778 } }
    if ($Template -eq 'LightingTest') {
        $scene.entities += [ordered]@{ id=@{ uuid=3 }; name='Point Light'; parent=0; transform=@{ position=@(0,2,0); rotationEuler=@(0,0,0); scale=@(1,1,1) }; light=@{ type=1; color=@(1,0.92,0.82); intensity=10; sizeOrRadius=0.25; enabled=$true } }
    }
    $scene | ConvertTo-Json -Depth 20 | Set-Content -LiteralPath (Join-Path $projectRoot 'Scenes\Main.rtlevel') -Encoding UTF8
}
$result = [pscustomobject]@{ projectRoot=$projectRoot; projectFile=(Join-Path $projectRoot "$Name.rtproject"); template=$Template }
if ($JsonOut) { $result | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $JsonOut -Encoding UTF8 }
$result
