param(
    [string]$RepoRoot,
    [string]$Exe = 'build\Debug\rtvulkan.exe',
    [string]$Scene = 'scenes\validation\cornell.rtlevel',
    [string]$Gltf = '',
    [string]$Project = 'Samples\CornellValidation\CornellValidation.vproject',
    [string]$Out = 'out\editor_ui_ux_audits\current_editor.png',
    [string]$JsonOut = 'out\editor_ui_ux_audits\current_editor.json',
    [string]$LogOut = '',
    [int]$WaitSeconds = 8,
    [int]$CaptureDelaySeconds = 5,
    [int]$SelectHierarchyRow = -1,
    [int]$ClickX = -1,
    [int]$ClickY = -1,
    [ValidateSet('Left','Right')]
    [string]$ClickButton = 'Left',
    [int]$ClickHoldMilliseconds = 40,
    [int]$WindowWidth = 0,
    [int]$WindowHeight = 0,
    [int]$PostSelectDelayMilliseconds = 700,
    [string]$StateName = 'default',
    [string]$FocusWindow = '',
    [switch]$UseProjectStartup,
    [switch]$ProjectManagerStartup,
    [switch]$FailOnIssues
)

Import-Module (Join-Path $PSScriptRoot 'EditorToolingCommon.psm1') -Force
$RepoRoot = Resolve-EditorToolRepoRoot $RepoRoot

Add-Type -AssemblyName System.Drawing
Add-Type @'
using System;
using System.Runtime.InteropServices;

public static class EditorWindowCaptureNative {
    [StructLayout(LayoutKind.Sequential)]
    public struct RECT {
        public int Left;
        public int Top;
        public int Right;
        public int Bottom;
    }

    [DllImport("user32.dll")]
    public static extern bool GetWindowRect(IntPtr hWnd, out RECT rect);

    [DllImport("user32.dll")]
    public static extern bool SetForegroundWindow(IntPtr hWnd);

    [DllImport("user32.dll")]
    public static extern bool BringWindowToTop(IntPtr hWnd);

    [DllImport("user32.dll")]
    public static extern bool ShowWindow(IntPtr hWnd, int nCmdShow);

    [DllImport("user32.dll")]
    public static extern bool SetWindowPos(IntPtr hWnd, IntPtr hWndInsertAfter, int X, int Y, int cx, int cy, uint uFlags);

    [DllImport("user32.dll")]
    public static extern bool SetCursorPos(int X, int Y);

    [DllImport("user32.dll")]
    public static extern void mouse_event(uint dwFlags, uint dx, uint dy, uint dwData, UIntPtr dwExtraInfo);

    public const uint MOUSEEVENTF_LEFTDOWN = 0x0002;
    public const uint MOUSEEVENTF_LEFTUP = 0x0004;
    public const uint MOUSEEVENTF_RIGHTDOWN = 0x0008;
    public const uint MOUSEEVENTF_RIGHTUP = 0x0010;
    public const uint SWP_SHOWWINDOW = 0x0040;
    public const uint SWP_NOMOVE = 0x0002;
    public const uint SWP_NOSIZE = 0x0001;
    public const int SW_RESTORE = 9;

    public static readonly IntPtr HWND_TOPMOST = new IntPtr(-1);
    public static readonly IntPtr HWND_NOTOPMOST = new IntPtr(-2);
}
'@

function Show-EditorWindowForCapture {
    param([IntPtr]$Handle)
    [void][EditorWindowCaptureNative]::ShowWindow($Handle, [EditorWindowCaptureNative]::SW_RESTORE)
    [void][EditorWindowCaptureNative]::SetWindowPos($Handle, [EditorWindowCaptureNative]::HWND_TOPMOST, 0, 0, 0, 0, [EditorWindowCaptureNative]::SWP_NOMOVE -bor [EditorWindowCaptureNative]::SWP_NOSIZE -bor [EditorWindowCaptureNative]::SWP_SHOWWINDOW)
    [void][EditorWindowCaptureNative]::SetWindowPos($Handle, [EditorWindowCaptureNative]::HWND_NOTOPMOST, 0, 0, 0, 0, [EditorWindowCaptureNative]::SWP_NOMOVE -bor [EditorWindowCaptureNative]::SWP_NOSIZE -bor [EditorWindowCaptureNative]::SWP_SHOWWINDOW)
    [void][EditorWindowCaptureNative]::BringWindowToTop($Handle)
    [void][EditorWindowCaptureNative]::SetForegroundWindow($Handle)
}

