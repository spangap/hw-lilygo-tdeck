# hw-lilygo-tdeck — internals

Maintainer reference for the LilyGo T-Deck Plus board HAL. The
[README](README.md) is the operator guide and pin map; this document is for
changing the board code without breaking the bring-up. It is self-authoritative.

## 1. What this straddle adds

A non-buildable spangap component (`idf_component_register`, no `app_main`). It
exports a handful of hook symbols the buildable's generated dispatcher calls, and
publishes the board's hardware description as `kconfig:` values and storage keys.
Source layout:

```
esp-idf/
├── CMakeLists.txt           component registration (+ SPANGAP_CONDITIONAL_SRCS glob)
├── idf_component.yml         deps: idf >=5.5, esp_lcd_touch_gt911
├── include/{tdeck,gps,rtc}.h public board API + BOARD_* pin macros
├── src/
│   ├── tdeck.cpp             power rail + CS park, shared I2C0, battery monitor
│   ├── gps.cpp               GNSS receiver task + clock discipline
│   └── rtc.cpp               PCF8563 RTC driver (pure HW shim)
└── conditional/
    ├── spangap-lcd/src/tdeck_lcd.cpp   input HAL: touch, trackball, button, keyboard
    └── audio/src/tdeck_audio.cpp       ES7210 mic codec register shim
```

The `conditional/<straddle>/` directories are compiled **only** when that
straddle is staged (the build globs them into `SPANGAP_CONDITIONAL_SRCS`), so
there is no `#if` gating in the board sources at all — `tdeck_lcd.cpp` exists
only on an LCD build, `tdeck_audio.cpp` only on an audio build.

Everything here is new (a board contributes hardware, not protocol). The subsystems:

- **Peripheral power rail + shared-SPI CS park** (`tdeckStart`/`tdeckPowerInit`).
- **Shared I2C0 master bus** (`tdeckI2cBus`) — keyboard, touch, RTC, audio codec.
- **Battery monitor** (`tdeckBatteryInit`) — ADC + curve + 1/min timer.
- **GNSS receiver task** (`gpsInit`) — autobaud, NMEA parse, clock discipline.
- **PCF8563 RTC driver** (`rtcRead`/`rtcWrite`/`rtcProbe`) — optional clock keeper.
- **On-device input HAL** (`tdeckLcdStart`/`tdeckLcdInit`) — touch, trackball
  pointer, centre button, QWERTY keyboard.
- **ES7210 mic codec shim** (`tdeckAudioInit`) — I2C register programming for the
  audio engine.

## 2. Bring-up ordering

The hooks run in two bands (declared in `straddle.yaml`):

```
start:  tdeckStart            (always)
        tdeckLcdStart         (when spangap-lcd)
init:   tdeckLcdInit          (when spangap-lcd)
        tdeckAudioInit        (when spangap/audio)
        gpsInit               (always)
        tdeckBatteryInit      (always)
```

**`tdeckStart` is the only `start:`-band board hook that must run before
`spangapInit()`.** The first shared-SPI-bus transaction is `fs_mount_sd()`
*inside* `spangapInit()`, and it fails (`ESP_ERR_TIMEOUT`) unless two things are
already true:

1. **Peripheral power rail HIGH.** The T-Deck gates +3.3 V to the SD card (and
   display/GPS/LoRa) behind `BOARD_POWER_EN_PIN` (GPIO 10). Until it is driven
   HIGH the SD card is unpowered. `tdeckPowerInit` drives it and waits ~100 ms
   for the rail to settle. (`loraInit()` re-asserts the pin later — a harmless
   idempotent no-op, far too late for the SD probe.)
2. **Idle CS lines parked HIGH.** The LCD (`CONFIG_LCD_CS_PIN`) and every
   configured LoRa radio (`CONFIG_LORA*_CS_PIN`) share the bus but no driver owns
   their CS yet — park them HIGH so none drives MISO during the SD/LoRa probe.
   The SX1262 CS especially: the power rail is now up so the chip is live, but
   `loraInit()` (which would own its CS) runs long after the SD probe.

