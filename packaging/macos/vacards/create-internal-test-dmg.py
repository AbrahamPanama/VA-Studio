#!/usr/bin/env python3
"""Owner-approved INTERNAL TEST channel. Never issues production attestations.

Reuse the existing dependency capture/resolution checks, but keep this entry
point and its clearly named artifacts separate from production packaging.
"""
import argparse
import importlib.metadata
import importlib.util
import json
import os
from pathlib import Path
import plistlib
import re
import shutil
import subprocess
import sys
import sysconfig
import tempfile
from types import SimpleNamespace

SOURCE = Path(__file__).resolve().parents[3]
spec = importlib.util.spec_from_file_location(
    "provenance", SOURCE / "packaging/vacards/packaging-provenance.py")
pp = importlib.util.module_from_spec(spec)
spec.loader.exec_module(pp)
NAME = "VA Studio"
# Public bundle data (ICC profile, public package manifest) is readable by the
# installed account, not only by the packaging builder.
PUBLIC_MODE = 0o644
PUBLIC_BUNDLE_MANIFEST = "VACARDS-INTERNAL-TEST.json"


def cairo_family(path):
    # cairomm is the separate C++ wrapper; it is not built by our Cairo prefix.
    return re.match(r"^libcairo[.-]", Path(path).name) is not None


def librevenge_core_target(prefix, manifest):
    """Verified-prefix path, basename and manifest digest for the paired core.

    Only the manifest's exact ``library_relative_path`` is the canonical core;
    the auxiliary librevenge-stream/generators siblings are deliberately out of
    scope so their own names keep the global collision guard.
    """
    relative = manifest.get("library_relative_path", "")
    pp.require(relative and not Path(relative).is_absolute() and ".." not in Path(relative).parts,
               "unsafe librevenge core library_relative_path")
    target = (Path(prefix).resolve() / relative).resolve()
    return target, Path(relative).name, manifest.get("library_sha256", "")


def attested_librevenge_core(candidate_records, observed, prefix, manifest):
    """Require one runtime-observed, verified-prefix, manifest-matching core.

    Mirrors the Cairo-family canonicalization: a duplicate copy may be dropped
    only when exactly one attested core can replace it. Missing observation, a
    wrong prefix, digest drift, or two candidates must reject rather than let a
    silent same-basename winner through.
    """
    target, core_name, core_sha = librevenge_core_target(prefix, manifest)
    prefix = Path(prefix).resolve()
    matches = [record for record in candidate_records
               if str(Path(record["file"]["realpath"]).resolve()) == str(target)
               and str(target) in observed
               and Path(record["file"]["realpath"]).resolve().is_relative_to(prefix)
               and core_name in record.get("aliases", [])
               and pp.digest(record["file"]["realpath"]) == core_sha]
    pp.require(len(matches) == 1,
               f"no unique attested observed librevenge core for {core_name}")
    return target, core_name, core_sha


def librevenge_core_duplicate(real, candidate_records, observed, prefix, manifest):
    """True when ``real`` is a non-attested duplicate of the paired core.

    The caller then drops it, exactly as the Cairo branch does; existing alias
    rewriting maps the referring load command onto the attested bundled copy.
    """
    target, core_name, _ = librevenge_core_target(prefix, manifest)
    if Path(real).name != core_name or str(Path(real).resolve()) == str(target):
        return False
    attested_librevenge_core(candidate_records, observed, prefix, manifest)
    return True


def bundle_aliases(records, images):
    """Map every alternate Mach-O name to its unique bundled source realpath."""
    aliases = {}
    for real, (original, _) in images.items():
        names = set(records.get(real, {}).get("aliases", [])) | {original.name, Path(real).name}
        for name in names:
            aliases.setdefault(name, set()).add(real)
    return aliases


def resolve_bundled_candidate(link, aliases, exact=None):
    """Narrow a dependency load command to its unique bundled source realpath.

    ``exact`` is the caller-resolved absolute/@loader_path/@rpath spelling. A
    unique captured alias remains the fallback when the exact path is not an
    observed bundled image.
    """
    candidates = aliases.get(Path(link).name, set())
    if exact in candidates:
        candidates = {exact}
    return candidates


def loader_destination(resources, record):
    """Canonical packaged path for one tested GdkPixbuf module.

    gdk-pixbuf and the launch-time query tool address the *registered* module
    path (``libpixbufloader_svg.so``). Homebrew's librsvg SVG module resolves to
    ``libpixbufloader_svg.dylib`` behind that ``.so`` symlink, so naming the copy
    after the real image silently drops SVG from the runtime cache. Match the
    captured aliases against the required names instead of the realpath.
    """
    matches = [name for name in pp.REQUIRED_LOADERS if name in record["aliases"]]
    pp.require(len(matches) == 1,
               f"no unique registered GdkPixbuf module name for {record['file']['realpath']}")
    return resources / pp.loader_relative(matches[0])


