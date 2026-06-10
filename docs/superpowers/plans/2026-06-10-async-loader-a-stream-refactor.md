# Async loader banky + StreamedSampleReader refactor — implementační plán

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Reload banky nikdy nezamrazí GUI (worker thread + modální overlay s progress 0–100 %), načítání je paralelní; streaming kód Voice/ResonanceVoice je sjednocený do `StreamedSampleReader` s bit-exact zárukou + tři levné výkonové výhry.

**Architecture:** Část A — `BankLoadProgress` (atomiky) plněná loaderem, GUI thread v `AppContext` volá stávající `engine.reloadBank()`, render smyčka polluje progress a kreslí overlay; ingest a stavba rezonanční cache jedou worker poolem. Část B — kompozice `StreamedSampleReader` (žádné virtuály), okno/seed/refill sdílené, policy (underrun vs EOF) zůstává ~10 řádků per hlas; poté bulk čtení ringu, O(1) `hasActiveMainVoice`, damp pool. Bit-exact hlídá hash fixture zafixovaná PŘED refactorem.

**Tech Stack:** C++20, doctest (`tests/`, nová binárka test_render_regression → úprava `tests/CMakeLists.txt`), build `cmake --build build -j`, testy `ctest --test-dir build --output-on-failure`. Spec: `docs/superpowers/specs/2026-06-10-async-loader-a-stream-refactor-design.md`.

**Mapa souborů:**
- Modify: `engine/sample/sample_store.{h,cpp}` (A1, A2), `engine/engine.{h,cpp}` (A3), `app/gui/app_context.{h,cpp}` (A4), `app/gui/panel_bank.cpp` + `app/gui/main.cpp` (A5)
- Create: `engine/stream/streamed_reader.{h,cpp}` (B2), `tests/test_render_regression.cpp` (B1)
- Modify: `engine/voice/voice.{h,cpp}`, `engine/voice/resonance_voice.{h,cpp}` (B2), `engine/stream/stream_engine.h` (B3), `engine/voice/voice_pool.{h,cpp}` (B4, B5), `CMakeLists.txt` + `tests/CMakeLists.txt` (B1, B2)
- Testy: `tests/test_sample_store.cpp`, `tests/test_resonance_cache_build.cpp`, `tests/test_voice_pool.cpp`, `tests/test_render_regression.cpp`
- Docs (průběžně, memory pravidlo): `docs/reference/{C,D,F,H}-*.md`

---

## ČÁST A — Async loader

### Task A1: BankLoadProgress + progress plumbing (sériový loader)

**Files:** Modify `engine/sample/sample_store.h`, `engine/sample/sample_store.cpp`; Test `tests/test_sample_store.cpp`

- [ ] **A1.1: Failing test** — do `tests/test_sample_store.cpp` přidej:

```cpp
TEST_CASE("loadBank plni BankLoadProgress (faze monotonni, done==total na konci)") {
    namespace fs = std::filesystem;
    std::string dir = "/tmp/ithaca_fixture_progress";
    fs::remove_all(dir); fs::create_directories(dir);
    writeConstWav(dir + "/m060-vel0-f48.wav", 0.2f);
    writeConstWav(dir + "/m062-vel0-f48.wav", 0.4f);
    writeConstWav(dir + "/m064-vel0-f48.wav", 0.6f);
    auto& L = log::Logger::default_();
    L.setOutputMode(false, false);
    BankLoadProgress prog;
    Bank bank = loadBank(dir, L, 0, 0, 127, 150, 500, &prog);
    fs::remove_all(dir);
    CHECK(bank.loaded_samples == 3);
    CHECK(prog.phase.load() == 1);              // heads dokoncene (cache az Engine)
    CHECK(prog.total.load() == 3);
    CHECK(prog.done.load()  == 3);
}

TEST_CASE("bankLoadFraction mapuje faze na 0..1 (heads 60 %, cache 40 %)") {
    CHECK(bankLoadFraction(0, 0, 0)   == doctest::Approx(0.f));
    CHECK(bankLoadFraction(1, 1, 2)   == doctest::Approx(0.3f));
    CHECK(bankLoadFraction(1, 2, 2)   == doctest::Approx(0.6f));
    CHECK(bankLoadFraction(2, 1, 4)   == doctest::Approx(0.7f));
    CHECK(bankLoadFraction(3, 0, 0)   == doctest::Approx(1.f));
    CHECK(bankLoadFraction(1, 0, 0)   == doctest::Approx(0.f));   // total 0 → 0
}
```

Run: `cmake --build build -j --target test_sample_store` → COMPILE FAIL (`BankLoadProgress` neexistuje).

- [ ] **A1.2: Implementace** — `sample_store.h` (za includy přidej `#include <atomic>`; před `loadBank`):

```cpp
// Prubeh nacitani banky pro GUI (vlastni ji caller — typicky AppContext;
// loader jen plni atomiky, GUI je polluje per frame).
// phase: 0 = scan, 1 = heads (done/total = soubory),
//        2 = rezonancni cache (done/total = nahrane noty), 3 = hotovo.
struct BankLoadProgress {
    std::atomic<int> phase{0};
    std::atomic<int> done{0};
    std::atomic<int> total{0};
};

// Mapovani fazi na jeden progress bar 0..1: heads = 0..0.6, cache = 0.6..1.0
// (cache build cte ~stovky MB z disku — bez vahy by bar "visel" na 100 %).
inline float bankLoadFraction(int phase, int done, int total) {
    const float t = (total > 0) ? (float)done / (float)total : 0.f;
    switch (phase) {
        case 1:  return 0.6f * t;
        case 2:  return 0.6f + 0.4f * t;
        case 3:  return 1.f;
        default: return 0.f;   // scan
    }
}
```

Signatury rozšiř o `BankLoadProgress* progress = nullptr` (poslední parametr `loadBank` i `buildResonanceCache`; nullptr = chování beze změny).

