# nanohardcore — 909 → Sherman → Mackie op de Livtra NanoCore

**Demo's en uitleg:** https://plieuw.github.io/nanohardcore/

Werk je hier (als mens of agent) aan mee: lees eerst [`AGENTS.md`](AGENTS.md).

```
make render   # demo's renderen naar out/
make test     # regressiepoort: faalt als een gemeten eigenschap uit deze README wegdrijft (ook in CI)
make rv       # RV32IMAFDC-build + instructietelling in Unicorn (clang, ld.lld, pip install unicorn pyelftools)
```

Een eigen effectketen voor de Livtra NanoCore (RV32IMAFDC, float32, 44,1 kHz):

```
ingang (trigger-puls of drumgeluid) ─► transient-detector ─► TR-909 bassdrum
                                                              │ (+ optioneel dry "thru")
                                                              ▼
                                   Sherman-stijl dual switched-capacitor filter
                                                              ▼
                                   Mackie 1202-VLZ PRO kanaal → main out (gain-distortion)
```

## Wat is waarop gebaseerd

| Blok | Bron | Niveau |
|---|---|---|
| 909 BD | TR-909 Service Notes, voicing board, BASS DRUM + metingen van echte units | Componentniveau: C9/R21/D7-pitchenvelope, TUNE-pot VR2, IC12-CV-som (R26/R27/R28), driehoek-VCO IC13a/b (C14, R59, Schmitt R49/R50), diode-shaper R52 + D10/D11 (opgelost met Shockley-vergelijking, als tabel), Q12-VCA met verzadiging, ENV1 C8/VR5, attack-tak Q8/Q9 (Sallen-Key uit R16/R7/C3/C4) + ruis R46/C13, ENV2 C12. Gekalibreerd (`CAL_*`) op gemeten units: zie hieronder |
| Sherman | LTC1060-datasheet, Filterbank-handleiding, reviews | Mechanisme: klokgestuurde sampled-data SVF met **vaste** coëfficiënt (f0 = fclk/50), ingang gesampled op de klokflanken (zonder anti-alias), sample-and-hold-uitgang, clock-feedthrough, HARMONICS als deler op één snelle masterklok, niet-50%-duty bij oneven delers |
| Mackie | 1202-VLZ PRO main board 990-217-00 rev B, sheet 1 | Alles uit het schema, 2× oversampled: preamp (instrumentatieversterker Q5/Q1 + U7A/U7B, R641/R642 5k11, U11A ×0,511) met afzonderlijke railclipping per poot; **LO CUT (U13A) en EQ (U27B) als echte circuitsimulatie** (nodale analyse, trapezium, exacte toestandsruimte over de condensatoren); GAIN-trap U40A met R565/R605/R570/R555; clippunten op U27B, U40A en de main mix |

Kalibratieconstanten die niet uit een tekening volgen, zijn in de code gemarkeerd met `CAL`.

### 909: gekalibreerd op echte units
De componenten uit het schema voorspellen een rustpitch van ~54–56 Hz. Echte units zakken lager (47 Hz op het gehoor, 49–53 Hz gemeten). Het model is gefit op:
- rust ~47 Hz, met een staart die nog naar ~46 Hz zakt;
- ~230 Hz op 5 ms en ~160 Hz op 10 ms (TUNE midden);
- TUNE hoog: ~166 Hz op 20 ms en ~111 Hz op 40 ms;
- een body die 50–80 ms op vol niveau blijft staan (verzadiging in Q12/IC11a).

Resultaat (`test/fit909.c`): 231 Hz @5 ms · 163 Hz @10 ms · 47 Hz @80 ms. TUNE max: 184 Hz @20 ms · 115 Hz @40 ms. Hold: 43 ms (decay 0,5) tot 84 ms (decay 0,7).
De Mackie LOW CUT staat nu standaard **uit**: met 18 dB/oct op 75 Hz sneed hij de grondtoon weg.

