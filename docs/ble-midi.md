# BLE- en USB-MIDI in nanohardcore (NanoCore-firmware 1.2)

Naslagdocument voor mensen en AI-agents die aan de MIDI-koppeling werken. Het beschrijft
de omgeving zoals die nu is, hoe hij gebouwd en getest wordt, en de grenzen ervan.

**Status:** werkt in emulatie tegen een echte stock-1.2-backup, niet op hardware getest
(2026-11-27). Lees eerst `AGENTS.md` (werkwijze, regels, wat niet in de repo mag).

---

## 1. Context in één alinea

nanohardcore draait als effect **HARDCORE** in het FX2/PITCH-slot (subtype 8) van de
originele Livtra NanoCore-firmware **1.2**. De rest blijft originele firmware: BLE-MIDI,
USB-MIDI, presets, scherm en de updater. Er is geen broncode van de originele firmware.
Alles hieronder komt uit statische analyse van de machinecode (RV32IMAFDC met T-Head-
uitbreidingen) en uit Unicorn-emulatie. De DSP-keten (909 → Sherman → Mackie) staat in
`dsp/`, de koppeling met de firmware in `firmware/nanocore-1.2/`.

## 2. Hoe MIDI in de originele firmware loopt

```
BLE-MIDI ─┐                          (BLE-timestamps al verwijderd vóór deze laag)
USB-MIDI ─┴─> byte-parser 0x4002de1c(poort, buf, len)
                │   running status, 1/2 databytes, SysEx apart verzameld
                └─> berichtfunctie 0x4002db26(poort, status, d1, d2, heeft_d2)   <── HAAK
                       ├─ 0x80/0x90 noten, 0xB0 CC (incl. NRPN 0x63/0x62/0x06), 0xC0 PC,
                       │  0xF2/0xF3, kanaalfilter via 0x40033672
                       └─ native CC-mapping naar de eigen parameters van de pedal
```

- **Byte-parser `0x4002de1c`** heeft drie aanroepers: `0x4002e0da` (doorgeefpoort),
  `0x4002e2e6` (poort 1) en `0x4002e326` (poort 0, leest max 0x80 bytes via `0x4002ff4c`).
  Welke poort BLE is en welke USB, is **niet vastgesteld**. Dat hoeft ook niet: beide
  komen langs de haak.
- **Berichtfunctie `0x4002db26`.** De ABI uit de disassembly:
  `a0` = poort (1 of anders), `a1` = statusbyte, `a2` = data1, `a3` = data2,
  `a4` = 1 als er een tweede databyte is. CC's worden native alleen verwerkt als `a4 != 0`.
- **De eerste instructie** op `0x4002db26` is `jal t0, 0x4000c3a4` (bytes `ef e2 fd 87`).
  Dat is de gedeelde register-save-helper van de 1.2-compiler (`__riscv_save_*`-stijl).
  Hervatten moet dus met `t0 = 0x4002db2a` en een sprong naar `0x4000c3a4`.
- **SysEx (`0xF0 … 0xF7`)** gaat **niet** door `0x4002db26`. De parser verzamelt SysEx
  apart (rond `0x4002df84`). Daardoor blijft het updater-commando `F0 7D 44 46 55 01 F7`
  buiten onze haak.

## 3. Wat nanohardcore toevoegt

| onderdeel | bestand | functie |
|---|---|---|
| haak | `firmware/nanocore-1.2/build.py` (`MIDI_HOOK`) | vervangt de 4 bytes op `0x4002db26` door `j nc_midi_gate` (JAL met rd = x0, dus `ra` blijft die van de aanroeper). Weigert te bouwen als de originele bytes niet `efe2fd87` zijn. |
| poort | `firmware/nanocore-1.2/midi_gate.S` | bewaart `ra`, `a0`–`a4` op de stack (32 bytes, 16-uitgelijnd), roept `nc_midi_in` aan, herstelt alles. Bij 1: `ret` (overgenomen). Bij 0: `li t0,0x4002db2a; li t1,0x4000c3a4; jr t1`, dus exact de verplaatste proloog naspelen. |
| beslissing + wachtrij | `firmware/nanocore-1.2/adapter.c` (`nc_midi_in`, `midi_drain`) | neemt **alleen** `0x8n`, `0x9n` en `0xBn` over, **en alleen** als HARDCORE geïnitialiseerd is (`g_state`, magic `HCR1`) en het FX2-slot subtype 8 heeft (byte `0x400b0ec1`). Zet het bericht in een ringbuffer van 64 (één schrijver: MIDI-taak). |
| toepassen | `nc_octave_process` → `midi_drain` → `chain_midi` | de audiotaak (één lezer) leegt de wachtrij aan het begin van elk blok. De DSP-toestand verandert dus alleen tussen blokken. |
| DSP-kant | `dsp/chain.c` (`chain_midi`) | CC-tabel (§5), note-on = 909-trigger + gate, note-off telt gehouden noten af. |

