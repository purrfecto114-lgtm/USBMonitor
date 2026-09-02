# demo.ps1 - Windows smoke test for usbmon.exe (run on a real Windows
# machine or a windows-latest GitHub runner; PowerShell 7 recommended,
# Windows PowerShell 5.1 also works).
#
# CI runners have no physical USB devices, so the plug/unplug path is
# verified by BROADCASTING a real WM_DEVICECHANGE / DBT_DEVICEARRIVAL
# volume event (exactly what the OS sends when a volume arrives) to all
# top-level windows: the daemon's invisible top-level listener must
# catch it, wake after its 0.7s debounce, and log a round with
# "wake":"hot".  A message-only listener window (the old bug) does not
# receive broadcast messages and would fail this test.
#
# Assertions (same spirit as tools/demo.sh on Linux):
#   1.  --version prints "usbmon <semver>", exit 0 (+ exact match when
#       -Version is given)
#   2.  --help prints usage, exit 0
#   3.  unknown option rejected with exit code 2
#   4.  --list exits 0 (read-only round)
#   5.  --once exits 0 and writes a JSONL log whose every line parses
#       as JSON with start + round + stop events
#   6.  hooks.json is parsed (start event reports hooks=N)
#   7.  single-instance lock: second daemon refused with exit code 3
#   8.  GUI daemon stays alive and creates its invisible TOP-LEVEL
#       listener window (class "usbmonListen", matched by PID)
#   9.  a broadcast WM_DEVICECHANGE wakes the daemon: JSONL round with
#       wake="hot" appears within ~3s
#
# Usage:
#   pwsh tools/demo.ps1 [-ExePath .\usbmon.exe] [-Version 2.2.0]
# Exits non-zero when any assertion fails.

param(
    [string]$ExePath = ".\usbmon.exe",
    [string]$Version = ""
)

$ErrorActionPreference = "Stop"
# Native (exe) stderr must not trip $ErrorActionPreference (PS 7.3+):
$PSNativeCommandUseErrorActionPreference = $false

$script:Pass = 0
$script:Fail = 0
function Ok([string]$msg) { $script:Pass++; Write-Output "  >>> PASS: $msg" }
function Bad([string]$msg) { $script:Fail++; Write-Output "  >>> FAIL: $msg" }

$ExePath = (Resolve-Path -LiteralPath $ExePath).Path
$Root    = Join-Path $env:TEMP ("usbmon-win-demo-" + [guid]::NewGuid().ToString("N").Substring(0, 8))
New-Item -ItemType Directory -Path $Root | Out-Null
Write-Output "(binary under test: $ExePath)"
Write-Output "(scratch dir:      $Root)"

function Invoke-Usbmon {
    param([string[]]$ArgList)
    $out = $null
    try { $out = & $ExePath @ArgList 2>&1 } catch { $out = @("$_") }
    [pscustomobject]@{
        Out  = @($out | ForEach-Object { "$_" })
        Code = $LASTEXITCODE
    }
}

$script:BadJson = 0
function Read-JsonLines {
    param([string]$Path)
    $evs = @()
    if (-not (Test-Path -LiteralPath $Path)) { return ,$evs }
    foreach ($l in @(Get-Content -LiteralPath $Path)) {
        if ($l.Trim() -eq "") { continue }
        try   { $evs += ($l | ConvertFrom-Json) }
        catch { $script:BadJson++ }
    }
    return ,$evs
}

# --- 1) --version ------------------------------------------------------------
$r = Invoke-Usbmon @("--version")
if ($r.Code -eq 0 -and $r.Out.Count -ge 1 -and $r.Out[0] -match '^usbmon \d+\.\d+\.\d+$') {
    Ok ("--version prints 'usbmon <semver>' (exit 0): " + $r.Out[0])
} else {
    Bad ("--version: exit=" + $r.Code + " out=" + ($r.Out -join "|"))
}
if ($Version -ne "") {
    if ($r.Out.Count -ge 1 -and $r.Out[0] -eq "usbmon $Version") {
        Ok "version matches the release tag: usbmon $Version"
    } else {
        Bad "version mismatch: expected 'usbmon $Version', got '$($r.Out[0])'"
    }
}

