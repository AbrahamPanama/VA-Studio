#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Tested macOS packaging inputs and relocation receipts (Python stdlib only).

The full gate anchors INPUTS through VACARDS-BUILD-PROVENANCE.env. A receipt
records a controlled copy/relocation/sign operation; it is not a second test
attestation or a cryptographic signing authority. Production authorization is
blocked until independent authentication/acceptance is provisioned; structural
suite closure cannot supply that authority. The retained bundle integrity checks
use bundled evidence, never the original build/dependency directories.
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
import tempfile


INPUTS = "VACARDS-PACKAGING-INPUTS.json"
RECEIPT = "VACARDS-BUNDLE-PROVENANCE.json"
SCHEMAS = "VACARDS-GTK-SCHEMAS"
SCHEMA_DEST = "share/glib-2.0/schemas"
LOADER_DIR = "lib/gdk-pixbuf-2.0/2.10.0/loaders"
# gdk-pixbuf discovers most raster formats (TIFF, BMP, GIF, ...) as loadable
# modules; only PNG/JPEG are built into the core library. Ship every loader the
# app relies on and freeze each module's closure. SVG remains required, and TIFF
# is required because macOS File > Open matches the extension from these modules.
REQUIRED_LOADERS = ("libpixbufloader_svg.so", "libpixbufloader-tiff.so")
LOADER_DEST = LOADER_DIR + "/" + REQUIRED_LOADERS[0]
MIGRATE = ("older evidence lacks tested packaging inputs; rebuild patched Cairo "
           "with build-patched-cairo.sh if needed, then rerun run-release-gate.sh "
           "--full BUILD_DIR FRESH_INSTALL_PREFIX. Do not regenerate evidence "
           "from replacement dependencies")
ROLES = {"cairo": r"libcairo(?:\.[0-9]+)*\.dylib",
         "cairo_gobject": r"libcairo-gobject(?:\.[0-9]+)*\.dylib",
         "libcdr": r"libcdr-0\.1(?:\.[0-9]+)*\.dylib",
         "gtk": r"libgtk-4(?:\.[0-9]+)*\.dylib"}
TOOLS = ("dylibbundler", "install_name_tool", "ldid", "codesign")


class Rejected(Exception):
    pass


def require(condition, message):
    if not condition:
        raise Rejected(message)


def require_production_authorization():
    """Fail closed until TEST_GATE_INTEGRATION's independent boundary exists.

    No candidate file, ambient receipt, CLI flag or environment variable can
    supply missing production authority. Replace this rejection only with the
    reviewed independent authentication AND acceptance/predicate verifier;
    retain the format-3 packaging/runtime checks as additional requirements.
    test-results.py verify-closure is structural evidence, not that verifier.
    """
    raise Rejected(
        "production release closure is unavailable: independent baseline authentication "
        "and acceptance/predicate verification are not provisioned. Obtain approved inputs "
        "and the reviewed production verification boundary, then run a fresh integrated "
        "full gate. Use separate cmake/ctest development checks; existing or structural-only "
        "attestations cannot authorize packaging.")


def digest(path):
    h = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            h.update(block)
    return h.hexdigest()


def identity(path):
    path = Path(path).absolute()
    require(path.is_file(), f"required input is missing: {path}")
    return {"path": str(path), "realpath": str(path.resolve()), "sha256": digest(path)}


def check_file(record):
    require(identity(record["path"]) == record,
            f"tested input changed: {record['path']}")


def fields(value, names):
    require(isinstance(value, dict) and set(value) == set(names.split()),
            f"invalid provenance fields; expected {names}")


def required_string(value, label):
    require(isinstance(value, str) and value.strip() and "\0" not in value,
            f"{label} must be a nonempty string without NUL characters")


def absolute_path(value, label):
    required_string(value, label)
    require(Path(value).is_absolute(), f"{label} must be an absolute path")


def file_record(record):
    fields(record, "path realpath sha256")
    for key in ("path", "realpath"):
        require(isinstance(record[key], str) and Path(record[key]).is_absolute(),
                f"invalid absolute input path: {record[key]}")
    sha(record["sha256"])


def sha(value):
    require(isinstance(value, str) and re.fullmatch(r"[0-9a-f]{64}", value),
            "invalid provenance SHA-256")


def unique_object(pairs):
    result = {}
    for key, value in pairs:
        require(key not in result, f"duplicate provenance field: {key}")
        result[key] = value
    return result


def read_json(path):
    return json.loads(Path(path).read_text(), object_pairs_hook=unique_object)


def write_json(path, data):
    # No successful record is exposed until all checks/operations have finished.
    path = Path(path)
    with tempfile.NamedTemporaryFile(mode="w", dir=path.parent, delete=False) as out:
        temporary = Path(out.name)
        try:
            json.dump(data, out, indent=2, sort_keys=True, ensure_ascii=False)
            out.write("\n")
            out.close()
            temporary.replace(path)
        finally:
            temporary.unlink(missing_ok=True)


def read_env(path):
    pairs = []
    for line in Path(path).read_text().splitlines():
        if not line or line.startswith("#"):
            continue
        require("=" in line, f"invalid manifest line in {path}")
        pairs.append(line.split("=", 1))
    return unique_object(pairs)


