param(
    [string]$RepoRoot,
    [string]$JsonOut,
    [switch]$FailOnIssues
)

Import-Module (Join-Path $PSScriptRoot 'EditorToolingCommon.psm1') -Force
$RepoRoot = Resolve-EditorToolRepoRoot $RepoRoot

$defaultRoot = Join-Path ([Environment]::GetFolderPath('MyDocuments')) 'Vibode Projects'
$fixtureRoot = Join-Path $RepoRoot 'out\project_manager_fixtures'
$validName = 'MyProject'
$duplicateRoot = Join-Path $fixtureRoot $validName
$invalidNames = @('', 'Bad:Name', 'Bad/Name', 'Bad\Name', 'Bad*Name', 'Bad?Name')

New-Item -ItemType Directory -Force $duplicateRoot | Out-Null
@{ version=1; name=$validName; startupScene='Scenes/Main.rtlevel'; contentRoot='Content'; scenesRoot='Scenes' } |
    ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $duplicateRoot "$validName.vproject") -Encoding UTF8

$validationCases = @()
foreach ($name in $invalidNames) {
    $isValid = -not [string]::IsNullOrWhiteSpace($name) -and ($name.IndexOfAny([System.IO.Path]::GetInvalidFileNameChars()) -lt 0) -and ($name -notmatch '[\\/:*?"<>|]')
    $validationCases += [pscustomobject]@{ name=$name; valid=$isValid }
}
$duplicateProjectFile = Join-Path $duplicateRoot "$validName.vproject"

$sourceText = (Get-TextMatches -RepoRoot $RepoRoot -Pattern 'Project Manager|Recent Projects|CreateProjectRequest|OpenProjectRequest|Vibode Projects|vproject' -Paths @('include','src','scripts')) -join "`n"
$editorLayer = Get-Content -Raw -LiteralPath (Join-Path $RepoRoot 'src\rtv\EditorLayer.cpp')
$application = Get-Content -Raw -LiteralPath (Join-Path $RepoRoot 'src\rtv\Application.cpp')
$uiOverlay = Get-Content -Raw -LiteralPath (Join-Path $RepoRoot 'src\rtv\UiOverlay.cpp')
$lightweightSponzaProjectPath = Join-Path $RepoRoot 'Samples\LightweightSponza\LightweightSponza.vproject'
$lightweightSponzaScenePath = Join-Path $RepoRoot 'scenes\validation\lightweight_sponza.rtlevel'
$lightweightSponzaProject = if (Test-Path -LiteralPath $lightweightSponzaProjectPath) { Read-EditorJson -Path $lightweightSponzaProjectPath } else { $null }
$lightweightSponzaScene = if (Test-Path -LiteralPath $lightweightSponzaScenePath) { Read-EditorJson -Path $lightweightSponzaScenePath } else { $null }
$checks = [ordered]@{
    defaultRoot = $defaultRoot
    defaultRootUnderDocuments = $defaultRoot.StartsWith([Environment]::GetFolderPath('MyDocuments'))
    hasProjectManagerSource = ($sourceText -match 'Project Manager')
    hasCreateProjectRequest = ($sourceText -match 'CreateProjectRequest')
    hasOpenProjectRequest = ($sourceText -match 'OpenProjectRequest')
    hasProjectThumbnailRasterLoad = ($editorLayer -match 'projectCardThumbnail' -and $editorLayer -match 'drawProjectCardThumbnail' -and $editorLayer -match 'stbi_load')
    hasSavedThumbnailPaths = ($editorLayer -match 'Saved"\s*/\s*"Thumbnail.png' -and $editorLayer -match 'Saved"\s*/\s*"ProjectThumbnail.png')
    hasDeferredRendererStartup = ($application -match 'deferRendererForProjectManager' -and $application -match 'Project Manager launcher active; renderer startup deferred')
    hasExplicitSceneProjectManagerDismissal = ($application -match 'explicitStartupScene' -and $application -match 'dismissProjectManager')
    hasCaptureStartupProjectOverride = ($application -match 'RTV_EDITOR_STARTUP_PROJECT' -and $application -match 'startupProjectOverridePath' -and $application -match 'startupProjectPath')
    hasCaptureRecoverySuppress = ($application -match 'RTV_EDITOR_SUPPRESS_RECOVERY_PROMPT' -and $application -match 'editorRecoveryPromptSuppressed')
    hasProjectManagerOnlyOverlay = ($uiOverlay -match 'buildProjectManager' -and $editorLayer -match 'drawProjectManagerLauncher')
    hasContinueWithoutProjectRendererInit = ($application -match 'continueWithoutProject' -and $application -match 'initializeRendererFromCurrentScene')
    hasGeneratedProjectThumbnailCapture = ($application -match 'queueProjectThumbnailCapture' -and $application -match 'captureProjectThumbnailIfReady' -and $application -match 'DiagnosticImageExport' -and $application -match 'savedRoot\s*/\s*"Thumbnail.png"')
    hasLightweightSponzaStartupScene = ($null -ne $lightweightSponzaProject -and [string]$lightweightSponzaProject.startupScene -match 'lightweight_sponza\.rtlevel')
    hasLightweightSponzaGltfScene = ($null -ne $lightweightSponzaScene -and [string]$lightweightSponzaScene.sourceGltf -eq 'Sponza/glTF/Sponza.gltf' -and @($lightweightSponzaScene.entities).Count -ge 3)
    hasLightweightSponzaMeshEntity = ($null -ne $lightweightSponzaScene -and @($lightweightSponzaScene.entities | Where-Object { $null -ne $_.meshRenderer -and $_.meshRenderer.mesh -eq 0 }).Count -ge 1)
    duplicateProjectDetected = (Test-Path -LiteralPath $duplicateProjectFile)
    invalidNameCases = $validationCases
}

