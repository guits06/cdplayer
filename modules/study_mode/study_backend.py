import os
import subprocess
import threading

LOFI_YOUTUBE_URL = "https://youtu.be/rFZHOHl-L8A"
WHITENOISE_YOUTUBE_URL = "https://youtu.be/nMfPqeZjc2c"

class StudyModeBackend:
    def __init__(self, manager, state):
        self.manager = manager
        self.state = state
        self.study_audio_proc = None

    def setup(self):
        self.manager.register_route("GET", "/api/study/status", self.handle_status)
        self.manager.register_route("GET", "/api/study/backgrounds", self.handle_backgrounds)
        self.manager.register_route("POST", "/api/study/enable", self.handle_enable)
        self.manager.register_route("POST", "/api/study/disable", self.handle_disable)
        self.manager.register_route("POST", "/api/study/lofi/start", self.handle_lofi_start)
        self.manager.register_route("POST", "/api/study/lofi/stop", self.handle_lofi_stop)
        self.manager.register_route("POST", "/api/study/whitenoise/start", self.handle_wn_start)
        self.manager.register_route("POST", "/api/study/whitenoise/stop", self.handle_wn_stop)
        self.manager.register_route("POST", "/api/study/volume", self.handle_volume)

    def teardown(self):
        self.stop_stream()
        with self.state.lock:
            self.state.study_mode_active = False
            self.state.study_lofi_active = False
            self.state.study_whitenoise_active = False

    def stop_stream(self):
        with self.state.lock:
            proc = self.study_audio_proc
            self.study_audio_proc = None
            self.state.study_lofi_active = False
            self.state.study_whitenoise_active = False
        if proc:
            try:
                proc.terminate()
                proc.wait(timeout=2)
            except Exception:
                try:
                    proc.kill()
                except Exception:
                    pass
        subprocess.run(["pkill", "-f", "ffmpeg.*rFZHOHl-L8A"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        subprocess.run(["pkill", "-f", "ffmpeg.*nMfPqeZjc2c"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        subprocess.run(["pkill", "-f", "ffmpeg.*googlevideo"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    def start_stream(self, audio_type="lofi"):
        self.stop_stream()
        with self.state.lock:
            cd_was_playing = self.state.state in ("playing", "paused")
        if cd_was_playing:
            try:
                import main
                main.stop_playback_unsafe()
            except Exception:
                pass
            with self.state.lock:
                self.state.state = "idle"

        yt_url = WHITENOISE_YOUTUBE_URL if audio_type == "whitenoise" else LOFI_YOUTUBE_URL
        tag = "[RUIDO-BLANCO]" if audio_type == "whitenoise" else "[LOFI]"
        stream_url = ""
        try:
            cmd = ["/usr/local/bin/yt-dlp", "-g", "-f", "bestaudio", yt_url]
            p = subprocess.run(cmd, capture_output=True, text=True, timeout=20)
            lines = [line.strip() for line in p.stdout.splitlines() if line.strip().startswith("http")]
            if lines:
                stream_url = lines[0]
        except Exception as e:
            print(f"{tag} yt-dlp error: {e}", flush=True)

        if not stream_url:
            with self.state.lock:
                self.state.study_lofi_active = False
                self.state.study_whitenoise_active = False
            return False

        has_audio = os.path.exists("/proc/asound/AUDIO")
        has_head = os.path.exists("/proc/asound/Headphones")
        if has_audio and has_head:
            ff_cmd = [
                "ffmpeg", "-re", "-loglevel", "warning", "-i", stream_url,
                "-filter_complex", "[0:a]asplit=2[a1][a2]",
                "-map", "[a1]", "-f", "alsa", "plughw:CARD=AUDIO,DEV=0",
                "-map", "[a2]", "-f", "alsa", "plughw:CARD=Headphones,DEV=0"
            ]
        elif has_head:
            ff_cmd = ["ffmpeg", "-re", "-loglevel", "warning", "-i", stream_url, "-f", "alsa", "plughw:CARD=Headphones,DEV=0"]
        elif has_audio:
            ff_cmd = ["ffmpeg", "-re", "-loglevel", "warning", "-i", stream_url, "-f", "alsa", "plughw:CARD=AUDIO,DEV=0"]
        else:
            ff_cmd = ["ffmpeg", "-re", "-loglevel", "warning", "-i", stream_url, "-f", "alsa", "default"]

        try:
            proc = subprocess.Popen(ff_cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            self.study_audio_proc = proc
            with self.state.lock:
                if audio_type == "whitenoise":
                    self.state.study_whitenoise_active = True
                    self.state.study_lofi_active = False
                else:
                    self.state.study_lofi_active = True
                    self.state.study_whitenoise_active = False
            return True
        except Exception as e:
            print(f"{tag} Failed to start ffmpeg: {e}", flush=True)
            with self.state.lock:
                self.state.study_lofi_active = False
                self.state.study_whitenoise_active = False
            return False

    def handle_status(self, post_data, handler):
        with self.state.lock:
            resp = {
                "study_mode_active": getattr(self.state, "study_mode_active", False),
                "study_lofi_active": getattr(self.state, "study_lofi_active", False),
                "study_whitenoise_active": getattr(self.state, "study_whitenoise_active", False),
                "aux_volume": getattr(self.state, "aux_volume", 80),
            }
        handler.send_json(resp)

    def handle_backgrounds(self, post_data, handler):
        bg_dir = "/home/guille/cdplayer/study_backgrounds"
        bgs = []
        if os.path.isdir(bg_dir):
            bgs = [f for f in sorted(os.listdir(bg_dir)) if f.lower().endswith(('.jpg', '.jpeg', '.png'))]
        with self.state.lock:
            cur_bg = getattr(self.state, "study_bg_current", "")
        handler.send_json({"backgrounds": bgs, "current": cur_bg})

    def handle_enable(self, post_data, handler):
        with self.state.lock:
            self.state.study_mode_active = True
        handler.send_json(self.state.to_dict())

    def handle_disable(self, post_data, handler):
        with self.state.lock:
            self.state.study_mode_active = False
        self.stop_stream()
        handler.send_json(self.state.to_dict())

    def handle_lofi_start(self, post_data, handler):
        threading.Thread(target=self.start_stream, args=("lofi",), daemon=True).start()
        with self.state.lock:
            self.state.study_lofi_active = True
            self.state.study_whitenoise_active = False
        handler.send_json(self.state.to_dict())

    def handle_lofi_stop(self, post_data, handler):
        self.stop_stream()
        handler.send_json(self.state.to_dict())

    def handle_wn_start(self, post_data, handler):
        threading.Thread(target=self.start_stream, args=("whitenoise",), daemon=True).start()
        with self.state.lock:
            self.state.study_whitenoise_active = True
            self.state.study_lofi_active = False
        handler.send_json(self.state.to_dict())

    def handle_wn_stop(self, post_data, handler):
        self.stop_stream()
        handler.send_json(self.state.to_dict())

    def handle_volume(self, post_data, handler):
        vol = int(post_data.get("volume", 80))
        vol = max(0, min(100, vol))
        with self.state.lock:
            self.state.aux_volume = vol
        def set_vol_bg(v):
            subprocess.run(["amixer", "-c", "Headphones", "set", "PCM", f"{v}%"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            subprocess.run(["amixer", "-c", "0", "set", "PCM", f"{v}%"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        threading.Thread(target=set_vol_bg, args=(vol,), daemon=True).start()
        handler.send_json(self.state.to_dict())

backend_instance = None

def init_module(manager, state):
    global backend_instance
    backend_instance = StudyModeBackend(manager, state)
    backend_instance.setup()

def teardown_module(manager, state):
    global backend_instance
    if backend_instance:
        backend_instance.teardown()
        backend_instance = None
