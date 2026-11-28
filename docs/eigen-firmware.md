# Eigen firmware: "HARDCORE-editie" (fase A t/m C)

Werkdocument voor mensen en AI-agents. Doel van de eigenaar: een eigen apparaat, geen
effect in de Livtra-firmware. Van Livtra blijft alleen het onzichtbare fundament over:
bootloader, updater, RT-Thread-kernel en de hardwaredrivers. Alles erboven wordt van
nanohardcore: audio, scherm, knoppen, presets, MIDI.

Gekozen door de eigenaar (2026-11-27): **fase A–C**. Fase D (een volledig eigen OS) alleen
als vaststaat dat de bootloader zonder werkende app herstelbaar is.

## Harde regels

1. **De herstelroute blijft altijd werken.** Het updater-commando (SysEx
   `F0 7D 44 46 55 01 F7` → reboot-reden "Upgrade-Mode" → Lupgrader in de bootloader) moet
   in elke uitgave bereikbaar zijn. Wordt de native MIDI/SysEx-afhandeling ooit vervangen,
   dan moet onze code dit zelf doen.
2. **Opstartpad zo dicht mogelijk bij het geteste pakket.** Eigen code start pas na de
   native init en na een grace-periode, net als nu de audio (3 s). Voor het overnemen van
   de UI: ook eerst enkele seconden wachten.
3. **Elke haak** controleert de originele bytes in `build.py` en heeft een test in
   `emulate.py`.
4. Geen vendorbytes in de repo behalve guard-bytes in manifests (zie `AGENTS.md`).

## Bevindingen (statische analyse van een stock-1.2-OS-backup)

Alle adressen zijn segment 3 (laadadres `0x40000000`) of ITCM (`0x30040000`, segment 0).

| onderwerp | bevinding | adres |
|---|---|---|
| SDK | ArtInChip **Luban-Lite** (RT-Thread + LVGL 8). Open-source SDK: drivers en LVGL-port zijn ter vergelijking beschikbaar. | strings `bsp/artinchip/...`, `lvgl` |
| flash-indeling | `spi0.0:152k(spl),8k(env),848k(os),688k(presets),348k(ui_asset),4k(device_info)` | `0x400883c0` |
| updater | reboot-reden-strings `Upgrade-Mode` / `Command-Reboot`: de app zet de reden en herstart, daarna start de **bootloader** de Lupgrader | `0x4008cf40`, `0x4008cf50` |
| scherm | framebuffer-device `aicfb`, paneel `panel-st7789`, 320×240 | open: `0x4005cb3c`–`0x4005cb66` (`rt_device_find("aicfb")`-patroon); `0x40039d90` |
| UI-taak | RT-Thread-taak "lvgl", stack 0x8000, prioriteit 20, tick 10 | aanmaak rond `0x40017ad6`–`0x40017b30` |
| lettertypen | `font_default`, `font_preset`, `font_tuner`, `font_detail`, `font_dsmall`, `font_settings` | `0x400896b8`… |
| iconen | uit partitie `ui_asset`, pad `U:/icons/%s.bin` | `0x4008964c`, ref `0x4001ba20` |
| knoppen | `knob`, `master_knob`, `preset_knob` | refs `0x4001c180`, `0x4001e09a`, `0x4001e0ca`, `0x400229fc` |
| Bluetooth | waarschijnlijk een aparte BLE-chip op **UART5** (`uart5`, `uart5_rx`, `uart_tx`) | `0x4008b9c8`… |
| MIDI | byte-parser `0x4002de1c`, berichtfunctie `0x4002db26` (gehaakt, zie `ble-midi.md`) | |
| audio-init | I2S/codec-registers, tellers op nul, deadline uit de timerfrequentie | `0x400108cc`–`0x40010bb2` |
| audiotellers | som `0x3008a9e0`, calls `0x3008a9f0`, deadline `0x3008a9fc`, overruns `0x3008aa00`; schrijvers o.a. `0x4000ee20`, `0x4000f952`, `0x40010d3c`, `0x40011588` | |
| kritieke secties | `0x30045ea8` = `csrrci a0,mstatus,8; ret` (interrupts uit), `0x30045eae` = `csrw mstatus,a0; ret` (terug); 401 aanroepen | ITCM |
| FX-dispatch | FX2-familie `0x40069820` (gehaakt); runtime-subtype-array `0x400b0ec0` (FX2 = slot 1) | |
| brokverwerker | `0x30043800(dma_in, dma_out, …)`: mono-ingang → float L/R, versterker, dispatcher, uitgang → int32 | ITCM |
| ketendispatcher | `0x30041d7e(0x301038b8, inL, inR, outL, outR, 64, ctx, res)`, procestabel `0x4008ea9c` (12 × 8 B) | ITCM (gehaakt op `0x3004395e`) |
| versterker/NAM | `0x400733f4(state, L, R, params, type, vlag)`, state `[0x3010487c]` | aanroep gehaakt op `0x3004392a` |

