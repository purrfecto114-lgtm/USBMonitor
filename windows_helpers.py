"""Win32 ctypes structures, drive enumeration, and volume helpers."""

from __future__ import annotations

import ctypes
from ctypes import wintypes
import os
import platform
import shutil
import subprocess
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Optional

from .config import APP_NAME

# ---------------------------------------------------------------------------
# DLL handles (None on non-Windows so feature code can short-circuit cleanly)
# ---------------------------------------------------------------------------

if platform.system() == "Windows":
    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    user32 = ctypes.WinDLL("user32", use_last_error=True)
else:
    kernel32 = None  # type: ignore[assignment]
    user32 = None  # type: ignore[assignment]

# ---------------------------------------------------------------------------
# Window-message and device-broadcast constants
# ---------------------------------------------------------------------------

WM_CLOSE = 0x0010
WM_DEVICECHANGE = 0x0219
DBT_CONFIGCHANGED = 0x0018
DBT_DEVNODES_CHANGED = 0x0007
DBT_DEVICEARRIVAL = 0x8000
DBT_DEVICEREMOVECOMPLETE = 0x8004
DBT_DEVTYP_VOLUME = 0x00000002
DBT_DEVTYP_DEVICEINTERFACE = 0x00000005
DBTF_MEDIA = 0x0001
DEVICE_NOTIFY_WINDOW_HANDLE = 0x00000000

DRIVE_UNKNOWN = 0
DRIVE_NO_ROOT_DIR = 1
DRIVE_REMOVABLE = 2
DRIVE_FIXED = 3
DRIVE_REMOTE = 4
DRIVE_CDROM = 5
DRIVE_RAMDISK = 6
ERROR_ALREADY_EXISTS = 183

DRIVE_TYPE_NAMES = {
    DRIVE_UNKNOWN: "unknown",
    DRIVE_NO_ROOT_DIR: "no_root",
    DRIVE_REMOVABLE: "removable",
    DRIVE_FIXED: "fixed",
    DRIVE_REMOTE: "remote",
    DRIVE_CDROM: "cdrom",
    DRIVE_RAMDISK: "ramdisk",
}

# ---------------------------------------------------------------------------
# Storage bus type codes (STORAGE_BUS_TYPE in ntddstor.h).
# We only need a handful; the full enum is far larger but unused here.
# ---------------------------------------------------------------------------

BusTypeUnknown = 0
BusTypeScsi = 1
BusTypeAtapi = 2
BusTypeAta = 3
BusType1394 = 4
BusTypeSsa = 5
BusTypeFibre = 6
BusTypeUsb = 7
BusTypeRAID = 8
BusTypeiScsi = 9
BusTypeSas = 10
BusTypeSata = 11
BusTypeSd = 12
BusTypeMmc = 13
BusTypeVirtual = 14
BusTypeFileBackedVirtual = 15

BUS_TYPE_NAMES = {
    BusTypeUnknown: "unknown",
    BusTypeScsi: "scsi",
    BusTypeAtapi: "atapi",
    BusTypeAta: "ata",
    BusType1394: "ieee1394",
    BusTypeSsa: "ssa",
    BusTypeFibre: "fibre",
    BusTypeUsb: "usb",
    BusTypeRAID: "raid",
    BusTypeiScsi: "iscsi",
    BusTypeSas: "sas",
    BusTypeSata: "sata",
    BusTypeSd: "sd",
    BusTypeMmc: "mmc",
    BusTypeVirtual: "virtual",
    BusTypeFileBackedVirtual: "file-backed-virtual",
}

# Bus types we consider "removable / external" for the purpose of this app.
# USB is the primary case. 1394 (FireWire) and SD/MMC card readers on USB
# bridges also count. Virtual/file-backed are excluded because they are
# typically RAM disks or mounted VHDs.
EXTERNAL_BUS_TYPES = {BusTypeUsb, BusType1394, BusTypeSd, BusTypeMmc}


# ---------------------------------------------------------------------------
# ctypes structures
# ---------------------------------------------------------------------------

