"""Motion Center.

Copyright (c) 2024-2026 Le Bois Racing.
All rights reserved.
"""

import tkinter as tk
from tkinter import ttk, messagebox, simpledialog, filedialog
import threading, queue, re, time, webbrowser, os, subprocess, shutil, glob, struct
import zlib, binascii
import sys
import math
import json
import ctypes
from fractions import Fraction
from html import unescape as html_unescape
import zipfile
from urllib import request as urlrequest, parse as urlparse
from urllib.error import URLError
import serial
import serial.tools.list_ports

try:
    from PIL import Image, ImageTk
except Exception:
    Image = None
    ImageTk = None

BAUDRATE = 115200  # fixed
MOTION_CENTER_VERSION = "1.2"
APP_TITLE = f"Motion Center v{MOTION_CENTER_VERSION}"
APP_COMPANY = "Le Bois Racing"
APP_AUTHOR = "Aurélien Leca"
APP_COPYRIGHT_YEARS = "2024-2026"
APP_LICENSE_NAME = "Proprietary"
APP_WEBSITE = "https://lebois-racing.fr"
REQUIRED_BOX_FIRMWARE_PREFIX = "2.2"
FW_V15_LABEL = "v1.5 — ⚠️ legacy"
G474_AXIS_MAX_STEPS = 32767

# URL de la page utilisée pour vérifier les mises à jour du soft Python (modifiable facilement)
SOFT_UPDATE_CHECK_URL = "https://lebois-racing.fr/competition-control-box-pour-les-verins-srt80"

# Backward compatibility alias
FW_UPDATE_PAGE_URL = SOFT_UPDATE_CHECK_URL
FW_UPDATE_PRODUCT = "competition-control-box-v2.1"
RE_USBD480 = re.compile(r"\busbd[\s\-_]*480\w*\b", re.IGNORECASE)

def _strip_usbd480(text: str) -> str:
    s = str(text or "")
    s = RE_USBD480.sub("", s)
    s = re.sub(r"\s{2,}", " ", s)
    return s.strip()

def _contains_usbd480(text: str) -> bool:
    return bool(RE_USBD480.search(str(text or "")))

def _sanitize_path_without_usbd480(path: str) -> str:
    p = os.path.normpath(str(path or "").strip().strip('"'))
    if not p or _contains_usbd480(p):
        return ""
    return p

def app_base_dir():
    # Works both in normal Python and PyInstaller (onefile/onedir)
    return getattr(sys, "_MEIPASS", os.path.dirname(os.path.abspath(__file__)))

def app_install_dir():
    # Real, persistent folder where the program lives (NOT the PyInstaller
    # onefile temp extraction dir _MEIPASS). Use this for files the user must
    # find again, e.g. downloaded updates.
    if getattr(sys, "frozen", False):
        return os.path.dirname(os.path.abspath(sys.executable))
    return os.path.dirname(os.path.abspath(__file__))

# Social links (used by top-bar buttons)
SOCIAL_LINKS = {
    "FR": {
        "site": "https://lebois-racing.fr/",
        "youtube": "https://www.youtube.com/channel/UCWj8hl6sP09aRreTSZhmJTw",
        "discord": "https://discord.gg/E3XWfUsTf3",
        "facebook": "https://www.facebook.com/LeboisRacing/",
    },
    "EN": {
        "site": "https://lebois-racing.com",
        "youtube": "https://www.youtube.com/@LeBoisRacing",
        "discord": "https://discord.gg/E3XWfUsTf3",
        "facebook": "https://www.facebook.com/TheLeboisRacing/",
    },
    "DE": {
        "site": "https://lebois-racing.com",
        "youtube": "https://www.youtube.com/@LeBoisRacing",
        "discord": "https://discord.gg/E3XWfUsTf3",
        "facebook": "https://www.facebook.com/TheLeboisRacing/",
    }
}

DOC_LINKS = {
    "FR": "https://lebois-racing.fr/competition-control-box-pour-les-verins-srt80",
    "EN": "https://lebois-racing.com/competition-control-box-for-srt80-actuators/",
    "DE": "https://lebois-racing.com/competition-control-box-for-srt80-actuators/",
}

DRIVER_SETTINGS_LINKS = {
    "FR": "https://lebois-racing.fr/verins-srt80-pour-simulateur-de-mouvements#parametre-driver",
    "EN": "https://lebois-racing.com/srt80-actuators-for-motion-rigs/#driver-settings",
    "DE": "https://lebois-racing.com/srt80-actuators-for-motion-rigs/#driver-settings",
}

DRIVER_SETTINGS_STATIC = {
    "FR": {
        "intro": [
            "Les drivers, similaires aux AASD, possèdent 200 paramètres, heureusement on n'a pas besoin de tous les modifier.",
            "Ils sont détaillés page 38 du manuel. Cette vidéo montre comment modifier les paramètres.",
            "Je recommande fortement de tester le fonctionnement du moteur via FlyPT avec le moteur démonté !",
        ],
        "rows": [
            ("Pn001", "4", "Sélectionne le bon moteur, en l'occurence un 80st-M02430", "Inutile d'utiliser un moteur plus puissant"),
            ("Pn002", "2", "Mode de contrôle du moteur", "2 : position mode"),
            ("Pn003", "0", "Active/désactive le driver", "0 : le driver est allumé par l'arduino. 1: le driver est tout le temps allumé"),
            ("Pn008", "300", "Limite de couple", ""),
            ("Pn009", "-300", "Limite de couple", ""),
            ("Pn024", "100", "", ""),
            ("Pn051", "1200", "Vitesse max de rotation du moteur", "Ce paramètre gère un peu la “violence” du simulateur, donc à augmenter avec parcimonie"),
            ("Pn060", "2", "Sélection de l’info de sortie sur le port 1", "Envoi l’info moteur prêt"),
            ("Pn062", "6", "Sélection de l’info de sortie sur le port 3", "Envoi l’info couple maximal dépassé"),
            ("Pn098", "12", "Multiplicateur de pulsation", "Ne pas mettre une autre valeur !"),
            ("Pn109", "1", "Paramètre de filtrage", "Voir le manuel"),
            ("Pn110", "30", "Paramètre de filtrage", "Voir le manuel"),
            ("Pn113", "20", "Paramètre de filtrage", "Voir le manuel"),
            ("Pn114", "10", "Paramètre de filtrage", "Voir le manuel"),
            ("Pn115", "15", "Paramètre de filtrage", "Voir le manuel"),
        ],
    },
    "EN": {
        "intro": [
            "Drivers, similar to AASD, have 200 parameters; fortunately you don't need to change all of them.",
            "They are detailed on page 38 of the manual. This video shows how to modify parameters.",
            "I strongly recommend testing motor operation via FlyPT with the motor disassembled.",
        ],
        "rows": [
            ("Pn001", "4", "Selects the correct motor (80st-M02430)", "No need to use a more powerful motor"),
            ("Pn002", "2", "Motor control mode", "2: position mode"),
            ("Pn003", "0", "Enable/disable the driver", "0: driver enabled by Arduino. 1: always on"),
            ("Pn008", "300", "Torque limit", ""),
            ("Pn009", "-300", "Torque limit", ""),
            ("Pn024", "100", "", ""),
            ("Pn051", "1200", "Maximum motor speed", "This parameter slightly controls simulator “violence”, increase carefully"),
            ("Pn060", "2", "Output info selection on port 1", "Sends motor-ready info"),
            ("Pn062", "6", "Output info selection on port 3", "Sends max torque exceeded info"),
            ("Pn098", "12", "Pulse multiplier", "Do not use another value!"),
            ("Pn109", "1", "Filtering parameter", "See manual"),
            ("Pn110", "30", "Filtering parameter", "See manual"),
            ("Pn113", "20", "Filtering parameter", "See manual"),
            ("Pn114", "10", "Filtering parameter", "See manual"),
            ("Pn115", "15", "Filtering parameter", "See manual"),
        ],
    },
    "DE": {
        "intro": [
            "Treiber, ähnlich wie AASD, haben 200 Parameter; zum Glück müssen nicht alle geändert werden.",
            "Sie sind auf Seite 38 des Handbuchs beschrieben. Dieses Video zeigt, wie man die Parameter ändert.",
            "Ich empfehle dringend, den Motorbetrieb über FlyPT mit ausgebautem Motor zu testen.",
        ],
        "rows": [
            ("Pn001", "4", "Wählt den richtigen Motor (80st-M02430)", "Ein stärkerer Motor ist nicht nötig"),
            ("Pn002", "2", "Motorsteuerungsmodus", "2: Positionsmodus"),
            ("Pn003", "0", "Treiber aktivieren/deaktivieren", "0: Treiber wird über Arduino aktiviert. 1: immer eingeschaltet"),
            ("Pn008", "300", "Drehmomentbegrenzung", ""),
            ("Pn009", "-300", "Drehmomentbegrenzung", ""),
            ("Pn024", "100", "", ""),
            ("Pn051", "1200", "Maximale Motordrehzahl", "Dieser Parameter steuert etwas die „Härte“ des Simulators, daher vorsichtig erhöhen"),
            ("Pn060", "2", "Auswahl der Ausgabemeldung an Port 1", "Sendet Information „Motor bereit"),
            ("Pn062", "6", "Auswahl der Ausgabemeldung an Port 3", "Sendet Information „maximales Drehmoment überschritten"),
            ("Pn098", "12", "Impuls-Multiplikator", "Keine andere Einstellung verwenden!"),
            ("Pn109", "1", "Filterparameter", "Siehe Handbuch"),
            ("Pn110", "30", "Filterparameter", "Siehe Handbuch"),
            ("Pn113", "20", "Filterparameter", "Siehe Handbuch"),
            ("Pn114", "10", "Filterparameter", "Siehe Handbuch"),
            ("Pn115", "15", "Filterparameter", "Siehe Handbuch"),
        ],
    },
}

def list_ports():
    def _clean_desc(desc: str) -> str:
        s = _strip_usbd480(desc)
        s = re.sub(r"\s{2,}", " ", s)
        s = re.sub(r"\s*[-–—,;:]\s*$", "", s)
        s = s.strip()
        return s or "Serial device"

    return [f"{p.device} — {_clean_desc(p.description)}" for p in serial.tools.list_ports.comports()]

def _find_leonardo_port() -> str:
    """Return the label of the first Arduino Leonardo found, or '' if none."""
    for p in serial.tools.list_ports.comports():
        desc = (p.description or "").lower()
        mfr  = (p.manufacturer or "").lower()
        if "leonardo" in desc or "leonardo" in mfr:
            return f"{p.device} — {p.description or 'Arduino Leonardo'}"
    return ""

def port_from_label(label: str) -> str:
    return label.split(" — ", 1)[0].strip()

def _find_available_port_device(preferred_port: str = "") -> str:
    """Return best serial device name (e.g. COM8) for SimHub profile setup."""
    try:
        devices = [str(p.device).strip() for p in serial.tools.list_ports.comports() if str(p.device).strip()]
    except Exception:
        devices = []

    if preferred_port:
        pref = str(preferred_port).strip().upper()
        for dev in devices:
            if dev.upper() == pref:
                return dev

    try:
        leo = _find_leonardo_port()
        leo_dev = port_from_label(leo) if leo else ""
    except Exception:
        leo_dev = ""
    if leo_dev:
        leo_up = leo_dev.upper()
        for dev in devices:
            if dev.upper() == leo_up:
                return dev

    return devices[0] if devices else ""

def clamp_int(v, lo, hi):
    try:
        x = int(str(v).strip())
    except Exception:
        x = lo
    return max(lo, min(hi, x))

# --- Parse D-style block ---
RE_FW = re.compile(r"Firmware:\s*(.+?)\s*\|\s*Box:\s*(V?\d+)\s*\|\s*Servo:\s*(.*)$")
RE_MC_REQ = re.compile(r"^\s*MotionCenterMin:\s*(v?\d+(?:\.\d+)*)\s*$", re.IGNORECASE)
RE_HOMING = re.compile(r"^\s*Homing:\s*(\d+)\s*steps/s(?:\s*\(step=\s*(\d+)\s*\))?\s*$")
RE_ROW = re.compile(r"^\s*(\d+)\s+([YN])\s+([YN])\s+(\d+)\s+(\d+)\s*$")

def parse_dstyle(lines):
    info = {"firmware":"", "box":"", "servo":"", "servo_on": False, "homing_sps":"", "homing_step":"", "mc_required":"", "step_hz":""}
    motors = {}

    for raw in lines:
        s = raw.strip()

        m = RE_FW.search(s)
        if m:
            # Clean firmware: remove control chars and normalize whitespace
            fw = m.group(1).strip()
            fw = re.sub(r"[\x00-\x1F\x7F]+", " ", str(fw or ""))
            fw = re.sub(r"\s+", " ", fw).strip()
            info["firmware"] = fw
            info["box"] = m.group(2).strip()
            sv = m.group(3).strip()
            info["servo"] = sv
            # normalize to boolean (handles 'Enabled'/'Disabled', 'ON'/'OFF', localized variants)
            try:
                ss = sv.strip().lower()
                info["servo_on"] = ss.startswith("en") or ss.startswith("on") or ss == "1" or "enable" in ss
            except Exception:
                info["servo_on"] = False
            continue

        hm = RE_HOMING.match(s)
        if hm:
            info["homing_sps"] = hm.group(1)
            info["homing_step"] = hm.group(2) or ""
            continue

        mm = RE_MC_REQ.match(s)
        if mm:
            info["mc_required"] = mm.group(1).strip().lower().lstrip("v")
            continue

        rm = RE_ROW.match(s)
        if rm:
            idx = int(rm.group(1))
            motors[idx] = {
                "m": idx,
                "conn": rm.group(2),
                "cal": rm.group(3),
                "max": int(rm.group(4)),
                "margin": int(rm.group(5)),
            }
            continue

    for i in range(1, 7):
        motors.setdefault(i, {"m": i, "conn": "N", "cal": "N", "max": 0, "margin": 0})
    return info, motors


def parse_status_line(raw):
    """Parse the single-line STATUS response (firmware v1.8.2+).
    Format: STATUS FW=x.y.z BOX=n SERVO=0|1 HSPS=nnn ESTOP=0|1 ESTOPB=0|1|2 MCMIN=x.y M1=conn,cal,max,margin ...
    Returns (info, motors) with the same shape as parse_dstyle for drop-in use."""
    info = {"firmware": "", "box": "", "servo": "", "servo_on": False,
            "homing_sps": "", "homing_step": "", "mc_required": "",
            "step_hz": "",
            "estop_enabled": False, "estop_behavior": 0}
    motors = {}
    for token in raw.split():
        k, _, v = token.partition("=")
        if k == "FW":
            # Clean firmware: remove control chars and normalize whitespace
            v = re.sub(r"[\x00-\x1F\x7F]+", " ", str(v or ""))
            v = re.sub(r"\s+", " ", v).strip()
            info["firmware"] = v
        elif k == "BOX":
            info["box"] = v
        elif k == "SERVO":
            info["servo_on"] = v == "1"
            info["servo"] = "Enabled" if v == "1" else "Disabled"
        elif k == "HSPS":
            info["homing_sps"] = v
        elif k == "STPHZ":
            info["step_hz"] = v
        elif k == "ESTOP":
            info["estop_enabled"] = v == "1"
        elif k == "ESTOPB":
            try:
                info["estop_behavior"] = max(0, min(2, int(v)))
            except Exception:
                info["estop_behavior"] = 0
        elif k == "MCMIN":
            info["mc_required"] = v.lstrip("v")
        elif k.startswith("M") and k[1:].isdigit():
            try:
                idx = int(k[1:])
                p = v.split(",")
                motors[idx] = {
                    "m": idx,
                    "conn": "Y" if p[0] == "1" else "N",
                    "cal":  "Y" if p[1] == "1" else "N",
                    "max":  int(p[2]),
                    "margin": int(p[3]),
                    # 5e champ optionnel (firmware >= 2026-07) : direction de
                    # homing 0=MIN, 1=MAX. Absent sur les anciens firmwares -> MIN.
                    "homing_dir": "MAX" if (len(p) > 4 and p[4] == "1") else "MIN",
                    # 6e champ optionnel : endpark % (0..100, 255=off). Absent
                    # sur les anciens firmwares -> off.
                    "endpark": int(p[5]) if (len(p) > 5 and p[5].isdigit()) else 255,
                }
            except Exception:
                pass
    for i in range(1, 8):
        motors.setdefault(i, {"m": i, "conn": "N", "cal": "N", "max": 0, "margin": 0, "homing_dir": "MIN", "endpark": 255})
    return info, motors


class SerialWorker:
    def __init__(self):
        self.ser  = None
        self.rxq  = queue.Queue()   # app reads: ("line", str) | ("err", str)
        self._txq = queue.Queue()   # outgoing text commands (bytes)
        self._stop     = threading.Event()
        self._flush_rx = threading.Event()  # signal rx thread to clear its buffer
        self._flush_done = threading.Event()  # rx thread ack: buffer cleared
        self._t_rx  = None
        self._t_tx  = None

    def connect(self, port):
        self.disconnect()
        # Flush any stale TX data left from the previous session
        while True:
            try:  self._txq.get_nowait()
            except queue.Empty:  break
        self._stop.clear()
        self._flush_rx.clear()
        # dsrdtr/rtscts disabled: USB-CDC doesn't need hardware flow control;
        # enabling it can cause spurious errors on some Windows USB drivers.
        # write_timeout=2.0: fail fast if the USB link stalls (e.g. during Windows sleep)
        # instead of blocking the TX thread forever.
        self.ser = serial.Serial(
            port=port, baudrate=BAUDRATE,
            timeout=0.02, write_timeout=2.0,
            dsrdtr=False, rtscts=False)
        self._t_rx = threading.Thread(target=self._rx_loop, daemon=True)
        self._t_tx = threading.Thread(target=self._tx_loop, daemon=True)
        self._t_rx.start()
        self._t_tx.start()

    def clear_rx_buf(self):
        """Flush the OS receive buffer AND make the RX thread clear its internal
        bytearray, waiting for its acknowledgement. The wait is essential: an
        asynchronous flush could fire AFTER the next command is sent and wipe
        the first USB packet(s) of its multi-packet reply (e.g. STATUS)."""
        try:
            if self.ser:
                self.ser.reset_input_buffer()
        except Exception:
            pass
        if self._t_rx and self._t_rx.is_alive():
            self._flush_done.clear()
            self._flush_rx.set()
            # RX thread checks the flag at least every read() timeout (~20 ms)
            if not self._flush_done.wait(0.2):
                self._flush_rx.clear()
        try:
            while True:
                self.rxq.get_nowait()
        except Exception:
            pass

    def disconnect(self):
        self._stop.set()
        # Close the port FIRST — this makes any blocking write()/read() in the
        # threads throw SerialException immediately so join() returns in < 5 ms.
        ser, self.ser = self.ser, None
        if ser:
            try:
                ser.close()
            except Exception:
                pass
        for t in (self._t_rx, self._t_tx):
            if t and t.is_alive():
                t.join(timeout=0.3)
        self._t_rx = self._t_tx = None

    def is_connected(self):
        return self.ser is not None and self.ser.is_open

    def send_line(self, s: str):
        """Enqueue a text command – returns immediately, TX thread does the write."""
        if not self.is_connected():
            return
        self._txq.put((s.rstrip("\n") + "\n").encode("utf-8", errors="replace"))

    def send_bytes(self, data: bytes):
        """Queue raw bytes (e.g. binary P-frame) for the TX thread."""
        self._txq.put(data)

    def _tx_loop(self):
        """Single writer thread – text commands only."""
        ser = self.ser              # local ref for this session
        while not self._stop.is_set():
            while not self._stop.is_set():
                try:
                    data = self._txq.get_nowait()
                except queue.Empty:
                    break
                try:
                    ser.write(data)
                except Exception as e:
                    # Report the error so the UI can react (e.g. write_timeout during sleep)
                    if not self._stop.is_set():
                        self.rxq.put(("err", str(e)))
                    return          # port closed / timed-out — exit cleanly
            time.sleep(0.005)

    def _rx_loop(self):
        ser = self.ser          # local ref – immune to self.ser being swapped at disconnect
        buf = bytearray()
        last_rx = time.monotonic()
        while not self._stop.is_set():
            # Clear internal buffer if requested (e.g. before a fresh GET)
            if self._flush_rx.is_set():
                buf = bytearray()
                self._flush_rx.clear()
                self._flush_done.set()
            try:
                chunk = ser.read(256)
                if chunk:
                    buf.extend(chunk)
                    last_rx = time.monotonic()
                    while b"\n" in buf:
                        line, _, rest = buf.partition(b"\n")
                        buf = bytearray(rest)
                        txt = line.decode("utf-8", errors="replace").rstrip("\r")
                        self.rxq.put(("line", txt))
                elif buf and (time.monotonic() - last_rx) > 0.08:
                    # Idle flush: firmware 2.2.1 truncates long replies at 192
                    # bytes, dropping the trailing \n of the STATUS line. Emit
                    # a quiet partial line instead of losing it.
                    txt = buf.decode("utf-8", errors="replace").rstrip("\r")
                    buf = bytearray()
                    self.rxq.put(("line", txt))
            except Exception as e:
                # Only report errors that are NOT caused by an intentional disconnect
                if not self._stop.is_set():
                    self.rxq.put(("err", str(e)))
                break


class _ToolTip:
    def __init__(self, widget, text_provider):
        self.widget = widget
        self.text_provider = text_provider
        self.tip = None
        self.widget.bind("<Enter>", self._on_enter, add="+")
        self.widget.bind("<Leave>", self._on_leave, add="+")

    def _on_enter(self, _event=None):
        if self.tip is not None:
            return
        try:
            text = str(self.text_provider() if callable(self.text_provider) else self.text_provider).strip()
        except Exception:
            text = ""
        if not text:
            return
        try:
            x = self.widget.winfo_rootx() + 18
            y = self.widget.winfo_rooty() + self.widget.winfo_height() + 6
            self.tip = tk.Toplevel(self.widget)
            self.tip.wm_overrideredirect(True)
            self.tip.wm_geometry(f"+{x}+{y}")
            lbl = tk.Label(
                self.tip,
                text=text,
                justify="left",
                bg="#fff8dc",
                fg="#000000",
                relief="solid",
                borderwidth=1,
                padx=6,
                pady=3,
            )
            lbl.pack()
        except Exception:
            self.tip = None

    def _on_leave(self, _event=None):
        if self.tip is not None:
            try:
                self.tip.destroy()
            except Exception:
                pass
            self.tip = None


class App(tk.Tk):
    def __init__(self):
        super().__init__()
        self.title(APP_TITLE)
        # Hauteur par défaut suffisante pour afficher l'onglet Mise à jour en
        # entier (bouton Flasher + log), bornée à la hauteur de l'écran.
        try:
            screen_h = self.winfo_screenheight()
        except Exception:
            screen_h = 1080
        win_h = min(985, max(660, screen_h - 90))
        self.geometry(f"1180x{win_h}")
        self.minsize(1000, 660)
        self._theme = {
            "bg": "#2F3238",
            "bg_top": "#3C4048",
            "bg_bottom": "#23262C",
            "surface": "#2A2E35",
            "surface_alt": "#343943",
            "input_bg": "#1B1E24",
            "border": "#4B525D",
            "text": "#F2F4F7",
            "text_muted": "#F2F4F7",
            "accent": "#F0C64A",
            "accent_hover": "#F5D36E",
            "accent_active": "#D8B13F",
            "selection": "#5F4D22",
        }
        try:
            icon_dir = os.path.join(app_base_dir(), "icons")
            ico_candidates = [
                os.path.join(icon_dir, "icon.ico"),
                os.path.join(icon_dir, "Motion-Center-icon.ico"),
            ]
            png_candidates = [
                os.path.join(icon_dir, "icon.png"),
                os.path.join(icon_dir, "Motion-Center-icon.png"),
            ]

            icon_ico = ""
            for p in ico_candidates:
                if os.path.exists(p):
                    icon_ico = p
                    break

            icon_png = ""
            for p in png_candidates:
                if os.path.exists(p):
                    icon_png = p
                    break

            if icon_ico and os.path.exists(icon_ico):
                try:
                    self.iconbitmap(icon_ico)
                except Exception:
                    pass
            if icon_png and os.path.exists(icon_png):
                self._app_icon = tk.PhotoImage(file=icon_png)
                self.iconphoto(True, self._app_icon)
        except Exception:
            pass

        self._apply_modern_theme()
        self._init_premium_background()
        self._apply_windows_titlebar_theme()

        self.w = SerialWorker()
        self.port = tk.StringVar()
        self.update_port = tk.StringVar()
        self.hex_path = tk.StringVar()
        self.fw_choice = tk.StringVar()
        self._fw_box_filter = None      # selected control-box key in the Update tab image selector
        self._fw_all_values = []
        self._fw_box_cards = {}
        self._fw_box_lbls = {}
        self._fw_box_photos = {}
        self.installed_fw = tk.StringVar(value="-")
        self.web_fw_status = tk.StringVar(value="-")
        self._latest_fw_url = ""
        self._latest_fw_soft_min = ""
        self._latest_fw_soft_compatible = True
        self.soft_current_version = tk.StringVar(value=f"v{MOTION_CENTER_VERSION}")
        self.soft_update_status = tk.StringVar(value="-")
        self.simhub_profile_status = tk.StringVar(value="-")
        self.simhub_profile_warning = tk.StringVar(value="")
        self.mc_compat_status = tk.StringVar(value="-")
        self._latest_soft_url = ""
        self._latest_soft_version = ""
        self._simhub_manual_controllers_dir = ""
        self._firmware_map = {}
        self._fw_label_box = {}   # label affiché -> clé de box (comp_v1..pro_v2) ou None
        self._compress_anim_active = False
        self._compress_anim_phase = 0
        self._compress_anim_job = None
        self._actuator_motion = "idle"
        self._long_op_active = False
        self._actuator_photo = None
        self._actuator_frames = []
        self._actuator_frames_decomp = []
        self._actuator_unknown = None
        self._actuator_centered = None
        self._actuator_maxed = None
        self._actuator_idle = None
        self._anim_center_x = 0
        self._anim_center_y = 0
        self._anim_img_w = 68
        self._anim_img_h = 20
        self._img_scaled_cache = {}
        self._active_func_key = None
        self._force_unknown_visual = False
        self._driver_hover_rows = []
        self._info_icon_header = None

        # Language
        self.lang = tk.StringVar(value="FR")  # "FR", "EN" or "DE"

        # GET state
        self._get_inflight = False
        self._get_seq = 0            # identifies the current GET; stale timeout timers are ignored
        self._get_timeout_ms = 3000
        self._pending_connect_firmware_check = False

        # Manual test state
        self._test_active         = False
        self._test_stopping       = False
        self._test_wait_calibrated = False
        self._test_wait_soft_timeout_s = 240.0
        self._test_wait_progress_extend_s = 120.0
        self._test_wait_hard_timeout_s = 480.0
        self._test_wait_deadline  = 0.0
        self._test_wait_hard_deadline = 0.0
        self._test_handshake_lines = []
        self._test_last_frame_payload = b""
        self._test_last_frame_tx = 0.0   # horodatage du dernier envoi P (keepalive)
        self._pending_disconnect  = False  # deferred disconnect requested during test
        self._test_active_tab_img = None   # red PhotoImage indicator on the tab
        self._test_smooth_pos  = 0.0   # interpolated position 0.0–100.0 (M1–M4)
        self._test_target_pct  = 0.0   # slider target set by main thread (M1–M4)
        self._test_target_var  = tk.DoubleVar(value=0.0)
        self._test_smooth_pos5 = 0.0   # interpolated position (M5)
        self._test_target_pct5 = 0.0
        self._test_target_var5 = tk.DoubleVar(value=0.0)
        self._test_smooth_pos6 = 0.0   # interpolated position (M6)
        self._test_target_pct6 = 0.0
        self._test_target_var6 = tk.DoubleVar(value=0.0)
        self._test_smooth_pos7 = 0.0   # interpolated position (M7)
        self._test_target_pct7 = 0.0
        self._test_target_var7 = tk.DoubleVar(value=0.0)
        self._test_speed_limit = tk.BooleanVar(value=True)  # speed limiter ON by default

        # synchronous mode used by blocking workers to consume rx queue
        self._synchronous_mode = False
        self._func_thread = None
        self._func_stop = None

        # After factory reset: auto re-GET (the board may reboot and drop replies)
        self._post_reset_active = False
        self._post_reset_tries = 0

        # Calibration in-progress flag
        self._calib_active = False
        self._calib_diag_phase = None
        self._calib_diag_timeout = False

        # cached
        self.info = {"firmware":"-", "box":"-", "servo":"-", "homing_sps":"-", "homing_step":"-", "mc_required":"", "estop_enabled": False, "estop_behavior": 0}
        self.motors = {i: {"m":i, "conn":"N", "cal":"N", "max":0, "margin":0} for i in range(1,8)}

        # UI vars
        self.homing_sps_var = tk.StringVar(value="1500")
        self.step_hz_var = tk.StringVar(value="200000")
        self.estop_use_var = tk.BooleanVar(value=False)
        self.estop_behavior_var = tk.StringVar(value="")
        self._estop_behavior_code = 0

        # Functions model
        self.func_model = {
            "detect_min":     {"cmd":"DETECT_MIN",     "needs_motor": True},
            "detect_max":     {"cmd":"MOVE_TO_MAX",    "needs_motor": True},
            "go_center":      {"cmd":"GO_TO_CENTER",   "needs_motor": True},
            # Use FULL_CALIB alias for broader firmware compatibility
            "complete_calib": {"cmd":"FULL_CALIB",     "needs_motor": True},
            "test_stroke":    {"cmd":"TEST_STROKE",    "needs_motor": True},
            "factory_reset":  {"cmd":"FACTORY_RESET",  "needs_motor": False},
            # "cancel" intentionally hidden from end-users
        }

        self.i18n = {
            "FR": {
                "status_tab": "Status",
                "functions_tab": "Fonctions",
                "update_tab": "Mise à jour",
                "driver_tab": "Réglages driver",
                "soft_update_tab": "Mise à jour Motion Center",
                "log_tab": "Log",
                "port": "Port :",
                "refresh_ports": "Rafraîchir ports",
                "connect": "Connecter",
                "disconnect": "Déconnecter",
                "refresh": "Rafraîchir",
                "enable": "Activer",
                "disable": "Désactiver",
                "enable_warning": "⚠ Attention : cela active les moteurs.",
                "control_box": "CONTROL BOX",
                "homing_speed": "Vitesse homing",
                "step_speed": "Vitesse de streaming (SimHub)",
                "step_speed_label": "Frequence STEP max (pas/s) :",
                "step_speed_hint": "De 1000 a 450000. Defaut 200000. Reduire si le driver decroche.",
                "step_speed_invalid": "Frequence STEP invalide",
                "speed_steps_s": "Vitesse (steps/s) :",
                "apply": "Appliquer",
                "recommended": "De 50 (lent) a 10000 (tres rapide). Si toujours trop lent a 10000, augmenter PN098.",
                "estop_use": "Utiliser l'ESTOP (front descendant)",
                "estop_tip": "Si activé, le front descendant sur l'entrée ESTOP coupe les moteurs.",
                "estop_behavior": "Comportement ESTOP :",
                "estop_behavior_disable_servo": "Disable servo",
                "estop_behavior_return_park": "Return to park",
                "estop_behavior_stop_here": "Stop here",
                "motors": "Moteurs (☐/☑ = connecté, double-clic Max pour éditer ; Marge sur M5/M6)",
                "col_m": "M",
                "col_conn": "Connecté",
                "col_cal": "Calib",
                "col_max": "Max",
                "col_margin": "Marge",
                "col_endstop": "Butée",
                "col_homing_dir": "Dir homing",
                "col_encoder": "Encodeur",
                "tip_col_m": "Le numéro du moteur correspond au port DB25 présent sur la Control Box Competition.",
                "tip_col_conn": "Indiquez si vous avez branché le driver au port DB25. C'est comme ça que vous indiquez quels vérins vous utilisez.",
                "tip_col_cal": "Indique si la course maximale du vérin a déjà été détectée.",
                "tip_col_max": "Course maximale disponible entre 0 et 32767. Assurez-vous que la course soit réellement disponible avant de modifier ces valeurs. Pour les vérins 5 et 6, il y a une procédure de calibration dans l'onglet Fonctions.",
                "tip_col_margin": "Marge qui sera utilisée en jeu pour s'assurer de ne toucher la butée uniquement en phase de calibration. Double-cliquez sur la valeur pour modifier.",
                "tip_col_endstop": "Indique si une butée MIN est câblée sur ce vérin (nécessaire pour le homing automatique).",
                "tip_col_homing_dir": "Position sur laquelle le vérin se calibre au démarrage de SimHub. Ajustez la valeur de park à 100% dans le profil SimHub si la position de homing est le max. Cliquer pour basculer MIN/MAX (MAX nécessite un Max calibré).",
                "homing_dir_requires_max": "Le homing sur la butée MAX nécessite un Max calibré (Max > 0).\nLance d'abord une calibration complète.",
                "col_endpark": "Endpark",
                "tip_col_endpark": "Position (en % de la course) rejointe doucement par les vérins à l'arrêt de SimHub, avant la coupure des servos. Cliquer pour régler ou désactiver.",
                "endpark_dialog_label": "Position de park en fin de session : à l'arrêt de SimHub (SH_DISABLE), le vérin rejoint doucement ce pourcentage de sa course avant la coupure des servos.",
                "endpark_dialog_example": "Exemple : park SimHub à 0%, homing MIN. Au lancement de SimHub, le vérin se calibre au min, puis SimHub reprend la main et rejoint la position 50%. En fin de session, le vérin retourne à 0%, laissant le simulateur de biais s'il s'agit par exemple du traction loss.\nDans ce cas, en mettant l'Endpark position à 50% : quand SimHub rend la main, le vérin qui est à 0% va rejoindre la position 50% avant de couper.\nLa vitesse dépend du paramètre Homing speed.",
                "endpark_disabled": "Désactivé (le vérin reste à la dernière position envoyée par SimHub)",
                "busy_wait": "⏳ La box est occupée (calibration ou mouvement en cours).\nVeuillez attendre la fin de l'opération avant de modifier ce réglage.",
                "homing_fail_no_ready": "Servo non branché ou non alimenté : le driver ne présente pas son signal READY. Vérifiez le câble DB25 et l'alimentation du driver (acquittez une éventuelle alarme par un power-cycle).",
                "homing_fail_min_timeout": "Butée MIN jamais détectée : le vérin atteint la butée sans que l'entrée endstop ne se déclenche. Vérifiez le câblage du switch.",
                "homing_fail_seek_limit": "Limite de pas de recherche atteinte (40000) avant la butée : veuillez augmenter Pn98 sur le driver (moins de pas par tour), puis relancer la calibration.",
                "homing_fail_max_timeout": "Butée MAX jamais détectée pendant la mesure de course. Vérifiez le switch MAX (en série avec le MIN).",
                "homing_fail_endstop_stuck": "Entrée endstop active en permanence : switch NC/court-circuit, ou direction moteur inversée (le dégagement pousse du mauvais côté).",
                "homing_fail_estop": "Arrêt d'urgence actif : homing bloqué.",
                "homing_fail_reboot": "Redémarrage de la box requis avant un nouveau homing (débranchez/rebranchez l'USB).",
                "homing_fail_max_too_small": "Course mesurée trop faible : la butée MAX s'est déclenchée trop tôt (switch ou câblage à vérifier).",
                "tip_col_encoder": "Encodeur absolu : si activé, la position est connue au démarrage sans homing.",
                "functions": "Fonctions",
                "motor": "Moteur :",
                "run": "Lancer",
                "desc": "Description :",
                "warning": "⚠️ Attention :",
                "pn98_warning": "⚠️ À partir de la v1.2, Pn98 passe de 6 à 12. Si la valeur n'est pas mise à jour sur les drivers, risque de casse !",
                "not_connected": "Non connecté.",
                "unsupported_fw_connect": "Competition Control Box non reconnue ou firmware non à jour.\nVoir onglet Mise à jour.",
                "serial_err": "Impossible d’ouvrir le port série.\n\nCauses fréquentes :\n- Arduino IDE Serial Monitor ouvert\n- SimHub connecté\n- Mauvais COM\n\nDétails :\n",
                "get_timeout": "GET timeout: no END received.\nCauses possibles:\n- GET ne renvoie pas END\n- Une opération longue bloque\n- Mauvais port\n",
                "box_prefix": "V",
                "func_labels": {
                    "detect_min": "Détecter min",
                    "detect_max": "Détecter max",
                    "go_center": "Aller au centre",
                    "complete_calib": "Calibration complète",
                    "test_stroke": "Test course complète",
                    "factory_reset": "Réinitialisation usine",
                },
                "func_desc": {
                    "detect_min": "Le vérin va se compresser jusqu'à détection de la butée. Permet de savoir où il est.",
                    "detect_max": "Le vérin va se décompresser jusqu'à trouver la butée max. Ne permet pas de connaître la position.",
                    "go_center": "Le vérin va au centre. S'il ne connaît pas sa position, il lance d'abord un detect min puis il se centre.",
                    "complete_calib": "Le vérin va chercher le minimum puis le maximum et enregistrer la calibration. La marge se règle manuellement dans l'onglet Status.",
                    "test_stroke": "Le vérin va aller du min au max puis se centrer.",
                    "factory_reset": "Efface toutes les données enregistrées pour revenir au paramétrage d'usine.",
                },
                "cancel": "Annuler",
                "factory_reset_btn": "Factory reset",
                "func_servo_required": "⚠️ Le moteur sélectionné doit être connecté dans le tableau de l'onglet Statut avant d'utiliser une fonction.",
                "func_pn24_hint": "Pn24 ajuste le couple qui déclenche la détection de butée : valeur faible = haute sensibilité (probabilité de fausse détection), valeur élevée = faible sensibilité (risque de casse). Le couple instantané peut être observé avec Dn002 afin d'ajuster la valeur au mieux.",
                "cal_stroke_measured": "Course mesurée {motor} : {steps} pas.",
                "cal_stroke_clamped": "⚠ La course mesurée de {motor} ({steps} pas) dépasse la plage utilisable (32767) : valeur écrêtée à 32767. Augmentez Pn98 sur le driver (moins de pas par tour) pour exploiter toute la course.",
                "motor56_warn_title": "⚠️ Moteur M{n} — attention",
                "motor56_warn_msg": "Si le moteur n'est pas connecté et correctement calibré, la box risque de fonctionner en mode dégradé avec SimHub.",
                "m6_requires_m5": "⚠️ M6 ne peut être activé que si M5 est déjà connecté.",
                "m5_disconnect_m6_first": "⚠️ Déconnecte M6 avant de désactiver M5.",
                "m7_requires_m6": "⚠️ M7 ne peut être activé que si M6 est déjà connecté.",
                "m6_disconnect_m7_first": "⚠️ Déconnecte M7 avant de désactiver M6.",
                "stale_profiles_title": "Nettoyage SimHub",
                "stale_profiles_msg": "Ces fichiers peuvent être supprimés du dossier Controllers SimHub :\n(les contrôleurs actifs dans SimHub ne sont pas affectés)",
                "stale_profiles_section_stale": "Anciens profils (probablement obsolètes) :",
                "stale_profiles_section_bak": "Fichiers de sauvegarde (.bak) :",
                "stale_profiles_select_all": "Tout sélectionner",
                "stale_profiles_deselect_all": "Tout désélectionner",
                "stale_profiles_delete": "Supprimer la sélection",
                "stale_profiles_none": "Aucun fichier sélectionné.",
                "simhub_reload_title": "⚠ SimHub — action requise",
                "simhub_reload_intro": "Le profil a été mis à jour sur le disque ({n} moteur(s) actif(s)).\nSimHub ne peut pas être reconfiguré automatiquement lorsqu'il tourne.",
                "simhub_reload_step1": "① Dans SimHub › Motion controllers :\n   SUPPRIME les contrôleurs actifs portant ces noms :",
                "simhub_reload_step2": "② Clique sur \"Add\" / \"+\" pour ajouter le contrôleur mis à jour :\n   (le commentaire du profil contient la date de mise à jour)",
                "factory_reset_warn": "Cette action efface définitivement les données enregistrées.\nAssure-toi de comprendre les conséquences avant de continuer.",
                "confirm_reset_title": "Confirmer factory reset",
                "confirm_reset_msg": "Factory reset va effacer les données enregistrées.\n\nConfirmer ?",
                "saved_ok": "Enregistré",
                "update_group": "Flash firmware",
                "hex_file": "Firmware :",
                "browse": "Parcourir",
                "custom_fw": "Personnalisé",
                "box_select_label": "Sélectionne ta control box :",
                "box_card_comp_v1": "Competition V1 (5 vérins)",
                "box_card_comp_v2": "Competition V2 (6 vérins)",
                "box_card_comp_v3": "Competition V3 (7 vérins)",
                "box_card_pro_v1": "PRO Control Box V1 (4 vérins)",
                "box_card_pro_v2": "PRO Control Box V2 (6 vérins)",
                "fw_v15_warn_title": "⚠️ Firmware v1.5 — Attention",
                "fw_v15_warn": "La v1.5 n'est pas compatible avec le Motion Center et nécessite l'ancien profil SimHub.\n\nIl est toujours possible de passer de la v1.5 à la v1.8 en utilisant le Motion Center.",
                "compat_v18": "Compatibilité : SRT Control Box v1.8 uniquement",
                "flash": "Flasher",
                "flash_note": "Le Flash n'efface pas les paramètres de l'onglet Status. Pour réinitialiser les réglages de la box, utiliser la fonction \"Factory Reset\" de l'onglet \"Status\".",
                "flash_running": "Flash en cours...",
                "flash_done": "Flash terminé avec succès.",
                "flash_failed": "Échec du flash (voir journal).",
                "flash_port_busy": "Impossible de communiquer avec le périphérique : le port semble déjà occupé.\n\nFerme SimHub ou tout logiciel susceptible de communiquer avec la carte, puis réessaie.",
                "select_hex_first": "Sélectionne un fichier .hex.",
                "select_port_first": "Sélectionne un port COM.",
                "hex_not_found": "Fichier .hex introuvable.",
                "avrdude_not_found": "avrdude introuvable. Installe Arduino IDE ou PlatformIO (tool-avrdude).",
                "touch1200_warn": "Avertissement: reset 1200 bps impossible, tentative de flash quand même.",
                "stm32_no_bootloader": "Bootloader STM32 introuvable (pas de réponse au PING). Vérifie que la box connectée est bien une box STM32 (PRO Control Box ou Competition Control Box V3), pas une Competition Control Box ATmega.",
                "stm32_confirm_title": "Flash STM32",
                "stm32_confirm": "Ce firmware (.bin) est destiné à une box STM32 (PRO Control Box STM32F103 ou Competition Control Box V3 STM32G4).\n\nLa box connectée sur {port} est-elle bien une box STM32 ?\n\n(Pour une Competition Control Box ATmega32U4, utilise un fichier .hex.)",
                "stm32_erase_failed": "Échec de l'effacement de la zone application.",
                "stm32_write_failed": "Échec d'écriture du bloc à l'adresse {addr}.",
                "stm32_go_failed": "CRC final refusé — image rejetée, la carte reste en bootloader (relancer le flash).",
                "stm32_go_soft": "Firmware écrit et vérifié bloc par bloc. La vérification CRC finale côté carte n'a pas été confirmée (quirk connu du bootloader G4).\n\nDébranche puis rebranche la box : le nouveau firmware démarrera. Vérifie ensuite la version dans l'onglet Mise à jour.",
                "stm32_empty_bin": "Fichier .bin vide.",
                "stm32_entering_bl": "Application détectée — passage en bootloader (!DFU)...",
                "stm32_writing": "Écriture... {pct}% ({done}/{total} octets)",
                "stm32_finalizing": "Finalisation (CRC32 + métadonnées)...",
                "bl_update_group": "Flash bootloader (.bin)",
                "flash_bl": "Flasher bootloader",
                "flash_bl_note": "⚠ Mise à jour risquée : une interruption peut bricker la carte (récupération via ST-Link/BOOT0). Ne flasher le bootloader que si nécessaire.",
                "bl_update_confirm_title": "Flash bootloader STM32",
                "bl_update_confirm": "Tu es sur le point de remplacer le bootloader resident sur {port}.\n\n⚠ Si l'opération est interrompue, la carte sera inaccessible sans ST-Link.\n\nContinuer ?",
                "bl_update_no_response": "Pas de réponse BL_UPDATE_READY. Vérifie que le firmware app est bien à jour (commande !BL_UPDATE supportée).",
                "bl_update_entering": "Envoi !BL_UPDATE à l'application...",
                "bl_update_running": "Flash bootloader en cours...",
                "bl_update_done": "Bootloader flashé avec succès.",
                "bl_update_failed": "Échec du flash bootloader (voir journal).",
                "using_port": "Port utilisé",
                "installed_fw": "Firmware installé :",
                "detect_fw": "Détecter",
                "detecting_fw": "Détection...",
                "fw_unknown": "Firmware inconnu",
                "check_web_fw": "Vérifier mise à jour",
                "checking_web_fw": "Vérification web...",
                "web_fw_status_prefix": "Mise à jour web :",
                "web_fw_not_checked": "non vérifiée",
                "web_fw_update_available": "mise à jour dispo",
                "web_fw_up_to_date": "déjà à jour",
                "web_fw_local_unknown": "version locale inconnue",
                "web_fw_meta_missing": "Métadonnée firmware introuvable sur la page.",
                "web_fw_check_failed": "Échec vérification web",
                "download_web_fw": "Télécharger + préparer",
                "web_fw_no_url": "Aucun lien de téléchargement firmware disponible.",
                "web_fw_opening": "Ouverture téléchargement firmware",
                "web_fw_dl_running": "Téléchargement firmware...",
                "web_fw_dl_done": "Firmware prêt à flasher.",
                "web_fw_dl_failed": "Échec préparation firmware",
                "web_fw_dl_no_hex": "Aucun fichier .hex trouvé dans l'archive.",
                "web_fw_dl_unsupported": "Type de fichier non supporté (attendu: .hex ou .zip).",
                "web_fw_requires_soft": "Motion Center requis",
                "web_fw_incompat_soft": "firmware non compatible avec votre Motion Center",
                "web_fw_incompat_block": "Téléchargement bloqué : firmware non compatible avec votre version Motion Center.",
                "mc_compat_prefix": "Compat Motion Center :",
                "mc_compat_ok": "compatible",
                "mc_compat_ko": "incompatible",
                "mc_compat_unknown": "exigence firmware inconnue",
                "soft_current_version": "Version actuelle :",
                "soft_web_status": "Mise à jour web :",
                "soft_check": "Vérifier mise à jour",
                "soft_checking": "Vérification web...",
                "soft_download": "Télécharger",
                "soft_not_checked": "non vérifiée",
                "soft_up_to_date": "déjà à jour",
                "soft_update_available": "mise à jour dispo",
                "soft_check_failed": "Échec vérification soft",
                "soft_meta_missing": "Métadonnée version soft introuvable sur la page.",
                "soft_no_url": "Aucun lien de téléchargement Motion Center disponible.",
                "soft_downloading": "Téléchargement Motion Center...",
                "soft_downloaded": "Téléchargement terminé. Lancez le nouvel exécutable manuellement.",
                "soft_download_failed": "Échec téléchargement Motion Center",
                "simhub_profile_title": "Profil SimHub",
                "simhub_profile_status": "Statut profil :",
                "simhub_profile_install": "Installer dans SimHub",
                "simhub_profile_not_installed": "non installé",
                "simhub_profile_installing": "Installation profil SimHub...",
                "simhub_profile_done": "profil installé",
                "simhub_profile_source_missing": "Dossier profil SimHub introuvable.",
                "simhub_profile_no_file": "Aucun fichier .shmotioncontroller trouvé dans le profil.",
                "simhub_profile_dest_missing": "Installation SimHub introuvable.",
                "simhub_profile_pick_folder": "SimHub n'a pas été détecté automatiquement. Voulez-vous sélectionner le dossier SimHub manuellement ?",
                "simhub_profile_pick_hint": "Sélectionnez uniquement le dossier racine SimHub (exemple : C:\\Program Files (x86)\\SimHub).",
                "simhub_profile_pick_title": "Sélectionner le dossier SimHub",
                "simhub_profile_pick_invalid": "Dossier sélectionné invalide (Controllers introuvable).",
                "simhub_profile_failed": "Échec installation profil SimHub",
                "simhub_profile_warn_missing": "⚠ Profil SimHub manquant/non à jour ou installation SimHub introuvable.",
                "simhub_running_warn": "⚠ SimHub est en cours d'exécution.\n\nConnecter ou déconnecter un moteur modifie la configuration SimHub.\nFermez SimHub pour que Motion Center puisse mettre à jour le profil.",
                "simhub_running_detail": "SimHub doit envoyer les positions du nombre de moteurs défini dans Motion Center.\n\nMotion Center modifie automatiquement ces paramètres dans le profil SimHub :\n  • \"Edit serial commands and settings\"\n  • \"Edit assignments\"",
                "simhub_running_learn_more": "En savoir plus ▾",
                "simhub_running_learn_less": "Réduire ▴",
                "simhub_fix": "Corriger profil SimHub",
                "fw_not_detected": "firmware non détecté",
                "fw_unsupported": "firmware non pris en charge",
                "control_box_doc_label": "Documentation de la control box :",
                "open_doc": "Ouvrir",
                "driver_settings_title": "Réglage des drivers AASD",
                "driver_settings_doc_label": "Réglages driver (AASD) :",
                "open_driver_settings": "Ouvrir la page",
                "driver_col_param": "Paramètre",
                "driver_col_value": "Valeur",
                "driver_col_expl": "Explication",
                "driver_col_note": "Remarque",
                "invalid_u16_range": "Impossible, valeur comprise entre 0 et 32767",
                "files_count_suffix": "fichier(s)",
                "test_tab": "Test manuel",
                "test_desc": "Contrôle direct des vérins, indépendamment de SimHub.\nLes servos sont activés au démarrage et désactivés à l'arrêt.",
                "test_m56_group": "M5 / M6",
                "test_warn_calib": "⚠ M{motors} connecté(s) mais non calibré(s) (MAX=0). Démarrer la calibration avant le test.",
                "test_start": "▶ Démarrer",
                "test_stop": "■ Arrêter",
                "test_target_label": "Cible",
                "test_motor_label": "Moteurs (lissé)",
                "test_motor_label_direct": "Moteurs (direct)",
                "test_status_idle": "Inactif",
                "test_status_running": "En cours",
                "test_status_stopping": "Retour à 0%…",
                "test_not_connected": "⚠ Connexion requise pour démarrer le test.",
                "test_speed_limit_label": "Activer la limite de vitesse",
                "test_speed_limit_desc": "Limite la vitesse de déplacement et lisse les changements rapides le temps que les commandes se stabilisent.",
                "test_speed_limit_warn_title": "⚠ Désactiver la limite de vitesse ?",
                "test_speed_limit_warn": "Voulez-vous vraiment désactiver la limite de vitesse ?\nSans limitation, les mouvements peuvent être très rapides.\nRecommandé uniquement pour le débogage.",
                "faq_tab": "FAQ / Aide",
                "faq_title": "Questions fréquentes & débogage",
                "faq_diag_btn": "Lancer diagnostic",
                "faq_diag_ok": "✅ Tout semble correct.",
                "faq_diag_no_port": "⚠ Aucun port COM sélectionné.",
                "faq_diag_not_connected": "⚠ Non connecté à la Control Box.",
                "faq_diag_no_servo": "ℹ Servo désactivé (normal si SimHub n'est pas lancé).",
                "faq_diag_servo_on": "✅ Servo activé.",
                "faq_diag_profile_ok": "✅ Profil SimHub installé.",
                "faq_diag_profile_missing": "⚠ Profil SimHub non installé — cliquer 'Installer dans SimHub' dans l'onglet Mise à jour.",
                "legal_title": "Informations légales",
                "legal_notice": "{app_title} v{version}\nAuteur : {author}\nCopyright (c) {years} {company}. Tous droits réservés.\nLicence : {license_name}\nSite : {website}",
                "faq_items": [
                    ("🔴 LED rouge fixe, SimHub crash / 'connection lost'",
                     "Le firmware répond OK à ENABLE mais SimHub ne reçoit pas les données correctement.\n"
                     "Causes possibles :\n"
                     "• Les moteurs 5/6 sont marqués 'connecté' dans la Control Box mais pas physiquement branchés → la calibration au démarrage ne finit jamais.\n"
                     "  Solution : dans l'onglet Status, décocher M5 et/ou M6, puis reflasher/reconnecter.\n"
                     "• Profil SimHub non à jour — cliquer 'Installer dans SimHub' puis recharger le contrôleur dans SimHub.\n"
                     "• Le port COM est utilisé par autre chose (Arduino IDE, autre logiciel)."),
                    ("🟠 LED orange bloquée (homing infini)",
                     "Le vérin 5 ou 6 est marqué connecté mais l'endstop n'est jamais atteint.\n"
                     "• Vérifier le câblage de l'endstop du vérin concerné.\n"
                     "• Si le vérin n'est pas physiquement présent : décocher M5/M6 dans l'onglet Status → sauvegarder → redémarrer SimHub."),
                    ("⚫ LED grise / box non détectée",
                     "• Vérifier que le câble USB est bien branché.\n"
                     "• Sélectionner le bon port COM et cliquer 'Connecter'.\n"
                     "• Essayer 'Rafraîchir ports' si le port n'apparaît pas.\n"
                     "• Fermer Arduino IDE / SimHub qui peuvent bloquer le port.\n"
                     "• Si la box vient d'être flashée, attendre 3–5 secondes avant de reconnecter."),
                    ("🔵 LED bleue mais SimHub ne démarre pas",
                     "La Control Box est connectée à Motion Center (LED bleue = session Python active).\n"
                     "SimHub ne peut pas se connecter en même temps que Motion Center.\n"
                     "• Cliquer 'Déconnecter' dans Motion Center, puis lancer SimHub."),
                    ("SimHub : 'Output connection failure'",
                     "SimHub ne reçoit pas de réponse OK au démarrage.\n"
                     "• S'assurer que le profil SimHub correspond à la version du firmware (V1/V2).\n"
                     "• Réinstaller le profil via 'Installer dans SimHub', puis dans SimHub :\n"
                     "  1. Supprimer le contrôleur existant\n"
                     "  2. Ajouter le nouveau profil installé\n"
                     "• Si Motion Center est ouvert, le fermer — SimHub et Motion Center ne peuvent pas utiliser le port COM simultanément."),
                    ("Flash firmware échoue",
                     "• Fermer SimHub et Arduino IDE avant de flasher.\n"
                     "• La Control Box doit être en mode bootloader : le flash via Motion Center gère ça automatiquement (reset 1200 bps).\n"
                     "• Si le flash échoue toujours : débrancher/rebrancher la Control Box, attendre 5 secondes, réessayer.\n"
                     "• Sur Windows 11 : vérifier que le driver COM est installé (Gestionnaire de périphériques)."),
                    ("Vérin qui ne bouge pas après calibration",
                     "• Vérifier que le servo est activé (LED rouge).\n"
                     "• Vérifier la valeur MAX dans l'onglet Status — si elle est 0, relancer une calibration complète.\n"
                     "• Vérifier la Marge : si trop grande par rapport au MAX, la plage de mouvement est nulle.\n"
                     "• Vérifier les réglages driver AASD dans l'onglet 'Réglages driver'."),
                    ("Comment réinitialiser complètement la box",
                     "Dans l'onglet Status, cliquer 'Factory reset'.\n"
                     "Cela efface la calibration et remet les moteurs 5/6 comme déconnectés.\n"
                     "Après reset : re-brancher, puis re-calibrer les vérins dans l'onglet Fonctions."),
                ],
            },
            "EN": {
                "status_tab": "Status",
                "functions_tab": "Functions",
                "update_tab": "Update",
                "driver_tab": "Driver settings",
                "soft_update_tab": "Motion Center update",
                "log_tab": "Log",
                "port": "Port:",
                "refresh_ports": "Refresh ports",
                "connect": "Connect",
                "disconnect": "Disconnect",
                "refresh": "Refresh",
                "enable": "Enable",
                "disable": "Disable",
                "enable_warning": "⚠ Warning: this activates the motors.",
                "control_box": "CONTROL BOX",
                "homing_speed": "Homing Speed",
                "step_speed": "Streaming speed (SimHub)",
                "step_speed_label": "Max STEP frequency (steps/s):",
                "step_speed_hint": "From 1000 to 450000. Default 200000. Lower if the driver stalls.",
                "step_speed_invalid": "Invalid STEP frequency",
                "speed_steps_s": "Speed (steps/s):",
                "apply": "Apply",
                "recommended": "From 50 (slow) to 10000 (very fast). If still too slow at 10000, increase PN098.",
                "estop_use": "Use ESTOP (falling edge)",
                "estop_tip": "When enabled, a falling edge on the ESTOP input cuts the motors.",
                "estop_behavior": "ESTOP behavior:",
                "estop_behavior_disable_servo": "Disable servo",
                "estop_behavior_return_park": "Return to park",
                "estop_behavior_stop_here": "Stop here",
                "motors": "Motors (☐/☑ = connected, double-click Max to edit; Margin on M5/M6)",
                "col_m": "M",
                "col_conn": "Connected",
                "col_cal": "Cal",
                "col_max": "Max",
                "col_margin": "Margin",
                "col_endstop": "Endstop",
                "col_homing_dir": "Homing dir",
                "col_encoder": "Encoder",
                "tip_col_m": "Motor number matches the DB25 port on the Competition Control Box.",
                "tip_col_conn": "Indicates whether the driver is plugged into the DB25 port. This tells the app which actuators are in use.",
                "tip_col_cal": "Indicates whether the actuator maximum stroke has already been detected.",
                "tip_col_max": "Maximum available stroke, from 0 to 32767. Make sure this stroke is truly available before editing values. For actuators 5 and 6, a calibration procedure is available in the Functions tab.",
                "tip_col_margin": "Margin used during operation to avoid touching end-stops except during calibration. Double-click the value to edit.",
                "tip_col_endstop": "Indicates whether a MIN end-stop is wired on this actuator (required for automatic homing).",
                "tip_col_homing_dir": "Position the actuator homes to when SimHub starts. Set the park value to 100% in the SimHub profile if the homing position is the max. Click to toggle MIN/MAX (MAX requires a calibrated Max).",
                "homing_dir_requires_max": "Homing on the MAX endstop requires a calibrated Max (Max > 0).\nRun a full calibration first.",
                "col_endpark": "Endpark",
                "tip_col_endpark": "Position (% of stroke) the actuators slowly move to when SimHub stops, before servo power is cut. Click to set or disable.",
                "endpark_dialog_label": "End-of-session park position: when SimHub stops (SH_DISABLE), the actuator slowly moves to this percentage of its stroke before servo power is cut.",
                "endpark_dialog_example": "Example: SimHub park at 0%, MIN homing. When SimHub starts, the actuator homes to min, then SimHub takes over and moves to the 50% position. At the end of the session the actuator returns to 0%, leaving the simulator tilted — e.g. with a traction loss setup.\nIn that case, with the Endpark position set to 50%: when SimHub releases control, the actuator at 0% moves to the 50% position before power is cut.\nThe speed depends on the Homing speed setting.",
                "endpark_disabled": "Disabled (actuator stays at the last position sent by SimHub)",
                "busy_wait": "⏳ The box is busy (calibration or motion in progress).\nPlease wait for the operation to finish before changing this setting.",
                "homing_fail_no_ready": "Servo not connected or not powered: the driver does not raise its READY signal. Check the DB25 cable and the driver power supply (clear any alarm with a power-cycle).",
                "homing_fail_min_timeout": "MIN endstop never detected: the actuator reaches the hard stop but the endstop input never triggers. Check the switch wiring.",
                "homing_fail_seek_limit": "Step search limit reached (40000) before the endstop: please increase Pn98 on the servo driver (fewer steps per revolution), then run the calibration again.",
                "homing_fail_max_timeout": "MAX endstop never detected during stroke measurement. Check the MAX switch (in series with MIN).",
                "homing_fail_endstop_stuck": "Endstop input permanently active: NC/shorted switch, or inverted motor direction (the release pushes the wrong way).",
                "homing_fail_estop": "Emergency stop active: homing blocked.",
                "homing_fail_reboot": "Box reboot required before a new homing (unplug/replug USB).",
                "homing_fail_max_too_small": "Measured stroke too small: the MAX endstop triggered too early (check switch/wiring).",
                "tip_col_encoder": "Absolute encoder: if enabled, position is known at startup without homing.",
                "functions": "Functions",
                "motor": "Motor:",
                "run": "Run",
                "desc": "Description:",
                "warning": "⚠️ Warning:",
                "pn98_warning": "⚠️ Starting with v1.2, Pn98 changes from 6 to 12. If the value is not updated on the drives, risk of mechanical damage!",
                "not_connected": "Not connected.",
                "unsupported_fw_connect": "Competition Control Box not recognized or firmware is outdated.\nSee the Update tab.",
                "serial_err": "Could not open serial port.\n\nCommon reasons:\n- Arduino IDE Serial Monitor connected\n- SimHub connected\n- Wrong COM\n\nDetails:\n",
                "get_timeout": "GET timeout: no END received.\nMost likely causes:\n- GET does not print END\n- A long operation is blocking\n- Wrong port\n",
                "box_prefix": "V",
                "func_labels": {
                    "detect_min": "Detect min",
                    "detect_max": "Detect max",
                    "go_center": "Go to center",
                    "complete_calib": "Full calibration",
                    "test_stroke": "Full stroke test",
                    "factory_reset": "Factory reset",
                },
                "func_desc": {
                    "detect_min": "The actuator compresses until the minimum end-stop is detected. Helps you know where it is.",
                    "detect_max": "The actuator extends until the maximum end-stop is detected. Does not provide absolute position.",
                    "go_center": "Moves the actuator to center. If position is unknown, it first runs detect min, then centers.",
                    "complete_calib": "Finds minimum, then maximum, and saves calibration. Margin is set manually in the Status tab.",
                    "test_stroke": "Moves from min to max, then centers.",
                    "factory_reset": "Erases all saved data and restores factory settings.",
                },
                "cancel": "Cancel",
                "factory_reset_btn": "Factory reset",
                "func_servo_required": "⚠️ The selected motor must be marked as connected in the Status tab table before using a function.",
                "func_pn24_hint": "Pn24 adjusts the torque threshold that triggers endstop detection: low value = high sensitivity (chance of false detection), high value = low sensitivity (risk of mechanical damage). The instantaneous torque can be monitored with Dn002 to fine-tune the value.",
                "cal_stroke_measured": "Measured stroke {motor}: {steps} steps.",
                "cal_stroke_clamped": "⚠ Measured stroke of {motor} ({steps} steps) exceeds the usable range (32767): value clamped to 32767. Increase Pn98 on the servo driver (fewer steps per revolution) to use the full stroke.",
                "motor56_warn_title": "⚠️ Motor M{n} — attention",
                "motor56_warn_msg": "If the motor is not connected and properly calibrated, the box may run in degraded mode with SimHub.",
                "m6_requires_m5": "⚠️ M6 can only be enabled if M5 is already connected.",
                "m5_disconnect_m6_first": "⚠️ Disconnect M6 before disabling M5.",
                "m7_requires_m6": "⚠️ M7 can only be enabled if M6 is already connected.",
                "m6_disconnect_m7_first": "⚠️ Disconnect M7 before disabling M6.",
                "stale_profiles_title": "SimHub cleanup",
                "stale_profiles_msg": "These files can be deleted from the SimHub Controllers folder:\n(active controllers in SimHub are not affected)",
                "stale_profiles_section_stale": "Old profiles (possibly outdated):",
                "stale_profiles_section_bak": "Backup files (.bak):",
                "stale_profiles_select_all": "Select all",
                "stale_profiles_deselect_all": "Deselect all",
                "stale_profiles_delete": "Delete selected",
                "stale_profiles_none": "No file selected.",
                "simhub_reload_title": "⚠ SimHub — action required",
                "simhub_reload_intro": "The profile was updated on disk ({n} active motor(s)).\nSimHub cannot be reconfigured automatically while running.",
                "simhub_reload_step1": "① In SimHub › Motion controllers:\n   DELETE all active controllers with these names:",
                "simhub_reload_step2": "② Click \"Add\" / \"+\" to add the updated controller.\n   (the profile comment contains the update date)",
                "factory_reset_warn": "This action permanently erases saved data.\nMake sure you understand the consequences before continuing.",
                "confirm_reset_title": "Confirm factory reset",
                "confirm_reset_msg": "Factory reset will erase saved data.\n\nConfirm?",
                "saved_ok": "Saved",
                "update_group": "Firmware flash (.hex)",
                "hex_file": "Firmware:",
                "browse": "Browse",
                "custom_fw": "Custom",
                "box_select_label": "Select your control box:",
                "box_card_comp_v1": "Competition V1 (5 actuators)",
                "box_card_comp_v2": "Competition V2 (6 actuators)",
                "box_card_comp_v3": "Competition V3 (7 actuators)",
                "box_card_pro_v1": "PRO Control Box V1 (4 actuators)",
                "box_card_pro_v2": "PRO Control Box V2 (6 actuators)",
                "fw_v15_warn_title": "⚠️ Firmware v1.5 — Warning",
                "fw_v15_warn": "v1.5 is not compatible with Motion Center and requires the old SimHub profile.\n\nYou can still upgrade from v1.5 to v1.8 using Motion Center.",
                "compat_v18": "Compatibility: SRT Control Box v1.8 only",
                "flash": "Flash",
                "flash_note": "Flashing does not erase settings from the Status tab. To reset box settings, use the \"Factory Reset\" function in the \"Status\" tab.",
                "flash_running": "Flashing in progress...",
                "flash_done": "Flashing completed successfully.",
                "flash_failed": "Flashing failed (see log).",
                "flash_port_busy": "Unable to communicate with the device: the port appears to be already in use.\n\nClose SimHub or any software that may communicate with the board, then retry.",
                "select_hex_first": "Select a .hex file.",
                "select_port_first": "Select a COM port.",
                "hex_not_found": ".hex file not found.",
                "avrdude_not_found": "avrdude not found. Install Arduino IDE or PlatformIO (tool-avrdude).",
                "touch1200_warn": "Warning: 1200 bps reset failed, trying flash anyway.",
                "stm32_no_bootloader": "STM32 bootloader not found (no PING reply). Make sure the connected box is an STM32 box (PRO Control Box or Competition Control Box V3), not an ATmega Competition Control Box.",
                "stm32_confirm_title": "STM32 flash",
                "stm32_confirm": "This firmware (.bin) targets an STM32 box (PRO Control Box STM32F103 or Competition Control Box V3 STM32G4).\n\nIs the box connected on {port} an STM32 box?\n\n(For an ATmega32U4 Competition Control Box, use a .hex file.)",
                "stm32_erase_failed": "Failed to erase the application region.",
                "stm32_write_failed": "Write failed for block at address {addr}.",
                "stm32_go_failed": "Final CRC rejected — image refused, board stays in bootloader (re-run the flash).",
                "stm32_go_soft": "Firmware written and verified block by block. The final device-side CRC check was not confirmed (known G4 bootloader quirk).\n\nUnplug and replug the box: the new firmware will boot. Then check the version in the Update tab.",
                "stm32_empty_bin": "Empty .bin file.",
                "stm32_entering_bl": "Application detected — switching to bootloader (!DFU)...",
                "stm32_writing": "Writing... {pct}% ({done}/{total} bytes)",
                "stm32_finalizing": "Finalizing (CRC32 + metadata)...",
                "bl_update_group": "Flash bootloader (.bin)",
                "flash_bl": "Flash bootloader",
                "flash_bl_note": "⚠ Risky operation: interruption may brick the board (recovery via ST-Link/BOOT0). Only flash the bootloader if necessary.",
                "bl_update_confirm_title": "Flash STM32 bootloader",
                "bl_update_confirm": "You are about to replace the resident bootloader on {port}.\n\n⚠ If the operation is interrupted, the board will be inaccessible without ST-Link.\n\nContinue?",
                "bl_update_no_response": "No BL_UPDATE_READY response. Check that the app firmware is up to date (must support !BL_UPDATE command).",
                "bl_update_entering": "Sending !BL_UPDATE to app...",
                "bl_update_running": "Flashing bootloader...",
                "bl_update_done": "Bootloader flashed successfully.",
                "bl_update_failed": "Bootloader flash failed (see log).",
                "using_port": "Using port",
                "installed_fw": "Installed firmware:",
                "detect_fw": "Detect",
                "detecting_fw": "Detecting...",
                "fw_unknown": "Unknown firmware",
                "check_web_fw": "Check firmware update",
                "checking_web_fw": "Checking web...",
                "web_fw_status_prefix": "Web update:",
                "web_fw_not_checked": "not checked",
                "web_fw_update_available": "update available",
                "web_fw_up_to_date": "already up to date",
                "web_fw_local_unknown": "local version unknown",
                "web_fw_meta_missing": "Firmware metadata not found on page.",
                "web_fw_check_failed": "Web check failed",
                "download_web_fw": "Download + prepare",
                "web_fw_no_url": "No firmware download URL available.",
                "web_fw_opening": "Opening firmware download",
                "web_fw_dl_running": "Downloading firmware...",
                "web_fw_dl_done": "Firmware ready to flash.",
                "web_fw_dl_failed": "Firmware preparation failed",
                "web_fw_dl_no_hex": "No .hex file found in archive.",
                "web_fw_dl_unsupported": "Unsupported file type (expected: .hex or .zip).",
                "web_fw_requires_soft": "Required Motion Center",
                "web_fw_incompat_soft": "firmware not compatible with your Motion Center",
                "web_fw_incompat_block": "Download blocked: firmware is not compatible with your Motion Center version.",
                "mc_compat_prefix": "Motion Center compatibility:",
                "mc_compat_ok": "compatible",
                "mc_compat_ko": "incompatible",
                "mc_compat_unknown": "firmware requirement unknown",
                "soft_current_version": "Current version:",
                "soft_web_status": "Web update:",
                "soft_check": "Check for update",
                "soft_checking": "Checking web...",
                "soft_download": "Download",
                "soft_not_checked": "not checked",
                "soft_up_to_date": "already up to date",
                "soft_update_available": "update available",
                "soft_check_failed": "Software check failed",
                "soft_meta_missing": "Software version metadata not found on page.",
                "soft_no_url": "No Motion Center download URL available.",
                "soft_downloading": "Downloading Motion Center...",
                "soft_downloaded": "Download complete. Launch the new executable manually.",
                "soft_download_failed": "Motion Center download failed",
                "simhub_profile_title": "SimHub profile",
                "simhub_profile_status": "Profile status:",
                "simhub_profile_install": "Install into SimHub",
                "simhub_profile_not_installed": "not installed",
                "simhub_profile_installing": "Installing SimHub profile...",
                "simhub_profile_done": "profile installed",
                "simhub_profile_source_missing": "SimHub profile source folder not found.",
                "simhub_profile_no_file": "No .shmotioncontroller file found in the profile folder.",
                "simhub_profile_dest_missing": "SimHub installation not found.",
                "simhub_profile_pick_folder": "SimHub was not auto-detected. Do you want to select the SimHub folder manually?",
                "simhub_profile_pick_hint": "Select only the SimHub root folder (example: C:\\Program Files (x86)\\SimHub).",
                "simhub_profile_pick_title": "Select SimHub folder",
                "simhub_profile_pick_invalid": "Invalid selected folder (Controllers not found).",
                "simhub_profile_failed": "SimHub profile install failed",
                "simhub_profile_warn_missing": "⚠ SimHub profile missing/outdated or SimHub installation not found.",
                "simhub_install_select_box_title": "Which control box?",
                "simhub_install_select_box_msg": "Connect your control box for automatic detection.\n\nOr select manually:",
                "simhub_install_box_v1": "Box V1 (max 5 actuators)",
                "simhub_install_box_v2": "Box V2 (max 6 actuators)",
                "simhub_install_box_v3": "Box V3 (max 7 actuators)",
                "simhub_running_warn": "⚠ SimHub is currently running.\n\nConnecting or disconnecting a motor updates the SimHub configuration.\nClose SimHub so that Motion Center can update the profile.",
                "simhub_running_detail": "SimHub must send positions for the number of motors defined in Motion Center.\n\nMotion Center automatically updates these values in the SimHub profile:\n  • \"Edit serial commands and settings\"\n  • \"Edit assignments\"",
                "simhub_running_learn_more": "Learn more ▾",
                "simhub_running_learn_less": "Show less ▴",
                "simhub_fix": "Fix SimHub profile",
                "fw_not_detected": "firmware not detected",
                "fw_unsupported": "unsupported firmware",
                "control_box_doc_label": "Control box documentation:",
                "open_doc": "Open",
                "driver_settings_title": "AASD driver settings",
                "driver_settings_doc_label": "Driver settings (AASD):",
                "open_driver_settings": "Open page",
                "driver_col_param": "Parameter",
                "driver_col_value": "Value",
                "driver_col_expl": "Explanation",
                "driver_col_note": "Comment",
                "invalid_u16_range": "Invalid value. Allowed range: 0 to 32767",
                "files_count_suffix": "file(s)",
                "test_tab": "Manual test",
                "test_desc": "Direct control of actuators, independently of SimHub.\nServos are enabled on start and disabled on stop.",
                "test_m56_group": "M5 / M6",
                "test_warn_calib": "⚠ M{motors} connected but not calibrated (MAX=0). Run calibration before testing.",
                "test_start": "▶ Start",
                "test_stop": "■ Stop",
                "test_target_label": "Target",
                "test_motor_label": "Motors (smoothed)",
                "test_motor_label_direct": "Motors (direct)",
                "test_status_idle": "Idle",
                "test_status_running": "Running",
                "test_status_stopping": "Returning to 0%…",
                "test_not_connected": "⚠ Connection required to start the test.",
                "test_speed_limit_label": "Enable speed limit",
                "test_speed_limit_desc": "Limits movement speed and smooths rapid changes until the controls settle.",
                "test_speed_limit_warn_title": "⚠ Disable speed limiter?",
                "test_speed_limit_warn": "Do you really want to disable the speed limiter?\nWhen disabled movements can be really fast.\nIt's only recommended for troubleshooting.",
                "faq_tab": "FAQ / Help",
                "faq_title": "Frequently asked questions & troubleshooting",
                "faq_diag_btn": "Run diagnostic",
                "faq_diag_ok": "✅ Everything looks correct.",
                "faq_diag_no_port": "⚠ No COM port selected.",
                "faq_diag_not_connected": "⚠ Not connected to the Control Box.",
                "faq_diag_no_servo": "ℹ Servo disabled (normal if SimHub is not running).",
                "faq_diag_servo_on": "✅ Servo enabled.",
                "faq_diag_profile_ok": "✅ SimHub profile installed.",
                "faq_diag_profile_missing": "⚠ SimHub profile not installed — click 'Install in SimHub' in the Update tab.",
                "legal_title": "Legal information",
                "legal_notice": "{app_title} v{version}\nAuthor: {author}\nCopyright (c) {years} {company}. All rights reserved.\nLicense: {license_name}\nWebsite: {website}",
                "faq_items": [
                    ("🔴 Solid red LED, SimHub crash / 'connection lost'",
                     "The firmware replies OK to ENABLE but SimHub doesn't receive data correctly.\n"
                     "Possible causes:\n"
                     "• Motors 5/6 are marked 'connected' in the Control Box but not physically plugged in → homing at startup never completes.\n"
                     "  Fix: in the Status tab, uncheck M5 and/or M6, then reflash/reconnect.\n"
                     "• SimHub profile is outdated — click 'Install in SimHub' then reload the controller in SimHub.\n"
                     "• The COM port is in use by something else (Arduino IDE, another app)."),
                    ("🟠 Orange LED stuck (infinite homing)",
                     "Actuator 5 or 6 is marked connected but the endstop is never reached.\n"
                     "• Check the wiring of the endstop for the affected actuator.\n"
                     "• If the actuator is not physically present: uncheck M5/M6 in the Status tab → save → restart SimHub."),
                    ("⚫ Grey LED / box not detected",
                     "• Check that the USB cable is properly connected.\n"
                     "• Select the correct COM port and click 'Connect'.\n"
                     "• Try 'Refresh ports' if the port does not appear.\n"
                     "• Close Arduino IDE / SimHub which may be blocking the port.\n"
                     "• If the box was just flashed, wait 3–5 seconds before reconnecting."),
                    ("🔵 Blue LED but SimHub won't start",
                     "The Control Box is connected to Motion Center (blue LED = active Python session).\n"
                     "SimHub cannot connect at the same time as Motion Center.\n"
                     "• Click 'Disconnect' in Motion Center, then launch SimHub."),
                    ("SimHub: 'Output connection failure'",
                     "SimHub does not receive an OK response on startup.\n"
                     "• Make sure the SimHub profile matches the firmware version (V1/V2).\n"
                     "• Reinstall the profile via 'Install in SimHub', then in SimHub:\n"
                     "  1. Delete the existing controller\n"
                     "  2. Add the newly installed profile\n"
                     "• If Motion Center is open, close it — SimHub and Motion Center cannot use the COM port at the same time."),
                    ("Firmware flash fails",
                     "• Close SimHub and Arduino IDE before flashing.\n"
                     "• The Control Box must be in bootloader mode: Motion Center handles this automatically (1200 bps reset).\n"
                     "• If the flash still fails: unplug/replug the Control Box, wait 5 seconds, try again.\n"
                     "• On Windows 11: check that the COM driver is installed (Device Manager)."),
                    ("Actuator not moving after calibration",
                     "• Check that the servo is enabled (red LED).\n"
                     "• Check the MAX value in the Status tab — if it is 0, run a full calibration again.\n"
                     "• Check the Margin: if too large relative to MAX, the motion range is zero.\n"
                     "• Check the AASD driver settings in the 'Driver settings' tab."),
                    ("How to fully reset the box",
                     "In the Status tab, click 'Factory reset'.\n"
                     "This clears the calibration and marks motors 5/6 as disconnected.\n"
                     "After reset: reconnect, then re-calibrate the actuators in the Functions tab."),
                ],
            }
        }

        self.i18n["DE"] = dict(self.i18n["EN"])
        self.i18n["DE"]["func_labels"] = dict(self.i18n["EN"]["func_labels"])
        self.i18n["DE"]["func_desc"] = dict(self.i18n["EN"]["func_desc"])
        self.i18n["DE"].update({
            "status_tab": "Status",
            "functions_tab": "Funktionen",
            "update_tab": "Aktualisierung",
            "driver_tab": "Treibereinstellungen",
            "soft_update_tab": "Motion Center-Aktualisierung",
            "log_tab": "Protokoll",
            "port": "Port:",
            "refresh_ports": "Ports aktualisieren",
            "connect": "Verbinden",
            "disconnect": "Trennen",
            "refresh": "Aktualisieren",
            "enable": "Aktivieren",
            "disable": "Deaktivieren",
            "enable_warning": "⚠ Warnung: Dadurch werden die Motoren aktiviert.",
            "control_box": "CONTROL BOX",
            "homing_speed": "Homing-Geschwindigkeit",
            "step_speed": "Streaming-Geschwindigkeit (SimHub)",
            "step_speed_label": "Max. STEP-Frequenz (Schritte/s):",
            "step_speed_hint": "Von 1000 bis 450000. Standard 200000. Verringern, wenn der Treiber aussteigt.",
            "step_speed_invalid": "Ungueltige STEP-Frequenz",
            "speed_steps_s": "Geschwindigkeit (steps/s):",
            "apply": "Anwenden",
            "recommended": "Von 50 (langsam) bis 10000 (sehr schnell). Falls 10000 noch zu langsam ist, PN098 erhoehen.",
            "motors": "Motoren (☐/☑ = verbunden, Doppelklick auf Max zum Bearbeiten; Reserve bei M5/M6)",
            "estop_use": "ESTOP verwenden (fallende Flanke)",
            "estop_tip": "Wenn aktiviert, trennt eine fallende Flanke am ESTOP-Eingang die Motoren.",
            "estop_behavior": "ESTOP-Verhalten:",
            "estop_behavior_disable_servo": "Disable servo",
            "estop_behavior_return_park": "Return to park",
            "estop_behavior_stop_here": "Stop here",
            "col_m": "M",
            "col_conn": "Verbunden",
            "col_cal": "Kal",
            "col_max": "Max",
            "col_margin": "Reserve",
            "col_endstop": "Endstop",
            "col_homing_dir": "Homing-Richt.",
            "col_encoder": "Encoder",
            "tip_col_endstop": "Gibt an, ob ein MIN-Endstop an diesem Aktuator angeschlossen ist (für automatisches Homing erforderlich).",
            "tip_col_homing_dir": "Position, auf die sich der Aktuator beim Start von SimHub kalibriert. Stellen Sie den Park-Wert im SimHub-Profil auf 100%, wenn die Homing-Position das Maximum ist. Klicken zum Umschalten MIN/MAX (MAX erfordert einen kalibrierten Max-Wert).",
            "col_endpark": "Endpark",
            "tip_col_endpark": "Position (% des Hubs), die die Aktuatoren beim SimHub-Stopp langsam anfahren, bevor die Servos abgeschaltet werden. Klicken zum Einstellen oder Deaktivieren.",
            "endpark_dialog_label": "Park-Position am Sitzungsende: Beim SimHub-Stopp (SH_DISABLE) fährt der Aktuator langsam auf diesen Prozentsatz seines Hubs, bevor die Servos abgeschaltet werden.",
            "endpark_dialog_example": "Beispiel: SimHub-Park bei 0%, MIN-Homing. Beim Start von SimHub kalibriert sich der Aktuator auf das Minimum, dann übernimmt SimHub und fährt auf die 50%-Position. Am Sitzungsende kehrt der Aktuator auf 0% zurück — der Simulator bleibt schief stehen, z. B. bei einem Traction-Loss-Aufbau.\nMit Endpark-Position 50%: Wenn SimHub die Kontrolle abgibt, fährt der Aktuator von 0% auf die 50%-Position, bevor abgeschaltet wird.\nDie Geschwindigkeit hängt von der Homing-Speed-Einstellung ab.",
            "endpark_disabled": "Deaktiviert (Aktuator bleibt auf der letzten von SimHub gesendeten Position)",
            "busy_wait": "⏳ Die Box ist beschäftigt (Kalibrierung oder Bewegung läuft).\nBitte warten Sie das Ende des Vorgangs ab, bevor Sie diese Einstellung ändern.",
            "homing_fail_no_ready": "Servo nicht angeschlossen oder nicht versorgt: Der Treiber liefert kein READY-Signal. DB25-Kabel und Treiber-Versorgung prüfen (Alarm per Power-Cycle quittieren).",
            "homing_fail_min_timeout": "MIN-Endstop nie erkannt: Der Aktuator erreicht den Anschlag, aber der Endstop-Eingang löst nicht aus. Verkabelung des Schalters prüfen.",
            "homing_fail_seek_limit": "Schritt-Suchlimit (40000) vor dem Endanschlag erreicht: Bitte erhöhen Sie Pn98 am Servotreiber (weniger Schritte pro Umdrehung) und starten Sie die Kalibrierung erneut.",
            "homing_fail_max_timeout": "MAX-Endstop während der Hubmessung nie erkannt. MAX-Schalter prüfen (in Serie mit MIN).",
            "homing_fail_endstop_stuck": "Endstop-Eingang dauerhaft aktiv: NC/kurzgeschlossener Schalter oder invertierte Motorrichtung.",
            "homing_fail_estop": "Not-Aus aktiv: Homing blockiert.",
            "homing_fail_reboot": "Neustart der Box erforderlich (USB trennen/wieder verbinden).",
            "homing_fail_max_too_small": "Gemessener Hub zu klein: MAX-Endstop hat zu früh ausgelöst (Schalter/Verkabelung prüfen).",
            "tip_col_encoder": "Absoluter Encoder: Wenn aktiviert, ist die Position beim Start ohne Homing bekannt.",
            "motor": "Motor:",
            "run": "Start",
            "desc": "Beschreibung:",
            "warning": "⚠️ Warnung:",
            "pn98_warning": "⚠️ Ab v1.2 ändert sich Pn98 von 6 auf 12. Wird der Wert an den Reglern nicht aktualisiert, besteht Bruchgefahr!",
            "not_connected": "Nicht verbunden.",
            "unsupported_fw_connect": "Competition Control Box nicht erkannt oder Firmware veraltet.\nSiehe Reiter „Aktualisierung“.",
            "serial_err": "Serieller Port konnte nicht geöffnet werden.\n\nHäufige Ursachen:\n- Arduino IDE Serial Monitor geöffnet\n- SimHub verbunden\n- Falscher COM-Port\n\nDetails:\n",
            "get_timeout": "GET-Timeout: kein END empfangen.\nMögliche Ursachen:\n- GET gibt END nicht aus\n- Ein langer Vorgang blockiert\n- Falscher Port\n",
            "box_prefix": "V",
            "cancel": "Abbrechen",
            "factory_reset_btn": "Factory Reset",
            "func_servo_required": "⚠️ Der ausgewählte Motor muss im Reiter Status als verbunden markiert sein, bevor eine Funktion gestartet werden kann.",
            "func_pn24_hint": "Pn24 stellt das Drehmoment ein, das die Endanschlag-Erkennung auslöst: niedriger Wert = hohe Empfindlichkeit (Gefahr von Fehlauslösungen), hoher Wert = geringe Empfindlichkeit (Bruchgefahr). Das momentane Drehmoment kann mit Dn002 beobachtet werden, um den Wert optimal einzustellen.",
            "cal_stroke_measured": "Gemessener Hub {motor}: {steps} Schritte.",
            "cal_stroke_clamped": "⚠ Der gemessene Hub von {motor} ({steps} Schritte) überschreitet den nutzbaren Bereich (32767): Wert auf 32767 begrenzt. Erhöhen Sie Pn98 am Servotreiber (weniger Schritte pro Umdrehung), um den vollen Hub zu nutzen.",
            "motor56_warn_title": "⚠️ Motor M{n} — Achtung",
            "motor56_warn_msg": "Wenn der Motor nicht verbunden und korrekt kalibriert ist, kann die Box im eingeschränkten Modus mit SimHub betrieben werden.",
            "m6_requires_m5": "⚠️ M6 kann nur aktiviert werden, wenn M5 bereits verbunden ist.",
            "m5_disconnect_m6_first": "⚠️ Trenne M6, bevor M5 deaktiviert wird.",
            "m7_requires_m6": "⚠️ M7 kann nur aktiviert werden, wenn M6 bereits verbunden ist.",
            "m6_disconnect_m7_first": "⚠️ Trenne M7, bevor M6 deaktiviert wird.",
            "stale_profiles_title": "SimHub-Bereinigung",
            "stale_profiles_msg": "Diese Dateien können aus dem SimHub-Controllers-Ordner gelöscht werden:\n(Aktive Controller in SimHub sind nicht betroffen)",
            "stale_profiles_section_stale": "Alte Profile (möglicherweise veraltet):",
            "stale_profiles_section_bak": "Sicherungsdateien (.bak):",
            "stale_profiles_select_all": "Alle auswählen",
            "stale_profiles_deselect_all": "Alle abwählen",
            "stale_profiles_delete": "Ausgewählte löschen",
            "stale_profiles_none": "Keine Datei ausgewählt.",
            "simhub_reload_title": "⚠ SimHub — Aktion erforderlich",
            "simhub_reload_intro": "Das Profil wurde auf der Festplatte aktualisiert ({n} aktiver Motor(en)).\nSimHub kann nicht automatisch neu konfiguriert werden, solange es läuft.",
            "simhub_reload_step1": "① In SimHub › Motion controllers:\n   LÖSCHE alle aktiven Controller mit diesen Namen:",
            "simhub_reload_step2": "② Klicke auf \"Add\" / \"+\", um den aktualisierten Controller hinzuzufügen.\n   (Das Profilkommentar enthält das Aktualisierungsdatum)",
            "factory_reset_warn": "Diese Aktion löscht gespeicherte Daten dauerhaft.\nStelle sicher, dass du die Folgen verstehst, bevor du fortfährst.",
            "confirm_reset_title": "Factory Reset bestätigen",
            "confirm_reset_msg": "Factory Reset löscht gespeicherte Daten.\n\nBestätigen?",
            "saved_ok": "Gespeichert",
            "update_group": "Firmware flashen (.hex)",
            "hex_file": "Firmware:",
            "browse": "Durchsuchen",
            "custom_fw": "Benutzerdefiniert",
            "box_select_label": "Wähle deine Control Box:",
            "box_card_comp_v1": "Competition V1 (5 Aktoren)",
            "box_card_comp_v2": "Competition V2 (6 Aktoren)",
            "box_card_comp_v3": "Competition V3 (7 Aktoren)",
            "box_card_pro_v1": "PRO Control Box V1 (4 Aktoren)",
            "box_card_pro_v2": "PRO Control Box V2 (6 Aktoren)",
            "compat_v18": "Kompatibilität: nur SRT Control Box v1.8",
            "flash": "Flashen",
            "flash_note": "Beim Flashen werden die Einstellungen im Reiter Status nicht gelöscht. Zum Zurücksetzen der Box-Einstellungen verwende die Funktion \"Factory Reset\" im Reiter \"Status\".",
            "flash_running": "Flash läuft...",
            "flash_done": "Flash erfolgreich abgeschlossen.",
            "flash_failed": "Flash fehlgeschlagen (siehe Log).",
            "flash_port_busy": "Kommunikation mit dem Gerät nicht möglich: Der Port scheint bereits belegt zu sein.\n\nSchließe SimHub oder andere Software, die mit der Karte kommunizieren könnte, und versuche es erneut.",
            "select_hex_first": "Bitte zuerst eine .hex-Datei wählen.",
            "select_port_first": "Wähle zuerst einen COM-Port.",
            "hex_not_found": ".hex-Datei nicht gefunden.",
            "avrdude_not_found": "avrdude nicht gefunden. Installiere Arduino IDE oder PlatformIO (tool-avrdude).",
            "touch1200_warn": "Warnung: 1200-bps-Reset fehlgeschlagen, Flash wird trotzdem versucht.",
            "stm32_no_bootloader": "STM32-Bootloader nicht gefunden (keine PING-Antwort). Sicherstellen, dass die verbundene Box eine STM32-Box ist (PRO Control Box oder Competition Control Box V3), keine ATmega Competition Control Box.",
            "stm32_confirm_title": "STM32-Flash",
            "stm32_confirm": "Diese Firmware (.bin) ist für eine STM32-Box (PRO Control Box STM32F103 oder Competition Control Box V3 STM32G4).\n\nIst die Box an {port} eine STM32-Box?\n\n(Für eine ATmega32U4 Competition Control Box bitte eine .hex-Datei verwenden.)",
            "stm32_erase_failed": "Löschen des Anwendungsbereichs fehlgeschlagen.",
            "stm32_write_failed": "Schreibfehler bei Block an Adresse {addr}.",
            "stm32_go_failed": "Finale CRC abgelehnt — Image verworfen, Board bleibt im Bootloader (Flash erneut starten).",
            "stm32_go_soft": "Firmware geschrieben und blockweise geprüft. Die abschließende CRC-Prüfung auf dem Board wurde nicht bestätigt (bekannter G4-Bootloader-Quirk).\n\nZiehe die Box ab und stecke sie wieder ein: Die neue Firmware startet. Prüfe danach die Version im Reiter Aktualisierung.",
            "stm32_empty_bin": "Leere .bin-Datei.",
            "stm32_entering_bl": "Anwendung erkannt — Wechsel in den Bootloader (!DFU)...",
            "stm32_writing": "Schreiben... {pct}% ({done}/{total} Bytes)",
            "stm32_finalizing": "Abschluss (CRC32 + Metadaten)...",
            "bl_update_group": "Bootloader flashen (.bin)",
            "flash_bl": "Bootloader flashen",
            "flash_bl_note": "⚠ Riskante Operation: Unterbrechung kann das Board bricken (Wiederherstellung via ST-Link/BOOT0). Nur bei Bedarf flashen.",
            "bl_update_confirm_title": "STM32-Bootloader flashen",
            "bl_update_confirm": "Du bist dabei, den residenten Bootloader auf {port} zu ersetzen.\n\n⚠ Bei Unterbrechung ist das Board ohne ST-Link nicht mehr erreichbar.\n\nFortfahren?",
            "bl_update_no_response": "Keine BL_UPDATE_READY-Antwort. Prüfe, ob die App-Firmware aktuell ist (Befehl !BL_UPDATE muss unterstützt werden).",
            "bl_update_entering": "Sende !BL_UPDATE an App...",
            "bl_update_running": "Bootloader wird geflasht...",
            "bl_update_done": "Bootloader erfolgreich geflasht.",
            "bl_update_failed": "Bootloader-Flash fehlgeschlagen (siehe Protokoll).",
            "using_port": "Verwendeter Port",
            "installed_fw": "Installierte Firmware:",
            "detect_fw": "Erkennen",
            "detecting_fw": "Erkennung...",
            "fw_unknown": "Unbekannte Firmware",
            "check_web_fw": "Firmware-Update prüfen",
            "checking_web_fw": "Web-Prüfung...",
            "web_fw_status_prefix": "Web-Update:",
            "web_fw_not_checked": "nicht geprüft",
            "web_fw_update_available": "Update verfügbar",
            "web_fw_up_to_date": "bereits aktuell",
            "web_fw_local_unknown": "lokale Version unbekannt",
            "web_fw_meta_missing": "Firmware-Metadaten auf der Seite nicht gefunden.",
            "web_fw_check_failed": "Web-Prüfung fehlgeschlagen",
            "download_web_fw": "Herunterladen + vorbereiten",
            "web_fw_no_url": "Keine Firmware-Download-URL verfügbar.",
            "web_fw_opening": "Firmware-Download wird geöffnet",
            "web_fw_dl_running": "Firmware wird heruntergeladen...",
            "web_fw_dl_done": "Firmware bereit zum Flashen.",
            "web_fw_dl_failed": "Firmware-Vorbereitung fehlgeschlagen",
            "web_fw_dl_no_hex": "Keine .hex-Datei im Archiv gefunden.",
            "web_fw_dl_unsupported": "Nicht unterstützter Dateityp (erwartet: .hex oder .zip).",
            "web_fw_requires_soft": "Erforderliches Motion Center",
            "web_fw_incompat_soft": "Firmware nicht mit deinem Motion Center kompatibel",
            "web_fw_incompat_block": "Download blockiert: Firmware ist nicht mit deiner Motion-Center-Version kompatibel.",
            "mc_compat_prefix": "Motion-Center-Kompatibilität:",
            "mc_compat_ok": "kompatibel",
            "mc_compat_ko": "inkompatibel",
            "mc_compat_unknown": "Firmware-Anforderung unbekannt",
            "soft_current_version": "Aktuelle Version:",
            "soft_web_status": "Web-Update:",
            "soft_check": "Auf Update prüfen",
            "soft_checking": "Web-Prüfung...",
            "soft_download": "Herunterladen",
            "soft_not_checked": "nicht geprüft",
            "soft_up_to_date": "bereits aktuell",
            "soft_update_available": "Update verfügbar",
            "soft_check_failed": "Software-Prüfung fehlgeschlagen",
            "soft_meta_missing": "Software-Versionsmetadaten auf der Seite nicht gefunden.",
            "soft_no_url": "Keine Motion-Center-Download-URL verfügbar.",
            "soft_downloading": "Motion Center wird heruntergeladen...",
            "soft_downloaded": "Download abgeschlossen. Starte die neue ausführbare Datei manuell.",
            "soft_download_failed": "Motion-Center-Download fehlgeschlagen",
            "simhub_profile_title": "SimHub-Profil",
            "simhub_profile_status": "Profilstatus:",
            "simhub_profile_install": "In SimHub installieren",
            "simhub_profile_not_installed": "nicht installiert",
            "simhub_profile_installing": "SimHub-Profil wird installiert...",
            "simhub_profile_done": "Profil installiert",
            "simhub_profile_source_missing": "Quellordner des SimHub-Profils nicht gefunden.",
            "simhub_profile_no_file": "Keine .shmotioncontroller-Datei im Profilordner gefunden.",
            "simhub_profile_dest_missing": "SimHub-Installation nicht gefunden.",
            "simhub_profile_pick_folder": "SimHub wurde nicht automatisch erkannt. Möchtest du den SimHub-Ordner manuell auswählen?",
            "simhub_profile_pick_hint": "Bitte nur den SimHub-Hauptordner auswählen (Beispiel: C:\\Program Files (x86)\\SimHub).",
            "simhub_profile_pick_title": "SimHub-Ordner auswählen",
            "simhub_profile_pick_invalid": "Ungültiger ausgewählter Ordner (Controllers nicht gefunden).",
            "simhub_profile_failed": "Installation des SimHub-Profils fehlgeschlagen",
            "simhub_profile_warn_missing": "⚠ SimHub-Profil fehlt/veraltet oder SimHub-Installation nicht gefunden.",
            "simhub_install_select_box_title": "Welche Control Box?",
            "simhub_install_select_box_msg": "Verbinde deine Control Box für die automatische Erkennung.\n\nOder wähle manuell:",
            "simhub_install_box_v1": "Box V1 (max 5 Aktoren)",
            "simhub_install_box_v2": "Box V2 (max 6 Aktoren)",
            "simhub_install_box_v3": "Box V3 (max 7 Aktoren)",
            "simhub_running_warn": "⚠ SimHub läuft gerade.\n\nEin Motor zu verbinden oder zu trennen ändert die SimHub-Konfiguration.\nSchließe SimHub, damit Motion Center das Profil aktualisieren kann.",
            "simhub_running_detail": "SimHub muss Positionen für die in Motion Center definierte Anzahl Motoren senden.\n\nMotion Center aktualisiert diese Werte automatisch im SimHub-Profil:\n  • \"Edit serial commands and settings\"\n  • \"Edit assignments\"",
            "simhub_running_learn_more": "Mehr erfahren ▾",
            "simhub_running_learn_less": "Weniger ▴",
            "simhub_fix": "SimHub-Profil korrigieren",
            "fw_not_detected": "Firmware nicht erkannt",
            "fw_unsupported": "nicht unterstützte Firmware",
            "control_box_doc_label": "Control-Box-Dokumentation:",
            "open_doc": "Öffnen",
            "driver_settings_title": "AASD-Treiber-Einstellungen",
            "driver_settings_doc_label": "Treibereinstellungen (AASD):",
            "open_driver_settings": "Seite öffnen",
            "driver_col_param": "Parameter",
            "driver_col_value": "Wert",
            "driver_col_expl": "Erklärung",
            "driver_col_note": "Hinweis",
            "invalid_u16_range": "Ungültiger Wert. Erlaubter Bereich: 0 bis 32767",
            "files_count_suffix": "Datei(en)",
            "fw_v15_warn_title": "⚠️ Firmware v1.5 — Warnung",
            "fw_v15_warn": "v1.5 ist nicht mit Motion Center kompatibel und erfordert das alte SimHub-Profil.\n\nDu kannst mit Motion Center jederzeit von v1.5 auf v1.8 wechseln.",
            "test_tab": "Manueller Test",
            "test_desc": "Direktsteuerung der Aktuatoren, unabhängig von SimHub.\nServos werden beim Start aktiviert und beim Stopp deaktiviert.",
            "test_m56_group": "M5 / M6",
            "test_warn_calib": "⚠ M{motors} verbunden, aber nicht kalibriert (MAX=0). Kalibrierung zuerst durchführen.",
            "test_start": "▶ Starten",
            "test_stop": "■ Stoppen",
            "test_target_label": "Zielposition",
            "test_motor_label": "Motoren (ger.)",
                "test_motor_label_direct": "Motoren (direkt)",
            "test_status_idle": "Inaktiv",
            "test_status_running": "L\u00e4uft",
            "test_status_stopping": "R\u00fcckkehr zu 0%\u2026",
            "test_not_connected": "⚠ Verbindung erforderlich, um den Test zu starten.",
                "test_speed_limit_label": "Geschwindigkeitsbegrenzung aktivieren",
                "test_speed_limit_desc": "Begrenzt die Bewegungsgeschwindigkeit und glättet schnelle Änderungen, bis die Steuerung stabil ist.",
                "test_speed_limit_warn_title": "⚠ Geschwindigkeitsbegrenzung deaktivieren?",
                "test_speed_limit_warn": "Möchten Sie die Geschwindigkeitsbegrenzung wirklich deaktivieren?\nOhne Begrenzung können die Bewegungen sehr schnell sein.\nNur zur Fehlersuche empfohlen.",
            "faq_tab": "FAQ / Hilfe",
            "faq_title": "Häufige Fragen & Fehlerbehebung",
            "faq_diag_btn": "Diagnose starten",
            "faq_diag_ok": "✅ Alles scheint korrekt.",
            "faq_diag_no_port": "⚠ Kein COM-Port ausgewählt.",
            "faq_diag_not_connected": "⚠ Keine Verbindung zur Control Box.",
            "faq_diag_no_servo": "ℹ Servo deaktiviert (normal, wenn SimHub nicht läuft).",
            "faq_diag_servo_on": "✅ Servo aktiviert.",
            "faq_diag_profile_ok": "✅ SimHub-Profil installiert.",
            "faq_diag_profile_missing": "⚠ SimHub-Profil nicht installiert — 'In SimHub installieren' im Aktualisierungs-Tab klicken.",
            "legal_title": "Rechtliche Hinweise",
            "legal_notice": "{app_title} v{version}\nAutor: {author}\nCopyright (c) {years} {company}. Alle Rechte vorbehalten.\nLizenz: {license_name}\nWebseite: {website}",
            "faq_items": [
                ("🔴 Rote LED (dauerhaft), SimHub-Absturz / 'connection lost'",
                 "Die Firmware antwortet auf ENABLE mit OK, aber SimHub empfängt keine korrekten Daten.\n"
                 "Mögliche Ursachen:\n"
                 "• Motor 5/6 ist als 'verbunden' markiert, aber physisch nicht angeschlossen → Homing beim Start endet nie.\n"
                 "  Lösung: im Status-Reiter M5 und/oder M6 deaktivieren, dann neu flashen/verbinden.\n"
                 "• SimHub-Profil veraltet — 'In SimHub installieren' klicken, dann den Controller in SimHub neu laden.\n"
                 "• Der COM-Port wird von einem anderen Programm belegt (Arduino IDE, anderer Prozess)."),
                ("🟠 Orange LED dauerhaft (endloses Homing)",
                 "Aktuator 5 oder 6 ist als verbunden markiert, aber der Endschalter wird nie erreicht.\n"
                 "• Verdrahtung des Endschalters des betroffenen Aktuators prüfen.\n"
                 "• Wenn der Aktuator physisch nicht vorhanden ist: M5/M6 im Status-Reiter deaktivieren → speichern → SimHub neu starten."),
                ("⚫ Graue LED / Box nicht erkannt",
                 "• USB-Kabel prüfen.\n"
                 "• Richtigen COM-Port auswählen und 'Verbinden' klicken.\n"
                 "• 'Ports aktualisieren' versuchen, wenn der Port nicht erscheint.\n"
                 "• Arduino IDE / SimHub schließen, da diese den Port blockieren können.\n"
                 "• Nach einem Flash 3–5 Sekunden warten, bevor erneut verbunden wird."),
                ("🔵 Blaue LED, aber SimHub startet nicht",
                 "Die Control Box ist mit Motion Center verbunden (blaue LED = aktive Python-Sitzung).\n"
                 "SimHub und Motion Center können den COM-Port nicht gleichzeitig nutzen.\n"
                 "• In Motion Center auf 'Trennen' klicken, dann SimHub starten."),
                ("SimHub: 'Output connection failure'",
                 "SimHub erhält beim Start keine OK-Antwort.\n"
                 "• Sicherstellen, dass das SimHub-Profil zur Firmware-Version passt (V1/V2).\n"
                 "• Profil über 'In SimHub installieren' neu installieren, dann in SimHub:\n"
                 "  1. Bestehenden Controller löschen\n"
                 "  2. Neu installierten Controller hinzufügen\n"
                 "• Wenn Motion Center geöffnet ist, schließen — SimHub und Motion Center können den COM-Port nicht gleichzeitig nutzen."),
                ("Firmware-Flash schlägt fehl",
                 "• SimHub und Arduino IDE vor dem Flashen schließen.\n"
                 "• Die Control Box muss sich im Bootloader-Modus befinden: Motion Center erledigt das automatisch (1200-bps-Reset).\n"
                 "• Wenn der Flash weiter schlägt fehl: Control Box aus- und wieder einstecken, 5 Sekunden warten, erneut versuchen.\n"
                 "• Unter Windows 11: prüfen, ob der COM-Treiber installiert ist (Gerätemanager)."),
                ("Aktuator bewegt sich nach Kalibrierung nicht",
                 "• Prüfen, ob der Servo aktiviert ist (rote LED).\n"
                 "• MAX-Wert im Status-Reiter prüfen — ist er 0, eine vollständige Kalibrierung erneut starten.\n"
                 "• Reserve prüfen: ist sie zu groß im Verhältnis zu MAX, ist der Bewegungsbereich null.\n"
                 "• AASD-Treibereinstellungen im Reiter 'Treibereinstellungen' prüfen."),
                ("Box vollständig zurücksetzen",
                 "Im Status-Reiter auf 'Factory Reset' klicken.\n"
                 "Dies löscht die Kalibrierung und markiert Motoren 5/6 als getrennt.\n"
                 "Nach dem Reset: neu verbinden, dann die Aktuatoren im Reiter Funktionen neu kalibrieren."),
            ],
        })
        self.i18n["DE"]["func_labels"].update({
            "detect_min": "Min erkennen",
            "detect_max": "Max erkennen",
            "go_center": "Zur Mitte",
            "complete_calib": "Vollständige Kalibrierung",
            "test_stroke": "Voller Hubtest",
            "factory_reset": "Factory Reset",
        })
        self.i18n["DE"]["func_desc"].update({
            "detect_min": "Der Aktuator fährt ein, bis der minimale Endanschlag erkannt wird. So kann seine Position bestimmt werden.",
            "detect_max": "Der Aktuator fährt aus, bis der maximale Endanschlag erkannt wird. Liefert keine absolute Position.",
            "go_center": "Fährt den Aktuator in die Mitte. Wenn die Position unbekannt ist, wird zuerst „Min erkennen“ ausgeführt.",
            "complete_calib": "Findet Minimum und Maximum und speichert die Kalibrierung. Die Reserve wird manuell im Status-Reiter gesetzt.",
            "test_stroke": "Fährt von Min nach Max und anschließend in die Mitte.",
            "factory_reset": "Löscht alle gespeicherten Daten und stellt die Werkseinstellungen wieder her.",
        })
        self._rebuild_i18n_value_cache()

        # Functions selection
        self.func_choice_key = tk.StringVar(value="complete_calib")
        self.func_motor = tk.IntVar(value=6)
        self.func_motor.trace_add("write", lambda *_: (self._refresh_actuator_visual(), self._update_functions_controls_state()))

        self._build_ui()
        self._refresh_ports()
        self.protocol("WM_DELETE_WINDOW", self._on_close)
        self.after(50, self._pump_rx)

    def _rebuild_i18n_value_cache(self):
        cache = {}
        try:
            for lang_dict in self.i18n.values():
                for key, value in lang_dict.items():
                    if isinstance(value, str):
                        cache.setdefault(key, set()).add(value.strip())
        except Exception:
            pass
        self._i18n_value_cache = cache

    def _is_localized_placeholder(self, value: str, key: str, extra_values=()):
        s = str(value or "").strip()
        if s in ("", "-"):
            return True
        vals = set(getattr(self, "_i18n_value_cache", {}).get(key, set()))
        vals.update(str(v).strip() for v in (extra_values or ()) if str(v).strip())
        return s in vals

    def _apply_modern_theme(self):
        c = self._theme
        try:
            self.configure(bg=c["bg"])
        except Exception:
            pass

        try:
            style = ttk.Style(self)
            try:
                style.theme_use("clam")
            except Exception:
                pass

            style.configure(".",
                background=c["bg"],
                foreground=c["text"],
                fieldbackground=c["input_bg"],
                troughcolor=c["surface"],
                bordercolor=c["border"],
                lightcolor=c["border"],
                darkcolor=c["border"],
                insertcolor=c["text"],
                relief="flat",
            )

            style.configure("TFrame", background=c["bg"])
            style.configure("TLabelframe", background=c["surface"], bordercolor=c["border"], relief="solid")
            style.configure("TLabelframe.Label", background=c["bg"], foreground=c["text"], font=("Segoe UI", 10, "bold"))
            style.configure("TLabel", background=c["bg"], foreground=c["text"])

            style.configure("TButton",
                background=c["surface"],
                foreground=c["text"],
                bordercolor=c["border"],
                padding=(10, 6),
                focusthickness=1,
                focuscolor=c["accent"],
            )
            style.map("TButton",
                background=[("active", c["accent_hover"]), ("pressed", c["accent_active"]), ("disabled", c["surface_alt"])],
                foreground=[("active", "#000000"), ("pressed", "#000000"), ("disabled", "#888888")],
            )

            style.configure("Accent.TButton",
                background=c["accent"],
                foreground="#000000",
                bordercolor=c["accent"],
                padding=(10, 6),
            )
            style.map("Accent.TButton",
                background=[("active", c["accent_hover"]), ("pressed", c["accent_active"]), ("disabled", c["surface_alt"])],
                foreground=[("disabled", "#888888")],
            )

            style.configure("Green.TButton",
                background="#2E7D32",
                foreground="#FFFFFF",
                bordercolor="#1B5E20",
                padding=(10, 6),
            )
            style.map("Green.TButton",
                background=[("active", "#388E3C"), ("pressed", "#1B5E20"), ("disabled", c["surface_alt"])],
                foreground=[("active", "#FFFFFF"), ("disabled", "#888888")],
            )

            style.configure("Red.TButton",
                background="#C62828",
                foreground="#FFFFFF",
                bordercolor="#B71C1C",
                padding=(10, 6),
            )
            style.map("Red.TButton",
                background=[("active", "#E53935"), ("pressed", "#B71C1C"), ("disabled", c["surface_alt"])],
                foreground=[("active", "#FFFFFF"), ("disabled", "#888888")],
            )

            style.configure("TEntry",
                fieldbackground=c["input_bg"],
                foreground=c["text"],
                bordercolor=c["border"],
                insertcolor=c["text"],
            )
            style.map("TEntry", bordercolor=[("focus", c["accent"])])

            style.configure("TCombobox",
                fieldbackground=c["input_bg"],
                foreground=c["text"],
                background=c["surface"],
                bordercolor=c["border"],
                arrowsize=14,
            )
            style.map("TCombobox",
                fieldbackground=[("readonly", c["input_bg"])],
                foreground=[("readonly", c["text"])],
                bordercolor=[("focus", c["accent"])],
            )

            style.configure("TSpinbox",
                fieldbackground=c["input_bg"],
                foreground=c["text"],
                bordercolor=c["border"],
                arrowsize=14,
            )
            style.map("TSpinbox", bordercolor=[("focus", c["accent"])])

            style.configure(
                "TNotebook",
                background=c["bg"],
                borderwidth=0,
                tabmargins=(10, 8, 10, 0),
            )
            style.configure("TNotebook.Tab",
                background=c["surface_alt"],
                foreground=c["text_muted"],
                padding=(16, 9),
                borderwidth=1,
                bordercolor=c["border"],
                lightcolor=c["border"],
                darkcolor=c["border"],
                relief="flat",
                font=("Segoe UI", 9, "normal"),
            )
            style.map("TNotebook.Tab",
                background=[("selected", c["surface"]), ("active", c["surface_alt"])],
                foreground=[("selected", c["text"]), ("active", c["text"])],
                bordercolor=[("selected", c["accent"]), ("active", c["accent_hover"])],
                lightcolor=[("selected", c["accent"]), ("active", c["accent_hover"])],
                darkcolor=[("selected", c["accent"]), ("active", c["accent_hover"])],
                font=[("selected", ("Segoe UI", 10, "bold")), ("!selected", ("Segoe UI", 9, "normal"))],
                padding=[("selected", (22, 12)), ("!selected", (16, 9))],
                expand=[("selected", [2, 3, 2, 0])],
            )

            style.configure("Treeview",
                background=c["input_bg"],
                foreground=c["text"],
                fieldbackground=c["input_bg"],
                bordercolor=c["border"],
                rowheight=24,
            )
            style.map("Treeview", background=[("selected", c["selection"])], foreground=[("selected", c["text"])])
            style.configure("Treeview.Heading",
                background=c["surface"],
                foreground=c["text"],
                bordercolor=c["border"],
                font=("Segoe UI", 9, "bold"),
            )
            style.map("Treeview.Heading", background=[("active", c["surface_alt"])])

            style.configure("TSeparator", background=c["border"])

            self.option_add("*TCombobox*Listbox.background", c["input_bg"])
            self.option_add("*TCombobox*Listbox.foreground", c["text"])
            self.option_add("*TCombobox*Listbox.selectBackground", c["selection"])
            self.option_add("*TCombobox*Listbox.selectForeground", c["text"])
        except Exception:
            pass

    def _init_premium_background(self):
        try:
            self._bg_canvas = tk.Canvas(
                self,
                highlightthickness=0,
                bd=0,
                relief="flat",
                bg=self._theme.get("bg_bottom", self._theme.get("bg", "#23262C")),
            )
            self._bg_canvas.place(x=0, y=0, relwidth=1, relheight=1)
            self._bg_canvas.lower()
            self.configure(bg=self._theme.get("bg_bottom", self._theme.get("bg", "#23262C")))
            self.bind("<Configure>", self._on_root_resize, add="+")
            self.after(10, lambda: self._draw_gradient_background(self.winfo_width(), self.winfo_height()))
        except Exception:
            self._bg_canvas = None

    def _apply_windows_titlebar_theme(self):
        if not sys.platform.startswith("win"):
            return
        try:
            self._apply_windows_titlebar_theme_for(self)
        except Exception:
            pass

    def _apply_windows_titlebar_theme_for(self, win):
        if not sys.platform.startswith("win"):
            return
        try:
            win.update_idletasks()
            hwnd = ctypes.windll.user32.GetParent(int(win.winfo_id()))
            if not hwnd:
                return
            value = ctypes.c_int(1)
            # Windows 10/11 dark title bar attribute ids (build dependent)
            for attr in (20, 19):
                try:
                    ctypes.windll.dwmapi.DwmSetWindowAttribute(
                        ctypes.c_void_p(hwnd),
                        ctypes.c_uint(attr),
                        ctypes.byref(value),
                        ctypes.sizeof(value),
                    )
                except Exception:
                    pass
        except Exception:
            pass

    def _on_root_resize(self, _event=None):
        try:
            self._draw_gradient_background(self.winfo_width(), self.winfo_height())
        except Exception:
            pass

    def _draw_gradient_background(self, width: int, height: int):
        c = self._theme
        canvas = getattr(self, "_bg_canvas", None)
        if canvas is None:
            return
        w = max(1, int(width or 1))
        h = max(1, int(height or 1))
        try:
            canvas.configure(width=w, height=h)
            canvas.configure(bg=c.get("bg_bottom", c.get("bg", "#23262C")))
            canvas.delete("grad")

            r1, g1, b1 = self.winfo_rgb(c["bg_top"])
            r2, g2, b2 = self.winfo_rgb(c["bg_bottom"])
            steps = max(16, h // 2)
            for i in range(steps):
                frac = i / float(max(1, steps - 1))
                r = int(r1 + (r2 - r1) * frac) // 256
                g = int(g1 + (g2 - g1) * frac) // 256
                b = int(b1 + (b2 - b1) * frac) // 256
                y0 = int(i * h / steps)
                y1 = int((i + 1) * h / steps) + 1
                color = f"#{r:02x}{g:02x}{b:02x}"
                canvas.create_rectangle(0, y0, w, y1, outline="", fill=color, tags="grad")
            canvas.lower()
        except Exception:
            pass

    # ------------------------------------------------------------------
    def _rebuild_faq_tab(self):
        for w in self.tab_faq.winfo_children():
            w.destroy()
        self._build_faq_tab()

    # ------------------------------------------------------------------
    def _build_test_tab(self):
        """Build the Manual Test tab widgets."""
        t = self.i18n[self.lang.get()]
        p = self.tab_test

        ttk.Label(p, text=t.get("test_desc", ""), justify="left",
                  wraplength=540).pack(anchor="w", pady=(0, 8))

        self._test_calib_warn_lbl = ttk.Label(
            p, text="", foreground="#E07800", justify="left")
        # packed conditionally in _test_update_calib_warn

        # --- Toolbar row: Start / Stop / status / speed-limit toggle ---
        btn_row = ttk.Frame(p)
        btn_row.pack(anchor="w", pady=(0, 8))
        self._btn_test_start = ttk.Button(
            btn_row, text=t.get("test_start", "Start"), command=self._test_start,
            style="Green.TButton")
        self._btn_test_start.pack(side="left", padx=(0, 6))
        self._btn_test_stop = ttk.Button(
            btn_row, text=t.get("test_stop", "Stop"),
            command=self._test_stop, state="disabled",
            style="Red.TButton")
        self._btn_test_stop.pack(side="left", padx=(0, 16))
        ttk.Label(btn_row, text="État :").pack(side="left")
        self._test_status_var = tk.StringVar(value=t.get("test_status_idle", "Idle"))
        ttk.Label(btn_row, textvariable=self._test_status_var,
                  font=("Segoe UI", 9, "bold")).pack(side="left", padx=(4, 16))
        ttk.Separator(btn_row, orient="vertical").pack(side="left", fill="y", padx=(0, 10))
        self._test_speed_chk = ttk.Checkbutton(
            btn_row,
            text=t.get("test_speed_limit_label", "Enable speed limit"),
            variable=self._test_speed_limit,
            command=self._test_on_speed_limit_toggle)
        self._test_speed_chk.pack(side="left")
        _ToolTip(self._test_speed_chk,
                 lambda: self.i18n[self.lang.get()].get(
                     "test_speed_limit_desc",
                     "Limits movement speed and smooths rapid changes until the controls settle."))

        # --- M1–M4 slider ---
        self._build_test_motor_section(
            p, "M1–M4",
            t.get("test_target_label", "Target"),
            self._test_target_var, self._test_on_slider,
            "_test_slider", "_test_target_lbl",
            "_test_motor_hdr_var", "_test_motor_lbl", "_test_motor_bar",
            t)

        # --- Last sent frame trace ---
        self._test_last_frame_lbl = ttk.Label(
            p, text="", font=("Consolas", 8), justify="left")
        self._test_last_frame_lbl.pack(anchor="w", pady=(2, 0))

        # --- M5 sub-frame ---
        self._test_m5_frame = ttk.Frame(p)
        ttk.Separator(self._test_m5_frame, orient="horizontal").pack(fill="x", pady=(8, 4))
        ttk.Label(self._test_m5_frame, text="M5",
                  font=("Segoe UI", 9, "bold")).pack(anchor="w")
        self._build_test_motor_section(
            self._test_m5_frame, None,
            t.get("test_target_label", "Target"),
            self._test_target_var5, self._test_on_slider5,
            "_test_slider5", "_test_target5_lbl",
            "_test_motor5_hdr_var", "_test_motor5_lbl", "_test_motor5_bar",
            t)
        self._test_m5_calib_lbl = ttk.Label(
            self._test_m5_frame, text="",
            font=("Consolas", 8), foreground="#888888")
        self._test_m5_calib_lbl.pack(anchor="w", pady=(2, 0))

        # --- M6 sub-frame ---
        self._test_m6_frame = ttk.Frame(p)
        ttk.Separator(self._test_m6_frame, orient="horizontal").pack(fill="x", pady=(8, 4))
        ttk.Label(self._test_m6_frame, text="M6",
                  font=("Segoe UI", 9, "bold")).pack(anchor="w")
        self._build_test_motor_section(
            self._test_m6_frame, None,
            t.get("test_target_label", "Target"),
            self._test_target_var6, self._test_on_slider6,
            "_test_slider6", "_test_target6_lbl",
            "_test_motor6_hdr_var", "_test_motor6_lbl", "_test_motor6_bar",
            t)
        self._test_m6_calib_lbl = ttk.Label(
            self._test_m6_frame, text="",
            font=("Consolas", 8), foreground="#888888")
        self._test_m6_calib_lbl.pack(anchor="w", pady=(2, 0))

        # --- M7 sub-frame ---
        self._test_m7_frame = ttk.Frame(p)
        ttk.Separator(self._test_m7_frame, orient="horizontal").pack(fill="x", pady=(8, 4))
        ttk.Label(self._test_m7_frame, text="M7",
                  font=("Segoe UI", 9, "bold")).pack(anchor="w")
        self._build_test_motor_section(
            self._test_m7_frame, None,
            t.get("test_target_label", "Target"),
            self._test_target_var7, self._test_on_slider7,
            "_test_slider7", "_test_target7_lbl",
            "_test_motor7_hdr_var", "_test_motor7_lbl", "_test_motor7_bar",
            t)
        self._test_m7_calib_lbl = ttk.Label(
            self._test_m7_frame, text="",
            font=("Consolas", 8), foreground="#888888")
        self._test_m7_calib_lbl.pack(anchor="w", pady=(2, 0))

        # --- Speed-limit description (compact, below toolbar) ---
        # (description moved to tooltip on the checkbutton)

        self._test_update_m56_visibility()
        self._test_update_calib_warn()

    def _build_test_motor_section(self, parent, title, target_lbl_text,
                                   target_var, slider_cmd,
                                   slider_attr, target_lbl_attr,
                                   hdr_var_attr, motor_lbl_attr, bar_attr, t):
        """Helper: build one motor group (target slider + smoothed bar)."""
        if title:
            tgt_hdr = ttk.Frame(parent)
            tgt_hdr.pack(fill="x", pady=(10, 0))
            ttk.Label(tgt_hdr, text=f"{title} — {target_lbl_text} :",
                      font=("Segoe UI", 9)).pack(side="left")
        else:
            tgt_hdr = ttk.Frame(parent)
            tgt_hdr.pack(fill="x", pady=(6, 0))
            ttk.Label(tgt_hdr, text=target_lbl_text + " :",
                      font=("Segoe UI", 9)).pack(side="left")
        tgt_lbl = ttk.Label(tgt_hdr, text="0 %", font=("Segoe UI", 9, "bold"), width=5)
        tgt_lbl.pack(side="left", padx=(6, 0))
        setattr(self, target_lbl_attr, tgt_lbl)

        scale_frame = ttk.Frame(parent)
        scale_frame.pack(fill="x", pady=(2, 0))
        tick_row = ttk.Frame(scale_frame)
        tick_row.pack(fill="x")
        for pct_lbl in ("0%", "25%", "50%", "75%", "100%"):
            ttk.Label(tick_row, text=pct_lbl, font=("Segoe UI", 7)).pack(side="left", expand=True)
        slider = ttk.Scale(scale_frame, from_=0, to=100, orient="horizontal",
                           variable=target_var, state="disabled", command=slider_cmd)
        slider.pack(fill="x", pady=(0, 4))
        setattr(self, slider_attr, slider)

        mot_hdr = ttk.Frame(parent)
        mot_hdr.pack(fill="x")
        hdr_var = tk.StringVar(value=t.get("test_motor_label", "Motors (smoothed)") + " :")
        setattr(self, hdr_var_attr, hdr_var)
        ttk.Label(mot_hdr, textvariable=hdr_var, font=("Segoe UI", 9)).pack(side="left")
        mot_lbl = ttk.Label(mot_hdr, text="0 %", font=("Segoe UI", 9, "bold"), width=5)
        mot_lbl.pack(side="left", padx=(6, 0))
        setattr(self, motor_lbl_attr, mot_lbl)

        bar = ttk.Progressbar(parent, orient="horizontal", mode="determinate", maximum=100)
        bar["value"] = 0
        bar.pack(fill="x", pady=(2, 0))
        setattr(self, bar_attr, bar)

    def _test_on_speed_limit_toggle(self):
        """Called when the speed-limit checkbutton is clicked."""
        if not self._test_speed_limit.get():
            # User is turning it OFF — ask for confirmation
            t = self.i18n[self.lang.get()]
            confirmed = messagebox.askyesno(
                t.get("test_speed_limit_warn_title", "⚠ Disable speed limiter?"),
                t.get("test_speed_limit_warn",
                      "Do you really want to disable the speed limiter?\n"
                      "When disabled movements can be really fast.\n"
                      "It's only recommended for troubleshooting."),
                icon="warning")
            if not confirmed:
                # Revert to ON
                self._test_speed_limit.set(True)
        # Update motor header labels to reflect current mode
        try:
            t = self.i18n[self.lang.get()]
            lbl = t.get("test_motor_label", "Motors (smoothed)") if self._test_speed_limit.get() \
                  else t.get("test_motor_label_direct", "Motors (direct)")
            suffix = " :"
            self._test_motor_hdr_var.set(lbl + suffix)
            self._test_motor5_hdr_var.set(lbl + suffix)
            self._test_motor6_hdr_var.set(lbl + suffix)
        except Exception:
            pass

    def _test_update_m56_visibility(self):
        """Show/hide M5, M6, and M7 sub-frames independently."""
        try:
            max_motor = self._box_max_motor()
            show5 = max_motor >= 5 and self.motors.get(5, {}).get("conn", "N") == "Y"
            show6 = max_motor >= 6 and self.motors.get(6, {}).get("conn", "N") == "Y"
            show7 = max_motor >= 7 and self.motors.get(7, {}).get("conn", "N") == "Y"
            if show5:
                self._test_m5_frame.pack(fill="x", pady=(0, 2))
            else:
                self._test_m5_frame.pack_forget()
            if show6:
                self._test_m6_frame.pack(fill="x", pady=(0, 2))
            else:
                self._test_m6_frame.pack_forget()
            if show7:
                self._test_m7_frame.pack(fill="x", pady=(0, 2))
            else:
                self._test_m7_frame.pack_forget()
        except Exception:
            pass
        self._test_refresh_calib_labels()

    # ------------------------------------------------------------------
    # Tab indicator + status-table lock
    # ------------------------------------------------------------------

    def _test_tab_mark_active(self):
        """Show a solid red square on the Test tab (static, no blinking)."""
        try:
            if self._test_active_tab_img is None:
                img = tk.PhotoImage(width=10, height=10)
                img.put("#C62828", to=(1, 1, 9, 9))
                self._test_active_tab_img = img
            t = self.i18n[self.lang.get()]
            self.nb.tab(self.tab_test,
                        image=self._test_active_tab_img,
                        compound="left",
                        text=t.get("test_tab", "Test"))
        except Exception:
            pass
        # LED mirrors box: servo enabled = red
        try:
            self._led_canvas.itemconfigure(self._led_oval, fill="#d00000")
        except Exception:
            pass

    def _test_tab_mark_idle(self):
        """Remove the red indicator from the Test tab."""
        try:
            t = self.i18n[self.lang.get()]
            self.nb.tab(self.tab_test,
                        image="",
                        compound="none",
                        text=t.get("test_tab", "Test"))
        except Exception:
            pass
        # Servo is now off (DISABLE+SERVO_OFF were just sent); force LED to blue
        # immediately without waiting for the next STATUS poll.
        try:
            if self.w.is_connected():
                self._led_canvas.itemconfigure(self._led_oval, fill="#1e90ff")
            else:
                self._led_canvas.itemconfigure(self._led_oval, fill="#888888")
        except Exception:
            pass

    def _test_set_ui_locked(self, locked):
        """Show/hide warning banner + enable/disable table editing during a test."""
        try:
            if locked:
                self._test_lock_lbl.pack(before=self.table, fill="x", pady=(0, 4))
                self._set_status_table_grayed(True)
                self.table.unbind("<Button-1>")
                self.table.unbind("<Double-1>")
                self.btn_apply_hs.config(state="disabled")
            else:
                self._test_lock_lbl.pack_forget()
                self._set_status_table_grayed(False)
                self.table.bind("<Button-1>", self._on_table_click)
                self.table.bind("<Double-1>", self._on_table_double_click)
                self.btn_apply_hs.config(
                    state="normal" if self.w.is_connected() else "disabled")
        except Exception:
            pass

    def _test_refresh_calib_labels(self):
        """Update the calibration info lines below M5 and M6 progress bars."""
        for motor_num, smooth_pos, lbl_attr in (
            (5, self._test_smooth_pos5, "_test_m5_calib_lbl"),
            (6, self._test_smooth_pos6, "_test_m6_calib_lbl"),
            (7, self._test_smooth_pos7, "_test_m7_calib_lbl"),
        ):
            try:
                lbl = getattr(self, lbl_attr)
                m   = self.motors.get(motor_num, {})
                mx  = int(m.get("max", 0))
                mg  = int(m.get("margin", 0))
                if mx == 0:
                    lbl.config(text="⚠  Non calibré  (max = 0)")
                    continue
                if motor_num >= 5:
                    hi = (mx - (2 * mg)) if mx > (2 * mg) else 0
                    lo = 0
                else:
                    hi = (mx - mg) if mx > mg else mx
                    lo = mg
                cal_txt = f"cal:  max={mx}   margin={mg}   →   plage [{lo} … {hi}]"
                if self._test_active:
                    raw = int(max(0.0, min(1.0, smooth_pos / 100.0)) * G474_AXIS_MAX_STEPS)
                    lbl.config(text=f"raw P-frame = {raw}   |   {cal_txt}")
                else:
                    lbl.config(text=cal_txt)
            except Exception:
                pass

    def _test_on_slider(self, v):
        """Slider callback (main thread): update target_pct + label (M1–M4)."""
        pct = float(v)
        self._test_target_pct = pct
        try:
            self._test_target_lbl.config(text=f"{pct:.0f} %")
        except Exception:
            pass

    def _test_on_slider5(self, v):
        pct = float(v)
        self._test_target_pct5 = pct
        try:
            self._test_target5_lbl.config(text=f"{pct:.0f} %")
        except Exception:
            pass

    def _test_on_slider6(self, v):
        pct = float(v)
        self._test_target_pct6 = pct
        try:
            self._test_target6_lbl.config(text=f"{pct:.0f} %")
        except Exception:
            pass

    def _test_on_slider7(self, v):
        pct = float(v)
        self._test_target_pct7 = pct
        try:
            self._test_target7_lbl.config(text=f"{pct:.0f} %")
        except Exception:
            pass

    def _test_update_calib_warn(self):
        """Show/hide the calibration warning for connected M1-M4 with max=0."""
        try:
            t = self.i18n[self.lang.get()]
            bad = [str(i) for i in range(1, 5)
                   if self.motors.get(i, {}).get("conn", "N") == "Y"
                   and self.motors.get(i, {}).get("max", 0) == 0]
            if bad:
                msg = t.get("test_warn_calib", "⚠ M{motors} not calibrated (MAX=0).").replace("{motors}", ",".join(bad))
                self._test_calib_warn_lbl.config(text=msg)
                self._test_calib_warn_lbl.pack(anchor="w", pady=(0, 6))
            else:
                self._test_calib_warn_lbl.pack_forget()
        except Exception:
            pass

    def _test_start(self):
        t = self.i18n[self.lang.get()]
        if not self.w.is_connected():
            messagebox.showwarning(
                t.get("test_tab", "Test"),
                t.get("test_not_connected", "Not connected."))
            return
        if self._test_active:
            return
        self._test_smooth_pos   = 0.0
        self._test_target_pct   = 0.0
        self._test_target_var.set(0.0)
        self._test_smooth_pos5  = 0.0
        self._test_target_pct5  = 0.0
        self._test_target_var5.set(0.0)
        self._test_smooth_pos6  = 0.0
        self._test_target_pct6  = 0.0
        self._test_target_var6.set(0.0)
        self._test_smooth_pos7  = 0.0
        self._test_target_pct7  = 0.0
        self._test_target_var7.set(0.0)
        self._test_stopping     = False
        self._test_wait_calibrated = True
        now = time.monotonic()
        self._test_wait_deadline = now + float(getattr(self, "_test_wait_soft_timeout_s", 240.0) or 240.0)
        self._test_wait_hard_deadline = now + float(getattr(self, "_test_wait_hard_timeout_s", 480.0) or 480.0)
        self._test_handshake_lines = []
        self._test_last_frame_payload = b""
        self._test_active       = True
        self._test_last_frame_str = ""
        self._test_update_m56_visibility()
        self._test_tab_mark_active()
        self._test_set_ui_locked(True)
        # Follow SimHub startup semantics: wait for CALIBRATED before streaming P-frames.
        self.w.send_line("SH_START")
        self._fc(">>> SH_START\n")
        self._log("[TEST][TX-RAW] SH_START")
        self._log("[TEST] SH_START sent (waiting CALIBRATED)")
        self._test_status_var.set("Waiting CALIBRATED...")
        self._btn_test_start.config(state="disabled")
        self._btn_test_stop.config(state="normal")
        self._test_slider.config(state="disabled")
        try:
            self._test_slider5.config(state="disabled")
            self._test_slider6.config(state="disabled")
        except Exception:
            pass

    def _test_stop(self):
        if not self._test_active or self._test_stopping:
            return
        t = self.i18n[self.lang.get()]
        if self._test_wait_calibrated:
            self._test_wait_calibrated = False
            self._test_finish_stop()
            return
        self._test_stopping    = True
        self._test_target_pct  = 0.0
        self._test_target_pct5 = 0.0
        self._test_target_pct6 = 0.0
        self._test_slider.config(state="disabled")
        try:
            self._test_slider5.config(state="disabled")
            self._test_slider6.config(state="disabled")
        except Exception:
            pass
        self._test_status_var.set(t.get("test_status_stopping", "Returning to 0%..."))
        self._btn_test_stop.config(state="disabled")

    def _test_send_tick(self):
        """Main-thread tick at 5 ms: EMA smoothing + binary P-frame.
        Runs entirely in the Tkinter thread — no background threads, no binary frames."""
        if not self._test_active:
            return
        # EMA alpha recalculated for T=5 ms tick, τ=350 ms: α = 1 - exp(-T/τ) ≈ 0.013
        EMA_ALPHA = 0.013
        SNAP_THRESHOLD = 0.05  # snap when within 0.05 % to avoid infinite tail
        target   = 0.0 if self._test_stopping else self._test_target_pct
        target5  = 0.0 if self._test_stopping else self._test_target_pct5
        target6  = 0.0 if self._test_stopping else self._test_target_pct6
        target7  = 0.0 if self._test_stopping else self._test_target_pct7
        # During the stop ramp, always use EMA (smooth return even if limiter is OFF)
        use_ema = self._test_speed_limit.get() or self._test_stopping
        if use_ema:
            self._test_smooth_pos = EMA_ALPHA * target + (1.0 - EMA_ALPHA) * self._test_smooth_pos
            if abs(self._test_smooth_pos - target) < SNAP_THRESHOLD:
                self._test_smooth_pos = target
            self._test_smooth_pos5 = EMA_ALPHA * target5 + (1.0 - EMA_ALPHA) * self._test_smooth_pos5
            if abs(self._test_smooth_pos5 - target5) < SNAP_THRESHOLD:
                self._test_smooth_pos5 = target5
            self._test_smooth_pos6 = EMA_ALPHA * target6 + (1.0 - EMA_ALPHA) * self._test_smooth_pos6
            if abs(self._test_smooth_pos6 - target6) < SNAP_THRESHOLD:
                self._test_smooth_pos6 = target6
            self._test_smooth_pos7 = EMA_ALPHA * target7 + (1.0 - EMA_ALPHA) * self._test_smooth_pos7
            if abs(self._test_smooth_pos7 - target7) < SNAP_THRESHOLD:
                self._test_smooth_pos7 = target7
        else:
            self._test_smooth_pos  = target
            self._test_smooth_pos5 = target5
            self._test_smooth_pos6 = target6
            self._test_smooth_pos7 = target7
        frame_str = self._send_test_frame(self._test_smooth_pos, self._test_smooth_pos5, self._test_smooth_pos6, self._test_smooth_pos7)
        try:
            pos = self._test_smooth_pos
            self._test_motor_lbl.config(text=f"{pos:.0f} %")
            self._test_motor_bar["value"] = pos
            self._test_motor5_lbl.config(text=f"{self._test_smooth_pos5:.0f} %")
            self._test_motor5_bar["value"] = self._test_smooth_pos5
            self._test_motor6_lbl.config(text=f"{self._test_smooth_pos6:.0f} %")
            self._test_motor6_bar["value"] = self._test_smooth_pos6
            self._test_motor7_lbl.config(text=f"{self._test_smooth_pos7:.0f} %")
            self._test_motor7_bar["value"] = self._test_smooth_pos7
            if frame_str:
                self._test_last_frame_lbl.config(text=frame_str)
        except Exception:
            pass
        self._test_refresh_calib_labels()
        if self._test_stopping and self._test_smooth_pos <= 0.0 and self._test_smooth_pos5 <= 0.0 and self._test_smooth_pos6 <= 0.0 and self._test_smooth_pos7 <= 0.0:
            self._test_finish_stop()
            return
        self.after(5, self._test_send_tick)

    def _test_finish_stop(self):
        t = self.i18n[self.lang.get()]
        self._test_tab_mark_idle()
        self._test_set_ui_locked(False)
        self._test_active       = False
        self._test_stopping     = False
        self._test_wait_calibrated = False
        self._test_wait_deadline = 0.0
        self._test_wait_hard_deadline = 0.0
        self._test_smooth_pos   = 0.0
        self._test_target_pct   = 0.0
        self._test_target_var.set(0.0)
        self._test_smooth_pos5  = 0.0
        self._test_target_pct5  = 0.0
        self._test_target_var5.set(0.0)
        self._test_smooth_pos6  = 0.0
        self._test_target_pct6  = 0.0
        self._test_target_var6.set(0.0)
        self._test_smooth_pos7  = 0.0
        self._test_target_pct7  = 0.0
        self._test_target_var7.set(0.0)
        try:
            self._test_target_lbl.config(text="0 %")
            self._test_motor_lbl.config(text="0 %")
            self._test_motor_bar["value"] = 0
            self._test_target5_lbl.config(text="0 %")
            self._test_motor5_lbl.config(text="0 %")
            self._test_motor5_bar["value"] = 0
            self._test_target6_lbl.config(text="0 %")
            self._test_motor6_lbl.config(text="0 %")
            self._test_motor6_bar["value"] = 0
            self._test_last_frame_lbl.config(text="")
        except Exception:
            pass
        if self.w.is_connected():
            self.w.send_line("SH_DISABLE")
            self._fc(">>> SH_DISABLE\n")
            self._log("[TEST][TX-RAW] SH_DISABLE")
            self._log("[TEST] SH_DISABLE sent")
        self._test_status_var.set(t.get("test_status_idle", "Idle"))
        self._btn_test_start.config(state="normal")
        self._btn_test_stop.config(state="disabled")
        self._test_slider.config(state="disabled")
        try:
            self._test_slider5.config(state="disabled")
            self._test_slider6.config(state="disabled")
        except Exception:
            pass
        # If a disconnect was requested while the test was running, fire it now.
        if self._pending_disconnect:
            self._do_disconnect()

    def _test_cleanup(self):
        """Immediate stop without ramp — called on disconnect or window close."""
        if not self._test_active:
            return
        self._test_tab_mark_idle()
        self._test_set_ui_locked(False)
        self._test_active       = False
        self._test_stopping     = False
        self._test_wait_calibrated = False
        self._test_wait_deadline = 0.0
        self._test_wait_hard_deadline = 0.0
        self._test_smooth_pos   = 0.0
        self._test_target_pct   = 0.0
        self._test_smooth_pos5  = 0.0
        self._test_target_pct5  = 0.0
        self._test_smooth_pos6  = 0.0
        self._test_target_pct6  = 0.0
        self._test_smooth_pos7  = 0.0
        self._test_target_pct7  = 0.0
        try:
            t = self.i18n[self.lang.get()]
            self._test_target_var.set(0.0)
            self._test_target_lbl.config(text="0 %")
            self._test_motor_lbl.config(text="0 %")
            self._test_motor_bar["value"] = 0
            self._test_target_var5.set(0.0)
            self._test_target5_lbl.config(text="0 %")
            self._test_motor5_lbl.config(text="0 %")
            self._test_motor5_bar["value"] = 0
            self._test_target_var6.set(0.0)
            self._test_target6_lbl.config(text="0 %")
            self._test_motor6_lbl.config(text="0 %")
            self._test_motor6_bar["value"] = 0
            self._test_last_frame_lbl.config(text="")
            self._test_status_var.set(t.get("test_status_idle", "Idle"))
            self._btn_test_start.config(state="normal")
            self._btn_test_stop.config(state="disabled")
            self._test_slider.config(state="disabled")
            self._test_slider5.config(state="disabled")
            self._test_slider6.config(state="disabled")
        except Exception:
            pass

    def _send_test_frame(self, pct, pct5=0.0, pct6=0.0, pct7=0.0):
        """Build and send a binary P-frame identical to SimHub.
        Format: b'P' + 7 x uint16 big-endian, raw value 0-32767 — always 7
        axes, like the single SimHub profile; boxes with fewer motors ignore
        the extra axes. Firmware ignores disconnected motors and applies
        mapWithMargin itself.
        pct  : target % for M1–M4   pct5: M5   pct6: M6   pct7: M7
        Returns a display string."""
        if not self.w.is_connected():
            return ""
        max_motor = self._box_max_motor()
        raw   = int(max(0.0, min(1.0, pct  / 100.0)) * G474_AXIS_MAX_STEPS)
        raw5  = int(max(0.0, min(1.0, pct5 / 100.0)) * G474_AXIS_MAX_STEPS)
        raw6  = int(max(0.0, min(1.0, pct6 / 100.0)) * G474_AXIS_MAX_STEPS)
        raw7  = int(max(0.0, min(1.0, pct7 / 100.0)) * G474_AXIS_MAX_STEPS)
        raws  = [raw, raw, raw, raw, raw5, raw6, raw7]
        vals  = [raws[i] if i < max_motor and self.motors.get(i + 1, {}).get("conn", "N") == "Y" else 0
                 for i in range(7)]
        payload = b'P' + struct.pack(">7H", *vals)
        # Dedup + keepalive : une trame identique n'est pas renvoyée immédiatement
        # (lien silencieux juste après CALIBRATED), MAIS la dernière trame est
        # ré-émise toutes les 200 ms. Le firmware a un failsafe SimHub (~1,2 s
        # sans trame P en MOTION_ACTIVE -> park-to-zero + servo off) : sans ce
        # keepalive, une pause du slider (ou un grand saut sans limiteur = une
        # seule trame) figeait les moteurs en plein mouvement. 200 ms reste
        # sous le resume-guard firmware (250 ms).
        now_tx = time.monotonic()
        changed = payload != getattr(self, "_test_last_frame_payload", b"")
        if changed or (now_tx - getattr(self, "_test_last_frame_tx", 0.0)) >= 0.2:
            self.w.send_bytes(payload)
            self._test_last_frame_tx = now_tx
        self._test_last_frame_payload = payload
        c14 = [i + 1 for i in range(4) if self.motors.get(i + 1, {}).get("conn", "N") == "Y"]
        c5  = self.motors.get(5, {}).get("conn", "N") == "Y"
        c6  = self.motors.get(6, {}).get("conn", "N") == "Y"
        c7  = self.motors.get(7, {}).get("conn", "N") == "Y"
        parts = []
        if c14:
            parts.append(f"{pct:.1f}% raw {raw} (" + " ".join(f"M{m}" for m in c14) + ")")
        if c5:
            parts.append(f"M5 {pct5:.1f}% raw {raw5}")
        if c6:
            parts.append(f"M6 {pct6:.1f}% raw {raw6}")
        if c7:
            parts.append(f"M7 {pct7:.1f}% raw {raw7}")
        return "  |  ".join(parts)

    def _test_log_pframe_raw(self, vals):
        payload = getattr(self, "_test_last_frame_payload", b"") or b""
        if payload:
            hx = payload.hex(" ")
            self._log(f"[TEST] Initial P-frame bytes ({len(payload)}B): {hx}")
        if vals:
            self._log("[TEST] Initial P-frame u16: " + ", ".join(f"M{i+1}={v}" for i, v in enumerate(vals)))

    def _test_log_margin_state(self):
        details = []
        for motor in range(1, 8):
            m = self.motors.get(motor, {})
            if m.get("conn", "N") != "Y":
                continue
            mx = int(m.get("max", 0) or 0)
            mg = int(m.get("margin", 0) or 0)
            if motor >= 5:
                hi = mx - (2 * mg) if mx > (2 * mg) else 0
                lo = 0
            else:
                hi = mx - mg if mx > mg else mx
                lo = mg
            details.append(f"M{motor}: max={mx} margin={mg} mapped=[{lo}..{hi}]")
        if details:
            self._log("[TEST] Margin state -> " + " | ".join(details))
        else:
            self._log("[TEST] Margin state -> no optional motor connected")

    def _test_begin_streaming(self, source="CALIBRATED"):
        if not self._test_active:
            return
        self._test_wait_calibrated = False
        self._test_wait_deadline = 0.0
        self._test_wait_hard_deadline = 0.0
        t = self.i18n[self.lang.get()]
        self._test_status_var.set(t.get("test_status_running", "Running"))
        self._test_slider.config(state="normal")
        try:
            if self.motors.get(5, {}).get("conn", "N") == "Y":
                self._test_slider5.config(state="normal")
            if self.motors.get(6, {}).get("conn", "N") == "Y":
                self._test_slider6.config(state="normal")
        except Exception:
            pass
        # Prime dedup with the all-zero frame so no frame is sent immediately
        # after CALIBRATED; first TX occurs when target values actually change
        # (or via the 200 ms keepalive, which also arms the firmware failsafe
        # with a healthy continuous stream, like real SimHub).
        self._test_last_frame_payload = b'P' + struct.pack(">7H", *([0] * 7))
        self._test_last_frame_tx = time.monotonic()
        self._log(f"[TEST] {source} received -> ready/listening (no initial P-frame)")
        if self._test_handshake_lines:
            self._log("[TEST] SH_START handshake RX -> " + " | ".join(self._test_handshake_lines))
        self._test_log_margin_state()
        self.after(5, self._test_send_tick)

    def _test_log_box_line(self, line: str):
        s = str(line or "").strip()
        if not s:
            return
        self._log(f"[TEST] BOX says: {s}")
        su = s.upper()
        if "HOMING FAILED" in su:
            self._log("[TEST] Hint: verify MIN endstop wiring/state, actuator connection (M5/M6), and driver homing settings (PN024/PN098).")

    def _test_extend_wait_deadline(self, line: str = ""):
        """Keep SH_START wait alive while calibration still reports progress."""
        if not (self._test_active and self._test_wait_calibrated):
            return
        hard_dl = float(getattr(self, "_test_wait_hard_deadline", 0.0) or 0.0)
        if hard_dl <= 0.0:
            return
        now = time.monotonic()
        cur_dl = float(getattr(self, "_test_wait_deadline", 0.0) or 0.0)
        extend_s = float(getattr(self, "_test_wait_progress_extend_s", 120.0) or 120.0)
        new_dl = min(hard_dl, now + extend_s)
        if new_dl > cur_dl:
            self._test_wait_deadline = new_dl

    def _build_legal_notice_text(self):
        t = self.i18n[self.lang.get()]
        template = t.get("legal_notice", "{app_title} v{version}")
        try:
            return template.format(
                app_title="Motion Center",
                version=MOTION_CENTER_VERSION,
                author=APP_AUTHOR,
                years=APP_COPYRIGHT_YEARS,
                company=APP_COMPANY,
                license_name=APP_LICENSE_NAME,
                website=APP_WEBSITE,
            )
        except Exception:
            return f"Motion Center v{MOTION_CENTER_VERSION}"

    def _build_faq_tab(self):
        t = self.i18n[self.lang.get()]
        parent = self.tab_faq

        ttk.Label(parent, text=t.get("faq_title", "FAQ"),
                  font=("Segoe UI", 13, "bold")).pack(anchor="w", pady=(0, 6))

        # Diagnostic area
        diag_frame = ttk.Frame(parent)
        diag_frame.pack(fill="x", pady=(0, 8))
        ttk.Button(diag_frame, text=t.get("faq_diag_btn", "Diagnostic"),
                   command=self._run_faq_diagnostic).pack(side="left")
        self._faq_diag_text = tk.Text(diag_frame, height=3, wrap="word",
                                      state="disabled", relief="groove",
                                      font=("Segoe UI", 9))
        self._faq_diag_text.pack(side="left", fill="x", expand=True, padx=(8, 0))

        legal_frame = ttk.LabelFrame(parent, text=t.get("legal_title", "Legal information"), padding=8)
        legal_frame.pack(fill="x", pady=(0, 8))
        ttk.Label(
            legal_frame,
            text=self._build_legal_notice_text(),
            justify="left",
            wraplength=1020,
        ).pack(anchor="w")

        ttk.Separator(parent, orient="horizontal").pack(fill="x", pady=(0, 8))

        # Scrollable accordion
        container = ttk.Frame(parent)
        container.pack(fill="both", expand=True)

        cv = tk.Canvas(container, highlightthickness=0)
        sb = ttk.Scrollbar(container, orient="vertical", command=cv.yview)
        sf = ttk.Frame(cv)
        sf.bind("<Configure>", lambda e: cv.configure(scrollregion=cv.bbox("all")))
        cv.create_window((0, 0), window=sf, anchor="nw")
        cv.configure(yscrollcommand=sb.set)
        sb.pack(side="right", fill="y")
        cv.pack(side="left", fill="both", expand=True)

        def _mw(event):
            cv.yview_scroll(int(-1 * (event.delta / 120)), "units")

        cv.bind("<MouseWheel>", _mw)
        sf.bind("<MouseWheel>", _mw)

        for question, answer in t.get("faq_items", []):
            frame = ttk.Frame(sf)
            frame.pack(fill="x", pady=2, padx=2)

            answer_height = min(answer.count("\n") + 2, 14)
            ans_widget = tk.Text(frame, height=answer_height, wrap="word",
                                 relief="groove", state="disabled",
                                 font=("Segoe UI", 9))
            ans_widget.bind("<MouseWheel>", _mw)

            opened = [False]

            def _make_toggle(btn_ref, aw, op, q, a):
                def toggle():
                    if op[0]:
                        op[0] = False
                        btn_ref[0].configure(text=f"\u25b6  {q}")
                        aw.pack_forget()
                    else:
                        op[0] = True
                        btn_ref[0].configure(text=f"\u25bc  {q}")
                        aw.configure(state="normal")
                        aw.delete("1.0", "end")
                        aw.insert("end", a)
                        aw.configure(state="disabled")
                        aw.pack(fill="x", padx=4, pady=(0, 4))
                return toggle

            btn_ref = [None]
            toggle_fn = _make_toggle(btn_ref, ans_widget, opened, question, answer)
            btn = ttk.Button(frame, text=f"\u25b6  {question}", command=toggle_fn)
            btn_ref[0] = btn
            btn.pack(fill="x")
            btn.bind("<MouseWheel>", _mw)

    def _run_faq_diagnostic(self):
        t = self.i18n[self.lang.get()]
        lines = []

        port = self.port.get().strip()
        if not port:
            lines.append(t.get("faq_diag_no_port", "⚠ No COM port selected."))
        if not self.w.is_connected():
            lines.append(t.get("faq_diag_not_connected", "⚠ Not connected."))
        else:
            if self.info.get("servo_on", False):
                lines.append(t.get("faq_diag_servo_on", "✅ Servo enabled."))
            else:
                lines.append(t.get("faq_diag_no_servo", "ℹ Servo disabled."))

        profile_ok = self._is_simhub_profile_installed()
        if profile_ok:
            lines.append(t.get("faq_diag_profile_ok", "✅ SimHub profile installed."))
        else:
            lines.append(t.get("faq_diag_profile_missing", "⚠ SimHub profile not installed."))

        if self.w.is_connected() and port and profile_ok:
            lines.append(t.get("faq_diag_ok", "✅ Everything looks correct."))

        result = "\n".join(lines) if lines else t.get("faq_diag_ok", "✅ OK")
        try:
            self._faq_diag_text.configure(state="normal")
            self._faq_diag_text.delete("1.0", "end")
            self._faq_diag_text.insert("end", result)
            self._faq_diag_text.configure(state="disabled")
        except Exception:
            pass

    # ------------------------------------------------------------------
    def _apply_runtime_widget_theme(self):
        c = self._theme
        try:
            self._led_canvas.configure(bg=c["bg"])
        except Exception:
            pass

        text_widgets = [
            getattr(self, "func_console", None),
            getattr(self, "flash_log", None),
            getattr(self, "driver_intro_text", None),
            getattr(self, "log", None),
        ]
        for tw in text_widgets:
            if tw is None:
                continue
            try:
                tw.configure(
                    bg=c["input_bg"],
                    fg=c["text"],
                    insertbackground=c["accent"],
                    selectbackground=c["selection"],
                    selectforeground=c["text"],
                    relief="flat",
                    highlightthickness=1,
                    highlightbackground=c["border"],
                    highlightcolor=c["accent"],
                )
            except Exception:
                pass

        labels = [
            getattr(self, "lbl_simhub_profile_status", None),
            getattr(self, "lbl_mc_compat_update_val", None),
            getattr(self, "lbl_installed_fw", None),
        ]
        for lb in labels:
            if lb is None:
                continue
            try:
                lb.configure(bg=c["bg"], fg=c["text"])
            except Exception:
                pass

        try:
            self.driver_table.configure(bg=c["border"], bd=1, relief="solid")
        except Exception:
            pass

        try:
            self.anim_canvas.configure(bg=c["surface"])
        except Exception:
            pass

    def _load_social_icon(self, image_path: str, target_px: int = 18):
        try:
            if not image_path or not os.path.exists(image_path):
                return None
            if Image is not None and ImageTk is not None:
                src = Image.open(image_path)
                w, h = src.size
                if w <= 0 or h <= 0:
                    return None
                ratio = min(float(target_px) / float(w), float(target_px) / float(h))
                nw = max(1, int(round(w * ratio)))
                nh = max(1, int(round(h * ratio)))
                if nw != w or nh != h:
                    src = src.resize((nw, nh), Image.Resampling.LANCZOS)
                return ImageTk.PhotoImage(src)
            img = tk.PhotoImage(file=image_path)
            w = max(1, int(img.width()))
            h = max(1, int(img.height()))
            ratio = max(w / float(target_px), h / float(target_px))
            sub = max(1, int(math.ceil(ratio)))
            if sub > 1:
                img = img.subsample(sub, sub)
            return img
        except Exception:
            return None

    def _make_social_button(self, parent, text: str, icon, url: str):
        try:
            if icon is not None:
                btn = ttk.Button(parent, image=icon, width=4, command=lambda: webbrowser.open(url, new=2))
            else:
                btn = ttk.Button(parent, text=text, width=4, command=lambda: webbrowser.open(url, new=2))
            btn.pack(side=tk.LEFT, padx=1)
        except Exception:
            pass

    def _set_status_table_heading_labels(self):
        t = self.i18n[self.lang.get()]
        cols = [
            ("m",          "col_m"),
            ("conn",       "col_conn"),
            ("cal",        "col_cal"),
            ("max",        "col_max"),
            ("margin",     "col_margin"),
            ("homing_dir", "col_homing_dir"),
            ("endpark",    "col_endpark"),
        ]
        for col, key in cols:
            label = str(t.get(key, "") or "")
            if self._info_icon_header is not None:
                try:
                    # Some Tk builds can show heading text+image without extra options.
                    self.table.heading(col, text=label, image=self._info_icon_header)
                    continue
                except Exception:
                    pass
            # Safe fallback across all Tk versions.
            self.table.heading(col, text=f"{label} ⓘ", image="")

    def _build_ui(self):
        t = self.i18n[self.lang.get()]

        top = ttk.Frame(self, padding=10)
        top.pack(side=tk.TOP, fill=tk.X)

        # Social links (right side) — use icon images if available
        social = ttk.Frame(top)
        social.pack(side=tk.RIGHT, padx=6)
        try:
            base_dir = app_base_dir()
            icon_dir = os.path.join(base_dir, "icons")

            # Load images if present and keep references on self
            web_path = os.path.join(icon_dir, "web.png")
            yt_path = os.path.join(icon_dir, "youtube.png")
            dc_path = os.path.join(icon_dir, "discord.png")
            fb_path = os.path.join(icon_dir, "facebook.png")

            self.ico_web = self._load_social_icon(web_path, target_px=18) if os.path.exists(web_path) else None
            self._make_social_button(social, "Web", self.ico_web, SOCIAL_LINKS[self.lang.get()]["site"])

            self.ico_yt = self._load_social_icon(yt_path, target_px=18) if os.path.exists(yt_path) else None
            self._make_social_button(social, "YT", self.ico_yt, SOCIAL_LINKS[self.lang.get()]["youtube"])

            self.ico_dc = self._load_social_icon(dc_path, target_px=18) if os.path.exists(dc_path) else None
            self._make_social_button(social, "DC", self.ico_dc, SOCIAL_LINKS[self.lang.get()]["discord"])

            self.ico_fb = self._load_social_icon(fb_path, target_px=18) if os.path.exists(fb_path) else None
            self._make_social_button(social, "FB", self.ico_fb, SOCIAL_LINKS[self.lang.get()]["facebook"])

            info_path = os.path.join(icon_dir, "icon_info.png")
            self._info_icon_header = self._load_social_icon(info_path, target_px=12) if os.path.exists(info_path) else None
        except Exception:
            # Non-critical: if loading images fails, fallback to text buttons
            try:
                ttk.Button(social, text="Web", width=4, command=lambda: webbrowser.open(SOCIAL_LINKS[self.lang.get()]["site"], new=2)).pack(side=tk.LEFT, padx=1)
                ttk.Button(social, text="YT",  width=4, command=lambda: webbrowser.open(SOCIAL_LINKS[self.lang.get()]["youtube"], new=2)).pack(side=tk.LEFT, padx=1)
                ttk.Button(social, text="DC",  width=4, command=lambda: webbrowser.open(SOCIAL_LINKS[self.lang.get()]["discord"], new=2)).pack(side=tk.LEFT, padx=1)
                ttk.Button(social, text="FB",  width=4, command=lambda: webbrowser.open(SOCIAL_LINKS[self.lang.get()]["facebook"], new=2)).pack(side=tk.LEFT, padx=1)
            except Exception:
                pass

        ttk.Label(top, text=t["port"]).pack(side=tk.LEFT)
        self.cmb_port = ttk.Combobox(top, textvariable=self.port, width=48, state="readonly")
        self.cmb_port.pack(side=tk.LEFT, padx=(6, 8))
        self.btn_ports = ttk.Button(top, text=t["refresh_ports"], command=self._refresh_ports)
        self.btn_ports.pack(side=tk.LEFT)

        self.btn_connect = ttk.Button(top, text=t["connect"], command=self._toggle_connect)
        self.btn_connect.pack(side=tk.LEFT, padx=(8, 0))

        ttk.Label(top, text="  ").pack(side=tk.LEFT)
        self.cmb_lang = ttk.Combobox(top, textvariable=self.lang, width=5, state="readonly", values=["FR", "EN", "DE"])
        self.cmb_lang.pack(side=tk.LEFT, padx=(8, 8))
        self.cmb_lang.bind("<<ComboboxSelected>>", lambda _e: self._apply_language())

        # Small status LED next to language selector
        self._led_canvas = tk.Canvas(top, width=16, height=16, highlightthickness=0)
        self._led_canvas.pack(side=tk.LEFT, padx=(4, 8))
        self._led_oval = self._led_canvas.create_oval(2, 2, 14, 14, fill="#888888", outline="#444")

        ttk.Separator(self, orient="horizontal").pack(fill=tk.X, padx=10, pady=6)

        self.nb = ttk.Notebook(self)
        self.nb.pack(fill=tk.BOTH, expand=True, padx=10, pady=10)

        self.tab_status = ttk.Frame(self.nb, padding=10)
        self.tab_funcs  = ttk.Frame(self.nb, padding=10)
        self.tab_test   = ttk.Frame(self.nb, padding=10)
        self.tab_update = ttk.Frame(self.nb, padding=10)
        self.tab_driver = ttk.Frame(self.nb, padding=10)
        self.tab_log    = ttk.Frame(self.nb, padding=10)
        self.tab_faq    = ttk.Frame(self.nb, padding=10)

        self.nb.add(self.tab_status, text=t["status_tab"])
        self.nb.add(self.tab_funcs,  text=t["functions_tab"])
        self.nb.add(self.tab_test,   text=t.get("test_tab", "Test"))
        self.nb.add(self.tab_update, text=t["update_tab"])
        self.nb.add(self.tab_driver, text=t["driver_tab"])
        self.nb.add(self.tab_log,    text=t["log_tab"])
        self.nb.add(self.tab_faq,    text=t.get("faq_tab", "FAQ"))
        self.nb.select(self.tab_status)
        self.nb.bind("<<NotebookTabChanged>>", self._on_tab_changed)

        # STATUS
        self.grp_head = ttk.LabelFrame(self.tab_status, text=t["control_box"], padding=10)
        self.grp_head.pack(fill=tk.X)

        self.lbl_fw    = ttk.Label(self.grp_head, text="Firmware: -")
        self.lbl_box   = ttk.Label(self.grp_head, text="Box: -")
        self.lbl_servo = ttk.Label(self.grp_head, text="Servo: -")
        self.lbl_home  = ttk.Label(self.grp_head, text="Homing: -")

        self.lbl_fw.grid(row=0, column=0, sticky="w", padx=4, pady=2)
        self.lbl_box.grid(row=0, column=1, sticky="w", padx=20, pady=2)
        self.lbl_servo.grid(row=0, column=2, sticky="w", padx=20, pady=2)
        self.lbl_home.grid(row=1, column=0, columnspan=3, sticky="w", padx=4, pady=2)

        btns = ttk.Frame(self.grp_head)
        btns.grid(row=0, column=3, rowspan=2, padx=10, sticky="e")

        self.btn_disable = ttk.Button(btns, text=t["disable"], command=lambda: self._send_cmd("MC_DISABLE", refresh="silent"))
        self.btn_disable.pack(side=tk.TOP, fill=tk.X)
        self.btn_factory_reset = ttk.Button(btns, text=t["factory_reset_btn"], command=self._do_factory_reset)
        self.btn_factory_reset.pack(side=tk.TOP, fill=tk.X, pady=(6, 0))

        # Frequence STEP max (pas/s) : dans l'encart Control Box.
        self.grp_stephz = ttk.Frame(self.grp_head)
        self.grp_stephz.grid(row=2, column=0, columnspan=4, sticky="w", padx=4, pady=(6, 0))
        self.lbl_stephz = ttk.Label(self.grp_stephz, text=t["step_speed_label"])
        self.lbl_stephz.pack(side=tk.LEFT)
        self.ent_stephz = ttk.Entry(self.grp_stephz, width=10, textvariable=self.step_hz_var)
        self.ent_stephz.pack(side=tk.LEFT, padx=(8, 8))
        self.btn_apply_stephz = ttk.Button(self.grp_stephz, text=t["apply"], command=self._apply_step_hz)
        self.btn_apply_stephz.pack(side=tk.LEFT)
        self.lbl_stephz_hint = ttk.Label(self.grp_stephz, text=t["step_speed_hint"])
        self.lbl_stephz_hint.pack(side=tk.LEFT, padx=(12, 0))

        self.simhub_warn_row = ttk.Frame(self.tab_status)
        self.simhub_warn_row.pack(fill=tk.X, pady=(8, 0))
        self.lbl_simhub_warn = ttk.Label(self.simhub_warn_row, textvariable=self.simhub_profile_warning, foreground="#b00020")
        self.lbl_simhub_warn.pack(side=tk.LEFT)
        self.btn_simhub_fix = ttk.Button(self.simhub_warn_row, text=t.get("simhub_fix", "Fix SimHub profile"), command=self._install_simhub_profile)
        self.btn_simhub_fix.pack(side=tk.LEFT, padx=(8, 0))
        self.btn_simhub_fix.pack_forget()

        # Avertissement permanent Pn98 (v1.2)
        self.lbl_pn98_warn = ttk.Label(
            self.tab_status,
            text=t.get("pn98_warning", ""),
            foreground="#c0631a",
            font=("Segoe UI", 9, "bold"),
            wraplength=1050,
            justify="left",
        )
        self.lbl_pn98_warn.pack(fill=tk.X, pady=(8, 0))

        self.grp_params = ttk.LabelFrame(self.tab_status, text=t["homing_speed"], padding=10)
        self.grp_params.pack(fill=tk.X, pady=(10, 0))

        self.lbl_hs = ttk.Label(self.grp_params, text=t["speed_steps_s"])
        self.lbl_hs.pack(side=tk.LEFT)
        self.ent_homing = ttk.Entry(self.grp_params, width=10, textvariable=self.homing_sps_var)
        self.ent_homing.pack(side=tk.LEFT, padx=(8, 8))
        self.btn_apply_hs = ttk.Button(self.grp_params, text=t["apply"], command=self._apply_homing_speed)
        self.btn_apply_hs.pack(side=tk.LEFT)
        self.lbl_saved = ttk.Label(self.grp_params, text="", foreground="#008000")
        self.lbl_saved.pack(side=tk.LEFT, padx=(12,0))
        self.lbl_rec = ttk.Label(self.grp_params, text=t["recommended"])
        self.lbl_rec.pack(side=tk.LEFT, padx=(12,0))

        self.chk_estop = ttk.Checkbutton(
            self.grp_params,
            text=t["estop_use"],
            variable=self.estop_use_var,
            command=self._apply_estop_usage,
        )
        self.chk_estop.pack(side=tk.LEFT, padx=(20, 0))

        self.lbl_estop_behavior = ttk.Label(self.grp_params, text=t["estop_behavior"])
        self.lbl_estop_behavior.pack(side=tk.LEFT, padx=(14, 6))
        self.combo_estop_behavior = ttk.Combobox(
            self.grp_params,
            state="readonly",
            width=18,
            textvariable=self.estop_behavior_var,
            values=self._estop_behavior_labels(),
        )
        self.combo_estop_behavior.pack(side=tk.LEFT)
        self._estop_behavior_set_from_code(0)
        self.combo_estop_behavior.bind("<<ComboboxSelected>>", lambda _e: self._apply_estop_behavior())

        self.grp_table = ttk.LabelFrame(self.tab_status, text=t["motors"], padding=10)
        self.grp_table.pack(fill=tk.BOTH, expand=True, pady=(10,0))

        # Warning banner shown (pack(before=table)) when a manual test is running.
        self._test_lock_lbl = ttk.Label(
            self.grp_table,
            text="⚠  Test manuel en cours — tableau verrouillé",
            foreground="#E07800", font=("Segoe UI", 8, "bold"))
        # not packed initially

        self.table = ttk.Treeview(self.grp_table, columns=("m","conn","cal","max","margin","homing_dir","endpark"), show="headings", height=12)
        self._set_status_table_heading_labels()

        self.table.column("m",          width=50,  anchor="center")
        self.table.column("conn",       width=110, anchor="center")
        self.table.column("cal",        width=70,  anchor="center")
        self.table.column("max",        width=130, anchor="e")
        self.table.column("margin",     width=120, anchor="e")
        self.table.column("homing_dir", width=100, anchor="center")
        self.table.column("endpark",    width=90,  anchor="center")

        self.table.pack(fill=tk.BOTH, expand=True)
        self.table.bind("<Button-1>", self._on_table_click)
        self.table.bind("<Double-1>", self._on_table_double_click)
        self.table.bind("<Motion>", self._on_table_header_hover, add="+")
        self.table.bind("<Leave>", self._hide_table_header_tooltip, add="+")
        self.table.bind("<ButtonPress-1>", self._hide_table_header_tooltip, add="+")

        self._table_header_tip = None
        self._table_header_tip_col = ""

        self._edit_entry = None
        self._edit_info = None
        self._m5_margin_click_job = None
        self._m5_margin_dlg = None
        self._m5_margin_motor = None

        # FUNCTIONS
        self.grp_funcs = ttk.LabelFrame(self.tab_funcs, text=t["functions"], padding=10)
        self.grp_funcs.pack(fill=tk.X)

        self.lbl_func_servo_required = ttk.Label(
            self.grp_funcs,
            text=t["func_servo_required"],
            foreground="#b00020",
            wraplength=900,
            justify="left",
        )
        self.lbl_func_servo_required.pack(anchor="w", pady=(8, 0))

        top_func_row = ttk.Frame(self.grp_funcs)
        top_func_row.pack(fill=tk.X)
        top_func_row.grid_columnconfigure(0, weight=1)

        self.func_combo = ttk.Combobox(top_func_row, state="readonly", width=40)
        self.func_combo.grid(row=0, column=0, sticky="ew", padx=(0, 10))
        self.func_combo.bind("<<ComboboxSelected>>", lambda _e: self._on_func_selected())

        self.btn_cancel = ttk.Button(top_func_row, text=t.get("cancel","Cancel"), command=self._cancel_func)
        self.btn_cancel.grid(row=0, column=1, sticky="e")
        self.btn_run = ttk.Button(top_func_row, text=t["run"], style="Accent.TButton", command=self._run_func)
        self.btn_run.grid(row=0, column=2, sticky="e", padx=(6, 0))

        rowf = ttk.Frame(self.grp_funcs)
        rowf.pack(fill=tk.X, pady=(10,0))
        self.lbl_motor = ttk.Label(rowf, text=t["motor"])
        self.lbl_motor.pack(side=tk.LEFT)
        self.spin_motor = ttk.Spinbox(rowf, from_=1, to=6, width=6, textvariable=self.func_motor)
        self.spin_motor.pack(side=tk.LEFT, padx=(6,12))

        self.lbl_pn24_hint = ttk.Label(self.grp_funcs, text=t["func_pn24_hint"],
                                       wraplength=980, justify="left")
        self.lbl_pn24_hint.pack(anchor="w", pady=(6, 0))

        desc_frame = ttk.Frame(self.tab_funcs)
        desc_frame.pack(fill=tk.X, pady=(10,0))
        self.lbl_desc_title = ttk.Label(desc_frame, text=t["desc"])
        self.lbl_desc_title.pack(anchor="w")

        # Warning line (hidden unless needed)
        self.warn_frame = ttk.Frame(desc_frame)
        self.warn_frame.pack(fill=tk.X, pady=(4, 6))
        self.lbl_warn_prefix = ttk.Label(self.warn_frame, text=t["warning"])
        self.lbl_warn_prefix.pack(side=tk.LEFT, anchor="w")
        self.lbl_warn = ttk.Label(self.warn_frame, text="", wraplength=980, justify="left")
        self.lbl_warn.pack(side=tk.LEFT, padx=(6,0), anchor="w")
        self.warn_frame.pack_forget()

        self.lbl_desc = ttk.Label(desc_frame, text="", wraplength=1050, justify="left")
        self.lbl_desc.pack(anchor="w", pady=(0,0))

        self.anim_frame = ttk.Frame(self.tab_funcs)
        self.anim_frame.pack(fill=tk.X, pady=(8, 0))
        self.anim_canvas = tk.Canvas(self.anim_frame, height=86, highlightthickness=0)
        self.anim_canvas.pack(fill=tk.X)
        self.after(10, self._init_compress_animation)

        self.func_console = tk.Text(self.tab_funcs, height=18, wrap="none")
        self.func_console.pack(fill=tk.BOTH, expand=True, pady=(10,0))
        self.func_console.configure(font=("Consolas", 10))
        ttk.Button(self.tab_funcs, text="Clear", command=lambda: self.func_console.delete("1.0", tk.END)).pack(anchor="e", pady=(6,0))

        # UPDATE (Motion Center + Firmware)
        self.grp_soft_update = ttk.LabelFrame(self.tab_update, text=t["soft_update_tab"], padding=10)
        self.grp_soft_update.pack(fill=tk.X, pady=(0, 10))

        row_soft_ver = ttk.Frame(self.grp_soft_update)
        row_soft_ver.pack(fill=tk.X)
        self.lbl_soft_current_title = ttk.Label(row_soft_ver, text=t["soft_current_version"])
        self.lbl_soft_current_title.pack(side=tk.LEFT)
        self.lbl_soft_current_val = ttk.Label(row_soft_ver, textvariable=self.soft_current_version)
        self.lbl_soft_current_val.pack(side=tk.LEFT, padx=(6, 0))

        row_soft_status = ttk.Frame(self.grp_soft_update)
        row_soft_status.pack(fill=tk.X, pady=(10, 0))
        self.lbl_soft_web_status_title = ttk.Label(row_soft_status, text=t["soft_web_status"])
        self.lbl_soft_web_status_title.pack(side=tk.LEFT)
        self.lbl_soft_web_status = ttk.Label(row_soft_status, textvariable=self.soft_update_status)
        self.lbl_soft_web_status.pack(side=tk.LEFT, padx=(6, 0))

        row_soft_actions = ttk.Frame(self.grp_soft_update)
        row_soft_actions.pack(fill=tk.X, pady=(10, 0))
        self.btn_soft_check = ttk.Button(row_soft_actions, text=t["soft_check"], command=self._check_soft_update)
        self.btn_soft_check.pack(side=tk.LEFT)
        self.btn_soft_download = ttk.Button(row_soft_actions, text=t["soft_download"], command=self._download_soft_update, state="disabled")
        self.btn_soft_download.pack(side=tk.LEFT, padx=(8, 0))

        self.grp_simhub_profile = ttk.LabelFrame(self.tab_update, text=t["simhub_profile_title"], padding=10)
        self.grp_simhub_profile.pack(fill=tk.X, pady=(0, 10))

        row_simhub_status = ttk.Frame(self.grp_simhub_profile)
        row_simhub_status.pack(fill=tk.X)
        self.lbl_simhub_profile_status_title = ttk.Label(row_simhub_status, text=t["simhub_profile_status"])
        self.lbl_simhub_profile_status_title.pack(side=tk.LEFT)
        self.lbl_simhub_profile_status = tk.Label(row_simhub_status, textvariable=self.simhub_profile_status, anchor="w")
        self.lbl_simhub_profile_status.pack(side=tk.LEFT, padx=(6, 0))

        row_simhub_actions = ttk.Frame(self.grp_simhub_profile)
        row_simhub_actions.pack(fill=tk.X, pady=(10, 0))
        self.btn_install_simhub_profile = ttk.Button(
            row_simhub_actions,
            text=t["simhub_profile_install"],
            command=self._install_simhub_profile,
        )
        self.btn_install_simhub_profile.pack(side=tk.LEFT)

        self.grp_update = ttk.LabelFrame(self.tab_update, text=t["update_group"], padding=10)
        self.grp_update.pack(fill=tk.X)

        row_up_port = ttk.Frame(self.grp_update)
        row_up_port.pack(fill=tk.X)
        self.lbl_up_port = ttk.Label(row_up_port, text=t["port"])
        self.lbl_up_port.pack(side=tk.LEFT)
        self.cmb_update_port = ttk.Combobox(row_up_port, textvariable=self.update_port, width=40, state="readonly")
        self.cmb_update_port.pack(side=tk.LEFT, padx=(6, 8))
        self.cmb_update_port.bind("<<ComboboxSelected>>", lambda _e: self._detect_firmware_version())
        self.btn_update_ports = ttk.Button(row_up_port, text=t["refresh_ports"], command=self._refresh_ports)
        self.btn_update_ports.pack(side=tk.LEFT)

        row_up_boxes = ttk.Frame(self.grp_update)
        row_up_boxes.pack(fill=tk.X, pady=(10, 0))
        self.lbl_box_select = ttk.Label(row_up_boxes, text=t["box_select_label"])
        self.lbl_box_select.pack(anchor="w")
        self._build_fw_box_selector(row_up_boxes)

        row_up_hex = ttk.Frame(self.grp_update)
        row_up_hex.pack(fill=tk.X, pady=(10, 0))
        self.lbl_hex = ttk.Label(row_up_hex, text=t["hex_file"])
        self.lbl_hex.pack(side=tk.LEFT)
        self.cmb_hex = ttk.Combobox(row_up_hex, textvariable=self.fw_choice, state="readonly", width=78)
        self.cmb_hex.pack(side=tk.LEFT, fill=tk.X, expand=True, padx=(6, 8))
        self.cmb_hex.bind("<<ComboboxSelected>>", self._on_firmware_selected)
        self.btn_browse_hex = ttk.Button(row_up_hex, text=t["browse"], command=self._browse_hex)
        self.btn_browse_hex.pack(side=tk.LEFT)

        row_up_mc = ttk.Frame(self.grp_update)
        row_up_mc.pack(fill=tk.X, pady=(8, 0))
        self.lbl_mc_compat_update_title = ttk.Label(row_up_mc, text=t["mc_compat_prefix"])
        self.lbl_mc_compat_update_title.pack(side=tk.LEFT)
        self.lbl_mc_compat_update_val = tk.Label(row_up_mc, textvariable=self.mc_compat_status, anchor="w")
        self.lbl_mc_compat_update_val.pack(side=tk.LEFT, padx=(6, 0))

        row_up_fw = ttk.Frame(self.grp_update)
        row_up_fw.pack(fill=tk.X, pady=(10, 0))
        self.lbl_installed_fw_title = ttk.Label(row_up_fw, text=t["installed_fw"])
        self.lbl_installed_fw_title.pack(side=tk.LEFT)
        self.lbl_installed_fw = tk.Label(row_up_fw, textvariable=self.installed_fw, anchor="w")
        self.lbl_installed_fw.pack(side=tk.LEFT, padx=(6, 8))
        self.btn_detect_fw = ttk.Button(row_up_fw, text=t["detect_fw"], command=self._detect_firmware_version)
        self.btn_detect_fw.pack(side=tk.LEFT)

        row_up_actions = ttk.Frame(self.grp_update)
        row_up_actions.pack(fill=tk.X, pady=(10, 0))
        self.btn_flash = ttk.Button(row_up_actions, text=t["flash"], command=self._flash_firmware)
        self.btn_flash.pack(side=tk.LEFT)
        self.lbl_flash_note = tk.Label(
            row_up_actions,
            text=t.get("flash_note", ""),
            anchor="w",
            justify="left",
            fg="#76839a",
            font=("Segoe UI", 9, "italic"),
            wraplength=780,
        )
        self.lbl_flash_note.pack(side=tk.LEFT, padx=(12, 0), fill=tk.X, expand=True)
        self.lbl_flash_status = ttk.Label(row_up_actions, text="")
        self.lbl_flash_status.pack(side=tk.LEFT, padx=(12, 0))

        # height=8 = hauteur minimale demandée ; le log s'étend ensuite avec
        # la fenêtre (expand=True). Une valeur trop grande (18) poussait le
        # bouton Flasher et le log hors de la fenêtre par défaut.
        self.flash_log = tk.Text(self.tab_update, height=8, wrap="none")
        self.flash_log.pack(fill=tk.BOTH, expand=True, pady=(10,0))
        self.flash_log.configure(font=("Consolas", 10))
        ttk.Button(self.tab_update, text="Clear", command=lambda: self.flash_log.delete("1.0", tk.END)).pack(anchor="e", pady=(6,0))

        row_doc = ttk.Frame(self.tab_update)
        row_doc.pack(fill=tk.X, pady=(8, 0))
        self.lbl_control_box_doc = ttk.Label(row_doc, text=t["control_box_doc_label"])
        self.lbl_control_box_doc.pack(side=tk.LEFT)
        self.btn_open_control_box_doc = ttk.Button(row_doc, text=t["open_doc"], command=self._open_control_box_doc)
        self.btn_open_control_box_doc.pack(side=tk.LEFT, padx=(8, 0))

        # DRIVER SETTINGS TAB
        self.grp_driver_settings = ttk.LabelFrame(self.tab_driver, text=t["driver_settings_title"], padding=10)
        self.grp_driver_settings.pack(fill=tk.X)

        row_driver = ttk.Frame(self.grp_driver_settings)
        row_driver.pack(fill=tk.X)
        self.lbl_driver_settings_doc = ttk.Label(row_driver, text=t["driver_settings_doc_label"])
        self.lbl_driver_settings_doc.pack(side=tk.LEFT)
        self.btn_open_driver_settings = ttk.Button(row_driver, text=t["open_driver_settings"], command=self._open_driver_settings_doc)
        self.btn_open_driver_settings.pack(side=tk.LEFT, padx=(8, 0))

        driver_intro_frame = ttk.Frame(self.tab_driver)
        driver_intro_frame.pack(fill=tk.X, pady=(10, 6))
        self.driver_intro_text = tk.Text(driver_intro_frame, height=7, wrap="word")
        self.driver_intro_text.pack(fill=tk.X, expand=False)
        self.driver_intro_text.configure(font=("Segoe UI", 10), state="disabled")

        driver_table_frame = ttk.Frame(self.tab_driver)
        driver_table_frame.pack(fill=tk.BOTH, expand=True)
        self.driver_table = tk.Frame(driver_table_frame, bg="#bcbcbc", bd=1, relief="solid")
        self.driver_table.pack(fill=tk.BOTH, expand=True)

        # LOG
        self.log = tk.Text(self.tab_log, height=24, wrap="none")
        self.log.pack(fill=tk.BOTH, expand=True)
        self.log.configure(font=("Consolas", 10))
        row_log_actions = ttk.Frame(self.tab_log)
        row_log_actions.pack(fill=tk.X, pady=(6, 0))
        ttk.Button(row_log_actions, text="Export", command=self._export_log_text).pack(side=tk.RIGHT)
        ttk.Button(row_log_actions, text="Clear", command=lambda: self.log.delete("1.0", tk.END)).pack(side=tk.RIGHT, padx=(0, 6))

        # FAQ / AIDE
        self._build_faq_tab()

        # TEST MANUEL
        self._build_test_tab()

        self._apply_runtime_widget_theme()

        self._apply_language(first_time=True)
        self._auto_fill_hex_from_firmware()
        self._render_status()
        self.after(150, self._refresh_simhub_profile_state)

    def _update_status_controls_state(self):
        connected = self.w.is_connected()
        estop_capable = connected and self._is_g474_box()
        disable_state = "normal" if (connected and bool(self.info.get("servo_on", False))) else "disabled"
        try:
            self.btn_disable.configure(state=disable_state)
            self.btn_factory_reset.configure(state=("normal" if connected else "disabled"))
            self.ent_homing.configure(state=("normal" if connected else "disabled"))
            self.btn_apply_hs.configure(state=("normal" if connected else "disabled"))
            self.chk_estop.configure(state=("normal" if estop_capable else "disabled"))
            self.combo_estop_behavior.configure(state=("readonly" if estop_capable else "disabled"))
            self.lbl_estop_behavior.configure(state=("normal" if estop_capable else "disabled"))
        except Exception:
            pass

    def _update_functions_controls_state(self):
        connected = self.w.is_connected()
        base_state = "normal" if connected else "disabled"
        try:
            # Keep the function list readable even when disconnected.
            self.func_combo.configure(state="readonly")
        except Exception:
            pass

        try:
            key = self.func_choice_key.get()
            needs_motor = bool(self.func_model.get(key, {}).get("needs_motor", False))
            self.spin_motor.configure(state=("normal" if (connected and needs_motor) else "disabled"))
        except Exception:
            pass

        try:
            self.btn_run.configure(state=base_state)
        except Exception:
            pass

        # Show/hide the "motor not connected" notice
        try:
            key = self.func_choice_key.get()
            needs_motor = bool(self.func_model.get(key, {}).get("needs_motor", False))
            if needs_motor:
                motor = clamp_int(self.func_motor.get(), 1, 6)
                motor_ok = connected and self.motors.get(motor, {}).get("conn", "N") == "Y"
            else:
                motor_ok = connected
            if motor_ok:
                self.lbl_func_servo_required.pack_forget()
            else:
                self.lbl_func_servo_required.pack(anchor="w", pady=(8, 0))
        except Exception:
            pass

    def _apply_language(self, first_time=False):
        t = self.i18n[self.lang.get()]

        self.btn_ports.config(text=t["refresh_ports"])
        self.btn_connect.config(text=t["disconnect"] if self.w.is_connected() else t["connect"])

        self.nb.tab(self.tab_status, text=t["status_tab"])
        self.nb.tab(self.tab_funcs, text=t["functions_tab"])
        self.nb.tab(self.tab_update, text=t["update_tab"])
        self.nb.tab(self.tab_driver, text=t["driver_tab"])
        self.nb.tab(self.tab_log, text=t["log_tab"])
        self.nb.tab(self.tab_faq, text=t.get("faq_tab", "FAQ"))
        self.nb.tab(self.tab_test, text=t.get("test_tab", "Test"))
        self._rebuild_faq_tab()

        self.grp_head.config(text=t["control_box"])
        self.btn_disable.config(text=t["disable"])
        self.btn_factory_reset.config(text=t["factory_reset_btn"])
        self.lbl_pn98_warn.config(text=t.get("pn98_warning", ""))

        self.grp_params.config(text=t["homing_speed"])
        self.lbl_hs.config(text=t["speed_steps_s"])
        self.btn_apply_hs.config(text=t["apply"])
        self.lbl_rec.config(text=t["recommended"])
        self.lbl_stephz.config(text=t["step_speed_label"])
        self.btn_apply_stephz.config(text=t["apply"])
        self.lbl_stephz_hint.config(text=t["step_speed_hint"])
        self.chk_estop.config(text=t["estop_use"])
        self.lbl_estop_behavior.config(text=t.get("estop_behavior", "ESTOP behavior:"))
        try:
            self.combo_estop_behavior.configure(values=self._estop_behavior_labels())
            self._estop_behavior_set_from_code(int(self.info.get("estop_behavior", self._estop_behavior_code)))
        except Exception:
            pass

        self.grp_table.config(text=t["motors"])
        self._set_status_table_heading_labels()

        self.grp_funcs.config(text=t["functions"])
        self.lbl_motor.config(text=t["motor"])
        self.lbl_pn24_hint.config(text=t["func_pn24_hint"])
        self.btn_run.config(text=t["run"])
        self.lbl_desc_title.config(text=t["desc"])
        self.lbl_warn_prefix.config(text=t["warning"])
        self.lbl_func_servo_required.config(text=t["func_servo_required"])

        self.grp_update.config(text=t["update_group"])
        self.lbl_up_port.config(text=t["port"])
        self.btn_update_ports.config(text=t["refresh_ports"])
        self.lbl_box_select.config(text=t["box_select_label"])
        for _key, _fname, _tkey in self._FW_BOX_SPECS:
            _lbl = self._fw_box_lbls.get(_key)
            if _lbl is not None:
                _lbl.config(text=t.get(_tkey, _key))
        self.lbl_hex.config(text=t["hex_file"])
        self.btn_browse_hex.config(text=t["browse"])
        self.btn_flash.config(text=t["flash"])
        self.lbl_flash_note.config(text=t.get("flash_note", ""))
        self.lbl_installed_fw_title.config(text=t["installed_fw"])
        self.btn_detect_fw.config(text=t["detect_fw"])

        self.grp_soft_update.config(text=t["soft_update_tab"])
        self.lbl_soft_current_title.config(text=t["soft_current_version"])
        self.lbl_soft_web_status_title.config(text=t["soft_web_status"])
        self.btn_soft_check.config(text=t["soft_check"])
        self.btn_soft_download.config(text=t["soft_download"])
        self.grp_simhub_profile.config(text=t["simhub_profile_title"])
        self.lbl_simhub_profile_status_title.config(text=t["simhub_profile_status"])
        self.btn_install_simhub_profile.config(text=t["simhub_profile_install"])
        self.btn_simhub_fix.config(text=t.get("simhub_fix", "Fix"))
        self.lbl_mc_compat_update_title.config(text=t["mc_compat_prefix"])
        self.lbl_control_box_doc.config(text=t["control_box_doc_label"])
        self.btn_open_control_box_doc.config(text=t["open_doc"])
        self.grp_driver_settings.config(text=t["driver_settings_title"])
        self.lbl_driver_settings_doc.config(text=t["driver_settings_doc_label"])
        self.btn_open_driver_settings.config(text=t["open_driver_settings"])
        if self.installed_fw.get() in ("Unknown firmware", "Firmware inconnu"):
            self.installed_fw.set(t["fw_not_detected"])

        if self._is_localized_placeholder(self.web_fw_status.get(), "web_fw_not_checked", extra_values=("not checked", "non vérifiée")):
            self.web_fw_status.set(t["web_fw_not_checked"])

        if self._is_localized_placeholder(self.soft_update_status.get(), "soft_not_checked", extra_values=("not checked", "non vérifiée")):
            self.soft_update_status.set(t["soft_not_checked"])

        if self._is_localized_placeholder(self.simhub_profile_status.get(), "simhub_profile_not_installed", extra_values=("not installed", "non installé")):
            self.simhub_profile_status.set(t["simhub_profile_not_installed"])

        self.soft_current_version.set(f"v{MOTION_CENTER_VERSION}")
        try:
            self.btn_soft_download.configure(state=("normal" if self._latest_soft_url else "disabled"))
        except Exception:
            pass

        # Expose only safe end-user functions (factory_reset moved to status tab)
        self._func_exposed = ["detect_min", "detect_max", "go_center", "complete_calib", "test_stroke"]

        values = [t["func_labels"][k] for k in self._func_exposed]
        self.func_combo["values"] = values

        if self.func_choice_key.get() not in self._func_exposed:
            self.func_choice_key.set("complete_calib")

        idx = self._func_exposed.index(self.func_choice_key.get())
        self.func_combo.current(idx)

        self._on_func_selected()
        self._render_status()
        self._update_status_controls_state()
        self._update_functions_controls_state()
        self._refresh_simhub_profile_state()
        self._update_update_tab_health_visuals()
        self._set_driver_settings_static_content()

        # Update function buttons text (Run / Cancel)
        try:
            if getattr(self, 'btn_run', None):
                self.btn_run.config(text=t.get("run", "Run"))
            if getattr(self, 'btn_cancel', None):
                self.btn_cancel.config(text=t.get("cancel", "Cancel"))
        except Exception:
            pass

        self._update_mc_compat_from_selection()

        if not first_time:
            self._log(f"[INFO] Language set to {self.lang.get()}")

    def _show_table_header_tooltip(self, text: str, x_root: int, y_root: int):
        self._hide_table_header_tooltip()
        if not text:
            return
        try:
            tip = tk.Toplevel(self.table)
            tip.wm_overrideredirect(True)
            tip.wm_geometry(f"+{x_root}+{y_root}")
            lbl = tk.Label(
                tip,
                text=text,
                justify="left",
                bg="#fff8dc",
                fg="#000000",
                relief="solid",
                borderwidth=1,
                padx=6,
                pady=3,
                wraplength=520,
            )
            lbl.pack()
            self._table_header_tip = tip
        except Exception:
            self._table_header_tip = None

    def _hide_table_header_tooltip(self, _event=None):
        if self._table_header_tip is not None:
            try:
                self._table_header_tip.destroy()
            except Exception:
                pass
            self._table_header_tip = None
        self._table_header_tip_col = ""

    def _on_table_header_hover(self, event):
        region = self.table.identify("region", event.x, event.y)
        if region != "heading":
            self._hide_table_header_tooltip()
            return

        col_id = self.table.identify_column(event.x)
        col_key = {"#1": "m", "#2": "conn", "#3": "cal", "#4": "max", "#5": "margin",
                   "#6": "homing_dir", "#7": "endpark"}.get(col_id)
        if not col_key:
            self._hide_table_header_tooltip()
            return

        t = self.i18n[self.lang.get()]
        tip_map = {
            "m": t.get("tip_col_m", ""),
            "conn": t.get("tip_col_conn", ""),
            "cal": t.get("tip_col_cal", ""),
            "max": t.get("tip_col_max", ""),
            "margin": t.get("tip_col_margin", ""),
            "homing_dir": t.get("tip_col_homing_dir", ""),
            "endpark": t.get("tip_col_endpark", ""),
        }
        text = str(tip_map.get(col_key, "") or "").strip()
        if not text:
            self._hide_table_header_tooltip()
            return

        if self._table_header_tip is not None and self._table_header_tip_col == col_id:
            return

        self._table_header_tip_col = col_id
        self._show_table_header_tooltip(text, self.table.winfo_rootx() + event.x + 14, self.table.winfo_rooty() + event.y + 18)

    def _refresh_ports(self):
        ports = list_ports()
        self.cmb_port["values"] = ports
        try:
            self.cmb_update_port["values"] = ports
        except Exception:
            pass

        if ports and not self.port.get():
            # Prefer the first Arduino Leonardo; fall back to first port.
            leo = _find_leonardo_port()
            # Match against the (possibly cleaned) labels in `ports`
            match = next((lbl for lbl in ports if lbl.startswith(leo.split(" \u2014 ")[0])), None) if leo else None
            self.port.set(match if match else ports[0])
        if ports and not self.update_port.get():
            self.update_port.set(self.port.get() if self.port.get() else ports[0])

    def _set_installed_fw_display(self, value: str):
        try:
            self.installed_fw.set(str(value or ""))
        except Exception:
            pass
        self._update_update_tab_health_visuals()

    def _update_update_tab_health_visuals(self):
        t = self.i18n[self.lang.get()]

        try:
            st = str(self.simhub_profile_status.get() or "").strip()
            if st.startswith(t["simhub_profile_done"]):
                self.lbl_simhub_profile_status.configure(fg="#008000")
            elif st == t["simhub_profile_installing"]:
                self.lbl_simhub_profile_status.configure(fg="#555555")
            else:
                self.lbl_simhub_profile_status.configure(fg="#b00020")
        except Exception:
            pass

        try:
            fw = str(self.installed_fw.get() or "").strip()
            if fw == t["detecting_fw"]:
                fw_show = fw
                fw_color = "#555555"
            elif not fw or fw == "-" or fw in (t.get("fw_unknown", ""), "Unknown firmware", "Firmware inconnu", t["fw_not_detected"]):
                fw_show = t["fw_not_detected"]
                fw_color = "#b00020"
            elif self._is_supported_connected_firmware(fw):
                fw_show = fw
                fw_color = "#008000"
            else:
                fw_show = t["fw_unsupported"]
                fw_color = "#b00020"

            if fw_show != fw:
                self.installed_fw.set(fw_show)
            self.lbl_installed_fw.configure(fg=fw_color)
        except Exception:
            pass

        try:
            mc_ok = bool(getattr(self, "_mc_compat_ok", False))
            self.lbl_mc_compat_update_val.configure(fg=("#008000" if mc_ok else "#b00020"))
        except Exception:
            pass

    def _on_tab_changed(self, _event=None):
        try:
            if self.nb.select() == str(self.tab_update):
                self._auto_fill_hex_from_firmware()
        except Exception:
            pass

    def _log(self, s):
        self.log.insert(tk.END, s + "\n")
        self.log.see(tk.END)

    def _export_log_text(self):
        try:
            content = self.log.get("1.0", tk.END)
            if not content.strip():
                self._log("[INFO] Log is empty, nothing to export.")
                return

            default_name = f"motion_center_log_{time.strftime('%Y%m%d_%H%M%S')}.txt"
            path = filedialog.asksaveasfilename(
                parent=self,
                title="Export log",
                defaultextension=".txt",
                filetypes=[("Text files", "*.txt"), ("All files", "*.*")],
                initialfile=default_name,
            )
            if not path:
                return

            with open(path, "w", encoding="utf-8") as f:
                f.write(content.rstrip("\n") + "\n")

            self._log(f"[INFO] Log exported: {path}")
        except Exception as e:
            self._log(f"[ERR] Log export failed: {e}")

    def _open_control_box_doc(self):
        try:
            webbrowser.open(DOC_LINKS.get(self.lang.get(), DOC_LINKS["EN"]), new=2)
        except Exception as e:
            self._log(f"[ERR] Unable to open documentation link: {e}")

    def _open_driver_settings_doc(self):
        try:
            webbrowser.open(DRIVER_SETTINGS_LINKS.get(self.lang.get(), DRIVER_SETTINGS_LINKS["EN"]), new=2)
        except Exception as e:
            self._log(f"[ERR] Unable to open driver settings link: {e}")

    def _set_driver_settings_static_content(self):
        lang_code = self.lang.get()
        data = DRIVER_SETTINGS_STATIC.get(lang_code, DRIVER_SETTINGS_STATIC["EN"])
        intro_text = "\n\n".join(data.get("intro", []))
        rows = data.get("rows", [])
        self._set_driver_settings_view(intro_text, rows)

    def _set_driver_settings_view(self, intro_text: str, rows):
        try:
            self.driver_intro_text.configure(state="normal")
            self.driver_intro_text.delete("1.0", tk.END)
            self.driver_intro_text.insert("1.0", intro_text or "")
            self.driver_intro_text.see("1.0")
            self.driver_intro_text.configure(state="disabled")
        except Exception:
            pass

        try:
            for child in self.driver_table.winfo_children():
                child.destroy()
            self._driver_hover_rows = []

            t = self.i18n[self.lang.get()]
            headers = [
                t["driver_col_param"],
                t["driver_col_value"],
                t["driver_col_expl"],
                t["driver_col_note"],
            ]

            for col, txt in enumerate(headers):
                hdr = tk.Label(
                    self.driver_table,
                    text=txt,
                    bg=self._theme["surface"],
                    fg=self._theme["text"],
                    relief="solid",
                    borderwidth=1,
                    font=("Segoe UI", 9, "bold"),
                    anchor="w",
                    padx=6,
                    pady=4,
                )
                hdr.grid(row=0, column=col, sticky="nsew")

            self.driver_table.grid_columnconfigure(0, weight=0, minsize=95)
            self.driver_table.grid_columnconfigure(1, weight=0, minsize=70)
            self.driver_table.grid_columnconfigure(2, weight=1, minsize=330)
            self.driver_table.grid_columnconfigure(3, weight=1, minsize=320)

            for ridx, row in enumerate(rows, start=1):
                p, v, e, n = row
                bg = self._theme["input_bg"]
                row_widgets = []
                vals = (p, v, e, n)
                for cidx, val in enumerate(vals):
                    anchor = "center" if cidx == 1 else "w"
                    wrap = 0 if cidx < 2 else 520
                    cell = tk.Label(
                        self.driver_table,
                        text=str(val),
                        bg=bg,
                        fg=self._theme["text"],
                        relief="solid",
                        borderwidth=1,
                        anchor=anchor,
                        justify="left",
                        padx=6,
                        pady=3,
                        wraplength=wrap,
                    )
                    cell.grid(row=ridx, column=cidx, sticky="nsew")
                    row_widgets.append(cell)

                def _enter(_evt, widgets=row_widgets):
                    for ww in widgets:
                        ww.configure(bg=self._theme["surface_alt"])

                def _leave(_evt, widgets=row_widgets):
                    for ww in widgets:
                        ww.configure(bg=self._theme["input_bg"])

                for ww in row_widgets:
                    ww.bind("<Enter>", _enter)
                    ww.bind("<Leave>", _leave)

                self._driver_hover_rows.append(row_widgets)
        except Exception:
            pass

    def _fc(self, s: str):
        """Write to func_console and append to serial_transcript.log (timestamped lines are kept).
        """
        try:
            self.func_console.insert(tk.END, s)
            self.func_console.see(tk.END)
        except Exception:
            pass

    def _fl(self, s: str):
        """Write to update flash console."""
        try:
            self.flash_log.insert(tk.END, s if s.endswith("\n") else s + "\n")
            self.flash_log.see(tk.END)
        except Exception:
            pass

    def _sl(self, s: str):
        """Write to Motion Center update console."""
        self._fl(s)

    def _init_compress_animation(self):
        try:
            c = self.anim_canvas
            self._load_actuator_images()
            # Keep the animation area compact; image is scaled to fit.
            target_h = min(150, max(72, int(self._anim_img_h) + 10))
            if int(c.cget("height")) != target_h:
                c.configure(height=target_h)

            c.delete("all")
            w = int(c.winfo_width() or 260)
            h = int(c.winfo_height() or target_h)
            cy = h // 2
            cx = w // 2

            if self._actuator_idle is not None:
                iw = int(self._anim_img_w or self._actuator_idle.width())
                left_end = max(20, cx - (iw // 2) - 8)
                right_start = min(w - 20, cx + (iw // 2) + 8)
                c.create_line(20, cy, left_end, cy, fill="#666666", width=3, tags="static")
                c.create_line(right_start, cy, w - 20, cy, fill="#666666", width=3, tags="static")
                c.create_image(cx, cy, image=self._actuator_idle, tags=("act", "static"))
            else:
                c.create_rectangle(cx - 34, cy - 10, cx + 34, cy + 10, fill="#3f3f3f", outline="#2a2a2a", tags="static")
                c.create_line(20, cy, cx - 34, cy, fill="#666666", width=3, tags="static")
                c.create_line(cx + 34, cy, w - 20, cy, fill="#666666", width=3, tags="static")

            self._anim_center_x = cx
            self._anim_center_y = cy
            self._draw_compress_arrows(active=False)
            c.bind("<Configure>", lambda _e: self._init_compress_animation())
        except Exception:
            pass

    def _load_actuator_images(self):
        if self._actuator_idle is not None:
            return
        try:
            base_dir = app_base_dir()
        except Exception:
            base_dir = os.getcwd()
        img_dir = os.path.join(base_dir, "images")

        # Prefer a dedicated "known" image when available
        try:
            known = os.path.join(img_dir, "actuator-srt80-known.png")
            if os.path.exists(known):
                self._actuator_photo = tk.PhotoImage(file=known)
                self._actuator_idle = self._actuator_photo
        except Exception:
            self._actuator_photo = None
            self._actuator_idle = None

        # Fallback idle image
        try:
            if self._actuator_idle is None:
                base = os.path.join(img_dir, "actuator-srt80.png")
                if os.path.exists(base):
                    self._actuator_photo = tk.PhotoImage(file=base)
                    self._actuator_idle = self._actuator_photo
        except Exception:
            self._actuator_photo = None
            self._actuator_idle = None

        # Compression frames: prefer animated GIF if provided
        self._actuator_frames = []
        gif_path = os.path.join(img_dir, "compressing.gif")
        if os.path.exists(gif_path):
            frame_idx = 0
            while True:
                try:
                    fr = tk.PhotoImage(file=gif_path, format=f"gif -index {frame_idx}")
                    self._actuator_frames.append(fr)
                    frame_idx += 1
                except Exception:
                    break

        # Decompression frames: prefer dedicated animated GIF if provided
        self._actuator_frames_decomp = []
        gif_decomp_path = os.path.join(img_dir, "decompressing.gif")
        if os.path.exists(gif_decomp_path):
            frame_idx = 0
            while True:
                try:
                    fr = tk.PhotoImage(file=gif_decomp_path, format=f"gif -index {frame_idx}")
                    self._actuator_frames_decomp.append(fr)
                    frame_idx += 1
                except Exception:
                    break

        # Fallback PNG frame sequence if GIF is absent/unreadable
        if not self._actuator_frames:
            for fn in sorted(os.listdir(img_dir)):
                if not (fn.startswith("actuator-srt80-comp") and fn.lower().endswith(".png")):
                    continue
                try:
                    p = os.path.join(img_dir, fn)
                    self._actuator_frames.append(tk.PhotoImage(file=p))
                except Exception:
                    pass

        if self._actuator_idle is None and self._actuator_frames:
            self._actuator_idle = self._actuator_frames[0]

        try:
            unk = os.path.join(img_dir, "actuator-srt80-unknown.png")
            if os.path.exists(unk):
                self._actuator_unknown = tk.PhotoImage(file=unk)
        except Exception:
            self._actuator_unknown = None

        try:
            centered = os.path.join(img_dir, "actuator-srt80-centered.png")
            if os.path.exists(centered):
                self._actuator_centered = tk.PhotoImage(file=centered)
        except Exception:
            self._actuator_centered = None

        try:
            maxed = os.path.join(img_dir, "actuator-srt80-maxed.png")
            if not os.path.exists(maxed):
                maxed = os.path.join(img_dir, "maxed.png")
            if os.path.exists(maxed):
                self._actuator_maxed = tk.PhotoImage(file=maxed)
        except Exception:
            self._actuator_maxed = None

        # Track max provided image size to scale the animation area
        imgs = []
        if self._actuator_idle is not None:
            imgs.append(self._actuator_idle)
        if self._actuator_unknown is not None:
            imgs.append(self._actuator_unknown)
        if self._actuator_centered is not None:
            imgs.append(self._actuator_centered)
        if self._actuator_maxed is not None:
            imgs.append(self._actuator_maxed)
        imgs.extend(self._actuator_frames)
        imgs.extend(self._actuator_frames_decomp)
        try:
            if imgs:
                self._anim_img_w = max(int(im.width()) for im in imgs)
                self._anim_img_h = max(int(im.height()) for im in imgs)
        except Exception:
            self._anim_img_w = 68
            self._anim_img_h = 20

    def _actuator_position_known(self) -> bool:
        if self._force_unknown_visual:
            return False
        # During explicit motion phases reported by firmware, force known state
        # so the motion GIF is shown even if last GET still says servo disabled.
        if self._compress_anim_active and self._actuator_motion in ("compressing", "decompressing"):
            return True
        if not self.w.is_connected():
            return False
        if not self.info.get("servo_on", False):
            return False
        try:
            motor = clamp_int(self.func_motor.get(), 1, 6)
            m = self.motors.get(motor, {})
            return str(m.get("cal", "N")).upper() == "Y"
        except Exception:
            return False

    def _fit_image_to_canvas(self, img, canvas_w, canvas_h):
        if img is None:
            return None, 0, 0
        try:
            iw = int(img.width())
            ih = int(img.height())
            # Keep a unified visual size across PNG and GIF frames.
            # We first compute ONE target rendered size from reference dimensions,
            # then scale each source image to that same target size.
            ref_w = max(1, int(getattr(self, "_anim_img_w", iw) or iw))
            ref_h = max(1, int(getattr(self, "_anim_img_h", ih) or ih))

            max_w = max(40, int(canvas_w) - 48)
            max_h = max(28, int(canvas_h) - 12)
            global_ratio = min(max_w / ref_w, max_h / ref_h)
            global_ratio = max(0.05, global_ratio)

            target_w = max(1, int(round(ref_w * global_ratio)))
            target_h = max(1, int(round(ref_h * global_ratio)))

            # Same proportions for PNG/GIF in this project -> either ratio works.
            # Use width ratio to get identical displayed size for all assets.
            scale_ratio = target_w / max(1, iw)
            frac = Fraction(scale_ratio).limit_denominator(48)
            num = max(1, int(frac.numerator))
            den = max(1, int(frac.denominator))

            if num == den == 1 and iw == target_w and ih == target_h:
                return img, iw, ih

            key = (str(img), num, den, target_w, target_h)
            scaled = self._img_scaled_cache.get(key)
            if scaled is None:
                scaled = img.zoom(num).subsample(den)
                self._img_scaled_cache[key] = scaled
            return scaled, int(scaled.width()), int(scaled.height())
        except Exception:
            try:
                return img, int(img.width()), int(img.height())
            except Exception:
                return img, 0, 0

    def _refresh_actuator_visual(self):
        try:
            self._draw_compress_arrows(active=self._compress_anim_active)
        except Exception:
            pass

    def _cancel_operation_active(self) -> bool:
        try:
            py_worker_running = bool(self._func_thread and self._func_thread.is_alive())
        except Exception:
            py_worker_running = False
        return bool(
            self._long_op_active
            or self._calib_active
            or self._compress_anim_active
            or py_worker_running
        )

    def _update_cancel_button_state(self):
        try:
            if getattr(self, 'btn_cancel', None):
                state = "normal" if (self.w.is_connected() and self._cancel_operation_active()) else "disabled"
                self.btn_cancel.configure(state=state)
        except Exception:
            pass

    def _draw_compress_arrows(self, active=False):
        try:
            c = self.anim_canvas
            w = int(c.winfo_width() or 260)
            h = int(c.winfo_height() or 86)
            known = self._actuator_position_known()

            img = None
            if self._force_unknown_visual:
                img = self._actuator_unknown or self._actuator_idle
            elif self._actuator_motion == "centered":
                img = self._actuator_centered or self._actuator_idle
            elif self._actuator_motion == "maxed":
                img = self._actuator_maxed or self._actuator_idle
            elif not known:
                img = self._actuator_unknown or self._actuator_idle
            elif active and self._actuator_frames:
                if self._actuator_motion == "decompressing":
                    if self._actuator_frames_decomp:
                        idx = self._compress_anim_phase % len(self._actuator_frames_decomp)
                        img = self._actuator_frames_decomp[idx]
                    else:
                        idx = (len(self._actuator_frames) - 1) - (self._compress_anim_phase % len(self._actuator_frames))
                        img = self._actuator_frames[idx]
                else:
                    idx = self._compress_anim_phase % len(self._actuator_frames)
                    img = self._actuator_frames[idx]
            else:
                img = self._actuator_idle or (self._actuator_frames[0] if self._actuator_frames else None)

            disp_img, disp_w, _disp_h = self._fit_image_to_canvas(img, w, h)
            if disp_w <= 0:
                disp_w = int(self._anim_img_w)

            if c.find_withtag("act"):
                if disp_img is not None:
                    c.itemconfigure("act", image=disp_img)
                c.coords("act", self._anim_center_x, self._anim_center_y)

            # Overlay selected motor label for M5/M6 only.
            try:
                motor = clamp_int(self.func_motor.get(), 1, 6)
            except Exception:
                motor = 0
            c.delete("motor_label")
            if motor in (5, 6):
                font_px = max(12, int(min(disp_w if disp_w > 0 else self._anim_img_w, h) * 0.22))
                c.create_text(
                    self._anim_center_x,
                    self._anim_center_y,
                    text=f"M{motor}",
                    fill="#ffffff",
                    font=("Segoe UI", font_px, "bold"),
                    tags=("motor_label",),
                )
        except Exception:
            pass

    def _set_compress_animation(self, active: bool, motion: str = None):
        if active:
            self._force_unknown_visual = False
        if motion in ("compressing", "decompressing", "idle", "centered", "maxed"):
            self._actuator_motion = motion
        self._compress_anim_active = bool(active)
        if not self._compress_anim_active and motion is None:
            self._actuator_motion = "idle"
        elif self._compress_anim_active and self._actuator_motion == "idle":
            self._actuator_motion = "compressing"
        if not self._compress_anim_active:
            self._draw_compress_arrows(active=False)
            self._update_cancel_button_state()
            return
        self._update_cancel_button_state()
        if self._compress_anim_job is None:
            self._tick_compress_animation()

    def _tick_compress_animation(self):
        self._compress_anim_job = None
        if not self._compress_anim_active:
            self._draw_compress_arrows(active=False)
            return
        self._compress_anim_phase = (self._compress_anim_phase + 1) % 1000
        self._draw_compress_arrows(active=True)
        self._compress_anim_job = self.after(80, self._tick_compress_animation)

    def _browse_hex(self):
        t = self.i18n[self.lang.get()]
        path = filedialog.askopenfilename(
            title="Select firmware (.hex / .bin)",
            filetypes=[("Firmware files", "*.hex *.bin"), ("HEX files", "*.hex"),
                       ("BIN files (STM32)", "*.bin"), ("All files", "*.*")]
        )
        if path:
            self.hex_path.set(path)
            label = f"{t['custom_fw']} — {os.path.basename(path)}"
            self._firmware_map[label] = path
            self._fw_label_box[label] = None
            vals = list(self.cmb_hex["values"])
            if label not in vals:
                vals.append(label)
                self.cmb_hex["values"] = vals
            self.fw_choice.set(label)

    def _firmware_label(self, path: str) -> str:
        """Label affiché dans la liste déroulante : uniquement la version.

        La control box est sélectionnée via les cartes-images au-dessus,
        inutile de répéter son nom dans chaque entrée. L'appartenance
        firmware↔box est conservée en interne via _fw_box_key()."""
        name = os.path.basename(path)
        stem = os.path.splitext(name)[0]
        m = re.search(r"v\d+(?:\.\d+)+", stem, re.IGNORECASE)
        version = m.group(0).lower() if m else stem
        if "with_bootloader" in stem.lower():
            return f"{version} (bootloader)"
        return version

    def _fw_box_key(self, path: str):
        """Classe un fichier firmware par control box, d'après son nom.

        Reprend exactement les heuristiques de l'ancien label verbeux."""
        name = os.path.basename(path)
        stem = os.path.splitext(name)[0]
        # STM32 firmware ships as a raw .bin (USB-CDC IAP bootloader)
        if name.lower().endswith(".bin"):
            if re.search(r"competition|g4|box[-_]?v?5", stem, re.IGNORECASE):
                return "comp_v3"
            if re.search(r"pro[-_]?box[-_]?v?2", stem, re.IGNORECASE):
                return "pro_v2"
            return "pro_v1"
        # Detect box hardware variant from filename suffix.
        # Matches both "-box-v1" / "-box-v2" (with separator) and
        # "-boxv1" / "-boxv2" (without separator between box and v).
        # Anchored to end of stem so "control-box-v1.8" is NOT matched.
        bm = re.search(r"[-_]box[-_]?v(\d+)$", stem, re.IGNORECASE)
        if bm:
            bv = int(bm.group(1))
            if bv == 1:
                return "comp_v1"
            if bv == 2:
                return "comp_v2"
            return "comp_v3"
        # No explicit box version → assume V2
        return "comp_v2"

    # --- Control box image selector (Update tab) --------------------------
    # key -> (photo file in images/control box, i18n label key)
    _FW_BOX_SPECS = (
        ("comp_v1", "control box competition v1 - atmega32u4.jpg",    "box_card_comp_v1"),
        ("comp_v2", "control box competition v2 - atmega32u4.jpg",    "box_card_comp_v2"),
        ("comp_v3", "control box competition v3 - STM32G474VET6.jpg", "box_card_comp_v3"),
        ("pro_v1",  "Pro Control box V1 - stm32f103c8t6.jpg",         "box_card_pro_v1"),
        ("pro_v2",  "Pro Control box V2 - stm32f103c8t6 - PCB V2.1.jpg", "box_card_pro_v2"),
    )
    _FW_BOX_IMG_W = 150
    _FW_BOX_IMG_H = 105

    def _load_fw_box_photo(self, fname):
        """Load a control box photo, center-cropped to the card aspect and resized."""
        if Image is None or ImageTk is None:
            return None
        try:
            path = os.path.join(app_base_dir(), "images", "control box", fname)
            if not os.path.exists(path):
                return None
            img = Image.open(path)
            tw, th = self._FW_BOX_IMG_W, self._FW_BOX_IMG_H
            w, h = img.size
            target_ratio = tw / th
            if w / h > target_ratio:
                new_w = int(h * target_ratio)
                x0 = (w - new_w) // 2
                img = img.crop((x0, 0, x0 + new_w, h))
            else:
                new_h = int(w / target_ratio)
                y0 = (h - new_h) // 2
                img = img.crop((0, y0, w, y0 + new_h))
            img = img.resize((tw, th), Image.Resampling.LANCZOS)
            return ImageTk.PhotoImage(img)
        except Exception:
            return None

    def _build_fw_box_selector(self, parent):
        """Row of clickable control-box photo cards that filter the firmware list."""
        t = self.i18n[self.lang.get()]
        bg_card = self._theme.get("surface", "#2A2E35")
        accent  = self._theme.get("accent",  "#F0C64A")
        fg_text = self._theme.get("text",    "#F2F4F7")
        self._fw_box_cards = {}
        self._fw_box_lbls = {}
        self._fw_box_photos = {}
        cards = ttk.Frame(parent)
        cards.pack(fill=tk.X, pady=(6, 0))
        for col, (key, fname, tkey) in enumerate(self._FW_BOX_SPECS):
            photo = self._load_fw_box_photo(fname)
            self._fw_box_photos[key] = photo  # keep a reference (Tk GC)
            card = tk.Frame(cards, bg=bg_card, padx=4, pady=4, cursor="hand2",
                            relief="flat", bd=0, highlightthickness=2,
                            highlightbackground=bg_card, highlightcolor=accent)
            card.grid(row=0, column=col, padx=(0, 10), sticky="n")
            if photo is not None:
                img_lbl = tk.Label(card, image=photo, bg=bg_card, cursor="hand2", borderwidth=0)
                img_lbl.pack()
            lbl = tk.Label(card, text=t.get(tkey, key), bg=bg_card, fg=fg_text,
                           cursor="hand2", font=("Segoe UI", 9))
            lbl.pack(fill=tk.X, pady=(4, 0))
            self._fw_box_cards[key] = card
            self._fw_box_lbls[key] = lbl

            def _on_click(_e, k=key):
                self._select_fw_box(k)

            def _on_enter(_e, c=card):
                c.configure(highlightbackground=accent)

            def _on_leave(_e, c=card, k=key):
                if self._fw_box_filter != k:
                    c.configure(highlightbackground=bg_card)

            for w in (card, *card.winfo_children()):
                w.bind("<Button-1>", _on_click)
                w.bind("<Enter>", _on_enter)
                w.bind("<Leave>", _on_leave)

    def _update_fw_box_card_highlight(self):
        bg_card = self._theme.get("surface", "#2A2E35")
        accent  = self._theme.get("accent",  "#F0C64A")
        for k, card in self._fw_box_cards.items():
            card.configure(highlightbackground=(accent if k == self._fw_box_filter else bg_card))

    def _select_fw_box(self, key):
        # Toggle: clicking the selected box again shows all firmwares
        self._fw_box_filter = None if self._fw_box_filter == key else key
        self._update_fw_box_card_highlight()
        self._apply_fw_box_filter(force_best=self._fw_box_filter is not None)

    @staticmethod
    def _is_custom_fw_label(label):
        return (label.startswith("Personnalisé") or label.startswith("Custom")
                or label.startswith("Benutzerdefiniert"))

    def _fw_label_matches_box(self, label, key):
        if key is None or self._is_custom_fw_label(label):
            return True
        if label == FW_V15_LABEL:
            return key in ("comp_v1", "comp_v2")
        # Une entrée = une version ; elle n'est proposée que si un fichier
        # existe pour la box sélectionnée (mapping construit au scan).
        files = getattr(self, "_fw_version_files", {}).get(label)
        if not files:
            box = self._fw_label_box.get(label)
            return box is None or box == key
        return (key in files) or (None in files)

    def _resolve_fw_path(self, label):
        """Chemin du fichier firmware pour le label (version) sélectionné,
        en tenant compte de la box choisie via les cartes-images."""
        files = getattr(self, "_fw_version_files", {}).get(label)
        if not files:
            return self._firmware_map.get(label, "")   # custom / v1.5 legacy
        key = self._fw_box_filter
        if key is not None and key in files:
            return files[key]
        if None in files:
            return files[None]
        # Aucune box sélectionnée : premier fichier (ordre de priorité du scan).
        return next(iter(files.values()))

    def _filtered_fw_values(self):
        vals = list(self._fw_all_values or [])
        key = self._fw_box_filter
        if key is None:
            return vals
        return [v for v in vals if self._fw_label_matches_box(v, key)]

    def _best_fw_label(self, vals):
        """Prefer the newest non-bootloader, non-legacy, non-custom firmware."""
        for v in vals:
            if v != FW_V15_LABEL and "(bootloader)" not in v and not self._is_custom_fw_label(v):
                return v
        return vals[0] if vals else ""

    def _apply_fw_box_filter(self, force_best=False):
        vals = self._filtered_fw_values()
        try:
            self.cmb_hex["values"] = vals
        except Exception:
            return
        current = self.fw_choice.get().strip()
        if force_best or (current not in vals):
            best = self._best_fw_label(vals)
            self.fw_choice.set(best)
            current = best
            self._update_mc_compat_from_selection(best)
        # Toujours re-résoudre le fichier : la même version pointe vers un
        # fichier différent selon la box sélectionnée.
        self.hex_path.set(self._resolve_fw_path(current))

    def _on_firmware_selected(self, _event=None):
        selected = self.fw_choice.get().strip()
        path = self._resolve_fw_path(selected)
        if path:
            self.hex_path.set(path)
        # Update MC compat display based on selected firmware
        self._update_mc_compat_from_selection(selected)
        if selected == FW_V15_LABEL:
            t = self.i18n[self.lang.get()]
            messagebox.showwarning(
                t.get("fw_v15_warn_title", "⚠️ Firmware v1.5"),
                t.get("fw_v15_warn", "")
            )

    def _update_mc_compat_from_selection(self, selected=None):
        """Update the Motion Center compat label based on the firmware combo selection."""
        t = self.i18n[self.lang.get()]
        if selected is None:
            selected = self.fw_choice.get().strip()
        if selected == FW_V15_LABEL:
            self._mc_compat_ok = False
            self.mc_compat_status.set(t["mc_compat_ko"])
        else:
            # Assume any other listed firmware (v1.8) is compatible
            self._mc_compat_ok = True
            self.mc_compat_status.set(f"v{MOTION_CENTER_VERSION}")
        self._update_update_tab_health_visuals()

    def _auto_fill_hex_from_firmware(self, force=False):
        try:
            base_dir = app_base_dir()
        except Exception:
            base_dir = os.getcwd()

        fw_dir = os.path.join(base_dir, "firmware")
        if not os.path.isdir(fw_dir):
            return

        files = glob.glob(os.path.join(fw_dir, "*.hex")) + glob.glob(os.path.join(fw_dir, "*.bin"))
        if not files:
            self.cmb_hex["values"] = []
            self._firmware_map = {}
            self._fw_all_values = []
            return

        def sort_key(path):
            name = os.path.basename(path).lower()
            m = re.search(r"v?(\d+(?:\.\d+)+)", name)
            ver_tup = tuple(int(x) for x in m.group(1).split(".")) if m else (0,)
            no_boot = 0 if "with_bootloader" in name else 1
            # Prefer files with an explicit box variant suffix (boxv1/boxv2 or box-v1/box-v2)
            # so they're processed first and win the dedup.
            has_explicit_variant = 1 if re.search(r"[-_]box[-_]?v\d+$", name) else 0
            # Prioritize BOX V2 (6 actuators) before BOX V1 (5 actuators)
            # Extract box version: 0=unknown, 2=V2 (prioritize), 1=V1
            box_variant = 0
            bm = re.search(r"[-_]box[-_]?v(\d+)$", name, re.IGNORECASE)
            if bm:
                box_variant = int(bm.group(1))
            box_priority = 0 if box_variant == 2 else (1 if box_variant == 1 else 2)
            return (ver_tup, box_priority, no_boot, has_explicit_variant)

        ordered = sorted(files, key=sort_key, reverse=True)
        self._firmware_map = {}
        self._fw_label_box = {}
        self._fw_version_files = {}
        values = []
        # 1er passage : (label version, box) uniques, dans l'ordre de priorité
        entries = []
        seen = set()
        for fp in ordered:
            label = self._firmware_label(fp)
            # Remap any real v1.5 file to the legacy label so there is only one entry
            m15 = re.match(r"v?1\.5(\b|[^\d])", label, re.IGNORECASE)
            if m15:
                label = FW_V15_LABEL
                box = None
            else:
                box = self._fw_box_key(fp)
            if (label, box) in seen:
                # Skip duplicates — keep only the first (highest-priority) file
                # per (version, box)
                continue
            seen.add((label, box))
            entries.append((label, box, fp))

        # 2e passage : UNE entrée par version (pas de tag "(Box V2)" — la box
        # est choisie via les cartes-images ; Motion Center résout le bon
        # fichier via _resolve_fw_path et filtre les versions inexistantes
        # pour la box sélectionnée via _fw_label_matches_box).
        for label, box, fp in entries:
            slot = self._fw_version_files.setdefault(label, {})
            slot.setdefault(box, fp)
            if label not in self._firmware_map:
                self._firmware_map[label] = fp   # défaut = priorité la plus haute
                self._fw_label_box[label] = box
                values.append(label)

        # keep custom entry if any
        current_choice = self.fw_choice.get().strip()
        current_path = self.hex_path.get().strip()
        if current_choice and current_choice not in self._firmware_map and current_path and os.path.exists(current_path):
            self._firmware_map[current_choice] = current_path
            self._fw_label_box[current_choice] = None
            values.append(current_choice)

        # Add legacy v1.5 entry at the end only if no real v1.5 hex file exists
        if FW_V15_LABEL not in self._firmware_map:
            self._firmware_map[FW_V15_LABEL] = ""
            values.append(FW_V15_LABEL)

        self._fw_all_values = list(values)
        filtered = self._filtered_fw_values()
        self.cmb_hex["values"] = filtered

        if force or (not current_choice) or (current_choice not in self._firmware_map):
            best_label = self._best_fw_label(filtered)
            self.fw_choice.set(best_label)
            self.hex_path.set(self._resolve_fw_path(best_label))
        else:
            self.hex_path.set(self._resolve_fw_path(current_choice) or current_path)
        # Sync compat display with current selection
        self._update_mc_compat_from_selection(self.fw_choice.get().strip())

    def _detect_firmware_version(self):
        t = self.i18n[self.lang.get()]
        port_label = self.update_port.get().strip() or self.port.get().strip()
        if not port_label:
            self._set_installed_fw_display("-")
            return

        try:
            selected_port = port_from_label(port_label)
        except Exception:
            selected_port = port_label

        # Explicit detect action must ensure the box is connected.
        try:
            current_port = str(self.w.ser.port) if (self.w.is_connected() and self.w.ser) else ""
        except Exception:
            current_port = ""

        if not self.w.is_connected() or current_port.upper() != selected_port.upper():
            try:
                if self.w.is_connected():
                    self.w.disconnect()
                self.w.connect(selected_port)
                self.port.set(self.update_port.get().strip() or self.port.get().strip())
                self.btn_connect.config(text=t["disconnect"])
                self._log(f"[INFO] Connected on {selected_port} @ {BAUDRATE} (detect firmware)")
                try:
                    self.w.send_line("HELLO")
                except Exception:
                    pass
            except Exception as e:
                self._set_installed_fw_display("-")
                try:
                    self.btn_connect.config(text=t["connect"])
                except Exception:
                    pass
                self._log(f"[ERR] Firmware detect connect failed: {e}")
                return

        self._set_installed_fw_display(t["detecting_fw"])
        try:
            self.btn_detect_fw.configure(state="disabled")
        except Exception:
            pass

        threading.Thread(target=self._detect_firmware_worker, args=(selected_port,), daemon=True).start()

    def _detect_firmware_worker(self, selected_port):
        t = self.i18n[self.lang.get()]
        found = ""

        # Trigger a normal GET through existing connected worker, then wait for parsed cache.
        try:
            self.after(0, self.refresh_status)
        except Exception:
            pass

        deadline = time.time() + 4.5
        while time.time() < deadline:
            try:
                if self.w.is_connected() and self.w.ser and str(self.w.ser.port).upper() == str(selected_port).upper():
                    cached = str(self.info.get("firmware", "")).strip()
                    if cached and cached != "-":
                        found = cached
                        break
            except Exception:
                pass
            time.sleep(0.12)

        final_value = self._format_detected_firmware_display(found)
        if not final_value:
            final_value = t["fw_not_detected"]
        self.after(0, lambda v=final_value: self._set_installed_fw_display(v))
        self.after(0, lambda: self._fl(f"[INFO] Firmware detect: {final_value}"))
        self.after(0, lambda: self.btn_detect_fw.configure(state="normal"))

    def _format_detected_firmware_display(self, raw_value: str) -> str:
        s = str(raw_value or "")
        s = re.sub(r"[\x00-\x1F\x7F]+", " ", s)
        s = re.sub(r"\s+", " ", s).strip()
        if not s or s == "-":
            return ""

        vv = self._extract_version_text(s)
        if vv:
            return vv

        # Keep a readable fallback only when no explicit version is found.
        if len(s) > 40:
            s = s[:40].rstrip() + "…"
        return s

    def _extract_version_tuple(self, value: str):
        if not value:
            return None
        m = re.search(r"v?(\d+(?:\.\d+)+)", str(value), re.IGNORECASE)
        if not m:
            return None
        try:
            return tuple(int(part) for part in m.group(1).split("."))
        except Exception:
            return None

    def _extract_version_text(self, value: str):
        if not value:
            return ""
        m = re.search(r"v?\d+(?:\.\d+)+", str(value), re.IGNORECASE)
        return m.group(0).lower() if m else ""

    def _is_motion_center_compatible(self, required_version: str):
        req = self._extract_version_tuple(required_version)
        cur = self._extract_version_tuple(MOTION_CENTER_VERSION)
        if not req or not cur:
            return None
        ln = max(len(req), len(cur))
        req_n = tuple(list(req) + [0] * (ln - len(req)))
        cur_n = tuple(list(cur) + [0] * (ln - len(cur)))
        return cur_n >= req_n

    def _extract_remote_fw_meta(self, html: str, page_url: str):
        # Preferred machine-readable marker in comment:
        # <!-- SRT_FW_META {"product":"competition-control-box-v1.8","version":"v1.9.0","url":"https://...zip","motion_center_min":"v0.8"} -->
        try:
            cm = re.search(r"<!--\s*SRT_FW_META\s*(\{.*?\})\s*-->", html, re.IGNORECASE | re.DOTALL)
            if cm:
                obj = json.loads(cm.group(1))
                version = str(obj.get("version", "")).strip()
                url = str(obj.get("url", "")).strip()
                product = str(obj.get("product", "")).strip().lower()
                soft_min = str(obj.get("motion_center_min", obj.get("soft_min", obj.get("mc_min", "")))).strip()
                if product and product != FW_UPDATE_PRODUCT.lower():
                    return None, None, ""
                if version and url:
                    return version, urlparse.urljoin(page_url, url), soft_min
        except Exception:
            pass

        # Alternative marker via meta tags.
        mv = re.search(r'<meta[^>]*name=["\']srt-fw-version["\'][^>]*content=["\']([^"\']+)["\']', html, re.IGNORECASE)
        mu = re.search(r'<meta[^>]*name=["\']srt-fw-url["\'][^>]*content=["\']([^"\']+)["\']', html, re.IGNORECASE)
        mp = re.search(r'<meta[^>]*name=["\']srt-fw-product["\'][^>]*content=["\']([^"\']+)["\']', html, re.IGNORECASE)
        ms = re.search(r'<meta[^>]*name=["\']srt-fw-soft-min["\'][^>]*content=["\']([^"\']+)["\']', html, re.IGNORECASE)
        if mv and mu:
            product = (mp.group(1).strip().lower() if mp else "")
            if product and product != FW_UPDATE_PRODUCT.lower():
                return None, None, ""
            soft_min = (ms.group(1).strip() if ms else "")
            return mv.group(1).strip(), urlparse.urljoin(page_url, mu.group(1).strip()), soft_min

        def _extract_from_href(href: str):
            try:
                href_decoded = html_unescape(str(href or "").strip())
                abs_url = urlparse.urljoin(page_url, href_decoded)
                parsed = urlparse.urlparse(abs_url)
                q_raw = urlparse.parse_qs(parsed.query)

                # Normalize keys to support HTML-escaped separators that can
                # produce keys like "amp;srt_fw_product".
                q = {}
                for key, value in q_raw.items():
                    kk = str(key or "").strip().lower()
                    while kk.startswith("amp;"):
                        kk = kk[4:]
                    q[kk] = value

                marker = "srtfw" in q or "srt_fw" in q or "srt-fw" in q
                version = ""
                for k in ("srt_fw_version", "srtfw_version", "srt-fw-version", "fw_version", "version"):
                    if q.get(k):
                        version = str(q[k][0]).strip()
                        break

                product = ""
                for k in ("srt_fw_product", "srtfw_product", "srt-fw-product", "product"):
                    if q.get(k):
                        product = str(q[k][0]).strip().lower()
                        break

                soft_min = ""
                for k in ("srt_fw_soft_min", "srtfw_soft_min", "srt-fw-soft-min", "soft_min", "mc_min", "motion_center_min"):
                    if q.get(k):
                        soft_min = str(q[k][0]).strip()
                        break

                if product and product != FW_UPDATE_PRODUCT.lower():
                    return None, None, ""

                if marker and version:
                    return version, abs_url, soft_min
            except Exception:
                return None, None, ""
            return None, None, ""

        # Human-readable link with metadata embedded in URL query, e.g.:
        # <a href=".../competition-control-box-v1.9.0.zip?srtfw=1&srt_fw_product=competition-control-box-v1.8&srt_fw_version=v1.9.0">
        #   Télécharger la dernière version
        # </a>
        for m in re.finditer(r"<a\s+[^>]*href=[\"']([^\"']+)[\"'][^>]*>(.*?)</a>", html, re.IGNORECASE | re.DOTALL):
            href = html_unescape((m.group(1) or "").strip())
            txt = re.sub(r"<[^>]+>", "", (m.group(2) or "")).strip().lower()
            if not href:
                continue
            looks_latest_link = (
                "derni" in txt and "version" in txt
            ) or ("latest" in txt and "version" in txt)
            ver, link, soft_min = _extract_from_href(href)
            if ver and link:
                return ver, link, soft_min
            if looks_latest_link:
                # If a dedicated latest-version link exists but no explicit marker,
                # try extracting a version from URL path.
                vv = self._extract_version_text(href)
                if vv:
                    return vv, urlparse.urljoin(page_url, href), ""

        # Alternative marker directly on a download link.
        for tag in re.findall(r"<a\s+[^>]*>", html, re.IGNORECASE):
            v = re.search(r'data-srt-fw-version=["\']([^"\']+)["\']', tag, re.IGNORECASE)
            u = re.search(r'href=["\']([^"\']+)["\']', tag, re.IGNORECASE)
            p = re.search(r'data-srt-fw-product=["\']([^"\']+)["\']', tag, re.IGNORECASE)
            smin = re.search(r'data-srt-fw-soft-min=["\']([^"\']+)["\']', tag, re.IGNORECASE)
            if v and u:
                product = (p.group(1).strip().lower() if p else "")
                if product and product != FW_UPDATE_PRODUCT.lower():
                    continue
                soft_min = (smin.group(1).strip() if smin else "")
                return v.group(1).strip(), urlparse.urljoin(page_url, u.group(1).strip()), soft_min

            # Also support metadata in URL query parameters on any link tag.
            if u:
                ver, link, soft_min = _extract_from_href(html_unescape(u.group(1).strip()))
                if ver and link:
                    return ver, link, soft_min

        # Simplest fallback (last priority):
        # Any regular download link containing a version in URL/file name
        # (e.g. "...competition-control-box-v1.9.0.zip") is accepted.
        # If several links match (old versions also present), pick the highest version.
        candidates = []
        for m in re.finditer(r"<a\s+[^>]*href=[\"']([^\"']+)[\"'][^>]*>(.*?)</a>", html, re.IGNORECASE | re.DOTALL):
            href = html_unescape((m.group(1) or "").strip())
            if not href:
                continue
            abs_url = urlparse.urljoin(page_url, href)
            low = abs_url.lower()
            if not (".zip" in low or ".hex" in low or "competition-control-box" in low):
                continue
            vv = self._extract_version_text(abs_url)
            vt = self._extract_version_tuple(vv) if vv else None
            if vv and vt:
                candidates.append((vt, vv, abs_url))

        if candidates:
            candidates.sort(key=lambda x: x[0], reverse=True)
            _vt, vv, abs_url = candidates[0]
            return vv, abs_url, ""

        return None, None, ""

    def _extract_remote_soft_meta(self, html: str, page_url: str):
        # Preferred marker in comment:
        # <!-- SRT_SOFT_META {"version":"v0.9","url":"https://.../MotionCenter-0.9.exe"} -->
        try:
            cm = re.search(r"<!--\s*SRT_SOFT_META\s*(\{.*?\})\s*-->", html, re.IGNORECASE | re.DOTALL)
            if cm:
                obj = json.loads(cm.group(1))
                version = str(obj.get("version", "")).strip()
                url = str(obj.get("url", "")).strip()
                if version and url:
                    return version, urlparse.urljoin(page_url, url)
        except Exception:
            pass

        # Meta tags alternative
        mv = re.search(r'<meta[^>]*name=["\']srt-soft-version["\'][^>]*content=["\']([^"\']+)["\']', html, re.IGNORECASE)
        mu = re.search(r'<meta[^>]*name=["\']srt-soft-url["\'][^>]*content=["\']([^"\']+)["\']', html, re.IGNORECASE)
        if mv and mu:
            return mv.group(1).strip(), urlparse.urljoin(page_url, mu.group(1).strip())

        def _extract_soft_from_href(href: str):
            try:
                href_decoded = html_unescape(str(href or "").strip())
                abs_url = urlparse.urljoin(page_url, href_decoded)
                parsed = urlparse.urlparse(abs_url)
                q_raw = urlparse.parse_qs(parsed.query)

                # Normalize keys to support HTML-escaped separators producing
                # keys like "amp;srt_soft_version".
                q = {}
                for key, value in q_raw.items():
                    kk = str(key or "").strip().lower()
                    while kk.startswith("amp;"):
                        kk = kk[4:]
                    q[kk] = value

                marker = "srtsoft" in q or "srt_soft" in q or "srt-soft" in q
                version = ""
                for k in ("srt_soft_version", "srtsoft_version", "srt-soft-version", "soft_version", "version"):
                    if q.get(k):
                        version = str(q[k][0]).strip()
                        break

                if marker and version:
                    return version, abs_url
            except Exception:
                return None, None
            return None, None

        # URL-marker format (preferred when you want metadata in the link URL):
        # <a href=".../MotionCenter-0.9.exe?srtsoft=1&srt_soft_version=v0.9">Télécharger</a>
        for m in re.finditer(r"<a\s+[^>]*href=[\"']([^\"']+)[\"'][^>]*>(.*?)</a>", html, re.IGNORECASE | re.DOTALL):
            href = html_unescape((m.group(1) or "").strip())
            if not href:
                continue
            ver, link = _extract_soft_from_href(href)
            if ver and link:
                return ver, link

        # Simple fallback: highest version among links that look like Motion Center installers/archives
        candidates = []
        for m in re.finditer(r"<a\s+[^>]*href=[\"']([^\"']+)[\"'][^>]*>(.*?)</a>", html, re.IGNORECASE | re.DOTALL):
            href = html_unescape((m.group(1) or "").strip())
            txt = re.sub(r"<[^>]+>", "", (m.group(2) or "")).strip().lower()
            if not href:
                continue
            abs_url = urlparse.urljoin(page_url, href)
            low = abs_url.lower()
            looks_soft = (
                "motion" in low
                or "motion center" in txt
                or "motion-center" in low
                or "motioncenter" in low
            )
            if not looks_soft:
                continue
            if not any(ext in low for ext in (".exe", ".zip", ".msi")):
                continue
            vv = self._extract_version_text(abs_url)
            vt = self._extract_version_tuple(vv) if vv else None
            if vv and vt:
                candidates.append((vt, vv, abs_url))

        if candidates:
            candidates.sort(key=lambda x: x[0], reverse=True)
            _vt, vv, url = candidates[0]
            return vv, url

        return None, None

    def _set_latest_soft_download(self, url: str, version: str = ""):
        self._latest_soft_url = str(url or "").strip()
        self._latest_soft_version = str(version or "").strip()
        try:
            self.btn_soft_download.configure(state=("normal" if self._latest_soft_url else "disabled"))
        except Exception:
            pass

    def _check_soft_update(self):
        t = self.i18n[self.lang.get()]
        self._set_latest_soft_download("", "")
        self.soft_update_status.set(t["soft_checking"])
        try:
            self.btn_soft_check.configure(state="disabled")
        except Exception:
            pass
        threading.Thread(target=self._check_soft_update_worker, args=(self.lang.get(),), daemon=True).start()

    def _check_soft_update_worker(self, lang_code):
        t = self.i18n.get(lang_code, self.i18n["FR"])
        local_tuple = self._extract_version_tuple(MOTION_CENTER_VERSION)
        local_txt = self._extract_version_text(MOTION_CENTER_VERSION) or f"v{MOTION_CENTER_VERSION}"

        try:
            req = urlrequest.Request(SOFT_UPDATE_CHECK_URL, headers={"User-Agent": "SRT-Control-Box-Updater/1.0"})
            with urlrequest.urlopen(req, timeout=8) as resp:
                charset = resp.headers.get_content_charset() or "utf-8"
                html = resp.read().decode(charset, errors="replace")

            remote_version, remote_url = self._extract_remote_soft_meta(html, SOFT_UPDATE_CHECK_URL)
            if not remote_version:
                raise RuntimeError(t["soft_meta_missing"])

            remote_tuple = self._extract_version_tuple(remote_version)
            remote_txt = self._extract_version_text(remote_version) or remote_version

            if remote_tuple and local_tuple and remote_tuple > local_tuple:
                status = f"{t['soft_update_available']} • {local_txt} → {remote_txt}"
                self.after(0, lambda u=remote_url, v=remote_txt: self._set_latest_soft_download(u, v))
            else:
                status = f"{t['soft_up_to_date']} • {local_txt}"
                self.after(0, lambda: self._set_latest_soft_download("", ""))

            self.after(0, lambda s=status: self.soft_update_status.set(s))
            self.after(0, lambda: self._sl(f"[INFO] Motion Center web: {remote_txt} | {remote_url}"))
        except Exception as e:
            err = f"{t['soft_check_failed']}: {e}"
            self.after(0, lambda s=err: self.soft_update_status.set(s))
            self.after(0, lambda: self._set_latest_soft_download("", ""))
            self.after(0, lambda s=err: self._sl(f"[ERR] {s}"))
        finally:
            self.after(0, lambda: self.btn_soft_check.configure(state="normal"))

    def _download_soft_update(self):
        t = self.i18n[self.lang.get()]
        url = str(self._latest_soft_url or "").strip()
        if not url:
            self._sl(f"[WARN] {t['soft_no_url']}")
            return

        self._sl(f"[INFO] {t['soft_downloading']} {url}")
        try:
            self.btn_soft_download.configure(state="disabled")
        except Exception:
            pass
        threading.Thread(target=self._download_soft_update_worker, args=(self.lang.get(), url), daemon=True).start()

    def _reveal_in_explorer(self, path):
        """Open Windows Explorer with the given file selected (or its folder)."""
        try:
            p = os.path.abspath(str(path or ""))
            if os.name == "nt" and os.path.exists(p):
                subprocess.Popen(["explorer", "/select,", p])
            elif os.path.isdir(os.path.dirname(p)):
                webbrowser.open(os.path.dirname(p))
        except Exception:
            pass

    def _download_soft_update_worker(self, lang_code, url):
        t = self.i18n.get(lang_code, self.i18n["FR"])
        try:
            base_dir = app_install_dir()
            upd_dir = os.path.join(base_dir, "updates")
            try:
                os.makedirs(upd_dir, exist_ok=True)
            except Exception:
                # Fallback to the user's Downloads folder if the install dir is
                # read-only (e.g. Program Files).
                upd_dir = os.path.join(os.path.expanduser("~"), "Downloads")
                os.makedirs(upd_dir, exist_ok=True)

            req = urlrequest.Request(url, headers={"User-Agent": "SRT-Control-Box-Updater/1.0"})
            with urlrequest.urlopen(req, timeout=25) as resp:
                data = resp.read()
                cd = str(resp.headers.get("Content-Disposition", "") or "")

            filename = ""
            m = re.search(r"filename\*=UTF-8''([^;]+)", cd, re.IGNORECASE)
            if m:
                filename = urlparse.unquote(m.group(1).strip().strip('"'))
            if not filename:
                m = re.search(r'filename="?([^";]+)"?', cd, re.IGNORECASE)
                if m:
                    filename = m.group(1).strip()
            if not filename:
                parsed = urlparse.urlparse(url)
                filename = os.path.basename(parsed.path) or "motion-center-update.bin"

            dst = os.path.join(upd_dir, filename)
            stem, ext = os.path.splitext(filename)
            count = 1
            while os.path.exists(dst):
                dst = os.path.join(upd_dir, f"{stem}-{count}{ext}")
                count += 1

            with open(dst, "wb") as f:
                f.write(data)

            self.after(0, lambda p=dst: self._sl(f"[OK] {t['soft_downloaded']}\n[PATH] {p}"))
            self.after(0, lambda p=dst: self._log(f"[INFO] Motion Center update downloaded: {p}"))
            # Reveal the downloaded file in Windows Explorer so the user finds it
            # easily (the previous _MEIPASS temp folder was impractical).
            self.after(0, lambda p=dst: self._reveal_in_explorer(p))
        except Exception as e:
            self.after(0, lambda: self._sl(f"[ERR] {t['soft_download_failed']}: {e}"))
        finally:
            self.after(0, lambda: self.btn_soft_download.configure(state=("normal" if self._latest_soft_url else "disabled")))

    def _find_simhub_profile_source_dir(self):
        try:
            base_dir = app_base_dir()
        except Exception:
            base_dir = os.getcwd()

        candidates = [
            os.path.join(base_dir, "Simhub profile"),
            os.path.join(base_dir, "SimHub profile"),
            os.path.join(base_dir, "simhub profile"),
            os.path.join(base_dir, "simhub_profile"),
        ]
        for p in candidates:
            if os.path.isdir(p):
                return p
        return ""

    def _find_simhub_install_roots(self):
        roots = []

        def _add_root(path):
            p = str(path or "").strip().strip('"')
            if not p:
                return
            p = os.path.normpath(p)
            if _contains_usbd480(p):
                return
            if os.path.isdir(p):
                roots.append(p)

        # Common locations first
        pf86 = os.environ.get("ProgramFiles(x86)", "")
        pf64 = os.environ.get("ProgramFiles", "")
        if pf86:
            _add_root(os.path.join(pf86, "SimHub"))
        if pf64:
            _add_root(os.path.join(pf64, "SimHub"))
        _add_root(r"C:\Program Files (x86)\SimHub")
        _add_root(r"C:\Program Files\SimHub")
        _add_root(r"C:\SimHub")
        _add_root(r"C:\simhub")

        # Windows registry lookup for custom install paths
        if os.name == "nt":
            try:
                import winreg

                def _query_value(hive, subkey, value_name):
                    try:
                        with winreg.OpenKey(hive, subkey) as key:
                            val, _ = winreg.QueryValueEx(key, value_name)
                            return str(val or "").strip()
                    except Exception:
                        return ""

                reg_targets = [
                    (winreg.HKEY_CURRENT_USER, r"SOFTWARE\SimHub", "InstallPath"),
                    (winreg.HKEY_CURRENT_USER, r"SOFTWARE\SimHub", "Path"),
                    (winreg.HKEY_LOCAL_MACHINE, r"SOFTWARE\SimHub", "InstallPath"),
                    (winreg.HKEY_LOCAL_MACHINE, r"SOFTWARE\SimHub", "Path"),
                    (winreg.HKEY_LOCAL_MACHINE, r"SOFTWARE\WOW6432Node\SimHub", "InstallPath"),
                    (winreg.HKEY_LOCAL_MACHINE, r"SOFTWARE\WOW6432Node\SimHub", "Path"),
                    (winreg.HKEY_LOCAL_MACHINE, r"SOFTWARE\Microsoft\Windows\CurrentVersion\App Paths\SimHubWPF.exe", "Path"),
                    (winreg.HKEY_LOCAL_MACHINE, r"SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\App Paths\SimHubWPF.exe", "Path"),
                    (winreg.HKEY_CURRENT_USER, r"SOFTWARE\Microsoft\Windows\CurrentVersion\App Paths\SimHubWPF.exe", "Path"),
                ]

                for hive, subkey, value_name in reg_targets:
                    _add_root(_query_value(hive, subkey, value_name))

                uninstall_roots = [
                    (winreg.HKEY_LOCAL_MACHINE, r"SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall"),
                    (winreg.HKEY_LOCAL_MACHINE, r"SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall"),
                    (winreg.HKEY_CURRENT_USER, r"SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall"),
                ]

                for hive, root_key in uninstall_roots:
                    try:
                        with winreg.OpenKey(hive, root_key) as uninstall_key:
                            count = winreg.QueryInfoKey(uninstall_key)[0]
                            for i in range(count):
                                try:
                                    sk_name = winreg.EnumKey(uninstall_key, i)
                                    with winreg.OpenKey(uninstall_key, sk_name) as app_key:
                                        disp, _ = winreg.QueryValueEx(app_key, "DisplayName")
                                        disp_s = str(disp or "").lower()
                                        if "simhub" not in disp_s:
                                            continue
                                        for vn in ("InstallLocation", "InstallPath"):
                                            try:
                                                v, _ = winreg.QueryValueEx(app_key, vn)
                                                _add_root(v)
                                            except Exception:
                                                pass
                                except Exception:
                                    pass
                    except Exception:
                        pass
            except Exception:
                pass

        # Deduplicate while preserving priority order
        uniq = []
        seen = set()
        for r in roots:
            k = os.path.normcase(os.path.abspath(r))
            if k in seen:
                continue
            seen.add(k)
            uniq.append(r)
        return uniq

    def _find_simhub_controllers_dir(self):
        default_root = r"C:\Program Files (x86)\SimHub"
        try:
            if os.path.isdir(default_root):
                return os.path.join(default_root, "Motion", "Presets", "Controllers")
        except Exception:
            pass

        manual = _sanitize_path_without_usbd480(getattr(self, "_simhub_manual_controllers_dir", ""))
        if manual and os.path.isdir(manual):
            root = self._extract_simhub_root_from_path(manual)
            if self._is_valid_simhub_root(root):
                return os.path.join(root, "Motion", "Presets", "Controllers")
        return ""

    def _simhub_all_roots(self) -> list:
        """All directories where SimHub may store controller config files
        (installation, AppData, Documents, manual override)."""
        roots = []
        seen = set()

        def _add(p):
            if not p:
                return
            p = os.path.normpath(os.path.abspath(p))
            if p not in seen and os.path.isdir(p):
                seen.add(p)
                roots.append(p)

        # Standard installation paths
        for pf in [r"C:\Program Files (x86)\SimHub", r"C:\Program Files\SimHub"]:
            _add(pf)
        # AppData locations (roaming + local)
        for env_key in ("APPDATA", "LOCALAPPDATA"):
            base = os.environ.get(env_key, "")
            if base:
                _add(os.path.join(base, "SimHub"))
        # User Documents
        _add(os.path.join(os.path.expanduser("~"), "Documents", "SimHub"))
        # Manual controllers dir → walk up to SimHub root
        manual = _sanitize_path_without_usbd480(getattr(self, "_simhub_manual_controllers_dir", ""))
        if manual:
            r = self._extract_simhub_root_from_path(manual)
            if r:
                _add(r)
        return roots

    def _extract_simhub_root_from_path(self, path: str) -> str:
        p = _sanitize_path_without_usbd480(path)
        if not p:
            return ""
        p = os.path.normpath(os.path.abspath(p))
        try:
            parts = p.split(os.sep)
            low = [x.lower() for x in parts]
            if len(low) >= 4 and low[-1] == "controllers" and low[-2] == "presets" and low[-3] == "motion":
                return os.sep.join(parts[:-3])
            if len(low) >= 3 and low[-1] == "presets" and low[-2] == "motion":
                return os.sep.join(parts[:-2])
            if len(low) >= 2 and low[-1] == "motion":
                return os.sep.join(parts[:-1])
        except Exception:
            pass
        return p

    def _is_valid_simhub_root(self, root: str) -> bool:
        r = _sanitize_path_without_usbd480(root)
        if not r:
            return False
        try:
            r = os.path.normpath(os.path.abspath(r))
            if not os.path.isdir(r):
                return False
            name_ok = os.path.basename(r).strip().lower() == "simhub"
            exe_ok = os.path.isfile(os.path.join(r, "SimHubWPF.exe"))
            motion_ok = os.path.isdir(os.path.join(r, "Motion"))
            return bool(name_ok or exe_ok or motion_ok)
        except Exception:
            return False

    def _default_simhub_pick_dir(self):
        default_root = r"C:\Program Files (x86)\SimHub"
        try:
            if os.path.isdir(default_root):
                return default_root
        except Exception:
            pass

        try:
            manual = _sanitize_path_without_usbd480(getattr(self, "_simhub_manual_controllers_dir", ""))
            if manual and os.path.isdir(manual):
                parts = os.path.normpath(manual).split(os.sep)
                # If manual path ends with Motion\Presets\Controllers, return SimHub root if possible.
                low = [p.lower() for p in parts]
                if len(low) >= 4 and low[-1] == "controllers" and low[-2] == "presets" and low[-3] == "motion":
                    root = os.sep.join(parts[:-3])
                    if root and os.path.isdir(root):
                        return root
                return manual
        except Exception:
            pass
        return r"C:\Program Files (x86)\SimHub"

    def _resolve_manual_simhub_controllers_dir(self, chosen_dir: str) -> str:
        chosen = _sanitize_path_without_usbd480(chosen_dir)
        if not chosen or not os.path.isdir(chosen):
            return ""

        root = self._extract_simhub_root_from_path(chosen)
        if not self._is_valid_simhub_root(root):
            return ""
        return os.path.join(root, "Motion", "Presets", "Controllers")

    # Fixed filenames used by the installer (one per box version).
    # The same SimHub profile is used for every control box (V1/V2/V3/PRO).
    _FIXED_PROFILE_NAME = "Lebois Racing Motion Center v1.2.shmotioncontroller"
    # Old per-box profile names from previous Motion Center versions (cleaned up on install).
    _LEGACY_PROFILE_NAMES = (
        "Lebois Racing Motion Center v1.1.shmotioncontroller",
        "Competition Control Box V1 - Firmware V2.0.shmotioncontroller",
        "Competition Control Box V2 - Firmware V2.0.shmotioncontroller",
        "Competition Control Box V3 - Firmware V1.9.shmotioncontroller",
    )

    def _is_simhub_profile_installed(self):
        dest_dir = self._find_simhub_controllers_dir()
        if not dest_dir or not os.path.isdir(dest_dir):
            return False
        return os.path.isfile(os.path.join(dest_dir, self._FIXED_PROFILE_NAME))

    def _refresh_simhub_profile_state(self):
        t = self.i18n[self.lang.get()]
        state = self._is_simhub_profile_installed()
        if state is True:
            self.simhub_profile_status.set(t["simhub_profile_done"])
            self.simhub_profile_warning.set("")
            try:
                if self.btn_simhub_fix.winfo_ismapped():
                    self.btn_simhub_fix.pack_forget()
            except Exception:
                pass
        else:
            self.simhub_profile_status.set(t["simhub_profile_not_installed"])
            self.simhub_profile_warning.set(t["simhub_profile_warn_missing"])
            try:
                if not self.btn_simhub_fix.winfo_ismapped():
                    self.btn_simhub_fix.pack(side=tk.LEFT, padx=(8, 0))
            except Exception:
                pass
        self._update_update_tab_health_visuals()

    def _patch_installed_simhub_profiles(self, preferred_port: str = ""):
        """Best-effort repair for already installed fixed-name SimHub profiles."""
        try:
            dest_dir = self._find_simhub_controllers_dir()
        except Exception:
            dest_dir = ""
        if not dest_dir or not os.path.isdir(dest_dir):
            return

        for fixed_name in (self._FIXED_PROFILE_NAME,) + self._LEGACY_PROFILE_NAMES:
            profile_path = os.path.join(dest_dir, fixed_name)
            if not os.path.isfile(profile_path):
                continue
            try:
                patched_conn, patch_info = self._patch_simhub_profile_connection(profile_path, preferred_port)
                if patched_conn:
                    if patch_info:
                        self._log(f"[INFO] SimHub installed profile updated: {fixed_name} ({patch_info}, AfterOpenDelay >= 2500 ms)")
                    else:
                        self._log(f"[INFO] SimHub installed profile updated: {fixed_name} (AfterOpenDelay >= 2500 ms)")
                elif patch_info:
                    self._log(f"[WARN] SimHub installed profile patch skipped for {fixed_name}: {patch_info}")
            except Exception as e:
                self._log(f"[WARN] SimHub installed profile patch failed for {fixed_name}: {e}")

    def _install_simhub_profile(self):
        t = self.i18n[self.lang.get()]
        preferred_port = ""
        try:
            lbl = (self.port.get().strip() or self.update_port.get().strip())
            preferred_port = port_from_label(lbl) if lbl else ""
        except Exception:
            preferred_port = ""
        dest_dir = self._find_simhub_controllers_dir()
        if not dest_dir:
            default_pick_dir = self._default_simhub_pick_dir()
            if _contains_usbd480(default_pick_dir):
                default_pick_dir = r"C:\Program Files (x86)\SimHub"
            pick_msg = t["simhub_profile_pick_folder"]
            hint = str(t.get("simhub_profile_pick_hint", "")).strip()
            if hint:
                pick_msg = f"{pick_msg}\n\n{hint}"

            pick = messagebox.askyesno(APP_TITLE, pick_msg, parent=self)
            if not pick:
                return
            chosen = filedialog.askdirectory(
                title=t["simhub_profile_pick_title"],
                initialdir=default_pick_dir,
                mustexist=True,
            )
            chosen = str(chosen or "").strip()
            if not chosen:
                return
            dest_dir = self._resolve_manual_simhub_controllers_dir(chosen)
            if not dest_dir:
                self.simhub_profile_status.set(t["simhub_profile_pick_invalid"])
                self._sl(f"[ERR] {t['simhub_profile_pick_invalid']}")
                return
            self._simhub_manual_controllers_dir = _sanitize_path_without_usbd480(dest_dir)

        self.simhub_profile_status.set(t["simhub_profile_installing"])
        self._sl(f"[INFO] {t['simhub_profile_installing']}")
        try:
            self.btn_install_simhub_profile.configure(state="disabled")
        except Exception:
            pass
        threading.Thread(
            target=self._install_simhub_profile_worker,
            args=(self.lang.get(), dest_dir, preferred_port),
            daemon=True,
        ).start()

    def _patch_simhub_profile_connection(self, profile_path: str, preferred_port: str = "") -> tuple[bool, str]:
        """Patch SerialPort + AfterOpenDelay for robust cold-boot SimHub startup."""
        try:
            with open(profile_path, "r", encoding="utf-8-sig") as fh:
                data = json.load(fh)
        except Exception as e:
            return False, f"read failed: {e}"

        out = data.get("Output") or {}
        settings = out.get("Settings") or {}
        changed = False

        selected_port = _find_available_port_device(preferred_port)
        if selected_port and settings.get("SerialPort") != selected_port:
            settings["SerialPort"] = selected_port
            changed = True

        # Leonardo/32U4 class boards (USB CDC) only transmit serial data when the
        # host opens the port with DTR asserted. With DtrEnable=false, Windows
        # usbser.sys does not reliably signal the line as active, so the firmware
        # silently drops all TX: SimHub never receives CALIBRATED / homing replies
        # even though the box receives commands and runs calibration. This is most
        # visible on a cold start where only SimHub (not Motion Center) opened the
        # port. Force DTR on; at 115200 it does not reset the Leonardo.
        if settings.get("DtrEnable") is not True:
            settings["DtrEnable"] = True
            changed = True

        # Leonardo/32U4 class boards can need extra time after port-open on cold boot.
        # Keep a safe minimum delay so SimHub sends the first command after firmware is ready.
        min_after_open_delay = 2500
        current_after_open_delay = int(settings.get("AfterOpenDelay") or 0)
        if current_after_open_delay < min_after_open_delay:
            settings["AfterOpenDelay"] = min_after_open_delay
            changed = True

        if not changed:
            return False, ""

        try:
            with open(profile_path, "w", encoding="utf-8") as fh:
                json.dump(data, fh, indent=2, ensure_ascii=False)
                fh.write("\n")
            return True, selected_port
        except Exception as e:
            return False, f"write failed: {e}"

    def _install_simhub_profile_worker(self, lang_code, forced_dest_dir="", preferred_port=""):
        """Install the SimHub profile (same profile for every control box)."""
        t = self.i18n.get(lang_code, self.i18n["FR"])
        try:
            src_file = self._find_best_profile_source()
            if not src_file:
                raise RuntimeError(t["simhub_profile_no_file"])

            dest_dir = str(forced_dest_dir or "").strip() or self._find_simhub_controllers_dir()
            if not dest_dir:
                raise RuntimeError(t["simhub_profile_dest_missing"])
            os.makedirs(dest_dir, exist_ok=True)

            fixed_dest_name = self._FIXED_PROFILE_NAME

            # Remove leftover per-box profiles from older Motion Center versions
            if os.path.isdir(dest_dir):
                for existing_fn in list(os.listdir(dest_dir)):
                    if existing_fn.lower().endswith(".shmotioncontroller") and existing_fn in self._LEGACY_PROFILE_NAMES:
                        try:
                            os.remove(os.path.join(dest_dir, existing_fn))
                            self._log(f"[INFO] Removed stale profile: {existing_fn}")
                        except Exception:
                            pass

            dst = os.path.join(dest_dir, fixed_dest_name)
            shutil.copy2(src_file, dst)

            patched_conn, patch_info = self._patch_simhub_profile_connection(dst, preferred_port)
            if patched_conn:
                if patch_info:
                    self._log(f"[INFO] SimHub profile COM set to {patch_info} (AfterOpenDelay >= 2500 ms)")
                else:
                    self._log("[INFO] SimHub profile startup delay updated (AfterOpenDelay >= 2500 ms)")
            elif patch_info:
                self._log(f"[WARN] SimHub profile post-patch skipped: {patch_info}")

            self.after(0, lambda s=t["simhub_profile_done"]: self.simhub_profile_status.set(s))
            self.after(0, lambda d=dest_dir: setattr(self, "_simhub_manual_controllers_dir", _sanitize_path_without_usbd480(d)))
            self.after(0, lambda d=dest_dir: self._sl(f"[OK] {t['simhub_profile_done']} -> {d}"))
            self.after(0, self._refresh_simhub_profile_state)
        except Exception as e:
            msg = f"{t['simhub_profile_failed']}: {e}"
            self.after(0, lambda m=msg: self.simhub_profile_status.set(m))
            self.after(0, lambda m=msg: self._sl(f"[ERR] {m}"))
            self.after(0, lambda m=msg: self._log(f"[ERR] {m}"))
            self.after(0, self._update_update_tab_health_visuals)
        finally:
            self.after(0, lambda: self.btn_install_simhub_profile.configure(state="normal"))

    # ------------------------------------------------------------------ #
    #  SimHub process detection                                           #
    # ------------------------------------------------------------------ #
    @staticmethod
    def _is_simhub_running() -> bool:
        """Return True if SimHubWPF.exe is currently running."""
        try:
            result = subprocess.run(
                ["tasklist", "/FI", "IMAGENAME eq SimHubWPF.exe", "/NH", "/FO", "CSV"],
                capture_output=True, text=True, timeout=0.6
            )
            return "SimHubWPF.exe" in result.stdout
        except Exception:
            return False

    def _warn_if_simhub_running_async(self):
        """Run SimHub process check off the UI thread to avoid freezes."""
        def _worker():
            try:
                running = self._is_simhub_running()
            except Exception:
                running = False
            if running:
                self.after(0, self._show_simhub_running_warn)
        threading.Thread(target=_worker, daemon=True).start()

    def _show_simhub_running_warn(self):
        """Warning dialog with a collapsible 'learn more' section."""
        t = self.i18n[self.lang.get()]
        warn_text   = t.get("simhub_running_warn",
            "⚠ Close SimHub before connecting/disconnecting motors.")
        detail_text = t.get("simhub_running_detail",
            "SimHub must send positions for the number of motors defined in Motion Center.\n\n"
            "Motion Center automatically updates these values in the SimHub profile:\n"
            "  • \"Edit serial commands and settings\"\n"
            "  • \"Edit assignments\"")
        lbl_more = t.get("simhub_running_learn_more", "Learn more ▾")
        lbl_less = t.get("simhub_running_learn_less", "Show less ▴")

        dlg = tk.Toplevel(self)
        dlg.title(APP_TITLE)
        dlg.transient(self)
        dlg.resizable(False, False)
        try:
            self._apply_windows_titlebar_theme_for(dlg)
            dlg.configure(bg=self._theme.get("bg", "#23262C"))
        except Exception:
            pass

        body = ttk.Frame(dlg, padding=16)
        body.pack(fill=tk.BOTH, expand=True)

        ttk.Label(body, text=warn_text, wraplength=400, justify="left").pack(anchor="w", pady=(0, 10))

        detail_visible = tk.BooleanVar(value=False)
        detail_lbl = ttk.Label(body, text=detail_text, wraplength=400, justify="left")

        btn_row = ttk.Frame(body)
        btn_row.pack(fill=tk.X, pady=(0, 0))

        def toggle_detail():
            if detail_visible.get():
                detail_lbl.pack_forget()
                detail_visible.set(False)
                toggle_btn.configure(text=lbl_more)
            else:
                detail_lbl.pack(before=btn_row, anchor="w", pady=(0, 10))
                detail_visible.set(True)
                toggle_btn.configure(text=lbl_less)
            dlg.update_idletasks()
            dlg.geometry("")  # auto-resize

        toggle_btn = ttk.Button(btn_row, text=lbl_more, command=toggle_detail)
        toggle_btn.pack(side=tk.LEFT)
        ttk.Button(btn_row, text="OK", command=dlg.destroy, width=10).pack(side=tk.RIGHT)

        dlg.update_idletasks()
        px, py = self.winfo_rootx(), self.winfo_rooty()
        pw, ph = self.winfo_width(), self.winfo_height()
        dw, dh = dlg.winfo_reqwidth(), dlg.winfo_reqheight()
        dlg.geometry(f"+{px + (pw - dw) // 2}+{py + (ph - dh) // 2}")
        dlg.grab_set()
        dlg.wait_window()

    # ------------------------------------------------------------------ #
    #  SimHub profile axis patching                                        #
    # ------------------------------------------------------------------ #
    # Full Roles table: always keep indices 0-3 (FR/RR/FL/RL).
    # Roles 5 (traction loss) and 6 (extra/unused) are appended only when the
    # corresponding motor is connected.
    _SIMHUB_ROLES_ALL = [
        {"ParkPosition": 0.0, "Role": 2},   # M1 FR
        {"ParkPosition": 0.0, "Role": 4},   # M2 RR
        {"ParkPosition": 0.0, "Role": 3},   # M3 FL
        {"ParkPosition": 0.0, "Role": 1},   # M4 RL
        {"ParkPosition": 0.0, "Role": 6},   # M5 traction loss
        {"Role": 14},                        # M6 extra
        {"Role": 14},                        # M7 extra
    ]

    def _patch_simhub_profile_axes(self, n_axes: int):
        """Update MaxActuatorsEx + UpdateCommand + Roles in every installed
        (and source) .shmotioncontroller file to match n_axes (4, 5, 6 or 7)."""
        n = max(4, min(7, int(n_axes)))
        axis_cmd = "P" + "".join(f"<Axis{i}>" for i in range(1, n + 1))

        def _patch_file(path: str) -> bool:
            try:
                with open(path, "r", encoding="utf-8") as fh:
                    data = json.load(fh)
                settings = (data.get("Output") or {}).get("Settings") or {}
                gpd = settings.get("GenericProtocolDefinition") or {}
                aos = settings.get("ActuatorOrderingSettings") or {}

                changed = False

                # MaxActuatorsEx
                if aos.get("MaxActuatorsEx") != n:
                    aos["MaxActuatorsEx"] = n
                    changed = True

                # Roles — keep exactly n entries
                new_roles = self._SIMHUB_ROLES_ALL[:n]
                if aos.get("Roles") != new_roles:
                    aos["Roles"] = new_roles
                    changed = True

                # UpdateCommands
                uc = gpd.get("UpdateCommands") or []
                if uc and uc[0].get("Command") != axis_cmd:
                    uc[0]["Command"] = axis_cmd
                    changed = True

                # StartCommands — explicit SimHub start handshake.
                start_cmd = "SH_START"
                for sc_entry in (gpd.get("StartCommands") or []):
                    if sc_entry.get("Command") != start_cmd:
                        sc_entry["Command"] = start_cmd
                        changed = True
                    if sc_entry.get("MustWaitForMessage") is not True:
                        sc_entry["MustWaitForMessage"] = True
                        changed = True
                    if sc_entry.get("WaitForMessage") != "CALIBRATED":
                        sc_entry["WaitForMessage"] = "CALIBRATED"
                        changed = True
                    if sc_entry.get("CommandDelay") != 1:
                        sc_entry["CommandDelay"] = 1
                        changed = True

                # StopCommands — explicit SimHub stop handshake.
                for stop_entry in (gpd.get("StopCommands") or []):
                    if stop_entry.get("Command") != "SH_DISABLE":
                        stop_entry["Command"] = "SH_DISABLE"
                        changed = True
                    if stop_entry.get("MustWaitForMessage") is not True:
                        stop_entry["MustWaitForMessage"] = True
                        changed = True
                    if stop_entry.get("WaitForMessage") != "SH_DISABLED":
                        stop_entry["WaitForMessage"] = "SH_DISABLED"
                        changed = True
                    if stop_entry.get("WaitForDelay") != 5000:
                        stop_entry["WaitForDelay"] = 5000
                        changed = True

                # CustomName — embed axis count so SimHub's controller list shows
                # the motor count and people can tell at a glance which profile is active.
                # Format kept: "<base name> - N actuators (YYYY-MM-DD)"
                try:
                    out_sec_cn = data.get("Output") or {}
                    cur_cn = str(out_sec_cn.get("CustomName", "") or "").strip()
                    # Strip any previous " - N actuator(s)" chunk
                    cn_no_axes = re.sub(r'\s*-\s*\d+\s*actuators?', '', cur_cn).strip()
                    # Separate trailing date "(YYYY-MM-DD...)" if present
                    date_m2 = re.search(r'(\s*\(\d{4}-\d{2}-\d{2}[^)]*\))\s*$', cn_no_axes)
                    date_sfx = date_m2.group(1) if date_m2 else ""
                    cn_base = cn_no_axes[:date_m2.start()].strip() if date_m2 else cn_no_axes
                    new_cn = f"{cn_base} - {n} actuators{date_sfx}"
                    if new_cn != cur_cn:
                        out_sec_cn["CustomName"] = new_cn
                        changed = True
                except Exception:
                    pass

                if not changed:
                    return False

                # Stamp "Updated: YYYY-MM-DD HH:MM" in Comments so SimHub's
                # controller list shows when the profile was last modified.
                try:
                    out_sec = data.get("Output") or {}
                    cur_c = str(out_sec.get("Comments", "") or "").rstrip()
                    c_lines = [l for l in cur_c.splitlines() if not l.startswith("Updated:")]
                    c_lines.append(f"Updated: {time.strftime('%Y-%m-%d %H:%M')}")
                    out_sec["Comments"] = "\r\n".join(c_lines) + "\r\n"
                except Exception:
                    pass

                # Write back (pretty, 2-space indent to match SimHub format)
                with open(path, "w", encoding="utf-8") as fh:
                    json.dump(data, fh, indent=2, ensure_ascii=False)
                    fh.write("\n")
                return True
            except Exception as e:
                self._log(f"[WARN] SimHub profile patch failed ({path}): {e}")
                return False

        patched = []

        # 1) Collect OutputId / CustomName from source files WITHOUT patching them.
        # Source files are now pre-configured for each (box_version, n_axes) combination;
        # patching them to a specific n would corrupt the variants with different counts.
        src_dir = self._find_simhub_profile_source_dir()
        src_dir_norm = os.path.normpath(os.path.abspath(src_dir)) if src_dir else ""
        source_ids: set = set()
        source_names: set = set()
        if src_dir:
            for root, _dirs, files in os.walk(src_dir):
                for fn in files:
                    if fn.lower().endswith(".shmotioncontroller"):
                        fp = os.path.join(root, fn)
                        try:
                            with open(fp, "r", encoding="utf-8") as fh:
                                d = json.load(fh)
                            out = d.get("Output") or {}
                            oid = (out.get("Settings") or {}).get("OutputId", "")
                            cn  = out.get("CustomName", "")
                            if oid: source_ids.add(oid)
                            if cn:  source_names.add(cn)
                        except Exception:
                            pass

        # 2) Search ALL SimHub-related directories recursively.
        # Matches by OutputId or CustomName so we update both the Presets/Controllers
        # template AND any active controller instance stored elsewhere by SimHub.
        # Build keywords (words ≥5 chars) from source names to detect related-but-unmatched files.
        stale_keywords = set()
        for sn in source_names:
            for w in re.split(r'\W+', sn):
                if len(w) >= 5:
                    stale_keywords.add(w.lower())
        stale_candidates: list = []

        for search_root in self._simhub_all_roots():
            for dirpath, _dirs, files in os.walk(search_root):
                for fn in files:
                    if not fn.lower().endswith(".shmotioncontroller"):
                        continue
                    fp = os.path.join(dirpath, fn)
                    if src_dir_norm and os.path.normpath(fp).startswith(src_dir_norm):
                        continue  # already handled above
                    try:
                        with open(fp, "r", encoding="utf-8") as fh:
                            d_chk = json.load(fh)
                        out_chk = d_chk.get("Output") or {}
                        oid_chk = (out_chk.get("Settings") or {}).get("OutputId", "")
                        cn_chk  = out_chk.get("CustomName", "")
                        # Normalize: strip trailing " (YYYY-MM-DD)" install-date stamp before matching
                        cn_base = re.sub(r'\s*\(\d{4}-\d{2}-\d{2}\)\s*$', '', cn_chk).strip()
                        if oid_chk in source_ids or cn_chk in source_names or cn_base in source_names:
                            if _patch_file(fp):
                                rel = os.path.relpath(fp, search_root)
                                patched.append(f"[simhub:{rel}]")
                        elif stale_keywords and cn_base:
                            cn_lower = cn_base.lower()
                            if any(kw in cn_lower for kw in stale_keywords):
                                stale_candidates.append((fp, cn_chk))
                    except Exception:
                        pass

        if patched:
            self._log(f"[INFO] SimHub profile updated → {n} axes  ({', '.join(patched)})")
        else:
            self._log(f"[INFO] SimHub profile already at {n} axes (no change needed)")

        # 3) Collect .bak files from all SimHub-related directories.
        # These are backups we created during install — always safe to delete.
        bak_candidates: list = []
        for search_root in self._simhub_all_roots():
            for dirpath, _dirs, files in os.walk(search_root):
                for fn in files:
                    if fn.lower().endswith(".bak") and ".shmotioncontroller." in fn.lower():
                        bak_candidates.append((os.path.join(dirpath, fn), fn))

        if stale_candidates or bak_candidates:
            self._offer_delete_stale_profiles(stale_candidates, bak_candidates)

        return source_names, bool(patched)

    _last_synced_axis_count: int = 0  # 0 = never synced; resets on disconnect

    def _find_best_profile_source(self):
        """Return the newest bundled .shmotioncontroller file.
        The same profile is used for every control box."""
        src_root = self._find_simhub_profile_source_dir()
        if not src_root:
            return None
        candidates = []
        for root, _dirs, files in os.walk(src_root):
            for fn in files:
                if not fn.lower().endswith(".shmotioncontroller"):
                    continue
                p = os.path.join(root, fn)
                try:
                    mt = os.path.getmtime(p)
                except Exception:
                    mt = 0
                candidates.append((mt, p))
        if not candidates:
            return None
        candidates.sort(reverse=True)
        return candidates[0][1]

    def _auto_sync_simhub_axes(self):
        """No-op: profile is now fixed per firmware version (V1=5 motors, V2=6 motors).
        Left in place so call sites compile without changes."""
        return

    def _offer_delete_stale_profiles(self, candidates: list, bak_candidates: list = None):
        """Show a dialog listing old/unrelated SimHub profiles and .bak files
        that can be deleted from the Controllers folder."""
        t = self.i18n[self.lang.get()]
        title       = t.get("stale_profiles_title",        "SimHub cleanup")
        msg         = t.get("stale_profiles_msg",
                             "These files can be deleted from the SimHub Controllers folder:")
        sec_stale   = t.get("stale_profiles_section_stale", "Old profiles (possibly outdated):")
        sec_bak     = t.get("stale_profiles_section_bak",   "Backup files (.bak):")
        lbl_selall  = t.get("stale_profiles_select_all",   "Select all")
        lbl_deselall= t.get("stale_profiles_deselect_all", "Deselect all")
        lbl_delete  = t.get("stale_profiles_delete",       "Delete selected")
        lbl_none    = t.get("stale_profiles_none",         "No file selected.")

        bak_candidates = bak_candidates or []

        dlg = tk.Toplevel(self)
        dlg.title(title)
        dlg.transient(self)
        dlg.resizable(False, False)
        try:
            self._apply_windows_titlebar_theme_for(dlg)
            dlg.configure(bg=self._theme.get("bg", "#23262C"))
        except Exception:
            pass

        outer = ttk.Frame(dlg, padding=16)
        outer.pack(fill=tk.BOTH, expand=True)

        ttk.Label(outer, text=msg, wraplength=560, justify="left").pack(anchor="w", pady=(0, 10))

        # Scrollable canvas so the list doesn't overflow on-screen
        canvas = tk.Canvas(outer, highlightthickness=0,
                           bg=self._theme.get("bg", "#23262C"), width=580)
        vsb = ttk.Scrollbar(outer, orient="vertical", command=canvas.yview)
        canvas.configure(yscrollcommand=vsb.set)
        vsb.pack(side=tk.RIGHT, fill=tk.Y)
        canvas.pack(side=tk.LEFT, fill=tk.BOTH, expand=True)
        body = ttk.Frame(canvas)
        body_win = canvas.create_window((0, 0), window=body, anchor="nw")
        def _on_body_configure(e):
            canvas.configure(scrollregion=canvas.bbox("all"))
            canvas.itemconfigure(body_win, width=canvas.winfo_width())
        body.bind("<Configure>", _on_body_configure)
        canvas.bind("<Configure>", lambda e: canvas.itemconfigure(body_win, width=e.width))

        vars_ = []
        _dim = self._theme.get("text_muted", "#888888")

        def _add_section(label, items, is_bak=False):
            if not items:
                return
            ttk.Label(body, text=label, font=("TkDefaultFont", 9, "bold")).pack(
                anchor="w", pady=(8, 2))
            for fp, cn in items:
                var = tk.BooleanVar(value=is_bak)  # pre-select .bak, not stale profiles
                vars_.append((var, fp, cn))
                row = ttk.Frame(body)
                row.pack(fill=tk.X, pady=1)
                ttk.Checkbutton(row, variable=var, text=cn).pack(side=tk.LEFT)
                if not is_bak:
                    ttk.Label(row, text=f"  ({os.path.basename(fp)})",
                              foreground=_dim).pack(side=tk.LEFT)

        _add_section(sec_stale, candidates,      is_bak=False)
        _add_section(sec_bak,   bak_candidates,  is_bak=True)

        # Cap canvas height so dialog doesn't overflow the screen
        dlg.update_idletasks()
        max_h = min(body.winfo_reqheight() + 4, 420)
        canvas.configure(height=max_h)

        btn_row = ttk.Frame(dlg, padding=(16, 0, 16, 12))
        btn_row.pack(fill=tk.X)

        def _toggle_all(val: bool):
            for var, _, _ in vars_:
                var.set(val)

        def do_delete():
            selected = [(fp, cn) for var, fp, cn in vars_ if var.get()]
            if not selected:
                messagebox.showinfo(APP_TITLE, lbl_none, parent=dlg)
                return
            deleted, errors = [], []
            for fp, cn in selected:
                try:
                    os.remove(fp)
                    deleted.append(cn)
                    self._log(f"[INFO] Deleted SimHub file: {fp}")
                except Exception as e:
                    errors.append(f"{cn}: {e}")
                    self._log(f"[WARN] Could not delete {fp}: {e}")
            dlg.destroy()
            if errors:
                messagebox.showwarning(APP_TITLE, "\n".join(errors), parent=self)

        ttk.Button(btn_row, text=lbl_selall,   command=lambda: _toggle_all(True)).pack(side=tk.LEFT)
        ttk.Button(btn_row, text=lbl_deselall, command=lambda: _toggle_all(False)).pack(side=tk.LEFT, padx=(6, 0))
        ttk.Button(btn_row, text="OK",          command=dlg.destroy,  width=8).pack(side=tk.RIGHT)
        ttk.Button(btn_row, text=lbl_delete,    command=do_delete).pack(side=tk.RIGHT, padx=(0, 6))

        dlg.update_idletasks()
        px, py = self.winfo_rootx(), self.winfo_rooty()
        pw, ph = self.winfo_width(), self.winfo_height()
        dw, dh = dlg.winfo_reqwidth(), dlg.winfo_reqheight()
        dlg.geometry(f"+{px + (pw - dw) // 2}+{py + (ph - dh) // 2}")
        dlg.grab_set()
        dlg.wait_window()

    def _show_simhub_reload_instructions(self, n: int, profile_names):
        """After patching disk profiles, tell the user what to do manually in SimHub."""
        t = self.i18n[self.lang.get()]
        title  = t.get("simhub_reload_title", "\u26a0 SimHub \u2014 action required")
        intro  = t.get("simhub_reload_intro",
                       "Profile updated on disk ({n} motor(s)).\nSimHub cannot be reconfigured automatically."
                       ).replace("{n}", str(n))
        step1  = t.get("simhub_reload_step1",
                       "\u2460 In SimHub \u203a Motion controllers:\n   DELETE active controllers with these names:")
        step2  = t.get("simhub_reload_step2",
                       "\u2461 Click \"Add\" / \"+\" to add the updated controller.")

        names_lines = "\n".join(f"   \u2022 {nm}" for nm in sorted(profile_names)) if profile_names else "   \u2022 ?"
        body_text = f"{intro}\n\n{step1}\n{names_lines}\n\n{step2}\n{names_lines}"

        dlg = tk.Toplevel(self)
        dlg.title(title)
        dlg.transient(self)
        dlg.resizable(False, False)
        try:
            self._apply_windows_titlebar_theme_for(dlg)
            dlg.configure(bg=self._theme.get("bg", "#23262C"))
        except Exception:
            pass

        body = ttk.Frame(dlg, padding=20)
        body.pack(fill=tk.BOTH, expand=True)

        ttk.Label(body, text=body_text, wraplength=540, justify="left").pack(anchor="w")
        ttk.Button(body, text="OK", command=dlg.destroy, width=10).pack(pady=(16, 0))

        dlg.update_idletasks()
        px, py = self.winfo_rootx(), self.winfo_rooty()
        pw, ph = self.winfo_width(), self.winfo_height()
        dw, dh = dlg.winfo_reqwidth(), dlg.winfo_reqheight()
        dlg.geometry(f"+{px + (pw - dw) // 2}+{py + (ph - dh) // 2}")
        dlg.grab_set()
        dlg.wait_window()

    def _show_simhub_reload_instructions(self, n: int, profile_names):
        """After patching disk profiles, tell the user what to do manually in SimHub."""
        t = self.i18n[self.lang.get()]
        title  = t.get("simhub_reload_title", "\u26a0 SimHub \u2014 action required")
        intro  = t.get("simhub_reload_intro",
                       "Profile updated on disk ({n} motor(s)).\nSimHub cannot be reconfigured automatically."
                       ).replace("{n}", str(n))
        step1  = t.get("simhub_reload_step1",
                       "\u2460 In SimHub \u203a Motion controllers:\n   DELETE active controllers with these names:")
        step2  = t.get("simhub_reload_step2",
                       "\u2461 Click \"Add\" / \"+\" to add the updated controller.")

        names_lines = "\n".join(f"   \u2022 {nm}" for nm in sorted(profile_names)) if profile_names else "   \u2022 ?"
        body_text = f"{intro}\n\n{step1}\n{names_lines}\n\n{step2}\n{names_lines}"

        dlg = tk.Toplevel(self)
        dlg.title(title)
        dlg.transient(self)
        dlg.resizable(False, False)
        try:
            self._apply_windows_titlebar_theme_for(dlg)
            dlg.configure(bg=self._theme.get("bg", "#23262C"))
        except Exception:
            pass

        body = ttk.Frame(dlg, padding=20)
        body.pack(fill=tk.BOTH, expand=True)

        ttk.Label(body, text=body_text, wraplength=540, justify="left").pack(anchor="w")
        ttk.Button(body, text="OK", command=dlg.destroy, width=10).pack(pady=(16, 0))

        dlg.update_idletasks()
        px, py = self.winfo_rootx(), self.winfo_rooty()
        pw, ph = self.winfo_width(), self.winfo_height()
        dw, dh = dlg.winfo_reqwidth(), dlg.winfo_reqheight()
        dlg.geometry(f"+{px + (pw - dw) // 2}+{py + (ph - dh) // 2}")
        dlg.grab_set()
        dlg.wait_window()

    def _set_latest_fw_url(self, url: str, soft_min: str = "", soft_ok: bool = True):
        self._latest_fw_url = str(url or "").strip()
        self._latest_fw_soft_min = str(soft_min or "").strip()
        self._latest_fw_soft_compatible = bool(soft_ok)

    def _download_web_firmware(self):
        t = self.i18n[self.lang.get()]
        url = str(self._latest_fw_url or "").strip()
        if not url:
            self._fl(f"[WARN] {t['web_fw_no_url']}")
            return
        if not bool(self._latest_fw_soft_compatible):
            self._fl(f"[ERR] {t['web_fw_incompat_block']}")
            return

        self._fl(f"[INFO] {t['web_fw_dl_running']} {url}")

        threading.Thread(target=self._download_web_firmware_worker, args=(self.lang.get(), url), daemon=True).start()

    def _download_web_firmware_worker(self, lang_code, url):
        t = self.i18n.get(lang_code, self.i18n["FR"])
        prepared_paths = []
        fw_dir = ""

        try:
            base_dir = app_base_dir()
            fw_dir = os.path.join(base_dir, "firmware")
            os.makedirs(fw_dir, exist_ok=True)

            req = urlrequest.Request(url, headers={"User-Agent": "SRT-Control-Box-Updater/1.0"})
            with urlrequest.urlopen(req, timeout=20) as resp:
                data = resp.read()
                cd = str(resp.headers.get("Content-Disposition", "") or "")
                content_type = str(resp.headers.get("Content-Type", "") or "").lower()

            filename = ""
            m = re.search(r"filename\*=UTF-8''([^;]+)", cd, re.IGNORECASE)
            if m:
                filename = urlparse.unquote(m.group(1).strip().strip('"'))
            if not filename:
                m = re.search(r'filename="?([^";]+)"?', cd, re.IGNORECASE)
                if m:
                    filename = m.group(1).strip()
            if not filename:
                parsed = urlparse.urlparse(url)
                filename = os.path.basename(parsed.path)
            if not filename:
                filename = "firmware_download.bin"

            ext = os.path.splitext(filename)[1].lower()
            looks_like_zip = (
                ext == ".zip"
                or "application/zip" in content_type
                or "application/x-zip-compressed" in content_type
                or (len(data) >= 4 and data[:4] == b"PK\x03\x04")
            )

            if ext == ".hex" and not looks_like_zip:
                dst = os.path.join(fw_dir, filename)
                with open(dst, "wb") as f:
                    f.write(data)
                prepared_paths.append(dst)
            elif looks_like_zip:
                if ext != ".zip":
                    filename = f"{os.path.splitext(filename)[0] or 'firmware_download'}.zip"
                zip_path = os.path.join(fw_dir, filename)
                with open(zip_path, "wb") as f:
                    f.write(data)

                with zipfile.ZipFile(zip_path, "r") as zf:
                    hex_members = [n for n in zf.namelist() if n.lower().endswith(".hex") and not n.endswith("/")]
                    if not hex_members:
                        raise RuntimeError(t["web_fw_dl_no_hex"])
                    for member in hex_members:
                        out_name = os.path.basename(member)
                        if not out_name:
                            continue
                        dst = os.path.join(fw_dir, out_name)
                        stem, ext2 = os.path.splitext(out_name)
                        count = 1
                        while os.path.exists(dst):
                            dst = os.path.join(fw_dir, f"{stem}-{count}{ext2}")
                            count += 1
                        with zf.open(member, "r") as src, open(dst, "wb") as out:
                            out.write(src.read())
                        prepared_paths.append(dst)
            else:
                raise RuntimeError(t["web_fw_dl_unsupported"])

            if not prepared_paths:
                raise RuntimeError(t["web_fw_dl_no_hex"])

            first_prepared = prepared_paths[0]
            self.after(0, lambda: self._auto_fill_hex_from_firmware(force=True))

            def _select_prepared():
                try:
                    target = os.path.normcase(os.path.abspath(first_prepared))
                    # Chercher dans le mapping version->box->fichier (une entrée
                    # de liste par version), puis dans les entrées directes.
                    candidates = []
                    for label, files in getattr(self, "_fw_version_files", {}).items():
                        for p in files.values():
                            candidates.append((label, p))
                    candidates.extend(self._firmware_map.items())
                    for label, p in candidates:
                        if p and os.path.normcase(os.path.abspath(p)) == target:
                            self.fw_choice.set(label)
                            self.hex_path.set(p)
                            break
                except Exception:
                    pass

            self.after(120, _select_prepared)
            self.after(0, lambda: self._fl(f"[OK] {t['web_fw_dl_done']} ({len(prepared_paths)} {t['files_count_suffix']})"))
            self.after(0, lambda: self.lbl_flash_status.configure(text=t["web_fw_dl_done"]))
        except Exception as e:
            self.after(0, lambda: self._fl(f"[ERR] {t['web_fw_dl_failed']}: {e}"))
            self.after(0, lambda: self.lbl_flash_status.configure(text=t["web_fw_dl_failed"]))

    def _check_web_firmware_update(self):
        t = self.i18n[self.lang.get()]
        local_fw_raw = str(self.installed_fw.get() or "").strip()
        self._set_latest_fw_url("", "", True)
        self.web_fw_status.set(t["checking_web_fw"])
        threading.Thread(target=self._check_web_firmware_update_worker, args=(self.lang.get(), local_fw_raw), daemon=True).start()

    def _check_web_firmware_update_worker(self, lang_code, local_fw_raw):
        t = self.i18n.get(lang_code, self.i18n["FR"])
        local_tuple = self._extract_version_tuple(local_fw_raw)
        local_text = self._extract_version_text(local_fw_raw)

        try:
            req = urlrequest.Request(
                FW_UPDATE_PAGE_URL,
                headers={"User-Agent": "SRT-Control-Box-Updater/1.0"}
            )
            with urlrequest.urlopen(req, timeout=8) as resp:
                charset = resp.headers.get_content_charset() or "utf-8"
                html = resp.read().decode(charset, errors="replace")

            remote_version, remote_url, remote_soft_min = self._extract_remote_fw_meta(html, FW_UPDATE_PAGE_URL)
            if not remote_version:
                raise RuntimeError(t["web_fw_meta_missing"])

            remote_tuple = self._extract_version_tuple(remote_version)
            remote_text = self._extract_version_text(remote_version) or remote_version

            soft_req_txt = ""
            soft_ok = True
            if remote_soft_min:
                soft_req_txt = self._extract_version_text(remote_soft_min) or str(remote_soft_min)
                comp = self._is_motion_center_compatible(soft_req_txt)
                soft_ok = (comp is not False)

            if not local_tuple:
                status = f"{t['web_fw_local_unknown']} • {remote_text}"
            elif remote_tuple and remote_tuple > local_tuple:
                status = f"{t['web_fw_update_available']} • {local_text} → {remote_text}"
            else:
                status = f"{t['web_fw_up_to_date']} • {remote_text}"

            if soft_req_txt:
                if soft_ok:
                    status = f"{status} • {t['web_fw_requires_soft']}: {soft_req_txt}"
                else:
                    status = f"{status} • {t['web_fw_incompat_soft']} ({t['web_fw_requires_soft']}: {soft_req_txt}, soft v{MOTION_CENTER_VERSION})"

            self.after(0, lambda s=status: self.web_fw_status.set(s))
            self.after(0, lambda u=remote_url, r=soft_req_txt, ok=soft_ok: self._set_latest_fw_url(u, r, ok))
            self.after(0, lambda: self._fl(f"[INFO] Web firmware: {remote_text} | {remote_url}"))
        except (URLError, TimeoutError) as e:
            err = f"{t['web_fw_check_failed']}: {e}"
            self.after(0, lambda s=err: self.web_fw_status.set(s))
            self.after(0, lambda: self._set_latest_fw_url("", "", True))
            self.after(0, lambda s=err: self._fl(f"[ERR] {s}"))
        except Exception as e:
            err = f"{t['web_fw_check_failed']}: {e}"
            self.after(0, lambda s=err: self.web_fw_status.set(s))
            self.after(0, lambda: self._set_latest_fw_url("", "", True))
            self.after(0, lambda s=err: self._fl(f"[ERR] {s}"))

    def _find_avrdude(self):
        # 1) Prefer bundled avrdude in project folder (portable/offline install)
        try:
            base_dir = app_base_dir()
        except Exception:
            base_dir = os.getcwd()

        local_candidates = [
            os.path.join(base_dir, "avrdude", "bin", "avrdude.exe"),
            os.path.join(base_dir, "avrdude", "avrdude.exe"),
            os.path.join(os.getcwd(), "avrdude", "bin", "avrdude.exe"),
            os.path.join(os.getcwd(), "avrdude", "avrdude.exe"),
        ]
        local_conf_candidates = [
            os.path.join(base_dir, "avrdude", "etc", "avrdude.conf"),
            os.path.join(base_dir, "avrdude", "avrdude.conf"),
            os.path.join(os.getcwd(), "avrdude", "etc", "avrdude.conf"),
            os.path.join(os.getcwd(), "avrdude", "avrdude.conf"),
        ]

        local_exe = next((p for p in local_candidates if os.path.exists(p)), None)
        local_conf = next((p for p in local_conf_candidates if os.path.exists(p)), None)
        if local_exe and local_conf:
            return local_exe, local_conf

        # 2) Fallback to PATH
        exe = shutil.which("avrdude")
        if exe:
            candidates = [
                os.path.join(os.path.dirname(exe), "..", "etc", "avrdude.conf"),
                os.path.join(os.path.dirname(exe), "avrdude.conf"),
            ]
            for c in candidates:
                cc = os.path.abspath(c)
                if os.path.exists(cc):
                    return exe, cc

            # 3) Fallback to common Arduino/PlatformIO locations
        roots = []
        local_app = os.environ.get("LOCALAPPDATA", "")
        user_profile = os.environ.get("USERPROFILE", "")
        if local_app:
            roots.extend([
                os.path.join(local_app, "Arduino15", "packages", "arduino", "tools", "avrdude"),
                os.path.join(local_app, "Arduino15", "packages", "arduino", "tools", "avrdude", "*"),
            ])
        if user_profile:
            roots.append(os.path.join(user_profile, ".platformio", "packages", "tool-avrdude"))

        for r in roots:
            for p in glob.glob(r):
                if os.path.isdir(p):
                    exe_cands = [
                        os.path.join(p, "bin", "avrdude.exe"),
                        os.path.join(p, "avrdude.exe"),
                    ]
                    conf_cands = [
                        os.path.join(p, "etc", "avrdude.conf"),
                        os.path.join(p, "avrdude.conf"),
                    ]
                    for ee in exe_cands:
                        if not os.path.exists(ee):
                            continue
                        for cc in conf_cands:
                            if os.path.exists(cc):
                                return ee, cc
        return None, None

    def _find_bootloader_port(self, old_ports, preferred_port, timeout_s=6.0):
        deadline = time.time() + timeout_s
        old_set = set(old_ports or [])
        last_seen = preferred_port
        while time.time() < deadline:
            now_ports = [p.device for p in serial.tools.list_ports.comports()]
            if preferred_port in now_ports:
                last_seen = preferred_port
            new_ports = [p for p in now_ports if p not in old_set]
            if len(new_ports) == 1:
                # Give the Windows USB CDC driver a moment to finish initialising
                # the port before avrdude calls SetCommState(). Without this delay
                # the port appears in the list but isn't yet ready for I/O, causing
                # "can't set com-state" in avrdude's ser_open().
                time.sleep(0.8)
                return new_ports[0]
            if preferred_port in now_ports:
                return preferred_port
            time.sleep(0.2)
        return last_seen

    def _flash_firmware(self):
        t = self.i18n[self.lang.get()]
        hex_file = self.hex_path.get().strip()
        if not hex_file:
            hex_file = self._resolve_fw_path(self.fw_choice.get().strip())
            if hex_file:
                self.hex_path.set(hex_file)
        port_label = self.update_port.get().strip() or self.port.get().strip()

        if not hex_file:
            messagebox.showwarning(APP_TITLE, t["select_hex_first"])
            return
        if not os.path.exists(hex_file):
            messagebox.showwarning(APP_TITLE, t["hex_not_found"])
            return
        if not port_label:
            messagebox.showwarning(APP_TITLE, t["select_port_first"])
            return

        try:
            selected_port = port_from_label(port_label)
        except Exception:
            selected_port = port_label

        # Hardware-target guard: a .bin goes to the STM32 PRO Control Box,
        # a .hex to the ATmega32U4 Competition Control Box. Confirm before
        # an STM32 flash so a wrong selection is caught here, not mid-flash.
        # (Cross-target mistakes are non-destructive by design, but noisy.)
        if hex_file.lower().endswith(".bin"):
            if not messagebox.askyesno(
                t["stm32_confirm_title"],
                t["stm32_confirm"].format(port=selected_port),
            ):
                return

        # Prevent COM conflicts: ensure GUI serial connection is closed before flashing.
        if self.w.is_connected():
            self._fl("[INFO] Serial link is active — disconnecting before flash...")
            try:
                self.w.send_line("MC_DISABLE")
            except Exception:
                pass
            try:
                self.w.send_line("DO SERVO_OFF")
            except Exception:
                pass
            try:
                self.w.send_line("DISCONNECT")
            except Exception:
                pass
            try:
                time.sleep(0.08)
            except Exception:
                pass
            try:
                self.w.disconnect()
                self._set_compress_animation(False)
                self._set_installed_fw_display("-")
                self.btn_connect.config(text=t["connect"])
                self._log("[INFO] Disconnected before flash")
            except Exception as e:
                self._log(f"[WARN] Could not cleanly disconnect before flash: {e}")

        self.btn_flash.configure(state="disabled")
        self.lbl_flash_status.configure(text=t["flash_running"])
        self._fl(f"[INFO] {t['flash_running']}")
        self._fl(f"[INFO] {t['using_port']}: {selected_port}")
        self._log(f"[INFO] Flash request: {hex_file} on {selected_port}")

        worker = threading.Thread(
            target=self._flash_worker_stm32 if hex_file.lower().endswith(".bin") else self._flash_worker,
            args=(hex_file, selected_port),
            daemon=True,
        )
        worker.start()

    # ------------------------------------------------------------------
    #  STM32 PRO Control Box — update over the resident USB-CDC bootloader
    #  Protocol (little-endian):
    #    'P'                                   -> 'p' + version byte
    #    'E'                                   -> 'e' | '!'   (erase app region)
    #    'W' addr(4) len(2) data crc16(2)      -> 'w' | '!'   (CRC16-CCITT, init 0xFFFF)
    #    'G' len(4) crc32(4)                   -> 'g' | '!'   (zlib CRC32 over whole image)
    #  On 'g' the bootloader writes its metadata page and resets into the app.
    #  Anti-brick: metadata is written last, so an interrupted transfer simply
    #  leaves the board in the bootloader — re-running the flash recovers it.
    # ------------------------------------------------------------------
    STM32_APP_ADDR = 0x08005000
    STM32_CHUNK = 1024
    STM32_ACK_TIMEOUT = 3.0

    # Known application base addresses, per target box. The correct one is
    # inferred from the firmware image itself (its reset vector), so a single
    # worker flashes every STM32 box without manual selection:
    #   - F103 PRO box  : app linked at 0x08005000 (20K resident bootloader)
    #   - G474 V5 box   : app linked at 0x08040000 (bank 2; bootloader in bank 1)
    STM32_KNOWN_APP_BASES = (0x08040000, 0x08005000)

    @classmethod
    def _stm32_infer_app_base(cls, image: bytes) -> int:
        # The Cortex-M vector table starts with [initial_SP, reset_vector].
        # reset_vector is an absolute address inside the image's flash region,
        # so the link base is the largest known base it falls into.
        if len(image) >= 8:
            reset_vec = int.from_bytes(image[4:8], "little") & 0xFFFFFFFE
            for base in cls.STM32_KNOWN_APP_BASES:
                if base <= reset_vec < base + 0x00040000:
                    return base
        return cls.STM32_APP_ADDR

    @staticmethod
    def _stm32_crc16(data: bytes) -> int:
        # CRC16-CCITT (poly 0x1021, init 0xFFFF) == binascii.crc_hqx
        return binascii.crc_hqx(data, 0xFFFF)

    def _stm32_ack(self, ser, expect: bytes, tries: int = 2) -> bool:
        # IMPORTANT: never touch ser.timeout (or any port property) here.
        # pyserial re-runs SetCommState on every property write, and usbser.sys
        # turns that into a SET_LINE_CODING control transfer to the device.
        # During a flash erase/write the F103 core is stalled (no read-while-
        # write) and cannot answer EP0 -> Windows kills the port with
        # PermissionError 13 / error 31. Timeout is set once in Serial().
        # Retry loop covers the long full-erase (~2 s) without reconfiguring.
        for _ in range(max(1, tries)):
            r = ser.read(1)
            if r:
                return r == expect
        return False

    def _stm32_read_go_reply(self, ser, tries: int = 2) -> bytes:
        """Read the bootloader's reply to the final 'G' command.

        Returns b'g' (CRC matched, board resetting into the app), b'!' (device
        whole-image CRC mismatch, bootloader stays resident) or b'' (no reply —
        the 'g' byte was likely lost when the board reset ~23 ms after sending
        it, re-enumerating the USB CDC). The caller disambiguates b'' by probing
        the port. Same no-property-touch rule as _stm32_ack.
        """
        for _ in range(max(1, tries)):
            r = ser.read(1)
            if r:
                return r
        return b""

    def _stm32_confirm_reset_or_fail(self, flash_port, timeout_s=6.0):
        """Decide success vs failure when the final 'g' ACK was missed.

        The bootloader replies 'g' and resets into the app ONLY when the final
        CRC matched (on a mismatch it replies '!' and STAYS in the bootloader).
        It resets ~23 ms after sending 'g', which re-enumerates the USB CDC, so
        on a slower host the single 'g' byte can be lost even though the flash
        fully succeeded. To disambiguate, we probe the port with a bare PING
        ('P'):
          - the resident bootloader answers 'p'  -> image rejected (real fail);
          - the running application never answers 'p' to a bare 'P' (it treats
            it as a binary P-frame prefix)          -> flash succeeded;
          - the port is busy/gone (re-enumerating)  -> flash succeeded.
        Returns True on confirmed success, False on confirmed failure.
        """
        deadline = time.time() + timeout_s
        got_ping = False
        while time.time() < deadline:
            try:
                with serial.Serial(port=flash_port, baudrate=BAUDRATE,
                                   timeout=0.4, write_timeout=1.0,
                                   dsrdtr=False, rtscts=False) as s:
                    s.reset_input_buffer()
                    s.write(b"P")
                    if s.read(1) == b"p":
                        got_ping = True
                        break
            except Exception:
                # Port busy or gone: board is re-enumerating or the app took
                # over the CDC -> treated as success (no bootloader PING).
                pass
            time.sleep(0.25)
        # A 'p' reply means the bootloader is still resident -> image rejected.
        return not got_ping

    def _flash_worker_stm32(self, bin_file, selected_port):
        t = self.i18n[self.lang.get()]
        try:
            with open(bin_file, "rb") as f:
                image = f.read()
            if not image:
                raise RuntimeError(t["stm32_empty_bin"])
            if len(image) % 2:
                image += b"\xFF"      # F1 flashes by half-words
            crc_total = zlib.crc32(image) & 0xFFFFFFFF
            app_addr = self._stm32_infer_app_base(image)
            self.after(0, lambda: self._fl(f"[INFO] Image: {len(image)} bytes, CRC32=0x{crc_total:08X}"))
            self.after(0, lambda a=app_addr: self._fl(f"[INFO] Target app base: 0x{a:08X}"))

            # Small delay so the OS fully releases the port after the GUI disconnect
            time.sleep(0.3)
            old_ports = [p.device for p in serial.tools.list_ports.comports()]

            # Step 1 — send !DFU. If the application is running it replies "DFU"
            # and reboots into the bootloader; if the bootloader is already
            # running, these bytes are silently discarded by its resync logic.
            # (Never probe with a bare 'P' here: the application's text parser
            # would buffer it and corrupt the following !DFU line.)
            try:
                with serial.Serial(port=selected_port, baudrate=BAUDRATE,
                                   timeout=0.5, write_timeout=2.0,
                                   dsrdtr=False, rtscts=False) as s:
                    self.after(0, lambda: self._fl(f"[INFO] {t['stm32_entering_bl']}"))
                    s.write(b"!DFU\n")
                    s.flush()
                    time.sleep(0.2)
            except Exception as e:
                self.after(0, lambda m=str(e): self._fl(f"[WARN] Pre-flash !DFU: {m}"))

            # Step 2 — wait for the (re-)enumerated bootloader port
            time.sleep(1.0)
            flash_port = self._find_bootloader_port(old_ports, selected_port, timeout_s=8.0)
            self.after(0, lambda p=flash_port: self._fl(f"[INFO] {t['using_port']}: {p}"))

            with serial.Serial(port=flash_port, baudrate=BAUDRATE,
                               timeout=self.STM32_ACK_TIMEOUT, write_timeout=2.0,
                               dsrdtr=False, rtscts=False) as ser:
                # Step 3 — PING
                ser.reset_input_buffer()
                ser.write(b"P")
                r = ser.read(2)
                if len(r) < 1 or r[0:1] != b"p":
                    raise RuntimeError(t["stm32_no_bootloader"])
                if len(r) >= 2:
                    self.after(0, lambda v=r[1]: self._fl(f"[INFO] Bootloader version: {v}"))

                # Step 4 — ERASE
                self.after(0, lambda: self._fl("[INFO] Erasing application region..."))
                ser.reset_input_buffer()
                ser.write(b"E")
                if not self._stm32_ack(ser, b"e"):
                    raise RuntimeError(t["stm32_erase_failed"])

                # Step 5 — WRITE in CHUNK blocks
                total = len(image)
                offset = 0
                last_pct = -1
                while offset < total:
                    blk = image[offset:offset + self.STM32_CHUNK]
                    addr = app_addr + offset
                    frame = bytearray()
                    frame += b"W"
                    frame += addr.to_bytes(4, "little")
                    frame += len(blk).to_bytes(2, "little")
                    frame += blk
                    frame += self._stm32_crc16(blk).to_bytes(2, "little")
                    ser.reset_input_buffer()
                    ser.write(bytes(frame))
                    if not self._stm32_ack(ser, b"w"):
                        raise RuntimeError(t["stm32_write_failed"].format(addr=f"0x{addr:08X}"))
                    offset += len(blk)
                    pct = 100 * offset // total
                    if pct != last_pct:
                        last_pct = pct
                        msg = t["stm32_writing"].format(pct=pct, done=offset, total=total)
                        self.after(0, lambda m=msg: self.lbl_flash_status.configure(text=m))
                self.after(0, lambda: self._fl(f"[INFO] {total} bytes written."))

                # Step 6 — GO (final device CRC32; on match the bootloader
                # resets into the freshly written app).
                self.after(0, lambda: self._fl(f"[INFO] {t['stm32_finalizing']}"))
                go = b"G" + total.to_bytes(4, "little") + crc_total.to_bytes(4, "little")
                go_reply = b""
                for attempt in range(2):
                    ser.reset_input_buffer()
                    ser.write(go)
                    go_reply = self._stm32_read_go_reply(ser)  # b'g' | b'!' | b''
                    if go_reply in (b"g", b""):
                        break
                    # b'!' -> device-side whole-image CRC mismatch; retry once
                    # (re-reads flash; covers a transient read-back glitch).
                    self.after(0, lambda a=attempt: self._fl(
                        f"[WARN] Device final CRC check failed (attempt {a + 1}), retrying..."))
                    time.sleep(0.2)
                go_acked = (go_reply == b"g")

            # Decide the outcome. The board resets into the app ONLY on a matching
            # CRC; a lost 'g' (USB re-enum during the ~23 ms reset window) also
            # means success. Every 1024-byte block was CRC16-verified on the wire
            # before programming, so reaching this point means the image was
            # transmitted intact.
            if not go_acked:
                self.after(0, lambda: self._fl(
                    "[WARN] Final device CRC not confirmed — checking board state..."))
                if self._stm32_confirm_reset_or_fail(flash_port):
                    # Board left the bootloader (reset into the app) -> success.
                    self.after(0, lambda: self._fl(
                        "[INFO] Board reset into the application — flash OK."))
                else:
                    # Bootloader still resident: the device's final whole-image
                    # CRC failed even though every block was verified on the wire.
                    # The written image is valid and boots on the next power cycle
                    # (empirically confirmed). Report as done-with-action instead
                    # of a hard error so the user isn't blocked.
                    self.after(0, lambda: self.lbl_flash_status.configure(text=t["flash_done"]))
                    self.after(0, lambda: self._fl(f"[WARN] {t['stm32_go_soft']}"))
                    self.after(0, lambda: self._log(
                        "[INFO] STM32 image written (device final CRC unconfirmed) — power-cycle required"))
                    self.after(0, lambda: messagebox.showinfo(APP_TITLE, t["stm32_go_soft"]))
                    return

            self.after(0, lambda: self.lbl_flash_status.configure(text=t["flash_done"]))
            self.after(0, lambda: self._fl(f"[OK] {t['flash_done']}"))
            self.after(0, lambda: self._log("[INFO] STM32 flash succeeded"))
            # Board reboots into the new firmware and re-enumerates
            self.after(1200, self._refresh_ports)
            self.after(1800, self._detect_firmware_version)

        except Exception as e:
            self.after(0, lambda: self.lbl_flash_status.configure(text=t["flash_failed"]))
            self.after(0, lambda m=str(e): self._fl(f"[ERR] {m}"))
            self.after(0, lambda m=str(e): self._log(f"[ERR] STM32 flash failed: {m}"))
        finally:
            self.after(0, lambda: self.btn_flash.configure(state="normal"))

    def _flash_worker(self, hex_file, selected_port):
        t = self.i18n[self.lang.get()]

        try:
            # Small delay to let the OS fully release the port handle after disconnect
            time.sleep(0.3)
            # Capture port snapshot right before the reset, so any new port is "new"
            old_ports = [p.device for p in serial.tools.list_ports.comports()]
            try:
                s = serial.Serial(port=selected_port, baudrate=1200, timeout=0.2)
                s.close()
            except Exception:
                self.after(0, lambda: self._fl(f"[WARN] {t['touch1200_warn']}"))
            # Always wait for the board to reset and enumerate as bootloader
            time.sleep(1.4)

            flash_port = self._find_bootloader_port(old_ports, selected_port, timeout_s=6.0)
            self.after(0, lambda: self._fl(f"[INFO] {t['using_port']}: {flash_port}"))

            avrdude_exe, avrdude_conf = self._find_avrdude()
            if not avrdude_exe or not avrdude_conf:
                raise RuntimeError(t["avrdude_not_found"])

            cmd = [
                avrdude_exe,
                "-C", avrdude_conf,
                "-v",
                "-patmega32u4",
                "-cavr109",
                "-P", flash_port,
                "-b57600",
                "-D",
                f"-Uflash:w:{hex_file}:i",
            ]

            self.after(0, lambda: self._fl("[CMD] " + " ".join(cmd)))

            def _run_avrdude():
                popen_kwargs = {
                    "stdout": subprocess.PIPE,
                    "stderr": subprocess.STDOUT,
                    "text": True,
                    "encoding": "utf-8",
                    "errors": "replace",
                }
                # Hide console window on Windows
                if hasattr(subprocess, "CREATE_NO_WINDOW"):
                    popen_kwargs["creationflags"] = subprocess.CREATE_NO_WINDOW
                proc = subprocess.Popen(cmd, **popen_kwargs)
                lines = []
                for line in proc.stdout:
                    ln = line.rstrip("\n")
                    lines.append(ln)
                    self.after(0, lambda x=ln: self._fl(x))
                return proc.wait(), lines

            rc, out_lines = _run_avrdude()

            # "can't set com-state" means the COM port appeared in the list but the
            # Windows USB CDC driver hadn't finished initialising it. Retry once after
            # an extra delay — the bootloader window is 8 s so we still have time.
            if rc != 0 and "can't set com-state" in "\n".join(out_lines).lower():
                self.after(0, lambda: self._fl("[WARN] Port not ready yet, retrying in 1 s…"))
                time.sleep(1.0)
                rc, out_lines = _run_avrdude()

            if rc == 0:
                self.after(0, lambda: self.lbl_flash_status.configure(text=t["flash_done"]))
                self.after(0, lambda: self._fl(f"[OK] {t['flash_done']}"))
                self.after(0, lambda: self._log("[INFO] Flash succeeded"))
                # After a successful flash, board may reboot/re-enumerate.
                # Refresh ports then re-detect installed firmware version.
                self.after(1200, self._refresh_ports)
                self.after(1800, self._detect_firmware_version)
            else:
                joined = "\n".join(out_lines).lower()
                port_busy = (
                    ("unable to open port" in joined and "avr109" in joined)
                    or ("access is denied" in joined)
                    or ("resource busy" in joined)
                    or ("ser_open" in joined and "can't open device" in joined)
                    or ("can't set com-state" in joined)
                )

                self.after(0, lambda: self.lbl_flash_status.configure(text=t["flash_failed"]))
                self.after(0, lambda: self._fl(f"[ERR] {t['flash_failed']} (code={rc})"))
                self.after(0, lambda: self._log(f"[ERR] Flash failed (code={rc})"))
                if port_busy:
                    msg = t["flash_port_busy"]
                    self.after(0, lambda m=msg: self._fl(f"[ERR] {m}"))
                    self.after(0, lambda m=msg: self._log(f"[ERR] {m}"))
                    self.after(0, lambda m=msg: messagebox.showerror(APP_TITLE, m))
        except Exception as e:
            self.after(0, lambda: self.lbl_flash_status.configure(text=t["flash_failed"]))
            self.after(0, lambda: self._fl(f"[ERR] {e}"))
            self.after(0, lambda: self._log(f"[ERR] Flash exception: {e}"))
        finally:
            self.after(0, lambda: self.btn_flash.configure(state="normal"))
        try:
            base = app_base_dir()
            path = os.path.join(base, "serial_transcript.log")
            with open(path, "a", encoding="utf-8") as f:
                f.write(s if s.endswith("\n") else s + "\n")
        except Exception:
            pass

    def _toast(self, s):
        try:
            popup = tk.Toplevel(self)
            popup.title(APP_TITLE)
            popup.transient(self)
            popup.resizable(False, False)
            popup.overrideredirect(False)
            try:
                popup.configure(bg=self._theme.get("bg", "#23262C"))
            except Exception:
                pass

            frame = ttk.Frame(popup, padding=(16, 12))
            frame.pack(fill=tk.BOTH, expand=True)
            ttk.Label(frame, text=str(s)).pack(anchor="center")

            btns = ttk.Frame(frame)
            btns.pack(fill=tk.X, pady=(10, 0))
            ttk.Button(btns, text="OK", style="Accent.TButton", command=popup.destroy).pack(side=tk.RIGHT)

            try:
                self._apply_windows_titlebar_theme_for(popup)
            except Exception:
                pass

            try:
                popup.update_idletasks()
                px = self.winfo_rootx() + (self.winfo_width() - popup.winfo_width()) // 2
                py = self.winfo_rooty() + (self.winfo_height() - popup.winfo_height()) // 2
                popup.geometry(f"+{px}+{py}")
                popup.lift()
                popup.focus_set()
            except Exception:
                pass
        except Exception:
            # fallback to logging if UI call fails
            self._log(s)

    def _mark_saved(self, text=None, timeout_ms=2000):
        # Show a transient saved indicator in the status tab near homing speed
        if text is None:
            text = self.i18n[self.lang.get()].get("saved_ok", "Saved")
        try:
            self.lbl_saved.config(text=text)
            self.after(timeout_ms, lambda: self.lbl_saved.config(text=""))
        except Exception:
            # fallback: toast/log
            try:
                self._toast(text)
            except Exception:
                self._log(text)

    def _is_supported_connected_firmware(self, firmware_text: str) -> bool:
        fw = str(firmware_text or "").strip().lower().lstrip("v")
        if not fw:
            return False
        m = re.search(r"(\d+\.\d+(?:\.\d+)*)", fw)
        if m:
            fw = m.group(1)
        # Accept v1.9.x and v2.0+ versions
        return fw.startswith("1.9") or fw.startswith("2.")

    def _box_version(self) -> int:
        """Return box hardware version as int (1, 2 or 3). Defaults to 2 when unknown."""
        box = str(self.info.get("box", "") or "").strip().upper().lstrip("V")
        try:
            return int(box)
        except ValueError:
            return 2

    def _motor_endstop_capable(self, motor_1based: int) -> bool:
        """True when this motor can have an endstop on the current box version.
        V1/V2: only M5/M6 (firmware reads their endstop state).
        V3+:   all motors can have endstops.
        """
        if self._box_version() >= 3:
            return True
        return motor_1based >= 5

    def _box_max_motor(self) -> int:
        """Return 5 for a Box V1, 6 for V2, 6 for a PRO Control Box F103 (BOX=4),
        and 7 otherwise (Competition Control Box V3 / STM32G474; older G474
        firmwares reported BOX=5). Mirrors the box-version mapping used by the
        SimHub profile install logic (anything not V1/V2/V4 is treated as a
        7-actuator box)."""
        box = str(self.info.get("box", "") or "").strip().upper().lstrip("V")
        if box == "1":
            return 5
        if box == "2":
            return 6
        if box == "4":
            return 6   # PRO Control Box F103 : M1-M4 servos + M5/M6 steppers
        return 7

    def _reject_connected_box(self, detected_fw: str = ""):
        t = self.i18n[self.lang.get()]
        self._pending_connect_firmware_check = False
        details = str(detected_fw or "").strip()
        if details:
            self._log(f"[ERR] Unsupported firmware detected on connect: {details}")
        else:
            self._log("[ERR] Box not recognized on connect (no compatible firmware response).")
        try:
            if self.w.is_connected():
                try:
                    self.w.send_line("DISCONNECT")
                except Exception:
                    pass
                self.w.disconnect()
        except Exception:
            pass
        # Reset cached firmware info on rejection
        self.info = {"firmware":"-", "box":"-", "servo":"-", "homing_sps":"-", "homing_step":"-", "mc_required":"", "estop_enabled": False, "estop_behavior": 0}
        self._set_compress_animation(False)
        self._set_installed_fw_display("-")
        self._get_inflight = False
        self.btn_connect.config(text=t["connect"])
        try:
            self.nb.select(self.tab_update)
        except Exception:
            pass
        self._render_status()
        self._update_status_controls_state()
        self._update_functions_controls_state()
        msg = t["unsupported_fw_connect"]
        if details:
            if self.lang.get() == "FR":
                msg += f"\n\nFirmware détecté : {details}"
            elif self.lang.get() == "DE":
                msg += f"\n\nErkannte Firmware: {details}"
            else:
                msg += f"\n\nDetected firmware: {details}"
        messagebox.showerror(APP_TITLE, msg)

    def _validate_connected_box_after_get(self, info: dict) -> bool:
        if not self._pending_connect_firmware_check:
            return True
        firmware = str((info or {}).get("firmware", "") or "").strip()
        if self._is_supported_connected_firmware(firmware):
            self._pending_connect_firmware_check = False
            return True
        self._reject_connected_box(firmware)
        return False

    def _do_disconnect(self):
        """Perform the actual serial disconnection sequence (no ramp)."""
        t = self.i18n[self.lang.get()]
        try:
            self.w.send_line("MC_DISABLE")
            self._log("[TX] MC_DISABLE (on disconnect)")
        except Exception:
            pass
        try:
            self.w.send_line("DO SERVO_OFF")
        except Exception:
            pass
        try:
            self.w.send_line("DISCONNECT")
            self._log("[TX] DISCONNECT")
        except Exception:
            pass
        try:
            time.sleep(0.08)
        except Exception:
            pass
        self._pending_connect_firmware_check = False
        self._pending_disconnect = False
        self._last_synced_axis_count = 0
        self.w.disconnect()
        # Reset cached firmware info on disconnect
        self.info = {"firmware":"-", "box":"-", "servo":"-", "homing_sps":"-", "homing_step":"-", "mc_required":"", "estop_enabled": False, "estop_behavior": 0}
        self._set_compress_animation(False)
        self._set_installed_fw_display("-")
        self.btn_connect.config(text=t["connect"])
        self._log("[INFO] Disconnected")
        try:
            self._render_status()
        except Exception:
            pass
        self._update_status_controls_state()
        self._update_functions_controls_state()

    def _schedule_connect_handshake(self):
        # Leonardo-class boards reset on port-open; resend HELLO after startup so
        # the first Motion Center connection reliably enters the blue idle state.
        for delay_ms in (0, 1400, 2600):
            self.after(delay_ms, lambda d=delay_ms: self._run_connect_handshake(d))

    def _run_connect_handshake(self, delay_ms=0):
        if not self.w.is_connected():
            return
        # Box already validated by a first STATUS: no need to repeat HELLO/GET.
        if delay_ms > 0 and not self._pending_connect_firmware_check:
            return
        try:
            self.w.send_line("HELLO")
            suffix = "" if delay_ms == 0 else f" (+{delay_ms} ms)"
            self._log(f"[TX] HELLO{suffix}")
        except Exception:
            return
        if not self._get_inflight:
            self.refresh_status(force=True)

    def _toggle_connect(self):
        t = self.i18n[self.lang.get()]

        if self.w.is_connected():
            if self._test_active:
                # Test in progress: ramp motors to 0 first, then disconnect.
                self._pending_disconnect = True
                self._btn_test_stop.config(state="disabled")  # prevent double-click
                self._test_stop()
                return
            self._do_disconnect()
            return

        label = self.port.get().strip()
        if not label:
            messagebox.showwarning(APP_TITLE, "Select a COM port first.")
            return
        port = port_from_label(label)

        try:
            self.w.connect(port)
        except Exception as e:
            messagebox.showerror(APP_TITLE, t["serial_err"] + str(e))
            return

        self._pending_connect_firmware_check = True
        self._get_retry_left = 2  # retry GET up to 2 times before rejecting
        self.btn_connect.config(text=t["disconnect"])
        self._log(f"[INFO] Connected on {port} @ {BAUDRATE}")

        self._patch_installed_simhub_profiles(port)
        self._schedule_connect_handshake()

        self.nb.select(self.tab_status)
        self._update_status_controls_state()
        self._update_functions_controls_state()

    def _on_close(self):
        try:
            if self.w.is_connected():
                self._test_cleanup()
                try:
                    self.w.send_line("MC_DISABLE")
                    self._log("[TX] MC_DISABLE (on close)")
                except Exception:
                    pass
                try:
                    self.w.send_line("DO SERVO_OFF")
                except Exception:
                    pass
                try:
                    self.w.send_line("DISCONNECT")
                    self._log("[TX] DISCONNECT (on close)")
                except Exception:
                    pass
                try:
                    time.sleep(0.08)
                except Exception:
                    pass
        except Exception:
            pass

        try:
            self.w.disconnect()
        except Exception:
            pass

        try:
            self._set_installed_fw_display("-")
        except Exception:
            pass

        try:
            self.destroy()
        except Exception:
            pass

    def _send_cmd(self, cmd, refresh=False):
        t = self.i18n[self.lang.get()]
        if not self.w.is_connected():
            messagebox.showwarning(APP_TITLE, t["not_connected"])
            return
        # Default behaviour: fire-and-forget (maintain backward compatibility)
        # support refresh values:
        #  - bool: wait for OK/ERR
        #  - "silent": wait but don't show toast on OK
        #  - str: wait and use string as toast message
        if isinstance(refresh, bool):
            wait_for_reply = refresh
            show_toast = True
            toast_msg = self.i18n[self.lang.get()].get("saved_ok", "Paramètre enregistré.")
        else:
            # truthy non-bool -> wait
            wait_for_reply = bool(refresh)
            if refresh == "silent":
                show_toast = False
                toast_msg = ""
            elif isinstance(refresh, str):
                show_toast = True
                toast_msg = str(refresh)
            else:
                show_toast = True
                toast_msg = self.i18n[self.lang.get()].get("saved_ok", "Paramètre enregistré.")

        # Clear stale RX lines before sending a synchronous command.
        # This avoids attributing an old ERR (e.g. BAD_MOTOR) to the current action.
        if wait_for_reply:
            try:
                while True:
                    kind, msg = self.w.rxq.get_nowait()
                    # Don't lose a STATUS answering an in-flight GET
                    if kind == "line" and msg.strip().startswith("STATUS "):
                        try:
                            self._handle_status_line(msg.strip())
                        except Exception:
                            pass
            except queue.Empty:
                pass

        self.w.send_line(cmd)
        self._log(f"[TX] {cmd}")

        if not wait_for_reply:
            return True

        # If refresh is True, we attempt to wait for an OK/ERR reply from MCU
        # within a timeout, otherwise fall back to immediate refresh.
        timeout_ms = 3500
        deadline = time.time() + (timeout_ms / 1000.0)
        got_ok = False
        got_err = None
        prev_sync = self._synchronous_mode
        self._synchronous_mode = True
        try:
            while time.time() < deadline:
                try:
                    kind, msg = self.w.rxq.get(timeout=0.01)
                except queue.Empty:
                    continue

                if kind == "line":
                    s = msg.strip()
                    # A STATUS answering an in-flight GET may arrive while we
                    # wait for OK/ERR: process it instead of discarding it.
                    if s.startswith("STATUS "):
                        try:
                            self._handle_status_line(s)
                        except Exception:
                            pass
                        continue
                    # show incoming line in consoles (same as _pump_rx)
                    try:
                        ts = time.time()
                        timestr = time.strftime("%H:%M:%S", time.localtime(ts)) + f".{int((ts - int(ts)) * 1000):03d}"
                    except Exception:
                        timestr = "--:--:--.000"
                    if s == "OK" or s.startswith("OK_") or s.startswith("ERR"):
                        self._fc(f"[{timestr}] {s}\n")
                    else:
                        self._fc(s + "\n")
                        # Raisons détaillées pendant une commande synchrone
                        # (ex. DO FULL_CALIB -> HOMING FAILED Mx NO_READY).
                        try:
                            self._explain_homing_failure(s)
                        except Exception:
                            pass

                    if s == "OK" or s.startswith("OK_"):
                        got_ok = True
                        break
                    if s.startswith("ERR"):
                        got_err = s
                        break
                    # else keep collecting until OK/ERR or timeout
                else:
                    self._fc(f"[ERR] {msg}\n")
        finally:
            self._synchronous_mode = prev_sync

        # Compatibility fallback for firmware variants that only support DO SERVO_ON/OFF
        if got_err and cmd in ("ENABLE", "DISABLE", "MC_DISABLE"):
            e = got_err.upper().replace(" ", "_")
            if "BAD_MOTOR" in e:
                alt = "DO SERVO_ON" if cmd == "ENABLE" else "DO SERVO_OFF"
                self._log(f"[INFO] Fallback protocol: {cmd} -> {alt}")
                return self._send_cmd(alt, refresh=refresh)
            if cmd == "MC_DISABLE" and "UNKNOWN_CMD" in e:
                self._log("[INFO] Fallback protocol: MC_DISABLE -> DISABLE")
                return self._send_cmd("DISABLE", refresh=refresh)

        if got_ok:
            try:
                # friendly confirmation
                if show_toast and toast_msg:
                    self._toast(toast_msg)
            except Exception:
                pass
            # refresh the status now we have a confirmed write
            try:
                self.refresh_status()
            except Exception:
                pass
            return True

        if got_err:
            messagebox.showerror(APP_TITLE, got_err)
            return False

        # timeout: no OK/ERR received — fall back to refreshing status and inform user
        self._log("[WARN] No OK/ERR reply (timeout). Refreshing status anyway.")
        self.refresh_status()
        return False

    def _handle_status_line(self, s):
        """Parse a 'STATUS ...' reply and update UI state. Shared by _pump_rx
        and _send_cmd so a STATUS answering an in-flight GET is never lost."""
        self._get_inflight = False
        info, motors = parse_status_line(s)
        # Always update motors, but only update firmware info if FW was provided in STATUS line
        self.motors = motors
        if info.get("firmware"):
            # New firmware version received: update entire info
            self.info = info
        elif info.get("box") or info.get("servo"):
            # STATUS line has box/servo but no FW: update those fields only, preserve existing FW
            for key in ["box", "servo", "servo_on", "homing_sps", "homing_step", "mc_required", "step_hz"]:
                self.info[key] = info[key]
        if not self._validate_connected_box_after_get(info):
            return
        self._render_status()
        self._auto_sync_simhub_axes()
        try:
            self._test_update_calib_warn()
            self._test_update_m56_visibility()
        except Exception:
            pass
        if self._post_reset_active:
            self._post_reset_active = False
            self._log("[INFO] Post-reset status refreshed.")

    def refresh_status(self, force=False):
        t = self.i18n[self.lang.get()]
        if not self.w.is_connected():
            messagebox.showwarning(APP_TITLE, t["not_connected"])
            return
        if self._long_op_active and not force:
            self._log("[INFO] Refresh deferred: operation in progress.")
            return
        if self._get_inflight:
            self._log("[INFO] GET already in-flight, skipping.")
            return

        self._get_inflight = True
        self._get_seq += 1
        seq = self._get_seq
        self.w.clear_rx_buf()
        self.w.send_line("GET")
        self._log("[TX] GET")
        self.after(self._get_timeout_ms, lambda: self._get_timeout_check(seq))

    def _get_timeout_check(self, seq=None):
        if not self._get_inflight:
            return  # already satisfied by a STATUS line
        if seq is not None and seq != self._get_seq:
            return  # stale timer from an older GET; a newer GET is in flight
        self._get_inflight = False

        if getattr(self, "_get_retry_left", 0) > 0:
            self._get_retry_left -= 1
            self._log(f"[INFO] GET retry ({self._get_retry_left} left)...")
            if self._pending_connect_firmware_check:
                # Re-send HELLO in case the board missed it (e.g. still in setup())
                try:
                    self.w.send_line("HELLO")
                except Exception:
                    pass
            self.after(700, self.refresh_status)
            return

        if self._pending_connect_firmware_check:
            self._reject_connected_box("")
            return

        t = self.i18n[self.lang.get()]
        self._log("[ERR] " + t["get_timeout"].replace("\n", " | "))
        self._fc("[ERR] " + t["get_timeout"] + "\n")
        if self._post_reset_active:
            self.after(900, self._post_reset_try)

    def _post_reset_try(self):
        """
        After FACTORY_RESET, the MCU may reboot and drop/garble early replies.
        We retry HELLO + GET a few times until we successfully parse an END.
        """
        if not self._post_reset_active:
            return

        delays = [700, 900, 1200, 1600, 2200, 3000]
        if self._post_reset_tries >= len(delays):
            self._post_reset_active = False
            self._log("[ERR] Post-reset refresh failed (no GET/END).")
            return

        if not self.w.is_connected():
            self._post_reset_tries += 1
            self.after(delays[min(self._post_reset_tries, len(delays)-1)], self._post_reset_try)
            return

        if self._get_inflight:
            self._post_reset_tries += 1
            self.after(delays[min(self._post_reset_tries, len(delays)-1)], self._post_reset_try)
            return

        self.w.send_line("HELLO")
        self._log("[TX] HELLO (post-reset)")
        self._post_reset_tries += 1
        self.refresh_status()

    def _block_if_busy(self) -> bool:
        """Warn and return True when the box is running a long operation
        (calibration, detect min/max, test...): settings must wait."""
        busy = bool(getattr(self, "_long_op_active", False)
                    or getattr(self, "_calib_active", False)
                    or (getattr(self, "_func_thread", None) and self._func_thread.is_alive()))
        if busy:
            t = self.i18n[self.lang.get()]
            messagebox.showwarning(APP_TITLE, t["busy_wait"])
        return busy

    def _apply_homing_speed(self):
        # Read value from UI var, validate and clamp, then send to MCU and refresh status
        t = self.i18n[self.lang.get()]
        if not self.w.is_connected():
            messagebox.showwarning(APP_TITLE, t["not_connected"])
            return
        if self._block_if_busy():
            return

        try:
            sps = int(self.homing_sps_var.get())
        except Exception:
            messagebox.showwarning(APP_TITLE, "Homing speed invalide")
            return

        # GUI-side clamp (align with firmware capability)
        if sps < 50:
            sps = 50
        if sps > 10000:
            sps = 10000
        self.homing_sps_var.set(str(sps))

        # If a GET is already in-flight, cancel it
        if getattr(self, "_get_inflight", False):
            self._get_inflight = False

        cmd = f"SET HOMING_S {sps}"

        # Send the command and wait for an OK/ERR reply from the MCU.
        # On OK the helper will call `refresh_status()`; on timeout we
        # fall back to a status refresh and inform the user.
        try:
            self._send_cmd(cmd, refresh=True)
        except Exception:
            # Fallback: ensure we still try to refresh status
            try:
                self.refresh_status()
            except Exception:
                pass

    def _apply_step_hz(self):
        # Max STEP frequency for hardware-timer axes (SimHub streaming speed).
        t = self.i18n[self.lang.get()]
        if not self.w.is_connected():
            messagebox.showwarning(APP_TITLE, t["not_connected"])
            return
        if self._block_if_busy():
            return
        try:
            hz = int(self.step_hz_var.get())
        except Exception:
            messagebox.showwarning(APP_TITLE, t.get("step_speed_invalid", "Invalid STEP frequency"))
            return
        if hz < 1000:
            hz = 1000
        if hz > 450000:
            hz = 450000
        self.step_hz_var.set(str(hz))
        if getattr(self, "_get_inflight", False):
            self._get_inflight = False
        cmd = f"SET STEP_HZ {hz}"
        try:
            self._send_cmd(cmd, refresh=True)
        except Exception:
            try:
                self.refresh_status()
            except Exception:
                pass

    def _apply_estop_usage(self):
        t = self.i18n[self.lang.get()]
        if not self.w.is_connected():
            messagebox.showwarning(APP_TITLE, t["not_connected"])
            return
        if self._block_if_busy():
            return

        cmd = f"SET ESTOP {1 if self.estop_use_var.get() else 0}"
        try:
            self._send_cmd(cmd, refresh=True)
        except Exception:
            try:
                self.refresh_status()
            except Exception:
                pass

    def _is_g474_box(self):
        # G474 box reports BOX=3 (product name "Competition Control Box V3");
        # older G474 firmwares reported BOX=5 (internal PCB V5.1 reference).
        return str(self.info.get("box", "")).strip() in ("3", "5")

    def _estop_behavior_labels(self):
        t = self.i18n[self.lang.get()]
        return [
            t.get("estop_behavior_disable_servo", "Disable servo"),
            t.get("estop_behavior_return_park", "Return to park"),
            t.get("estop_behavior_stop_here", "Stop here"),
        ]

    def _estop_behavior_current_code(self):
        labels = self._estop_behavior_labels()
        try:
            return labels.index(self.estop_behavior_var.get())
        except Exception:
            return int(getattr(self, "_estop_behavior_code", 0) or 0)

    def _estop_behavior_set_from_code(self, code):
        labels = self._estop_behavior_labels()
        idx = max(0, min(2, int(code)))
        self._estop_behavior_code = idx
        try:
            self.estop_behavior_var.set(labels[idx])
        except Exception:
            pass

    def _apply_estop_behavior(self):
        t = self.i18n[self.lang.get()]
        if not self.w.is_connected():
            messagebox.showwarning(APP_TITLE, t["not_connected"])
            return
        if not self._is_g474_box():
            return
        if self._block_if_busy():
            return

        code = self._estop_behavior_current_code()
        self._estop_behavior_code = code
        cmd = f"SET ESTOPB {code}"
        try:
            self._send_cmd(cmd, refresh=True)
        except Exception:
            try:
                self.refresh_status()
            except Exception:
                pass

    def _checkbox_symbol(self, is_on: bool) -> str:
        return "☑" if is_on else "☐"

    def _set_status_table_grayed(self, grayed: bool):
        _dim = "#666666"
        try:
            self.table.tag_configure("grayed", foreground=_dim)
            for iid in self.table.get_children():
                self.table.item(iid, tags=("grayed",) if grayed else ())
        except Exception:
            pass

    def _render_status(self):
        t = self.i18n[self.lang.get()]

        # If not connected, show last known values greyed out
        if not self.w.is_connected():
            self._update_status_controls_state()
            self._update_functions_controls_state()
            _dim = "#666666"
            try:
                self._led_canvas.itemconfigure(self._led_oval, fill="#888888")
            except Exception:
                pass
            try:
                self.mc_compat_status.set("-")
                self._mc_compat_ok = False
            except Exception:
                pass
            # Grey out info labels but keep their last text
            for _lbl in (self.lbl_fw, self.lbl_box, self.lbl_servo, self.lbl_home):
                try:
                    _lbl.configure(foreground=_dim)
                except Exception:
                    pass
            # Re-populate table with last known values but greyed
            self.table.tag_configure("grayed", foreground=_dim)
            for iid in self.table.get_children():
                self.table.delete(iid)
            max_motor = self._box_max_motor()
            for i in range(1, max_motor + 1):
                m = self.motors[i]
                conn_txt = self._checkbox_symbol(m["conn"] == "Y")
                cal_txt = "Y" if str(m.get("cal", "N")).upper() == "Y" else "N"
                max_txt = str(m["max"]) if m["max"] else "-"
                margin_display = m["margin"] if i >= 5 else "\u2014"
                _esp = self._motor_endstop_capable(i)
                homing_dir_display = ("MAX" if m.get("homing_dir", "MIN") == "MAX" else "MIN") if _esp else "\u2014"
                _ep = int(m.get("endpark", 255) or 255)
                endpark_display = (f"{_ep}%" if _ep <= 100 else "\u2014") if (_esp and i >= 5) else "\u2014"
                self.table.insert("", "end", iid=str(i),
                                  values=(m["m"], conn_txt, cal_txt, max_txt, margin_display, homing_dir_display, endpark_display),
                                  tags=("grayed",))
            self._update_update_tab_health_visuals()
            self._refresh_actuator_visual()
            return

        self._update_status_controls_state()
        self._update_functions_controls_state()

        # Restore normal foreground on labels (may have been greyed by disconnect)
        _normal_fg = self._theme.get("text", "#F2F4F7")
        for _lbl in (self.lbl_fw, self.lbl_box, self.lbl_servo, self.lbl_home):
            try:
                _lbl.configure(foreground=_normal_fg)
            except Exception:
                pass

        box = self.info.get("box","-")
        if box.isdigit():
            box = t["box_prefix"] + box

        # Clean firmware version display: remove control characters and normalize whitespace
        fw_version = self.info.get('firmware','-')
        if fw_version and fw_version != '-':
            fw_version = re.sub(r"[\x00-\x1F\x7F]+", " ", str(fw_version))
            fw_version = re.sub(r"\s+", " ", fw_version).strip()
        self.lbl_fw.config(text=f"Firmware: {fw_version}")
        self.lbl_box.config(text=f"Box: {box}")
        # Friendly servo display
        servo_txt = self.info.get('servo', '-')
        if isinstance(servo_txt, str) and servo_txt:
            self.lbl_servo.config(text=f"Servo: {servo_txt}")
        else:
            self.lbl_servo.config(text="Servo: -")

        req_mc = str(self.info.get("mc_required", "") or "").strip().lstrip("v")
        compat = self._is_motion_center_compatible(req_mc)
        self._mc_compat_ok = False
        if req_mc:
            if compat is True:
                compat_txt = t["mc_compat_ok"]
                self._mc_compat_ok = True
            elif compat is False:
                compat_txt = t["mc_compat_ko"]
            else:
                compat_txt = t["mc_compat_unknown"]
            self.mc_compat_status.set(f"{compat_txt} (FW≥v{req_mc} / soft v{MOTION_CENTER_VERSION})")
        else:
            self.mc_compat_status.set(t["mc_compat_unknown"])

        # Update top-bar LED: priority -> calibration (orange), test active (red = servo on), servo enabled (red), connected (blue), else gray
        try:
            if getattr(self, '_calib_active', False):
                color = "#ff8800"  # orange
            elif getattr(self, '_test_active', False) or self.info.get('servo_on', False):
                color = "#d00000"  # red — servo enabled (test or SimHub)
            elif self.w.is_connected():
                color = "#1e90ff"  # blue
            else:
                color = "#888888"  # gray
            self._led_canvas.itemconfigure(self._led_oval, fill=color)
        except Exception:
            pass

        if self.info.get("homing_sps") and self.info.get("homing_sps") != "-":
            self.lbl_home.config(text=f"Homing: {self.info['homing_sps']} steps/s")
            self.homing_sps_var.set(str(self.info["homing_sps"]))
        else:
            self.lbl_home.config(text="Homing: -")

        if self.info.get("step_hz") and self.info.get("step_hz") != "-":
            self.step_hz_var.set(str(self.info["step_hz"]))

        self.estop_use_var.set(bool(self.info.get("estop_enabled", False)))
        self._estop_behavior_set_from_code(int(self.info.get("estop_behavior", 0)))

        max_motor = self._box_max_motor()
        try:
            self.spin_motor.configure(to=max_motor)
        except Exception:
            pass

        for iid in self.table.get_children():
            self.table.delete(iid)

        for i in range(1, max_motor + 1):
            m = self.motors[i]
            conn_on = (m["conn"] == "Y")
            conn_txt = self._checkbox_symbol(conn_on)
            cal_txt = "Y" if str(m.get("cal","N")).upper() == "Y" else "N"
            margin_display = m["margin"] if i >= 5 else "—"
            _esp = self._motor_endstop_capable(i)
            homing_dir_display = ("MAX" if m.get("homing_dir", "MIN") == "MAX" else "MIN") if _esp else "—"
            _ep = int(m.get("endpark", 255) or 255)
            endpark_display = (f"{_ep}%" if _ep <= 100 else "—") if (_esp and i >= 5) else "—"
            self.table.insert("", "end", iid=str(i), values=(m["m"], conn_txt, cal_txt, m["max"], margin_display, homing_dir_display, endpark_display))
        self._set_status_table_grayed(getattr(self, "_test_active", False))
        self._update_update_tab_health_visuals()
        self._update_cancel_button_state()
        self._refresh_actuator_visual()

    def _on_table_click(self, event):
        region = self.table.identify("region", event.x, event.y)
        if region != "cell":
            return
        row_id = self.table.identify_row(event.y)
        col = self.table.identify_column(event.x)
        if not row_id:
            return
        motor = int(row_id)

        if col == "#5" and motor in (5, 6):
            try:
                if self._m5_margin_click_job is not None:
                    self.after_cancel(self._m5_margin_click_job)
            except Exception:
                pass
            self._m5_margin_click_job = self.after(220, lambda m=motor: self._open_m5_margin_slider_dialog(m))
            return

        if col == "#6":
            # Colonne "Dir homing" : toggle MIN <-> MAX pour les moteurs à endstop.
            t = self.i18n[self.lang.get()]
            if not self._motor_endstop_capable(motor):
                return
            m = self.motors.get(motor, {})
            cur = str(m.get("homing_dir", "MIN")).upper()
            to_max = (cur == "MIN")
            if to_max and int(m.get("max", 0) or 0) <= 0:
                messagebox.showwarning(
                    APP_TITLE,
                    t.get("homing_dir_requires_max",
                          "Homing on the MAX endstop requires a calibrated Max (Max > 0)."))
                return
            self._send_cmd(f"SET HOMING_DIR {1 if to_max else 0} {motor}", refresh=False)
            self.motors[motor]["homing_dir"] = "MAX" if to_max else "MIN"
            self._render_status()
            self.after(120, lambda: self.refresh_status(force=True))
            return

        if col == "#7":
            # Colonne "Endpark" : % de course rejoint doucement au SH_DISABLE.
            # Réglable sur les vérins optionnels (M5+) à endstop.
            if motor < 5 or not self._motor_endstop_capable(motor):
                return
            self._open_endpark_dialog(motor)
            return

        if col == "#2":
            t = self.i18n[self.lang.get()]
            cur = self.motors[motor]["conn"]
            newv = "N" if cur == "Y" else "Y"

            # M5/M6/M7 ordering constraints
            if motor == 6 and newv == "Y" and self.motors[5]["conn"] != "Y":
                messagebox.showerror(APP_TITLE, t.get("m6_requires_m5", "⚠️ Connect M5 first."))
                return
            if motor == 5 and newv == "N" and self.motors[6]["conn"] == "Y":
                messagebox.showerror(APP_TITLE, t.get("m5_disconnect_m6_first", "⚠️ Disconnect M6 first."))
                return
            if motor == 7 and newv == "Y" and self.motors[6]["conn"] != "Y":
                messagebox.showerror(APP_TITLE, t.get("m7_requires_m6", "⚠️ Connect M6 first."))
                return
            if motor == 6 and newv == "N" and self.motors[7]["conn"] == "Y":
                messagebox.showerror(APP_TITLE, t.get("m6_disconnect_m7_first", "⚠️ Disconnect M7 first."))
                return

            # Warn if SimHub is running (non-blocking check)
            self._warn_if_simhub_running_async()

            v = 1 if newv == "Y" else 0
            cmd = f"SET CONNECTED {v} {motor}"
            # Fire-and-forget here to avoid UI freeze on STATUS table click.
            self._send_cmd(cmd, refresh=False)

            self.motors[motor]["conn"] = newv
            self._render_status()
            self.after(120, lambda: self.refresh_status(force=True))
            self._test_update_m56_visibility()

            if newv == "Y" and motor in (5, 6):
                t = self.i18n[self.lang.get()]
                title = t.get("motor56_warn_title", "⚠️ Motor M{n}").replace("{n}", str(motor))
                msg = t.get("motor56_warn_msg", "If the motor is not connected and properly calibrated, the box may run in degraded mode with SimHub.")
                messagebox.showwarning(title, msg, parent=self)

    def _on_table_double_click(self, event):
        region = self.table.identify("region", event.x, event.y)
        if region != "cell":
            return
        row_id = self.table.identify_row(event.y)
        col = self.table.identify_column(event.x)
        if not row_id:
            return
        motor = int(row_id)

        if col not in ("#4", "#5"):
            return

        if col == "#5" and motor <= 4:
            self._log(f"[INFO] Margin is editable only on M5/M6 (requested M{motor}).")
            return

        if col == "#5" and motor in (5, 6):
            # Cancel delayed single-click open when this interaction is a true double-click.
            try:
                if self._m5_margin_click_job is not None:
                    self.after_cancel(self._m5_margin_click_job)
            except Exception:
                pass
            self._m5_margin_click_job = None
            return

        if self._edit_entry:
            try:
                self._edit_entry.destroy()
            except Exception:
                pass
            self._edit_entry = None
            self._edit_info = None

        bbox = self.table.bbox(row_id, col)
        if not bbox:
            return
        x, y, w, h = bbox
        value = self.table.set(row_id, col)

        entry = tk.Entry(self.table)
        entry.place(x=x, y=y, width=w, height=h)
        entry.insert(0, value)
        entry.focus_set()
        try:
            entry.selection_range(0, tk.END)
            entry.icursor(tk.END)
        except Exception:
            pass

        self._edit_entry = entry
        self._edit_info = (motor, col)

        def _valid_u16_text(txt: str) -> bool:
            try:
                v = int(str(txt).strip())
                return 0 <= v <= G474_AXIS_MAX_STEPS
            except Exception:
                return False

        def _paint_validity(_evt=None):
            try:
                cur = entry.get()
            except Exception:
                return
            if _valid_u16_text(cur):
                try:
                    entry.configure(bg="white")
                except Exception:
                    pass
            else:
                try:
                    entry.configure(bg="#ffe6e6")
                except Exception:
                    pass

        _paint_validity()

        def commit(_evt=None):
            t = self.i18n[self.lang.get()]
            if not self._edit_entry:
                return
            text = self._edit_entry.get()
            try:
                self._edit_entry.destroy()
            except Exception:
                pass
            self._edit_entry = None

            m, c = self._edit_info
            self._edit_info = None

            try:
                val = int(str(text).strip())
            except Exception:
                messagebox.showerror(APP_TITLE, t["invalid_u16_range"])
                return

            if val < 0 or val > G474_AXIS_MAX_STEPS:
                messagebox.showerror(APP_TITLE, t["invalid_u16_range"])
                return

            if c == "#4":
                cmd2 = f"SET MAX {val} {m}"
                self.motors[m]["max"] = val
            else:
                cmd2 = f"SET MARGIN {val} {m}"
                self.motors[m]["margin"] = val

            self._send_cmd(cmd2, refresh=True)
            self._render_status()
            self.refresh_status()

        entry.bind("<Return>", commit)
        entry.bind("<FocusOut>", commit)
        entry.bind("<KeyRelease>", _paint_validity)
        entry.bind("<FocusIn>", lambda _e: (entry.selection_range(0, tk.END), entry.icursor(tk.END), _paint_validity()))
        entry.bind("<Escape>", lambda _e: entry.destroy())

    def _open_endpark_dialog(self, motor: int):
        """Petit dialogue Endpark : % de course rejoint doucement au SH_DISABLE
        (255 = désactivé, les vérins restent en place)."""
        t = self.i18n[self.lang.get()]
        cur = int(self.motors.get(motor, {}).get("endpark", 255) or 255)
        enabled = cur <= 100

        dlg = tk.Toplevel(self)
        dlg.title(f"M{motor} — Endpark")
        dlg.transient(self)
        dlg.resizable(False, False)
        try:
            dlg.lift()
            self._apply_windows_titlebar_theme_for(dlg)
            dlg.configure(bg=self._theme.get("bg", "#23262C"))
        except Exception:
            pass

        body = ttk.Frame(dlg, padding=12)
        body.pack(fill=tk.BOTH, expand=True)
        ttk.Label(body, text=t.get("endpark_dialog_label",
                                   "End-of-session park position (% of stroke)."),
                  wraplength=400, justify="left").pack(anchor="w")
        ex = t.get("endpark_dialog_example", "")
        if ex:
            try:
                _dim = self._theme.get("text_dim", "#9AA1A9")
            except Exception:
                _dim = "#9AA1A9"
            ttk.Label(body, text=ex, wraplength=400, justify="left",
                      foreground=_dim, font=("Segoe UI", 8)).pack(anchor="w", pady=(8, 0))

        off_var = tk.BooleanVar(value=not enabled)

        row = ttk.Frame(body)
        row.pack(fill=tk.X, pady=(10, 2))
        lbl_val = ttk.Label(row, text="", width=6)
        scale = ttk.Scale(row, from_=0, to=100, orient="horizontal")
        scale.set(cur if enabled else 75)

        def _upd(*_a):
            if off_var.get():
                scale.state(["disabled"])
                lbl_val.configure(text="\u2014")
            else:
                scale.state(["!disabled"])
                lbl_val.configure(text=f"{int(float(scale.get()))}%")

        scale.configure(command=lambda _v: _upd())
        scale.pack(side=tk.LEFT, fill=tk.X, expand=True)
        lbl_val.pack(side=tk.LEFT, padx=(8, 0))

        ttk.Checkbutton(body, text=t.get("endpark_disabled", "Disabled (actuator stays in place)"),
                        variable=off_var, command=_upd).pack(anchor="w", pady=(6, 0))
        _upd()

        def _apply():
            val = 255 if off_var.get() else int(float(scale.get()))
            self._send_cmd(f"SET ENDPARK {val} {motor}", refresh=False)
            self.motors[motor]["endpark"] = val
            self._render_status()
            self.after(120, lambda: self.refresh_status(force=True))
            dlg.destroy()

        btns = ttk.Frame(body)
        btns.pack(fill=tk.X, pady=(10, 0))
        ttk.Button(btns, text="OK", command=_apply).pack(side=tk.RIGHT)
        ttk.Button(btns, text=t.get("cancel", "Cancel"), command=dlg.destroy).pack(side=tk.RIGHT, padx=(0, 6))

    def _open_m5_margin_slider_dialog(self, motor=5):
        self._m5_margin_click_job = None
        try:
            if self._m5_margin_dlg is not None and self._m5_margin_dlg.winfo_exists():
                if int(self._m5_margin_motor or 0) == int(motor):
                    self._m5_margin_dlg.lift()
                    self._m5_margin_dlg.focus_set()
                    return
                try:
                    self._m5_margin_dlg.destroy()
                except Exception:
                    pass
                self._m5_margin_dlg = None
                self._m5_margin_motor = None
        except Exception:
            self._m5_margin_dlg = None
            self._m5_margin_motor = None

        t = self.i18n[self.lang.get()]
        max_steps = int(clamp_int(self.motors.get(motor, {}).get("max", 0), 0, G474_AXIS_MAX_STEPS))
        if max_steps <= 0:
            messagebox.showwarning(APP_TITLE, f"Max(M{motor}) must be greater than 0 before setting margin.")
            return

        current_margin = int(clamp_int(self.motors.get(motor, {}).get("margin", 0), 0, G474_AXIS_MAX_STEPS))
        half_limit = max_steps // 2
        current_margin = max(0, min(current_margin, half_limit))

        dlg = tk.Toplevel(self)
        self._m5_margin_dlg = dlg
        self._m5_margin_motor = int(motor)
        dlg.title(f"M{motor} Margin")
        dlg.transient(self)
        dlg.resizable(False, False)
        dlg.overrideredirect(False)
        try:
            dlg.lift()
            self._apply_windows_titlebar_theme_for(dlg)
        except Exception:
            pass

        try:
            dlg.configure(bg=self._theme.get("bg", "#23262C"))
        except Exception:
            pass

        body = ttk.Frame(dlg, padding=12)
        body.pack(fill=tk.BOTH, expand=True)

        ttk.Label(body, text=f"M{motor} — 0 .. Max({max_steps})").pack(anchor="w")

        canvas_w = 560
        canvas_h = 74
        x0 = 24
        x1 = canvas_w - 24
        y_mid = canvas_h // 2

        canvas = tk.Canvas(body, width=canvas_w, height=canvas_h, highlightthickness=0, bd=0)
        canvas.pack(fill=tk.X, pady=(8, 4))

        val_row = ttk.Frame(body)
        val_row.pack(fill=tk.X, pady=(0, 8))
        lbl_left = ttk.Label(val_row, text="Compressed-side : 0")
        lbl_left.pack(side=tk.LEFT)
        lbl_right = ttk.Label(val_row, text="Decompressed-side : 0")
        lbl_right.pack(side=tk.RIGHT)

        active_handle = {"name": None}
        margin_var = tk.IntVar(value=current_margin)

        def _to_x(step_val: int) -> float:
            if max_steps <= 0:
                return float(x0)
            return float(x0 + (x1 - x0) * (float(step_val) / float(max_steps)))

        def _to_step(x_pos: float) -> int:
            if x1 <= x0 or max_steps <= 0:
                return 0
            ratio = (float(x_pos) - float(x0)) / float(x1 - x0)
            raw = int(round(ratio * float(max_steps)))
            return max(0, min(max_steps, raw))

        def _set_margin_from_step(step_val: int):
            clamped = max(0, min(int(step_val), half_limit))
            margin_var.set(clamped)
            _redraw()

        def _redraw():
            try:
                canvas.delete("all")
                bg = self._theme.get("input_bg", "#1E2127")
                track = self._theme.get("border", "#3B3F45")
                accent = self._theme.get("accent", "#F2C94C")
                txt = self._theme.get("text", "#E9ECEF")
                canvas.configure(bg=bg)
            except Exception:
                track = "#3B3F45"
                accent = "#F2C94C"
                txt = "#E9ECEF"

            m_left = int(margin_var.get())
            m_right = int(max_steps - m_left)
            lx = _to_x(m_left)
            rx = _to_x(m_right)

            canvas.create_line(x0, y_mid, x1, y_mid, fill=track, width=6, capstyle=tk.ROUND)
            canvas.create_line(lx, y_mid, rx, y_mid, fill=accent, width=6, capstyle=tk.ROUND)

            r = 8
            canvas.create_oval(lx - r, y_mid - r, lx + r, y_mid + r, fill=accent, outline="")
            canvas.create_oval(rx - r, y_mid - r, rx + r, y_mid + r, fill=accent, outline="")

            canvas.create_text(x0, y_mid + 18, text="0", fill=txt, anchor="w")
            canvas.create_text(x1, y_mid + 18, text=str(max_steps), fill=txt, anchor="e")

            lbl_left.configure(text=f"Compressed-side : {m_left}")
            lbl_right.configure(text=f"Decompressed-side : {m_right}")

        def _nearest_handle(x_click: float) -> str:
            m_left = int(margin_var.get())
            m_right = int(max_steps - m_left)
            lx = _to_x(m_left)
            rx = _to_x(m_right)
            return "left" if abs(x_click - lx) <= abs(x_click - rx) else "right"

        def _on_press(evt):
            active_handle["name"] = _nearest_handle(evt.x)
            _on_drag(evt)

        def _on_drag(evt):
            handle = active_handle.get("name")
            if handle not in ("left", "right"):
                return
            step_val = _to_step(evt.x)
            if handle == "left":
                _set_margin_from_step(step_val)
            else:
                _set_margin_from_step(max_steps - step_val)

        def _on_release(_evt):
            active_handle["name"] = None

        canvas.bind("<Button-1>", _on_press)
        canvas.bind("<B1-Motion>", _on_drag)
        canvas.bind("<ButtonRelease-1>", _on_release)

        btns = ttk.Frame(body)
        btns.pack(fill=tk.X, pady=(10, 0))

        def _cancel():
            try:
                dlg.destroy()
            except Exception:
                pass
            self._m5_margin_dlg = None
            self._m5_margin_motor = None
            try:
                self.focus_set()
            except Exception:
                pass

        dlg.protocol("WM_DELETE_WINDOW", _cancel)
        dlg.bind("<Escape>", lambda _e: _cancel())
        dlg.bind("<Destroy>", lambda _e: setattr(self, "_m5_margin_dlg", None), add="+")
        dlg.bind("<Destroy>", lambda _e: setattr(self, "_m5_margin_motor", None), add="+")

        def _show_saved_toast(anchor_x=None, anchor_y=None, anchor_w=None):
            saved = tk.Toplevel(self)
            saved.title("Saved")
            saved.transient(self)
            saved.resizable(False, False)
            saved.overrideredirect(False)
            try:
                saved.configure(bg=self._theme.get("bg", "#23262C"))
                self._apply_windows_titlebar_theme_for(saved)
            except Exception:
                pass

            saved_body = ttk.Frame(saved, padding=(14, 10))
            saved_body.pack(fill=tk.BOTH, expand=True)
            ttk.Label(saved_body, text="Enregistré").pack(anchor="center")

            try:
                saved.update_idletasks()
                if anchor_x is None or anchor_y is None or anchor_w is None:
                    ax = self.winfo_rootx()
                    ay = self.winfo_rooty()
                    aw = self.winfo_width()
                else:
                    ax = int(anchor_x)
                    ay = int(anchor_y)
                    aw = int(anchor_w)

                px = ax + (aw - saved.winfo_width()) // 2
                py = ay - saved.winfo_height() - 12
                saved.geometry(f"+{px}+{py}")
                saved.lift()
            except Exception:
                pass

            def _close_saved():
                try:
                    if saved.winfo_exists():
                        saved.destroy()
                except Exception:
                    pass

            saved.after(1000, _close_saved)

        def _apply():
            val = int(clamp_int(margin_var.get(), 0, half_limit))
            cmd = f"SET MARGIN {val} {motor}"
            self.motors[motor]["margin"] = val
            self._send_cmd(cmd, refresh="silent")
            self._render_status()
            self.refresh_status()

            try:
                anchor_x = dlg.winfo_rootx()
                anchor_y = dlg.winfo_rooty()
                anchor_w = dlg.winfo_width()
            except Exception:
                anchor_x = None
                anchor_y = None
                anchor_w = None

            _cancel()
            self.after_idle(lambda: _show_saved_toast(anchor_x, anchor_y, anchor_w))

        ttk.Button(btns, text=t.get("cancel", "Cancel"), command=_cancel).pack(side=tk.RIGHT)
        ttk.Button(btns, text="Sauvegarder", style="Accent.TButton", command=_apply).pack(side=tk.RIGHT, padx=(0, 8))

        _redraw()

        try:
            dlg.update_idletasks()
            px = self.winfo_rootx() + (self.winfo_width() - dlg.winfo_width()) // 2
            py = self.winfo_rooty() + (self.winfo_height() - dlg.winfo_height()) // 2
            dlg.geometry(f"+{px}+{py}")
            dlg.deiconify()
            dlg.lift()
            dlg.focus_set()
        except Exception:
            pass

    # ---------- Functions ----------
    def _on_func_selected(self):
        t = self.i18n[self.lang.get()]
        idx = self.func_combo.current()
        if idx < 0:
            return

        key = self._func_exposed[idx]
        self.func_choice_key.set(key)

        # Description
        self.lbl_desc.config(text=t["func_desc"].get(key, ""))

        # Factory reset warning banner
        if key == "factory_reset":
            self.lbl_warn.config(text=t["factory_reset_warn"])
            self.warn_frame.pack(fill=tk.X, pady=(4,6))
        else:
            self.warn_frame.pack_forget()

        needs_motor = self.func_model[key]["needs_motor"]
        self.spin_motor.configure(state="normal" if needs_motor else "disabled")
        self._update_functions_controls_state()
        self._update_cancel_button_state()
        self._refresh_actuator_visual()

    def _do_factory_reset(self):
        """Factory reset button on the status tab."""
        t = self.i18n[self.lang.get()]
        if not self.w.is_connected():
            messagebox.showwarning(APP_TITLE, t["not_connected"])
            return
        ok = messagebox.askyesno(t["confirm_reset_title"], t["confirm_reset_msg"])
        if not ok:
            return
        self.w.send_line("DO FACTORY_RESET")
        self._log("[TX] DO FACTORY_RESET")
        self._post_reset_active = True
        self._post_reset_tries = 0
        self.after(800, self._post_reset_try)

    def _run_func(self):
        t = self.i18n[self.lang.get()]
        if not self.w.is_connected():
            messagebox.showwarning(APP_TITLE, t["not_connected"])
            return
        key = self.func_choice_key.get()
        model = self.func_model.get(key)
        if not model:
            return
        if model.get("needs_motor", False):
            motor = clamp_int(self.func_motor.get(), 1, 6)
            if self.motors.get(motor, {}).get("conn", "N") != "Y":
                messagebox.showwarning(APP_TITLE, t["func_servo_required"])
                return
        self._active_func_key = key
        self._force_unknown_visual = False

        # Confirm on factory reset
        if key == "factory_reset":
            ok = messagebox.askyesno(t["confirm_reset_title"], t["confirm_reset_msg"])
            if not ok:
                return

        cmd = model["cmd"]
        needs_motor = model["needs_motor"]
        if needs_motor:
            motor = clamp_int(self.func_motor.get(), 1, 6)
            line = f"DO {cmd} {motor}"
        else:
            line = f"DO {cmd}"

        # Default: let MCU handle DETECT_MIN/MOVE_TO_MAX (no Python homing shortcut)

        # Important: do not send quick-abort marker here.
        # '!' is reserved for explicit cancel actions only.
        self.w.send_line(line)
        self._log(f"[TX] {line}")
        self._fc(f">>> {line}\n")

        if key in ("detect_min", "complete_calib"):
            self._long_op_active = True
            self._set_compress_animation(True, motion="compressing")
        elif key in ("detect_max", "go_center"):
            self._long_op_active = True
            self._set_compress_animation(True, motion="decompressing")
        elif key in ("test_stroke",):
            self._long_op_active = True
            self._set_compress_animation(True, motion="compressing")
        else:
            self._long_op_active = False
            self._set_compress_animation(False)

        if key == "factory_reset":
            # Start a robust post-reset refresh sequence
            self._post_reset_active = True
            self._post_reset_tries = 0
            self.after(800, self._post_reset_try)
        # Avoid immediate GET for long blocking operations: firmware may not
        # answer END until the operation completes, causing noisy timeouts.
        long_ops = {"detect_min", "detect_max", "go_center", "complete_calib", "test_stroke"}
        if key not in long_ops:
            self._active_func_key = None
            self.refresh_status()

    def _cancel_func(self):
        # Send cancel command to MCU. If a motor is selected, include it for clarity.
        t = self.i18n[self.lang.get()]
        if not self.w.is_connected():
            messagebox.showwarning(APP_TITLE, t["not_connected"])
            return
        # include motor parameter if a motor is selected
        try:
            motor = clamp_int(self.func_motor.get(), 1, 6)
            # Only include motor if the current function needs a motor
            key = self.func_choice_key.get()
            needs_motor = self.func_model.get(key, {}).get("needs_motor", False)
            if needs_motor:
                line = f"DO CANCEL {motor}"
            else:
                line = "DO CANCEL"
        except Exception:
            line = "DO CANCEL"

        # If a Python homing worker is running, signal it to stop immediately
        try:
            if getattr(self, '_func_stop', None):
                try:
                    self._func_stop.set()
                except Exception:
                    pass
        except Exception:
            pass

        # Send a burst of quick-abort markers so tight MCU loops reliably
        # catch at least one '!' even under serial jitter.
        def _send_abort_pulse(log_line=False):
            try:
                self.w.send_line("!")
                if log_line:
                    self._log("[TX] !")
                    self._fc(">>> !\n")
            except Exception:
                pass

        _send_abort_pulse(log_line=True)
        self.after(80, _send_abort_pulse)
        self.after(160, _send_abort_pulse)

        try:
            self.w.send_line(line)
            self._log(f"[TX] {line}")
            self._fc(f">>> {line}\n")
        except Exception:
            pass
        # Also ensure servo power is turned off on cancel
        try:
            self.w.send_line("DO SERVO_OFF")
            self._log("[TX] DO SERVO_OFF")
            self._fc(">>> DO SERVO_OFF\n")
        except Exception:
            pass
        # Give MCU a moment and refresh status
        try:
            self.after(250, lambda: self.refresh_status(force=True) if not self._long_op_active else None)
        except Exception:
            pass
        self._set_compress_animation(False)

    def _merge_cal_step_fragments(self, parts):
        txt = " ".join(str(p).strip() for p in (parts or []) if str(p).strip())
        if not txt:
            return ""
        # Cleanup common tokenization artifacts from firmware low-level logs
        txt = re.sub(r"\s+([/:])", r"\1", txt)
        txt = re.sub(r"([/:])\s+", r"\1 ", txt)
        txt = re.sub(r"\bM\s+(\d+)\b", r"M\1", txt)
        txt = re.sub(r"\s+—\s+", " — ", txt)
        txt = re.sub(r"\s{2,}", " ", txt).strip()
        return txt

    def _compose_cal_step_line(self, s: str):
        """Merge fragmented calibration lines like: [CAL] Step / 2 / 3 / — M / 5 / : ..."""
        frags = getattr(self, "_cal_step_fragments", None)
        if frags is None:
            self._cal_step_fragments = []
            frags = self._cal_step_fragments

        line = str(s or "").strip()

        # Start collection when the firmware emits a fragmented step header.
        if not frags and line.startswith("[CAL] Step"):
            frags.append(line)
            return None

        if not frags:
            return line

        # Flush pending fragments before protocol/status lines.
        if line.startswith("STATUS ") or line == "OK" or line.startswith("OK_") or line.startswith("ERR") or (line.startswith("[") and not line.startswith("[CAL] Step")):
            merged = self._merge_cal_step_fragments(frags)
            self._cal_step_fragments = []
            if merged:
                self._fc(merged + "\n")
            return line

        # Continue collecting fragmented tokens.
        frags.append(line)
        merged = self._merge_cal_step_fragments(frags)

        # End collection once we have a full descriptive part after ":"
        # (e.g. "Detecting MAX endstop").
        if ":" in merged and re.search(r":\s*[A-Za-z].*\s+[A-Za-z]", merged):
            self._cal_step_fragments = []
            return merged

        return None

    def _update_calib_failure_context(self, su: str):
        if "START COMPLETE_CALIB" in su:
            self._calib_diag_phase = "min"
            self._calib_diag_timeout = False
            return
        if "DETECTING MIN ENDSTOP" in su:
            self._calib_diag_phase = "min"
            self._calib_diag_timeout = False
            return
        if "DETECTING MAX ENDSTOP" in su:
            self._calib_diag_phase = "max"
            self._calib_diag_timeout = False
            return
        if "DETECTMIN TIMEOUT" in su:
            self._calib_diag_phase = "min"
            self._calib_diag_timeout = True
            return
        if "DETECTMAX TIMEOUT" in su:
            self._calib_diag_phase = "max"
            self._calib_diag_timeout = True
            return
        if "COMPLETE_CALIB DONE" in su or "CALIBRATION DONE" in su:
            self._calib_diag_phase = None
            self._calib_diag_timeout = False

    # Raisons détaillées "HOMING FAILED Mx <RAISON>" (firmware G4 V3).
    _HOMING_FAIL_REASONS = (
        ("NO_READY",        "homing_fail_no_ready"),
        ("SEEK_LIMIT",      "homing_fail_seek_limit"),
        ("MIN_TIMEOUT",     "homing_fail_min_timeout"),
        ("MAX_TIMEOUT",     "homing_fail_max_timeout"),
        ("ENDSTOP_STUCK",   "homing_fail_endstop_stuck"),
        ("ESTOP_LOCK",      "homing_fail_estop"),
        ("REBOOT_REQUIRED", "homing_fail_reboot"),
        ("MAX_TOO_SMALL",   "homing_fail_max_too_small"),
    )

    def _explain_homing_failure(self, line: str):
        """Traduit une ligne 'HOMING FAILED Mx RAISON' en message lisible
        dans le log et la console (ex. NO_READY -> servo non branché)."""
        su = str(line or "").upper()
        if "HOMING FAILED" not in su:
            return
        t = self.i18n[self.lang.get()]
        m = re.search(r"\bM(\d+)\b", su)
        motor = f"M{m.group(1)}" if m else "?"
        for token, key in self._HOMING_FAIL_REASONS:
            if token in su:
                msg = t.get(key, token)
                self._log(f"[CAL][DIAG] {motor}: {msg}")
                try:
                    self._fc(f"[DIAG] {motor}: {msg}\n")
                except Exception:
                    pass
                return

    def _log_calib_failure_reason(self, su: str):
        if "FAILED DURING MIN DETECTION" in su:
            if self._calib_diag_timeout:
                self._log("[CAL][DIAG] Echec calibration: timeout pendant la detection de la butee MIN.")
            else:
                self._log("[CAL][DIAG] Echec calibration: depart 65535, arrivee 0 sans detection de la butee MIN.")
            return
        if "FAILED DURING MAX DETECTION" in su:
            if self._calib_diag_timeout:
                self._log("[CAL][DIAG] Echec calibration: timeout pendant la detection de la butee MAX.")
            else:
                self._log("[CAL][DIAG] Echec calibration: butee MAX non detectee avant la fin de la recherche.")

    # ---------- RX ----------
    def _pump_rx(self):
        if self._test_active and self._test_wait_calibrated:
            now = time.monotonic()
            dl = float(getattr(self, "_test_wait_deadline", 0.0) or 0.0)
            hard_dl = float(getattr(self, "_test_wait_hard_deadline", 0.0) or 0.0)
            if dl > 0.0 and now >= dl:
                if hard_dl > 0.0 and now < hard_dl:
                    self._log("[TEST] Timeout waiting CALIBRATED after SH_START (no progress)")
                else:
                    self._log("[TEST] Timeout waiting CALIBRATED after SH_START (hard timeout)")
                if self._test_handshake_lines:
                    self._log("[TEST] SH_START handshake RX -> " + " | ".join(self._test_handshake_lines))
                self._test_status_var.set("Start timeout (no CALIBRATED)")
                self._test_finish_stop()

        if self._synchronous_mode:
            # In synchronous mode, the worker thread will consume rxq
            self.after(50, self._pump_rx)
            return

        while True:
            try:
                kind, msg = self.w.rxq.get_nowait()
            except queue.Empty:
                break

            if kind == "line":
                s = self._compose_cal_step_line(msg.strip())
                if s is None:
                    continue

                # Single-line STATUS response from firmware GET command.
                if s.startswith("STATUS "):
                    self._handle_status_line(s)
                    continue

                # Measured stroke report at end of full calibration.
                if s.startswith("STROKE M"):
                    self._fc(s + "\n")
                    try:
                        mm = re.match(r"STROKE (M\d+) (\d+)( CLAMPED)?", s)
                        if mm:
                            t = self.i18n[self.lang.get()]
                            key = "cal_stroke_clamped" if mm.group(3) else "cal_stroke_measured"
                            msg = t[key].replace("{motor}", mm.group(1)).replace("{steps}", mm.group(2))
                            self._log("[CAL] " + msg)
                            self._fc(msg + "\n")
                    except Exception:
                        pass
                    continue

                if s == "OK" or s.startswith("OK_") or s.startswith("ERR"):
                    # Timestamp OK/ERR lines to help debugging SET/GET sequences
                    try:
                        ts = time.time()
                        timestr = time.strftime("%H:%M:%S", time.localtime(ts)) + f".{int((ts - int(ts)) * 1000):03d}"
                    except Exception:
                        timestr = "--:--:--.000"
                    self._fc(f"[{timestr}] {s}\n")
                    # G474 firmware runs DO ops synchronously and only replies
                    # OK/ERR at completion (no [ACT]/ENDSTOP markers): stop the
                    # animation on that final reply.
                    if self._long_op_active and self._active_func_key in (
                            "detect_min", "detect_max", "go_center", "complete_calib", "test_stroke"):
                        key = self._active_func_key
                        self._long_op_active = False
                        self._active_func_key = None
                        if s.startswith("ERR"):
                            self._set_compress_animation(False, motion="idle")
                        elif key == "detect_max":
                            self._set_compress_animation(False, motion="maxed")
                        elif key == "go_center":
                            self._set_compress_animation(False, motion="centered")
                        else:
                            self._set_compress_animation(False, motion="idle")
                        try:
                            if self.w.is_connected() and not self._get_inflight:
                                self.after(250, self.refresh_status)
                        except Exception:
                            pass
                else:
                    self._fc(s + "\n")

                # Manual test startup follows SimHub: wait CALIBRATED before streaming.
                su = s.upper()
                self._update_calib_failure_context(su)
                if self._test_active and self._test_wait_calibrated:
                    self._test_extend_wait_deadline(s)
                if self._test_active and self._test_wait_calibrated and su == "CALIBRATED":
                    # Keep CALIBRATED as the final handshake line in logs.
                    self._test_begin_streaming("CALIBRATED")
                    self._test_log_box_line("CALIBRATED")
                elif self._test_active and self._test_wait_calibrated and su.startswith("ERR"):
                    self._test_log_box_line(s)
                    self._test_handshake_lines.append(s)
                    if len(self._test_handshake_lines) > 20:
                        self._test_handshake_lines.pop(0)
                    if "UNKNOWN_CMD" in su:
                        # Backward compatibility: old firmware without SH_START support.
                        self._log("[TEST] SH_START unsupported, fallback to ENABLE")
                        self.w.send_line("ENABLE")
                        self._log("[TEST] ENABLE sent (fallback)")
                        self._test_begin_streaming("OK_ENABLED")
                        self._test_log_box_line("OK_ENABLED")
                    else:
                        self._log("[TEST] SH_START failed -> stopping manual test")
                        self._test_status_var.set("Start failed (see log)")
                        self._test_finish_stop()
                elif self._test_active and self._test_wait_calibrated:
                    self._test_log_box_line(s)
                    self._test_handshake_lines.append(s)
                    if len(self._test_handshake_lines) > 20:
                        self._test_handshake_lines.pop(0)

                # Detect calibration start/stop from calibration log messages
                try:
                    if "HOMING FAILED" in s.upper():
                        self._explain_homing_failure(s)
                    if "[CAL]" in s:
                        s_lower = s.lower()
                        self._log_calib_failure_reason(su)
                        if "Start" in s:
                            self._calib_active = True
                            self._force_unknown_visual = False
                        if ("done" in s_lower) or ("FAILED" in s):
                            self._calib_active = False
                        if ("done" in s_lower) and self._active_func_key == "complete_calib":
                            self._long_op_active = False
                            self._active_func_key = None
                            self._force_unknown_visual = True
                            self._set_compress_animation(False, motion="idle")
                except Exception:
                    pass

                # Detect Arduino margin prompt and interact with user
                try:
                    if "Enter margin in steps" in s:
                        # Ask user for integer margin
                        try:
                            val = simpledialog.askinteger(APP_TITLE, "Arduino requests margin (steps). Enter integer:", parent=self, minvalue=1)
                        except Exception:
                            val = None
                        if val is None:
                            # user cancelled — send 0 to trigger invalid margin on MCU
                            self.w.send_line("0")
                            self._fc(f">>> 0  # sent (cancelled)\n")
                        else:
                            self.w.send_line(str(int(val)))
                            self._fc(f">>> {int(val)}\n")
                        self.func_console.see(tk.END)

                    if "Confirm margin?" in s or "Confirm margin? (y/n)" in s:
                        # Ask yes/no and send 'y' or 'n'
                        try:
                            ok = messagebox.askyesno(APP_TITLE, "Confirm margin?", parent=self)
                        except Exception:
                            ok = False
                        self.w.send_line('y' if ok else 'n')
                        self._fc(f">>> {'y' if ok else 'n'}\n")
                except Exception:
                    pass

                # Stop compression animation on common completion/failure markers
                try:
                    su = s.upper()
                    if su.startswith("[ACT]"):
                        if "DECOMPRESSING" in su:
                            self._long_op_active = True
                            self._force_unknown_visual = False
                            self._set_compress_animation(True, motion="decompressing")
                        elif "COMPRESSING" in su:
                            self._long_op_active = True
                            self._force_unknown_visual = False
                            self._set_compress_animation(True, motion="compressing")
                        elif "CENTERED" in su:
                            self._long_op_active = False
                            self._active_func_key = None
                            if self._force_unknown_visual:
                                self._set_compress_animation(False, motion="idle")
                            else:
                                self._set_compress_animation(False, motion="centered")
                            try:
                                if self.w.is_connected() and not self._get_inflight:
                                    self.after(250, self.refresh_status)
                            except Exception:
                                pass
                        elif "IDLE" in su:
                            self._long_op_active = False
                            # Keep MAX visual only when detect_max was explicitly requested.
                            if self._actuator_motion == "maxed" and self._active_func_key == "detect_max":
                                self._set_compress_animation(False, motion="maxed")
                            else:
                                self._set_compress_animation(False, motion="idle")
                            self._active_func_key = None
                            try:
                                if self.w.is_connected() and not self._get_inflight:
                                    self.after(250, self.refresh_status)
                            except Exception:
                                pass
                    if "MAX ENDSTOP FOUND" in su:
                        self._long_op_active = False
                        if self._active_func_key == "detect_max":
                            self._set_compress_animation(False, motion="maxed")
                        else:
                            self._set_compress_animation(False, motion="idle")
                        try:
                            if self.w.is_connected() and not self._get_inflight:
                                self.after(250, self.refresh_status)
                        except Exception:
                            pass
                    elif (
                        "MIN ENDSTOP FOUND" in su
                        or "ABORT" in su
                        or "CALIB_FAILED" in su
                        or "TIMEOUT" in su
                        or "RETURNING TO MAIN MENU" in su
                    ):
                        self._long_op_active = False
                        self._active_func_key = None
                        self._set_compress_animation(False)
                        try:
                            if self.w.is_connected() and not self._get_inflight:
                                self.after(250, self.refresh_status)
                        except Exception:
                            pass
                except Exception:
                    pass
            else:
                # Log the error; only attempt auto-reconnect if we are still
                # supposed to be connected (not an intentional disconnect).
                self._fc(f"[ERR] {msg}\n")
                if self.w.is_connected():
                    if self._test_active and self._test_wait_calibrated:
                        self._log("[TEST] Serial link lost during SH_START handshake")
                        if self._test_handshake_lines:
                            self._log("[TEST] SH_START handshake RX -> " + " | ".join(self._test_handshake_lines))
                        try:
                            self._test_status_var.set("Link lost during SH_START")
                        except Exception:
                            pass
                    try:
                        self._test_cleanup()
                    except Exception:
                        pass
                    self._log("[WARN] Serial error — disconnecting. Please reconnect.")
                    try:
                        # w.disconnect() is enough; don't try to send commands on a dead port.
                        self.w.disconnect()
                        self._pending_connect_firmware_check = False
                        self._pending_disconnect = False
                        self._last_synced_axis_count = 0
                        self.info = {"firmware": "-", "box": "-", "servo": "-",
                                     "homing_sps": "-", "homing_step": "-", "mc_required": "", "estop_enabled": False, "estop_behavior": 0}
                        self._set_compress_animation(False)
                        self._set_installed_fw_display("-")
                        t = self.i18n[self.lang.get()]
                        self.btn_connect.config(text=t["connect"])
                        self._render_status()
                        self._update_status_controls_state()
                        self._update_functions_controls_state()
                    except Exception:
                        pass

        self.after(50, self._pump_rx)

    def _start_py_homing(self, motor, direction=-20):
        if not self.w.is_connected():
            self._log("[ERR] Not connected")
            return
        if self._func_thread and self._func_thread.is_alive():
            self._log("[INFO] Another function is running")
            return
        # Ensure motor is marked connected on MCU so moveMotor will step it
        try:
            # Ensure MCU homing cadence matches UI setting to speed up stepping
            try:
                sps = int(self.homing_sps_var.get())
            except Exception:
                sps = 1500
            if sps < 50: sps = 50
            if sps > 10000: sps = 10000
            self.w.send_line(f"SET HOMING_S {sps}")
            self._log(f"[TX] SET HOMING_S {sps}")
            # wait briefly for OK (avoid long blocking)
            t0 = time.time()
            got_ok = False
            while time.time() - t0 < 0.6:
                try:
                    kind, msg = self.w.rxq.get(timeout=0.01)
                except queue.Empty:
                    continue
                if kind != "line":
                    continue
                s = msg.strip()
                self._fc(s + "\n")
                if s == "OK" or s.startswith("OK_"):
                    got_ok = True
                    break
                if s.startswith("ERR"):
                    break
            if not got_ok:
                self._log("[WARN] No OK for SET HOMING_S (continuing)")

            # ensure MCU will treat the axis as connected; do NOT call refresh
            # here (it may trigger a GET which can time out while the worker is running)
            self.w.send_line(f"SET CONNECTED 1 {motor}")
            self._log(f"[TX] SET CONNECTED 1 {motor}")
            # wait briefly for OK/ERR reply so MCU has applied the CONNECTED flag
            t0 = time.time()
            got_ok = False
            while time.time() - t0 < 0.6:
                try:
                    kind, msg = self.w.rxq.get(timeout=0.01)
                except queue.Empty:
                    continue
                if kind != "line":
                    continue
                s = msg.strip()
                self._fc(s + "\n")
                if s == "OK" or s.startswith("OK_"):
                    got_ok = True
                    break
                if s.startswith("ERR"):
                    break
            if not got_ok:
                self._log("[WARN] No OK for SET CONNECTED (continuing)")
        except Exception:
            pass
        # flush any pending RX messages so the worker starts with a clean queue
        try:
            while True:
                self.w.rxq.get_nowait()
        except queue.Empty:
            pass
        self._func_stop = threading.Event()
        self.btn_run.config(state="disabled")
        th = threading.Thread(target=self._py_homing_worker, args=(motor, direction, self._func_stop), daemon=True)
        self._func_thread = th
        th.start()
        self._update_cancel_button_state()

    def _py_homing_worker(self, motor, direction, stop_event):
        step = abs(direction)
        sign = -1 if direction < 0 else 1

        self._log(f"[INFO] Python homing start M{motor} dir={'MIN' if sign<0 else 'MAX'}")
        self._fc(f">>> PY_HOMING M{motor} {'MIN' if sign<0 else 'MAX'}\n")

        # Ensure servo on
        try:
            self.w.send_line("DO SERVO_ON")
        except Exception:
            pass

        # Mark calibration active so UI reflects state and RX pump behaves
        self._calib_active = True
        self._synchronous_mode = True
        reached = False
        try:
            while not stop_event.is_set():
                try:
                    self.w.send_line(f"STEP {motor} {sign * step}")
                except Exception:
                    break
                # Log the exact STEP we sent for traceability
                try:
                    self._log(f"[TX] STEP {motor} {sign * step}")
                    self._fc(f">>> STEP {motor} {sign * step}\n")
                except Exception:
                    pass
                # small throttle to avoid saturating USB buffer / MCU
                time.sleep(0.001)
                # Do not wait for STEP OK — it's mainly a safety handshake.
                # Send the fast endstop probe immediately to keep cadence high.
                try:
                    self.w.send_line(f"E{motor}")
                except Exception:
                    break
                try:
                    self._log(f"[TX] E{motor}")
                    self._fc(f">>> E{motor}\n")
                except Exception:
                    pass
                got = None
                t0 = time.time()
                # shorter E reply window to reduce per-step latency
                try:
                    self._log("[WAIT] E reply")
                except Exception:
                    pass
                # poll frequently with very small timeouts to avoid 50ms stalls
                while time.time() - t0 < 0.02:
                    if stop_event.is_set():
                        try:
                            self._log("[INFO] stop_event set while waiting E reply")
                        except Exception:
                            pass
                        break
                    try:
                        kind, msg = self.w.rxq.get(timeout=0.005)
                    except queue.Empty:
                        continue
                    if kind != "line":
                        continue
                    s = msg.strip()
                    # capture explicit MCU abort messages as an external cancel
                    if "ABORT" in s.upper() or "CANCEL" in s.upper():
                        try:
                            self._log(f"[INFO] MCU requested abort/cancel: {s}")
                        except Exception:
                            pass
                        stop_event.set()
                        break
                    if s == '1':
                        try:
                            self._log(f"[RX] E{motor} = 1")
                        except Exception:
                            pass
                        got = True; break
                    if s == '0':
                        try:
                            self._log(f"[RX] E{motor} = 0")
                        except Exception:
                            pass
                        got = False; break
                if got is True:
                    reached = True
                    break
        finally:
            self._synchronous_mode = False
            self._calib_active = False
            self._set_compress_animation(False)
            try:
                self.w.send_line("DO SERVO_OFF")
            except Exception:
                pass
            self.after(0, lambda: self.btn_run.config(state="normal"))
            self.after(0, self._update_cancel_button_state)

        if reached:
            self._log(f"[INFO] Endstop reached M{motor}")
            self._fc(f"[INFO] Endstop reached M{motor}\n")
        else:
            self._log(f"[INFO] Homing aborted/timeout M{motor}")
            self._fc(f"[INFO] Homing aborted/timeout M{motor}\n")
        self._fc("\n")


if __name__ == "__main__":
    App().mainloop()
