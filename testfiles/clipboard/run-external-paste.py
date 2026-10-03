#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Driver for the external-process clipboard paste protocol (milestone A).

One fresh receiver process per case, one native macOS publisher process per case,
one exclusive flock lease for the whole run. The lease file lives directly under
the run-INDEPENDENT session root (<session-root>/lease.lock), so two drivers with
different run ids contend for the same lock; the per-run diagnostic lease.json
stays in <session-root>/<run-id>/ where the publisher and receiver read it. The
driver owns clipboard hygiene: it snapshots the user's clipboard before any
publication and restores it only while the harness still owns the pasteboard
(changeCount unchanged since the last harness publication). It never logs
clipboard bytes: lengths and SHA-256 only.

This suite is OPT-IN. Without --allow-clipboard-takeover (or VACARDS_CLIP_ALLOW=1)
it refuses to publish and records every selected case as env-blocked:not-opted-in,
exiting 77 so CTest can report a skip instead of a false pass. Once opted in, a
non-pass outcome (including env-blocked) is a non-zero exit: a qualification run
must never turn "could not test" into green.

`--protocol-self-test` runs the pure protocol checks (paths, lease contention
between two subprocesses, the skip-aware receiver wrapper's exit-code mapping,
fixture/expect hash binding including the multi-representation schema, summary
accounting, restore decision logic with injected failures and the
snapshot-refusal/retention rules). It never starts the publisher or the receiver
and never reads or writes the clipboard.

