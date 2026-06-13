# nomad — Nomad Network pages on spangap

> **Key-shape update (2026-06-12):** browsing is now session-based —
> 7 parallel browser contexts (0–5 web tabs, 6 LCD), each with its own
> Link. `nomad.nav.*` / `nomad.page.*` below read as
> `nomad.s<sid>.nav.*` / `nomad.s<sid>.page.*`, and cmd values carry a
> `<sid>|` prefix. Bookmarks moved to opaque ids addressing host AND
> path: `s.nomad.bookmarks.<id>` = `"<hash>[:<path>]|<name>|<note>"`
> (no longer keyed by dest hash), and the SPA publish cap is now
> compile-time `NOMAD_MAX_PAGE_PUBLISH` (128 KB), not
> `s.nomad.max_page_publish`. See `nomad/INTERNALS.md` for the current
> shape.

> **Status — client implemented + hardware-verified (2026-05-25).** The
> browser/client half of Nomad Network's *page/web* layer is live: the
> `nomad` task, the SPA "Nomad Browser" window (TS Micron renderer), and
> the on-device "Nomad" LCD launcher (C++ Micron→LVGL renderer). Page
> fetch over a Reticulum Link — including **bzip2-compressed** Resource
> pages — has been fetched, decompressed, and rendered on the T-Deck.
> Forms (Phase 4) are wired end-to-end and proven Python↔Python; a
> desktop-`nomadnet` field/var interop pass is the remaining gap. The
> **node/server** half (hosting pages) is not built — see *Server,
> later*. Phasing detail lives in [`plans/nomad.md`](plans/nomad.md);
> this is the black-box doc for consumers.

Read [`lxmf.md`](lxmf.md) and [`rnsd.md`](rnsd.md) first — nomad is
modelled on lxmf (storage-as-API, cmd sentinels, single `itsPoll`) and
rides rnsd's Link request/response byte-array API.

## What it does

Nomad Network (markqvist/NomadNet, GPL-3.0) is **two layers fused**:

1. **Messaging** — it is an LXMF client. reticulous already has this
   ([`lxmf.md`](lxmf.md)). NomadNet adds no protocol here.
2. **A text web** — nodes host **pages** (Micron markup) and **files**
   on a `nomadnetwork.node` destination; clients **browse** them over
   Reticulum Links. **This is the nomad task.**

The `nomad` task is pure transport + state:
- subscribes to rnsd's announce fan-out filtered to `nomadnetwork.node`
  → an LRU **drift feed** of heard nodes (display name comes free in the
  announce `app_data`, zero fetches);
- **navigates**: `request_path` → Link → `link.request(path[, data])` →
  raw Micron bytes published back to the frontend;
- keeps a **page cache** so re-viewing costs zero air time;
- persists **bookmarks**.

