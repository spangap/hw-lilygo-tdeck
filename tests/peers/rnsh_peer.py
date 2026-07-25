"""Wire-compatible reference peer for the firmware rnsh straddle.

The device's rnsh does **not** speak the upstream `rnsh` (acehoss) protocol.
Upstream frames structured messages (VersionInfoMessage, ExecuteCommandMesssage,
StreamDataMessage, …, MSGTYPEs 0..7); the device instead carries **raw terminal
bytes** in a single Channel message type, `MSGTYPE = 0x0100`
(`microreticulum/src/Channel.h: MSGTYPE_RAW`), with session end signalled by
Link/Channel teardown — see rnsh/INTERNALS.md §6. So the stock `rnsh` command
cannot drive or be driven by the device (the MSGTYPEs don't match and the
handshake differs); pointing it at the device fails "in and out". This peer is
the correct reference: a stock `RNS.Channel` plus a `MessageBase` whose
`MSGTYPE = 0x0100` packs/unpacks the raw bytes, matching the device exactly.

Two modes, mirroring the two device halves:

  # test the device SERVER (rnsh "in"): drive the device's rnsh destination.
  python -m peers.rnsh_peer <device_rnsh_dest_hash>
      Establishes a Link+Channel to the device's SINGLE `rnsh` destination and
      relays your terminal: stdin -> 0x0100 messages; received 0x0100 messages
      -> stdout. You will see the device's `Enter admin password:` prompt; type
      the admin password, then use the CLI. Ctrl-C to quit.

  # test the device CLIENT (rnsh "out"): host an rnsh destination for the
  # device's `rnsh <hash>` command to dial.
  python -m peers.rnsh_peer --listen [--identity HEX]
      Hosts `RNS.Destination(IN, SINGLE, "rnsh")`, announces it, and echoes back
      every raw byte it receives (so the operator at the device sees their
      keystrokes). Prints `READY <dest_hash_hex>` once announced.

By default it uses the RNS default config dir (~/.reticulum) — the one
`install-reticulum` wires to the rns.beleth.net:4242 uplink — so the peer and a
device on the same mesh can reach each other. Override with --configdir.

NOTE: this peer is built to the documented wire contract; validate it against a
live device (it cannot be exercised without one).
"""

import argparse
import sys
import threading
import time
from pathlib import Path

import RNS

# The device hosts its rnsh destination as Destination(IN, SINGLE, "rnsh") with
# no further aspects: rnsh.cpp calls rnsdDestOpen("rnsh", ...) and publishes
# rnsdDestinationHash(identity, app_name="rnsh", aspect=""). In RNS terms the
# app_name is "rnsh" and there are zero aspects.
APP_NAME = "rnsh"
ASPECTS = ()


class RawMessage(RNS.MessageBase):
    """Opaque raw-bytes Channel message — the device's only rnsh message type."""
    MSGTYPE = 0x0100

    def __init__(self, data: bytes = b""):
        self.data = data if isinstance(data, (bytes, bytearray)) else bytes(data, "utf-8")

    def pack(self) -> bytes:
        return bytes(self.data)

    def unpack(self, raw):
        self.data = bytes(raw)


def _bind_channel(channel, on_bytes):
    """Register RawMessage on *channel* and route inbound payloads to on_bytes."""
    channel.register_message_type(RawMessage)

    def handler(message):
        if isinstance(message, RawMessage):
            on_bytes(message.data)
            return True
        return False

    channel.add_message_handler(handler)
    return channel


