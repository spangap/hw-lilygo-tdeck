# gps — GNSS receiver task

[`gps.cpp/h`](../main/gps.cpp) — reads NMEA off the T-Deck Plus's on-board
GNSS receiver, parses every fix it can, and republishes a full snapshot into
ephemeral `gps.*` every `s.gps.interval` seconds. Autobauds and identifies the
chip on enable; on disable it commands the receiver into hardware-appropriate
standby. Pinned to core 0, priority 1, 6 KB PSRAM stack. NMEA-only today (no
UBX/binary parsing).

The task is a pure consumer/producer of storage: frontends (CLI, on-device
Settings, anything else) read `gps.*` and toggle `s.gps.enable`; the task owns
the UART and the parse. It has no dependency on rnsd/lxmf.

## Hardware

The T-Deck Plus ships **one of two receivers, batch-dependent**, with nothing
host-visible to tell them apart (see [`tdeck.md` §1.3](tdeck.md)):

| Chip | Default baud | Notes |
|---|---|---|
| Quectel **L76K** | 9600 | CASIC/`$PCAS` command set; no PMTK power-down without the FORCE pin |
| u-blox **MIA-M10Q** | 38400 | speaks UBX as well as NMEA; UART-wakeable software backup |

Both are hard-wired to the Grove header (the Plus repurposes it exclusively for
GPS), NMEA 8N1. Pins in [`tdeck.h`](../main/tdeck.h):

| Define | T-Deck Plus | Meaning |
|---|---|---|
| `BOARD_GPS_UART_NUM` | 1 | UART peripheral (typed to `uart_port_t` in `gps.cpp`) |
| `BOARD_GPS_RX_PIN` | 44 | host RX ← GPS TX |
| `BOARD_GPS_TX_PIN` | 43 | host TX → GPS RX |

Two constraints worth knowing:

- **No independent GPS power switch.** The receiver hangs off the shared
  `BOARD_POWER_EN_PIN` (GPIO 10) rail with the display/SD/LoRa, already driven
  HIGH by `tdeckPreInit()`. We cannot cut its power alone — the only per-chip
  lever is what we send over the UART (see *Standby* below).
- **No PPS** is routed on the Plus (the Pro breaks it out on GPIO 1). So there
  is no GPS interrupt line; the task drains the UART on a ≤1 s cadence, which is
  the receiver's natural 1 Hz output rate anyway.

## Autobaud & chip detection

On enable the task tries **38400 then 9600**, locking on the first
checksum-valid NMEA sentence, and infers the model from the baud that worked
(38400 → MIA-M10Q, 9600 → L76K). This is the documented LilyGo batch
distinguisher, not a true probe — a receiver reconfigured to a non-default baud
would be mis-identified, which is out of scope. The detected model and baud are
published to `gps.model` / `gps.baud`.

Each autobaud candidate sends a wake edge (`0xFF`) before listening, so a u-blox
sitting in software backup is revived before detection (its ~0.5–1 s restart is
covered by the 1.5 s listen window).

## Configuration (`s.gps.*`, persisted)

| Key | Default | Meaning |
|---|---|---|
| `s.gps.enable` | `0` | gate: 1 runs the task, 0 puts the chip in standby and tears down the UART |
| `s.gps.interval` | `5` | fix cadence: `0` = continuous tracking (full power, 1 Hz), `1`–`10` = PSM cyclic tracking (PSMCT) period in seconds |
| `s.gps.ignore_clock` | `0` | 1 = never set the system clock from GPS |

`s.gps.interval` drives the receiver's own update rate on the u-blox MIA-M10Q: it
emits `UBX-CFG-VALSET` setting `CFG-RATE-MEAS` to the period and
`CFG-PM-OPERATEMODE` to `FULL` (interval 0) or `PSMCT` (1–10). In PSMCT the chip
low-power-tracks between fixes — roughly halving VCC draw at 1 Hz, less at longer
periods — versus continuous tracking at 0. The Quectel L76K speaks no UBX PSM, so
there the interval is only a publish throttle and the chip keeps running at 1 Hz.
Either way the UART is still drained at ≤1 Hz so the RX buffer can't overflow, and
the publish cadence follows the interval (continuous publishes every 1 s).

Note: PSM doesn't cover BeiDou B1C and won't process SBAS — neither is enabled in
the default constellation set, so this is only a constraint if that config changes.

## System clock from GPS

A fresh fix (valid position **and** date+time) disciplines the system clock once
per acquisition via `settimeofday()`, then publishes `sys.time.valid = 1` — the
same flag NTP raises — so the status-bar clock and any time-gated logic work on
a GPS-only device with no network. The fix time is UTC; the displayed wall time
still follows `s.ntp.tz`. Times before 2025-01-01 are rejected as bogus, and the
clock is re-disciplined on the next acquisition after a fix is lost. The actual
`settimeofday()` only fires if the system clock is **≥ 2 s off** from the GPS
time — a step is not a slew, and re-jamming whole-second NMEA precision on top
of an already-good clock would just produce small non-monotonic jumps;
`sys.time.valid` is still set in the within-tolerance case. Set
`s.gps.ignore_clock = 1` to leave the clock entirely to NTP / the browser.

## Published state (`gps.*`, ephemeral)

Republished as one atomic snapshot every `s.gps.interval` s. Lat/lon/alt/etc.
are strings (the storage API has no float type); ints are ints.