It **never parses Micron** — bytes in, bytes out. Both viewing endpoints
(SPA, LCD) own their own renderer; 320×240 and a browser tab render
differently by design (we don't chase pixel-parity). The task has
**zero mR includes** — everything goes through rnsd's byte-array API,
exactly like lxmf.

## The contract: storage *is* the API

Frontends (browser, CLI, on-device UI) drive nomad entirely through the
config tree. nomad subscribes to the cmd sentinels and publishes state;
it never exposes a custom DataChannel or RPC.

### Key namespaces

| Key | Lifetime | Shape | Meaning |
|---|---|---|---|
| `nomad.nodes.<dest_hex>` | ephemeral | `<last_s>\|<hops>\|<name>` | announce-drift feed (one leaf per heard node) |
| `s.nomad.bookmarks.<dest_hex>` | persistent, synced | `<name>\|<note>` | a saved node |
| `nomad.nav.status` | ephemeral | enum (below) | live navigation status |
| `nomad.nav.{hash,path,error}` | ephemeral | string | current target + last error |
| `nomad.page.{hash,path,size,fetched_s}` | ephemeral | — | last-fetched page metadata |
| `nomad.page.body` | ephemeral | UTF-8 Micron | last page bytes (capped — see below) |
| `nomad.page.truncated` | ephemeral | `0`/`1` | body too large to sync; read from device/cache |
| `nomad.cmd.*` | self-clearing | — | command sentinels (below) |
| `nomad.submit.<field_*\|var_*>` | self-clearing | string | staged form values, consumed by `nomad.cmd.submit` |
| `s.nomad.*` | persistent, synced | — | configuration knobs (below) |

`dest_hex` is the 16-byte destination hash as 32 hex chars. The drift
feed and page metadata are **ephemeral** (RAM-only, lost on reboot);
bookmarks and config are **persisted + browser-synced** (`s.` prefix).

## Commands — self-clearing keys

Write the key; nomad acts on the change subscription and `storageUnset`s
it immediately (same pattern as lxmf). Values are `|`-delimited where a
field may itself contain spaces.

| Key | Value | Action |
|---|---|---|
| `nomad.cmd.go` | `<hash>[:<path>]` | navigate; empty/missing path → `/page/index.mu` |
| `nomad.cmd.reload` | `1` | re-fetch the current page, **bypassing the cache** |
| `nomad.cmd.submit` | `<hash>[:<path>]` | submit the staged `nomad.submit.*` fields as a form |
| `nomad.cmd.bookmark.add` | `<hash>\|<name>\|<note>` | add/update a bookmark (note may contain `\|`) |
| `nomad.cmd.bookmark.del` | `<hash>` | remove a bookmark |

History/back is **frontend-owned** — the firmware keeps no nav history.

## Navigation lifecycle (`nomad.nav.status`)

A fetch walks these states; the frontend renders progress from them.
nomad mirrors rnsd's per-Link progress (`rnsd.links.nomad.state`) into
the intermediate states and owns the terminal ones:

```
idle → path_requested → establishing → requesting → done
                                       requesting → failed
```

- `path_requested` — `request_path` issued, awaiting `has_path`.
- `establishing` — Link handshake in flight.
- `requesting` — Link ACTIVE, request issued (also the state on a
  reused link, which skips straight here).
- `done` — response received; `nomad.page.*` updated. **Cache hits go
  straight to `done`** with zero air time.
- `failed` — `nomad.nav.error` carries the reason (`bad hash`,
  `link open failed`, `request failed`, …).

## Page delivery — body + cache

On a successful fetch nomad publishes metadata (`nomad.page.hash/path/
size/fetched_s`) and the bytes:

- **`nomad.page.body`** carries the full Micron text **if** it fits
  `s.nomad.max_page_publish` (default 32 KB). It rides the normal
  storage DC sync, so the browser receives the complete value (the 128 B
  change-notification cap applies only to the *in-device* notification,
  not the synced value). Larger pages set `nomad.page.truncated=1` and an
  empty body.
- The **full bytes always live in the task's RAM/PSRAM cache** regardless
  of the publish cap. The on-device LCD renderer reads the cache directly,
  so it renders even oversized pages; the SPA falls back on `truncated`.
- The cache is LRU, capped at 16 entries / 512 KB PSRAM (oldest-fetch
  eviction). Cache key is `<hash>:<path>`. `nomad.cmd.reload` bypasses it;
  form-submit responses are never cached.

Each fetch logs a one-line preview to the serial log — a short hex head
plus a sanitized text fragment (CR/LF/controls folded to `.`) and an
ellipsis. The full page is never dumped (keeps raw newlines/CRs out of
the log).

## Per-node Link reuse

Like a desktop NomadNet browser, nomad keeps **one Link to the current
node open** and reuses it for every request to that node. It is dropped
only on a node change, a failure, or when Reticulum closes it
idle/STALE (surfaced via the link-disconnect callback). So same-node
navigation has **no re-establish cost and no per-fetch ITS-conn churn**.

One request is in flight at a time (v1). The teardown order is
deliberate — nomad drops *its* ITS conn first, then tears down the rnsd
Link slot — so only one side ever disconnects the conn and no stale
DISCONNECT can hit a reused handle (this was an early double-free, now
fixed; see [`plans/nomad.md`](plans/nomad.md)).

## Announce-drift feed

nomad connects to `RNSD_PORT_ANNOUNCES` with an aspect filter of
`nomadnetwork.node`, so rnsd delivers only NomadNet node announces. Each
becomes a `nomad.nodes.<hex>` leaf (`<last_s>|<hops>|<name>`). The feed
is an LRU capped at `s.nomad.max_nodes` (default 256; `0` disables the
cap) — oldest-heard node evicted when full. The display name is the
announce `app_data` verbatim (UTF-8, may be empty); it is sanitized only
when *logged*, never when stored.

Frontends present the feed grouped as **Contacts** (your bookmarks) and
**On the Mesh** (everything else heard) — the same two-group model the
LXMF UI uses (see [`lxmf.md`](lxmf.md)). Bookmarks are nomad's
"contacts" equivalent.

## Bookmarks

`s.nomad.bookmarks.<hex>` = `<name>|<note>`, persisted and browser-synced.
Managed via the `nomad.cmd.bookmark.add` / `.del` sentinels or the CLI.
This is the only durable nomad state besides config.

## Forms — msgpack field/var map

A NomadNet page can carry input fields and links that submit them. The
flow (Phase 4):

1. The frontend stages each value under `nomad.submit.<key>`, where
   `<key>` is already the NomadNet map key (`field_<name>` or
   `var_<name>`).
2. It writes `nomad.cmd.submit = "<hash>:<path>"`.
3. nomad collects the staged `nomad.submit.*` leaves, packs them as a
   **msgpack string→string map**, deletes the staging tree, and issues
   `rnsdLinkRequest(..., data_packed=true)` so µR splices the map as the
   request envelope's 3rd element verbatim (vs. the bin-wrapped GET path).

Submit responses bypass the cache. The round-trip is proven Python↔Python
(`tests/test_nomad_forms.py`); a desktop-`nomadnet` `field`/`var`
interop pass is still wanted.

## Compression — bz2 (transparent)

Reticulum/NomadNet compress Resource payloads with Python `bz2`, and
**every client speaks it**, so a page served as a Resource usually
arrives bz2-compressed. This is handled entirely in the µR Resource
layer ([`Resource.cpp`](../components/microreticulum/src/Resource.cpp),
vendored bzip2 1.0.8 in [`components/bzip2/`](../components/bzip2/)) —
**transparent to the nomad task**, which only ever sees decompressed
bytes. The subtlety that bit us: Reticulum hashes/proofs the Resource
over the **uncompressed** data, so inbound decompresses *before* hashing
and outbound hashes the plaintext while shipping the compressed bytes
on-wire. nomad does both compression directions (outbound matters once
the server half exists).

## CLI — `nomad`

```
nomad nodes                          heard nodes (announce drift): hash, hops, age, name
nomad go <hash>[:<path>]             fetch a page (default /page/index.mu)
nomad reload                         re-fetch the current page, bypassing the cache
nomad bookmarks                      list bookmarks
nomad bookmark add <hash> <name>[ <note>]
nomad bookmark del <hash>
```

Page bytes are logged on fetch (one-line preview); nav state lives in
`nomad.nav.*`. The CLI verbs just write the corresponding `nomad.cmd.*`
sentinel, so they behave identically to the SPA/LCD paths.

A lower-level request smoke verb lives on rnsd:
`rnsd creq <dest_hash> <path>` issues a raw request/response over a Link
and logs the response — the Phase 0 de-risking tool for the µR
request/response path, independent of the nomad task.

## Configuration knobs (`s.nomad.*`)

Self-registering defaults, gated on `s.nomad.version` (bumped when keys
are added):

| Key | Default | Meaning |
|---|---|---|
| `s.nomad.max_nodes` | `256` | announce-drift LRU cap (`0` = unbounded) |
| `s.nomad.max_page_publish` | `32768` | max page bytes synced to the SPA via `nomad.page.body`; larger → `truncated` |
| `s.nomad.bookmarks.<hex>` | — | per-bookmark `<name>\|<note>` (written via cmd) |

## Frontends

Two renderers, each native to its UI; both consume the same wire Micron.

- **SPA** — [`web-interface/src/modules/nomad.ts`](../web-interface/src/modules/nomad.ts)
  (`useNomad()` composable + state/RPC over the storage tree), the
  **"Nomad Browser"** floating window
  ([`panels/NomadWindow.vue`](../web-interface/src/panels/NomadWindow.vue),
  two-section Contacts/On-the-Mesh list + page view + font +/− controls),
  and a **TS Micron→HTML/Vue renderer**
  ([`lib/micron.ts`](../web-interface/src/lib/micron.ts)) — wide layout,
  mouse-clickable links/forms, real text selection. The renderer is
  **reticulous-local**, not in `spangap-browser`: Micron is a NomadNet
  wire format, and the shared platform UI (used by seccam) never renders
  it, so a parser there would be a layering violation.
- **Device** (`CONFIG_SPANGAP_LCD`) — the **"Nomad"** launcher program
  + a **C++ Micron→LVGL renderer**
  ([`main/nomad_lcd.cpp`](../main/nomad_lcd.cpp)): hard-wrap to width,
  collapse multi-column to single, trackball/keyboard to step links and
  fill fields, "On the Mesh" / bookmark grouping. v1 drops inline
  colour/bold emphasis (headings + links are colour-emphasised); forms
  render as placeholders. Colour maps fine (ST7789 is RGB565) — layout
  and interaction are where it diverges from the SPA, by design.

## How it rides rnsd

nomad holds **zero mR types**. It is both an rnsd ITS *client* (announce
fan-out + the per-node Link) and an aux-only ITS *server* (one port for
request responses). The Link request/response surface it uses
([`rnsd.h`](../main/rnsd.h)):

- `rnsdLinkOpen(dest_hash, "nomadnetwork.node", identity_key, tag, …,
  onRecv, onDisc)` — opens (or, on path miss, requests path then opens)
  an outbound Link, tagged `"nomad"`.
- `rnsdLinkRequest(tag, path, data, len, resp_port, data_packed=false)`
  — issues `link.request`. Held until the Link is ACTIVE if needed (one
  pending per link), so open + request can be issued back-to-back. GET =
  `nullptr/0`; form = a caller-built msgpack map with `data_packed=true`.
- Response arrives as **one aux frame** (`rnsd_link_resource_done_t`,
  opcode `RNSD_LINK_REQUEST_RESPONSE`, `opaque_id` = the request id) on
  nomad's response port; nomad owns `buf` and `rnsdResourceRelease()`s
  it. Failure/timeout → `RNSD_LINK_REQUEST_FAILED`.
- `rnsdLinkTeardown(tag)` — closes the Link slot.

Whether the response came inline in a packet or as a (possibly
bz2-compressed) Resource is rnsd/µR's concern; nomad always receives
plain decompressed bytes.

## Wire protocol reference (markqvist/NomadNet)

Authoritative wire contract, cited into the reference clone at
[`../research/NomadNet/`](../research/NomadNet/). **GPL-3.0 — read for
the contract, reimplement in C++, never copy.**

### Destination & announces
- One destination: `Destination(identity, IN, SINGLE, "nomadnetwork",
  "node")` — app `nomadnetwork`, aspect `node`
  ([`Node.py:18`](../research/NomadNet/nomadnet/Node.py#L18)).
- Announce `app_data` = the node's **display name**, UTF-8 bytes
  ([`Node.py:217-221`](../research/NomadNet/nomadnet/Node.py#L217-L221)).

### Pages & files (server registers request handlers)
- Pages: `register_request_handler("/page/<rel>.mu", …,
  allow=ALLOW_ALL)`; default `/page/index.mu`
  ([`Node.py:54-71`](../research/NomadNet/nomadnet/Node.py#L54-L71)).
- Files: `/file/<rel>`, `auto_compress=32MB`; the generator returns
  `[filehandle, {"name": …}]` → RNS ships it as a **Resource**
  ([`Node.py:73-85`](../research/NomadNet/nomadnet/Node.py#L73-L85)).
- Dynamic pages (an executable file) receive `field_*`/`var_*` from the
  request `data` as env vars; stdout is the Micron response
  ([`Node.py:108-190`](../research/NomadNet/nomadnet/Node.py#L108-L190)).
  **On firmware there is no fork+exec** — see *Server, later*.

### Request / response (the Link layer)
Client flow
([`Browser.py:874-1048`](../research/NomadNet/nomadnet/ui/textui/Browser.py#L874-L1048)):
`request_path` → wait `has_path` → `Identity.recall` → build `OUT
SINGLE` dest → `Link` → wait ACTIVE → `link.request(path, data=…,
response_callback, failed_callback, progress_callback)`.

On the wire:
- Request = msgpack `[time, path_hash, data]`; `path_hash =
  truncated_hash(path)` (16 B). ≤ MDU → one packet; larger → a Resource.
- `data` is **nil** for a plain GET, or a **msgpack map** of
  `field_<name>`/`var_<name>` for forms.
- Response = msgpack `[request_id, response_bytes]`; ≤ MDU → packet,
  larger → Resource.

### URL grammar (link targets in Micron)
From [`Browser.py:204-314`](../research/NomadNet/nomadnet/ui/textui/Browser.py#L204-L314):
- `<dest_hash_hex>:<path>` — hash 32 hex chars; empty path →
  `/page/index.mu`; empty hash → current node.
- Trailing `` `var1=v1|var2=v2 `` → `var_<name>` request entries.
- Field references → `field_<name>` (form widgets); `*` = all fields.
- `@`-prefixes select destination type: `nnn@` (default), `lxmf@`
  (compose), `rrc@` (chat room).
- `p:<id>…` = in-page **partial**; `#anchor` = in-doc jump;
  `rrc://…` = chat. **Out of scope for v1** — the SPA URL resolver
  ignores `@`-scheme and `://` targets so such links don't misfire.

### Micron markup (renderer contract)
Backtick `` ` `` is the control char
([`MicronParser.py:588-700`](../research/NomadNet/nomadnet/ui/textui/MicronParser.py#L588)):
`` `_ `` underline · `` `! `` bold · `` `* `` italic · `` `F<rgb> ``/`` `f ``
fg colour (3 hex) · `` `B<rgb> ``/`` `b `` bg · `` `` `` reset · `` `c``/```l``/```r``/```a ``
align · `` `[label`target] `` link · `` <…|name`value> `` input field
(`!`=masked, width int, `?`=checkbox, `^`=radio). Line-level: `>`/`>>`/`>>>`
headings, dividers `` `= ``, literal blocks (a line of backticks toggles),
`#` comments, `` `t `` tables. **MicronParser.py is the grammar of record**
for both `micron.ts` and `nomad_lcd.cpp`.

## Server (later) — dynamic pages via Lua

Hosting pages is not built. The shape when it lands: static pages = a
`register_request_handler` returning a stored Micron blob; dynamic =
the handler runs an embedded **Lua** interpreter (~200–250 KB flash,
single-digit-KB `lua_State`, heap in PSRAM — trivial here; official
Espressif ESP-IDF Lua component de-risks integration). We can't
fork+exec like upstream, so a sandboxed script reads request fields and
returns a Micron string. The cost is the **sandbox**: strip
`io`/`os`/`loadfile`/`require`, expose a curated API (read request
fields, read storage/sensors, emit Micron), bound it with Lua's
instruction-count debug hook (no infinite loops) and a heap ceiling (no
OOM). File serving needs the `[fh, {name}]`/Resource path added to µR's
`handle_request`.

## Known limitations

- **One in-flight request per Link**; one open Link (to the current
  node) at a time. Fine for interactive browsing.
- **Forms** are HW-untested against a real desktop `nomadnet` — the
  `field`/`var` map is Python↔Python-proven only. Note the data-less GET
  packs an empty *bin* (`0xc4 0x00`) not *nil*; static handlers ignore
  request data so it interops, but if a node rejects it, add an
  empty-→-nil case in `Link::request`.
- **No file download** in the client yet (the `/file/…` Resource path);
  Phase 5.
- **No partials (`p:`) / `rrc://` chat** — v1 ignores them.
- **Identity model:** nomad browses with the rnsd node identity passed
  to `rnsdLinkOpen` (`identity_key` is currently empty → anonymous);
  per-page `.allowed` ACLs and `field_`/`var_` provenance that depend on
  a presented identity are not exercised yet.
