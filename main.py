#!/usr/bin/env python3
import os
import sys
import json
import time
import signal
import subprocess
import threading
from http.server import ThreadingHTTPServer, BaseHTTPRequestHandler
import urllib.request
import urllib.parse
from urllib.parse import urlparse, parse_qs
import re
import socket
import fcntl
import struct
import ctypes
import shutil
import base64



# Forzar idioma inglés (C) en todos los subprocesos (setcd, cdparanoia, etc)
# para que el parseo de texto (ej. "No disc is inserted") funcione correctamente
# sin importar el idioma del sistema operativo (ej. es_ES.UTF-8).
os.environ["LC_ALL"] = "C"
os.environ["LANG"] = "C"

# Configuración de Dispositivos y Rutas
PORT = 8000

def get_cd_device():
    import glob
    # 1. Comprobar enlace simbólico persistente de unidad óptica USB y resolverlo a /dev/sr*
    usb_disks = glob.glob("/dev/disk/by-id/usb-*DVD*") + glob.glob("/dev/disk/by-id/usb-*CD*")
    if usb_disks and os.path.exists(usb_disks[0]):
        return os.path.realpath(usb_disks[0])

    # 2. Si no, buscar los dispositivos /dev/sr* y elegir el más reciente
    devices = sorted(glob.glob("/dev/sr*"), key=lambda x: os.path.getmtime(x) if os.path.exists(x) else 0, reverse=True)
    if devices:
        return os.path.realpath(devices[0])
        
    return "/dev/sr0"



MODULES_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "modules")
MODULES_CONFIG_FILE = "/etc/cdplayer_modules.json"
LOCAL_MODULES_CONFIG_FILE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "modules_config.json")

class ModuleManager:
    """Manages game-like modules/mods located in the modules/ directory."""
    def __init__(self, modules_dir):
        self.modules_dir = modules_dir
        self.config_file = MODULES_CONFIG_FILE if (os.path.exists(MODULES_CONFIG_FILE) or os.access("/etc", os.W_OK)) else LOCAL_MODULES_CONFIG_FILE
        self.modules = {} # mod_id -> manifest dict
        self.enabled_modules = set()
        self.routes = {} # (METHOD, PATH) -> handler_func(post_data, http_handler)
        self.loaded_backends = {} # mod_id -> module_instance
        self.state_ref = None
        self.load_config()
        self.discover_modules()
        self.sync_active_services()

    def set_state(self, state):
        self.state_ref = state
        self.init_all_backends()

    def load_config(self):
        for p in [self.config_file, LOCAL_MODULES_CONFIG_FILE]:
            if os.path.exists(p):
                try:
                    with open(p, "r", encoding="utf-8") as f:
                        data = json.load(f)
                        self.enabled_modules = set(data.get("enabled", []))
                        return
                except Exception as e:
                    print(f"[Modules] Error reading config from {p}: {e}", flush=True)
        self.enabled_modules = set()

    def save_config(self):
        data = {"enabled": sorted(list(self.enabled_modules))}
        saved = False
        for p in [self.config_file, LOCAL_MODULES_CONFIG_FILE]:
            try:
                with open(p, "w", encoding="utf-8") as f:
                    json.dump(data, f, indent=2)
                saved = True
                break
            except Exception:
                continue
        if not saved:
            print("[Modules] Warning: could not persist modules config", flush=True)

    def discover_modules(self):
        self.modules = {}
        if not os.path.isdir(self.modules_dir):
            try:
                os.makedirs(self.modules_dir, exist_ok=True)
            except Exception:
                pass
            return

        for entry in sorted(os.listdir(self.modules_dir)):
            mod_path = os.path.join(self.modules_dir, entry)
            manifest_path = os.path.join(mod_path, "module.json")
            if os.path.isdir(mod_path) and os.path.isfile(manifest_path):
                try:
                    with open(manifest_path, "r", encoding="utf-8") as f:
                        manifest = json.load(f)
                    mod_id = manifest.get("id", entry)
                    manifest["id"] = mod_id
                    manifest["path"] = mod_path
                    self.modules[mod_id] = manifest
                except Exception as e:
                    print(f"[Modules] Error loading manifest for '{entry}': {e}", flush=True)

    def list_modules(self):
        self.discover_modules()
        res = []
        for mod_id, m in self.modules.items():
            res.append({
                "id": mod_id,
                "name": m.get("name", mod_id),
                "version": m.get("version", "1.0.0"),
                "description": m.get("description", ""),
                "author": m.get("author", ""),
                "icon": m.get("icon", "🧩"),
                "capabilities": m.get("capabilities", []),
                "enabled": mod_id in self.enabled_modules,
                "service": m.get("service"),
                "web": m.get("web"),
                "ui": m.get("ui")
            })
        return res

    def has_capability(self, capability):
        for mod_id in self.enabled_modules:
            m = self.modules.get(mod_id)
            if m and capability in m.get("capabilities", []):
                return True
        return False

    def get_active_capabilities(self):
        caps = set()
        for mod_id in self.enabled_modules:
            m = self.modules.get(mod_id)
            if m:
                for c in m.get("capabilities", []):
                    caps.add(c)
        return sorted(list(caps))

    def register_route(self, method, path, handler_fn):
        self.routes[(method.upper(), path)] = handler_fn

    def unregister_route(self, method, path):
        self.routes.pop((method.upper(), path), None)

    def dispatch_route(self, method, path, post_data, http_handler):
        handler_fn = self.routes.get((method.upper(), path))
        if handler_fn:
            try:
                handler_fn(post_data, http_handler)
                return True
            except Exception as e:
                print(f"[Modules] Route exception {method} {path}: {e}", flush=True)
                http_handler.send_json({"status": "error", "error": str(e)}, 500)
                return True
        return False

    def load_backend(self, mod_id):
        m = self.modules.get(mod_id)
        if not m or not m.get("backend") or not self.state_ref:
            return
        backend_file = os.path.join(m["path"], m["backend"])
        if not os.path.isfile(backend_file):
            return
        try:
            import importlib.util
            spec = importlib.util.spec_from_file_location(f"mod_{mod_id}", backend_file)
            mod_obj = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(mod_obj)
            if hasattr(mod_obj, "init_module"):
                mod_obj.init_module(self, self.state_ref)
            self.loaded_backends[mod_id] = mod_obj
            print(f"[Modules] Backend loaded for '{mod_id}'", flush=True)
        except Exception as e:
            print(f"[Modules] Error loading backend for '{mod_id}': {e}", flush=True)

    def unload_backend(self, mod_id):
        mod_obj = self.loaded_backends.pop(mod_id, None)
        if mod_obj and hasattr(mod_obj, "teardown_module") and self.state_ref:
            try:
                mod_obj.teardown_module(self, self.state_ref)
                print(f"[Modules] Backend unloaded for '{mod_id}'", flush=True)
            except Exception as e:
                print(f"[Modules] Error unloading backend for '{mod_id}': {e}", flush=True)
        to_del = [k for k, v in self.routes.items() if getattr(v, "__module__", "").startswith(f"mod_{mod_id}")]
        for k in to_del:
            self.routes.pop(k, None)

    def init_all_backends(self):
        for mod_id in list(self.enabled_modules):
            self.load_backend(mod_id)

    def get_module_ui(self, mod_id):
        m = self.modules.get(mod_id)
        if not m or not m.get("ui"):
            return None
        ui_cfg = m["ui"]
        html_content, js_content, css_content = "", "", ""
        html_file = os.path.join(m["path"], ui_cfg.get("html", "ui.html"))
        if os.path.isfile(html_file):
            with open(html_file, "r", encoding="utf-8") as f:
                html_content = f.read()
        js_file = os.path.join(m["path"], ui_cfg.get("js", "ui.js"))
        if os.path.isfile(js_file):
            with open(js_file, "r", encoding="utf-8") as f:
                js_content = f.read()
        css_file = os.path.join(m["path"], ui_cfg.get("css", "ui.css"))
        if os.path.isfile(css_file):
            with open(css_file, "r", encoding="utf-8") as f:
                css_content = f.read()
        return {
            "id": mod_id,
            "slot": ui_cfg.get("slot", "dashboard"),
            "html": html_content,
            "js": js_content,
            "css": css_content
        }

    def toggle_module(self, mod_id, enable=None):
        self.discover_modules()
        if mod_id not in self.modules:
            return False, f"Module '{mod_id}' not found"

        m = self.modules[mod_id]
        cur = mod_id in self.enabled_modules
        target = (not cur) if enable is None else bool(enable)

        if target:
            self.enabled_modules.add(mod_id)
            self.load_backend(mod_id)
        else:
            self.enabled_modules.discard(mod_id)
            self.unload_backend(mod_id)

        self.save_config()

        service = m.get("service")
        if service:
            act = "start" if target else "stop"
            try:
                subprocess.Popen(["systemctl", act, service])
                print(f"[Modules] Executed systemctl {act} {service}", flush=True)
            except Exception as e:
                print(f"[Modules] Error executing systemctl {act} {service}: {e}", flush=True)

        hook_script = os.path.join(m.get("path", ""), "hooks.py")
        if os.path.isfile(hook_script):
            act = "enable" if target else "disable"
            try:
                subprocess.Popen(["python3", hook_script, act], cwd=m.get("path"))
            except Exception as e:
                print(f"[Modules] Error executing hook {hook_script}: {e}", flush=True)

        return True, target

    def sync_active_services(self):
        for mod_id, m in self.modules.items():
            service = m.get("service")
            if service:
                act = "start" if mod_id in self.enabled_modules else "stop"
                try:
                    subprocess.Popen(["systemctl", act, service])
                except Exception:
                    pass

module_manager = ModuleManager(MODULES_DIR)

