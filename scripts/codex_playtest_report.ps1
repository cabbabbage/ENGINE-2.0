param(
    [Parameter(Mandatory=$true)][string]$RepoRoot,
    [Parameter(Mandatory=$true)][string]$ReportPath,
    [Parameter(Mandatory=$true)][string]$MetadataPath,
    [int]$BuildExitCode = 0,
    [int]$RunExitCode = 0,
    [object]$TimedOut = $false,
    [object]$TimeoutForcedKill = $false,
    [double]$DurationSeconds = 0,
    [string]$Map = "forrest",
    [int]$FrameLimit = 3600,
    [string]$LauncherLogPath,
    [string]$StdoutPath,
    [string]$StderrPath
)

$ErrorActionPreference = "Stop"

$RepoRoot = (Resolve-Path $RepoRoot).Path
$LogPath = Join-Path $RepoRoot "log.txt"
$FrameStatsPath = Join-Path $RepoRoot "runtime_frame_stats.csv"

function Convert-ToBool {
    param($Value)
    if ($Value -is [bool]) {
        return $Value
    }
    if ($null -eq $Value) {
        return $false
    }
    $text = ([string]$Value).Trim()
    if ($text -match '^(1|true|yes|on)$') {
        return $true
    }
    return $false
}

$TimedOut = Convert-ToBool $TimedOut
$TimeoutForcedKill = Convert-ToBool $TimeoutForcedKill

function Read-TextLines([string]$Path) {
    if ($Path -and (Test-Path $Path)) {
        return @(Get-Content -Path $Path -ErrorAction SilentlyContinue)
    }
    return @()
}

function To-DoubleOrNull($Value) {
    if ($null -eq $Value -or $Value -eq "") {
        return $null
    }
    $parsed = 0.0
    if ([double]::TryParse([string]$Value, [System.Globalization.NumberStyles]::Float, [System.Globalization.CultureInfo]::InvariantCulture, [ref]$parsed)) {
        return $parsed
    }
    return $null
}

function Percentile {
    param([double[]]$Values, [double]$Percent)
    if (-not $Values -or $Values.Count -eq 0) {
        return $null
    }
    $sorted = @($Values | Sort-Object)
    $index = [int][Math]::Ceiling(($Percent / 100.0) * $sorted.Count) - 1
    $index = [Math]::Max(0, [Math]::Min($sorted.Count - 1, $index))
    return $sorted[$index]
}

function Format-Ms($Value) {
    if ($null -eq $Value) {
        return "n/a"
    }
    return ("{0:N2} ms" -f [double]$Value)
}

function Format-Number($Value) {
    if ($null -eq $Value) {
        return "n/a"
    }
    return ("{0:N2}" -f [double]$Value)
}

function Get-MetricValues {
    param([object[]]$Rows, [string]$Metric)
    $values = New-Object System.Collections.Generic.List[double]
    foreach ($row in $Rows) {
        if (-not ($row.PSObject.Properties.Name -contains $Metric)) {
            continue
        }
        $v = To-DoubleOrNull $row.$Metric
        if ($null -ne $v) {
            $values.Add($v)
        }
    }
    return [double[]]$values.ToArray()
}

function Max-Metric {
    param([object[]]$Rows, [string]$Metric)
    $values = Get-MetricValues -Rows $Rows -Metric $Metric
    if ($values.Count -eq 0) {
        return $null
    }
    return ($values | Measure-Object -Maximum).Maximum
}

function Add-SectionLine {
    param([System.Collections.Generic.List[string]]$Lines, [string]$Text = "")
    $Lines.Add($Text) | Out-Null
}

$launcherLines = Read-TextLines $LauncherLogPath
$stdoutLines = Read-TextLines $StdoutPath
$stderrLines = Read-TextLines $StderrPath
$logLines = Read-TextLines $LogPath
$runtimeLogSource = $LogPath
if ($logLines.Count -eq 0 -and $stdoutLines.Count -gt 0) {
    $logLines = $stdoutLines
    $runtimeLogSource = $StdoutPath
}

$metadata = $null
if (Test-Path $MetadataPath) {
    try {
        $metadata = Get-Content -Path $MetadataPath -Raw | ConvertFrom-Json
    } catch {
        $metadata = $null
    }
}

$rows = @()
if (Test-Path $FrameStatsPath) {
    try {
        $rows = @(Import-Csv -Path $FrameStatsPath)
    } catch {
        $rows = @()
    }
}

