#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Pack, stage, and verify the hash-pinned M3/M4 workspace evidence."""
import argparse
import hashlib
import json
import ntpath
from pathlib import Path, PurePosixPath
import sys
import tarfile


MANIFEST = Path(__file__).with_name('vacards-cli-m3-evidence-manifest.json')


def sha256(path):
    digest = hashlib.sha256()
    with Path(path).open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            digest.update(block)
    return digest.hexdigest()


def load_manifest():
    data = json.loads(MANIFEST.read_text(encoding='utf-8'))
    if data.get('schema') != 'vacards-m3-evidence/1' or not isinstance(data.get('files'), list):
        raise ValueError('unsupported M3 evidence manifest')
    paths = [entry.get('path') for entry in data['files']]
    if paths != sorted(paths) or len(paths) != len(set(paths)):
        raise ValueError('manifest paths must be unique and sorted')
    for entry in data['files']:
        if not isinstance(entry.get('sha256'), str) or len(entry['sha256']) != 64:
            raise ValueError('invalid SHA-256 for ' + str(entry.get('path')))
        if not isinstance(entry.get('bytes'), int) or entry['bytes'] < 0:
            raise ValueError('invalid byte count for ' + str(entry.get('path')))
        safe_name(entry['path'])
    return data


def safe_name(name):
    if (not isinstance(name, str) or not name or name in ('.', '..') or '\\' in name
            or ntpath.isabs(name) or ntpath.splitdrive(name)[0]):
        raise ValueError('unsafe archive name: ' + repr(name))
    path = PurePosixPath(name)
    if path.is_absolute() or any(part in ('..', '.') for part in path.parts) or str(path) != name:
        raise ValueError('unsafe archive name: ' + repr(name))
    return path


def check_file(path, entry):
    path = Path(path)
    try:
        if path.is_symlink() or not path.is_file():
            return 'missing'
        if path.stat().st_size != entry['bytes'] or sha256(path) != entry['sha256']:
            return 'mismatched'
    except OSError:
        return 'missing'
    return None


def verify_workspace(workspace, manifest=None):
    manifest = manifest or load_manifest()
    workspace = Path(workspace)
    missing, mismatched = [], []
    for entry in manifest['files']:
        state = check_file(workspace / safe_name(entry['path']), entry)
        if state == 'missing':
            missing.append(entry['path'])
        elif state == 'mismatched':
            mismatched.append(entry['path'])
    return {'passed': not missing and not mismatched, 'missing': missing, 'mismatched': mismatched}


def pack(workspace, output, manifest):
    workspace, output = Path(workspace), Path(output)
    failures = verify_workspace(workspace, manifest)
    if not failures['passed']:
        raise ValueError('workspace evidence verification failed: ' + json.dumps(failures, sort_keys=True))
    output.parent.mkdir(parents=True, exist_ok=True)
    with tarfile.open(output, 'w', format=tarfile.USTAR_FORMAT) as archive:
        for entry in manifest['files']:
            name = entry['path']
            path = workspace / safe_name(name)
            info = tarfile.TarInfo(name)
            info.size = entry['bytes']
            info.mtime = 0
            info.uid = info.gid = 0
            info.uname = info.gname = ''
            info.mode = 0o644
            info.type = tarfile.REGTYPE
            with path.open('rb') as source:
                archive.addfile(info, source)
    print(sha256(output))


def stage(bundle, workspace, manifest):
    bundle, workspace = Path(bundle), Path(workspace)
    entries = {entry['path']: entry for entry in manifest['files']}
    members = {}
    with tarfile.open(bundle, 'r:') as archive:
        for member in archive.getmembers():
            safe_name(member.name)
            if member.name not in entries:
                raise ValueError('unmanifested bundle member: ' + member.name)
            if member.name in members:
                raise ValueError('duplicate bundle member: ' + member.name)
            if not member.isreg():
                raise ValueError('non-regular bundle member: ' + member.name)
            if member.size != entries[member.name]['bytes']:
                raise ValueError('bundle size differs from manifest: ' + member.name)
            members[member.name] = member
        omitted = sorted(set(entries) - set(members))
        if omitted:
            raise ValueError('bundle omits manifest members: ' + ', '.join(omitted))
        # Check the complete archive payload before changing the workspace.
        for name, member in members.items():
            source = archive.extractfile(member)
            if source is None:
                raise ValueError('cannot read bundle member: ' + name)
            digest = hashlib.sha256()
            for block in iter(lambda: source.read(1024 * 1024), b''):
                digest.update(block)
            if digest.hexdigest() != entries[name]['sha256']:
                raise ValueError('bundle hash differs from manifest: ' + name)
        # Validate every destination before writing any member.
        for name, member in members.items():
            target = workspace / safe_name(name)
            state = check_file(target, entries[name])
            if state == 'mismatched':
                raise ValueError('refusing to overwrite differing file: ' + name)
            if state == 'missing':
                parent = workspace
                for part in safe_name(name).parts[:-1]:
                    parent = parent / part
                    if parent.is_symlink():
                        raise ValueError('refusing symlink destination directory: ' + str(parent))
                    if parent.exists() and not parent.is_dir():
                        raise ValueError('destination parent is not a directory: ' + str(parent))
        for name, member in members.items():
            target = workspace / safe_name(name)
            if check_file(target, entries[name]) is None:
                continue
            target.parent.mkdir(parents=True, exist_ok=True)
            source = archive.extractfile(member)
            if source is None:
                raise ValueError('cannot read bundle member: ' + name)
            try:
                with target.open('xb') as output:
                    while True:
                        block = source.read(1024 * 1024)
                        if not block:
                            break
                        output.write(block)
            except FileExistsError:
                raise ValueError('refusing to overwrite existing file: ' + name)
            if check_file(target, entries[name]) is not None:
                raise ValueError('staged file failed verification: ' + name)
    result = verify_workspace(workspace, manifest)
    if not result['passed']:
        raise ValueError('staged workspace incomplete: ' + json.dumps(result, sort_keys=True))
    print('verified %d files' % len(manifest['files']))


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest='command', required=True)
    p = commands.add_parser('pack'); p.add_argument('--workspace', required=True, type=Path); p.add_argument('--out', required=True, type=Path)
    p = commands.add_parser('stage'); p.add_argument('--bundle', required=True, type=Path); p.add_argument('--workspace', required=True, type=Path)
    p = commands.add_parser('verify'); p.add_argument('--workspace', required=True, type=Path); p.add_argument('--json', type=Path)
    args = parser.parse_args(argv)
    try:
        manifest = load_manifest()
        if args.command == 'pack':
            pack(args.workspace, args.out, manifest)
            return 0
        if args.command == 'stage':
            stage(args.bundle, args.workspace, manifest)
            return 0
        result = verify_workspace(args.workspace, manifest)
        if args.json:
            args.json.parent.mkdir(parents=True, exist_ok=True)
            args.json.write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
        print(json.dumps(result, sort_keys=True))
        return 0 if result['passed'] else 1
    except (OSError, ValueError, tarfile.TarError, json.JSONDecodeError) as error:
        print('M3 evidence error: ' + str(error), file=sys.stderr)
        return 1


if __name__ == '__main__':
    sys.exit(main())
