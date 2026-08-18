# hw-lilygo-tdeck — LilyGo T-Deck Plus board HAL

**hw-lilygo-tdeck** is the board-support straddle for the **LilyGo T-Deck Plus**
(ESP32-S3FN16R8 — 16 MB flash, 8 MB octal PSRAM; SX1262 LoRa; 2.8" 320×240
ST7789V LCD; GT911 touch; BlackBerry-style trackball; ESP32-C3 QWERTY keyboard;
pre-fitted GNSS; microSD). It makes the board usable by an application: it owns
the power/CS bring-up, the on-device-UI input HAL, the GNSS receiver, the
optional RTC, the battery monitor, and the mic-codec shim, and it publishes the
board's pin map and hardware tuning as Kconfig and storage keys.

It is a **non-buildable** component — it decides nothing about what the device
*does*. A buildable assembler (`reticulous/reticulous`) adds it and inherits the
board: `spangap build reticulous/reticulous --with spangap/hw-lilygo-tdeck`. The
mesh stack, the IP/web platform, `app_main`, the partition layout, the update
story and the browser SPA all come from the buildable and its other straddles —
not from here.

## Origins

The board itself is LilyGo's T-Deck Plus; the pin assignments are cross-checked
against LilyGo's own `utilities.h`, the Meshtastic `t-deck` variant, and the
ratspeak `ratdeck` board config (which is AGPL — read for hardware facts, never
copy code). The original (non-Plus) T-Deck is pin-compatible; the T-Deck **Pro**
is a different PCB (different SPI/I2C buses, e-paper, no trackball) and is **not**
this straddle. Sub-revision quirks a maintainer can hit — GT911 address, ball
orientation — are in [INTERNALS.md](INTERNALS.md).

## What it does, and how it fits

The board contributes hooks that the buildable's generated init dispatcher calls
in two bands — a `start:` band that runs before `spangapInit()` (bare-hardware
prerequisites) and an `init:` band that runs after the platform is up. There is
nothing to call by hand: if the straddle is in the build, the board comes up
automatically.

| Hook | Band | Present when | Brings up |
|---|---|---|---|
| `tdeckStart` | start | always | peripheral power rail HIGH, shared-SPI CS park, shared I2C0 bus |
| `tdeckLcdStart` | start | `spangap-lcd` staged | registers the touch/trackball/button input HAL with the LCD component (before `lcdInit`) |
| `tdeckLcdInit` | init | `spangap-lcd` staged | QWERTY keyboard (needs the LCD task) |
| `tdeckAudioInit` | init | `spangap/audio` staged | registers the ES7210 mic codec ops |
| `gpsInit` | init | always | spawns the GNSS task |
| `tdeckBatteryInit` | init | always | ADC + 1/min battery-voltage timer |

`tdeckStart` **must** precede `spangapInit()`, because the first shared-SPI-bus
access is the SD-card mount *inside* `spangapInit()` and the SD card is dead
until the +3.3 V peripheral rail is up and the LCD/LoRa CS lines are parked HIGH.
That ordering is the reason the board has a `start:` hook at all.

The one thing this board pulls in itself is the on-device LCD UI
([spangap-lcd](../spangap-lcd)), because the T-Deck physically has a screen
(`additional_installs:`). Drop it with `--no-lcd` for a headless build — the
power rail, GNSS, RTC and battery monitor still work; only the input HAL and
keyboard are compiled out. The display panel, LoRa radio and audio I2S engine
are owned by other straddles ([spangap-lcd](../spangap-lcd),
[iface-lora](../iface-lora), [spangap/audio](../audio)); this board only
supplies their pins (below) and the input/control glue.

## Board identity (`detect_hw`)

`esp-idf/src/detect.cpp` answers one question about this board: it returns
`"hw-lilygo-tdeck"` when the hardware under the firmware is this board, and NULL when
it is not. What it asks:

16 MB flash, then the ESP32-C3 QWERTY keyboard acking on the shared I2C0 bus with
the peripheral rail up — unique to this board among the ones spangap knows —
confirmed by the SX1262, since a keyboard alone could be a bare C3 on a bench.
The rail is released **only** when the probe fails; on a T-Deck the firmware
wants it up anyway. Touch, the ES7210 codec, the RTC and the GNSS receiver are
fitted or not on this same straddle, so they identify nothing and are logged for
the trace rather than tested — and only in flashmon's detector, where the 1.2 s
per rate the GNSS autobaud costs is affordable.

