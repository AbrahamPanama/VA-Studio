#!/usr/bin/env node
// SPDX-License-Identifier: GPL-2.0-or-later
// Deterministic synthetic PAM fixtures and expected oracle JSON. Fixed seeds, no randomness from the host.
// Usage: node generate-fixtures.cjs [outDir]   (default: this directory)
'use strict';
const fs = require('node:fs'), path = require('node:path');
const { writePam, parsePam, oracle } = require('../oracle/export-oracle.cjs');
const DPI = 300, PARAMS = [[128, 40, 300], [128, 0, 300], [1, 0, 300], [255, 0, 300], [128, 40, 25.4], [128, 0, 25.4]]; // 25.4 dpi: reach 1 px, speck limit 2 px^2

function mulberry32(a) { return () => { a |= 0; a = a + 0x6D2B79F5 | 0; let t = Math.imul(a ^ a >>> 15, 1 | a); t = t + Math.imul(t ^ t >>> 7, 61 | t) ^ t; return ((t ^ t >>> 14) >>> 0) / 4294967296; }; }
function canvas(w, h) {
  const d = new Uint8ClampedArray(w * h * 4);
  return { w, h, d, set(x, y, a, rgb = [200, 60, 90]) { if (x < 0 || y < 0 || x >= w || y >= h) return; const p = (y * w + x) * 4; d[p] = rgb[0]; d[p + 1] = rgb[1]; d[p + 2] = rgb[2]; d[p + 3] = a; },
    rect(x0, y0, x1, y1, a = 255, rgb) { for (let y = y0; y <= y1; y++) for (let x = x0; x <= x1; x++) this.set(x, y, a, rgb); } };
}
const F = {};
F.empty = () => canvas(16, 16);
F.opaque = () => { const c = canvas(16, 16); c.rect(0, 0, 15, 15); return c; };
F.one_pixel = () => { const c = canvas(9, 9); c.set(4, 4, 255); return c; };
F.diagonal_touch = () => { const c = canvas(24, 24); for (let i = 2; i < 20; i++) c.set(i + 1, i, 255); // 8-connected chain
  c.set(20, 3, 255); c.set(21, 2, 255); c.set(22, 1, 255); return c; };
F.diamond_hole = () => { // diagonal-touching diamond ring: 8-connected fg, bg hole is 4-connected-enclosed
  const c = canvas(32, 32), r = 8; for (let i = 0; i <= r; i++) { for (const [x, y] of [[16 - r + i, 16 - i], [16 + r - i, 16 - i], [16 - r + i, 16 + i], [16 + r - i, 16 + i]]) c.set(x, y + 0, 255); }
  c.set(16, 16, 255); return c; };
F.diamond_hole_block = () => { const c = canvas(40, 40), r = 12; for (let i = 0; i <= r; i++) for (const [x, y] of [[20 - r + i, 20 - i], [20 + r - i, 20 - i], [20 - r + i, 20 + i], [20 + r - i, 20 + i]]) c.set(x, y, 255);
  c.rect(18, 18, 22, 22); return c; };
F.checker_1px = () => { const c = canvas(16, 16); for (let y = 0; y < 16; y++) for (let x = 0; x < 16; x++) if ((x + y) & 1) c.set(x, y, 255); return c; };
F.checker_3px = () => { const c = canvas(30, 30); for (let y = 0; y < 30; y++) for (let x = 0; x < 30; x++) if (((x / 3 | 0) + (y / 3 | 0)) & 1) c.set(x, y, 255); return c; };
F.rings = () => { const c = canvas(61, 61); for (let k = 0; k < 6; k++) { const o = k * 5, i = 60 - o; for (let t = o; t <= i; t++) for (const [x, y] of [[t, o], [t, i], [o, t], [i, t]]) if (k < 5 || t === o || t === i || true) c.set(x, y, 255); } return c; };
F.rings_round = () => { const c = canvas(64, 64); for (let y = 0; y < 64; y++) for (let x = 0; x < 64; x++) { const d = Math.hypot(x - 31.5, y - 31.5); if ([[28, 31], [22, 24], [16, 18], [10, 12], [3, 6]].some(([a, b]) => d >= a && d <= b)) c.set(x, y, 255); } return c; };
F.labyrinth = () => { // seeded DFS maze, cells 2x2 with 1px walls, bordered
  const n = 12, W = 2 * n + 1, c = canvas(W + 6, W + 6), rnd = mulberry32(20260930), open = new Uint8Array(W * W), seen = new Uint8Array(n * n), st = [[0, 0]]; seen[0] = 1; open[W + 1] = 1;
  while (st.length) { const [x, y] = st[st.length - 1], nb = [[1, 0], [-1, 0], [0, 1], [0, -1]].filter(([dx, dy]) => x + dx >= 0 && y + dy >= 0 && x + dx < n && y + dy < n && !seen[(y + dy) * n + x + dx]);
    if (!nb.length) { st.pop(); continue; } const [dx, dy] = nb[rnd() * nb.length | 0]; seen[(y + dy) * n + x + dx] = 1; open[(2 * y + 1 + dy) * W + 2 * x + 1 + dx] = 1; open[(2 * (y + dy) + 1) * W + 2 * (x + dx) + 1] = 1; st.push([x + dx, y + dy]); }
  for (let y = 0; y < W; y++) for (let x = 0; x < W; x++) if (!open[y * W + x]) c.set(x + 3, y + 3, 255); return c; };
