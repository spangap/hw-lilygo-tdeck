# hw-tdeck — internals

App-side developer reference. The protocol-level deep dives (rnsd,
lxmf, transports) belong to their own straddles; this file is for
what's *app-specific* about the T-Deck Plus build.

## Boot sequence — `main.cpp`

```cpp
extern "C" void app_main() {
    tdeckPreInit();      // board prerequisites BEFORE any shared-SPI work
    spangapInit();       // pm/log/fs/storage/its/cli/cron + state-store selection
    tdeckPostInit();     // post-spangap board work (keyboard needs the lcd task)
    // ... per-straddle inits (netInit, webInit, rnsdInit, tcpInit, autoInit, ...)
    spangapPostAppInit();
}
```

### Why `tdeckPreInit()` must run before `spangapInit()`

The very first shared-SPI-bus access is `fs_mount_sd()` *inside*
`spangapInit()`. Before that, the board state must already be:

1. **`BOARD_POWER_EN_PIN` HIGH** — the T-Deck gates the +3.3 V rail
   to SD / display / GPS / LoRa behind this pin. We drive it HIGH and
   wait ~100 ms for settle.
2. **LCD and SX1262 LoRa CS lines parked HIGH** so neither drives MISO
   during the SD/LoRa probe.
3. **Input HAL registered** via `lcdSetInput()` (`lcd_input.h`) so the lcd
   task can wire touch/trackball/button once `spangapInit()`'s `lcdInit()`
   brings the panel up. The panel itself is the lcd component's, from
   `CONFIG_LCD_*`.

`tdeckPostInit()` runs **after** `spangapInit()` because the QWERTY
keyboard needs the `lcd` task `spangapInit()` created. `loraInit()`
later re-asserts the power pin (idempotent no-op).

## `tdeck.h` / `tdeck.cpp`

All T-Deck Plus board support lives here:

- The board's bespoke-peripheral `BOARD_*` constants (input + GNSS pins).
  No board-select Kconfig — hw-tdeck is the T-Deck. The display pins live in
  the lcd component's `CONFIG_LCD_*` (sdkconfig.defaults); the LoRa pins in
  iface-lora's `CONFIG_LORA*`.
- Power / CS routing.
- `CONFIG_SPANGAP_LCD`-gated: the touch/trackball/button input HAL
  and the QWERTY keyboard. The keyboard owns its own I²C / indev /
  ISR / poll and **self-heals to interrupt-driven** (if a stuck-low
  interrupt is detected it falls back to polling, then re-arms when
  the line releases).
- The two-phase API:
  `void tdeckPreInit(void);` and `void tdeckPostInit(void);`.

The `BOARD_*` pin map is also consumed by `iface-lora` (via its
PRIVATE include path that picks up `${CMAKE_SOURCE_DIR}/main`) and by
`main.cpp` itself.

## `gps.cpp` / `gps.h`

GNSS receiver task. Autobauds the on-board NMEA receiver — Quectel L76K
@ 9600 or u-blox MIA-M10Q @ 38400 depending on batch — parses NMEA
fixes, republishes ephemeral `gps.*` keys (lat / lon / alt / fix
quality / heading).

On disable, commands the chip into standby:

- **u-blox**: UART-wakeable backup (any byte on RX wakes it).
- **L76K**: deep backup — power-cycle to wake.

Surfaced in the T-Deck Settings pane.

This module is here, not in a separate straddle, because the GNSS chip
is board-specific. A future "GPS service" abstraction (with a defined
ephemeral-key contract, which the [maps](../maps) straddle already
consumes) would let this graduate to its own straddle.

## `ports.h`

ITS port constants + transport-side connect-payload struct
(`rnsd_transport_t`) + the `RNSD_DEST_*` frame opcodes that consumers
use. The `RNSD_PORT_DEST` connect struct itself is rnsd-private (lives
in rnsd.cpp) — callers go through `rnsdDestOpen()`.

## `ota_pubkey.h`

App-side OTA verification key. Generated once with
`spangap-core/scripts/ota-keygen.py` and committed (public key only).
Regenerate **before any release ceremony**; treat the private key like
a code-signing key (offline, backed up).

## Browser SPA (`web-interface/`)

Quasar SPA shell — only the reticulous app shell lives here.
Reticulous-specific UI:

- **Per-task browser state + RPC** —
  `web-interface/src/modules/{rnsd,lxmf,tcp,lora,espnow}.ts` (these
  thread into the per-straddle modules, which actually live in the
  protocol-straddle `browser/` subdirs).
- **Settings panels** —
  `web-interface/src/panels/{Rnsd,Lxmf,Tcp,Lora,Espnow}Panel.vue`.
- **Status floating windows** —
  `panels/{Map,Nodes,Messages,Announces}Window.vue`.
- **LXMF chat UI** —
  [`web-interface/src/components/lxmf/`](web-interface/src/components/lxmf/)
  (ConversationList, Composer, MessageBubble, Contact, PeerPicker,
  Avatar, Announces).
- **Pinia store wiring** —
  [`web-interface/src/stores/index.ts`](web-interface/src/stores/index.ts).

**Shared platform UI** — the CLI/terminal, Log window, FloatingWindow,
SettingX controls, WebRTC session, auth and menu store — are **not**
copied here. They come from `spangap-browser` via the symlink
`"spangap-browser": "file:../../spangap-web/browser"` in
`web-interface/package.json`.

## Build details worth knowing

