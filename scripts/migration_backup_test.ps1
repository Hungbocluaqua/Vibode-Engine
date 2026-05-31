param(
    [string]$RepoRoot,
    [Alias('Scene')][string[]]$ScenePath = @('scenes\validation\cornell.rtlevel'),
    [string]$WorkDir = 'out\migration_backup_test',
    [string]$JsonOut,
    [switch]$FailOnInvalid
)

Import-Module (Join-Path $PSScriptRoot 'EditorToolingCommon.psm1') -Force
$RepoRoot = Resolve-EditorToolRepoRoot $RepoRoot
$work = if ([System.IO.Path]::IsPathRooted($WorkDir)) { $WorkDir } else { Join-Path $RepoRoot $WorkDir }
New-Item -ItemType Directory -Force $work | Out-Null
$results = @()
foreach ($sceneItem in $ScenePath) {
    $source = if ([System.IO.Path]::IsPathRooted($sceneItem)) { $sceneItem } else { Join-Path $RepoRoot $sceneItem }
    if (!(Test-Path $source)) { $results += New-ToolResult -Name "Scene exists: $sceneItem" -Passed $false -Message 'not found'; continue }
    $copy = Join-Path $work (Split-Path -Leaf $source)
    $backup = $copy + '.bak'
    Copy-Item -LiteralPath $source -Destination $copy -Force
    Copy-Item -LiteralPath $copy -Destination $backup -Force
    try {
        $srcJson = Read-EditorJson -Path $source
        $copyJson = Read-EditorJson -Path $copy
        $backupJson = Read-EditorJson -Path $backup
        $sameEntityCount = (@($srcJson.entities).Count -eq @($copyJson.entities).Count) -and (@($copyJson.entities).Count -eq @($backupJson.entities).Count)
        $results += New-ToolResult -Name "Migration backup fixture: $sceneItem" -Passed ((Test-Path $backup) -and $sameEntityCount) -Message "copy=$copy backup=$backup" -Details @{ source=$source; copy=$copy; backup=$backup; entityCount=@($copyJson.entities).Count }
    } catch {
        $results += New-ToolResult -Name "Migration backup fixture: $sceneItem" -Passed $false -Message $_.Exception.Message
    }
}
$passed = Write-ToolResults -Results $results -JsonOut $JsonOut
if ($FailOnInvalid -and -not $passed) { exit 1 }
