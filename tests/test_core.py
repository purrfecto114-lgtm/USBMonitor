"""Tests for the dependency-free core module."""

from __future__ import annotations

import sys
from pathlib import Path

import pytest

# Make the package importable when running ``pytest`` from the repo root
# without installing the package first.
sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from usb_monitor.core import (
    AppConfig,
    LogMode,
    SENSITIVE_KEYS,
    UsbEvent,
    VolumeInfo,
    anchored_window_geometry,
    as_bool,
    as_int,
    countdown_label,
    display_name_for_path,
    event_summary,
    format_bytes,
    group_title,
    group_volumes,
    hash_id,
    normalize_drive_path,
    normalize_recent_records,
    precise_percent,
    progress_tooltip_text,
    redact,
    sanitize_for_log,
)


# ---------------------------------------------------------------------------
# normalize_drive_path
# ---------------------------------------------------------------------------


@pytest.mark.parametrize(
    "raw,expected",
    [
        ("E:\\", "E:\\"),
        ("e:\\", "E:\\"),
        ("e:/", "E:\\"),
        ("E:/", "E:\\"),
        ("E:\\Some\\Path", "E:\\"),
        ("e:\\some\\deep\\path", "E:\\"),
        ("", ""),
        (None, ""),
        ("USB Drive", "usb drive"),
        ("   ", ""),
    ],
)
def test_normalize_drive_path(raw, expected):
    assert normalize_drive_path(raw) == expected


@pytest.mark.parametrize("p", ["E:\\", "F:\\Sub", "G:\\\\double", "z:\\"])
def test_normalize_drive_path_idempotent(p):
    """normalize ∘ normalize == normalize."""
    once = normalize_drive_path(p)
    twice = normalize_drive_path(once)
    assert once == twice


def test_normalize_drive_path_preserves_letters():
    """For paths without drive prefix, lowercasing should be casefolded."""
    assert normalize_drive_path("USBDrive") == "usbdrive"


# ---------------------------------------------------------------------------
# format_bytes
# ---------------------------------------------------------------------------


def test_format_bytes_zero():
    assert format_bytes(0) == "0 B"


def test_format_bytes_kb():
    # format_bytes uses no decimals for B/KB to match OS file-manager UX.
    assert format_bytes(1024) == "1 KB"


def test_format_bytes_mb():
    assert format_bytes(1024 * 1024) == "1.0 MB"


def test_format_bytes_gb():
    assert format_bytes(1024 ** 3) == "1.0 GB"


def test_format_bytes_tb():
    assert format_bytes(1024 ** 4) == "1.0 TB"


def test_format_bytes_none():
    assert format_bytes(None) == "未知"


def test_format_bytes_negative():
    assert format_bytes(-2048) == "-2 KB"


# ---------------------------------------------------------------------------
# LogMode.parse
# ---------------------------------------------------------------------------


@pytest.mark.parametrize(
    "raw,expected",
    [
        ("off", LogMode.OFF),
        ("OFF", LogMode.OFF),
        ("0", LogMode.OFF),
        ("disabled", LogMode.OFF),
        ("raw", LogMode.RAW),
        ("RAW", LogMode.RAW),
        ("明文", LogMode.RAW),
        ("redacted", LogMode.REDACTED),
        (None, LogMode.REDACTED),
        ("", LogMode.REDACTED),
        ("garbage", LogMode.REDACTED),
    ],
)
def test_log_mode_parse(raw, expected):
    assert LogMode.parse(raw) is expected


# ---------------------------------------------------------------------------
# Coercion helpers
# ---------------------------------------------------------------------------


@pytest.mark.parametrize(
    "raw,default,expected",
    [
        ("1", False, True),
        ("true", False, True),
        ("YES", False, True),
        ("on", False, True),
        ("0", True, False),
        ("false", True, False),
        (None, True, True),
        (None, False, False),
        (True, False, True),
        (False, True, False),
    ],
)
def test_as_bool(raw, default, expected):
    assert as_bool(raw, default=default) is expected


@pytest.mark.parametrize(
    "raw,default,minimum,expected",
    [
        ("10", 0, None, 10),
        ("abc", 5, None, 5),
        (None, 7, None, 7),
        ("100", 0, 10, 100),
        ("3", 10, 5, 5),  # below minimum → clamp up to minimum
        ("-50", 0, -10, -10),
    ],
)
def test_as_int(raw, default, minimum, expected):
    assert as_int(raw, default, minimum) == expected


def test_hash_id_stable():
    assert hash_id("hello") == hash_id("hello")


def test_hash_id_distinguishes_inputs():
    assert hash_id("hello") != hash_id("world")


def test_hash_id_short():
    assert len(hash_id("anything")) == 12


# ---------------------------------------------------------------------------
# Volume grouping
# ---------------------------------------------------------------------------


