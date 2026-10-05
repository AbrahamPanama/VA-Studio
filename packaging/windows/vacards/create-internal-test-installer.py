#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Stage/pack an unsigned, per-user VACards Test installer on native Windows.

This independent entry point never invokes or changes production release gates.
Only its new output directory is populated. The original debug app is retained.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys

HERE = Path(__file__).resolve().parent
MANIFEST = "share/vacards-test/WINDOWS-INTERNAL-TEST.json"

# Bounded retry for the whole generated removal list. gdbus.exe outlives the
# closed window by ~3-5s and holds its DLL closure; 30 x 1s tolerates that
# normal helper exit and transient AV/indexer locks. There is deliberately no
# /REBOOTOK: a pending reboot is not a completed uninstall.
REMOVE_RETRY_ATTEMPTS = 30
REMOVE_RETRY_INTERVAL_MS = 1000

# One NSI serves both explicitly chosen scopes. The machine scope is an
# owner-requested Program Files internal test; it never becomes the default.
SCOPES = ("user", "machine")
ARTIFACT_SCOPE_SUFFIX = {"user": "", "machine": "-programfiles"}


def require(ok, message):
    if not ok:
        raise RuntimeError(message)


def sha(path):
    with Path(path).open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def fields(path, cache=False):
    result = {}
    for line in Path(path).read_text(encoding="utf-8-sig").splitlines():
        if not line or line.startswith(("#", "//")) or "=" not in line:
            continue
        k, v = line.split("=", 1)
        if cache:
            k = k.split(":", 1)[0]
        require(k not in result, f"duplicate field: {k}")
        result[k] = v
    return result


def run(*args, env=None, log=None, quiet=False):
    args = [str(a) for a in args]
    if not quiet:
        print("+", subprocess.list2cmdline(args), flush=True)
    p = subprocess.run(args, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                       encoding="utf-8", errors="replace")
    if log:
        with Path(log).open("a", encoding="utf-8") as stream:
            stream.write(subprocess.list2cmdline(args) + "\n" + p.stdout + "\n")
    require(p.returncode == 0, f"command exited {p.returncode}: {p.stdout[-4000:]}")
    return p.stdout.strip()


def copy(source, destination):
    destination = Path(destination)
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(source, destination)


def stage_env(cairo, cdr, ucrt, environ=None):
    """Build the one explicit child-process environment used by every stage command.

    The approved prefixes and the UCRT64/MSYS2 shell directories are searched in
    a fixed order and the ambient PATH is deliberately not carried over, so an
    inherited entry cannot shadow an attested dependency. ``usr/bin/core_perl``
    is explicit because the tracked verifier calls ``shasum``, which MSYS2 ships
    there; without it the restricted PATH cannot find the checksum tool.
    ``LANG``/``LC_ALL`` are pinned to ``C`` for child processes only:
    ``dict(environ, ...)`` copies the ambient mapping, so the caller's
    environment is never modified. ``SystemRoot`` is read from the same ambient
    mapping that supplies every other inherited variable.
    """
    environ = os.environ if environ is None else environ
    system_root = Path(environ["SystemRoot"])
    directories = [cairo / "bin", cdr / "bin", ucrt / "bin", ucrt.parent / "usr/bin",
                   ucrt.parent / "usr/bin/core_perl", system_root / "System32", system_root]
    return dict(environ, PATH=";".join(map(str, directories)), LANG="C", LC_ALL="C")


def test_version(version, commit, rehearsal):
    require(version.get("format") in {"1", "2"}, "unsupported VERSION.env format")
    require(re.fullmatch(r"(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)",
                         version.get("base_version", "")), "invalid base version")
    require(version.get("edition") == "vacards", "expected VACards edition")
    require(re.fullmatch(r"[1-9][0-9]*", version.get("build_number", "")), "invalid build number")
    require(re.fullmatch(r"[0-9a-f]{40}", commit), "invalid source SHA")
    if version["format"] == "2":
        require(re.fullmatch(r"(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)-(beta|rc)\.[1-9][0-9]*",
                             version.get("product_version", "")), "invalid product version")
        require(re.fullmatch(r"[A-Za-z][A-Za-z0-9_]*", version.get("product_name", "")), "invalid product name")
        require(re.fullmatch(r"[0-9][A-Za-z0-9._-]*", version.get("display_version", "")), "invalid display version")
        return (f"{version['product_version']}-build.{version['build_number']}-internal-test-{commit[:10]}"
                + ("-rehearsal" if rehearsal else ""))
    return (f"{version['base_version']}-{version['edition']}.{version['build_number']}"
            f"-internal-test-{commit[:10]}" + ("-rehearsal" if rehearsal else ""))


def validate_features(dependencies, features):
    for field, expected in (("required_cmake_features", "ON"), ("disabled_cmake_features", "OFF")):
        names = dependencies[field].split(",")
        require(all(re.fullmatch(r"[A-Z][A-Z0-9_]*", name) for name in names), f"invalid {field}")
        for name in names:
            require(features.get(name) == expected,
                    f"CMake feature {name}: expected {expected}, got {features.get(name, 'missing')}")


def libcdr_test_evidence(prefix, record):
    # Older Windows prefixes include the result in their build manifest. The
    # shared verifier uses a closed schema, so newer prefixes keep Windows test
    # evidence separately and bind it to the same source and installed DLL.
    if record.get("tests") == "make-check-passed":
        return []
    receipt_path = prefix / "VACARDS-LIBCDR-TESTS.env"
    log_path = prefix / "VACARDS-LIBCDR-TESTS.log"
    receipt = fields(receipt_path)
    require(receipt.get("format") == "1" and receipt.get("tests") == "make-check-passed",
            "libcdr input has no passing Windows test record")
    for key in ("source_commit", "library_sha256"):
        require(receipt.get(key) == record[key], f"libcdr test evidence has different {key}")
    require(receipt.get("test_log_sha256") == sha(log_path), "libcdr test log changed")
    log = log_path.read_text(encoding="utf-8")
    require(re.search(r"^OK \([1-9][0-9]*\)$", log, re.MULTILINE) and
            re.search(r"^PASS test\.exe \(exit status: 0\)$", log, re.MULTILINE) and
            not re.search(r"\b(?:FAIL|ERROR|SKIP)\b", log), "libcdr native tests did not pass")
    return [receipt_path, log_path]


