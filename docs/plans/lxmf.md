# LXMF — design proposal

> **Superseded (2026-05-16).** Everything load-bearing here now lives
> in [docs/internals/lxmf.md](../internals/lxmf.md) (architecture,
> protocol facts, full schema, phasing, design decisions, open
> questions) and [docs/lxmf.md](../lxmf.md) (the black-box / consumer
> view). This file is retained only for historical context; where it
> disagrees with those two, **they win**. It can be deleted without
> loss.

Working design for the `lxmf` task and its browser/device frontends.
Expands [component-plan.md §15](../component-plan.md) (a stub) with a
foundation strong enough to outclass every existing LXMF client on
storage model, multi-frontend behaviour, propagation visibility,
spam-resistance UX, threading, and operational clarity.

Status: proposal. Where this disagrees with component-plan.md §15 or
the older [reticulous-early-thoughts.md](reticulous-early-thoughts.md),
this document supersedes them — fold accepted bits back into
component-plan.md once landed.

---

## 1. Ecosystem context

Why we need to design carefully, not just port an existing client.

### 1.1 What's out there

| Client | Stack | Storage | Notable strength | Notable weakness |
|---|---|---|---|---|
| Sideband (markqvist) | Python + Kivy | SQLite, raw identity file | Reference impl; first to land paper msgs, voice, telemetry, command/result | Kivy UX rough; opaque schema; single locked DB stalls UI during sync; no multi-device sync |
| NomadNet (markqvist) | Python + urwid | Loose files in `messagestore/` | Best at low-bandwidth; doubles as page + propagation node | Messaging an afterthought; no threading; attachments awkward; TUI only |
| MeshChat (liamcottle) | Python WS + Vue SPA | SQLite | Best UX out there; voice; can self-host propagation | Raises receive cap to 10 MB unilaterally → silent cross-client failures; FD leaks; non-standard WS JSON protocol locks the frontend |
| Columba (torlando-tech) | Kotlin native Android | Android | Real native Android; multi-identity; BLE; voice | RNS reimplemented on JVM → drift risk; Android-only |
| lxmf-cli (fr33n0w) | Python + prompt\_toolkit | Flat files + JSON | Pluggable, headless | No attachments; debug-only |
| LXMFy | Python framework | JSON / SQLite / memory | Clean bot API | Bot-shaped, not a user inbox |
| SidebandiOS | Stale Kivy fork | — | — | Not a real native iOS client; no releases |
| lxmd | Python | LXMRouter store | Canonical headless propagation node | Daemon only |

### 1.2 What every existing client gets wrong

These are the structural gaps. Anything we build can clear them
trivially by virtue of architecture.

1. **No multi-device / multi-frontend sync.** Protocol-layer issue: a
   propagation node delivers a stored message to *one* requesting
   client and forgets it. Two clients with the same identity race the
   propagation queue. The fix is a single canonical inbox somewhere
   that every frontend is a view over — **exactly the spangap storage
   SoT model**.
2. **Ad-hoc opaque storage.** Sideband/MeshChat private SQLite, Nomad
   loose files, CLI flat JSON. No portable on-disk format for an
   LXMF inbox; export means the identity file and history dies with
   the install.
3. **Attachments fragile.** `DELIVERY_LIMIT=1 MB` and
   `PROPAGATION_LIMIT=256 KB` not negotiated; no MIME, no chunking,
   no thumbnails, no resumability. MeshChat's 10 MB extension is
   interop poison.
4. **Propagation reliability invisible.** No client surfaces
   last-sync timestamps, queue depth, drop counts, or peer health.
5. **Stamp / ticket UX missing.** Both primitives exist (`LXStamper`,
   `FIELD_TICKET`). No client automates "auto-ticket my contacts,
   require stamps from strangers."
6. **`FIELD_THREAD = 0x08` exists; nobody renders threads.** Flat
   per-correspondent lists everywhere.
7. **Blocking I/O on the UI path.** Router co-process with UI in all
   Python clients. Sideband visibly stalls during sync; MeshChat
   leaks FDs.
8. **No search.** Even where SQLite is right there.
9. **Offline-first queue UX crude.** Per-message state minimally
   exposed; no per-recipient reachability indicator.
10. **No native iOS.** Sidestepped entirely by the browser-frontend
    approach.

---

## 2. LXMF protocol facts that drive design

(Pinning to LXMF 0.9.8, `markqvist/LXMF`. File:line refs are inside
that source tree.)

### 2.1 Wire format

`LXMessage.pack()` emits:

```
destination_hash(16) | source_hash(16) | Ed25519 sig(64) | msgpack(payload)
```

- `LXMF_OVERHEAD = 112` bytes (`LXMessage.py:54-62`).
- Destination hash is **128-bit / 16 bytes** (`Reticulum.py:144`).
- Payload msgpack tuple order is **`[timestamp, title, content, fields]`**
  (`LXMessage.py:359`). The README says `[timestamp, content, title,
  fields]` — README is wrong; the code is authoritative. Title
  before content on the wire. Getting this wrong = no interop.
- Signature scope: **`dest || src || msgpack(payload-sans-stamp) ||
  SHA-256(dest||src||msgpack(payload-sans-stamp))`** — the hash is
  signed *in addition to* the data (`LXMessage.py:372-376`).
  Sign-only-data does not interop.
- `message_id = SHA-256(dest || src || packed)`. Never on the wire;
  both sides re-derive.