F.serpentine = () => { const c = canvas(48, 40); for (let r = 0; r < 6; r++) { c.rect(2, 2 + r * 6, 45, 3 + r * 6); if (r < 5) { const x = r & 1 ? 2 : 44; c.rect(x, 4 + r * 6, x + 1, 7 + r * 6); } } return c; };
F.specks_equidistant = () => { // tie-break: speck equidistant between two big pieces; plus mirrored vertical case
  const c = canvas(80, 60); c.rect(2, 2, 25, 25); c.rect(40, 2, 63, 25); c.rect(32, 12, 33, 13); // x gap 6 each side
  c.rect(2, 36, 25, 58); c.rect(2, 62 - 62 + 36, 25, 58); c.rect(70, 40, 71, 41); // far (gap 44) stays
  return c; };
F.specks_field = () => { const c = canvas(96, 96), rnd = mulberry32(7); c.rect(30, 30, 65, 65);
  for (let i = 0; i < 60; i++) { const x = rnd() * 94 | 0, y = rnd() * 94 | 0, s = 1 + (rnd() * 3 | 0); c.rect(x, y, x + s - 1, y + s - 1); } return c; };
F.specks_boundary = () => { // 300 dpi area limit 279.000558 px^2: 279 joins at gap 11; 280 stays at gap 11; far 4px speck stays
  const c = canvas(120, 80); c.rect(0, 0, 9, 79); c.rect(21, 5, 29, 35); c.rect(21, 50, 30, 77); c.rect(100, 40, 101, 41); return c; };
F.lines_1px = () => { const c = canvas(40, 40); for (let x = 2; x < 38; x++) c.set(x, 5, 255); for (let y = 8; y < 36; y++) c.set(5, y, 255); for (let i = 0; i < 30; i++) c.set(8 + i, 8 + i, 255); for (let x = 2; x < 38; x += 1) c.set(x, 37, 255); return c; };
F.aa_edges = () => { const c = canvas(64, 48); for (let y = 0; y < 48; y++) for (let x = 0; x < 64; x++) { let d = Math.hypot(x - 20, y - 24); if (d < 15 + 3) c.set(x, y, Math.round(255 * Math.max(0, Math.min(1, 15 + 1.5 - d)) ) || (d < 18 ? 20 + ((x * 7 + y * 3) % 60) : 0), [30, 120, 220]);
  const e = Math.hypot(x - 46, y - 24); if (e < 13) c.set(x, y, Math.round(255 * Math.min(1, (13 - e) / 5)), [220, 120, 30]); } return c; };
F.alpha_ramp = () => { const c = canvas(256, 4); for (let x = 0; x < 256; x++) for (let y = 0; y < 4; y++) c.set(x, y, x, [x, 255 - x, 128]); return c; };
F.alpha_edges = () => { // blocks at exactly T-S, T, T+S for (128,40) and neighbours; separated by 14 px (> 11.8 px reach)
  const c = canvas(13 * 24 + 4, 12), vals = [0, 1, 87, 88, 89, 127, 128, 129, 167, 168, 169, 254, 255]; vals.forEach((a, i) => c.rect(1 + i * 24, 1, 10 + i * 24, 10, a)); return c; };
F.alpha_edges_ring = () => { const c = canvas(40, 40); c.rect(2, 2, 37, 37, 168); c.rect(6, 6, 33, 33, 0); c.rect(12, 12, 27, 27, 128); c.rect(16, 16, 23, 23, 88); return c; };
F.far_blocks = () => { const c = canvas(100, 70); c.rect(2, 2, 20, 20); c.rect(35, 2, 50, 20); c.rect(66, 2, 90, 20); // gaps > reach (11.8 px)
  for (let i = 0; i < 12; i++) c.set(2 + i, 40 + i, 255); for (let i = 0; i < 12; i++) c.set(14 + i, 51 - i, 255); // diagonal V (8-connected)
  c.rect(50, 40, 70, 60); c.rect(60, 48, 61, 49, 0); c.rect(80, 40, 81, 41); return c; };
F.rings_wide = () => { const c = canvas(101, 101); for (let k = 0; k < 4; k++) { const o = k * 14, i = 100 - o; for (let t = o; t <= i; t++) for (const [x, y] of [[t, o], [t, i], [o, t], [i, t]]) c.set(x, y, 255); } return c; };
F.island_in_ring_far = () => { const c = canvas(120, 60); for (let t = 0; t < 50; t++) for (const [x, y] of [[t, 0], [t, 49], [0, t], [49, t]]) c.set(x + 2, y + 2, 255);
  c.rect(20, 20, 31, 31); c.rect(70, 10, 100, 40); return c; };

function build(outDir) {
  const names = Object.keys(F), made = {};
  fs.mkdirSync(path.join(outDir, 'pam'), { recursive: true }); fs.mkdirSync(path.join(outDir, 'expected'), { recursive: true });
  for (const n of names) {
    const c = F[n](), pam = writePam(c.w, c.h, c.d); made['pam/' + n + '.pam'] = pam;
    fs.writeFileSync(path.join(outDir, 'pam', n + '.pam'), pam);
    const parsed = parsePam(pam);
    for (const [T, S, dpi] of PARAMS) { const f = `expected/${n}.T${T}_S${S}_dpi${dpi}.json`; const j = oracle(parsed, T, S, dpi); made[f] = j; fs.writeFileSync(path.join(outDir, f), j); }
  }
  return made;
}
module.exports = { build, PARAMS, DPI, names: () => Object.keys(F) };
if (require.main === module) { const out = path.resolve(process.argv[2] || __dirname), m = build(out); console.log(`wrote ${Object.keys(m).length} files to ${out}`); }
