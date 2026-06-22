# lxmf — internals

Read this only if you need to reach inside the `lxmf` component:
change the wire codec, the ITS framing, the lifecycle state machine,
the identity model, or interop behaviour. The black-box / consumer view
is in [docs/lxmf.md](../lxmf.md).

This document is self-contained. It supersedes the planning document
`docs/plans/lxmf.md` — everything load-bearing from the plan
(ecosystem rationale, protocol facts, schema, phasing, design
decisions, open questions) is folded in here. The plan can be
discarded without loss.

> **Relationship to mR.** `lxmf.cpp` has **zero microReticulum (mR)
> includes**. LXMF is a layer *above* the Reticulum stack: it is
> implemented from scratch in C++ against the **markqvist/LXMF Python
> reference, pinned to LXMF 0.9.8**. mR (the RNS fork) is `rnsd`'s
> concern, reached only through rnsd's byte-array C API
> ([rnsd.h](../../main/rnsd.h)) — sha256, sign, verify,
> destination-hash, identity gen/erase/recall, request_path. So
> "what the upstream does + our deltas" below is framed against
> **upstream LXMF**, not mR. (For the mR fork itself, see the rnsd /
> microreticulum internals.)

`lxmf.cpp/h` runs as one FreeRTOS task, core 1, priority 1, 8 KB PSRAM
stack. The file header still says "Phase 4a"; in reality opportunistic
+ DIRECT + Resource are all wired and hardware-verified (see
[Status](#status) — phase labels below are kept for traceability).

---

# Part A — Upstream LXMF (what it is and how it works)

LXMF (Lightweight Extensible Message Format) is markqvist's messaging
layer on top of Reticulum. File:line refs below are inside
`markqvist/LXMF` at the 0.9.8 tag.

## A.1 Why we did not port a client

Every existing LXMF client has the same structural gaps; a clean
storage/SoT design clears them by construction. This is the rationale
for building rather than porting:

| Client | Stack / store | Strength | Weakness |
|---|---|---|---|
| Sideband | Python+Kivy / SQLite | reference impl; paper msgs, voice, telemetry | rough UX; opaque schema; locked DB stalls UI; no multi-device |
| NomadNet | Python+urwid / loose files | best low-bandwidth; doubles as prop node | messaging an afterthought; no threading; TUI only |
| MeshChat | Python WS+Vue / SQLite | best UX; voice; self-host prop | unilateral 10 MB cap (interop poison); FD leaks; non-standard WS |
| Columba | Kotlin native / Android | real native Android; multi-identity; BLE | RNS reimplemented on JVM (drift); Android-only |
| lxmf-cli / LXMFy / lxmd | Python | headless / bot / canonical prop daemon | not a user inbox |

Recurring failures, all sidestepped by the storage-SoT model: no
multi-device/multi-frontend sync (a propagation node delivers a stored
message to *one* requesting client and forgets it); ad-hoc opaque
storage with no portable on-disk format; fragile non-negotiated
attachments; invisible propagation reliability; missing ticket UX
(stamps now have a cost slider + generate/enforce toggles);
`FIELD_THREAD` exists but nobody renders threads; blocking I/O on
the UI path; no search; crude offline-queue UX; no native iOS.

## A.2 Wire format

`LXMessage.pack()`:

```
destination_hash(16) | source_hash(16) | Ed25519 sig(64) | msgpack(payload)
```

- `LXMF_OVERHEAD = 112` B (`LXMessage.py:54-62`). Destination hash is
  **16 bytes** (`Reticulum.py:144`).
- Payload msgpack tuple order is **`[timestamp, title, content,
  fields]`** (`LXMessage.py:359`). The README says `[timestamp,
  content, title, fields]` — **the README is wrong, the code wins.**
  Title before content. Getting this wrong = zero interop.
- Signature scope: **`dest || src || packed || SHA-256(dest || src ||
  packed)`** — the SHA-256 of the data is signed *in addition to* the
  data (`LXMessage.py:372-376`). Signing only the data does not
  interop.
- `message_id = SHA-256(dest || src || packed)`. Never on the wire;
  both sides re-derive.
- `transient_id = SHA-256(encrypted_lxmf_data)` — the propagation
  store's index. **`transient_id ≠ message_id`**; conflating them
  breaks dedup or threading. (Not used until propagation lands.)
- `src` is the sender's **`lxmf.delivery` destination hash**, not the
  identity hash. The recipient does `Identity.recall(src_hash)` and
  that map is keyed by destination hash. Wrong `src` →
  `SOURCE_UNKNOWN`, silent drop.

## A.3 Delivery modes (`LXMessage.py:29-33`)

| Mode | Code | Single-packet content | Mechanics |
|---|---|---|---|
| OPPORTUNISTIC | 0x01 | 311 B | one RNS encrypted packet, ECDH AES-128 per packet |
| DIRECT | 0x02 | 319 B/pkt; larger via Resource | RNS Link, ratcheted; `RESOURCE` for large payloads |
| PROPAGATED | 0x03 | ≤ `PROPAGATION_LIMIT=256 KB` | shipped to a propagation node, recipient pulls |
| PAPER | 0x05 | ~2210 B | `lxm://` URI, sneakernet |

Caller picks the mode; the router never auto-promotes DIRECT →
PROPAGATED. State machine: `GENERATING → OUTBOUND → SENDING → SENT →
DELIVERED`. `DELIVERED` is reachable only on a DIRECT link-ack;
propagation never proves delivery.

## A.4 Stamps (`LXStamper.py`)

Hashcash PoW: `SHA-256(workblock || nonce) ≤ target`, `target = 1 <<
(256 - cost)`. Workblock is a memory-hard, scope-specific HKDF chain:
recipient-stamp `WORKBLOCK_EXPAND_ROUNDS = 3000`, propagation-stamp
`1000`, node-peering key `25` — **do not unify the round counts**.
`STAMP_SIZE = 32`. The **recipient-stamp** is msgpack element [4] of
the payload list; the **propagation-stamp** is appended after the
encrypted blob in the propagation envelope — different positions,
different workblock seeds.

## A.5 Tickets

16-byte preimages issued by a recipient to known senders. Sender
computes `stamp = SHA-256(ticket || message_id)` — no PoW. Lifetimes
(`LXMessage.py:46-51`): `EXPIRY=21d`, `RENEW=14d`, `GRACE=5d`,
`INTERVAL=1d`. A ticket-backed stamp gets `stamp_value = COST_TICKET =
0x100`, ranking above any PoW stamp. `FIELD_TICKET = 0x0C` value is
`[expiry_unix_s, ticket_16B]`, not raw bytes.

## A.6 Field registry (`LXMF.py:8-41`, msgpack int keys)

`0x01 EMBEDDED_LXMS, 0x02/0x03 TELEMETRY[_STREAM], 0x04
ICON_APPEARANCE, 0x05 FILE_ATTACHMENTS, 0x06 IMAGE, 0x07 AUDIO, 0x08
THREAD, 0x09/0x0A COMMANDS/RESULTS, 0x0B GROUP, 0x0C TICKET, 0x0D
EVENT, 0x0E RNR_REFS, 0x0F RENDERER, 0xFB-0xFD CUSTOM_*, 0xFE
NON_SPECIFIC, 0xFF DEBUG`. `RENDERER ∈ {PLAIN, MICRON, MARKDOWN,
BBCODE}`.

## A.7 Router defaults worth copying

`MAX_DELIVERY_ATTEMPTS=5, DELIVERY_RETRY_WAIT=10s, PATH_REQUEST_WAIT=7s,
MESSAGE_EXPIRY=30d, STAMP_COST_EXPIRY=45d, LINK_MAX_INACTIVITY=10min,
PN_STAMP_THROTTLE=180s, MAX_PEERS=20`.

## A.8 Identity & destinations

LXMF uses `RNS.Destination.SINGLE` (Ed25519+X25519, ratchets enforced)
on aspect `lxmf.delivery` (mailbox) and `lxmf.propagation`
(propagation node, control sub-aspect `lxmf.propagation.control`). The
identity is the standard RNS Identity — there is no LXMF-specific
keypair.

---

# Part B — Our architecture and how it realises LXMF

## B.1 One task, storage as the only API

Single FreeRTOS task, single wait point `itsPoll(deadline())`. There
is no DataChannel and no `LXMF_PORT_*`. Every frontend reads/writes
storage; lxmf subscribes to its own command subtree and reacts. The
control surface collapses to "writes to storage drive every state
change"; the only thing not expressible by subscription — "do X now,
no persistent state" — uses the self-clearing-key convention (B.3).

Two structural wins: multi-frontend correctness is free (no DB locks,
no merge), and there is no blocking I/O on any UI path (backend in
firmware, frontends async over the storage subscription).

## B.2 Single writer per subkey

Client-owned and firmware-owned fields are strictly disjoint, so no
field is mutually written and the firmware's subscriptions match
exclusively what clients write — no self-notify churn.

Messages are stored **per contact**: every `msgs.<id>.*` below is
really `msgs.<peer>.<id>.*` (`<peer>` = 32-hex destination, the
conversation subtree; see
[../plans/lxmf-messages-per-contact.md](../plans/lxmf-messages-per-contact.md)).
The schema block uses the short `msgs.<id>` form for brevity.

| Field | Owner | Notes |
|---|---|---|
| `msgs.<id>.{peer,title,content,thread,method}` | client | editable while `stage==draft` |
| `msgs.<id>.read` | client | inbound flag; firmware ignores it |
| `msgs.<id>.stage` | client writes initial `draft`; **firmware owns all later values** | transitions only via `cmd.*` sentinels |
| `msgs.<id>.{wire,message_id,attempts,next_retry_s,last_error,stamp_value}` | firmware | derived after pack |
| `contacts.<m>.*` | client | firmware reads only (stubs on first inbound) |
| `tickets.*` | client (manual) + firmware (auto-issue) | expiry is the truth gate |
| `lxmf.cmd.*`, `lxmf.id.<n>.cmd.*` | client writes, firmware deletes | imperative actions |

## B.3 Self-clearing command keys

One-shot actions with no persistent state. Client writes a sentinel;
firmware processes and deletes it. Existence = request, absence = ack.
A cross-task subscription to `*.cmd.*` is therefore a free
observability channel — every pending firmware action is visible.

Subscription surface (firmware side, deliberately narrow):

| Subscription | Installed | Fires for | Handler |
|---|---|---|---|
| `lxmf.cmd.` | `lxmfInit` once | `identity_new` / `identity_import` / `identity_destroy` | `onIdentityLevelCmd` |
| `lxmf.id.<n>.cmd.` | per slot, `create/loadIdentityForSlot`; removed by `destroyIdentity` | `send` / `cancel` / `delete` / `announce` | static `onIdCmd<n>` → `handleIdCmd(n,…)` |

The slot index is captured at compile time in four static stubs
(`onIdCmd0…3`) so the callback stays `(key,val)` with no key parsing.
**The firmware never subscribes to `s.lxmf.*` or `lxmf.id.<n>.*`
(non-`cmd`).** Every firmware write to message records / ephemeral
mirrors is silent relative to its own subscriptions: there is no
`s_event_pending` flag and no `scanReady` walker — the cmd sentinels
are the only wake sources. Resist widening these prefixes "for
symmetry"; the design was driven by exactly that smell.

Reserved sentinels not yet wired: `cmd.prop_sync` (propagation),
`cmd.rotate_ratchet`, `cmd.factory_reset`.

## B.4 Identity model — array from day one

Schema is an array (`lxmf.id.<n>.*`, `n ∈ [0, LXMF_MAX_IDENTITIES=4)`)
even though there is no multi-identity UX yet; single-identity simply
runs at `n=0`. Each slot is fully compartmentalised — contacts,
tickets, messages, drafts siloed by path; `destroyIdentity(n)` wipes
`secrets.lxmf.id.<n>.*`, `s.lxmf.id.<n>.*`, `lxmf.id.<n>.*` and
nothing else.

LXMF owns its identities, not rnsd. It generates Ed25519+X25519
keypairs via an `rnsd.h` helper on the lxmf task (no rnsd round-trip),
stores them at `secrets.lxmf.id.<n>.privkey`, and tells rnsd "use that
key" at connect time via the [keys-by-reference convention](../../../memory/project_keys_by_reference.md)
— ITS payloads carry the **storage key name**, never the secret
bytes. rnsd never needs to know LXMF has multiple identities.

In-RAM per-slot state (`s_ids[LXMF_MAX_IDENTITIES]`): `identity_key`
string (a storage path, *not* an `RNS::Identity`), precomputed
`dest_hash[16]`, the ITS handle, an 8-deep outbox (`send_id → message
key`), counters, and `last_announce_tick`. Every sign/hash/derive goes
through rnsd.h byte-array helpers that load the key from storage on
demand (curve unpack is microseconds) — lxmf knows nothing about mR's
type system.

`bootstrapFirstBoot()` at task start: `loadIdentityForSlot(n)` for
every `n` (recovers from `secrets.lxmf.id.<n>.privkey`); if none
loaded, auto-`createIdentityForSlot(0,"main")`. There is **no**
auto-announce at boot — the first announce is the iface-up debounce
(B.7), at least 10 s in, which eliminates the old boot race.

## B.5 ITS port — `RNSD_PORT_DEST` (the mailbox)

One bidirectional connection per identity (LXMF must be the active
connector both directions — only it knows where its keys live).
Connect payload `rnsd_mailbox_connect_t` ([ports.h](../../main/ports.h)):
`aspect[32]="lxmf.delivery"`, `identity_key[40]`, `dest_type`. rnsd
derives the dest hash, registers it for inbound dispatch, hosts the
`Destination`, owns `Transport::request_path` and the in-flight retry
table.

In-band frames (first byte = opcode, `RNSD_DEST_*` in ports.h):

| Opcode | Dir | Payload | Handler |
|---|---|---|---|
| `0x01 OUT_PACKET` | lxmf→rnsd | `send_id(2) \| lxm_wire` | `processSend` |
| `0x02 OUT_RESULT` | rnsd→lxmf | `send_id(2) \| status(1) \| rtt_ms(4 BE) \| hops(1)` | `applyOutResult` |
| `0x03 OUT_CANCEL` | lxmf→rnsd | `send_id(2)` | `processCancel` |
| `0x04 IN_PACKET` | rnsd→lxmf | full LXM plaintext | `onInboundLxm` |
| `0x05 OUT_STATUS` | rnsd→lxmf | `send_id(2) \| type(1) \| tail` | `applyOutStatus` |
| `0x06 ANNOUNCE` | lxmf→rnsd | `app_data` | `sendAnnounce` |

`OUT_RESULT.status`: `0` sent (opportunistic egress acknowledged) ·
`1` delivered (DIRECT/Resource proof) · `2` cancelled (after our
`OUT_CANCEL`) · `3` evicted (rnsd resource limit). **There is no
`failed_*` status** — rnsd never gives up on its own; path/link/retry
trouble is narrated via out-of-band `OUT_STATUS` aux frames
(`REQUESTING_PATH`, `PATH_KNOWN`, `EGRESS_QUEUED`, `LINK_ESTABLISHING`,
`RESOURCE_PROGRESS`, `RETRY{attempt,reason}`, `PATH_LOST`). LXMF owns
the giving-up policy: it counts `RETRY` against a per-message budget
(`MAX_DELIVERY_ATTEMPTS=5`) and writes `OUT_CANCEL` on overflow. The
user-initiated `cancelled` vs policy-driven `failed` distinction lives
entirely in LXMF (which reason triggered the `OUT_CANCEL`). `send_id`
is a 16-bit per-identity correlator (wraps to 1, skipping 0),
resolved back to the message key via `outboundFindBySendId`. The same
port also serves `rnprobe` (connect with `identity_key=""`, one
`OUT_PACKET`, wait `OUT_RESULT`) — it replaced the old
`RNSD_PORT_PACKET`.

### DIRECT (over a Reticulum Link) — Phase E

Real-world LXMF peers default to DIRECT; without it nobody can reply.

- **Inbound:** `connectMailbox` also calls
  `rnsdDestListenLinks(handle, LXMF_LINK_INBOX_PORT=100)`. rnsd flips
  `accepts_links(true)` on the `lxmf.delivery` dest and back-connects
  each accepted inbound Link to that port with an
  `rnsd_link_incoming_t`; `onLinkInboxRecv` feeds the bytes straight
  into the shared `onInboundLxm`.
- **Outbound:** `processReady` resolves a method (B.6), opens
  `rnsdLinkOpen(peer,"lxmf.delivery",…,tag="lxmf.id<n>.<mid8>")` and
  sends the full wire. Because `RNSD_PORT_LINK` has no `OUT_RESULT`,
  the 1 Hz `resolveDirectSends()` settles `sent`/`failed` from
  `rnsd.links.<tag>.{state,tx_packets,last_error}` and tears the Link
  down.

### Resource (large messages) — Phase F

Messages above the single-packet budget go as an RNS Resource over a
Link. lxmf hands rnsd the wire via `rnsdLinkSendResource(tag, buf,
len, send_id)`; rnsd runs the Resource engine and reports completion
on `LXMF_LINK_RESOURCE_AUX_PORT (101)` with
`rnsd_link_resource_done_t` opcodes
`RNSD_LINK_RESOURCE_{INBOUND_DONE,OUTBOUND_DONE,FAILED}` →
`onResourceAux`. Inbound Resource completion delivers the reassembled
wire straight into `onInboundLxm`; the buffer is rnsd-owned and
released via `rnsdResourceRelease` even on the drop path. Inbound and
outbound Resource are hardware-verified against a real upstream LXMF
peer (16 KB / 128 KB multi-segment in; 109-part out). See
[Status](#status) for the platform storage caveat this surfaced.

## B.6 Outbound lifecycle

```
client: write msgs.<peer>.<localkey>.{peer,title,content,thread,method,stage=draft}
client: write lxmf.id.<n>.cmd.send = <peer>/<localkey>   ◄── commit (same txn ideal)

handleIdCmd → split val on '/' → delete sentinel → processSend(id,peer,localkey):
  peer arrives from the sentinel (it IS the record's path segment, not
    read back from storage); validate 32-hex → 16 B dest
  pack [ts_ms,title,content,fields] msgpack (title BEFORE content)
  sign over dest||src||packed||SHA-256(...)
  message_id = SHA-256(dest||src||packed); persist wire+message_id; stage=queued
  allocate outbox slot (send_id); processReady decides transport:

processReady — method resolution (Phase E §8.2):
  msgs.<id>.method → s.lxmf.id.<n>.default_method → "auto"
  oversize = (title+content+32 > LXMF_OPP_CONTENT_BUDGET=311)
  "direct"        → use_direct = true
  "opportunistic" → fail if oversize, else opportunistic
  "auto"/other    → use_direct = oversize
  opportunistic → OUT_PACKET on the mailbox handle
  direct, fits a Link packet → rnsdLinkOpen + send full wire
  direct, large → rnsdLinkSendResource (Resource over Link)

rnsd → mailbox/aux:
  OUT_STATUS → applyOutStatus  (attempts/last_error; RETRY budget → OUT_CANCEL)
  OUT_RESULT → applyOutResult  (stage = sent|delivered|cancelled|failed)
  resource aux (port 101) → onResourceAux (outbound done/failed)
```

Local outbound key is `o_<unix_ms>_<rand4>` (the real `message_id`
isn't stable while a draft mutates; it's carried as a sidecar field
for thread/ticket cross-refs), stored under the `<peer>` subtree.
Inbound records key directly by `message_id`, also under `<peer>` (=
`src`). The outbox slot (`outbound_t`) carries `peer` alongside
`msg_key` so `applyOutResult`/`applyOutStatus`/`resolveDirectSends`/
`onResourceAux` rebuild the path from a `send_id` without re-reading
storage. Atomic commit: the client writes fields + sentinel in one
`storageBegin/End`; `processSend` reads each field via `storageGetStr`
and fails the send with a `last_error` if any are missing. No
auto-retry; the client re-issues `cmd.send` after `failed`. Outbox is
8 deep; a 9th in-flight send → `last_error="outbox full"`.

## B.7 Inbound lifecycle

`onMailboxRecv` dispatches on the opcode byte; `IN_PACKET` (and the
DIRECT/Resource inbound paths) funnel into `onInboundLxm`:

1. Length ≥ `LXMF_OVERHEAD=112`.
2. `wire[0..16] == id.dest_hash` (else rnsd routing weirdness — warn,
   drop).
3. `RNS::Identity::recall(src_hash)`; if absent →
   `Transport::request_path(src_hash)` and **drop** (no pending-verify
   buffer; the sender's retransmit resolves it once the path lands).
4. Verify Ed25519 over `dest||src||packed||SHA-256(...)`. Bad → drop.
5. `message_id = SHA-256(dest||src||packed)`, hex64.
6. Dedup: in-RAM ring (`s_dedup_ring`, 64 entries — cheap, not
   reboot-durable) **and** storage existence
   (`s.lxmf.id.<n>.msgs.<peer>.<mid>.stage` — authoritative, reboot-durable).
7. `lxmParsePayload`.
8. Persist under `s.lxmf.id.<n>.msgs.<peer>.<mid>.*` (`<peer>` = `src`)
   with `stage=received`,
   `dir=in`, `read=0`; stub `contacts.<src>` (trust=0, copy
   `display_name` from the announce catalogue if heard); refresh `last_seen`.
9. `id.received++`.

## B.8 Wire codec — in-tree msgpack

LXMF needs only fixarray/fixmap/str/bin/uint/int/float/nil, so lxmf.cpp
inlines its own writer (`mpPack*`) and walker (`mpScan`/`mpScanNext`)
— no allocator dependency, no Arduino MsgPack. The walker bails on
`fixext`/`ext` (LXMF doesn't use them). The decoder tolerates
`float32/64` timestamps from the Python reference (`f*1000 → uint64`
ms) though we always pack `uint64`. Implemented in `lxmPackPayload`,
`lxmPackWire`, `lxmParsePayload`, `lxmMessageIdHex`. Constants:
`LXMF_DEST_HASH_LEN=16`, `LXMF_SIG_LEN=64`, `LXMF_OVERHEAD=112`,
`LXMF_OPP_CONTENT_BUDGET=311`. `FIELD_THREAD` is stored hex64 but
packed as raw 32 B on the wire (`lxmPackPayload` converts both ways).

## B.9 Announces

`sendAnnounce(id)` builds msgpack `[display_name_or_nil, stamp_cost]`
and pushes `ANNOUNCE | app_data`; rnsd calls
`listener_dest.announce(app_data)`. Two triggers, both per used
identity with a live mailbox: an **iface-up debounce** (lxmf
subscribes `rnsd.iface_event_seq`; each bump arms `now+10 s`,
re-arming on every iface-up so a burst yields one announce 10 s after
the last; also armed once at startup), and a **periodic re-announce**
(1 Hz check vs `s.lxmf.announce_interval_s`, default 1800; 0
disables).

Inbound: lxmf subscribes rnsd's `RNSD_PORT_ANNOUNCES` fan-out with
aspect filter `lxmf.delivery`. rnsd forwards matches as
`hops(1)|dest(16)|identity(16)|app_data(N)`; `onAnnounceFromRnsd`
parses (`parseLxmfAnnounce` handles the five shapes seen in the wild),
skips own dests, and writes **one packed leaf per destination**:

```
lxmf.announces.<dest_hex> = "<last_s>|<cost>|<hops>|<ratchet>|<name>"
```

`last_s` first so `atoi(value)` recovers it for the eviction walker
without parsing the rest; `name` last so embedded `|` need no
escaping. One cJSON node per entry (vs the old 6 leaves) keeps the
PSRAM block count down. The announce catalogue is ephemeral,
cross-identity, bounded by `s.lxmf.max_announces` (default 2048;
`annCountAndMaybeOldest` evicts the smallest `last_s` on overflow —
O(N), ~12 ms at 2048 under `CFG_LOCK`, only on a new dest at
capacity). Per-identity `contacts.*` is the separate per-identity
address book.

**Concurrency:** `AnnounceFanout::received_announce` runs on the
**rnsd** task (inside `Transport::inbound`) but only does
`memcpy + itsSend(timeout=0)` per subscriber; all announce-catalogue storage
writes happen on the **lxmf** task in `onAnnounceFromRnsd`. rnsd never
touches storage in the announce path, so TCP backpressure under heavy
announce traffic cannot overflow rnsd's recv queue.

## B.10 Main loop

```cpp
itsClientInit(LXMF_MAX_IDENTITIES + 1);      // +1 announce sub
storageSubscribeChanges("lxmf.cmd.", onIdentityLevelCmd);
bootstrapFirstBoot();                        // load/create slots + per-id cmd subs
for n used: connectMailbox(); sendAnnounce();
connectAnnounceSub();                        // RNSD_PORT_ANNOUNCES, "lxmf.delivery"
publishStats();
for (;;) {
  itsPoll(nextDeadline());                   // ITS callbacks OR 1 Hz deadline
  if (1 s elapsed) {
    publishStats();
    for n: if used && handle<0: connectMailbox()+sendAnnounce()
    if announce sub dropped: connectAnnounceSub()
  }
}
```

Single wait point. Callbacks (mailbox recv, announce-sub recv, cmd
handlers, resource aux) run inline from `itsPoll` dispatch on the lxmf
task — no event-pending flag, no scan walker.

---

# Part C — Deltas from upstream LXMF

What we changed / chose differently to get here.

1. **No `LXMRouter`.** Upstream couples a router co-process with the
   UI and a private store. We replace it with the firmware task + the
   spangap storage SoT: the inbox is `message_id → wire + sidecar`,
   every message re-verifiable from its bytes alone, multi-frontend
   with zero merge logic. This is the headline divergence and the
   reason a port was rejected (A.1).
2. **Opportunistic 16-byte strip moved into rnsd.** On the wire,
   OPPORTUNISTIC drops the leading dest_hash (`LXMessage.py:631`
   sends `Packet(dest, packed[16:])`; recipient re-prepends,
   `LXMRouter.py:1827-1830`). lxmf always hands rnsd the **full** wire
   (`dest||src||sig||packed`); rnsd does the strip on `OUT_PACKET` and
   the prepend on `IN_PACKET`, so consumers see complete wires both
   sides.
3. **DIRECT does *not* strip/prepend (corrects an earlier plan
   assumption).** Upstream `LXMessage.__as_packet` sends the full
   `self.packed` for DIRECT and only strips for OPPORTUNISTIC
   (LXMessage.py:630-632); `LXMRouter.delivery_packet` does
   `lxmf_data = data` for LINK vs `dest+data` for opportunistic
   (LXMRouter.py:1825-1833). So a DIRECT Link packet already carries
   the full wire — lxmf must **not** prepend on inbound or strip on
   outbound. (First impl followed the opportunistic rule, doubled the
   dest, signature failed; fixed by passing the Link payload verbatim
   both ways.)
4. **Method auto-selection in firmware.** Upstream makes the caller
   pick the mode. We resolve `per-message → per-identity default →
   auto` and auto-promote oversize to DIRECT/Resource (B.6), because
   the storage API has no good place for a human to pick transport per
   send and "just works" is the goal.
5. **Resource handoff is a shared-memory aux, not in-band.** Large
   messages go via `rnsdLinkSendResource` + the
   `LXMF_LINK_RESOURCE_AUX_PORT (101)` completion aux, keeping the
   data path type-byte-free. **Platform finding (2026-05-16):**
   `onInboundLxm` persists the full body into the RAM-backed cJSON
   storage tree, backed by the 256 KB `/state` LittleFS, with **no
   retention/eviction cap** (`s.lxmf.max_resource_size` ≈ the whole
   partition). Sustained large inbound fills `/state`, which cascades
   (storage writes fail → ITS/CLI wedge → an rnsd path-table
   watchdog panic). This is a platform/storage-policy gap, *not* a
   Resource-engine or lxmf-protocol defect (the engine is clean,
   12/12 soak). The real fix is the planned storage/log → SD move
   (large Resource-delivered bodies are blob-scale and should not live
   in the config tree); interim mitigation is `cmd.delete`
   housekeeping (incl. the bare-`<peer>` whole-conversation form from
   delta 9). **Correction (2026-05-18):** the previously-recorded
   "`storageDeleteTree` success not reflected by an immediate `show` —
   harmless consistency caveat" was **not** harmless and **not** a
   consistency issue — `processDelete` passed a *trailing-dot* prefix
   (`…msgs.<id>.`) to `storageDeleteTree`, whose `deleteFromTree`
   splits on the **last** dot and so tried to detach a child named
   `""` → silent no-op. `cmd.delete` never freed anything; this is a
   likely contributor to the Phase F `/state`-full incident. Fixed in
   delta 9: `processDelete` now passes the node path with **no**
   trailing dot (the platform contract, cf. `destroyIdentity`'s
   `idPath(n,"")`); HW-verified single + whole-conversation delete.
   Side note: `cmd.delete` is a single sentinel key
   (rapid repeats overwrite-race) and `processDelete`'s
   `storageDeleteTree` success was observed not reflected by an
   immediate `show` — a separate, harmless delete-or-show
   consistency caveat.
6. **`OUT_RESULT status=0 ≡ sent`, never `delivered`.** Opportunistic
   gets no native ack; only a proven DIRECT/Resource transfer is
   `delivered`. `applyOutResult` must not optimistically upgrade.
7. **Unknown-sender inbound is dropped, not buffered.** A
   verification queue would have to reason about `MESSAGE_EXPIRY=30d`;
   we rely on the sender's normal retransmit after `request_path`.
8. **Not yet wired vs upstream:** stamps + tickets + auto-ticket +
   spam-gate (Core 0 PoW worker, `lxmf.id.<n>.stamping.<id>.progress`);
   `applyOutStatus` counts `RETRY` but the auto-`OUT_CANCEL` at
   `MAX_DELIVERY_ATTEMPTS` is not wired (user cancel is);
   reachability map; propagation (PROPAGATED mode, prop-node `/get`,
   running as a prop node ourselves); blob-store attachments
   (>8 KB → content-addressed `data/lxmf-blobs/<sha256>` + HTTP
   `GET /lxmf/blob/<hex>`, custom-type marker in `0xFB..0xFD`);
   multi-identity UX; backchannel link reuse after DIRECT delivery
   (re-identify on the same Link so the peer can reply without a new
   one — skipping it halves DIRECT's latency advantage).
9. **Per-contact message store.** `msgs.<peer>.<key>` instead of a flat
   `msgs.<key>` pile; `cmd.{send,cancel,delete}` value is `<peer>/<key>`
   (`delete` also takes a bare `<peer>` = whole conversation). This is
   layout-only — it creates the subtree seam
   [../plans/evictable_storage.md](../plans/evictable_storage.md) hangs
   retention on; it does **not** add eviction. No migration (zero
   install base). Full rationale + CLI design:
   [../plans/lxmf-messages-per-contact.md](../plans/lxmf-messages-per-contact.md).

## C.1 Implementation gotchas

- **mR `Log.h` macro clash.** mR defines `info/warn/error/debug/msg`
  as free functions in `namespace RNS` with `#define msg (msg)`;
  spangap's `log.h` defines `info()/warn()/err()/dbg()/verb()` as
  macros that corrupt mR's declarations on parse. The fix at the top
  of `lxmf.cpp` (same as `rnsd.cpp`): `#pragma push_macro` + `#undef`
  each name, include mR-touching headers, `#undef msg`, `#pragma
  pop_macro`. Build dies in `Log.h` → you missed the bracket.
- **`Identity::recall` cache size.** rnsd raises mR's default from 100
  to 1000 (`Identity::known_destinations_maxsize(1000)`) so inbound
  from infrequent correspondents doesn't drop on eviction. "inbound
  from unknown sender" for a peer that just announced may still be
  this.
- **Never construct `RNS::Destination` for the IN mailbox here.** mR's
  `Destination` ctor auto-registers with `Transport`; rnsd hosts the
  IN destination. lxmf only computes `Destination::hash(id,"lxmf",
  "delivery")` as a static pre-computation for storage publishing.
- **`storageDeleteTree` wants a node path with NO trailing dot.**
  `deleteFromTree` does `strrchr(path,'.')` and detaches the segment
  after the last dot from its parent; a trailing dot ⇒ it detaches
  `""` ⇒ silent no-op (it returns `false`, so not even the save-timer
  or browser-null-notify fire). `msgPrefix`'s trailing dot is for
  `storageForEach`/`collectTokens` *only* — `processDelete` builds its
  own dotless path. Don't unify the two.
- **`storageForEach` returns leaves, not subtrees.** The CLI's
  `collectTokens` extracts the token between known prefix/suffix
  segments; there is no "list subtrees" API. With the per-contact
  store this is leaned on twice: `collectTokens("…msgs.")` yields the
  **peer** tokens (the conversation list, `lxmf chats`), and
  `collectTokens("…msgs.<peer>.")` the keys within one thread.
- **No `thread_local`.** Plain `static` is correct: ITS recv callbacks
  for a port dispatch only on the registering task. libgcc lazy TLS
  init has corrupted the FreeRTOS scheduler at boot before.
- **`s.lxmf.debug.only_local`** demotes per-announce catalogue `dbg()`
  lines to `verb()` (live-mirrored, no reboot) so debug-level lxmf
  surfaces message/command activity, not announce churn — same
  pattern as `s.rnsd.debug.only_local`.

---

# Storage schema (full intended; ✓ = implemented today)

Frontends query by indexed fields (`peer`, `thread`, `stage`, `dir`,
`read`), not by key — the inbound-vs-outbound keying asymmetry is fine.

**Messages are stored per contact.** Every `msgs.<id>.*` line below is
`s.lxmf.id.<n>.msgs.<peer>.<id>.*` on disk (`<peer>` = 32-hex
destination = the conversation subtree); the `<id>` short form is used
here only for brevity. This is the seam
[../plans/evictable_storage.md](../plans/evictable_storage.md) attaches
to; rationale in
[../plans/lxmf-messages-per-contact.md](../plans/lxmf-messages-per-contact.md).

### Global (`s.lxmf.*`)

```
s.lxmf.id.<n>.enabled         ✓ per-identity participation (default 1; 0 = dark)
s.lxmf.stamp_cost             ✓ advertised PoW cost, single global (default 16; 0–18, 0 = none)
s.lxmf.generate_stamps        ✓ pay a peer's advertised stamp cost when sending (default 1)
s.lxmf.enforce_stamps         ✓ drop inbound without a valid stamp for our cost (default 0)
s.lxmf.auto_ticket              auto-issue tickets to contacts (not implemented)
s.lxmf.version                ✓ LXMF_VERSION = 1
s.lxmf.announce_interval_s    ✓ periodic re-announce s (default 1800)
s.lxmf.max_announces          ✓ announce-catalogue cap (default 2048)
s.lxmf.max_resource_size      ✓ largest inbound Resource (default 262144)
s.lxmf.cli.selected_id        ✓ CLI selected identity
s.lxmf.debug.only_local       ✓ quiet announce dbg
```

### Per-identity persistent (`s.lxmf.id.<n>.*`)

```
label ✓ · enabled ✓ · display_name ✓ · default_method ✓ ·
privkey_ref (default secrets.lxmf.id.<n>.privkey) ·
propagation.{node,sync_interval_s} (Phase 6)

contacts.<m>.{hash,nick,display_name,icon_appearance,stamp_cost,
              trust(0/1/2),muted,last_seen}        ✓ (subset: hash,nick,
                                                   display_name,trust,last_seen)
tickets.{in,out}.<m>.{peer,ticket,expiry}          (Phase 4b)

msgs.<id>.dir            ✓ in|out
msgs.<id>.stage          ✓ draft(client)|queued|sending|sent|delivered|
                             failed|cancelled|received (firmware)
msgs.<id>.peer           ✓ hex16
msgs.<id>.title          ✓
msgs.<id>.content        ✓
msgs.<id>.thread         ✓ hex64 root message_id, "" if none
msgs.<id>.method         ✓ opp|direct|auto (hint while draft)
msgs.<id>.ts             ✓ unix s
msgs.<id>.read           ✓ inbound only, 0|1
msgs.<id>.wire           ✓ hex of packed LXM (firmware)
msgs.<id>.message_id     ✓ hex64 SHA-256 (firmware)
msgs.<id>.attempts       ✓ (OUT_STATUS RETRY)
msgs.<id>.last_error     ✓ short UI string
msgs.<id>.next_retry_s     monotonic ETA (Phase 4b)
msgs.<id>.stamp_value      uint / 0x100 ticket (Phase 4b)
msgs.<id>.fields_present   bitmap 0x01..0x0F (future)
msgs.<id>.attach_blob      hex64 externalised attachment (Phase 5)
```

Keying: every record lives under its conversation, `msgs.<peer>.<key>`;
`<key>` is inbound → real `message_id`; outbound → local
`o_<unix_ms>_<rand4>` (with `message_id` as a sidecar once packed).
`peer` is also kept as a field (redundant with the path segment) so
the indexed-query contract and consumer reads are unchanged.

### Secrets / ephemeral

```
secrets.lxmf.id.<n>.privkey  ✓ 128-hex Ed25519+X25519 (wiped by identity_destroy)

lxmf.up ✓ · lxmf.id.<n>.up ✓ · lxmf.id.<n>.dest_hash ✓ ·
lxmf.id.<n>.last_announce_s ✓ · lxmf.id.<n>.stats.{sent,received,pending,
failed} ✓ (+stamps_solved future) ·
lxmf.id.<n>.stamping.<msgid>.progress       (Phase 4b)
lxmf.id.<n>.propagation.{last_sync_ts,last_sync_count,queue_depth} (Phase 6)
lxmf.id.<n>.reachability.<peer>.{mode,since}                       (future)
lxmf.announces.<dest_hex> ✓ "<last_s>|<cost>|<hops>|<ratchet>|<name>"
```

---

# Status

| Phase | Scope | State |
|---|---|---|
| 4a | identity bootstrap, mailbox port + frame parser, opportunistic send/recv, sig + dedup, message storage, threading, contacts | **done, HW-verified** |
| E | DIRECT mode (Link) inbound + outbound; method selection | **done, HW-verified** |
| F | Resource (large messages) inbound + outbound over Link | **engine done, HW-verified** — surfaced a *platform* storage-retention / path-table-pruning gap (Delta C.5), tracked, folds into storage/log→SD; not an lxmf/engine defect |
| 4b | stamps + tickets + auto-ticket + spam gate; Core 0 PoW worker; progress streaming | not started |
| 6 | PROPAGATED mode, prop-node `/get` client, sync UX | not started |
| 7 | run as a propagation node (peer gossip, autopeer, MAX_PEERS=20) | not started |
| 8+ | full multi-identity UX (picker, generate/import flows) | schema ready, no UX |

# Design decisions & open questions (carried from the plan)

- **Opportunistic delivery semantics.** RNS opportunistic packets get
  no native ack; `OUT_RESULT status=0` = "rnsd handed bytes to a
  transport" → `sent`, never `delivered`. Confirmed.
- **Retry budget per send.** `MAX_DELIVERY_ATTEMPTS=5` default;
  some paths may want different policy (one-shot probe vs critical
  reply). Decision: LXMF-side counter (keeps the rnsd port surface
  minimal) — an optional per-`OUT_PACKET` budget field is the
  alternative if needed.
- **Audio/voice (LXST).** Out of scope; `FIELD_AUDIO=0x07` reserved,
  such messages display as "[audio — unsupported]".
- **Group conversations.** `FIELD_GROUP=0x0B` exists; deferred past
  propagation. Would add `s.lxmf.id.<n>.groups.<g>.*`, no other shape
  change.
- **Blob-hash attachment custom-type marker.** Concrete value in
  `0xFB..0xFD` to be chosen alongside the blob-store work.
- **Backchannel link reuse** after DIRECT delivery (re-identify on the
  same Link) — required to keep DIRECT's latency advantage; not yet
  wired.

# Interop checklist (must stay true)

1. Payload tuple `[timestamp, title, content, fields]` — title before
   content (`LXMessage.py:359`; README is wrong).
2. Signature scope `dest||src||packed||SHA-256(dest||src||packed)` —
   hash signed alongside data (`LXMessage.py:372-376`).
3. Destination hash is 16 bytes (`Reticulum.py:144`).
4. `src` = sender's `lxmf.delivery` dest hash, not identity hash.
5. Recipient stamp = payload element [4]; propagation stamp appended
   after the encrypted blob — different positions/seeds.
6. `transient_id ≠ message_id` (propagation indexes by transient; our
   inbox by message_id).
7. Stamp workblock HKDF rounds 3000/1000/25 by scope — do not unify.
8. `FIELD_TICKET = 0x0C` = `[expiry_unix_s, ticket_16B]`.
9. Test payloads for Resource must be incompressible (bz2 is disabled
   in this build — compressible inbound is *correctly* dropped).
