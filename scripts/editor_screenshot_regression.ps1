param(
    [string]$Baseline = 'docs',
    [string]$Current,
    [string]$JsonOut = 'out\editor_screenshot_regression.json',
    [double]$MseThreshold = 2.0,
    [double]$ChangedPercentThreshold = 1.0,
    [switch]$InventoryOnly,
    [switch]$UpdateBaseline,
    [switch]$RequireEditorStateSet,
    [switch]$RequireReferenceSceneSet,
    [switch]$FailOnDifference
)

Import-Module (Join-Path $PSScriptRoot 'EditorToolingCommon.psm1') -Force
$RepoRoot = Resolve-EditorToolRepoRoot ''

Add-Type -AssemblyName System.Drawing

function Resolve-ToolPath {
    param([string]$Path)
    if ([System.IO.Path]::IsPathRooted($Path)) { return $Path }
    return (Join-Path $RepoRoot $Path)
}

function Get-ToolRelativePath {
    param([string]$BasePath, [string]$FullPath)
    $baseFull = [System.IO.Path]::GetFullPath($BasePath)
    $targetFull = [System.IO.Path]::GetFullPath($FullPath)
    if (!$baseFull.EndsWith([System.IO.Path]::DirectorySeparatorChar)) {
        $baseFull += [System.IO.Path]::DirectorySeparatorChar
    }
    $baseUri = [System.Uri]::new($baseFull)
    $targetUri = [System.Uri]::new($targetFull)
    return [System.Uri]::UnescapeDataString($baseUri.MakeRelativeUri($targetUri).ToString()).Replace('/', [System.IO.Path]::DirectorySeparatorChar)
}

function Get-ImagePairs {
    param([string]$BaselinePath, [string]$CurrentPath)
    $pairs = @()
    if ((Test-Path -LiteralPath $BaselinePath -PathType Container) -and (Test-Path -LiteralPath $CurrentPath -PathType Container)) {
        $baselineFiles = @(Get-ChildItem -LiteralPath $BaselinePath -File -Recurse | Where-Object { $_.Extension -match '^\.(png|jpg|jpeg)$' })
        foreach ($base in $baselineFiles) {
            $relative = Get-ToolRelativePath -BasePath $BaselinePath -FullPath $base.FullName
            $candidate = Join-Path $CurrentPath $relative
            $pairs += [pscustomobject]@{ name=$relative; baseline=$base.FullName; current=$candidate }
        }
    } else {
        $pairs += [pscustomobject]@{ name=(Split-Path -Leaf $BaselinePath); baseline=$BaselinePath; current=$CurrentPath }
    }
    return $pairs
}

function Get-ImageInventory {
    param([string]$Path)
    $files = @()
    if (Test-Path -LiteralPath $Path -PathType Container) {
        $files = @(Get-ChildItem -LiteralPath $Path -File -Recurse | Where-Object { $_.Extension -match '^\.(png|jpg|jpeg)$' })
    } elseif (Test-Path -LiteralPath $Path -PathType Leaf) {
        $files = @(Get-Item -LiteralPath $Path)
    }
    return @($files | ForEach-Object {
        $bitmap = $null
        try {
            $bitmap = [System.Drawing.Bitmap]::new($_.FullName)
            [pscustomobject]@{
                name = $_.Name
                path = $_.FullName
                width = $bitmap.Width
                height = $bitmap.Height
                bytes = $_.Length
                readable = $true
                message = 'ok'
            }
        } catch {
            [pscustomobject]@{
                name = $_.Name
                path = $_.FullName
                width = 0
                height = 0
                bytes = $_.Length
                readable = $false
                message = $_.Exception.Message
            }
        } finally {
            if ($null -ne $bitmap) { $bitmap.Dispose() }
        }
    })
}

function Get-RequiredEditorStateFiles {
    return @(
        'project_manager_home.png',
        'default_editor.png',
        'content_active.png',
        'timeline_active.png',
        'render_settings_active.png',
        'selected_camera.png',
        'selected_light.png'
    )
}

