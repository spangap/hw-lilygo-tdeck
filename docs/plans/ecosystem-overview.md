# Reticulum Ecosystem Overview

A reference for readers who already know how to read a protocol stack and
want to understand Reticulum well enough to decide whether to build on
it, integrate with it, or port part of it to a constrained device.

Scope: protocol architecture, software inventory (reference
implementation, alternative-language ports, application stack, CLI
tools), hardware, honest quality read on each component, and pointers.
Not a tutorial.

Timestamps / versions mentioned below are from repository pages checked
while writing this document (April 2026). Project activity moves; treat
specific dates as anchors rather than gospel.

---

## 1. What Reticulum actually is

Reticulum (RNS, "Reticulum Network Stack") is a cryptographic
networking stack authored by Mark Qvist. The design target is a single
stack that collapses what IP networking does across several layers
(L3 routing, L4 transport, and a fair amount of the session /
presentation layers) into one protocol that rides on top of any medium
that can pass framed bytes with an MTU of about 500 bytes and at least
a few hundred bits per second of throughput.

The fundamental claims worth keeping in mind throughout:

- **Cryptography is not optional.** There is no cleartext "single" or
  "link" destination. An unencrypted packet cannot exist in the design
  at those layers; the stack drops malformed or unencrypted frames at
  the destination type boundary. A "plain" destination type exists but
  it is explicitly scoped to local, discovery-only uses.
- **No source addresses on the wire.** Packets carry a destination
  hash (16 bytes) and, for link requests, a second address; the
  originator is not identified at the network layer. Replies travel
  via the cryptographic link channel, not by turning a source address
  around.
- **Self-sovereign addresses.** A "destination" is derived from a
  cryptographic identity (Curve25519 public keys plus an aspect
  naming tuple). There is no central registration, no DNS, no
  certificate authority. Conflicts are resolved by 128-bit
  (truncated) hashing.
- **Medium-agnostic.** The same packet format rides LoRa, WiFi, TCP,
  UDP, KISS TNCs, AX.25, serial lines, I2P, named pipes, or anything
  else you can wrap in a 500-byte framed interface. The stack does
  not require IP at all; IP is just one possible interface.
- **Very wide bandwidth envelope.** The design target is ~250 bps to
  ~1 Gbps; measured operating range today is roughly 150 bps to about
  500 Mbps. Idle link cost is quoted at 0.44 bps on the upstream
  docs.

### 1.1 Where it sits relative to OSI

Reticulum is best thought of as L3+L4+some presentation in one
bundle:

- **Below Reticulum**: any "interface" that can carry a whole packet
  (up to MTU ~500 B) and notify the stack on receive. That's it.
  Reticulum does not specify or care about framing below itself.
- **Reticulum layer**: identity, destinations, announces, paths,
  transport nodes, packets, links, resources, channels, buffers.
  Roughly: naming, routing, session establishment, reliable
  segmented transfer, stream abstraction.
- **Above Reticulum**: LXMF (messaging), LXST (real-time signal /
  voice), Nomad Network pages (Micron markup), rnsh (remote shell),
  bespoke application code.

The most common confusion on first contact: Reticulum is not just a
"mesh networking library," and it is not a "secure messaging library."
It is a whole network stack. The messaging and voice pieces are
separate protocols built on top.

### 1.2 Cryptographic primitives (authoritative list)

From reticulum.network/crypto.html and the reference implementation:

| Purpose               | Primitive                              |
|-----------------------|----------------------------------------|
| Key exchange          | X25519 (Curve25519 ECDH)               |
| Signatures            | Ed25519                                |
| Identity keyset size  | 512 bits total (256-bit X25519 +       |
|                       | 256-bit Ed25519)                       |
| Hashing               | SHA-256, SHA-512                       |
| Key derivation        | HKDF                                   |
| Symmetric cipher      | AES-256 in CBC mode with PKCS7 padding |
| Message authentication| HMAC-SHA256                            |
| Token format          | Fernet-style (custom, AES-CBC+HMAC)    |

