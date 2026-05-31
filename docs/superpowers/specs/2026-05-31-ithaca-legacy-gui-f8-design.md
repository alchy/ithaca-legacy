# Faze 8: GUI (Hybrid MVP) — Design Spec

> Hybrid scope: minimalisticky user-facing top bar (bank/MIDI/master) +
> 88-key keyboard vizualizace + diagnosticky panel + slidery pro
> runtime parametrizaci. Cilem je rychla iterace pri ladeni chovani
> playeru a sledovani metricy pod zatezi (pedal, rezonance, ring pool).

---

## 1. Cile

1. **Rychla parametrizace** — slidery pro `resonance_strength`, `release_ms`,
   `excite_decay_ms`, `max_resonance_voices`, `master_gain` s okamzitym
   efektem (bez restartu playeru).
2. **Sledovani chovani** — live diagnostiky: voice count, resonance count,
   ring pool usage, pedal CC64, master peak meter, aktivni MIDI noty.
3. **Pohodlne testovani** — bank/MIDI port selektor v dropdownu, persistence
   nastaveni pri exitu, aby se nemuselo znovu cvakat po restartu.
4. **Cross-platform** — macOS, Linux (vc. Rpi 4 GB), Windows; vsechny
   knihovny vendorovane stejnym vzorem jako miniaudio/rtmidi.

Mimo MVP: VST wrapper, audio device selector, block size selector v UI,
CPU load meter, polished Keyscape-style koncovy UI (to je dale).

---

## 2. Architektura

### 2.1 Build target

Novy exekutabl `ithaca-gui`, linkuje stavajici `libithaca_core` (statickou
knihovnu). CLI `ithaca-cli` zustava pro headless / scripted / render pouziti
beze zmen.

```
libithaca_core (existuje)
   |
   +-- ithaca-cli (existuje, headless)
   +-- ithaca-gui (NOVY)
```

### 2.2 Vendorovane knihovny (`third-party/`)

- **Dear ImGui** v1.91+ — immediate-mode UI, single-header + cpp files
  - core: `imgui.cpp`, `imgui_draw.cpp`, `imgui_tables.cpp`, `imgui_widgets.cpp`
  - backend: `imgui_impl_glfw.cpp`, `imgui_impl_opengl3.cpp`
- **GLFW** 3.4+ — window/input cross-platform
  - macOS: link `-framework Cocoa -framework IOKit -framework CoreVideo`
  - Linux: `libGL`, `pthread`, `dl`, `xrandr`, `xinerama`, `xcursor`, `xi`
  - Windows: `gdi32`, `opengl32`

### 2.3 Vlakna

Stejny model jako `ithaca-cli`:

- **Main thread** — GLFW window event loop, ImGui render (~60 fps), draw call OpenGL
- **Audio thread** — miniaudio callback, `Engine::processBlock`
- **MIDI thread** — RtMidi callback, `Engine::noteOn/Off/sustainPedal/...`
  (vola lock-free MidiQueue)

GUI komunikuje s Engine cestou:
- **GUI → Engine (setter)**: primym volanim atomic-store setteru
  (`setMasterGain`, `setResonanceStrength`, ...) z main threadu.
- **Engine → GUI (live read)**: atomic-load gettery z main threadu kazdy
  render frame. Zadne mutexy na audio threadu.

---

## 3. Layout

Single window, default 1024×768, resizable. Vertikalni rozvrzeni
podle schvalene varianty A:

```
+----------------------------------------------+
| TOP BAR (height ~32px)                       |
|  [Bank: as-blackgrand ▼] [MIDI: IAC Bus 1 ▼] |
|  ____________________________ [Master ━●━━ ] |
+----------------------------------------------+
| KEYBOARD VIZ (height ~140px)                 |
|  88-key piano keyboard                       |
|  Aktivni noty: bila/cerna klavesa zvyraznena |
|  Intenzita = aktualni gain (alpha 0..1)      |
|  Pedal indikator: ramecek pod keyboardem     |
|    (zeleny=CC0, zluty=CC64, cerveny=CC127)   |
+--------------------+-------------------------+
| DIAG PANEL (left)  | PARAMS PANEL (right)    |
| (~360px width)     | (~360px width)          |
|                    |                         |
|  Voices  6/256     |  Resonance ━●━━━ 0.50   |
|  Resonance 18/32   |  Release ms ━━●━ 200    |
|  Rings   24/288    |  Excite decay ━●━ 5000  |
|  Pedal   CC64=64   |  Max reson  ━━●━ 32     |
|                    |  Master gain ●━━ -6 dB  |
|  Master meter:     |                         |
|   L ━━━━●━ -8 dB   |  [ Reset to defaults ]  |
|   R ━━━━●━ -7 dB   |                         |
+--------------------+-------------------------+
| LOG STRIP (height ~80px, scrollable)         |
|  21:00:49.787  voice_end midi=62 ...         |
|  21:00:49.638  voice_on midi=50 slot=0 ...   |
|  ...                                         |
+----------------------------------------------+
```

