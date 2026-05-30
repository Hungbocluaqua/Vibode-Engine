param(
    [string]$BuildDir = "build\Release",
    [string]$OutputDir = "out\game_stability_suite",
    [string[]]$Presets = @("low", "balanced", "ultra"),
    [string[]]$Scenes = @("cornell", "closeup_cornell", "sponza_light"),
    [int]$Frames = 80,
    [int]$WarmupFrames = 20,
    [int]$FixedSeed = 1,
    [string]$BudgetPath = "",
    [switch]$FullDiagnostics,
    [switch]$IncludeMotion,
    [switch]$CompareClassicMotion
)

$ErrorActionPreference = "Stop"

$exe = Join-Path $BuildDir "rtvulkan.exe"
if (-not (Test-Path $exe)) {
    Write-Error "rtvulkan.exe not found: $exe"
    exit 1
}
if (-not [string]::IsNullOrWhiteSpace($BudgetPath) -and -not (Test-Path $BudgetPath)) {
    Write-Error "Budget JSON not found: $BudgetPath"
    exit 1
}

$sceneCatalog = @{
    cornell = [PSCustomObject]@{
        Name = "cornell"
        Flag = "--scene"
        Path = "scenes\validation\cornell.rtlevel"
    }
    closeup_cornell = [PSCustomObject]@{
        Name = "closeup_cornell"
        Flag = "--scene"
        Path = "scenes\validation\closeup_cornell.rtlevel"
    }
    sponza_light = [PSCustomObject]@{
        Name = "sponza_light"
        Flag = "--gltf"
        Path = "Sponza\glTF\Sponza.gltf"
    }
    sponza_heavy = [PSCustomObject]@{
        Name = "sponza_heavy"
        Flag = "--scene"
        Path = "main_sponza\sponza.rtlevel"
    }
    sponza_heavy_gltf = [PSCustomObject]@{
        Name = "sponza_heavy_gltf"
        Flag = "--gltf"
        Path = "main_sponza\NewSponza_Main_glTF_003.gltf"
    }
}

function Resolve-SceneList {
    param([string[]]$Names)

    $resolved = @()
    foreach ($name in $Names) {
        if (-not $sceneCatalog.ContainsKey($name)) {
            $known = ($sceneCatalog.Keys | Sort-Object) -join ", "
            throw "Unknown scene '$name'. Known scenes: $known"
        }
        $scene = $sceneCatalog[$name]
        if (-not (Test-Path $scene.Path)) {
            throw "Scene path not found for '$name': $($scene.Path)"
        }
        $resolved += $scene
    }
    return $resolved
}

function Get-JsonPropertyValue {
    param(
        [object]$Object,
        [string]$Name,
        [double]$Default = 0.0
    )

    if ($null -eq $Object) { return $Default }
    $property = $Object.PSObject.Properties[$Name]
    if ($null -eq $property -or $null -eq $property.Value) { return $Default }
    return [double]$property.Value
}

