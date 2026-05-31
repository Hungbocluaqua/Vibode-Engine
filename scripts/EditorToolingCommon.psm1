Set-StrictMode -Version 2.0

function Get-EditorToolRepoRoot {
    param([string]$StartPath = (Get-Location).Path)
    $dir = Resolve-Path $StartPath
    while ($null -ne $dir) {
        $candidate = Join-Path $dir.Path 'CMakeLists.txt'
        $agent = Join-Path $dir.Path 'AGENTS.md'
        if ((Test-Path $candidate) -and (Test-Path $agent)) { return $dir.Path }
        $parent = Split-Path -Parent $dir.Path
        if ([string]::IsNullOrWhiteSpace($parent) -or $parent -eq $dir.Path) { break }
        $dir = Resolve-Path $parent
    }
    throw "Could not find repo root from '$StartPath'. Run from native/vulkan or pass -RepoRoot."
}

function Resolve-EditorToolRepoRoot {
    param([string]$RepoRoot)
    if ([string]::IsNullOrWhiteSpace($RepoRoot)) { return Get-EditorToolRepoRoot }
    return (Resolve-Path $RepoRoot).Path
}

function Read-EditorJson {
    param([Parameter(Mandatory=$true)][string]$Path)
    if (!(Test-Path $Path)) { throw "JSON file not found: $Path" }
    $raw = Get-Content -Raw -LiteralPath $Path
    if ([string]::IsNullOrWhiteSpace($raw)) { throw "JSON file is empty: $Path" }
    return $raw | ConvertFrom-Json
}

function Find-RtvulkanExe {
    param([string]$RepoRoot, [string]$Config = 'Release', [string]$ExePath)
    if (![string]::IsNullOrWhiteSpace($ExePath)) {
        if (!(Test-Path $ExePath)) { throw "rtvulkan executable not found: $ExePath" }
        return (Resolve-Path $ExePath).Path
    }
    $candidates = @(
        (Join-Path $RepoRoot "build\$Config\rtvulkan.exe"),
        (Join-Path $RepoRoot "build\Release\rtvulkan.exe"),
        (Join-Path $RepoRoot "build\Debug\rtvulkan.exe"),
        (Join-Path $RepoRoot "build\rtvulkan.exe")
    )
    foreach ($candidate in $candidates) { if (Test-Path $candidate) { return (Resolve-Path $candidate).Path } }
    throw "Could not find rtvulkan.exe. Build first or pass -ExePath."
}

function New-ToolResult {
    param(
        [Parameter(Mandatory=$true)][string]$Name,
        [Parameter(Mandatory=$true)][bool]$Passed,
        [string]$Message = '',
        [object]$Details = $null
    )
    [pscustomobject]@{ name = $Name; passed = $Passed; message = $Message; details = $Details }
}

function Write-ToolResults {
    param([Parameter(Mandatory=$true)][object[]]$Results, [string]$JsonOut)
    $failed = @($Results | Where-Object { -not $_.passed })
    foreach ($result in $Results) {
        $prefix = if ($result.passed) { '[PASS]' } else { '[FAIL]' }
        if ([string]::IsNullOrWhiteSpace($result.message)) { Write-Host "$prefix $($result.name)" }
        else { Write-Host "$prefix $($result.name): $($result.message)" }
    }
    if (![string]::IsNullOrWhiteSpace($JsonOut)) {
        $dir = Split-Path -Parent $JsonOut
        if (![string]::IsNullOrWhiteSpace($dir)) { New-Item -ItemType Directory -Force $dir | Out-Null }
        try {
            $Results | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath $JsonOut -Encoding UTF8
        } catch {
            Write-Warning ("Detailed JSON export failed: {0}. Writing summary only." -f $_.Exception.Message)
            $Results | Select-Object name,passed,message | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath $JsonOut -Encoding UTF8
        }
    }
    return ($failed.Count -eq 0)
}

function Get-TextMatches {
    param([Parameter(Mandatory=$true)][string]$RepoRoot, [Parameter(Mandatory=$true)][string]$Pattern, [string[]]$Paths = @('include','src'))
    $resolved = @()
    foreach ($path in $Paths) { $resolved += (Join-Path $RepoRoot $path) }
    $args = @('-n', '-S', $Pattern) + $resolved
    $output = & rg @args 2>$null
    if ($LASTEXITCODE -gt 1) { throw "rg failed for pattern '$Pattern'." }
    return @($output)
}

function Get-EntityCountFromRtLevel {
    param([Parameter(Mandatory=$true)][string]$Path)
    $json = Read-EditorJson -Path $Path
    if ($null -eq $json.entities) { return 0 }
    return @($json.entities).Count
}

Export-ModuleMember -Function Get-EditorToolRepoRoot, Resolve-EditorToolRepoRoot, Read-EditorJson, Find-RtvulkanExe, New-ToolResult, Write-ToolResults, Get-TextMatches, Get-EntityCountFromRtLevel