class GUID(ctypes.Structure):
    _fields_ = [
        ("Data1", wintypes.DWORD),
        ("Data2", wintypes.WORD),
        ("Data3", wintypes.WORD),
        ("Data4", ctypes.c_ubyte * 8),
    ]


class DEV_BROADCAST_HDR(ctypes.Structure):
    _fields_ = [
        ("dbch_size", wintypes.DWORD),
        ("dbch_devicetype", wintypes.DWORD),
        ("dbch_reserved", wintypes.DWORD),
    ]


class DEV_BROADCAST_VOLUME(ctypes.Structure):
    _fields_ = [
        ("dbcv_size", wintypes.DWORD),
        ("dbcv_devicetype", wintypes.DWORD),
        ("dbcv_reserved", wintypes.DWORD),
        ("dbcv_unitmask", wintypes.DWORD),
        ("dbcv_flags", wintypes.WORD),
    ]


class DEV_BROADCAST_DEVICEINTERFACE_W(ctypes.Structure):
    _fields_ = [
        ("dbcc_size", wintypes.DWORD),
        ("dbcc_devicetype", wintypes.DWORD),
        ("dbcc_reserved", wintypes.DWORD),
        ("dbcc_classguid", GUID),
        ("dbcc_name", ctypes.c_wchar * 1),
    ]


# STORAGE_PROPERTY_QUERY
class STORAGE_PROPERTY_QUERY(ctypes.Structure):
    _fields_ = [
        ("PropertyId", wintypes.DWORD),
        ("QueryType", wintypes.DWORD),
        ("AdditionalParameters", ctypes.c_ubyte * 1),
    ]


# STORAGE_DEVICE_DESCRIPTOR (variable-size; we allocate enough trailing bytes
# for the vendor/product/serial strings).
class STORAGE_DEVICE_DESCRIPTOR(ctypes.Structure):
    _fields_ = [
        ("Version", wintypes.DWORD),
        ("Size", wintypes.DWORD),
        ("DeviceType", ctypes.c_ubyte),
        ("DeviceTypeModifier", ctypes.c_ubyte),
        ("RemovableMedia", ctypes.c_ubyte),
        ("CommandQueueing", ctypes.c_ubyte),
        ("VendorIdOffset", wintypes.DWORD),
        ("ProductIdOffset", wintypes.DWORD),
        ("ProductRevisionOffset", wintypes.DWORD),
        ("SerialNumberOffset", wintypes.DWORD),
        ("BusType", wintypes.DWORD),
        ("RawPropertiesLength", wintypes.DWORD),
        ("RawDeviceProperties", ctypes.c_ubyte * 1),
    ]


class DISK_EXTENT(ctypes.Structure):
    _fields_ = [
        ("DiskNumber", wintypes.DWORD),
        ("StartingOffset", ctypes.c_longlong),
        ("ExtentLength", ctypes.c_longlong),
    ]


class VOLUME_DISK_EXTENTS(ctypes.Structure):
    _fields_ = [
        ("NumberOfDiskExtents", wintypes.DWORD),
        ("Extents", DISK_EXTENT * 1),
    ]


# ---------------------------------------------------------------------------
# Constants for CreateFile / DeviceIoControl
# ---------------------------------------------------------------------------

GENERIC_READ = 0x80000000
GENERIC_WRITE = 0x40000000
FILE_SHARE_READ = 0x00000001
FILE_SHARE_WRITE = 0x00000002
OPEN_EXISTING = 3
INVALID_HANDLE_VALUE = ctypes.c_void_p(-1).value

IOCTL_STORAGE_QUERY_PROPERTY = 0x002D1400
IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS = 0x00560000

PropertyStandardQuery = 0
StorageDeviceProperty = 0


# ---------------------------------------------------------------------------
# DLL signature setup (done lazily so non-Windows import doesn't fail)
# ---------------------------------------------------------------------------