- `transient_id = SHA-256(encrypted_lxmf_data)` — the propagation
  store's index. **`transient_id ≠ message_id`**; conflating them
  breaks dedup or threading.

### 2.2 Delivery modes (`LXMessage.py:29-33`)

| Mode | Code | Single-packet content budget | Mechanics |
|---|---|---|---|
| OPPORTUNISTIC | 0x01 | 311 B | one RNS encrypted packet, ECDH AES-128 per packet |
| DIRECT | 0x02 | 319 B/pkt; larger via Resource | RNS Link, ratcheted; `RESOURCE` representation for large payloads |
| PROPAGATED | 0x03 | up to `PROPAGATION_LIMIT=256 KB` | shipped to propagation node, recipient pulls |
| PAPER | 0x05 | ~2210 B | `lxm://` URI, sneakernet |

Caller picks mode; the router does not auto-promote DIRECT →
PROPAGATED. State machine: `GENERATING → OUTBOUND → SENDING → SENT →
DELIVERED`. `DELIVERED` is only reachable on DIRECT link-ack —
propagation never proves delivery.

### 2.3 Stamps (`LXStamper.py`)

Hashcash PoW: `SHA-256(workblock || nonce) ≤ target`, `target = 1 <<
(256 - cost)`. Workblock is an HKDF chain whose round count is
memory-hard and scope-specific:

- Recipient-stamp: `WORKBLOCK_EXPAND_ROUNDS = 3000`
- Propagation-stamp: `1000`
- Node peering key: `25`

`STAMP_SIZE = 32` bytes. **Recipient-stamp** sits as msgpack element
[4] of the payload list. **Propagation-stamp** is appended after the
encrypted blob in the propagation envelope. Different positions,
different workblock seeds.

### 2.4 Tickets

16-byte preimages issued by recipient to known senders. Sender
computes `stamp = SHA-256(ticket || message_id)` — no PoW.
Lifetimes (`LXMessage.py:46-51`): `EXPIRY=21d`, `RENEW=14d`,
`GRACE=5d`, `INTERVAL=1d`. Validated stamps from tickets get
`stamp_value = COST_TICKET = 0x100`, ranking top.

### 2.5 Field registry (`LXMF.py:8-41`, msgpack int keys)

`0x01 EMBEDDED_LXMS, 0x02/0x03 TELEMETRY[_STREAM], 0x04
ICON_APPEARANCE, 0x05 FILE_ATTACHMENTS, 0x06 IMAGE, 0x07 AUDIO, 0x08
THREAD, 0x09/0x0A COMMANDS/RESULTS, 0x0B GROUP, 0x0C TICKET, 0x0D
EVENT, 0x0E RNR_REFS, 0x0F RENDERER, 0xFB-0xFD CUSTOM_*, 0xFE
NON_SPECIFIC, 0xFF DEBUG`. RENDERER ∈ `{PLAIN, MICRON, MARKDOWN,
BBCODE}`.

### 2.6 Router defaults to copy

`MAX_DELIVERY_ATTEMPTS=5, DELIVERY_RETRY_WAIT=10s,
PATH_REQUEST_WAIT=7s, MESSAGE_EXPIRY=30d, STAMP_COST_EXPIRY=45d,
LINK_MAX_INACTIVITY=10min, PN_STAMP_THROTTLE=180s, MAX_PEERS=20`.

### 2.7 Identity & destinations

LXMF uses `RNS.Destination.SINGLE` (Ed25519+X25519, ratchets
enforced) on aspect `lxmf.delivery` (mailbox) and `lxmf.propagation`
(propagation node), with control sub-aspect
`lxmf.propagation.control`. Identity is the standard RNS Identity
(no LXMF-specific keypair).

---

## 3. Headline architecture

### 3.1 Single task, storage as API

One FreeRTOS task `lxmf` (Core 1, prio 1, 8 KB PSRAM stack — already
scaffolded). Single wait point `itsPoll(deadline)`. Wakes on ITS
messages and storage subscription events.

**Storage is the only API.** No DC, no `LXMF_PORT_CHAT`. Every
frontend — browser FloatingWindow now, on-device LCD UI later, a
second browser session, a CLI — reads and writes storage; the
firmware lxmf task subscribes to its own subtree and reacts.

The `rns_chat:1` DC label in [component-plan.md §9](../component-plan.md)
goes away. Frontends do not need a dedicated DataChannel for LXMF.

```
Browser / Device-UI  ←─ storage subscribe ─→  s.lxmf.* / lxmf.* (read/write)
                                                       │
                                                       ▼
                                                  ┌─────────┐
                                                  │  lxmf   │ ──ITS── rnsd ─── transports
                                                  └─────────┘
                                                       │
                                              stamp worker (Core 0, idle)
```

Two consequences:

- **Multi-frontend correctness is free.** Open two browser tabs on
  the same device, mark a message read in one → the other reflects
  it instantly. No DB locks. No merge logic.
- **The control surface collapses.** Writes to storage drive every
  state change. The only thing the firmware can't observe by
  subscription is "do this RIGHT NOW with no persistent state" — and
  that pattern has its own self-clearing convention (§3.3).

### 3.2 Single writer per subkey

Safety comes from disjoint write authority over individual fields.
Firmware-owned fields and client-owned fields are strictly disjoint —
no field is mutually written. Anything the firmware reacts to lives
under `lxmf.cmd.*` or `lxmf.id.<n>.cmd.*` (see §3.3), so the firmware
**never subscribes** to `s.lxmf.id.<n>.msgs.*`; it only reads on
demand when a cmd sentinel fires.

