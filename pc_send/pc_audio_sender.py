#!/usr/bin/env python3
import sys
import os
import socket
import subprocess
import time
import math
import struct
import argparse
import shutil
import threading
import platform
import atexit

loaded_modules = []

def cleanup_modules():
    for mod in list(loaded_modules):
        try:
            print(f"Limpieza atexit: Eliminando dispositivo virtual {mod}...")
            subprocess.run(["pactl", "unload-module", mod], capture_output=True)
        except Exception:
            pass
        try:
            loaded_modules.remove(mod)
        except ValueError:
            pass

atexit.register(cleanup_modules)

DEFAULT_IP = os.environ.get("CDPLAYER_IP", "raspberrypi.local")
DEFAULT_PORT = int(os.environ.get("CDPLAYER_PORT", "3000"))

try:
    from PyQt6.QtWidgets import (
        QApplication, QMainWindow, QWidget, QVBoxLayout, QHBoxLayout,
        QLabel, QPushButton, QLineEdit, QProgressBar, QFrame, QSizePolicy,
        QSlider, QSpinBox, QComboBox
    )
    from PyQt6.QtCore import QTimer, Qt, QThread, pyqtSignal, QMetaObject
    from PyQt6.QtGui import QFont, QColor, QPalette
    HAS_GUI = True
except ImportError:
    HAS_GUI = False