function Test-MotionStabilityBudget {
    param([string]$RunDir)

    if ([string]::IsNullOrWhiteSpace($BudgetPath)) { return 0 }

    $budget = Get-Content $BudgetPath -Raw | ConvertFrom-Json
    $motionBudgetProperty = $budget.PSObject.Properties["motion_stability"]
    if ($null -eq $motionBudgetProperty -or $null -eq $motionBudgetProperty.Value) { return 0 }

    $reportPath = Join-Path $RunDir "self_temporal_report\motion_stability_report.json"
    if (-not (Test-Path $reportPath)) { return 0 }

    $report = Get-Content $reportPath -Raw | ConvertFrom-Json
    $stability = $report.stability_summary
    if ($null -eq $stability) { return 0 }

    $motionBudget = $motionBudgetProperty.Value
    $failures = @()
    $maxUncorrelated = $motionBudget.PSObject.Properties["uncorrelated_temporal_noise_score"]
    if ($null -ne $maxUncorrelated) {
        $actual = Get-JsonPropertyValue $stability "uncorrelated_temporal_noise_score"
        $budgetValue = [double]$maxUncorrelated.Value
        if ($actual -gt $budgetValue) {
            $failures += [PSCustomObject]@{ metric = "motion_stability.uncorrelated_temporal_noise_score"; actual = $actual; budget = $budgetValue }
        }
    }
    $maxBeautyVariance = $motionBudget.PSObject.Properties["beauty_temporal_variance_score"]
    if ($null -ne $maxBeautyVariance) {
        $actual = Get-JsonPropertyValue $stability "beauty_temporal_variance_score"
        $budgetValue = [double]$maxBeautyVariance.Value
        if ($actual -gt $budgetValue) {
            $failures += [PSCustomObject]@{ metric = "motion_stability.beauty_temporal_variance_score"; actual = $actual; budget = $budgetValue }
        }
    }
    $maxDenoiserResidual = $motionBudget.PSObject.Properties["denoiser_residual_noise_score"]
    if ($null -ne $maxDenoiserResidual) {
        $actual = Get-JsonPropertyValue $stability "denoiser_residual_noise_score"
        $budgetValue = [double]$maxDenoiserResidual.Value
        if ($actual -gt $budgetValue) {
            $failures += [PSCustomObject]@{ metric = "motion_stability.denoiser_residual_noise_score"; actual = $actual; budget = $budgetValue }
        }
    }
    $minCorrelation = $motionBudget.PSObject.Properties["strongest_temporal_diagnostic_correlation_min"]
    if ($null -ne $minCorrelation) {
        $actual = Get-JsonPropertyValue $stability "strongest_temporal_diagnostic_correlation"
        $budgetValue = [double]$minCorrelation.Value
        if ($actual -lt $budgetValue) {
            $failures += [PSCustomObject]@{ metric = "motion_stability.strongest_temporal_diagnostic_correlation"; actual = $actual; budget_min = $budgetValue }
        }
    }

    $result = [PSCustomObject]@{
        status = if ($failures.Count -eq 0) { "pass" } else { "fail" }
        failures = $failures
    }
    $resultPath = Join-Path $RunDir "motion_budget.json"
    $result | ConvertTo-Json -Depth 5 | Set-Content $resultPath
    Write-Host ($result | ConvertTo-Json -Depth 5)
    if ($failures.Count -gt 0) { return 1 }
    return 0
}

