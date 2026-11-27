// nanohardcore-installer voor NanoCore-firmware 1.2. Opzet en veiligheidsstappen volgen
// re-nanocore web/app.js (MIT, (c) 2026 jcuhnoio and contributors):
// lezen -> exacte stock 1.2 controleren -> backup bewaren -> alleen gewijzigde sectoren
// schrijven -> alles teruglezen en vergelijken -> pas dan herstarten.
import {WebUSBTransport, RawOS, USB_FILTER} from './dfu.js';
import {validateImage, install, prepareRestore, sha256} from './firmware.js';
import {get, put, recoveryTarget} from './storage.js';

const $ = id => document.getElementById(id);
let mode = 'install', busy = false, critical = false, imported = null, backupURL;
const manifestPromise = fetch('./hardcore-1.2.json').then(r => { if (!r.ok) throw Error('Kon de patch niet laden; ververs de pagina.'); return r.json(); });
manifestPromise.catch(() => {});
const supported = !!navigator.usb && !!navigator.requestMIDIAccess && isSecureContext;

const log = t => { $('log').textContent += t + '\n'; };
const status = (t, d = '') => { $('status').textContent = t; $('detail').textContent = d; };
function controls() {
  const ok = !busy && supported && $('audio').checked;
  for (const id of ['install', 'restore', 'recovery', 'connect']) $(id).disabled = !ok;
  $('importBackup').disabled = busy;
}
function progress({phase, completed, total}) {
  $('progress').value = total ? completed / total * 100 : 0;
  status({read: 'Firmware lezen…', preflight: 'Pedal nogmaals controleren…', write: 'Installeren: USB aangesloten laten!',
          verify: 'Elke byte controleren…', complete: 'Geverifieerd. De pedal herstart.'}[phase] || phase,
         total ? `${Math.round(completed / 1024)} / ${Math.round(total / 1024)} KB` : '');
}
function linkBackup(bytes) {
  if (backupURL) URL.revokeObjectURL(backupURL);
  backupURL = URL.createObjectURL(new Blob([bytes], {type: 'application/octet-stream'}));
  const a = $('backupLink'); a.href = backupURL; a.download = 'nanocore-1.2-stock-backup.bin'; a.hidden = false;
}
async function refresh() {
  const b = await get('original'); if (b) linkBackup(b.bytes);
  $('recovery').hidden = !(await get('pending'))?.active;
}
async function run(fn) {
  if (busy) return; busy = true; controls(); $('error').hidden = true;
  try { await fn(); }
  catch (e) {
    $('error').textContent = e.message; $('error').hidden = false;
    status('Gestopt', critical ? 'Een upload is onderbroken. Laat de pedal in updatemodus en gebruik "Herstel onderbroken upload".'
                              : 'Er is niets geschreven. USB eruit en pedal opnieuw aanzetten; hij start normaal.');
  } finally { critical = false; busy = false; controls(); await refresh().catch(() => {}); }
}

async function begin(next) {
  if (!supported) throw Error('Gebruik desktop Chrome of Edge (ook op een Chromebook).');
  if (!$('audio').checked) throw Error('Haal eerst koptelefoon en audiokabels eruit en sluit ToneCommand.');
  mode = next;
  if (mode === 'restore' && !imported && !(await get('original'))) throw Error('Kies eerst je backupbestand (nanocore-1.2-stock-backup.bin).');
  const boot = (await navigator.usb.getDevices()).filter(d => d.vendorId === USB_FILTER.vendorId && d.productId === USB_FILTER.productId);
  if (boot.length === 1) return upload(boot[0]);
  status('Verbinden met NanoCore', 'Sta MIDI-toegang toe, inclusief system-exclusive.');
  const midi = await navigator.requestMIDIAccess({sysex: true});
  const outs = [...midi.outputs.values()];
  log('MIDI-uitgangen: ' + (outs.map(p => '"' + p.name + '"').join(', ') || 'geen'));
  let ports = outs.filter(p => /nanocore|livtra/i.test(p.name));
  if (ports.length > 1) throw Error('Sluit maar één NanoCore aan.');
  if (ports.length === 1) {
    await ports[0].open(); ports[0].send([0xf0, 0x7d, 0x44, 0x46, 0x55, 0x01, 0xf7]);
    log('Update-commando verstuurd naar "' + ports[0].name + '".');
    await new Promise(r => setTimeout(r, 1500)); try { await ports[0].close(); } catch {}
  }
  status('Kies de bootloader', 'Klik "Verbind bootloader" en kies Lupgrader. Chromebook: bij "verbinden met Linux?" op het kruisje klikken.');
}

