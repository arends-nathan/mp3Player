# AuraSync MP3 Player

An ESP32-based networked MP3 player with an OLED menu UI, paired with a Python
backend ([`spotify-sync/`](spotify-sync/)) that curates a fresh batch of tracks
from Spotify every night and serves them over HTTP.

The device connects to WiFi, pulls new music from the backend's manifest API
onto a microSD card during idle hours, and plays it back through an I2S
amplifier — all navigable from a 4-button menu on a 128x64 display.

---

## How it works

```
┌──────────────────────┐         WiFi / HTTP        ┌─────────────────────────┐
│  spotify-sync (PC /   │  ───────────────────────▶  │  ESP32 firmware         │
│  Docker backend)      │   GET /manifest            │                         │
│                       │   GET /files/<name>.mp3    │  • downloads new tracks │
│  • curates Spotify    │                            │  • stores on SD card    │
│  • downloads MP3s     │                            │  • decodes + plays I2S  │
│  • serves manifest    │                            │  • OLED menu UI         │
└──────────────────────┘                            └─────────────────────────┘
```

Each night the backend rebuilds its library and exposes it via a manifest
(file names, sizes, SHA-256 hashes). The firmware fetches the manifest and
downloads anything it doesn't already have onto the SD card.

---

## Firmware architecture

The firmware is split into focused ESP-IDF components. [`main/main.c`](main/main.c)
is a thin composition root that wires them together.

| Component | Responsibility |
|-----------|----------------|
| [`audio_driver`](components/audio_driver/) | I2S output + Helix MP3 decoding. Runs its own playback task on **core 0**. |
| [`music_library`](components/music_library/) | Mounts the SD card (FAT over SPI), scans for `.mp3` files, drives playback. |
| [`sync_manager`](components/sync_manager/) | WiFi association + nightly manifest sync. Network task pinned to **core 0**. |
| [`buttons`](components/buttons/) | Interrupt-driven GPIO input with debounce and long-press detection. |
| [`ui`](components/ui/) | OLED rendering, menu state machine, and UI task on **core 1**. |
| [`wifi_download`](components/wifi_download/) | WiFi station bring-up, scanning, HTTP download / manifest parsing, NVS credentials. |
| [`bt_manager`](components/bt_manager/) | Bluetooth (Bluedroid classic) enable/disable, GAP device discovery, and A2DP audio source. |
| [`u8g2`](components/u8g2/), [`u8g2-hal-esp-idf`](components/u8g2-hal-esp-idf/) | Display library + ESP-IDF HAL (git submodules). |

### Dual-core layout

- **Core 0 (PRO_CPU):** audio decode task + WiFi/network task. The nightly sync
  is idle-time only, so it never competes with playback. Bluetooth is
  automatically suspended during a sync (the radios share RAM and never need to
  run together) and restored afterwards.
- **Core 1 (APP_CPU):** UI rendering at ~30 FPS.

---

## Menu

After a boot splash the player opens straight to **Now Playing**. The four
buttons (Up / Down / Select / Back) are interrupt-driven, and **holding Select**
opens the menu from any playback screen.

- **Now Playing** (default screen) – scrolling title, state, volume, and a
  progress bar. Tap Select to play/pause; Up/Down skip tracks; hold Select to
  open the menu.
- **Library** – browse and play tracks found on the SD card.
- **Volume** – adjust output level in 5% steps.
- **WiFi** – settings hub (see below).
- **Bluetooth** – settings hub (see below).
- **Info** – firmware version, free heap, track count, uptime.

In the menu, Back returns to Now Playing.

### WiFi settings

A small hub showing the current SSID, connection state and IP, with two actions:

- **Scan networks** – scans for nearby APs and lists them by signal strength
  (a leading `#` marks a secured network). Select one to join it; secured
  networks open an on-screen keyboard (Up/Down pick a character, Select adds it,
  Back deletes, **hold Select** to confirm and connect). Credentials are saved
  to NVS and reused on the next boot.