def librevenge_inputs(cache, prefix, source):
    """Validate the paired patched librevenge prefix before staging.

    A stock 0.0.6 DLL has the same release and pkg-config name, so release alone
    is not evidence. Require the ON cache option, the resolved prefix, the
    prefix-bound library bytes, and marker/capability equality with the tracked
    bundle manifest. Deliberately portable so the preflight test can exercise it
    without a Windows host.
    """
    require(cache.get("VACARDS_REQUIRE_PATCHED_LIBREVENGE") == "ON",
            "VACARDS_REQUIRE_PATCHED_LIBREVENGE=ON is required for internal packages")
    resolved = cache.get("VACARDS_RESOLVED_LIBREVENGE_PREFIX")
    require(resolved and Path(resolved).is_dir(), "resolved librevenge prefix is missing")
    require(Path(resolved).resolve() == Path(prefix).resolve(),
            "librevenge prefix differs from the resolved CMake prefix")
    prefix = Path(prefix)
    manifest = prefix / "VACARDS-LIBREVENGE.env"
    require(manifest.is_file(), f"missing librevenge manifest: {manifest}")
    record = fields(manifest)
    for name in ("format", "dependency", "release", "source_identity", "archive_filename",
                 "archive_sha256", "patch_filename", "patch_sha256", "capability_name",
                 "capability_value", "capability_binary_marker", "library_relative_path",
                 "library_sha256"):
        require(record.get(name), f"librevenge manifest is missing {name}")
    require(record["format"] == "1" and record["dependency"] == "librevenge" and
            record["release"] == "0.0.6", "unsupported librevenge manifest")
    relative = record["library_relative_path"]
    require(not Path(relative).is_absolute() and ".." not in Path(relative).parts,
            "unsafe librevenge library_relative_path")
    library = prefix / relative
    require(library.is_file(), f"missing attested librevenge library: {library}")
    require(re.fullmatch(r"[0-9a-f]{64}", record["library_sha256"]),
            "invalid librevenge library_sha256")
    require(sha(library) == record["library_sha256"],
            "librevenge library differs from its build provenance")
    tracked = fields(source / "packaging/dependencies/librevenge-0.0.6/VACARDS-LIBREVENGE-BUNDLE.env")
    for key in ("capability_name", "capability_value", "capability_binary_marker",
                "archive_sha256", "patch_sha256"):
        require(record[key] == tracked.get(key),
                f"librevenge {key} differs from the tracked bundle manifest")
    marker = record["capability_binary_marker"]
    require(marker.encode("utf-8") in library.read_bytes(),
            "librevenge DLL is missing the capability marker (stock 0.0.6 is not the patched build)")
    return record


def pe_export_names(headers):
    """Extract the Name Pointer Table's export names from objdump -p text.

    binutils 2.47 prints extra base and hint columns before the name, while
    older objdump prints the name directly after the ordinal::

        [   0] +base[   1]  0000 _gdk_win32_display_hcursor_ref   (2.47)
        [   0] _gdk_win32_display_hcursor_ref                     (older)

    The tracked producer's awk oracle takes the last whitespace field in both
    cases (``$NF``); do the same instead of assuming a fixed column. Each
    row still must carry a name token after the ordinal bracket, so a truncated
    table cannot contribute a bogus token.
    """
    names, in_table = [], False
    for line in headers.splitlines():
        if "[Ordinal/Name Pointer] Table" in line:
            in_table = True
            continue
        if not in_table:
            continue
        if not line.strip():
            in_table = False
            continue
        match = re.match(r"\s*\[\s*\d+\]\s+(?:.*\s)?(\S+)\s*$", line)
        if match:
            names.append(match.group(1))
    return names


def pe_tables(ucrt, path, env=None):
    """Read a PE module's import DLL names and export names via objdump -p.

    Used by gtk_inputs for the candidate DLL and the pinned stock baseline. The
    default probe is injectable so the portable preflight can exercise the
    validator without a Windows host or objdump.
    """
    headers = run(Path(ucrt) / "bin/objdump.exe", "-p", path, env=env, quiet=True)
    require("pei-x86-64" in headers, f"wrong PE architecture: {path}")
    imports = re.findall(r"DLL Name:\s*(\S+)", headers)
    require(imports, f"PE has no import inventory: {path}")
    exports = pe_export_names(headers)
    require(exports, f"PE has no export inventory: {path}")
    return imports, exports


def gtk_inventory(run_root, install):
    """Verify every entry of the run's VACARDS-GTK.sha256 byte inventory.

    The producer writes ``bin/`` and ``lib/`` entries relative to the install
    prefix but the ``abi/`` entry relative to the run root, so each entry is
    resolved against both roots (install first). Unsafe or missing paths and any
    byte drift fail closed.
    """
    for line in (run_root / "VACARDS-GTK.sha256").read_text(encoding="utf-8-sig").splitlines():
        match = re.fullmatch(r"([0-9a-f]{64})\s+[*]?(.+)", line)
        require(match, f"malformed GTK inventory line: {line!r}")
        expected, relative = match.group(1), match.group(2).strip()
        require(relative and not Path(relative).is_absolute() and ".." not in Path(relative).parts,
                f"unsafe GTK inventory path: {relative!r}")
        for base in (install, run_root):
            candidate = base / relative
            if candidate.is_file():
                require(sha(candidate) == expected, f"GTK inventory changed: {relative}")
                break
        else:
            raise RuntimeError(f"GTK inventory file missing: {relative}")


def gtk_dependency_manifest(path):
    result = {}
    for line in Path(path).read_text(encoding="utf-8").splitlines():
        match = re.fullmatch(r"([0-9a-f]{64})  ([a-z0-9_.+-]+\.dll)", line)
        require(match, f"malformed GTK dependency manifest entry: {line!r}")
        digest, name = match.groups()
        require(name not in result, f"duplicate GTK dependency entry: {name}")
        result[name] = digest
    require(result, "empty GTK dependency manifest")
    return result


def compare_gtk_dependencies(expected, staged, pinned=()):
    """Require every build-time GTK dependency to exist with identical bytes.

    Names in `pinned` are staged from a separately verified prefix (the pinned
    Cairo's libcairo-gobject/-script-interpreter replace the stock UCRT64
    copies the GTK build recorded); their own byte check covers them.
    """
    pinned = {name.lower() for name in pinned}
    for name, digest in expected.items():
        if name in pinned:
            continue
        require(name in staged, f"missing staged GTK dependency: {name}")
        require(staged[name] == digest, f"staged GTK dependency changed: {name}")


