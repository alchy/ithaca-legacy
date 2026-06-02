# GUI

Oblast `app/gui/` implementuje grafické uživatelské rozhraní nástroje **ithaca-gui** nad Dear ImGui (backend GLFW + OpenGL 3.3). Celý životní cyklus aplikace je řízen funkcí `main()`: načti persistovaný `GuiState` (nebo defaults) → otevři GLFW okno (DPI scale z `glfwGetWindowContentScale`) → inicializuj ImGui s Art Deco tématem a fonty Cormorant → inicializuj `AppContext` (engine + audio + MIDI + log subscriber) → spusť pomocný thread pro flush RT log ringu → **render loop** (~vsync, typicky 60 Hz) → finální `saveState` → shutdown v opačném pořadí. Render loop sestavuje plnoobrazovkové kořenové okno `##root` a rozdecompila ho do pevně daných horizontálních pásem: **top bar** (logo + MIDI dropdown + LOG level) → **indicator strip** (MIDI lampy + sustain bar + 5 diagnostických dlaždic vč. DSP LOAD + peak metr L/R) → **hlavní řada 3 sloupců** (BANK 250 px | VOICE/DSP params flex | CONFIG selektor 290 px) → **klaviatura 88 kláves** → **LOG strip** (pohltí zbytek výšky). Aktivní stránka uprostřed se volí přes `ctx.state.config_page` (0 = VOICE, 1–3 = DSP stage): klik v CONFIG panelu přepne index, `renderParamPage` pak genericky nakreslí příslušnou `IParamPage`. Engine je přístupný výhradně skrze `AppContext::engine`; runtime parametry se zapisují buď přes atomické settery (`setMasterGain`, `setResonanceStrength`, …) nebo přes `IParamPage::set()` (které settery volají interně), přičemž přeshraniční čtení diagnostických hodnot z GUI vlákna je bezpečné díky `std::atomic` polím enginu. Stav se ukládá do `state.json` s debounce 1 s (atomicky: zápis do `.tmp` + rename).

---

## Implementováno v souborech

| Soubor | Odpovědnost | Klíčové typy |
|---|---|---|
| `main.cpp` | Vstupní bod: CLI parse, GLFW/ImGui init, DPI scale, render loop, layout shell, persistence debounce, shutdown sekvence | `GuiState`, `AppContext`, `VoicePage`, `IParamPage*[4]` |
| `app_context.h` / `app_context.cpp` | Vlastník všech těžkých objektů (engine, audio device, MIDI, log buffer); inicializace z persistovaného stavu a shutdown | `AppContext`, `audioCallback()` |
| `persistence.h` / `persistence.cpp` | JSON load/save `GuiState` (schema v3→v4 migrace); atomický zápis přes `.tmp` + rename; platformní cesta | `GuiState`, `loadState()`, `saveState()`, `defaultStatePath()` |
| `log_subscriber.h` / `log_subscriber.cpp` | Thread-safe kruhový buffer 256 log eventů; snapshot pro GUI render | `LogRingBuffer` |
| `voice_page.h` | Adaptér `IParamPage` nad engine settery (MASTER, RESONANCE, RELEASE, EXCITE DECAY, MAX RESONANCE) | `VoicePage` |
| `theme.h` | Art Deco barevné tokeny, 4 fonty Cormorant, `apply_theme()`, `load_fonts()`, `find_asset_path()` | `Colors`, `Fonts` |
| `layout.h` | Jediný zdroj pravdy pro všechny rozměry GUI (px konstanty + DPI scale `g_scale`) | `Dims`, `g_scale`, `S()` |
| `widgets.h` | Art Deco widgety kreslené přes `ImDrawList` | `DecoSlider`, `StatTile`, `Keyboard`, `HBar`, `ToggleChip`, `Lamp`, `Eyebrow`, `ParamSliderF` |
| `panel_topbar.{h,cpp}` | Top bar: logo ITHACA, MIDI IN dropdown + RESCAN, CHANNEL (OMNI/1–16), SR (read-only) + BUFFER combo, LOG level, RESET | `renderTopBar()` |
| `panel_bank.{h,cpp}` | Levý sloupec: výběr banky z adresáře, TYPE badge, statistiky, RELOAD | `renderBankPanel()`, `scanBanks()` |
| `panel_indicators.{h,cpp}` | Indicator strip: NOTE/OFF lampy, sustain bar, 5 diagnostických dlaždic (VOICES/RESONANCE/MAIN RINGS/RESO RINGS/DSP LOAD), peak L/R | `renderIndicatorStrip()` |
| `panel_keyboard.{h,cpp}` | 88kláves vizualizace aktivních a rezonujících not | `renderKeyboardPanel()` |
| `panel_params.{h,cpp}` | Generický renderer `IParamPage` (ON/OFF toggle + DecoSlidery + metr) | `renderParamPage()` |
| `panel_config.{h,cpp}` | Pravý sloupec CONFIG: seznam stránek s LED + výběr klikem | `renderConfigPanel()` |
| `panel_log.{h,cpp}` | Log strip: snapshot 50 nejnovějších eventů, auto-scroll, obarvení dle severity | `renderLogPanel()` |

---

## `main.cpp`

### Přehled struktury

`main()` je monolitická funkce, která vlastní celý životní cyklus aplikace. Podrobný popis níže pokrývá části, které nejsou přímočaré.