## Fase A: alleen HARDCORE in het audiopad

**Doel:** de hele Livtra-keten (versterkers, NAM, IR's, effecten, tuner) draait niet meer.
Input → `chain_process` → output, met de volledige CPU.

**Status (2026-11-27):** gebouwd en in emulatie bewezen (stap 3 hieronder). Nog niet op hardware.

**Aanpak:** de per-blok-functie vinden die de effectketen aanroept, tussen de
DMA-buffer-ophaal en de teruggave. De timingtellers worden juist rond die aanroep
bijgewerkt: begin bij de schrijvers van `0x3008a9e0`/`0x3008aa00`
(`0x4000ee20`, `0x4000f952`, `0x40010d3c`–`0x40011588`). Daar wordt de keten-aanroep
vervangen door onze processor. De haak naar FX2 blijft als terugval tot dit op hardware
is bewezen.

**Gevonden (2026-11-27):**
- **Audiotaak `0x4001140a`**
  - Neemt een semafoor (`0x3004635e`, wachten = −1), daarna haalt hij blokindices uit een
    ring van 8 (`index & 7`).
  - Per index roept hij de **blokverwerker `0x30044580(idx)`** in ITCM aan.
  - Een tweede pad (`0x400114ee`, vanuit de DMA-interrupt, als een vlag staat) roept
    dezelfde functie direct aan.
  - Gemiste blokken gaan via `0x4000f940(n)` naar de overrun-teller (`0x3008aa00`).
- **Blokverwerker `0x30044580`–`~0x300459xx`** (ITCM, ~5 KB)
  - Proloog: `jal t0, 0x4000c3a4`, en `fs0`–`fs9` worden bewaard. Argument `a0` = blokindex.
  - Leest de timer (`0x3004393c`) en houdt min/max/telling bij in de struct op
    `0x3008a9e0` (+0x24/+0x28/+0x2c/+0x30/+0x34).
  - Leest en wist I2S-/codecstatus op `0x1860000c`/`0x18600018` (bits 0x2 en 0x40 worden
    geteld).
  - Bufferadres = basis + `idx * blokgrootte` (`s8`, gelezen rond `0x30044588`).
  - Cache-onderhoud met T-Head-instructies (`0x0180000b`, door `llvm-objdump` als
    `<unknown>` getoond).
  - Int↔float-conversie: `fcvt.s.w` / `fcvt.w.s rtz` rond `0x30044bf8`–`0x30044f20`.
  - Daarna de keten-aanroepen (`0x30042780`, `0x30043800`, `0x300426c0`, `0x30048532`, …).

**Twee mogelijke haakpunten:**
1. De hele blokverwerker vervangen. Dan moeten wij het buffer- en cachewerk en de
   I2S-status zelf doen.
2. Na de input-conversie en vóór de output-conversie haken, zodat alleen de keten wordt
   vervangen.

Optie 2 is veiliger. Volgende stap: de conversielussen rond `0x30044bf8` en `0x30044d22`
volgen tot de float-buffers en de eerste keten-aanroep, en in emulatie één blok laten
lopen met een gesimuleerd DMA-buffer.

**Stap 2 (2026-11-27, voortgang):**
- **DMA-bufferformaat:** interleaved stereo **int32**, verwerkt in brokken van `0x200` bytes
  (64 frames).
- **Brokkenlus:** begint op `0x300447b0`. De teller staat op `sp+0x14` (+0x40 per brok)
  en loopt tot `s1` = de helft van de blokgrootte. Twee bufferpointers: `s6` en `s10`.
  Sprong terug naar de luskop vanaf `0x30044be8`.
- **Twee paden per brok**, gekozen met vlaggen op `0x30103130`-achtig (`lbu` rond
  `0x30044bb0`):
  - **Pad 1:** `0x30046900` = `memcpy(s6, s10, 0x200)`, dan helper `0x4001068e`. Dat is een
    kleine reset/clear-helper die `0x400271a2` en `0x400100ba` aanroept, **niet** de
    effectketen. Daarna, voorwaardelijk, `0x4001042e(s10)`.
  - **Pad 2:** per frame int32 → float (`fcvt.s.w`), aanroep `0x300471a2(state, &L, &R, 1)`,
    terug naar int32 met `fcvt.w.s rtz`, dan `0x400100ba`. Dit is een
    per-sample-trap; de functie ervan is nog onbekend.
- **De eigenlijke Livtra-keten** zit waarschijnlijk verder in de functie: vanaf `0x30044c48`
  tot `0x300457c8`, met de conversies rond `0x30044d22`/`0x30044dec`/`0x30044f02` en de
  aanroepen `0x300426c0`, `0x30043800`, `0x30042780`, `0x30048532`.

**Stap 3 (2026-11-27): haakpunt gekozen en gebouwd.** Gelezen met T-Head-decodering
(`llvm-objdump --mattr=…,+xtheadmemidx,+xtheadba,+xtheadcondmov,…`; daarmee verdwijnen
bijna alle `<unknown>` in de audiocode).

- **Het pad op `0x30044c48`** (vlaggen `0x30104880`/`0x301041b8`) is geen keten. Het
  neemt 4:1 af naar een 16-bit-ring op `0x400a1fc0`, of speelt een USB-bron af. Het
  normale pad (beide vlaggen 0) springt naar `0x30044f8e` en roept per brok
  **`0x30043800(dma_in, dma_out, usb, …)`** aan.
- **Brokverwerker `0x30043800`** (per 64 frames):
  1. Ingang **mono**: alleen het linkerkanaal van het DMA-blok.
     `(x >> 8) · 2⁻²³ · ingangsgain(0x3008ab3c)` gaat als float naar **L `0x301034b8`**
     en **R `0x301035b8`** (dezelfde waarden).
  2. `0x4002717e(0x3010412c)`: preset-wissel-mute. Geeft 0 terug als de uitgang stil
     moet; dan worden de buffers gewist.
  3. Optioneel **versterker/NAM `0x400733f4`** (alleen als `0x30104879` en het slot
     `0x30104810 + 0x34·[0x30104878]` aan staan). State `[0x3010487c]` is ~118 KB
     (NAM-runtime 115.916 B). Werkt ter plekke op L/R.
  4. **Ketendispatcher `0x30041d7e(state 0x301038b8, inL, inR, outL 0x301036b8,
     outR 0x301037b8, 64, ctx, res)`**. Hij loopt de 10 slots van het preset af via de
     **procestabel `0x4008ea9c`**: 12 soorten × (processor, vlaggen), net als in 1.12.
     Soort 8 = `0x40004afa` = de FX2-familie (`0x40069820`). Hij wisselt buffers af en
     schrijft in `res`: **`{float *L; float *R (0 = mono); u8 stereo}`**.
  5. Daarna native: `0x400271a2` (fade), volume `0x3008ab48`→`0x3008ab44`, master
     `0x3008ab4d`/100. `0x30042ab6` zet om naar int32 in `dma_out`
     (`x · gain · 0,5 · master`).
- **Gekozen haakpunt: punt 4 en punt 3** (dus "optie 2": native conversie, volume,
  cache en DMA blijven).
  - `0x3004395e` (12 B: `auipc a0 / addi a0 / jal 0x30041d7e`) wordt
    `auipc ra / jalr nc_hc_dispatch / nop`. Een `jal` reikt niet van ITCM naar de payload.
  - `0x3004392a` (8 B: `auipc ra / jalr 0x400733f4`) wordt `auipc ra / jalr nc_hc_amp`.
  - Tijdens de eerste 3 s audio (2250 blokken) en na een CPU-fout roepen beide de
    **native** functie aan: dan is het stock 1.2. Daarna draait alleen `chain_process`
    (inL → outL/outR, stereo) en slaat de versterker/NAM over. MIDI-CC's en noten gaan
    dan altijd naar HARDCORE. SysEx en programmawissel blijven native (regel 1).
- **Emulatie** (`emulate.py` §6, met `xthead.py` voor de T-Head- en P-instructies): de
  echte `0x30043800` loopt in Unicorn met een gesimuleerd DMA-blok. Uitkomsten:
  - 2250 blokken native, en de lege stock-keten geeft het signaal door (fout < 1,2·10⁻⁷
    op schaal 0,5);
  - daarna 200 blokken eigen keten, waarbij de versterker 200× is overgeslagen en de
    909 slaat (piek 0,36);
  - maximaal 43,7 % van het blok, bij 360 instr/µs;
  - met een te trage klok latcht de bewaking en neemt de native keten het weer over.

**Nog niet bewezen:** alles op hardware: caches, echte timing, of de NAM-state na lang
overslaan schoon blijft. De FX2-haak blijft in het pakket, maar draait na de grace niet
meer (de dispatcher die hem aanriep is vervangen). Het pakket op de installatiepagina
(`docs/install/hardcore-1.2.json`) is nog niet bijgewerkt: dat is een release, en
daarover beslist de eigenaar.

Werkwijze voor het lezen: `tools`-scripts staan niet in de repo. Maak ELF-wrappers van
de segmenten (`.incbin` in een `.S`, linken met `ld.lld -Ttext=<laadadres>`) en
disassembleer met de `--mattr` hierboven. Wat dan nog `<unknown>` is met opcode `0x77`,
hoort bij de P-extensie (bv. `ADD64`).

**Open vragen:**
- Ingangsgain `0x3008ab3c` en uitgangsgain `0x3008ab48` komen uit de globale
  instellingen. Welke waarden de pedal daar zet, is nog niet gemeten.
- Er is geen aparte noise gate vóór de dispatcher gevonden. Een gate zit dus als slot in
  de keten en valt met fase A weg.

## Fase B: eigen scherm en knoppen

**Doel:** het HARDCORE-ontwerp (A en B: draaien en drukken) in plaats van de Livtra-UI.

**Status (2026-11-27):** gebouwd en in emulatie bewezen. Nog niet op hardware.

**Bevindingen (stock 1.2):**

| onderwerp | bevinding | adres |
|---|---|---|
| GUI | **LVGL 9** (niet 8): `lv_display_create(w, h)` `0x4004fd76`, `set_buffers(…, DIRECT)` `0x4004ffac`, flush-cb `+0x28` | |
| LVGL-taak | `"LVGL"`, lus: lock → **`lv_timer_handler` `0x4004f4b8`** → unlock → `0x40017eb4` → `mdelay(8..16)` | `0x40017a86`, aanroep `0x40017ab2` |
| displayinit | `0x40017c68`: opent `aicfb` (singleton `0x30118f3c` = `{rt_device*, refs}`), ioctl **`0x4662`** = scherminfo `{format, bpp, stride(bytes), width, height, framebuffer, smem_len}`, twee buffers (fb, fb + stride·h) | |
| flush | `0x40017b86`: cache schoonmaken `0x40017b48(adres, lengte)`, ioctl **`0x4643`** (pan, arg = &index 0/1), `0x4663` (eenmalig, aanzetten), **`0x4620`** (vsync) | ioctl-wrapper `0x4005cb68(handle, cmd, arg)` |
| pixelformaat | tabel `0x40088f84`: mpp-formaat → LVGL-formaat. 14 = RGB565, 0/4 = (A/X)RGB8888 | |
| paneel | ST7789, init-tabel met MADCTL `0x00` (mogelijk portret 240×320) | `0x4008c36c` |
| encoders | GPIO-interrupts tellen int16-delta's. **enc1**: PB.5 (irq) / PB.6 (richting) → `0x30117f74`. **enc2**: PE.9 / PE.10 → `0x30117f76` | ISR `0x40042b3e`, `0x40042b18` |
| drukknoppen | **PB.7** (`[0x30117f78]`): kort → `0x30117f86`, lang (> 0,6 s) → `0x30117f84`. **PE.11** (`[0x30117f7c]`): kort → `0x30117f87`. Actief laag, `rt_pin_read` `0x40045290` | scanner `0x40042d50` (taak `enc_poll`, 2 ms) |
| knoppendispatcher | `0x40017f72` (elke 8 ms): leest en wist de tellers met `0x40042c7e`/`ca8`/`cd2`/`cfc`/`d26` en stuurt ze naar de Livtra-UI | aanroep `0x40018004` |

**Haken** (`build.py`, beide `jal ra`, met controle op de originele bytes):
- `0x40017ab2`: `jal lv_timer_handler` → **`jal nc_hc_ui_tick`** (`screen.c`).
  - De eerste 3 s na de start van de LVGL-taak draait LVGL gewoon (regel 2).
  - Daarna vraagt hij de scherminfo op. Bij RGB565 of (A/X)RGB8888, 320×240 of 240×320,
    en genoeg ruimte voor twee buffers neemt onze code het scherm over.
  - Vanaf dan draait LVGL niet meer. Wij tekenen het opstartscherm en daarna de UI met
    de vector-engine in de achterste buffer, maken de cache schoon, pannen en wachten op
    vsync.
  - Anders blijft alles native (`nc_hc_screen[1]` = reden).
- `0x40018004`: `jal 0x40017f72` → **`jal nc_hc_enc_dispatch`**. Native zolang onze UI het
  scherm niet heeft, daarna leeg. Onze UI leest dan zelf de encoder-delta's (met de native
  lees-en-wis-functies) en de knop-pinnen (ontdenderd), dus ook lang drukken en A+B.