### Sherman: waar de "harmonics" vandaan komen
- Filter 1 klokt op master/2 en filter 2 op master/D2, met D2 ∈ {2,3,4,6,8,10,12,14,16,18,32}. Dat geeft de ratio's 1, 1.5, 2 … 16 uit de handleiding.
- Beide klokken komen uit dezelfde mastertelling (integer + fractie), dus samenvallende flanken vallen bitexact samen. Filter 2 neemt het sample vóór filter 1 zijn uitgang bijwerkt.
- De klokfluit wordt alleen hoorbaar als de klok zelf in het audiogebied zakt (vol onder 8 kHz klok, weg boven 20 kHz). Bij 50:1 is dat cutoff < ~400 Hz: het onderste stuk van de lowpass. In HARMONICS-modus bepaalt de snelle klok van filter 1 dat, omdat de gedeelde klok van filter 2 uit dezelfde master komt.
- **INPUT (gain/overdrive):** geen openbaar schema, dus de structuur volgt het gedocumenteerde gedrag:
  1. gain −20…+40 dB (unity bij CC 24 ≈ 42);
  2. HI-schakelaar cut/flat/boost (CC 120);
  3. asymmetrische triode-achtige trap: schoon bij laag niveau, de asymmetrie groeit met de drive, en bij hard aansturen verschuift de bias (gridstroom);
  4. LIMIT (CC 121, −6 dB naar de filters);
  5. de LTC1060-integratoren op ±5 V, die bij veel input de resonantie wegduwen.

  | INPUT | THD (200 Hz, 1 V) | resonantiepiek (reso 0,9) |
  |---|---|---|
  | −20 dB | 0,0% | +46 dB |
  | −8 dB | 0,7% | +40 dB |
  | −2 dB | 2,5% | — |
  | +4 dB | 8% | +30 dB |
  | +10 dB | 26% | — |
  | +16 dB | 37% | +15 dB |
  | +28 dB | 44% | +1 dB |

  Met LIMIT aan blijft er bij volle drive nog +14 dB resonantie over.

  **OVER-zone (bovenste 40% van INPUT):** een tweede, harde railclip achter de buistrap (tot +26 dB extra, rail 4 V, dus de LTC1060 clipt mee). De bias-verschuiving is begrensd, zodat hij het signaal niet dichtknijpt.

  **Laatste stuk (80–100%): rectify.** Een gelijkrichter vóór de drivetrappen gaat traploos over van normaal, via halfgolf (90%), naar dubbelzijdig (100%). De DC verdwijnt via een koppelcondensator (8 Hz). Gemeten met een sinus van 200 Hz: bij 90% grondtoon +18 dB en octaaf +8 dB; bij 100% grondtoon −84 dB en octaaf +18 dB. Het octaaf gaat daarna door de buis- en de harde trap.

  De niveaus zijn `CAL` (VK_IN, OD_OUT, POS_SAT/NEG_SAT, BIAS_K/BIAS_MAX, HI_F, OVER_*, RECT_K0).
- **Filtertype sweepen (L-B-H, CC 27 / 31):** traploos LP → BP → HP, zoals de pot op de unit de drie uitgangen mengt. Correctie (CC 28 / 85): naar links −B (BP eruit), naar rechts −B+LH (notch). Alle knoppen (L-B-H, correctie, cutoff, reso) glijden per sample met 12 ms, dus MIDI-stappen en blok-updates geven geen zipper. Gemeten (fc 1 kHz, reso 0,5, relatief t.o.v. LP-doorlaat):

  | stand | 100 Hz | 300 Hz | 1 kHz | 3 kHz | 8 kHz |
  |---|---|---|---|---|---|
  | LP | 0 | +0,8 | +16 | −16 | −34 |
  | LP/BP | −6 | −5 | +13 | −13 | −23 |
  | BP | −20 | −10 | +16 | −7 | −16 |
  | BP/HP | −26 | −15 | +13 | −2 | −4 |
  | HP | −40 | −20 | +16 | +3 | +1 |
  | notch (BP + correctie rechts) | 0 | 0 | −81 | +2 | +1 |

  De resonantiepiek blijft overal staan. In de tussenstanden is hij 3 dB lager, omdat LP en BP 90° uit fase zijn: hetzelfde als bij een mengpot.
