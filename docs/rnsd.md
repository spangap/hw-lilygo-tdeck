# rnsd — RNS protocol task

`rnsd.cpp/h` — owns mR's `Reticulum` + `Transport`, the identity, the
iface table (proxy `Interface` objects for each connected transport),
plus a byte-array C-style API ([rnsd.h](../main/rnsd.h)) that
encapsulates **every** mR primitive consumers need: SHA-256, sign /
verify, destination-hash derivation, identity create/erase/recall,
async path request, the bidirectional mailbox port, and an announce
fan-out port. Downstream tasks (`lxmf`, future tools) operate on raw
byte arrays and storage sentinels — they never include `RNS::Identity`,
`RNS::Bytes`, `RNS::Destination`, or `RNS::Transport` directly. mR
could be swapped out behind `rnsd.h` without changing a single
consumer line.

**Zero networking/radio dependencies** — rnsd includes neither lwIP
nor RadioLib; transports show up as ITS server connections on
`RNSD_PORT_TRANSPORT` and feed RNS packets in/out as packet-mode
bytes.

Core 0, priority 2, 12 KB PSRAM stack. Allocates the iface and
mailbox-connection tables in PSRAM via `heap_caps_malloc` +
placement-new (`RNS::Interface` / `RNS::Destination` / `RNS::Identity`
have non-trivial constructors).

For the persistent / ephemeral / CLI / panel / status-window contracts
this code implements, see `component-plan.md` §11.

## Public API — rnsd.h

Byte-array primitives, callable from any task. Pure-crypto helpers run
inline on the caller's task (mbedTLS is thread-safe; Identity objects
are local to each call). The state-touching ones (`rnsdRecallPubkey`,
`rnsdRequestPath`) either take a mutex around mR's
`_known_destinations` or cross to rnsd's task via a storage sentinel.

```cpp
// Pure crypto (caller-task safe):
void rnsdSha256(const uint8_t* data, size_t n, uint8_t out[RNSD_HASH_LEN]);
bool rnsdDestinationHash(const char* identity_key,
                         const char* app_name, const char* aspect,
                         uint8_t out[RNSD_DEST_HASH_LEN]);
bool rnsdSign            (const char* identity_key,
                          const uint8_t* data, size_t n,
                          uint8_t out_sig[RNSD_SIG_LEN]);
bool rnsdVerify          (const uint8_t pubkey[RNSD_PUBKEY_LEN],
                          const uint8_t* data, size_t n,
                          const uint8_t sig[RNSD_SIG_LEN]);

// Identity management (caller-task safe):
bool rnsdIdentityGenerate(const char* identity_key);
bool rnsdIdentityExists  (const char* identity_key);
bool rnsdIdentityHash    (const char* identity_key, uint8_t out[RNSD_IDENT_HASH_LEN]);
void rnsdIdentityErase   (const char* identity_key);

// mR-state access (mutex-protected / async via sentinel):
bool rnsdRecallPubkey    (const uint8_t dest_hash[RNSD_DEST_HASH_LEN],
                          uint8_t out_pubkey[RNSD_PUBKEY_LEN]);
void rnsdRequestPath     (const uint8_t dest_hash[RNSD_DEST_HASH_LEN]);

// Destination / link client API — wraps itsConnect on the right port
// with the right connect payload, so callers don't construct structs:
int  rnsdDestOpen        (const char* aspect, const char* identity_key,
                          uint8_t dest_type, int ref,
                          void (*on_recv)(int, size_t),
                          void (*on_disconnect)(int));
int  rnsdLinkOpen        (const uint8_t dest_hash[RNSD_DEST_HASH_LEN],
                          const char* aspect, const char* identity_key,
                          uint32_t timeout_ms, int ref,
                          void (*on_recv)(int, size_t),
                          void (*on_disconnect)(int));
bool rnsdDestListenLinks (int dest_handle, uint16_t target_port);
```

Implementation notes:

- `identity_key` everywhere is a **storage path** (e.g.
  `"secrets.lxmf.id.0.privkey"`), not the bytes themselves. rnsd loads
  the 128-hex private key from that path on each call. Consumers refer
  to identities by their storage location; the keys-by-reference
  convention from the plan extends to this API.
- `rnsdRecallPubkey` looks up by destination hash in mR's
  `_known_destinations`. Spangap adds a `std::recursive_mutex` around
  every read+write site in `Identity.cpp` so cross-task lookup is
  race-free. rnsd's own internal callers also pay the (cheap)
  uncontended-lock overhead.
- `rnsdRequestPath` writes the self-clearing sentinel
  `rnsd.cmd.request_path = <32-hex>`; rnsd's task subscribes to that
  key and calls `Transport::request_path` on its own task (mR's
  request_path silently drops outbound packets when called from any
  other task — fixed by routing through the sentinel).
