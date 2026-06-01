# Phase F — Resource engine port (executable spec)

Branch `phase-f-resource`. This is the actionable port plan; the
scoping/decision rationale is in [link.md §9.0 ⚠️](link.md). Reference
(Apache-2.0, re-fetch into gitignored `research/` if absent):

```
research/ratspeak-mr/{Resource.cpp,Resource.h,ResourceData.h}
# raw.githubusercontent.com/ratspeak/microReticulum/master/src/<f>
```

Do **not** copy ratspeak/ratdeck (AGPL). `ratspeak/microReticulum` is
Apache-2.0 — algorithm reference only; adapt to our fork's APIs.

## Current status (2026-05-16) — READ THIS FIRST IF CONTINUING

Branch `phase-f-resource` (Phases A–E are on `main` beneath at
`e2bb0ae`). Build is **0-warning** throughout
(`cd reticulous && umask 000 && /tmp/idfenv.sh --spangap build`).
Steps 1–3 + the Step-4 HW fixes are committed, one commit per concern:

- `7538fdd` engine ported · `1af435b` proof/hash wire corrected to
  **upstream Reticulum** (ratspeak's reference diverges — two random
  hashes, plaintext-domain resource_hash/proof; this was essential for
  real-peer interop).
- `c77006b` Link.cpp `RESOURCE_*` dispatch + 28 pre-existing fork
  warnings zeroed.
- `33db4a9` rnsd shared-memory aux handoff + lxmf port-101
  (`rnsd_link_resource_done_t`, `rnsdLinkSendResource`/`Release`).
- `5b96102` **MTU clamp + windowed pull → 16 KB inbound VERIFIED**.
- `408c574` **multi-segment hashmap (RESOURCE_HMU) → 128 KB inbound
  VERIFIED**.
- (`75d92e7`,`2cdd27e` are Step-4 WIP/record commits superseded by the
  two above.)

### Verified on hardware (§9.5)

- ✅ **16 KB inbound** Resource round-trip and ✅ **128 KB inbound**
  (multi-segment: 283 parts, HMU seg 1/2/3). Full path each time:
  Link → `Resource::accept` → windowed `request_next` (+`RESOURCE_HMU`
  for >~74 parts) → all parts → assemble → `link.decrypt` → strip
  stream-rh → integrity OK → `RESOURCE_PRF` proof → rnsd aux→lxmf:101
  → `onInboundLxm` → persisted (`lxmf msgs` in-count +1); the Python
  sender reports `DELIVERED`. PSRAM returns to baseline, no leak/wdt
  observed (LoRa off).

### Bugs found+fixed during the §9.5 HW run (all committed)

1. ratspeak vs upstream proof/hash domain → `1af435b`.
2. mR Link MTU discovery negotiated 8192, but **every fork transport
   carries only the RNS base MTU** (tcp logs `[tcp] hdlc: frame > 500
   B, dropped`): `LINK_MTU_DISCOVERY=false` (Type.h) + clamp the
   negotiated MTU to `Reticulum::MTU` in `Link::validate_request`
   (responder) and `validate_proof` (initiator). → `5b96102`.
3. windowed pull absent → `request_next` + receive_part trigger →
   `5b96102`.
4. multi-segment hashmap stubbed → ported `request_next`(exhausted
   flag) / `hashmap_update_packet` + Link.cpp `RESOURCE_HMU` wiring
   (`ResourceData` gained `_hash_known/_consec/_hashmap_height/
   _waiting_hmu`, `_map_hashes` sized to `_total_parts`) → `408c574`.

### Remaining work (ordered — for the new context)

1. **Run the 3 remaining §9.5 cases** (engine is believed correct;
   these are acceptance, not new code). A drafted autonomous runner
   exists at **`scripts/phasef-95`** (untracked) — disables LoRa, runs
   *oversize-reject* (`> s.lxmf.max_resource_size`, default 262144),
   *10×64 KB PSRAM-flat soak*, *advertise-then-drop → FAILED + buffer
   freed*; logs `.rns/phasef-95.log`, emits one line per milestone.
   Run it (background/Monitor) and record results. Expected:
   oversize → `onResAdvertised` returns false, no alloc, in-count
   unchanged; soak → `spangap-cli top` PSRAM `min` flat across 10;
   drop → `link[in.*]: resource inbound failed`, PSRAM recovers.
