#!/usr/bin/env node
// SPDX-License-Identifier: GPL-2.0-or-later
// Extracts the exact bytes of <script id="engine"> from the simulator HTML into sim-engine.js.
// Usage: node extract-engine.cjs <explode-bitmap-sim.html> [out sim-engine.js]
'use strict';
const fs = require('node:fs'), path = require('node:path'), crypto = require('node:crypto');
const sha = b => crypto.createHash('sha256').update(b).digest('hex');
function extract(htmlBytes) {
  const open = Buffer.from('<script id="engine">'), close = Buffer.from('</script>');
  const a = htmlBytes.indexOf(open);
  if (a < 0 || htmlBytes.indexOf(open, a + 1) >= 0) throw new Error('script#engine not found exactly once');
  const start = a + open.length, end = htmlBytes.indexOf(close, start);
  if (end < 0) throw new Error('unterminated script#engine');
  return htmlBytes.subarray(start, end);
}
module.exports = { extract, sha };
if (require.main === module) {
  const src = process.argv[2];
  if (!src) { console.error('usage: extract-engine.cjs <sim.html> [out]'); process.exit(2); }
  const out = process.argv[3] || path.join(__dirname, 'sim-engine.js');
  const html = fs.readFileSync(src), eng = extract(html);
  fs.writeFileSync(out, eng);
  console.log(JSON.stringify({ html_sha256: sha(html), engine_sha256: sha(eng), bytes: eng.length }));
}