| Funkce (signatura) | Vlákno | Vstup → výstup | Volá ji | Volá (proč) | Parametry | Vysvětlení |
|---|---|---|---|---|---|---|
| `glfwErrorCb(int err, const char* desc) → void` | GUI | `err`, `desc` | GLFW interně | — | err: kód chyby GLFW | Registrovaný GLFW error callback; loguje na `stderr`. Bez něj by GLFW chyby (např. selhání vytvoření okna) byly tiché. |
| `printUsage(const char* argv0) → void` | GUI | `argv0` | `main()` při `--help` nebo neznámé volbě | `fprintf(stderr, …)` | argv0: název spustitelného souboru | Tiskne nápovědu CLI voleb (`--bank-dir`, `--log-level`, `--help`). |
| `isWindowOnAnyMonitor() → bool` (lambda) | GUI | — | `main()` po `glfwSetWindowPos` | `glfwGetWindowPos`, `glfwGetWindowSize`, `glfwGetMonitors`, `glfwGetMonitorPos`, `glfwGetVideoMode` | — | Počítá překryv okna (minimálně 100×100 px) s každým připojeným monitorem. Chrání před situací, kdy persistovaná pozice okna ukazuje na odpojenou obrazovku. Pokud žádný překryv nevyhovuje, fallback na `(100, 100)` a zapíše novou pozici i do `st`. |
| **Render loop** (`while (!glfwWindowShouldClose(w))`) | GUI | — | — | `glfwPollEvents`, `glfwGetWindowSize/Pos`, `ImGui_ImplOpenGL3_NewFrame`, `ImGui_ImplGlfw_NewFrame`, `ImGui::NewFrame`, render funkce panelů, `saveState` | — | **Viz detailní popis níže.** |
| **log_thr** (lambda background thread) | separátní | `log_run` atomic | `main()` | `Logger::default_().flushRTBuffer()`, `sleep_for(10ms)` | — | Každých 10 ms volá `flushRTBuffer()` — přesouvá zprávy z lock-free RT SPSCu do plného `Logger`. Bez toho by audio thread přetékal RT ring a zprávy se zahazovaly. Před `join()` provede jeden finální flush. |

#### Render loop — podrobně

Každá iterace provede:

1. **Aktualizace `ctx.state.window_*`** — `glfwGetWindowSize/Pos` každý frame, aby layout panelů reagoval na resize uživatelem (drive se aktualizovalo jen při shutdownu).

2. **ImGui frame** — standardní `NewFrame()` → sestavení UI → `Render()`.

3. **Layout výpočet** (vertikální rozpočet):
   - `vfixed` = fixní výška všech pásem + 2× vnější padding + ImGui `ItemSpacing.y` × 9 vkládaných mezer mezi 9 stackovanými sekcemi kořenového okna. Bez odečtení těchto mezer by obsah přesahoval výšku okna o ~`9 × spacing` px a lze jej scrollovat (nežádoucí).
   - `body = H − vfixed` = dostupná výška pro hlavní řadu + log.
   - `main_h = min(main_h_max, body − log_h)` — hlavní řada je zastropena na `280 px`, zbytek pohltí LOG. Pokud by `main_h < 0` (velmi malé okno), fallback `body × 0.5`.

4. **Sestavení 3sloupé hlavní řady** (všechny jako `BeginChild` s `SameLine(0,0)` bez mezery):
   - `##bank` (COL1) → `renderBankPanel(ctx)`
   - `##voice` (flex = `content_w − COL1 − COL3`) → `renderParamPage(ctx, *pages[ctx.state.config_page])`
   - `##config` (COL3) → `renderConfigPanel(ctx, pages, 4, ctx.state.config_page)`

5. **Zrcadlení DSP parametrů do `ctx.state`** — po renderu hlavní řady se hodnoty DSP stage zpětně čtou z enginu (`ch.stage(i).get(j)`, `agc.enabled()` atd.) a ukládají do `ctx.state`. Tak persistence vidí aktuální hodnoty i po přímé změně z panelu (bez samostatného callback mechanismu).

6. **Persistence debounce** — porovnání 20 polí `ctx.state` vs. `last_saved`. Při první detekované změně se zaznamená `dirty_since = now()`. Teprve po 1 s beze změny (nebo spíše 1 s od první změny, `now − dirty_since > 1 s`) se volá `saveState`. `dirty_since` se resetuje. Tím se zabrání ukládání každý frame při tažení slideru.
   - **Sledovaná pole:** `bank_path`, `midi_port_name`, `master_gain_db`, `resonance_strength`, `release_ms`, `excite_decay_ms`, `log_level`, `midi_channel`, `agc_enabled`, `agc_target`, `agc_release_ms`, `agc_floor`, `bbe_enabled`, `bbe_definition`, `bbe_bass`, `limiter_enabled`, `limiter_threshold_db`, `limiter_release_ms`, `config_page`, `max_resonance_voices`, `audio_block_size`. (`audio_sample_rate` se z GUI nemění → není ve sledovaných, ale ukládá se v `saveState` i tak.)
   - **Chybějící pole v debounce kontrole:** `bank_search_dir`, `window_x`, `window_y`, `window_w`, `window_h` — tyto se uloží vždy při shutdownu (finální `saveState`), ale ne přes debounce, viz Nálezy revize.

7. **OpenGL render** — `glViewport`, `glClear(0.1, 0.1, 0.1)`, `RenderDrawData`, `SwapBuffers`.

---

## `app_context.h` / `app_context.cpp`

| Funkce (signatura) | Vlákno | Vstup → výstup | Volá ji | Volá (proč) | Parametry | Vysvětlení |
|---|---|---|---|---|---|---|
| `audioCallback(void* userdata, float* output, uint32_t frames) → void` | audio | userdata=`Engine*`, output=interleaved L/R float buffer, frames | `AudioDevice` (miniaudio) | `engine.processBlock(L, R, frames)` | — | Audio callback s free-function signaturou kompatibilní s `AudioDevice`. Udržuje `static std::vector<float> L, R` (alokuje jen při prvním volání nebo při zvětšení frames). Po `processBlock` interleavuje do výstupního bufferu: `output[2i]=L[i]`, `output[2i+1]=R[i]`. `userdata` = ukazatel na `Engine` předaný při `audio->start()`. |
| `AppContext::initFromState(const GuiState& s) → bool` | GUI | persistovaný `GuiState` → `true` při úspěchu | `main()` před render loop | viz níže | s: stav načtený z JSON nebo default | **Viz detailní popis níže.** |
| `AppContext::shutdown() → void` | GUI | — | `main()` po render loop | `midi.close()`, `audio->stop()`, `Logger::clearSubscribers()` | — | Pořadí shutdown: (1) MIDI close — zastaví MIDI callback thread, žádné nové noty do fronty; (2) audio stop — zastaví miniaudio callback, engine už nikdo nevolá; (3) clear subscribers — žádné log události se nepokusí sahnout na `log_buf` po jeho potenciální destrukci. |

#### `initFromState` — podrobně

Inicializační sekvence (v pořadí volání):

