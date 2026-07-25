# T-Deck — the `tdeck` board module, plus a hardware/firmware reference

This file documents reticulous's **`tdeck` board-support module** first
(`main/tdeck.cpp` + `main/tdeck.h`), then — below the divider — the exhaustive
**hardware and firmware reference** the module is built from: every T-Deck
variant's pin map, and architecture notes on `ratspeak/ratdeck`, the
Reticulum/LXMF firmware we learn from but don't copy. Sources are cited per
claim; where upstream repos disagree, the disagreement is called out rather than
papered over.

> Docs-format note: "what the module does" stays at the top; the deep research
> reference follows. A standardized per-module docs format is a TODO across the
> tree — for now, just keep the module contract near the top of this file.

## The `tdeck` module — what `tdeck.cpp` / `tdeck.h` provide

`tdeck` is the single owner of all LilyGo T-Deck Plus hardware bring-up: the
board layer beneath the spangap `lcd` UI component, and the home of everything
T-Deck-specific that the platform layer stays generic about.

**Public surface (`tdeck.h`).** Two things:

1. **The board pin map** — compile-time `BOARD_*` constants for the board's
   bespoke peripherals (touch / trackball / keyboard / centre button; GNSS; the
   peripheral power-enable pin), consumed by `tdeck.cpp` and `gps.cpp`. There is
   no board-select Kconfig — hw-lilygo-tdeck is the T-Deck. The **display** pins live in
   the lcd component's `CONFIG_LCD_*` (sdkconfig.defaults); the **LoRa** pins in
   iface-lora's `CONFIG_LORA*`.
2. **The bring-up API** — `tdeckPreInit()` and `tdeckPostInit()`.

