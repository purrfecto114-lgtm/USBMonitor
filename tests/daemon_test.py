#!/usr/bin/env python3
"""Use isolated sysfs and state trees to exercise the CLI and daemon."""
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time

binary = Path(sys.argv[1] if len(sys.argv) > 1 else "./usbmon").resolve()
checks = 0


def check(condition, message):
    global checks
    if not condition:
        raise AssertionError(message)
    checks += 1
    print("ok", message)


with tempfile.TemporaryDirectory(prefix="usbmon-test-", dir=os.getenv("TMPDIR", ".")) as tmp:
    root = Path(tmp).resolve()
    sysfs = root / "sys"
    block = sysfs / "block"
    block.mkdir(parents=True)
    home = root / "state"
    snapshot = home / "usbmon/last-snapshot.txt"
    log = root / "events.jsonl"
    env = {**os.environ, "XDG_STATE_HOME": str(home), "XDG_CONFIG_HOME": str(root / "config")}

    def cli(*args, exe=binary):
        return subprocess.run([str(exe), *map(str, args)], env=env, text=True,
                              capture_output=True, timeout=12)

    def run_once(*args, sys_root=sysfs):
        return cli("--once", "--sys-root", sys_root, "--log", log, *args)

    def events():
        return [json.loads(line) for line in log.read_text(encoding="utf-8").splitlines()]

    def device(key, serial, model=b"Flash Drive", usb=True):
        port = sysfs / "devices/pci/fake-usb-name" / key
        scsi = port / "host/target"
        disk = scsi / "block" / key
        disk.mkdir(parents=True, exist_ok=True)
        (disk / "size").write_text("2048")
        (disk / "removable").write_text("0")
        (disk / f"{key}1").mkdir(exist_ok=True)
        (disk / f"{key}17not-partition").mkdir(exist_ok=True)
        (scsi / "model").write_bytes(model)
        (disk / "device").symlink_to("../..", target_is_directory=True)
        if usb:
            (port / "subsystem").symlink_to("../../../bus/usb")
            (port / "serial").write_text(serial)
        (block / key).symlink_to("../" + disk.relative_to(sysfs).as_posix(),
                                 target_is_directory=True)

    device("sdb", "USB-SECRET-123", b"USB-\xff-Drive")
    device("sdc", "", usb=False)
    result = cli("--list", "--sys-root", sysfs, "--log", log)
    check(result.returncode == 0 and "sdb" in result.stdout and "sdc" not in result.stdout,
          "canonical USB ancestry works even for fixed media; a misleading directory name is ignored")
    check(not log.exists() and not home.exists() and not (root / "config").exists(),
          "--list has no state, config or log side effects")
    check(cli("--list", "--sys-root", root / "missing").returncode == 1,
          "--list reports enumeration failure")

    snapshot.parent.mkdir(parents=True)
    snapshot.write_text("sdb\n", encoding="utf-8")
    check(run_once().returncode == 0 and not any(e["ev"] == "add" for e in events()),
          "legacy key-only state loads without duplicate additions")
    old = snapshot.read_bytes()
    check(b"\tsha256:" in old and b"USB-SECRET" not in old, "snapshot migrates to a redacted identity")
    check(run_once("--baseline", sys_root=root / "missing").returncode == 1,
          "failed --once returns nonzero")
    check(snapshot.read_bytes() == old, "failed scan preserves the last valid snapshot")

    port = sysfs / "devices/pci/fake-usb-name/sdb"
    (port / "serial").write_text("USB-SECRET-456")
    before = len(events())
    check(run_once().returncode == 0, "rescan after a device reuses the same key")
    diff = [e for e in events()[before:] if e["ev"] in {"add", "remove"}]
    check([e["ev"] for e in diff] == ["remove", "add"], "same-key replacement is a removal and an addition")
    check(diff[-1]["serial"] == "sha256:" + hashlib.sha256(b"USB-SECRET-456").hexdigest()[:12],
          "event serial fingerprint matches SHA-256")
    check(diff[-1]["partitions"] == ["sdb1"] and "\ufffd" in diff[-1]["model"],
          "invalid device bytes are replaced and JSONL remains valid UTF-8")
    check("USB-SECRET" not in log.read_text(encoding="utf-8"), "raw serial is absent by default")

    hooks = root / "hooks.json"
    marker = root / "hook-result.json"
    command = [sys.executable, "-c",
               "import json,sys;open(sys.argv[1],'w').write(json.dumps(sys.argv[2:]))",
               str(marker), "  中文𐐷", "", "{key}"]
    hooks.write_text(json.dumps({"hooks": [{"name": "中文", "command": command}]}, ensure_ascii=False),
                     encoding="utf-8-sig")
    check(run_once("--baseline", "--hooks", hooks).returncode == 0, "UTF-8 hooks configuration runs")
    check(json.loads(marker.read_text()) == ["  中文𐐷", "", "sdb"],
          "hook preserves Unicode, leading spaces and empty arguments")
    marker.unlink()
    hooks.write_text('{"hooks":[{"name":"bad","command":[' + json.dumps(sys.executable) +
                     ',"\\u0000unexpected"]}]}', encoding="utf-8")
    result = run_once("--baseline", "--hooks", hooks)
    check(result.returncode == 0 and "invalid JSON" in result.stderr and not marker.exists(),
          "embedded NUL in a hook disables the malformed configuration")

    # Long interval guarantees that these rounds are from the event path.
    daemon = subprocess.Popen([str(binary), "--sys-root", str(sysfs), "--log", str(log),
                               "--interval", "3600", "--no-gui", "--no-hooks"], env=env)
    try:
        deadline = time.monotonic() + 6
        while time.monotonic() < deadline:
            if events()[-1]["ev"] == "round":
                break
            time.sleep(.03)
        check(run_once().returncode == 3, "--once shares the daemon lock")
        competing_log = root / "other.jsonl"
        check(cli("--once", "--sys-root", sysfs, "--log", competing_log).returncode == 3 and
              not competing_log.exists(), "refused process never opens its log")
        block.rename(sysfs / "block-old")
        time.sleep(1)
        block.mkdir()
        device("sdd", "MOCK-HOT")
        deadline = time.monotonic() + 6
        while time.monotonic() < deadline:
            if any(e.get("key") == "sdd" for e in events()):
                break
            time.sleep(.05)
        check(any(e.get("key") == "sdd" for e in events()), "mock watcher recovers after directory replacement")
        check(any(e.get("wake") == "hot" for e in events()), "hot event is not an interval scan")
    finally:
        daemon.terminate()
        daemon.wait(timeout=8)
    check(daemon.returncode == 0 and events()[-1]["ev"] == "stop", "daemon shuts down cleanly")

    exe = root / 'location with spaces $q' / 'usbmon'
    exe.parent.mkdir()
    shutil.copy2(binary, exe)
    check(cli("--install-startup", exe=exe).returncode == 0, "autostart accepts quoted executable paths")
    desktop = root / "config/autostart/usbmon.desktop"
    check('Exec="' in desktop.read_text() and r"\\$q" in desktop.read_text(),
          "desktop entry quotes and escapes the executable")
    if shutil.which("desktop-file-validate"):
        check(subprocess.run(["desktop-file-validate", str(desktop)], check=False).returncode == 0,
              "freedesktop validator accepts the generated entry")
    check(cli("--uninstall-startup", exe=exe).returncode == 0 and not desktop.exists(),
          "autostart uninstalls cleanly")

print(f"{checks} checks, 0 failures")
