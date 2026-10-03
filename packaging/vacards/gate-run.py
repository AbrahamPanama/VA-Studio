#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Reserve fresh Ninja qualification runs and verify their observed inputs.

claim --source PATH --build PATH --mode quick|full [--install PATH]
verify --build PATH

Wire claim after configuration/preflight and BEFORE any build or evidence
write. It exclusively creates VACARDS-GATE-RUN.json, vacards-release-evidence,
and (for full) the explicit install prefix. Its parent must already exist.
The install prefix overrides CMAKE_INSTALL_PREFIX just as cmake --install
--prefix does; both the override and the original cache bytes are bound.
Wire verify after builds/tests, before installation, and before attestation.
Keep the existing gate's success criteria; this helper never attests success.

Validation errors before reservation create nothing. Once exclusive ledger
creation succeeds, every failure consumes the run: preserve even an empty or
partial ledger and all output. Multiple-directory creation is not a filesystem
transaction; interrupted reservations fail closed, with no rollback/reuse.
POSIX dir_fd operations and Windows NtCreateFile RootDirectory handles anchor
child operations. Neither backend follows symlinks/reparse points for outputs.
Ledger bytes are flushed; POSIX directory entries are also fsynced. Windows
does not offer the same directory-fsync contract: power-loss recovery still
requires filesystem/recovery evidence. Python 3.10+ and Git are required.
Keep source/configuration quiescent during claim/verify. A missing root Ninja
log is only an observation, NOT proof that a tree has never been built; deleted
history cannot be reconstructed. This local record is not authenticated against
an owner deliberately rewriting the ledger and recomputing its checksum.
Source hashing covers tracked and nonignored untracked files, including
submodule working trees. External libraries, SDK contents, compiler subprocesses
and ignored inputs still need the gate's separate provenance/feature checks.
Build/test control files and their includes are frozen, not just build.ninja;
ordinary generated binaries and runtime logs are not configuration inputs.
New build/test script files also fail verification. Materialize control scripts at
configure time; this deliberately rejects build-time test discovery rewrites.
Ninja includes and CMake/CTest include/subdirs references are followed without
executing them. Unsupported dynamic include paths fail closed. This is not a
shell interpreter or a hermetic sandbox: arbitrary commands' external data and
runtime-generated binaries remain the responsibility of the existing gate.
Keep the recorded compiler/search/Cargo environment unchanged through verify;
the helper observes it, but does not sanitize a caller's later subprocesses.
"""

import argparse
from contextlib import ExitStack
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import stat
import subprocess
import sys
import uuid


LEDGER = "VACARDS-GATE-RUN.json"
EVIDENCE = "vacards-release-evidence"
FEATURES = "VACARDS-CMAKE-FEATURES.env"
PURPOSE = "qualification-input-reservation"
REQUIRED = {
    "CMAKE_HOME_DIRECTORY", "CMAKE_CACHEFILE_DIR", "CMAKE_GENERATOR",
    "CMAKE_BUILD_TYPE", "CMAKE_C_COMPILER", "CMAKE_CXX_COMPILER",
    "VACARDS_NESTING_RUSTC_EXECUTABLE", "VACARDS_NESTING_RUST_TARGET",
}
COMPILERS = ("CMAKE_C_COMPILER", "CMAKE_CXX_COMPILER", "VACARDS_NESTING_RUSTC_EXECUTABLE")
OTHER_TOOLS = ("CMAKE_MAKE_PROGRAM", "CMAKE_LINKER", "CMAKE_AR", "CMAKE_RANLIB",
               "CMAKE_C_COMPILER_AR", "CMAKE_C_COMPILER_RANLIB",
               "CMAKE_CXX_COMPILER_AR", "CMAKE_CXX_COMPILER_RANLIB",
               "VACARDS_NESTING_CARGO_EXECUTABLE", "VACARDS_NESTING_RUSTDOC_EXECUTABLE")
CACHE_RECORD = re.compile(r'("[^"\r\n]+"|[^:=\s"]+):(BOOL|FILEPATH|PATH|STRING|INTERNAL|STATIC|UNINITIALIZED)=(.*)')
BUILD_ENVIRONMENT = ("CC", "CXX", "CFLAGS", "CXXFLAGS", "CPPFLAGS", "LDFLAGS",
                     "RUSTFLAGS", "CARGO_ENCODED_RUSTFLAGS", "RUSTC", "RUSTDOC",
                     "RUSTDOCFLAGS", "CARGO_ENCODED_RUSTDOCFLAGS", "RUSTC_WRAPPER", "RUSTC_WORKSPACE_WRAPPER",
                     "RUSTC_BOOTSTRAP", "RUSTUP_TOOLCHAIN", "RUSTUP_HOME", "CARGO_HOME",
                     "SDKROOT", "MACOSX_DEPLOYMENT_TARGET", "DEVELOPER_DIR",
                     "PATH", "PATHEXT", "CPATH", "C_INCLUDE_PATH", "CPLUS_INCLUDE_PATH",
                     "OBJC_INCLUDE_PATH", "LIBRARY_PATH", "COMPILER_PATH", "GCC_EXEC_PREFIX",
                     "INCLUDE", "LIB", "LIBPATH", "CL", "_CL_", "LINK", "_LINK_",
                     "LD_LIBRARY_PATH", "LD_PRELOAD", "DYLD_LIBRARY_PATH",
                     "DYLD_FALLBACK_LIBRARY_PATH", "DYLD_INSERT_LIBRARIES",
                     "SOURCE_DATE_EPOCH", "CCC_OVERRIDE_OPTIONS", "CCC_ADD_ARGS",
                     "SHELL", "COMSPEC", "MAKEFLAGS", "CMAKE_BUILD_PARALLEL_LEVEL")
CONTROL_SUFFIXES = {".ninja", ".cmake", ".ctest", ".sh", ".bash", ".py", ".pl",
                    ".rb", ".ps1", ".bat", ".cmd", ".rsp"}


def build_environment():
    # Deliberately do not copy the whole environment (registry tokens, signing
    # keys, etc.). These are compiler/config overrides, not credential settings.
    # Cargo documents build/target/profile overrides separately from credentials:
    # https://doc.rust-lang.org/cargo/reference/environment-variables.html
    return {key: os.environ[key] for key in os.environ if key.upper() in BUILD_ENVIRONMENT or
            re.fullmatch(r"(?:CARGO_(?:BUILD|TARGET|PROFILE)_[A-Z0-9_]+|"
                         r"(?:CC|CXX|AR|CFLAGS|CXXFLAGS|CPPFLAGS|LDFLAGS)_.+|"
                         r"(?:HOST|TARGET)_(?:CC|CXX|AR|CFLAGS|CXXFLAGS|CPPFLAGS|LDFLAGS))", key)}


class GateError(ValueError):
    pass


def require(condition, message):
    if not condition:
        raise GateError(message)


def canonical_json(value):
    return json.dumps(value, sort_keys=True, ensure_ascii=True, separators=(",", ":"),
                      allow_nan=False).encode("ascii")


def digest(data):
    return hashlib.sha256(data).hexdigest()


def unique_object(pairs):
    result = {}
    for key, value in pairs:
        require(key not in result, f"duplicate JSON field: {key}")
        result[key] = value
    return result


def safe_path(value, *, existing=True, directory=True):
    """Require an absolute, lexical-normal path with no symlink components."""
    require(isinstance(value, str) and value and not any(ord(c) < 32 for c in value),
            "path must be a nonempty string without control characters")
    path = Path(value)
    require(path.is_absolute() and ".." not in path.parts, f"path must be absolute without traversal: {value}")
    if os.name == "nt":
        for component in path.parts[1:]:
            windows_name(component)
    for part in (path, *path.parents):
        try:
            info = part.lstat()
        except FileNotFoundError:
            continue
        require(not stat.S_ISLNK(info.st_mode) and not getattr(info, "st_file_attributes", 0) & 0x400,
                f"symlinked/reparse target or parent: {part}")
    resolved = path.resolve(strict=existing)
    if existing:
        require(resolved.is_dir() if directory else resolved.is_file(), f"wrong input type: {path}")
    return resolved


def disjoint(left, right):
    return left != right and left not in right.parents and right not in left.parents


def identity(info):
    return {"device": info.st_dev, "inode": info.st_ino}


def windows_name(name):
    require(name and name not in (".", "..") and not name.endswith((".", " "))
            and not any(ord(c) < 32 or c in '\\/:<>"|?*' for c in name),
            f"unsafe Windows path component: {name!r}")
    stem = name.split(".", 1)[0].upper()
    require(stem not in {"CON", "PRN", "AUX", "NUL", "CONIN$", "CONOUT$"}
            and not re.fullmatch(r"(?:COM|LPT)[1-9¹²³]", stem), f"reserved Windows filename: {name}")


class WindowsFiles:
    """Standard-library ctypes adapter; no shell/MSYS dependency for file I/O.

    NtCreateFile's FILE_CREATE is exclusive, and RootDirectory supplies the
    same anchor as POSIX dir_fd. FILE_OPEN_REPARSE_POINT opens the link object
    itself; FileAttributeTagInfo rejects it before any content read/write.
    https://learn.microsoft.com/en-us/windows/win32/api/winternl/nf-winternl-ntcreatefile
    https://docs.python.org/3/library/msvcrt.html#msvcrt.open_osfhandle
    """
    def __init__(self):
        import ctypes as ct
        import msvcrt

        self.ct, self.crt = ct, msvcrt
        class UnicodeString(ct.Structure):
            _fields_ = [("length", ct.c_uint16), ("maximum", ct.c_uint16), ("buffer", ct.c_void_p)]
        class ObjectAttributes(ct.Structure):
            _fields_ = [("length", ct.c_uint32), ("root", ct.c_void_p),
                        ("name", ct.POINTER(UnicodeString)), ("attributes", ct.c_uint32),
                        ("security", ct.c_void_p), ("quality", ct.c_void_p)]
        class IoStatus(ct.Structure):
            _fields_ = [("status_or_pointer", ct.c_void_p), ("information", ct.c_size_t)]
        class AttributeTag(ct.Structure):
            _fields_ = [("attributes", ct.c_uint32), ("tag", ct.c_uint32)]
        class BasicInfo(ct.Structure):
            _fields_ = [("creation", ct.c_int64), ("access", ct.c_int64),
                        ("write", ct.c_int64), ("change", ct.c_int64), ("attributes", ct.c_uint32)]
        self.UnicodeString, self.ObjectAttributes = UnicodeString, ObjectAttributes
        self.IoStatus, self.AttributeTag, self.BasicInfo = IoStatus, AttributeTag, BasicInfo
        kernel = ct.WinDLL("kernel32", use_last_error=True)
        native = ct.WinDLL("ntdll", use_last_error=True)
        self.create = kernel.CreateFileW
        self.create.argtypes = [ct.c_wchar_p, ct.c_uint32, ct.c_uint32, ct.c_void_p,
                                ct.c_uint32, ct.c_uint32, ct.c_void_p]
        self.create.restype = ct.c_void_p
        self.nt_create = native.NtCreateFile
        self.nt_create.argtypes = [ct.POINTER(ct.c_void_p), ct.c_uint32, ct.POINTER(ObjectAttributes),
                                   ct.POINTER(IoStatus), ct.c_void_p, ct.c_uint32, ct.c_uint32,
                                   ct.c_uint32, ct.c_uint32, ct.c_void_p, ct.c_uint32]
        self.nt_create.restype = ct.c_int32
        self.dos_error = native.RtlNtStatusToDosError
        self.dos_error.argtypes, self.dos_error.restype = [ct.c_int32], ct.c_uint32
        self.get_info = kernel.GetFileInformationByHandleEx
        self.get_info.argtypes = [ct.c_void_p, ct.c_int, ct.c_void_p, ct.c_uint32]
        self.get_info.restype = ct.c_int
        self.set_info = kernel.SetFileInformationByHandle
        self.set_info.argtypes, self.set_info.restype = self.get_info.argtypes, ct.c_int
        self.close = kernel.CloseHandle
        self.close.argtypes, self.close.restype = [ct.c_void_p], ct.c_int

    def open(self, name, *, parent=None, create=False, directory=False, attributes_only=False):
        ct = self.ct
        access = 0x001000A0 if directory else (0x00100080 if attributes_only else 0x80100000)
        if create and not directory:
            access = 0xC0100000  # GENERIC_READ | GENERIC_WRITE | SYNCHRONIZE
        if parent is None:
            require(directory and not create, "Windows root open must be an existing directory")
            raw = self.create(str(name), access, 7, None, 3, 0x02200000, None)
            if raw == ct.c_void_p(-1).value:
                raise ct.WinError(ct.get_last_error())
        else:
            windows_name(name)
            encoded = name.encode("utf-16-le")
            require(len(encoded) <= 65532, "Windows component exceeds UNICODE_STRING limit")
            buffer = ct.create_string_buffer(encoded + b"\0\0")
            text = self.UnicodeString(len(encoded), len(encoded) + 2, ct.cast(buffer, ct.c_void_p))
            attributes = self.ObjectAttributes(ct.sizeof(self.ObjectAttributes),
                self.crt.get_osfhandle(parent), ct.pointer(text), 0x40, None, None)
            result, status = ct.c_void_p(), self.IoStatus()
            # SYNCHRONOUS_IO_NONALERT | OPEN_REPARSE_POINT, then restrict type.
            options = 0x00200020 | (1 if directory else (0 if attributes_only else 0x40))
            code = self.nt_create(ct.byref(result), access, ct.byref(attributes), ct.byref(status),
                                  None, 0x80, 1 if create and not directory else 7,
                                  2 if create else 1, options, None, 0)
            if code < 0:
                raise ct.WinError(self.dos_error(code))
            raw = result.value
        try:
            tag = self.AttributeTag()
            if not self.get_info(raw, 9, ct.byref(tag), ct.sizeof(tag)):
                raise ct.WinError(ct.get_last_error())
            require(not tag.attributes & 0x400, f"reparse point rejected: {name}")
            # The native handle's access mask controls read/write permission.
            fd = self.crt.open_osfhandle(raw, os.O_BINARY | os.O_NOINHERIT)
        except BaseException:
            self.close(raw)
            raise
        return fd  # The CRT now owns and closes the native handle.

    def readonly(self, fd):
        info = self.BasicInfo()
        info.attributes = 1  # FILE_ATTRIBUTE_READONLY, applied to the open file.
        if not self.set_info(self.crt.get_osfhandle(fd), 0, self.ct.byref(info), self.ct.sizeof(info)):
            raise self.ct.WinError(self.ct.get_last_error())


class Directory:
    """Pin a directory for safe direct-child reads and exclusive creation."""
    def __init__(self, path):
        self.path = safe_path(str(path))
        self.windows = WindowsFiles() if os.name == "nt" else None
        if self.windows:
            self.fd = self.windows.open(self.path, directory=True)
        else:
            self.fd = os.open(self.path, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
        self.info = identity(os.fstat(self.fd))

    def __enter__(self):
        try:
            self.check()
        except BaseException:
            os.close(self.fd)
            raise
        return self

    def __exit__(self, *_):
        os.close(self.fd)

    def check(self):
        require(safe_path(str(self.path)) == self.path, "directory path changed")
        require(identity(self.path.stat()) == self.info, f"directory replaced: {self.path}")

    def exists(self, name):
        self.check()
        try:
            self.stat(name)
            return True
        except FileNotFoundError:
            return False

    def stat(self, name):
        if self.windows:
            fd = self.windows.open(name, parent=self.fd, attributes_only=True)
            try:
                return os.fstat(fd)
            finally:
                os.close(fd)
        return os.stat(name, dir_fd=self.fd, follow_symlinks=False)

    def open_file(self, name, *, create=False):
        self.check()
        require(Path(name).name == name and name not in (".", ".."), "expected a direct child")
        if self.windows:
            return self.windows.open(name, parent=self.fd, create=create)
        flags = os.O_WRONLY | os.O_CREAT | os.O_EXCL if create else os.O_RDONLY | os.O_NONBLOCK
        return os.open(name, flags | os.O_NOFOLLOW, 0o600, dir_fd=self.fd)

    def sync(self):
        if not self.windows:
            os.fsync(self.fd)

    def readonly(self, fd):
        if self.windows:
            self.windows.readonly(fd)
        else:
            os.fchmod(fd, 0o444)

    def read(self, name, *, optional=False):
        self.check()
        if optional and not self.exists(name):
            return None
        fd = self.open_file(name)
        with os.fdopen(fd, "rb") as stream:
            before = os.fstat(stream.fileno())
            require(stat.S_ISREG(before.st_mode), f"not a regular input file: {name}")
            data = stream.read()
            after = os.fstat(stream.fileno())
            current = self.stat(name)
            require((before.st_dev, before.st_ino, before.st_size, before.st_mtime_ns)
                    == (after.st_dev, after.st_ino, after.st_size, after.st_mtime_ns)
                    == (current.st_dev, current.st_ino, current.st_size, current.st_mtime_ns),
                    f"input changed while reading: {name}")
        self.check()
        return data

    def mkdir(self, name):
        self.check()
        require(Path(name).name == name and name not in (".", ".."), "expected a direct child")
        if self.windows:
            fd = self.windows.open(name, parent=self.fd, create=True, directory=True)
            os.close(fd)
        else:
            os.mkdir(name, mode=0o755, dir_fd=self.fd)
        self.sync()
        self.check()
        return self.child_identity(name)

    def child_identity(self, name):
        self.check()
        info = self.stat(name)
        require(stat.S_ISDIR(info.st_mode), f"output is not a real directory: {name}")
        return {"path": str(self.path / name), **identity(info)}


def parse_cache(data):
    text = data.decode("utf-8").replace("\r\n", "\n")
    require(not any(ord(c) < 32 and c not in "\n\t" or ord(c) == 127 for c in text),
            "control byte in CMakeCache.txt")
    entries = {}
    for number, line in enumerate(text.split("\n"), 1):
        if not line or line.startswith(("//", "#")):
            continue
        match = CACHE_RECORD.fullmatch(line)
        require(match is not None, f"malformed cache record at line {number}")
        key, kind, value = match.groups()
        key = key.strip('"')
        require(key not in entries, f"duplicate cache field: {key}")
        entries[key] = {"type": kind, "value": value}
    for field in sorted(REQUIRED):
        require(field in entries and entries[field]["value"], f"missing cache field: {field}")
    return entries


def git(source, *args):
    # Ignore ambient repository overrides and external diff/fsmonitor helpers.
    environment = {key: value for key, value in os.environ.items() if not key.startswith("GIT_")}
    environment["GIT_TERMINAL_PROMPT"] = "0"
    result = subprocess.run(["git", "--no-optional-locks", "-c", "core.fsmonitor=false",
                             "-C", str(source), *args], env=environment, capture_output=True)
    require(result.returncode == 0, "Git input inspection failed: " + result.stderr.decode("utf-8", "replace"))
    return result.stdout


def native_git_path(data):
    value = os.fsdecode(data.rstrip(b"\r\n"))
    if os.name == "nt" and value.startswith("/") and not Path(value).is_absolute():
        # MSYS Git reports POSIX paths to native Python. Use the converter from
        # that Git installation, not a guessed /c -> C: mapping or a second Git.
        executable = shutil.which("git")
        require(executable is not None, "Git executable not found")
        converter = Path(executable).resolve().with_name("cygpath.exe")
        require(converter.is_file(), "MSYS Git requires its sibling cygpath.exe")
        converted = subprocess.run([str(converter), "-am", "-C", "UTF8", "--", value],
                                   capture_output=True, timeout=30)
        require(converted.returncode == 0, "Git path conversion failed")
        value = converted.stdout.decode("utf-8").rstrip("\r\n")
    require(Path(value).is_absolute(), "Git returned a non-absolute path")
    return Path(value)


def source_file(source, name):
    file = source / name
    safe_path(str(file.parent), existing=False)
    if file.is_symlink():
        return {"path": name, "kind": "symlink", "sha256": digest(os.fsencode(os.readlink(file)))}
    if not file.exists():
        return {"path": name, "kind": "missing"}
    resolved = safe_path(str(file), directory=False)
    require(source in resolved.parents, "source file escapes worktree")
    return {"path": name, "kind": "file", "sha256": digest(resolved.read_bytes()),
            "mode": stat.S_IMODE(resolved.stat().st_mode)}


def repository_identity(source, ancestors=()):
    require(source not in ancestors, "recursive submodule path cycle")
    actual = native_git_path(git(source, "rev-parse", "--show-toplevel")).resolve()
    require(actual == source, "source must be the Git worktree root")
    # Equivalent commit peel, without braces in native Python -> MSYS Git argv.
    head = git(source, "rev-parse", "--verify", "HEAD^0").decode().strip()
    require(re.fullmatch(r"[0-9a-f]{40}|[0-9a-f]{64}", head), "invalid full source SHA")
    common = git(source, "rev-parse", "--path-format=absolute", "--git-common-dir")
    common_path = str(native_git_path(common).resolve(strict=True))
    staged = git(source, "diff", "--cached", "--binary", "--full-index", "--no-ext-diff", "--no-textconv", "HEAD", "--")
    unstaged = git(source, "diff", "--binary", "--full-index", "--no-ext-diff", "--no-textconv", "--")
    status = git(source, "status", "--porcelain=v1", "-z", "--untracked-files=all", "--ignore-submodules=none")
    untracked = []
    for raw in sorted(filter(None, git(source, "ls-files", "--others", "--exclude-standard", "-z").split(b"\0"))):
        untracked.append(source_file(source, os.fsdecode(raw)))
    submodules = []
    tracked = []
    for entry in git(source, "ls-files", "--stage", "-z").split(b"\0"):
        if not entry:
            continue
        metadata, raw_name = entry.split(b"\t", 1)
        mode, revision, stage = metadata.split()
        require(stage == b"0", "unmerged source index")
        if mode == b"160000":
            name = os.fsdecode(raw_name)
            checkout = safe_path(str(source / name), existing=False)
            require(source in checkout.parents, "submodule path escapes source")
            state = (repository_identity(checkout, (*ancestors, source))
                     if (checkout / ".git").exists() else None)
            submodules.append({"path": name, "gitlink": revision.decode(), "checkout": state})
        else:
            # Also hash tracked working bytes: assume-unchanged/index stat-cache
            # flags must not hide source tampering behind a clean Git diff.
            tracked.append(source_file(source, os.fsdecode(raw_name)))
    return {"path": str(source), "head": head, "git_common_dir": common_path,
            "dirty_diff_sha256": digest(canonical_json([digest(staged), digest(unstaged)])),
            "status_sha256": digest(status), "tracked_tree_sha256": digest(canonical_json(tracked)),
            "untracked": untracked, "submodules": submodules}


def executable_identity(value):
    # Cache strings are paths, NEVER shell commands or programs to execute.
    lexical = Path(value)
    require(lexical.is_absolute(), f"compiler/tool path must be absolute: {value}")
    resolved = lexical.resolve(strict=True)
    require(resolved.is_file(), f"compiler/tool is not a regular file: {value}")
    before = resolved.stat()
    sha = hashlib.sha256()
    with resolved.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            sha.update(chunk)
    after = resolved.stat()
    require((before.st_dev, before.st_ino, before.st_size, before.st_mtime_ns)
            == (after.st_dev, after.st_ino, after.st_size, after.st_mtime_ns)
            and lexical.resolve(strict=True) == resolved, f"tool changed while hashing: {value}")
    return {"path": str(lexical), "resolved_path": str(resolved), "sha256": sha.hexdigest(),
            "size": after.st_size, "mode": stat.S_IMODE(after.st_mode)}


def install_path(source, build, mode, install):
    require(mode in ("quick", "full"), "mode must be quick or full")
    if mode == "quick":
        require(install is None, "quick mode must not reserve an install prefix")
        return None
    require(install is not None, "full mode requires an install prefix")
    result = safe_path(install, existing=False)
    require(disjoint(source, result) and disjoint(build, result),
            "install prefix must be separate from source and build")
    safe_path(str(result.parent))
    return result


def runtime_file(relative):
    parts = Path(relative).parts
    return (parts[0] in (LEDGER, EVIDENCE) or parts[-1] in (".ninja_log", ".ninja_deps") or
            (parts[0] == "Testing" and Path(relative).suffix.lower() in (".log", ".xml", ".txt")))


def configuration_files(build):
    """Enumerate control files; include closure handles extensionless inputs.

    Newly produced objects, executables and logs are not configure inputs.
    Known script/control suffixes are always enumerated, so injecting a new
    CTestCustom.cmake, nested script or Ninja include is also identity drift.
    Nothing from a recorded inventory is trusted as a path to read on verify.
    """
    names = set()
    def visit(path):
        with Directory(path) as directory:
            for entry in sorted(path.iterdir()):
                name = entry.relative_to(build).as_posix()
                if runtime_file(name):
                    continue
                # Classify without following links, including native Windows
                # junctions; ordinary newly generated library links are outputs.
                info = entry.lstat()
                link = stat.S_ISLNK(info.st_mode) or getattr(info, "st_file_attributes", 0) & 0x400
                selected = (entry.suffix.lower() in CONTROL_SUFFIXES or entry.name == "CMakeLists.txt")
                if link:
                    require(not selected, f"symlinked configuration input: {name}")
                elif stat.S_ISDIR(info.st_mode):
                    visit(entry)
                elif selected:
                    require(stat.S_ISREG(info.st_mode), f"not a regular configuration input: {name}")
                    names.add(name)
            directory.check()
    visit(build)
    result = []
    for name in sorted(names):
        path = build / name
        with Directory(path.parent) as directory:
            data = directory.read(path.name)
            result.append({"path": name, "sha256": digest(data),
                           "mode": stat.S_IMODE(directory.stat(path.name).st_mode)})
    return result


def ninja_expand(text, variables):
    """Expand only Ninja's literal escapes/variables, never shell syntax."""
    def replace(match):
        token = match.group(1)
        if token in ("$", " ", ":"):
            return token
        key = token[1:-1] if token.startswith("{") else token
        require(key in variables, f"unresolved Ninja include variable: {key}")
        return variables[key]
    return re.sub(r"\$(\$| |:|\{[^}]+\}|[A-Za-z0-9_.-]+)", replace, text)


