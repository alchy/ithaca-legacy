# Mitigace nálezů revize 2026-06-10 — implementační plán

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Opravit kritické a vysoké nálezy z `docs/review-2026-06-10.md` (RT-safety, race conditions, audio artefakty, GUI/persistence chybové cesty), doplnit testy a uvést dokumentaci do souladu s kódem.

> **STAV: ✅ VŠECHNY TASKY 0–14 DOKONČENY** (commity `c5be9b9..e26a600` na větvi
> `fix/revize-2026-06-10`). 33/33 testových binárek PASS v Release, ASan/UBSan
> i TSan buildu. Jediná odchylka od plánu: underrun hold-last (Task 4) se
> aplikuje JEN na skutečný underrun — čistý EOF zůstal deactivate+zero
> (reference icr; odhalily to regresní testy seam/EOF).

**Architecture:** RT audio engine (C++20, lock-free SPSC vzory, audio vlákno bez zámků/alokací). Opravy zachovávají stávající architekturu: logy v audio cestě přechází na existující `LOG_RT_*` ring, sleep-heuristiky se nahrazují epoch handshake (`block_epoch_`), ringy dostávají generation counter, MidiQueue přechází na Vyukov bounded MPSC.

**Tech Stack:** CMake + doctest (testy v `tests/`, jeden TU = jedna binárka, registrace v `tests/CMakeLists.txt` — nové testy přidáváme do EXISTUJÍCÍCH souborů, CMake se nemění). Build: `cmake --build build -j` ; testy: `ctest --test-dir build --output-on-failure`.

**Mimo rozsah této větve** (vědomě odloženo, viz review §5.6–5.8): async reload banky s progress barem, paralelní loader, WAV format whitelist, změny zvuku convolveru (energetická normalizace, limiter lookahead, stale-state) — mění vyladěný zvuk, vyžadují rozhodnutí uživatele; výkonové přestavby (SoA voice pool, bulk popFrame, partitioned FFT).

---

### Task 0: Příprava

- [x] Branch `fix/revize-2026-06-10` vytvořena, review dokument commitnut (c5be9b9).
- [ ] **Step 0.1:** Ověř zelený výchozí stav: `cmake --build build -j && ctest --test-dir build --output-on-failure`. Očekávání: všechny testy PASS (baseline).

---

### Task 1: Warm-up harmonické matice mimo audio vlákno (review 1.1, KRITICKÁ)

**Files:**
- Modify: `engine/resonance/harmonic_proximity.h`, `engine/resonance/harmonic_proximity.cpp`
- Modify: `engine/engine.cpp` (init)
- Test: `tests/test_harmonic_proximity.cpp`

- [ ] **Step 1.1: Failing test** — do `tests/test_harmonic_proximity.cpp` přidej:

```cpp
TEST_CASE("initHarmonicProximity predpocita matici off-RT (idempotentni)") {
    initHarmonicProximity();
    initHarmonicProximity();   // druhe volani = no-op
    CHECK(harmonicProximity(60, 72) == doctest::Approx(1.0).epsilon(0.05));
}
```

Run: `cmake --build build -j --target test_harmonic_proximity && ./build/tests/test_harmonic_proximity` → očekávej COMPILE FAIL (`initHarmonicProximity` neexistuje).

- [ ] **Step 1.2: Implementace** — `harmonic_proximity.h` před `harmonicProximity` přidej:

```cpp
// Predpocita 128x128 coupling matici (drahy build: ~4M iteraci s log2f/expf,
// na RPi5 az ~1 s). Volat z off-RT kontextu (Engine::init) PRED prvnim pouzitim
// — jinak lazy init probehne pri prvnim note-onu NA AUDIO VLAKNE → dropout.
void initHarmonicProximity();
```

`harmonic_proximity.cpp` za `couplingMatrix()` (vně anonymního namespace, vedle `harmonicProximity`):

```cpp
void initHarmonicProximity() { (void)couplingMatrix(); }
```

`engine.cpp`: přidej `#include "resonance/harmonic_proximity.h"` a v `Engine::init` za blok `resonance_->...` (před `master_gain_.store`):

```cpp
    // Warm-up 128x128 harmonicke matice (off-RT). Lazy init na audio vlakne
    // by pri prvni note kazde session stal stovky ms → deterministicky dropout.
    initHarmonicProximity();
```

- [ ] **Step 1.3:** Run test → PASS. Run celé ctest → PASS.
- [ ] **Step 1.4: Commit** `fix(resonance): warm-up harmonicke matice v Engine::init misto lazy initu na audio vlakne`

---

### Task 2: RT-safe logování v audio cestě + RT logy do subscriberů (review 1.2, KRITICKÁ + H-střední)

**Files:**
- Modify: `engine/util/log.cpp` (`flushRTBuffer`), `engine/util/log.h` (komentář Subscriber API)
- Modify: `engine/engine.cpp` (drain logy, dbg_reso_last), `engine/engine.h` (member)
- Modify: `engine/voice/voice.cpp`, `engine/voice/voice_pool.cpp`
- Test: `tests/test_log.cpp`

- [ ] **Step 2.1: Failing test** — do `tests/test_log.cpp`:

```cpp
TEST_CASE("flushRTBuffer doruci RT zpravy subscriberum (GUI log strip)") {
    auto& L = ithaca::log::Logger::default_();
    L.setMinSeverity(ithaca::log::Severity::Debug);
    std::vector<std::string> got;
    L.addSubscriber([&](const ithaca::log::LogEntry& e) {
        got.push_back(e.topic + ":" + e.message);
    });
    L.logRT("rt_test", ithaca::log::Severity::Info, "hello %d", 42);
    L.flushRTBuffer();
    L.clearSubscribers();
    bool found = false;
    for (auto& s : got) if (s == "rt_test:hello 42") found = true;
    CHECK(found);
}
```

Run → FAIL (subscriber nedostane nic).

- [ ] **Step 2.2:** `log.cpp` — nahraď `flushRTBuffer` (notifikace po dávkách, mimo `log_mutex_`, stejné pořadí zámků jako `vlog`):

```cpp
int Logger::flushRTBuffer() {
    // Davkovy flush: writeEntry pod log_mutex_, notifikace subscriberu az PO
    // uvolneni log_mutex_ (zamky se nikdy nedrzi vnorene; vzor vlog()).
    // Subscribery dostavaji i RT zpravy → GUI log strip vidi underruny/RT stav.
    LogEntry pending[64];
    int flushed = 0;
    for (;;) {
        int batch = 0;
        {
            std::lock_guard<std::mutex> lk(log_mutex_);
            size_t r = rt_read_idx_.load(std::memory_order_relaxed);
            const size_t w = rt_write_idx_.load(std::memory_order_acquire);
            while (r < w && batch < 64) {
                Entry& e = rt_buffer_[r % RT_BUFFER_SIZE];
                writeEntry(e.component, e.severity, e.message, e.timestamp_us);
                pending[batch++] = LogEntry{ (long long)e.timestamp_us,
                                             std::string(e.component), e.severity,
                                             std::string(e.message) };
                ++r;
            }
            rt_read_idx_.store(r, std::memory_order_release);
        }
        if (batch == 0) break;
        flushed += batch;
        if (!subscribers_.empty()) {
            std::lock_guard<std::mutex> lk(subscriber_mtx_);
            for (int i = 0; i < batch; ++i)
                for (auto& sub : subscribers_)
                    if (sub) sub(pending[i]);
        }
    }
    return flushed;
}
```

V `log.h` uprav komentář Subscriber API (ř. ~101–104): RT zprávy JSOU doručovány při `flushRTBuffer()`.