**UI** (`ui/hcui.c`, alles vector):
- Home: de vier blokken met een vectoricoon en hun hoofdwaarde, plus een macrobalk
  (B = FREQ 1).
- Blokpagina's: 47 parameters, alle CC's van de keten.
- Gebaren zoals in het ontwerp:
  - A draaien = kiezen, A kort = openen/volgende pagina, A lang = home;
  - B draaien = waarde (grof 4, fijn 1), B kort = fijn of schakelaar omzetten,
    B lang = beginwaarde (bipolair exact 0);
  - A+B kort = bypass (5 ms ramp), A+B lang = presets (fase C).
- Waarden komen altijd uit de draaiende keten. Wijzigingen gaan als CC via een eigen
  ring naar de audiotaak.
- Er wordt alleen opnieuw getekend als het beeld verandert (handtekening over de
  toestand en de zichtbare waarden).
- Voorbeelden: `docs/img/ui-home.png`, `ui-sherman-env.png`, `ui-portret.png`, of `make ui`.

**Emulatie** (`emulate.py` §7, echte gepatchte image; aicfb-ioctl's, pinnen en de
native LVGL/knop-functies onderschept):
- 3 s native, daarna overname;
- pan 1, 0, 1, 0;
- het opstartframe is 100 % pixelgelijk aan de host-render;
- B +2 levert één CC in de wachtrij, A kort opent een blok;
- daarna geen native LVGL of knopafhandeling meer;
- een tick zonder wijziging tekent niets (~2.600 instructies);
- RGB888 → geen overname, alles native.

