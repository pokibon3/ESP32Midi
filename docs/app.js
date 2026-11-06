// ESP32 MIDI Box Player: plays Standard MIDI Files to a BLE-MIDI device
// through Web Bluetooth (Chrome / Edge / Android, iPhone: Bluefy etc.)
'use strict';

const MIDI_SERVICE = '03b80e5a-ede8-4b33-a751-6ce34ec4c700';
const MIDI_CHAR = '7772e5db-3868-4112-a1a9-f2669d106bf3';

const $ = (id) => document.getElementById(id);
const ui = {
  connect: $('connect'), status: $('status'), devName: $('devName'), noBle: $('noBle'),
  title: $('title'), pos: $('pos'), dur: $('dur'), seek: $('seek'),
  play: $('play'), stop: $('stop'), prev: $('prev'), next: $('next'), panic: $('panic'),
  vol: $('vol'), volVal: $('volVal'), add: $('add'), file: $('file'), list: $('list'), empty: $('empty'),
  repeat: $('repeat'), diag: $('diag'),
};

// ---------------------------------------------------------------- BLE-MIDI
const ble = {
  device: null, chr: null,
  maxPayload: 100,   // shrinks to 20 if the link has the default 23-byte MTU
  queue: [],         // MIDI messages (Uint8Array) waiting to be sent
  busy: false,
};

async function connect() {
  try {
    if (!ble.device) {
      ble.device = await navigator.bluetooth.requestDevice({ filters: [{ services: [MIDI_SERVICE] }] });
      ble.device.addEventListener('gattserverdisconnected', onDisconnected);
    }
    setStatus('connecting');
    const server = await ble.device.gatt.connect();
    const service = await server.getPrimaryService(MIDI_SERVICE);
    ble.chr = await service.getCharacteristic(MIDI_CHAR);
    try { await ble.chr.startNotifications(); } catch (_) { /* input is not used */ }
    ble.maxPayload = 100;
    setStatus('connected');
    sendVolume();
  } catch (e) {
    if (e.name !== 'NotFoundError') alert('接続できませんでした: ' + e.message);  // NotFound = chooser cancelled
    setStatus(ble.device && ble.device.gatt.connected ? 'connected' : 'disconnected');
  }
}

function onDisconnected() {
  ble.chr = null;
  ble.queue.length = 0;
  setStatus('disconnected');
  if (player.playing) pause();
}

function setStatus(s) {
  ui.status.dataset.state = s;
  ui.status.textContent = { connected: '接続中', connecting: '接続しています…', disconnected: '未接続' }[s];
  ui.devName.textContent = ble.device ? ble.device.name || '' : '';
  ui.connect.textContent = s === 'connected' ? '切断' : ble.device ? '再接続' : 'BLE-MIDI に接続';
}

// at: the intended time of the message on the performance.now() clock. It goes
// out as the BLE-MIDI timestamp, so the synth can undo transport jitter.
function send(msg, at = performance.now()) {
  if (!ble.chr) return;
  ble.queue.push({ m: msg, at });
  pump();
}

const diag = { late: 0, size: 0 };