function Resolve-CapturePath {
    param([string]$Path)
    if ([System.IO.Path]::IsPathRooted($Path)) { return $Path }
    return Join-Path $RepoRoot $Path
}

function Invoke-EditorClick {
    param(
        [IntPtr]$Handle,
        [int]$X,
        [int]$Y,
        [string]$Button = 'Left',
        [int]$HoldMilliseconds = 40
    )
    [void][EditorWindowCaptureNative]::SetForegroundWindow($Handle)
    [void][EditorWindowCaptureNative]::SetCursorPos($X, $Y)
    $down = if ($Button -eq 'Right') { [EditorWindowCaptureNative]::MOUSEEVENTF_RIGHTDOWN } else { [EditorWindowCaptureNative]::MOUSEEVENTF_LEFTDOWN }
    $up = if ($Button -eq 'Right') { [EditorWindowCaptureNative]::MOUSEEVENTF_RIGHTUP } else { [EditorWindowCaptureNative]::MOUSEEVENTF_LEFTUP }
    [EditorWindowCaptureNative]::mouse_event($down, 0, 0, 0, [UIntPtr]::Zero)
    Start-Sleep -Milliseconds ([Math]::Max(1, $HoldMilliseconds))
    [EditorWindowCaptureNative]::mouse_event($up, 0, 0, 0, [UIntPtr]::Zero)
}

function Write-CaptureJsonNoBom {
    param(
        [Parameter(Mandatory=$true)]$Value,
        [Parameter(Mandatory=$true)][string]$Path
    )
    $json = $Value | ConvertTo-Json -Depth 8
    [System.IO.File]::WriteAllText($Path, $json, [System.Text.UTF8Encoding]::new($false))
}

$exePath = Resolve-CapturePath $Exe
$scenePath = Resolve-CapturePath $Scene
$gltfPath = if (![string]::IsNullOrWhiteSpace($Gltf)) { Resolve-CapturePath $Gltf } else { '' }
$projectPath = Resolve-CapturePath $Project
$outPath = Resolve-CapturePath $Out
$jsonPath = Resolve-CapturePath $JsonOut
$logPath = if (![string]::IsNullOrWhiteSpace($LogOut)) { Resolve-CapturePath $LogOut } else { [System.IO.Path]::ChangeExtension($jsonPath, '.log') }
$stderrLogPath = [System.IO.Path]::Combine([System.IO.Path]::GetDirectoryName($logPath), ([System.IO.Path]::GetFileNameWithoutExtension($logPath) + '.stderr.log'))
$prefsPath = Join-Path $RepoRoot 'editor_preferences.json'
$exePrefsPath = Join-Path (Split-Path -Parent $exePath) 'editor_preferences.json'
$prefsPaths = @($prefsPath, $exePrefsPath) | Select-Object -Unique
$prefsBackupRoot = Join-Path $RepoRoot 'out\editor_ui_ux_audits\prefs_backup'
$projectRoot = Split-Path -Parent $projectPath
$projectRecoveryMarkerPath = Join-Path $projectRoot 'Saved\editor_session.json'
$legacyProjectRecoveryMarkerPath = Join-Path $projectRoot 'Saved\Editor\editor_session.json'
$outDir = Split-Path -Parent $outPath
if (![string]::IsNullOrWhiteSpace($outDir)) {
    New-Item -ItemType Directory -Force $outDir | Out-Null
}
$jsonDir = Split-Path -Parent $jsonPath
if (![string]::IsNullOrWhiteSpace($jsonDir)) {
    New-Item -ItemType Directory -Force $jsonDir | Out-Null
}
$logDir = Split-Path -Parent $logPath
if (![string]::IsNullOrWhiteSpace($logDir)) {
    New-Item -ItemType Directory -Force $logDir | Out-Null
}
Remove-Item -LiteralPath $logPath -Force -ErrorAction SilentlyContinue
Remove-Item -LiteralPath $stderrLogPath -Force -ErrorAction SilentlyContinue