def _v(path: str, disk: int | None = None, label: str = "") -> VolumeInfo:
    return VolumeInfo(path=path, title=label or path, drive_type="removable",
                      disk_number=disk, total=100, used=50, free=50, label=label)


def test_group_volumes_empty():
    assert group_volumes([]) == []


def test_group_volumes_by_disk_number():
    v1 = _v("E:\\", 1)
    v2 = _v("F:\\", 1)
    v3 = _v("G:\\", 2)
    groups = group_volumes([v1, v2, v3])
    assert len(groups) == 2
    paths_per_group = [{v.path for v in g} for g in groups]
    assert {"E:\\", "F:\\"} in paths_per_group
    assert {"G:\\"} in paths_per_group


def test_group_volumes_no_disk_separates():
    v1 = _v("E:\\", None)
    v2 = _v("F:\\", None)
    groups = group_volumes([v1, v2])
    assert len(groups) == 2


def test_group_title_single():
    # Single-volume group returns the volume's title field verbatim.
    assert group_title([_v("E:\\", 1, "BACKUP")]) == "BACKUP"


def test_group_title_multi_partition():
    g = [_v("E:\\", 1, "BACKUP"), _v("F:\\", 1, "BACKUP")]
    title = group_title(g)
    assert "BACKUP" in title
    assert "2 个分区" in title
    assert "E:\\" in title and "F:\\" in title


def test_group_title_fallback_label():
    g = [_v("E:\\", 1, ""), _v("F:\\", 1, "")]
    assert "USB 存储设备" in group_title(g)


# ---------------------------------------------------------------------------
# event_summary
# ---------------------------------------------------------------------------


def _ev(action: str, paths=(), message: str | None = None) -> UsbEvent:
    details = {"message": message} if message else {}
    return UsbEvent(action=action, changed_paths=tuple(paths),
                    snapshot=(), details=details)


def test_event_summary_add():
    s = event_summary(_ev("add", ["E:\\"]))
    assert "已连接" in s and "E:\\" in s


def test_event_summary_remove():
    s = event_summary(_ev("remove", ["F:\\"]))
    assert "已移除" in s and "F:\\" in s


def test_event_summary_many_paths_truncated():
    s = event_summary(_ev("add", ["E:\\", "F:\\", "G:\\", "H:\\", "I:\\"]))
    assert " 等" in s  # 4+ → truncation marker


def test_event_summary_message_fallback():
    s = event_summary(_ev("change", [], message="custom msg"))
    assert s == "custom msg"


def test_event_summary_unknown_action():
    s = event_summary(_ev("frobnicate", []))
    assert "USB frobnicate" in s


# ---------------------------------------------------------------------------
# Sensitive field redaction
# ---------------------------------------------------------------------------


def test_sanitize_for_log_raw_passthrough():
    payload = {"path": "E:\\", "label": "BACKUP"}
    assert sanitize_for_log(payload, raw=True) == payload


def test_sanitize_for_log_strips_sensitive_keys():
    payload = {"path": "E:\\", "label": "BACKUP", "count": 3}
    clean = sanitize_for_log(payload)
    assert "count" in clean and clean["count"] == 3
    assert "redacted:" in str(clean["path"])
    assert "redacted:" in str(clean["label"])


def test_sanitize_for_log_case_insensitive():
    payload = {"PATH": "E:\\", "Label": "BACKUP"}
    clean = sanitize_for_log(payload)
    assert "redacted:" in str(clean["PATH"])
    assert "redacted:" in str(clean["Label"])


def test_redact_mapping():
    out = redact({"a": 1, "b": "secret"})
    assert isinstance(out, dict)
    assert all(v.startswith("redacted:") for v in out.values())


def test_redact_list():
    out = redact(["a", "b", "c"])
    assert all(v.startswith("redacted:") for v in out)


def test_redact_none():
    assert redact(None) is None


def test_sensitive_keys_frozen():
    """SENSITIVE_KEYS must be a frozenset so it can't be mutated at runtime."""
    assert isinstance(SENSITIVE_KEYS, frozenset)


# ---------------------------------------------------------------------------
# anchored_window_geometry
# ---------------------------------------------------------------------------


def test_anchored_window_geometry_basic():
    work = (0, 0, 1920, 1080)
    size = (400, 300)
    x, y, w, h = anchored_window_geometry(work, size, 18)
    # Bottom-right corner, 18px margin
    assert (x, y, w, h) == (1502, 762, 400, 300)


def test_anchored_window_geometry_clamp_width():
    work = (0, 0, 800, 600)
    size = (1000, 400)  # request wider than work area
    x, y, w, h = anchored_window_geometry(work, size, 18)
    assert w <= 800 - 2 * 18
    assert x >= 0


def test_anchored_window_geometry_negative_origin():
    """Multi-monitor setups can have negative virtual-desktop coordinates."""
    work = (-1920, 0, 1920, 1080)
    x, y, w, h = anchored_window_geometry(work, (400, 300), 18)
    # Should clamp inside the left monitor's right edge
    assert -1920 <= x <= -1920 + 1920 - w