### 3.1 Top bar (komponenty)

- **Bank dropdown** — vychazi z `bank_search_path` (config) nebo z
  command-line argumentu `--bank-dir <path>`. Scanuje podadresare,
  default sample bank z aktualniho `bank_path`. Pri zmene → reload (viz 4.2).
- **MIDI dropdown** — `MidiInput::listPorts()` enumeruje vsechny porty.
  Pri zmene → close current + open new. Idle stav (zadny port) dovolen
  (pak hraje jen z ineho zdroje, napr. predesly --play file by mel byt
  v GUI take? — viz 7 out-of-MVP).
- **Master slider** — okamzity `Engine::setMasterGain(linear)`. Zobrazeni
  v dB s prepoctem `20 * log10(g)`.

### 3.2 Keyboard viz (88 klaves, MIDI 21–108)

Render OpenGL pres ImGui draw list — kazda klavesa je `ImGui::AddRectFilled`.
Bile klavesy: 52 ks (sirka W). Cerne: 36 ks (sirka 0.6W, vyska 0.6H,
zapozicovane na hranicich). Layout v octave: C/D/E/F/G/A/H + diesy.

Pro kazdou MIDI N v 21–108:
- Pokud `pool->isActive(N)`: vykresli zvyraznenou s alpha = `pool->currentGainFor(N)`
  (clamped 0.3..1.0 aby slabe pozadi nevypadlo do nuly).
- Cerne klavesy preden na top of bile (overlap).
- Pedal indikator pod klavesnici: 8px ramecek, barva podle CC64.

### 3.3 Diag panel

Refresh 30 Hz (kazdy 2. frame pri 60fps). Hodnoty atomic-load:

| Metrika | Zdroj |
|---------|-------|
| Voices | `Engine::activeVoices()` (existuje) / `cfg.max_voices` |
| Resonance | `Engine::resonanceVoices()` (NEW) / `cfg.max_resonance_voices` |
| Rings | `Engine::numRingsUsed()` (NEW) / `cfg.num_rings` |
| Pedal CC | `Engine::pedalCC()` (NEW) |
| Master peak L | `Engine::masterPeakL()` (NEW, decay 100 ms) |
| Master peak R | `Engine::masterPeakR()` (NEW, decay 100 ms) |

Pedal indikator + CC64 hodnotu zobrazit i v keyboard panelu (3.2).

### 3.4 Params panel

Slider widgety — kazdy callback volaji setter Engine **az pri zmene** (`if (changed) ...`).

| Slider | Engine setter | Rozsah | Default |
|--------|---------------|--------|---------|
| Resonance strength | `Engine::setResonanceStrength(float)` (wrap existing) | 0..1 | 0.5 |
| Release ms | `Engine::setReleaseMs(float)` (NEW) | 50..2000 | 200 |
| Excite decay ms | `Engine::setExciteDecayMs(float)` (NEW, wrap) | 500..30000 | 5000 |
| Max resonance voices | (init-only — slider DISABLED s tooltipem "Vyzaduje restart") | 1..64 | 32 |
| Master gain | `Engine::setMasterGain(float)` (existuje) | -60..+6 dB | 0 dB |

`[Reset to defaults]` tlacitko — vrati vsechny slidery na default a zavola setter.

### 3.5 Log strip

Scrollable list poslednich 50 log eventu z `log::Logger` (subscriber API
viz 4.5). Defaultne zobrazi posledni 5 viditelnych, scroll up pro vic.
Auto-scroll k novemu eventu (pokud uzivatel neposunul scrolovatko).

---

## 4. Engine API doplneni

Tabulka existuje vs nove:

