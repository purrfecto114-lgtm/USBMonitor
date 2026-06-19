"""Toast notification window."""

from __future__ import annotations

import time
from typing import Any, Callable, Optional, Sequence

from PySide6.QtCore import (
    QEasingCurve,
    QEvent,
    QObject,
    QParallelAnimationGroup,
    QPoint,
    QPropertyAnimation,
    QRect,
    QTimer,
    Qt,
    Signal,
)
from PySide6.QtGui import QCursor, QGraphicsDropShadowEffect, QIcon
from PySide6.QtWidgets import (
    QApplication,
    QFrame,
    QHBoxLayout,
    QLabel,
    QPushButton,
    QScrollArea,
    QSizePolicy,
    QVBoxLayout,
    QWidget,
)

from ..config import APP_DISPLAY_NAME, AppConfig, ConfigStore, hash_id
from ..device_classifier import invalidate_cache
from ..events import UsbEvent, paths_from_event
from ..logging_setup import log_action, log_error, log_event
from ..recent import remember_recent_volume
from ..volume_grouping import (
    PhysicalDeviceGroup,
    group_volumes_by_physical_device,
)
from ..windows_helpers import (
    open_path,
    reveal_in_explorer,
    safe_eject_drive,
    user32,
)
from ..config import display_name_for_path
from .helpers import (
    display_timestamp,
    gui_event_summary,
)
from .theme import IconFactory, Theme
from .widgets import VolumeRow


class Bridge(QObject):
    event_received = Signal(object)


