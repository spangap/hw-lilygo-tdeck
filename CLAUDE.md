# reticulous — Reticulum (RNS) on spangap

ESP-IDF firmware bringing Mark Qvist's [Reticulum Network Stack](https://reticulum.network)
to a spangap device. Initial hardware target: LilyGo T-Deck Plus
(ESP32-S3, SX1262 LoRa, 8 MB octal PSRAM, 16 MB flash).

> **Status — 2026-05-23:** well past scaffold. The µR fork is ported
> and patched, and the per-task firmware is real and hardware-verified
> against upstream Reticulum/LXMF — `rnsd` (identity, Transport, path
> table) and `lxmf` (messaging, per-contact threads, Link + Resource
> transfer) are the large modules, with `tcp`/`auto`/`lora`/`espnow`
> transports live (`auto` = AutoInterface, zero-config IPv6 link-local
> multicast LAN). The browser SPA is
> built out too: per-transport settings panes, Status floating windows,
> and a full LXMF chat UI. **`docs/component-plan.md` is still the
> authoritative architecture + rollout plan — several decisions are
> non-obvious, so read it before touching anything.**

Sibling subproject to seccam: same spangap platform, completely
different application. Anything not specific to Reticulum / LXMF /
the T-Deck lives in [`../spangap/CLAUDE.md`](../spangap/CLAUDE.md) —
read it together with this file.

## Where is everything?

Fast map to files. Paths from this dir (`reticulous/`). Only reticulous-specific things live here; everything platform-wide is in [`../spangap/`](../spangap/) (see its "Where is everything?").

**Firmware (`main/`)** — per-task files (rnsd, lxmf, nomad, lora, tcp, auto, espnow, gps, maps) plus `lxmf_lcd.cpp`, `nomad_lcd.cpp`, `tdeck.h`/`tdeck.cpp` (all T-Deck Plus board support: power/CS/reset, display/touch/trackball/button HAL, and the QWERTY keyboard that owns its own I2C/indev/ISR/poll and self-heals to interrupt-driven), `esp_idf_hal.*`, `ports.h`, `main.cpp` are detailed under **What's reticulous-specific** below.

**µR fork (`components/microreticulum/`)** — the hard fork of attermann/microReticulum:
- Core RNS classes (Reticulum, Transport, Destination, Identity, Link, Packet, Channel, Interface, Resource) → [`src/`](components/microreticulum/src/)`*.cpp/.h`
- Crypto → [`src/Cryptography/`](components/microreticulum/src/Cryptography/); elliptic-curve math → [`src/donna/`](components/microreticulum/src/donna/)
- Storage abstraction (`microStore`) → [`include/microStore/`](components/microreticulum/include/microStore/)
- Our spangap patches: summarized under **Components** below + in [`components/README.md`](components/README.md)

**Browser (`web-interface/src/`)** — Quasar SPA, reticulous-specific UI only:

| Looking for | Path |
|---|---|
| Per-task browser state + RPC | `web-interface/src/modules/{rnsd,lxmf,tcp,lora,espnow}.ts` |
| Settings panels (Reticulum + Transports) | `web-interface/src/panels/{Rnsd,Lxmf,Tcp,Lora,Espnow}Panel.vue` |
| Status floating windows (Map, Nodes, Messages, Announces) | `web-interface/src/panels/{Map,Nodes,Messages,Announces}Window.vue` |
| LXMF chat UI (conversation list/thread, bubble, composer, contact, peer picker/avatar, announces) | [`web-interface/src/components/lxmf/`](web-interface/src/components/lxmf/)`*.vue` |
| Pinia store wiring | [`web-interface/src/stores/index.ts`](web-interface/src/stores/index.ts) |

**Shared platform UI** — the web-UI's CLI/terminal, Log window, `FloatingWindow`, `Setting*` controls, WebRTC session, auth and menu store are **not** copied here. They come from the `spangap-browser` package, symlinked via `"spangap-browser": "file:../../spangap/browser"` in `web-interface/package.json` — look in [`../spangap/browser/src/`](../spangap/browser/src/).

**Docs** — [`docs/component-plan.md`](docs/component-plan.md) is authoritative; black-box component docs (`rnsd`, `lxmf`, `tcp`, `lora`), [`tdeck.md`](docs/tdeck.md), [`testing.md`](docs/testing.md), [`internals/`](docs/internals/) and older [`plans/`](docs/plans/) are indexed under **Subsystem deep dives** and **Authoritative plan** below.