function Test-RequiredEditorStateSet {
    param([string]$Path)
    $missing = @()
    foreach ($name in Get-RequiredEditorStateFiles) {
        $candidate = Join-Path $Path $name
        if (!(Test-Path -LiteralPath $candidate -PathType Leaf)) {
            $missing += $name
        }
    }
    return [pscustomobject]@{ path=$Path; missing=$missing; complete=($missing.Count -eq 0) }
}

function Get-RequiredReferenceSceneFiles {
    return @(
        'sponza_default.png',
        'sponza_content_active.png',
        'sponza_timeline_active.png',
        'sponza_selected_sun.png'
    )
}

function Test-RequiredReferenceSceneSet {
    param([string]$Path)
    $missing = @()
    foreach ($name in Get-RequiredReferenceSceneFiles) {
        $candidate = Join-Path $Path $name
        if (!(Test-Path -LiteralPath $candidate -PathType Leaf)) {
            $missing += $name
        }
    }
    return [pscustomobject]@{ path=$Path; missing=$missing; complete=($missing.Count -eq 0) }
}

function Copy-BaselineImages {
    param([string]$CurrentPath, [string]$BaselinePath)
    if (!(Test-Path -LiteralPath $CurrentPath -PathType Container)) {
        throw "Current screenshot directory is required for -UpdateBaseline: $CurrentPath"
    }
    New-Item -ItemType Directory -Force $BaselinePath | Out-Null
    $copied = @()
    foreach ($image in @(Get-ChildItem -LiteralPath $CurrentPath -File -Recurse | Where-Object { $_.Extension -match '^\.(png|jpg|jpeg)$' })) {
        $relative = Get-ToolRelativePath -BasePath $CurrentPath -FullPath $image.FullName
        $dest = Join-Path $BaselinePath $relative
        $destDir = Split-Path -Parent $dest
        if (![string]::IsNullOrWhiteSpace($destDir)) {
            New-Item -ItemType Directory -Force $destDir | Out-Null
        }
        Copy-Item -LiteralPath $image.FullName -Destination $dest -Force
        $copied += [pscustomobject]@{ name=$relative; source=$image.FullName; baseline=$dest }
    }
    return $copied
}

function Compare-Images {
    param([string]$Name, [string]$BaselinePath, [string]$CurrentPath)
    if (!(Test-Path -LiteralPath $BaselinePath)) {
        return [pscustomobject]@{ name=$Name; passed=$false; message='baseline missing'; baseline=$BaselinePath; current=$CurrentPath }
    }
    if (!(Test-Path -LiteralPath $CurrentPath)) {
        return [pscustomobject]@{ name=$Name; passed=$false; message='current missing'; baseline=$BaselinePath; current=$CurrentPath }
    }

    $base = [System.Drawing.Bitmap]::new($BaselinePath)
    $curr = [System.Drawing.Bitmap]::new($CurrentPath)
    try {
        if ($base.Width -ne $curr.Width -or $base.Height -ne $curr.Height) {
            return [pscustomobject]@{
                name=$Name; passed=$false; message='dimension mismatch'; baseline=$BaselinePath; current=$CurrentPath;
                baselineSize=@($base.Width,$base.Height); currentSize=@($curr.Width,$curr.Height)
            }
        }
        $sumSquared = 0.0
        $changed = 0
        $maxError = 0
        $pixels = [int64]$base.Width * [int64]$base.Height
        for ($y = 0; $y -lt $base.Height; ++$y) {
            for ($x = 0; $x -lt $base.Width; ++$x) {
                $a = $base.GetPixel($x, $y)
                $b = $curr.GetPixel($x, $y)
                $dr = [math]::Abs([int]$a.R - [int]$b.R)
                $dg = [math]::Abs([int]$a.G - [int]$b.G)
                $db = [math]::Abs([int]$a.B - [int]$b.B)
                $da = [math]::Abs([int]$a.A - [int]$b.A)
                $localMax = [math]::Max([math]::Max($dr, $dg), [math]::Max($db, $da))
                if ($localMax -gt 1) { ++$changed }
                if ($localMax -gt $maxError) { $maxError = $localMax }
                $sumSquared += ($dr * $dr) + ($dg * $dg) + ($db * $db) + ($da * $da)
            }
        }
        $mse = $sumSquared / ([double]$pixels * 4.0)
        $changedPercent = if ($pixels -gt 0) { 100.0 * [double]$changed / [double]$pixels } else { 0.0 }
        $passed = ($mse -le $MseThreshold -and $changedPercent -le $ChangedPercentThreshold)
        return [pscustomobject]@{
            name=$Name
            passed=$passed
            baseline=$BaselinePath
            current=$CurrentPath
            width=$base.Width
            height=$base.Height
            mse=$mse
            changedPercent=$changedPercent
            maxError=$maxError
            thresholds=[pscustomobject]@{ mse=$MseThreshold; changedPercent=$ChangedPercentThreshold }
        }
    } finally {
        $base.Dispose()
        $curr.Dispose()
    }
}

