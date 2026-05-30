param(
    [Parameter(Mandatory=$true)][string]$RepoRoot,
    [Parameter(Mandatory=$true)][string]$ExePath,
    [string]$Map = "forrest",
    [string]$Profile = "default",
    [int]$Seconds = 60,
    [int]$FrameLimit = 3600,
    [Parameter(Mandatory=$true)][string]$ReportPath,
    [Parameter(Mandatory=$true)][string]$MetadataPath,
    [Parameter(Mandatory=$true)][string]$LauncherLogPath,
    [Parameter(Mandatory=$true)][string]$StdoutPath,
    [Parameter(Mandatory=$true)][string]$StderrPath
)

$ErrorActionPreference = "Stop"

$RepoRoot = (Resolve-Path $RepoRoot).Path
$ExePath = (Resolve-Path $ExePath).Path
$Map = $Map.Trim()
$Profile = $Profile.Trim()
$LogPath = Join-Path $RepoRoot "log.txt"
$FrameStatsPath = Join-Path $RepoRoot "runtime_frame_stats.csv"
$ReportScript = Join-Path $RepoRoot "scripts\codex_playtest_report.ps1"
$Seconds = [Math]::Max(1, $Seconds)
$FrameLimit = [Math]::Max(1, $FrameLimit)
$hardTimeoutSeconds = [Math]::Max($Seconds + 180, 240)

function Write-Metadata {
    param(
        [int]$BuildExitCode,
        [int]$RunExitCode,
        [bool]$TimedOut,
        [bool]$TimeoutForcedKill,
        [double]$DurationSeconds,
        [datetime]$StartedAt,
        [datetime]$EndedAt
    )

    [pscustomobject]@{
        build_exit_code = $BuildExitCode
        run_exit_code = $RunExitCode
        timed_out = $TimedOut
        timeout_forced_kill = $TimeoutForcedKill
        duration_seconds = [Math]::Round($DurationSeconds, 3)
        map = $Map
        profile = $Profile
        frame_limit = $FrameLimit
        requested_seconds = $Seconds
        hard_timeout_seconds = $hardTimeoutSeconds
        started_at = $StartedAt.ToString("o")
        ended_at = $EndedAt.ToString("o")
    } | ConvertTo-Json -Depth 3 | Set-Content -Path $MetadataPath -Encoding UTF8
}

function Invoke-Report {
    param(
        [int]$BuildExitCode,
        [int]$RunExitCode,
        [bool]$TimedOut,
        [bool]$TimeoutForcedKill,
        [double]$DurationSeconds
    )

    & powershell -NoProfile -ExecutionPolicy Bypass -File $ReportScript `
        -RepoRoot $RepoRoot `
        -ReportPath $ReportPath `
        -MetadataPath $MetadataPath `
        -BuildExitCode $BuildExitCode `
        -RunExitCode $RunExitCode `
        -TimedOut:$TimedOut `
        -TimeoutForcedKill:$TimeoutForcedKill `
        -DurationSeconds $DurationSeconds `
        -Map $Map `
        -Profile $Profile `
        -FrameLimit $FrameLimit `
        -LauncherLogPath $LauncherLogPath `
        -StdoutPath $StdoutPath `
        -StderrPath $StderrPath
    return $LASTEXITCODE
}

Add-Type @"
using System;
using System.Runtime.InteropServices;

public static class CodexInput {
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr hWnd);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr hWnd, int nCmdShow);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr hWnd, out RECT lpRect);
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int X, int Y);
    [DllImport("user32.dll")] public static extern void mouse_event(uint dwFlags, uint dx, uint dy, uint dwData, UIntPtr dwExtraInfo);
    [DllImport("user32.dll")] public static extern void keybd_event(byte bVk, byte bScan, uint dwFlags, UIntPtr dwExtraInfo);

    public const int SW_RESTORE = 9;
    public const uint KEYEVENTF_KEYUP = 0x0002;
    public const uint MOUSEEVENTF_LEFTDOWN = 0x0002;
    public const uint MOUSEEVENTF_LEFTUP = 0x0004;

    [StructLayout(LayoutKind.Sequential)]
    public struct RECT {
        public int Left;
        public int Top;
        public int Right;
        public int Bottom;
    }
}
"@

