"""Standalone Nomad Network node peer (page-serving).

Run as:
    python -m peers.nomad_peer --port 37500 [--bind 127.0.0.1] [--identity HEX] [--configdir DIR]

The page/web counterpart to echo_peer.py. Brings up a Reticulum stack,
listens on TCP, registers a SINGLE destination on `nomadnetwork/node`
(exactly the app/aspect real Nomad Network nodes use, Node.py:18), serves
a tiny Micron page at `/page/index.mu` via a request handler, and
announces its display name as `app_data` (Node.py:217-221) so the
announce-drift list is human-readable with zero fetches. Prints

    READY <dest_hash_hex>

to stdout (flushed) once announce has been emitted, so a pytest fixture
can wait for it synchronously.

Two uses:
  - tests/test_nomad_page.py drives an in-process RNS client through
    `link.request("/page/index.mu")` against this peer — proves the
    request/response round-trip the device's nomad client rides on,
    Python↔Python (the device-in-the-loop check is `rnsd creq`).
  - Manual device interop: `rnsd creq <dest_hash> /page/index.mu` from the
    device CLI should print the same Micron below.

Designed to be parameterized: pinned `--identity` keeps the destination
hash stable across runs; `--bind 0.0.0.0` lets the firmware dial in over
LAN. GPL-3.0 NomadNet is the wire-contract reference only — this is an
independent minimal node, not a copy.
"""

import argparse
import sys
import threading
import time
from pathlib import Path

import RNS

APP_NAME = "nomadnetwork"
ASPECTS = ("node",)

# Display name announced as app_data — what the drift feed shows.
DISPLAY_NAME = "reticulous test node"

# The tiny Micron blob served at /page/index.mu. Kept small on purpose so
# the request rides a single Link packet (Phase 0 exercises the packet
# path; the Resource path for larger pages is proven separately). Backtick
# is Micron's control char (`! bold, `_ underline, `[label`target] link).
INDEX_MICRON = (
    ">Reticulous test node\n"
    "\n"
    "Welcome to the `!reticulous`! Nomad Network test node.\n"
    "\n"
    "This page is served by tests/peers/nomad_peer.py and is the\n"
    "Phase 0 request/response round-trip fixture.\n"
    "\n"
    "`[About this node`:/page/about.mu]\n"
).encode("utf-8")


def _as_text(v) -> str:
    return v.decode("utf-8", "replace") if isinstance(v, (bytes, bytearray)) else str(v)


def render_form_result(data) -> bytes:
    """A dynamic page that echoes submitted field_/var_ values. `data` is the
    request envelope's 3rd element: a dict (form submission) or empty/None
    (plain GET). This is the exact wire contract the device's form submit
    targets — a msgpack map of `field_<name>`/`var_<name>` → value."""
    fields = {}
    if isinstance(data, dict):
        for k, v in data.items():
            fields[_as_text(k)] = _as_text(v)
    user = fields.get("field_user", "")
    csrf = fields.get("var_csrf", "")
    return (
        ">Form result\n\n"
        f"user=`!{user}`!\n"
        f"csrf=`!{csrf}`!\n"
    ).encode("utf-8")


def _write_config(configdir: Path, listen_ip: str, listen_port: int) -> None:
    """Materialize a peer config from the shared template into *configdir*."""
    template_path = Path(__file__).parent / "peer-config.template"
    body = template_path.read_text().format(listen_ip=listen_ip, listen_port=listen_port)
    configdir.mkdir(parents=True, exist_ok=True)
    (configdir / "config").write_text(body)


def main() -> int:
    parser = argparse.ArgumentParser(description="Reticulum Nomad Network node peer")
    parser.add_argument("--port", type=int, required=True,
                        help="TCPServerInterface listen port")
    parser.add_argument("--bind", default="127.0.0.1",
                        help="TCPServerInterface listen IP (default 127.0.0.1)")
    parser.add_argument("--identity",
                        help="hex-encoded private identity bytes (else fresh)")
    parser.add_argument("--configdir", required=True,
                        help="Reticulum config dir (will be created)")
    args = parser.parse_args()

    configdir = Path(args.configdir)
    _write_config(configdir, args.bind, args.port)

    RNS.Reticulum(configdir=str(configdir), loglevel=RNS.LOG_VERBOSE)

    if args.identity:
        identity = RNS.Identity.from_bytes(bytes.fromhex(args.identity))
    else:
        identity = RNS.Identity()

    destination = RNS.Destination(
        identity,
        RNS.Destination.IN,
        RNS.Destination.SINGLE,
        APP_NAME,
        *ASPECTS,
    )

    # Request handler — the 6-arg response-generator signature NomadNet
    # uses (path, data, request_id, link_id, remote_identity, requested_at).
    # A static page ignores `data`, so a plain GET (data=None) interops.
    def serve_index(path, data, request_id, link_id, remote_identity, requested_at):
        RNS.log(f"nomad_peer: serving {path} ({len(INDEX_MICRON)} B)", RNS.LOG_DEBUG)
        return INDEX_MICRON

    destination.register_request_handler(
        "/page/index.mu",
        response_generator=serve_index,
        allow=RNS.Destination.ALLOW_ALL,
    )

    def serve_form(path, data, request_id, link_id, remote_identity, requested_at):
        RNS.log(f"nomad_peer: form {path} data={data!r}", RNS.LOG_DEBUG)
        return render_form_result(data)

    destination.register_request_handler(
        "/page/form.mu",
        response_generator=serve_form,
        allow=RNS.Destination.ALLOW_ALL,
    )

    # READY sentinel — fixture waits for this on stdout before asserting.
    sys.stdout.write(f"READY {destination.hash.hex()}\n")
    sys.stdout.flush()

    # Tight startup-cadence announce so a freshly attached TCPClient sees an
    # announce within ~1 s; app_data carries the display name. Back off
    # after the warm-up window.
    def _reannounce_loop() -> None:
        deadline_fast = time.monotonic() + 10.0
        while time.monotonic() < deadline_fast:
            destination.announce(app_data=DISPLAY_NAME.encode("utf-8"))
            time.sleep(1.0)
        while True:
            time.sleep(30.0)
            destination.announce(app_data=DISPLAY_NAME.encode("utf-8"))

    threading.Thread(target=_reannounce_loop, daemon=True).start()

    # Block until stdin closes (fixture teardown) or EOF.
    try:
        for _ in sys.stdin:
            pass
    except KeyboardInterrupt:
        pass

    return 0


if __name__ == "__main__":
    sys.exit(main())
