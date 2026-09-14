# hw-lilygo-tdeck — internals

Maintainer reference for the LilyGo T-Deck Plus board HAL. The
[README](README.md) is the operator guide and pin map; this document is for
changing the board code without breaking the bring-up. It is self-authoritative.

## 1. What this straddle adds

A non-buildable spangap component (`idf_component_register`, no `app_main`). It
exports a handful of `Service` subclasses the buildable's generated `app_main`
constructs and walks, and publishes the board's hardware description as
`kconfig:` values and storage keys.
Source layout:

```
esp-idf/
├── CMakeLists.txt           component registration (+ SPANGAP_CONDITIONAL_SRCS glob)
├── idf_component.yml         deps: idf >=5.5, esp_lcd_touch_gt911
├── include/tdeck.h           public board API + BOARD_* pin macros
├── src/
│   ├── tdeck.cpp             power rail + chip-select park, shared I2C0, battery monitor
│   └── detect.cpp            board self-assertion (detect_hw)
└── conditional/
    ├── spangap-lcd/src/tdeck_lcd.cpp   input HAL: touch, trackball, button, keyboard
    └── audio/src/tdeck_audio.cpp       ES7210 mic codec register shim
```

The GNSS receiver is the generic [gps](../gps) straddle (staged
from `additional_installs:`, pins supplied as `CONFIG_GPS_*` in `kconfig:`);
the PCF8563 RTC is [spangap-rtc](../spangap-rtc), which no board stages today.

The `conditional/<straddle>/` directories are compiled **only** when that
straddle is staged (the build globs them into `SPANGAP_CONDITIONAL_SRCS`), so
there is no `#if` gating in the board sources at all — `tdeck_lcd.cpp` exists
only on an LCD build, `tdeck_audio.cpp` only on an audio build.

Everything here is new (a board contributes hardware, not protocol). The subsystems:

- **Peripheral power rail + shared-SPI chip-select park** (`TdeckBoard::onStart`
  → `tdeckPowerInit`).
- **Shared I2C0 master bus** (`tdeckI2cBus`) — keyboard, touch, audio codec.
- **Battery monitor** (`TdeckBattery`) — ADC + curve + 1/min timer.
- **On-device input HAL** (`TdeckLcdInput`) — touch, trackball pointer, centre
  button, QWERTY keyboard and its lamp.
- **ES7210 mic codec shim** (`TdeckAudio`) — I2C register programming for the
  audio engine.

## 2. Bring-up ordering

The services are listed in `straddle.yaml` and walked in two bands:

```
start:  TdeckBoard::onStart       (always)
        TdeckLcdInput::onStart    (when spangap-lcd)
init:   TdeckBoard::onInit        (always)
        TdeckLcdInput::onInit     (when spangap-lcd)
        TdeckAudio::onInit        (when spangap/audio)
        TdeckBattery::onInit      (always)
```

(The GNSS task is gps's own `GpsService`, registered at that straddle's
init_order position.)

**`TdeckBoard::onStart` is the only board work that must run before
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

`TdeckBoard::onStart` also creates the shared **I2C0** bus eagerly, while still
single-threaded, so the touch (LCD task), keyboard (poll task) and codec (audio
task) can't race `i2c_new_master_bus()` on the same port. `tdeckI2cBus()` is
otherwise lazy / first-caller-wins.

`TdeckLcdInput::onStart` registers the input HAL with the LCD component
**before** `lcdInit()` runs inside `spangapInit()`, so the component can wire
touch / trackball / button when it brings the panel up. Its `onInit` brings up
the keyboard **after** `spangapInit()`, because the keyboard needs the LCD task to
exist (it creates and drives an LVGL indev via `lcdRun()`).

## 3. Battery monitor (`tdeck.cpp`)

`BOARD_BAT_ADC` (GPIO 4, ADC1 channel 3) reads VBAT through a 2:1 resistor
divider (100k/100k) that sits **upstream** of the power-enable gate — it has no
enable, conducts continuously (~21 µA @ 4.2 V), and so needs no power-up step.

`TdeckBattery::onInit` (init band, needs storage up) configures the ADC with curve-
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

## 4. On-device input HAL (`conditional/spangap-lcd/tdeck_lcd.cpp`)

