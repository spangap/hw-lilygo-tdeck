# Reticulum integration — architecture sketch

A rough architecture for running a Reticulum (RNS) transport node as
a task inside a new project that reuses the seccam source tree's
ITS, storage, net, web, webrtc_task, mbedTLS and WireGuard modules
as shared infrastructure. Seccam itself is a security-camera firmware
and is not the target of this work — this document is for a separate
project that happens to share the same ESP-IDF building blocks. This
is a sketch, not a spec. Assumes familiarity with
`reticulum/overview.md` and with the module layout described in
seccam's `CLAUDE.md`.

Everything below is opinionated. Pushback welcome.

---

## 1. Goal, not-goal

**Goal.** One `reticulum` task in the firmware that:

1. Participates as an RNS transport node on at least WiFi (LAN via
   AutoInterface-compatible multicast, plus TCPServer/TCPClient and
   UDP interfaces for internet reach).
2. Exposes its identity, destinations, announces, paths and LXMF
   inbox to other tasks over ITS, so any task (or the browser over
   WebRTC DataChannels) can send/receive RNS packets and LXMF
   messages without touching RNS internals.
3. Drops in behind the existing webrtc_task "content-free DC router"
   with at least `reticulum:1` (network map / control) and
   `rnschat:1` (LXMF chat), following the same `label:port` pattern
   used by `live:1`, `play:1`, `storage:1`, `log:1`, `cli:1`.

**Not-goal, unless the target hardware grows a LoRa radio.** RNode /
LoRa framing. A `SerialInterface` or `KISSInterface` binding plugs
in cleanly once the hardware appears — architecturally trivial.

**Not-goal, for now.** Voice (LXST). License (CC BY-NC-ND 4.0) is
wrong for anything this codebase might want to become, and the
reference implementation is early alpha.

---

## 2. Which RNS codebase to build on

Four options, ranked by practicality:

### 2.1 ratspeak/microReticulum — recommended

`github.com/ratspeak/microReticulum` — a maintained fork of
`attermann/microReticulum`, Apache-2.0, C++17, ESP32-targeted.
Actively developed (last commit within the past day as of
2026-04-24). Currently implements:

- Identity, Destination, Packet primitives.
- Announce mechanism and path finding, tested.
- Ed25519 signatures and X25519 key exchange.
- Full Fernet-compatible crypto stack (AES-CBC, HKDF, HMAC, PKCS7).
- Dynamic Interface abstraction and UDPInterface.
- Basic Transport with path persistence.

On the roadmap but **not yet implemented**: Link, Resource, Channel,
Buffer, ratchets. Practical consequences:

- **No forward secrecy on opportunistic packets.** Without ratchets,
  every packet to a SINGLE destination does a fresh X25519 ECDH
  against the recipient's long-term identity key, then Fernet
  (AES-CBC + HMAC-SHA256). Traffic is encrypted, but if the
  long-term identity key leaks later, captured ciphertext can be
  decrypted retroactively. Ratchets (added to Python RNS in the
  0.7.x line) rotate ephemeral keys to close this window. A
  pre-ratchet endpoint remains wire-interoperable with modern
  Python RNS — the Python side gracefully degrades — so ratdeck
  talks to Sideband just fine, at reduced crypto hygiene.
- **ECDH per packet is expensive** without ratchets, which is why
  the software-Curve25519 fallback matters on ESP32 (see §9.1).
  Ratchets amortize ECDH across many messages in a session.
- **No Link** means no persistent encrypted session, so no
  stream-over-RNS for other tasks; datagrams only.
- **No Resource** means no segmented reliable transfer, so LXMF
  propagation-node flows don't work and large messages can't be
  split across packets.
- **No Channel / Buffer** means no stream abstraction over Link
  (moot until Link lands).

What you can do with what exists today:

- Be a participating transport node. Announce, receive, forward
  announces, discover paths, maintain a path table.
- Send and receive single-packet datagrams to any SINGLE destination.
- **Opportunistic LXMF messaging.** LXMF has three delivery modes —
  opportunistic (one encrypted packet to a SINGLE destination),
  direct (requires Link), propagated (requires Resource). The
  opportunistic path works with current microReticulum primitives;
  short messages to known contacts go through cleanly.

**Ratdeck is the existence proof.** `ratspeak/ratdeck` is a
standalone Reticulum node on the LilyGo T-Deck Plus (ESP32-S3)
running this microReticulum fork, with a working LXMF messenger UI.
77 stars, actively shipping releases. It demonstrates that the
ESP32-S3 can be a useful RNS node *today* with only what
microReticulum currently provides. Ratdeck itself is GPL-3.0 — use
it for ideas and as confirmation that the library works, do not copy
its source into a non-GPL codebase.

**License asymmetry worth respecting.** ratspeak chose Apache-2.0 for
the library and GPL-3.0 for ratdeck — library permissive, firmware
copyleft. That's the right layering. Depend on microReticulum as a
library; keep a clean separation from ratdeck.

**Integration cost from the current state:**

- Port the build from PlatformIO to ESP-IDF as an `idf_component`
  under `components/microreticulum/`. The repo carries a
  `CMakeLists.txt` alongside `platformio.ini`, so this is wrapping,
  not rewriting.
