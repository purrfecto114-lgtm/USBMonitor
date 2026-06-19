"""Theme + SVG icon factory."""

from __future__ import annotations

from typing import Callable

from PySide6.QtCore import QByteArray, QRectF
from PySide6.QtGui import QColor, QIcon, QPainter, QPixmap, QPalette
from PySide6.QtSvg import QSvgRenderer
from PySide6.QtWidgets import QApplication


def make_px(scale: float) -> Callable[[float], int]:
    def px(value: float) -> int:
        return max(1, int(round(value * scale)))
    return px


class Theme:
    def __init__(self, name: str, app: QApplication, px: Callable[[float], int]) -> None:
        requested = name
        if name == "auto":
            bg = app.palette().color(QPalette.Window)
            name = "dark" if bg.lightness() < 128 else "light"
        self.requested = requested
        self.name = name
        self.px = px
        self.border_radius = px(14)
        self.row_radius = px(11)
        self.button_radius = px(9)
        if name == "light":
            self.bg = "rgba(0,0,0,0)"
            self.panel = "#fbfcff"
            self.panel2 = "#f3f6fb"
            self.text = "#111827"
            self.muted = "#687386"
            self.border = "#dbe3ef"
            self.accent = "#1769e0"
            self.accent_hover = "#0f5ed2"
            self.progress_bg = "#e6ecf5"
            self.icon_shell = "#ffffff"
            self.icon_socket = "#dce6f4"
            self.icon_line = "#1769e0"
            self.icon_shadow = "#a9b7c8"
            self.shadow_color = QColor(15, 23, 42, 55)
        else:
            self.bg = "rgba(0,0,0,0)"
            self.panel = "#1f2530"
            self.panel2 = "#29313d"
            self.text = "#f6f8fc"
            self.muted = "#b6c0cf"
            self.border = "#3a4656"
            self.accent = "#6ea2ff"
            self.accent_hover = "#8ab6ff"
            self.progress_bg = "#3a4656"
            self.icon_shell = "#2d3746"
            self.icon_socket = "#3d4a5c"
            self.icon_line = "#8ab6ff"
            self.icon_shadow = "#111827"
            self.shadow_color = QColor(0, 0, 0, 145)
        self.ok = "#34c759"
        self.warn = "#ffb020"
        self.err = "#ff5c5c"

    def style_sheet(self) -> str:
        px = self.px
        return f"""
        QWidget {{
            background: {self.bg};
            color: {self.text};
            font-family: "Segoe UI", "Microsoft YaHei UI", Arial, sans-serif;
            font-size: {px(13)}px;
        }}
        QFrame#root {{
            background: {self.panel};
            border: 1px solid {self.border};
            border-radius: {self.border_radius}px;
        }}
        QLabel {{ background: transparent; color: {self.text}; }}
        QLabel#headline {{ font-size: {px(16)}px; font-weight: 700; }}
        QLabel#muted, QLabel#summary, QLabel#count, QLabel#pathLabel, QLabel#capacityLabel {{
            color: {self.muted}; font-size: {px(12)}px;
        }}
        QLabel#appIcon, QLabel#driveIcon {{ background: transparent; }}
        QFrame#volumeRow {{
            background: {self.panel2};
            border: 1px solid {self.border};
            border-radius: {self.row_radius}px;
            margin: 1px 0px;
        }}
        QLabel#rowTitle {{
            color: {self.text}; font-size: {px(13)}px; font-weight: 650; background: transparent;
        }}
        QPushButton {{
            background: transparent;
            color: {self.text};
            border: 1px solid {self.border};
            border-radius: {self.button_radius}px;
            padding: {px(8)}px {px(15)}px;
            font-weight: 650;
        }}
        QPushButton:hover {{ background: {self.panel2}; }}
        QPushButton#primaryButton, QPushButton#openButton {{
            background: {self.accent}; color: white; border-color: {self.accent};
        }}
        QPushButton#primaryButton:hover, QPushButton#openButton:hover {{
            background: {self.accent_hover}; border-color: {self.accent_hover};
        }}
        QScrollArea {{ background: transparent; border: 0; }}
        QScrollArea > QWidget > QWidget {{ background: transparent; }}
        QScrollBar:vertical {{ background: transparent; width: {px(8)}px; margin: 1px; }}
        QScrollBar::handle:vertical {{ background: {self.border}; border-radius: {px(4)}px; }}
        QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical {{ height: 0; }}
        QProgressBar#usage {{ background: {self.progress_bg}; border: 0; border-radius: {px(4)}px; }}
        QProgressBar#usage::chunk {{ background: {self.accent}; border-radius: {px(4)}px; }}
        QMenu {{
            background: {self.panel};
            border: 1px solid {self.border};
            border-radius: {self.button_radius}px;
            padding: {px(4)}px;
            color: {self.text};
        }}
        QMenu::item {{
            background: transparent;
            padding: {px(6)}px {px(18)}px {px(6)}px {px(14)}px;
            border-radius: {px(5)}px;
            min-width: {px(180)}px;
        }}
        QMenu::item:selected {{ background: {self.accent}; color: white; }}
        QMenu::separator {{
            height: 1px; background: {self.border}; margin: {px(3)}px {px(8)}px;
        }}
        QMenu::indicator {{
            width: {px(14)}px; height: {px(14)}px;
            margin-left: {px(4)}px;
        }}
        """


