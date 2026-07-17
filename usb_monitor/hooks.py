"""Lightweight, opt-in event hooks for USB events.

This module implements a simple subscription mechanism that allows users to
define rules which trigger external commands when certain USB volumes are
inserted.  A rule can match on volume paths and/or labels using glob
patterns, and commands may reference the path or label via ``{path}`` and
``{label}`` placeholders.  Hooks are debounced on a per-rule, per-volume
basis to avoid firing repeatedly on bursty device events.

Example configuration entry::

    [
      {
        "name": "auto-backup",
        "match_labels": ["BACKUP*"],
        "command": ["powershell", "-File", "C:/scripts/backup.ps1", "{path}"]
      }
    ]

Rules can be provided via the application configuration (`AppConfig.hooks`) as
a list of dictionaries.  The GUI runtime will convert these dictionaries into
``HookRule`` instances and create a ``HookRunner`` that subscribes to the
internal event bridge.  When a matching `UsbEvent` is received the runner
spawns a thread to invoke the configured command.

"""
from __future__ import annotations

import fnmatch
import logging
import subprocess
import threading
import time
from concurrent.futures import ThreadPoolExecutor
from dataclasses import dataclass
from typing import Iterable, Sequence

from .core import UsbEvent, VolumeInfo

LOG = logging.getLogger("usb_monitor.hooks")


@dataclass(frozen=True)
class HookRule:
    """Definition of a user-defined hook rule.

    Attributes
    ----------
    name: str
        A unique name for the rule, used for debouncing.
    match_paths: tuple[str, ...]
        Glob patterns to match against the volume path (e.g. ``'E:\\'``).
    match_labels: tuple[str, ...]
        Glob patterns to match against the volume label (e.g. ``'BACKUP*'``).
    command: tuple[str, ...]
        The command to run when the rule fires.  Each token may contain
        ``{path}`` or ``{label}`` placeholders which will be substituted with
        the corresponding values from the `VolumeInfo`.
    debounce_seconds: float
        Minimum number of seconds between consecutive firings for the same
        ``(rule.name, volume.path)`` combination.
    enabled: bool
        Whether this rule is active.  Disabled rules are ignored.
    """

    name: str
    match_paths: tuple[str, ...] = ()
    match_labels: tuple[str, ...] = ()
    command: tuple[str, ...] = ()
    debounce_seconds: float = 2.0
    enabled: bool = True


class HookRunner:
    """Evaluate hook rules against incoming USB events and run matching commands."""

    def __init__(self, rules: Iterable[HookRule], max_workers: int = 4) -> None:
        # Filter only enabled rules and store as a list.
        self._rules: list[HookRule] = [r for r in rules if r.enabled]
        # Track the last fire time per (rule name, volume path).
        self._last_fired: dict[str, float] = {}
        self._lock = threading.Lock()
        self._stopped = False
        self._executor = ThreadPoolExecutor(max_workers=max(1, int(max_workers)), thread_name_prefix="usb-hook")

    def on_event(self, event: UsbEvent) -> None:
        """Callback invoked for each USB event.

        Currently hooks only fire on ``add`` (insertion) events.  For every
        volume in the snapshot the runner iterates over all rules and
        dispatches commands for those that match.
        """
        if self._stopped:
            return
        if event.action not in ("add",):
            return
        for volume in event.snapshot:
            for rule in self._rules:
                if self._matches(rule, volume):
                    self._fire(rule, volume)

    @staticmethod
    def _match_glob(value: str, pattern: str) -> bool:
        """Case-insensitive glob match for Windows-style paths and labels."""
        return fnmatch.fnmatchcase(value.casefold(), pattern.casefold())

    @staticmethod
    def _matches(rule: HookRule, volume: VolumeInfo) -> bool:
        """Return True if the given volume satisfies the rule's patterns."""
        if rule.match_paths:
            if not any(HookRunner._match_glob(volume.path, pat) for pat in rule.match_paths):
                return False
        if rule.match_labels:
            # Volume labels may be empty; use empty string for matching.
            label = volume.label or ""
            if not any(HookRunner._match_glob(label, pat) for pat in rule.match_labels):
                return False
        return True

    def _fire(self, rule: HookRule, volume: VolumeInfo) -> None:
        """Invoke the command associated with a matching rule, with debouncing."""
        now = time.monotonic()
        key = f"{rule.name}:{volume.path}"
        with self._lock:
            last = self._last_fired.get(key, 0.0)
            if now - last < rule.debounce_seconds:
                return
            self._last_fired[key] = now
        # Substitute only the two documented placeholders.  Avoid str.format()
        # attribute/index traversal and reject unknown brace expressions.
        cmd = tuple(
            token.replace("{path}", volume.path).replace("{label}", volume.label or "")
            for token in rule.command
        )
        if not cmd or any("{" in token or "}" in token for token in cmd):
            LOG.error("hook_template_failed", extra={"rule": rule.name})
            return

        def run() -> None:
            # SECURITY: never enable shell parsing for hook commands.
            #
            # Why shell=False is mandatory:
            #   * Hook argv contains user-supplied {path} / {label}, which can
            #     include spaces, quotes, and shell metacharacters.  Even though
            #     we reject unknown braces, the path/label themselves are
            #     attacker-controllable (e.g. a USB volume labeled
            #     "foo; rm -rf /").  With shell=True these would be passed
            #     through cmd.exe and executed.
            #   * Disabling the shell does NOT sandbox the spawned process —
            #     it only prevents command-interpreter parsing.  The hooked
            #     program still runs with the full token of the current user.
            #     See README "Security" for the trust boundary.
            #
            # We also explicitly redirect all three standard streams to
            # DEVNULL so a malicious or buggy hook cannot block on stdin,
            # flood the parent's pipe buffers, or leak output into the
            # GUI process.
            LOG.info("hook_fired", extra={"rule": rule.name, "executable": cmd[0]})
            try:
                subprocess.run(
                    cmd,
                    check=False,
                    timeout=60,
                    shell=False,  # SECURITY: see block comment above.
                    stdin=subprocess.DEVNULL,
                    stdout=subprocess.DEVNULL,
                    stderr=subprocess.DEVNULL,
                )
            except subprocess.TimeoutExpired:
                LOG.warning("hook_timeout", extra={"rule": rule.name})
            except Exception as exc:
                LOG.error("hook_failed", extra={"rule": rule.name, "err": str(exc)})

        with self._lock:
            if self._stopped:
                return
            try:
                self._executor.submit(run)
            except RuntimeError as exc:
                LOG.error("hook_submit_failed", extra={"rule": rule.name, "err": str(exc)})

    def stop(self) -> None:
        """Disable the runner and prevent further hook firing."""
        with self._lock:
            self._stopped = True
            self._executor.shutdown(wait=False, cancel_futures=True)