$mapAutoSelected = ($logLines -match "Auto-selecting map via VIBBLE_AUTOSTART_MAP: $([regex]::Escape($Map))").Count -gt 0
$mapSelected = $mapAutoSelected -or (($logLines -match "Map selected: $([regex]::Escape($Map))").Count -gt 0)
$loopStarted = (($logLines -match "Game loop started\.").Count -gt 0) -or
               (($logLines -match "\[MenuUI\] Runtime frame limit:").Count -gt 0)
$finalLogLine = if ($logLines.Count -gt 0) { $logLines[-1] } else { "<no log lines>" }

$frameMetric = Get-MetricValues -Rows $rows -Metric "main.frame_total_ms"
$frameCount = $rows.Count
$avgFrame = if ($frameMetric.Count -gt 0) { ($frameMetric | Measure-Object -Average).Average } else { $null }
$p95Frame = Percentile -Values $frameMetric -Percent 95
$maxFrame = if ($frameMetric.Count -gt 0) { ($frameMetric | Measure-Object -Maximum).Maximum } else { $null }

$worstFrames = @()
if ($rows.Count -gt 0 -and ($rows[0].PSObject.Properties.Name -contains "main.frame_total_ms")) {
    $worstFrames = @(
        $rows |
            ForEach-Object {
                $frameId = if ($_.PSObject.Properties.Name -contains "frame_id") { $_.frame_id } elseif ($_.PSObject.Properties.Name -contains "assets.frame_id") { $_."assets.frame_id" } else { "" }
                [pscustomobject]@{
                    frame = $frameId
                    total_ms = To-DoubleOrNull $_."main.frame_total_ms"
                    assets_update_ms = if ($_.PSObject.Properties.Name -contains "main.assets_update_ms") { To-DoubleOrNull $_."main.assets_update_ms" } else { $null }
                    world_ms = if ($_.PSObject.Properties.Name -contains "assets.world_ms") { To-DoubleOrNull $_."assets.world_ms" } else { $null }
                    render_ms = if ($_.PSObject.Properties.Name -contains "assets.render_ms") { To-DoubleOrNull $_."assets.render_ms" } else { $null }
                }
            } |
            Where-Object { $null -ne $_.total_ms } |
            Sort-Object total_ms -Descending |
            Select-Object -First 8
    )
}

$warningLines = @($logLines | Where-Object { $_ -match "\[(WARN|WARNING)\]" })
$errorLines = @($logLines | Where-Object { $_ -match "\[(ERROR|FATAL)\]" })

$suspiciousMetrics = @(
    "main.assets_update_ms",
    "assets.world_ms",
    "assets.visibility_ms",
    "assets.runtime_effects_ms",
    "assets.render_ms",
    "movement.player_path_blocked_ms",
    "movement.player_path_blocked_checks",
    "assets.active_count",
    "assets.filtered_active_count",
    "assets.active_fail_open_frames",
    "assets.startup_safety_active"
)

$freezeSignals = New-Object System.Collections.Generic.List[string]
if ($TimedOut) {
    $freezeSignals.Add("Process hit the hard timeout.") | Out-Null
}
if ($TimeoutForcedKill) {
    $freezeSignals.Add("Process required force kill after close request.") | Out-Null
}
if (-not $loopStarted) {
    $freezeSignals.Add("Game loop marker was not found.") | Out-Null
}
if ($FrameLimit -gt 0 -and $frameCount -gt 0 -and $frameCount -lt [Math]::Min($FrameLimit, 300)) {
    $freezeSignals.Add("Only $frameCount frame-stat rows were captured; expected closer to $FrameLimit unless startup ended early.") | Out-Null
}
if ($FrameLimit -gt 0 -and $frameCount -eq 0) {
    $freezeSignals.Add("No frame-stat rows were captured.") | Out-Null
}
if ($maxFrame -ne $null -and $maxFrame -gt 1000.0) {
    $freezeSignals.Add("At least one frame took more than 1000 ms.") | Out-Null
} elseif ($maxFrame -ne $null -and $maxFrame -gt 250.0) {
    $freezeSignals.Add("At least one frame took more than 250 ms.") | Out-Null
}
if ($finalLogLine -match "Loading|Creating map|Generating|Start for map|Spawning assets") {
    $freezeSignals.Add("Final log line appears to be in startup/loading work: $finalLogLine") | Out-Null
}