def gtk_inputs(run_root, source, ucrt=None, probe=None, baseline_dll=None, bundle=None):
    """Validate the tested patched GTK run root before staging.

    A stock 4.22.4 DLL shares the basename and version, so release alone is not
    evidence. Require the run's own manifest, toolchain record, ABI export list,
    byte inventory, tracked patch and the actual DLL's PE tables to agree with
    the tracked bundle, and require no new import over the pinned stock baseline.
    Deliberately portable: ``probe`` and ``baseline_dll`` are injectable.
    """
    run_root, source = Path(run_root), Path(source)
    require(run_root.is_dir(), f"GTK run root is not a directory: {run_root}")
    bundle = (Path(bundle) if bundle else
              source / "packaging/dependencies/gtk-4.22.4-win32-cairo-buffer/VACARDS-GTK-BUNDLE.env")
    require(bundle.is_file(), f"missing tracked GTK bundle manifest: {bundle}")
    pin = fields(bundle)
    for name in ("format", "dependency", "gtk_version", "platform", "architecture",
                 "source_archive_sha256", "msys2_commit", "msys2_pkgbuild_sha256",
                 "p001_sha256", "p003_sha256", "patch_filename", "patch_sha256",
                 "clipboard_patch_filename", "clipboard_patch_sha256",
                 "clipboard_priority_patch_filename", "clipboard_priority_patch_sha256",
                 "library_relative_path", "library_sha256", "exports_count",
                 "exports_sha256", "baseline_stock_library_sha256"):
        require(pin.get(name), f"GTK bundle manifest is missing {name}")
    require(pin["format"] == "3" and pin["dependency"] == "gtk4", "unsupported GTK bundle manifest")
    require(pin["gtk_version"] == "4.22.4" and pin["platform"] == "windows-ucrt64" and
            pin["architecture"] == "x86_64", "unsupported GTK bundle target")
    for name in ("library_sha256", "patch_sha256", "clipboard_patch_sha256",
                 "clipboard_priority_patch_sha256", "exports_sha256",
                 "baseline_stock_library_sha256"):
        require(re.fullmatch(r"[0-9a-f]{64}", pin[name]), f"invalid GTK bundle {name}")
    require(re.fullmatch(r"[1-9][0-9]*", pin["exports_count"]), "invalid GTK bundle exports_count")

    env_path, chain_path = run_root / "VACARDS-GTK.env", run_root / "toolchain.txt"
    require(env_path.is_file(), f"missing GTK run manifest: {env_path}")
    require(chain_path.is_file(), f"missing GTK toolchain record: {chain_path}")
    record, chain = fields(env_path), fields(chain_path)
    for name in ("impl_patch_sha256", "libgtk_4_1_dll_sha256", "exports_sha256",
                 "exports_count", "inventory_sha256"):
        require(record.get(name), f"GTK run manifest is missing {name}")
    require(chain.get("format") == "3" and chain.get("platform") == pin["platform"] and
            chain.get("architecture") == pin["architecture"], "unsupported GTK toolchain record")
    require(chain.get("gtk_version") == pin["gtk_version"],
            "GTK version differs from the tracked bundle")
    for name in ("gtk_tarball_sha256", "msys2_commit", "msys2_pkgbuild_sha256",
                 "p001_sha256", "p003_sha256", "impl_patch_sha256"):
        require(chain.get(name), f"GTK toolchain record is missing {name}")
    require(chain.get("configure_only") == "0", "GTK run was configure-only")
    for name, value in (("gtk_tarball_sha256", pin["source_archive_sha256"]),
                        ("msys2_commit", pin["msys2_commit"]),
                        ("msys2_pkgbuild_sha256", pin["msys2_pkgbuild_sha256"]),
                        ("p001_sha256", pin["p001_sha256"]),
                        ("p003_sha256", pin["p003_sha256"]),
                        ("impl_patch_sha256", pin["patch_sha256"])):
        require(chain[name] == value, f"GTK toolchain {name} differs from the tracked bundle")

    clipboard_name = pin["clipboard_patch_filename"]
    require(clipboard_name == "gtk-4.22.4-win32-clipboard-empty.patch",
            "unsupported GTK clipboard patch filename")
    clipboard_patch = source / "packaging/windows/vacards" / clipboard_name
    require(clipboard_patch.is_file() and sha(clipboard_patch) == pin["clipboard_patch_sha256"],
            "GTK clipboard patch differs from the tracked bundle")
    copied_clipboard_patch = run_root / "inputs" / clipboard_name
    require(copied_clipboard_patch.is_file() and
            sha(copied_clipboard_patch) == pin["clipboard_patch_sha256"],
            "GTK run clipboard patch differs from the tracked bundle")
    for provenance in (record, chain):
        require(provenance.get("clipboard_patch_filename") == clipboard_name and
                provenance.get("clipboard_patch_sha256") == pin["clipboard_patch_sha256"],
                "GTK clipboard patch differs from build provenance")

    clipboard_priority_name = pin["clipboard_priority_patch_filename"]
    require(clipboard_priority_name == "gtk-4.22.4-win32-clipboard-format-priority.patch",
            "unsupported GTK clipboard_priority patch filename")
    clipboard_priority_patch = source / "packaging/windows/vacards" / clipboard_priority_name
    require(clipboard_priority_patch.is_file() and sha(clipboard_priority_patch) == pin["clipboard_priority_patch_sha256"],
            "GTK clipboard_priority patch differs from the tracked bundle")
    copied_clipboard_priority_patch = run_root / "inputs" / clipboard_priority_name
    require(copied_clipboard_priority_patch.is_file() and
            sha(copied_clipboard_priority_patch) == pin["clipboard_priority_patch_sha256"],
            "GTK run clipboard_priority patch differs from the tracked bundle")
    for provenance in (record, chain):
        require(provenance.get("clipboard_priority_patch_filename") == clipboard_priority_name and
                provenance.get("clipboard_priority_patch_sha256") == pin["clipboard_priority_patch_sha256"],
                "GTK clipboard_priority patch differs from build provenance")

    relative = pin["library_relative_path"]
    require(not Path(relative).is_absolute() and ".." not in Path(relative).parts,
            "unsafe GTK library_relative_path")
    install = run_root / "install"
    library = install / relative
    require(library.is_file(), f"missing attested GTK library: {library}")
    require(record["libgtk_4_1_dll_sha256"] == pin["library_sha256"],
            "GTK library hash differs from the tracked bundle")
    require(sha(library) == pin["library_sha256"],
            "GTK library differs from its build provenance")
    patch = source / "packaging/windows/vacards" / pin["patch_filename"]
    require(patch.is_file(), f"missing tracked GTK patch: {patch}")
    require(record["impl_patch_sha256"] == pin["patch_sha256"] and
            sha(patch) == pin["patch_sha256"], "GTK patch differs from the tracked bundle")
    copied_patch = run_root / "inputs" / pin["patch_filename"]
    require(copied_patch.is_file() and sha(copied_patch) == pin["patch_sha256"],
            "GTK run Cairo patch differs from the tracked bundle")
    require(record["exports_sha256"] == pin["exports_sha256"] and
            record["exports_count"] == pin["exports_count"],
            "GTK export identity differs from the tracked bundle")

    exports_path = run_root / "abi/libgtk-4-1.exports.txt"
    require(exports_path.is_file(), f"missing GTK ABI export record: {exports_path}")
    require(sha(exports_path) == pin["exports_sha256"], "GTK ABI export record changed")
    exports_lines = exports_path.read_text(encoding="utf-8").splitlines()
    require(exports_lines and len(exports_lines) == int(pin["exports_count"]),
            "GTK ABI export count differs from the tracked bundle")

    require((run_root / "VACARDS-GTK.sha256").is_file(), "missing GTK inventory")
    require(sha(run_root / "VACARDS-GTK.sha256") == record["inventory_sha256"],
            "GTK inventory record changed")
    gtk_inventory(run_root, install)
    dep_manifest = run_root / "abi/gtk-ucrt64-dlls.sha256"
    require(record.get("ucrt64_dll_manifest_sha256") == sha(dep_manifest),
            "GTK UCRT64 dependency manifest differs from build provenance")
    gtk_dependency_manifest(dep_manifest)

    baseline_dll = Path(baseline_dll) if baseline_dll else Path(ucrt) / "bin/libgtk-4-1.dll"
    require(baseline_dll.is_file(), f"missing stock GTK baseline: {baseline_dll}")
    require(sha(baseline_dll) == pin["baseline_stock_library_sha256"],
            "stock GTK baseline differs from the tracked pin")

    probe = probe or (lambda path: pe_tables(ucrt, path))
    candidate_imports, candidate_exports = probe(library)
    baseline_imports, baseline_exports = probe(baseline_dll)
    expected_exports = "".join(f"{name}\n" for name in sorted(set(candidate_exports)))
    require(exports_path.read_bytes() == expected_exports.encode("utf-8"),
            "GTK DLL exports do not byte-match the ABI export record")
    require(sorted(set(baseline_exports)) == sorted(set(candidate_exports)),
            "stock GTK baseline export set differs from the candidate")
    extra = {name.lower() for name in candidate_imports} - {name.lower() for name in baseline_imports}
    require(not extra, f"patched GTK adds runtime imports: {sorted(extra)}")

    return dict(gtk_version=pin["gtk_version"], platform=pin["platform"],
                architecture=pin["architecture"], library_relative_path=relative,
                library_sha256=pin["library_sha256"], patch_sha256=pin["patch_sha256"],
                clipboard_patch_sha256=pin["clipboard_patch_sha256"],
                clipboard_priority_patch_sha256=pin["clipboard_priority_patch_sha256"],
                exports_sha256=pin["exports_sha256"], exports_count=pin["exports_count"],
                baseline_stock_library_sha256=pin["baseline_stock_library_sha256"],
                inventory_sha256=record["inventory_sha256"], env_sha256=sha(env_path),
                dependency_manifest_sha256=sha(dep_manifest),
                toolchain_sha256=sha(chain_path), abi_sha256=pin["exports_sha256"],
                bundle_sha256=sha(bundle), run_root=str(run_root))