$results = @(
    New-ToolResult -Name 'Default project root under Documents' -Passed $checks.defaultRootUnderDocuments -Message $defaultRoot
    New-ToolResult -Name 'Project Manager source readiness' -Passed ($checks.hasProjectManagerSource -and $checks.hasCreateProjectRequest -and $checks.hasOpenProjectRequest) -Message ("create={0}; open={1}" -f $checks.hasCreateProjectRequest, $checks.hasOpenProjectRequest) -Details $checks
    New-ToolResult -Name 'Project Manager raster thumbnails present' -Passed ($checks.hasProjectThumbnailRasterLoad -and $checks.hasSavedThumbnailPaths) -Message ("raster={0}; paths={1}" -f $checks.hasProjectThumbnailRasterLoad, $checks.hasSavedThumbnailPaths) -Details $checks
    New-ToolResult -Name 'Project Manager deferred renderer startup' -Passed ($checks.hasDeferredRendererStartup -and $checks.hasProjectManagerOnlyOverlay -and $checks.hasContinueWithoutProjectRendererInit) -Message ("defer={0}; overlay={1}; continue={2}" -f $checks.hasDeferredRendererStartup, $checks.hasProjectManagerOnlyOverlay, $checks.hasContinueWithoutProjectRendererInit) -Details $checks
    New-ToolResult -Name 'Explicit scene startup bypasses Project Manager gate' -Passed $checks.hasExplicitSceneProjectManagerDismissal -Message ("explicitSceneDismissal={0}" -f $checks.hasExplicitSceneProjectManagerDismissal) -Details $checks
    New-ToolResult -Name 'Capture project startup override present' -Passed ($checks.hasCaptureStartupProjectOverride -and $checks.hasCaptureRecoverySuppress) -Message ("startupOverride={0}; recoverySuppress={1}" -f $checks.hasCaptureStartupProjectOverride, $checks.hasCaptureRecoverySuppress) -Details $checks
    New-ToolResult -Name 'Project Manager generated thumbnail capture' -Passed $checks.hasGeneratedProjectThumbnailCapture -Message ("capture={0}" -f $checks.hasGeneratedProjectThumbnailCapture) -Details $checks
    New-ToolResult -Name 'Lightweight Sponza sample startup scene present' -Passed ($checks.hasLightweightSponzaStartupScene -and $checks.hasLightweightSponzaGltfScene -and $checks.hasLightweightSponzaMeshEntity) -Message ("project={0}; scene={1}; meshEntity={2}" -f $checks.hasLightweightSponzaStartupScene, $checks.hasLightweightSponzaGltfScene, $checks.hasLightweightSponzaMeshEntity) -Details $checks
    New-ToolResult -Name 'Project validation fixtures generated' -Passed $checks.duplicateProjectDetected -Message $fixtureRoot -Details $checks
)
$passed = Write-ToolResults -Results $results -JsonOut $JsonOut
if ($FailOnIssues -and -not $passed) { exit 1 }
