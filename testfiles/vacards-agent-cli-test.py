#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""VACards agent CLI case runner.

Usage: vacards-agent-cli-test.py INKSCAPE CASE_DIR

Runs every ``*.json`` case in CASE_DIR (sorted) against the VACards agent CLI
of the given Inkscape binary.  Case format is described in
``testfiles/cli_tests/vacards-agent/README.md``.  Exit status:

* 77 -- CASE_DIR is missing or holds no ``*.json`` case (ctest "Skipped");
*  1 -- at least one case failed;
*  0 -- every case passed.
"""

import json
import os
import shutil
import subprocess
import sys
import tempfile

RESULT_SCHEMA = "va-studio.cli-result/1"
RECORD_PREFIX = "VASTUDIO-RESULT "
TAIL_CHARS = 3000


def _tokens(work_dir, source_testfiles):
    """Placeholder substitutions applied to every string of a case."""
    return (
        ("{WORK}", work_dir),
        ("{RESULT}", os.path.join(work_dir, "result.jsonl")),
        ("{TESTCASES}", os.path.join(source_testfiles, "cli_tests", "testcases")),
        ("{FIXTURES}", os.path.join(source_testfiles, "cli_tests", "vacards-agent", "fixtures")),
    )


def substitute(value, tokens):
    """Recursively str.replace the placeholders in every string of a JSON value."""
    if isinstance(value, str):
        for token, replacement in tokens:
            value = value.replace(token, replacement)
        return value
    if isinstance(value, list):
        return [substitute(item, tokens) for item in value]
    if isinstance(value, dict):
        return {key: substitute(item, tokens) for key, item in value.items()}
    return value


def match(expected, actual, path, failures):
    """Structural match: dicts are subset matches, lists are elementwise, else equal."""
    if isinstance(expected, dict):
        if not isinstance(actual, dict):
            failures.append("%s: expected an object, got %s" % (path, type(actual).__name__))
            return False
        ok = True
        for key, value in expected.items():
            if key not in actual:
                failures.append("%s.%s: missing key (actual keys: %s)" % (path, key, sorted(actual)))
                ok = False
                continue
            if not match(value, actual[key], "%s.%s" % (path, key), failures):
                ok = False
        return ok
    if isinstance(expected, list):
        if not isinstance(actual, list) or len(expected) != len(actual):
            shown = actual if isinstance(actual, list) else type(actual).__name__
            failures.append("%s: expected a list of %d, got %s" % (path, len(expected), shown))
            return False
        ok = True
        for index, (exp_item, act_item) in enumerate(zip(expected, actual)):
            if not match(exp_item, act_item, "%s[%d]" % (path, index), failures):
                ok = False
        return ok
    # A bool only matches a bool: type and value must agree, so true != 1. This is
    # checked before the numeric tolerance, which must never swallow a bool.
    if isinstance(expected, bool) or isinstance(actual, bool):
        if type(expected) is not type(actual) or expected != actual:
            failures.append("%s: expected %r, got %r" % (path, expected, actual))
            return False
        return True
    # Numbers (but not bools) compare with a relative tolerance so a value pinned
    # from one rounding does not fail on an equivalent floating-point result.
    if (
        isinstance(expected, (int, float))
        and not isinstance(expected, bool)
        and isinstance(actual, (int, float))
        and not isinstance(actual, bool)
    ):
        if abs(expected - actual) <= 1e-6 * max(1.0, abs(expected), abs(actual)):
            return True
        failures.append("%s: expected %r, got %r" % (path, expected, actual))
        return False
    if expected != actual:
        failures.append("%s: expected %r, got %r" % (path, expected, actual))
        return False
    return True


def gather_records(case, work_dir, stdout_text, stderr_text, failures):
    """Return (record_lines, records) for the selected result channel."""
    source = case["_records_from"]
    if source == "file":
        path = os.path.join(work_dir, "result.jsonl")
        lines = []
        if os.path.exists(path):
            with open(path, "r", encoding="utf-8", errors="replace") as handle:
                lines = [line for line in handle.read().splitlines() if line.strip()]
    else:
        stream = stdout_text if source == "stdout" else stderr_text
        lines = [
            line[len(RECORD_PREFIX):] for line in stream.splitlines() if line.startswith(RECORD_PREFIX)
        ]

    records = []
    for index, line in enumerate(lines):
        try:
            records.append(json.loads(line))
        except ValueError as error:
            failures.append("record %d: invalid JSON (%s): %s" % (index + 1, error, line[:200]))
    return lines, records


def check_records(records, case, failures):
    """Schema/seq invariants plus an optional expect_records comparison."""
    for index, record in enumerate(records):
        if not isinstance(record, dict):
            failures.append("record %d: expected a JSON object" % (index + 1))
            continue
        if record.get("schema") != RESULT_SCHEMA:
            failures.append("record %d: schema %r != %r" % (index + 1, record.get("schema"), RESULT_SCHEMA))
        if record.get("seq") != index + 1:
            failures.append("record %d: seq %r != %d" % (index + 1, record.get("seq"), index + 1))

    if "expect_records" in case:
        expected = case["expect_records"]
        if len(expected) != len(records):
            failures.append("expect_records: expected %d record(s), got %d" % (len(expected), len(records)))
        else:
            for index, (exp_item, act_item) in enumerate(zip(expected, records)):
                match(exp_item, act_item, "expect_records[%d]" % index, failures)


def check_contains(name, needles, haystack, failures):
    for needle in needles:
        if needle not in haystack:
            failures.append("%s: missing %r" % (name, needle))


def check_not_contains(name, needles, haystack, failures):
    for needle in needles:
        if needle in haystack:
            failures.append("%s: unexpectedly present %r" % (name, needle))


def check_files(work_dir, expected_files, failures):
    for rel_path, spec in expected_files.items():
        path = os.path.join(work_dir, rel_path)
        exists = os.path.exists(path)
        if "exists" in spec and exists != spec["exists"]:
            failures.append("expect_files[%s]: exists %s != %s" % (rel_path, exists, spec["exists"]))
        if not exists:
            continue
        with open(path, "r", encoding="utf-8", errors="replace") as handle:
            content = handle.read()
        check_contains("expect_files[%s].contains" % rel_path, spec.get("contains", []), content, failures)
        check_not_contains(
            "expect_files[%s].not_contains" % rel_path, spec.get("not_contains", []), content, failures
        )


def check_same_files(work_dir, pairs, failures):
    """Every [A, B] pair must name two existing files with identical bytes."""
    for pair in pairs:
        rel_a, rel_b = pair[0], pair[1]
        path_a = os.path.join(work_dir, rel_a)
        path_b = os.path.join(work_dir, rel_b)
        same = False
        if os.path.exists(path_a) and os.path.exists(path_b):
            with open(path_a, "rb") as handle_a, open(path_b, "rb") as handle_b:
                same = handle_a.read() == handle_b.read()
        if not same:
            failures.append("files differ: %s %s" % (rel_a, rel_b))


def run_case(case_path, inkscape, source_testfiles):
    with open(case_path, "r", encoding="utf-8") as handle:
        raw_case = json.load(handle)

    raw_actions = raw_case.get("actions", "")
    if not isinstance(raw_actions, str):
        raw_actions = ""

    work_dir = tempfile.mkdtemp(prefix="vacards-agent-")
    case = substitute(raw_case, _tokens(work_dir, source_testfiles))

    actions = case.get("actions", "")
    args = list(case.get("args", []))
    timeout = case.get("timeout", 120)
    expect_exit = case.get("expect_exit", 0)
    stdin_text = case.get("stdin")
    uses_result = "{RESULT}" in raw_actions or "{RESULT}" in (raw_case.get("stdin") or "")
    default_source = "file" if uses_result else "stderr"
    case["_records_from"] = case.get("records_from", default_source)

    input_copy = None
    if case.get("input"):
        source_path = case["input"]
        input_copy = os.path.join(work_dir, os.path.basename(source_path))
        shutil.copyfile(source_path, input_copy)

    cmd = [inkscape]
    if input_copy:
        cmd.append(input_copy)
    if "actions" in case:
        cmd.append("--actions=" + actions)
    cmd.extend(args)

    env = dict(os.environ)
    env["INKSCAPE_PROFILE_DIR"] = os.path.join(work_dir, "profile")
    env["INKSCAPE_APP_ID_TAG"] = "vacardsagent"
    env["LC_ALL"] = "C"

    failures = []
    timed_out = False
    stdout_text = ""
    stderr_text = ""
    returncode = None
    try:
        completed = subprocess.run(
            cmd, cwd=work_dir, env=env, capture_output=True, timeout=timeout, check=False,
            input=stdin_text.encode("utf-8") if stdin_text is not None else None
        )
        returncode = completed.returncode
        stdout_text = (completed.stdout or b"").decode("utf-8", errors="replace")
        stderr_text = (completed.stderr or b"").decode("utf-8", errors="replace")
    except subprocess.TimeoutExpired as error:
        timed_out = True
        failures.append("timed out after %s second(s)" % timeout)
        stdout_text = (error.stdout or b"").decode("utf-8", errors="replace")
        stderr_text = (error.stderr or b"").decode("utf-8", errors="replace")
        returncode = "timeout"

    if not timed_out and returncode != expect_exit:
        failures.append("exit code: expected %s, got %s" % (expect_exit, returncode))

    record_lines, records = gather_records(case, work_dir, stdout_text, stderr_text, failures)
    check_records(records, case, failures)

    record_text = "\n".join(record_lines)
    check_contains(
        "expect_records_text_contains", case.get("expect_records_text_contains", []), record_text, failures
    )
    check_contains("expect_stdout_contains", case.get("expect_stdout_contains", []), stdout_text, failures)
    check_contains("expect_stderr_contains", case.get("expect_stderr_contains", []), stderr_text, failures)
    check_not_contains(
        "expect_stderr_not_contains", case.get("expect_stderr_not_contains", []), stderr_text, failures
    )
    check_files(work_dir, case.get("expect_files", {}), failures)
    check_same_files(work_dir, case.get("expect_same_files", []), failures)

    return {
        "failures": failures,
        "cmd": cmd,
        "returncode": returncode,
        "stdout": stdout_text,
        "stderr": stderr_text,
        "records": records,
        "work_dir": work_dir,
    }


def main(argv):
    if len(argv) != 3:
        sys.stderr.write("Usage: %s INKSCAPE CASE_DIR\n" % os.path.basename(argv[0]))
        return 2

    inkscape, case_dir = argv[1], argv[2]
    source_testfiles = os.path.dirname(os.path.abspath(__file__))

    case_files = []
    if os.path.isdir(case_dir):
        case_files = sorted(
            os.path.join(case_dir, name) for name in os.listdir(case_dir) if name.endswith(".json")
        )
    if not case_files:
        print("SKIP: no cases in %s" % case_dir)
        return 77

    any_failed = False
    for case_path in case_files:
        name = os.path.basename(case_path)
        try:
            result = run_case(case_path, inkscape, source_testfiles)
        except Exception as error:  # a runner-level problem is still a case failure
            any_failed = True
            print("FAIL %s: runner error: %s" % (name, error))
            continue

        if result["failures"]:
            any_failed = True
            print("FAIL %s: %s" % (name, result["failures"][0]))
            for failure in result["failures"][1:]:
                print("  %s" % failure)
            print("  command: %s" % " ".join(result["cmd"]))
            print("  exit code: %s" % result["returncode"])
            print("  stdout (last %d chars):\n%s" % (TAIL_CHARS, result["stdout"][-TAIL_CHARS:]))
            print("  stderr (last %d chars):\n%s" % (TAIL_CHARS, result["stderr"][-TAIL_CHARS:]))
            print("  records: %s" % json.dumps(result["records"], indent=2))
            print("  work dir: %s" % result["work_dir"])
        else:
            print("PASS %s" % name)
            shutil.rmtree(result["work_dir"], ignore_errors=True)

    return 1 if any_failed else 0


# The legacy path above deliberately remains byte-for-byte unchanged.
def typed_schema_module():
    sys.dont_write_bytecode = True
    import importlib.util
    path = os.path.join(os.path.dirname(__file__), 'vacards-cli-schema.py')
    spec = importlib.util.spec_from_file_location('vacards_cli_schema', path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def typed_file_oracles():
    import importlib.util
    from pathlib import Path
    path = Path(__file__).parent / 'cli_tests/vacards-agent-session/files/support/oracles.py'
    spec = importlib.util.spec_from_file_location('p9_file_oracles', path)
    module = importlib.util.module_from_spec(spec); spec.loader.exec_module(module)
    return module


def typed_refs(value, context):
    if isinstance(value, dict) and set(value) == {'$ref'}:
        return typed_file_oracles().lookup(context, value['$ref'])
    if isinstance(value, dict): return {k: typed_refs(v, context) for k, v in value.items()}
    if isinstance(value, list): return [typed_refs(v, context) for v in value]
    return value


def typed_snapshot(root):
    from pathlib import Path
    oracle = typed_file_oracles()
    return {str(p.relative_to(root)): ('link', os.readlink(p)) if p.is_symlink() else ('file', oracle.sha(p))
            for p in Path(root).rglob('*') if p.is_symlink() or p.is_file()}


WINDOWS_SYMLINK_CASES = {'save-symlink-parent.json', 'export-symlink-parent.json',
                        'save-symlink-target-ungranted.json', 'export-symlink-target-ungranted.json'}
WINDOWS_SYMLINK_SKIP = 'windows-symlink-creation-not-permitted'

class WindowsSymlinkUnavailable(OSError):
    pass


def typed_platform_case(case, name, platform=None):
    platform = platform or sys.platform
    if platform != 'win32': return case
    import copy
    case = copy.deepcopy(case)
    for step in case.get('requests', []):
        override = step.pop('platform_expect', {}).get(platform)
        if override:
            if name not in WINDOWS_SYMLINK_CASES: raise ValueError('undeclared platform oracle')
            for key in ('expect', 'expect_error', 'expect_absent', 'file_oracles', 'cells'):
                step.pop(key, None)
            step.update(override)
    return case


def typed_symlink_skip(case, name, error, platform=None):
    return ((platform or sys.platform) == 'win32' and name in WINDOWS_SYMLINK_CASES
            and case.get('windows_symlink_skip') == WINDOWS_SYMLINK_SKIP
            and isinstance(error, WindowsSymlinkUnavailable))


def typed_setup(actions, work):
    """Controlled test filesystem changes confined to the disposable root."""
    from pathlib import Path
    root = Path(work).resolve()
    for action in actions:
        target = Path(action['path'])
        if not target.is_absolute() or not target.parent.resolve().is_relative_to(root):
            raise ValueError('setup target outside disposable work root')
        target.parent.mkdir(parents=True, exist_ok=True)
        op = action['op']
        if op == 'copy': shutil.copyfile(action['source'], target)
        elif op == 'write': target.write_text(action['text'], encoding='utf-8')
        elif op == 'mkdir': target.mkdir(exist_ok=True)
        elif op == 'symlink':
            try: target.symlink_to(action['source'], target_is_directory=action.get('directory', False))
            except OSError as error:
                if sys.platform == 'win32' and getattr(error, 'winerror', None) in (5, 1314):
                    raise WindowsSymlinkUnavailable(str(error)) from error
                raise
        elif op == 'unlink': target.unlink()
        elif op == 'replace-same-bytes':
            other = target.with_name(target.name + '.replacement'); other.write_bytes(target.read_bytes()); os.replace(other, target)
        elif op == 'change-same-size-mtime':
            stat = target.stat(); data = bytearray(target.read_bytes())
            if not data: raise ValueError('empty same-size mutation')
            data[-1] ^= 1; target.write_bytes(data); os.utime(target, ns=(stat.st_atime_ns, stat.st_mtime_ns))
        else: raise ValueError('unknown controlled setup operation: ' + op)


def typed_disk_expect(step, record, failures, label):
    oracle = typed_file_oracles()
    try:
        for spec in step.get('file_oracles', []): oracle.verify_file(spec, record)
    except (Exception,) as error: failures.append(label + ': disk oracle: ' + str(error))


def typed_expect(actual, step, failures, label):
    """Subset values, explicit exact numbers/tolerances, absent dotted keys."""
    if step.get('m3'):
        import importlib.util
        from pathlib import Path
        spec = importlib.util.spec_from_file_location('p9_m3', Path(__file__).with_name('vacards-cli-m3-cases.py'))
        m3 = importlib.util.module_from_spec(spec); spec.loader.exec_module(m3)
        try: m3.validate_result(actual, step['request'])
        except (ValueError, KeyError, TypeError) as error: failures.append(label + ': M3 outcome: ' + str(error))
    match(step.get('expect', {}), actual, label, failures)
    def lookup(path):
        node = actual
        for part in path.split('.'):
            node = node[int(part)] if isinstance(node, list) else node[part]
        return node
    for path in step.get('expect_absent', []):
        try: lookup(path)
        except (KeyError, IndexError, TypeError, ValueError): pass
        else: failures.append(label + ': unexpected key ' + path)
    schema = typed_schema_module()
    for path, value in step.get('expect_exact', {}).items():
        try:
            if not schema.equal(lookup(path), value): failures.append(label + ': exact mismatch ' + path)
        except (KeyError, IndexError, TypeError, ValueError): failures.append(label + ': missing ' + path)
    for path, value in step.get('expect_not_equal', {}).items():
        try:
            if schema.equal(lookup(path), value): failures.append(label + ': expected fresh/different ' + path)
        except (KeyError, IndexError, TypeError, ValueError): failures.append(label + ': missing ' + path)
    for path, spec in step.get('expect_numeric', {}).items():
        try:
            value = lookup(path)
            if not schema.number(value) or abs(value - spec['value']) > spec.get('abs', 0) + spec.get('rel', 0) * abs(spec['value']):
                failures.append(label + ': numeric tolerance violated ' + path)
        except (KeyError, IndexError, TypeError, ValueError): failures.append(label + ': invalid numeric expectation ' + path)
    for path in step.get('expect_nonempty', []):
        try:
            if not lookup(path): failures.append(label + ': empty ' + path)
        except (KeyError, IndexError, TypeError, ValueError): failures.append(label + ': missing ' + path)
    if 'expect_error' in step:
        error = actual.get('error', {})
        if error.get('code') != step['expect_error']: failures.append(label + ': wrong error code ' + repr(error.get('code')))
        if not isinstance(error.get('hint'), str) or not error['hint'].strip(): failures.append(label + ': empty error hint')



def typed_result_command(record, requested, commands, schema):
    """Resolve only catalog-advertised aliases, then bind the result to its request."""
    command = commands.get(requested)
    if command is None:
        matches = [c for c in commands.values() if requested in c.get('aliases', [])]
        if len(matches) != 1: raise ValueError('result command has no unique catalog schema: ' + str(requested))
        command = matches[0]
    action = record.get('action')
    if action not in [command['id']] + command.get('aliases', []):
        raise ValueError('result action mismatch: requested %r (%s), got %r' % (requested, command['id'], action))
    schema.validate(record, command['result_schema'])
    return command['id']


def typed_admission_result(record, requested, commands, schema, accepted):
    """Anonymous transport refusals have no dispatched command identity."""
    codes = {'malformed-json', 'invalid-utf8', 'request-too-large', 'repeated-key',
             'duplicate-request-id', 'session-busy', 'request-history-full', 'unknown-command'}
    if accepted or record.get('action') != '' or record.get('status') != 'rejected':
        raise ValueError('invalid admission refusal')
    if record.get('error', {}).get('code') not in codes:
        raise ValueError('unknown admission refusal code')
    # system.catalog uses the common closed result envelope, without a
    # command-specific successful payload requirement. No schema is relaxed.
    schema.validate(record, commands['system.catalog']['result_schema'])
    for key in ('created', 'modified', 'deleted', 'selection_after', 'exclusions'):
        if record.get(key) != []: raise ValueError('admission refusal changed ' + key)
    # The registry omits empty optional data; normalized_params is always required.
    if record.get('data', {}) != {}: raise ValueError('admission refusal has data')
    if record.get('normalized_params') != {}: raise ValueError('admission refusal has normalized_params')
    if record.get('undo_effect') != 'none' or record.get('publication') != {'state': 'not-published', 'persisted': False}:
        raise ValueError('admission refusal published or changed history')
    if record.get('revision_before') != record.get('revision_after'):
        raise ValueError('admission refusal changed revision')
    return requested


def typed_declared_paths(name, work):
    """A declared alias allows only its lexical name and resolved local target."""
    from pathlib import Path
    root = Path(work).resolve()
    path = Path(name)
    declared = set()
    for candidate in (path, path.resolve()):
        try: declared.add(candidate.relative_to(root).as_posix())
        except ValueError: pass
    return declared


def typed_case(case_path, executable, transport, case_override=None):
    from pathlib import Path
    import queue
    import threading
    import time
    schema = typed_schema_module()
    # Keep physical grants independent from requests using system aliases.
    raw_case = case_override if case_override is not None else schema.loads(Path(case_path).read_text(encoding='utf-8'))
    parent = '/tmp' if raw_case.get('work_parent') == 'system-tmp' and os.name == 'posix' else None
    work = os.path.realpath(tempfile.mkdtemp(prefix='vacards-session-', dir=parent))
    tokens = _tokens(work, os.path.dirname(os.path.abspath(__file__)))
    tokens += (('{WORK_TMP_ALIAS}', work.replace('/private/tmp/', '/tmp/', 1)),
               ('{WORK_VAR_ALIAS}', work.replace('/private/var/', '/var/', 1)))
    case = typed_platform_case(substitute(raw_case, tokens), Path(case_path).name)
    failures, records, lines, stderr_chunks = [], [], [], []
    args = case.get('startup_args', case.get('args', []))
    prefix = [executable] if isinstance(executable, str) else list(executable)
    timeout = case.get('timeout', 30)
    deadline = time.monotonic() + timeout
    result = {'failures': failures, 'cmd': prefix, 'returncode': None, 'stdout': '', 'stderr': '', 'records': records, 'work_dir': work}
    processes = []
    context = {}
    file_case = any(s.get('request', {}).get('command', '').startswith('file.') for s in case.get('requests', []))
    env = dict(os.environ)
    if file_case:
        env['INKSCAPE_PROFILE_DIR'] = os.path.join(work, 'profile')
        env['INKSCAPE_APP_ID_TAG'] = 'vacardsagent'
        env['LC_ALL'] = 'C'
        env.pop('INKSCAPE_VACARDS_TIFF_ICC_PROFILE', None)
        if case.get('profile_override'):
            env['INKSCAPE_VACARDS_TIFF_ICC_PROFILE'] = case['profile_override']
        if case.get('fontconfig'):
            env['FONTCONFIG_FILE'] = case['fontconfig']
            env['FONTCONFIG_PATH'] = os.path.dirname(case['fontconfig'])
    try:
        if file_case:
            from pathlib import Path
            import re
            fixtures = Path(__file__).parent / 'cli_tests/vacards-agent/fixtures/m2'
            manifest = typed_file_oracles().fixture_manifest(fixtures)
            inputs = Path(work) / 'inputs'; inputs.mkdir()
            # Copy only declared inputs and their fixture-declared dependencies.
            # In particular, ordinary cases do not copy the 15 MiB limit probe.
            def strings(value):
                if isinstance(value, str): yield value
                elif isinstance(value, dict):
                    for child in value.values(): yield from strings(child)
                elif isinstance(value, list):
                    for child in value: yield from strings(child)
            pattern = re.escape(str(inputs).replace('\\','/')) + r'/([^\s"<>]+)'
            names = {name for value in strings(case) for name in re.findall(pattern, value.replace('\\','/'))}
            if 'linked.svg' in names: names.add('rgba.png')
            for entry in manifest['fixtures']:
                relative = entry['path']
                if relative in names or any(relative.startswith(name.rstrip('/') + '/') for name in names):
                    target = inputs / relative; target.parent.mkdir(parents=True, exist_ok=True)
                    shutil.copyfile(fixtures / relative, target)
            shutil.copyfile(fixtures / 'manifest.json', inputs / 'manifest.json')
        typed_setup(case.get('setup', []), work)
        initial_disk = typed_snapshot(os.path.join(work, 'inputs')) if file_case else {}
        if transport == 'one-shot':
            def shot(request, name, startup):
                if time.monotonic() >= deadline: raise TimeoutError('case timeout')
                path = os.path.join(work, name + '.json')
                with open(path, 'w', encoding='utf-8') as f: json.dump(request, f, ensure_ascii=False)
                cmd = prefix + ['--agent-request-file', path] + startup
                p = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=env)
                processes.append(p)
                out, err = p.communicate(timeout=max(0.001, deadline - time.monotonic()))
                if len(out) > 8 * 1024 * 1024: raise ValueError('one-shot response exceeds 8 MiB')
                data = out.decode('utf-8', errors='strict').splitlines()
                if len(data) != 1: raise ValueError('one-shot stdout must contain exactly one result')
                return schema.loads(data[0]), p.returncode, out.decode('utf-8'), err.decode('utf-8')
            probe, rc, _, probe_stderr = shot({'schema': 'va-studio.cli-request/1', 'id': '__p9_catalog', 'command': 'system.catalog', 'params': {}}, 'catalog', [])
            if rc != 0: failures.append('catalog probe exit ' + str(rc))
            commands = schema.catalog_schemas(probe['data'])
            typed_result_command(probe, 'system.catalog', commands, schema)
            steps = case.get('requests', [])
            if len(steps) != 1: raise ValueError('one-shot case requires exactly one request')
            step = steps[0]
            record, rc, out, err = shot(step['request'], 'request', args)
            result.update(cmd=prefix + ['--agent-request-file', os.path.join(work, 'request.json')] + args, returncode=rc, stdout=out, stderr=err)
            records.append(record)
            command = step['request']['command']
            typed_result_command(record, command, commands, schema)
            if record.get('id') != step['request']['id']: failures.append('one-shot result id mismatch')
            if record.get('seq') != 1: failures.append('one-shot terminal seq must be 1')
            typed_expect(record, step, failures, 'result')
            if command.startswith('file.'):
                typed_file_oracles().command_result(record, step['request'])
            typed_disk_expect(step, record, failures, 'result')
        else:
            cmd = prefix + ['--agent-session'] + args
            result['cmd'] = cmd
            p = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=env)
            processes.append(p)
            events = queue.Queue()
            def read_stdout():
                try:
                    while True:
                        raw = p.stdout.readline(8 * 1024 * 1024 + 1)
                        if not raw: break
                        events.put((time.monotonic(), raw))
                finally: events.put((time.monotonic(), None))
            def read_stderr():
                while True:
                    raw = p.stderr.read(4096)
                    if not raw: break
                    stderr_chunks.append(raw)
            readers = [threading.Thread(target=read_stdout, daemon=True), threading.Thread(target=read_stderr, daemon=True)]
            for t in readers: t.start()
            seq, terminal_seq = 0, 0
            pending, terminals, accepted = [], {}, set()
            commands = None
            hello = None
            eof = False
            def receive():
                nonlocal seq, terminal_seq, commands, hello, eof
                if time.monotonic() >= deadline: raise TimeoutError('case timeout')
                arrival, raw = events.get(timeout=max(0.001, deadline - time.monotonic()))
                if raw is None:
                    eof = True
                    return None
                if len(raw) > 8 * 1024 * 1024: raise ValueError('response exceeds 8 MiB')
                text_line = raw.decode('utf-8', errors='strict')
                lines.append(text_line)
                if not text_line.endswith('\n'): raise ValueError('unterminated stdout envelope')
                envelope = schema.loads(text_line)
                if not isinstance(envelope, dict): raise ValueError('envelope is not an object')
                if envelope.get('schema') != 'va-studio.cli-session/1': raise ValueError('invalid envelope schema')
                event = envelope.get('event')
                if event not in ('hello', 'accepted', 'progress', 'result'): raise ValueError('unknown envelope event')
                event_seq = envelope.get('event_seq')
                if type(event_seq) is not int or event_seq <= seq: raise ValueError('non-monotonic event_seq')
                seq = event_seq
                if hello is None:
                    if event != 'hello': raise ValueError('hello must be first')
                    # The session protocol documents the precise field layout. These
                    # are the required semantic metadata for the M1 wire contract.
                    for key in ('session_id', 'protocol', 'catalog_version', 'catalog_hash', 'product', 'build', 'source_sha', 'capabilities', 'limits', 'document'):
                        if key not in envelope: raise ValueError('hello missing ' + key)
                    for key in ('session_id', 'catalog_hash', 'product', 'source_sha'):
                        if not isinstance(envelope[key], str) or not envelope[key]: raise ValueError('hello invalid ' + key)
                    if envelope['protocol'] != 'va-studio.cli-session/1' or envelope['catalog_version'] != 'va-studio.cli-catalog/1': raise ValueError('hello protocol/catalog version')
                    if not isinstance(envelope['build'], (str, int, dict)) or isinstance(envelope['build'], bool): raise ValueError('hello invalid build')
                    if not isinstance(envelope['capabilities'], dict) or not isinstance(envelope['limits'], dict) or not envelope['limits']: raise ValueError('hello capabilities/limits')
                    if envelope['document'] is not None and not isinstance(envelope['document'], dict): raise ValueError('hello document')
                    if 'startup_error' in envelope:
                        e = envelope['startup_error']
                        if not all(isinstance(e.get(k), str) and e[k].strip() for k in ('code', 'message', 'hint')): raise ValueError('invalid startup error')
                    import re
                    if not re.fullmatch('[0-9a-f]{64}', envelope['catalog_hash']): raise ValueError('invalid catalog SHA-256')
                    # Git builds report the full 40-hex SHA; an archive build without .git sets
                    # VACARDS_EXPECTED_SOURCE_SHA (for example "unknown") so the check stays exact.
                    expected_sha = os.environ.get('VACARDS_EXPECTED_SOURCE_SHA')
                    if expected_sha is not None:
                        if envelope['source_sha'] != expected_sha: raise ValueError('source SHA differs from VACARDS_EXPECTED_SOURCE_SHA')
                    elif not re.fullmatch('[0-9a-f]{40}', envelope['source_sha']): raise ValueError('invalid source SHA')
                    if 'M1' not in envelope['capabilities'].get('enabled_slices', []): raise ValueError('hello does not advertise M1')
                    for key in ('request_bytes', 'response_bytes'):
                        if type(envelope['limits'].get(key)) is not int or envelope['limits'][key] < 1: raise ValueError('invalid effective limit ' + key)
                    if envelope['limits']['request_bytes'] > 1048576 or envelope['limits']['response_bytes'] > 8388608: raise ValueError('hello exceeds protocol ceilings')
                    hello = envelope
                    context['hello'] = hello
                    typed_expect(hello, {'expect_nonempty': case.get('expect_hello_nonempty', [])}, failures, 'hello')
                    match(case.get('expect_hello', {}), hello, 'hello', failures)
                    return envelope
                if event == 'hello': raise ValueError('duplicate hello')
                rid = envelope.get('id')
                if rid is not None and not isinstance(rid, str): raise ValueError('invalid envelope request id')
                if event == 'accepted':
                    choices = [i for i, step in enumerate(pending) if step['_id'] == rid and i not in accepted and i not in terminals]
                    if not choices: raise ValueError('unexpected/duplicate accepted')
                    i = choices[0]
                    accepted.add(i)
                elif event == 'progress':
                    choices = [i for i in accepted if pending[i]['_id'] == rid and i not in terminals]
                    if not choices: raise ValueError('progress without an active accepted request')
                    progress = envelope.get('progress', envelope)
                    for key in ('phase', 'phase_label'):
                        if not isinstance(progress.get(key), str) or not progress[key]: raise ValueError('invalid progress ' + key)
                    percent = progress.get('phase_percent')
                    if not schema.number(percent) or not 0 <= percent <= 100: raise ValueError('invalid phase percentage')
                elif event == 'result':
                    choices = [i for i, step in enumerate(pending) if step['_id'] == rid and i not in terminals]
                    if not choices: raise ValueError('duplicate/unrequested terminal')
                    i = choices[0]
                    step = pending[i]
                    record = envelope.get('result')
                    if not isinstance(record, dict): raise ValueError('terminal missing result object')
                    if record.get('id') != rid: raise ValueError('terminal/envelope id mismatch')
                    terminal_seq += 1
                    if record.get('seq') != terminal_seq: raise ValueError('terminal seq is not consecutive')
                    terminals[i] = (record, arrival)
                    records.append(record)
                    if step.get('_catalog'):
                        commands = schema.catalog_schemas(record['data'])
                        if hello['catalog_hash'] != record['data'].get('hash'): raise ValueError('hello/catalog hash mismatch')
                    if commands is None: raise ValueError('result before catalog')
                    requested = step.get('request', {}).get('command', step.get('command'))
                    if record.get("action") == "":
                        command = typed_admission_result(record, requested, commands, schema, i in accepted)
                    else:
                        command = typed_result_command(record, requested, commands, schema)
                    if record.get('status') in ('rejected', 'cancelled', 'failed', 'uncertain'):
                        error = record.get('error', {})
                        if not all(isinstance(error.get(k), str) and error[k].strip() for k in ('code', 'message', 'hint')): raise ValueError('terminal error needs code/message/hint')
                    if command.startswith(('system.', 'session.')) and record.get('revision_before') != record.get('revision_after'):
                        failures.append('system/session command changed document revision')
                    typed_expect(record, step, failures, 'request[%d]' % i)
                    if command.startswith('file.'):
                        typed_file_oracles().command_result(record, step['request'])
                    typed_disk_expect(step, record, failures, 'request[%d]' % i)
                    context[rid] = record
                return envelope
            def send(step):
                if time.monotonic() >= deadline: raise TimeoutError('case timeout')
                step = typed_refs(dict(step), context)
                typed_setup(step.get('before', []), work)
                if 'raw_hex' in step:
                    raw = bytes.fromhex(step['raw_hex'])
                    step['_id'] = step.get('id')
                elif 'raw' in step:
                    raw = step['raw'].encode('utf-8')
                    step['_id'] = step.get('id')
                elif 'raw_repeat' in step:
                    raw = (step.get('raw_prefix', '') + step['raw_repeat']['text'] * step['raw_repeat']['count']).encode('utf-8')
                    step['_id'] = step.get('id')
                else:
                    raw = json.dumps(step['request'], ensure_ascii=False, separators=(',', ':')).encode('utf-8')
                    step['_id'] = step['request']['id']
                step['_sent'] = time.monotonic()
                pending.append(step)
                raw += case.get('line_ending', '\n').encode('ascii')
                fragment = step.get('fragment_bytes', len(raw))
                if type(fragment) is not int or fragment < 1: raise ValueError('invalid fragment_bytes')
                sent = threading.Event()
                write_errors = []
                def write_request():
                    try:
                        for pos in range(0, len(raw), fragment):
                            p.stdin.write(raw[pos:pos + fragment]); p.stdin.flush()
                    except Exception as error: write_errors.append(error)
                    finally:
                        step['_sent'] = time.monotonic()
                        sent.set()
                writer = threading.Thread(target=write_request, daemon=True)
                writer.start()
                if not sent.wait(max(0.001, deadline - time.monotonic())): raise TimeoutError('timeout writing request')
                if write_errors: raise write_errors[0]
                return len(pending) - 1
            def settle(indices):
                while any(i not in terminals for i in indices):
                    if eof: raise ValueError('missing terminal before EOF')
                    receive()
            receive()
            probe = {'request': {'schema': 'va-studio.cli-request/1', 'id': '__p9_catalog', 'command': 'system.catalog', 'params': {}}, '_catalog': True}
            settle([send(probe)])
            steps = case.get('requests', [])
            if not steps: raise ValueError('session case has zero requests')
            batch = []
            for n, step in enumerate(steps):
                if step.get('concurrent'):
                    if not batch: raise ValueError('concurrent request has no predecessor')
                elif batch:
                    settle(batch); batch = []
                index = send(step)
                if step.get('concurrent'):
                    pending[index]['_predecessor'] = index - 1
                batch.append(index)
            settle(batch)
            p.stdin.close()
            while not eof: receive()
            p.wait(timeout=max(0.001, deadline - time.monotonic()))
            for t in readers: t.join(timeout=max(0.001, deadline - time.monotonic()))
            for i, step in enumerate(pending):
                if i not in terminals: failures.append('request[%d]: missing terminal' % i)
                expected_accept = step.get('expect_accepted', 'raw' not in step and 'raw_repeat' not in step and 'raw_hex' not in step)
                if (i in accepted) != expected_accept: failures.append('request[%d]: accepted cardinality mismatch' % i)
                if '_predecessor' in step:
                    previous = terminals.get(step['_predecessor'])
                    if previous and previous[1] <= step['_sent']: failures.append('concurrent request sent after previous terminal arrived')
            result['returncode'] = p.returncode
            result['stdout'] = ''.join(lines)
            result['stderr'] = b''.join(stderr_chunks).decode('utf-8', errors='strict')
        if time.monotonic() >= deadline: raise TimeoutError('case timeout')
        if result['returncode'] != case.get('expect_exit', 0): failures.append('exit code: expected %s, got %s' % (case.get('expect_exit', 0), result['returncode']))
        check_contains('expect_stderr_contains', case.get('expect_stderr_contains', []), result['stderr'], failures)
        check_not_contains('expect_stderr_not_contains', case.get('expect_stderr_not_contains', []), result['stderr'], failures)
        if 'expect_stderr' in case and result['stderr'] != case['expect_stderr']: failures.append('stderr exact mismatch')
        typed_disk_expect(case, records[-1] if records else {}, failures, 'final')
        if file_case and case.get('preserve_inputs', True) and typed_snapshot(os.path.join(work, 'inputs')) != initial_disk:
            failures.append('input fixture filesystem changed')
        if file_case and case.get('unchanged_profile', True):
            from pathlib import Path
            profile = Path(work) / 'profile'
            if any(p.is_file() for p in profile.rglob('*')):
                failures.append('CLI persisted preferences/recent/autosave profile files')
        if file_case:
            from pathlib import Path
            allowed = {'catalog.json','request.json'}
            declarations = [case] + case.get('requests', [])
            for declaration in declarations:
                for action in declaration.get('setup', []) + declaration.get('before', []):
                    allowed.update(typed_declared_paths(action['path'], work))
                for spec in declaration.get('file_oracles', []):
                    if spec.get('absent'): continue
                    allowed.update(typed_declared_paths(spec['path'], work))
            for path in Path(work).rglob('*'):
                rel = path.relative_to(work).as_posix()
                if not (path.is_file() or path.is_symlink()) or rel.startswith(('inputs/', 'font-cache/')): continue
                if rel not in allowed: failures.append('unexpected output/autosave/multipage file: ' + rel)
    except (Exception,) as error:
        if not processes and not records and typed_symlink_skip(case, Path(case_path).name, error): result['skip'] = WINDOWS_SYMLINK_SKIP
        else: failures.append('timeout waiting for transport' if type(error).__name__ in ('Empty', 'TimeoutExpired', 'TimeoutError') else 'transport runner error: %s: %s' % (type(error).__name__, error))
    finally:
        for p in processes:
            if p.poll() is None:
                p.kill()  # exact child PID only; never a process pattern
                p.wait()
        if processes and result['returncode'] is None:
            result['returncode'] = processes[-1].returncode
        if not result['stdout']: result['stdout'] = ''.join(lines)
        if not result['stderr']: result['stderr'] = b''.join(stderr_chunks).decode('utf-8', errors='replace')
    return result


def typed_main(argv):
    if len(argv) != 5 or argv[1] != '--transport' or argv[2] not in ('session', 'one-shot'):
        print('Usage: %s --transport session|one-shot VASTUDIO_CLI CASE_DIR' % argv[0], file=sys.stderr)
        return 2
    executable, case_dir = argv[3:]
    if not os.path.isfile(executable):
        print('SKIP: vastudio-cli missing: ' + executable); return 77
    paths = sorted(os.path.join(case_dir, x) for x in os.listdir(case_dir) if x.endswith('.json')) if os.path.isdir(case_dir) else []
    if not paths:
        print('FAIL: zero session cases in ' + case_dir); return 1
    failed = skipped = 0
    for path in paths:
        try:
            with open(path, encoding='utf-8') as handle: bundle = typed_schema_module().loads(handle.read())
            mode = bundle.get('transport', argv[2])
            if mode not in ('session', 'one-shot'): raise ValueError('invalid case transport')
            if bundle.get('schema') == 'p9-m3-cases/1':
                children = bundle.get('cases', [])
                if not children or len({c['id'] for c in children}) != len(children): raise ValueError('zero/duplicate M3 case IDs')
                child_failures = []
                for child in children:
                    child_result = typed_case(path, executable, mode, case_override=child)
                    if child_result.get('skip'): child_result['failures'].append('required M3 case skipped')
                    child_failures.extend(child['id'] + ': ' + e for e in child_result['failures'])
                    print(('M3-FAIL ' if child_result['failures'] else 'M3-PASS ') + child['id'])
                    shutil.rmtree(child_result['work_dir'])
                result = dict(failures=child_failures, cmd=[executable], stdout='', stderr='', work_dir='(retained by CTest)')
            else:
                result = typed_case(path, executable, mode)
        except Exception as error:
            failed += 1
            print('FAIL ' + os.path.basename(path) + ': runner error: ' + str(error))
            continue
        name = os.path.basename(path)
        if result.get('skip'):
            skipped += 1
            print('SKIP ' + name + ': ' + result['skip'])
            shutil.rmtree(result['work_dir'])
        elif result['failures']:
            failed += 1
            print('FAIL ' + name + ': ' + '; '.join(result['failures']))
            print('  command: ' + ' '.join(result['cmd']))
            print('  stdout tail: ' + result['stdout'][-TAIL_CHARS:])
            print('  stderr tail: ' + result['stderr'][-TAIL_CHARS:])
            print('  work dir: ' + result['work_dir'])
        else:
            print('PASS ' + name)
            if result['work_dir'] != '(retained by CTest)': shutil.rmtree(result['work_dir'])
    if skipped: print('CASES: %d passed, %d failed, %d skipped, %d total' % (len(paths) - failed - skipped, failed, skipped, len(paths)))
    else: print('CASES: %d passed, %d failed, %d total' % (len(paths) - failed, failed, len(paths)))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(typed_main(sys.argv) if len(sys.argv) > 1 and sys.argv[1] == "--transport" else main(sys.argv))