$vk = @{
    W = [byte]0x57
    A = [byte]0x41
    S = [byte]0x53
    D = [byte]0x44
    Space = [byte]0x20
}

function Send-KeyDown([byte]$key) {
    [CodexInput]::keybd_event($key, 0, 0, [UIntPtr]::Zero)
}

function Send-KeyUp([byte]$key) {
    [CodexInput]::keybd_event($key, 0, [CodexInput]::KEYEVENTF_KEYUP, [UIntPtr]::Zero)
}

function Release-AllKeys {
    foreach ($key in $vk.Values) {
        Send-KeyUp $key
    }
}

function Focus-GameWindow {
    param([System.Diagnostics.Process]$Process)
    $Process.Refresh()
    if ($Process.HasExited -or $Process.MainWindowHandle -eq [IntPtr]::Zero) {
        return $false
    }
    [void][CodexInput]::ShowWindow($Process.MainWindowHandle, [CodexInput]::SW_RESTORE)
    [void][CodexInput]::SetForegroundWindow($Process.MainWindowHandle)
    return $true
}

function Move-MousePattern {
    param(
        [System.Diagnostics.Process]$Process,
        [int]$Step
    )
    if (-not (Focus-GameWindow $Process)) {
        return
    }
    $rect = New-Object CodexInput+RECT
    if (-not [CodexInput]::GetWindowRect($Process.MainWindowHandle, [ref]$rect)) {
        return
    }
    $width = [Math]::Max(1, $rect.Right - $rect.Left)
    $height = [Math]::Max(1, $rect.Bottom - $rect.Top)
    $points = @(
        @{ X = 0.70; Y = 0.50 },
        @{ X = 0.50; Y = 0.30 },
        @{ X = 0.30; Y = 0.50 },
        @{ X = 0.50; Y = 0.70 }
    )
    $point = $points[$Step % $points.Count]
    $x = $rect.Left + [int]($width * $point.X)
    $y = $rect.Top + [int]($height * $point.Y)
    [void][CodexInput]::SetCursorPos($x, $y)
}

function Click-Left {
    [CodexInput]::mouse_event([CodexInput]::MOUSEEVENTF_LEFTDOWN, 0, 0, 0, [UIntPtr]::Zero)
    Start-Sleep -Milliseconds 35
    [CodexInput]::mouse_event([CodexInput]::MOUSEEVENTF_LEFTUP, 0, 0, 0, [UIntPtr]::Zero)
}

function Wait-ForLogMarker {
    param(
        [string[]]$Path,
        [string[]]$Markers,
        [int]$TimeoutSeconds
    )
    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
    while ((Get-Date) -lt $deadline) {
        foreach ($candidate in $Path) {
            if (-not $candidate -or -not (Test-Path $candidate)) {
                continue
            }
            $tail = Get-Content -Path $candidate -Tail 250 -ErrorAction SilentlyContinue
            foreach ($marker in $Markers) {
                if ($tail -match [regex]::Escape($marker)) {
                    return $true
                }
            }
        }
        Start-Sleep -Milliseconds 500
    }
    return $false
}

$env:VIBBLE_AUTOSTART_MAP = $Map
$env:VIBBLE_RUNTIME_FRAME_LIMIT = [string]$FrameLimit
$env:VIBBLE_SAFE_LOADING = "1"
$env:VIBBLE_CODEX_PLAYTEST_INPUT = "1"
$env:VIBBLE_CODEX_PLAYTEST_PROFILE = $Profile

$startedAt = Get-Date
$process = $null
$timedOut = $false
$forcedKill = $false
$runExitCode = 1

