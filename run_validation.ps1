param([string]$BuildDir = "build\Release")

$manifestPath = "scenes/validation/manifest.json"
if (-not (Test-Path $manifestPath)) {
    Write-Error "Validation manifest not found: $manifestPath"
    exit 1
}

$manifest = Get-Content $manifestPath -Raw | ConvertFrom-Json
$Scenes = @()
foreach ($scene in $manifest.scenes) {
    $scenePath = Join-Path "scenes/validation" $scene.path
    $sceneName = [System.IO.Path]::GetFileNameWithoutExtension($scene.path)
    $Scenes += @{
        Name = $sceneName
        Path = $scenePath
        Frames = 120
        Warmup = 30
    }
}

$outBase = "validation_output"
$Results = @()
foreach ($scene in $Scenes) {
    $outDir = "$outBase/$($scene.Name)"
    New-Item -ItemType Directory -Force -Path $outDir | Out-Null

    & "$BuildDir\rtvulkan.exe" --headless `
        --scene $scene.Path `
        --frames $scene.Frames `
        --warmup-frames $scene.Warmup `
        --fixed-seed 1 `
        --profile `
        --profile-json "$outDir/profile.json" `
        --save-debug-views "$outDir/debug_views" `
        2>&1 | Tee-Object -FilePath "$outDir/log.txt"

    $exitCode = $LASTEXITCODE
    $Results += [PSCustomObject]@{
        Scene = $scene.Name
        Status = if ($exitCode -eq 0) { "pass" } else { "fail" }
        ExitCode = $exitCode
    }
}

$Results | Format-Table -AutoSize

$summary = @{
    scenes = @()
    total_pass = 0
    total_fail = 0
}
foreach ($r in $Results) {
    $summary.scenes += @{
        name = $r.Scene
        status = $r.Status
        gpu_ms_total = 0
        validation_errors = 0
        frames_rendered = 0
    }
    if ($r.Status -eq "pass") { $summary.total_pass++ } else { $summary.total_fail++ }
}
$summary | ConvertTo-Json -Depth 3 | Set-Content "$outBase/summary.json"

if ($Results | Where-Object { $_.Status -eq "fail" }) { exit 1 }
exit 0