function New-SummaryRow {
    param(
        [object]$Scene,
        [string]$Preset,
        [string]$Kind,
        [string]$OutDir,
        [int]$ExitCode
    )

    $profilePath = Join-Path $OutDir "profile.json"
    $profile = $null
    if (Test-Path $profilePath) {
        $profile = Get-Content $profilePath -Raw | ConvertFrom-Json
    }

    $passes = if ($null -ne $profile) { $profile.per_pass_gpu_ms } else { $null }
    $passesP95 = if ($null -ne $profile -and $null -ne $profile.PSObject.Properties["per_pass_gpu_ms_p95"]) { $profile.per_pass_gpu_ms_p95 } else { $null }
    $passesP99 = if ($null -ne $profile -and $null -ne $profile.PSObject.Properties["per_pass_gpu_ms_p99"]) { $profile.per_pass_gpu_ms_p99 } else { $null }
    $settings = if ($null -ne $profile) { $profile.settings } else { $null }
    $resolution = if ($null -ne $profile) { $profile.resolution } else { $null }
    $adaptive = if ($null -ne $profile) { $profile.adaptive_quality } else { $null }
    $motionReportPath = Join-Path $OutDir "self_temporal_report\motion_stability_report.json"
    $motionReport = $null
    $stability = $null
    if (Test-Path $motionReportPath) {
        $motionReport = Get-Content $motionReportPath -Raw | ConvertFrom-Json
        $stability = $motionReport.stability_summary
    }

    $restirDiMs = (Get-JsonPropertyValue $passes "restir_spatial") + (Get-JsonPropertyValue $passes "restir_spatial_copy")
    $restirGiMs = (Get-JsonPropertyValue $passes "restir_gi_clear") + (Get-JsonPropertyValue $passes "restir_gi_spatial") + (Get-JsonPropertyValue $passes "restir_gi_final")
    $denoiserMs = Get-JsonPropertyValue $passes "denoiser"
    $momentUpdateMs = Get-JsonPropertyValue $passes "moment_update"
    $historyCopyMs = Get-JsonPropertyValue $passes "history_copy"
    $taaResolveMs = Get-JsonPropertyValue $passes "taa"
    $taaHistoryCopyMs = Get-JsonPropertyValue $passes "taa_history_copy"
    $taaMs = $taaResolveMs + $taaHistoryCopyMs
    $temporalTailMs = $momentUpdateMs + $denoiserMs + $historyCopyMs + $taaMs
    $temporalTailP95Ms = (Get-JsonPropertyValue $passesP95 "moment_update") +
        (Get-JsonPropertyValue $passesP95 "denoiser") +
        (Get-JsonPropertyValue $passesP95 "history_copy") +
        (Get-JsonPropertyValue $passesP95 "taa") +
        (Get-JsonPropertyValue $passesP95 "taa_history_copy")
    $temporalTailP99Ms = (Get-JsonPropertyValue $passesP99 "moment_update") +
        (Get-JsonPropertyValue $passesP99 "denoiser") +
        (Get-JsonPropertyValue $passesP99 "history_copy") +
        (Get-JsonPropertyValue $passesP99 "taa") +
        (Get-JsonPropertyValue $passesP99 "taa_history_copy")
    $memoryBytes = 0.0
    if ($null -ne $profile -and $null -ne $profile.memory) {
        $memory = $profile.memory
        $memoryBytes = (Get-JsonPropertyValue $memory "textures_bytes") +
            (Get-JsonPropertyValue $memory "buffers_bytes") +
            (Get-JsonPropertyValue $memory "acceleration_structure_bytes") +
            (Get-JsonPropertyValue $memory "temporal_history_bytes") +
            (Get-JsonPropertyValue $memory "restir_reservoir_bytes")
    }

    return [PSCustomObject]@{
        scene = $Scene.Name
        preset = $Preset
        kind = $Kind
        status = if ($ExitCode -eq 0) { "pass" } else { "fail" }
        exit_code = $ExitCode
        budget_json = if ([string]::IsNullOrWhiteSpace($BudgetPath)) { $null } else { $BudgetPath }
        budget_status = if ([string]::IsNullOrWhiteSpace($BudgetPath)) { $null } elseif ($ExitCode -eq 0) { "pass" } else { "fail" }
        profile_json = $profilePath
        gpu_avg_ms = if ($null -ne $profile) { [double]$profile.gpu_frame_ms.avg } else { $null }
        gpu_max_ms = if ($null -ne $profile) { [double]$profile.gpu_frame_ms.max } else { $null }
        gpu_p95_ms = if ($null -ne $profile -and $null -ne $profile.gpu_frame_ms.PSObject.Properties["p95"]) { [double]$profile.gpu_frame_ms.p95 } else { $null }
        gpu_p99_ms = if ($null -ne $profile -and $null -ne $profile.gpu_frame_ms.PSObject.Properties["p99"]) { [double]$profile.gpu_frame_ms.p99 } else { $null }
        cpu_avg_ms = if ($null -ne $profile) { [double]$profile.cpu_frame_ms.avg } else { $null }
        cpu_p95_ms = if ($null -ne $profile -and $null -ne $profile.cpu_frame_ms.PSObject.Properties["p95"]) { [double]$profile.cpu_frame_ms.p95 } else { $null }
        validation_errors = if ($null -ne $profile) { [int]$profile.validation_error_count } else { $null }
        render_scale = if ($null -ne $resolution) { [double]$resolution.render_scale } else { $null }
        max_bounces = if ($null -ne $settings) { [int]$settings.max_bounces } else { $null }
        samples_per_pixel = if ($null -ne $settings -and $null -ne $settings.PSObject.Properties["samples_per_pixel"]) { [int]$settings.samples_per_pixel } else { $null }
        limit_samples_per_pixel = if ($null -ne $settings -and $null -ne $settings.PSObject.Properties["limit_samples_per_pixel"]) { [bool]$settings.limit_samples_per_pixel } else { $null }
        effective_samples_per_pixel = if ($null -ne $settings -and $null -ne $settings.PSObject.Properties["effective_samples_per_pixel"]) { [int]$settings.effective_samples_per_pixel } else { $null }
        restir_mode = if ($null -ne $settings) { [string]$settings.restir_mode } else { $null }
        restir_gi_enabled = if ($null -ne $settings) { [bool]$settings.restir_gi_enabled } else { $null }
        restir_gi_half_resolution = if ($null -ne $settings) { [bool]$settings.restir_gi_half_resolution } else { $null }
        adaptive_tier = if ($null -ne $adaptive) { [int]$adaptive.tier } else { $null }
        adaptive_smoothed_gpu_ms = if ($null -ne $adaptive) { [double]$adaptive.smoothed_gpu_ms } else { $null }
        effective_max_bounces = if ($null -ne $adaptive) { [int]$adaptive.effective_max_bounces } else { $null }
        effective_atrous_iterations = if ($null -ne $adaptive) { [int]$adaptive.effective_atrous_iterations } else { $null }
        motion_stability_report = if (Test-Path $motionReportPath) { $motionReportPath } else { $null }
        beauty_temporal_variance_score = if ($null -ne $stability) { [double]$stability.beauty_temporal_variance_score } else { $null }
        uncorrelated_temporal_noise_score = if ($null -ne $stability) { [double]$stability.uncorrelated_temporal_noise_score } else { $null }
        denoiser_residual_noise_score = if ($null -ne $stability -and $null -ne $stability.PSObject.Properties["denoiser_residual_noise_score"]) { [double]$stability.denoiser_residual_noise_score } else { $null }
        strongest_temporal_diagnostic_correlation = if ($null -ne $stability) { [double]$stability.strongest_temporal_diagnostic_correlation } else { $null }
        strongest_denoiser_temporal_diagnostic_correlation = if ($null -ne $stability -and $null -ne $stability.PSObject.Properties["strongest_denoiser_temporal_diagnostic_correlation"]) { [double]$stability.strongest_denoiser_temporal_diagnostic_correlation } else { $null }
        path_trace_ms = Get-JsonPropertyValue $passes "path_trace"
        path_trace_p95_ms = Get-JsonPropertyValue $passesP95 "path_trace"
        path_trace_p99_ms = Get-JsonPropertyValue $passesP99 "path_trace"
        restir_di_ms = $restirDiMs
        restir_gi_ms = $restirGiMs
        moment_update_ms = $momentUpdateMs
        moment_update_p95_ms = Get-JsonPropertyValue $passesP95 "moment_update"
        moment_update_p99_ms = Get-JsonPropertyValue $passesP99 "moment_update"
        denoiser_ms = $denoiserMs
        denoiser_p95_ms = Get-JsonPropertyValue $passesP95 "denoiser"
        denoiser_p99_ms = Get-JsonPropertyValue $passesP99 "denoiser"
        history_copy_ms = $historyCopyMs
        history_copy_p95_ms = Get-JsonPropertyValue $passesP95 "history_copy"
        history_copy_p99_ms = Get-JsonPropertyValue $passesP99 "history_copy"
        taa_resolve_ms = $taaResolveMs
        taa_resolve_p95_ms = Get-JsonPropertyValue $passesP95 "taa"
        taa_resolve_p99_ms = Get-JsonPropertyValue $passesP99 "taa"
        taa_history_copy_ms = $taaHistoryCopyMs
        taa_ms = $taaMs
        temporal_tail_ms = $temporalTailMs
        temporal_tail_p95_ms = $temporalTailP95Ms
        temporal_tail_p99_ms = $temporalTailP99Ms
        memory_mb = [math]::Round($memoryBytes / 1048576.0, 2)
    }
}