| Symbol | Status | Vraci |
|--------|--------|-------|
| `int Engine::activeVoices() const` | existuje | hlavni active count |
| `int Engine::resonanceVoices() const` | **NEW** | `resonance_->activeCount()` |
| `int Engine::numRingsUsed() const` | **NEW** | iteruje `stream_->rings_`, vraci pocet `in_use_` |
| `uint8_t Engine::pedalCC() const` | **NEW** | `pedal_.sustainCC()` |
| `float Engine::masterPeakL() const` | **NEW** | atomic peak meter (decay 100 ms) |
| `float Engine::masterPeakR() const` | **NEW** | dtto |
| `void Engine::activeMidiNotes(bool out[128]) const` | **NEW** | bool maska aktivnich main voice midi |
| `float Engine::currentGainFor(int midi) const` | **NEW** | max currentLevel() pres voicy te midi |
| `void Engine::setReleaseMs(float ms)` | **NEW** | atomic set `cfg_.release_ms` |
| `void Engine::setResonanceStrength(float)` | **NEW (wrap)** | `resonance_->setStrength(s)` |
| `void Engine::setExciteDecayMs(float ms)` | **NEW (wrap)** | `resonance_->setExciteDecayTimeMs(ms, ...)` |

### 4.1 Peak meter implementace

V `Engine::processBlock` po master_gain aplikaci:
```cpp
float peak_l = 0, peak_r = 0;
for (int i = 0; i < n_samples; ++i) {
    peak_l = std::max(peak_l, std::abs(out_l[i]));
    peak_r = std::max(peak_r, std::abs(out_r[i]));
}
// Decay: new = max(peak, current * decay_factor)
const float decay = std::exp(-(float)n_samples / (0.1f * sr));
master_peak_l_.store(std::max(peak_l, master_peak_l_.load() * decay));
master_peak_r_.store(std::max(peak_r, master_peak_r_.load() * decay));
```

GUI cte atomic load, prevadi na dB pres `20 * log10(max(peak, 1e-6))`.

### 4.2 Bank reload

Runtime swap:
1. GUI vola `Engine::reloadBank(new_path)`.
2. Engine: mute audio (atomic flag `bank_loading_=true`), drain voicy
   (allNotesOff + release short), `loadBank(new_path)`, unmute.
3. UI behem load zobrazi modal overlay "Loading bank...".

Pre MVP: loading na main threadu (UI freezne ~200 ms). Future: background thread.

### 4.3 MIDI port runtime change

`MidiInput::close()` + `MidiInput::open(engine, port_name)`. Trivial.
RtMidi je thread-safe pro tyto operace z main threadu.

### 4.4 Active midi maska + gain

```cpp
void Engine::activeMidiNotes(bool out[128]) const noexcept {
    std::memset(out, 0, 128 * sizeof(bool));
    for (const Voice& v : pool_->voicesView()) {  // const accessor TBD
        if (v.active() && v.midi() >= 0 && v.midi() < 128) {
            out[v.midi()] = true;
        }
    }
}

float Engine::currentGainFor(int midi) const noexcept {
    float g = 0.f;
    for (const Voice& v : pool_->voicesView()) {
        if (v.active() && v.midi() == midi)
            g = std::max(g, v.currentLevel());
    }
    return g;
}
```

VoicePool dostane novy const accessor `voicesView()` vracejici
`const std::vector<Voice>&` (jednoduchy getter).

### 4.5 Log subscriber API

`log::Logger` dnes pise jen do konzole/souboru. Pridat:
```cpp
class Logger {
    // ...
    using Subscriber = std::function<void(const LogEntry&)>;
    void addSubscriber(Subscriber s);  // GUI registruje svuj kruhovy buffer
};
```

GUI drzi ring buffer kapacity 256 entries, zobrazi posledni vyriznute v log strip.
Subscriber callback bezi na threadu, ktery loguje — synchronizace mezi
producent (any thread) a konzument (main thread) pres mutex (log strip
neni RT-critical).

---

## 5. Persistence

### 5.1 Cesta

Per OS:
- macOS: `$HOME/Library/Application Support/ithaca-legacy/state.json`
- Linux: `$XDG_CONFIG_HOME/ithaca-legacy/state.json` (fallback `$HOME/.config/...`)
- Windows: `%APPDATA%\ithaca-legacy\state.json`

Detekce: standardni `std::filesystem` + platform-specific env var lookup.

### 5.2 Format

```json
{
  "schema_version": 1,
  "bank_path": "/Users/j/SoundBanks/Ithaca/as-blackgrand",
  "midi_port_name": "IAC Driver Bus 1",
  "master_gain_db": -6.0,
  "resonance_strength": 0.5,
  "release_ms": 200,
  "excite_decay_ms": 5000,
  "max_resonance_voices": 32,
  "window": { "x": 100, "y": 100, "w": 1024, "h": 768 }
}
```