def utf8_process_manifest(executable):
    """Update only the copied app's manifest, retaining DPI and other resources.

    Fontconfig's narrow Windows getenv/file APIs need a UTF-8 process code page
    for non-ASCII install paths. This changes no machine/user locale setting.
    """
    import ctypes as c
    from ctypes import wintypes as w
    kernel = c.WinDLL("kernel32", use_last_error=True)
    signatures = {
        "LoadLibraryExW": ([w.LPCWSTR, w.HANDLE, w.DWORD], w.HMODULE),
        "FreeLibrary": ([w.HMODULE], w.BOOL),
        "FindResourceExW": ([w.HMODULE, c.c_void_p, c.c_void_p, w.WORD], w.HANDLE),
        "SizeofResource": ([w.HMODULE, w.HANDLE], w.DWORD),
        "LoadResource": ([w.HMODULE, w.HANDLE], w.HANDLE),
        "LockResource": ([w.HANDLE], c.c_void_p),
        "BeginUpdateResourceW": ([w.LPCWSTR, w.BOOL], w.HANDLE),
        "UpdateResourceW": ([w.HANDLE, c.c_void_p, c.c_void_p, w.WORD, c.c_void_p, w.DWORD], w.BOOL),
        "EndUpdateResourceW": ([w.HANDLE, w.BOOL], w.BOOL),
    }
    for name, (arguments, result) in signatures.items():
        function = getattr(kernel, name)
        function.argtypes, function.restype = arguments, result
    callback_type = c.WINFUNCTYPE(w.BOOL, w.HMODULE, c.c_void_p, c.c_void_p, w.WORD, c.c_ssize_t)
    kernel.EnumResourceLanguagesW.argtypes = [w.HMODULE, c.c_void_p, c.c_void_p, callback_type, c.c_ssize_t]
    kernel.EnumResourceLanguagesW.restype = w.BOOL
    languages, updated = [], {}
    callback = callback_type(lambda module, kind, name, language, data: languages.append(language) or True)
    module = kernel.LoadLibraryExW(str(executable), None, 0x22)
    require(module, "cannot inspect staged manifest")
    try:
        require(kernel.EnumResourceLanguagesW(module, 24, 1, callback, 0), "app manifest missing")
        for language in languages:
            resource = kernel.FindResourceExW(module, 24, 1, language)
            require(resource, "manifest resource missing")
            size = kernel.SizeofResource(module, resource)
            pointer = kernel.LockResource(kernel.LoadResource(module, resource))
            require(pointer and size, "cannot read app manifest")
            body = c.string_at(pointer, size).rstrip(b"\0")
            if b"activeCodePage" not in body:
                require(body.count(b"</windowsSettings>") == 1, "unexpected Windows manifest structure")
                body = body.replace(b"</windowsSettings>",
                    b'<activeCodePage xmlns="http://schemas.microsoft.com/SMI/2019/WindowsSettings">'
                    b'UTF-8</activeCodePage>\n    </windowsSettings>')
            require(b">UTF-8</activeCodePage>" in body, "unexpected existing process code page")
            updated[language] = body
    finally:
        kernel.FreeLibrary(module)
    handle = kernel.BeginUpdateResourceW(str(executable), False)
    require(handle, "cannot update copied app manifest")
    try:
        for language, body in updated.items():
            buffer = c.create_string_buffer(body)
            require(kernel.UpdateResourceW(handle, 24, 1, language, buffer, len(body)), "manifest update failed")
    except BaseException:
        kernel.EndUpdateResourceW(handle, True)
        raise
    require(kernel.EndUpdateResourceW(handle, False), "manifest update could not be saved")
    return {str(language): hashlib.sha256(body).hexdigest() for language, body in updated.items()}


