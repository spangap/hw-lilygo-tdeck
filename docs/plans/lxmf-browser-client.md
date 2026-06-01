# LXMF browser client — design proposal

A Signal-faithful messaging client for the `spangap-browser` SPA, built
entirely on top of the LXMF storage contract. No new DataChannel, no new
ITS port, no protocol work — every screen is a reactive view over the
config-tree mirror and every action is a storage write.

Status: **v1 implemented 2026-05-18** (browser form factor, all six
phases) — `web-interface/src/modules/lxmf.ts` (state layer) + the
`components/lxmf/*` view layer + `panels/{MessagesWindow,AnnouncesWindow,
LxmfPanel}.vue` composition roots, wired into `boot/modules.ts` and
`MainLayout.vue`. SPA builds and type-checks clean. Not yet
hardware-verified against a live upstream peer (the Phase 2–4
visibility check). Consumer of the LXMF subsystem as described in
[../lxmf.md](../lxmf.md) (black-box / storage API) — read that first;
this plan does not restate the schema, it maps it onto a UI.

The three user-facing nouns this client is built around:

- **Contacts** — *this identity's* address book
  (`s.lxmf.id.<n>.contacts.<peer>`).
- **Announces** — everyone heard on the mesh, cross-identity, ephemeral
  (`lxmf.announces.<hash>`, capped by `s.lxmf.max_announces`). Sideband
  calls this the "Announce Stream"; same idea.
- **Messages** — conversations (`s.lxmf.id.<n>.msgs.<peer>.<key>`,
  stored per contact — see
  [lxmf-messages-per-contact.md](lxmf-messages-per-contact.md)).

---

## 1. Goal, scope, non-goals

**Goal.** A messaging app that someone arriving from Sideband or
NomadNet recognises instantly as "Signal, but on the mesh": a
conversation list, per-peer threads of direction/status-stamped bubbles,
a composer, and a way to start a new conversation with anyone we've
heard. Honest about the network underneath it.

**In scope (v1).**

- One Signal-shaped Messages app (master/detail).
- Per-peer conversation threads, scoped to the active identity.
- Send / resend / cancel / delete; inbound display + read-marking.
- "New message" peer picker unifying Contacts + Announces.
- A standalone **Announces** monitor (mesh-activity view, Status menu).
- Single-identity assumption with a hidden-at-one selector.
- Identity create / import / destroy as a Settings panel (admin, not
  the chat window).
- Contact detail with the `trust` flag surfaced as a "Verified" badge,
  and the destination hash + ratchet shown as the safety-number analog.

**Not in scope (v1), and why.**

| Deferred | Reason |
|---|---|
| Groups | No native LXMF group concept in our impl. |
| Typing indicators | No mechanism on the wire. |
| Voice notes / rich attachments | Our lxmf is `title` + `content` only. |
| Read receipts (network) | We have no network read receipt; faking the blue tick would lie. The local `read` flag is UI-only. |
| Disappearing-messages timer | A *real* optional privacy feature, deferred. Not a housekeeping lever: with `/state` on SD history is effectively unbounded. |
| Presence ("online" dot) | The closest signal is last-heard-announce age — mesh reachability, not presence. Surfaced subtly, never as a green dot. |
| Storage meter / bulk-delete-first-class | `/state` on SD (separate effort) makes history effectively unbounded. The client assumes unbounded history and stays a pure storage consumer. |

---

## 2. Architecture: a layered, two-form-factor design

The same component design must later drive the device's own 320×240
screen, where master/detail splits into separate full-screen views with
a back stack. So the design is **three layers**, and only the top layer
differs between browser and on-device:

```
┌─────────────────────────────────────────────────────────────┐
│ Composition layer  (per form factor)                         │
│  • browser: one FloatingWindow, master/detail two-pane        │
│  • device : a screen stack — List ⇄ Thread ⇄ Compose ⇄ …      │
├─────────────────────────────────────────────────────────────┤
│ View layer  (shared, presentational, layout-agnostic)        │
│  ConversationList · ConversationThread · MessageBubble ·      │
│  Composer · PeerPicker · AnnouncesView · ContactCard          │
├─────────────────────────────────────────────────────────────┤
│ State layer  (shared, headless)  — modules/lxmf.ts            │
│  useLxmf(): reactive derived state + action verbs.            │
│  Pure functions over useDeviceStore().settings; no DOM.       │
└─────────────────────────────────────────────────────────────┘
```

Rules that keep it portable:

- **No component reads `useDeviceStore` directly.** Everything goes
  through `useLxmf()`. Swapping the persistence layer or unit-testing a
  view never touches a component.
- **No component knows whether it is in a pane or a screen.** Selection
  is state (`activePeer`, `activeIdentity`), not layout. The browser
  renders list+thread simultaneously bound to that state; the device
  renders one at a time and uses the same state for back-stack
  navigation. A view emits intent (`open-peer`, `compose`,
  `open-announces`); the composition layer decides what that does.
- **Views take props / emit events only.** No `FloatingWindow`,
  `q-drawer`, or router import below the composition layer.

This mirrors the existing split in the repo: `modules/*.ts` register
menu entries and hold cross-window state (see
[../../web-interface/src/modules/rnsd.ts](../../web-interface/src/modules/rnsd.ts)),
panels/windows are presentational and bind to `useDeviceStore`
(see [NodesWindow.vue](../../web-interface/src/panels/NodesWindow.vue)).
We extend that pattern with an explicit headless state layer because the
LXMF app is far larger than a status window.

---

## 3. State layer — `modules/lxmf.ts`

A composable wrapping `useDeviceStore()`. All reads are `computed` over
the reactive `settings` mirror, so multi-frontend coherence is free
(another tab, the CLI, the device UI all converge — see
[../lxmf.md](../lxmf.md) "Multi-frontend coherence is free").

### 3.1 Derived reactive state

```ts
identities        // [{ n, label, displayName, up, destHash }]   from s.lxmf.id.*
activeIdentity    // ref<number>, client-local (see §3.3), default lowest loaded slot
conversations     // Conversation[]  — msgs grouped by peer, newest-first
activeConversation// messages for activePeer, ascending, day-bucketed
contacts          // this identity's address book, by peer
announces         // parsed lxmf.announces.* → [{ hash, name, lastSeen, hops, cost }]
peerDirectory     // unified Contacts ∪ Announces for the picker
unreadTotal       // Σ messages with dir=in & read=0
displayName(peer) // memoized resolver: Contacts.display_name → announce name → hash[0:8]…
peerAvatar(peer)  // pure: deterministic identicon (hue from hash bytes, initials from name)
reachability(peer)// announce-catalogue entry → { lastSeenS, hops } | null (never a "presence" claim)
```

