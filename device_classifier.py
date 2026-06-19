"""Device classification: detect whether a volume is really USB-attached.

The legacy approach used ``GetDriveTypeW`` and treated every
``DRIVE_REMOVABLE`` as a USB device. That misclassified built-in SD card
readers (which enumerate as ``DRIVE_REMOVABLE`` even when no card is
present) and certain fixed USB hard disks (which enumerate as
``DRIVE_FIXED`` even though they really are external).

This module asks the storage stack directly via
``IOCTL_STORAGE_QUERY_PROPERTY`` for the device's ``BusType``. A bus type
of ``BusTypeUsb`` is the authoritative answer.

For robustness we layer three signals, best signal first:

1. ``query_storage_descriptor`` — exact BusType from the storage stack.
2. ``query_physical_disk_descriptor`` — same call on the underlying
   ``\\\\.\\PhysicalDriveN`` (useful when the volume handle rejects the
   IOCTL, e.g. for some BitLocked volumes).
3. WMI fallback via ``wmic``/PowerShell (slow but works without admin).
4. ``GetDriveTypeW`` — only as a last resort, with the well-known
   false-positives filtered out.

Callers consume :class:`DeviceClassification` via
:func:`classify_volume`. The result is cached per (drive-letter, mtime)
so repeated classifications during a burst of device-change events stay
cheap.
"""

from __future__ import annotations

import subprocess
import threading
import time
from dataclasses import dataclass, field
from typing import Any, Optional

from .logging_setup import log_event, log_error
from .windows_helpers import (
    BusTypeUsb,
    BusType1394,
    BusTypeSd,
    BusTypeMmc,
    BUS_TYPE_NAMES,
    EXTERNAL_BUS_TYPES,
    DRIVE_FIXED,
    DRIVE_REMOVABLE,
    drive_type as get_drive_type_code,
    drive_snapshot as raw_drive_snapshot,
    get_volume_disk_numbers,
    query_storage_descriptor,
    system_drive_path,
)


@dataclass
class DeviceClassification:
    """Result of classifying a single volume."""

    drive_letter: str  # "E:"
    is_usb: bool
    bus_type: int
    bus_type_name: str
    method: str  # "storage_descriptor" | "physical_disk" | "wmi" | "drive_type" | "fallback"
    physical_disk: Optional[int] = None
    vendor: str = ""
    product: str = ""
    removable_media: bool = False
    extra: dict[str, Any] = field(default_factory=dict)

    @property
    def display_model(self) -> str:
        parts = [self.vendor, self.product]
        return " ".join(p for p in parts if p).strip()


# ---------------------------------------------------------------------------
# Cache
# ---------------------------------------------------------------------------

_LOCK = threading.Lock()
_CACHE: dict[str, tuple[float, DeviceClassification]] = {}
_CACHE_TTL_S = 5.0


def _cache_get(key: str) -> Optional[DeviceClassification]:
    with _LOCK:
        item = _CACHE.get(key)
        if item is None:
            return None
        ts, value = item
        if time.monotonic() - ts > _CACHE_TTL_S:
            _CACHE.pop(key, None)
            return None
        return value


def _cache_put(key: str, value: DeviceClassification) -> None:
    with _LOCK:
        _CACHE[key] = (time.monotonic(), value)


def invalidate_cache(drive_letter: Optional[str] = None) -> None:
    """Drop cached classifications, either for one drive or all of them."""
    with _LOCK:
        if drive_letter is None:
            _CACHE.clear()
        else:
            key = drive_letter.upper().rstrip("\\").rstrip(":")
            _CACHE.pop(f"{key}:", None)
            _CACHE.pop(key, None)


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def _is_system_drive(path: str) -> bool:
    return path.upper() == system_drive_path().upper()


def _normalize_drive_letter(path: str) -> str:
    raw = str(path or "").strip()
    if len(raw) >= 2 and raw[1] == ":":
        return raw[:2].upper()
    return ""


# ---------------------------------------------------------------------------
# WMI fallback (no admin needed; slow)
# ---------------------------------------------------------------------------