**Bewust niet overgenomen** (gaan naar de originele code): programmawissel (`0xCn`),
aftertouch, pitch bend, systeemberichten, klok, SysEx. En **alle** berichten zolang
HARDCORE niet het actieve FX2-effect is.

**Waarom de originele firmware de overgenomen CC's niet ziet:** de CC-nummers van
nanohardcore zouden kunnen botsen met de native MIDI-mapping van de pedal, die
bijvoorbeeld effecten aan of uit zet. Zolang HARDCORE actief is, gaan CC's en noten daarom
uitsluitend naar de keten.

**Kanaal:** alle 16 kanalen worden geaccepteerd. Het native kanaalfilter wordt voor
overgenomen berichten overgeslagen.

## 4. Gelijktijdigheid en timing

- **Schrijver:** de MIDI-taak, in `nc_midi_in`. **Lezer:** de audiotaak, in `midi_drain`.
  Indices `nc_hc_midi_head` / `nc_hc_midi_tail` zijn `volatile uint32_t`, met een
  compiler-barrière tussen het schrijven van het element en het ophogen van `head`.
  Eén kern, dus geen hardware-fence nodig.
- **Wachtrij vol (64 berichten binnen één blok van 1,33 ms):** het bericht wordt geteld in
  `nc_hc_midi_drop` en weggegooid. Het gaat dan niet alsnog naar de originele code.
- **Latentie:** maximaal één audioblok (64 frames @ 48 kHz = 1,33 ms) plus de BLE-latentie.
- **CPU:** de wachtrij kost bijna niets. Een CC op de EQ (109/110/111) laat het
  Mackie-EQ-netwerk opnieuw oplossen (`mc_build`). Dat gebeurt hooguit één keer per blok,
  omdat `dirty` een vlag is. Gemeten met `make rv`, scenario 5: een EQ-CC in elk blok in de
  zwaarste pedalstand komt op max **61%** van het 48 kHz-budget. De CPU-bewaking latcht bij
  65% zacht of 67,5% hard.

## 5. CC-tabel (alle kanalen; CC 120–127 genegeerd)

| groep | CC |
|---|---|
| 909 | 20 TUNE · 21 DECAY · 22 ATTACK · 23 LEVEL · 14 ACCENT-niveau · 15 accentbron (0–63 slagsterkte, 64+ vast) · 16 toonhoogte ±12 st (64 = stock) · 17 sweepdiepte (64 = stock) |
| Sherman | 24 INPUT · 25 FREQ1 · 26 RESO1 · 27 L-B-H 1 · 28 CORR 1 · 29 FREQ2 · 30 RESO2 · 31 L-B-H 2 · 85 CORR 2 · 86 HARMONICS (0 vrij, 1–11) · 87 PAR↔SER · 88 ADSR→F1 · 89 ADSR→F2 (bipolair, 64 = exact 0) · 90 clock-bleed · 102 A · 103 D · 104 S · 105 R · 5 ADSR-snelheid · 106 klokverhouding 50/100 · 80 HI cut/flat/boost · 81 LIMIT · 82 minimale gate |
| Mackie | 107 TRIM · 108 LOW CUT · 109 LOW · 110 MID · 111 HI (64 = vlak) · 112 GAIN · 113 MAIN · 18 PAN · 19 MUTE |
| keten | 3 input (0–63 TRIGGER, 64+ AUDIO) · 114 triggergevoeligheid · 115 hold-off · 116 thru · 117 uitgangsniveau · 118 Sherman→Mackie-lijnniveau · 119 auto-level |
| noten | note-on = 909-slag + Sherman-gate (velocity = accent); note-off / velocity 0 = gate los |

**Samenspel met de 11 pedalknoppen:** een pedalknop zet alleen zijn eigen parameter, en
alleen wanneer die knop beweegt (`chain_device_set`, toegepast per gewijzigde index).
Een via MIDI gezette waarde blijft dus staan tot de bijbehorende pedalknop wordt gedraaid.
Overlap: SENS↔114, TUNE↔20, DECAY↔21, DRIVE↔24, FREQ↔25, RESO↔26, L-B-H↔27, ENV>F↔88,
HARM↔86, INPUT↔3/116, LEVEL↔117. De stand AUDIO+909 is alleen via de pedalknop INPUT
bereikbaar; CC 3 kent alleen TRIGGER en AUDIO.