def run(args, env=None):
    result = subprocess.run([str(a) for a in args], env=env, text=True,
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    require(result.returncode == 0,
            f"command failed: {args!r}\n{result.stderr.strip()}")
    return result.stdout, result.stderr


def tool(name, version=False):
    executable = shutil.which(name)
    require(executable, f"required tool not found: {name}")
    record = {"file": identity(executable)}
    if version:
        record["version"] = run([executable, "--version"])[0].strip()
    return record


def gtk_config():
    prefix = run(["pkg-config", "--variable=prefix", "gtk4"])[0].strip()
    require(prefix and Path(prefix).is_dir(), "GTK pkg-config prefix is missing")
    return {"prefix": str(Path(prefix).resolve()),
            "version": run(["pkg-config", "--modversion", "gtk4"])[0].strip(),
            "pkgconfig": tool("pkg-config")}


def schema_sources(directory):
    paths = sorted(p for p in Path(directory).iterdir()
                   if p.name.endswith((".gschema.xml", ".gschema.override")))
    require(any(p.name == "org.gtk.gtk4.Settings.FileChooser.gschema.xml" for p in paths),
            "GTK FileChooser schema source is missing")
    return {p.name: identity(p) for p in paths}


def system_path(path):
    return path.startswith(("/usr/lib/", "/System/Library/"))


def runtime_env(cairo_prefix, gtk_prefix=None, libcdr_prefix=None, librevenge_prefix=None):
    # Every ambient DYLD_* entry is discarded; the returned search path is
    # exactly the caller-supplied tested prefixes. libcdr and librevenge are
    # optional so the previous call sites keep their exact gtk-then-cairo value.
    # A supplied librevenge prefix goes first: libcdr links librevenge by
    # basename, so the paired patched copy must win over any stock 0.0.6 copy.
    env = {k: v for k, v in os.environ.items() if not k.startswith("DYLD_")}
    directories = [str(Path(cairo_prefix) / "lib")]
    if gtk_prefix is not None:
        directories.insert(0, str(Path(gtk_prefix) / "lib"))
    if libcdr_prefix is not None:
        directories.append(str(Path(libcdr_prefix) / "lib"))
    if librevenge_prefix is not None:
        directories.insert(0, str(Path(librevenge_prefix) / "lib"))
    env.update(DYLD_LIBRARY_PATH=os.pathsep.join(directories))
    return env


def runtime_paths(executable, cairo_prefix, gtk_prefix=None, libcdr_prefix=None,
                  librevenge_prefix=None):
    if librevenge_prefix is None:
        env = runtime_env(cairo_prefix, gtk_prefix, libcdr_prefix)
    else:
        env = runtime_env(cairo_prefix, gtk_prefix, libcdr_prefix, librevenge_prefix)
    env["DYLD_PRINT_LIBRARIES"] = "1"
    _, trace = run([executable, "--version"], env)
    paths = set()
    for line in trace.splitlines():
        match = re.match(r"^dyld(?:\[\d+\])?:\s+(?:loaded:\s+)?(?:<[^>]+>\s+)?(/.+)$", line)
        if match and match[1].endswith(".dylib") and not system_path(match[1]):
            paths.add(match[1])
    require(paths, "runtime trace contained no non-system dylibs")
    return sorted(paths)


def links(path):
    # Strip the compatibility-version suffix, not whitespace inside a path.
    output = run(["otool", "-L", path])[0]
    return list(dict.fromkeys(line.strip().split(" (compatibility version", 1)[0]
                             for line in output.splitlines()
                             if line.startswith(("\t", " ")) and " (compatibility version" in line))


def library_id(path):
    output = run(["otool", "-D", path])[0]
    values = [line.strip() for line in output.splitlines() if line.strip() and not line.endswith(":")]
    return values[-1] if values else ""


def rpaths(path):
    """Read ordered LC_RPATH values, preserving spaces in recorded directories."""
    output = run(["otool", "-l", path])[0]
    result = []
    for command in re.split(r"(?m)^Load command [0-9]+\s*$", output)[1:]:
        if not re.search(r"(?m)^\s*cmd LC_RPATH\s*$", command):
            continue
        values = re.findall(r"(?m)^\s*path (.+) \(offset [0-9]+\)\s*$", command)
        require(len(values) == 1, f"malformed LC_RPATH in {path}")
        result.append(values[0])
    return result


def expand_image_path(value, image, executable):
    """Expand only explicit dyld image-relative tokens, never cwd/search env."""
    for token, binary in (("@loader_path", image), ("@executable_path", executable)):
        if value == token or value.startswith(token + "/"):
            require(binary is not None, f"{value} needs an explicit executable context")
            return str(Path(binary).resolve().parent / value[len(token):].lstrip("/"))
    require(Path(value).is_absolute(),
            f"unsupported relative dyld path {value!r} in {image}; use explicit LC_RPATH inputs")
    return value


def image_rpaths(image, executable):
    return tuple(expand_image_path(value, image, executable) for value in rpaths(image))


def resolve_link(link, image, executable, run_paths):
    """First existing declared run path wins, in dyld load-chain order.

    The caller builds the stack from the referring image out to its ancestors;
    an ancestor's @loader_path is expanded relative to that ancestor, not to the
    child. Do not search Homebrew, PATH, cwd, sibling directories, or install IDs
    of unobserved files when a declared path cannot be resolved.
    """
    if link.startswith("@rpath/"):
        suffix = link.removeprefix("@rpath/")
        require(suffix and not suffix.startswith("/"), f"invalid run-path dependency: {link}")
        candidates = [str(Path(directory) / suffix) for directory in run_paths]
    else:
        candidates = [expand_image_path(link, image, executable)]
    for candidate in candidates:
        # System dylibs may reside only in dyld's shared cache.
        if system_path(candidate) or Path(candidate).is_file():
            return candidate
    raise Rejected(f"cannot freeze unresolved dependency {link} in {image}; "
                   f"declared paths tried: {candidates}. Supply the correct binary/LC_RPATH context; "
                   "ambient dependency-prefix guesses are not permitted")


def collect_libraries(paths, loaders, executable=None, gtk_prefix=None):
    """Freeze observed runtime images and every required GdkPixbuf module closure.

    ``loaders`` is one dlopen root or an iterable of them. Each is marked so the
    receipt ties every bundled module back to a tested input. Follow dependency
    edges with their own ordered LC_RPATH stack. The explicit dlopen root uses
    its own paths plus the main executable's, not a union of unrelated runtime
    libraries' run paths. Unknown caller-specific context must fail closed
    instead of searching an ambient dependency prefix.
    """
    if isinstance(loaders, (str, os.PathLike)):
        loaders = [loaders]
    loader_reals = {str(Path(loader).resolve()) for loader in loaders}
    records = {}
    observed = {str(Path(p).resolve()): Path(p).absolute() for p in paths}
    ids = {real: library_id(path) for real, path in observed.items()}
    visited = set()
    executable_real = str(Path(executable).resolve()) if executable is not None else None
    executable_rpaths = image_rpaths(executable, executable) if executable is not None else ()

    def observed_link(link):
        # Reuse an actually loaded image by exact install name/path. Cairo and
        # an explicitly supplied internal-test GTK prefix can override transitive
        # system links; selected_roles checks their observed prefix first.
        matches = {real for real, ident in ids.items() if ident == link}
        if Path(link).is_absolute():
            real = str(Path(link).resolve())
            if real in observed:
                matches.add(real)
        if gtk_prefix is not None and re.fullmatch(ROLES["gtk"], Path(link).name):
            matches = {real for real, path in observed.items()
                       if re.fullmatch(ROLES["gtk"], Path(real).name)
                       and Path(real).is_relative_to(Path(gtk_prefix).resolve())}
            require(len(matches) == 1, "missing unique observed GTK in the requested prefix")
        elif any(re.fullmatch(ROLES[role], Path(link).name) for role in ("cairo", "cairo_gobject")):
            matches = {real for real, path in observed.items()
                       if Path(link).name in (path.name, Path(real).name)}
        require(len(matches) <= 1, f"ambiguous observed runtime dependency {link}")
        return observed[next(iter(matches))] if matches else None

    def visit(value, inherited):
        path = Path(value).absolute()
        real = str(path.resolve())
        if real in records:
            records[real]["aliases"] = sorted(set(records[real]["aliases"] + [path.name]))
        if real in visited:
            return
        visited.add(real)  # A loaded image is reused; also terminates cycles.
        ident = ids[real] if real in ids else library_id(path)
        aliases = {path.name, Path(real).name}
        if ident:
            aliases.add(Path(ident).name)
        if real != executable_real:
            records[real] = {"file": identity(path), "aliases": sorted(aliases),
                             "loader": real in loader_reals}
        run_paths = image_rpaths(path, executable) + inherited
        for link in links(path):
            if link == ident or system_path(link):
                continue
            # Loader-relative spellings are contextual, not globally reusable
            # install-name keys shared by unrelated images.
            lookup = (expand_image_path(link, path, executable)
                      if link.startswith(("@loader_path/", "@executable_path/")) else link)
            selected = observed_link(lookup)
            target = str(selected) if selected is not None else resolve_link(link, path, executable, run_paths)
            if not system_path(target):
                visit(target, run_paths)

    if executable is not None:
        visit(executable, ())
    for path in paths:
        visit(path, executable_rpaths)
    for loader in loaders:
        visit(loader, executable_rpaths)
    return sorted(records.values(), key=lambda r: r["file"]["realpath"])


def selected_roles(paths, prefixes):
    result = {}
    for role, pattern in ROLES.items():
        candidates = {str(Path(p).resolve()): p for p in paths if re.fullmatch(pattern, Path(p).name)}
        require(len(candidates) == 1, f"runtime must load exactly one {role}; found {len(candidates)}")
        real = next(iter(candidates))
        prefix = prefixes["cairo" if role == "cairo_gobject" else role]
        require(Path(real).is_relative_to(prefix), f"runtime {role} is outside the tested prefix: {real}")
        result[role] = real
    return result


LIBREVENGE_LIBRARY = r"librevenge-0\.0(?:\.[0-9]+)*\.dylib"


def verify_librevenge(prefix, observed):
    """Require the observed runtime librevenge to be the attested prefix copy.

    This is a parallel to the libcdr provenance check: the paired patched
    librevenge has the same release/pkg-config name as stock 0.0.6, so only the
    prefix-bound bytes distinguish it. It deliberately does not enter the
    format-1 ``prefixes``/``roles`` schema; the caller records it separately.
    """
    prefix = Path(prefix).resolve()
    manifest = prefix / "VACARDS-LIBREVENGE.env"
    require(manifest.is_file(), f"missing librevenge manifest: {manifest}")
    values = read_env(manifest)
    relative = values.get("library_relative_path", "")
    require(relative and not Path(relative).is_absolute() and ".." not in Path(relative).parts,
            "unsafe librevenge library_relative_path")
    library = prefix / relative
    require(library.is_file(), f"missing attested librevenge library: {library}")
    expected = values.get("library_sha256", "")
    require(re.fullmatch(r"[0-9a-f]{64}", expected), "invalid librevenge library_sha256")
    require(digest(library) == expected, "librevenge library differs from its build provenance")
    matches = [str(p) for p in observed if re.fullmatch(LIBREVENGE_LIBRARY, Path(p).name)]
    require(len(matches) == 1, f"runtime must load exactly one librevenge; found {len(matches)}")
    real = str(Path(matches[0]).resolve())
    require(Path(real).is_relative_to(prefix),
            f"runtime librevenge is outside the attested prefix: {real}")
    require(digest(real) == expected, "loaded librevenge bytes differ from its build provenance")
    return real


def validate_inputs(data):
    fields(data, "format platform prefixes gtk schema_compiler schemas schema_cache schema_stage libraries roles files")
    require(type(data["format"]) is int and data["format"] == 1 and data["platform"] == "Darwin",
            "unsupported packaging input format/platform; format must be integer 1")
    fields(data["prefixes"], "cairo libcdr gtk")
    fields(data["roles"], "cairo cairo_gobject libcdr gtk")
    fields(data["gtk"], "prefix version pkgconfig")
    fields(data["schema_compiler"], "file version libraries")
    for group in ("prefixes", "roles"):
        for name, value in data[group].items():
            absolute_path(value, f"{group}.{name}")
    absolute_path(data["schema_stage"], "schema_stage")
    absolute_path(data["gtk"]["prefix"], "gtk.prefix")
    required_string(data["gtk"]["version"], "gtk.version")
    required_string(data["schema_compiler"]["version"], "schema_compiler.version")
    fields(data["gtk"]["pkgconfig"], "file")
    file_record(data["schema_compiler"]["file"])
    require(isinstance(data["schema_compiler"]["libraries"], list), "invalid schema compiler library inventory")
    for record in data["schema_compiler"]["libraries"]:
        file_record(record)
    file_record(data["gtk"]["pkgconfig"]["file"])
    fields(data["files"], "cache executable cairo_marker libcdr_manifest patch cxx_compiler")
    for record in data["files"].values():
        file_record(record)
    require(isinstance(data["schemas"], dict) and data["schemas"], "empty schema inventory")
    for name, record in data["schemas"].items():
        require(isinstance(name, str) and Path(name).name == name and name.endswith((".gschema.xml", ".gschema.override")),
                "unsafe schema filename")
        file_record(record)
    sha(data["schema_cache"])
    require(isinstance(data["libraries"], list) and data["libraries"], "empty library inventory")
    seen = set()
    for record in data["libraries"]:
        fields(record, "file aliases loader")
        file_record(record["file"])
        real = record["file"]["realpath"]
        require(real not in seen, "duplicate library input")
        seen.add(real)
        require(type(record["loader"]) is bool and isinstance(record["aliases"], list) and record["aliases"],
                "invalid library aliases")
        for name in record["aliases"]:
            require(isinstance(name, str) and name not in (".", "..") and Path(name).name == name,
                    "unsafe library alias")
    require(set(data["roles"].values()) <= seen, "required runtime library omitted from inventory")
    loader_records = [r for r in data["libraries"] if r["loader"]]
    require(len(loader_records) == len(REQUIRED_LOADERS),
            f"expected {len(REQUIRED_LOADERS)} gdk-pixbuf loaders; found {len(loader_records)}")
    present = {name for record in loader_records for name in record["aliases"]
               if name in REQUIRED_LOADERS}
    require(present == set(REQUIRED_LOADERS),
            f"expected gdk-pixbuf loader set {sorted(REQUIRED_LOADERS)}; found {sorted(present)}")
    return data


def load_inputs(path):
    require(Path(path).is_file(), MIGRATE)
    return validate_inputs(read_json(path))


def verify_inputs(data, cairo_prefix=None, runtime=False, gtk_prefix=None,
                  librevenge_prefix=None):
    validate_inputs(data)
    if gtk_prefix is not None:
        require(str(Path(gtk_prefix).resolve()) == data["prefixes"]["gtk"],
                "requested GTK prefix differs from the tested prefix")
    if cairo_prefix is not None:
        require(str(Path(cairo_prefix).resolve()) == data["prefixes"]["cairo"],
                "packaging Cairo prefix differs from the tested prefix")
    for record in data["files"].values():
        check_file(record)
    for record in data["libraries"]:
        check_file(record["file"])
    require(gtk_config() == data["gtk"], "GTK pkg-config inputs differ from the tested prefix/version/tool")
    check_file(data["schema_compiler"]["file"])
    require(tool("glib-compile-schemas")["file"] == data["schema_compiler"]["file"],
            "schema compiler differs from the tested compiler")
    for record in data["schema_compiler"]["libraries"]:
        check_file(record)
    require(schema_compiler(data["prefixes"]["cairo"]) == data["schema_compiler"],
            "schema compiler runtime differs from the tested compiler")
    source = Path(data["prefixes"]["gtk"]) / "share/glib-2.0/schemas"
    require(schema_sources(source) == data["schemas"], "GTK schema source set changed after testing")
    check_schemas(data, data["schema_stage"])
    if runtime:
        if librevenge_prefix is None:
            observed = runtime_paths(data["files"]["executable"]["path"], data["prefixes"]["cairo"],
                                     gtk_prefix, data["prefixes"]["libcdr"])
        else:
            observed = runtime_paths(data["files"]["executable"]["path"], data["prefixes"]["cairo"],
                                     gtk_prefix, data["prefixes"]["libcdr"], librevenge_prefix)
        require(selected_roles(observed, data["prefixes"]) == data["roles"],
                "runtime library selection changed after testing")
        approved = {r["file"]["realpath"] for r in data["libraries"]}
        require({str(Path(p).resolve()) for p in observed} <= approved,
                "runtime loaded a library absent from tested inputs")
        if librevenge_prefix is not None:
            verify_librevenge(librevenge_prefix, observed)


def schema_compiler(cairo_prefix):
    record = tool("glib-compile-schemas", version=True)
    record["libraries"] = [identity(p) for p in runtime_paths(record["file"]["path"], cairo_prefix)]
    return record


def capture(args):
    require(not Path(args.output).exists(), "input inventory already exists; use fresh gate evidence")
    gtk = gtk_config()
    gtk_prefix = getattr(args, "gtk_prefix", None)
    librevenge_prefix = getattr(args, "librevenge_prefix", None)
    if gtk_prefix is not None:
        require(str(Path(gtk_prefix).resolve()) == gtk["prefix"],
                "pkg-config does not resolve the requested GTK prefix")
    if librevenge_prefix is not None:
        librevenge_prefix = str(Path(librevenge_prefix).resolve())
    prefixes = {"cairo": str(Path(args.cairo_prefix).resolve()),
                "libcdr": str(Path(args.libcdr_prefix).resolve()), "gtk": gtk["prefix"]}
    source_root = Path(__file__).resolve().parents[2]
    marker = Path(prefixes["cairo"]) / "VACARDS-CAIRO.txt"
    patch = source_root / "packaging/macos/vacards/cairo-1.18.4-clip-all.patch"
    marker_values = unique_object(line.split(": ", 1) for line in marker.read_text().splitlines() if ": " in line)
    require("Patch SHA-256" in marker_values and "GObject Library SHA-256" in marker_values, MIGRATE)
    require(marker_values["Patch SHA-256"] == digest(patch), "Cairo patch bytes differ from the built patch")
    require(marker_values.get("Library SHA-256") == digest(Path(prefixes["cairo"]) / "lib/libcairo.2.dylib"),
            "Cairo library differs from its build provenance")
    require(marker_values["GObject Library SHA-256"] == digest(Path(prefixes["cairo"]) / "lib/libcairo-gobject.2.dylib"),
            "Cairo GObject library differs from its build provenance")
    cache = read_env_cache(args.cache)
    direct = [link for link in links(args.executable)
              if any(re.fullmatch(ROLES[role], Path(link).name) for role in ("cairo", "cairo_gobject"))]
    require(sum(bool(re.fullmatch(ROLES["cairo"], Path(link).name)) for link in direct) == 1,
            "build must directly link exactly one base Cairo library")
    require(all(Path(link).is_absolute() and Path(link).resolve().is_relative_to(prefixes["cairo"])
                for link in direct), "build directly links Cairo outside the tested prefix")
    if librevenge_prefix is None:
        paths = runtime_paths(args.executable, prefixes["cairo"], gtk_prefix, prefixes["libcdr"])
    else:
        paths = runtime_paths(args.executable, prefixes["cairo"], gtk_prefix,
                              prefixes["libcdr"], librevenge_prefix)
    roles = selected_roles(paths, prefixes)
    libcdr_manifest = Path(prefixes["libcdr"]) / "VACARDS-LIBCDR.env"
    require(read_env(libcdr_manifest).get("library_sha256") == digest(roles["libcdr"]),
            "loaded libcdr bytes differ from its build provenance")
    if librevenge_prefix is not None:
        verify_librevenge(librevenge_prefix, paths)
    loader_output = run(["gdk-pixbuf-query-loaders"])[0]
    loaders_by_name = {}
    for path in re.findall(r'^"([^"]*libpixbufloader[^"]*\.so)"', loader_output, flags=re.MULTILINE):
        loaders_by_name.setdefault(Path(path).name, []).append(path)
    loaders = []
    for name in REQUIRED_LOADERS:
        matches = loaders_by_name.get(name, [])
        require(len(matches) == 1, f"expected exactly one {name} input; found {len(matches)}")
        loaders.append(matches[0])
    schemas = schema_sources(Path(gtk["prefix"]) / "share/glib-2.0/schemas")
    compiler = schema_compiler(prefixes["cairo"])
    data = {"format": 1, "platform": "Darwin", "prefixes": prefixes, "gtk": gtk,
            "schema_compiler": compiler, "schemas": schemas,
            "schema_stage": str(Path(args.schema_dir).absolute()),
            "libraries": collect_libraries(paths, loaders, args.executable, gtk_prefix), "roles": roles,
            "files": {"cache": identity(args.cache), "executable": identity(args.executable),
                      "cairo_marker": identity(marker), "patch": identity(patch),
                      "libcdr_manifest": identity(libcdr_manifest),
                      "cxx_compiler": identity(cache["CMAKE_CXX_COMPILER"])}}
    stage = Path(args.schema_dir)
    stage.mkdir()  # Never overlay a prior gate's compiled schema cache.
    for name, record in schemas.items():
        shutil.copyfile(record["path"], stage / name)
    run([compiler["file"]["path"], "--strict", stage], runtime_env(prefixes["cairo"]))
    data["schema_cache"] = digest(stage / "gschemas.compiled")
    verify_inputs(data, runtime=True, gtk_prefix=gtk_prefix, librevenge_prefix=librevenge_prefix)
    write_json(args.output, data)


def read_env_cache(path):
    return unique_object((line.split("=", 1)[0].split(":", 1)[0], line.split("=", 1)[1])
                         for line in Path(path).read_text().splitlines()
                         if "=" in line and not line.startswith(("#", "//")))


def check_schemas(data, directory):
    directory = Path(directory)
    require(directory.is_dir(), f"tested schema directory is missing: {directory}")
    expected = set(data["schemas"]) | {"gschemas.compiled"}
    require({p.name for p in directory.iterdir()} == expected, "packaged/tested schema file set differs")
    for name, record in data["schemas"].items():
        require(not (directory / name).is_symlink(), f"schema input must be a regular copied file: {name}")
        require(digest(directory / name) == record["sha256"], f"schema bytes changed: {name}")
    require(not (directory / "gschemas.compiled").is_symlink(), "compiled schema cache must not be a symlink")
    require(digest(directory / "gschemas.compiled") == data["schema_cache"], "compiled schema cache changed")


def attest(directory, *, candidate=False):
    require_production_authorization()
    directory = Path(directory)
    # Only the explicit validation command opts in; bundle callers remain final-only.
    basename = "VACARDS-RELEASE-GATE.pending.env" if candidate else "VACARDS-RELEASE-GATE.env"
    gate = read_env(directory / basename)
    require(gate.get("format") == "3" and gate.get("scope") == "full", "packaging requires a full format-3 gate")
    provenance = directory / "VACARDS-BUILD-PROVENANCE.env"
    require(digest(provenance) == gate.get("build_provenance_sha256"), "build provenance differs from the gate")
    values = read_env(provenance)
    # This is an integrity link to the gate's existing provenance, not an
    # authentication mechanism or a substitute for re-observation by the gate.
    reservation = directory / "VACARDS-GATE-RUN.json"
    require(values.get("gate_run_sha256"),
            "gate reservation binding is missing; rerun run-release-gate.sh --full in a fresh build")
    sha(values["gate_run_sha256"])
    require(reservation.is_file() and not reservation.is_symlink(), "copied gate reservation is missing or linked")
    require(digest(reservation) == values["gate_run_sha256"], "gate reservation differs from the full gate")
    require(values.get("packaging_inputs_sha256"), MIGRATE)
    sha(values["packaging_inputs_sha256"])
    require((directory / INPUTS).is_file(), MIGRATE)
    require(digest(directory / INPUTS) == values["packaging_inputs_sha256"], "packaging inputs differ from the gate")
    data = load_inputs(directory / INPUTS)
    require(digest(directory / "VACARDS-CAIRO.txt") == data["files"]["cairo_marker"]["sha256"],
            "copied Cairo provenance differs from tested inputs")
    require(digest(directory / "VACARDS-LIBCDR.env") == data["files"]["libcdr_manifest"]["sha256"],
            "copied libcdr provenance differs from tested inputs")
    require(data["files"]["libcdr_manifest"]["sha256"] == gate.get("libcdr_manifest_sha256"),
            "tested libcdr provenance differs from the gate")
    libcdr = next(r for r in data["libraries"] if r["file"]["realpath"] == data["roles"]["libcdr"])
    require(libcdr["file"]["sha256"] == gate.get("libcdr_library_sha256"), "tested libcdr bytes differ from the gate")
    return data, gate


def copy_schemas(args):
    data = load_inputs(args.inputs)
    check_schemas(data, args.source)
    destination = Path(args.destination)
    destination.mkdir(parents=True, exist_ok=True)
    require(not list(destination.iterdir()), "schema destination must be empty; refusing untested schema overlays")
    for source in Path(args.source).iterdir():
        shutil.copyfile(source, destination / source.name)
    check_schemas(data, destination)


def payload_files(app):
    root = Path(app)
    return [root / "Contents/MacOS/inkscape-bin"] + sorted(
        p for p in (root / "Contents/Resources/lib").rglob("*")
        if p.is_file() and p.suffix in (".dylib", ".so"))


def relative_payload(app, path):
    root = Path(app).resolve()
    require(not path.is_symlink() and path.resolve().is_relative_to(root), "payload path escapes bundle or is a symlink")
    return path.relative_to(Path(app)).as_posix()


def required_loader_records(data):
    """Map each required GdkPixbuf module name to its tested inventory record."""
    result = {}
    for name in REQUIRED_LOADERS:
        matches = [record for record in data["libraries"]
                   if record["loader"] and name in record["aliases"]]
        require(len(matches) == 1, f"tested inputs must identify exactly one {name}")
        result[name] = matches[0]
    return result


def loader_relative(name):
    return LOADER_DIR + "/" + name


def restore_bundle(args):
    """Replace discovered copies with gate inputs, then relocate the complete graph.

    Dylibbundler remains the graph discovery/layout adapter. Its possibly stale
    dependency selections cannot become package inputs: every output candidate
    must map uniquely to a captured library before any final rewriting.
    """
    app = Path(args.app)
    resources = app / "Contents/Resources"
    data, gate = attest(resources)
    verify_inputs(data, cairo_prefix=args.cairo_prefix)
    require(not (resources / RECEIPT).exists(), "bundle transformation receipt already exists")
    executable = Path(args.install) / "bin/inkscape"
    require(digest(executable) == gate["binary_sha256"], "installed executable differs from the gate")
    sources = {}
    for path in payload_files(app):
        relative = relative_payload(app, path)
        if path == app / "Contents/MacOS/inkscape-bin":
            sources[relative] = identity(executable)
            continue
        candidates = [r for r in data["libraries"] if path.name in r["aliases"]]
        require(len(candidates) == 1, f"bundled library has no unique tested input: {path.name}")
        sources[relative] = candidates[0]["file"]
    seen = [r["realpath"] for r in sources.values()]
    require(len(seen) == len(set(seen)), "multiple bundled copies of one tested library")
    require(set(data["roles"].values()) <= set(seen), "bundle omits a required tested library")
    for name, record in required_loader_records(data).items():
        relative = "Contents/Resources/" + loader_relative(name)
        require(sources.get(relative, {}).get("realpath") == record["file"]["realpath"],
                f"bundle omits the tested {name}")
    tools = {name: tool(name) for name in TOOLS}
    sources_receipt = {}
    for relative, record in sources.items():
        check_file(record)
        destination = app / relative
        shutil.copyfile(record["path"], destination)
        require(digest(destination) == record["sha256"], f"copy changed tested bytes: {relative}")
        sources_receipt[relative] = {"source": record, "before_sha256": digest(destination)}
    commands = []
    for relative in sources:
        destination = app / relative
        ident = library_id(destination) if destination.suffix in (".dylib", ".so") else ""
        for link in links(destination):
            if link == ident or system_path(link):
                continue
            candidates = [target for target in sources if Path(target).name == Path(link).name]
            if not candidates:
                candidates = [target for target, record in sources.items()
                              if any(Path(link).name in r["aliases"] and r["file"]["realpath"] == record["realpath"]
                                     for r in data["libraries"])]
            require(len(candidates) == 1, f"unapproved dependency {link} in {relative}")
            new = "@loader_path/" + os.path.relpath(app / candidates[0], destination.parent)
            command = [tools["install_name_tool"]["file"]["path"], "-change", link, new, str(destination)]
            run(command)
            commands.append(["install_name_tool", "-change", link, new, relative])
        if ident:
            run([tools["install_name_tool"]["file"]["path"], "-id", "@rpath/" + destination.name, destination])
            commands.append(["install_name_tool", "-id", "@rpath/" + destination.name, relative])
    receipt = {"format": 1, "phase": "relocated", "inputs_sha256": digest(resources / INPUTS),
               "gate_sha256": digest(resources / "VACARDS-RELEASE-GATE.env"),
               "tools": tools, "transform": {"helper": identity(__file__),
               "bundler": identity(Path(__file__).resolve().parents[1] / "macos/vacards/bundle-vacards-app.sh")},
               "commands": commands, "files": sources_receipt}
    write_json(resources / RECEIPT, receipt)


def finish_bundle(args):
    require_production_authorization()
    resources = Path(args.app) / "Contents/Resources"
    receipt = read_json(resources / RECEIPT)
    fields(receipt, "format phase inputs_sha256 gate_sha256 tools transform commands files")
    require(type(receipt["format"]) is int and receipt["format"] == 1,
            "unsupported bundle provenance format; format must be integer 1")
    require(receipt["phase"] == "relocated", "bundle receipt was already finalized")
    require({relative_payload(args.app, path) for path in payload_files(args.app)} == set(receipt["files"]),
            "bundle binary set changed before receipt finalization")
    require(receipt["inputs_sha256"] == digest(resources / INPUTS) and
            receipt["gate_sha256"] == digest(resources / "VACARDS-RELEASE-GATE.env"),
            "tested evidence changed during transformation")
    for name, record in receipt["tools"].items():
        require(tool(name) == record, f"packaging tool changed during transformation: {name}")
    for record in receipt["transform"].values():
        check_file(record)
    for relative, record in receipt["files"].items():
        record["after_sha256"] = digest(Path(args.app) / relative)
    receipt["phase"] = "signed"
    # Nested signatures are now final; the caller seals this receipt with a
    # shallow outer signature and verifies that no recorded binary changed.
    receipt["commands"].extend([["ldid", "-S", "nested Mach-O files"],
                                ["codesign", "--force", "--deep", "--sign", "-", "APP"],
                                ["codesign", "--force", "--sign", "-", "APP (seal receipt)"]])
    write_json(resources / RECEIPT, receipt)


def verify_bundle(args):
    app = Path(args.app)
    resources = app / "Contents/Resources"
    data, gate = attest(resources)
    require((resources / RECEIPT).is_file(), "missing bundle transformation receipt; rebuild the app from a new full gate")
    receipt = read_json(resources / RECEIPT)
    fields(receipt, "format phase inputs_sha256 gate_sha256 tools transform commands files")
    require(type(receipt["format"]) is int and receipt["format"] == 1,
            "unsupported bundle provenance format; format must be integer 1")
    require(receipt["phase"] == "signed", "bundle transformation is incomplete")
    require(receipt["inputs_sha256"] == digest(resources / INPUTS), "receipt identifies different tested inputs")
    require(receipt["gate_sha256"] == digest(resources / "VACARDS-RELEASE-GATE.env"), "receipt identifies a different gate")
    fields(receipt["tools"], " ".join(TOOLS))
    fields(receipt["transform"], "helper bundler")
    for record in receipt["tools"].values():
        fields(record, "file")
        file_record(record["file"])
    for record in receipt["transform"].values():
        file_record(record)
    require(isinstance(receipt["commands"], list) and receipt["commands"], "missing transformation commands")
    actual = {relative_payload(app, p) for p in payload_files(app)}
    require(actual == set(receipt["files"]), "bundle binary set differs from the transformation receipt")
    approved = {r["file"]["realpath"]: r["file"] for r in data["libraries"]}
    seen = set()
    for relative, record in receipt["files"].items():
        fields(record, "source before_sha256 after_sha256")
        file_record(record["source"])
        sha(record["before_sha256"])
        sha(record["after_sha256"])
        require(record["before_sha256"] == record["source"]["sha256"], "pre-rewrite hash differs from copied input")
        if relative == "Contents/MacOS/inkscape-bin":
            require(record["before_sha256"] == gate["binary_sha256"], "pre-rewrite executable differs from gate")
        else:
            source = record["source"]
            require(approved.get(source["realpath"]) == source, "pre-rewrite library differs from tested input")
            require(source["realpath"] not in seen, "duplicate tested library in bundle")
            seen.add(source["realpath"])
        require(digest(app / relative) == record["after_sha256"], f"post-signing binary changed: {relative}")
    require(set(data["roles"].values()) <= seen, "bundle omits a required tested library")
    for name, expected in required_loader_records(data).items():
        entry = receipt["files"].get("Contents/Resources/" + loader_relative(name), {})
        require(entry.get("source") == expected["file"], f"bundle omits the tested {name}")
    require((resources / SCHEMA_DEST).resolve().is_relative_to(app.resolve()), "schema directory escapes bundle")
    check_schemas(data, resources / SCHEMA_DEST)
    for relative in actual:
        path = app / relative
        ident = library_id(path) if path.suffix in (".dylib", ".so") else ""
        for link in links(path):
            if link == ident or system_path(link):
                continue
            require(link.startswith("@loader_path/"), f"unapproved runtime dependency: {link}")
            target = (path.parent / link.removeprefix("@loader_path/")).resolve()
            require(target.is_relative_to(app.resolve()), f"runtime dependency escapes bundle: {link}")
            require(target.relative_to(app.resolve()).as_posix() in actual, f"runtime dependency absent from receipt: {link}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    p = sub.add_parser("require-release-authorization",
                       help="require the independent production boundary (currently unavailable)")
    p.set_defaults(action=lambda a: require_production_authorization())
    p = sub.add_parser("capture")
    for name in ("cache", "executable", "cairo-prefix", "libcdr-prefix", "output", "schema-dir"):
        p.add_argument("--" + name, required=True)
    p.add_argument("--librevenge-prefix")
    p.set_defaults(action=capture)
    p = sub.add_parser("verify-inputs")
    p.add_argument("inputs")
    p.add_argument("--cairo-prefix")
    p.add_argument("--librevenge-prefix")
    p.add_argument("--runtime", action="store_true")
    p.set_defaults(action=lambda a: verify_inputs(load_inputs(a.inputs), a.cairo_prefix, a.runtime,
                                                 librevenge_prefix=a.librevenge_prefix))
    p = sub.add_parser("verify-attestation")
    p.add_argument("directory")
    p.add_argument("--candidate", action="store_true", help="validate the pending basename only; not packaging authorization")
    p.set_defaults(action=lambda a: attest(a.directory, candidate=a.candidate))
    p = sub.add_parser("copy-schemas")
    for name in ("inputs", "source", "destination"):
        p.add_argument("--" + name, required=True)
    p.set_defaults(action=copy_schemas)
    p = sub.add_parser("restore-bundle")
    for name in ("app", "install", "cairo-prefix"):
        p.add_argument("--" + name, required=True)
    p.set_defaults(action=restore_bundle)
    for name, action in (("finish-bundle", finish_bundle), ("verify-bundle", verify_bundle)):
        p = sub.add_parser(name)
        p.add_argument("app")
        p.set_defaults(action=action)
    args = parser.parse_args()
    try:
        args.action(args)
        if args.command == "verify-attestation" and args.candidate:
            print("Validated pending attestation inputs; not packaging authorization")
    except (Rejected, OSError, ValueError, KeyError, TypeError) as error:
        parser.exit(1, f"VACards packaging provenance rejected: {error}\n")


if __name__ == "__main__":
    main()
