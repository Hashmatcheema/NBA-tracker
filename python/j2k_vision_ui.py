"""
J2K Vision — PyQt6 panel styled like the NBA Vision reference (purple / charcoal, white slider thumbs).

Tuning is **PyQt only**. ``python run_cpp.py`` opens the panel. When Helios constructs ``GCVWorker``,
``run_cpp`` spawns a **child Python process** for the panel (Qt on a background thread is unreliable on
Windows). Set ``J2K_NO_AUTO_UI=1`` to disable auto-launch.
"""
from __future__ import annotations

import json
import os
import sys
import ctypes
from typing import Any

_SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
_CFG_NAME = "j2k_config.json"
_DEFAULT_PROFILE = "1"

# Set True while ``run_app()`` owns QApplication in this process (suppresses Helios child panel).
_standalone_cli_active = False

# NBA-style reference palette (branding: J2K VISION)
C_BG = "#0d0d12"
C_SIDEBAR = "#0a0a10"
C_CARD = "#16161f"
C_TEXT = "#fafafa"
C_MUTED = "#a1a1aa"
C_ACCENT = "#8a2be2"
C_ACCENT_DIM = "#6d28d9"
C_ACTIVE = "#34d399"
C_OFFLINE = "#f87171"
C_WARN = "#facc15"

# Horizontal slider: thin track, purple fill, white circular thumb (Fusion-friendly)
K_SLIDER_QSS = """
QSlider#visionSlider::groove:horizontal {
    height: 4px;
    background: #2a2540;
    border-radius: 2px;
    margin: 0 6px;
}
QSlider#visionSlider::sub-page:horizontal {
    background: #8a2be2;
    border-radius: 2px;
}
QSlider#visionSlider::add-page:horizontal {
    background: #2a2540;
    border-radius: 2px;
}
QSlider#visionSlider::handle:horizontal {
    background: #ffffff;
    border: none;
    width: 16px;
    height: 16px;
    margin: -7px 0;
    border-radius: 8px;
}
"""


def _config_path() -> str:
    return os.path.join(_SCRIPT_DIR, _CFG_NAME)


def _vision_slider(orientation) -> Any:
    from PyQt6.QtWidgets import QSlider

    s = QSlider(orientation)
    s.setObjectName("visionSlider")
    s.setStyleSheet(K_SLIDER_QSS)
    s.setMinimumHeight(32)
    return s


def _merge_root_cue_tempo(path: str, release_cue: float, tempo_speed: float) -> bool:
    release_cue = max(0.0, min(100.0, float(release_cue)))
    tempo_speed = max(0.0, min(100.0, float(tempo_speed)))
    try:
        if os.path.isfile(path):
            with open(path, encoding="utf-8") as f:
                body = f.read()
        else:
            body = "{\n}\n"
    except OSError:
        return False

    import re

    def replace_key(s: str, key: str, val: float) -> tuple[str, bool]:
        pat = re.compile(rf'("{re.escape(key)}"\s*:\s*)-?[0-9]+\.?[0-9]*')
        if pat.search(s):
            return pat.sub(rf"\g<1>{val:.2f}", s, count=1), True
        return s, False

    b, ok_rc = replace_key(body, "release_cue_value", release_cue)
    if not ok_rc:
        b = body.rstrip()
        if b.endswith("}"):
            ins = f',\n  "release_cue_value": {release_cue:.2f}'
            b = b[:-1] + ins + "\n}\n"
        else:
            b = body + f'\n  "release_cue_value": {release_cue:.2f}\n'
    body = b
    b, ok_ts = replace_key(body, "tempo_speed", tempo_speed)
    if not ok_ts:
        b = body.rstrip()
        if b.endswith("}"):
            ins = f',\n  "tempo_speed": {tempo_speed:.2f}'
            b = b[:-1] + ins + "\n}\n"
        else:
            b = body + f'\n  "tempo_speed": {tempo_speed:.2f}\n'
    body = b

    tmp = path + ".tmp"
    try:
        with open(tmp, "w", encoding="utf-8", newline="\n") as out:
            out.write(body)
        os.replace(tmp, path)
    except OSError:
        try:
            if os.path.isfile(tmp):
                os.remove(tmp)
        except OSError:
            pass
        return False
    return True


