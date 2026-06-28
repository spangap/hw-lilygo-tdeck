# reticulous

## What it is

[Reticulum](https://reticulum.network) is Mark Qvist's cryptography-based
networking stack for building resilient, self-configuring networks over
anything that can carry packets — LoRa, packet radio, plain TCP/IP, even a
serial wire. There is no central authority and no assigned addresses:
every node is a self-generated cryptographic identity, links are
end-to-end encrypted by default, and the network keeps routing over
cheap, low-bandwidth, intermittently-connected links where conventional
stacks fall over.

reticulous runs Reticulum on a **spangap** device. spangap is a dual-side
ESP32-S3 platform — a firmware component plus a paired browser package —
that hands an application its networking, web UI, storage, configuration,
logging, and OTA so the application only has to write its own domain code.
spangap is developed in parallel in the sibling tree; reticulous (and the
camera firmware, seccam) are its consumers and drive its API. Anything not
specific to Reticulum lives in [`../spangap/`](../spangap/) — read its
[`CLAUDE.md`](../spangap/CLAUDE.md) alongside this project.

reticulous itself is an ESP-IDF port of Reticulum onto spangap, built on a
hard fork of [`attermann/microReticulum`](https://github.com/attermann/microReticulum)
(a C++ embedded RNS implementation). The fork rewrites crypto against
mbedTLS plus software Curve25519, swaps ArduinoJson/MsgPack for
cJSON/msgpack-c, replaces `Serial.print*` with spangap's logging, and
removes µR's file I/O because spangap owns persistence. It is made
event-driven, with no top-level `Reticulum::loop()`.

## Status — work in progress

reticulous is **not finished**. It currently targets the LilyGo T-Deck
Plus (ESP32-S3 + SX1262 LoRa, 8 MB PSRAM, 16 MB flash). The board's screen,
touch, keyboard, and trackball drive an **on-device LVGL UI** — spangap's
`lcd` launcher with on-device Settings and an LXMessenger chat program
(board support in [`main/tdeck.cpp`](main/tdeck.cpp)); the device is also
driven from the spangap-browser SPA and the CLI.

What has been done so far:

- **Architecture locked.** The authoritative design and phased rollout
  live in [`docs/component-plan.md`](docs/component-plan.md): codebase
  choice and rationale, the µR replacements, the task layout and
  concurrency model, the ITS port surface, storage conventions, and the
  phase plan (Phase 1 = rnsd + outbound TCP, Phase 2 = AutoInterface,
  Phase 3 = LoRa — the visibility milestone).
- **µR hard fork in place** with the spangap-specific patches it needs to
  run multi-task safely: a RAII lock guard in `Transport::inbound` (the
  upstream code leaks `_jobs_locked` on several error exits), a recursive
  mutex over `Identity`'s known-destinations table so `recall` is safe
  across tasks, and a hex dump on the malformed-packet path for on-air
  debugging.
- **Task scaffold compiles.** One task per concern — `rnsd` (pure RNS
  protocol; zero radio/socket dependencies), `lxmf` (LXMF messaging; zero
  µR includes), and the `lora` / `tcp` / `auto` / `espnow` transport
  tasks. Each
  transport owns its own lifecycle and registers with rnsd, which has no
  compile-time knowledge of which transports exist.
- **rnsd client API and ITS port surface defined** — byte-array
  primitives plus typed connection openers, so downstream tasks operate
  on raw bytes and storage sentinels and never include mR types.
- **Browser panels planned** — settings under Settings → Reticulum /
  Transports, live state in FloatingWindow status windows.

The actual µR port and the per-task plumbing are the work remaining.

## Build

ESP-IDF 5.5.4 (installed at `~/.espressif/v5.5.4/esp-idf`).

```bash
idf.py build                              # also builds the web SPA + LittleFS image
idf.py -p /dev/tty.usbmodemNNNN flash monitor
```

The committed `main/idf_component.yml` resolves `spangap/spangap-core`
from the registry. To build against the sibling checkout instead, pass
`--spangap` — it injects a transient `path:` for the run and restores the
manifest on exit:

```bash
idf.py --spangap -p /dev/tty.usbmodemNNNN flash monitor
```

The T-Deck Plus uses native ESP32-S3 USB CDC; its serial port appears as
`/dev/tty.usbmodemNNNN` and varies by reset.

## Layout

- `main/` — firmware entry point and reticulous-specific tasks (rnsd,
  lxmf, lora/tcp/udp transports, board constants).
- `components/microreticulum/` — the µR hard fork.
- `web-interface/` — Quasar SPA built on `spangap-browser`.
- `docs/` — architecture (`component-plan.md`), the T-Deck board module +
  hardware reference (`tdeck.md`), per-subsystem deep dives, and prior plans.
- `../research/` — read-only clones of reference codebases (gitignored).

See [`CLAUDE.md`](CLAUDE.md) and
[`docs/component-plan.md`](docs/component-plan.md) before touching code.