**Build / tests / scripts** — [`build.sh`](build.sh) (idf.py wrapper), [`idf_ext.py`](idf_ext.py) (`--spangap`), [`CMakeLists.txt`](CMakeLists.txt), `partitions.csv`; Python test harness + echo peers → [`tests/`](tests/); host-side RNS helper scripts (`rnsd-up`, `lxmf-send`, `announce-sniff`, `link-probe`, …) → [`scripts/`](scripts/); LCD icon SVGs → [`assets/lcd-icons/`](assets/lcd-icons/).

## Build

ESP-IDF 5.5.4 (installed at `~/.espressif/v5.5.4/esp-idf`).

First-time host setup: **`./build-dependencies.sh`** (macOS + Homebrew; errors if
brew / ESP-IDF / Node are missing). It `brew install`s cairo, bootstraps the IDF
Python venv, pip-installs the launcher-icon rasterizer deps (`pillow cairosvg
pypng lz4` — without all four `scripts/lcd-icons.py` silently ships label-only
tiles), and `npm install`s the web-interface. Idempotent; run it once per machine.

`build.sh` takes a **single-build lock** (`build.lock` next to it) for build-like
invocations (build/flash/app/all, or bare): a second concurrent build errors out
until the first finishes (the lock auto-frees on exit, incl. Ctrl-C / failure). A
hard-killed build leaves a stale `build.lock` — remove it by hand (the error says
so). Needed because the SMB tree is built from both the VM and the host, and two
concurrent builds corrupt each other. monitor/clean/menuconfig/size aren't locked.

On the builder VM use **`./build.sh`** — a thin idf.py wrapper that sets
`IDF_PATH`/`IDF_TOOLS_PATH`, sources `export.sh`, and self-heals the IDF
Python venv if a host `python3` bump invalidated it (probes `export.sh`,
runs `idf_tools.py install-python-env` on failure). Args pass through; it
also `chmod -R a+rwX build` after (the tree is an SMB share — the host-side
flasher runs as a different user). A host-Python bump that leaves `build/`
configured for the old venv is handled too: build.sh pins `IDF_PYTHON_ENV_PATH`
to the interpreter recorded in `build/CMakeCache.txt`, so incremental builds
keep working without a fullclean (set `IDF_PYTHON_ENV_PATH` yourself to override).

```bash
./build.sh                 # = idf.py build (quasar + deploy.sh + LittleFS image)
./build.sh --spangap build # build against the sibling spangap-core checkout
./build.sh -p /dev/tty.usbmodemNNNN flash monitor
```

Direct `idf.py` still works if the env is already set up:

```bash
idf.py build          # also runs quasar build + deploy.sh + LittleFS image
idf.py -p /dev/tty.usbmodemNNNN flash
idf.py -p /dev/tty.usbmodemNNNN monitor   # exit: Ctrl+]
```

