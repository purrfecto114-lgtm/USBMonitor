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
#  10.  the exe is a GUI-subsystem PE (IMAGE_SUBSYSTEM_WINDOWS_GUI):
#       double-clicking it must never open a black console window
#  11.  the system-tray icon installs (Shell_NotifyIcon; the script
#       starts explorer.exe when the runner has no shell running)
#  12.  LEFT-click menu content: fresh scan -> per-volume entries with
#       打开 / 在资源管理器中显示 / 安全弹出 — or the honest empty state
#  13.  RIGHT-click menu content: 状态 / 立即重新扫描 / 工具 / 随系统启动 / 退出
#  14.  tray-triggered rescan: the tray rescan message produces another
#       wake="hot" round (same path the menu item uses)
#  15.  tray quit: the tray quit message exits the daemon with code 0
#       and a JSONL stop event whose reason is "tray-quit"
#
# Tray internals (10-15) are exercised by posting the exact window
# messages a real tray click delivers (USBMON_TRAY_TEST additionally
# asks the daemon to dump menu contents instead of popping menus up).
#
# Usage:
#   pwsh tools/demo.ps1 [-ExePath .\usbmon.exe] [-Version 2.3.0]
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
        [DllImport("user32.dll", SetLastError = true)]
        public static extern bool PostMessageW(IntPtr hWnd, uint msg,
            UIntPtr wParam, IntPtr lParam);
        public static IntPtr FindListenerHwnd(uint pid) {
            IntPtr found = IntPtr.Zero;
            EnumWindows(delegate(IntPtr h, IntPtr l) {
                if (found != IntPtr.Zero) return false;
                var sb = new StringBuilder(64);
                GetClassName(h, sb, 64);
                if (sb.ToString() == "usbmonListen") {
                    uint wpid;
                    GetWindowThreadProcessId(h, out wpid);
                    if (wpid == pid) found = h;
                }
                return true;
            }, IntPtr.Zero);
            return found;
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

# USBMON_TRAY_TEST: daemon appends tray install result + menu dumps here
# (menus are dumped instead of popped up, so a headless runner can still
# assert their CONTENT).
$TrayFile = Join-Path $Root "tray-test.txt"
$env:USBMON_TRAY_TEST = $TrayFile

# CI runners may run without explorer.exe; a tray icon needs a shell.
# Start one when absent — a no-op on real desktops (already running).
try {
    if (-not (Get-Process -Name explorer -ErrorAction SilentlyContinue)) {
        Write-Output "(no explorer running — starting one for the tray test)"
        Start-Process explorer.exe
        Start-Sleep -Seconds 3
    }
} catch {
    Write-Output "(explorer start attempt failed — tray test may report it)"
}

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

function Read-TrayLog {
    if (Test-Path -LiteralPath $TrayFile) {
        return (Get-Content -LiteralPath $TrayFile -Raw -Encoding UTF8)
    }
    return ""
}
function Truncate-From([string]$text, [string]$marker) {
    $i = $text.LastIndexOf($marker)
    if ($i -ge 0) { return $text.Substring($i) }
    return ""
}

# --- 10) GUI subsystem (no black console window on double-click) ---------------
$bytes  = [IO.File]::ReadAllBytes($ExePath)
$peOff  = [BitConverter]::ToInt32($bytes, 0x3C)
$subsys = [BitConverter]::ToUInt16($bytes, $peOff + 24 + 68)   # PE32+ optional header
if ($subsys -eq 2) {
    Ok "PE subsystem = WINDOWS_GUI (2): double-click opens no console window"
} else {
    Bad "PE subsystem = $subsys (expected 2 = GUI)"
}

# --- 11) system-tray icon installed --------------------------------------------
$trayTxt = Read-TrayLog
if ($trayTxt -match 'icon_add ok' -or $trayTxt -match 'icon_readd ok') {
    Ok "system-tray icon installed (Shell_NotifyIcon)"
} else {
    Bad "tray icon not installed; tray log: $(($trayTxt -replace "`n", ' | ').Trim())"
}

# --- 12) LEFT-click menu: USB devices (打开 / 显示 / 安全弹出) ------------------
$hwnd = [UsbmonDemo.Win32]::FindListenerHwnd([uint32]$proc2.Id)
if ($hwnd -ne [IntPtr]::Zero) {
    # WM_APP+3 (UMWM_TRAY) with LPARAM=WM_LBUTTONUP: exactly what a real
    # left click on the tray icon delivers.
    [UsbmonDemo.Win32]::PostMessageW($hwnd, 0x8003, [UIntPtr]::Zero, [IntPtr]0x0202) | Out-Null
    Start-Sleep -Milliseconds 800
    $menuLeft = Truncate-From (Read-TrayLog) "menu left"
    if ($menuLeft -ne "") {
        if ($menuLeft -match '安全弹出' -or $menuLeft -match '当前没有检测到 USB 存储设备') {
            Ok "left-click menu built from a fresh scan (volume entries or honest empty state)"
        } else {
            Bad "left menu has neither volume entries nor empty state: $(($menuLeft -replace "`n", ' | ').Trim())"
        }
    } else {
        Bad "no 'menu left' dump after left-click message"
    }
} else {
    Bad "listener hwnd not found for pid $($proc2.Id) — cannot inject tray clicks"
}