The reference implementation gets X25519, Ed25519, and AES-256-CBC
from OpenSSL via PyCA `cryptography`, and uses the standard library
for SHA. A pure-Python fallback (`rnspure`) exists and is slow; the
upstream docs warn against using it for anything that needs real
performance or timing-attack resistance.

Nothing exotic: every primitive here is in mbedTLS. The ESP-IDF TLS
build for seccam already links mbedTLS for the TLS server and the
WireGuard handshake. For an embedded port this means there is no
ground-up crypto work needed, only glue.

Noteworthy gotcha: the stack uses AES-CBC + HMAC, not an AEAD
construction (GCM/ChaCha20-Poly1305). Mark has discussed this as a
tradeoff for implementability across a wide range of embedded
platforms and packet-level efficiency. The security margin is fine as
long as the MAC-then-encrypt or encrypt-then-MAC ordering is
implemented per spec.

### 1.3 Core protocol primitives

These are the names you will see everywhere. If you understand this
section you can read the Python reference and the microReticulum C++
port without reaching for the manual every five minutes.

**Identity.** A Curve25519 keyset (Ed25519 for signatures, X25519 for
ECDH). Stored as ~128 bytes of key material plus metadata. The
identity is what a human trusts; everything else is derived.

**Destination.** A triple of `(identity, direction, type, app_name,
*aspects)` collapsed by SHA-256 truncation to a 16-byte destination
hash. Four types:

- `SINGLE`: end-to-end encrypted to one identity. This is the
  workhorse. Announces are per-destination.
- `GROUP`: symmetric encryption shared among participants. Currently
  local-only.
- `PLAIN`: unencrypted. Local discovery only.
- `LINK`: the address for an ephemeral link session (see below).

Aspects are hierarchical strings like `app.service.subservice`. LXMF,
for example, publishes each user's inbox at
`{lxmf}.delivery` aspects off their identity.

**Announce.** A packet that says "identity X claims destination hash
H, here's my public key, my app data blob, a proof-of-work stamp, and
a recent ratchet key." Announces are the only way path information
enters the transport fabric. They propagate by flooding with rules:

- Dedup by the exact announce bytes.
- Each transport node records who it heard the announce from (next
  hop toward the origin) and the hop count so far.
- Hop limit 128 by default (max 255).
- Per-interface bandwidth cap (default ~2% of interface rate).
- Announces from rapidly-re-announcing destinations are deprioritized
  — spamming announces moves you to the back of the queue.
