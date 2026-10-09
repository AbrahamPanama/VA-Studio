#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Deterministic docs from the executable's production catalog. No command registry.

Generate: --binary ABS_CLI [--family-dir EVIDENCE_DIR]
Check:   --binary ABS_CLI --check [--family-dir EVIDENCE_DIR]
--catalog is an explicitly offline snapshot mode, never live build qualification.
"""
import argparse
import importlib.util
import json
import re
import subprocess
import sys
import tempfile
from pathlib import Path

sys.dont_write_bytecode = True
SOURCE = Path(__file__).resolve().parents[1]
DOC = SOURCE / 'doc/vacards/cli'
CASES = SOURCE / 'testfiles/cli_tests/vacards-agent/examples'
DRAFT = 'https://json-schema.org/draft/2020-12/schema'
UNKNOWN = 'not declared by this descriptor'


def schema_module():
    spec = importlib.util.spec_from_file_location('docs_schema', Path(__file__).with_name('vacards-cli-schema.py'))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def json_text(value):
    return json.dumps(value, ensure_ascii=False, indent=2, allow_nan=False) + '\n'


def read_catalog(binary):
    binary = Path(binary).resolve(strict=True)
    with tempfile.TemporaryDirectory(prefix='vacards-docs-') as folder:
        request = Path(folder) / 'request.json'
        request.write_text(json_text(dict(schema='va-studio.cli-request/1', id='docs-catalog', command='system.catalog', params={})), encoding='utf-8')
        run = subprocess.run([str(binary), '--agent-request-file', str(request)], capture_output=True, timeout=45)
        if run.returncode:
            raise ValueError('catalog exit %d: %s' % (run.returncode, run.stderr.decode('utf-8', errors='replace')[-2000:]))
        lines = run.stdout.decode('utf-8').splitlines()
        if len(lines) != 1:
            raise ValueError('catalog must produce exactly one result')
        record = schema_module().loads(lines[0])
        if record.get('status') != 'ok' or record.get('action') != 'system.catalog':
            raise ValueError('catalog probe did not succeed')
        catalog = record['data']
        commands = validate_catalog(catalog)
        schema_module().validate(record, commands['system.catalog']['result_schema'])
        return catalog


def validate_catalog(catalog):
    if not re.fullmatch('[a-f0-9]{64}', catalog.get('hash', '')):
        raise ValueError('missing catalog hash')
    commands = schema_module().catalog_schemas(catalog)
    for cid, command in commands.items():
        if not re.fullmatch('[a-z0-9]+(?:[.-][a-z0-9]+)*', cid):
            raise ValueError('unsafe command ID: ' + cid)
        if command['example']['command'] != cid:
            raise ValueError('example command mismatch: ' + cid)
        if not command.get('summary'):
            raise ValueError('missing purpose: ' + cid)
    return commands


def cell(value):
    if isinstance(value, (dict, list, bool)) or value is None:
        value = json.dumps(value, ensure_ascii=False, separators=(',', ':'))
    return str(value).replace('|', '&#124;').replace('\n', ' ')


def fields(schema, path='$', required=False):
    """Show nested properties and alternative routes without flattening constraints."""
    info = {k: v for k, v in schema.items() if k not in ('properties', 'items', 'oneOf', 'anyOf', 'allOf', 'x-m3-contract', '$schema')}
    yield [path, required, info.get('type', 'schema-defined'), info.get('x-unit', UNKNOWN), info.get('default', UNKNOWN), info.get('description', '') or UNKNOWN, info]
    for key, child in sorted(schema.get('properties', {}).items()):
        yield from fields(child, path + '.' + key, key in schema.get('required', []))
    if isinstance(schema.get('items'), dict):
        yield from fields(schema['items'], path + '[]')
    for kind in ('oneOf', 'anyOf', 'allOf'):
        for i, branch in enumerate(schema.get(kind, [])):
            yield from fields(branch, path + '/' + kind + '/' + str(i))


def table(headers, rows):
    return '| ' + ' | '.join(headers) + ' |\n| ' + ' | '.join('---' for _ in headers) + ' |\n' + ''.join('| ' + ' | '.join(cell(v) for v in row) + ' |\n' for row in rows) + '\n'


def fenced(value):
    return '```json\n' + json_text(value) + '```\n\n'


def data_schemas(schema):
    if 'data' in schema.get('properties', {}):
        yield schema['properties']['data']
    for kind in ('anyOf', 'oneOf', 'allOf'):
        for branch in schema.get(kind, []):
            yield from data_schemas(branch)


def command_page(command):
    cid = command['id']
    meta = command.get('contract', {})
    text = '### ' + cid + '\n\n' + command['summary'] + '\n\n'
    policies = [(key, command.get(key, UNKNOWN)) for key in ('version', 'available', 'accepted', 'experimental', 'schema_status', 'effects', 'target_policy', 'undo_policy', 'dry_run_grade', 'cancellation_boundary')]
    policies += [(key, meta.get(key, command.get(key, UNKNOWN))) for key in ('selection_mode', 'guard_domain', 'guard_required', 'needs_document', 'target_cardinality', 'normalization', 'partial_policy', 'role_order')]
    text += table(['Policy', 'Value'], policies)
    text += 'Parameters and units (branch paths retain alternatives; required is local to its parent):\n\n'
    text += table(['Field / branch', 'Required', 'Type', 'Unit', 'Default', 'Description', 'Constraints'], fields(command['request_schema']['properties']['params'], 'params'))
    text += 'Result data:\n\n'
    data = [command['result_data']] if 'result_data' in command else list(data_schemas(command['result_schema']))
    if not data:
        text += UNKNOWN + '. See the result schema below.\n\n'
    for i, schema in enumerate(data):
        text += table(['Field / branch', 'Required', 'Type', 'Unit', 'Default', 'Description', 'Constraints'], fields(schema, 'data/' + str(i)))
    text += 'Error codes:\n\n'
    rows = meta.get('error_rows', [])
    text += table(['Code', 'Retryable', 'Mutation', 'Layer / route', 'Reason'], [[r['code'], r['retryable'], r['mutation_state'], r['layer'] + ' / ' + r['route'], r.get('detail.reason', r.get('native_branch', UNKNOWN))] for r in rows] + [[code, UNKNOWN, UNKNOWN, UNKNOWN, UNKNOWN] for code in command.get('error_codes', []) if code not in {r['code'] for r in rows}])
    text += 'Warnings / constraints:\n\n' + fenced({k: command.get(k, meta.get(k, UNKNOWN)) for k in ('warning_codes', 'warnings', 'constraints', 'limits', 'transform_policy', 'normalization_order', 'success_rows')})
    text += 'Executable descriptor example (literal IDs and paths are illustrative; bind fresh context before stateful use):\n\n' + fenced(command['example'])
    text += '<details><summary>Exact request and result schemas</summary>\n\n' + fenced({'request': command['request_schema'], 'result': command['result_schema']}) + '</details>\n\n'
    return text


PY_SESSION = '''#!/usr/bin/env python3
"""Run with an absolute selected CLI and an existing private output directory."""
import copy, json, pathlib, subprocess, sys, uuid
cli, work = pathlib.Path(sys.argv[1]).resolve(strict=True), pathlib.Path(sys.argv[2]).resolve(strict=True)
if not work.is_dir(): raise ValueError('output directory required')
p = subprocess.Popen([str(cli), '--agent-session', '--grant-write', str(work)],
                     stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True, encoding='utf-8')
def read():
    line = p.stdout.readline()
    if not line: raise RuntimeError('disconnected; publication outcome may be uncertain')
    return json.loads(line)
hello = read()
if hello.get('event') != 'hello': raise RuntimeError(hello)
def call(request):
    request['id'] = uuid.uuid4().hex
    p.stdin.write(json.dumps(request) + '\\n'); p.stdin.flush()
    while True:
        event = read()
        if event.get('event') == 'result' and event.get('id') == request['id']:
            result = event['result']; print(json.dumps(result))
            if result['status'] not in ('ok', 'changed', 'unchanged'): raise RuntimeError(result)
            return result
base = dict(schema='va-studio.cli-request/1', params={})
try:
    catalog = call(dict(base, command='system.catalog'))['data']
    if catalog['hash'] != hello['catalog_hash']: raise RuntimeError('catalog identity changed')
    commands = {c['id']: c for c in catalog['commands']}
    def invoke(cid, params=None):
        status = call(dict(base, command='session.status'))['data']
        req = copy.deepcopy(commands[cid]['example'])
        if params is not None: req['params'] = params
        doc = status.get('document')
        if 'document' in req:
            if not doc: raise RuntimeError('document required')
            req['document'] = doc['id']
        if 'if_revision' in req:
            domain = commands[cid].get('guard_domain')
            if domain is None: domain = 'document' if 'document' in req else 'session'
            req['if_revision'] = doc['revision'] if domain == 'document' else status['session_revision']
        return call(req)
    invoke('file.new')
    invoke('query.document')
    destination = work / ('sheet-' + uuid.uuid4().hex + '.svg')
    params = copy.deepcopy(commands['file.save']['example']['params'])
    params['path'] = str(destination)
    saved = invoke('file.save', params)
    if not saved.get('publication', {}).get('persisted'): raise RuntimeError('not persisted')
    invoke('session.close')
finally:
    p.stdin.close()
    try: p.wait(timeout=10)
    except subprocess.TimeoutExpired: p.terminate(); p.wait(timeout=10)
if p.returncode: raise SystemExit(p.returncode)
'''

PS_SESSION = '''param([Parameter(Mandatory=$true)][string]$Cli,
      [Parameter(Mandatory=$true)][string]$Work)
$ErrorActionPreference = 'Stop'
$Cli = (Resolve-Path -LiteralPath $Cli).Path
$Work = (Resolve-Path -LiteralPath $Work).Path
if ($Cli.Contains('"') -or $Work.Contains('"')) { throw 'Invalid quoted path' }
$psi = New-Object System.Diagnostics.ProcessStartInfo
$psi.FileName = $Cli
$psi.Arguments = '--agent-session --grant-write "' + $Work + '"'
$psi.UseShellExecute = $false
$psi.RedirectStandardInput = $true; $psi.RedirectStandardOutput = $true
$psi.StandardOutputEncoding = New-Object System.Text.UTF8Encoding($false)
$psi.StandardInputEncoding = New-Object System.Text.UTF8Encoding($false)
$p = New-Object System.Diagnostics.Process; $p.StartInfo = $psi
[void]$p.Start()
function Read-Event {
    $line = $p.StandardOutput.ReadLine()
    if ($null -eq $line) { throw 'Disconnected; publication may be uncertain' }
    return ($line | ConvertFrom-Json)
}
function Call($request) {
    $request.id = [Guid]::NewGuid().ToString('N')
    $p.StandardInput.WriteLine(($request | ConvertTo-Json -Depth 100 -Compress))
    $p.StandardInput.Flush()
    do { $event = Read-Event } until ($event.event -eq 'result' -and $event.id -eq $request.id)
    $r = $event.result
    if ($r.status -notin @('ok','changed','unchanged')) { throw ($r | ConvertTo-Json -Depth 100) }
    return $r
}
function Invoke-Example($cid, $params) {
    $status = (Call @{schema='va-studio.cli-request/1'; command='session.status'; params=@{}}).data
    $command = $commands[$cid]
    $req = $command.example | ConvertTo-Json -Depth 100 | ConvertFrom-Json
    if ($null -ne $params) { $req.params = $params }
    if ($null -ne $req.document) { $req.document = $status.document.id }
    if ($null -ne $req.if_revision) {
        $domain = $command.guard_domain
        if (!$domain) { if ($req.document) { $domain='document' } else { $domain='session' } }
        if ($domain -eq 'document') { $req.if_revision = $status.document.revision }
        else { $req.if_revision = $status.session_revision }
    }
    return (Call $req)
}
try {
    $hello = Read-Event
    if ($hello.event -ne 'hello') { throw 'Missing hello' }
    $catalog = (Call @{schema='va-studio.cli-request/1'; command='system.catalog'; params=@{}}).data
    if ($catalog.hash -ne $hello.catalog_hash) { throw 'Catalog identity changed' }
    $commands = @{}; foreach ($c in $catalog.commands) { $commands[$c.id]=$c }
    $null = Invoke-Example 'file.new' $null
    Invoke-Example 'query.document' $null | ConvertTo-Json -Depth 100
    $params = $commands['file.save'].example.params | ConvertTo-Json -Depth 100 | ConvertFrom-Json
    $params.path = Join-Path $Work ('sheet-' + [Guid]::NewGuid().ToString('N') + '.svg')
    $saved = Invoke-Example 'file.save' $params
    if (!$saved.publication.persisted) { throw 'Not persisted' }
    $null = Invoke-Example 'session.close' $null
} finally {
    $p.StandardInput.Close()
    if (!$p.WaitForExit(10000)) { $p.Kill(); $p.WaitForExit() }
}
if ($p.ExitCode -ne 0) { throw ('CLI exit ' + $p.ExitCode) }
'''


def outputs(catalog, doc=DOC, cases=CASES, family_dir=None):
    commands = validate_catalog(catalog)
    families = {}
    for cid in sorted(commands):
        families.setdefault(cid.split('.', 1)[0], []).append(commands[cid])
    banner = 'Generated from the executable catalog; do not edit.\n\nCatalog hash: `' + catalog['hash'] + '`.\n\n'
    index = '# Agent CLI reference index\n\n' + banner
    index += 'English is the primary documentation language. Experimental and undeclared acceptance fields are not release qualification.\n\n'
    index += table(['Family', 'Commands'], [['[' + family + '](#' + family + '-family)', ', '.join('`' + c['id'] + '`' for c in rows)] for family, rows in families.items()])
    pages = {family + '.md': '# ' + family + ' command family\n\n' + banner + '[Index](index.md)\n\n' + ''.join(command_page(c) for c in rows) for family, rows in families.items()}
    result = {doc / 'catalog.json': json_text(catalog), doc / 'reference.md': index + ''.join('## ' + family + ' family\n\n' + ''.join(command_page(c) for c in rows) for family, rows in families.items())}
    wrapper = {'$schema': DRAFT, 'description': 'Executable catalog hash: ' + catalog['hash']}
    for name, key in [('request', 'request_schema'), ('result', 'result_schema')]:
        # Result variants overlap between commands, so the union is anyOf.
        result[doc / 'schemas' / (name + '.json')] = json_text(dict(wrapper, anyOf=[commands[cid][key] for cid in sorted(commands)]))
    result[doc / 'schemas/commands.json'] = json_text(dict(wrapper, **{'$defs': {cid + '.' + kind: c[kind + '_schema'] for cid, c in sorted(commands.items()) for kind in ('request', 'result')}}))
    examples = {'catalog_hash': catalog['hash'], 'examples': [commands[cid]['example'] for cid in sorted(commands)]}
    result[doc / 'examples/requests.json'] = json_text(examples)
    result[cases / 'reference.json'] = json_text(examples)
    result[cases / 'workflow.json'] = json_text({'catalog_hash': catalog['hash'], 'description': 'Run the Python or PowerShell session example. Bind fresh context; independent P9 outcome validation is pending.', 'python': 'doc/vacards/cli/examples/' + 'session.py', 'powershell': 'doc/vacards/cli/examples/' + 'session.ps1'})
    result[doc / 'examples/session.py'] = PY_SESSION
    result[doc / 'examples/session.ps1'] = PS_SESSION
    template = (doc / 'AGENT_GUIDE.template.md').read_text(encoding='utf-8')
    result[doc / 'AGENT_GUIDE.md'] = template.replace('{{CATALOG_HASH}}', catalog['hash']).replace('{{COMMAND_COUNT}}', str(len(commands))).replace('{{PYTHON_SESSION}}', PY_SESSION.rstrip())
    if not 120 <= len(result[doc / 'AGENT_GUIDE.md'].splitlines()) <= 150:
        raise ValueError('guide must contain 120–150 lines')
    if family_dir:
        family_dir = Path(family_dir)
        result.update({family_dir / name: page for name, page in pages.items()})
        result[family_dir / 'index.md'] = index.replace('](#', '](').replace('-family)', '.md)')
    return result


def drift(expected):
    return [str(path) for path, text in expected.items() if not path.is_file() or path.read_bytes() != text.encode('utf-8')]


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument('--binary', type=Path)
    group.add_argument('--catalog', type=Path)
    parser.add_argument('--check', action='store_true')
    parser.add_argument('--family-dir', type=Path)
    args = parser.parse_args(argv)
    try:
        catalog = read_catalog(args.binary) if args.binary else schema_module().loads(args.catalog.read_text(encoding='utf-8'))
        expected = outputs(catalog, family_dir=args.family_dir)
        if args.check:
            changed = drift(expected)
            if changed:
                print('FAIL: generated docs drift:\n' + '\n'.join(changed), file=sys.stderr)
                return 1
        else:
            for path, text in expected.items():
                path.parent.mkdir(parents=True, exist_ok=True)
                # Complete each replacement before any peer can read it.
                with tempfile.NamedTemporaryFile(dir=path.parent, delete=False) as stream:
                    temporary = Path(stream.name)
                    stream.write(text.encode('utf-8'))
                temporary.chmod(0o644)
                temporary.replace(path)
        print('PASS: %d commands; %d generated files; catalog %s%s' % (len(catalog['commands']), len(expected), catalog['hash'], ' (offline snapshot)' if args.catalog else ' (live executable)'))
        return 0
    except (ValueError, KeyError, OSError, subprocess.SubprocessError) as error:
        print('FAIL: ' + str(error), file=sys.stderr)
        return 1


if __name__ == '__main__':
    raise SystemExit(main())