try {
    Write-Host "[codex_playtest.ps1] Starting game: $ExePath"
    $process = Start-Process -FilePath $ExePath `
        -WorkingDirectory $RepoRoot `
        -RedirectStandardOutput $StdoutPath `
        -RedirectStandardError $StderrPath `
        -PassThru

    $markerPaths = @($LogPath, $StdoutPath)
    [void](Wait-ForLogMarker -Path $markerPaths -Markers @("Auto-selecting map via VIBBLE_AUTOSTART_MAP: $Map", "Map selected: $Map") -TimeoutSeconds 90)
    $loopStarted = Wait-ForLogMarker -Path $markerPaths -Markers @("Game loop started.", "[MenuUI] Runtime frame limit:") -TimeoutSeconds 180
    if (-not $loopStarted) {
        Write-Host "[codex_playtest.ps1] Game loop marker was not observed before input drive started."
    }

    $driveStartedAt = Get-Date
    $deadline = $driveStartedAt.AddSeconds($Seconds)
    $hardDeadline = $startedAt.AddSeconds($hardTimeoutSeconds)
    $step = 0
    $held = @()
    $spiderSlow = $Profile -eq "spider_slow"
    $patterns = if ($spiderSlow) {
        @(
            @($vk.W),
            @($vk.W),
            @($vk.W, $vk.A),
            @($vk.W),
            @($vk.W, $vk.D),
            @($vk.W),
            @($vk.A),
            @($vk.D)
        )
    } else {
        @(
            @($vk.W),
            @($vk.D),
            @($vk.S),
            @($vk.A),
            @($vk.W, $vk.D),
            @($vk.S, $vk.A)
        )
    }

    while (-not $process.HasExited -and (Get-Date) -lt $deadline -and (Get-Date) -lt $hardDeadline) {
        Focus-GameWindow $process | Out-Null
        foreach ($key in $held) {
            Send-KeyUp $key
        }
        $held = $patterns[$step % $patterns.Count]
        foreach ($key in $held) {
            Send-KeyDown $key
        }
        Move-MousePattern -Process $process -Step $step
        if (-not $spiderSlow -and ($step % 4) -eq 0) {
            Click-Left
        }
        if (-not $spiderSlow -and ($step % 6) -eq 0) {
            Send-KeyDown $vk.Space
            Start-Sleep -Milliseconds 60
            Send-KeyUp $vk.Space
        }
        Start-Sleep -Milliseconds ($spiderSlow ? 1250 : 850)
        $process.Refresh()
        $step++
    }

    Release-AllKeys

    if (-not $process.HasExited) {
        Write-Host "[codex_playtest.ps1] Requested play duration complete; requesting close."
        if ($process.MainWindowHandle -ne [IntPtr]::Zero) {
            [void]$process.CloseMainWindow()
        }
        if (-not $process.WaitForExit(10000)) {
            while (-not $process.HasExited -and (Get-Date) -lt $hardDeadline) {
                Start-Sleep -Milliseconds 250
                $process.Refresh()
            }
            if (-not $process.HasExited) {
                $timedOut = $true
                $forcedKill = $true
                Write-Host "[codex_playtest.ps1] Hard timeout reached; force killing game process."
                Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
            }
        }
    }

    if ($process.HasExited) {
        $runExitCode = $process.ExitCode
    } elseif ($forcedKill) {
        $runExitCode = 124
    }
} catch {
    Write-Host "[codex_playtest.ps1] ERROR: $($_.Exception.Message)"
    $runExitCode = 1
    if ($process -and -not $process.HasExited) {
        Release-AllKeys
        Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
        $forcedKill = $true
    }
} finally {
    Release-AllKeys
    $endedAt = Get-Date
    $duration = ($endedAt - $startedAt).TotalSeconds
    if ((-not (Test-Path $LogPath)) -and (Test-Path $StdoutPath)) {
        Copy-Item -Path $StdoutPath -Destination $LogPath -Force
    }
    Write-Metadata -BuildExitCode 0 -RunExitCode $runExitCode -TimedOut $timedOut -TimeoutForcedKill $forcedKill -DurationSeconds $duration -StartedAt $startedAt -EndedAt $endedAt
    $reportExitCode = Invoke-Report -BuildExitCode 0 -RunExitCode $runExitCode -TimedOut $timedOut -TimeoutForcedKill $forcedKill -DurationSeconds $duration
    if ($runExitCode -eq 0 -and $reportExitCode -ne 0) {
        $runExitCode = $reportExitCode
    }
}

exit $runExitCode