def _read_cue_tempo_from_text(text: str) -> tuple[float, float]:
    try:
        data = json.loads(text)
    except json.JSONDecodeError:
        return 50.0, 35.0
    rc = data.get("release_cue_value")
    ts = data.get("tempo_speed")
    profs = data.get("profiles") or {}
    p1 = profs.get(_DEFAULT_PROFILE) or profs.get("1") or {}
    if rc is None:
        rc = p1.get("manual_shot_normal_hold")
    if ts is None:
        ts = p1.get("manual_shot_normal_tempo")
    try:
        rc_f = float(rc) if rc is not None else 50.0
    except (TypeError, ValueError):
        rc_f = 50.0
    try:
        ts_f = float(ts) if ts is not None else 35.0
    except (TypeError, ValueError):
        ts_f = 35.0
    return max(0.0, min(100.0, rc_f)), max(0.0, min(100.0, ts_f))


def load_config_dict(path: str) -> dict[str, Any]:
    if not os.path.isfile(path):
        return {}
    with open(path, encoding="utf-8") as f:
        return json.load(f)


def save_config_dict(path: str, data: dict[str, Any]) -> None:
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8", newline="\n") as f:
        json.dump(data, f, indent=2)
        f.write("\n")
    os.replace(tmp, path)


def collect_readiness_issues() -> list[str]:
    issues: list[str] = []
    cfg = _config_path()
    if not os.path.isfile(cfg):
        issues.append(f"Missing {_CFG_NAME}")
        return issues

    try:
        with open(cfg, encoding="utf-8") as f:
            raw = f.read()
        data = json.loads(raw)
    except (OSError, json.JSONDecodeError) as e:
        issues.append(f"Invalid {_CFG_NAME}: {e}")
        return issues

    rel_onnx = (data.get("yolo_onnx") or "").strip()
    if not rel_onnx:
        issues.append("Config missing yolo_onnx path")
    else:
        onnx_abs = os.path.normpath(os.path.join(_SCRIPT_DIR, rel_onnx.replace("/", os.sep)))
        if not os.path.isfile(onnx_abs):
            issues.append(f"YOLO weights not found: {rel_onnx}")

    dll_name = (os.environ.get("J2K_GCV_DLL_NAME") or "j2k_ch.dll").strip() or "j2k_ch.dll"
    env_dll = (os.environ.get("J2K_GCV_DLL") or "").strip()
    dll_abs = os.path.normpath(env_dll) if env_dll else os.path.join(_SCRIPT_DIR, "bin", dll_name)
    if not os.path.isfile(dll_abs):
        issues.append(f"Native DLL missing: {dll_abs}")

    return issues


def _latency_label_ms(ms: int) -> str:
    if ms <= 25:
        return "LOW"
    if ms <= 55:
        return "MEDIUM"
    return "HIGH"