def cmake_calls(text):
    """Lex CMake arguments, not commands; comments/quoted/bracket args matter.

    No evaluation, include execution, or external tool invocation. Dynamic
    includes beyond known list-directory variables must be resolved before
    qualification instead of being silently omitted from the input closure.
    """
    token = re.compile(r'\s+|\#\[(=*)\[.*?\]\1\]|\#[^\n]*|\[(=*)\[.*?\]\2\]|'
                       r'"(?:\\.|[^"\\])*"|[()]|(?:\\.|[^\s()"#])+', re.S)
    tokens = []
    end = 0
    for match in token.finditer(text):
        require(not text[end:match.start()].strip(), "unsupported CMake control syntax")
        word = match.group()
        end = match.end()
        if word.isspace() or word.startswith("#"):
            continue
        if word.startswith('"'):
            word = re.sub(r'\\([\\"; ()#])', r'\1', word[1:-1])
        elif word.startswith("[") and match.group(2) is not None:
            width = len(match.group(2)) + 2
            word = word[width:-width]
        tokens.append(word)
    require(not text[end:].strip(), "unsupported CMake control syntax")
    index = 0
    while index + 1 < len(tokens):
        if tokens[index + 1] != "(":
            index += 1
            continue
        name, args, depth = tokens[index].lower(), [], 1
        index += 2
        while index < len(tokens) and depth:
            word = tokens[index]
            depth += (word == "(") - (word == ")")
            if depth:
                args.append(word)
            index += 1
        require(depth == 0, "unterminated CMake control command")
        yield name, args