- Review the crypto dependency. microReticulum historically used
  `rweather/Crypto` (Arduino-world); check whether the ratspeak
  fork has moved or abstracted this. If it still pulls Crypto,
  rewire to mbedTLS (already linked). If it abstracts cleanly,
  provide an mbedTLS backend only.
- Review serialization. Upstream used ArduinoJson + MsgPack. Replace
  ArduinoJson with cJSON (already present via the storage module);
  keep msgpack or pull in msgpack-c.
- Wire logging to `log.cpp`'s `err/warn/info/dbg` instead of
  `Serial.print`.

**Where LXMF lives — and doesn't.** LXMF is **not** in
microReticulum, and no permissively-licensed C++ LXMF exists
anywhere. The full landscape:

- ratdeck implements LXMF itself under `src/reticulum/LXMFManager.*`
  and `LXMFMessage.h` — part of the ratdeck firmware, **GPL-3.0**.
  Useful as a reference read, not as a source to copy.
- Leviculum implements LXMF — **AGPL-3.0-or-later**. Same license
  trap as the rest of Leviculum.
- `markqvist/LXMF` (Python, MIT) is the protocol reference.

So **LXMF for this project is first-party work**: roughly 500 LOC
of C++ on top of microReticulum's Destination / Packet primitives,
cribbed from the Python reference. No shortcut available. Put it
in `main/lxmf.cpp/h` alongside the `reticulum.cpp/h` adapter.

**Filling the Link / Resource / ratchets gap:**

- v1: opportunistic LXMF only, on top of current microReticulum.
  Enough for a working network-map + chat UI; ratdeck proves the
  user experience is viable.
- v2: implement Link + Resource either as upstream contributions
  to ratspeak/microReticulum (Apache-2.0, benefits everyone) or as
  a local extension. Reference Leviculum's Rust for algorithmic
  guidance — read it, don't copy it. AGPL-3.0 doesn't reach across
  a clean-room reimplementation but absolutely reaches across a
  transcription.
- v3: add ratchets once Link is stable. Small protocol-level
  change on top of Link state; upstream contribution is the right
  path.

Verdict: **start here.** Best license, active maintenance, proven on
ESP32-S3, visible community.

### 2.2 attermann/microReticulum — fallback

The upstream ratspeak forked from. Apache-2.0, same protocol
coverage. Historically a single developer; lower activity than the
fork. Use only if ratspeak diverges badly and the upstream looks
healthier.

### 2.3 Leviculum (Rust)

Most complete non-Python port. `no_std + alloc`, wire-compatible
with Python RNS, implements Link / Resource / ratchets / Channel /
Buffer. Hosted on Codeberg (`codeberg.org/Lew_Palm/leviculum`), 532
commits, actively maintained.

Blockers:

- **AGPL-3.0-or-later.** For a network-exposing firmware the network
  clause (section 13) entitles any connecting RNS peer to the
  complete corresponding source of the whole linked binary.
  Acceptable if this project is going to be open-source under AGPL;
  unworkable otherwise.
- A C++ transcription of Leviculum is still a derivative work —
  AGPL does not launder through language porting.
- Introducing Rust to an ESP-IDF build is tractable via `esp-rs` but
  not trivial. LTO, PSRAM placement, and linker-script assumptions
  are all C/C++ today.

Verdict: **only if AGPL is acceptable for the whole project.** In
that case Leviculum jumps ahead of microReticulum on feature
completeness and would be worth the Rust-in-ESP-IDF effort.

### 2.4 Write from spec against mbedTLS

Read the Python reference, implement directly in C++. Rough scope:

- Identity: ~200 LOC (two mbedtls keypairs, HKDF, Fernet token).
- Destination / announces / path table: ~1000 LOC.
- Packet framing and crypto: ~500 LOC.
- Transport / forwarding: ~800 LOC.
- Link (3-packet handshake + state): ~600 LOC.
- Resource (segmented reliable transfer): ~1000 LOC.
- LXMF on top: ~500 LOC.

~4500 LOC to catch up to ratspeak/microReticulum plus the
Link/Resource/ratchet gap. Since ratspeak is already ~70% of the
way there under a permissive license, there's no reason to start
from zero. Only revisit if ratspeak turns out to be a dead weight.

### 2.5 Recommended path

1. Fork `ratspeak/microReticulum` into
   `components/microreticulum/`. Port the build to ESP-IDF CMake.
   Ensure the crypto backend is mbedTLS (adapt or replace as
   needed). Replace ArduinoJson with cJSON. Keep msgpack (pull in
   msgpack-c). Delete Arduino `Serial.print` logging; wire it to
   `log.cpp`'s `err/warn/info/dbg`.
2. Add a thin `reticulum.cpp/h` adapter in `main/` that owns the
   task lifecycle, ITS server plumbing, config binding, and exports
   a narrow C++ API for other tasks.
3. Implement opportunistic LXMF on top. LXMF is ~500 LOC of
   protocol above the existing Destination/Packet layer; Links are
   not required for the opportunistic path. This matches what
   ratdeck ships today.
4. Track ratspeak upstream for Link / Resource / ratchet work. When
   it lands, pick it up and extend LXMF to use direct (Link-based)
   delivery for longer messages.
