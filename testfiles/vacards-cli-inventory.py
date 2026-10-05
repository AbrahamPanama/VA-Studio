#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""M1 catalog-derived case matrix. Missing executable is the only skip (77)."""
import argparse
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

CELLS = ('success', 'invalid', 'no-op', 'refusal', 'dry-run', 'undo', 'group', 'save', 'cancel')
M1 = ('system.catalog', 'system.options', 'session.status', 'session.cancel', 'session.release', 'session.close',
      'query.document', 'query.pages', 'query.layers', 'query.objects', 'query.geometry', 'query.styles',
      'query.images', 'query.fonts', 'query.selection')
M2 = ('file.new', 'file.open', 'file.close', 'file.import', 'file.save', 'file.export')

# Reviewed legacy canonical commands must survive catalog changes independently
# of the current catalog iteration and the coverage ledger.
LEGACY = ('geometry.boolean', 'geometry.corners', 'geometry.offset', 'geometry.resize',
          'bitmap.histogram', 'bitmap.tone-query', 'history.undo', 'history.redo')


def schema_module():
    sys.dont_write_bytecode = True
    spec = importlib.util.spec_from_file_location('inventory_schema', Path(__file__).with_name('vacards-cli-schema.py'))
    m = importlib.util.module_from_spec(spec); spec.loader.exec_module(m); return m


def read_catalog(binary):
    schema = schema_module()
    with tempfile.TemporaryDirectory(prefix='p9-inventory-') as temp:
        path = Path(temp) / 'request.json'
        path.write_text(json.dumps({'schema': 'va-studio.cli-request/1', 'id': 'inventory', 'command': 'system.catalog', 'params': {}}))
        p = subprocess.run([str(binary), '--agent-request-file', str(path)], capture_output=True, timeout=30)
        if p.returncode != 0: raise ValueError('catalog exit %d: %s' % (p.returncode, p.stderr.decode('utf-8', errors='replace')[-1000:]))
        lines = p.stdout.decode('utf-8').splitlines()
        if len(lines) != 1: raise ValueError('catalog stdout is not exactly one JSON result')
        record = schema.loads(lines[0]); catalog = record['data']
        commands = schema.catalog_schemas(catalog)
        schema.validate(record, commands['system.catalog']['result_schema'])
        if record.get('status') != 'ok': raise ValueError('catalog not successful')
        destination = os.environ.get('VACARDS_CLI_CATALOG_JSON')
        if destination: Path(destination).write_text(json.dumps(catalog, indent=2) + '\n')
        return catalog


def applicability(command):
    effects, policy = command['effects'], command['target_policy']
    if effects not in ('read-only', 'session-state', 'document-edit', 'file-publication'):
        raise ValueError('unreviewed effects: ' + effects)
    if policy not in ('Q', 'S', 'G', 'M', 'C', 'P', 'I'): raise ValueError('unreviewed policy: ' + policy)
    params = command['request_schema']['properties']['params'].get('properties', {})
    return {
        'success': None, 'invalid': None, 'no-op': None, 'refusal': None,
        'dry-run': None if effects != 'read-only' else 'read-only command has no state-changing dry-run',
        'undo': None if effects == 'document-edit' else 'session state and queries do not add document Undo steps',
        'group': None if policy in ('G', 'M', 'C', 'P') or 'ids' in params else 'policy %s has no group/multi-selection targets' % policy,
        'save': None if effects in ('document-edit', 'file-publication') else 'no persisted output class in M1',
        'cancel': None if command.get('cancellation_boundary') not in ('none', 'before-handler') else 'no cooperative long operation; before-handler cancellation belongs to transport tests',
    }


def parameter_enums(schema, path='params'):
    """Only advertised input enums, not result-status or command-alias enums."""
    out = []
    if 'enum' in schema: out.append((path, schema))
    for key, child in schema.get('properties', {}).items(): out.extend(parameter_enums(child, path + '.' + key))
    if 'items' in schema: out.extend(parameter_enums(schema['items'], path + '.[]'))
    for key in ('anyOf', 'oneOf', 'allOf'):
        for child in schema.get(key, []): out.extend(parameter_enums(child, path))
    for key in ('if', 'then', 'else'):
        if key in schema: out.extend(parameter_enums(schema[key], path))
    return out


