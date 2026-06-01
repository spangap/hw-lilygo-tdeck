# Reticulous — architecture and component plan

Authoritative plan for the first reticulous implementation. Builds on
[plans/ecosystem-overview.md](plans/ecosystem-overview.md) (what
Reticulum is and what code exists in the world) and
[plans/reticulous-early-thoughts.md](plans/reticulous-early-thoughts.md)
(an early sketch). Where those documents and this one disagree,
**this one wins**.

Two halves:

- **§1–§10 Architecture.** Scope, codebase fork, mR replacements, task
  layout, concurrency, wake sources, transports (LoRa / Networking),
  ITS ports, storage conventions.
- **§11–§16 Per-component plumbing + rollout.** For each component
  (rnsd, lora, auto, tcp, lxmf): persistent storage keys, ephemeral
  storage keys, CLI verbs, browser Settings panels, Status windows.
  Then phased rollout, menu hierarchy, non-goals, open questions.

We will refine this document as we keep talking.

---

## 1. Scope & non-goals

**Headline milestone.** Participate as an RNS endpoint over **LoRa**
and over outbound **TCP** to peer servers, with the browser able to
see other nodes and routes show up live as paths are learned.
**Display is ignored** — everything happens in the browser.

AutoInterface is included as a zero-config LAN transport (peer with a
Python `rnsd` on the same WiFi link with no addresses to configure;
useful for bring-up, lands second). LXMF is sketched here for
completeness but is not on the critical path for this milestone.

**Hardware target for the first build.** LilyGo T-Deck Plus (see
[tdeck.md](tdeck.md) for the board module + hardware reference). The Plus is the
working device; the same code should run on the original T-Deck
unchanged and on the Pro with a different pin map.

**LXMF v1.** Opportunistic-only delivery, on top of mR's existing
Destination + Packet primitives. No Link / Resource / propagation-
node support in v1 — those wait for upstream Link work or our own.

**Out of scope for v1:** LXST (license + early alpha), RNode-as-host
mode, propagation-node behavior, AGPL components, on-device chat
UI, TLS-on-TCP (RNS doesn't have it; IFAC is the access-control
primitive — see §10.1).

---

## 2. Codebase choice and license

