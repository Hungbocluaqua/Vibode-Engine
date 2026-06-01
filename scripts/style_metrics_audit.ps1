param(
    [string]$RepoRoot,
    [string]$JsonOut,
    [switch]$FailOnIssues
)

Import-Module (Join-Path $PSScriptRoot 'EditorToolingCommon.psm1') -Force
$RepoRoot = Resolve-EditorToolRepoRoot $RepoRoot

$sourceFiles = @(Get-ChildItem -LiteralPath (Join-Path $RepoRoot 'src') -Recurse -File -Include *.cpp,*.h) + @(Get-ChildItem -LiteralPath (Join-Path $RepoRoot 'include') -Recurse -File -Include *.cpp,*.h)
$style = Get-Content -Raw -LiteralPath (Join-Path $RepoRoot 'include\rtv\EditorUiStyle.h')
$hierarchy = Get-Content -Raw -LiteralPath (Join-Path $RepoRoot 'src\rtv\SceneHierarchyPanel.cpp')
$content = Get-Content -Raw -LiteralPath (Join-Path $RepoRoot 'src\rtv\AssetBrowserPanel.cpp')
$dockspace = Get-Content -Raw -LiteralPath (Join-Path $RepoRoot 'src\rtv\EditorDockspace.cpp')
$editorLayer = Get-Content -Raw -LiteralPath (Join-Path $RepoRoot 'src\rtv\EditorLayer.cpp')
$uiOverlay = Get-Content -Raw -LiteralPath (Join-Path $RepoRoot 'src\rtv\UiOverlay.cpp')
$inspector = Get-Content -Raw -LiteralPath (Join-Path $RepoRoot 'src\rtv\InspectorPanel.cpp')
$viewport = Get-Content -Raw -LiteralPath (Join-Path $RepoRoot 'src\rtv\ViewportPanel.cpp')
$metrics = @()
foreach ($file in $sourceFiles) {
    $lineNo = 0
    foreach ($line in Get-Content -LiteralPath $file.FullName) {
        ++$lineNo
        if ($line -match 'ImGuiStyleVar_([A-Za-z0-9_]+).*?([0-9]+\.[0-9]+)f?') {
            $metrics += [pscustomobject]@{ file=$file.FullName; line=$lineNo; kind='styleVar'; name=$Matches[1]; value=[double]$Matches[2]; source=$line.Trim() }
        } elseif ($line -match 'ImGuiCol_([A-Za-z0-9_]+)') {
            $metrics += [pscustomobject]@{ file=$file.FullName; line=$lineNo; kind='color'; name=$Matches[1]; value=$null; source=$line.Trim() }
        } elseif ($line -match '(RowHeight|IconSize|TabHeight|ToolbarHeight|Splitter|Padding|Spacing)') {
            $metrics += [pscustomobject]@{ file=$file.FullName; line=$lineNo; kind='namedMetric'; name=$Matches[1]; value=$null; source=$line.Trim() }
        }
    }
}

$requiredMetricNames = @('RowHeight','IconSize','TabHeight','ToolbarHeight')
$hasNamed = @($requiredMetricNames | ForEach-Object {
    [pscustomobject]@{ name=$_; present=(@($metrics | Where-Object { $_.source -match $_.name }).Count -gt 0) }
})