def payload_files(root):
    files = {}
    for path in sorted(root.rglob("*")):
        require(not path.is_symlink() and not path.is_junction(), f"linked payload: {path}")
        if path.is_file():
            relative = path.relative_to(root).as_posix()
            require(not any(c in relative for c in '$"\r\n'), f"unsafe installer path: {relative}")
            files[relative] = sha(path)
    return files


def remove_list_lines(files, directories):
    """Render the exact, manifest-driven removal list for the uninstaller.

    One ``Delete`` per owned payload file, followed by an ``IfErrors`` check
    that raises ``$RemoveFailure``. ``Delete`` never sets the error flag for a
    file that does not exist and ``IfErrors`` clears the flag after each check,
    so the enclosing bounded retry loop can safely re-run the whole list and
    detect whether any single owned file is still locked. Directory removal is
    non-recursive and best-effort; the deliberately preserved unknown sentinel
    keeps the install root non-empty by design.
    """
    lines = []
    for relative in files:
        lines.append(f'Delete "$INSTDIR\\{relative.replace("/", chr(92))}"')
        lines.append("IfErrors 0 +2")
        lines.append("  StrCpy $RemoveFailure 1")
    for relative in directories:
        lines.append(f'RMDir "$INSTDIR\\{relative.replace("/", chr(92))}"')
    return lines


def complete_pe_closure(stage, ucrt, approved, env, log):
    """Inspect all executable/module imports, including Python's .pyd modules."""
    scanned, records = set(), []
    system = Path(os.environ["SystemRoot"]) / "System32"
    candidates = {p.name.lower(): p for p in (ucrt / "bin").glob("*.dll")}
    candidates.update({p.name.lower(): p for p in approved})
    while True:
        pending = [p for p in stage.rglob("*") if p.is_file() and
                   p.suffix.lower() in (".exe", ".com", ".dll", ".pyd") and p not in scanned]
        if not pending:
            break
        for pe in pending:
            headers = run(ucrt / "bin/objdump.exe", "-p", pe, env=env, quiet=True)
            require("pei-x86-64" in headers, f"wrong PE architecture: {pe}")
            imports = re.findall(r"DLL Name:\s*(\S+)", headers)
            require(imports, f"PE has no import inventory: {pe}")
            for name in imports:
                lower = name.lower()
                if lower.startswith(("api-ms-win-", "ext-ms-win-")) or (system / name).is_file():
                    continue
                if (pe.parent / name).is_file() or (stage / "bin" / name).is_file():
                    continue
                require(lower in candidates, f"unresolved {name} imported by {pe}")
                copy(candidates[lower], stage / "bin" / name)
            records.append({"file": pe.relative_to(stage).as_posix(), "imports": imports})
            scanned.add(pe)
    Path(log).write_text(json.dumps(records, indent=2), encoding="utf-8")
    return len(scanned)