def _ensure_kernel32_signatures() -> None:
    if kernel32 is None:
        return
    kernel32.GetDriveTypeW.argtypes = [wintypes.LPCWSTR]
    kernel32.GetDriveTypeW.restype = wintypes.UINT
    kernel32.GetLogicalDrives.restype = wintypes.DWORD
    kernel32.CreateFileW.argtypes = [
        wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD,
        ctypes.c_void_p, wintypes.DWORD, wintypes.DWORD, ctypes.c_void_p,
    ]
    kernel32.CreateFileW.restype = wintypes.HANDLE
    kernel32.DeviceIoControl.argtypes = [
        wintypes.HANDLE, wintypes.DWORD, ctypes.c_void_p, wintypes.DWORD,
        ctypes.c_void_p, wintypes.DWORD, ctypes.POINTER(wintypes.DWORD), ctypes.c_void_p,
    ]
    kernel32.DeviceIoControl.restype = wintypes.BOOL
    kernel32.CloseHandle.argtypes = [wintypes.HANDLE]
    kernel32.CloseHandle.restype = wintypes.BOOL


# ---------------------------------------------------------------------------
# GUID helpers
# ---------------------------------------------------------------------------

def usb_device_interface_guid() -> GUID:
    return GUID(
        0xA5DCBF10, 0x6530, 0x11D2,
        (ctypes.c_ubyte * 8)(0x90, 0x1F, 0x00, 0xC0, 0x4F, 0xB9, 0x51, 0xED),
    )


# ---------------------------------------------------------------------------
# Drive enumeration
# ---------------------------------------------------------------------------

def windows_drive_paths_from_unitmask(unitmask: int) -> list[str]:
    return [f"{chr(ord('A') + index)}:\\" for index in range(26) if unitmask & (1 << index)]


def drive_type(path: str) -> int:
    if kernel32 is None:
        return DRIVE_UNKNOWN
    _ensure_kernel32_signatures()
    return int(kernel32.GetDriveTypeW(path))


def logical_drive_paths() -> list[str]:
    if kernel32 is None:
        return []
    _ensure_kernel32_signatures()
    mask = int(kernel32.GetLogicalDrives())
    return [f"{chr(ord('A') + i)}:\\" for i in range(26) if mask & (1 << i)]


def volume_label(path: str) -> str:
    if kernel32 is None:
        return ""
    volume_name = ctypes.create_unicode_buffer(261)
    fs_name = ctypes.create_unicode_buffer(261)
    serial = wintypes.DWORD()
    max_component = wintypes.DWORD()
    flags = wintypes.DWORD()
    ok = kernel32.GetVolumeInformationW(
        wintypes.LPCWSTR(path),
        volume_name,
        len(volume_name),
        ctypes.byref(serial),
        ctypes.byref(max_component),
        ctypes.byref(flags),
        fs_name,
        len(fs_name),
    )
    if not ok:
        return ""
    return volume_name.value or ""


def system_drive_path() -> str:
    """The Windows boot/system drive (usually C:\\) must never be treated as
    a monitored USB volume, even though it is a DRIVE_FIXED drive just like
    many external USB hard disks."""
    letter = (os.environ.get("SystemDrive") or "C:").strip().rstrip("\\").rstrip(":")
    return f"{letter}:\\"


# ---------------------------------------------------------------------------
# DeviceIoControl-based physical device probing
# ---------------------------------------------------------------------------

def _open_volume_handle(drive_letter_or_physical: str) -> Optional[int]:
    """Open a synchronous handle to ``\\\\.\\X:`` or ``\\\\.\\PhysicalDriveN``."""
    if kernel32 is None:
        return None
    _ensure_kernel32_signatures()
    raw = drive_letter_or_physical.strip()
    if len(raw) == 2 and raw[1] == ":":
        target = f"\\\\.\\{raw.upper()}"
    elif raw.startswith("\\\\.\\"):
        target = raw
    elif raw.lower().startswith("physicaldrive"):
        target = f"\\\\.\\{raw}"
    else:
        return None
    # Some volume handles reject GENERIC_READ for non-admin users, while the
    # storage IOCTLs we use can often work with metadata-only access. Try the
    # normal read handle first, then fall back to access=0.
    for access in (GENERIC_READ, 0):
        handle = kernel32.CreateFileW(
            target,
            access,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            None,
            OPEN_EXISTING,
            0,
            None,
        )
        if handle != INVALID_HANDLE_VALUE and handle is not None:
            return int(handle)
    return None


