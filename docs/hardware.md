# Hardware

The firmware is an ESP-IDF 5.x application (`firmware/esp32`) built on the portable core. The reference board uses common modules you can wire on a breadboard. Other boards are a configuration change; see [porting.md](porting.md).

## Requirements

- **Chip:** an ESP32-S3 with PSRAM is recommended (N8R8 or N16R8). Plain ESP32/S2/C3/C6 work for displays up to about 240×240 without PSRAM, provided the framebuffer (width × height × 2 bytes) fits in internal RAM next to Wi-Fi.
- **Display:** an SPI ST7789 panel (240×320, 240×240 or 240×135). Other controllers need a different `esp_lcd` panel driver.
- **Microphone:** an I2S MEMS microphone (INMP441, ICS-43434, SPH0645 style).
- **Speaker (optional):** an I2S class-D amplifier (MAX98357A) and a 4–8 Ω speaker.
- **Buttons:** at least one (TALK). The DevKit's BOOT button works; a second button for CANCEL is recommended.

## ESP32-S3 breadboard

Board option `esp32s3-breadboard`, for an ESP32-S3-DevKitC-1 N8R8 and the modules above.

- **Flash:** 4 MB or more. The same image runs on 4, 8 and 16 MB modules.
- **PSRAM:** octal, as on the N8R8 and N16R8. On an N8R2 (quad PSRAM) or a module without PSRAM, the firmware still starts and says so in its log, but the display may not: the framebuffer needs PSRAM. For an N8R2, build with `CONFIG_SPIRAM_MODE_QUAD=y` instead.

| Module | Signal | ESP32-S3 GPIO |
|---|---|---|
| ST7789 LCD | MOSI / SDA | 11 |
| | SCLK / SCL | 12 |
| | CS | 10 |
| | DC | 9 |
| | RST | 8 |
| | BL (backlight) | 7 |
| | VCC / GND | 3V3 / GND |
| INMP441 mic | SCK | 4 |
| | WS | 5 |
| | SD | 6 |
| | L/R | GND (left channel) |
| | VDD / GND | 3V3 / GND |
| MAX98357A amp | BCLK | 15 |
| | LRC | 16 |
| | DIN | 17 |
| | VIN / GND | 5V / GND |
| Buttons (to GND) | TALK | 0 (BOOT) |
| | CANCEL | 14 |

All pins avoid the S3's flash/PSRAM pins (26–37) and native USB (19/20). Use **Custom pins** in menuconfig to change any of them.

## Waveshare ESP32-S3-LCD-1.54

Board option `esp32s3-lcd-154`, for Waveshare's all-in-one 1.54" board (SKUs 33866/33867; the `-EN` SKU is the same hardware): an ESP32-S3R8 (8 MB octal PSRAM) with 16 MB flash, a 240×240 ST7789 panel over SPI, an ES8311 DAC and an ES7210 microphone ADC with two microphones, an NS4150B amplifier, a speaker, a QMI8658 6-axis IMU, a TF card slot, a battery charger and the BOOT / PLUS / PWR keys. Nothing to wire and nothing to connect, it works out of the box. The touch version (`ESP32-S3-Touch-LCD-1.54`, SKUs 33868/33869) adds a CST816 touchscreen that this port does not use.

| Part | Chip | Connection |
|---|---|---|
| Display | ST7789, SPI | SCLK 38, MOSI 39, CS 21, DC 45, RST 40, BL 46 (LEDC) |
| Speaker DAC | ES8311 | I2C 0x18; I2S MCLK 8, BCLK 9, WS 10, DOUT 12; amplifier enable 7 |
| Microphones | ES7210 | I2C 0x40; I2S DIN 11 (shares the bus above), MIC1 + MIC2 |
| IMU | QMI8658 | I2C 0x6B |
| I2C bus | | SDA 42, SCL 41, 400 kHz |
| Buttons (to GND) | | TALK = BOOT (0), CANCEL = PLUS (4) |
| Battery | ETA6098 | GPIO 1 (BAT_ADC), GPIO 2 (power latch), GPIO 3 (CHG_STAT) — unused |

The PWR key is deliberately not mapped: Waveshare's factory firmware uses a long press for a software power-off through the GPIO 2 power latch. The TF card, QMI8658, battery gauge and touchscreen are unused for now.

**Build and flash it** with PlatformIO (`pio run -e esp32s3-lcd-154 -t upload -t monitor`) or with `idf.py`:

```bash
cd firmware/esp32
idf.py -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;boards/waveshare-esp32s3-lcd-154/sdkconfig.defaults" build
idf.py -p /dev/cu.usbmodem101 flash monitor
```