| Field | Written by | Notes |
|---|---|---|
| `s.lxmf.id.<n>.msgs.<id>.{peer,title,content,thread,method}` | client | free to edit while `stage == draft` |
| `s.lxmf.id.<n>.msgs.<id>.read` | client | inbound flag; firmware ignores it |
| `s.lxmf.id.<n>.msgs.<id>.stage` | firmware | `draft` (initial, set by the client when creating the record) → firmware values after a sentinel fires: `stamping / queued / sending / sent / delivered / failed / cancelled / received` |
| `s.lxmf.id.<n>.msgs.<id>.{wire,message_id,attempts,next_retry_s,last_error,stamp_value}` | firmware | derived after pack |
| `s.lxmf.id.<n>.contacts.<m>.*` | client | firmware reads only |
| `s.lxmf.id.<n>.tickets.in.<m>.*` | client (manual) + firmware (auto-issue) | expiry is the truth gate; written exclusively by whichever side is responsible at the time |
| `lxmf.cmd.*`, `lxmf.id.<n>.cmd.*` | client writes the sentinel | firmware observes, processes, deletes |

**`stage` is firmware-owned after the initial `draft` write.** The
client never writes `ready` / `cancelled` / `deleted` to `stage`;
those transitions are triggered by `cmd.send` / `cmd.cancel` /
`cmd.delete` sentinels (§3.3) and the firmware stamps the field. This
removes the only shared-write surface in the schema, which means the
firmware's storage subscriptions match exclusively what clients write
— no self-notify churn on firmware-side writes.

### 3.3 Imperative actions: self-clearing keys

For one-shot commands with no persistent state, the client writes a
sentinel; the firmware processes and deletes. The key's existence
is the request, its absence is the ack. Every client→firmware
transition is a cmd sentinel — there are no "shared field" triggers.

```
lxmf.cmd.identity_new            value = optional label → firmware generates, allocates next <n>, deletes
lxmf.cmd.identity_import         value = 128-hex privkey → firmware imports, allocates <n>, deletes
lxmf.cmd.identity_destroy        value = <n>            → firmware wipes id.<n>.* + secrets.lxmf.id.<n>.*, deletes

lxmf.id.<n>.cmd.send             value = <local_key>    → firmware packs/signs/queues msg <local_key>; stage = queued
lxmf.id.<n>.cmd.cancel           value = <local_key>    → firmware OUT_CANCELs if in flight; stage = cancelled
lxmf.id.<n>.cmd.delete           value = <local_key>    → firmware wipes the msg record
lxmf.id.<n>.cmd.announce         (any)                  → emit a delivery announce for this identity
lxmf.id.<n>.cmd.prop_sync        (Phase 6) pull from this identity's outbound prop node
lxmf.id.<n>.cmd.rotate_ratchet   (Phase 4b) force ratchet rotation
lxmf.id.<n>.cmd.factory_reset    wipe this identity's inbox + tickets, keep keys
```

Identity-array operations (create / destroy) live at the LXMF root
because they affect the array itself. Per-identity actions live
inside `lxmf.id.<n>.cmd.*`.

**Subscription surface (firmware side):** one prefix sub on `lxmf.cmd.`
(installed at `lxmfInit`), plus one prefix sub on `lxmf.id.<n>.cmd.`
per allocated identity (installed by `createIdentityForSlot` /
`loadIdentityForSlot`, removed by `destroyIdentity` via
`storageUnsubscribe`). Nothing under `s.lxmf.*` or `lxmf.id.<n>.*`
(non-cmd) is subscribed — every firmware-side write is silent
relative to the firmware's own task.

A subscription to `*.cmd.*` across tasks is a free observability
channel: every pending firmware action is visible.

---

## 4. Identity model — array from day one

### 4.1 Why an array

LXMF wants multiple keypairs on one device. The case for personas /
threat-model compartmentalization / throwaway identities / imported
keys is real. Columba does it; nobody else does well. The case
against it in v1 is mostly scope, not architecture.

We make the **schema** an array from day one (`lxmf.id.<n>.*`).
Multi-identity **features** (UI picker, identity manager, per-aspect
listeners) land in a later phase. Single-identity v1 simply runs at
`n=0` and the UI never shows a picker.

### 4.2 LXMF owns its identities, not rnsd

