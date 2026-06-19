"""Runtime-switchable structured logging."""

from __future__ import annotations

import json
import logging
from logging.handlers import QueueHandler, QueueListener, RotatingFileHandler
import os
from pathlib import Path
import platform
import queue
import sys
import threading
import traceback
from datetime import datetime, timezone
from typing import Any, Optional

from .config import (
    APP_NAME,
    LogConfig,
    LogMode,
    SENSITIVE_KEYS,
    default_log_dir,
    hash_id,
)

LOG = logging.getLogger("usb_monitor")


def _redact_value(value: Any) -> Any:
    if value is None:
        return None
    if isinstance(value, dict):
        return {str(k): _redact_value(v) for k, v in value.items()}
    if isinstance(value, (list, tuple, set)):
        return [f"redacted:{hash_id(item)}" for item in value]
    text = str(value)
    if not text:
        return ""
    return f"redacted:{hash_id(text)}"


def sanitize_for_log(value: Any, raw: bool = False) -> Any:
    if raw:
        return value
    if isinstance(value, dict):
        result: dict[str, Any] = {}
        for key, item in value.items():
            lowered = str(key).lower()
            if lowered in SENSITIVE_KEYS:
                result[str(key)] = _redact_value(item)
            elif isinstance(item, dict):
                result[str(key)] = sanitize_for_log(item, raw=False)
            elif isinstance(item, (list, tuple, set)):
                result[str(key)] = [sanitize_for_log(child, raw=False) for child in item]
            else:
                result[str(key)] = item
        return result
    if isinstance(value, (list, tuple, set)):
        return [sanitize_for_log(child, raw=False) for child in value]
    return value


class CategoryFilter(logging.Filter):
    def __init__(self, category: str) -> None:
        super().__init__()
        self.category = category

    def filter(self, record: logging.LogRecord) -> bool:
        return getattr(record, "category", None) == self.category


class JsonFormatter(logging.Formatter):
    def format(self, record: logging.LogRecord) -> str:
        # record.created is captured at LogRecord construction time, not when
        # this formatter later runs on the QueueListener thread.
        created_utc = datetime.fromtimestamp(record.created, tz=timezone.utc)
        created_local = created_utc.astimezone()
        payload: dict[str, Any] = {
            "time_utc": created_utc.isoformat(timespec="seconds"),
            "time_local": created_local.isoformat(timespec="seconds"),
            "timezone": created_local.tzname() or "local",
            "level": record.levelname,
            "logger": record.name,
            "message": record.getMessage(),
        }
        if hasattr(record, "payload"):
            payload.update(getattr(record, "payload"))
        if record.exc_info:
            payload["exception"] = self.formatException(record.exc_info)
        return json.dumps(payload, ensure_ascii=False, sort_keys=True, default=str)


def _make_rotating_handler(path: Path, category: str, max_bytes: int, backup_count: int) -> RotatingFileHandler:
    handler = RotatingFileHandler(path, maxBytes=max_bytes, backupCount=backup_count, encoding="utf-8")
    handler.setLevel(logging.DEBUG)
    handler.addFilter(CategoryFilter(category))
    handler.setFormatter(JsonFormatter())
    return handler


def write_crash_log(
    log_dir: Path,
    exc_type: type[BaseException],
    exc: BaseException,
    tb: Any,
    thread_name: Optional[str] = None,
) -> None:
    log_dir.mkdir(parents=True, exist_ok=True)
    now_utc = datetime.now(timezone.utc).isoformat(timespec="seconds")
    now_local = datetime.now().astimezone().isoformat(timespec="seconds")
    tz = datetime.now().astimezone().tzinfo
    payload = {
        "time_utc": now_utc,
        "time_local": now_local,
        "timezone": tz.tzname(None) if tz else "local",
        "thread": thread_name,
        "type": getattr(exc_type, "__name__", str(exc_type)),
        "message": str(exc),
        "traceback": "".join(traceback.format_exception(exc_type, exc, tb)),
    }
    try:
        with (log_dir / "crash.log").open("a", encoding="utf-8") as fh:
            fh.write(json.dumps(payload, ensure_ascii=False, sort_keys=True) + "\n")
    except Exception:
        pass


