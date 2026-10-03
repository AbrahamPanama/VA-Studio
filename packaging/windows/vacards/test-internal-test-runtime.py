#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Smoke checks against a staged or actually installed internal-test payload.

Run the harness with native Windows Python; every tested child uses the payload.
No developer PATH, Python module path, or
pre-existing Inkscape profile is used. Existing Sparrow acceptance is not rerun.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys


def expand_nsis(text, defines):
    """Expand flat !ifdef/!ifndef/!else/!endif scope guards for portable checks.

    A deterministic stand-in for the NSIS preprocessor, sufficient because the
    installer uses only non-nested scope conditionals.
    """
    kept, stack = [], []
    for line in text.splitlines():
        stripped = line.strip()
        tokens = stripped[1:].split(None, 1) if stripped.startswith("!") else []
        word = tokens[0] if tokens else ""
        rest = tokens[1].strip() if len(tokens) > 1 else ""
        if word in ("ifdef", "ifndef"):
            stack.append((rest in defines) if word == "ifdef" else (rest not in defines))
            continue
        if word == "else":
            assert stack, "!else without !ifdef"
            stack[-1] = not stack[-1]
            continue
        if word == "endif":
            assert stack, "!endif without !ifdef"
            stack.pop()
            continue
        if all(stack):
            kept.append(line)
    assert not stack, "unterminated !ifdef in installer"
    return "\n".join(kept)