1. **`Logger::setMinSeverity`** — z `state.log_level` (přes `severity_from_string`). Nastaveno *před* `engine.init()`, aby i bank-load logy ctily zvolenou úroveň.
2. **`Logger::addSubscriber`** — lambda `[this](LogEntry e){ log_buf.push(e); }`. Připojeno *před* `engine.init()`, aby GUI strip viděla i init logy (bank load, stream threads, atd.). Logger drží callback by-value; lambda zachycuje `this` — `AppContext` musí přežít do `shutdown()`.
3. **`engine.init(cfg)`** — sestavení `EngineConfig` z `GuiState`: `master_gain = pow(10, gain_db/20)` (dB→linear), `sample_rate = state.audio_sample_rate` (fallback 48000 při ≤ 0), `block_size = clamp(state.audio_block_size, 32, 8192)` (Fáze 8 — dřív napevno 48000/256). Validované hodnoty se promítnou zpět do `state`. **Při selhání vrací `false`** — jediná hard-failure cesta.
4. **Aplikace DSP parametrů** — `ch.stage(0..2).set(i, v)` a `setEnabled(bool)` pro AGC (3 params), BBE (2 params), LIMITER (2 params). Pořadí odpovídá indexům v `Param` tabulkách stage.
5. **`engine.setMaxResonanceVoices`** — explicitně, přestože hodnota byla předána přes `cfg` (exercizuje setter cestu).
6. **Bank load** — `engine.loadBank(state.bank_path)`, jen při neprázdném `bank_path`. Selhání = warning, engine běží prázdný (uživatel vybere banku v UI). Nevrací `false`.
7. **Audio device start** — `audio->start(&audioCallback, &engine, 48000, 256)`. Musí být *po* `engine.init()` (voice pool / stream / ringy jsou připravené). Selhání = warning (GUI stále funguje bez zvuku).
8. **MIDI otevření** — substring match `state.midi_port_name` v `MidiInput::listPorts()`. Při shodě: `midi.open(engine, i)`, `midi.setChannel(state.midi_channel)`, uloží přesný název portu do `state.midi_port_name`. Selhání = warning.

---

## `persistence.h` / `persistence.cpp`

| Funkce (signatura) | Vlákno | Vstup → výstup | Volá ji | Volá (proč) | Parametry | Vysvětlení |
|---|---|---|---|---|---|---|
| `platformConfigDir() → filesystem::path` | GUI | — | `defaultStatePath()` | `getenv("HOME"/"APPDATA"/"XDG_CONFIG_HOME")` | — | Interní helper: vrací OS-specifický konfigurační adresář. macOS: `$HOME/Library/Application Support`; Linux: `$XDG_CONFIG_HOME` nebo `$HOME/.config`; Windows: `%APPDATA%`. Fallback na `current_path()` pokud env proměnná chybí. |
| `jsonEscape(const std::string& s) → std::string` | GUI | řetězec → escaped řetězec | `saveState()` | — | — | Escapuje `"`, `\`, `\n` pro JSON string hodnoty. Ostatní znaky (včetně UTF-8) prochází beze změny. |
| `findValue(const std::string& json, const std::string& key) → std::string` | GUI | flat JSON text + klíč → raw hodnota | `loadState()` | — | json: celý obsah souboru; key: název klíče | Primitivní parser flat JSON (žádné vnořené objekty). Najde `"key"` v textu, přeskočí `:` a whitespace. Pro string hodnoty dekóduje `\"`, `\\`, `\n`. Pro číselné hodnoty čte do `,`/`}`/`\n`. Při chybě (klíč nenalezen) vrací prázdný řetězec — volající rozliší prázdný string od `nullopt`. |
| `defaultStatePath() → filesystem::path` | GUI | — | `main()` | `platformConfigDir()` | — | Vrací `<configDir>/ithaca-legacy/state.json`. |
| `loadState(const filesystem::path& path) → optional<GuiState>` | GUI | cesta k JSON → `GuiState` nebo `nullopt` | `main()` před render loop | `findValue()`, `stof()`, `stoi()` | path: cesta k `state.json` | **Viz detailní popis níže.** |
| `saveState(const filesystem::path& path, const GuiState& s) → bool` | GUI | cesta + `GuiState` → `true` při úspěchu | `main()` (debounce + shutdown) | `create_directories()`, `ofstream`, `filesystem::rename()` | path: cílová cesta; s: stav k uložení | **Viz detailní popis níže.** |

#### `loadState` — podrobně

1. Přečte celý soubor do `std::string`.
2. Zkontroluje `schema_version`: akceptuje **3 nebo 4** (jiné hodnoty nebo chybějící klíč → `nullopt`).
3. Načte povinná pole přítomná v obou verzích: `bank_search_dir`, `bank_path`, `midi_port_name`, `log_level` (default `"info"` při prázdném), `midi_channel` (default `-1`), `master_gain_db`, `resonance_strength`, `release_ms`, `excite_decay_ms`, `max_resonance_voices`, `window_x/y/w/h`.
4. **Schema v4 DSP pole** — načítána obraně pomocí lambda helperů `readF/readB/readI`: při chybějícím klíči (tj. v3 soubor) vrací default ze struktury `GuiState`. Tím je migrace v3→v4 automatická a bezúpadková: DSP stage budou ve výchozím stavu (disabled, default hodnoty).
5. Nakonec nastaví `s.schema_version = 4` — soubor se při příštím `saveState` uloží vždy jako v4.
6. Celý blok je v `try/catch(...)` → při jakékoli výjimce ze `stof`/`stoi` vrací `nullopt` (korupce souboru = začni s defaults).

#### `saveState` — podrobně

1. `create_directories(path.parent_path())` — vytvoří `ithaca-legacy/` při prvním spuštění.
2. Zapíše do `path + ".tmp"` (flat JSON, jeden klíč na řádek, 2 mezery odsazení).
3. `filesystem::rename(tmp, path)` — atomická operace na POSIX systémech; na Windows může selhat přes hranice svazků, ale pro lokální config adresář to nevadí.
4. Vrací `false` při selhání `create_directories` nebo `ofstream`.

---

## `log_subscriber.h` / `log_subscriber.cpp`