function Invoke-RtvulkanRun {
    param(
        [object]$Scene,
        [string]$Preset,
        [string]$Kind,
        [string[]]$ExtraArgs = @()
    )

    $runDir = Join-Path $OutputDir (Join-Path $Scene.Name (Join-Path $Preset $Kind))
    New-Item -ItemType Directory -Force -Path $runDir | Out-Null

    $args = @(
        "--headless",
        $Scene.Flag, $Scene.Path,
        "--render-preset", $Preset,
        "--warmup-frames", [string]$WarmupFrames,
        "--frames", [string]$Frames,
        "--fixed-seed", [string]$FixedSeed,
        "--profile",
        "--profile-json", (Join-Path $runDir "profile.json"),
        "--dump-rendergraph", (Join-Path $runDir "rendergraph.json"),
        "--dump-rendergraph-dot", (Join-Path $runDir "rendergraph.dot"),
        "--dump-memory", (Join-Path $runDir "memory.json"),
        "--dump-frame-timeline", (Join-Path $runDir "timeline.json"),
        "--dump-resource-lifetimes", (Join-Path $runDir "lifetimes.json"),
        "--dump-shader-report", (Join-Path $runDir "shaders.json"),
        "--dump-bindings", (Join-Path $runDir "bindings.json")
    )

    if ($FullDiagnostics) {
        $args += @(
            "--save-debug-views", (Join-Path $runDir "debug_views"),
            "--make-debug-package", (Join-Path $runDir "debug_package")
        )
    }

    if (-not [string]::IsNullOrWhiteSpace($BudgetPath)) {
        $args += @("--check-budget", $BudgetPath)
    }

    $args += $ExtraArgs

    Write-Host "Running $($Scene.Name) / $Preset / $Kind"
    $previousErrorActionPreference = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    & $exe @args 2>&1 | Tee-Object -FilePath (Join-Path $runDir "log.txt") | ForEach-Object { Write-Host $_ }
    $exitCode = $LASTEXITCODE
    $ErrorActionPreference = $previousErrorActionPreference

    if ($exitCode -eq 0 -and $Kind -eq "motion") {
        $sequenceDir = Join-Path $runDir "frame_sequence"
        if (Test-Path $sequenceDir) {
            $reportDir = Join-Path $runDir "self_temporal_report"
            $temporalViews = "beauty,variance,motion-vectors,reprojection-confidence,temporal-history-weight,restir-gi-validity,restir-gi-final"
            Write-Host "Generating temporal stability report for $($Scene.Name) / $Preset / $Kind"
            $previousErrorActionPreference = $ErrorActionPreference
            $ErrorActionPreference = "Continue"
            & $exe --compare-image-sequence $sequenceDir $sequenceDir --out $reportDir --sequence-views $temporalViews 2>&1 | Tee-Object -FilePath (Join-Path $reportDir "log.txt") | ForEach-Object { Write-Host $_ }
            $ErrorActionPreference = $previousErrorActionPreference
        }
        $motionBudgetExit = Test-MotionStabilityBudget -RunDir $runDir
        if ($motionBudgetExit -ne 0) {
            $exitCode = $motionBudgetExit
        }
    }
    return New-SummaryRow -Scene $Scene -Preset $Preset -Kind $Kind -OutDir $runDir -ExitCode $exitCode
}

