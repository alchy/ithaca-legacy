# ithaca-gui — Art Deco redesign

**Datum:** 2026-06-01
**Stav:** schváleno k implementaci
**Vychází z:** brainstorm s vizuálním companionem (mockupy v `.superpowers/brainstorm/`, finální `final-deco-v3.html`). Vzor architektury: `/Users/j/Projects/icr2/player/gui/` (stejný stack ImGui+GLFW+OpenGL3).

## Cíl

Předělat ithaca-gui ze stock `StyleColorsDark()` + default font na esteticky výrazné **Art Deco** prostředí (stříbro = struktura, zlato = živé/primární prvky), s modulární strukturou převzatou z icr2 (centralizovaný theme + widget vrstva + sekce per panel), a **připravit místo pro budoucí DSP chain** (AGC → Convolver → BBE → Limiter). Layout musí mapovat všechny současné funkce a být snadno rozšiřitelný.

## Vizuální jazyk

- **Paleta (schéma A — stříbro nese strukturu, zlato je vzácný akcent):**
  - pozadí: `#0d0e10` (near-black), panel/log tmavší `#090a0b`
  - stříbro: text `#cdd2d6`, jasné hodnoty `#e2e6ea`, světlé stříbro `#aab0b6`, linky `#3a4046`/`#242a2f`, tlumené popisky `#7e858c`
  - zlato (akcent): `#d4af37` — **zlato JEN na:** logo, VOICES hodnota, MIDI NOTE lampa, SUSTAIN fill, RESONANCE slider zarážka, aktivní klávesy, DSP LED, LEGACY badge, grid rysky. Vše ostatní je stříbrné/tlumené.
  - **RESONANCE hodnota = stříbrná** (sekundární), **VOICES = zlatá** (primární).
- **Číslice:** arabské (12, 7, 9/32) — čitelné na metr.
- **Font:** **Cormorant** (OFL deco serif, vendorovat TTF) ve více velikostech přes `AddFontFromFileTTF`. Log strip zůstává monospace (stávající/ImGui default mono je OK).
- **Letterspacing** prostrkaných verzálek (logo, nadpisy) přes `ImFontConfig::GlyphExtraAdvanceX` (vzor icr2) — vyžaduje samostatnou font instanci pro „spaced" variantu.
- **Mřížka:** interní layout (sloupce 230 | flex | 280) + vizuálně jen **krátké zlaté rysky na kříženích** sloupcové dráhy s vodorovnými dělítky (var. C). Žádné plné svislé čáry.
- **Glow: VYNECHAT** (jako icr2) — aktivní prvky odlišit jasem/barvou, ne září. `box-shadow` v ImGui neexistuje; nepředstírat ho.
- **DPI:** MVP fixní px (jako icr2). Hi-DPI scaling (`ScaleAllSizes` + content-scale) je **future**, ne v tomto specu.

## Layout (shora dolů)

1. **TOP BAR** (výška ~48px): logo „ITHACA" (zlaté, prostrkané) | MIDI IN dropdown + ⟳ rescan tlačítko + CHANNEL dropdown (OMNI / 1–16) | MASTER slider + dB hodnota (vpravo).
2. **INDICATOR + STAT STRIP** (~78px), zarovnaný do stejných 3 sloupců:
   - **col1 (230px):** MIDI lampy (NOTE zlatá / CC stříbrná) + **SUSTAIN** vodorovný proužek (fill zleva = CC64 0–127, svislá ryska = half-pedal práh, legenda UP / ½ HALF / DOWN).
   - **col2 (flex):** diag dlaždice VOICES (zlatá) · RESONANCE (stříbrná) · RINGS (9/32).
   - **col3 (280px):** **PEAK** dva vodorovné proužky L, R + dB legenda (−∞ … 0).