`displayName` / `peerAvatar` / `reachability` are **pure functions of a
snapshot**, recomputed once per render off the contacts+announce
revision, not per row (per §9's name-thrash risk). They live in the
state layer with no DOM, so the device port reuses them verbatim.
`peerAvatar` is the open identicon question made concrete: hue =
function of the first hash bytes, glyph = up-to-two initials of the
resolved name (hex prefix if unnamed) — legible at 24 px, identical on
320×240.

`conversations` is the heart of it: a `computed` over
`s.lxmf.id.<active>.msgs.*`. The store is **already keyed per peer**
(`msgs.<peer>.<key>` — see
[lxmf-messages-per-contact.md](lxmf-messages-per-contact.md)), so this
is a direct iteration of peer subtrees, not a group-by over a `peer`
field (the field is still present, redundantly). Threading is by
`peer`, never the `thread` field (§5). One row per peer with last
message, timestamp, unread count, and resolved display name (Contacts
name → Announce name → truncated hash).

### 3.2 Action verbs (the only writers)

| Verb | Storage effect |
|---|---|
| `send(peer, content, opts?)` | one `sendJson()` of a **single nested patch** carrying both `msgs.<peer>.<o_key>.{dir,peer,title,content,stage:draft}` and `lxmf.id.<n>.cmd.send=<peer>/<o_key>` — one DC message = one storage patch = atomic, the firmware-side equivalent of `storageBegin/End`. |
| `resend(peer, key)` | re-write `cmd.send=<peer>/<key>` (no auto-retry exists; per [../lxmf.md](../lxmf.md)). |
| `cancel(peer, key)` | `cmd.cancel=<peer>/<key>`. |
| `deleteMessage(peer, key)` | `cmd.delete=<peer>/<key>`. Whole-thread = `cmd.delete=<peer>` (bare peer). |
| `markConversationRead(peer)` | **one** `sendJson()` setting `read=1` on every inbound-unread key in `msgs.<peer>` — local UI only, firmware ignores it, no sentinel. |
| `announceNow()` | `lxmf.id.<n>.cmd.announce=1`. |
| `createIdentity` / `importIdentity` / `destroyIdentity` | `lxmf.cmd.identity_*` sentinels (Settings panel only). |

Two implementation rules that the API forces and the plan must pin
down, both verifiable in [../../web-interface/src/stores/device.ts](../../web-interface/src/stores/device.ts):

- **Atomicity is one `sendJson(nestedObj)`, never two `set()`s.** The
  device store sends one DC message per call; `set(path,val)` is one
  leaf, so a record + its `cmd` sentinel written as two `set()`s race
  (firmware can see the sentinel before the body). The state layer
  builds the combined nested object itself and calls `sendJson` once.
  Same rule for `markConversationRead`: one patch over all keys, not a
  `set()` per message.
- **Per-sentinel serialization, not one global queue.** `cmd.send`,
  `cmd.cancel`, `cmd.delete`, `cmd.announce` are four *distinct* keys
  per identity, each single-valued and firmware-cleared. The state
  layer holds **one small queue per `cmd` key per identity**: the next
  enqueued write for that key waits until the mirror shows the key
  `undefined` again (firmware deleted it = done), with a timeout
  fallback. Different cmd kinds do not block each other; rapid same-kind
  actions (delete-delete, send-send) serialize. This replaces §9's
  "a small action queue" — one global queue would needlessly stall a
  `delete` behind an in-flight `send`.

`send()` keys follow the documented convention `o_<unix_ms>_<rand4>`,
write `title=""` (we are content-only — §1 non-goals), and leave
`method` unset unless `opts.method` is given (firmware picks `auto`). No
client retry logic, no optimistic dedup — the firmware owns
`stage`/`message_id`/`attempts`/`last_error`.

---

### 3.3 `activeIdentity` is client-local, not the CLI's selection

`s.lxmf.cli.selected_id` is the **CLI's** selected identity. The browser
must **not** read or write it: the CLI and a browser tab legitimately
view different identities at the same time, and the multi-frontend
coherence guarantee is about *shared message state*, not about hijacking
each other's view cursor. `activeIdentity` is therefore a client-local
`ref` (optionally persisted to `localStorage`, never to the device),
defaulting to the lowest loaded slot. This keeps the single-identity
common case invisible (§7.1) without coupling two frontends' navigation.

## 4. View layer — components

All presentational, Quasar primitives + scoped dark styles consistent
with the existing panels. Each is independently usable on a 320×240
screen (no fixed two-pane assumptions, no hover-only affordances).

- **ConversationList** — Signal's left rail. Rows: identicon (derived
  from peer hash), resolved name, last-message preview, relative time,
  unread badge. Sort by last-message time. Emits `open-peer(hash)` and
  `compose`.
- **ConversationThread** — the message pane/screen for `activePeer`.
  Day separators, ascending order, sticky header with peer name +
  reachability hint + a "verify"/info affordance. Pull-to-top loads
  nothing extra (history is fully in the mirror).
