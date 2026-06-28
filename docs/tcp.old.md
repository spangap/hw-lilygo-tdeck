# tcp — TCP transport task

`tcp.cpp/h` — outbound TCP transport for RNS-over-TCP peering. One task
watches `s.tcp.peers.*`, dials each enabled peer via spangap's
`NET_PORT_TCP_DIAL`, registers each connection with rnsd as its own
iface (`tcp/<id>`), and shuttles bytes both ways with HDLC byte
stuffing on the wire.

Core 0, priority 2, 6 KB PSRAM stack. Pinned alongside net + rnsd so
the ITS hops between them stay on-core.

Phase 1 scope: **outbound only.** Inbound server (`s.tcp.server_*`)
lands in Phase 5 — the code path is config-disabled by default per
plan §16; only `s.tcp.server_enable=0` / `s.tcp.server_port=4965`
defaults are installed.

For the persistent / ephemeral / CLI / panel / status-window contracts,
see `component-plan.md` §14.

## ITS topology

Each connected peer holds two simultaneous ITS handles:

```
                ┌───────────────────── tcp task ────────────────────────┐
                │                                                       │
   net  ◄─────► p.net_handle  ←  HDLC bytes  →  p.rnsd_handle  ◄─────► rnsd
   (NET_PORT_                                   (RNSD_PORT_
    TCP_DIAL)                                    REGISTER)
                │                                                       │
                └───────────────────────────────────────────────────────┘
```

Outbound (rnsd→peer): `onRnsdRecv` reads one RNS packet from
`p.rnsd_handle`, HDLC-encodes it, `itsSend`s to `p.net_handle`.

Inbound (peer→rnsd): `onNetRecv` reads up to 1 KB of bytes from
`p.net_handle`, feeds `hdlcConsume` which assembles frames into
`p.rx_pkt`; on each complete frame, `itsSend` one packet to
`p.rnsd_handle`.

`itsClientInit(TCP_MAX_PEERS * 2)` reserves enough client-side
connection slots for both directions across all peers.

## Wire format — HDLC byte stuffing

Same byte stuffing as upstream Python `TCPInterface.py:44`:

```
FLAG = 0x7E    ESC = 0x7D    ESC_MASK = 0x20

emit:    FLAG <escaped(packet_bytes)> FLAG
escape:  0x7D → 0x7D 0x5D
         0x7E → 0x7D 0x5E
decode:  scan for FLAG; inside, on 0x7D, XOR next byte with 0x20
```

`hdlcSend(netHandle, data, len)` writes one framed packet
(`FLAG | escaped | FLAG`) in a single `itsSend`. Buffer is sized
`RNS_MTU * 2 + 4` (worst case: every byte escaped + start/end FLAG).
500 ms send timeout per frame.

`hdlcConsume(peer, in, n)` is a per-peer streaming decoder. State on
the `peer_t`: `rx_pkt[RNS_MTU+8]`, `rx_len`, `rx_in_frame`,
`rx_escaping`. On every FLAG: if we were assembling a non-empty frame,
emit it to rnsd, then reset. Frames exceeding `RNS_MTU` are aborted
(assembly state reset) and one `warn()` is logged.

Inside each HDLC frame is the same RNS packet that goes on UDP or LoRa
— TCP only adds frame delimitation. Different from KISS framing on
serial (which uses 0xC0 / 0xDB).

## Peer table

`std::vector<peer_t> s_peers` is rebuilt by `reloadPeers()` whenever
`storageSubscribeChanges("s.tcp.peers", ...)` fires. Each peer holds:

```cpp
struct peer_t {
    int      id;             // current array index (matches storage)
    int      runtime_id;     // stable across reloads — ITS ref
    bool     enabled;
    char     host[64];
    uint16_t port;
    uint8_t  mode;
    uint32_t retry_min_s, retry_max_s, cur_backoff_s;

    peer_state_t state;      // IDLE | CONNECTING | UP | BACKOFF
    int      net_handle;
    int      rnsd_handle;
    TickType_t next_attempt_tick;

    // HDLC inbound assembly:
    uint8_t  rx_pkt[RNS_MTU + 8];
    size_t   rx_len;
    bool     rx_in_frame, rx_escaping;

    uint64_t bytes_in, bytes_out;
};
```

`runtime_id` is the key distinction: array indices shift when peers
are inserted or removed in storage, but the ITS disconnect callbacks
fire with the original `ref` we passed at `itsConnect` time. So we
hand `runtime_id` to net + rnsd as the ref, and look up by
`peerByRuntimeId(ref)` from the disconnect path. `s_next_runtime_id`
monotonically increases.

`TCP_MAX_PEERS = 16`. Storage with more entries is warned and capped.

### Config reload — preserve state across renames

`reloadPeers()` does a host:port match between old and new arrays
rather than blindly trusting the index:

1. Build `desired_t[]` from `s.tcp.peers.<i>.{host,port}` for each new
   index `i`.
