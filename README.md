# LED Matrix Web Player — ESP32-WROOM + 64×64 P3 HUB75

A self-contained web player for a 64×64 P3 HUB75 LED panel driven by a plain
**ESP32-WROOM-32 (4 MB flash, no PSRAM)**. The ESP32 hosts the whole app: the
web UI, the GIF/image pipeline, the playlist, an NTP clock and a scheduler.
Nothing else is needed — no PC, no cloud, no phone app. Open the page in any
browser on your LAN and the panel follows.

> Tested on an ESP32-WROOM-32 DevKit with a 64×64 P3 panel, Arduino-ESP32
> 3.3.12. Firmware size is ~1.27 MB (96% of the stock 1.31 MB app partition),
> so the stock partition table must be kept.

## Quick start

1. Wire the panel (table below) and power it from a 5 V supply, not the ESP32.
2. Put your Wi-Fi credentials at the top of `esp32_p3_lib.ino`.
3. Install the ESP32 core and the five libraries listed under
   [Software requirements](#software-requirements).
4. Select **ESP32 Dev Module** + the stock 4 MB partition scheme, then Upload.
5. Open `http://ledmatrix.local/` (or the IP printed at 115200 baud) and drop an
   image on the page.

---

## Pinout

```
R1  → GPIO 25
G1  → GPIO 26
B1  → GPIO 27
R2  → GPIO 14
G2  → GPIO 32   (default 12 is a strapping pin)
B2  → GPIO 33   (default 13)
E   → GPIO 18   (mandatory for 64×64)
A   → GPIO 23
B   → GPIO 19
C   → GPIO 22   (default 5 is a strapping pin)
D   → GPIO 17
CLK → GPIO 16   (via 100–220 Ω series resistor)
LAT → GPIO 4
OE  → GPIO 13   (default 15 is a strapping pin)
GND → GND (pins 4, 8, 12, 16 on HUB75)
```

---

## Features

- **Upload from the browser** — drag & drop animated GIFs, JPEG, PNG, WebP or
  BMP files. Decoding, cropping, scaling to 64×64 and re-encoding all happen in
  the browser; the ESP32 only stores and plays the result.
- **Animated GIF playback** — loops indefinitely, pause/resume, next/previous.
- **Playlist** — reorder, per-file hold time, enable/disable, auto-rotate and
  shuffle. Settings and the last played file survive a reboot (NVS).
- **Clock** — digital and analog faces, date, NTP sync, configurable timezone.
- **On/off schedule** — daily window (overnight windows such as 18:00 → 02:00
  are supported).
- **Live status** — WebSocket pushes FPS, heap, RSSI, uptime and the current
  file once per second.
- **Brightness and power limit** — dim the panel or cap current draw if your
  supply is marginal.
- **Panel rotation** — 0°, 90°, 180° or 270°, saved in NVS.
- **8-bit colour** — 256 levels per channel, no banding on gradients.
- **PIN protection** — optional 4–16 character PIN, cookie session.
- **Wi-Fi setup without reflashing** — change SSID/password from the UI; falls
  back to a captive-portal AP if the network is unavailable.
- **mDNS** — reachable at `http://ledmatrix.local` as well as by IP.
- **Diagnostics** — `/diag` reports heap, reset reason and boot stage, which is
  what you need when something goes wrong.

The page never downloads the stored media. The file list shows names and sizes,
and the live 64×64 preview is rendered locally in the browser from the file you
are about to upload — that is what keeps a 4 MB, PSRAM-less board responsive.

---

## Hardware

| Part | Notes |
| --- | --- |
| ESP32-WROOM-32 dev board | 4 MB flash, **no PSRAM required** |
| 64×64 P3 HUB75 panel | 1/32 scan (the driver default for 64×64) |
| 5 V power supply | 3 A minimum, 4 A recommended at full brightness |
| Jumper wires | 14 signals + GND; keep them short |

### Wiring

The pins below are what the sketch uses (`CONFIGURATION` block at the top of
`esp32_p3_lib.ino`). Change them there if your board is wired differently.

| HUB75 pin | ESP32 GPIO | Notes |
| --- | --- | --- |
| R1 | 25 | |
| G1 | 26 | |
| B1 | 27 | |
| R2 | 14 | |
| G2 | 32 | default 12 is a strapping pin |
| B2 | 33 | default 13 |
| A | 23 | |
| B | 19 | |
| C | 22 | default 5 is a strapping pin |
| D | 17 | |
| E | 18 | mandatory for 64×64 |
| CLK | 16 | via a 100–220 Ω series resistor |
| LAT | 4 | |
| OE | 13 | default 15 is a strapping pin |
| GND | GND | pins 4, 8, 12, 16 on the HUB75 connector |

### Power

- **Do not power the panel from the ESP32's 5 V pin.** Feed the panel from the
  external 5 V supply and connect the supply ground to the ESP32 ground.
- A 64×64 panel can pull 2–3 A on bright white content. Brownouts show up as
  random reboots or a flickering panel; fix the supply before blaming the
  firmware, or lower **Power limit** in the UI.
- The panel inputs are 3.3 V compatible in practice; use a level shifter if
  yours is not.

---

## Software requirements

- **Arduino IDE 2.x** (or `arduino-cli`).
- **ESP32 board package** `esp32` by Espressif, version **3.3.x**
  (tested with 3.3.12).
- Libraries (Library Manager names and versions used for the tested build):

| Library | Version | Author |
| --- | --- | --- |
| ESP Async WebServer | 3.12.1 | ESP32Async |
| Async TCP | 3.5.0 | ESP32Async |
| ESP32 HUB75 LED MATRIX PANEL DMA Display | 3.1.0 | MrCodetastic |
| AnimatedGIF | 2.2.0 | Larry Bank |
| JPEGDEC | 1.8.5 | Larry Bank |

`LittleFS`, `Preferences`, `ESPmDNS`, `DNSServer` and `WiFi` ship with the
ESP32 core.

### Board settings

| Setting | Value |
| --- | --- |
| Board | **ESP32 Dev Module** |
| Flash size | 4 MB |
| Partition scheme | **Default 4 MB with spiffs (1.2 MB APP / 1.5 MB SPIFFS)** |
| CPU frequency | 240 MHz |
| Upload speed | 921600 (drop to 115200 if uploads fail) |

The app uses about **1.27 MB of the 1.31 MB** application partition. Do not
switch to a partition scheme with a smaller app slot — the build will not fit.

---

## Build and flash

1. Copy this folder into your Arduino sketchbook, e.g.
   `Documents/Arduino/esp32_p3_lib/`. The folder name must match the `.ino`
   file name (`esp32_p3_lib.ino`).
2. Open `esp32_p3_lib.ino` and set your network at the top:

   ```cpp
   const char* SSID = "your-ssid";
   const char* PASS = "your-password";
   ```

   These are only defaults: once you save credentials from the web UI they are
   stored in NVS and the constants are ignored. Still, do not commit real
   credentials.
3. Install the libraries listed above, select **ESP32 Dev Module** and your
   port, then press **Upload**.
4. With `arduino-cli`:

   ```bash
   arduino-cli compile --fqbn esp32:esp32:esp32 .
   arduino-cli upload  -p COM5 --fqbn esp32:esp32:esp32 .
   ```

   (On Linux/macOS the port is something like `/dev/ttyUSB0`.)

If the upload stops with `Wrong boot mode detected (0x13)`, the board's
auto-reset circuit did not pull GPIO0 low. Hold **BOOT**, tap **EN**, release
**BOOT**, then upload again. If that still fails (some clone boards have no
working auto-reset at all), connect **IO0 to GND** with a jumper, tap **EN**,
and keep the jumper in place until the upload finishes. See
[Troubleshooting](#troubleshooting).

---

## First boot and connecting

1. Open the serial monitor at **115200 baud**. The boot log ends with the IP:

   ```
   [WIFI] IP: 192.168.0.x
   [MDNS] http://ledmatrix.local/
   [WEB] ready on http://192.168.0.x/  (AP: -)
   ```

2. Open `http://<ip>/` or `http://ledmatrix.local/` in a browser on the same
   network.

3. If the board cannot join the configured network it starts an access point:

   - SSID: `LEDMatrix-XXXX` (last bytes of the MAC)
   - Password: `ledmatrix`
   - Open `http://192.168.4.1/`, go to **Wi-Fi** and enter your network.

   The board reboots with the new credentials. This also works as a recovery
   path if you move house or change your router.

4. If a PIN has been set, the page asks for it before doing anything else.
   The PIN is stored in NVS and can be removed from the same panel.

---

## Using the web UI

The page is one file served from flash; it works on desktop and mobile.

- **Upload** — choose or drop images/GIFs. You can crop and zoom the source
  before uploading; the browser produces a 64×64 file named
  `<original>_64.gif` (static images may be sent as JPEG). Files larger than
  1 MB are refused, and uploads that would leave the filesystem nearly full
  are rejected before they start.
- **Live preview** — the 64×64 preview shows exactly what will be stored,
  including animation and the crop, before you upload.
- **Preview & transport** — play/pause, next/previous, auto-rotate, shuffle,
  and the current file with its delay.
- **Playlist** — every stored file with its hold time and an enable toggle;
  move entries with the up/down arrows.
- **Clock** — digital/analog, show date, timezone (POSIX TZ string), and the
  daily on/off window.
- **Panel & color** — power limit, rotation (0/90/180/270) and
  fade-between-files.
- **Brightness** — 1–255 slider; the value is persisted.
- **Files** — name, size, rename, delete, delete all, free-space display.
- **Tools** — PIN, Wi-Fi change, config export, diagnostics, firmware version.

The file list deliberately has no thumbnails: streaming full-size images to the
browser was the main source of heap exhaustion on a board without PSRAM. The
playlist and preview give you the same information without transferring media.

---

## Memory, performance and design limits

This build targets the hardest configuration: 4 MB flash and **no PSRAM**.

- **Single framebuffer.** A second 64×64 framebuffer costs ~40 KB of internal
  RAM and fragments the heap until the web server can no longer allocate its
  send buffer. `double_buff = false` is therefore deliberate.
- **8-bit colour depth.** Text mode and the ambient effect generators were
  removed so the framebuffer could grow from 6-bit to full 8-bit (256 levels
  per channel, ~13 KB more RAM than 6-bit).
- **No media bytes over HTTP.** The UI never downloads stored images; the page
  is one download at a time. Extra page requests are dropped at the connection
  level instead of allocating a response the board does not have memory for.
- **Uploads are reaped.** A connection that dies mid-upload leaves a half
  file; it is closed and removed after 20 s, and playback refuses to open a
  file that is still being written (LittleFS has very few file descriptors).
- **Watchdog.** If a client's socket stops draining for 40 s the async task is
  considered wedged and the board reboots into a clean state.
- **Crash-loop guard.** Three fast restarts put the board into safe mode: no
  boot QR, no auto-resume, web UI always up.
- **Flash budget.** The UI is ~64 KB of HTML/CSS/JS stored in program memory
  and streamed out, never copied to RAM. Adding large assets will not fit.

Typical idle memory with 8-bit colour: **~75–80 KB free heap** with a
~35–40 KB largest block. The largest free block, not the total, is what limits
the web server.

### Panel behaviour worth knowing

- GIF playback is limited by decoding speed, not by the panel. Large GIFs
  (100 KB+) may play slower than their nominal frame delay.
- Fades are done by ramping panel brightness; with a single framebuffer there
  is no cross-fade between two images.
- The clock overrides the playlist until you stop it.
- Rotation applies to everything drawn on the panel (images, clock, QR code).

---

## HTTP API

All endpoints are `GET` and return `text/plain` or JSON, except `/upload`
(`POST multipart/form-data`, field name `file`). If a PIN is set, call
`/auth?pin=...` first; the session cookie is then used by everything else.

| Endpoint | Parameters | Purpose |
| --- | --- | --- |
| `/` | – | Web UI (HTML) |
| `/upload` | multipart `file` | Store a 64×64 image/GIF (max 1 MB) |
| `/files-json` | – | List files with size and playlist metadata |
| `/play` | `f` | Play a stored file |
| `/stop` | – | Stop playback |
| `/ctl` | `a`=toggle\|pause\|resume\|next\|prev\|auto\|shuffle\|interval, `v`=0\|1 or seconds | Transport, rotation, interval |
| `/clock` | `face`=0\|1, `date`=0\|1, `tz`=POSIX TZ; `off=1` | Clock |
| `/schedule` | `on`=0\|1, `from`=HH:MM, `to`=HH:MM | Daily on/off window |
| `/bright` | `v`=1–255 | Brightness |
| `/panel` | `pwr`=20–100, `fade`=0\|1, `rot`=0–3, `qr`=0\|1 | Power limit, fades, rotation, boot QR |
| `/pl` | `f`, plus `up` \| `down` \| `en`=0\|1 \| `dur`=seconds | Playlist edit |
| `/del` | `f` | Delete one file |
| `/delall` | – | Delete all files |
| `/rename` | `f`, `to` | Rename a file |
| `/pin` | `set`=PIN or `off=1` | Set/clear PIN |
| `/auth` | `pin` | Start a session |
| `/authstate` | – | `{"token":...}` for the WebSocket |
| `/wifi` | `ssid`, `pass` | Save credentials and reboot |
| `/config` | – | Current settings as JSON |
| `/status` | – | Full live state (polled by the UI) |
| `/diag` | – | Heap, reset reason, boot stage, uptime |
| `/qr` | – | URL to encode as the boot QR code |
| `/reset` | – | Erase settings and reboot |
| `/ws` | WebSocket | Live status; send the token from `/authstate` first |

Examples:

```bash
curl "http://192.168.0.50/play?f=logo_64.gif"
curl "http://192.168.0.50/panel?rot=1"
curl "http://192.168.0.50/bright?v=120"
curl "http://192.168.0.50/diag"
curl -F "file=@logo_64.gif" "http://192.168.0.50/upload"
```

---

## Troubleshooting

| Symptom | Cause / fix |
| --- | --- |
| Upload fails: `Wrong boot mode detected (0x13)` | Auto-reset did not enter download mode. Hold **BOOT**, tap **EN**, release **BOOT**, upload again. If it persists, jumper **IO0–GND**, tap **EN** and keep the jumper until flashing ends. |
| Upload dies part-way, board then logs `invalid segment length` / `No bootable app partitions` | The application partition was overwritten partially. Reflash — the bootloader is fine. |
| `503 Busy, retrying…` when opening several tabs | Admission control. One page transfer at a time is deliberate. |
| `ledmatrix.local` does not resolve | mDNS is not always available on Windows. Use the IP shown in the serial log. |
| Panel stays black | Check the E pin and the 1/32 scan type, the 5 V supply and ground, and brightness (try `/bright?v=128`). If the board restarted three times quickly it is in safe mode — power-cycle once. |
| Image is sideways | Set **Panel & color → Rotation** to 0/90/180/270. Rotation is stored in NVS. |
| Random reboots on bright content | Power supply too weak. Lower **Power limit**, use a 4 A supply, shorten the power leads. |
| FPS drops with many files | GIF decoding is CPU-bound; smaller/shorter GIFs and fewer files help. |
| Settings keep reverting | `/reset` erases NVS. Otherwise settings persist automatically a few seconds after a change. |
| Out of space | The filesystem is ~1.4 MB. Delete files you no longer play. |

Serial log at 115200 baud is the first place to look; boot stage numbers are
also exposed by `/diag` (`stage`, `boots`, `reset`, `heap`, `heapMin`,
`heapLargest`).

---

## Security notes

- The UI is plain HTTP. The optional PIN keeps casual visitors out; it is not
  a substitute for a trusted LAN.
- Do not port-forward the device to the internet.
- Wi-Fi credentials are stored in NVS on the board. The default AP password is
  `ledmatrix`; change it in the sketch before using the fallback AP in public.
- The sketch ships with placeholder credentials; never commit your own.

---

## Repository layout

```
esp32_p3_lib/
├── esp32_p3_lib.ino   firmware + embedded web UI (HTML/CSS/JS in PROGMEM)
├── qr_local.c         QR encoder used for the boot screen
├── qr_local.h
├── LICENSE
└── README.md
```

`build/` holds generated binaries and is ignored by `.gitignore`; never commit
it (the merged image alone is 4 MB). The old `build/` binaries were removed
from the repository in v3.4 — build from source instead of flashing stale
images.

---

## Contributing

Issues and pull requests are welcome. Please include:

- board and panel model, ESP32 core version and library versions,
- the serial log from boot to the failure,
- the output of `/diag` while the problem is visible.

For memory-related changes, keep an eye on `heap` and `heapLargest` in
`/diag` after a page load and a few GIF frames: the largest free block is what
limits the server, not the total heap.

---

## License

GNU General Public License v3.0 — see [`LICENSE`](LICENSE).

## Credits

- [ESP32-HUB75-MatrixPanel-DMA](https://github.com/mrcodetastic/ESP32-HUB75-MatrixPanel-DMA) — panel driver
- [ESPAsyncWebServer](https://github.com/ESP32Async/ESPAsyncWebServer) and
  [AsyncTCP](https://github.com/ESP32Async/AsyncTCP) — web server and sockets
- [AnimatedGIF](https://github.com/bitbank2/AnimatedGIF) and
  [JPEGDEC](https://github.com/bitbank2/JPEGDEC) — decoders
- [QRCode](https://github.com/ricmoo/QRCode) — boot QR code
