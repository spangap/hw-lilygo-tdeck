# auto — AutoInterface transport task

`auto.cpp/h` — Reticulum's zero-configuration LAN transport. One task
finds every reachable RNS node on the same WiFi link automatically (no
host/port to configure) and carries RNS packets over UDP. It is
wire-compatible with upstream RNS `AutoInterface`
(`RNS/Interfaces/AutoInterface.py`), so a desktop Reticulum on the LAN
peers with the device out of the box.

Core 0, priority 2, 6 KB PSRAM stack — pinned alongside net + rnsd so
the ITS hops between them stay on-core. A second tiny `auto-rx` helper
task (priority 2, 4 KB PSRAM) owns the blocking `recvfrom`.

For the persistent / ephemeral / CLI / panel contracts, see
`component-plan.md` §13.

## Protocol (interoperable with upstream)

All values are the AutoInterface defaults; only the **group name** is
configurable (`s.auto.group`, default `"reticulum"`).

- **Group address.** `group_hash = SHA-256(group_name)`. The IPv6
  multicast discovery address is `ff{flags}{scope}:` followed by six
  hextets of `group_hash[2..13]`; with the default temporary(1) /
  link-local(2) nibbles that is `ff12:0:…`. Every node joins it on the
  active WiFi netif. Different group names → different addresses →
  separate networks.
- **Peering token.** `SHA-256(group_name || link_local_addr_text)`.
  Each node multicasts its token to the group on **port 29716** every
  1.6 s. A node that receives a token from source `S` recomputes
  `SHA-256(group_name || text(S))`; a match adds/refreshes `S` as a
  peer. A token from our own address is just the multicast echo and is
  ignored.
- **Reverse peering.** A node also unicasts its token to known peers'
  **port 29717** every ~5.2 s, so asymmetric multicast still peers both
  directions. We listen there and treat it exactly like a multicast
  token.
- **Data.** An outbound RNS packet is unicast as one UDP datagram to
  every live peer on **port 42671**; an inbound datagram from a known
  peer is forwarded verbatim to rnsd.
- **Peer expiry.** A peer not heard from in 22 s is dropped.

The hash is computed over the *text* form of the IPv6 address, so the
device's `inet_ntop` output must match the peer's. lwIP and CPython
both emit RFC 5952 (lowercase, compressed, no scope suffix), so they
agree.

## Why BSD sockets, not the lwIP raw API

`component-plan.md` §8.1 originally specced a lwIP `udp_pcb` driven from
the owning task. That is only safe with lwIP **core locking**, which is
**off** in this build (`CONFIG_LWIP_TCPIP_CORE_LOCKING` unset). BSD
sockets are task-safe and give us IPv6 multicast join
(`IPV6_ADD_MEMBERSHIP`), zoned link-local sends (`sin6_scope_id`), and
`inet_ntop` address text in one place. rnsd includes no networking
headers regardless.

## Task topology

```
        ┌──────────────── auto task ─────────────────┐
        │  peer table · announce/peer-job timers ·    │
        │  all sendto() · all ITS                     │
        │                                             │
 rnsd ◄─┤ s_rnsdHandle (RNSD_PORT_TRANSPORT, "auto")  │
        │        ▲                    │               │
        │   itsRecv (outbound)   itsSend (inbound)    │
        │        │                    ▼               │
        │   drainOutbound        drainRx ◄── s_rxQueue (PSRAM)
        └────────┼─────────────────────▲──────────────┘
                 │ sendto()             │ xTaskNotifyGive
        discovery / unicast / data sockets
                 ▲                      │
                 └──── auto-rx task: select() + recvfrom() ──┘
```

The auto task's single wait point is `itsPoll(nextDeadline())`. It
wakes on: rnsd ITS traffic (outbound packets, dispatched by itsPoll), a
notify from `auto-rx` (a datagram arrived), a storage-subscription
change (`s.auto.*`), or the computed announce / peer-job deadline. No
polling. `recvfrom` blocks only inside `auto-rx`, which `select()`s the
three UDP sockets, copies each datagram into the PSRAM queue, and
notifies the auto task — the same recv-on-another-context → queue →
notify shape the espnow transport uses.

## Lifecycle

Gated on `netIsUp()` (the auto task calls `netUp()` when enabled so WiFi
comes up). On enable + WiFi up it brings the netif's IPv6 link-local
address up with `esp_netif_create_ip6_linklocal`, retrying on the
deadline loop until DAD assigns it (`auto.state = waiting_addr`), then
opens the sockets, registers with rnsd, and starts announcing. On WiFi
down, group change, or disable it tears the sockets + registration down;
`auto-rx` parks itself once the sockets become `-1`.

## Registration

Registers as iface `auto`, `mtu = 500` (mR base MTU — links stay at 500,
`LINK_MTU_DISCOVERY` off), `bitrate = 10 Mbit/s` (AutoInterface's
guess), mode from `s.auto.mode` (default `gateway`, so `fwd = 1`),
`in = out = 1`. It presents a broadcast medium to rnsd: a single iface
whose outbound packets fan out as unicast datagrams to all peers — the
same shape as LoRa and espnow.

## CLI

```
auto                 # state, group, mcast + our address, peers, tx/rx
auto up | down       # set s.auto.enable
auto peers           # discovered peers + last-heard age
```

## Verifying against desktop Reticulum

Run `rnsd` on a workstation on the same WiFi LAN with a default
AutoInterface (nothing to configure on either side):

```ini
[[Default Interface]]
  type = AutoInterface
  interface_enabled = true
```

`auto peers` on the device should list the workstation within a few
seconds; `rnstatus` on the workstation should list the device; and the
browser Status / Map shows each side's announces as paths under iface
`auto`.
