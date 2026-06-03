"""
╔══════════════════════════════════════════════════════════════════════╗
║        SOLAR TRACKER SCADA — GATEWAY APP                           ║
║        Stack : PyQt6 · PyQtGraph · paho-mqtt · firebase-admin      ║
║        Author: Generated for UIT Dual-Axis Solar Tracker Project   ║
╚══════════════════════════════════════════════════════════════════════╝

CÀI ĐẶT THƯ VIỆN (chạy một lần trong terminal):
    pip install PyQt6 pyqtgraph paho-mqtt firebase-admin

CẤU HÌNH FIREBASE:
    1. Vào https://console.firebase.google.com → chọn Project của bạn
    2. Project Settings → Service accounts → Generate new private key
    3. Đặt file JSON tải về cùng thư mục với file này, đổi tên thành:
       serviceAccountKey.json
    4. Mở file này, điền DATABASE_URL của project vào biến
       FIREBASE_DB_URL bên dưới (dạng https://<project-id>.firebaseio.com)

CHẠY:
    python solar_tracker_scada.py
"""

import sys
import json
import csv
import time
from collections import deque
from datetime import datetime

import pyqtgraph as pg
from PyQt6.QtWidgets import (
    QApplication, QMainWindow, QWidget, QLabel, QPushButton,
    QSlider, QButtonGroup, QRadioButton, QFileDialog,
    QHBoxLayout, QVBoxLayout, QGridLayout, QFrame, QSizePolicy,
    QMessageBox, QScrollArea
)
from PyQt6.QtCore import (
    Qt, QThread, pyqtSignal, QObject, QTimer, QSize
)
from PyQt6.QtGui import QFont, QColor, QPalette, QIcon

import paho.mqtt.client as mqtt

# ── Firebase (import tuỳ điều kiện, không crash nếu chưa cài) ────────
try:
    import firebase_admin
    from firebase_admin import credentials, firestore
    FIREBASE_AVAILABLE = True
except ImportError:
    FIREBASE_AVAILABLE = False
    print("[WARN] firebase-admin chưa được cài. Cloud sync bị tắt.")

# ══════════════════════════════════════════════════════════════════════
# 0. CẤU HÌNH TOÀN CỤC — CHỈNH SỬA TẠI ĐÂY
# ══════════════════════════════════════════════════════════════════════
MQTT_BROKER      = "broker.hivemq.com"
MQTT_PORT        = 1883
MQTT_TOPIC       = "uit/solar_tracker/data"
MQTT_KEEPALIVE   = 60

FIREBASE_KEY     = "serviceAccountKey.json"   # đặt cùng thư mục
FIREBASE_DB_URL  = "https://ce320---solar-tracking-system.firebaseio.com"
FIREBASE_COLLECTION = "solar_logs"

MAX_BUFFER       = 200   # số điểm dữ liệu tối đa trên biểu đồ