- **Resonantie:** de RESO-pot stelt 1/Q logaritmisch in (bij de LTC1060 is Q een weerstandsverhouding), dus een beetje reso doet al veel: reso 0,2 → Q 2, 0,4 → Q 4, 0,6 → Q 7, 0,8 → Q 12, max → Q 20 (filter 1) of Q 33 (filter 2, die heeft meer resonantie). De knop alleen haalt nooit nul demping. Zelfoscillatie ontstaat pas door de Q-verhoging van de SC-integratoren bij hoge klokfrequenties (eindige op-amp-settling), dus alleen in het midden/hoog. Gemeten (oscilleert hij 1,5 s na een klik nog?):

  | reso | ≤ 1,5 kHz | 3 kHz | 5 kHz | 8 kHz |
  |---|---|---|---|---|
  | 0,6 | nee | nee | filter 2 | beide |
  | 0,8 | nee | filter 2 | beide | beide |
  | 1,0 | nee | beide | beide | beide |

  De amplitude van de oscillatie wordt begrensd door de ±5 V-integratoren ("tinny"), en veel input duwt hem weg. De constanten zijn `CAL` (Q1/Q2_MAX_DAMP, QE_FCLK, QE_AMT). Bij 100:1 ligt de klok 2× hoger, dus begint de oscillatie een octaaf lager.
- **ADSR (envelope):** één condensator die via A-, D- en R-weerstanden laadt en ontlaadt.
  - **Attack:** laadt naar een doel van 1,5 en de comparator stopt hem op 1,0. Daardoor de holle analoge curve.
  - **Gate:** komt van de input zelf, zoals de audio-trigger van de Sherman. Elke gedetecteerde hit zet de gate aan; die blijft hoog zolang de input boven het triggerniveau zit (minimaal CC 122, standaard 20 ms). Sustain blijft dus staan zolang er signaal is. MIDI-noten werken als gate (note on/off).
  - **Hertriggeren:** een trigger tijdens de attack verandert niets. Een lange attack slaat dus vanzelf elke 2e/3e trigger over (handleiding les 6; getest: 16 triggers op 125 ms met attack 200 ms → 8 envelopes).
  - **Tijden:** 2 ms tot 30 s (handleiding fig. 22). CC 5 maakt hem tot 4× sneller, zoals CC 5 op de unit.
  - **Diepte per filter, bipolair (CC 88 / CC 89):** CC 64 = exact 0, ±7 octaaf aan de uiteinden. Kwadratische curve, dus fijne controle rond nul.
  - **Sync-modus:** in HARMONICS volgt filter 2 filter 1 en heeft CC 89 geen effect, net als op de hardware. In free-modus heeft elk filter zijn eigen diepte.
- De overdrive wordt op de klokmomenten berekend. Daardoor vouwen boventonen boven 22 kHz via de SC-klok terug, net als in de hardware. Gemeten: zonder deze stap week het spectrum onder 30 Hz tot 25 dB af van de 8×/16×-referentie.

## Bediening (MIDI CC, kanaal maakt niet uit)

CC 120–127 zijn in de MIDI-standaard kanaalmodusberichten (All Sound Off, Reset All Controllers, All Notes Off …) en worden genegeerd. Bipolaire knoppen: 64 = exact midden/nul.

**TR-909 bassdrum**

