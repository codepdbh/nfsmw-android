// Builds nfsmw_shaders.nfsp on the phone, with the same steps and modules as the nfsmw-nx installer page
// (lib/ and wasm/ are copied from it unchanged; see INSTALLER_COMMIT). ShaderBuilder.java serves this folder and
// the game files under https://appassets.androidplatform.net/, and receives progress and the result through the
// NfsmwShaders interface.
import createHlslModule from './wasm/hlsl.mjs';
import createDxcModule from './wasm/dxc_web.mjs';
import createPackModule from './wasm/pack.mjs';
import createLzxModule from './wasm/lzx.mjs';
import { readXexImage } from './lib/xex.js';
import { ContainerScanner } from './lib/containers.js';
import { buildShaderLibrary } from './lib/shaders.js';
import { setLanguage } from './lib/i18n.js';

const host = window.NfsmwShaders;
const GAME = 'https://appassets.androidplatform.net/game/';
const CHUNK = 16 << 20;  // as the installer: the scanner keeps 64 KB of look-ahead between chunks
// SHA-256 of the Android library (this folder's shader_common.h) by SHA-256 of default.xex. Rebuild the value
// with the same modules whenever shader_common.h changes (the version in ShaderBuilder.LIBRARY_VERSION too).
const ANDROID_LIBRARIES = {
  // PAL (Spanish)
  aad15fc218d034de3131f746e0b763dd8049a40dd676efc87667fcaf4fc72694:
    'b84602ca9bf6efd84f54ad1509297baccd57c04ffcda494c77f5780a433f95a7',
};

const progress = (fraction, text) => host.progress(Math.max(0, Math.min(1, fraction)), text);

const hex = (buffer) => [...new Uint8Array(buffer)].map((b) => b.toString(16).padStart(2, '0')).join('');
const sha256 = async (bytes) => hex(await crypto.subtle.digest('SHA-256', bytes));

async function fetchBytes(url) {
  const response = await fetch(url);
  if (!response.ok) {
    throw new Error(`no se pudo leer ${url} (${response.status})`);
  }
  return new Uint8Array(await response.arrayBuffer());
}

// Streams one disc file into the scanner in 16 MB pieces.
async function scanFile(scanner, file, onBytes) {
  const response = await fetch(GAME + file.path.split('/').map(encodeURIComponent).join('/'));
  if (!response.ok || !response.body) {
    throw new Error(`no se pudo leer ${file.path}`);
  }
  scanner.beginFile(file.path, file.size);
  const reader = response.body.getReader();
  let pending = [];
  let pendingBytes = 0;
  let seen = 0;
  const flush = (final) => {
    const piece = new Uint8Array(pendingBytes);
    let at = 0;
    for (const p of pending) {
      piece.set(p, at);
      at += p.length;
    }
    pending = [];
    pendingBytes = 0;
    scanner.push(piece, final);
  };
  for (;;) {
    const { done, value } = await reader.read();
    if (done) {
      break;
    }
    pending.push(value);
    pendingBytes += value.length;
    seen += value.length;
    onBytes(value.length);
    if (pendingBytes >= CHUNK && seen < file.size) {
      flush(false);
    }
  }
  if (seen !== file.size) {
    throw new Error(`${file.path}: se leyeron ${seen} de ${file.size} bytes`);
  }
  flush(true);
}

async function main() {
  setLanguage('es');
  progress(0, 'Cargando el compilador de shaders…');
  const quiet = () => ({ print: () => {}, printErr: () => {} });
  const [hlsl, dxc, pack, lzx] = await Promise.all([
    createHlslModule(quiet()), createDxcModule(quiet()), createPackModule(quiet()), createLzxModule(quiet()),
  ]);
  const manifest = JSON.parse(new TextDecoder().decode(await fetchBytes('./release/manifest.json')));
  const shaderCommon = await fetchBytes('./shader_common.h');

  progress(0.02, 'Leyendo default.xex…');
  const xex = await fetchBytes(GAME + 'default.xex');
  const xexHash = await sha256(xex);
  const build = manifest.builds.find((b) => b.xex_sha256 === xexHash);
  const { image } = await readXexImage(xex, async (compressed, bits, size) => {
    lzx.FS.writeFile('/i.lzx', compressed);
    if (lzx.callMain(['/i.lzx', '/i.bin', String(bits), String(size)])) {
      throw new Error('fallo al descomprimir default.xex');
    }
    return lzx.FS.readFile('/i.bin');
  });
  const executable = new ContainerScanner('xex_');
  executable.scanWhole('default.xex', image);

  // The disc files in the installer's order (lower-case path), so the containers get the same names.
  const files = JSON.parse(host.listDiscFiles());
  const total = files.reduce((sum, f) => sum + f.size, 0);
  const disc = new ContainerScanner('');
  let read = 0;
  for (const f of files) {
    await scanFile(disc, f, (n) => {
      read += n;
      progress(0.03 + 0.37 * (read / total), `Buscando shaders en ${f.path}…`);
    });
  }
  const containers = [...disc.found, ...executable.found];
  progress(0.4, `Encontrados ${containers.length} shaders. Traduciendo…`);

  const blurSha = build ? build.blur_container_sha256 : manifest.builds[0].blur_container_sha256;
  let blurShader = null;
  for (const c of containers) {
    if ((await sha256(c.bytes)) === blurSha) {
      blurShader = c.name.slice(0, -4);
      break;
    }
  }
  if (!blurShader) {
    throw new Error('no se encontró el shader de composición: ¿es una copia completa del juego?');
  }

  let compiled = 0;
  const library = await buildShaderLibrary(containers, { hlsl, dxc, pack }, shaderCommon, (text) => {
    if (text.startsWith('Compilados')) {
      compiled += 25;
      progress(0.45 + 0.53 * (compiled / containers.length), text);
    } else {
      progress(0.45, text);
    }
  }, blurShader);

  progress(0.99, 'Comprobando la biblioteca…');
  const libraryHash = await sha256(library);
  // Android's library differs from the Switch one on purpose: its shader_common.h has no 64-bit pointer path
  // (Vulkan 1.1 and drivers without shaderInt64), so it is checked against its own SHA-256 per executable.
  const expected = ANDROID_LIBRARIES[xexHash];
  if (expected && libraryHash !== expected) {
    throw new Error(`la biblioteca no coincide con la esperada (${libraryHash.slice(0, 12)}…)`);
  }
  // Base64 in pieces, so no single string conversion of 3 MB of bytes has to fit in the call stack.
  let text = '';
  for (let i = 0; i < library.length; i += 0x8000) {
    text += String.fromCharCode.apply(null, library.subarray(i, i + 0x8000));
  }
  host.done(btoa(text), libraryHash, build ? build.edition : '');
}

main().catch((error) => host.fail(String(error && error.message ? error.message : error)));