This file exists only on an LCD build. It registers the board's `lcd_input_t`
ops (`init`, `touch_read`, `pointer_read`, `click_read`) with the LCD component
via `lcdSetInput()` in `TdeckLcdInput::onStart`; the component owns the panel, the cursor,
the focus group and the shell — see [spangap-lcd](../spangap-lcd) for the
`LcdApp`/launcher/statusbar model. The board supplies only input hardware.

**The LCD-owned indevs are interrupt-driven.** Touch, trackball and button are
`LV_INDEV_MODE_EVENT`, so LVGL runs no read timer. Every edge ends at the
component's exported `lcdInputISR` — the button's INT line straight (through
`tdeckStandbyBtnISR` while armed for wake), the trackball's four through
`tboxIsr`, which counts the pulse first — and it only flags +
`vTaskNotifyGiveFromISR`s the LCD task, which reads the indevs once. The GT911's
INT is the one that does not: it wakes the poll task (`tdeckTouchIsr`) to do the
I2C off the render path, and the latched sample reaches the LCD task through
`lcdTouchPoll()`, the same flag-and-notify from task context. The keyboard is the
only input that travels as an `lcdRun` hop (§4.4), and it carries the debt for a
post that doesn't land, because that one can be dropped where a notify cannot.
Idle LCD CPU is ~0 %. The shared GPIO ISR service is installed
`ESP_INTR_FLAG_IRAM` (the same flag LoRa's DIO1 path uses) so the IRAM-safe ISRs
survive cache-disabled windows.

### 4.1 GT911 touch

`tdeckTouchInit` builds the `esp_lcd_touch` handle by hand (the GT911 CONFIG
macro uses out-of-order designated initializers — a hard error in C++). The
controller **latches its I2C address from the INT level at power-on** (low →
0x5D, high → 0x14); the T-Deck has no touch reset, so the address is
sub-revision dependent — the code probes both. The result string is published to
`tdeck.touch`.

A failing GT911 read logs three lines for one event — the driver's, the I2C IO's,
and `tdeckTouchSample`'s own `warn()`, which is the one that names the error.
`tdeckTouchInit` drops the other two with two `logRule()` prefixes
(`esp_lcd_touch_gt911_read_data`, `panel_io_i2c_rx_buffer`), registered before the
probe so they cover it too: reading the address the board did *not* latch fails in
exactly that way before the real one answers. Prefixes, not tag levels —
everything else those two components have to say still comes through at whatever
the log settings ask for. Touch is left at IDENTITY (native coords) — the LCD component
applies the same `CONFIG_LCD_ROTATION` to the points as to the pixels — so
`tdeckTouchRead` returns raw native points and the maxes are
`CONFIG_LCD_NATIVE_WIDTH/HEIGHT`. The INT is taken `ANYEDGE` (polarity is
sub-rev dependent; a redundant edge costs one empty read). A consumer can set the
runtime `lcd.multi_touch` key to flip the GT911 (a 5-point controller) into
multipoint mode — the subscription lives in spangap-lcd (`lcd_touch.cpp`), not
here.

### 4.2 Trackball → pointer

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

### 4.3 Centre / Home button & standby

GPIO 0 (BOOT-strap, also the trackball centre-press, also the unused mic) is read
pulled-up active-low. The board owns the timing and the five meanings of a press,
driven by two one-shot `lv_timer`s on the LCD task:

- hold 300 ms → set `sys.standby = 1`;
- one click → a pointer click (asserted for exactly one poll);
- two clicks → `lcdGoHome`;
- three clicks → `lcdShowRecents`;
- any press while in standby → clear `sys.standby` (wake); the press still counts
  as the burst's first click.

A hold has one tier and one meaning: sleep. Navigation is by click count instead,
so it reads the same from the launcher, an app, or the switcher — the button asks
nothing about what is on screen.

Clicks accumulate while releases keep landing within 250 ms of each other, and
the burst is dispatched when that window closes: a press cancels
the pending window, a release restarts it. So a plain click costs one window of
latency — the price of a second click meaning something else — and three, being
the maximum, dispatches on its own release without waiting. The dispatched single
click is handed back through `click_read` (`s_clickAssert` + `lcdInputSignal()` to
make the LCD task poll the indev), which is what keeps *all* click policy on the
board side of the `lcd_input.h` contract.

After a transition the rest of the press is swallowed (`s_wakeAbsorb`) until the
finger lifts, so a held finger can't wake the device it just slept.

**Waking counts as a click.** The press that clears `sys.standby` opens a burst
(`beginWakeBurst`) rather than being discarded, so two or three clicks on a
sleeping device wake it *and* reach the launcher or the switcher — the gesture
means the same whether the screen was on or off. The burst is marked
(`s_wokeBurst`), and its one-click case dispatches nothing: that press meant
"wake", and a click at the cursor is not what the first touch of a sleeping
device should do. Which release opens the burst depends on how the wake was seen:
the still-down path opens it when the finger lifts, the ISR-latched path (the
press already lifted during the sleep-exit latency) has no release left to wait
for and opens it at once. `beginWakeBurst` is idempotent so the two can't both
restart the count.

Standby is **not** done by the button — the button (and the LCD inactivity
timeout) only flip the ephemeral `sys.standby` key; `tdeckStandby` (subscribed on
the LCD task) is what actually sleeps/wakes: GT911 reads gated off
(`s_touchAsleep`), display off via `lcdScreenSleep/Wake`, and the keyboard poll
task parked. Only the centre button stays live to wake.

### 4.4 QWERTY keyboard

The keyboard is an ESP32-C3 MCU on I2C0 @ 0x55. It is a poor fit for the generic
indev model, so it lives in the consumer, not spangap-core:

- **GPIO 46 ("key buffered" INT) is dead** — the stock/rgrizzell C3 firmware
  never drives it (verified on hardware and in the C3 source).
- The 1-byte read is **destructive** (pops the key, returns 0 when empty, no
  peek/count), so you can't tell a key is pending without consuming it.

So a dedicated `kbpoll` task (**prio 6, core 1**) polls the I2C off the
LCD task at a fixed `POLL_PERIOD` (30 ms). The C3 holds only the last unread
key (no buffer), so a lazy/adaptive backoff drops keystrokes under fast typing;
standby parks the task, so the always-on scan costs nothing while asleep.
It buffers bytes into a queue and bumps the LCD task via `lcdRun(kbDrain)` to
drain them through an LVGL keypad indev joined to `lcdInputGroup()`. That bump is
best-effort (a flash flush can stall the LCD task past `lcdRun`'s send bound), and
the bytes are queued either way, so a failed post leaves the drain *owed* and the
next pass — one `POLL_PERIOD` later — re-posts it, rather than stranding those keys
until someone presses another. Prio 6 is
one notch above the LCD task (prio 5) so a long synchronous redraw can't starve
the poll. The same task samples the GT911 — touch and keyboard are the only two
peripherals on I2C0, so one owner means no bus contention, and outranking the
render is what keeps touch sampling on cadence while the LCD task is busy.
`lcdSetHasKeyboard(true)` tells the component to suppress the on-screen keyboard.

GPIO 46 is still wired `ANYEDGE`: an edge wakes the poll early and is counted
for the `kbint` diagnostic (useful if a future C3 firmware ever drives it).
The indev is created lazily
in `kbDrain` (not in `TdeckLcdInput::onInit`): that `onInit` fires `lcdRun(kbCreateIndev)`
right after `spangapInit()`, which can land before the LCD task has registered
its `LCD_RUN_PORT` aux handler, so the create can silently fail; `kbDrain` runs
only once the LCD task is fully up, making it the race-proof place to build it.
The `readCb` synthesizes press+release over two reads and decodes a `0x0C` prefix
(Alt-C) as a one-shot "next lowercase is `LCD_KEY_CTRL | letter`" lead-in (1 s
window) for the terminal. A keystroke that woke the screen is swallowed (it only
served to wake).

### 4.5 Keyboard lamp

The keys are lit by the C3's own GPIO 9 at a PWM duty it sets on command `0x01`;
the host has a two-byte write and nothing else — no read-back, no state. So the
lamp is **driven by intent, not held at a level**: we say what it should be at
each moment the screen changes and say nothing in between.

**Whether the lamp is wanted at all is a latch (`s_kbLit`), and only a HELD wake
press sets it.** The waking press is already absorbed until it lifts, so that
window is free to carry a second meaning: `armKbHoldTimer()` on the press,
`cancelKbHoldTimer()` on the lift, and `kStandbyHoldMs` (300 ms) in between —
reusing the hold that sleeps the deck, in the other direction. The screen is
never made to wait for it: `sys.standby` is cleared the moment the press lands
and only the keys watch the clock. A tap therefore wakes the screen alone, which
is the point — waking a deck to glance at it should not light a keyboard.

Once lit it follows `lcdBacklightOnChange` (spangap-lcd), which reports every panel
duty on the LCD task. The wanted duty is `s.tdeck.kb_backlight` scaled by
`panel duty / lcdBacklightTarget()`: the same proportional cut at the dim step,
dark before the panel powers off. Reaching 0 also **clears the latch**, so each
trip out of standby has to ask again rather than inheriting the last one.
The LCD task only computes it; the **`kbpoll` task does the write**, because it
owns I2C0 — which is also why a parked task still pushes: the fade to dark runs on
past the moment standby parks it, and each step of that fade notifies it awake for
one write. A notify is therefore no longer proof that the glass fired, hence
`s_touchWakeFired`: only the GT911's own edge earns a `tdeckTouchWakeCheck`.

**A duty already written is never rewritten**, and that is the whole of the Alt+B
story. The C3 handles Alt+B itself and sends no byte, so the lamp can change
without us knowing; not rewriting means an Alt+B *stands* until the screen next
changes state, instead of being stamped back out within 30 ms. `0x02` keeps Alt+B's
own on-duty at the configured level so both routes light the keys the same.

The exception is `s_kbBlForce`, set whenever the computed duty and the panel duty
are both 0: **the write that takes the lamp out with the screen always lands**. A
deck must not sleep with lit keys, and an Alt+B we never saw is exactly how it
otherwise could — our cache would say 0, the keys would say otherwise, and nothing
would correct it. There is no way to read the lamp back in key mode; raw-matrix
mode would make the combo visible (`docs/tdeck.md`), which is the fix if this ever
needs to be exact rather than merely safe.

Moving the slider lights the keys at the level under the thumb for 3 s
(`KB_BL_PREVIEW_MS`) — a level is a thing you judge by looking at the keys, not at
a number — and that preview shows the level as set rather than ratio-scaled, since
the point is the level being chosen. It is suppressed while the panel is dark, so
a write to the key from the CLI can't light a deck in somebody's pocket.

## 5. ES7210 mic codec shim (`conditional/audio/tdeck_audio.cpp`)

Compiled only on an audio build. The [spangap/audio](../audio) engine owns the
I2S read/write engine; a board contributes Kconfig pins plus, for an
I2C-controlled input codec, this slice. The ES7210 is a quad-mic ADC on I2C0
@ 0x40 running as an **I2S slave** — the S3 (I2S0 master) supplies MCLK/BCLK/WS,
the ES7210 just clocks ADC samples onto DIN once its registers are programmed. So
`es7210InInit` is pure I2C register programming, no I2S; slave mode needs no
sample-rate coefficient table (the chip derives serial clocks from the supplied
BCLK/WS), so the init sequence is rate-independent. `TdeckAudio::onInit` (init band)
only stows the `audio_codec_ops_t` with the engine via `audioRegisterCodec`; the
audio task applies them lazily on first capture. The register table configures
16-bit I2S-slave, all four mics at a fixed gain (regs 0x43–0x46), HPF on. Of the
four hardware mics only one is populated on the board (the rest are unconnected).

## 6. Pitfalls

- **`TdeckBoard::onStart` before `spangapInit()`.** Power rail + chip-select park must
  precede the SD mount; this is the whole reason the board does anything in the
  start band. Don't move it to `onInit`.
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
  probe are expected, not a fault — which is why the library tags are muted and
  the board reports touch failures itself, in one line.
- **Trackball direction→pin and ball orientation are sub-revision dependent** (a
  sample had DOWN/RIGHT swapped). Flip `BOARD_TBOX_*` or the `dx/dy` signs if
  motion feels wrong — it is not a code bug.
- **The keyboard INT is dead on current firmware** — don't build an INT-only read
  path. The poll path is load-bearing; the self-healing edge detection is the
  only thing that would ever switch it off.
- **Battery divider is 2.0, not 2.11.** The ADC is curve-fit calibrated, so the
  true divider ratio applies; 2.11 is Meshtastic's compensation for an
  *uncalibrated* ADC and would over-read here.
