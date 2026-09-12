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
#   5b. --startup-status / --install-startup / --uninstall-startup CLI
#       roundtrip (v1.1.1 parity): HKCU Run value appears/disappears and
#       the status output flips 未启用 -> 已启用 -> 未启用
#   6.  hooks.json is parsed (start event reports hooks=N)
#   7.  single-instance lock: second daemon refused with exit code 3
#  16.  device panel window created (class usbmonToast2, owned by the
#       daemon) — run as a DEDICATED daemon instance between 7 and 8,
#       because runners have no USB devices: USBMON_PANEL_TEST feeds a
#       fixed 3-device evidence set through the full production path
#       (daemon thread -> um_toast_model -> UMWM_PANEL heap marshaling
#       -> GUI thread copy -> window)
#  17.  panel content dump: headline/subtitle + storage (E:+F:) + dock
#       + pen rows
#  18.  panel interactions: row click + bottom 打开 button both fire
#       open actions (logged by the callback -> tray volume action chain)
#  19.  展开 grows the real window (SetWindowPos geometry, not just repaint)
#  20.  Esc hides the panel and logs the close action
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
#  14b. async safe-eject (v1.1.1 parity): UMWM_TRAY_EJECT_TEST drives the
#       full worker chain headlessly — GUI thread returns immediately,
#       worker runs the IOCTL, result toast is marshaled back and the tray
#       log records the eject result; the daemon must stay alive
#  15.  tray quit: the tray quit message exits the daemon with code 0
#       and a JSONL stop event whose reason is "tray-quit"
#
# Tray internals (10-15) are exercised by posting the exact window
# messages a real tray click delivers (USBMON_TRAY_TEST additionally
# asks the daemon to dump menu contents instead of popping menus up).
# Panel internals (16-20) are exercised the same way: clicks and keys
# are posted as the exact window messages real input delivers.
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
    param([string[]]$ArgList, [int]$TimeoutSec = 60)
    # PowerShell only WAITS for console-subsystem executables; a GUI-
    # subsystem binary (usbmon.exe since -mwindows) returns immediately
    # from the call operator with $LASTEXITCODE unset.  Start the process
    # explicitly, redirect stdout/stderr to files, and wait on the process
    # handle with a timeout — identical semantics for console builds.
    $so = [IO.Path]::GetTempFileName()
    $se = [IO.Path]::GetTempFileName()
    try {
        $p = Start-Process -FilePath $ExePath -ArgumentList $ArgList `
                -WindowStyle Hidden -PassThru `
                -RedirectStandardOutput $so -RedirectStandardError $se
        if (-not $p.WaitForExit($TimeoutSec * 1000)) {
            Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
            $p.WaitForExit(5000) | Out-Null
            return [pscustomobject]@{ Out = @("TIMEOUT after $TimeoutSec s (process killed)"); Code = -1 }
        }
        $p.Refresh()
        $out = @()
        foreach ($f in @($so, $se)) {
            if ((Test-Path -LiteralPath $f) -and ((Get-Item -LiteralPath $f).Length -gt 0)) {
                $out += @(Get-Content -LiteralPath $f)
            }
        }
        [pscustomobject]@{
            Out  = @($out | ForEach-Object { "$_" })
            Code = $p.ExitCode
        }
    } finally {
        Remove-Item -LiteralPath $so, $se -Force -ErrorAction SilentlyContinue
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

# --- 5b) startup CLI roundtrip: HKCU Run appears/disappears -------------------
$RunKeyPs = "HKCU:\Software\Microsoft\Windows\CurrentVersion\Run"
$r = Invoke-Usbmon @("--startup-status")
if ($r.Code -eq 0 -and (($r.Out -join "") -match "未启用")) {
    Ok "--startup-status exits 0 and reports 未启用 (fresh)"
} else {
    Bad "--startup-status fresh: exit=$($r.Code) out=$($r.Out -join '|')"
}
$r = Invoke-Usbmon @("--install-startup")
$runVal = (Get-ItemProperty -Path $RunKeyPs -Name "usbmon" -ErrorAction SilentlyContinue).usbmon
if ($r.Code -eq 0 -and $runVal) {
    Ok "--install-startup writes HKCU Run usbmon = $runVal"
} else {
    Bad "--install-startup: exit=$($r.Code) registry value='$runVal'"
}
$r = Invoke-Usbmon @("--startup-status")
if ($r.Code -eq 0 -and (($r.Out -join "") -match "已启用")) {
    Ok "--startup-status reports 已启用 after install"
} else {
    Bad "--startup-status post-install: exit=$($r.Code) out=$($r.Out -join '|')"
}
$r = Invoke-Usbmon @("--uninstall-startup")
$runVal2 = (Get-ItemProperty -Path $RunKeyPs -Name "usbmon" -ErrorAction SilentlyContinue).usbmon
if ($r.Code -eq 0 -and -not $runVal2) {
    Ok "--uninstall-startup removes the HKCU Run value"
} else {
    Bad "--uninstall-startup: exit=$($r.Code) value still='$runVal2'"
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
        public static List<long> WindowsByClassAndPid(string className, uint pid) {
            var hwnds = new List<long>();
            EnumWindows(delegate(IntPtr h, IntPtr l) {
                var sb = new StringBuilder(64);
                GetClassName(h, sb, 64);
                if (sb.ToString() == className) {
                    uint wpid;
                    GetWindowThreadProcessId(h, out wpid);
                    if (wpid == pid) hwnds.Add(h.ToInt64());
                }
                return true;
            }, IntPtr.Zero);
            return hwnds;
        }
        [DllImport("user32.dll", SetLastError = true)]
        public static extern bool PostMessageW(IntPtr hWnd, uint msg,
            UIntPtr wParam, IntPtr lParam);
        [DllImport("user32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        public static extern IntPtr FindWindowW(string className, string windowName);
        [DllImport("user32.dll")]
        public static extern bool GetClientRect(IntPtr hWnd, out RECT rc);
        [DllImport("user32.dll")]
        public static extern bool IsWindowVisible(IntPtr hWnd);
        [StructLayout(LayoutKind.Sequential)]
        public struct RECT { public int Left, Top, Right, Bottom; }
        public static uint WindowPid(IntPtr hWnd) {
            uint pid;
            GetWindowThreadProcessId(hWnd, out pid);
            return pid;
        }
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

# --- 16-20) device panel (DEDICATED daemon instance; run before test 8) ------
# Runners have no USB devices to plug, so USBMON_PANEL_TEST feeds a fixed
# 3-device evidence set (dual-partition stick E:+F:, a dock, an HID pen)
# through the FULL production path: daemon-thread model build -> heap
# um_toast_model marshaled via UMWM_PANEL -> GUI-thread copy -> single-
# instance window.  Panel shows dump their content and every action
# callback logs a line, so the whole chain is assertable headlessly.
$PanelFile = Join-Path $Root "panel-test.txt"
$LogP = Join-Path $Root "panel.jsonl"
$env:USBMON_PANEL_TEST = $PanelFile

$procP = Start-Process -FilePath $ExePath `
    -ArgumentList @("--log", $LogP, "--interval", "3600") `
    -PassThru -WindowStyle Hidden
Start-Sleep -Seconds 2

# 16) panel window exists and belongs to this daemon instance
$panelHwnd = [IntPtr]::Zero
foreach ($h in [UsbmonDemo.Win32]::WindowsByClassAndPid("usbmonToast2", [uint32]$procP.Id)) {
    $panelHwnd = [IntPtr]$h
    break
}
if ($panelHwnd -ne [IntPtr]::Zero) {
    $panelOwner = [UsbmonDemo.Win32]::WindowPid($panelHwnd)
    if ($panelOwner -eq [uint32]$procP.Id) {
        Ok "device panel window created (class usbmonToast2, owned by the daemon)"
    } else {
        Bad "panel window owned by pid $panelOwner, expected $($procP.Id)"
    }
} else {
    Bad "device panel window (class usbmonToast2) not found"
}

# 17) panel content: model -> window -> dump (storage + dock + pen rows)
$panelTxt = ""
if (Test-Path -LiteralPath $PanelFile) {
    $panelTxt = Get-Content -LiteralPath $PanelFile -Raw -Encoding UTF8
}
$needP = @('panel show', 'USB 设备监控', '3 个设备 · 2 个卷',
           'E:、F: 可移动磁盘', 'USB 拓展坞 / 集线器', 'USB HID 设备')
$missingP = @($needP | Where-Object { $panelTxt -notlike "*$_*" })
if ($panelTxt -ne "" -and $missingP.Count -eq 0) {
    Ok "panel content: headline/subtitle + storage(E:+F:) + dock + pen rows"
} else {
    Bad "panel content missing: $($missingP -join ' | ')"
}

# 18) row click + bottom 打开 button both fire open actions
#     (logical geometry: window width 440; row0 center (100, 117);
#      button row center y = H-37; main button center x = 365; the client
#      rect gives the actual scale, so this holds at any DPI)
function Send-PanelClick([IntPtr]$Hwnd, [double]$X, [double]$Y, [double]$Scale) {
    $px = [int]([math]::Round($X * $Scale))
    $py = [int]([math]::Round($Y * $Scale))
    $lp = [IntPtr](((($py -band 0xFFFF) -shl 16) -bor ($px -band 0xFFFF)))
    # [UIntPtr]::One does not resolve in every pwsh build the runners use
    # (evaluates to $null -> "cannot convert null to UIntPtr"); an explicit
    # cast is portable everywhere.
    [UsbmonDemo.Win32]::PostMessageW($Hwnd, 0x0201, [UIntPtr][uint64]1, $lp) | Out-Null
    Start-Sleep -Milliseconds 60
    [UsbmonDemo.Win32]::PostMessageW($Hwnd, 0x0202, [UIntPtr]::Zero, $lp) | Out-Null
}
if ($panelHwnd -ne [IntPtr]::Zero) {
    $rcp = New-Object UsbmonDemo.Win32+RECT
    [UsbmonDemo.Win32]::GetClientRect($panelHwnd, [ref]$rcp) | Out-Null
    $scaleP = $rcp.Right / 440.0
    $logicalH = $rcp.Bottom / $scaleP
    Send-PanelClick $panelHwnd 100 117 $scaleP                  # row 0
    Send-PanelClick $panelHwnd 365 ($logicalH - 37) $scaleP     # main 打开 button
    Start-Sleep -Milliseconds 400
    $panelTxt2 = ""
    if (Test-Path -LiteralPath $PanelFile) {
        $panelTxt2 = Get-Content -LiteralPath $PanelFile -Raw -Encoding UTF8
    }
    if ($panelTxt2 -match 'action open_row row=0 E:' -and
        $panelTxt2 -match 'action open row=0 E:') {
        Ok "panel row click + 打开 button fire open actions (callback -> tray volume action)"
    } else {
        Bad "panel open actions missing from log (got: $($panelTxt2 -replace "`n", ' | '))"
    }

    # 19) 展开 grows the real window (SetWindowPos geometry, not repaint only)
    $h0 = $rcp.Bottom
    Send-PanelClick $panelHwnd 61 ($logicalH - 37) $scaleP      # expand button
    Start-Sleep -Milliseconds 400
    $rcp2 = New-Object UsbmonDemo.Win32+RECT
    [UsbmonDemo.Win32]::GetClientRect($panelHwnd, [ref]$rcp2) | Out-Null
    if ($rcp2.Bottom - $h0 -gt 20) {
        Ok "展开 grows the real window height (${h0}px -> $($rcp2.Bottom)px)"
    } else {
        Bad "展开 did not resize the window (${h0}px -> $($rcp2.Bottom)px)"
    }

    # 20) Esc hides the panel + logs the close action
    [UsbmonDemo.Win32]::PostMessageW($panelHwnd, 0x0100, [UIntPtr]0x1B, [IntPtr]::Zero) | Out-Null
    Start-Sleep -Milliseconds 400
    $vis = [UsbmonDemo.Win32]::IsWindowVisible($panelHwnd)
    $panelTxt3 = ""
    if (Test-Path -LiteralPath $PanelFile) {
        $panelTxt3 = Get-Content -LiteralPath $PanelFile -Raw -Encoding UTF8
    }
    if (-not $vis -and $panelTxt3 -match 'action close') {
        Ok "Esc hides the panel and logs the close action"
    } else {
        Bad "Esc: visible=$vis, close action logged=$($panelTxt3 -match 'action close')"
    }
} else {
    Bad "panel interaction tests skipped (no panel window)"
    Bad "panel expand/geometry test skipped"
    Bad "panel Esc test skipped"
}

if ($procP -and -not $procP.HasExited) { Stop-Process -Id $procP.Id -Force }
Start-Sleep -Milliseconds 700      # release the single-instance lock cleanly
Remove-Item Env:USBMON_PANEL_TEST -ErrorAction SilentlyContinue

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
    # LPARAM = WM_RBUTTONUP (0x0205; 0x0204 is RBUTTONDOWN — the filter
    # swallows it like a real hover)
    [UsbmonDemo.Win32]::PostMessageW($hwnd, 0x8003, [UIntPtr]::Zero, [IntPtr]0x0205) | Out-Null
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

# --- 14b) async safe-eject: worker thread + marshaled result toast -----------
# UMWM_TRAY_EJECT_TEST (0x8007), honored only under USBMON_TRAY_TEST: drives
# tray_do_eject with letter 'Q' (0x51) — almost certainly absent on a runner,
# so the worker's CreateFileW fails fast, but the WHOLE async chain runs:
# GUI thread returns immediately, worker runs the IOCTL attempt, the result
# toast is marshaled back via PostThreadMessageW and the tray log records it.
if ($hwnd -ne [IntPtr]::Zero) {
    [UsbmonDemo.Win32]::PostMessageW($hwnd, 0x8007, [UIntPtr]0x51, [IntPtr]::Zero) | Out-Null
    Start-Sleep -Milliseconds 1500     # progress toast + worker + result toast
    $ejectTxt = Read-TrayLog
    if ($ejectTxt -match 'eject (ok|fail)') {
        Ok "async safe-eject ran on a worker thread (tray log: $($Matches[0]))"
    } else {
        Bad "async eject chain left no result in the tray log"
    }
    $toastWnds = [UsbmonDemo.Win32]::WindowsByClassAndPid("usbmonToast", [uint32]$proc2.Id)
    if ($toastWnds.Count -gt 0) {
        Ok "eject progress/result toast window exists (class usbmonToast, pid $($proc2.Id))"
    } else {
        # surface the C-side toast diagnostics (um_tray_test_log lines) so
        # CI failures are diagnosable from the run log alone
        $toastDiag = (Read-TrayLog) -split "`n" | Where-Object { $_ -match '^toast ' } | Select-Object -Last 4
        Bad "no usbmonToast window (pid $($proc2.Id)) after the eject test; C diagnostics: $($toastDiag -join ' | ')"
    }
    if (-not $proc2.HasExited) {
        Ok "daemon still alive after async eject (GUI thread never blocked)"
    } else {
        Bad "daemon died during the async eject test"
    }
} else {
    Bad "async eject test skipped (no listener hwnd)"
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
Remove-Item Env:USBMON_PANEL_TEST -ErrorAction SilentlyContinue
Remove-Item -LiteralPath $Root -Recurse -Force -ErrorAction SilentlyContinue

Write-Output ""
Write-Output "result: $script:Pass passed, $script:Fail failed"
if ($script:Fail -ne 0) { exit 1 }
exit 0