Kosten per frame: opstart ~3,8 M instructies (10,5 ms), homescherm ~7,5 M (20,9 ms),
alleen bij een wijziging.

**Hardwaretest v0.1-beta1 (2026-11-28):**
- De 909 klinkt, dus fase A werkt op de pedal.
- Het scherm bleef Livtra en de knoppen bedienden de Livtra-UI: de overname is niet gebeurd.
- Vermoedelijke oorzaak: mijn controle was strenger dan de native displayinit. Ik eiste
  ioctl-return 0, een `bpp`-veld van 16/32 en `smem_len` ≥ 2 frames. De native code
  controleert geen van die drie en leidt de pixelgrootte af uit `format` via tabel
  `0x40088f84`.
- Gerepareerd: de overname volgt nu de native code. Pixelgrootte via die tabel (16/24/32),
  returnwaarde en `smem_len` genegeerd, buffer 2 = fb + stride·h, elke resolutie van
  64..800. De firmware bevat panelmodi voor 240×320 én 320×240 (`0x3008b2b8`…`b3e4`).
- **Hoorbare diagnose (beta, `NC_DIAG_BEEPS`):** ~7 s na de start van de eigen keten piept
  de pedal zo vaak als de reden waarom het scherm niet is overgenomen:

| piepjes | betekenis |
|---|---|
| geen | scherm is van HARDCORE |
| 1 | geen aicfb-handle |
| 3 | onbekend pixelformaat |
| 4 | onwaarschijnlijke resolutie |
| 5 | framebuffer of stride ongeldig |
| 6 | de UI-haak (`0x40017ab2`) is nooit aangeroepen |