$likely = New-Object System.Collections.Generic.List[string]
if ($errorLines.Count -gt 0) {
    $likely.Add("Start with ERROR/FATAL log entries; they are the clearest failure signal in this run.") | Out-Null
}
if ($TimedOut -or $TimeoutForcedKill) {
    $likely.Add("Treat this run as a hang/freeze repro because the harness had to intervene.") | Out-Null
}
$assetsMax = Max-Metric -Rows $rows -Metric "main.assets_update_ms"
$worldMax = Max-Metric -Rows $rows -Metric "assets.world_ms"
$visibilityMax = Max-Metric -Rows $rows -Metric "assets.visibility_ms"
$renderMax = Max-Metric -Rows $rows -Metric "assets.render_ms"
$pathBlockedMax = Max-Metric -Rows $rows -Metric "movement.player_path_blocked_ms"
$intentValues = Get-MetricValues -Rows $rows -Metric "movement.player_has_intent"
$intentFrameCount = @($intentValues | Where-Object { $_ -ne 0.0 }).Count
$inputXValues = Get-MetricValues -Rows $rows -Metric "movement.player_input_x"
$inputYValues = Get-MetricValues -Rows $rows -Metric "movement.player_input_y"
$codexDriverValues = Get-MetricValues -Rows $rows -Metric "codex_playtest.input_driver"
$codexDriverFrameCount = @($codexDriverValues | Where-Object { $_ -ne 0.0 }).Count
$longSegmentFrameCount = 0
$burstSegmentFrameCount = 0
if ($rows.Count -gt 0 -and ($rows[0].PSObject.Properties.Name -contains "codex_playtest.segment_kind")) {
    $longSegmentFrameCount = @($rows | Where-Object { $_."codex_playtest.segment_kind" -eq "long" }).Count
    $burstSegmentFrameCount = @($rows | Where-Object { $_."codex_playtest.segment_kind" -eq "burst" }).Count
}
if ($assetsMax -ne $null -and $assetsMax -gt 100.0) {
    $likely.Add("Large `main.assets_update_ms` spikes point at runtime update work rather than present/input pacing.") | Out-Null
}
if ($worldMax -ne $null -and $worldMax -gt 100.0) {
    $likely.Add("Large `assets.world_ms` spikes point at world/player/active-set update work.") | Out-Null
}
if ($visibilityMax -ne $null -and $visibilityMax -gt 100.0) {
    $likely.Add("Large `assets.visibility_ms` spikes point at active visibility traversal or active-set publishing.") | Out-Null
}
if ($renderMax -ne $null -and $renderMax -gt 100.0) {
    $likely.Add("Large `assets.render_ms` spikes point at renderer/GPU handoff work.") | Out-Null
}
if ($pathBlockedMax -ne $null -and $pathBlockedMax -gt 20.0) {
    $likely.Add("Path-blocking cost is elevated; inspect player movement collision/path fallback metrics.") | Out-Null
}
if ($likely.Count -eq 0) {
    $likely.Add("No single dominant freeze signal was detected; compare worst frames against the suspicious metric table.") | Out-Null
}

