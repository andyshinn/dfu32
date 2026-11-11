// SPDX-License-Identifier: MIT
// "Stage nRF52 image": write a DFU package to a DFU32's staging partition over
// USB, without an artifact server (SPEC.md §8, §9.4).
//
// The entry point is an item in ESP Web Tools' device dialog, the one that
// lists "Update DFU32", "Connect to Wi-Fi" and "Logs & Console" once Improv
// has identified a running DFU32. ESP Web Tools has no API for extra items, so
// this adds one to the dialog's (open) shadow root, and draws its own dialog
// with the same ew-* elements, which ESP Web Tools has registered by then.
// Both are internals of the pinned version: tools/fetch_esp_web_tools.py
// refuses a bundle that lacks them.
//
// The item closes ESP Web Tools' dialog (which closes the serial port), then
// reopens the same port with esptool-js: the ESP32 resets into its ROM
// bootloader, the page reads the partition table to find `staging`, writes
// the image (staging.js) there, checks the MD5 of what was written and resets
// the ESP32. Nothing else on flash is touched, so WiFi, the token, the target
// and the run log are kept.

import * as S from './staging.js';

const ESPTOOL_JS = './vendor/esptool-js-0.7.0/bundle.js';
const ITEM_ID = 'dfu32-stage-item';
const DFU32 = 'DFU32';  // the firmware name DFU32 reports over Improv

const ICON_UPLOAD = '<svg slot="start" viewBox="0 0 24 24"><path d="M14,2H6A2,2 0 0,0 4,4V20A2,2 0 0,0 6,22H18A2,2 0 0,0 20,20V8L14,2M18,20H6V4H13V9H18V20M12,12L16,16H13.5V19H10.5V16H8L12,12Z"/></svg>';
const ICON_FILE = '<svg slot="start" viewBox="0 0 24 24"><path d="M14,2H6A2,2 0 0,0 4,4V20A2,2 0 0,0 6,22H18A2,2 0 0,0 20,20V8L14,2M18,20H6V4H13V9H18V20M10,19L12,15H9V10H15V15L13,19H10Z"/></svg>';
const ICON_CLOSE = '<svg width="24" height="24" viewBox="0 0 24 24"><path fill="currentColor" d="M19,6.41L17.59,5L12,10.59L6.41,5L5,6.41L10.59,12L5,17.59L6.41,19L12,13.41L17.59,19L19,17.59L13.41,12L19,6.41Z"/></svg>';
const OK_ICON = '🎉', ERROR_ICON = '⚠️';  // ESP Web Tools' own

