# Plan — Nomad Network pages (browser first, server later)

> Detailed implementation plan. Architecture + wire contract live in
> [`../nomad.md`](../nomad.md); this is the *how, in what order, which
> files, what changes*. Read [`link.md`](link.md) (the Link rollout this
> builds on) and [`../lxmf.md`](../lxmf.md) (the task model we copy) too.

> **Status — Phases 0–4 implemented (2026-05-25); HW-verify pending.**
> The whole **browser/client** is built: the request/response plumbing
> (Section 1 + Phase 0), the **firmware `nomad` task** (Phase 1), the
> **SPA browser + TS Micron renderer** (Phase 2), the **LCD browser +
> C++ Micron→LVGL renderer** (Phase 3), and **forms** (Phase 4 — additive
> msgpack-map request data, field widgets + submit in both renderers).
> Automated tests green Python↔Python (`test_nomad_page.py` GET +
> `test_nomad_forms.py` map submit); the device path is **not** flashed/
> verified yet. **Remaining:** Phase 5 (file download) rides the
> inbound-Resource aux but the `{name}` surfacing needs verifying; the
> Server phase is a separate later effort. Phase 4's `field`/`var` link
> grammar is reimplemented from the doc (no reference clone) and wants a
> desktop-`nomadnet` interop pass — isolated to forms.
>
> **Known caveat to watch in HW test:** a data-less GET packs the request
> envelope's data element as an empty msgpack *bin* (0xc4 0x00), not *nil*
> — the MsgPack shim's `pack_one(Bytes)` has no empty-→-nil case. Static
> page handlers ignore request data, so it should interop; if a desktop
> node rejects it, pack nil for empty data in `Link::request` (one-liner).
> See per-phase status notes below.

## 0. Status reconciliation — "links work, echo uses them"

True, and it's the right instinct — but it's a **different mechanism**
than page-fetch needs, so the de-risk step stays. Precisely:

| Mechanism | Status | Who uses it |
|---|---|---|
| Outbound Link establish + **raw packet** send/recv (`link.send`/`set_packet_callback`) | **Done, HW-verified** (Phase C/D) | echo_peer, `rnsd clink`. This is what you see in the logs. |
| Resource transfer over a Link | **Done, HW-verified** (Phase F) | lxmf large messages |
| **`link.request(path, data)` / `register_request_handler`** (request/response) | **library code present in µR, never driven end-to-end** | nobody yet |

