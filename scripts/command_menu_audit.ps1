param(
    [string]$RepoRoot,
    [string]$JsonOut,
    [switch]$FailOnIssues
)

Import-Module (Join-Path $PSScriptRoot 'EditorToolingCommon.psm1') -Force
$RepoRoot = Resolve-EditorToolRepoRoot $RepoRoot

$commandsHeader = Get-Content -Raw -LiteralPath (Join-Path $RepoRoot 'include\rtv\EditorCommands.h')
$commandsCpp = Get-Content -Raw -LiteralPath (Join-Path $RepoRoot 'src\rtv\EditorCommands.cpp')
$dockspace = Get-Content -Raw -LiteralPath (Join-Path $RepoRoot 'src\rtv\EditorDockspace.cpp')
$style = Get-Content -Raw -LiteralPath (Join-Path $RepoRoot 'include\rtv\EditorUiStyle.h')

$enumBlock = [regex]::Match($commandsHeader, 'enum class EditorCommandId[^{]*\{(?<body>.*?)\};', [System.Text.RegularExpressions.RegexOptions]::Singleline).Groups['body'].Value
$enumCommands = @($enumBlock -split ',' | ForEach-Object { $_.Trim() } | Where-Object { $_ -match '^[A-Za-z0-9_]+' } | ForEach-Object { ($_ -split '\s+')[0] })
$registered = @([regex]::Matches($commandsCpp, 'add\(EditorCommandId::([A-Za-z0-9_]+)') | ForEach-Object { $_.Groups[1].Value } | Select-Object -Unique)
$missingRegistry = @($enumCommands | Where-Object { $registered -notcontains $_ })

$requiredMenus = @('File','Create','Engine','Window','Render','Layout')
$menuPresence = @($requiredMenus | ForEach-Object {
    [pscustomobject]@{ menu=$_; present=($dockspace -match ('BeginMenu\("' + [regex]::Escape($_) + '"\)')) }
})

$requiredRenderNames = @('Render current viewport','Render image','Render sequence','Stop render')
$renderPresence = @($requiredRenderNames | ForEach-Object {
    [pscustomobject]@{ command=$_; present=($dockspace -match [regex]::Escape($_) -or $commandsCpp -match [regex]::Escape($_)) }
})

$requiredMenuSections = @(
    'OPEN','SAVE','IMPORT / EXPORT','PROJECT','APPLICATION',
    'ENTITY','3D OBJECT','LIGHT','CAMERA','ENVIRONMENT','ASSET',
    'SETTINGS','ASSET REGISTRY','CACHE','VALIDATION','DIAGNOSTICS',
    'PANELS','DEBUG / ADVANCED','LAYOUT','OUTPUT','PREVIEW','DEBUG VIEWS',
    'WORKSPACE','LAYOUT FILES','APPEARANCE'
)
$sectionPresence = @($requiredMenuSections | ForEach-Object {
    [pscustomobject]@{ section=$_; present=($dockspace -match ('menuSection\("' + [regex]::Escape($_) + '"\)')) }
})

$requiredMenuRows = @(
    'Favorite Scenes','Open Asset...','Save All','Choose Files to Save...','Export All...','Export Selected...',
    'Zip Project','Open Current Project Directory','Recent Projects',
    'Folder / Group','Cube','Sphere','Plane','Cylinder','Cone','Mesh From Asset','Disk Area Light','Sphere Light','Emissive Mesh Light','Cine Camera','Material Instance','Prefab From Selection',
    'Project Settings...','Editor Preferences...','Engine Settings...','Rebuild Asset Registry','Validate Asset References','Clear Derived Data Cache...','Open Cache Directory','Run Validation Suite','Run Current Scene Checks','Open Log Folder','Open Debug Package Folder','Copy System Info',
    'Floating Render Controls','Load Layout...',
    'Pause / Resume Render','Screenshot','High Resolution Render','Capture RenderDoc','Export Debug Views','Export Debug Package','Dump RenderGraph','Profile Current Scene',
    'Default Editor','Content Editing','Lighting','Animation / Timeline','Debug / Profiling','Manage Layouts...'
)
$rowPresence = @($requiredMenuRows | ForEach-Object {
    [pscustomobject]@{ row=$_; present=($dockspace -match [regex]::Escape($_)) }
})

