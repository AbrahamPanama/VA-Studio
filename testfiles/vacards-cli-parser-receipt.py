#!/usr/bin/env python3
"""Writer and validator for parser-open interception receipts (p9-parser-opens/2)."""
import hashlib
import json
import re
from pathlib import Path

SCHEMA = 'p9-parser-opens/2'
CONTROL_CASES = {
    'InterceptorCoverageControls', 'HookAbsentIsNotObserved', 'NoUngrantedParserOpens',
    'OutsideGrantSvg', 'OutsideGrantCdr', 'OutsideGrantPdf', 'OutsideGrantImage',
    'SvgExternalResource',
}
REQUIRED_CONTROL_PATHS = {
    'control.txt': 'granted', 'control-libxml.xml': 'ungranted',
    'outside.icc': 'ungranted', 'control-gdk.png': 'ungranted',
    'control-poppler.pdf': 'ungranted', 'control-librevenge.cdr': 'ungranted',
    'control-ifstream.txt': 'ungranted', 'control-glib.txt': 'ungranted',
}
PROCESS_NETWORK_KINDS = {'spawn', 'exec', 'fork', 'process', 'network', 'connect'}
EVENT_RE = re.compile(r'^P9-OPEN-EVENT (.+)$')
CASE_RE = re.compile(r'^P9-PARSER-OPEN-CASE (.+)$')
EXECUTABLE_RE = re.compile(r'^P9-AUDIT-EXECUTABLE (.+)$')


def sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            h.update(block)
    return h.hexdigest()


def fields(raw):
    result = {}
    for item in raw.split():
        if '=' not in item:
            raise ValueError('malformed parser audit field: ' + item)
        key, value = item.split('=', 1)
        if key in result:
            raise ValueError('duplicate parser audit field: ' + key)
        result[key] = value
    return result


def extract(log_path):
    cases, events, executables = [], [], {}
    for raw in Path(log_path).read_text(errors='replace').splitlines():
        line = re.sub(r'^\d+: ', '', raw)
        match = CASE_RE.match(line)
        if match:
            row = fields(match.group(1))
            if set(row) - {'id', 'events', 'hook', 'observed'} or 'id' not in row or 'events' not in row:
                raise ValueError('invalid P9-PARSER-OPEN-CASE record')
            try:
                count = int(row['events'])
            except ValueError as error:
                raise ValueError('invalid parser case event count') from error
            cases.append({'id': row['id'], 'events': count})
            continue
        match = EXECUTABLE_RE.match(line)
        if match:
            row = fields(match.group(1))
            if set(row) != {'case', 'path'} or not row['path'].startswith('/'):
                raise ValueError('invalid P9-AUDIT-EXECUTABLE record')
            path = re.sub(r'%([0-9A-Fa-f]{2})', lambda m: chr(int(m.group(1), 16)), row['path'])
            if row['case'] in executables:
                raise ValueError('duplicate parser executable record')
            executables[row['case']] = path
            continue
        match = EVENT_RE.match(line)
        if match:
            row = fields(match.group(1))
            required = {'case', 'kind', 'api', 'path', 'access', 'result', 'caller', 'classification'}
            if set(row) != required:
                raise ValueError('invalid P9-OPEN-EVENT fields')
            events.append(row)
    if len({row['id'] for row in cases}) != len(cases):
        raise ValueError('duplicate P9 parser case record')
    return cases, events, executables


def macos_system_event_indices(events, executables):
    """Return indices matching the two narrowly identified macOS system events."""
    if not isinstance(events, list) or not isinstance(executables, dict):
        return set()
    result = set()
    readonly_forbidden = 0x1 | 0x2 | 0x200 | 0x400 | 0x8  # O_WRONLY, O_RDWR, O_CREAT, O_TRUNC, O_APPEND
    bundle_callers = {
        '/System/Library/Frameworks/CoreFoundation.framework/Versions/A/CoreFoundation',
        '/usr/lib/system/libxpc.dylib',
    }
    open_apis = {'opendir', 'opendir$INODE64', 'open', 'open$NOCANCEL'}
    for index, event in enumerate(events):
        if not isinstance(event, dict):
            continue
        try:
            access = int(event.get('access', ''), 0)
        except (TypeError, ValueError):
            access = -1
        if (event.get('kind') == 'network' and event.get('api') == 'connect'
                and access == 1 and event.get('path') == '/var/run/syslog'
                and event.get('caller') == '/usr/lib/system/libsystem_platform.dylib'):
            result.add(index)
            continue
        executable = executables.get(event.get('case'))
        if not isinstance(executable, str) or not executable.startswith('/'):
            continue
        flags = access
        if (event.get('kind') == 'open' and event.get('api') in open_apis
                and not (flags & readonly_forbidden)
                and event.get('caller') in bundle_callers
                and event.get('path') in (executable, str(Path(executable).parent))):
            result.add(index)
    return result


