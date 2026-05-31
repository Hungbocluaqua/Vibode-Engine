param(
    [Parameter(Mandatory=$true)][string]$Before,
    [string]$After,
    [int]$ExpectedDelta,
    [string]$JsonOut,
    [switch]$FailOnMismatch
)

Import-Module (Join-Path $PSScriptRoot 'EditorToolingCommon.psm1') -Force
$beforeCount = Get-EntityCountFromRtLevel -Path $Before
$details = [ordered]@{ before=$Before; beforeCount=$beforeCount }
$passed = $true
$message = "before=$beforeCount"
if (![string]::IsNullOrWhiteSpace($After)) {
    $afterCount = Get-EntityCountFromRtLevel -Path $After
    $delta = $afterCount - $beforeCount
    $details.after = $After
    $details.afterCount = $afterCount
    $details.delta = $delta
    $message = "before=$beforeCount after=$afterCount delta=$delta"
    if ($PSBoundParameters.ContainsKey('ExpectedDelta')) {
        $passed = ($delta -eq $ExpectedDelta)
        $message += " expectedDelta=$ExpectedDelta"
    }
}
$results = @((New-ToolResult -Name 'Scene entity count probe' -Passed $passed -Message $message -Details $details))
$ok = Write-ToolResults -Results $results -JsonOut $JsonOut
if ($FailOnMismatch -and -not $ok) { exit 1 }
