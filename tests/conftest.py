"""Pytest fixtures for the reticulous test harness.

Shape:
  - `peer(port, *, identity=None, bind="127.0.0.1")` spawns a fresh
    echo_peer subprocess per test, waits for its READY sentinel, and
    yields a handle exposing the destination hash + a terminate() method.
    Function-scoped: each test gets a clean peer.
  - `client_rns` is session-scoped because `RNS.Reticulum` carries
    process-global state via `RNS.Transport`. Its config has a static
    TCPClientInterface pointed at `DEFAULT_TEST_PEER_PORT`; the per-test
    peer must bind to that port. Bare `TCPClientInterface(...)` construction
    skips the post-init attribute setup `Reticulum.__init__` performs, so
    static config is the working path.

See docs/plans/test-harness.md §7 for the design context.
"""

from __future__ import annotations

import dataclasses
import socket
import subprocess
import sys
import threading
import time
from pathlib import Path
from typing import Iterator, Optional

import pytest


REPO = Path(__file__).resolve().parents[1]
VENV_PY = REPO / "research" / ".venv" / "bin" / "python"
ECHO_PEER = REPO / "tests" / "peers" / "echo_peer.py"
READY_TIMEOUT_SEC = 15.0

# Fixed loopback port the session-scoped `client_rns` dials into. The
# default `peer()` fixture binds to this port so the in-process client
# Reticulum (which can only be initialised once per process) has a stable
# target baked into its config. Tests that need a different port spawn
# the peer with `port=` explicitly and don't use client_rns.
DEFAULT_TEST_PEER_PORT = 37500


@dataclasses.dataclass
class PeerHandle:
    """Handle to a running echo_peer subprocess."""
    dest_hash: str       # hex
    port: int
    proc: subprocess.Popen
    configdir: Path
    _stderr_drain: threading.Thread

    def terminate(self) -> None:
        if self.proc.poll() is None:
            try:
                self.proc.stdin.close()
            except Exception:
                pass
            try:
                self.proc.wait(timeout=3)
            except subprocess.TimeoutExpired:
                self.proc.terminate()
                try:
                    self.proc.wait(timeout=2)
                except subprocess.TimeoutExpired:
                    self.proc.kill()
                    self.proc.wait()


def _free_port() -> int:
    """Pick a free TCP port. There's a TOCTOU window between this and
    the subprocess binding it; for serial test runs that's a non-issue,
    and we'd rather have a small window than a brittle fixed-port list."""
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def _drain(stream, label: str) -> threading.Thread:
    """Mirror a peer's stderr to ours so pytest -s shows it inline.
    Without this the subprocess pipe can fill and block the peer."""
    def _run():
        for line in iter(stream.readline, b""):
            sys.stderr.write(f"[{label}] {line.decode(errors='replace')}")
        stream.close()
    t = threading.Thread(target=_run, daemon=True)
    t.start()
    return t


@pytest.fixture
def peer(tmp_path: Path) -> Iterator:
    """Factory fixture: `p = peer()` spawns a fresh echo peer.

    Defaults: random free port, fresh identity, loopback bind. Callers can
    override `port`, `identity` (hex private bytes), `bind`, and
    `configdir` (else a tmp subdir under tmp_path)."""
    spawned: list[PeerHandle] = []

    def _spawn(*,
               port: Optional[int] = None,
               identity: Optional[str] = None,
               bind: str = "127.0.0.1",
               configdir: Optional[Path] = None,
               label: Optional[str] = None) -> PeerHandle:
        port = port or DEFAULT_TEST_PEER_PORT
        configdir = configdir or (tmp_path / f"peer-{port}")
        configdir.mkdir(parents=True, exist_ok=True)

        argv = [
            str(VENV_PY), "-u", str(ECHO_PEER),
            "--port", str(port),
            "--bind", bind,
            "--configdir", str(configdir),
        ]
        if identity:
            argv += ["--identity", identity]

        proc = subprocess.Popen(
            argv,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        label = label or f"peer:{port}"
        stderr_drain = _drain(proc.stderr, label)

        # Wait for the READY sentinel. echo_peer flushes it after announce.
        deadline = time.monotonic() + READY_TIMEOUT_SEC
        dest_hash: Optional[str] = None
        while time.monotonic() < deadline:
            line = proc.stdout.readline()
            if not line:
                # Subprocess exited before READY — bubble up stderr.
                rc = proc.poll()
                raise RuntimeError(
                    f"echo_peer exited (rc={rc}) before READY; see [{label}] stderr above"
                )
            decoded = line.decode(errors="replace").rstrip()
            sys.stderr.write(f"[{label}] {decoded}\n")
            if decoded.startswith("READY "):
                dest_hash = decoded.split(" ", 1)[1]
                break
        if dest_hash is None:
            proc.kill()
            raise TimeoutError(f"echo_peer did not emit READY within {READY_TIMEOUT_SEC}s")

        # Keep draining stdout in the background so the peer doesn't block.
        threading.Thread(target=_drain_stdout,
                         args=(proc.stdout, label), daemon=True).start()

        handle = PeerHandle(dest_hash=dest_hash, port=port, proc=proc,
                            configdir=configdir, _stderr_drain=stderr_drain)
        spawned.append(handle)
        return handle

    yield _spawn

    for handle in spawned:
        handle.terminate()


def _drain_stdout(stream, label: str) -> None:
    for line in iter(stream.readline, b""):
        sys.stderr.write(f"[{label}] {line.decode(errors='replace')}")
    stream.close()


# ---- Session-scoped client-side Reticulum -------------------------------

@pytest.fixture(scope="session")
def client_configdir(tmp_path_factory) -> Path:
    """A configdir for the test-process Reticulum with a static
    TCPClientInterface to DEFAULT_TEST_PEER_PORT. Going through the
    standard config path matters: `RNS.Reticulum.__init__` sets a long
    list of post-construction attributes on each interface (announce
    rate, ifac, ingress/egress controls, …); a bare constructor leaves
    them absent and the announce pipeline crashes when it touches them.
    """
    cdir = tmp_path_factory.mktemp("client-rns")
    (cdir / "config").write_text(
        "[reticulum]\n"
        "enable_transport = No\n"
        "share_instance = No\n"
        "\n"
        "[logging]\n"
        "loglevel = 6\n"
        "\n"
        "[interfaces]\n"
        "  [[Test Client]]\n"
        "    type = TCPClientInterface\n"
        "    enabled = Yes\n"
        "    target_host = 127.0.0.1\n"
        f"    target_port = {DEFAULT_TEST_PEER_PORT}\n"
    )
    return cdir


@pytest.fixture(scope="session")
def client_rns(client_configdir: Path):
    """Process-singleton RNS.Reticulum for the test process. Imported
    lazily so a `pytest --collect-only` doesn't pay the import cost.

    The client's TCPClientInterface keeps trying to reconnect across
    tests — that's fine, because each test spawns a new peer on the
    same DEFAULT_TEST_PEER_PORT and the client picks it up on the next
    reconnect cycle (within ~5s of peer-up).
    """
    import RNS  # noqa: WPS433
    instance = RNS.Reticulum(configdir=str(client_configdir),
                             loglevel=RNS.LOG_VERBOSE)
    yield instance
    # No explicit teardown — Reticulum has its own atexit hooks and trying
    # to manually unwind the singleton produces noise without value.
