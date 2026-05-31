param(
    [string]$RepoRoot,
    [string[]]$Path,
    [string]$JsonOut,
    [switch]$FailOnInvalid
)

Import-Module (Join-Path $PSScriptRoot 'EditorToolingCommon.psm1') -Force
$RepoRoot = Resolve-EditorToolRepoRoot $RepoRoot
if ($null -eq $Path -or $Path.Count -eq 0) { $Path = @('scenes\validation') }
$files = @()
foreach ($p in $Path) {
    $full = if ([System.IO.Path]::IsPathRooted($p)) { $p } else { Join-Path $RepoRoot $p }
    if (Test-Path $full -PathType Container) { $files += Get-ChildItem -Recurse -Filter *.rtlevel -LiteralPath $full }
    elseif (Test-Path $full -PathType Leaf) { $files += Get-Item -LiteralPath $full }
    else { Write-Warning "Path not found: $p" }
}

$results = @()
foreach ($file in $files) {
    try {
        $json = Read-EditorJson -Path $file.FullName
        $errors = @()
        if ($null -eq $json.version) { $errors += 'missing version' }
        if ($null -eq $json.entities) { $errors += 'missing entities array' }
        $uuids = @()
        foreach ($entity in @($json.entities)) {
            if ($null -eq $entity.id) { $errors += "entity missing id: $($entity.name)"; continue }
            $uuid = $entity.id.uuid
            if ($null -eq $uuid) { $uuid = $entity.id.stable }
            if ($null -eq $uuid) { $errors += "entity missing uuid/stable: $($entity.name)" } else { $uuids += [string]$uuid }
            if ($null -eq $entity.transform) { $errors += "entity missing transform: $($entity.name)" }
        }
        $dupes = @($uuids | Group-Object | Where-Object { $_.Count -gt 1 } | ForEach-Object { $_.Name })
        if ($dupes.Count -gt 0) { $errors += "duplicate entity uuid(s): $($dupes -join ',')" }
        $results += New-ToolResult -Name $file.FullName -Passed ($errors.Count -eq 0) -Message ($errors -join '; ') -Details @{ entityCount=@($json.entities).Count; errors=$errors }
    } catch {
        $results += New-ToolResult -Name $file.FullName -Passed $false -Message $_.Exception.Message
    }
}
$passed = Write-ToolResults -Results $results -JsonOut $JsonOut
if ($FailOnInvalid -and -not $passed) { exit 1 }
