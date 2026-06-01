param(
    [string]$RepoRoot,
    [string]$Scene = 'scenes\validation\cornell.rtlevel',
    [string]$JsonOut,
    [switch]$FailOnMissing
)

Import-Module (Join-Path $PSScriptRoot 'EditorToolingCommon.psm1') -Force
$RepoRoot = Resolve-EditorToolRepoRoot $RepoRoot

$requiredComponents = @(
    'Transform',
    'Camera',
    'Light',
    'Sun',
    'EnvironmentLight',
    'SkyAtmosphere',
    'HeightFog',
    'VolumetricCloud',
    'PostProcessVolume',
    'MeshRenderer',
    'CameraPostProcess'
)

$sourceText = (Get-Content -Raw -LiteralPath (Join-Path $RepoRoot 'src\rtv\EditorLayer.cpp')) + "`n" +
    (Get-Content -Raw -LiteralPath (Join-Path $RepoRoot 'src\rtv\InspectorPanel.cpp')) + "`n" +
    (Get-Content -Raw -LiteralPath (Join-Path $RepoRoot 'include\rtv\EditorPanels.h'))
$coverage = @()
foreach ($component in $requiredComponents) {
    $patterns = switch ($component) {
        'Transform' { @('Transform', 'setEntityTransform') }
        'CameraPostProcess' { @('DOF|Bloom|Vignetting|Film grain|Film Grain|Color correction') }
        default { @($component) }
    }
    $found = $false
    foreach ($pattern in $patterns) {
        if ($sourceText -match $pattern) { $found = $true; break }
    }
    $coverage += [pscustomobject]@{ component=$component; sourceFound=$found }
}

$scenePath = if ([System.IO.Path]::IsPathRooted($Scene)) { $Scene } else { Join-Path $RepoRoot $Scene }
$sceneComponents = @()
if (Test-Path -LiteralPath $scenePath) {
    $sceneJson = Read-EditorJson -Path $scenePath
    foreach ($entity in @($sceneJson.entities)) {
        foreach ($name in @('camera','sun','light','meshRenderer','environmentLight','skyAtmosphere','heightFog','volumetricCloud','postProcessVolume')) {
            if ($null -ne $entity.$name) {
                $sceneComponents += [pscustomobject]@{ entity=$entity.name; component=$name }
            }
        }
    }
}

