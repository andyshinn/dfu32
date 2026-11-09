// SPDX-License-Identifier: MIT
// "Custom build": install DFU32 from a build .zip instead of a published
// version, so a pull request's build can be tried from the page (SPEC.md §9.4).
//
// The zip is the dfu32-<board> artifact of a CI run (or any zip of an IDF
// build directory). This is tools/dfu32_web_manifest.py in JavaScript: it
// reads flasher_args.json, project_description.json and sdkconfig, and lists
// the same four parts (bootloader, partition table, otadata, app) at their
// flash offsets, never the merged image, so NVS, runlog and staging are left
// alone. The page hands ESP Web Tools a manifest whose part paths are blob:
// URLs of the parts. It has no DOM or serial code, so Node runs it too:
// tools/test_custom_build_js.py checks it against the Python tool.

import { openZip, parsePartitionTable, StageError } from './staging.js';

export const MAX_BUILD_ZIP = 256 * 1024 * 1024;  // the CI artifact carries dfu32.elf too
const MAX_PART = 16 * 1024 * 1024;               // larger than any app partition
// flasher_args.json sections, in flash order (dfu32_web_manifest.PARTS).
export const PARTS = ['bootloader', 'partition-table', 'otadata', 'app'];
// Partitions whose contents a keep-settings update must find where it left them (check_partitions.KEPT).
export const KEPT = ['nvs', 'runlog', 'staging'];
export const PARTITION_TABLE_OFFSET = 0x8000;
const BOARD_RE = /^[a-z0-9_]+$/;
const IMAGE_MAGIC = 0xE9;
const APP_DESC_OFFSET = 24 + 8;  // image header + first segment header (check_board_tag.py)
const APP_DESC_MAGIC = 0xABCD5432;
const BOARD_TAG_OFFSET = APP_DESC_OFFSET + 256;
const BOARD_TAG_MAGIC = 'DF32BRD1';

const dec = new TextDecoder();
const cstr = b => { const z = b.indexOf(0); return dec.decode(z < 0 ? b : b.subarray(0, z)); };

/** IDF target name to ESP Web Tools chipFamily: esp32 -> ESP32, esp32s3 -> ESP32-S3. */
export function chipFamily(chip) {
  const m = String(chip).trim().toLowerCase().match(/^esp(32|8266)([a-z]\d+)?$/);
  if (!m) throw new StageError(`unknown chip '${chip}'`);
  return `ESP${m[1]}` + (m[2] ? `-${m[2].toUpperCase()}` : '');
}

/** CONFIG_* lines of an sdkconfig, values unquoted (dfu32_web_manifest.read_sdkconfig). */
export function parseSdkconfig(text) {
  const out = {};
  for (const line of text.split(/\r?\n/)) {
    if (!line.startsWith('CONFIG_') || !line.includes('=')) continue;
    const i = line.indexOf('=');
    out[line.slice(0, i)] = line.slice(i + 1).trim().replace(/^"|"$/g, '');
  }
  return out;
}

/** Version, project and board tag ('' if untagged) from an app image; null without esp_app_desc_t. */
export function appInfo(app) {
  if (app.length < BOARD_TAG_OFFSET + 40 || app[0] !== IMAGE_MAGIC) return null;
  const dv = new DataView(app.buffer, app.byteOffset, app.byteLength);
  if (dv.getUint32(APP_DESC_OFFSET, true) !== APP_DESC_MAGIC) return null;
  const tag = app.subarray(BOARD_TAG_OFFSET, BOARD_TAG_OFFSET + 40);
  return {
    version: cstr(app.subarray(APP_DESC_OFFSET + 16, APP_DESC_OFFSET + 48)),
    project: cstr(app.subarray(APP_DESC_OFFSET + 48, APP_DESC_OFFSET + 80)),
    board: dec.decode(tag.subarray(0, 8)) === BOARD_TAG_MAGIC ? cstr(tag.subarray(8)) : '',
  };
}

/** Where nvs, runlog and staging sit in a partition table: { label: [offset, size] }. */
export function keptLayout(table) {
  const out = {};
  for (const e of parsePartitionTable(table)) if (KEPT.includes(e.label)) out[e.label] = [e.offset, e.size];
  return out;
}

/** The kept partitions that sit differently in two tables, by name. */
export function layoutDiff(a, b) {
  const x = keptLayout(a), y = keptLayout(b);
  return KEPT.filter(n => String(x[n]) !== String(y[n]));
}

const dirOf = p => p.includes('/') ? p.slice(0, p.lastIndexOf('/') + 1) : '';
const baseName = p => p.slice(p.lastIndexOf('/') + 1);
// Join a flasher_args.json file path onto the build directory, resolving "./" and "../".
// null if it leaves the build directory: IDF only names files inside it.
function joinPath(dir, rel) {
  const out = dir.split('/').filter(Boolean), floor = out.length;
  for (const seg of rel.replace(/\\/g, '/').split('/')) {
    if (seg === '' || seg === '.') continue;
    if (seg !== '..') out.push(seg);
    else if (out.length > floor) out.pop();
    else return null;
  }
  return out.join('/');
}
// flasher_args.json writes offsets as "0x8000"; a bare number is accepted too.
const OFFSET_RE = /^(0x[0-9a-f]+|\d+)$/i;

