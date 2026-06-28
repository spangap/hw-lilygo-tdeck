# testing — Reticulum test harness

A Python companion environment that doubles as **(a)** an interactive
debugging peer for the device firmware (long-lived `rnsd` you can poke
at across many firmware reflashes) and **(b)** the substrate for an
automated test suite (pytest with subprocess-per-peer). Same upstream
code (Mark Qvist's reference `Reticulum` + `LXMF`), two lifetimes;
neither must break the other.

Scope: reticulous-specific. Not a general spangap test harness —
testing ITS / storage / webrtc-task in isolation is out of scope.

## Quick start

```bash
cd reticulous/

make harness                              # one-time: clone + install
scripts/rns up &                          # background dev-loop rnsd
scripts/rns status                        # what does the daemon see?
research/.venv/bin/pytest tests/          # run the test suite
scripts/rns down                          # stop dev-loop rnsd
```

`make harness-clean` wipes `research/` so the next `make harness` starts
from scratch. `make rns-reset` separately wipes `.rns/` (dev-loop state).

## Two lifetimes

| | Dev-loop rnsd | Test runtime |
|---|---|---|
| Process | one, long-lived | spawned per-test, torn down after |
| Config dir | `reticulous/.rns/` (persistent) | `tmp_path` (gitignored, ephemeral) |
| Interfaces | TCPServer on `:4242` + shared-instance on `:37428` | TCPServer on the test's chosen port |
| Identity | survives reflashes | fresh or pinned via `fixed_keys` |
| Purpose | warm peer the firmware dials during interactive dev | hermetic per-test peers + asserts |

Tests and the dev-loop rnsd never touch each other: different ports,
different config dirs, different lifetimes.

## What `make harness` does

```
reticulous/research/        ← gitignored, regenerated on demand
├── Reticulum/              git clone of markqvist/Reticulum
├── LXMF/                   git clone of markqvist/LXMF
├── .venv/                  python -m venv + editable installs
└── INSTALLED.txt           pinned commit SHAs at install time
```

The pinned SHAs in `INSTALLED.txt` are what we currently develop against
— if upstream changes shape, that file shows what we last saw working.
`make harness-clean && make harness` re-resolves to current `main`.

`research/.venv/bin/` carries the standard RNS CLIs (`rnsd`,
`rnstatus`, `rnpath`, `rnprobe`, `rncp`, `rnsh`, …) plus `pytest`. They
all work the same as system installs; the venv just keeps the install
isolated.

## Dev-loop rnsd

`scripts/rns up` boots `rnsd` against `reticulous/.rns/`, logging to
`reticulous/.rns/rnsd.log`. The config (generated from
[scripts/rnsd.config.template](../scripts/rnsd.config.template) on
first boot) advertises two interfaces:

- `TCPServerInterface` on `0.0.0.0:4242` — what the firmware dials.
- The shared-instance socket on `:37428` — what `rn{status,path,probe,…}`
  attach to so they see the live state of the running daemon rather
  than spinning up a fresh stack.

`scripts/rns <verb>` is a thin wrapper:

| Verb | Action |
|---|---|
| `up` | exec `scripts/rnsd-up` (foreground; caller is expected to background) |
| `down` | `pkill -f "rnsd .*--config $RNS_DIR"` (matches *our* rnsd only) |
| `log [-f]` | cat/follow `.rns/rnsd.log` |
| anything else | exec `research/.venv/bin/rn<verb> --config $RNS_DIR …` |

So `scripts/rns status`, `scripts/rns path -t <hash>`, `scripts/rns
probe spangap test`, etc. all reach the live daemon.

The dev-loop rnsd has `enable_transport = Yes`, so it can act as
transit if you start more peers around it. Forwarding scenarios fall
out for free — start a second `python -m peers.echo_peer` with a
`TCPClientInterface` pointed at `127.0.0.1:4242` and the dev-loop rnsd
becomes a transit hub. We don't bundle that today; the dev-loop is
single-node because its value is *persistent identity + path cache
across firmware reflashes*, which is a single-process concept.

### Operating loop while developing firmware

1. `scripts/rns up &` once, leave running.
2. `flasher -d` cycles the device; firmware dials `:4242`.
3. Watch `/tmp/flasher.log` (firmware) and `.rns/rnsd.log` (host) in
   parallel.
4. Use `scripts/rns status` / `path` / `probe` to interrogate the
   daemon. No restarts on either side unless the firmware crashes or
   interface config changes.

## Test substrate

```
tests/
├── conftest.py                fixtures: peer(), client_rns
├── peers/
│   ├── echo_peer.py           standalone Reticulum echo peer
│   └── peer-config.template   parameterized by {listen_ip, listen_port}
├── fixed_keys.py              pinned (priv_hex, identity_hash) tuples
├── mint_fixed_keys.py         regenerate fixed_keys.py
└── test_link_roundtrip.py     two-peer Link round-trip (smoke test)
```

### Fixtures

- **`peer(port=37500, *, identity=None, bind="127.0.0.1", configdir=None)`** —
  function-scoped factory. Spawns
  [tests/peers/echo_peer.py](../tests/peers/echo_peer.py) as a subprocess
  with a fresh `tmp_path` configdir, waits for the peer to print
  `READY <dest_hash_hex>` on stdout (replaces upstream's flaky
  `sleep(2)`), and yields a handle with `.dest_hash` / `.port` /
  `.terminate()`. Designed to be called **N times in a single test** —
  multi-peer topologies don't change the fixture, only the test.
- **`client_rns`** — session-scoped `RNS.Reticulum` *in the test
  process*. `RNS.Transport` carries module-global state, so we only
  init once per session. Its config statically declares a
  `TCPClientInterface` to the default test peer port (37500). Tests
  that need that in-process client must spawn their peer on 37500;
  tests that don't need it (pure subprocess topologies) can use any
  port.

### Pinned identities

`tests/fixed_keys.py` carries `(priv_hex, identity_hash_hex)` tuples
shaped like upstream Reticulum's `fixed_keys` list in
`tests/link.py`. Loading an identity from one of these tuples gives
the peer a stable destination hash across runs, which keeps
`rnsd.log` grep-able and lets tests `assert peer.dest_hash ==
<expected>` cheaply. Regenerate with
`research/.venv/bin/python tests/mint_fixed_keys.py N > tests/fixed_keys.py`
when you need more keys; existing assertions hard-code hashes from
the committed file, so don't re-mint casually.

## Gotchas and design points

### `TCPClientInterface` constructed bare is broken

Reticulum interfaces aren't fully usable straight out of their
constructor — `RNS.Reticulum.__init__` performs a long list of
post-construction attribute setup (`announce_rate_target`, ifac fields,
ingress/egress control caps, …) before calling `final_init`. Manually
building `TCPClientInterface(RNS.Transport, {…})` and appending it to
`RNS.Transport.interfaces` crashes the announce path with
`AttributeError: 'TCPClientInterface' object has no attribute
'announce_rate_target'` the first time an announce lands.

**Working path:** put the interface in the config file *before*
`RNS.Reticulum(configdir=…)`. This is why `client_rns` is session-scoped
with a static `TCPClientInterface` to a fixed port — and why the
default `peer()` binds to that same port. Tests that want a different
port simply don't depend on `client_rns`.

### `LoopbackInterface` doesn't exist

Don't put `type = LoopbackInterface` in any config — Reticulum logs an
error and ignores it. `LoopbackInterface.py` isn't in
`RNS/Interfaces/`. If you want intra-process loopback for shared-instance
fan-out, that happens automatically via `share_instance = Yes`.

### Subprocess-per-peer, not threads

`RNS.Reticulum` is a process-level singleton — `RNS.Transport` has
module-global state. Two peers in one Python process produce silent
crosstalk and aren't supported by upstream. Upstream's own tests
([tests/link.py](../research/Reticulum/tests/link.py)) solve this with
`subprocess.Popen`; we do the same. The test *process itself* counts as
one peer (via `client_rns`); every additional peer is a subprocess.

### Test interference with the dev-loop rnsd

Tests use ports 37500+ (peers) and ephemeral connect ports. Dev-loop
rnsd uses 4242 (TCPServer) and 37428 (shared instance). Different
config dirs. They don't see each other.

If you do find a test failing because the dev-loop rnsd's path cache
remembers something stale, that's an actual bug in the harness's
isolation — not expected.

## What this doesn't cover yet

The substrate is N-peer-capable but only one smoke test ships today.
These are cheap to add once we have a reason:

- **Firmware as third party.** Mark a test `@pytest.mark.firmware`,
  treat the firmware's destination hash as a discovered-via-announce
  handle rather than spawned-and-known, dial the firmware's
  `TCPClientInterface` upstream to a Python peer the test brings up.
  Default `pytest` skips these; `pytest -m firmware` runs them.
- **Forwarding (three-peer transit).** Three `peer()` calls in one
  test; the middle peer has `enable_transport = Yes` (already the
  default in our peer template). Same Link test body, different
  fixture parameters.
- **Multi-hop Link, announce-propagation timing, path failure /
  recovery, LXMF over Link with transit, resource transfer.** All fall
  out of N-peer once the relevant peer-side handlers are written.

## Anti-patterns

- **Tests depending on the dev-loop rnsd being up.** They don't; tests
  spin up their own peers. The dev-loop is interactive-only.
- **Tests sharing config dirs.** They don't; each test gets a fresh
  `tmp_path` configdir.
- **Committing anything under `research/`.** Never. Gitignored; always
  regenerated by `make harness`.
- **`make harness-clean` touching `.rns/` or anywhere outside
  `research/`.** It doesn't. Dev-loop state survives a clean.
- **The dev-loop rnsd using `~/.reticulum`.** It doesn't —
  `--config reticulous/.rns` keeps it isolated from whatever Reticulum
  activity the operator has elsewhere on the machine.
