"""Nomad Network form submission — the request-with-map wire contract.

Phase 4 of docs/plans/nomad.md. A form submit sends the request envelope's
3rd element as a msgpack **map** of `field_<name>`/`var_<name>` → value
(not a bin, which is the plain-GET case). This proves that contract
end-to-end: a dict passed to `link.request(data=…)` arrives at the node's
request handler as a dict it can read by those keys.

This is exactly the wire our firmware targets: rnsd's `rnsdLinkRequest(...,
data_packed=true)` splices a caller-built msgpack map as the 3rd element,
and the nomad task builds that map from `field_*`/`var_*` storage keys. The
device-side packer (`mpMapHeader`/`mpStr` in main/nomad.cpp) emits the same
msgpack bytes RNS's umsgpack produces here; this test locks the structure.
"""

from __future__ import annotations

import threading
import time

import RNS

from tests.fixed_keys import FIXED_KEYS
from tests.peers.nomad_peer import APP_NAME, ASPECTS


def _nomad_dest_for(identity_hex: str) -> bytes:
    identity = RNS.Identity.from_bytes(bytes.fromhex(identity_hex))
    dest = RNS.Destination(identity, RNS.Destination.OUT, RNS.Destination.SINGLE,
                           APP_NAME, *ASPECTS)
    return dest.hash


def test_nomad_form_submit(nomad_peer, client_rns):
    priv_hex, _ = FIXED_KEYS[2]
    p = nomad_peer(identity=priv_hex)
    expected = _nomad_dest_for(priv_hex)
    assert p.dest_hash == expected.hex()

    deadline = time.monotonic() + 15.0
    nxt = 0.0
    while time.monotonic() < deadline:
        if RNS.Transport.has_path(expected):
            break
        if time.monotonic() >= nxt:
            RNS.Transport.request_path(expected)
            nxt = time.monotonic() + 1.0
        time.sleep(0.05)
    assert RNS.Transport.has_path(expected), "no path to peer after 15s"

    dest = RNS.Destination(RNS.Identity.recall(expected), RNS.Destination.OUT,
                           RNS.Destination.SINGLE, APP_NAME, *ASPECTS)
    link_up = threading.Event()
    link = RNS.Link(dest)
    link.set_link_established_callback(lambda l: link_up.set())
    assert link_up.wait(timeout=5.0), f"link did not establish (status={link.status})"

    done = threading.Event()
    result: dict = {}

    def on_response(receipt):
        result["response"] = receipt.response
        done.set()

    def on_failed(receipt):
        result["failed"] = True
        done.set()

    # The form map — the device builds the identical structure from its
    # nomad.submit.field_*/var_* storage keys.
    link.request(
        "/page/form.mu",
        data={"field_user": "alice", "var_csrf": "abc123"},
        response_callback=on_response,
        failed_callback=on_failed,
    )

    assert done.wait(timeout=10.0), "no response/failure within 10s"
    assert "failed" not in result, "form request reported failure"
    body = result.get("response", b"")
    assert b"user=" in body and b"alice" in body, f"field not echoed: {body!r}"
    assert b"abc123" in body, f"var not echoed: {body!r}"