$rowStyleChecks = @(
    [pscustomobject]@{ name='hierarchyRowHeight'; present=($style -match 'hierarchyRowHeight') }
    [pscustomobject]@{ name='hierarchyIndentSpacing'; present=($style -match 'hierarchyIndentSpacing') }
    [pscustomobject]@{ name='hierarchyIconSize'; present=($style -match 'hierarchyIconSize') }
    [pscustomobject]@{ name='hierarchyRowRightFadeWidth'; present=($style -match 'hierarchyRowRightFadeWidth') }
    [pscustomobject]@{ name='contentRowHeight'; present=($style -match 'contentRowHeight') }
    [pscustomobject]@{ name='contentThreePaneRatios'; present=($style -match 'contentTreePanelRatio' -and $style -match 'contentDetailsPanelRatio' -and $content -match 'EditorUiMetric::contentListMinWidth') }
    [pscustomobject]@{ name='inspectorComponentHeaderHeight'; present=($style -match 'inspectorComponentHeaderHeight') }
    [pscustomobject]@{ name='inspectorComponentHeaderIconSize'; present=($style -match 'inspectorComponentHeaderIconSize') }
    [pscustomobject]@{ name='inspectorComponentActionSize'; present=($style -match 'inspectorComponentActionSize') }
    [pscustomobject]@{ name='viewportOverlayPaddingX'; present=($style -match 'viewportOverlayPaddingX') }
    [pscustomobject]@{ name='viewportOverlayRounding'; present=($style -match 'viewportOverlayRounding') }
    [pscustomobject]@{ name='viewportOverlayColors'; present=($style -match 'editorViewportOverlayBgColor' -and $style -match 'editorViewportOverlayBorderColor') }
    [pscustomobject]@{ name='iconTextSpacingMetrics'; present=($style -match 'glyphLabelPrefixSpaces' -and $style -match 'iconTextButtonTextOffsetX' -and $style -match 'iconTextReadoutTextGap') }
    [pscustomobject]@{ name='editorSelectedRowColor'; present=($style -match 'editorSelectedRowColor') }
    [pscustomobject]@{ name='editorPushRowSelectionStyle'; present=($style -match 'editorPushRowSelectionStyle') }
    [pscustomobject]@{ name='editorPreRowBandStyle'; present=($style -match 'editorDrawPreRowBand' -and $style -match 'editorRowBandColor' -and $hierarchy -match 'editorDrawPreRowBand' -and $content -match 'editorDrawPreRowBand') }
    [pscustomobject]@{ name='hierarchyUsesSharedRowStyle'; present=($hierarchy -match 'editorPushRowSelectionStyle') }
    [pscustomobject]@{ name='contentUsesSharedRowStyle'; present=($content -match 'editorPushRowSelectionStyle') }
    [pscustomobject]@{ name='inspectorUsesHeaderMetrics'; present=($inspector -match 'EditorUiMetric::inspectorComponentHeaderHeight' -and $inspector -match 'EditorUiMetric::inspectorComponentHeaderIconSize' -and $inspector -match 'EditorUiMetric::inspectorComponentActionSize') }
    [pscustomobject]@{ name='viewportUsesOverlayChrome'; present=($viewport -match 'drawViewportOverlayBackdrop' -and $viewport -match 'ChannelsSplit' -and $viewport -match 'editorViewportOverlayBgColor') }
)

$dockChromeChecks = @(
    [pscustomobject]@{ name='dockRightPanelRatio'; present=($style -match 'dockRightPanelRatio') }
    [pscustomobject]@{ name='dockBottomPanelRatio'; present=($style -match 'dockBottomPanelRatio') }
    [pscustomobject]@{ name='dockRightInspectorRatio'; present=($style -match 'dockRightInspectorRatio') }
    [pscustomobject]@{ name='dockTabRounding'; present=($style -match 'dockTabRounding') }
    [pscustomobject]@{ name='dockTabIconSize'; present=($style -match 'dockTabIconSize') }
    [pscustomobject]@{ name='dockTabIconPaddingX'; present=($style -match 'dockTabIconPaddingX') }
    [pscustomobject]@{ name='dockPanelBorderThickness'; present=($style -match 'dockPanelBorderThickness') }
    [pscustomobject]@{ name='dockPanelActiveAccentWidth'; present=($style -match 'dockPanelActiveAccentWidth') }
    [pscustomobject]@{ name='dockPanelBorderColor'; present=($style -match 'editorPanelBorderColor') }
    [pscustomobject]@{ name='dockPanelSplitterColor'; present=($style -match 'editorPanelSplitterColor' -and $style -match 'editorPanelSplitterHoveredColor') }
    [pscustomobject]@{ name='dockWindowTitleIds'; present=($style -match 'EditorDockWindowTitle' -and $style -match '###Scene' -and $style -match '###Render Settings') }
    [pscustomobject]@{ name='dockTabColorsPushed'; present=($dockspace -match 'ImGuiCol_TabActive' -and $dockspace -match 'ImGuiCol_TabUnfocusedActive') }
    [pscustomobject]@{ name='dockPanelChromeOverlay'; present=($dockspace -match 'drawDockPanelChromeOverlays' -and $dockspace -match 'OuterRectClipped' -and $dockspace -match 'dockPanelBorderThickness' -and $dockspace -match 'dockPanelActiveAccentWidth') }
    [pscustomobject]@{ name='dockSplitterColorsUseStyle'; present=($dockspace -match 'editorPanelSplitterColor' -and $dockspace -match 'editorPanelSplitterHoveredColor' -and $dockspace -match 'editorPanelActiveAccentColor') }
    [pscustomobject]@{ name='dockSplitRatiosUseStyle'; present=($dockspace -match 'EditorUiMetric::dockRightPanelRatio' -and $dockspace -match 'EditorUiMetric::dockBottomPanelRatio' -and $dockspace -match 'EditorUiMetric::dockRightInspectorRatio') }
    [pscustomobject]@{ name='nativeDockTabIconOverlay'; present=($dockspace -match 'drawDockTabIconOverlays' -and $dockspace -match 'FindWindowByID' -and $dockspace -match 'DockNode->TabBar' -and ($dockspace -match 'editorDrawIconGlyph' -or $dockspace -match 'editorDrawTablerIconGlyph')) }
    [pscustomobject]@{ name='knownDockTabIconMappings'; present=($dockspace -match '"Scene", EditorGlyphIcon::Sky' -and $dockspace -match '"Hierarchy", EditorGlyphIcon::Hierarchy' -and $dockspace -match '"Render Settings", EditorGlyphIcon::ViewSettings' -and $dockspace -match '"Inspector", EditorGlyphIcon::Details' -and $dockspace -match '"Content", EditorGlyphIcon::Folder' -and $dockspace -match '"Timeline", EditorGlyphIcon::Timeline' -and $dockspace -match '"Log", EditorGlyphIcon::List') }
)

