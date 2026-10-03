#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Hash a quiescent evidence tree into UTF-8/LF SHA256SUMS records.

Usage: hash-evidence.py --root ROOT [--output SHA256SUMS] [PATH ...]
PATH and --output are relative to ROOT; PATH defaults to the complete tree.
Records are '<lowercase SHA-256>  <NFC, forward-slash relative path>\n',
sorted by normalized path. Repeated/overlapping selections are a set. Internal
symlinks are hashed under their logical paths; escapes and cycles are errors.
No timestamps, absolute paths, platform newline conversion or symlink metadata
enter the manifest. Run only after evidence producers have stopped writing.
"""

import argparse
import hashlib
import os
from pathlib import Path, PurePosixPath, PureWindowsPath
import stat
import sys
import tempfile
import unicodedata


class EvidenceError(ValueError):
    pass


def relative_path(value):
    """Accept either CLI separator; reject traversal before normalization."""
    windows = PureWindowsPath(value)
    parts = value.replace("\\", "/").split("/")
    if not value or windows.drive or windows.root or ".." in parts:
        raise EvidenceError(f"expected a relative path without traversal: {value!r}")
    return Path(*(part for part in parts if part not in ("", ".")))


def normalized_name(path):
    parts = []
    for part in path.parts:
        # A POSIX backslash filename is not a Windows directory separator.
        # Reject names that could acquire a different meaning on Windows or
        # inject a checksum record. NFC makes macOS decomposition portable.
        if (any(ord(char) < 32 or ord(char) == 127 or char in '\\<>:"|?*'
                for char in part) or part.endswith((".", " "))):
            raise EvidenceError(f"nonportable evidence filename: {str(path)!r}")
        stem = part.split(".")[0].upper()
        if stem in {"CON", "PRN", "AUX", "NUL", "CONIN$", "CONOUT$"} or (
                len(stem) == 4 and stem[:3] in {"COM", "LPT"} and stem[3] in "123456789¹²³"):
            raise EvidenceError(f"reserved Windows filename: {str(path)!r}")
        parts.append(unicodedata.normalize("NFC", part))
    return "/".join(parts)


def register_name(path, names, root=None):
    """Reject portable-name collisions in every prefix, including directories.

    Explicit file selections may never visit their parent directories. Register
    those prefixes here too, so e.g. 'item' and 'ITEM/child' cannot produce an
    archive that works only on a case-sensitive filesystem. Different Unicode
    spellings are rejected unless the host aliases them to the same entry; in
    that case the emitted name is the same NFC string either way.
    """
    for count in range(1, len(path.parts) + 1):
        raw = "/".join(path.parts[:count])
        prefix = PurePosixPath(raw)
        name = normalized_name(prefix)
        key = name.casefold()
        previous = names.get(key)
        # Compare strings, not Path objects: WindowsPath equality itself folds
        # case and would conceal exactly the collision we need to reject.
        if previous is not None and previous != raw:
            if (root is not None and normalized_name(PurePosixPath(previous)) == name
                    and os.path.samefile(root / previous, root / raw)):
                continue
            raise EvidenceError(f"portable path collision: {previous} and {prefix}")
        names[key] = raw
    return normalized_name(path)


def contained(root, path, strict=True):
    try:
        resolved = path.resolve(strict=strict)
        resolved.relative_to(root)
    except (ValueError, RuntimeError) as error:
        raise EvidenceError(f"path escapes evidence root or forms a symlink cycle: {path}") from error
    return resolved


def output_path(root, output):
    relative = relative_path(output)
    if relative == Path("."):
        raise EvidenceError("output must name a file")
    normalized_name(relative)
    result = root / relative
    contained(root, result, strict=False)
    if result.is_symlink() or (result.exists() and not result.is_file()):
        raise EvidenceError("output must be a regular file, not a symlink or directory")
    if result.exists() and result.stat().st_nlink > 1:
        # Atomic replacement breaks the link: an alias excluded on this run
        # would become an input on the next run. Never publish that unstable set.
        raise EvidenceError("output must not have hard links")
    return result


def manifest_bytes(root, paths=(), output="SHA256SUMS"):
    root = Path(root).resolve(strict=True)
    if not root.is_dir():
        raise EvidenceError("evidence root must be a directory")
    destination = output_path(root, output)
    resolved_output = destination.resolve()
    entries = {}
    case_names = {}
    register_name(relative_path(output), case_names, root)

    def visit(relative, ancestors):
        logical = root / relative
        resolved = contained(root, logical)
        if resolved == resolved_output or (
                destination.exists() and os.path.samefile(logical, destination)):
            return
        name = register_name(relative, case_names, root)
        mode = resolved.stat().st_mode
        if stat.S_ISDIR(mode):
            if resolved in ancestors:
                raise EvidenceError(f"symlink directory cycle: {relative}")
            for child in sorted(logical.iterdir()):
                visit(relative / child.name, ancestors | {resolved})
        elif stat.S_ISREG(mode):
            previous = entries.get(name)
            if (previous is not None and previous.as_posix() != relative.as_posix()
                    and not os.path.samefile(root / previous, logical)):
                raise EvidenceError(f"normalized path collision: {name}")
            entries[name] = relative
        else:
            raise EvidenceError(f"evidence is not a regular file: {relative}")

    for selection in paths or (".",):
        visit(relative_path(selection), frozenset())
    if not entries:
        raise EvidenceError("no evidence files selected")
    records = []
    for name, relative in sorted(entries.items()):
        # Recheck containment just before opening. The tree must be quiescent;
        # this utility is not a sandbox against concurrent filesystem changes.
        resolved = contained(root, root / relative)
        digest = hashlib.sha256()
        with resolved.open("rb") as stream:
            for chunk in iter(lambda: stream.read(1024 * 1024), b""):
                digest.update(chunk)
        records.append(f"{digest.hexdigest()}  {name}\n")
    return "".join(records).encode("utf-8")


def main(argv=None):
    # Redirected Windows output otherwise uses the legacy code page. A valid
    # Unicode destination must not report failure after publishing its manifest.
    sys.stdout.reconfigure(encoding="utf-8")
    sys.stderr.reconfigure(encoding="utf-8", errors="backslashreplace")
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", required=True, type=Path)
    parser.add_argument("--output", default="SHA256SUMS")
    parser.add_argument("paths", nargs="*")
    args = parser.parse_args(argv)
    temporary = None
    try:
        root = args.root.resolve(strict=True)
        data = manifest_bytes(root, args.paths, args.output)
        destination = output_path(root, args.output)
        # Compute everything first; errors preserve an existing manifest.
        with tempfile.NamedTemporaryFile(dir=destination.parent, delete=False) as stream:
            temporary = Path(stream.name)
            stream.write(data)
        os.replace(temporary, destination)
        temporary = None
        print(destination)
    except (EvidenceError, OSError, UnicodeError) as error:
        parser.exit(1, f"Invalid VACards evidence: {error}\n")
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