`tdeckStart` also creates the shared **I2C0** bus eagerly, while still
single-threaded, so the touch (LCD task), keyboard (poll task) and RTC (GNSS
task) can't race `i2c_new_master_bus()` on the same port. `tdeckI2cBus()` is
otherwise lazy / first-caller-wins and is always compiled (the RTC needs it even
on a headless build).

`tdeckLcdStart` registers the input HAL with the LCD component **before**
`lcdInit()` runs inside `spangapInit()`, so the component can wire touch /
trackball / button when it brings the panel up. `tdeckLcdInit` brings up the
keyboard **after** `spangapInit()`, because the keyboard needs the LCD task to
exist (it creates and drives an LVGL indev via `lcdRun()`).

## 3. Battery monitor (`tdeck.cpp`)

`BOARD_BAT_ADC` (GPIO 4, ADC1 channel 3) reads VBAT through a 2:1 resistor
divider (100k/100k) that sits **upstream** of the power-enable gate — it has no
enable, conducts continuously (~21 µA @ 4.2 V), and so needs no power-up step.

`tdeckBatteryInit` (init band, needs storage up) configures the ADC with curve-
fitting calibration (falling back to nominal 12-bit scaling if calibration is
unavailable), publishes an initial reading, and arms a `esp_timer` that re-reads
once a minute on the timer task — **no dedicated task**. Each read averages 16
samples, multiplies the calibrated pin voltage by the divider ratio (true 2.0,
*not* Meshtastic's 2.11, which compensates for an uncalibrated ADC), and applies
a light EMA (`mv*3+new)/4`, ~3–4 min) so the icon doesn't wobble. It writes
`battery.millivolt` and `battery.percent` in one `storageBegin/End` so
subscribers see both atomically.

Percent comes from a 100-entry measured discharge curve (`s_scaledVoltage`,
voltage scaled 0–255 across `[BAT_MIN_MV=3040, BAT_MAX_MV=4260]`), monotonic
non-increasing — the first entry ≤ the scaled reading gives the percent.
Re-measure the curve, or trim `BAT_DIV_NUM/DEN`, if a multimeter disagrees.

## 4. GNSS receiver (`gps.cpp`)

One FreeRTOS task, **core 0, prio 1, 6 KB PSRAM stack**, `itsPoll` as the single
wait point. No PPS is routed on the Plus, so there is no interrupt line: the task
drains the UART on a ≤1 s cadence (the receiver's natural 1 Hz output) and
publishes a full `gps.*` snapshot every publish period.

**Two receivers, batch-dependent**, nothing host-visible to tell them apart:
Quectel **L76K** (default 9600) or u-blox **MIA-M10Q** (default 38400), both
NMEA 8N1 on UART1. On enable the task **autobauds** — tries 38400 then 9600,
locks on the first checksum-valid recognized NMEA sentence, and infers the model
from the baud that worked. This is the LilyGo batch distinguisher, not a true
probe; a receiver reconfigured off its default baud would be mis-identified
(out of scope). Each candidate sends a `0xFF` wake edge first, so a u-blox in
software backup is revived within the 1.5 s listen window.

**Parse.** `nmeaApply` folds RMC / GGA / GSA / GSV / VTG into a working `GpsFix`,
matching the last 3 chars of the talker token so any GP/GN/GL/GA/GB prefix works.
`gps.sats_view` is summed across constellations — each talker's in-view count
counted once (on its first GSV) and zeroed per epoch in `drainUart`; `gps.snr` is
the best C/N0 this epoch, not a running max.

**Fix cadence / power.** On the u-blox M10, `s.gps.interval` drives the receiver
itself via one `UBX-CFG-VALSET`: `CFG-RATE-MEAS` to the period and
`CFG-PM-OPERATEMODE` to `FULL` (interval 0) or `PSMCT` (1–10) — the chip
low-power-tracks between fixes. `OPERATEMODE` goes last in the VALSET because
u-blox requires it set after its dependent keys. The L76K speaks no UBX PSM, so
for it the interval is only a publish throttle. Settings are written to the RAM
layer and re-sent on every (re-)enable and on a live interval change.

**Standby (on disable).** The shared rail can't be cut, so the task commands the
chip into its deepest UART-reachable state, then drops the UART:

- **u-blox M10** — `UBX-RXM-PMREQ` software backup (`backup|force`, wakeup
  `uartrx`). Real low power, wakes on a UART RX edge → re-enable revives it
  automatically.
- **L76K** — `$PMTK225,4` deep backup. The `$PCAS` set has no UART-wakeable
  standby (its low-power is a FORCE pin not routed on the Plus), so this is a
  one-way trip: the task sets `s_needsPowerCycle`, and on re-enable publishes
  `gps.state = power-cycle to wake` instead of fumbling the serial. A reboot
  clears the flag (fresh power = fresh chip).

### 4.1 Clock discipline

Unless `s.gps.ignore_clock` is set, GPS is a clock authority. The model:

- While the system clock is **invalid** (< `kValidEpoch` = 2025-01-01) the task
  accepts *any* valid GPS date+time — no satellites/position required, since the
  receiver streams time before it locks. Once valid it re-disciplines only from a
  real positioned fix.
- True UTC now = fix epoch + the NMEA sub-second field + a static per-model
  **pipeline lag** (70 ms u-blox @ 38400, 260 ms L76K @ 9600 — no PPS to measure
  it) + the time elapsed since the sentence was parsed. `settimeofday` steps only
  when the residual is ≥ `kStepThreshUs` (250 ms — below that is serial jitter),
  always stepping a still-invalid clock. `newlib` ships no `timegm`, so
  `utcToEpoch` converts directly (Howard Hinnant days-from-civil).
- Ownership is published as `sys.time.ext` (1 = a local authority owns the
  clock); NTP subscribes and parks SNTP while set. Going through storage keeps
  GPS free of any compile-time dependency on net — with no net staged there is
  simply no subscriber. GPS holds ownership while the clock is valid *and* GPS
  time was seen within the staleness window, then hands it back: **3 days** with
  an RTC to hold time, but only **1 hour** without one (the RC-oscillator clock
  drifts hard). `clockOwnershipReconcile` mirrors the last decision to write only
  on change.
- `logTimeUpdate` scales the log level to the correction magnitude
  (< 100 ms verbose, < 5 s debug, < 60 s info, else warn).

### 4.2 RTC keeper

`rtcBootSync` (first thing on the task) probes the PCF8563 once and remembers
absence — the **stock T-Deck has no RTC** (I2C0 carries only keyboard 0x55 and
touch 0x5D), so absence is the normal case: it is noted once at info and I2C is
never touched for the RTC again (no per-minute probes, no per-fix writes). The
driver stays wired so an external PCF8563 on the Grove I2C — or a board rev that
adds one — is picked up automatically. If present and trustworthy and the system
clock is unset, its time is adopted at boot; every GPS step is mirrored to it;
and once a minute with no recent GPS coverage `gpsHeartbeat` re-syncs the system
clock from it (whole-second; steps only for ≥1 s drift).

### 4.3 PCF8563 driver (`rtc.cpp`)

A thin BCD↔`struct tm` shim over the on-board PCF8563 (I2C0 @ 0x51, 100 kHz,
time registers auto-increment from 0x02), sharing the bus via `tdeckI2cBus()`.
Time is always UTC and always century 2000–2099 — the century bit is written 0
and masked on read, sidestepping the PCF8563 century-bit polarity confusion (it
only matters past 2099). The chip's **VL** (voltage-low) flag latches whenever
the oscillator may have stopped; `rtcRead` surfaces it as `clockValid == false`
so callers never trust a clock that lost time, and `rtcWrite` clears it.
`rtcWrite` refuses years outside 2000–2099 rather than write a wrapped year.
`gps.cpp` owns all policy; this module is pure hardware.

## 5. On-device input HAL (`conditional/spangap-lcd/tdeck_lcd.cpp`)

This file exists only on an LCD build. It registers the board's `lcd_input_t`
ops (`init`, `touch_read`, `pointer_read`, `click_read`) with the LCD component
via `lcdSetInput()` in `tdeckLcdStart`; the component owns the panel, the cursor,
the focus group and the shell — see [spangap-lcd](../spangap-lcd) for the
`LcdApp`/launcher/statusbar model. The board supplies only input hardware.

**The LCD-owned indevs are interrupt-driven.** Touch, trackball and button are
`LV_INDEV_MODE_EVENT`, so LVGL runs no read timer. The board attaches the
component's exported `lcdInputISR` to each INT line; the ISR only flags +
`vTaskNotifyGiveFromISR`s the LCD task, which reads the indev once. Idle LCD CPU
is ~0 %. The shared GPIO ISR service is installed `ESP_INTR_FLAG_IRAM` (the same
flag LoRa's DIO1 path uses) so the IRAM-safe ISR survives cache-disabled windows.

### 5.1 GT911 touch

`tdeckTouchInit` builds the `esp_lcd_touch` handle by hand (the GT911 CONFIG
macro uses out-of-order designated initializers — a hard error in C++). The
controller **latches its I2C address from the INT level at power-on** (low →
0x5D, high → 0x14); the T-Deck has no touch reset, so the address is
sub-revision dependent — the code probes both. Probing the wrong address makes
`esp_lcd_touch_gt911` and the I2C IO log a failed read at ERROR; those two tags
are muted across the probe and restored after. The result string is published to
`tdeck.touch`. Touch is left at IDENTITY (native coords) — the LCD component
applies the same `CONFIG_LCD_ROTATION` to the points as to the pixels — so
`tdeckTouchRead` returns raw native points and the maxes are
`CONFIG_LCD_NATIVE_WIDTH/HEIGHT`. The INT is taken `ANYEDGE` (polarity is
sub-rev dependent; a redundant edge costs one empty read). A consumer can set the
runtime `tdeck.multi_touch` key to flip the GT911 (a 5-point controller) into
multipoint mode via `lcdTouchSetMultipoint`.

### 5.2 Trackball → pointer

Four direction lines pulse active-low; four `NEGEDGE` ISRs just count under a
spinlock (`s_tbMux`) shared with the reader. `tdeckPointerRead` integrates the
counts into an absolute screen position, clamped to the post-rotation panel.

The per-pulse **step is velocity-dependent**: it ramps linearly from 1 px when
the smoothed pulse rate is at/below `s.tdeck.trackball_accel_min` (a slow,
deliberate roll → pixel-exact for clicks) up to `s.tdeck.trackball_speed` px once
the rate reaches `s.tdeck.trackball_accel_max` (a fast flick). The rate is a
**time-decayed EMA** with `s.tdeck.trackball_smooth_ms` as the time constant —
`decay = tau/(tau+dt)` — so a short gap during a roll barely moves it (steady
feel) while a long gap decays it toward zero (the first nudge after a pause is
precise, no leftover-velocity jump). A flat step can't win: fast enough to cross
the screen means jumps too coarse to click. All four knobs are live `s.tdeck.*`
config keys (no sliders); a dbg-gated, rate-limited line logs the live pulse rate
for re-dialling `accel_min`/`max`.

Two extra behaviours:

- **Arrow mode.** Entered when a program claimed the wheel
  (`lcdScrollwheelArrowsActive`, e.g. the on-device terminal) **or** a text caret
  is live (`lcdCaretActive` — editing a box). The raw per-read pulse delta is sent
  as `LV_KEY_UP/DOWN/LEFT/RIGHT` to the focus group (capped at 4/flick) and the
  pointer stays put — so it never sticks at a clamped screen edge. No timeout: the
  caret blink is the state. **Walk-out** — 3 quick UPs against the top line while a
  caret holds — calls `lcdCaretRelease()` and warps the pointer accumulator to the
  caret, so the ball reappears parked on the text cursor. The ball cursor defaults
  to always-visible (`s.tdeck.pointer_visible_time` = -1) so it marks where a click
  lands; arrow mode hides it explicitly.
- **Edge-pan.** When the cursor is pinned against a screen edge and the ball
  keeps pushing that way, the motion the clamp would swallow becomes a
  `lcdScroll` distance instead — so a touchless deck reaches offscreen content
  and pages the launcher.

reticulous owns the whole pointing device; spangap-lcd stays generic (it draws
the cursor and knows the `pointer_read` hook, owns no pointer config). Cursor
dwell is pushed in via `lcdPointerSetVisibleMs` from `s.tdeck.pointer_visible_time`.

### 5.3 Centre / Home button & standby

GPIO 0 (BOOT-strap, also the trackball centre-press, also the unused mic) is read
pulled-up active-low. The board owns the timing and the four meanings of a press,
driven by two one-shot `lv_timer`s on the LCD task:

- tap (< `launcher_hold`) → a pointer click (asserted for exactly one poll);
- hold `s.tdeck.launcher_hold_ms` → `lcdGoHome`;
- hold `launcher_hold + s.tdeck.standby_hold_ms` → set `sys.standby = 1`;
- any press while in standby → clear `sys.standby` (wake), absorbed.

The two holds stack (Home first, then standby). When the press starts already at
the launcher (`lcdAtLauncher`), the Home tier is a no-op so standby comes at the
shorter `launcher_hold` instead. After a transition the rest of the press is
swallowed (`s_wakeAbsorb`) until the finger lifts, so a held finger can't wake
the device it just slept.

Standby is **not** done by the button — the button (and the LCD inactivity
timeout) only flip the ephemeral `sys.standby` key; `tdeckStandby` (subscribed on
the LCD task) is what actually sleeps/wakes: GT911 reads gated off
(`s_touchAsleep`), display off via `lcdScreenSleep/Wake`, and the keyboard poll
task parked. Only the centre button stays live to wake.

### 5.4 QWERTY keyboard

The keyboard is an ESP32-C3 MCU on I2C0 @ 0x55. It is a poor fit for the generic
indev model, so it lives in the consumer, not spangap-core:

- **GPIO 46 ("key buffered" INT) is dead** — the stock/rgrizzell C3 firmware
  never drives it (verified on hardware and in the C3 source).
- The 1-byte read is **destructive** (pops the key, returns 0 when empty, no
  peek/count), so you can't tell a key is pending without consuming it.

So a dedicated low-prio `kbpoll` task (**prio 3, core 1**) polls the I2C off the
LCD task at a fixed `POLL_PERIOD` (30 ms). The C3 holds only the last unread
key (no buffer), so a lazy/adaptive backoff drops keystrokes under fast typing;
standby parks the task, so the always-on scan costs nothing while asleep.
It buffers bytes into a queue and bumps the LCD task via `lcdRun(kbDrain)` to
drain them through an LVGL keypad indev joined to `lcdInputGroup()`. Prio 3 is
one notch above the LCD task so a long synchronous redraw can't starve the poll
(the C3 holds only the last unread key); not higher, because the read takes the
shared I2C0 bus (touch, codec). `lcdSetHasKeyboard(true)` tells the component to
suppress the on-screen keyboard.

GPIO 46 is still wired `ANYEDGE`: an edge wakes the poll early and is counted
for the `kbint` diagnostic (useful if a future C3 firmware ever drives it).
The indev is created lazily
in `kbDrain` (not in `tdeckLcdInit`): `tdeckLcdInit` fires `lcdRun(kbCreateIndev)`
right after `spangapInit()`, which can land before the LCD task has registered
its `LCD_RUN_PORT` aux handler, so the create can silently fail; `kbDrain` runs
only once the LCD task is fully up, making it the race-proof place to build it.
The `readCb` synthesizes press+release over two reads and decodes a `0x0C` prefix
(Alt-C) as a one-shot "next lowercase is `LCD_KEY_CTRL | letter`" lead-in (1 s
window) for the terminal. A keystroke that woke the screen is swallowed (it only
served to wake).

## 6. ES7210 mic codec shim (`conditional/audio/tdeck_audio.cpp`)

Compiled only on an audio build. The [spangap/audio](../audio) engine owns the
I2S read/write engine; a board contributes Kconfig pins plus, for an
I2C-controlled input codec, this slice. The ES7210 is a quad-mic ADC on I2C0
@ 0x40 running as an **I2S slave** — the S3 (I2S0 master) supplies MCLK/BCLK/WS,
the ES7210 just clocks ADC samples onto DIN once its registers are programmed. So
`es7210InInit` is pure I2C register programming, no I2S; slave mode needs no
sample-rate coefficient table (the chip derives serial clocks from the supplied
BCLK/WS), so the init sequence is rate-independent. `tdeckAudioInit` (init band)
only stows the `audio_codec_ops_t` with the engine via `audioRegisterCodec`; the
audio task applies them lazily on first capture. The register table configures
16-bit I2S-slave, all four mics at a fixed gain (regs 0x43–0x46), HPF on. Of the
four hardware mics only one is populated on the board (the rest are unconnected).

## 7. Pitfalls

- **`tdeckStart` before `spangapInit()`.** Power rail + CS park must precede the
  SD mount; this is the whole reason for the `start:` band. Don't reorder it into
  `init:`.
- **Keep FreeRTOS sync objects out of PSRAM.** Internal DRAM/DMA is scarce on the
  T-Deck; queues/stream-buffers/mutexes in PSRAM trip the `S32C1I` spinlock
  assert. Task stacks and large buffers go in PSRAM (`STACK_PSRAM`); sync objects
  stay internal.
- **SD writes failing with `not enough mem, err=0x101` are DMA-pool exhaustion,
  not a bad card.** `E [fs_strm] sdmmc_cmd: sdmmc_write_sectors: not enough mem,
  err=0x101` (plus a `diskio_sdmmc` follow-on) means the scarce internal
  DMA-capable RAM — already mostly claimed by LCD/LVGL and WiFi — has no 512-byte
  block left. The T-Deck runs its SD on the shared FSPI bus
  (`CONFIG_SPANGAP_SDCARD_BUS_SPI`), where stock `sdmmc_cmd` bounces every
  PSRAM-sourced or unaligned sector through a per-write
  `heap_caps_malloc(512, MALLOC_CAP_DMA)` that then fails. spangap-core fixes this
  at the source and the board just inherits it: its `CMakeLists.txt` defines
  `SOC_SDMMC_PSRAM_DMA_CAPABLE=1` for the `sdmmc` component on SD-on-SPI builds
  (so SDSPI's own once-allocated block buffer absorbs PSRAM traffic and the
  per-write bounce is skipped), and the fs worker gives SD-backed files a
  one-sector `setvbuf` plus sub-sector chunked writes (`fsSdFwrite`) so FatFs
  never hands `disk_write` an unaligned multi-sector run — the mid-sector-append
  case the macro alone can't cover. Don't chase 0x101 by freeing internal RAM;
  fix SD writes at the source. See
  [spangap-core fs internals](../spangap-core/docs/fs-internals.md) and
  [idf-tweaks](../spangap-core/docs/idf-tweaks.md).
- **Never enable `CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP` on the T-Deck.** It looks
  like an easy way to reclaim internal RAM (e.g. to relieve the 0x101 above), but
  this is a display board: LCD/LVGL already hold the internal DMA pool, and
  nudging WiFi/lwIP toward PSRAM tips WiFi's 16 internal-DMA-only static RX
  buffers over the edge — `malloc buffer fail` / `Expected to init 16 rx buffer,
  actual is 12`, and WiFi never inits. It is only safe per-board on the
  display-less seeed, never blanket in `sdkconfig.defaults.spangap`. See
  [spangap-core memory internals](../spangap-core/docs/memory-internals.md).
- **GT911 address is INT-level-latched, with no reset to force it.** Always probe
  both 0x5D and 0x14; never hardcode. The wrong-address ERROR logs during the
  probe are expected, not a fault.
- **Trackball direction→pin and ball orientation are sub-revision dependent** (a
  sample had DOWN/RIGHT swapped). Flip `BOARD_TBOX_*` or the `dx/dy` signs if
  motion feels wrong — it is not a code bug.
- **The keyboard INT is dead on current firmware** — don't build an INT-only read
  path. The poll path is load-bearing; the self-healing edge detection is the
  only thing that would ever switch it off.
- **No RTC on a stock T-Deck.** Don't add per-minute RTC probes on the assumption
  one exists — `rtcBootSync` deliberately probes once and goes quiet. The driver
  is for an *external* PCF8563 (Grove I2C) or a board rev that adds one.
- **Battery divider is 2.0, not 2.11.** The ADC is curve-fit calibrated, so the
  true divider ratio applies; 2.11 is Meshtastic's compensation for an
  *uncalibrated* ADC and would over-read here.
- **GPS clock ownership is published, not called.** GPS never links net; it
  claims/releases the clock through `sys.time.ext` on the storage bus. If you add
  a clock consumer, subscribe to that key — don't add a direct dependency.