def control_includes(source, build, files):
    """Hash include closure even for extensionless/ignored-source inputs.

    Include files must stay in source/build: imported external controls must be
    materialized in these recorded trees before qualification. This restriction
    does not apply to the separately identified tool executables or SDK data.
    """
    included, active, command_scripts = {}, set(), set()

    def read(path):
        path = safe_path(str(path), existing=False, directory=False)
        require(source in path.parents or build in path.parents, f"control include escapes source/build: {path}")
        require(not (build in path.parents and runtime_file(path.relative_to(build).as_posix())),
                "control include uses reserved/runtime output")
        if not path.exists():
            included[str(path)] = {"path": str(path), "sha256": None, "mode": None}
            return path, None
        with Directory(path.parent) as directory:
            data = directory.read(path.name)
            included[str(path)] = {"path": str(path), "sha256": digest(data),
                                   "mode": stat.S_IMODE(directory.stat(path.name).st_mode)}
        return path, data.decode("utf-8")

    def resolve(value, parent):
        require(value and not any(char in value for char in ("$", "\n", "\r", ";")),
                f"unresolved/dynamic control include: {value}")
        path = Path(value)
        # Generated CMake installs legitimately use ../ within the build tree.
        # Check each component before collapsing .. so link/../ cannot conceal
        # an escape. read() still requires the final path inside source/build.
        candidate = Path(path.anchor) if path.is_absolute() else parent
        parts = path.parts[1:] if path.is_absolute() else path.parts
        for part in parts:
            candidate = candidate.parent if part == ".." else candidate / part
            safe_path(str(candidate), existing=False)
        return candidate

    def ninja(path, variables):
        path, text = read(path)
        require(text is not None, f"missing Ninja include: {path}")
        require(path not in active, f"cyclic control include: {path}")
        active.add(path)
        # A continuation is an odd trailing dollar followed by newline/spaces.
        text = re.sub(r"(\$+)\r?\n[ \t]*",
                      lambda m: m[1][:-1] if len(m[1]) % 2 else m[0], text)
        for line in text.splitlines():
            if re.match(r"\s*(?:command|COMMAND)\s*=", line):
                # Generated CMake commands use literal -P script arguments.
                # Identify those operands only; never interpret/evaluate a shell
                # command, expand shell environment, or inspect binary outputs.
                for match in re.finditer(r'(?:^|\s)-P\s+(?:"([^"]+)"|([^\s"]+))', line):
                    operand = ninja_expand(match[1] or match[2], variables)
                    parent = build
                    if not Path(operand).is_absolute():
                        # Recognize CMake's literal generated `cd DIR && cmake
                        # -P ...` form, not arbitrary shell control flow. This is
                        # also emitted by native Windows with cd /D and cmd /C.
                        prefix = line[:match.start()]
                        cd = re.match(r'\s*(?:command|COMMAND)\s*=\s*'
                                      r'(?:.*?cmd(?:\.exe)?\s+/[Cc]\s*")?cd(?: /[Dd])?\s+'
                                      r'(?:"([^"]+)"|([^&|;]+?))\s+&&\s*', prefix)
                        require(cd is not None and not re.search(r'[&|;]', prefix[cd.end():]),
                                "relative Ninja -P control needs a literal generated CMake cwd")
                        parent = resolve(ninja_expand((cd[1] or cd[2]).strip(), variables), build)
                    command_scripts.add(resolve(operand, parent))
            if not line or line[0].isspace() or line.startswith("#"):
                continue
            include = re.fullmatch(r"(include|subninja)[ \t]+(.+)", line)
            if include:
                target = resolve(ninja_expand(include[2], variables), build)
                ninja(target, variables if include[1] == "include" else dict(variables))
            else:
                assignment = re.fullmatch(r"([A-Za-z0-9_.-]+)\s*=\s*(.*)", line)
                if assignment:
                    variables[assignment[1]] = ninja_expand(assignment[2], variables)
        active.remove(path)

    visited = set()
    def cmake(path):
        if path in visited:
            return
        visited.add(path)
        path, text = read(path)
        if text is None:
            return  # Optional/conditional absence is bound; later appearance drifts.
        # Cache entries are not a CMake interpreter: local variables may shadow
        # them. Only the current list identities are safe to resolve here;
        # CMAKE_BINARY_DIR/SOURCE_DIR in -P mode depend on the caller's cwd.
        variables = dict(CMAKE_CURRENT_LIST_DIR=str(path.parent), CMAKE_CURRENT_LIST_FILE=str(path))
        for command, args in cmake_calls(text):
            if command == "add_test":
                for index, argument in enumerate(args[:-1]):
                    if argument == "-P":
                        require(Path(args[index + 1]).is_absolute(), "CTest -P control needs an absolute path")
                        cmake(resolve(args[index + 1], path.parent))
            if command not in ("include", "subdirs") or not args:
                continue
            targets = args if command == "subdirs" else args[:1]
            for target in targets:
                target = re.sub(r"\$\{([^}]+)\}", lambda m: variables.get(m[1], m[0]), target)
                require(command != "include" or Path(target).is_absolute(),
                        "CMake include needs an absolute/list-directory path; cwd/module lookup is not evaluated")
                resolved = resolve(target, path.parent)
                if command == "subdirs":
                    resolved /= "CTestTestfile.cmake"
                cmake(resolved)

    ninja(build / "build.ninja", {})
    for path in sorted(command_scripts):
        cmake(path)
    for item in files:
        # Hash every control file, but only traverse the gate's executable roots.
        # CPack/compiler-detection configuration is not executed by Ninja/CTest/
        # cmake --install; interpreting all .cmake files invents false failures
        # for legitimate conditional variables such as CPACK_PROPERTIES_FILE.
        if Path(item["path"]).name in ("CTestTestfile.cmake", "CTestCustom.cmake", "cmake_install.cmake"):
            cmake(build / item["path"])
    return [included[name] for name in sorted(included)]