def _wmi_query_disk_for_volume(drive_letter: str) -> Optional[dict[str, Any]]:
    """Use wmic to look up the physical disk and PnP device id for a drive.

    Returns None if wmic is unavailable or the lookup fails. We intentionally
    accept some latency here — the storage-descriptor path handles 99% of
    real cases; this is just a safety net.
    """
    letter = _normalize_drive_letter(drive_letter)
    if not letter:
        return None
    try:
        # The wmic CLI is deprecated but still present on every supported
        # Windows build at the time of writing. If it disappears we can
        # switch to ``Get-CimInstance`` via PowerShell.
        result = subprocess.run(
            [
                "wmic",
                "path",
                "Win32_LogicalDiskToPartition",
                "where",
                f"LogicalDisk='{letter}'",
                "get",
                "Antecedent",
                "/value",
            ],
            capture_output=True,
            text=True,
            timeout=4.0,
            check=False,
        )
        if result.returncode != 0:
            return None
        text = result.stdout or ""
        # The Antecedent looks like:
        #   \\MACHINE\root\cimv2:Win32_DiskPartition.DeviceID="Disk #0, Partition #0"
        # We extract "Disk #0" to learn the physical disk number.
        disk_number: Optional[int] = None
        for token in text.split('"'):
            lowered = token.lower()
            if "disk #" in lowered and "partition" in lowered:
                # token like "Disk #0, Partition #0"
                head = token.split(",", 1)[0]  # "Disk #0"
                digits = "".join(ch for ch in head if ch.isdigit())
                if digits:
                    disk_number = int(digits)
                break
        if disk_number is None:
            return None

        # Now ask Win32_DiskDrive for the PnPDeviceID + InterfaceType.
        result = subprocess.run(
            [
                "wmic",
                "diskdrive",
                "where",
                f"Index={disk_number}",
                "get",
                "InterfaceType,PnPDeviceID,Model",
                "/value",
            ],
            capture_output=True,
            text=True,
            timeout=4.0,
            check=False,
        )
        if result.returncode != 0:
            return {"disk_number": disk_number}
        fields: dict[str, str] = {}
        for line in (result.stdout or "").splitlines():
            if "=" not in line:
                continue
            key, _, value = line.partition("=")
            fields[key.strip().lower()] = value.strip()
        pnp_id = fields.get("pnpdeviceid", "")
        interface_type = fields.get("interfacetype", "")
        model = fields.get("model", "")
        is_usb = pnp_id.upper().startswith("USBSTOR\\") or interface_type.upper() == "USB"
        return {
            "disk_number": disk_number,
            "pnp_device_id": pnp_id,
            "interface_type": interface_type,
            "model": model,
            "is_usb": is_usb,
        }
    except Exception as exc:
        log_error("wmi_disk_lookup_failed", {"drive": drive_letter, "message": str(exc)})
        return None


# ---------------------------------------------------------------------------
# Main classifier
# ---------------------------------------------------------------------------

