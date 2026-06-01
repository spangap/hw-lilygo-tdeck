# Evictable / segmented storage

Bound the firmware's RAM and flash growth as contact lists and message
chains grow, without rewriting the storage stack. This is the doc
[lxmf-messages-per-contact.md](lxmf-messages-per-contact.md) pointed at
as "later": that change made each conversation a `storageDeleteTree`-able
subtree (`s.lxmf.id.<n>.msgs.<peer>.…`); this one hangs *persistence
granularity* on that seam, and sets the roadmap for retention + eviction.

Status: **step 1 (per-conversation files) implemented 2026-05-24, not yet
built/HW-verified.** Eviction and retention are designed here but **not
implemented**. The platform mechanism (`storageNewTreeFile`) lives in
spangap-core — see [spangap storage.md](../../../spangap/docs/storage.md)
"External storage files".

---

## 1. The problem

Everything persisted lives in one in-RAM cJSON tree (`cfgRoot`) flushed to
`<stateDir>/storage/root.json`. Two ways that grows without bound:

- **Flash write-amplification.** Before this change, `s.lxmf.id.<n>.msgs.*`
  lived in `root.json`, so *every* message — a new arrival, a stage
  transition mid-send — rewrote the **entire** config blob. Chatty
  conversations churned flash on every keystroke of activity.
- **RAM + browser-dump bloat.** The whole tree is resident and
  `cJSON_Duplicate`d to the browser on connect. Unbounded history means
  unbounded RAM and an ever-larger connect dump. (This is the same family
  as the rnsd path-table `/state`-full + wdt incident — see §4.)

## 2. Step 1 — per-conversation files (implemented)

Lean into the existing external-file mechanism (the one `s.time.zones`
uses), but register prefixes **at runtime** instead of only discovering
factory files at boot.

**Platform (spangap-core):**

- `storageNewTreeFile(prefix)` — registers `prefix` as its own external
  file (`<stateDir>/storage/external/<prefix>.json`). RAM-only (no
  caller-side fs I/O → safe on `itsPoll` tasks); the file is created on
  the next flush by `writeExternalFile` once a key under it dirties.
  Idempotent.
- `storageDeleteTree(keyOrPrefix)` now also flags any external **at or
  below** the key (`markExternalsDeletedUnder`) for `fs_remove` +
  unregister on the next flush — so deleting a conversation or identity
  drops its `.json` instead of leaving an orphan that resurrects on
  reboot. Wired into the browser-null path (`mergeIncomingPatch`) too.

**reticulous (lxmf.cpp):**

- `ensureConvFile(n, peer)` → `storageNewTreeFile("s.lxmf.id.<n>.msgs.<peer>")`,
  called at the three conversation-creation points: `processReady` (the
  outbound chokepoint both browser and CLI sends reach via `cmd.send`),
  the inbound store (`onInboundLxm`), and `cliEnqueueSend`.

**Behaviour:**

- A message write rewrites only that conversation's small file; `root.json`
  stops churning and shrinks (the subtree is detached from it on save).
- **Browser-composed drafts self-heal.** The browser writes draft fields
  over the `storage:1` DC *before* `cmd.send`, so they briefly land in
  `root.json` (no external registered yet). When `cmd.send` reaches
  `processReady`, `ensureConvFile` registers the prefix and the send
  writes dirty it; the next flush captures the whole subtree (draft +
  status) into the file and `withExternalsDetached` removes it from
  `root.json`. No loss.
- Reboot: `scanExternals` re-discovers the files, `loadExternals` mounts
  them; prefixes round-trip identically.

**Deliberately NOT done here:** no eviction (subtrees stay resident and
still sync to the browser) and no retention cap. This step only fixes
flash write-amplification and gives later steps a per-conversation file to
evict/cap. A draft composed then *abandoned* (never sent) stays in
`root.json` — tiny and transient; we don't touch the browser to fix it.

## 3. Step 2 — retention (designed, not implemented)

The `/state`-full risk is *accumulation*, not write rate. Cheapest guard
that needs no new format: a per-conversation **retention cap** (keep last
N messages or N KB), enforced on insert in `onInboundLxm` /
`cliEnqueueSend` by deleting the oldest `msgPath` records. Because a
conversation is now its own file (step 1) and its own
`storageForEach`/`collectTokens` subtree, the retention walk is per-peer,
not an O(all) scan of the whole pile.

## 4. Step 3 — eviction, and why rnsd's tables are NOT the target

The tempting next step is a "load only what's in use" disk store. **A
code audit says it's the wrong tool for rnsd's transport/identity
tables** (Identity.cpp, Transport.cpp, microStore/HeapStore.h):

- **They're tiny and capped.** Keys are **16 bytes** (truncated dest
  hash), values ~100–1000 B; `RNS_KNOWN_DESTINATIONS_MAX` /
  `RNS_PATH_TABLE_MAX` / `RNS_ANNOUNCE_TABLE_MAX` default **100**
  (known-destinations raised to 1000 at runtime in rnsd.cpp). Worst case
  ~hundreds of KB on a device with ~7 MB free PSRAM.
- **They're walked constantly.** `HeapStore::put()` sweeps the whole map
  on every insert; known-destinations is fully iterated on every insert
  (cull), the announce table every ~1 s, reverse/link tables every 60 s.
  A point-lookup-on-disk store is backwards for a table iterated every
  second — you'd re-read it from flash repeatedly.
- **The growth incident was a missing cap, fixed in RAM.** The path
  table's `BasicHeapStore` ran TTL-only (24 h) with no count cap; the fix
  is `Transport::path_table_maxsize()` → `_path_store.set_max_recs()`
  ("uncapped until now", rnsd.cpp), not a move to disk.
- **The real gap for rnsd is persistence, not eviction.** `Persistence.h`
  is fully stubbed (the fork dropped ArduinoJson; plan §10.5 = "rnsd owns
  persistence end-to-end"). These tables are small + walked, so the right
  persistence is a **whole-table snapshot loaded entirely at boot** — the
  *opposite* of lazy — behind the existing `TypedStore`/`Codec` seam
  (`NewPathTable` already uses it). If snapshotted, cap × value size must
  fit the medium: a 1000-entry store is ~0.6–1 MB, so it **cannot** sit in
  the 128 KB `/state` — that's the only reason flash needs a *lower* cap
  than SD.

So the bespoke "fetch-one, evict-block" engine's real home is **message
bodies** (large, accumulating, point-accessed per conversation,
browser-windowable), not rnsd's tables.

When step-1 files prove they need true eviction (subtree dropped from
`cfgRoot`, faulted back on access), note the key safety property: a reader
gets a **copy** over the wire / by value, never a live pointer into
evictable memory, so eviction between operations needs no refcount — only
"don't evict mid-write/mid-snapshot," which single-writer-per-mount gives.

## 5. Graduation triggers (don't pre-build)

Instrument, then act per signal — and each fires for **one** mount at a
time, upgraded behind the external-file seam without touching callers:

- whole-file rewrite of one conversation shows in the flash-write budget →
  append-delta backing instead of rewrite-whole-file;
- the connect dump or a single conversation gets too large to ship whole →
  browser windowing (LIST/cursor/SUBSCRIBE; see
  [lxmf-browser-client.md](lxmf-browser-client.md));
- a wide store's cold read on deep-sleep wakeup gets slow → a
  seek-navigable on-disk format with a stateless `peek`.

Until a trigger fires: per-conversation files (step 1) + retention
(step 2) keep RAM, flash, and `root.json` bounded with none of the
database.
