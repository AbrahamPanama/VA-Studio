#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Build the complete corresponding source archive of one VA Studio release.

usage: make_source_archive.py (--public-repo DIR | --dev-repo DIR) --commit REV
                              --output FILE.tar.xz --downloads DIR [--fetch]
                              --icc FILE|none --hyphenation DIR|none

The archive (deterministic: sorted entries, fixed times and owners) contains

  source/     the source tree of the release commit. With --public-repo it is a
              `git archive` of a public-repository commit, which is already
              self-contained. With --dev-repo the development commit is exported
              with public_tree.py (flattened submodules, libcdr fork, exclusions,
              verification).
  upstream/   the upstream release archives the build scripts download, each
              verified against the SHA-256 pinned in the tree: Cairo, GTK,
              librevenge and the MSYS2 mingw-w64-gtk4 recipe files.
  inputs/     the external build inputs given as parameters: the TIFF ICC
              profile (SHA-256 checked against VACARDS-DEPENDENCIES.env) and the
              hyphenation dictionaries. "none" records that an input is
              deliberately not included (for example, no redistribution right).
  SOURCE-ARCHIVE.json, SHA256SUMS

It fails closed when any part is missing: an unflattened gitlink, the libcdr
fork or its manifest, an incomplete Cargo vendor tree, a packaging patch or
script, an upstream archive with the wrong SHA-256, or an omitted input
parameter. --fetch downloads missing upstream archives into --downloads (HTTPS
only, SHA-256 verified); without it nothing is downloaded.
"""

import argparse
import hashlib
import json
import lzma
import os
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile
import tomllib
import urllib.request
from pathlib import Path

HERE = Path(__file__).resolve().parent
REQUIRED_INPUTS = [
    "VACARDS-DEPENDENCIES.env",
    "packaging/vacards/VERSION.env",
    "packaging/macos/vacards/cairo-1.18.4-clip-all.patch",
    "packaging/macos/vacards/gtk-4.22.4-macos-unmapped-freeze.patch",
    "packaging/macos/vacards/gtk-4.22.4-icon-cache-dispose.patch",
    "packaging/macos/vacards/gtk-4.22.4-macos-deferred-file-menu.patch",
    "packaging/macos/vacards/gtk-4.22.4-recent-groups.patch",
    "packaging/windows/vacards/gtk-4.22.4-win32-cairo-buffer.patch",
    "packaging/dependencies/gtk-4.22.4-win32-cairo-buffer/VACARDS-GTK-BUNDLE.env",
    "packaging/dependencies/librevenge-0.0.6/vacards-librevenge-0.0.6.patch",
    "packaging/dependencies/librevenge-0.0.6/VACARDS-LIBREVENGE-BUNDLE.env",
    "packaging/macos/vacards/build-patched-cairo.sh",
    "packaging/macos/vacards/build-patched-gtk.sh",
    "packaging/macos/vacards/build-patched-librevenge.sh",
    "packaging/macos/vacards/build-vacards-libcdr.sh",
    "packaging/windows/vacards/build-patched-cairo.sh",
    "packaging/windows/vacards/build-patched-gtk.sh",
    "packaging/windows/vacards/build-patched-librevenge.sh",
    "packaging/windows/vacards/build-vacards-libcdr.sh",
    "packaging/windows/vacards/build-sparrow.sh",
    "src/3rdparty/vacards-nesting-rs/Cargo.lock",
    "src/3rdparty/vacards-nesting-rs/rust-toolchain.toml",
    "src/3rdparty/sparrow/windows-x64.Cargo.lock",
    "third_party/VACARDS-LIBCDR-SOURCE.env",
    "third_party/libcdr-vacards.sha256",
]


class ArchiveError(Exception):
    pass


def sha256_file(path):
    digest = hashlib.sha256()
    with open(path, "rb") as stream:
        for chunk in iter(lambda: stream.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def read_env(path):
    values = {}
    for raw in Path(path).read_text("utf-8").splitlines():
        line = raw.strip()
        if line and not line.startswith("#") and "=" in line:
            key, value = line.split("=", 1)
            values[key.strip()] = value.strip()
    return values


def git(repo, *args):
    result = subprocess.run(["git", "-C", str(repo), *args], capture_output=True, text=True)
    if result.returncode != 0:
        raise ArchiveError(f"git {' '.join(args)}: {result.stderr.strip()}")
    return result.stdout


def stage_public(repo, commit, destination):
    if any(line.startswith("160000") for line in git(repo, "ls-tree", "-r", commit).splitlines()):
        raise ArchiveError("the public commit still has gitlinks; export it with make-public-tree.sh first")
    destination.mkdir(parents=True)
    process = subprocess.Popen(["git", "-C", str(repo), "archive", "--format=tar", commit], stdout=subprocess.PIPE)
    with tarfile.open(fileobj=process.stdout, mode="r|") as archive:
        archive.extractall(destination, filter="data")
    if process.wait() != 0:
        raise ArchiveError(f"git archive {commit} failed")


def stage_dev(repo, commit, destination):
    result = subprocess.run([sys.executable, "-B", str(HERE / "public_tree.py"), "--repo", str(repo),
                             "--commit", commit, "--output", str(destination),
                             "--report", str(destination.parent.parent / "export-report.json")])
    if result.returncode != 0:
        raise ArchiveError("public_tree.py export failed")


def check_tree(tree):
    missing = [p for p in REQUIRED_INPUTS if not (tree / p).is_file()]
    if missing:
        raise ArchiveError("missing build inputs: " + ", ".join(missing))
    deps = read_env(tree / "VACARDS-DEPENDENCIES.env")
    record = read_env(tree / "third_party/VACARDS-LIBCDR-SOURCE.env")
    if record.get("source_commit") != deps.get("libcdr_commit"):
        raise ArchiveError("third_party/libcdr-vacards is not the pinned libcdr commit")
    libcdr = tree / "third_party/libcdr-vacards"
    for line in (tree / "third_party/libcdr-vacards.sha256").read_text("utf-8").splitlines():
        digest, relative = line.split("  ", 1)
        if not (libcdr / relative).is_file() or sha256_file(libcdr / relative) != digest:
            raise ArchiveError(f"libcdr source differs from its manifest: {relative}")
    nesting = tree / "src/3rdparty/vacards-nesting-rs"
    vendored = set()
    for manifest in (nesting / "vendor").glob("*/Cargo.toml"):
        package = tomllib.loads(manifest.read_text("utf-8")).get("package", {})
        vendored.add((package.get("name"), str(package.get("version"))))
    lock = tomllib.loads((nesting / "Cargo.lock").read_text("utf-8"))
    absent = [f"{p['name']} {p['version']}" for p in lock.get("package", [])
              if p.get("source") and (p["name"], p["version"]) not in vendored]
    if absent:
        raise ArchiveError("crates in Cargo.lock without vendored source: " + ", ".join(absent))
    for gitlink_dir in ("src/3rdparty/2geom", "src/3rdparty/libcroco", "src/3rdparty/capypdf", "po",
                        "share/extensions", "share/themes", "src/3rdparty/libdepixelize", "src/3rdparty/libuemf"):
        if not any((tree / gitlink_dir).iterdir()):
            raise ArchiveError(f"flattened module is empty: {gitlink_dir}")
    return deps


def upstream_archives(tree, deps):
    gtk = read_env(tree / "packaging/dependencies/gtk-4.22.4-win32-cairo-buffer/VACARDS-GTK-BUNDLE.env")
    librevenge = read_env(tree / "packaging/dependencies/librevenge-0.0.6/VACARDS-LIBREVENGE-BUNDLE.env")
    script = (tree / "packaging/windows/vacards/build-patched-gtk.sh").read_text("utf-8")

    def pinned(name):
        match = re.search(rf"^{name}=([0-9a-f]+)$", script, re.M)
        if not match:
            raise ArchiveError(f"build-patched-gtk.sh has no {name}")
        return match.group(1)

    cairo = deps["cairo_release"]
    gtk_version = gtk["gtk_version"]
    msys2 = pinned("msys2_commit")
    recipe = f"https://raw.githubusercontent.com/msys2/MINGW-packages/{msys2}/mingw-w64-gtk4"
    release = librevenge["release"]
    return [
        (f"cairo-{cairo}.tar.xz", deps["cairo_release_sha256"],
         [f"https://cairographics.org/releases/cairo-{cairo}.tar.xz"]),
        (f"gtk-{gtk_version}.tar.xz", gtk["source_archive_sha256"],
         [f"https://download.gnome.org/sources/gtk/{gtk_version.rsplit('.', 1)[0]}/gtk-{gtk_version}.tar.xz"]),
        (librevenge["archive_filename"], librevenge["archive_sha256"],
         [f"https://sourceforge.net/projects/libwpd/files/librevenge/librevenge-{release}/"
          f"{librevenge['archive_filename']}/download",
          f"https://dev-www.libreoffice.org/src/{librevenge['archive_filename']}"]),
        ("msys2-mingw-w64-gtk4/PKGBUILD", pinned("pkgbuild_sha"), [f"{recipe}/PKGBUILD"]),
        ("msys2-mingw-w64-gtk4/001-fix-font-rendering.patch", pinned("p001_sha"),
         [f"{recipe}/001-fix-font-rendering.patch"]),
        ("msys2-mingw-w64-gtk4/003-default-dcomp-off.patch", pinned("p003_sha"),
         [f"{recipe}/003-default-dcomp-off.patch"]),
    ]


def obtain(name, digest, urls, downloads, fetch):
    path = downloads / name
    if not path.is_file() and fetch:
        path.parent.mkdir(parents=True, exist_ok=True)
        for url in urls:
            if not url.startswith("https://"):
                continue
            try:
                request = urllib.request.Request(url, headers={"User-Agent": "va-studio-source-archive/1"})
                with urllib.request.urlopen(request, timeout=300) as response, open(path.with_suffix(".part"), "wb") as out:
                    shutil.copyfileobj(response, out)
            except OSError as error:
                print(f"  {url}: {error}", file=sys.stderr)
                continue
            if sha256_file(path.with_suffix(".part")) == digest:
                path.with_suffix(".part").rename(path)
                break
            path.with_suffix(".part").unlink()
            print(f"  {url}: SHA-256 mismatch", file=sys.stderr)
    if not path.is_file():
        raise ArchiveError(f"missing upstream archive {name} (expected SHA-256 {digest}); put it in "
                           f"{downloads} or use --fetch. Sources: {', '.join(urls)}")
    if sha256_file(path) != digest:
        raise ArchiveError(f"{path}: SHA-256 does not match the pinned {digest}")
    return path


def deterministic_tar(stage, output, top, mtime):
    entries = sorted(p for p in stage.rglob("*"))
    with lzma.open(output, "wb", preset=6) as compressed, \
            tarfile.open(fileobj=compressed, mode="w", format=tarfile.PAX_FORMAT) as archive:
        for path in [stage] + entries:
            relative = path.relative_to(stage).as_posix()
            name = top if relative == "." else f"{top}/{relative}"
            info = archive.gettarinfo(str(path), arcname=name)
            info.mtime, info.uid, info.gid, info.uname, info.gname = mtime, 0, 0, "", ""
            if info.isdir():
                info.mode = 0o755
            elif info.isreg():
                info.mode = 0o755 if os.access(path, os.X_OK) else 0o644
            if info.isreg():
                with open(path, "rb") as stream:
                    archive.addfile(info, stream)
            else:
                archive.addfile(info)


def main():
    parser = argparse.ArgumentParser(description="Build the complete source archive of a VA Studio release.")
    origin = parser.add_mutually_exclusive_group(required=True)
    origin.add_argument("--public-repo")
    origin.add_argument("--dev-repo")
    parser.add_argument("--commit", required=True)
    parser.add_argument("--output", required=True)
    parser.add_argument("--downloads", required=True)
    parser.add_argument("--fetch", action="store_true")
    parser.add_argument("--icc", required=True, help="the TIFF ICC profile file, or 'none'")
    parser.add_argument("--hyphenation", required=True, help="the hyphenation dictionary directory, or 'none'")
    args = parser.parse_args()
    output = Path(args.output).resolve()
    if output.exists():
        raise SystemExit(f"output exists: {output}")
    repo = Path(args.public_repo or args.dev_repo).resolve()
    commit = git(repo, "rev-parse", "--verify", f"{args.commit}^{{commit}}").strip()
    mtime = int(git(repo, "show", "-s", "--format=%ct", commit).strip())
    top = f"va-studio-source-{commit[:12]}"
    with tempfile.TemporaryDirectory(dir=output.parent) as work_dir:
        work = Path(work_dir)
        stage = work / top
        stage.mkdir()
        try:
            (stage_public if args.public_repo else stage_dev)(repo, commit, stage / "source")
            deps = check_tree(stage / "source")
            upstream = []
            for name, digest, urls in upstream_archives(stage / "source", deps):
                found = obtain(name, digest, urls, Path(args.downloads).resolve(), args.fetch)
                target = stage / "upstream" / name
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(found, target)
                upstream.append({"file": f"upstream/{name}", "sha256": digest, "sources": urls})
            inputs = {}
            if args.icc == "none":
                inputs["tiff_icc_profile"] = "not included (owner decision; see VA_ICC_PROFILE_STATUS)"
            else:
                icc = Path(args.icc)
                if not icc.is_file() or sha256_file(icc) != deps["tiff_rgb_profile_sha256"]:
                    raise ArchiveError("the ICC profile is missing or differs from tiff_rgb_profile_sha256")
                target = stage / "inputs/icc" / deps["tiff_rgb_profile_filename"]
                target.parent.mkdir(parents=True)
                shutil.copy2(icc, target)
                inputs["tiff_icc_profile"] = f"inputs/icc/{deps['tiff_rgb_profile_filename']}"
            if args.hyphenation == "none":
                inputs["hyphenation"] = "not included"
            else:
                source_dir = Path(args.hyphenation)
                files = sorted(p for p in source_dir.iterdir() if p.is_file()) if source_dir.is_dir() else []
                if not files:
                    raise ArchiveError("the hyphenation directory is missing or empty")
                for path in files:
                    target = stage / "inputs/hyphenation" / path.name
                    target.parent.mkdir(parents=True, exist_ok=True)
                    shutil.copy2(path, target)
                inputs["hyphenation"] = [f"inputs/hyphenation/{p.name}" for p in files]
        except ArchiveError as error:
            raise SystemExit(f"make-source-archive: {error}")
        record = {"schema": "va-studio-source-archive/1", "commit": commit,
                  "origin": "public repository" if args.public_repo else "development repository export",
                  "upstream": upstream, "inputs": inputs}
        (stage / "SOURCE-ARCHIVE.json").write_text(json.dumps(record, indent=2) + "\n", encoding="utf-8")
        sums = [f"{sha256_file(p)}  {p.relative_to(stage).as_posix()}\n"
                for p in sorted(stage.rglob("*")) if p.is_file() and not p.is_symlink()]
        (stage / "SHA256SUMS").write_text("".join(sums), encoding="utf-8")
        partial = output.with_name(output.name + ".partial")
        deterministic_tar(stage, partial, top, mtime)
        partial.rename(output)
    print(f"wrote {output} ({output.stat().st_size} bytes, SHA-256 {sha256_file(output)})")


if __name__ == "__main__":
    main()