- `announce_rate_target` / `_grace` / `_penalty` let operators throttle
  forwarding per interface (e.g. "don't re-forward the same
  destination on this LoRa interface more than once per hour").
- Retry count `r` defaults to 1 — an announce is re-transmitted once
  if no other node is heard retransmitting it with a greater hop
  count.

The announce also carries a cryptographic "stamp" (proof-of-work)
whose difficulty acts as a rate-limit against announce floods. This is
not PoW for consensus; it is a per-announce cost.

**Path table.** Every transport node keeps `(destination_hash ->
next_hop_interface, next_hop_nexthop, hop_count, announce_time,
ratchet_key)`. This is the only routing state. A node only knows the
single best next hop per destination — there is no link-state
database, no full topology.

**Path request.** If a node wants to reach a destination it has not
yet heard an announce for, it sends a path request. Transport nodes
with the entry reply with an announce. Think of this as reactive
ARP/route-discovery for the entire network.

**Transport node.** A regular Reticulum instance with
`enable_transport = Yes`. Transport nodes forward packets, propagate
announces, maintain path tables, and answer path requests. Non-
transport instances still participate as endpoints but do not forward.
The operator guidance is: run transport on things that are always on
and well-connected; don't turn every phone into a router.

**Packet.** Up to ~500 B on the wire. Structure (summarized, not the
exact bit layout):

- 2-byte header: flags — IFAC present, header type (ONE/TWO
  addresses), context type, propagation type, destination type,
  packet type (DATA / ANNOUNCE / LINKREQUEST / PROOF).
- Optional IFAC authentication code (interface access code, for
  private interfaces).
- 16 B destination hash. For link requests and similar, a second
  16 B address follows.
- 1 B context byte (what this packet means in session terms).
- Payload: up to ~464 B plain, ~383 B encrypted.

Encryption for `SINGLE` destinations: ephemeral X25519 ECDH against
the recipient's public key, HKDF to derive AES-CBC + HMAC keys,
Fernet-style token. Forward secrecy comes from a ratchet on the
recipient's side — destinations periodically rotate an announcement
ratchet key (30-day expiry in the reference implementation).

**Link.** An ephemeral, bidirectional, encrypted session between two
identities. Link establishment is a 3-packet handshake:

1. Initiator sends a LINK REQUEST containing its ephemeral X25519
   public key.
2. Recipient derives the shared secret, replies with a PROOF (Ed25519
   signature over the negotiated key material).
3. Forwarding transport nodes along the way validate the proof and
   mark the link active; both endpoints verify and the session is
   live.

Three packets, ~297 B total. Initiator anonymity is preserved: the
request carries no source address and the ephemeral key is a one-time
value. Forward secrecy is per-link. RTT, RSSI, SNR and a synthetic
"link quality" metric are exposed on the link object for stats.

**Resource.** A framed, reliable, segmented transfer over a Link.
Handles sequencing, retransmission, integrity, compression, progress
callbacks. Think of it as the file-transfer / large-blob primitive.

**Channel.** A reliable, bidirectional, typed-message layer over a
Link. Messages must fit in a single packet. Apps register handlers by
message type. This is what LXMF uses for direct delivery.

**Buffer.** A `file`-like wrapper over a Channel — stream semantics
with standard Python buffered I/O on top. If you want `read()` /
`write()` over the mesh, this is the layer.

### 1.4 Bandwidth, MTU, scale

- **MTU**: 500 bytes is the design constant. It is tied to LoRa frame
  sizes and is deliberately conservative; changing it breaks
  interoperability. An `interface.fixed_mtu = 500` option exists for
  KISS devices that need it pinned.
- **Min throughput**: the stack operates over half-duplex channels as
  slow as about 5 bps. Anything below that is technically supportable
  but not practical.
- **Hop limit**: default 128, hardware max 255 (IP parity).
- **Open-link maintenance**: ~0.44 bps.
- **Scale**: there is no published hard cap, but announce bandwidth
  (2% of interface capacity by default) is the scaling constraint for
  large flat networks. Partitioning by interface mode (`gateway`,
  `access_point`, `roaming`, `boundary`) is how operators bound
  announce propagation in larger deployments. Nobody publishes a
  "Reticulum can handle N destinations" number because it depends
  entirely on how much announce traffic a given interface can afford.

### 1.5 Interface modes

Every interface has a `mode` that changes how it participates in
announce propagation and pathing. Verbatim names:

- `full` — fully participates in announces in both directions.
- `gateway` — will re-announce received announces onto other
  interfaces but does not prioritize remote announces over local
  ones.
- `access_point` — announces received here are not re-propagated to
  other interfaces; intended for a LoRa/WiFi AP-style use where
  downstream devices should not burden the backbone.
- `roaming` — the node itself is mobile; announces are treated as
  hints, paths are expected to churn.
- `boundary` — the interface is a boundary to a different network
  (e.g. the public internet); announce propagation is firewalled.

These modes are the main operator-side lever for containing announce
traffic in large deployments.

---

## 2. Software inventory

Split into: reference implementation, ports/reimplementations,
application stack, hardware firmware, auxiliary tooling.

### 2.1 Reference implementation

**markqvist/Reticulum** — the authoritative Python implementation.

- Language: Python 3 (>= 3.7-ish).
- License: Reticulum License (a permissive-ish custom license — not
  OSI-approved but not copyleft). Check it before relinking.
- Size: on the order of 10 kLOC core + examples + tests.
- Dependencies: PyCA `cryptography` and `pyserial`. A pure-Python
  fallback is packaged as `rnspure` with no external deps.
- Status: 5.6 k stars, ~2500 commits, 100+ releases; current version
  1.1.9 (April 2026). Actively maintained by Mark Qvist as
  near-solo upstream with ecosystem contributors.
- Wire format and API are declared stable; the Python source is the
  spec. There is no separate RFC-style document.

Practical read: if you want to know what a feature should do, read
the Python. The manual explains intent and the reference implements
it; where they disagree, the code wins.

### 2.2 Language ports

All of these are community efforts. Mark's stated plan (issue #21 on
the upstream repo) was to eventually produce an official C port, but
that hasn't happened — he has said he considers the API and wire
format stable enough for ports to track.