def stage(args):
    require(os.name == "nt", "Use native Windows Python on the UCRT64 build host")
    build, source, ucrt = args.build.resolve(), args.source.resolve(), args.ucrt.resolve()
    cache = fields(build / "CMakeCache.txt", cache=True)
    require(Path(cache["CMAKE_HOME_DIRECTORY"]).resolve() == source, "build/source mismatch")
    require(re.fullmatch(r"[0-9a-f]{40}", args.source_commit), "expected full frozen source SHA")
    git = ucrt.parent / "usr/bin/git.exe"
    git_env = dict(os.environ, PATH=str(git.parent) + ";" + os.environ["PATH"])
    head = run(git, "-C", source, "rev-parse", "HEAD", env=git_env)
    require(head == args.source_commit, "source HEAD differs from requested commit")
    status = run(git, "-C", source, "status", "--porcelain", "--untracked-files=no", env=git_env)
    require(not status or args.rehearsal, "commit tracked source edits first")
    submodules = run(git, "-C", source, "submodule", "status", "--recursive", env=git_env)
    require(not any(line.startswith(("-", "+", "U")) for line in submodules.splitlines()),
            "submodules differ from pinned gitlinks")
    deps = fields(source / "VACARDS-DEPENDENCIES.env")
    features = fields(build / "VACARDS-CMAKE-FEATURES.env")
    validate_features(deps, features)
    cairo = Path(cache["VACARDS_RESOLVED_CAIRO_PREFIX"])
    cdr = Path(cache["VACARDS_RESOLVED_LIBCDR_PREFIX"])
    require(Path(cache["VACARDS_LIBCDR_PREFIX"]).resolve() == cdr.resolve(), "libcdr prefix mismatch")
    require(sha(args.icc) == deps["tiff_rgb_profile_sha256"], "wrong ICC bytes")
    helper = build / "bin/vacards-sparrow.exe"
    require(sha(helper) == deps["sparrow_windows_x64_sha256"], "wrong Sparrow bytes")
    cdr_record = fields(cdr / "VACARDS-LIBCDR.env")
    require(cdr_record["source_commit"] == deps["libcdr_commit"], "wrong enhanced libcdr source")
    require(sha(cdr / "bin/libcdr-0.1.dll") == cdr_record["library_sha256"], "libcdr changed")
    cdr_test_files = libcdr_test_evidence(cdr, cdr_record)
    librevenge = Path(cache["VACARDS_RESOLVED_LIBREVENGE_PREFIX"])
    librevenge_record = librevenge_inputs(cache, librevenge, source)
    # The internal-test channel requires a validated patched GTK run on this
    # branch. There is no optional path and no silent stock fallback.
    gtk_run_root = args.gtk_run.resolve()
    gtk_record = gtk_inputs(gtk_run_root, source, ucrt)
    gtk_dll = gtk_run_root / "install" / gtk_record["library_relative_path"]
    cairo_record = fields(cairo / "VACARDS-CAIRO.env")
    require(cairo_record["clipping_test"] == "passed" and
            cairo_record["upstream_fix"] == deps["cairo_fix_commit"] and
            cairo_record["source_archive_sha256"] == deps["cairo_release_sha256"], "wrong Cairo record")
    require(sha(cairo / "VACARDS-CAIRO.sha256") == cairo_record["inventory_sha256"], "Cairo inventory changed")
    require(sha(source / "packaging/macos/vacards/cairo-1.18.4-clip-all.patch") ==
            cairo_record["patch_sha256"], "Cairo patch differs from source")
    for line in (cairo / "VACARDS-CAIRO.sha256").read_text().splitlines():
        digest, relative = line.split("  ", 1)
        require(not Path(relative).is_absolute() and ".." not in Path(relative).parts, "unsafe Cairo inventory")
        require(sha(cairo / relative) == digest, f"Cairo input changed: {relative}")
    env = stage_env(cairo, cdr, ucrt)
    version = run(build / "bin/inkscape.com", "--version", env=env)
    require(args.source_commit[:10] in version or args.rehearsal, f"rebuild frozen commit first: {version}")
    version_fields = fields(source / "packaging/vacards/VERSION.env")
    if version_fields.get("format") == "2":
        expected_product = version_fields["product_name"].replace("_", " ") + " " + version_fields["display_version"].replace("_", " ")
        require(version.startswith(expected_product + " ("), "executable product identity is missing or stale")
    release = test_version(version_fields, args.source_commit, args.rehearsal)
    out = args.output.resolve()
    require(not out.is_relative_to(source) and not out.is_relative_to(build), "output must be separate")
    out.mkdir()  # Preserve previous attempts; never reuse an output directory.
    payload = out / "payload"
    payload.mkdir()
    log = out / "stage.log"
    # Re-run the tracked platform verifier against the resolved prefix. The
    # in-process gate above already binds marker/capability/hash; this proves
    # the shell verifier and the prefix agree in the UCRT64 environment.
    run(ucrt.parent / "usr/bin/bash.exe",
        source / "packaging/windows/vacards/verify-vacards-librevenge.sh", librevenge,
        env=dict(env, MSYSTEM="UCRT64"), log=log)
    originals = {name: sha(build / "bin" / name) for name in ("inkscape.exe", "inkscape.com", "vastudio-cli.exe")}
    # This small existing target generates fonts.conf; it does not rebuild the app.
    run(ucrt / "bin/cmake.exe", "--build", build, "--parallel", "2", "--target", "fonts_conf",
        env=env, log=log)
    # Existing top-level rules supply the GTK/Python runtime. LOCAL_ONLY omits
    # app targets (inkview isn't built by the app-only parity build) and po.
    # The separate share script installs the actual application data/extensions.
    run(ucrt / "bin/cmake.exe", f"-DCMAKE_INSTALL_PREFIX={payload.as_posix()}",
        "-DCMAKE_INSTALL_LOCAL_ONLY=1", "-P", build / "cmake_install.cmake", env=env, log=log)
    run(ucrt / "bin/cmake.exe", f"-DCMAKE_INSTALL_PREFIX={payload.as_posix()}",
        "-P", build / "share/cmake_install.cmake", env=env, log=log)
    # Overwrite the ambient /ucrt64 GTK runtime with the verified patched DLL
    # before the import closure runs. complete_pe_closure only fills missing
    # imports, so a stock same-basename DLL would otherwise survive staging.
    copy(gtk_dll, payload / "bin" / gtk_dll.name)
    app_manifests = {}
    for name in originals:
        copy(build / "bin" / name, payload / "bin" / name)
        run(ucrt / "bin/strip.exe", "--strip-debug", payload / "bin" / name, env=env, log=log)
        app_manifests[name] = utf8_process_manifest(payload / "bin" / name)
    copy(helper, payload / "bin/vacards-sparrow.exe")
    copy(source / "src/3rdparty/sparrow/LICENSE", payload / "share/licenses/sparrow-MIT.txt")
    copy(args.icc, payload / "share/inkscape/color/icc/TheBest.icc")
    for name in ("hyph_en_US.dic", "hyph_es.dic", "README_hyph_en_US.txt", "README_hyph_es.txt", "LICENSE-es.md", "GPLv3-es.txt"):
        require((args.hyphen / name).is_file(), f"missing dictionary/license: {name}")
        destination = "hyph_es_ES.dic" if name == "hyph_es.dic" else name
        copy(args.hyphen / name, payload / "share/inkscape/hyphen" / destination)
    # Include complete pinned-source license texts omitted by the runtime rules.
    shutil.copytree(source / "LICENSES", payload / "share/licenses/inkscape", dirs_exist_ok=True)
    # MSYS2 stores third-party license texts here, including Python and the
    # extension prerequisites. Package records/dist-info come from the install rules.
    shutil.copytree(ucrt / "share/licenses", payload / "share/licenses/ucrt64", dirs_exist_ok=True)
    for name in ("gdk-pixbuf-query-loaders.exe", "glib-compile-schemas.exe", "gsettings.exe"):
        copy(ucrt / "bin" / name, payload / "bin" / name)
    if (ucrt / "lib/gio/modules").is_dir():
        shutil.copytree(ucrt / "lib/gio/modules", payload / "lib/gio/modules", dirs_exist_ok=True)
    # Old convenience/debug launchers skip the isolated identity; omit from this new payload.
    for path in payload.glob("*.bat"):
        path.unlink()
    icon = payload / "share/vacards-test/VACards-AppIcon.ico"
    # Share one official converted ICO with inkscape.exe, launcher and NSIS.
    copy(source / "share/branding/VACards-AppIcon.ico", icon)
    gtk_icon = payload / "share/inkscape/icons/com.vacards.Inkscape.png"
    require(sha(gtk_icon) == sha(source / "share/icons/com.vacards.Inkscape.png"),
            "missing or substituted GTK application icon")
    resource = out / "launcher-icon.rc"
    resource.write_text('1 ICON "' + icon.as_posix() + '"\n', encoding="utf-8")
    run(ucrt / "bin/windres.exe", resource, "-O", "coff", "-o", out / "launcher-icon.o", env=env, log=log)
    run(ucrt / "bin/g++.exe", "-std=c++17", "-O2", "-s", "-static", "-municode", "-mwindows",
        '-DVACARDS_BUILD_VERSION="' + version_fields.get("product_version", version_fields["base_version"]) + '"',
        '-DVACARDS_BUILD_NUMBER="' + version_fields["build_number"] + '"',
        '-DVACARDS_SOURCE_COMMIT="' + args.source_commit + '"',
        HERE / "internal-test-launcher.cpp", out / "launcher-icon.o", "-o", payload / "VACards-Test.exe", "-lshell32", "-lole32", "-luuid", "-luser32",
        env=env, log=log)
    # VIEW-1: Explorer SVG thumbnail provider (framed on the drawing, rendered
    # by the system WebView2 runtime) and the pinned WebView2 loader it loads.
    shell_source = HERE / "svg-shell"
    run(ucrt / "bin/g++.exe", "-std=c++20", "-O2", "-s", "-static", "-shared", "-municode",
        shell_source / "vasvgthumb.cpp", shell_source / "vasvgthumb.def", "-o", payload / "bin/vasvgthumb.dll",
        "-lole32", "-loleaut32", "-luuid", "-lshlwapi", "-lwindowscodecs", "-luser32", "-lgdi32", "-lshell32",
        env=env, log=log)
    webview2_loader = ucrt / "bin/WebView2Loader.dll"
    require(webview2_loader.is_file(), "missing WebView2Loader.dll (mingw-w64-ucrt-x86_64-webview2-loader)")
    copy(webview2_loader, payload / "bin/WebView2Loader.dll")
    webview2_package = run(ucrt.parent / "usr/bin/pacman.exe", "-Q", "mingw-w64-ucrt-x86_64-webview2-loader",
                           env=env).strip()
    librevenge_dll = librevenge / librevenge_record["library_relative_path"]
    approved = (list((cairo / "bin").glob("libcairo*.dll")) +
                [cdr / "bin/libcdr-0.1.dll", librevenge_dll, gtk_dll])
    # A stock same-basename librevenge may already be staged by the GTK/Python
    # runtime install rules. Overwrite it with the verified paired prefix copy
    # before the import closure runs so the imported name resolves to the
    # attested bytes; the digest check below fails closed if that did not hold.
    copy(librevenge_dll, payload / "bin" / librevenge_dll.name)
    pe_count = complete_pe_closure(payload, ucrt, approved, env, out / "pe-imports.json")
    gtk_deps = gtk_dependency_manifest(gtk_run_root / "abi/gtk-ucrt64-dlls.sha256")
    staged_deps = {p.name.lower(): sha(p) for p in (payload / "bin").glob("*.dll")
                   if p.name.lower() in gtk_deps}
    compare_gtk_dependencies(gtk_deps, staged_deps, pinned=[dll.name for dll in approved])
    for dll in approved:
        require(sha(payload / "bin" / dll.name) == sha(dll), f"staged dependency substituted: {dll.name}")
    require(sha(payload / "bin" / librevenge_dll.name) == librevenge_record["library_sha256"],
            "staged librevenge bytes differ from the verified runtime dependency")
    require(sha(payload / "bin" / gtk_dll.name) == gtk_record["library_sha256"],
            "staged GTK bytes differ from the verified patched runtime dependency")
    run(payload / "bin/glib-compile-schemas.exe", "--strict", payload / "share/glib-2.0/schemas", env=env, log=log)
    required = ["bin/inkscape.exe", "bin/inkscape.com", "bin/vastudio-cli.exe",
                "bin/python.exe", "bin/libgtk-4-1.dll", "etc/fonts/fonts.conf",
                "lib/gdk-pixbuf-2.0/2.10.0/loaders/pixbufloader_svg.dll",
                "share/inkscape/ui/menus.ui", "share/inkscape/extensions/inkex/__init__.py",
                "share/glib-2.0/schemas/gschemas.compiled", "share/icons/Adwaita/index.theme",
                "bin/vasvgthumb.dll", "bin/WebView2Loader.dll"]
    for name in required:
        require((payload / name).is_file(), f"missing runtime: {name}")
    require(sha(payload / "bin/vacards-sparrow.exe") == deps["sparrow_windows_x64_sha256"], "Sparrow changed")
    for name, digest in originals.items():
        require(sha(build / "bin" / name) == digest, f"build input changed during staging: {name}")
    for original in (source / "VACARDS-DEPENDENCIES.env", build / "VACARDS-CMAKE-FEATURES.env",
                     cdr / "VACARDS-LIBCDR.env", cairo / "VACARDS-CAIRO.env",
                     librevenge / "VACARDS-LIBREVENGE.env", *cdr_test_files):
        copy(original, payload / "share/vacards-test" / original.name)
    # Carry the validated GTK run provenance beside the pinned dependency records.
    for original, name in ((gtk_run_root / "VACARDS-GTK.env", "VACARDS-GTK.env"),
                           (gtk_run_root / "VACARDS-GTK.sha256", "VACARDS-GTK.sha256"),
                           (gtk_run_root / "toolchain.txt", "VACARDS-GTK.toolchain.txt"),
                           (gtk_run_root / "abi/gtk-ucrt64-dlls.sha256", "VACARDS-GTK.dependencies.sha256"),
                           (gtk_run_root / "abi/libgtk-4-1.exports.txt", "VACARDS-GTK.exports.txt")):
        copy(original, payload / "share/vacards-test" / name)
    copy(source / "packaging/vacards/VERSION.env", payload / "share/vacards-test/VACARDS-VERSION.env")
    record = dict(schema_version=1, channel="internal-test", production_qualified=False,
                  signed=False, rehearsal=args.rehearsal, source_commit=head, release_version=release,
                  version_fields=version_fields,
                  validated_cmake_features=features,
                  application_version=version, original_application_sha256=originals,
                  transformation="strip --strip-debug and UTF-8 process manifest, staged app copies only",
                  app_manifest_sha256=app_manifests,
                  cairo_sha256={p.name: sha(p) for p in approved if "cairo" in p.name},
                  libcdr_sha256=cdr_record["library_sha256"], sparrow_sha256=sha(helper),
                  librevenge_sha256=librevenge_record["library_sha256"],
                  librevenge_manifest_sha256=sha(librevenge / "VACARDS-LIBREVENGE.env"),
                  librevenge_relative_path=librevenge_record["library_relative_path"],
                  librevenge_capability={
                      "name": librevenge_record["capability_name"],
                      "value": librevenge_record["capability_value"],
                      "binary_marker": librevenge_record["capability_binary_marker"]},
                  gtk_sha256=gtk_record["library_sha256"],
                  gtk_relative_path=gtk_record["library_relative_path"],
                  gtk_patch_sha256=gtk_record["patch_sha256"],
                  gtk_clipboard_patch_sha256=gtk_record["clipboard_patch_sha256"],
                  gtk_clipboard_priority_patch_sha256=gtk_record["clipboard_priority_patch_sha256"],
                  gtk_exports_sha256=gtk_record["exports_sha256"],
                  gtk_exports_count=int(gtk_record["exports_count"]),
                  gtk_baseline_stock_library_sha256=gtk_record["baseline_stock_library_sha256"],
                  gtk_env_sha256=gtk_record["env_sha256"],
                  gtk_inventory_sha256=gtk_record["inventory_sha256"],
                  gtk_toolchain_sha256=gtk_record["toolchain_sha256"],
                  gtk_dependency_manifest_sha256=gtk_record["dependency_manifest_sha256"],
                  gtk_bundle_sha256=gtk_record["bundle_sha256"],
                  gtk_run_root=gtk_record["run_root"],
                  icc_sha256=sha(args.icc), pe_count=pe_count, submodules=submodules.splitlines(),
                  official_icon_source_sha256=sha(source / "packaging/macos/res/VACards-AppIcon.png"),
                  official_icon_sha256=sha(icon), gtk_app_icon_sha256=sha(gtk_icon),
                  configuration=cache,
                  compiler_version=run(ucrt / "bin/g++.exe", "--version", env=env),
                  compiler_sha256=sha(ucrt / "bin/g++.exe"),
                  cmake_cache_sha256=sha(build / "CMakeCache.txt"),
                  packaging_source_sha256={p.name: sha(p) for p in HERE.glob("*internal-test*") if p.is_file()},
                  svg_shell_source_sha256={p.name: sha(p) for p in shell_source.glob("*") if p.is_file()},
                  svg_shell_sha256=sha(payload / "bin/vasvgthumb.dll"),
                  webview2_loader_sha256=sha(payload / "bin/WebView2Loader.dll"),
                  webview2_loader_package=webview2_package,
                  omitted_optional_applications=["inkview"], payload_sha256=payload_files(payload))
    (payload / MANIFEST).write_text(json.dumps(record, indent=2) + "\n", encoding="utf-8")
    print(f"STAGED {payload}", flush=True)