Evidence:
- The echo peer does `link.set_packet_callback(on_packet)` →
  `RNS.Packet(link, message).send()`
  ([`tests/peers/echo_peer.py:79-86`](../../tests/peers/echo_peer.py#L79-L86)).
  Raw packets, not requests.
- rnsd's consumer API
  ([`rnsd.h`](../../main/rnsd.h)) exposes `rnsdLinkOpen` (explicitly
  "packet-mode: each itsSend is one Link packet"), `rnsdLinkSendResource`,
  `rnsdLinkTeardown`, `rnsdDestListenLinks`. **There is no
  `rnsdLinkRequest`.** The request/response layer is simply not plumbed
  to consumers.
- rnsd itself defers it: the management dest does
  `accepts_links(false)` with "refuse Link requests until **Phase G**
  ports `register_request_handler` and we know how to service them"
  ([`rnsd.cpp:1290-1293`](../../main/rnsd.cpp#L1290-L1293)).

So: **page-fetch is the `link.request` client path**, which rides on the
already-proven Link but is itself unproven. The plan's first job is to
plumb it and prove it (cheap), *then* build on it.

The good news from [`../nomad.md`](../nomad.md): µR's `Link.cpp` already
has both `Link::request`/`handle_response` (client) and
`handle_request`/response-generator (server) implemented, so this is
*wiring and verifying*, not porting from scratch.

## 1. The core plumbing gap (fix this first)

Per house rule "fix the plumbing/API first, not the call sites": add a
**request/response primitive to rnsd's byte-array consumer API** before
touching any frontend. Downstream tasks never see mR types.

> **Implemented (2026-05-25).** Final shapes (refined from the sketch
> below — the aux carries the link `tag` and the consumer's response port,
> which the original payload sketch omitted):
> - `rnsd.h`: `int rnsdLinkRequest(const char* tag, const char* path,
>   const void* data, size_t data_len, uint16_t resp_port)` → returns a
>   correlation id.
> - `ports.h`: aux op `RNSD_LINK_AUX_REQUEST` with `rnsd_link_request_t`
>   (`op | tag[24] | req_id(2) | resp_port(2) | path_len(2) | data_len(2)`)
>   followed inline by `path` then `data`. Response/failed reuse the
>   Resource handoff struct `rnsd_link_resource_done_t` with new opcodes
>   `RNSD_LINK_REQUEST_RESPONSE` (buf = response bytes, consumer owns) /
>   `RNSD_LINK_REQUEST_FAILED`; `opaque_id` carries the req_id.
> - `rnsd.cpp`: one in-flight request per link slot (`req_mrid` resolves
>   the slot in µR's userdata-less response/failed thunks, like `res_hash`
>   does for Resources); a pre-active deferral (issued on establish, like
>   the packet/Resource outboxes); and a `linkTick` timeout backstop
>   (`s.rnsd.link.request_timeout_s`, default 15) because µR doesn't drive
>   `RequestReceipt` timeouts. Large (>MDU) responses ride a Resource and
>   route through `is_response()`→pending-request, *not* the Phase-F
>   `ACCEPT_APP` gate — verified in `Link.cpp` `RESOURCE_ADV`.

**`rnsd.h` / `rnsd.cpp`:**
- `int rnsdLinkRequest(const char* tag, const char* path, const void* data, size_t data_len)`
  — issue `link.request(path, data, …)` on the Link already opened as
  `tag` (via `rnsdLinkOpen`). `data == nullptr` → plain GET (request
  envelope's 3rd element is nil). Returns a request id (or queues an aux
  and returns a handle).
- Response delivery: reuse the existing Resource-inbound aux shape —
  responses can be large (a page, or a whole file), so they come back
  as a heap buffer the consumer owns, tagged by request id.

**`ports.h`:**
- New `RNSD_PORT_LINK` aux op: `RNSD_LINK_AUX_REQUEST` (payload:
  `req_id(2) | path_len(1) | path | data`).
- New consumer-side response aux ops, mirroring `RNSD_LINK_RESOURCE_*`:
  `RNSD_LINK_REQUEST_RESPONSE` (`req_id(2)` + heap buf, consumer owns)
  and `RNSD_LINK_REQUEST_FAILED` (`req_id(2)`).

**`rnsd.cpp` internals:**
- On `RNSD_LINK_AUX_REQUEST`: look up the Link by tag, call
  `link.request(Bytes(path), Bytes(data), response_cb, failed_cb, progress_cb)`.
- `response_cb` → copy `RequestReceipt` response bytes into a heap buf,
  emit `RNSD_LINK_REQUEST_RESPONSE` to the consumer's aux port.
  `failed_cb`/timeout → `RNSD_LINK_REQUEST_FAILED`.
- `progress_cb` → optional, reuse `RNSD_DEST_AUX_RESOURCE_PROGRESS`
  shape for a progress bar.

µR detail: `Link::request` already packs `[time, path_hash, data]` and
chooses packet-vs-Resource by size; `handle_response` already
reassembles and fires `response_received(bytes)`
([`Link.cpp:469`, `922`](../../components/microreticulum/src/Link.cpp#L922)).
We just bridge its callback to ITS.

## Phase 0 — verify (smallest possible, before any task)

Goal: prove µR's `link.request` round-trips end-to-end. No nomad task
yet.

1. Land the `rnsdLinkRequest` plumbing above.
2. Add a smoke verb: `rnsd creq <dest_hash> <path>` (sibling to `rnsd
   clink`) → `rnsdLinkOpen` then `rnsdLinkRequest`, log the response
   bytes hex+ascii.
3. **Test peer:** `tests/peers/nomad_peer.py` — a real-shape Nomad node
   (`RNS.Destination(IN, SINGLE, "nomadnetwork", "node")` +
   `register_request_handler("/page/index.mu", …)` returning a tiny
   Micron blob), mirroring `echo_peer.py`. Also point it at a desktop
   `nomadnet` node for true interop.
4. `tests/test_nomad_page.py` — device fetches `/page/index.mu`,
   asserts the Micron bytes come back.

**Exit:** raw Micron of `index.mu` printed by `rnsd creq`. If this
works, the rest is UI.

> **Done so far:** steps 1–4 landed. `rnsd creq <hash> <path> [aspect]`
> (sibling to `rnsd clink`, in the same smoke task, default aspect
> `nomadnetwork.node`) opens a Link and issues the request; the response
> aux is logged hex + Micron text on `CREQ_RESP_PORT`. `tests/peers/
> nomad_peer.py` serves a tiny `/page/index.mu`; `tests/test_nomad_page.py`
> drives `link.request` against it and asserts the bytes — **green**
> (`pytest tests/test_nomad_page.py`). **Remaining for Exit:** flash +
> `rnsd creq` on hardware against `nomad_peer.py` and a desktop
> `nomadnet`.

## Phase 1 — firmware `nomad` task (`main/nomad.cpp` / `nomad.h`)

> **Implemented (2026-05-25), HW-verify pending.** [`main/nomad.cpp`](../../main/nomad.cpp)
> + [`main/nomad.h`](../../main/nomad.h), wired into `app_main`
> (`nomadInit()`) and `main/CMakeLists.txt`. Built on the Phase 0
> request/response plumbing. Done: announce-drift feed (`nomad.nodes.*`
> LRU, `s.nomad.max_nodes`), bookmarks (`s.nomad.bookmarks.<hex>` =
> `<name>|<note>`), navigate (`nomad.cmd.go` = `<hash>[:<path>]` →
> `rnsdLinkOpen` + `rnsdLinkRequest` → response published to
> `nomad.page.*` + logged), page cache (RAM/PSRAM, LRU by count+bytes,
> `nomad.cmd.reload` bypasses), nav status (`nomad.nav.*`, with rnsd's
> per-fetch Link state reflected in for `path_requested`/`establishing`/
> `requesting`), and a `nomad` CLI (`nodes`/`go`/`reload`/`bookmark[s]`).
> **Page bytes** are held in the task's RAM cache and **logged** on fetch
> (the Phase 1 verification surface, like `rnsd creq`); the SPA byte
> transport is Phase 2. **HW-verify:** `nomad go <hash>` against
> `nomad_peer.py` / a desktop nomadnet should log the Micron and set
> `nomad.nav.status=done`; announces from a running node should populate
> `nomad.nodes.*`.
>
> rnsd robustness fix folded in: `linkFreeSlot` now emits
> `RNSD_LINK_REQUEST_FAILED` for an outstanding/deferred request when a
> Link is reclaimed (establish failure, no-path, remote close), so the
> consumer never hangs waiting for a response that can't come.

Pure transport + state, copying the lxmf task model (storage-as-API,
cmd sentinels, single `itsPoll` wait point, **zero mR includes** — goes
through rnsd's byte-array API).

**Announce-drift feed:** subscribe to `RNSD_PORT_ANNOUNCES`, filter to
the `nomadnetwork.node` aspect, keep an LRU of `{dest_hash, name (from
app_data), last_seen}`. Publish as `nomad.nodes.*` ephemeral state.

**Bookmarks:** `s.nomad.bookmarks` (persistent) — `{dest_hash, name,
note}`. Add/remove via cmd.

**Navigate state machine** (driven by `nomad.cmd.go` = `hash:path`):
`request_path` (if no path) → `rnsdLinkOpen` → `rnsdLinkRequest(path)`
→ response bytes → publish to the requesting frontend. `/file/…` →
expect a Resource (Phase 5). Statuses mirror Browser.py
(NO_PATH/PATH_REQUESTED/ESTABLISHING/REQUESTING/DONE/FAILED/TIMEOUT)
→ `nomad.nav.status`.

**Page cache:** keyed by `hash:path` → bytes (+ fetched-at). Re-view =
zero air time. Cap by count/bytes (PSRAM). `nomad.cmd.reload` bypasses.

**Storage schema:**
- `s.nomad.bookmarks` — persistent list.
- `s.nomad.announce_node` / `s.nomad.node_name` — only relevant to the
  *server* phase; omit for now.
- `nomad.nodes.*`, `nomad.nav.*`, `nomad.page.*` — ephemeral.
- cmd sentinels: `nomad.cmd.go`, `.reload`, `.bookmark.add/.del`,
  `.back` (history is frontend-side; firmware is stateless re history).

**ITS ports:** one consumer aux port for request responses + resource
inbound (reuse `rnsd_link_resource_done_t` shape).

**Task placement:** core 1, low prio, PSRAM stack — same class as lxmf.

**Never parse Micron.** Bytes in, bytes out.

## Phase 2 — SPA browser (`web-interface/src/`)

> **Implemented (2026-05-25).** [`modules/nomad.ts`](../../web-interface/src/modules/nomad.ts)
> (composable `useNomad()` + `registerNomad()`, wired into
> [`boot/modules.ts`](../../web-interface/src/boot/modules.ts)),
> [`panels/NomadPanel.vue`](../../web-interface/src/panels/NomadPanel.vue)
> (Settings → Reticulum → Nomad Network: announce cap + bookmark admin),
> [`panels/NomadWindow.vue`](../../web-interface/src/panels/NomadWindow.vue)
> (the browser floating window: address bar + back, bookmarks/nodes
> sidebar, page view; mounted in
> [`MainLayout.vue`](../../web-interface/src/layouts/MainLayout.vue), toggled
> from Status → Nomad Browser), and the **TS Micron→HTML renderer**
> [`lib/micron.ts`](../../web-interface/src/lib/micron.ts). State is read
> from the storage mirror (`nomad.nodes.*`, `s.nomad.bookmarks.*`,
> `nomad.nav.*`, `nomad.page.body`), writes are `nomad.cmd.*` sentinels —
> no new DataChannel. The renderer typechecks clean (`tsc --strict`);
> full SPA build is validated by the user's `idf.py build`. **Open
> decision resolved → renderer is reticulous-local, not `spangap-browser`:**
> Micron is a NomadNet wire format, while `spangap-browser` is the generic
> device-platform UI shared with seccam (a camera app that never renders
> Micron) — putting a NomadNet parser there would be a layering violation.
> Form fields are interactive as of Phase 4 (below).

- `modules/nomad.ts` — state + RPC (bookmarks, node list, navigate,
  current page bytes, status). Wires into `stores/index.ts`.
- `panels/NomadPanel.vue` — Settings → Nomad (bookmarks mgmt, announce
  interval later for server).
- `panels/NomadWindow.vue` — the **browser** floating window: two-section
  list (bookmarks / announce-drift) + page view + address bar + back.
- **TS Micron→HTML renderer.** Implement against
  [`MicronParser.py`](../../research/NomadNet/nomadnet/ui/textui/MicronParser.py)
  as the grammar of record (control char `` ` ``: styles, `F/B` colors,
  align, headings, dividers, links, fields, literals, tables). Links →
  navigate RPC; fields → request data (Phase 4). **Put the renderer in
  `spangap-browser`** if nothing reticulous-specific leaks in (it's a
  candidate shared piece, like the other shared UI).

Wide layout, mouse-clickable links/forms, real text selection — render
*for the browser*, not pixel-matched to the LCD.

## Phase 3 — LCD browser (`CONFIG_SPANGAP_LCD`)

> **Implemented (2026-05-25), HW-verify pending.**
> [`main/nomad_lcd.cpp`](../../main/nomad_lcd.cpp) — the on-device "Nomad"
> launcher program (registered via `nomadLcdRegister()` in `app_main`,
> CMakeLists), modelled on [`lxmf_lcd.cpp`](../../main/lxmf_lcd.cpp): two
> screens in one layer — a **list** (bookmarks on top, announce-drift nodes
> below) and a **page** view (back + node name + reload over a scrollable
> rendered Micron column). All on the lcd task; storage subscriptions drive
> redraws. The **C++ Micron→LVGL renderer** is independent of the TS one:
> single column, links are focusable/clickable labels added to
> `lcdInputGroup()` (trackball steps them) that write `nomad.cmd.go`.
> **v1 LCD simplifications** (documented divergence — the plan says layout +
> interaction diverge by design): inline fg/bg color and bold/italic are
> dropped (headings are colour-emphasised, links link-coloured + underlined,
> dividers/literal blocks/comments handled); input fields render as `[name]`
> placeholders (forms = Phase 4). Body comes from `nomad.page.body`; pages
> over `s.nomad.max_page_publish` show a "too large" notice. Plus a
> Settings → Reticulum → Nomad pane. **HW-verify:** open the Nomad program,
> tap a node, confirm the page renders and links step/navigate; LVGL
> flex/label layout on 320×240 is the tune-on-hardware part.

- Launcher program shaped like [`maps.cpp`](../../main/maps.cpp):
  worker owns nav state, lcd task composites.
- **Two-section list = your LXMF model:** bookmarks on top, announce-drift
  below; select → page view.
- **C++ Micron→LVGL renderer:** hard-wrap to 320 px, collapse
  multi-column to single, trackball/keyboard to step links + fill
  fields. Color maps directly (RGB565). Diverges from SPA in **layout +
  interaction**, by design.

Independent of the TS renderer — two parsers, each native to its UI.
The C++ parser is browser-only (server emits Micron, never parses).

## Phase 4 — forms (the Phase G / msgpack tie-in)

> **Implemented (2026-05-25); field/link grammar wants interop tuning.**
> Additive throughout — the GET path is byte-for-byte unchanged.
> - **µR** ([Link.h](../../components/microreticulum/src/Link.h)/[Link.cpp](../../components/microreticulum/src/Link.cpp)):
>   `Link::request(..., bool data_packed=false)`. When true, `data` is a
>   complete msgpack object spliced as the envelope's 3rd element verbatim
>   (vs bin-wrapped). The only caller of `request` is nomad, so this can't
>   touch the verified lxmf/Resource/opportunistic paths (they don't call it).
> - **rnsd**: `data_packed` flag on `rnsd_link_request_t` + `rnsdLinkRequest`,
>   threaded through `onLinkAux`/`linkStartRequest` (incl. the pre-active
>   deferral).
> - **nomad task**: local msgpack map writers (`mpMapHeader`/`mpStr`);
>   `onCmdSubmit` reads the frontend-staged `nomad.submit.<field_*|var_*>`
>   keys, packs `{field_*,var_*}` (str→str, matching Python umsgpack), and
>   issues `rnsdLinkRequest(..., data_packed=true)`. Submit responses are
>   published but **not cached** (forms aren't idempotent).
> - **SPA**: `.mfield` inputs are now editable; `NomadWindow.followTarget`
>   parses `url`fields`vars`, gathers the named field values (`*`=all) + var
>   literals, and calls `useNomad.submit` (stages `nomad.submit.*` + triggers).
> - **LCD**: fields render as LVGL textareas (trackball/keyboard); a form
>   link gathers them + vars and writes the same submit sentinels.
> - **Test**: `tests/test_nomad_forms.py` + `nomad_peer.py` `/page/form.mu`
>   prove the map-as-3rd-element contract round-trips through real RNS
>   (a dict → handler reads `field_user`/`var_csrf`) — **green**.
>
> **Interop caveat:** the `url`fields`vars`` link grammar + `field_`/`var_`
> key convention are reimplemented from the doc, not a reference clone
> (`research/NomadNet/` is absent). The structure is validated against our
> own peer; matching a desktop `nomadnet`'s exact link encoding is the
> remaining tune — isolated to forms (GET browsing unaffected).

NomadNet field data is a msgpack **map** (`field_<n>`/`var_<n>`), but
µR's `request(Bytes data)` packs `data` as a msgpack **bin** — so this
won't interop until we can emit a map as the request envelope's 3rd
element. Options:
- extend `Link::request` to accept a typed map (cleanest, shared with
  the server phase's response packing), or
- a small "pack these k/v pairs as a msgpack map" helper in rnsd that
  feeds `data` and a flag telling `request` to splice it raw.

Then: field widgets in both renderers, link-data collection per
[`Browser.py:214-267`](../../research/NomadNet/nomadnet/ui/textui/Browser.py#L214-L267)
(`field_`/`var_`, `*`=all, trailing `` `var=val|… ``).

## Phase 5 — file download (client)

`/file/…` responses arrive as a **Resource** — reuse the existing
inbound-Resource aux (`RNSD_LINK_RESOURCE_INBOUND_DONE`, consumer owns
buf). Save to SD / offer download in the SPA. Name metadata from the
node ([`Node.py:206`](../../research/NomadNet/nomadnet/Node.py#L206)) —
note µR's `handle_response` may not surface the `{name}` dict; verify,
fall back to the path basename.

## Server (separate effort, later)

Out of scope for the browser, summarized so the seams are right:
- Wire `register_request_handler` at the rnsd consumer level (this is
  the actual Phase G work the `accepts_links(false)` comment refers to)
  + flip `accepts_links(true)` on the nomad dest.
- Static pages = handler returns a stored Micron blob.
- Dynamic pages = **Lua** (sandboxed: strip `io`/`os`/`loadfile`,
  instruction-count debug hook, heap ceiling; curated API to read
  request fields + storage/sensors and emit Micron). ~200–250 KB flash,
  trivial here; official Espressif ESP-IDF Lua component de-risks it.
- File serving = add the `[fh, {name}]`/Resource path to µR's
  `handle_request` (not present today).

## Testing

- `tests/peers/nomad_peer.py` — real-shape node (pages + a dynamic
  page + a file), mirrors `echo_peer.py`; `conftest.py` fixture.
- `tests/test_nomad_page.py` — GET round-trip (Phase 0/1).
- `tests/test_nomad_forms.py` — field submission (Phase 4).
- Manual interop: a desktop `nomadnet` node on the LAN (AutoInterface)
  — the real conformance bar.

## Open decisions
- **Browse identity:** present an identity to nodes (enables `.allowed`
  ACLs + field provenance) or browse anonymously? Outbound Links recall
  *our* identity for the link; nomad page ACLs key on
  `remote_identity.hash`. Decide before Phase 4.
- **TS renderer home:** `spangap-browser` (shared) vs reticulous-local.
- **History/back:** frontend-owned (firmware stateless) — confirm.
- **Non-page link schemes** (`lxmf@` compose, `rrc://` chat, `p:`
  partials): parse-but-defer in v1 so links don't break; wire later.
