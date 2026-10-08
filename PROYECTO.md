# Technical Specification & Maintenance Notes: Raspberry Pi Hi-Fi CD Player

Reference document covering system architecture, design decisions, diagnostic records, applied corrections, and maintenance instructions.

---

## 1. System Hardware

| Component | Specifications / Configuration |
|---|---|
| Host | Raspberry Pi 3B / 4 (`root@raspberrypi.local`), static IP, direct SSH key access |
| Audio DAC | SMSL SU-1 (USB-Audio card 1 `AUDIO`), connected directly to Raspberry Pi USB port |
| Optical Drive | USB optical drive `/dev/sr0`, connected to an externally powered USB hub |
| Tray Mechanism | Spring-loaded manual tray; software door unlocking via `eject -i 0` |
| ALSA Output | `plughw:AUDIO` (no software `dmix` mixer, bit-perfect direct output) |
| Test Disc | Standard Red Book Audio CD (13 audio tracks, ~49m 47s) |

---

## 2. Software Architecture

- **Language**: Python 3 and native C/C++.
- **HTTP Server**: Built-in Python `http.server` running simultaneously:
  - **Port 8000**: Main web control interface (`index.html` + `app.js` + `style.css`).
  - **Port 8001**: Dedicated fullscreen visualizer (`visualizer.html`).
- **Systemd Service**: `cdplayer.service` located at `/etc/systemd/system/cdplayer.service`.
- **Operating Modes**:
  1. **CD Playback (Bit-Perfect 16-bit / 44.1 kHz)**:
     - Disc detection: polling every 3 seconds.
     - Digital extraction: direct SCSI ioctl audio block reads in `cdda_engine.c`.
     - RAM ring buffer: ~8.2 MB circular buffer (up to 46 seconds of audio).
     - Audio output: direct ALSA stream (`plughw:AUDIO`).
  2. **PC / Android Audio Streaming (UDP / RTP Hi-Res 24-bit / 48 kHz)**:
     - GStreamer / Python RTP receiver on port 3000.
     - 2 MB UDP socket buffer.
     - Streaming from native Android App (Kotlin / Jetpack Compose) and desktop PC sender (`pc_send/pc_audio_sender.py`).
  3. **CD-TEXT Metadata**:
     - TOC reading via `cd-info -C /dev/sr0 --no-device-info` (15s timeout).
     - TOC fallback via `cdparanoia -S 2 -Q` (10s timeout).
     - Online metadata fallback via MusicBrainz / Cover Art Archive.

---

## 3. Diagnostic Log and Bug Fixes

### Bug 1: `UnboundLocalError: cover_url`
- **Diagnosis**: In `get_track_list()`, `cover_url` was referenced at the end but only assigned conditionally. If `cd-info` timed out, the function raised an unhandled exception.
- **Resolution**: Explicit initialization of `cover_url = ""` at the start of `get_track_list()`.

### Bug 2: `cd-info` Timeout
- **Diagnosis**: `cd-info` had a 4-second timeout. At reduced spin speed (2x), optical drives took longer to stabilize and read the TOC.
- **Resolution**: Timeout increased to 15s.

### Bug 3: `NameError: parsed_path` in POST Handler
- **Diagnosis**: `parsed_path` was defined only in `do_GET()` but referenced in `do_POST()` when handling `/api/eject`, `/api/load`, and `/api/refresh`.
- **Resolution**: Added `parsed_path = urlparse(self.path).path` at the beginning of `do_POST()`.

### Bug 4: Missing `/api/prev` Endpoint
- **Diagnosis**: Frontend invoked `POST /api/prev` but only `/api/next` had an implemented route.
- **Resolution**: Implemented handler for `/api/prev` skipping to previous track or restarting current track if elapsed > 3s.

---

## 4. Maintenance and Deployment

To deploy local updates to the Raspberry Pi:
```bash
./deploy.sh
```