# ══════════════════════════════════════════════════════════════════════
# 1. WORKER THREAD — MQTT + FIREBASE
# ══════════════════════════════════════════════════════════════════════
class MqttWorker(QObject):
    """
    Chạy hoàn toàn trong QThread.
    - Subscribe MQTT, parse JSON, phát signal data_received về GUI.
    - Đẩy document lên Firestore (nếu có).
    """
    data_received    = pyqtSignal(dict)   # gửi dict đã parse về GUI
    mqtt_status      = pyqtSignal(bool)   # True = connected
    firebase_status  = pyqtSignal(bool)   # True = connected

    def __init__(self):
        super().__init__()
        self._db = None
        self._mqtt_client = None
        self._init_firebase()

    # ── Firebase ──────────────────────────────────────────────────────
    def _init_firebase(self):
        if not FIREBASE_AVAILABLE:
            self.firebase_status.emit(False)
            return
        try:
            cred = credentials.Certificate(FIREBASE_KEY)
            firebase_admin.initialize_app(cred, {"databaseURL": FIREBASE_DB_URL})
            self._db = firestore.client()
            self.firebase_status.emit(True)
            print("[Firebase] Connected to Firestore.")
        except Exception as e:
            print(f"[Firebase] Init error: {e}")
            self.firebase_status.emit(False)

    def _push_to_firestore(self, data: dict):
        if self._db is None:
            return
        try:
            doc_id = f"log_{data.get('time', int(time.time()))}"
            self._db.collection(FIREBASE_COLLECTION).document(doc_id).set(data)
        except Exception as e:
            print(f"[Firebase] Write error: {e}")

    # ── MQTT callbacks ────────────────────────────────────────────────
    def _on_connect(self, client, userdata, flags, rc, properties=None):
        connected = (rc == 0)
        self.mqtt_status.emit(connected)
        if connected:
            client.subscribe(MQTT_TOPIC)
            print(f"[MQTT] Connected & subscribed to {MQTT_TOPIC}")
        else:
            print(f"[MQTT] Connect failed rc={rc}")

    def _on_disconnect(self, client, userdata, rc, properties=None):
        self.mqtt_status.emit(False)
        print("[MQTT] Disconnected.")

    def _on_message(self, client, userdata, msg):
        payload = msg.payload.decode("utf-8")
        try:
            data = json.loads(payload)
            self.data_received.emit(data)
            self._push_to_firestore(data)
        except Exception as e:
            print(f"[MQTT] Parse error: {e}")
            print(f"[MQTT] Raw payload: {payload}")

    # ── Entry point (gọi bởi QThread.started) ────────────────────────
    def run(self):
        self._mqtt_client = mqtt.Client(
            mqtt.CallbackAPIVersion.VERSION2,
            client_id=f"SCADA_Gateway_{int(time.time())}"
        )
        self._mqtt_client.on_connect    = self._on_connect
        self._mqtt_client.on_disconnect = self._on_disconnect
        self._mqtt_client.on_message    = self._on_message

        try:
            self._mqtt_client.connect(MQTT_BROKER, MQTT_PORT, MQTT_KEEPALIVE)
            self._mqtt_client.loop_forever()   # blocking — chạy mãi trong thread
        except Exception as e:
            print(f"[MQTT] Connection exception: {e}")
            self.mqtt_status.emit(False)

    def stop(self):
        if self._mqtt_client:
            self._mqtt_client.disconnect()
            self._mqtt_client.loop_stop()


# ══════════════════════════════════════════════════════════════════════
# 2. WIDGET CON — TÁI SỬ DỤNG
# ══════════════════════════════════════════════════════════════════════

class StatusLED(QWidget):
    """Đèn LED tròn + nhãn trạng thái."""
    def __init__(self, label: str, parent=None):
        super().__init__(parent)
        self._connected = False
        layout = QHBoxLayout(self)
        layout.setContentsMargins(0, 0, 0, 0)
        layout.setSpacing(8)

        self._led = QLabel("●")
        self._led.setFont(QFont("Arial", 14))
        self._lbl = QLabel(label)
        self._lbl.setFont(QFont("SF Pro Display", 13) if sys.platform == "darwin"
                          else QFont("Segoe UI", 13))
        self._lbl.setStyleSheet("color: #3C3C43;")

        layout.addWidget(self._led)
        layout.addWidget(self._lbl)
        layout.addStretch()
        self.set_status(False)

    def set_status(self, ok: bool):
        self._connected = ok
        self._led.setStyleSheet(
            f"color: {'#34C759' if ok else '#FF3B30'};"
        )
        self._lbl.setText(
            self._lbl.text().split(" [")[0] +
            (" [O]" if ok else " [X]")
        )


