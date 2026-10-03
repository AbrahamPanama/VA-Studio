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
* packaging/public-release/third-party-components.json: curated source-tree
  components, external build inputs, assets and external inputs.
* --sbom FILE (optional, repeatable): platform library inventories written by
  capture-sbom-msys2.sh or capture-sbom-homebrew.sh.

Outputs: --output (default share/doc/THIRD-PARTY-NOTICES.md) and --status (default
packaging/public-release/notices-status.json, a machine-readable completeness
record used by check-public-release.py). --check regenerates both in memory and
fails if the committed files differ.

Submodule components are read from the working tree when present, otherwise from
the pinned gitlink objects with `git show` (read-only), so the generator works in
the development repository and in the flattened public tree alike.
"""

import argparse
import datetime
import hashlib
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
OUTPUT = "share/doc/THIRD-PARTY-NOTICES.md"
STATUS = "packaging/public-release/notices-status.json"
LIBCDR_DIR = "third_party/libcdr-vacards"
USER_AGENT = "va-studio-third-party-notices/1 (license metadata for a notices file)"
LICENSE_NAME = re.compile(r"^(licen[cs]e|copying|copyright|notice|unlicense)", re.I)
COPYRIGHT_LINE = re.compile(r"^\s*(?:copyright\b|\(c\)\s|©)", re.I)
TEMPLATE_HINT = re.compile(r"\[yyyy\]|\{yyyy\}|<year>|\[name of copyright owner\]|\{name of copyright owner\}", re.I)
CRATE_PATH = re.compile(rb"index\.crates\.io-[0-9a-f]{16}[/\\]([A-Za-z0-9_\-]+?)-(\d+\.\d+\.\d+(?:-[0-9A-Za-z.]+)?)[/\\]")


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

    def origin(self, relative):
        if (self.root / relative).is_file():
            return "working tree"
        spec = self._object_spec(relative)
        return f"git object {spec.split(':', 1)[0][:12]}" if spec else "missing"


def read_toml(data):
    return tomllib.loads(data.decode("utf-8") if isinstance(data, bytes) else data)


def lock_packages(source, relative):
    data = source.read_bytes(relative)
    if data is None:
        fail(f"missing lockfile {relative}")
    packages = read_toml(data).get("package", [])
    return sorted(packages, key=lambda p: (p["name"], p["version"]))


def split_copyright(text):
    rights, body = [], []
    for line in text.splitlines():
        if COPYRIGHT_LINE.match(line) and not TEMPLATE_HINT.search(line):
            rights.append(" ".join(line.split()))
        else:
            body.append(line)
    normalized = " ".join(" ".join(body).split())
    return rights, normalized


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
        self.text_groups = {}

    def issue(self, area, message):
        self.status["complete"] = False
        self.status["issues"].append({"area": area, "issue": message})

    def out(self, line=""):
        self.lines.append(line)

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
        for number, title in enumerate(["Source-tree components", "Rust crates in the nesting engine",
                                        "Sparrow nesting helper", "Assets and external inputs",
                                        "Platform libraries", "License texts used by the Rust crates",
                                        "Status of this file"], 1):
            self.out(f"{number}. {title}")
        self.out()

    def components_section(self):
        self.out("## 1. Source-tree components")
        self.out()
        self.out("| Component | Location | Version or commit | License | License files | Upstream | Shipped | Modified |")
        self.out("| --- | --- | --- | --- | --- | --- | --- | --- |")
        for component in self.components["components"]:
            files = []
            for name in component["license_files"]:
                relative = name[2:] if name.startswith("@/") else (
                    os.path.normpath(os.path.join(component["path"], name)).replace(os.sep, "/"))
                if not self.source.exists(relative):
                    self.issue("components", f"{component['id']}: license file not found: {relative}")
                    files.append(f"`{relative}` (missing)")
                else:
                    files.append(f"`{relative}`")
            for relative in component.get("inputs", []):
                if not self.source.exists(relative):
                    self.issue("components", f"{component['id']}: build input not found: {relative}")
            self.out("| " + " | ".join(esc(v) for v in (
                component["name"], f"`{component['path']}`", component["pin"], component["license"],
                ", ".join(files), component["upstream"], component["shipped"], component["modified"])) + " |")
        self.out()
        self.out("The forks marked as modified are published as part of this source tree; their exact "
                 "upstream base commits are listed in the Modified column.")
        self.out()

    def nesting_section(self):
        self.out("## 2. Rust crates in the nesting engine")
        self.out()
        self.out(f"The nesting engine (`{NESTING}`, GPL-2.0-or-later) is a Rust static library linked into "
                 "VA Studio. Its dependencies are vendored in the source tree with their own license files. "
                 "Target-specific crates (for example the WebAssembly crates) are vendored for the lockfile "
                 "but are not compiled for Windows or macOS; they are listed for completeness.")
        self.out()
        packages = lock_packages(self.source, f"{NESTING}/Cargo.lock")
        vendored = vendored_crates(self.root)
        self.out("| Crate | Version | License | Source | License files |")
        self.out("| --- | --- | --- | --- | --- |")
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
                self.out(f"| {key[0]} | {key[1]} | ? | {esc(source)} | not vendored |")
                continue
            documents = crate_texts(info, self.root, self.overrides, key)
            self.record_texts(key, info["license"], documents, info["authors"], info["repository"])
            origin = "crates.io" if source.startswith("registry+") else source.split("#")[0]
            names = ", ".join(f"`{Path(p).name}`" for p, _ in documents) or "none in crate (see section 6)"
            self.out(f"| {key[0]} | {key[1]} | {esc(info['license'])} | {esc(origin)} | {names} |")
        self.out()
        self.out(f"{count} crates. Each crate's sources are under `{NESTING}/vendor/`.")
        self.out()
        self.status["nesting_crates"] = count

    def sparrow_section(self):
        self.out("## 3. Sparrow nesting helper")
        self.out()
        self.out("`vacards-sparrow` is the unmodified Sparrow program (MIT, Copyright (c) 2025 Jeroen Gardeyn, "
                 "KU Leuven) built from upstream commit 57c45cd295f5d2ce2a11edf6e765318a51d2b41e. It is a "
                 "separate executable that VA Studio starts as a child process. Its license is in "
                 "`src/3rdparty/sparrow/LICENSE` and is installed as `share/inkscape/sparrow/LICENSE`. "
                 "The executable contains the Rust crates below. jagua-rs is licensed under MPL-2.0; its "
                 "source code is available from https://crates.io/crates/jagua-rs and "
                 "https://github.com/JeroenGar/jagua-rs, and version 0.8.0 is also in "
                 f"`{NESTING}/vendor/jagua-rs`.")
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
        self.out("| Crate | Version | License | License recorded from | Checksum matches lock | Crate license texts |")
        self.out("| --- | --- | --- | --- | --- | --- |")
        count = 0
        for package in packages:
            if not package.get("source"):
                continue
            count += 1
            key = (package["name"], package["version"])
            meta = entries.get(f"{key[0]}@{key[1]}")
            if meta is None:
                self.issue("sparrow-crates", f"{key[0]} {key[1]}: no crates.io metadata cached (--refresh-crates-io)")
                self.out(f"| {key[0]} | {key[1]} | ? | not recorded | ? | ? |")
                continue
            texts = None
            if sparrow_vendor is not None:
                info = self.sparrow_vendor_info(sparrow_vendor, key)
                if info is not None:
                    documents = crate_texts(info, info["dir"].parent, {}, key)
                    if documents:
                        self.record_texts(key, meta["license"], [(f"sparrow-vendor/{p}", t) for p, t in documents], info["authors"])
                        texts = "from `cargo vendor`"
            if texts is None and key in vendored:
                texts = f"same crate as in section 2 (`{vendored[key]['relative']}`)"
            if texts is None:
                missing_texts += 1
                texts = "NOT CAPTURED"
            matches = "yes" if meta.get("checksum_matches_lock") else "NO"
            if not meta.get("checksum_matches_lock"):
                self.issue("sparrow-crates", f"{key[0]} {key[1]}: crates.io checksum does not match the lockfile")
            self.out(f"| {key[0]} | {key[1]} | {esc(meta['license'])} | crates.io API, {meta['retrieved_utc'][:10]} | {matches} | {texts} |")
        self.out()
        self.status["sparrow_crates"] = count
        if missing_texts:
            self.issue("sparrow-crates", f"{missing_texts} crate license texts not captured; rerun with "
                                         "--sparrow-vendor on a host with `cargo vendor` output for the lockfile")
            self.out(f"**Incomplete:** the crates' own license files and copyright lines are not yet captured "
                     f"for {missing_texts} crates. Their license identifiers above come from crates.io; the "
                     "standard texts are in `LICENSES/`. Before a public release the supervisor reruns this "
                     "generator with `--sparrow-vendor` (see packaging/public-release/README.md).")
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

    def assets_section(self):
        self.out("## 4. Assets and external inputs")
        self.out()
        self.out("| Item | Location | License | Status |")
        self.out("| --- | --- | --- | --- |")
        for asset in self.components["assets"]:
            if asset["status"] in ("blocked", "needs-owner", "needs-provenance"):
                self.issue("assets", f"{asset['id']}: {asset['status']}")
            self.out(f"| {esc(asset['name'])} | {esc(', '.join(asset['paths']))} | {esc(asset['license'])} | {asset['status']} |")
        self.out()

    def platform_section(self):
        self.out("## 5. Platform libraries")
        self.out()
        self.out("The Windows package bundles libraries and data from MSYS2 UCRT64 packages; the macOS package "
                 "bundles libraries from Homebrew formulae. Their exact package versions, licenses and license "
                 "files are generated at release time from the SBOM captured on the build hosts "
                 "(`packaging/public-release/capture-sbom-msys2.sh`, `capture-sbom-homebrew.sh`). The Windows "
                 "package also carries the MSYS2 license directory as `share/licenses/ucrt64`.")
        self.out()
        if not self.args.sbom:
            self.issue("platform", "platform libraries: generated at release time from the SBOM (no --sbom given)")
            self.out("**Platform libraries: generated at release time from the SBOM.** No SBOM was supplied "
                     "to this generation run.")
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
    def record_texts(self, key, expression, documents, authors, repository=""):
        label = f"{key[0]} {key[1]}"
        if not documents:
            self.text_groups.setdefault(("none", expression), []).append((label, repository, [], authors))
            return
        for relative, text in documents:
            rights, normalized = split_copyright(text)
            digest = hashlib.sha256(normalized.encode()).hexdigest()
            group = self.text_groups.setdefault(("text", digest), [])
            group.append((label, relative, rights, text))

    def texts_section(self):
        self.out("## 6. License texts used by the Rust crates")
        self.out()
        self.out("Texts that differ only in their copyright lines are printed once; each crate's own "
                 "copyright lines are listed above the text.")
        self.out()
        number = 0
        for (kind, ident), members in sorted(self.text_groups.items(), key=lambda kv: (kv[0][0], kv[1][0][0])):
            if kind == "none":
                continue
            number += 1
            label, relative, rights, text = members[0]
            title = next((line.strip() for line in text.splitlines()
                          if line.strip() and not COPYRIGHT_LINE.match(line)), "License text")
            self.out(f"### 6.{number}. {esc(title.lstrip('# '))[:80]}")
            self.out()
            self.out("Used by:")
            self.out()
            for member_label, member_path, member_rights, _ in members:
                suffix = ("; " + "; ".join(member_rights)) if member_rights else ""
                self.out(f"- {member_label} (`{member_path}`){esc(suffix)}")
            self.out()
            fence = "````" if "```" in text else "```"
            self.out(fence + "text")
            self.out(text.rstrip("\n"))
            self.out(fence)
            self.out()
        none = [(ident, members) for (kind, ident), members in self.text_groups.items() if kind == "none"]
        if none:
            self.out("### Crates without license files in their package")
            self.out()
            self.out("These crates do not ship a license file. The standard text of each license named in their "
                     "license expression is in `LICENSES/`. Copyright is held by the crate authors listed:")
            self.out()
            for expression, members in sorted(none):
                for label, repository, _, authors in members:
                    who = ", ".join(authors) if authors else "the crate's contributors (no authors field)"
                    where = f"; project: {repository}" if repository else ""
                    self.out(f"- {label} ({esc(expression)}): {esc(who)}{esc(where)}")
            self.out()

    def status_section(self):
        self.out("## 7. Status of this file")
        self.out()
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
    parser.add_argument("--sparrow-vendor")
    parser.add_argument("--sbom", action="append", default=[])
    args = parser.parse_args()
    root = Path(args.root).resolve()
    output = Path(args.output) if args.output else root / OUTPUT
    status_path = Path(args.status) if args.status else root / STATUS

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