def inventory_refs(value):
    """Schema-only representatives; runtime harness resolves actual guard identities."""
    if isinstance(value, dict) and set(value) == {'$ref'}:
        path = value['$ref']
        if 'version' in path: return {'identity':'fixture-identity','sha256':'0'*64,'bytes':0}
        return 0 if 'revision' in path else 'fixture-identity'
    if isinstance(value, dict): return {k: inventory_refs(v) for k,v in value.items()}
    if isinstance(value, list): return [inventory_refs(v) for v in value]
    return value


def error_applicability(cid, code):
    load = {'read-grant-denied','resource-denied','remote-resource','invalid-file','input-too-large',
            'format-unsupported','pages-invalid','font-policy-required','stale-dependency','invalid-path',
            'missing-file','read-failed','invalid-xml','unsafe-xml','invalid-svgz','duplicate-id',
            'unsafe-reference','invalid-svg','intake-failed'}
    write = {'write-grant-denied','unsafe-destination','expected-version-required','publication-conflict',
             'publication-unsupported','publication-uncertain','publication-failed','writes-blocked'}
    if code in load and cid not in ('file.open','file.import','file.save','file.export'):
        return 'Lifecycle new/close performs no source intake'
    if code in write and cid not in ('file.save','file.export'):
        if code == 'write-grant-denied' and cid == 'file.close': return None
        return 'No output publication for this command; close reconciliation only inspects disk'
    exclusive = {'dirty-document':('file.new','file.open','file.close'), 'no-document':('file.import','file.save','file.export'),
                 'import-failed':('file.import',),'save-failed':('file.save',),'export-failed':('file.export',),
                 'invalid-target':('file.export',),'profile-invalid':('file.export',),'profile-unsupported':('file.export',),
                 'document-read-only':('file.import','file.save'),'document-busy':('file.import',),'transaction-unavailable':('file.import',)}
    if code in exclusive and cid not in exclusive[code]: return 'Error belongs to '+', '.join(exclusive[code])+'; shared catalog code vocabulary'
    legacy_only = {'slice-unavailable','cannot-open-result-file','offset-failed','no-such-node','no-corners','invalid-input',
                   'engine-limit','corner-edit-failed','empty-selection','unavailable','no-eligible-targets','requires-single-bitmap',
                   'missing-source','zero-dimension','invalid-image','bitmap-failed'}
    if code in legacy_only: return 'Inherited legacy/M3 engine error; this file command does not invoke that operation'
    return None


def enum_values(params, param_schema, path):
    parts = path.split('.')[1:]
    nodes = [(params, param_schema)]
    for part in parts:
        next_nodes = []
        for value, spec in nodes:
            if part == '[]':
                if isinstance(value, list): next_nodes.extend((item, spec.get('items', {})) for item in value)
            else:
                child = spec.get('properties', {}).get(part, {})
                if isinstance(value, dict) and part in value: next_nodes.append((value[part], child))
                elif 'default' in child: next_nodes.append((child['default'], child))
        nodes = next_nodes
    return [v for v, _ in nodes]