def observe(source, build, mode, install):
    source = safe_path(str(source))
    build = safe_path(str(build))
    require(disjoint(source, build), "source and build must be separate trees")
    install = install_path(source, build, mode, install)
    with Directory(build) as directory:
        data = directory.read("CMakeCache.txt")
        entries = parse_cache(data)
        value = lambda key: entries[key]["value"]
        require(safe_path(value("CMAKE_HOME_DIRECTORY")) == source,
                "CMAKE_HOME_DIRECTORY identifies a different source worktree")
        require(safe_path(value("CMAKE_CACHEFILE_DIR")) == build,
                "CMAKE_CACHEFILE_DIR identifies a different build tree")
        require(value("CMAKE_GENERATOR") == "Ninja", "qualification requires the Ninja generator")
        ninja = directory.read("build.ninja")
        features = directory.read(FEATURES, optional=True)
        executables = {}
        for key in (*COMPILERS, *OTHER_TOOLS):
            if key in entries:
                tool = value(key)
                if key in OTHER_TOOLS and (not tool or tool.endswith("-NOTFOUND")):
                    continue
                executables[key] = executable_identity(tool)
        options = {key: entry for key, entry in entries.items() if key.startswith(
            ("CMAKE_", "VACARDS_", "WITH_", "ENABLE_", "TESTS_", "BUILD_"))}
        files = configuration_files(build)
        return {"source": repository_identity(source), "build": str(build),
                "build_directory": directory.info, "mode": mode,
                "install": str(install) if install else None, "generator": "Ninja",
                "cache_sha256": digest(data), "build_ninja_sha256": digest(ninja),
                "effective_features": ({"path": str(build / FEATURES), "sha256": digest(features)}
                                       if features is not None else None),
                "executables": executables, "options": options, "build_environment": build_environment(),
                "configuration_files": files, "control_includes": control_includes(source, build, files)}