| CC | functie | bereik |
|---|---|---|
| 20 | TUNE (lengte van de pitch-sweep) | VR2 100k(A) |
| 21 | DECAY | VR5 1M(A), tot ~0,9 s |
| 22 | ATTACK (klik + ruis) | |
| 23 | LEVEL | VR4 |
| 14 | ACCENT (niveau) | schaalt de accentspanning |
| 15 | accentbron | 0–63 = sterkte van de hit / velocity, 64–127 = altijd vol |
| 16 | pitch-trim (mod) | ±12 halve tonen, 64 = origineel |
| 17 | sweep-diepte (mod) | 0…2×, 64 = origineel |
| note-on | trigger + gate | velocity = accent |

**Sherman**

| CC | functie |
|---|---|
| 24 | INPUT (−20…+40 dB; 60–100% OVER-trap, 80–100% rectify) |
| 80 / 81 | HI cut/flat/boost · LIMIT |
| 25 / 29 | FREQ 1 / FREQ 2 |
| 26 / 30 | RESO 1 / RESO 2 |
| 27 / 31 | L-B-H 1 / 2 (LP → BP → HP, traploos) |
| 28 / 85 | CORRECTIE 1 / 2 (−B … −B+LH) |
| 86 | HARMONICS (0 = free, 1…11 = 1, 1.5, 2, 3, 4, 5, 6, 7, 8, 9, 16) |
| 87 | PAR ↔ SER |
| 88 / 89 | ADSR → filter 1 / 2 (bipolair) |
| 102–105 | ADSR A / D / S / R |
| 5 | ADSR-snelheid (127 = normaal, 0 = 4× sneller) |
| 82 | minimale gate |
| 90 | clock bleed |
| 106 | klokratio 50:1 / 100:1 |

**Mackie 1202-VLZ PRO channelstrip**

| CC | functie |
|---|---|
| 107 | TRIM (−15…+45 dB) |
| 108 | LOW CUT 75 Hz (≥ 64 = aan) |
| 109 / 110 / 111 | EQ LOW / MID / HI (±15 / ±13 / ±15 dB, 64 = vlak) |
| 112 | GAIN (unity bij ~96, max +10 dB) |
| 18 | PAN (stereo-uitgang, constant power, midden = unity per kant) |
| 19 | MUTE (≥ 64, met een ramp van 5 ms) |
| 113 | MAIN MIX (unity bij ~96) |

**Keten**

| CC | functie |
|---|---|
| 114 | triggergevoeligheid (−20…−60 dBFS) |
| 115 | hold-off (15…265 ms) |
| 116 | thru (dry input in de Sherman) |
| 117 | output |
| 118 | Sherman → Mackie lijnniveau (64 = −10 dBV) |
| 119 | auto level (≥ 64 aan) |

Getest (`test/ccmap.c`): elk van de 46 CC's verandert de output aantoonbaar; CC 120–127 doen niets.


**Gain staging Mackie:** de Sherman levert nominaal lijnniveau aan de Mackie (CC 118). TRIM staat standaard op 0 dB (CC 107 = 32), dus schoon. De eerste stap die clipt is U40A: de GAIN-versterker draait ~11,5 dB heet en de mixbus haalt dat later terug. Die clipt bij ~+14 dB TRIM (CC 107 ≈ 61). Auto level (CC 119) trekt het uitgangsniveau omlaag met de TRIM mee tot dat punt, zoals je met MAIN op een echte tafel zou doen. Daarboven voegt TRIM alleen overdrive toe, geen volume. Gemeten crestfactor per kick: 8,1 dB schoon → 7,0 (+14) → 4,4 (+19) → 2,8 (+24) → ~2 dB vanaf +28 dB.

**Trigger:** de onset-detector vergelijkt het vermogen over 1 ms met dat over 30 ms (factor 3). De piek in de eerste 1,5 ms bepaalt de 909-ACCENT. Latentie: ~1,5–3 ms. Bij een basdrum die over een nagalmende tom heen valt is dat ~7 ms. Een schone trigger-puls is het strakst.