# ── server: host an rnsh destination and echo raw bytes ───────────────────
def run_listen(configdir: Path, identity_hex: str | None) -> int:
    RNS.Reticulum(configdir=str(configdir), loglevel=RNS.LOG_VERBOSE)
    identity = RNS.Identity.from_bytes(bytes.fromhex(identity_hex)) if identity_hex else RNS.Identity()

    destination = RNS.Destination(identity, RNS.Destination.IN, RNS.Destination.SINGLE, APP_NAME, *ASPECTS)
    destination.set_proof_strategy(RNS.Destination.PROVE_ALL)

    def on_link(link: "RNS.Link") -> None:
        RNS.log(f"rnsh_peer: link up {link}", RNS.LOG_DEBUG)
        channel = link.get_channel()

        def echo(data: bytes) -> None:
            # Send the bytes straight back so the far end sees its own typing,
            # proving the device client's outbound relay works end to end.
            channel.send(RawMessage(data))

        _bind_channel(channel, echo)
        channel.send(RawMessage(b"rnsh_peer: connected (raw-echo). Type; Ctrl-C to quit.\r\n"))

    destination.set_link_established_callback(on_link)

    sys.stdout.write(f"READY {destination.hash.hex()}\n")
    sys.stdout.flush()

    def _announce_loop() -> None:
        fast_until = time.monotonic() + 10.0
        while time.monotonic() < fast_until:
            destination.announce()
            time.sleep(1.0)
        while True:
            time.sleep(30.0)
            destination.announce()

    threading.Thread(target=_announce_loop, daemon=True).start()

    try:
        for _ in sys.stdin:
            pass
    except KeyboardInterrupt:
        pass
    return 0


# ── client: dial a device rnsh destination and relay the terminal ─────────
def run_connect(configdir: Path, dest_hex: str) -> int:
    RNS.Reticulum(configdir=str(configdir), loglevel=RNS.LOG_VERBOSE)

    dest_hash = bytes.fromhex(dest_hex)
    dest_len = RNS.Reticulum.TRUNCATED_HASHLENGTH // 8
    if len(dest_hash) != dest_len:
        sys.stderr.write(f"dest_hash must be {dest_len * 2} hex chars\n")
        return 2

    if not RNS.Transport.has_path(dest_hash):
        RNS.log("rnsh_peer: requesting path...", RNS.LOG_INFO)
        RNS.Transport.request_path(dest_hash)
        for _ in range(150):  # ~15 s
            if RNS.Transport.has_path(dest_hash):
                break
            time.sleep(0.1)
    if not RNS.Transport.has_path(dest_hash):
        sys.stderr.write("no path to destination (is the device announcing its rnsh dest?)\n")
        return 1

    identity = RNS.Identity.recall(dest_hash)
    if identity is None:
        sys.stderr.write("could not recall destination identity from the announce\n")
        return 1

    destination = RNS.Destination(identity, RNS.Destination.OUT, RNS.Destination.SINGLE, APP_NAME, *ASPECTS)
    link = RNS.Link(destination)

    def write_out(data: bytes) -> None:
        sys.stdout.buffer.write(data)
        sys.stdout.buffer.flush()

    established = threading.Event()

    def on_established(_lnk) -> None:
        _bind_channel(link.get_channel(), write_out)
        established.set()

    link.set_link_established_callback(on_established)

    if not established.wait(timeout=30.0):
        sys.stderr.write("link establishment timed out\n")
        return 1

    channel = link.get_channel()
    RNS.log("rnsh_peer: channel up — relaying terminal (Ctrl-C to quit)", RNS.LOG_INFO)

    try:
        while link.status == RNS.Link.ACTIVE:
            chunk = sys.stdin.buffer.read(1)
            if not chunk:
                break
            channel.send(RawMessage(chunk))
    except KeyboardInterrupt:
        pass
    finally:
        link.teardown()
    return 0


def main() -> int:
    p = argparse.ArgumentParser(description="Wire-compatible rnsh reference peer (raw 0x0100 Channel)")
    p.add_argument("dest_hash", nargs="?", help="device rnsh destination hash (client mode)")
    p.add_argument("-l", "--listen", action="store_true", help="server mode: host an rnsh destination")
    p.add_argument("--identity", help="hex private identity for --listen (else fresh)")
    p.add_argument("--configdir", default=str(Path.home() / ".reticulum"),
                   help="Reticulum config dir (default ~/.reticulum, the beleth-uplinked instance)")
    args = p.parse_args()

    configdir = Path(args.configdir)
    if args.listen:
        return run_listen(configdir, args.identity)
    if not args.dest_hash:
        p.error("provide a destination hash (client mode) or --listen (server mode)")
    return run_connect(configdir, args.dest_hash)


if __name__ == "__main__":
    sys.exit(main())