5. If ratspeak's upstream stalls on those items, contribute them.
   Apache-2.0 means the work benefits the whole ecosystem and we
   avoid an unnecessary private fork.

---

## 3. Task shape

A new task: `reticulum` (or `rns` — pick one and stick with it).

### 3.1 Boot, stack, core

- **Stack**: 12 KB PSRAM stack. RNS is heavier than anything except
  webrtc: running the path table, processing announces, signing,
  decrypting, and msgpack-ing. WireGuard's crypto fit in 8 KB after
  tuning; RNS is roughly similar but does more work per packet.
- **Core pinning**: core 0 (like webrtc_task). lwIP's `tiT` runs on
  core 0 in this build; UDP interface chatter stays on core 0. Core
  1 runs web / CLI / app-specific consumers.
- **Priority**: 2, same as webrtc. Above idle, below hot hardware
  tasks.
- **PSRAM-stack implication**: cannot touch SPI flash directly from
  the reticulum task — must use the `fs` worker for any read/write
  of `/state/reticulum/*`. Path-table persistence is the main flash
  writer on the RNS side; batching is already the right pattern
  (similar to storage.cpp's coalescing save).

### 3.2 Main loop

```
while (itsPoll(shortTimeoutMs)) {
    // ITS aux + connects + forwards processed by callbacks
}

// After each poll, service the RNS state machine:
rnsPumpAnnounces();     // outgoing announces due this tick
rnsPumpPathRequests();  // retries for unresolved paths
rnsPumpLinks();         // (v2) link handshake timers, keepalives
rnsPumpResources();     // (v2) segment retransmit
rnsDrainUdp();          // our own UDP socket(s) for UDPInterface
lxmfPump();             // LXMF delivery attempts, propagation
```

`shortTimeoutMs` is the shorter of "next RNS timer" and "1000 ms" —
same pattern as rec_task's 1000 ms cap. The task wakes on:

- ITS inbox messages (aux / connect / disconnect / forward) via the
  FreeRTOS queue.
- Task notifications from `itsSend` when stream-mode clients are
  available to send to.
- UDP socket readiness (via `select` inside `rnsDrainUdp` — same
  pattern as webrtc_task's DTLS UDP).
- TCP socket readiness on TCPServerInterface / TCPClientInterface
  sockets — opened via `net_port_msg_t` to net, so inbound connects
  arrive as ITS forwards.

### 3.3 ITS ports exposed

Following the `label:port` convention:

| Port const (itsPort) | Label (DCEP / internal) | Direction | Purpose |
|---|---|---|---|
| `RNS_PORT_MAP   = 1` | `rns_map:1` | bi stream | Network map stream: announces, paths, link events, interface stats. Packet-mode, one JSON message per event. |
| `RNS_PORT_CTL   = 2` | `rns_ctl:1` | bi stream | Control: list destinations, force announce, rotate identity, import/export identity, set ratchet. |
| `RNS_PORT_LINK  = 10` | (internal, v2) | bi stream | Generic RNS link socket for other tasks: `itsConnect("reticulum", RNS_PORT_LINK, dest_hash, 16, ...)` opens an RNS Link; `itsSend/itsRecv` pump bytes via Channel/Buffer. Requires Link upstream. |
| `RNS_PORT_DGRAM = 11` | (internal) | aux + stream | Datagram send. Aux-message API for small (≤aux cap) opportunistic sends; stream fallback for payloads larger than the aux cap. |
| `RNS_PORT_DEST  = 12` | (internal) | bi stream | Destination registration: a task (notably `lxmf`) opens a packet-mode stream, passes its destination hash in the connect payload, and from then on receives inbound packets for that destination as messages and sends outbound packets as messages from it. |

The map DC delivers announce / path / link events as the transport
state changes. Packet-mode, small JSON blobs; fits the existing
webrtc DCEP router as-is. No new routing code in webrtc_task.

For plain TCP access (nc-style), a stream-mode port analogous to
`LOG_PORT_TCP`: `s.net.rns_tcp_port` (disabled by default). Useful
for an `rnsstatus`-like read-only dump over LAN.

### 3.4 Interaction with other tasks

**Any task (v2, once Links land):**

```
int h = itsConnect("reticulum", RNS_PORT_LINK, dest_hash, 16, 5000, ref);
if (h < 0) { /* no path, or link setup failed */ }
itsSend(h, buf, len, 500);   // goes out over the RNS link
n = itsRecv(h, buf, sizeof buf, 500);
```

Semantics: the ITS handle carries bytes over an RNS Link's Buffer
channel. MTU is RNS's ~383 B per underlying packet; fragmentation
is Buffer's job, so from the caller it looks like a normal stream.
Backpressure via `itsSpacesAvailable(h)`. Setup latency is one path
request + three-packet link handshake; plan for 300–2000 ms on WiFi,
much more on LoRa.

**Datagram style (v1, works today):**

```
rnsSendPacket(dest_hash, buf, len, RNS_FLAG_OPPORTUNISTIC);
```

Aux message, fire-and-forget. Used by LXMF for opportunistic
delivery and by any task that only needs a single packet.

**LXMF (on the `lxmf` task — see §3.5):**

```
lxmfSend(dest_hash, "title", content, content_len);
// Delivery receipt arrives on a registered aux callback:
lxmfRegisterInbox(cb);
```

These are thin wrappers around ITS aux / packet-mode stream to the
`lxmf` task; they do not call reticulum directly.

### 3.5 LXMF as its own task

LXMF is a protocol on top of RNS, not part of RNS state. It gets
its own FreeRTOS task for the same reason every consumer in the
seccam tree gets one: it's a separate concern with its own
lifecycle, its own storage, its own retry timers, and no shared
mutable state with the RNS layer below. The same split will apply
to any future protocol on RNS — nomadnet-style pages, group chat,
file sync, LXST telephony — each is its own task consuming
reticulum's services over ITS.

- **Stack**: 8 KB PSRAM. Lighter than reticulum — no path table,
  no crypto scratch beyond hashing message fields. Inbox disk I/O
  goes via the `fs` worker so the stack stays small.
- **Core pinning**: core 1 (user-facing / storage-heavy side).
  Keeps reticulum's packet forwarding on core 0 uninterrupted by
  flash writes.
- **Priority**: 1 (below reticulum's 2). LXMF can wait; RNS
  transport can't.

**Internal API to reticulum:**

- At init, `lxmf` opens a packet-mode stream via
  `itsConnect("reticulum", RNS_PORT_DEST, <dest_hash>, 16, ...)`.
  From then on, inbound packets for that destination arrive on the
  handle as messages; outbound packets go out on the same handle.
- For sends, `lxmf` resolves destination paths either on demand
  (aux query on `RNS_PORT_CTL`) or by subscribing to path-update
  events from `RNS_PORT_MAP`.
- Small LXMF control packets can take the aux-flavored
  `RNS_PORT_DGRAM` path when the payload fits the aux cap.

**ITS ports exposed by `lxmf`:**

| Port const (itsPort) | Label (DCEP / internal) | Direction | Purpose |
|---|---|---|---|
| `LXMF_PORT_CHAT = 1` | `rns_chat:1` | bi stream | Browser chat DC. Packet-mode JSON: `{send: {dest, content, title?}}` / `{recv: {...}}`. |
| `LXMF_PORT_API  = 2` | (internal) | bi stream | Task-to-task LXMF API for other modules that want to send or subscribe to messages without going through the browser protocol. |

Storage: `/state/rns/lxmf/` (fall back to `/sdcard/rns/lxmf/` if
present). Inbox is the flash writer — coalesce and prune as
described in §5.2.

### 3.6 Stream vs aux — broad strokes

ITS offers two shapes: stream-mode connections (byte or packet-mode
buffers with backpressure) and aux messages (fire-and-forget queue
entries, capped at `ITS_MAX_MSG_DATA` — 96 bytes today minus
header). Some choices between them are genuinely arbitrary and can
be revisited as the implementation matures. The rules we're
converging on:

**Packet-mode streams are the default for RNS-adjacent traffic.**
Everything in this subsystem is message-oriented: RNS packets have
a ~383 B MTU with discrete boundaries, LXMF messages are discrete
units, LXST voice is RTP-like real-time packets, Link/Channel/
Buffer present message APIs. Byte-mode streams would force every
consumer to re-frame and would smear boundaries for anything
latency-sensitive. Use byte-mode only for genuinely stream-shaped
things (CLI keystrokes, log text tails).

**Use aux for:**

- Commands and notifications that fit the cap: "force announce",
  "path found for dest X", "link down", "config changed".
- State-change pings between tasks that share a storage key —
  like `storageSet()` aux-notifying subscribed modules.
- Small opportunistic datagrams where the sender doesn't need a
  reply.

**Use a packet-mode stream for:**

- Any long-lived session with a peer: every browser DC, every
  network TCP/RTSP/HTTP connection, every task-to-task
  subscription (storage sync, log tail, chat feed, inbound-packet
  delivery to a registered destination).
- Anything whose payload might exceed the aux cap: full RNS
  packets, LXMF messages with bodies, LXST voice frames.
- Anything that needs backpressure (`itsSpacesAvailable`) so
  producer and consumer stay in step.
- Request/response where the response needs to be reliable and
  the sender has to wait on it. (For short one-shot replies, aux
  with a correlation id also works; gray area.)

**Gray areas we'll commit to a convention for later, not now:**

- Task-to-task control that fits in aux today but might grow —
  e.g. "rotate identity with these parameters". Safer as a stream
  from day one.
- Event broadcasts to N subscribers — aux per subscriber is
  simpler; stream-per-subscriber is cleaner if the events grow.
  No fixed rule yet.

The 96-byte cap and "RNS-adjacent = packet-mode" are the hard
constraints. Everything else is judgment; pick the shape that
makes the contract simplest and only regret it if the code starts
getting awkward.

---

## 4. Interfaces to implement initially

Prioritized by usefulness and by how much they reuse existing
infrastructure.

### 4.1 UDPInterface (first)

The simplest thing that works. Owns its own UDP socket on a
configurable port (`s.rns.udp.port`, default 4242). Uses
`MALLOC_CAP_INTERNAL` for the socket struct; PSRAM-stack task is
fine for recvfrom/sendto (they don't hit flash).

Uses: peering with another RNS node at a known IP:port. Good for
testing and for a known-good home server running `rnsd`.

Maps cleanly to net.cpp's model for UDP: the task owns the socket
(same as webrtc_task owns its DTLS socket). No net_port_msg_t
needed.

ratspeak/microReticulum already ships UDPInterface, so Phase 1
reduces to "wire the existing interface to an ESP-IDF socket."

### 4.2 AutoInterface-compatible multicast (close second)

AutoInterface on the Python side uses IPv6 link-local multicast plus
UDP to auto-discover RNS peers on a LAN. ESP-IDF supports IPv6 and
multicast; the pattern is to subscribe to a well-known multicast
group and exchange beacons.

Caveat: Python RNS uses link-local IPv6 multicast; we need IPv6 MLD
join configured. Worst case, implement a UDP-broadcast fallback on
IPv4 that isn't strictly AutoInterface-compatible but lets two
nodes find each other on LAN.

### 4.3 TCPServerInterface

Register a listen port with net.cpp (same as `RTSP_PORT` / `HTTP_PORT`
via `net_port_msg_t`). Inbound connects arrive as ITS forwards with
`net_connect_t {ws=0}`. Each forwarded handle becomes one RNS TCP
peering session.

Config: `s.rns.tcp.listen_port` (0 = disabled; default 0).

### 4.4 TCPClientInterface

Outbound TCP peering to a known RNS node (host:port). Opens its own
socket on lwIP; socket runs in the reticulum task, select()'d in
the main loop.

Config: `s.rns.tcp.peers.N.host`, `s.rns.tcp.peers.N.port`.

### 4.5 Later

- SerialInterface: for a plugged-in RNode over UART. Add when LoRa
  hardware appears.
- I2PInterface: out of scope for ESP-IDF (I2P router dependency).
  Skip.
- KISSInterface / AX.25: ham-radio territory; wait for demand.
- BackboneInterface: Linux/Android only in the Python reference;
  skip.

---

## 5. Storage layout

Follows the existing `s.*` / `secrets.*` / ephemeral split from the
storage module.

### 5.1 Persistent (`/state/settings.json`)

```
s.rns.enable             = 0    # 0 = off, 1 = run
s.rns.transport_enabled  = 1    # full transport node vs endpoint-only

s.rns.udp.enable         = 1
s.rns.udp.port           = 4242
s.rns.udp.discovery      = 1    # multicast/broadcast discovery

s.rns.tcp.listen_port    = 0    # 0 = no server
s.rns.tcp.peers.0.host   = ""   # numeric-keyed object, not JSON array
s.rns.tcp.peers.0.port   = 4242

s.rns.announce.interval  = 1800 # seconds between opportunistic
                                # announces; 0 = on demand only
s.rns.announce.cap_pct   = 2    # % of iface bw for announce forwarding

s.rns.path.max           = 512  # path table entry cap
s.rns.path.ttl           = 86400

s.rns.name               = ""   # human-readable handle for the node

secrets.rns.identity     = "…"  # 64-byte hex, X25519 + Ed25519 privs
secrets.rns.lxmf.config  = "…"  # if LXMF has its own persistent state
```

Numeric-keyed objects for `peers` — per the `project_array_merge_
hazard` note, JSON arrays + storage deepMerge silently drop siblings.

### 5.2 Persistent on LittleFS directly (not in settings.json)

Things that are too chunky for the config tree:

- **Path table** cache: `/state/rns/paths.bin`. Rewrite coalesced
  every N minutes via the fs worker. Lost on crash → re-learned on
  next announce sweep.
- **Announce cache** (recent announces for dedup and replay
  protection): `/state/rns/announces.bin`. Small, 1–10 KB.
- **Identity backup**: `/state/rns/identity.pem` (or equivalent).
  The same material that's in `secrets.rns.identity` — keep both
  so factory reset wipes both in lockstep.
- **LXMF inbox**: `/state/rns/lxmf/inbox/*.msg`. One file per
  received message. If the box grows we prune oldest-first.

LittleFS writes have a flash wear cost. Path tables should *not*
churn to flash — RAM cache, flushed on a timer only if entries have
actually changed. Announce caches can stay RAM-only, regenerated on
restart. Use `/sdcard` for LXMF bulk storage if we expect real
volume.

### 5.3 Ephemeral (RAM, not persisted)

```
rns.up                 # 1 when transport task is fully initialized
rns.identity_hash      # our own destination hash, hex
rns.destinations.N.*   # our registered destinations (LXMF inbox, etc.)
rns.stats.announces_in
rns.stats.announces_out
rns.stats.packets_in
rns.stats.packets_out
rns.stats.links_open
rns.stats.paths_known
rns.paths.<hash>.next_hop / .hops / .age / .rtt
```

Published via the same `storageSet()` path used by everything else;
the browser picks them up over `storage:1` automatically.

---

## 6. Browser integration

### 6.1 Shared WebRTC PC — add one more DC builder

In `web-interface/src/modules/reticulum.ts` (new file):

```
session.registerChannel((pc) => {
  const ch = pc.createDataChannel('rns_map:1', { ordered: true });
  // …wire to a Pinia store
});
```

The existing webrtc_task router sees the label, parses `reticulum:1`
(or `rns_map:1` — whatever we name the ITS port), does
`itsConnect("reticulum", RNS_PORT_MAP, …)`, and from then on the DC
is a transparent bidi pipe to the reticulum task.

Auth: already inherited from the `/webrtc` endpoint. No extra work.

### 6.2 Network map panel

`web-interface/src/modules/panels/ReticulumPanel.vue`. Same pattern
as existing panels (SystemPanel, WireGuardPanel).

Data model:

```ts
interface RnsNode {
  hash: string;          // 16-byte dest hash, hex
  handle?: string;       // if we know the app_name / aspect
  hops: number;
  nextHopIface: string;
  nextHopAddr?: string;  // IP for TCP/UDP, MAC for LoRa, etc.
  firstSeen: number;
  lastAnnounce: number;
  rtt?: number;          // ms, once linked (v2)
  active: boolean;       // has an open link to us right now (v2)
}

interface RnsIface {
  name: string;
  type: 'udp' | 'tcp_server' | 'tcp_client' | 'auto' | ...;
  mode: 'full' | 'gateway' | 'access_point' | 'roaming' | 'boundary';
  up: boolean;
  bitrate: number;
  txBytes: number;
  rxBytes: number;
  announceCapPct: number;
}
```

Map rendering: nodes as circles, edges drawn between nodes that
share a next-hop along our path table. First version: a flat list
sorted by hops, with expandable rows. Graph view: a follow-on using
d3-force or a simple static layout.

Event stream on the DC:

```json
{ "t": "announce", "hash": "abcd…", "hops": 3, "iface": "udp0" }
{ "t": "path_update", "hash": "abcd…", "hops": 2, "next": "…" }
{ "t": "link_up", "hash": "abcd…", "rtt": 42 }
{ "t": "link_down", "hash": "abcd…" }
{ "t": "iface_stats", "name": "udp0", "tx": 123456, "rx": 234567 }
```

Client sends control:

```json
{ "cmd": "announce", "aspect": "service.x" }
{ "cmd": "drop_path", "hash": "…" }
{ "cmd": "subscribe", "aspect": "*" }
```

### 6.3 Chat panel (LXMF)

Second DC builder, `rns_chat:1`, **routed to the `lxmf` task** via
`LXMF_PORT_CHAT` (see §3.5 — webrtc_task picks the target task
from the DC label, so no proxying through reticulum). One JSON
packet per LXMF message in either direction. Minimum viable UI:

- Sidebar of known peers (derived from the map store).
- Message list per peer, from `/state/rns/lxmf/inbox/…`.
- Send box.
- Attachments: out-of-scope for v1. LXMF's field dictionary allows
  images and audio, but bandwidth is thin and v1 has no Resource
  support — leave for later.

### 6.4 Identity / onboarding

A settings panel subsection under "Network → Reticulum":

- Show our identity hash and destination hashes.
- Show our LXMF address.
- Generate / rotate / import identity (via RNS_PORT_CTL).
- Copy-to-clipboard for our destination hash.
- QR code for the destination hash and a signed "claim this is me"
  blob — compatible with Sideband's paper-messaging QR import.

---

## 7. Interop with existing modules

### 7.1 WireGuard as an RNS interface

WireGuard gives us an authenticated L3 tunnel over any transport.
If WireGuard is up, there's a netif with an IP on the WG side. An
RNS UDPInterface bound to the WG interface address peers RNS over
the tunnel.

Concretely: `s.rns.udp.interface = "wg0"` binds the RNS UDP socket
to the WireGuard netif. Two nodes on the same WG network then
auto-find each other (if AutoInterface discovery is configured) and
peer RNS traffic inside WG. A free private RNS overlay for anyone
who already runs WG.

Minor risk: RNS over WG over WiFi is three layers of framing. MTU
math: WG adds 32 bytes (header + auth tag), and UDP+IP is ~28. A
500 B RNS packet plus RNS's own framing fits inside a 1500 B WiFi
MTU comfortably, but set `fixed_mtu = 500` on this interface so
path discovery doesn't get confused.

### 7.2 Crontab driving announces

Reticulum's default announce pattern is opportunistic: announce on
start, re-announce at a long interval (hours). The existing crontab
machinery (`/state/crontab`, minute-resolution) is perfect for
driving "announce once an hour" without the RNS side needing its
own timer daemon. A line like:

```
0 * * * * N rns announce
```

Works fine. Cron output is serialized through the CLI, so a new
`rns` CLI command (see §8) ties in for free. Same goes for periodic
path-table compaction, LXMF propagation node sync, etc.

### 7.3 mDNS helping peer discovery on LAN

mDNS (already running via `spangap_mdns`) can advertise `_reticulum._udp`
on the LAN for zero-config peer discovery. Two nodes on the same
WiFi then find each other by mDNS without needing multicast RNS
announces. Redundant with AutoInterface if we implement
AutoInterface properly, but cheaper to implement mDNS first and get
peer discovery working end-to-end.

Registering one more service ad alongside the existing `_http`,
`_https`, `_rtsp` entries in `spangap_mdns.cpp` is trivial.

### 7.4 Web task and HTTP-01 webroot conflict

Nothing shared between RNS and web/HTTP; the only interaction is
over webrtc DCs, which is the whole point. No conflict.

### 7.5 SD card and LXMF storage

If LXMF inbox volume grows (a chatty group, a propagation node with
many peers), prefer SD card (`/sdcard/rns/lxmf/`) over `/state/` —
the state partition is 128 KB and a handful of LXMF messages with
images would fill it instantly. Gate on presence of SD card at
init; fall back to `/state/rns/lxmf/` with a small-quota pruner if
no SD.

---

## 8. CLI

Following the existing pattern (`wg`, `duckdns`, `upnp`, `cert`):

```
rns                       # status: up/down, id hash, #paths, #links
rns up | down             # enable/disable transport
rns announce [aspect]     # force announce
rns paths                 # dump path table
rns destinations          # dump our own destinations
rns peers                 # dump known nodes with hops/RTT
rns identity              # show identity hash + public keys
rns identity rotate       # generate new identity (destructive!)
rns identity import <file>
rns lxmf send <hash> <msg>
rns lxmf inbox            # list received messages
rns stats                 # tx/rx packets, bytes, announces
```

Silent on success (matches `set`/`unset`/`save`). `rns up` sets
`s.rns.enable=1` via storage; the reticulum task reacts to the
`storageSubscribeChanges` callback and (re)starts itself.

---

## 9. Open questions and risks

### 9.1 Crypto performance

On-device benchmarks matter. The WireGuard integration on ESP32-S3
does ChaCha20-Poly1305 on tcpip_thread and hits ~20% CPU at 14 fps
HTTPS streaming in the seccam code — so the *symmetric* side is
fine. RNS's AES-256-CBC + HMAC-SHA256 per packet is cheaper than
ChaCha-Poly per mbedTLS on the S3.

Where RNS gets expensive is asymmetric: X25519 ECDH runs on every
packet to a SINGLE destination (for key agreement) unless there's
an established Link. The open ESP-IDF thread on mbedTLS Curve25519
points at ~100 ms per scalar multiply with the "hardware-accelerated"
wrappers — too slow if we want to fan-out opportunistic packets to
many destinations. Two mitigations:

- **Use a software Curve25519 implementation** (Daniel J.
  Bernstein's donna / curve25519-ref10). Under 10 ms on a 240 MHz
  Xtensa. The seccam WireGuard component already ships one;
  reusing it is the right call and sidesteps the mbedTLS Curve25519
  perf issue entirely.
- **Prefer Links over opportunistic packets** for anything more
  than a single message (v2, once Links are implemented). Once a
  Link is up, ECDH happens once during handshake and subsequent
  packets use cheap symmetric ops.

Single biggest unknown. Benchmark before committing the architecture
to a given crypto backend.

### 9.2 DRAM vs PSRAM

Path table, announce cache, link state, LXMF inbox in RAM → PSRAM.
Fine.

Any buffer touched from lwIP callbacks must be DRAM
(`MALLOC_CAP_INTERNAL`). Pattern: the UDP recv callback copies into
a DRAM socket-level buffer, then the reticulum task picks it up —
don't have lwIP touch PSRAM directly. webrtc_task follows this
rule; same discipline here.

### 9.3 LittleFS write rate from path caching

If we write path-table updates to flash every time an announce
arrives, `/state/` will wear out fast. Must coalesce:

- RAM path table is authoritative at runtime.
- Flush to `/state/rns/paths.bin` at most every 5 minutes, and only
  if anything has changed.
- Skip persistence entirely on very-short-lived paths.

Settings.json changes coalesce at 60 s already (`s.storage.flash_
delay`); path table should coalesce at 300–600 s.

### 9.4 Wire-format parity

Whatever we pick, we have to test against real Python RNS. The
Reticulum test network (discuss.reticulum.network announces public
testbed nodes) is the obvious target. Plan for a week of "announces
disappear, links fail to establish, packets get dropped somewhere"
before declaring interop. Ratspeak/microReticulum already does this
testing for the primitives it implements, which removes most of the
pain for Phase 1; Phase 3+ (LXMF) needs its own round.

### 9.5 Announce storms on slow interfaces

Slow interfaces (LoRa especially) need careful announce propagation
policy. `announce_rate_target` / `announce_cap` / interface-mode
knobs cover this on the Python side; microReticulum implements
only basic announce dedup. May need rate-limit logic ourselves, or
upstream it.

### 9.6 Identity loss on factory reset

`secrets.rns.identity` lives in `secrets.*`-land — in settings.json,
factory reset wipes it. Correct default. The browser-side "export
identity" flow (QR / file download) lets the user back up before a
reset. Emphasize this in the UI.

### 9.7 Maintainer risk

ratspeak is a small org (one or two active people based on commit
patterns) but multi-repo (microReticulum, ratdeck, ratcom,
LXMFace, ratkey). More resilient than a single-developer upstream.
If ratspeak stalls:

- attermann's upstream remains a fallback (Apache-2.0, less
  active).
- Leviculum is the pivot option for a ground-up redo, at the cost
  of either AGPL acceptance or a clean-room reimplementation.
- Our own Link / Resource work, once done, is the bridge —
  contributing upstream first means the code already lives where
  we need it.

Pin the fork commit carefully; keep changes isolated enough to
rebase or cherry-pick forward cleanly.

### 9.8 One task for RNS internals, separate tasks per protocol

The "one task" rule applies to **RNS internals**: identity,
destinations, path table, announces, transport forwarding, Links,
Resources. These are tightly cross-referenced (an announce updates
paths which may unblock a pending Link which may satisfy a
Resource transfer). Splitting them would force locking or
message-passing across state that is conceptually one state
machine. So reticulum stays monolithic at the RNS layer.

**Protocols on top of RNS are their own tasks.** LXMF (§3.5) is
the first; LXST telephony, nomadnet-style pages, group chat, file
sync will each be their own task if/when they appear. Same pattern
as every other consumer in the seccam tree: rec_task ≠ camera
task, detect ≠ audio task. No shared mutable state crosses the
boundary; ITS is the interface.

---

## 10. Phased implementation

### Phase 0 — Feasibility (1–2 weeks)

- Benchmark mbedTLS X25519 / Ed25519 / AES-CBC / HMAC on the
  ESP32-S3 under realistic load. Get per-packet overhead numbers.
  Bench the software Curve25519 from the WireGuard component
  alongside for comparison.
- Check ratspeak/microReticulum compiles as an ESP-IDF component
  with minimal changes. Audit the current crypto dependency;
  decide on adaptation strategy.
- Quick interop test against Python `rnsd` on the LAN.

### Phase 1 — Minimum transport node (2–3 weeks)

- `reticulum` task boots, loads/generates identity.
- UDPInterface on a configured port. Can peer with `rnsd` on a
  workstation on the LAN.
- Path table + announces working. Verified against Python RNS.
- Stats published to ephemeral config; `rns` CLI for read-only
  status.
- No browser UI yet; verification via logs + `rnstatus` on the
  peer.

### Phase 2 — ITS exposure + browser map (2 weeks)

- `rns_map:1` DC and ReticulumPanel.vue.
- Event stream: announces, paths, iface stats.
- `rns_ctl:1` DC for identity / force-announce.
- Still no Links — transport only.

### Phase 3 — Opportunistic LXMF + chat panel (2 weeks)

- New `lxmf` task boots, registers its destination with reticulum
  via a `RNS_PORT_DEST` packet-mode stream.
- LXMF send and receive via opportunistic (single-packet)
  delivery, using existing Destination primitives. No Link, no
  Resource required.
- `rns_chat:1` DC (routed to `lxmf` by label) and ChatPanel.vue.
- Inbox on flash via the fs worker, basic prune policy.
- At this point the project is feature-equivalent to ratdeck's
  messaging, minus LoRa.

### Phase 4 — Links and datagrams for other tasks (deferred)

Deferred until ratspeak lands Link support upstream, or we
contribute it:

- Link establishment.
- `RNS_PORT_LINK` and `RNS_PORT_DGRAM` ITS ports (the
  stream-over-Link variant).
- Demo task that opens a Link and streams data.
- Upgrade LXMF delivery to prefer Link-based when available.

### Phase 5 — Niceties

- TCPServer / TCPClient interfaces.
- AutoInterface / IPv6 multicast, or an mDNS-based substitute.
- Crontab integration for scheduled announces.
- Propagation node client behavior (pull inbox from a public
  propagation node at boot / on wake).
- QR import/export of identity.

Stop there for v1. LXST, RNode, Resources as a general-purpose
stream API — all later.

---

## 11. TL;DR

- Build on `ratspeak/microReticulum` (Apache-2.0). Fork into
  `components/microreticulum/`, port the build to ESP-IDF, ensure
  mbedTLS backend, drop ArduinoJson for cJSON, wire logging to
  `log.cpp`.
- Two tasks, following the seccam "task per concern" pattern:
  `reticulum` (RNS internals, core 0, prio 2, 12 KB PSRAM stack)
  and `lxmf` (LXMF protocol, core 1, prio 1, 8 KB PSRAM stack).
  LXMF talks to reticulum over ITS like any other consumer.
  Future RNS-layer protocols (LXST, nomadnet, …) get their own
  tasks too.
- Reticulum exposes map / control / link / datagram / dest
  ports. `lxmf` exposes chat and a task-to-task API. Browser
  reaches each via the shared WebRTC PC.
- RNS-adjacent ITS streams are packet-mode by default (packets
  all the way up — telephony and LXST need the boundaries
  preserved); aux for small fire-and-forget messages under the
  96-byte cap.
- Reuse mDNS, WireGuard, crontab, storage, fs worker, auth from
  the seccam source tree. No new infrastructure.
- Start with UDPInterface. Add TCP and multicast after core works.
- Opportunistic LXMF works with what microReticulum has today —
  proven by ratdeck. Ship that first.
- Link / Resource / ratchets are future work; contribute them
  upstream to ratspeak when they're needed.
- Crypto performance of ECDH on opportunistic packets is the open
  risk; the software Curve25519 from the WireGuard component is
  the right path.
- Avoid LXST (license) and RNode (no LoRa yet) for v1.
- Avoid Leviculum for now: AGPL-3.0 is the wrong license for a
  network-exposed firmware unless the whole project adopts AGPL.
