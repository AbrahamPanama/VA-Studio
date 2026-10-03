#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Generate share/doc/THIRD-PARTY-NOTICES.md for VA Studio (Python standard library only).

Inputs (local files unless --refresh-crates-io is given):

* src/3rdparty/vacards-nesting-rs/Cargo.lock and vendor/*/: crate metadata and the
  license files shipped inside each vendored crate.
* src/3rdparty/sparrow/windows-x64.Cargo.lock plus the crates.io metadata cache
  packaging/public-release/crates-io-sparrow.json. --refresh-crates-io rebuilds the
  cache with read-only GETs of https://crates.io/api/v1/crates/NAME/VERSION and
  records the URL, the retrieval time and whether the registry checksum matches
  the lockfile. The cache is the recorded source of each Sparrow crate license.
* --sparrow-vendor DIR (optional): a `cargo vendor` tree for the Sparrow lockfile.
  It supplies the crates' own license texts, which crates.io metadata does not.
* src/3rdparty/sparrow/bin/darwin-arm64/sparrow: crate paths embedded in the
  executable (no lockfile is recorded for that build; the list is partial).
* packaging/public-release/rust-std-*.json: the Rust standard library notices of
  each toolchain that builds shipped code. --import-rust-std FILE VERSION creates
  one from a toolchain's share/doc/rust/COPYRIGHT-library.html.
* packaging/public-release/third-party-components.json: curated source-tree
  components, runtime libraries, external build inputs, assets and inputs.
* --platform-license-file ID=PATH (optional, repeatable): runtime license texts
  captured on a build host (for example the MinGW-w64 runtime terms).
* --sbom FILE (optional, repeatable): platform library inventories written by
  capture-sbom-msys2.sh or capture-sbom-homebrew.sh.

Outputs: --output (default share/doc/THIRD-PARTY-NOTICES.md) and --status (default
packaging/public-release/notices-status.json, a machine-readable completeness
record). --check regenerates both in memory and fails if the committed files differ.

Copyright holders come from each crate's own license and notice files (lines
that name a year or "(c)"), else from the authors in its package metadata, else
the crate's contributors are named with its repository.

Submodule components are read from the working tree when present, otherwise from
the pinned gitlink objects with `git show` (read-only), so the generator works in
the development repository and in the flattened public tree alike.
"""

import argparse
import datetime
import hashlib
import html
import json
import os
import re
import subprocess
import sys
import time
import tomllib
import urllib.request
from pathlib import Path

HERE = Path(__file__).resolve().parent
DEFAULT_ROOT = HERE.parents[1]
NESTING = "src/3rdparty/vacards-nesting-rs"
SPARROW_LOCK = "src/3rdparty/sparrow/windows-x64.Cargo.lock"
SPARROW_DARWIN = "src/3rdparty/sparrow/bin/darwin-arm64/sparrow"
CACHE = "packaging/public-release/crates-io-sparrow.json"
COMPONENTS = "packaging/public-release/third-party-components.json"
RUST_STD_GLOB = "rust-std-*.json"
OUTPUT = "share/doc/THIRD-PARTY-NOTICES.md"
STATUS = "packaging/public-release/notices-status.json"
LIBCDR_DIR = "third_party/libcdr-vacards"
USER_AGENT = "va-studio-third-party-notices/1 (license metadata for a notices file)"
LICENSE_NAME = re.compile(r"^(licen[cs]e|copying|copyright|notice|unlicense)", re.I)
# A copyright line names a year or "(c)"/"©": "Copyright (c) 2014 Name", "Copyright 2018 Name",
# "Copyright (c) Name", "© 2020 Name". License body text ("copyright notice that ...",
# "(c) You must retain ...") does not qualify.
COPYRIGHT_LINE = re.compile(
    r"^\s*(?:copyright\s*(?:\(c\)|©)?\s*(?:[0-9]{4}|\(c\)|©)|\(c\)\s*[0-9]{4}|©\s*[0-9]{4})", re.I)
TEMPLATE_HINT = re.compile(r"\[yyyy\]|\{yyyy\}|<year>|\[name of copyright owner\]|\{name of copyright owner\}"
                           r"|<copyright holders?>|<owner>|<name of author>", re.I)
CRATE_PATH = re.compile(rb"index\.crates\.io-[0-9a-f]{16}[/\\]([A-Za-z0-9_\-]+?)-(\d+\.\d+\.\d+(?:-[0-9A-Za-z.]+)?)[/\\]")
SECTIONS = ["Source-tree components", "Rust crates in the nesting engine", "Sparrow nesting helper",
            "Runtime libraries linked into the programs", "Assets and external inputs", "Platform libraries",
            "License texts", "Status of this file"]


def fail(message):
    print(f"generate-third-party-notices: {message}", file=sys.stderr)
    sys.exit(1)


class Source:
    """Read files from the working tree, falling back to pinned submodule objects."""

    def __init__(self, root):
        self.root = Path(root)
        self._gitlinks = None

    def _git(self, *args, binary=False):
        result = subprocess.run(["git", "-C", str(self.root), *args], capture_output=True)
        if result.returncode != 0:
            return None
        return result.stdout if binary else result.stdout.decode("utf-8", "replace")

    def gitlinks(self):
        """Map of submodule path -> pinned commit, recursively, from git objects.

        The libcdr fork is not a submodule in the development repository; its pinned
        commit (VACARDS-DEPENDENCIES.env) is mapped to third_party/libcdr-vacards,
        where the public tree carries it.
        """
        if self._gitlinks is not None:
            return self._gitlinks
        links = {}
        manifest = self.root / "VACARDS-DEPENDENCIES.env"
        if manifest.is_file() and not (self.root / LIBCDR_DIR).is_dir():
            for line in manifest.read_text("utf-8").splitlines():
                if line.startswith("libcdr_commit="):
                    links[LIBCDR_DIR] = line.split("=", 1)[1].strip()
        listing = self._git("ls-tree", "-r", "HEAD")
        pending = [("", line) for line in (listing or "").splitlines()]
        while pending:
            prefix, line = pending.pop()
            meta, _, path = line.partition("\t")
            mode, kind, sha = meta.split()
            if mode != "160000":
                continue
            full = prefix + path
            links[full] = sha
            nested = self._git("ls-tree", "-r", sha)
            if nested is not None:
                pending.extend((full + "/", entry) for entry in nested.splitlines())
        self._gitlinks = links
        return links

    def _object_spec(self, relative):
        for module, sha in sorted(self.gitlinks().items(), key=lambda item: -len(item[0])):
            if relative == module or relative.startswith(module + "/"):
                return f"{sha}:{relative[len(module) + 1:]}"
        return None

    def read_bytes(self, relative):
        path = self.root / relative
        if path.is_file():
            return path.read_bytes()
        spec = self._object_spec(relative)
        if spec:
            return self._git("show", spec, binary=True)
        return None

    def exists(self, relative):
        return self.read_bytes(relative) is not None


def read_toml(data):
    return tomllib.loads(data.decode("utf-8") if isinstance(data, bytes) else data)


def lock_packages(source, relative):
    data = source.read_bytes(relative)
    if data is None:
        fail(f"missing lockfile {relative}")
    packages = read_toml(data).get("package", [])
    return sorted(packages, key=lambda p: (p["name"], p["version"]))


def copyright_lines(text):
    return [" ".join(line.split()) for line in text.splitlines()
            if COPYRIGHT_LINE.match(line) and not TEMPLATE_HINT.search(line)]


def normalized_body(text):
    """The license text without its copyright lines, whitespace-normalized (for grouping)."""
    body = [line for line in text.splitlines()
            if not (COPYRIGHT_LINE.match(line) and not TEMPLATE_HINT.search(line))]
    return " ".join(" ".join(body).split())


def holders_of(name, documents, authors, repository):
    """Copyright holders of one crate: its notices' copyright lines, else its authors, else contributors."""
    lines = []
    for _, text in documents:
        for line in copyright_lines(text):
            if line not in lines:
                lines.append(line)
    if lines:
        return "; ".join(lines)
    if authors:
        return "Copyright the authors of " + name + ": " + ", ".join(authors)
    return f"Copyright the {name} contributors" + (f" ({repository})" if repository else "")


def license_files_in(directory, declared=None):
    names = []
    if directory.is_dir():
        for entry in sorted(directory.iterdir()):
            if entry.is_file() and LICENSE_NAME.match(entry.name):
                names.append(entry.name)
    if declared and declared not in names and (directory / declared).is_file():
        names.append(declared)
    if not names and directory.is_dir():
        # Some crates (r-efi) carry their license grants and copyright lines in AUTHORS.
        names = [e.name for e in sorted(directory.iterdir()) if e.is_file() and e.name.upper().startswith("AUTHORS")]
    return names


def vendored_crates(root):
    vendor = root / NESTING / "vendor"
    crates = {}
    for directory in sorted(vendor.iterdir()):
        manifest = directory / "Cargo.toml"
        if not manifest.is_file():
            continue
        package = read_toml(manifest.read_bytes()).get("package", {})
        key = (package["name"], str(package["version"]))
        crates[key] = {
            "dir": directory,
            "relative": directory.relative_to(root).as_posix(),
            "license": package.get("license") or "",
            "license_file": package.get("license-file"),
            "authors": package.get("authors", []),
            "repository": package.get("repository", ""),
        }
    return crates


def crate_texts(info, root, overrides, key):
    """Return a list of (relative path, text) license documents for one crate."""
    documents = []
    if info is not None:
        for name in license_files_in(info["dir"], info.get("license_file")):
            path = info["dir"] / name
            documents.append((path.relative_to(root).as_posix(), path.read_text("utf-8", "replace")))
    for relative in overrides.get(f"{key[0]}@{key[1]}", []):
        path = root / relative
        if path.is_file():
            documents.append((relative, path.read_text("utf-8", "replace")))
    return documents


def fetch_crates_io(packages, previous):
    entries = {}
    for package in packages:
        if "registry+" not in package.get("source", ""):
            continue
        name, version = package["name"], package["version"]
        key = f"{name}@{version}"
        url = f"https://crates.io/api/v1/crates/{name}/{version}"
        request = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
        with urllib.request.urlopen(request, timeout=60) as response:
            meta = json.load(response)["version"]
        entries[key] = {
            "name": name,
            "version": version,
            "license": meta.get("license") or "",
            "registry_checksum": meta.get("checksum") or "",
            "lock_checksum": package.get("checksum", ""),
            "checksum_matches_lock": bool(package.get("checksum")) and meta.get("checksum") == package.get("checksum"),
            "repository": meta.get("repository") or "",
            "source_of_license": url,
            "retrieved_utc": datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
        }
        time.sleep(1.0)  # crates.io asks API clients for at most one request per second
    previous.update(entries)
    return previous


def import_rust_std(html_path, version):
    """Extract a toolchain's COPYRIGHT-library.html into rust-std-VERSION.json data."""
    text = Path(html_path).read_text("utf-8")
    plain = lambda fragment: html.unescape(re.sub(r"<[^>]+>", " ", fragment)).strip()
    in_tree = []
    tree_start = text.find('id="in-tree-files"')
    tree_end = text.find('id="out-of-tree-dependencies"')
    if tree_start < 0 or tree_end < 0:
        fail(f"{html_path}: not a Rust COPYRIGHT-library.html")
    for block in re.findall(r"<b>File/Directory:</b>(.*?)(?=<b>File/Directory:</b>|$)",
                            text[tree_start:tree_end], re.S):
        path = plain(re.split(r"</p>", block, maxsplit=1)[0])
        license_id = re.search(r"<b>License:</b>(.*?)</p>", block, re.S)
        rights = [" ".join(plain(r).split()) for r in re.findall(r"<b>Copyright:</b>(.*?)</p>", block, re.S)]
        in_tree.append({"path": path, "license": plain(license_id.group(1)) if license_id else "",
                        "copyright": rights})
    if not in_tree:
        fail(f"{html_path}: no in-tree license entries found")
    crates, texts = [], {}
    for name_version, body in re.findall(r"<h3>📦\s*([^<]+)</h3>(.*?)(?=<h3>|</body>)", text[tree_end:], re.S):
        match = re.fullmatch(r"(.+?)-([0-9]+\.[0-9]+\.[0-9]+\S*)", name_version.strip())
        if not match:
            fail(f"{html_path}: cannot read crate name and version from {name_version!r}")
        name, crate_version = match.groups()
        url = re.search(r"<b>URL:</b>\s*<a href=\"([^\"]+)\"", body)
        authors = re.search(r"<b>Authors:</b>(.*?)</p>", body, re.S)
        license_id = re.search(r"<b>License:</b>(.*?)</p>", body, re.S)
        notices = []
        for file_name, notice in re.findall(r"<summary><code>([^<]+)</code></summary>\s*<pre>(.*?)</pre>", body, re.S):
            content = html.unescape(notice).strip("\n") + "\n"
            digest = hashlib.sha256(content.encode()).hexdigest()
            texts[digest] = content
            notices.append({"file": html.unescape(file_name), "sha256": digest})
        author_text = " ".join(plain(authors.group(1)).split()) if authors else ""
        crates.append({
            "name": name, "version": crate_version, "url": url.group(1) if url else "",
            "authors": [author_text] if author_text else [],
            "license": plain(license_id.group(1)) if license_id else "", "notices": notices,
        })
    return {
        "schema": "va-studio-rust-std-notices/1",
        "rust_version": version,
        "source": "share/doc/rust/COPYRIGHT-library.html of the Rust " + version + " toolchain",
        "source_sha256": hashlib.sha256(Path(html_path).read_bytes()).hexdigest(),
        "in_tree": in_tree,
        "crates": crates,
        "texts": dict(sorted(texts.items())),
    }


def darwin_observed(source):
    data = source.read_bytes(SPARROW_DARWIN)
    if not data:
        return []
    return sorted({(m.group(1).decode(), m.group(2).decode()) for m in CRATE_PATH.finditer(data)})


def esc(value):
    return str(value).replace("|", "\\|").replace("\n", " ")


class Notices:
    def __init__(self, args):
        self.args = args
        self.root = Path(args.root).resolve()
        self.source = Source(self.root)
        self.lines = []
        self.status = {"schema": "va-studio-notices-status/1", "complete": True, "issues": []}
        components = self.source.read_bytes(COMPONENTS)
        if components is None:
            fail(f"missing {COMPONENTS}")
        self.components = json.loads(components)
        self.overrides = self.components.get("crate_license_files", {})
        self.captured = {}
        for item in args.platform_license_file:
            ident, _, path = item.partition("=")
            if not ident or not path or not Path(path).is_file():
                fail(f"--platform-license-file expects ID=PATH of an existing file: {item}")
            self.captured[ident] = (Path(path).name, Path(path).read_text("utf-8", "replace"))
        self.text_groups = {}
        self.no_text = []

    def issue(self, area, message):
        self.status["complete"] = False
        self.status["issues"].append({"area": area, "issue": message})

    def out(self, line=""):
        self.lines.append(line)

    def heading(self, number):
        self.out(f"## {number}. {SECTIONS[number - 1]}")
        self.out()

    # ------------------------------------------------------------------ sections
    def header(self):
        self.out("# Third-party notices for VA Studio")
        self.out()
        self.out("<!-- Generated by packaging/public-release/generate-third-party-notices.py; do not edit by hand. -->")
        self.out()
        self.out("VA Studio is a modified version of Inkscape. As a whole it is distributed under the GNU "
                 "General Public License, version 3 or later (see `COPYING` and `LICENSE`). It contains or "
                 "is distributed with the third-party components listed here, each under its own license.")
        self.out()
        self.out("- Full license texts: `LICENSES/` in the source tree; in an installation, the "
                 "`share/inkscape/doc` folder (macOS: inside the application bundle's `Contents/Resources`).")
        self.out("- How to obtain the complete corresponding source code: `SOURCE.md`.")
        self.out("- Where a component is offered under a choice of licenses, VA Studio redistributes it under "
                 "each of the offered licenses; recipients may choose as the component's license allows.")
        self.out()
        self.out("## Contents")
        self.out()
        for number, title in enumerate(SECTIONS, 1):
            self.out(f"{number}. {title}")
        self.out()

    def components_section(self):
        self.heading(1)
        self.out("| Component | Location | Version or commit | License | License files | Upstream | Shipped | Modified |")
        self.out("| --- | --- | --- | --- | --- | --- | --- | --- |")
        for component in self.components["components"]:
            if component.get("kind") == "runtime-static":
                continue  # section 4
            files = self.component_files(component)
            for relative in component.get("inputs", []):
                if not self.source.exists(relative):
                    self.issue("components", f"{component['id']}: build input not found: {relative}")
            modified = component["modified"]
            if component.get("upstream_base"):
                modified += f" (upstream base: {component['upstream_base']})"
            self.out("| " + " | ".join(esc(v) for v in (
                component["name"], f"`{component['path']}`", component["pin"], component["license"],
                ", ".join(files), component["upstream"], component["shipped"], modified)) + " |")
        self.out()
        self.out("The VA Studio forks (marked as modified) are part of this source tree; their commits exist "
                 "only in the VA Studio history, and the upstream base each one starts from is named above.")
        self.out()

    def component_files(self, component):
        files = []
        for name in component["license_files"]:
            relative = name[2:] if name.startswith("@/") else (
                os.path.normpath(os.path.join(component["path"], name)).replace(os.sep, "/"))
            if not self.source.exists(relative):
                self.issue("components", f"{component['id']}: license file not found: {relative}")
                files.append(f"`{relative}` (missing)")
            else:
                files.append(f"`{relative}`")
        return files

    def nesting_section(self):
        self.heading(2)
        self.out(f"The nesting engine (`{NESTING}`, GPL-2.0-or-later) is a Rust static library linked into "
                 "VA Studio. Its dependencies are vendored in the source tree with their own license files. "
                 "Target-specific crates (for example the WebAssembly crates) are vendored for the lockfile "
                 "but are not compiled for Windows or macOS; they are listed for completeness.")
        self.out()
        packages = lock_packages(self.source, f"{NESTING}/Cargo.lock")
        vendored = vendored_crates(self.root)
        self.out("| Crate | Version | License | Copyright | Source | License files |")
        self.out("| --- | --- | --- | --- | --- | --- |")
        count = 0
        for package in packages:
            key = (package["name"], package["version"])
            source = package.get("source", "")
            if not source:
                continue  # the workspace crate itself
            count += 1
            info = vendored.get(key)
            if info is None:
                self.issue("nesting-crates", f"{key[0]} {key[1]} is in Cargo.lock but not vendored")
                self.out(f"| {key[0]} | {key[1]} | ? | ? | {esc(source)} | not vendored |")
                continue
            documents = crate_texts(info, self.root, self.overrides, key)
            holders = holders_of(key[0], documents, info["authors"], info["repository"])
            self.record_texts(f"{key[0]} {key[1]}", info["license"], documents, holders)
            origin = "crates.io" if source.startswith("registry+") else source.split("#")[0]
            names = ", ".join(f"`{Path(p).name}`" for p, _ in documents) or "none in crate (see section 7)"
            self.out(f"| {key[0]} | {key[1]} | {esc(info['license'])} | {esc(holders)} | {esc(origin)} | {names} |")
        self.out()
        self.out(f"{count} crates. Each crate's sources are under `{NESTING}/vendor/`.")
        self.out()
        self.status["nesting_crates"] = count

    def sparrow_section(self):
        self.heading(3)
        self.out("`vacards-sparrow` is the unmodified Sparrow program (MIT, Copyright (c) 2025 Jeroen Gardeyn, "
                 "KU Leuven) built from upstream commit 57c45cd295f5d2ce2a11edf6e765318a51d2b41e. It is a "
                 "separate executable that VA Studio starts as a child process. Its license is in "
                 "`src/3rdparty/sparrow/LICENSE` and is installed as `share/inkscape/sparrow/LICENSE`. "
                 "The executable contains the Rust crates below and the Rust standard library (section 4). "
                 "jagua-rs is licensed under MPL-2.0; its source code is available from "
                 "https://crates.io/crates/jagua-rs and https://github.com/JeroenGar/jagua-rs, and version "
                 f"0.8.0 is also in `{NESTING}/vendor/jagua-rs`.")
        self.out()
        cache_bytes = self.source.read_bytes(CACHE)
        cache = json.loads(cache_bytes) if cache_bytes else {}
        entries = cache.get("crates", {})
        packages = lock_packages(self.source, SPARROW_LOCK)
        vendored = vendored_crates(self.root)
        sparrow_vendor = Path(self.args.sparrow_vendor).resolve() if self.args.sparrow_vendor else None
        missing_texts = 0
        self.out("### 3.1 Windows x64 executable (from `src/3rdparty/sparrow/windows-x64.Cargo.lock`)")
        self.out()
        self.out("| Crate | Version | License | Copyright | License recorded from | Checksum matches lock | Crate license texts |")
        self.out("| --- | --- | --- | --- | --- | --- | --- |")
        count = 0
        for package in packages:
            if not package.get("source"):
                continue
            count += 1
            key = (package["name"], package["version"])
            meta = entries.get(f"{key[0]}@{key[1]}")
            if meta is None:
                self.issue("sparrow-crates", f"{key[0]} {key[1]}: no crates.io metadata cached (--refresh-crates-io)")
                self.out(f"| {key[0]} | {key[1]} | ? | ? | not recorded | ? | ? |")
                continue
            texts, holders = None, None
            if sparrow_vendor is not None:
                info = self.sparrow_vendor_info(sparrow_vendor, key)
                if info is not None:
                    documents = crate_texts(info, info["dir"].parent, {}, key)
                    holders = holders_of(key[0], documents, info["authors"], meta.get("repository", ""))
                    if documents:
                        self.record_texts(f"{key[0]} {key[1]}", meta["license"],
                                          [(f"sparrow-vendor/{p}", t) for p, t in documents], holders)
                        texts = "from `cargo vendor`"
            if texts is None and key in vendored:
                info = vendored[key]
                holders = holders_of(key[0], crate_texts(info, self.root, self.overrides, key),
                                     info["authors"], info["repository"])
                texts = f"same crate as in section 2 (`{info['relative']}`)"
            if texts is None:
                missing_texts += 1
                texts = "NOT CAPTURED"
            if holders is None:
                holders = (f"the {key[0]} contributors ({meta.get('repository') or 'crates.io'}); "
                           "copyright lines not captured yet")
            matches = "yes" if meta.get("checksum_matches_lock") else "NO"
            if not meta.get("checksum_matches_lock"):
                self.issue("sparrow-crates", f"{key[0]} {key[1]}: crates.io checksum does not match the lockfile")
            self.out(f"| {key[0]} | {key[1]} | {esc(meta['license'])} | {esc(holders)} | crates.io API, "
                     f"{meta['retrieved_utc'][:10]} | {matches} | {texts} |")
        self.out()
        self.status["sparrow_crates"] = count
        if missing_texts:
            self.issue("sparrow-crates", f"{missing_texts} crate license texts not captured; rerun with "
                                         "--sparrow-vendor on a host with `cargo vendor` output for the lockfile")
            self.out(f"**Incomplete:** the crates' own license files and copyright lines are not yet captured "
                     f"for {missing_texts} crates. Their license identifiers above come from crates.io; the "
                     "standard texts are in `LICENSES/`. Before a public release this file is generated again "
                     "with `--sparrow-vendor` (see packaging/public-release/README.md).")
            self.out()
        observed = darwin_observed(self.source)
        lock_keys = {(p["name"], p["version"]) for p in packages}
        self.out("### 3.2 macOS arm64 executable")
        self.out()
        self.out("No lockfile was recorded for the macOS arm64 build. The crate paths embedded in the "
                 "executable identify at least the following crates (a partial list):")
        self.out()
        extra = []
        for name, version in observed:
            marker = "" if (name, version) in lock_keys else " (not in the Windows lockfile)"
            if marker:
                extra.append((name, version))
            known = entries.get(f"{name}@{version}")
            license_id = known["license"] if known else (vendored.get((name, version)) or {}).get("license", "?")
            self.out(f"- {name} {version}: {license_id}{marker}")
        self.out()
        if observed:
            self.issue("sparrow-crates", "macOS arm64 Sparrow build has no recorded lockfile; rebuild it from a "
                                         "recorded lock (and with --remap-path-prefix) or record its crate list")
        self.status["sparrow_darwin_observed"] = len(observed)
        self.status["sparrow_darwin_not_in_windows_lock"] = [f"{n} {v}" for n, v in extra]

    def sparrow_vendor_info(self, vendor, key):
        for candidate in (vendor / key[0], vendor / f"{key[0]}-{key[1]}"):
            manifest = candidate / "Cargo.toml"
            if manifest.is_file():
                package = read_toml(manifest.read_bytes()).get("package", {})
                if package.get("name") == key[0] and str(package.get("version")) == key[1]:
                    return {"dir": candidate, "license_file": package.get("license-file"),
                            "authors": package.get("authors", [])}
        return None

    def runtime_section(self):
        self.heading(4)
        self.out("Compilers link parts of their runtime libraries into every program they build. These are "
                 "the runtime libraries in the VA Studio programs.")
        self.out()
        self.out("| Component | Version | License | License files | Upstream | Linked into |")
        self.out("| --- | --- | --- | --- | --- | --- |")
        for component in self.components["components"]:
            if component.get("kind") != "runtime-static":
                continue
            files = self.component_files(component)
            for ident in component.get("capture_ids", []):
                if ident in self.captured:
                    files.append(f"captured `{self.captured[ident][0]}` (below)")
                else:
                    files.append(f"`{ident}` license text: captured on the build host at release time")
                    self.issue("runtime", f"{component['id']}: {ident} license text not captured "
                                          "(--platform-license-file on the Windows build host)")
            self.out("| " + " | ".join(esc(v) for v in (
                component["name"], component["pin"], component["license"], ", ".join(files),
                component["upstream"], component["shipped"])) + " |")
        self.out()
        for ident, (name, text) in sorted(self.captured.items()):
            self.out(f"### {ident}: `{name}` (captured from the build host)")
            self.out()
            fence = "````" if "```" in text else "```"
            self.out(fence + "text")
            self.out(text.rstrip("\n"))
            self.out(fence)
            self.out()
        self.rust_std()

    def rust_std(self):
        files = sorted((self.root / "packaging/public-release").glob(RUST_STD_GLOB))
        versions = []
        for path in files:
            data = json.loads(path.read_text("utf-8"))
            version = data["rust_version"]
            versions.append(version)
            self.out(f"### Rust standard library {version}")
            self.out()
            self.out(f"From the {data['source']} (SHA-256 `{data['source_sha256']}`). The standard library "
                     "is licensed under Apache-2.0 OR MIT, except as listed here. Crates that only serve "
                     "other targets (WebAssembly, SGX, UEFI, Hermit, other Windows targets) are listed as in "
                     "the toolchain's notice file but are not compiled into VA Studio's programs.")
            self.out()
            for entry in data["in_tree"]:
                rights = "; ".join(entry["copyright"])
                self.out(f"- `{entry['path']}`: {entry['license']}; Copyright {rights}")
            self.out()
            self.out("| Crate | Version | License | Copyright |")
            self.out("| --- | --- | --- | --- |")
            for crate in data["crates"]:
                documents = [(note["file"], data["texts"][note["sha256"]]) for note in crate["notices"]]
                holders = holders_of(crate["name"], documents, crate["authors"], crate["url"])
                self.record_texts(f"{crate['name']} {crate['version']} (Rust {version} standard library)",
                                  crate["license"], [(f"{crate['name']}-{crate['version']}/{f}", t)
                                                     for f, t in documents], holders)
                self.out(f"| {crate['name']} | {crate['version']} | {esc(crate['license'])} | {esc(holders)} |")
            self.out()
        if "1.88.0" not in versions:
            self.issue("runtime", "Rust 1.88.0 standard library notices missing (--import-rust-std)")
        if "1.90.0" not in versions:
            self.issue("runtime", "Rust 1.90.0 standard library notices (Windows Sparrow helper) not imported; run "
                                  "--import-rust-std with that toolchain's COPYRIGHT-library.html")
            self.out("The Rust 1.90.0 standard library notices of the Windows Sparrow helper are imported from "
                     "that toolchain at release time; the macOS Sparrow helper's toolchain is not recorded.")
            self.out()

    def assets_section(self):
        self.heading(5)
        self.out("| Item | Location | License | Status |")
        self.out("| --- | --- | --- | --- |")
        for asset in self.components["assets"]:
            if asset["status"] in ("blocked", "needs-owner", "needs-provenance"):
                self.issue("assets", f"{asset['id']}: {asset['status']}")
            self.out(f"| {esc(asset['name'])} | {esc(', '.join(asset['paths']))} | {esc(asset['license'])} | {asset['status']} |")
        self.out()

    def platform_section(self):
        self.heading(6)
        self.out("The Windows package bundles libraries and data from MSYS2 UCRT64 packages; the macOS package "
                 "bundles libraries from Homebrew formulae. Their exact package versions, licenses and license "
                 "files are generated at release time from the SBOM captured on the build hosts "
                 "(`packaging/public-release/capture-sbom-msys2.sh`, `capture-sbom-homebrew.sh`). The Windows "
                 "package also carries the MSYS2 license directory as `share/licenses/ucrt64`.")
        self.out()
        if not self.args.sbom:
            self.issue("platform", "platform libraries: generated at release time from the SBOM (no --sbom given)")
            self.out("**Platform libraries: generated at release time from the SBOM.** No SBOM was supplied "
                     "to this generation run, so this file does not list their versions yet.")
            self.out()
            return
        for path in self.args.sbom:
            self.sbom_table(Path(path))

    def sbom_table(self, path):
        header, rows = {}, []
        for line in path.read_text("utf-8").splitlines():
            if line.startswith("#"):
                for token in line[1:].split():
                    if "=" in token:
                        k, v = token.split("=", 1)
                        header[k] = v
                continue
            if not line.strip():
                continue
            fields = line.split("\t")
            if fields[0] == "file":
                continue
            rows.append(fields)
        if header.get("schema") != "va-studio-platform-sbom/1":
            fail(f"{path}: not a va-studio-platform-sbom/1 file")
        packages = {}
        for fields in rows:
            if len(fields) < 7:
                fail(f"{path}: malformed row {fields}")
            file_, sha, package, version, license_id, license_files, url = fields[:7]
            entry = packages.setdefault((package, version), {"license": license_id, "files": 0,
                                                             "license_files": set(), "url": url})
            entry["files"] += 1
            entry["license_files"].update(f for f in license_files.split(";") if f)
            if package in ("UNKNOWN", ""):
                self.issue("platform", f"{path.name}: {file_} has no owning package")
            if not license_files and package not in ("local-build",):
                self.issue("platform", f"{path.name}: {package} {version} has no license file path")
        self.out(f"### {header.get('platform', path.name)} (captured {header.get('captured_utc', '?')})")
        self.out()
        self.out("| Package | Version | License | Files | License files | Project |")
        self.out("| --- | --- | --- | --- | --- | --- |")
        for (package, version), entry in sorted(packages.items()):
            self.out(f"| {esc(package)} | {esc(version)} | {esc(entry['license'])} | {entry['files']} | "
                     f"{esc('; '.join(sorted(entry['license_files'])))} | {esc(entry['url'])} |")
        self.out()

    # ------------------------------------------------------------- license texts
    def record_texts(self, label, expression, documents, holders):
        if not documents:
            self.no_text.append((label, expression, holders))
            return
        for relative, text in documents:
            digest = hashlib.sha256(normalized_body(text).encode()).hexdigest()
            self.text_groups.setdefault(digest, []).append((label, relative, holders, text))

    def texts_section(self):
        self.heading(7)
        self.out("Each text is printed once. Texts that differ only in their copyright lines are grouped; the "
                 "copyright holders of every crate that uses a text are listed above it.")
        self.out()
        number = 0
        for digest, members in sorted(self.text_groups.items(), key=lambda kv: kv[1][0][0]):
            number += 1
            label, relative, holders, text = members[0]
            title = next((line.strip() for line in text.splitlines()
                          if line.strip() and not COPYRIGHT_LINE.match(line)), "License text")
            self.out(f"### 7.{number}. {esc(title.lstrip('# -'))[:80]}")
            self.out()
            self.out("Used by:")
            self.out()
            seen = set()
            for member_label, member_path, member_holders, _ in members:
                if (member_label, member_path) in seen:
                    continue
                seen.add((member_label, member_path))
                self.out(f"- {member_label} (`{member_path}`): {esc(member_holders)}")
            self.out()
            fence = "````" if "```" in text else "```"
            self.out(fence + "text")
            self.out(text.rstrip("\n"))
            self.out(fence)
            self.out()
        if self.no_text:
            self.out("### Crates without license files in their package")
            self.out()
            self.out("These crates do not ship a license file. The standard text of each license named in their "
                     "license expression is in `LICENSES/`.")
            self.out()
            for label, expression, holders in sorted(self.no_text):
                self.out(f"- {label} ({esc(expression)}): {esc(holders)}")
            self.out()

    def status_section(self):
        self.heading(8)
        if self.status["complete"]:
            self.out("Complete for the inputs listed above.")
        else:
            self.out("**Incomplete.** The following items must be resolved before a public release:")
            self.out()
            for item in self.status["issues"]:
                self.out(f"- {item['area']}: {esc(item['issue'])}")
        self.out()

    def render(self):
        self.header()
        self.components_section()
        self.nesting_section()
        self.sparrow_section()
        self.runtime_section()
        self.assets_section()
        self.platform_section()
        self.texts_section()
        self.status_section()
        text = "\n".join(self.lines).rstrip("\n") + "\n"
        status = json.dumps(self.status, indent=2, ensure_ascii=False) + "\n"
        return text, status


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--root", default=str(DEFAULT_ROOT))
    parser.add_argument("--output", default=None)
    parser.add_argument("--status", default=None)
    parser.add_argument("--check", action="store_true")
    parser.add_argument("--refresh-crates-io", action="store_true")
    parser.add_argument("--import-rust-std", nargs=2, metavar=("COPYRIGHT_LIBRARY_HTML", "RUST_VERSION"))
    parser.add_argument("--sparrow-vendor")
    parser.add_argument("--platform-license-file", action="append", default=[], metavar="ID=PATH")
    parser.add_argument("--sbom", action="append", default=[])
    args = parser.parse_args()
    root = Path(args.root).resolve()
    output = Path(args.output) if args.output else root / OUTPUT
    status_path = Path(args.status) if args.status else root / STATUS

    if args.import_rust_std:
        html_path, version = args.import_rust_std
        if not re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+", version):
            fail("RUST_VERSION must look like 1.88.0")
        data = import_rust_std(html_path, version)
        target = root / "packaging/public-release" / f"rust-std-{version}.json"
        target.write_text(json.dumps(data, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
        print(f"wrote {target} ({len(data['crates'])} crates, {len(data['texts'])} distinct notice texts)")

    if args.refresh_crates_io:
        source = Source(root)
        packages = lock_packages(source, SPARROW_LOCK)
        known = {(p["name"], p["version"]) for p in packages}
        extra = [{"name": n, "version": v, "source": "registry+https://github.com/rust-lang/crates.io-index"}
                 for n, v in darwin_observed(source) if (n, v) not in known]
        cache_path = root / CACHE
        previous = json.loads(cache_path.read_text("utf-8")).get("crates", {}) if cache_path.is_file() else {}
        crates = fetch_crates_io(packages + extra, previous)
        cache_path.write_text(json.dumps({
            "schema": "va-studio-crates-io-cache/1",
            "about": "crates.io version metadata for the Sparrow helper's crates (windows-x64.Cargo.lock plus "
                     "crates observed only in the macOS arm64 executable). Written by "
                     "generate-third-party-notices.py --refresh-crates-io; each entry names its source URL.",
            "crates": dict(sorted(crates.items())),
        }, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")

    text, status = Notices(args).render()
    if args.check:
        stale = []
        if not output.is_file() or output.read_text("utf-8") != text:
            stale.append(str(output))
        if not status_path.is_file() or status_path.read_text("utf-8") != status:
            stale.append(str(status_path))
        if stale:
            fail("out of date: " + ", ".join(stale) + " (rerun generate-third-party-notices.py)")
        print("third-party notices are up to date")
        return
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(text, encoding="utf-8")
    status_path.write_text(status, encoding="utf-8")
    state = "complete" if json.loads(status)["complete"] else "INCOMPLETE"
    print(f"wrote {output} ({len(text)} bytes, {state}) and {status_path}")


if __name__ == "__main__":
    main()
