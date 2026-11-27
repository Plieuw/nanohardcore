// nanohardcore: alleen-lezen backup van het NanoCore OS-partitie.
// Deze pagina roept nooit RawOS.program() aan: er wordt niets naar de pedal geschreven.
import {WebUSBTransport, RawOS, USB_FILTER, OS_SIZE} from './dfu.js';
import {identifyImage, sha256} from './firmware.js';

const $ = id => document.getElementById(id);
// Versieherkenning met de exacte stock-hashes uit re-nanocore (runtime opgehaald, niet in deze repo).
const MANIFESTS = ['patch.json','patch-1.11.json','patch-1.12.json','octave-1.12.json','patch-1.2.json','octave-1.2.json']
  .map(p => 'https://jcuhnoio.github.io/re-nanocore/' + p);
let busy = false;

const log = t => { $('log').textContent += t + '\n'; };
const status = (t, d = '') => { $('status').textContent = t; $('detail').textContent = d; };
const ok = () => !!navigator.usb && !!navigator.requestMIDIAccess && isSecureContext;

function controls() {
  const ready = !busy && ok() && $('audio').checked;
  $('start').disabled = !ready; $('connect').disabled = !(!busy && ok());
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
  const outs = [...midi.outputs.values()];
  log('MIDI-uitgangen: ' + (outs.length ? outs.map(p => '"' + p.name + '"').join(', ') : 'geen'));
  let ports = outs.filter(p => /nanocore|livtra/i.test(p.name));
  if (ports.length === 0 && outs.length === 1) ports = outs;          // enige poort: die gebruiken
  if (ports.length > 1) throw Error('Meer dan één mogelijke NanoCore-poort gevonden; sluit maar één pedal aan.');
  if (ports.length === 1) {   // stock-updater openen (Lupgrader); dit wist niets
    await ports[0].open(); ports[0].send([0xf0, 0x7d, 0x44, 0x46, 0x55, 0x01, 0xf7]);
    log('Update-commando verstuurd naar "' + ports[0].name + '".');
    await new Promise(r => setTimeout(r, 1500)); try { await ports[0].close(); } catch {}
    status('Kies de bootloader', 'Klik "Verbind bootloader" en kies Livtra Lupgrader. Het scherm van de pedal kan nu zwart of stil zijn: dat is normaal.');
  } else {
    status('Geen NanoCore-MIDI gevonden', 'Staat de pedal al in updatemodus (bijv. na een eerdere poging)? Klik dan "Verbind bootloader". Anders: USB eruit, pedal aan, ToneCommand dicht, opnieuw Start.');
  }
}

async function read(device) {
  let t;
  try { t = await WebUSBTransport.open(device); }
  catch (e) {
    if (/access denied|security/i.test(e.message)) throw Error('Toegang geweigerd tot de bootloader. Chromebook: zet Lupgrader/NanoCore uit bij Instellingen → Ontwikkelaars → Linux → USB-apparaten beheren, sluit andere NanoCore-tabbladen, en probeer opnieuw "Verbind bootloader". (' + e.message + ')');
    throw e;
  }
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
$('connect').onclick = () => run(async () => {
  let dev;
  try { dev = await navigator.usb.requestDevice({filters: [USB_FILTER]}); }
  catch { throw Error('Geen Livtra Lupgrader gekozen of gevonden. Staat er niets in het lijstje, dan staat de pedal niet in updatemodus: klik eerst Start.'); }
  log('Bootloader gekozen: ' + (dev.productName || 'Lupgrader') + ' (33c3:6678).');
  return read(dev);
});
if (!ok()) status('Niet ondersteund', 'Gebruik desktop Chrome of Edge (WebUSB + Web MIDI).');
controls();
window.__backupReady = true;