- `rnsdDestOpen` is the right entry point for hosting a destination
  (LXMF, rnprobe, custom apps). Replaces hand-rolled
  `itsConnect("rnsd", RNSD_PORT_DEST, &req, …)` with a typed signature.
  Same pattern as net's TCP_DIAL or web's path registration.
- `rnsdLinkOpen(dest_hash, aspect, identity_key, tag, path_timeout_ms,
  ref, on_recv, on_disconnect)` is the **live** outbound Link API
  (Phase C). It accepts immediately and establishes the Link
  asynchronously; the caller watches `rnsd.links.<tag>.*` (storage,
  browser-synced) for `state` → `active`/`failed`. The handle is
  packet-mode (one Link plaintext per send/recv, no framing).
  `rnsdLinkTeardown(tag)` is the explicit close (bare `itsDisconnect`
  only *parks* the Link for `s.rnsd.link.orphan_ttl_s` per §10a.1).
- `rnsdDestListenLinks(dest_handle, inbox_port)` is the **live** inbound
  Link API (Phase D): sends `RNSD_DEST_LINK_LISTEN` on the existing
  `RNSD_PORT_DEST` handle; rnsd flips `accepts_links(true)` on that
  destination and, on each accepted inbound Link, back-connects
  `itsConnectByTaskHandle` to the owning task's `inbox_port` with an
  `rnsd_link_incoming_t`. Inbound links share the Phase-C slot table /
  state tree (`direction=in`, tag `in.<8hex>`).
  See [docs/plans/link.md](plans/link.md) for the full design.

## ITS port surface

Open as `packetBased=true` servers in `rnsdTaskMain` — one ITS packet
in = one RNS-format byte string. See [ports.h](../main/ports.h).

| Port | Constant | Purpose |
|---|---|---|
| 1 | `RNSD_PORT_TRANSPORT` | Transport registration. Connect payload `rnsd_transport_t` describes the iface (name, MTU, bitrate, mode, in/out/fwd/rpt). Stream then carries inbound packets to rnsd and outbound packets from rnsd. Disconnect = deregister. 4 KB recv buffer per handle (bumped from 2 KB to absorb announce bursts from busy testnets). |
| 4 | `RNSD_PORT_DEST` | Bidirectional destination API. Consumers open via `rnsdDestOpen(aspect, identity_key, dest_type, …)` from rnsd.h — the connect payload struct is rnsd-private. rnsd registers an IN destination and bridges in/out packets to the client via opcode-framed frames (`OUT_PACKET` / `OUT_RESULT` / `OUT_CANCEL` / `IN_PACKET` / `OUT_STATUS` / `ANNOUNCE`). See [lxmf.md](lxmf.md) for the consumer side. |
| 6 | `RNSD_PORT_ANNOUNCES` | **Announce fan-out (rnsd → subscribers).** Consumers connect with an optional aspect filter (`rnsd_announces_connect_t`); rnsd registers a single internal `AnnounceHandler` with empty filter at boot and forwards each received announce as one packet-mode ITS frame (`hops(1) \| dest_hash(16) \| identity_hash(16) \| app_data(N)`). Unidirectional (toSize=0); slow consumers drop announces with `itsSend(.., 0)` — they never block rnsd. |
| 10 | `RNSD_PORT_LINK` | Outbound Reticulum Link conns (`rnsdLinkOpen`). Packet-mode data path (no framing); rnsd establishes the Link async and bridges plaintext both ways. Out-of-band aux `RNSD_LINK_AUX_TEARDOWN` (payload = tag) = explicit close. Per-link state at `rnsd.links.<tag>.*`. Phase C — see [docs/plans/link.md](plans/link.md). |
| 2, 3, 5 | MAP, CTL, DGRAM | Reserved per plan §9 — not opened yet. |

The legacy `RNSD_PORT_REGISTER` / `rnsd_register_t` names were renamed
to `RNSD_PORT_TRANSPORT` / `rnsd_transport_t` to reflect what the port
is for. The previous `RNSD_PORT_PACKET` / `rnsd_packet_connect_t`
shape was replaced by `RNSD_PORT_DEST` (bidirectional, opcode-framed);
see `lxmf.md` for the frame format.

The `RNSD_PORT_DEST` connect payload (`rnsd_dest_connect_t`) is
declared inside `rnsd.cpp` — no consumer constructs it. Callers go
through the typed `rnsdDestOpen()` helper in rnsd.h, which fills the
struct from named args. Same pattern as net's TCP_DIAL or web's path
registration.

Connect-payload structs no longer pad to exactly 96 bytes. They're now
declared at their natural size and `static_assert`-ed `<=
ITS_MAX_MSG_DATA` (which itself was bumped to 320 in spangap-core).

## Identity (`secrets.rnsd.identity`)