| Funkce (signatura) | Vlákno | Vstup → výstup | Volá ji | Volá (proč) | Parametry | Vysvětlení |
|---|---|---|---|---|---|---|
| `LogRingBuffer::push(const LogEntry& e) → void` | libovolný (Logger drží mutex) | nový log event | Logger subscriber callback (z libovolného vlákna) | `std::lock_guard<std::mutex>` | e: log event | Zapíše event na pozici `head_`, inkrementuje `head_ = (head_+1) % kCapacity`, zvyšuje `size_` (max na `kCapacity`). Při přetečení přepíše nejstarší záznam. |
| `LogRingBuffer::snapshot(LogEntry* out, int max_n) → int` | GUI | `max_n` nejnovějších eventů → počet skutečně zkopírovaných | `renderLogPanel()` | `std::lock_guard<std::mutex>` | out: výstupní pole; max_n: max počet | Vypočítá `start = (head_ - min(size_, max_n) + kCapacity) % kCapacity` a kopíruje chronologicky (nejstarší první). Vrátí `n = min(max_n, size_)`. Mutex je držen po celou dobu kopírování — render loop tak nedostane race s `push()`. |
| `LogRingBuffer::size() → int` | libovolný | — → počet platných záznamů (0..256) | diagnostika / UI tooltip | `std::lock_guard<std::mutex>` | — | Vrátí `size_` pod zámkem. |
| `LogRingBuffer::clear() → void` | GUI | — | „Clear" tlačítko (plánováno) | `std::lock_guard<std::mutex>` | — | Resetuje `head_=0`, `size_=0`. Obsah pole záměrně nemaže — stará data jsou nedosažitelná přes `snapshot()` a budou přepsána při dalším `push()`. Šetří alokace `std::string` uvnitř `LogEntry`. |

Kapacita bufferu je `kCapacity = 256`. Producent může být libovolný vlákno (GUI, MIDI, audio přes RT buffer flush). Konzument je vždy GUI vlákno (`snapshot` v `renderLogPanel`). Synchronizace je `std::mutex` (ne lock-free) — `push` je trivializovaný (1 assign + 2 inc) a Logger ho volá pod svým vlastním mutexem, takže celkové zpomalení je zanedbatelné.

---

## `voice_page.h`

| Funkce (signatura) | Vlákno | Vstup → výstup | Volá ji | Volá (proč) | Parametry | Vysvětlení |
|---|---|---|---|---|---|---|
| `VoicePage::name() → const char*` | GUI | — → `"VOICE"` | `renderConfigPanel()`, `renderParamPage()` | — | — | Název stránky pro CONFIG panel a eyebrow nadpis. |
| `VoicePage::paramCount() → int` | GUI | — → 5 | `renderParamPage()` | — | — | 5 parametrů: MASTER dB, RESONANCE, RELEASE, EXCITE DECAY, MAX RESONANCE. |
| `VoicePage::param(int i) → const Param&` | GUI | index → deskriptor parametru | `renderParamPage()` | — | i: 0–4 | Vrátí statický `kParams[i]` s metadaty (label, min, max, fmt, readonly=false pro všechny). |
| `VoicePage::get(int i) → float` | GUI | index → aktuální hodnota z `ctx_.state` | `renderParamPage()` | — | i: 0–4 | Čte přímo z `ctx_.state.*` (ne z engine atomiku) — GUI zobrazuje hodnotu naposledy zapsanou do stavu. Pro i=4 vrátí `(float)ctx_.state.max_resonance_voices`. |
| `VoicePage::set(int i, float v) → void` | GUI | index + nová hodnota | `renderParamPage()` při pohybu slideru | `engine.setMasterGain`, `engine.setResonanceStrength`, `engine.setReleaseMs`, `engine.setExciteDecayMs`, `engine.setMaxResonanceVoices` | i: 0–4; v: hodnota | Klampuje `v` do `[p.min, p.max]`, zapíše do `ctx_.state.*` a okamžitě propaguje do enginu přes příslušný setter. Konverze pro MASTER: `linear = pow(10, v/20)`. Pro MAX RESONANCE: `(int)v` (engine ihned aktualizuje strop polyfonie). |
| `VoicePage::hasEnable() → bool` | GUI | — → `false` | `renderParamPage()` | — | — | VOICE stránka nemá ON/OFF toggle (vždy aktivní). |
| `VoicePage::enabled() → bool` | GUI | — → `true` | `renderConfigPanel()` (LED stav) | — | — | Vždy vrací `true` — VOICE LED svítí zlatě. |
| `VoicePage::setEnabled(bool) → void` | GUI | — | `renderParamPage()` | — | — | No-op. |
| `VoicePage::meter(float&, const char*&) → bool` | GUI | — → `false` | `renderParamPage()` | — | — | VOICE stránka nemá metr (no-op). |

`kParams[5]` jsou `static constexpr` — inicializovány jednou, sdíleny všemi instancemi (je jich vždy právě jedna). Rozsahy: MASTER −60..+6 dB, RESONANCE 0..1, RELEASE 50..2000 ms, EXCITE DECAY 500..30000 ms, MAX RESONANCE 1..64.

---

## `theme.h`

| Funkce / entita (signatura) | Vlákno | Vstup → výstup | Volá ji | Volá (proč) | Parametry | Vysvětlení |
|---|---|---|---|---|---|---|
| `Colors` (struct se static constexpr ImU32) | — | — | Všechny widgety a panely | — | — | 9 barevných tokenů: `bg` (tmavé pozadí), `bg_panel`, `ink` (nejsvětlejší stříbro), `silver`, `silver2` (titulky/peak), `line`, `line_soft`, `muted` (eyebrow/dim), `gold` (primární akcent). `Colors::v(c)` konvertuje `ImU32` na `ImVec4` (pozor: pořadí kanálů RGBA → ABGR, viz poznámka). |
| `Fonts` (struct se static inline ImFont*) | — | — | Všechny widgety | — | — | 4 statické ukazatele: `body` (18 px Cormorant), `eyebrow` (11 px + 1.5 tracking), `value` (34 px stat čísla), `brand` (20 px logo + 6 tracking). Null = ImGui default font. |
| `find_asset_path(const string& rel) → string` | GUI | relativní cesta → absolutní nebo prázdný | `main()` | `ifstream::good()` | rel: např. `"cormorant/Cormorant-Medium.ttf"` | Prohledává CWD, `./third-party/`, `../third-party/`. Vrátí první existující cestu nebo prázdný řetězec. |
| `load_fonts(const string& ttf_path, float scale) → void` | GUI | cesta k TTF + DPI scale | `main()` | `ImFontAtlas::AddFontFromFileTTF()` | ttf_path: cesta k Cormorant; scale: `g_scale` (~2.0 na Retina) | Rasterizuje fonty ve fyzickém rozlišení (`size × scale`); `main()` následně nastaví `io.FontGlobalScale = 1/scale` → fonty jsou ostré, ale zobrazují se v logické velikosti. GlyphExtraSpacing.x pro eyebrow (+1.5×scale) a brand (+6×scale). Při selhání načtení fallback na ImGui default. Idempotentní (guard `if (Fonts::body) return`). |
| `apply_theme() → void` | GUI | — | `main()` | `ImGuiStyle` settery | — | Nastaví `ImGuiStyle`: nulové rounding (ostré rohy Art Deco), `WindowPadding={0,0}` (panely si dělají vlastní padding), `FramePadding={8,4}`, `ItemSpacing={10,4}`, `ScrollbarSize=8`, nulové border size. Nastaví barvy 16 `ImGuiCol_*` tokenů dle `Colors`. |

