# GUI

Oblast `app/gui/` implementuje grafické uživatelské rozhraní nástroje **ithaca-gui** nad Dear ImGui (backend GLFW + OpenGL 3.3). Celý životní cyklus aplikace je řízen funkcí `main()`: načti persistovaný `GuiState` (nebo defaults) → otevři GLFW okno (DPI scale z `glfwGetWindowContentScale`) → inicializuj ImGui s Art Deco tématem a fonty Cormorant → inicializuj `AppContext` (engine + audio + MIDI + log subscriber; **banka se načítá asynchronně** na worker threadu — okno se ukáže hned) → spusť pomocný thread pro flush RT log ringu → **render loop** (~vsync, typicky 60 Hz; per frame `renderShell()` + zrcadlení DSP + `pollReloadCompletion()` + modální overlay) → finální `saveState` → shutdown v opačném pořadí.

`renderShell()` sestavuje plnoobrazovkové kořenové okno `##root` a rozděluje ho do vodorovných pásem: **top bar** (logo + MIDI dropdown + CH + SR/BUFFER + LOG level + RESET) → **indicator strip** (MIDI lampy + sustain bar + 5 diagnostických dlaždic vč. DSP LOAD + peak metr L/R) → **hlavní řada 3 sloupců** (BANK 250 px | stránka parametrů flex | CONFIG selektor 290 px) → **klaviatura 88 kláves** → **LOG strip** (pohltí zbytek výšky). Aktivní stránka uprostřed se volí přes `ctx.state.config_page` (0 = MASTER, 1 = RESONANCE, 2–5 = DSP stage CONVOLVER/AGC/ENHANCER/LIMITER): klik v CONFIG panelu přepne index, `renderParamPage` pak genericky nakreslí příslušnou `IParamPage`.

Engine je přístupný výhradně skrze `AppContext::engine`; runtime parametry se zapisují buď přes atomické settery, nebo přes `IParamPage::set()`, přičemž přeshraniční čtení diagnostických hodnot z GUI vlákna je bezpečné díky `std::atomic` polím enginu. Stav se ukládá do `state.json` (schema **v5**) s debounce 1 s (atomicky: zápis do `.tmp` + rename).

---

## Implementováno v souborech

| Soubor | Odpovědnost | Klíčové typy |
|---|---|---|
| `main.cpp` | Vstupní bod: CLI parse, GLFW/ImGui init, DPI scale, render loop, shutdown sekvence. V anonymním namespace `Debouncer`, `drawLoadingOverlay()`, `renderShell()` | `GuiState`, `AppContext`, `MasterPage`, `ResonancePage`, `IParamPage*[6]` |
| `app_context.h` / `app_context.cpp` | Vlastník všech těžkých objektů (engine, audio device, MIDI, log buffer, stav panelů); inicializace z persistovaného stavu a shutdown; async bank reload worker | `AppContext`, `PanelState`, `audioCallback()` |
| `persistence.h` / `persistence.cpp` | JSON load/save `GuiState` (schema v3/v4 → v5 migrace); atomický zápis přes `.tmp` + rename; platformní cesta | `GuiState`, `DspStageState`, `WindowGeom`, `loadState()`, `saveState()` |
| **`dsp_state.h`** | Most `DspChain` ↔ `GuiState::dsp`. Generické přes `stageCount()`/`paramCount()`, klíčováno `Param::id` | `dspStateFromChain()`, `applyDspStateToChain()` |
| **`state_binding.h`** | Most `GuiState` ↔ `Engine`. Validace persistovaných hodnot + runtime settery | `engineConfigFromState()`, `applyStateToEngine()` |
| `log_subscriber.h` / `log_subscriber.cpp` | Thread-safe kruhový buffer 256 log eventů; snapshot pro GUI render | `LogRingBuffer` |
| `master_page.h` | Adaptér `IParamPage` „MASTER": MASTER gain + RELEASE | `MasterPage` |
| `resonance_page.h` | Adaptér `IParamPage` „RESONANCE" (hasEnable): Resonance Layer (dyn. rozsah) + Gain + Excite Decay + Max Resonance; override `resetToDefaults()` | `ResonancePage` |
| `theme.h` | Art Deco barevné tokeny, 4 fonty Cormorant, `apply_theme()`, `load_fonts()` | `Colors`, `Fonts` |
| **`embedded_font.cpp`** | Zabudovaný Cormorant (komprimované C pole generované při buildu) | `cormorantCompressedData/Size()` |
| `layout.h` | Jediný zdroj pravdy pro všechny rozměry GUI (logické px konstanty) | `Dims`, `g_scale` |
| `widgets.h` | Art Deco widgety kreslené přes `ImDrawList` | `TextRow`/`Span`, `DecoSlider`, `StatTile`, `Keyboard`, `HBar`, `ToggleChip`, `Lamp`, `Eyebrow` |
| `panel_topbar.{h,cpp}` | Top bar: logo, MIDI IN + RESCAN, CHANNEL, SR + BUFFER, LOG level, RESET | `renderTopBar()` |
| `panel_bank.{h,cpp}` | Levý sloupec: výběr banky, TYPE badge, statistiky, badge „NEUPLNA (RAM limit)", RELOAD | `renderBankPanel()`, `scanBanks()` |
| `panel_indicators.{h,cpp}` | Indicator strip: NOTE/OFF lampy, sustain bar, 5 diagnostických dlaždic, peak L/R | `renderIndicatorStrip()` |
| `panel_keyboard.{h,cpp}` | 88kláves vizualizace aktivních a rezonujících not | `renderKeyboardPanel()` |
| `panel_params.{h,cpp}` | Generický renderer `IParamPage` (ON/OFF toggle + volič + DecoSlidery + metr) | `renderParamPage()` |
| `panel_config.{h,cpp}` | Pravý sloupec CONFIG: seznam stránek s LED + výběr klikem | `renderConfigPanel()` |
| `panel_log.{h,cpp}` | Log strip: snapshot 50 nejnovějších eventů, auto-scroll, obarvení dle severity | `renderLogPanel()` |

