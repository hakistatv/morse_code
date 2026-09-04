# morse_code

ESP-IDF firmware for the Waveshare **ESP32-S3-ePaper-1.54** (V1:
ESP32-S3FH4R2, 4MB Flash / 2MB PSRAM). Stores ten short text messages (up
to 25 characters each) and plays the selected one in Morse code through
the speaker &mdash; a keyed sine whose speed, pitch, volume and
infinite-loop mode are set from the `/audio` web page (defaults 15 wpm /
600 Hz / 100 % / loop off, persisted in NVS) &mdash; on a **BOOT** button
press; pressing again while it plays (or loops) **stops** it, a
**double-press** advances to the next stored message and shows it on the
e-paper panel, and a **~2s hold** toggles infinite-loop mode (an infinity
mark in the panel's top-left corner shows when it is on). The ten slots
are editable from a phone
over a Wi-Fi access point and persist in NVS (slot 1 defaults to `X`). The web UI has the same
Home / Settings page layout as `../qr_code_wallet`, the same default AP
`Hakista` / `hak1sta!`, and the AP SSID/password plus the mDNS device name
are runtime-configurable too.

Same board and driver style as its sibling projects
[`../qr_code_wallet`](../qr_code_wallet) and
[`../photo_album`](../photo_album); the e-paper, BOOT-button, Wi-Fi AP and
web-form pieces are carried over from `../qr_code_wallet`.

## Hardware

- Board: Waveshare ESP32-S3-ePaper-1.54, V1
- Wiki: http://www.waveshare.com/wiki/ESP32-S3-ePaper-1.54
- Docs: https://docs.waveshare.com/ESP32-S3-ePaper-1.54

| Function | Pin(s) |
|---|---|
| E-paper (SPI2) | DC=GPIO10, CS=GPIO11, SCK=GPIO12, MOSI=GPIO13, RST=GPIO9, BUSY=GPIO8, PWR=GPIO6 (**active-LOW**) |
| BOOT button (press = play/stop, double-press = next message, ~2s hold = toggle infinite loop) | GPIO0 |
| Audio codec ES8311 control (I2C0) | SDA=GPIO47, SCL=GPIO48, address 0x30 |
| Audio I2S | MCLK=GPIO14, BCLK=GPIO15, WS=GPIO38, DOUT=GPIO45 |
| Audio codec power enable | GPIO42 (**active-LOW**, same as the e-paper PWR pin) |
| Speaker amplifier enable | GPIO46 (active-high; driven by the ES8311 driver) |

There is **no onboard speaker** — audio comes out the MX1.25 2-pin speaker
header and needs an external speaker plugged in.

The audio pin map, PA-enable pin, and "use MCLK" setting come from
Waveshare's own ESP-IDF audio example for this board
(`02_Example/ESP-IDF/V1/08_Audio_Test`, board id `S3_ePaper_1_54`). The
e-paper pin map matches `components/epd_1in54/epd.h`. See
[`../photo_album/docs/waveshare-esp32-s3-epaper-1.54-reference.md`](../photo_album/docs/waveshare-esp32-s3-epaper-1.54-reference.md)
for the board's full peripheral list.

## Behaviour

1. On boot: init NVS, load the ten message slots (slot 1 = `X` on first
   boot), the selected slot, and the saved Wi-Fi/hostname/audio settings,
   then draw the selected slot's message on the e-paper (3×-scaled bold
   5×7 font, word-wrapped, centred on both axes; a small infinity mark in
   the top-left corner if infinite-loop mode is on), one full refresh.
2. Bring up the ES8311 codec + I2S TX path for playback.
3. Start a Wi-Fi access point, mDNS, and an HTTP server (see below).
4. Watch the BOOT button:
   - **Press** re-reads the selected slot from NVS and plays it as an
     on/off-keyed sine at the configured speed / pitch / volume (default
     15 wpm / 600 Hz / 100 %), then goes quiet. If **Infinite loop** is
     set in `/audio`, the message repeats (with a word gap between reps)
     until the next press.
   - **Press while it is playing (or looping)** stops playback &mdash; it
     ends at the next element boundary (within ~a second), the PA drops
     cleanly, and nothing restarts.
   - **Double-press** stops any playback, then advances the selection to
     the next non-empty slot (wrapping), persists it, and redraws the
     panel &mdash; no new playback.
   - **~2s hold** toggles **Infinite loop** on/off, persists it to NVS
     (same setting as the `/audio` checkbox), and redraws the panel so the
     top-left infinity mark appears or clears. Playback already running is
     left alone; the new mode applies on the next press.
   - A lone press is classified ~350 ms after it is released (that wait is
     how a double-press is ruled out), so a stop takes effect ~350 ms +
     one element after the press. Synthesis runs on its own task, so the
     button stays responsive during playback.