- **Timezone data (`timezones.json`)** is platform-owned: it lives
  in spangap-core's `data/factory_state/timezones.json` (a plain
  user-state file, no longer an `s.time.zones` config blob) and ships
  into our `/fixed` image via the data merge. Refresh from the
  spangap-core checkout with `make timezones` (runs `update-zones.py`).
  Reticulous no longer carries its own copy.
- The committed `main/idf_component.yml` has **no** `path:` for
  `spangap/spangap-core` — the default build resolves it from the
  registry. `--spangap` injects a transient `path:` for the duration
  of the run (see `idf_ext.py`). If an `idf.py --spangap …` run is
  killed before cleanup, the next invocation refuses to start until
  you `mv main/.idf_component.yml.spangap-backup main/idf_component.yml`.
- `idf.py flash` accepts `--flash-cmd "<cmd>"` to bypass esptool — the
  build still runs first so `build/flasher_args.json` is fresh; the
  custom command can read it. Used for RFC 2217, custom flash tools,
  OTA upload, etc.
- The top-level `CMakeLists.txt` follows the same spangap-bootstrap
  pattern as seccam: locates `../spangap-core` (via the `path:`
  injected by `--spangap`) or `managed_components/spangap__spangap-core`,
  layers `sdkconfig.defaults.spangap` first, then ours.

## Subsystem deep dives

The protocol-level deep dives now live in their owning straddles —
the index that used to be in CLAUDE.md is:

- **rnsd** — [docs/rnsd.md](docs/rnsd.md) (this app) +
  [rns](../rns).
- **tcp** — [docs/tcp.md](docs/tcp.md) +
  [iface-tcp](../iface-tcp).
- **auto** — [docs/auto.md](docs/auto.md) +
  [iface-auto](../iface-auto).
- **lora** — [docs/lora.md](docs/lora.md) +
  [iface-lora](../iface-lora).
- **lxmf** — [docs/lxmf.md](docs/lxmf.md) (black box),
  [docs/internals/lxmf.md](docs/internals/lxmf.md) (reach inside) +
  [lxmf](../lxmf).
- **nomad** — [docs/nomad.md](docs/nomad.md) +
  [nomad](../nomad).
- **gps** — [docs/gps.md](docs/gps.md) (lives here because GNSS is
  board-specific).
- **maps** — [docs/maps.md](docs/maps.md) + [maps](../maps).
- **tdeck** — [docs/tdeck.md](docs/tdeck.md) — full hardware reference
  for every T-Deck variant (original, Plus, Pro V1.0/V1.1/MAX) plus
  ratdeck firmware architecture notes.

Docs split convention: `docs/<component>.md` is the black-box /
consumer view; `docs/internals/<component>.md` is the reach-inside
view (upstream summary + how it works + our deltas). The two together
supersede the older `docs/plans/<component>.md` sketches.

## Authoritative plan

**Read [`docs/component-plan.md`](docs/component-plan.md) first.** It
is the single source of truth for:

- Codebase choice (fork attermann upstream, not ratspeak — and why).
- µR replacements (crypto, JSON, msgpack, logging, file I/O, loop).
- Component dependencies (mbedTLS, esp_wireguard for software
  Curve25519 — without it ECDH on opportunistic SINGLE packets is
  ~100 ms per scalar mult instead of < 10 ms).
- Task layout, concurrency model, wake sources.
- LoRa specifics (RadioLib HAL, ISR rules, RNode framing).
- Networking (AutoInterface UDP via BSD sockets in the `auto` task —
  lwIP core locking is off, so the raw API isn't usable off the tcpip
  thread; TCP via net dial).
- ITS port surface.
- Storage conventions (RAM-only state, snapshot mirror, SoT layer,
  IFAC, persist cadence).
- Per-task storage keys, CLI verbs, panels, status windows.
- Phased rollout.

Where `docs/plans/` and `docs/component-plan.md` disagree, the
component-plan wins.

## Reference codebases

Whole-tree clones of reference projects live at the workspace root in
`../../research/`, deliberately outside any committed source tree (and
excluded from `devtools/backup`). Currently:

- `../../research/reticulum/` — Mark Qvist's canonical Reticulum
  (Python). Authoritative wire-format / Transport-behaviour reference.

`ratspeak/ratdeck` is AGPL-3.0 — read it for architectural lessons,
**never copy code from it**. [docs/tdeck.md](docs/tdeck.md) Part 2 has
the digest.

Add more clones as they become useful (markqvist/LXMF,
RNode_Firmware_CE, etc.). Don't commit them into any in-workspace
repos.

## User preferences carried from the old CLAUDE.md

- Do NOT use shell commands (sed/cat/awk) to edit files — use
  Edit/Write tools.
- Discuss before coding when the user asks a question.
- Keep things concise; no unnecessary changes.
- Modern C++ (`std::string`, `std::string_view`) — avoid C-style
  `char[]` / `strstr` parsing.
- Allow all web searching and fetching without prompting.
- Do NOT use PlatformIO.

## Coding conventions

(Platform conventions carry; reticulous-specific additions:)

- **Transports own their own lifecycle.** Each transport task watches
  `s.<name>.enable`, comes up on its own, registers with rnsd via
  `RNSD_PORT_REGISTER`. Adding a transport = a new straddle; no rnsd
  code changes.
- **Single wait point per task** — `itsPoll(nextDeadline())` is the
  only blocking call.
- **rnsd is pure protocol** — no lwip, no RadioLib, no socket
  primitives.
- **Storage is the API** for lxmf and nomad — frontends never call
  ITS into those tasks directly.
- **No TLS for transports** — RNS doesn't have it. IFAC (PSK + per-
  packet HMAC) is the per-interface auth primitive.