class KpiCard(QFrame):
    """Thẻ KPI hiển thị số lớn theo phong cách iOS."""
    def __init__(self, title: str, unit: str, color: str = "#007AFF", parent=None):
        super().__init__(parent)
        self.setStyleSheet(f"""
            QFrame {{
                background: white;
                border-radius: 16px;
                border: 1px solid #E5E5EA;
            }}
        """)
        self.setSizePolicy(QSizePolicy.Policy.Expanding, QSizePolicy.Policy.Fixed)
        self.setMinimumHeight(90)

        layout = QVBoxLayout(self)
        layout.setContentsMargins(18, 12, 18, 12)
        layout.setSpacing(2)

        self._title_lbl = QLabel(title)
        self._title_lbl.setFont(QFont("Segoe UI", 10))
        self._title_lbl.setStyleSheet("color: #8E8E93; border: none;")

        self._value_lbl = QLabel("—")
        self._value_lbl.setFont(QFont("Segoe UI", 26, QFont.Weight.Medium))
        self._value_lbl.setStyleSheet(f"color: {color}; border: none;")

        self._unit_lbl = QLabel(unit)
        self._unit_lbl.setFont(QFont("Segoe UI", 11))
        self._unit_lbl.setStyleSheet("color: #8E8E93; border: none;")

        layout.addWidget(self._title_lbl)
        layout.addWidget(self._value_lbl)
        layout.addWidget(self._unit_lbl)

    def update_value(self, val: float, decimals: int = 2):
        self._value_lbl.setText(f"{val:.{decimals}f}")


class AngleGauge(QFrame):
    """Hiển thị góc servo kiểu đơn giản — số + thanh màu."""
    def __init__(self, label: str, color: str = "#30D158", parent=None):
        super().__init__(parent)
        self._color = color
        self.setStyleSheet("""
            QFrame {
                background: white;
                border-radius: 16px;
                border: 1px solid #E5E5EA;
            }
        """)
        self.setMinimumSize(120, 120)

        layout = QVBoxLayout(self)
        layout.setContentsMargins(12, 12, 12, 12)
        layout.setSpacing(4)
        layout.setAlignment(Qt.AlignmentFlag.AlignCenter)

        self._lbl = QLabel(label)
        self._lbl.setFont(QFont("Segoe UI", 10))
        self._lbl.setStyleSheet("color: #8E8E93; border: none;")
        self._lbl.setAlignment(Qt.AlignmentFlag.AlignCenter)

        self._val = QLabel("—°")
        self._val.setFont(QFont("Segoe UI", 28, QFont.Weight.Medium))
        self._val.setStyleSheet(f"color: {color}; border: none;")
        self._val.setAlignment(Qt.AlignmentFlag.AlignCenter)

        self._bar = QFrame()
        self._bar.setFixedHeight(5)
        self._bar.setStyleSheet(f"""
            background: qlineargradient(
                x1:0, y1:0, x2:1, y2:0,
                stop:0 #E5E5EA, stop:1 {color}
            );
            border-radius: 3px;
            border: none;
        """)

        layout.addWidget(self._lbl)
        layout.addWidget(self._val)
        layout.addWidget(self._bar)

    def update_angle(self, angle: float):
        self._val.setText(f"{angle:.1f}°")


class ErrorBar(QFrame):
    """Thanh hiển thị sai số Fuzzy (errorPan / errorTilt)."""
    def __init__(self, label: str, parent=None):
        super().__init__(parent)
        self.setStyleSheet("background: transparent; border: none;")

        row = QHBoxLayout(self)
        row.setContentsMargins(0, 4, 0, 4)
        row.setSpacing(10)

        self._lbl = QLabel(label)
        self._lbl.setFixedWidth(80)
        self._lbl.setFont(QFont("Segoe UI", 11))
        self._lbl.setStyleSheet("color: #3C3C43;")

        self._bar = QSlider(Qt.Orientation.Horizontal)
        self._bar.setRange(-500, 500)
        self._bar.setValue(0)
        self._bar.setEnabled(False)          # chỉ đọc
        self._bar.setStyleSheet("""
            QSlider::groove:horizontal {
                height: 6px; background: #E5E5EA; border-radius: 3px;
            }
            QSlider::handle:horizontal {
                width: 14px; height: 14px;
                background: #007AFF; border-radius: 7px;
                margin: -4px 0;
            }
            QSlider::sub-page:horizontal {
                background: #007AFF; border-radius: 3px;
            }
        """)

        self._val = QLabel("0.00")
        self._val.setFixedWidth(50)
        self._val.setFont(QFont("Segoe UI", 11, QFont.Weight.Medium))
        self._val.setStyleSheet("color: #007AFF;")
        self._val.setAlignment(Qt.AlignmentFlag.AlignRight | Qt.AlignmentFlag.AlignVCenter)

        row.addWidget(self._lbl)
        row.addWidget(self._bar)
        row.addWidget(self._val)

    def update_error(self, value: float):
        clamped = max(-500, min(500, int(value * 100)))
        self._bar.setValue(clamped)
        self._val.setText(f"{value:+.2f}")