class Toast(QWidget):
    AUTO_HIDE_MS = 10_000
    DEBOUNCE_MS = 140
    FAST_FLUSH_ACTIONS = {"remove", "error"}
    ANIM_MS = 260

    def __init__(
        self,
        app: QApplication,
        theme: Theme,
        icons: IconFactory,
        topmost: bool,
        app_config: AppConfig,
        config_store: ConfigStore,
    ) -> None:
        super().__init__(None)
        self.app = app
        self.theme = theme
        self.icons = icons
        self.app_config = app_config
        self.config_store = config_store
        self.events: list[UsbEvent] = []
        self.pending_events: list[UsbEvent] = []
        # volumes maps drive-letter path -> VolumeInfo (one entry per
        # *partition*, even if several share a physical disk)
        self.volumes: dict[str, Any] = {}
        self.last_paths: list[str] = []
        self.collapsed = True
        self.keep_topmost = topmost
        self._recent_ui: dict[str, float] = {}
        self._animation: Optional[QParallelAnimationGroup] = None
        self._animating_size = False
        self._hiding = False
        self.px: Callable[[float], int] = theme.px

        self.hide_timer = QTimer(self)
        self.hide_timer.setSingleShot(True)
        self.hide_timer.timeout.connect(self.animate_hide)
        self.debounce_timer = QTimer(self)
        self.debounce_timer.setSingleShot(True)
        self.debounce_timer.timeout.connect(self.flush_pending_events)

        self.setWindowTitle(APP_DISPLAY_NAME)
        self.setWindowIcon(self.icons.app_icon(theme))
        flags = Qt.Tool | Qt.FramelessWindowHint
        if self.keep_topmost:
            flags |= Qt.WindowStaysOnTopHint
        if hasattr(Qt, "WindowDoesNotAcceptFocus"):
            flags |= Qt.WindowDoesNotAcceptFocus
        self.setWindowFlags(flags)
        self.setAttribute(Qt.WA_TranslucentBackground, True)
        self.setStyleSheet(theme.style_sheet())
        self.setFixedSize(*self.target_size())

        px = self.px
        outer = QVBoxLayout(self)
        outer.setContentsMargins(px(12), px(12), px(12), px(12))
        self.root = QFrame()
        self.root.setObjectName("root")
        outer.addWidget(self.root)
        shadow = QGraphicsDropShadowEffect(self.root)
        shadow.setBlurRadius(px(28))
        shadow.setOffset(0, px(8))
        shadow.setColor(theme.shadow_color)
        self.root.setGraphicsEffect(shadow)

        layout = QVBoxLayout(self.root)
        layout.setContentsMargins(px(16), px(14), px(16), px(14))
        layout.setSpacing(px(10))

        top = QHBoxLayout()
        self.app_icon = QLabel()
        self.app_icon.setObjectName("appIcon")
        self.app_icon.setAlignment(Qt.AlignCenter)
        self.app_icon.setFixedSize(px(34), px(34))
        self.app_icon.setPixmap(self.icons.pixmap("usb", theme, px(32)))
        top.addWidget(self.app_icon)

        title_box = QVBoxLayout()
        title_box.setSpacing(px(2))
        self.headline = QLabel("USB 设备监控")
        self.headline.setObjectName("headline")
        self.subtitle = QLabel("等待 USB 设备事件")
        self.subtitle.setObjectName("muted")
        title_box.addWidget(self.headline)
        title_box.addWidget(self.subtitle)
        top.addLayout(title_box, 1)

        self.count_label = QLabel("")
        self.count_label.setObjectName("count")
        self.count_label.setAlignment(Qt.AlignRight | Qt.AlignTop)
        top.addWidget(self.count_label)
        layout.addLayout(top)

        self.summary = QLabel("插入 U 盘后会显示可打开位置。")
        self.summary.setObjectName("summary")
        self.summary.setWordWrap(True)
        self.summary.setMaximumHeight(px(40))
        self.summary.setTextInteractionFlags(Qt.TextSelectableByMouse)
        layout.addWidget(self.summary)

        self.scroll = QScrollArea()
        self.scroll.setWidgetResizable(True)
        self.scroll.setHorizontalScrollBarPolicy(Qt.ScrollBarAlwaysOff)
        self.scroll.setMinimumHeight(px(108))
        self.scroll.setMaximumHeight(px(250))
        self.rows_widget = QWidget()
        self.rows_layout = QVBoxLayout(self.rows_widget)
        self.rows_layout.setContentsMargins(0, 0, 0, 0)
        self.rows_layout.setSpacing(px(8))
        self.scroll.setWidget(self.rows_widget)
        layout.addWidget(self.scroll, 1)

        buttons = QHBoxLayout()
        buttons.setSpacing(px(8))
        self.toggle = QPushButton("展开")
        self.toggle.setMinimumHeight(px(42))
        self.toggle.setMinimumWidth(px(78))
        self.toggle.clicked.connect(self.toggle_expanded)
        self.close_btn = QPushButton("关闭")
        self.close_btn.setMinimumHeight(px(42))
        self.close_btn.setMinimumWidth(px(78))
        self.close_btn.clicked.connect(lambda: self._log_and_hide("toast_close_button"))
        self.primary_open = QPushButton("打开U盘")
        self.primary_open.setObjectName("primaryButton")
        self.primary_open.setMinimumHeight(px(42))
        self.primary_open.setMinimumWidth(px(110))
        self.primary_open.clicked.connect(self.open_first)
        buttons.addWidget(self.toggle)
        buttons.addWidget(self.close_btn)
        buttons.addStretch(1)
        buttons.addWidget(self.primary_open)
        layout.addLayout(buttons)

        self.installEventFilter(self)
        self.root.installEventFilter(self)
        self.refresh()

    # ------------------------------------------------------------------
    # Layout
    # ------------------------------------------------------------------

    def target_size(self) -> tuple[int, int]:
        px = self.px
        return (px(510), px(274)) if self.collapsed else (px(510), px(432))

    def _rect_for_size(self, size: tuple[int, int], bottom_right: Optional[QPoint] = None) -> QRect:
        px = self.px
        width, height = size
        if bottom_right is None:
            screen = self.app.screenAt(QCursor.pos()) or self.app.primaryScreen()
            if screen is None:
                return QRect(0, 0, width, height)
            rect = screen.availableGeometry()
            margin = px(18)
            x = rect.right() - width - margin
            y = rect.bottom() - height - margin
            return QRect(max(rect.left() + margin, x), max(rect.top() + margin, y), width, height)
        return QRect(bottom_right.x() - width + 1, bottom_right.y() - height + 1, width, height)

    def _animate_geometry_opacity(
        self,
        start: QRect,
        end: QRect,
        start_opacity: float,
        end_opacity: float,
        finished: Optional[Callable[[], None]] = None,
        duration: Optional[int] = None,
    ) -> None:
        if self._animation is not None:
            self._animation.stop()
        group = QParallelAnimationGroup(self)
        geo = QPropertyAnimation(self, b"geometry", self)
        geo.setDuration(duration or self.ANIM_MS)
        geo.setStartValue(start)
        geo.setEndValue(end)
        geo.setEasingCurve(QEasingCurve.OutCubic)
        opacity = QPropertyAnimation(self, b"windowOpacity", self)
        opacity.setDuration(duration or self.ANIM_MS)
        opacity.setStartValue(start_opacity)
        opacity.setEndValue(end_opacity)
        opacity.setEasingCurve(QEasingCurve.OutCubic)
        group.addAnimation(geo)
        group.addAnimation(opacity)
        if finished is not None:
            group.finished.connect(finished)
        self._animation = group
        group.start()

    def eventFilter(self, obj: QObject, event: QEvent) -> bool:
        if event.type() in (QEvent.MouseButtonPress, QEvent.MouseMove, QEvent.MouseButtonRelease, QEvent.Enter):
            self.hide_timer.start(self.AUTO_HIDE_MS)
        return super().eventFilter(obj, event)

    def _log_and_hide(self, action: str) -> None:
        log_action(action, {"visible": self.isVisible(), "path_count": len(self.last_paths)})
        self.animate_hide()

    # ------------------------------------------------------------------
    # Theme / topmost
    # ------------------------------------------------------------------

    def apply_theme(self, theme: Theme) -> None:
        self.theme = theme
        self.px = theme.px
        self.setWindowIcon(self.icons.app_icon(theme))
        self.setStyleSheet(theme.style_sheet())
        effect = self.root.graphicsEffect()
        if isinstance(effect, QGraphicsDropShadowEffect):
            effect.setColor(theme.shadow_color)
        self.app_icon.setPixmap(self.icons.pixmap("usb", theme, self.px(32)))
        self.refresh()

    def set_topmost(self, enabled: bool) -> None:
        was_visible = self.isVisible()
        old_geometry = self.geometry()
        self.keep_topmost = bool(enabled)
        flags = Qt.Tool | Qt.FramelessWindowHint
        if self.keep_topmost:
            flags |= Qt.WindowStaysOnTopHint
        if hasattr(Qt, "WindowDoesNotAcceptFocus"):
            flags |= Qt.WindowDoesNotAcceptFocus
        self.setWindowFlags(flags)
        if was_visible:
            self.setGeometry(old_geometry)
            self.show()
            self.raise_()
            self.ensure_topmost_without_focus()

    # ------------------------------------------------------------------
    # User actions
    # ------------------------------------------------------------------

    def reveal_path(self, path: str) -> None:
        try:
            reveal_in_explorer(path)
            remember_recent_volume(self.app_config, self.config_store, path, opened=True)
            log_action("reveal_path", {"path": path, "path_hash": hash_id(path)})
        except Exception as exc:
            log_error("reveal_path_failed", {"path": path, "message": str(exc)}, exc_info=True)
            self.queue_event(UsbEvent(action="error", details={"kind": "error", "message": f"定位失败：{exc}"}))

    def copy_path(self, path: str) -> None:
        self.app.clipboard().setText(path)
        self.summary.setText(f"已复制路径：{path}")
        self.summary.setToolTip(path)
        self.hide_timer.start(self.AUTO_HIDE_MS)
        log_action("copy_path", {"path": path, "path_hash": hash_id(path)})

    def safe_eject_path(self, path: str) -> None:
        try:
            drive = safe_eject_drive(path)
        except Exception as exc:
            log_error("safe_eject_failed", {"path": path, "message": str(exc)}, exc_info=True)
            self.queue_event(UsbEvent(action="error", details={"kind": "error", "message": str(exc)}))
            return
        log_action("safe_eject_requested", {"path": path, "drive": drive, "path_hash": hash_id(path)})
        self.summary.setText(f"已请求安全弹出 {drive}，请等待 Windows 完成提示。")
        self.summary.setToolTip(self.summary.text())
        self.hide_timer.start(self.AUTO_HIDE_MS)

    def _set_status_icon(self, action: Optional[str]) -> None:
        kind = {"error": "error", "remove": "remove", "change": "change", "add": "add"}.get(action or "", "usb")
        self.app_icon.setPixmap(self.icons.pixmap(kind, self.theme, self.px(32)))

    def _fingerprint(self, event: UsbEvent) -> str:
        return hash_id(
            __import__("json").dumps(
                {"a": event.action, "p": paths_from_event(event), "k": event.details.get("kind")},
                sort_keys=True,
                default=str,
            )
        )

    # ------------------------------------------------------------------
    # Event pipeline
    # ------------------------------------------------------------------

    def queue_event(self, event: UsbEvent) -> None:
        fp = self._fingerprint(event)
        now = time.monotonic()
        if fp in self._recent_ui and now - self._recent_ui[fp] < 0.45:
            log_event("ui_event_deduplicated", {"fingerprint": fp, "action": event.action})
            return
        self._recent_ui[fp] = now
        self.pending_events.append(event)
        if event.action in self.FAST_FLUSH_ACTIONS:
            self.debounce_timer.stop()
            QTimer.singleShot(0, self.flush_pending_events)
        else:
            self.debounce_timer.start(self.DEBOUNCE_MS)

    def flush_pending_events(self) -> None:
        if not self.pending_events:
            return
        events = self.pending_events[:]
        self.pending_events.clear()
        self.apply_events(events)
        self.prune_missing_volumes()
        self.refresh()
        self.show_toast()
        if any(paths_from_event(e) for e in events):
            QTimer.singleShot(900, self.refresh_capacity_after_mount)

    def apply_events(self, events: Sequence[UsbEvent]) -> None:
        # Always rebuild the volume set from the filtered USB snapshot.
        # This way we naturally pick up partitions of the same physical
        # device that arrived together, and we never keep stale rows.
        invalidate_cache()
        groups = group_volumes_by_physical_device()
        self.volumes = {}
        for group in groups:
            for vol in group.volumes:
                self.volumes[vol.path] = vol
                remember_recent_volume(self.app_config, self.config_store, vol, opened=False)
        # Track events for the headline / summary line.
        for event in events:
            self.events.insert(0, event)
            self.events = self.events[:32]
        if len(self.volumes) > 12:
            self.volumes = dict(list(self.volumes.items())[-12:])
        self.last_paths = list(self.volumes.keys())

    def prune_missing_volumes(self) -> None:
        invalidate_cache()
        groups = group_volumes_by_physical_device()
        current_paths = {vol.path for group in groups for vol in group.volumes}
        changed = False
        for path in list(self.volumes):
            if path not in current_paths:
                self.volumes.pop(path, None)
                changed = True
        if changed:
            log_event("ui_stale_volume_pruned", {"remaining_count": len(self.volumes)})
        self.last_paths = list(self.volumes.keys())

    def refresh_capacity_after_mount(self) -> None:
        invalidate_cache()
        groups = group_volumes_by_physical_device()
        new_volumes: dict[str, Any] = {}
        for group in groups:
            for vol in group.volumes:
                new_volumes[vol.path] = vol
        self.volumes = new_volumes
        self.last_paths = list(self.volumes.keys())
        self.refresh()
        if self.isVisible():
            self.move_to_bottom_right()

    def clear_rows(self) -> None:
        while self.rows_layout.count():
            item = self.rows_layout.takeAt(0)
            widget = item.widget()
            if widget is not None:
                widget.deleteLater()

    def current_paths(self) -> list[str]:
        return list(self.volumes.keys())

    def refresh(self) -> None:
        self.clear_rows()
        volume_list = list(self.volumes.values())
        latest = self.events[0] if self.events else None
        volume_count = len(volume_list)
        if latest:
            action_text = {"add": "已连接", "change": "已更新", "remove": "已移除", "error": "异常"}.get(latest.action, latest.action)
            self.headline.setText(f"USB {action_text}")
            self._set_status_icon(latest.action)
        else:
            self.headline.setText("USB 设备监控")
            self._set_status_icon(None)
        if volume_count:
            total = sum(item.total or 0 for item in volume_list)
            free = sum(item.free or 0 for item in volume_list)
            path_preview = "、".join(display_name_for_path(v.path) for v in volume_list[:2])
            more = f"，另有 {volume_count - 2} 个" if volume_count > 2 else ""
            if latest and latest.action == "remove":
                self.subtitle.setText(f"最近事件：{display_timestamp(latest)}")
                summary = gui_event_summary(latest, include_history=True)
            else:
                from ..windows_helpers import format_bytes

                self.subtitle.setText(
                    f"{volume_count} 个可打开位置 · 总计 {format_bytes(total) if total else '读取中'} · 可用 {format_bytes(free) if free else '读取中'}"
                )
                summary = f"可打开：{path_preview}{more}"
            self.summary.setText(summary)
            self.summary.setToolTip(summary)
        elif latest:
            self.subtitle.setText(f"最近事件：{display_timestamp(latest)}")
            if latest.action in {"remove", "add", "change", "error"}:
                summary = gui_event_summary(latest, include_history=latest.action == "remove")
            else:
                summary = str(latest.details.get("message") or latest.details.get("kind") or "USB 设备事件")
            self.summary.setText(summary)
            self.summary.setToolTip(summary)
        else:
            self.subtitle.setText("等待 USB 设备事件")
            self.summary.setText("插入 U 盘后会显示可打开位置。")
            self.summary.setToolTip("")
        shown = volume_list if not self.collapsed else volume_list[:1]
        for info in shown:
            self.rows_layout.addWidget(
                VolumeRow(
                    info,
                    self.open_and_hide,
                    self.reveal_path,
                    self.copy_path,
                    self.safe_eject_path,
                    self.theme,
                    self.icons,
                )
            )
        self.rows_layout.addStretch(1)
        self.scroll.setVisible(bool(shown))
        self.primary_open.setVisible(volume_count > 0)
        self.primary_open.setText("打开U盘" if volume_count <= 1 else "打开第一个")
        self.toggle.setVisible(volume_count > 1)
        self.toggle.setText("展开" if self.collapsed else "折叠")
        self.count_label.setText(f"{volume_count} 个" if volume_count else "")
        if not self._animating_size:
            self.setFixedSize(*self.target_size())

    # ------------------------------------------------------------------
    # Show / hide
    # ------------------------------------------------------------------

    def show_toast(self) -> None:
        self._hiding = False
        end = self._rect_for_size(self.target_size())
        if self.isVisible():
            start = self.geometry()
            start_opacity = max(0.25, self.windowOpacity())
        else:
            start = QRect(end.x(), end.y() + self.px(16), end.width(), end.height())
            start_opacity = 0.0
            self.setGeometry(start)
            self.setWindowOpacity(0.0)
        self.show()
        self.raise_()
        self.ensure_topmost_without_focus()
        self._animate_geometry_opacity(start, end, start_opacity, 1.0, duration=240)
        self.hide_timer.start(self.AUTO_HIDE_MS)
        log_action("toast_shown", {"volume_count": len(self.volumes), "event_count": len(self.events)})

    def animate_hide(self) -> None:
        if not self.isVisible() or self._hiding:
            return
        self._hiding = True
        self.hide_timer.stop()
        start = self.geometry()
        end = QRect(start.x(), start.y() + self.px(14), start.width(), start.height())

        def finish() -> None:
            QWidget.hide(self)
            self.setWindowOpacity(1.0)
            self._hiding = False

        self._animate_geometry_opacity(start, end, self.windowOpacity(), 0.0, finish, duration=210)

    def ensure_topmost_without_focus(self) -> None:
        if not self.keep_topmost or user32 is None:
            return
        try:
            hwnd = int(self.winId())
            HWND_TOPMOST = -1
            SWP_NOMOVE = 0x0002
            SWP_NOSIZE = 0x0001
            SWP_NOACTIVATE = 0x0010
            SWP_SHOWWINDOW = 0x0040
            user32.SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW)
        except Exception:
            log_error("topmost_failed", {}, exc_info=True)

    def move_to_bottom_right(self) -> None:
        self.setGeometry(self._rect_for_size(self.target_size()))

    def toggle_expanded(self) -> None:
        old_rect = self.geometry() if self.isVisible() else self._rect_for_size(self.target_size())
        old_bottom_right = old_rect.bottomRight()
        self.collapsed = not self.collapsed
        log_action("toast_toggle", {"collapsed": self.collapsed, "volume_count": len(self.volumes)})
        self._animating_size = True
        self.setMinimumSize(1, 1)
        self.setMaximumSize(16777215, 16777215)
        self.refresh()
        target = self._rect_for_size(self.target_size(), bottom_right=old_bottom_right)
        geo = QPropertyAnimation(self, b"geometry", self)
        geo.setDuration(260)
        geo.setStartValue(old_rect)
        geo.setEndValue(target)
        geo.setEasingCurve(QEasingCurve.OutCubic)

        def finish() -> None:
            self._animating_size = False
            self.setFixedSize(*self.target_size())
            self.setGeometry(target)
            self.hide_timer.start(self.AUTO_HIDE_MS)

        geo.finished.connect(finish)
        self._animation = QParallelAnimationGroup(self)
        self._animation.addAnimation(geo)
        self._animation.start()

    def open_first(self) -> None:
        # Pick the primary partition of the first device group.
        invalidate_cache()
        groups = group_volumes_by_physical_device()
        primary_path = ""
        if groups:
            primary_path = groups[0].primary_path
        if primary_path:
            self.open_and_hide(primary_path)
        else:
            self.refresh()

    def open_and_hide(self, path: str) -> None:
        self.hide_timer.stop()
        try:
            open_path(path)
        except Exception as exc:
            log_error("open_path_failed", {"path": path, "message": str(exc)}, exc_info=True)
            self.queue_event(UsbEvent(action="error", details={"kind": "error", "message": f"打开失败：{exc}"}))
            return
        remember_recent_volume(self.app_config, self.config_store, path, opened=True)
        log_action("open_path", {"path": path, "path_hash": hash_id(path)})
        self.animate_hide()
