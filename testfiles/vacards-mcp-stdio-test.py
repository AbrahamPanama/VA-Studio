#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""End-to-end MCP stdio contract test against the built vastudio-cli binary."""
import json
import os
import pathlib
import queue
import shutil
import subprocess
import sys
import tempfile
import threading
import time


class Harness:
    def __init__(self, process):
        self.process = process
        self.lines = queue.Queue()
        self.buffer = bytearray()
        self.reader = threading.Thread(target=self._read, daemon=True)
        self.reader.start()

    def _read(self):
        while True:
            chunk = os.read(self.process.stdout.fileno(), 4096)
            if not chunk:
                self.lines.put(None)
                return
            self.buffer.extend(chunk)
            while b"\n" in self.buffer:
                raw, _, remaining = self.buffer.partition(b"\n")
                self.buffer = bytearray(remaining)
                try:
                    message = json.loads(raw.decode("utf-8"))
                    if not isinstance(message, dict) or message.get("jsonrpc") != "2.0":
                        raise ValueError("line is not a JSON-RPC 2.0 object")
                    self.lines.put(message)
                except Exception as exc:
                    self.lines.put(("invalid", raw.decode("utf-8", "replace"), str(exc)))

    def send(self, message):
        self.process.stdin.write((json.dumps(message, separators=(",", ":")) + "\n").encode())
        self.process.stdin.flush()

    def response(self, request_id, timeout=30):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            try:
                message = self.lines.get(timeout=max(0.01, deadline - time.monotonic()))
            except queue.Empty:
                break
            if message is None:
                raise RuntimeError("child closed stdout before the requested response")
            if isinstance(message, tuple):
                raise RuntimeError(f"invalid stdout JSON-RPC line: {message[1]!r}: {message[2]}")
            if message.get("id") == request_id:
                if "error" in message:
                    raise RuntimeError(f"JSON-RPC error for {request_id}: {message['error']}")
                return message.get("result")
        raise TimeoutError(f"timed out waiting for JSON-RPC response {request_id}")

    def request(self, request_id, method, params=None):
        message = {"jsonrpc": "2.0", "id": request_id, "method": method}
        if params is not None:
            message["params"] = params
        self.send(message)
        return self.response(request_id)

    def call(self, request_id, name, arguments):
        return self.request(request_id, "tools/call", {"name": name, "arguments": arguments})

    def wait_eof(self, timeout):
        try:
            message = self.lines.get(timeout=timeout)
        except queue.Empty:
            return False
        if message is not None:
            if isinstance(message, tuple):
                raise RuntimeError(f"invalid stdout JSON-RPC line: {message[1]!r}: {message[2]}")
            raise RuntimeError(f"unexpected stdout message after EOF request: {message!r}")
        return True


def step(name, passed, detail=""):
    print(f"MCP-STEP {name} {'PASS' if passed else 'FAIL'}" + (f" ({detail})" if detail else ""), flush=True)
    return passed


def unwrap(call_result):
    if not isinstance(call_result, dict):
        raise RuntimeError(f"tools/call result is not an object: {call_result!r}")
    structured = call_result.get("structuredContent")
    if not isinstance(structured, dict):
        raise RuntimeError(f"tools/call result has no structuredContent: {call_result!r}")
    return structured