def usb_svg(theme: Theme, kind: str) -> str:
    status = {
        "usb": theme.accent,
        "drive": theme.accent,
        "add": theme.ok,
        "change": theme.warn,
        "remove": theme.err,
        "error": theme.err,
    }.get(kind, theme.accent)
    mark = ""
    if kind in {"add", "change", "remove", "error"}:
        if kind == "add":
            mark = '<path d="M44 45l4 4 8-10" fill="none" stroke="white" stroke-width="3.1" stroke-linecap="round" stroke-linejoin="round"/>'
        elif kind == "change":
            mark = '<path d="M43 45h11M50 39l6 6-6 6" fill="none" stroke="white" stroke-width="3" stroke-linecap="round" stroke-linejoin="round"/>'
        else:
            mark = '<path d="M44 40l10 10M54 40L44 50" fill="none" stroke="white" stroke-width="3" stroke-linecap="round"/>'
    return f'''
    <svg xmlns="http://www.w3.org/2000/svg" width="64" height="64" viewBox="0 0 64 64">
      <defs>
        <linearGradient id="g" x1="12" y1="10" x2="52" y2="58" gradientUnits="userSpaceOnUse">
          <stop offset="0" stop-color="{theme.icon_shell}"/>
          <stop offset="1" stop-color="{theme.panel2}"/>
        </linearGradient>
      </defs>
      <rect x="19" y="5" width="26" height="18" rx="5" fill="{theme.icon_socket}" stroke="{theme.border}" stroke-width="2"/>
      <rect x="24" y="9" width="5" height="7" rx="1.5" fill="{theme.icon_line}" opacity="0.95"/>
      <rect x="35" y="9" width="5" height="7" rx="1.5" fill="{theme.icon_line}" opacity="0.95"/>
      <rect x="13" y="20" width="38" height="35" rx="10" fill="url(#g)" stroke="{theme.border}" stroke-width="2"/>
      <path d="M32 26v16M24 34h16M24 34l-5-5M40 34l5-5" fill="none" stroke="{theme.icon_line}" stroke-width="3" stroke-linecap="round" stroke-linejoin="round"/>
      <rect x="20" y="48" width="24" height="3" rx="1.5" fill="{theme.icon_shadow}" opacity="0.35"/>
      <circle cx="50" cy="45" r="11" fill="{status}" stroke="{theme.panel}" stroke-width="3"/>
      {mark}
    </svg>
    '''


class IconFactory:
    def __init__(self, app: QApplication, px: Callable[[float], int]) -> None:
        self.app = app
        self.px = px
        self.cache: dict[tuple[str, str, int, int], QPixmap] = {}

    def dpr(self) -> float:
        screen = self.app.primaryScreen()
        return float(screen.devicePixelRatio() if screen is not None else 1.0) or 1.0

    def pixmap(self, kind: str, theme: Theme, logical_size: int) -> QPixmap:
        from PySide6.QtCore import Qt
        ratio = max(1.0, self.dpr())
        physical_size = max(1, int(round(logical_size * ratio)))
        key = (theme.name, kind, int(logical_size), physical_size)
        if key in self.cache:
            return self.cache[key]
        pixmap = QPixmap(physical_size, physical_size)
        pixmap.setDevicePixelRatio(ratio)
        pixmap.fill(Qt.transparent)
        renderer = QSvgRenderer(QByteArray(usb_svg(theme, kind).encode("utf-8")))
        painter = QPainter(pixmap)
        painter.setRenderHint(QPainter.Antialiasing, True)
        painter.setRenderHint(QPainter.SmoothPixmapTransform, True)
        renderer.render(painter, QRectF(0, 0, logical_size, logical_size))
        painter.end()
        self.cache[key] = pixmap
        return pixmap

    def app_icon(self, theme: Theme) -> QIcon:
        icon = QIcon()
        for size in (16, 20, 24, 32, 48, 64, 96, 128, 256):
            icon.addPixmap(self.pixmap("usb", theme, size))
        return icon