See testfiles/clipboard/README.md for the protocol and limitations.
"""

from __future__ import annotations

import argparse
import fcntl
import hashlib
import json
import os
import platform
import plistlib
import shutil
import signal
import subprocess
import sys
import tempfile
import time
import uuid
from pathlib import Path

EXIT_OK = 0
EXIT_FAIL = 1
EXIT_USAGE = 2
EXIT_NOT_OPTED_IN = 77

PUB_OK = 0
PUB_LEASE = 3
PUB_PUBLISH = 4
PUB_SNAPSHOT = 5
PUB_OWNERSHIP = 6
PUB_HOLD_TIMEOUT = 7

FAILURE_CLASSES = {"success", "abort-no-mutation", "reject-no-mutation", "fallback-plain",
                   "import-object", "no-op"}
# Classes whose contract requires the paste to put text into the document. A
# fallback-plain case publishes a malformed/rich representation plus a complete
# plain alternative and must paste that alternative, so it is NOT a reject class.
# import-object pastes an object/SVG representation instead of text: the document
# changes and one Undo step appears, but no text object may be created.
PASTING_CLASSES = {"success", "fallback-plain"}
NON_PASTING_CLASSES = {"abort-no-mutation", "reject-no-mutation", "no-op"}
OBJECT_IMPORT_CLASSES = {"import-object"}
EMPTY_TEXT_SHA256 = hashlib.sha256(b"").hexdigest()
DEFAULT_CASES = ("E01,E02,E04,E05,E06,E07_at_cap,E07_multibyte,E07_over_cap,"
                 "E13_object_priority_svg_plain,B15_object_priority_svg_html,"
                 "F13_plain_svg_object_path,E08_object_target_stall,R04_fallback_plain_distinct")
TERMINATE_GRACE_S = 5.0
# A restore that failed while writing may be retried once; every other non-zero
# publisher exit is deterministic (missing blob, ownership lost, timeout).
MAX_RESTORE_ATTEMPTS = 2
RETRYABLE_RESTORE_CODES = frozenset({PUB_PUBLISH})
# Restore statuses after which the user's data is still on the pasteboard (or was
# never replaced), so deleting the snapshot blobs cannot lose anything. An
# incomplete snapshot is deliberately NOT safe: the partial copy may be the only
# recovery data the user has, so its session (and blobs) is retained until a
# verified restore.
SAFE_TO_DELETE_RESTORE_STATUSES = (
    "restored",
    "restore-skipped:no-publication",
    "restore-skipped:clipboard-ownership-lost",
    "restore-skipped:not-opted-in",
)


class HarnessError(Exception):
    """A harness/environment problem: the product was not exercised."""


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def atomic_write_json(path: Path, payload, mode: int = 0o600) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_name(path.name + f".tmp.{os.getpid()}")
    data = json.dumps(payload, ensure_ascii=False, indent=2, sort_keys=True).encode("utf-8")
    fd = os.open(tmp, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, mode)
    try:
        os.write(fd, data)
        os.fsync(fd)
    finally:
        os.close(fd)
    os.replace(tmp, path)


def append_jsonl(path: Path, payload) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    line = json.dumps(payload, ensure_ascii=False, sort_keys=True) + "\n"
    with path.open("a", encoding="utf-8") as handle:
        handle.write(line)
        handle.flush()
        os.fsync(handle.fileno())


def read_json(path: Path):
    with path.open("r", encoding="utf-8") as handle:
        return json.load(handle)


def summarize_outcomes(outcomes) -> dict:
    """Count product outcomes for summary.json.

    `executed` counts cases whose product behavior was observed (pass/fail and
    anything unknown); `skipped` counts cases the harness could not run at all
    (env-blocked/not-run). A summary must never let a skipped case look executed.
    """
    counts = {"total": 0, "pass": 0, "fail": 0, "env-blocked": 0, "not-run": 0,
              "executed": 0, "skipped": 0, "other": 0}
    for outcome in outcomes.values():
        counts["total"] += 1
        if outcome in ("pass", "fail", "env-blocked", "not-run"):
            counts[outcome] += 1
        else:
            counts["other"] += 1
    counts["executed"] = counts["pass"] + counts["fail"] + counts["other"]
    counts["skipped"] = counts["env-blocked"] + counts["not-run"]
    return counts


def restore_precheck(snapshot_complete: bool, expected_change_count, current_change_count):
    """Decision before a restore attempt: (proceed, status).

    Pure and injectable so the self-test can cover every refusal without a
    pasteboard. The changeCount comparison is the ownership guard: the driver
    never writes when a third party wrote after the harness publication.
    """
    if not snapshot_complete:
        return False, "restore-skipped:snapshot-incomplete"
    if expected_change_count is None:
        return False, "restore-skipped:no-publication"
    if current_change_count is None:
        return False, "restore-skipped:read-back-failed"
    if current_change_count != expected_change_count:
        return False, "restore-skipped:clipboard-ownership-lost"
    return True, "restore-pending"


def restore_outcome(returncode: int, attempt: int):
    """Decision after a restore attempt: (action, status).

    action is "retry" (bounded by MAX_RESTORE_ATTEMPTS, and the caller re-runs
    the ownership precheck before the next write) or "done". The publisher also
    retries a transient writeObjects failure internally; this outer retry covers
    a failure that left the pasteboard unchanged.
    """
    if returncode == PUB_OK:
        return "done", "restored"
    if returncode == PUB_OWNERSHIP:
        return "done", "restore-skipped:clipboard-ownership-lost"
    if returncode in RETRYABLE_RESTORE_CODES and attempt < MAX_RESTORE_ATTEMPTS:
        return "retry", f"restore-retrying:{returncode}"
    return "done", f"restore-skipped:restore-failed-{returncode}"


def restore_snapshot_retained(status: str) -> bool:
    """True when the snapshot must survive the run for manual recovery.

    Blobs are deleted only when the pasteboard either holds the user's data
    again (restored) or was never replaced / is owned by a newer user copy.
    A failed, timed-out or unverifiable restore leaves the pasteboard without the
    user's data, so the snapshot may be the only copy and is kept.
    """
    return status not in SAFE_TO_DELETE_RESTORE_STATUSES


def restore_status_without_publication(snapshot_captured: bool, snapshot_complete: bool,
                                       snapshot_restorable: bool) -> str:
    """Restore status when no harness publication ever happened.

    `last_change_count is None` does not mean "nothing to protect". A captured
    snapshot that is incomplete, or complete but failed pre-publication
    validation, has no verified recovery copy, so its session must be reported
    as unsafe to delete instead of being mislabelled as the safe no-publication
    case. A run whose snapshot process never produced a manifest captured
    nothing, so there is no recovery data to keep.
    """
    if not snapshot_captured:
        return "restore-skipped:no-publication"
    if not snapshot_complete:
        return "restore-skipped:snapshot-incomplete"
    if not snapshot_restorable:
        return "restore-skipped:snapshot-unrestorable"
    return "restore-skipped:no-publication"


def publication_refusal(snapshot_complete: bool, snapshot_failure) -> str | None:
    """Refusal reason that stops the run before any publication, or None.

    The snapshot is the only copy of the user's clipboard, so an incomplete or
    unverifiable snapshot refuses the whole run: the pasteboard is never
    replaced, every selected case is recorded env-blocked with this reason and
    the driver exits non-zero. There is no opt-out flag.
    """
    if snapshot_failure:
        return snapshot_failure
    if not snapshot_complete:
        return "snapshot-incomplete:refusing-publication"
    return None


def validate_snapshot_restore_data(snapshot, blob_dir: Path) -> list:
    """Reasons this snapshot cannot be restored; [] when every blob is usable.

    Called BEFORE any publication so a snapshot that could never be put back is
    discovered while the user's clipboard is still intact, instead of after the
    pasteboard was replaced. Every non-null entry must have a readable blob whose
    length and SHA-256 match the snapshot manifest; a snapshot with no usable
    item is unusable even if its `complete` flag lies.
    """
    if not isinstance(snapshot, dict):
        return ["snapshot-manifest-invalid"]
    problems = []
    if not snapshot.get("complete"):
        problems.append("snapshot-incomplete")
    items = snapshot.get("items")
    if not isinstance(items, list) or not items:
        problems.append("snapshot-has-no-items")
        return problems
    for index, item in enumerate(items):
        if not isinstance(item, dict):
            problems.append(f"item-{index}-invalid")
            continue
        types = item.get("types")
        if not isinstance(types, list) or not types:
            problems.append(f"item-{index}-has-no-types")
            continue
        usable = 0
        for entry in types:
            if not isinstance(entry, dict):
                problems.append(f"item-{index}-type-invalid")
                continue
            uti = entry.get("uti")
            name = entry.get("blob")
            if not uti or not name:
                # A type without a blob can never be restored (the publisher marks
                # such a snapshot incomplete; this is the second, independent check).
                problems.append(f"item-{index}-type-without-blob:{uti or '?'}")
                continue
            path = blob_dir / name
            if not path.is_file():
                problems.append(f"blob-missing:{name}")
                continue
            data = path.read_bytes()
            if entry.get("bytes") is not None and len(data) != entry.get("bytes"):
                problems.append(f"blob-size-mismatch:{name}")
                continue
            if entry.get("sha256") and sha256_bytes(data) != entry["sha256"]:
                problems.append(f"blob-hash-mismatch:{name}")
                continue
            usable += 1
        if usable == 0:
            problems.append(f"item-{index}-unrestorable")
    return problems


def run_command(cmd, timeout_s: float, env=None):
    return subprocess.run(cmd, capture_output=True, text=True, timeout=timeout_s, env=env, check=False)


def terminate_process(process, grace_s: float = TERMINATE_GRACE_S) -> None:
    """SIGTERM then SIGKILL: every path that started a publisher ends here."""
    if process is None or process.poll() is not None:
        return
    try:
        process.terminate()
        process.wait(timeout=grace_s)
        return
    except subprocess.TimeoutExpired:
        pass
    try:
        process.kill()
        process.wait(timeout=grace_s)
    except subprocess.TimeoutExpired:
        pass


def process_start_time(pid: int):
    try:
        result = run_command(["ps", "-p", str(pid), "-o", "lstart="], 5)
        text = result.stdout.strip()
        return text or None
    except Exception:
        return None


def parse_json_stdout(text: str, what: str):
    text = text.strip()
    if not text:
        raise HarnessError(f"{what} produced no stdout")
    try:
        return json.loads(text.splitlines()[-1])
    except json.JSONDecodeError as error:
        raise HarnessError(f"{what} produced invalid JSON: {error}") from error


# ---------------------------------------------------------------------------
# Manifest
# ---------------------------------------------------------------------------
class Manifest:
    def __init__(self, fixtures_dir: Path, verify_hashes: bool = True):
        self.dir = fixtures_dir
        self.path = fixtures_dir / "manifest.json"
        if not self.path.is_file():
            raise HarnessError(f"fixture manifest not found: {self.path}")
        data = read_json(self.path)
        if data.get("manifest_version") != 1:
            raise HarnessError("unsupported fixture manifest version")
        self.fixtures = {}
        for fixture in data.get("fixtures", []):
            self._validate_fixture(fixture, verify_hashes)
            self.fixtures[fixture["id"]] = fixture
        self.cases = data.get("cases", [])
        if not self.cases:
            raise HarnessError("fixture manifest has no cases")
        for case in self.cases:
            if case.get("fixture") not in self.fixtures:
                raise HarnessError(f"case {case.get('id')} references unknown fixture {case.get('fixture')}")
            if case.get("route") not in ("selector", "text-tool"):
                raise HarnessError(f"case {case.get('id')} has unsupported route {case.get('route')!r}")

    def _validate_fixture(self, fixture: dict, verify_hashes: bool) -> None:
        fixture_id = fixture.get("id")
        for key in ("id", "requirement", "file", "sha256", "bytes", "required_formats", "failure_class",
                    "transport_bytes", "publish", "expect"):
            if key not in fixture:
                raise HarnessError(f"fixture {fixture_id!r} is missing {key!r}")
        if fixture["failure_class"] not in FAILURE_CLASSES:
            raise HarnessError(f"fixture {fixture_id} has unknown failure_class {fixture['failure_class']!r}")
        if not isinstance(fixture["required_formats"], list) or not fixture["required_formats"]:
            raise HarnessError(f"fixture {fixture_id} has no required_formats")
        representations = self._validate_macos_publication(fixture_id, fixture, verify_hashes)
        expect = fixture["expect"]
        for key in ("text_sha256", "text_bytes", "object_count", "undo_steps", "document_unchanged"):
            if key not in expect:
                raise HarnessError(f"fixture {fixture_id} expect is missing {key!r}")
        digest = expect["text_sha256"]
        if not isinstance(digest, str) or len(digest) != 64:
            raise HarnessError(f"fixture {fixture_id} has an invalid text_sha256")
        for key in ("text_bytes", "object_count", "undo_steps"):
            if not isinstance(expect[key], int) or isinstance(expect[key], bool) or expect[key] < 0:
                raise HarnessError(f"fixture {fixture_id} expect.{key} must be a non-negative integer")
        if not isinstance(expect["document_unchanged"], bool):
            raise HarnessError(f"fixture {fixture_id} expect.document_unchanged must be a boolean")
        literal = expect.get("text_utf8")
        if literal is not None:
            if not isinstance(literal, str):
                raise HarnessError(f"fixture {fixture_id} expect.text_utf8 must be a string or null")
            if len(literal.encode("utf-8")) != expect["text_bytes"]:
                raise HarnessError(f"fixture {fixture_id} expect.text_utf8 length does not match expect.text_bytes")
        # New oracle fields (see README "Expect fields"):
        #   import_object_id     element id the imported object/SVG payload must add
        #   forbidden_text_utf8  distinctive string from the losing representation
        #                        that must never become document text (string or list)
        #   max_elapsed_ms       upper bound on one paste command's wall time
        import_object_id = expect.get("import_object_id")
        if import_object_id is not None and (not isinstance(import_object_id, str) or not import_object_id):
            raise HarnessError(f"fixture {fixture_id} expect.import_object_id must be a non-empty string or null")
        forbidden = self._forbidden_texts(fixture_id, expect)
        max_elapsed = expect.get("max_elapsed_ms")
        if max_elapsed is not None and (not isinstance(max_elapsed, int) or isinstance(max_elapsed, bool)
                                        or max_elapsed <= 0):
            raise HarnessError(f"fixture {fixture_id} expect.max_elapsed_ms must be a positive integer or null")
        if any(rep["stall"] for rep in representations) and max_elapsed is None:
            raise HarnessError(
                f"fixture {fixture_id}: a stalled representation must declare expect.max_elapsed_ms "
                f"(the receiver must prove the paste deadline is bounded)")
        if max_elapsed is not None and not any(rep["stall"] for rep in representations):
            raise HarnessError(
                f"fixture {fixture_id}: expect.max_elapsed_ms is a paste-deadline oracle, so at least one "
                f"representation must declare stall: true (otherwise nothing exercises the bound)")
        self._validate_expect_binding(fixture_id, fixture)
        if verify_hashes:
            path = self.dir / fixture["file"]
            if not path.is_file():
                raise HarnessError(f"fixture file missing: {path}")
            data = path.read_bytes()
            if len(data) != fixture["bytes"]:
                raise HarnessError(f"fixture {fixture_id} byte count does not match the manifest")
            if sha256_bytes(data) != fixture["sha256"]:
                raise HarnessError(f"fixture {fixture_id} hash does not match the manifest")
            blobs = [rep["data"] for rep in representations]
            for text in forbidden:
                if not any(text.encode("utf-8") in blob for blob in blobs):
                    raise HarnessError(
                        f"fixture {fixture_id}: expect.forbidden_text_utf8 {text!r} does not occur in any "
                        f"published representation, so the oracle cannot discriminate a fallback")
            if import_object_id and not any(import_object_id.encode("utf-8") in blob for blob in blobs):
                raise HarnessError(
                    f"fixture {fixture_id}: expect.import_object_id {import_object_id!r} does not occur in "
                    f"any published representation, so the import oracle is not discriminating")

    def _validate_macos_publication(self, fixture_id: str, fixture: dict, verify_hashes: bool) -> list:
        """Validate and normalize the macOS publication; return representation dicts.

        Two forms are accepted and they must not be mixed:
          * legacy  publish.macos{utis[], as}  — every UTI carries the fixture's
            own file/sha256/bytes with one `as` mode;
          * extended publish.macos.representations[{uti, file, sha256, bytes, as,
            stall}] — each representation carries ITS OWN payload. `stall: true`
            declares a representation whose data is advertised but never
            delivered (the lazy-provider protocol check).
        """
        macos = fixture["publish"].get("macos", {})
        if not isinstance(macos, dict):
            raise HarnessError(f"fixture {fixture_id} publish.macos must be an object")
        has_extended = "representations" in macos
        has_legacy = "utis" in macos or "as" in macos
        if has_extended and has_legacy:
            raise HarnessError(
                f"fixture {fixture_id} declares both publish.macos.representations and the legacy utis/as form")
        if has_extended:
            raw = macos["representations"]
            if not isinstance(raw, list) or not raw:
                raise HarnessError(f"fixture {fixture_id} publish.macos.representations must be a non-empty list")
        else:
            as_mode = macos.get("as")
            utis = macos.get("utis")
            if as_mode not in ("string", "data") or not isinstance(utis, list) or not utis:
                raise HarnessError(f"fixture {fixture_id} has no native macOS publication")
            raw = [{"uti": uti, "file": fixture.get("file"), "sha256": fixture.get("sha256"),
                    "bytes": fixture.get("bytes"), "as": as_mode, "stall": False} for uti in utis]
        representations = []
        for entry in raw:
            if not isinstance(entry, dict):
                raise HarnessError(f"fixture {fixture_id} has a non-object representation")
            uti = entry.get("uti")
            as_mode = entry.get("as")
            file_name = entry.get("file")
            sha = entry.get("sha256")
            size = entry.get("bytes")
            stall = entry.get("stall", False)
            if not isinstance(uti, str) or not uti:
                raise HarnessError(f"fixture {fixture_id} has a representation without a UTI")
            if as_mode not in ("string", "data"):
                raise HarnessError(f"fixture {fixture_id} representation {uti} has unsupported as {as_mode!r}")
            if not isinstance(file_name, str) or not file_name:
                raise HarnessError(f"fixture {fixture_id} representation {uti} has no file")
            if not isinstance(sha, str) or len(sha) != 64:
                raise HarnessError(f"fixture {fixture_id} representation {uti} has an invalid sha256")
            if not isinstance(size, int) or isinstance(size, bool) or size < 0:
                raise HarnessError(f"fixture {fixture_id} representation {uti} has an invalid byte count")
            if not isinstance(stall, bool):
                raise HarnessError(f"fixture {fixture_id} representation {uti} stall must be a boolean")
            representation = {"uti": uti, "file": file_name, "sha256": sha, "bytes": size, "as": as_mode,
                              "stall": stall}
            if verify_hashes:
                path = self.dir / file_name
                if not path.is_file():
                    raise HarnessError(f"fixture {fixture_id} representation file missing: {path}")
                data = path.read_bytes()
                if len(data) != size:
                    raise HarnessError(
                        f"fixture {fixture_id} representation {uti} byte count does not match the manifest")
                if sha256_bytes(data) != sha:
                    raise HarnessError(
                        f"fixture {fixture_id} representation {uti} hash does not match the manifest")
                representation["data"] = data
            representations.append(representation)
        # The fixture file/sha256/bytes stay the record's primary payload, so it
        # must be one of the published representations.
        if not any(rep["file"] == fixture["file"] and rep["sha256"] == fixture["sha256"]
                   and rep["bytes"] == fixture["bytes"] for rep in representations):
            raise HarnessError(
                f"fixture {fixture_id}: the fixture file/sha256/bytes must be one of "
                f"publish.macos representations")
        # transport_bytes is what the receiver sees on the wire: every string
        # representation gets exactly one trailing NUL from the macOS string path.
        expected_transport = sum(rep["bytes"] + (1 if rep["as"] == "string" else 0)
                                 for rep in representations)
        if fixture["transport_bytes"] != expected_transport:
            raise HarnessError(
                f"fixture {fixture_id}: transport_bytes must be {expected_transport} for its "
                f"representations (string payloads carry one terminal NUL), not {fixture['transport_bytes']}")
        return representations

    @staticmethod
    def macos_representations(fixture: dict) -> list:
        """Normalized macOS publication: one entry per UTI (no hash verification).

        The legacy `publish.macos{utis[], as}` form publishes the fixture's own
        file/sha256/bytes for every UTI; the `representations[]` form gives each
        UTI its own file/sha256/bytes/as. Callers that must trust the payload use
        the validated list returned by `_validate_macos_publication`.
        """
        macos = fixture["publish"]["macos"]
        if "representations" in macos:
            return [{"uti": entry.get("uti"), "file": entry.get("file"), "sha256": entry.get("sha256"),
                     "bytes": entry.get("bytes"), "as": entry.get("as"),
                     "stall": bool(entry.get("stall", False))}
                    for entry in macos["representations"]]
        return [{"uti": uti, "file": fixture.get("file"), "sha256": fixture.get("sha256"),
                 "bytes": fixture.get("bytes"), "as": macos.get("as"), "stall": False}
                for uti in macos.get("utis", [])]

    @staticmethod
    def _forbidden_texts(fixture_id: str, expect: dict):
        """Normalize expect.forbidden_text_utf8 (string, list of strings or null)."""
        raw = expect.get("forbidden_text_utf8")
        if raw is None:
            return []
        entries = raw if isinstance(raw, list) else [raw]
        if not entries:
            raise HarnessError(f"fixture {fixture_id} expect.forbidden_text_utf8 must not be an empty list")
        for entry in entries:
            if not isinstance(entry, str) or not entry:
                raise HarnessError(
                    f"fixture {fixture_id} expect.forbidden_text_utf8 entries must be non-empty strings")
        return entries

    @staticmethod
    def _validate_expect_binding(fixture_id: str, fixture: dict) -> None:
        """Bind expect.text_sha256/text_bytes to what the case must observe.

        For `success` the document text is exactly the fixture bytes, so the
        oracle hash and length must equal the manifest fixture hash and byte
        count. For `fallback-plain` the fixture is the malformed representation
        and the oracle describes the complete plain alternative, which is a
        different string, so only a real paste may be declared. For
        `import-object` the object/SVG payload must win: the document changes and
        one Undo step appears, but the oracle's text is empty and no text object
        may be created; `import_object_id` and `forbidden_text_utf8` are required
        so the case can tell "object imported" from "plain text pasted". For the
        reject/abort/no-op classes the document must not change, so the expected
        text is empty and no Undo step and no object may be expected.
        """
        failure_class = fixture["failure_class"]
        expect = fixture["expect"]
        forbidden = Manifest._forbidden_texts(fixture_id, expect)
        if failure_class == "success":
            if expect["text_sha256"] != fixture["sha256"] or expect["text_bytes"] != fixture["bytes"]:
                raise HarnessError(
                    f"fixture {fixture_id}: a success case must expect exactly the fixture bytes "
                    f"(expect.text_sha256/text_bytes must equal sha256/bytes)")
            if expect["document_unchanged"] or expect["object_count"] < 1 or expect["undo_steps"] < 1:
                raise HarnessError(
                    f"fixture {fixture_id}: a success case must expect a document change, at least one "
                    f"object and at least one Undo step")
        elif failure_class == "fallback-plain":
            if expect["document_unchanged"] or expect["object_count"] < 1 or expect["text_bytes"] < 1:
                raise HarnessError(
                    f"fixture {fixture_id}: fallback-plain must paste the complete plain alternative "
                    f"(document_unchanged=false, object_count>=1, text_bytes>=1)")
            if expect["text_sha256"] == EMPTY_TEXT_SHA256:
                raise HarnessError(
                    f"fixture {fixture_id}: fallback-plain must expect the plain alternative text, not empty text")
            if not forbidden:
                raise HarnessError(
                    f"fixture {fixture_id}: fallback-plain must declare expect.forbidden_text_utf8 "
                    f"(a distinctive string from the malformed rich payload that must not be pasted)")
        elif failure_class == "import-object":
            if expect["document_unchanged"]:
                raise HarnessError(f"fixture {fixture_id}: import-object must change the document")
            if expect["object_count"] != 0:
                raise HarnessError(
                    f"fixture {fixture_id}: import-object must not create a text object (object_count must be 0)")
            if expect["undo_steps"] < 1:
                raise HarnessError(f"fixture {fixture_id}: import-object must add at least one Undo step")
            if expect["text_bytes"] != 0 or expect["text_sha256"] != EMPTY_TEXT_SHA256:
                raise HarnessError(
                    f"fixture {fixture_id}: import-object must expect no pasted text "
                    f"(text_bytes=0, empty-text sha256)")
            if not expect.get("import_object_id"):
                raise HarnessError(
                    f"fixture {fixture_id}: import-object must declare expect.import_object_id")
            if not forbidden:
                raise HarnessError(
                    f"fixture {fixture_id}: import-object must declare expect.forbidden_text_utf8 "
                    f"(the losing text alternative must not become document text)")
        else:
            if not expect["document_unchanged"]:
                raise HarnessError(f"fixture {fixture_id}: {failure_class} must declare document_unchanged")
            if expect["object_count"] != 0 or expect["undo_steps"] != 0:
                raise HarnessError(
                    f"fixture {fixture_id}: {failure_class} must not create objects or Undo steps")
            if expect["text_bytes"] != 0 or expect["text_sha256"] != EMPTY_TEXT_SHA256:
                raise HarnessError(
                    f"fixture {fixture_id}: {failure_class} must expect the unchanged (empty) document text")
            if expect.get("require_nonempty_style", False):
                raise HarnessError(
                    f"fixture {fixture_id}: {failure_class} cannot require a pasted-text style")
        if forbidden:
            # The marker exists to prove the losing payload did NOT win, so it
            # must not be part of the text the oracle requires.
            literal = expect.get("text_utf8")
            for text in forbidden:
                if literal is not None and text in literal:
                    raise HarnessError(
                        f"fixture {fixture_id}: expect.forbidden_text_utf8 {text!r} is part of the expected "
                        f"pasted text; the losing payload must not share a marker with the winner")

    def select(self, tokens: str):
        wanted = [token.strip() for token in tokens.split(",") if token.strip()]
        if not wanted or "all" in wanted:
            return list(self.cases)
        selected = []
        for token in wanted:
            matches = [case for case in self.cases if case["id"] == token]
            if not matches:
                matches = [case for case in self.cases
                           if case.get("requirement") == token or case["id"].startswith(token + "_")]
            if not matches:
                raise HarnessError(f"unknown case selector: {token}")
            selected.extend(matches)
        deduped = []
        for case in selected:
            if case not in deduped:
                deduped.append(case)
        return deduped

    def expect_document(self, case: dict) -> dict:
        fixture = self.fixtures[case["fixture"]]
        representations = self.macos_representations(fixture)
        as_modes = sorted({rep["as"] for rep in representations})
        return {
            "schema": 1,
            "case": {
                "id": case["id"],
                "requirement": case["requirement"],
                "route": case["route"],
                "title": case.get("title", ""),
            },
            "fixture": {
                "id": fixture["id"],
                "file": fixture["file"],
                "sha256": fixture["sha256"],
                "bytes": fixture["bytes"],
            },
            "publisher": {
                "requested_utis": [rep["uti"] for rep in representations],
                "publish_as": as_modes[0] if len(as_modes) == 1 else "mixed",
                "representations": [
                    {"uti": rep["uti"], "file": rep["file"], "sha256": rep["sha256"],
                     "bytes": rep["bytes"], "as": rep["as"], "stall": rep["stall"]}
                    for rep in representations
                ],
                "transport_bytes": fixture["transport_bytes"],
            },
            "required_formats": fixture["required_formats"],
            "failure_class": fixture["failure_class"],
            "expect": fixture["expect"],
            "nonce": "",
            "session": "",
        }


# ---------------------------------------------------------------------------
# Lease
# ---------------------------------------------------------------------------
class Lease:
    """Exclusive flock over <session-root>/lease.lock, with a diagnostic record.

    The lock file must be run-INDEPENDENT: it lives directly under the session
    root, while each run keeps its own <session-root>/<run-id>/ directory. Two
    drivers with different run ids therefore contend for the same file, which is
    what actually serializes parallel runs. The per-run lease.json stays in the
    session directory because the publisher and receiver verify it there.
    """

    LOCK_NAME = "lease.lock"
    HOLDER_NAME = "lease.holder.json"

    def __init__(self, session_root: Path, session_dir: Path, nonce: str, run_id: str):
        self.session_root = session_root
        self.session_dir = session_dir
        self.nonce = nonce
        self.run_id = run_id
        self.fd = None
        self.lock_path = self.session_root / self.LOCK_NAME
        self.holder_path = self.session_root / self.HOLDER_NAME
        self.holder_record = None

    def _holder_description(self) -> str:
        try:
            record = read_json(self.holder_path)
        except Exception:
            return "unknown holder"
        parts = []
        if record.get("pid") is not None:
            parts.append(f"pid={record.get('pid')}")
        if record.get("run_id"):
            parts.append(f"run_id={record.get('run_id')}")
        if record.get("session"):
            parts.append(f"session={record.get('session')}")
        return ", ".join(parts) if parts else "unknown holder"

    def acquire(self) -> None:
        self.session_root.mkdir(parents=True, exist_ok=True)
        self.session_dir.mkdir(parents=True, exist_ok=True)
        os.chmod(self.session_dir, 0o700)
        self.fd = os.open(self.lock_path, os.O_RDWR | os.O_CREAT, 0o600)
        try:
            fcntl.flock(self.fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError as error:
            holder = self._holder_description()
            os.close(self.fd)
            self.fd = None
            raise HarnessError(f"clipboard lease held by another run ({holder})") from error
        # A free lock with a leftover record is a crashed run with a reused
        # --run-id; rotate the per-run record out of the way.
        record_path = self.session_dir / "lease.json"
        if record_path.exists():
            record_path.rename(self.session_dir / f"lease.stale.{int(time.time())}.json")
        self.holder_record = {
            "schema": 1,
            "pid": os.getpid(),
            "ppid": os.getppid(),
            "nonce": self.nonce,
            "run_id": self.run_id,
            "session": str(self.session_dir),
            "host": platform.node(),
            "process_start_time": process_start_time(os.getpid()),
            "started_mono": time.monotonic(),
            "started_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        }
        # The publisher/receiver contract reads this record from the session dir.
        atomic_write_json(record_path, self.holder_record, mode=0o600)
        # Run-independent record used only to report who holds the shared lock.
        atomic_write_json(self.holder_path, self.holder_record, mode=0o600)

    def release(self) -> None:
        if self.fd is None:
            return
        try:
            fcntl.flock(self.fd, fcntl.LOCK_UN)
        finally:
            os.close(self.fd)
            self.fd = None
        # Remove our holder record, but never someone else's.
        try:
            record = read_json(self.holder_path)
            if record.get("nonce") == self.nonce:
                self.holder_path.unlink()
        except Exception:
            pass


# ---------------------------------------------------------------------------
# Publisher
# ---------------------------------------------------------------------------
class Publisher:
    def __init__(self, path: Path, session_dir: Path):
        self.path = path
        self.session_dir = session_dir
        self.read_back_calls = 0

    def _base(self):
        return [str(self.path), "--session", str(self.session_dir)]

    def read_back(self, timeout_s: float):
        if not self.path.is_file():
            raise HarnessError(f"publisher binary not found: {self.path}")
        self.read_back_calls += 1
        try:
            result = run_command(self._base() + ["--read-back"], timeout_s)
        except subprocess.TimeoutExpired as error:
            raise HarnessError("publisher --read-back timed out") from error
        if result.returncode != PUB_OK:
            raise HarnessError(f"publisher --read-back failed ({result.returncode}): {result.stderr.strip()}")
        return parse_json_stdout(result.stdout, "publisher --read-back")

    def snapshot(self, snapshot_path: Path, timeout_s: float):
        """Capture the general pasteboard; an incomplete snapshot is never accepted.

        The publisher's own `--allow-partial-snapshot` flag is intentionally not
        passed: the driver refuses publication on any incomplete snapshot, so
        asking the publisher to exit 0 for one would only hide the refusal.
        """
        cmd = self._base() + ["--snapshot", str(snapshot_path),
                              "--blob-dir", str(snapshot_path) + ".d"]
        result = run_command(cmd, timeout_s)
        manifest = None
        if snapshot_path.is_file():
            try:
                manifest = read_json(snapshot_path)
            except Exception:
                manifest = None
        return result, manifest

    def restore(self, snapshot_path: Path, expect_change_count: int, timeout_s: float):
        cmd = self._base() + ["--restore", str(snapshot_path),
                              "--blob-dir", str(snapshot_path) + ".d",
                              "--expect-change-count", str(expect_change_count)]
        return run_command(cmd, timeout_s)

    def spawn_publish(self, fixture_manifest: Path, fixture_id: str, nonce: str,
                      ready_path: Path, hold_until: Path, hold_timeout_ms: int,
                      stdout_path: Path):
        cmd = self._base() + [
            "--publish",
            "--fixture-manifest", str(fixture_manifest),
            "--fixture", fixture_id,
            "--nonce", nonce,
            "--ready-path", str(ready_path),
            "--hold-until", str(hold_until),
            "--hold-timeout-ms", str(hold_timeout_ms),
        ]
        stdout_path.parent.mkdir(parents=True, exist_ok=True)
        log = stdout_path.open("w", encoding="utf-8")
        try:
            return subprocess.Popen(cmd, stdout=log, stderr=subprocess.STDOUT, text=True), log
        except Exception:
            log.close()
            raise


# ---------------------------------------------------------------------------
# Harness
# ---------------------------------------------------------------------------
class Harness:
    def __init__(self, args, manifest: Manifest, cases, run_id: str):
        self.args = args
        self.manifest = manifest
        self.cases = cases
        self.run_id = run_id
        self.evidence_dir = Path(args.evidence).expanduser().resolve() / run_id
        self.results_path = self.evidence_dir / "results.jsonl"
        self.session_root = Path(args.session_root).expanduser().resolve()
        self.session_dir = self.session_root / run_id
        self.publisher = Publisher(Path(args.publisher).expanduser().resolve(), self.session_dir)
        self.receiver = Path(args.receiver).expanduser().resolve()
        self.source_dir = Path(args.source_dir).expanduser().resolve()
        self.lease = Lease(self.session_root, self.session_dir, uuid.uuid4().hex, run_id)
        self.publisher_process = None
        self.publisher_log = None
        self.last_change_count = None
        self.snapshot_path = self.session_dir / "snapshot" / "manifest.json"
        self.snapshot_complete = False
        self.snapshot_attempted = False
        self.snapshot_captured = False
        self.snapshot_restorable = False
        self.snapshot_retained = False
        self.restore_status = "restore-skipped:no-publication"
        self.restore_attempts = 0
        self.clipboard_changed = False
        self.opted_in = False
        self.opt_in_source = None
        self.refusal_reason = None
        self.identity = self._collect_identity()

    # -- identity ----------------------------------------------------------
    def _collect_identity(self):
        receiver_sha = sha256_file(self.receiver) if self.receiver.is_file() else None
        publisher_sha = sha256_file(self.publisher.path) if self.publisher.path.is_file() else None
        source_sha = None
        source_dirty = None
        if (self.source_dir / ".git").exists():
            try:
                source_sha = run_command(["git", "-C", str(self.source_dir), "rev-parse", "HEAD"], 10).stdout.strip()
                status = run_command(["git", "-C", str(self.source_dir), "status", "--porcelain"], 10).stdout
                source_dirty = bool(status.strip())
            except Exception:
                pass
        os_version = platform.platform()
        if sys.platform == "darwin":
            try:
                os_version = "macOS " + run_command(["sw_vers", "-productVersion"], 5).stdout.strip()
            except Exception:
                pass
        app_version = os.environ.get("VACARDS_APP_VERSION")
        if not app_version:
            plist_path = Path(self.args.app_plist)
            if plist_path.is_file():
                try:
                    with plist_path.open("rb") as handle:
                        plist = plistlib.load(handle)
                    app_version = f"{plist.get('CFBundleShortVersionString', '?')} ({plist.get('CFBundleVersion', '?')})"
                except Exception:
                    app_version = None
        dependency_identity = None
        if os.environ.get("VACARDS_DEP_IDENTITY"):
            try:
                dependency_identity = json.loads(os.environ["VACARDS_DEP_IDENTITY"])
            except json.JSONDecodeError:
                dependency_identity = {"raw": os.environ["VACARDS_DEP_IDENTITY"]}
        return {
            "source_sha": source_sha,
            "source_dirty": source_dirty,
            "source_dir": str(self.source_dir),
            "binary": {
                "receiver_path": str(self.receiver),
                "receiver_sha256": receiver_sha,
                "publisher_path": str(self.publisher.path),
                "publisher_sha256": publisher_sha,
            },
            "os": {"platform": platform.platform(), "os_version": os_version, "machine": platform.machine()},
            "app_version": app_version or "source-build",
            "dependency_identity": dependency_identity,
        }

    # -- records -----------------------------------------------------------
    def base_record(self, case: dict, outcome: str, reason=None, started=None):
        fixture = self.manifest.fixtures[case["fixture"]]
        return {
            "schema": 1,
            "run_id": self.run_id,
            "requirement_id": case["requirement"],
            "case_id": case["id"],
            "title": case.get("title", ""),
            "outcome": outcome,
            "failure": {"class": fixture["failure_class"], "reason": reason},
            **self.identity,
            "fixture": {
                "id": fixture["id"],
                "file": str(self.manifest.dir / fixture["file"]),
                "sha256": fixture["sha256"],
                "bytes": fixture["bytes"],
                "transport_bytes": fixture["transport_bytes"],
            },
            "publisher": {},
            "expected": fixture["expect"],
            "actual": None,
            "duration_ms": None if started is None else int((time.monotonic() - started) * 1000),
            "started_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
            "finished_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
            "artifacts": [],
            "clipboard_protection": {
                "snapshot_complete": self.snapshot_complete,
                "restore": self.restore_status,
                "publication_change_count": self.last_change_count,
            },
            "command": {},
        }

    def emit(self, record: dict) -> None:
        append_jsonl(self.results_path, record)

    # -- run ---------------------------------------------------------------
    def run(self) -> int:
        self.evidence_dir.mkdir(parents=True, exist_ok=True)
        os.chmod(self.evidence_dir, 0o755)
        try:
            self.lease.acquire()
        except HarnessError as error:
            self.snapshot_attempted = False
            self.emit_environment_failure(str(error))
            return EXIT_FAIL

        try:
            if not self._take_snapshot():
                return EXIT_FAIL
            for case in self.cases:
                if self.clipboard_changed:
                    record = self.base_record(case, "env-blocked", "clipboard-changed")
                    record["clipboard_protection"]["restore"] = self.restore_status
                    self.emit(record)
                    continue
                record = self._run_case(case)
                self.emit(record)
            return self._final_exit_code()
        finally:
            self._restore_and_cleanup()
            self._finalize_records()

    def emit_environment_failure(self, reason: str) -> None:
        for case in self.cases:
            record = self.base_record(case, "env-blocked", reason)
            self.emit(record)

    def _take_snapshot(self) -> bool:
        """Snapshot the user's clipboard and prove a restore is possible.

        Publication may only start once the snapshot is complete AND every blob
        it lists has been read back with a matching length and hash. An
        incomplete or unrestorable snapshot refuses the whole run (env-blocked
        records plus a non-zero exit): there is no flag that trades the user's
        clipboard for a test run.
        """
        self.snapshot_attempted = True
        result = None
        snapshot = None
        try:
            result, snapshot = self.publisher.snapshot(self.snapshot_path,
                                                       self.args.publisher_timeout_ms / 1000.0)
        except subprocess.TimeoutExpired:
            pass
        except OSError as error:
            # A missing/unexecutable publisher must be an explicit environment
            # failure, not an uncaught traceback.
            reason = f"snapshot-spawn-failed:{error}"
            self.refusal_reason = reason
            self.emit_environment_failure(reason)
            return False
        if snapshot is None and self.snapshot_path.is_file():
            # A timed-out or crashed publisher may still have written a partial
            # manifest: read it so retention is decided on the real capture.
            try:
                snapshot = read_json(self.snapshot_path)
            except Exception:
                snapshot = None
        self.snapshot_captured = snapshot is not None
        if snapshot is not None:
            self.snapshot_complete = bool(snapshot.get("complete"))
        failure = None
        if result is None:
            failure = "snapshot-timeout"
        elif result.returncode != PUB_OK:
            detail = f"publisher-exit-{result.returncode}"
            if snapshot is None:
                detail += ":snapshot-manifest-missing"
            elif not self.snapshot_complete:
                detail += ":snapshot-incomplete"
            stderr = (result.stderr or "").strip().splitlines()
            if stderr:
                detail += ":" + stderr[-1].strip()[:200]
            failure = f"snapshot-failed:{detail}"
        elif snapshot is None:
            failure = "snapshot-manifest-missing"
        if failure is None and not self.snapshot_complete:
            failure = "snapshot-failed:snapshot-incomplete"
        if failure is None:
            # Prepare the restore BEFORE the pasteboard is replaced: a missing or
            # hash-mismatched blob must stop the run while the user's clipboard is
            # still intact, not surface at restore time.
            problems = validate_snapshot_restore_data(snapshot, Path(str(self.snapshot_path) + ".d"))
            if problems:
                failure = "snapshot-unrestorable:" + ",".join(problems[:6])
        if failure is None:
            self.snapshot_restorable = True
        refusal = publication_refusal(self.snapshot_complete, failure)
        if refusal is not None:
            # Never discard the reason: a bare "snapshot-incomplete" hid a
            # publisher crash, a refusal, and a genuine incomplete snapshot.
            self.refusal_reason = refusal
            # A captured-but-unusable snapshot may still hold the user's data
            # (e.g. a truncated blob): report it as unsafe to delete so the
            # session and its blobs survive for manual recovery.
            self.restore_status = restore_status_without_publication(
                self.snapshot_captured, self.snapshot_complete, self.snapshot_restorable)
            self.emit_environment_failure(refusal)
            return False
        return True

    def _run_case(self, case: dict) -> dict:
        started = time.monotonic()
        case_dir = self.evidence_dir / "cases" / case["id"]
        case_dir.mkdir(parents=True, exist_ok=True)
        fixture = self.manifest.fixtures[case["fixture"]]
        nonce = uuid.uuid4().hex
        record = self.base_record(case, "fail", None, started)
        record["artifacts"] = [f"cases/{case['id']}/expect.json"]

        expect = self.manifest.expect_document(case)
        expect["nonce"] = nonce
        expect["session"] = str(self.session_dir)
        atomic_write_json(case_dir / "expect.json", expect, mode=0o644)

        ready_path = self.session_dir / "pub" / "ready.json"
        release_path = self.session_dir / "pub" / "release"
        for path in (ready_path, release_path):
            if path.exists():
                path.unlink()
        atomic_write_json(self.session_dir / "pub" / "request.json", {
            "nonce": nonce,
            "case_id": case["id"],
            "fixture": fixture["id"],
            "publish": fixture["publish"]["macos"],
            "required_formats": fixture["required_formats"],
            "transport_bytes": fixture["transport_bytes"],
        }, mode=0o600)

        # 1. Publish and wait for the explicit readiness artifact (never a sleep).
        process = None
        log = None
        try:
            try:
                process, log = self.publisher.spawn_publish(
                    self.manifest.path, fixture["id"], nonce, ready_path, release_path,
                    self.args.hold_timeout_ms, case_dir / "publisher.log")
            except OSError as error:
                record["outcome"] = "env-blocked"
                record["failure"]["reason"] = f"publisher-spawn-failed:{error}"
                return record
            self.publisher_process, self.publisher_log = process, log
            ready, ready_error = self._wait_ready(process, ready_path, nonce)
            if ready is None:
                reason = ready_error
                if process.poll() is not None:
                    reason = f"publisher-exit-{process.returncode}:{ready_error}"
                record["outcome"] = "env-blocked"
                record["failure"]["reason"] = reason
                return record
            record["publisher"] = {
                "pid": ready.get("pid"),
                "backend": ready.get("backend"),
                "requested_utis": ready.get("requested_utis"),
                "formats_observed": ready.get("formats_observed"),
                "representations": ready.get("representations"),
                "change_count_before": ready.get("change_count_before"),
                "change_count": ready.get("change_count"),
                "fixture_sha256": ready.get("fixture_sha256"),
                "publish_as": ready.get("publish_as"),
                "published_utc": ready.get("published_utc"),
            }
            # Keep the readiness artifact with the evidence: the session dir is
            # deleted at the end of the run.
            atomic_write_json(case_dir / "publication.json", ready, mode=0o644)
            self.last_change_count = ready.get("change_count")
            exactness = self._publication_exactness(fixture, ready)
            if exactness is not None:
                record["outcome"] = "env-blocked"
                record["failure"]["reason"] = exactness
                return record

            # 2. Fresh receiver process for this case.
            outcome, details = self._run_receiver(case, case_dir)
            record.update(details)
            record["outcome"] = outcome
            if outcome == "pass":
                record["failure"]["reason"] = None
        finally:
            self._release_publisher(release_path)

        # 3. Ownership check: any change means a third party wrote the clipboard.
        try:
            read_back = self.publisher.read_back(self.args.publisher_timeout_ms / 1000.0)
            if self.last_change_count is not None and read_back.get("change_count") != self.last_change_count:
                self.clipboard_changed = True
                record["clipboard_protection"]["clipboard_changed_after_receiver"] = True
        except HarnessError:
            self.clipboard_changed = True
            record["clipboard_protection"]["clipboard_changed_after_receiver"] = True

        record["duration_ms"] = int((time.monotonic() - started) * 1000)
        record["finished_utc"] = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())
        record["clipboard_protection"]["restore"] = self.restore_status
        record["clipboard_protection"]["publication_change_count"] = self.last_change_count
        return record

    def _wait_ready(self, process, ready_path: Path, nonce: str):
        deadline = time.monotonic() + self.args.ready_timeout_ms / 1000.0
        last_error = "ready-artifact-timeout"
        while time.monotonic() < deadline:
            exited = process.poll() is not None
            if ready_path.is_file():
                try:
                    ready = read_json(ready_path)
                except Exception as error:
                    last_error = f"ready-artifact-unreadable:{error}"
                    ready = None
                if ready is not None:
                    if ready.get("nonce") != nonce:
                        last_error = "ready-artifact-nonce-mismatch"
                    elif not isinstance(ready.get("pid"), int):
                        last_error = "ready-artifact-missing-pid"
                    else:
                        try:
                            os.kill(ready["pid"], 0)
                        except OSError:
                            last_error = "publisher-not-alive-after-ready"
                        else:
                            return ready, None
            if exited:
                # The publisher only exits before the release file when it failed.
                return None, last_error if last_error != "ready-artifact-timeout" else "publisher-exited-before-ready"
            time.sleep(0.025)
        return None, last_error

    @staticmethod
    def _publication_exactness(fixture: dict, ready: dict):
        """Reject a publication that does not exactly match the manifest.

        Every declared representation must be observed, a raw-data payload must
        not be published through the string spelling behind the manifest's back
        (GDK would prefer it and never see the bytes), and a stalled
        representation must be reported as stalled by the publisher — otherwise
        the oracle would believe it exercised the lazy path when it did not.
        """
        representations = Manifest.macos_representations(fixture)
        observed = ready.get("formats_observed") or []
        for rep in representations:
            if rep["uti"] not in observed:
                return f"publication-not-exact:missing:{rep['uti']}"
        string_spellings = {rep["uti"] for rep in representations if rep["as"] == "string"}
        for rep in representations:
            if rep["as"] == "data" and rep["uti"] != "public.utf8-plain-text":
                if "public.utf8-plain-text" in observed and "public.utf8-plain-text" not in string_spellings:
                    return "publication-not-exact:unexpected-string-spelling"
        reported = ready.get("representations")
        if not isinstance(reported, list):
            if any(rep["stall"] for rep in representations):
                return "publication-not-exact:stall-not-reported"
            return None
        by_uti = {entry.get("uti"): entry for entry in reported if isinstance(entry, dict)}
        for rep in representations:
            entry = by_uti.get(rep["uti"])
            if entry is None:
                return f"publication-not-exact:representation-not-reported:{rep['uti']}"
            if bool(entry.get("stall")) != rep["stall"]:
                return f"publication-not-exact:stall-mismatch:{rep['uti']}"
            if entry.get("sha256") and entry["sha256"] != rep["sha256"]:
                return f"publication-not-exact:sha256-mismatch:{rep['uti']}"
        return None

    def _release_publisher(self, release_path: Path) -> None:
        try:
            release_path.parent.mkdir(parents=True, exist_ok=True)
            release_path.touch()
        except OSError:
            pass
        process = self.publisher_process
        if process is not None:
            try:
                process.wait(timeout=TERMINATE_GRACE_S)
            except subprocess.TimeoutExpired:
                terminate_process(process)
        if self.publisher_log is not None:
            self.publisher_log.close()
        self.publisher_process = None
        self.publisher_log = None

    def _run_receiver(self, case: dict, case_dir: Path):
        fixture = self.manifest.fixtures[case["fixture"]]
        result_path = case_dir / "result.json"
        if result_path.exists():
            result_path.unlink()
        gtest_path = case_dir / "gtest.json"
        if gtest_path.exists():
            gtest_path.unlink()
        log_path = case_dir / "receiver.log"

        env = dict(os.environ)
        env.update({
            "VACARDS_CLIP_SESSION": str(self.session_dir),
            "VACARDS_CLIP_CASE": case["id"],
            "VACARDS_CLIP_EXPECT": str(case_dir / "expect.json"),
            "VACARDS_CLIP_RESULT": str(result_path),
            "VACARDS_CLIP_REQUIRE_LEASE": env.get("VACARDS_CLIP_REQUIRE_LEASE", "1"),
            "INKSCAPE_TEST_GUI": "1",
        })
        profile_dir = case_dir / "profile"
        profile_dir.mkdir(parents=True, exist_ok=True)
        env["INKSCAPE_PROFILE_DIR"] = str(profile_dir)

        command = [str(self.receiver), f"--gtest_output=json:{gtest_path}"]
        details = {
            "command": {"receiver": command, "env_contract": sorted(k for k in env if k.startswith("VACARDS_CLIP_")
                                                                    or k.startswith("INKSCAPE_"))},
            "exit_status": {"receiver": None, "timed_out": False},
            "artifacts": [f"cases/{case['id']}/expect.json", f"cases/{case['id']}/receiver.log",
                          f"cases/{case['id']}/gtest.json", f"cases/{case['id']}/result.json",
                          f"cases/{case['id']}/publisher.log", f"cases/{case['id']}/publication.json"],
        }
        if not self.receiver.is_file():
            # Explicit environment failure instead of an uncaught FileNotFoundError.
            details["failure"] = {"class": fixture["failure_class"],
                                  "reason": f"receiver-missing:{self.receiver}"}
            return "env-blocked", details
        with log_path.open("w", encoding="utf-8") as log:
            try:
                process = subprocess.run(command, capture_output=True, text=True, env=env,
                                         timeout=self.args.receiver_timeout_ms / 1000.0, check=False)
                log.write(process.stdout)
                log.write(process.stderr)
                returncode = process.returncode
                timed_out = False
            except subprocess.TimeoutExpired as error:
                log.write((error.stdout or "") if isinstance(error.stdout, str) else "")
                log.write((error.stderr or "") if isinstance(error.stderr, str) else "")
                returncode = None
                timed_out = True
            except OSError as error:
                log.write(f"receiver spawn failed: {error}\n")
                log.flush()
                details["failure"] = {"class": fixture["failure_class"],
                                      "reason": f"receiver-spawn-failed:{error}"}
                return "env-blocked", details

        details["exit_status"] = {"receiver": returncode, "timed_out": timed_out}
        if timed_out:
            return "fail", {**details, "failure": {"class": fixture["failure_class"],
                                                   "reason": "receiver-timeout"}}
        if returncode != 0:
            return "fail", {**details, "failure": {"class": fixture["failure_class"],
                                                   "reason": f"receiver-exit-{returncode}"}}

        gtest_error = self._validate_gtest(gtest_path)
        if gtest_error:
            return "fail", {**details, "failure": {"class": fixture["failure_class"], "reason": gtest_error}}

        if not result_path.is_file():
            return "fail", {**details, "failure": {"class": fixture["failure_class"],
                                                   "reason": "result-json-missing"}}
        try:
            result = read_json(result_path)
        except Exception as error:
            return "fail", {**details, "failure": {"class": fixture["failure_class"],
                                                   "reason": f"result-json-unreadable:{error}"}}
        details["actual"] = result.get("actual")
        details["receiver_result"] = {
            "outcome": result.get("outcome"),
            "failure_class_expected": result.get("failure_class_expected"),
            "remote_preconditions": result.get("remote_preconditions"),
            "checks": result.get("checks"),
            "duration_ms": result.get("duration_ms"),
        }
        if result.get("outcome") != "pass":
            checks = result.get("checks") or []
            failed = ",".join(check.get("name", "?") for check in checks if not check.get("ok"))
            return "fail", {**details, "failure": {"class": fixture["failure_class"],
                                                   "reason": f"receiver-assertions-failed:{failed or 'unknown'}"}}
        if result.get("failure_class_expected") != fixture["failure_class"]:
            return "fail", {**details, "failure": {"class": fixture["failure_class"],
                                                   "reason": "failure-class-mismatch"}}
        return "pass", {**details, "failure": {"class": fixture["failure_class"], "reason": None}}

    @staticmethod
    def _gtest_testcases(data: dict):
        """Testcase dicts from a gtest JSON report.

        Real gtest nests them as testsuites[].testsuite[]; a flat top-level
        testsuite list (and a dict variant) is tolerated so a layout change cannot
        silently turn a skipped test into a pass.
        """
        testcases = []

        def collect_suite(suite):
            if not isinstance(suite, dict):
                return
            nested = suite.get("testsuite")
            if isinstance(nested, list):
                for entry in nested:
                    if not isinstance(entry, dict):
                        continue
                    if isinstance(entry.get("testsuite"), (list, dict)):
                        collect_suite(entry)
                    else:
                        testcases.append(entry)
            elif isinstance(nested, dict):
                collect_suite(nested)

        top = data.get("testsuites")
        if isinstance(top, list):
            for suite in top:
                collect_suite(suite)
        elif isinstance(top, dict):
            collect_suite(top)
        flat = data.get("testsuite")
        if isinstance(flat, list):
            for entry in flat:
                if not isinstance(entry, dict):
                    continue
                if isinstance(entry.get("testsuite"), (list, dict)):
                    collect_suite(entry)
                else:
                    testcases.append(entry)
        elif isinstance(flat, dict):
            collect_suite(flat)
        return testcases

    @staticmethod
    def _validate_gtest(gtest_path: Path):
        if not gtest_path.is_file():
            return "gtest-json-missing"
        try:
            data = read_json(gtest_path)
        except Exception as error:
            return f"gtest-json-unreadable:{error}"
        if not isinstance(data, dict):
            return "gtest-json-not-an-object"
        if data.get("tests") != 1:
            return f"gtest-tests-{data.get('tests')}"
        if data.get("failures"):
            return f"gtest-failures-{data.get('failures')}"
        if data.get("errors"):
            return f"gtest-errors-{data.get('errors')}"
        testcases = Harness._gtest_testcases(data)
        if not testcases:
            return "gtest-testcases-missing"
        for testcase in testcases:
            if testcase.get("result") == "SKIPPED" or testcase.get("status") == "SKIPPED":
                return "gtest-test-skipped"
        return None

    def _final_exit_code(self) -> int:
        outcomes = [record["outcome"] for record in self._read_records()]
        return EXIT_OK if outcomes and all(outcome == "pass" for outcome in outcomes) else EXIT_FAIL

    def _read_records(self):
        records = []
        if not self.results_path.is_file():
            return records
        with self.results_path.open("r", encoding="utf-8") as handle:
            for line in handle:
                line = line.strip()
                if line:
                    records.append(json.loads(line))
        return records

    def _finalize_records(self) -> None:
        """Apply the run-final clipboard restoration outcome to every record.

        Per-case records are emitted as soon as the case ends, before the run's
        single restore; rewriting here keeps the durable results.jsonl honest
        about what happened to the user's clipboard.
        """
        records = self._read_records()
        if not records:
            return
        for record in records:
            protection = record.setdefault("clipboard_protection", {})
            protection["restore"] = self.restore_status
            protection["restore_attempts"] = self.restore_attempts
            protection["publication_change_count"] = self.last_change_count
            protection["snapshot_complete"] = self.snapshot_complete
            protection["snapshot_retained"] = self.snapshot_retained
        tmp = self.results_path.with_name(self.results_path.name + f".tmp.{os.getpid()}")
        with tmp.open("w", encoding="utf-8") as handle:
            for record in records:
                handle.write(json.dumps(record, ensure_ascii=False, sort_keys=True) + "\n")
            handle.flush()
            os.fsync(handle.fileno())
        os.replace(tmp, self.results_path)

    def _restore_and_cleanup(self) -> None:
        try:
            self._release_publisher(self.session_dir / "pub" / "release")
        except Exception as error:  # cleanup must never mask the run result
            self.restore_status = f"restore-skipped:publisher-cleanup-failed:{error}"
        try:
            if self.snapshot_attempted and self.last_change_count is not None:
                self._restore()
            elif self.last_change_count is None:
                # No harness publication: nothing was replaced. This is only the
                # safe no-publication status when the snapshot itself is complete;
                # a captured-but-incomplete snapshot must stay flagged (and
                # retained) because there is no verified recovery copy.
                self.restore_status = restore_status_without_publication(self.snapshot_captured,
                                                                         self.snapshot_complete,
                                                                         self.snapshot_restorable)
        except Exception as error:
            self.restore_status = f"restore-skipped:restore-error:{error}"
        finally:
            self.lease.release()
            # The snapshot may be the only copy of the user's data after an
            # incomplete snapshot or a failed restore: keep the 0700 session dir
            # and say where it is. It is only deleted when the clipboard was
            # verified restored, never replaced, or is owned by a newer user copy.
            retained = self.snapshot_captured and restore_snapshot_retained(self.restore_status)
            self.snapshot_retained = retained
            if retained:
                sys.stderr.write(
                    "external clipboard harness: restore did not complete "
                    f"({self.restore_status}); the user's snapshot was retained at "
                    f"{self.session_dir} (0700, blobs 0600). Remove it after manual recovery; "
                    "it contains clipboard bytes.\n")
            else:
                shutil.rmtree(self.session_dir, ignore_errors=True)
                # Never leave clipboard bytes behind: the snapshot lives only here.
                shutil.rmtree(Path(str(self.snapshot_path) + ".d"), ignore_errors=True)

    def _read_back_change_count(self):
        try:
            current = self.publisher.read_back(self.args.publisher_timeout_ms / 1000.0)
        except (HarnessError, subprocess.TimeoutExpired) as error:
            return None, str(error)
        return current.get("change_count"), None

    def _restore(self) -> None:
        """Restore the snapshot with a bounded retry and honest ownership refusal.

        The publisher prepares and verifies the restore objects before it clears
        the pasteboard; this driver never writes unless the changeCount still
        matches its own last publication, and it never retries over a pasteboard
        that changed in between (the retry loop re-runs the same precheck).
        """
        attempt = 0
        last_returncode = None
        while True:
            current, error = self._read_back_change_count()
            if error is None:
                if last_returncode in RETRYABLE_RESTORE_CODES and current != self.last_change_count:
                    # The failed write may itself have cleared/partly written the
                    # pasteboard, so a changeCount change here cannot be told
                    # apart from a user copy. Never write again, and retain the
                    # snapshot: the pasteboard may no longer hold the user's data.
                    self.restore_status = f"restore-skipped:restore-failed-{last_returncode}"
                    return
                proceed, status = restore_precheck(self.snapshot_complete, self.last_change_count, current)
            else:
                proceed, status = False, f"restore-skipped:read-back-failed:{error}"
            if not proceed:
                self.restore_status = status
                return
            attempt += 1
            self.restore_attempts = attempt
            try:
                result = self.publisher.restore(self.snapshot_path, int(self.last_change_count),
                                                self.args.publisher_timeout_ms / 1000.0)
            except subprocess.TimeoutExpired:
                self.restore_status = "restore-skipped:restore-timeout"
                return
            except OSError as error:
                self.restore_status = f"restore-skipped:restore-error:{error}"
                return
            action, status = restore_outcome(result.returncode, attempt)
            self.restore_status = status
            if action == "done":
                return
            last_returncode = result.returncode
            # retry: the loop re-reads changeCount first, so a user copy between
            # the two attempts stops the retry (see the guard above).

    # -- summary -----------------------------------------------------------
    def write_summary(self) -> None:
        outcomes = {record["case_id"]: record["outcome"] for record in self._read_records()}
        counts = summarize_outcomes(outcomes)
        atomic_write_json(self.evidence_dir / "summary.json", {
            "run_id": self.run_id,
            # Truthful opt-in state: the refusal path is env-blocked, not a run.
            "opt_in": bool(self.opted_in),
            "opt_in_source": self.opt_in_source,
            "snapshot_complete": self.snapshot_complete,
            "snapshot_attempted": self.snapshot_attempted,
            "snapshot_captured": self.snapshot_captured,
            "snapshot_restorable": self.snapshot_restorable,
            "snapshot_retained": self.snapshot_retained,
            "snapshot_dir": str(self.session_dir) if self.snapshot_retained else None,
            "refusal": self.refusal_reason,
            "restore": self.restore_status,
            "restore_attempts": self.restore_attempts,
            "counts": counts,
            "outcomes": outcomes,
            **{key: self.identity[key] for key in ("source_sha", "source_dirty", "os", "app_version")},
        }, mode=0o644)
        latest = self.evidence_dir.parent / "LATEST"
        tmp = latest.with_name(latest.name + f".tmp.{os.getpid()}")
        tmp.write_text(str(self.evidence_dir) + "\n", encoding="utf-8")
        os.replace(tmp, latest)


# ---------------------------------------------------------------------------
# Dry run
# ---------------------------------------------------------------------------
def dry_run(args, manifest: Manifest, cases) -> int:
    plan = {
        "dry_run": True,
        "clipboard_touched": False,
        "manifest": str(manifest.path),
        "opt_in_required": "--allow-clipboard-takeover or VACARDS_CLIP_ALLOW=1",
        "publisher": None,
        "receiver": None,
        "cases": [],
    }
    publisher = Path(args.publisher).expanduser().resolve()
    receiver = Path(args.receiver).expanduser().resolve()
    plan["publisher"] = {"path": str(publisher), "exists": publisher.is_file(),
                         "sha256": sha256_file(publisher) if publisher.is_file() else None}
    plan["receiver"] = {"path": str(receiver), "exists": receiver.is_file(),
                        "sha256": sha256_file(receiver) if receiver.is_file() else None}
    for case in cases:
        fixture = manifest.fixtures[case["fixture"]]
        plan["cases"].append({
            "id": case["id"],
            "requirement": case["requirement"],
            "route": case["route"],
            "fixture": fixture["id"],
            "failure_class": fixture["failure_class"],
            "required_formats": fixture["required_formats"],
            "publish": {"representations": manifest.macos_representations(fixture)},
            "transport_bytes": fixture["transport_bytes"],
            "expected": fixture["expect"],
        })
    json.dump(plan, sys.stdout, ensure_ascii=False, indent=2, sort_keys=True)
    sys.stdout.write("\n")
    problems = []
    if not publisher.is_file():
        problems.append("publisher binary missing")
    if not receiver.is_file():
        problems.append("receiver binary missing")
    if problems:
        sys.stderr.write("dry-run problems: " + "; ".join(problems) + "\n")
        return EXIT_FAIL
    return EXIT_OK


# ---------------------------------------------------------------------------
# Protocol self-test (pure: never starts the publisher or receiver, never
# touches the pasteboard)
# ---------------------------------------------------------------------------
PROTOCOL_SELF_TEST_CHECKS = (
    "paths.session_and_run_independent_lease",
    "lease.two_processes_contend_on_one_lock",
    "wrapper.skip_exit_code_mapping",
    "manifest.representations_and_expect_hash_binding",
    "summary.accounting_and_truthful_opt_in",
    "restore.decision_with_injected_failures",
    "snapshot.refusal_and_retention",
)


def _self_test_namespace(root: Path):
    """Minimal args for Harness path/summary checks; no binaries are executed."""
    return argparse.Namespace(
        evidence=str(root / "evidence"),
        session_root=str(root / "sessions"),
        publisher=str(root / "no-such-publisher"),
        receiver=str(root / "no-such-receiver"),
        source_dir=str(root),
        app_plist=str(root / "no-such-app.plist"),
        publisher_timeout_ms=1000,
        receiver_timeout_ms=1000,
        ready_timeout_ms=1000,
        hold_timeout_ms=1000,
    )


def _self_test_paths() -> None:
    fixtures = Path(__file__).resolve().parent / "fixtures"
    manifest = Manifest(fixtures)
    case = manifest.select("E01")[0]
    with tempfile.TemporaryDirectory(prefix="vacards-clip-selftest-") as tmp:
        root = Path(tmp)
        session_root = root / "sessions"
        lease_a = Lease(session_root, session_root / "run-a", "nonce-a", "run-a")
        lease_b = Lease(session_root, session_root / "run-b", "nonce-b", "run-b")
        assert lease_a.lock_path == lease_b.lock_path, "the lease lock path depends on the run id"
        assert lease_a.lock_path == session_root / Lease.LOCK_NAME
        assert lease_a.session_dir != lease_b.session_dir
        assert lease_a.session_dir.parent == session_root
        harness = Harness(_self_test_namespace(root), manifest, [case], "run-selftest")
        assert harness.session_dir == (session_root / "run-selftest").resolve()
        assert harness.evidence_dir == (root / "evidence" / "run-selftest").resolve()
        assert harness.lease.lock_path == (session_root / Lease.LOCK_NAME).resolve()
        assert harness.lease.lock_path == harness.session_dir.parent / Lease.LOCK_NAME
        assert harness.snapshot_path.parent.parent == harness.session_dir


def _self_test_lease_child(args) -> int:
    """Hidden subprocess mode used by the contention check; no clipboard access."""
    root = Path(args.self_test_session_root)
    run_id = "selftest-" + uuid.uuid4().hex[:8]
    lease = Lease(root, root / run_id, uuid.uuid4().hex, run_id)
    if args.self_test_mode == "lease-hold":
        lease.acquire()
        sys.stdout.write("acquired\n")
        sys.stdout.flush()
        try:
            time.sleep(max(0, args.self_test_hold_ms) / 1000.0)
        finally:
            lease.release()
        return EXIT_OK
    try:
        lease.acquire()
    except HarnessError as error:
        sys.stdout.write("blocked:" + str(error) + "\n")
        sys.stdout.flush()
        return EXIT_OK
    lease.release()
    sys.stderr.write("contender acquired the same lock while the holder still had it\n")
    return EXIT_FAIL


def _self_test_lease_contention() -> None:
    driver = str(Path(__file__).resolve())
    holder = None
    with tempfile.TemporaryDirectory(prefix="vacards-clip-selftest-") as tmp:
        root = Path(tmp) / "sessions"
        holder = subprocess.Popen(
            [sys.executable, driver, "--self-test-mode", "lease-hold",
             "--self-test-session-root", str(root), "--self-test-hold-ms", "3000"],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        try:
            line = holder.stdout.readline().strip()
            assert line == "acquired", f"lease holder failed to start: {line!r} {holder.stderr.read()!r}"
            contender = run_command(
                [sys.executable, driver, "--self-test-mode", "lease-try",
                 "--self-test-session-root", str(root)], 30)
            assert contender.returncode == EXIT_OK, f"contender crashed: {contender.stderr}"
            assert contender.stdout.startswith("blocked:"), \
                f"a second driver acquired the same lease: {contender.stdout!r} {contender.stderr!r}"
            # Let the holder finish its bounded hold and release the lock itself.
            holder.wait(timeout=15)
        finally:
            terminate_process(holder, grace_s=5.0)
        assert holder.returncode == EXIT_OK, f"lease holder exited {holder.returncode}: {holder.stderr.read()!r}"


def _receiver_self_test_child(payload, exit_code: int = 0, print_skip: bool = False) -> str:
    """Source for a trivial python child that fakes a gtest JSON report.

    The wrapper under test never sees a real receiver here: no GTest, no GUI,
    no clipboard.
    """
    lines = ["import json,pathlib,sys", "print('fake receiver child')"]
    if print_skip:
        lines.append("print('[  SKIPPED ] FakeSuite.FakeTest')")
    if payload is not None:
        lines += [
            "arg=[a for a in sys.argv[1:] if a.startswith('--gtest_output=json:')]",
            "assert arg, 'no --gtest_output argument'",
            "path=pathlib.Path(arg[0].split('json:',1)[1])",
            "path.write_text(json.dumps(" + repr(payload) + "), encoding='utf-8')",
        ]
    lines.append(f"sys.exit({exit_code})")
    return "\n".join(lines)


def _self_test_wrapper_mapping() -> None:
    wrapper = Path(__file__).resolve().with_name("run-receiver-skip-aware.py")
    assert wrapper.is_file(), f"skip-aware wrapper missing: {wrapper}"

    def invoke(child_code: str, gtest_path: Path):
        return run_command(
            [sys.executable, str(wrapper), "--receiver", sys.executable,
             "--gtest-output", str(gtest_path), "--", "-c", child_code], 30)

    # Real gtest nests testcases as testsuites[].testsuite[]; the flat layout is
    # kept as a tolerated variant. Both must be understood: a layout mismatch
    # would make "all skipped" look like "executed".
    executed = {"tests": 1, "failures": 0, "errors": 0,
                "testsuites": [{"name": "FakeSuite", "tests": 1, "failures": 0, "errors": 0,
                                "testsuite": [{"name": "FakeTest", "status": "RUN",
                                               "result": "COMPLETED"}]}]}
    skipped_nested = {"tests": 1, "failures": 0, "errors": 0,
                      "testsuites": [{"name": "FakeSuite", "tests": 1, "failures": 0, "errors": 0,
                                      "testsuite": [{"name": "FakeTest", "status": "SKIPPED",
                                                     "result": "SKIPPED"}]}]}
    skipped_flat = {"tests": 1, "failures": 0, "errors": 0,
                    "testsuite": [{"name": "FakeTest", "status": "SKIPPED", "result": "SKIPPED"}]}
    empty = {"tests": 0, "failures": 0, "errors": 0, "testsuites": []}
    unverifiable = {"tests": 1, "failures": 0, "errors": 0}
    with tempfile.TemporaryDirectory(prefix="vacards-clip-selftest-") as tmp:
        case_dir = Path(tmp)

        result = invoke(_receiver_self_test_child(executed), case_dir / "executed.json")
        assert result.returncode == EXIT_OK, f"executed test mapped to {result.returncode}: {result.stderr}"

        result = invoke(_receiver_self_test_child(skipped_nested, print_skip=True), case_dir / "skipped-nested.json")
        assert result.returncode == EXIT_NOT_OPTED_IN, \
            f"nested all-skipped report mapped to {result.returncode}, not 77: {result.stdout}"
        assert "[  SKIPPED ]" in result.stdout, "the receiver's [ SKIPPED ] output must be passed through"

        result = invoke(_receiver_self_test_child(skipped_flat), case_dir / "skipped-flat.json")
        assert result.returncode == EXIT_NOT_OPTED_IN, \
            f"flat all-skipped report mapped to {result.returncode}, not 77"

        result = invoke(_receiver_self_test_child(empty), case_dir / "empty.json")
        assert result.returncode == EXIT_NOT_OPTED_IN, \
            f"zero executed tests mapped to {result.returncode}, not 77"

        result = invoke(_receiver_self_test_child(unverifiable), case_dir / "unverifiable.json")
        assert result.returncode == EXIT_USAGE, \
            f"a report without per-test detail mapped to {result.returncode}, not {EXIT_USAGE} (never a pass)"

        result = invoke(_receiver_self_test_child(None, exit_code=3), case_dir / "failed.json")
        assert result.returncode == 3, f"receiver exit 3 propagated as {result.returncode}"

        result = invoke(_receiver_self_test_child(None), case_dir / "no-evidence.json")
        assert result.returncode == EXIT_USAGE, \
            f"missing gtest evidence mapped to {result.returncode}, not {EXIT_USAGE} (never a pass)"

        # The driver's own gtest validation must read the real nested layout.
        nested_report = case_dir / "driver-nested.json"
        nested_report.write_text(json.dumps(skipped_nested), encoding="utf-8")
        assert Harness._validate_gtest(nested_report) == "gtest-test-skipped", \
            "the driver must reject an all-skipped nested gtest report"
        detail_less = case_dir / "driver-no-detail.json"
        detail_less.write_text(json.dumps(unverifiable), encoding="utf-8")
        assert Harness._validate_gtest(detail_less) == "gtest-testcases-missing", \
            "the driver must reject a gtest report without per-test results"
        executed_report = case_dir / "driver-executed.json"
        executed_report.write_text(json.dumps(executed), encoding="utf-8")
        assert Harness._validate_gtest(executed_report) is None, \
            "the driver must accept an executed nested gtest report"


def _self_test_manifest_binding() -> None:
    fixtures = Path(__file__).resolve().parent / "fixtures"
    manifest = Manifest(fixtures)  # the shipped manifest must satisfy every binding
    with tempfile.TemporaryDirectory(prefix="vacards-clip-selftest-") as tmp:
        work = Path(tmp) / "fixtures"
        shutil.copytree(str(fixtures), str(work))
        manifest_path = work / "manifest.json"
        original = read_json(manifest_path)

        def fixture(data, fixture_id):
            return next(entry for entry in data["fixtures"] if entry["id"] == fixture_id)

        def expect_rejected(label, mutate):
            data = json.loads(json.dumps(original))
            mutate(data)
            manifest_path.write_text(json.dumps(data), encoding="utf-8")
            try:
                Manifest(work)
            except HarnessError:
                return
            raise AssertionError(f"manifest mutation accepted: {label}")

        def expect_accepted(label, mutate):
            data = json.loads(json.dumps(original))
            mutate(data)
            manifest_path.write_text(json.dumps(data), encoding="utf-8")
            try:
                Manifest(work)
            except HarnessError as error:
                raise AssertionError(f"manifest mutation rejected: {label}: {error}") from error

        def wrong_success_hash(data):
            fixture(data, "E01_utf8_only")["expect"]["text_sha256"] = EMPTY_TEXT_SHA256

        def wrong_success_bytes(data):
            fixture(data, "E01_utf8_only")["expect"]["text_bytes"] = 3

        def wrong_literal_length(data):
            fixture(data, "E01_utf8_only")["expect"]["text_utf8"] = "short"

        def reject_with_mutation(data):
            fixture(data, "E06_embedded_nul")["expect"]["document_unchanged"] = False

        def reject_with_object(data):
            fixture(data, "E06_embedded_nul")["expect"]["object_count"] = 1

        def abort_with_text(data):
            fixture(data, "E07_over_cap")["expect"]["text_sha256"] = "0" * 64

        def legacy_to_representations(data):
            entry = fixture(data, "E01_utf8_only")
            entry["publish"]["macos"] = {"representations": [
                {"uti": "public.utf8-plain-text", "file": entry["file"], "sha256": entry["sha256"],
                 "bytes": entry["bytes"], "as": "string"}]}

        def both_forms(data):
            entry = fixture(data, "E01_utf8_only")
            entry["publish"]["macos"]["representations"] = [
                {"uti": "public.utf8-plain-text", "file": entry["file"], "sha256": entry["sha256"],
                 "bytes": entry["bytes"], "as": "string"}]

        def representation_wrong_hash(data):
            fixture(data, "E13_object_priority_svg_plain")["publish"]["macos"]["representations"][0]["sha256"] = "0" * 64

        def representation_missing_file(data):
            fixture(data, "E13_object_priority_svg_plain")["publish"]["macos"]["representations"][1]["file"] = \
                "bytes/does-not-exist.txt"

        def representation_bad_as(data):
            fixture(data, "E13_object_priority_svg_plain")["publish"]["macos"]["representations"][0]["as"] = "blob"

        def representation_not_primary(data):
            entry = fixture(data, "E13_object_priority_svg_plain")
            entry["file"] = "bytes/E01_utf8_only.txt"
            entry["sha256"] = "e7b0a3aa4d42802c49536fc7cd5ad82000fe3ce0e3a37721016b1ade9f0fd991"
            entry["bytes"] = 63

        def wrong_transport_sum(data):
            fixture(data, "E13_object_priority_svg_plain")["transport_bytes"] += 1

        def import_with_unchanged_doc(data):
            fixture(data, "E13_object_priority_svg_plain")["expect"]["document_unchanged"] = True

        def import_with_text_object(data):
            fixture(data, "E13_object_priority_svg_plain")["expect"]["object_count"] = 1

        def import_without_object_id(data):
            del fixture(data, "E13_object_priority_svg_plain")["expect"]["import_object_id"]

        def import_without_forbidden(data):
            del fixture(data, "F13_plain_svg_object_path")["expect"]["forbidden_text_utf8"]

        def import_object_id_absent_from_payload(data):
            fixture(data, "E13_object_priority_svg_plain")["expect"]["import_object_id"] = "not-in-any-payload"

        def fallback_forbidden_in_expected_text(data):
            entry = fixture(data, "R04_fallback_plain_distinct")
            marker = entry["expect"]["forbidden_text_utf8"][0]
            entry["expect"]["text_utf8"] = entry["expect"]["text_utf8"] + marker
            entry["expect"]["text_bytes"] = len(entry["expect"]["text_utf8"].encode("utf-8"))
            entry["expect"]["text_sha256"] = sha256_bytes(entry["expect"]["text_utf8"].encode("utf-8"))

        def stall_without_deadline(data):
            del fixture(data, "E08_object_target_stall")["expect"]["max_elapsed_ms"]

        def stall_not_declared(data):
            fixture(data, "E08_object_target_stall")["publish"]["macos"]["representations"][0]["stall"] = False

        tampered = work / "bytes" / "E01_utf8_only.txt"
        original_bytes = tampered.read_bytes()
        tampered.write_bytes(original_bytes + b"x")
        try:
            Manifest(work)
        except HarnessError:
            pass
        else:
            raise AssertionError("tampered fixture bytes accepted")
        tampered.write_bytes(original_bytes)

        expect_rejected("success text_sha256 not bound to the fixture bytes", wrong_success_hash)
        expect_rejected("success text_bytes not bound to the fixture bytes", wrong_success_bytes)
        expect_rejected("text_utf8 length not bound to text_bytes", wrong_literal_length)
        expect_rejected("reject class declaring a document change", reject_with_mutation)
        expect_rejected("reject class expecting an object", reject_with_object)
        expect_rejected("abort class expecting pasted text", abort_with_text)
        expect_rejected("legacy and representations form declared together", both_forms)
        expect_rejected("representation sha256 not bound to its file", representation_wrong_hash)
        expect_rejected("representation file missing", representation_missing_file)
        expect_rejected("representation with an unsupported as mode", representation_bad_as)
        expect_rejected("fixture file not among the representations", representation_not_primary)
        expect_rejected("transport_bytes not the representation transport sum", wrong_transport_sum)
        expect_rejected("import-object declaring an unchanged document", import_with_unchanged_doc)
        expect_rejected("import-object expecting a text object", import_with_text_object)
        expect_rejected("import-object without an import_object_id", import_without_object_id)
        expect_rejected("import-object without a forbidden text marker", import_without_forbidden)
        expect_rejected("import_object_id that occurs in no payload", import_object_id_absent_from_payload)
        expect_rejected("forbidden marker shared with the expected text", fallback_forbidden_in_expected_text)
        expect_rejected("stalled representation without max_elapsed_ms", stall_without_deadline)
        expect_rejected("stall fixture whose manifest does not declare a stall", stall_not_declared)
        expect_accepted("legacy utis/as form converted to representations", legacy_to_representations)
        # Restore the pristine manifest and confirm the copy is accepted again.
        manifest_path.write_text(json.dumps(original), encoding="utf-8")
        Manifest(work)

    # The publication-exactness oracle must read the normalized representation
    # list, including the stall report the lazy publisher promises.
    case = next(entry for entry in manifest.cases if entry["id"] == "E13_object_priority_svg_plain")
    exact_fixture = manifest.fixtures[case["fixture"]]
    representations = Manifest.macos_representations(exact_fixture)
    ready = {
        "formats_observed": [rep["uti"] for rep in representations],
        "representations": [{"uti": rep["uti"], "stall": rep["stall"], "sha256": rep["sha256"]}
                            for rep in representations],
    }
    assert Harness._publication_exactness(exact_fixture, ready) is None, \
        "a complete multi-representation publication must be exact"
    missing = json.loads(json.dumps(ready))
    missing["formats_observed"] = missing["formats_observed"][:-1]
    assert Harness._publication_exactness(exact_fixture, missing) is not None, \
        "a publication missing a declared representation must not be exact"
    mismatch = json.loads(json.dumps(ready))
    mismatch["representations"][0]["sha256"] = "1" * 64
    assert Harness._publication_exactness(exact_fixture, mismatch) is not None, \
        "a representation published with different bytes must not be exact"
    stall_fixture = manifest.fixtures[next(entry for entry in manifest.cases
                                           if entry["id"] == "E08_object_target_stall")["fixture"]]
    no_stall_report = {"formats_observed": [rep["uti"] for rep in Manifest.macos_representations(stall_fixture)]}
    assert Harness._publication_exactness(stall_fixture, no_stall_report) is not None, \
        "a stalled representation without a stall report must not be exact"


def _self_test_summary() -> None:
    counts = summarize_outcomes({"E01": "pass", "E02": "fail", "E04": "env-blocked", "E05": "not-run"})
    assert counts == {"total": 4, "pass": 1, "fail": 1, "env-blocked": 1, "not-run": 1,
                      "executed": 2, "skipped": 2, "other": 0}, counts
    fixtures = Path(__file__).resolve().parent / "fixtures"
    manifest = Manifest(fixtures)
    case = manifest.select("E01")[0]
    with tempfile.TemporaryDirectory(prefix="vacards-clip-selftest-") as tmp:
        root = Path(tmp)
        harness = Harness(_self_test_namespace(root), manifest, [case], "run-refusal")
        assert harness.opted_in is False, "a fresh harness must not claim opt-in"
        harness.opted_in = False
        harness.opt_in_source = None
        harness.evidence_dir.mkdir(parents=True, exist_ok=True)
        record = harness.base_record(case, "env-blocked", "not-opted-in")
        record["clipboard_protection"] = {
            "snapshot_complete": False,
            "restore": "restore-skipped:not-opted-in",
            "publication_change_count": None,
        }
        harness.emit(record)
        harness.write_summary()
        summary = read_json(harness.evidence_dir / "summary.json")
        assert summary["opt_in"] is False, "the refusal path must record opt_in=false"
        assert summary["counts"]["env-blocked"] == 1
        assert summary["counts"]["executed"] == 0
        assert summary["counts"]["skipped"] == 1
        assert summary["counts"]["total"] == 1
        assert summary["outcomes"] == {"E01": "env-blocked"}


def _self_test_restore_logic() -> None:
    assert restore_precheck(False, 5, 5) == (False, "restore-skipped:snapshot-incomplete")
    assert restore_precheck(True, None, 5) == (False, "restore-skipped:no-publication")
    assert restore_precheck(True, 5, None) == (False, "restore-skipped:read-back-failed")
    assert restore_precheck(True, 5, 6) == (False, "restore-skipped:clipboard-ownership-lost")
    assert restore_precheck(True, 5, 5) == (True, "restore-pending")
    assert restore_outcome(PUB_OK, 1) == ("done", "restored")
    assert restore_outcome(PUB_OWNERSHIP, 1) == ("done", "restore-skipped:clipboard-ownership-lost")
    assert restore_outcome(PUB_PUBLISH, 1) == ("retry", "restore-retrying:4")
    assert restore_outcome(PUB_PUBLISH, MAX_RESTORE_ATTEMPTS) == ("done", "restore-skipped:restore-failed-4")
    assert restore_outcome(PUB_SNAPSHOT, 1) == ("done", "restore-skipped:restore-failed-5")

    def simulate(publisher_codes, read_backs):
        """Mirror Harness._restore with injected publisher exits and changeCounts."""
        attempt = 0
        reads = 0
        last_returncode = None
        while True:
            current = read_backs[min(reads, len(read_backs) - 1)]
            reads += 1
            if last_returncode in RETRYABLE_RESTORE_CODES and current != 5:
                return f"restore-skipped:restore-failed-{last_returncode}"
            proceed, status = restore_precheck(True, 5, current)
            if not proceed:
                return status
            attempt += 1
            code = publisher_codes[min(attempt - 1, len(publisher_codes) - 1)]
            action, status = restore_outcome(code, attempt)
            if action == "done":
                return status
            last_returncode = code

    # transient write failure -> one retry succeeds
    assert simulate([PUB_PUBLISH, PUB_OK], [5, 5, 5]) == "restored"
    # two failed writes -> failure, snapshot must be retained
    failed = simulate([PUB_PUBLISH, PUB_PUBLISH], [5, 5, 5])
    assert failed == "restore-skipped:restore-failed-4"
    assert restore_snapshot_retained(failed) is True
    # the user's copy is already on the pasteboard before any write -> refuse and
    # keep their newer content (the stale snapshot is safe to delete)
    stolen = simulate([], [6])
    assert stolen == "restore-skipped:clipboard-ownership-lost"
    assert restore_snapshot_retained(stolen) is False
    # a changeCount change after a failed write cannot be told apart from the
    # failed write itself: stop, never write again, and retain the snapshot
    ambiguous = simulate([PUB_PUBLISH, PUB_OK], [5, 6, 6])
    assert ambiguous == "restore-skipped:restore-failed-4"
    assert restore_snapshot_retained(ambiguous) is True
    # deterministic failures are not retried
    unavailable = simulate([PUB_SNAPSHOT], [5])
    assert unavailable == "restore-skipped:restore-failed-5"
    assert restore_snapshot_retained(unavailable) is True
    assert restore_snapshot_retained("restored") is False
    assert restore_snapshot_retained("restore-skipped:no-publication") is False
    assert restore_snapshot_retained("restore-skipped:read-back-failed:boom") is True
    assert restore_snapshot_retained("restore-skipped:restore-timeout") is True
    assert restore_snapshot_retained("restore-skipped:restore-error:boom") is True
    # An incomplete snapshot is NEVER safe to delete: the partial copy may be the
    # only recovery data the user has. The same holds for a snapshot that was
    # captured but failed pre-publication validation.
    assert restore_snapshot_retained("restore-skipped:snapshot-incomplete") is True
    assert restore_snapshot_retained("restore-skipped:snapshot-unrestorable") is True


def _self_test_snapshot_safety() -> None:
    """Publication refusal and blob retention for incomplete snapshots."""
    # A refusal happens before any publication, whenever a snapshot was attempted
    # and is incomplete or unrestorable.
    assert publication_refusal(True, None) is None, "a complete, validated snapshot permits publication"
    assert publication_refusal(False, None) == "snapshot-incomplete:refusing-publication"
    assert publication_refusal(True, "snapshot-failed:publisher-exit-5") == "snapshot-failed:publisher-exit-5"
    assert publication_refusal(False, "snapshot-unrestorable:blob-missing:x") == \
        "snapshot-unrestorable:blob-missing:x"
    # `last_change_count is None` after a failed snapshot must stay flagged as
    # incomplete/unrestorable, not be relabelled as the safe no-publication
    # status; a snapshot that captured nothing has no recovery data to keep.
    assert restore_status_without_publication(True, False, False) == "restore-skipped:snapshot-incomplete"
    assert restore_status_without_publication(True, True, False) == "restore-skipped:snapshot-unrestorable"
    assert restore_status_without_publication(True, True, True) == "restore-skipped:no-publication"
    assert restore_status_without_publication(False, False, False) == "restore-skipped:no-publication"
    assert restore_snapshot_retained(restore_status_without_publication(True, False, False)) is True
    assert restore_snapshot_retained(restore_status_without_publication(True, True, False)) is True
    assert restore_snapshot_retained(restore_status_without_publication(False, False, False)) is False

    # Restore data is prepared and validated before publication: a missing blob,
    # a size mismatch or a hash mismatch is a refusal, not a restore-time surprise.
    with tempfile.TemporaryDirectory(prefix="vacards-clip-selftest-") as tmp:
        blob_dir = Path(tmp)
        payload = b"snapshot payload"
        (blob_dir / "blob-000-000.bin").write_bytes(payload)
        good = {
            "complete": True,
            "items": [{"types": [{"uti": "public.utf8-plain-text", "blob": "blob-000-000.bin",
                                  "bytes": len(payload), "sha256": sha256_bytes(payload)}]}],
        }
        assert validate_snapshot_restore_data(good, blob_dir) == [], "a complete snapshot must validate"
        assert validate_snapshot_restore_data(None, blob_dir) == ["snapshot-manifest-invalid"]

        incomplete = dict(good, complete=False)
        assert "snapshot-incomplete" in validate_snapshot_restore_data(incomplete, blob_dir)

        missing = json.loads(json.dumps(good))
        missing["items"][0]["types"][0]["blob"] = "blob-absent.bin"
        problems = validate_snapshot_restore_data(missing, blob_dir)
        assert any(problem.startswith("blob-missing:") for problem in problems), problems

        mismatched = json.loads(json.dumps(good))
        mismatched["items"][0]["types"][0]["sha256"] = "0" * 64
        problems = validate_snapshot_restore_data(mismatched, blob_dir)
        assert any(problem.startswith("blob-hash-mismatch:") for problem in problems), problems

        wrong_size = json.loads(json.dumps(good))
        wrong_size["items"][0]["types"][0]["bytes"] = len(payload) + 1
        problems = validate_snapshot_restore_data(wrong_size, blob_dir)
        assert any(problem.startswith("blob-size-mismatch:") for problem in problems), problems

        # A type the publisher could not snapshot marks a whole item unrestorable.
        no_blob = {"complete": True,
                   "items": [{"types": [{"uti": "public.png", "blob": None, "bytes": 0, "sha256": None}]}]}
        problems = validate_snapshot_restore_data(no_blob, blob_dir)
        assert any("type-without-blob" in problem for problem in problems), problems
        assert any(problem.endswith("unrestorable") for problem in problems), problems


def protocol_self_test() -> int:
    checks = (
        (PROTOCOL_SELF_TEST_CHECKS[0], _self_test_paths),
        (PROTOCOL_SELF_TEST_CHECKS[1], _self_test_lease_contention),
        (PROTOCOL_SELF_TEST_CHECKS[2], _self_test_wrapper_mapping),
        (PROTOCOL_SELF_TEST_CHECKS[3], _self_test_manifest_binding),
        (PROTOCOL_SELF_TEST_CHECKS[4], _self_test_summary),
        (PROTOCOL_SELF_TEST_CHECKS[5], _self_test_restore_logic),
        (PROTOCOL_SELF_TEST_CHECKS[6], _self_test_snapshot_safety),
    )
    sys.stdout.write("protocol self-test: pure checks only (no publisher, no receiver, no clipboard)\n")
    failures = 0
    for name, function in checks:
        try:
            function()
        except Exception as error:  # a failed check must be reported, never raised
            failures += 1
            sys.stdout.write(f"[FAIL] {name}: {error}\n")
        else:
            sys.stdout.write(f"[ ok ] {name}\n")
    sys.stdout.write(f"protocol self-test: {len(checks) - failures}/{len(checks)} checks passed\n")
    return EXIT_OK if failures == 0 else EXIT_FAIL


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------
def parse_args(argv):
    parser = argparse.ArgumentParser(
        description="External-process clipboard paste driver (opt-in; never touches the clipboard without "
                    "--allow-clipboard-takeover).",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter)
    parser.add_argument("--publisher", help="vacards-clipboard-publisher binary")
    parser.add_argument("--receiver", help="test_text-paste-external binary")
    parser.add_argument("--fixtures", help="fixtures directory containing manifest.json")
    parser.add_argument("--protocol-self-test", action="store_true",
                        help="run the pure protocol self-test (no publisher, no receiver, no clipboard) and exit")
    parser.add_argument("--self-test-mode", choices=("lease-hold", "lease-try"), default=None,
                        help=argparse.SUPPRESS)
    parser.add_argument("--self-test-session-root", default=None, help=argparse.SUPPRESS)
    parser.add_argument("--self-test-hold-ms", type=int, default=1500, help=argparse.SUPPRESS)
    parser.add_argument("--session-root", default=str(Path(os.environ.get("TMPDIR", "/tmp")) / "vacards-clip-sessions"),
                        help="root for the 0700 per-run session directory")
    parser.add_argument("--evidence", default="clipboard-evidence", help="evidence root (one run subdirectory)")
    parser.add_argument("--source-dir", default=str(Path(__file__).resolve().parents[2]),
                        help="checkout recorded as the source identity")
    parser.add_argument("--app-plist", default="/Applications/VA Studio Nesting Test.app/Contents/Info.plist",
                        help="installed app Info.plist for the app version field")
    parser.add_argument("--cases", default=DEFAULT_CASES,
                        help="comma-separated case ids, requirement ids (E07) or 'all'")
    parser.add_argument("--allow-clipboard-takeover", action="store_true",
                        help="explicit consent to replace the user's clipboard for the run")
    parser.add_argument("--allow-partial-snapshot", action="store_true",
                        help="removed: an incomplete snapshot always refuses publication")
    parser.add_argument("--ready-timeout-ms", type=int, default=20000)
    parser.add_argument("--receiver-timeout-ms", type=int, default=180000)
    parser.add_argument("--publisher-timeout-ms", type=int, default=120000)
    parser.add_argument("--hold-timeout-ms", type=int, default=300000)
    parser.add_argument("--run-id", default=None)
    parser.add_argument("--dry-run", action="store_true",
                        help="validate manifest/binaries/cases and print the plan without any clipboard access")
    return parser.parse_args(argv)


def main(argv) -> int:
    args = parse_args(argv)
    if args.self_test_mode:
        # Hidden child mode used by the lease-contention self-test only.
        return _self_test_lease_child(args)
    if args.protocol_self_test:
        return protocol_self_test()
    if args.allow_partial_snapshot:
        # The unsafe publication path is gone. Refuse before any clipboard
        # access instead of running with no verified recovery copy.
        sys.stderr.write(
            "harness error: --allow-partial-snapshot is no longer accepted: an incomplete snapshot "
            "refuses publication because the snapshot may be the only copy of the user's clipboard\n")
        return EXIT_USAGE
    missing = [name for name in ("publisher", "receiver", "fixtures") if not getattr(args, name)]
    if missing:
        sys.stderr.write(
            "harness error: " + ", ".join("--" + name for name in missing) + " required\n")
        return EXIT_USAGE
    try:
        manifest = Manifest(Path(args.fixtures).expanduser().resolve())
        cases = manifest.select(args.cases)
    except HarnessError as error:
        sys.stderr.write(f"harness error: {error}\n")
        return EXIT_FAIL

    if args.dry_run:
        return dry_run(args, manifest, cases)

    run_id = args.run_id or time.strftime("%Y%m%dT%H%M%SZ", time.gmtime()) + "-" + uuid.uuid4().hex[:8]
    harness = Harness(args, manifest, cases, run_id)

    opted_in = args.allow_clipboard_takeover or os.environ.get("VACARDS_CLIP_ALLOW") == "1"
    harness.opted_in = bool(opted_in)
    harness.opt_in_source = "--allow-clipboard-takeover" if args.allow_clipboard_takeover else (
        "VACARDS_CLIP_ALLOW=1" if opted_in else None)
    if not opted_in:
        # Refuse to publish. Record env-blocked for every case, touch nothing.
        harness.refusal_reason = "not-opted-in"
        harness.evidence_dir.mkdir(parents=True, exist_ok=True)
        for case in cases:
            record = harness.base_record(case, "env-blocked", "not-opted-in")
            record["clipboard_protection"] = {
                "snapshot_complete": False,
                "snapshot_retained": False,
                "restore": "restore-skipped:not-opted-in",
                "restore_attempts": 0,
                "publication_change_count": None,
            }
            harness.emit(record)
        harness.write_summary()
        sys.stderr.write("external clipboard suite not opted in: refusing to publish "
                         "(--allow-clipboard-takeover or VACARDS_CLIP_ALLOW=1)\n")
        return EXIT_NOT_OPTED_IN

    def handle_signal(signum, _frame):
        raise SystemExit(128 + signum)

    for signum in (signal.SIGINT, signal.SIGTERM):
        try:
            signal.signal(signum, handle_signal)
        except ValueError:
            pass

    try:
        exit_code = harness.run()
    except SystemExit:
        raise
    except KeyboardInterrupt:
        exit_code = EXIT_FAIL
    except HarnessError as error:
        sys.stderr.write(f"harness error: {error}\n")
        exit_code = EXIT_FAIL
    finally:
        pass
    harness.write_summary()
    return exit_code


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
