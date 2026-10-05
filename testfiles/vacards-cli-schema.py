#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Independent, fail-closed validator for the registry's 2020-12 subset.

Usage: vacards-cli-schema.py CATALOG.json
No external packages. maxBytes and x-unit are the registry's explicit extensions.
"""
import json
import math
import sys

DRAFT = 'https://json-schema.org/draft/2020-12/schema'
KEYWORDS = frozenset(('$schema', 'type', 'properties', 'required', 'additionalProperties',
                      'items', 'enum', 'const', 'minimum', 'maximum', 'minLength',
                      'maxLength', 'maxBytes', 'minItems', 'maxItems', 'anyOf', 'allOf',
                      'if', 'then', 'else', 'oneOf', 'not', 'uniqueItems',
                      'exclusiveMinimum', 'exclusiveMaximum', 'pattern',
                      'description', 'default', 'x-unit', 'x-m3-contract', 'x-exact-uint64',
                      'propertyNames', 'minProperties', 'maxProperties',
                      'x-css-px-absolute-maximum', 'x-nonnegative-css-px'))
TYPES = frozenset(('object', 'array', 'string', 'number', 'integer', 'boolean', 'null'))

class SchemaError(ValueError):
    """Invalid or unsupported schema (never an instance mismatch)."""

class ValidationError(ValueError):
    """Instance violates a supported schema."""

def loads(text):
    def pairs(items):
        out = {}
        for key, value in items:
            if key in out:
                raise ValueError('duplicate JSON key: ' + key)
            out[key] = value
        return out
    def constant(value):
        raise ValueError('nonfinite JSON number: ' + value)
    value = json.loads(text, object_pairs_hook=pairs, parse_constant=constant)
    def finite(node):
        if isinstance(node, float) and not math.isfinite(node):
            raise ValueError('nonfinite JSON number')
        if isinstance(node, dict):
            for child in node.values(): finite(child)
        if isinstance(node, list):
            for child in node: finite(child)
    finite(value)
    return value

def number(value):
    return type(value) is int or (type(value) is float and math.isfinite(value))

def equal(a, b):
    if number(a) and number(b): return a == b
    if type(a) is not type(b): return False
    if isinstance(a, dict): return a.keys() == b.keys() and all(equal(a[k], b[k]) for k in a)
    if isinstance(a, list): return len(a) == len(b) and all(equal(x, y) for x, y in zip(a, b))
    return a == b

def check_schema(schema, path='$'):
    if not isinstance(schema, dict): raise SchemaError(path + ': schema must be an object')
    unknown = set(schema) - KEYWORDS
    if unknown: raise SchemaError(path + ': unknown keyword(s): ' + ', '.join(sorted(unknown)))
    def bad(message): raise SchemaError(path + ': ' + message)
    if '$schema' in schema and schema['$schema'] != DRAFT: bad('unsupported draft')
    if 'type' in schema:
        kinds = schema['type'] if isinstance(schema['type'], list) else [schema['type']]
        if not kinds or any(not isinstance(k, str) or k not in TYPES for k in kinds) or len(set(kinds)) != len(kinds): bad('unsupported type')
    if 'x-exact-uint64' in schema and schema['x-exact-uint64'] is not True: bad('invalid exact uint64 annotation')
    if 'x-m3-contract' in schema and not isinstance(schema['x-m3-contract'], dict): bad('invalid M3 metadata')
    for key in ('description', 'x-unit'):
        if key in schema and not isinstance(schema[key], str): bad(key + ' must be a string')
    for key in ('minimum', 'maximum', 'exclusiveMinimum', 'exclusiveMaximum'):
        if key in schema and not number(schema[key]): bad(key + ' must be finite numeric')
    for key in ('minLength', 'maxLength', 'minItems', 'maxItems', 'maxBytes', 'minProperties', 'maxProperties'):
        if key in schema and (type(schema[key]) is not int or schema[key] < 0): bad(key + ' must be a nonnegative integer')
    if 'required' in schema:
        r = schema['required']
        if not isinstance(r, list) or any(not isinstance(k, str) for k in r) or len(set(r)) != len(r): bad('invalid required')
    if 'enum' in schema:
        e = schema['enum']
        if not isinstance(e, list) or not e: bad('enum must be nonempty')
        if any(equal(x, y) for i, x in enumerate(e) for y in e[:i]): bad('duplicate enum value')
    if 'properties' in schema:
        if not isinstance(schema['properties'], dict): bad('properties must be an object')
        for key, child in schema['properties'].items(): check_schema(child, path + '/properties/' + key)
    if 'uniqueItems' in schema and type(schema['uniqueItems']) is not bool: bad('uniqueItems must be boolean')
    if 'pattern' in schema:
        import re
        if not isinstance(schema['pattern'], str): bad('pattern must be string')
        try: re.compile(schema['pattern'])
        except re.error: bad('invalid pattern')
    for key in ('items', 'if', 'then', 'else', 'not', 'propertyNames'):
        if key in schema: check_schema(schema[key], path + '/' + key)
    if 'additionalProperties' in schema and type(schema['additionalProperties']) is not bool:
        check_schema(schema['additionalProperties'], path + '/additionalProperties')
    for key in ('anyOf', 'allOf', 'oneOf'):
        if key in schema:
            if not isinstance(schema[key], list) or not schema[key]: bad(key + ' must be a nonempty schema array')
            for i, child in enumerate(schema[key]): check_schema(child, path + '/' + key + '/' + str(i))
    return schema

def validate(instance, schema, path='$'):
    check_schema(schema)
    _validate(instance, schema, path)
    return instance

def _validate(v, s, path):
    def bad(message): raise ValidationError(path + ': ' + message)
    kind = s.get('type')
    valid = {'object': isinstance(v, dict), 'array': isinstance(v, list), 'string': isinstance(v, str),
             'number': number(v), 'integer': number(v) and v == int(v),
             'boolean': type(v) is bool, 'null': v is None}
    if kind and not any(valid[k] for k in (kind if isinstance(kind, list) else [kind])): bad('expected ' + str(kind))
    if s.get('x-exact-uint64') and (type(v) is not int or not 0 <= v <= 18446744073709551615): bad('expected exact uint64 integer token')
    if 'const' in s and not equal(v, s['const']): bad('const mismatch')
    if 'enum' in s and not any(equal(v, x) for x in s['enum']): bad('enum mismatch')
    if number(v):
        if 'minimum' in s and v < s['minimum']: bad('below minimum')
        if 'maximum' in s and v > s['maximum']: bad('above maximum')
        if 'exclusiveMinimum' in s and v <= s['exclusiveMinimum']: bad('below exclusive minimum')
        if 'exclusiveMaximum' in s and v >= s['exclusiveMaximum']: bad('above exclusive maximum')
    if isinstance(v, str):
        if 'pattern' in s:
            import re
            if not re.search(s['pattern'], v): bad('pattern mismatch')
        for k, length in (('minLength', len(v)), ('maxLength', len(v)), ('maxBytes', len(v.encode('utf-8')))):
            if k in s and ((k == 'minLength' and length < s[k]) or (k != 'minLength' and length > s[k])): bad(k + ' violated')
    if isinstance(v, list):
        if s.get('uniqueItems') and any(equal(x,y) for i,x in enumerate(v) for y in v[:i]): bad('duplicate array item')
        if 'minItems' in s and len(v) < s['minItems']: bad('below minItems')
        if 'maxItems' in s and len(v) > s['maxItems']: bad('above maxItems')
        if 'items' in s:
            for i, child in enumerate(v): _validate(child, s['items'], path + '/' + str(i))
    if isinstance(v, dict):
        if 'x-css-px-absolute-maximum' in s or s.get('x-nonnegative-css-px'):
            factors = {'px':1, 'mm':96/25.4, 'cm':96/2.54, 'in':96, 'pt':96/72, 'pc':16}
            if v.get('unit') not in factors or not number(v.get('value')): bad('invalid CSS length')
            converted = v['value'] * factors[v['unit']]
            if not math.isfinite(converted) or abs(converted) > s.get('x-css-px-absolute-maximum', math.inf): bad('CSS px limit')
            if s.get('x-nonnegative-css-px') and converted < 0: bad('negative CSS px')
        if 'minProperties' in s and len(v) < s['minProperties']: bad('below minProperties')
        if 'maxProperties' in s and len(v) > s['maxProperties']: bad('above maxProperties')
        if 'propertyNames' in s:
            for k in v: _validate(k, s['propertyNames'], path + '/key')
        for k in s.get('required', []):
            if k not in v: bad('missing required ' + k)
        props = s.get('properties', {})
        for k, child in v.items():
            if k in props: _validate(child, props[k], path + '/' + k)
            elif s.get('additionalProperties') is False: bad('unknown property ' + k)
            elif isinstance(s.get('additionalProperties'), dict): _validate(child, s['additionalProperties'], path + '/' + k)
    for child in s.get('allOf', []): _validate(v, child, path)
    if 'anyOf' in s:
        for child in s['anyOf']:
            try: _validate(v, child, path)
            except ValidationError: continue
            break
        else: bad('no anyOf alternative matched')
    if 'oneOf' in s:
        matches = 0
        for child in s['oneOf']:
            try: _validate(v, child, path)
            except ValidationError: pass
            else: matches += 1
        if matches != 1: bad('oneOf requires exactly one matching alternative')
    if 'not' in s:
        try: _validate(v, s['not'], path)
        except ValidationError: pass
        else: bad('not schema matched')
    if 'if' in s:
        try: _validate(v, s['if'], path)
        except ValidationError:
            if 'else' in s: _validate(v, s['else'], path)
        else:
            if 'then' in s: _validate(v, s['then'], path)

def catalog_schemas(catalog):
    if catalog.get('schema') != 'va-studio.cli-catalog/1' or not catalog.get('commands'):
        raise SchemaError('missing/empty catalog')
    out = {}
    for command in catalog['commands']:
        cid = command['id']
        if cid in out: raise SchemaError('duplicate command ' + cid)
        check_schema(command['request_schema'])
        check_schema(command['result_schema'])
        validate(command['example'], command['request_schema'])
        out[cid] = command
    return out

def main(argv):
    if len(argv) != 2:
        print('Usage: vacards-cli-schema.py CATALOG.json', file=sys.stderr); return 2
    try:
        with open(argv[1], encoding='utf-8') as f: commands = catalog_schemas(loads(f.read()))
        print('PASS: %d example requests; %d request schemas; %d result schemas' % (len(commands), len(commands), len(commands)))
        return 0
    except (ValueError, KeyError, OSError, TypeError) as error:
        print('ERROR: ' + str(error), file=sys.stderr); return 1

if __name__ == '__main__': sys.exit(main(sys.argv))