### Web pages

- Connect to the access point **`Hakista`** (WPA2, password `hak1sta!`).
- Browse to **`http://192.168.4.1/`** or **`http://hakista.local/`**.

| Page | What |
|---|---|
| `/` | Home — links to Messages, Audio and Settings |
| `/messages` | The ten slots, each an inline text field with **Save** / **Clear**; the selected slot is marked *playing now*. Saving the selected slot (or the first slot filled when none was selected) redraws the panel. Letters, digits, spaces; up to 25 chars. Save an empty field to clear a slot. |
| `/audio` | Speed (8–40 wpm), tone frequency (300–1200 Hz), volume (0–100), and an **Infinite loop** checkbox (off by default — when on, a BOOT press loops the message until the next press; also toggleable by a ~2s BOOT hold, and shown as a top-left mark on the panel). **Save** persists to NVS; **Save & test** also plays `PARIS` once at the new settings so you can tune by ear. |
| `/settings` | Wi-Fi network name, Wi-Fi password (blank = keep current), and device name (`http://NAME.local/`). Save writes them to NVS and **restarts** the device to apply them. |

Message slots live in NVS namespace `morse` (keys `msg1`..`msg10`, plus
`sel` for the selected slot); the audio settings in namespace `audio_cfg`
(`wpm` / `tone_hz` / `vol` / `loop`). First-boot defaults for the AP SSID/password
and the mDNS hostname are compile-time constants in
[`components/device_settings/device_config.h`](components/device_settings/device_config.h)
(once saved from `/settings` they live in NVS namespace `dev_cfg`); the
audio defaults are in
[`components/audio_settings/audio_config.h`](components/audio_settings/audio_config.h).

### Timing (PARIS standard)

One unit = `1200 / wpm` ms (**80 ms** at the default 15 wpm). dit = 1
unit, dah = 3, gap between elements = 1, between letters = 3, between
words = 7. Each keyed element gets a 4 ms raised-cosine edge ramp so it
doesn't click. At 15 wpm that's roughly **0.9–1.0 s per character**, so a
full 25-char message runs ~22–25 s (faster/slower as WPM is changed).

Audio is synthesized and streamed one Morse element at a time, so
playback RAM is a fixed ~35 KB element buffer no matter how long the
message is — nothing stores rendered PCM.

## Layout

| Path | What |
|---|---|
| `main/morse_code.c` | Wiring: load slots + settings, draw the selected one, init audio + Wi-Fi + mDNS + web, single/double/long BOOT-press callbacks |
| `components/message_store/` | NVS-backed store for the 10 message slots + selected slot (namespace `morse`, keys `msg1`..`msg10`, `sel`); migrates the pre-slots `message` key into slot 1 |
| `components/device_settings/` | NVS-backed Wi-Fi SSID/password + mDNS hostname (namespace `dev_cfg`) — from `../qr_code_wallet` |
| `components/audio_settings/` | NVS-backed WPM / tone frequency / volume / infinite-loop flag (namespace `audio_cfg`); same shape as `device_settings` |
| `components/wifi_ap/` | SoftAP bring-up from `current_ssid`/`current_pass` — from `../qr_code_wallet` |
| `components/mdns_service/` | Advertises `http://<hostname>.local/` — from `../qr_code_wallet` (pulls `espressif/mdns`) |
| `components/web_server/` | Home / Messages / Audio / Settings pages — adapted from `../qr_code_wallet`'s, shared `style.css` |
| `components/boot_button/` | Polls GPIO0, debounces, and classifies each press as single, double, or a ~2s long hold |
| `components/text_display/` | Renders the message string with the `font5x7` bitmap font (3× scale, faux-bold), word-wrapped and centred on both axes; optional top-left infinity mark for infinite-loop mode |
| `components/morse_player/` | ES8311 + I2S TX bring-up and the chunked Morse tone synthesizer; plays on its own task with a non-blocking `play()` + `stop()`, runtime `set_volume()` |
| `components/epd_1in54/` | 1.54" 200×200 e-paper driver — unchanged from `../qr_code_wallet` |

Custom `partitions.csv` grows the app partition to fill 4 MB flash (the
Wi-Fi + HTTP + mbedtls stack outgrows the default 1 MB).

## Build & flash

Requires ESP-IDF **v5.5+** (developed on v6.0.2).

```sh
idf.py set-target esp32s3
idf.py build
idf.py -p /dev/tty.usbmodemXXXX flash monitor
```