def inventory_rows(path):
    rows = []
    for raw in Path(path).read_text(errors='strict').splitlines():
        if not raw or raw.startswith('#'):
            continue
        parts = raw.split('\t', 1)
        if len(parts) != 2 or not parts[1].strip():
            raise ValueError('malformed parser runtime-read inventory row')
        rows.append((parts[0].lower(), parts[1].strip()))
    return rows


def inventory_matches(path_value, rows, platform):
    normalized = path_value.replace('\\', '/').lower()
    for pattern, _reason in rows:
        if platform == 'darwin' and pattern in ('.', '/'):
            if normalized == pattern:
                return True
        elif pattern in normalized:
            return True
    return False


def canonicalization_walk_indices(events):
    """Return only relative opens forming the recorded canonicalization parent walk."""
    if not isinstance(events, list):
        return set()
    targets = [
        (index, event) for index, event in enumerate(events)
        if isinstance(event, dict)
        and event.get('case') == 'CanonicalizationOpen'
        and event.get('kind') == 'open'
        and event.get('api') == 'open'
        and event.get('path', '').startswith('/')
        and event.get('path', '').endswith('/canonicalization-probe.txt')
        and 'libinkscape_base' in event.get('caller', '')
    ]
    if len(targets) != 1:
        return set()
    _target_index, target = targets[0]
    target_path = Path(target['path'])
    expected = target_path.parent.parts[1:]
    caller = target['caller']
    case_events = [
        (index, event) for index, event in enumerate(events)
        if isinstance(event, dict) and event.get('case') == 'CanonicalizationOpen'
    ]

    def successful_directory_search(event, api):
        try:
            flags = int(event.get('access', ''), 0)
        except (TypeError, ValueError):
            return False
        # Darwin O_DIRECTORY | O_SEARCH. O_CLOEXEC may also be present.
        return (event.get('kind') == 'open' and event.get('api') == api
                and event.get('result') == '0'
                and (flags & 0x100100) == 0x100100)

    anchors = [
        (index, event) for index, event in case_events
        if event.get('caller') == caller and event.get('path') == '/'
        and successful_directory_search(event, 'open')
    ]
    if len(anchors) != 1:
        return set()
    anchor_index, _anchor = anchors[0]
    walk = [
        (index, event) for index, event in case_events
        if event.get('caller') == caller and event.get('api', '').startswith('openat')
    ]
    if not expected or len(walk) != len(expected):
        return set()
    components = []
    for index, event in walk:
        component = event.get('path', '')
        if (index <= anchor_index or not component or component in ('.', '..')
                or '/' in component or '\\' in component
                or not successful_directory_search(event, event.get('api'))):
            return set()
        components.append(component)
    if components != list(expected):
        return set()
    return {index for index, _event in walk}


def write_receipt(log_path, receipt_path, inventory_path, platform):
    cases, events, executables = extract(log_path)
    receipt = {
        'schema': SCHEMA,
        'platform': platform,
        'log': str(Path(log_path).resolve()),
        'log_sha256': sha256(log_path),
        'inventory': {'path': str(Path(inventory_path).resolve()), 'sha256': sha256(inventory_path)},
        'cases': cases,
        'executables': executables,
        'events': events,
    }
    Path(receipt_path).write_text(json.dumps(receipt, indent=2) + '\n')
    return receipt


