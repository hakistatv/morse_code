# morse_code

ESP-IDF firmware for the Waveshare **ESP32-S3-ePaper-1.54** (V1: ESP32-S3FH4R2,
4MB Flash / 2MB PSRAM). Stores up to ten short text messages in flash and plays
the selected one in Morse code through the speaker -- an on/off-keyed sine whose
speed, pitch, volume and infinite-loop mode are set from a web form over the
board's own Wi-Fi access point, with the current message shown on the onboard
e-paper panel.

## Hardware

- Board: Waveshare ESP32-S3-ePaper-1.54, V1
- Docs: https://docs.waveshare.com/ESP32-S3-ePaper-1.54

| Function                                                                        | Pin(s) |
|--------------------------------------------------------------------------------- |--------|
| E-paper (SPI2)                                                                  | DC=GPIO10, CS=GPIO11, SCK=GPIO12, MOSI=GPIO13, RST=GPIO9, BUSY=GPIO8, PWR=GPIO6 (**active-LOW**) |
| BOOT button (press = play/stop, double-press = next message, ~2s hold = toggle loop) | GPIO0 |
| Audio codec ES8311 control (I2C0)                                               | SDA=GPIO47, SCL=GPIO48, address 0x30 |
| Audio I2S                                                                       | MCLK=GPIO14, BCLK=GPIO15, WS=GPIO38, DOUT=GPIO45 |
| Audio codec power enable                                                        | GPIO42 (**active-LOW**, same as the e-paper PWR pin) |
| Speaker amplifier enable                                                        | GPIO46 (active-high; driven by the ES8311 codec driver) |

There is **no onboard speaker** -- audio comes out the MX1.25 2-pin speaker
header, so an external speaker has to be plugged in to hear anything.

See [`components/epd_1in54/epd.h`](components/epd_1in54/epd.h) for the e-paper
pin map as implemented, and
[`components/morse_player/morse_player.c`](components/morse_player/morse_player.c)
for the audio pin map and ES8311 setup (both taken from Waveshare's own ESP-IDF
example for this board).

## Features

- Boots, draws the selected message on the e-paper, and starts a Wi-Fi access
  point (SoftAP) -- connect directly to the board, no router needed
- Starts an HTTP server: a Home page at `/` linking to Messages (`/messages`),
  Audio (`/audio`), and Settings (`/settings`)
- Messages page: ten persistent text slots, up to 25 characters each (letters,
  digits, spaces), each an inline field with a Save and a Clear button. Slot 1
  defaults to `X`. Saving the selected slot -- or the first slot filled when
  none was selected -- redraws the panel. Stored in NVS (`message_store.c`,
  namespace `morse`), survives reboot/power loss
- Draws the message with a 3x-scaled bold 5x7 bitmap font, word-wrapped and
  centred on both axes (`text_display.c`); a small infinity mark is added in the
  top-left corner while infinite-loop mode is on
- Plays the selected slot in Morse code through the ES8311 codec on a **BOOT**
  press: an on/off-keyed sine at the configured speed, pitch and volume,
  synthesized and streamed one element at a time on its own task
  (`morse_player.c`)
- **BOOT button** (`boot_button.c` -- polled and debounced, no ISR):
  - **single press** -- play the selected message, or stop it if it is already
    playing
  - **double press** -- advance the selection to the next non-empty slot
    (wrapping), persist it, and redraw the panel; no playback
  - **~2s hold** -- toggle infinite-loop mode. When it is on, a press repeats
    the message (a word gap between reps) until the next press, and the panel
    shows the corner infinity mark. Playback already running is left alone; the
    new mode applies on the next press
- Audio page: set Morse speed (8-40 wpm), tone frequency (300-1200 Hz), output
  volume (0-100), and the infinite-loop flag from the browser -- no reflash
  needed. Save persists to NVS (`audio_settings.c`, namespace `audio_cfg`);
  Save & test also plays `PARIS` once at the new settings so you can tune by ear