def _close_volume_handle(handle: int) -> None:
    if kernel32 is None:
        return
    try:
        kernel32.CloseHandle(wintypes.HANDLE(handle))
    except Exception:
        pass


@dataclass
class StorageDeviceDescriptor:
    bus_type: int
    bus_type_name: str
    removable_media: bool
    vendor: str
    product: str
    serial_offset: int
    raw: bytes


def _read_string_at(buffer: bytes, offset: int) -> str:
    """Read a NUL-terminated ASCII string from the trailing buffer."""
    if offset <= 0 or offset >= len(buffer):
        return ""
    end = buffer.find(b"\x00", offset)
    if end < 0:
        end = len(buffer)
    try:
        return buffer[offset:end].decode("ascii", errors="ignore").strip()
    except Exception:
        return ""


def query_storage_descriptor(drive_letter_or_physical: str) -> Optional[StorageDeviceDescriptor]:
    """Run IOCTL_STORAGE_QUERY_PROPERTY on a volume or physical drive.

    Returns None if the call fails (e.g. running as non-admin on some
    physical drives, or non-Windows platform). The caller should fall back
    to a less precise signal in that case.
    """
    if kernel32 is None:
        return None
    _ensure_kernel32_signatures()
    handle = _open_volume_handle(drive_letter_or_physical)
    if handle is None:
        return None
    try:
        query = STORAGE_PROPERTY_QUERY()
        query.PropertyId = StorageDeviceProperty
        query.QueryType = PropertyStandardQuery

        # First, probe with a small buffer to learn the required size.
        probe = (ctypes.c_ubyte * 512)()
        returned = wintypes.DWORD(0)
        ok = kernel32.DeviceIoControl(
            wintypes.HANDLE(handle),
            IOCTL_STORAGE_QUERY_PROPERTY,
            ctypes.byref(query),
            ctypes.sizeof(query),
            probe,
            len(probe),
            ctypes.byref(returned),
            None,
        )
        if not ok:
            return None

        header = ctypes.cast(probe, ctypes.POINTER(STORAGE_DEVICE_DESCRIPTOR)).contents
        needed = int(header.Size) + 64  # slack for trailing strings
        if needed <= len(probe):
            buffer_bytes = bytes(probe[: int(header.Size)])
        else:
            big = (ctypes.c_ubyte * needed)()
            ok = kernel32.DeviceIoControl(
                wintypes.HANDLE(handle),
                IOCTL_STORAGE_QUERY_PROPERTY,
                ctypes.byref(query),
                ctypes.sizeof(query),
                big,
                len(big),
                ctypes.byref(returned),
                None,
            )
            if not ok:
                return None
            big_header = ctypes.cast(big, ctypes.POINTER(STORAGE_DEVICE_DESCRIPTOR)).contents
            buffer_bytes = bytes(big[: int(big_header.Size)])

        desc = ctypes.cast(
            (ctypes.c_ubyte * len(buffer_bytes)).from_buffer_copy(buffer_bytes),
            ctypes.POINTER(STORAGE_DEVICE_DESCRIPTOR),
        ).contents
        bus = int(desc.BusType)
        vendor = _read_string_at(buffer_bytes, int(desc.VendorIdOffset))
        product = _read_string_at(buffer_bytes, int(desc.ProductIdOffset))
        return StorageDeviceDescriptor(
            bus_type=bus,
            bus_type_name=BUS_TYPE_NAMES.get(bus, str(bus)),
            removable_media=bool(desc.RemovableMedia),
            vendor=vendor,
            product=product,
            serial_offset=int(desc.SerialNumberOffset),
            raw=buffer_bytes,
        )
    except Exception:
        return None
    finally:
        _close_volume_handle(handle)