$resolvedScenes = Resolve-SceneList -Names $Scenes
New-Item -ItemType Directory -Force -Path $OutputDir | Out-Null

$rows = @()
foreach ($scene in $resolvedScenes) {
    foreach ($preset in $Presets) {
        $rows += Invoke-RtvulkanRun -Scene $scene -Preset $preset -Kind "profile"

        if ($IncludeMotion -and $preset -eq "balanced") {
            $motionViews = "beauty,variance,motion-vectors,reprojection-confidence,temporal-history-weight,restir-reservoir-confidence,restir-pairwise-mis,restir-gi-validity,restir-gi-final"
            $rows += Invoke-RtvulkanRun -Scene $scene -Preset $preset -Kind "motion" -ExtraArgs @(
                "--validation-camera-motion",
                "--save-frame-sequence", (Join-Path $OutputDir (Join-Path $scene.Name (Join-Path $preset "motion\frame_sequence"))),
                "--sequence-views", $motionViews,
                "--sequence-frame-count", "8"
            )
        }
    }
}

if ($CompareClassicMotion) {
    $comparePreset = "balanced"
    $compareViews = "beauty,variance,reprojection-confidence,temporal-history-weight"
    foreach ($scene in $resolvedScenes) {
        $classicDir = Join-Path $OutputDir (Join-Path $scene.Name "classic_motion_compare\classic")
        $tunedDir = Join-Path $OutputDir (Join-Path $scene.Name "classic_motion_compare\tuned")
        New-Item -ItemType Directory -Force -Path $classicDir | Out-Null
        New-Item -ItemType Directory -Force -Path $tunedDir | Out-Null

        Write-Host "Running $($scene.Name) / classic motion comparison baseline"
        $previousErrorActionPreference = $ErrorActionPreference
        $ErrorActionPreference = "Continue"
        & $exe --headless $scene.Flag $scene.Path --render-preset $comparePreset --restir classic --restir-gi off --validation-camera-motion --warmup-frames 4 --frames 16 --fixed-seed $FixedSeed --save-frame-sequence (Join-Path $classicDir "frame_sequence") --sequence-views $compareViews --sequence-frame-count 8 --profile --profile-json (Join-Path $classicDir "profile.json") 2>&1 | Tee-Object -FilePath (Join-Path $classicDir "log.txt") | ForEach-Object { Write-Host $_ }
        $classicExit = $LASTEXITCODE
        $ErrorActionPreference = $previousErrorActionPreference
        $rows += New-SummaryRow -Scene $scene -Preset $comparePreset -Kind "classic_motion_compare_baseline" -OutDir $classicDir -ExitCode $classicExit

        Write-Host "Running $($scene.Name) / tuned motion comparison candidate"
        $previousErrorActionPreference = $ErrorActionPreference
        $ErrorActionPreference = "Continue"
        & $exe --headless $scene.Flag $scene.Path --render-preset $comparePreset --validation-camera-motion --warmup-frames 4 --frames 16 --fixed-seed $FixedSeed --save-frame-sequence (Join-Path $tunedDir "frame_sequence") --sequence-views $compareViews --sequence-frame-count 8 --profile --profile-json (Join-Path $tunedDir "profile.json") 2>&1 | Tee-Object -FilePath (Join-Path $tunedDir "log.txt") | ForEach-Object { Write-Host $_ }
        $tunedExit = $LASTEXITCODE
        $ErrorActionPreference = $previousErrorActionPreference
        $rows += New-SummaryRow -Scene $scene -Preset $comparePreset -Kind "classic_motion_compare_tuned" -OutDir $tunedDir -ExitCode $tunedExit

        if ($classicExit -eq 0 -and $tunedExit -eq 0) {
            $diffDir = Join-Path $OutputDir (Join-Path $scene.Name "classic_motion_compare\diff")
            New-Item -ItemType Directory -Force -Path $diffDir | Out-Null
            $previousErrorActionPreference = $ErrorActionPreference
            $ErrorActionPreference = "Continue"
            & $exe --compare-image-sequence (Join-Path $classicDir "frame_sequence") (Join-Path $tunedDir "frame_sequence") --out $diffDir --sequence-views $compareViews 2>&1 | Tee-Object -FilePath (Join-Path $diffDir "log.txt") | ForEach-Object { Write-Host $_ }
            $ErrorActionPreference = $previousErrorActionPreference
        }
    }
}

$summaryJson = Join-Path $OutputDir "summary.json"
$summaryCsv = Join-Path $OutputDir "summary.csv"
$rows | ConvertTo-Json -Depth 6 | Set-Content $summaryJson
$rows | Export-Csv -NoTypeInformation -Path $summaryCsv
$rows | Sort-Object scene,preset,kind | Format-Table scene,preset,kind,status,budget_status,gpu_avg_ms,gpu_p95_ms,gpu_p99_ms,gpu_max_ms,samples_per_pixel,limit_samples_per_pixel,effective_samples_per_pixel,adaptive_tier,effective_max_bounces,uncorrelated_temporal_noise_score,denoiser_residual_noise_score,strongest_temporal_diagnostic_correlation,strongest_denoiser_temporal_diagnostic_correlation,path_trace_ms,restir_di_ms,restir_gi_ms,moment_update_ms,denoiser_ms,history_copy_ms,taa_ms,temporal_tail_ms,memory_mb -AutoSize

if ($rows | Where-Object { $_.status -eq "fail" }) {
    Write-Error "One or more diagnostics runs failed. See $OutputDir for logs."
    exit 1
}

Write-Host "Wrote $summaryJson and $summaryCsv"
exit 0