**Two-phase init (why it isn't one call).** Bring-up straddles `spangapInit()`:

| Phase | When | Does |
|---|---|---|
| `tdeckPreInit()`  | **before** `spangapInit()` | drive the +3.3 V peripheral rail HIGH; park the shared-SPI CS lines (LCD + LoRa) so they don't drive MISO; *(CONFIG_SPANGAP_LCD)* register the touch/pointer/button input HAL with `lcd` (`lcdSetInput`) |
| *(spangapInit)* | — | mounts SD over the shared SPI bus (needs the rail + parked CS), then `lcdInit()` brings the panel up from `CONFIG_LCD_*` and calls the input HAL's `init()` (→ touch + button + trackball wiring) |
| `tdeckPostInit()` | **after** `spangapInit()` | *(CONFIG_SPANGAP_LCD)* bring up the QWERTY keyboard — it needs the lcd task `spangapInit()` created |

It can't collapse to one call: the power rail must be up before `spangapInit()`'s
first shared-bus access (`fs_mount_sd()`), the HAL must be registered before its
`lcdInit()`, but the keyboard needs the lcd task `spangapInit()` creates.
`main.cpp` is correspondingly thin — `tdeckPreInit(); spangapInit(); tdeckPostInit();`.

**What it owns.** The power/CS glue is always compiled (SD and LoRa need the
rail even with no UI). The ST7789V display itself is **not** the board's — the lcd
component brings it up from `CONFIG_LCD_*` (320×240, esp_lcd, LEDC-PWM backlight).
Under `CONFIG_SPANGAP_LCD` the board registers, as the `lcd` input HAL
(`lcd_input.h`):

- **GT911 capacitive touch** (probed at 0x5D/0x14; interrupt-driven) → `touch_read`
- **Trackball → mouse pointer** with velocity-dependent acceleration — the module
  owns the whole pointing device (the curve *and* the settings). Driving the
  pointer **into a screen edge** while it's already pinned there pans the active
  widget instead via `lcdScroll()` (the step px the clamp would swallow become the
  scroll distance) — so a touchless deck reaches offscreen content and pages the
  launcher. Skipped in trackball→arrows mode (the CLI/terminal claims the ball).
- **Centre/Home button** (GPIO 0) → the pointer's click / hold-to-Home
- **QWERTY keyboard** (ESP32-C3 @ I2C 0x55) — owned entirely here, *not* by `lcd`:
  its INT is dead and its read is destructive, so it runs its own poll task +
  keypad indev. lcd knows nothing about it.

**Settings it owns** (root-level "T-Deck" Settings pane): `s.tdeck.trackball_speed`
(pointer speed 4–40) and `s.tdeck.pointer_visible_time` (cursor dwell seconds,
`-1` = always). It also surfaces the generic `s.lcd.backlight`.

The input-HAL contract itself (what `lcd` expects of any board) is spangap-lcd's
`lcd_input.h` — see [`../../spangap/docs/lcd.md`](../../spangap/docs/lcd.md). The
deep wiring (the interrupt-driven indev model, the pointer-acceleration math, the
keyboard's self-healing INT) is in **§1.8** below.

---

Everything below is the hardware + firmware **reference** the module is built
from — cite it when writing or revising drivers.

---

## Part 1 — T-Deck hardware

### 1.1 Family overview

LilyGo currently ships three T-Deck branded board families:

| Variant | Display | LoRa | GPS | Cell | Audio out | Battery | Case | Notable adds |
|---|---|---|---|---|---|---|---|---|
| **T-Deck** | 2.8" ST7789 LCD 320×240 | SX1262 | optional add-on | — | I2S codec ES7210 (mic) + I2S DAC for spkr | varies (~1000 mAh add-on) | none | reference design |
| **T-Deck Plus** | 2.8" ST7789 LCD 320×240 | SX1262 | **fitted** (L76K or u-blox MIA-M10Q) | — | same as T-Deck | 2000 mAh built-in | ABS shell with 1/4" mount | external-ant SKU |
| **T-Deck Pro V1.0** | 3.1" e-paper GDEQ031T10 320×240 + CST328 touch | SX1262 | u-blox MIA-M10Q (always) | optional A7682E LTE Cat-1 | PCM5102A I2S DAC *or* via A7682E | 1400–1500 mAh built-in | ABS shell | TCA8418 keyboard, BHI260AP IMU, LTR-553 ALS, BQ25896 charger, BQ27220 fuel gauge |
| **T-Deck Pro V1.1** | same as V1.0 | SX1262 | MIA-M10Q | A7682E option | same | same | same | adds DRV2605 haptic driver (I2C 0x5A) and `PIN_VIBRATION` |
| **T-Deck Pro MAX** | same | SX1262 | MIA-M10Q | A7682E option | same | same | same | adds XL9555 I/O expander (I2C 0x20) on top of V1.1 features |

Sub-SKUs for every variant are sold by frequency band: **433 / 868 /
915 / 920 MHz**, in either **internal PCB-trace** or **external
IPEX/u.FL** antenna versions. The 920 MHz SKU is the MIC-certified
Japan unit; firmware-side it is identical to the 915 MHz unit (same
SX1262, same matching network family).

A few important up-front facts:

- All three families use the **same MCU package** —
  `ESP32-S3FN16R8`, 16 MB flash, 8 MB octal PSRAM.
- All three families use the **Semtech SX1262** as the LoRa radio.
- The Pro is the only one that breaks the bus topology — it moves
  the SPI bus, the I2C bus, and most of the peripheral pinout. The
  original T-Deck and the T-Deck Plus are pin-compatible; the Plus
  is essentially the original T-Deck PCB inside an ABS case with a
  GPS module pre-soldered onto the Grove header and a higher-
  capacity battery. Driver code written for the T-Deck runs on the
  Plus unchanged.
- The original T-Deck is **non-touchscreen** (trackball-only
  navigation) per the LilyGo wiki, despite some retailer listings
  claiming "touchscreen." The Meshtastic `variant.h` for `t-deck`
  declares a GT911 (`I2C addr 0x5D, INT GPIO16`) that corresponds
  to a small touchpad area on certain T-Deck **keyboard PCB sub-
  revisions**, not a touch overlay on the LCD. Most users report
  touch as non-functional. Treat GT911 support as best-effort. The
  T-Deck Pro is the only variant in the family with a real LCD (or
  EPD) touch overlay — CST328.

No "T-Deck Mini" / "T-Deck Lite" / other-named SKUs were found as of
May 2026. LilyGo's "T-Keyboard" is a separate, related product (a
Bluetooth-only BlackBerry-style keyboard, not a T-Deck). The "T-Deck
Pro MAX" is currently at "MAX V0.1, not yet available" per the
LilyGo wiki.

---

### 1.2 T-Deck (original) — full spec

Source-of-truth file: `Xinyuan-LilyGO/T-Deck/examples/UnitTest/utilities.h`
(referenced as the canonical pinout by the LilyGo wiki and DeepWiki)
and `meshtastic/firmware/variants/esp32s3/t-deck/variant.h`.

#### MCU & memory

- **Part number:** ESP32-S3FN16R8 (Espressif). Dual-core Xtensa LX7
  @ 240 MHz, 2.4 GHz Wi-Fi 802.11 b/g/n, Bluetooth 5.0 LE.
- **Flash:** 16 MB integrated (the `N16R8` suffix).
- **PSRAM:** 8 MB **octal** PSRAM, in-package (the `R8` suffix; this
  is octal, not quad — menuconfig must select octal-mode PSRAM or
  the chip will not initialize).
- **Secondary MCU:** ESP32-C3 dedicated to keyboard scanning. I2C
  **slave** at address `0x55` to the host ESP32-S3. Reflashable via
  the 6-pin header next to the RST button (pin order from RST end:
  TX, RX, BOOT, RST, GND, VCC; needs an external USB-TTL).

#### Power rail

- `BOARD_POWERON = GPIO 10` — **must be driven HIGH at boot** before
  any peripheral will work. This gates the +3.3 V rail to the
  display, LoRa, SD, audio, and I2C bus. Forgetting this is the #1
  bring-up mistake.
- `BOARD_BAT_ADC = GPIO 4` — battery voltage divider (2:1, ADC
  multiplier 2.11 per Meshtastic).
- `BOARD_BOOT_PIN = GPIO 0` — also wired to the trackball center-
  press, so trackball clicks register as BOOT-button presses (and
  vice versa). When the microphone is in use, GPIO 0 is unavailable
  as a button.
- USB-C connector for power and JTAG/CDC. No external USB-to-UART
  chip — the ESP32-S3's native USB peripheral is used.
- No dedicated PMIC. Charging is done by an unmarked Li-Po linear
  charger on the PCB; there is no I2C charger IC on the original
  T-Deck.

#### Buses

- **I2C0** (shared): `SDA = GPIO 18`, `SCL = GPIO 8`. Devices:
  keyboard MCU at `0x55`, GT911 touch at `0x5D` (or `0x14` depending
  on ADDR strap) **if** present on the keyboard PCB sub-revision.
  INT lines: `BOARD_TOUCH_INT = GPIO 16` (works); `BOARD_KEYBOARD_INT =
  GPIO 46` is wired but the C3 firmware never drives it, so the keyboard is
  polled, not interrupt-driven (see Keyboard below).
- **SPI2 (HSPI)** — single shared bus for display, LoRa, and SD.
  `MOSI = GPIO 41`, `MISO = GPIO 38`, `SCK = GPIO 40`. CS lines split
  per-device (see below).

#### Display

- **Panel:** 2.8" IPS LCD, 320×240, 16 bpp RGB565.
- **Controller:** Sitronix **ST7789V** over SPI. **Init sequence was
  updated 2024-07-26** in the LilyGo repo — older code may produce
  wrong gamma; use the current sequence from `T-Deck/firmware`.
- **Pins:** `BOARD_TFT_CS = GPIO 12`, `BOARD_TFT_DC = GPIO 11`, **no
  dedicated RST pin** (panel is reset by toggling `BOARD_POWERON`),
  `BOARD_TFT_BACKLIGHT = GPIO 42` (PWM-capable).
- **Bus:** SPI2 shared, MOSI 41 / MISO 38 / SCK 40. Run at 40–80 MHz.
- On reticulous this panel is driven by spangap-core's `lcd` LVGL component over
  `esp_lcd` — see [§1.8](#18-reticulous-on-device-ui--how-we-wire-it) for the
  wiring and [../../spangap/docs/lcd.md](../../spangap/docs/lcd.md) for the UI.

#### Input devices

- **Trackball** (BlackBerry-style optical ball, 5 outputs):
  - `BOARD_TBOX_G01 = GPIO 3` (one direction)
  - `BOARD_TBOX_G02 = GPIO 2`
  - `BOARD_TBOX_G03 = GPIO 15`
  - `BOARD_TBOX_G04 = GPIO 1`
  - Center press: `BOARD_BOOT_PIN = GPIO 0` (shared with strapping
    pin and microphone)
  - The four direction lines are GPIO interrupts; firmware counts
    edges to derive scroll deltas. The LilyGo factory firmware sets
    all four as `INPUT_PULLUP` and triggers on `FALLING`.
  - Note: ratdeck ships a board-quirk fix: on its T-Deck Plus
    sample the DOWN pin generates rightward motion and RIGHT
    generates downward. Verify per unit.
  - On reticulous these four lines drive an LVGL **mouse pointer**
    (not scroll/keys) with an auto-hiding cursor — see
    [§1.8](#18-reticulous-on-device-ui--how-we-wire-it).
- **Keyboard:** physical QWERTY membrane keyboard with backlight.
  Keys are scanned by the on-keyboard ESP32-C3, which exposes a
  tiny I2C-slave protocol. Host reads the next pressed character
  with `Wire.requestFrom(0x55, 1)` — a **destructive** read (pops the key;
  returns `0` when empty, with no peek/count), so you can't tell a key is
  pending without consuming it. Asking for more than 1 byte returns garbage on
  stock firmware. Keyboard backlight is driven by the C3's own GPIO; no separate
  BL pin on the host. Sources: `rgrizzell/lilygo-t-deck-keyboard` and the LilyGo
  `firmware/` tree.
  **No usable keyboard interrupt:** `GPIO 46` is wired as the alleged keyboard
  INT, but the C3 firmware never drives it — verified on hardware (the pin never
  moves on a keypress) *and* in the C3 source (`INT_PIN` is defined but never
  written; LilyGo's own "keyboard interrupt" issue #9 was closed unanswered).
  Because of all this, the keyboard is **owned by the board module
  ([`main/tdeck.cpp`](../main/tdeck.cpp))**, not the generic `lcd` component
  (touch/trackball/button stay interrupt-driven there). It runs a dedicated
  low-prio task that polls the I2C off the lcd task (~30 ms while typing, ~200 ms
  idle — the C3 buffers keys, so a lazy poll only delays the first keypress),
  buffers keys into a queue, and bumps the lcd task via `lcdRun()` to drain them
  through its own LVGL keypad indev (joined to `lcdInputGroup()`); it tells lcd
  `lcdSetHasKeyboard(true)` so Settings edits text in place. `GPIO 46` is still
  wired: the **first edge ever seen flips the reader from polling to
  interrupt-driven** (self-healing if a future C3 firmware drives it). See
  [§1.8](#18-reticulous-on-device-ui--how-we-wire-it).
  **Path to interrupt-driven (C3-side fix):** the S3 needs *no* changes — the
  self-healing ISR path above is already waiting for the edge; the fix is entirely
  in the reflashable C3 ([§1.2](#12-t-deck-original--full-spec)). A corrected C3
  firmware must drive `INT_PIN` **open-drain, active-low, level-held**: pull it LOW
  whenever its key FIFO is non-empty and release it (high-Z — the S3's GPIO 46
  pull-up restores HIGH) once a host read drains the FIFO to empty. Open-drain +
  active-low matches the S3's pull-up; *level-held*, not a per-key pulse, is the
  key choice — it's robust to a missed edge (the line just stays LOW until the host
  has drained every key), and the `ANYEDGE` drain-loop already copes with both
  transitions (falling → drain; the trailing rising edge after empty costs one
  harmless empty read). The hole to fill is in `rgrizzell/lilygo-t-deck-keyboard`,
  where `INT_PIN` is defined but never written. Caveat: reflashing replaces
  LilyGo's stock keyboard firmware — recoverable (BOOT/RST are on the same 6-pin
  header) but physical and per-board, so the poll path stays as the fallback for
  un-reflashed units.
- Buttons: physical RST (hardware), BOOT (= GPIO 0 = trackball
  click).

#### Radio (LoRa)

- **Chip:** Semtech **SX1262** (LLCC68 footprint-compatible; shipped
  as SX1262 on all production units). Single-CPU TX/RX, 150 MHz–960
  MHz capable but only one band populated per SKU.
- **Bus:** SPI2 shared with display + SD. CS = `RADIO_CS_PIN =
  GPIO 9`.
- **Control pins:**
  - `RADIO_BUSY_PIN = GPIO 13`
  - `RADIO_RST_PIN = GPIO 17`
  - `RADIO_DIO1_PIN = GPIO 45` — IRQ
  - `DIO0`, `DIO2`, `DIO3` are not broken out as host GPIOs. `DIO2`
    is wired internally as the **antenna RF switch** control (set
    `SX126X_DIO2_AS_RF_SWITCH = 1` in RadioLib); `DIO3` is wired to
    the TCXO power.
- **TCXO:** present on-module. **DIO3-controlled, 1.8 V (not 2.4)**
  on the original T-Deck reference design — Meshtastic's T-Deck-Pro
  variant.h uses 2.4 V; for the original T-Deck, the LilyGo example
  projects use `1.8` V. Verify against your unit; the symptom of
  wrong TCXO voltage is "frequency error" reported by RadioLib (~30
  kHz off).
- **Frequency SKUs:** 433, 868, 915, 920 MHz — sold separately. Chip
  is identical; difference is the matching network and antenna.
- **Antenna:** PCB-trace antenna on the standard SKU. The "external
  antenna" SKU has an IPEX (u.FL / MHF1) connector instead of the
  trace, with a flexible whip antenna in the box. **No SMA bulkhead
  on the case** — confirmed by GitHub issue #20 (closed without an
  SMA-version answer).
- **WiFi/BT antenna:** PCB-trace, soldered to the ESP32-S3 module.
  No IPEX option.

#### Audio

- **Codec / mic input:** Everest Semi **ES7210** quad-mic ADC (only
  one mic populated — the MSM381A3729H9CP MEMS mic). I2S input.
  - `BOARD_ES7210_MCLK = GPIO 48`
  - `BOARD_ES7210_LRCK = GPIO 21`
  - `BOARD_ES7210_SCK  = GPIO 47`
  - `BOARD_ES7210_DIN  = GPIO 14`
  - I2C control on the shared I2C0 bus (default ES7210 address
    `0x40`).
- **Speaker output:** I2S output on a separate pin set, fed into a
  small Class-D amp (Meshtastic and CNX writeups identify it as
  **MAX98357A**, soldered directly under the speaker). I2S pins:
  - `BOARD_I2S_BCK  = GPIO 7`
  - `BOARD_I2S_WS   = GPIO 5`
  - `BOARD_I2S_DOUT = GPIO 6`
  - The MAX98357A is a self-clocking Class-D device with **no MCLK
    input at all**; GPIO 21 is purely the ES7210's word-select.
    Mic and speaker run **full-duplex** on two physically separate
    I2S controllers (mic on I2S0, speaker on I2S1 — see
    spangap/audio). The LilyGo factory firmware operates them
    mutually exclusively, but that is a software choice, not a
    hardware constraint.
- **Headphone jack:** none on the original T-Deck.

#### Storage

- microSD slot, **SPI mode only** (no SDIO), shares SPI2 with
  display + LoRa.
- `BOARD_SDCARD_CS = GPIO 39`. Recommended max clock 75 MHz
  (Meshtastic `SD_SPI_FREQUENCY 75000000U`).

#### Connectivity / external pins

- **2 mm-pitch 6-pin Grove-ish header** on the side. Pins:
  `GPS_TX = GPIO 43`, `GPS_RX = GPIO 44`, plus 3.3 V, 5 V, GND, and
  one extra GPIO. On the original T-Deck this is free for any
  UART/I2C peripheral; it is the connector LilyGo uses for the
  optional GPS shield.
- 6-pin keyboard programming header (TX/RX/BOOT/RST/GND/VCC) for
  the on-keyboard ESP32-C3 — not for host MCU use.

#### Sensors

- **None.** No IMU, no compass, no barometer, no ALS.

---

### 1.3 T-Deck Plus — deltas vs. T-Deck

Same PCB family. Pin mapping is a **strict superset** of the T-Deck
mapping. CNX summarized it bluntly: "they have changed nothing and
put the T-Deck inside a case and called it a Plus." For driver
purposes that is true.

#### What changed vs. T-Deck

1. **GPS module is fitted by default**, hard-wired to the Grove
   header pins:
   - GPS TX → ESP32-S3 GPIO 44 (host RX)
   - GPS RX → ESP32-S3 GPIO 43 (host TX)
   - GPS chip: **either Quectel L76K (default baud 9600) or u-blox
     MIA-M10Q (default baud 38400)** depending on production batch.
     There is no production-side identifier visible from the host
     without probing UART. Robust firmware autobauds 9600/38400 and
     parses the version string.
   - PPS output: not routed on the T-Deck Plus (unlike the Pro,
     which has it on GPIO 1).
   - Consequence: the Grove connector is **no longer free** — the
     four signal pins are committed to GPS.
2. **Battery upgraded to 2000 mAh** Li-Polymer, built-in.
3. **ABS plastic case** with a 1/4"-20 tripod mount on the back.
4. **External-antenna SKU** is more common (still IPEX on-board,
   with a flex antenna).
5. Optional GT911 touch is more reliably present on Plus keyboards
   than on early T-Decks — some Plus units do route a GT911-driven
   touch surface. Firmware should probe I2C `0x5D`/`0x14` at boot
   and use it if present, but not require it.

#### What did NOT change

MCU, flash, PSRAM, LoRa chip and pinout, display chip and pinout,
audio codec, SD card pinout, keyboard, trackball, I2C/SPI bus pin
assignments — all identical to the original T-Deck. Same single-cell
Li-Po, same linear charger, same `GPIO 10` master power-enable, same
`GPIO 4` battery ADC.

---

### 1.4 T-Deck Pro — full spec

Source-of-truth: `Xinyuan-LilyGO/T-Deck-Pro` repo (`HD-V1-250326`,
`HD-V2-250915` schematic branches), `meshtastic/firmware/variants/
esp32s3/t-deck-pro/variant.h` and `t-deck-pro-v1_1/variant.h`, plus
the LilyGo wiki page for T-Deck-Pro.

The Pro is **not** a Plus-with-extras. It is a different PCB with a
different pin map. **Do not port T-Deck pin assignments to the Pro.**

#### MCU & memory

- Same package as T-Deck/Plus: **ESP32-S3FN16R8**, 16 MB flash, 8 MB
  octal PSRAM.
- **No secondary keyboard MCU** — the keyboard is a TI **TCA8418**
  I/O expander matrix scanner instead.

#### Buses

- **I2C0**: `SDA = GPIO 13`, `SCL = GPIO 14`. All on-board
  peripherals share this bus.
  - TCA8418 keyboard scanner: `0x34`, INT = GPIO 15
  - CST328 touchscreen controller: `0x1A`, INT = GPIO 12, RST =
    GPIO 45 (V1.0) / GPIO 38 (V1.1)
  - BHI260AP IMU: `0x28`, INT = GPIO 21
  - LTR-553ALS ambient light + proximity: `0x23`, INT = GPIO 16
  - BQ25896 charger: `0x6B`
  - BQ27220 fuel gauge: `0x55`
  - DRV2605 haptic driver (V1.1, MAX): `0x5A`, enable = GPIO 2
  - XL9555 I/O expander (MAX only): `0x20`
- **SPI2** (shared): `SCK = GPIO 36`, `MOSI = GPIO 33`, `MISO =
  GPIO 47`. CS pins per device.
- **UART1** to A7682E modem (when fitted): `RX (host) = GPIO 10`,
  `TX (host) = GPIO 11`, `RST = GPIO 9`, `RI = GPIO 7`, `DTR =
  GPIO 8`, `PWRKEY = GPIO 40`, `POWER_EN = GPIO 41`.
- **UART2** to GPS: `RX (host) = GPIO 44`, `TX (host) = GPIO 43`,
  `EN = GPIO 39` (V1.1; on V1.0 GPS_EN is GPIO 15 — verify against
  your unit), `PPS = GPIO 1`.

#### Power-enable rails (peripherals are off by default)

- `BOARD_1V8_EN = GPIO 38` (V1.0) — gyroscope 1.8 V rail.
- `LORA_EN = GPIO 46` — must be HIGH before LoRa SPI works.
- `PIN_GPS_EN = GPIO 39` (V1.1) / GPIO 15 (V1.0), active HIGH.
- `MODEM_POWER_EN = GPIO 41` — gates the A7682E supply.
- Backlight on V1.0 e-paper: `KB_BL_PIN = GPIO 42` is used for the
  **keyboard backlight only** (the e-paper has no backlight). On
  V1.1 a front-light is added: `TFT_BL = GPIO 45`.

#### Display

- **Panel:** GoodDisplay **GDEQ031T10** 3.1" e-paper, 320×240,
  4-grayscale or 1-bit. Active-matrix EPD with controller integrated
  in the FPC.
- **Bus:** SPI2 shared.
- **Pins:** `PIN_EINK_CS = GPIO 34`, `PIN_EINK_DC = GPIO 35`,
  `PIN_EINK_BUSY = GPIO 37`, `PIN_EINK_SCLK = GPIO 36`,
  `PIN_EINK_MOSI = GPIO 47`. The `47` value here conflicts with the
  documented shared `MISO = GPIO 47` — the two are wired to the
  same trace because the EPD is write-only (no MISO on the EPD).
  Drivers should ignore MISO for the EPD CS slot. Cross-check on
  schematic rev HD-V1-250326 if anything looks wrong.
- **EPD reset:** `PIN_EINK_RES = -1` on V1.0 (tied to system reset
  / power rail), GPIO 16 on V1.1.
- **Touch:** Hynitron **CST328** capacitive controller on the EPD
  glass. I2C `0x1A`, INT GPIO 12, RST GPIO 45 (V1.0) or 38 (V1.1).

#### Input

- **Keyboard:** TI **TCA8418** matrix scanner, I2C `0x34`, INT
  GPIO 15. Backlight on `KB_BL_PIN = GPIO 42`.
- **Buttons:** `BUTTON_PIN = 0` (BOOT/strap and user button).
- **Trackball:** **none** on the Pro — replaced by the touchscreen.
- **Haptic** (V1.1, MAX): TI DRV2605 driven via I2C, enable on
  GPIO 2 (`PIN_DRV_EN`), used as the new vibration / button-feedback
  channel.

#### Radio (LoRa)

- **Chip:** Semtech SX1262, same as T-Deck/Plus, but on a completely
  different SPI CS:
  - `LORA_CS = GPIO 3`
  - `LORA_DIO1 = GPIO 5` (IRQ)
  - `LORA_DIO2 = GPIO 6` — wired as **BUSY** (Meshtastic header
    aliases `SX126X_BUSY = LORA_DIO2 = GPIO 6`; DeepWiki and the
    LilyGo schematic show the BUSY net at GPIO 6).
  - `LORA_RESET = GPIO 4`
  - SCK 36 / MOSI 33 / MISO 47 (shared SPI2).
  - `LORA_EN = GPIO 46` — must be HIGH before access.
- **TCXO:** present, DIO3-controlled, **2.4 V**
  (`SX126X_DIO3_TCXO_VOLTAGE 2.4` in Meshtastic's Pro variant).
- **DIO2 as RF switch:** the Meshtastic Pro variant defines
  `SX126X_DIO2_AS_RF_SWITCH` AND maps `SX126X_BUSY = LORA_DIO2`.
  Both cannot be simultaneously true. Reading the actual T-Deck Pro
  schematic (`hardware/T-Deckpro v1.1`), DIO2 is the **BUSY** line
  broken out for host polling, and the TX/RX RF switch is on a
  separate antenna-side switch IC (not driven by the SX1262). Treat
  the RF switch define as legacy/no-op.
- **Frequency SKUs:** 433 / 868 / 915 / 920 MHz. Antennas are PCB /
  internal by default; "external antenna" SKUs add an IPEX bulkhead.

#### GPS

- **Chip:** u-blox **MIA-M10Q** (always — no L76K option on the
  Pro), supporting GPS, BeiDou, Galileo, GLONASS, QZSS.
- UART2: GPS_TX → host RX GPIO 44, GPS_RX → host TX GPIO 43, baud
  38400.
- **PPS** broken out on GPIO 1 (`PIN_GPS_PPS`).
- Power: `PIN_GPS_EN` gates the GPS regulator.

#### Cellular (optional, A7682E SKU)

- **Modem:** SimCom **A7682E** LTE Cat 1 (FDD-LTE B1/B3/B5/B7/B8/
  B20, 2G GSM/GPRS/EDGE 900/1800 MHz). Voice-capable; in the A7682E
  SKU the modem itself is the audio output path (no PCM5102A is
  fitted).
- AT-command UART on host UART1: RX 10, TX 11, plus PWRKEY (40),
  POWER_EN (41), RST (9), DTR (8), RI (7).
- Nano-SIM slot on the side of the case.

#### Audio

- **PCM5102A SKU only:** Texas Instruments **PCM5102A** stereo I2S
  DAC.
  - `PCM5102A_SCK  = GPIO 47` (BCLK)
  - `PCM5102A_DIN  = GPIO 17`
  - `PCM5102A_LRCK = GPIO 18`
  - No I2C control — PCM5102A is configured by hard-strap pins on
    the PCB; firmware just drives I2S.
  - PCM5102A on this board shares `47` with the SPI2 MISO net. The
    chip is muxed/enabled via `LORA_EN`-style gating in LilyGo
    example code; reads don't conflict in practice because PCM5102A
    only consumes I2S bit-clock and data, not MISO. Verify on
    schematic before relying on simultaneous SPI + audio.
- **A7682E SKU:** no PCM5102A. Audio in/out goes through the
  modem's built-in voice codec, controlled by AT commands. Firmware
  that wants to play arbitrary WAVs cannot do so on this SKU
  without going through the modem.
- **Microphone:** present in both SKUs (analog mic into either the
  PCM5102A's companion ADC path, or directly into the A7682E's
  voice codec — schematic-dependent).
- **Headphone jack:** **3.5 mm jack on the case side, present on
  both SKUs** of the Pro.

#### Storage

- microSD, SPI mode, on the shared SPI2 bus.
- `SDCARD_CS = SPI_CS = GPIO 48`.

#### Sensors

- **IMU:** Bosch **BHI260AP** "self-learning AI smart sensor" —
  16-bit 3-axis accelerometer + 3-axis gyro with on-chip Fuser2 DSP
  and built-in motion-classification firmware. I2C `0x28`, INT
  GPIO 21.
- **ALS / Proximity:** Lite-On **LTR-553ALS-WA**, I2C `0x23`, INT
  GPIO 16.
- **Barometer / compass:** none (despite some marketing implying a
  "9-DOF" sensor; the BHI260AP is 6-axis, no magnetometer).
- **Haptic driver** (V1.1+): TI **DRV2605L**, I2C `0x5A`, EN on
  GPIO 2.
- **I/O expander** (MAX only): NXP-style **XL9555** 16-channel I2C
  I/O expander, `0x20`.

#### Power

- **Charger:** TI **BQ25896** I2C-controlled 1-cell Li-ion charger,
  addr `0x6B`. Configurable charge current up to 3 A.
- **Fuel gauge:** TI **BQ27220** Impedance-Track gas gauge, addr
  `0x55`. Configured at boot for `BQ27220_DESIGN_CAPACITY = 1400`
  mAh.
- **Battery:** 1500 mAh (LilyGo product page) or 1400 mAh (some
  retailer listings and the Meshtastic gauge config). Production
  units appear to be 1400 mAh nominal — gauge config is
  authoritative.
- **USB:** USB-C, 5 V / 500 mA. Native ESP32-S3 USB CDC (no
  FT2232 / CP210x).
- **Power switch:** physical slide switch on the case (cuts battery
  to the system; USB still powers when off).

#### Connectivity / external pins

- **Qwiic / STEMMA-QT** I2C connector on the case (4-pin JST-SH
  1.0 mm). On the same I2C0 bus as on-board peripherals — addresses
  must not clash.
- **2×20 GPIO header** on the back PCB (under the case on the bare-
  board version), exposing unused GPIOs and 3.3 V/GND. Refer to the
  HD-V* schematic per revision for exact assignments.

#### Variant deltas inside the Pro family

| Feature | V1.0 | V1.1 | MAX |
|---|---|---|---|
| Schematic branch | HD-V1-250326 | HD-V2-250915 | HD-V3-250911 |
| Meshtastic variant | `t-deck-pro` | `t-deck-pro-v1_1` | (separate repo) |
| DRV2605 haptic (0x5A) | no | yes | yes |
| `PIN_VIBRATION` / `PIN_DRV_EN` | n/a | GPIO 2 | GPIO 2 |
| XL9555 I/O expander (0x20) | no | no | yes |
| EPD `RES` pin | -1 (none) | GPIO 16 | GPIO 16 |
| EPD `BL` (front-light) | none | GPIO 45 | GPIO 45 |
| CST328 RST | GPIO 45 | GPIO 38 | GPIO 38 |
| GPS_EN | GPIO 15 (some sources) | GPIO 39 | GPIO 39 |
| `BOARD_1V8_EN` | GPIO 38 | (folded into CST328 RST) | (same) |

LilyGo's recommended detection method: **scan I2C at boot.** Presence
of `0x5A` ⇒ V1.1 or MAX. Presence of `0x20` on top of that ⇒ MAX.
Otherwise V1.0.

#### Audio/Cellular SKU split (orthogonal to V1.0/V1.1/MAX)

| Suffix | Audio chain | Notes |
|---|---|---|
| **PCM5102A** ("Voice") | PCM5102A I2S DAC → Class-D amp → speaker; mic in | Full-fidelity audio under host control. No cellular. |
| **A7682E** ("4G") | Audio in/out via A7682E's voice codec, AT-commanded | Adds LTE Cat-1 + voice. Cannot play arbitrary I2S audio. |

LilyGo also brands "Meshtastic" and "MeshCore" pre-flashed editions
— firmware-side SKUs only; hardware identical to the corresponding
PCM5102A or A7682E unit.

---

### 1.5 Cross-variant pin map (quick reference)

GPIO numbers refer to the host ESP32-S3 in all cases.

| Function | T-Deck | T-Deck Plus | T-Deck Pro |
|---|---|---|---|
| Master power EN | 10 | 10 | LORA_EN 46, GPS_EN 39, MODEM_EN 41, 1V8_EN 38 |
| Battery ADC | 4 | 4 | (via BQ27220 fuel gauge over I2C) |
| BOOT button | 0 | 0 | 0 |
| I2C SDA | 18 | 18 | 13 |
| I2C SCL | 8 | 8 | 14 |
| Keyboard I2C addr | 0x55 (ESP32-C3) | 0x55 (ESP32-C3) | 0x34 (TCA8418) |
| Keyboard INT | 46 | 46 | 15 |
| Touch chip / addr | GT911 0x5D (sub-rev) | GT911 0x5D (sub-rev) | CST328 0x1A |
| Touch INT | 16 | 16 | 12 |
| Touch RST | — | — | 45 (V1.0) / 38 (V1.1) |
| SPI SCK | 40 | 40 | 36 |
| SPI MOSI | 41 | 41 | 33 |
| SPI MISO | 38 | 38 | 47 |
| LCD/EPD CS | 12 (LCD) | 12 (LCD) | 34 (EPD) |
| LCD/EPD DC | 11 | 11 | 35 |
| LCD/EPD BUSY | — | — | 37 |
| LCD/EPD BL | 42 (LCD BL) | 42 | 45 (V1.1 front-light) / 42 (KB BL) |
| LoRa chip | SX1262 | SX1262 | SX1262 |
| LoRa CS | 9 | 9 | 3 |
| LoRa BUSY | 13 | 13 | 6 (DIO2 net) |
| LoRa RST | 17 | 17 | 4 |
| LoRa DIO1 | 45 | 45 | 5 |
| LoRa TCXO voltage | 1.8 V | 1.8 V | 2.4 V |
| SD CS | 39 | 39 | 48 |
| SD bus | SPI2 (shared) | SPI2 (shared) | SPI2 (shared) |
| GPS chip | (none / optional) | L76K or u-blox MIA-M10Q | u-blox MIA-M10Q |
| GPS UART RX (host) | 44 | 44 | 44 |
| GPS UART TX (host) | 43 | 43 | 43 |
| GPS PPS | — | — | 1 |
| Mic / codec | ES7210 (I2S) | ES7210 | mic via PCM5102A path or A7682E |
| Audio I2S BCK | 7 | 7 | 47 (PCM5102A SCK) |
| Audio I2S WS / LRCK | 5 | 5 | 18 |
| Audio I2S DOUT / DIN | 6 | 6 | 17 |
| ES7210 MCLK / LRCK / SCK / DIN | 48 / 21 / 47 / 14 | same | n/a |
| Headphone jack | none | none | yes (3.5 mm) |
| Charger IC | unmarked linear | unmarked linear | BQ25896 (0x6B) |
| Fuel gauge | none | none | BQ27220 (0x55) |
| IMU | none | none | BHI260AP (0x28, INT 21) |
| ALS | none | none | LTR-553ALS (0x23, INT 16) |
| Haptic | none | none | DRV2605 V1.1+ (0x5A, EN 2) |
| I/O expander | none | none | XL9555 MAX-only (0x20) |
| Cellular | none | none | A7682E (UART1: RX 10, TX 11, PWRKEY 40, POWER_EN 41) |
| External I2C | none | none | Qwiic/STEMMA-QT |
| External UART/I2C | Grove (free) | Grove (used by GPS) | Qwiic + 2×20 GPIO header |
| Battery | external/varies | 2000 mAh built-in | 1400/1500 mAh built-in |
| USB | USB-C native | USB-C native | USB-C native |
| WiFi/BT antenna | PCB trace | PCB trace | PCB trace |
| LoRa antenna | PCB trace OR IPEX | PCB trace OR IPEX | PCB trace OR IPEX |

---

### 1.6 Source disagreements / things to verify on your hardware

- **GT911 touch on the original T-Deck and Plus:** Meshtastic's
  variant.h declares it; LilyGo wiki and CNX writeups say standard
  input is trackball-only. Reality is sub-revision-dependent. Probe
  I2C `0x5D`/`0x14` at boot; treat touch as optional.
- **T-Deck/Plus TCXO voltage:** LilyGo `utilities.h` examples imply
  1.8 V; some forks use 2.4 V. The Meshtastic Pro variant uses 2.4
  V definitively. For T-Deck/Plus, 1.8 V is the more common choice
  in working firmware. Wrong value manifests as ~30 kHz frequency
  error reported by RadioLib.
- **T-Deck Plus GPS chip:** L76K vs u-blox MIA-M10Q is batch-
  dependent; autobaud (9600 / 38400) and parse the version string.
- **T-Deck Pro V1.0 GPS_EN pin:** LilyGo wiki says GPIO 15,
  Meshtastic V1.1 variant.h says GPIO 39. Schematic branch
  HD-V1-250326 is authoritative for V1.0.
- **T-Deck Pro `PIN_EINK_MOSI = 47` colliding with `SPI_MISO = 47`:**
  intentional bus-sharing because the EPD is write-only. Drivers
  must not attempt MISO reads on the EPD CS slot.
- **T-Deck Pro DIO2 dual-purpose define:** Meshtastic defines
  `SX126X_DIO2_AS_RF_SWITCH` AND aliases `SX126X_BUSY = DIO2`.
  Schematic shows DIO2 as BUSY; the RF switch define appears to be
  legacy. Treat BUSY as authoritative.

---

### 1.7 Authoritative source files (cite these in driver code)

- **LilyGo T-Deck / Plus pinout:**
  `Xinyuan-LilyGO/T-Deck/examples/UnitTest/utilities.h`.
- **LilyGo T-Deck-Pro pinout & schematics:**
  `Xinyuan-LilyGO/T-Deck-Pro` repo, `hardware/` directory
  (`HD-V1-250326`, `HD-V2-250915`, `HD-V3-250911`).
- **Meshtastic firmware definitions:**
  - `meshtastic/firmware/variants/esp32s3/t-deck/variant.h`
  - `meshtastic/firmware/variants/esp32s3/t-deck-pro/variant.h`
  - `meshtastic/firmware/variants/esp32s3/t-deck-pro-v1_1/variant.h`
- **Ratdeck (Reticulum on T-Deck Plus) hardware mapping:**
  `ratspeak/ratdeck/src/config/BoardConfig.h` (matches `utilities.h`
  for LoRa, display, SD, trackball pins). Also `ratdeck/docs/
  PINMAP.md` if/when published — the docs/ dir was empty at our
  snapshot.
- **RNode_Firmware_CE T-Deck definitions:**
  `liberatedsystems/RNode_Firmware_CE/Boards.h` — defines
  `BOARD_TDECK = 0x3B`, models `MODEL_D4` (433 MHz) and `MODEL_D9`
  (868 MHz). Interface-pin tuple for SX1262: `{9, 40, 41, 38, 13,
  45, 17, -1, -1, -1}` = `{CS, SCK, MOSI, MISO, BUSY, DIO1, RST,
  …}`. **No T-Deck-Plus or T-Deck-Pro entry exists in
  RNode_Firmware_CE as of May 2026.**
- **LilyGo wiki:**
  - `wiki.lilygo.cc/get_started/en/Wearable/T-Deck-Plus/T-Deck-Plus.html`
  - `wiki.lilygo.cc/get_started/en/Wearable/T-Deck-Pro/T-Deck-Pro.html`
- **DeepWiki hardware references:**
  `deepwiki.com/Xinyuan-LilyGO/T-Deck/3-hardware-reference`,
  `deepwiki.com/Xinyuan-LilyGO/T-Deck/3.4-radio-module`,
  `deepwiki.com/Xinyuan-LilyGO/T-Deck-Pro`.
- **Meshtastic device docs:**
  `meshtastic.org/docs/hardware/devices/lilygo/tdeck/`.

### 1.8 reticulous on-device UI — how we wire it

The on-device UI is **spangap-core's `lcd` LVGL component** (launcher + status
bar + built-in Settings), gated on `CONFIG_SPANGAP_LCD` — not a reticulous widget
set. The software architecture (LVGL bring-up, the lcd task loop, the focus
group, Settings panes, the panel Kconfig + input HAL contract) lives in
[../../spangap/docs/lcd.md](../../spangap/docs/lcd.md). This section is only the
**T-Deck Plus hardware wiring** behind that contract; the board layer is
[../main/tdeck.cpp](../main/tdeck.cpp).

**Display.** ST7789V (320×240, RGB565) on the shared SPI2 bus via `esp_lcd` —
CS 12, DC 11, no RST (resets with the +3.3 V rail behind GPIO 10), backlight LEDC
PWM on GPIO 42. These are the lcd component's `CONFIG_LCD_*` (set in
`sdkconfig.defaults`), not board code — `lcd_panel.cpp` brings the panel up and
the board contributes no display driver. LVGL renders into two ~6-line DMA strips
(`LV_DISPLAY_RENDER_MODE_PARTIAL`); there is **no full framebuffer in RAM** to
read back.

**The lcd-owned input is interrupt-driven; the keyboard is the exception.**
Touch, trackball and button indevs are `LV_INDEV_MODE_EVENT`, so LVGL runs no
read timer for them. The board attaches spangap-core's exported `lcdInputISR` to
each INT line; the ISR does nothing but flag + `vTaskNotifyGiveFromISR` the lcd
task (whose `itsPoll` blocks on that notification), which then reads the indev
once. With nothing held, idle lcd CPU is **~0 %** (it pauses any released indev's
LVGL read timer each loop so a missed pointer release-pause can't leave LVGL
auto-reading at 30 Hz). The **keyboard can't join this model** (dead INT +
destructive read), so it lives in the board module
([`main/tdeck.cpp`](../main/tdeck.cpp)): its own poll task does the I2C off the
lcd task and bumps lcd via `lcdRun()`.

| Device | INT pin(s) | Edge | Read path | LVGL indev | Owner |
|---|---|---|---|---|---|
| GT911 touch | GPIO 16 | `ANYEDGE` | I2C via `esp_lcd_touch` → `touch_read` (raw native; lcd rotates) | pointer | `tdeck.cpp` input HAL |
| Trackball — 4 direction lines | GPIO 3 / 15 / 1 / 2 (U/D/L/R) | `NEGEDGE` | count falling edges → cursor position (`pointer_read`) | pointer (visible cursor) | `tdeck.cpp` input HAL |
| Centre button | GPIO 0 | `ANYEDGE` | `gpio_get_level` → `click_read` (board owns click-vs-300ms-hold → `lcdGoHome`) | → the trackball pointer's click | `tdeck.cpp` input HAL |
| QWERTY keyboard (C3) | GPIO 46 (dead — see above) | `ANYEDGE` | I2C 1-byte read @ `0x55`, **polled** | keypad | `tdeck.cpp` (not lcd) |

- **`ANYEDGE` for touch / button / keyboard INT:** INT polarity is sub-revision /
  C3-firmware dependent, and a redundant edge just costs one empty read. The
  keyboard's GPIO 46 is wired ANYEDGE only so its `tdeck.cpp` ISR can detect
  the (currently never-fired) edge and self-heal to interrupt-driven; until then
  the poll task drains the C3 FIFO each wake until the read returns 0.
- **Trackball → mouse pointer, with acceleration.** Each direction line pulses
  (active-low) as the ball rolls that way; the four `NEGEDGE` ISRs just count
  (under a spinlock shared with the reader). `tdeckPointerRead()` integrates the
  counts into an absolute position, clamped to the panel. The per-pulse **step is
  velocity-dependent**: ~**1 px/pulse when rolling slowly** (pixel-precise for
  clicks) ramping linearly to **`s.tdeck.trackball_speed` px/pulse at a fast
  flick** (default 16). A flat step can't win — fast enough to cross the screen
  means jumps too coarse to click — so this was the fix. To avoid jerk the pulse
  rate is a **time-decayed EMA** (`TB_VEL_TAU_US` ≈ 120 ms): a short gap barely
  moves it, a long gap decays it to zero so the first nudge after a pause stays
  precise. `TB_VEL_FULL` (pulses/sec for full speed) and `TB_VEL_TAU_US` are
  compile-time tunables in [tdeck.cpp](../main/tdeck.cpp); the slider sets
  the fast-end ceiling. **reticulous owns the whole pointing device** — both the
  curve and the settings (`s.tdeck.*`). spangap-core stays generic: it only knows
  the `pointer_read` HAL hook and draws the cursor — it owns no pointer config.
  reticulous pushes the cursor dwell in via `lcdPointerSetVisibleMs`
  (`s.tdeck.pointer_visible_time`, default 2 s, `-1` = always). The centre button
  is the pointer's **click** (short press → click under the cursor, ≥1 s hold →
  Home), so on a board with `pointer_read` lcd does **not** create a keypad button
  indev. **Direction→pin and ball orientation are sub-revision dependent** (a
  sample had DOWN/RIGHT swapped) — flip `BOARD_TBOX_*` or the `dx/dy` signs in
  [tdeck.h](../main/tdeck.h) / [tdeck.cpp](../main/tdeck.cpp) if it feels
  wrong.
- **`T-Deck` Settings panel** (root of on-device Settings, registered during
  `tdeckPreInit`): **Trackball** → Pointer speed (`s.tdeck.trackball_speed`,
  4–40) + Cursor dwell (`s.tdeck.pointer_visible_time`, 1–30 s; `-1`/always stays
  CLI/browser-only); **Display** → Backlight (`s.lcd.backlight`).
- **Touch tracking** while a finger is down is a 10 ms `lv_timer`, created on
  press and deleted on release (the GT911 INT only guarantees the first edge).
- **GPIO ISR service** is installed with `ESP_INTR_FLAG_IRAM` so the IRAM-safe
  `lcdInputISR` survives cache-disabled windows — the **same flag LoRa's DIO1
  path uses** ([../main/esp_idf_hal.cpp](../main/esp_idf_hal.cpp)). Whichever of
  `tdeck.cpp` / `esp_idf_hal.cpp` runs first installs it; the other tolerates
  `ESP_ERR_INVALID_STATE`.

---

## Part 2 — Ratdeck firmware architecture

The reference firmware we're learning from. Source is at
`ratspeak/ratdeck`, cloned locally at
[research/ratdeck/](../research/ratdeck/) (gitignored). License is
AGPL-3.0-or-later — read for design lessons, do not copy code.

### 2.1 Repository overview

[ratspeak/ratdeck](https://github.com/ratspeak/ratdeck) is ratspeak's
flagship "all-in-one" firmware for the LilyGo T-Deck Plus
(ESP32-S3). No companion phone or Sideband bridge required, although
a BLE Sideband interface is also exposed.

**Build system.** PlatformIO with the Arduino-ESP32 framework, **not
ESP-IDF**. Relevant `platformio.ini` entries:

```
platform = espressif32@6.7.0          # Arduino-ESP32 ~ 2.0.x, IDF 4.4.x
board    = esp32-s3-devkitc-1
framework = arduino
board_build.flash_size = 16MB
board_build.partitions = partitions_16MB.csv
board_build.arduino.memory_type = qio_opi
build_flags =
    -std=gnu++17 -fexceptions -O2
    -DRATDECK=1 -DARDUINO_USB_CDC_ON_BOOT=1 -DARDUINO_USB_MODE=1
    -DRNS_USE_FS -DRNS_PERSIST_PATHS
    -DRNS_DEFAULT_ALLOCATOR=RNS_PSRAM_ALLOCATOR
    -DRNS_CONTAINER_ALLOCATOR=RNS_PSRAM_POOL_ALLOCATOR
    -DRNS_PSRAM_POOL_BUFFER_SIZE=2048000
    -DRNS_KNOWN_DESTINATIONS_MAX=256
    -DRNS_HASHLIST_MAX=256 -DRNS_PATH_TABLE_MAX=256
    -DRNS_ANNOUNCE_TABLE_MAX=64 -DRNS_RECEIPTS_MAX=32
    -DRNS_QUEUED_ANNOUNCES_MAX=16
    ...
lib_deps =
    https://github.com/ratspeak/microReticulum.git#858010cee03f0b94f645726a516f8827ba02075f
    bblanchon/ArduinoJson@^7.4.2
    lovyan03/LovyanGFX@~1.1.16
    lvgl/lvgl@~8.3.4
```

Things to flag:

- `espressif32@6.7.0` pins to an Arduino-ESP32 release built on
  **IDF 4.4.x**. Ratdeck does not use the IDF 5.x toolchain.
  Everything is Arduino-on-IDF: they `#include <esp_netif.h>` only
  as plumbing, not for service registration.
- Reticulum is configured as an **endpoint client**, not a transport
  node: hashlist, known-destinations, path table all sized at 256,
  announce table 64, receipts 32. Whole RNS state fits in a single
  2 MB PSRAM pool (`RNS_PSRAM_POOL_BUFFER_SIZE=2048000`) so nothing
  fragments the heap.
- `-DRNS_USE_FS` and `-DRNS_PERSIST_PATHS` enable RNS's filesystem-
  backed persistence layer, implemented via LittleFS.
- microReticulum is pinned to a specific ratspeak fork commit
  (`858010cee0…`); they do not track upstream attermann.

**License.** `LICENSE` is **AGPL-3.0-or-later**. Vendored
`lib/Crypto/` (Arduino Crypto by Rhys Weatherley) keeps its **MIT**
license. AGPL changes the threat model: anyone serving Reticulum-
mediated communications using ratdeck-derived code must offer source
to remote users. Clean-room reimplementation against the spangap
ESP-IDF tree side-steps this entirely; nobody on the team should
copy-paste ratdeck source.

**Top-level layout.**

```
.github/workflows/      # CI builds firmware artifacts
assets/                 # README images
lib/Crypto/             # vendored MIT-licensed crypto primitives
src/                    # all firmware code (see below)
lv_conf.h               # LVGL configuration (PROJECT_DIR include)
partitions_16MB.csv     # 6-partition layout
platformio.ini
merge_firmware.py       # post-build script producing combined .bin
```

There is **no `docs/` directory** in the tree despite README links
to `docs/QUICKSTART.md` etc.; those links 404. Docs live only in
`README.md` and GitHub Releases notes.

**Partition layout** (`partitions_16MB.csv`):

```
nvs       data nvs      0x9000   0x5000
otadata   data ota      0xe000   0x2000
app0      app  ota_0    0x10000  0x400000     # 4 MB
app1      app  ota_1    0x410000 0x400000     # 4 MB
littlefs  data spiffs   0x810000 0x7E0000     # ~7.9 MB
coredump  data coredump 0xFF0000 0x10000
```

The LittleFS partition is declared with `SubType=spiffs` — required
by the Arduino-ESP32 LittleFS driver, which only enumerates
partitions of that subtype. A recent commit ("FlashStore: fall back
to 'spiffs' partition label") confirms they hit and resolved this.

**Source tree.**

```
src/
├── main.cpp                       # one giant Arduino sketch
├── lv_conf.h                      # local LVGL config
├── audio/      AudioNotify.{h,cpp}
├── config/     BoardConfig.h Config.h UserConfig.{h,cpp}
├── fonts/                         # custom LVGL fonts
├── hal/        Display.{h,cpp} GPSManager.{h,cpp} Keyboard.{h,cpp}
│               NMEAParser.h Power.{h,cpp} TouchInput.{h,cpp}
│               Trackball.{h,cpp}
├── input/      HotkeyManager.{h,cpp} InputManager.{h,cpp}
├── radio/      RadioConstants.h SX1262.{h,cpp}
├── reticulum/  AnnounceManager.{h,cpp} IdentityManager.{h,cpp}
│               LXMFManager.{h,cpp} LXMFMessage.h
│               ReticulumManager.{h,cpp}
├── storage/    FlashStore.{h,cpp} MessageStore.{h,cpp} SDStore.{h,cpp}
├── transport/  AutoInterfaceWrapper.{h,cpp} BLEInterface.{h,cpp}
│               BLESideband.{h,cpp} LoRaInterface.{h,cpp}
│               TCPClientInterface.{h,cpp} WiFiInterface.{h,cpp}
└── ui/         LvInput.{h,cpp} LvStatusBar.{h,cpp} LvTabBar.{h,cpp}
                LvTheme.{h,cpp} LxmFaceAvatar.{h,cpp}
                Theme.h UIManager.{h,cpp}
                screens/         # 13 LVGL screens
```

Architecture is straightforward — every concern has a `*Manager`
class and `main.cpp` wires them together.

### 2.2 Driver inventory

All drivers are **custom code in `src/hal/` and `src/radio/`** built
on Arduino primitives (`Wire`, `SPI`, `attachInterrupt`,
`analogRead`, `i2s_*`). There is no LovyanGFX-style auto-detect, no
LVGL HAL helper. Pin assignments live in
`src/config/BoardConfig.h` — they match the canonical T-Deck pin map
from §1.2. (The `BoardConfig.h` file is the cleanest single-file
summary of T-Deck Plus pins available.)

**LoRa (`src/radio/SX1262.{h,cpp}`).** Custom bare-metal SPI driver,
**not RadioLib**. Header comment notes "Direct port from Ratputer"
with "only change: pin assignments via BoardConfig.h." TX is polled
(busy-wait on `IRQ_TX_DONE_MASK_6X` with timeout) and there is an
async path that sets `_txActive` and exposes `isTxBusy()`. RX is
interrupt-driven via DIO1: an ISR sets
`volatile bool packetAvailable = true` and **deliberately does no
SPI work**, because the SX1262 shares the SPI bus with the display
and SD card; touching SPI from interrupt context would deadlock.
Main loop polls `packetAvailable` and drains the FIFO synchronously.

**LoRa interface (`src/transport/LoRaInterface.{h,cpp}`).** Subclass
of `RNS::InterfaceImpl`. Reconciles half-duplex LoRa with RNS's 500-
byte MTU. Single SX1262 frame is capped at 254 bytes, so packets
larger than that are **split across two LoRa frames using a sequence
nibble**. Outgoing frames queued; loop transmits when radio is idle
and not in the middle of a split RX. Per-window airtime accounting,
`SPLIT_RX_TIMEOUT_MS` for orphaned half-packets. RNS-side MTU is
held at 500 to keep wire compat with vanilla Reticulum on bigger
radios.

**Display (`src/hal/Display.{h,cpp}`).** LovyanGFX with a custom
`LGFX_TDeck` panel definition. ST7789V at 320×240 in landscape
(`setRotation(1)`). LVGL flush callback uses `startWrite` /
`setAddrWindow` / `pushPixels` synchronously into double-buffered
20-line strips allocated from PSRAM with `MALLOC_CAP_DMA` preferred.
Inline comment: "blocking pushPixels prevents SPI bus contention
with SX1262 radio on shared FSPI bus." Brightness PWM via
`_gfx.setBrightness()`.

**UI toolkit.** **LVGL 8.3.4**, fed via a small `LvInput` shim
(`src/ui/LvInput.{h,cpp}`). Custom theme (`LvTheme`). Status bar and
tab bar are hand-rolled widgets, not LVGL tabview. 13 screens under
`src/ui/screens/`.

**Keyboard (`src/hal/Keyboard.{h,cpp}`).** I2C to the on-board
ESP32-C3 at addr 0x55 via `Wire`. `readKey()` does
`Wire.requestFrom(addr, 1)` because requesting more than one byte
returns garbage on stock LilyGo firmware. Polled; INT line (GPIO
46) is wired but not used as an interrupt. Repeat keys filtered by
comparing to previous key.

**Trackball (`src/hal/Trackball.{h,cpp}`).** Five GPIO-interrupt
lines (UP/DOWN/LEFT/RIGHT/CLICK), `attachInterrupt(..., FALLING)`
each. Each ISR increments a `volatile int delta`. **No debouncing**
— they accept the noise. There's a hardware-orientation quirk: on
their unit the DOWN pin generates rightward motion and RIGHT
generates downward.

**Touch (`src/hal/TouchInput.{h,cpp}`).** GT911 at I2C addr 0x5D
(fallback 0x14). Reads status from register 0x814E and point data
from 0x814F+. Polled but throttled by the INT line — only re-reads
after the controller signals new data.

**SD card (`src/storage/SDStore.{h,cpp}`).** Standard Arduino `SD`
library on the **shared SPI bus** (CS=39). Used as primary
persistence store when present, with LittleFS as fallback. Mounts a
`/ratdeck/` tree containing `config/`, `messages/`, `contacts/`,
`identity/`, and `transport/`. Atomic-write wrapper writes to
`path.tmp` → fsync → `path.bak ← old` → `rename path.tmp → path`
with `.bak` recovery on read. **No SDMMC** — 4-bit mode is not
used; everything on the same SPI as radio + display, which is part
of why the LoRa ISR is so careful.

**Audio (`src/audio/AudioNotify.{h,cpp}`).** Native Arduino-ESP32
`i2s.h` driver, not the codec helpers. Configured
`I2S_MODE_MASTER | I2S_MODE_TX`, 16-bit PCM @ 16 kHz. **All sounds
synthesized in-memory** as sine + 2nd + 3rd harmonic with linear
fade envelopes. No PCM assets in flash. Notification types: tone(f,
d), error (3×400 Hz), announce (800 Hz), boot arpeggio.

**Power (`src/hal/Power.{h,cpp}`).** **No PMIC driver code.** The
T-Deck Plus has no PMIC anyway (see §1.3); they just toggle PWR_EN
(GPIO 10) high at boot and read battery voltage via ADC on GPIO 4
through a 2× divider. Battery percentage is a piecewise linear
approximation across 3.0–4.2 V LiPo curve. Display brightness and
keyboard backlight are independent PWM channels.

**GPS (`src/hal/GPSManager.{h,cpp}` + `NMEAParser.h`).** UBlox over
UART, 8N1, **autobaud on boot** by trying a `BAUD_RATES[]` table.
Hand-written NMEA parser. UART read rate-limited to 64 bytes per
`loop()` iteration to keep cooperative pump responsive. Time policy:
epoch restored from NVS on boot (so clock has *some* value before
fix), then trusted only when satellites > 0, with a sanity gate
that rejects timestamps outside 2024–2030 to defeat spoofs, then
re-persisted to NVS at intervals.

### 2.3 Reticulum integration

**microReticulum fork.** Ratdeck depends on
[ratspeak/microReticulum](https://github.com/ratspeak/microReticulum)
pinned to commit `858010cee03f0b94f645726a516f8827ba02075f`. Upstream
is [attermann/microReticulum](https://github.com/attermann/microReticulum)
(latest 0.3.1, May 2026). Upstream is **Apache-2.0**; the ratspeak
fork inherits Apache-2.0 — separate from ratdeck's AGPL.

The big functional delta of ratspeak's fork is **LXMF wire-format
support**. Upstream microReticulum is "almost entirely complete
except LXMF" per discuss.reticulum.network; ratspeak added an
`LXMFMessage` class plus the link/resource glue needed for
opportunistic and on-demand-link delivery. Ratdeck's
`src/reticulum/LXMFMessage.h` is a 4-line forward header:

```cpp
// LXMFMessage is now provided by the microReticulum library.
// This forward header ensures existing #include "LXMFMessage.h" still resolves.
#pragma once
#include <LXMFMessage.h>
```

So the LXMF type lives in mR; the **LXMF orchestration** (queue,
dedup, conversation index, link-establishment policy, retry,
contacts) lives in `src/reticulum/LXMFManager.cpp` in ratdeck.

**Manager structure.** Three classes carry RNS state:

- `ReticulumManager` (`src/reticulum/ReticulumManager.{h,cpp}`) —
  owns `RNS::Reticulum`, `RNS::Identity`, `RNS::Destination`, the
  LoRa interface, and the file system glue. Provides `loop()`,
  `persistData()`, `announce()`, accessors for identity/destination
  hash, transport active state, link counts.
- `IdentityManager` (`src/reticulum/IdentityManager.{h,cpp}`) —
  multi-slot identity storage (`id_0.key`, `id_1.key`, …) with JSON
  metadata describing slot, display name, active flag.
- `AnnounceManager` (`src/reticulum/AnnounceManager.{h,cpp}`) —
  subclass of `RNS::AnnounceHandler`, registered for the LXMF
  aspect. Maintains a `vector<DiscoveredNode>` (the Peers tab) and a
  name cache. Rate limiting (`MAX_GLOBAL_ANNOUNCES_PER_SEC`,
  default 10/s), per-node minimum interval
  (`ANNOUNCE_MIN_INTERVAL_MS`), and deferred persistence (saves
  every 30 s rather than per announce).

**Initialization sequence** (from `ReticulumManager::begin()`):

1. Register a `LittleFSFileSystem` (subclass of
   `RNS::FileSystemImpl`) with RNS so that `RNS_USE_FS` writes
   through LittleFS.
2. Clear stale `destination_table` and `packet_hashlist` files from
   previous runs (avoid resurrecting dead state across firmware
   updates).
3. Optionally restore known-destinations from SD backup.
4. Construct `LoRaInterface`, register with
   `RNS::Transport::register_interface()`.
5. Construct `RNS::Reticulum` in **endpoint mode** (not transport).
   Install a transport-level filter callback that rate-limits
   forwarded announces.
6. `loadOrCreateIdentity()` — try LittleFS, then NVS Preferences,
   otherwise generate a new `RNS::Identity` and persist.
7. Create the LXMF `RNS::Destination` with aspect `lxmf/delivery`.

**Where extra interfaces get registered.** Each transport interface
(`WiFiInterface`, `TCPClientInterface`, `BLEInterface`,
`BLESideband`, `AutoInterfaceWrapper`) is its own
`RNS::InterfaceImpl` subclass, instantiated by `main.cpp` after
`ReticulumManager::begin()` returns and registered via
`RNS::Transport::register_interface()`. Sideband and BLE are
conditional on `bleEnabled` user config; AutoInterface and TCP
client connections obey their own enable flags and have async
startup paths to avoid blocking boot when WiFi is unavailable.

**Where the LXMF destination is created.** Inside
`ReticulumManager::begin()`. `LXMFManager::begin(rns, store)` then
calls:

```cpp
RNS::Destination& dest = _rns->destination();
dest.set_packet_callback(onPacketReceived);
dest.set_link_established_callback(onLinkEstablished);
```

So the destination is owned by `ReticulumManager` and `LXMFManager`
is just a callback consumer plus an outbound queue.

### 2.4 Concurrency model

The sharpest single observation. **Ratdeck is a single-threaded
cooperative event loop.** No `xTaskCreate` /
`xTaskCreatePinnedToCore` in `main.cpp` — everything runs on the
Arduino main loop, which is one FreeRTOS task with default Arduino
stack and priority. Radio, RNS pump, LVGL renderer, input poll,
persistence flush, GPS UART drain, BLE Sideband packet drain, TCP
frame drain — all sequential in `loop()`.

Two FreeRTOS exceptions:

1. **TCPClientInterface** spawns a short-lived FreeRTOS task in
   `tryConnect()` (`connectTaskFn`) so blocking `WiFiClient.connect()`
   doesn't stall the main loop for a TCP SYN timeout. Once
   connected, frame I/O moves back to `loop()`.
2. **BLE / NimBLE** runs the BLE stack on its own NimBLE task by
   virtue of the library. Incoming frames on `BLESideband` /
   `BLEInterface` pushed onto a queue under a mutex; `loop()` drains
   them.

That's it. No dedicated radio task, no dedicated RNS task, no
dedicated UI task. Reason given in code comments: microReticulum's
transport state isn't thread-safe — it assumes a single owner — and
shoving the radio onto a second core would race against transport.

Sync primitives:
- One `portMUX_TYPE` / `noInterrupts/interrupts` pair guarding
  trackball delta counters against ISRs.
- One `volatile bool packetAvailable` plus implicit deferral as the
  ISR/main-loop handshake for SX1262 RX.
- One mutex (`SemaphoreHandle_t`) guarding the BLE inbound queue.
- LittleFS atomic-write recipe substituting for write-locks.

**Loop structure** (sequenced phases per main-loop iteration):

```
1.  inputMgr.update()              # keyboard, trackball, touch
2.  long-press / wake handling
3.  hotkey dispatch -> overlays / LVGL feed / tab cycle
4.  lv_timer_handler()             # ~30 FPS, ~5 FPS dimmed
5.  rns.loop()                     # SX1262.parsePacket() at ~100 Hz
6.  auto-announce timer
7.  lxmf.loop() + announceMgr.loop()
8.  WiFi STA reconnect (exp. backoff 5/15/60/300 s)
9.  AutoInterface lifecycle (deferred IPv6 LL bring-up, SLAAC churn watch)
10. TCP reload (deferred config apply)
11. WiFi/TCP loops, rate-limited under RNS load
12. BLE loops (conditional)
13. GPS UART drain (≤64 B, non-blocking)
14. Power: brightness, dim, blank
15. Status bar (1 Hz: battery, TCP, AutoIface, GPS, time)
16. RSSI sampler (5 s, hotkey-toggled)
17. Heartbeat diag (5 s heap/links)
```

Release notes contain a telling line: **"v1.7.3 — Main loop
optimized from 9 seconds to ~100ms."** They had a phase early on
where one full `loop()` iteration could take 9 s. That's what
cooperative scheduling on top of `RNS::Transport` looks like when
somebody adds a synchronous network call in the wrong place. For
the spangap ESP-IDF reimplementation, this is the strongest lesson:
**co-locating the RNS pump, the radio driver, and the UI on a
single cooperative task means every long-running call somewhere
becomes a UI freeze and a missed LoRa packet.** The spangap/seccam
codebase already structures around explicit FreeRTOS tasks and
event posting; do not regress to single-task cooperative just
because mR encourages it.

### 2.5 UI architecture

**Toolkit:** LVGL 8.3.4. **Manager:** `UIManager`
(`src/ui/UIManager.{h,cpp}`) implements **single-active-screen**
semantics, not a navigation stack. `setScreen(next)` calls
`current->onExit(); current->destroyUI(); next->createUI();
next->onEnter();`. Each screen is a class with virtual
`createUI/destroyUI/onEnter/onExit`.

**Screens (`src/ui/screens/`).** Boot, Home, Contacts, Nodes,
Messages, MessageView, Settings, NameInput, Timezone, Map, QR
(overlay), Help (overlay), DataClean. The five tabs in the README
(Home/Friends/Msgs/Peers/Setup) map to
Home/Contacts/Messages/Nodes/Settings.

**Tab and status bars** are hand-rolled (`LvTabBar`, `LvStatusBar`)
because LVGL tabview misbehaved with their input model. `LvTheme`
centralizes colors and font choices including custom Cyrillic /
Latin-Extended fonts in `src/fonts/`. `LxmFaceAvatar` integrates
with [ratspeak/LXMFace](https://github.com/ratspeak/LXMFace) —
deterministic visual identity blocks derived from the destination
hash, like blockies for Ethereum addresses.

**Input plumbing.** `InputManager`
(`src/input/InputManager.{h,cpp}`) consolidates
keyboard/trackball/touch into a unified `KeyEvent`. Trackball deltas
accumulated, thresholded, converted to discrete up/down/left/right
key events with configurable speed. Bag-suppression: when device
looks blanked, phantom inputs dropped to avoid waking from a
backpack. `HotkeyManager` (`src/input/HotkeyManager.{h,cpp}`)
intercepts `Ctrl+key` combinations before LVGL sees them; everything
else feeds LVGL via the input shim in `src/ui/LvInput.cpp`. Tab
cycling is handled in main loop *after* the screen has consumed the
key.

**Frame-rate adaptive throttling.** LVGL pumped at ~30 FPS when
screen active, ~5 FPS when dimmed; any input activity bypasses the
throttle for one frame. Deliberate trade-off against the LoRa loop
budget.

### 2.6 Persistence

**Three storage tiers**, in priority order: SD card (when present
and enabled), LittleFS (always), NVS Preferences (small key-value).

**LittleFS layout** (FlashStore at `/`):
- `/identity/` — `id_0.key`, `id_1.key`, … plus `slots.json`
  metadata
- `/messages/<peer-hex-32>/` — per-conversation message files
- `/contacts/<peer-hex-32>.json` — per-contact discovered-node
  records
- `/transport/` — RNS-managed `destination_table`,
  `packet_hashlist`, `path_table`, etc. (microReticulum
  filesystem-backed persistence)
- Various config blobs

**SD layout** (SDStore at `/ratdeck/`): same subtree (`config/`,
`messages/`, `contacts/`, `identity/`, `transport/`).

**Atomic write recipe** (in both `FlashStore::writeAtomic` and
`SDStore::writeAtomic`):
1. write to `path.tmp`, verify length
2. rename existing `path` → `path.bak`
3. rename `path.tmp` → `path`
4. delete `path.bak`

Reads fall back to `path.bak` if `path` is missing or short.
LittleFS error logs are suppressed at compile time because the
rename-during-write pattern emits "file not found" warnings during
normal operation.

**Formats.**
- **Identities:** raw 64-byte private-key blob via
  `_flash->writeAtomic(keyPath, privKey.data(), privKey.size())`.
  Slot index, display name, path, active flag are JSON in
  `slots.json`.
- **Messages:** **JSON** via ArduinoJson 7. Filenames are counter-
  prefixed with direction suffix: `0000000000001_i.json` (incoming),
  `..._o.json` (outgoing). Fields: `src`, `dst`, `ts`, `content`,
  `title`, `incoming`, `status`, `read`, `msgid`.
- **Contacts:** JSON, one file per contact (hash → display-name +
  last-seen + RSSI + interface metadata).
- **NVS Preferences:** GPS epoch checkpoint, boot-loop counter
  (used to detect partial-boot loops and reset persisted state),
  last-active identity hash.
- **microReticulum's own state** (paths, hashlist, destination
  table) goes through the `LittleFSFileSystem` adapter — those
  files are mR's wire format (mix of msgpack and binary), not JSON.

For spangap: the `IStorage` abstraction ratdeck's `FlashStore` /
`SDStore` express maps cleanly to the existing seccam storage
layer. JSON-per-message is operationally easy but inflates flash —
~200 bytes per message minimum even for small payloads. Worth
considering CBOR/msgpack with a per-conversation index file for
thousands of messages.

### 2.7 LXMF specifics

**Inbox structure.** Conversations indexed by peer destination
hash. `MessageStore::_conversations` is a `vector<string>` of peer
hex IDs; per-peer message files live under `/messages/<peer-hex>/`
numbered monotonically and tagged `_i`/`_o`. `getMessages(peerHex)`
reads the directory; `unreadCount(peerHex)` and
`getConversationSummary(peerHex)` produce summary tuples for the
UI. A "seen message IDs" deduplication set with hard cap;
`storeRevision()` exposes its version for cache invalidation in the
UI.

**Delivery model.** Strictly **direct delivery** — no propagation-
node client behavior. Two strategies:

- **Opportunistic** for messages that fit in a single LoRa frame
  (≤254 bytes once framed). One LXMF packet, no link.
- **On-demand link** for larger messages. `LXMFManager` requests a
  path with `RNS::Transport::request_path`, retries 6 times at 10 s
  intervals (~60 s budget) before marking the message FAILED with
  `LXMF_DISCOVERY_MAX_ATTEMPTS`, then opens a link, transfers via
  Resource, awaits the proof, and tears down.

Comment from `LXMFManager.cpp` about why they don't speculatively
pre-establish links:

> "Speculative background links cause collisions on LoRa when both
> devices try to establish simultaneously. Link establishment is now
> only triggered on-demand when a message is too large for
> opportunistic delivery."

A real LoRa-specific gotcha. On a half-duplex airwave shared between
two endpoints, both running the same link-eager logic causes
deterministic collision storms.

**Contact management.** Two-stage. The `AnnounceManager` (subclass
of `RNS::AnnounceHandler` with the `lxmf/delivery` aspect filter)
catches every announce, extracts the display name from the announce
app data, and either upserts it into the `_nodes` vector (Peers
tab — fluid list of every node ever seen) or, if you've explicitly
added the contact, updates the contact record. Contact files in
`/contacts/<hash>.json` map dest-hash to nickname + metadata. Name
cache (`_nameCache`) is an `unordered_map` for O(1) UI lookups.
`lookupName(hash)` does cache → contact-store → fallback-to-hex.

**LXMF wire format.** Constructed in
`ratspeak/microReticulum`'s `LXMFMessage` class (not in ratdeck).
Ratdeck calls:

```cpp
LXMFMessage msg;
msg.sourceHash = _rns->destination().hash();
msg.destHash   = destHash;
msg.timestamp  = (double)now;
msg.content    = content;
msg.title      = title;
std::vector<uint8_t> payload = msg.packFull(_rns->identity());
```

The opportunistic on-wire form is `[src:16][sig:64][msgpack-
payload]`, signed with the source identity's Ed25519 key. For a
clean-room reimplementation refer to the upstream Python LXMF spec
(markqvist/LXMF) — that's the authoritative wire format and is
independent of ratspeak.

### 2.8 Noteworthy / unusual

- **No transport mode.** Ratdeck is configured strictly as an
  endpoint. Issue #48 ("Feature request: transport mode") is open.
  RNS tables sized for endpoint scale (256 destinations, 64
  announces); a transport rebuild would need different config.

- **No propagation-node client.** Decision, not omission. Direct
  delivery only. If we ever want offline delivery (sender online,
  recipient offline), we'd need to add propagation-client behavior
  ratdeck doesn't have.

- **Single-task cooperative everything.** Discussed in §2.4. The
  9 s → 100 ms loop optimization (v1.7.3) is the canonical
  cautionary tale.

- **Shared SPI bus discipline.** Display + SX1262 + SD all on FSPI.
  Display flushes intentionally synchronous. SX1262 ISR refuses to
  do SPI. SD writes happen inside `loop()` between RNS pumps.
  Fragile and they admit it in comments. With an extra SPI bus
  available (the T-Deck Plus has limited choice), wiring the radio
  to a dedicated SPI host eliminates an entire class of problems.

- **PSRAM pool allocator for RNS.**
  `-DRNS_DEFAULT_ALLOCATOR=RNS_PSRAM_ALLOCATOR` and a 2 MB pool.
  microReticulum-specific config that prevents heap fragmentation
  from RNS's many small allocations during packet processing. If we
  build against mR we want the same; ESP-IDF's
  `heap_caps_malloc(MALLOC_CAP_SPIRAM)` gives us the primitive.

- **LittleFS partition labeled `spiffs`.** Required by
  Arduino-ESP32's LittleFS driver. On ESP-IDF native this issue
  vanishes — declare a `littlefs` subtype and use `esp_littlefs`.

- **JSON for messages, atomic-rename for everything.** Pragmatic.
  Costly on flash but bulletproof against power loss. NVS is used
  only for tiny housekeeping values.

- **Hand-rolled NMEA parser, autobaud.** Pragmatic. Avoids any GPS
  library dependency.

- **Boot-loop detection via NVS counter.** If repeated partial
  boots are detected, persisted RNS state is reset. Useful guardrail
  for a device that may end up with corrupt path tables.

- **Audio is synthesized, not stored.** Saves a few hundred KB and
  simplifies the build. Sine + 2nd + 3rd harmonic with linear
  envelopes via I2S DMA at 16 kHz.

- **No PMIC driver.** They drive `PWR_EN` high and read battery via
  ADC + voltage divider, ignoring any potential PMIC entirely.
  Battery percentage is a piecewise approximation. For a security-
  context device with critical battery accounting we'd want the
  real PMIC driver.

- **BLE Sideband uses KISS framing**, BLE generic interface uses
  HDLC framing. Sideband is the protocol Mark's official iOS/Android
  Sideband app speaks to a companion radio over BLE. Ratdeck
  implements it so Sideband on a phone can use a ratdeck as the
  radio backend.

- **AutoInterface wrapper has deferred startup** waiting for a
  stable IPv6 link-local address and re-binding when SLAAC rotates.
  The kind of detail that breaks RNS-over-WiFi on networks with
  churn — they got bit by it.

- **Things they got wrong and revisited** (from release notes /
  commits):
  - v1.7.3: 9 s loop iteration → 100 ms (cooperative scheduling
    overload).
  - v1.8.3: LXMF interop fixes against vanilla Sideband / Reticulum.
  - v1.8.4-beta: contact persistence and peer management bugs.
  - v1.9.0: full LVGL UI rebuild (the original UI was different,
    presumably more limited).
  - "InputManager: suppress wake-click as enter event" — phantom
    enters when waking from sleep.
  - "FlashStore: fall back to 'spiffs' partition label" — partition
    naming gotcha.
  - Issue #27 (closed without published resolution): "radio detected
    interference" — ratdeck transmissions visible on SDR but not
    decoded by RNodes. Lack of documented fix is a useful signal
    that LoRa interop with the wider RNode ecosystem can be fragile
    and worth verifying ourselves with both T-Beam SX1262 hardware
    and an RNode reference.

### 2.9 Adjacent ratspeak projects

- **[ratspeak/microReticulum](https://github.com/ratspeak/microReticulum)**
  — fork of attermann/microReticulum (Apache-2.0). Adds LXMF wire-
  format support (`LXMFMessage` class) and ESP32-specific allocator
  / persistence hooks. Ratdeck pins this fork at commit
  `858010cee0…`. Should probably upstream the LXMF additions but as
  of now is a hard fork. Actively developed alongside ratdeck.
  Cloned locally at [research/microReticulum/](../research/microReticulum/).

- **[ratspeak/ratcom](https://github.com/ratspeak/ratcom)** —
  Ratdeck's sibling for the **M5Stack Cardputer Adv**. Same
  architecture: PlatformIO + Arduino, custom HAL, microReticulum,
  LXMF, WiFi/TCP/LoRa interfaces. **GPL-3.0** (note: not AGPL like
  ratdeck — they may be tightening over time). v1.7.9 as of April
  2026, 24 releases. Worth comparing to ratdeck to see what's
  portable vs board-specific.

- **[ratspeak/Ratspeak](https://github.com/ratspeak/Ratspeak)** —
  desktop/mobile native client. Rust + Tauri. macOS / Windows /
  Linux / Android / iOS (iOS unsigned). AGPL-3.0. Alpha. Built on
  rsReticulum + rsLXMF.

- **[ratspeak/rsReticulum](https://github.com/ratspeak/rsReticulum)**
  — Rust reimplementation of the Reticulum stack (AGPL-3.0).
  Explicitly experimental / alpha; PRs closed; targets Reticulum
  1.2.4 compatibility. Useful as a second reference implementation
  for cross-validation.

- **[ratspeak/rsLXMF](https://github.com/ratspeak/rsLXMF)** — Rust
  LXMF (AGPL-3.0). Useful as a second reference implementation to
  read alongside markqvist's Python LXMF when you want clarity on a
  wire-format edge case.

- **[ratspeak/LXMFace](https://github.com/ratspeak/LXMFace)** —
  deterministic visual avatars from LXMF/Reticulum address hashes.
  Rust + JS. **MIT.** Stable. Ratdeck embeds the rendering logic in
  `src/ui/LxmFaceAvatar.{h,cpp}`. Trivially reusable for any
  Reticulum UI.

- **[ratspeak/ratkey](https://github.com/ratspeak/ratkey)** — moves
  Reticulum identity keys from disk to a YubiKey 5 (Ed25519 sign +
  X25519 ECDH on hardware). Python. **MIT.** Beta. Two modes:
  hardware-only (no backup possible, key never extractable) or
  BIP-39 24-word seed (recoverable). Most interesting adjacent
  project for security-camera context — argues for hardware-secure-
  element identity. ESP32-S3 has its own HMAC + digital-signature
  peripheral and flash encryption; consider whether the camera's
  identity should sit behind those rather than on plain LittleFS.

- **[ratspeak/rathole](https://github.com/ratspeak/rathole)** —
  security suite + transport node, intended to make running a
  public Reticulum gateway from home easy. Python. **GPL-3.0.**
  "Vibed together — built quickly, tested lightly." Auto-enables
  transport mode, integrates I2P for IP privacy, ships 12 packet
  filters. Not for embedded use, but useful operational reference.

- **[ratspeak/lrgp-py](https://github.com/ratspeak/lrgp-py)** +
  **[ratspeak/lrgp-rs](https://github.com/ratspeak/lrgp-rs)** —
  Lightweight Reticulum Gaming Protocol (MIT). Compact session-
  based protocol for multi-player games over LXMF. Single-packet
  encrypted move envelopes, no link setup. Reference games:
  tic-tac-toe, chess. Nice example of a higher-level protocol
  layered on LXMF.

- **[ratspeak/C6-Reticulum-ASM](https://github.com/ratspeak/C6-Reticulum-ASM)**
  — ESP32-C6 assembly experimentation. Python wrapper. MIT. Not
  relevant for a normal port.

- **[ratspeak/ratspeak-website](https://github.com/ratspeak/ratspeak-website)**
  — the website (https://ratspeak.org). HTML. AGPL-3.0.

### 2.10 Quick clean-room reimplementation cheat sheet

Things to take from ratdeck as architectural lessons:

- Pin RNS to endpoint-scale tables (256/64/etc) and back it with a
  PSRAM pool allocator.
- Atomic-rename writes for everything persistent; have a `.bak`
  recovery path.
- LXMF: opportunistic for ≤254 B, on-demand link otherwise, retry
  path discovery 6× / 10 s. Don't pre-establish links.
- Aspect-filtered `RNS::AnnounceHandler` for peer discovery, with
  rate limiting and deferred persistence.
- LoRa: split frames at 254 B with a sequence nibble; defer TX
  during split RX; ISR sets a flag, never touches SPI.
- BLE Sideband if you want phone connectivity; KISS framing over
  Nordic UART Service.

Things to *not* take:

- Single-task cooperative everything. Use proper FreeRTOS tasks
  like the rest of spangap/seccam does.
- Synchronous LVGL flush blocking the radio. Use a separate display
  task and a frame-buffer queue.
- JSON-per-message storage (use msgpack / CBOR with a per-
  conversation index for thousands of messages).
- Skipping a real PMIC driver wherever the board has one.
- Arduino-on-IDF entirely — our codebase is ESP-IDF native; stay
  there.

Things ratspeak hasn't done that we'll have to figure out ourselves:

- Propagation-node client behavior (offline message delivery).
- Multi-link concurrency on a single radio.
- Hardware-secure-element identity (ratkey shows it for YubiKey,
  not for ESP32-S3 native security).
- Rigorous airtime / duty-cycle accounting beyond the per-window
  heuristic.