def get_volume_disk_numbers(drive_letter: str) -> list[int]:
    """Return the physical disk numbers backing this volume.

    A simple volume has one extent; a striped/RAID volume may have several.
    Returns an empty list if the query fails (e.g. permission denied).
    """
    if kernel32 is None:
        return []
    _ensure_kernel32_signatures()
    handle = _open_volume_handle(drive_letter)
    if handle is None:
        return []
    try:
        # Most volumes fit in 16 extents; allocate enough for that.
        max_extents = 16
        buffer_size = ctypes.sizeof(VOLUME_DISK_EXTENTS) + (max_extents - 1) * ctypes.sizeof(DISK_EXTENT)
        buffer = (ctypes.c_ubyte * buffer_size)()
        returned = wintypes.DWORD(0)
        ok = kernel32.DeviceIoControl(
            wintypes.HANDLE(handle),
            IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS,
            None,
            0,
            buffer,
            buffer_size,
            ctypes.byref(returned),
            None,
        )
        if not ok:
            return []
        extents = ctypes.cast(buffer, ctypes.POINTER(VOLUME_DISK_EXTENTS)).contents
        count = int(extents.NumberOfDiskExtents)
        if count <= 0:
            return []
        # Read the variable-length Extents array manually.
        results: list[int] = []
        offset = VOLUME_DISK_EXTENTS.Extents.offset
        for i in range(count):
            extent = DISK_EXTENT.from_buffer_copy(buffer, offset + i * ctypes.sizeof(DISK_EXTENT))
            results.append(int(extent.DiskNumber))
        return results
    except Exception:
        return []
    finally:
        _close_volume_handle(handle)


# ---------------------------------------------------------------------------
# Drive snapshot
# ---------------------------------------------------------------------------

def drive_snapshot() -> dict[str, dict[str, Any]]:
    """Return current candidate drives WITHOUT applying the USB filter.

    The USB filter (device_classifier) is applied separately so callers can
    re-filter cheaply without re-querying the OS.
    """
    drives: dict[str, dict[str, Any]] = {}
    system_drive = system_drive_path().upper()
    for path in logical_drive_paths():
        if path.upper() == system_drive:
            continue
        dtype = drive_type(path)
        if dtype in {DRIVE_NO_ROOT_DIR, DRIVE_REMOTE, DRIVE_CDROM, DRIVE_RAMDISK}:
            continue
        drives[path] = {
            "path": path,
            "drive_type": DRIVE_TYPE_NAMES.get(dtype, str(dtype)),
            "drive_type_code": dtype,
            "volume_label": volume_label(path),
        }
    return drives


def details_from_lparam(lparam: int) -> tuple[dict[str, Any], list[str]]:
    if not lparam:
        return {"kind": "device_change", "has_lparam": False}, []
    header = ctypes.cast(lparam, ctypes.POINTER(DEV_BROADCAST_HDR)).contents
    if header.dbch_devicetype == DBT_DEVTYP_VOLUME:
        volume = ctypes.cast(lparam, ctypes.POINTER(DEV_BROADCAST_VOLUME)).contents
        paths = windows_drive_paths_from_unitmask(int(volume.dbcv_unitmask))
        return {
            "kind": "volume",
            "unitmask": int(volume.dbcv_unitmask),
            "flags": int(volume.dbcv_flags),
            "media_flag": bool(volume.dbcv_flags & DBTF_MEDIA),
            "drive_paths": paths,
        }, paths
    if header.dbch_devicetype == DBT_DEVTYP_DEVICEINTERFACE:
        name_offset = DEV_BROADCAST_DEVICEINTERFACE_W.dbcc_name.offset
        device_path = ctypes.wstring_at(lparam + name_offset)
        return {"kind": "device_interface", "device_path": device_path}, []
    return {"kind": "device_change", "devicetype": int(header.dbch_devicetype)}, []


# ---------------------------------------------------------------------------
# Disk usage and formatting
# ---------------------------------------------------------------------------

