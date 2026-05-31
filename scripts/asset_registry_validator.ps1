param(
    [Parameter(Mandatory=$true)][string]$RegistryPath,
    [string]$ProjectRoot,
    [string]$JsonOut,
    [switch]$FailOnInvalid
)

Import-Module (Join-Path $PSScriptRoot 'EditorToolingCommon.psm1') -Force
$registry = Read-EditorJson -Path $RegistryPath
if ([string]::IsNullOrWhiteSpace($ProjectRoot)) { $ProjectRoot = Split-Path -Parent (Split-Path -Parent $RegistryPath) }
$ProjectRoot = (Resolve-Path $ProjectRoot).Path
$records = @()
$hasRegistryCollection = $false
if ($null -ne $registry.assets) { $records = @($registry.assets); $hasRegistryCollection = $true }
elseif ($null -ne $registry.records) { $records = @($registry.records); $hasRegistryCollection = $true }
elseif ($registry -is [System.Array]) { $records = @($registry); $hasRegistryCollection = $true }
$errors = @()
$guids = @{}
foreach ($record in $records) {
    $guid = $record.guid
    if ($null -eq $guid) { $guid = $record.assetGuid }
    if ([string]::IsNullOrWhiteSpace([string]$guid)) { $errors += "record missing guid: $($record.displayName)"; continue }
    if ($guids.ContainsKey([string]$guid)) { $errors += "duplicate guid: $guid" } else { $guids[[string]$guid] = $true }
    foreach ($field in @('importedPath','cachePath','thumbnailPath')) {
        $value = $record.$field
        if ([string]::IsNullOrWhiteSpace([string]$value)) { continue }
        if ([System.IO.Path]::IsPathRooted([string]$value)) { $errors += "$guid has rooted $field; expected project-relative path" }
        if ([string]$value -match '\\') { $errors += "$guid has backslashes in $field; prefer normalized forward slashes" }
    }
}
foreach ($record in $records) {
    $guid = if ($record.guid) { $record.guid } else { $record.assetGuid }
    foreach ($dep in @($record.dependencies)) {
        $depGuid = if ($dep.guid) { $dep.guid } elseif ($dep.assetGuid) { $dep.assetGuid } else { $dep }
        if (![string]::IsNullOrWhiteSpace([string]$depGuid) -and -not $guids.ContainsKey([string]$depGuid)) { $errors += "$guid references missing dependency $depGuid" }
    }
}
$results = @(
    (New-ToolResult -Name 'Registry file parsed' -Passed $true -Message $RegistryPath),
    (New-ToolResult -Name 'Registry collection present' -Passed $hasRegistryCollection -Message ("records={0}" -f $records.Count)),
    (New-ToolResult -Name 'Registry integrity' -Passed ($errors.Count -eq 0) -Message ($errors -join '; ') -Details @{ errors=$errors; recordCount=$records.Count })
)
$passed = Write-ToolResults -Results $results -JsonOut $JsonOut
if ($FailOnInvalid -and -not $passed) { exit 1 }
