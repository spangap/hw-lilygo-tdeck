"""Two-peer Link round-trip — the substrate smoke test.

Phase 3 of docs/plans/test-harness.md. Proves that:
  - `make harness` installed RNS correctly;
  - a Python echo peer can come up as a subprocess and be discovered
    over TCP;
  - the test-process Reticulum can request a path, open a Link, and
    receive an echoed packet.
"""

from __future__ import annotations

import threading
import time

import pytest
import RNS

from tests.fixed_keys import FIXED_KEYS


APP_NAME = "spangap"
ASPECTS = ("test", "echo")


def _destination_for(identity_hex: str) -> bytes:
    """Compute the OUT destination hash for a peer pinned to identity_hex."""
    identity = RNS.Identity.from_bytes(bytes.fromhex(identity_hex))
    dest = RNS.Destination(
        identity,
        RNS.Destination.OUT,
        RNS.Destination.SINGLE,
        APP_NAME,
        *ASPECTS,
    )
    return dest.hash


def test_link_open_send_receive(peer, client_rns):
    priv_hex, _identity_hash = FIXED_KEYS[0]

    p = peer(identity=priv_hex)

    expected_hash = _destination_for(priv_hex)
    assert p.dest_hash == expected_hash.hex(), (
        f"peer announced {p.dest_hash}, expected {expected_hash.hex()}"
    )

    # Wait for the peer's startup-cadence announce to propagate over the
    # freshly attached TCP interface. RNS doesn't expose a "path learned"
    # event; we poll has_path() and re-request periodically. With the
    # echo peer announcing every 1s for the first 10s, 15s is generous.
    path_deadline = time.monotonic() + 15.0
    next_request_at = 0.0
    while time.monotonic() < path_deadline:
        if RNS.Transport.has_path(expected_hash):
            break
        if time.monotonic() >= next_request_at:
            RNS.Transport.request_path(expected_hash)
            next_request_at = time.monotonic() + 1.0
        time.sleep(0.05)
    assert RNS.Transport.has_path(expected_hash), "no path to peer after 15s"

    identity = RNS.Identity.recall(expected_hash)
    assert identity is not None, "path was learned but identity not recalled"
    dest = RNS.Destination(
        identity,
        RNS.Destination.OUT,
        RNS.Destination.SINGLE,
        APP_NAME,
        *ASPECTS,
    )

    link_up = threading.Event()
    link_closed = threading.Event()
    received: list[bytes] = []

    def on_link_established(link):
        link_up.set()

    def on_link_closed(link):
        link_closed.set()

    def on_packet(message, packet):
        received.append(bytes(message))

    link = RNS.Link(dest)
    link.set_link_established_callback(on_link_established)
    link.set_link_closed_callback(on_link_closed)
    link.set_packet_callback(on_packet)

    assert link_up.wait(timeout=5.0), f"link did not establish (status={link.status})"

    payload = b"ping-from-test"
    RNS.Packet(link, payload).send()

    deadline = time.monotonic() + 5.0
    while time.monotonic() < deadline and not received:
        time.sleep(0.02)

    assert received, "no echo received within 5s"
    assert received[0] == payload, f"echo mismatch: {received[0]!r} != {payload!r}"

    link.teardown()
    link_closed.wait(timeout=2.0)
