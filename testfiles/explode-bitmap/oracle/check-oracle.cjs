#!/usr/bin/env node
// SPDX-License-Identifier: GPL-2.0-or-later
// Verifies: pinned engine hash (and match with the simulator HTML when present), two independent
// regenerations are byte-identical and equal the committed fixtures/oracle JSON, CLI == library.
// Usage: node check-oracle.cjs [path/to/explode-bitmap-sim.html]
'use strict';
const fs = require('node:fs'), os = require('node:os'), path = require('node:path'), cp = require('node:child_process'), assert = require('node:assert/strict');
const { extract, sha } = require('./extract-engine.cjs');
const { build, PARAMS } = require('../fixtures/generate-fixtures.cjs');
const PIN = { html: '8e5d7405fda19957adb55fa682523a6c852e70c11c5947dbd9304be619132e3a', engine: '7a9ce306fee5e83241a672a082669a95a8213b21b76e93358a2f54f4012704b4' };
const html = process.argv[2] || path.resolve(__dirname, '../../../../work/explode-bitmap/sim/explode-bitmap-sim.html');
const eng = fs.readFileSync(path.join(__dirname, 'sim-engine.js'));
assert.equal(sha(eng), PIN.engine, 'sim-engine.js hash differs from pin');
if (fs.existsSync(html)) { const h = fs.readFileSync(html); assert.equal(sha(h), PIN.html, 'simulator HTML hash differs from pin'); assert(extract(h).equals(eng), 'engine bytes differ from script#engine'); console.log('PASS engine == script#engine of pinned HTML'); }
else console.log('SKIP html not found; engine hash pin checked only');
const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'eb-oracle-'));
try {
  const a = build(path.join(tmp, 'a')), b = build(path.join(tmp, 'b'));
  const dir = path.join(__dirname, '../fixtures');
  let n = 0;
  for (const f of Object.keys(a)) { assert(a[f].equals ? a[f].equals(b[f]) : a[f] === b[f], 'nondeterministic ' + f); assert.equal(sha(Buffer.from(a[f])), sha(fs.readFileSync(path.join(dir, f))), 'committed file differs ' + f); n++; }
  console.log(`PASS ${n} files regenerated twice: identical, and equal to committed`);
  for (const [mode, file, count] of [
    ['--label-suite', 'label-regions.json', 184],
    ['--enclosure-suite', 'enclosure-regions.json', 368],
  ]) {
    const regenerate = () => {
      const r = cp.spawnSync(process.execPath, [path.join(__dirname, 'export-oracle.cjs'), mode, path.join(dir, 'pam')], { maxBuffer: 16 * 1024 * 1024 });
      assert.equal(r.status, 0, r.stderr && r.stderr.toString());
      return r.stdout;
    };
    const first = regenerate(), second = regenerate();
    assert(first.equals(second), 'nondeterministic ' + file);
    assert(first.equals(fs.readFileSync(path.join(__dirname, file))), 'committed file differs ' + file);
    assert.equal(JSON.parse(first).length, count, 'entry count differs ' + file);
    console.log(`PASS ${file}: ${count} entries regenerated twice, byte-identical and equal to committed`);
  }
  const r = cp.spawnSync(process.execPath, [path.join(__dirname, 'export-oracle.cjs'), '--T', '128', '--S', '40', '--dpi', '300', path.join(dir, 'pam/far_blocks.pam')], { encoding: 'utf8' });
  assert.equal(r.status, 0, r.stderr); assert.equal(r.stdout, a['expected/far_blocks.T128_S40_dpi300.json']); console.log('PASS CLI output equals library output');
  assert(PARAMS.some(q => q[2] === 25.4), 'missing 25.4 dpi params'); assert(a['expected/far_blocks.T128_S0_dpi25.4.json'], 'missing 25.4 dpi file');
  const exact = 300 / 25.4 * (25.4 / 300); assert.equal(exact, 1); assert.equal(2 * exact * exact, 2); console.log('PASS 25.4 dpi float expression is exactly reach 1 px, speck limit 2 px2');
  const j = JSON.parse(a['expected/far_blocks.T128_S40_dpi300.json']), { sha256, ...body } = j;
  assert.equal(sha(Buffer.from(JSON.stringify(body))), sha256); console.log('PASS embedded sha256 verifies');
} finally { fs.rmSync(tmp, { recursive: true, force: true }); }
