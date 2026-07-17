"""Tests for DriveScanner's two-level classification cache.

These tests bypass the Win32 API by injecting a fake ``WindowsStorageApi``-like
object. We don't import the real class (it requires Windows + pywin32); instead
we rely on duck typing — ``DriveScanner`` only calls ``logical_drives``,
``drive_type``, ``volume_disk_numbers``, ``physical_disk_is_external``, and
``volume_label``.
"""

from __future__ import annotations

import sys
import time
from pathlib import Path

import pytest

# Add the project root to sys.path so ``import usb_monitor`` resolves.
sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

# Stub ctypes.wintypes to satisfy DriveScanner's module-level imports on Linux.
# Without this, the bare ``from ctypes import wintypes`` succeeds but the
# DriveScanner class definition references ``DRIVE_REMOVABLE`` etc. which
# ARE defined at module load (those are plain ints). So this should already work,
# but if not we patch as needed below.

import usb_monitor as um


# ---------------------------------------------------------------------------
# Fake API
# ---------------------------------------------------------------------------


class FakeApi:
    """Minimal duck-typed stand-in for ``WindowsStorageApi``."""

    DRIVE_REMOVABLE = um.DRIVE_REMOVABLE

    def __init__(self, drives: dict[str, dict]) -> None:
        # drives: {path: {"disk": int, "external": bool, "label": str}}
        self._drives = drives
        self.volume_disk_calls: list[str] = []
        self.physical_disk_calls: list[int] = []

    def logical_drives(self) -> tuple[str, ...]:
        return tuple(sorted(self._drives.keys()))

    def drive_type(self, path: str) -> int:
        return self.DRIVE_REMOVABLE  # always pretend removable in these tests

    def volume_label(self, path: str) -> str:
        return self._drives.get(path, {}).get("label", "")

    def volume_disk_numbers(self, path: str) -> tuple[int, ...]:
        self.volume_disk_calls.append(path)
        disk = self._drives.get(path, {}).get("disk")
        return (disk,) if disk is not None else ()

    def physical_disk_is_external(self, disk_number: int) -> bool:
        self.physical_disk_calls.append(disk_number)
        for info in self._drives.values():
            if info.get("disk") == disk_number:
                return info["external"]
        return False


# ---------------------------------------------------------------------------
# Fixtures
# ---------------------------------------------------------------------------


@pytest.fixture
def dual_partition_drive() -> FakeApi:
    """A single physical disk (1) with two partitions; another disk (2)."""
    return FakeApi(
        {
            "E:\\": {"disk": 1, "external": True, "label": "USB_A"},
            "F:\\": {"disk": 1, "external": True, "label": "USB_A"},  # same disk
            "G:\\": {"disk": 2, "external": False, "label": "Internal"},
        }
    )


# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------


def test_classify_l1_cache_hits(dual_partition_drive):
    """Second call for same path doesn't re-invoke the API."""
    scanner = um.DriveScanner(dual_partition_drive)  # type: ignore[arg-type]
    scanner._classify("E:\\", um.DRIVE_REMOVABLE)
    initial = len(dual_partition_drive.volume_disk_calls)
    scanner._classify("E:\\", um.DRIVE_REMOVABLE)
    assert len(dual_partition_drive.volume_disk_calls) == initial


def test_classify_l2_cache_hits_for_shared_disk(dual_partition_drive):
    """Two volumes on the same disk → physical_disk_is_external called once."""
    scanner = um.DriveScanner(dual_partition_drive)  # type: ignore[arg-type]
    scanner._classify("E:\\", um.DRIVE_REMOVABLE)
    scanner._classify("F:\\", um.DRIVE_REMOVABLE)
    # Two distinct paths → two volume_disk_numbers calls (L1 path-level)
    assert len(dual_partition_drive.volume_disk_calls) == 2
    # But both share disk=1 → only one physical_disk_is_external call (L2 hit)
    assert len(dual_partition_drive.physical_disk_calls) == 1


def test_bus_cache_stats_track_hits_misses(dual_partition_drive):
    scanner = um.DriveScanner(dual_partition_drive)  # type: ignore[arg-type]
    scanner._classify("E:\\", um.DRIVE_REMOVABLE)   # miss → external=True
    scanner._classify("F:\\", um.DRIVE_REMOVABLE)   # hit on disk=1
    scanner._classify("G:\\", um.DRIVE_REMOVABLE)   # miss → external=False
    stats = scanner.cache_stats
    assert stats["l2_misses"] == 2
    assert stats["l2_hits"] == 1
    assert stats["l2_size"] == 2


def test_internal_disk_filtered_out(dual_partition_drive):
    scanner = um.DriveScanner(dual_partition_drive)  # type: ignore[arg-type]
    disk_numbers, external = scanner._classify("G:\\", um.DRIVE_REMOVABLE)
    assert disk_numbers == (2,)
    assert external is False


def test_invalidate_clears_both_caches(dual_partition_drive):
    scanner = um.DriveScanner(dual_partition_drive)  # type: ignore[arg-type]
    scanner._classify("E:\\", um.DRIVE_REMOVABLE)
    scanner._classify("F:\\", um.DRIVE_REMOVABLE)
    assert scanner.cache_stats["l2_size"] > 0

    # Path-specific invalidate: L1 path entry drops, L2 disk entry kept.
    scanner.invalidate(["E:\\"])
    # (We can't easily assert L1 size without poking internals, but L2 stays.)
    assert scanner.cache_stats["l2_size"] > 0

    # Topology-wide invalidate: both wiped.
    scanner.invalidate([])
    assert scanner.cache_stats["l2_size"] == 0
    assert scanner.cache_stats["l1_size"] == 0


def test_bus_cache_ttl_expiry(monkeypatch):
    """After BUS_CACHE_TTL_SECONDS, physical_disk_is_external must be called again."""
    api = FakeApi({"E:\\": {"disk": 7, "external": True, "label": "X"}})
    scanner = um.DriveScanner(api)  # type: ignore[arg-type]
    scanner._classify("E:\\", um.DRIVE_REMOVABLE)
    first_count = len(api.physical_disk_calls)
    # Expire the L2 entry by jumping monotonic clock past the TTL.
    future = time.monotonic() + um.DriveScanner.BUS_CACHE_TTL_SECONDS + 1.0
    monkeypatch.setattr(time, "monotonic", lambda: future)
    scanner._classify("E:\\", um.DRIVE_REMOVABLE)
    assert len(api.physical_disk_calls) == first_count + 1


def test_l1_cache_size_bounded(monkeypatch):
    """L1 cache should evict oldest entries past CACHE_MAX_ITEMS."""
    api = FakeApi({f"{chr(65+i)}:\\": {"disk": i, "external": True, "label": f"D{i}"}
                   for i in range(um.DriveScanner.CACHE_MAX_ITEMS + 5)})
    scanner = um.DriveScanner(api)  # type: ignore[arg-type]
    for path in api.logical_drives():
        scanner._classify(path, um.DRIVE_REMOVABLE)
    # After visiting more than CACHE_MAX_ITEMS paths, L1 size stays at the cap.
    assert scanner.cache_stats["l1_size"] <= um.DriveScanner.CACHE_MAX_ITEMS