## Verificatie
- 909: de rustpitch volgt uit de componenten alleen en komt uit op **~56 Hz**. De sweep start bij ~215 Hz (accent max), het TUNE-bereik is τ 7–29 ms en DECAY loopt tot ~0,9 s (tot −40 dB).
- Trigger: 24/24 pulsen gedetecteerd. Op de drumloop vuurden alle kicks, snares en toms, plus een deel van de ghost-hats (hangt af van de gevoeligheid).
- Sherman-modelaliasing (44,1 kHz tegen een 8× oversampled referentie op identieke ingang, met 16× als controle): alles wat alleen in het model zit ligt **≥ 41 dB** onder het maximum (de rest boven 15 kHz bij maximale bleed). Zonder bleed is dat ≥ 52 dB.
- Mackie-EQ: de simulatie in C komt op ±0,4 dB tot 12 kHz overeen met een onafhankelijke frequentiedomein-analyse van hetzelfde netwerk (`test/mna.py`). Die analyse geeft: vlak ≈ 0 dB, HI ±15 dB vanaf 8 kHz (+18 dB @ 12 kHz), MID ±13 dB @ 2,5 kHz, LO ±14 dB @ 80 Hz, LO CUT −4 dB @ 80 Hz, −16,5 dB @ 50 Hz. Totale Mackie-respons vlak: ±0,7 dB tot 18 kHz.
- RISC-V (clang, rv32imafdc/ilp32d, -O2, geëmuleerd met Unicorn): de uitvoer komt overeen met de host-build.

| | instructies/sample | MIPS @ 44,1 kHz |
|---|---|---|
| standaard | ~3 120 | ~138 |
| sync 1.5, lage klokken + bleed | ~2 570 | ~113 |
| worst case (beide cutoffs maximaal) | ~6 600 | ~291 |

De Mackie op 2× kost nu ~1 550 instructies/sample (EQ-circuit ~710, clippers ~510, halfband ~330). Code: 21 KB, state: 5,6 KB.

## Op de pedal: firmware 1.2 (experimenteel)

**Installeren:** https://plieuw.github.io/nanohardcore/install/ (desktop Chrome/Edge, ook op een Chromebook). Eerst een backup maken kan via https://plieuw.github.io/nanohardcore/backup/ (alleen lezen).

De keten zit als **HARDCORE** in het FX2/PITCH-slot (de plek van de Octaver). De rest van de firmware blijft origineel: BLE-MIDI, USB-MIDI, presets en de updater.