$baselinePath = Resolve-ToolPath $Baseline
$baselineInventory = @(Get-ImageInventory -Path $baselinePath)
$captureSource = Get-Content -Raw -LiteralPath (Join-Path $RepoRoot 'scripts\capture_editor_screenshot.ps1')
$stateCaptureSource = Get-Content -Raw -LiteralPath (Join-Path $RepoRoot 'scripts\capture_editor_ui_states.ps1')
$multiStateCaptureDetails = [pscustomobject]@{
    selectHierarchyRow = ($captureSource -match 'SelectHierarchyRow')
    cursorClick = ($captureSource -match 'SetCursorPos' -and $captureSource -match 'mouse_event')
    selectedCamera = ($stateCaptureSource -match 'selected-camera')
    selectedLight = ($stateCaptureSource -match 'selected-light')
    projectManagerHome = ($stateCaptureSource -match 'project-manager-home')
    defaultContentFocus = ($stateCaptureSource -match "name='default'[^`r`n]+focusWindow='Content'")
    contentActive = ($stateCaptureSource -match 'content-active')
    timelineActive = ($stateCaptureSource -match 'timeline-active')
    renderSettingsActive = ($stateCaptureSource -match 'render-settings-active')
    explicitWindowSizeCapture = ($captureSource -match '\[int\]\$WindowWidth' -and $captureSource -match '\[int\]\$WindowHeight' -and $stateCaptureSource -match 'WindowWidth' -and $stateCaptureSource -match 'WindowHeight')
    explicitGltfCapture = ($captureSource -match '\[string\]\$Gltf' -and $captureSource -match '--gltf')
    disablesOpenLastForExplicitScene = ($captureSource -match 'openLastProject = \(\$UseProjectStartup -and !\$ProjectManagerStartup\)')
    deterministicProjectStartup = ($captureSource -match 'RTV_EDITOR_STARTUP_PROJECT' -and $captureSource -match 'RTV_EDITOR_SUPPRESS_RECOVERY_PROMPT')
}
$multiStateCaptureReady = ($multiStateCaptureDetails.selectHierarchyRow -and $multiStateCaptureDetails.cursorClick -and $multiStateCaptureDetails.selectedCamera -and $multiStateCaptureDetails.selectedLight -and $multiStateCaptureDetails.projectManagerHome -and $multiStateCaptureDetails.defaultContentFocus -and $multiStateCaptureDetails.contentActive -and $multiStateCaptureDetails.timelineActive -and $multiStateCaptureDetails.renderSettingsActive -and $multiStateCaptureDetails.explicitWindowSizeCapture -and $multiStateCaptureDetails.explicitGltfCapture -and $multiStateCaptureDetails.disablesOpenLastForExplicitScene -and $multiStateCaptureDetails.deterministicProjectStartup)
if ($UpdateBaseline) {
    if ([string]::IsNullOrWhiteSpace($Current)) {
        throw '-UpdateBaseline requires -Current to point at reviewed current screenshots.'
    }
    $currentPath = Resolve-ToolPath $Current
    $copied = @(Copy-BaselineImages -CurrentPath $currentPath -BaselinePath $baselinePath)
    $baselineInventory = @(Get-ImageInventory -Path $baselinePath)
    $stateSet = Test-RequiredEditorStateSet -Path $baselinePath
    $referenceSceneSet = Test-RequiredReferenceSceneSet -Path $baselinePath
    $results = @(
        New-ToolResult -Name 'Baseline update explicitly requested' -Passed $true -Message ("copied={0}; baseline={1}" -f $copied.Count, $baselinePath) -Details $copied
        New-ToolResult -Name 'Required editor state baseline set present' -Passed (!$RequireEditorStateSet -or $stateSet.complete) -Message ("complete={0}; missing={1}" -f $stateSet.complete, ($stateSet.missing -join ', ')) -Details $stateSet
        New-ToolResult -Name 'Required reference scene baseline set present' -Passed (!$RequireReferenceSceneSet -or $referenceSceneSet.complete) -Message ("complete={0}; missing={1}" -f $referenceSceneSet.complete, ($referenceSceneSet.missing -join ', ')) -Details $referenceSceneSet
        New-ToolResult -Name 'Multi-state screenshot capture harness present' -Passed $multiStateCaptureReady -Message ("multiState={0}" -f $multiStateCaptureReady) -Details $multiStateCaptureDetails
    )
} elseif ($InventoryOnly -or [string]::IsNullOrWhiteSpace($Current)) {
    $readableInventory = @($baselineInventory | Where-Object { $_.readable })
    $stateSet = if (Test-Path -LiteralPath $baselinePath -PathType Container) { Test-RequiredEditorStateSet -Path $baselinePath } else { [pscustomobject]@{ path=$baselinePath; missing=(Get-RequiredEditorStateFiles); complete=$false } }
    $referenceSceneSet = if (Test-Path -LiteralPath $baselinePath -PathType Container) { Test-RequiredReferenceSceneSet -Path $baselinePath } else { [pscustomobject]@{ path=$baselinePath; missing=(Get-RequiredReferenceSceneFiles); complete=$false } }
    $referenceLike = @($readableInventory | Where-Object { $_.name -match '2026-05-31|reference|screenshot|Ảnh chụp' })
    $results = @(
        New-ToolResult -Name 'Reference screenshot inventory' -Passed ($readableInventory.Count -gt 0) -Message ("readable={0}; files={1}" -f $readableInventory.Count, $baselineInventory.Count) -Details $baselineInventory
        New-ToolResult -Name 'Reference screenshot set recognized' -Passed (($referenceLike.Count -ge 4) -or ($RequireEditorStateSet -and $stateSet.complete) -or ($RequireReferenceSceneSet -and $referenceSceneSet.complete)) -Message ("referenceLike={0}; editorStateSet={1}; referenceSceneSet={2}" -f $referenceLike.Count, $stateSet.complete, $referenceSceneSet.complete) -Details ([pscustomobject]@{ referenceLike=$referenceLike; editorStateSet=$stateSet; referenceSceneSet=$referenceSceneSet })
        New-ToolResult -Name 'Multi-state screenshot capture harness present' -Passed $multiStateCaptureReady -Message ("multiState={0}" -f $multiStateCaptureReady) -Details $multiStateCaptureDetails
        New-ToolResult -Name 'Required editor state baseline set present' -Passed (!$RequireEditorStateSet -or $stateSet.complete) -Message ("complete={0}; missing={1}" -f $stateSet.complete, ($stateSet.missing -join ', ')) -Details $stateSet
        New-ToolResult -Name 'Required reference scene baseline set present' -Passed (!$RequireReferenceSceneSet -or $referenceSceneSet.complete) -Message ("complete={0}; missing={1}" -f $referenceSceneSet.complete, ($referenceSceneSet.missing -join ', ')) -Details $referenceSceneSet
    )
} else {
    $currentPath = Resolve-ToolPath $Current
    $baselineStateSet = if (Test-Path -LiteralPath $baselinePath -PathType Container) { Test-RequiredEditorStateSet -Path $baselinePath } else { [pscustomobject]@{ path=$baselinePath; missing=(Get-RequiredEditorStateFiles); complete=$false } }
    $currentStateSet = if (Test-Path -LiteralPath $currentPath -PathType Container) { Test-RequiredEditorStateSet -Path $currentPath } else { [pscustomobject]@{ path=$currentPath; missing=(Get-RequiredEditorStateFiles); complete=$false } }
    $baselineReferenceSceneSet = if (Test-Path -LiteralPath $baselinePath -PathType Container) { Test-RequiredReferenceSceneSet -Path $baselinePath } else { [pscustomobject]@{ path=$baselinePath; missing=(Get-RequiredReferenceSceneFiles); complete=$false } }
    $currentReferenceSceneSet = if (Test-Path -LiteralPath $currentPath -PathType Container) { Test-RequiredReferenceSceneSet -Path $currentPath } else { [pscustomobject]@{ path=$currentPath; missing=(Get-RequiredReferenceSceneFiles); complete=$false } }
    $comparisons = @(Get-ImagePairs -BaselinePath $baselinePath -CurrentPath $currentPath | ForEach-Object {
        Compare-Images -Name $_.name -BaselinePath $_.baseline -CurrentPath $_.current
    })
    $results = @(
        New-ToolResult -Name 'Reference screenshot inventory' -Passed ($baselineInventory.Count -gt 0) -Message ("images={0}" -f $baselineInventory.Count) -Details $baselineInventory
        New-ToolResult -Name 'Required editor state baseline set present' -Passed (!$RequireEditorStateSet -or ($baselineStateSet.complete -and $currentStateSet.complete)) -Message ("baselineComplete={0}; currentComplete={1}; baselineMissing={2}; currentMissing={3}" -f $baselineStateSet.complete, $currentStateSet.complete, ($baselineStateSet.missing -join ', '), ($currentStateSet.missing -join ', ')) -Details ([pscustomobject]@{ baseline=$baselineStateSet; current=$currentStateSet })
        New-ToolResult -Name 'Required reference scene baseline set present' -Passed (!$RequireReferenceSceneSet -or ($baselineReferenceSceneSet.complete -and $currentReferenceSceneSet.complete)) -Message ("baselineComplete={0}; currentComplete={1}; baselineMissing={2}; currentMissing={3}" -f $baselineReferenceSceneSet.complete, $currentReferenceSceneSet.complete, ($baselineReferenceSceneSet.missing -join ', '), ($currentReferenceSceneSet.missing -join ', ')) -Details ([pscustomobject]@{ baseline=$baselineReferenceSceneSet; current=$currentReferenceSceneSet })
        New-ToolResult -Name 'Screenshot pairs compared' -Passed ($comparisons.Count -gt 0) -Message ("pairs={0}" -f $comparisons.Count) -Details $comparisons
        New-ToolResult -Name 'Screenshots within thresholds' -Passed (@($comparisons | Where-Object { -not $_.passed }).Count -eq 0) -Message ("failed={0}" -f @($comparisons | Where-Object { -not $_.passed }).Count) -Details $comparisons
    )
}
$out = if ([System.IO.Path]::IsPathRooted($JsonOut)) { $JsonOut } else { Join-Path $RepoRoot $JsonOut }
$passed = Write-ToolResults -Results $results -JsonOut $out
if ($FailOnDifference -and -not $passed) { exit 1 }