$missing = @($coverage | Where-Object { -not $_.sourceFound })
$inspectorSource = Get-Content -Raw -LiteralPath (Join-Path $RepoRoot 'src\rtv\InspectorPanel.cpp')
$sceneComponentsSource = Get-Content -Raw -LiteralPath (Join-Path $RepoRoot 'include\rtv\SceneComponents.h')
$sceneDocumentSource = Get-Content -Raw -LiteralPath (Join-Path $RepoRoot 'src\rtv\SceneDocument.cpp')
$propertyRowChecks = [ordered]@{
    hasPropertyRowHelper = ($inspectorSource -match 'beginInspectorPropertyRow' -and $inspectorSource -match 'inspectorCheckboxRow' -and $inspectorSource -match 'inspectorDragFloatRow')
    usesLightPropertyRows = ($inspectorSource -match 'inspectorComboRow\("Type"' -and $inspectorSource -match 'inspectorReadonlyRow\("Units"' -and $inspectorSource -match 'inspectorCheckboxRow\("Visible To Camera"')
    usesSunPropertyRows = ($inspectorSource -match 'inspectorReadonlyRow\("Light Type"' -and $inspectorSource -match 'inspectorDragIntRow\("Shadow Bounces"')
    usesCameraPostPropertyRows = ($inspectorSource -match 'inspectorCheckboxRow\("Bloom"' -and $inspectorSource -match 'inspectorCheckboxRow\("Film grain"')
    usesTransformPropertyRows = ($inspectorSource -match 'inspectorDragFloat3Row\("Position"' -and $inspectorSource -match 'inspectorDragFloat3Row\("Rotation"' -and $inspectorSource -match 'inspectorDragFloat3Row\("Scale"')
    usesCameraPropertyRows = ($inspectorSource -match 'inspectorSliderFloatRow\("Vertical FOV"' -and $inspectorSource -match 'inspectorCheckboxRow\("Active Camera"' -and $inspectorSource -match 'inspectorDragFloatRow\("DOF Focus Distance"')
    usesMeshPropertyRows = ($inspectorSource -match 'inspectorCheckboxRow\("Cast Shadow"' -and $inspectorSource -match 'inspectorReadonlyRow\("Mesh Asset"' -and $inspectorSource -match 'inspectorComboRow\("Alpha Mode"')
    usesWorldPropertyRows = ($inspectorSource -match 'inspectorDragFloatRow\("Environment Intensity"' -and $inspectorSource -match 'inspectorSliderFloatRow\("Mie Anisotropy"' -and $inspectorSource -match 'inspectorDragFloatRow\("Density"' -and $inspectorSource -match 'inspectorSliderFloatRow\("Cloud Density"' -and $inspectorSource -match 'inspectorCheckboxRow\("Post Process Enabled"')
    usesGlyphActionMenus = ($inspectorSource -match 'editorGlyphMenuItem\(EditorGlyphIcon::Add, "Duplicate Entity"' -and $inspectorSource -match 'editorGlyphMenuItem\(EditorGlyphIcon::Trash, "Remove Component"' -and $inspectorSource -match 'addComponentMenuItem\("Camera".*EditorGlyphIcon::Camera')
    usesStateCardChrome = ($inspectorSource -match 'drawInspectorStateCard' -and $inspectorSource -match 'No selection' -and $inspectorSource -match 'Bulk component editing')
    usesComponentHeaderChrome = ($inspectorSource -match 'drawInspectorComponentHeader' -and $inspectorSource -match 'inspectorComponentHeaderHeight' -and $inspectorSource -match '"Transform", "Local position' -and $inspectorSource -match '"Camera", "Lens' -and $inspectorSource -match '"Mesh Renderer", "Visibility')
    usesHeaderActionPopup = ($inspectorSource -match 'drawInspectorComponentActionsPopup' -and $inspectorSource -match 'ComponentActions' -and $inspectorSource -match 'inspectorComponentActionSize')
    usesLockedStateBanner = ($inspectorSource -match 'drawInspectorLockedBanner' -and $inspectorSource -match 'Entity locked' -and $inspectorSource -notmatch 'This entity is locked')
    usesIconAddComponentButtons = ($inspectorSource -match 'drawInspectorAddComponentButton' -and $inspectorSource -match 'InspectorAddCamera' -and $inspectorSource -match 'InspectorAddPostProcessVolume' -and $inspectorSource -notmatch 'ImGui::Button\("Light"')
    removesRawComponentRemoveButtons = ($inspectorSource -notmatch 'SmallButton\("Remove ')
}
$storedUnsupportedChecks = [ordered]@{
    lightAuthoredFields = ($sceneComponentsSource -match 'exposureMultiplier' -and $sceneComponentsSource -match 'visibleToCamera' -and $sceneComponentsSource -match 'castVolumetricShadows' -and $sceneComponentsSource -match 'iesProfile')
    sunAuthoredFields = ($sceneComponentsSource -match 'shadowBounces' -and $sceneComponentsSource -match 'volumetricShadowBounces' -and $sceneComponentsSource -match 'castSurfaceShadows')
    cameraPostAuthoredFields = ($sceneComponentsSource -match 'bloomEnabled' -and $sceneComponentsSource -match 'vignettingEnabled' -and $sceneComponentsSource -match 'filmGrainEnabled')
    serializedFields = ($sceneDocumentSource -match '"bloomEnabled"' -and $sceneDocumentSource -match '"castVolumetricShadows"' -and $sceneDocumentSource -match '"shadowBounces"' -and $sceneDocumentSource -match '"iesProfile"')
}
$results = @(
    New-ToolResult -Name 'Inspector component source coverage' -Passed ($missing.Count -eq 0) -Message ("missing={0}" -f ($missing.component -join ', ')) -Details $coverage
    New-ToolResult -Name 'Inspector shared property rows present' -Passed ($propertyRowChecks.Values -notcontains $false) -Message ("helpers={0}; transform={1}; camera={2}; light={3}; sun={4}; mesh={5}; world={6}; cameraPost={7}; glyphMenus={8}; stateCard={9}; headers={10}; headerActions={11}; lockedBanner={12}; addButtons={13}; rawRemoveGone={14}" -f $propertyRowChecks.hasPropertyRowHelper, $propertyRowChecks.usesTransformPropertyRows, $propertyRowChecks.usesCameraPropertyRows, $propertyRowChecks.usesLightPropertyRows, $propertyRowChecks.usesSunPropertyRows, $propertyRowChecks.usesMeshPropertyRows, $propertyRowChecks.usesWorldPropertyRows, $propertyRowChecks.usesCameraPostPropertyRows, $propertyRowChecks.usesGlyphActionMenus, $propertyRowChecks.usesStateCardChrome, $propertyRowChecks.usesComponentHeaderChrome, $propertyRowChecks.usesHeaderActionPopup, $propertyRowChecks.usesLockedStateBanner, $propertyRowChecks.usesIconAddComponentButtons, $propertyRowChecks.removesRawComponentRemoveButtons) -Details $propertyRowChecks
    New-ToolResult -Name 'Inspector authored unsupported fields persist' -Passed ($storedUnsupportedChecks.Values -notcontains $false) -Message ("light={0}; sun={1}; cameraPost={2}; serialized={3}" -f $storedUnsupportedChecks.lightAuthoredFields, $storedUnsupportedChecks.sunAuthoredFields, $storedUnsupportedChecks.cameraPostAuthoredFields, $storedUnsupportedChecks.serializedFields) -Details $storedUnsupportedChecks
    New-ToolResult -Name 'Scene component inventory' -Passed $true -Message ("components={0}" -f $sceneComponents.Count) -Details $sceneComponents
)
$passed = Write-ToolResults -Results $results -JsonOut $JsonOut
if ($FailOnMissing -and -not $passed) { exit 1 }