def matrix(catalog, testfiles):
    schema = schema_module(); commands = schema.catalog_schemas(catalog)
    root = Path(testfiles) / 'vacards-agent-session'
    review = schema.loads((root / 'inventory-na.json').read_text())
    overrides, legacy = review['na'], review['legacy_covered']
    rows, gaps = [], []
    covers, success_requests = {}, {}
    error_requests = {}; required_error_domains = []
    m2_review = {}
    if M2:
        file_root = root / 'files'
        try:
            m2_review = schema.loads((file_root / 'support/inventory.json').read_text())
            actual = sorted(p.name for p in file_root.glob('*.json'))
            if actual != m2_review['required_cases']: gaps.append('M2 case inventory differs from required ledger')
            if not actual: gaps.append('zero required M2 files cases')
            if not m2_review.get('native_domains'): gaps.append('missing M2 native/fault/platform inventory')
            contract = schema.loads((file_root / 'support/contract.json').read_text())
            for cid, frozen in contract['commands'].items():
                if cid not in commands: continue
                live = commands[cid]
                for key in ('effects','target_policy','dry_run_grade'):
                    if live.get(key) != frozen[key]: gaps.append('M2 descriptor mismatch: '+cid+'/'+key)
                params = live['request_schema']['properties']['params']
                if set(params.get('properties',{})) != set(frozen['params']): gaps.append('M2 parameter inventory mismatch: '+cid)
                if not set(frozen['required']).issubset(params.get('required',[])): gaps.append('M2 missing required parameters: '+cid)
                if params.get('additionalProperties') is not False: gaps.append('M2 request params must be closed: '+cid)
                if 'if_revision' not in live['request_schema'].get('required',[]): gaps.append('M2 revision guard not schema-required: '+cid)
                if cid in ('file.import','file.save','file.export') and 'document' not in live['request_schema'].get('required',[]): gaps.append('M2 document guard not schema-required: '+cid)
        except (OSError, ValueError, KeyError) as e: gaps.append('missing/invalid M2 case ledger: ' + str(e))
    for group in ('system', 'session', 'query', 'files'):
        for path in sorted((root / group).glob('*.json')):
            case = schema.loads(path.read_text())
            for step in case.get('requests', []):
                request = inventory_refs(step.get('request', {}))
                cid = request.get('command')
                if step.get('expect_error'): error_requests.setdefault(cid, set()).add(step['expect_error'])
                if cid in commands and not step.get('expect_error') and step.get('expect', {}).get('status', 'ok') in ('ok', 'changed', 'unchanged'):
                    try: schema.validate(request, commands[cid]['request_schema'])
                    except schema.ValidationError: continue
                    success_requests.setdefault(cid, []).append(request)
            declarations = [case] + case.get('coverage', []) + case.get('requests', [])
            for declaration in declarations:
                if 'command' not in declaration or 'cells' not in declaration: continue
                command, cells = declaration['command'], declaration['cells']
                if command not in commands:
                    if command not in M1: gaps.append('case %s declares absent command %s' % (path.name, command))
                    continue
                allowed = set(CELLS) | ({'boundary'} if command in M2 else set())
                if not isinstance(cells, list) or not cells or set(cells) - allowed:
                    gaps.append('invalid coverage declaration in ' + str(path)); continue
                # Metadata must reference an actual request in the case.
                requested = [s.get('command', s.get('request', {}).get('command')) for s in case.get('requests', [])]
                if command not in requested: gaps.append('coverage without request: ' + str(path)); continue
                if not case.get('requests'): gaps.append('zero requests in ' + str(path)); continue
                for cell in cells: covers.setdefault((command, cell), []).append(str(path.relative_to(Path(testfiles))))
    for command in M1:
        if command not in commands: gaps.append('missing M1 catalog command: ' + command)
    for command in M2:
        if command not in commands: gaps.append('missing M2 catalog command: ' + command)
    for cid in sorted(set(LEGACY) | set(legacy)):
        if cid not in commands: gaps.append('missing reviewed legacy catalog command: ' + cid)
    m3_report = m3_matrix(catalog=catalog) if any('x-m3-contract' in c['request_schema']['properties']['params'] for c in commands.values()) else None
    m3_rows = {r['command']:r for r in m3_report['commands']} if m3_report else {}
    if m3_report: gaps.extend(m3_report['gaps'])
    for cid, command in sorted(commands.items()):
        if cid in m3_rows:
            rows.append({'command':cid, 'm3_authored':m3_rows[cid]})
            continue
        if cid.startswith(('geometry.', 'bitmap.', 'history.')):
            entry = legacy.get(cid)
            if not entry or not entry.get('reason') or not entry.get('cases'):
                gaps.append('unreviewed legacy command: ' + cid); continue
            missing = [p for p in entry['cases'] if not (Path(testfiles) / p).is_file()]
            if missing: gaps.append('missing reviewed legacy cases: ' + ', '.join(missing))
            rows.append({'command': cid, 'legacy_covered': entry})
            continue
        if not cid.startswith(('system.', 'session.', 'query.')) and cid not in M2:
            gaps.append('unreviewed command outside M1: ' + cid); continue
        cells = {}
        applicable = applicability(command)
        if cid in M2:
            applicable['boundary'] = None
            applicable['dry-run'] = None
            applicable['save'] = None if cid in ('file.import','file.save','file.export') else 'lifecycle has no persisted output'
            applicable['cancel'] = None
        for cell, reason in applicable.items():
            cases = covers.get((cid, cell), [])
            override = overrides.get(cid, {}).get(cell)
            if cid in M2: override = m2_review.get('na', {}).get(cid, {}).get(cell)
            if override is not None and (not isinstance(override, str) or not override.strip()):
                gaps.append('empty N/A reason: ' + cid + '/' + cell); override = None
            if cases:
                cells[cell] = {'status': 'covered', 'cases': sorted(set(cases))}
            elif reason or override:
                cells[cell] = {'status': 'na', 'reason': override or reason}
            elif cid in M2 and cell in m2_review.get('native_cells', {}).get(cid, []):
                cells[cell] = {'status':'native-required','reason':'real service hook coverage required by gate; wire has no fault backdoor'}
            else:
                cells[cell] = {'status': 'gap'}; gaps.append(cid + '/' + cell)
        enums = []
        params = command['request_schema']['properties']['params']
        for path, spec in parameter_enums(params):
            values = [value for request in success_requests.get(cid, []) for value in enum_values(request['params'], params, path)]
            for value in spec['enum']:
                covered = any(schema.equal(value, actual) for actual in values)
                enums.append({'path': path, 'value': value, 'status': 'covered' if covered else 'gap'})
                if not covered: gaps.append(cid + '/' + path + '/enum=' + json.dumps(value))
        errors = []
        if cid in M2:
            for code in command.get('error_codes', []):
                covered = code in error_requests.get(cid, set())
                na = error_applicability(cid, code)
                # Lexical admission errors are shared by every request transport;
                # the retained executable M1 raw/fragmentation cases prove them.
                admission = code in ('repeated-key','invalid-utf8','malformed-json','nonfinite-number','request-too-large','unknown-command')
                shared = admission and any(code in codes for key,codes in error_requests.items() if key not in M2)
                row = {'code':code,'status':'covered' if covered else 'shared-M1-admission' if shared else 'na' if na else 'native-required'}
                if na and not covered and not shared: row['reason'] = na
                if row['status']=='native-required': required_error_domains.append('error:'+cid+'/'+code)
                errors.append(row)
            for step_request in success_requests.get(cid, []):
                schema.validate(step_request, command['request_schema'])
        rows.append({'command': cid, 'cells': cells, 'enums': enums, 'errors':errors})
    # Typoed N/A entries must not quietly authorize missing coverage.
    for cid, cells in overrides.items():
        if cid not in commands and cid not in M1: gaps.append('unknown N/A command: ' + cid)
        if set(cells) - set(CELLS): gaps.append('unknown N/A cell: ' + cid)
    return {'catalog_hash': catalog.get('hash'), 'rows': rows, 'gaps': gaps,
            'required_native_domains':m2_review.get('native_domains', []), 'required_error_domains':required_error_domains,
            'required_m2_cases':m2_review.get('required_cases', []), 'passed': not gaps}