2. **Device-as-SENDER large outbound Resource** (>~34 KB, i.e.
   >`HASHMAP_MAX_LEN`≈74 parts, device→peer). Receiver multi-seg is
   done; the sender side is the remaining engine gap:
   - `Resource::advertise()` currently packs the **full** `_hashmap`
     into the advertisement — for >74 parts that packet won't fit;
     it must window the hashmap to the first `HASHMAP_MAX_LEN`.
   - `Resource::request()` / Link.cpp `RESOURCE_REQ` must handle the
     `HASHMAP_IS_EXHAUSTED` flag from the peer and emit `RESOURCE_HMU`
     segments (mirror upstream `Resource.request`'s HMU branch).
   - Small outbound DIRECT (≤~440 B → 1 Link packet, Phase E) and
     ≤74-part outbound Resource should already work; verify a
     >74-part device→echo send after the fix
     (`s.lxmf.id.0.default_method=direct`, or oversize auto→direct).
3. Then per **Definition of done** below: flip [link.md](link.md) §11
   table (F ✅) + the §"Current status" box, the `MEMORY.md` pointer,
   and memory `project_link_resource_unported` → done. **Phase G stays
   separately blocked** (link.md §10, upstream-incompatible mgmt
   msgpack) — not part of this branch's mandate.

### Environment / gotchas the new context MUST know

- **Test payloads must be incompressible** (`scripts/lxmf-send` uses
  raw `os.urandom`). bz2 is disabled on the build (SF_COMPRESSION
  absent) — compressible payloads are *correctly* dropped
  (`Resource: dropping compressed inbound payload`); that is not a
  bug. Real attachments (already-compressed) are incompressible.
- **LoRa off for all tests** (`spangap-cli "lora down"`). LoRa is
  pre-Phase-3 scaffold; under Resource packet volume its TX path
  starves CPU0 → `task_wdt` (CPU 0: lora). Unrelated to Phase F — do
  **not** chase it.
- `build/flasher.log` **is the live device serial log** (the apparent
  ~2 h lag was UTC; device tz is now `Europe/Berlin` and matches the
  host). The spangap-cli port **persists across flash**, so detect a
  reflash by the post-flash boot banner in flasher.log
  (`Returned from app_main` / `cli: end /state/boot`), *not* by
  spangap-cli becoming unreachable.
- Rig: device `lxmf.delivery` = `e9904da9695d0394ab52d8e1fe25c0a5`;
  dev-loop rnsd via `scripts/rns up`; `scripts/lxmf-send <hash>
  --size N --method direct`; `scripts/lxmf-echo --echo --pad N`.
  Device heap/PSRAM: `spangap-cli top` (`PSRAM free X/Y min Z` —
  track `min` for leak). Device CLI input is line-capped (~440 B) so
  big device-originated sends can't be typed — drive inbound with
  `lxmf-send`; for outbound use forced `direct` + an oversize body.
- Files are **co-edited** (linter/another context commits with the
  same intent) — `git log`/re-read before editing; `git commit` may
  no-op if your change already landed. Never `git add`
  `web-interface/package-lock.json`.

## Operating directive (READ FIRST)

This is a long mechanical port, not a research task. **There is no
remaining architectural decision** — approach A is locked, the
reference is chosen, the API deltas and wiring points are mapped
below. "Large" and "not incrementally hardware-verifiable until
substantially complete" (§9.0) are **not** stop conditions.

- **Do not stop** until all four build/verify steps are done and the
  §9.5 acceptance has been attempted on hardware, OR you hit a genuine
  *unfixable* blocker (a real one — a missing primitive that cannot be
  ported, hardware unavailable, etc.), OR a *new* architectural fork
  that did not exist at spec time. Running low on context is itself a
  reason to keep moving and commit progress, not to pause for
  acknowledgement.
- **Decide small things autonomously.** Naming, helper placement,
  log-level choices, struct layout — just choose and proceed. Do not
  ask "should I continue / proceed / commit per step" — the answer is
  always yes; this document is the standing authorization.
- **Report faithfully, never overclaim.** A step is "done" only when
  it builds with **0 warnings** (`feedback_fix_warnings`); the port is
  "verified" only after the §9.5 hardware soak passes. Mark WIP as WIP.
- Commit each numbered step as its own commit on this branch as you
  finish it — never batch-at-the-end, never wait to be told.

## Decision (locked): approach A

Keep the attermann-shaped `Resource` / `ResourceAdvertisement` /
`Resource::accept` API that our `Link.cpp` already calls. Implement the
bodies with ratspeak's algorithm inside `ResourceData`. Minimal
`Link.cpp` churn. Do **not** adopt ratspeak's `OutboundResource`/
`InboundResource` class split (that would force porting ratspeak's
Link.cpp too — different fork lineage).

## Fork API deltas to adapt (verified 2026-05-16)

- **`Bytes` has no single-byte `append(uint8_t)`** — only
  `append(const Bytes&)`, `append(const Data&)`,
  `append(const uint8_t*,size_t)`, `append(const void*,size_t)`.
  ratspeak's msgpack helpers call `buf.append((uint8_t)b)` constantly.
  Add a local `static inline void ab(Bytes&, uint8_t)` helper
  (`uint8_t v=b; buf.append(&v,1);`) and s/`.append((uint8_t)…)`/`ab(buf,…)`/.
- **`Identity::full_hash(const Bytes&) -> Bytes`** exists; use for
  `get_map_hash` / `compute_resource_hash` / `compute_expected_proof`
  (ratspeak uses the same).
- **`Identity::get_random_hash() -> Bytes`** exists — confirm it yields
  ≥4 bytes; resource random_hash is 4 (`Type::Resource::RANDOM_HASH_SIZE`).
- **`Link::encrypt/decrypt(const Bytes&) -> const Bytes`** exist
  (Link.h:223-224) — ratspeak's `link.encrypt/decrypt` map directly.
- **`Type::Resource::SDU`** = `Packet::MDU`; `MAPHASH_LEN`=4,
  `WINDOW`=4, `RANDOM_HASH_SIZE`=4, `status` enum already matches
  ratspeak's `ResourceStatus` value-for-value (NONE=0…CORRUPT=8).
- **Logging:** our fork uses microReticulum LOG macros (DEBUGF/
  WARNINGF/…), not ratspeak's `WARNING()`. Map accordingly.
- **`ResourceAdvertisement` is already ported** in our `Resource.h`
  (t/d/n/h/r/o/i/l/q/f/m + pack/unpack/is_request/is_response/
  read_request_id/read_size/read_transfer_size). Cross-check its wire
  against ratspeak's `pack()`/`unpack()` (both target Python
  Resource.py — fixmap(11), keys t,d,n,h(32),r(4),o(32),i,l,q(bin|nil),
  f(uint flags),m(bin)). Reuse ours; only fill gaps.