def run(*args, **kwargs):
    print("+", " ".join(map(str, args)), flush=True)
    return subprocess.run(list(map(str, args)), check=True, **kwargs)


def output(*args, env=None):
    return subprocess.check_output(list(map(str, args)), text=True, env=env).strip()


def copy(source, target, mode=None):
    source, target = Path(source), Path(target)
    target.parent.mkdir(parents=True, exist_ok=True)
    # copy2 also carries protected com.apple.provenance metadata from Homebrew.
    # Copy payload bytes and executable mode, not the source installation's xattrs.
    shutil.copyfile(source, target)
    shutil.copymode(source, target)
    if mode is None:
        # Homebrew libraries are often 0444. Only our fresh output must be
        # writable for relocation, attribute cleanup and signing; never chmod
        # source inputs.
        target.chmod(target.stat().st_mode | 0o200)
    else:
        # Public bundle data must be readable by the installed (non-owner)
        # account independent of the source mode or the packaging umask.
        target.chmod(mode)
    return str(target)


MAIN_EXECUTABLE = "Contents/MacOS/inkscape-bin"


def sign_internal_app_entry(app):
    """Ad-hoc sign the nested entries, then seal the outer bundle.

    CFBundleExecutable is the native ``inkscape-bin``. ``Contents/MacOS/inkscape``
    and ``Contents/MacOS/python3`` stay as nested shell scripts, and
    ``Contents/MacOS/python3-bin``, ``Contents/MacOS/gdk-pixbuf-query-loaders``
    and ``Contents/MacOS/vastudio-cli``
    are nested native executables whose ``ldid`` signatures are not accepted by
    ``codesign --verify --deep --strict``, so all five must be signed explicitly
    before the outer bundle is sealed (``vacards-sparrow`` keeps its pinned
    signature; ``inkscape-bin`` is sealed with the bundle). Signing the outer bundle
    without ``--deep`` then preserves the already ``ldid``-signed libraries and
    the pinned helper. Any ``run`` failure propagates immediately, so a failed
    step can never be followed by a bundle seal around an unsigned entry.
    """
    app = Path(app)
    macos = app / "Contents/MacOS"
    run("codesign", "--force", "--sign", "-", macos / "inkscape")
    run("codesign", "--force", "--sign", "-", macos / "python3")
    run("codesign", "--force", "--sign", "-", macos / "python3-bin")
    run("codesign", "--force", "--sign", "-", macos / "gdk-pixbuf-query-loaders")
    run("codesign", "--force", "--sign", "-", macos / "vastudio-cli")
    run("codesign", "--force", "--sign", "-", app)


def clean_env(profile):
    return {"HOME": os.environ["HOME"], "PATH": "/usr/bin:/bin:/usr/sbin:/sbin",
            "LANG": "en_US.UTF-8", "LC_ALL": "en_US.UTF-8",
            "TMPDIR": "/tmp", "INKSCAPE_PROFILE_DIR": str(profile)}


def verify_bundled_data_permissions(app, public_files=None):
    """Reject owner-only public data in a built bundle before app smoke.

    Reads POSIX mode bits only. os.access(path, R_OK) is deliberately not used:
    the packaging builder owns these files, so owner-only modes still pass
    os.access and previously hid the defect from builder-owner test runs. The
    verifier rejects bad permissions; it never chmods an already-built app.
    """
    app = Path(app)
    pp.require(app.is_dir(), f"app bundle is missing: {app}")
    if public_files is None:
        deps = pp.read_env(SOURCE / "VACARDS-DEPENDENCIES.env")
        resources = app / "Contents/Resources"
        public_files = (resources / "share/inkscape/color/icc" / deps["tiff_rgb_profile_filename"],
                        resources / PUBLIC_BUNDLE_MANIFEST)
    for path in (Path(path) for path in public_files):
        pp.require(path.is_file(), f"required public bundled data is missing: {path}")
        mode = path.stat().st_mode & 0o777
        pp.require(mode & 0o044 == 0o044 and not mode & 0o022,
                   f"public bundled data is owner-only or not publicly readable: {path} mode {mode:04o}")
        # Public resources only: walk ancestors up to and including the bundle
        # root, never the mount/user directories that contain the bundle.
        current = path.parent
        while current == app or app in current.parents:
            directory_mode = current.stat().st_mode & 0o777
            pp.require(directory_mode & 0o055 == 0o055 and not directory_mode & 0o022,
                       f"public bundle directory is not readable/traversable by non-owner: "
                       f"{current} mode {directory_mode:04o}")
            if current == app:
                break
            current = current.parent