**attermann/microReticulum (C++)**

- License: Apache-2.0.
- Target: 32-bit MCUs, primarily ESP32 / ESP32-S3 family, also nRF52.
- Build: PlatformIO + CMake. Not set up for ESP-IDF out of the box.
- Crypto: `rweather/Crypto` (Arduino-ecosystem library) — not
  mbedTLS. This is the single biggest friction point for integrating
  into an ESP-IDF project that already links mbedTLS.
- Deps: ArduinoJson, MsgPack, rweather/Crypto — all declared as hard
  dependencies in the library manifest.
- Status (April 2026): ~260 stars, ~150 commits, release 0.3.0. A few
  open issues/PRs. Active but a one-developer project. Most of the
  protocol core is implemented: identities, destinations, packets,
  UDP interface, basic transport, path table persistence via an
  abstracted filesystem layer (microStore). Roadmap items still
  open include full Link + Resources support, AES-256 (they had
  AES-128 first), ratchets, Channel, and Buffer.
- Memory: configurable allocators (heap, PSRAM, TLSF pool). Explicit
  nod to constrained platforms (28 KB flash budget on some nRF52
  variants). No specific footprint number published.
- Intended use: library, not an app. The author ships a separate
  firmware project (see below) that uses it.
- Quality: competent, not battle-tested. A port of a still-evolving
  reference is always going to lag; treat it as a starting point,
  not a drop-in RNS replacement.

**varna9000/microReticulum_Firmware and awkaplan/microReticulum_Firmware**

- Forks of RNode_Firmware that link microReticulum on-device, turning
  an RNode-style board into a standalone transport node (LoRa
  interface, no Python host required).
- C++/C + some Python for tooling.
- GPLv3. Active-ish; both are derivatives of the RNode firmware tree
  and inherit its license.
- Good reference for "how does one wire microReticulum into a real
  FreeRTOS-ish build."

**BeechatNetworkSystemsLtd/Reticulum-rs (Rust)**

- License: MIT. ~260 stars, 150 commits, ~30 open issues, ~24 open
  PRs. Active.
- Targets embedded + constrained, tactical radios, UAVs. Advertised
  for "sub-GHz transceivers."
- Uses `protoc` which suggests protobuf-serialized configuration at
  some boundaries; that's a non-standard choice relative to the
  Python reference (which uses configobj + msgpack) and warrants a
  closer look if you care about strict wire-format parity.
- `no_std` support is not explicitly claimed; this one is more
  "lightweight and modular" than "truly microcontroller-ready out
  of the box."

**Lew_Palm/leviculum (Rust)**

- License: AGPL-3.0-or-later — significant for embedded firmware.
  If you ship a device running a modified copy of leviculum and it
  communicates with users over a network, the AGPL obligations are
  triggered. This is usually incompatible with a proprietary camera
  product; reasonable for an open-source project.