`sample_store.cpp` — `loadBank`: po scanu `if (progress) { progress->phase.store(1); progress->total.store(<pocet eligible souboru po midi filtru>); progress->done.store(0); }` (eligible spočítej předem do `std::vector<int> idx` indexů do `scan.files` — využije ho i A2); po každém `ingestSampleFile` `if (progress) progress->done.fetch_add(1, std::memory_order_relaxed);`.
`buildResonanceCache`: na začátku `if (progress) { progress->phase.store(2); progress->total.store(<pocet recorded not>); progress->done.store(0); }`; na konci těla každé recorded noty `done.fetch_add(1)`.

- [ ] **A1.3:** Run test → PASS; celé ctest → PASS.
- [ ] **A1.4: Commit** `feat(loader): BankLoadProgress + bankLoadFraction (progress plumbing, zatim seriove)`

### Task A2: Paralelní ingest + paralelní stavba rezonanční cache

**Files:** Modify `engine/sample/sample_store.cpp`; Test `tests/test_sample_store.cpp`, `tests/test_resonance_cache_build.cpp`

- [ ] **A2.1: Failing test (determinismus + obsah)** — `tests/test_sample_store.cpp`:

```cpp
TEST_CASE("paralelni ingest: dva behy daji identickou banku (deterministicky merge)") {
    namespace fs = std::filesystem;
    std::string dir = "/tmp/ithaca_fixture_par";
    fs::remove_all(dir); fs::create_directories(dir);
    for (int n = 50; n < 70; ++n) {                  // 20 souboru → vic nez workeru
        char name[40];
        std::snprintf(name, sizeof(name), "/m%03d-vel0-f48.wav", n);
        writeConstWav(dir + name, 0.1f + 0.04f * (float)(n - 50));
    }
    auto& L = log::Logger::default_();
    L.setOutputMode(false, false);
    Bank a = loadBank(dir, L);
    Bank b = loadBank(dir, L);
    fs::remove_all(dir);
    CHECK(a.loaded_samples == 20);
    CHECK(b.loaded_samples == 20);
    CHECK(a.total_bytes == b.total_bytes);
    for (int n = 0; n < 128; ++n) {
        REQUIRE(a.notes[n].slots.size() == b.notes[n].slots.size());
        for (size_t s = 0; s < a.notes[n].slots.size(); ++s) {
            CHECK(a.notes[n].slots[s].rms_db == b.notes[n].slots[s].rms_db);
            CHECK(a.notes[n].slots[s].variants[0].mics[0].preload_head
                  == b.notes[n].slots[s].variants[0].mics[0].preload_head);
        }
    }
}
```

(Test projde i na sériové verzi — slouží jako regresní síť pro A2.2; race odhalí TSan v B6.)

- [ ] **A2.2: parallelFor + prepare/commit split** — `sample_store.cpp`, anonymní namespace (přidej `#include <atomic>`, `#include <thread>`, `#include <vector>`):

```cpp
// Jednoduchy fork-join: rozdej indexy 0..n-1 mezi az n_workers vlaken.
template <typename Fn>
void parallelFor(int n, int n_workers, Fn&& fn) {
    if (n <= 0) return;
    const int nw = (std::min)(n_workers, n);
    if (nw <= 1) { for (int i = 0; i < n; ++i) fn(i); return; }
    std::atomic<int> next{0};
    std::vector<std::thread> ts;
    ts.reserve((size_t)nw);
    for (int t = 0; t < nw; ++t)
        ts.emplace_back([&] {
            for (;;) {
                const int i = next.fetch_add(1, std::memory_order_relaxed);
                if (i >= n) break;
                fn(i);
            }
        });
    for (auto& t : ts) t.join();
}

int loaderWorkers() {
    int hc = (int)std::thread::hardware_concurrency();
    if (hc <= 0) hc = 4;
    return std::clamp(hc / 2, 2, 8);
}
```

Rozděl `ingestSampleFile` na čistou přípravu (paralelní, bez zápisu do `bank`) a commit (sériový):

```cpp
struct PreparedSample {
    int          midi = -1;
    std::string  filename;     // jen pro logy
    VelocitySlot slot;
    size_t       bytes = 0;    // preload_head bajty (budget/statistiky)
    bool         ok = false;
};

// Cteni + analyza JEDNOHO souboru (bezi paralelne; logger ma vlastni mutex).
// budget_bytes/approx_bytes: hruby OOM guard pro paralelni cteni — kdyz uz
// rozectene heads presahly 2x budget, soubor se NEcte (autoritativni presna
// kontrola je v commitu; pripad prekroceni je tak jako tak ERROR + neuplna
// banka, mnozina nactenych souboru v chybovem pripade neni garantovana).
PreparedSample prepareSampleFile(int midi, const std::string& full_path,
                                 const std::string& filename,
                                 log::Logger& logger, int preload_ms,
                                 size_t budget_bytes,
                                 std::atomic<size_t>& approx_bytes) {
    PreparedSample out;
    out.midi = midi; out.filename = filename;
    WavInfo info = peekWavInfo(full_path);
    if (!info.valid) {
        logger.log("bank", log::Severity::Warning,
                   "Nelze precist hlavicku: %s", filename.c_str());
        return out;
    }
    MicLayer mic;
    mic.mic_name         = "stereo";
    mic.file.path        = full_path;
    mic.file.frames      = info.frames;
    mic.file.sample_rate = info.sample_rate;
    mic.file.valid       = true;
    const int preload_frames =
        (int)((int64_t)preload_ms * info.sample_rate / 1000);
    if (info.frames <= preload_frames * 2) {
        mic.mode = MicLayerMode::FullyLoaded; mic.head_frames = info.frames;
    } else {
        mic.mode = MicLayerMode::Streamed;    mic.head_frames = preload_frames;
    }
    const size_t est = (size_t)mic.head_frames * 2 * sizeof(float);
    if (budget_bytes &&
        approx_bytes.fetch_add(est, std::memory_order_relaxed) + est
            > budget_bytes * 2) {
        return out;   // OOM guard paralelniho cteni (commit by stejne odmitl)
    }
    WavData head = readWavRange(full_path, 0, mic.head_frames);
    if (!head.valid) {
        logger.log("bank", log::Severity::Warning,
                   "Nelze nacist preload (read failed): %s", filename.c_str());
        return out;
    }
    if (head.frames < mic.head_frames) {
        logger.log("bank", log::Severity::Warning,
                   "Oriznuty soubor — pouzivam %d/%d frames: %s",
                   head.frames, mic.head_frames, filename.c_str());
        mic.head_frames = head.frames;
        if (mic.head_frames >= mic.file.frames)
            mic.mode = MicLayerMode::FullyLoaded;
    }
    mic.preload_head = std::move(head.samples);
    const float rms = measurePeakRmsDb(mic.preload_head.data(),
                                       mic.head_frames, info.sample_rate);
    const int   ae  = findAttackEnd  (mic.preload_head.data(),
                                       mic.head_frames, info.sample_rate);
    if (mic.mode == MicLayerMode::Streamed) mic.resonance_start_frame = ae;
    out.bytes = mic.preload_head.size() * sizeof(float);
    SampleAsset asset;
    asset.peak_rms_db      = rms;
    asset.attack_end_frame = ae;
    asset.mics.push_back(std::move(mic));
    out.slot.rms_db = rms;
    out.slot.variants.push_back(std::move(asset));
    out.ok = true;
    return out;
}

// Vlozeni do banky + statistiky (POUZE z merge vlakna, scan poradi).
void commitSample(Bank& bank, PreparedSample&& p) {
    const MicLayer& m = p.slot.variants[0].mics[0];
    bank.resident_frames += (size_t)m.head_frames + (size_t)m.resonance_frames;
    bank.total_bytes     += p.bytes;
    bank.loaded_samples++;
    bank.notes[p.midi].slots.push_back(std::move(p.slot));
    bank.notes[p.midi].recorded = true;
}
```