$disabledStyleChecks = @(
    [pscustomobject]@{ name='disabledTextColor'; present=($style -match 'editorDisabledTextColor') }
    [pscustomobject]@{ name='disabledIconTint'; present=($style -match 'editorDisabledIconTint') }
    [pscustomobject]@{ name='disabledRowAccentColor'; present=($style -match 'editorDisabledRowAccentColor') }
    [pscustomobject]@{ name='disabledRowChrome'; present=($style -match 'editorDrawDisabledRowChrome') }
    [pscustomobject]@{ name='disabledTextPushPop'; present=($style -match 'editorPushDisabledTextStyle' -and $style -match 'editorPopDisabledTextStyle') }
    [pscustomobject]@{ name='glyphMenuDisabledChrome'; present=($style -match 'editorGlyphMenuItem' -and $style -match 'editorDrawDisabledRowChrome\(min, max\)' -and $style -match 'editorDisabledIconTint\(\)') }
    [pscustomobject]@{ name='glyphSubmenuChrome'; present=($style -match 'editorGlyphBeginMenu' -and $style -match 'BeginMenu\(padded\.c_str\(\), enabled\)' -and $hierarchy -match 'editorGlyphBeginMenu\(EditorGlyphIcon::Add, "Create"' -and $hierarchy -match 'editorGlyphBeginMenu\(EditorGlyphIcon::Add, "Create Child"' -and $inspector -match 'editorGlyphBeginMenu\(EditorGlyphIcon::Add, "Add Component"') }
    [pscustomobject]@{ name='topMenuDisabledChrome'; present=($dockspace -match 'drawMenuItemGlyph' -and $dockspace -match 'editorDrawDisabledRowChrome\(min, max\)' -and $dockspace -match 'editorDisabledIconTint\(\)') }
)

$sceneTabStyleChecks = @(
    [pscustomobject]@{ name='sceneTabWidthMetrics'; present=($style -match 'sceneTabMinWidth' -and $style -match 'sceneTabMaxWidth' -and $style -match 'sceneTabExtraWidth') }
    [pscustomobject]@{ name='sceneTabIconMetrics'; present=($style -match 'sceneTabIconMinX' -and $style -match 'sceneTabIconMaxX' -and $style -match 'sceneTabIconPaddingY') }
    [pscustomobject]@{ name='sceneTabCloseMetrics'; present=($style -match 'sceneTabCloseMinOffset' -and $style -match 'sceneTabCloseMaxOffset' -and $style -match 'sceneTabCloseIconPaddingX') }
    [pscustomobject]@{ name='sceneTabColors'; present=($style -match 'editorSceneTabBgColor' -and $style -match 'editorSceneTabBorderColor' -and $style -match 'editorSceneTabCloseHoverColor') }
    [pscustomobject]@{ name='sceneTabUsesSharedMetrics'; present=($dockspace -match 'EditorUiMetric::sceneTabExtraWidth' -and $dockspace -match 'EditorUiMetric::sceneTabCloseMinOffset' -and $dockspace -match 'EditorUiMetric::sceneTabCloseIconPaddingX') }
    [pscustomobject]@{ name='sceneTabUsesSharedColors'; present=($dockspace -match 'editorSceneTabBgColor' -and $dockspace -match 'editorSceneTabBorderColor' -and $dockspace -match 'editorSceneTabCloseIconColor') }
)

$themeStyleChecks = @(
    [pscustomobject]@{ name='baseThemeColors'; present=($style -match 'editorWindowBgColor' -and $style -match 'editorChildBgColor' -and $style -match 'editorFrameBgColor' -and $style -match 'editorHeaderColor') }
    [pscustomobject]@{ name='tabMenuThemeColors'; present=($style -match 'editorTitleBgColor' -and $style -match 'editorMenuBarBgColor' -and $style -match 'editorTabColor') }
    [pscustomobject]@{ name='controlThemeColors'; present=($style -match 'editorButtonColor' -and $style -match 'editorCheckMarkColor' -and $style -match 'editorSliderGrabColor') }
    [pscustomobject]@{ name='uiOverlayUsesSharedTheme'; present=($uiOverlay -match 'editorWindowBgColor' -and $uiOverlay -match 'editorTabColor' -and $uiOverlay -match 'editorSeparatorColor') }
    [pscustomobject]@{ name='editorLayerUsesSharedTheme'; present=($editorLayer -match 'editorWindowBgColor' -and $editorLayer -match 'editorHeaderColor' -and $editorLayer -match 'editorTabColor') }
)

