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


def unwrap(call_result):
    if not isinstance(call_result, dict):
        raise RuntimeError(f"tools/call result is not an object: {call_result!r}")
    structured = call_result.get("structuredContent")
    if not isinstance(structured, dict):
        raise RuntimeError(f"tools/call result has no structuredContent: {call_result!r}")
    return structured


def require(passed, reason):
    if not passed:
        raise RuntimeError(reason)


def run_check(cli, name):
    fixture = pathlib.Path(__file__).resolve().parent / "cli_tests/vacards-agent/fixtures/m3/explode.svg"
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
            if name == "initialize":
                require(initialized.get("protocolVersion") == "2025-06-18"
                        and "tools" in initialized.get("capabilities", {}),
                        "initialize response did not advertise the expected protocol and tools")
                return

            harness.send({"jsonrpc": "2.0", "method": "notifications/initialized"})
            ping = harness.request(2, "ping", {})
            if name == "initialized-notification":
                require(ping == {}, "ping acknowledgment was not an empty object")
                return

            if name in ("tools-list-pages", "catalog-tool-coverage"):
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
                if name == "tools-list-pages":
                    require(len(tools) > 0 and len(names) == len(set(names)),
                            f"{len(tools)} tools across {page_number} page(s); names must be unique")
                    return
                catalog = unwrap(harness.call(30, "va_system_catalog", {"params": {}})).get("data", {})
                available = [row["id"] for row in catalog.get("commands", [])
                             if row.get("available", True)]
                expected = {"va_" + command.replace(".", "_").replace("-", "_") for command in available}
                require(bool(expected) and expected.issubset(set(names)),
                        f"{len(expected)} available catalog commands are not covered")
                return

            if name == "eof-clean-exit":
                cursor = None
                page_number = 0
                while True:
                    page = harness.request(10 + page_number, "tools/list",
                                           {"cursor": cursor} if cursor else {})
                    cursor = page.get("nextCursor")
                    page_number += 1
                    if not cursor:
                        break
                unwrap(harness.call(30, "va_system_catalog", {"params": {}}))
                opened = unwrap(harness.call(31, "va_file_open", {
                    "params": {"path": str(source), "format": "svg", "resource-policy": "embed",
                               "font-policy": "reject", "discard": False}, "if_revision": 0}))
                document = opened.get("document_id")
                revision = opened.get("revision_after", 0)
                moved = unwrap(harness.call(32, "va_geometry_move", {
                    "params": {"ids": ["shape1"], "dx": {"value": 2, "unit": "px"},
                               "dy": {"value": 3, "unit": "px"}},
                    "document": document, "if_revision": revision}))
                revision = moved.get("revision_after", revision)
                saved_path = root / "saved.svg"
                harness.call(33, "va_file_save", {
                    "params": {"path": str(saved_path), "embedding-policy": "embed", "overwrite": False},
                    "document": document, "if_revision": revision})
                unwrap(harness.call(34, "va_bitmap_histogram", {
                    "params": {"ids": ["image1"], "channel": "luminance", "bins": 256, "remap": "none"},
                    "document": document, "if_revision": revision}))
                unwrap(harness.call(35, "va_geometry_move", {
                    "params": {"ids": ["unknown-object-id"], "dx": {"value": 1, "unit": "px"},
                               "dy": {"value": 1, "unit": "px"}},
                    "document": document, "if_revision": revision}))
                process.stdin.close()
                eof_seen = harness.wait_eof(10)
                exit_code = process.wait(timeout=10)
                require(eof_seen and exit_code == 0, f"EOF={eof_seen}, exit={exit_code}")
                return

            opened = unwrap(harness.call(31, "va_file_open", {
                "params": {"path": str(source), "format": "svg", "resource-policy": "embed",
                           "font-policy": "reject", "discard": False}, "if_revision": 0}))
            document = opened.get("document_id")
            revision = opened.get("revision_after", 0)
            if name == "file-open":
                require(opened.get("status") == "changed" and bool(document), "file open did not change a document")
                return
            require(bool(document), "file open prerequisite did not return a document")

            if name in ("geometry-command", "file-save", "bitmap-affinity"):
                moved = unwrap(harness.call(32, "va_geometry_move", {
                    "params": {"ids": ["shape1"], "dx": {"value": 2, "unit": "px"},
                               "dy": {"value": 3, "unit": "px"}},
                    "document": document, "if_revision": revision}))
                revision = moved.get("revision_after", revision)
                if name == "geometry-command":
                    require(moved.get("status") == "changed", "geometry move did not change the document")
                    return
                saved_path = root / "saved.svg"
                saved = unwrap(harness.call(33, "va_file_save", {
                    "params": {"path": str(saved_path), "embedding-policy": "embed", "overwrite": False},
                    "document": document, "if_revision": revision}))
                if name == "file-save":
                    require(saved.get("status") == "changed" and saved_path.is_file(),
                            "save did not change the document and create the output file")
                    return
                bitmap = unwrap(harness.call(34, "va_bitmap_histogram", {
                    "params": {"ids": ["image1"], "channel": "luminance", "bins": 256, "remap": "none"},
                    "document": document, "if_revision": revision}))
                require("status" in bitmap and isinstance(bitmap.get("data"), dict),
                        f"bitmap result has no typed status/data (status={bitmap.get('status')})")
                return

            if name == "structured-refusal":
                refused_call = harness.call(35, "va_geometry_move", {
                    "params": {"ids": ["unknown-object-id"], "dx": {"value": 1, "unit": "px"},
                               "dy": {"value": 1, "unit": "px"}},
                    "document": document, "if_revision": revision})
                refused = unwrap(refused_call)
                require(refused_call.get("isError") is True and isinstance(refused.get("error"), dict)
                        and bool(refused["error"].get("code")), "unknown object refusal was not structured")
                return
            raise ValueError(f"unknown check: {name}")
        finally:
            if process.poll() is None:
                if process.stdin and not process.stdin.closed:
                    process.stdin.close()
                try:
                    process.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()


def load_cases(directory):
    cases = []
    for path in sorted(pathlib.Path(directory).glob("*.json"), key=lambda item: item.name):
        try:
            data = json.loads(path.read_text(encoding="utf-8"))
            check = data["check"]
            if not isinstance(check, str) or not check:
                raise ValueError("check must be a non-empty string")
            cases.append((path.name, check))
        except (OSError, ValueError, KeyError, TypeError) as exc:
            cases.append((path.name, exc))
    return cases


def main():
    if len(sys.argv) != 3:
        print("usage: vacards-mcp-stdio-test.py <vastudio-cli> <case-directory>", file=sys.stderr)
        return 2
    cli = pathlib.Path(sys.argv[1]).resolve()
    cases = load_cases(sys.argv[2])
    if not cases:
        print("no JSON cases found", file=sys.stderr)
        return 1
    known = {"initialize", "initialized-notification", "tools-list-pages", "catalog-tool-coverage",
             "file-open", "geometry-command", "file-save", "bitmap-affinity", "structured-refusal",
             "eof-clean-exit"}
    failed = False
    for filename, check in cases:
        try:
            if isinstance(check, Exception):
                raise check
            if check not in known:
                raise ValueError(f"unknown check: {check}")
            run_check(cli, check)
            print(f"PASS {filename}", flush=True)
        except Exception as exc:
            failed = True
            print(f"FAIL {filename}: {exc}", flush=True)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