# --- 13) RIGHT-click menu: 状态 / 立即重新扫描 / 工具 / 随系统启动 / 退出 -------
if ($hwnd -ne [IntPtr]::Zero) {
    [UsbmonDemo.Win32]::PostMessageW($hwnd, 0x8003, [UIntPtr]::Zero, [IntPtr]0x0204) | Out-Null
    Start-Sleep -Milliseconds 800
    $menuRight = Truncate-From (Read-TrayLog) "menu right"
    $need = @('状态：', '立即重新扫描', '打开日志目录', '随系统启动', '退出')
    $missing = @($need | Where-Object { $menuRight -notmatch [regex]::Escape($_) })
    if ($menuRight -ne "" -and $missing.Count -eq 0) {
        Ok "right-click menu complete (状态/重新扫描/工具/随系统启动/退出)"
    } else {
        Bad "right menu missing: $($missing -join ',') | got: $(($menuRight -replace "`n", ' | ').Trim())"
    }
}

# --- 14) tray-triggered rescan produces another hot round ------------------------
$eventsG2 = Read-JsonLines $LogG
$hotBefore = @($eventsG2 | Where-Object { $_.ev -eq "round" -and $_.wake -eq "hot" }).Count
if ($hwnd -ne [IntPtr]::Zero) {
    [UsbmonDemo.Win32]::PostMessageW($hwnd, 0x8004, [UIntPtr]::Zero, [IntPtr]::Zero) | Out-Null
    Start-Sleep -Seconds 3      # 0.7s debounce + round
    $eventsG3 = Read-JsonLines $LogG
    $hotAfter = @($eventsG3 | Where-Object { $_.ev -eq "round" -and $_.wake -eq "hot" }).Count
    if ($hotAfter -gt $hotBefore) {
        Ok "tray 立即重新扫描 message triggered another wake=hot round"
    } else {
        Bad "no new hot round after tray rescan (before=$hotBefore after=$hotAfter)"
    }
}

# --- 15) tray quit: graceful shutdown, exit 0, stop reason tray-quit -------------
if ($hwnd -ne [IntPtr]::Zero) {
    [UsbmonDemo.Win32]::PostMessageW($hwnd, 0x8005, [UIntPtr]::Zero, [IntPtr]::Zero) | Out-Null
    $exited = $false
    foreach ($i in 1..20) {
        if ($proc2.HasExited) { $exited = $true; break }
        Start-Sleep -Milliseconds 250
    }
    if ($exited -and $proc2.ExitCode -eq 0) {
        Ok "tray 退出 shut the daemon down cleanly (exit code 0)"
    } else {
        Bad "tray quit did not exit cleanly (exited=$exited code=$($proc2.ExitCode))"
    }
    $eventsG4 = Read-JsonLines $LogG
    $stopEv = $eventsG4 | Where-Object { $_.ev -eq "stop" } | Select-Object -Last 1
    if ($stopEv -and $stopEv.detail -eq "tray-quit") {
        Ok "JSONL stop event records reason 'tray-quit'"
    } else {
        Bad "stop event reason: $($stopEv.detail) (expected 'tray-quit')"
    }
}

# --- cleanup ---------------------------------------------------------------------
if ($proc2 -and -not $proc2.HasExited) { Stop-Process -Id $proc2.Id -Force }
Remove-Item Env:USBMON_TRAY_TEST -ErrorAction SilentlyContinue
Remove-Item -LiteralPath $Root -Recurse -Force -ErrorAction SilentlyContinue

Write-Output ""
Write-Output "result: $script:Pass passed, $script:Fail failed"
if ($script:Fail -ne 0) { exit 1 }
exit 0