// BLE-MIDI packet: header (10hhhhhh = timestamp bits 12-7), then per message
// a timestamp byte (1lllllll = bits 6-0) and the message. Running status drops
// the status byte, and the timestamp too when it is unchanged. Timestamps in a
// packet must not go backwards (a smaller low part means the low bits wrapped).
// SysEx goes in packets of its own: continuation packets carry only the
// header, and the closing F7 gets a timestamp.
async function pump() {
  if (ble.busy) return;
  ble.busy = true;
  try {
    while (ble.queue.length && ble.chr) {
      const packets = [];
      let taken = 0;
      const first = ble.queue[0];
      const ts0 = Math.floor(first.at) & 0x1fff;
      const hdr = 0x80 | (ts0 >> 7);
      if (first.m[0] === 0xf0) {
        const tsb = 0x80 | (ts0 & 0x7f);
        const body = first.m.subarray(0, first.m.length - 1);  // F0 ... (without F7)
        taken = 1;
        let i = 0, firstPkt = true;
        while (i < body.length || firstPkt) {
          const pkt = [hdr];
          if (firstPkt) pkt.push(tsb);
          firstPkt = false;
          while (i < body.length && pkt.length < ble.maxPayload) pkt.push(body[i++]);
          if (i >= body.length && pkt.length + 2 <= ble.maxPayload) { pkt.push(tsb, 0xf7); packets.push(pkt); break; }
          packets.push(pkt);
          if (i >= body.length) { packets.push([hdr, tsb, 0xf7]); break; }
        }
      } else {
        const pkt = [hdr];
        let hi = ts0 >> 7, lastLow = -1, running = 0;
        while (taken < ble.queue.length) {
          const { m, at } = ble.queue[taken];
          if (m[0] === 0xf0) break;
          const ts = Math.floor(at) & 0x1fff, h = ts >> 7, low = ts & 0x7f;
          let wrap = false;
          if (h !== hi) {
            if (h !== ((hi + 1) & 0x3f) || lastLow < 0 || low >= lastLow) break;
            wrap = true;
          } else if (low < lastLow) {
            break;  // earlier than the previous message: next packet
          }
          let status = m[0], data = m.subarray(1);
          if ((status & 0xf0) === 0x80) { status = 0x90 | (status & 0x0f); data = new Uint8Array([m[1], 0]); }
          const bytes = status === running
            ? (low === lastLow ? [...data] : [0x80 | low, ...data])
            : [0x80 | low, status, ...data];
          if (pkt.length + bytes.length > ble.maxPayload) break;
          pkt.push(...bytes);
          if (wrap) hi = h;
          running = status;
          lastLow = low;
          taken++;
        }
        packets.push(pkt);
      }
      try {
        const late = performance.now() - first.at;
        for (const pkt of packets) await write(new Uint8Array(pkt));
        ble.queue.splice(0, taken);
        diag.late = Math.max(diag.late, late);
        diag.size = Math.max(diag.size, ...packets.map((p) => p.length));
      } catch (e) {
        if (ble.maxPayload > 20) { ble.maxPayload = 20; continue; }  // retry with small packets
        console.warn('BLE write failed', e);
        ble.queue.splice(0, taken);
        if (!ble.device || !ble.device.gatt.connected) break;
      }
    }
  } finally {
    ble.busy = false;
  }
}

function write(bytes) {
  return ble.chr.writeValueWithoutResponse ? ble.chr.writeValueWithoutResponse(bytes) : ble.chr.writeValue(bytes);
}

function panic() {
  ble.queue.length = 0;  // drop what is still waiting
  for (let ch = 0; ch < 16; ch++) {
    send(new Uint8Array([0xb0 | ch, 64, 0]));
    send(new Uint8Array([0xb0 | ch, 123, 0]));
    send(new Uint8Array([0xb0 | ch, 120, 0]));
  }
}

const GM_ON = new Uint8Array([0xf0, 0x7e, 0x7f, 0x09, 0x01, 0xf7]);

let volTimer = 0;
function sendVolume() {
  clearTimeout(volTimer);
  volTimer = setTimeout(() => {
    const v = Math.round(ui.vol.value / 100 * 16383);
    send(new Uint8Array([0xf0, 0x7f, 0x7f, 0x04, 0x01, v & 0x7f, v >> 7, 0xf7]));  // GM master volume
  }, 30);
}

// ---------------------------------------------------------------- player
const player = {
  song: null,      // {id, name, events, duration}
  index: -1,       // position in library
  playing: false,
  next: 0,         // next event index
  startAt: 0,      // performance.now() of song time 0
  offset: 0,       // song time while paused (ms)
  timer: 0,
};

function songTime() {
  return player.playing ? performance.now() - player.startAt : player.offset;
}

function tick() {
  const now = songTime();
  const ev = player.song.events;
  while (player.next < ev.length && ev[player.next].t <= now) {
    const e = ev[player.next++];
    send(e.data, player.startAt + e.t);
  }
  if (player.next >= ev.length && now >= player.song.duration + 500) {
    finish();
    return;
  }
  updatePosition();
}

async function play() {
  if (!player.song) { if (library.length) await load(0); else return; }
  if (!ble.chr) { alert('先に BLE-MIDI に接続してください'); return; }
  if (player.playing) return;
  if (player.offset === 0 && player.next === 0) {
    send(GM_ON);
    sendVolume();
    player.offset = -300;  // let the synth finish the reset
  } else {
    chase(player.offset);
  }
  player.startAt = performance.now() - player.offset;
  player.playing = true;
  player.timer = setInterval(tick, 4);
  keepAwake(true);
  updateButtons();
}