**Hoe:** de route van [re-nanocore](https://github.com/jcuhnoio/re-nanocore) (MIT) voor hun op hardware geteste 1.2-Octaver.
- Er komt een reset-shim bij die de payload achter de native BSS kopieert.
- Vier haken in het PITCH-slot: dispatch, init, reset en shift.
- De ring wordt vergroot van 8 naar 64 KB.
- Zeven UI-haken voor de naam, de 11 knoppen, de waardeweergave en de defaults.
- De heap verschuift en de FIT-hashes worden bijgewerkt.

Onze native wijzigingen zijn een **strikte deelverzameling** van die van de geteste Octaver-image: alles wat wij veranderen, veranderen zij ook. Wij laten UniVibe en de diagnosehaak weg. De stock-hash van de gebackupte pedal is gelijk aan die van hun geteste 1.2.

| controle | resultaat |
|---|---|
| payload | 28 KB code; 3,8 KB state in de 64 KB-ring |
| reset-shim in Unicorn (re-nanocore `check_octave_120_boot`) | payload exact gekopieerd, native entry bereikt, registers behouden, BSS-clear raakt payload niet |
| functionele emulatie van de gepatchte image (`firmware/nanocore-1.2/emulate.py`) | init 85k instructies (~0,24 ms); 3 s exact droog, daarna verwerkt; bewaking latcht bypass en toont CPU LIMIT |
| CPU (RV32, 360 instr/µs zoals gemeten op de pedal) | standaardknoppen gem. 36%, zwaarste blok 43%; zwaarste knopstand 52,5% (grens 65%) |
| browserinstaller tegen de backup + nagebootste Lupgrader | 8 sectoren geschreven, exact gelijk aan de build; terugzetten geeft exact stock; 1.10 en een beschadigde image geweigerd |

**Pedalknoppen:** SENS · TUNE · DECAY · DRIVE · FREQ · RESO · L-B-H · ENV>F (midden = 0) · HARM (FREE/x2/x4/x16) · INPUT (909 / 909+DRY / AUDIO / AUD+909) · LEVEL. Het Sherman-filterplafond op de pedal is 8 kHz (`CHAIN_DEV_FMAX`), zodat de zwaarste stand binnen het budget blijft.

**Bouwen** (lokaal; de backup komt niet in de repo):
`python3 firmware/nanocore-1.2/build.py --backup nanocore-1.2-stock-backup.bin --re-nanocore PAD --manifest docs/install/hardcore-1.2.json`
en daarna `python3 firmware/nanocore-1.2/emulate.py PAD`.

Het manifest bevat onze code plus 78 bytes aan originele instructies op de haakplekken (als controle) en bijgewerkte hashes; geen vendorimage.

**Eerste hardwaretest**
1. Bewaar `nanocore-1.2-stock-backup.bin`.
2. Installeer, zonder audio aangesloten.
3. Controleer na de herstart dat de pedal normaal opstart en dat USB- en BLE-MIDI werken.
4. Maak een preset met alleen HARDCORE aan, op laag volume. Na 3 s droog komt de keten in.
5. Probeer de knoppen en noteer het als er CPU LIMIT verschijnt.
6. Test "Origineel terugzetten".

**Restrisico:** start een image niet op, dan is de enige bekende herstelroute de updater. Die opent via de draaiende app; of de bootloader zelf DFU aanbiedt, is niet bekend. Dat risico wordt kleiner doordat het opstartpad gelijk is aan het geteste pakket en in emulatie is gecontroleerd.

## Integratie op de NanoCore: wat nog moet
- Het bewezen patch-slot (Velvet Vibrato) is 1 392 bytes. Deze keten is ~16 KB code, dus die past daar niet in. Nodig is een grotere code-plek: het FIT-segment uitbreiden (de OS-partitie heeft ~124 KB rekenkundige ruimte, maar die is nog niet als uitvoerbaar gemapt bevestigd) of een gedoneerde effectroutine.
- De 5,6 KB state moet uit een eigen allocatie komen, met init/reset-paden.
- De MIDI-CC's moeten gekoppeld worden aan de NanoCore-parameterbank (12 float-slots per node), of via de SysEx-route van de editor.
- Het CPU-budget op de echte hardware is onbekend (de kloksnelheid is niet vastgesteld). Als het krap wordt, verlaagt `fmax` in `sherman.c` het worst case (minder klokflanken per sample).

## Bestanden
- `dsp/` — freestanding C99 (geen libm, geen malloc): `bd909.c`, `sherman.c`, `mackie.c`, `mkcirc.c` (circuitsimulator), `chain.c`, `mathx.h`
- `host/render.c` — offline renders · `test/alias.c` + `alias_an.py` — aliasingtest · `test/mna.py`, `eqresp.c`, `flat.c` — Mackie-verificatie · `test/fit909.c` — 909-kalibratie · `rv/` — RISC-V build + emulator/profiler
- `docs/` — GitHub Pages-site met de demo's (mp3) en spectrogrammen; `out/` wordt door `make render` gevuld

## Licentie en bronnen
GPL-3.0. Niet gelieerd aan Livtra, Roland, Sherman of Mackie. Gebruikte documentatie: TR-909 Service Notes (1984), Mackie 1202-VLZ PRO-schema (990-217-00 rev B), Sherman Filterbank-handleiding, LTC1060-datasheet, en [re-nanocore](https://github.com/jcuhnoio/re-nanocore) voor de NanoCore-ISA en -ABI. Die documenten staan om auteursrechtelijke redenen niet in deze repo.