# Global player state
class PlayerState:
    def __init__(self):
        self.state = "idle"  # idle, playing, paused, error, no_disc, pc_audio
        self.current_track = 1
        self.tracks = []
        self.dac_name = "default"
        self.manual_dac = False
        self.p_read = None  # cd-read subprocess (fallback)
        self.p_play = None  # aplay subprocess (fallback)
        self.bytes_played = 0
        self.play_start_time = 0.0
        self.seek_offset_time = 0.0
        self.session_id = 0
        self.lock = threading.RLock()
        self.error_message = ""
        self.pc_audio_active = False
        self.pc_audio_thread = None
        self.pc_audio_socket = None
        self.buffer_ms = 2500  # aplay buffer in milliseconds (default 2500ms)
        self.buffer_fill_pct = 0  # C engine RAM ring buffer fill percentage
        self.pause_position = 0.0  # track position (seconds) at the moment of pause
        self.album_title = ""
        self.album_artist = ""
        self.cover_url = ""
        self.toc_offsets = []
        self.toc_leadout = 0
        self.recovery_attempts = 0
        # Bluetooth LDAC/A2DP state
        self.bluetooth_active = False
        self.bt_mode = "off"
        self.bt_connected = False
        self.bt_device_name = ""
        self.bt_title = ""
        self.bt_artist = ""
        self.bt_album = ""
        self.bt_status = "stopped"
        self.bt_duration = 0.0
        self.bt_elapsed = 0.0
        # Study mode state
        self.study_mode_active = False
        self.study_lofi_active = False
        self.study_whitenoise_active = False
        self.study_audio_proc = None
        self.study_lofi_proc = None
        self.aux_volume = 80

    @property
    def voip_module_enabled(self):
        return module_manager.has_capability("telephony")

    @property
    def study_module_enabled(self):
        return module_manager.has_capability("study")

    def to_dict(self):
        with self.lock:
            cur_dur = 0
            if self.tracks and self.current_track <= len(self.tracks):
                cur_dur = self.tracks[self.current_track - 1].get("duration", 0)

            if self.state == "playing" and self.play_start_time > 0:
                elapsed = max(0.0, (time.time() - self.play_start_time) + self.seek_offset_time)
                if cur_dur > 0:
                    elapsed = min(elapsed, float(cur_dur))
            else:
                elapsed = self.bytes_played / 176400.0
                if cur_dur > 0:
                    elapsed = min(elapsed, float(cur_dur))

            return {
                "state": self.state,
                "current_track": self.current_track,
                "tracks": self.tracks,
                "dac_name": self.dac_name,
                "elapsed_time": elapsed,
                "error_message": self.error_message,
                "pc_audio_active": self.pc_audio_active,
                "buffer_ms": self.buffer_ms,
                "buffer_fill_pct": getattr(self, "buffer_fill_pct", 0),
                "album_title": getattr(self, "album_title", ""),
                "album_artist": getattr(self, "album_artist", ""),
                "cover_url": getattr(self, "cover_url", ""),
                "bluetooth_active": self.bluetooth_active,
                "bt_mode": getattr(self, "bt_mode", "off"),
                "bt_connected": self.bt_connected,
                "bt_device_name": self.bt_device_name,
                "bt_title": self.bt_title,
                "bt_artist": self.bt_artist,
                "bt_album": self.bt_album,
                "bt_status": self.bt_status,
                "bt_duration": self.bt_duration,
                "bt_elapsed": self.bt_elapsed,
                "study_mode_active": getattr(self, "study_mode_active", False),
                "study_lofi_active": getattr(self, "study_lofi_active", False),
                "study_whitenoise_active": getattr(self, "study_whitenoise_active", False),
                "aux_volume": getattr(self, "aux_volume", 80),
                "voip_module_enabled": self.voip_module_enabled,
                "study_module_enabled": self.study_module_enabled,
                "capabilities": module_manager.get_active_capabilities(),
                "modules": module_manager.list_modules()
            }


state = PlayerState()
module_manager.set_state(state)

# --- NATIVE CDDA AUDIO ENGINE (C/ALSA) ---
class CDTrackC(ctypes.Structure):
    _fields_ = [
        ("track_num", ctypes.c_int),
        ("start_lba", ctypes.c_int),
        ("total_sectors", ctypes.c_int),
        ("duration_sec", ctypes.c_int),
        ("title", ctypes.c_char * 128),
        ("artist", ctypes.c_char * 128)
    ]

class CDDiscInfoC(ctypes.Structure):
    _fields_ = [
        ("album_title", ctypes.c_char * 128),
        ("album_artist", ctypes.c_char * 128),
        ("track_count", ctypes.c_int),
        ("first_track", ctypes.c_int),
        ("last_track", ctypes.c_int),
        ("leadout_lba", ctypes.c_int),
        ("tracks", CDTrackC * 99)
    ]

class CDStatusC(ctypes.Structure):
    _fields_ = [
        ("state", ctypes.c_int),
        ("current_track", ctypes.c_int),
        ("elapsed_time", ctypes.c_double),
        ("duration", ctypes.c_double),
        ("buffer_fill_pct", ctypes.c_int),
        ("error_msg", ctypes.c_char * 256)
    ]

HAS_NATIVE_ENGINE = False
cdda = None

try:
    cdda_lib_dir = os.path.dirname(os.path.abspath(__file__))
    cdda_candidates = [
        os.path.join(cdda_lib_dir, "libcdda_engine.so"),
        "/home/guille/cdplayer/libcdda_engine.so",
        "libcdda_engine.so"
    ]
    cdda_path = next((p for p in cdda_candidates if os.path.exists(p)), None)
    if cdda_path:
        cdda = ctypes.CDLL(cdda_path)
        cdda.cdda_init.argtypes = [ctypes.c_char_p, ctypes.c_int]
        cdda.cdda_init.restype = ctypes.c_int
        cdda.cdda_read_disc_info.argtypes = [ctypes.POINTER(CDDiscInfoC)]
        cdda.cdda_read_disc_info.restype = ctypes.c_int
        cdda.cdda_play.argtypes = [ctypes.c_int, ctypes.c_double, ctypes.c_char_p]
        cdda.cdda_play.restype = ctypes.c_int
        cdda.cdda_pause.restype = ctypes.c_int
        cdda.cdda_resume.restype = ctypes.c_int
        cdda.cdda_stop.restype = ctypes.c_int
        cdda.cdda_get_status.argtypes = [ctypes.POINTER(CDStatusC)]
        cdda.cdda_get_status.restype = ctypes.c_int
        cdda.cdda_set_track_title.argtypes = [ctypes.c_int, ctypes.c_char_p]
        cdda.cdda_set_track_title.restype = ctypes.c_int
        cdda.cdda_set_device.argtypes = [ctypes.c_char_p]
        cdda.cdda_set_device.restype = ctypes.c_int
        cdda.cdda_close.restype = None

        dev_bytes = get_cd_device().encode("utf-8")
        cdda.cdda_init(dev_bytes, 2)
        HAS_NATIVE_ENGINE = True
        print("Motor nativo C CDDA cargado con exito (Bit-Perfect ALSA, buffer 8MB, velocidad 2x fija).", flush=True)
except Exception as e:
    print(f"Motor nativo C CDDA no disponible ({e}). Usando fallback subprocess.", flush=True)


def get_audio_interfaces():
    """Parses /proc/asound/cards to return all detected sound interfaces."""
    interfaces = [{"id": "default", "name": "Salida por Defecto (ALSA)"}]
    try:
        if not os.path.exists("/proc/asound/cards"):
            return interfaces
        with open("/proc/asound/cards", "r") as f:
            content = f.read()
        
        matches = re.findall(r"\s*(\d+)\s*\[(\w+)\s*\]:\s*(.*?)\n", content)
        for m in matches:
            card_id = m[1]
            card_desc = m[2].strip()
            if " - " in card_desc:
                card_name = card_desc.split(" - ")[-1]
            else:
                card_name = card_desc
            interfaces.append({"id": card_id, "name": f"{card_name} ({card_id})"})
    except Exception as e:
        print(f"Error listing interfaces: {e}")
    return interfaces


def detect_usb_dac():
    """Detects connected USB DACs from /proc/asound/cards."""
    if state.manual_dac:
        interfaces = get_audio_interfaces()
        if any(i["id"] == state.dac_name for i in interfaces):
            return state.dac_name
        else:
            state.manual_dac = False
            
    try:
        if not os.path.exists("/proc/asound/cards"):
            return "disconnected"
        with open("/proc/asound/cards", "r") as f:
            content = f.read()
        
        # Look for USB-Audio interface (e.g. AUDIO for SMSL SU-1)
        matches = re.findall(r"\s*(\d+)\s*\[(\w+)\s*\]:\s*USB-Audio", content)
        if matches:
            return matches[0][1]
        
        matches_smsl = re.findall(r"\s*(\d+)\s*\[(\w+)\s*\]:\s*.*(?:SMSL|AUDIO)", content, re.IGNORECASE)
        if matches_smsl:
            return matches_smsl[0][1]
    except Exception as e:
        print(f"Error detecting DAC: {e}")
    return "disconnected"


CDROM_SELECT_SPEED = 0x5322

def set_drive_speed(speed=2):
    """Clamps hardware drive speed to safe threshold (default 2x = 300 KB/s).
    Prevents spindle motor overspeed (~10,000 RPM -> ~450 RPM), disc flutter/wobble,
    lens collisions/scratches, and current brownouts."""
    dev = get_cd_device()
    if not os.path.exists(dev):
        return

    # 1. Direct Linux CD-ROM MMC ioctl
    try:
        fd = os.open(dev, os.O_RDONLY | os.O_NONBLOCK)
        fcntl.ioctl(fd, CDROM_SELECT_SPEED, speed)
        os.close(fd)
    except Exception:
        pass

    # 2. Userspace tool fallback (setcd / eject)
    try:
        subprocess.run(["setcd", "-x", str(speed), dev], capture_output=True, timeout=2)
    except Exception:
        pass


def check_cd_inserted(current_presence=False):
    """Checks if a CD is inserted using setcd."""
    try:
        dev = get_cd_device()
        if not os.path.exists(dev):
            return False
        res = subprocess.run(["setcd", "-i", dev], capture_output=True, text=True, timeout=3)
        out_lower = (res.stdout + res.stderr).lower()
        if "disc found" in out_lower:
            return True
        if "no disc is inserted" in out_lower or "tray is open" in out_lower:
            return False
        if "drive is not ready" in out_lower:
            # Drive is in standby with disc inside (or spinning up)
            return True
        return current_presence
    except Exception:
        return current_presence