def print_matrix(report):
    print('COMMAND | ' + ' | '.join(CELLS))
    for row in report['rows']:
        if 'm3_authored' in row:
            print(row['command'] + ' | M3 authored coverage; runtime/branch receipts required separately'); continue
        if 'legacy_covered' in row:
            print(row['command'] + ' | legacy-covered: ' + ', '.join(row['legacy_covered']['cases'])); continue
        print(row['command'] + ' | ' + ' | '.join(row['cells'][c]['status'] for c in CELLS))
        for enum in row.get('enums', []):
            print('  ENUM %s=%s: %s' % (enum['path'], json.dumps(enum['value']), enum['status']))
        if 'boundary' in row['cells']: print('  BOUNDARY: ' + row['cells']['boundary']['status'])
        for error in row.get('errors', []): print('  ERROR %s: %s' % (error['code'], error['status']))
        for cell, data in row['cells'].items():
            if data['status'] == 'na': print('  N/A %s: %s' % (cell, data['reason']))
    for gap in report['gaps']: print('GAP: ' + gap)
    print('INVENTORY: %d commands; %d gaps' % (len(report['rows']), len(report['gaps'])))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('binary', type=Path); parser.add_argument('testfiles', type=Path)
    parser.add_argument('--catalog', type=Path, help='offline review only; live ctest never uses this')
    parser.add_argument('--json', type=Path, default=os.environ.get('VACARDS_CLI_INVENTORY_JSON'))
    args = parser.parse_args()
    if not args.catalog and not args.binary.is_file(): print('SKIP: vastudio-cli missing'); return 77
    try:
        catalog = schema_module().loads(args.catalog.read_text()) if args.catalog else read_catalog(args.binary.resolve())
        report = matrix(catalog, args.testfiles); print_matrix(report)
    except (OSError, ValueError, KeyError, TypeError, subprocess.SubprocessError) as error:
        report = {'passed': False, 'gaps': [str(error)], 'rows': []}; print('ERROR: ' + str(error))
    if args.json: args.json.write_text(json.dumps(report, indent=2) + '\n')
    return 0 if report['passed'] else 1



