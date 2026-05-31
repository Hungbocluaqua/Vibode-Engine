param(
    [string]$RepoRoot,
    [string]$ExePath,
    [ValidateSet('Debug','Release')][string]$Config = 'Release',
    [string]$OutDir = 'out\editor_smoke',
    [int]$WarmupFrames = 30,
    [int]$Frames = 120,
    [switch]$BuildDebug,
    [switch]$BuildRelease,
    [switch]$SkipRun,
    [string]$Scene = 'scenes\validation\cornell.rtlevel',
    [string]$JsonOut
)

Import-Module (Join-Path $PSScriptRoot 'EditorToolingCommon.psm1') -Force
$RepoRoot = Resolve-EditorToolRepoRoot $RepoRoot
Push-Location $RepoRoot
try {
    $results = @()
    if ($BuildDebug) {
        & cmake --build build --config Debug
        $results += New-ToolResult -Name 'Build Debug' -Passed ($LASTEXITCODE -eq 0) -Message "exit=$LASTEXITCODE"
        if ($LASTEXITCODE -ne 0) { Write-ToolResults -Results $results -JsonOut $JsonOut | Out-Null; exit 1 }
    }
    if ($BuildRelease) {
        & cmake --build build --config Release
        $results += New-ToolResult -Name 'Build Release' -Passed ($LASTEXITCODE -eq 0) -Message "exit=$LASTEXITCODE"
        if ($LASTEXITCODE -ne 0) { Write-ToolResults -Results $results -JsonOut $JsonOut | Out-Null; exit 1 }
    }
    if (!$SkipRun) {
        $exe = Find-RtvulkanExe -RepoRoot $RepoRoot -Config $Config -ExePath $ExePath
        $outPath = Join-Path $RepoRoot $OutDir
        New-Item -ItemType Directory -Force $outPath | Out-Null
        $profile = Join-Path $outPath 'profile.json'
        $rendergraph = Join-Path $outPath 'rendergraph.json'
        $debugViews = Join-Path $outPath 'debug_views'
        $debugPackage = Join-Path $outPath 'debug_package'
        & $exe --headless --scene $Scene --warmup-frames $WarmupFrames --frames $Frames --fixed-seed 1 --profile --profile-json $profile --dump-rendergraph $rendergraph --save-debug-views $debugViews --make-debug-package $debugPackage
        $results += New-ToolResult -Name 'Cornell diagnostic smoke run' -Passed ($LASTEXITCODE -eq 0) -Message "exit=$LASTEXITCODE"
        $expected = @($profile, $rendergraph, $debugViews, $debugPackage)
        foreach ($item in $expected) {
            $results += New-ToolResult -Name "Output exists: $item" -Passed (Test-Path $item)
        }
    }
    $passed = Write-ToolResults -Results $results -JsonOut $JsonOut
    if (!$passed) { exit 1 }
} finally {
    Pop-Location
}