function pause() {
  if (!player.playing) return;
  player.offset = Math.max(0, songTime());
  player.playing = false;
  clearInterval(player.timer);
  panic();
  keepAwake(false);
  updateButtons();
}

function stop() {
  const was = player.playing;
  player.playing = false;
  clearInterval(player.timer);
  if (was || player.next) panic();
  player.offset = 0;
  player.next = 0;
  keepAwake(false);
  updatePosition();
  updateButtons();
}

function finish() {
  stop();
  if (player.index + 1 < library.length) load(player.index + 1).then(play);
  else if (ui.repeat.checked && library.length) load(0).then(play);
}

function seek(ms) {
  const wasPlaying = player.playing;
  if (wasPlaying) { player.playing = false; clearInterval(player.timer); }
  panic();
  const ev = player.song.events;
  let lo = 0, hi = ev.length;
  while (lo < hi) { const mid = (lo + hi) >> 1; if (ev[mid].t < ms) lo = mid + 1; else hi = mid; }
  player.next = lo;
  player.offset = ms;
  if (wasPlaying) play(); else { chase(ms); updatePosition(); }
}

// Re-sends the controller state that the skipped part of the song set up
// (latest value per controller, in an order that keeps bank/RPN semantics)
function chase(ms) {
  const last = new Map();
  for (const e of player.song.events) {
    if (e.t >= ms) break;
    const s = e.data[0], type = s & 0xf0;
    if (type === 0xb0 && e.data[1] !== 64 && e.data[1] < 120) last.set(s * 256 + e.data[1], e.data);
    else if (type === 0xc0 || type === 0xe0) last.set(s * 256, e.data);
  }
  const rank = (d) => {
    const type = d[0] & 0xf0;
    if (type === 0xb0) return { 0: 0, 32: 1, 101: 3, 100: 4, 6: 5, 38: 6 }[d[1]] ?? 7;
    return type === 0xc0 ? 2 : 8;
  };
  [...last.values()].sort((a, b) => rank(a) - rank(b)).forEach(send);
}

// ---------------------------------------------------------------- library (IndexedDB)
let library = [];   // [{id, name, buffer}]
let db = null;

function openDb() {
  return new Promise((resolve) => {
    try {
      const req = indexedDB.open('esp32-midi-box-player', 1);
      req.onupgradeneeded = () => req.result.createObjectStore('songs', { keyPath: 'id', autoIncrement: true });
      req.onsuccess = () => resolve(req.result);
      req.onerror = () => resolve(null);
    } catch (_) { resolve(null); }
  });
}

function dbOp(mode, fn) {
  return new Promise((resolve) => {
    if (!db) return resolve(null);
    try {
      const tx = db.transaction('songs', mode);
      const req = fn(tx.objectStore('songs'));
      tx.oncomplete = () => resolve(req && req.result);
      tx.onerror = () => resolve(null);
    } catch (_) { resolve(null); }
  });
}

async function addFiles(files) {
  for (const f of files) {
    const buffer = await f.arrayBuffer();
    try { parseSMF(buffer); } catch (e) { alert(`${f.name}: ${e.message}`); continue; }
    const item = { name: f.name.replace(/\.(mid|midi|kar|rmi)$/i, ''), buffer };
    const id = await dbOp('readwrite', (s) => s.add(item));
    item.id = id ?? `mem-${Date.now()}-${Math.random()}`;
    library.push(item);
  }
  renderList();
}

async function removeSong(i) {
  const item = library[i];
  if (player.index === i) { stop(); player.song = null; player.index = -1; showSong(); }
  else if (player.index > i) player.index--;
  library.splice(i, 1);
  await dbOp('readwrite', (s) => s.delete(item.id));
  renderList();
}

async function load(i) {
  stop();
  const item = library[i];
  const smf = parseSMF(item.buffer);
  player.song = { name: item.name, events: smf.events, duration: smf.duration };
  player.index = i;
  showSong();
  renderList();
}