def m3_matrix(testfiles=None, catalog=None):
    """Authoring coverage is separate from executable/receipt acceptance."""
    spec=importlib.util.spec_from_file_location('p9_m3_inventory',Path(__file__).with_name('vacards-cli-m3-cases.py'))
    m3=importlib.util.module_from_spec(spec);spec.loader.exec_module(m3)
    rows=m3.load_cases(); manifest=json.loads(m3.MANIFEST.read_text()); gaps=[]; commands=[]
    if manifest.get('descriptor_hash') != 'aedbdc2776bd24e09415ff14662640fd460fe41eb94a7082a967b52671e56a40': gaps.append('unreviewed M3 descriptor hash')
    if catalog is not None and catalog.get('hash') != 'cca1282df999f5e18a8314f823a3498b05a058b2f0e19a2cac3b3e6cd29e2e36': gaps.append('unreviewed DESC-3 catalog hash')
    if sum(len(c['errors']) for c in manifest['commands']) != 527: gaps.append('M3 error inventory must contain 527 rows')
    # Frozen schema is for independently validating authored params, never generating expected results.
    frozen=Path(__file__).resolve().parents[2]/'work/cli-b31/evidence/INT/m3-obligations-02-snapshots/catalog.json'
    snapshot = catalog if catalog is not None else json.loads(frozen.read_text()) if frozen.exists() else {'commands':[]}
    schemas = {c['id']:dict(c, params=c.get('request_schema',{}).get('properties',{}).get('params', c.get('params',{}))) for c in snapshot['commands']}
    schema=schema_module()
    def tokens(v):
        if isinstance(v,dict) and set(v)=={'$ref'}:return 'p9-token'
        if isinstance(v,dict):return {k:tokens(x) for k,x in v.items()}
        if isinstance(v,list):return [tokens(x) for x in v]
        return v
    for cid in m3.COMMANDS:
        cases=[c for _,c in rows if c['command']==cid]
        cells={cell:[c['id'] for c in cases if c['cell']==cell] for cell in m3.CELLS}
        for cell in ('success','invalid','boundary','refusal','dry-run','preservation'):
            if not cells[cell]:gaps.append(cid+': missing '+cell)
        positive=[]
        for c in cases:
            for st in c['requests']:
                req=st.get('request',{})
                if req.get('command')!=cid or st.get('expect_error'):continue
                if cid in schemas:
                    try:schema.validate(tokens(req['params']),schemas[cid]['params'])
                    except ValueError as e:gaps.append(c['id']+': invalid authored request '+str(e));continue
                positive.append(req['params'])
        enums=[]
        if cid in schemas:
            for path,s in parameter_enums(schemas[cid]['params']):
                for value in s['enum']:
                    covered=any(schema.equal(v,value) for p in positive for v in enum_values(p,schemas[cid]['params'],path))
                    enums.append({'path':path,'value':value,'authored':covered})
                    if not covered:gaps.append(cid+': missing enum '+path+'='+json.dumps(value))
        else:gaps.append(cid+': missing frozen request schema')
        commands.append({'command':cid,'cells':cells,'enums':enums,'native':m3.native_oracle(cid),
                         'cancel':'native-required: deterministic cancellation boundary, never a fast-command timing race',
                         'noop':'authored' if cells['noop'] else 'requires reviewed native applicability disposition'})
    return {'schema':'p9-m3-matrix/1','commands':commands,'wire_cases':len(rows),'native_obligations':31,
            'gaps':gaps,'passed':not gaps,'status':'authored coverage only; native receipts and D12 separately required',
            'manifest_sha256':__import__('hashlib').sha256(m3.MANIFEST.read_bytes()).hexdigest()}

if __name__ == '__main__': sys.exit(main())
