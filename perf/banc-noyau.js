// Copyright 2026 Doalo
// Rhénium : noyaux du banc de division entière (banc-division.html).

'use strict';
const N = 3000000;

function jsDiv(n) {
  let s = 0;
  for (let i = 1; i < n; i++) s = (s + (((i * 7) / ((i & 15) + 1)) | 0)) | 0;
  return s;
}
function jsMod(n) {
  let s = 0;
  for (let i = 1; i < n; i++) s = (s + (i % ((i & 31) + 3))) | 0;
  return s;
}
function jsDivU(n) {
  let s = 0;
  for (let i = 1; i < n; i++) {
    const a = Math.imul(i, 0x9e3779b1) >>> 0;
    s = (s + ((a / (((i & 63) + 1) >>> 0)) >>> 0)) | 0;
  }
  return s;
}
function jsTemoin(n) {
  let s = 0;
  for (let i = 1; i < n; i++) s = (s + ((i * 7) >> ((i & 15) + 1))) | 0;
  return s;
}
function jsTemoinEntier(n) {
  let s = 0;
  for (let i = 1; i < n; i++) s = (s + (Math.imul(i, 7) >> ((i & 15) + 1))) | 0;
  return s;
}

// Module wasm : div(n) = somme de (i*7) div_s ((i&15)+1),
// rem(n) = somme de (i*7) rem_u ((i&31)+3), pour i de 1 à n-1.
function wasmModule() {
  const sec = (id, b) => [id, b.length, ...b];
  const body = (k, op) => {
    const b = [0x01, 0x02, 0x7f,              // 2 locales i32 : i, s
      0x41, 0x01, 0x21, 0x01,                 // i = 1
      0x03, 0x40,                             // loop
      0x20, 0x02,                             //   s
      0x20, 0x01, 0x41, 0x07, 0x6c,           //   i * 7
      0x20, 0x01, 0x41, k[0], 0x71, 0x41, k[1], 0x6a,
      op, 0x6a, 0x21, 0x02,                   //   s += ...
      0x20, 0x01, 0x41, 0x01, 0x6a, 0x22, 0x01,
      0x20, 0x00, 0x48, 0x0d, 0x00,           //   br_if ++i < n
      0x0b, 0x20, 0x02, 0x0b];
    return [b.length, ...b];
  };
  const name = s => [s.length, ...Array.from(s, c => c.charCodeAt(0))];
  const bytes = [0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
    ...sec(1, [0x01, 0x60, 0x01, 0x7f, 0x01, 0x7f]),
    ...sec(3, [0x02, 0x00, 0x00]),
    ...sec(7, [0x02, ...name('div'), 0x00, 0x00, ...name('rem'), 0x00, 0x01]),
    ...sec(10, [0x02, ...body([0x0f, 0x01], 0x6d), ...body([0x1f, 0x03], 0x70)])];
  return new WebAssembly.Instance(new WebAssembly.Module(new Uint8Array(bytes)))
      .exports;
}

function mesure(f) {
  const t = [];
  let r = 0;
  for (let k = 0; k < 7; k++) {
    const t0 = performance.now();
    r = f(N);
    t.push(performance.now() - t0);
  }
  const s = t.slice(2).sort((a, b) => a - b);
  return { ms: s[2], r, t };
}

const pause = () => new Promise(r => setTimeout(r, 50));

// Une session : chaque test dans l'ordre, sur du code compilé pour ce
// contexte seulement.
async function session() {
  const tests = [
    ['JS division', jsDiv],
    ['JS modulo', jsMod],
    ['JS division non signée', jsDivU],
    ['JS témoin (décalage)', jsTemoin],
    ['JS témoin entier', jsTemoinEntier],
  ];
  try {
    const w = wasmModule();
    tests.push(['Wasm i32.div_s', w.div], ['Wasm i32.rem_u', w.rem]);
  } catch (e) {
    console.log('BANC wasm indisponible : ' + e);
  }
  const res = [];
  for (const [nom, f] of tests) {
    await pause();
    res.push({ nom, ...mesure(f) });
  }
  return res;
}