**Testy:** `test_persistence` (round-trip, generické DSP klíče, migrace v3/v4→v5, sanitizace), `test_gui_dsp_state` (zrcadlení chain ↔ state, `resetToDefaults`), `test_gui_state_binding` (`GuiState` → `EngineConfig`, validace), `test_log_subscriber`.

---

## Generický DSP stav (`dsp_state.h`, `GuiState::dsp`)

Historicky měl `GuiState` 16 plochých polí (`agc_target`, `convolver_mix`, …). Přidání jednoho parametru do DSP stage znamenalo úpravu na **šesti** místech: `GuiState`, `loadState`, `saveState`, `initFromState`, zrcadlení v render loopu a řetězec porovnání pro debounce. Zapomenutí kteréhokoli byla **tichá** chyba — parametr se neuložil nebo neaplikoval po restartu a žádný test to nechytil. `Param::id` i `Param::def` přitom byly mrtvá pole, která se nikde nečetla.

Nyní:

```cpp
struct DspStageState {
    bool enabled = false;
    int  choice  = -1;                     // -1 = stage nemá volič
    std::map<std::string, float> params;   // Param::id -> hodnota
};
std::map<std::string, DspStageState> dsp;  // "CONVOLVER" -> stav
```

| Funkce | Vlákno | Vysvětlení |
|---|---|---|
| `dspStateFromChain(GuiState&, DspChain&)` | GUI (per frame) | Chain → state. Projde `stageCount()` × `paramCount()`, klíčuje přes `param(j).id`. Volá se každý frame z render loopu, protože panely mění stage přímo a `saveState` by je jinak neviděl. |
| `applyDspStateToChain(const GuiState&, DspChain&)` | GUI (init) | State → chain. Chybějící stage/parametr se přeskočí (stage si drží vlastní default), neznámý klíč se ignoruje — `state.json` může být starší nebo ručně editovaný. Volič se nastavuje **až po** parametrech (u Convolveru `selectChoice` přestavuje IR, které z parametrů vychází). |

Nová stage nebo nový parametr projde persistencí **bez jediné změny v GUI kódu**.

> **Pozor na pořadí:** `applyDspStateToChain` musí běžet až po `engine.init()`, protože ten volá `dsp_.prepare()` a teprve tam Convolver naplní seznam IR (`choiceCount`). Dřív by se persistovaný volič zahodil guardem `st.choice < page.choiceCount()`.

---

## `state_binding.h`