- Workspace layout: `reticulum-core` (the no_std + alloc core),
  `reticulum-std`, `reticulum-nrf` (LoRa / nRF radio), `reticulum-
  ffi`, `reticulum-proxy`, plus CLI tools `lnsd` (daemon), `lncp`
  (copy), `lns` (status).
- Claims feature-complete protocol: routing, path discovery, link
  establishment, encrypted channels, segmented transfer, forward
  secrecy ratchets, LoRa radio. Claims interop against Python RNS on
  real hardware.
- Not declared production-ready but is the most complete non-Python
  implementation by a clear margin.
- ~530 commits, active April 2026.
- Quality read: this is the implementation to watch if you want a
  Rust path. The AGPL is the only real blocker for most commercial
  embedded use.

**svanichkin/go-reticulum (Go)**

- License: MIT. ~40 stars, ~110 commits. Actively tracked against
  Python 1.1.5. Bundled CLI utilities (rncp, announce, interface
  examples). Author discloses substantial AI-assisted authorship.
- Does not support Python's external-interface plugin mechanism
  (would require loading .py modules at runtime).
- WIP; "unstable areas may still exist." Not an embedded target at
  all (Go runtime is wrong for ESP32-class hardware).
- Useful if you want a Linux daemon in Go or a test node on a
  server.

**ExReticulum (Elixir)** — a small Elixir port exists on hex.pm at
v0.2.0. Essentially experimental.