const esc = s => String(s).replace(/[&<>"']/g, c => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]));
const kB = n => (n / 1024).toFixed(n < 10240 ? 1 : 0) + ' KB';
const { h4, utf8len } = S;
const sleep = ms => new Promise(r => setTimeout(r, ms));
const LOGS_ITEM = 'Logs & Console';  // the device dialog's last fixed item; HOOK_ITEM in fetch_esp_web_tools.py
// dfu32.app/?trace=1: esptool-js logs every serial exchange up to the image
// write (console.log, "TRACE ..." lines) plus its debug messages, for debugging a
// stage that fails; on a failure the whole trace is also printed in one block.
const TRACE = typeof location !== 'undefined' && new URLSearchParams(location.search).get('trace') === '1';
const PORT_BUSY = 'The serial port is still open, in another tab or program (a serial monitor?). Close it, or unplug the board and plug it back in, then try again.';


// ---- the item in ESP Web Tools' device dialog ----

// ESP Web Tools' dialog that is open (it shows one at a time) and the Improv
// device info for it. ESP Web Tools hands the info to
// overrides.checkSameFirmware() (its public hook) whenever it renders the
// device's options; the page's own override still decides the answer. The
// entry goes when that dialog closes, so a later dialog never sees old info.
let current = null;  // { dlg, info, warned }

function wrapOverrides(button) {
  const base = button.overrides || {};
  button.overrides = {
    ...base,
    checkSameFirmware(manifest, info) {
      if (current) {
        current.info = info;
        if (info && info.firmware === DFU32) warnIfMissing(current);
      }
      return base.checkSameFirmware ? base.checkSameFirmware(manifest, info) : info.firmware === manifest.name;
    },
  };
}

const logsItem = root => [...root.querySelectorAll('ew-list-item')].find(i => i.textContent.trim() === LOGS_ITEM);

// The device options are showing for a DFU32 but the item couldn't be placed:
// ESP Web Tools' markup changed in a way the deploy-time check didn't catch.
function warnIfMissing(c) {
  if (c.warned) return;
  setTimeout(() => {
    const root = c.dlg.shadowRoot;
    if (c.warned || current !== c || !root || root.getElementById(ITEM_ID) || logsItem(root)) return;
    c.warned = true;
    console.warn(`DFU32: no "${LOGS_ITEM}" item in ESP Web Tools' device dialog, so "Stage nRF52 image" isn't offered. ` +
      'Its markup may have changed: update web/stage-dialog.js.');
  }, 1500);
}

function addItem(dlg) {
  const root = dlg.shadowRoot, c = current;
  if (!root || !c || c.dlg !== dlg || !c.info || c.info.firmware !== DFU32 || root.getElementById(ITEM_ID)) return;
  // Only on the device options page, before its last fixed item.
  const logs = logsItem(root);
  if (!logs) return;
  const item = document.createElement('ew-list-item');
  item.id = ITEM_ID;
  item.type = 'button';
  item.innerHTML = `${ICON_UPLOAD}<div slot="headline">Stage nRF52 image</div>`;
  item.addEventListener('click', () => leaveFor(dlg, c.info));
  logs.before(item);
}

const portOpen = port => !!(port && (port.readable || port.writable));

// Close ESP Web Tools' dialog, wait for it to release the port, open ours on it.
async function leaveFor(dlg, info) {
  const port = dlg.port || null;
  const md = dlg.shadowRoot.querySelector('ew-dialog');
  if (md) md.close();
  const until = Date.now() + 5000;
  while (Date.now() < until && (dlg.isConnected || portOpen(port))) await sleep(50);
  // Still held: say so now rather than fail later with a bootloader error. Without
  // a port, Stage asks for one again.
  if (portOpen(port)) openStageDialog({ info, error: PORT_BUSY });
  else openStageDialog({ port, info });
}

function hook(dlg) {
  const obs = new MutationObserver(() => addItem(dlg));
  obs.observe(dlg.shadowRoot, { childList: true, subtree: true });
  dlg.addEventListener('closed', () => {
    obs.disconnect();
    if (current && current.dlg === dlg) current = null;
  }, { once: true });
  addItem(dlg);
}

function watch(button) {
  wrapOverrides(button);
  new MutationObserver(records => {
    for (const r of records) {
      for (const n of r.addedNodes) {
        if (n.localName !== 'ewt-install-dialog') continue;
        current = { dlg: n, info: null, warned: false };  // info arrives once its Improv answers
        // ESP Web Tools can append the dialog before its module has loaded;
        // it gets its shadow root when the element is upgraded.
        if (n.shadowRoot) hook(n);
        else customElements.whenDefined('ewt-install-dialog').then(() => { if (n.shadowRoot) hook(n); });
      }
    }
  }).observe(document.body, { childList: true });
}

// ---- the staging dialog ----

const STYLE = `
  :host {
    /* ESP Web Tools' dialog theme (its dialogStyles), so both dialogs match. */
    --roboto-font: Roboto, system-ui;
    --text-color: rgba(0, 0, 0, 0.6);
    --danger-color: #db4437;
    --md-sys-color-primary: #03a9f4;
    --md-sys-color-on-primary: #fff;
    --md-ref-typeface-brand: var(--roboto-font);
    --md-ref-typeface-plain: var(--roboto-font);
    --md-sys-color-surface: #fff;
    --md-sys-color-surface-container: #fff;
    --md-sys-color-surface-container-high: #fff;
    --md-sys-color-surface-container-highest: #f5f5f5;
    --md-sys-color-secondary-container: #e0e0e0;
    --md-sys-typescale-headline-font: var(--roboto-font);
    --md-sys-typescale-title-font: var(--roboto-font);
    font-family: var(--roboto-font);
  }
  a { color: var(--md-sys-color-primary); }
  ew-dialog { max-width: 390px; }
  div[slot="headline"] { padding-right: 48px; }
  ew-icon-button[slot="headline"] { position: absolute; right: 4px; top: 8px; }
  ew-icon-button[slot="headline"] svg { padding: 8px; color: var(--text-color); }
  ew-list { margin: 0 -24px; padding: 0; }
  ew-list-item svg { height: 24px; }
  ew-filled-text-field { display: block; margin-top: 16px; }
  p { margin: 0 0 12px; font-size: 14px; line-height: 20px; color: var(--text-color); }
  p.fine { margin: 16px 0 0; font-size: 12px; line-height: 16px; }
  p.center { text-align: center; }
  .drop { outline: 2px dashed transparent; outline-offset: -2px; border-radius: 12px; }
  .drop.over { outline-color: var(--md-sys-color-primary); }
  .error { color: var(--danger-color); }
  .detail { display: block; margin-top: 12px; font-size: 12px; color: var(--text-color); text-align: center; word-break: break-word; }
  input[type="file"] { display: none; }
`;

class StageDialog extends HTMLElement {
  constructor() {
    super();
    this.attachShadow({ mode: 'open' });
    this.port = null;     // a SerialPort the user already granted, or null to ask on Stage
    this.info = null;     // Improv device info, for the name shown
    this.startError = ''; // shown instead of the first page
    this.pkg = null;      // { file, bin, dat, pkt }
    this.busy = false;
  }

  connectedCallback() {
    this.shadowRoot.innerHTML = `<style>${STYLE}</style><ew-dialog open></ew-dialog><input type="file" accept=".zip,application/zip">`;
    this.dialog = this.shadowRoot.querySelector('ew-dialog');
    this.input = this.shadowRoot.querySelector('input');
    // Like ESP Web Tools: Escape and the scrim never close it, only the X or a button.
    this.dialog.addEventListener('cancel', e => e.preventDefault());
    this.dialog.addEventListener('closed', () => this.remove());
    this.input.addEventListener('change', () => { if (this.input.files[0]) this.load(this.input.files[0]); this.input.value = ''; });
    this.bodyOverflow = document.body.style.overflow;
    document.body.style.overflow = 'hidden';
    this.onUnload = e => { if (this.busy) { e.preventDefault(); e.returnValue = ''; } };
    window.addEventListener('beforeunload', this.onUnload);
    if (this.startError) this.showError(this.startError);
    else this.showChoose();
  }

  disconnectedCallback() {
    document.body.style.overflow = this.bodyOverflow;
    window.removeEventListener('beforeunload', this.onUnload);
  }

  close() { this.dialog.close(); }

  // headline: text or '' (none); closable: show the X.
  render(headline, content, actions, { closable = true } = {}) {
    const d = this.dialog;
    d.innerHTML =
      (headline ? `<div slot="headline">${esc(headline)}</div>` : '') +
      (closable ? `<ew-icon-button slot="headline" data-act="close" aria-label="Close">${ICON_CLOSE}</ew-icon-button>` : '') +
      `<div slot="content">${content}</div>` +
      (actions ? `<div slot="actions">${actions}</div>` : '');
    d.querySelectorAll('[data-act]').forEach(el => el.addEventListener('click', () => this.act(el.dataset.act)));
  }

  act(name) {
    if (name === 'close') this.close();
    else if (name === 'choose') this.input.click();
    else if (name === 'back') this.pkg ? this.showReview() : this.showChoose();
    else if (name === 'another') { this.pkg = null; this.showChoose(); }
    else if (name === 'stage') this.stage();
  }

  deviceLine() {
    const i = this.info;
    return i ? `${esc(i.name)} (${esc(String(i.chipFamily || '').split(' / ')[0])})` : 'your DFU32';
  }

  showChoose() {
    this.render('Stage nRF52 image', `
      <div class="drop">
        <p>Pick a DFU package for the repeater: the <b>.zip</b> from adafruit-nrfutil, such as a MeshCore release asset. The page checks it, then writes it to ${this.deviceLine()} over USB. WiFi, the API token and the target are kept.</p>
        <ew-list>
          <ew-list-item type="button" data-act="choose">${ICON_UPLOAD}<div slot="headline">Choose DFU .zip</div><div slot="supporting-text">or drop it here</div></ew-list-item>
        </ew-list>
      </div>`,
      `<ew-text-button data-act="close">Cancel</ew-text-button>`);
    const drop = this.dialog.querySelector('.drop');
    drop.addEventListener('dragover', e => { e.preventDefault(); drop.classList.add('over'); });
    drop.addEventListener('dragleave', () => drop.classList.remove('over'));
    drop.addEventListener('drop', e => {
      e.preventDefault();
      drop.classList.remove('over');
      const f = e.dataTransfer.files[0];
      if (f) this.load(f);
    });
  }

  async load(file) {
    // Before reading it into memory: a multi-GB drop would stall the tab.
    if (file.size > S.MAX_ZIP) {
      this.pkg = null;
      this.showError(`${file.name} can't be staged: it is ${kB(file.size)}, far too large for a DFU package.`);
      return;
    }
    this.render('Stage nRF52 image', `<ewt-page-progress label="Checking ${esc(file.name)}"></ewt-page-progress>`, '', { closable: false });
    try {
      const { bin, dat } = await S.loadPackage(new Uint8Array(await file.arrayBuffer()));
      const pkt = S.validate(bin, dat);
      this.pkg = { file, bin, dat, pkt, version: S.guessVersion(file.name) };
      this.showReview();
    } catch (e) {
      this.pkg = null;
      this.showError(`${file.name} can't be staged: ${e instanceof S.StageError ? e.message : 'it is not a readable zip (' + e.message + ')'}`);
    }
  }

  showReview() {
    const p = this.pkg, t = p.pkt;
    const kind = t.deviceType === S.ADAFRUIT_DEVICE_TYPE ? 'nRF52' : 'nRF54';
    this.render('Stage nRF52 image', `
      <ew-list>
        <ew-list-item>${ICON_FILE}
          <div slot="headline">${esc(p.file.name)}</div>
          <div slot="supporting-text">${kB(p.bin.length)} · ${kind} (${h4(t.deviceType)}) · SoftDevice ${t.sdReq.map(h4).join(', ')} · CRC OK</div>
        </ew-list-item>
      </ew-list>
      <ew-filled-text-field id="version" label="Version" required
        supporting-text="As the repeater's ver command prints it"></ew-filled-text-field>
      <p class="fine">Check that it's built for your repeater's board: DFU32 doesn't check that for an image staged here. Writes to ${this.deviceLine()}, then restarts it. Afterwards, flash the repeater from DFU32's page with <b>Flash from: staged image</b>.</p>`,
      `<ew-text-button data-act="another">Choose another</ew-text-button><ew-text-button data-act="stage">Stage</ew-text-button>`);
    const version = this.dialog.querySelector('#version');
    version.value = p.version;
    version.addEventListener('input', () => { p.version = version.value.trim(); this.checkFields(); });
    this.checkFields();
  }

  checkFields() {
    const p = this.pkg, d = this.dialog;
    const version = d.querySelector('#version'), btn = d.querySelector('[data-act="stage"]');
    const versionErr = utf8len(p.version) > 63 ? 'At most 63 bytes' : '';
    version.error = !!versionErr; version.errorText = versionErr;
    btn.disabled = !p.version || !!versionErr;
  }

  // ESP Web Tools' progress page: a spinner, or a ring with a percentage once pct is given.
  setProgress(label, pct) {
    let el = this.progressEl;
    if (!el || !el.isConnected) {
      this.render('Staging', '<ewt-page-progress style="white-space: pre-line"></ewt-page-progress>', '', { closable: false });
      el = this.progressEl = this.dialog.querySelector('ewt-page-progress');
    }
    el.label = `${label}\n\nKeep this page visible to prevent slow down`;
    el.progress = pct;
  }

  async stage() {
    const p = this.pkg;
    // Ask for the port first, while the click still counts as a user gesture.
    if (!this.port) {
      try { this.port = await navigator.serial.requestPort(); } catch (e) { return; }
    }
    this.busy = true;
    try {
      this.setProgress('Building the image');
      const image = await S.buildImage(p.bin, p.dat, {
        // No board: DFU32 checks the board only on images that name one (SPEC §8).
        version: p.version, manifestUrl: S.clipUtf8('usb:' + p.file.name, 255),
        stagedAt: Math.floor(Date.now() / 1000),
      });
      await writeStaging(this.port, image, (label, pct) => this.setProgress(label, pct));
    } catch (e) {
      this.busy = false;
      console.error('DFU32 staging failed', e);
      // A native-USB ESP32 re-enumerates on reset: that port is gone, so ask again next time.
      if (this.port && this.port.connected === false) this.port = null;
      this.showError(e.friendly || e.message, e.friendly ? e.message : '');
      return;
    }
    this.busy = false;
    this.render('', `<ewt-page-message label="Image staged"></ewt-page-message>
      <p class="fine center"><b>${esc(p.version)}</b> is on ${this.deviceLine()}. DFU32 is restarting; once it's back on WiFi, open its page and flash the repeater with <b>Flash from: staged image</b>.</p>`,
      `<ew-text-button data-act="close">Close</ew-text-button>`);
    this.dialog.querySelector('ewt-page-message').icon = OK_ICON;
  }

  showError(message, detail = '') {
    this.render('Error', `<ewt-page-message></ewt-page-message>${detail ? `<span class="detail">${esc(detail)}</span>` : ''}`,
      `<ew-text-button data-act="back">Back</ew-text-button><ew-text-button data-act="close">Close</ew-text-button>`);
    const m = this.dialog.querySelector('ewt-page-message');
    m.icon = ERROR_ICON;
    m.label = message;
  }
}
customElements.define('dfu32-stage-dialog', StageDialog);

class FriendlyError extends Error {
  constructor(friendly, cause) {
    super(cause && cause.message ? cause.message : String(cause || friendly));
    this.friendly = friendly;
  }
}

/** Reset into the ROM bootloader, find `staging`, write `image` there, verify, reset. Returns the partition. */
export async function writeStaging(port, image, onStep) {
  const { ESPLoader, Transport } = await import(ESPTOOL_JS);
  const transport = new Transport(port, TRACE);  // tracing: the Transport's own flag
  const log = [];
  const terminal = { clean() {}, writeLine: s => log.push(s), write: s => log.push(s) };
  // 115200 like ESP Web Tools: the boards' USB bridges are known good there.
  // The same read buffer ESP Web Tools opens the port with: Web Serial's default
  // is 255 bytes, and esptool-js leaves it at that unless told otherwise.
  const loader = new ESPLoader({ transport, baudrate: 115200, terminal, debugLogging: TRACE,
    serialOptions: { bufferSize: 8192 } });
  try {
    onStep('Connecting to the ESP32');
    try {
      await loader.main();
    } catch (e) {
      if (e && e.name === 'InvalidStateError') throw new FriendlyError(PORT_BUSY, e);  // port.open() on a port already open
      throw new FriendlyError("Couldn't start the ESP32's bootloader. Close other programs using the port, or hold BOOT, tap RESET, release BOOT and try again.", e);
    }
    onStep('Reading the partition table');
    const table = await loader.readFlash(S.PARTITION_TABLE_OFFSET, S.PARTITION_TABLE_SIZE);
    const part = S.findStaging(table);
    if (!part) throw new FriendlyError('This ESP32 has no DFU32 staging partition. Install DFU32 on it first.', new Error('no data partition "staging" with subtype 0x40'));
    if (image.length > part.size) {
      throw new FriendlyError(`The image (${kB(image.length)}) doesn't fit the staging partition (${kB(part.size)}).`, new Error('image larger than the partition'));
    }
    // The write is hundreds of KB, which esptool-js would hex-dump twice per
    // block: stop tracing here; the steps worth tracing are behind us.
    transport.tracing = false;
    onStep('Writing the image', 0);
    await loader.writeFlash({
      fileArray: [{ data: image, address: part.offset }],
      flashSize: 'keep', flashMode: 'keep', flashFreq: 'keep',
      eraseAll: false, compress: true,
      reportProgress: (_, written, total) => {
        if (written >= total) onStep('Checking what was written');
        else onStep('Writing the image', Math.floor((written / total) * 100));
      },
      // The stub reports the MD5 of the region it wrote; writeFlash throws on a mismatch.
      calculateMD5Hash: S.md5Hex,
    });
    onStep('Restarting DFU32');
    return part;
  } catch (e) {
    // Visible at the default console level, so a failure report can include it,
    // and with ?trace=1 the serial trace too, in one block to copy.
    if (log.length) console.warn('esptool-js log:\n' + log.join('\n'));
    if (TRACE && transport.traceLog) console.warn('esptool-js trace:\n' + transport.traceLog);
    throw e;
  } finally {
    // ESP Web Tools' hard reset: RTS first, then esptool-js' chip-specific release.
    try { await transport.setRTS(true); await sleep(100); await loader.after(); } catch (e) { /* port already gone */ }
    try { await transport.disconnect(); } catch (e) { /* USB re-enumerated on reset */ }
  }
}

export function openStageDialog({ port = null, info = null, error = '' } = {}) {
  const el = document.createElement('dfu32-stage-dialog');
  el.port = port;
  el.info = info;
  el.startError = error;
  document.body.appendChild(el);
  return el;
}

const button = document.getElementById('esp');
if (button) watch(button);