## Engine state — extend `ResourceData`

`ResourceData` currently holds `_link,_hash,_request_id,_data,_status,
_size,_total_size,_callbacks`. Add (covers both roles; role flag):

```
bool                 _outbound;
std::vector<Bytes>   _parts;            // enc chunks (out) / slots (in)
Bytes                _hashmap;          // concat 4-byte map hashes
uint8_t              _resource_hash[32];
uint8_t              _random_hash[4];
uint8_t              _original_hash[32];
Bytes                _expected_proof;   // out
std::vector<std::array<uint8_t,4>> _map_hashes;  // in
size_t _transfer_size,_data_size,_total_parts,_received,_window;
ResourceFlags        _flags;            // add ResourceFlags to Resource.h
double               _started_at, _timeout;
Bytes                _assembled;        // cached for proof/data()
```

## `Resource` body map (ratspeak fn → our member)

| our `Resource` method | ratspeak source | notes |
|---|---|---|
| ctor(data,link,advertise,…) outbound | `OutboundResource::init` + `get_advertisement` | chunk/encrypt(link)/random_hash/resource_hash/hashmap/expected_proof; status ADVERTISED; if `advertise` send RESOURCE_ADV packet (see Link wiring) |
| `static Resource accept(packet, concluded_cb, [progress], [request_id])` | `InboundResource::init` | parse `ResourceAdvertisement::unpack(packet.plaintext())`; alloc slots; status TRANSFERRING; stash cb; emit initial RESOURCE_REQ |
| `receive_part(Bytes)` (new) | `InboundResource::receive_part` | map-hash match → slot; on complete → `assemble` |
| `assemble()` (new, internal) | `InboundResource::assemble` | concat → `link.decrypt` → strip 4B rh → (compressed⇒CORRUPT) → `_data`; status COMPLETE; fire concluded |
| `handle_request(Bytes)->parts` | `OutboundResource::handle_request` | for RESOURCE_REQ |
| `validate_proof(Bytes)` | `OutboundResource::handle_proof` | COMPLETE + fire concluded |
| `generate_proof()` (new) | `InboundResource::generate_proof` | for RESOURCE_PRF send |
| `cancel/get_progress/status/hash/data/request_id/size/total_size` | trivial | back by `_object` |

