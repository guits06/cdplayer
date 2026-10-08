# Modular Architecture ("Mods") for CD Player

The `modules/` folder functions like the `mods` directory of a video game.
Each subdirectory is a self-contained module that extends or adapts player functionality without modifying any core player files.

If `modules/` is empty, the CD player and touchscreen interface run normally as a purist standalone player.

---

## Directory Layout

```text
modules/
└── my_module/
    ├── module.json         # [Required] Manifest with metadata and settings
    ├── ui.html             # (Optional) HTML template injected dynamically into web UI
    ├── ui.js               # (Optional) JavaScript logic and event handlers for web UI
    ├── my_service.service  # (Optional) systemd unit managed when enabled/disabled
    ├── panel_plugin.cpp    # (Optional) C++ plugin compiled (.so) for the touch panel
    └── Makefile            # (Optional) Local compilation script
```

---

## Manifest Specification (`module.json`)

```json
{
  "id": "my_module",
  "name": "My Cool Extension",
  "version": "1.0.0",
  "description": "Adds custom features to the ecosystem.",
  "author": "Author Name",
  "icon": "box",
  "capabilities": ["telephony", "custom_feature"],
  "service": "my-service",
  "web": {
    "enabled": true,
    "port": 8088,
    "label": "Web Access"
  },
  "ui": {
    "tab": true,
    "tab_label": "My Tool",
    "html": "ui.html",
    "js": "ui.js"
  }
}
```

### Manifest Fields:
* **`id`** (string): Unique lowercase identifier with no spaces.
* **`name`** (string): Human-readable title for graphical interfaces.
* **`icon`** (string): Icon identifier for tabs or menus.
* **`capabilities`** (array): Capabilities unlocked by the module:
  * `"telephony"`: Unlocks VoIP call handling and Bluetooth HFP profiles in UI.
* **`service`** (string, optional): systemd unit name started or stopped when the user toggles the module.
* **`web`** (object, optional): Embedded web dashboard port and route configuration.
* **`ui`** (object, optional): Files injected into the main web dashboard (`index.html`).

---

## Bundled Modules

### 1. `study_mode` (Study Mode)
* **Purpose**: Converts the player into a distraction-free productivity display.
* **Tools**:
  * **Pomodoro Timer**: Interactive countdown timer for touch panel and web.
  * **Flip Clock**: Minimalist large clock view for desktop displays.
  * **Ambient Audio**: Background LoFi and soothing white noise generator.
* **Backend**: `study_backend.py` (running in the background with independent volume control).

### 2. `cisco_voip` (Cisco 7940/7960 VoIP Gateway)
* **Purpose**: Integrates enterprise desktop IP phones with the Raspberry Pi.
* **Features**:
  * Lightweight RFC 3261 SIP server in C (`cisco_gateway.c`) using < 2 MB of RAM.
  * Zero-cost ITU-T G.711 u-law audio transcoding.
  * Bidirectional bridge with mobile phone Bluetooth HFP audio.
  * Real-time monitoring and XML phone directory service.

---

## Module REST API

The main orchestrator (`main.py`) provides the following endpoints:

* **`GET /api/modules`**: Returns all detected modules, manifests, and active states (`enabled: true/false`).
* **`POST /api/modules/toggle`**:
  ```json
  {
    "id": "study_mode",
    "enabled": true
  }
  ```
  Persists the setting to disk and starts/stops associated systemd services.