def artifact_path(output, release, scope):
    """Distinguish the Program Files variant from the default per-user artifact."""
    require(scope in SCOPES, f"unknown installer scope: {scope}")
    return Path(output) / f"VA-Studio-{release}-windows-x64{ARTIFACT_SCOPE_SUFFIX[scope]}.exe"


def pack_defines(scope, release, version_fields, payload, remove_list, artifact):
    """Assemble the makensis defines for the selected installer scope.

    ``SCOPE_MACHINE`` is the single switch selecting the admin/HKLM/Program
    Files branch inside the one tracked NSI; the default user scope passes no
    scope define and keeps the existing per-user behavior.
    """
    require(scope in SCOPES, f"unknown installer scope: {scope}")
    defines = [
        "/V3",
        f"/DTEST_VERSION={release}",
        "/DDISPLAY_VERSION=" + version_fields.get("display_version", release).replace("_", " "),
        f"/DREMOVE_RETRY_ATTEMPTS={REMOVE_RETRY_ATTEMPTS}",
        f"/DREMOVE_RETRY_INTERVAL_MS={REMOVE_RETRY_INTERVAL_MS}",
        # MSYS2 Python can stringify Windows paths with forward slashes.
        # Native makensis accepts these for File but rejects them in !include.
        "/DPAYLOAD=" + Path(payload).as_posix().replace("/", "\\"),
        "/DREMOVE_LIST=" + Path(remove_list).as_posix().replace("/", "\\"),
        "/DOUTPUT_FILE=" + Path(artifact).as_posix().replace("/", "\\"),
    ]
    if scope == "machine":
        defines.append("/DSCOPE_MACHINE")
    return defines


