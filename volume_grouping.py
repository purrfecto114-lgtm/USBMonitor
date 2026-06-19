"""Group logical volumes by their underlying physical disk.

A single USB stick or external hard disk can expose multiple drive letters
(e.g. an EFI partition + a data partition, or a Windows+Linux split). The
tray menu is much friendlier when those are presented as one entry with
sub-items than as N independent rows.

This module reads the physical-disk mapping cached by
:mod:`device_classifier` and produces :class:`PhysicalDeviceGroup` objects
that the GUI can render directly.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from typing import Any, Optional

from .device_classifier import (
    DeviceClassification,
    classify_volume,
    invalidate_cache,
    usb_drive_snapshot,
)
from .windows_helpers import safe_disk_usage, format_bytes
from .events import VolumeInfo


@dataclass
class PhysicalDeviceGroup:
    """All USB volumes that live on the same physical disk."""

    physical_disk: Optional[int]
    device_model: str
    bus_type: str
    volumes: list[VolumeInfo] = field(default_factory=list)

    @property
    def drive_paths(self) -> list[str]:
        return [vol.path for vol in self.volumes]

    @property
    def primary_path(self) -> str:
        """The volume the user most likely wants when clicking "Open"."""
        if not self.volumes:
            return ""
        # Prefer the volume with the largest total size — that's the data
        # partition, not the small EFI/recovery partition.
        sorted_vols = sorted(self.volumes, key=lambda v: (v.total or 0), reverse=True)
        return sorted_vols[0].path

    @property
    def total_size(self) -> int:
        # Sum is misleading (a partition's size is already the whole disk
        # share it occupies), but the largest single volume's size is a
        # good approximation of the device capacity.
        if not self.volumes:
            return 0
        return max(vol.total or 0 for vol in self.volumes)

    @property
    def free_size(self) -> int:
        return sum(vol.free or 0 for vol in self.volumes)

    @property
    def label(self) -> str:
        """Human-friendly label for menus and toasts."""
        parts: list[str] = []
        if self.device_model:
            parts.append(self.device_model)
        elif self.volumes:
            # Fall back to the largest volume's title.
            biggest = max(self.volumes, key=lambda v: v.total or 0)
            label = biggest.title.split("·")[0].strip()
            if label:
                parts.append(label)
        letters = "、".join(v.path.rstrip("\\") for v in self.volumes)
        if letters:
            parts.append(f"({letters})")
        return " ".join(parts) if parts else "USB 设备"

    @property
    def detail(self) -> str:
        """Second-line description: bus type + capacity + partition count."""
        bits: list[str] = []
        if self.bus_type and self.bus_type not in {"unknown", "usb"}:
            bits.append(self.bus_type.upper())
        if self.total_size:
            bits.append(f"{format_bytes(self.total_size)}")
        if len(self.volumes) > 1:
            bits.append(f"{len(self.volumes)} 个分区")
        return " · ".join(bits)


def _volume_info_from_classification(
    path: str,
    raw_info: dict[str, Any],
    classification: DeviceClassification,
    source: str = "drive_scan",
) -> VolumeInfo:
    from datetime import datetime, timezone

    total, used, free = safe_disk_usage(path)
    label = raw_info.get("volume_label") or ""
    drive_type = raw_info.get("drive_type") or "unknown"
    title = f"{label} · {path}" if label else f"移动磁盘 {path}"
    return VolumeInfo(
        path=path,
        title=title,
        source=source,
        timestamp_utc=datetime.now(timezone.utc).isoformat(timespec="seconds"),
        drive_type=drive_type,
        total=total,
        used=used,
        free=free,
        physical_disk=classification.physical_disk,
        is_usb=classification.is_usb,
        bus_type=classification.bus_type_name,
        device_model=classification.display_model,
    )


def group_volumes_by_physical_device(
    snapshot: Optional[dict[str, dict[str, Any]]] = None,
) -> list[PhysicalDeviceGroup]:
    """Return USB volumes grouped by physical disk.

    Volumes whose physical-disk lookup failed are placed in singleton
    groups keyed by their drive letter — they still appear in the menu,
    just without merging.
    """
    snap = snapshot if snapshot is not None else usb_drive_snapshot()
    # Always invalidate the cache first so freshly-inserted devices get
    # re-classified even if a stale entry lingers.
    invalidate_cache()

    grouped: dict[Any, PhysicalDeviceGroup] = {}
    for path, info in snap.items():
        classification = classify_volume(path)
        if not classification.is_usb:
            continue
        volume = _volume_info_from_classification(path, info, classification)
        key: Any
        if classification.physical_disk is not None:
            key = ("physical", classification.physical_disk)
        else:
            # No physical-disk info: keep each volume as its own group so
            # it still shows up, but never merge two volumes that simply
            # lack metadata.
            key = ("letter", path)
        group = grouped.get(key)
        if group is None:
            group = PhysicalDeviceGroup(
                physical_disk=classification.physical_disk,
                device_model=classification.display_model,
                bus_type=classification.bus_type_name,
            )
            grouped[key] = group
        group.volumes.append(volume)
        # If the first volume had no model but a later one does, adopt it.
        if not group.device_model and classification.display_model:
            group.device_model = classification.display_model
        if not group.bus_type or group.bus_type == "unknown":
            group.bus_type = classification.bus_type_name

    # Sort volumes within each group by drive letter, then sort groups by
    # their smallest drive letter so the menu order is stable.
    for group in grouped.values():
        group.volumes.sort(key=lambda v: v.path)
    return sorted(grouped.values(), key=lambda g: g.volumes[0].path if g.volumes else "")


def flatten_groups(groups: list[PhysicalDeviceGroup]) -> list[VolumeInfo]:
    """Convenience: flatten groups back into a list of VolumeInfo."""
    out: list[VolumeInfo] = []
    for group in groups:
        out.extend(group.volumes)
    return out