- **MessageBubble** — direction-aware. Outbound right, inbound left.
  Footer carries the **status chip** (§6). `failed` shows `last_error`
  inline with a one-tap **Resend**; `sending`/`queued` show progress;
  `cancelled` is muted. Long-press / context → Delete, Copy.
- **Composer** — text only. **No persisted draft record.** Unsent text
  is component/state-layer memory only — a per-peer in-memory
  `Map<peer,string>` so switching threads and back preserves what you
  typed (Signal-faithful) while never writing a `stage=draft` record or
  touching storage (consistent with the platform's no-"save" rule). A
  record springs into existence only when Send is pressed, written
  *with* its `cmd.send` in the one atomic patch (§3.2). Consequence: the
  persisted `draft` stage never appears in this client's UI — every
  stored message is already at least `queued`, so there is no
  "draft bubble" state to render (resolves §5/§6). A faint hint flips to
  "will send DIRECT" when `content+~32 B` exceeds the ~311 B
  opportunistic budget (informational, never blocking). `outbox full` /
  `too large for opportunistic` surface as inline errors on the
  resulting `failed` bubble, not modals.
- **PeerPicker** — "New message". Search box over `peerDirectory`:
  Contacts on top (you've talked to them), then Announces
  (searchable, name-substring + hash). Selecting opens/creates the
  conversation. This is the *only* place Announces feeds messaging.
- **AnnouncesView** — the standalone mesh monitor. A live table
  (name, hash, hops, cost, last-heard age), same visual language as
  [NodesWindow.vue](../../web-interface/src/panels/NodesWindow.vue),
  fed by `lxmf.announces.*`. Row action → "Message" (hands off to a
  new conversation).
- **ContactCard** — per-peer detail. The `trust` flag renders as a
  Signal-style **Verified** badge; destination hash + ratchet shown
  as the safety-number analog (compare-to-verify, no QR in v1).

---

## 5. Threading model — group by peer, not by `thread`

`s.lxmf.id.<n>.msgs.<peer>.<key>.thread` is a root-message-id *reply
pointer*, not the conversation key. Real LXMF traffic
(Sideband/NomadNet) is effectively flat per peer. So:

- The conversation unit is **`peer`** — and now also the storage
  partition (`msgs.<peer>.*`), so this falls out of the layout for
  free rather than being a client-side group-by.
- `thread` is ignored for layout in v1. It is retained only as an
  optional future "in reply to" quote affordance.

This is a deliberate refinement of "per-contact threads": contact-keyed
grouping, yes; email-style trees, no.

---

## 6. Honest status — the three principled departures from Signal

Signal's shapes, but the receipt semantics must not lie about the mesh:

1. **Two ticks, not three.** `sent` = a single tick (handed to a
   transport). `delivered` = a double tick (only on a *proven*
   DIRECT/Resource transfer). There is **no blue "read" tick** — we
   have no network read receipt; the `read` flag is local-only.
2. **Single-tick-forever is normal, not stuck.** An opportunistic
   message never gets an ack and legitimately stays single-tick.
   Tooltip: *"sent — opportunistic messages aren't acknowledged."*
   Never styled as a warning.
3. Stage → chip mapping, straight from
   [../lxmf.md](../lxmf.md) ("Sending a message"):

   ```
   draft     → never rendered: this client writes the record only at
               Send, atomically with cmd.send, so no stored message is
               ever observed at draft (§4 Composer). Listed for
               completeness against the schema.
   queued    → clock
   sending   → spinner  (+ last_error as transient sub-text: "establishing link"…)
   sent      → ✓        (tooltip per #2)
   delivered → ✓✓        (only DIRECT/Resource)
   failed    → ! + last_error inline + [Resend]
   cancelled → muted ✓-with-slash
   ```

Getting #1 and #2 right is the single highest-value differentiator of
this client over a naive port — it stays trustworthy where a literal
Signal clone would cry wolf.

---

## 7. Composition layer

### 7.1 Browser (v1 target)

- `modules/lxmf.ts` additionally registers menu entries (pattern from
  [modules/rnsd.ts](../../web-interface/src/modules/rnsd.ts)):
  - **Status → Messages** → toggles the Messages FloatingWindow
    (`messagesVisible` ref, bound in
    [MainLayout.vue](../../web-interface/src/layouts/MainLayout.vue)
    next to Nodes/Map).
  - **Status → Announces** → toggles the AnnouncesView FloatingWindow.
  - **Settings → Reticulum → LXMF** → the identity/admin panel.
- `MessagesWindow.vue` is the composition root: a `FloatingWindow`
  containing a two-pane master/detail — `ConversationList` (rail) +
  `ConversationThread` + `Composer`, with `PeerPicker` as an in-window
  overlay for "New message". A header hosts the identity selector,
  **rendered only when `identities.length > 1`**.
- Boot wiring: uncomment / add `registerLxmf()` in
  [boot/modules.ts](../../web-interface/src/boot/modules.ts) (the
  Phase-4 placeholder is already there).

### 7.2 Device 320×240 (future, designed-for now)

- No `FloatingWindow`; a screen stack: List → (tap) → Thread → (tap)
  → Composer; New-message → PeerPicker screen; an Announces screen.
- Same `useLxmf()` state, same view components, different composition
  root + a minimal nav stack honoring `activePeer`/back. Because views
  emit intent rather than mutate layout, this root is the only new
  code the on-device port needs — no view rewrites.

The on-device UI is **not built in this plan**; §2's rules exist so it
costs one composition root later, not a rewrite.

---

## 8. Phasing

| Phase | Deliverable | Verifiable by |
|---|---|---|
| 1 | State layer `useLxmf()` + headless unit checks | conversations/contacts/announces derive correctly from a captured mirror |
| 2 | Read-only Messages window: List + Thread + status chips | inbound traffic from a real upstream peer renders correctly threaded with honest ticks |
| 3 | Composer + send/resend/cancel/delete | round-trip with Sideband/NomadNet; `sent` vs `delivered` observed correctly |
| 4 | PeerPicker + new conversation; Announces monitor window | start a conversation from an announce; monitor matches `rnstatus`/Nodes |
| 5 | Settings → LXMF identity panel; identity selector | create/import/destroy; selector hidden at one identity |
| 6 | ContactCard + trust/Verified + safety-number view | trust flag toggles badge; hash/ratchet compare |

Phases 2–4 are the visibility milestone (a usable messenger); 5–6 are
completeness.

---

## 9. Risks / open points

- **`cmd.*` single-sentinel races.** *Resolved in §3.2:* per-`cmd`-key
  queues that wait for the mirror to show the key cleared. The residual
  risk is the timeout-fallback path (firmware never clears the key — a
  wedged task); on timeout the queue surfaces an inline error rather
  than silently dropping the next action.
- **`sent`→`delivered` only on DIRECT/Resource.** The UI must never
  imply opportunistic `sent` is a failure (designed in §6, but easy to
  regress — covered by a Phase 3 check against a real peer).
- **Name resolution precedence** (Contacts → Announce → hash): *the
  thrash is the bug, not the precedence.* §3.1's memoized
  snapshot-pure `displayName` is the mitigation — recompute on the
  contacts+announce revision, never per row, so a churning announce
  catalogue cannot reorder or rename the rail mid-scroll.
- **Reachability surface, bounded.** `reachability(peer)` renders only
  as a faint secondary line in the *thread header* ("heard 4m ago · 2
  hops"), and silently nothing if the peer was never heard. Never in
  the conversation list, never a coloured/“online” dot — the closest
  honest signal is announce age, not presence (§1 non-goals). This is
  the "honest about the network" principle given an exact, minimal
  surface so it can't grow into a presence indicator by accident.
- **Multi-identity merged view** is explicitly *not* v1 (single active
  identity, client-local per §3.3); revisit only if a real
  multi-identity workflow appears.
