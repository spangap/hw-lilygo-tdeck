# LXMF — messages stored per contact

Re-key the per-identity message store from one flat pile to a per-peer
subtree. This is **layout work only**: it creates the subtree seam that
[evictable_storage.md](evictable_storage.md) (later) hangs retention /
eviction policy on. It decides *no* eviction policy itself, and it is
mergeable + HW-verifiable on its own (send/recv round-trip unchanged).

Status: implemented 2026-05-17; HW sanity-checked 2026-05-18 (outbound
per-contact storage shape, `cmd.send=<peer>/<key>` sentinel split,
single + whole-conversation `cmd.delete` — the last surfaced and fixed
a pre-existing trailing-dot `storageDeleteTree` no-op). Inbound over a
live peer not yet exercised (needs a shared rnsd path — device dials
`rns.beleth.net`, not the dev-loop rnsd). Companion to the
LXMF subsystem docs — [../lxmf.md](../lxmf.md) (black box) and
[../internals/lxmf.md](../internals/lxmf.md) (reach-inside); those
carry the authoritative schema, this carries the rationale.

---

## 1. The change

Before, every message for an identity was one flat namespace, with
`peer` as a queryable *field*:

```
s.lxmf.id.<n>.msgs.<key>.<field>          key = message_id (in) | o_<ms>_<rand> (out)
```

After, `peer` is a **path segment** and a conversation is a subtree:

```
s.lxmf.id.<n>.msgs.<peer>.<key>.<field>
```

`msgs.<peer>` is now a sibling of the existing `contacts.<peer>` — the
per-identity tree is cleanly peer-partitioned. The `peer` *field* is
still written (by the client on a draft, by the firmware on inbound) so
the documented indexed-field contract and consumer reads keep working;
it is redundant with the path segment, deliberately, at the cost of one
leaf.

Why this and not anything bigger: eviction wants a contact's history as
a `storageDeleteTree`-able subtree and a per-conversation retention walk
that does not scan the whole pile. A flat pile makes both an O(all)
group-by. Fully unifying `contacts.<peer>` and `msgs.<peer>` under one
`peer.<peer>.*` tree (so destroying a contact drops their messages in
one op) is a real option but a wider blast radius — left as an
evictable_storage.md decision, **not** done here.

`peer` is known at every write site (it is `src` on inbound, a required
draft field on outbound), so nothing needs new information — only a
deeper path.

## 2. The one real subtlety: the sentinel value

`cmd.send` / `cmd.cancel` / `cmd.delete` carried `<key>` as the value;
`handleIdCmd` did `processX(id, val)`. With messages no longer in a flat
namespace, `<key>` alone no longer locates a record. The value format
becomes:

```
lxmf.id.<n>.cmd.send   = <peer>/<key>      peer + key both required
lxmf.id.<n>.cmd.cancel = <peer>/<key>
lxmf.id.<n>.cmd.delete = <peer>/<key>      one message
lxmf.id.<n>.cmd.delete = <peer>/           whole conversation (key empty)
lxmf.id.<n>.cmd.delete = <peer>            whole conversation (no slash)
```