$topMenuGlyphReady = (
    $dockspace -match 'drawMenuItemGlyph' -and
    $dockspace -match 'menuLabelWithGlyphPadding' -and
    $dockspace -match 'commandGlyph' -and
    $dockspace -match 'EditorGlyphIcon::Render'
)
$sceneTabChromeReady = (
    $dockspace -match 'drawSceneTabChrome' -and
    $dockspace -match 'EditorGlyphIcon::SceneFile' -and
    $dockspace -match 'EditorGlyphIcon::Exit' -and
    $dockspace -notmatch 'SmallButton\("x##CloseSceneTab"\)'
)
$legacyIconMatches = @([regex]::Matches(($dockspace + "`n" + $style), 'EditorIcon::|namespace\s+EditorIcon|editorIconForPath|editorIconForEntity') | ForEach-Object { $_.Value } | Select-Object -Unique)
$disabledFutureRows = @([regex]::Matches($dockspace, 'filteredMenuItem\("[^"]+"[^\r\n]+false,\s*false,\s*"[^"]+"') | ForEach-Object { $_.Value })
$disabledReasonReady = (
    $dockspace -match 'menuItemTooltip\(const char\* description, const char\* disabledReason' -and
    $dockspace -match 'Disabled: %s' -and
    $dockspace -match 'editorDrawDisabledRowChrome'
)

$results = @(
    New-ToolResult -Name 'Every command enum has registry entry' -Passed ($missingRegistry.Count -eq 0) -Message ("missing={0}" -f ($missingRegistry -join ', ')) -Details $missingRegistry
    New-ToolResult -Name 'Target top menus present' -Passed (@($menuPresence | Where-Object { -not $_.present }).Count -eq 0) -Message ("missing={0}" -f ((@($menuPresence | Where-Object { -not $_.present }).menu) -join ', ')) -Details $menuPresence
    New-ToolResult -Name 'Render command names present' -Passed (@($renderPresence | Where-Object { -not $_.present }).Count -eq 0) -Message ("missing={0}" -f ((@($renderPresence | Where-Object { -not $_.present }).command) -join ', ')) -Details $renderPresence
    New-ToolResult -Name 'Reference top menu sections present' -Passed (@($sectionPresence | Where-Object { -not $_.present }).Count -eq 0) -Message ("missing={0}" -f ((@($sectionPresence | Where-Object { -not $_.present }).section) -join ', ')) -Details $sectionPresence
    New-ToolResult -Name 'Reference top menu rows present' -Passed (@($rowPresence | Where-Object { -not $_.present }).Count -eq 0) -Message ("missing={0}" -f ((@($rowPresence | Where-Object { -not $_.present }).row) -join ', ')) -Details $rowPresence
    New-ToolResult -Name 'Top menu drawn glyph rows present' -Passed $topMenuGlyphReady -Message ("glyphRows={0}" -f $topMenuGlyphReady) -Details @{ dockspace='src\rtv\EditorDockspace.cpp'; style='include\rtv\EditorUiStyle.h' }
    New-ToolResult -Name 'Scene tab chrome uses drawn close glyph' -Passed $sceneTabChromeReady -Message ("sceneTabChrome={0}" -f $sceneTabChromeReady) -Details @{ dockspace='src\rtv\EditorDockspace.cpp' }
    New-ToolResult -Name 'Disabled future menu rows have reasons' -Passed ($disabledFutureRows.Count -ge 20 -and $disabledReasonReady) -Message ("disabledRows={0}; reasons={1}" -f $disabledFutureRows.Count, $disabledReasonReady) -Details @{ rows=$disabledFutureRows; reasonReady=$disabledReasonReady }
    New-ToolResult -Name 'Legacy bracket menu icon fallbacks removed' -Passed ($legacyIconMatches.Count -eq 0) -Message ("matches={0}" -f ($legacyIconMatches -join ', ')) -Details $legacyIconMatches
)
$passed = Write-ToolResults -Results $results -JsonOut $JsonOut
if ($FailOnIssues -and -not $passed) { exit 1 }