- Settings page: change the Wi-Fi SSID, password, and device (mDNS) name from
  the browser -- no reflash needed. Saving restarts the board so the new
  settings take effect
- Advertises itself over mDNS so the AP can be reached by hostname instead of IP

## Morse timing (PARIS standard)

One unit is `1200 / wpm` milliseconds (80 ms at the default 15 wpm). A dit is 1
unit, a dah is 3; the gap between elements within a letter is 1 unit, between
letters 3, between words 7. Each keyed element gets a 4 ms raised-cosine edge
ramp so it doesn't click on and off. At 15 wpm that works out to roughly
0.9-1.0 s per character, so a full 25-character message runs about 22-25 s
(faster or slower as the speed is changed).

Audio is synthesized and streamed one Morse element at a time, so playback RAM
is a fixed ~35 KB buffer regardless of message length -- nothing holds rendered
PCM.

## Configuring

### Wi-Fi and device name

There are two ways to set the Wi-Fi SSID/password and device (mDNS) name:

1. **Before you build** -- edit the defaults in
   [`components/device_settings/device_config.h`](components/device_settings/device_config.h):

   ```c
   #define WIFI_AP_SSID     "Hakista"
   #define WIFI_AP_PASS     "hak1sta!"
   #define WIFI_AP_CHANNEL  1
   #define WIFI_AP_MAX_CONN 4

   #define MDNS_HOSTNAME    "hakista"
   ```

   Edit that file, then build/flash as usual -- no menuconfig step needed.

2. **After flashing, from the browser** -- open `/settings` on the device (see
   below) and change the SSID, password, and device name there. These are saved
   to NVS flash and take priority over `device_config.h` from then on (survives
   reflashing the app, but not `idf.py erase-flash`). Saving restarts the board
   immediately. Leave the password field blank to keep the current password
   unchanged.

### Audio

The first-boot Morse speed, tone pitch, volume and infinite-loop default are
compile-time constants in
[`components/audio_settings/audio_config.h`](components/audio_settings/audio_config.h):

```c
#define AUDIO_DEFAULT_WPM      15
#define AUDIO_DEFAULT_TONE_HZ  600
#define AUDIO_DEFAULT_VOLUME   100
#define AUDIO_DEFAULT_LOOP     0
```

After the first boot these are runtime-editable from the `/audio` page and
stored in NVS (namespace `audio_cfg`), taking priority from then on (same
survival rules as the Wi-Fi settings above). The accepted ranges the web form
validates against are in `audio_settings.h`.

Note: message text and all settings are stored **unencrypted** in NVS -- fine
for a local device you control, but don't expose this AP's endpoints beyond
that.

## Building & flashing

This project uses ESP-IDF v6.0.2. Activate the toolchain, then use `idf.py` as
normal:

```sh
source ~/.espressif/tools/activate_idf_v6.0.2.sh
idf.py set-target esp32s3   # first time only
idf.py build
idf.py -p /dev/cu.usbmodem101 flash monitor
```

(To exit the serial monitor, press `Ctrl-]`.)

### Notes for this board

- Flash over UART, not JTAG (`idf.flashType` in `.vscode/settings.json` if using
  the VS Code ESP-IDF extension) -- this board's native USB-JTAG interface can
  leave the chip stuck in bootloader/download mode with the JTAG flash path.
- If a flash/monitor gets stuck at "waiting for download", check for (and kill)
  a leftover `openocd` process still holding the JTAG interface.
- `sdkconfig.defaults` sets `CONFIG_ESPTOOLPY_NO_STUB=y`, which this board needs
  for reliable flashing.

## Connecting to the device

1. Connect your phone/laptop's Wi-Fi to the SSID set in
   `components/device_settings/device_config.h` (default `Hakista`), using the
   configured password (default `hak1sta!`).