| Funkce | Vysvětlení |
|---|---|
| `engineConfigFromState(GuiState&) → EngineConfig` | Sestaví `EngineConfig` a **validuje** persistované hodnoty: `sample_rate ≤ 0` → 48000, `block_size` clamp `[32, 8192]`, `master_gain = pow(10, db/20)`. Validované hodnoty zapisuje **zpět do `state`** — jinak by GUI ukazovalo něco jiného, než čím engine jede, a nesmysl by se při příštím uložení zapsal znovu. Nastavuje `rt_priority = true` (GUI je reálná audio aplikace; default v `EngineConfig` je `false`, aby ji testy a offline render nedostaly). |
| `applyStateToEngine(Engine&, const GuiState&)` | Runtime settery: `setMaxResonanceVoices`, `setResonanceEnabled`, `setResonanceGainDb`, `setResonanceLayerDb`. |

Obojí bylo dřív zavařené uvnitř `initFromState` dohromady se startem audio device a MIDI, tedy neotestovatelné.

---

## `main.cpp`

### Pomocníci (anonymní namespace)

| Entita | Vysvětlení |
|---|---|
| `struct Debouncer` | `touch()` = zaznamenej okamžik **první** změny (persistence: ukládá se ~1 s od první změny, takže při tažení slideru se průběžně ukládá). `retouch()` = od **poslední** změny (rezonanční cache: 400 ms ticha, přestavba je drahá). `expired(delay)` po vypršení resetuje. |
| `drawLoadingOverlay(ctx, W, H, license_bad)` | Fullscreen topmost okno (`SetNextWindowFocus` → pohltí veškerý vstup, poloprůhledné tmavé pozadí). Dva režimy: **průběh načítání** (`ImGui::ProgressBar` s `ithaca::bankLoadFraction`, text fáze s čítači, řádek „RAM: X / Y MB (budget)", při `truncated` červené varování) nebo **neplatná licence** — anglický text „Soundbank is corrupted or license file is invalid." + tlačítko „Click to continue", které drží dokud uživatel nepotvrdí (`clearBankLicenseInvalid()`). |
| `renderShell(ctx, pages, n_pages, n_reset, W, H)` | Kořenové okno a všechna vodorovná pásma. Viz níže. |

### Vertikální rozpočet

Hlavní řada je zastropena na `Dims::main_h_max` (280 px), klaviatura má pevnou výšku, **LOG pohltí zbytek**:

```cpp
const float avail = ImGui::GetContentRegionAvail().y;      // od kurzoru dolů
const float below = kbd_h + log_h + 2 * row_gap;
float main_h = std::min(Dims::main_h_max, avail - below);
...
ImGui::BeginChild("##log", {content_w, 0});                // 0 = přesně zbytek
```

Dřív se sčítala pevná výška všech pásem včetně ručně spočítaného `9 * ItemSpacing.y`. Ta devítka byla vázaná na počet sekcí kořenového okna — při přidání pásma se na ni zapomínalo, obsah přesáhl okno a šlo o pár px scrollovat. Nyní se měří živě a `BeginChild` s výškou 0 rozpočet dopočítá sám, takže nic nikdy nepřesáhne.

### Render loop

1. `glfwPollEvents()` + aktualizace `ctx.state.window` (per frame, aby layout reagoval na resize).
2. ImGui frame → `renderShell(...)`.
3. `dspStateFromChain(ctx.state, ctx.engine.dspChain())` — zrcadlení DSP do stavu.
4. `ctx.pollReloadCompletion()`; při `reloadInProgress()` nebo `bankLicenseInvalid()` → `drawLoadingOverlay(...)`.
5. **Persistence debounce** — `last_saved.window = ctx.state.window;` (geometrie okna se mění při každém posunu a spustila by ukládání pořád dokola; ukládá se stejně při každém `saveState` vč. finálního), pak `if (ctx.state != last_saved) save_db.touch();` a po 1 s `saveState`. Porovnává se **celý** `GuiState` přes `operator== = default`.
6. **Resonance Layer** — změna → `layer_db.retouch()`, po 400 ms ticha `engine.rebuildResonanceCache()`.
7. OpenGL render + `glfwSwapBuffers`.

---

## `app_context.h` / `app_context.cpp`

### `PanelState`

Stav panelů, který musí přežít mezi framy. Dřív to byly function-local `static` proměnné přímo v render funkcích — skrytý globální stav, který nešlo otestovat ani resetovat.

| Pole | Účel |
|---|---|
| `midi_ports`, `midi_ports_scanned`, `midi_combo_open` | Cache MIDI portů. `listPorts()` konstruuje RtMidi klienta (OS IPC) a per-frame volání bývalo nejdražší operace celého GUI. Rescan jen při prvním frame, otevření comba a tlačítkem RESCAN. |
| `bank_cands`, `bank_cands_root`, `bank_cands_valid`, `bank_combo_open` | Cache kandidátů na banku. Rescan při změně rootu **a při otevření comba**. |
| `h_voices`, `h_reso`, `h_main_rings`, `h_reso_rings`, `h_load` | Sample-and-hold (max za 400ms okno) pro číselné dlaždice — jinak by čísla při 60 fps nečitelně blikala. |
| `log_scratch` | Předalokovaný buffer pro snapshot LOG stripu (50 × `LogEntry`), aby se nealokoval každý frame. |

### Metody

| Funkce | Vlákno | Vysvětlení |
|---|---|---|
| `initFromState(const GuiState&) → bool` | GUI | Viz níže. |
| `shutdown()` | GUI | Pořadí: (0) **join `reload_thread_`** — běžící reload nelze přerušit, musí doběhnout před zavřením MIDI/audia (quiesce handshake využívá běžící audio); (1) MIDI close; (2) audio stop; (3) clear subscribers (žádné log eventy nesáhnou na `log_buf` po jeho destrukci). |
| `requestBankReload(const std::string&)` | GUI | CAS guard `reload_in_progress_` — druhé volání během běhu je no-op. Joinne doběhlý předchozí thread, vyresetuje `load_progress_` (vč. `license_invalid`), zapíše `reload_dir_` **před** spawnem a spustí worker s `engine.reloadBank(dir, &load_progress_)`. |
| `pollReloadCompletion()` | GUI (per frame) | `reload_done_pending_.exchange(false)` — zpracuje se právě jednou. Převezme `truncated` → `bank_truncated_` (badge v BANK panelu) a `license_invalid` → `bank_license_invalid_` (overlay). Při úspěchu aplikuje **layer heuristiku „1/3 rozsahu banky"** (jen když `state.resonance_layer_db == -30`, tj. uživatel ještě nenastavil) — změna hodnoty spustí 400ms layer debounce → `rebuildResonanceCache`. |
| `setAudioBlockSize(int)` | GUI | `audio->stop()` **první** (joinne miniaudio callback), pak `engine.setBlockSize()`, pak restart. Jinak by re-prepare běžel souběžně s in-flight `processBlock` = data race. |
| `bankLicenseInvalid()` / `clearBankLicenseInvalid()` | GUI | Poslední load licencované banky selhal (license/MAC) → overlay drží varování dokud uživatel neklikne. |

#### `initFromState` — pořadí

1. `Logger::setMinSeverity` z `state.log_level` — **před** `engine.init()`, aby i bank-load logy ctily zvolenou úroveň.
2. `Logger::addSubscriber` (lambda zachycuje `this` — `AppContext` musí přežít do `shutdown()`).
3. `engineConfigFromState(state)` → `engine.init(cfg)`. Při selhání vrací `false` — jediná hard-failure cesta.
4. `applyDspStateToChain(state, engine.dspChain())` — až po `init()` kvůli `choiceCount` (viz výše).
5. `applyStateToEngine(engine, state)`.
6. `audio->start(...)` — po `engine.init()`. Selhání = warning (GUI funguje bez zvuku).
7. MIDI otevření: substring match nad `listPorts()`, `setChannel` **před** `open()`.
8. `requestBankReload(state.bank_path)` — startovní load jede stejnou async cestou jako runtime reload.

---

## `persistence.h` / `persistence.cpp` (schema v5)

`GuiState` je rozdělen: skalární pole + `std::map<std::string, DspStageState> dsp` + `WindowGeom window`. Má `operator== = default`, který používá persistence debounce.

| Funkce | Vysvětlení |
|---|---|
| `parseFlatJson(const std::string&) → map<string,string>` | Rozparsuje celý flat JSON na klíč → raw hodnota v **jednom průchodu** (dřív ~40 opakovaných `find()`). Hlavní důvod: umí klíče **vyjmenovat**, což je nutné pro generické `dsp.*` — jejich jména persistence dopředu nezná, pochází z `Param::id`. |
| `loadState(path) → optional<GuiState>` | Akceptuje schema **3, 4 i 5** (jinak `nullopt` — jediná cesta, jak zahodit celý stav). Ostatní pole se čtou defenzivně (`readF`/`readB`/`readI` s `try/catch`): jeden vadný klíč nezahodí celý stav. Sanitizace: `midi_channel` clamp `[-1, 15]`, `window.w < 320` → 1280, `window.h < 240` → 720. Po načtení vždy `schema_version = 5`. |
| `saveState(path, s) → bool` | Zapíše do `path + ".tmp"`, `flush()` + kontrola `good()` (při IO chybě smaže torzo a vrátí `false`, aby se dobrý config nikdy nepřepsal), pak `rename` (atomický na POSIX). |

### Formát DSP klíčů

```
"dsp.CONVOLVER.enabled": true,
"dsp.CONVOLVER.choice": 1,
"dsp.CONVOLVER.mix": 0.4,
"dsp.AGC.target_rms": 0.164824,
```

Čárka se píše **před** každý řádek, takže prázdná mapa nenechá visící čárku. `choice` se zapisuje jen když `>= 0` (stage bez voliče ho nemá).

### Migrace v3/v4 → v5

Tabulka `kMigF` (float) a `kMigB` (bool) mapuje staré ploché klíče na `dsp.<STAGE>.<Param::id>`. Klíče odpovídají `Param::id` v jednotlivých stage:

| Stage | `Param::id` | Starý plochý klíč | Legacy (v3) |
|---|---|---|---|
| CONVOLVER | `mix`, `decay`, `tone`, `size` | `convolver_*` | — |
| AGC | `target_rms`, `release_ms`, `gain_floor` | `agc_target`, `agc_release_ms`, `agc_floor` | — |
| ENHANCER | `process`, `contour`, `mid` | `enhancer_*` | `bbe_definition`, `bbe_bass`, `bbe_enabled` |
| LIMITER | `threshold_db`, `release_ms` | `limiter_*` | — |

Migrace je jednosměrná: soubor se při příštím `saveState` uloží jako v5 a starší binárka ho odmítne (spadne na defaulty).

---

## `theme.h`

| Entita | Vysvětlení |
|---|---|
| `Colors` | 9 barevných tokenů: `bg`, `bg_panel`, `ink`, `silver`, `silver2`, `line`, `line_soft`, `muted`, `gold` (jediný akcent). `Colors::v(c)` konvertuje `ImU32` (`IM_COL32`) na `ImVec4`. |
| `Fonts` | 4 statické ukazatele: `body` (18 px), `eyebrow` (11 px + 1.5 tracking), `value` (34 px stat čísla), `brand` (20 px logo + 6 tracking). |
| `load_fonts(scale)` | Rasterizuje ve **fyzickém** rozlišení (`size × g_scale`); `main()` pak nastaví `io.FontGlobalScale = 1/g_scale` → ostré fonty v logické velikosti. Všechny čtyři velikosti jedou z téhož zabudovaného blobu. Idempotentní. |
| `apply_theme()` | Nulové rounding (ostré rohy Art Deco), `WindowPadding={0,0}`, `FramePadding={8,4}`, `ItemSpacing={10,4}`, 16 `ImGuiCol_*` tokenů. |

### Zabudovaný font

`ithaca-gui` za běhu **nepotřebuje žádný asset** — jedinou externí věcí je jeho `state.json`. Font je zalinkovaný přímo do binárky:

1. CMake postaví ImGui nástroj `binary_to_compressed_c` (vendorovaný v `third-party/imgui/misc/fonts/`).
2. Custom command jím prožene `third-party/cormorant/Cormorant-Medium.ttf` → `${CMAKE_BINARY_DIR}/generated/cormorant_medium.inl` (502 KB → 290 KB komprimovaně). Do gitu se negeneruje nic.
3. `embedded_font.cpp` ten `.inl` includuje a vystavuje přes dvě funkce. Generované pole je `static`, takže smí být includnuté v **právě jednom** translation unitu — jinak by se 290 KB duplikovalo do každého, kdo includne `theme.h`.
4. `load_fonts()` volá `AddFontFromMemoryCompressedTTF`, který si data rozbalí do vlastního bufferu (o vlastnictví paměti se nestaráme).

Dopad na velikost binárky: ~1.09 → ~1.37 MB.

> **Cross-compilace:** nástroj se staví pro *host*. Při cross-buildu je potřeba použít už vygenerovaný `.inl` nebo nástroj postavit zvlášť nativně.
>
> **Licence:** Cormorant je pod SIL Open Font License (`third-party/cormorant/OFL.txt`). OFL zabudování do software výslovně dovoluje; text licence je nutné distribuovat s produktem.

---

## `layout.h`

Všechny konstanty jsou `inline constexpr float` v `ithaca::gui::layout::Dims`, v **logických px**.

> **DPI se na rozměry neaplikuje.** ImGui pracuje v logických bodech a přepočet na fyzické pixely řeší backend (`io.DisplayFramebufferScale` z GLFW), takže násobit rozměry content-scalem by scale aplikovalo **dvakrát**. `g_scale` slouží výhradně k rasterizaci fontů. Dřív tu žila funkce `S(px)` a sada škálovaných getterů (`padOuter()`, `colBank()`, …) — nikde se nevolaly a jejich použití by právě to dvojí škálování způsobilo. Odstraněny.

| Entita | Hodnota / popis |
|---|---|
| `win_w` / `win_h` | 1280 × 720 (HW cílový display) |
| `col_bank` / `col_dsp` | 250 / 290 px; střední sloupec = flex |
| `topbar_h` / `strip_h` / `kbd_h` / `log_h` | 44 / 100 / 100 / 80 px (`log_h` je minimum, LOG pohltí zbytek) |
| `main_h_max` | 280 px — strop hlavní řady |
| `pad_outer` / `pad_panel` / `pad_inset` | 20 / 20 / 14 px (`pad_inset` = vnitřní odsazení obsahu panelu, dřív duplikované jako lokální `14.f` ve dvou panelech) |
| `row_gap` / `row_gap_s` | 10 / 8 px |
| `slider_h` / `slider_track` / `slider_grab` | 28 / 3 / 12 px |
| `bar_h` / `kbd_keys_h` / `tick_len` / `lamp_gap` | 9 / 56 / 10 / 16 px |
| `tb_midi_w` / `tb_ch_w` / `tb_buffer_w` / `tb_log_w` / `tb_gap` | 210 / 90 / 72 / 120 / 18 px — šířky prvků v top baru |

---

## `widgets.h`

| Funkce | Vysvětlení |
|---|---|
| `Eyebrow(t, col)` | Tlumený prostrkávaný uppercase popisek v `eyebrow` fontu. |
| **`Span` + `TextRow(spans, n, gap)`** | Řádek míchaných fontů na **společné účaří**. Viz níže. |
| `TextRowHeight(spans, n)` | Výška takového řádku (ascent + descent největšího úseku), když si volající potřebuje předpočítat místo. |
| `StatTile(label, value, col, align, margin)` | Dvouřádková dlaždice: eyebrow nahoře, velká hodnota dole. `align` 0/0.5/1. |
| `Lamp(label, on, col)` | `● label` / `○ label` v eyebrow fontu. |
| `ToggleChip(id, on) → bool` | ON/OFF přepínač jako button `● ON` / `○ OFF`. Volající přepíná stav. |
| `DecoSlider(label, v, lo, hi, fmt, col, enabled) → bool` | Ručně kreslený slider: eyebrow label vlevo, hodnota vpravo, tenký track, svislá zarážka. `enabled=false` → ztlumené barvy, `Dummy()` místo `InvisibleButton`. |
| `HBar(frac, w, h, lo, hi, tick)` | Gradient fill bar s volitelnou ryskou (sustain half-pedal práh). |
| `Keyboard<ActiveFn,ResoFn>(w, h, active, reso)` | 88 kláves MIDI 21–108. `active` = zlatá (má přednost), `resonating` = jemně světlejší podklad; u černých kláves výraznější (`kBlackReso`), aby na tmavém podkladu nesplynula. |

### `TextRow` — proč existuje

ImGui skládá položky na řádku podle **horní hrany**. Když na jednom řádku potkají dvě velikosti fontu (eyebrow 11 px „TYPE" + body 18 px „PACKED"), sednou si na různé účaří a text opticky poskakuje. `TextRow` spočítá největší ascent na řádku a každý úsek posadí tak, aby všechny stály na téže účaři:

```cpp
const float k   = ImGui::GetFontSize() / f->FontSize;   // render / raster
const float asc = f->Ascent * k;
dl->AddText({x, o.y + (max_asc - asc)}, span.color, span.text);
```

Kreslí se přes `ImDrawList` (stejně jako `DecoSlider`) a místo se rezervuje jedním `Dummy` o výšce `max_asc + max_desc`. Používá se na řádek TYPE v BANK panelu.

---

## Panely

### `panel_topbar`

`renderTopBar(AppContext&, IParamPage** reset_pages, int n_reset)`.

Zleva doprava: **logo ITHACA** (brand font, `AlignTextToFramePadding` jako ostatní popisky na řádku) → **MIDI IN** dropdown + RESCAN → **CH** (OMNI/1–16) → **SR** (read-only) + **BUFFER** combo → **LOG level** (pravý margin = `Dims::col_dsp`, aby lícoval s CONFIG sloupcem) → **RESET**.

- MIDI port se otevírá **podle jména**, ne podle indexu do cachovaného seznamu: při odpojení zařízení mezi otevřením comba a klikem by index ukazoval jinam a otevřel by se cizí port. Před `open()` se volá `setChannel()` (callback může běžet hned po otevření).
- **RESET** jede genericky přes `Param::def`: `for (i < n_reset) reset_pages[i]->resetToDefaults();`. Rozsah = MASTER + RESONANCE (`kResetPages = 2` v `main.cpp`); DSP chain se záměrně neresetuje — smazání celého řetězce jedním tlačítkem by bylo destruktivní překvapení.

### `panel_bank`

Dropdown kandidátů, TYPE badge (`FIXED`/`DYNAMIC`/`EXTENDED`/`PACKED`/`—`) přes `wdg::TextRow`, statistiky, volitelný badge „NEUPLNA (RAM limit)", RELOAD. Výběr banky → `ctx.requestBankReload(b)` (async). `scanBanks(root)` vrací podadresáře; root se určuje z `state.bank_search_dir`, jinak z rodiče `state.bank_path`.

### `panel_indicators`

`renderIndicatorStrip(ctx, col1_w, col3_w)` — tři sekce: MIDI lampy + sustain bar | 5 dlaždic (VOICES zlatě, RESONANCE, MAIN RINGS, RESO RINGS, DSP LOAD) | peak L/R. Ring dlaždice a DSP LOAD **zčervenají na 4 s** po underrunu resp. overloadu. Všechna čísla jdou přes sample-and-hold (400 ms) ze `ctx.panels`.

### `panel_params`

`renderParamPage(ctx, IParamPage&)` — zcela generický: eyebrow s `name()`, volitelný ON/OFF toggle + volič (`choiceCount()`), smyčka `DecoSlider` přes `paramCount()` (i=0 zlatě, ostatní stříbrně), volitelný metr. Nezná konkrétní stage.

### `panel_config`

`renderConfigPanel(ctx, pages, n, int& selected)` — pro každou stránku kolečko (gold při `enabled()`) + název (gold při `i == selected`) + `InvisibleButton` přes celý řádek.

### `panel_log`

Snapshot 50 nejnovějších eventů do `ctx.panels.log_scratch` (mutex ring bufferu se nedrží během renderu). Barva dle severity: `muted` (Info/Debug), `gold` (Warning), červená (Error/Fatal). Auto-scroll jen když je uživatel u dna.

---

## Křížové odkazy

| Oblast | Co GUI ovládá / čte |
|---|---|
| **Engine** (`engine/engine.h`) | `engineConfigFromState` → `engine.init(cfg)`; `applyStateToEngine` (rezonance, layer, strop polyfonie); bank load async přes `requestBankReload` → `engine.reloadBank(dir, &load_progress_)`. Runtime: `setMasterGain`, `setReleaseMs`, `setResonanceGainDb/LayerDb/Enabled`, `setExciteDecayMs`, `setMaxResonanceVoices`, `bankPeakRmsMinDb/MaxDb`, `rebuildResonanceCache` (debounced 400 ms). Diagnostika: `activeVoices`, `resonanceVoices`, `mainRingsUsed/Total`, `resonanceRingsUsed/Total`, `masterPeakL/R`, `noteOnRecent`, `noteOffRecent`, `pedalCC`, `activeMidiNotes`, `resonatingMidiNotes`, `bankType`, `recordedNotes`, `loadedSamples`, `mainStreamUnderrunRecent`, `resonanceStreamUnderrunRecent`, `dspLoadPeak`, `overloadRecent`. |
| **DSP Chain** (`engine/dsp/dsp_stage.h`, `dsp_chain.h`) | `dsp_state.h` zrcadlí obousměrně přes `stageCount()`/`paramCount()`/`Param::id`. `IParamPage::resetToDefaults()` (virtuální, `ResonancePage` override) pohání RESET. `renderParamPage` + `renderConfigPanel` pracují s `IParamPage*` polymorfně. |
| **Audio** (`io/audio_device.h`) | `audio->start(&audioCallback, &engine, sr, block)`; `setAudioBlockSize` dělá stop → `engine.setBlockSize` → start. |
| **MIDI** (`midi/midi_input.h`) | `MidiInput::listPorts()` (cachovaně v `PanelState`), `midi.open/close`, `midi.setChannel`. |
| **Loader** (`engine.h::reloadBank`) | Async worker; průběh hlásí atomiky `BankLoadProgress` (`phase`/`done`/`total`/`bytes_loaded`/`budget_bytes`/`truncated`/`license_invalid`). Viz [F-loader](F-loader.md). |
| **Logger** (`util/log.h`) | Subscriber → `LogRingBuffer::push`. `main()` spouští background thread pro `flushRTBuffer` (10 ms), takže LOG strip vidí i RT zprávy z audio vlákna. |

---

## Nálezy revize

### Otevřené

Žádné.

### Vyřešené

| # | Nález | Řešení |
|---|---|---|
| 1 | Debounce nesledoval `bank_search_dir` (a při přidání pole se na ně zapomínalo) | `GuiState::operator== = default`; geometrie okna vyřazena explicitně jedním přiřazením |
| 2 | RESET neresetoval `max_resonance_voices` | Generický `resetToDefaults()` přes `Param::def` |
| 3 | `reloadBank` blokoval GUI vlákno | Async loader (2026-06) |
| 4 | Thread-safety čtení DSP stage z GUI vlákna | Vyvráceno — všechny parametry i enable flagy jsou `std::atomic` |
| 5 | `Colors::v()` pořadí kanálů | Ověřeno správné pro `IM_COL32` |
| 6 | Přidání DSP parametru vyžadovalo změnu na 6 místech | Generický `GuiState::dsp` klíčovaný `Param::id` |
| 7 | `9 * ItemSpacing.y` v layout budgetu | Měření `GetContentRegionAvail()` + LOG s výškou 0 |
| 8 | `S()` a škálované gettery v `layout.h` mrtvé a zavádějící | Odstraněny, důvod zdokumentován |
| 9 | Cache kandidátů na banku se invalidovala jen při změně rootu | Rescan i při otevření comba |
| 10 | MIDI port se otevíral podle indexu do cachovaného seznamu | Otevírá se podle jména |
| 11 | `find_asset_path` hledal font jen relativně k CWD | Font zabudován do binárky; `find_asset_path` odstraněn úplně |
| 12 | Mapovací vrstva `GuiState` ↔ Engine byla netestovatelná | `state_binding.h` + `test_gui_state_binding` |
| 13 | Míchané fonty na řádku poskakovaly | `wdg::TextRow` se společnou účařou |
| 14 | `setBlockSize` shodil volič IR Convolveru na 0 | `Convolver::prepare()` volbu zachovává, viz [G-dsp](G-dsp.md) |
| 15 | Neúplné JSON unescape (`\t`, `\r`, `\uXXXX`) a control znaky psané syrově | Plný escape/unescape vč. `\uXXXX` a surrogate párů |
| 16 | `main_h` nezapočítával `ItemSpacing` následujících sekcí | Odečítá se `kItemsBelow * spacing`; čtyřka je ověřitelná z kódu hned pod výpočtem |