async function upload(device) {
  const m = await manifestPromise;
  let t;
  try { t = await WebUSBTransport.open(device); }
  catch (e) {
    if (/access denied|security/i.test(e.message)) throw Error('Toegang geweigerd. Chromebook: herstart, zet Lupgrader uit bij Instellingen → Ontwikkelaars → Linux → USB-apparaten beheren, en kies bij de melding niet voor Linux. (' + e.message + ')');
    throw e;
  }
  log('Bootloader verbonden.');
  try {
    const dfu = new RawOS(t, {onProgress: progress});
    const current = await dfu.readPartition();
    let target;
    if (mode === 'recover') {
      const pending = await get('pending');
      if (!pending?.active) throw Error('Geen onderbroken upload bewaard in deze browser.');
      target = recoveryTarget(current, pending);
    } else {
      const state = await validateImage(current, m);          // gooit bij alles behalve exact stock 1.2 / HARDCORE
      log('Gevonden: ' + (state.kind === 'stock' ? 'stock 1.2' : 'nanohardcore op 1.2'));
      if (mode === 'install') {
        if (state.kind === 'installed') { status('Al geïnstalleerd', 'HARDCORE staat al op deze pedal. Er is niets geschreven.'); return; }
        status('Backup bewaren…');
        const backup = {bytes: current, sha256: await sha256(current), version: '1.2'};
        await put('original', backup); linkBackup(current);
        target = await install(current, m);
      } else {
        const backup = imported || (await get('original'))?.bytes;
        if (!backup) throw Error('Geen backup gevonden. Kies je backupbestand.');
        if (state.kind === 'stock') { status('Al origineel', 'Deze pedal draait de originele 1.2. Er is niets geschreven.'); return; }
        target = await prepareRestore(current, backup, m);
      }
    }
    const pending = {active: true, current, target, mode, created: new Date().toISOString()};
    if (mode !== 'recover') await put('pending', pending);
    critical = true;
    const receipt = await dfu.program({expectedCurrent: current, target,
      backupSaved: async bytes => { if (mode !== 'recover') await put('pending', {...pending, current: bytes}); return true; },
      onVerified: async (_b, rec) => { await put('receipt', rec); }});
    await put('pending', {...pending, active: false, receipt});
    log(JSON.stringify(receipt));
    status(mode === 'install' ? 'HARDCORE is geïnstalleerd.' : mode === 'restore' ? 'Originele firmware teruggezet.' : 'Vorige firmware hersteld.',
      mode === 'install' ? 'Wacht tot de pedal opnieuw is opgestart. Kies HARDCORE in FX2 (de plek van Octaver/PITCH), zet de andere effecten uit, sluit audio aan op laag volume. De eerste 3 s na het aanzetten is het effect droog.'
                         : 'Elke byte is gecontroleerd. Wacht tot de pedal opnieuw is opgestart.');
    $('audio').checked = false;
  } finally { await t.close(); }
}

$('audio').onchange = controls;
$('install').onclick = () => run(() => begin('install'));
$('restore').onclick = () => run(() => begin('restore'));
$('recovery').onclick = () => run(() => begin('recover'));
$('connect').onclick = () => run(async () => {
  let dev;
  try { dev = await navigator.usb.requestDevice({filters: [USB_FILTER]}); }
  catch { throw Error('Geen Lupgrader gekozen. Staat het lijstje leeg, klik dan eerst opnieuw op de knop van wat je wilde doen.'); }
  return upload(dev);
});
$('importBackup').onclick = () => $('backupFile').click();
$('backupFile').onchange = () => run(async () => {
  const f = $('backupFile').files[0]; if (!f) return;
  const bytes = new Uint8Array(await f.arrayBuffer());
  if ((await validateImage(bytes, await manifestPromise)).kind !== 'stock') throw Error('Dit is geen originele 1.2-backup.');
  imported = bytes; status('Backupbestand klaar', 'Klik "Origineel terugzetten".');
});
if (!supported) status('Niet ondersteund', 'Gebruik desktop Chrome of Edge (WebUSB + Web MIDI).');
controls(); refresh().catch(e => { $('error').hidden = false; $('error').textContent = 'Browseropslag niet beschikbaar: ' + e.message; });
window.__installReady = true;