---

## `layout.h`

| Entita | Popis |
|---|---|
| `g_scale` (`inline float`) | Globální DPI scale, nastavena v `main()` z `glfwGetWindowContentScale()`. Default 1.0 (HiDPI Retina ~2.0). |
| `S(float px) → float` | `px × g_scale` — konverze logického px na fyzický. |
| `Dims::win_w/h` | Výchozí velikost okna při prvním spuštění: 1280×820 px. |
| `Dims::col_bank` / `col_dsp` | Šířky pevných sloupců: 250 px (BANK) / 290 px (CONFIG/DSP). Střední VOICE sloupec = flex (zbytek). |
| `Dims::topbar_h` / `strip_h` / `kbd_h` / `log_h` | Výšky pásem: 44 / 100 / 100 / 80 px. `log_h` je minimum; LOG pohltí zbytek výšky. |
| `Dims::main_h_max` | Strop výšky hlavní řady: 280 px. Omezuje prázdný prostor pod slidery při větším okně. |
| `Dims::pad_outer` / `pad_panel` | Vnější okraj okna: 20 px. Vnitřní padding panelů: 20 px. |
| `Dims::row_gap` / `row_gap_s` | Vertikální mezery: 10 px (mezi pásmy/prvky) / 8 px (label↔control). |
| `Dims::slider_h` / `slider_track` / `slider_grab` | DecoSlider rozměry: 28 px výška řádku, 3 px track, 12 px grab. |
| `Dims::bar_h` / `kbd_keys_h` / `tick_len` / `lamp_gap` | HBar výška: 9 px; klávesy: 56 px; grid ryska: 10 px; mezera MIDI lamp: 16 px. |

Všechny konstanty jsou v namespace `ithaca::gui::layout::Dims` jako `inline constexpr float/int`. Skalovací helpery (`padOuter()`, `padPanel()`, `rowGap()`, atd.) volají `S(Dims::*)` — panely je mohou volat místo přímého `Dims::*`.

---

## `widgets.h`

| Funkce (signatura) | Vlákno | Vstup → výstup | Volá ji | Volá (proč) | Parametry | Vysvětlení |
|---|---|---|---|---|---|---|
| `Eyebrow(const char* t, ImU32 col) → void` | GUI | text + barva | panely, `StatTile`, `ParamSliderF` | `ImGui::PushFont(Fonts::eyebrow)`, `ImGui::TextUnformatted` | t: text; col: default `Colors::muted` | Tlumený prostrkávaný uppercase popisek v `eyebrow` fontu. |
| `StatTile(const char* label, const char* value, ImU32 value_col, float align, float margin) → void` | GUI | popisek + hodnota + zarovnání | `renderIndicatorStrip()` | `ImGui::CalcTextSize`, `ImGui::SetCursorPosX`, `ImGui::PushFont` | label: eyebrow; value: velké číslo; align: 0=vlevo/0.5=střed/1=vpravo; margin: odsazení od okraje | Dvouřádková dlaždice: eyebrow nahoře (muted), velká hodnota dole (`Fonts::value`). Zarovnání obou řádků v rámci dostupné šířky buňky (`GetContentRegionAvail().x`) přes `SetCursorPosX`. margin=0 při `align=0.5` (přesný střed). |
| `Lamp(const char* label, bool on, ImU32 on_col) → void` | GUI | stav → vizuální lampa | `renderIndicatorStrip()` | `ImGui::PushStyleColor`, `ImGui::Text` | label: text; on: stav; on_col: default `Colors::gold` | Vypíše `● label` (svítí) nebo `○ label` (zhasnuto) v `eyebrow` fontu. Unicode: U+25CF / U+25CB jako UTF-8 sekvence. |
| `ToggleChip(const char* id, bool on) → bool` | GUI | stav → `true` při kliknutí | `renderParamPage()` | `ImGui::Button` | id: ImGui ID; on: aktuální stav | ON/OFF přepínač jako button `● ON` / `○ OFF`. Barva textu: gold (on) / muted (off). Průhledné pozadí, hover/active dle téma. Volající je zodpovědný za přepnutí stavu při `true`. |
| `ParamSliderF(const char* label, float* v, float lo, float hi, const char* fmt) → bool` | GUI | label + hodnota → `true` při změně | — (nevyužito, nahrazeno `DecoSlider`) | `Eyebrow()`, `ImGui::SliderFloat` | label, v, lo, hi, fmt | Standardní ImGui slider s eyebrow popiskem. Plná šířka. |
| `DecoSlider(const char* label, float* v, float lo, float hi, const char* fmt, ImU32 grab_col, bool enabled) → bool` | GUI | hodnota + rozsah → `true` při změně + kreslení | `renderParamPage()` | `ImDrawList::AddRectFilled`, `ImGui::InvisibleButton`, `ImGui::GetIO().MousePos` | label: eyebrow vlevo; v: ukazatel na hodnotu; lo/hi: rozsah; fmt: formát; grab_col: barva fill/grab; enabled: false=readonly | **Viz detailní popis níže.** |
| `HBar(float frac01, float width, float h, ImU32 fill_lo, ImU32 fill_hi, float tick01) → void` | GUI | frakce 0–1 → vodorovný bar | `renderIndicatorStrip()` | `ImDrawList::AddRectFilled`, `AddRectFilledMultiColor`, `AddLine` | frac01: výplň; fill_lo/hi: gradient; tick01: ryska (-1 = žádná) | Gradient fill bar s volitelnou svislou ryskou. width≤0 → plná dostupná šířka. Pozadí `Colors::line_soft`. Gradient: `AddRectFilledMultiColor` s fill_lo vlevo a fill_hi vpravo. |
| `Keyboard<ActiveFn,ResoFn>(float width, float height, ActiveFn active, ResoFn resonating) → void` | GUI | rozměry + 2 predikáty MIDI→bool → kreslení | `renderKeyboardPanel()` | `ImDrawList::AddRectFilled`, `AddLine` | width, height: rozměry; active(m): zlatá klávesa; resonating(m): tlumená klávesa | **Viz detailní popis níže.** |