128-hex-char (64-byte) Curve25519+Ed25519 private key. Loaded on boot
by `loadOrCreateIdentity()`; if absent or malformed, a fresh `Identity`
is generated and its private key written back. Hex serialization round-
trips through `RNS::Bytes::assignHex` / `toHex`.

CLI:
- `rnsd identity` — print hash + public key.
- `rnsd reload` — reload from storage (used after `set secrets.rnsd.identity …`).
- (Plan §11.3 also lists `rnsd identity import|rotate` — not wired yet.)

## TaskInterface — proxy `Interface` per connected transport

```cpp
class TaskInterface : public RNS::InterfaceImpl {
    void send_outgoing(const RNS::Bytes& data) override;  // → itsSend(handle, …)
};
```

One `iface_t` slot per connected transport (`RNSD_MAX_IFACES = 16`).
On connect:
1. `onTransportConnect` copies the `rnsd_transport_t` into the slot.
2. Constructs a `TaskInterface` wrapping the ITS handle, with the
   transport's MTU / bitrate / mode / in/out/fwd/rpt copied across.
3. Wraps in `RNS::Interface(impl)` (shared_ptr) and calls
   `RNS::Transport::register_interface(...)`, so announces and paths
   route through us.
4. Publishes `rnsd.ifaces.<name>.{up,mtu,bitrate,mode}` to ephemeral
   storage.

Inbound: `onTransportRecv` reads one ITS packet (`itsRecv`, max
600 B — RNS MTU 500 + slack), bumps counters, then
`iface.handle_incoming(data)` which routes through
`InterfaceImpl::handle_incoming` → `Transport::inbound` and updates the
path table.

Outbound: when mR's Transport calls `send_outgoing(data)` on the
proxy, we `itsSend` the bytes back over the same handle. Counters bump
on the rnsd side; the transport task counts on the radio/socket side.

`onTransportDisconnect` calls `Transport::deregister_interface` and
clears the slot. The transport task is responsible for reconnecting if
it wants to come back (each task watches its own `s.<name>.enable`).

## Mailbox API (`RNSD_PORT_DEST`)