spangap-core calls it before the first `onStart()` — the last moment no bus is
claimed — and **halts the device awake** when the answer disagrees with the board
this image was built for, since every pin map here would then belong to someone
else's hardware. The confirmed answer is published as `sys.hw` and announced on
the console as `build: hw hw-lilygo-tdeck`. flashmon's standalone detector carries a
hand-kept copy of the same function, renamed `detect_hw_lilygo_tdeck`, to identify a chip
whose firmware is unknown; change one, change the other. See
[spangap-core/docs/init.md](../spangap-core/docs/init.md) and
[flashmon/docs/detect.md](../flashmon/docs/detect.md).

## Hardware & pin map

LilyGo T-Deck Plus — **ESP32-S3FN16R8** (16 MB flash, 8 MB **octal** PSRAM,
selected via `CONFIG_SPIRAM_MODE_OCT`). Native USB CDC (no USB-UART bridge); the
serial port enumerates as a `usbmodem`/`ttyACM` device. There is no PMIC — an
unmarked linear charger handles the single Li-Po cell, and the case slide switch
is the only hard power cut.

A single FSPI bus (SPI host 2) is shared by the SD card, the SX1262 LoRa modem
and the ST7789V display: **SCK 40, MOSI 41, MISO 38**, with a per-device CS.
A single I2C0 bus (**SDA 18, SCL 8**) carries the keyboard, touch, RTC and audio
codec.

### Board-owned pins (in this straddle's `tdeck.h`)

| Signal | GPIO | Notes |
|---|---|---|
| Peripheral power EN | 10 | active-high; gates +3.3 V to display/SD/GPS/LoRa |
| Battery sense (VBAT/2) | 4 | ADC1, 2:1 divider, upstream of the power gate (always live) |
| Home / centre button | 0 | BOOT-strap pin; also trackball centre-press; active-low |
| Trackball U / D / L / R | 3 / 15 / 1 / 2 | four direction lines, active-low pulses |
| QWERTY keyboard (ESP32-C3) | I2C 0x55, INT 46 | INT dead on stock C3 firmware → polled |
| GT911 touch | I2C 0x5D *or* 0x14, INT 16 | address latched from INT level at power-on; no RST |
| PCF8563 RTC | I2C 0x51 | optional — absent on a stock T-Deck |
| ES7210 mic codec | I2C 0x40 | control only; I2S pins below |
| GNSS UART | RX 44, TX 43 | UART1, NMEA 8N1; no PPS routed |

### Bus/peripheral pins this board sets for other straddles (`kconfig:`)

A non-buildable straddle's `sdkconfig.defaults` would be ignored, so every value
that describes this hardware is published from `straddle.yaml`'s `kconfig:`
block and consumed by the owning straddle.

| Group (owner) | Pins |
|---|---|
| **SD card** (spangap-core) | CS 39, SCK 40, MOSI 41, MISO 38, SPI host 2 (SPI mode) |
| **LoRa SX1262** (iface-lora) | CS 9, DIO1 45, BUSY 13, RST 17; SCK 40 / MOSI 41 / MISO 38; TCXO 1.8 V; DIO2 = RF switch |
| **Display ST7789** (spangap-lcd) | CS 12, DC 11, BL 42, no RST (resets with the power rail); 240×320 native, rotated 90°, colour-inverted, 40 MHz PCLK |
| **Audio out — MAX98357A** (spangap/audio) | I2S1: BCK 7, WS 5, DOUT 6 |
| **Audio in — ES7210** (spangap/audio) | I2S0: MCLK 48, SCK 47, WS 21, DIN 14; default rate 16 kHz |

Flash size is intentionally **not** pinned: the platform ships a 4 MB floor
image and self-grows to the real 16 MB at first boot, so one image is universal
across flash sizes. PSRAM runs at the platform's 80 MHz default.

LoRa is sold per band (433 / 868 / 915 / 920 MHz — the radio is identical, only
the matching network and antenna differ). The antenna is a PCB trace on the
standard SKU or an IPEX/u.FL connector on the external-antenna SKU; there is no
SMA bulkhead. The WiFi/BT antenna is always the module's PCB trace.

## Storage variables