#### `DecoSlider` — podrobně

Nekombinuje standardní ImGui slider, ale kreslí celý widget ručně přes `ImDrawList`:

- **Label (vlevo) + hodnota (vpravo)** — v `eyebrow` fontu na horním okraji řádku (výška `Dims::slider_h = 28 px`).
- **Track** — tenká linka (`Dims::slider_track = 3 px`) ve spodní třetině řádku; pozadí `Colors::line_soft`, vyplněná část po grab pozici barvou `fill_col`.
- **Grab** — svislá zárazka `3 px × 12 px` (`Dims::slider_grab`) na pozici `gx = o.x + width × t` kde `t = (v − lo) / (hi − lo)`.
- **Interakce** — `ImGui::InvisibleButton` přes celý řádek. Při `IsItemActive()` čte `MousePos.x`, přepočítá `t` a nastaví `*v`. Vrátí `true` při změně hodnoty.
- **Readonly** (`enabled=false`) — ztlumené barvy (`Colors::line` fill, `Colors::muted` text), `Dummy()` místo `InvisibleButton`, vrátí `false`. Používá se pro `MAX RESONANCE` (init-only parametr).
- **DPI** — používá `Dims::*` konstanty (logické px); DPI škálování řeší ImGui/GLFW na úrovni framebufferu, nikoli widget.

#### `Keyboard` — podrobně

Template funkce (ActiveFn, ResoFn = lambda `int→bool`). Kreslí 88 kláves MIDI 21–108:

- Nejprve spočítá počet bílých kláves (`n_white`), šířku bílé klávesy `kw = width / n_white`, šířku černé `bw = kw × 0.6`, výšku černé `bh = height × 0.62`.
- **Průchod 1 (bílé klávesy):** pro každou nebílou (ne-black) notu. Barva: `Colors::gold` pokud `active(m)`, `kWhiteReso` (o málo světlejší než pozadí, `0x2a,0x30,0x36`) pokud `resonating(m)`, jinak `kWhiteBase` (`0x1c,0x20,0x24`). Dělicí svislá čára `Colors::line` tloušťka 0.5.
- **Průchod 2 (černé klávesy):** kresleny přes bílé. Pozice `kx = (wi-1) × kw + kw − bw/2`. Barva: zlatavě tmavá (`0x8a,0x73,0x30`) pro active, `kBlackReso` (`0x3c,0x44,0x4c`) pro rezonující, `Colors::bg` jinak. Rezonanční šedá černé klávesy je **výraznější** než u bílé (`kWhiteReso 0x2a3036`) — černá klávesa je sama tmavá, takže by jinak na tmavém podkladu splynula.
- `active` má přednost: klávesa rezonující i aktivní bude zlatá.

---

## `panel_topbar.h` / `panel_topbar.cpp`

`renderTopBar(AppContext& ctx)` — GUI vlákno — kreslí do `##topbar` child okna (celá šířka, ~44 px).

Obsah zleva doprava:
1. **Logo ITHACA** — `Fonts::brand`, `Colors::gold`.
2. **MIDI IN dropdown** (`ImGui::BeginCombo("##midi")`) — zobrazuje `listPorts()` každý frame (live scan). Výběr `(none)` zavře port (`midi.close()`, smaže `state.midi_port_name`). Výběr portu: `midi.close()` + `midi.open(engine, i)` + `midi.setChannel(state.midi_channel)`, uloží přesný název. Tlačítko `RESCAN` je vizuální hook (listPorts se volá každý frame, tlačítko zatím nic extra neprovádí).
3. **CH dropdown** (`ImGui::BeginCombo("##ch")`) — OMNI nebo 1–16. Při změně: `state.midi_channel = c`, `midi.setChannel(c)`.
4. **SR | BUFFER skupina** (Fáze 8, mezi CH a LOG) — viz oblast C (Buffery):
   - **`SR`** read-only label = `engine.sampleRate()` formátovaný (`%g kHz`). Konfiguruje se jen v `state.json` (`audio_sample_rate`), GUI ho jen zobrazuje.
   - **`BUFFER`** combo `{32…8192}` framů (`##buffer`) — při výběru `ctx.setAudioBlockSize(v)` (stop→setBlockSize→start, persist). Jen počet framů (ms latence vynechána kvůli místu na liště).
   - (DSP load metr je v indicator stripu jako 5. dlaždice, ne zde. MIDI IN combo zkráceno na 210 px.)
4. **LOG level combo** (vpravo, pevný right margin 290 px) — 6 úrovní `debug/info/warn/error/fatal/off`. Při změně: `state.log_level = kLevels[cur]`, `Logger::setMinSeverity(…)`.
5. **RESET tlačítko** — resetuje `state.resonance_strength = 0.5`, `release_ms = 200`, `excite_decay_ms = 5000`, `master_gain_db = 0` a volá příslušné engine settery (`setResonanceStrength`, `setReleaseMs`, `setExciteDecayMs`, `setMasterGain(1.f)`).

Chybí reset `max_resonance_voices` v RESET akci — viz Nálezy revize.

---

## `panel_bank.h` / `panel_bank.cpp`

`renderBankPanel(AppContext& ctx)` — GUI vlákno — kreslí do `##bank` child okna (levý sloupec, 250 px).