**Hardwaretest v0.1-beta2 (2026-11-28):** het scherm en de knoppen werken op de pedal.
Na een tijdje verscheen **CPU LIMIT**.
- Oorzaak (vermoedelijk): fase A gebruikte nog de FX2-bewaking. Eén blok boven 67,5 %, of
  3 van de 64 boven 65 %, gaf al een fout die tot de herstart blijft. Die meting telt ook
  onderbrekingen (scherm naar paneel, BLE) mee.
- Nu, omdat fase A het hele blok heeft: de native deadline-bewaking (fout 2) blijft
  zoals hij was. Onze eigen meting is een vangnet: één blok > 95 %, of 8 van de 64 > 80 %
  (`FASE_A_SOFT_PERCENT`, `FASE_A_HARD_PERCENT`).
- Op het scherm:
  - een CPU-meter in de kop (gemiddelde, stappen van 5 %, 2× per s);
  - bij een fout "CPU LIMIT 97%" (eigen meting) of "CHAIN LIMIT 3X 81%" (aantal native
    overschrijdingen en de belasting).
- Opstartanimatie van ~30 naar ~20 fps.

**Op de pedal te bevestigen** (instelbaar in `screen.c`):
1. Welke encoder links zit: `NC_UI_A_IS_ENC1`. Nu enc1 = A, de knop met de native
   lange druk.
