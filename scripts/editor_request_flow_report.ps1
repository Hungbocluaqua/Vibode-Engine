param(
    [string]$RepoRoot,
    [string]$JsonOut,
    [switch]$FailOnUnused
)

Import-Module (Join-Path $PSScriptRoot 'EditorToolingCommon.psm1') -Force
$RepoRoot = Resolve-EditorToolRepoRoot $RepoRoot
$header = Join-Path $RepoRoot 'include\rtv\EditorPanels.h'
$text = Get-Content -Raw -LiteralPath $header
$structMatch = [regex]::Match($text, 'struct\s+EditorRequests\s*\{(?<body>[\s\S]*?)\n\};')
if (!$structMatch.Success) { throw 'Could not find struct EditorRequests.' }
$body = $structMatch.Groups['body'].Value
$fieldMatches = [regex]::Matches($body, '(?:std::optional<[^;]+>|bool)\s+([A-Za-z_][A-Za-z0-9_]*)')
$fields = @($fieldMatches | ForEach-Object { $_.Groups[1].Value } | Select-Object -Unique)
$srcPaths = @((Join-Path $RepoRoot 'include'), (Join-Path $RepoRoot 'src'))
$report = @()
foreach ($field in $fields) {
    $pattern = "requests\.$field|\.$field\s*=|$field\.has_value\(\)"
    $hits = Get-TextMatches -RepoRoot $RepoRoot -Pattern $pattern -Paths @('include','src')
    $ui = @($hits | Where-Object { $_ -like '*Panel*' -or $_ -like '*Dockspace*' -or $_ -like '*UiOverlay*' -or $_ -like '*EditorLayer*' })
    $app = @($hits | Where-Object { $_ -like '*Application.cpp*' })
    $report += [pscustomobject]@{ field=$field; matchCount=@($hits).Count; uiMatchCount=$ui.Count; applicationMatchCount=$app.Count; hasApplicationHandler=($app.Count -gt 0); matches=@($hits) }
}
$uiOnlyFields = @('resetLayout', 'saveLayout', 'showProjectManager', 'showCommandPalette')
$unhandled = @($report | Where-Object { -not $_.hasApplicationHandler -and $_.field -notin (@('settings') + $uiOnlyFields) })
$results = @(
    (New-ToolResult -Name 'EditorRequests fields parsed' -Passed ($fields.Count -gt 0) -Message ("fields={0}" -f $fields.Count) -Details $report),
    (New-ToolResult -Name 'Fields have Application handlers' -Passed ($unhandled.Count -eq 0) -Message ("unhandled={0}" -f ($unhandled.field -join ', ')) -Details $unhandled)
)
$passed = Write-ToolResults -Results $results -JsonOut $JsonOut
if ($FailOnUnused -and -not $passed) { exit 1 }