$timelineChromeChecks = @(
    [pscustomobject]@{ name='timelineControlMetrics'; present=($style -match 'timelineFrameWidth' -and $style -match 'timelineRangeFrameWidth' -and $style -match 'timelineFrameRateWidth') }
    [pscustomobject]@{ name='timelineRulerMetrics'; present=($style -match 'timelineRulerHeight' -and $style -match 'timelineRulerTickCount' -and $style -match 'timelineRulerRangeTextOffsetX') }
    [pscustomobject]@{ name='timelineTrackMetrics'; present=($style -match 'timelineTrackColumnWidth' -and $style -match 'timelineTrackLaneHeight' -and $style -match 'timelineKeyHitSize') }
    [pscustomobject]@{ name='timelineKeyEditorMetrics'; present=($style -match 'timelineKeyEditorTrackWidth' -and $style -match 'timelineKeyEditorFrameWidth' -and $style -match 'timelineKeyEditorActionWidth') }
    [pscustomobject]@{ name='timelineColors'; present=($style -match 'editorTimelineRulerBgColor' -and $style -match 'editorTimelineLaneBgColor' -and $style -match 'editorTimelineKeyColor' -and $style -match 'editorTimelinePlayheadColor') }
    [pscustomobject]@{ name='timelineUsesSharedMetrics'; present=($editorLayer -match 'EditorUiMetric::timelineFrameWidth' -and $editorLayer -match 'EditorUiMetric::timelineRulerHeight' -and $editorLayer -match 'EditorUiMetric::timelineTrackColumnWidth' -and $editorLayer -match 'EditorUiMetric::timelineKeyEditorActionWidth') }
    [pscustomobject]@{ name='timelineUsesSharedColors'; present=($editorLayer -match 'editorTimelineRulerBgColor' -and $editorLayer -match 'editorTimelineLaneBgColor' -and $editorLayer -match 'editorTimelineKeyColor' -and $editorLayer -match 'editorTimelinePlayheadColor') }
)

$results = @(
    New-ToolResult -Name 'Style metrics inventory' -Passed ($metrics.Count -gt 0) -Message ("metrics={0}" -f $metrics.Count) -Details $metrics
    New-ToolResult -Name 'Reference-match named metrics present' -Passed (@($hasNamed | Where-Object { -not $_.present }).Count -eq 0) -Message ("missing={0}" -f ((@($hasNamed | Where-Object { -not $_.present }).name) -join ', ')) -Details $hasNamed
    New-ToolResult -Name 'Shared row selection style present' -Passed (@($rowStyleChecks | Where-Object { -not $_.present }).Count -eq 0) -Message ("missing={0}" -f ((@($rowStyleChecks | Where-Object { -not $_.present }).name) -join ', ')) -Details $rowStyleChecks
    New-ToolResult -Name 'Dock chrome style metrics present' -Passed (@($dockChromeChecks | Where-Object { -not $_.present }).Count -eq 0) -Message ("missing={0}" -f ((@($dockChromeChecks | Where-Object { -not $_.present }).name) -join ', ')) -Details $dockChromeChecks
    New-ToolResult -Name 'Shared disabled styling present' -Passed (@($disabledStyleChecks | Where-Object { -not $_.present }).Count -eq 0) -Message ("missing={0}" -f ((@($disabledStyleChecks | Where-Object { -not $_.present }).name) -join ', ')) -Details $disabledStyleChecks
    New-ToolResult -Name 'Scene tab chrome style metrics present' -Passed (@($sceneTabStyleChecks | Where-Object { -not $_.present }).Count -eq 0) -Message ("missing={0}" -f ((@($sceneTabStyleChecks | Where-Object { -not $_.present }).name) -join ', ')) -Details $sceneTabStyleChecks
    New-ToolResult -Name 'Base theme style colors present' -Passed (@($themeStyleChecks | Where-Object { -not $_.present }).Count -eq 0) -Message ("missing={0}" -f ((@($themeStyleChecks | Where-Object { -not $_.present }).name) -join ', ')) -Details $themeStyleChecks
    New-ToolResult -Name 'Timeline chrome style metrics present' -Passed (@($timelineChromeChecks | Where-Object { -not $_.present }).Count -eq 0) -Message ("missing={0}" -f ((@($timelineChromeChecks | Where-Object { -not $_.present }).name) -join ', ')) -Details $timelineChromeChecks
)
$passed = Write-ToolResults -Results $results -JsonOut $JsonOut
if ($FailOnIssues -and -not $passed) { exit 1 }