2. De draairichting: `NC_UI_DIR_A` / `NC_UI_DIR_B`.
3. Pixelformaat en oriëntatie: `nc_hc_screen[3..6]`. De engine past zich aan, maar de
   echte waarde is nog onbekend.
4. Of iets anders nog in de framebuffer schrijft (niet gevonden).
5. Hoeveel tellen één klik van de encoder geeft.

## Vector-UI (geldt voor fase B en C)

Keuze van de eigenaar (2026-11-27): **alle graphics zijn vector**, in de stijl van de Atari-vectorkasten. Het beeld wordt echt opgebouwd uit lijnsegmenten, zonder bitmaps.

- `ui/vec.c` tekent elk segment als een gloeiende straal. Het licht valt af met de afstand tot de middellijn: kern ≤ 0,55 px, gloed tot 3 px, optellend en verzadigend. Kruisingen en hoekpunten lichten dus feller op, net als op de buis; overstuurd wordt het wit.
- Eigen streepfont op een raster van 4×6 (hoofdletters, cijfers, leestekens). `vec_text3d` geeft geëxtrudeerde letters (achtervlak, diepte-lijnen, voorvlak).
- Tekenbudget: een scherm kan segment voor segment "door de straal" worden opgebouwd.
- Kosten op de pedal (Unicorn, RV32 -O2): volledig opstartframe ~4,1 M instructies ≈ 11,5 ms bij 360 instr/µs. Gewone schermen tekenen alleen opnieuw bij een wijziging.
- Pixelformaat: de engine schrijft RGB565. Het formaat van `aicfb` wordt in fase B bevestigd.

**Opstartscherm** (`vec_boot(fb, t_ms)`, een pure functie van de tijd):
1. warp-sterrenveld (0–1,1 s);
2. **NANOHARDCORE** in schuine, geëxtrudeerde oranje vectorletters, streep voor streep getekend (0,45–1,75 s);
3. twee horizonlijnen die vanuit het midden openen;
4. **BY PLIEUWSKI** in teal (1,9–2,6 s).

Daarna blijft het beeld staan met langzame sterren. Gemaakt in de geest van de Atari-vectorkasten, met eigen letters, zonder logo's of lettertypes van anderen. Voorbeeld: `docs/img/opstartscherm.gif`, of `make ui`.

## Fase C: apparaat

- Eigen opstartscherm: **gebouwd en op de pedal bevestigd** (v0.1-beta2, 2026-11-28).
- MIDI: de eigen indeling bestaat al. Programmawissel kan eigen presets laden.
- ToneCommand-protocol (NC70/71) niet nodig. **Behalve de updater-SysEx**: die blijft
  native, of wordt door onze code nagedaan (regel 1).
- Eigen presets: **opslag uitgezocht, nog niet gebouwd** (zie hieronder).

**Bevindingen presets-opslag (stock 1.2, 2026-11-28):**

