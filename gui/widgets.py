"""Volume row widget displayed inside the toast."""

from __future__ import annotations

from typing import Callable

from PySide6.QtCore import QPoint, Qt
from PySide6.QtGui import QAction
from PySide6.QtWidgets import (
    QFrame,
    QGridLayout,
    QHBoxLayout,
    QLabel,
    QMenu,
    QPushButton,
    QProgressBar,
    QSizePolicy,
)

from ..events import VolumeInfo
from ..windows_helpers import format_bytes, usage_percent
from .theme import IconFactory, Theme


class VolumeRow(QFrame):
    def __init__(
        self,
        info: VolumeInfo,
        opener: Callable[[str], None],
        revealer: Callable[[str], None],
        copier: Callable[[str], None],
        ejecter: Callable[[str], None],
        theme: Theme,
        icons: IconFactory,
    ) -> None:
        super().__init__()
        self.info = info
        self.opener = opener
        self.revealer = revealer
        self.copier = copier
        self.ejecter = ejecter
        self.setObjectName("volumeRow")
        self.setSizePolicy(QSizePolicy.Expanding, QSizePolicy.Fixed)
        px = theme.px
        self.setMinimumHeight(px(94))
        self.setMaximumHeight(px(101))
        self.setContextMenuPolicy(Qt.CustomContextMenu)
        self.customContextMenuRequested.connect(self.show_context_menu)
        grid = QGridLayout(self)
        grid.setContentsMargins(px(12), px(9), px(12), px(9))
        grid.setHorizontalSpacing(px(10))
        grid.setVerticalSpacing(px(4))

        icon = QLabel()
        icon.setObjectName("driveIcon")
        icon.setAlignment(Qt.AlignCenter)
        icon.setFixedSize(px(34), px(34))
        icon.setPixmap(icons.pixmap("drive", theme, px(31)))
        grid.addWidget(icon, 0, 0, 2, 1)

        title = QLabel(info.title)
        title.setObjectName("rowTitle")
        title.setWordWrap(False)
        title.setToolTip(info.title)
        title.setSizePolicy(QSizePolicy.Expanding, QSizePolicy.Preferred)
        grid.addWidget(title, 0, 1, 1, 1)

        # Second line: bus type + drive type + path + (optional) physical disk
        meta_bits = [info.drive_type]
        if info.bus_type and info.bus_type != "unknown":
            meta_bits.append(info.bus_type.upper())
        if info.physical_disk is not None:
            meta_bits.append(f"物理盘 {info.physical_disk}")
        meta_text = " · ".join(meta_bits) + f" · {info.path}"
        path_label = QLabel(meta_text)
        path_label.setObjectName("pathLabel")
        path_label.setToolTip(info.path)
        path_label.setTextInteractionFlags(Qt.TextSelectableByMouse)
        grid.addWidget(path_label, 1, 1, 1, 1)

        button = QPushButton("打开")
        button.setObjectName("openButton")
        button.setMinimumWidth(px(88))
        button.setMinimumHeight(px(42))
        button.clicked.connect(lambda: opener(info.path))
        grid.addWidget(button, 0, 2, 2, 1)

        capacity = QLabel(f"容量 {format_bytes(info.total)} · 可用 {format_bytes(info.free)}")
        capacity.setObjectName("capacityLabel")
        grid.addWidget(capacity, 2, 0, 1, 3)

        progress = QProgressBar()
        progress.setObjectName("usage")
        progress.setTextVisible(False)
        progress.setFixedHeight(px(8))
        percent = usage_percent(info.total, info.used)
        if percent is None:
            progress.setRange(0, 0)
        else:
            progress.setRange(0, 100)
            progress.setValue(percent)
        grid.addWidget(progress, 3, 0, 1, 3)

    def show_context_menu(self, pos: QPoint) -> None:
        menu = QMenu(self)
        open_action = QAction("打开", menu)
        open_action.triggered.connect(lambda: self.opener(self.info.path))
        menu.addAction(open_action)

        reveal_action = QAction("在资源管理器中显示", menu)
        reveal_action.triggered.connect(lambda: self.revealer(self.info.path))
        menu.addAction(reveal_action)

        copy_action = QAction("复制路径", menu)
        copy_action.triggered.connect(lambda: self.copier(self.info.path))
        menu.addAction(copy_action)

        menu.addSeparator()
        eject_action = QAction("安全弹出", menu)
        eject_action.triggered.connect(lambda: self.ejecter(self.info.path))
        menu.addAction(eject_action)
        menu.exec(self.mapToGlobal(pos))