Timezone data (`timezones.json`) is **platform-owned**: it lives in `spangap-core/data/factory_state/timezones.json` — a plain user-state file at the root of the state store (no longer an `s.time.zones` external storage blob, so the ~15 KB IANA→POSIX map stays out of RAM; the device parses it transiently on a timezone change). It ships here through the data merge (`spangap_create_factory_image` folds spangap-core's `data/` into our `/fixed` image). It is not fetched on any build. Refresh it from the **spangap-core** checkout with `make timezones` (runs `update-zones.py`; commit the result there) — every consumer inherits it. reticulous no longer carries its own copy.

The committed `main/idf_component.yml` has **no** `path:` for `spangap/spangap-core` — the default build resolves it from the registry. To develop against the sibling checkout (`../../spangap/spangap-core`), pass `--spangap` to idf.py; the flag injects a transient `path:` for the duration of the run and restores the manifest on exit (see [idf_ext.py](idf_ext.py)). If an `idf.py --spangap …` run is killed before cleanup, the next invocation refuses to start until you `mv main/.idf_component.yml.spangap-backup main/idf_component.yml`. The committed manifest is never edited.

```bash
idf.py --spangap build                          # registry-shape manifest + local sibling
idf.py --spangap -p /dev/tty.usbmodemNNNN flash monitor
```

`idf.py flash` accepts `--flash-cmd "<cmd>"` to bypass esptool — the build still runs first so `build/flasher_args.json` is fresh; the custom command can read it. Use for RFC 2217, custom flash tools, OTA upload, etc.

Same spangap bootstrap pattern as seccam: top-level `CMakeLists.txt`
locates `../spangap/spangap-core` (via the path: injected by `--spangap`)
or `managed_components/spangap__spangap-core`, layers
`sdkconfig.defaults.spangap` first, then ours.

T-Deck Plus serial port appears as `/dev/tty.usbmodemNNNN` (varies
by reset). The board uses native ESP32-S3 USB CDC — no FT2232 / CP210x.

## What's reticulous-specific

**Firmware (`main/`):** task per concern.

- `rnsd.cpp/h` — RNS protocol task (identity, destinations, path
  table, transport state machine). Owns mR's `Reticulum` + `Transport`,
  the iface table, the bidirectional mailbox port, the announce
  fan-out port, the management destination (`rnstransport.remote.management`),
  and the optional probe responder (`rnstransport.probe`, PROVE_ALL,
  gated on `s.rnsd.respond_to_probes`). **Zero networking/radio
  dependencies.** Core 0, prio 2, 12 KB PSRAM stack.
  Exposes a **client API ([rnsd.h](main/rnsd.h))** with two halves:
  (1) byte-array primitives — sha256, sign, verify, destination_hash,
  identity ops, recall, request_path — encapsulating every mR
  primitive downstream consumers need; (2) typed conn-openers —
  `rnsdDestOpen()` for destinations plus `rnsdLinkOpen()` /
  `rnsdDestListenLinks()` for Links (implemented and hardware-verified;
  see [docs/plans/link.md](docs/plans/link.md), exercised by `rnsd clink`).
  Downstream tasks operate
  on raw byte arrays and storage sentinels; they never include
  `RNS::Identity` or other mR types. → [docs/rnsd.md](docs/rnsd.md).
- `lxmf.cpp/h` — LXMF 0.9.8 messaging task. Sits on top of rnsd's
  byte-array API + the mailbox + announce-fanout ITS ports. **Zero
  mR includes**; mR is an implementation detail of rnsd. Storage is
  the API — frontends (browser, CLI, on-device UI) read/write
  `s.lxmf.*` / `lxmf.*` / `secrets.lxmf.*`; lxmf subscribes only to
  cmd sentinels (`lxmf.cmd.` + per-id `lxmf.id.<n>.cmd.`) and
  processes each sentinel inline. No auto-create at boot — devices
  without identities run as transport-only nodes. Core 1, prio 1,
  8 KB PSRAM stack. → [docs/lxmf.md](docs/lxmf.md).
- `nomad.cpp/h` — Nomad Network **page client** (the "text web" half;
  messaging is lxmf). Sits on rnsd's byte-array API: subscribes to the
  `nomadnetwork.node` announce fan-out (drift feed → `nomad.nodes.*`),
  and fetches pages via the request/response primitive (`rnsdLinkOpen`
  + `rnsdLinkRequest`, nav driven by `nomad.cmd.go`). **Zero mR
  includes**, **never parses Micron** — bytes in, bytes out; the
  viewing endpoint (SPA/LCD) renders. Storage is the API
  (`nomad.nodes.*`, `nomad.nav.*`, `nomad.page.*`, `s.nomad.bookmarks.*`).
  Core 1, prio 1, 8 KB PSRAM stack. → [docs/nomad.md](docs/nomad.md),
  [docs/plans/nomad.md](docs/plans/nomad.md).
- `lora.cpp/h` — SX1262 transport task. RadioLib +
  [`esp_idf_hal.cpp/h`](main/esp_idf_hal.h) (custom ESP-IDF HAL).
  Owns DIO1 ISR. Implements RNode on-air framing (mandatory for
  ecosystem interop). → [docs/lora.md](docs/lora.md).
- `auto.cpp/h` — AutoInterface transport task. Zero-config RNS over
  the LAN: IPv6 link-local multicast peer discovery + unicast UDP data,
  wire-compatible with upstream RNS AutoInterface. BSD sockets (lwIP
  core locking is off, so the raw API can't be driven off the tcpip
  thread); a small `auto-rx` helper task `select()`s the UDP sockets
  and hands datagrams to the auto task via queue + notify, keeping the
  single `itsPoll` wait point. → [docs/auto.md](docs/auto.md).
- `tcp.cpp/h` — TCP transport task. Outbound dial (via net's
  `NET_PORT_TCP_DIAL`) only in Phase 1; inbound listen lands in
  Phase 5. HDLC byte-stuffing on the wire. Each peer registers as
  its own iface. → [docs/tcp.md](docs/tcp.md).
- `espnow.cpp/h` — RNS-over-ESP-NOW transport task. Single broadcast
  peer (FF:FF:…), one RNS packet ↔ one ESP-NOW v2 frame (no on-air
  framing), Espressif long-range PHY (250/500 kbps). WiFi ownership
  stays with net — gates on `netIsUp()`, (de)inits on `NET_EV_UP`/
  `NET_EV_DOWN`. Same lifecycle/registration model as the other
  transports (`s.espnow.enable`, `RNSD_PORT_REGISTER`). See
  [docs/component-plan.md](docs/component-plan.md) §5/§11.
- `gps.cpp/h` — GNSS receiver task (T-Deck Plus). Autobauds the on-board
  NMEA receiver (Quectel L76K @9600 or u-blox MIA-M10Q @38400, batch-dependent),
  parses fixes, republishes ephemeral `gps.*`; on disable commands the chip into
  standby (u-blox UART-wakeable backup; L76K deep backup → power-cycle to wake).
  Surfaced in the T-Deck Settings pane. → [docs/gps.md](docs/gps.md).
- `maps.cpp/h` — on-device offline map viewer (`CONFIG_SPANGAP_LCD`). Launcher
  program that blits pre-baked RGB565 slippy-map tiles from SD
  (`/sdcard/maps/<z>/<x>/<y>.bin`) centred on the GPS fix; worker task owns the
  tile cache + slippy math, lcd task composites. Tiles built on a computer with
  [`scripts/maketiles.py`](scripts/maketiles.py). → [docs/maps.md](docs/maps.md).
- `ports.h` — ITS port constants + transport-side connect-payload
  struct (`rnsd_transport_t`) + the `RNSD_DEST_*` frame opcodes consumers
  use. The `RNSD_PORT_DEST` connect struct itself is rnsd-private
  (lives in rnsd.cpp) — callers go through `rnsdDestOpen()`.
- `tdeck.h`/`tdeck.cpp` — all T-Deck Plus board support. Per-target
  `BOARD_*` constants (`CONFIG_RETICULOUS_BOARD_*`; T-Deck Plus only today —
  Heltec WiFi LoRa 32 V3 was evaluated and rejected, no PSRAM, the spangap
  platform requires octal PSRAM) plus the driver code: power/CS/reset, and
  (CONFIG_SPANGAP_LCD) the display/touch/trackball/button HAL and the QWERTY
  keyboard. Two-phase bring-up API around `spangapInit()`:
  **`tdeckPreInit()`** before, **`tdeckPostInit()`** after — see
  [docs/tdeck.md](docs/tdeck.md). The `BOARD_*` pin map is also consumed by
  `lora.cpp` and `main.cpp`.
- `main.cpp` — `app_main`: **`tdeckPreInit()`** → `spangapInit()` →
  **`tdeckPostInit()`** → task inits → `spangapPostAppInit()`.
  `tdeckPreInit()` MUST run **before** `spangapInit()`: the first
  shared-SPI-bus access is `fs_mount_sd()` *inside* `spangapInit()`, so the
  board prerequisites must already be set — (1) drive `BOARD_POWER_EN_PIN`
  HIGH (T-Deck gates the +3.3 V rail to SD/display/GPS/LoRa behind it; +100 ms
  settle), (2) park the LCD **and** SX1262 LoRa CS lines HIGH so neither drives
  MISO during the SD/LoRa probe — and it registers the display HAL that
  `spangapInit()`'s `lcdInit()` brings up. `tdeckPostInit()` runs **after**
  (the keyboard needs the lcd task `spangapInit()` created). `loraInit()`
  re-asserts the power pin (idempotent no-op).
- `ota_pubkey.h` — placeholder copied from seccam; regenerate via
  spangap's `ota-keygen.py` before any release ceremony.

**Components (`components/`):** µR hard fork.

- `microreticulum/` — fork of `attermann/microReticulum` at a pinned
  commit. Crypto rewritten against mbedTLS + esp_wireguard's
  software Curve25519. cJSON + msgpack-c instead of ArduinoJson +
  MsgPack. Spangap logging macros instead of `Serial.print*`. No-op
  file I/O (we own persistence). Event-driven, no top-level
  `Reticulum::loop()`.

  **Spangap patches** to upstream (carried in our fork):
  - `Transport.cpp`: RAII guard for `_jobs_locked` — every early
    return from `Transport::inbound` releases the lock. Upstream
    leaks it on malformed-packet / cache-request / link-MTU-clamp
    exits, permanently disabling `Transport::jobs()`.
  - `Transport.cpp`: a requested `PATH_RESPONSE` bypasses the
    `random_blob` replay guard in announce ingest. Relays answer from
    a cached announce, so a re-requested path always carries a
    seen blob; upstream escapes via `path_is_unresponsive` (not ported)
    — we key on an outstanding `_path_requests` entry instead.
    Without this, path discovery works exactly once then goes silent.
  - `Identity.cpp` + `.h`: `static std::recursive_mutex
    _known_destinations_mux` taken by every accessor
    (`remember` / `recall` / `recall_app_data` / `validate_announce`
    / `cull_known_destinations` / save+load). Enables safe cross-task
    `Identity::recall` via rnsd's `rnsdRecallPubkey`.
  - `Packet.cpp`: malformed-packet error path now dumps the first
    ≤8 bytes hex, so we can tell HEADER_1 vs HEADER_2 mis-parse,
    HDLC desync, noise byte etc.

**Browser (`web-interface/`):** Quasar SPA on `spangap-browser`.

- Settings panels per task at **Settings → Reticulum** and
  **Settings → Transports → {TCP, AutoInterface, LoRa, ESPnow}**.
- Live state in floating subwindows under the **Status** menu, same
  `FloatingWindow` component as Log and CLI: Map, Reticulum, TCP,
  LoRa.
- On-device UI: enabled (`CONFIG_SPANGAP_LCD`) — spangap-core's `lcd` LVGL
  launcher + on-device Settings panes, developed in lockstep with the SPA.
  See [`../spangap/docs/lcd.md`](../spangap/docs/lcd.md).

## Subsystem deep dives

- **rnsd** — [docs/rnsd.md](docs/rnsd.md). RNS protocol task: identity,
  Reticulum/Transport bring-up, iface table, raw-packet API,
  announce logger, path-table snapshot, CLI (`rnsd`, `rnstatus`,
  `rnpath`, `rnprobe`).
- **tcp** — [docs/tcp.md](docs/tcp.md). Outbound TCP transport: peer
  table, HDLC framing, dial via net's `NET_PORT_TCP_DIAL`,
  registration with rnsd, reconnect backoff.
- **auto** — [docs/auto.md](docs/auto.md). AutoInterface transport:
  IPv6 link-local multicast peer discovery + unicast UDP data over BSD
  sockets, `auto-rx` select() helper, group/peer model, registration
  with rnsd. Wire-compatible with upstream RNS AutoInterface.
- **lora** — [docs/lora.md](docs/lora.md). SX1262 transport: RadioLib +
  EspIdfHal, DIO1 ISR + task-notification loop, RNode on-air
  framing with split reassembly, half-duplex TX/RX coordination.
- **lxmf** — [docs/lxmf.md](docs/lxmf.md) is the black-box / consumer
  view (storage API, cmd sentinels, CLI, behaviours, limits);
  [docs/internals/lxmf.md](docs/internals/lxmf.md) is the reach-inside
  view (upstream LXMF 0.9.8 summary, our architecture + deltas, full
  schema, phasing). The two together supersede the old
  `docs/plans/lxmf.md`.
- **gps** — [docs/gps.md](docs/gps.md). GNSS receiver task: dual-chip
  autobaud + model detection, `gps.*` fix snapshot, chip standby on
  disable, T-Deck Settings integration.
- **maps** — [docs/maps.md](docs/maps.md). On-device offline map
  viewer: worker/lcd-task split, SD RGB565 tile format, slippy-map
  math, and the desktop tile toolchain (`scripts/maketiles.py`).

Docs split convention: `docs/<component>.md` describes a component as
a black box for its consumers; `docs/internals/<component>.md` is what
you read only to reach inside it (upstream summary + how it works +
our deltas, complete enough to retire the planning doc).

Per-task **persistent / ephemeral / CLI / panel / status-window**
contracts are in [`docs/component-plan.md`](docs/component-plan.md)
§11–§14; the doc files above explain how the in-tree code implements
those contracts.


## Authoritative plan

**Read [`docs/component-plan.md`](docs/component-plan.md) first.** It is
the single source of truth for:

- Codebase choice (fork attermann upstream, not ratspeak — and why).
- µR replacements (crypto, JSON, msgpack, logging, file I/O, loop).
- Component dependencies (mbedTLS, **esp_wireguard for software
  Curve25519** — without it ECDH on opportunistic SINGLE packets is
  ~100 ms per scalar mult instead of <10 ms).
- Task layout, concurrency model, wake sources.
- LoRa specifics (RadioLib HAL, ISR rules, RNode framing).
- Networking (AutoInterface UDP via BSD sockets in the `auto` task —
  lwIP core locking is off, so the raw API isn't usable off the tcpip
  thread; TCP via net dial).
- ITS port surface.
- Storage conventions (RAM-only state, snapshot mirror, SoT layer,
  IFAC, persist cadence).
- Per-task storage keys, CLI verbs, panels, status windows.
- Phased rollout (Phase 1 = rnsd + tcp outbound, Phase 2 = auto
  (AutoInterface), Phase 3 = lora; that's the visibility milestone).

Other docs:

- [`docs/tdeck.md`](docs/tdeck.md) — the `tdeck` board module contract up
  top, then an exhaustive hardware reference for every T-Deck variant
  (original, Plus, Pro V1.0/V1.1/MAX) plus ratdeck firmware architecture
  notes. Cite this when writing drivers.
- [`docs/plans/`](docs/plans/) — older sketches kept for context.
  Where they disagree with `component-plan.md`, the plan wins.

## Reference codebases

Whole-tree clones of reference projects live at the workspace root in
[`../research/`](../research/) — one level above this repo, deliberately
outside any committed source tree (and excluded from `devtools/backup`).
Currently:

- [`../research/reticulum/`](../research/reticulum/) — Mark Qvist's
  canonical Reticulum (Python). Authoritative wire format / Transport
  behavior reference.
- (older `ratdeck/` and `microReticulum/` clones moved here too if you
  re-add them; `ratspeak/ratdeck` is AGPL-3.0 — read for architectural
  lessons, **never copy code from**; tdeck.md Part 2 has the digest.)

Add more clones as they become useful (markqvist/LXMF, RNode_Firmware_CE,
etc.). Don't commit them into any of the in-workspace repos.

## Conventions (reticulous-specific)

Everything platform-wide is in [`../spangap/CLAUDE.md`](../spangap/CLAUDE.md).
Reticulous adds:

- **Transports own their own lifecycle.** Each transport task
  watches `s.<name>.enable`, comes up on its own, and registers
  with rnsd via `RNSD_PORT_REGISTER`. Rnsd has zero compile-time
  knowledge of which transports exist. Adding a transport = a new
  task, no rnsd code changes.
- **Single wait point per task.** `itsPoll(nextDeadline())` is the
  only blocking call — wakes on ITS messages, task notifications
  (radio ISR, lwIP recv callback), or computed deadlines. Idle
  CPU = 0. No `while (itsPoll(0)) {}` polling drains.
- **rnsd is pure protocol.** No `lwip/`, no RadioLib, no socket
  primitives. It only sees RNS packets via ITS streams.
- **Storage as SoT for the durable layer**, ephemeral 1 Hz mirror
  for live state. Cron-driven `rnsd persist if-transport`. mR's
  own `OS::read_file/write_file` are no-op'd.
- **No TLS for transports** — RNS doesn't have it. IFAC is the
  per-interface access-control primitive (PSK + per-packet HMAC).

## Hardware

- LilyGo T-Deck Plus: ESP32-S3FN16R8 (16 MB flash, 8 MB octal
  PSRAM), SX1262 LoRa, 2.8" 320×240 ST7789V LCD, BlackBerry-style
  trackball, on-keyboard ESP32-C3 (I2C addr 0x55), GPS, microSD on
  shared FSPI. Full pin map in [`docs/tdeck.md §1.2`](docs/tdeck.md).
- LoRa pins: CS=9, DIO1=45, RST=17, BUSY=13. SPI shared with
  display + SD: SCK=40, MOSI=41, MISO=38. TCXO 1.8 V.
- LoRa antenna: PCB-trace on standard SKU; IPEX (u.FL) on
  external-antenna SKU. No SMA bulkhead exists.
- WiFi/BT antenna: PCB-trace on the ESP32-S3 module.
- Display + capacitive touch (GT911), QWERTY keyboard, centre button, and
  trackball (as a mouse pointer): all driven by the `lcd` LVGL UI
  (`CONFIG_SPANGAP_LCD`), fully interrupt-driven; board support in
  [`main/tdeck.cpp`](main/tdeck.cpp), wiring in [`docs/tdeck.md`](docs/tdeck.md) §1.8.
  Audio codec: not yet wired.
