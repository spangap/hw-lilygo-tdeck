"""Standalone Reticulum echo peer.

Run as:
    python -m peers.echo_peer --port 37500 [--bind 127.0.0.1] [--identity HEX] [--configdir DIR]

Brings up a Reticulum stack, listens on TCP, registers a SINGLE destination
`diptych/test/echo`, and echoes any packet received on a Link back to the
sender via `RNS.Packet(link, data).send()`. Prints

    READY <dest_hash_hex>

to stdout (flushed) once announce has been emitted, so a pytest fixture
can synchronously wait for it rather than racing on a sleep. Blocks
until stdin closes (the parent fixture closes stdin to signal teardown)
or SIGTERM arrives.

Designed to be parameterized: pinned `--identity` keeps destination
hashes stable across runs (grep-able logs); `--bind 0.0.0.0` is what
Phase 4 will pass so the firmware can dial in over LAN.
"""

import argparse
import os
import sys
import threading
import time
from pathlib import Path

import RNS

APP_NAME = "diptych"
ASPECTS = ("test", "echo")


def _write_config(configdir: Path, listen_ip: str, listen_port: int) -> None:
    """Materialize a peer config from the template into *configdir*.

    The template lives next to this module, so the same file backs both
    the pytest-spawned peer and any manual invocation."""
    template_path = Path(__file__).parent / "peer-config.template"
    body = template_path.read_text().format(listen_ip=listen_ip, listen_port=listen_port)
    configdir.mkdir(parents=True, exist_ok=True)
    (configdir / "config").write_text(body)


def main() -> int:
    parser = argparse.ArgumentParser(description="Reticulum echo peer")
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

    # Bring up Reticulum. logdest=None means RNS logs to stdout, which the
    # fixture captures alongside the READY sentinel.
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
    destination.set_proof_strategy(RNS.Destination.PROVE_ALL)

    def on_link_established(link: "RNS.Link") -> None:
        RNS.log(f"echo_peer: link up {link}", RNS.LOG_DEBUG)

        def on_packet(message: bytes, packet: "RNS.Packet") -> None:
            RNS.log(f"echo_peer: echoing {len(message)} bytes", RNS.LOG_DEBUG)
            RNS.Packet(link, message).send()

        link.set_packet_callback(on_packet)

    destination.set_link_established_callback(on_link_established)

    # READY sentinel — fixture waits for this line on stdout before
    # asserting anything about the peer. Improves on upstream's `sleep(2)`.
    sys.stdout.write(f"READY {destination.hash.hex()}\n")
    sys.stdout.flush()

    # Re-announce on a tight cadence at startup so a freshly attached
    # TCPClient sees an announce within ~1s without us having to hook
    # into Reticulum's interface-up event. Back off after the warm-up
    # window — Transport.request_path() will trigger an out-of-band
    # response anyway once paths settle.
    def _reannounce_loop() -> None:
        deadline_fast = time.monotonic() + 10.0
        while time.monotonic() < deadline_fast:
            destination.announce()
            time.sleep(1.0)
        while True:
            time.sleep(30.0)
            destination.announce()

    threading.Thread(target=_reannounce_loop, daemon=True).start()

    # Block until stdin closes (fixture teardown) or EOF arrives via SIGPIPE.
    try:
        for _ in sys.stdin:
            pass
    except KeyboardInterrupt:
        pass

    return 0


if __name__ == "__main__":
    sys.exit(main())