3. **MAIN ROW** — 3 sloupce **BANK | VOICE | DSP RACK**:
   - **BANK (230px):** SELECT dropdown · TYPE read-only badge `LEGACY`/`FOLDER` (+„· auto") · fakta (8 velocity · N samplů · MB · noty · SR) · RELOAD tlačítko.
   - **VOICE (flex):** slidery RESONANCE / RELEASE / EXCITE DECAY, disabled MAX RESONANCE (restart), dole LOG LEVEL dropdown + RESET.
   - **DSP RACK (280px):** stohované moduly AGC · CONVOLVER · BBE · LIMITER (LED + label + hodnota), dole tok signálu. **Zatím vizuální placeholder** — naplní se až s DSP chainem.
4. **KEYBOARD** (full-width, ~48px): 88 kláves, aktivní zlaté, popisek A0 — SUSTAIN ½ — C8.
5. **LOG STRIP** (full-width): monospace, tlumené, posledních N řádků z log ring bufferu.

## Architektura (modulární, vzor icr2)

| Soubor | Odpovědnost |
|--------|-------------|
| `app/gui/theme.h` | NOVÝ. Barevné tokeny (`struct Colors`), `Fonts` (ImFont* per velikost), `apply_theme()`, `load_fonts()`. Header-only, inline. |
| `app/gui/widgets.h` | NOVÝ. Znovupoužitelné kreslicí helpery: `Eyebrow()`, `LabelValue()`, `Indicator()` (lampa), `ParamSlider()`, `BeginCard/EndCard`, `DrawHBar()` (sustain/peak proužek + ryska), `DrawKeyboard()`, `DrawGridTick()`. Header-only. |
| `third-party/cormorant/` | NOVÝ. Vendorovaný Cormorant TTF + OFL licence. Přidat do `.gitignore`? NE — font je malý a má být v repu (na rozdíl od velkých vendored deps). Ověřit velikost; pokud >~1 MB, zvážit fetch skript. |
| `app/gui/main.cpp` | Nahradit `StyleColorsDark()` voláním `apply_theme()` + `load_fonts()`; layout přepočítat na 3-sloupcovou mřížku (child regiony jako icr2, ne absolutní rekty). |
| `app/gui/panel_topbar.cpp` | Přidat ⟳ rescan + CHANNEL dropdown; přestylovat. BANK selektor PŘESUNOUT odsud do nového bank panelu. |
| `app/gui/panel_bank.cpp` | NOVÝ (vznikne vyčleněním z topbaru). BANK sloupec: select + type badge + fakta + reload. |
| `app/gui/panel_diag.cpp` | Přestylovat na deco dlaždice + indikátory; SUSTAIN a PEAK jako `DrawHBar`. |
| `app/gui/panel_params.cpp` | Přestylovat slidery na `ParamSlider` (eyebrow label + linka + zlatá/stříbrná zarážka). |
| `app/gui/panel_keyboard.cpp` | Přestylovat klaviaturu na deco barvy (`DrawKeyboard`). |
| `app/gui/panel_dsp.cpp` | NOVÝ. DSP RACK placeholder sloupec (stohované moduly, zatím statické). |
| `app/gui/panel_log.cpp` | Přestylovat (tlumené barvy, monospace). |

Sekce dostávají `AppContext& ctx` jako dnes (držíme stávající vzor; icr2 předává `AudioEngine&` — náš `AppContext` agregát je čistší, neměníme).

## Funkční přírůstky (a jejich náročnost — POCTIVĚ)

| Prvek | Náročnost | Pozn. |
|-------|-----------|-------|
| Theme, paleta, layout, slidery, dropdowny, log | **GUI-only** | čistě ImGui/draw, žádný engine zásah |
| Klaviatura, peak/sustain proužky, MIDI lampy, DSP LED, grid rysky | **GUI-only** | `ImDrawList` (čáry, rekty, `AddRectFilledMultiColor` pro 2-barevný gradient) |
| Cormorant font + letterspacing | **GUI-only** | vendorovat TTF, načíst více velikostí + spaced varianta |
| MIDI port **RELOAD** (rescan) | **GUI-only** | `MidiInput::listPorts()` už existuje; jen re-scan + refresh dropdownu |
| **CHANNEL / OMNI** filtr | **ENGINE DEPENDENCY** | dnešní `MidiInput::callback` (midi_input.cpp:98) používá jen `status & 0xF0` a **kanál zahazuje**. Filtrování podle kanálu = nová funkce: uložit zvolený kanál, v callbacku porovnat `status & 0x0F`. GUI připraví dropdown; engine musí přidat filtr. |
| **TYPE legacy/folder autodetekce** | **ENGINE DEPENDENCY / FUTURE** | dnešní `scanBank` (bank_index.cpp) detekuje jen legacy/extended dle názvů, folder-type loader neexistuje (`sample_store.cpp` „extended zatim nepodporovan"). GUI rezervuje read-only badge a čte `bank.type`; samotná detekce + folder loader je **samostatný engine task**. Do té doby badge ukazuje „LEGACY" (jediný podporovaný typ). |
| **DSP RACK** moduly (AGC/conv/BBE/limiter) | **FUTURE** | jen vizuální placeholder; skutečné ovladače přijdou s DSP chain featurou (viz samostatný plán). Layout místo rezervuje. |

## Perzistence

Žádná nová pole nutná pro vizuál. CHANNEL přidá `GuiState.midi_channel` (int, −1 = OMNI) → **schema bump na 3** (stejný pattern jako log_level v2, bez zpětné kompat). Reload je akce, neperzistuje se.

## Testy

GUI kreslení se unit-testuje obtížně (ImGui kontext). Testovatelné headless:
- `test_persistence`: `midi_channel` round-trip + schema-v2-reject (jako u log_level).
- Pokud CHANNEL filtr půjde do enginu: unit test `MidiInput` / engine filtru (nota na jiném kanálu při zvoleném kanálu N se zahodí, OMNI projde vše). **Pozn.:** MidiInput callback je dnes static + RtMidi — test může vyžadovat refaktor na testovatelnou filtr funkci.
- Zbytek (theme, widgets, layout) ověřen buildem `ithaca-gui` + manuálním smoke testem (otevřít, zahrát, přepnout banku, sešlápnout pedál — vizuální kontrola).

## Rozsah / dekompozice

Tohle je **velký** kus. Navrhuju implementační plán rozdělit na fáze, každá samostatně buildovatelná a vizuálně ověřitelná:
1. **theme.h + load_fonts + apply_theme** (Cormorant, paleta) — okamžitě vidět změnu barvy/fontu.
2. **widgets.h** (Eyebrow/LabelValue/Indicator/ParamSlider/Card/DrawHBar/DrawKeyboard/DrawGridTick).
3. **Layout přestavba** main.cpp na 3-sloupcovou mřížku + grid rysky.
4. **Přestylování panelů** (topbar, diag, params, keyboard, log) na widgets/theme.
5. **BANK panel** vyčlenění + TYPE badge (read-only) + RELOAD.
6. **MIDI rozšíření**: port reload (GUI) + CHANNEL dropdown + perzistence; CHANNEL filtr v enginu jako navázaný engine task.
7. **DSP RACK** placeholder panel.

DSP chain samotný (AGC/conv/BBE/limiter funkčnost) a TYPE autodetekce + folder loader jsou **mimo tento spec** (vlastní specy/plány).

## Mimo rozsah (YAGNI / future)

- Funkční DSP chain (jen placeholder UI).
- Folder-type bank loader + autodetekce (GUI jen rezervuje badge).
- Hi-DPI scaling.
- Glow efekty.
- Docking / přesouvatelné panely (fixní layout stačí).
- Myší/klávesové hraní z GUI klaviatury (zůstává jen vizualizace + MIDI vstup).