// ---------------------------------------------------------------- UI
const fmt = (ms) => {
  const s = Math.max(0, Math.floor(ms / 1000));
  return `${Math.floor(s / 60)}:${String(s % 60).padStart(2, '0')}`;
};

function showSong() {
  ui.title.textContent = player.song ? player.song.name : '曲を選んでください';
  ui.dur.textContent = fmt(player.song ? player.song.duration : 0);
  ui.seek.max = player.song ? Math.ceil(player.song.duration) : 0;
  updatePosition();
  updateButtons();
}

let seeking = false;
function updatePosition() {
  const t = player.song ? Math.min(songTime(), player.song.duration) : 0;
  ui.pos.textContent = fmt(t);
  if (!seeking) ui.seek.value = Math.max(0, t);
}

function updateButtons() {
  ui.play.textContent = player.playing ? '❚❚' : '▶';
  ui.play.setAttribute('aria-label', player.playing ? '一時停止' : '再生');
  const has = !!player.song;
  ui.stop.disabled = ui.seek.disabled = !has;
  ui.prev.disabled = !has || player.index <= 0;
  ui.next.disabled = !has || player.index >= library.length - 1;
  ui.play.disabled = !library.length;
}

function renderList() {
  ui.list.innerHTML = '';
  ui.empty.hidden = library.length > 0;
  library.forEach((item, i) => {
    const li = document.createElement('li');
    li.className = i === player.index ? 'current' : '';
    const b = document.createElement('button');
    b.className = 'song';
    b.textContent = item.name;
    b.onclick = async () => { await load(i); if (ble.chr) play(); };
    const del = document.createElement('button');
    del.className = 'del';
    del.textContent = '✕';
    del.setAttribute('aria-label', `${item.name} を削除`);
    del.onclick = () => removeSong(i);
    li.append(b, del);
    ui.list.append(li);
  });
  updateButtons();
}

let wakeLock = null;
async function keepAwake(on) {
  try {
    if (on && !wakeLock && navigator.wakeLock) wakeLock = await navigator.wakeLock.request('screen');
    if (!on && wakeLock) { await wakeLock.release(); wakeLock = null; }
  } catch (_) { wakeLock = null; }
}

// ---------------------------------------------------------------- wiring
ui.connect.onclick = () => {
  if (ble.device && ble.device.gatt.connected) { pause(); ble.device.gatt.disconnect(); }
  else connect();
};
ui.play.onclick = () => (player.playing ? pause() : play());
ui.stop.onclick = stop;
ui.prev.onclick = () => player.index > 0 && load(player.index - 1).then(() => ble.chr && play());
ui.next.onclick = () => player.index < library.length - 1 && load(player.index + 1).then(() => ble.chr && play());
ui.panic.onclick = () => { pause(); panic(); };
ui.add.onclick = () => ui.file.click();
ui.file.onchange = () => { addFiles([...ui.file.files]); ui.file.value = ''; };
ui.seek.oninput = () => { seeking = true; ui.pos.textContent = fmt(+ui.seek.value); };
ui.seek.onchange = () => { seeking = false; if (player.song) seek(+ui.seek.value); };
ui.vol.oninput = () => {
  ui.volVal.textContent = ui.vol.value;
  try { localStorage.setItem('vol', ui.vol.value); } catch (_) {}
  sendVolume();
};
setInterval(() => {
  if (!ble.chr) { ui.diag.textContent = ''; return; }
  ui.diag.textContent = `送信の遅れ 最大 ${Math.round(diag.late)} ms ・ パケット 最大 ${diag.size} B (上限 ${ble.maxPayload} B) ・ 待ち ${ble.queue.length}`;
  diag.late = 0;
  diag.size = 0;
}, 1000);

document.addEventListener('visibilitychange', () => {
  if (document.visibilityState === 'visible' && player.playing) keepAwake(true);
});

(async function init() {
  if (!navigator.bluetooth) {
    ui.noBle.hidden = false;
    ui.connect.disabled = true;
  }
  try { const v = localStorage.getItem('vol'); if (v) ui.vol.value = v; } catch (_) {}
  ui.volVal.textContent = ui.vol.value;
  setStatus('disconnected');
  db = await openDb();
  library = (await dbOp('readonly', (s) => s.getAll())) || [];
  renderList();
  showSong();
})();