class J2KVisionWindow:
    def __init__(self, helios_embedded: bool = False) -> None:
        from PyQt6.QtCore import Qt, QTimer
        from PyQt6.QtWidgets import (
            QFrame,
            QHBoxLayout,
            QLabel,
            QMainWindow,
            QPushButton,
            QStackedWidget,
            QVBoxLayout,
            QWidget,
        )

        self._Qt = Qt
        self._worker = None
        self._page = 0
        self._helios_embedded = helios_embedded

        self.win = QMainWindow()
        self.win.setWindowTitle("J2K Vision")
        self.win.setMinimumSize(1080, 700)

        self.win.setStyleSheet(
            f"""
            QMainWindow {{ background-color: {C_BG}; }}
            QWidget {{ color: {C_TEXT}; font-size: 13px; font-family: 'Segoe UI', 'Inter', sans-serif; }}
            QLabel#brandTitle {{ font-size: 20px; font-weight: 800; color: {C_TEXT}; letter-spacing: 0.04em; }}
            QLabel#brandSub {{ font-size: 12px; color: {C_ACCENT}; font-weight: 600; }}
            QLabel#sectionTitle {{ font-size: 11px; font-weight: 700; color: {C_ACCENT}; letter-spacing: 0.12em; }}
            QLabel#statusPill {{
                padding: 10px 16px; border-radius: 20px; font-weight: 700; font-size: 12px;
            }}
            QFrame#card {{
                background-color: {C_CARD}; border-radius: 14px; border: 1px solid #2a2a36;
            }}
            QPushButton#sideNav {{
                text-align: left; padding: 12px 14px; border: 1px solid transparent;
                border-radius: 10px; color: {C_MUTED}; background: transparent; font-weight: 600;
            }}
            QPushButton#sideNav:hover {{ background-color: #1a1428; color: {C_TEXT}; }}
            QPushButton#sideNav:checked {{
                border: 1px solid {C_ACCENT}; background-color: #1a1428; color: {C_TEXT};
            }}
            QPushButton#topTab {{
                padding: 10px 20px; border-radius: 10px; border: 1px solid #3f3f4a;
                color: {C_TEXT}; background: transparent; font-weight: 600; font-size: 11px;
            }}
            QPushButton#topTab:checked {{
                background-color: {C_ACCENT}; border: 1px solid {C_ACCENT}; color: white;
                font-weight: 700;
            }}
            QPushButton#topTab:hover:!checked {{ background-color: #1f1f2e; }}
            QPushButton#primary {{
                background: qlineargradient(x1:0, y1:0, x2:1, y2:1, stop:0 {C_ACCENT}, stop:1 {C_ACCENT_DIM});
                color: white; padding: 12px 18px; border-radius: 12px; font-weight: 700; border: none;
            }}
            QPushButton#primary:hover {{ background: {C_ACCENT_DIM}; }}
            QPushButton#ghost {{
                background-color: #252530; color: {C_TEXT}; padding: 10px 16px;
                border-radius: 10px; border: 1px solid #3f3f46; font-weight: 600;
            }}
            QCheckBox#nbaToggle {{ spacing: 14px; color: {C_MUTED}; font-size: 12px; }}
            QCheckBox#nbaToggle::indicator {{
                width: 48px; height: 26px; border-radius: 13px;
            }}
            QCheckBox#nbaToggle::indicator:unchecked {{
                background: #3f3f46; border: 2px solid #52525b;
            }}
            QCheckBox#nbaToggle::indicator:checked {{
                background: {C_ACCENT}; border: 2px solid #c4b5fd;
            }}
            """
        )

        central = QWidget()
        self.win.setCentralWidget(central)
        root = QHBoxLayout(central)
        root.setContentsMargins(0, 0, 0, 0)
        root.setSpacing(0)

        # --- Sidebar ---
        side = QWidget()
        side.setFixedWidth(280)
        side.setStyleSheet(f"background-color: {C_SIDEBAR}; border-right: 1px solid #1f1f2e;")
        sv = QVBoxLayout(side)
        sv.setContentsMargins(22, 28, 18, 24)
        sv.setSpacing(14)

        icon = QLabel("⬡")
        icon.setFixedSize(52, 52)
        icon.setAlignment(Qt.AlignmentFlag.AlignCenter)
        icon.setStyleSheet(
            "background-color: #1f1f2e; border-radius: 14px; border: 1px solid #2e2e3e; "
            "font-size: 20px; color: #71717a;"
        )
        sv.addWidget(icon)

        brand_t = QLabel("J2K VISION")
        brand_t.setObjectName("brandTitle")
        brand_s = QLabel("AUTO GREEN · GCV")
        brand_s.setObjectName("brandSub")
        sv.addWidget(brand_t)
        sv.addWidget(brand_s)

        lic = QFrame()
        lic.setObjectName("card")
        lic.setStyleSheet(
            f"QFrame#card {{ background-color: {C_CARD}; border-radius: 14px; "
            f"border: 1px solid #2a2a36; padding: 4px; }}"
        )
        lv = QVBoxLayout(lic)
        lv.addWidget(QLabel("LICENSE REMAINING"))
        lic_big = QLabel("—")
        lic_big.setStyleSheet("font-size: 28px; font-weight: 800; color: #fafafa;")
        lv.addWidget(lic_big)
        lic_note = QLabel("License / expiration are managed in the host app (Helios).")
        lic_note.setWordWrap(True)
        lic_note.setStyleSheet(f"color: {C_MUTED}; font-size: 11px;")
        lv.addWidget(lic_note)
        sv.addWidget(lic)

        self._pill = QLabel("● OFFLINE")
        self._pill.setObjectName("statusPill")
        self._pill.setAlignment(Qt.AlignmentFlag.AlignCenter)
        sv.addWidget(self._pill)

        self._detail = QLabel("")
        self._detail.setWordWrap(True)
        self._detail.setStyleSheet(f"color: {C_MUTED}; font-size: 11px;")
        sv.addWidget(self._detail)

        nav_labels = (
            ("⚙  SETTINGS", 0),
            ("⚡  STAMINA", 1),
            ("🔒  CONFIGS", 2),
            ("🕐  HISTORY", 3),
        )
        self._nav_buttons: list[QPushButton] = []
        for text, _i in nav_labels:
            b = QPushButton(text)
            b.setObjectName("sideNav")
            b.setCheckable(True)
            self._nav_buttons.append(b)
            sv.addWidget(b)
        self._nav_buttons[0].setChecked(True)

        sv.addStretch()
        self._btn_start = QPushButton("Start scripting")
        self._btn_start.setObjectName("primary")
        self._btn_stop = QPushButton("Stop scripting")
        self._btn_stop.setObjectName("ghost")
        self._btn_stop.setEnabled(False)
        sv.addWidget(self._btn_start)
        sv.addWidget(self._btn_stop)
        if helios_embedded:
            self._btn_start.hide()
            self._btn_stop.hide()

        root.addWidget(side)

        # --- Content ---
        content = QWidget()
        cv = QVBoxLayout(content)
        cv.setContentsMargins(24, 22, 28, 28)
        cv.setSpacing(18)

        tabs_row = QHBoxLayout()
        tabs_row.setSpacing(10)
        top_names = ["SETTINGS", "STAMINA", "CONFIGS", "HISTORY"]
        self._top_tab_buttons: list[QPushButton] = []
        for name in top_names:
            b = QPushButton(name)
            b.setObjectName("topTab")
            b.setCheckable(True)
            self._top_tab_buttons.append(b)
            tabs_row.addWidget(b)
        self._top_tab_buttons[0].setChecked(True)
        tabs_row.addStretch()
        cv.addLayout(tabs_row)

        self._stack = QStackedWidget()
        cv.addWidget(self._stack, 1)

        settings = QWidget()
        sg = QVBoxLayout(settings)
        sg.setSpacing(18)

        row1 = QHBoxLayout()
        row1.setSpacing(18)
        row1.addWidget(self._make_shot_card(), 1)
        row1.addWidget(self._make_online_card(), 1)
        sg.addLayout(row1)
        sg.addWidget(self._make_timing_card())

        self._stack.addWidget(settings)

        for title in ("Stamina", "Configs", "History"):
            p = QWidget()
            pl = QVBoxLayout(p)
            pl.addWidget(
                QLabel(f"{title} — configure via j2k_config.json in this build."),
                alignment=Qt.AlignmentFlag.AlignTop,
            )
            pl.addStretch()
            self._stack.addWidget(p)

        root.addWidget(content, 1)

        for i, b in enumerate(self._nav_buttons):
            b.clicked.connect(lambda _c, idx=i: self._select_page(idx))
        for i, b in enumerate(self._top_tab_buttons):
            b.clicked.connect(lambda _c, idx=i: self._select_page(idx))

        self._btn_start.clicked.connect(self._on_start)
        self._btn_stop.clicked.connect(self._on_stop)

        self._timer = QTimer(self.win)
        self._timer.setInterval(1500)
        self._timer.timeout.connect(self._refresh_status)
        self._timer.start()

        self._load_sliders_from_config()
        self._refresh_status()

    def _section_title(self, text: str) -> Any:
        from PyQt6.QtWidgets import QLabel

        lab = QLabel(text)
        lab.setObjectName("sectionTitle")
        return lab

    def _value_pill(self) -> Any:
        from PyQt6.QtWidgets import QLabel

        p = QLabel("—")
        p.setStyleSheet(
            f"background-color: #2e1065; color: {C_TEXT}; padding: 8px 16px; "
            "border-radius: 12px; font-weight: 700; font-size: 13px; min-width: 72px;"
        )
        p.setAlignment(self._Qt.AlignmentFlag.AlignCenter)
        return p

    def _make_shot_card(self) -> Any:
        from PyQt6.QtWidgets import QCheckBox, QFrame, QLabel, QVBoxLayout

        card = QFrame()
        card.setObjectName("card")
        v = QVBoxLayout(card)
        v.setSpacing(14)
        v.addWidget(self._section_title("• SHOT MODE"))

        self._cb_button = QCheckBox("BUTTON\nTimed meter release")
        self._cb_button.setObjectName("nbaToggle")
        self._cb_rhythm = QCheckBox("RHYTHM\nLB / L1 flick · Auto detect [Listening...]")
        self._cb_rhythm.setObjectName("nbaToggle")
        self._cb_auto = QCheckBox("AUTO LEARN")
        self._cb_auto.setObjectName("nbaToggle")

        listen = QFrame()
        listen.setStyleSheet(
            f"border: 1px solid {C_ACCENT}; border-radius: 12px; background-color: #12121a; "
            "padding: 12px;"
        )
        lh = QVBoxLayout(listen)
        listen_l = QLabel("PRESS ANY BUTTON ON YOUR CONTROLLER...")
        listen_l.setWordWrap(True)
        listen_l.setStyleSheet(f"color: {C_ACCENT}; font-weight: 700; font-size: 12px; border: none;")
        lh.addWidget(listen_l)
        v.addWidget(self._cb_button)
        v.addWidget(self._cb_rhythm)
        v.addWidget(listen)
        v.addWidget(self._cb_auto)
        v.addStretch()

        self._cb_button.toggled.connect(self._on_toggle_shot_mode)
        self._cb_rhythm.toggled.connect(self._on_toggle_rhythm)
        self._cb_auto.toggled.connect(self._on_toggle_autolearn)
        return card

    def _make_online_card(self) -> Any:
        from PyQt6.QtWidgets import QFrame, QHBoxLayout, QLabel, QVBoxLayout

        card = QFrame()
        card.setObjectName("card")
        v = QVBoxLayout(card)
        v.setSpacing(12)
        v.addWidget(self._section_title("• ONLINE"))

        self._lat_big = QLabel("— ms")
        self._lat_big.setStyleSheet("font-size: 40px; font-weight: 800; letter-spacing: -0.02em;")
        v.addWidget(self._lat_big)

        self._lat_band = QLabel("—")
        self._lat_band.setStyleSheet(
            f"color: {C_BG}; background-color: {C_WARN}; font-size: 11px; font-weight: 700; "
            "padding: 4px 12px; border-radius: 10px; max-width: 120px;"
        )
        v.addWidget(self._lat_band)

        v.addWidget(QLabel("LATENCY COMP"))
        self._sl_latency = _vision_slider(self._Qt.Orientation.Horizontal)
        self._sl_latency.setRange(0, 80)
        self._sl_latency.valueChanged.connect(self._on_latency_changed)

        ends = QHBoxLayout()
        ends.addWidget(QLabel("0ms"))
        ends.addStretch()
        ends.addWidget(QLabel("80ms"))
        for i in range(ends.count()):
            w = ends.itemAt(i).widget()
            if isinstance(w, QLabel):
                w.setStyleSheet(f"color: {C_MUTED}; font-size: 11px;")

        v.addWidget(self._sl_latency)
        v.addLayout(ends)
        v.addStretch()
        return card

    def _make_timing_card(self) -> Any:
        from PyQt6.QtWidgets import QFrame, QHBoxLayout, QLabel, QPushButton, QVBoxLayout

        card = QFrame()
        card.setObjectName("card")
        v = QVBoxLayout(card)
        v.setSpacing(16)

        head = QHBoxLayout()
        head.addWidget(self._section_title("• TIMING"))
        head.addStretch()
        btn_reset = QPushButton("RESET SESSION")
        btn_reset.setObjectName("ghost")
        btn_reset.clicked.connect(self._load_sliders_from_config)
        head.addWidget(btn_reset)
        v.addLayout(head)

        # Release delay row
        row_r = QHBoxLayout()
        row_r.addWidget(QLabel("RELEASE DELAY"))
        row_r.addStretch()
        self._pill_release = self._value_pill()
        row_r.addWidget(self._pill_release)
        v.addLayout(row_r)

        self._sl_rc = _vision_slider(self._Qt.Orientation.Horizontal)
        self._sl_rc.setRange(0, 100)
        self._sl_rc.valueChanged.connect(self._on_rc_changed)
        v.addWidget(self._sl_rc)
        r_ends = QHBoxLayout()
        r_lo = QLabel("25ms faster")
        r_hi = QLabel("100ms slower")
        for lb in (r_lo, r_hi):
            lb.setStyleSheet(f"color: {C_MUTED}; font-size: 11px;")
        r_ends.addWidget(r_lo)
        r_ends.addStretch()
        r_ends.addWidget(r_hi)
        v.addLayout(r_ends)
        self._lbl_rc_detail = QLabel("")
        self._lbl_rc_detail.setWordWrap(True)
        self._lbl_rc_detail.setStyleSheet(f"color: {C_MUTED}; font-size: 11px;")
        v.addWidget(self._lbl_rc_detail)

        # Flick row
        row_f = QHBoxLayout()
        row_f.addWidget(QLabel("FLICK SPEED"))
        row_f.addStretch()
        self._pill_flick = self._value_pill()
        row_f.addWidget(self._pill_flick)
        v.addLayout(row_f)

        self._sl_ts = _vision_slider(self._Qt.Orientation.Horizontal)
        self._sl_ts.setRange(0, 100)
        self._sl_ts.valueChanged.connect(self._on_ts_changed)
        v.addWidget(self._sl_ts)
        self._lbl_ts_detail = QLabel("")
        self._lbl_ts_detail.setWordWrap(True)
        self._lbl_ts_detail.setStyleSheet(f"color: {C_MUTED}; font-size: 11px;")
        v.addWidget(self._lbl_ts_detail)

        v.addStretch()
        return card

    def _select_page(self, idx: int) -> None:
        self._page = idx
        self._stack.setCurrentIndex(idx)
        for i, b in enumerate(self._nav_buttons):
            b.blockSignals(True)
            b.setChecked(i == idx)
            b.blockSignals(False)
        for i, b in enumerate(self._top_tab_buttons):
            b.blockSignals(True)
            b.setChecked(i == idx)
            b.blockSignals(False)

    def _load_sliders_from_config(self) -> None:
        path = _config_path()
        if not os.path.isfile(path):
            return
        try:
            with open(path, encoding="utf-8") as f:
                text = f.read()
            rc, ts = _read_cue_tempo_from_text(text)
            data = json.loads(text)
        except (OSError, json.JSONDecodeError):
            return
        prof = (data.get("profiles") or {}).get(_DEFAULT_PROFILE, {})
        lat = int(prof.get("nm_server_delay_ms") or 0)
        lat = max(0, min(80, lat))

        self._sl_rc.blockSignals(True)
        self._sl_ts.blockSignals(True)
        self._sl_latency.blockSignals(True)
        self._sl_rc.setValue(int(round(rc)))
        self._sl_ts.setValue(int(round(ts)))
        self._sl_latency.setValue(lat)
        self._sl_rc.blockSignals(False)
        self._sl_ts.blockSignals(False)
        self._sl_latency.blockSignals(False)

        tm = (prof.get("timing_mode") or "animation") == "meter"
        self._cb_button.blockSignals(True)
        self._cb_button.setChecked(tm)
        self._cb_button.blockSignals(False)

        self._cb_rhythm.blockSignals(True)
        self._cb_rhythm.setChecked(bool(prof.get("manual_tuning_enabled")))
        self._cb_rhythm.blockSignals(False)

        self._cb_auto.blockSignals(True)
        self._cb_auto.setChecked(bool(prof.get("timing_predictor_enabled")))
        self._cb_auto.blockSignals(False)

        self._update_rc_label()
        self._update_ts_label()
        self._update_latency_ui(lat)

    def _release_display_ms(self, rc: int) -> int:
        return 25 + int(round(75 * rc / 100.0))

    def _update_rc_label(self) -> None:
        rc = self._sl_rc.value()
        self._pill_release.setText(f"{self._release_display_ms(rc)}ms")
        self._lbl_rc_detail.setText(
            f"release_cue_value: {rc} - visual / fire-zone height (0-100). DLL reads j2k_config.json."
        )

    def _update_ts_label(self) -> None:
        v = self._sl_ts.value()
        # Keep UI units aligned with native DLL mapping (tempo_speed -> 20..120 ms flick hold).
        flick_ms = int(round(20 + (v / 100.0) * 100))
        self._pill_flick.setText(f"{flick_ms}ms")
        self._lbl_ts_detail.setText(
            f"tempo_speed: {v} - flick duration {flick_ms}ms (0 = fastest .. 100 = slowest). "
            "Release cue drives native fire-zone vertical placement. "
            "Tempo maps to flick length inside j2k_ch.dll."
        )

    def _update_latency_ui(self, ms: int) -> None:
        self._lat_big.setText(f"{ms} ms")
        band = _latency_label_ms(ms)
        self._lat_band.setText(band)
        if band == "MEDIUM":
            bg = C_WARN
            fg = C_BG
        elif band == "HIGH":
            bg = "#fb923c"
            fg = C_BG
        else:
            bg = C_ACTIVE
            fg = "#052e1a"
        self._lat_band.setStyleSheet(
            f"color: {fg}; background-color: {bg}; font-size: 11px; font-weight: 700; "
            "padding: 4px 12px; border-radius: 10px;"
        )

    def _on_rc_changed(self, _v: int) -> None:
        self._update_rc_label()
        _merge_root_cue_tempo(_config_path(), float(self._sl_rc.value()), float(self._sl_ts.value()))

    def _on_ts_changed(self, _v: int) -> None:
        self._update_ts_label()
        _merge_root_cue_tempo(_config_path(), float(self._sl_rc.value()), float(self._sl_ts.value()))

    def _on_latency_changed(self, v: int) -> None:
        self._update_latency_ui(v)
        path = _config_path()
        try:
            data = load_config_dict(path)
            profs = data.setdefault("profiles", {})
            p = profs.setdefault(_DEFAULT_PROFILE, {})
            p["nm_server_delay_ms"] = int(v)
            save_config_dict(path, data)
        except (OSError, json.JSONDecodeError):
            pass

    def _on_toggle_shot_mode(self, checked: bool) -> None:
        path = _config_path()
        try:
            data = load_config_dict(path)
            p = data.setdefault("profiles", {}).setdefault(_DEFAULT_PROFILE, {})
            p["timing_mode"] = "meter" if checked else "animation"
            save_config_dict(path, data)
        except (OSError, json.JSONDecodeError):
            pass

    def _on_toggle_rhythm(self, checked: bool) -> None:
        path = _config_path()
        try:
            data = load_config_dict(path)
            p = data.setdefault("profiles", {}).setdefault(_DEFAULT_PROFILE, {})
            p["manual_tuning_enabled"] = bool(checked)
            save_config_dict(path, data)
        except (OSError, json.JSONDecodeError):
            pass

    def _on_toggle_autolearn(self, checked: bool) -> None:
        path = _config_path()
        try:
            data = load_config_dict(path)
            p = data.setdefault("profiles", {}).setdefault(_DEFAULT_PROFILE, {})
            p["timing_predictor_enabled"] = bool(checked)
            save_config_dict(path, data)
        except (OSError, json.JSONDecodeError):
            pass

    def _refresh_status(self) -> None:
        issues = collect_readiness_issues()
        if self._helios_embedded:
            if issues:
                self._pill.setText("● OFFLINE")
                self._pill.setStyleSheet(
                    f"QLabel#statusPill {{ background-color: #450a0a; color: {C_OFFLINE}; "
                    f"padding: 10px 16px; border-radius: 20px; font-weight: 700; }}"
                )
                self._detail.setText("; ".join(issues))
            else:
                self._pill.setText("● ACTIVE")
                self._pill.setStyleSheet(
                    f"QLabel#statusPill {{ background-color: #052e1a; color: {C_ACTIVE}; "
                    f"padding: 10px 16px; border-radius: 20px; font-weight: 700; }}"
                )
                self._detail.setText(
                    "GCV is running in Helios — use sliders and toggles; j2k_config.json hot-reloads in the DLL."
                )
            return

        scripting = self._worker is not None
        if issues:
            self._pill.setText("● OFFLINE")
            self._pill.setStyleSheet(
                f"QLabel#statusPill {{ background-color: #450a0a; color: {C_OFFLINE}; "
                f"padding: 10px 16px; border-radius: 20px; font-weight: 700; }}"
            )
            detail = "; ".join(issues)
            if scripting:
                detail = "Scripting was loaded but environment is no longer valid. Stop and fix: " + detail
            self._detail.setText(detail)
            return

        self._pill.setText("● ACTIVE")
        self._pill.setStyleSheet(
            f"QLabel#statusPill {{ background-color: #052e1a; color: {C_ACTIVE}; "
            f"padding: 10px 16px; border-radius: 20px; font-weight: 700; }}"
        )
        if scripting:
            self._detail.setText("Scripting running — GCV / j2k_ch.dll loaded.")
        else:
            self._detail.setText(
                "Ready — DLL, config, and YOLO weights found. Press Start scripting to load the engine."
            )

    def _on_start(self) -> None:
        from PyQt6.QtWidgets import QMessageBox

        issues = collect_readiness_issues()
        if issues:
            QMessageBox.warning(
                self.win,
                "J2K Vision — cannot start",
                "Fix the following, then try again:\n\n• " + "\n• ".join(issues),
            )
            self._refresh_status()
            return

        if self._worker is not None:
            return

        from run_cpp import GCVWorker

        try:
            self._worker = GCVWorker(640, 360)
        except Exception as e:
            self._worker = None
            QMessageBox.critical(self.win, "J2K Vision — DLL failed", str(e))
        self._btn_start.setEnabled(self._worker is None)
        self._btn_stop.setEnabled(self._worker is not None)
        self._refresh_status()

    def _on_stop(self) -> None:
        if self._worker is not None:
            try:
                self._worker.close()
            except Exception:
                pass
            self._worker = None
        self._btn_start.setEnabled(True)
        self._btn_stop.setEnabled(False)
        self._refresh_status()