Settings live under `s.tdeck.*` and `s.gps.*` (writable by the user / browser /
CLI; the board's **Hardware**, **Display**, **GPS**, **Trackball** and **Centre
button** sections of the **System** settings page are generated from
`straddle.yaml`'s `settings:` block). Runtime telemetry is published under bare
namespaces for anything to observe. All values below are verified against the source.

### Settings — trackball & pointer (`s.tdeck.*`, live)

| Key | Default | Meaning |
|---|---|---|
| `s.tdeck.trackball_speed` | `16` | px per pulse at a full flick (the accel ceiling) |
| `s.tdeck.trackball_accel_min` | `3` | pulses/s at or below which a pulse moves 1 px |
| `s.tdeck.trackball_accel_max` | `18` | pulses/s at which `trackball_speed` is reached |
| `s.tdeck.trackball_smooth_ms` | `150` | time constant of the pulse-rate EMA |
| `s.tdeck.pointer_visible_time` | `2` | cursor dwell, seconds after activity; `-1` = always on |

### Centre button

One hold and three click counts: hold 300 ms to sleep the device, one click
clicks, two go to the launcher, three raise the running-app switcher. The two
navigation counts work from anywhere, including inside an app — and on a sleeping
device, where the waking press counts as the first click, so a double or triple
click wakes and navigates in one gesture. (A single click on a sleeping device
just wakes it.) A further click extends the burst if it lands within 250 ms of
the last.

Neither timing is a setting — they are reflexes, not preferences.

### Settings — GNSS (`s.gps.*`)

| Key | Default | Meaning |
|---|---|---|
| `s.gps.enable` | `1` | run the receiver; `0` → standby + drop the UART |
| `s.gps.interval` | `5` | fix cadence: `0` = continuous (1 Hz), `1`–`10` = PSM cyclic-tracking period (s, u-blox only) |
| `s.gps.ignore_clock` | `0` | `1` = never set the system clock from GPS |

### Runtime / telemetry (published)

| Key | Meaning |
|---|---|
| `sys.board` | the board's name for a reader (`BOARD_NAME`), shown in Settings → System → Hardware |
| `battery.millivolt` | true VBAT in mV (pin reading × 2, EMA-smoothed) |
| `battery.percent` | 0–100 via the measured discharge curve |
| `tdeck.touch` | GT911 probe result string (`GT911 @ 0x5D` / `not found`) |
| `tdeck.multi_touch` | runtime *input* flag (no `s.`): a consumer (e.g. maps) sets it to enable multi-point touch; not persisted |
| `gps.model` | inferred receiver, or `detecting…` / `not detected` |
| `gps.baud` | locked baud (`0` when not running) |
| `gps.state` | `off` / `detecting` / `not detected` / `standby` / `power-cycle to wake` / `acquiring` / `fix` |
| `gps.fix` | `none` / `2D` / `3D` |
| `gps.quality` | GGA fix-quality indicator |
| `gps.lat` `gps.lon` | decimal degrees (6 dp); **last-known**, not cleared on fix loss |
| `gps.alt` `gps.geoid` | metres (1 dp) — MSL altitude, geoid separation |
| `gps.speed` `gps.course` | km/h, degrees (1 dp) |
| `gps.sats_used` `gps.sats_view` | satellites in solution / summed in-view |
| `gps.hdop` `gps.vdop` `gps.pdop` | dilution of precision (2 dp) |
| `gps.snr` | best C/N0 this epoch (dBHz) |
| `gps.utc` | `YYYY-MM-DD HH:MM:SS`, UTC |
| `gps.fix_age` | seconds since the last positioned fix; `-1` = never this session |

The GNSS task also interacts with the platform's clock namespace (owned by
[spangap-core](../spangap-core)/its clock layer): it writes `sys.time.valid`
when it disciplines the clock and asserts `sys.time.ext` to claim clock
authority (NTP parks while it is set). The centre-button / inactivity standby
path drives the ephemeral `sys.standby` key, which the input HAL subscribes to.

### Surfaced but owned elsewhere

The board adds `s.lcd.backlight` and `s.lcd.inactivity_timeout` (owned by
[spangap-lcd](../spangap-lcd)) to the **Display** section that straddle opens —
this block only surfaces them. Runtime LoRa parameters live at `s.lora.*`
([iface-lora](../iface-lora)).

## CLI

Run any of these on the live device through `spangap cli "<command>"`.

```
gps              GNSS status — state, model, baud, interval, position, fix, sats, DOP, UTC
gps on | off     set s.gps.enable
```

## Dependencies

- [spangap-core](../spangap-core) — base runtime (storage, log, CLI, fs, ITS).
- [spangap-lcd](../spangap-lcd) — pulled in by default (the board has a screen);
  drop with `--no-lcd`. Supplies the LCD shell/`LcdApp` model; this board only
  wires the touch/trackball/button/keyboard hardware into it.
- `esp_lcd_touch_gt911` — GT911 driver for the touch input HAL.

## Read next

- [INTERNALS.md](INTERNALS.md) — bring-up ordering, the GNSS/RTC/battery/audio/
  LCD-input internals, the threading model, and the board pitfalls.