The USB-C port is the S3's own USB Serial/JTAG, so flashing and the serial console (115200 baud) both use it, exactly like the AMOLED board.

### First flash: what to check

1. **Boot log:** `ST7789 240x240 ready`, `codecs: speaker ready, microphones ready`, and in `hermes-gadget diag` an `i2c` list with `0x18` (ES8311) and `0x40` (ES7210). `0x6b` (QMI8658) answers too.
2. **Screen:** the mascot is centred, upright and not mirrored, and the colours are right (amber accents, not blue). If the image is mirrored, flip `mirror_x`; a one-pixel stripe at an edge means the gap is off. The panel is written for SPI mode 0; Waveshare's own demo uses mode 3, so a blank or garbled screen is a candidate for changing `io_cfg.spi_mode` in `port_display.cpp`.
3. **Microphone:** say something while holding BOOT; the waves move with your voice and Hermes's transcript is right. The ES7210 is configured here the same way as the AMOLED board (I2S standard mode, MIC1 + MIC2). If the capture is silent or wrong, Waveshare's factory demo drives the ADC in I2S TDM mode (4 slots, `bclk_div` 8) instead, which is the first thing to try.
4. **Speaker:** replies are clear and loud enough (`set volume 80`); no hiss between replies.
5. **Buttons:** BOOT holds to talk, PLUS cancels, and holding PLUS for 2 s starts a new conversation.

## ESP32-2432S028R Cheap Yellow Display

Board option `esp32-cyd`, for the ESP32-2432S028R ("CYD"): a classic ESP32-WROOM-32 (dual core, 240 MHz, 4 MB flash, **no PSRAM**) with a 2.8" 320×240 ILI9341 panel, an XPT2046 resistive touchscreen, an SC8002B amplifier on the DAC, an RGB LED, a light sensor (LDR) and the BOOT key. The board has **no microphone**: wire an INMP441 to the expansion pins to talk to it. Without one it still shows replies, speaks them and takes cards and actions from the agent.

| Part | Chip | Connection |
|---|---|---|
| Display | ILI9341, SPI (HSPI) | SCLK 14, MOSI 13, CS 15, DC 2, RST = EN, BL 21 (LEDC) |
| Touch | XPT2046, SPI (VSPI) | SCLK 25, MOSI 32, MISO 39, CS 33, IRQ 36 |
| Speaker | ESP32 DAC2 → SC8002B | GPIO 26 → **SPEAK** connector (JST 1.25 mm, 8 Ω speaker) |
| RGB LED | common anode | R 4, G 16, B 17 (low = lit), the agent's `led.set` |
| Light sensor | LDR divider | GPIO 34 (ADC1), reported as `light_pct` every 5 s |
| Button | BOOT | GPIO 0: TALK |
| Microphone (add-on) | INMP441 | SCK → GPIO 22 (CN1 / P3), WS → GPIO 27 (CN1), SD → GPIO 35 (P3), L/R → GND, VDD → 3V3 |

**Memory.** Without PSRAM a 320×240 framebuffer (150 KB) doesn't fit next to Wi-Fi and TLS, so the display keeps a 160×120 framebuffer and doubles every pixel on its way to the panel (`LcdConfig::fb_scale`). The UI lays itself out for 160×120; try it with the `sim-cyd` simulator board. Agent images (`gadget_display` pictures) arrive at that size too. The speaker queues 8-bit samples (1.5 s in 24 KB) and the DAC runs at 32 kHz, upsampled from the 16 kHz stream.

**Controls.** Hold the screen (or BOOT) to talk, tap to answer "yes", swipe down to cancel or answer "no". There are no scroll keys, so long replies turn their own pages.

**Panel revisions.** The single-USB (micro-USB) 2432S028R carries an ILI9341. The later revision with USB-C *and* micro-USB ports often carries an ST7789 with inverted colours: if the screen looks like a photo negative, set `CONFIG_HG_CYD_ST7789=y` (menuconfig → Hermes Gadget → Cheap Yellow Display options). Turn `CONFIG_HG_CYD_MIC` off when no microphone is wired, so the device doesn't offer voice input.

**Build and flash it** with PlatformIO. The board's USB port goes through a CH340 serial bridge (`/dev/ttyUSB0`; on Linux your user needs the `dialout` or `uucp` group):

```bash
cd firmware/esp32
pio run -e esp32-cyd -t upload -t monitor
```

### First flash: what to check