def get_track_list():
    """Parses track list with native C engine (instantaneous), enriched with CD-Text and MusicBrainz."""
    dev = get_cd_device()
    if not os.path.exists(dev):
        return []

    # Enforce safe speed to prevent high-RPM vibration
    set_drive_speed(2)

    tracks = []
    raw_entries = []
    leadout_lba = 0
    album_artist = ""
    album_title = ""

    # 1. Native C Engine TOC read (< 5 milliseconds)
    if HAS_NATIVE_ENGINE:
        try:
            try:
                cdda.cdda_set_device(dev.encode("utf-8"))
            except Exception:
                pass
            info = CDDiscInfoC()
            count = cdda.cdda_read_disc_info(ctypes.byref(info))
            if count > 0:
                leadout_lba = info.leadout_lba
                for i in range(count):
                    t = info.tracks[i]
                    dur = t.duration_sec
                    mins = dur // 60
                    secs = dur % 60
                    raw_entries.append((t.track_num, t.start_lba))
                    tracks.append({
                        "track": t.track_num,
                        "title": f"Pista {t.track_num}",
                        "artist": "",
                        "start_lba": t.start_lba,
                        "start_sec": float(t.start_lba / 75.0),
                        "total_sectors": t.total_sectors,
                        "duration": dur,
                        "formatted_duration": f"{mins:02d}:{secs:02d}"
                    })
                if raw_entries:
                    state.toc_offsets = raw_entries
                    state.toc_leadout = leadout_lba
        except Exception as e:
            print(f"Notice: C engine TOC read exception: {e}", flush=True)

    # 2. CD-Text enrichment via safe non-probing cd-info scan
    track_titles = {}
    track_performers = {}

    try:
        res = subprocess.run(
            ["cd-info", "--no-device-info", "--no-disc-mode", "--no-analyze", "-C", dev],
            capture_output=True, text=True, timeout=5
        )
        lines = res.stdout.splitlines()
        current_track_idx = None
        in_cdtext = False

        for line in lines:
            line_str = line.strip()
            if line_str.startswith("CD-TEXT for Disc:"):
                in_cdtext = True
                continue
            m_tr_title = re.search(r"CD-TEXT for Track\s+(\d+):", line_str)
            if m_tr_title:
                current_track_idx = int(m_tr_title.group(1))
                continue
            if in_cdtext:
                m_title = re.search(r"TITLE:\s*(.*)", line_str)
                if m_title:
                    val = m_title.group(1).strip()
                    if current_track_idx is not None:
                        track_titles[current_track_idx] = val
                    elif not album_title:
                        album_title = val
                m_perf = re.search(r"PERFORMER:\s*(.*)", line_str)
                if m_perf:
                    val = m_perf.group(1).strip()
                    if current_track_idx is not None:
                        track_performers[current_track_idx] = val
                    elif not album_artist:
                        album_artist = val

            # Fallback parse LSN table if C engine didn't populate tracks
            if not tracks:
                m_lsn = re.match(r"\s*(\d+):\s+[\d:]+\s+(\d+)\s+(audio|leadout)", line)
                if m_lsn:
                    t_num = int(m_lsn.group(1))
                    lba = int(m_lsn.group(2))
                    t_type = m_lsn.group(3)
                    if t_type == "audio" and t_num < 100:
                        raw_entries.append((t_num, lba))
                    elif t_type == "leadout" or t_num >= 100:
                        leadout_lba = lba

        if not tracks and raw_entries:
            for i in range(len(raw_entries)):
                t_num, start_lba = raw_entries[i]
                next_lba = raw_entries[i+1][1] if i + 1 < len(raw_entries) else leadout_lba
                total_sectors = max(75, next_lba - start_lba)
                duration = int(total_sectors / 75.0)
                mins = duration // 60
                secs = duration % 60
                tracks.append({
                    "track": t_num,
                    "title": f"Pista {t_num}",
                    "artist": "",
                    "start_lba": start_lba,
                    "start_sec": float(start_lba / 75.0),
                    "total_sectors": total_sectors,
                    "duration": duration,
                    "formatted_duration": f"{mins:02d}:{secs:02d}"
                })
            state.toc_offsets = raw_entries
            state.toc_leadout = leadout_lba

        # Apply CD-Text metadata to tracks
        for t in tracks:
            trk_n = t["track"]
            if trk_n in track_titles:
                t["title"] = track_titles[trk_n]
                if HAS_NATIVE_ENGINE:
                    try: cdda.cdda_set_track_title(trk_n, track_titles[trk_n].encode("utf-8")[:127])
                    except Exception: pass
            if trk_n in track_performers:
                t["artist"] = track_performers[trk_n]

    except Exception as e:
        print(f"Notice: cd-info CD-Text parse exception: {e}", flush=True)

    state.album_artist = album_artist
    state.album_title = album_title
    state.cover_url = ""

    return tracks


def musicbrainz_lookup():
    """Queries MusicBrainz using the stored CD TOC offsets.
    Returns a list of release dicts: {id, title, artist, year, cover_url, tracks:[{n, title, artist}]}.
    Uses the free MusicBrainz Web Service v2 (no API key required, User-Agent header mandatory)."""
    import urllib.request
    import urllib.parse

    with state.lock:
        offsets = list(state.toc_offsets)
        leadout = state.toc_leadout

    if not offsets:
        return {"error": "No hay datos TOC en memoria. Escanea el CD primero.", "releases": []}

    first_track = offsets[0][0]
    last_track = offsets[-1][0]
    # MusicBrainz TOC: first last_track leadout offset1 offset2 ...
    # All values are in CD frames (sectors at 75 fps). cdparanoia gives us begin_sector directly.
    sector_offsets = [o[1] for o in offsets]

    # Add 150-frame offset (CD pre-gap standard) to each sector
    mb_offsets = [s + 150 for s in sector_offsets]
    mb_leadout = leadout + 150 if leadout > 0 else (mb_offsets[-1] + 300)  # rough estimate

    toc_str = f"{first_track}+{last_track}+{mb_leadout}+" + "+".join(str(o) for o in mb_offsets)
    url = f"https://musicbrainz.org/ws/2/discid/-?toc={urllib.parse.quote(toc_str)}&fmt=json&inc=artist-credits+recordings"

    headers = {
        "User-Agent": "RpiCdPlayer/1.0 (https://github.com/guillewan2/cdplayer)",
        "Accept": "application/json",
    }

    try:
        req = urllib.request.Request(url, headers=headers)
        with urllib.request.urlopen(req, timeout=10) as resp:
            data = json.loads(resp.read().decode("utf-8"))
    except Exception as e:
        return {"error": f"Error consultando MusicBrainz: {e}", "releases": []}

    releases_raw = data.get("releases", [])
    if not releases_raw:
        return {"error": "No se encontró el disco en MusicBrainz.", "releases": []}

    results = []
    for rel in releases_raw[:10]:  # max 10 results
        rel_id = rel.get("id", "")
        title = rel.get("title", "Desconocido")
        date = rel.get("date", "")[:4]  # year only

        # Artist
        artist = ""
        ac = rel.get("artist-credit", [])
        if ac:
            artist = ac[0].get("name", "") or ac[0].get("artist", {}).get("name", "")

        # Find the medium that matched the TOC query (it's the one that has a 'tracks' list).
        # Other media in the release are present in the response but without track data.
        all_media = rel.get("media", [])
        disc_total = len(all_media)

        matched_medium = None
        disc_number = 1
        for medium in all_media:
            if medium.get("tracks"):  # only the matching medium has tracks
                matched_medium = medium
                disc_number = medium.get("position", 1)
                break

        track_list = []
        if matched_medium:
            for tr in matched_medium.get("tracks", []):
                tn = tr.get("number", "")
                try:
                    tn_int = int(tn)
                except Exception:
                    tn_int = len(track_list) + 1
                recording = tr.get("recording", {})
                tr_title = recording.get("title", tr.get("title", f"Pista {tn_int}"))
                tr_artist = ""
                tr_ac = recording.get("artist-credit", [])
                if tr_ac:
                    tr_artist = tr_ac[0].get("name", "") or tr_ac[0].get("artist", {}).get("name", "")
                tr_len_ms = recording.get("length", 0) or 0
                tr_dur = tr_len_ms // 1000
                track_list.append({
                    "track": tn_int,
                    "title": tr_title,
                    "artist": tr_artist if tr_artist != artist else "",
                    "duration": tr_dur,
                    "formatted_duration": f"{tr_dur//60:02d}:{tr_dur%60:02d}"
                })

        # Cover art from Cover Art Archive
        cover_url = f"https://coverartarchive.org/release/{rel_id}/front-250" if rel_id else ""

        results.append({
            "id": rel_id,
            "title": title,
            "artist": artist,
            "year": date,
            "cover_url": cover_url,
            "disc_number": disc_number,
            "disc_total": disc_total,
            "tracks": track_list,
        })

    return {"releases": results}


def stop_playback_unsafe():
    """Stops playback without acquiring lock (internal use)."""
    state.session_id += 1
    if state.pc_audio_active:
        state.pc_audio_active = False

    if HAS_NATIVE_ENGINE:
        try:
            cdda.cdda_stop()
        except Exception:
            pass

    proc_play = state.p_play
    proc_read = state.p_read
    state.p_play = None
    state.p_read = None
    state.bytes_played = 0

    if state.state in ("playing", "paused", "pc_audio", "starting"):
        state.state = "idle"

    # Cerrar pipes y terminar procesos sincronamente para liberar el DAC ALSA
    for p in (proc_read, proc_play):
        if p:
            try:
                if hasattr(p, 'stdin') and p.stdin:
                    try: p.stdin.close()
                    except Exception: pass
                if hasattr(p, 'stdout') and p.stdout:
                    try: p.stdout.close()
                    except Exception: pass
                p.terminate()
                p.wait(timeout=0.15)
            except Exception:
                try:
                    p.kill()
                except Exception:
                    pass


def monitor_playback_native(track, session_id):
    """Thread function to monitor playback via native C CDDA engine."""
    st = CDStatusC()
    while True:
        time.sleep(0.25)
        engine_state = None
        with state.lock:
            if state.session_id != session_id:
                break
            if state.state not in ("playing", "starting", "paused"):
                break

            cdda.cdda_get_status(ctypes.byref(st))
            state.current_track = st.current_track
            state.bytes_played = int(st.elapsed_time * 176400.0)
            state.buffer_fill_pct = st.buffer_fill_pct
            engine_state = st.state

        # If entire disc completed naturally (engine state 0 = idle)
        if engine_state == 0:
            stop_playback_unsafe()
            with state.lock:
                state.current_track = 1
                state.state = "idle"
                state.error_message = ""
            print("Fin del disco alcanzado. Volviendo a modo espera (idle)...", flush=True)
            break

        # If DAC was disconnected or switched to console
        if engine_state == -3:
            stop_playback_unsafe()
            with state.lock:
                state.state = "idle"
                state.dac_name = "disconnected"
                state.error_message = st.error_msg.decode("utf-8", errors="replace") or "DAC SMSL SU-1 no disponible (modo consola)"
            print(f"Estado de DAC: {st.error_msg.decode('utf-8', errors='replace')}", flush=True)
            break

        # If engine encountered an unrecoverable error
        if engine_state == -1:
            stop_playback_unsafe()
            with state.lock:
                state.state = "error"
                state.error_message = st.error_msg.decode("utf-8", errors="replace") or "Fallo en motor de audio C"
            break