def claim(source, build, mode, install=None):
    inputs = observe(source, build, mode, install)
    build = Path(inputs["build"])
    prefix = Path(inputs["install"]) if inputs["install"] else None
    with ExitStack() as stack:
        directory = stack.enter_context(Directory(build))
        parent = stack.enter_context(Directory(prefix.parent)) if prefix else None
        for name in (LEDGER, EVIDENCE, ".ninja_log"):
            require(not directory.exists(name), f"run is not fresh; existing {name}")
        if prefix:
            require(not os.path.lexists(prefix), "full install prefix already exists")
        # All static validation precedes the first output. O_EXCL is also the
        # interprocess lock. Never delete it on any subsequent error.
        directory.check()
        descriptor = directory.open_file(LEDGER, create=True)
        with os.fdopen(descriptor, "wb") as stream:
            directory.sync()
            # Recheck freshness under the claim lock; another producer may have
            # started after preflight. Such a race consumes this reservation.
            require(not directory.exists(".ninja_log"), "root Ninja log appeared during claim")
            evidence = directory.mkdir(EVIDENCE)
            installed = None
            if prefix:
                installed = parent.mkdir(prefix.name)
            require(canonical_json(observe(source, build, mode, install)) == canonical_json(inputs),
                    "inputs changed during reservation; preserve this failed run")
            require(not directory.exists(".ninja_log"), "root Ninja log appeared during claim")
            require(directory.child_identity(EVIDENCE) == evidence and
                    (not prefix or parent.child_identity(prefix.name) == installed),
                    "reserved output replaced during claim")
            require(identity(directory.stat(LEDGER)) == identity(os.fstat(stream.fileno())),
                    "exclusive ledger replaced during claim")
            record = {"schema_version": 1, "purpose": PURPOSE, "run_uuid": str(uuid.uuid4()),
                      "created_utc": datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
                      "root_ninja_log_at_claim": "absent", "inputs": inputs,
                      "configuration_sha256": digest(canonical_json(inputs)),
                      "outputs": {"evidence": evidence, "install": installed}}
            record["record_sha256"] = digest(canonical_json(record))
            stream.write(canonical_json(record) + b"\n")
            stream.flush()
            directory.readonly(stream.fileno())
            os.fsync(stream.fileno())
            directory.check()
        return record


