# lxmf — LXMF messaging

The `lxmf` component is the device's LXMF mailbox: it sends and
receives signed [LXMF](https://github.com/markqvist/LXMF) messages over
Reticulum, holds up to four independent identities, and advertises each
one so other nodes can reach it.

This document describes lxmf **as a black box** — what it does and how
you drive it from the outside (a browser frontend, the on-device UI, a
CLI, another task). It does not cover how it is built; for that see
[docs/internals/lxmf.md](internals/lxmf.md).

## What it does

- **Messaging.** Pack, sign, and transmit outbound LXMs; verify,
  de-duplicate, and store inbound LXMs. Signatures are checked against
  the sender's Reticulum identity — forged or corrupt messages are
  dropped silently.
- **Identities.** Up to `LXMF_MAX_IDENTITIES = 4` independent mailboxes
  (`lxmf.delivery` destinations), each with its own keypair, contacts,
  and message history, fully siloed. **No identity is created
  automatically** — a device with none runs as a transport-only node
  (it relays and tracks the network but has no mailbox of its own).
- **Delivery mode is automatic.** You do not choose how a message goes
  out. lxmf picks per message: a single opportunistic packet for small
  messages, a Reticulum Link (DIRECT) otherwise, and a Resource
  transfer for large bodies — transparently, end to end, interoperable
  with stock LXMF peers (Sideband, NomadNet, MeshChat).
- **Announces.** Each identity periodically announces its delivery
  destination. Inbound announces from the whole network are collected
  into a shared, cross-identity **announce catalogue** of everyone the
  device has heard of.

## The contract: storage *is* the API

There is no DataChannel and no dedicated ITS port for consumers.
**Everything is done by reading and writing storage keys.** The lxmf
task subscribes to its own command keys and reacts; it publishes
message records and live state back into storage.

```
Browser / Device-UI / CLI ──storage read+write──► s.lxmf.* · lxmf.* · secrets.lxmf.*
                                                          │  (lxmf subscribes to *.cmd.*)
                                                          ▼
                                                     ┌────────┐
                                                     │  lxmf  │ ──► rnsd ──► transports
                                                     └────────┘
```

Consequences you can rely on:

- **Multi-frontend coherence is free.** Two browser tabs, the CLI, and
  the device UI all see the same inbox. Mark a message read in one and
  the others reflect it immediately. No locking, no merge.
- **You never call rnsd or the network directly.** Every action is a
  storage write.
- **Single writer per field.** Client-owned and firmware-owned fields
  are disjoint (see the schema below). You create a message record and
  set its content; the firmware owns `stage`, `wire`, `message_id`,
  `attempts`, `last_error`.

### Key namespaces

| Prefix | Persistence | Who writes | Purpose |
|---|---|---|---|
| `s.lxmf.*` | survives reboot | client + firmware (disjoint fields) | identities, messages, contacts, config |
| `secrets.lxmf.*` | survives reboot | firmware | private keys (never leave the device) |
| `lxmf.*` | RAM, re-published ~1 Hz | firmware | live status, stats, announce catalogue |
| `lxmf.cmd.*`, `lxmf.id.<n>.cmd.*` | transient | client writes, firmware deletes | imperative actions (see below) |

## Commands — self-clearing keys

Imperative actions are **sentinels**: you write the key, the firmware
performs the action and deletes the key. Presence = request in flight;
absence = done. This is the only way to make lxmf *do* something.

**Identity-level** (`lxmf.cmd.*`):

| Key | Value | Effect |
|---|---|---|
| `lxmf.cmd.identity_new` | optional label | generate a new identity, allocate the next slot, bring its mailbox up |
| `lxmf.cmd.identity_import` | 128-hex private key | import a key into a new slot |
| `lxmf.cmd.identity_destroy` | `<n>` | wipe identity `n` — its secret, all its storage, its subscriptions |

**Per-identity** (`lxmf.id.<n>.cmd.*`):

| Key | Value | Effect |
|---|---|---|
| `lxmf.id.<n>.cmd.send` | `<peer>/<key>` | pack, sign, and transmit the draft at `s.lxmf.id.<n>.msgs.<peer>.<key>` |
| `lxmf.id.<n>.cmd.cancel` | `<peer>/<key>` | cancel an in-flight send (else mark it `cancelled`) |
| `lxmf.id.<n>.cmd.delete` | `<peer>/<key>`, or `<peer>` | delete one message; bare `<peer>` (or `<peer>/`) deletes the whole conversation |
| `lxmf.id.<n>.cmd.announce` | any | emit a delivery announce for identity `n` now |

To make a sentinel write *atomic with its data*, write the data fields
and the sentinel in one `storageBegin()/storageEnd()` transaction — the
firmware then sees a fully-populated record the instant the sentinel
fires.

## Identities

Create or destroy from any task via [lxmf.h](../main/lxmf.h):

```cpp
int  lxmfCreateIdentity (const char* display_name, bool sync = false);
bool lxmfDestroyIdentity(int n,                    bool sync = false);
```

These just write the corresponding `lxmf.cmd.*` sentinel. With
`sync = true` the call blocks (≤ 5 s) until the firmware has finished,
then returns the allocated slot (create) or success (destroy);
`sync = false` returns immediately. The CLI's `lxmf create` /
`lxmf destroy` use the sync form so they can report the outcome.

Per loaded identity you can observe:

```
s.lxmf.id.<n>.label          "main" | "imported" | user-set
s.lxmf.id.<n>.enabled        1 (default) — 0 = identity dark: no announce, no send, inbound dropped
s.lxmf.id.<n>.display_name   utf-8, advertised in announces
lxmf.id.<n>.up               1 once the mailbox is connected
lxmf.id.<n>.dest_hash        hex16 — this identity's lxmf.delivery address
```

## Sending a message

1. Write a message record. Messages are stored **per contact**:
   `<peer>` is the 32-hex destination, `<key>` a local key (convention
   `o_<unix_ms>_<rand4>`):

   ```
   s.lxmf.id.<n>.msgs.<peer>.<key>.dir      = out
   s.lxmf.id.<n>.msgs.<peer>.<key>.peer     = <32-hex destination>
   s.lxmf.id.<n>.msgs.<peer>.<key>.title    = <utf-8>
   s.lxmf.id.<n>.msgs.<peer>.<key>.content  = <utf-8>
   s.lxmf.id.<n>.msgs.<peer>.<key>.thread   = <hex64 root message_id, or "">
   s.lxmf.id.<n>.msgs.<peer>.<key>.stage    = draft
   ```

   (`peer` is both the path segment and a field — the field is kept for
   the indexed-query contract.) While `stage == draft` you may edit
   freely — the firmware is not watching.

2. Commit with `lxmf.id.<n>.cmd.send = <peer>/<key>` (ideally in the
   same transaction as step 1).

3. Watch `s.lxmf.id.<n>.msgs.<peer>.<key>.stage` progress through:

   ```
   draft → queued → sending → sent          (delivered only on a proven DIRECT/Resource transfer)
                            ↘ failed | cancelled
   ```

   `last_error` carries a short human string during the attempt
   (`requesting path`, `establishing link`, …). `attempts` counts
   retries.

**Delivery method is automatic.** lxmf chooses opportunistic vs DIRECT
vs Resource by size. You can hint with
`s.lxmf.id.<n>.msgs.<peer>.<key>.method` (`opp` | `direct` | `auto`) or set a
per-identity default `s.lxmf.id.<n>.default_method` (default `auto`).
`auto` = opportunistic if it fits one packet, otherwise DIRECT, with a
Resource transfer for large bodies.

**Cancel** with `lxmf.id.<n>.cmd.cancel = <peer>/<key>`; **delete** with
`lxmf.id.<n>.cmd.delete = <peer>/<key>` (or a bare `<peer>` to delete
the whole conversation). There is **no automatic retry** — to re-send
after `stage=failed`, write `cmd.send` again.

## Receiving a message

Inbound LXMs are verified, de-duplicated, and stored at
`s.lxmf.id.<n>.msgs.<peer>.<message_id>.*` with `stage=received`,
`dir=in`, `read=0`. `<peer>` is the sender's 32-hex destination; the
64-hex key is the real LXMF `message_id`. Dedup survives reboots, so
the same message arriving twice is stored once.

The sender is stubbed into the per-identity address book at
`s.lxmf.id.<n>.contacts.<peer>.*` (with `trust=0`) on first contact,
and `last_seen` is refreshed. To mark a message read, set
`s.lxmf.id.<n>.msgs.<peer>.<message_id>.read = 1` (the firmware ignores
this field — it is purely for your UI).

A message from a sender the device has never heard announce is **dropped
and not buffered**; the device asks the network for a path and the
sender's normal retransmit delivers it once the path resolves.

## Announces

- Each enabled identity announces automatically: ~10 s after an
  interface comes up, then every `s.lxmf.announce_interval_s` seconds
  (default 1800; `0` disables periodic). Force one with
  `lxmf.id.<n>.cmd.announce`.
- Every `lxmf.delivery` announce the device hears is written to the
  **announce catalogue**, a shared cross-identity catalogue of
  everyone heard on the mesh:

  ```
  lxmf.announces.<dest_hex> = "<last_s>|<cost>|<hops>|<ratchet>|<name>"
  ```

  It is ephemeral (RAM), bounded by `s.lxmf.max_announces` (default
  2048; oldest entry evicted on overflow), and is the source for a
  "people we've heard of" picker. It is distinct from per-identity
  `contacts`, which is each identity's own address book.

## CLI — `lxmf`

All verbs act on the **selected identity** (`s.lxmf.cli.selected_id`,
default 0) unless noted.

```
lxmf create <name>          generate a new identity (prints the slot, or failure)
lxmf destroy <n>            wipe identity at slot <n>
lxmf id                     list identities (* = selected)
lxmf id <n>                 switch selected identity
lxmf chats                  list conversations (one row per peer; numbered)
lxmf msgs [<arg>]           no arg = chats; <peer> = that thread (numbered,
                            newest first); a bare <stage> word = cross-
                            conversation filter
lxmf read <n>               print message #n from the last `lxmf msgs`; marks it read
lxmf contacts               list this identity's contacts (numbered)
lxmf announces [<arg>]      cross-identity announce catalogue; <arg> = 32-hex
                            (one row) or a name substring; no arg = full dump
lxmf send <peer> <msg>      send; <peer> = 32-hex, a number from the last
                            numbered listing, or a name substring
lxmf announce               announce the selected identity now
```

Numbered listings (`chats`, `msgs`, `contacts`, `announces`) feed the
index arguments of `read` / `send` / `msgs <#>`. A name substring with
multiple matches prints a disambiguation list instead of sending.

## Configuration knobs (`s.lxmf.*`)

```
s.lxmf.announce_interval_s    periodic re-announce seconds (default 1800; 0 = off)
s.lxmf.max_announces          announce-catalogue entry cap (default 2048)
s.lxmf.max_resource_size      largest inbound Resource message accepted (default 262144)
s.lxmf.cli.selected_id        CLI's selected identity (default 0)
s.lxmf.debug.only_local       quiet announce-churn logs at debug level (default 0)
s.lxmf.id.<n>.default_method  per-identity send method: auto|opp|direct (default auto)
```

## Known limitations

- **Opportunistic size budget.** A message small enough to go
  opportunistically must keep `title + content + ~32 B` under ~311 B.
  Larger messages automatically use DIRECT/Resource instead; an
  explicit `method=opp` that exceeds the budget fails with
  `last_error = "too large for opportunistic"`.
- **No message retention/eviction.** Stored messages accumulate
  indefinitely. Message bodies live in the RAM-backed config store on a
  small (256 KB) on-device partition; sustained inbound traffic with
  large bodies can exhaust it and degrade the node. Bounding this is a
  tracked follow-up tied to moving storage to SD — until then, a busy
  mailbox needs periodic `cmd.delete` housekeeping.
- **`cmd.delete` is a single sentinel key.** Writing it repeatedly in
  quick succession can overwrite an unprocessed value; delete one
  message at a time (settle between writes). A delete may not be
  reflected by an immediately-following `show` (read-after-delete
  consistency caveat).
- **At most 8 sends in flight per identity.** A 9th `cmd.send` while 8
  are unfinished fails with `last_error = "outbox full"`.
- **`sent` is not `delivered`.** Opportunistic messages get no network
  ack — `sent` means "handed to a transport." Only a proven
  DIRECT/Resource transfer reaches `delivered`.
- **Stamps, tickets, spam-gating, and propagation are not enforced
  yet.** Strangers are not currently challenged for proof-of-work, and
  there is no propagation-node (store-and-forward) support — both are
  planned. Multi-identity exists at the schema level but there is no
  multi-identity UX yet.

For protocol details, the upstream LXMF reference, the wire format, and
the internal architecture, see [docs/internals/lxmf.md](internals/lxmf.md).