class LoggingManager:
    """Runtime-switchable structured logging."""

    def __init__(self) -> None:
        self.listener: Optional[QueueListener] = None
        self.config: Optional[LogConfig] = None
        self.enabled = False
        self.raw_logs = False
        self._configured_once = False

    def configure(self, config: LogConfig, reset_logs: bool = False) -> None:
        self.stop()
        self.config = config
        self.enabled = config.mode != LogMode.OFF
        self.raw_logs = config.mode == LogMode.RAW
        LOG.raw_logs = self.raw_logs  # type: ignore[attr-defined]
        LOG.log_dir = str(config.log_dir)  # type: ignore[attr-defined]
        root = logging.getLogger()
        root.handlers.clear()
        root.setLevel(logging.INFO)

        if config.mode == LogMode.OFF:
            root.addHandler(logging.NullHandler())
            self._install_exception_hooks(config.log_dir)
            return

        config.log_dir.mkdir(parents=True, exist_ok=True)
        if reset_logs:
            self.reset_log_files(config.log_dir)

        log_queue: queue.Queue[logging.LogRecord] = queue.Queue(maxsize=2000)
        root.addHandler(QueueHandler(log_queue))
        handlers: list[logging.Handler] = [
            _make_rotating_handler(config.log_dir / "events.log", "events", config.max_bytes, config.backup_count),
            _make_rotating_handler(config.log_dir / "actions.log", "actions", config.max_bytes, config.backup_count),
            _make_rotating_handler(config.log_dir / "errors.log", "errors", config.max_bytes, config.backup_count),
        ]
        if config.console_log and getattr(sys, "stderr", None) is not None:
            stream = logging.StreamHandler()
            stream.setFormatter(logging.Formatter("%(asctime)s - %(levelname)s - %(message)s"))
            handlers.append(stream)
        self.listener = QueueListener(log_queue, *handlers, respect_handler_level=True)
        self.listener.start()
        self._install_exception_hooks(config.log_dir)
        self._configured_once = True
        log_event("logging_started", {"log_dir": str(config.log_dir), "mode": config.mode.value, "raw_logs": self.raw_logs})

    def _install_exception_hooks(self, log_dir: Path) -> None:
        def excepthook(exc_type: type[BaseException], exc: BaseException, tb: Any) -> None:
            if self.enabled:
                write_crash_log(log_dir, exc_type, exc, tb)
                log_error("unhandled_exception", {"type": getattr(exc_type, "__name__", str(exc_type)), "message": str(exc)}, exc_info=(exc_type, exc, tb))
            else:
                traceback.print_exception(exc_type, exc, tb)

        def threading_excepthook(args: threading.ExceptHookArgs) -> None:
            if self.enabled:
                write_crash_log(log_dir, args.exc_type, args.exc_value, args.exc_traceback, thread_name=args.thread.name if args.thread else None)
                log_error(
                    "thread_unhandled_exception",
                    {"thread": args.thread.name if args.thread else None, "type": getattr(args.exc_type, "__name__", str(args.exc_type)), "message": str(args.exc_value)},
                    exc_info=(args.exc_type, args.exc_value, args.exc_traceback),
                )
            else:
                traceback.print_exception(args.exc_type, args.exc_value, args.exc_traceback)

        sys.excepthook = excepthook
        threading.excepthook = threading_excepthook

    def set_mode(self, mode: LogMode) -> None:
        cfg = self.config or LogConfig(default_log_dir())
        self.configure(LogConfig(cfg.log_dir, mode, cfg.max_bytes, cfg.backup_count, cfg.console_log), reset_logs=False)

    def reset_log_files(self, log_dir: Optional[Path] = None) -> None:
        target = log_dir or (self.config.log_dir if self.config else default_log_dir())
        target.mkdir(parents=True, exist_ok=True)
        for pattern in ("events.log*", "actions.log*", "errors.log*", "crash.log*"):
            for file_path in target.glob(pattern):
                try:
                    if file_path.is_file():
                        file_path.unlink()
                except Exception:
                    pass

    def stop(self) -> None:
        if self.listener is not None:
            try:
                self.listener.stop()
            except Exception:
                pass
            self.listener = None
        logging.getLogger().handlers.clear()


LOGGER_MANAGER = LoggingManager()


def stop_logging() -> None:
    LOGGER_MANAGER.stop()


def _log_structured(category: str, message: str, payload: dict[str, Any], level: int = logging.INFO, exc_info: Any = None) -> None:
    if not LOGGER_MANAGER.enabled:
        return
    safe_payload = sanitize_for_log(payload, raw=LOGGER_MANAGER.raw_logs)
    LOG.log(level, message, extra={"category": category, "payload": safe_payload}, exc_info=exc_info)


def log_event(name: str, payload: dict[str, Any]) -> None:
    _log_structured("events", name, {"event": name, **payload}, logging.INFO)


def log_action(name: str, payload: dict[str, Any]) -> None:
    _log_structured("actions", name, {"action": name, **payload}, logging.INFO)


def log_error(name: str, payload: dict[str, Any], exc_info: Any = None) -> None:
    _log_structured("errors", name, {"error": name, **payload}, logging.ERROR, exc_info=exc_info)