def pack(args):
    require(os.name == "nt", "Pack on native Windows")
    payload = args.payload.resolve()
    record = json.loads((payload / MANIFEST).read_text())
    require(record["channel"] == "internal-test" and record["production_qualified"] is False, "not an internal stage")
    current = payload_files(payload)
    current.pop(MANIFEST)
    require(current == record["payload_sha256"], "staged payload changed")
    gtk_pin = record.get("gtk_sha256")
    require(isinstance(gtk_pin, str) and re.fullmatch(r"[0-9a-f]{64}", gtk_pin),
            "stage has no valid gtk_sha256")
    gtk_relative = Path(record.get("gtk_relative_path", ""))
    require(gtk_relative.as_posix() == "bin/libgtk-4-1.dll", "invalid staged GTK path")
    require(sha(payload / gtk_relative) == gtk_pin, "staged GTK differs from its pin")
    release = record["release_version"]
    version_fields = fields(HERE.parents[1] / "vacards/VERSION.env")
    require(record["version_fields"] == version_fields, "stage/common VERSION.env mismatch")
    require(release == test_version(version_fields, record["source_commit"], record["rehearsal"]),
            "invalid test version")
    scope = args.scope
    output = args.output.resolve()
    output.mkdir()
    remove = output / "remove-payload.nsh"
    dirs = sorted((p for p in payload.rglob("*") if p.is_dir()),
                  key=lambda p: len(p.parts), reverse=True)
    lines = remove_list_lines(sorted(payload_files(payload)),
                              [p.relative_to(payload).as_posix() for p in dirs])
    remove.write_text("\n".join(lines) + "\n", encoding="utf-8-sig")
    artifact = artifact_path(output, release, scope)
    run(args.ucrt / "bin/makensis.exe",
        *pack_defines(scope, release, version_fields, payload, remove, artifact),
        HERE / "internal-test-installer.nsi", log=output / "nsis.log")
    (output / (artifact.name + ".sha256")).write_text(
        sha(artifact) + "  " + artifact.name + "\n", encoding="utf-8", newline="\n")
    (output / "installer-inputs.json").write_text(json.dumps({
        "channel": "internal-test", "scope": scope,
        "stage_manifest_sha256": sha(payload / MANIFEST),
        "nsis_script_sha256": sha(HERE / "internal-test-installer.nsi"),
        "makensis_sha256": sha(args.ucrt / "bin/makensis.exe"),
        "packager_sha256": sha(Path(__file__)),
        "installer_sha256": sha(artifact), "source_commit": record["source_commit"],
        "release_version": release}, indent=2), encoding="utf-8")
    print(f"UNSIGNED INTERNAL TEST INSTALLER ({scope}): {artifact}")


def build_parser():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    s = commands.add_parser("stage")
    for name in ("build", "source", "icc", "hyphen", "output"):
        s.add_argument("--" + name, type=Path, required=True)
    s.add_argument("--source-commit", required=True)
    s.add_argument("--gtk-run", type=Path, required=True,
                   help="validated patched GTK build run root; required for this internal-test channel")
    s.add_argument("--rehearsal", action="store_true", help="visibly label pre-freeze implementation tests")
    p = commands.add_parser("pack")
    p.add_argument("--payload", type=Path, required=True)
    p.add_argument("--output", type=Path, required=True)
    p.add_argument("--scope", choices=SCOPES, default="user",
                   help="installer scope; machine requests admin and installs under Program Files (x64)")
    for subparser in (s, p):
        subparser.add_argument("--ucrt", type=Path, default=Path("C:/msys64/ucrt64"))
    return parser


def main():
    args = build_parser().parse_args()
    (stage if args.command == "stage" else pack)(args)


if __name__ == "__main__":
    try:
        main()
    except (RuntimeError, OSError, KeyError) as exc:
        sys.exit(f"VACards internal test packaging failed: {exc}")