# =====================================================================
# CORE AUDIO SENDER LOGIC
# =====================================================================
class AudioSender:
    def __init__(self, ip, port, latency=20, device_name=None, low_latency=False, mode="lossless"):
        self.ip = ip
        self.port = port
        self.latency = latency
        self.device_name = device_name
        self.mode = mode if isinstance(mode, str) else ("ultra_low" if low_latency else "lossless")
        self.proc = None
        self.sock = None
        self.module_id = None
        self.bytes_sent = 0
        self.start_time = 0
        self.is_running = False
        self.peak_db = -100.0
        self.peak_linear = 0.0
        self.latency_ms = -1.0

    def start(self):
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.bytes_sent = 0
        self.start_time = time.time()
        self.is_running = True
        self.latency_ms = -1.0
        
        # Start ping/pong threads for active network latency measurement
        threading.Thread(target=self._ping_sender_loop, daemon=True).start()
        threading.Thread(target=self._pong_receiver_loop, daemon=True).start()

        if platform.system() == 'Windows':
            # Start background Windows WASAPI loopback streaming loop
            threading.Thread(target=self._stream_loop_windows, daemon=True).start()
        else:
            # Unload any previously orphaned rpi_cdplayer modules
            try:
                out = subprocess.run(["pactl", "list", "modules", "short"], capture_output=True, text=True).stdout
                for line in out.splitlines():
                    if "module-null-sink" in line and "rpi_cdplayer" in line:
                        mod_idx = line.split()[0]
                        subprocess.run(["pactl", "unload-module", mod_idx], capture_output=True)
            except Exception:
                pass

            # Save current default sink so we can restore it on stop
            try:
                self.prev_sink = subprocess.run(["pactl", "get-default-sink"], capture_output=True, text=True).stdout.strip()
            except Exception:
                self.prev_sink = None

            # 1. Create the virtual sound device on Linux
            print("Creando dispositivo de sonido virtual...")
            try:
                res = subprocess.run([
                    "pactl", "load-module", "module-null-sink", 
                    "sink_name=rpi_cdplayer", 
                    "sink_properties=device.description=RPi-CD-Player-Audio-Compartido",
                    "format=s32le",
                    "rate=48000",
                    "channels=2"
                ], capture_output=True, text=True, check=True)
                self.module_id = res.stdout.strip()
                loaded_modules.append(self.module_id)
                print(f"Dispositivo virtual creado (Module ID: {self.module_id})")

                # Set rpi_cdplayer as default system audio output
                subprocess.run(["pactl", "set-default-sink", "rpi_cdplayer"], capture_output=True)
                print("Audio del PC redirigido automáticamente a RPi-CD-Player-Audio-Compartido.")
            except Exception as e:
                print(f"Advertencia: No se pudo crear el dispositivo virtual con pactl: {e}")
                self.module_id = None

            # Capture from the virtual device if successfully created, else default monitor
            capture_device = "rpi_cdplayer.monitor" if self.module_id else "@DEFAULT_MONITOR@"
            
            # 3. Spawn GStreamer RTP sender process
            cmd = [
                "gst-launch-1.0", "-q",
                "pulsesrc", f"device={capture_device}",
                "!", "audio/x-raw,format=S24BE,rate=48000,channels=2",
                "!", "rtpL24pay",
                "!", "udpsink", f"host={self.ip}", f"port={self.port}"
            ]
            
            try:
                self.proc = subprocess.Popen(cmd)
                print(f"Emisor GStreamer RTP iniciado -> {self.ip}:{self.port}")
            except Exception as e:
                self.stop()
                raise e

            # Start background level tap & monitoring loop
            threading.Thread(target=self._stream_loop_linux, daemon=True).start()

    def _ping_sender_loop(self):
        while self.is_running and self.sock:
            try:
                pkt = b"PING" + struct.pack("d", time.time())
                self.sock.sendto(pkt, (self.ip, self.port + 1))
            except Exception:
                pass
            time.sleep(1.0)

    def _pong_receiver_loop(self):
        if not self.sock:
            return
        self.sock.settimeout(0.5)
        while self.is_running:
            try:
                data, addr = self.sock.recvfrom(1024)
                if data.startswith(b"PONG") and len(data) >= 12:
                    sent_time = struct.unpack("d", data[4:12])[0]
                    rtt = (time.time() - sent_time) * 1000.0
                    self.latency_ms = rtt
            except socket.timeout:
                continue
            except Exception:
                break

    def _stream_loop_linux(self):
        # VU meter level tap reading from parec
        capture_device = "rpi_cdplayer.monitor" if self.module_id else "@DEFAULT_MONITOR@"
        cmd = [
            "parec",
            "-d", capture_device,
            "--format=s32le",
            "--rate=48000",
            "--channels=2",
            "--latency-msec=40"
        ]
        tap_proc = None
        try:
            tap_proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
            fd = tap_proc.stdout.fileno()
            while self.is_running and self.proc and self.proc.poll() is None:
                chunk = os.read(fd, 7680)
                if not chunk:
                    break
                self.bytes_sent += len(chunk)
                self._calculate_levels(chunk)
        except Exception:
            pass
        finally:
            if tap_proc:
                try: tap_proc.terminate()
                except Exception: pass
            self.peak_db = -100.0
            self.peak_linear = 0.0

    def _stream_loop_windows(self):
        try:
            import soundcard as sc
            import numpy as np
        except ImportError:
            print("Error: Se necesitan 'soundcard' y 'numpy' para capturar audio en Windows.", file=sys.stderr)
            self.is_running = False
            return

        try:
            target_spk = None
            if self.device_name:
                for spk in sc.all_speakers():
                    if spk.name == self.device_name or self.device_name in spk.name:
                        target_spk = spk
                        break
            if not target_spk:
                target_spk = sc.default_speaker()

            if not target_spk:
                print("Error: No se detectó altavoz o dispositivo de salida en Windows.", file=sys.stderr)
                self.is_running = False
                return

            loopback = sc.get_microphone(id=str(target_spk.name), include_loopback=True)
            print(f"Windows Loopback activo en dispositivo: {target_spk.name} [Modo: {self.mode}]")
        except Exception as e:
            print(f"Error accediendo al loopback de Windows: {e}", file=sys.stderr)
            self.is_running = False
            return

        if self.mode == "ultra_low":
            samples_per_packet = 240  # 5ms por paquete
        elif self.mode == "low":
            samples_per_packet = 480  # 10ms por paquete
        else:
            samples_per_packet = 960  # 20ms por paquete (Óptimo para Lossless Bit-Perfect sin jitter)

        payload_type = 96
        seq = 0
        timestamp = 0
        ssrc = 0x12345678

        try:
            with loopback.recorder(samplerate=48000, channels=2, blocksize=samples_per_packet) as recorder:
                while self.is_running:
                    # Obtener inmediatamente los datos disponibles sin esperar acumulación interna
                    data = recorder.record(numframes=None)
                    if data is None or len(data) == 0:
                        continue

                    # Calcular nivel VU
                    peak = float(np.max(np.abs(data)))
                    self.peak_linear = min(1.0, max(0.0, peak))
                    if self.peak_linear > 0:
                        self.peak_db = 20.0 * math.log10(self.peak_linear)
                    else:
                        self.peak_db = -100.0

                    # Convertir a PCM 24-bit Big-Endian (L24 Bit-Perfect)
                    clipped = np.clip(data, -1.0, 1.0)
                    int32_samples = (clipped * 2147483647.0).astype(np.int32)
                    s24be = np.ascontiguousarray(int32_samples).astype('>i4').view(np.uint8).reshape(-1, 4)[:, :3].tobytes()

                    # Fragmentar en paquetes RTP de tamaño óptimo para la red
                    bytes_per_pkt = samples_per_packet * 2 * 3
                    total_len = len(s24be)
                    
                    for offset in range(0, total_len, bytes_per_pkt):
                        chunk = s24be[offset:offset + bytes_per_pkt]
                        chunk_samples = len(chunk) // (2 * 3)
                        
                        rtp_hdr = struct.pack("!BBHII", 0x80, payload_type & 0x7F, seq & 0xFFFF, timestamp & 0xFFFFFFFF, ssrc)
                        packet = rtp_hdr + chunk

                        if self.sock:
                            self.sock.sendto(packet, (self.ip, self.port))
                        self.bytes_sent += len(packet)

                        seq = (seq + 1) & 0xFFFF
                        timestamp = (timestamp + chunk_samples) & 0xFFFFFFFF
        except Exception as e:
            print(f"Error en bucle de streaming Windows: {e}", file=sys.stderr)
        finally:
            self.peak_db = -100.0
            self.peak_linear = 0.0

    def _calculate_levels(self, pcm_data):
        # pcm_data is S32_LE (4 bytes per sample) - used on Linux
        sample_count = len(pcm_data) // 4
        if sample_count == 0:
            return
        
        try:
            samples = struct.unpack(f"<{sample_count}i", pcm_data[:sample_count * 4])
            peak = max(abs(s) for s in samples)
            self.peak_linear = peak / 2147483648.0
            if self.peak_linear == 0:
                self.peak_db = -100.0
            else:
                self.peak_db = 20 * math.log10(self.peak_linear)
        except Exception:
            pass

    def stop(self):
        self.is_running = False
        
        # Terminate parec process
        if self.proc:
            self.proc.terminate()
            try:
                self.proc.wait(timeout=1.0)
            except Exception:
                try: self.proc.kill()
                except Exception: pass
            self.proc = None

        # Close socket
        if self.sock:
            self.sock.close()
            self.sock = None

        # Restore previous default sink if set
        if hasattr(self, 'prev_sink') and self.prev_sink:
            try:
                subprocess.run(["pactl", "set-default-sink", self.prev_sink], capture_output=True)
                print(f"Salida por defecto restaurada a: {self.prev_sink}")
            except Exception:
                pass

        # Remove virtual sound device
        if self.module_id:
            print(f"Eliminando dispositivo de sonido virtual (Module ID: {self.module_id})...")
            subprocess.run(["pactl", "unload-module", self.module_id], capture_output=True)
            try:
                loaded_modules.remove(self.module_id)
            except ValueError:
                pass
            self.module_id = None
            print("Dispositivo virtual eliminado.")