V `loadBank` nahraď sekvenční smyčku (pozn.: `idx` filtr z A1.2):

```cpp
    std::vector<PreparedSample> prepared(idx.size());
    std::atomic<size_t> approx_bytes{0};
    if (progress) { progress->phase.store(1);
                    progress->total.store((int)idx.size());
                    progress->done.store(0); }
    parallelFor((int)idx.size(), loaderWorkers(), [&](int i) {
        const BankFileEntry& e = scan.files[(size_t)idx[(size_t)i]];
        prepared[(size_t)i] = prepareSampleFile(
            e.parsed.midi, e.full_path, e.parsed.filename, logger,
            preload_ms, budget_bytes, approx_bytes);
        if (progress) progress->done.fetch_add(1, std::memory_order_relaxed);
    });
    // Merge jednovlaknove ve scan poradi → deterministicka banka + presny budget.
    for (auto& p : prepared) {
        if (!p.ok) continue;
        if (budget_bytes && bank.total_bytes >= budget_bytes) {
            logger.log("bank", log::Severity::Error,
                       "Banka '%s': RAM budget %d MB prekrocen (~%zu MB) — nacitani "
                       "PRERUSENO, banka NEUPLNA. Sniz banku / preload_ms / "
                       "resonance_window_ms, nebo zvys cache_budget_mb.",
                       bank.name.c_str(), cache_budget_mb,
                       bank.total_bytes / (1024 * 1024));
            break;
        }
        commitSample(bank, std::move(p));
    }
```

Starý `ingestSampleFile` smaž (nahrazen prepare+commit). Pozn.: `BankFileEntry::parsed.filename` — ověř přesné jméno pole v `bank_index.h` (ParsedName), případně uprav.

- [ ] **A2.3: paralelní buildResonanceCache** — tělo per-note smyčky beze změny, jen obal:

```cpp
    parallelFor(128, loaderWorkers(), [&](int n) {
        // <puvodni telo smycky pro notu n — kazda nota pise JEN sve sloty
        //  a svuj prvek ready[n]; logger ma mutex>
        // na konci tela (jen recorded): if (progress) progress->done.fetch_add(1, ...);
    });
```

- [ ] **A2.4:** Build + nové testy → PASS; celé ctest → PASS (zvlášť test_sample_store, test_resonance_cache_build, test_sample_rate_and_reload).
- [ ] **A2.5: Commit** `feat(loader): paralelni ingest (prepare/commit) + paralelni stavba rezonancni cache`

### Task A3: Engine pass-through progress

**Files:** Modify `engine/engine.h` (fwd decl + signatury), `engine/engine.cpp`

- [ ] **A3.1:** `engine.h`: nad `struct EngineConfig` přidej `struct BankLoadProgress;` (fwd decl; definice v sample_store.h). Signatury: `bool loadBank(const std::string& dir, BankLoadProgress* progress = nullptr);` a `bool reloadBank(const std::string& dir, BankLoadProgress* progress = nullptr);`.
- [ ] **A3.2:** `engine.cpp`: `Engine::loadBank(dir, progress)` předá progress do `ithaca::loadBank(..., progress)` a `ithaca::buildResonanceCache(..., progress)`; po `setCacheReady` (i v catch větvi a při `loaded_samples<=0`) nastav `if (progress) progress->phase.store(3);`. `Engine::reloadBank(dir, progress)` předá do `loadBank(dir, progress)`.
- [ ] **A3.3:** Build + celé ctest → PASS (volání bez progress beze změny).
- [ ] **A3.4: Commit** `feat(engine): loadBank/reloadBank predavaji BankLoadProgress`

### Task A4: AppContext async reload

**Files:** Modify `app/gui/app_context.h`, `app/gui/app_context.cpp`, `app/gui/panel_bank.cpp`

- [ ] **A4.1:** `app_context.h` — přidej `#include "sample/sample_store.h"`, `#include <atomic>`, `#include <string>`, `#include <thread>`; do struktury:

```cpp
    // -- Async reload banky (spec 2026-06-10, cast A) --
    // requestBankReload: spusti worker thread, ktery vola engine.reloadBank
    // (engine ochrany bank_loading_/epoch zustavaji). Druhe volani behem
    // behu = no-op (modalni overlay stejne blokuje UI). Completion zpracuje
    // pollReloadCompletion() na GUI vlakne (layer heuristika, log).
    void requestBankReload(const std::string& dir);
    bool reloadInProgress() const {
        return reload_in_progress_.load(std::memory_order_acquire);
    }
    void pollReloadCompletion();
    const ithaca::BankLoadProgress& loadProgress() const { return load_progress_; }

    std::thread              reload_thread_;
    std::atomic<bool>        reload_in_progress_{false};
    std::atomic<bool>        reload_done_pending_{false};
    std::atomic<bool>        reload_ok_{false};
    std::string              reload_dir_;        // psano PRED spawnem threadu
    ithaca::BankLoadProgress load_progress_;
```

- [ ] **A4.2:** `app_context.cpp`:

```cpp
void AppContext::requestBankReload(const std::string& dir) {
    bool expected = false;
    if (!reload_in_progress_.compare_exchange_strong(expected, true,
                                                     std::memory_order_acq_rel))
        return;                                       // uz bezi (modal blokuje UI)
    if (reload_thread_.joinable()) reload_thread_.join();   // predchozi dobehl
    reload_dir_ = dir;
    load_progress_.phase.store(0); load_progress_.done.store(0);
    load_progress_.total.store(0);
    reload_thread_ = std::thread([this] {
        const bool ok = engine.reloadBank(reload_dir_, &load_progress_);
        reload_ok_.store(ok, std::memory_order_release);
        reload_done_pending_.store(true, std::memory_order_release);
        reload_in_progress_.store(false, std::memory_order_release);
    });
}

void AppContext::pollReloadCompletion() {
    if (!reload_done_pending_.exchange(false, std::memory_order_acq_rel)) return;
    if (!reload_ok_.load(std::memory_order_acquire)) {
        log::Logger::default_().log("gui", log::Severity::Warning,
            "Nelze nacist banku: %s", reload_dir_.c_str());
        return;
    }
    // Default Resonance Layer = 1/3 rozsahu banky, kdyz uzivatel drzi default
    // (-30 dB). Zmena state.resonance_layer_db spusti existujici 400ms layer
    // debounce v main.cpp → engine.rebuildResonanceCache (vyresi i nalez
    // H-stredni z revize: heuristika driv bezela bez rebuilu cache).
    const float lo = engine.bankPeakRmsMinDb(), hi = engine.bankPeakRmsMaxDb();
    if (hi > lo && state.resonance_layer_db == -30.f)
        state.resonance_layer_db = lo + (hi - lo) / 3.f;
    log::Logger::default_().log("gui", log::Severity::Info,
        "Banka nactena: %s (%d not, %d samplu)", reload_dir_.c_str(),
        engine.recordedNotes(), engine.loadedSamples());
}
```

`initFromState`: smaž synchronní bank-load blok i layer heuristiku (řádky „Bank: best-effort…" a „Default Resonance Layer…"); `engine.setResonanceLayerDb(state.resonance_layer_db);` ponech. Na ÚPLNÝ konec (za MIDI open, před `return true`):

```cpp
    // Startovni load banky jede async stejnou cestou jako reload — okno se
    // ukaze hned, prubeh kryje modalni overlay v render smycce.
    if (!state.bank_path.empty()) requestBankReload(state.bank_path);
```

`shutdown()`: na začátek přidej

```cpp
    // Reload nelze prerusit (drzi bank_loading_) — pockej na dobeh PRED
    // zavrenim MIDI/audia (quiesce handshake vyuzije bezici audio).
    if (reload_thread_.joinable()) reload_thread_.join();
```