1. **Boot log:** `ILI9341 320x240 ready (framebuffer 160x120)`, `DAC speaker on GPIO26 ready`, `XPT2046 touch ready`, and about 150 KB of internal memory free.
2. **Screen:** the mascot is upright, not mirrored, and the accents are amber, not blue. Mirrored: flip `mirror_x` in `board.cpp`. Blue accents: flip `bgr`. Negative colours: the ST7789 revision, see above.
3. **Touch:** holding the screen shows the listening screen; a swipe down cancels. If a tap lands far from your finger, adjust `xpt.x_min/x_max/y_min/y_max` (the log prints the touch position at debug level, `log_level hg.xpt debug`).
4. **Speaker:** replies are audible on an 8 Ω speaker at the SPEAK connector (`set volume 80`). The DAC is 8-bit, so expect clear but slightly lo-fi speech.
5. **Microphone:** say something while holding the screen; the waves move with your voice and Hermes's transcript is right.

## ESP32-S3-Touch-AMOLED-1.75

Board option `esp32s3-touch-amoled-175`, for Waveshare's all-in-one board: an ESP32-S3R8 (8 MB octal PSRAM) with 16 MB flash, a 1.75" 466×466 AMOLED, touch, two microphones, a speaker output, a battery charger and an optional case. Nothing needs wiring; plug a small 8 Ω speaker into the **SPK** connector to hear replies.

| Part | Chip | Connection |
|---|---|---|
| Display | CO5300, QSPI | CS 12, SCLK 38, D0–D3 4/5/6/7, RST 39; column offset 6 |
| Touch | CST9217 | I2C 0x5A, RST 40 (INT 11 unused: polled) |
| Speaker DAC | ES8311 | I2C 0x18; I2S MCLK 42, BCLK 9, WS 45, DOUT 8; amplifier enable 46 |
| Microphones | ES7210 | I2C 0x40; I2S DIN 10 (shares the bus above), MIC1 + MIC2 |
| Power | AXP2101 | I2C 0x34; left at its power-on defaults |
| I/O expander | TCA9554 | I2C 0x20; P4 mirrors the PWR key |
| I2C bus | | SDA 15, SCL 14, 400 kHz |
| BOOT key | | GPIO 0 |

**Controls.** The screen is the main input:

| Do this | Does |
|---|---|
| Hold the screen | TALK: speak while holding, lift to send |
| Tap the screen | Answer "yes" to a question |
| Swipe down | CANCEL: discard a recording, close a card, stop a turn, answer "no" |
| Press the side PWR key | CANCEL as well; hold it 2 s for a new conversation |
| Hold BOOT | TALK, like holding the screen |

`set touch_cancel swipe` keeps only the swipe as CANCEL, `set touch_cancel pwr` only the PWR key, and `set touch_cancel both` restores the default. The same gestures work on the `sim-466x466-round` simulator board with the mouse.

**Build and flash it** with PlatformIO:

```bash
cd firmware/esp32
pio run -e esp32s3-touch-amoled-175 -t upload -t monitor
```

The USB-C port is the S3's own USB. It shows up as a "USB JTAG/serial debug unit" (a COM port on Windows), and both flashing and the serial console use it.

### First flash: what to check

This port is written from Waveshare's published pinout and drivers. On the first flash, go through this list, and for anything that looks wrong send the report from `hermes-gadget diag --port COMx`:

1. **Boot log:** `CO5300 466x466 ready`, `codecs: speaker ready, microphones ready` and `touch ready, key ready`. A `did not answer` or `missing` line names the part to look at. The `hg.diag` lines sum it up: reset reason, memory, and `parts: display co5300, microphone es7210, speaker es8311, touch yes, key yes`. In the `diag` report, `i2c` should include `0x18` (ES8311), `0x20` (TCA9554), `0x34` (AXP2101), `0x40` (ES7210) and `0x5a` (CST9217).
2. **Console:** `hermes-gadget console --port COMx` on the USB-C port answers `status`. If the log shows but commands get no answer, the console is still on UART0.
3. **Screen:** the mascot is centred, upright and not mirrored, and the colours are right (amber accents, not blue). A thin stripe at one edge means the column offset is off.
4. **Touch:** hold the screen and the listening waves appear; a swipe *down* (not up) cancels. A reversed swipe means the touch mirroring needs flipping.
5. **PWR key:** a short press cancels and holding 2 s starts a new conversation without powering the board off.
6. **Microphone:** say something; the waves move with your voice, and Hermes's transcript is right.
7. **Speaker:** replies are clear and loud enough (`set volume 80`); no hiss between replies.

## Build and flash