**JS/TypeScript** — no serious browser-native port. The upstream
guidance (discussion #128) is to run `rnsd` locally and talk to it
via a local socket (SAM-style), then provide a JS library around that
local port. This is what `rBrowser` and MeshChat do: a Python backend
speaks RNS, a JS front-end speaks to the backend. A pure browser RNS
has never shipped.

### 2.3 LXMF: the messaging layer

**markqvist/LXMF**

- Language: Python. License: MIT (see repo — confirm per release).
- "Lightweight Extensible Message Format." Minimal overhead: claimed
  111 bytes of metadata per message.
- Message structure: destination, source, Ed25519 signature, and a
  msgpack payload containing timestamp, content, optional title, and
  a field dictionary. Message ID = SHA-256 over (dest, source,
  payload).
- Delivery methods:
  - **Opportunistic** — one-shot unencrypted-envelope single-packet
    delivery where it fits. (The LXMF payload is still encrypted
    end-to-end by the RNS SINGLE destination type.)
  - **Direct** — set up a Link and push the message through a
    Channel with forward-secret ephemeral AES-128. This is the path
    used when both endpoints are simultaneously reachable.
  - **Propagation nodes** — store-and-forward. A propagation node is
    a Reticulum transport node that also runs the LXMF propagation
    service: it accepts inbound messages for any destination, stores
    them, and delivers them when the recipient announces or asks.
    Propagation nodes peer with each other in a pull-based
    mesh, building a distributed encrypted message store.
- Status: declared beta / experimental by the author. Works in
  practice; widely deployed.
- Messages can be encoded as QR codes ("paper messaging") for
  air-gapped transport.

If you ship one messaging integration on seccam, this is the one.

### 2.4 LXST: real-time signal / voice

**markqvist/LXST**

- Language: Python (94%). License: CC BY-NC-ND 4.0 — non-commercial
  and no derivatives. That is a hard blocker for integration into a
  commercial product and for any modified port; do not plan around
  LXST for anything non-trivial until the license changes.
- "Lightweight Extensible Signal Transport." Real-time audio on RNS:
  zero-conf routing, per-link forward secrecy, end-to-end latencies
  quoted below 10 ms in good conditions.
- Codecs: Opus at 4.5–96 kbps (voice/podcast/music profiles),
  Codec2 at 0.7–3.2 kbps for LoRa-class links, and raw PCM up to 32
  channels / 128-bit samples. Supports mid-call codec switching.
- Status: β 0.4.4 as of Nov 2025, ~95 stars. Author-declared "very
  early alpha."

### 2.5 User-facing applications

| Program | Language | License | UI | Focus |
|---------|----------|---------|----|----|
| **Nomad Network** (markqvist/NomadNet) | Python 99.8% | GPL-3.0 | Terminal / text | All-in-one mesh client: LXMF messaging, page node, Micron-markup text browser. 2.1k stars, actively maintained. |
| **Sideband** (markqvist/Sideband) | Python 93.8% + Kivy | CC BY-NC-SA 4.0 | Kivy GUI | LXMF + LXST client for Android/Linux/macOS/Windows. Voice calls, file transfer, maps, telemetry, plugins. Mature. |
| **Reticulum MeshChat** (liamcottle) | JS/Vue + Python | MIT | Electron / web | Modern UI, audio calls via Codec2, interops with Sideband/NomadNet. Mostly active. |
| **MeshChatX** | — | — | Web/Electron | Fork of MeshChat with LXST support, voicemail, phonebook. |
| **Columba** | Kotlin | — | Android Material 3 | LXMF messaging app for Android. |
| **Reticulum Relay Chat (RRC)** | Python | — | Terminal | IRC-style live group chat on RNS. |
| **RetiBBS** | Python | — | Terminal | BBS-style bulletin board. |
| **rBrowser** (fr33n0w) | Python + web | — | Local web UI | Cross-platform browser for NomadNet pages. |
| **Retipedia** | Python | — | Page node | Serves ZIM archives (Wikipedia etc.) to NomadNet clients. |
| **RNS FileSync** | Python | — | Daemon | Server-less file sync. |
| **LXMFy** | Python | — | Framework | Bot framework for LXMF. |
| **LXMF Interactive Client** | Python | — | Terminal | Terminal LXMF messenger. |
| **LXST Phone / rnphone** | Python | CC BY-NC-ND 4.0 | CLI + desktop | Voice-call apps on LXST. |

### 2.6 CLI utilities (shipped with RNS)

- **rnsd** — daemon. Keeps RNS and its interfaces up as a long-lived
  service. Most setups run this.
- **rnstatus** — dumps interface state, reachability, stats.
- **rnpath** — query / edit the local path table.
- **rnprobe** — reachability probe to a destination.
- **rncp** — file copy over RNS (think `scp` over mesh).
- **rnx** — remote command execution ("run and fetch output").
- **rnsh** — full interactive remote shell (the SSH analog). Separate
  package (markqvist/rnsh, MIT).
- **rnid** — identity management.
- **rnodeconf** — flash / configure RNode firmware on LoRa boards.

### 2.7 Auxiliary tooling

- **RNMon** — InfluxDB metrics exporter for RNS nodes.
- **Micron Parser JS** — JS parser for the Micron markup used by
  NomadNet pages.

---

## 3. Hardware

### 3.1 RNode firmware

**markqvist/RNode_Firmware** — canonical firmware that turns a LoRa
board into a "radio peripheral" that a host (running RNS) drives over
USB serial via a KISS-style framing. GPLv3. 571 stars, 871 commits,
v1.86 released April 2026. Declared a "public mirror" — bug fixes and
security only. New development has moved to:

**liberatedsystems/RNode_Firmware_CE** — Community Edition. GPLv3,
212 stars, ~1020 commits, v1.75 released mid-2025. Adds hardware
support and accepts contributions the upstream no longer takes.

Between these two, CE is the one to target for new hardware support.
Upstream is the stable reference.

Supported boards (both firmwares, in combination):

- **ESP32-based**: LilyGO T-Beam (several SX12xx variants), T-Beam
  Supreme, T3S3, LoRa32 v1.0/v2.0/v2.1, T-Deck, plus Heltec LoRa32
  v2.0/v3.0/v4.0, Unsigned RNode v2.x, SeeedStudio XIAO ESP32S3
  (yes — the seccam board hardware sibling, with a LoRa module
  added).
- **nRF52-based**: RAK4631, LilyGO T-Echo, Heltec T114, OpenCom XL
  (dual-transceiver SX1262 + SX1280 for simultaneous sub-GHz +
  2.4 GHz).
- **Commercial**: openCom XL, Unsigned handheld v2.x RNodes.

Transceivers: SX1276/78 (first-gen), SX1262/68 (current mainstream),
SX1280 (2.4 GHz). All SPI-attached.

Operating bands: 433 MHz, 868 MHz, 915 MHz, 2.4 GHz — ISM bands, raw
LoRa (not LoRaWAN).

### 3.2 Standalone firmware variants

**microReticulum_Firmware** (varna9000, awkaplan) — runs an RNS
transport node *on the MCU*, so the board itself is a participant
rather than a radio peripheral. Requires more flash and RAM; the
nRF52 variant is tight (28 KB filesystem). ESP32-S3 with PSRAM is
comfortable.

### 3.3 DIY and design

Meshnets, Unsigned.io shop kits, and a design-your-own guide from
the Reticulum community site are the usual starting points. The
hardware bill of materials for a minimum RNode is: an MCU with SPI,
a Semtech LoRa transceiver, a TCXO if you want to run high spreading
factors reliably, an antenna matched to band, and USB-serial for the
host connection. That is it.

### 3.4 ESP32-S3 / seccam relevance

The XIAO ESP32S3 (Sense board variant of which is the seccam host) is
supported by RNode_Firmware_CE as a LoRa-less plain-ESP32 variant and
is supported by microReticulum as a PlatformIO target. It has 8 MB
PSRAM and 8 MB flash, which is comfortable by embedded-RNS standards.
The catch: Sense board has no LoRa module — remote-access interfaces
would have to be WiFi-based (UDP/TCP/I2P) unless a LoRa add-on is
added.

---

## 4. Quality assessment

Read these with the caveat that the ecosystem is relatively small
(one or two developers per project, in most cases) and quality varies.

**markqvist/Reticulum (core)** — the most polished piece. Code is
readable Python, API is stable, test coverage is OK, the author is
active and responsive. The Reticulum License is not OSI-approved but
is permissive for most practical uses (derivative Reticulum stacks
are explicitly allowed). No external security audit; that's the
biggest caveat on depending on it in adversarial settings. For
non-adversarial mesh use, it's production-grade enough that people
run large public networks on it.

**LXMF** — mature enough to bet on, author-declared beta. Wire format
is stable. If you want one messaging protocol to carry, this is it.

**LXST** — interesting, not licensable for most integrations. Treat
as research-grade.

**microReticulum** — the right starting point for C++ on MCUs but
lags the reference. You will hit "that feature isn't implemented yet"
on resources and ratchets. Treat as 70% of the stack; expect to fill
gaps. Also: the Arduino ecosystem assumptions (rweather/Crypto,
ArduinoJson, PlatformIO) are a non-trivial integration cost for an
ESP-IDF project.

**leviculum (Rust)** — most complete non-Python port. AGPL-3.0-or-
later. Not embedded-friendly unless you want Rust in your build.
Leviculum is also the most honest about its limitations ("not yet
production-ready, but functionally complete").

**Reticulum-rs (Rust)** — earlier-stage than leviculum. MIT is nicer
for commercial use but the completeness gap is real.

**go-reticulum** — competent WIP, server-side only.

**Ecosystem apps** — quality varies wildly. NomadNet and Sideband are
mature. MeshChat is mature. Half the other apps are one-person
hobby projects — fine for what they are, but not infrastructure.

**RNode firmware** — stable, many deployments, well-characterized
behavior. The CE fork is where new hardware lands.

Community health: the upstream author is active, there's a Matrix
room and a forum (discuss.reticulum.network), and release cadence is
steady. It's not an abandoned codebase by any measure, but it's not
Linux-kernel levels of contributor diversity either. If Mark stopped
tomorrow, the ports and the Liberated Systems fork would keep it
alive, but momentum would slow visibly.

---

## 5. Security posture

What Reticulum does well:

- Every `SINGLE` or `LINK` packet is end-to-end encrypted and
  authenticated. No cleartext content at the network layer.
- Ephemeral keys on every link give forward secrecy.
- Announce ratchets let destinations rotate the key material used for
  new opportunistic SINGLE packets (30-day default expiry in the
  reference).
- No source addresses in packets — network-layer initiator
  anonymity. An observer on the path sees "someone sent a packet
  with this destination hash" but not who.
- Proof-of-work stamps on announces rate-limit announce floods.

What Reticulum does not do:

- No traffic-analysis resistance beyond source-address elision. Link
  establishment and packet sizes are observable.
- No key revocation protocol. You rotate identity, which changes
  your destinations, which means consumers have to re-announce.
- No formal external security audit.
- AES-CBC+HMAC rather than AEAD. Fine if implemented correctly, but
  a source of subtle mistakes in ports.
- IFAC (Interface Access Code) gives symmetric interface-level
  authentication, not per-message. It's the equivalent of a WiFi
  pre-shared key on a given interface; useful, but not a substitute
  for end-to-end trust decisions.

---

## 6. Pointers for deeper reading

Core docs:

- Site: https://reticulum.network/
- Manual: https://markqvist.github.io/Reticulum/manual/
- Crypto primitives page: https://reticulum.network/crypto.html
- Hardware page: https://reticulum.network/hardware.html
- Getting started: https://reticulum.network/start.html

Reference implementation and discussions:

- https://github.com/markqvist/Reticulum
- Port-to-C discussion: https://github.com/markqvist/Reticulum/discussions/21
- Browser support: https://github.com/markqvist/Reticulum/discussions/128
- Awesome list: https://github.com/markqvist/Reticulum/wiki/Awesome-Reticulum
- FAQ: https://github.com/markqvist/Reticulum/wiki/Frequently-Asked-Questions

Embedded / alternate-language:

- microReticulum: https://github.com/attermann/microReticulum
- Leviculum (Rust): https://codeberg.org/Lew_Palm/leviculum
- Reticulum-rs (Rust): https://github.com/BeechatNetworkSystemsLtd/Reticulum-rs
- go-reticulum: https://github.com/svanichkin/go-reticulum

Hardware:

- RNode_Firmware (reference): https://github.com/markqvist/RNode_Firmware
- RNode_Firmware_CE (Liberated Systems): https://github.com/liberatedsystems/RNode_Firmware_CE
- microReticulum_Firmware: https://github.com/awkaplan/microReticulum_Firmware

Applications:

- LXMF: https://github.com/markqvist/LXMF
- LXST: https://github.com/markqvist/LXST
- NomadNet: https://github.com/markqvist/NomadNet
- Sideband: https://github.com/markqvist/Sideband
- MeshChat (liamcottle): https://github.com/liamcottle/reticulum-meshchat
- rnsh: https://github.com/markqvist/rnsh

Community:

- Forum: https://discuss.reticulum.network/
- Matrix: #reticulum:matrix.org (see site for current handle)
- Community wiki: https://reticulum.miraheze.org/

---

## 7. One-paragraph verdict

Reticulum is the most interesting "full stack that is not IP" design
shipping today. The reference implementation works, the cryptography
is reasonable and buildable on any platform with mbedTLS, and the
application layer (LXMF especially) is actually usable. The
ecosystem's weaknesses are exactly what you'd expect for a one-author
protocol: limited external review, a handful of half-finished ports,
license choices that complicate commercial integration in spots
(LXST, leviculum). For an embedded project on ESP32-S3, the realistic
options are "use microReticulum and live with the gaps" or "write a
targeted subset of the protocol directly against mbedTLS." Which
to pick depends on how much of Reticulum you actually need.