### 5.3 Lifecycle

- **Start**: `loadState()` po inicializaci ImGui, pred prvnim renderem.
  Pokud soubor neexistuje nebo schema_version nesedi → defaulty + warn log.
- **Save**: `saveState()` v `atexit` + pri zmene (debounce 1 s).
- **Atomic write**: `state.json.tmp` + rename → predejde corrupted state pri crash.

---

## 6. Build (CMake)

```cmake
# third-party/imgui/CMakeLists.txt
add_library(imgui STATIC
    imgui/imgui.cpp imgui/imgui_draw.cpp imgui/imgui_tables.cpp
    imgui/imgui_widgets.cpp
    imgui/backends/imgui_impl_glfw.cpp
    imgui/backends/imgui_impl_opengl3.cpp)
target_include_directories(imgui PUBLIC imgui imgui/backends)
target_link_libraries(imgui PUBLIC glfw)

# third-party/glfw/CMakeLists.txt
add_subdirectory(glfw)   # vendorovany glfw repo

# top-level CMakeLists.txt
add_executable(ithaca-gui
    app/gui/main.cpp
    app/gui/window.cpp
    app/gui/panels.cpp
    app/gui/keyboard.cpp
    app/gui/persistence.cpp)
target_link_libraries(ithaca-gui PRIVATE ithaca_core imgui glfw)
if(APPLE)
    target_link_libraries(ithaca-gui PRIVATE
        "-framework OpenGL" "-framework Cocoa" "-framework IOKit"
        "-framework CoreVideo")
endif()
```

---

## 7. Out of MVP (defer)

- Audio device selector (vyuziva default device pres miniaudio)
- Block size selector v UI (zatim pres CLI flag `--block-size`)
- CPU load meter (vyzaduje audio thread instrumentaci)
- File-based `--play <wav>` rendering v GUI (CLI staci)
- Polished Keyscape-style koncovy UI (mixdown UI, refresh tlacitka, ...)
- VST wrapper (oddelena faze)

---

## 8. Architektonicke invarianty

1. **GUI thread NIKDY nesahne do audio threadu primo** — vsechny setter
   volani jsou atomic-store nebo lock-free queue.
2. **Engine API zustava pouzitelna headless** — vse co GUI pridava jsou
   gettery/settery, ne narocne dependence. CLI nedostane regresse.
3. **Vendoring** — ImGui a GLFW vendorovane stejne jako miniaudio/rtmidi
   (zadne system pkg-config dependency).
4. **No exceptions na audio threadu** — gettery vraci hodnotu nebo
   default; setter sety atomic.
5. **Persistence ne-blokuje hlavni thread** — load je sync (na startu OK),
   save je async (debounce, na vlastnim threadu nebo idle hook).

---

## 9. Test plan

Doctest unit testy:
- `test_persistence`: round-trip serialize/deserialize state.json + missing
  file + invalid schema.
- `test_engine_getters`: numRingsUsed/activeMidiNotes/pedalCC vraci spravne
  hodnoty po sekvenci noteOn/noteOff/sustain.
- `test_master_peak_meter`: po block s peak 0.5 → masterPeakL/R returns ~0.5,
  po decay window vraci nizsi.

Manualni:
- Spustit gui, vybrat banku, vybrat MIDI, hrat — vidim aktivni klavesy zvyraznene.
- Pohyb sliderem `resonance_strength` zmeni rezonanci za behu (slysitelne).
- Restart aplikace nacte state.json a polozky se obnovi.
- Zmena banky za behu funguje bez pádu (krátký mute, sample swap, unmute).

---

## 10. Otevrene technicke otazky (k vyresseni pri implementaci)

- **GLFW header strategy** — vendorovat `glfw/include/GLFW/glfw3.h` nebo
  pouzit `find_package(glfw3)`? Pro consistency s miniaudio/rtmidi → vendoring.
- **ImGui vykres bez OpenGL**? Backend OpenGL3 vyzaduje OpenGL 3.3+; na
  starsim Rpi mozna problem (default Mesa OK na 4 GB Rpi 4/5). Fallback
  na Vulkan backend (imgui_impl_vulkan) je vetsi prace; defer.
- **HiDPI / retina scaling** — ImGui podporuje `io.DisplayFramebufferScale`.
  Pri startu detekovat dpi a setnout `style.ScaleAllSizes()`. Pre MVP staci
  default; polish v iteraci.