/**
 * Read a build zip. Returns { board, chipFamily, version, debug, table, parts: [{ name, offset, data }] }.
 * fileName is only a fallback for the version.
 */
export async function loadBuild(bytes, fileName = '') {
  const zip = openZip(bytes, { maxZip: MAX_BUILD_ZIP, maxEntry: MAX_PART, tooLarge: 'the file is too large for a DFU32 build' });
  // The build directory is wherever flasher_args.json is (build/<board>/ in a CI artifact).
  // Archive tools' own entries (macOS's __MACOSX/._name) are skipped.
  const found = zip.names.filter(n => baseName(n) === 'flasher_args.json' && !n.startsWith('__MACOSX/'));
  if (!found.length) {
    throw new StageError("it has no flasher_args.json. Pick the dfu32-<board> artifact of a CI run (not dfu32-<board>-debug, which holds only the app), or a zip of an IDF build directory");
  }
  if (found.length > 1) throw new StageError(`it holds more than one build (${found.join(', ')}); zip one build directory`);
  const dir = dirOf(found[0]);
  const json = async name => {
    try { return JSON.parse(dec.decode(await zip.read(name))); } catch (e) {
      if (e instanceof StageError) throw e;
      throw new StageError(`${name} is not valid JSON`);
    }
  };

  if (!zip.names.includes(dir + 'sdkconfig')) throw new StageError(`it has no ${dir}sdkconfig, so the board can't be told`);
  const cfg = parseSdkconfig(dec.decode(await zip.read(dir + 'sdkconfig')));
  const board = cfg.CONFIG_DFU32_BOARD_NAME || '';
  if (!BOARD_RE.test(board)) throw new StageError("its sdkconfig names no DFU32 board (CONFIG_DFU32_BOARD_NAME): it isn't a DFU32 build");

  const args = await json(found[0]);
  const chip = args?.extra_esptool_args?.chip;
  if (!chip) throw new StageError('flasher_args.json has no extra_esptool_args.chip');
  const family = chipFamily(chip);

  const parts = [];
  for (const name of PARTS) {
    const entry = args[name];
    if (!entry || typeof entry.file !== 'string' || !entry.file) throw new StageError(`flasher_args.json has no '${name}' part`);
    const path = joinPath(dir, entry.file);
    if (path === null) throw new StageError(`flasher_args.json puts '${name}' outside the build directory (${entry.file})`);
    if (!zip.names.includes(path)) throw new StageError(`it has no ${path}`);
    const raw = entry.offset;
    const ok = (typeof raw === 'number' && Number.isSafeInteger(raw) && raw >= 0) || (typeof raw === 'string' && OFFSET_RE.test(raw.trim()));
    if (!ok) throw new StageError(`flasher_args.json gives '${name}' the offset ${JSON.stringify(raw)}`);
    const offset = Number(typeof raw === 'string' ? raw.trim() : raw);
    parts.push({ name, file: baseName(path), offset, data: await zip.read(path) });
  }
  const spans = parts.map(p => [p.offset, p.offset + p.data.length]).sort((a, b) => a[0] - b[0]);
  if (spans.some(([, end], i) => i + 1 < spans.length && end > spans[i + 1][0])) throw new StageError('two parts overlap');
  const [boot, table, , app] = parts;
  if (boot.data[0] !== IMAGE_MAGIC || app.data[0] !== IMAGE_MAGIC) throw new StageError("the bootloader or the app isn't an ESP32 image");
  if (table.offset !== PARTITION_TABLE_OFFSET || !parsePartitionTable(table.data).length) throw new StageError('the partition table is empty or not at 0x8000');

  const info = appInfo(app.data);
  if (!info) throw new StageError('the app has no esp_app_desc_t, so it can\'t be told for DFU32');
  if (info.project !== 'dfu32') throw new StageError(`the app is '${info.project}', not DFU32`);
  if (info.board && info.board !== board) throw new StageError(`the app is tagged for ${info.board} but sdkconfig says ${board}`);

  let version = '';
  if (zip.names.includes(dir + 'project_description.json')) version = (await json(dir + 'project_description.json')).project_version || '';
  version = String(version || info.version || baseName(fileName).replace(/\.zip$/i, '') || 'custom');

  return {
    board, chipFamily: family, version, table: table.data, parts,
    debug: cfg.CONFIG_DFU32_DEBUG_ENDPOINTS === 'y',
  };
}

/** An ESP Web Tools manifest for a loaded build, given a URL for each part's data. */
export function buildManifest(build, urlFor) {
  return {
    name: 'DFU32',
    version: build.version,
    new_install_prompt_erase: true,
    builds: [{ chipFamily: build.chipFamily, parts: build.parts.map(p => ({ path: urlFor(p), offset: p.offset })) }],
  };
}