$report = New-Object System.Collections.Generic.List[string]
Add-SectionLine $report "# Codex Playtest Report"
Add-SectionLine $report
Add-SectionLine $report "Generated: $((Get-Date).ToString('yyyy-MM-dd HH:mm:ss zzz'))"
Add-SectionLine $report
Add-SectionLine $report "## Result"
Add-SectionLine $report "- Build exit code: $BuildExitCode"
Add-SectionLine $report "- Run exit code: $RunExitCode"
Add-SectionLine $report "- Duration: $(Format-Number $DurationSeconds) seconds"
Add-SectionLine $report "- Timed out: $TimedOut"
Add-SectionLine $report "- Forced kill: $TimeoutForcedKill"
Add-SectionLine $report "- Requested map: $Map"
Add-SectionLine $report "- Map selected: $mapSelected"
Add-SectionLine $report "- Game loop started: $loopStarted"
Add-SectionLine $report "- Frame limit: $FrameLimit"
Add-SectionLine $report
Add-SectionLine $report "## Frame Stats"
Add-SectionLine $report "- Rows captured: $frameCount"
Add-SectionLine $report "- Average frame: $(Format-Ms $avgFrame)"
Add-SectionLine $report "- P95 frame: $(Format-Ms $p95Frame)"
Add-SectionLine $report "- Max frame: $(Format-Ms $maxFrame)"
Add-SectionLine $report
Add-SectionLine $report "### Player Input"
Add-SectionLine $report "- Codex driver frames: $codexDriverFrameCount"
Add-SectionLine $report "- Movement-intent frames: $intentFrameCount"
Add-SectionLine $report "- Long-hold frames: $longSegmentFrameCount"
Add-SectionLine $report "- Burst frames: $burstSegmentFrameCount"
if ($inputXValues.Count -gt 0) {
    Add-SectionLine $report ("- Input X range: {0} to {1}" -f (($inputXValues | Measure-Object -Minimum).Minimum), (($inputXValues | Measure-Object -Maximum).Maximum))
}
if ($inputYValues.Count -gt 0) {
    Add-SectionLine $report ("- Input Y range: {0} to {1}" -f (($inputYValues | Measure-Object -Minimum).Minimum), (($inputYValues | Measure-Object -Maximum).Maximum))
}
Add-SectionLine $report
Add-SectionLine $report "### Suspicious Metrics"
foreach ($metric in $suspiciousMetrics) {
    $values = Get-MetricValues -Rows $rows -Metric $metric
    if ($values.Count -eq 0) {
        Add-SectionLine $report ("- {0}: n/a" -f $metric)
        continue
    }
    $avg = ($values | Measure-Object -Average).Average
    $max = ($values | Measure-Object -Maximum).Maximum
    Add-SectionLine $report ("- {0}: avg {1}, max {2}" -f $metric, (Format-Number $avg), (Format-Number $max))
}
Add-SectionLine $report
Add-SectionLine $report "### Worst Frames"
if ($worstFrames.Count -eq 0) {
    Add-SectionLine $report "- No frame rows with `main.frame_total_ms` were available."
} else {
    foreach ($frame in $worstFrames) {
        Add-SectionLine $report "- Frame $($frame.frame): total $(Format-Ms $frame.total_ms), assets $(Format-Ms $frame.assets_update_ms), world $(Format-Ms $frame.world_ms), render $(Format-Ms $frame.render_ms)"
    }
}
Add-SectionLine $report
Add-SectionLine $report "## Freeze Signals"
if ($freezeSignals.Count -eq 0) {
    Add-SectionLine $report "- No hard freeze signal detected by the harness."
} else {
    foreach ($signal in $freezeSignals) {
        Add-SectionLine $report "- $signal"
    }
}
Add-SectionLine $report
Add-SectionLine $report "## Log Issues"
Add-SectionLine $report "- Warnings: $($warningLines.Count)"
Add-SectionLine $report "- Errors/Fatal: $($errorLines.Count)"
Add-SectionLine $report ("- Final log line: {0}" -f $finalLogLine)
if ($warningLines.Count -gt 0) {
    Add-SectionLine $report
    Add-SectionLine $report "### Recent Warnings"
    foreach ($line in ($warningLines | Select-Object -Last 12)) {
        Add-SectionLine $report ("- {0}" -f $line)
    }
}
if ($errorLines.Count -gt 0) {
    Add-SectionLine $report
    Add-SectionLine $report "### Recent Errors"
    foreach ($line in ($errorLines | Select-Object -Last 12)) {
        Add-SectionLine $report ("- {0}" -f $line)
    }
}
Add-SectionLine $report
Add-SectionLine $report "## Likely Investigation Points"
foreach ($item in $likely) {
    Add-SectionLine $report "- $item"
}
Add-SectionLine $report
Add-SectionLine $report "## Files"
Add-SectionLine $report ("- Runtime log: {0}" -f $runtimeLogSource)
Add-SectionLine $report ("- Frame stats: {0}" -f $FrameStatsPath)
Add-SectionLine $report ("- Launcher log: {0}" -f $LauncherLogPath)
Add-SectionLine $report ("- stdout: {0}" -f $StdoutPath)
Add-SectionLine $report ("- stderr: {0}" -f $StderrPath)

$parent = Split-Path -Parent $ReportPath
if ($parent -and -not (Test-Path $parent)) {
    New-Item -ItemType Directory -Force -Path $parent | Out-Null
}
$report | Set-Content -Path $ReportPath -Encoding UTF8
Write-Host "[codex_playtest_report.ps1] Wrote report: $ReportPath"
