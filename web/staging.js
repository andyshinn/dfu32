// SPDX-License-Identifier: MIT
// Build a DFU32 staging-partition image in the browser (SPEC.md §8).
//
// This is tools/dfu32_stage_image.py in JavaScript: read an adafruit-nrfutil
// DFU .zip, validate the .bin and .dat the way DFU32 does before it touches a
// target, and lay out the 4 KiB header sector followed by the .bin. It has no
// DOM or serial code, so Node runs it too: tools/test_staging_js.py checks its
// output byte for byte against the Python tool's.

export const MAGIC = 'DF32STG1';
export const HEADER_VERSION = 1;
export const HEADER_SIZE = 0x240;
export const DATA_OFFSET = 0x1000;
export const MAX_DAT = 64;
export const MAX_APP_SIZE = 802816;  // nRF52840 + S140 6.1.1; S140 7.3.0 is 798720
export const ADAFRUIT_DEVICE_TYPE = 0x0052;  // Adafruit nRF52 bootloader
export const NRF54_DEVICE_TYPE = 0x0054;     // meshtastic/nRF54_Bootloader (checks size itself at START)
export const PARTITION_TABLE_OFFSET = 0x8000;  // CONFIG_PARTITION_TABLE_OFFSET in sdkconfig.defaults
export const PARTITION_TABLE_SIZE = 0xC00;
export const STAGING_SUBTYPE = 0x40;

export const MAX_ZIP = 16 * 1024 * 1024;  // a DFU zip is well under 1 MB
const MAX_ENTRY = 4 * 1024 * 1024;  // inflated size cap per entry

export class StageError extends Error {}

// ---- checksums ----

/** CRC-16/CCITT-FALSE, same as Nordic crc16_compute(). */
export function crc16(data, crc = 0xFFFF) {
  for (const b of data) {
    crc = ((crc >> 8) | (crc << 8)) & 0xFFFF;
    crc ^= b;
    crc ^= (crc & 0xFF) >> 4;
    crc ^= (crc << 12) & 0xFFFF;
    crc ^= ((crc & 0xFF) << 5) & 0xFFFF;
  }
  return crc;
}

