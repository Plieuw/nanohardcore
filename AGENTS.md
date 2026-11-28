# Afspraken voor agents (en mensen) in nanohardcore

Eigen effectketen voor de Livtra NanoCore: audio/MIDI-trigger → TR-909 BD → Sherman-achtig
dubbel switched-capacitor-filter → Mackie 1202-VLZ PRO-channelstrip. Zie `README.md` voor het
ontwerp en `docs/` voor de Pages-site.

## Werkwijze
- **Eén branch per taak, altijd een pull request.** Nooit direct naar `main`.
- **Toezicht op `main`:** tot de eigenaar (Plieuw) iets anders aangeeft, beoordeelt de toezichthoudende Claude-sessie alle PR's en merget alleen wat aantoonbaar werkt (groene CI plus een eigen controle van de metingen en het geluid). Andere agents mergen niet zelf.
- **Releases** (een definitieve versie of een nieuwe beta) komen er alleen op verzoek van de eigenaar. Ze worden dan getagd vanaf `main` (`vX.Y` of `vX.Y-betaN`) met release notes en de gemeten eigenschappen.
- **Kleine PR's per blok.** `dsp/bd909.c`, `dsp/sherman.c`, `dsp/mackie.c` + `dsp/mkcirc.c` en `dsp/chain.c` zijn losse blokken. `chain.c` (CC-map, triggering, gate, uitgang) raakt bijna alles; werk daar met maar één agent tegelijk aan.
- **`make test` moet slagen** (de CI draait hem bij elke PR). `test/check.py` toetst de beweringen uit README/docs: 909-pitchverloop en hold, Mackie-EQ, Sherman-resonantie en aliasing, de MIDI-map en de triggerdetectie.
  - Faalt een check, pas dan de code aan, niet de grens.
  - Verander je bewust een bewering, wijzig dan de grens, de README en `docs/index.html` in dezelfde PR, met de meting erbij.
- **Bewijs met een meting.** Een PR die het geluid verandert, laat zien wat er gemeten is (een tabel in de PR-tekst, en waar zinnig een nieuwe check in `test/check.py`). Vertrouw niet op het spectrogram alleen: kleurschalen verzadigen.
- **CPU telt.** Draai `make rv` en zet de MIPS-regels in de PR. Elke instructie per sample telt mee voor de 48 kHz-callback op de pedal (firmware 1.2: 64 frames per blok, 1,33 ms).

## Controles per uitgave (afspraak met de eigenaar, 2026-11-28)
- Zolang de eigenaar een uitgave nog niet als werkend heeft bevestigd: volledige emulatie
  (`emulate.py`) bij elke wijziging aan de firmwarecode, plus de installercheck (node:
  stock → nieuw, vorige → nieuw, terugzetten = stock).
- Heeft de eigenaar gezegd dat de nanohardcore werkt: **lichter**. Volledige emulatie
  alleen als de firmwarecode verandert. Verandert alleen het manifest of de docs, dan is het
  bewijs dat de ELF byte voor byte gelijk is aan de geteste build genoeg, plus de
  installercheck. Op het apparaat kunnen dan ook de diagnose-piepjes uit
  (`NC_DIAG_BEEPS 0`), als de eigenaar dat wil.

## Codeconventies (`dsp/` en `ui/`)
- **UI-blokken:** `ui/vec.c` (engine, font, opstartscherm) en `ui/hcui.c` (schermen, gebaren, parametertabel). `firmware/nanocore-1.2/screen.c` koppelt ze aan de pedal. Voeg je een CC toe, zet hem dan ook in de parametertabel van `hcui.c`.
- **De UI is volledig vector** (wens van de eigenaar): alles op het scherm bestaat uit lijnsegmenten via `ui/vec.c` (`vec_line`, het streepfont, `vec_text3d`). Geen bitmaps, geen bitmapfont, geen beeldbestanden in de firmware.
- Freestanding C99 voor RV32IMAFDC/ilp32d (ook `ui/`): **geen libm, geen malloc, geen schrijfbare statics**. Wiskunde staat in `mathx.h`, alle state zit in de structs.
- Float32 overal. Dure dingen (pow/exp, matrixinversie) alleen bij een knopwijziging (`*_update`), niet per sample.
- **Componentwaarden uit het schema** krijgen hun referentie (`R59`, `C14`, `U40A` …) in de code. **Waarden die niet uit een schema komen** (fits op metingen, gedrag uit handleidingen) heten `CAL_*` of hebben een `CAL`-commentaar met de bron.
- Niet-lineariteiten in de Sherman worden op de klokmomenten berekend (net als de hardware-sampler). Discontinuïteiten krijgen polyBLEP. De Mackie draait 2× oversampled met ADAA en een compensatie-FIR.

## MIDI
- CC 120–127 zijn kanaalmodusberichten: **nooit gebruiken** (`chain_midi` negeert ze; de check faalt als ze iets doen).
- Bipolaire knoppen: CC 64 = exact nul (`ccpm`).
- Voeg je een CC toe, werk dan bij: de header in `chain.c`, de tabellen in `README.md` en `docs/index.html`, en `test/ccmap.c`.

## Niet in de repo
- **Geen auteursrechtelijk beschermde documenten**: Roland service notes, het Mackie-schema, de Sherman-handleiding, datasheets. Noem ze als bron, maar commit ze niet. Dat geldt ook voor uitsneden of scans ervan.
- **Geen vendor-firmware, firmware-images of device-backups van de NanoCore** (zie de afspraken in re-nanocore).

## Taal
README, docs en commentaar voor de gebruiker zijn Nederlands. Code-identifiers en codecommentaar mogen Engels.