2. Browse to either:
   - `http://192.168.4.1/` (always works -- the SoftAP's fixed gateway IP), or
   - `http://<MDNS_HOSTNAME>.local/` (default `http://hakista.local/` -- works
     out of the box on macOS/iOS/Linux; Windows needs Bonjour installed)

You should see the Home page, with links to Messages, Audio and Settings. On
Messages, type into a slot and Save -- the panel should redraw within a couple
seconds, as a quick end-to-end check that Wi-Fi + web server + display + flash
storage all agree with each other. Press **BOOT** to hear that slot in Morse;
hold **BOOT** for ~2s and press again to hear it loop.

## Project layout

Each piece besides the app entry point lives in its own ESP-IDF component under
`components/`, with its own `CMakeLists.txt` declaring exactly what it requires:

```
main/
  morse_code.c              -- app_main, single/double/long BOOT-press callbacks
  idf_component.yml         -- managed dependency (esp_codec_dev)
  CMakeLists.txt
components/
  message_store/             -- 10 text slots + selected-slot index (NVS namespace "morse")
    message_store.c/.h
    CMakeLists.txt
  audio_settings/            -- runtime Morse speed / tone / volume / loop flag (NVS namespace "audio_cfg")
    audio_settings.c/.h
    audio_config.h          -- edit first-boot audio defaults here before building
    CMakeLists.txt
  device_settings/           -- runtime SSID/password/hostname storage (NVS namespace "dev_cfg")
    device_settings.c/.h
    device_config.h         -- edit Wi-Fi/mDNS defaults here before building
    CMakeLists.txt
  mdns_service/              -- mDNS advertisement (<hostname>.local)
    mdns_service.c/.h
    idf_component.yml        -- managed dependency (espressif/mdns)
    CMakeLists.txt
  wifi_ap/                   -- Wi-Fi access-point bring-up
    wifi_ap.c/.h
    CMakeLists.txt
  epd_1in54/                 -- SSD1681 e-paper panel driver (SPI + GPIO control)
    epd.c/.h
    CMakeLists.txt
  text_display/              -- 5x7 bitmap-font renderer: word-wrap, both-axis centering, optional loop mark
    text_display.c/.h
    font5x7.h
    CMakeLists.txt
  morse_player/              -- ES8311 + I2S bring-up and the streaming Morse tone synthesizer
    morse_player.c/.h
    CMakeLists.txt
  boot_button/               -- BOOT button (GPIO0) polling + debounce, single/double/long classify
    boot_button.c/.h
    CMakeLists.txt
  web_server/                -- HTTP server: Home, Messages (GET+POST+clear),
    web_server.c/.h             Audio (GET+POST), Settings (GET+POST)
    pages/                   -- every page's HTML lives here (embedded into the
                                  binary at build time, see CMakeLists.txt):
                                    home.html, messages.html, audio.html,
                                    settings.html, restart.html, style.css
    CMakeLists.txt
partitions.csv               -- custom table: standard single-app layout with the app
                                  partition grown to fill 4MB flash
sdkconfig.defaults           -- 4MB flash, custom partition table, ES8311 via i2c_master
```

**Partition table:** this project uses a custom `partitions.csv` -- the standard
single-app layout, but with the `factory` app partition grown to fill the
board's 4MB flash, because the Wi-Fi AP + HTTP server + mbedtls stack pushes the
image well past the default 1MB. The 24KB `nvs` partition is unchanged, so saved
messages and settings survive a firmware update. If `idf.py build` ever reports
a stale/wrong partition size after pulling changes, delete `sdkconfig` and
rebuild so it regenerates from `sdkconfig.defaults` (`sdkconfig` is a local
cache, not checked in).

## Attribution

This project is shared publicly for anyone to fork, learn from, and build on. If
you use this code -- in full or in a substantial part, source or compiled
firmware -- in your own project, please credit **Hakista TV**:

- [github.com/hakistatv](https://github.com/hakistatv)
- [youtube.com/HakistaTV](https://youtube.com/HakistaTV)

A link back to this repo in your README or project description is enough.
