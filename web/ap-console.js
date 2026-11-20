// SPDX-License-Identifier: MIT
// "Enable AP mode" on the installer page: turn on DFU32's access-point
// fallback over its serial console, once ESP Web Tools has closed its dialog
// (SPEC.md §9.4, §9.5).
//
// The page reopens the port ESP Web Tools was granted and types the console
// commands a person would: `ap password "<pw>"` (when one is given) and then
// `ap on`, and reads the fixed first line of each reply. It has no DOM code,
// so Node runs it: tools/test_ap_console_js.py drives it with a fake port.
//
// The console echoes what is typed, so the transcript carries the password:
// nothing here returns, shows or logs it, not even with ?trace=1.

export const MIN_PW = 8;   // ap::kMinPasswordLen (dfu32_core/ap.hpp)
export const MAX_PW = 63;  // ap::kMaxPasswordLen

const PROMPT = 'dfu32> ';  // kPromptPlain in main/console.cpp, once colours are stripped
const MAGIC = [0x49, 0x4D, 0x50, 0x52, 0x4F, 0x56];  // "IMPROV"
const IMPROV_VERSION = 1;
const PW_CMD = 'ap password ';

/** An esp_console argument in double quotes: split_argv unescapes only \\ and \" there. */
export const quoteArg = s => '"' + String(s).replace(/[\\"]/g, c => '\\' + c) + '"';

/**
 * '' when `pw` can be sent: empty (keep the device's password, or open), or a
 * WPA2 passphrase of 8-63 printable ASCII characters, as the firmware checks
 * it (ap::validate_ap_password). Otherwise the reason it can't.
 */
export function validateApPassword(pw) {
  if (pw === '') return '';
  if (pw.length < MIN_PW || pw.length > MAX_PW) return `The password needs ${MIN_PW} to ${MAX_PW} characters.`;
  if (!/^[\x20-\x7E]*$/.test(pw)) return 'The password can only use plain ASCII letters, digits, spaces and punctuation.';
  // The firmware's console reader takes "IMPROV" for the start of an Improv
  // Serial frame (SPEC §9.2), so the rest of the line would never reach the console.
  if (pw.includes('IMPROV')) return 'The password can’t contain “IMPROV”.';
  return '';
}

/**
 * The console lines to send. The password always goes first, so the access
 * point never comes up open, even for a moment; `ap on` alone keeps a
 * password the device already has.
 */
export const apCommands = pw => pw ? [PW_CMD + quoteArg(pw), 'ap on'] : ['ap on'];

/**
 * Console bytes in, readable text out, chunk by chunk: Improv Serial frames
 * are removed (byte level, so a frame's data can't fake a prompt), then the
 * bytes are decoded as UTF-8, then CSI escape sequences (the coloured prompt,
 * the terminal probe ESC [5n) and '\r' are dropped. Frames, characters and
 * escape sequences may all be split across chunks.
 */
export class ConsoleStream {
  constructor() {
    this.decoder = new TextDecoder();
    this.held = [];   // bytes that may start a frame: "IMPROV", then version, type, length
    this.skip = 0;    // frame bytes still to drop: data and checksum
    this.lf = false;  // drop one '\n' right after a frame (the client side sends one)
    this.esc = '';    // an escape sequence cut off at the end of the last chunk
  }

  feed(bytes) {
    const out = [];
    for (const b of bytes) {
      if (this.skip) { if (--this.skip === 0) this.lf = true; continue; }
      if (this.lf) { this.lf = false; if (b === 0x0A) continue; }
      this.byte(b, out);
    }
    let s = this.esc + this.decoder.decode(Uint8Array.from(out), { stream: true });
    const cut = /\x1b(\[[0-9;?]*)?$/.exec(s);
    this.esc = cut ? cut[0] : '';
    if (cut) s = s.slice(0, cut.index);
    return s.replace(/\x1b\[[0-9;?]*[A-Za-z]/g, '').replace(/\r/g, '');
  }

  // A frame is "IMPROV" | version | type | length | data[length] | checksum
  // (improv/serial_frame.hpp). The firmware writes its frames whole but not
  // always at a line start (the prompt has no newline), so they are matched
  // anywhere, and only with version 1 and a known type.
  byte(b, out) {
    const h = this.held, n = h.length;
    const fits = n < MAGIC.length ? b === MAGIC[n] : n === MAGIC.length ? b === IMPROV_VERSION : n === MAGIC.length + 1 ? b >= 1 && b <= 4 : true;
    if (!fits) {
      this.held = [];
      if (n === 0) { out.push(b); return; }
      // Not a frame: the held bytes were text. The rest may start a frame again.
      out.push(h[0]);
      for (const x of [...h.slice(1), b]) this.byte(x, out);
      return;
    }
    h.push(b);
    if (h.length === MAGIC.length + 3) {
      this.held = [];
      this.skip = b + 1;
    }
  }
}

/**
 * The outcome of one command so far, from the console text since it was sent:
 * 'ok' once the `expected` first line is seen, 'old' for firmware without the
 * command, { error } for a refusal, null while waiting. Only complete lines
 * count, so a reply split across chunks is read whole.
 */
export function classifyReply(text, expected) {
  const lines = text.split('\n');
  lines.pop();  // not finished yet
  for (const line of lines) {
    if (line.startsWith(expected)) return 'ok';
    if (line.startsWith('Unrecognized command')) return 'old';
    if (line.startsWith('Not saved: ') || line.startsWith('Usage:') || line.startsWith('AP mode is not in this build')) {
      return { error: line.trim() };
    }
  }
  return null;
}

const expectedReply = line => line.startsWith(PW_CMD) ? 'AP password set.' : line === 'ap on' ? 'AP mode on.' : null;

// A refusal is shown to the user. Should a line of the echo (which holds the
// password) ever be taken for one, show nothing of it.
function scrub(msg, line) {
  if (!line.startsWith(PW_CMD)) return msg;
  const pw = line.slice(PW_CMD.length + 1, -1).replace(/\\(.)/g, '$1');
  for (let i = 0; i + 4 <= pw.length; i++) if (msg.includes(pw.slice(i, i + 4))) return 'DFU32 refused the password.';
  return msg;
}

/**
 * Send `lines` to DFU32's console on an open `port` and wait for each reply.
 * Returns { ok: true, security: 'WPA2' | 'open' | null } or
 * { error: 'timeout' | 'old' | 'gone' | <the device's refusal> }. The port's
 * reader and writer are released before it returns; the port stays open.
 *
 * It first sends '\n' about once a second until a prompt arrives after it:
 * some boards reset when the port opens, and the REPL prints its prompt only
 * when it is ready for input. It never answers the REPL's terminal probe
 * (ESC [5n), so a REPL started now stays in dumb mode. Lines end in '\n'
 * alone: dumb mode would keep a '\r' as part of the line.
 */
export async function runCommands(port, lines, {
  now = () => Date.now(), sleep = ms => new Promise(r => setTimeout(r, ms)), syncMs = 15000, replyMs = 10000,
} = {}) {
  const reader = port.readable.getReader();
  const writer = port.writable.getWriter();
  const enc = new TextEncoder(), stream = new ConsoleStream();
  let text = '', gone = false, finished = false;
  const pump = (async () => {
    try {
      for (;;) {
        const { value, done } = await reader.read();
        if (done) break;
        if (value) text += stream.feed(value);
      }
    } catch (e) { /* unplugged, or the board re-enumerated */ }
    if (!finished) gone = true;
  })();
  const send = async s => {
    try { await writer.write(enc.encode(s)); } catch (e) { gone = true; }
  };
  // Polls `check` until it answers, the port goes or `ms` pass.
  const wait = async (check, ms) => {
    const end = now() + ms;
    for (;;) {
      const r = check();
      if (r) return r;
      if (gone) return { error: 'gone' };
      if (now() >= end) return { error: 'timeout' };
      await sleep(20);
    }
  };
  try {
    // A fresh prompt: after our latest '\n', and the last thing received.
    let mark = Infinity, last = -Infinity;
    const synced = await wait(() => {
      const at = text.length - PROMPT.length;
      if (text.endsWith(PROMPT) && at >= mark) return 'ok';
      if (now() - last >= 1000) {
        last = now();
        mark = text.length;
        send('\n');
      }
      return null;
    }, syncMs);
    if (synced !== 'ok') return synced;

    let security = null;
    for (const line of lines) {
      const expected = expectedReply(line);
      if (!expected) throw new Error(`no known reply to ${line.split(' ').slice(0, 2).join(' ')}`);
      const start = text.length;
      await send(line + '\n');
      const r = await wait(() => classifyReply(text.slice(start), expected), replyMs);
      if (r === 'old') return { error: 'old' };
      if (r !== 'ok') return { error: r.error === 'timeout' || r.error === 'gone' ? r.error : scrub(r.error, line) };
      // Then the next prompt, after the status the command prints.
      const t = () => text.slice(start);
      const after = () => { const s = t(), i = s.indexOf('\n' + expected); return i < 0 ? 0 : i; };
      const p = await wait(() => t().indexOf(PROMPT, after()) >= 0 ? 'ok' : null, replyMs);
      if (p !== 'ok') return p;
      const m = /^ *ap mode +on, (open|WPA2)$/m.exec(t());
      if (m) security = m[1];
    }
    return { ok: true, security };
  } finally {
    finished = true;
    try { await reader.cancel(); } catch (e) { /* already gone */ }
    await pump;
    reader.releaseLock();
    writer.releaseLock();
  }
}