let crcTable;
/** CRC-32/IEEE (zlib.crc32): the header's header_crc32 and zip entry CRCs. */
export function crc32(data) {
  if (!crcTable) {
    crcTable = new Uint32Array(256);
    for (let n = 0; n < 256; n++) {
      let c = n;
      for (let k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320 ^ (c >>> 1) : c >>> 1;
      crcTable[n] = c >>> 0;
    }
  }
  let c = 0xFFFFFFFF;
  for (const b of data) c = crcTable[(c ^ b) & 0xFF] ^ (c >>> 8);
  return (c ^ 0xFFFFFFFF) >>> 0;
}

export async function sha256(data) {
  return new Uint8Array(await crypto.subtle.digest('SHA-256', data));
}

export const hex = bytes => Array.from(bytes, b => b.toString(16).padStart(2, '0')).join('');
/** 0x0052-style: a .dat field as the bootloader docs write it. */
export const h4 = v => '0x' + v.toString(16).toUpperCase().padStart(4, '0');

const utf8 = new TextEncoder();
export const utf8len = s => utf8.encode(s).length;

// MD5 (RFC 1321), only to check the write: the flasher stub reports the MD5 of
// the flash region it wrote, and Web Crypto has no MD5.
const MD5_S = [7, 12, 17, 22, 5, 9, 14, 20, 4, 11, 16, 23, 6, 10, 15, 21];
const MD5_K = Array.from({ length: 64 }, (_, i) => Math.floor(Math.abs(Math.sin(i + 1)) * 2 ** 32) >>> 0);
export function md5Hex(data) {
  const len = data.length;
  const padded = new Uint8Array(((len + 8) >> 6) * 64 + 64);
  padded.set(data);
  padded[len] = 0x80;
  const dv = new DataView(padded.buffer);
  dv.setUint32(padded.length - 8, (len * 8) >>> 0, true);
  dv.setUint32(padded.length - 4, Math.floor(len / 0x20000000), true);
  let a0 = 0x67452301, b0 = 0xEFCDAB89, c0 = 0x98BADCFE, d0 = 0x10325476;
  const m = new Uint32Array(16);
  for (let off = 0; off < padded.length; off += 64) {
    for (let i = 0; i < 16; i++) m[i] = dv.getUint32(off + i * 4, true);
    let a = a0, b = b0, c = c0, d = d0;
    for (let i = 0; i < 64; i++) {
      let f, g;
      if (i < 16) { f = (b & c) | (~b & d); g = i; }
      else if (i < 32) { f = (d & b) | (~d & c); g = (5 * i + 1) & 15; }
      else if (i < 48) { f = b ^ c ^ d; g = (3 * i + 5) & 15; }
      else { f = c ^ (b | ~d); g = (7 * i) & 15; }
      const s = MD5_S[(i >> 4) * 4 + (i & 3)];
      const t = (a + f + MD5_K[i] + m[g]) >>> 0;
      a = d; d = c; c = b;
      b = (b + ((t << s) | (t >>> (32 - s)))) >>> 0;
    }
    a0 = (a0 + a) >>> 0; b0 = (b0 + b) >>> 0; c0 = (c0 + c) >>> 0; d0 = (d0 + d) >>> 0;
  }
  const out = new DataView(new ArrayBuffer(16));
  [a0, b0, c0, d0].forEach((v, i) => out.setUint32(i * 4, v, true));
  return hex(new Uint8Array(out.buffer));
}

// ---- zip ----

async function inflateRaw(data) {
  const stream = new Blob([data]).stream().pipeThrough(new DecompressionStream('deflate-raw'));
  const reader = stream.getReader();
  const chunks = [];
  let size = 0;
  for (;;) {
    const { done, value } = await reader.read();
    if (done) break;
    size += value.length;
    if (size > MAX_ENTRY) { await reader.cancel(); throw new StageError('a file in the zip is too large'); }
    chunks.push(value);
  }
  const out = new Uint8Array(size);
  let o = 0;
  for (const c of chunks) { out.set(c, o); o += c.length; }
  return out;
}

/** List a zip's files. Returns { names, read(name) -> Promise<Uint8Array> }. No zip64, no encryption. */
export function openZip(bytes) {
  if (bytes.length > MAX_ZIP) throw new StageError('the file is too large for a DFU package');
  const dv = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  let eocd = -1;
  for (let i = bytes.length - 22; i >= Math.max(0, bytes.length - 22 - 0xFFFF); i--) {
    if (dv.getUint32(i, true) === 0x06054B50) { eocd = i; break; }
  }
  if (eocd < 0) throw new StageError('not a zip file');
  const count = dv.getUint16(eocd + 10, true);
  let p = dv.getUint32(eocd + 16, true);
  const entries = new Map();
  const dec = new TextDecoder();
  for (let n = 0; n < count; n++) {
    if (p + 46 > bytes.length || dv.getUint32(p, true) !== 0x02014B50) throw new StageError('the zip directory is damaged');
    const flags = dv.getUint16(p + 8, true), method = dv.getUint16(p + 10, true);
    const crc = dv.getUint32(p + 16, true), csize = dv.getUint32(p + 20, true), usize = dv.getUint32(p + 24, true);
    const nlen = dv.getUint16(p + 28, true), elen = dv.getUint16(p + 30, true), clen = dv.getUint16(p + 32, true);
    const local = dv.getUint32(p + 42, true);
    const name = dec.decode(bytes.subarray(p + 46, p + 46 + nlen));
    entries.set(name, { flags, method, crc, csize, usize, local });
    p += 46 + nlen + elen + clen;
  }
  async function read(name) {
    const e = entries.get(name);
    if (!e) throw new StageError(`the zip has no ${name}`);
    if (e.flags & 1) throw new StageError(`${name} is encrypted`);
    if (e.usize > MAX_ENTRY || e.csize === 0xFFFFFFFF) throw new StageError(`${name} is too large`);
    if (e.local + 30 > bytes.length || dv.getUint32(e.local, true) !== 0x04034B50) throw new StageError('the zip is damaged');
    const start = e.local + 30 + dv.getUint16(e.local + 26, true) + dv.getUint16(e.local + 28, true);
    if (start + e.csize > bytes.length) throw new StageError('the zip is truncated');
    const raw = bytes.subarray(start, start + e.csize);
    let data;
    if (e.method === 0) data = raw.slice();
    else if (e.method === 8) data = await inflateRaw(raw);
    else throw new StageError(`${name} uses an unsupported compression method (${e.method})`);
    if (data.length !== e.usize || crc32(data) !== e.crc) throw new StageError(`${name} is damaged (CRC mismatch)`);
    return data;
  }
  return { names: [...entries.keys()], read };
}

/** The application .bin and .dat from an adafruit-nrfutil DFU zip. */
export async function loadPackage(bytes) {
  const zip = openZip(bytes);
  let binName, datName;
  if (zip.names.includes('manifest.json')) {
    let app;
    try {
      app = JSON.parse(new TextDecoder().decode(await zip.read('manifest.json')))?.manifest?.application;
    } catch (e) {
      if (e instanceof StageError) throw e;
      throw new StageError("the zip's manifest.json is not valid JSON");
    }
    if (!app) throw new StageError("the zip's manifest.json has no application (SoftDevice and bootloader packages are not supported)");
    binName = app.bin_file; datName = app.dat_file;
    if (typeof binName !== 'string' || typeof datName !== 'string') throw new StageError("the zip's manifest.json names no bin_file and dat_file");
  } else {
    const bins = zip.names.filter(n => n.endsWith('.bin')), dats = zip.names.filter(n => n.endsWith('.dat'));
    if (bins.length !== 1 || dats.length !== 1) throw new StageError('the zip has no manifest.json and not exactly one .bin and one .dat');
    [binName] = bins; [datName] = dats;
  }
  return { bin: await zip.read(binName), dat: await zip.read(datName), binName, datName };
}

// ---- init packet and checks ----

export function parseDat(dat) {
  if (dat.length < 14) throw new StageError(`.dat too short (${dat.length} bytes)`);
  const dv = new DataView(dat.buffer, dat.byteOffset, dat.byteLength);
  const deviceType = dv.getUint16(0, true), deviceRev = dv.getUint16(2, true), appVersion = dv.getUint32(4, true);
  const n = dv.getUint16(8, true);
  if (n === 0) throw new StageError('.dat has an empty sd_req list (the bootloader rejects it)');
  const expected = 12 + 2 * n;
  if (dat.length !== expected) {
    throw new StageError(`.dat is ${dat.length} bytes, expected ${expected} for dfu-version 0.5 (extended and signed init packets are not supported)`);
  }
  const sdReq = [];
  for (let i = 0; i < n; i++) sdReq.push(dv.getUint16(10 + 2 * i, true));
  return { deviceType, deviceRev, appVersion, sdReq, crc16: dv.getUint16(10 + 2 * n, true) };
}

export function validate(bin, dat, maxAppSize = MAX_APP_SIZE) {
  const pkt = parseDat(dat);
  if (pkt.deviceType !== ADAFRUIT_DEVICE_TYPE && pkt.deviceType !== NRF54_DEVICE_TYPE) {
    throw new StageError(`.dat device_type is ${h4(pkt.deviceType)}; expected 0x0052 (Adafruit nRF52 bootloader) or 0x0054 (nRF54 bootloader)`);
  }
  if (!bin.length) throw new StageError('empty .bin');
  if (bin.length % 4) throw new StageError(`.bin size ${bin.length} is not a multiple of 4`);
  if (pkt.deviceType === ADAFRUIT_DEVICE_TYPE && bin.length > maxAppSize) {
    throw new StageError(`.bin size ${bin.length} exceeds the nRF52 app limit of ${maxAppSize}`);
  }
  const crc = crc16(bin);
  if (crc !== pkt.crc16) throw new StageError(`CRC16 mismatch: .bin is ${h4(crc)}, .dat says ${h4(pkt.crc16)}`);
  return pkt;
}

// ---- header and image ----

function putStr(buf, off, size, s, name) {
  const raw = utf8.encode(s);
  if (raw.length >= size) throw new StageError(`${name} too long (${raw.length} bytes, max ${size - 1})`);
  buf.set(raw, off);
}

/** Truncate to at most `max` UTF-8 bytes without splitting a character. */
export function clipUtf8(s, max) {
  let out = '', len = 0;
  for (const ch of s) {
    len += utf8len(ch);
    if (len > max) break;
    out += ch;
  }
  return out;
}

export async function buildHeader({ bin, dat, crc16: crc, board = '', version = '', targetId = '', manifestUrl = '', stagedAt = 0 }) {
  if (!(dat.length > 0 && dat.length <= MAX_DAT)) throw new StageError('dat size out of range');
  const buf = new Uint8Array(HEADER_SIZE);
  const dv = new DataView(buf.buffer);
  buf.set(utf8.encode(MAGIC), 0x000);
  dv.setUint32(0x008, HEADER_VERSION, true);
  dv.setUint32(0x00C, bin.length, true);
  buf.set(await sha256(bin), 0x010);
  dv.setUint16(0x030, dat.length, true);
  buf.set(dat, 0x032);
  buf.set(await sha256(dat), 0x072);
  dv.setUint16(0x092, crc, true);
  putStr(buf, 0x094, 32, targetId, 'target_id');
  putStr(buf, 0x0B4, 32, board, 'board');
  putStr(buf, 0x0D4, 64, version, 'version');
  // 0x114 manifest_sha256 stays zero: there is no manifest.
  dv.setBigUint64(0x134, BigInt(stagedAt), true);
  putStr(buf, 0x13C, 256, manifestUrl, 'manifest_url');
  dv.setUint32(0x23C, crc32(buf.subarray(0, 0x23C)), true);
  return buf;
}

/** Header sector (0xFF padded to 4 KiB) followed by the .bin: the staging partition's content. */
export async function buildImage(bin, dat, opts = {}) {
  const pkt = parseDat(dat);
  const header = await buildHeader({ bin, dat, crc16: pkt.crc16, ...opts });
  const image = new Uint8Array(DATA_OFFSET + bin.length).fill(0xFF, HEADER_SIZE, DATA_OFFSET);
  image.set(header, 0);
  image.set(bin, DATA_OFFSET);
  return image;
}

// ---- partition table ----

/** Entries of an ESP-IDF partition table (the bytes at 0x8000). */
export function parsePartitionTable(buf) {
  const dv = new DataView(buf.buffer, buf.byteOffset, buf.byteLength);
  const out = [];
  for (let p = 0; p + 32 <= buf.length; p += 32) {
    const magic = dv.getUint16(p, true);
    if (magic !== 0x50AA) break;  // 0xEBEB is the MD5 entry, 0xFFFF the end
    let label = '';
    for (let i = 0; i < 16 && buf[p + 12 + i]; i++) label += String.fromCharCode(buf[p + 12 + i]);
    out.push({ type: buf[p + 2], subtype: buf[p + 3], offset: dv.getUint32(p + 4, true), size: dv.getUint32(p + 8, true), label });
  }
  return out;
}

export function findStaging(buf) {
  return parsePartitionTable(buf).find(e => e.label === 'staging' && e.type === 1 && e.subtype === STAGING_SUBTYPE) || null;
}

// ---- version from the file name ----

// MeshCore assets are <env>-<version>-<commit>.zip, e.g. RAK_4631_repeater-v1.18.0-81dbae5.zip.
const MESHCORE = /^(.+?)-(v?\d[\w.]*-[0-9a-f]{6,40})$/i;

/** The version string: "<version>-<commit>" for a MeshCore asset (what the repeater's `ver` prints), else the file name. */
export function guessVersion(fileName) {
  const stem = fileName.replace(/^.*[\\/]/, '').replace(/\.zip$/i, '');
  const m = stem.match(MESHCORE);
  return clipUtf8(m ? m[2] : stem, 63);
}