**We hard-fork `attermann/microReticulum`** (Apache-2.0, the upstream)
into `components/microreticulum/`. **Pinned commit:**
`5642ae7fe17de6a8be9dc4891e95cf8a47c6ebe9` ("Restored fallback
heap-based path storage", 2026-05-09 — one commit past tag `0.3.1`).
Do not track upstream automatically.

**Why this specific commit and not the tag:** tag `0.3.1` (commit
`65e3a77`) hardcoded the path store to `microStore::BasicFileStore`.
The very next day `5642ae7` added an
`#if defined(RNS_USE_FS) && defined(RNS_PERSIST_PATHS)` guard so
that without those flags the path store falls back to
`microStore::BasicHeapStore`. We explicitly do NOT define those
flags (we no-op file I/O and own persistence — see §10.5), so the
heap-store fallback is essential. The 0.3.1 tag itself would not
build correctly for our configuration.

We do not start from `ratspeak/microReticulum` because:

- ratspeak's main delta vs upstream is `LXMFMessage`, the
  `FileSystem`/`FileStream` abstraction, AutoInterface, BZ2
  compression. **None of these are things we need**:
  - LXMF is its own task in our design, not part of mR.
  - We register no filesystem at all, so the abstraction layer is
    moot — easier to no-op the underlying `OS::read_file()` /
    `OS::write_file()` calls in upstream than to carry an
    abstraction we exclusively use to ignore.
  - AutoInterface and BZ2 we can cherry-pick later if/when needed.
- Upstream attermann is the authoritative source. Cleaner provenance,
  simpler cherry-pick story in either direction.

**Apache-2.0** permits derivative works. Our fork stays Apache-2.0
(or the spangap-wide license, whichever is selected per the
project's licensing decision). We keep ratspeak's source visible at
[reticulous/research/microReticulum/](../research/microReticulum/)
for selective reference.

**LXMF lives outside microReticulum.** ratspeak's `LXMFMessage` is
visible in our research clone as a permissively-licensed reference;
we write our own ~500 LOC LXMF implementation in a separate
component/task. The protocol surface is small and documented in the
Python reference (`markqvist/LXMF`).

**Reference repos cloned locally** at
[reticulous/research/](../research/) (gitignored):

- `ratdeck/` — `ratspeak/ratdeck` (AGPL-3.0, read-only reference for
  ideas, never copy code from)
- `microReticulum/` — `ratspeak/microReticulum` (Apache-2.0, fork
  reference; not the base we build from)

---

## 3. What we keep, replace, and remove from microReticulum

### Keep (mR's value, the protocol-correct guts)

- `Identity` — keypair generation, signing, verification, hash
  derivation.
- `Destination` — aspect tuples, hash truncation, types.
- `Packet` — header/encryption/decryption flow.
- `Announce` mechanism — flooding rules, dedup, hop tracking.
- `Transport` — path table maintenance, path requests, forwarding.
- `Interface` abstract base — subclassed by our `TaskInterface` proxy.
- Eventually: `Link`, `Resource`, `Channel`, `Buffer` — when ratspeak
  upstream lands them or we contribute them.

### Replace

| What | From | To | Why |
|---|---|---|---|
| Crypto backend | rweather/Crypto (hard-coded `#include <Curve25519.h>`, `<AES.h>`) | mbedTLS for AES/HMAC/HKDF/SHA, plus **vendored donna sources** for the curve crypto: `ed25519-donna` for Ed25519 sign/verify, re-vendored `x25519.c` (Mike Hamburg / strobe, MIT) for X25519 ECDH (see note below) | mbedTLS already in spangap build; software donna for ESP32 perf (ESP-IDF mbedTLS Curve25519 ~100 ms/scalar mult, software <10 ms); ed25519-donna provides the Ed25519 sign/verify path that mbedTLS doesn't ship |
| JSON | ArduinoJson | cJSON | already used by storage module |
| msgpack | MsgPack (Arduino lib) | msgpack-c | ESP-IDF-friendly, no Arduino dep |
| Logging | `Serial.print*` | `info()` / `warn()` / `err()` / `dbg()` / `verb()` macros | spangap convention |
| File I/O | `OS::read_file()` / `OS::write_file()` | **No-op** — return 0 / discard | We own persistence end-to-end via the CLI/cron path; mR's hourly autopersist becomes harmless |
| Top-level loop driver | `Reticulum::loop()` / `Transport::loop()` called every tick | Event-driven invocation of mR internals (announce arrived → `Transport::receive()`, timer expired → `expire_path()`, etc.) | Idle CPU goes to zero; matches spangap's event-driven pattern |
| Allocator hint | `RNS_DEFAULT_ALLOCATOR` (mR-specific config) | `heap_caps_malloc(MALLOC_CAP_SPIRAM)` for mR's bulk state; DRAM for anything touched from lwIP/ISR callbacks | Avoid heap fragmentation from many small RNS allocations |

### Remove

- All Arduino `#include`s and any `Arduino.h` dependency.
- `Wire`, `SPI`, `attachInterrupt` shim usage (Arduino-only).
- Any direct flash / SD writes — we own all persistence.
- `rweather/Crypto` dependency entirely.

### Component dependencies

`components/microreticulum/` declares (in its `idf_component.yml` /
`CMakeLists.txt`) a hard dependency on:

- **mbedTLS** — for AES-CBC, HMAC-SHA256, HKDF, SHA-256/512, Fernet
  primitives, RNG (via `mbedtls_ctr_drbg` or `esp_fill_random`).
  Already linked by spangap for TLS and WG.
- **vendored crypto sources under `components/microreticulum/src/donna/`** —
  this turned out cleaner than depending on spangap-core exporting its
  esp_wireguard symbols (which live under `PRIV_INCLUDE_DIRS`). Two pieces:
  - **ed25519-donna** (Andrew M. <liquidsun@gmail.com>, public domain) —
    Ed25519 sign/verify and key derivation. Wired with custom hooks for
    SHA-512 (mbedTLS) and RNG (`esp_fill_random`); compiled with
    `ED25519_CUSTOMHASH`, `ED25519_CUSTOMRANDOM`, `ED25519_FORCE_32BIT`,
    `ED25519_NO_INLINE_ASM`. This is the Ed25519 source the rest of the
    plan was missing — esp_wireguard ships only X25519, never Ed25519
    (WG doesn't use Ed25519).
  - **`x25519.c`** (Mike Hamburg / Cryptography Research, MIT) —
    re-vendored from `spangap-core/src/esp_wireguard/crypto/refc/x25519.c`.
    Provides full X25519 scalar multiplication for ECDH (donna's
    public API exposes only `curved25519_scalarmult_basepoint`, which is
    not enough for the ECDH operation `out = scalar * peerpoint`).
  Re-vendoring decouples us from spangap-core's internal layout and
  avoids the symbol-export coupling. ESP-IDF mbedTLS's `ECP_DP_CURVE25519`
  is ~100 ms/scalar mult; software donna is <10 ms — required for
  per-packet ECDH on opportunistic SINGLE packets.

---

## 4. Task layout

Following spangap's "task per concern" pattern, with one important
rule: **transports own their own lifecycle, not rnsd.** Each
transport is an independent task that watches its own config
subtree, comes up on its own, and registers itself with rnsd via
ITS. Rnsd has zero compile-time knowledge of which transports
exist; it reacts to whatever connects.

| Task | Core | Prio | Stack | Owns |
|---|---|---|---|---|
| `rnsd` | 0 | 2 | 12 KB PSRAM | RNS internals: identity, destinations, path table, transport, links, resources. **Zero networking/radio dependencies.** |
| `lxmf` | 1 | 1 | 8 KB PSRAM | LXMF protocol: inbox, contacts, dedup, send queue |
| `lora` | 0 | 2 | ~6 KB PSRAM (DRAM frame buffers) | SX1262 driver, RNode on-air framing/split-reassembly, DIO1 ISR target |
| `auto` | 0 | 2 | ~6 KB PSRAM (+ `auto-rx` 4 KB) | AutoInterface: IPv6 link-local multicast peer discovery + unicast UDP data. BSD sockets (lwIP core locking is off, so the raw API can't be driven off the tcpip thread); an `auto-rx` helper task `select()`s the sockets and notifies. |
| `tcp` | 0 | 2 | ~4 KB PSRAM | Both outbound (dial via net) and inbound (listen via net). Each connection registers as its own iface. HDLC byte-stuffing on the wire. |
| (future) `lxst`, `nomadnet`, etc. | 1 | 1 | per-task | Each higher-level RNS protocol gets its own task |

**Lifecycle.** Each transport task starts on boot from a module-init
list, watches `s.<name>.enable` and related config keys, and:

- On enable: `itsConnect("rnsd", RNSD_PORT_TRANSPORT, payload, ...)`
  with payload describing itself (name, MTU, bitrate, mode,
  capabilities). The connect handle is the bidirectional packet
  stream — inbound packets to rnsd, outbound packets from rnsd.
- On disable / crash: disconnects (or net does for it). Rnsd's
  disconnect handler removes the proxy; mR `deregister_interface`s.

**rnsd never knows what transports exist at compile time.** Adding a
new transport = a new task in the build, no rnsd code changes.

PSRAM-stack implication: no task touches SPI flash directly. All
flash I/O goes through the `fs` worker.

`rnsd` and the transport tasks are pinned to core 0 alongside
`tcpip_thread`; this keeps lwIP recv callbacks and task-switch
latency on the same core. Other consumers and UI live on core 1.

---

## 5. Concurrency model — fully event-driven, single wait point per task

Every task in this subsystem follows the same shape: block in
`itsPoll(timeout)`, wake on either a notification or the timeout,
service whatever needs servicing, recompute the next deadline,
sleep again. **Idle CPU per task = 0.**

`itsPoll` is `ulTaskNotifyTake(pdTRUE, timeout)` —
[its.cpp:555](../../spangap/spangap-core/src/its.cpp#L555) — so any
`xTaskNotifyGive` (or `xTaskNotifyGiveFromISR`) wakes it. ITS
internals already notify on inbox traffic.

### 5.1 rnsd loop

```
for (;;) {
    itsPoll(nextDeadline());      // block until ITS event,
                                  // task notification, or deadline.
                                  // Returns immediately if work pending.

    serviceExpiredTimers();       // path expiry, link handshake
                                  // timeouts, resource RTOs,
                                  // snapshot tick, gracious-persist

    publishSnapshotsIfDue();      // 1 Hz path/link/stats diff into
                                  // ephemeral storage
}
```

Per loop iteration: one ITS callback dispatched (if pending) plus
housekeeping. Multiple pending events drain naturally across
iterations — itsPoll returns immediately when work is available.
Idle iterations cost nothing; the task just blocks longer inside
itsPoll. `nextDeadline()` returns the smallest of (next pending
timer − now), or `portMAX_DELAY` if no timers pending.

`Reticulum::loop()` and `Transport::loop()` are not called at the
top level. Their internal responsibilities — `clean_caches()` (15
min cadence), path aging, expired-link cleanup, time_offset
bookkeeping — become explicit entries in our timer set.

### 5.2 Per-transport task loop

```
for (;;) {
    itsPoll(nextDeadline());      // block on ITS, ISR notification,
                                  // lwIP callback notification, or
                                  // deadline

    drainHardwareIfReady();       // checks volatile flags set by ISR
                                  // (radio) / rx-queue notify (auto) /
                                  // etc. and drains accordingly

    framePendingTxIfReady();      // radio: split + schedule;
                                  // auto: sendto() to each peer;
                                  // tcp: tcp_write/output

    publishIfaceStatsIfDue();     // 1 Hz tx/rx counters, bitrate,
                                  // mode → ephemeral storage
}
```

Same shape as rnsd. Hardware events arrive as task notifications
(not ITS callbacks), so itsPoll returns false-but-woken; the
drainHardware step inspects the volatile flags the ISR / lwIP
callback set. Outbound packets from rnsd arrive as ITS messages
and are dispatched by itsPoll itself.

Rnsd sees only ITS streams of RNS-format packets.

---

## 6. Wake sources

| Task | Source | Mechanism |
|---|---|---|
| `rnsd` | Transport registers/deregisters | ITS connect/disconnect handler (existing plumbing) |
| `rnsd` | Inbound RNS packet on a registered transport | ITS packet-mode message on the handle (existing plumbing) |
| `rnsd` | Browser/CLI/lxmf control | ITS aux/stream (existing plumbing) |
| `rnsd` | Snapshot tick / cleanup tick / persist-soft-due | Computed timer deadline in `itsPoll(timeout)` |
| `lora` | DIO1 ISR | `vTaskNotifyGiveFromISR(loraTask, ...)` from IRAM_ATTR ISR registered via `radio.setPacketReceivedAction(...)` |
| `lora` | TX-done ISR (optional) | Same mechanism via `setPacketSentAction(...)` |
| `lora` | Outbound packet from rnsd | ITS packet-mode message |
| `lora` | Split-RX timeout, duty-cycle timer | Computed deadline |
| `auto` | UDP RX (discovery + data) | `auto-rx` helper task `select()`s the 3 sockets, `recvfrom`s, copies into a PSRAM queue + `xTaskNotifyGive(autoTask)` |
| `auto` | Announce / peer-job cadence | Computed deadline (1.6 s announce, 4 s peer job) |
| `auto` | Outbound packet from rnsd | ITS packet-mode message |
| `tcp` | Inbound listen accept / dial result / per-conn bytes | Net's existing ITS forward + new dial-on-behalf-of API; each delivered as ITS messages |
| `tcp` | Outbound packet from rnsd | ITS packet-mode message |
| `tcp` | Per-peer reconnect backoff | Computed deadline |
| any | Config change | `storageSubscribeChanges` callback → `xTaskNotifyGive(self)` |

Computing the next deadline is one-line: `timeout = (next_due > now) ?
min(next_due - now, MAX_TIMEOUT) : 0`. Past-due timers get
serviced immediately; idle gets `portMAX_DELAY`.

---

## 7. LoRa (`lora` task): RadioLib + our ISR + RNode framing

Radio lives entirely inside the `lora` task. Rnsd never includes
RadioLib.

**Library.** `jgromes/RadioLib` as an ESP-IDF managed component
(also on the Espressif Component Registry). No Arduino. RadioLib has
a `RadioLibHal` abstract class — we implement it in ~150 LOC of
ESP-IDF glue (`spi_device_polling_transmit`, `gpio_set_level`,
`vTaskDelay`, etc.).

**HAL.** Custom `EspIdfHal : public RadioLibHal` rather than the
example one in `RadioLib/examples/NonArduino/Esp-idf/`, so SPI shares
whatever bus management we set up for display/SD. Pin mapping comes
from the T-Deck Plus pinout in [tdeck.md §1.2](tdeck.md):
CS=9, DIO1=45, RST=17, BUSY=13, SCK=40, MOSI=41, MISO=38.

**TCXO.** 1.8 V on T-Deck Plus (per LilyGo `utilities.h`). Verify
on hardware — wrong voltage = ~30 kHz frequency error.

**ISR ownership.** RadioLib provides the wiring (calls into our
HAL's `attachInterrupt` to register the function pointer); we
provide the ISR body, targeting the `lora` task:

```cpp
static IRAM_ATTR void loraRadioIsr(void) {
    BaseType_t hpTaskWoken = pdFALSE;
    vTaskNotifyGiveFromISR(loraTaskHandle, &hpTaskWoken);
    portYIELD_FROM_ISR(hpTaskWoken);
}
```

**ISR rules:**

- `IRAM_ATTR` mandatory (ISR may fire while SPI flash is being
  accessed).
- No SPI from the ISR. Task-side reads `getIrqStatus()`, drains
  FIFO via `readData()`, clears IRQ status, re-arms with
  `startReceive()`.
- DIO1 configured RISING edge. SX126x keeps DIO1 high until IRQ
  cleared; level-trigger would re-fire continuously.
- Drain pattern: `while (irqs = getIrqStatus()) { clear(irqs);
  dispatch(irqs); }`. Handles read-vs-clear race.

**Unmasked IRQs** (default): `RxDone`, `TxDone`, `Timeout`. Others
masked unless we have a specific need (CrcErr for stats, etc.).

### 7.1 RNode on-air framing (mandatory for interop)

The SX126x physical layer caps each LoRa frame at **255 bytes**
(8-bit length register). Reticulum's protocol MTU is **500 bytes**.
So full-sized RNS packets must be split across multiple LoRa frames.

This is not a ratdeck-specific quirk. It's the **RNode on-air framing
protocol**, defined in `RNode_Firmware/Framing.h` / `Config.h`. Any
device speaking RNS-over-LoRa to RNode-equipped peers (RNodes,
T-Beams, other Reticulum-on-LoRa firmware) must use this framing;
it is the price of admission to the RNS-on-LoRa ecosystem.

**Frame format:**

```
[ 1 byte: header ][ up to 254 bytes: payload ]

header byte:
  upper nibble (0xF0) = random sequence id
  lower nibble (0x0F) = flags
    bit 0 (0x01) = RNODE_FLAG_SPLIT — "this is part of a split packet,
                                       expect another frame with the
                                       same sequence nibble"
```

- Single small RNS packet (≤254 B) → one LoRa frame, SPLIT=0.
- Large RNS packet (255–508 B) → two LoRa frames, both with same
  random seq nibble, both SPLIT=1, payloads concatenated by the
  receiver.
- Random seq nibble (16 values) disambiguates interleaved splits
  from different senders. Reassembly timeout: 5 s (per ratdeck).

**Half-duplex consequence.** While waiting for the second half of a
split RX, the radio task must not TX (transitioning into TX would
lose the second half). Guarded by a `_splitRxPending` flag.

All split/reassemble logic lives inside the `lora` task. Rnsd hands
the lora task a 0–500 byte RNS packet via ITS, lora task decides
whether that's one or two on-air frames.

---

## 8. Networking

### 8.1 AutoInterface — `auto` task, BSD sockets

The LAN transport is **AutoInterface**, not a hand-configured UDP
interface: a node finds every reachable RNS peer on the same WiFi
link automatically, with no host/port to set. It is wire-compatible
with upstream RNS `AutoInterface` (`RNS/Interfaces/AutoInterface.py`),
so a desktop Reticulum on the LAN peers with the device out of the box.

**Why BSD sockets, not the raw API.** The original plan was to own a
lwIP `udp_pcb` and use the per-PCB `udp_recv` callback. That requires
driving the raw API from the owning task — only safe with lwIP **core
locking**, which is **off** in this build (`CONFIG_LWIP_TCPIP_CORE_LOCKING`
unset). BSD sockets are task-safe and give us IPv6 multicast join
(`IPV6_ADD_MEMBERSHIP`), zoned link-local sends (`sin6_scope_id`), and
`inet_ntop` text that matches Python's address strings (the peering
hash is computed over the address text, so the formatting must agree).

**Protocol** (defaults all interoperable):

- group name `"reticulum"` → `group_hash = SHA-256(name)`. The IPv6
  multicast discovery address is `ff{flags}{scope}:` + six hextets of
  `group_hash[2..13]`; default flags=temporary(1), scope=link(2) →
  `ff12:0:…`. Every node joins it on the active netif.
- peering token = `SHA-256(name || link_local_addr_text)`. Each node
  multicasts its token to the group on **29716** every 1.6 s; a token
  from src S that recomputes to `SHA-256(name || text(S))` adds/refreshes
  S as a peer (a token from our own address is the multicast echo).
- reverse peering: also unicast the token to known peers on **29717**,
  so asymmetric multicast still peers both ways.
- data: an outbound RNS packet is unicast (one UDP datagram) to every
  live peer on **42671**; inbound datagrams from a known peer go to rnsd.

**Threading.** `recvfrom` has to block somewhere, but the `auto` task's
single wait point is `itsPoll`. A tiny `auto-rx` helper task `select()`s
the three UDP sockets, `recvfrom`s, copies each datagram into a PSRAM
queue, and `xTaskNotifyGive(autoTask)` — exactly the espnow shape (recv
on another context → queue → notify → `itsPoll` wakes). All sends, all
peer/timer state, and all ITS live on the `auto` task. The link-local
IPv6 address is brought up on the active netif with
`esp_netif_create_ip6_linklocal`; rnsd includes no networking headers.

### 8.2 TCP — net handles dial and listen, forwards as ITS

Net keeps its existing inbound TCP pattern (registers listen ports,
accepts, forwards as ITS). We **add an outbound dial API**:

```cpp
itsConnect("net", NET_PORT_TCP_DIAL, "host:port", ..., timeoutMs);
```

Net's connect handler reads the payload, does the connect on its
own task, returns success/failure as the ITS connect result. The
ITS handle IS the TCP stream from byte zero. DNS, retry, timeout
handling all live in net.

A single `tcp` task handles both directions — outbound dials per
`s.tcp.peers.*`, inbound accepts on `s.tcp.server_*`. Each
connection (dialed or accepted) registers as its own iface with
rnsd via `RNSD_PORT_TRANSPORT`. Two ITS hops per packet (net ↔ tcp
↔ rnsd) — fine for RNS rates.

**Wire format.** HDLC byte stuffing per the upstream Python
`TCPInterface.py:44`:

```
FLAG = 0x7E    ESC = 0x7D    ESC_MASK = 0x20

emit:    FLAG <escaped(packet_bytes)> FLAG
escape:  0x7D → 0x7D 0x5D
         0x7E → 0x7D 0x5E
decode:  scan for FLAG; inside, on 0x7D, XOR next byte with 0x20
```

Inside each HDLC frame is the same RNS packet that goes on UDP or
LoRa — TCP only adds frame delimitation. Different from KISS framing
on serial (which uses 0xC0 / 0xDB).

For now, **net keeps polling internally** — the lwIP-raw refactor of
net (which would eliminate net's polling for everyone) is out of
scope for the first reticulous build. Revisit when the rest is
stable.

### 8.3 No webrtc refactor

Webrtc keeps its existing direct-socket pattern with 10 ms polling.
Refactoring webrtc to lwIP raw or to net-owned UDP is out of scope
— the latency/CPU cost of net-owned UDP at DTLS rates would be
real, and the lwIP-raw path is the same architectural change for
webrtc as for net.

---

## 9. ITS port surface

Packet-mode by default for everything RNS-adjacent; aux for small
fire-and-forget messages under the ITS aux cap.

### Rnsd ports

| Port | Label | Mode | Purpose |
|---|---|---|---|
| `RNSD_PORT_TRANSPORT = 1` | (internal) | bi packet stream | **Transport registration.** Connect payload describes the interface (name, MTU, bitrate, mode, capabilities). Stream then carries inbound RNS packets to rnsd and outbound RNS packets from rnsd. Disconnect = deregister. |
| `RNSD_PORT_MAP = 2` | `rnsd_map:1` | bi packet stream | Browser network-map DC: announce / path / link / iface events |
| `RNSD_PORT_CTL = 3` | `rnsd_ctl:1` | bi packet stream | Control: list dests, force announce, rotate identity, import/export |
| `RNSD_PORT_DEST = 4` | (internal) | bi packet stream | Destination registration for protocol consumers (lxmf, etc.): connect with destination hash; thereafter receives inbound packets for that dest, sends outbound from it |
| `RNSD_PORT_DGRAM = 5` | (internal) | aux + stream fallback | Datagram send. Aux for small (≤ITS aux cap), stream for larger payloads |
| `RNSD_PORT_LINK = 10` | (v2) | bi packet stream | Generic Link socket — opens an RNS Link, ITS bytes flow over Channel/Buffer. Requires Link upstream (deferred) |

### LXMF ports

| Port | Label | Mode | Purpose |
|---|---|---|---|
| `LXMF_PORT_CHAT = 1` | `rns_chat:1` | bi packet stream | Browser chat DC: `{send: …}` / `{recv: …}` JSON per message |
| `LXMF_PORT_API = 2` | (internal) | bi packet stream | Task-to-task LXMF API for other modules |

### Net ports (additions for reticulous)

| Port | Label | Mode | Purpose |
|---|---|---|---|
| `NET_PORT_TCP_DIAL` | (internal) | bi packet stream | Outbound TCP dial. Connect payload = `host:port` ASCII. Handle is the TCP stream from byte zero. |

The webrtc DC label-router dispatches `rns_chat:1` directly to
`lxmf`, not via rnsd. Same for any future protocol-specific browser
DC.

---

## 10. Storage conventions

### 10.1 Naming and lifetime

- `s.<task>.*` — persistent settings, owned by the named task.
- `secrets.<task>.*` — persistent secret material (private keys,
  PSKs), owned by the named task. Wiped by factory reset.
- `<task>.*` — ephemeral runtime state, RAM-only, published by the
  task at a fixed cadence (1 Hz for paths/links, 0.2–1 Hz for
  stats). `storageSet` dedupes equal-value writes itself, so the
  per-task diff (compare-against-last-published) is purely a
  performance optimization — avoids the serialize / hash / lookup
  work on unchanged keys, which matters at 256 paths × 5 fields ×
  1 Hz.
- Numeric-keyed objects (`peers.0.host`) for arrays — the JSON-array
  merge hazard.

Browser subscribes to `<task>.*` for live state and `s.<task>.*`
for settings; renders a panel per task and a status floating window
per task.

### 10.2 RAM authoritative for protocol-machine state, never persist

- Hashlist (packet dedup, replay/loop suppression).
- Link state machines: per-link RTT, sequence numbers, retx,
  half-open handshake state.
- Ratchet ephemerals, in-flight X25519 secrets, derived AES/HMAC
  keys, Fernet contexts.
- In-flight Resource segments and reassembly buffers.

Sensitive material (ratchets, ephemerals, derived keys) **must
never touch storage** even briefly — credibility cost if a debug
dump includes them.

### 10.3 RAM authoritative + 1 Hz snapshot mirror in storage

- **Path table** — mR's native `unordered_map`-style storage in RAM
  (hot path is `dest_hash → next_hop` lookup per received packet).
  rnsd snapshots the table to ephemeral `rnsd.paths.*` storage at
  1 Hz, diff-published.
- **Link state summary** — each open link snapshotted at 1 Hz to
  `rnsd.links.*` (visible state only: peer, rtt, rssi, hops; not
  ratchet contexts or seq numbers).
- **Stats** — published per task at its own cadence:
  - `rnsd.stats.*` — protocol-level counters at 0.2–1 Hz.
  - `<iface>.stats.*` — per-interface tx/rx, bitrate, mode, by the
    transport task that owns it, at 1 Hz.

### 10.4 Storage as SoT for the durable, low-frequency layer

- **Identity** — private key (X25519 + Ed25519) in
  `secrets.rnsd.identity`. Rare access, rare write.
- **Destination registrations** — our own SINGLE/PLAIN destinations
  and their aspect tuples in `s.rnsd.destinations.*`.
- **Known-destinations cache** — identity-hash → public keys + app
  data, in `s.rnsd.known_destinations.*`. Persistent (matters across
  reboots — without it every cold boot re-does X25519 announce-
  discovery for every contact).
- **Per-task config** — owned by each task in its own subtree
  (`s.lora.*`, `s.auto.*`, `s.tcp.*`, `s.lxmf.*`). Rnsd has no
  knowledge of these; the transport tasks watch them.
- **Contacts** (LXMF dest hash → nickname, last seen, RSSI) — owned
  by the lxmf task in `s.lxmf.contacts.*`.

### 10.5 mR's own persistence is disabled

We no-op `OS::read_file()` / `OS::write_file()` in our fork of mR.
All mR persist calls (hourly auto-save of `destination_table`,
`packet_hashlist`, `known_destinations`, `tunnels`, `time_offset`)
become silent no-ops. rnsd owns persistence end-to-end via the
CLI/cron path.

### 10.6 Persistence cadence (CLI + cron driven)

- **Snapshots to ephemeral storage:** 1 Hz path/links, 0.2–1 Hz
  stats (per-task). Diff against last published; only changed keys
  call `storageSet`.
- **Flash persist:** cron-driven via CLI:
  ```
  0 * * * * N rnsd persist if-transport
  ```
  No-op for endpoint mode; transport mode writes paths + hashlist
  + tunnels.
- **Identity:** one-shot writes on creation/import, never on cadence.
- **Known-destinations:** event-persisted (on cull / on add), not
  on cadence.
- **Total persistent footprint:** ~60–80 KB for endpoints (matches
  mR's design assumption).

`rnsd persist` reads from in-RAM authoritative state, not from the
ephemeral mirror. The mirror is for browser/CLI introspection;
persist reads the source.

### 10.7 IFAC — Interface Access Code

Several transport configs include `ifac_netname` + `ifac_netkey` +
`ifac_size`. IFAC is RNS's per-interface authentication primitive
(see `Packet.cpp` / `Interface.h` in mR for the flag bit and field
layout). It's a pre-shared key per interface; both ends derive HMAC
keys from it and verify a per-packet authentication code. Wrong key
→ packet rejected at the interface layer.

**RNS does not have TLS**, in the protocol or the Python reference.
IFAC is the access-control primitive; for full traffic obfuscation
upstream's answer is `i2p_tunneled = True`. Neither TLS nor I2P is
on our roadmap.

Convention in this plan:

- `s.<iface>.ifac_netname` — public network name (string), part of
  the IFAC HMAC derivation. Joining a named IFAC network requires
  matching netname + netkey on both ends.
- `secrets.<iface>.ifac_netkey` — pre-shared key bytes (hex). Empty
  string ⇒ IFAC disabled on this interface (the default).
- `s.<iface>.ifac_size` — bytes (default 16, range 1..512).

For per-peer TCP, IFAC is per-peer (each `s.tcp.peers.<id>` has its
own IFAC fields). For LoRa and AutoInterface, IFAC is the single-iface
global (for `auto`, keyed on the group name).

---

## 11. `rnsd` — RNS protocol task

### 11.1 Persistent storage

```
secrets.rnsd.identity            # 64-byte private key (X25519+Ed25519),
                                 #   hex. Auto-generated on first boot;
                                 #   `rnsd identity import <file>` to
                                 #   replace.

s.rnsd.enable                    # 0|1 — task on/off
s.rnsd.transport_enabled         # 0|1 — full transport node vs endpoint
s.rnsd.name                      # human handle for the node (optional)
s.rnsd.announce.interval         # seconds between opportunistic
                                 #   announces — global default;
                                 #   each interface can override
                                 #   (s.<iface>.announce.interval)
                                 #   and TCP can override per peer
                                 #   (s.tcp.peers.<id>.announce.interval).
                                 #   0 = on demand only.
s.rnsd.path.max                  # path table capacity (default 256)
s.rnsd.path.ttl                  # seconds before unrenewed paths
                                 #   expire (default 86400)

s.rnsd.destinations.0.aspect     # our own destinations (numeric-keyed)
s.rnsd.destinations.0.type       # SINGLE | PLAIN

s.rnsd.known_destinations.0.hash # identity-hash → pubkeys + app data
s.rnsd.known_destinations.0.pub  #   (cache that survives reboot;
s.rnsd.known_destinations.0.app  #    written by `rnsd persist`)
```

### 11.2 Ephemeral storage

```
rnsd.up                          # 1 when fully initialized
rnsd.identity_hash               # our destination hash (hex)

# 1 Hz diff-snapshot of mR's RAM-authoritative path table:
rnsd.paths.<hash>.next_hop       # interface name (e.g. "lora",
                                 #   "tcp/0", "tcp_in/1.2.3.4:51234")
rnsd.paths.<hash>.next_hop_addr  # immediate neighbor's identity hash
                                 #   on that iface — distinguishes
                                 #   neighbors when one iface carries
                                 #   multiple peers (e.g. tcp_server,
                                 #   LoRa with several heard nodes)
rnsd.paths.<hash>.hops
rnsd.paths.<hash>.last_announce  # epoch seconds
rnsd.paths.<hash>.app_data       # decoded human handle if announce
                                 #   carries one

# 1 Hz diff-snapshot of open links (visible state only — no crypto):
rnsd.links.<hash>.iface
rnsd.links.<hash>.rtt            # ms
rnsd.links.<hash>.rssi           # dBm (if iface reports)
rnsd.links.<hash>.hops
rnsd.links.<hash>.opened         # epoch seconds

# 0.2–1 Hz stats:
rnsd.stats.packets_in
rnsd.stats.packets_out
rnsd.stats.announces_in
rnsd.stats.announces_out
rnsd.stats.bytes_in
rnsd.stats.bytes_out
rnsd.stats.paths_known
rnsd.stats.links_open

# Set on transport (de)registration:
rnsd.ifaces.<name>.up            # 1|0 — registered transport task
rnsd.ifaces.<name>.mtu
rnsd.ifaces.<name>.bitrate
rnsd.ifaces.<name>.mode          # full|gateway|access_point|roaming|boundary
```

### 11.3 CLI

Top-level command `rnsd`:

```
rnsd                      # status: up/down, identity hash, #paths,
                          #         #links, registered ifaces
rnsd up | down            # set s.rnsd.enable and react
rnsd persist              # write durable state to flash via fs worker.
                          #   Identity is one-shot on creation/import,
                          #   not on cadence. known_destinations is
                          #   persisted on cull / on add (event-driven,
                          #   not cron). This verb writes whatever
                          #   transport-mode state is dirty in RAM
                          #   (paths, hashlist, tunnels).
rnsd persist if-transport # same as `rnsd persist` but no-op when
                          #   s.rnsd.transport_enabled = 0. Used in
                          #   the cron line so endpoints don't pay
                          #   the hourly write.
rnsd reload               # re-read durable state from flash
rnsd announce [aspect]    # force announce
rnsd paths                # dump path table (RAM authoritative);
                          #   shows iface AND neighbor hash per row
rnsd peers                # dump known nodes with hops/RTT
rnsd ifaces               # list registered transport interfaces
rnsd identity             # show identity hash + public keys
rnsd identity rotate      # destructive — generate new identity
rnsd identity import <file>
rnsd stats                # tx/rx packets, bytes, announces
```

Cron line for persistence (no-op for endpoint mode):

```
0 * * * * N rnsd persist if-transport
```

### 11.4 Web panel — `RnsdPanel.vue`

Lives at **Settings → Reticulum**. Registered via `registerRnsd()`
in `web-interface/src/boot/modules.ts`. This is the top-level panel
for the protocol task — identity, transport-vs-endpoint mode,
announce policy, path table sizing. Per-transport configuration
lives under **Settings → Transports** (see §12.4 / §13.4 / §14.5).

**Section: Identity**

- Read-only display of `rnsd.identity_hash` (with copy-to-clipboard).
- Read-only display of public keys (X25519 + Ed25519 pubkeys),
  collapsible.
- "Force announce" button — writes a control message via
  `RNSD_PORT_CTL`.
- "Export identity" button → downloads `secrets.rnsd.identity` as a
  file, with a "back this up" warning.
- "Import identity" button → file upload, replaces current identity
  (with confirm modal).
- "Rotate identity" button (with confirm modal) — destructive;
  generates fresh keys, drops all known_destinations.

**Section: Configuration**

- `SettingToggle` → `s.rnsd.enable`
- `SettingToggle` → `s.rnsd.transport_enabled` (label: "Act as
  transport node — forward packets for others")
- `SettingText` → `s.rnsd.name`
- `SettingSlider` → `s.rnsd.announce.interval` (0 / 600 / 1800 /
  3600 / 21600 seconds, with "On demand only" at 0). This is the
  global default; per-iface and per-peer overrides live in their
  own panels.
- `SettingSlider` → `s.rnsd.path.max` (64 / 128 / 256 / 512)
- `SettingSlider` → `s.rnsd.path.ttl` (3600 / 21600 / 86400 / 604800)

Live status (counters, registered interfaces, paths known, links
open) is **not** in the Settings panel — it lives in the
Status / Reticulum subwindow (§11.6) and the Status / Map
subwindow (§11.5).

### 11.5 Status / Map — `MapWindow.vue`

Lives in a `FloatingWindow` (same component used by log and CLI),
opened from the **Status → Map** menu item. Subscribes to
`rnsd.paths.*` and `rnsd.ifaces.*` and renders a live table:

| Destination | Handle | Iface | Neighbor | Hops | Last announce | App data |
|---|---|---|---|---|---|---|

The **Iface** column shows the registered iface name (`tcp/0`,
`tcp_in/1.2.3.4:51234`, `lora`, `auto`). The **Neighbor** column
shows the directly-connected peer's identity hash on that iface —
matters when a single iface (LoRa, tcp_server) carries traffic for
multiple peers; identifies which physical hop the path goes through.

Sort by hops ascending, then by last_announce descending. New
entries fade in; stale entries (no announce in TTL) fade out.
Clicking a row shows the full hash and a per-path detail card.

A graph view (d3-force) can come later; the table is enough for the
visibility milestone.

### 11.6 Status / Reticulum — `RnsdStatusWindow.vue`

`FloatingWindow` opened from **Status → Reticulum**. Live overview:

- Identity: hash, public keys (read-only).
- Counters from `rnsd.stats.*`: packets in/out, announces in/out,
  bytes in/out, paths known, links open.
- Registered interfaces list from `rnsd.ifaces.*` — name, mode, MTU,
  bitrate, up/down indicator. Click an iface row to open its own
  Status window.

---

## 12. `lora` — LoRa transport task

### 12.1 Persistent storage

```
s.lora.enable                    # 0|1
s.lora.frequency                 # Hz (e.g. 868100000)
s.lora.bandwidth                 # Hz (125000 / 250000 / 500000)
s.lora.spreading_factor          # 7..12
s.lora.coding_rate               # 5..8 (4/5 .. 4/8)
s.lora.tx_power                  # dBm (2..22 for SX1262)
s.lora.preamble                  # symbols (default 8)
s.lora.sync_word                 # 0x14 = public LoRa, 0x12 = some
                                 #   private nets; defaults to RNode/RNS
s.lora.mode                      # full | gateway | access_point |
                                 #   roaming | boundary
s.lora.tcxo_voltage              # 1.8 (T-Deck) or 2.4 (T-Deck Pro)
s.lora.cad_enabled               # 0|1 — listen-before-talk via CAD
s.lora.airtime_cap_pct           # % of duty cycle ceiling for TX
                                 #   (region-specific, default 1)

s.lora.announce.interval         # override of s.rnsd.announce.interval
                                 #   for this iface (LoRa is slow,
                                 #   typically less frequent than the
                                 #   global default). Empty ⇒ use
                                 #   global.

# IFAC (per §10.7) — empty netkey ⇒ IFAC disabled
s.lora.ifac_netname              # public network name string
s.lora.ifac_size                 # bytes (default 16)
secrets.lora.ifac_netkey         # PSK bytes (hex)
```

### 12.2 Ephemeral storage

```
lora.up                          # 1 when SX1262 initialized + iface
                                 #   registered with rnsd
lora.chip                        # "SX1262"
lora.bitrate_eff                 # bits/sec from current SF/BW/CR
lora.stats.tx_bytes
lora.stats.rx_bytes
lora.stats.tx_frames             # LoRa-frame level (post-split)
lora.stats.rx_frames
lora.stats.crc_err
lora.stats.split_rx_timeout      # incomplete reassembly count
lora.stats.airtime_used_pct      # rolling window
lora.stats.rssi_last             # dBm of last received frame
lora.stats.snr_last              # dB
lora.stats.rssi_best             # dBm (best in last hour)
lora.stats.rssi_worst
lora.stats.ifac_drops            # packets dropped due to IFAC mismatch
```

### 12.3 CLI

```
lora                      # status: up/down, frequency, SF/BW/CR, TXP,
                          #         tx/rx counters, last RSSI/SNR
lora up | down            # toggle s.lora.enable
lora freq <Hz>            # shortcut for `set s.lora.frequency`
lora bw <Hz>
lora sf <7..12>
lora cr <5..8>
lora txp <dBm>
lora mode <name>          # full | gateway | access_point | roaming | boundary
lora rssi                 # dump rolling RSSI/SNR window
```

### 12.4 Web panel — `LoraPanel.vue`

Lives at **Settings → Transports → LoRa**. Registered via
`registerLora()`. Holds all radio settings (frequency, BW, SF, CR,
TX power, etc.) and the LoRa-specific RNS interface config (mode,
IFAC, airtime cap, announce interval override).

**No preselected defaults.** Selectors render with no value chosen
until the user picks one — frequency, bandwidth, SF, CR all show as
"not set" on a fresh device, and the radio stays disabled until the
user has configured the band they actually want to use. No
"sensible-default mesh config" wizard.

**Section: Radio**

- `SettingToggle` → `s.lora.enable`
- `SettingSelect` → `s.lora.frequency` (options: 433.0 / 868.0 /
  915.0 / 920.0 MHz with regional notes; no default)
- `SettingSelect` → `s.lora.bandwidth` (125 / 250 / 500 kHz)
- `SettingSelect` → `s.lora.spreading_factor` (7..12 with
  bitrate/range hints in option labels)
- `SettingSelect` → `s.lora.coding_rate` (4/5 .. 4/8)
- `SettingSlider` → `s.lora.tx_power` (2..22 dBm)
- `SettingSlider` → `s.lora.preamble` (6..16)

**Section: RNS interface**

- `SettingSelect` → `s.lora.mode`
- `SettingSlider` → `s.lora.airtime_cap_pct` (0.1 .. 10)
- `SettingToggle` → `s.lora.cad_enabled`
- `SettingSlider` → `s.lora.announce.interval` (override of global;
  empty ⇒ use `s.rnsd.announce.interval`)

**Section: IFAC** (collapsible, default collapsed)

- `SettingText` → `s.lora.ifac_netname`
- `SettingText` (password-style with show/hide) →
  `secrets.lora.ifac_netkey`
- `SettingSlider` → `s.lora.ifac_size`

**Section: Hardware**

- `SettingSelect` → `s.lora.tcxo_voltage` (1.8 / 2.4 V)

Live status (RSSI, SNR, frame counts, airtime, IFAC drops) is
**not** in the Settings panel — see Status / LoRa subwindow (§12.5).

### 12.5 Status / LoRa — `LoraStatusWindow.vue`

`FloatingWindow` opened from **Status → LoRa**. Live LoRa state:

- `lora.up`, `lora.chip`, `lora.bitrate_eff` (bits/sec computed from
  current SF/BW/CR).
- Counters from `lora.stats.*`: tx/rx bytes, frames, CRC errors,
  split-RX timeouts, IFAC drops.
- Last RSSI/SNR + best/worst rolling window.
- Visual airtime gauge tracking `airtime_used_pct` against the cap.

---

## 13. `auto` — AutoInterface transport task

Zero-config RNS over the LAN: IPv6 link-local multicast discovery +
unicast UDP data, wire-compatible with upstream RNS AutoInterface. See
§8.1 for the protocol and threading model; this section is the plumbing
surface.

### 13.1 Persistent storage

```
s.auto.enable                    # 0|1 (default 0)
s.auto.group                     # group name (default "reticulum");
                                 #   only same-group nodes peer, and it
                                 #   sets the IPv6 multicast discovery
                                 #   address.
s.auto.mode                      # interface mode (default "gateway":
                                 #   gateway|full|access_point|roaming|
                                 #   boundary)
s.auto.version                   # defaults gate
```

No host/port/peer keys — discovery is automatic. No IFAC keys yet (IFAC
for `auto` can fold in later per §10.7, keyed on the group).

### 13.2 Ephemeral storage

```
auto.state                       # down|waiting_wifi|waiting_addr|up|
                                 #   rnsd_unavailable|error
auto.up                          # 1 when sockets bound + iface registered
auto.addr                        # our IPv6 link-local address
auto.group_addr                  # the group's multicast address
auto.peers                       # discovered peer count
auto.stats.tx_bytes
auto.stats.rx_bytes
auto.stats.tx_packets
auto.stats.rx_packets
auto.stats.tx_fail               # sendto failures
auto.stats.rx_drop               # rx queue overruns
```

### 13.3 CLI

```
auto                      # status: state, group, mcast + our addr,
                          #         peer count, tx/rx counters
auto up | down
auto peers                # discovered peers + last-heard age
```

### 13.4 Web panel — `AutoPanel.vue`

Lives at **Settings → Transports → AutoInterface**. Registered via
`registerAuto()`.

**Section: Configuration**

- `SettingToggle` → `s.auto.enable`
- `SettingText` → `s.auto.group`
- `SettingSelect` → `s.auto.mode`

**Section: Status** (inline, no separate window)

- State badge from `auto.state`; our address (`auto.addr`).
- `auto.peers` count; tx/rx packet counters + drops/fails from
  `auto.stats.*`.

The peer set is small and self-managing, so AutoInterface has no
dedicated Status floating window — the panel carries live status.

---

## 14. `tcp` — TCP transport task (outbound + inbound)

One task. Watches both `s.tcp.peers.*` (outbound dials) and
`s.tcp.server_*` (inbound accepts). Each connection — dialed or
accepted — registers as its own iface with rnsd.

The wire format is the HDLC byte stuffing documented in §8.2.

### 14.1 Persistent storage

```
# Outbound peers (numeric-keyed)
s.tcp.peers.0.enable             # 0|1 per peer
s.tcp.peers.0.host               # hostname or IP
s.tcp.peers.0.port               # default 4965 (RNS TCP convention)
s.tcp.peers.0.mode               # interface mode (default "gateway")
s.tcp.peers.0.name               # optional friendly name
s.tcp.peers.0.retry_min          # backoff floor in seconds (default 2)
s.tcp.peers.0.retry_max          # backoff ceiling (default 300)
s.tcp.peers.0.announce.interval  # per-peer announce override; empty
                                 #   ⇒ s.rnsd.announce.interval
s.tcp.peers.0.ifac_netname
s.tcp.peers.0.ifac_size
secrets.tcp.peers.0.ifac_netkey

# Inbound (TCP server)
s.tcp.server_enable              # 0|1 — single boolean for "run server"
s.tcp.server_port                # default 4965 (RNS TCP convention)
s.tcp.server_mode                # interface mode for accepted peers
                                 #   (default "gateway")
s.tcp.max_inbound                # cap on concurrent inbound (default 8)
s.tcp.server_announce.interval   # override applied to all accepted
                                 #   peers; empty ⇒ global
s.tcp.server_ifac_netname        # IFAC applied to ALL accepted peers
s.tcp.server_ifac_size
secrets.tcp.server_ifac_netkey
```

### 14.2 Ephemeral storage

```
# Per outbound peer
tcp.peers.<id>.up                # 1 when connected and iface registered
tcp.peers.<id>.state             # idle | connecting | up | backoff
tcp.peers.<id>.last_error        # short string on last failure
tcp.peers.<id>.connected_since   # epoch seconds
tcp.peers.<id>.stats.tx_bytes
tcp.peers.<id>.stats.rx_bytes
tcp.peers.<id>.stats.reconnects
tcp.peers.<id>.stats.ifac_drops

# Listen + per-accepted-peer
tcp.server.up                    # 1 when listening
tcp.server.listen_addr           # bound IP:port
tcp.server.inbound_count         # current accepted connections
tcp.in.<id>.peer_addr            # per-accepted-conn ephemerals
tcp.in.<id>.connected_since
tcp.in.<id>.stats.tx_bytes
tcp.in.<id>.stats.rx_bytes
tcp.in.<id>.stats.ifac_drops
```

### 14.3 CLI

```
tcp                       # status: peer up/down, server state,
                          #         byte counters
tcp peers                 # list configured outbound peers
tcp peer add <host:port> [name]
tcp peer rm <id>
tcp peer up <id> | down <id>
tcp server on | off       # toggle s.tcp.server_enable
tcp server port <n>       # set s.tcp.server_port
tcp inbound               # list current accepted connections
tcp kick <id>             # close one inbound connection
```

### 14.4 Web panel — `TcpPanel.vue`

Lives at **Settings → Transports → TCP**. Registered via
`registerTcp()`.

**Section: Server**

- `SettingToggle` → `s.tcp.server_enable` ("Run TCP server — accept
  inbound RNS connections"). This is the primary toggle.
- Below the toggle (visible when enabled, or in an expandable
  "Advanced" subsection):
  - `SettingSlider` → `s.tcp.server_port` (default 4965).
  - `SettingSelect` → `s.tcp.server_mode`.
  - `SettingSlider` → `s.tcp.max_inbound`.
  - `SettingSlider` → `s.tcp.server_announce.interval` (override of
    global; applies to all accepted peers).
  - IFAC subsection (collapsed): server-side IFAC netname / key /
    size.

**Section: Peers**

- Editable list backed by `s.tcp.peers.*`. Each row: enable toggle,
  host, port, name. Add / remove. Per-row edit dialog opens IFAC
  config (netname, key, size) and announce-interval override.

Live status (per-peer state, byte counters, accepted-peer list)
lives in Status / TCP subwindow (§14.5).

### 14.5 Status / TCP — `TcpStatusWindow.vue`

`FloatingWindow` opened from **Status → TCP**:

- **Outbound peers section.** For each `s.tcp.peers.<id>`: state
  badge (idle / connecting / up / backoff), connected duration,
  byte counters, last error. "Iface" column shows the iface name
  registered with rnsd (`tcp/<id>`) — clicking links to that row
  in Status / Map.
- **Server section.** If `s.tcp.server_enable`: listen address
  (`tcp.server.listen_addr`), current inbound count
  (`tcp.server.inbound_count`), list of accepted peers (addr,
  duration, byte counters, kick button → triggers `tcp kick <id>`).

---

## 15. `lxmf` — messaging task (sketch only, out of milestone scope)

Included for completeness; not on the critical path for the
node/route visibility milestone.

### 15.1 Persistent storage

```
s.lxmf.id.<n>.enabled            # 0|1, default 1 — per-identity participation
                                 #   (0 = dark: no announce/send, inbound dropped)
s.lxmf.display_name              # appears in our LXMF announces
s.lxmf.inbox_max                 # message cap before pruning
s.lxmf.contacts.0.hash           # numeric-keyed
s.lxmf.contacts.0.nick
s.lxmf.contacts.0.last_seen
```

### 15.2 Ephemeral storage

```
lxmf.up
lxmf.dest_hash                   # our LXMF destination hash
lxmf.stats.sent
lxmf.stats.received
lxmf.stats.pending               # in send queue
lxmf.stats.failed
```

### 15.3 CLI / panel — Phase 4.

---

## 16. Phased rollout to the visibility milestone

Each phase delivers something verifiable. The milestone
("LoRa + TCP outbound, see nodes and routes in storage") is hit at
the end of Phase 3.

### Phase 0 — Hard fork + plumbing (1–2 weeks)

**Deliverables**

- Vendor `attermann/microReticulum` at pinned commit
  `5642ae7fe17de6a8be9dc4891e95cf8a47c6ebe9` under
  `components/microreticulum/`. Required for the heap-store
  fallback that lets us build without `RNS_USE_FS` /
  `RNS_PERSIST_PATHS` (see §2 for the rationale).
- Declare the component's hard dependency on **mbedTLS** and on
  the **WireGuard component (`esp_wireguard`)** in its
  `idf_component.yml` / `CMakeLists.txt`. The WG dep is for
  software Curve25519 (see §3 "Component dependencies").
- Crypto: rewrite `src/Cryptography/` against mbedTLS (AES-CBC,
  HMAC, HKDF, SHA, Fernet) + esp_wireguard's Curve25519 (X25519
  ECDH, Ed25519 sign/verify). Bench all of them on the T-Deck S3.
- ArduinoJson → cJSON, MsgPack → msgpack-c.
- `Serial.print*` → spangap logging macros.
- No-op `OS::read_file()` / `OS::write_file()`.
- `idf.py build` succeeds; the task stubs (`rnsd`, `lxmf`, `auto`,
  `tcp`, `lora`) start and log "task up."

**No storage / panels yet.** Just plumbing.

### Phase 1 — `rnsd` + `tcp` outbound, peer with public RNS TCP servers (2–3 weeks)

**Deliverables**

- `rnsd` task implements:
  - Identity load / generate. **Auto-generated on first boot;
    persisted to `secrets.rnsd.identity`.** Replaceable later via
    `rnsd identity import`.
  - `RNSD_PORT_TRANSPORT` accepts transport task connects, instantiates
    `TaskInterface` proxies, calls `Transport::register_interface()`.
  - Path table updates fire snapshots into `rnsd.paths.*` at 1 Hz,
    including `next_hop` (iface name) and `next_hop_addr` (neighbor
    hash on that iface).
  - `rnsd.identity_hash` and `rnsd.up` set on init; `rnsd.stats.*`
    published at 1 Hz.
  - `rnsd persist` CLI writes identity + known_destinations to flash.
  - Cron line installed: `0 * * * * N rnsd persist if-transport`.
- `NET_PORT_TCP_DIAL` added to net (outbound dial-on-behalf-of):
  - Connect payload = `host:port` ASCII.
  - Net's connect handler does DNS + connect on its own task,
    returns success/failure as the ITS connect result.
  - Handle is the TCP stream from byte zero.
- `tcp` task implements outbound side:
  - Watches `s.tcp.peers.*`. For each enabled peer:
    - `itsConnect("net", NET_PORT_TCP_DIAL, "host:port", ...)`.
    - On success: `itsConnect("rnsd", RNSD_PORT_TRANSPORT, payload)`
      with `name = "tcp/<peer-id>"`.
    - HDLC-encode outbound rnsd packets → net handle.
    - HDLC-decode inbound from net → emit packets to rnsd.
    - On disconnect: backoff retry per `s.tcp.peers.<id>.retry_*`.
  - Publishes `tcp.peers.<id>.*` ephemeral state.
- Browser:
  - `RnsdPanel.vue` + `TcpPanel.vue` (outbound peers section)
    registered.
  - `MapWindow.vue` opens from Status → Map — table view of
    `rnsd.paths.*` with iface and neighbor columns.

**Verification**

- Configure `s.tcp.peers.0.host = <public-rns-server>`,
  `s.tcp.peers.0.port = 4965`, enable.
- Browser Status / Map shows nodes learned via that TCP peer, with
  iface column showing `tcp/0` and the neighbor column showing the
  upstream peer's hash.
- After reboot, `secrets.rnsd.identity` and
  `s.rnsd.known_destinations.*` are restored; identity hash
  unchanged; cached destinations recognized.

### Phase 2 — `auto` (AutoInterface), peer with Python rnsd on LAN (1.5 weeks)

**Deliverables**

- `auto` task implements (BSD sockets; lwIP core locking is off):
  - Watches `s.auto.enable`. On enable + WiFi up: brings up the
    netif's IPv6 link-local address, opens the discovery (29716),
    unicast-discovery (29717) and data (42671) sockets, joins the
    group's multicast address.
  - Self-registers with rnsd via `RNSD_PORT_TRANSPORT` as iface `auto`.
  - `auto-rx` helper task `select()`s the sockets → PSRAM queue +
    `xTaskNotifyGive`; the auto task validates discovery tokens,
    maintains the peer table, and forwards data to rnsd.
  - Outbound: drains ITS, `sendto`s each datagram to every live peer.
  - Publishes `auto.stats.*`, `auto.peers`, `auto.addr`, `auto.up`.
- Browser:
  - `AutoPanel.vue` registered.
  - Map already shows AutoInterface-learned paths via the same path
    snapshot.

**Verification**

- Run Python `rnsd` with an `[[Default Interface]]` AutoInterface on a
  workstation on the same WiFi LAN (no addresses to configure on
  either side).
- `auto peers` on the device lists the workstation; Map shows its
  announces appearing as paths under iface `auto`.
- `rnstatus` on the workstation lists the T-Deck Plus.
- Disable TCP entirely; AutoInterface-only operation still works.
- Both enabled simultaneously: paths from each iface visible
  separately in the map.

### Phase 3 — `lora`, see RNS announces from the air (2–3 weeks)

**Deliverables**

- RadioLib added to `idf_component.yml`. `EspIdfHal` shim in
  `components/lora_hal/` (or under `main/` — either works).
- `lora` task implements:
  - Watches `s.lora.enable`. On enable:
    - SX1262 init via RadioLib + `EspIdfHal`.
    - Configure freq / BW / SF / CR / TXP / TCXO from `s.lora.*`.
    - Register IRAM_ATTR ISR via `radio.setPacketReceivedAction(...)`
      and `setPacketSentAction(...)`.
    - `itsConnect("rnsd", RNSD_PORT_TRANSPORT, payload)` with
      `name = "lora"`, `MTU = 500`.
  - Main loop:
    - Drain RX FIFO on ISR notification.
    - **RNode on-air framing** (1-byte header, seq nibble + SPLIT
      flag, ≤254 B payload per frame, ≤2 frames per RNS packet).
      Reassemble with 5 s timeout.
    - Forward reassembled RNS packets to rnsd via ITS.
    - Outbound: split if >254 B, schedule TX, defer during
      split-RX-pending.
    - Airtime accounting per rolling window; throttle on
      `airtime_cap_pct`.
  - Publishes `lora.stats.*`, `lora.bitrate_eff`, `lora.up`.
- Browser:
  - `LoraPanel.vue` registered.
  - Map already handles LoRa-learned paths via path snapshot;
    iface column shows `lora`.

**Verification**

- Tune to a band with known LoRa-RNS activity (typically 868 / 915
  MHz with SF7/BW125 — match the local mesh's settings).
- Map populates with peers heard on air.
- Verify interop with at least one of: an RNode, a T-Beam, another
  T-Deck. Send a test packet from a known peer; verify it appears
  in our path table with `iface = "lora"` and neighbor = the
  upstream peer's hash.
- `lora.stats.split_rx_timeout` should be 0 or near-zero on a
  reasonable link.

**Milestone hit:** browser Status / Map shows nodes and routes from
all three transports (TCP/UDP/LoRa) live, with iface and neighbor
columns revealing where each path was learned and through which
neighbor. Storage subscriptions deliver live updates without
polling.

### Phase 4 — Opportunistic LXMF + chat panel (out of milestone scope)

- `lxmf` task. Registers its destination with rnsd via
  `RNSD_PORT_DEST` packet-mode stream.
- LXMF send/receive via opportunistic single-packet delivery.
- `rns_chat:1` DC and `ChatPanel.vue`.
- Inbox via fs worker.
- Feature parity with ratdeck's messaging at this point (minus
  on-demand-link delivery for >254 B messages).

### Phase 5 — TCP server, niceties (out of milestone scope)

`tcp_server` capability is already present in the `tcp` task (just
config-disabled by default). Phase 5 is verification + UI polish:

- Inbound listen section in `TcpPanel.vue` exercised against
  external RNS clients connecting to us.
- IFAC bring-up across both directions.
- AutoInterface / multicast peer discovery (new task `auto`).
- Crontab integration for scheduled announces.
- QR identity import/export (Sideband-compatible format).
- mDNS service advertisement (`_reticulum._udp`).

---

## 17. Browser menu hierarchy and module registration

Two parallel menus: **Settings** for configuration panels, **Status**
for live-state floating subwindows.

```
Settings                       (config panels — Quasar drawer)
├─ System            (spangap-browser)
├─ Network           (spangap-browser — WiFi, WG, etc.)
├─ Reticulum         (RnsdPanel  — identity, transport mode,
│                                  announce policy, path table)
├─ Transports
│   ├─ TCP           (TcpPanel   — server toggle, peer list)
│   ├─ AutoInterface (AutoPanel  — group, mode, live status)
│   ├─ LoRa          (LoraPanel  — all radio + RNS interface settings)
│   └─ ESPnow        (EspnowPanel— channel, rate, conflict policy)
├─ Messaging         (LxmfPanel  — Phase 4)
└─ Advanced          (spangap-browser)

Status                         (live state — FloatingWindow per item,
                                same component used by Log and CLI)
├─ Map               (MapWindow         — paths, neighbors, hops)
├─ Reticulum         (RnsdStatusWindow  — overall counters, ifaces)
├─ TCP               (TcpStatusWindow   — peer states, byte counters)
└─ LoRa              (LoraStatusWindow  — RSSI, airtime, frames)
                     (AutoInterface carries its live status inline in
                      AutoPanel — no separate Status window)
```

Settings panels are pure-config — what you set. Status windows are
live-only — what's currently happening. The split keeps each panel
small and lets multiple status windows be open and visible at once
(typical use: LoRa airtime gauge open while editing tcp peers).

`web-interface/src/boot/modules.ts` populated in milestone order.
Each `registerX()` declares both its Settings panel(s) and any
Status windows it owns:

```ts
// Spangap baseline
registerSystem()
registerNetwork()

// Reticulous components, registered in the order their tasks land
registerRnsd()        // Phase 1 — Settings/Reticulum,
                      //           Status/Reticulum, Status/Map
registerTcp()         // Phase 1 — Settings/Transports/TCP, Status/TCP
registerAuto()        // Phase 2 — Settings/Transports/AutoInterface
registerLora()        // Phase 3 — Settings/Transports/LoRa, Status/LoRa
// registerLxmf()     // Phase 4 — Settings/Messaging, Status/LXMF

// Spangap baseline (last per convention — Advanced lives at the end)
registerAdvanced()
```

The "Transports" submenu under Settings is a grouping container —
exact API depends on spangap-browser's settings-registry shape; if
a group/parent mechanism doesn't exist yet, the convention is that
`registerTcp()`, `registerAuto()`, `registerLora()` declare
`{ group: 'Transports' }` and the registry buckets them under that
heading.

Status windows are registered the same way as Log and CLI — the
existing spangap-browser FloatingWindow + Status menu mechanism. No
router routes added; nothing at `/map`.

---

## 18. What we are NOT building for this milestone

Spelled out so it doesn't drift in:

- LXMF (messages, contacts, chat panel) — Phase 4.
- TCP server inbound listen — code is in the `tcp` task from Phase 1
  but defaults to disabled; verification is Phase 5.
- Link / Resource / ratchets — wait for upstream.
- On-device chat UI on the T-Deck display — display ignored entirely.
- TLS — not a thing in RNS; IFAC is the access-control primitive.
- WireGuard-bound RNS — works for free once UDP supports
  `bind_iface = "wg0"`, but not exercised as a verification step.
- mDNS service advertisement (`_reticulum._udp`) — Phase 5.
- QR identity import/export — Phase 5.
- LXST / voice — out of scope entirely.
- No Arduino, no Arduino-style cooperative loop, no `Serial.print`,
  no `Wire`, no `SPI` Arduino class.
- No mR-driven persistence — we own all flash writes.
- No `Reticulum::loop()` / `Transport::loop()` calls at the top
  level.
- No webrtc-task refactor.
- No net-task lwIP-raw refactor (deferred).
- No rnsd ownership of transport task lifecycles — transports
  self-register.
- No Rust dependencies, no Leviculum.
- No on-device transport-node duties as the default for endpoints —
  endpoint is the default; transport mode is opt-in via config.
- No closed-mesh LoRa framing; we follow RNode on-air framing for
  ecosystem interop.

---

## 19. Open questions

Still TBD; flagged so we don't forget.

- **Crypto bench numbers.** Software Curve25519 on the S3 — measure
  X25519 scalar mult, Ed25519 sign/verify, before committing the
  per-packet ECDH cost model.
- **mR internals that secretly assume polled invocation.** We'll
  find out by porting; expect to audit Transport / Link / Resource
  for hidden poll assumptions.
- **Registration aux-message protocol.** Final wire format for the
  `RNSD_PORT_TRANSPORT` connect payload (msgpack? cJSON?), and the
  out-of-band aux messages for stats/state updates from transport
  task to rnsd post-registration.
- **TCP dial connect payload format.** ASCII `host:port` is
  simplest. Extension hooks (e.g. SNI hints if we ever want
  TLS-tunneled TCP) can be added later.
- **`auto-rx` queue sizing.** How many in-flight datagrams before the
  PSRAM rx queue overflows (`auto.stats.rx_drop`); depth 16 should be
  plenty for RNS rates.
- **lwIP raw API + LOCK_TCPIP_CORE pattern review.** First time
  we're using it in spangap; worth a careful walkthrough of all the
  paths that mutate PCB state.
- **Announce storms on slow interfaces.** `announce_rate_target` /
  `announce_cap` knobs in the Python reference; mR has only basic
  dedup. May need to add rate-limit logic ourselves.
- **Storage subscription firehose under load.** A busy WiFi
  interface could see many announces/sec; subscribers need either
  rate limiting or "transitions only" semantics. (Note: storage
  itself dedupes equal-value writes, but the per-key change rate
  is what subscribers see.)
- **Snapshot ownership for transports.** Each transport task
  publishes its own ephemeral state (e.g., `lora.stats.*`). For
  per-iface stats that rnsd needs to surface in
  `rnsd.ifaces.<name>.*` (bitrate, mode), should rnsd snapshot from
  its own RAM proxy state, or subscribe to the transport's
  published values? Probably the former — rnsd's proxy already
  holds these from the registration payload + aux updates.
- **IFAC bring-up — minimum subset to ship.** mR has IFAC plumbing
  in the packet header (flag bit + variable IFAC field). At a
  minimum we need to: (a) parse incoming IFAC fields and pass /
  reject by interface-key match, (b) generate IFAC on outgoing
  packets when our interface has a key configured. Need to confirm
  mR's existing IFAC code path is functional vs stubbed; if
  stubbed, this is one more thing on the Phase 0 swap list.
- **Transport task crash recovery.** What if the lora task crashes
  mid-packet? ITS disconnect cleans up rnsd's proxy; supervisor or
  main needs to restart the task. Same model as any other spangap
  task; verify it works for self-registering ones.

### Decided (no longer open)

- **Identity bootstrap UX:** auto-generate on first boot, with
  "Export identity" button in `RnsdPanel.vue` for backup and
  "Import identity" / "Rotate identity" for replacement. No
  first-boot wizard required.
- **TLS-on-TCP:** not a thing. RNS doesn't support it; IFAC is the
  access-control primitive (§10.7).
- **TCP source-file split:** one task in `tcp.{h,cpp}` handles both
  outbound dial and inbound listen. Wire framing (HDLC), state
  machine pattern, and config namespace are shared; the directional
  differences are small enough that splitting would just duplicate
  code.
- **LoRa defaults:** none. The LoRa Settings panel ships with no
  preselected band / SF / BW / CR / TXP — user must configure to
  use. No first-boot wizard. No `CONFIG_LORA_DEFAULT_FREQUENCY`.
- **Live status placement:** lives in floating subwindows under the
  Status menu (parallel to Settings), same FloatingWindow component
  as log and CLI. Settings panels are pure-config. The
  network-map view is `Status → Map`, not a router page.
- **`storageSet` dedupe:** confirmed; `storageSet` already dedupes
  equal-value writes. Per-task diff in the snapshot pump is purely
  a performance optimization (avoids the serialize/hash work on
  unchanged keys), not a correctness concern.
- **Persist cron is `if-transport`-gated:** endpoints don't pay the
  hourly write because identity is one-shot and known_destinations
  is event-persisted (on cull / on add). Transport nodes get the
  hourly flush of paths/hashlist/tunnels.
