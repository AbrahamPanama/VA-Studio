#!/usr/bin/env node
// SPDX-License-Identifier: GPL-2.0-or-later
// Runs the pinned simulator engine (sim-engine.js) on binary PAM (P7, RGB_ALPHA, 8-bit) files
// and writes canonical JSON. Node only, no packages.
// Usage: node export-oracle.cjs --T 128 --S 40 --dpi 300 --out out.json in.pam
'use strict';
const fs = require('node:fs'), vm = require('node:vm'), path = require('node:path'), crypto = require('node:crypto');
const sha = b => crypto.createHash('sha256').update(b).digest('hex');
const ENGINE = path.join(__dirname, 'sim-engine.js');

function parsePam(buf) {
  const marker = Buffer.from('ENDHDR\n'), at = buf.indexOf(marker);
  if (!buf.subarray(0, 3).equals(Buffer.from('P7\n')) || at < 0) throw new Error('not a binary PAM (P7)');
  const head = buf.subarray(0, at).toString('latin1'), g = k => { const m = head.match(new RegExp('^' + k + ' (.+)$', 'm')); return m && m[1].trim(); };
  const w = +g('WIDTH'), h = +g('HEIGHT');
  if (+g('DEPTH') !== 4 || +g('MAXVAL') !== 255 || g('TUPLTYPE') !== 'RGB_ALPHA') throw new Error('PAM must be 8-bit RGB_ALPHA');
  const data = buf.subarray(at + marker.length);
  if (!(w > 0 && h > 0) || data.length !== w * h * 4) throw new Error('PAM size mismatch');
  return { w, h, data: new Uint8ClampedArray(data) };
}
function writePam(w, h, rgba) {
  return Buffer.concat([Buffer.from(`P7\nWIDTH ${w}\nHEIGHT ${h}\nDEPTH 4\nMAXVAL 255\nTUPLTYPE RGB_ALPHA\nENDHDR\n`), Buffer.from(rgba.buffer, rgba.byteOffset, rgba.length)]);
}
let ctx, labelCtx, enclosureCtx;
function labelEngine() {
  if (!labelCtx) {
    labelCtx = vm.createContext({console, performance});
    const source = fs.readFileSync(ENGINE, 'utf8');
    // Reuse the pinned initial flood labeling verbatim, stopping before graph/crop/output policy.
    const end = source.indexOf("\n stage('Etiquetado');");
    if (end < 0) throw new Error('pinned label boundary missing');
    vm.runInContext(source.slice(0, end) + `
      const boxes=new Map();
      for(let p=0;p<n;p++)if(labels[p]>0){let id=labels[p],x=p%w,y=(p/w)|0,b=boxes.get(id);
        if(!b){b={id,x,y,x2:x,y2:y,area:0};boxes.set(id,b)}
        b.x=Math.min(b.x,x);b.y=Math.min(b.y,y);b.x2=Math.max(b.x2,x);b.y2=Math.max(b.y2,y);b.area++}
      return {labels,pieces:[...boxes.values()].sort((a,b)=>a.y-b.y||a.x-b.x||a.id-b.id)};
    }` + source.slice(source.indexOf('function alphaThreshold('), source.indexOf('function proxyRaster(')), labelCtx);
  }
  return labelCtx;
}
function enclosureEngine() {
  if (!enclosureCtx) {
    enclosureCtx = vm.createContext({console, performance});
    const source = fs.readFileSync(ENGINE, 'utf8');
    const end = source.indexOf("\n stage('Contención');");
    if (end < 0) throw new Error('pinned enclosure boundary missing');
    vm.runInContext(source.slice(0, end) + `
      const boxes=new Map();
      for(let p=0;p<n;p++)if(labels[p]>0){let id=labels[p]=root(labels[p]),x=p%w,y=(p/w)|0,b=boxes.get(id);
        if(!b){b={id,x,y,x2:x,y2:y,area:0};boxes.set(id,b)}
        b.x=Math.min(b.x,x);b.y=Math.min(b.y,y);b.x2=Math.max(b.x2,x);b.y2=Math.max(b.y2,y);b.area++}
      return {labels,pieces:[...boxes.values()].sort((a,b)=>a.y-b.y||a.x-b.x||a.id-b.id)};
    }` + source.slice(source.indexOf('function alphaThreshold('), source.indexOf('function proxyRaster(')), enclosureCtx);
  }
  return enclosureCtx;
}
function engine() {
  if (!ctx) {
    ctx = vm.createContext({ console, performance });
    vm.runInContext(fs.readFileSync(ENGINE, 'utf8'), ctx, { filename: 'sim-engine.js' });
  }
  return ctx;
}
// Alternating run lengths over the bbox, row-major, starting with a background run (may be 0).
function rle(labels, w, p) {
  const runs = []; let cur = 0, len = 0;
  for (let y = p.y; y <= p.y2; y++) for (let x = p.x; x <= p.x2; x++) {
    const v = labels[y * w + x] === p.id ? 1 : 0;
    if (v === cur) len++; else { runs.push(len); cur = v; len = 1; }
  }
  runs.push(len); return runs;
}
function oracle(pam, T, S, dpi, intermediate = false) {
  const c = intermediate === 'enclosure' ? enclosureEngine() : intermediate ? labelEngine() : engine();
  // dpi mapping is the engine's own: preview(r,T,S,scale) uses reach = 300/25.4*scale px, scale = dpi/300.
  const scale = dpi / 300;
  c.__r = { w: pam.w, h: pam.h, data: pam.data };
  const res = intermediate ? null : vm.runInContext(`preview(__r, ${T}, ${S}, ${scale})`, c);
  const lutProbe = new Uint8ClampedArray(256 * 4);
  for (let a = 0; a < 256; a++) lutProbe[a * 4 + 3] = a;
  c.__p = lutProbe; vm.runInContext(`alphaThreshold({data:__p},{threshold:${T},softness:${S}})`, c);
  const lut = new Uint8Array(256); for (let a = 0; a < 256; a++) lut[a] = c.__p[a * 4 + 3];
  let part = res && res.partition;
  if (intermediate) {
    // v3.2 keeps truly transparent samples transparent, even for T<S.
    lut[0] = 0;
    c.__alpha = Uint8Array.from({length:pam.w*pam.h}, (_, i) => lut[pam.data[i*4+3]]);
    part = vm.runInContext(`explode(__alpha, ${pam.w}, ${pam.h},
      {T:0,D:0,reach:0,enclosure:${intermediate === 'enclosure'},remove:false,dots:false,S:0,min:0,gutter:0,conn:8})`, c);
  }
  const body = {
    format: intermediate === 'enclosure' ? 'explode-bitmap-enclosure-oracle-1' : intermediate ? 'explode-bitmap-label-oracle-1' : 'explode-bitmap-oracle-1',
    width: pam.w, height: pam.h, T, S, dpi,
    lut_sha256: sha(Buffer.from(lut)),
    piece_count: part.pieces.length,
    pieces: part.pieces.map((p, i) => ({ index: i, id: p.id, bbox: [p.x, p.y, p.x2, p.y2], area: p.area, rle: rle(part.labels, pam.w, p) })),
  };
  const text = JSON.stringify(body);
  return JSON.stringify({ ...body, sha256: sha(Buffer.from(text)) }) + '\n';
}
module.exports = { parsePam, writePam, oracle, sha };
if (require.main === module) {
  const a = process.argv.slice(2), o = { T: 128, S: 40, dpi: 300 }, files = [];
  for (let i = 0; i < a.length; i++) { if (a[i] === '--T') o.T = +a[++i]; else if (a[i] === '--S') o.S = +a[++i]; else if (a[i] === '--dpi') o.dpi = +a[++i]; else if (a[i] === '--enclosure') o.enclosure = true; else if (a[i] === '--enclosure-suite') o.enclosureSuite = true; else if (a[i] === '--labels') o.labels = true; else if (a[i] === '--label-suite') o.suite = true; else if (a[i] === '--out') o.out = a[++i]; else files.push(a[i]); }
  if (files.length !== 1 || !Number.isInteger(o.T) || !Number.isInteger(o.S)) { console.error('usage: export-oracle.cjs [--T n --S n --dpi n --out file] in.pam'); process.exit(2); }
  const json = o.enclosureSuite ? JSON.stringify(fs.readdirSync(files[0]).filter(f => f.endsWith('.pam')).sort().flatMap(file =>
    [[128,40],[128,0],[1,0],[200,60],[0,0],[0,127],[255,0],[255,127]].flatMap(([T,S]) =>
      [300,25.4].map(dpi => ({file, ...JSON.parse(oracle(parsePam(fs.readFileSync(path.join(files[0], file))), T,S,dpi,'enclosure'))}))))) + '\n' : o.suite ? JSON.stringify(fs.readdirSync(files[0]).filter(f => f.endsWith('.pam')).sort().flatMap(file =>
    [[128,40],[128,0],[1,0],[200,60],[0,0],[0,127],[255,0],[255,127]].map(([T,S]) =>
      ({file, ...JSON.parse(oracle(parsePam(fs.readFileSync(path.join(files[0], file))), T,S,300,true))})))) + '\n' :
    oracle(parsePam(fs.readFileSync(files[0])), o.T, o.S, o.dpi, o.enclosure ? 'enclosure' : o.labels);
  if (o.out) fs.writeFileSync(o.out, json); else process.stdout.write(json);
}
