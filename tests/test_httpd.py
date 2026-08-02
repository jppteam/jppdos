"""The in-house HTTP server and the WebDAV surface on top of it, on the host.

jpp_http_server_core.c is plain C over BSD sockets — its only ESP-IDF
dependencies are FreeRTOS task/mutex creation and ESP_LOG — so the real
firmware source compiles natively against a small pthread shim and can be
driven over loopback with real HTTP bytes. That covers the parts a device
build cannot check cheaply: request framing, keep-alive, chunked bodies,
Expect: 100-continue, and the WebDAV verbs a client actually sends.

The same pattern as tests/keypad_harness.py: compile the firmware source, not
a copy of it, so a regression fails here instead of on hardware.
"""

import os
import shutil
import socket
import subprocess
from pathlib import Path

import pytest

REPO_ROOT = Path(__file__).resolve().parent.parent
HARNESS_DIR = Path(__file__).resolve().parent / "httpd_harness"

CORE_INC = REPO_ROOT / "components" / "jpp_core" / "include"
CORE_SRC = REPO_ROOT / "components" / "jpp_core" / "src"
POOL_INC = REPO_ROOT / "components" / "jpp_app_pool" / "include"
POOL_SRC = REPO_ROOT / "components" / "jpp_app_pool" / "src"

HARNESSES = {
    "http_server": ["http_server_harness.c"],
    "webdav": ["webdav_harness.c"],
}

FIRMWARE_SRCS = {
    "http_server": [CORE_SRC / "jpp_http_server_core.c",
                    POOL_SRC / "jpp_app_pool.c"],
    "webdav": [CORE_SRC / "jpp_http_server_core.c",
               CORE_SRC / "jpp_fileserver_core.c",
               POOL_SRC / "jpp_app_pool.c"],
}


def _free_port() -> int:
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def _build(name: str, out_dir: Path) -> Path:
    cc = shutil.which("cc") or shutil.which("gcc") or shutil.which("clang")
    if cc is None:
        pytest.skip("no C compiler available to build the httpd harness")

    binary = out_dir / name
    cmd = [
        cc, "-std=gnu11", "-Wall", "-Wextra", "-Wno-unused-parameter", "-g",
        "-I", str(HARNESS_DIR / "stub"),
        "-I", str(CORE_INC),
        "-I", str(POOL_INC),
        "-o", str(binary),
    ]
    cmd += [str(HARNESS_DIR / f) for f in HARNESSES[name]]
    cmd += [str(HARNESS_DIR / "stub" / "stubs.c")]
    cmd += [str(p) for p in FIRMWARE_SRCS[name]]
    cmd += ["-lpthread"]

    result = subprocess.run(cmd, capture_output=True, text=True)
    if result.returncode != 0:
        pytest.fail(f"harness build failed:\n{result.stdout}\n{result.stderr}")
    return binary


def _run(binary: Path, *args: str) -> str:
    result = subprocess.run([str(binary), *args], capture_output=True,
                            text=True, timeout=120)
    if result.returncode != 0:
        pytest.fail(
            f"{binary.name} reported failures:\n{result.stdout}\n"
            f"--- server log ---\n{result.stderr}"
        )
    return result.stdout


def test_http_server_core(tmp_path):
    """Framing, keep-alive, chunked/fixed bodies, and the app-pool interlock."""
    out = _run(_build("http_server", tmp_path), str(_free_port()))
    assert "PASSED" in out
    # The pool interlock is the point of the rewrite: no server may start while
    # an app holds the pool, and stopping must hand every byte back.
    assert "pool released on stop" in out
    assert "server refuses to start while an app holds the pool" in out
    # Stopping while a client is parked mid-body is the case the release
    # ordering exists for: the task runs *on* the pool it is about to hand back.
    assert "pool released after a mid-transfer stop" in out


def test_webdav_surface(tmp_path):
    """The WebDAV verbs a real client sends, over the ported fileserver."""
    # Served root must fit jpp_fileserver_config_t.sd_root (64 bytes) — on the
    # device it is "/sd", so a short path is required here, not pytest's
    # tmp_path (which is well over 64 chars on macOS).
    root = Path("/tmp") / f"jppd_dav_{os.getpid()}"
    try:
        out = _run(_build("webdav", tmp_path), str(_free_port()), str(root))
    finally:
        shutil.rmtree(root, ignore_errors=True)
    assert "PASSED" in out
    # A 100 KB round trip is what proves the pool I/O buffer path, and the
    # traversal check is the one security-relevant behaviour in build_path().
    assert "GET 100 KB: bytes match what was PUT" in out
    assert "traversal (..) -> 400" in out
