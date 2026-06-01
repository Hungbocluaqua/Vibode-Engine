param(
    [string]$RepoRoot,
    [string]$JsonOut,
    [switch]$FailOnStale
)

Import-Module (Join-Path $PSScriptRoot 'EditorToolingCommon.psm1') -Force
$RepoRoot = Resolve-EditorToolRepoRoot $RepoRoot
$files = @(Get-ChildItem -Recurse -File -Include *.cpp,*.h -LiteralPath (Join-Path $RepoRoot 'src')) + @(Get-ChildItem -Recurse -File -Include *.cpp,*.h -LiteralPath (Join-Path $RepoRoot 'include'))
$patterns = @(
    'ImGui::Begin\("([^"]+)"',
    'ImGui::BeginMenu\("([^"]+)"',
    'ImGui::MenuItem\("([^"]+)"',
    'DockBuilderDockWindow\("([^"]+)"'
)
$items = @()
foreach ($file in $files) {
    $lineNo = 0
    foreach ($line in Get-Content -LiteralPath $file.FullName) {
        $lineNo++
        foreach ($pattern in $patterns) {
            $m = [regex]::Match($line, $pattern)
            if ($m.Success) {
                $items += [pscustomobject]@{ file=$file.FullName; line=$lineNo; label=$m.Groups[1].Value; source=$line.Trim() }
            }
        }
    }
}
$staleLabels = @('Viewport','Scene Hierarchy','Inspector / Properties','Asset Browser','Open glTF','Load glTF')
$stale = @($items | Where-Object { $staleLabels -contains $_.label -or $_.label -like 'Open glTF*' -or $_.label -like 'Load glTF*' })
$required = @('Scene','Hierarchy','Inspector','Content','Timeline','Log','Render Settings')
$presentLabels = @($items | ForEach-Object { $_.label } | Select-Object -Unique)
$missingRequired = @($required | Where-Object { $presentLabels -notcontains $_ })
$results = @(
    (New-ToolResult -Name 'ImGui labels collected' -Passed ($items.Count -gt 0) -Message ("labels={0}" -f $items.Count) -Details $items),
    (New-ToolResult -Name 'No stale editor labels' -Passed ($stale.Count -eq 0) -Message ("stale={0}" -f $stale.Count) -Details $stale),
    (New-ToolResult -Name 'Required target labels present' -Passed ($missingRequired.Count -eq 0) -Message ("missing={0}" -f ($missingRequired -join ', ')) -Details $missingRequired)
)
$passed = Write-ToolResults -Results $results -JsonOut $JsonOut
if ($FailOnStale -and -not $passed) { exit 1 }