def monitor_playback(p_read, p_play, track, session_id):
    """Thread function to monitor playback process and advance tracks with session guard."""
    time.sleep(1.0)
    with state.lock:
        if state.session_id == session_id and state.state == "starting":
            state.state = "playing"
            state.play_start_time = time.time()

    while True:
        time.sleep(0.3)
        with state.lock:
            if state.session_id != session_id or state.state not in ("playing", "starting", "paused"):
                break
            
            cur_dur = 0
            if state.tracks and track <= len(state.tracks):
                cur_dur = state.tracks[track - 1].get("duration", 0)

            if state.play_start_time > 0:
                played_secs = max(0.0, (time.time() - state.play_start_time) + state.seek_offset_time)
            else:
                played_secs = state.bytes_played / 176400.0

            # Reset recovery attempts if played cleanly for more than 5 seconds
            if played_secs > 5.0 and state.recovery_attempts > 0:
                state.recovery_attempts = 0

            # CD reader finished reading track sectors
            read_done = (p_read and p_read.poll() is not None)
            time_exceeded = (cur_dur > 0 and played_secs >= cur_dur)

            # Si cd-read murió prematuramente con error
            if read_done and not time_exceeded:
                ret_read = p_read.returncode
                if ret_read != 0 and cur_dur > 0 and played_secs < cur_dur - 4.0:
                    err_msg = ""
                    try:
                        if p_read.stderr:
                            err_msg = p_read.stderr.read().decode("utf-8", errors="replace").strip()
                    except Exception:
                        pass

                    state.recovery_attempts += 1
                    print(f"Lectura de CD interrumpida (código {ret_read}) en seg {played_secs:.1f}. Intento {state.recovery_attempts}/3. Detalle: {err_msg}", flush=True)

                    if state.recovery_attempts > 3:
                        stop_playback_unsafe()
                        state.state = "error"
                        state.error_message = f"Fallo persistente leyendo CD en seg {int(played_secs)}s."
                        break

                    stop_playback_unsafe()
                    time.sleep(0.6)
                    dev = get_cd_device()
                    if os.path.exists(dev):
                        print(f"Reanudando Pista {track} desde segundo {played_secs:.1f}...", flush=True)
                        start_playback_unsafe(track, seek_time=played_secs)
                    else:
                        state.state = "error"
                        state.error_message = "Lector de CD desconectado."
                    break

            if time_exceeded or (read_done and cur_dur > 0 and played_secs >= cur_dur - 1.0):
                if state.session_id != session_id:
                    break

                stop_playback_unsafe()
                
                next_track = track + 1
                has_next = any(t["track"] == next_track for t in state.tracks)
                if has_next:
                    print(f"Pista {track} finalizada ({played_secs:.1f}s). Avanzando automáticamente a Pista {next_track}...", flush=True)
                    start_playback_unsafe(next_track)
                else:
                    print("Fin del disco alcanzado. Volviendo a modo espera (idle)...", flush=True)
                    state.current_track = 1
                    state.state = "idle"
                    state.error_message = ""
                break

            # Check for DAC crash
            if p_play and p_play.poll() is not None:
                ret_play = p_play.returncode
                if ret_play not in (0, -15, -9) and state.session_id == session_id and played_secs < 3.0:
                    stop_playback_unsafe()
                    state.state = "error"
                    state.error_message = f"Fallo de audio DAC (código {ret_play})."
                    break


def start_playback_unsafe(track=1, seek_time=0.0):
    """Starts playback of a given track using kernel direct pipe cd-read -> aplay."""
    # If Bluetooth or PC audio sharing is active, disable them first
    if state.bluetooth_active:
        if 'bluetooth_ctl' in globals() and bluetooth_ctl:
            bluetooth_ctl.disable()
        time.sleep(0.3)

    if state.pc_audio_active:
        state.pc_audio_active = False
        if state.pc_audio_socket:
            try: state.pc_audio_socket.close()
            except Exception: pass
        if state.p_play:
            try: state.p_play.terminate()
            except Exception: pass
        time.sleep(0.3)

    stop_playback_unsafe()
    
    state.session_id += 1
    current_session = state.session_id

    dev = get_cd_device()
    state.dac_name = detect_usb_dac()
    if state.dac_name == "disconnected":
        state.state = "idle"
        state.error_message = "DAC SMSL SU-1 no disponible (comprueba entrada USB en el DAC)"
        print("Intento de reproducción con DAC SMSL SU-1 desconectado (modo consola).", flush=True)
        return

    state.bytes_played = int(seek_time * 176400.0)
    state.seek_offset_time = seek_time
    state.play_start_time = 0.0
    state.current_track = track

    if not state.tracks:
        state.tracks = get_track_list()
    
    track_obj = next((t for t in state.tracks if t["track"] == track), None)
    if track_obj:
        start_lba = int(track_obj.get("start_lba", 0)) + int(seek_time * 75)
        total_sectors = int(track_obj.get("total_sectors", int(track_obj.get("duration", 0) * 75))) - int(seek_time * 75)
    else:
        start_lba = int(seek_time * 75)
        total_sectors = 75 * 300  # fallback 5 mins

    total_sectors = max(75, total_sectors)

    # Enforce safe 2x speed to prevent high-RPM vibration and disc scratches
    set_drive_speed(2)

    # 1. Intentar reproducción con el motor nativo C (Bit-Perfect, ring buffer 8MB, velocidad 2x fija)
    if HAS_NATIVE_ENGINE:
        try:
            try:
                cdda.cdda_set_device(dev.encode("utf-8"))
            except Exception:
                pass
            alsa_dev = f"plughw:{state.dac_name}".encode("utf-8") if state.dac_name != "default" else b"default"
            ret = cdda.cdda_play(track, float(seek_time), alsa_dev)
            if ret == 0:
                with state.lock:
                    state.state = "playing"
                    state.current_track = track
                    state.error_message = ""
                threading.Thread(target=monitor_playback_native, args=(track, current_session), daemon=True).start()
                return
            elif ret == -3:
                with state.lock:
                    state.state = "idle"
                    state.dac_name = "disconnected"
                    state.error_message = "DAC SMSL SU-1 no disponible (modo consola)"
                print("DAC SMSL SU-1 no disponible en ALSA.", flush=True)
                return
            else:
                st = CDStatusC()
                cdda.cdda_get_status(ctypes.byref(st))
                err = st.error_msg.decode("utf-8", errors="replace") or f"cdda_play error {ret}"
                print(f"Motor C retorno código {ret} ({err}). Usando fallback cd-read|aplay...", flush=True)
        except Exception as e:
            print(f"Excepción en motor C ({e}). Usando fallback cd-read|aplay...", flush=True)

    # 2. Fallback a pipe cd-read -> aplay
    try:
        alsa_device = f"plughw:{state.dac_name}" if state.dac_name != "default" else "default"
        buf_usec = state.buffer_ms * 1000
        state.p_play = subprocess.Popen(
            ["aplay", "-D", alsa_device, "-t", "raw", "-f", "S16_LE", "-r", "44100", "-c", "2", "-B", str(buf_usec), "-F", "50000", "-q"],
            stdin=subprocess.PIPE,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.PIPE
        )

        # Usar -C (--cdrom-device) en lugar de -i para abrir correctamente unidades ópticas /dev/sr*
        cmd = [
            "cd-read",
            "-C", dev,
            "-m", "audio",
            "-s", str(start_lba),
            "-n", str(total_sectors),
            "--no-header",
            "--no-hexdump"
        ]

        state.p_read = subprocess.Popen(
            cmd,
            stdout=state.p_play.stdin,
            stderr=subprocess.PIPE
        )

        state.state = "starting"
        state.current_track = track
        state.error_message = ""
        
        threading.Thread(target=monitor_playback, args=(state.p_read, state.p_play, track, current_session), daemon=True).start()
        
    except Exception as e:
        state.state = "error"
        state.error_message = str(e)
        stop_playback_unsafe()


def pc_audio_receiver_loop(mode="low"):
    """GStreamer RTP Receiver for PC & Mobile audio sharing with clock synchronization and zero pops."""
    if state.bluetooth_active:
        if 'bluetooth_ctl' in globals() and bluetooth_ctl:
            bluetooth_ctl.disable()
        time.sleep(0.3)

    with state.lock:
        dac_id = state.dac_name

    alsa_dev = f"plughw:{dac_id}" if dac_id != "default" else "default"
    print(f"Audio Receiver (GStreamer RTP) [Modo: {mode}]: Port 3000, DAC: {alsa_dev}")

    if mode == "ultra_low":
        # Modo ultra baja latencia / gaming (~15ms)
        queue_params = ["queue", "max-size-buffers=3", "max-size-bytes=0", "max-size-time=15000000", "leaky=downstream"]
        alsa_params = ["alsasink", f"device={alsa_dev}", "sync=false", "buffer-time=20000", "latency-time=5000"]
    elif mode == "lossless":
        # Modo Lossless Audiophile / Cero descartes (~120ms de buffer en vez de 3s)
        queue_params = ["queue", "max-size-buffers=0", "max-size-bytes=0", "max-size-time=120000000", "leaky=no"]
        alsa_params = ["alsasink", f"device={alsa_dev}", "sync=false", "buffer-time=120000", "latency-time=20000"]
    else:
        # Modo equilibrado sin saltos ni descarte de paquetes (~80-100ms buffer, cero cortes)
        queue_params = ["queue", "max-size-buffers=0", "max-size-bytes=0", "max-size-time=100000000", "leaky=no"]
        alsa_params = ["alsasink", f"device={alsa_dev}", "sync=false", "buffer-time=100000", "latency-time=20000"]

    pipeline = [
        "gst-launch-1.0", "-q",
        "udpsrc", "port=3000", "buffer-size=4194304",
        'caps=application/x-rtp,media=audio,clock-rate=48000,encoding-name=L24,channels=2',
        "!", "rtpL24depay",
        "!", *queue_params,
        "!", "audioconvert",
        "!", "audioresample",
        "!", *alsa_params
    ]

    try:
        proc = subprocess.Popen(pipeline)
        with state.lock:
            state.p_play = proc
    except Exception as e:
        print(f"Failed to start GStreamer RTP receiver: {e}")
        with state.lock:
            state.state = "error"
            state.error_message = f"Error GStreamer: {e}"
        return

    # Start UDP ping/pong responder for network latency calculation
    ping_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    ping_sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    try:
        ping_sock.bind(("", 3001))
        ping_sock.settimeout(0.5)
    except Exception:
        ping_sock = None

    def ping_responder():
        while state.pc_audio_active and ping_sock:
            try:
                data, addr = ping_sock.recvfrom(512)
                if data.startswith(b"PING"):
                    ping_sock.sendto(b"PONG" + data[4:], addr)
            except Exception:
                pass
        if ping_sock:
            try: ping_sock.close()
            except Exception: pass

    if ping_sock:
        threading.Thread(target=ping_responder, daemon=True).start()

    # Wait until pc_audio_active is turned off
    while state.pc_audio_active and proc.poll() is None:
        time.sleep(0.2)

    try:
        proc.terminate()
        proc.wait(timeout=1.0)
    except Exception:
        try:
            proc.kill()
        except Exception:
            pass

    with state.lock:
        state.p_play = None
        if state.pc_audio_active:
            state.pc_audio_active = False
            state.state = "idle"


