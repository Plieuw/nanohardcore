// spl.js - SPL (bootloader-partitie) alleen-lezen uitlezen via HARDCORE (fase D1).
// De pedal stuurt de partitie in blokken als SysEx:
//   F0 7D 4E 43 5C <7-bit verpakt: "NHS1", offset u32, totaal u32, crc32 u32, data> F7
// Er wordt niets naar de pedal geschreven behalve het startsein (CC 84: 'S' 'P' 'L').

/** Livtra 7-bit uitpakken (zoals native 0x400184ae): per groep een byte met de hoge
 * bits (bit i = hoogste bit van byte i), gevolgd door maximaal 7 bytes. */
export function unpack7(packed) {
  const out = [];
  for (let i = 0; i < packed.length;) {
    const hi = packed[i++];
    for (let b = 0; b < 7 && i < packed.length; b++) out.push((((hi >> b) & 1) << 7) | packed[i++]);
  }
  return Uint8Array.from(out);
}

export function crc32(bytes) {
  let c = ~0 >>> 0;
  for (const x of bytes) { c ^= x; for (let k = 0; k < 8; k++) c = (c >>> 1) ^ (0xedb88320 & -(c & 1)); }
  return (~c) >>> 0;
}

const u32 = (b, o) => (b[o] | b[o + 1] << 8 | b[o + 2] << 16 | b[o + 3] << 24) >>> 0;

/** Eén SysEx-bericht van de pedal ontleden; null als het geen SPL-blok is. */
export function parseChunk(msg) {
  if (msg.length < 8 || msg[0] !== 0xf0 || msg[1] !== 0x7d || msg[2] !== 0x4e || msg[3] !== 0x43 || msg[4] !== 0x5c || msg[msg.length - 1] !== 0xf7) return null;
  const raw = unpack7(msg.subarray(5, msg.length - 1));
  if (raw.length < 16 || raw[0] !== 0x4e || raw[1] !== 0x48 || raw[2] !== 0x53 || raw[3] !== 0x31) throw Error('Onbekend SPL-blok.');
  const offset = u32(raw, 4), total = u32(raw, 8), crc = u32(raw, 12), data = raw.subarray(16);
  if (crc32(data) !== crc) throw Error(`CRC klopt niet bij offset ${offset}.`);
  return {offset, total, data};
}

/** Blokken verzamelen tot de partitie compleet is. */
export class Collector {
  constructor() { this.total = 0; this.buf = null; this.have = 0; this.seen = new Set(); }
  add(chunk) {
    if (!this.buf) { if (!chunk.total || chunk.total > 0x100000) throw Error('Onwaarschijnlijke grootte.'); this.total = chunk.total; this.buf = new Uint8Array(chunk.total); }
    if (chunk.total !== this.total || chunk.offset + chunk.data.length > this.total) throw Error('Blok past niet.');
    if (!this.seen.has(chunk.offset)) { this.buf.set(chunk.data, chunk.offset); this.seen.add(chunk.offset); this.have += chunk.data.length; }
    return this.have >= this.total;
  }
}

/** SHA-256 van de SPL-dump die re-nanocore publiceerde (evidence/firmware-decoding.json). */
export const KNOWN_SPL_SHA256 = '2c254261882452a9fb0ace73f8547616dbff5f767dd57d25c2a06282fed6cf1f';

export async function sha256hex(bytes) {
  const d = await crypto.subtle.digest('SHA-256', bytes);
  return [...new Uint8Array(d)].map(x => x.toString(16).padStart(2, '0')).join('');
}

// ---- pagina ----
if (typeof document !== 'undefined') {
  const $ = id => document.getElementById(id);
  const status = (t, d = '') => { $('status').textContent = t; $('detail').textContent = d; };
  $('start').onclick = async () => {
    $('start').disabled = true; $('error').hidden = true; $('save').hidden = true;
    try {
      const midi = await navigator.requestMIDIAccess({sysex: true});
      const pick = list => [...list.values()].find(p => /nano\s*core/i.test(p.name || '')) || [...list.values()][0];
      const out = pick(midi.outputs), inp = pick(midi.inputs);
      if (!out || !inp) throw Error('Geen NanoCore gevonden via USB-MIDI. Sluit de pedal met USB aan en wacht tot HARDCORE draait.');
      const col = new Collector();
      const done = new Promise((resolve, reject) => {
        const timer = setTimeout(() => reject(Error('Geen (volledige) dump ontvangen binnen 60 s. Draait HARDCORE beta4 of nieuwer, en is het scherm overgenomen?')), 60000);
        inp.onmidimessage = ev => {
          try {
            const c = parseChunk(ev.data); if (!c) return;
            const complete = col.add(c);
            $('progress').value = Math.round(100 * col.have / col.total);
            status('Bezig met lezen…', `${col.have} van ${col.total} bytes`);
            if (complete) { clearTimeout(timer); resolve(col.buf); }
          } catch (e) { clearTimeout(timer); reject(e); }
        };
      });
      for (const v of [0x53, 0x50, 0x4c]) out.send([0xb0, 84, v]);   // CC 84: S P L
      status('Startsein verstuurd', 'Op het scherm van de pedal verschijnt "SPL LEZEN".');
      const spl = await done;
      const sha = await sha256hex(spl);
      const known = sha === KNOWN_SPL_SHA256;
      status(known ? 'Klaar: identiek aan de bekende SPL' : 'Klaar: andere SPL dan de bekende',
             `${spl.length} bytes, SHA-256 ${sha}` + (known ? '' : ' (dat kan: andere hardware- of bootloaderversie; bewaar het bestand)'));
      const url = URL.createObjectURL(new Blob([spl], {type: 'application/octet-stream'}));
      $('save').href = url; $('save').download = 'nanocore-spl.bin'; $('save').hidden = false;
    } catch (e) { $('error').textContent = e.message; $('error').hidden = false; status('Gestopt'); }
    finally { $('start').disabled = false; }
  };
}