**No toolchain needed:** the [browser installer](https://adolanium.github.io/hermes-gadget-sdk/) flashes each release's prebuilt firmware from Chrome or Edge, then sets up Wi-Fi and pairing. The release files are also on the [releases page](https://github.com/Adolanium/hermes-gadget-sdk/releases), for `esptool.py write_flash 0x0 hermes-gadget-<board>-<version>.bin`, which also erases the board's settings.

To build it yourself, with ESP-IDF 5.3 or later installed (`. $IDF_PATH/export.sh`):

```bash
cd firmware/esp32
idf.py set-target esp32s3
idf.py -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;boards/esp32s3-breadboard/sdkconfig.defaults" build
idf.py -p COM5 flash monitor
```

Or with PlatformIO, which downloads ESP-IDF itself:

```bash
cd firmware/esp32
pio run -e esp32s3-breadboard -t upload -t monitor
```

The firmware is written for ESP-IDF 5.3 and later. It builds with ESP-IDF 6.1 through PlatformIO:

- ESP32-S3 breadboard: app image 1.13 MB in a 1.94 MB app slot, 14% of static RAM;
- ESP32-S3-Touch-AMOLED-1.75: 1.12 MB, otherwise the same;
- classic ESP32 with custom pins.

The flash holds two app slots (`partitions.csv`), so later firmware can arrive over the air. A board flashed with an earlier release gets the new partition table with its next USB flash. NVS stays where it was, so its settings and device key survive.

## Updates over the air

Once a board runs this firmware, new builds can reach it over Wi-Fi. On the Hermes host:

```bash
hermes gadget update "Kitchen" firmware/esp32/.pio/build/esp32s3-touch-amoled-175/firmware.bin
```

- The gateway installs the image as soon as the device is online, and the command waits and reports progress (`--no-wait` returns at once).
- The device shows the progress, restarts into the new firmware, and keeps it once it reaches Hermes again.
- If the new firmware doesn't reach Hermes within 5 minutes, or crashes before then, the device goes back to the previous one by itself.
- Only the Hermes that enrolled the device can update it: every image is authorized with the device's own key. The command also refuses an image built for another board.
- The dev server takes the same images: `update <path>` in its console.

On Windows, keep the project on a short path such as `C:\src\hermes-gadget-sdk`. ESP-IDF's linker-script step can exceed the Windows command-line length limit when the build directory is deeply nested.

`idf.py menuconfig` → **Hermes Gadget** sets the board, default Wi-Fi, server URL and device name. All of them can also be changed at run time over serial.

## Serial console

Open the console at 115200 baud (`idf.py monitor`, `pio device monitor`, or `hermes-gadget console --port COM5`). It shares its command set with the simulator:

```
gadget> set wifi_ssid MyWifi
@ok wifi_ssid
gadget> set wifi_pass secret
@ok wifi_pass
gadget> set server ws://192.168.1.20:8765/gadget
@ok server
gadget> status
@status {"device_id":"hg-...","phase":"online","screen":"pairing","pairing_code":"ABCD2345",...}
```

| Command | Meaning |
|---|---|
| `status` | JSON status |
| `diag` | JSON diagnostics: build, reset reason, memory, Wi-Fi, which parts came up, I2C addresses that answer, task stacks, connection |
| `diag log` | The last few KB of log lines, kept in RAM since boot |
| `get <key>` | Read a setting (secrets are masked) |
| `set <key> <value>` | Write a setting; an empty value clears it |
| `say <text>` | Send a typed message |
| `talk` / `release` | Press or release TALK (bench automation) |
| `cancel` | Press CANCEL |
| `new-session` | Start a fresh conversation (same as holding CANCEL for 2 s) |
| `yes` / `no` | Answer the question on screen |
| `reconnect` | Drop and re-open the Hermes connection |
| `forget-key` | New device identity on next boot (re-enrollment and re-pairing) |
| `factory-reset` | Erase the device key and all settings |

The keys are `name`, `server`, `token`, `talk_mode` (`hold` or `tap`), `volume`, `wifi_ssid` and `wifi_pass`.

Machine-readable lines start with `@`, so tools can drive a bench device. `hermes-gadget provision` is a thin wrapper over these commands.

## Power-on sequence

1. **Boot:** about 1 s.
2. **Wi-Fi:** credentials from NVS, else from menuconfig. Without credentials the screen says "No network".
3. **Connect:** the device opens the WebSocket to `server`, retrying 1 → 30 s with backoff.
4. **Authenticate:** it enrolls its key on first contact and proves it with an HMAC afterwards.
5. **Pair or ready:** an unpaired device shows a pairing code (approve with `hermes gadget pair`); a paired one goes to **Ready**.

## Known limits of the reference firmware

- Push-to-talk or tap with energy VAD. There is no wake word; the protocol leaves room for one (`audio.start.mode`).
- Wi-Fi provisioning is over serial (or menuconfig). There is no SoftAP or BLE provisioning yet.
- Firmware updates are authorized with the device key, but images aren't signed: the bootloader runs whatever a trusted Hermes installs. Secure Boot isn't enabled.
- The text font is ASCII only; the host folds other characters.