# --- BLUETOOTH LDAC / A2DP CONTROLLER ---
class BluetoothController:
    def __init__(self, state_ref):
        self.state = state_ref
        self.aplay_proc = None
        self.bus = None
        self.auto_played = False
        self._was_connected = False
        self._init_dbus()
        self.running = True
        self.monitor_thread = threading.Thread(target=self._monitor_loop, daemon=True)
        self.monitor_thread.start()

    def _init_dbus(self):
        try:
            import dbus
            self.bus = dbus.SystemBus()
        except Exception as e:
            self.bus = None
            print(f"[Bluetooth] D-Bus initialization warning: {e}", flush=True)

    def set_mode(self, mode):
        """Sets Bluetooth mode: 'voip', 'music', 'both', or 'off'."""
        print(f"[Bluetooth] set_mode called with: {mode}", flush=True)
        if mode == "off":
            self.disable()
            return

        # Stop PC audio receiver if active
        if self.state.pc_audio_active:
            self.state.pc_audio_active = False
            if self.state.pc_audio_socket:
                try: self.state.pc_audio_socket.close()
                except Exception: pass
            if self.state.p_play:
                try: self.state.p_play.terminate()
                except Exception: pass

        if mode in ("music", "both"):
            try:
                stop_playback_unsafe()
            except Exception:
                pass
            with self.state.lock:
                self.state.bluetooth_active = True
                self.state.state = "bluetooth"
                self.state.bt_mode = mode
                self.state.error_message = ""
        elif mode == "voip":
            with self.state.lock:
                self.state.bt_mode = "voip"
                # If currently on bluetooth full screen, restore idle screen
                if self.state.state == "bluetooth":
                    self.state.bluetooth_active = False
                    self.state.state = "idle"

        # Ensure Bluetooth adapter is powered on
        try:
            subprocess.run(["bluetoothctl", "power", "on"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        except Exception:
            pass

        # Make Bluetooth discoverable and pairable
        self._set_adapter_properties(discoverable=True, pairable=True)

        # Handle ALSA routing: only for music/both
        if mode in ("music", "both"):
            self._start_aplay()
        else:
            self._stop_aplay()

        # Connect / apply profiles in background thread
        threading.Thread(target=self._auto_connect_and_apply, args=(mode,), daemon=True).start()

        # Start monitor thread
        self.running = True
        if not self.monitor_thread or not self.monitor_thread.is_alive():
            self.monitor_thread = threading.Thread(target=self._monitor_loop, daemon=True)
            self.monitor_thread.start()

    def enable(self):
        self.set_mode("both")

    def disable(self):
        self._stop_aplay()
        self._set_adapter_properties(discoverable=False, pairable=False)
        try:
            subprocess.Popen(["bluetoothctl", "disconnect"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        except Exception:
            pass
        try:
            # Power off adapter so Bluetooth is strictly inactive
            subprocess.Popen(["bluetoothctl", "power", "off"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        except Exception:
            pass
        try:
            if os.path.exists("/tmp/current_cover.bmp"):
                os.remove("/tmp/current_cover.bmp")
            if os.path.exists("/tmp/current_cover.jpg"):
                os.remove("/tmp/current_cover.jpg")
        except Exception:
            pass
        with self.state.lock:
            self.state.bluetooth_active = False
            self.state.bt_mode = "off"
            self.state.bt_connected = False
            self.state.bt_device_name = ""
            self.state.bt_title = ""
            self.state.bt_artist = ""
            self.state.bt_album = ""
            self.state.bt_status = "stopped"
            self.state.bt_duration = 0.0
            self.state.bt_elapsed = 0.0
            self.state.cover_url = ""
            if self.state.state == "bluetooth":
                self.state.state = "idle"

    def toggle(self):
        with self.state.lock:
            cur_mode = getattr(self.state, "bt_mode", "off")
            is_active = self.state.bluetooth_active
        if cur_mode != "off" or is_active:
            self.disable()
        else:
            self.set_mode("both")

    def _start_aplay(self):
        self._stop_aplay()
        with self.state.lock:
            dac_id = self.state.dac_name
        alsa_dev = f"plughw:{dac_id}" if dac_id != "default" else "default"
        # Bit-perfect playback with no volume modification, direct to DAC
        cmd = ["bluealsa-aplay", "-D", alsa_dev, "--volume=none", "--profile-a2dp"]
        try:
            self.aplay_proc = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            print(f"[Bluetooth] bluealsa-aplay started on {alsa_dev} (PID: {self.aplay_proc.pid})", flush=True)
        except Exception as e:
            print(f"[Bluetooth] Error starting bluealsa-aplay: {e}", flush=True)

    def _stop_aplay(self):
        if self.aplay_proc:
            try:
                self.aplay_proc.terminate()
                self.aplay_proc.wait(timeout=1.0)
            except Exception:
                try: self.aplay_proc.kill()
                except Exception: pass
            self.aplay_proc = None
            print("[Bluetooth] bluealsa-aplay stopped", flush=True)

    def _process_direct_cover(self, art_url):
        try:
            local_path = None
            if art_url.startswith("file://"):
                local_path = urllib.parse.unquote(art_url[7:])
            elif os.path.exists(art_url):
                local_path = art_url

            if local_path and os.path.exists(local_path):
                subprocess.run(
                    ["ffmpeg", "-y", "-i", local_path, "-vf", "scale=240:240", "/tmp/current_cover.bmp"],
                    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
                )
                with self.state.lock:
                    self.state.cover_url = art_url
                print(f"[Bluetooth Cover] Carátula local cargada desde {local_path}", flush=True)
            else:
                self._clean_cover_files()
        except Exception as e:
            print(f"[Bluetooth Cover] Error procesando carátula directa: {e}", flush=True)

    def _clean_cover_files(self):
        try:
            if os.path.exists("/tmp/current_cover.bmp"):
                os.remove("/tmp/current_cover.bmp")
            if os.path.exists("/tmp/current_cover.jpg"):
                os.remove("/tmp/current_cover.jpg")
        except Exception:
            pass
        with self.state.lock:
            self.state.cover_url = ""

    def _trigger_auto_play(self):
        def _worker():
            print("[Bluetooth] Connection detected! Starting auto-play loop...", flush=True)
            for attempt in range(20):
                time.sleep(0.5)
                if not self.state.bluetooth_active:
                    break
                with self.state.lock:
                    if not self.state.bt_connected:
                        break
                    if self.state.bt_status == "playing":
                        print(f"[Bluetooth] Auto-play active (status is playing on attempt {attempt+1})", flush=True)
                        break

                try:
                    player, control = self._get_player_and_control()
                    import dbus
                    sent = False
                    if player:
                        try:
                            dbus.Interface(player, "org.bluez.MediaPlayer1").Play()
                            sent = True
                        except Exception:
                            pass
                    if control and not sent:
                        try:
                            dbus.Interface(control, "org.bluez.MediaControl1").Play()
                            sent = True
                        except Exception:
                            pass
                    if sent:
                        print(f"[Bluetooth] Auto-play command sent (attempt {attempt+1})", flush=True)
                except Exception as e:
                    print(f"[Bluetooth] Auto-play attempt error: {e}", flush=True)

        threading.Thread(target=_worker, daemon=True).start()

    def _set_adapter_properties(self, discoverable=True, pairable=True):
        if not self.bus:
            self._init_dbus()
        if not self.bus:
            return
        try:
            import dbus
            adapter = self.bus.get_object("org.bluez", "/org/bluez/hci0")
            props = dbus.Interface(adapter, "org.freedesktop.DBus.Properties")
            props.Set("org.bluez.Adapter1", "Discoverable", dbus.Boolean(discoverable))
            props.Set("org.bluez.Adapter1", "Pairable", dbus.Boolean(pairable))
        except Exception as e:
            print(f"[Bluetooth] Could not set adapter properties: {e}", flush=True)

    def _auto_connect_and_apply(self, mode):
        """Connects to known paired phone and enforces profile routing for mode."""
        time.sleep(0.4)
        AUDIO_SOURCE_UUID = "0000110a-0000-1000-8000-00805f9b34fb"
        HFP_AG_UUID       = "0000111f-0000-1000-8000-00805f9b34fb"
        try:
            if not self.bus:
                self._init_dbus()
            if not self.bus:
                return

            import dbus
            bluez = self.bus.get_object("org.bluez", "/")
            mgr = dbus.Interface(bluez, "org.freedesktop.DBus.ObjectManager")
            objects = mgr.GetManagedObjects()

            candidates = []
            for path, ifaces in objects.items():
                if "org.bluez.Device1" in ifaces:
                    d = ifaces["org.bluez.Device1"]
                    if d.get("Paired") and (d.get("Icon") == "phone" or d.get("Address") == "24:95:2F:5C:4E:D3"):
                        candidates.append((path, d.get("Address"), str(d.get("Alias") or d.get("Name") or "")))

            if not candidates:
                candidates.append(("/org/bluez/hci0/dev_24_95_2F_5C_4E_D3", "24:95:2F:5C:4E:D3", "Pixel 7"))

            for path, addr, name in candidates:
                with self.state.lock:
                    cur_mode = getattr(self.state, "bt_mode", "off")
                if cur_mode == "off":
                    break
                print(f"[Bluetooth] Connecting {name} ({addr}) for mode '{mode}'...", flush=True)
                dev = self.bus.get_object("org.bluez", path)
                props = dbus.Interface(dev, "org.freedesktop.DBus.Properties")
                is_connected = bool(props.Get("org.bluez.Device1", "Connected"))

                if not is_connected:
                    init_uuid = HFP_AG_UUID if mode == "voip" else AUDIO_SOURCE_UUID
                    try:
                        dev.ConnectProfile(init_uuid, dbus_interface="org.bluez.Device1")
                        print(f"[Bluetooth] Connected to {name} via profile {init_uuid}.", flush=True)
                    except Exception as e:
                        print(f"[Bluetooth] ConnectProfile to {name} failed: {e}. Trying generic Connect...", flush=True)
                        try:
                            dev.Connect(dbus_interface="org.bluez.Device1")
                            print(f"[Bluetooth] Connected to {name} via generic Connect.", flush=True)
                        except Exception as e2:
                            print(f"[Bluetooth] Generic Connect to {name} failed: {e2}", flush=True)

                # Enforce profiles based on selected mode
                time.sleep(0.3)
                if mode == "voip":
                    try:
                        dev.ConnectProfile(HFP_AG_UUID, dbus_interface="org.bluez.Device1")
                    except Exception: pass
                    try:
                        dev.DisconnectProfile(AUDIO_SOURCE_UUID, dbus_interface="org.bluez.Device1")
                    except Exception: pass
                    print(f"[Bluetooth] Configured {name} for VoIP (HFP connected, A2DP disconnected)", flush=True)
                elif mode == "music":
                    try:
                        dev.ConnectProfile(AUDIO_SOURCE_UUID, dbus_interface="org.bluez.Device1")
                    except Exception: pass
                    try:
                        dev.DisconnectProfile(HFP_AG_UUID, dbus_interface="org.bluez.Device1")
                    except Exception: pass
                    print(f"[Bluetooth] Configured {name} for Music (A2DP connected, HFP disconnected)", flush=True)
                    self._trigger_auto_play()
                elif mode == "both":
                    try:
                        dev.ConnectProfile(HFP_AG_UUID, dbus_interface="org.bluez.Device1")
                    except Exception: pass
                    try:
                        dev.ConnectProfile(AUDIO_SOURCE_UUID, dbus_interface="org.bluez.Device1")
                    except Exception: pass
                    print(f"[Bluetooth] Configured {name} for Both (A2DP & HFP connected)", flush=True)
                    self._trigger_auto_play()
                break
        except Exception as e:
            print(f"[Bluetooth] _auto_connect_and_apply error: {e}", flush=True)

    def _get_player_and_control(self):
        if not self.bus:
            self._init_dbus()
        if not self.bus:
            return None, None
        try:
            import dbus
            bluez = self.bus.get_object("org.bluez", "/")
            mgr = dbus.Interface(bluez, "org.freedesktop.DBus.ObjectManager")
            objects = mgr.GetManagedObjects()
            player = None
            control = None
            for path, ifaces in objects.items():
                if "org.bluez.MediaPlayer1" in ifaces:
                    player = self.bus.get_object("org.bluez", path)
                if "org.bluez.MediaControl1" in ifaces:
                    if ifaces["org.bluez.MediaControl1"].get("Connected", False):
                        control = self.bus.get_object("org.bluez", path)
            return player, control
        except Exception:
            return None, None

    def play(self):
        player, control = self._get_player_and_control()
        try:
            import dbus
            if player:
                dbus.Interface(player, "org.bluez.MediaPlayer1").Play()
            elif control:
                dbus.Interface(control, "org.bluez.MediaControl1").Play()
        except Exception as e:
            print(f"[Bluetooth] Play error: {e}", flush=True)

    def pause(self):
        player, control = self._get_player_and_control()
        try:
            import dbus
            if player:
                dbus.Interface(player, "org.bluez.MediaPlayer1").Pause()
            elif control:
                dbus.Interface(control, "org.bluez.MediaControl1").Pause()
        except Exception as e:
            print(f"[Bluetooth] Pause error: {e}", flush=True)

    def play_pause(self):
        with self.state.lock:
            cur_status = self.state.bt_status
        if cur_status == "playing":
            self.pause()
        else:
            self.play()

    def next(self):
        player, control = self._get_player_and_control()
        try:
            import dbus
            if player:
                dbus.Interface(player, "org.bluez.MediaPlayer1").Next()
            elif control:
                dbus.Interface(control, "org.bluez.MediaControl1").Next()
        except Exception as e:
            print(f"[Bluetooth] Next error: {e}", flush=True)

    def prev(self):
        player, control = self._get_player_and_control()
        try:
            import dbus
            if player:
                dbus.Interface(player, "org.bluez.MediaPlayer1").Previous()
            elif control:
                dbus.Interface(control, "org.bluez.MediaControl1").Previous()
        except Exception as e:
            print(f"[Bluetooth] Previous error: {e}", flush=True)

    def _monitor_loop(self):
        while self.running:
            try:
                if not self.bus:
                    self._init_dbus()

                if self.bus:
                    import dbus
                    bluez = self.bus.get_object("org.bluez", "/")
                    mgr = dbus.Interface(bluez, "org.freedesktop.DBus.ObjectManager")
                    objects = mgr.GetManagedObjects()

                    found_connected_dev = False
                    dev_name = ""
                    dev_addr = ""
                    bat_pct = 0
                    player_obj = None

                    for path, ifaces in objects.items():
                        if "org.bluez.Device1" in ifaces:
                            dev = ifaces["org.bluez.Device1"]
                            if dev.get("Connected", False):
                                found_connected_dev = True
                                dev_name = str(dev.get("Alias") or dev.get("Name") or "")
                                dev_addr = str(dev.get("Address") or "")
                        if "org.bluez.Battery1" in ifaces:
                            try:
                                bat_pct = int(ifaces["org.bluez.Battery1"].get("Percentage", 0))
                            except Exception:
                                pass
                        if "org.bluez.MediaPlayer1" in ifaces:
                            player_obj = self.bus.get_object("org.bluez", path)

                    just_connected = found_connected_dev and not self._was_connected
                    self._was_connected = found_connected_dev

                    with self.state.lock:
                        self.state.bt_connected = found_connected_dev
                        if found_connected_dev:
                            self.state.bt_device_name = dev_name
                        cur_mode = getattr(self.state, "bt_mode", "off")

                    if found_connected_dev and cur_mode == "voip" and player_obj:
                        try:
                            dev_path = player_obj.object_path.rsplit('/', 1)[0]
                            dev_obj = self.bus.get_object("org.bluez", dev_path)
                            dev_obj.DisconnectProfile("0000110a-0000-1000-8000-00805f9b34fb", dbus_interface="org.bluez.Device1")
                            print("[Bluetooth] Auto-disconnected A2DP in VoIP mode", flush=True)
                        except Exception:
                            pass

                    if just_connected and cur_mode in ("music", "both"):
                        print(f"[Bluetooth] Device connected: {dev_name}. Triggering auto-play...", flush=True)
                        self._trigger_auto_play()

                    if found_connected_dev and player_obj and cur_mode in ("music", "both"):
                        try:
                            props = dbus.Interface(player_obj, "org.freedesktop.DBus.Properties")
                            all_props = props.GetAll("org.bluez.MediaPlayer1")
                            status = str(all_props.get("Status", "stopped")).lower()
                            position_ms = int(all_props.get("Position", 0))
                            track = all_props.get("Track", {})
                            title = str(track.get("Title", ""))
                            artist = str(track.get("Artist", ""))
                            album = str(track.get("Album", ""))
                            duration_ms = int(track.get("Duration", 0))

                            with self.state.lock:
                                self.state.bt_status = status
                                self.state.bt_title = title
                                self.state.bt_artist = artist
                                self.state.bt_album = album
                                self.state.bt_duration = duration_ms / 1000.0
                                self.state.bt_elapsed = position_ms / 1000.0

                            # Direct cover art from mobile if provided over AVRCP/MPRIS
                            art_url = str(track.get("mpris:artUrl") or track.get("ArtUrl") or "")
                            if art_url and art_url != getattr(self, "_last_art_url", ""):
                                self._last_art_url = art_url
                                self._process_direct_cover(art_url)
                        except Exception:
                            pass
                    elif not found_connected_dev:
                        self._last_art_url = ""
                        self._clean_cover_files()
                        with self.state.lock:
                            self.state.bt_status = "stopped"
                            self.state.bt_title = ""
                            self.state.bt_artist = ""
                            self.state.bt_album = ""
                            self.state.bt_duration = 0.0
                            self.state.bt_elapsed = 0.0
                    else:
                        with self.state.lock:
                            self.state.bt_status = "stopped"

                    # Ensure bluealsa-aplay is alive only when in music or both mode
                    if self.running and cur_mode in ("music", "both") and (self.aplay_proc is None or self.aplay_proc.poll() is not None):
                        self._start_aplay()
            except Exception:
                pass

            time.sleep(0.5)


bluetooth_ctl = BluetoothController(state)






class CDPlayerHTTPHandler(BaseHTTPRequestHandler):
    def log_message(self, format, *args):
        pass

    def handle_one_request(self):
        try:
            super().handle_one_request()
        except (BrokenPipeError, ConnectionResetError):
            pass

    def send_json(self, data, status=200):
        try:
            payload = json.dumps(data, ensure_ascii=False).encode("utf-8")
            self.send_response(status)
            self.send_header("Content-Type", "application/json")
            self.send_header("Access-Control-Allow-Origin", "*")
            self.send_header("Cache-Control", "no-cache, no-store, must-revalidate")
            self.send_header("Pragma", "no-cache")
            self.send_header("Expires", "0")
            self.send_header("Content-Length", str(len(payload)))
            self.end_headers()
            self.wfile.write(payload)
        except (BrokenPipeError, ConnectionResetError):
            pass
        except Exception:
            pass

    def do_OPTIONS(self):
        try:
            self.send_response(200)
            self.send_header("Access-Control-Allow-Origin", "*")
            self.send_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS")
            self.send_header("Access-Control-Allow-Headers", "Content-Type")
            self.end_headers()
        except (BrokenPipeError, ConnectionResetError):
            pass

    def do_GET(self):
        try:
            self._handle_get()
        except (BrokenPipeError, ConnectionResetError):
            pass

    def _handle_get(self):
        parsed_path = urlparse(self.path).path

        if parsed_path == "/api/status":
            if state.state == "idle":
                state.dac_name = detect_usb_dac()
            self.send_json(state.to_dict())
            return
            
        elif parsed_path == "/api/interfaces":
            self.send_json(get_audio_interfaces())
            return

        elif module_manager.dispatch_route("GET", parsed_path, None, self):
            return

        elif parsed_path.startswith("/api/modules/") and parsed_path.endswith("/ui"):
            parts = [p for p in parsed_path.split("/") if p]
            if len(parts) == 4:
                mod_id = parts[2]
                ui_data = module_manager.get_module_ui(mod_id)
                if ui_data:
                    self.send_json(ui_data)
                    return
                else:
                    self.send_json({"error": "No UI available for module"}, 404)
                    return

        elif parsed_path == "/api/modules":
            self.send_json({
                "modules": module_manager.list_modules(),
                "capabilities": module_manager.get_active_capabilities(),
                "voip_enabled": module_manager.has_capability("telephony"),
                "study_enabled": module_manager.has_capability("study")
            })
            return
            
        elif parsed_path == "/api/tracks":
            with state.lock:
                has_tracks = bool(state.tracks)
                cur_state = state.state
            if not has_tracks and cur_state != "no_disc":
                def fetch_bg():
                    t = get_track_list()
                    if t:
                        with state.lock:
                            state.tracks = t
                            state.state = "idle"
                threading.Thread(target=fetch_bg, daemon=True).start()
            self.send_json(state.to_dict())
            return

        elif parsed_path == "/api/play":
            parsed_url = urlparse(self.path)
            query_params = parse_qs(parsed_url.query)
            target_track = None
            seek_time = 0.0
            if "track" in query_params:
                try: target_track = int(query_params["track"][0])
                except Exception: pass
            if "seek" in query_params:
                try: seek_time = float(query_params["seek"][0])
                except Exception: pass
            if "position" in query_params:
                try: seek_time = float(query_params["position"][0])
                except Exception: pass
            with state.lock:
                track = target_track if target_track is not None else (state.current_track or 1)
            start_playback_unsafe(track, seek_time=seek_time)
            self.send_json(state.to_dict())
            return

        elif parsed_path == "/api/pause":
            with state.lock:
                if state.state == "playing":
                    if HAS_NATIVE_ENGINE:
                        cdda.cdda_pause()
                        state.state = "paused"
                    else:
                        if state.p_read:
                            try: os.kill(state.p_read.pid, signal.SIGSTOP)
                            except Exception: pass
                        sent_secs = state.bytes_played / 176400.0
                        state.pause_position = max(0.0, sent_secs - state.buffer_ms / 1000.0)
                        if state.p_play:
                            try:
                                state.p_play.terminate()
                                state.p_play.wait(timeout=0.3)
                            except Exception:
                                try: state.p_play.kill()
                                except Exception: pass
                            state.p_play = None
                        state.state = "paused"
            self.send_json(state.to_dict())
            return

        elif parsed_path == "/api/resume":
            with state.lock:
                if state.state == "paused":
                    if HAS_NATIVE_ENGINE:
                        cdda.cdda_resume()
                        state.state = "playing"
                    else:
                        if state.p_read:
                            try:
                                state.p_read.kill()
                                state.p_read.wait(timeout=0.3)
                            except Exception: pass
                            state.p_read = None
                        start_playback_unsafe(state.current_track, seek_time=state.pause_position)
            self.send_json(state.to_dict())
            return

        elif parsed_path == "/api/stop":
            with state.lock:
                stop_playback_unsafe()
            self.send_json(state.to_dict())
            return

        elif parsed_path == "/api/lookup":
            # Run MusicBrainz lookup in current thread (caller should be patient)
            result = musicbrainz_lookup()
            self.send_json(result)
            return

        # Serve static web files
        filename = parsed_path.lstrip("/")
        if not filename or filename == "/":
            if self.server.server_port == 8001:
                filename = "visualizer.html"
            else:
                filename = "index.html"
            
        if ".." in filename or filename.startswith("/"):
            self.send_error(403, "Forbidden")
            return
            
        if os.path.exists(filename):
            content_type = "text/html"
            if filename.endswith(".css"):
                content_type = "text/css"
            elif filename.endswith(".js"):
                content_type = "application/javascript"
                
            self.send_response(200)
            self.send_header("Content-Type", content_type)
            self.end_headers()
            with open(filename, "rb") as f:
                self.wfile.write(f.read())
        else:
            self.send_error(404, "Not Found")

    def do_POST(self):
        parsed_path = urlparse(self.path).path

        if parsed_path == "/api/speedtest":
            content_length = int(self.headers.get('Content-Length', 0))
            bytes_read = 0
            chunk_size = 65536
            while bytes_read < content_length:
                to_read = min(chunk_size, content_length - bytes_read)
                chunk = self.rfile.read(to_read)
                if not chunk:
                    break
                bytes_read += len(chunk)
            self.send_json({"speed_test": "ok", "bytes_received": bytes_read})
            return

        content_length = int(self.headers.get('Content-Length', 0))
        post_data = {}
        if content_length > 0:
            try:
                post_data = json.loads(self.rfile.read(content_length).decode('utf-8'))
            except Exception:
                pass

        if self.path.startswith("/api/play") or parsed_path == "/api/play":
            target_track = post_data.get("track")
            seek_time = float(post_data.get("seek", post_data.get("position", 0.0)))
            parsed_url = urlparse(self.path)
            query_params = parse_qs(parsed_url.query)
            if target_track is None and "track" in query_params:
                try: target_track = int(query_params["track"][0])
                except Exception: pass
            if seek_time == 0.0 and "seek" in query_params:
                try: seek_time = float(query_params["seek"][0])
                except Exception: pass
            if seek_time == 0.0 and "position" in query_params:
                try: seek_time = float(query_params["position"][0])
                except Exception: pass

            should_start = False
            track_to_play = 1
            with state.lock:
                if state.state == "paused" and target_track is None and seek_time == 0.0:
                    if HAS_NATIVE_ENGINE:
                        cdda.cdda_resume()
                        state.state = "playing"
                    else:
                        should_start = True
                        track_to_play = state.current_track
                        seek_time = state.pause_position
                elif state.state == "playing" and target_track is None and seek_time == 0.0:
                    pass
                else:
                    should_start = True
                    track_to_play = target_track if target_track is not None else (state.current_track or 1)

            if should_start:
                start_playback_unsafe(track_to_play, seek_time=seek_time)
            self.send_json(state.to_dict())
            
        elif self.path == "/api/seek" or parsed_path == "/api/seek":
            position = post_data.get("position", post_data.get("seconds", 0.0))
            try:
                position = float(position)
            except Exception:
                position = 0.0
            track_to_play = None
            with state.lock:
                if state.tracks:
                    track_to_play = state.current_track
            if track_to_play is not None:
                start_playback_unsafe(track_to_play, seek_time=position)
            self.send_json(state.to_dict())
            
        elif self.path == "/api/select_interface" or parsed_path == "/api/select_interface":
            dac_id = post_data.get("dac_id", "default")
            track_to_play = None
            curr_pos = 0.0
            with state.lock:
                state.dac_name = dac_id
                state.manual_dac = (dac_id != "default")
                if state.state == "playing":
                    track_to_play = state.current_track
                    curr_pos = state.bytes_played / 176400.0
                elif state.pc_audio_active:
                    if state.pc_audio_socket:
                        try: state.pc_audio_socket.close()
                        except Exception: pass
                    if state.p_play:
                        try: state.p_play.terminate()
                        except Exception: pass
                    time.sleep(0.3)
                    state.pc_audio_thread = threading.Thread(target=pc_audio_receiver_loop, daemon=True)
                    state.pc_audio_thread.start()
            if track_to_play is not None:
                start_playback_unsafe(track_to_play, seek_time=curr_pos)
            self.send_json(state.to_dict())
            
        elif self.path.startswith("/api/pc_audio/enable") or parsed_path == "/api/pc_audio/enable":
            mode_param = "low"
            if isinstance(post_data, dict) and "mode" in post_data:
                mode_param = post_data["mode"]
            
            if 'bluetooth_ctl' in globals() and bluetooth_ctl:
                bluetooth_ctl.disable()
            
            with state.lock:
                stop_playback_unsafe()
                state.bluetooth_active = False
                state.pc_audio_active = True
                state.state = "pc_audio"
                state.pc_audio_thread = threading.Thread(target=pc_audio_receiver_loop, args=(mode_param,), daemon=True)
                state.pc_audio_thread.start()
            self.send_json(state.to_dict())
            
        elif self.path == "/api/pc_audio/disable" or parsed_path == "/api/pc_audio/disable":
            with state.lock:
                state.pc_audio_active = False
                if state.pc_audio_socket:
                    try: state.pc_audio_socket.close()
                    except Exception: pass
                if state.p_play:
                    try: state.p_play.terminate()
                    except Exception: pass
                state.state = "idle"
            self.send_json(state.to_dict())

        elif parsed_path == "/api/modules/toggle":
            mod_id = post_data.get("id")
            enabled_val = post_data.get("enabled")
            ok, target_state = module_manager.toggle_module(mod_id, enabled_val)
            if not module_manager.has_capability("telephony"):
                with state.lock:
                    if getattr(state, "bt_mode", "off") in ("voip", "both"):
                        state.bt_mode = "music"
            self.send_json({"status": "ok" if ok else "error", "id": mod_id, "enabled": target_state})
            return

        elif parsed_path.startswith("/api/modules/") and parsed_path.endswith("/toggle"):
            parts = [p for p in parsed_path.split("/") if p]
            if len(parts) == 4:
                mod_id = parts[2]
                enabled_val = post_data.get("enabled")
                ok, target_state = module_manager.toggle_module(mod_id, enabled_val)
                if not module_manager.has_capability("telephony"):
                    with state.lock:
                        if getattr(state, "bt_mode", "off") in ("voip", "both"):
                            state.bt_mode = "music"
                self.send_json({"status": "ok" if ok else "error", "id": mod_id, "enabled": target_state})
                return
            return

        elif self.path == "/api/modules/voip" or parsed_path == "/api/modules/voip":
            enabled_val = post_data.get("enabled", False)
            ok, target_state = module_manager.toggle_module("cisco_voip", enabled_val)
            if not module_manager.has_capability("telephony"):
                with state.lock:
                    if getattr(state, "bt_mode", "off") in ("voip", "both"):
                        state.bt_mode = "music"
            self.send_json({"status": "ok" if ok else "error", "voip_enabled": target_state})
            return

        elif self.path == "/api/bluetooth/mode" or parsed_path == "/api/bluetooth/mode":
            mode = post_data.get("mode", "both")
            if not module_manager.has_capability("telephony") and mode in ("voip", "both"):
                mode = "music"
            bluetooth_ctl.set_mode(mode)
            self.send_json(state.to_dict())

        elif self.path == "/api/bluetooth/enable" or parsed_path == "/api/bluetooth/enable":
            bluetooth_ctl.enable()
            self.send_json(state.to_dict())

        elif self.path == "/api/bluetooth/disable" or parsed_path == "/api/bluetooth/disable":
            bluetooth_ctl.disable()
            self.send_json(state.to_dict())

        elif self.path == "/api/bluetooth/toggle" or parsed_path == "/api/bluetooth/toggle":
            bluetooth_ctl.toggle()
            self.send_json(state.to_dict())

        elif self.path == "/api/bluetooth/play_pause" or parsed_path == "/api/bluetooth/play_pause":
            bluetooth_ctl.play_pause()
            self.send_json(state.to_dict())

        elif self.path == "/api/bluetooth/play" or parsed_path == "/api/bluetooth/play":
            bluetooth_ctl.play()
            self.send_json(state.to_dict())

        elif self.path == "/api/bluetooth/pause" or parsed_path == "/api/bluetooth/pause":
            bluetooth_ctl.pause()
            self.send_json(state.to_dict())

        elif self.path == "/api/bluetooth/next" or parsed_path == "/api/bluetooth/next":
            bluetooth_ctl.next()
            self.send_json(state.to_dict())

        elif self.path == "/api/bluetooth/prev" or parsed_path == "/api/bluetooth/prev":
            bluetooth_ctl.prev()
            self.send_json(state.to_dict())

        elif self.path == "/api/pause" or parsed_path == "/api/pause":
            with state.lock:
                is_bt = state.bluetooth_active
            if is_bt:
                bluetooth_ctl.pause()
            else:
                with state.lock:
                    if state.state == "playing":
                        if HAS_NATIVE_ENGINE:
                            cdda.cdda_pause()
                            state.state = "paused"
                        else:
                            if state.p_read:
                                try: os.kill(state.p_read.pid, signal.SIGSTOP)
                                except Exception: pass

                            sent_secs = state.bytes_played / 176400.0
                            state.pause_position = max(0.0, sent_secs - state.buffer_ms / 1000.0)

                            if state.p_play:
                                try:
                                    state.p_play.terminate()
                                    state.p_play.wait(timeout=0.3)
                                except Exception:
                                    try: state.p_play.kill()
                                    except Exception: pass
                                state.p_play = None

                            state.state = "paused"
            self.send_json(state.to_dict())
            
        elif self.path == "/api/resume" or parsed_path == "/api/resume":
            with state.lock:
                is_bt = state.bluetooth_active
            if is_bt:
                bluetooth_ctl.play()
            else:
                should_start = False
                track_to_play = 1
                seek_pos = 0.0
                with state.lock:
                    if state.state == "paused":
                        if HAS_NATIVE_ENGINE:
                            cdda.cdda_resume()
                            state.state = "playing"
                        else:
                            if state.p_read:
                                try:
                                    state.p_read.kill()
                                    state.p_read.wait(timeout=0.3)
                                except Exception: pass
                                state.p_read = None
                            should_start = True
                            track_to_play = state.current_track
                            seek_pos = state.pause_position
                if should_start:
                    start_playback_unsafe(track_to_play, seek_time=seek_pos)
            self.send_json(state.to_dict())
            
        elif self.path == "/api/stop" or parsed_path == "/api/stop":
            with state.lock:
                is_bt = state.bluetooth_active
            if is_bt:
                bluetooth_ctl.disable()
            else:
                with state.lock:
                    stop_playback_unsafe()
            self.send_json(state.to_dict())
            
        elif self.path == "/api/prev" or parsed_path == "/api/prev":
            with state.lock:
                is_bt = state.bluetooth_active
            if is_bt:
                bluetooth_ctl.prev()
            else:
                prev_track = None
                with state.lock:
                    pt = state.current_track - 1
                    if any(t["track"] == pt for t in state.tracks):
                        prev_track = pt
                if prev_track is not None:
                    start_playback_unsafe(prev_track)
            self.send_json(state.to_dict())

        elif self.path == "/api/next" or parsed_path == "/api/next":
            with state.lock:
                is_bt = state.bluetooth_active
            if is_bt:
                bluetooth_ctl.next()
            else:
                next_track = None
                with state.lock:
                    nt = state.current_track + 1
                    if any(t["track"] == nt for t in state.tracks):
                        next_track = nt
                if next_track is not None:
                    start_playback_unsafe(next_track)
            self.send_json(state.to_dict())

        elif self.path == "/api/eject" or parsed_path == "/api/eject":
            with state.lock:
                stop_playback_unsafe()
                state.state = "no_disc"
                state.tracks = []
            
            def do_eject():
                subprocess.run(["umount", get_cd_device()], stderr=subprocess.DEVNULL)
                subprocess.run(["eject", "-i", "0", get_cd_device()], stderr=subprocess.DEVNULL)
                subprocess.run(["eject", get_cd_device()], stderr=subprocess.DEVNULL)
            
            threading.Thread(target=do_eject, daemon=True).start()
            self.send_json(state.to_dict())
            
        elif self.path == "/api/load" or parsed_path == "/api/load":
            with state.lock:
                stop_playback_unsafe()
                state.state = "idle"
            
            def do_load():
                subprocess.run(["eject", "-t", get_cd_device()], stderr=subprocess.DEVNULL)
            
            threading.Thread(target=do_load, daemon=True).start()
            self.send_json(state.to_dict())
            
        elif self.path == "/api/refresh" or parsed_path == "/api/refresh":
            def do_refresh():
                with state.lock:
                    state.tracks = get_track_list()
                    if state.tracks:
                        if state.state == "no_disc":
                            state.state = "idle"
                    else:
                        state.state = "no_disc"
            
            threading.Thread(target=do_refresh, daemon=True).start()
            self.send_json(state.to_dict())
            
        elif self.path == "/api/set_buffer" or parsed_path == "/api/set_buffer":
            ms = int(post_data.get("buffer_ms", 2500))
            ms = max(50, min(10000, ms))
            with state.lock:
                state.buffer_ms = ms
                if state.state == "playing":
                    curr_pos = state.bytes_played / 176400.0
                    start_playback_unsafe(state.current_track, seek_time=curr_pos)
            self.send_json(state.to_dict())

        elif parsed_path == "/api/set_release":
            # Apply a selected release's metadata to current state
            # Body: {title, artist, cover_url, tracks: [{track, title, artist, duration, formatted_duration}]}
            new_title = post_data.get("title", "")
            new_artist = post_data.get("artist", "")
            new_cover = post_data.get("cover_url", "")
            new_tracks = post_data.get("tracks", [])
            with state.lock:
                if new_title:
                    state.album_title = new_title
                if new_artist:
                    state.album_artist = new_artist
                if new_cover:
                    state.cover_url = new_cover
                if new_tracks:
                    # Merge: update title/artist of existing tracks by track number
                    existing = {t["track"]: t for t in state.tracks}
                    for nt in new_tracks:
                        tnum = nt.get("track")
                        if tnum in existing:
                            existing[tnum]["title"] = nt.get("title", existing[tnum]["title"])
                            existing[tnum]["artist"] = nt.get("artist", existing[tnum]["artist"])
                    state.tracks = list(existing.values())
            self.send_json(state.to_dict())

        elif module_manager.dispatch_route("POST", parsed_path, post_data, self):
            return

        else:
            self.send_error(404, "Not Found")


def monitor_cd_insertion():
    """Background thread: polls the CD drive every 3 seconds.
    When a disc is inserted, interrupts PC audio/idle mode, reads track list and auto-plays track 1.
    Retries TOC read up to 3 times with 5s gaps to handle slow USB drive spinup under low power."""
    last_disc_present = False
    toc_failed_since = None  # timestamp of first TOC failure for this insertion
    while True:
        time.sleep(3)
        new_last = last_disc_present  # default: keep previous value on exception
        try:
            with state.lock:
                # If already playing, starting or paused a CD, do not query the drive over SCSI/USB
                if state.state in ("playing", "starting", "paused"):
                    last_disc_present = True
                    toc_failed_since = None
                    continue

            disc_present = check_cd_inserted(last_disc_present)
            new_last = disc_present

            with state.lock:
                if not disc_present:
                    toc_failed_since = None
                    if state.state not in ("pc_audio", "bluetooth") and (last_disc_present or state.state != "no_disc"):
                        stop_playback_unsafe()
                        state.tracks = []
                        state.album_title = ""
                        state.album_artist = ""
                        state.cover_url = ""
                        state.state = "no_disc"
                elif disc_present and not last_disc_present:
                    # Fresh insertion: clamp speed immediately and stop any active mode
                    set_drive_speed(2)
                    print("Disco detectado en el lector. Interrumpiendo otros modos y cargando CD...")
                    if state.pc_audio_active:
                        state.pc_audio_active = False
                        if state.pc_audio_socket:
                            try: state.pc_audio_socket.close()
                            except Exception: pass
                        if state.p_play:
                            try: state.p_play.terminate()
                            except Exception: pass
                    state.state = "idle"  # mark as disc present but not yet loaded
                    toc_failed_since = time.time()

            if disc_present and not last_disc_present:
                if state.bluetooth_active and 'bluetooth_ctl' in globals() and bluetooth_ctl:
                    bluetooth_ctl.disable()

            if disc_present and (not last_disc_present or toc_failed_since is not None):
                tracks = get_track_list()
                if tracks:
                    with state.lock:
                        state.tracks = tracks
                        state.current_track = 1
                        state.error_message = ""
                        state.state = "starting"
                    print("Disco cargado correctamente. Iniciando reproducción automática (Pista 1)...", flush=True)
                    toc_failed_since = None
                    start_playback_unsafe(1)
                else:
                    # TOC read failed: will retry on next poll cycle (every 3s)
                    elapsed = time.time() - (toc_failed_since or time.time())
                    if elapsed > 30:
                        # Give up after 30s of retries
                        print("No se pudo leer la tabla de contenidos tras 30s. Disco incompatible o sin datos.")
                        with state.lock:
                            state.state = "no_disc"
                        toc_failed_since = None
                    else:
                        print(f"TOC no leído aún, reintentando en 3s (t+{elapsed:.0f}s)...")

        except Exception as e:
            print(f"Error en monitor CD: {e}")
        finally:
            last_disc_present = new_last


def run_server():
    state.dac_name = detect_usb_dac()
    
    server_address_8000 = ('', 8000)
    server_address_8001 = ('', 8001)
    
    class ReusableThreadingHTTPServer(ThreadingHTTPServer):
        allow_reuse_address = True
        daemon_threads = True

        def handle_error(self, request, client_address):
            ex_type = sys.exc_info()[0]
            if ex_type in (BrokenPipeError, ConnectionResetError):
                return
            super().handle_error(request, client_address)

    httpd_8000 = ReusableThreadingHTTPServer(server_address_8000, CDPlayerHTTPHandler)
    httpd_8001 = ReusableThreadingHTTPServer(server_address_8001, CDPlayerHTTPHandler)
    
    print("CD Player Web Control Server running on port 8000 and 8001 (Visualizer)...")
    
    t_8000 = threading.Thread(target=httpd_8000.serve_forever, daemon=True)
    t_8001 = threading.Thread(target=httpd_8001.serve_forever, daemon=True)
    
    t_8000.start()
    t_8001.start()
    
    # Start background CD insertion monitor (handles initial detection & insertion)
    threading.Thread(target=monitor_cd_insertion, daemon=True).start()

    try:
        t_8000.join()
        t_8001.join()
    except KeyboardInterrupt:
        pass
    finally:
        with state.lock:
            stop_playback_unsafe()
        httpd_8000.server_close()
        httpd_8001.server_close()
        print("Servers stopped.")


if __name__ == "__main__":
    run_server()