def verify(app, evidence, sha):
    """Exercise the delivered app with no developer PATH or DYLD overrides."""
    evidence.mkdir()
    resources = app / "Contents/Resources"
    macos = app / "Contents/MacOS"
    verify_bundled_data_permissions(app)
    env = clean_env(evidence / "profile")
    run("codesign", "--verify", "--deep", "--strict", app)
    version = output(macos / "inkscape", "--version", env=env)
    pp.require(sha[:10] in version, f"stale packaged binary: {version}")
    (evidence / "version.txt").write_text(version + "\n")
    # The launcher regenerates the loaders cache before exec; a TIFF-less cache
    # silently drops File > Open's .tif/.tiff input extension (BUG-003).
    loader_cache = evidence / "profile/gdk-pixbuf-loaders.cache"
    pp.require(loader_cache.is_file(), "launcher did not generate a GdkPixbuf cache")
    cache_text = loader_cache.read_text()
    pp.require("libpixbufloader_svg.so" in cache_text, "packaged SVG loader missing from runtime cache")
    pp.require("libpixbufloader-tiff.so" in cache_text and "image/tiff" in cache_text,
               "packaged TIFF loader missing from runtime cache")
    # The bundle's actual entry point is the native inkscape-bin, which must
    # bootstrap the same loader cache itself (no shell). Run it with a pristine
    # profile and require it to regenerate the SVG+TIFF cache.
    native_profile = evidence / "native-profile"
    native_version = output(macos / "inkscape-bin", "--version", env=clean_env(native_profile))
    pp.require(sha[:10] in native_version, f"stale native packaged binary: {native_version}")
    native_cache = native_profile / "gdk-pixbuf-loaders.cache"
    pp.require(native_cache.is_file(), "native entry did not generate a GdkPixbuf cache")
    native_text = native_cache.read_text()
    pp.require("libpixbufloader_svg.so" in native_text and "libpixbufloader-tiff.so" in native_text
               and "image/tiff" in native_text,
               "native entry cache is missing the bundled SVG/TIFF loaders")
    manifest = pp.read_json(resources / PUBLIC_BUNDLE_MANIFEST)
    pp.require(manifest["channel"] == "internal-test" and manifest["source_commit"] == sha,
               "wrong package channel/source")
    pp.require(manifest["payload_sealed_by_codesign"] == [MAIN_EXECUTABLE],
               "unexpected files excluded from payload digests")
    for relative, digest in manifest["payload_sha256"].items():
        pp.require(pp.digest(app / relative) == digest, f"package bytes changed: {relative}")
    # Plain SVG Save As, PNG, PDF and RGB/ICC TIFF exercise real export paths.
    svg = evidence / "export.svg"
    svg.write_text('<svg xmlns="http://www.w3.org/2000/svg" width="1in" height="1in" '
                   'viewBox="0 0 96 96"><rect width="96" height="96" '
                   'fill="#cc9933" fill-opacity="0.5"/>'
                   '<text x="8" y="48" font-family="Arial" font-size="12">VACards</text></svg>')
    for suffix in ("svg", "png", "pdf", "tiff"):
        destination = evidence / ("saved." + suffix)
        run(macos / "inkscape", svg, "--export-dpi=300",
            "--export-filename=" + str(destination), env=env)
        pp.require(destination.stat().st_size > 0, f"empty {suffix} output")
    metadata = output("/usr/bin/sips", "-g", "format", "-g", "pixelWidth", "-g",
                      "pixelHeight", "-g", "dpiWidth", "-g", "space", "-g", "hasAlpha",
                      "-g", "profile", evidence / "saved.tiff")
    (evidence / "tiff-metadata.txt").write_text(metadata + "\n")
    for expected in ("format: tiff", "pixelWidth: 300", "pixelHeight: 300",
                     "dpiWidth: 300.000", "space: RGB", "hasAlpha: yes"):
        pp.require(expected in metadata, f"incorrect TIFF metadata: {expected}")
    run(macos / "python3", "-c",
        "import inkex, numpy, serial, scour, tinycss2; from PIL import Image; "
        "from pathlib import Path; import hashlib; "
        "im=Image.open(__import__('sys').argv[1]); "
        "assert hashlib.sha256(im.info['icc_profile']).hexdigest()==__import__('sys').argv[2]; im.close()",
        evidence / "saved.tiff", manifest["icc_sha256"], env=env)
    run(macos / "python3", "-c",
        "import ctypes as c,sys; from pathlib import Path; r=Path(sys.argv[1]); "
        "g=c.CDLL(str(r/'lib/libgio-2.0.0.dylib')); "
        "g.g_settings_schema_source_new_from_directory.argtypes=[c.c_char_p,c.c_void_p,c.c_int,c.c_void_p]; "
        "g.g_settings_schema_source_new_from_directory.restype=c.c_void_p; "
        "g.g_settings_schema_source_lookup.argtypes=[c.c_void_p,c.c_char_p,c.c_int]; "
        "g.g_settings_schema_source_lookup.restype=c.c_void_p; "
        "s=g.g_settings_schema_source_new_from_directory(str(r/'share/glib-2.0/schemas').encode(),None,1,None); "
        "assert s; assert g.g_settings_schema_source_lookup(s,b'org.gtk.gtk4.Settings.FileChooser',0)",
        resources, env=env)
    run(macos / "vacards-sparrow", "--help", env=env, cwd=evidence, stdout=subprocess.DEVNULL)
    traced_env = dict(env, DYLD_PRINT_LIBRARIES="1")
    # /bin/sh is platform-protected and removes DYLD_* before a shell launcher
    # can observe it. Trace the same packaged Mach-O directly; exports above
    # already exercised the launcher and its relocated loader/schema settings.
    trace = subprocess.run([str(macos / "inkscape-bin"), "--version"], env=traced_env,
                           text=True, capture_output=True, check=True).stderr
    (evidence / "runtime-libraries.txt").write_text(trace)
    pp.require("/opt/homebrew/" not in trace and str(SOURCE.parent) + "/" not in
               trace.replace(str(app), "APP"), "external developer runtime loaded")
    pp.require("libcairo.2.dylib" in trace, "Cairo was not exercised")
    pp.require("libgtk-4.1.dylib" in trace, "GTK was not exercised")
    pp.require("librevenge-0.0" in trace, "librevenge was not exercised")
    pp.require(output("dwarfdump", "--uuid", resources / "lib/libgtk-4.1.dylib").split()[1]
               == manifest["gtk"]["uuid"], "packaged GTK is not the tested lifecycle build")
    # The paired patched librevenge must be present with its capability marker;
    # payload_sha256 above already binds the exact packaged bytes.
    librevenge_files = list((resources / "lib").glob("librevenge-0.0*.dylib"))
    pp.require(len(librevenge_files) == 1, "packaged librevenge is missing or ambiguous")
    marker = manifest["librevenge"]["capability_binary_marker"]
    pp.require(marker and marker.encode() in librevenge_files[0].read_bytes(),
               "packaged librevenge lacks the attested capability marker")
    (evidence / "PASS.txt").write_text("Internal-test package smoke checks passed; not production qualification.\n")