## Link.cpp wiring (replace //p///z sketches, lines ~1158-1297)

- `RESOURCE_ADV`: `ResourceAdvertisement::unpack(packet.plaintext())`;
  if `is_request` → `Resource::accept(... _request_resource_concluded)`;
  `is_response` → match pending_request, `accept(... request_id=)`;
  else → `accept(... _callbacks.resource_concluded)`; insert into
  `_object->_incoming_resources`; send initial RESOURCE_REQ.
- `RESOURCE` (data part): for each `r : _incoming_resources`
  `r.receive_part(packet.data())`; on complete assemble → concluded →
  send RESOURCE_PRF; remove from set.
- `RESOURCE_REQ`: for each `r : _outgoing_resources`
  `idx=r.handle_request(plaintext)`; send those parts as RESOURCE
  context packets; window/flow per ratspeak (initial: send all
  requested, no retransmit window v1 — note as limitation).
- `RESOURCE_PRF`: for each `r : _outgoing_resources`
  `r.validate_proof(plaintext)`; COMPLETE → concluded → erase.
- `RESOURCE_HMU` / `RESOURCE_ICL`: HMU = hashmap-update (multi-segment;
  v1 single-segment resources don't need it — log+ignore). ICL =
  initiator-cancel → cancel matching incoming resource.
- Outbound send path: `Resource` ctor with `advertise=true` must emit
  the RESOURCE_ADV packet on the link (Packet(link, adv.pack(),
  context=RESOURCE_ADV).send()). Wire where Link.cpp:495
  (`request_resource`) / :891 (`response_resource`) construct Resources,
  and a new `Link` API for lxmf-driven big sends.

## rnsd + lxmf (after engine compiles & Link wired)

- rnsd §9.1: `set_resource_callback`/`_started`/`_concluded` on Link in
  `onIncomingLinkEstablished` + outbound slots. Policy gate
  `s.lxmf.max_resource_size` (262144). `malloc(total_size)` PSRAM,
  fill via engine, hand to consumer via aux on
  `LXMF_LINK_RESOURCE_AUX_PORT(101)` (`rnsd_link_resource_done_t`,
  ports.h). Progress → `rnsd.links.<tag>.resource.<rid>.*`.
- lxmf Phase F: open port 101; on INBOUND_DONE take buf ownership,
  feed `onInboundLxm`, `rnsdResourceRelease`. Outbound: `processReady`
  oversize-direct path sends via Resource instead of one Link packet.

## Build/verify strategy (§9.0: not incrementally HW-verifiable)

1. Engine bodies in Resource.cpp + ResourceData/Resource.h + add
   `ResourceFlags`; keep Link.cpp untouched first → `idf.py --spangap
   build` green (engine compiles standalone, 0 warnings).
2. Wire Link.cpp contexts → build green.
3. rnsd §9.1 + lxmf 101 → build green.
4. HW: `lxmf send <echo> <16KB lorem>` → echo Resource reply → lands +
   sig-verifies; `<128KB>` round-trip; advertise-and-drop → FAILED, no
   leak; 10× 64KB → flat PSRAM HWM (link.md §9.5).

Each numbered step is one reviewable commit on this branch.

## Branch / fork discipline

- All Phase F work happens on reticulous branch **`phase-f-resource`**
  (already created; `git switch phase-f-resource`). Tested A–E is on
  `main` beneath it (`e2bb0ae`).
- **Never commit to `main`.** One commit per numbered step (§"Build/
  verify"), on this branch. Do not push (user controls remotes).
- **spangap stays on `main`** — Phase F is reticulous-only
  (`components/microreticulum/` + `main/lxmf.cpp` + `main/rnsd.cpp`).
  No spangap-core changes are expected; if one becomes necessary that
  is a *new* fork — surface it.
- Don't `git add` `web-interface/package-lock.json` (deliberately
  untracked since `7f87eaf`). `research/` is gitignored by design.

## Build / flash / test environment

The bare `idf.py` wrapper has a silent-exit-1 bug (sourced-probe;
see spangap `docs/development.md`). Reconstruct the working wrapper
(machine-local, not in git):

```sh
# /tmp/idfenv.sh — eval activate -e env, prepend good node, exec real idf.py
ACT="$HOME/.espressif/tools/activate_idf_v5.5.4.sh"
E="$(sh "$ACT" -e 2>/dev/null)"
export IDF_PATH=$(printf '%s\n' "$E"|sed -n 's/^IDF_PATH=//p')
export IDF_PYTHON_ENV_PATH=$(printf '%s\n' "$E"|sed -n 's/^IDF_PYTHON_ENV_PATH=//p')
export IDF_TOOLS_PATH=$(printf '%s\n' "$E"|sed -n 's/^IDF_TOOLS_PATH=//p')
export ESP_ROM_ELF_DIR=$(printf '%s\n' "$E"|sed -n 's/^ESP_ROM_ELF_DIR=//p')
export PATH="$HOME/.nvm/versions/node/v22.22.2/bin:$(printf '%s\n' "$E"|sed -n 's/^PATH=//p'):$(printf '%s\n' "$E"|sed -n 's/^SYSTEM_PATH=//p')"
exec "$IDF_PYTHON_ENV_PATH/bin/python" "$IDF_PATH/tools/idf.py" "$@"
```

- **Build:** `cd reticulous && umask 000 && /tmp/idfenv.sh --spangap
  build` (the `--spangap` flag pulls the sibling spangap-core checkout,
  required — the storage fix lives there). Confirm `EXIT=0` and
  `grep -ciE 'warning:' == 0`.
- **Flash:** host runs a flasher daemon. From the VM:
  `flasher host /Volumes/code/spangap/reticulous` touches
  `build/flashme`; daemon flashes + reboots. Wait until `build/flashme`
  is consumed, then poll `spangap-cli date` for a `2026-*` reply.
- **Device output is NOT tee'd here** (`build/flasher.log` is stale).
  Interrogate live via `spangap-cli "<cmd>"` (default host
  `reticulous.local` = 192.168.2.22, CLI port persists across flash),
  the dev-loop `.rns/rnsd.log`, and storage observability
  (`spangap-cli "show rnsd.links"`, `"show rnsd.mailbox"`,
  `"rnsd links"`, `"lxmf msgs"`).
- **Test rig (host, this machine = 192.168.2.21):**
  `scripts/rns up &` (dev-loop rnsd, :4242 + shared :37428);
  `scripts/lxmf-echo --echo &` (LXMF/Link peer — **DIRECT by
  default**, prints `READY <hash>`; `--opp` forces opportunistic).
  For Resource specifically the echo must reply with a payload >
  ~440 B so it goes out as a Resource (extend lxmf-echo if needed —
  e.g. `--pad N` to fatten the echo body; that is a small autonomous
  change, just do it).
- `scripts/announce-sniff [aspect]` discovers a device-hosted dest
  hash via the shared instance.

## Definition of done

Phase F is **done** when, on `phase-f-resource`:

1. Steps 1–3 each built green (0 warnings) and committed.
2. §9.5 hardware acceptance attempted and results recorded honestly in
   this file + `link.md` (status box) + memory
   (`project_link_resource_unported` → flip to done, or record the
   precise failure):
   - `lxmf send <echo> <~16 KB>` → echo Resource reply lands, signature
     verifies, persists in `lxmf msgs`.
   - `<~128 KB>` round-trips.
   - peer advertises then drops → FAILED aux, buffer freed, no leak.
   - peer advertises > `s.lxmf.max_resource_size` → rejected at
     advertisement, no allocation.
   - 10× 64 KB sequential inbound → flat PSRAM HWM (`top`/heap).
3. Then update `link.md` §11 table (F ✅), `MEMORY.md` pointer, and
   stop — Phase G (its own §10 ⚠️ blocker) is a *separate* decision,
   not part of this branch's mandate.

If §9.5 reveals the port is functionally incomplete, that is normal
for a port this size — keep going (fix and re-verify), do not stop to
report a half-result as if it were terminal.

---

## Results — 2026-05-16 (honest record per Definition-of-done §2)

**Steps 1–3 + step-4 hardening: DONE, committed, build EXIT=0 / 0 warnings.**

| commit | what |
|---|---|
| `7538fdd` | step 1: Resource transfer engine ported (Link.cpp untouched) |
| `1af435b` | step 1 fix: upstream Reticulum proof/hash wire (not ratspeak's) |
| `c77006b` | step 2: Link.cpp RESOURCE_* contexts wired + 28 fork warnings zeroed |
| `33db4a9` | step 3: rnsd resource aux handoff + lxmf port 101 |
| `75d92e7` | step 4 wip: harden all Link RESOURCE_* with try/catch + failure breadcrumb |

**§9.5 hardware acceptance (2026-05-16): 4/5 criteria PASS;
10×64 KB soak leak-verdict INCONCLUSIVE** (16 KB ✅, 128 KB
multiseg ✅, drop→FAILED+no-leak ✅, oversize-reject ✅; 10×64 KB
data-path ✅ but leak inconclusive — see the bullet below: a
flasher-daemon `USB_UART_CHIP_RESET` interrupted iter 6 and reset the
HWM, invalidating the PS0→PS1 comparison). The earlier stall *was*
root-caused (device serial *is* available via `build/flasher.log` —
the device's serial log, just UTC; the prior writeup wrongly judged
it stale) and fixed. Real bugs surfaced and were fixed; the four
clean criteria + the 9 uninterrupted soak transfers all held against
a real upstream Python LXMF DIRECT peer (`scripts/lxmf-send` /
`scripts/lxmf-echo`).

Bugs found + fixed (commits on `phase-f-resource`):

| commit | fix |
|---|---|
| `1af435b` | upstream proof/hash wire (two random hashes, plaintext domain) — not ratspeak's |
| `5b96102` | **Link MTU clamp** — mR negotiated 8192; every fork transport carries only RNS-base 500 (tcp drops HDLC frames >500 B) so the peer chunked Resources too big and the device never saw the parts. `LINK_MTU_DISCOVERY=false` + clamp in `validate_request`/`validate_proof` (link.md §12.1). **Plus windowed request-next** (`accept()` only asked for the first window). |
| `408c574` | **multi-segment hashmap (RESOURCE_HMU)** — advertisement only carries the first `HASHMAP_MAX_LEN`(~74) hashes; the receiver must pull later segments via `RESOURCE_REQ(exhausted)` → `RESOURCE_HMU`. Required for any message >~34 KB. |
| `0a53731` | device-as-sender >74-part: `advertise()` windows the hashmap + sender emits `RESOURCE_HMU`. |

Results (device `e9904da9…lxmf.delivery`, LoRa disabled — see note):

- **16 KB inbound** — PASS. `inbound resource complete 16536B →
  consumer`; `[lxmf] recv mid=… len=16536B title="phasef"`; sender
  `DELIVERED`; `lxmf msgs in` +1.
- **128 KB inbound** — PASS. `n=283 advhashes=74/283` →
  `request_next exhausted=1` → `hashmap_update seg=1/2/3`
  (74→148→222→283) → all parts → sender `DELIVERED 131072B`;
  `lxmf msgs in` +1.
- **advertise-then-drop → FAILED, no leak** — PASS. Mid-transfer
  kill; PSRAM HWM (`top` min-free) flat at 6221524 across the drop +
  soak + ~dozens of debugging-run failures; FAILED-on-timeout
  (`resource inbound failed (status=7)` → CLOSED → slot reclaimed)
  observed repeatedly. By design the inbound path only `malloc`s on
  COMPLETE, so a dropped transfer cannot leak.
- **oversize > `s.lxmf.max_resource_size`** — PASS. 300154 B >
  262144 → `resource 300154B > max 262144B, rejecting` at the
  advertisement; no allocation; `lxmf msgs in` unchanged.
- **64 KB sequential soak — RESOLVED 2026-05-16** (supersedes both the
  earlier "10/10 flat" claim and the later "inconclusive" note). A
  **clean uninterrupted 12×64 KB run** (`.rns/soak-clean.sh`, no
  flasher-daemon reset, reboot-guarded): **12/12 `DELIVERED`, zero
  reboots**. The Resource transfer **engine is sound** — the data path
  is not the problem and there is **no Resource/Link memory leak**
  (completed Resources are erased from the Link; 0 panics in the
  engine).

  The soak did, however, flush out **two pre-existing, *documented*
  platform unbounded-growth defects — neither in the Phase F Resource
  engine** — which together degraded then crashed the node:
  1. **lxmf message retention.** `onInboundLxm` (lxmf.cpp:1498)
     persists the full decoded body into the cJSON-in-RAM storage
     tree, backed by the **256 KB `/state` LittleFS** partition, with
     **no retention/eviction cap**. `s.lxmf.max_resource_size`
     (262144) ≈ the whole `/state` partition, so even one max message
     can fill it. Sustained inbound traffic → `lfs.c: No more free
     space` → storage writes fail.
  2. **rnsd path-table growth.** The path table is an **unbounded
     `BasicHeapStore`, pruned only every 24 h** (rnsd.cpp:3782 comment:
     "Re-add when we implement post-process pruning"; `s.rnsd.path.max`
     /`ttl` were deliberately removed). `publishPathTable()`
     (rnsd.cpp:1395/1407) does an O(N) full decode of it every cycle;
     once it grew under announce/link churn the cycle ran long enough
     to **trip the task watchdog → panic** (backtrace tops at
     `task_wdt_timeout_handling` in `publishPathTable()`).

  Net incident: `/state`-full cascaded (storage writes fail → ITS
  notify-drop storm → CLI ITS port refused connections) and rnsd
  task-watchdog-panicked in `publishPathTable()`. **Recovery:** a
  reflash cleared it (RAM path-table reset; identity + config in
  `/state` preserved; the soak's inbound bodies had never durably
  persisted because their writes failed on the full FS). Device healthy
  post-reflash (PSRAM ~7.66 MB free, no errors).

  Both gaps are **tracked follow-ups, out of the Phase F mandate**,
  and naturally fold into the planned **storage/log → SD** work (large
  Resource-delivered bodies are attachment/blob-scale data that should
  not live in the RAM-cJSON `/state` config tree at all). Minor
  observation also logged: `lxmf` `cmd.delete` is a single sentinel
  key (rapid repeats race/overwrite) and `processDelete`'s
  `storageDeleteTree` success was not reflected by `show` — a separate
  lxmf/storage delete-or-show inconsistency, harmless, unrelated to F.

**Device-outbound Resource: HW-VERIFIED (2026-05-16 16:04, HEAD
`9ac0ea7`).** `spangap-cli "lxmf send <echo> @rand50000"` (the
`3da6928` test affordance — device CLI line buffer is 128 B, so a
>74-part body can't be typed; `@randN` substitutes an N-byte
incompressible body) → 50111 B wire → `SEND_RESOURCE deferred (link
establishing)` → link `ACTIVE` → `sending 50111B resource` →
`_init_outbound`/windowed `advertise` (109 parts, first 74 hashes
advertised) → upstream Python echo `Accepting resource advertisement
… 50.18 KB in 109 parts` → echo pulled the first 74, sent
`RESOURCE_REQ(EXHAUSTED)` → device **`Resource::request HMU seg=1
parts[74..109) of 109`** (the `0a53731` `wants_more_hashmap` branch) →
echo got the rest → `outbound resource delivered (proof ok)` →
`[lxmf] DIRECT resource proven` → msg `stage=sent` → echo logged
`LXM from e9904da9… method=Curve25519` carrying the exact 50000-byte
body. ~3.3 s round trip. The enabling rnsd fix is `9ac0ea7` (defer
the outbound Resource until the link is ACTIVE — pre-fix it was
**dropped** with `SEND_RESOURCE but link not active`, so the engine
was never reached and the msg failed `remote_closed`). The echo's
`--echo` reply then round-tripped **inbound** (83 parts, HMU
`74/83→83/83`), re-confirming inbound multi-segment in the same boot.

Environment / caveats (not Phase F defects):

- **LoRa disabled for the run.** The §9.5 transport is the dev-loop
  TCP. rnsd fans outbound traffic to every registered interface; the
  Resource packet burst hit the pre-Phase-3 LoRa scaffold, whose
  `SX126x::transmit` busy-wait starved CPU 0 → a *task-watchdog
  warning* (no reboot). LoRa is unfinished scaffold; `lora down`
  removes it from the TCP test. (Pre-existing follow-ups, unrelated to
  Resource: link packets shouldn't broadcast to LoRa; the LoRa TX
  busy-wait should feed the watchdog.)
- **Incompressible payloads only.** bz2 is disabled by design
  (SF_COMPRESSION advertised absent). A compressible payload is bz2'd
  by upstream LXMF and **correctly dropped** (`dropping compressed
  inbound payload`). Binary/attachment payloads (the realistic large
  case) work. For compressible *text* from a non-SF-respecting peer to
  succeed, the device's lxmf advertising SF_COMPRESSION-absent so
  compliant peers don't compress is a separate lxmf-layer follow-up
  (not Resource-engine).