Replaces the older single-shot `RNSD_PORT_PACKET`. Clients open via
`rnsdDestOpen(aspect, identity_key, dest_type, ref, on_recv, on_disconnect)`
— rnsd loads the listener identity, constructs an IN destination on
that aspect (mR's `Destination` ctor auto-registers with Transport),
and from that point on the handle carries both directions framed by
an opcode byte:

| Opcode | Direction | Payload |
|---|---|---|
| `0x01 OUT_PACKET`   | client → rnsd | `send_id(2)` + payload bytes (LXM wire for lxmf) |
| `0x02 OUT_RESULT`   | rnsd → client | `send_id(2)` + `status(1)` + `rtt_ms(4 BE)` + `hops(1)` |
| `0x03 OUT_CANCEL`   | client → rnsd | `send_id(2)` |
| `0x04 IN_PACKET`    | rnsd → client | decrypted plaintext for the destination |
| `0x05 OUT_STATUS`   | rnsd → client | `send_id(2)` + `type(1)` + tail (progress narration: requesting path, path known, egress queued, retry, etc.) |
| `0x06 ANNOUNCE`     | client → rnsd | `app_data` bytes (may be empty) — triggers a Destination::announce on the listener |

State machine, retries, path tracking all live inside rnsd's task —
clients hand over an opcode-framed byte stream and consume status
updates by reading from the same handle. `RNSD_MAX_MAILBOX_CONNS = 4`.

**Opportunistic wire-format convention.** For SINGLE destinations, the
LXMF (and similar) wire on the network omits the leading 16-byte
destination hash — `LXMessage.py:631` does
`Packet(dest, self.packed[16:])` on send, and `LXMRouter.py:1827-1830`
does `lxmf_data = packet.destination.hash + data` on receive. To keep
consumers' frames symmetric and self-contained, rnsd's `OUT_PACKET`
and `IN_PACKET` carry the **full** wire (`dest || src || sig ||
packed`) — rnsd strips the leading 16 bytes before placing it in the
Reticulum Packet payload on send, and prepends `packet.destination.hash`
back before forwarding on receive. Consumers never see the half-wire
state on the actual network.

## Link API (`RNSD_PORT_LINK`) — Phase C

Outbound Reticulum Link as an ITS connection. `rnsdLinkOpen()` fills
the rnsd-private `rnsd_link_connect_t` (dest_hash, aspect,
identity_key, caller `tag`, path_timeout) and `itsConnect`s
`RNSD_PORT_LINK`. The connect **accepts immediately** — Link
establishment (path request → LR → LRPROOF → key derivation) is
seconds-scale and runs async on the rnsd task; blocking the connect
would be the wrong shape.

- `s_link_conns[RNSD_MAX_LINK_CONNS=8]`, 4096/4096 packet-mode buffers.
- `onLinkConnect`: validate tag (non-empty, unique), publish the
  initial `rnsd.links.<tag>.*` tree, then either kick the Link off
  (identity recallable) or `request_path` + `state=awaiting_path`.
- `linkKickoff`: build OUT `Destination` + `RNS::Link`, wire the
  established/closed/packet thunks, `state=establishing`.
- **Slot↔Link** is resolved by *shared-LinkData pointer identity* —
  `sameLink(a,b) = a&&b && !(a<b)&&!(b<a)` over mR's public
  `operator<`. mR hands callbacks `Link` wrapper *copies* (different
  address, same `shared_ptr<LinkData>`), so `&slot->link==&link` can't
  work. The packet callback has no `Link&`; `packet.link()` is stamped
  by Transport before `link.receive()`.
- **Pre-active outbox**: one packet buffered before `active`, flushed
  on the established callback (drop-newer, `last_error=send_queue_full`).
- `linkTick()` (1 Hz, beside `mailboxTickPending`): no-path deadline,
  establishment-timeout safety, `orphan_ttl` cull of consumer-detached
  Links, terminal-state grace reclaim (publish `closed` so subscribers
  see it, then `storageDeleteTree` + free).
- **Teardown matrix**: remote close → `onLinkClosedCb` → reclaim;
  consumer `itsDisconnect` → *park* `orphan_ttl_s` then teardown
  (§10a.1, in-session reconnect is deferred); explicit
  `rnsdLinkTeardown(tag)` → aux `RNSD_LINK_AUX_TEARDOWN` → `onLinkAux`
  closes + frees the slot/tag now.
- Per-link storage (`rnsd.links.<tag>.{state,direction,aspect,
  remote_hash,link_id,mtu,rtt_ms,tx_packets,rx_packets,opened_s,
  activated_s,last_*,last_error}`) is the status channel — browser
  picks it up via storage's empty-prefix sync; rnsd ships **no** Link
  aux for status by design.

The `clink` task (`rnsd clink <hash> [aspect] | send <t> | close |
listen <aspect>`) is a real `RNSD_PORT_LINK` / `RNSD_PORT_DEST`
ITS-client test consumer — distinct from the Phase-B `rnsd link` probe,
which builds `RNS::Link` on the rnsd task.

### Inbound Links (Phase D — `RNSD_DEST_LINK_LISTEN`)

A consumer holding an `RNSD_PORT_DEST` handle sends opcode
`RNSD_DEST_LINK_LISTEN` (payload: `inbox_port` 2 BE) via
`rnsdDestListenLinks()`. rnsd records `(itsRemoteTask(handle),
inbox_port)` on the mailbox conn and flips `accepts_links(true)` +
sets `onIncomingLinkEstablished` as the IN destination's
`set_link_established_callback` (mR fires `_owner.callbacks()
._link_established` at [Link.cpp:539] when an inbound Link reaches
ACTIVE). That callback resolves the owning mailbox conn by
`link.destination().hash()` (set by `validate_request`), slots the
Link into the shared `s_link_conns` table (`direction=in`, tag
`in.<8hex>`, Phase-C packet/closed thunks reused), and
`itsConnectByTaskHandle`s back to the consumer with an
`rnsd_link_incoming_t` (tag, link_id, remote_identity_hash,
local_dest_hash, mtu). Consumer→link sends and consumer-detach reuse
`onLinkRecv`/`onLinkDisconnect`. **rnsd must `itsClientInit()`** for
this back-connect — `rnsdTaskMain` does both server and client init.
Hosted destinations are surfaced at `rnsd.mailbox.<idx>.{aspect,dest}`.

## Announce fan-out (`RNSD_PORT_ANNOUNCES`)

Single internal `AnnounceFanout : RNS::AnnounceHandler` with empty
mR-side filter, registered after `Reticulum::start()`. Every announce
mR sees fires `received_announce`, which:

1. Builds the wire frame once
   (`hops(1) | dest_hash(16) | identity_hash(16) | app_data(N)`).
2. Iterates subscriber slots; if a slot has a non-empty aspect
   filter, applies the same predicate mR uses for
   `AnnounceHandler` subclasses (`Destination::hash_from_name_and_identity`).
3. Forwards via `itsSend(handle, frame, n, 0)` — drop-on-full, never
   blocks rnsd.

`RNSD_MAX_ANNOUNCE_SUBS = 4`. Subscribers connect with
`rnsd_announces_connect_t { aspect[32] }`; empty aspect = receive
everything. lxmf uses this with `aspect="lxmf.delivery"` to drive
its announce catalogue of heard mailboxes.

## Management destination

rnsd hosts `rnstransport.remote.management` on its own transport
identity (`s_identity`), gated by `s.rnsd.remote_management` (default
1). Constructed after `Reticulum::start()` if enabled; the gate
subscription handles runtime flips (`set s.rnsd.remote_management 0`
deregisters the destination; flipping back to 1 rebuilds it). No
packet callback yet — inbound requests silently land and drop.

This is the conventional Reticulum admin/status endpoint. Upstream
Reticulum exposes `/status` and `/path` Request handlers here
([Transport.py:253-255](../research/Reticulum/RNS/Transport.py#L253-L255))
backing `rnstatus -R <hash>` and `rnpath -R <hash>`. Those handlers
ride on Reticulum **Link**, not opportunistic SINGLE — wiring them up
is gated on Link support landing. See
[docs/plans/link.md §10](plans/link.md#10-phase-g--rnstransportremotemanagement-handlers).

It's bound to rnsd's identity, **not** LXMF's — LXMF identities live
in lxmf and announce under `lxmf.delivery`.

## Probe responder

rnsd optionally hosts `rnstransport.probe` on the same identity, gated
by `s.rnsd.respond_to_probes` (default 0 — opt-in). Constructed with
`accepts_links(false)` (opportunistic SINGLE only) and
`set_proof_strategy(PROVE_ALL)`, so mR auto-proves every incoming
DATA packet without any application code — same shape as upstream
rnsd's probe destination
([Transport.py:397-401](../research/Reticulum/RNS/Transport.py#L397-L401)).

This is the conventional probe endpoint that `rnprobe` defaults to.
With `respond_to_probes=1`, peers can `rnprobe <our-hash>` and round-
trip at the Reticulum protocol layer.

## Announce scheduler

Three announce paths are scheduled from rnsd's 1 Hz tick (when their
underlying destinations are up):

- **Management announce** (rnsd's own `rnstransport.remote.management`):
  debounced 10 s after the last iface-up transition, plus periodic
  re-announce every `s.rnsd.announce.interval` seconds (default 1800).
- **Probe announce** (rnsd's own `rnstransport.probe`): same schedule,
  same identity. Fires alongside management — both share a single set
  of debounce/last-announce timers.
- The **lxmf delivery announce** schedule lives in the lxmf task —
  it subscribes to `rnsd.iface_event_seq` (a counter rnsd bumps on
  every iface-up) and follows the same 10 s debounce + periodic
  pattern.

Iface-up debounce: each new iface-up re-arms the timer to `now + 10 s`.
A burst of ifaces coming up in quick succession produces **one**
announce 10 s after the last one came up. No `attached_interface` —
announce broadcasts on all enabled FULL/GATEWAY ifaces (per-iface
targeting is a future optimization).

The periodic re-announce check uses a **signed `int32_t` compare** on
`now - last_announce_tick` rather than the unsigned `TickType_t`
result. `sendXAnnounce()` updates `last_announce_tick` via a fresh
`xTaskGetTickCount()` *after* the mR announce call (which takes a few
ms); that timestamp can land just past the outer loop's captured `now`.
Unsigned subtraction underflows to ~UINT32_MAX, trivially passes any
interval threshold, and fires the periodic branch immediately after
the debounce fire. Signed cast keeps that case negative.

## Reticulum / Transport startup

In order, inside `rnsdTaskMain`:

```cpp
itsServerInit();
itsServerPortOpen(RNSD_PORT_TRANSPORT, true, RNSD_MAX_IFACES, 4096, 4096);
itsServerOnConnect / OnDisconnect / OnRecv  (register handlers)
itsServerPortOpen(RNSD_PORT_PACKET,   true, RNSD_MAX_PACKET_CONNS, 2048, 512);
itsServerOnConnect / OnDisconnect / OnRecv  (packet handlers)

loadOrCreateIdentity();
storageSet("rnsd.up", 1);
storageSet("rnsd.identity_hash", s_identity->hexhash().c_str());

RNS::Identity::known_destinations_maxsize(1000);  // up from default 100
s_reticulum = std::make_unique<RNS::Reticulum>();
RNS::Reticulum::transport_enabled(storageGetInt("s.rnsd.transport_enabled", 0) != 0);
s_reticulum->start();
RNS::Transport::register_announce_handler(s_announce_logger);
```

The identity cache is bumped from mR's default 100 entries to 1000
(~200 KiB PSRAM). On busy testnet traffic, 100 evicts entries before we
get a chance to probe them — `Identity::recall` then fails inside
`packetConnEnsureResources` even though the path table still has the
route, and rnprobe can't proceed.

`RNS::Reticulum::transport_enabled(...)` is a **static** global that
Transport consults for forwarding decisions. We mirror our persistent
flag (`s.rnsd.transport_enabled`) into it before `start()`.

## Main loop — single wait point, staggered 1 Hz housekeeping

```cpp
for (;;) {
    itsPoll(nextDeadline());          // ITS messages OR deadline.
    if (now - lastPublishTick >= 1s) {
        if (tickPhase == 0) {
            Transport::jobs();        // mR housekeeping
            mailboxTickPending();     // retry pending mailbox sends
        } else {
            publishPathTable();       // RAM-authoritative path table → storage
        }
        publishStats();               // cheap, every tick
        tickPhase ^= 1;
        lastPublishTick = now;
    }
}
```

`nextDeadline` returns the time until the next 1 Hz tick. No polling
drain — the canonical `while (itsPoll(0)) {}` pattern would burn CPU.
ITS callbacks (transport/recv/disconnect, mailbox, announce-fanout)
run inline from `itsPoll`; the 1 Hz block handles snapshots and mR's
own bookkeeping.

**Why staggered.** Running `Transport::jobs()` *and*
`publishPathTable()` in the same tick can park the rnsd task past
tcp's 100 ms `itsSend` timeout on busy networks — see the
[lxmf doc](lxmf.md) for the original symptom (`[tcp] rnsd ITS send
dropped`). Alternating across two ticks keeps the longest
contributors apart, and `Transport::jobs()` still runs at ~2 s
cadence — fast enough for RNS receipt timeouts and announce-queue
drain.

`Transport::jobs()` is mR's announce-queue drain, link/resource state
machine tick, hashlist culling, and path expiry. Originally designed
for per-tick invocation — capping it at 2 s here is the spangap port
deciding for mR.

## Path-table snapshot — `publishPathTable`

mR's `Transport::get_new_path_table()` is the RAM-authoritative path
table; we snapshot up to `RNSD_PATHS_PUBLISH_MAX = 64` entries per tick
into a JSON array at `rnsd.paths` via `storageSetTree`. Each entry:

```json
{ "dest":"<hash hex>", "next_hop_addr":"<neighbor hash>",
  "next_hop":"<iface name>", "hops":N, "last_announce":<epoch_s> }
```

Why the 64 cap:
- Each entry runs ~150 B in the cJSON patch; storage caps patches at
  60 KB.
- Each iteration step copies a `DestinationEntry` whose dtor walks an
  RB-tree of `_random_blobs` in PSRAM — uncapped + busy testnet =
  IDLE0 starvation.

Inside the loop, `vTaskDelay(1)` every 8 entries
(`RNSD_PATHS_YIELD_EVERY`) keeps the watchdog happy.

`ifaceShortName` strips mR's default `Interface[name]` wrapper to a
bare `tcp/0` / `lora` / `tcp_in/<addr:port>` for the browser map.

## Stats snapshot — `publishStats`

Wrapped in `storageBegin()` / `storageEnd()` so one storage:1 patch
gets sent instead of one-per-key:

```
rnsd.stats.{packets_in,packets_out,bytes_in,bytes_out,ifaces_up}
rnsd.ifaces.<name>.{rx_packets,tx_packets,rx_bytes,tx_bytes}
```

Counters are mirrored from `int64_t` into `int` by masking the low 31
bits — `storageSet` is int-typed and we don't need long-term roll-over
precision in the snapshot (the underlying counters stay 64-bit for
print purposes).

## Announce debug logger

`AnnounceDebugLogger : RNS::AnnounceHandler` is registered once at
boot. Every announce mR sees fires `received_announce(dest, id,
app_data)` and we emit a dbg() line (or verb() under `only_local` —
see below). **No persistence** — that's the application layer's job
(LXMF) and would scale terribly on busy TCP-relay traffic (see plan
§19 "announce storms on slow interfaces").

App_data formatting is factored into a shared helper,
`formatAnnounceAppData(const RNS::Bytes& app_data)`, used both by the
incoming-announce logger and the outgoing-announce info() line
("`announced <hash> aspect=<aspect> app_data <body>`"). One pretty-
printer, one format, browser-consistent.

`app_data` is structured differently per announce dialect. The
heuristic walks four shapes in order:
1. Whole buffer parses as one msgpack value → `mp=…`.
2. ≥32 B and bytes 32..end parse as msgpack → 32 B ratchet (X25519
   pubkey) + msgpack payload (Nomadnet pattern).
3. ≥32 B and bytes 32..end are plausible UTF-8/ASCII → 32 B ratchet +
   raw display-name (Sideband/LXMF older).
4. ≥33 B, version-byte + msgpack(1..-32) + 32 B ratchet at the end
   (Reticulum interface advertisements with location etc.).

`mpDecode` is a bounded msgpack pretty-printer — handles fixint /
fixstr / str8/16/32 / fixarray / fixmap / nil / bool / int / uint /
bin / float / fixext / ext, bails on reserved. Depth cap 8, output cap
800 chars, no allocation per node beyond the running string.

At verbose level we also dump the raw `app_data` hex.

### `s.rnsd.debug.only_local`

A bool gate (default 0) that **demotes** announce/path-request
chatter from `dbg()` to `verb()`, so running rnsd at debug level
surfaces just the traffic that affects this node directly (sends,
inbound packets, our own path requests, path requests we answer)
rather than the network's bulk announce + path-request flow.

The flag is cached in `s_dbg_only_local` and live-mirrored via a
storage subscription, so toggling at runtime takes effect on the
next announce. The subscription also pushes the flag into mR via
`RNS::Transport::demote_dbg(bool)` so the high-volume DEBUGFs inside
mR's own announce/path-request code paths
(`Transport::inbound`, `Transport::packet_filter`,
`Transport::path_request`, `Identity::clean_known_destinations`) are
routed through a `DBGF_DEMOTE` / `DBG_DEMOTE` macro that picks
`VERBOSEF` or `DEBUGF` based on the same flag.

A few mR DEBUGFs are *promoted* unconditionally to INFOF instead
because they describe directly-meaningful events:
- `Packet destination <hash> found, destination is local` (for non-
  control destinations) — a DATA packet arrived for one of our IN
  destinations.
- `Answering path request for destination <hash> …, destination is
  local to this system` — we satisfied a path request because we own
  the destination.
- `Answering path request for destination <hash> …, path is known` —
  we satisfied a path request as a transport node.

And one entirely new diagnostic INFOF was added:
- `DATA arrived for dest <hash> (hops=H, NB) — no local destination` —
  a DATA packet reached us but didn't match any registered local
  destination. Useful when "send went out, no reply came back" to
  distinguish "reply never arrived" (no log) from "reply arrived for
  a hash we don't own" (this log).

## CLI

Top-level commands registered in `rnsdInit`. Each parses its own short
options (`-a`, `-s`, `-j`, etc.) using `rnpathNextToken`. All accept
`help` as a positional for inline usage.

### `rnsd [identity|persist|reload]`

Status / control. `rnsd identity` prints hash + pubkey. `rnsd persist
[if-transport]` is the cron entrypoint — currently a no-op stub for
transport-mode flush (paths/hashlist/tunnels TBD); endpoint mode (the
default) silently skips. `rnsd reload` re-runs `loadOrCreateIdentity`.

### `rnstatus [filter] [-t] [-j]`

Iface + traffic. With no args, prints node header (identity, transport
enabled?, ifaces up) and then a per-iface block (status, mode, MTU,
bitrate, traffic). `-t` adds global totals. `-j` emits one JSON object
for scripting. `filter` is a substring match against iface name.

### `rnpath [destination] [-i iface] [-m hops] [-n N] [-a] [-s] [-d] [-j]`

Path table.

- Positional `destination` is a prefix-match against the hex hash.
- `-i iface` restricts to one iface by name.
- `-m hops` caps by hop count.
- `-n N` row limit (default `RNPATH_DEFAULT_LIMIT = 32`), `-a` removes
  it.
- `-s` summary only (counts, no rows).
- `-j` JSON output.
- `-d` drops the path to the destination (`Transport::expire_path`).
  Destructive — requires the positional hash.

Single pass through the path table per invocation: collects matching
rows AND accumulates by-iface / by-hops histograms (printed even when
`-s`). `vTaskDelay(1)` every 16 entries
(`RNPATH_YIELD_EVERY`); iteration copies each `DestinationEntry`, so
bounded yields matter on tables with thousands of paths.

### `rnprobe [aspect] <destination_hash> [-s N] [-n N] [-t S] [-w S]`

Probe a destination, measure RTT. **Implemented as a client of
`RNSD_PORT_PACKET`** so all mR work runs on rnsd's task:

1. Validate hash length (32-char hex, `DESTINATION_LENGTH * 2`).
2. Single-positional shortcut: if the single positional is a valid hex
   hash, treat it as the hash and default aspect to
   `rnstransport.probe` — the conventional probe endpoint hosted by
   any node with `respond_to_probes = yes`. (Earlier versions defaulted
   to `rnstransport.remote.management`; that's the admin/status
   endpoint, not the probe one.)
3. `itsConnect("rnsd", RNSD_PORT_PACKET, &req, sizeof(req), 2s)`.
4. `itsSend` `size` zero bytes (clamped to 0..400).
5. Block on `itsRecv(result, 6, (timeout+2)s)` for the result struct.
6. Print human-readable line per status code.

`-n > 1` accepted but only one probe is sent today.

Why the indirection: `has_path` + `request_path` + `Packet::send` must
all run on rnsd's task. Earlier prototypes did them directly from the
CLI task and saw silently-dropped outbound — request_path's outbound
packet vanished because the iface send_outgoing was on the wrong task.

## Storage defaults

In `rnsdInit`, gated on `s.rnsd.version < RNSD_VERSION`:

```
s.rnsd.enable              = 1
s.rnsd.transport_enabled   = 0     (endpoint mode by default)
s.rnsd.name                = ""
s.rnsd.announce.interval   = 1800  (30 min)
s.rnsd.remote_management   = 1     (host rnstransport.remote.management)
s.rnsd.respond_to_probes   = 0     (host rnstransport.probe with PROVE_ALL — opt-in)
s.rnsd.debug.only_local    = 0     (demote announce/path-request dbg chatter to verb)
```

`s.rnsd.path.max` / `s.rnsd.path.ttl` from the plan are intentionally
*not* defaulted — mR's path table is unbounded (`BasicHeapStore`),
pruned only by `PATHFINDER_E` (24 h). Re-add when we implement
post-process pruning or interface-mode intake control.

Cron line installed via `cronDefault`:

```
0 * * * * N rnsd persist if-transport
```

Flag `N` = STA upstream only; the `if-transport` arg makes the verb a
silent no-op on endpoints, so endpoints don't pay the hourly write.

## Gotchas

- **mR's `Log.h` redefines `info` / `warn` / `error` / `debug` / `msg`
  in `namespace RNS`** as free functions, with `#define msg (msg)` (a
  format-string mangling wart from the Arduino side). spangap's
  `log.h` defines `info()` / `warn()` / `err()` / `dbg()` / `verb()`
  as preprocessor macros (ESP_LOGx shims). The macros corrupt mR's
  function declarations on parse. The fix at the top of `rnsd.cpp` is:
  `#pragma push_macro` + `#undef` for each name, include mR headers,
  `#undef msg`, then `#pragma pop_macro`.
- **`thread_local` is unsafe in this codebase.** Use plain `static` in
  ITS recv callbacks. libgcc lazy TLS init has been seen to corrupt
  FreeRTOS scheduler state at boot. Acceptable here because ITS
  callbacks for a given port only dispatch on the task that registered
  them — no concurrency on the static buffer.
- **`Transport::request_path` outbound silently drops** when called
  off rnsd's task. mR is not thread-safe; `request_path` from any
  other task fails to actually emit the path-request packet.
  `rnsdRequestPath(dest_hash)` routes through the storage sentinel
  `rnsd.cmd.request_path`, which rnsd's task subscribes to and
  processes on its own task. Consumers should always use the
  rnsd.h API.
- **`Transport::inbound` early-returns leak `_jobs_locked`** (upstream
  mR bug). Multiple early-exit paths inside the function (malformed
  packet, cache-request handled, link-MTU clamp threw) skip the
  `_jobs_locked = false` reset at the end. After the first leak,
  `Transport::jobs()` becomes a no-op for the rest of the session
  (gates on `if (!_jobs_locked)`). Spangap patch in
  `Transport.cpp` replaces the manual set/clear with a function-local
  RAII guard — every return path now releases the lock.
- **`Identity::_known_destinations` cross-task race** — mR maintains
  a static map keyed by destination hash, written by Transport
  during announce processing, read by `Identity::recall`. Spangap
  patch in `Identity.cpp` adds a `static std::recursive_mutex` taken
  by every accessor (`remember`, `recall`, `recall_app_data`,
  `validate_announce`, `cull_known_destinations`, save/load). Locks
  are uncontended in the common case (rnsd-internal callers); the
  one cross-task caller is `rnsdRecallPubkey` from lxmf.
- **Malformed-packet log lines include first 8 bytes hex** — added
  to `Packet::unpack`'s error path so we can tell what shape of
  packet is failing (HEADER_1 vs HEADER_2 misparse, HDLC desync,
  noise byte, etc.):
  ```
  E [rnsd] Received malformed packet, dropping it (30B,
    first=51 03 7e 4a 1b 88 a1 c4 ): ...
  ```
  Bit 6 of byte 0 is HEADER_2 ↔ HEADER_1; byte 1 is hops.
- **mR's `Log.h` redefines `info` / `warn` / `error` / `debug` / `msg`
  in `namespace RNS`** as free functions, with `#define msg (msg)` (a
  format-string mangling wart from the Arduino side). spangap's
  `log.h` defines `info()` / `warn()` / `err()` / `dbg()` / `verb()`
  as preprocessor macros (ESP_LOGx shims). The macros corrupt mR's
  function declarations on parse. The fix at the top of `rnsd.cpp` is:
  `#pragma push_macro` + `#undef` for each name, include mR headers,
  `#undef msg`, then `#pragma pop_macro`. lxmf does not need this
  dance any more — after the rnsd.h refactor, lxmf has zero mR
  includes.
- **`thread_local` is unsafe in this codebase.** Use plain `static` in
  ITS recv callbacks. libgcc lazy TLS init has been seen to corrupt
  FreeRTOS scheduler state at boot. Acceptable here because ITS
  callbacks for a given port only dispatch on the task that registered
  them — no concurrency on the static buffer.
- **`PSRAM-stack tasks must not printf`** — use `info()` etc. Plain
  Reticulum/mR code that escapes via exceptions reaches our `err()` /
  `warn()` calls via `catch (const std::exception& e)` blocks; mR is
  exception-using.