def classify_volume(drive_path: str) -> DeviceClassification:
    """Return the best-effort :class:`DeviceClassification` for a drive path."""
    letter = _normalize_drive_letter(drive_path)
    cache_key = letter or drive_path
    cached = _cache_get(cache_key)
    if cached is not None:
        return cached

    if not letter:
        result = DeviceClassification(
            drive_letter="",
            is_usb=False,
            bus_type=0,
            bus_type_name=BUS_TYPE_NAMES[0],
            method="invalid_path",
        )
        _cache_put(cache_key, result)
        return result

    if _is_system_drive(f"{letter}\\"):
        result = DeviceClassification(
            drive_letter=letter,
            is_usb=False,
            bus_type=0,
            bus_type_name="system",
            method="system_drive",
        )
        _cache_put(cache_key, result)
        return result

    # ----- Strategy 1: STORAGE_QUERY_PROPERTY on the volume handle -----
    descriptor = query_storage_descriptor(letter)
    physical_disk_numbers = get_volume_disk_numbers(letter)
    physical_disk = physical_disk_numbers[0] if physical_disk_numbers else None

    if descriptor is not None:
        is_usb = descriptor.bus_type in EXTERNAL_BUS_TYPES
        result = DeviceClassification(
            drive_letter=letter,
            is_usb=is_usb,
            bus_type=descriptor.bus_type,
            bus_type_name=descriptor.bus_type_name,
            method="storage_descriptor",
            physical_disk=physical_disk,
            vendor=descriptor.vendor,
            product=descriptor.product,
            removable_media=descriptor.removable_media,
        )
        _cache_put(cache_key, result)
        log_event("volume_classified", {
            "drive": letter,
            "method": result.method,
            "bus_type": result.bus_type_name,
            "is_usb": result.is_usb,
            "physical_disk": physical_disk,
        })
        return result

    # ----- Strategy 2: query the underlying physical disk ----------
    if physical_disk is not None:
        phys_descriptor = query_storage_descriptor(f"\\\\.\\PhysicalDrive{physical_disk}")
        if phys_descriptor is not None:
            is_usb = phys_descriptor.bus_type in EXTERNAL_BUS_TYPES
            result = DeviceClassification(
                drive_letter=letter,
                is_usb=is_usb,
                bus_type=phys_descriptor.bus_type,
                bus_type_name=phys_descriptor.bus_type_name,
                method="physical_disk",
                physical_disk=physical_disk,
                vendor=phys_descriptor.vendor,
                product=phys_descriptor.product,
                removable_media=phys_descriptor.removable_media,
            )
            _cache_put(cache_key, result)
            log_event("volume_classified", {
                "drive": letter,
                "method": result.method,
                "bus_type": result.bus_type_name,
                "is_usb": result.is_usb,
                "physical_disk": physical_disk,
            })
            return result

    # ----- Strategy 3: WMI fallback --------------------------------
    wmi_info = _wmi_query_disk_for_volume(letter)
    if wmi_info is not None:
        is_usb = bool(wmi_info.get("is_usb"))
        result = DeviceClassification(
            drive_letter=letter,
            is_usb=is_usb,
            bus_type=BusTypeUsb if is_usb else 0,
            bus_type_name="usb" if is_usb else (wmi_info.get("interface_type") or "unknown"),
            method="wmi",
            physical_disk=wmi_info.get("disk_number"),
            product=wmi_info.get("model", ""),
            extra={"pnp_device_id_prefix": (wmi_info.get("pnp_device_id") or "")[:8]},
        )
        _cache_put(cache_key, result)
        log_event("volume_classified", {
            "drive": letter,
            "method": result.method,
            "is_usb": result.is_usb,
            "physical_disk": result.physical_disk,
        })
        return result

    # ----- Strategy 4: drive-type fallback ------------------------
    dtype = get_drive_type_code(f"{letter}\\")
    # DRIVE_REMOVABLE without other context is too noisy — many built-in SD
    # readers report it. Treat it as "not USB" by default, so we don't pop a
    # toast for an internal slot with nothing in it. DRIVE_FIXED gets the
    # same treatment: a real USB hard disk would have been caught by the
    # storage-descriptor path above; if we got here, it's almost certainly
    # an internal disk.
    is_usb = False
    result = DeviceClassification(
        drive_letter=letter,
        is_usb=is_usb,
        bus_type=0,
        bus_type_name=BUS_TYPE_NAMES[0],
        method="drive_type_fallback",
        physical_disk=physical_disk,
        extra={"drive_type_code": dtype},
    )
    _cache_put(cache_key, result)
    log_event("volume_classified", {
        "drive": letter,
        "method": result.method,
        "drive_type_code": dtype,
        "is_usb": result.is_usb,
    })
    return result


# ---------------------------------------------------------------------------
# Bulk helpers
# ---------------------------------------------------------------------------

def usb_drive_snapshot() -> dict[str, dict[str, Any]]:
    """Return ``{drive_path: {...}}`` for drives we believe are truly USB.

    The dict values are the raw ``drive_snapshot()`` entries augmented with
    classification info (``is_usb``, ``bus_type``, ``physical_disk``,
    ``device_model``).
    """
    out: dict[str, dict[str, Any]] = {}
    for path, info in raw_drive_snapshot().items():
        classification = classify_volume(path)
        if not classification.is_usb:
            continue
        merged = dict(info)
        merged["is_usb"] = True
        merged["bus_type"] = classification.bus_type_name
        merged["physical_disk"] = classification.physical_disk
        merged["device_model"] = classification.display_model
        merged["removable_media"] = classification.removable_media
        out[path] = merged
    return out


def is_likely_usb_drive(path: str) -> bool:
    """Convenience predicate used by hot-path callers (detector, GUI)."""
    return classify_volume(path).is_usb
