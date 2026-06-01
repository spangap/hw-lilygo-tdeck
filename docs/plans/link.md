# Reticulum Link support in rnsd — design proposal

Plan for bringing Reticulum's **Link** layer (and the **Resource** + **Channel**
layers that ride on it) online in our µR fork, plus the rnsd-side client API
that exposes it to consumers (lxmf primarily, but also rnstatus/-R,
rnpath/-R, and future apps).

Status: implementation-ready as of 2026-05-13. Supersedes the "Link /
Resource / propagation node support is out of scope for v1" note in
[component-plan.md §1](../component-plan.md#1). This plan is the gate
to enable that work in the rolling-rollout sense — we keep Link off
until everything below is checked.

> **Verification + decisions (2026-05-12 → 2026-05-13).** All file:line
> refs below were checked against the current tree; the design choices
> §15 originally left open are now resolved (see §15 for the locked
> decisions and the alternatives considered).
>
> Two consequential plan gaps surfaced during verification and have
> been folded into the phase text:
>
> 1. §4.3 — `Destination::_accept_link_requests` defaults to **true**
>    in mR; Phase A *must* add explicit `accepts_links(false)` to
>    every rnsd-managed IN destination, or the build immediately
>    starts silently accumulating unhandled inbound Link objects.
> 2. §10 / §12.6 / §9.0 — two µR features the plan assumed are
>    missing: `Destination::register_request_handler` (Phase G prereq)
>    and `ResourceAdvertisement` is a stub class (Phase F prereq).
>    Both are real ~150 LOC ports from upstream Reticulum; effort
>    estimates bumped in §11.
>
> Phase F's ITS surface is now a **shared-memory aux handoff** (§9): on
> the receiving side, rnsd `malloc`s the full resource buffer at
> advertisement time (size is in `adv.d`, known up front), mR fills it
> in place, rnsd then hands the buffer pointer to the consumer over one
> small ITS aux frame. Keeps Link ITS buffers at 4 KB. Supersedes the
> earlier "(a) second ITS connection vs (b) type-byte mux" deferral.
>
> Smaller verified facts inlined throughout: ITS_MAX_MSG_DATA is 320
> (not 96); mR's `Link::Callbacks` take no userdata (slot ↔ Link
> resolved by `&link` walk over `s_link_conns[]`); ITS back-connects
> use `itsConnectByTaskHandle` keyed by `itsRemoteTask(dest_handle)`;
> `Link::send` is `Packet(link, data).send()` not a member call.
> Browser sync for the `rnsd.links.<tag>.*` ephemeral tree is
> automatic (storage's empty-prefix subscribe at
> [storage.cpp:1488-1491](../../../spangap/spangap-core/src/storage.cpp#L1488-L1491)
> coalesces all non-`secrets.*` changes into the DC patch). No
> remaining structural unknowns; the plan is ready to implement
> phase-by-phase.

---

## Current status (2026-05-16) — read this first if continuing

Phase A **done** and re-verified. Phase B Link establishment **works**
(re-verified on a clean fresh-flash 2026-05-16: `active_links` 0→1 in
<2 s, clean teardown 1→0). The inbound-LXM storage seg-buffer fix is
**verified on hardware** — an opportunistic echo reply now persists
(`lxmf msgs` shows `1 in received`); pre-fix it was silently 0.
**Phase C (`RNSD_PORT_LINK` outbound consumer API) is implemented and
verified on hardware** (2026-05-16, 0 warnings): 3 clean `clink`
cycles — `rnsdLinkOpen()` → `state=active`, `tx_packets=1` (pre-active
one-packet outbox buffered the probe and flushed on establishment),
`active_links:1` → `rnsdLinkTeardown()` aux → slot + `rnsd.links.clink.*`
tree reclaimed, `active_links:0`, tag reused next cycle with no
dup-rejection, no leak.
**Phase D (`RNSD_DEST_LINK_LISTEN` inbound forwarding) is implemented
and verified on hardware** (2026-05-16, 0 warnings): host
`scripts/link-probe` → device's `clink`-hosted `reticulous.linktest`
dest → LR accepted (accepts_links flipped), `onIncomingLinkEstablished`
→ `itsConnectByTaskHandle` back-connect to the consumer succeeds →
`rnsd.links.in.<8hex>.*` published (direction=in, state=active,
mtu=8192, last_error empty), host link held **ACTIVE the full 20 s**,
remote teardown → slot + tree reclaimed, `active_links:0`, no leak.
**Phase E (`lxmf` DIRECT delivery) is implemented and verified on
hardware** (2026-05-16, 0 warnings): lxmf calls `rnsdDestListenLinks()`
on each `lxmf.delivery` mailbox + hosts `LXMF_LINK_INBOX_PORT`;
inbound DIRECT reply from `lxmf-echo --echo` (now DIRECT by default)
landed and persisted (`lxmf msgs` → `#1 in received <echo> echo`,
signature-verified through the existing `onInboundLxm` pipeline) — the
§1 primary goal (real-world LXMF peers can finally talk back). Outbound
DIRECT verified by forcing `s.lxmf.id.0.default_method=direct`: device
opened a Link to the echo's `lxmf.delivery`, echo received + processed
the LXM (logged its own DIRECT echo-back), msg resolved to
`stage=sent`. Method selection (`s.lxmf.id.<n>.msgs.<mid>.method` →
`s.lxmf.id.<n>.default_method` → `auto`) in `processReady`.
**Phase F (Resource) — INBOUND + OUTBOUND hardware-verified vs a real
upstream LXMF peer; one soak caveat (2026-05-16).** On branch
`phase-f-resource`: the upstream-faithful Resource transfer engine is
ported into the µR fork (`7538fdd`+`1af435b`), Link.cpp RESOURCE_*
contexts wired + 28 pre-existing fork warnings zeroed (`c77006b`),
rnsd shared-memory aux hand-off + lxmf port 101 (`33db4a9`). §9.5
hardware debugging found and fixed real bugs: **Link MTU clamp** (mR
negotiated 8192 but every fork transport carries only RNS-base 500 →
tcp dropped the big parts; `LINK_MTU_DISCOVERY=false` + clamp) **plus
windowed request-next** (`5b96102`), **multi-segment hashmap**
RESOURCE_HMU pull (`408c574`), device-as-sender hashmap windowing +
`RESOURCE_HMU` emit (`0a53731`), and **defer outbound Resource until
the link is ACTIVE** (`9ac0ea7` — pre-fix the resource was *dropped*
pre-ACTIVE so the engine was never reached). Every step `idf.py
--spangap build` EXIT=0, **0 warnings**. **§9.5 inbound: 4/5 PASS**
vs a real upstream Python LXMF DIRECT peer — 16 KB ✅, 128 KB ✅
(multi-segment), advertise-then-drop → FAILED + no leak ✅,
oversize-reject-at-advertisement ✅; **64 KB soak RESOLVED — clean
uninterrupted 12/12, zero reboots** (`.rns/soak-clean.sh`): the
Resource **engine is sound, no engine leak**. The soak instead
flushed out **two pre-existing platform unbounded-growth defects,
neither in the Resource engine**: (a) `onInboundLxm` persists full
bodies into the 256 KB `/state` cJSON tree with **no retention cap**
(`max_resource_size`≈`/state` size) → `/state` full; (b) rnsd's
path-table is an **unbounded `BasicHeapStore` pruned only every 24 h**
and `publishPathTable()` O(N)-decodes it every cycle → grew under
churn → **task-watchdog panic in `publishPathTable()`**. Net: a
`/state`-full + wdt-panic incident; recovered by reflash (identity
preserved). Both are **tracked follow-ups gated on the storage/log→SD
work** (large Resource bodies are blob-scale, should not sit in the
RAM-cJSON config tree) — *not* Phase F engine defects. **Device-outbound: HW
VERIFIED** — `lxmf send <echo> @rand50000` → 50111 B / **109 parts**
→ windowed `advertise` (74 hashes) → echo `RESOURCE_REQ(EXHAUSTED)`
→ `Resource::request HMU seg=1 parts[74..109)` → `outbound resource
delivered (proof ok)` → msg `stage=sent`; the upstream echo logged
`LXM from <device> method=Curve25519` with the exact body, then its
`--echo` reply round-tripped inbound (83 parts, HMU 74/83→83/83).
The §1 primary goal — real-world peers exchanging big LXMF messages
with the device, both directions — is met. Caveats (not Phase F defects):
LoRa disabled for the TCP run (pre-Phase-3 scaffold; its TX busy-wait
tripped a task-watchdog under the packet burst). **bz2 caveat now
resolved (2026-05-25):** Resource payloads are bz2 (de)compressed both
directions via a vendored `bzip2` component — the old "incompressible
only / compressed dropped" limitation is gone (added for NomadNet pages,
which every client compresses). Full record:
[phase-f-resource-port.md → §9.5 results](phase-f-resource-port.md).
Phases A–E remain hardware-verified and committed on `main`
(`e2bb0ae`).

> **Bug found + fixed during Phase E (corrects §8.1):** the plan's
> "lxmf_data = packet.destination.hash() + data — same reconstruct as
> opportunistic" is **wrong for DIRECT**. Upstream
> `LXMessage.__as_packet` sends the *full* `self.packed` (incl. the
> 16-byte dest) for DIRECT and only **strips** it for OPPORTUNISTIC
> (LXMessage.py:630-632); `LXMRouter.delivery_packet` does
> `lxmf_data = data` (no prepend) for LINK vs. `dest + data` for
> opportunistic (LXMRouter.py:1825-1833). So the DIRECT Link packet
> already carries the full wire — lxmf must **not** prepend on inbound
> and must **not** strip on outbound. First impl followed §8.1, doubled
> the dest, signature failed, nothing persisted; fixed by passing the
> Link payload through verbatim both ways. (No `OUT_RESULT` exists on
> `RNSD_PORT_LINK`, so outbound DIRECT settles `sent/failed` from the
> 1 Hz `resolveDirectSends()` watching `rnsd.links.<tag>.*`; true
> proof-on-delivery is a documented follow-up.) Test-rig: `lxmf-echo`
> gained `--opp` (default now DIRECT, matching real peers); a >~440 B
> CLI send can't be exercised via `spangap-cli` (device CLI input line
> cap) — oversize auto→direct is the same selector branch as the
> verified forced-direct path.

> **Bug found + fixed during Phase D:** `rnsdTaskMain` only called
> `itsServerInit()`. Phase D's inbound back-connect
> (`itsConnectByTaskHandle` to the consumer) needs rnsd to also be an
> ITS *client* — without `itsClientInit()` the connect failed and
> `onIncomingLinkEstablished` tore the Link down (peer saw
> `DESTINATION_CLOSED` ~0.5 s after establish). Fixed by adding
> `itsClientInit(RNSD_MAX_LINK_CONNS + 4)` to `rnsdTaskMain`. Also
> added: hosted-destination observability (`rnsd.mailbox.<idx>.{aspect,
> dest}` published in `onMailboxConnect`, cleared on disconnect) so the
> browser/CLI/tests can see what rnsd hosts without log access; and a
> 20 s periodic re-announce in the `clink` listen test path so a
> host-side peer's path stays fresh across test runs.

