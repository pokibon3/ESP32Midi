// Standard MIDI File (format 0/1) parser.
// parseSMF(ArrayBuffer) -> { events: [{t, data}], duration, tracks, name }
//   t:    time in milliseconds from the start (tempo map applied)
//   data: Uint8Array of a complete MIDI message (channel messages with status,
//         SysEx as F0 ... F7). Meta events other than tempo are dropped.
(function (global) {
  'use strict';

  function parseSMF(buffer) {
    const d = new DataView(buffer);
    let p = 0;
    const u8 = () => d.getUint8(p++);
    const u16 = () => { const v = d.getUint16(p); p += 2; return v; };
    const u32 = () => { const v = d.getUint32(p); p += 4; return v; };
    const str4 = () => String.fromCharCode(u8(), u8(), u8(), u8());
    const vlq = () => {
      let v = 0;
      for (let i = 0; i < 4; i++) {
        const b = u8();
        v = (v << 7) | (b & 0x7f);
        if (!(b & 0x80)) break;
      }
      return v;
    };

    // RIFF-wrapped MIDI (.rmi)
    if (str4() === 'RIFF') {
      p = 12;
      while (p + 8 <= d.byteLength) {
        const id = str4(), len = d.getUint32(p, true); p += 4;
        if (id === 'data') break;
        p += len + (len & 1);
      }
    } else {
      p = 0;
    }
    if (str4() !== 'MThd') throw new Error('MIDI ファイルではありません');
    const hdrLen = u32();
    const hdrStart = p;
    u16();  // format
    const ntracks = u16();
    const division = u16();
    p = hdrStart + hdrLen;
    if (division & 0x8000) {
      // SMPTE time: ticks per second = fps * ticks/frame
      var smpteMsPerTick = 1000 / ((256 - (division >> 8)) * (division & 0xff));
    }
    const tpq = division & 0x7fff;

    const raw = [];   // {tick, order, data | tempo}
    let name = '';
    let order = 0;
    for (let tr = 0; tr < ntracks && p + 8 <= d.byteLength; tr++) {
      const id = str4();
      const len = u32();
      const end = Math.min(p + len, d.byteLength);
      if (id !== 'MTrk') { p = end; continue; }
      let tick = 0, status = 0;
      while (p < end) {
        tick += vlq();
        let b = d.getUint8(p);
        if (b >= 0x80) { p++; } else if (status) { b = status; } else { p++; continue; }
        if (b === 0xff) {
          const type = u8(), mlen = vlq();
          if (type === 0x51 && mlen === 3) {
            const us = (d.getUint8(p) << 16) | (d.getUint8(p + 1) << 8) | d.getUint8(p + 2);
            raw.push({ tick, order: order++, tempo: us });
          } else if (type === 0x03 && !name && tr <= 1) {
            name = decodeText(new Uint8Array(buffer, p, mlen));
          } else if (type === 0x2f) {
            p += mlen;
            break;
          }
          p += mlen;
        } else if (b === 0xf0 || b === 0xf7) {
          const slen = vlq();
          const body = new Uint8Array(buffer, p, Math.min(slen, end - p));
          p += slen;
          if (b === 0xf0) {
            const m = new Uint8Array(body.length + 1);
            m[0] = 0xf0; m.set(body, 1);
            if (m[m.length - 1] !== 0xf7) continue;  // split SysEx: not supported
            raw.push({ tick, order: order++, data: m });
          }
        } else if (b >= 0xf0) {
          // system common / realtime inside a file: skip the byte
        } else {
          status = b;
          const type = b & 0xf0;
          const n = type === 0xc0 || type === 0xd0 ? 1 : 2;
          const m = new Uint8Array(n + 1);
          m[0] = b;
          for (let i = 1; i <= n; i++) m[i] = u8() & 0x7f;
          // Note On with velocity 0 is a Note Off
          raw.push({ tick, order: order++, data: m });
        }
      }
      p = end;
    }

    // Merge by tick; at equal ticks note-offs first so re-struck notes are not cut
    raw.sort((a, b) => a.tick - b.tick || rank(a) - rank(b) || a.order - b.order);

    const events = [];
    let usPerQ = 500000, lastTick = 0, ms = 0;
    for (const e of raw) {
      ms += smpteMsPerTick ? (e.tick - lastTick) * smpteMsPerTick
                           : (e.tick - lastTick) * usPerQ / tpq / 1000;
      lastTick = e.tick;
      if (e.tempo) { usPerQ = e.tempo; continue; }
      events.push({ t: ms, data: e.data });
    }
    return { events, duration: ms, tracks: ntracks, name };
  }

  function rank(e) {
    if (e.tempo) return 0;
    const s = e.data[0] & 0xf0;
    if (s === 0x80 || (s === 0x90 && e.data[2] === 0)) return 1;
    return 2;
  }

  function decodeText(bytes) {
    try { return new TextDecoder('shift_jis', { fatal: true }).decode(bytes).trim(); } catch (_) {}
    try { return new TextDecoder('utf-8').decode(bytes).trim(); } catch (_) {}
    return '';
  }

  global.parseSMF = parseSMF;
  if (typeof module !== 'undefined') module.exports = { parseSMF };
})(typeof window !== 'undefined' ? window : globalThis);
