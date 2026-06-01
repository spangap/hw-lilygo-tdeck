"""Nomad Network page-fetch round-trip — the request/response smoke test.

Phase 0 of docs/plans/nomad.md. The device's nomad client (and rnsd's
`rnsdLinkRequest` plumbing under it) rides Reticulum's request/response
layer: open a Link to a `nomadnetwork.node` destination, then
`link.request("/page/index.mu")` and get Micron bytes back. That layer is
implemented in our µR fork but was never driven end-to-end. This proves
it, Python↔Python, against the same nomad_peer the device hits manually
via `rnsd creq <hash> /page/index.mu`.

Mirrors test_link_roundtrip.py: in-process `client_rns` ↔ a peer
subprocess over TCP loopback.
"""

from __future__ import annotations

import threading
import time

import RNS

from tests.fixed_keys import FIXED_KEYS
from tests.peers.nomad_peer import APP_NAME, ASPECTS, INDEX_MICRON


def _nomad_dest_for(identity_hex: str) -> bytes:
    """OUT destination hash for a nomad node pinned to identity_hex."""
    identity = RNS.Identity.from_bytes(bytes.fromhex(identity_hex))
    dest = RNS.Destination(
        identity,
        RNS.Destination.OUT,
        RNS.Destination.SINGLE,
        APP_NAME,
        *ASPECTS,
    )
    return dest.hash


def test_nomad_index_page(nomad_peer, client_rns):
    # A different pinned key than the link round-trip test uses, so the two
    # can't collide if ever run in the same process.
    priv_hex, _identity_hash = FIXED_KEYS[1]

    p = nomad_peer(identity=priv_hex)

    expected_hash = _nomad_dest_for(priv_hex)
    assert p.dest_hash == expected_hash.hex(), (
        f"peer announced {p.dest_hash}, expected {expected_hash.hex()}"
    )

    # Wait for the peer's announce to propagate over the freshly attached
    # TCP interface, re-requesting the path periodically (no "path learned"
    # event exists). The peer announces every 1s for the first 10s.
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

    def on_link_established(link):
        link_up.set()

    link = RNS.Link(dest)
    link.set_link_established_callback(on_link_established)

    assert link_up.wait(timeout=5.0), f"link did not establish (status={link.status})"

    # Issue the page GET. data=None → the request envelope's data element is
    # nil; a static page handler ignores it.
    done = threading.Event()
    result: dict = {}

    def on_response(receipt):
        result["response"] = receipt.response
        done.set()

    def on_failed(receipt):
        result["failed"] = True
        done.set()

    link.request(
        "/page/index.mu",
        data=None,
        response_callback=on_response,
        failed_callback=on_failed,
    )

    assert done.wait(timeout=10.0), "no response/failure within 10s"
    assert "failed" not in result, "request reported failure"
    assert result.get("response") == INDEX_MICRON, (
        f"page mismatch: got {result.get('response')!r}"
    )

    link.teardown()