def test_anchored_window_geometry_invalid_work():
    with pytest.raises(ValueError):
        anchored_window_geometry((0, 0, 0, 1080), (400, 300), 18)
    with pytest.raises(ValueError):
        anchored_window_geometry((0, 0, 1920, 0), (400, 300), 18)


def test_anchored_window_geometry_negative_margin_treated_as_zero():
    work = (0, 0, 1920, 1080)
    x, y, w, h = anchored_window_geometry(work, (400, 300), -5)
    assert x == 1920 - 400  # margin=0 → flush with edge


# ---------------------------------------------------------------------------
# normalize_recent_records
# ---------------------------------------------------------------------------


def test_normalize_recent_records_empty():
    assert normalize_recent_records(None) == []
    assert normalize_recent_records([]) == []
    assert normalize_recent_records("not a list") == []


def test_normalize_recent_records_skips_invalid():
    items = [
        None,
        "string",
        {"path": "e:/", "label": "BACKUP"},
        {"path": ""},                   # empty path
        {"path": "f:\\", "title": "F"},
    ]
    out = normalize_recent_records(items)
    paths = [item["path"] for item in out]
    assert "E:\\" in paths
    assert "F:\\" in paths
    assert len(out) == 2


def test_normalize_recent_records_dedupes():
    items = [
        {"path": "E:\\", "label": "A"},
        {"path": "e:\\", "label": "B"},  # duplicate (case-insensitive)
        {"path": "E:\\Sub\\Path", "label": "C"},
    ]
    out = normalize_recent_records(items)
    paths = [item["path"] for item in out]
    assert paths.count("E:\\") == 1


def test_normalize_recent_records_limit():
    items = [{"path": f"{chr(65+i)}:\\"} for i in range(20)]
    out = normalize_recent_records(items)
    assert len(out) == 10


# ---------------------------------------------------------------------------
# AppConfig smoke test
# ---------------------------------------------------------------------------


def test_app_config_defaults(tmp_path):
    config = AppConfig(log_dir=tmp_path)
    assert config.log_mode is LogMode.REDACTED
    assert config.theme == "auto"
    assert config.topmost is True
    assert config.recent_volumes == []


# ---------------------------------------------------------------------------
# precise_percent / progress_tooltip_text
# ---------------------------------------------------------------------------


def test_precise_percent_normal():
    assert precise_percent(50, 100) == 50.0
    assert precise_percent(0, 100) == 0.0
    assert precise_percent(100, 100) == 100.0


def test_precise_percent_clamps_over_100():
    """Filesystem usage > 100% is impossible; clamp instead of returning > 100."""
    assert precise_percent(150, 100) == 100.0


def test_precise_percent_clamps_negative_used():
    """Defensive: negative used is treated as 0."""
    assert precise_percent(-10, 100) == 0.0


def test_precise_percent_zero_total():
    assert precise_percent(50, 0) is None
    assert precise_percent(50, None) is None


def test_precise_percent_decimal():
    assert precise_percent(33, 100) == 33.0
    assert precise_percent(1, 3) == 33.3


def test_progress_tooltip_text_basic():
    text = progress_tooltip_text(100, 25, 75)
    assert "已用 25 B" in text
    assert "剩余 75 B" in text
    assert "25.0%" in text
    assert "100 字节" in text


def test_progress_tooltip_text_unknown_total():
    assert progress_tooltip_text(None, 0, 0) == "容量：未知"


def test_progress_tooltip_text_unknown_used():
    text = progress_tooltip_text(1024 * 1024, None, 512 * 1024)
    # We can't read the volume's usage; the tooltip should still show the total
    # and label missing values as '未知' rather than failing.
    assert "未知" in text
    assert "1,048,576" in text  # raw byte count for 1 MiB (with thousands sep)
    assert "512 KB" in text  # free is known


def test_progress_tooltip_text_kb_units():
    text = progress_tooltip_text(2 * 1024, 1024, 1024)
    assert "已用 1 KB" in text
    assert "剩余 1 KB" in text
    assert "50.0%" in text


# ---------------------------------------------------------------------------
# countdown_label
# ---------------------------------------------------------------------------


def test_countdown_label_seconds():
    assert countdown_label(500) == "1 秒后自动关闭"
    assert countdown_label(5_000) == "5 秒后自动关闭"
    # 59.5s remaining should round up to 60s, not flip into '1 分钟后…'
    assert countdown_label(59_500) == "60 秒后自动关闭"


def test_countdown_label_minutes():
    # 60s is the cutover point — show '1 分钟后…' only at >= 60_000ms.
    assert countdown_label(60_000) == "1 分钟后自动关闭"
    assert countdown_label(180_000) == "3 分钟后自动关闭"


def test_countdown_label_zero():
    assert countdown_label(0) == "即将关闭"


def test_countdown_label_negative():
    assert countdown_label(-100) == "即将关闭"