def verify_dmg(dmg, evidence, sha):
    # Disk Arbitration can reject mountpoints nested inside the external
    # workspace's sparsebundle. Mount locally; keep outputs in the evidence dir.
    with tempfile.TemporaryDirectory(prefix="vacards-test-dmg-", dir="/tmp") as temporary:
        mount = Path(temporary) / "mount"
        mount.mkdir()
        run("hdiutil", "attach", "-nobrowse", "-readonly", "-mountpoint", mount, dmg)
        try:
            verify(mount / (NAME + ".app"), evidence, sha)
        finally:
            run("hdiutil", "detach", mount)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path,
                        help="new output directory; existing output is never overwritten")
    parser.add_argument("--icc", required=True, type=Path)
    parser.add_argument("--gtk-prefix", type=Path,
                        help="tested patched GTK install prefix (required when building)")
    parser.add_argument("--gtk-sha256",
                        help="independently recorded tested GTK dylib hash (required when building)")
    parser.add_argument("--verify-app", type=Path)
    args = parser.parse_args()
    sha = output("git", "-C", SOURCE, "rev-parse", "HEAD")
    pp.require(not output("git", "-C", SOURCE, "status", "--porcelain", "--untracked-files=no"),
               "commit tracked changes before packaging")
    pins = output("git", "-C", SOURCE, "submodule", "status", "--recursive")
    pp.require(all(line.startswith(" ") for line in (" " + pins).splitlines()), "submodules not at pins")
    if args.verify_app:
        verify(args.verify_app.resolve(), args.output.resolve(), sha)
        return
    pp.require(args.gtk_prefix is not None and args.gtk_sha256,
               "supply the tested --gtk-prefix and --gtk-sha256; no stock GTK fallback")
    gtk = args.gtk_prefix.resolve()
    gtk_library = gtk / "lib/libgtk-4.1.dylib"
    pp.require(pp.digest(gtk_library) == args.gtk_sha256, "GTK differs from the tested binary")
    pp.require(pp.gtk_config()["prefix"] == str(gtk), "pkg-config must resolve the tested GTK prefix")
    gtk_patches = {}
    for name in ("gtk-4.22.4-macos-unmapped-freeze.patch", "gtk-4.22.4-icon-cache-dispose.patch",
                 "gtk-4.22.4-macos-deferred-file-menu.patch", "gtk-4.22.4-recent-groups.patch"):
        expected = pp.digest(SOURCE / "packaging/macos/vacards" / name)
        pp.require(pp.digest(gtk.parent / name) == expected, "GTK build patch mismatch: " + name)
        gtk_patches[name] = expected
    gtk_record = {"prefix": str(gtk), "input_sha256": args.gtk_sha256,
                  "uuid": output("dwarfdump", "--uuid", gtk_library).split()[1],
                  "patches": gtk_patches,
                  "build_options_sha256": pp.digest(gtk.parent / "build-options.json"),
                  "resolved_dependencies_sha256": pp.digest(gtk.parent / "resolved-dependencies.json")}
    args.output = args.output.resolve()
    args.output.mkdir()  # Keep failed evidence; never overwrite a previous attempt.
    cache = pp.read_env_cache(args.build / "CMakeCache.txt")
    pp.require(Path(cache["CMAKE_HOME_DIRECTORY"]).resolve() == SOURCE, "wrong build source")
    deps = pp.read_env(SOURCE / "VACARDS-DEPENDENCIES.env")
    features = pp.read_env(args.build / "VACARDS-CMAKE-FEATURES.env")
    for key, expected in (("required_cmake_features", "ON"), ("disabled_cmake_features", "OFF")):
        for feature in deps[key].split(","):
            pp.require(features.get(feature) == expected, f"build feature {feature} must be {expected}")
    version = pp.read_env(SOURCE / "packaging/vacards/VERSION.env")
    release = output(SOURCE / "packaging/vacards/vacards-version.sh", "--release")
    release += f"-build.{version['build_number']}-internal-test-{sha[:10]}"
    cairo = Path(cache["VACARDS_RESOLVED_CAIRO_PREFIX"])
    libcdr = Path(cache["VACARDS_RESOLVED_LIBCDR_PREFIX"])
    pp.require(cache.get("VACARDS_REQUIRE_PATCHED_LIBREVENGE") == "ON",
               "internal packages require VACARDS_REQUIRE_PATCHED_LIBREVENGE=ON")
    librevenge = Path(cache["VACARDS_RESOLVED_LIBREVENGE_PREFIX"])
    runtime = pp.runtime_env(cairo, gtk, libcdr, librevenge)
    actual_version = output(args.build / "bin/inkscape", "--version", env=runtime)
    pp.require(sha[:10] in actual_version, f"rebuild first; executable is {actual_version}")
    expected_product = output(SOURCE / "packaging/vacards/vacards-version.sh", "--name") + " " + output(SOURCE / "packaging/vacards/vacards-version.sh", "--display")
    pp.require(actual_version.startswith(expected_product + " ("), "executable product identity is missing or stale")
    for verifier, prefix in (("verify-vacards-cairo-prefix.sh", cairo),
                              ("verify-vacards-libcdr.sh", libcdr),
                              ("verify-vacards-librevenge.sh", librevenge)):
        run(SOURCE / "packaging/macos/vacards" / verifier, prefix)
    # The tracked verifier above already rejected a missing/mislabelled prefix
    # and proved the marker equals the tracked bundle manifest.
    librevenge_manifest = librevenge / "VACARDS-LIBREVENGE.env"
    librevenge_record = pp.read_env(librevenge_manifest)
    pp.require(pp.digest(args.icc) == deps["tiff_rgb_profile_sha256"], "wrong ICC profile")
    helper = args.build / "bin/vacards-sparrow"
    pp.require(pp.digest(helper) == deps["sparrow_darwin_arm64_sha256"], "wrong Sparrow helper")
    stage = args.output / "install"
    run("cmake", "--install", args.build, "--prefix", stage)
    # Capture against the actual build executable: this is development input
    # provenance, deliberately not a VACARDS-RELEASE-GATE.env attestation.
    captured = args.output / "runtime-inputs.json"
    schemas = args.output / "schemas"
    pp.capture(SimpleNamespace(output=captured, cache=args.build / "CMakeCache.txt",
                              executable=args.build / "bin/inkscape", cairo_prefix=cairo,
                              libcdr_prefix=libcdr, librevenge_prefix=librevenge,
                              schema_dir=schemas, gtk_prefix=gtk))
    data = pp.read_json(captured)
    pp.require(pp.digest(data["roles"]["gtk"]) == args.gtk_sha256,
               "captured runtime did not load the tested GTK binary")
    app = args.output / (NAME + ".app")
    macos = app / "Contents/MacOS"
    resources = app / "Contents/Resources"
    macos.mkdir(parents=True)
    resources.mkdir()
    shutil.copytree(stage / "share", resources / "share", copy_function=copy)
    shutil.copytree(schemas, resources / "share/glib-2.0/schemas", dirs_exist_ok=True, copy_function=copy)
    # Use system/user macOS fonts without depending on Homebrew configuration.
    fontconfig = resources / "etc/fonts/fonts.conf"
    fontconfig.parent.mkdir(parents=True)
    fontconfig.write_text('<?xml version="1.0"?><!DOCTYPE fontconfig SYSTEM "urn:fontconfig:fonts.dtd">'
                          '<fontconfig><dir>/System/Library/Fonts</dir><dir>/Library/Fonts</dir>'
                          '<dir>~/Library/Fonts</dir><cachedir prefix="xdg">fontconfig</cachedir>'
                          '<include ignore_missing="yes" prefix="xdg">fontconfig/fonts.conf</include>'
                          '</fontconfig>')
    copy(args.icc, resources / "share/inkscape/color/icc" / deps["tiff_rgb_profile_filename"],
         mode=PUBLIC_MODE)
    copy(SOURCE / "packaging/macos/res/VACards-AppIcon.icns", resources / "inkscape.icns")
    for source, name in ((SOURCE / "VACARDS-DEPENDENCIES.env", "VACARDS-DEPENDENCIES.env"),
                         (SOURCE / "packaging/vacards/VERSION.env", "VACARDS-VERSION.env"),
                         (cairo / "VACARDS-CAIRO.txt", "VACARDS-CAIRO.txt"),
                         (libcdr / "VACARDS-LIBCDR.env", "VACARDS-LIBCDR.env"),
                         (librevenge_manifest, "VACARDS-LIBREVENGE.env"),
                         (args.build / "VACARDS-CMAKE-FEATURES.env", "VACARDS-CMAKE-FEATURES.env")):
        copy(source, resources / name)
    with (SOURCE / "packaging/macos/res/inkscape.plist").open("rb") as stream:
        plist = plistlib.load(stream)
    plist.update(CFBundleName=NAME, CFBundleDisplayName=NAME,
                 # The internal bundle is entered through the native executable,
                 # which bootstraps its own resources; Contents/MacOS/inkscape
                 # stays only as a CLI-compatibility shell launcher.
                 CFBundleExecutable="inkscape-bin",
                 CFBundleIdentifier="com.vacards.inkscape.test", CFBundleShortVersionString=version["base_version"],
                 CFBundleVersion=version["build_number"], CFBundleGetInfoString=NAME + " " + version["display_version"].replace("_", " "),
                 VAStudioProductVersion=version["product_version"],
                 VACardsReleaseVersion=release, VACardsSourceCommit=sha, VACardsChannel="internal-test",
                 CFBundleIconFile="inkscape.icns", LSMinimumSystemVersion=deps["macos_deployment_target"])
    # Test builds are not the default handler for users' existing SVG documents.
    plist.pop("CFBundleDocumentTypes", None)
    with (app / "Contents/Info.plist").open("wb") as stream:
        plistlib.dump(plist, stream)
    images = {str((args.build / "bin/inkscape").resolve()): (args.build / "bin/inkscape", macos / "inkscape-bin")}
    cli = args.build / "bin/vastudio-cli"
    pp.require(cli.is_file(), "build vastudio_cli before packaging Build 30")
    cli_identity = json.loads(output(cli, "--version", "--json", env=runtime))
    # The CLI's `build` is the release string (internal note cli/session), not the build number.
    pp.require(cli_identity.get("build") == version["product_version"]
               and cli_identity.get("source_sha") == sha,
               "CLI identity differs from the package source")
    images[str(cli.resolve())] = (cli, macos / "vastudio-cli")
    (app / "Contents/Resources/cli-location.json").write_text(json.dumps({
        "executable": "Contents/MacOS/vastudio-cli", "identity": cli_identity}, indent=2) + "\n")
    observed = {str(Path(path).resolve()) for path in pp.runtime_paths(args.build / "bin/inkscape", cairo, gtk, libcdr, librevenge)}
    records = {}
    for record in data["libraries"]:
        real = record["file"]["realpath"]
        if cairo_family(real) and not Path(real).is_relative_to(cairo.resolve()):
            pinned = [candidate for candidate in data["libraries"]
                      if candidate["file"]["realpath"] in observed and
                      Path(candidate["file"]["realpath"]).is_relative_to(cairo.resolve()) and
                      Path(real).name in candidate["aliases"]]
            pp.require(len(pinned) == 1, f"no unique observed patched Cairo for {real}")
            continue
        # A stock librevenge core reached through Homebrew visio/wpg/wpd links
        # is canonicalized onto the single attested patched core (see Cairo).
        if librevenge_core_duplicate(real, data["libraries"], observed, librevenge, librevenge_record):
            continue
        records[real] = record
    for real, record in records.items():
        destination = (loader_destination(resources, record)
                       if record["loader"] else resources / "lib" / Path(real).name)
        images[real] = (Path(record["file"]["path"]), destination)
    query = Path(shutil.which("gdk-pixbuf-query-loaders")).resolve()
    images[str(query)] = (query, macos / "gdk-pixbuf-query-loaders")
    # Include a relocatable Python runtime for the existing extension system.
    pyversion = f"python{sys.version_info.major}.{sys.version_info.minor}"
    stdlib = Path(sysconfig.get_path("stdlib"))
    pydest = resources / "python/lib" / pyversion
    shutil.copytree(stdlib, pydest, copy_function=copy,
                    ignore=shutil.ignore_patterns("__pycache__", "site-packages", "test", "tests"))
    packages = pydest / "site-packages"
    packages.mkdir()
    python_packages = {}
    for name in ("lxml", "PIL", "numpy", "serial", "scour", "packaging", "pyparsing",
                 "cssselect", "tinycss2", "webencodings"):
        module = importlib.util.find_spec(name)
        pp.require(module is not None, f"missing extension prerequisite: {name}")
        package = Path(module.origin).parent
        shutil.copytree(package, packages / package.name, copy_function=copy,
                        ignore=shutil.ignore_patterns("__pycache__", "tests", "test"))
        distribution = importlib.metadata.distribution({"PIL": "Pillow", "serial": "pyserial"}.get(name, name))
        python_packages[distribution.metadata["Name"]] = distribution.version
        metadata_directories = {Path(distribution.locate_file(file)).parent
                                for file in distribution.files or [] if str(file).endswith(".dist-info/METADATA")}
        for metadata_directory in metadata_directories:
            shutil.copytree(metadata_directory, packages / metadata_directory.name, copy_function=copy)
        for native in package.rglob("*"):
            if native.is_file() and native.suffix in (".so", ".dylib"):
                images[str(native.resolve())] = (native, packages / package.name / native.relative_to(package))
    for native in stdlib.rglob("*.so"):
        if "site-packages" not in native.parts:
            images[str(native.resolve())] = (native, pydest / native.relative_to(stdlib))
    # Framework bin/python3 is a trampoline which spawns Resources/Python.app.
    # Bundle the actual interpreter, not that non-relocatable trampoline.
    python = (Path(sys.base_prefix) / "Resources/Python.app/Contents/MacOS/Python").resolve()
    pp.require(python.is_file(), "expected the framework's actual Python interpreter")
    python_license = Path(sys.base_prefix) / "LICENSE.txt"
    if python_license.exists():
        copy(python_license, resources / "python/LICENSE.txt")
    images[str(python)] = (python, macos / "python3-bin")
    # Extend the captured graph for Python plug-ins, preserving all app role pins.
    app_paths = [r["file"]["path"] for r in records.values() if not r["loader"]]
    extra_roots = [str(original) for real, (original, _) in images.items()
                   if real not in records and original not in (args.build / "bin/inkscape", python)]
    closure = pp.collect_libraries(app_paths + extra_roots, query, python, gtk)
    for record in closure:
        target = record["file"]["realpath"]
        # The observed app graph uses DYLD_LIBRARY_PATH for the entire patched
        # Cairo family, including the script interpreter (not just base/GObject).
        # Traversing the Python closure must not reintroduce Homebrew's copy.
        if cairo_family(target) and target not in records:
            pinned = [real for real, captured_record in records.items()
                      if Path(real).is_relative_to(cairo.resolve()) and
                      Path(target).name in captured_record["aliases"]]
            pp.require(len(pinned) == 1, f"unmatched Cairo-family dependency: {target}")
            continue
        if (target not in records and
                librevenge_core_duplicate(target, list(records.values()), observed,
                                          librevenge, librevenge_record)):
            continue
        records.setdefault(target, record)
        if target not in images:
            images[target] = (Path(record["file"]["path"]), resources / "lib" / Path(target).name)
    destinations = {}
    for real, (_, destination) in images.items():
        destinations.setdefault(str(destination), []).append(real)
    collisions = {dest: inputs for dest, inputs in destinations.items() if len(inputs) > 1}
    pp.require(not collisions, f"native filename collision: {collisions}")
    aliases = bundle_aliases(records, images)
    for real, (original, destination) in images.items():
        copy(original, destination)
    # The paired patched librevenge shares its release/basename with a stock
    # 0.0.6 library. Bind the copied bundle bytes to the verified runtime
    # prefix before relocation/signing mutates them.
    librevenge_source = str((librevenge / librevenge_record["library_relative_path"]).resolve())
    pp.require(librevenge_source in images, "captured runtime omitted the attested librevenge library")
    librevenge_destination = images[librevenge_source][1]
    pp.require(pp.digest(librevenge_destination) == librevenge_record["library_sha256"],
               "packaged librevenge bytes differ from the verified runtime dependency")
    librevenge_provenance = {
        "prefix": str(librevenge),
        "manifest_sha256": pp.digest(librevenge_manifest),
        "library_relative_path": librevenge_record["library_relative_path"],
        "library_sha256": librevenge_record["library_sha256"],
        "packaged_before_relocation_sha256": pp.digest(librevenge_destination),
        "capability_name": librevenge_record["capability_name"],
        "capability_value": librevenge_record["capability_value"],
        "capability_binary_marker": librevenge_record["capability_binary_marker"],
    }
    for real, (original, destination) in images.items():
        ident = pp.library_id(original)
        architectures = output("lipo", "-archs", original).split()
        pp.require("arm64" in architectures, f"missing arm64 slice: {original}")
        minimums = re.findall(r"(?m)^\s*minos ([0-9.]+)", output("vtool", "-show-build", original))
        target = tuple(map(int, deps["macos_deployment_target"].split(".")))
        pp.require(minimums and all(tuple(map(int, value.split(".")))[:2] <= target[:2]
                                   for value in minimums), f"incompatible minimum macOS: {original}")
        for link in pp.links(original):
            if link == ident or pp.system_path(link):
                continue
            # Wheels can carry private libraries with the same basename as an
            # app dependency. Honor the actual load-command path before aliases.
            exact = None
            if Path(link).is_absolute():
                exact = str(Path(link).resolve())
            elif link.startswith("@loader_path/"):
                exact = str((original.parent / link.removeprefix("@loader_path/")).resolve())
            elif link.startswith("@rpath/"):
                executable_context = args.build / "bin/inkscape"
                try:
                    exact = str(Path(pp.resolve_link(link, original, executable_context,
                        pp.image_rpaths(original, executable_context) +
                        pp.image_rpaths(executable_context, executable_context))).resolve())
                except pp.Rejected:
                    pass  # A unique captured alias remains required below.
            candidates = resolve_bundled_candidate(link, aliases, exact)
            pp.require(len(candidates) == 1, f"ambiguous/missing dependency {link} in {original}: {candidates}")
            target = images[next(iter(candidates))][1]
            run("install_name_tool", "-change", link,
                "@loader_path/" + os.path.relpath(target, destination.parent), destination)
        if ident:
            run("install_name_tool", "-id", "@rpath/" + destination.name, destination)
        for rpath in pp.rpaths(original):
            run("install_name_tool", "-delete_rpath", rpath, destination)
        run("ldid", "-S", destination)
    # Cache regeneration occurs at launch so read-only DMGs and relocated apps work.
    for name in ("inkscape", "python3"):
        copy(SOURCE / "packaging/macos/vacards" / ("internal-" + name + "-launcher.sh"), macos / name)
        (macos / name).chmod(0o755)
    # Preserve the exact pinned helper bytes, including its existing signature.
    copy(helper, macos / "vacards-sparrow")
    run("xattr", "-cr", app)
    pp.verify_inputs(data, runtime=True, gtk_prefix=gtk, librevenge_prefix=librevenge)
    pp.require(pp.digest(gtk_library) == args.gtk_sha256, "tested GTK changed during packaging")
    pp.require(pp.digest(librevenge_source) == librevenge_record["library_sha256"],
               "tested librevenge changed during packaging")
    # Sign the nested shell entries and seal the outer bundle in one checked
    # step; the pinned Sparrow helper is restored immediately afterwards.
    sign_internal_app_entry(app)
    copy(helper, macos / "vacards-sparrow")
    manifest = {"channel": "internal-test", "production_qualified": False, "source_commit": sha,
                "version": release, "gitlinks": pins, "build": str(args.build),
                "python_version": sys.version, "python_packages": python_packages,
                "configuration": cache, "runtime_inputs_sha256": pp.digest(captured),
                "input_binary_sha256": pp.digest(args.build / "bin/inkscape"),
                "icc_sha256": deps["tiff_rgb_profile_sha256"], "gtk": gtk_record,
                "librevenge": librevenge_provenance,
                "payload_sha256": {p.relative_to(app).as_posix(): pp.digest(p) for p in app.rglob("*")
                                   if p.is_file() and "_CodeSignature" not in p.parts
                                   and p.relative_to(app).as_posix() != MAIN_EXECUTABLE},
                # The main executable's embedded signature seals this manifest, so its
                # bytes necessarily change when the bundle is sealed below. Its integrity
                # is checked by codesign --verify --deep --strict and the version check.
                "payload_sealed_by_codesign": [MAIN_EXECUTABLE]}
    pp.write_json(resources / PUBLIC_BUNDLE_MANIFEST, manifest)
    # write_json stays private-evidence safe (0600); the *public* package
    # manifest is made explicitly world/group readable here, umask-independent.
    (resources / PUBLIC_BUNDLE_MANIFEST).chmod(PUBLIC_MODE)
    run("codesign", "--force", "--sign", "-", app)
    verify(app, args.output / "app-smoke", sha)
    root = args.output / "dmg-root"
    root.mkdir()
    run("ditto", app, root / app.name)
    (root / "Applications").symlink_to("/Applications")
    (root / "READ ME.txt").write_text(
        f"{NAME} {release}\nINTERNAL TEST — not notarized; not a production release.\n"
        "Drag VA Studio to Applications. Existing Inkscape/VACards apps are not replaced.\n"
        "Test settings are separate. Owner testing/sign-off is still required.\n")
    dmg = args.output / ("VA-Studio-" + release + "-arm64.dmg")
    run("hdiutil", "create", "-volname", NAME, "-srcfolder", root, "-format", "UDZO", dmg)
    run("hdiutil", "verify", dmg)
    verify_dmg(dmg, args.output / "dmg-smoke", sha)
    (args.output / (dmg.name + ".sha256")).write_text(pp.digest(dmg) + "  " + dmg.name + "\n")
    print("Created INTERNAL TEST installer:", dmg, flush=True)


if __name__ == "__main__":
    main()
