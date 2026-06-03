# hw-tdeck

## What is this?

**hw-tdeck** is the application straddle that produces the
device image of [reticulous](https://github.com/reticulous) for the
LilyGo T-Deck Plus (ESP32-S3FN16R8, 8 MB octal PSRAM, 16 MB flash,
SX1262 LoRa, GPS, 320×240 ST7789 LCD, QWERTY keyboard, trackball).

It owns the **T-Deck Plus board HAL**, the **GNSS task**, the
**partition layout**, the **OTA public key**, the **`app_main` boot
sequence**, and the **Quasar browser SPA shell**. Every protocol piece
(rnsd, lxmf, nomad, lora, tcp, auto, espnow, maps) lives in its own
sibling straddle and is consumed by name from `straddle.yaml`.

[Reticulum](https://reticulum.network) is Mark Qvist's cryptography-
based networking stack for building resilient, self-configuring
networks over anything that can carry packets — LoRa, packet radio,
plain TCP/IP. **reticulous** is the first serious application built on
[spangap](https://github.com/spangap) — it brings Reticulum (and LXMF
messaging, and Nomad Network browsing, and live maps) to a single
hand-held device.

## What this straddle owns

```
hw-tdeck/
├── straddle.yaml          requires: every reticulous-* + the spangap straddles it needs
├── esp-idf/
│   └── main/
│       ├── main.cpp       app_main: tdeckPreInit → spangapInit → tdeckPostInit →
│       │                  task inits → spangapPostAppInit
│       ├── tdeck.{cpp,h}  T-Deck Plus board support: power/CS, the lcd input HAL
│       │                  (touch/trackball/button), QWERTY keyboard (own I2C/indev/
│       │                  ISR/poll, self-heals to interrupt-driven). Panel pins are
│       │                  the lcd component's CONFIG_LCD_* (sdkconfig.defaults).
│       ├── gps.{cpp,h}    GNSS receiver task (dual-chip autobaud, NMEA parse,
│       │                  ephemeral gps.* publish, standby on disable)
│       ├── ports.h        ITS port constants (the few that are app-side)
│       ├── ota_pubkey.h   the OTA verification key for this app (public)
│       └── esp_idf_hal.h  pin-defs include path consumed by tr-lora
├── partitions.csv         the app's partition layout (overrides platform default)
├── web-interface/         Quasar SPA shell — only the reticulous app shell
│   ├── package.json       depends on spangap-browser (symlinked locally)
│   └── src/
│       ├── App.vue
│       ├── boot/          register straddle modules + auth
│       ├── modules/       reticulous-app-specific
│       └── pages/         routes
├── assets/lcd-icons/      app-specific launcher icons
├── data/                  app-specific factory-state overlay folded with spangap-core's
├── docs/                  black-box subsystem docs + component-plan.md
├── tests/                 Python test harness + echo peers
├── scripts/               host-side RNS helpers (rnsd-up, lxmf-send, announce-sniff, …)
├── tools/                 misc developer tools
├── keys/                  ACME / OTA keys (private keys gitignored)
├── build.sh               idf.py wrapper: sets IDF env, locks builds, chmods build/
├── idf_ext.py             idf.py extension: --spangap flag for sibling-checkout dev
└── CMakeLists.txt
```

## What lives in sibling straddles

Every protocol piece is its own straddle:

| Concern         | Straddle                                         |
| --------------- | ------------------------------------------------ |
| RNS core        | [rns](../rns)            |
| TCP transport   | [tr-tcp](../tr-tcp)              |
| AutoInterface   | [tr-auto](../tr-auto)            |
| ESP-NOW         | [tr-espnow](../tr-espnow)        |
| LoRa            | [tr-lora](../tr-lora)            |
| LXMF messaging  | [lxmf](../lxmf)            |
| Nomad pages     | [nomad](../nomad)          |
| Offline maps    | [maps](../maps)                                  |
| Platform        | [spangap-core / -net / -web / -lcd](../../s/)    |
| Remote access   | [wg / acme / duckdns / upnp / ota](../../s/)     |

## Building

ESP-IDF 5.5.4 (installed at `~/.espressif/v5.5.4/esp-idf`).

First-time host setup: `./build-dependencies.sh` (macOS + Homebrew;
errors loud if brew / ESP-IDF / Node are missing). Idempotent.

`build.sh` is a thin idf.py wrapper:

```bash
./build.sh                 # = idf.py build (Quasar + deploy.sh + LittleFS image)
./build.sh --spangap build # build against sibling spangap-core checkout
./build.sh -p /dev/tty.usbmodemNNNN flash monitor
```

Sibling-checkout development uses `--spangap` (handled by `idf_ext.py`):
the flag injects a transient `path:` for `spangap/spangap-core` for the
duration of the run and restores the registry-shaped manifest on exit.

`build.sh` takes a **single-build lock** (`build.lock` next to it) so a
second concurrent build errors out; the lock auto-frees on exit. A
hard-killed build leaves a stale lock — remove it by hand.

The T-Deck Plus uses native ESP32-S3 USB CDC; the serial port appears
as `/dev/tty.usbmodemNNNN`.

## Hardware

LilyGo T-Deck Plus: **ESP32-S3FN16R8** (16 MB flash, 8 MB octal PSRAM),
SX1262 LoRa, **2.8" 320×240 ST7789V LCD**, BlackBerry-style trackball,
on-keyboard ESP32-C3 (I²C addr 0x55), GPS (Quectel L76K @9600 or u-blox
MIA-M10Q @38400 depending on batch), microSD on shared FSPI. Full pin
map in [`docs/tdeck.md §1.2`](docs/tdeck.md).

- LoRa pins: CS=9, DIO1=45, RST=17, BUSY=13. SPI shared with display
  + SD: SCK=40, MOSI=41, MISO=38. TCXO 1.8 V.
- LoRa antenna: PCB-trace on the standard SKU; IPEX (u.FL) on the
  external-antenna SKU. No SMA bulkhead exists.
- WiFi/BT antenna: PCB-trace on the ESP32-S3 module.
- Display + capacitive touch (GT911), QWERTY keyboard, centre button,
  and trackball: all driven by spangap-lcd's LVGL UI
  (`CONFIG_SPANGAP_LCD`), fully interrupt-driven.

The Heltec WiFi LoRa 32 V3 was evaluated and rejected — no PSRAM, and
the spangap platform requires octal PSRAM.

## Read next

- [INTERNALS.md](INTERNALS.md) — the app-side bring-up sequence,
  board HAL details, subsystem index, conventions.
- [docs/component-plan.md](docs/component-plan.md) — **authoritative**
  architecture + rollout plan. Several decisions are non-obvious; read
  this before touching anything.
- The individual subsystem docs under `docs/` (rnsd, lxmf, tcp, auto,
  lora, gps, maps, tdeck, testing, internals, plans).

## See also

- [README-old.md](README-old.md) — pre-split README (now in this
  file).
- [CLAUDE.md](CLAUDE.md) — pre-split developer guide (content moved
  to [INTERNALS.md](INTERNALS.md), with platform-wide bits moved to
  [spangap/INTERNALS.md](../../s/spangap/INTERNALS.md) and per-straddle
  pieces moved to each straddle's INTERNALS).