def main():
    if len(sys.argv) != 2:
        print("usage: vacards-mcp-stdio-test.py <vastudio-cli>", file=sys.stderr)
        return 2
    cli = pathlib.Path(sys.argv[1]).resolve()
    fixture = pathlib.Path(__file__).resolve().parent / "cli_tests/vacards-agent/fixtures/m3/explode.svg"
    all_passed = True
    with tempfile.TemporaryDirectory(prefix="vacards-mcp-stdio-") as temporary:
        root = pathlib.Path(temporary)
        source = root / "embedded-image.svg"
        shutil.copyfile(fixture, source)
        process = subprocess.Popen(
            [str(cli), "--mcp-stdio", "--grant-read", str(root), "--grant-write", str(root)],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, bufsize=0)
        harness = Harness(process)
        try:
            initialized = harness.request(1, "initialize", {
                "protocolVersion": "2025-06-18", "capabilities": {},
                "clientInfo": {"name": "vacards-mcp-test", "version": "1"}})
            all_passed &= step("initialize", initialized.get("protocolVersion") == "2025-06-18"
                               and "tools" in initialized.get("capabilities", {}))
            harness.send({"jsonrpc": "2.0", "method": "notifications/initialized"})
            # The next request is the acknowledgment that the initialized notification was accepted.
            ping = harness.request(2, "ping", {})
            all_passed &= step("initialized-notification", ping == {})

            tools = []
            cursor = None
            page_number = 0
            while True:
                params = {"cursor": cursor} if cursor else {}
                page = harness.request(10 + page_number, "tools/list", params)
                tools.extend(page.get("tools", []))
                cursor = page.get("nextCursor")
                page_number += 1
                if not cursor:
                    break
            names = [item.get("name") for item in tools]
            all_passed &= step("tools-list-pages", len(tools) > 0 and len(names) == len(set(names)),
                               f"{len(tools)} tools across {page_number} page(s)")

            catalog_call = harness.call(30, "va_system_catalog", {"params": {}})
            catalog_result = unwrap(catalog_call)
            catalog = catalog_result.get("data", {})
            available = [row["id"] for row in catalog.get("commands", [])
                         if row.get("available", True)]
            expected = {"va_" + command.replace(".", "_").replace("-", "_") for command in available}
            all_passed &= step("catalog-tool-coverage", bool(expected) and expected.issubset(set(names)),
                               f"{len(expected)} available catalog commands")

            opened = unwrap(harness.call(31, "va_file_open", {
                "params": {"path": str(source), "format": "svg", "resource-policy": "embed",
                           "font-policy": "reject", "discard": False}, "if_revision": 0}))
            document = opened.get("document_id")
            revision = opened.get("revision_after", 0)
            all_passed &= step("file-open", opened.get("status") == "changed" and bool(document))

            moved = unwrap(harness.call(32, "va_geometry_move", {
                "params": {"ids": ["shape1"], "dx": {"value": 2, "unit": "px"},
                           "dy": {"value": 3, "unit": "px"}},
                "document": document, "if_revision": revision}))
            revision = moved.get("revision_after", revision)
            all_passed &= step("geometry-command", moved.get("status") == "changed")

            saved_path = root / "saved.svg"
            saved = unwrap(harness.call(33, "va_file_save", {
                "params": {"path": str(saved_path), "embedding-policy": "embed", "overwrite": False},
                "document": document, "if_revision": revision}))
            all_passed &= step("file-save", saved.get("status") == "changed" and saved_path.is_file())

            bitmap = unwrap(harness.call(34, "va_bitmap_histogram", {
                "params": {"ids": ["image1"], "channel": "luminance", "bins": 256, "remap": "none"},
                "document": document, "if_revision": revision}))
            all_passed &= step("bitmap-affinity", "status" in bitmap and isinstance(bitmap.get("data"), dict),
                               f"typed status={bitmap.get('status')}")

            refused_call = harness.call(35, "va_geometry_move", {
                "params": {"ids": ["unknown-object-id"], "dx": {"value": 1, "unit": "px"},
                           "dy": {"value": 1, "unit": "px"}},
                "document": document, "if_revision": revision})
            refused = unwrap(refused_call)
            all_passed &= step("structured-refusal", refused_call.get("isError") is True
                               and isinstance(refused.get("error"), dict)
                               and bool(refused["error"].get("code")))

            process.stdin.close()
            eof_seen = harness.wait_eof(10)
            try:
                exit_code = process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                exit_code = None
            all_passed &= step("eof-clean-exit", eof_seen and exit_code == 0,
                               f"exit={exit_code}")
        except Exception as exc:
            all_passed &= step("harness", False, str(exc))
            if process.poll() is None:
                process.stdin.close()
                try:
                    process.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    pass
        finally:
            if process.poll() is None:
                process.stdin.close()
                try:
                    process.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()
    return 0 if all_passed else 1


if __name__ == "__main__":
    sys.exit(main())