# =====================================================================
# PYQT6 GUI IMPLEMENTATION
# =====================================================================
if HAS_GUI:
    class MainWindow(QMainWindow):
        status_received = pyqtSignal(dict)
        set_btn_enabled = pyqtSignal(bool)
        reset_ui_signal = pyqtSignal()

        def __init__(self, default_ip, default_port):
            super().__init__()
            self.is_closed = False
            self.default_ip = default_ip
            self.default_port = default_port
            self.sender = None
            self.last_status_poll = 0
            self.init_ui()
            self.status_received.connect(self.handle_status_update)
            self.set_btn_enabled.connect(self.btn_toggle.setEnabled)
            self.reset_ui_signal.connect(self.reset_ui_after_fail)
            
        def init_ui(self):
            self.setWindowTitle("Hi-Fi RPi Audio Compartido")
            self.setMinimumSize(420, 580)
            
            # Dark styling matching the Web App
            self.setStyleSheet("""
                QMainWindow {
                    background-color: #0d0d11;
                }
                QWidget {
                    color: #e2e8f0;
                    font-family: 'Outfit', 'Segoe UI', sans-serif;
                }
                QLabel {
                    font-size: 13px;
                }
                QLineEdit {
                    background-color: #1e1e24;
                    border: 1px solid #2d2d39;
                    border-radius: 8px;
                    padding: 8px 12px;
                    color: #ffffff;
                    font-size: 14px;
                }
                QLineEdit:focus {
                    border: 1px solid #3b82f6;
                }
                QPushButton {
                    background-color: #3b82f6;
                    border: none;
                    border-radius: 10px;
                    padding: 12px;
                    color: white;
                    font-weight: bold;
                    font-size: 14px;
                }
                QPushButton:hover {
                    background-color: #2563eb;
                }
                QPushButton:pressed {
                    background-color: #1d4ed8;
                }
                QPushButton#btn-toggle.active {
                    background-color: #10b981;
                }
                QPushButton#btn-toggle.active:hover {
                    background-color: #059669;
                }
                QProgressBar {
                    background-color: #1e1e24;
                    border: 1px solid #2d2d39;
                    border-radius: 6px;
                    text-align: center;
                    height: 12px;
                }
                QProgressBar::chunk {
                    background: qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 #3b82f6, stop:0.8 #8b5cf6, stop:1 #ef4444);
                    border-radius: 5px;
                }
                QFrame#card {
                    background-color: rgba(25, 25, 35, 0.6);
                    border: 1px solid rgba(255, 255, 255, 0.08);
                    border-radius: 16px;
                }
            """)
            
            # Main Layout
            central_widget = QWidget()
            self.setCentralWidget(central_widget)
            main_layout = QVBoxLayout(central_widget)
            main_layout.setContentsMargins(20, 20, 20, 20)
            main_layout.setSpacing(15)
            
            # Header
            header_label = QLabel("COMPARTIR AUDIO PC")
            header_label.setStyleSheet("font-size: 18px; font-weight: 800; letter-spacing: 1px; color: #3b82f6;")
            header_label.setAlignment(Qt.AlignmentFlag.AlignCenter)
            main_layout.addWidget(header_label)
            
            # Connection Card
            self.card = QFrame()
            self.card.setObjectName("card")
            card_layout = QVBoxLayout(self.card)
            card_layout.setContentsMargins(15, 15, 15, 15)
            card_layout.setSpacing(12)
            
            # IP Input field
            ip_layout = QHBoxLayout()
            ip_label = QLabel("IP Raspberry Pi:")
            self.ip_input = QLineEdit(self.default_ip)
            ip_layout.addWidget(ip_label)
            ip_layout.addWidget(self.ip_input)
            card_layout.addLayout(ip_layout)

            # Audio Output Device Selection (Windows)
            device_layout = QVBoxLayout()
            dev_header_layout = QHBoxLayout()
            dev_label = QLabel("Dispositivo de Salida (Captura):")
            self.btn_refresh_devs = QPushButton("🔄")
            self.btn_refresh_devs.setFixedWidth(32)
            self.btn_refresh_devs.setToolTip("Refrescar dispositivos de audio")
            self.btn_refresh_devs.setCursor(Qt.CursorShape.PointingHandCursor)
            self.btn_refresh_devs.setStyleSheet("padding: 4px; font-size: 12px;")
            self.btn_refresh_devs.clicked.connect(self.populate_devices)
            dev_header_layout.addWidget(dev_label)
            dev_header_layout.addWidget(self.btn_refresh_devs)
            device_layout.addLayout(dev_header_layout)

            self.combo_devices = QComboBox()
            self.combo_devices.setStyleSheet("""
                QComboBox {
                    background-color: #1e1e24;
                    border: 1px solid #2d2d39;
                    border-radius: 8px;
                    padding: 6px 10px;
                    color: #ffffff;
                }
                QComboBox::drop-down {
                    border: 0px;
                }
                QComboBox QAbstractItemView {
                    background-color: #1e1e24;
                    color: #ffffff;
                    selection-background-color: #3b82f6;
                }
            """)
            device_layout.addWidget(self.combo_devices)
            card_layout.addLayout(device_layout)
            self.populate_devices()

            # Latency Mode Selection
            lat_mode_layout = QHBoxLayout()
            lat_mode_label = QLabel("Modo de Latencia:")
            self.combo_latency = QComboBox()
            self.combo_latency.setStyleSheet("""
                QComboBox {
                    background-color: #1e1e24;
                    border: 1px solid #2d2d39;
                    border-radius: 8px;
                    padding: 6px 10px;
                    color: #ffffff;
                }
                QComboBox QAbstractItemView {
                    background-color: #1e1e24;
                    color: #ffffff;
                    selection-background-color: #3b82f6;
                }
            """)
            self.combo_latency.addItem("Lossless Audiophile (Sin Pausas - Búfer Grande)", "lossless")
            self.combo_latency.addItem("Baja Latencia Equilibrada (40ms - Vídeo/Música)", "low")
            self.combo_latency.addItem("Ultra Baja Latencia (5ms - Gaming/En Vivo)", "ultra_low")
            lat_mode_layout.addWidget(lat_mode_label)
            lat_mode_layout.addWidget(self.combo_latency)
            card_layout.addLayout(lat_mode_layout)

            # Latency Meter Display (replaces the slider)
            latency_layout = QHBoxLayout()
            latency_label = QLabel("Latencia de Red (RTT):")
            self.latency_value_lbl = QLabel("Inactivo")
            self.latency_value_lbl.setStyleSheet("color: #60a5fa; font-weight: bold;")
            latency_layout.addWidget(latency_label)
            latency_layout.addWidget(self.latency_value_lbl)
            card_layout.addLayout(latency_layout)

            # Speedtest button
            speed_layout = QHBoxLayout()
            self.btn_speedtest = QPushButton("Test de Velocidad")
            self.btn_speedtest.setCursor(Qt.CursorShape.PointingHandCursor)
            self.btn_speedtest.setStyleSheet("""
                QPushButton {
                    background-color: rgba(59, 130, 246, 0.15);
                    border: 1px solid rgba(59, 130, 246, 0.3);
                    color: #93c5fd;
                    padding: 8px;
                    border-radius: 8px;
                }
                QPushButton:hover {
                    background-color: rgba(59, 130, 246, 0.3);
                }
            """)
            self.btn_speedtest.clicked.connect(self.run_speedtest)
            self.speedtest_result = QLabel("")
            self.speedtest_result.setStyleSheet("color: #10b981; font-weight: bold;")
            speed_layout.addWidget(self.btn_speedtest)
            speed_layout.addWidget(self.speedtest_result)
            card_layout.addLayout(speed_layout)
            
            # Mode Control Buttons
            mode_layout = QHBoxLayout()
            mode_label = QLabel("Modo RPi:")
            self.btn_mode_cd = QPushButton("Modo CD")
            self.btn_mode_pc = QPushButton("Audio PC")
            self.btn_mode_cd.setCursor(Qt.CursorShape.PointingHandCursor)
            self.btn_mode_pc.setCursor(Qt.CursorShape.PointingHandCursor)
            
            mode_btn_style = """
                QPushButton {
                    background-color: rgba(255, 255, 255, 0.05);
                    border: 1px solid rgba(255, 255, 255, 0.1);
                    padding: 6px;
                    border-radius: 6px;
                    font-size: 12px;
                    color: #e2e8f0;
                }
                QPushButton:hover {
                    background-color: rgba(255, 255, 255, 0.15);
                }
            """
            self.btn_mode_cd.setStyleSheet(mode_btn_style)
            self.btn_mode_pc.setStyleSheet(mode_btn_style)
            
            self.btn_mode_cd.clicked.connect(lambda: self.set_rpi_mode("cd"))
            self.btn_mode_pc.clicked.connect(lambda: self.set_rpi_mode("pc"))
            
            mode_layout.addWidget(mode_label)
            mode_layout.addWidget(self.btn_mode_cd)
            mode_layout.addWidget(self.btn_mode_pc)
            card_layout.addLayout(mode_layout)

            # Device Info Label
            self.device_info = QLabel("Modo Bit-Perfect 24-bit / 48kHz RTP L24")
            self.device_info.setStyleSheet("color: #94a3b8; font-size: 11px;")
            self.device_info.setAlignment(Qt.AlignmentFlag.AlignCenter)
            card_layout.addWidget(self.device_info)
            
            main_layout.addWidget(self.card)
            
            # VU Meter Section
            vu_layout = QVBoxLayout()
            vu_label = QLabel("Nivel de Volumen (VU):")
            vu_label.setStyleSheet("font-weight: 600; font-size: 12px; color: #94a3b8;")
            self.vu_progress = QProgressBar()
            self.vu_progress.setRange(0, 100)
            self.vu_progress.setValue(0)
            self.vu_progress.setTextVisible(False)
            vu_layout.addWidget(vu_label)
            vu_layout.addWidget(self.vu_progress)
            main_layout.addLayout(vu_layout)
            
            # Stats Section
            self.stats_frame = QFrame()
            stats_layout = QVBoxLayout(self.stats_frame)
            stats_layout.setContentsMargins(0, 5, 0, 5)
            stats_layout.setSpacing(4)
            
            self.status_lbl = QLabel("Estado: Inactivo")
            self.status_lbl.setStyleSheet("font-weight: bold; color: #94a3b8;")
            self.time_lbl = QLabel("Tiempo: 00:00")
            self.bytes_lbl = QLabel("Datos enviados: 0.0 MB")
            self.rate_lbl = QLabel("Velocidad: 0.0 KB/s")
            
            stats_layout.addWidget(self.status_lbl)
            stats_layout.addWidget(self.time_lbl)
            stats_layout.addWidget(self.bytes_lbl)
            stats_layout.addWidget(self.rate_lbl)
            main_layout.addWidget(self.stats_frame)
            
            # Action Button
            self.btn_toggle = QPushButton("TRANSMITIR AUDIO")
            self.btn_toggle.setObjectName("btn-toggle")
            self.btn_toggle.setCursor(Qt.CursorShape.PointingHandCursor)
            self.btn_toggle.clicked.connect(self.toggle_streaming)
            main_layout.addWidget(self.btn_toggle)

            # CD Controls panel
            self.cd_group = QFrame()
            self.cd_group.setObjectName("card")
            cd_layout = QVBoxLayout(self.cd_group)
            cd_layout.setContentsMargins(10, 10, 10, 10)
            
            cd_title = QLabel("CONTROLES REPRODUCTOR CD")
            cd_title.setStyleSheet("font-size: 11px; font-weight: bold; letter-spacing: 0.5px; color: #3b82f6;")
            cd_title.setAlignment(Qt.AlignmentFlag.AlignCenter)
            cd_layout.addWidget(cd_title)
            
            btn_layout = QHBoxLayout()
            btn_prev = QPushButton("⏮")
            btn_play = QPushButton("▶/⏸")
            btn_stop = QPushButton("⏹")
            btn_next = QPushButton("⏭")
            btn_eject = QPushButton("⏏")
            
            for b in [btn_prev, btn_play, btn_stop, btn_next, btn_eject]:
                b.setCursor(Qt.CursorShape.PointingHandCursor)
                b.setStyleSheet("""
                    QPushButton {
                        background-color: rgba(255, 255, 255, 0.04);
                        border: 1px solid rgba(255, 255, 255, 0.08);
                        font-size: 16px;
                        padding: 8px;
                        border-radius: 8px;
                    }
                    QPushButton:hover {
                        background-color: rgba(255, 255, 255, 0.12);
                    }
                """)
                
            btn_prev.clicked.connect(lambda: self.trigger_cd_command("prev"))
            btn_play.clicked.connect(lambda: self.trigger_cd_command("play_pause"))
            btn_stop.clicked.connect(lambda: self.trigger_cd_command("stop"))
            btn_next.clicked.connect(lambda: self.trigger_cd_command("next"))
            btn_eject.clicked.connect(lambda: self.trigger_cd_command("eject"))
            
            btn_layout.addWidget(btn_prev)
            btn_layout.addWidget(btn_play)
            btn_layout.addWidget(btn_stop)
            btn_layout.addWidget(btn_next)
            btn_layout.addWidget(btn_eject)
            cd_layout.addLayout(btn_layout)
            
            main_layout.addWidget(self.cd_group)
            
            # Timer to update GUI stats (every 100ms)
            self.timer = QTimer()
            self.timer.timeout.connect(self.update_stats)
            self.timer.start(100)

        def populate_devices(self):
            self.combo_devices.clear()
            if platform.system() == 'Windows':
                try:
                    import soundcard as sc
                    speakers = sc.all_speakers()
                    default_spk = sc.default_speaker()
                    default_idx = 0
                    for i, spk in enumerate(speakers):
                        self.combo_devices.addItem(spk.name, spk.name)
                        if default_spk and spk.name == default_spk.name:
                            default_idx = i
                    if speakers:
                        self.combo_devices.setCurrentIndex(default_idx)
                except Exception as e:
                    self.combo_devices.addItem("Salida por defecto del sistema", None)
            else:
                self.combo_devices.addItem("Dispositivo Virtual PulseAudio / PipeWire", None)
            
        def reset_ui_after_fail(self):
            self.btn_toggle.setText("TRANSMITIR AUDIO")
            self.btn_toggle.setStyleSheet("")
            self.ip_input.setEnabled(True)
            self.combo_devices.setEnabled(True)
            self.combo_latency.setEnabled(True)
            self.status_lbl.setText("Estado: Falló al iniciar")
            self.vu_progress.setValue(0)
            self.latency_value_lbl.setText("Inactivo")

        def toggle_streaming(self):
            self.btn_toggle.setEnabled(False)
            
            if self.sender and self.sender.is_running:
                # Stop streaming
                self.btn_toggle.setText("TRANSMITIR AUDIO")
                self.btn_toggle.setStyleSheet("") # Clear active styles
                self.ip_input.setEnabled(True)
                self.combo_devices.setEnabled(True)
                self.combo_latency.setEnabled(True)
                self.status_lbl.setText("Estado: Deteniendo...")
                
                # Stop sender logic
                self.sender.stop()
                self.sender = None
                self.status_lbl.setText("Estado: Inactivo")
                self.vu_progress.setValue(0)
                self.latency_value_lbl.setText("Inactivo")
                
                # Tell RPi to disable PC audio receiver
                target_ip = self.ip_input.text().strip()
                def stop_task():
                    self.send_api_post(target_ip, "pc_audio/disable")
                    self.set_btn_enabled.emit(True)
                threading.Thread(target=stop_task, daemon=True).start()
            else:
                # Start streaming
                target_ip = self.ip_input.text().strip()
                if not target_ip:
                    self.btn_toggle.setEnabled(True)
                    return
                
                self.btn_toggle.setText("DETENER TRANSMISIÓN")
                self.btn_toggle.setStyleSheet("""
                    QPushButton#btn-toggle {
                        background-color: #10b981;
                    }
                    QPushButton#btn-toggle:hover {
                        background-color: #059669;
                    }
                """)
                self.ip_input.setEnabled(False)
                self.combo_devices.setEnabled(False)
                self.combo_latency.setEnabled(False)
                self.status_lbl.setText("Estado: Iniciando...")
                
                selected_dev = self.combo_devices.currentData()
                selected_mode = self.combo_latency.currentData() or "lossless"

                def start_task():
                    try:
                        self.send_api_post(target_ip, "pc_audio/enable", payload={"mode": selected_mode})
                        self.sender = AudioSender(target_ip, self.default_port, device_name=selected_dev, mode=selected_mode)
                        self.sender.start()
                        
                        if selected_mode == "ultra_low":
                            mode_desc = "Ultra Baja Latencia (5ms)"
                        elif selected_mode == "low":
                            mode_desc = "Baja Latencia (40ms)"
                        else:
                            mode_desc = "Lossless Audiophile (Búfer 2.5s - Cero Pausas)"
                            
                        self.status_lbl.setText(f"Estado: Transmitiendo - {mode_desc} (24-bit/48kHz Bit-Perfect)")
                    except Exception as e:
                        print(f"Error starting stream: {e}")
                        self.status_lbl.setText(f"Error: {e}")
                        if self.sender:
                            self.sender.stop()
                        self.sender = None
                        self.reset_ui_signal.emit()
                    finally:
                        self.set_btn_enabled.emit(True)
                
                threading.Thread(target=start_task, daemon=True).start()
                    
        def run_speedtest(self):
            target_ip = self.ip_input.text().strip()
            if not target_ip:
                return
            
            self.btn_speedtest.setEnabled(False)
            self.speedtest_result.setText("Probando...")
            
            def run():
                try:
                    import urllib.request
                    url = f"http://{target_ip}:8000/api/speedtest"
                    payload = b'\0' * 5242880 # 5MB
                    start_t = time.time()
                    req = urllib.request.Request(url, method="POST", data=payload)
                    req.add_header('Content-Type', 'application/octet-stream')
                    with urllib.request.urlopen(req, timeout=8.0) as resp:
                        resp.read()
                    duration = time.time() - start_t
                    speed_mbps = (len(payload) * 8) / (duration * 1024 * 1024)
                    self.speedtest_result.setText(f"{speed_mbps:.1f} Mbps")
                except Exception as e:
                    self.speedtest_result.setText("Error")
                    print(f"Speedtest error: {e}")
                finally:
                    self.btn_speedtest.setEnabled(True)
                    
            threading.Thread(target=run, daemon=True).start()

        def trigger_cd_command(self, cmd):
            target_ip = self.ip_input.text().strip()
            if not target_ip:
                return
            
            def run():
                if cmd == "play_pause":
                    status = self.get_api_status(target_ip)
                    if status:
                        state_val = status.get("state")
                        if state_val == "playing":
                            self.send_api_post(target_ip, "pause")
                        elif state_val == "paused":
                            self.send_api_post(target_ip, "resume")
                        else:
                            self.send_api_post(target_ip, "play", {"track": status.get("current_track", 1)})
                else:
                    self.send_api_post(target_ip, cmd)
                    
            threading.Thread(target=run, daemon=True).start()
            
        def get_api_status(self, ip):
            try:
                import urllib.request
                import json
                with urllib.request.urlopen(f"http://{ip}:8000/api/status", timeout=1.5) as req:
                    return json.loads(req.read().decode('utf-8'))
            except Exception:
                return None
                
        def send_api_post(self, ip, endpoint, payload=None, body=None):
            try:
                import urllib.request
                import json
                url = f"http://{ip}:8000/api/{endpoint}"
                req = urllib.request.Request(url, method="POST")
                req.add_header('Content-Type', 'application/json')
                data = json.dumps(payload if payload is not None else (body or {})).encode('utf-8')
                with urllib.request.urlopen(req, data=data, timeout=2.0) as resp:
                    resp.read()
            except Exception as e:
                print(f"Error calling API {endpoint}: {e}")
                    
        def set_rpi_mode(self, mode):
            target_ip = self.ip_input.text().strip()
            if not target_ip:
                return
            def run():
                if mode == "cd":
                    self.send_api_post(target_ip, "pc_audio/disable")
                elif mode == "pc":
                    self.send_api_post(target_ip, "pc_audio/enable")
                self.poll_rpi_status()
            threading.Thread(target=run, daemon=True).start()

        def poll_rpi_status(self):
            if self.is_closed:
                return
            target_ip = self.ip_input.text().strip()
            if not target_ip:
                return
            def run():
                status = self.get_api_status(target_ip)
                if status and not self.is_closed:
                    try:
                        self.status_received.emit(status)
                    except RuntimeError:
                        pass
            threading.Thread(target=run, daemon=True).start()

        def handle_status_update(self, status):
            is_pc = status.get("pc_audio_active", False) or status.get("state") == "pc_audio"
            is_cd = not is_pc
            active_style = """
                QPushButton {
                    background-color: #3b82f6;
                    border: 1px solid #3b82f6;
                    color: white;
                    padding: 6px;
                    border-radius: 6px;
                    font-size: 12px;
                    font-weight: bold;
                }
            """
            inactive_style = """
                QPushButton {
                    background-color: rgba(255, 255, 255, 0.05);
                    border: 1px solid rgba(255, 255, 255, 0.1);
                    padding: 6px;
                    border-radius: 6px;
                    font-size: 12px;
                    color: #e2e8f0;
                }
                QPushButton:hover {
                    background-color: rgba(255, 255, 255, 0.15);
                }
            """
            self.btn_mode_cd.setStyleSheet(active_style if is_cd else inactive_style)
            self.btn_mode_pc.setStyleSheet(active_style if is_pc else inactive_style)

        def update_stats(self):
            now = time.time()
            if now - self.last_status_poll >= 2.0:
                self.last_status_poll = now
                self.poll_rpi_status()

            if self.sender and self.sender.is_running:
                # Update stats labels
                elapsed = time.time() - self.sender.start_time
                mins = int(elapsed // 60)
                secs = int(elapsed % 60)
                self.time_lbl.setText(f"Tiempo: {mins:02d}:{secs:02d}")
                
                mb = self.sender.bytes_sent / (1024 * 1024)
                self.bytes_lbl.setText(f"Datos enviados: {mb:.1f} MB")
                
                rate = (self.sender.bytes_sent / elapsed) / 1024 if elapsed > 0 else 0
                self.rate_lbl.setText(f"Velocidad: {rate:.1f} KB/s")
                
                # Update real-time latency
                if self.sender.latency_ms >= 0:
                    rtt = self.sender.latency_ms
                    one_way = rtt / 2.0
                    self.latency_value_lbl.setText(f"{rtt:.1f} ms (Est. Red: {one_way:.1f} ms)")
                else:
                    self.latency_value_lbl.setText("Calculando...")

                # Update VU meter progress bar (peak_linear scales 0.0 to 1.0)
                vu_value = int(self.sender.peak_linear * 100)
                self.vu_progress.setValue(vu_value)
            else:
                self.time_lbl.setText("Tiempo: 00:00")
                self.bytes_lbl.setText("Datos enviados: 0.0 MB")
                self.rate_lbl.setText("Velocidad: 0.0 KB/s")
                self.vu_progress.setValue(0)
                self.latency_value_lbl.setText("Inactivo")
                
        def closeEvent(self, event):
            self.is_closed = True
            # Ensure cleanup is done on window close
            if self.sender:
                self.sender.stop()
            event.accept()


# =====================================================================
# CLI HEADLESS MODE FALLBACK
# =====================================================================
def run_cli(ip, port, latency):
    print("====================================================")
    print("      HI-FI PC AUDIO SENDER (HEADLESS CLI)          ")
    print("====================================================")
    print(f"Destino:  {ip}:{port}")
    print(f"Latencia: {latency} ms")
    print("----------------------------------------------------")
    
    sender = AudioSender(ip, port, latency)
    
    def get_vu_bar(linear_val, db_val, width=30):
        filled = int(linear_val * width)
        bar = ""
        for i in range(width):
            if i < filled:
                if i < int(width * 0.6): bar += "█"
                elif i < int(width * 0.85): bar += "▓"
                else: bar += "▒"
            else:
                bar += "░"
        db_str = f"{db_val:5.1f} dB" if db_val > -100.0 else " -inf dB"
        return f"[{bar}] {db_str}"

    try:
        sender.start()
        print("Transmitiendo audio... Presione Ctrl+C para detener.")
        print("")
        
        last_update = 0
        while True:
            time.sleep(0.1)
            now = time.time()
            if now - last_update >= 0.1:
                elapsed = now - sender.start_time
                mb_sent = sender.bytes_sent / (1024 * 1024)
                rate = (sender.bytes_sent / elapsed) / 1024 if elapsed > 0 else 0
                vu = get_vu_bar(sender.peak_linear, sender.peak_db)
                
                sys.stdout.write(f"\r\033[K[Stream] {elapsed:6.1f}s | {mb_sent:6.1f} MB | {rate:5.1f} KB/s\n")
                sys.stdout.write(f"\r\033[K[VU]     {vu}")
                sys.stdout.write("\033[A")
                sys.stdout.flush()
                last_update = now
                
    except KeyboardInterrupt:
        print("\n\nTransmisión detenida por el usuario.")
    finally:
        sender.stop()
        print("Conexiones y dispositivo virtual cerrados.")


# =====================================================================
# MAIN ENTRYPOINT
# =====================================================================
if __name__ == "__main__":
    import signal
    
    parser = argparse.ArgumentParser(description="Hi-Fi PC Audio Sender to RPi CD Player")
    parser.add_argument("-i", "--ip", default=DEFAULT_IP, help=f"Target RPi IP address (default: {DEFAULT_IP})")
    parser.add_argument("-p", "--port", type=int, default=DEFAULT_PORT, help=f"Target UDP port (default: {DEFAULT_PORT})")
    parser.add_argument("-l", "--latency", type=int, default=20, help="Latency in milliseconds for parec (default: 20)")
    parser.add_argument("--headless", action="store_true", help="Run in command-line mode without GUI")
    args = parser.parse_args()

    # Register signal handlers to raise KeyboardInterrupt on termination
    def sig_handler(signum, frame):
        signal.signal(signal.SIGTERM, signal.SIG_DFL)
        signal.signal(signal.SIGINT, signal.SIG_DFL)
        raise KeyboardInterrupt
    signal.signal(signal.SIGTERM, sig_handler)
    signal.signal(signal.SIGINT, sig_handler)

    # Verify parec is available on Linux/macOS
    if platform.system() != 'Windows' and not shutil.which("parec"):
        print("Error: 'parec' no está instalado en este sistema. Es necesario para capturar audio de PulseAudio/PipeWire.", file=sys.stderr)
        print("Por favor, instale pulseaudio-utils.", file=sys.stderr)
        sys.exit(1)

    # Launch GUI or CLI depending on state
    if HAS_GUI and not args.headless:
        print("Iniciando interfaz gráfica PyQt6...")
        app = QApplication(sys.argv)
        main_win = MainWindow(args.ip, args.port)
        main_win.show()
        sys.exit(app.exec())
    else:
        if not HAS_GUI and not args.headless:
            print("Advertencia: PyQt6 no está disponible. Iniciando en modo consola (CLI).")
        run_cli(args.ip, args.port, args.latency)