| onderwerp | bevinding | adres |
|---|---|---|
| partities | MTD-string `spi0.0:152k(spl),8k(env),848k(os),688k(presets),348k(ui_asset),4k(device_info)`, flash `norflash0`; per partitie `blk_%s` | `0x400883c0` |
| partitie zoeken | `0x4000e65e(naam)` → partitie-object (0x40 B: naam +4, flash-naam +0x1c, offset +0x34, grootte +0x38) | |
| opslagmodule presets | handle `0x30113bd8`. Controle `0x400350e6`: grootte ≥ `0xac000` en wis-eenheid `0x1000`. Lezen `0x40035116(off, buf, len)`, schrijven `0x4003515e(off, buf, len)`, nog een functie `0x400351a6`. Offsets zijn partitie-relatief | |
| blok-API | 4K-blok wissen `0x40035e3a(off)`, schrijven en verifiëren `0x40035cbe`, blok schrijven `0x40035eda(off, buf)`, controlesommen `0x400356cc`/`0x400356fc` | |
| index | A/B-journaal van één indexrecord (`0x650` B plus een trailer van 16 B) op **`0xaa000` en `0xab000`**. Het actieve blok staat in `0x30113c18`. Kop van `0x614` B op offset 0 | `0x400363a8`… |
| ruimte | `0x0`…`0xaa000` wordt dynamisch toegewezen (blokken met kop en offset). Er is **geen vast vrij gebied**: ook de laatste 8 KB is in gebruik (het indexjournaal) | |

**Presetbank (gevonden 2026-11-28):**

| functie | betekenis | adres |
|---|---|---|
| bank | 64 plekken. Preset *n* staat op partitie-offset **`(n+4)·0x400`**, max 1 KB. Indextabel `0x400bfe18` (+0x14 + n·0x18: crc32 op +0x14, lengte op +0x18). Actieve preset `0x400bfe1e` | |
| lezen | `0x40037544(n, buf, cap, &len)`: leest en controleert de CRC (`0x4000ec12`) | |
| schrijven | `0x4003785a(n, data, len ≤ 0x400)`. Eist dat ontleden (`0x40019e2c` → struct van `0x35c` B) plus opnieuw serialiseren (`0x40019c7e`) **exact** dezelfde bytes geeft, en een niet-lege naam (16 B op +2). Daarna flash met journaal en CRC. Aangeroepen door ToneCommand (`0x40018368`, `0x40019ad8`) | |
| wissen | `0x40037608(eerste, aantal)`; met (0, 64) wordt de hele bank geformatteerd (`0x40035f74`) | |
| actieve preset | `0x40037316(&n)` haalt hem op; `0x40037352(n)` zet hem (journaal `"1VJP"` op `0x16000`/`0x17000`) | |
| formaat | TLV (type, lengte, waarde): 1 = ?, 2 = naam (16), 3 = ?, `0x10` = effectslot (8 slots van `0x64` B op struct+0x1c, slotnummer ≤ 7), `0x20`…`0x2c` via sprongtabel `0x400890fc`. Typen > `0x25` worden genegeerd, en vallen daardoor bij het herserialiseren weg (het schrijven wordt dan geweigerd). Versie: struct+0 = 7. Per slot controleert `0x40019b4a`/`0x40019b1e`/`0x40019ba8` het model en het aantal parameters | `0x40019e2c` |

Gevolg voor HARDCORE-presets: er past geen eigen blok in. De waarden moeten in
**geldige** Livtra-parameters van een eigen presetplek staan, en het schrijven moet via
`0x4003785a`, zodat de native controle, het journaal en de CRC blijven werken. Eerst
echte presets bekijken: de uitleesfunctie leest daarom ook `presets` (`P R E`) en `env`
(`E N V`), alleen lezen (v0.1-beta5).

Gevolg: zelf in de partitie wissen of schrijven kan de Livtra-presets van de gebruiker
beschadigen. Veilige opties:
1. **Via de native presetroute**: de HARDCORE-instellingen in de effectparameters van een
   eigen presetplek zetten en de native "preset opslaan" aanroepen. De opslagmodule doet
   dan het wissen, het journaal en de controle. Nog uit te zoeken: de RAM-structuur van
   een preset en de opslaan- en laadfunctie.
2. **Eerst zonder flash**: presets in RAM (weg bij uitzetten), en via MIDI te dumpen en
   terug te zetten. Geen risico voor de flash.

## Fase D (alleen na bewijs)