| Funkce (signatura) | Vlákno | Vstup → výstup | Volá ji | Volá (proč) | Parametry | Vysvětlení |
|---|---|---|---|---|---|---|
| `scanBanks(const string& search_root) → vector<string>` | GUI | adresář → podadresáře jako kandidáti | `renderBankPanel()` | `filesystem::directory_iterator` | search_root: adresář bank | Vrátí seznam podadresářů `search_root`. Prázdný root → prázdný výstup. Chybný/neexistující adresář → prázdný výstup (error_code). Výsledek cachován staticky (`static cands`, `static last_root`) — rescan jen při změně rootu. |
| `renderBankPanel(AppContext& ctx) → void` | GUI | — | `main()` render loop | `scanBanks()`, `engine.reloadBank()`, `engine.bankType()`, `engine.recordedNotes()`, `engine.loadedSamples()` | — | Vykreslí: eyebrow BANK, dropdown se seznamem kandidátů (filename jako label), TYPE badge (`FIXED`/`DYNAMIC`/`EXTENDED`/`—` ze `engine.bankType()`), statistiky (`X not · Y samplu`), RELOAD tlačítko. Výběr jiné banky: `state.bank_path = b`, `engine.reloadBank(b)` (safe drain→silence→load→resume, blokuje ~60 ms na GUI vlákně). RELOAD tlačítko volá `engine.reloadBank(state.bank_path)`. |

`root` pro scan se určuje prioritně z `state.bank_search_dir`, pak z rodiče `state.bank_path`, pak prázdný string.

---

## `panel_indicators.h` / `panel_indicators.cpp`

`renderIndicatorStrip(AppContext& ctx, float col1_w, float col3_w)` — GUI vlákno — kreslí do `##strip` child okna (plná šířka, 100 px). Přijímá `col1_w` a `col3_w` z `main.cpp` pro zachování zarovnání se sloupci hlavní řady.

Tři sekce vedle sebe (`SameLine(0,0)`):

1. **col1 (MIDI lampy + sustain)** — `##ind_midi`:
   - `wdg::Lamp("ON", engine.noteOnRecent(120ms), gold)` + `Lamp("OFF", engine.noteOffRecent(120ms), silver)` — blikání 120 ms.
   - `wdg::HBar(pedalCC/127, …, tick01=0.5)` — sustain bar s ryskou na 50 % (half-pedal práh). Popisek `"SUSTAIN  CC"`.

2. **center (5 diagnostických dlaždic)** — `##ind_diag`, šířka `content_w − col3_w`, každá `fifth = center_w/5`:
   - `##t_v`: `StatTile("VOICES", activeVoices, gold, align=0, margin=pad)` — zlaté číslo aktivních hlasů.
   - `##t_r`: `StatTile("RESONANCE", resonanceVoices, silver, align=0.5)` — stříbrné, střed.
   - `##t_m`: `StatTile("MAIN RINGS", mainRingsUsed/Total, main_ur ? ring_red : silver, 0.5)` — zčervená při underrunu > 4 s.
   - `##t_g`: `StatTile("RESO RINGS", resonanceRingsUsed/Total, res_ur ? ring_red : silver, 0.5)`.
   - `##t_d`: `StatTile("DSP LOAD", dspLoadPeak*100 %, overload ? ring_red : silver, 1.f)` — vpravo; zčervená na 4 s po overloadu (`overloadRecent(4000)`, load ≥ 1.0 = blok minul deadline). Viz oblast C (Buffery).

3. **col3 (peak L/R)** — `##ind_peak`:
   - Dvě `HBar`: L a R výstupní peak, převedeny `lin → dB → 0..1` (rozsah −60..0 dB, `dbTo01(toDb(...))`). Gradient `silver2 → ink`.

---

## `panel_keyboard.h` / `panel_keyboard.cpp`

`renderKeyboardPanel(AppContext& ctx)` — GUI vlákno — kreslí do `##kbd` child okna (plná šířka, 100 px).

Načte `engine.activeMidiNotes(active[128])` a `engine.resonatingMidiNotes(reso[128])`, poté zavolá `wdg::Keyboard(w, Dims::kbd_keys_h, [active], [reso])`. Výška kláves = `Dims::kbd_keys_h = 56 px` (zbytek do 100 px = padding/dummy). Žádný textový popisek — rozsah a sustain jsou v indicator stripu.

---

## `panel_params.h` / `panel_params.cpp`

`renderParamPage(AppContext& ctx, IParamPage& page)` — GUI vlákno — generický renderer libovolné `IParamPage`.

| Krok | Podmínka | Akce |
|---|---|---|
| Nadpis | vždy | `wdg::Eyebrow(page.name(), silver2)` |
| ON/OFF toggle | `page.hasEnable()` | `wdg::ToggleChip(name, enabled)` → při kliknutí `page.setEnabled(!enabled)` |
| Smyčka sliderů | vždy | pro `i = 0..paramCount()-1`: `page.get(i)` → `wdg::DecoSlider(p.label, &v, p.min, p.max, p.fmt, accent, !p.readonly)`. Barva: i=0 → `gold`, ostatní → `silver2`. Při změně: `page.set(i, v)`. |
| Metr | `page.meter(mv, ml)` | `wdg::Eyebrow(ml)` + formátovaná hodnota `Colors::ink` |

Zcela generický — nezná konkrétní stage. Funguje pro `VoicePage` (5 params, hasEnable=false) i pro `DspStage` (proměnný počet, hasEnable=true). Parametr `ctx` je přijat ale nevyužit (`(void)ctx`) — připravenost pro budoucí kontextové akce.

---

## `panel_config.h` / `panel_config.cpp`

`renderConfigPanel(AppContext& ctx, IParamPage** pages, int n, int& selected)` — GUI vlákno — pravý sloupec CONFIG (290 px).

Pro každou stránku `pages[i]`:
- Nakreslí kruháč `AddCircleFilled(r=3.5, gold nebo line)` dle `p->enabled()`.
- Nakreslí název `p->name()` — zlatý při `i == selected`, jinak `ink`.
- `ImGui::InvisibleButton` přes celý řádek (šířka = `content_w − pad_panel`, výška 20 px) → při kliknutí `selected = i`.

Parametr `ctx` přijat ale nevyužit (`(void)ctx`). `selected` je `int&` — in/out: vstup pro zobrazení aktuálního výběru, výstup při kliknutí. `main.cpp` předává `ctx.state.config_page` přímo (reference).

---

## `panel_log.h` / `panel_log.cpp`

`renderLogPanel(AppContext& ctx)` — GUI vlákno — kreslí do `##log` child okna (plná šířka, zbytek výšky po hlavní řadě a klaviatuře).