- [ ] **Step 2.3:** Run test → PASS.
- [ ] **Step 2.4:** Konverze audio-thread logů na `LOG_RT_*` (ring; všechna volání běží na audio vlákně = single producer kontrakt dodržen). Producer-side logy (`noteOn`/`noteOff` „fronta plná") zůstávají non-RT (MIDI/GUI vlákno).

`engine.cpp` processBlock — 5 míst:
1. ř. ~300 `log(...)"midi_on"...` → `LOG_RT_INFO("midi_on", "noteOn midi=%d vel=%d ch=%d first=%d asset=%s active_voices=%d", m, v, ch, (int)first, spec.asset ? "yes" : "NULL", pool_->activeCount());`
2. ř. ~321 `"midi_off"` noteOff → `LOG_RT_INFO("midi_off", "noteOff midi=%d ch=%d last=%d release_ms=%.0f cc64=%d sustained=%d", ...)` (stejné argumenty)
3. ř. ~337 `"midi_cc"` → `LOG_RT_INFO("midi_cc", "Sustain CC64=%d", (int)e.data1);`
4. ř. ~353 `"midi_off"` AllNotesOff → `LOG_RT_INFO("midi_off", "AllNotesOff");`
5. ř. ~376 `"resonance"` → `LOG_RT_INFO("resonance", "active rezonance = %d (cc64=%d)", rc, (int)pedal_.sustainCC());` a `static int dbg_reso_last` nahraď memberem: do `engine.h` private přidej `int dbg_reso_count_ = -1;  // audio-thread only (DIAG zmeny poctu rezonanci)` a v kódu používej `dbg_reso_count_`.

`voice.cpp`:
- `log_end` lambda → `LOG_RT_INFO("voice_end", "DEACTIVATE midi=%d reason=%s ...", ...)` (stejný formát; aktualizuj komentář — už není „NE RT-safe").
- `prepareDamp` log (ř. ~81) → `LOG_RT_INFO("voice_end", "DEACTIVATE midi=%d reason=damped_for_retrigger_or_steal damp_len=%d", midi_, damp_len_);`
- END-OF-SAMPLE (ř. ~276) → `LOG_RT_INFO(...)`, UNDERRUN (ř. ~281) → `LOG_RT_WARN(...)`.

`voice_pool.cpp` noteOn debug blok: STEAL → `LOG_RT_WARN("voice_steal", ...)`, voice_on → `LOG_RT_INFO("voice_on", ...)` (stejné argumenty; aktualizuj komentář bloku).

- [ ] **Step 2.5:** Ověř, že CLI `--play` cesta má flush smyčku RT ringu (vzor GUI `main.cpp` log_thr). Pokud chybí, přidej stejný 10ms flush thread do `--play` větve `app/cli/main.cpp`.
- [ ] **Step 2.6:** Build + celé ctest → PASS.
- [ ] **Step 2.7: Commit** `fix(rt): logy v audio hot-path pres LOG_RT_* ring; flushRTBuffer notifikuje subscribery`

---

### Task 3: Osiřelý damping crossfade — pool zpracuje dampující hlasy (review 1.5, VYSOKÁ)

**Files:**
- Modify: `engine/voice/voice.h` (getter), `engine/voice/voice_pool.cpp` (processBlock)
- Test: `tests/test_voice_pool.cpp`

- [ ] **Step 3.1: Failing test** — do `tests/test_voice_pool.cpp`:

```cpp
TEST_CASE("damping crossfade dozni i kdyz novy hlas dostane jiny slot (osirely damp)") {
    // Slot 0: kratka nota, dohraje → volny. Slot 1: dlouha nota B.
    // Retrigger B → prepareDamp na slotu 1, novy hlas jde do slotu 0
    // (findSlot = prvni volny). Damp ocas slotu 1 MUSI doznit (click-free).
    SampleAsset shortA = makeAsset(0.5f, 64);
    SampleAsset longB  = makeAsset(0.5f, 48000);
    VoicePool pool(2);
    VoiceSpec va; va.asset = &shortA; va.pitch_ratio = 1.0; va.vel_gain = 1.0f;
    VoiceSpec vb; vb.asset = &longB;  vb.pitch_ratio = 1.0; vb.vel_gain = 1.0f;
    pool.noteOn(60, va, 48000.f);   // slot 0
    pool.noteOn(72, vb, 48000.f);   // slot 1
    std::vector<float> L(256, 0.f), R(256, 0.f);
    pool.processBlock(L.data(), R.data(), 256, 48000.f);   // A (64 fr) dohraje
    CHECK(pool.activeCount() == 1);
    pool.noteOn(72, vb, 48000.f);   // retrigger B → damp slot 1, novy slot 0
    std::fill(L.begin(), L.end(), 0.f);
    std::fill(R.begin(), R.end(), 0.f);
    pool.processBlock(L.data(), R.data(), 256, 48000.f);
    // Prvni vzorek: novy hlas ma onset ~0 → slysitelny signal dodava JEN damp
    // ocas stareho hlasu (start na plne urovni ~0.5*pan ≈ 0.34).
    CHECK(std::fabs(L[0]) > 0.1f);
}
```

Run → FAIL (`L[0]` ~0.002, damp slot 1 se nikdy nezpracuje).

- [ ] **Step 3.2:** `voice.h` vedle `active()`: `bool  isDamping() const { return damping_; }`

`voice_pool.cpp` processBlock:

```cpp
    for (auto& v : voices_) {
        // Dampujici hlas po prepareDamp je !active(), ale crossfade ocas MUSI
        // doznit i kdyz novy hlas dostal jiny slot (jinak tvrdy strih = lupnuti
        // + "duch" zdedeny pozdejsim noteOn na tomto slotu).
        if (!v.active() && !v.isDamping()) continue;
        if (v.process(out_l, out_r, n_samples)) any = true;
    }
```

- [ ] **Step 3.3:** Run test → PASS; celé ctest → PASS.
- [ ] **Step 3.4: Commit** `fix(voice): damping crossfade dozni i v osirelem slotu (click-free retrigger/steal)`

---

### Task 4: Obálkové opravy Voice (review D-střední ×3 + nízká)

**Files:**
- Modify: `engine/voice/voice.cpp`
- Test: `tests/test_voice_pool.cpp`, `tests/test_streamed_interp.cpp`

- [ ] **Step 4.1: Failing test (release během onsetu)** — `tests/test_voice_pool.cpp`:

```cpp
TEST_CASE("release behem onsetu nezpusobi skok obalky (g0 -> g0*rel, ne g0^2)") {
    SampleAsset a = makeAsset(1.0f, 48000);
    VoicePool pool(2);
    VoiceSpec vs; vs.asset = &a; vs.pitch_ratio = 1.0; vs.vel_gain = 1.0f;
    pool.noteOn(60, vs, 48000.f);
    std::vector<float> L(64, 0.f), R(64, 0.f);
    pool.processBlock(L.data(), R.data(), 64, 48000.f);   // uprostred 3ms onsetu
    const float before = std::fabs(L[63]);
    pool.noteOff(60, 50.f, 48000.f);
    std::vector<float> L2(64, 0.f), R2(64, 0.f);
    pool.processBlock(L2.data(), R2.data(), 64, 48000.f);
    const float after = std::fabs(L2[0]);
    CHECK(after > before * 0.8f);    // spojitost (drive ~0.63x = skok -4 dB)
    CHECK(after < before * 1.2f);
}
```

Run → FAIL.

- [ ] **Step 4.2:** `voice.cpp` `Voice::release`:

```cpp
void Voice::release(float release_ms, float engine_sr) {
    if (!active_ || releasing_) return;
    if (release_ms < 0.1f) release_ms = 0.1f;   // guard: 0 → -inf step
    releasing_ = true;
    // Soucin onset*release je spojity: release startuje VZDY z 1.0 a onset
    // rampa dobiha dal (drive rel_gain_=onset_gain_ → env skok g0 → g0^2).
    rel_gain_  = 1.f;
    rel_step_  = -1.f / (release_ms * 0.001f * engine_sr);
}
```

Run test → PASS.

- [ ] **Step 4.3:** `prepareDamp` — env zahrne onset (klik při stealu během onsetu): za `float env = vel_gain_;` přidej `if (in_onset_)       env *= onset_gain_;`
- [ ] **Step 4.4: Failing test (underrun hold-last)** — `tests/test_streamed_interp.cpp`:

```cpp
TEST_CASE("underrun fade tvaruje drzeny posledni vzorek, ne tvrdy strih do nuly") {
    StreamEngine se(2, 64, 1);                 // workery NEstartujeme → underrun
    SampleAsset a;
    MicLayer m;
    m.file.frames = 100000; m.file.sample_rate = 48000; m.file.valid = true;
    m.mode = MicLayerMode::Streamed;
    m.head_frames = 64;
    m.preload_head.assign(64 * 2, 0.5f);
    a.mics.push_back(std::move(m));
    VoicePool pool(1);
    pool.setStreamEngine(&se);
    VoiceSpec vs; vs.asset = &a; vs.pitch_ratio = 1.0; vs.vel_gain = 1.0f;
    pool.noteOn(60, vs, 48000.f);
    std::vector<float> L(256, 0.f), R(256, 0.f);
    pool.processBlock(L.data(), R.data(), 256, 48000.f);
    // Za headem (frame >64) underrun: 5ms fade drzeneho 0.5 → nenulove vzorky.
    CHECK(std::fabs(L[100]) > 0.05f);
    // Fade dobehne do nuly (64+240 < 512) → druhy blok konci tichem.
    std::vector<float> L2(256, 0.f), R2(256, 0.f);
    pool.processBlock(L2.data(), R2.data(), 256, 48000.f);
    CHECK(std::fabs(L2[255]) == doctest::Approx(0.f));
}
```

Run → FAIL (`L[100] == 0`, fade násobí nuly).

- [ ] **Step 4.5:** `voice.cpp` v underrun větvi (ř. ~287) nahraď `sL = 0.f; sR = 0.f;`:

```cpp
                // Drz posledni znamy vzorek — underrun_gain rampa ho fadne k 0.
                // (Drive sL=sR=0 → fade nasobil nuly = tvrdy strih/klik.)
                sL = ring_lo_l_; sR = ring_lo_r_;
```

Run test → PASS.

- [ ] **Step 4.6:** Totéž v `resonance_voice.cpp` (ř. ~242): `sL = ring_lo_l_; sR = ring_lo_r_;` (stejný komentář).
- [ ] **Step 4.7:** Build + celé ctest → PASS.
- [ ] **Step 4.8: Commit** `fix(voice): spojita obalka pri release-v-onsetu, onset v prepareDamp, underrun fade drzi posledni vzorek`

---

### Task 5: StreamEngine — generation counter ringů + requestRead vrací bool (review 1.3, KRITICKÁ + C-střední)

**Files:**
- Modify: `engine/stream/stream_engine.h`, `engine/stream/stream_engine.cpp`
- Modify: `engine/voice/voice.cpp`, `engine/voice/resonance_voice.cpp` (callery)
- Test: `tests/test_stream_engine.cpp`

- [ ] **Step 5.1: Failing test** — `tests/test_stream_engine.cpp` (pro WAV fixture použij existující helper v souboru; pokud žádný není, vytvoř dočasný WAV přes `writeWavStereo16` jako v ostatních testech):

```cpp
TEST_CASE("stale request po releaseRing nezapise data ani EOF do recyklovaneho ringu") {
    // Fixture: maly stereo WAV (1000 frames, nenulovy obsah).
    const auto path = makeTempWav1000();        // helper viz existujici testy
    StreamEngine se(1, 128, 1);                 // 1 ring; worker zatim NEbezi
    RingHandle* r1 = se.acquireRing();
    REQUIRE(r1 != nullptr);
    CHECK(se.requestRead(r1, path, 0, 500, /*eof_when_done=*/true));
    se.releaseRing(r1);                         // gen bump → request je stale
    RingHandle* r2 = se.acquireRing();          // tentyz slot, nova generace
    REQUIRE(r2 == r1);
    se.start();                                 // worker zpracuje stale request
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    CHECK(r2->available() == 0);                // zadna stara data
    CHECK_FALSE(r2->eof_.load());               // zadny falesny EOF
    CHECK(se.requestRead(r2, path, 0, 100, false));   // novy request projde
    for (int i = 0; i < 100 && r2->available() < 100; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    CHECK(r2->available() >= 100);
    se.stop();
}

TEST_CASE("requestRead vraci false pri plne fronte (drop je viditelny callerum)") {
    StreamEngine se(1, 64, 1);                  // nestartovano → fronta se plni
    RingHandle* r = se.acquireRing();
    int ok = 0;
    for (int i = 0; i < 300; ++i)
        if (se.requestRead(r, "x.wav", 0, 1, false)) ++ok;
    CHECK(ok == 256);                           // kCap
}
```

Run → COMPILE FAIL (`requestRead` je void) — to je očekávaný první fail.

- [ ] **Step 5.2: Implementace** — `stream_engine.h`:

`RingHandle` přidej membery (za `in_use_`):

```cpp
    // Generace vlastnictvi: inkrement pri kazdem releaseRing. StreamRequest
    // nese snapshot — worker pred zapisem overi shodu, takze pozdni/stale
    // request nikdy nezapise data/EOF do ringu noveho vlastnika (ABA guard).
    std::atomic<uint32_t> gen_{0};
    // Producer try-lock (0/1): per-ring smi zapisovat jen jeden worker a
    // acquireRing na nej kratce pocka pred resetem kurzoru. Drzi se JEN pres
    // push (memcpy), ne pres disk I/O → ceka se max ~desitky us.
    std::atomic<int> producers_{0};
```

`StreamRequest` přidej: `uint32_t gen = 0;   // snapshot RingHandle::gen_ pri odeslani`

`requestRead` deklarace: `bool requestRead(RingHandle* ring, const std::string& path, int64_t frame_off, int64_t n_frames, bool eof_when_done = false) noexcept;` + komentář „Vrati false pri plne fronte — caller NEPOSOUVA file_request_off_ (retry pristi blok)."

`stream_engine.cpp`:

```cpp
void StreamEngine::releaseRing(RingHandle* r) {
    if (!r) return;
    // Zneplatni in-flight requesty (worker overuje gen_ pred kazdym zapisem).
    // Kurzory NEresetujeme zde — reset dela az acquireRing po kratke
    // synchronizaci s pripadnym prave probihajicim push (producers_).
    r->gen_.fetch_add(1, std::memory_order_acq_rel);
    r->in_use_.store(false, std::memory_order_release);
}

RingHandle* StreamEngine::acquireRing() {
    for (auto& uptr : rings_) {
        bool expected = false;
        if (uptr->in_use_.compare_exchange_strong(expected, true,
                std::memory_order_acquire, std::memory_order_relaxed)) {
            // Stale push muze prave dobehat (drzi producers_ jen pres memcpy;
            // gen check uvnitr zamku uz dalsi zapis nepusti). Bounded spin.
            for (int spin = 0;
                 uptr->producers_.load(std::memory_order_acquire) != 0
                     && spin < 1000000; ++spin) { /* ~max desitky us */ }
            uptr->resetForReuse();
            return uptr.get();
        }
    }
    return nullptr;
}

bool StreamEngine::requestRead(RingHandle* ring, const std::string& path,
                               int64_t frame_off, int64_t n_frames,
                               bool eof_when_done) noexcept {
    if (!ring) return false;
    StreamRequest req;
    req.ring          = ring;
    req.path          = path;       // kopie (SBO casto staci; FUTURE const char*)
    req.frame_off     = frame_off;
    req.n_frames      = n_frames;
    req.eof_when_done = eof_when_done;
    req.gen           = ring->gen_.load(std::memory_order_acquire);
    // Drop-on-full je RT-safe; false → caller neposune offset a zopakuje.
    return req_q_.push(req);
}
```

`workerLoop` — chunk smyčka (nahraď tělo `while (remain > 0 ...)` a EOF zápis):

```cpp
        while (remain > 0 && run_.load(std::memory_order_acquire)) {
            // Stale request? (ring mezitim uvolnen/preprideleny → gen bump)
            if (req.ring->gen_.load(std::memory_order_acquire) != req.gen) break;
            int free_frames = req.ring->capacity_frames - req.ring->available();
            if (free_frames <= 0) {
                std::this_thread::sleep_for(1ms);
                continue;
            }
            int64_t chunk = (remain < (int64_t)free_frames)
                          ? remain : (int64_t)free_frames;

            WavData data = readWavRange(req.path, off, chunk);
            if (!data.valid) {
                log::Logger::default_().log(
                    "stream", log::Severity::Warning,
                    "readWavRange failed: %s @ %lld", req.path.c_str(),
                    (long long)off);
                break;
            }
            if (data.frames == 0) { eof = true; break; }

            // Producer try-lock + gen re-check: do ringu zapisuje vzdy max
            // jeden worker a nikdy do ringu, ktery uz byl uvolnen (zbytkove
            // okno gen-bumpu BEHEM memcpy kryje acquireRing spinem vyse).
            bool stale = false;
            int  wrote = 0;
            int  exp0  = 0;
            while (!req.ring->producers_.compare_exchange_weak(
                       exp0, 1, std::memory_order_acq_rel,
                       std::memory_order_relaxed)) {
                exp0 = 0;
                std::this_thread::sleep_for(std::chrono::microseconds(100));
                if (!run_.load(std::memory_order_acquire)) { stale = true; break; }
            }
            if (!stale) {
                if (req.ring->gen_.load(std::memory_order_acquire) == req.gen)
                    wrote = req.ring->push(data.samples.data(), data.frames);
                else
                    stale = true;
                req.ring->producers_.store(0, std::memory_order_release);
            }
            if (stale) break;

            off    += wrote;
            remain -= wrote;
            if (data.frames < chunk) { eof = true; break; }
        }

        if (eof && req.eof_when_done &&
            req.ring->gen_.load(std::memory_order_acquire) == req.gen) {
            req.ring->eof_.store(true, std::memory_order_release);
        }
```

- [ ] **Step 5.3:** Callery — `voice.cpp` `start()` (Streamed větev):

```cpp
            const bool pushed = stream_->requestRead(ring_, mic_->file.path,
                                 (int64_t)mic_->head_frames, actual, eof_done);
            if (pushed) {
                file_request_off_ = (int64_t)mic_->head_frames + actual;
                stream_pending_   = true;
            } else {
                // Fronta plna: offset NEposouvat → refill heuristika v process()
                // request prirozene zopakuje.
                file_request_off_ = (int64_t)mic_->head_frames;
            }
```

`voice.cpp` refill heuristika v `process()`:

```cpp
                if (stream_->requestRead(ring_, mic_->file.path,
                                         file_request_off_, want, eof_done)) {
                    file_request_off_ += want;
                    stream_pending_    = true;
                }
                // false → drop; zadny posun offsetu (jinak by se underrun
                // maskoval jako END-OF-SAMPLE a framy se uz nikdy nedozadaly).
```

`resonance_voice.cpp` — analogicky obě místa (`start()`: posun `file_request_off_ = res_end + want` jen při úspěchu, jinak `file_request_off_ = res_end;` bez `stream_pending_`; refill v `process()` stejně jako Voice).

- [ ] **Step 5.4:** Build + oba nové testy → PASS; celé ctest → PASS.
- [ ] **Step 5.5: Commit** `fix(stream): generation counter ringu (ABA guard pro steal/retrigger) + requestRead vraci bool`

---

### Task 6: MidiQueue — MPSC-safe push (review 1.6, VYSOKÁ)

**Files:**
- Modify: `engine/midi/midi_queue.h` (Vyukov bounded queue), `engine/engine.cpp` (drop warningy)
- Test: `tests/test_midi_queue.cpp`

- [ ] **Step 6.1: Failing test** — `tests/test_midi_queue.cpp` (+ `#include <thread>`, `#include <atomic>`):

```cpp
TEST_CASE("MPSC: dva producenti soubezne — zadny event ztracen ani poskozen") {
    MidiQueue q;
    constexpr int N = 20000;
    std::atomic<bool> done_a{false}, done_b{false};
    std::thread ta([&] {
        for (int i = 0; i < N; ++i) {
            MidiEvent e{MidiEvent::NoteOn, (uint8_t)(i & 127), 1, 0};
            while (!q.push(e)) std::this_thread::yield();
        }
        done_a = true;
    });
    std::thread tb([&] {
        for (int i = 0; i < N; ++i) {
            MidiEvent e{MidiEvent::NoteOff, (uint8_t)(i & 127), 2, 1};
            while (!q.push(e)) std::this_thread::yield();
        }
        done_b = true;
    });
    int got_a = 0, got_b = 0, exp_a = 0, exp_b = 0;
    bool ok_order = true, ok_payload = true;
    while (!done_a || !done_b || got_a + got_b < 2 * N) {
        MidiEvent e;
        if (!q.pop(e)) { std::this_thread::yield(); continue; }
        if (e.type == MidiEvent::NoteOn) {
            ok_payload = ok_payload && e.data2 == 1 && e.channel == 0;
            ok_order   = ok_order && (int)e.data1 == (exp_a & 127);
            ++exp_a; ++got_a;
        } else {
            ok_payload = ok_payload && e.data2 == 2 && e.channel == 1;
            ok_order   = ok_order && (int)e.data1 == (exp_b & 127);
            ++exp_b; ++got_b;
        }
    }
    ta.join(); tb.join();
    CHECK(got_a == N);
    CHECK(got_b == N);
    CHECK(ok_order);     // per-producer FIFO
    CHECK(ok_payload);   // zadny torn event
}
```

Run vícekrát (`for i in 1 2 3; do ./build/tests/test_midi_queue; done`) → očekávané občasné FAIL/flaky na stávající SPSC implementaci (ztracené eventy). Pozn.: race je pravděpodobnostní — test po fixu MUSÍ být stabilní, před fixem nemusí selhat na 100 %.

- [ ] **Step 6.2:** `midi_queue.h` — nahraď třídu `MidiQueue` (Vyukov bounded MPMC, použitá jako MPSC; header komentář aktualizuj na „MPSC: producenti = MIDI/GUI/CLI thready, konzument = audio thread"):

```cpp
class MidiQueue {
public:
    static constexpr int MIDI_Q_SIZE = 1024;

    MidiQueue() {
        for (size_t i = 0; i < MIDI_Q_SIZE; ++i)
            cells_[i].seq.store(i, std::memory_order_relaxed);
    }

    // Producent (MIDI/GUI/CLI thread — MULTI-producer safe: CAS claim slotu +
    // per-slot seq publish; Vyukov bounded queue). false = plna (drop).
    // Bez zamku a alokaci; non-RT strana smi spinovat jen pres CAS retry.
    bool push(const MidiEvent& e) {
        size_t pos = w_.load(std::memory_order_relaxed);
        Cell* c;
        for (;;) {
            c = &cells_[pos % MIDI_Q_SIZE];
            const size_t   seq = c->seq.load(std::memory_order_acquire);
            const intptr_t dif = (intptr_t)seq - (intptr_t)pos;
            if (dif == 0) {
                if (w_.compare_exchange_weak(pos, pos + 1,
                                             std::memory_order_relaxed))
                    break;                       // slot claimnut
            } else if (dif < 0) {
                return false;                    // plna → drop
            } else {
                pos = w_.load(std::memory_order_relaxed);
            }
        }
        c->e = e;
        c->seq.store(pos + 1, std::memory_order_release);   // publish
        return true;
    }

    // Konzument (JEDINY — audio thread). false = prazdna NEBO slot jeste
    // nepublikovan (probihajici push → event dorazi pristim pop).
    bool pop(MidiEvent& out) {
        const size_t pos = r_.load(std::memory_order_relaxed);
        Cell* c = &cells_[pos % MIDI_Q_SIZE];
        const size_t seq = c->seq.load(std::memory_order_acquire);
        if ((intptr_t)seq - (intptr_t)(pos + 1) < 0) return false;
        out = c->e;
        c->seq.store(pos + MIDI_Q_SIZE, std::memory_order_release);
        r_.store(pos + 1, std::memory_order_relaxed);
        return true;
    }

private:
    struct Cell {
        std::atomic<size_t> seq{0};
        MidiEvent           e;
    };
    Cell                cells_[MIDI_Q_SIZE];
    std::atomic<size_t> w_{0};
    std::atomic<size_t> r_{0};
};
```

- [ ] **Step 6.3:** `engine.cpp` — drop už není tichý:

```cpp
void Engine::allNotesOff() {
    if (!midi_q_.push({MidiEvent::AllNotesOff, 0, 0}))
        log::Logger::default_().log("midi", log::Severity::Warning,
            "MIDI fronta plna — AllNotesOff ZAHOZEN");
}
void Engine::sustainPedal(uint8_t cc) {
    if (!midi_q_.push({MidiEvent::Sustain, cc, 0}))
        log::Logger::default_().log("midi", log::Severity::Warning,
            "MIDI fronta plna — Sustain CC64=%d ZAHOZEN", (int)cc);
}
```

- [ ] **Step 6.4:** Run test_midi_queue 5× → vždy PASS; celé ctest → PASS.
- [ ] **Step 6.5: Commit** `fix(midi): MidiQueue prepnuta na MPSC-safe Vyukov queue; drop AllNotesOff/Sustain se loguje`

---

### Task 7: reloadBank handshake přes block_epoch_ (review 1.4a, VYSOKÁ)

**Files:**
- Modify: `engine/engine.h`, `engine/engine.cpp`
- Test: `tests/test_engine.cpp`

- [ ] **Step 7.1: Failing test** — `tests/test_engine.cpp`:

```cpp
TEST_CASE("processBlock inkrementuje block epoch (podklad reload/recache handshake)") {
    Engine e;
    EngineConfig cfg; cfg.sample_rate = 48000; cfg.block_size = 64;
    REQUIRE(e.init(cfg));
    const uint64_t e0 = e.blockEpoch();
    std::vector<float> L(64, 0.f), R(64, 0.f);
    e.processBlock(L.data(), R.data(), 64);
    e.processBlock(L.data(), R.data(), 64);
    CHECK(e.blockEpoch() == e0 + 2);
}
```

Run → COMPILE FAIL (`blockEpoch` neexistuje).

- [ ] **Step 7.2:** `engine.h` — public getter + private membery:

```cpp
    // Pocitadlo zapocatych audio bloku (diagnostika + reload/recache handshake).
    uint64_t blockEpoch() const noexcept {
        return block_epoch_.load(std::memory_order_seq_cst);
    }
```

private: `std::atomic<uint64_t> block_epoch_{0};` a deklarace `void waitForAudioQuiesce(int min_epochs, int timeout_ms) noexcept;`

`engine.cpp` processBlock — hned za `if (!initialized_ || !pool_) return;`:

```cpp
    // Epoch tick: zapocaty blok. reloadBank/recache cekaji na epoch+2 = "in-flight
    // blok dobehl a dalsi uz vidi aktualni flagy" (nahrazuje sleep heuristiku,
    // ktera neplatila pro block_size az 8192 ≈ 170 ms periody).
    block_epoch_.fetch_add(1, std::memory_order_seq_cst);
```

Helper (vedle reloadBank):

```cpp
void Engine::waitForAudioQuiesce(int min_epochs, int timeout_ms) noexcept {
    // Pocka, az audio thread ZAPOCNE aspon min_epochs novych bloku — tj.
    // pripadny in-flight blok dobehl a nasledujici uz cetl aktualni atomic
    // flagy (bank_loading_/fade request). Timeout kryje zastavene audio
    // (testy, odpojene zarizeni) — pak je mutace trivialne bezpecna.
    const uint64_t e0 = block_epoch_.load(std::memory_order_seq_cst);
    const auto deadline = std::chrono::steady_clock::now()
                        + std::chrono::milliseconds(timeout_ms);
    while (block_epoch_.load(std::memory_order_seq_cst)
               < e0 + (uint64_t)min_epochs) {
        if (std::chrono::steady_clock::now() >= deadline) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}
```

`reloadBank` krok 4 — nahraď `std::this_thread::sleep_for(std::chrono::milliseconds(10));`:

```cpp
    // 4) Handshake misto pevneho sleepu: epoch+2 garantuje dobeh in-flight
    //    bloku + ze dalsi blok videl bank_loading_ (vraci ticho). Timeout 500 ms
    //    pokryva i block_size 8192 (~170 ms perioda) a stojici audio.
    waitForAudioQuiesce(2, 500);
```

Aktualizuj komentář kroků v `engine.h` u `reloadBank` (bod 4: handshake místo „~10 ms").

- [ ] **Step 7.3:** Run test → PASS; celé ctest → PASS (pozor: testy volající `reloadBank` bez běžícího audia nyní čekají timeout 500 ms — ověř, že ctest nezpomalil neúnosně; případně u testů s reloadem tickni processBlock z testu).
- [ ] **Step 7.4: Commit** `fix(engine): reloadBank pres block-epoch handshake misto 10ms sleep (UAF pri velkych blocich)`

---

### Task 8: Recache stavový automat + fade před každou iterací (review 1.4b/c, VYSOKÁ + F-nízká lost-update)

**Files:**
- Modify: `engine/engine.h`, `engine/engine.cpp`
- Modify: `engine/voice/resonance_voice.cpp` (stream-mode nečte resonance_frames)
- Test: `tests/test_resonance_cache_build.cpp`

- [ ] **Step 8.1: Failing test** — `tests/test_resonance_cache_build.cpp`:

```cpp
TEST_CASE("rebuildResonanceCache: coalesce neztrati pending a rebuild dobehne") {
    ithaca::Engine e;
    ithaca::EngineConfig cfg; cfg.sample_rate = 48000; cfg.block_size = 64;
    REQUIRE(e.init(cfg));
    e.rebuildResonanceCache(-25.f);
    e.rebuildResonanceCache(-20.f);   // okamzite znovu → pending/coalesce
    bool done = false;
    std::vector<float> L(64, 0.f), R(64, 0.f);
    for (int i = 0; i < 500 && !done; ++i) {
        e.processBlock(L.data(), R.data(), 64);   // tick epoch (quiesce handshake)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        done = !e.recacheInProgress();
    }
    CHECK(done);
}
```

Run → COMPILE FAIL (`recacheInProgress` neexistuje).

- [ ] **Step 8.2:** `engine.h` — nahraď `recache_running_`/`recache_has_pending_` atomiky:

```cpp
    std::thread          recache_thread_;
    mutable std::mutex   recache_mtx_;        // chrani running/pending (non-RT)
    bool                 recache_running_ = false;
    bool                 recache_pending_ = false;
    std::atomic<float>   recache_target_{-30.f};
```

public: `bool recacheInProgress() const noexcept;` (+ `#include <mutex>`).

`engine.cpp`:

```cpp
bool Engine::recacheInProgress() const noexcept {
    std::lock_guard<std::mutex> lk(recache_mtx_);
    return recache_running_;
}

void Engine::rebuildResonanceCache(float target_db) noexcept {
    if (!resonance_) return;
    cfg_.resonance_layer_db = target_db;
    recache_target_.store(target_db, std::memory_order_release);
    resonance_->setLayerTargetDb(target_db);
    {
        std::lock_guard<std::mutex> lk(recache_mtx_);
        if (recache_running_) {
            // Bezici rebuild si pending vyzvedne POD TYMZE mutexem, kterym
            // shazuje running → lost-update z revize (pending nastaveny tesne
            // po exchange workeru) uz nemuze nastat.
            recache_pending_ = true;
            return;
        }
        recache_running_ = true;
    }
    if (recache_thread_.joinable()) recache_thread_.join();   // predchozi dobehl
    try {
        recache_thread_ = std::thread([this]() {
            for (;;) {
                const float t = recache_target_.load(std::memory_order_acquire);
                // KAZDA iterace (i coalesce — drive chybelo!): shod ready flagy
                // (nove spawny jedou stream modem, preload_resonance nectou)
                // + fade aktivnich hlasu; pak pockej na konzumaci requestu
                // audio threadem (epoch+2) a dobeh 5ms fade. Teprve pak je
                // realokace preload_resonance bezpecna.
                resonance_->clearCacheReady();
                resonance_->requestRecacheFade();
                waitForAudioQuiesce(2, 500);
                std::this_thread::sleep_for(std::chrono::milliseconds(15));
                auto ready = ithaca::buildResonanceCache(
                    bank_, t, cfg_.resonance_window_ms, log::Logger::default_());
                if (resonance_) resonance_->setCacheReady(ready);
                std::lock_guard<std::mutex> lk(recache_mtx_);
                if (recache_pending_) { recache_pending_ = false; continue; }
                recache_running_ = false;
                break;
            }
        });
    } catch (...) {
        // Spawn vlakna selhal (extremni OOM) — vrat stav, cache zustane stream-mode.
        std::lock_guard<std::mutex> lk(recache_mtx_);
        recache_running_ = false;
    }
}
```

`reloadBank` — místo `recache_running_.store(false, ...)`:

```cpp
    if (recache_thread_.joinable()) recache_thread_.join();
    {
        std::lock_guard<std::mutex> lk(recache_mtx_);
        recache_running_ = false;
        recache_pending_ = false;
    }
```

- [ ] **Step 8.3:** `resonance_voice.cpp` `start()` — viability check nesmí ve stream módu sahat na pole přepisovaná rebuildem:

```cpp
    // Viabilita: cache mod cte preload_resonance (resonance_frames), stream mod
    // streamuje od resonance_start_frame → rozhoduje file.frames. Behem cache
    // rebuilu (use_cache=false) se resonance_frames NESMI cist (bg thread ho
    // prave prepisuje).
    const int eff_res_frames0 = (mic_ && use_cache_) ? mic_->resonance_frames : 0;
    active_ = (mic_ != nullptr) &&
              (mic_->mode == MicLayerMode::FullyLoaded
                   ? mic_->head_frames > mic_->resonance_start_frame
                   : (eff_res_frames0 > 0 ||
                      (int64_t)mic_->file.frames
                          > (int64_t)mic_->resonance_start_frame));
```

- [ ] **Step 8.4:** Build + test → PASS; celé ctest → PASS.
- [ ] **Step 8.5: Commit** `fix(resonance): recache stavovy automat pod mutexem, fade+clear v kazde iteraci, stream-mode necte resonance_frames`

---

### Task 9: ResonanceEngine — budget gate, decay atomic, prealloc, pořadí eligibility (review 1.8 + E-střední/nízká)

**Files:**
- Modify: `engine/voice/resonance_voice.h` (targetGain getter, oprava komentáře)
- Modify: `engine/resonance/resonance_engine.h`, `engine/resonance/resonance_engine.cpp`
- Test: `tests/test_resonance_engine.cpp`

- [ ] **Step 9.1: Failing test** — `tests/test_resonance_engine.cpp` (banku postav podle existujících helperů v tom souboru; pokud chybí, vytvoř lokální fixture: `Bank` s `notes[N].recorded=true` a jedním FullyLoaded slotem na notách 60–84 — vzor `makeAsset` z test_voice_pool):

```cpp
TEST_CASE("budget gate pri plnem rozpoctu: prezije nejsilnejsi harmonika, zadny spawn-churn") {
    // cap=1, hrana nota 48 budi vice harmonik (60=oktava je nejsilnejsi).
    // Drive: gate srovnaval init_gain s currentLevel()==0 cerstvych spawnu →
    // kazda dalsi harmonika fadla predchozi a prezila POSLEDNI (nejvyssi N).
    Bank bank = makeBankWithNotes({60, 67, 72, 76, 79, 84});   // fixture
    ResonanceEngine re(/*max_voices=*/1);
    PedalState pedal; pedal.setSustainCC(127);
    VoicePool pool(4);
    re.onPlayedNoteOn(48, 100, bank, pool, pedal, 48000.f);
    CHECK(re.isResonating(60));          // oktava (harm ~1.0) musi prezit
    int live_or_fading = 0;
    for (int n = 0; n < 128; ++n) if (re.isResonating(n)) ++live_or_fading;
    CHECK(live_or_fading <= 2);          // max 1 zivy + max 1 dohasinajici
}
```

Run → FAIL (přežije nejvyšší N, ne 60).

- [ ] **Step 9.2:** `resonance_voice.h` vedle `currentLevel()`:

```cpp
    // Cilovy gain (rampa k nemu tece). Budget gate v ResonanceEngine porovnava
    // max(currentLevel, targetGain) — cerstve spawnuty hlas ma gain 0, ale
    // target uz plny → bez toho gate fadl prave spawnle hlasy (spawn-churn).
    float targetGain()   const noexcept { return target_gain_; }
```

Zároveň oprav lživý komentář u `recomputeGainStep` (smaž větu „Pri is_fading_out_ se nepocita znovu...", guard neexistuje).

- [ ] **Step 9.3:** `resonance_engine.h`: `float decay_per_block_` → `std::atomic<float> decay_per_block_{0.998f};`; getter `exciteDecayPerBlock()` → `return decay_per_block_.load(std::memory_order_relaxed);`. V `.cpp` `setExciteDecayTimeMs` počítej do lokální proměnné a `decay_per_block_.store(d, std::memory_order_relaxed);`; v `processBlock` načti jednou: `const float decay = decay_per_block_.load(std::memory_order_relaxed);`.
- [ ] **Step 9.4:** Konstruktor — prealloc (žádný malloc v RT cestě):

```cpp
ResonanceEngine::ResonanceEngine(int max_resonance_voices) {
    // Predalokace vsech 128 hlasu. (Drive lazy make_unique v onPlayedNoteOn =
    // malloc na audio vlakne.)
    for (auto& v : voices_) v = std::make_unique<ResonanceVoice>();
    setMaxVoices(max_resonance_voices);
}
```

V `onPlayedNoteOn` smaž `if (!slot) slot = std::make_unique<ResonanceVoice>();` (ponech `slot->setStreamEngine(stream_);`).

- [ ] **Step 9.5:** `onPlayedNoteOn` — pořadí eligibility (levné O(1) testy před O(pool) scanem) + práh na init_gain:

```cpp
    for (int N = 0; N < 128; ++N) {
        if (N == played_midi) continue;              // play-on-self
        // Levne testy nejdriv: harmonicka blizkost je O(1) lookup a vyradi
        // ~90 % not; drahy pool scan (hasActiveMainVoice) az nakonec.
        const float harm = harmonicProximity(N, played_midi);
        if (harm < kResonanceHarmonicMin) continue;
        const float excite = vel_norm * harm * gain;
        if (excite < kResonanceExciteMinGain) continue;
        if (!pedal.isUndamped(N)) continue;          // damping <= eps
        auto& slot = voices_[(size_t)N];
        if (slot->active() && slot->fadingOut()) continue;  // rule B in-progress
        if (pool.hasActiveMainVoice(N)) continue;    // main voice existuje
        ...
```

a za `const float init_gain = excite * pedal.dampingFor(N);`:

```cpp
        // Half-pedal: damping muze srazit init_gain hluboko pod slysitelnost —
        // nealokuj ring + diskova cteni pro hlas, ktery nikdo neuslysi.
        if (init_gain < kResonanceExciteMinGain) continue;
```

- [ ] **Step 9.6:** Budget gate — obě porovnání přes `max(level, target)`:

```cpp
                const float lvl = std::max(s->currentLevel(), s->targetGain());
```

(v quietest scanu) a komentář gate doplň: srovnává se cíl vs. max(okamžitý, cíl) — čerstvé spawny už nejsou „nulové" oběti.

- [ ] **Step 9.7:** Build + test → PASS; celé ctest → PASS.
- [ ] **Step 9.8: Commit** `fix(resonance): budget gate pres targetGain, atomic decay, prealloc hlasu, levne eligibility testy driv`

---

### Task 10: Convolver — IR swap potvrzovaný audio vláknem (review 1.7, VYSOKÁ)

**Files:**
- Modify: `engine/dsp/convolver.h`, `engine/dsp/convolver.cpp`
- Test: `tests/test_convolver.cpp`

- [ ] **Step 10.1: Test (smoke + neblokování)** — `tests/test_convolver.cpp` (+ `<thread>`, `<chrono>`):

```cpp
TEST_CASE("setIR bez beziciho audia neblokuje (seen sentinel)") {
    using namespace ithaca::dsp;
    Convolver cv;
    cv.prepare(48000.f, 256);
    const auto t0 = std::chrono::steady_clock::now();
    std::vector<float> ir(64, 0.1f);
    for (int i = 0; i < 10; ++i) cv.setIR(ir);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();
    CHECK(ms < 100);
    CHECK(cv.irLength() == 64);
}

TEST_CASE("soubezny setIR + process: zadny crash, konecny vystup (smoke)") {
    using namespace ithaca::dsp;
    Convolver cv;
    cv.prepare(48000.f, 256);
    cv.setEnabled(true);
    cv.set(0, 1.f);   // MIX=1 → plna konvoluce
    std::atomic<bool> run{true};
    std::thread audio([&] {
        std::vector<float> L(256, 0.5f), R(256, 0.5f);
        while (run.load()) {
            cv.process(L.data(), R.data(), 256);
            for (float v : L) { if (!std::isfinite(v)) { run = false; FAIL("non-finite"); } }
        }
    });
    std::vector<float> ir1(512, 0.2f), ir2(2048, 0.05f);
    for (int i = 0; i < 500; ++i) cv.setIR((i & 1) ? ir1 : ir2);
    run = false;
    audio.join();
    CHECK(true);
}
```

Run → PASS i před fixem (smoke); skutečnou verifikaci dodá TSan v Task 13. Commit testů společně s fixem.

- [ ] **Step 10.2:** `convolver.h` private: `std::atomic<int> seen_{-1};   // slot prave cteny v process(); -1 = audio necte`

`convolver.cpp` `prepare()`: přidej `seen_.store(-1, std::memory_order_relaxed);`

`process()` — publikace čteného slotu (a úklid na VŠECH návratových cestách):

```cpp
void Convolver::process(float* L, float* R, int n) {
    if (!enabled_.load(std::memory_order_relaxed)) return;
    const int active = active_.load(std::memory_order_acquire);
    // Oznam GUI threadu cteny slot — setIR neprepise slot pod in-flight blokem.
    seen_.store(active, std::memory_order_release);
    const auto& ir = ir_[(size_t)active];
    const int M = (int)ir.size();
    if (M == 0) { seen_.store(-1, std::memory_order_release); return; }
    ...
    if (wet < 1e-4f) {
        ...early-out smycka...
        seen_.store(-1, std::memory_order_release);
        return;
    }
    ...hlavni smycka...
    seen_.store(-1, std::memory_order_release);
}
```

`setIR` — vyčkej, dokud audio čte cílový slot (+ `#include <chrono>`, `#include <thread>`):

```cpp
void Convolver::setIR(const std::vector<float>& ir) {
    const int inactive = 1 - active_.load(std::memory_order_relaxed);
    // Dve publikace behem jednoho in-flight process() driv prepsaly slot pod
    // rukama audio threadu (use-after-free vectoru). Pockej na konec bloku
    // (seen_ != inactive); bounded ~1 perioda bloku, timeout pro stojici audio.
    const auto deadline = std::chrono::steady_clock::now()
                        + std::chrono::milliseconds(250);
    while (seen_.load(std::memory_order_acquire) == inactive &&
           std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
    std::vector<float> tmp = ir;
    if ((int)tmp.size() > kMaxIr) tmp.resize(kMaxIr);
    float peak = 0.f; for (float v : tmp) peak = std::max(peak, std::fabs(v));
    if (peak > 1e-9f) { const float g = 1.f/peak; for (auto& v : tmp) v *= g; }
    ir_[(size_t)inactive] = std::move(tmp);
    active_.store(inactive, std::memory_order_release);
}
```

Aktualizuj header komentář (`convolver.h` ř. 3: „RT-safe 2-slot IR swap" → doplnit „s ack od audio threadu (seen_)").

- [ ] **Step 10.3:** Build + testy → PASS.
- [ ] **Step 10.4: Commit** `fix(dsp): convolver IR swap ceka na opusteni slotu audio threadem (latentni UAF)`

---

### Task 11: RT priorita opt-in + drobné RT/bezpečnostní opravy (review A-střední/nízká, C-nízká)

**Files:**
- Modify: `engine/engine.h` (EngineConfig), `engine/engine.cpp` (processBlock, noteOn/Off clamp)
- Modify: `engine/midi/midi_input.h` (atomic channel), `engine/midi/midi_input.cpp` (callback local load)
- Modify: `app/gui/app_context.cpp` (rt_priority, scratch prealloc, setChannel před open)
- Modify: `app/cli/main.cpp` (rt_priority u --play, scratch prealloc)
- Modify: `app/gui/main.cpp` (failure path shutdown)
- Test: `tests/test_engine.cpp`

- [ ] **Step 11.1: Failing test** — `tests/test_engine.cpp`:

```cpp
TEST_CASE("noteOn/noteOff s out-of-range parametry jsou bezpecne (clamp/zahozeni)") {
    Engine e;
    EngineConfig cfg; cfg.sample_rate = 48000; cfg.block_size = 64;
    REQUIRE(e.init(cfg));
    e.noteOn(200, 100);    // midi mimo rozsah → zahodit
    e.noteOn(-3, 100);
    e.noteOn(60, 300);     // velocity > 127 → clamp (drive pretek na 44/0!)
    e.noteOff(-5);
    std::vector<float> L(64, 0.f), R(64, 0.f);
    e.processBlock(L.data(), R.data(), 64);   // nesmi spadnout / UB
    CHECK(true);
}
```

(Hodnotový efekt chrání ASan/UBSan běh v Task 13; tady jde o kontrakt.)

- [ ] **Step 11.2:** `engine.h` `EngineConfig` přidej:

```cpp
    // RT priorita audio vlakna (SCHED_FIFO/time-constraint/MMCSS) — nastavi se
    // pri prvnim processBlock NA VOLAJICIM VLAKNE. Default OFF: zapinaji jen
    // realne audio aplikace (GUI, CLI --play). Jinak by testy/offline render
    // dostaly SCHED_FIFO na main threadu (vyhladoveni systemu, RLIMIT_RTTIME).
    bool  rt_priority = false;
```

`engine.cpp` processBlock — obal RT blok: `if (!rt_set) { if (cfg_.rt_priority) { ...stavajici switch... } rt_set = true; }`

noteOn/noteOff clamp:

```cpp
void Engine::noteOn(int midi, int velocity, int channel) {
    if (midi < 0 || midi > 127) return;          // (uint8_t)300 by hral jinou notu
    if (velocity > 127) velocity = 127;          // (uint8_t)256==0 → falesny NoteOff
    if (channel < 0 || channel > 15) channel = 0;
    if (velocity <= 0) { noteOff(midi, channel); return; }
    ...
void Engine::noteOff(int midi, int channel) {
    if (midi < 0 || midi > 127) return;
    if (channel < 0 || channel > 15) channel = 0;
    ...
```

- [ ] **Step 11.3:** `midi_input.h`: `#include <atomic>`; `int channel_ = -1;` → `std::atomic<int> channel_{-1};`; `setChannel`: `channel_.store((ch < 0 || ch > 15) ? -1 : ch, std::memory_order_relaxed);`; `channel()`: `return channel_.load(std::memory_order_relaxed);`. V `midi_input.cpp` callbacku načti jednou: `const int chan = self->channel_.load(std::memory_order_relaxed);` a používej `chan` v obou `channelAccepts` voláních.
- [ ] **Step 11.4:** `app_context.cpp`:
  - `cfg.rt_priority = true;` (u sestavení EngineConfig),
  - scratch prealloc: `static std::vector<float> L(8192), R(8192);` (guard `if ((uint32_t)L.size() < frames)` ponech pro jistotu, s komentářem „nemelo by nastat — 8192 je engine max"),
  - MIDI: `midi.setChannel(state.midi_channel);` PŘED `midi.open(...)` (smaž následné setChannel).

  `app/cli/main.cpp`: v `playAudioCb` stejný prealloc 8192; v `--play` větvi `cfg.rt_priority = true;` (najdi sestavení EngineConfig pro play).

  `app/gui/main.cpp` failure větev `initFromState`: přidej `ctx.shutdown();` před ImGui teardown (jinak v Logger singletonu zůstane subscriber s visícím `this`).
- [ ] **Step 11.5:** Build + testy → PASS.
- [ ] **Step 11.6: Commit** `fix(rt): RT priorita opt-in pres EngineConfig; atomic MIDI channel; clamp noteOn/Off; prealloc audio scratch`

---

### Task 12: Persistence + GUI opravy (review H-střední/nízká)

**Files:**
- Modify: `app/gui/persistence.cpp`
- Modify: `app/gui/panel_log.cpp`, `app/gui/panel_topbar.cpp`
- Test: `tests/test_persistence.cpp`

- [ ] **Step 12.1: Failing testy** — `tests/test_persistence.cpp` (vzor zápisu temp souboru viz existující testy v souboru):

```cpp
TEST_CASE("poskozena numericka hodnota nezahodi cely stav (bank_path prezije)") {
    const auto p = tempStatePath("corrupt");   // helper dle existujicich testu
    {
        std::ofstream f(p);
        f << "{\n  \"schema_version\": 4,\n  \"bank_path\": \"/moje/banka\",\n"
             "  \"master_gain_db\": abc,\n  \"window_w\": 1280\n}\n";
    }
    auto st = ithaca::gui::loadState(p);
    REQUIRE(st.has_value());
    CHECK(st->bank_path == "/moje/banka");
    CHECK(st->master_gain_db == doctest::Approx(ithaca::gui::GuiState{}.master_gain_db));
}

TEST_CASE("window geometrie se sanitizuje (0x0 z minimalizovaneho okna nezabije start)") {
    const auto p = tempStatePath("geom");
    {
        std::ofstream f(p);
        f << "{\n  \"schema_version\": 4,\n  \"window_w\": 0,\n  \"window_h\": -5\n}\n";
    }
    auto st = ithaca::gui::loadState(p);
    REQUIRE(st.has_value());
    CHECK(st->window_w >= 320);
    CHECK(st->window_h >= 240);
}
```

Run → FAIL (první vrací nullopt, druhý 0/-5).

- [ ] **Step 12.2:** `persistence.cpp` `loadState` — přesuň `readF/readB/readI` lambdy NAD první použití a obal je try/catch:

```cpp
        auto readF = [&](const char* k, float dv) {
            std::string v = findValue(json, k);
            if (v.empty()) return dv;
            try { return std::stof(v); } catch (...) { return dv; }
        };
        auto readB = [&](const char* k, bool dv) {
            std::string v = findValue(json, k);
            return v.empty() ? dv : (v == "true" || v == "1");
        };
        auto readI = [&](const char* k, int dv) {
            std::string v = findValue(json, k);
            if (v.empty()) return dv;
            try { return std::stoi(v); } catch (...) { return dv; }
        };
```

a 9 striktních polí přepiš defenzivně (schema_version nech striktní — chybějící/cizí schema = nullopt):

```cpp
        s.midi_channel = readI("midi_channel", -1);
        if (s.midi_channel < -1 || s.midi_channel > 15) s.midi_channel = -1;
        s.master_gain_db       = readF("master_gain_db", s.master_gain_db);
        s.release_ms           = readF("release_ms", s.release_ms);
        s.excite_decay_ms      = readF("excite_decay_ms", s.excite_decay_ms);
        s.max_resonance_voices = readI("max_resonance_voices", s.max_resonance_voices);
        s.window_x = readI("window_x", s.window_x);
        s.window_y = readI("window_y", s.window_y);
        s.window_w = readI("window_w", s.window_w);
        s.window_h = readI("window_h", s.window_h);
        // Sanitizace geometrie: minimalizovane okno na Windows uklada 0x0 a
        // glfwCreateWindow(0,0) pri pristim startu selze → app nejde spustit.
        if (s.window_w < 320) s.window_w = 1280;
        if (s.window_h < 240) s.window_h = 720;
```

(vnější `try/catch` ponech jako poslední pojistku).

- [ ] **Step 12.3:** `saveState` — kontrola zápisu před rename:

```cpp
        f << "}\n";
        f.flush();
        if (!f.good()) {
            // Plny disk / IO chyba: NIKDY neprepisuj dobry config torzem.
            f.close();
            std::error_code rec;
            std::filesystem::remove(tmp, rec);
            return false;
        }
    }
    std::filesystem::rename(tmp, path, ec);
```

- [ ] **Step 12.4:** `panel_log.cpp`: `if (e.sev == log::Severity::Error) col = ...` → `if ((int)e.sev >= (int)log::Severity::Error) col = IM_COL32(0xd0,0x5a,0x4a,255);` (Error i Fatal červeně; komentář v hlavičce souboru aktualizuj).
- [ ] **Step 12.5:** `panel_topbar.cpp` — cache MIDI portů (konstrukce RtMidi klienta @60 Hz byla nejdražší per-frame operace GUI):

```cpp
    // MIDI IN dropdown + RESCAN. Seznam portu je CACHOVANY — listPorts()
    // konstruuje RtMidi klienta (OS IPC). Rescan: prvni frame, otevreni comba,
    // tlacitko RESCAN.
    static std::vector<std::string> ports;
    static bool ports_scanned  = false;
    static bool combo_was_open = false;
    if (!ports_scanned) { ports = ithaca::MidiInput::listPorts(); ports_scanned = true; }
```

uvnitř `if (ImGui::BeginCombo("##midi", cur)) {` jako první řádek:

```cpp
        if (!combo_was_open) { ports = ithaca::MidiInput::listPorts(); combo_was_open = true; }
```

za `ImGui::EndCombo(); }` přidej `else combo_was_open = false;` a tlačítko:

```cpp
    if (ImGui::Button("RESCAN##reload")) ports = ithaca::MidiInput::listPorts();
```

(smaž starý komentář „listPorts() se vola kazdy frame").

- [ ] **Step 12.6:** Build (vč. ithaca-gui) + testy → PASS.
- [ ] **Step 12.7: Commit** `fix(gui): defenzivni load + bezpecny save state.json, sanitizace geometrie, cache MIDI portu, Fatal cervene`

---

### Task 13: Verifikace — plné testy + sanitizery

- [ ] **Step 13.1:** `cmake --build build -j && ctest --test-dir build --output-on-failure` → vše PASS.
- [ ] **Step 13.2:** ASan/UBSan: `cmake --build build-asan -j && ctest --test-dir build-asan --output-on-failure` → vše PASS, žádný report.
- [ ] **Step 13.3:** TSan: `cmake --build build-tsan -j && ctest --test-dir build-tsan --output-on-failure` — cíleně sleduj `test_midi_queue` (MPSC stress), `test_stream_engine` (gen guard), `test_convolver` (swap smoke) → žádný data race report. Pozn.: build-asan/build-tsan jsou již nakonfigurované (ITHACA_SANITIZE) — jen rebuild.
- [ ] **Step 13.4:** Pokud sanitizery odhalí nález v měněném kódu → oprav v rámci větve (samostatný commit). Nálezy mimo rozsah jen zaznamenej do review dokumentu.
- [ ] **Step 13.5: Commit** případných oprav.

---

### Task 14: Dokumentace do souladu s kódem (review §3 + memory „keep reference docs current")

**Files:** `docs/reference/A-core.md`, `B-events.md`, `C-buffers.md`, `D-polyphony.md`, `E-resonance.md`, `F-loader.md`, `G-dsp.md`, `H-gui.md`, `I-multithreading.md`, `00-overview.md`, `docs/config-file.md`, `docs/bank-format-legacy.md`, `docs/rt-thread-priority.md`, `docs/review-2026-06-10.md`; komentáře v kódu: `engine/sample/sample_store.h`, `engine/midi/midi_input.h`(hlavička), `engine/stream/stream_engine.h` (ř. 42–44 EOF), `engine/voice/voice.h` (ř. 111 „hold last sample"), `app/gui/persistence.h` (ř. 41), `engine/voice/patch_manager.h` (ř. 26), `engine/voice/voice_pool.cpp` (ř. 13 komentář C4).

- [ ] **Step 14.1:** Oprav per-dokument rozpory vyjmenované v review §3 — A: scaledReleaseMs/AllNotesOff, RT-priority krok v processBlock, CLI volby `--resonance`/`--resonance-layer`, drop-warningy; B: pořadí setChannel/open (nyní opraveno v kódu — popsat nový stav), MPSC fronta; C: nový gen-guard protokol, EOF cesta Voice (hold-last + fade), auto-sizing workerů, n_rings 256/48, threshold clamp; D: damping dozní v každém slotu (nový stav), underrun hold-last, requestRead bool; E: warm-up matice v init, budget gate přes targetGain, `Engine::processBlock` (ne processNoteQueue), doplnit resonance_layer_select do tabulky, recache handshake; F: window default 12000, scanBank výjimky (pozn. zůstává — kód neměněn), uzavřít Nález 2 (vyvráceno); G: 4 stage včetně convolveru, MIX default 0,15, baseline ořezu −16 % z 7200, doplnit seen_ ack; H: 6 stránek, stage(0..3), 30 polí debounce, okno 720, RT logy nyní v GUI stripu, MIDI cache, uzavřít Nález #4 (vyvráceno); I: doplnit recache vlákno + block_epoch_ handshake, opravit `strength_`→`gain_lin_`, aktualizovat P1 (vyřešeno).
- [ ] **Step 14.2:** `docs/config-file.md`: max_resonance_voices NENÍ init-only (živý slider); debounce = 1 s od první změny (ne idle). `docs/bank-format-legacy.md`: banner → „HISTORICKÝ — popisuje stav před dynamic-velocity; aktuální chování viz F-loader.md". `docs/rt-thread-priority.md`: hlavička „Status: implementováno" + zmínka o `EngineConfig::rt_priority` opt-in.
- [ ] **Step 14.3:** Komentáře v kódu z výčtu výše (krátké opravy lživých vět).
- [ ] **Step 14.4:** `docs/review-2026-06-10.md`: ke každému opravenému nálezu připiš `→ OPRAVENO (fix/revize-2026-06-10)`, k odloženým `→ ODLOŽENO (mimo rozsah větve)`.
- [ ] **Step 14.5: Commit** `docs: uvedeni reference docs + config/bank docs do souladu s kodem po mitigacich`

---

## Self-review

- Pokrytí review §5: 5.1 (Task 1+2), 5.2 (Task 3), 5.3 (Task 5), 5.4 (Task 7+8), 5.5 (Task 6+9+10), 5.6 částečně (Task 12; async reload odložen), 5.7 odloženo, 5.8 (Task 14). Obálkové opravy navíc (Task 4) řeší slyšitelné D-nálezy.
- Typová konzistence: `requestRead` → bool používán v Task 5 callerech; `isDamping()`/`targetGain()`/`blockEpoch()`/`recacheInProgress()` definovány tam, kde se používají.
- Testy se přidávají jen do existujících binárek → CMake beze změn.