**Filterplafond op de pedal:** 8 kHz (`CHAIN_DEV_FMAX`, `Chain.fmax_cap`). CC 106 houdt
zich daaraan; op de host blijft 12 kHz gelden.

## 6. Aannames die nog niet op hardware zijn bevestigd

1. **Byte `0x400b0ec1` = subtype van FX2.** Het is een runtime-subtype-array op `0x400b0ec0`
   (bytes, `lbu`-toegang), met slot 1 = FX2 volgens re-nanocore
   `docs/firmware-1.2-octave-ui.txt`. Klopt dit niet, dan neemt HARDCORE te weinig of te
   veel MIDI over. Crashen kan daardoor niet.
2. **Alle BLE-MIDI komt langs `0x4002db26`.** De twee poorten zijn gevonden; dat een ervan
   BLE is, volgt uit het ontwerp van de pedal en is niet gemeten.
3. **De MIDI-taak mag een korte C-functie aanroepen op zijn eigen stack.** `nc_midi_in`
   gebruikt geen FPU en roept niets aan; de stack groeit met 32 bytes plus een klein frame.
4. **Dit is de enige haak buiten de op hardware geteste Octaver-set.** Gaat er hier iets
   mis, dan gebeurt dat bij binnenkomende MIDI, niet bij het opstarten. Het
   updater-commando (SysEx) loopt niet door de haak.

## 7. Bouwen en testen

```sh
make test                      # DSP-regressie, incl. device-bank- en MIDI-map-checks
make rv                        # RV32-instructietelling; scenario 5 = EQ-CC elk blok
python3 firmware/nanocore-1.2/build.py --backup nanocore-1.2-stock-backup.bin \
    --re-nanocore PAD/re-nanocore --manifest docs/install/hardcore-1.2.json
python3 firmware/nanocore-1.2/emulate.py PAD/re-nanocore      # ~5 min
```

`emulate.py` controleert onder andere:
- **CC en noot naar HARDCORE:** ze komen in de wachtrij en de haak keert terug naar de
  aanroeper met `sp` intact.
- **PC, `0xF2` en een CC zonder actief HARDCORE naar de originele code:** de uitvoering
  komt bij helper `0x4000c3a4` aan met `t0 = 0x4002db2a`, ongewijzigde argumenten en
  dezelfde `sp`.
- **Wachtrij geleegd na een blok.**
- **Een MIDI-noot bij stille input geeft een 909-slag** (piek ~0,8).

De backup en de volledige images blijven lokaal (`build/fw12`, staat in `.gitignore`).
Een nieuwe uitgave moet het vorige manifest toevoegen als upgradebron:
`docs/install/hardcore-1.2-rN.json` en de lijst `OLDER` in `docs/install/app.js`.

## 8. Uitbreiden: richtlijnen

- **Meer berichttypen overnemen**, bijvoorbeeld PC voor eigen HARDCORE-presets: pas de
  filter in `nc_midi_in` aan en houd SysEx altijd buiten de haak. Neem nooit `0xF0`–`0xF7`
  over.
- **Geen werk in de MIDI-taak:** alleen in de wachtrij zetten. DSP-toestand alleen wijzigen
  in `midi_drain`, tussen blokken.
- **Nieuwe CC's:** in `chain_midi` (`dsp/chain.c`) en in `test/ccmap.c`. CC 120–127
  blijven verboden (`AGENTS.md`). Elke CC moet hoorbaar effect hebben, anders faalt
  `make test`.
- **Andere firmwareversie:** de adressen gelden alleen voor 1.2 (stock-genormaliseerde hash
  `eb3d8681…9d1b24`). `build.py` controleert de originele bytes op elke haakplek en
  weigert bij een verschil.
- **MIDI uitsturen** (bijvoorbeeld een gate-out als noot) bestaat nog niet. Daarvoor moet de
  native verzendfunctie voor BLE en USB eerst worden gevonden.

## 9. Herkomst

- Haakplekken, ABI en adressen: eigen analyse van een stock-1.2-OS-backup
  (`llvm-objdump`, cross-references) in deze sessie.
- Opstartmethode, PITCH-haken, UI-haken, ring-vergroting, packager en DFU/installer:
  [re-nanocore](https://github.com/jcuhnoio/re-nanocore) (MIT, (c) 2026 jcuhnoio and
  contributors). Hun 1.2-Octaver is op hardware getest. Onze native wijzigingen zijn een
  deelverzameling daarvan, plus deze ene MIDI-haak.