def audit(receipt, log_path, inventory_path, platform):
    errors = []
    if not isinstance(receipt, dict) or receipt.get('schema') != SCHEMA:
        return {'passed': False, 'errors': ['wrong parser-open receipt schema']}
    if platform not in ('darwin', 'win32'):
        errors.append('unsupported parser-open receipt platform')
    if receipt.get('platform') != platform:
        errors.append('parser-open receipt platform mismatch')
    expected_inventory = 'macos-runtime-read-inventory.tsv' if platform == 'darwin' else 'windows-runtime-read-inventory.tsv'
    if Path(inventory_path).name != expected_inventory:
        errors.append('wrong platform parser runtime-read inventory')
    recorded_inventory = receipt.get('inventory', {})
    if not isinstance(recorded_inventory, dict) or recorded_inventory.get('sha256') != sha256(inventory_path):
        errors.append('parser runtime-read inventory drift')
    elif not isinstance(recorded_inventory.get('path'), str) or Path(recorded_inventory['path']).resolve() != Path(inventory_path).resolve():
        errors.append('parser receipt inventory path mismatch')
    try:
        log_cases, log_events, log_executables = extract(log_path)
        if receipt.get('log_sha256') != sha256(log_path):
            errors.append('parser audit log drift')
        if receipt.get('cases') != log_cases or receipt.get('events') != log_events or receipt.get('executables') != log_executables:
            errors.append('parser receipt events/cases differ from log-extracted records')
    except (OSError, ValueError) as error:
        errors.append('cannot extract parser audit log: ' + str(error))
        log_cases, log_events = [], []
    cases = receipt.get('cases')
    events = receipt.get('events')
    executables = receipt.get('executables')
    if not isinstance(cases, list) or not isinstance(events, list):
        errors.append('parser receipt cases/events must be arrays')
        cases, events = [], []
    if not isinstance(executables, dict) or any(not isinstance(case, str) or not isinstance(path, str) or not path.startswith('/') for case, path in executables.items()):
        errors.append('parser receipt executables must map cases to absolute paths')
        executables = {}
    valid_events = []
    required_event_fields = {'case', 'kind', 'api', 'path', 'access', 'result', 'caller', 'classification'}
    canonicalization_walk = canonicalization_walk_indices(events) if platform == 'darwin' else set()
    for index, event in enumerate(events):
        if not isinstance(event, dict) or set(event) != required_event_fields or not all(isinstance(value, str) for value in event.values()):
            errors.append('malformed parser open event')
        else:
            valid_events.append(event)
    events = valid_events
    case_map = {row.get('id'): row for row in cases if isinstance(row, dict) and isinstance(row.get('id'), str)}
    if platform == 'darwin' and (set(executables) != set(case_map) or any(not path.startswith('/') for path in executables.values())):
        errors.append('Darwin parser receipt must record one absolute executable for every case')
    required_cases = CONTROL_CASES | ({'CanonicalizationOpen'} if platform == 'darwin' else set())
    if not required_cases.issubset(case_map):
        errors.append('missing required parser-open control case(s): ' + ','.join(sorted(required_cases - set(case_map))))
    if platform == 'darwin':
        canonical_targets = [event for event in events
                             if event.get('case') == 'CanonicalizationOpen'
                             and event.get('kind') == 'open' and event.get('api') == 'open'
                             and event.get('path', '').endswith('/canonicalization-probe.txt')]
        if len(canonical_targets) != 1 or canonical_targets[0].get('classification') != 'ungranted':
            errors.append('canonicalization target open must remain classified ungranted')
    try:
        inventory = inventory_rows(inventory_path)
    except (OSError, ValueError) as error:
        errors.append('cannot read parser runtime-read inventory: ' + str(error))
        inventory = []
    for row in cases:
        if not isinstance(row, dict) or not isinstance(row.get('id'), str) or type(row.get('events')) is not int:
            errors.append('malformed parser case control')
            continue
        actual = sum(1 for event in events if isinstance(event, dict) and event.get('case') == row['id'])
        if actual != row['events']:
            errors.append('parser case event count mismatch: ' + row['id'])
    for case in required_cases:
        if case in case_map and not any(isinstance(event, dict) and event.get('case') == case for event in events):
            errors.append('parser control case has no observed events: ' + case)
    coverage = [event for event in events if isinstance(event, dict) and event.get('case') == 'InterceptorCoverageControls']
    for suffix, classification in REQUIRED_CONTROL_PATHS.items():
        if not any(event.get('path', '').endswith(suffix) and event.get('classification') == classification for event in coverage):
            errors.append('missing or misclassified parser control: ' + suffix)
    safe = [event for event in events if isinstance(event, dict) and event.get('case') == 'NoUngrantedParserOpens']
    if not any(event.get('path', '').replace('\\', '/').endswith('/granted/control.txt') and event.get('classification') == 'granted' for event in safe):
        errors.append('no-ungranted-parser-opens positive grant control missing')
    system_events = macos_system_event_indices(events, executables) if platform == 'darwin' else set()
    for index, event in enumerate(events):
        if (event.get('kind', '').lower() in PROCESS_NETWORK_KINDS or event.get('api', '').lower() in PROCESS_NETWORK_KINDS) and index not in system_events:
            errors.append('process or network event in parser receipt: ' + str(event.get('kind')) + '/' + str(event.get('api')))
        classification = event.get('classification')
        matched = inventory_matches(event.get('path', ''), inventory, platform)
        if classification == 'inventory' and not matched:
            errors.append('inventory-classified event no longer matches platform inventory: ' + str(event.get('path')))
        if matched and classification != 'inventory' and classification not in ('granted', 'ungranted'):
            errors.append('runtime-read inventory classification drift: ' + str(event.get('path')))
        if event.get('case') == 'NoUngrantedParserOpens' and classification in ('ungranted', 'unclassified'):
            errors.append('ungranted or unclassified open in no-ungranted-parser-opens case: ' + str(event.get('path')))
        if classification == 'system' and index not in system_events:
            errors.append('unmatched macOS system-event classification: ' + str(event.get('kind')) + '/' + str(event.get('api')))
        if index in system_events and classification not in ('system', 'unclassified'):
            errors.append('exact macOS system event has an unexpected classification: ' + str(classification))
        if classification not in ('granted', 'ungranted', 'inventory', 'unclassified', 'system'):
            errors.append('unknown parser event classification: ' + str(classification))
    if any(event.get('classification') == 'unclassified' and index not in canonicalization_walk and index not in system_events
           for index, event in enumerate(events) if isinstance(event, dict)):
        errors.append('unclassified parser-open event')
    return {'passed': not errors, 'errors': errors}