def safe_disk_usage(path: str) -> tuple[Optional[int], Optional[int], Optional[int]]:
    try:
        usage = shutil.disk_usage(path)
        return int(usage.total), int(usage.used), int(usage.free)
    except OSError:
        return None, None, None


def format_bytes(value: Optional[int]) -> str:
    if value is None:
        return "未知"
    size = float(value)
    for unit in ("B", "KB", "MB", "GB", "TB", "PB"):
        if abs(size) < 1024 or unit == "PB":
            return f"{size:.0f} {unit}" if unit in {"B", "KB"} else f"{size:.1f} {unit}"
        size /= 1024
    return f"{size:.1f} PB"


def usage_percent(total: Optional[int], used: Optional[int]) -> Optional[int]:
    if not total or used is None:
        return None
    return max(0, min(100, round(used / total * 100)))


# ---------------------------------------------------------------------------
# Path manipulation
# ---------------------------------------------------------------------------

def open_path(path: str) -> None:
    os.startfile(path)  # type: ignore[attr-defined]


def reveal_in_explorer(path: str) -> None:
    """Open File Explorer at a path without stealing this app's control flow."""
    clean = str(path or "").strip()
    if len(clean) == 2 and clean[1] == ":":
        clean += "\\"
    if not clean:
        raise ValueError("empty path")
    if platform.system() != "Windows":
        open_path(clean)
        return
    if len(clean) <= 3 and clean.endswith("\\") and clean[1:2] == ":":
        open_path(clean)
        return
    subprocess.Popen(["explorer", f"/select,{clean}"], close_fds=True)


def safe_eject_drive(path: str) -> str:
    """Ask Windows Explorer Shell to safely eject a removable drive."""
    if platform.system() != "Windows":
        raise RuntimeError("安全弹出仅支持 Windows。")
    clean = str(path or "").strip()
    if len(clean) < 2 or clean[1] != ":":
        raise ValueError(f"不是有效的盘符路径：{path}")
    drive = clean[:2].upper()
    try:
        import win32com.client  # type: ignore[import-not-found]
        shell = win32com.client.Dispatch("Shell.Application")
        drives = shell.NameSpace(17)  # ssfDRIVES / This PC
        if drives is None:
            raise RuntimeError("无法访问 Windows 资源管理器的驱动器列表。")
        item = drives.ParseName(drive)
        if item is None:
            raise RuntimeError(f"未找到驱动器 {drive}。")
        verbs = item.Verbs()
        count_attr = getattr(verbs, "Count", 0)
        count = int(count_attr() if callable(count_attr) else count_attr)
        for index in range(count):
            verb = verbs.Item(index)
            name_attr = getattr(verb, "Name", "")
            name = str(name_attr() if callable(name_attr) else name_attr).replace("&", "").strip().lower()
            if any(token in name for token in ("eject", "弹出", "安全删除", "safely remove")):
                verb.DoIt()
                return drive
        item.InvokeVerb("Eject")
        return drive
    except Exception as exc:
        raise RuntimeError(f"安全弹出 {drive} 失败：{exc}") from exc


# ---------------------------------------------------------------------------
# Single-instance lock
# ---------------------------------------------------------------------------

_SINGLE_INSTANCE_HANDLE: Optional[int] = None


def acquire_single_instance_lock() -> bool:
    global _SINGLE_INSTANCE_HANDLE
    if platform.system() != "Windows" or kernel32 is None:
        return True
    _ensure_kernel32_signatures()
    kernel32.CreateMutexW.argtypes = [wintypes.LPVOID, wintypes.BOOL, wintypes.LPCWSTR]
    kernel32.CreateMutexW.restype = wintypes.HANDLE
    handle = kernel32.CreateMutexW(None, True, f"Local\\{APP_NAME}_SingleInstance")
    err = ctypes.get_last_error()
    _SINGLE_INSTANCE_HANDLE = int(handle or 0)
    return err != ERROR_ALREADY_EXISTS
