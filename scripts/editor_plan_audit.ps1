param(
    [string]$RepoRoot,
    [string]$JsonOut,
    [switch]$FailOnMissing
)

Import-Module (Join-Path $PSScriptRoot 'EditorToolingCommon.psm1') -Force
$RepoRoot = Resolve-EditorToolRepoRoot $RepoRoot

$checks = @(
    @{ name='ProjectContext type'; pattern='ProjectContext'; paths=@('include','src') },
    @{ name='.rtproject support'; pattern='rtproject'; paths=@('include','src') },
    @{ name='AssetGuid type'; pattern='AssetGuid'; paths=@('include','src') },
    @{ name='AssetRegistry support'; pattern='AssetRegistry|AssetRecord'; paths=@('include','src') },
    @{ name='Import Asset request'; pattern='importAsset'; paths=@('include','src') },
    @{ name='Import Scene as New Scene request'; pattern='importSceneAsNewScene'; paths=@('include','src') },
    @{ name='Import and Place request'; pattern='importAndPlace'; paths=@('include','src') },
    @{ name='Merge Scene request'; pattern='mergeScene'; paths=@('include','src') },
    @{ name='Prefab support'; pattern='PrefabAsset|PrefabInstance|PrefabOverride'; paths=@('include','src') },
    @{ name='Render World Settings panel'; pattern='RenderWorldSettings|renderWorldSettings|Render World Settings'; paths=@('include','src') },
    @{ name='Timeline panel'; pattern='TimelinePanel|Begin\("Timeline"\)|timeline ='; paths=@('include','src') },
    @{ name='Log panel'; pattern='LogPanel|Begin\("Log"\)|EditorLog'; paths=@('include','src') },
    @{ name='Console panel'; pattern='ConsolePanel|Begin\("Console"\)|EditorConsole'; paths=@('include','src') },
    @{ name='Command registry'; pattern='EditorCommand|EditorCommandId|CommandRegistry'; paths=@('include','src') },
    @{ name='AsyncSceneLoader'; pattern='AsyncSceneLoader|SceneLoadRequest|SceneLoadResult|SceneLoadMode'; paths=@('include','src') },
    @{ name='GUID-backed rtlevel header'; pattern='RtLevelHeader|sceneGuid|assetGuid'; paths=@('include','src') },
    @{ name='Autosave/recovery'; pattern='Autosave|Recovery|Saved\\Autosaves|Saved/Autosaves'; paths=@('include','src') }
)

$results = @()
foreach ($check in $checks) {
    $matches = Get-TextMatches -RepoRoot $RepoRoot -Pattern $check.pattern -Paths $check.paths
    $results += New-ToolResult -Name $check.name -Passed ($matches.Count -gt 0) -Message ("matches={0}" -f $matches.Count) -Details @{ pattern=$check.pattern; matches=$matches }
}

$passed = Write-ToolResults -Results $results -JsonOut $JsonOut
if ($FailOnMissing -and -not $passed) { exit 1 }