Volledig eigen OS uit de Luban-Lite-SDK; alleen `spl` en `env` blijven over. Voorwaarde:
aantonen dat de bootloader of de BROM de updater ook opent als de app niet opstart,
bijvoorbeeld een toets bij het aanzetten of de BROM-USB-modus. De chips zitten achter het
scherm en zijn vrijwel onbereikbaar. Zonder die route betekent één fout een brick.

**D0, stand van het bewijs (2026-11-28): niet aangetoond.**
- De updater ("Livtra Lupgrader", USB `33c3:6678`, DFU-interface `os`, blokken van
  4096 B) zit in de SPL. Het OS start hem na de SysEx `F0 7D 44 46 55 01 F7`
  (re-nanocore `tools/custom_fx/RAW_DFU.txt`). Via DFU is alleen de partitie `os`
  bereikbaar, de SPL niet.
- re-nanocore heeft de SPL (155.648 B) en de updater (99.704 B) ooit uitgelezen met een
  tijdelijke image (`evidence/firmware-decoding.json`, alleen hashes). De binaries zijn
  niet openbaar. Daarom is niet vast te stellen of de SPL de updater ook opent zonder
  werkend OS, met een toets of na een mislukte FIT-controle.
- Het OS zelf opent de partitie `spl` (`"spl"` op `0x4008c000`, gebruikt bij `0x400384d6`).
  Onze code kan de SPL dus **alleen-lezen** via dezelfde MTD-route uitlezen.

**Nieuw bewijs (2026-11-28): de updater hangt van het OS af.**
- De SysEx-verzamelaar `0x400193a2` (max 1 KB) herkent precies
  `F0 7D 44 46 55 01 F7` en roept **`0x400384be`** aan.
- Die functie opent de partitie `spl` (`0x4000e65e("spl")`) en zoekt de updater erin op
  (`0x400383fc`). Hij leest hem met `0x4000e6ac(partitie, offset, buf, len)` in blokken
  van 4 KB in RAM (venster `0x40300000`–`0x40340000`) en start hem.
- **De gewone herstelroute wordt dus door het OS gestart.** Zonder werkend OS hangt herstel
  af van wat de SPL of de BROM zelf doet, en dat is nog onbekend.
- Verder gevonden:
  - ToneCommand-kop `F0 7D 4E 43 70` ("NCp");
  - de antwoordbouwer `0x4001862a` (kop van 8 B plus ≤ `0x300` B);
  - de **verzender `0x4001854c(poort, cmd, data, len ≤ 0x400)`**, die 7-bit inpakt en
    `F0 7D 4E 43 cmd … F7` verstuurt;
  - schrijffuncties per poort: poort 0 = **USB** (`0x4002ffc6`, gezet via `0x40018d34`),
    poort 1 = **BLE** (de radiomodule zet hem op 0 bij een verbreking).

**D1 gebouwd (2026-11-28): alleen-lezende SPL-dump** (v0.1-beta4; vanaf beta5 ook `presets` en `env`).
- Startsein over USB: CC 84 met de waarden `S`, `P`, `L` (`nc_midi_in`, alleen poort 0).
- `screen.c` leest per UI-tick (~30 ms) 768 B via `0x4000e65e`/`0x4000e6ac` en verstuurt
  dat met de native verzender als `F0 7D 4E 43 5C <7-bit: "NHS1", offset, totaal, crc32,
  data> F7`. De voortgang staat op het scherm ("SPL LEZEN 37%"). Er wordt niets geschreven.
- Pagina `docs/install/spl.html` met `spl.js`: stuurt het startsein, zet de blokken in
  elkaar, controleert elke CRC en vergelijkt de SHA-256 met de bekende re-nanocore-dump
  (`2c254261…`). Daarna download `nanocore-spl.bin`.
- Emulatie (§8): de echte native verzender verpakt; de echte `spl.js` zet het in node
  weer in elkaar tot exact de bron. Een startsein via BLE start niets.

**Veilige volgende stap voor D (D1, oorspronkelijk plan):** een alleen-lezende SPL-uitleesfunctie in de
HARDCORE-firmware. Op een eigen SysEx leest hij de partitie `spl` en stuurt die in
SysEx-blokken met controlesom terug over USB-MIDI. Een pagina zoals de installer slaat het
op. Er wordt niets geschreven. Met die dump is te analyseren of er een herstelroute
zonder app bestaat. Pas met dat bewijs gaat D verder.