[ports.h:74-81](../../main/ports.h#L74-L81) already binds the
identity to a destination via a **storage-key path**, not the
private bytes:

```c
typedef struct {
    uint8_t dest_hash[16];
    char    aspect[32];
    char    identity_key[40];   /* "secrets.lxmf.id.<n>.privkey"; empty → default */
    uint8_t dest_type;
    uint8_t reserved[7];
} rnsd_packet_connect_t;
```

This is exactly the [keys-by-reference](../../../memory/project_keys_by_reference.md)
convention: ITS payloads carry storage key names, never raw secrets.
LXMF generates its own Ed25519+X25519 keypairs (via an rnsd.h helper
called on the lxmf task — no rnsd round-trip required), stashes them
at `secrets.lxmf.id.<n>.privkey`, and tells rnsd "use that key" at
connect time. rnsd never needs to know LXMF has multiple identities.

### 4.3 First-boot bootstrap

```
lxmf init:
  subscribe s.lxmf.id.*
  if empty:
    write lxmf.cmd.identity_new = "main"

identity-management subscriber on lxmf.cmd.identity_new:
  generate Ed25519+X25519 via rnsd.h helper
  write secrets.lxmf.id.0.privkey
  write s.lxmf.id.0.{label="main", enabled=1, display_name=hostname, stamp_cost=16}
  delete lxmf.cmd.identity_new

per-identity subscriber wakes for id.0:
  itsConnect("rnsd", RNSD_PORT_DEST, rnsd_mailbox_connect_t{
      aspect = "lxmf.delivery",
      identity_key = "secrets.lxmf.id.0.privkey"
  })
  publish lxmf.id.0.dest_hash
```

The same loop runs for any future `cmd.identity_new` or
`cmd.identity_import`.

### 4.4 Full compartmentalization

Each `lxmf.id.<n>` is a self-contained mailbox. Contacts, tickets,
messages, drafts (just `stage=draft` records inside the per-identity
`msgs` tree) are siloed by path. Alice exists as
`id.0.contacts.<m>` AND `id.1.contacts.<m>` if you've corresponded
from both — by design. Wipe an identity, wipe its world, leave the
others untouched.

The browser conditionally shows an identity picker when
`len(s.lxmf.id.*) > 1`. Single-identity users never see one.

---

## 5. ITS port — `RNSD_PORT_DEST` bidirectional mailbox

### 5.1 Why bidirectional

LXMF must always be the active connector for both directions: rnsd
needs LXMF's `identity_key` at connect time, and only LXMF knows
where its keys live. Both inbound and outbound are the same logical
relationship — "this identity's mailbox session." Splitting them
into two ports doubles the handle count and shares no state between
them. Folding into one bidirectional handle with a type byte is the
clean shape.

### 5.2 Frame format

Connect payload:

```c
typedef struct {
    char    aspect[32];          /* "lxmf.delivery" */
    char    identity_key[40];    /* "secrets.lxmf.id.<n>.privkey" */
    uint8_t dest_type;           /* 0=SINGLE 1=PLAIN 2=GROUP */
    uint8_t reserved[23];
} rnsd_mailbox_connect_t;
```

rnsd derives `dest_hash` from `identity_pub || aspect`, registers it
for inbound dispatch, and from this point on the handle carries
both directions.

**In-band frames (the bi packet stream):**

```
LXMF → rnsd
  0x01 OUT_PACKET   | send_id(2B) | lxm_wire_bytes(112+ B)
  0x03 OUT_CANCEL   | send_id(2B)

rnsd → LXMF
  0x02 OUT_RESULT   | send_id(2B) | status(1B) | rtt_ms(4B) | hops(1B)
  0x04 IN_PACKET    | lxm_wire_bytes(112+ B)
```

`OUT_RESULT.status` — rnsd only emits this on a terminal outcome.
**rnsd never gives up on its own**; the client decides when to stop
trying by writing `OUT_CANCEL`.

| Code | Meaning |
|---|---|
| 0 | sent — opportunistic egress acknowledged by the chosen transport |
| 1 | delivered — DIRECT link-proof received |
| 2 | cancelled — client wrote OUT_CANCEL; rnsd confirms termination and frees buffers |
| 3 | evicted — rnsd hit a buffer / memory limit and had to drop. Resource concern, not policy |

There is no "failed_no_path" or "failed_max_retries" status. Path
trouble, link trouble, repeated retries are all surfaced via aux
status frames (below). The client counts retries and decides when
the message has been queued long enough; if so, it writes
`OUT_CANCEL` and treats the resulting `OUT_RESULT status=2` as
"failed."

Notes:

- rnsd reads the first 16 B of `OUT_PACKET` as the target dest_hash —
  no separate field needed; LXMF wire format already encodes it.
- For `IN_PACKET`, rnsd has decrypted the RNS layer using the
  listener's `identity_key` and hands LXMF the full plaintext LXM
  (`dest||src||sig||msgpack`). LXMF verifies dest matches the
  listener's identity as a sanity check and proceeds.
- `send_id` correlates async `OUT_RESULT` and aux status frames to
  in-flight `OUT_PACKET`s. Two bytes; LXMF tracks a small ring.

**Aux status frames (rnsd → LXMF, out-of-band progress pings).**

The client unconditionally hands rnsd an `OUT_PACKET` and forgets
about it. rnsd does whatever it has to do — request a path, wait
for it, encrypt, queue, retry — and narrates progress via ITS aux
messages keyed by `send_id`. The client never issues "request path"
itself; rnsd is the authority on its own path table.

```c
typedef struct {
    uint8_t  type;                /* status type, see table below */
    uint8_t  reserved;
    uint16_t send_id;             /* matches the outbound's send_id */
    /* type-specific payload follows, all variants ≤ 96 B total */
} rnsd_send_status_t;
```

| Type | Name | Extra payload | Meaning |
|---|---|---|---|
| 0x01 | REQUESTING_PATH | — | no cached path, rnsd has emitted an RNS path request |
| 0x02 | PATH_KNOWN | `char iface[24]; uint8_t hops; uint8_t next_hop[16]` | path resolved on `iface` via `next_hop`, `hops` away |
| 0x03 | EGRESS_QUEUED | — | encrypted + handed to a transport task |
| 0x04 | LINK_ESTABLISHING | — | DIRECT mode: link handshake in progress |
| 0x05 | RESOURCE_PROGRESS | `uint8_t pct` | DIRECT+Resource transfer progress, 0..100 |
| 0x06 | RETRY | `uint8_t attempt; uint8_t reason` | this send is being retried; `reason` ∈ path_timeout / link_failed / no_egress |
| 0x07 | PATH_LOST | — | path we were waiting for or using was dropped |

LXMF mirrors aux frames into the message record so frontends see
what's happening without subscribing to anything outside the
message. LXMF also owns the giving-up policy: it counts `RETRY`
frames against `MAX_DELIVERY_ATTEMPTS = 5` (a per-message budget,
overridable per send), and on overflow writes `OUT_CANCEL`.

```
REQUESTING_PATH        → stage = queued, last_error = "requesting path"
PATH_KNOWN             → reachability.<peer> updated; last_error cleared
EGRESS_QUEUED          → stage = sending
LINK_ESTABLISHING      → stage = sending, last_error = "establishing link"
RESOURCE_PROGRESS      → stage = sending, last_error = "transfer N%"
RETRY                  → attempts++; if attempts > budget → write OUT_CANCEL;
                         else last_error = reason, next_retry_s = ETA from frame
PATH_LOST              → last_error = "path lost, retrying"
OUT_RESULT status=0    → stage = sent
OUT_RESULT status=1    → stage = delivered
OUT_RESULT status=2    → stage = failed (after our own OUT_CANCEL) or cancelled (after user cancel)
OUT_RESULT status=3    → stage = failed, last_error = "evicted (resource limit)"
```

The distinction between user-initiated `cancelled` and policy-driven
`failed` lives entirely in LXMF: if LXMF wrote `OUT_CANCEL` because
the client (user) wrote `stage = cancelled`, the resulting
`OUT_RESULT status=2` maps to `cancelled`. If LXMF wrote
`OUT_CANCEL` because retry budget exceeded, it maps to `failed`.

UX consequence: when a user composes the first message to a never-
contacted peer, the message goes ready → stamping → queued
("requesting path") → sending ("egress queued") → sent, all
narrated live in the message detail view. No silent 14-second pause
to wonder about.

rnprobe is a thin consumer of the same frames and renders the same
narrative for path probes — "probing... requesting path... path via
tcp/0 (3 hops)... egress queued... reply in 287 ms" — instead of
today's silence-then-result.

### 5.3 Replaces RNSD_PORT_PACKET

Renames port 4 from `RNSD_PORT_PACKET` to `RNSD_PORT_DEST` (matches
the original §9 name in component-plan.md). The frame shape changes
from "one in-flight result frame per send" to the bidirectional
type-byte form above.

rnprobe migrates trivially: connect with `identity_key=""` (default)
+ target aspect, send one `OUT_PACKET`, wait for `OUT_RESULT`, close.
The bidirectional capability just isn't used.

### 5.4 Handle map

One handle per identity. LXMF maintains:

```
identity_index → handle              (for outbound dispatch)
handle         → identity_index      (for inbound dispatch)
send_id        → message_id          (for OUT_RESULT correlation, per identity)
```

No per-peer handle cache; one mailbox per identity carries all
peers.

---

## 6. Storage schema

### 6.1 Global LXMF (root)

Truly cross-identity policy. Almost nothing belongs here.

```
s.lxmf.enforce_stamps                  0|1 — strict-mode global default
s.lxmf.auto_ticket                     0|1 — auto-issue tickets to contacts (global default)
```

### 6.2 Per-identity persistent (`s.lxmf.id.<n>.*`)

```
s.lxmf.id.<n>.label                    user-facing name ("main", "public", "alt")
s.lxmf.id.<n>.enabled                  0|1, default 1 — 0 = dark: no announce, no send, inbound dropped
s.lxmf.id.<n>.display_name             included in this identity's delivery announce
s.lxmf.id.<n>.stamp_cost               leading-zero bits required from strangers (default 16)
s.lxmf.id.<n>.enforce_stamps           optional override of global
s.lxmf.id.<n>.auto_ticket              optional override of global
s.lxmf.id.<n>.privkey_ref              storage key path; default "secrets.lxmf.id.<n>.privkey"
s.lxmf.id.<n>.propagation.node         hex16 of outbound prop node, "" = disabled
s.lxmf.id.<n>.propagation.sync_interval_s

s.lxmf.id.<n>.contacts.<m>.hash        hex16 delivery destination
s.lxmf.id.<n>.contacts.<m>.nick        user label
s.lxmf.id.<n>.contacts.<m>.display_name what they announced
s.lxmf.id.<n>.contacts.<m>.icon_appearance  cached FIELD_ICON_APPEARANCE bytes
s.lxmf.id.<n>.contacts.<m>.stamp_cost  what they require of us
s.lxmf.id.<n>.contacts.<m>.trust       0=stranger 1=known 2=pinned
s.lxmf.id.<n>.contacts.<m>.muted       0|1
s.lxmf.id.<n>.contacts.<m>.last_seen   unix ms

s.lxmf.id.<n>.tickets.in.<m>.peer      hex16, we issued ticket to them
s.lxmf.id.<n>.tickets.in.<m>.ticket    hex16
s.lxmf.id.<n>.tickets.in.<m>.expiry    unix s
s.lxmf.id.<n>.tickets.out.<m>.peer     hex16, they issued ticket to us
s.lxmf.id.<n>.tickets.out.<m>.ticket   hex16
s.lxmf.id.<n>.tickets.out.<m>.expiry   unix s
```

### 6.3 Secrets

```
secrets.lxmf.id.<n>.privkey            Ed25519+X25519 private key bytes
```

Wiped by factory reset of the device, or by
`lxmf.cmd.identity_destroy = <n>`.

### 6.4 Messages (`s.lxmf.id.<n>.msgs.<id>.*`)

One record per message, cradle to grave. Same shape inbound and
outbound; `dir` discriminates. Per-identity scoped — no `our_dest`
field needed because the identity is in the path.

```
s.lxmf.id.<n>.msgs.<id>.stage          draft (client, initial) |
                                       stamping|queued|sending|sent|delivered|
                                       failed|cancelled|received (firmware)
s.lxmf.id.<n>.msgs.<id>.dir            in | out
s.lxmf.id.<n>.msgs.<id>.peer           hex16 of counterparty
s.lxmf.id.<n>.msgs.<id>.title          utf-8
s.lxmf.id.<n>.msgs.<id>.content        utf-8
s.lxmf.id.<n>.msgs.<id>.thread         hex64 root message_id, "" if standalone
s.lxmf.id.<n>.msgs.<id>.method         opp|direct|prop|auto (hint while draft, frozen on pack)
s.lxmf.id.<n>.msgs.<id>.ts             authored unix ms
s.lxmf.id.<n>.msgs.<id>.read           inbound only, 0|1

# firmware-populated after pack
s.lxmf.id.<n>.msgs.<id>.wire           packed LXM bytes (binary)
s.lxmf.id.<n>.msgs.<id>.message_id     hex64 SHA-256 (derived)
s.lxmf.id.<n>.msgs.<id>.attempts       uint
s.lxmf.id.<n>.msgs.<id>.next_retry_s   monotonic seconds
s.lxmf.id.<n>.msgs.<id>.last_error     short string for UI
s.lxmf.id.<n>.msgs.<id>.stamp_value    uint, or 0x100 for ticket
s.lxmf.id.<n>.msgs.<id>.fields_present bitmap over 0x01..0x0F
s.lxmf.id.<n>.msgs.<id>.attach_blob    hex64 of externalised attachment, "" if inline
```

**Keying:**
- Inbound: real `message_id` (hex64 SHA-256 of `dest||src||packed`).
- Outbound: local `o_<unix_ms>_<rand4>` since `message_id` isn't
  stable while a draft mutates. The `message_id` field carries the
  real value once packed, for cross-references (thread roots, ticket
  scope).

The asymmetry is fine — frontends query by indexed fields (peer,
thread, stage, dir, read), not by key.

### 6.5 Ephemeral runtime

```
lxmf.up                                       overall task health
lxmf.id.<n>.up
lxmf.id.<n>.dest_hash                         this identity's mailbox hash (hex16)
lxmf.id.<n>.stats.{sent,received,pending,failed,stamps_solved}
lxmf.id.<n>.stamping.<msgid>.progress         0..100, only while stage == stamping
lxmf.id.<n>.propagation.last_sync_ts
lxmf.id.<n>.propagation.last_sync_count
lxmf.id.<n>.propagation.queue_depth
lxmf.id.<n>.reachability.<peer>.mode          0=none 1=prop 2=path 3=link
lxmf.id.<n>.reachability.<peer>.since
```

Diff-published per the standard 1 Hz mirror cadence in
[component-plan §10.3](../component-plan.md).

---

## 7. Lifecycles

### 7.1 Outbound

```
client:    write msgs.<localkey>.{peer,title,content,thread,method,stage=draft}
client:    edit content freely (stage stays draft; firmware not subscribed)
client:    write lxmf.id.<n>.cmd.send = <localkey>     ── commit ──┐
                                                                    │
firmware:  cmd.send sentinel fires its prefix subscription          │
firmware:  reads msgs.<localkey>.* from storage           ◄─────────┘
firmware:  deletes the cmd.send sentinel
firmware:                              ──► stamping  (if recipient stamp_cost > 0 and no fresh outbound ticket)
firmware:                              ──► queued    (packed, awaiting path/link)
firmware:                              ──► sending   (OUT_PACKET written to mailbox)
firmware:                              ──► sent      (OUT_RESULT status=0 received)
firmware:                              ──► delivered (DIRECT link-ack only)
firmware:                              ──► failed    (after MAX_DELIVERY_ATTEMPTS=5)

client:    write lxmf.id.<n>.cmd.cancel = <localkey>
firmware:                              ──► cancelled (OUT_CANCEL if in flight, else stamps stage directly)

client:    write lxmf.id.<n>.cmd.delete = <localkey>
firmware:                              wipes the msgs.<localkey>.* subtree

client:    re-issue lxmf.id.<n>.cmd.send = <localkey>  after stage=failed → re-pack and try again
```

Send detail:

1. Frontend writes a new record under `s.lxmf.id.<n>.msgs.<localkey>.*`
   with `stage = draft` and the fields it cares about.
2. Frontend writes `lxmf.id.<n>.cmd.send = <localkey>` (typically in
   the same transaction as the field writes — the cJSON patch commits
   atomically, so the firmware sees the fully-populated record when
   the sentinel fires).
3. Firmware's per-identity `lxmf.id.<n>.cmd.` subscription dispatches
   on key tail; `cmd.send` runs `processSend(id, <localkey>)`. The
   sentinel is deleted as the first step.
4. If recipient `stamp_cost > 0` and no fresh outbound ticket:
   transition to `stamping`, enqueue PoW on the Core 0 worker.
   `lxmf.id.<n>.stamping.<id>.progress` published.
5. Pack: `[timestamp, title, content, fields]` msgpack (title before
   content per §2.1), sign over `dest||src||packed||SHA-256(...)`.
   Store wire bytes; populate `message_id`.
6. Write `OUT_PACKET` on the mailbox handle. Path state is rnsd's
   problem — LXMF does not check, does not signal, does not wait.
   Initial stage is `queued`; aux frames drive subsequent state.
7. Aux frames from rnsd update the message record (see §5.2
   mapping table). If `RETRY` frames accumulate past
   `MAX_DELIVERY_ATTEMPTS = 5`, write `OUT_CANCEL` and treat the
   resulting `OUT_RESULT status=2` as `stage = failed`.
8. On `OUT_RESULT status=0` (or 1 for DIRECT), terminal success.
9. DIRECT (Phase 5): same flow but rnsd-side Link establishment and
   `RESOURCE` framing happen inside rnsd; the same `OUT_PACKET` /
   aux / `OUT_RESULT` shape on the mailbox handle, with extra
   `LINK_ESTABLISHING` and `RESOURCE_PROGRESS` aux frames during
   the transfer.

The firmware does **not** subscribe to `s.lxmf.id.<n>.msgs.*`. There
is no scan-for-`stage=ready` loop. Every transition out of `draft` is
explicit — driven by a cmd sentinel — and every firmware write back
to the record (`stage`, `wire`, `message_id`, `attempts`, …) happens
silently relative to firmware's own subscriptions.

### 7.2 Inbound

```
firmware:  IN_PACKET arrives on mailbox handle for identity <n>
firmware:  parse: dest(16) | src(16) | sig(64) | msgpack
firmware:  recall src identity from rnsd's identity cache
           (if absent: Transport::request_path(src), drop this LXM — sender will retry)
firmware:  verify Ed25519 over dest||src||packed||SHA-256(...)
firmware:  if payload list has 5 elements: pop element [4] as stamp
firmware:  validate stamp/ticket against required cost (s.lxmf.id.<n>.stamp_cost)
           or any stored s.lxmf.id.<n>.tickets.in.<m>.ticket
firmware:  compute message_id; dedup against in-RAM hashlist + storage existence
firmware:  write s.lxmf.id.<n>.msgs.<message_id>.* with stage = received, dir = in, read = 0
firmware:  if sender unknown: stub s.lxmf.id.<n>.contacts.<m> with trust = 0
firmware:  if auto_ticket && contact.trust ≥ 1 && no fresh outbound ticket:
              generate ticket; embed FIELD_TICKET in our next outbound reply

client:    write s.lxmf.id.<n>.msgs.<message_id>.read = 1   (firmware doesn't care)
client:    write lxmf.id.<n>.cmd.delete = <message_id>      → firmware wipes
```

---

## 8. Stamps & tickets — automated UX

Policy the firmware enforces by default:

- **Stranger (`contact.trust = 0`):** stamp required at
  `s.lxmf.id.<n>.stamp_cost` bits (default 16). With
  `enforce_stamps = 1`, drop silently. With `= 0`, quarantine to a
  spam view (frontend filter: `dir=in && peer.trust=0 && method=opp`).
- **Known contact (`trust ≥ 1`):** ticket auto-issued on first reply
  to them; auto-renew when < 7 d remain on the existing ticket;
  expire at 21 d. PoW bypassed.
- **Pinned contact (`trust = 2`):** identical to known, plus surfaced
  in the contacts list with a pin icon. Reserved for future "always
  notify even if muted" semantics.

PoW work runs on a Core 0 idle-priority worker fed from a queue.
Progress streams to `lxmf.id.<n>.stamping.<id>.progress`. On
ESP32-S3 with hardware SHA-256 a 16-bit stamp completes in
single-digit seconds; tickets eliminate it for normal correspondents.

Tickets are embedded in our **next outbound message** to a contact,
via `FIELD_TICKET = 0x0C = [expiry_unix_s, ticket_16B]`. Never a
separate "ticket message" — they ride along.

---

## 9. Threading

`FIELD_THREAD = 0x08` populated on every reply with the **root**
message_id (the first message in the chain that has no thread).
Frontends group conversations two-level: peer → threads → messages,
filtering `s.lxmf.id.<n>.msgs.*` by `peer` then `thread`. Old
clients see opaque thread bytes; nothing breaks.

---

## 10. Attachments

**Phase 4a/4b:** drop oversize at compose time. Any LXM whose total
packed size > 311 B (OPPORTUNISTIC limit) refuses to enter `stage =
ready` and the frontend explains why.

**Phase 5 (with DIRECT + Resource):**
- ≤ 8 KB: inline in `FIELD_FILE_ATTACHMENTS = [[filename, bytes], …]`.
- \> 8 KB: blob store at `data/lxmf-blobs/<sha256>`. The LXM wire
  carries `FIELD_FILE_ATTACHMENTS = [[filename, blob_hash_only_32B],
  …]` plus a custom-type marker indicating our convention. Stock
  LXMF clients see the filename and a small placeholder; ours
  resolve the blob hash via an HTTP endpoint the lxmf task exposes
  (`GET /lxmf/blob/<hex>`).

The blob store is content-addressed, so duplicate sends of the same
attachment cost zero extra storage. Garbage collected when no
`s.lxmf.id.<n>.msgs.<id>.attach_blob` field references the hash.

---

## 11. Visibility / UX surface

What every existing client hides, we surface from the storage
subscription.

Per-message views:
- **Conversation list:** filter `s.lxmf.id.<n>.msgs.*` by `peer`,
  group by `thread`, sort by `ts`.
- **Outbox view:** filter `dir=out && stage ∉ {sent, delivered,
  failed, cancelled, deleted}`.
- **Per-message detail:** live `stage`, `attempts`, `next_retry_s`,
  `last_error`, `stamping.<id>.progress`.

Status FloatingWindow:
- **Reachability dots per contact** from `lxmf.id.<n>.reachability.*`.
  Green = link-live, yellow = path-known, orange = prop-only, red =
  no-route.
- **Propagation node:** address, last sync time, last sync count,
  current queue depth. Force-sync button writes
  `lxmf.id.<n>.cmd.prop_sync`.
- **Active stamping:** live progress bars for any PoW in flight.
- **Stats per identity:** sent / received / pending / failed.

Search: each frontend builds its own search index over `.title +
.content` in memory / IndexedDB as records stream in. No
inverted-index in storage (write amplification not worth it).

---

## 12. Phasing

| Phase | Lands |
|---|---|
| **4a** | identity bootstrap, mailbox port + frame parser, opportunistic send/recv, sig + dedup, message storage, browser chat FloatingWindow, threading, contacts |
| **4b** | stamps + tickets + auto-ticket + spam gate; Core 0 PoW worker; progress streaming |
| **5**  | DIRECT mode, Resource attachments, blob store, backchannel link reuse |
| **6**  | PROPAGATED mode, prop-node `/get` client, sync UX, propagation reachability |
| **7**  | run as a propagation node ourselves (peer gossip, `/offer`, autopeer, MAX_PEERS=20) |
| **8+** | full multi-identity UX (picker, generate/import flows, per-identity announce control) |

---

## 13. Interop gotchas to bake in from day one

1. **Payload tuple order** is `[timestamp, title, content, fields]`
   (`LXMessage.py:359`). README is wrong; code wins.
2. **Signature scope** is `dest || src || packed || SHA-256(dest ||
   src || packed)` (`LXMessage.py:372-376`). Hash is signed
   alongside the data.
3. **Destination hash is 16 bytes**, not 10 (`Reticulum.py:144`).
4. **Recipient stamp** sits as msgpack list element [4] inside the
   payload. **Propagation stamp** is appended after the encrypted
   blob in the propagation envelope. Different positions, different
   workblock seeds.
5. **`transient_id ≠ message_id`.** Propagation stores index by
   transient; our inbox indexes by message_id. Conflation breaks
   dedup or threading.
6. **Workblock HKDF rounds** are 3000 / 1000 / 25 by scope. Don't
   unify.
7. **`FIELD_TICKET = 0x0C`** value is `[expiry_unix_s, ticket_16B]`,
   not raw bytes.
8. **Backchannel link** (Phase 5): after a successful DIRECT
   delivery, re-identify as our own delivery destination on the same
   Link so the recipient can reply without a new link. Skipping this
   halves the latency advantage of DIRECT.

---

## 14. What this gets us that no one else has

- **Multi-frontend inbox** with zero merge logic, courtesy of
  storage SoT.
- **Documented, portable, content-addressed on-disk format** — the
  inbox is just message_id → wire bytes + sidecar, every message
  re-verifiable from its bytes alone.
- **Real threading** rendered, not just transported.
- **Stamp + ticket automation** that makes spam-resistance usable.
- **Reachability indicator** per contact, per delivery mode.
- **Propagation visibility** absent everywhere else in the
  ecosystem.
- **No blocking UI**: backend in firmware, frontends async over
  storage subscription.
- **Multi-identity from day one** at the schema level, with full
  compartmentalization when the feature lands.
- **Clean extension path** to DIRECT / PROPAGATED / propagation-node
  / multi-device replication without schema migration.

---

## 15. Open questions for follow-up

- **Outbound delivery-result semantics for OPPORTUNISTIC.** RNS
  opportunistic packets get no native ack. `OUT_RESULT status=0` for
  opp means "rnsd handed bytes to the egress transport" — promote
  that to `sent`, never to `delivered`. Worth confirming with the
  rnsd implementation when Phase 4a lands.
- **Retry budget per send.** `MAX_DELIVERY_ATTEMPTS = 5` is the
  default; some send paths might want different policy (e.g., a
  one-shot probe might give up after 1 retry; a critical reply
  might allow 50). Plumbing this as an optional field on the
  `OUT_PACKET` framing vs. an LXMF-side counter is a Phase 4a
  decision. Default to LXMF-side counter — keeps the rnsd port
  surface minimal.
- **Audio / voice (LXST).** Out of scope for v1; the `FIELD_AUDIO =
  0x07` framing is reserved and our messages with it will display
  as "[audio message — unsupported]" until we decide to support it.
- **Group conversations.** `FIELD_GROUP = 0x0B` exists. Deferred
  past Phase 7. Storage model would add `s.lxmf.id.<n>.groups.<g>.*`
  but no other shape change is required.
- **Custom-type marker for blob-hash attachments.** Concrete byte
  value to pick from the `0xFB..0xFD` range; defer to Phase 5
  alongside DIRECT-mode work.