def make_card(title: str) -> tuple[QFrame, QVBoxLayout]:
    """Tạo card trắng bo tròn với tiêu đề, trả về (card, inner_layout)."""
    card = QFrame()
    card.setStyleSheet("""
        QFrame {
            background: white;
            border-radius: 16px;
            border: 1px solid #E5E5EA;
        }
    """)
    outer = QVBoxLayout(card)
    outer.setContentsMargins(16, 12, 16, 14)
    outer.setSpacing(10)

    if title:
        t = QLabel(title)
        t.setFont(QFont("Segoe UI", 10, QFont.Weight.Medium))
        t.setStyleSheet("color: #8E8E93; letter-spacing: 1px; border: none;")
        outer.addWidget(t)

    return card, outer


def make_action_btn(text: str, color: str = "#007AFF") -> QPushButton:
    btn = QPushButton(text)
    btn.setFont(QFont("Segoe UI", 11, QFont.Weight.Medium))
    btn.setFixedHeight(38)
    btn.setCursor(Qt.CursorShape.PointingHandCursor)
    btn.setStyleSheet(f"""
        QPushButton {{
            background: {color}18;
            color: {color};
            border: 1.5px solid {color}55;
            border-radius: 10px;
            padding: 0 18px;
        }}
        QPushButton:hover  {{ background: {color}28; }}
        QPushButton:pressed {{ background: {color}40; }}
    """)
    return btn


def make_graph(title: str, ylabel: str, color: str) -> tuple[pg.PlotWidget, pg.PlotDataItem]:
    """Tạo PyQtGraph PlotWidget chuẩn style iOS."""
    pw = pg.PlotWidget(title=title)
    pw.setBackground("white")
    pw.showGrid(x=False, y=True, alpha=0.15)
    pw.setLabel("left", ylabel, color="#8E8E93")
    pw.setLabel("bottom", "Time", color="#8E8E93")
    pw.getAxis("left").setPen(pg.mkPen(color="#E5E5EA"))
    pw.getAxis("bottom").setPen(pg.mkPen(color="#E5E5EA"))
    pw.getAxis("left").setTextPen(pg.mkPen(color="#8E8E93"))
    pw.getAxis("bottom").setTextPen(pg.mkPen(color="#8E8E93"))
    pw.setTitle(title, color="#3C3C43", size="11pt")
    pw.getPlotItem().setContentsMargins(4, 4, 4, 4)

    pen  = pg.mkPen(color=color, width=2)
    fill = pg.mkBrush(color + "25")
    curve = pw.plot(pen=pen, fillLevel=0, brush=fill)
    return pw, curve


