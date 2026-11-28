# USBMonitor 2.6.0

[English](README.md) · [简体中文](README.zh-CN.md)

Native C99 external-storage monitor. Windows gets a tray icon and a device overview panel; Linux gets a CLI and optional X11 notifications. The daemon coalesces device events that arrive close together and rechecks the inventory at a fixed interval.

Diagnostic notes cover baseline results, design trade-offs, tested scope and on-machine acceptance checks: [Diagnostics & verification](docs/DIAGNOSTIC.md) · [UI preview](docs/USBMonitor-preview.png).

## Getting started

| Platform | Entry point | What you get |
| --- | --- | --- |
| Windows x64 | Double-click `bin/windows-amd64/usbmon.exe` | Left-click the tray icon for volumes and the device overview; right-click to rescan, toggle "Show other devices", open the log/config folders, manage login autostart or quit. |
| Linux x86-64 | `./bin/linux-amd64/usbmon` | Starts the static daemon. When X11 and Xft are available, `usbmon-toast` in the same directory shows notifications. |

In a Windows terminal, replace `./usbmon` with `./usbmon.exe` below. The Windows executable uses the GUI subsystem, so automation should wait explicitly for process exit, for example PowerShell's `Start-Process -Wait -PassThru`.

```sh
./usbmon --list
./usbmon --once --no-hooks
./usbmon --no-gui --interval 3600
./usbmon --help
```

- `--list` reads the current storage inventory only. It exits 1 if enumeration fails.
- `--once` runs one round and keeps the snapshot. It shares the state lock with the daemon and exits 3 when the lock is held.
- `--no-gui` disables the UI and notifications; device events still trigger a scan. `--no-hotpath` disables automatic event wake-ups.
- `--baseline` ignores stored state, records the devices already present and runs matching hooks.
- `--install-startup`, `--startup-status` and `--uninstall-startup` manage login autostart for the current user (HKCU Run on Windows, XDG autostart on Linux). The Windows installer also uses `--install-startup` so the registry value matches what the app reads.

## Device overview

Windows drive letters, volumes and disks are counted separately. Several letters on one physical disk are merged into one row, and the capacity bar refers to the first accessible mount point. Drives without a letter, directory mounts, inaccessible volumes and readers positively identified as empty each show their own status. USB HID devices, network adapters and hubs never appear in notifications; they are shown in the overview only and hidden by default. Use "Show other devices" in the tray right-click menu to display them — the choice is remembered. The panel footer only keeps Expand/Collapse and Open USB drive. An expanded list can be scrolled; the arrow keys move to the selected device, Tab cycles controls, Enter activates an action and Esc hides the panel.

Right-click a device row to open the volume, reveal it in Explorer or copy its path. Eligible USB storage can also request a Windows safe-removal of the physical device. For composite devices or devices without proof of removable capability, use the Windows system tray ejection UI instead. Volume accessibility depends on the current user and driver state.

Linux notifications work on X11 or desktops providing XWayland. The bundled X11 helper needs glibc 2.34 or newer, libX11 and libXft. Capacity, partition count and mount points are read at scan time. If the mount manager finishes mounting after the scan, the first event may not have a path yet.

## Logs and hooks

| Item | Windows | Linux |
| --- | --- | --- |
| Logs, snapshots and lock | `%LOCALAPPDATA%\usbmon` | `${XDG_STATE_HOME:-$HOME/.local/state}/usbmon` |
| Hooks | `%APPDATA%\usbmon\hooks.json` | `${XDG_CONFIG_HOME:-$HOME/.config}/usbmon/hooks.json` |

`--log FILE` and `--hooks FILE` override these paths. JSONL events are `start`, `round`, `add`, `remove`, `error` and `stop`. Each `round` has a `wake` marker of `start`, `hot` or `tick`, and `scan_ms` records enumeration time. Logs rotate after 1 MiB, keeping three older files. Serial numbers are stored as truncated SHA-256 fingerprints by default; `--log-raw` keeps the original value.

Hooks are unconfigured by default. On first run, `hooks.example.json` is written into the config directory; edit its command, rename it to `hooks.json` and set `"enabled"` to `true`. Windows example:

```json
{
  "hooks": [
    {
      "name": "backup",
      "match_keys": ["disk*"],
      "match_models": ["SanDisk*"],
      "command": ["C:\\Tools\\backup.exe", "--source", "{path}"],
      "debounce_seconds": 3,
      "enabled": true
    }
  ]
}
```

- `command` is an argv array. The executable must be a literal path; Windows additionally requires an absolute `.exe` path. Arguments may use `{key}`, `{model}` and `{path}`.
- `{path}` prefers the currently accessible mount point. When nothing is mounted it falls back to a device hint: usually `/dev/sdX` on Linux, or a drive letter / `diskN` on Windows.
- Hooks run with the current user's privileges and have a 60-second child timeout. `--no-hooks` disables them. Linux can reload with SIGHUP; Windows reads them again after a restart.

## Building from source

```sh
make strict selftest integration-test
make analyze
make static
make CROSS=x86_64-w64-mingw32- windows
```

The Linux GUI build needs `pkg-config`, `libx11-dev` and `libxft-dev`; the static daemon needs `musl-tools`; the Windows cross-build needs MinGW-w64 GCC and windres. The Linux GUI regression `bash tools/demo-gui.sh` needs Xvfb and xwininfo. The native Windows smoke test is `tools/demo.ps1`.

`src/main.c` drives scheduling and `src/state.c` persists snapshots. On Windows `um_enum.c` collects device evidence and `scan_win32.c` projects it into storage events and UI rows. On Linux `scan_linux.c` reads sysfs and `hotpath.c` receives device changes. The native window, tray actions and UI layout live in `gui_win32.c`, `tray_win32.c` and `um_toast_ui.c`.

Release archives include `bin/SHA256SUMS.txt` for the shipped binaries. Verify with `sha256sum -c SHA256SUMS.txt` from the `bin` directory.