- **Sync now** – trigger a manifest sync on demand.

### Bluetooth settings

The hub shows the remembered device and live connection status, with two actions:

- **Bluetooth: ON/OFF** – powers the Bluetooth stack up or down. Turning it on
  automatically reconnects to the remembered device.
- **Scan devices** – runs a GAP inquiry and lists nearby devices; select one to
  remember it and connect. Pairing uses "just works" SSP (no keypad needed).

Once a sink is connected, decoded audio is routed over **A2DP** to the speaker
or headphones instead of the local I2S amplifier; disconnecting (or turning
Bluetooth off) restores local output automatically.

> Bluetooth requires classic BT + A2DP enabled in `menuconfig` (already set in
> `sdkconfig.defaults`) and a 4 MB flash module. The A2DP source streams
> 44.1 kHz 16-bit stereo, matching the backend's MP3 output.

---

## Hardware wiring

Default GPIO assignments (override in `menuconfig` where noted):

### Display — SSD1306 128x64 OLED (I2C)
| Signal | GPIO |
|--------|------|
| SDA | 21 |
| SCL | 22 |

### Audio — I2S amplifier (e.g. MAX98357A)
| Signal | GPIO | Config key |
|--------|------|------------|
| BCLK | 27 | `CONFIG_AUDIO_I2S_BCLK_PIN` |
| LRCLK / WS | 26 | `CONFIG_AUDIO_I2S_LRCLK_PIN` |
| DOUT / DIN | 25 | `CONFIG_AUDIO_I2S_DOUT_PIN` |

### Storage — microSD card over SPI (WWZMDiB Micro SD / TF adapter)
| Signal | GPIO |
|--------|------|
| MOSI | 23 |
| MISO | 19 |
| CLK | 18 |
| CS | 5 |

> The SD adapter is a 3.3 V board — power it from the ESP32's **3.3 V** rail.
> Pins are defined at the top of [`music_library.c`](components/music_library/music_library.c).

### Buttons (active-low, internal pull-ups)
| Button | GPIO |
|--------|------|
| Up | 32 |
| Down | 33 |
| Select | 14 |
| Back | 13 |

---

## Building the firmware

This is an [ESP-IDF](https://docs.espressif.com/projects/esp-idf/en/latest/get-started/index.html)
project (v5.0+).

1. **Clone with submodules** (the u8g2 display libraries are submodules):
   ```bash
   git submodule update --init --recursive
   ```

2. **Configure WiFi and the backend URL:**
   ```bash
   idf.py menuconfig
   ```
   Under *Example Configuration*, set:
   - `WiFi SSID` / `WiFi Password`
   - `MP3 sync backend base URL` (e.g. `http://192.168.1.50:8000`)
   - `Local music directory` (defaults to `/sdcard`)

   The audio I2S pins live under *Audio Driver Configuration*.

3. **Build, flash, and monitor:**
   ```bash
   idf.py build
   idf.py -p <PORT> flash monitor
   ```

The Helix MP3 decoder ([`chmorgan/esp-libhelix-mp3`](https://components.espressif.com/components/chmorgan/esp-libhelix-mp3))
is pulled automatically by the component manager.

---

## Backend (spotify-sync)

The companion Python service curates and serves the music. It runs locally or in
Docker and exposes the manifest API the firmware syncs against.

See [`spotify-sync/README.md`](spotify-sync/README.md) for full setup, but in short:

```bash
cd spotify-sync
pip install -r requirements.txt
cp config.example.json config.json   # add your Spotify credentials
python main.py            # daily curator + downloader
python api.py --port 8000 # serve files to the device
```

API endpoints consumed by the firmware:
- `GET /manifest` — file names, sizes, timestamps, SHA-256 hashes.
- `GET /files/<name>` — download a single MP3.
- `GET /health` — readiness check.

---

## License

Firmware template portions are Public Domain / CC0. See [`LICENSE`](LICENSE).
The bundled Helix MP3 decoder and u8g2 library retain their respective licenses.