# --- 2) --help ----------------------------------------------------------------
$r = Invoke-Usbmon @("--help")
if ($r.Code -eq 0 -and (($r.Out -join "`n") -match "Usage: usbmon")) {
    Ok "--help prints usage (exit 0)"
} else {
    Bad "--help: exit=$($r.Code)"
}

# --- 3) unknown option rejected ------------------------------------------------
$r = Invoke-Usbmon @("--definitely-not-an-option")
if ($r.Code -eq 2) {
    Ok "unknown option rejected with exit code 2"
} else {
    Bad "unknown option: expected exit 2, got $($r.Code)"
}

# --- 4) --list (read-only round; zero USB devices is fine on a runner) --------
$r = Invoke-Usbmon @("--list")
if ($r.Code -eq 0) {
    Ok "--list exits 0 (output lines: $($r.Out.Count))"
} else {
    Bad "--list exit=$($r.Code): $($r.Out -join '|')"
}

# --- 5) --once: JSONL log with start + round + stop ----------------------------
$Log1 = Join-Path $Root "once.jsonl"
$r = Invoke-Usbmon @("--once", "--log", $Log1)
if ($r.Code -eq 0) { Ok "--once exits 0" } else { Bad "--once exit=$($r.Code)" }
$script:BadJson = 0
$events = Read-JsonLines $Log1
if ($script:BadJson -eq 0 -and $events.Count -ge 3) {
    Ok "JSONL log has $($events.Count) lines, all valid JSON"
} else {
    Bad "JSONL: $script:BadJson invalid lines, $($events.Count) parsed"
}
$evNames = @($events | ForEach-Object { $_.ev })
if ($evNames -contains "start" -and $evNames -contains "round" -and $evNames -contains "stop") {
    Ok "JSONL contains start + round + stop events"
} else {
    Bad "JSONL events seen: $($evNames -join ',')"
}

# --- 6) hooks.json is parsed (hook count shows in the start event) -------------
$Hooks = Join-Path $Root "hooks.json"
@"
{
  "hooks": [
    { "name": "demo-notepad", "match_keys": ["*"],
      "command": ["C:\\Windows\\System32\\notepad.exe", "--never-used"], "enabled": true },
    { "name": "demo-clone", "match_keys": ["zzz*"],
      "command": ["usbmon-clone.exe"], "enabled": true }
  ]
}
"@ | Set-Content -LiteralPath $Hooks -Encoding UTF8
$Log2 = Join-Path $Root "hooks.jsonl"
$r = Invoke-Usbmon @("--once", "--log", $Log2, "--hooks", $Hooks)
if ($r.Code -eq 0) { Ok "--once with hooks.json exits 0" } else { Bad "--once hooks exit=$($r.Code)" }
$events2 = Read-JsonLines $Log2
$startEv = $events2 | Where-Object { $_.ev -eq "start" } | Select-Object -First 1
if ($startEv -and $startEv.detail -match "hooks=2") {
    Ok "hooks.json parsed: start event reports hooks=2"
} else {
    Bad "start detail: $($startEv.detail)"
}

# --- 7) single-instance lock (second daemon exits 3) ----------------------------
$LogD = Join-Path $Root "daemon.jsonl"
$proc = Start-Process -FilePath $ExePath `
    -ArgumentList @("--log", $LogD, "--no-gui", "--interval", "3600") `
    -PassThru -WindowStyle Hidden
Start-Sleep -Seconds 2
if (-not $proc.HasExited) {
    Ok "headless daemon (--no-gui) starts and stays alive"
} else {
    Bad "headless daemon exited early with code $($proc.ExitCode)"
}
$r = Invoke-Usbmon @("--log", $LogD, "--no-gui")
if ($r.Code -eq 3) {
    Ok "second daemon refused by single-instance lock (exit 3)"
} else {
    Bad "expected exit 3 for second instance, got $($r.Code)"
}
if ($proc -and -not $proc.HasExited) { Stop-Process -Id $proc.Id -Force }
Start-Sleep -Milliseconds 500

# --- 8) GUI daemon + invisible top-level listener window ------------------------
Add-Type -TypeDefinition @"
using System;
using System.Text;
using System.Collections.Generic;
using System.Runtime.InteropServices;

