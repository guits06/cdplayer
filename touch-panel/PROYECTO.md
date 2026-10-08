# Technical Specification: Touchscreen Interface for Raspberry Pi

## 1. Project Overview

The objective is to provide the Raspberry Pi with a responsive touchscreen interface via a 5-inch capacitive IPS display (800x480) or secondary HDMI monitor.

The system runs a bit-perfect CD player (`cdplayer.service`) with USB output to an external DAC (SMSL SU-1). The touch interface must strictly respect **low CPU and RAM usage constraints** to avoid interruptions or buffer under-runs in 16-bit/44.1kHz or 24-bit/48kHz PCM playback.

---

## 2. Technology Evaluation and Architectural Decisions

| Criterion | Chromium Kiosk | Cage + Cog (WPE WebKit) | Native C++/SDL2 |
|---|---|---|---|
| Idle RAM Usage | 350 - 450 MB | 75 - 110 MB | **< 30 MB** |
| CPU Load | 15 - 40% | 5 - 15% | **< 3%** |
| Hardware Acceleration | Partial | Native (Mesa vc4 DRM/KMS) | **Direct DRM/KMS** |
| Responsiveness | Moderate | High | **Instant (60 FPS)** |
| Selection | Deprecated | Supported (Web fallback) | **Primary (cdpanel)** |

---

## 3. Visual Design: Catppuccin Mocha

The user interface uses the **Catppuccin Mocha** dark palette, providing high contrast and pastel accent tones for comfortable viewing:

```css
--ctp-base:      #1e1e2e; /* Main background */
--ctp-mantle:    #181825; /* Header & keyboard background */
--ctp-surface0:  #313244; /* Cards */
--ctp-surface1:  #45475a; /* Borders */
--ctp-text:      #cdd6f4; /* Primary text */
--ctp-lavender:  #b4befe; /* Accent color */
--ctp-green:     #a6e3a1; /* Online / Running state */
--ctp-red:       #f38ba8; /* Offline / Stopped state */
--ctp-yellow:    #f9e2af; /* Idle / Warning state */
```

---

## 4. Connector Pattern (Backend Proxy)

To avoid CORS restrictions, manage credentials securely, and isolate network errors, `panel_server.py` implements an abstract connector class:

```python
class BaseConnector:
    def __init__(self, name, config=None): ...
    def fetch_data(self, endpoint=None, params=None): ...
```

### Connectors:
1. `CDPlayerConnector`: Communicates with `http://localhost:8000/api/status` and dispatches POST commands (`/api/play`, `/api/prev`, `/api/next`, `/api/eject`).
2. `ProxmoxConnector`: Performs authenticated requests using configured API tokens to Proxmox cluster endpoints.
3. `PersonalAPIConnector`: Aggregates external service metrics.
4. `HomeAssistantConnector`: Implements device controls for smart home integration.

---

## 5. Virtual On-Screen Keyboard

The web interface incorporates `keyboard.js`, an embedded touch keyboard:
- Slides up from the bottom when an input field receives focus.
- QWERTY and symbols layout.
- Touch target height of at least 48px.
- Native `wvkbd` fallback available for OS-level virtual input.

---

## 6. Validation Phases

### Phase 1: Virtual Display & Simulation (800x480)
- Validate backend proxy, CD player integration, and swipe navigation.
- Automated deployment via `./touch-panel/deploy.sh`.

### Phase 2: Physical 5-inch Touchscreen
- Configure custom HDMI timings in `/boot/firmware/config.txt`.
- Calibrate capacitive touch USB-HID input.
- Long-run stress test with active bit-perfect audio streaming.
