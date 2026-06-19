"""Shared GUI helpers: formatting, event-name resolution, history text."""

from __future__ import annotations

from datetime import datetime
from typing import Any, Sequence

from ..events import UsbEvent, paths_from_event
from ..config import display_name_for_path


GUI_HISTORY_LIMIT = 3
GUI_HISTORY_MAX_CHARS = 168
ACTION_SIGNS = {"add": "+", "remove": "-", "change": "±", "error": "!"}

EVENT_NAME_BY_KIND = {
    "volume": "盘符事件",
    "drive_scan": "盘符刷新",
    "device_interface": "设备接口",
    "system_device_change": "系统设备变更",
    "quick_remove": "快速拔出",
    "manual_scan": "手动扫描",
    "error": "异常",
}

EVENT_NAME_BY_CODE = {
    "DBT_DEVICEARRIVAL": "设备插入",
    "DBT_DEVICEREMOVECOMPLETE": "设备拔出",
    "DBT_DEVNODES_CHANGED": "设备节点变更",
    "DBT_CONFIGCHANGED": "配置变更",
}

THEME_LABELS = {"auto": "跟随系统", "dark": "深色", "light": "浅色"}


def display_timestamp(event: UsbEvent) -> str:
    try:
        dt = datetime.fromisoformat(event.timestamp_utc).astimezone()
        return dt.strftime("%Y-%m-%d %H:%M:%S %Z")
    except Exception:
        return event.timestamp_local.replace("T", " ")


def fit_gui_text(text: str, max_chars: int = GUI_HISTORY_MAX_CHARS) -> str:
    text = " ".join(str(text).split())
    if len(text) <= max_chars:
        return text
    return text[: max(0, max_chars - 1)].rstrip("；、,，｜| ") + "…"


def paths_from_history_item(item: dict[str, Any]) -> list[str]:
    paths = item.get("paths")
    if isinstance(paths, list):
        return [str(path) for path in paths if path]
    return []


def event_name_from_parts(action: str, details: dict[str, Any]) -> str:
    event_name = str(details.get("event_name") or "")
    kind = str(details.get("kind") or "")
    if event_name in EVENT_NAME_BY_CODE:
        return EVENT_NAME_BY_CODE[event_name]
    if kind in EVENT_NAME_BY_KIND:
        base = EVENT_NAME_BY_KIND[kind]
    else:
        base = {"add": "设备插入", "remove": "设备拔出", "change": "设备变更", "error": "异常"}.get(action, action or "事件")
    if kind == "drive_scan":
        reason = str(details.get("scan_reason") or "")
        if reason == "add":
            return "盘符新增"
        if reason == "remove":
            return "盘符移除"
        return "盘符刷新"
    return base


def event_name_from_item(item: Any) -> str:
    if isinstance(item, UsbEvent):
        return event_name_from_parts(item.action, item.details)
    if isinstance(item, dict):
        details = {
            "event_name": item.get("event_name"),
            "kind": item.get("kind"),
            "message": item.get("message"),
        }
        return event_name_from_parts(str(item.get("action") or ""), details)
    return "事件"


def change_text(action: str, paths: Sequence[str], empty_hint: str = "无盘符") -> str:
    sign = ACTION_SIGNS.get(action, "±")
    path_list = [str(path) for path in paths if path]
    if path_list:
        detail = "、".join(display_name_for_path(path) for path in path_list[:2])
        if len(path_list) > 2:
            detail += "等"
    else:
        detail = empty_hint
    return f"{sign} {detail}"


def compact_history_item(item: Any) -> str:
    if not isinstance(item, dict):
        return fit_gui_text(str(item), 34)
    action = str(item.get("action") or "change")
    name = event_name_from_item(item)
    change = change_text(action, paths_from_history_item(item))
    return fit_gui_text(f"{name} {change}", 38)


def remove_history_text(event: UsbEvent) -> str:
    raw_items = event.details.get("recent_events_before_remove")
    if not isinstance(raw_items, list) or not raw_items:
        return ""
    items = [compact_history_item(item) for item in raw_items[-GUI_HISTORY_LIMIT:]]
    return fit_gui_text("前序：" + "；".join(item for item in items if item), 92)


def gui_event_summary(event: UsbEvent, include_history: bool = False) -> str:
    paths = paths_from_event(event)
    name = event_name_from_item(event)
    change = change_text(event.action, paths)
    text = f"事件：{name}｜变动：{change}"
    history = remove_history_text(event) if include_history else ""
    if history:
        text = f"{text}｜{history}"
    return fit_gui_text(text)