- [ ] **A4.3:** `panel_bank.cpp`: obě volání `ctx.engine.reloadBank(...)` → `ctx.requestBankReload(...)` (komentář u Selectable uprav na „async; modal overlay viz main.cpp").
- [ ] **A4.4:** Build (vč. ithaca-gui) → OK; ctest → PASS.
- [ ] **A4.5: Commit** `feat(gui): async reload banky pres AppContext worker (start i runtime)`

### Task A5: Modální overlay s progress

**Files:** Modify `app/gui/main.cpp`

- [ ] **A5.1:** Do render smyčky za `ImGui::PopStyleVar();` (konec ##root, před persistence debounce):

```cpp
        // Async bank reload: completion (GUI vlakno) + modalni overlay.
        ctx.pollReloadCompletion();
        if (ctx.reloadInProgress()) {
            const auto& p = ctx.loadProgress();
            const int   phase = p.phase.load(std::memory_order_relaxed);
            const int   done  = p.done.load(std::memory_order_relaxed);
            const int   total = p.total.load(std::memory_order_relaxed);
            const float frac  = ithaca::bankLoadFraction(phase, done, total);
            ImGui::SetNextWindowPos({0, 0});
            ImGui::SetNextWindowSize({W, H});
            ImGui::SetNextWindowFocus();   // topmost → pohlti vsechen vstup
            ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(10, 10, 12, 215));
            ImGui::Begin("##loading", nullptr,
                ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                ImGuiWindowFlags_NoScrollbar);
            const std::string bank_name =
                std::filesystem::path(ctx.state.bank_path).filename().string();
            char line[96];
            if (phase == 2)
                std::snprintf(line, sizeof(line),
                              "Stavim rezonancni cache (%d/%d)", done, total);
            else if (phase == 1)
                std::snprintf(line, sizeof(line),
                              "Nacitam samply (%d/%d)", done, total);
            else
                std::snprintf(line, sizeof(line), "Prohledavam banku...");
            const float cw = 420.f;
            ImGui::SetCursorPos({(W - cw) * 0.5f, H * 0.42f});
            ImGui::BeginGroup();
            ImGui::PushStyleColor(ImGuiCol_Text,
                                  ithaca::gui::theme::Colors::v(
                                      ithaca::gui::theme::Colors::gold));
            ImGui::TextUnformatted(bank_name.c_str());
            ImGui::PopStyleColor();
            ImGui::Dummy({0, 8});
            ImGui::ProgressBar(frac, ImVec2(cw, 14));
            ImGui::Dummy({0, 4});
            ImGui::TextUnformatted(line);
            ImGui::EndGroup();
            ImGui::End();
            ImGui::PopStyleColor();
        }
```

(+ `#include <filesystem>` nahoru, pokud chybí.)

- [ ] **A5.2:** Build ithaca-gui → OK. Manuální smoke: spusť `./build/ithaca-gui`, vyber banku v BANK dropdownu → okno nezamrzne, overlay s barem proběhne, po dokončení fakta o bance naskočí; start aplikace s persistovanou bankou ukáže overlay hned po otevření okna.
- [ ] **A5.3:** Celé ctest → PASS.
- [ ] **A5.4: Commit** `feat(gui): modalni overlay s progress behem nacitani banky`
- [ ] **A5.5: Docs** — `docs/reference/F-loader.md` (loadBank: progress param, paralelní ingest s prepare/commit a sémantikou budgetu, paralelní cache build), `docs/reference/H-gui.md` (async reload, overlay, přesun layer heuristiky do pollReloadCompletion, startovní async load), `docs/reference/A-core.md` (Engine::loadBank/reloadBank signatury). Commit `docs: async loader v F/H/A reference`.

---

## ČÁST B — StreamedSampleReader + výkonové výhry (bit-exact)

### Task B1: Bit-exact hash fixture (PŘED refactorem)

**Files:** Create `tests/test_render_regression.cpp`; Modify `tests/CMakeLists.txt`

- [ ] **B1.1:** Nový test (pokrývá RAM cesty deterministicky: FullyLoaded hlasy, damp/retrigger/steal, pedál, rezonance cache-mode; streamovaný ring kryjí stávající behaviorální testy — seam/ramp/EOF/underrun):

```cpp
// tests/test_render_regression.cpp
// Bit-exact regrese pro refactor StreamedSampleReader (spec 2026-06-10 cast B):
// hash vystupu deterministicke sceny se NESMI zmenit. Fixture je vazana na
// toolchain (FP poradi operaci) — pri zmene platformy/kompilatoru hashe
// preregeneruj: spust test, vypsane hodnoty vloz do kExpected.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "engine.h"
#include "io/wav_writer.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

using namespace ithaca;
namespace fs = std::filesystem;

namespace {
uint64_t fnv1a(const float* L, const float* R, int n, uint64_t h) {
    auto mix = [&](float v) {
        uint32_t b; std::memcpy(&b, &v, 4);
        h ^= b; h *= 1099511628211ull;
    };
    for (int i = 0; i < n; ++i) { mix(L[i]); mix(R[i]); }
    return h;
}
void writeRampWav(const std::string& p, int frames, float amp) {
    std::vector<float> s((size_t)frames * 2);
    for (int i = 0; i < frames; ++i) {
        const float v = amp * (float)i / (float)frames;
        s[(size_t)i * 2] = v; s[(size_t)i * 2 + 1] = v;
    }
    REQUIRE(writeWavStereo16(p, s, 48000));
}
} // namespace

TEST_CASE("render regression: hash deterministicke sceny (bit-exact guard)") {
    // 0 = placeholder → test vypise skutecne hodnoty; po zafixovani PASS.
    static const uint64_t kExpected[8] = {0, 0, 0, 0, 0, 0, 0, 0};

    const std::string dir = "/tmp/ithaca_regr_bank";
    fs::remove_all(dir); fs::create_directories(dir);
    // 12000 frames @48k < 2*preload(150ms=7200) → FullyLoaded (deterministicke).
    for (int n : {48, 60, 64, 67, 72}) {
        char name[40];
        std::snprintf(name, sizeof(name), "/m%03d-vel0-f48.wav", n);
        writeRampWav(dir + name, 12000, 0.2f + 0.1f * (float)((n - 48) % 5));
    }
    log::Logger::default_().setOutputMode(false, false);

    Engine e;
    EngineConfig cfg;
    cfg.sample_rate = 48000; cfg.block_size = 128;
    cfg.max_voices  = 2;                  // vynuti steal
    cfg.midi_from = 40; cfg.midi_to = 80;
    REQUIRE(e.init(cfg));
    REQUIRE(e.loadBank(dir));
    fs::remove_all(dir);

    std::vector<float> L(128), R(128);
    uint64_t h = 1469598103934665603ull;
    uint64_t got[8]; int gi = 0;
    auto run = [&](int blocks) {
        for (int b = 0; b < blocks; ++b) {
            std::fill(L.begin(), L.end(), 0.f);
            std::fill(R.begin(), R.end(), 0.f);
            e.processBlock(L.data(), R.data(), 128);
            h = fnv1a(L.data(), R.data(), 128, h);
        }
        got[gi++] = h;
    };
    e.noteOn(60, 100);                          run(8);   // [0] onset + tone
    e.sustainPedal(127); e.noteOn(48, 90);      run(8);   // [1] pedal + rezonance
    e.noteOn(60, 80);                           run(4);   // [2] retrigger (damp)
    e.noteOn(64, 70); e.noteOn(67, 60);         run(8);   // [3] pool 2 → steal
    e.noteOff(60); e.sustainPedal(0);           run(8);   // [4] release + pedal up
    e.noteOn(72, 110);                          run(4);   // [5] novy ton
    e.allNotesOff();                            run(8);   // [6] panic release
    run(8);                                                // [7] doznivani

    bool placeholder = true;
    for (auto v : kExpected) if (v != 0) placeholder = false;
    for (int i = 0; i < 8; ++i) {
        if (placeholder)
            MESSAGE("kExpected[" << i << "] = " << got[i] << "ull");
        else
            CHECK(got[i] == kExpected[i]);
    }
    CHECK_FALSE(placeholder);   // donuti zafixovat hodnoty
}
```

`tests/CMakeLists.txt` přidej:

```cmake
add_executable(test_render_regression test_render_regression.cpp)
target_link_libraries(test_render_regression PRIVATE ithaca_core doctest)
add_test(NAME test_render_regression COMMAND test_render_regression)
```

- [ ] **B1.2:** Run: `cmake --build build -j --target test_render_regression && ./build/tests/test_render_regression` → FAIL (placeholder) + vypíše 8 hodnot. Hodnoty vlož do `kExpected`, rebuild, run → PASS. Run 2× po sobě → stejné hashe (determinismus).
- [ ] **B1.3: Commit** `test(regression): bit-exact hash fixture pred refactorem streamingu`

### Task B2: Extrakce StreamedSampleReader

**Files:** Create `engine/stream/streamed_reader.h`, `engine/stream/streamed_reader.cpp`; Modify `engine/voice/voice.{h,cpp}`, `engine/voice/resonance_voice.{h,cpp}`, `CMakeLists.txt` (přidat `engine/stream/streamed_reader.cpp` do ithaca_core)

- [ ] **B2.1: Reader** — `streamed_reader.h`:

```cpp
#pragma once
// engine/stream/streamed_reader.h
// Sdileny streaming ctec pro Voice a ResonanceVoice (spec 2026-06-10 cast B):
// vlastnictvi ringu, lo/hi interpolacni okno, sev z predchoziho RAM regionu,
// refill heuristika (no-advance-on-drop). POLICY pri prazdnem ringu (underrun
// vs cisty konec vs EOF-hold) zustava v hlasech — reader vraci RingEmpty a
// vystavuje primitivy (eofAcquire, holdHiFromLo, bumpLoIdx). Bit-exact
// extrakce — zadna zmena chovani. Bezi vyhradne na audio vlakne.

#include <cstdint>
#include <string>

namespace ithaca {

class StreamEngine;
struct RingHandle;

class StreamedSampleReader {
public:
    enum class Advance { Reached, RingEmpty };

    // Reset stavu + acquire ring + prvni request [start_frame, min(cap, zbytek)).
    // false = ring pool plny (hlas dohraje RAM region a utichne).
    bool begin(StreamEngine* se, const std::string& path,
               int64_t start_frame, int64_t total_frames) noexcept;
    // Varianta bez requestu (ResonanceVoice: total_after <= 0 → jen EOF ring).
    bool beginEofOnly(StreamEngine* se, int64_t request_off) noexcept;

    // Sev: lo = posledni frame predchoziho RAM regionu, hi = prvni ring pop
    // (fallback lo pri prazdnem ringu).
    void seed(float lo_l, float lo_r, int64_t lo_idx) noexcept;
    bool seeded() const noexcept { return ring_lo_idx_ >= 0; }

    // Posouva okno (lo<-hi, pop hi, idx++) dokud lo_idx < target. RingEmpty:
    // lo uz prepsane hi, idx NEinkrementovan (presne puvodni chovani Voice).
    Advance advance(int64_t target) noexcept;

    float   loL() const noexcept { return ring_lo_l_; }
    float   loR() const noexcept { return ring_lo_r_; }
    float   hiL() const noexcept { return ring_hi_l_; }
    float   hiR() const noexcept { return ring_hi_r_; }
    int64_t loIdx() const noexcept { return ring_lo_idx_; }
    void    holdHiFromLo() noexcept { ring_hi_l_ = ring_lo_l_; ring_hi_r_ = ring_lo_r_; }
    void    bumpLoIdx() noexcept { ring_lo_idx_++; }

    // Per-blok refill (prah z StreamEngine, half-cap reset pendingu,
    // no-advance-on-drop). Volat na konci process() u aktivniho hlasu.
    void refill(StreamEngine* se, const std::string& path) noexcept;

    // Vypopuj az max_frames do interleaved dst (prepareDamp). Vraci pocet.
    int  popInto(float* dst_interleaved, int max_frames) noexcept;

    bool    hasRing() const noexcept { return ring_ != nullptr; }
    int     ringAvailable() const noexcept;
    bool    eofAcquire() const noexcept;     // ring->eof_ (acquire)
    bool    eofRelaxed() const noexcept;     // jen diagnostika/logy
    int64_t requestOffset() const noexcept { return file_request_off_; }
    bool    cleanEnd() const noexcept { return file_request_off_ >= total_frames_; }

    // Vrat ring do poolu (pokud je) + plny reset stavu.
    void release(StreamEngine* se) noexcept;

private:
    RingHandle* ring_   = nullptr;
    int64_t  total_frames_     = 0;
    int64_t  file_request_off_ = 0;
    bool     stream_pending_   = false;
    float    ring_lo_l_ = 0.f, ring_lo_r_ = 0.f;
    float    ring_hi_l_ = 0.f, ring_hi_r_ = 0.f;
    int64_t  ring_lo_idx_ = -1;
};

} // namespace ithaca
```

`streamed_reader.cpp` — implementace přesně přenáší dnešní kód (begin = ring acquire + first request s no-advance-on-drop jako ve `Voice::start`; advance = smyčka `while (lo_idx < target)`; refill = heuristika z `Voice::process` vč. `avail >= half_cap` resetu; popInto = smyčka `popFrame`; release = `releaseRing` + reset všech polí na defaulty). Kód vezmi 1:1 z voice.cpp (řádky start/process/refill) — jediné změny jsou jména členů.

- [ ] **B2.2: Voice na readeru** — `voice.h`: nahraď streaming membery (`ring_`, `file_request_off_`, `stream_pending_`, `ring_lo_*`, `ring_hi_*`, `ring_lo_idx_`) jediným `StreamedSampleReader reader_;` (+ include). `voice.cpp`: `start()` streamed větev → `reader_.begin(stream_, mic_->file.path, mic_->head_frames, mic_->file.frames)`; seed větev → `reader_.seed(head_data[...], head_data[...+1], head_frames - 1)` + první pop přes `advance`? POZOR bit-exact: seed dělá i první pop hi — přenes do `reader_.seed(...)` (reader popne hi, fallback hi=lo). Ring větev `process()`:

```cpp
        } else if (reader_.hasRing()) {
            if (!reader_.seeded())
                reader_.seed(head_data[(size_t)(head_frames - 1) * 2],
                             head_data[(size_t)(head_frames - 1) * 2 + 1],
                             (int64_t)head_frames - 1);
            const bool underrun =
                reader_.advance((int64_t)position_) ==
                StreamedSampleReader::Advance::RingEmpty;
            if (underrun) {
                const bool clean_end = reader_.cleanEnd();
                ... (stavajici underrun/clean-end blok; ring_->available() →
                     reader_.ringAvailable(), sL/sR drzi reader_.loL()/loR()) ...
            } else {
                float frac = (float)(position_ - (double)reader_.loIdx());
                ... interpolace pres reader_.loL/loR/hiL/hiR ...
            }
            position_ += pos_inc_;
        }
```

refill blok → `if (reader_.hasRing() && stream_ && active_) reader_.refill(stream_, mic_->file.path);` (total_frames drží reader). `prepareDamp` ring větev → `int n = reader_.popInto(damp_buf_, damp_frames);`. Všechna `releaseRing` místa (`prepareDamp`, `hardStop`, `process` konec/začátek) → `reader_.release(stream_);`. Diagnostický log → `reader_.hasRing()/ringAvailable()/eofRelaxed()`.

- [ ] **B2.3: ResonanceVoice na readeru** — stejná náhrada; policy větev při RingEmpty zachovává EOF-hold:

```cpp
                const bool empty =
                    reader_.advance(target) ==
                    StreamedSampleReader::Advance::RingEmpty;
                bool underrun = false;
                if (empty) {
                    if (reader_.eofAcquire()) {
                        reader_.holdHiFromLo();
                        reader_.bumpLoIdx();
                        if (reader_.loIdx() >= (int64_t)total_frames - 1)
                            active_ = false;
                    } else {
                        underrun = true;
                    }
                }
```

`start()`: `total_after <= 0` → `reader_.beginEofOnly(stream_, res_end)`; jinak `reader_.begin(stream_, mic_->file.path, res_end, mic_->file.frames)`. Hard guard na konec souboru → `reader_.eofAcquire() && reader_.ringAvailable() == 0`.

- [ ] **B2.4:** CMakeLists: `engine/stream/streamed_reader.cpp` do ithaca_core. Build + **celé ctest včetně test_render_regression → PASS (hashe beze změny!)**. Pokud hash nesedí → chování se odchýlilo, oprav reader (žádné „převzorkování" fixture!).
- [ ] **B2.5: Commit** `refactor(stream): StreamedSampleReader sdileny Voice+ResonanceVoice (bit-exact)`

### Task B3: Bulk čtení ringu

**Files:** Modify `engine/stream/stream_engine.h` (RingHandle API), `engine/stream/streamed_reader.{h,cpp}`, `engine/voice/voice.cpp`, `engine/voice/resonance_voice.cpp`

- [ ] **B3.1:** `RingHandle` přidej (vedle popFrame, který zůstává pro testy):

```cpp
    // Bulk cteni pro StreamedSampleReader: konzument si 1x za blok snapshotne
    // w_ (acquire), cte lokalnim kurzorem a 1x za blok commitne r_ (release)
    // — misto 3 atomickych operaci na kazdy frame. Hodnoty identicke s
    // popFrame (bit-exact).
    size_t snapshotW() const noexcept { return w_.load(std::memory_order_acquire); }
    size_t cursorR()   const noexcept { return r_.load(std::memory_order_relaxed); }
    void   commitR(size_t r) noexcept { r_.store(r, std::memory_order_release); }
```

- [ ] **B3.2:** Reader: membery `size_t blk_r_ = 0, blk_w_ = 0; bool blk_open_ = false;` + `void beginBlock() noexcept; void endBlock() noexcept;`. `beginBlock`: `blk_w_ = ring_->snapshotW(); blk_r_ = ring_->cursorR(); blk_open_ = true;`. Privátní `popLocal(float&, float&)`: `if (blk_r_ >= blk_w_) return false;` čti `ring_->buf[(blk_r_ % cap)*2 ...]`, `++blk_r_`. `endBlock`: `if (blk_open_) { ring_->commitR(blk_r_); blk_open_ = false; }`. `advance`/`seed` používají `popLocal` když `blk_open_`, jinak `popFrame` (popInto a beginEofOnly cesty si dělají begin/end interně). `ringAvailable()` při otevřeném bloku vrací `(int)(blk_w_ - blk_r_)`.
- [ ] **B3.3:** `Voice::process` + `ResonanceVoice::process`: na začátku `if (reader_.hasRing()) reader_.beginBlock();`, před refill blokem `reader_.endBlock();` (a před každým `return`/`release` v těle — release dělá endBlock interně). `refill` musí běžet PO endBlock (čte `available()` z atomik — jako dnes).
- [ ] **B3.4:** Build + celé ctest → **hashe test_render_regression beze změny**; test_streamed_interp/seam/EOF PASS.
- [ ] **B3.5: Commit** `perf(stream): bulk cteni ringu (1 acquire + 1 release za blok misto 3 atomik/vzorek)`

### Task B4: hasActiveMainVoice O(1)

**Files:** Modify `engine/voice/voice_pool.{h,cpp}`; Test `tests/test_voice_pool.cpp`

- [ ] **B4.1: Failing test** — `tests/test_voice_pool.cpp`:

```cpp
TEST_CASE("note_active_count sleduje presne active() stav (ekvivalence se scanem)") {
    SampleAsset a = makeAsset(0.5f, 2000);
    VoicePool pool(2);
    VoiceSpec vs; vs.asset = &a; vs.pitch_ratio = 1.0; vs.vel_gain = 1.0f;
    auto groundTruth = [&](int midi) {
        for (const auto& v : pool.voicesView())
            if (v.active() && v.midi() == midi) return true;
        return false;
    };
    auto checkAll = [&] {
        for (int n = 0; n < 128; ++n)
            CHECK(pool.hasActiveMainVoice(n) == groundTruth(n));
    };
    std::vector<float> L(256, 0.f), R(256, 0.f);
    pool.noteOn(60, vs, 48000.f); checkAll();
    pool.noteOn(64, vs, 48000.f); checkAll();
    pool.noteOn(67, vs, 48000.f); checkAll();          // steal (pool 2)
    pool.noteOn(60, vs, 48000.f); checkAll();          // retrigger (damp)
    pool.noteOff(60, 5.f, 48000.f);
    for (int i = 0; i < 12; ++i) {                      // release dozni + EOF
        std::fill(L.begin(), L.end(), 0.f); std::fill(R.begin(), R.end(), 0.f);
        pool.processBlock(L.data(), R.data(), 256, 48000.f);
        checkAll();
    }
    pool.reset(); checkAll();
}
```

Run → PASS i na scanu (ekvivalence) — slouží jako regresní síť; FAIL nastane, až čítač implementuješ špatně.

- [ ] **B4.2:** `voice_pool.h`: member `uint8_t note_active_count_[128] = {};` + komentář (eligibility rezonance volá hasActiveMainVoice až 127× per note-on → O(1) místo O(pool) scan). `voice_pool.cpp`:
  - `noteOn`: retrigger smyčka — `if (v.active()) { v.prepareDamp(...); if (note_active_count_[midi]) note_active_count_[midi]--; }` (prepareDamp deaktivuje). Steal větev: `if (v.active()) { int om = v.midi(); v.prepareDamp(...); if (om >= 0 && om < 128 && note_active_count_[om]) note_active_count_[om]--; }`. Po `v.start(...)`: `if (v.active()) note_active_count_[midi]++;`.
  - `processBlock`: `const bool was = v.active(); ... v.process(...); if (was && !v.active()) { const int m = v.midi(); if (m >= 0 && m < 128 && note_active_count_[m]) note_active_count_[m]--; }`.
  - `reset()`: `std::memset(note_active_count_, 0, sizeof(note_active_count_));` (hardStop deaktivuje vše).
  - `hasActiveMainVoice`: `return midi >= 0 && midi < 128 && note_active_count_[midi] > 0;` (komentář o invariantu: čítač sleduje výhradně přechody řízené poolem).
- [ ] **B4.3:** Build + testy (vč. hash regrese — nesmí se hnout) → PASS.
- [ ] **B4.4: Commit** `perf(voice): hasActiveMainVoice O(1) pres per-nota citac aktivnich hlasu`

### Task B5: Damp buffery do souvislého poolu

**Files:** Modify `engine/voice/voice.h`, `engine/voice/voice_pool.{h,cpp}`; Test `tests/test_voice_pool.cpp`

- [ ] **B5.1: Failing test:**

```cpp
TEST_CASE("sizeof(Voice) je maly (damp buffer zije v poolu, ne inline)") {
    CHECK(sizeof(Voice) < 512);   // drive ~16.5 kB (16kB damp_buf_ inline)
}
```

Run → FAIL (sizeof ~16528).

- [ ] **B5.2:** `voice.h`: `float damp_buf_[2 * kDampMaxFrames] = {};` → `float* damp_buf_ = nullptr;   // 2*kDampMaxFrames floatu; vlastni VoicePool (cache locality skenu)` + public `void setDampBuffer(float* p) { damp_buf_ = p; }`. V `prepareDamp` přidej guard `if (active_ && mic_ && damp_buf_)`. `voice_pool.h`: member `std::vector<float> damp_pool_;`. `voice_pool.cpp` konstruktor po `voices_.resize`:

```cpp
    // Damp buffery mimo Voice: sizeof(Voice) ~16.5 kB → ~stovky B; skeny poolu
    // (processBlock/findSlot/citace) prestanou krokovat pres cache po 16 kB.
    damp_pool_.assign((size_t)n * 2 * kDampMaxFrames, 0.f);
    for (int i = 0; i < n; ++i)
        voices_[(size_t)i].setDampBuffer(
            damp_pool_.data() + (size_t)i * 2 * kDampMaxFrames);
```

- [ ] **B5.3:** Build + testy (vč. hash regrese beze změny) → PASS.
- [ ] **B5.4: Commit** `perf(voice): damp buffery v souvislem poolu (sizeof(Voice) 16.5kB -> stovky B)`

### Task B6: Finální verifikace + dokumentace

- [ ] **B6.1:** `ctest --test-dir build` → vše PASS (34 binárek). `cmake --build build-asan -j && ctest --test-dir build-asan` → PASS. `cmake --build build-tsan -j && ctest --test-dir build-tsan` → PASS, 0 race reportů (kriticky: test_sample_store paralelní loader, test_render_regression).
- [ ] **B6.2: Docs** — `docs/reference/C-buffers.md` (RingHandle bulk API, StreamedSampleReader), `docs/reference/D-polyphony.md` (reader kompozice, note_active_count_, damp pool, sizeof), `docs/reference/E-resonance.md` (ResonanceVoice na readeru, hasActiveMainVoice O(1) poznámka). Aktualizuj i zmínky ve výkonových nálezech review dokumentu (§2 D-střední sizeof/popFrame → OPRAVENO v této větvi). Spec: doplň „Stav: implementováno".
- [ ] **B6.3: Commit** `docs: StreamedSampleReader + perf zmeny v C/D/E reference; spec uzavren`
- [ ] **B6.4:** Spusť `./build/ithaca-gui` pro finální uživatelský poslechový test.

---

## Self-review

- **Pokrytí specu:** BankLoadProgress+fraction (A1), paralelní ingest/cache (A2), Engine pass-through (A3), requestBankReload/poll/shutdown/start-async/heuristika (A4), overlay (A5), hash fixture před refactorem (B1), reader (B2), bulk (B3), O(1) čítač (B4), damp pool (B5), sanitizery+docs (B6). Mimo rozsah dle specu: zrušitelnost, CLI progress, SoA, FFT.
- **Typová konzistence:** `BankLoadProgress` (phase/done/total) shodné v A1/A3/A4/A5; `StreamedSampleReader` API (begin/beginEofOnly/seed/advance/refill/popInto/release/beginBlock/endBlock) konzistentní B2↔B3; `note_active_count_` jen uvnitř VoicePool; `setDampBuffer` definováno v B5 a voláno tamtéž.
- **Známá rizika:** (1) `BankFileEntry`/`ParsedName` pole — ověřit přesná jména při A2; (2) hash fixture vázaná na toolchain — dokumentováno v testu; (3) budget+paralelní čtení mění množinu načtených souborů v CHYBOVÉM případě (ERROR + neúplná banka) — dokumentováno v F-loader.md (A5.5).
