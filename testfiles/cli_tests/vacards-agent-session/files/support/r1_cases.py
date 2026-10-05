#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""R1 policy cases, with expected bytes and semantics authored independently."""
import copy
import hashlib
import json
from pathlib import Path
import sys
sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).parent))
import cases as c


def main():
    c.ARTIFACTS[:] = [p.name for p in c.ROOT.glob('*.json')]
    for fmt in ('png', 'tiff'):
        for fault, code in [('changed', 'stale-dependency'), ('missing', 'resource-unavailable'),
                            ('ungranted', 'read-grant-denied')]:
            path = c.ROOT / ('export-' + fmt + '-profile-' + fault + '.json')
            case = json.loads(path.read_text()); case['requests'][-1]['expect_error'] = code
            c.atomic_json(path, case)
        path = c.ROOT / ('export-' + fmt + '-refuse-oversize-icc.json')
        case = json.loads(path.read_text()); case['requests'][-1]['expect_error'] = 'engine-limit'
        c.atomic_json(path, case)
    # Keep both independently asserted PDF page queries, each with a unique ID.
    for op in ('open','import'):
        path = c.ROOT / (op + '-pdf-pages-1_2.json')
        case = json.loads(path.read_text()); used = set()
        for index, row in enumerate(case['requests']):
            request = row.get('request', {})
            ident = request.get('id')
            if ident in used: request['id'] = ident + '-repeat-' + str(index)
            used.add(request.get('id'))
        c.atomic_json(path,case)
    for op in ('save', 'export'):
        s = c.save() if op == 'save' else c.raster()
        suffix = 'svg' if op == 'save' else 'png'
        s['request']['params']['path'] = '{WORK}/link/output.' + suffix
        s['file_oracles'][0]['path'] = s['request']['params']['path']
        setup = [{'op':'mkdir','path':'{WORK}/real'},
                 {'op':'symlink','source':'{WORK}/real','path':'{WORK}/link','directory':True}]
        c.write(op + '-symlink-parent', [c.opened(), s], setup=setup)
        denied = c.step('file.' + op, s['request']['params'], status='rejected',
                        error='write-grant-denied', cells=['refusal'], document=c.live())
        denied['file_oracles'] = [{'path':s['request']['params']['path'],'absent':True}]
        c.write(op + '-symlink-target-ungranted', [c.opened(), denied],
                ['--grant-read-file','{WORK}/inputs/sheet.svg','--grant-write','{WORK}/out'],
                setup=setup + [{'op':'mkdir','path':'{WORK}/out'}])
    # Literal # is part of a command filename, never a reference fragment.
    setup = [{'op':'copy','source':'{WORK}/inputs/sheet.svg','path':'{WORK}/inputs/sheet.svg#private.svg'}]
    opened = c.opened(); opened['request']['params']['path'] = '{WORK}/inputs/sheet.svg#private.svg'
    opened['file_oracles'][0]['path'] = opened['request']['params']['path']
    c.write('open-literal-hash-granted', [opened, c.query()],
            ['--grant-read-file',opened['request']['params']['path']], setup=setup)
    denied = c.step('file.open', opened['request']['params'], status='rejected',
                    error='read-grant-denied', cells=['refusal'])
    c.write('open-literal-hash-prefix-ungranted', [denied],
            ['--grant-read-file','{WORK}/inputs/sheet.svg'], setup=setup)
    for op in ('save','export'):
        s = c.save() if op == 'save' else c.raster()
        s['request']['params']['path'] = '{WORK}/output#literal.' + ('svg' if op == 'save' else 'png')
        s['file_oracles'][0]['path'] = s['request']['params']['path']
        c.write(op + '-literal-hash-granted', [c.opened(),s],
                ['--grant-read-file','{WORK}/inputs/sheet.svg','--grant-write-file',s['request']['params']['path']])
    # Both read and publication use the system spelling; grants use physical paths.
    for alias, parent in [('TMP','system-tmp'),('VAR',None)]:
        a = '{WORK_' + alias + '_ALIAS}'
        opened = c.opened(); opened['request']['params']['path'] = a + '/inputs/sheet.svg'
        opened['file_oracles'][0]['path'] = opened['request']['params']['path']
        s = c.save(); s['request']['params']['path'] = a + '/saved.svg'
        s['file_oracles'][0]['path'] = s['request']['params']['path']
        extra = {'work_parent':parent} if parent else {}
        c.write('system-' + alias.lower() + '-alias-read-write', [opened,s],
                ['--grant-read-file','{WORK}/inputs/sheet.svg','--grant-write-file','{WORK}/saved.svg'], **extra)
    opened = c.opened(); opened['request']['params']['path'] = '{WORK}/inputs/CASE-EQUIVALENT.svg'
    opened['file_oracles'][0]['path'] = opened['request']['params']['path']
    c.write('open-granted-case-variant', [opened,c.query()],
            ['--grant-read-file','{WORK}/inputs/case-equivalent.svg'],
            setup=[{'op':'copy','source':'{WORK}/inputs/sheet.svg','path':'{WORK}/inputs/case-equivalent.svg'}])
    # Fixed first missing family followed by a generic. No user fonts are required.
    fixture = b'<svg xmlns="http://www.w3.org/2000/svg" width="16" height="8"><text id="letter" x="2" y="6" style="font-family:\'P9 Missing R1\', serif;font-size:6px">A</text></svg>\n'
    target = c.FIXTURES / 'font-list.svg'
    tmp = target.with_name(target.name + '.p9-tmp'); tmp.write_bytes(fixture); tmp.replace(target)
    manifest = json.loads((c.FIXTURES/'manifest.json').read_text())
    manifest['fixtures'] = [e for e in manifest['fixtures'] if e['path'] != 'font-list.svg']
    manifest['fixtures'].append(dict(path='font-list.svg',bytes=len(fixture),sha256=hashlib.sha256(fixture).hexdigest(),
        provenance='P9 R1 literal SVG in files/support/r1_cases.py; missing first family followed by generic serif',
        oracle={'size_px':[16,8],'text':'A','first_family':'P9 Missing R1','policy':'reject or report first-family substitution'}))
    c.atomic_json(c.FIXTURES/'manifest.json',manifest)
    for policy in ('reject','substitute'):
        s = c.step('file.open',c.load('font-list.svg',font=policy),status='rejected' if policy=='reject' else 'changed',
                    error='font-policy-required' if policy=='reject' else None,cells=['refusal'] if policy=='reject' else ['success'])
        if policy == 'substitute':
            s['expect_exact'] = {'data.intake.fonts.0.family':'P9 Missing R1','data.intake.fonts.0.substituted':True}
            s['expect_nonempty'] = ['warnings']
        c.write('open-font-family-list-' + policy,[s])
    # Stale requested SHA and changed admitted bytes are both stale-dependency.
    # An invalid profile with its correct independent SHA remains profile-invalid.
    for fault in ('stale','changed-bytes','ungranted','invalid'):
        out = c.raster(profile='custom-rgb.icc')
        code = {'stale':'stale-dependency','changed-bytes':'stale-dependency',
                'ungranted':'read-grant-denied','invalid':'profile-invalid'}[fault]
        out.update(expect={'status':'rejected','publication':c.N,'undo_effect':'none'},expect_error=code,cells=['refusal'])
        out.pop('expect_absent'); out['file_oracles'] = [{'path':out['request']['params']['path'],'absent':True}]
        extra = {}; startup = None
        if fault == 'stale': out['request']['params']['profile']['sha256'] = '0'*64
        if fault in ('changed-bytes','invalid'):
            out['request']['params']['profile']['path'] = '{WORK}/custom.icc'
            extra['setup'] = [{'op':'copy','source':'{WORK}/inputs/custom-rgb.icc','path':'{WORK}/custom.icc'}]
            out['before'] = [{'op':'write','path':'{WORK}/custom.icc','text':'bad'}]
            out['file_oracles'].append({'path':'{WORK}/custom.icc','sha256':hashlib.sha256(b'bad').hexdigest()})
        if fault == 'ungranted': startup = ['--grant-read-file','{WORK}/inputs/sheet.svg','--grant-write','{WORK}']
        if fault == 'invalid':
            out['request']['params']['profile']['sha256'] = hashlib.sha256(b'bad').hexdigest()
        c.write('export-icc-' + fault,[c.opened(),out],startup,**extra)
    ledger = json.loads((c.ROOT/'support/inventory.json').read_text())
    ledger['required_cases'] = sorted(set(c.ARTIFACTS))
    if 'version-read-unavailable' not in ledger['native_domains']:
        ledger['native_domains'].append('version-read-unavailable')
    ledger['native_only'] = [{'domain':'version-read-unavailable',
        'reason':'Portable CLI harness has no deterministic version-reader fault hook. chmod depends on credentials/ACLs and Windows sharing needs a held native handle. Native R1 Files version-unavailable tests exercise permissions/sharing; no wire pass or exemption is inferred.'},
        {'domain':'filesystem-equivalent-spelling-on-equivalent-volume',
         'reason':'The case-variant wire case requires case-equivalent filesystem semantics (Mac APFS here); case-sensitive volumes must qualify NFD/case aliases with native fixtures. The authored wire case is strict and never skipped.'}]
    c.atomic_json(c.ROOT/'support/inventory.json',ledger)
    print('CASES:',len(ledger['required_cases']))

if __name__ == '__main__': main()