def packaging_preflight():
    """Portable, narrow checks for common version, features and official icons."""
    import runpy
    here = Path(__file__).resolve().parent
    builder = runpy.run_path(str(here / "create-internal-test-installer.py"))
    version = builder["fields"](here.parents[1] / "vacards/VERSION.env")
    make_version = builder["test_version"]
    commit = "a" * 40
    release = (f"{version['product_version']}-build.{version['build_number']}" if version["format"] == "2"
               else f"{version['base_version']}-{version['edition']}.{version['build_number']}")
    expected = f"{release}-internal-test-{'a' * 10}"
    assert make_version(version, commit, False) == expected
    assert make_version(version, commit, True) == expected + "-rehearsal"
    for field, value in (("base_version", "1.5/evil"), ("build_number", "6$bad"),
                         ("edition", "other"), ("format", "3"),
                         ("product_name", "VA/Studio"), ("product_version", "1.0-beta.2"),
                         ("display_version", "1.0;bad")):
        try:
            make_version(dict(version, **{field: value}), commit, False)
        except RuntimeError:
            pass
        else:
            raise AssertionError(f"unsafe version accepted: {field}")
    deps = {"required_cmake_features": "WITH_LIBCDR,WITH_VACARDS_NESTING",
            "disabled_cmake_features": "WITH_IMAGE_MAGICK"}
    good = {"WITH_LIBCDR": "ON", "WITH_VACARDS_NESTING": "ON", "WITH_IMAGE_MAGICK": "OFF"}
    validate = builder["validate_features"]
    validate(deps, good)
    for name in good:
        for value in (None, "OFF" if good[name] == "ON" else "ON"):
            bad = dict(good)
            if value is None:
                del bad[name]
            else:
                bad[name] = value
            try:
                validate(deps, bad)
            except RuntimeError:
                pass
            else:
                raise AssertionError(f"wrong/missing feature accepted: {name}")
    # Pin the unchanged Mac artwork and its Windows format conversions together.
    # No imaging dependency is needed to detect a substituted/out-of-sync icon.
    source = here.parents[2]
    icons = {
        "packaging/macos/res/VACards-AppIcon.png": "36ae28a4daa4f073478a5045604f7c5a120071f3b6746eb9e71c8eb7ddb2659f",
        "share/branding/VACards-AppIcon.ico": "e2b62e2e4972335cbe0e145f659436a57732245ab44c72e2dc4fdf5aba22c202",
        "share/icons/com.vacards.Inkscape.png": "92f74ae2abc0e10c7e417f0508c1a9ede7bcdaa6384282a7c6735a682fb89ee0",
    }
    for name, expected in icons.items():
        assert digest(source / name) == expected, f"official icon changed: {name}"
    assert b'share/branding/VACards-AppIcon.ico' in (source / "src/inkscape.rc").read_bytes()
    app = (source / "src/inkscape-application.cpp").read_text()
    assert 'set_default_icon_name("com.vacards.Inkscape")' in app
    assert 'set_default_icon_name("org.inkscape.Inkscape")' not in app
    assert 'install(FILES com.vacards.Inkscape.png DESTINATION ${INKSCAPE_SHARE_INSTALL}/icons)' in (
        source / "share/icons/CMakeLists.txt").read_text()
    import tempfile
    with tempfile.TemporaryDirectory() as directory:
        prefix = Path(directory)
        record = {"source_commit": "a" * 40, "library_sha256": "b" * 64}
        log = prefix / "VACARDS-LIBCDR-TESTS.log"
        log.write_text("OK (4)\nPASS test.exe (exit status: 0)\n", encoding="utf-8")
        receipt = dict(record, format="1", tests="make-check-passed", test_log_sha256=digest(log))
        path = prefix / "VACARDS-LIBCDR-TESTS.env"
        def write_receipt(values):
            path.write_text("".join(f"{k}={v}\n" for k, v in values.items()), encoding="utf-8")
        verify = builder["libcdr_test_evidence"]
        write_receipt(receipt)
        assert verify(prefix, record) == [path, log]
        for key, value in (("source_commit", "c" * 40), ("library_sha256", "d" * 64),
                           ("test_log_sha256", "e" * 64), ("tests", "failed")):
            write_receipt(dict(receipt, **{key: value}))
            try:
                verify(prefix, record)
            except RuntimeError:
                pass
            else:
                raise AssertionError(f"libcdr test substitution accepted: {key}")
        log.write_text("OK (4)\nFAIL test.exe (exit status: 1)\n", encoding="utf-8")
        write_receipt(dict(receipt, test_log_sha256=digest(log)))
        try:
            verify(prefix, record)
        except RuntimeError:
            pass
        else:
            raise AssertionError("failing native log accepted")
    # Paired patched librevenge staging gate: the same release/basename as a
    # stock 0.0.6 DLL, so require the ON cache option, the resolved prefix,
    # prefix-bound bytes, and marker/capability equality with the tracked
    # bundle manifest. Portable: no Windows API is used by librevenge_inputs.
    import tempfile
    with tempfile.TemporaryDirectory() as directory:
        prefix = Path(directory) / "librevenge prefix"
        directory = prefix.parent
        (prefix / "lib").mkdir(parents=True)
        tracked_path = source / "packaging/dependencies/librevenge-0.0.6/VACARDS-LIBREVENGE-BUNDLE.env"
        tracked = builder["fields"](tracked_path)
        marker = tracked["capability_binary_marker"]
        library = prefix / "lib/librevenge-0.0-0.dll"
        manifest = prefix / "VACARDS-LIBREVENGE.env"
        keys = ("format", "dependency", "release", "source_identity", "archive_filename",
                "archive_sha256", "patch_filename", "patch_sha256", "capability_name",
                "capability_value", "capability_binary_marker", "library_relative_path",
                "library_sha256")
        def write_manifest(**overrides):
            values = {key: tracked[key] for key in keys if key in tracked}
            values.update(library_relative_path="lib/librevenge-0.0-0.dll",
                          library_sha256=digest(library))
            values.update(overrides)
            manifest.write_text("".join(f"{k}={values[k]}\n" for k in keys), encoding="utf-8")
        def write_library(has_marker=True):
            library.write_bytes(b"synthetic librevenge 0.0.6 " + (marker.encode() if has_marker else b"") +
                                b" payload")
        verify = builder["librevenge_inputs"]
        cache = {"VACARDS_REQUIRE_PATCHED_LIBREVENGE": "ON",
                 "VACARDS_RESOLVED_LIBREVENGE_PREFIX": str(prefix)}
        write_library()
        write_manifest()
        assert verify(cache, prefix, source)["library_sha256"] == digest(library)
        # Reject a missing or OFF requirement.
        for bad in (dict(cache, VACARDS_REQUIRE_PATCHED_LIBREVENGE="OFF"),
                    {k: v for k, v in cache.items() if k != "VACARDS_REQUIRE_PATCHED_LIBREVENGE"}):
            try:
                verify(bad, prefix, source)
            except RuntimeError:
                pass
            else:
                raise AssertionError("OFF/missing librevenge requirement accepted")
        # Reject a prefix that is not the resolved CMake prefix or is missing.
        other = directory / "other prefix"
        other.mkdir()
        for bad_prefix in (other, directory / "absent"):
            try:
                verify(cache, bad_prefix, source)
            except RuntimeError:
                pass
            else:
                raise AssertionError(f"foreign/missing librevenge prefix accepted: {bad_prefix}")
        # Reject stale bytes while the manifest still records the old hash.
        write_library()
        library.write_bytes(library.read_bytes() + b"drift")
        try:
            verify(cache, prefix, source)
        except RuntimeError:
            pass
        else:
            raise AssertionError("stale librevenge library hash accepted")
        # Reject a stock DLL whose manifest self-consistently has no marker.
        write_library(has_marker=False)
        write_manifest()
        try:
            verify(cache, prefix, source)
        except RuntimeError:
            pass
        else:
            raise AssertionError("markerless stock librevenge accepted")
        # Reject any capability/identity field that differs from the tracked
        # bundle manifest (the library itself is valid again here).
        write_library()
        for key, value in (("capability_name", "vacards:other"),
                           ("capability_value", "2"),
                           ("capability_binary_marker", "librevenge:other"),
                           ("archive_sha256", "0" * 64),
                           ("patch_sha256", "0" * 64)):
            write_manifest(**{key: value})
            try:
                verify(cache, prefix, source)
            except RuntimeError:
                pass
            else:
                raise AssertionError(f"librevenge {key} mismatch accepted")
        # Reject unsafe library paths and a missing manifest.
        write_manifest(library_relative_path="../escape.dll")
        try:
            verify(cache, prefix, source)
        except RuntimeError:
            pass
        else:
            raise AssertionError("unsafe librevenge path accepted")
        write_manifest()
        manifest.unlink()
        try:
            verify(cache, prefix, source)
        except RuntimeError:
            pass
        else:
            raise AssertionError("missing librevenge manifest accepted")
    # Patched GTK staging gate: same basename/version as stock 4.22.4, so require
    # the run manifest, toolchain record, ABI export list, byte inventory,
    # tracked patch and actual PE tables to agree with the tracked bundle, with
    # no new import over the pinned stock baseline. Portable: probe is injected.
    # GTK dependency closure: stock entries must match byte for byte; entries
    # staged from a pinned prefix (Cairo) are checked against that prefix instead.
    compare_deps = builder["compare_gtk_dependencies"]
    recorded = {"libcairo-gobject-2.dll": "a" * 64, "libpango-1.0-0.dll": "b" * 64}
    compare_deps(recorded, {"libcairo-gobject-2.dll": "c" * 64, "libpango-1.0-0.dll": "b" * 64},
                 pinned=["libcairo-gobject-2.dll"])
    for staged in ({"libcairo-gobject-2.dll": "c" * 64, "libpango-1.0-0.dll": "d" * 64},
                   {"libcairo-gobject-2.dll": "c" * 64}):
        try:
            compare_deps(recorded, staged, pinned=["libcairo-gobject-2.dll"])
        except RuntimeError:
            pass
        else:
            raise AssertionError("changed or missing stock GTK dependency accepted")
    try:
        compare_deps(recorded, {"libcairo-gobject-2.dll": "c" * 64, "libpango-1.0-0.dll": "b" * 64})
    except RuntimeError:
        pass
    else:
        raise AssertionError("unpinned changed GTK dependency accepted")
    tracked_bundle = source / "packaging/dependencies/gtk-4.22.4-win32-cairo-buffer/VACARDS-GTK-BUNDLE.env"
    tracked_gtk = builder["fields"](tracked_bundle)
    assert tracked_gtk["library_sha256"] == (
        "f812c202af97524d92ee5d4e72dc9497c5e41fc19fe80410d8e304d50fa829bd")
    assert tracked_gtk["patch_sha256"] == (
        "d11d7c22c2e063cb04cca171436befe7f9544702a2a92a23a7463c4f79f00272")
    assert tracked_gtk["exports_sha256"] == (
        "783e9b178e29deaf13eb9749974cf025c59381592d71b65199071a005e1ff71a")
    assert tracked_gtk["exports_count"] == "5275"
    assert tracked_gtk["baseline_stock_library_sha256"] == (
        "6438b88ec657c20b8549c9fbc80ea6bafe795c477db60ee20dade69fa63a63ca")
    # Focused parser oracle: feed the actual captured objdump text straight to
    # the same pe_export_names() that pe_tables() uses on the real host. This is
    # not the injected gtk_inputs probe, so it exercises real column extraction.
    parse_exports = builder["pe_export_names"]
    new_format = (
        "[Ordinal/Name Pointer] Table -- Ordinal Base 1\n"
        "\t          Ordinal   Hint Name\n"
        "\t[   0] +base[   1]  0000 _gdk_win32_display_hcursor_ref\n"
        "\t[   1] +base[   2]  0001 _gdk_win32_display_hcursor_unref\n"
        "\t[   2] +base[   3]  0002 gdk_anchor_hints_get_type\n"
        "\t[   3] +base[   4]  0003 gdk_app_launch_context_get_display\n"
        "\n")
    assert parse_exports(new_format) == [
        "_gdk_win32_display_hcursor_ref", "_gdk_win32_display_hcursor_unref",
        "gdk_anchor_hints_get_type",
        "gdk_app_launch_context_get_display"], "binutils 2.47 export column misparsed"
    old_format = (
        "[Ordinal/Name Pointer] Table\n"
        "[   0] _gdk_win32_display_hcursor_ref\n"
        "[  12] gdk_anchor_hints_get_type\n"
        "\n")
    assert parse_exports(old_format) == [
        "_gdk_win32_display_hcursor_ref", "gdk_anchor_hints_get_type"], \
        "older objdump export format misparsed"
    # A table with no name column must not fabricate an export token.
    assert parse_exports("DLL Name: KERNEL32.dll\n") == [], "absent table accepted"
    gtk_verify = builder["gtk_inputs"]
    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory)
        fake_source = root / "source"
        patch_dir = fake_source / "packaging/windows/vacards"
        patch_dir.mkdir(parents=True)
        patch = patch_dir / "gtk-4.22.4-win32-cairo-buffer.patch"
        patch.write_bytes(b"synthetic gtk implementation patch\n")
        bundle = root / "VACARDS-GTK-BUNDLE.env"
        run_root = root / "run"
        library = run_root / "install/bin/libgtk-4-1.dll"
        library.parent.mkdir(parents=True)
        library.write_bytes(b"synthetic patched gtk dll\n")
        extra_lib = run_root / "install/lib"
        (extra_lib / "pkgconfig").mkdir(parents=True)
        import_lib = extra_lib / "libgtk-4.dll.a"
        import_lib.write_bytes(b"synthetic import library\n")
        pc_file = extra_lib / "pkgconfig/gtk4.pc"
        pc_file.write_bytes(b"synthetic gtk pc\n")
        abi = run_root / "abi"
        abi.mkdir()
        exports = ["gdk_alpha", "gdk_beta", "gtk_gamma"]
        exports_path = abi / "libgtk-4-1.exports.txt"
        exports_path.write_bytes("".join(n + "\n" for n in sorted(exports)).encode("utf-8"))
        dependencies = abi / "gtk-ucrt64-dlls.sha256"
        dependencies.write_text(f"{'a' * 64}  libglib-2.0-0.dll\n", encoding="utf-8")
        inventory = run_root / "VACARDS-GTK.sha256"
        baseline = root / "ucrt/bin/libgtk-4-1.dll"
        baseline.parent.mkdir(parents=True)
        baseline.write_bytes(b"synthetic stock gtk dll\n")

        def write_inventory():
            inventory.write_text("\n".join(
                f"{digest(library) if name == 'bin/libgtk-4-1.dll' else digest(path)}  {name}"
                for name, path in (("bin/libgtk-4-1.dll", library),
                                   ("lib/libgtk-4.dll.a", import_lib),
                                   ("lib/pkgconfig/gtk4.pc", pc_file),
                                   ("abi/libgtk-4-1.exports.txt", exports_path))) + "\n",
                encoding="utf-8")

        def bundle_values():
            return {
                "format": "1", "dependency": "gtk4", "gtk_version": "4.22.4",
                "platform": "windows-ucrt64", "architecture": "x86_64",
                "source_archive_sha256": "1" * 64,
                "msys2_commit": "d07b8dabb92443fd1daed511ad7b406825964b0d",
                "msys2_pkgbuild_sha256": "2" * 64, "p001_sha256": "3" * 64,
                "p003_sha256": "4" * 64, "patch_filename": patch.name,
                "patch_sha256": digest(patch), "library_relative_path": "bin/libgtk-4-1.dll",
                "library_sha256": digest(library), "exports_count": str(len(exports)),
                "exports_sha256": digest(exports_path),
                "baseline_stock_library_sha256": digest(baseline),
            }

        def write_bundle(**overrides):
            values = dict(bundle_values(), **overrides)
            bundle.write_text("".join(f"{k}={v}\n" for k, v in values.items()), encoding="utf-8")

        def write_env(**overrides):
            values = {"impl_patch_sha256": digest(patch),
                      "libgtk_4_1_dll_sha256": digest(library),
                      "exports_sha256": digest(exports_path),
                      "exports_count": str(len(exports)),
                      "inventory_sha256": digest(inventory),
                      "ucrt64_dll_manifest_sha256": digest(dependencies), "configure_only": "0"}
            values.update(overrides)
            (run_root / "VACARDS-GTK.env").write_text(
                "".join(f"{k}={v}\n" for k, v in values.items()), encoding="utf-8")

        def write_toolchain(**overrides):
            values = {"format": "2", "platform": "windows-ucrt64", "architecture": "x86_64",
                      "gtk_version": "4.22.4", "gtk_tarball_sha256": "1" * 64,
                      "msys2_commit": "d07b8dabb92443fd1daed511ad7b406825964b0d",
                      "msys2_pkgbuild_sha256": "2" * 64, "p001_sha256": "3" * 64,
                      "p003_sha256": "4" * 64, "impl_patch_sha256": digest(patch),
                      "configure_only": "0"}
            values.update(overrides)
            (run_root / "toolchain.txt").write_text(
                "".join(f"{k}={v}\n" for k, v in values.items()), encoding="utf-8")

        def probe(path):
            return ["KERNEL32.dll", "USER32.dll"], list(exports)

        def check(probe_fn=probe):
            return gtk_verify(run_root, fake_source, probe=probe_fn, bundle=bundle,
                              baseline_dll=baseline)

        def rejects(label, probe_fn=probe):
            try:
                check(probe_fn)
            except RuntimeError:
                pass
            else:
                raise AssertionError(f"GTK substitution accepted: {label}")

        write_inventory()
        write_env()
        write_toolchain()
        write_bundle()
        result = check()
        assert result["library_sha256"] == digest(library)
        assert result["exports_count"] == str(len(exports))
        assert result["baseline_stock_library_sha256"] == digest(baseline)
        # Valid-but-different GTK bytes must not enter as the tested library.
        library.write_bytes(b"synthetic patched gtk dll\n" + b"drift")
        rejects("changed library bytes")
        library.write_bytes(b"synthetic patched gtk dll\n")
        # Wrong implementation patch, both sides of the binding.
        write_env(impl_patch_sha256="0" * 64)
        rejects("run manifest patch sha")
        write_env()
        patch.write_bytes(b"different tracked patch\n")
        rejects("tracked patch sha")
        patch.write_bytes(b"synthetic gtk implementation patch\n")
        # Missing run records fail closed.
        (run_root / "VACARDS-GTK.env").unlink()
        rejects("missing run manifest")
        write_env()
        (run_root / "toolchain.txt").unlink()
        rejects("missing toolchain record")
        write_toolchain()
        # ABI list must byte-match the DLL's actual export table.
        rejects("abi export mismatch",
                lambda path: (["KERNEL32.dll"], exports + ["gtk_delta"]))
        # No new runtime dependency beyond the pinned stock baseline.
        rejects("unexpected import",
                lambda path: (["KERNEL32.dll"] + (["newdep-1.dll"] if Path(path) == library else []),
                              exports))
        rejects("baseline export set differs",
                lambda path: (["KERNEL32.dll"], exports if Path(path) == library else exports[:2]))
        # Unsafe/missing fields and provenance cross-checks fail closed.
        write_bundle(library_relative_path="../escape.dll")
        rejects("unsafe library path")
        write_bundle()
        write_bundle(library_sha256="not-hex")
        rejects("invalid bundle hash")
        write_bundle()
        values = bundle_values()
        del values["exports_sha256"]
        bundle.write_text("".join(f"{k}={v}\n" for k, v in values.items()), encoding="utf-8")
        rejects("missing bundle field")
        write_bundle()
        bundle.unlink()
        rejects("missing bundle manifest")
        write_bundle()
        write_toolchain(gtk_tarball_sha256="9" * 64)
        rejects("source archive cross-check")
        write_toolchain()
        write_toolchain(configure_only="1")
        rejects("configure-only run")
        write_toolchain()
        write_env(exports_count="2")
        rejects("wrong exports count")
        write_env()
        write_env(exports_sha256="0" * 64)
        rejects("wrong exports sha")
        write_env()
        baseline.write_bytes(b"different stock baseline\n")
        rejects("stock baseline hash")
        baseline.write_bytes(b"synthetic stock gtk dll\n")
        (run_root / "VACARDS-GTK.env").write_text("a=1\na=2\n", encoding="utf-8")
        rejects("duplicate field")
        write_env()
        # Byte inventory catches a swapped support artifact and unsafe paths.
        import_lib.write_bytes(b"drifted import library\n")
        rejects("inventory drift")
        import_lib.write_bytes(b"synthetic import library\n")
        inventory.write_text("0" * 64 + "  ../escape.dll\n", encoding="utf-8")
        write_env(inventory_sha256=digest(inventory))
        rejects("unsafe inventory path")
        write_inventory()
        write_env()
        assert check()["library_sha256"] == digest(library)
    # Installer scope wiring: `pack --scope user` stays the default, and
    # `--scope machine` selects the admin/HKLM/Program Files branch of the one
    # tracked NSI. Expanded text replaces a real makensis or installation run.
    nsi = (here / "internal-test-installer.nsi").read_text(encoding="utf-8")
    user, machine = expand_nsis(nsi, set()), expand_nsis(nsi, {"SCOPE_MACHINE"})
    def code(text):
        return "\n".join(line for line in text.splitlines() if not line.strip().startswith(";"))
    uc, mc = code(user), code(machine)
    for text in (nsi, user, machine):
        assert "RMDir /r" not in code(text), "recursive install-root deletion must not appear"
        assert "INSTALL_ROOT" not in code(text), "fixed install root must not appear"
    assert "RequestExecutionLevel user" in uc and "RequestExecutionLevel admin" not in uc
    assert "HKCU" in uc and "HKLM" not in uc and "SetRegView 64" not in uc
    assert 'InstallDir "$LOCALAPPDATA\\Programs\\VACards Test\\${TEST_VERSION}"' in uc
    assert '"$SMPROGRAMS\\VACards Test\\${PRODUCT} ${DISPLAY_VERSION} ${TEST_VERSION}.lnk"' in uc
    assert '"$DESKTOP' not in uc
    assert "RequestExecutionLevel admin" in mc and "RequestExecutionLevel user" not in mc
    assert "SetRegView 64" in mc and "SetShellVarContext all" in mc
    assert "HKLM" in mc and "HKCU" not in mc
    assert 'InstallDir "$PROGRAMFILES64\\${PRODUCT} ${DISPLAY_VERSION}"' in mc
    assert '"$PROGRAMFILES64\\"' in mc
    assert '"$DESKTOP\\${PRODUCT} ${DISPLAY_VERSION}.lnk"' in mc
    assert 'CreateDirectory "$SMPROGRAMS\\${PRODUCT} ${DISPLAY_VERSION}"' in mc
    # Machine ownership guards: expanded machine branch must refuse a
    # pre-existing display-version desktop shortcut, Start Menu folder or Start
    # Menu link BEFORE any payload write, and require the normalized
    # destination's parent to be Program Files (x64) itself.
    for shortcut in ('"$DESKTOP\\${PRODUCT} ${DISPLAY_VERSION}.lnk"',
                     '"$SMPROGRAMS\\${PRODUCT} ${DISPLAY_VERSION}"',
                     '"$SMPROGRAMS\\${PRODUCT} ${DISPLAY_VERSION}\\${PRODUCT} ${DISPLAY_VERSION}.lnk"'):
        assert f"GetFileAttributesW(w {shortcut})" in mc, shortcut
    assert '${GetParent} "$INSTDIR" $1' in mc
    assert '${If} $1 != "$PROGRAMFILES64"' in mc
    assert "Call CheckMachineShortcuts" in mc
    assert mc.index("Call CheckMachineShortcuts") < mc.index("SetOutPath")
    assert mc.index("Call CheckMachineShortcuts") < mc.index('File /r "${PAYLOAD}\\*"')
    assert "directly inside Program Files" in mc
    assert "existing shortcuts are never overwritten" in mc
    assert "machine_shortcut_conflict" not in uc
    assert "${GetParent}" not in uc
    for text in (uc, mc):
        assert "GetFullPathNameW" in text and "GetFileAttributesW" in text
        assert "VACards.Inkscape.InternalTest.${TEST_VERSION}" in text
        assert '!include "${REMOVE_LIST}"' in text and "REMOVE_RETRY_ATTEMPTS" in text
        assert 'RMDir "$INSTDIR"' in text and '"$INSTDIR\\VACards-Test-Install.ini"' in text
    pack_defines, artifact_path = builder["pack_defines"], builder["artifact_path"]
    paths = (Path("C:/payload"), Path("C:/remove.nsh"), Path("C:/artifact.exe"))
    assert "/DSCOPE_MACHINE" not in pack_defines("user", release, version, *paths)
    assert "/DSCOPE_MACHINE" in pack_defines("machine", release, version, *paths)
    assert "programfiles" not in artifact_path(Path("C:/out"), release, "user").name
    assert "programfiles" in artifact_path(Path("C:/out"), release, "machine").name
    for bad in ("ordinary", ""):
        for call in (lambda: pack_defines(bad, release, version, *paths),
                     lambda: artifact_path(Path("C:/out"), release, bad)):
            try:
                call()
            except RuntimeError:
                pass
            else:
                raise AssertionError(f"unknown installer scope accepted: {bad!r}")
    parser, base = builder["build_parser"](), ["pack", "--payload", "p", "--output", "o"]
    assert parser.parse_args(base).scope == "user"
    assert parser.parse_args(base + ["--scope", "machine"]).scope == "machine"
    import contextlib, io
    with contextlib.redirect_stderr(io.StringIO()):
        try:
            parser.parse_args(base + ["--scope", "ordinary"])
        except SystemExit:
            pass
        else:
            raise AssertionError("unknown --scope accepted by pack")
    print("PASS common version, required/disabled features, official icons, patched GTK staging gate and installer scope wiring")