namespace UsbmonDemo {
    public class Win32 {
        private delegate bool EnumWindowsProc(IntPtr hWnd, IntPtr lParam);
        [DllImport("user32.dll")]
        private static extern bool EnumWindows(EnumWindowsProc cb, IntPtr lParam);
        [DllImport("user32.dll", CharSet = CharSet.Unicode)]
        private static extern int GetClassName(IntPtr hWnd, StringBuilder sb, int max);
        [DllImport("user32.dll")]
        private static extern uint GetWindowThreadProcessId(IntPtr hWnd, out uint pid);
        [DllImport("user32.dll", CharSet = CharSet.Unicode)]
        public static extern IntPtr SendMessageTimeoutW(IntPtr hWnd, uint msg,
            UIntPtr wParam, ref DEV_BROADCAST_VOLUME lParam, uint flags, uint timeout,
            out UIntPtr result);
        public static List<long> ListenerPids() {
            var pids = new List<long>();
            EnumWindows(delegate(IntPtr h, IntPtr l) {
                var sb = new StringBuilder(64);
                GetClassName(h, sb, 64);
                if (sb.ToString() == "usbmonListen") {
                    uint pid;
                    GetWindowThreadProcessId(h, out pid);
                    pids.Add(pid);
                }
                return true;
            }, IntPtr.Zero);
            return pids;
        }
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct DEV_BROADCAST_VOLUME {
        public uint dbcv_size;        // must be sizeof(struct)
        public uint dbcv_devicetype;  // 2 = DBT_DEVTYP_VOLUME
        public uint dbcv_unitmask;    // bit 0 = drive A:
        public uint dbcv_flags;
    }

    public class DevBcast {
        // Broadcast DBT_DEVICEARRIVAL for volume P: to every top-level
        // window -- byte-for-byte what the OS does when a volume mounts.
        public static void BroadcastVolumeArrival() {
            var dbv = new DEV_BROADCAST_VOLUME();
            dbv.dbcv_size = (uint)Marshal.SizeOf(typeof(DEV_BROADCAST_VOLUME));
            dbv.dbcv_devicetype = 2;
            dbv.dbcv_unitmask = 0x10000000;   // bit 28 = drive P:
            dbv.dbcv_flags = 0;
            UIntPtr res;
            Win32.SendMessageTimeoutW(new IntPtr(0xFFFF), 0x0219, new UIntPtr(0x8000),
                ref dbv, 2, 1000, out res);
            // HWND_BROADCAST, WM_DEVICECHANGE, DBT_DEVICEARRIVAL, SMTO_ABORTIFHUNG
        }
    }
}
"@

$LogG = Join-Path $Root "daemon-gui.jsonl"
$proc2 = Start-Process -FilePath $ExePath `
    -ArgumentList @("--log", $LogG, "--interval", "3600") `
    -PassThru -WindowStyle Hidden
Start-Sleep -Seconds 2
if (-not $proc2.HasExited) {
    Ok "GUI daemon alive (GUI thread + windows created)"
} else {
    Bad "GUI daemon exited early with code $($proc2.ExitCode)"
}
$pids = [UsbmonDemo.Win32]::ListenerPids()
if (@($pids) -contains $proc2.Id) {
    Ok "invisible TOP-LEVEL listener window exists (class usbmonListen, pid $($proc2.Id))"
} else {
    Bad "listener window for pid $($proc2.Id) not found (found pids: $($pids -join ','))"
}

# --- 9) WM_DEVICECHANGE broadcast must wake the daemon (hot path) ---------------
[UsbmonDemo.DevBcast]::BroadcastVolumeArrival()
Start-Sleep -Seconds 3     # 0.7s debounce + scan + log flush
$eventsG = Read-JsonLines $LogG
$hotRounds = @($eventsG | Where-Object { $_.ev -eq "round" -and $_.wake -eq "hot" })
if ($hotRounds.Count -ge 1) {
    Ok ("WM_DEVICECHANGE broadcast woke daemon: round wake=hot logged (scan_ms=" + $hotRounds[0].scan_ms + ")")
} else {
    Bad "no 'wake':'hot' round after broadcast -- hot path is not receiving device events"
}

# --- cleanup ---------------------------------------------------------------------
if ($proc2 -and -not $proc2.HasExited) { Stop-Process -Id $proc2.Id -Force }
Remove-Item -LiteralPath $Root -Recurse -Force -ErrorAction SilentlyContinue

Write-Output ""
Write-Output "result: $script:Pass passed, $script:Fail failed"
if ($script:Fail -ne 0) { exit 1 }
exit 0