2. For each desired entry, find a matching old peer by exact
   `host == p.host && port == p.port`.
3. If matched: copy the peer over, refresh remaining config fields via
   `loadPeerConfig`. Disable→enable transitions arm a fresh
   `next_attempt_tick = now`; enable→disable transitions trigger
   `disconnectPeer(*, "disabled")` and force `PS_IDLE`.
4. If unmatched: brand new peer (`runtime_id = s_next_runtime_id++`),
   `PS_IDLE`, immediate attempt.
5. Old peers not matched in step 2 are `disconnectPeer(p, "removed")`.

Net effect: editing a single field of `s.tcp.peers.5` (e.g. retry
ceiling) doesn't tear down peer 5's existing connection.

## State machine

```
IDLE ──────── attemptConnect ─────────► CONNECTING
  ▲                                      │
  │                                      │ net dial fails
  │                                      ▼
  └─── reload(disable) ────────── BACKOFF ◄─── disconnectPeer
              ▲                    │
              │                    │ next_attempt_tick reached
              │                    ▼
              ├──── reload ──── attemptConnect
              │                    │
              │                    ▼
              └───────────────── UP
```

`servicePeers()` runs every tick:
- If `!p.enabled` and currently `UP`/`CONNECTING`, `disconnectPeer(p,
  "disabled")`.
- If `p.enabled && (state == IDLE || BACKOFF) && now >= next_attempt_tick`,
  `attemptConnect(p)`.

`attemptConnect(p)`:
1. `state = PS_CONNECTING`, publish.
2. `itsConnect("net", NET_PORT_TCP_DIAL, "host:port", ..., 12s,
   p.runtime_id, onNetRecv, onNetDisconnect)`. Net does DNS + connect
   on its own task; success means the ITS handle is the TCP stream
   from byte zero.
3. On success, `itsConnect("rnsd", RNSD_PORT_TRANSPORT, &reg,
   sizeof(reg), 500ms, p.runtime_id, onRnsdRecv, onRnsdDisconnect)`.
   The `rnsd_transport_t` fills `name="tcp/<id>"`, `mtu=RNS_MTU`,
   `bitrate=0` (variable), `mode=p.mode`, `in=out=1`,
   `fwd=(mode==gateway||full ? 1 : 0)`, `rpt=0`.
4. `state = PS_UP`, `cur_backoff_s = 0`, publish.

`disconnectPeer(p, reason)`:
1. `itsDisconnect` both handles, reset HDLC assembly state.
2. Log if was previously connecting/up.
3. `state = PS_BACKOFF`; backoff doubles: `cur_backoff_s ←
   min(cur_backoff_s * 2, retry_max_s)`, starting at `retry_min_s`
   (default 2 s, ceiling 300 s).
4. `next_attempt_tick = now + cur_backoff_s * 1000 ms`.

## Per-peer config keys

```
s.tcp.peers.<id>.enable     1|0
s.tcp.peers.<id>.host       hostname or IPv4 string
s.tcp.peers.<id>.port       u16, default 4965 (RNS convention)
s.tcp.peers.<id>.mode       "full" | "gateway" (default) | "access_point" |
                            "roaming" | "boundary"
s.tcp.peers.<id>.retry_min  seconds, floor (default 2)
s.tcp.peers.<id>.retry_max  seconds, ceiling (default 300; clamped to ≥ min)
```

Mode strings map to `RNS_IFACE_MODE_*` in `ports.h`. Unknown values
fall back to `RNS_IFACE_MODE_GATEWAY`.

Plan §14.1 also enumerates `name`, `announce.interval` override, and
`ifac_*` per peer — those config keys are not yet read.

## Per-peer ephemeral keys

Published by `publishPeerState(p)` every loop turn:

```
tcp.peers.<id>.up                1|0   (PS_UP only)
tcp.peers.<id>.state             "idle" | "connecting" | "up" | "backoff"
tcp.peers.<id>.stats.tx_bytes    cumulative
tcp.peers.<id>.stats.rx_bytes
```

Plan §14.2 lists `last_error`, `connected_since`, `reconnects`,
`ifac_drops` — pending.

## Main loop

```cpp
for (;;) {
    if (s_configDirty) { s_configDirty = false; reloadPeers(); }
    servicePeers();                    // disable→disconnect, retry due → dial
    for (auto& p : s_peers) publishPeerState(p);
    itsPoll(nextDeadline());           // ITS recv/disconnect/connect-result,
                                       //   config-change notification,
                                       //   or soonest retry deadline
}
```

`onCfgChange` (from `storageSubscribeChanges`) sets `s_configDirty`
and `xTaskNotifyGive(s_task)` so the loop drops out of `itsPoll`
immediately.