def verify(build):
    build = safe_path(str(build))
    with Directory(build) as directory:
        data = directory.read(LEDGER)
        record = json.loads(data, object_pairs_hook=unique_object)
        fields = {"schema_version", "purpose", "run_uuid", "created_utc", "inputs",
                  "root_ninja_log_at_claim", "configuration_sha256", "outputs", "record_sha256"}
        require(type(record) is dict and set(record) == fields, "invalid ledger fields")
        require(type(record["schema_version"]) is int and record["schema_version"] == 1
                and record["purpose"] == PURPOSE and record["root_ninja_log_at_claim"] == "absent",
                "invalid ledger schema/purpose/history observation")
        run = uuid.UUID(record["run_uuid"])
        require(run.version == 4 and str(run) == record["run_uuid"], "invalid run UUID")
        created = datetime.strptime(record["created_utc"], "%Y-%m-%dT%H:%M:%SZ")
        require(created.strftime("%Y-%m-%dT%H:%M:%SZ") == record["created_utc"], "invalid creation time")
        checksum = {key: value for key, value in record.items() if key != "record_sha256"}
        require(record["record_sha256"] == digest(canonical_json(checksum)), "ledger checksum mismatch")
        inputs = record["inputs"]
        require(type(inputs) is dict and type(inputs.get("source")) is dict, "invalid recorded inputs")
        observed = observe(inputs["source"]["path"], build, inputs["mode"], inputs["install"])
        require(canonical_json(observed) == canonical_json(inputs), "source/configuration/tool/feature identity drift")
        require(record["configuration_sha256"] == digest(canonical_json(observed)),
                "configuration fingerprint mismatch")
        installed = None
        if observed["install"]:
            prefix = Path(observed["install"])
            with Directory(prefix.parent) as parent:
                installed = parent.child_identity(prefix.name)
        outputs = {"evidence": directory.child_identity(EVIDENCE), "install": installed}
        require(canonical_json(outputs) == canonical_json(record["outputs"]), "reserved outputs changed")
        directory.check()
        return record


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    reserve = commands.add_parser("claim")
    reserve.add_argument("--source", required=True)
    reserve.add_argument("--build", required=True)
    reserve.add_argument("--mode", required=True, choices=("quick", "full"))
    reserve.add_argument("--install")
    check = commands.add_parser("verify")
    check.add_argument("--build", required=True)
    args = parser.parse_args(argv)
    try:
        record = (claim(args.source, args.build, args.mode, args.install)
                  if args.command == "claim" else verify(args.build))
    except (GateError, OSError, ValueError, TypeError, KeyError, AttributeError, RecursionError) as error:
        parser.exit(1, f"VACards gate run rejected: {error}\n")
    print(f"{args.command}: {record['run_uuid']} (input reservation only; no success attestation)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