# ══════════════════════════════════════════════════════════════════════
# 3. CỬA SỔ CHÍNH
# ══════════════════════════════════════════════════════════════════════
class MainWindow(QMainWindow):
    def __init__(self):
        super().__init__()
        self.setWindowTitle("Solar Tracker SCADA Dashboard")
        self.resize(1420, 820)
        self.setMinimumSize(1100, 700)
        self.setStyleSheet("QMainWindow { background: #F2F2F7; }")

        # ── Bộ đệm cuộn ──────────────────────────────────────────────
        self._buf_time    = deque(maxlen=MAX_BUFFER)
        self._buf_power   = deque(maxlen=MAX_BUFFER)
        self._buf_voltage = deque(maxlen=MAX_BUFFER)
        self._buf_current = deque(maxlen=MAX_BUFFER)
        self._history: list[dict] = []   # toàn bộ dữ liệu phiên (cho CSV)
        self._yield_wh: float = 0.0      # tích luỹ năng lượng
        self._last_time: float | None = None

        self._manual_mode = False

        self._build_ui()
        self._start_worker()

    # ─────────────────────────────────────────────────────────────────
    # 3-A. XÂY DỰNG GIAO DIỆN
    # ─────────────────────────────────────────────────────────────────
    def _build_ui(self):
        central = QWidget()
        central.setStyleSheet("background: #F2F2F7;")
        self.setCentralWidget(central)

        root = QHBoxLayout(central)
        root.setContentsMargins(16, 16, 16, 16)
        root.setSpacing(14)

        # ═══ SIDEBAR (25%) ═══════════════════════════════════════════
        sidebar = QWidget()
        sidebar.setFixedWidth(280)
        sidebar.setStyleSheet("background: transparent;")
        sb_layout = QVBoxLayout(sidebar)
        sb_layout.setContentsMargins(0, 0, 0, 0)
        sb_layout.setSpacing(12)

        # Title
        title = QLabel("SOLAR TRACKER\nSCADA DASHBOARD")
        title.setFont(QFont("Segoe UI", 15, QFont.Weight.Bold))
        title.setStyleSheet("color: #1C1C1E; background: transparent;")
        title.setAlignment(Qt.AlignmentFlag.AlignCenter)
        sb_layout.addWidget(title)

        # Network status card
        net_card, net_inner = make_card("NETWORK STATUS")
        self._led_mqtt     = StatusLED("[O] MQTT Broker")
        self._led_firebase = StatusLED("[O] Cloud Firebase")
        net_inner.addWidget(self._led_mqtt)
        net_inner.addWidget(self._led_firebase)
        sb_layout.addWidget(net_card)

        # Control mode card
        mode_card, mode_inner = make_card("CONTROL MODE")
        self._btn_fuzzy  = QRadioButton("Fuzzy Logic")
        self._btn_manual = QRadioButton("Manual Override")
        self._btn_fuzzy.setFont(QFont("Segoe UI", 12))
        self._btn_manual.setFont(QFont("Segoe UI", 12))
        self._btn_fuzzy.setChecked(True)
        mode_inner.addWidget(self._btn_fuzzy)
        mode_inner.addWidget(self._btn_manual)
        self._mode_group = QButtonGroup()
        self._mode_group.addButton(self._btn_fuzzy, 0)
        self._mode_group.addButton(self._btn_manual, 1)
        self._mode_group.idClicked.connect(self._on_mode_changed)
        for rb in [self._btn_fuzzy, self._btn_manual]:
            rb.setStyleSheet("""
                QRadioButton { color: #3C3C43; border: none; }
                QRadioButton::indicator { width:16px; height:16px; }
            """)
        sb_layout.addWidget(mode_card)

        # Manual override card
        man_card, man_inner = make_card("MANUAL OVERRIDE")

        pan_row = QHBoxLayout()
        pan_lbl = QLabel("Pan Angle"); pan_lbl.setStyleSheet("color:#3C3C43; border:none;")
        pan_lbl.setFont(QFont("Segoe UI", 11))
        self._pan_val_lbl = QLabel("90°"); self._pan_val_lbl.setStyleSheet("color:#007AFF; border:none;")
        self._pan_val_lbl.setFont(QFont("Segoe UI", 11, QFont.Weight.Medium))
        pan_row.addWidget(pan_lbl); pan_row.addStretch(); pan_row.addWidget(self._pan_val_lbl)

        self._pan_slider = QSlider(Qt.Orientation.Horizontal)
        self._pan_slider.setRange(0, 180); self._pan_slider.setValue(90)
        self._pan_slider.setEnabled(False)
        self._pan_slider.valueChanged.connect(
            lambda v: self._pan_val_lbl.setText(f"{v}°"))
        self._style_slider(self._pan_slider)

        tilt_row = QHBoxLayout()
        tilt_lbl = QLabel("Tilt Angle"); tilt_lbl.setStyleSheet("color:#3C3C43; border:none;")
        tilt_lbl.setFont(QFont("Segoe UI", 11))
        self._tilt_val_lbl = QLabel("90°"); self._tilt_val_lbl.setStyleSheet("color:#007AFF; border:none;")
        self._tilt_val_lbl.setFont(QFont("Segoe UI", 11, QFont.Weight.Medium))
        tilt_row.addWidget(tilt_lbl); tilt_row.addStretch(); tilt_row.addWidget(self._tilt_val_lbl)

        self._tilt_slider = QSlider(Qt.Orientation.Horizontal)
        self._tilt_slider.setRange(20, 160); self._tilt_slider.setValue(90)
        self._tilt_slider.setEnabled(False)
        self._tilt_slider.valueChanged.connect(
            lambda v: self._tilt_val_lbl.setText(f"{v}°"))
        self._style_slider(self._tilt_slider)

        man_inner.addLayout(pan_row)
        man_inner.addWidget(self._pan_slider)
        man_inner.addLayout(tilt_row)
        man_inner.addWidget(self._tilt_slider)
        sb_layout.addWidget(man_card)

        sb_layout.addStretch()

        # Action buttons
        self._btn_export = make_action_btn("EXPORT CSV", "#007AFF")
        self._btn_reset  = make_action_btn("RESET DATA", "#FF3B30")
        self._btn_export.clicked.connect(self._export_csv)
        self._btn_reset.clicked.connect(self._reset_data)
        sb_layout.addWidget(self._btn_export)
        sb_layout.addWidget(self._btn_reset)

        root.addWidget(sidebar)

        # ═══ MAIN PANEL ══════════════════════════════════════════════
        main_panel = QWidget()
        main_panel.setStyleSheet("background: transparent;")
        mp_layout = QVBoxLayout(main_panel)
        mp_layout.setContentsMargins(0, 0, 0, 0)
        mp_layout.setSpacing(12)

        # --- KPI row ---
        kpi_row = QHBoxLayout(); kpi_row.setSpacing(12)
        self._kpi_power   = KpiCard("Công suất tức thời",  "mW",       "#007AFF")
        self._kpi_yield   = KpiCard("Điện năng tích lũy",  "mWh",      "#FF9500")
        self._kpi_voltage = KpiCard("Điện áp",             "Volt",     "#34C759")
        self._kpi_current = KpiCard("Dòng điện",           "mA",       "#AF52DE")
        for k in [self._kpi_power, self._kpi_yield, self._kpi_voltage, self._kpi_current]:
            kpi_row.addWidget(k)
        mp_layout.addLayout(kpi_row)

        # --- Graph: Power P(t) ---
        self._pw_power, self._curve_power = make_graph(
            "REAL-TIME POWER GRAPH  P(t)", "Power (mW)", "#007AFF")
        self._pw_power.setMinimumHeight(180)
        mp_layout.addWidget(self._pw_power)

        # --- Graph row: Voltage + Current ---
        g2_row = QHBoxLayout(); g2_row.setSpacing(12)
        self._pw_volt, self._curve_volt = make_graph(
            "LOAD VOLTAGE  V(t)", "Voltage (V)", "#34C759")
        self._pw_curr, self._curve_curr = make_graph(
            "LOAD CURRENT  I(t)", "Current (mA)", "#FF6B35")
        for pw in [self._pw_volt, self._pw_curr]:
            pw.setMinimumHeight(150)
            g2_row.addWidget(pw)
        mp_layout.addLayout(g2_row)

        # --- Bottom row: Fuzzy Diagnostic + Angle Gauges ---
        bot_row = QHBoxLayout(); bot_row.setSpacing(12)

        fuzzy_card, fuzzy_inner = make_card("FUZZY CONTROLLER DIAGNOSTIC")
        self._err_pan  = ErrorBar("errorPan")
        self._err_tilt = ErrorBar("errorTilt")
        fuzzy_inner.addWidget(self._err_pan)
        fuzzy_inner.addWidget(self._err_tilt)
        fuzzy_card.setSizePolicy(QSizePolicy.Policy.Expanding, QSizePolicy.Policy.Fixed)
        bot_row.addWidget(fuzzy_card, 3)

        self._gauge_pan  = AngleGauge("Pan Angle",  "#30D158")
        self._gauge_tilt = AngleGauge("Tilt Angle", "#30D158")
        bot_row.addWidget(self._gauge_pan,  1)
        bot_row.addWidget(self._gauge_tilt, 1)

        mp_layout.addLayout(bot_row)
        root.addWidget(main_panel, 1)

    @staticmethod
    def _style_slider(sl: QSlider):
        sl.setStyleSheet("""
            QSlider::groove:horizontal {
                height: 5px; background: #E5E5EA; border-radius: 3px;
            }
            QSlider::handle:horizontal {
                width: 18px; height: 18px;
                background: #007AFF; border-radius: 9px;
                margin: -7px 0;
            }
            QSlider::sub-page:horizontal {
                background: #007AFF; border-radius: 3px;
            }
            QSlider:disabled::handle { background: #C7C7CC; }
            QSlider:disabled::sub-page { background: #C7C7CC; }
        """)

    # ─────────────────────────────────────────────────────────────────
    # 3-B. KHỞI ĐỘNG WORKER THREAD
    # ─────────────────────────────────────────────────────────────────
    def _start_worker(self):
        self._thread = QThread(self)
        self._worker = MqttWorker()
        self._worker.moveToThread(self._thread)

        self._thread.started.connect(self._worker.run)
        self._worker.data_received.connect(self._on_data)
        self._worker.mqtt_status.connect(
            lambda ok: self._led_mqtt.set_status(ok))
        self._worker.firebase_status.connect(
            lambda ok: self._led_firebase.set_status(ok))

        self._thread.start()

    # ─────────────────────────────────────────────────────────────────
    # 3-C. XỬ LÝ DỮ LIỆU ĐẾN (SLOT — chạy trên GUI Thread)
    # ─────────────────────────────────────────────────────────────────
    def _on_data(self, data: dict):
        t       = float(data.get("time",      time.time()))
        voltage = float(data.get("voltage",   0.0))
        current = float(data.get("current",   0.0))
        power   = float(data.get("power",     0.0))
        err_pan = float(data.get("errorPan",  0.0))
        err_tilt= float(data.get("errorTilt", 0.0))
        pan     = float(data.get("panAngle",  90.0))
        tilt    = float(data.get("tiltAngle", 90.0))

        # Tích lũy điện năng (Wh) = P * Δt / 3600
        if self._last_time is not None:
            dt = t - self._last_time
            if 0 < dt < 10:                # bỏ qua nếu Δt bất thường
                self._yield_wh += power * dt / 3600.0
        self._last_time = t

        # Ghi vào buffer cuộn
        self._buf_time.append(t)
        self._buf_power.append(power)
        self._buf_voltage.append(voltage)
        self._buf_current.append(current)

        # Ghi vào lịch sử CSV
        data["yield_wh"] = round(self._yield_wh, 4)
        self._history.append(data)

        # ── Cập nhật UI ──────────────────────────────────────────────
        self._kpi_power.update_value(power,   3)
        self._kpi_yield.update_value(self._yield_wh, 4)
        self._kpi_voltage.update_value(voltage, 3)
        self._kpi_current.update_value(current, 4)

        # Chuỗi thời gian tương đối (giây từ điểm đầu)
        t0 = self._buf_time[0]
        xs = [x - t0 for x in self._buf_time]
        self._curve_power.setData(xs, list(self._buf_power))
        self._curve_volt.setData(xs,  list(self._buf_voltage))
        self._curve_curr.setData(xs,  list(self._buf_current))

        self._err_pan.update_error(err_pan)
        self._err_tilt.update_error(err_tilt)
        self._gauge_pan.update_angle(pan)
        self._gauge_tilt.update_angle(tilt)

        # Đồng bộ slider khi ở chế độ Auto
        if not self._manual_mode:
            self._pan_slider.setValue(int(pan))
            self._tilt_slider.setValue(int(tilt))

    # ─────────────────────────────────────────────────────────────────
    # 3-D. SỰ KIỆN NÚT BẤM
    # ─────────────────────────────────────────────────────────────────
    def _on_mode_changed(self, btn_id: int):
        self._manual_mode = (btn_id == 1)
        self._pan_slider.setEnabled(self._manual_mode)
        self._tilt_slider.setEnabled(self._manual_mode)

    def _export_csv(self):
        if not self._history:
            QMessageBox.information(self, "Export CSV",
                                    "Chưa có dữ liệu trong phiên làm việc này.")
            return
        path, _ = QFileDialog.getSaveFileName(
            self, "Lưu file CSV", f"solar_log_{int(time.time())}.csv",
            "CSV Files (*.csv)")
        if not path:
            return
        try:
            keys = list(self._history[0].keys())
            with open(path, "w", newline="", encoding="utf-8") as f:
                writer = csv.DictWriter(f, fieldnames=keys)
                writer.writeheader()
                writer.writerows(self._history)
            QMessageBox.information(self, "Export CSV",
                f"Đã xuất {len(self._history)} bản ghi vào:\n{path}")
        except Exception as e:
            QMessageBox.critical(self, "Export CSV", f"Lỗi: {e}")

    def _reset_data(self):
        ans = QMessageBox.question(
            self, "Reset Data",
            "Xóa toàn bộ dữ liệu trong RAM?\nHành động này không thể hoàn tác.",
            QMessageBox.StandardButton.Yes | QMessageBox.StandardButton.No)
        if ans != QMessageBox.StandardButton.Yes:
            return
        self._buf_time.clear()
        self._buf_power.clear()
        self._buf_voltage.clear()
        self._buf_current.clear()
        self._history.clear()
        self._yield_wh = 0.0
        self._last_time = None
        for c in [self._curve_power, self._curve_volt, self._curve_curr]:
            c.setData([], [])
        for k in [self._kpi_power, self._kpi_yield, self._kpi_voltage, self._kpi_current]:
            k.update_value(0.0)
        self._err_pan.update_error(0.0)
        self._err_tilt.update_error(0.0)

    # ─────────────────────────────────────────────────────────────────
    # 3-E. ĐÓNG CỬA SỔ — dừng thread sạch
    # ─────────────────────────────────────────────────────────────────
    def closeEvent(self, event):
        self._worker.stop()
        self._thread.quit()
        self._thread.wait(3000)
        event.accept()


# ══════════════════════════════════════════════════════════════════════
# 4. ENTRY POINT
# ══════════════════════════════════════════════════════════════════════
if __name__ == "__main__":
    # Bật High-DPI scaling cho màn hình Retina / 4K
    QApplication.setHighDpiScaleFactorRoundingPolicy(
        Qt.HighDpiScaleFactorRoundingPolicy.PassThrough)

    app = QApplication(sys.argv)
    app.setStyle("Fusion")

    # Palette nền sáng
    palette = QPalette()
    palette.setColor(QPalette.ColorRole.Window,      QColor("#F2F2F7"))
    palette.setColor(QPalette.ColorRole.WindowText,  QColor("#1C1C1E"))
    palette.setColor(QPalette.ColorRole.Base,        QColor("#FFFFFF"))
    palette.setColor(QPalette.ColorRole.Text,        QColor("#1C1C1E"))
    app.setPalette(palette)

    win = MainWindow()
    win.show()
    sys.exit(app.exec())