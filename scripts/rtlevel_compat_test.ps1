param(
    [string]$RepoRoot,
    [string[]]$Path = @('scenes\validation'),
    [string]$ExePath,
    [ValidateSet('Debug','Release')][string]$Config = 'Release',
    [switch]$RunHeadless,
    [int]$Frames = 1,
    [string]$JsonOut,
    [switch]$FailOnInvalid
)

Import-Module (Join-Path $PSScriptRoot 'EditorToolingCommon.psm1') -Force
$RepoRoot = Resolve-EditorToolRepoRoot $RepoRoot
$results = @()
$schemaJson = if ($JsonOut) { [System.IO.Path]::ChangeExtension($JsonOut, '.schema.json') } else { $null }
& (Join-Path $PSScriptRoot 'rtlevel_schema_validator.ps1') -RepoRoot $RepoRoot -Path $Path -JsonOut $schemaJson
$schemaExit = if ($null -eq $LASTEXITCODE) { 0 } else { $LASTEXITCODE }
$results += New-ToolResult -Name 'Schema validation script' -Passed ($schemaExit -eq 0) -Message "exit=$schemaExit"

if ($RunHeadless) {
    $exe = Find-RtvulkanExe -RepoRoot $RepoRoot -Config $Config -ExePath $ExePath
    $files = @()
    foreach ($p in $Path) {
        $full = if ([System.IO.Path]::IsPathRooted($p)) { $p } else { Join-Path $RepoRoot $p }
        if (Test-Path $full -PathType Container) { $files += Get-ChildItem -Recurse -Filter *.rtlevel -LiteralPath $full }
        elseif (Test-Path $full -PathType Leaf) { $files += Get-Item -LiteralPath $full }
    }
    foreach ($file in $files) {
        & $exe --headless --scene $file.FullName --frames $Frames --fixed-seed 1
        $results += New-ToolResult -Name "Headless load: $($file.Name)" -Passed ($LASTEXITCODE -eq 0) -Message "exit=$LASTEXITCODE"
    }
}
$passed = Write-ToolResults -Results $results -JsonOut $JsonOut
if ($FailOnInvalid -and -not $passed) { exit 1 }