`nextDeadline()` returns the smallest of (peer's `next_attempt_tick -
now`) across all enabled backoff/idle peers, capped at 1 s so we
re-publish stats while a connection is healthy and idle.

## CLI — `tcp`

Top-level command (replaces the previous `rnsd tcp`). Sub-verbs:

```
tcp                              list peers + status
tcp start | stop | restart       global gate (s.tcp.enable)
tcp connect <slot>               force-connect peer (clear backoff)
tcp disconnect <slot>            kick peer's connection
tcp peer add <host[:port]> [mode]
                                 add a peer (port=4965, mode=gateway)
tcp peer rm <slot>               remove peer slot
tcp peer enable <slot>           persistently enable
tcp peer disable <slot>          persistently disable
```

Status output:

```
global: enabled
  #   state      host:port                        per-peer  rx/tx
  0   up         rns.noderage.org:4242            enabled   rx=245312 tx=1244
  1   backoff    lab.local:4965                   enabled   rx=0      tx=0
```

### Global gate (`s.tcp.enable`, default 1)

`tcp start` / `tcp stop` flip `s.tcp.enable`. When 0, every peer is
disconnected regardless of per-peer `enable`. Each disconnect closes
the `rnsd_handle` for that peer, which makes rnsd call
`Transport::deregister_interface` — so flipping the gate off and back
on **re-registers** each iface fresh on rnsd's side. Useful for
clearing wedged state without rebooting.

`tcp restart` writes a `tcp.cmd.restart` sentinel; the tcp task tears
down every peer connection (state → IDLE, backoff cleared) and the
servicePeers tick redials all enabled peers. Single-shot, atomic
within tcp's task loop — no CLI-side sleep dance.

### Ad-hoc connect / disconnect

`tcp connect <slot>` and `tcp disconnect <slot>` write
`tcp.cmd.connect` / `tcp.cmd.disconnect` sentinels (slot-number value).
The handlers run on the tcp task, mutate the peer state directly:

- `connect`: if up, disconnect first; then clear backoff, set state to
  IDLE, set `next_attempt_tick = now`. servicePeers redials on the
  next iteration (which the handler nudges via `xTaskNotifyGive`).
- `disconnect`: if up/connecting, `disconnectPeer(p, "user")`. Peer
  goes to BACKOFF; auto-reconnect resumes if peer is still enabled.

The difference between `disconnect` and `peer disable` is intentional:

- `disconnect`: ad-hoc kick, reconnect resumes after backoff.
- `peer disable`: persistent (`s.tcp.peers.<n>.enable = 0`),
  survives reboot, no auto-reconnect.

### Peer config commands

`tcp peer add <host[:port]> [mode]` finds the lowest free slot in
`s.tcp.peers.0..TCP_MAX_PEERS-1` (first slot with no `.host` key),
writes all four config fields atomically in one `storageBegin/End`
block. Existing `s.tcp.peers` subscription fires `reloadPeers` once
on commit. Port defaults to 4965 (RNS convention); mode defaults to
`gateway`.

`tcp peer rm <n>` does `storageDeleteTree("s.tcp.peers.<n>")`.

`tcp peer enable/disable <n>` writes `s.tcp.peers.<n>.enable = 1/0`.

All of these are direct storage writes — the existing
`s.tcp.peers` subscription handles reload. No cmd sentinels needed.

## Inbound — Phase 5 (defaults only today)

`tcpInit` installs `s.tcp.server_enable = 0` and `s.tcp.server_port =
4965` so the panel can render the toggle, but no listen socket is
opened. When this lands:

- One `tcp` task handles both directions (no second task).
- Each accepted connection: a fresh `peer_t`-like entry registered with
  rnsd as `tcp_in/<addr:port>`, framed identically to outbound.
- `s.tcp.max_inbound` (default 8) caps concurrency.

Server-side IFAC (`s.tcp.server_ifac_*`) applies to all accepted
peers; per-peer outbound IFAC (`secrets.tcp.peers.<id>.ifac_netkey`)
applies per-peer.

## Gotchas

- **`runtime_id`, not array index.** ITS connect/disconnect refs
  outlive storage edits. Always look up disconnect peers via
  `peerByRuntimeId(ref)`; `peer_t::id` floats with each reload.
- **Static recv buffer, not `thread_local`.** Same reason as rnsd —
  libgcc lazy TLS init has been seen to corrupt FreeRTOS scheduler
  state at boot, and ITS recv callbacks for a given handle only
  dispatch on the registering task (no concurrency).
- **No `TCP_NODELAY` from this task.** Net owns the socket; if RNS
  starts taking latency hits during bursty announce traffic this is
  the first knob to revisit (set in net's dial path).
- **HDLC frame > `RNS_MTU` aborts assembly.** Genuinely malformed
  remote, or `RNS_MTU` increased on one side only. One `warn()`,
  scanner resyncs on the next FLAG.
- **`tcpip_thread` colocation is intentional.** rnsd, tcp, net all on
  core 0 — keeps ITS hops between them off the cross-core path; lwIP
  recv callbacks (net's side) and rnsd's mR handlers share a core.