def run_helios_panel_process() -> int:
    """Entry point for the Helios-spawned subprocess: Qt on this process's main thread."""
    try:
        from PyQt6.QtWidgets import QApplication
    except ImportError:
        print(
            "[j2k] J2K Vision panel: PyQt6 not installed for this Python.\n"
            "      Run: scripts\\j2k.bat install   (records the pip interpreter in python\\j2k_panel_python.path)\n"
            "      Or:  set J2K_PANEL_PYTHON=C:\\Path\\to\\python.exe   (same one where you ran pip install -r requirements.txt)",
            file=sys.stderr,
        )
        return 1

    app = QApplication(["j2k_vision_helios"])
    app.setStyle("Fusion")
    panel = J2KVisionWindow(helios_embedded=True)
    parent_pid = int((os.environ.get("J2K_PANEL_PARENT_PID") or "0").strip() or "0")
    if parent_pid > 0:
        from PyQt6.QtCore import QTimer

        PROCESS_QUERY_LIMITED_INFORMATION = 0x1000
        SYNCHRONIZE = 0x00100000
        WAIT_TIMEOUT = 0x00000102
        k32 = ctypes.windll.kernel32
        hproc = k32.OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, False, parent_pid)

        def _parent_alive() -> bool:
            if not hproc:
                return False
            return k32.WaitForSingleObject(hproc, 0) == WAIT_TIMEOUT

        def _watch_parent() -> None:
            if not _parent_alive():
                panel._on_stop()
                app.quit()

        timer = QTimer()
        timer.setInterval(750)
        timer.timeout.connect(_watch_parent)
        timer.start()

        def _close_parent_handle() -> None:
            try:
                if hproc:
                    k32.CloseHandle(hproc)
            except Exception:
                pass

        app.aboutToQuit.connect(_close_parent_handle)
    panel.win.show()
    return int(app.exec())


def run_app() -> int:
    global _standalone_cli_active
    _standalone_cli_active = True

    try:
        from PyQt6.QtWidgets import QApplication
    except ImportError:
        print(
            "[j2k] PyQt6 is required for the control panel.\n"
            "      Run: python -m pip install -r python\\requirements.txt",
            file=sys.stderr,
        )
        return 1

    app = QApplication(sys.argv)
    app.setStyle("Fusion")
    panel = J2KVisionWindow(helios_embedded=False)
    app.aboutToQuit.connect(panel._on_stop)
    panel.win.show()
    return int(app.exec())
