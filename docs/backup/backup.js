// nanohardcore: alleen-lezen backup van het NanoCore OS-partitie.
// Deze pagina roept nooit RawOS.program() aan: er wordt niets naar de pedal geschreven.
import {WebUSBTransport, RawOS, USB_FILTER, OS_SIZE} from './dfu.js';
import {identifyImage, sha256} from './firmware.js';

const $ = id => document.getElementById(id);
// Versieherkenning met de exacte stock-hashes uit re-nanocore (runtime opgehaald, niet in deze repo).
const MANIFESTS = ['patch.json','patch-1.11.json','patch-1.12.json','octave-1.12.json','patch-1.2.json','octave-1.2.json']
  .map(p => 'https://jcuhnoio.github.io/re-nanocore/' + p);
let busy = false;

const status = (t, d = '') => { $('status').textContent = t; $('detail').textContent = d; };
const ok = () => !!navigator.usb && !!navigator.requestMIDIAccess && isSecureContext;

function controls() {
  const ready = !busy && ok() && $('audio').checked;
  $('start').disabled = !ready; $('connect').disabled = !ready;
}

async function run(fn) {
  if (busy) return; busy = true; controls(); $('error').hidden = true;
  try { await fn(); }
  catch (e) { $('error').hidden = false; $('error').textContent = e.message;
              status('Gestopt', 'Er is niets geschreven. Haal de USB-kabel eruit en zet de pedal opnieuw aan; hij start normaal op.'); }
  finally { busy = false; controls(); }
}

async function start() {
  if (!ok()) throw Error('Open deze pagina in desktop Chrome of Edge.');
  if (!$('audio').checked) throw Error('Haal eerst koptelefoon en audiokabels eruit.');
  const boot = (await navigator.usb.getDevices()).filter(d => d.vendorId === USB_FILTER.vendorId && d.productId === USB_FILTER.productId);
  if (boot.length === 1) return read(boot[0]);
  status('Verbinden met NanoCore', 'Sta MIDI-toegang toe, inclusief system-exclusive.');
  const midi = await navigator.requestMIDIAccess({sysex: true});
  const ports = [...midi.outputs.values()].filter(p => /nanocore/i.test(p.name));
  if (ports.length > 1) throw Error('Sluit maar één NanoCore aan.');
  if (ports.length === 1) {   // stock-updater openen (Lupgrader); dit wist niets
    await ports[0].open(); ports[0].send([0xf0, 0x7d, 0x44, 0x46, 0x55, 0x01, 0xf7]);
    await new Promise(r => setTimeout(r, 1200)); await ports[0].close();
  }
  $('connect').hidden = false;
  status('Kies de bootloader', 'Klik "Verbind bootloader" en kies Livtra Lupgrader.');
}

async function read(device) {
  const t = await WebUSBTransport.open(device);
  try {
    const dfu = new RawOS(t, {onProgress: ({completed, total}) => {
      $('progress').value = total ? completed / total * 100 : 0;
      status('Firmware lezen…', `${Math.round(completed / 1024)} / ${Math.round(total / 1024)} KB`);
    }});
    const image = await dfu.readPartition();
    if (image.length !== OS_SIZE) throw Error('Onvolledige readback.');
    const hash = await sha256(image);
    let version = 'onbekend', kind = '';
    try {
      const patches = await Promise.all(MANIFESTS.map(u => fetch(u).then(r => { if (!r.ok) throw Error(); return r.json(); })));
      const s = await identifyImage(image, patches); version = s.version; kind = s.kind;
    } catch { /* herkenning mislukt of offline: backup blijft geldig */ }
    const url = URL.createObjectURL(new Blob([image], {type: 'application/octet-stream'}));
    const a = $('download'); a.href = url; a.download = `nanocore-${version}-${kind || 'os'}-backup.bin`; a.hidden = false;
    $('result').textContent = `Versie: ${version}${kind ? ' (' + (kind === 'stock' ? 'stock' : 're-nanocore-patch') + ')' : ''}\nSHA-256: ${hash}\nGrootte: ${image.length} bytes`;
    status('Klaar, er is niets geschreven', 'Download de backup en bewaar hem goed. Haal daarna de USB-kabel eruit en zet de pedal opnieuw aan: hij start normaal op.');
  } finally { await t.close(); }   // nooit resetten of schrijven
}

$('audio').onchange = controls;
$('start').onclick = () => run(start);
$('connect').onclick = () => run(async () => read(await navigator.usb.requestDevice({filters: [USB_FILTER]})));
if (!ok()) status('Niet ondersteund', 'Gebruik desktop Chrome of Edge (WebUSB + Web MIDI).');
controls();
window.__backupReady = true;
