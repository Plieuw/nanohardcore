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

## Fase A: alleen HARDCORE in het audiopad

**Doel:** de hele Livtra-keten (versterkers, NAM, IR's, effecten, tuner) draait niet meer.
Input → `chain_process` → output, met de volledige CPU.

**Aanpak:** de per-blok-functie vinden die de effectketen aanroept, tussen de
DMA-buffer-ophaal en de teruggave. De timingtellers worden juist rond die aanroep
bijgewerkt: begin bij de schrijvers van `0x3008a9e0`/`0x3008aa00`
(`0x4000ee20`, `0x4000f952`, `0x40010d3c`–`0x40011588`). Daar wordt de keten-aanroep
vervangen door onze processor. De haak naar FX2 blijft als terugval tot dit op hardware
is bewezen.

**Open vragen:**
- Het sampleformaat aan die grens (int32/float, gescheiden kanalen of interleaved).
- Wat de native keten doet aan invoertrim, noise gate en uitgangsniveau.

## Fase B: eigen scherm en knoppen

**Doel:** het HARDCORE-ontwerp (Design-artifact, 320×240, A en B draai/druk) in plaats van
de Livtra-UI.

**Aanpak:** de LVGL-taak laten doorlopen tot na de grace, en dan:
- **schermupdates afvangen**: de flush-callback naar `aicfb` op de display-driver van de
  LVGL-port (Luban-Lite `lv_port_disp`) wordt een no-op;
- **zelf tekenen** in de framebuffer, met een eigen bitmapfont in de payload;
- **knoppen afvangen**: de encoder-events in de indev-read-callback (`knob`, `master_knob`).

Te vinden: de framebuffer-info-ioctl, het flush-adres, het indev-read-adres en de
GPIO's van de encoders.

## Fase C: apparaat

- Eigen opstartscherm, zodra het scherm vrij is.
- Eigen presets (knopstanden en CC-staat) in een eigen stuk van de `presets`-partitie.
  Eerst uitzoeken welk deel vrij of veilig is.
- MIDI: de eigen indeling bestaat al. Programmawissel kan eigen presets laden.
- ToneCommand-protocol (NC70/71) niet nodig. **Behalve de updater-SysEx**: die blijft
  native, of wordt door onze code nagedaan (regel 1).

## Fase D (alleen na bewijs)

Volledig eigen OS uit de Luban-Lite-SDK; alleen `spl` en `env` blijven over. Voorwaarde:
aantonen dat de bootloader of de BROM de updater ook opent als de app niet opstart,
bijvoorbeeld een toets bij het aanzetten of de BROM-USB-modus. De chips zitten achter het
scherm en zijn vrijwel onbereikbaar. Zonder die route betekent één fout een brick.
