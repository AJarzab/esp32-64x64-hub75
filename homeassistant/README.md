# Home Assistant + Android

The firmware already exposes a small HTTP API, so nothing has to be reflashed to
control the panel from Home Assistant. This folder contains a ready-made
package that turns the API into normal Home Assistant entities:

| File | What it does |
| --- | --- |
| `led_matrix.yaml` | Package: host/PIN helpers, REST sensor, all commands, scripts, `switch`/`number`/`select` entities |
| `dashboard.yaml` | Lovelace view for the phone (built-in cards only, no HACS) |

Everything has been tested against firmware **v3.4** on a 64×64 panel.

---

## 1. Install the package

1. Copy `led_matrix.yaml` to `/config/packages/led_matrix.yaml` on your Home
   Assistant machine (Samba, SSH or the File editor add-on).
2. Make sure `configuration.yaml` loads packages:

   ```yaml
   homeassistant:
     packages: !include_dir_named packages
   ```

3. Restart Home Assistant.

4. Set the address of the board once:
   **Settings → Devices & services → Helpers → “LED matrix host”**
   (default `192.168.0.113`, no `http://`, no trailing slash).

5. If you set a PIN on the panel, type it into the **“LED matrix PIN”** helper.
   The API accepts the PIN as a query parameter (`?pin=…`), so no cookie
   handling is needed. Leave the helper empty if no PIN is set.

The board should get a **static DHCP lease** in your router; otherwise a new IP
after a reboot breaks the integration.

### What you get

- `sensor.led_matrix` — current file, type, FPS, heap, brightness, power limit,
  rotation, playlist flags, schedule state, uptime, RSSI (polled every 30 s)
- `switch.led_matrix_clock`, `.led_matrix_auto_rotate`, `.led_matrix_shuffle`
- `number.led_matrix_brightness` (1–255), `number.led_matrix_power_limit` (20–100 %)
- `select.led_matrix_rotation` (0/90/180/270)
- Scripts: `led_matrix_next`, `led_matrix_previous`, `led_matrix_pause_toggle`,
  `led_matrix_stop`, `led_matrix_play`, `led_matrix_clock_on`,
  `led_matrix_clock_off`, `led_matrix_brightness`, `led_matrix_rotation`,
  `led_matrix_power_limit`, `led_matrix_auto_rotate`, `led_matrix_shuffle`,
  `led_matrix_interval`, `led_matrix_schedule`

Scripts can be called from automations, the dashboard, Assist, widgets and
NFC tags. Actions with fields use `data:`, for example:

```yaml
action: script.led_matrix_play
data:
  file: "logo_64.gif"
```

---

## 2. Dashboard for the phone

1. **Settings → Dashboards → + Add dashboard → New dashboard from scratch.**
2. Open it, press the pencil, then **⋮ → Raw configuration editor**.
3. Paste the contents of `dashboard.yaml` and save.

The view has prev/pause/next/stop buttons, the now-playing block, switches,
sliders for brightness and power limit, a rotation selector and a “play file”
box. It is laid out for a phone screen.

---

## 3. Android specifics

**Home Assistant Companion app** (Play Store or F-Droid) — log in to the same
instance. The dashboard above appears in the sidebar; long-press the app icon
for a shortcut to it.

**Home-screen widgets** — long-press the Android home screen → *Widgets* →
*Home Assistant*. A **Button** widget can run any script (`led_matrix_next`,
`led_matrix_pause_toggle`, `led_matrix_clock_off` …), so you get one-tap
control without opening the app.

**Quick Settings tiles** — Android’s notification shade can host Home
Assistant tiles for scripts/scenes, so the matrix is one swipe away.

**Assist / voice** — the template switches and scripts are normal entities.
Expose them in **Settings → Voice assistants → Expose to Assist**, then
“turn on the LED matrix clock” works with Assist and, if you have Google
Assistant linked to Home Assistant, with “Hey Google …” as well.

**NFC** — write an NFC tag with the `script.led_matrix_clock_on` action
(Companion app → Settings → NFC tags) and stick it next to the panel.

---

## 4. Example automations

Clock at sunset, images again at sunrise:

```yaml
automation:
  - alias: "Matrix: clock at sunset"
    triggers:
      - trigger: sun
        event: sunset
    actions:
      - action: script.led_matrix_clock_on
        data:
          face: 0
          date: 1

  - alias: "Matrix: images at sunrise"
    triggers:
      - trigger: sun
        event: sunrise
    actions:
      - action: script.led_matrix_clock_off
```

Doorbell / motion: upload one 64×64 GIF per event, then play it. Text mode was
removed in v3.4, so “scrolling text” is a pre-rendered GIF now.

```yaml
  - alias: "Matrix: show alert gif"
    triggers:
      - trigger: state
        entity_id: binary_sensor.front_door
        to: "on"
    actions:
      - action: script.led_matrix_play
        data:
          file: "doorbell_64.gif"
      - delay: "00:00:20"
      - action: script.led_matrix_clock_off
```

Night dimming (the panel has no hard “off”, so brightness 1 is used):

```yaml
  - alias: "Matrix: dim at night"
    triggers:
      - trigger: time
        at: "23:30:00"
    actions:
      - action: script.led_matrix_brightness
        data:
          value: 1
```

---

## 5. Notes and limits

- **PIN:** every request in the package appends `&pin=…`; with a PIN set, a
  wrong value returns `401 PIN required`. The PIN is stored in the helper and
  in Home Assistant’s database — fine on a trusted LAN, not on a public one.
- **Polling:** one `GET /status` every 30 s (≈450 bytes). Do not lower it much:
  the board has no PSRAM and the web server is deliberately single-flight.
- **No media streaming:** the firmware no longer serves stored images
  (`/raw` was removed in v3.4), so there is no camera/still-image entity. The
  live preview exists only in the board’s own web UI, in the browser.
- **No MQTT discovery:** adding MQTT would need PubSubClient and another TCP
  socket; the app partition is 96 % full and heap headroom is ~36 KB. REST is
  the lighter path and behaves the same from the phone.
- **Uploads** (new GIFs) still go through the board’s page:
  `http://<board-ip>/` — linked at the bottom of the dashboard.
- **Remote access:** keep the board on the LAN; reach Home Assistant remotely
  through Nabu Casa or a VPN rather than exposing the panel itself.

---

## 6. Troubleshooting

| Symptom | Fix |
| --- | --- |
| `sensor.led_matrix` is `unavailable` | Check the host helper (IP, no `http://`, no trailing slash) and that HA can reach the board: `curl http://<ip>/status` |
| Everything returns 401 | A PIN is set on the board; put it in the PIN helper |
| Commands work but entities do not change | The sensor only refreshes every 30 s; wait or reload the integration |
| Buttons do nothing | Home Assistant 2024.8 or newer is required for `action: perform-action` in the dashboard; on older versions replace it with `action: call-service` + `service:` |
| Wrong colours / rotated image | Rotation is `select.led_matrix_rotation`; the firmware stores it in NVS |
| Panel stays black | Brightness at 1, `/stop` was called, or the on/off schedule put it to sleep — check `sensor.led_matrix` (`asleep`) |