| Key | Unit / format | Source |
|---|---|---|
| `gps.model` | string | inferred chip, or `detecting...` / `not detected` / `(disabled)` |
| `gps.baud` | int | locked baud (0 when not running) |
| `gps.state` | string | `off` / `detecting` / `not detected` / `standby` / `power-cycle to wake` / `acquiring` / `fix` |
| `gps.fix` | `none` / `2D` / `3D` | GSA fix type |
| `gps.quality` | int | GGA fix-quality indicator |
| `gps.lat` `gps.lon` | decimal degrees (6 dp) | RMC/GGA; empty until first position |
| `gps.alt` | m above MSL (1 dp) | GGA |
| `gps.geoid` | m (1 dp) | GGA geoid separation |
| `gps.speed` | km/h (1 dp) | RMC (knots×1.852) / VTG |
| `gps.course` | deg (1 dp) | RMC / VTG track made good |
| `gps.sats_used` | int | GGA satellites used in the solution |
| `gps.sats_view` | int | sum of per-constellation GSV in-view counts |
| `gps.hdop` `gps.vdop` `gps.pdop` | dilution (2 dp) | GSA |
| `gps.snr` | dBHz | best C/N0 seen this epoch (GSV) |
| `gps.utc` | `YYYY-MM-DD HH:MM:SS` | RMC date + time, UTC |
| `gps.fix_age` | seconds, `-1` = never | time since the last *valid* RMC fix |

### Reading a fix correctly

`gps.lat`/`gps.lon` hold the **last-known** position and are *not* cleared when
the fix is lost — `gps.fix_age` is the staleness gate. A snapshot showing a
position with `gps.fix = none` and a growing `gps.fix_age` means "we had a fix
N seconds ago, here's where, but it's stale now". `gps.fix_age = -1` means no
fix has ever been acquired this session (the position fields are empty).

`gps.sats_view` is summed across constellations (GP/GL/GA/GB…), counting each
talker's reported total once (on its first GSV message) and reset per 1 Hz
epoch; `gps.snr` is likewise the best carrier-to-noise this epoch, not a
running maximum.

## Standby (on disable)

Because the rail is shared, "disable" cannot cut power — instead the task
commands the detected chip into its deepest reachable low-power state over the
UART, then drops the UART:

- **u-blox M10** — `UBX-RXM-PMREQ` software backup (flags `backup|force`,
  wakeup source `uartrx`). Real low power, and it **wakes on a UART RX edge**,
  so re-enabling revives it automatically (autobaud's wake byte does it). No
  user action needed.
- **L76K** — `$PMTK225,4` **deep backup**. The documented `$PCAS` set has no
  UART-wakeable standby; its low-power is a FORCE_ON pin that the Plus doesn't
  route. This is a deliberate trade (chosen over no standby at all): real power
  saving, but **the only way back is a power cycle**. The task records this in a
  RAM flag (`s_needsPowerCycle`); on re-enable it doesn't fumble the serial — it
  publishes `gps.state = power-cycle to wake` and the on-screen status shows the
  same. A real reboot clears the flag (fresh power = fresh chip) and the next
  enable autobauds normally. We never send the L76K's other backup variants that
  also need the FORCE pin to wake, for the same reason.

> Caveat: the L76K deep-backup command relies on the chip accepting `$PMTK225,4`
> (Meshtastic drives L76K backup the same way). If a batch ignores PMTK entirely,
> the command is a silent no-op — the chip keeps running on the shared rail and
> the "power-cycle to wake" status is misleading for that unit. The u-blox path
> is unambiguous.

## CLI

| Command | Effect |
|---|---|
| `gps` | print status (state, model, baud, interval, position, fix, sats, DOP, UTC) |
| `gps on` / `gps off` | set `s.gps.enable` |

## On-device settings (`CONFIG_SPANGAP_LCD`)

GPS is surfaced among the board's own sections of the **System** Settings page
(generated from `straddle.yaml`), not a menu of its own:

- **Hardware** section (top): `Board` shows `sys.board`, `GPS` shows `gps.model`
  (which receiver was found, or its disabled/standby state) and `Touch` shows
  `tdeck.touch` — the result of the GT911 I²C probe (`GT911 @ 0x5D` or
  `not found`), published by `tdeckTouchInit`.
- **GPS** section: `Enable` (→ `s.gps.enable`), `Interval (s)` slider
  (→ `s.gps.interval`, 0–10, where 0 = continuous and 1–10 = PSMCT period), and
  `Status` (→ `gps.state`, where the "power-cycle to wake" message appears).

## Scope & possible extensions

NMEA-only by choice. The richer per-fix fields the MIA-M10Q can give
(`hAcc`/`vAcc` accuracy estimates, velocity vector) need UBX `NAV-PVT` parsing —
M10-only, a future branch. Other deferred items, each a separate subsystem:
**AGPS** (u-blox AssistNow Online/Autonomous for faster TTFF), **power
duty-cycling** (chip PSM cyclic-tracking rather than all-or-nothing), and
**WiFi-positioning** (BSSID scan → a geolocation API for a coarse fix or an
AGPS seed; the receiver itself can't ingest BSSIDs).

## Files

- [`main/gps.cpp`](../main/gps.cpp) / [`main/gps.h`](../main/gps.h) — the task.
- [`main/tdeck.h`](../main/tdeck.h) — `BOARD_GPS_*` pin constants.
- [`main/tdeck.cpp`](../main/tdeck.cpp) — `sys.board` publish; `tdeck.touch` publish.
- [`main/main.cpp`](../main/main.cpp) — `gpsInit()`.