def digest(path):
    with Path(path).open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--payload", type=Path, required=True)
    parser.add_argument("--evidence", type=Path, required=True)
    args = parser.parse_args()
    root, evidence = args.payload.resolve(), args.evidence.resolve()
    evidence.mkdir()
    record = json.loads((root / "share/vacards-test/WINDOWS-INTERNAL-TEST.json").read_text())
    assert record["channel"] == "internal-test" and not record["production_qualified"]
    for name, expected in record["payload_sha256"].items():
        assert digest(root / name) == expected, f"Changed payload: {name}"
    for dll, expected in record["cairo_sha256"].items():
        assert digest(root / "bin" / dll) == expected
    assert digest(root / "bin/libcdr-0.1.dll") == record["libcdr_sha256"]
    librevenge = root / record["librevenge_relative_path"]
    assert digest(librevenge) == record["librevenge_sha256"]
    assert digest(root / "share/vacards-test/VACARDS-LIBREVENGE.env") == record["librevenge_manifest_sha256"]
    assert record["librevenge_capability"]["binary_marker"].encode() in librevenge.read_bytes()
    # Every internal-test stage must carry the patched GTK pin.
    if not record.get("gtk_sha256"):
        raise RuntimeError("internal-test manifest has no gtk_sha256")
    if record["gtk_sha256"]:
        gtk_relative = record["gtk_relative_path"]
        assert not Path(gtk_relative).is_absolute() and ".." not in Path(gtk_relative).parts
        assert digest(root / gtk_relative) == record["gtk_sha256"], "GTK runtime changed"
        provenance = root / "share/vacards-test"
        assert digest(provenance / "VACARDS-GTK.env") == record["gtk_env_sha256"]
        assert digest(provenance / "VACARDS-GTK.sha256") == record["gtk_inventory_sha256"]
        assert digest(provenance / "VACARDS-GTK.toolchain.txt") == record["gtk_toolchain_sha256"]
        assert digest(provenance / "VACARDS-GTK.exports.txt") == record["gtk_exports_sha256"]
        declared = dict(line.split("=", 1) for line in
                        (provenance / "VACARDS-GTK.env").read_text(encoding="utf-8-sig").splitlines()
                        if "=" in line)
        assert declared["libgtk_4_1_dll_sha256"] == record["gtk_sha256"]
        assert declared["impl_patch_sha256"] == record["gtk_patch_sha256"]
        assert declared["exports_sha256"] == record["gtk_exports_sha256"]
        assert int(declared["exports_count"]) == record["gtk_exports_count"]
    assert digest(root / "bin/vacards-sparrow.exe") == record["sparrow_sha256"]
    assert digest(root / "share/vacards-test/VACards-AppIcon.ico") == record["official_icon_sha256"]
    assert digest(root / "share/inkscape/icons/com.vacards.Inkscape.png") == record["gtk_app_icon_sha256"]
    env = {k: v for k, v in os.environ.items() if k.upper() in (
        "SYSTEMROOT", "SYSTEMDRIVE", "PROGRAMDATA", "WINDIR", "USERPROFILE", "APPDATA", "LOCALAPPDATA", "TEMP", "TMP",
        "COMSPEC", "PROCESSOR_ARCHITECTURE", "NUMBER_OF_PROCESSORS")}
    env["PATH"] = str(Path(env["SYSTEMROOT"]) / "System32") + ";" + env["SYSTEMROOT"]
    env["VACARDS_TEST_PROFILE_DIR"] = str(evidence / "profile")
    env["PYTHONDONTWRITEBYTECODE"] = "1"
    results = {}

    def run(label, *command, runtime_env=None, timeout=90):
        p = subprocess.run([str(c) for c in command], env=runtime_env or env, cwd=evidence,
                           capture_output=True, encoding="utf-8", errors="replace", timeout=timeout)
        (evidence / (label + ".log")).write_text(p.stdout + "\n" + p.stderr, encoding="utf-8")
        assert p.returncode == 0, (label, p.returncode, p.stderr[-1500:])
        assert "Fontconfig error:" not in p.stderr, (label, p.stderr)
        assert "g_module_open() failed" not in p.stderr, (label, p.stderr)
        results[label] = {"exit_code": p.returncode}
        print("PASS", label, flush=True)
        return p.stdout

    version = run("version", root / "VACards-Test.exe", "--version")
    assert version.strip(), "Launcher did not forward CLI output"
    assert record["source_commit"][:10] in version or record["rehearsal"], version
    data = run("data-directory", root / "VACards-Test.exe", "--system-data-directory")
    # The environment is the share prefix; this CLI reports its inkscape child.
    assert Path(data.strip()).resolve() == root / "share/inkscape"
    profile = run("profile-directory", root / "VACards-Test.exe", "--user-data-directory")
    assert Path(profile.strip()).resolve() == evidence / "profile"
    # A legacy inherited profile must not contaminate first launch. Then a
    # real preference save/reload must keep changes in the new profile.
    import xml.etree.ElementTree as ET
    legacy = evidence / "legacy-profile"
    legacy.mkdir()
    legacy_xml = b'<inkscape version="1"><group id="legacy-profile-sentinel" value="do-not-import"/></inkscape>'
    (legacy / "preferences.xml").write_bytes(legacy_xml)
    inherited = dict(env, INKSCAPE_PROFILE_DIR=str(legacy))
    run("first-profile-save", root / "VACards-Test.exe", "--actions=save-preferences;quit", runtime_env=inherited)
    preferences = evidence / "profile/preferences.xml"
    document = ET.parse(preferences)
    assert document.find(".//group[@id='legacy-profile-sentinel']") is None
    assert (legacy / "preferences.xml").read_bytes() == legacy_xml
    ET.SubElement(document.getroot(), "group", id="va-studio-persistence-test", value="kept")
    document.write(preferences, encoding="utf-8", xml_declaration=True)
    run("second-profile-save", root / "VACards-Test.exe", "--actions=save-preferences;quit", runtime_env=inherited)
    retained = ET.parse(preferences).find(".//group[@id='va-studio-persistence-test']")
    assert retained is not None and retained.get("value") == "kept", "Profile was reset on relaunch"
    assert (legacy / "preferences.xml").read_bytes() == legacy_xml
    run("sparrow", root / "bin/vacards-sparrow.exe", "--help")
    schema_env = dict(env, GSETTINGS_SCHEMA_DIR=str(root / "share/glib-2.0/schemas"))
    schemas = run("schemas", root / "bin/gsettings.exe", "list-schemas", runtime_env=schema_env)
    assert "org.gtk.gtk4.Settings.FileChooser" in schemas
    svg = evidence / "Prueba á 文.svg"
    svg.write_text('<svg xmlns="http://www.w3.org/2000/svg" width="1in" height="1in" '
                   'viewBox="0 0 96 96"><rect width="96" height="96" '
                   'fill="#cc9933" fill-opacity="0.5"/>'
                   '<text x="4" y="92" font-size="8">VACards Test</text></svg>', encoding="utf-8")
    for extension in ("svg", "png", "pdf", "tiff"):
        output = evidence / ("saved." + extension)
        run("export-" + extension, root / "VACards-Test.exe", svg,
            "--export-dpi=300", "--export-filename=" + str(output))
        assert output.stat().st_size > 0
    # Native TIFF works while neither normal bundled Python executable is available.
    # Only files in this dedicated test payload are temporarily renamed; restore always.
    moved = []
    try:
        for name in ("python.exe", "pythonw.exe"):
            original = root / "bin" / name
            hold = original.with_suffix(".qa-held")
            assert not hold.exists()
            original.rename(hold)
            moved.append((hold, original))
        run("native-tiff-without-python", root / "VACards-Test.exe", svg,
            "--export-dpi=300", "--export-filename=" + str(evidence / "native.tiff"))
    finally:
        for hold, original in reversed(moved):
            hold.rename(original)
    pyenv = dict(env, PYTHONHOME=str(root), PYTHONNOUSERSITE="1",
                 PYTHONPATH=str(root / "share/inkscape/extensions"),
                 PATH=str(root / "bin") + ";" + env["PATH"])
    check = (
        "import sys,json,hashlib,importlib.metadata as md,inkex,lxml,numpy,cssselect,tinycss2,scour,serial,packaging,pyparsing; "
        "from PIL import Image; from pathlib import Path; "
        "im=Image.open(sys.argv[1]); assert im.mode=='RGBA',im.mode; "
        "assert im.size==(300,300),im.size; assert abs(im.info['dpi'][0]-300)<0.1; "
        "assert hashlib.sha256(im.info['icc_profile']).hexdigest()==sys.argv[2]; "
        "assert abs(im.getpixel((150,150))[3]-128)<=1; "
        "root=Path(sys.argv[3]).resolve(); "
        "modules=[inkex,lxml,numpy,Image,cssselect,tinycss2,scour,serial,packaging,pyparsing]; "
        "assert all(Path(m.__file__).resolve().is_relative_to(root) for m in modules); "
        "distributions=['lxml','numpy','Pillow','cssselect','tinycss2','scour','pyserial','packaging','pyparsing']; "
        "assert all(md.distribution(d).files for d in distributions); "
        "print(json.dumps({'python':sys.executable,'modules':{m.__name__:m.__file__ for m in modules},"
        "'tiff':{'mode':im.mode,'size':im.size,'dpi':list(map(float,im.info['dpi']))}}))")
    run("python-modules-and-tiff", root / "bin/python.exe", "-B", "-c", check,
        evidence / "native.tiff", record["icc_sha256"], root, runtime_env=pyenv)
    # Pillow uses the bundled libtiff to independently decode the native app's export.
    for name, expected in record["payload_sha256"].items():
        assert digest(root / name) == expected, f"Payload mutated by smoke test: {name}"
    actual_files = {p.relative_to(root).as_posix() for p in root.rglob("*") if p.is_file()}
    expected_files = set(record["payload_sha256"]) | {"share/vacards-test/WINDOWS-INTERNAL-TEST.json"}
    # Actual installations add exactly these two owned installer files.
    expected_files |= {p for p in ("Uninstall-VACards-Test.exe", "VACards-Test-Install.ini") if (root / p).is_file()}
    assert actual_files == expected_files, ("unexpected runtime writes", sorted(actual_files - expected_files))
    (evidence / "results.json").write_text(json.dumps(results, indent=2), encoding="utf-8")
    (evidence / "PASS.txt").write_text("Internal-test payload smoke passed. Not production qualification.\n")


if __name__ == "__main__":
    if sys.argv[1:] == ["--packaging-preflight"]:
        packaging_preflight()
    else:
        main()