`handleIdCmd` splits on the first `/`. The whole-conversation delete
form is the primitive evictable_storage.md will reuse (and what the
CLI's `lxmf delete chat` would post). This was chosen over "firmware
scans every `msgs.*` subtree for `<key>`", which would reintroduce
exactly the O(all) walk the re-key removes.

## 3. Plumbing-first chokepoints (firmware)

All in [../../main/lxmf.cpp](../../main/lxmf.cpp):

- **`msgPath(n, peer, key, field)`** — the single chokepoint, now
  `s.lxmf.id.%d.msgs.%s.%s.%s`. Every message storage access flows
  through it.
- **`outbound_t`** — gains `std::string peer;` so `applyOutResult` /
  `applyOutStatus` / `resolveDirectSends` can rebuild the path from a
  `send_id` without re-reading storage.
- **`processReady`** — takes `(id, peer, key)`; `peer` arrives from the
  sentinel (param is authoritative — it *is* the path segment), no
  longer read from storage to find the record.
- **`handleIdCmd`** — splits `val` into `peer`,`key`; threads both into
  `processSend` / `processCancel` / `processDelete`.
- **`processDelete`** — `s.lxmf.id.<n>.msgs.<peer>.<key>` for one
  message; `s.lxmf.id.<n>.msgs.<peer>` (empty key) for a whole
  conversation. **No trailing dot**: `storageDeleteTree`/`deleteFromTree`
  split on the last dot, so a trailing-dot prefix silently no-ops
  (this latent bug predated the rework — the old flat `processDelete`
  passed `…msgs.<id>.` and never actually deleted; see
  [../internals/lxmf.md](../internals/lxmf.md) Delta C.5 correction).
  `msgPrefix`'s trailing dot is for `storageForEach`/`collectTokens`
  only — `processDelete` builds its own dotless path.
- **`onInboundLxm`** — `peer` = `src` hex (already computed for the
  contact stub); dedup existence check + persist move under
  `msgs.<peer>.<mid>`.
- **`cliEnqueueSend`** — writes the draft under the peer subtree and
  posts `cmd.send = <peer>/<key>` in the same `storageBegin/End`.

Path depth: `navigatePath`/`navigateOrCreate` in spangap-core
`storage.cpp` are depth-unbounded `while (*p)` walks — only the 96-byte
per-segment cap matters; a 32-hex peer segment is well under it. No
max-depth constant exists. No migration / version bump: install base is
zero — change the layout, reflash.

## 4. CLI — looking at messages

The per-peer store makes the CLI naturally two-level (conversation list
→ thread), matching [lxmf-browser-client.md](lxmf-browser-client.md)'s
"conversation unit is `peer`". Selector grammar is the existing one
(32-hex / number from the last numbered listing / name substring;
multi-match → disambiguation).

### `lxmf chats` — conversation list (new verb)

`collectTokens("s.lxmf.id.<n>.msgs.")` now yields **peer** tokens (one
walk). One row per peer subtree: count, unread, last-message ts/title,
resolved name (contact `display_name` → announce catalogue → truncated
hash). Populates the peer list so `lxmf msgs <#>` drills in.

```
> lxmf chats
id 0  3 conversation(s)
  #   peer             msgs  unread  last        who
  1   a3f19c2…(16hex)  6     2       1715900000  alice
  2   7b8041a…         3     0       1715896400  NomadNet relay
  3   c4d2f08…         1     1       1715885200  (unknown)
```

### `lxmf msgs [<arg>]`

```
lxmf msgs              → conversation list (alias of `lxmf chats`)
lxmf msgs <peer|#>     → that thread, numbered, newest-first
lxmf msgs <stage>      → cross-conversation filter (bare stage word)
```

`<stage>` ∈ {draft,queued,sending,sent,delivered,failed,cancelled,
received} is checked first, so it is unambiguous against a peer hash /
index / name. The thread listing feeds `read`.

### `lxmf read <n>`

Unchanged in feel; the listing now records `(peer,key)` pairs so the
deeper path resolves. Marks inbound read.

### Untouched at the user surface

`send` / `contacts` / `announces` grammar is unchanged — peer selectors
already work this way. Whole-conversation delete (`lxmf delete chat
<peer>` → `cmd.delete = <peer>/`) falls out of §2 for free but is an
eviction-adjacent verb; noted for evictable_storage.md, not built here.

## 5. Out of scope (evictable_storage.md owns these)

Retention caps, LRU, per-conversation budgets, the `/state` → SD move,
blob-scale body externalisation. This pass only creates the seam:
per-peer `storageDeleteTree`, and a per-peer iteration boundary a
retention walk can attach to.
