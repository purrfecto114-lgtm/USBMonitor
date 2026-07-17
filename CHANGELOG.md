# Changelog

All notable changes to this project are documented in this file.
Format loosely follows [Keep a Changelog](https://keepachangelog.com/).

## [1.0.0] — 2026-07-17

### Security
- **Hooks trust boundary documented.** README now ships a dedicated
  `🔒 Security` section describing that `config.json` is an implicit trust
  point, hooks run with the full current-user token, and the feature is
  opt-in with an empty default `hooks` array.
- **`shell=False` enforced at AST level.** Every `subprocess.*` call in
  `usb_monitor/hooks.py` now explicitly passes `shell=False` as a keyword
  literal, with `stdin/stdout/stderr=DEVNULL` and a 60s timeout. A new
  regression test (`tests/test_hook_security.py`) parses the module with
  `ast` and fails if any call site forgets the guard.
- **`hash_id` renamed to `stable_fingerprint`.** The old name implied
  cryptographic hashing; the new name matches what the function actually
  is (a plain truncated SHA-256 with no salt). The docstring now states
  explicitly that it is **not** encryption, **not** anonymization, and
  **not** safe for secrets.

### Added
- `USBMonitor.pyw` — GUI entry point for double-click and Nuitka builds.
- `USBMonitor_console.py` — diagnostic entry point that keeps the console.
- `run_usb_monitor.bat` — user-facing launcher that creates a `.venv`,
  installs runtime deps, and starts the app without a console window.
- `tests/test_hook_security.py` — 5 AST-based regression tests guarding
  the hook command-execution contract.
- `docs/BUILD.md` — Nuitka packaging guide covering onefile, standalone,
  debug-console, and common Qt/AV-false-positive pitfalls.
- `pyproject.toml` — editable-install metadata and `usb-monitor` console
  script.
- `MANIFEST.md` — per-file change manifest for this release.

### Changed
- Version bumped from 1.2.7 to 1.0.0 (project reset; semantic versioning
  from here on).
- `build/windows_nuitka.bat` replaces the legacy
  `build/windows_nuitka_upx.bat`.  The new script:
  - targets `USBMonitor.pyw` instead of `usb_monitor/__main__.py`;
  - accepts `onefile` / `standalone` as an explicit argument;
  - auto-detects the bundled `upx/upx.exe` and passes it to Nuitka via
    `--upx-binary` (override with `USBMONITOR_NO_UPX=1`);
  - prefers `py -3.11` and falls back to `python` on PATH;
  - honours `USBMONITOR_CONSOLE=1` to force a console for debugging;
  - writes `build/nuitka/nuitka-report.xml` for dependency audits.
- `CONFIG_VERSION` bumped from 2 to 3 (new `hooks` field).
- `usb_monitor/__init__.py` re-exports `stable_fingerprint` and the new
  hooks API surface.

### Removed
- `build/windows_nuitka_upx.bat` — superseded by `windows_nuitka.bat`.
- `docs/MARKDOWN_REPORT_20260630_IMPLEMENTATION.md` — internal diagnostic,
  no longer referenced.
- `optimized-prompt.md` — scratch file, not part of the shipped project.

### Tests
- `tests/test_core.py` — `hash_id` tests renamed to `stable_fingerprint`;
  added `test_stable_fingerprint_is_documented_as_not_cryptographic` to
  guard the security docstring against regression.
- `tests/test_entrypoint_and_nuitka.py` — version assertions updated to
  `1.0.0`; new test verifies `USBMonitor.pyw` is syntax-valid and calls
  `usb_monitor.app.main`.
- `151 passed, 1 failed` on Linux (the 1 failure is a Qt real-GUI smoke
  test that only passes on Windows).

---

Older history predates the 1.0.0 reset and is not tracked here.
