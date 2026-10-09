#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Live catalog/byte drift gate; optional isolated literal-example executions.

No missing executable skip. The test is registered in testfiles/CMakeLists.txt.
"""
import argparse
import copy
import importlib.util
import json
import subprocess
import sys
import tempfile
import time
from pathlib import Path

sys.dont_write_bytecode = True
spec = importlib.util.spec_from_file_location('docs_generator', Path(__file__).with_name('vacards-cli-docs-generate.py'))
gen = importlib.util.module_from_spec(spec)
spec.loader.exec_module(gen)


def negative_fixture(catalog, family_dir=None):
    """Create the clean generated-doc fixture shared by the existing drift scenarios."""
    root = Path(tempfile.mkdtemp(prefix='vacards-docs-negative-'))
    doc = root / 'doc'; doc.mkdir()
    (doc / 'AGENT_GUIDE.template.md').write_bytes((gen.DOC / 'AGENT_GUIDE.template.md').read_bytes())
    expected = gen.outputs(catalog, doc, root / 'cases', root / 'families')
    for path, text in expected.items():
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding='utf-8', newline='\n')
    if gen.drift(expected):
        raise ValueError('clean negative fixture drifted')
    return root, doc, expected


def check_missing_example_rejected(catalog, family_dir=None):
    broken = copy.deepcopy(catalog)
    del broken['commands'][0]['example']
    try:
        gen.validate_catalog(broken)
    except (ValueError, KeyError):
        return
    else:
        raise ValueError('missing example did not fail')


def with_negative_fixture(catalog, check):
    root, doc, expected = negative_fixture(catalog)
    try:
        return check(root, doc, expected)
    finally:
        import shutil
        shutil.rmtree(root)


def check_catalog_hash_drift_rejected(catalog, family_dir=None):
    def check(root, doc, expected):
        changed = copy.deepcopy(catalog)
        changed['hash'] = ('0' if catalog['hash'][0] != '0' else '1') + catalog['hash'][1:]
        if not gen.drift(gen.outputs(changed, doc, root / 'cases', root / 'families')):
            raise ValueError('catalog hash drift did not fail')
    with_negative_fixture(catalog, check)


def check_reference_content_drift_rejected(catalog, family_dir=None):
    def check(root, doc, expected):
        path = doc / 'reference.md'; path.write_text('stale docs\n', encoding='utf-8', newline='\n')
        if str(path) not in gen.drift(expected):
            raise ValueError('content drift did not fail')
    with_negative_fixture(catalog, check)


def check_missing_family_page_rejected(catalog, family_dir=None):
    def check(root, doc, expected):
        page = next(path for path in expected if path.parent == root / 'families' and path.name != 'index.md')
        page.unlink()
        if str(page) not in gen.drift(expected):
            raise ValueError('missing family page did not fail')
    with_negative_fixture(catalog, check)


def check_generated_docs_current(catalog, family_dir=None):
    expected = gen.outputs(catalog, family_dir=family_dir)
    changed = gen.drift(expected)
    if changed:
        raise ValueError('generated docs drift: ' + ', '.join(changed))


CHECKS = {
    'generated-docs-current': check_generated_docs_current,
    'missing-example-rejected': check_missing_example_rejected,
    'catalog-hash-drift-rejected': check_catalog_hash_drift_rejected,
    'reference-content-drift-rejected': check_reference_content_drift_rejected,
    'missing-family-page-rejected': check_missing_family_page_rejected,
}


def load_cases(directory):
    cases = []
    for path in sorted(Path(directory).glob('*.json'), key=lambda item: item.name):
        try:
            data = json.loads(path.read_text(encoding='utf-8'))
            check = data['check']
            if not isinstance(check, str) or not check:
                raise ValueError('check must be a non-empty string')
            cases.append((path.name, check))
        except (OSError, ValueError, KeyError, TypeError) as error:
            cases.append((path.name, error))
    return cases


def execute_examples(binary, catalog, evidence):
    """Literal examples test typed outcomes, not successful stateful applicability.

    No grants, no document and a private working directory per example. Preserve
    requests exactly; expected placeholder/context refusals are recorded as such.
    Successful native outcomes must be bound and validated independently.
    """
    evidence.mkdir(parents=True, exist_ok=True)
    schema = gen.schema_module()
    rows = []
    for command in sorted(catalog['commands'], key=lambda c: c['id']):
        with tempfile.TemporaryDirectory(prefix='vacards-docs-example-') as folder:
            path = Path(folder) / 'request.json'
            path.write_text(gen.json_text(command['example']), encoding='utf-8', newline='\n')
            argv = [str(binary), '--agent-request-file', str(path)]
            start = time.monotonic()
            process = subprocess.run(argv, cwd=folder, capture_output=True, timeout=45)
            row = dict(command=command['id'], request=command['example'], argv=argv, exit=process.returncode, seconds=time.monotonic()-start, stderr=process.stderr.decode('utf-8', errors='replace'))
            try:
                lines = process.stdout.decode('utf-8').splitlines()
                if len(lines) != 1: raise ValueError('expected one terminal record')
                record = schema.loads(lines[0]); row['result'] = record
                schema.validate(record, command['result_schema'])
                if record.get('action') != command['id'] or record.get('id') != command['example']['id']:
                    raise ValueError('result identity mismatch')
                exit_by_status = {'ok': 0, 'changed': 0, 'unchanged': 0, 'rejected': 3, 'cancelled': 3, 'failed': 4, 'uncertain': 4}
                expected_exit = exit_by_status[record['status']]
                if process.returncode != expected_exit: raise ValueError('result/exit mismatch')
                row['passed'] = True
            except (ValueError, KeyError) as error:
                row.update(passed=False, failure=str(error), stdout=process.stdout.decode('utf-8', errors='replace'))
            receipt = evidence / (command['id'] + '.json')
            if receipt.exists(): raise ValueError('refusing to overwrite example receipt: ' + str(receipt))
            receipt.write_text(gen.json_text(row), encoding='utf-8', newline='\n')
            rows.append(row)
    return rows


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=Path, required=True)
    parser.add_argument('--family-dir', type=Path)
    parser.add_argument('--execute-examples', type=Path, metavar='FRESH_EVIDENCE_DIR')
    parser.add_argument('case_dir', type=Path)
    args = parser.parse_args()
    cases = load_cases(args.case_dir)
    if not cases:
        print('no JSON cases found', file=sys.stderr)
        return 1
    try:
        catalog = gen.read_catalog(args.binary)
    except (ValueError, KeyError, OSError, subprocess.SubprocessError) as error:
        for filename, _ in cases:
            print('FAIL ' + filename + ': ' + str(error), flush=True)
        return 1
    failed = False
    for filename, check in cases:
        try:
            if isinstance(check, Exception):
                raise check
            if check not in CHECKS:
                raise ValueError('unknown check: ' + check)
            CHECKS[check](catalog, args.family_dir)
            print('PASS ' + filename, flush=True)
        except Exception as error:
            failed = True
            print('FAIL ' + filename + ': ' + str(error), flush=True)
    if args.execute_examples:
        rows = execute_examples(args.binary.resolve(strict=True), catalog, args.execute_examples)
        failed = failed or not all(row['passed'] for row in rows)
    return 1 if failed else 0


if __name__ == '__main__':
    raise SystemExit(main())