> **Added in Phase C (extends §6.2 teardown):** consumer-initiated
> *explicit* teardown is an out-of-band aux frame on `RNSD_PORT_LINK`
> (opcode `RNSD_LINK_AUX_TEARDOWN`, payload = tag), exposed as
> `rnsdLinkTeardown(tag)` in rnsd.h. Bare `itsDisconnect` still only
> *parks* the Link for `orphan_ttl_s` (§10a.1). The aux is what frees
> the Link + slot + tag now — the data path stays type-byte-free, so
> teardown had to be out-of-band rather than an in-band opcode. The
> plan originally put explicit teardown only on the Phase-D
> `RNSD_PORT_DEST` handle; an outbound-only Phase-C consumer needs its
> own teardown before Phase D lands, hence this addition.

> **Slot↔Link mapping correction (supersedes §6.2 / decision #8).**
> mR passes `Link` wrapper *copies* into callbacks (different address,
> same `shared_ptr<LinkData>`), so the literal `&slot->link == &link`
> in decision #8 cannot work — the Phase B probe code already noted
> this. rnsd resolves slots by **shared-LinkData pointer identity**
> via the public `operator<`: `sameLink(a,b) = a&&b && !(a<b)&&!(b<a)`.
> The packet callback has no `Link&` arg; Transport stamps
> `packet.link()` before `link.receive()`, so resolve via that. Intent
> of decision #8 (slot↔Link by shared identity) preserved; only the
> comparison expression changed.

### What works (verified on hardware, 2026-05-15)

- **Phase A regression battery (§4.4 / §5.1.5)** — all pass: no
  `Link::… on Link-disabled build` lines, announces propagate both
  ways, `rnprobe` both directions, opportunistic LXMF outbound,
  `accepts_links(false)` reject (host→device Link sits at PENDING for
  the full 20 s, never advances — silent LR drop as designed).
- **Phase B outbound Link establishment** — `device → host
  lxmf.delivery` reached `ACTIVE` end-to-end. Full trace observed: LR
  sent → LRPROOF validated → MTU 500 negotiated → ESTABLISHED in
  **184–195 ms**, RTT ~105 ms. Test packet (16 B) encrypt+send on the
  established Link succeeds. mR's Link.cpp did **not** exhibit any of
  the §5.2 expected failure modes on first contact — see memory
  `project_link_phase_b_first_light.md`. Treat Link.cpp as
  likely-correct; hunt bugs in rnsd glue / Resource / inbound first.
- **Leak check** — 4 of 5 open/close cycles clean (`pending_links: 0,
  active_links: 0` after every teardown). The 1 miss was a dropped
  command (see notify-drop storm below), not a leak.

### Test rig (built this round, lives in tree)

- `scripts/rns up` — dev-loop rnsd on the Mac. Config now has
  `respond_to_probes = Yes` and the bogus `LoopbackInterface` line
  removed (`docs/testing.md` warned it doesn't exist in mR).
- `scripts/link-probe <hash> [aspect…]` — Python host→device outbound
  Link probe via the dev-loop shared instance. Used for Phase A §5.1.5
  reject check.
- `scripts/lxmf-echo [--echo]` — Python LXMF receiver/echo, joins the
  dev-loop rnsd as a shared-instance client.
- **Device-side CLI added** ([main/rnsd.cpp](../../main/rnsd.cpp)):
  `rnsd link <hash> [aspect]` (one-shot outbound Link probe; default
  aspect `rnstransport.probe`), `rnsd link teardown`, `rnsd links`
  (prints `_pending_links` / `_active_links` sizes — the §5.1.8 leak
  instrumentation). Backed by a single static `s_probe_link` slot, a
  `rnsd.cmd.link.open` storage subscriber on the rnsd task, and
  established/closed/packet callbacks. This is the §5.1.6 probe, *not*
  the Phase C consumer API.
- µR fork: `Transport::pending_links_count()` /
  `active_links_count()` added to
  [Transport.h](../../components/microreticulum/src/Transport.h).

### Bugs found and fixed (built, **not yet flashed/verified** — device
was wedged in AP mode at session end)

1. **Inbound-LXM persistence was silently broken.** Storage's
   path-parser (`navigatePath`/`navigateOrCreate`/`navigateLeaf` in
   [spangap-core/src/storage.cpp](../../../spangap/spangap-core/src/storage.cpp))
   used `char seg[48]` / `char leaf[48]`. lxmf's inbound msg keys are
   `s.lxmf.id.0.msgs.<64-hex-SHA256>.<field>` — the 64-char segment
   exceeded 48, so `navigatePath` returned nullptr and `storageSet`
   no-op'd **silently**. `[lxmf] id N: recv mid=…` logged but nothing
   persisted; `lxmf msgs in` always returned 0. This is *the* reason
   the plan's §1 "send-only node" symptom persisted even when peers
   replied opportunistically. Fix: all six seg/leaf buffers 48 → 96,
   plus a loud `warn("storage: segment too long …")` at the three
   reject sites, plus CLI `set` key 48 → 128 and `show` partial-match
   buffers 64 → 128 / 128 → 256. See memory
   `project_storage_seg_buffer_96.md`.
2. **`DestinationEntry::decode` WARN spam.**
   [Persistence/DestinationEntry.cpp:134](../../components/microreticulum/src/Persistence/DestinationEntry.cpp#L134)
   logged `Path Interface <hash> not found` at WARNING on *every*
   `TypedStore::get()` (no decoded-value cache — every path lookup
   re-decodes). Under normal announce traffic with a stale iface hash
   (routine after a TCP-iface reconnect re-registers) this fired
   ~333 Hz and starved CLI/WiFi tasks. Demoted to VERBOSE; the call
   site already handles a null `_receiving_interface`.

### Open / deferred

- **Notify-drop storm** on `rnsd.cmd.link.open=` during tight
  back-to-back Link cycles. Root cause: the storage task is itself a
  catch-all subscriber (`storageSubscribeChanges("", …)` at
  [spangap-core/src/storage.cpp](../../../spangap/spangap-core/src/storage.cpp),
  ~line 1498) feeding the browser DC sync; when that backs up, every
  config write piles in storage's ITS inbox until drops kick in.
  Non-fatal (caused the one missed leak cycle). Revisit before N>1
  concurrent Links or before relying on rapid `rnsd link` cycling.
- **Phase B §5.3 not fully closed.** Still owe: (a) outbound to
  `rnstransport.probe` specifically with the SENT→DELIVERED
  `OUT_RESULT` upgrade (we tested `lxmf.delivery`, which is what the
  host announces; the dev-loop rnsd's `rnstransport.probe` dest isn't
  announced so the device never learns its hash — either announce it
  host-side or `rnsd link <hash> rnstransport.probe` after a manual
  path request); (b) receive-side proof — `lxmf-echo`'s LXMRouter
  silently drops the raw `reticulous probe` bytes since they don't
  parse as an LXM; add a `packet_callback` on the inbound Link in
  `lxmf-echo` to close the loop; (c) the 100-cycle DRAM-leak soak (we
  did 5).
- **Phase C (`RNSD_PORT_LINK` consumer API)** — implemented.
  `rnsdLinkOpen()` is real: `RNSD_PORT_LINK` ITS server
  (`RNSD_MAX_LINK_CONNS=8`, 4096/4096, packet-mode), `s_link_conns[]`
  slot table, async kickoff (recall → request_path → `RNS::Link`),
  established/closed/packet thunks resolving slots by `sameLink`
  shared-LinkData identity, one-packet pre-active outbox, per-link
  `rnsd.links.<tag>.*` state tree, `linkTick()` (no-path / establish
  timeout / orphan-ttl cull / terminal-grace reclaim). `rnsd.h` gained
  a `tag` arg. A dedicated `clink` ITS-client test-consumer task
  (`rnsd clink <hash> [aspect] | send <t> | close`) exercises the full
  path per §6.3 — distinct from the Phase B `rnsd link` probe (which
  builds `RNS::Link` on the rnsd task). **Deferred within C:** §10a.1
  in-session consumer reconnect/re-attach (lxmf Phase E is the first
  reconnecting consumer; the plan treats 10a as cross-cutting C/D/E) —
  for now consumer disconnect parks the Link for `orphan_ttl_s` then
  tears it down, never silently dying with the handle.
  `rnsdDestListenLinks()` remains a Phase D stub.

### Environmental blocker (not code)

Device dropped to AP mode (`192.168.1.1`) after a flash — WiFi scan
didn't find the known SSIDs (A20 / WeFree); coverage/RF, not a
regression. Recovery needs physical access: move into range, or join
the device's `reticulous` AP and re-add WiFi creds via the web UI.
Once back on the LAN, flash `build/reticulous.bin` (already built with
every fix above) and the first thing to re-verify is inbound LXM
persistence: send to the device's `lxmf.delivery` from `lxmf-echo`,
then `spangap-cli lxmf msgs in` should be non-empty (was 0 before the
storage fix).

### Files touched (all uncommitted)

- `reticulous/main/rnsd.cpp` — `rnsd link`/`rnsd links`, slot,
  callbacks, `rnsd.cmd.link.open` subscriber.
- `reticulous/components/microreticulum/src/Transport.h` — link-table
  size getters.
- `reticulous/components/microreticulum/src/Persistence/DestinationEntry.cpp`
  — WARN → VERBOSE.
- `reticulous/scripts/{link-probe,lxmf-echo,rnsd.config.template}` —
  new test-rig tooling.
- `reticulous/.rns/config` — dev-loop config (gitignored).
- `spangap/spangap-core/src/storage.cpp` — seg/leaf 48 → 96, overflow
  warn, CLI buffer bumps.

---

## 1. Why this matters

Today rnsd exposes **opportunistic** (single-packet) delivery on
[`RNSD_PORT_DEST`](../../main/ports.h) and that's it. That's enough to
*send* LXMF messages and announce, but for receiving messages from real-world
LXMF peers it isn't enough. Here's why.

When an LXMF peer wants to deliver a message to a destination, the
LXMessage constructor at
[LXMessage.py:389](../../research/LXMF/LXMF/LXMessage.py#L389) picks a
default delivery method:

```python
if self.desired_method == None:
    self.desired_method = LXMessage.DIRECT
```

`DIRECT` means: open a Reticulum **Link** to the recipient first, then send
the message over the encrypted link.

**Empirical confirmation**: we send our (opportunistic) LXMF outbound to
nine reachable echo bots. We see the bots' subsequent path requests
land — `Answering path request for destination e9904da9695d0394ab52d8e1fe25c0a5 on Interface[tcp/0], destination is local to this system`
— which means they accepted our packet, validated our signature, decided
to reply, and need a path to us. But the next thing in our log is:

```
E [rnsd] Link::validate_request called on Link-disabled build
```

repeated every 30–45 s as the bot's link-establishment timeout fires and
retries. No bot eventually falls back to opportunistic delivery; they
just give up.

In short: **without Link, nobody can talk back to us.** Until this is
fixed we're a send-only node, which is the wrong half of useful.

Beyond LXMF, Link gates:

- **Resource** transfers — large messages, file attachments,
  propagation-node sync (everything > ~440 B encrypted).
- **`rnstatus -R`** and **`rnpath -R`** against us
  ([Transport.py:254-255](../../research/Reticulum/RNS/Transport.py#L254-L255))
  — both use Link + Request/Response on
  `rnstransport.remote.management`. We currently announce that
  destination but can't service it.
- **Channel** (Link's full-duplex sub-stream API used by NomadNet pages,
  Sideband telemetry, MeshChat voice, etc.).
- **Reticulum's encrypted control plane** in general.

---

## 2. Current state of the µR Link layer

| File | Status | Notes |
|---|---|---|
| `Link.h` | In tree, included in build | ~286 LOC. Class declaration only. |
| `Link.cpp` | **In tree but excluded from build** | ~1853 LOC. Mostly ported from `RNS/Link.py`. Excluded because of one external dep: `#include <MsgPack.h>` (a third-party Arduino msgpack lib mR's upstream uses). See [CMakeLists.txt:58-65](../../components/microreticulum/CMakeLists.txt#L58-L65). |
| `Link_stub.cpp` | In tree, in build | Link-time no-op stubs so `Transport.cpp` can reference `Link::receive`, `Link::status`, etc. without a real implementation. Each stub `ERROR()`s if reached (none should be at runtime since we never create `Link` instances yet). |
| `Resource.cpp` | In tree, **in build** | Compiles today. Untested. Used by Link for chunked transfers. |
| `Channel.cpp` | In tree, **in build** | Compiles today. Untested. Used by Link for full-duplex sub-streams. |
| `LinkData.h` | In tree | Constants / structs only. |

So the heavy lift — porting 1535 lines of `Link.py` to C++ — is already
done. The remaining work is:

1. Resolve the `MsgPack.h` dependency so `Link.cpp` builds.
2. Drop `Link_stub.cpp` from the build, swap in `Link.cpp`.
3. Find the bugs that bite once Link instances actually start touching
   `Transport`'s `_pending_links` / `_active_links` tables.
4. Validate against upstream Reticulum peers (mR has drifted from
   upstream in subtle ways before — RAII guards, mutexes, ratchet
   handling — and Link is a much bigger surface than what's tested so
   far).
5. Wire rnsd's client API on top so consumers don't have to know mR
   types.
6. Integrate `lxmf` to actually use DIRECT delivery for receiving
   messages.

---

## 3. Scope

### 3.1 In scope

- **Outbound Link** from the rnsd-hosting device to a peer. Used to:
  - send LXMF DIRECT messages (replies to inbound, fall-back from
    oversize opportunistic);
  - issue `/status` and `/path` Requests against another node's
    `rnstransport.remote.management`;
  - exercise upstream peers from our CLI tools.
- **Inbound Link** to a destination we host. Used to:
  - receive LXMF DIRECT messages (the primary need);
  - service inbound Requests on `rnstransport.remote.management` once
    we register handlers for it.
- **Resource** transfers in both directions, riding on Link.
  Mandatory for LXMF DIRECT once messages exceed
  `LINK_PACKET_MAX_CONTENT` (~440 B encrypted) — without Resource we
  silently drop big inbound messages and can't send big outbound ones.
- **rnsd client API**: `rnsdLinkOpen()` (outbound), `rnsdDestListenLinks()`
  (inbound forwarding), packet-mode ITS handles on `RNSD_PORT_LINK`.

### 3.2 Out of scope (defer)

- **Channel** as an explicit consumer-facing API. It still has to compile
  and not crash since Link references it, but we don't expose it via rnsd.
  Channels are used by NomadNet pages and Sideband telemetry — neither
  is a v1 reticulous workload. Re-evaluate when a concrete consumer
  shows up.
- **Propagation node** (`lxmd`-equivalent inbox-on-our-device for offline
  peers). Needs Link + Resource + LXMF's propagation peer-sync protocol.
  Own plan, post-Link.
- **`rnstransport.remote.management` request handlers**. We host the
  destination today (passive announce); serving `/status` and `/path`
  needs Link + Request/Response handlers. Plan §11 below sketches the
  hookup, but the implementation is a follow-up.

### 3.3 Non-goals

- We are **not** going to fork or replace the upstream Link wire
  protocol. The whole point is interop with the existing Reticulum
  network. Any deviation we discover is a bug, not a feature.
- We are **not** going to add a "Link enabled?" runtime flag. Either
  the build supports Link or it doesn't (Link.cpp in or out). Mixed-mode
  is more state to think about for no gain.

---

## 4. Phase A — make Link.cpp build

Smallest-possible-change phase. Goal: replace `Link_stub.cpp` with the
real `Link.cpp`, with everything else equal. No new APIs, no behaviour
changes. We pay the symbol cost, run the tests, see what falls over.

### 4.1 MsgPack shim

`Link.cpp` uses `MsgPack::Packer` / `MsgPack::Unpacker` / `MsgPack::bin_t`
from [hideakitai/MsgPack](https://github.com/hideakitai/MsgPack) — an
Arduino-flavoured msgpack library. We don't ship msgpack-c (the original
plan claimed otherwise — it isn't in REQUIRES, and adding a managed
component just for this surface is overkill). The clean fix is a
~200-line `src/MsgPack.h` header that hand-rolls the small subset of
msgpack-wire-format Link.cpp + `ResourceAdvertisement` actually use —
float64, fixarray/array16/array32, fixmap/map16/map32, bin8/bin16/bin32,
fixstr, uint compact encoding, and `nil` for sentinels. Pure C++17 fold
expressions over the variadic packer/unpacker. No upstream patch, no
external dep.

API surface to cover (from `grep MsgPack:: components/microreticulum/src/Link.cpp`):

```cpp
namespace MsgPack {
    template<class T> using bin_t = std::vector<T>;     // raw byte vector

    class Packer {
    public:
        void to_array(...);                             // variadic, packs each arg
        const uint8_t* data() const;
        size_t         size() const;
    private:
        msgpack_sbuffer buf;
        msgpack_packer  pk;
    };

    class Unpacker {
    public:
        void feed(const uint8_t* p, size_t n);
        bool from_array(...);                           // variadic, unpacks into each arg
    private:
        msgpack_unpacked result;
        size_t           off;
        std::vector<uint8_t> raw;
    };
}
```

Call sites in Link.cpp pack/unpack four shapes:

- `[double, bin_t<uint8_t>, bin_t<uint8_t>]` — request hash + path + data
- `[double]` — RTT echo
- `[bin_t<uint8_t>, bin_t<uint8_t>]` — request_id + response
- Same shapes for unpack.

`variadic to_array` / `from_array` can be done with C++17 fold
expressions over each arg's msgpack representation, with overloads for
`double`, `bin_t<uint8_t>`, and `RNS::Bytes` (already wraps a byte
vector). Total LOC ≤ 100.

**Deliverable**: `components/microreticulum/src/MsgPack.h`, no upstream
patch. Link.cpp compiles unchanged.

### 4.2 Build flip

- Remove `"src/Link_stub.cpp"` from `MICRORET_SRCS`.
- Add `"src/Link.cpp"`.
- Update the comment block to reflect the new state.

Expected fallout: there may be one or two other Link-internal references
that the stub silently no-op'd. Compile, fix, repeat. Should not be more
than a couple of iterations.

### 4.3 Gate `accepts_links` per-destination (must-do, easy to miss)

**Verified 2026-05-12:** mR's `Destination::_accept_link_requests`
defaults to `true`
([Destination.h:232](../../components/microreticulum/src/Destination.h#L232)),
and `Destination::receive` unconditionally hands `LINKREQUEST` packets to
`incoming_link_request`
([Destination.cpp:399-401](../../components/microreticulum/src/Destination.cpp#L399-L401)),
which inserts the resulting `Link` into `_object->_links` whenever
`accepts_links()` is true
([Destination.cpp:422-439](../../components/microreticulum/src/Destination.cpp#L422-L439)).

Today `onMailboxConnect`
([rnsd.cpp:611+](../../main/rnsd.cpp#L611)) never sets
`accepts_links(false)` on the IN destination it constructs. Pre-Phase-A
this is harmless — `Link::validate_request` is stubbed and returns
`NONE`, so `_links.insert(link)` no-ops. **As soon as Phase A lands,
every `lxmf.delivery` destination and `rnstransport.remote.management`
starts accepting LRs and silently accumulating half-functional Link
objects** (no `set_link_established_callback` wired yet → packets land
nowhere; `_pending_links`/`_active_links` accumulate until cull).

So Phase A must include:

- `onMailboxConnect`: `d.accepts_links(false)` immediately after
  construction
  ([rnsd.cpp:666-668](../../main/rnsd.cpp#L666-L668)) and before
  `set_packet_callback`.
- `s_management_dest`: same after construction
  ([rnsd.cpp:1145-1149](../../main/rnsd.cpp#L1145-L1149)).
- `s_probe_dest` already does this
  ([rnsd.cpp:1207](../../main/rnsd.cpp#L1207)) — no change.

Phase D's `RNSD_DEST_LINK_LISTEN` aux is then the *only* way for any
destination to flip back to `accepts_links(true)`.

### 4.4 Phase A acceptance criteria

- Clean build, no new warnings.
- Boot the firmware. No `ERROR` log lines from Link methods (the
  stub-error log family stops appearing).
- `rnstatus` still works (no regressions in opportunistic path).
- `lxmf send` to an echo bot still works opportunistically (we already
  proved this).
- Peers' LRs against `lxmf.delivery` / `remote.management` now log
  "rejected: accepts_links false" (or the µR equivalent) rather than
  silently spawning unattached Link objects. Probe still rejects as
  before.
- A peer trying to open a Link to a destination we *will* later flip on
  (Phase D) — none yet — verifiably no longer logs
  `Link::validate_request called on Link-disabled build`. The Link may
  still fail to establish; that's Phase B.

---

## 5. Phase B — Link establishment end-to-end

Goal: a Link opened from us to an upstream Reticulum peer (or vice versa)
gets to `Type::Link::ACTIVE`, can carry one packet of plaintext each way,
and tears down cleanly.

### 5.1 Test-rig setup checklist

Sized to be a *runbook*. Follow top-to-bottom; each section is a
prerequisite for the one below it.

#### 5.1.1 Hardware + serial

- LilyGo T-Deck Plus.
- USB-C cable to the dev laptop. Serial port appears as
  `/dev/tty.usbmodem2101` (varies on reset — re-check after each
  flash with `ls /dev/tty.usbmodem*`).
- Flash + monitor combined:
  ```bash
  . ~/.espressif/v5.5.4/esp-idf/export.sh
  cd /Users/rop/code/spangap/reticulous
  idf.py -p /dev/tty.usbmodem2101 flash monitor
  ```
  Exit monitor with `Ctrl+]`.

#### 5.1.2 Network topology

Both board and laptop on the same LAN (or laptop in AP mode, board
joining). Phase 1 only has TCP transport, so:

- Laptop's LAN IP known and stable (DHCP reservation or static).
- Firewall allows the chosen TCP port (default 4242).
- `s.net.wifi.nets.0.ssid`/`.pass` set on the board for the shared
  network; verify with `net status` CLI on the board.

#### 5.1.3 Laptop-side Reticulum install

```bash
pip install rns lxmf
```

Create `~/.reticulum/config`:

```
[reticulum]
  enable_transport = yes
  respond_to_probes = yes

[interfaces]
  [[Default Interface]]
    type = TCPServerInterface
    listen_ip = 0.0.0.0
    listen_port = 4242
```

Then:

```bash
rnsd -v &              # starts the daemon, generates an identity
rnstatus               # confirm the iface is up and our identity is listed
```

Note the laptop's destination hash for `rnstransport.probe` (printed by
`rnstatus`) — we'll target it for outbound-Link tests.

#### 5.1.4 Board-side TCP iface config

In the device CLI (over the monitor):

```
set s.tcp.enable           1
set s.tcp.peers.0.host     <LAPTOP_LAN_IP>
set s.tcp.peers.0.port     4242
save
```

Then reboot. Confirm:

- Board's `rnsd` CLI shows the TCP iface UP in `rnstatus`.
- Laptop's `rnstatus` shows the board's announces arriving (our
  identity + `rnstransport.probe` + `rnstransport.remote.management` +
  any `lxmf.delivery` identities we have).

#### 5.1.5 Phase A regression battery (run before any Phase B work)

A green pass here is the gate to starting Phase B debugging.

1. **Clean boot.** No `ERROR` lines from Link, ResourceAdvertisement,
   register_request_handler, or accepts_links code. No
   `Link::… called on Link-disabled build` lines (those should be
   permanently gone post-Phase-A).
2. **Announces propagate.** Laptop's `rnstatus` lists our identity
   within ~30 s of boot.
3. **Probe both directions.**
   - From laptop: `rnprobe <our_identity_hash>` — should PROVE_ALL back.
   - From board CLI: `rnprobe <laptop_identity_hash>` — should
     succeed (this exercises our outbound opportunistic path).
4. **Opportunistic LXMF still works.** From board CLI:
   `lxmf send <short_msg> <peer_lxmf_hash>` to an echo bot or
   laptop-side LXMF receiver. Reply (if opportunistic) lands in our
   inbox per the existing flow.
5. **`accepts_links(false)` confirmed.** Run the script in §5.1.7
   pointing at our `lxmf.delivery` — it should fail to establish a
   Link (timeout from the laptop side), and the board log should show
   the µR equivalent of "incoming_link_request: accepts_links false,
   dropping" *without* growing `_pending_links` or `_active_links`. To
   inspect mR's tables, see §5.1.8.

If any of 1–5 fails, fix before proceeding.

#### 5.1.6 Tools that need to be added before Phase B starts

Two CLI verbs and one Python script. None are big; bundle into the
first Phase B PR.

**`rnsd link <dest_hash>` CLI** — temporary one-shot outbound-link
probe. Outline (in `rnsd.cpp` alongside `rnprobe`):
- Parse the 16-byte hash from hex.
- `rnsdRecallPubkey` → if unknown, `rnsdRequestPath` and wait up to
  `s.rnsd.link.path_timeout_s`.
- Construct an `RNS::Link` directly (skipping the `RNSD_PORT_LINK`
  consumer API since that's Phase C).
- Wire `set_link_established_callback` (print link_id, mtu, rtt),
  `set_packet_callback` (hex-dump bytes received), and
  `set_link_closed_callback` (print teardown reason).
- On established: send one test packet `b"reticulous probe"`, then
  `link.teardown()` after either a 5 s timeout or a packet response.
- Print state transitions and final timing.

**`rnsd links` CLI** — print `_pending_links.size()` /
`_active_links.size()` from `RNS::Transport`, plus any rnsd-side link
slot state once Phase C lands. For Phase B, just the mR table sizes.
This is the leak-detection probe.

**Throwaway Python LXMF client** — pasted into `tools/` (in
git-ignored space if you like). ~30 lines using the upstream `lxmf`
library, opens a `DIRECT` delivery against a given dest hash, logs
every callback (`delivery_callback`, `failed_callback`, link state
transitions). Used both for testing the §5.1.5 "accepts_links(false)
rejection" path and, post-Phase-D, for real inbound LXMF.

#### 5.1.7 Sample throwaway Python clients

Place under `tools/` (git-ignored). Two scripts:

**`tools/link_probe.py`** — opens a raw Link to a given dest hash,
sends one packet, waits for proof, exits. Mirrors the board-side
`rnsd link` CLI but from the laptop side, used to drive inbound-Link
testing against our board:

```python
import RNS, sys, time
r = RNS.Reticulum()
remote = bytes.fromhex(sys.argv[1])
ident  = RNS.Identity.recall(remote)
if not ident:
    RNS.Transport.request_path(remote)
    while not ident:
        time.sleep(0.5); ident = RNS.Identity.recall(remote)
dest = RNS.Destination(ident, RNS.Destination.OUT,
                       RNS.Destination.SINGLE,
                       "lxmf", "delivery")   # or the aspect to test
def on_estab(l):  print("ESTABLISHED", l.link_id.hex(), "mtu", l.mtu)
def on_closed(l): print("CLOSED", l.teardown_reason)
def on_pkt(d, p): print("PKT", d.hex())
link = RNS.Link(dest, established_callback=on_estab,
                closed_callback=on_closed, packet_callback=on_pkt)
time.sleep(30); link.teardown()
```

**`tools/lxmf_send.py`** — sends a DIRECT-mode LXMF message to a
destination hash and logs every state transition. Used for Phase E
acceptance later, but useful in Phase B for triggering the inbound LR
path. ~25 lines using `LXMF.LXMRouter` + `LXMF.LXMessage(method=DIRECT)`.

Both should print upstream Reticulum's verbose `RNS.log` output —
keep `loglevel=RNS.LOG_DEBUG` or higher.

#### 5.1.8 Inspecting mR's link tables from the board

For Phase B leak-detection per §5.3, we need a way to see
`_pending_links.size()` and `_active_links.size()`. Two options:

- Add the `rnsd links` CLI in §5.1.6.
- Cheaper for first-light: bump rnsd's existing 1 Hz stats publish to
  also publish `rnsd.stats.pending_links` and `rnsd.stats.active_links`.
  Browser panels see them via the existing storage sync.

#### 5.1.9 Temporarily flipping accepts_links for inbound testing

Phase B's inbound acceptance test (§5.3) needs a destination to
accept LRs *before* Phase D's RNSD_DEST_LINK_LISTEN wiring exists. Two
clean options:

- Add a `rnsd accept-links <dest_aspect> on|off` debug CLI that walks
  `s_mailbox_conns[]`, matches by aspect, and toggles
  `accepts_links()`. Easy to remove later.
- Add a `s.rnsd.test.accept_all_links = 0|1` storage gate; when 1,
  rnsd flips every mailbox dest to `accepts_links(true)` on creation.
  Easier but uglier.

Recommendation: the CLI verb. It's scoped, leaves no permanent
storage key behind, and disappears with Phase D.

#### 5.1.10 Stress harness for the leak test (§5.3 acceptance)

```bash
# laptop side — repeat outbound LR against the board's probe dest
for i in $(seq 1 100); do
  python tools/link_probe.py <board_probe_hash>
  sleep 2
done
```

Watch `rnsd.stats.pending_links` and `rnsd.stats.active_links` on the
board after each round. Also dump heap HWM:

```
top heap
```

Acceptance: flat heap HWM across 100 cycles and `pending_links` /
`active_links` returning to 0 within `STALE_TIME` (~12 min).

#### 5.1.11 Wire capture (optional, useful for crypto debug)

When §5.2 "Crypto derivation mismatch" looks likely, `tcpdump -i any
-w link.pcap port 4242` on the laptop captures the raw RNS-over-TCP
stream. Compare HEADER_2 bytes against upstream Reticulum's expected
encoding ([RNS/Packet.py:pack](../../research/Reticulum/RNS/Packet.py)).
The Packet.cpp spangap patch (first-8-bytes hex on malformed-parse)
helps narrow this on the board side; the wire capture nails it from
the peer side.

### 5.2 Failure modes to expect

`Link.cpp` is 1853 lines of ported code that has never run end-to-end
against upstream. The expected bug categories:

| Category | Why | How to find |
|---|---|---|
| **Crypto derivation mismatch** | HKDF salt/context strings differ subtly between mR fork and upstream | Compare derived `_link_id` and per-direction keys on both ends; first packet decryption fails with HMAC mismatch. |
| **Ratchet involvement on link establishment** | Upstream uses a ratchet pubkey in the LR if the destination has ratchets enabled. mR's fork doesn't include the ratchet field. | LR parse off-by-32. Same bug as the announce format we worked around for opportunistic — same fix applies (gate on `context_flag`). |
| **Frame sequencing** | Link packets carry an HMAC chain + per-direction sequence numbers. Off-by-one or endian flips silently kill the link. | Stress-test with rapid bidirectional traffic; verify against captured upstream traffic. |
| **Threading** | Link adds `_pending_links` / `_active_links` to Transport's state machine, accessed from `Transport::jobs` and `Transport::inbound`. Both already run on the rnsd task in our model; verify nothing else (mR's `Link::send`, packet callbacks) accidentally runs cross-task. | Same recursive-mutex audit we did for `_known_destinations`. |
| **MTU clamping** | Upstream has the link-MTU-upgrade negotiation; intermediate hops can clamp it. Our `Transport::inbound` has the LR-handling block but it's never been exercised. | Test through a 2+ hop path; verify MTU survives clamp. |
| **Stale `_pending_links` / `_active_links` accumulators** | mR has a pattern of `// CBA ACCUMULATES` markers on tables that grow without bound. Link's tables need a TTL / cull pass. | Stress-test long-running establishment failures; check `_pending_links.size()` after an hour. |

This phase is the unbounded one. Budget 1-2 weeks of debugging.

### 5.3 Phase B acceptance criteria

- Outbound: from our board, open Link to a Python rnsd peer's
  `rnstransport.probe`. Link reaches `ACTIVE`. We can send one Link
  packet, the responder PROVE_ALLs it (now over Link, so a "delivered"
  ack rather than just a PROOF), and we get the `OUT_RESULT` upgrade
  from `SENT` to `DELIVERED` (status 1, already in our wire format).
- Inbound: a Python LXMF client opens a Link to our hosted
  `lxmf.delivery` destination. We accept it and the link reaches
  `ACTIVE` on both sides. (Inbound packets are deferred to Phase D.)
- A Link torn down on either side cleans up `_pending_links` /
  `_active_links` / `_reverse_table` entries on the other side within
  one cull interval.
- No `ERROR` log lines from Link internals on the golden path.
- No measurable DRAM leak after 100 link-open / link-close cycles.

---

## 6. Phase C — `RNSD_PORT_LINK` (outbound API)

Goal: expose Link to non-rnsd tasks via the client API
[`rnsdLinkOpen()`](../../main/rnsd.h) — stubbed today, lit up here.

### 6.1 ITS surface

[`RNSD_PORT_LINK = 10`](../../main/ports.h) is reserved in ports.h.

```c
/* rnsd-private (lives in rnsd.cpp like rnsd_dest_connect_t). */
typedef struct {
    uint8_t  dest_hash[16];      // target's destination hash
    char     aspect[32];         // target's aspect (for OUT destination ctor)
    char     identity_key[40];   // our identity (for link auth); "" → rnsd default
    char     tag[24];            // caller-chosen short tag, e.g. "lxmf.id0.4" —
                                 // used to key the link's storage state so the
                                 // caller (and the browser) can watch progress
                                 // before the link_id is even derived
    uint8_t  reserved[8];
} rnsd_link_connect_t;
/* sizeof == 120; `ITS_MAX_MSG_DATA = 320`
   ([include/its.h:55](../../../spangap/spangap-core/include/its.h#L55))
   so the static_assert mirrors `rnsd_dest_connect_t`'s. */
```

`rnsdLinkOpen(...)` (rnsd.h):

1. Build the connect payload from typed args. Caller supplies `tag`
   (unique per concurrent in-flight link for that caller — e.g.
   `"lxmf.id<n>.<slot>"`).
2. `itsConnect("rnsd", RNSD_PORT_LINK, &req, sizeof(req), …)`. **Connect
   accepts immediately** — the ITS handle comes back before any
   Reticulum handshake has happened. The Link sits in `establishing`
   state, and the caller watches `rnsd.links.<tag>.state` for the
   transition to `active` / `failed`.
3. Handle is packet-mode: each `itsSend` = one Link packet, each
   `itsRecv` = one Link packet. No type byte. Sends queued before the
   Link is `active` are buffered (one-packet outbox) and flushed on
   establishment, or dropped with `rnsd.links.<tag>.last_error` set on
   failure.

This matters for two reasons:

- **No ITS "deferred connect"**. Reticulum Link establishment can take
  seconds (path request → LR → LRPROOF → key derivation). ITS connect
  is synchronous-accept/reject by design — blocking the rnsd task for
  seconds per outbound link is the wrong shape.
- **Browser visibility for free**. The browser already mirrors storage
  over WebRTC. Status that lives in `rnsd.links.<tag>.*` shows up in
  whatever panel renders it, without rnsd having to also push aux
  frames over a separate DC. ITS aux frames are device-internal; vars
  cross to the browser by default.

### 6.2 rnsd internals

- New slot table parallel to `s_mailbox_conns`:
  `s_link_conns[RNSD_MAX_LINK_CONNS]`, with `RNSD_MAX_LINK_CONNS = 8`
  (single-user node: 1 outbound per identity + 2–3 inbounds is the
  realistic peak; 8 gives 5× headroom at ~16 KB).
- ITS server port opens with `itsServerPortOpen(RNSD_PORT_LINK,
  /*packetBased=*/true, RNSD_MAX_LINK_CONNS, 4096, 4096)` — ~9 in-flight
  Link packets per direction, matches Resource cadence without bloating
  the stream-buffer pool.
- Storage-tunable timeouts: `s.rnsd.link.path_timeout_s` (default 30 —
  matches upstream Reticulum's LR retry budget),
  `s.rnsd.link.orphan_ttl_s` (default 600 — mirrors mR's `STALE_TIME`).
  Slot fields beyond the usual `used/handle/req/tag`: an owned `RNS::Link`
  (assignable; default-constructed as `Type::NONE` until kicked off) and a
  small `pending_outbound` (one packet) for sends-before-active.
- **Slot ↔ Link mapping.** mR's callbacks have shape
  `void(*)(Link& link)` —
  [Link.h:127-145](../../components/microreticulum/src/Link.h#L127-L145) —
  no userdata pointer. Resolve a slot in the thunk by walking
  `s_link_conns[]` and matching `&slot->link == &link` (mR stores
  `Link` in `std::set` and accessor refs are stable for the link's
  lifetime). Once `Link::status() == ACTIVE`, also build the reverse
  index `rnsd.links.byid.<link_id> = <tag>` for inbound-side lookups.
- `onLinkConnect(handle, data, len)`:
  - Validate `tag` (non-empty, ≤ 24 chars), reject duplicate tags from
    the same caller.
  - Write `rnsd.links.<tag>.state = "establishing"` and the
    caller-known metadata (`tag`, `direction = "out"`, `aspect`,
    `remote_hash`, `opened_s`).
  - Return **accept** immediately — the ITS handle is live.
  - On the rnsd task (which we're already on, since storage subs and
    ITS callbacks run there): recall the target identity. If unknown,
    issue `Transport::request_path`, set
    `rnsd.links.<tag>.state = "awaiting_path"`, schedule a retry. If
    `s.rnsd.link.path_timeout_s` (default 30) elapses without a path
    answering, set `state = "failed"` + `last_error = "no_path"` and
    `itsDisconnect(handle)`.
  - Once identity known: construct `out_dest`, `RNS::Link link(out_dest)`
    — kicks off LR. Stash callbacks
    (`set_link_established_callback` → set
    `rnsd.links.<tag>.state = "active"` + populate `link_id`,
    `rtt_ms`, `mtu`; `set_link_closed_callback` → state `closing` →
    `closed`, then storageDeleteTree the entry).
- `onLinkSend` (recv on the ITS handle):
  - If `state != "active"`: buffer one packet in the slot's
    `pending_outbound`. More than one queued before active → set
    `last_error = "send_queue_full"`, drop newer.
  - If `state == "active"`: `link.send(bytes)`; refuse
    `> LINK_PACKET_MAX_CONTENT` with `last_error = "oversize"`.
- `onLinkInbound` (mR's packet callback on the Link): forward plaintext
  as one ITS packet over the consumer's handle, no type byte. Bump
  `rnsd.links.<tag>.rx_packets`, `last_inbound_s`.
- `onLinkClosed`: write `state = "closed"`, then storageDeleteTree the
  entry after a brief grace window (so subscribers see the close
  transition), then `itsDisconnect(handle)`.
- Caller `itsDisconnect`: see §10a.1 — does **not** by itself tear down
  the Link. The slot stays; rnsd holds the Link. If the caller doesn't
  reconnect within `s.rnsd.link.orphan_ttl_s` (default 600 = 10 min),
  rnsd tears the Link down. Explicit `RNSD_DEST_LINK_TEARDOWN` aux on
  the original `RNSD_PORT_DEST` handle teardown immediately.

### 6.3 Acceptance criteria

- `rnsd link <hash>` CLI (now backed by `rnsdLinkOpen()` instead of a
  one-off) works against a Python peer.
- A trivial test consumer task on the device can open a Link, send 5
  packets, receive 5 packets, disconnect, and we see no leaks in
  `s_link_conns` or in mR's link tables.

---

## 7. Phase D — `DEST_LINK_LISTEN` (inbound API)

Goal: a consumer task that already has a `RNSD_PORT_DEST` connection
(lxmf, …) registers a follow-up ITS port to receive incoming Links for
that destination.

### 7.1 ITS surface

New in-band opcode on the `RNSD_PORT_DEST` handle (same shape as the
other RNSD_DEST_* opcodes — first byte of a packet-mode frame, not an
ITS aux):

```c
enum : uint8_t {
    /* existing 0x01..0x06 */
    RNSD_DEST_LINK_LISTEN = 0x07,
    /* payload: link_inbox_port(2 BE) | resource_aux_port(2 BE) */
};
```

`rnsdDestListenLinks(dest_handle, LXMF_LINK_INBOX_PORT,
LXMF_LINK_RESOURCE_AUX_PORT)` (rnsd.h): sends a 5-byte frame on
`dest_handle`. `resource_aux_port = 0` opts out of Resource forwarding;
inbound resources will be rejected by rnsd at the advertisement (§9).
rnsd already knows the owning task: `itsRemoteTask(handle)`
([include/its.h:292](../../../spangap/spangap-core/include/its.h#L292))
returns the `TaskHandle_t` of the connecting consumer task. So the
frame only carries the port numbers. rnsd flips `accepts_links(true)`
on the destination and records the (owning_task_handle, port) pair in
the mailbox slot. Back-connects use `itsConnectByTaskHandle`
([include/its.h:171](../../../spangap/spangap-core/include/its.h#L171)).

Tying the listen registration to the existing dest handle has three
practical benefits beyond brevity:

- **One identity, one subscription.** You can't accidentally register
  Link forwarding for a destination you don't own — the act of
  registering proves you own it (you have its `RNSD_PORT_DEST` handle).
- **Lifetime tracking is free.** When the consumer disconnects the
  `RNSD_PORT_DEST` handle, rnsd already needs to clean up the listener
  destination from mR. The Link forwarding registration disappears
  with it.
- **No name lookups.** rnsd doesn't have to resolve "lxmf" → task
  handle at link-arrival time; it has the task reference cached in
  the dest conn slot.

### 7.2 rnsd internals

- Extend `mailbox_conn_t` with
  `TaskHandle_t link_listener_task`, `uint16_t link_inbox_port`, and
  `uint16_t resource_aux_port`. All zero by default → destination keeps
  `accepts_links(false)`.
- On `RNSD_DEST_LINK_LISTEN` frame: validate the args (ports non-zero
  for link_inbox; resource_aux may be zero), fill the fields, call
  `c.listener_dest.accepts_links(true)`. `link_listener_task` comes
  from `itsRemoteTask(dest_handle)`.
- When mR's `Destination::incoming_link_request` fires for that
  destination (it already does, the path was just unreachable because
  `accepts_links` was false), the resulting `Link` instance gets stashed
  in `_pending_links`. After LRPROOF + handshake reaches `ACTIVE`, we get
  a `set_link_established_callback` fire. **In that callback** we:
  - allocate a slot in `s_incoming_link_conns[]` (rnsd-generated tag:
    `"in.<hex link_id[0..8]>"`)
  - build the connect payload `rnsd_link_incoming_t` (defined below) —
    80 bytes, ≤ `ITS_MAX_MSG_DATA`
  - `itsConnectByTaskHandle(link_listener_task, link_inbox_port,
    &payload, sizeof, timeout, ref, on_recv, on_disconnect)`
  - on accept, install `link.set_packet_callback` → forward to consumer's handle
  - on reject / busy / timeout: `link.teardown()` (peer sees LR_CLOSED),
    free the slot, write `rnsd.links.<tag>.state = "failed"` +
    `last_error = "consumer_unreachable"`, then storageDeleteTree the
    entry after a brief grace window.

```c
/* rnsd-private connect payload sent by rnsd to the consumer task's
 * link_inbox_port on every accepted inbound Link. ≤ ITS_MAX_MSG_DATA. */
typedef struct {
    char     tag[24];                       // rnsd-generated, "in.<8hex>"
    uint8_t  link_id[16];
    uint8_t  remote_identity_hash[16];
    uint8_t  local_dest_hash[16];           // which of our dests it landed on
    uint16_t mtu;
    uint8_t  reserved[6];
} rnsd_link_incoming_t;
```

`local_dest_hash` matters when one consumer task hosts multiple
destinations (lxmf with multiple identities does this today — one task,
N destinations). `remote_identity_hash` is free at the rnsd side from
`link.destination()` and saves the consumer a `rnsdRecallPubkey` call.
- On `itsDisconnect` from the consumer (in-session reconnect): the
  Link stays — see §10a.1. Only `link.teardown` from the remote, or an
  explicit `RNSD_DEST_LINK_TEARDOWN` aux from the consumer (§10a.4),
  closes it.

### 7.3 Acceptance criteria

- Python LXMF client opens a Link to our `lxmf.delivery` destination.
- rnsd accepts it (`accepts_links` true now) and opens an ITS
  connection to lxmf on the registered port with the `rnsd_link_incoming_t`
  payload identifying the remote.
- (Inbound LXMF parsing happens in Phase E.)
- Tear-down from either side cleans up.

---

## 8. Phase E — `lxmf` DIRECT delivery

Goal: when an echo bot replies to us via DIRECT, the message lands in our
LXMF inbox.

### 8.1 lxmf changes

New ITS ports in `main/ports.h`:

```c
constexpr uint16_t LXMF_LINK_INBOX_PORT        = 100;  /* inbound Link forwards */
constexpr uint16_t LXMF_LINK_RESOURCE_AUX_PORT = 101;  /* Resource handoff aux (Phase F) */
```

Phase E only uses `LXMF_LINK_INBOX_PORT`; the resource port is opened
but receives nothing until Phase F.

- On identity startup, in addition to the existing
  [`rnsdDestOpen("lxmf.delivery", ...)`](../../main/lxmf.cpp) call, also
  call `rnsdDestListenLinks(handle, LXMF_LINK_INBOX_PORT,
  LXMF_LINK_RESOURCE_AUX_PORT)`.
- Open an ITS server port `LXMF_LINK_INBOX_PORT` on the lxmf task.
- On incoming connect (the `rnsd_link_incoming_t` payload arrives): track
  the link in a per-identity table. The link is now a packet-mode stream
  carrying LXMF wire bytes (no type prefix — Link is the destination, so
  the recipient already knows which destination it's for).
- On receive: for each packet, `lxmf_data = packet.destination.hash() + data`
  — same reconstruct as the opportunistic path
  ([LXMRouter.py:1827-1830](../../research/LXMF/LXMF/LXMRouter.py#L1827-L1830)).
  Then run the same `onInboundLxm` pipeline (verify sig, dedup, store).

### 8.2 Outbound DIRECT

When does our `lxmf send` want to open a Link instead of opportunistic?
Per upstream rules ([LXMessage.py:394-398](../../research/LXMF/LXMF/LXMessage.py#L394-L398)):

- If user explicitly requests DIRECT in the CLI / browser.
- If content exceeds `LXMF_OPP_CONTENT_BUDGET` (~311 B). Today
  [`processReady`](../../main/lxmf.cpp#L1186) rejects these with
  `last_error = "too large for opportunistic"`
  ([lxmf.cpp:1213-1216](../../main/lxmf.cpp#L1213-L1216)) — Phase E
  makes them work.

**Method-choice plumbing** (storage-driven, mirrors existing patterns):

- New per-message field `s.lxmf.id.<n>.msgs.<mid>.method =
  "auto"|"direct"|"opportunistic"` (default `"auto"`).
- New per-identity default `s.lxmf.id.<n>.default_method` (default
  `"auto"`).
- `processReady`'s size check becomes a method selector: under "auto",
  pick opportunistic if size ≤ budget, else direct; under explicit
  values, honour the choice and fail explicitly if direct is asked for
  but Link establishment fails.

Path:

1. Same `processReady` entry point.
2. If oversize / explicit-direct, `rnsdLinkOpen(target_dest, ...)`.
3. On link-established, send the LXMF wire (with the 16-byte dest hash
   stripped, same convention as opportunistic) as one Link packet.
4. Wait for the `OUT_RESULT DELIVERED` aux from rnsd (Link.send returns
   a `PacketReceipt` with proof-on-delivery semantics —
   [LXMessage.py:521+](../../research/LXMF/LXMF/LXMessage.py#L521)).
5. `itsDisconnect(link_handle)`.

### 8.3 Acceptance criteria

- Send `lxmf send 1 hello` to an echo bot, wait for its DIRECT reply.
- Reply lands in our inbox, signature validates, content is "hello"
  (or "you said: hello" or whatever the bot does).
- `lxmf send 1 <512 byte string>` works end-to-end (oversize fallback
  to DIRECT).
- Storage entries `s.lxmf.id.0.msgs.<mid>.*` populate as expected.
- No "Link::… called on Link-disabled build" in the log.

---

## 9. Phase F — Resource for big messages

Goal: handle LXMF messages larger than `LINK_PACKET_MAX_CONTENT` (~440 B
encrypted). Required for any real attachment or long text.

`Resource.cpp` is already in our build but `ResourceAdvertisement` is a
stub — see §9.0.

### 9.0 µR porting prereq

> **⚠️ CORRECTED 2026-05-16 — this section materially understated the
> prereq. Verified by reading the fork:**
>
> `ResourceAdvertisement` **is** now ported (full pack/unpack, fields
> t/d/n/h/r/o/i/l/q/f/m — [Resource.h:132+](../../components/microreticulum/src/Resource.h#L132)).
> That part is done.
>
> **But the entire Resource *transfer engine* is unported.**
> `Resource.cpp` is a 329-line shell: both `Resource::Resource(...)`
> ctors have empty bodies (allocate `ResourceData`, nothing else);
> `validate_proof`/`cancel` are empty `{}`; `get_progress` is
> commented out; the rest are trivial getters.
> **`Resource::accept` is not defined anywhere** — Link.cpp:1165/
> 1200 reference it from inside commented/`//z` blocks. No
> `receive_part`, `hashmap_update`, `request_next`, `assemble`,
> `__advertise_job` exist in the fork. Upstream `RNS/Resource.py` is
> ~1500 LOC of windowed hashmap reconstruction, part request/retry
> flow control, proof, and segmented multi-part transfers — **none
> of it ported.**
>
> So Phase F is **not** the "shared-memory aux wire-up" §9.1
> describes. The real prereq is porting the upstream Resource
> state machine (both directions) into the µR fork *first* — a
> large, high-risk effort that cannot be incrementally hardware-
> verified until substantially complete. Phase F effort/risk in §11
> is revised to **"port + wire, 2–3 wk, High"**. This is the
> architectural fork that halted the A–E run on 2026-05-16.
>
> **Verified 2026-05-16 (GitHub API):** `attermann/microReticulum`
> `src/Resource.cpp` is **3,706 B even at HEAD** — a stub; our pinned
> commit never had Resource and *no newer attermann commit has it
> either*. "Pull a newer upstream" is **not** an option. The fork that
> *does* have it is **`ratspeak/microReticulum`**:
> `src/Resource.cpp` ≈ **18,722 B**, a real implementation (ratspeak
> powers ratdeck, which needs file transfer). ratspeak/microReticulum
> is Apache-2.0 (license-compatible to crib from; distinct from
> ratspeak/**ratdeck** which is AGPL-3.0 — do not copy *that*).
> component-plan.md §1 chose attermann over ratspeak deliberately
> (provenance, ratspeak's `LXMFMessage` delta we don't want in mR) and
> always foresaw porting Link/Resource/Channel later — only this
> §9.0's "~150 LOC" estimate was wrong.
>
> Real options now: **(a)** port mR Resource into our fork, using
> `ratspeak/microReticulum/src/Resource.cpp` as the Apache-2.0
> reference (the path component-plan always envisioned); **(c)**
> descope attachments/propagation and ship A–E (opportunistic + DIRECT
> single-packet already covers the §1 "peers can reply" goal). There
> is no quick (b).
>
> Original (inaccurate) text follows for history:

Our fork's `ResourceAdvertisement`
([Resource.h:120-122](../../components/microreticulum/src/Resource.h#L120-L122))
is an empty placeholder. Upstream's class at
[RNS/Resource.py:1234](../../research/Reticulum/RNS/Resource.py#L1234)
carries `t` (transfer size), `d` (total uncompressed size), `n` (parts),
`h` (hash), `r` (random hash), plus flags. Port the pack/unpack via the
MsgPack shim Phase A introduces, plus the flag-byte assembly. This is a
real ~150 LOC port, not a wire-up — Phase F's effort estimate should
reflect that (bumped to 2 wk in §11).

### 9.1 Surface: shared-memory aux handoff

The Link ITS handle stays exactly as Phase C/D defined it — packet-mode,
no type bytes, one Link packet per send/recv. Resource transfers do
*not* appear on that handle at all. Instead:

1. **Advertisement arrives.** mR fires the resource callback with the
   advertisement (which carries `total_size` etc., known *before* any
   chunk is on the wire).
2. **Policy gate** in rnsd: reject if `adv.d >
   s.lxmf.max_resource_size` (default 262144 = 256 KB) or if
   concurrency caps would be exceeded.
3. **`malloc(total_size)` from PSRAM.** Spangap's malloc defaults to
   PSRAM ([CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=0](../../../spangap/CLAUDE.md)),
   so this Just Works for 256 KB-class transfers.
4. **`resource.accept(on_done_callback)`**, hand mR the buffer to fill.
   mR reassembles in place — no copies inside rnsd.
5. **Progress** published to storage at `rnsd.links.<tag>.resource.<rid>.{progress,size,parts}`,
   auto-syncs to the browser like all other rnsd state.
6. **`resource_concluded`** → rnsd sends one small ITS aux frame to the
   consumer task on the resource-aux port the consumer registered at
   `rnsdDestListenLinks` time (inbound) or `rnsdLinkOpen` time
   (outbound):

   ```c
   /* Aux opcodes on the resource-aux port. */
   enum : uint8_t {
       RNSD_LINK_RESOURCE_INBOUND_DONE = 0x01,
       RNSD_LINK_RESOURCE_OUTBOUND_DONE = 0x02,
       RNSD_LINK_RESOURCE_FAILED      = 0x03,
   };

   struct rnsd_link_resource_done_t {     /* ~64 B, ≤ ITS_MAX_MSG_DATA */
       uint8_t  opcode;
       uint8_t  link_id[16];
       uint8_t  resource_hash[32];
       void*    buf;                       /* PSRAM pointer; consumer
                                            * takes ownership on
                                            * INBOUND_DONE, must call
                                            * rnsdResourceRelease(buf) */
       uint32_t len;
       uint8_t  flags;                     /* compressed, has_metadata */
       uint8_t  reserved[5];
   };
   ```

7. **Consumer takes ownership** of `buf` on INBOUND_DONE, parses (e.g.
   the LXMF wire), then `rnsdResourceRelease(buf)` (a thin wrapper over
   `free()` — symmetry hook in case we move to a slab allocator later).

### 9.2 Ownership rules

- **Inbound:** rnsd owns the buffer until INBOUND_DONE is sent; consumer
  owns it after, must release. On FAILED (mR timeout / link drop / peer
  abort) rnsd frees the buffer itself and the aux frame's `buf` is nullptr.
- **Outbound:** caller owns the buffer for the duration of the
  transfer. `rnsdLinkSendResource(link_handle, buf, len, opaque_id)`
  *borrows* the buffer; mR reads from it to generate chunks. Caller
  must not mutate or free until OUTBOUND_DONE or FAILED. rnsd does not
  copy — doubling peak memory for a 256 KB send is the wrong trade.
- **rnsd reboot mid-transfer:** buffer leaks until reboot recovers
  everything. Acceptable per §10a.2 option (A).
- **Consumer crash mid-transfer (inbound):** rnsd has the buffer; on
  consumer disconnect, rnsd frees it. (Distinct from §10a.1 in-session
  reconnect: that path applies to the Link itself, not to mid-flight
  Resource state, which is too transient to be worth resuming.)

### 9.3 Concurrency caps

To bound peak PSRAM use under abuse:

- `s.rnsd.link.max_inbound_resources_per_link` (default 2)
- `s.rnsd.link.max_inbound_resources_total`    (default 4)
- `s.lxmf.max_resource_size`                    (default 262144 = 256 KB)

Worst case: 4 × 256 KB = 1 MB transient PSRAM hold. T-Deck Plus has
8 MB octal PSRAM; comfortable headroom.

### 9.4 Consumer-side port registration

Extend the listen + open APIs to accept the resource aux port:

```c
bool rnsdDestListenLinks(int dest_handle,
                         uint16_t link_inbox_port,
                         uint16_t resource_aux_port);

int  rnsdLinkOpen(... ,
                  uint16_t resource_aux_port,   /* 0 = no resource support */
                  ...);
```

`resource_aux_port = 0` means the consumer doesn't accept resources;
rnsd rejects inbound advertisements with no policy work needed. lxmf
will set both ports to live numbers from day one. For Phase E (which
only sends one-packet LXMF messages) the resource port can be unused;
Phase F is the moment it starts mattering.

### 9.5 Acceptance criteria

- `lxmf send 1 <16 KB lorem ipsum>` reaches the echo bot, bot replies
  via Resource, reply lands in our inbox, signature validates.
- `lxmf send 1 <128 KB blob>` round-trips end-to-end.
- A peer that advertises and drops produces a FAILED aux frame within
  mR's resource timeout; buffer is freed; no leak.
- A peer that advertises > `s.lxmf.max_resource_size` is rejected at
  the advertisement (no allocation occurs); peer sees an mR resource
  reject; no aux frame to the consumer.
- 10 sequential 64 KB inbound resources show flat PSRAM HWM across runs.

---

## 10. Phase G — `rnstransport.remote.management` handlers

> **⚠️ Verified 2026-05-16 — interop blocker, same class as §9.0.**
> The `register_request_handler` / `RequestHandler` / dispatch path
> *is* ported (Destination.h:196, Link.cpp:844-888) and the
> `accepts_links` flip + ALLOW_LIST gate would be a small wire-up.
> **But the response serialization is structurally
> upstream-incompatible.** Our ported `Link::handle_request`
> ([Link.cpp:876-881](../../components/microreticulum/src/Link.cpp#L876-L881))
> does `packer.to_array(request_id, response)` where `response` is the
> handler's **`Bytes`** — the MsgPack.h shim encodes it as one opaque
> msgpack `bin`. Upstream `Transport.remote_status_handler` returns a
> **structured `[get_interface_stats() dict, link_count]`** which
> `umsgpack` encodes *inline*, and upstream `rnstatus -R` reads
> `unpacked_response[1]` as that nested structure — not as a bin blob
> ([Transport.py:2802-2848](../../research/Reticulum/RNS/Transport.py#L2802)).
> So real `rnstatus -R` / `rnpath -R` against us cannot parse what our
> port emits. Closing this needs: (a) the MsgPack.h shim extended to
> arbitrary nested string-keyed maps with heterogeneous values, (b)
> `Link::handle_request` patched to splice a pre-encoded structured
> response into the `[request_id, …]` array rather than as `bin`, and
> (c) faithful replication of upstream `get_interface_stats()` /
> `get_path_table()` dict schemas. That is its own mR-porting +
> interop-serialization subproject (smaller than §9.0 but the same
> *kind* of work), not the "Low / 3–5 d wire-up" the §11 table
> assumed. **Phase G is therefore also blocked on an mR-fork decision**
> — bundle it with the §9.0 Resource decision (port mR vs. pull newer
> upstream vs. descope). The link plan's §1 *primary* goal (receive
> from real-world DIRECT peers) is already met by Phase E and needs
> neither F nor G.

Goal: now that Link is in, light up the `/status` and `/path` Request
handlers on our hosted management destination, so peers' `rnstatus -R`
and `rnpath -R` work against us.

We already construct `s_management_dest` and announce it
([rnsd.cpp:1135+](../../main/rnsd.cpp#L1135)). What's missing:

- **`register_request_handler` / `deregister_request_handler` ported
  2026-05-13 (Phase G prereq landed early).** Originally we thought the
  whole dispatch path was missing; on actually reading mR's tree, the
  `RequestHandler` class, the `_request_handlers` map, the
  `request_handlers()` getter, the `ALLOW_NONE/ALL/LIST` enum (in
  `Type.h:214`), the REQUEST/RESPONSE context branches in `Link.cpp`,
  and the msgpack `[request_id, response]` packing at `Link.cpp:881-888`
  were *all already in place*. Only the public `(de)register_*` methods
  on Destination were missing (the file actually had Python-syntax
  sketches as comments at `Destination.cpp:366-378`). Effort was ~1 hour,
  not the 3–5 days the plan originally estimated. Public surface now in
  fork:
  ```cpp
  void register_request_handler(const Bytes& path,
      RequestHandler::response_generator handler,
      Type::Destination::request_policies policy = ALLOW_NONE,
      const std::vector<Bytes>& allowed_list = {});
  bool deregister_request_handler(const Bytes& path);
  ```
  Lookup is lock-safe via a new per-Destination `recursive_mutex`.

  **Three things the Phase G handler implementer needs to know:**
  1. Handler signature is fixed 6-arg in our port
     (`Bytes path, data, request_id, link_id, Identity remote_identity, double requested_at → Bytes response`).
     Upstream supports a 5-arg variant via Python `inspect.signature`;
     we picked 6-arg to match what `Link::handle_request` already calls.
  2. `ALLOW_LIST` requires the peer to have issued `link.identify(...)`
     before the request — otherwise `link.remote_identity()` is empty
     and the request is silently dropped. Upstream `rnstatus -R` /
     `rnpath -R` already identify first; verify any other Phase G
     consumer does too.
  3. `auto_compress` defaults to true in upstream and is on `RequestHandler`
     in our port. Currently a no-op until Phase F (the only path that
     uses it is the Resource-promotion branch at `Link.cpp:892`).

- `remote_status_handler` — collect iface + traffic + path-table sizes
  from our existing state, msgpack-pack, return.
- `remote_path_handler` — given a query hash, look it up in
  `_new_path_table`, msgpack-pack the path entry, return.
- `s.rnsd.remote_management_allowed` storage list — identity hashes
  permitted to invoke handlers. Empty → deny all (current state).

### 10.1 Acceptance criteria

- From a Python rnsd with our identity hash in its `remote_management_allowed`:
  - `rnstatus -R <our-hash>` returns interfaces, RSSI/SNR, traffic counters.
  - `rnpath -R <our-hash> <dest>` returns our known path to `<dest>`.
- Unauthorised requests are rejected via ALLOW_LIST without
  service-level errors.

---

## 10a. Link state in storage + reconnect behaviour

Cross-cutting concern that touches Phases C/D/E. Two reconnect cases
matter; we treat them differently.

### 10a.1 Consumer reconnect to rnsd (in-session)

This is the common case. lxmf crashes / restarts / loses its
`RNSD_PORT_DEST` handle (any reason). Meanwhile rnsd is still running
and still has live Reticulum Links to remote peers. We must not tear
those Links down just because the consumer handle dropped.

Pattern (mirrors how net handles a TCP listener with a transient
consumer):

- The Link's lifetime is rooted at **rnsd**, not at the consumer's ITS
  handle. Consumer ITS handle close ≠ Link teardown. Only an explicit
  `RNSD_DEST_LINK_TEARDOWN` aux frame (or rnsd's own stale-link cull)
  closes the Link.
- When the consumer reconnects (`rnsdDestOpen` for the same identity
  and `rnsdDestListenLinks` for the same target task/port), rnsd walks
  its in-memory link table and, **for every active Link on that
  destination**, opens a fresh ITS connection back to the consumer
  carrying the existing `rnsd_link_incoming_t` payload (link_id, remote
  identity, mtu). The consumer sees the same Link reappear with the
  same identifying link_id and resumes packet I/O.
- For outbound Links the consumer initiated: same idea —
  `rnsdLinkOpen()` looks up "do we already have a Link to this dest
  hash on this identity for this caller?" before constructing a new
  one. The "this caller" gate is via the `RNSD_PORT_DEST` connection
  ownership; outbound Links are scoped under it.

This is the "see that link already exists and just plop right in"
behaviour and it requires no persistence — rnsd just doesn't tie Link
lifetime to consumer-handle lifetime.

### 10a.2 rnsd reboot (across-session)

Harder. rnsd loses all in-memory mR state on reboot. The peer's
Reticulum stack still has the Link in its `_active_links` table and
will continue sending Link packets to us until its inactivity timeout
fires (`Type::Link::STALE_TIME`, ~10 min). Our options:

- **(A) Don't persist. Let stale links die.** Simplest. On reboot we
  refuse incoming Link packets (no matching `link_id` in our empty
  table). Peer eventually times out; messages in flight are lost; LXMF
  retries at the application layer (or the user re-sends). ~10 min
  worst-case visible black hole.
- **(B) Persist enough to resume.** Per-Link state stored in
  `s.rnsd.links.<link_id>.*` (visibility) and
  `secrets.rnsd.links.<link_id>.*` (cipher material). On boot, walk the
  table, reconstruct `RNS::Link` objects, hand to mR's Transport.
  Survives reboot if rnsd comes back within ~10 min and storage
  flushed before the crash.

  Hard parts:
  - **Sequence numbers must be persisted *before* every send** or
    replay attacks become possible. Storage write per Link packet is
    cost-prohibitive.
  - **HKDF-derived link keys + per-direction HMAC chain state**. Same.
  - **mR's Link object isn't designed for re-hydration** — its
    constructor sets up the handshake. Restoring an ACTIVE Link without
    re-handshake means new mR plumbing.

Recommendation: **start with (A)**. The 10-minute black hole is
acceptable for a personal node; rare reboots make the engineering
cost not worth it. Reconsider for always-on transport nodes where
persistent Link state would matter more.

### 10a.3 Storage layout (for in-session visibility — option A)

Even without reboot persistence, we publish per-Link state for the
browser / CLI / rnstatus to render. Same shape as `rnsd.ifaces.*`. This
is *the* status channel — we deliberately don't ship Link state over
ITS aux frames, because vars cross to the browser over WebRTC storage
sync for free, while aux frames don't.

The key is the caller-supplied **tag**, not the link_id. We choose
this because the tag exists from the moment the caller calls
`rnsdLinkOpen`, but the link_id isn't derived until after LR/LRPROOF
— and the caller needs to watch progress through *establishing*. Once
established, we add `link_id` as a field on the same record (and
maintain a reverse index `rnsd.links.byid.<link_id> = <tag>` for any
consumer that only knows the link_id, e.g. on the inbound side).

Per-Link keys (all ephemeral — `rnsd.links.<tag>.*`, no `s.` prefix
since we don't resume across reboot):

```
rnsd.links.<tag>.state            "establishing"|"awaiting_path"|"active"|"closing"|"closed"|"failed"
rnsd.links.<tag>.direction        "out"|"in"
rnsd.links.<tag>.link_id          <32-hex>            (empty until ACTIVE)
rnsd.links.<tag>.local_hash       <32-hex our dest hash>
rnsd.links.<tag>.remote_hash      <32-hex peer dest hash>
rnsd.links.<tag>.remote_identity  <32-hex peer identity hash, if known>
rnsd.links.<tag>.aspect           "lxmf.delivery"
rnsd.links.<tag>.consumer_task    "lxmf"|""
rnsd.links.<tag>.mtu              integer             (empty until ACTIVE)
rnsd.links.<tag>.opened_s         unix seconds
rnsd.links.<tag>.activated_s      unix seconds        (empty until ACTIVE)
rnsd.links.<tag>.last_inbound_s   unix seconds
rnsd.links.<tag>.last_outbound_s  unix seconds
rnsd.links.<tag>.rtt_ms           last measured RTT (after ACTIVE)
rnsd.links.<tag>.tx_packets
rnsd.links.<tag>.rx_packets
rnsd.links.<tag>.last_error       set on failed/closed-with-reason
```

For inbound Links (`direction = "in"`), the tag is rnsd-generated:
`"in.<hex link_id-first-8>"`. The owning consumer (e.g. lxmf) learns
the tag from the `rnsd_link_incoming_t` payload that arrives on the
forwarded ITS connect (see §7.1) and can write that tag into its own
storage so the browser can cross-reference.

Cipher keys / sequence numbers stay in mR's in-memory `Link` object,
not exposed via storage. (They'd be `secrets.*` if persisted, but
we're choosing option A.)

Subscriber pattern is the same as iface state: rnsd writes; browser
panels and CLI `rnsd link list` read. The browser's `links` panel can
just render the table.

### 10a.4 Acceptance criteria (folded into Phases C–E)

- (Phase C/D) After `rnsdDestOpen` accept, rnsd publishes
  `rnsd.links.<id>.*` for any active Link belonging to the new conn.
  Consumer sees existing Links reappear.
- (Phase C/D) `itsDisconnect` from consumer or consumer-task crash does
  **not** call `link.teardown()`. The Link stays in mR until either
  the remote tears it down or rnsd's stale cull fires.
- (Phase D) Explicit `RNSD_DEST_LINK_TEARDOWN` aux frame from consumer
  → rnsd calls `link.teardown()` → mR sends LR_CLOSED → both sides
  remove the Link → `rnsd.links.<id>.*` keys cleared (storageDeleteTree).
- (Phase G) `rnsd link list` CLI walks `rnsd.links.*` and prints a
  table. Browser panel renders the same.

---

## 11. Phase ordering and dependencies

```
A. Build Link.cpp  ────►  B. Link establishment  ───►  C. RNSD_PORT_LINK  ─┐
                                                                            ├──► E. lxmf DIRECT
                                              D. DEST_LINK_LISTEN  ─────────┘     │
                                                                                  │
                                                                          F. Resource  ───► G. management handlers
```

Each phase has its own acceptance criteria; the build stays releasable
between phases.

| Phase | Effort | Risk | Blocks | Status |
|---|---|---|---|---|
| A. MsgPack shim + build flip + accepts_links | 1–2 d | Low | everything | ✅ landed 2026-05-13 |
| B. Link establishment | 1–2 wk | High | C, D | ✅ verified 2026-05-16 (re-flash) |
| C. RNSD_PORT_LINK outbound | 2–3 d | Med | E | ✅ verified 2026-05-16 (10a.1 reconnect deferred; +teardown aux) |
| D. DEST_LINK_LISTEN inbound | 2–3 d | Med | E | ✅ verified 2026-05-16 (+itsClientInit fix, mailbox observability) |
| E. lxmf DIRECT | 1 wk | Med | F | ✅ verified 2026-05-16 (in+out; §8.1 wire-format corrected) |
| F. Resource | 2–3 wk | High | (propagation node, file transfer) | **INBOUND + OUTBOUND ✅ hardware-verified** on `phase-f-resource` vs a real upstream LXMF DIRECT peer. §9.5 inbound 4/5 pass (16 KB / 128 KB-multiseg / drop→FAILED+no-leak / oversize-reject); **64 KB soak RESOLVED — clean 12/12, zero reboots; engine sound, no engine leak**. Soak surfaced two *pre-existing platform* unbounded-growth gaps (lxmf `/state` msg retention; rnsd path-table 24 h-prune → `publishPathTable()` wdt panic) → tracked follow-ups gated on storage→SD, **not** Phase F engine defects. Outbound: device→echo 109-part Resource (windowed advertise + `RESOURCE_HMU` seg=1), proof ok, `stage=sent`, echo received the LXM. 0-warning builds. See §"Current status" + phase-f-resource-port.md §9.5/Results |
| G. management handlers (dispatch ported; **response msgpack upstream-incompatible** — see §10 ⚠️) | port+wire ~1 wk | Med | rnstatus/-R interop | **blocked: extend MsgPack shim + patch handle_request** |

Wall-clock estimate: 6–8 weeks of focused work if Phase B is bug-light,
10–12 weeks if Phase B turns into bug-hunt hell. The two µR prereqs
(Phase F's `ResourceAdvertisement`, Phase G's
`(de)register_request_handler`) landed cheaper than first estimated —
Phase G's prereq was ~1 hour because the dispatch path was already in
tree; only the registration methods were missing.

---

## 12. Risks and open questions

### 12.1 Link MTU vs ITS packet size

ITS handles in packet mode have an upper size per packet. We need to
ensure that limit is ≥ `Type::Link::ENCRYPTED_PACKET_MDU` so Link
packets fit. Current ITS packet-mode limit is plenty (KB-scale), but
worth double-checking when wiring Phase C.

### 12.2 Channel API

Channel rides on Link. We're not exposing Channel via rnsd in this plan.
But Link.cpp may unconditionally instantiate a Channel per Link. If so:
either (a) leave it inert and unused, (b) compile Channel out. (a) is
cheaper; do that and revisit if/when a Channel consumer appears.

### 12.3 Memory pressure

Each active Link carries cipher state, sequence numbers, pending
PacketReceipts, optional Channel buffers. Estimate ~2 KB per Link
minimum on our build. With `RNSD_MAX_LINK_CONNS = 16` (current
proposal) that's 32 KB DRAM/PSRAM dedicated. Acceptable; profile after
Phase B to confirm.

### 12.4 Tear-down semantics

Link teardown is reliable in upstream Reticulum only when both sides
exchange teardown packets. Network drops between mean half-open links
get culled by inactivity timeout (`Type::Link::STALE_TIME`, ~10 min).
This is fine for our use cases but means short-term `s_link_conns`
slot exhaustion is possible if peers keep opening and dropping links.
Mitigation: aggressive cull on remote-side TCP iface disconnect (we
know the link is dead immediately in that case).

### 12.5 µR drift from upstream

Our µR fork already carries several spangap patches (RAII for
`_jobs_locked`, recursive mutex on `_known_destinations`, packet-error
hex dump). Link will surface more — be prepared to either:

- Patch our fork locally, document in [`../spangap-patches.md`](../spangap-patches.md)-
  or-wherever.
- Upstream them where the maintainer accepts (attermann/microReticulum
  has been receptive in the past — confirm before assuming).

### 12.6 Missing µR features that block phases

Verified 2026-05-12 by reading the µR fork; prereq ports landed 2026-05-13:

- ✅ **`Destination::register_request_handler` /
  `deregister_request_handler`** — ported 2026-05-13. The dispatch path,
  RequestHandler class, ALLOW_* enum, and REQUEST-context branches were
  already in tree; only the registration methods were missing. See §10
  above for the post-port public surface and the three implementer
  gotchas.
- **`Link::send()` is via `Packet(link, data).send()`** — not a
  member; the plan's "`link.send(bytes)`" shorthand in §6.2 maps to
  constructing a Packet with `Link&` and sending it
  ([Link.cpp Packet path, Packet.cpp:76-88](../../components/microreticulum/src/Packet.cpp#L76-L88)).
  Trivial to adapt; flag here so the implementer doesn't search for a
  method that isn't there.
- **`Link::Callbacks`** take no userdata — see §6.2 for the
  `&link` slot-walk pattern.

### 12.7 Will Link ever be "off" in a built firmware?

Decision: **no**. Once Phase A lands, Link is in every reticulous
build. We don't carry runtime "use link / don't use link" flags on the
rnsd side (consumers can still choose opportunistic vs direct in lxmf).
Less state, fewer test matrices.

---

## 13. Documentation updates

When Link lands:

- [`docs/rnsd.md`](../rnsd.md) — add a Link section paralleling the
  RNSD_PORT_DEST section. Document the new aux frame, the link-incoming
  connect payload, examples.
- [`docs/lxmf.md`](../lxmf.md) — update the delivery-method section to
  mention DIRECT now works inbound and outbound.
- [`docs/plans/component-plan.md`](component-plan.md) — flip the "no
  Link in v1" note; reference this plan; mark
  the open ITS port surface as filled.
- [`README.md`](../../README.md) — update the feature table.

---

## 14. What we keep doing in the meantime

Until Phase E lands, lxmf receive is still effectively non-functional
against real-world peers. Concrete fallbacks:

- **Loopback tests**: two reticulous devices that talk to each other
  opportunistically work today (and will be the easiest first end-to-end
  test once Link lands).
- **Local Python LXMF "echo" bot** that explicitly uses
  `desired_method=LXMessage.OPPORTUNISTIC` — bypasses the whole
  problem, lets us exercise our wire format and storage layer for real
  while Link is being built.
- **Send-only field deployment** — node can announce + send LXMF + serve
  probes; replies arrive as nothing until a user pairs two reticulous
  devices or runs the local opportunistic bot.

None of this is a substitute for shipping Link; it's just what makes the
intervening weeks productive.

---

## 15. Resolved design decisions (2026-05-13)

All §15 items from the earlier round are now locked. The decision and
the alternatives considered are recorded here so future work doesn't
re-litigate them. Each cross-references the phase section where the
decision lives in normative form.

1. **`RNSD_MAX_LINK_CONNS = 8`.** Single-user node concurrency is 1
   outbound per identity + 2–3 inbounds; 8 gives 5× headroom at ~16 KB.
   Alts: 4 (too tight under bot storms), 16 (defensible only after
   Resource-cadence profiling). Locked in §6.2.

2. **RNSD_PORT_LINK ITS buffers: 4096 / 4096.** ~9 in-flight Link
   packets per direction. Alts: 2048/2048 (no headroom under
   retransmit), 8192/8192 (only if Phase F surface had stayed on the
   same handle, which it doesn't — see item 4). Locked in §6.2.

3. **Timeout defaults: `path_timeout_s = 30`, `orphan_ttl_s = 600`.**
   Storage-tunable from day one. Alts: 15 / 60 — rejected as too
   aggressive on LoRa and as undoing §10a.1's whole point. Locked in
   §6.2.

4. **Phase F Resource surface: shared-memory aux handoff** (option
   (c), supersedes (a) and (b)). rnsd mallocs `total_size` from PSRAM on
   advertisement, fills in place via mR, hands the buffer to the
   consumer in a small ITS aux frame on the consumer-registered
   resource port. Ownership transfers to the consumer on INBOUND_DONE;
   `rnsdResourceRelease(buf)` releases. Outbound borrows the caller's
   buffer for the transfer duration. Alts: (a) second ITS connection
   streaming the bytes — copies through ITS buffers, defeats the lean-
   buffer goal; (b) type-byte multiplex on the Link handle — rejected
   per the no-type-byte design call. Locked in §9.

5. **`LXMF_LINK_INBOX_PORT = 100`, `LXMF_LINK_RESOURCE_AUX_PORT = 101`.**
   Round numbers, well above the 1–10 rnsd port range. Locked in §8.1.

6. **Method-choice storage shape: per-message override + per-identity
   default.** `s.lxmf.id.<n>.default_method` (default `"auto"`) +
   `s.lxmf.id.<n>.msgs.<mid>.method` (default `"auto"`). Resolution at
   send time: msg wins → identity → fallback "auto" (size-based).
   Alts: single global `s.lxmf.default_method` (couples identities,
   reject); per-contact override (additive, defer to follow-up). Locked
   in §8.2.

### 15.1 Other decisions made during this verification round

These weren't in the earlier list but were forced by detailed review.

7. **Pre-active outbound buffer: 1 packet, drop-newer.** Already in
   §6.2. Alts: 4-packet ring (no measurable win for sub-second pre-
   active window), block in `itsSend` (couples consumer latency to
   establishment). Locked in §6.2.

8. **Slot ↔ Link mapping: `&link` walk over `s_link_conns[]`.**
   Callback thunks match by member address (stable for slot lifetime).
   Alts: `std::unordered_map<RNS::Link*, …>` (more code, no perf win
   at 8 slots), patch mR for userdata pointers (drags us off upstream).
   Locked in §6.2 / §12.6.

9. **`rnsd_link_incoming_t` payload shape**: 80 bytes carrying tag,
   link_id, remote_identity_hash, local_dest_hash, mtu. Locked in
   §7.2.

10. **Tag namespace: flat under `rnsd.links.<tag>.*`.** Convention
    over enforcement — consumers self-prefix (lxmf uses
    `"lxmf.id<n>.<mid>"`, inbound uses `"in.<8hex>"`). Alts:
    consumer-scoped path `rnsd.links.<consumer_task>.<tag>.*` (more
    foot-gun-safe, more verbose). Stay flat; revisit if a real
    collision happens. Locked in §10a.3.