- Snapshot 50 nejnovějších eventů do statického `std::array<LogEntry, 50> tmp` (na stacku procesu/staticky, nedochází k heap alokaci každý frame).
- `BeginChild("##loglist", {0,0}, false, HorizontalScrollbar)` — 0,0 = vyplň celou dostupnou oblast.
- Pro každý event: barva dle severity — `muted` (Info/Debug), `gold` (Warning), červená `0xd0,0x5a,0x4a` (Error/Fatal). `ImGui::Text("[%s] %s", topic, message)`.
- **Auto-scroll:** `if (ScrollY >= ScrollMaxY - 1) SetScrollHereY(1.0)` — scrolluje k nejnovějšímu záznamu pouze pokud je uživatel u dna. Při ručním scrollování nahoru se nesnáží „uchytit" nové zprávy.

---

## Křížové odkazy

| Oblast | Co GUI ovládá / čte |
|---|---|
| **Engine** (`engine/engine.h`) | `initFromState`: `engine.init(cfg)`, `loadBank`, `setMaxResonanceVoices`. Runtime: `setMasterGain`, `setResonanceStrength`, `setReleaseMs`, `setExciteDecayMs` (přes `VoicePage::set`), `dspChain().stage(i).set/setEnabled` (přes `renderParamPage`). Diagnostika: `activeVoices`, `resonanceVoices`, `mainRingsUsed/Total`, `resonanceRingsUsed/Total`, `masterPeakL/R`, `noteOnRecent`, `noteOffRecent`, `pedalCC`, `activeMidiNotes`, `resonatingMidiNotes`, `bankType`, `recordedNotes`, `loadedSamples`, `mainStreamUnderrunRecent`, `resonanceStreamUnderrunRecent`. |
| **DSP Chain** (`engine/dsp/dsp_stage.h`, `dsp_chain.h`) | `renderParamPage` + `renderConfigPanel` pracují s `IParamPage*` (polymorfní). `main.cpp` získá reference `dspChain().stage(0..2)` a zrcadlí hodnoty do `ctx.state` každý frame. |
| **Audio** (`io/audio_device.h`) | `AppContext::initFromState` volá `audio->start(&audioCallback, &engine, 48000, 256)`. `shutdown` volá `audio->stop()`. GUI jinak s audio device nekomunikuje. |
| **MIDI** (`midi/midi_input.h`) | `renderTopBar` volá `MidiInput::listPorts()`, `midi.open/close`, `midi.setChannel`. `AppContext::initFromState` otevírá port dle persistovaného jména. |
| **Loader / Bank reload** (`engine/engine.h::reloadBank`) | `renderBankPanel` volá `engine.reloadBank(path)` — blokující (~60 ms) GUI-thread safe operace (drain → silence → load → resume). |
| **Logger** (`util/log.h`) | `AppContext::initFromState` registruje subscriber → `LogRingBuffer::push`. `main()` spouští background thread pro `flushRTBuffer` (10 ms interval). `renderTopBar` volá `Logger::setMinSeverity` při změně úrovně. |
| **Events (MIDI/CC)** | Engine drží lock-free `MidiQueue`; MIDI thread vkládá `noteOn/noteOff/sustainPedal` — GUI tato data čte přes atomické diagnostické metody (ne přímý přístup do fronty). |

---

## Nálezy revize

### 1. Debounce nesleduje `bank_search_dir` a `window_*`

Porovnávací blok v render loop kontroluje 20 polí `GuiState`, ale **vynechává** `bank_search_dir`, `window_x`, `window_y`, `window_w`, `window_h`. Změna `bank_search_dir` (přes CLI nebo budoucí Browse dialog) se tedy uloží až při příštím shutdownu nebo při jiné sledované změně. `window_*` se mění každý frame (z `glfwGetWindowSize/Pos`), takže jejich **zařazení do debounce by triggrovalo ukládání neustále** při jakémkoli pohybu okna — zřejmě záměrné (ukládají se jen při shutdownu). `bank_search_dir` by zařadit šlo, bez rizika záplavy ukládání.

### 2. RESET netleží `max_resonance_voices`

`renderTopBar` → RESET tlačítko resetuje `resonance_strength`, `release_ms`, `excite_decay_ms`, `master_gain_db` na defaults a volá odpovídající engine settery. **Neresetuje** `max_resonance_voices` (default 32). Pokud uživatel sníží MAX RESONANCE na 1 a klikne RESET, hodnota zůstane na 1. Nekonzistentnost s ostatními VOICE parametry.

### 3. `reloadBank` blokuje GUI vlákno ~60 ms

`engine.reloadBank()` (z `renderBankPanel` i `main.cpp`) volá `std::this_thread::sleep_for` celkem ~60 ms v GUI vlákně. Při ~60 Hz render loopu způsobí viditelné zaseknutí (přeskočí ~4 framy). Žádný workaround není implementován — reload by měl být delegován do pomocného threadu s indikátorem průběhu.

### 4. Thread-safety čtení DSP stage z GUI vlákna

Po renderu hlavní řady `main.cpp` čte `ch.stage(i).get(j)` a `agc.enabled()` z GUI vlákna — tyto hodnoty mohly být právě zapisovány audio threadem přes `process()`. `DspStage` implementace by měla parametry udržovat atomicky (nebo pod mutexem). Bez inspekce `dsp_chain.cpp` / konkrétních stage implementací nelze potvrdit, zda jsou settery/gettery skutečně atomické — potenciální race condition.

### 5. `Colors::v()` — pořadí kanálů

`Colors::v(ImU32 c)` konvertuje `ImU32` (formát `0xAABBGGRR` v ImGui) na `ImVec4(R, G, B, A)`, ale extrahuje kanály jako `c & 0xFF` (byte 0 = R), `(c >> 8) & 0xFF` (byte 1 = G), `(c >> 16) & 0xFF` (byte 2 = B), `(c >> 24) & 0xFF` (byte 3 = A). To je správné pro `IM_COL32(R,G,B,A)` makro. Výsledek je `ImVec4(R/255, G/255, B/255, A/255)` — konzistentní s ImGui konvencí.

### 6. `findValue` — neúplné unescape

`findValue` při parsování string hodnot decoduje pouze `\n` a `\\` + obecný `\x → x`. Ostatní JSON escape sekvence (`\t`, `\r`, `\uXXXX`) jsou nekorektně dekoduji jako literální druhý znak. Pro aktuální obsah `state.json` (cesty k adresářům, port jména, klíčová slova) to nevadí, ale není to plně spec-kompatibilní JSON parser.