$results = @()
$process = $null
$prefsBackups = @{}
$prefsManaged = $false
$oldFocusWindow = $env:RTV_EDITOR_CAPTURE_FOCUS_WINDOW
$oldStartupProject = $env:RTV_EDITOR_STARTUP_PROJECT
$oldSuppressRecovery = $env:RTV_EDITOR_SUPPRESS_RECOVERY_PROMPT
try {
    if (!(Test-Path -LiteralPath $exePath)) {
        throw "Editor executable not found: $exePath"
    }
    if (($UseProjectStartup -or $ProjectManagerStartup) -and !(Test-Path -LiteralPath $projectPath)) {
        throw "Project not found: $projectPath"
    }
    if (!$UseProjectStartup -and !$ProjectManagerStartup -and [string]::IsNullOrWhiteSpace($gltfPath) -and !(Test-Path -LiteralPath $scenePath)) {
        throw "Scene not found: $scenePath"
    }
    if (!$UseProjectStartup -and !$ProjectManagerStartup -and ![string]::IsNullOrWhiteSpace($gltfPath) -and !(Test-Path -LiteralPath $gltfPath)) {
        throw "glTF not found: $gltfPath"
    }

    if ($UseProjectStartup -or $ProjectManagerStartup) {
        if (Test-Path -LiteralPath $projectRecoveryMarkerPath) {
            Remove-Item -LiteralPath $projectRecoveryMarkerPath -Force
        }
        if (Test-Path -LiteralPath $legacyProjectRecoveryMarkerPath) {
            Remove-Item -LiteralPath $legacyProjectRecoveryMarkerPath -Force
        }
    }

    if ($UseProjectStartup -or $ProjectManagerStartup -or (!$UseProjectStartup -and !$ProjectManagerStartup)) {
        New-Item -ItemType Directory -Force $prefsBackupRoot | Out-Null
        $prefsManaged = $true
        $prefs = [ordered]@{
            cameraMoveSpeed = 2.4
            cameraFastMoveSpeed = 7.5
            gridVisible = $true
            showHud = $true
            hudScale = 1.0
            uiScale = 1.0
            themePreset = 0
            workspacePreset = 0
            layoutVersion = 2
            confirmDelete = $true
            recentFiles = if ($UseProjectStartup -or $ProjectManagerStartup) { @() } elseif (![string]::IsNullOrWhiteSpace($gltfPath)) { @($gltfPath) } else { @($scenePath) }
            favoriteFiles = @()
            recentProjects = if ($UseProjectStartup -or $ProjectManagerStartup) { @($projectPath) } else { @() }
            lastOpenedProject = if ($UseProjectStartup -or $ProjectManagerStartup) { $projectPath } else { '' }
            openLastProject = ($UseProjectStartup -and !$ProjectManagerStartup)
            commandShortcutOverrides = @{}
        }
        for ($i = 0; $i -lt $prefsPaths.Count; ++$i) {
            $managedPrefsPath = $prefsPaths[$i]
            $backupPath = Join-Path $prefsBackupRoot ("editor_preferences.{0}.before_capture.json" -f $i)
            if (Test-Path -LiteralPath $managedPrefsPath) {
                Copy-Item -LiteralPath $managedPrefsPath -Destination $backupPath -Force
                $prefsBackups[$managedPrefsPath] = $backupPath
            }
            $managedPrefsDir = Split-Path -Parent $managedPrefsPath
            if (![string]::IsNullOrWhiteSpace($managedPrefsDir)) {
                New-Item -ItemType Directory -Force $managedPrefsDir | Out-Null
            }
            Write-CaptureJsonNoBom -Value $prefs -Path $managedPrefsPath
        }
    }

    if (![string]::IsNullOrWhiteSpace($FocusWindow)) {
        $env:RTV_EDITOR_CAPTURE_FOCUS_WINDOW = $FocusWindow
    } elseif (Test-Path Env:\RTV_EDITOR_CAPTURE_FOCUS_WINDOW) {
        Remove-Item Env:\RTV_EDITOR_CAPTURE_FOCUS_WINDOW
    }

    if ($UseProjectStartup -and !$ProjectManagerStartup) {
        $env:RTV_EDITOR_STARTUP_PROJECT = $projectPath
        $env:RTV_EDITOR_SUPPRESS_RECOVERY_PROMPT = '1'
    } elseif (Test-Path Env:\RTV_EDITOR_STARTUP_PROJECT) {
        Remove-Item Env:\RTV_EDITOR_STARTUP_PROJECT
    }

    if ($UseProjectStartup -or $ProjectManagerStartup) {
        $process = Start-Process -FilePath $exePath -WorkingDirectory $RepoRoot -PassThru -WindowStyle Normal
    } elseif (![string]::IsNullOrWhiteSpace($gltfPath)) {
        $process = Start-Process -FilePath $exePath -ArgumentList @('--gltf', $gltfPath) -WorkingDirectory $RepoRoot -PassThru -WindowStyle Normal
    } else {
        $process = Start-Process -FilePath $exePath -ArgumentList @('--scene', $scenePath) -WorkingDirectory $RepoRoot -PassThru -WindowStyle Normal
    }
    $deadline = [DateTime]::UtcNow.AddSeconds($WaitSeconds)
    $handle = [IntPtr]::Zero
    while ([DateTime]::UtcNow -lt $deadline) {
        Start-Sleep -Milliseconds 250
        $process.Refresh()
        if ($process.HasExited) {
            throw "Editor exited before screenshot capture. ExitCode=$($process.ExitCode)"
        }
        if ($process.MainWindowHandle -ne 0) {
            $handle = [IntPtr]$process.MainWindowHandle
            break
        }
    }
    if ($handle -eq [IntPtr]::Zero) {
        throw "Editor window handle was not available within $WaitSeconds seconds."
    }

    Show-EditorWindowForCapture -Handle $handle
    $initialRect = New-Object EditorWindowCaptureNative+RECT
    if ([EditorWindowCaptureNative]::GetWindowRect($handle, [ref]$initialRect)) {
        $initialWidth = [Math]::Max(640, $initialRect.Right - $initialRect.Left)
        $initialHeight = [Math]::Max(480, $initialRect.Bottom - $initialRect.Top)
        $targetWidth = if ($WindowWidth -gt 0) { [Math]::Max(640, $WindowWidth) } else { $initialWidth }
        $targetHeight = if ($WindowHeight -gt 0) { [Math]::Max(480, $WindowHeight) } else { $initialHeight }
        [void][EditorWindowCaptureNative]::SetWindowPos($handle, [IntPtr]::Zero, 8, 8, $targetWidth, $targetHeight, [EditorWindowCaptureNative]::SWP_SHOWWINDOW)
    }
    Start-Sleep -Seconds $CaptureDelaySeconds
    Show-EditorWindowForCapture -Handle $handle
    Start-Sleep -Milliseconds 200
    $process.Refresh()
    if ($process.HasExited) {
        throw "Editor exited before delayed screenshot capture. ExitCode=$($process.ExitCode)"
    }
    $rect = New-Object EditorWindowCaptureNative+RECT
    if (![EditorWindowCaptureNative]::GetWindowRect($handle, [ref]$rect)) {
        throw "GetWindowRect failed for editor window."
    }
    $width = $rect.Right - $rect.Left
    $height = $rect.Bottom - $rect.Top
    if ($width -le 0 -or $height -le 0) {
        throw "Editor window has invalid bounds: $width x $height"
    }

    if ($SelectHierarchyRow -ge 0) {
        $hierarchyX = $rect.Left + [Math]::Max(0, $width - 335)
        $hierarchyY = $rect.Top + 151 + ($SelectHierarchyRow * 18)
        Invoke-EditorClick -Handle $handle -X $hierarchyX -Y $hierarchyY
        Start-Sleep -Milliseconds $PostSelectDelayMilliseconds
    }
    if ($ClickX -ge 0 -and $ClickY -ge 0) {
        Invoke-EditorClick -Handle $handle -X ($rect.Left + $ClickX) -Y ($rect.Top + $ClickY) -Button $ClickButton -HoldMilliseconds $ClickHoldMilliseconds
        Start-Sleep -Milliseconds $PostSelectDelayMilliseconds
    }

    $bitmap = [System.Drawing.Bitmap]::new($width, $height)
    $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
    try {
        $graphics.CopyFromScreen($rect.Left, $rect.Top, 0, 0, [System.Drawing.Size]::new($width, $height))
        $bitmap.Save($outPath, [System.Drawing.Imaging.ImageFormat]::Png)
        $darkPixels = 0
        $sampledPixels = 0
        $stepX = [Math]::Max(1, [int]($width / 32))
        $stepY = [Math]::Max(1, [int]($height / 24))
        for ($y = 0; $y -lt $height; $y += $stepY) {
            for ($x = 0; $x -lt $width; $x += $stepX) {
                $pixel = $bitmap.GetPixel($x, $y)
                $luma = (0.2126 * $pixel.R) + (0.7152 * $pixel.G) + (0.0722 * $pixel.B)
                if ($luma -lt 120.0) { $darkPixels++ }
                $sampledPixels++
            }
        }
        $darkRatio = if ($sampledPixels -gt 0) { [double]$darkPixels / [double]$sampledPixels } else { 0.0 }
        if ($darkRatio -lt 0.08) {
            throw ("Captured image appears blank or blocked by a light crash dialog. darkRatio={0:N3}" -f $darkRatio)
        }
    } finally {
        $graphics.Dispose()
        $bitmap.Dispose()
    }

    $details = [pscustomobject]@{
        path = $outPath
        state = $StateName
        executable = $exePath
        scene = if ($UseProjectStartup) { $null } elseif (![string]::IsNullOrWhiteSpace($gltfPath)) { $gltfPath } else { $scenePath }
        gltf = if (![string]::IsNullOrWhiteSpace($gltfPath)) { $gltfPath } else { $null }
        project = if ($UseProjectStartup -or $ProjectManagerStartup) { $projectPath } else { $null }
        selectHierarchyRow = $SelectHierarchyRow
        clickX = $ClickX
        clickY = $ClickY
        clickButton = $ClickButton
        clickHoldMilliseconds = $ClickHoldMilliseconds
        requestedWindowWidth = $WindowWidth
        requestedWindowHeight = $WindowHeight
        focusWindow = $FocusWindow
        projectManagerStartup = [bool]$ProjectManagerStartup
        clearedRecoveryMarker = $projectRecoveryMarkerPath
        clearedLegacyRecoveryMarker = $legacyProjectRecoveryMarkerPath
        width = $width
        height = $height
        left = $rect.Left
        top = $rect.Top
        processId = $process.Id
        stdoutLog = $logPath
        stderrLog = $stderrLogPath
        managedPreferences = $prefsPaths
    }
    $results += New-ToolResult -Name 'Current editor screenshot captured' -Passed (Test-Path -LiteralPath $outPath) -Message ("{0}x{1}: {2}" -f $width, $height, $outPath) -Details $details
} catch {
    $results += New-ToolResult -Name 'Current editor screenshot captured' -Passed $false -Message $_.Exception.Message -Details ([pscustomobject]@{ executable=$exePath; scene=$scenePath; gltf=$gltfPath; project=$projectPath; output=$outPath })
} finally {
    if ($null -ne $process -and -not $process.HasExited) {
        Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
        try { [void]$process.WaitForExit(5000) } catch {}
    }
    if ($prefsManaged) {
        foreach ($managedPrefsPath in $prefsPaths) {
            if ($prefsBackups.ContainsKey($managedPrefsPath) -and (Test-Path -LiteralPath $prefsBackups[$managedPrefsPath])) {
                Copy-Item -LiteralPath $prefsBackups[$managedPrefsPath] -Destination $managedPrefsPath -Force
            } elseif (Test-Path -LiteralPath $managedPrefsPath) {
                Remove-Item -LiteralPath $managedPrefsPath -Force
            }
        }
    }
    if ($UseProjectStartup -or $ProjectManagerStartup) {
        Remove-Item -LiteralPath $projectRecoveryMarkerPath -Force -ErrorAction SilentlyContinue
        Remove-Item -LiteralPath $legacyProjectRecoveryMarkerPath -Force -ErrorAction SilentlyContinue
    }
    if ($null -ne $oldFocusWindow) {
        $env:RTV_EDITOR_CAPTURE_FOCUS_WINDOW = $oldFocusWindow
    } elseif (Test-Path Env:\RTV_EDITOR_CAPTURE_FOCUS_WINDOW) {
        Remove-Item Env:\RTV_EDITOR_CAPTURE_FOCUS_WINDOW
    }
    if ($null -ne $oldStartupProject) {
        $env:RTV_EDITOR_STARTUP_PROJECT = $oldStartupProject
    } elseif (Test-Path Env:\RTV_EDITOR_STARTUP_PROJECT) {
        Remove-Item Env:\RTV_EDITOR_STARTUP_PROJECT
    }
    if ($null -ne $oldSuppressRecovery) {
        $env:RTV_EDITOR_SUPPRESS_RECOVERY_PROMPT = $oldSuppressRecovery
    } elseif (Test-Path Env:\RTV_EDITOR_SUPPRESS_RECOVERY_PROMPT) {
        Remove-Item Env:\RTV_EDITOR_SUPPRESS_RECOVERY_PROMPT
    }
}

$passed = Write-ToolResults -Results $results -JsonOut $jsonPath
if ($FailOnIssues -and -not $passed) { exit 1 }
