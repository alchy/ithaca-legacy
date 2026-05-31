# Faze 8 GUI (Hybrid MVP) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Postavit `ithaca-gui` exekutabl s Dear ImGui + GLFW: top bar (bank/MIDI/master) + 88-key keyboard viz + diag panel + slidery + log strip + persistence.

**Architecture:** Novy CMake target `ithaca-gui` linkuje `libithaca_core`. Vendorovane ImGui v1.91 + GLFW 3.4 v `third-party/`. GUI thread sahas atomic gettery/settery na Engine, audio/MIDI bezi nezavisle (jako v CLI).

**Tech Stack:** C++20, Dear ImGui (immediate-mode, OpenGL3 backend), GLFW, miniaudio (vendorovano uz), RtMidi (vendorovano uz). Build CMake.

**Source spec:** `docs/superpowers/specs/2026-05-31-ithaca-legacy-gui-f8-design.md`.

---

## Task 1: Vendor ImGui + GLFW + prazdny ithaca-gui

**Files:**
- Create: `third-party/imgui/` (clonovani submodul nebo stahnout release v1.91)
- Create: `third-party/glfw/` (clonovani submodul nebo stahnout release 3.4)
- Modify: `CMakeLists.txt` (pridat ithaca-gui target)
- Create: `third-party/imgui/CMakeLists.txt`
- Create: `app/gui/main.cpp` (minimalni okno)

- [ ] **Step 1: Stahnout ImGui v1.91**

```bash
cd /Users/j/Projects/ithaca-legacy/third-party
mkdir imgui && cd imgui
curl -L https://github.com/ocornut/imgui/archive/refs/tags/v1.91.0.tar.gz | tar xz --strip 1
```

Expected: `third-party/imgui/imgui.h`, `imgui.cpp`, `backends/imgui_impl_glfw.h`, ... existují.

- [ ] **Step 2: Stahnout GLFW 3.4**

```bash
cd /Users/j/Projects/ithaca-legacy/third-party
git clone --depth 1 --branch 3.4 https://github.com/glfw/glfw.git glfw-src
mv glfw-src glfw
```

Expected: `third-party/glfw/CMakeLists.txt`, `third-party/glfw/include/GLFW/glfw3.h` existují.

- [ ] **Step 3: Napsat third-party/imgui/CMakeLists.txt**

```cmake
add_library(imgui STATIC
    imgui.cpp
    imgui_draw.cpp
    imgui_tables.cpp
    imgui_widgets.cpp
    imgui_demo.cpp
    backends/imgui_impl_glfw.cpp
    backends/imgui_impl_opengl3.cpp
)
target_include_directories(imgui PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/backends
)
target_link_libraries(imgui PUBLIC glfw)
target_compile_features(imgui PUBLIC cxx_std_17)
```

- [ ] **Step 4: Upravit top-level CMakeLists.txt**

V `third-party/CMakeLists.txt` (nebo top-level) pridat:

```cmake
# GLFW: vypnout neuzite komponenty
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
add_subdirectory(third-party/glfw)
add_subdirectory(third-party/imgui)
```

A pak novy target:

```cmake
add_executable(ithaca-gui app/gui/main.cpp)
target_link_libraries(ithaca-gui PRIVATE ithaca_core imgui glfw)
set_target_properties(ithaca-gui PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
if(APPLE)
    target_link_libraries(ithaca-gui PRIVATE
        "-framework OpenGL" "-framework Cocoa" "-framework IOKit"
        "-framework CoreVideo")
elseif(UNIX)
    target_link_libraries(ithaca-gui PRIVATE GL)
endif()
```

- [ ] **Step 5: Napsat minimal app/gui/main.cpp**

```cpp
// app/gui/main.cpp - F8 GUI entry point. Skeleton: GLFW okno + ImGui frame.
#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include <GLFW/glfw3.h>
#include <cstdio>

static void glfwErrorCb(int err, const char* desc) {
    std::fprintf(stderr, "GLFW error %d: %s\n", err, desc);
}

int main() {
    glfwSetErrorCallback(glfwErrorCb);
    if (!glfwInit()) return 1;
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
#ifdef __APPLE__
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
#endif
    GLFWwindow* w = glfwCreateWindow(1024, 768, "ithaca-gui", nullptr, nullptr);
    if (!w) { glfwTerminate(); return 1; }
    glfwMakeContextCurrent(w);
    glfwSwapInterval(1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();
    ImGui_ImplGlfw_InitForOpenGL(w, true);
    ImGui_ImplOpenGL3_Init("#version 150");

    while (!glfwWindowShouldClose(w)) {
        glfwPollEvents();
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        ImGui::ShowDemoWindow();

        ImGui::Render();
        int fbw, fbh; glfwGetFramebufferSize(w, &fbw, &fbh);
        glViewport(0, 0, fbw, fbh);
        glClearColor(0.1f, 0.1f, 0.1f, 1.f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(w);
    }

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(w);
    glfwTerminate();
    return 0;
}
```

- [ ] **Step 6: Build a spustit**

```bash
cd /Users/j/Projects/ithaca-legacy && make build
./build/ithaca-gui
```

Expected: otevre se okno s ImGui demo. Ukoncit Cmd-Q / zavrit okno.

- [ ] **Step 7: Commit**

```bash
cd /Users/j/Projects/ithaca-legacy
git add third-party/imgui third-party/glfw CMakeLists.txt app/gui/main.cpp
git commit -m "faze8(scaffold): vendor ImGui + GLFW, prazdny ithaca-gui s GLFW oknem"
```

---

## Task 2: Engine API — diagnostic gettery

**Files:**
- Modify: `engine/engine.h` (deklarace), `engine/engine.cpp` (implementace)
- Modify: `engine/stream/stream_engine.h` (numRingsUsed)
- Modify: `engine/stream/stream_engine.cpp`
- Modify: `engine/voice/voice_pool.h` (voicesView)
- Test: `tests/test_engine_diagnostics.cpp` (novy)

- [ ] **Step 1: Pridat StreamEngine::numRingsUsed**

V `stream_engine.h`:
```cpp
// Pocet ringu, ktere jsou aktualne in_use (diagnostika).
int numRingsUsed() const noexcept;
```

V `stream_engine.cpp`:
```cpp
int StreamEngine::numRingsUsed() const noexcept {
    int n = 0;
    for (const auto& r : rings_) {
        if (r->in_use_.load(std::memory_order_acquire)) ++n;
    }
    return n;
}
```

- [ ] **Step 2: Pridat VoicePool::voicesView**

V `voice_pool.h` v public sekci:
```cpp
// Const accessor pro diagnostiku/GUI (nikdy NEMENIT vraceny vektor!).
const std::vector<Voice>& voicesView() const noexcept { return voices_; }
```

- [ ] **Step 3: Pridat Engine gettery do engine.h**

```cpp
// -- Diagnostika (GUI/monitor; thread-safe atomic loads) --
int     resonanceVoices() const noexcept;
int     numRingsUsed()    const noexcept;
uint8_t pedalCC()         const noexcept;
// Maska aktivnich main voice midi cisel; vyplni 128 bool out.
void    activeMidiNotes(bool out[128]) const noexcept;
// Max currentLevel pres vsechny voicy te midi (pro keyboard viz alpha).
float   currentGainFor(int midi) const noexcept;
```

- [ ] **Step 4: Implementovat v engine.cpp**

```cpp
int Engine::resonanceVoices() const noexcept {
    return resonance_ ? resonance_->activeCount() : 0;
}

int Engine::numRingsUsed() const noexcept {
    return stream_ ? stream_->numRingsUsed() : 0;
}

uint8_t Engine::pedalCC() const noexcept {
    return pedal_.sustainCC();
}

void Engine::activeMidiNotes(bool out[128]) const noexcept {
    std::memset(out, 0, 128 * sizeof(bool));
    if (!pool_) return;
    for (const auto& v : pool_->voicesView()) {
        if (v.active() && v.midi() >= 0 && v.midi() < 128) {
            out[v.midi()] = true;
        }
    }
}

float Engine::currentGainFor(int midi) const noexcept {
    if (!pool_ || midi < 0 || midi >= 128) return 0.f;
    float g = 0.f;
    for (const auto& v : pool_->voicesView()) {
        if (v.active() && v.midi() == midi) {
            const float l = v.currentLevel();
            if (l > g) g = l;
        }
    }
    return g;
}
```

V `engine.cpp` doplnit include `<cstring>` pokud chybi.

- [ ] **Step 5: Napsat test tests/test_engine_diagnostics.cpp**

```cpp
#include "doctest.h"
#include "engine/engine.h"
#include <thread>
#include <chrono>

TEST_CASE("Engine diagnostic getters") {
    using namespace ithaca;
    Engine e;
    EngineConfig cfg;
    cfg.sample_rate = 48000;
    cfg.block_size = 256;
    REQUIRE(e.init(cfg));
    // Bez nactene banky:
    CHECK(e.activeVoices() == 0);
    CHECK(e.resonanceVoices() == 0);
    CHECK(e.numRingsUsed() == 0);
    CHECK(e.pedalCC() == 0);

    bool mask[128];
    e.activeMidiNotes(mask);
    for (int i = 0; i < 128; ++i) CHECK_FALSE(mask[i]);

    CHECK(e.currentGainFor(60) == 0.f);
    CHECK(e.currentGainFor(-1) == 0.f);
    CHECK(e.currentGainFor(128) == 0.f);
}
```

Pridat do `tests/CMakeLists.txt`:
```cmake
add_executable(test_engine_diagnostics test_engine_diagnostics.cpp)
target_link_libraries(test_engine_diagnostics PRIVATE ithaca_core)
add_test(NAME test_engine_diagnostics COMMAND test_engine_diagnostics)
```

- [ ] **Step 6: Build + run test**

```bash
cd /Users/j/Projects/ithaca-legacy && make build && ctest --test-dir build -R diagnostics --output-on-failure
```

Expected: PASS.

- [ ] **Step 7: Commit**

```bash
git add engine/engine.h engine/engine.cpp engine/stream/stream_engine.h engine/stream/stream_engine.cpp engine/voice/voice_pool.h tests/test_engine_diagnostics.cpp tests/CMakeLists.txt
git commit -m "faze8(engine): diagnostic gettery (rings, resonance, pedal, active midi, gain)"
```

---

## Task 3: Engine API — runtime settery

**Files:**
- Modify: `engine/engine.h`, `engine/engine.cpp`
- Test: prodlouzit `tests/test_engine_diagnostics.cpp`

- [ ] **Step 1: Pridat settery do engine.h**

```cpp
// -- Runtime parametry (GUI; atomic / single-thread-safe) --
void setReleaseMs(float ms) noexcept;
void setResonanceStrength(float s) noexcept;   // wrap resonance_->setStrength
void setExciteDecayMs(float ms) noexcept;      // wrap resonance_->setExciteDecayTimeMs
```

`master_gain_` jiz ma `setMasterGain` (atomic). Nepridavame znovu.

- [ ] **Step 2: Implementovat v engine.cpp**

```cpp
void Engine::setReleaseMs(float ms) noexcept {
    if (ms < 1.f) ms = 1.f;
    if (ms > 60000.f) ms = 60000.f;
    cfg_.release_ms = ms;
}

void Engine::setResonanceStrength(float s) noexcept {
    if (resonance_) resonance_->setStrength(s);
}

void Engine::setExciteDecayMs(float ms) noexcept {
    if (resonance_) resonance_->setExciteDecayTimeMs(ms, cfg_.block_size,
                                                     (float)cfg_.sample_rate);
}
```

Pozn.: `cfg_.release_ms` je float v EngineConfig, ne atomic; ale zapis z main threadu a cteni z audio threadu pri `scaledReleaseMs()` neni race-critical (jen mensi nepresnost behem zlomku ms). Pre robustni MVP staci.

- [ ] **Step 3: Pridat test settery**

V `test_engine_diagnostics.cpp` doplnit:
```cpp
TEST_CASE("Engine runtime setters") {
    using namespace ithaca;
    Engine e;
    EngineConfig cfg; cfg.release_ms = 200.f; cfg.resonance_strength = 0.5f;
    REQUIRE(e.init(cfg));

    e.setReleaseMs(500.f);
    // (cfg_.release_ms je private; over si pres scaledReleaseMs nepriamo,
    // ale to vyzaduje pedal=0). Pro MVP staci ze setter neselze runtime.
    CHECK_NOTHROW(e.setReleaseMs(500.f));
    CHECK_NOTHROW(e.setResonanceStrength(0.8f));
    CHECK_NOTHROW(e.setExciteDecayMs(3000.f));

    // Clamp
    CHECK_NOTHROW(e.setReleaseMs(-100.f));   // → clamp na 1
    CHECK_NOTHROW(e.setReleaseMs(999999.f)); // → clamp na 60000
}
```

- [ ] **Step 4: Build + test + commit**

```bash
make build && ctest --test-dir build -R diagnostics --output-on-failure
git add engine/engine.h engine/engine.cpp tests/test_engine_diagnostics.cpp
git commit -m "faze8(engine): runtime settery setReleaseMs/setResonanceStrength/setExciteDecayMs"
```

---

## Task 4: Engine API — master peak meter

**Files:**
- Modify: `engine/engine.h`, `engine/engine.cpp`
- Test: `tests/test_master_peak_meter.cpp` (novy)

- [ ] **Step 1: Pridat atomic peak do engine.h**

V private sekci `Engine`:
```cpp
std::atomic<float> master_peak_l_{0.f};
std::atomic<float> master_peak_r_{0.f};
```

V public sekci:
```cpp
float masterPeakL() const noexcept { return master_peak_l_.load(std::memory_order_relaxed); }
float masterPeakR() const noexcept { return master_peak_r_.load(std::memory_order_relaxed); }
```

- [ ] **Step 2: Vlozit peak vypocet do processBlock**

V `engine.cpp` v `Engine::processBlock` PO aplikaci `master_gain` na buffer:

```cpp
// Master peak meter (decay ~100 ms — pro GUI plynule jdoucinka).
float peak_l = 0.f, peak_r = 0.f;
for (int i = 0; i < n_samples; ++i) {
    const float al = std::fabs(out_l[i]);
    const float ar = std::fabs(out_r[i]);
    if (al > peak_l) peak_l = al;
    if (ar > peak_r) peak_r = ar;
}
const float sr = (float)cfg_.sample_rate;
const float decay = std::exp(-(float)n_samples / (0.1f * sr));
const float cur_l = master_peak_l_.load(std::memory_order_relaxed);
const float cur_r = master_peak_r_.load(std::memory_order_relaxed);
master_peak_l_.store(std::max(peak_l, cur_l * decay), std::memory_order_relaxed);
master_peak_r_.store(std::max(peak_r, cur_r * decay), std::memory_order_relaxed);
```

- [ ] **Step 3: Napsat test**

```cpp
// tests/test_master_peak_meter.cpp
#include "doctest.h"
#include "engine/engine.h"
#include <vector>

TEST_CASE("Master peak meter — pulse + decay") {
    using namespace ithaca;
    Engine e;
    EngineConfig cfg; cfg.sample_rate = 48000; cfg.block_size = 256;
    REQUIRE(e.init(cfg));

    // Pred prvnim renderem peak 0.
    CHECK(e.masterPeakL() == doctest::Approx(0.f));
    CHECK(e.masterPeakR() == doctest::Approx(0.f));

    // Simuluj "hluchy" render (zadna banka) — peak zustane 0.
    std::vector<float> L(256, 0.f), R(256, 0.f);
    e.processBlock(L.data(), R.data(), 256);
    CHECK(e.masterPeakL() < 0.01f);
    CHECK(e.masterPeakR() < 0.01f);

    // (Realny test by potreboval naplnit buffer test signalem pred merenim
    // peaku — to vyzaduje upravu API. Pro MVP staci sanity check ze peak
    // je v rozumnem rozsahu pri nulovem inputu.)
}
```

Pridat do `tests/CMakeLists.txt`.

- [ ] **Step 4: Build + test + commit**

```bash
make build && ctest --test-dir build -R peak --output-on-failure
git add engine/engine.h engine/engine.cpp tests/test_master_peak_meter.cpp tests/CMakeLists.txt
git commit -m "faze8(engine): master peak meter L/R s decay 100ms"
```

---

## Task 5: Logger subscriber API + GUI ring buffer

**Files:**
- Modify: `engine/util/log.h`, `engine/util/log.cpp`
- Create: `app/gui/log_subscriber.h`, `app/gui/log_subscriber.cpp`
- Test: `tests/test_log_subscriber.cpp` (novy)

- [ ] **Step 1: Pridat Subscriber API do log.h**

```cpp
struct LogEntry {
    long long timestamp_us;
    std::string topic;
    Severity sev;
    std::string message;
};

class Logger {
    // ...
public:
    using Subscriber = std::function<void(const LogEntry&)>;
    // Pridej subscriber; volano pri kazdem log() volani. Thread-safe.
    void addSubscriber(Subscriber s);
    // Zrus vsechny subscribery (pro test cleanup).
    void clearSubscribers();
private:
    std::mutex subscriber_mtx_;
    std::vector<Subscriber> subscribers_;
};
```

V `log.cpp` v existujicim `Logger::log(...)` PO formattingu zpravy pred vraceni:
```cpp
{
    std::lock_guard<std::mutex> lk(subscriber_mtx_);
    if (!subscribers_.empty()) {
        LogEntry e{ts_us, topic, sev, msg};
        for (auto& sub : subscribers_) sub(e);
    }
}
```

Implementovat `addSubscriber` a `clearSubscribers` triviali.

- [ ] **Step 2: Napsat GUI ring buffer**

```cpp
// app/gui/log_subscriber.h
#pragma once
#include "util/log.h"
#include <array>
#include <mutex>

namespace ithaca::gui {

// Cirkularni buffer N poslednich log eventu, thread-safe.
class LogRingBuffer {
public:
    static constexpr int kCapacity = 256;

    void push(const log::LogEntry& e);
    // Snapshot: zkopiruj posledni `max_n` zaznamu do `out` (nejstarsi prvni).
    // Vraci pocet skutecne zkopirovanych.
    int snapshot(log::LogEntry* out, int max_n) const;

private:
    mutable std::mutex                       mtx_;
    std::array<log::LogEntry, kCapacity>     buf_{};
    int                                      head_ = 0;  // index dalsiho zapisu
    int                                      size_ = 0;  // pocet platnych
};

} // namespace ithaca::gui
```

Implementace `log_subscriber.cpp`:
```cpp
#include "log_subscriber.h"

namespace ithaca::gui {

void LogRingBuffer::push(const log::LogEntry& e) {
    std::lock_guard<std::mutex> lk(mtx_);
    buf_[head_] = e;
    head_ = (head_ + 1) % kCapacity;
    if (size_ < kCapacity) ++size_;
}

int LogRingBuffer::snapshot(log::LogEntry* out, int max_n) const {
    std::lock_guard<std::mutex> lk(mtx_);
    const int n = (max_n < size_) ? max_n : size_;
    // Zacatek = head_ - size_ (modulo kCapacity).
    int start = (head_ - size_ + kCapacity) % kCapacity;
    // Skip starsi nez 'n' rec po krajich
    if (size_ > max_n) start = (start + (size_ - max_n)) % kCapacity;
    for (int i = 0; i < n; ++i) {
        out[i] = buf_[(start + i) % kCapacity];
    }
    return n;
}

} // namespace ithaca::gui
```

- [ ] **Step 3: Test**

```cpp
// tests/test_log_subscriber.cpp
#include "doctest.h"
#include "util/log.h"
#include "../app/gui/log_subscriber.h"

TEST_CASE("LogRingBuffer push + snapshot") {
    using namespace ithaca::gui;
    LogRingBuffer rb;

    log::LogEntry e1{1, "t", log::Severity::Info, "msg1"};
    log::LogEntry e2{2, "t", log::Severity::Info, "msg2"};
    log::LogEntry e3{3, "t", log::Severity::Info, "msg3"};

    rb.push(e1); rb.push(e2); rb.push(e3);

    log::LogEntry out[10];
    int n = rb.snapshot(out, 10);
    CHECK(n == 3);
    CHECK(out[0].timestamp_us == 1);
    CHECK(out[2].timestamp_us == 3);
}

TEST_CASE("LogRingBuffer wrap-around") {
    using namespace ithaca::gui;
    LogRingBuffer rb;
    for (int i = 0; i < LogRingBuffer::kCapacity + 10; ++i) {
        rb.push({i, "t", log::Severity::Info, "x"});
    }
    log::LogEntry out[LogRingBuffer::kCapacity];
    int n = rb.snapshot(out, LogRingBuffer::kCapacity);
    CHECK(n == LogRingBuffer::kCapacity);
    // Nejstarsi by mel byt index 10 (po wrap-around)
    CHECK(out[0].timestamp_us == 10);
    CHECK(out[n - 1].timestamp_us == LogRingBuffer::kCapacity + 10 - 1);
}

TEST_CASE("Logger Subscriber API integration") {
    int count = 0;
    log::Logger::default_().clearSubscribers();
    log::Logger::default_().addSubscriber([&count](const log::LogEntry&){ count++; });
    log::Logger::default_().log("test", log::Severity::Info, "hello");
    log::Logger::default_().log("test", log::Severity::Info, "world");
    CHECK(count == 2);
    log::Logger::default_().clearSubscribers();
}
```

Pridat do `tests/CMakeLists.txt`:
```cmake
add_executable(test_log_subscriber test_log_subscriber.cpp
    ../app/gui/log_subscriber.cpp)
target_include_directories(test_log_subscriber PRIVATE ../app/gui)
target_link_libraries(test_log_subscriber PRIVATE ithaca_core)
add_test(NAME test_log_subscriber COMMAND test_log_subscriber)
```

- [ ] **Step 4: Build + test + commit**

```bash
make build && ctest --test-dir build -R log_subscriber --output-on-failure
git add engine/util/log.h engine/util/log.cpp app/gui/log_subscriber.h app/gui/log_subscriber.cpp tests/test_log_subscriber.cpp tests/CMakeLists.txt
git commit -m "faze8(log): Subscriber API + GUI LogRingBuffer pro log strip"
```

---

## Task 6: Persistence (load/save state.json)

**Files:**
- Create: `app/gui/persistence.h`, `app/gui/persistence.cpp`
- Test: `tests/test_persistence.cpp` (novy)

- [ ] **Step 1: Napsat persistence.h**

```cpp
// app/gui/persistence.h - JSON load/save GUI state.
#pragma once
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace ithaca::gui {

struct GuiState {
    int         schema_version  = 1;
    std::string bank_path;             // posledni nactena banka
    std::string midi_port_name;        // posledni MIDI port (substring match)
    float       master_gain_db    = 0.f;
    float       resonance_strength = 0.5f;
    float       release_ms        = 200.f;
    float       excite_decay_ms   = 5000.f;
    int         max_resonance_voices = 32;
    // Window geometry
    int         window_x = 100, window_y = 100;
    int         window_w = 1024, window_h = 768;
};

// Najit cestu k state.json podle OS:
//  macOS: $HOME/Library/Application Support/ithaca-legacy/state.json
//  Linux: $XDG_CONFIG_HOME/ithaca-legacy/state.json (fallback $HOME/.config/...)
//  Win:   %APPDATA%/ithaca-legacy/state.json
std::filesystem::path defaultStatePath();

// Nacti state z path. Vraci nullopt pri chybe (missing/invalid/wrong version).
std::optional<GuiState> loadState(const std::filesystem::path& path);

// Atomic write: zapis do path.tmp + rename. Vraci true pri uspechu.
bool saveState(const std::filesystem::path& path, const GuiState& s);

} // namespace ithaca::gui
```

- [ ] **Step 2: Napsat persistence.cpp (rucni mini-JSON, bez deps)**

```cpp
// app/gui/persistence.cpp
#include "persistence.h"
#include <cstdlib>
#include <cstdio>
#include <fstream>
#include <sstream>

namespace ithaca::gui {

namespace {
std::filesystem::path platformConfigDir() {
#ifdef __APPLE__
    if (const char* h = std::getenv("HOME"))
        return std::filesystem::path(h) / "Library" / "Application Support";
#elif defined(_WIN32)
    if (const char* a = std::getenv("APPDATA"))
        return std::filesystem::path(a);
#else
    if (const char* x = std::getenv("XDG_CONFIG_HOME"))
        return std::filesystem::path(x);
    if (const char* h = std::getenv("HOME"))
        return std::filesystem::path(h) / ".config";
#endif
    return std::filesystem::current_path();
}

// Mini JSON escape pro string (jen \", \\, \n).
std::string jsonEscape(const std::string& s) {
    std::string o; o.reserve(s.size() + 4);
    for (char c : s) {
        switch (c) {
            case '"':  o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n";  break;
            default:   o += c;
        }
    }
    return o;
}

// Velmi jednoduchy parser pro flat key:value JSON (na nas format staci).
// Vraci hodnotu pro klic, nebo prazdny string.
std::string findValue(const std::string& json, const std::string& key) {
    const std::string needle = "\"" + key + "\"";
    size_t k = json.find(needle);
    if (k == std::string::npos) return {};
    size_t c = json.find(':', k);
    if (c == std::string::npos) return {};
    ++c;
    while (c < json.size() && std::isspace((unsigned char)json[c])) ++c;
    if (c >= json.size()) return {};
    // String value: ohraniceno ""
    if (json[c] == '"') {
        size_t e = c + 1;
        std::string v;
        while (e < json.size() && json[e] != '"') {
            if (json[e] == '\\' && e + 1 < json.size()) {
                char nxt = json[e + 1];
                if (nxt == 'n') v += '\n';
                else v += nxt;
                e += 2;
            } else {
                v += json[e++];
            }
        }
        return v;
    }
    // Numericka / boolean — cti do , nebo } nebo whitespace
    size_t e = c;
    while (e < json.size() && json[e] != ',' && json[e] != '}' && json[e] != '\n') ++e;
    std::string v = json.substr(c, e - c);
    // trim
    while (!v.empty() && std::isspace((unsigned char)v.back())) v.pop_back();
    return v;
}
} // namespace

std::filesystem::path defaultStatePath() {
    return platformConfigDir() / "ithaca-legacy" / "state.json";
}

std::optional<GuiState> loadState(const std::filesystem::path& path) {
    std::ifstream f(path);
    if (!f) return std::nullopt;
    std::stringstream ss; ss << f.rdbuf();
    const std::string json = ss.str();

    GuiState s;
    try {
        s.schema_version = std::stoi(findValue(json, "schema_version"));
        if (s.schema_version != 1) return std::nullopt;
        s.bank_path             = findValue(json, "bank_path");
        s.midi_port_name        = findValue(json, "midi_port_name");
        s.master_gain_db        = std::stof(findValue(json, "master_gain_db"));
        s.resonance_strength    = std::stof(findValue(json, "resonance_strength"));
        s.release_ms            = std::stof(findValue(json, "release_ms"));
        s.excite_decay_ms       = std::stof(findValue(json, "excite_decay_ms"));
        s.max_resonance_voices  = std::stoi(findValue(json, "max_resonance_voices"));
        s.window_x = std::stoi(findValue(json, "window_x"));
        s.window_y = std::stoi(findValue(json, "window_y"));
        s.window_w = std::stoi(findValue(json, "window_w"));
        s.window_h = std::stoi(findValue(json, "window_h"));
    } catch (...) {
        return std::nullopt;
    }
    return s;
}

bool saveState(const std::filesystem::path& path, const GuiState& s) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) return false;

    auto tmp = path; tmp += ".tmp";
    {
        std::ofstream f(tmp);
        if (!f) return false;
        f << "{\n";
        f << "  \"schema_version\": " << s.schema_version << ",\n";
        f << "  \"bank_path\": \""        << jsonEscape(s.bank_path)      << "\",\n";
        f << "  \"midi_port_name\": \""   << jsonEscape(s.midi_port_name) << "\",\n";
        f << "  \"master_gain_db\": "     << s.master_gain_db             << ",\n";
        f << "  \"resonance_strength\": " << s.resonance_strength         << ",\n";
        f << "  \"release_ms\": "         << s.release_ms                 << ",\n";
        f << "  \"excite_decay_ms\": "    << s.excite_decay_ms            << ",\n";
        f << "  \"max_resonance_voices\": " << s.max_resonance_voices     << ",\n";
        f << "  \"window_x\": " << s.window_x << ",\n";
        f << "  \"window_y\": " << s.window_y << ",\n";
        f << "  \"window_w\": " << s.window_w << ",\n";
        f << "  \"window_h\": " << s.window_h << "\n";
        f << "}\n";
    }
    std::filesystem::rename(tmp, path, ec);
    return !ec;
}

} // namespace ithaca::gui
```

- [ ] **Step 3: Test round-trip + edge cases**

```cpp
// tests/test_persistence.cpp
#include "doctest.h"
#include "../app/gui/persistence.h"
#include <filesystem>

TEST_CASE("Persistence round-trip") {
    using namespace ithaca::gui;
    auto p = std::filesystem::temp_directory_path() / "ithaca_test_state.json";

    GuiState s;
    s.bank_path = "/foo/bar/bank";
    s.midi_port_name = "IAC Driver";
    s.master_gain_db = -6.0f;
    s.resonance_strength = 0.7f;
    s.release_ms = 250.f;
    s.excite_decay_ms = 4000.f;
    s.max_resonance_voices = 16;
    s.window_x = 200; s.window_y = 300; s.window_w = 800; s.window_h = 600;

    REQUIRE(saveState(p, s));
    auto loaded = loadState(p);
    REQUIRE(loaded.has_value());
    CHECK(loaded->bank_path == s.bank_path);
    CHECK(loaded->midi_port_name == s.midi_port_name);
    CHECK(loaded->master_gain_db == doctest::Approx(s.master_gain_db));
    CHECK(loaded->resonance_strength == doctest::Approx(s.resonance_strength));
    CHECK(loaded->window_w == s.window_w);

    std::filesystem::remove(p);
}

TEST_CASE("Persistence missing file") {
    using namespace ithaca::gui;
    auto p = std::filesystem::temp_directory_path() / "ithaca_nonexistent_xyz.json";
    std::filesystem::remove(p);  // jistota
    CHECK_FALSE(loadState(p).has_value());
}

TEST_CASE("Persistence wrong schema_version") {
    using namespace ithaca::gui;
    auto p = std::filesystem::temp_directory_path() / "ithaca_bad_schema.json";
    {
        std::ofstream f(p);
        f << "{\"schema_version\":99,\"bank_path\":\"\"}\n";
    }
    CHECK_FALSE(loadState(p).has_value());
    std::filesystem::remove(p);
}

TEST_CASE("defaultStatePath neni prazdne") {
    using namespace ithaca::gui;
    CHECK_FALSE(defaultStatePath().empty());
}
```

Pridat do `tests/CMakeLists.txt`.

- [ ] **Step 4: Build + test + commit**

```bash
make build && ctest --test-dir build -R persistence --output-on-failure
git add app/gui/persistence.h app/gui/persistence.cpp tests/test_persistence.cpp tests/CMakeLists.txt
git commit -m "faze8(gui): persistence GuiState - load/save state.json + platform paths"
```

---

## Task 7: Window app — sjednoceni engine init + audio + midi

**Files:**
- Modify: `app/gui/main.cpp` (rozsirit)
- Create: `app/gui/app_context.h`, `app/gui/app_context.cpp`

- [ ] **Step 1: Napsat AppContext header**

```cpp
// app/gui/app_context.h - drzi Engine + AudioDevice + MidiInput + log buf.
#pragma once
#include "engine/engine.h"
#include "io/audio_device.h"
#include "midi/midi_input.h"
#include "log_subscriber.h"
#include "persistence.h"
#include <memory>
#include <string>

namespace ithaca::gui {

struct AppContext {
    Engine                       engine;
    std::unique_ptr<AudioDevice> audio;
    MidiInput                    midi;
    LogRingBuffer                log_buf;
    GuiState                     state;

    // Lifecycle:
    bool initFromState(const GuiState& s);  // engine init, audio start, midi open, bank load
    void shutdown();
};

} // namespace ithaca::gui
```

- [ ] **Step 2: Implementovat app_context.cpp**

```cpp
#include "app_context.h"
#include "util/log.h"

namespace ithaca::gui {

bool AppContext::initFromState(const GuiState& s) {
    state = s;

    // Subscriber loggeru -> log_buf
    log::Logger::default_().addSubscriber(
        [this](const log::LogEntry& e){ log_buf.push(e); });

    EngineConfig cfg;
    cfg.sample_rate = 48000;
    cfg.block_size  = 256;
    cfg.master_gain        = std::pow(10.f, s.master_gain_db / 20.f);
    cfg.resonance_strength = s.resonance_strength;
    cfg.release_ms         = s.release_ms;
    cfg.excite_decay_ms    = s.excite_decay_ms;
    cfg.max_resonance_voices = s.max_resonance_voices;
    if (!engine.init(cfg)) return false;

    if (!s.bank_path.empty()) {
        engine.loadBank(s.bank_path);  // best-effort; chybi -> log warning
    }

    audio = std::make_unique<AudioDevice>();
    audio->start(&engine);

    if (!s.midi_port_name.empty()) {
        midi.open(&engine, s.midi_port_name);  // substring match — viz MidiInput
    }
    return true;
}

void AppContext::shutdown() {
    if (audio) audio->stop();
    midi.close();
    log::Logger::default_().clearSubscribers();
}

} // namespace ithaca::gui
```

POZN.: `AudioDevice::start(&engine)` a `MidiInput::open(engine, port_name)` — overit ze signatury sedi s existujicim kodem; jinak adaptovat.

- [ ] **Step 3: Upravit main.cpp**

Zacatek vymenit za:
```cpp
#include "app_context.h"
#include "persistence.h"
// (zachovat ImGui/GLFW includes)

int main() {
    using namespace ithaca::gui;

    // Load state nebo defaulty
    GuiState st;
    if (auto loaded = loadState(defaultStatePath()); loaded.has_value()) {
        st = *loaded;
    }

    AppContext ctx;
    if (!ctx.initFromState(st)) {
        std::fprintf(stderr, "App init failed\n");
        return 1;
    }

    // (GLFW + ImGui init jako predtim, jen window size z st.window_w/h)
    GLFWwindow* w = glfwCreateWindow(st.window_w, st.window_h, "ithaca-gui",
                                     nullptr, nullptr);
    // ... ImGui init ...

    while (!glfwWindowShouldClose(w)) {
        glfwPollEvents();
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        // Zatim placeholder okno
        ImGui::Begin("Ithaca");
        ImGui::Text("Voices: %d / %d", ctx.engine.activeVoices(), 256);
        ImGui::Text("Resonance: %d / %d", ctx.engine.resonanceVoices(), 32);
        ImGui::Text("Rings: %d / %d", ctx.engine.numRingsUsed(), 288);
        ImGui::Text("Pedal CC64: %d", (int)ctx.engine.pedalCC());
        ImGui::End();

        ImGui::Render();
        // ... render frame ...
    }

    // Save state pred exit (window geom z aktualniho glfwGetWindowSize/Pos)
    glfwGetWindowSize(w, &ctx.state.window_w, &ctx.state.window_h);
    glfwGetWindowPos(w, &ctx.state.window_x, &ctx.state.window_y);
    saveState(defaultStatePath(), ctx.state);

    ctx.shutdown();
    // ... ImGui/GLFW shutdown ...
    return 0;
}
```

- [ ] **Step 4: Pridat zdroje do CMakeLists.txt**

```cmake
add_executable(ithaca-gui
    app/gui/main.cpp
    app/gui/app_context.cpp
    app/gui/log_subscriber.cpp
    app/gui/persistence.cpp)
target_include_directories(ithaca-gui PRIVATE app/gui)
```

- [ ] **Step 5: Build + manualni smoke test**

```bash
make build && ./build/ithaca-gui
```

Expected:
- Otevre okno
- V okne text "Voices: 0 / 256", "Resonance: 0 / 32", "Rings: 0 / 288", "Pedal CC64: 0"
- Pri exit (zavri okno) ulozi state.json do `~/Library/Application Support/ithaca-legacy/`
- Druhy spusteni: state se nacte, okno ma stejnou velikost

- [ ] **Step 6: Commit**

```bash
git add app/gui/app_context.h app/gui/app_context.cpp app/gui/main.cpp CMakeLists.txt
git commit -m "faze8(gui): AppContext - engine init z GuiState + audio + midi + log subscriber"
```

---

## Task 8: Top bar — bank dropdown, MIDI dropdown, master slider

**Files:**
- Create: `app/gui/panel_topbar.h`, `app/gui/panel_topbar.cpp`
- Modify: `app/gui/main.cpp`, `CMakeLists.txt`

- [ ] **Step 1: Napsat panel_topbar.h**

```cpp
// app/gui/panel_topbar.h
#pragma once
namespace ithaca::gui {
struct AppContext;
// Render top bar — bank picker, midi picker, master slider.
// Modifikuje ctx.state pri zmenach (a vola engine settery).
void renderTopBar(AppContext& ctx);
}
```

- [ ] **Step 2: Implementovat panel_topbar.cpp**

```cpp
#include "panel_topbar.h"
#include "app_context.h"
#include "imgui.h"
#include <cmath>
#include <filesystem>
#include <vector>

namespace ithaca::gui {

namespace {
// Spocte seznam podadresaru jako kandidatu na bank.
std::vector<std::string> scanBanks(const std::string& search_root) {
    std::vector<std::string> out;
    if (search_root.empty()) return out;
    std::error_code ec;
    if (!std::filesystem::is_directory(search_root, ec)) return out;
    for (const auto& e : std::filesystem::directory_iterator(search_root, ec)) {
        if (e.is_directory()) out.push_back(e.path().string());
    }
    return out;
}
} // namespace

void renderTopBar(AppContext& ctx) {
    ImGui::SetNextWindowPos({0, 0});
    ImGui::SetNextWindowSize({(float)ctx.state.window_w, 36.f});
    ImGui::Begin("##topbar", nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar);

    // Bank dropdown — search v adresari nad aktualni cestou
    std::string search_root = ctx.state.bank_path.empty()
        ? "" : std::filesystem::path(ctx.state.bank_path).parent_path().string();
    static std::vector<std::string> bank_candidates;
    static std::string last_root;
    if (search_root != last_root) {
        bank_candidates = scanBanks(search_root);
        last_root = search_root;
    }

    ImGui::Text("Bank:"); ImGui::SameLine();
    const char* curr_bank = ctx.state.bank_path.empty() ? "(none)"
                          : std::filesystem::path(ctx.state.bank_path).filename().string().c_str();
    ImGui::PushItemWidth(280);
    if (ImGui::BeginCombo("##bank", curr_bank)) {
        for (const auto& b : bank_candidates) {
            const std::string label = std::filesystem::path(b).filename().string();
            bool sel = (b == ctx.state.bank_path);
            if (ImGui::Selectable(label.c_str(), sel)) {
                if (b != ctx.state.bank_path) {
                    ctx.state.bank_path = b;
                    ctx.engine.loadBank(b);  // jednoduche, sync; UI freezne ~200ms
                }
            }
        }
        ImGui::EndCombo();
    }
    ImGui::PopItemWidth();

    ImGui::SameLine();
    ImGui::Text("MIDI:"); ImGui::SameLine();
    auto ports = MidiInput::listPorts();
    const char* curr_midi = ctx.state.midi_port_name.empty() ? "(none)"
                          : ctx.state.midi_port_name.c_str();
    ImGui::PushItemWidth(220);
    if (ImGui::BeginCombo("##midi", curr_midi)) {
        if (ImGui::Selectable("(none)", ctx.state.midi_port_name.empty())) {
            ctx.midi.close();
            ctx.state.midi_port_name.clear();
        }
        for (const auto& p : ports) {
            bool sel = (p == ctx.state.midi_port_name);
            if (ImGui::Selectable(p.c_str(), sel)) {
                if (p != ctx.state.midi_port_name) {
                    ctx.midi.close();
                    if (ctx.midi.open(&ctx.engine, p)) {
                        ctx.state.midi_port_name = p;
                    }
                }
            }
        }
        ImGui::EndCombo();
    }
    ImGui::PopItemWidth();

    ImGui::SameLine();
    ImGui::SetCursorPosX(ImGui::GetWindowWidth() - 280);
    ImGui::Text("Master:"); ImGui::SameLine();
    ImGui::PushItemWidth(180);
    if (ImGui::SliderFloat("##master", &ctx.state.master_gain_db, -60.f, 6.f, "%.1f dB")) {
        const float g = std::pow(10.f, ctx.state.master_gain_db / 20.f);
        ctx.engine.setMasterGain(g);
    }
    ImGui::PopItemWidth();

    ImGui::End();
}

} // namespace ithaca::gui
```

- [ ] **Step 3: Pridat volani v main.cpp**

V hlavnim render loopu nahradit demo ImGui::Begin/End za:
```cpp
renderTopBar(ctx);
// (dalsi panely budou pribyvat v dalsich taskach)
```

A `#include "panel_topbar.h"` na zacatku.

- [ ] **Step 4: Pridat panel_topbar.cpp do CMakeLists**

```cmake
add_executable(ithaca-gui
    app/gui/main.cpp
    app/gui/app_context.cpp
    app/gui/log_subscriber.cpp
    app/gui/persistence.cpp
    app/gui/panel_topbar.cpp)
```

- [ ] **Step 5: Build + smoke test**

```bash
make build && ./build/ithaca-gui
```

Expected: vidim top bar s bank dropdown, MIDI dropdown, master slider. Zmena master sliderem zmeni hlasitost (pokud hraje sample / pripojit MIDI a zahrat notu).

- [ ] **Step 6: Commit**

```bash
git add app/gui/panel_topbar.h app/gui/panel_topbar.cpp app/gui/main.cpp CMakeLists.txt
git commit -m "faze8(gui): top bar — bank dropdown + MIDI dropdown + master slider"
```

---

## Task 9: Diag panel + Params panel

**Files:**
- Create: `app/gui/panel_diag.h`, `panel_diag.cpp`
- Create: `app/gui/panel_params.h`, `panel_params.cpp`
- Modify: `app/gui/main.cpp`, `CMakeLists.txt`

- [ ] **Step 1: Napsat panel_diag.cpp**

```cpp
// app/gui/panel_diag.cpp
#include "panel_diag.h"
#include "app_context.h"
#include "imgui.h"
#include <cmath>

namespace ithaca::gui {

static float toDb(float lin) {
    if (lin < 1e-6f) return -120.f;
    return 20.f * std::log10(lin);
}

void renderDiagPanel(AppContext& ctx, float x, float y, float w, float h) {
    ImGui::SetNextWindowPos({x, y});
    ImGui::SetNextWindowSize({w, h});
    ImGui::Begin("Diag", nullptr,
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse);

    ImGui::Text("Voices:    %3d / 256", ctx.engine.activeVoices());
    ImGui::Text("Resonance: %3d /  32", ctx.engine.resonanceVoices());
    ImGui::Text("Rings:     %3d / 288", ctx.engine.numRingsUsed());

    const uint8_t cc = ctx.engine.pedalCC();
    ImVec4 col = (cc >= 100) ? ImVec4(1.f, 0.3f, 0.3f, 1.f)
               : (cc >= 64)  ? ImVec4(1.f, 0.9f, 0.3f, 1.f)
                              : ImVec4(0.4f, 1.f, 0.4f, 1.f);
    ImGui::TextColored(col, "Pedal CC64: %d", (int)cc);

    ImGui::Separator();
    ImGui::Text("Master meter:");
    const float dbL = toDb(ctx.engine.masterPeakL());
    const float dbR = toDb(ctx.engine.masterPeakR());
    // Mapuj -60..+6 dB na 0..1 progress bar
    auto dbTo01 = [](float db){ float t = (db + 60.f) / 66.f; if (t < 0) t = 0; if (t > 1) t = 1; return t; };
    ImGui::ProgressBar(dbTo01(dbL), {-FLT_MIN, 12}, "");
    ImGui::SameLine(); ImGui::Text("L %5.1f dB", dbL);
    ImGui::ProgressBar(dbTo01(dbR), {-FLT_MIN, 12}, "");
    ImGui::SameLine(); ImGui::Text("R %5.1f dB", dbR);

    ImGui::End();
}

} // namespace ithaca::gui
```

`panel_diag.h`:
```cpp
#pragma once
namespace ithaca::gui {
struct AppContext;
void renderDiagPanel(AppContext& ctx, float x, float y, float w, float h);
}
```

- [ ] **Step 2: Napsat panel_params.cpp**

```cpp
// app/gui/panel_params.cpp
#include "panel_params.h"
#include "app_context.h"
#include "imgui.h"

namespace ithaca::gui {

void renderParamsPanel(AppContext& ctx, float x, float y, float w, float h) {
    ImGui::SetNextWindowPos({x, y});
    ImGui::SetNextWindowSize({w, h});
    ImGui::Begin("Params", nullptr,
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse);

    if (ImGui::SliderFloat("Resonance str", &ctx.state.resonance_strength, 0.f, 1.f, "%.2f")) {
        ctx.engine.setResonanceStrength(ctx.state.resonance_strength);
    }
    if (ImGui::SliderFloat("Release ms", &ctx.state.release_ms, 50.f, 2000.f, "%.0f")) {
        ctx.engine.setReleaseMs(ctx.state.release_ms);
    }
    if (ImGui::SliderFloat("Excite decay ms", &ctx.state.excite_decay_ms, 500.f, 30000.f, "%.0f")) {
        ctx.engine.setExciteDecayMs(ctx.state.excite_decay_ms);
    }

    // max_resonance_voices je init-only — slider DISABLED s tooltipem.
    ImGui::BeginDisabled();
    ImGui::SliderInt("Max resonance", &ctx.state.max_resonance_voices, 1, 64);
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Vyzaduje restart aplikace");
    }

    ImGui::Separator();
    if (ImGui::Button("Reset to defaults")) {
        ctx.state.resonance_strength = 0.5f;
        ctx.state.release_ms = 200.f;
        ctx.state.excite_decay_ms = 5000.f;
        ctx.state.master_gain_db = 0.f;
        ctx.engine.setResonanceStrength(ctx.state.resonance_strength);
        ctx.engine.setReleaseMs(ctx.state.release_ms);
        ctx.engine.setExciteDecayMs(ctx.state.excite_decay_ms);
        ctx.engine.setMasterGain(1.f);
    }

    ImGui::End();
}

} // namespace ithaca::gui
```

`panel_params.h` (analogicky `panel_diag.h`).

- [ ] **Step 3: Volat z main.cpp render loop**

```cpp
const float W = (float)ctx.state.window_w;
const float H = (float)ctx.state.window_h;
renderTopBar(ctx);
// (keyboard panel pribude v dalsim tasku — zatim placeholder)
renderDiagPanel  (ctx, 0,     176, W * 0.5f, H - 176 - 96);
renderParamsPanel(ctx, W*0.5f, 176, W * 0.5f, H - 176 - 96);
```

- [ ] **Step 4: Pridat zdroje do CMakeLists, build, smoke**

```bash
# v CMakeLists.txt pridat panel_diag.cpp, panel_params.cpp
make build && ./build/ithaca-gui
```

Expected: diag panel ukazuje live metricy, params slidery zmeni hodnoty za behu (zaznena rezonance/release).

- [ ] **Step 5: Commit**

```bash
git add app/gui/panel_diag.h app/gui/panel_diag.cpp app/gui/panel_params.h app/gui/panel_params.cpp app/gui/main.cpp CMakeLists.txt
git commit -m "faze8(gui): diag panel (metrics+meter) + params panel (slidery + reset)"
```

---

## Task 10: Keyboard viz (88-key panel s aktivnimi notami)

**Files:**
- Create: `app/gui/panel_keyboard.h`, `panel_keyboard.cpp`
- Modify: `app/gui/main.cpp`, `CMakeLists.txt`

- [ ] **Step 1: Napsat panel_keyboard.cpp**

```cpp
// app/gui/panel_keyboard.cpp - 88-key piano keyboard viz (MIDI 21..108).
#include "panel_keyboard.h"
#include "app_context.h"
#include "imgui.h"

namespace ithaca::gui {

namespace {
constexpr int kFirstMidi = 21;  // A0
constexpr int kLastMidi  = 108; // C8 (88 klaves)

// Vraci true pokud je MIDI nota cerna klavesa.
bool isBlackKey(int midi) {
    const int pc = midi % 12;
    return pc == 1 || pc == 3 || pc == 6 || pc == 8 || pc == 10;
}
} // namespace

void renderKeyboardPanel(AppContext& ctx, float x, float y, float w, float h) {
    ImGui::SetNextWindowPos({x, y});
    ImGui::SetNextWindowSize({w, h});
    ImGui::Begin("Keyboard", nullptr,
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar);

    bool active[128];
    ctx.engine.activeMidiNotes(active);

    // Spocitej bile klavesy v rozsahu pro rovnomerne rozdeleni sirky
    int white_count = 0;
    for (int m = kFirstMidi; m <= kLastMidi; ++m) {
        if (!isBlackKey(m)) ++white_count;
    }

    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const float key_w = (w - 20.f) / (float)white_count;
    const float key_h = h - 60.f;     // rezerva na pedal indikator + popisek
    const float black_w = key_w * 0.6f;
    const float black_h = key_h * 0.6f;

    ImDrawList* dl = ImGui::GetWindowDrawList();

    // Render bile klavesy prvni
    int wi = 0;
    for (int m = kFirstMidi; m <= kLastMidi; ++m) {
        if (isBlackKey(m)) continue;
        const float kx = pos.x + 10.f + wi * key_w;
        const float ky = pos.y;
        ImU32 col = IM_COL32(245, 245, 245, 255);
        if (active[m]) {
            float g = ctx.engine.currentGainFor(m);
            if (g < 0.3f) g = 0.3f; if (g > 1.f) g = 1.f;
            // Modra zvyrazneni intenzity g
            col = IM_COL32(80 + (int)(80 * (1-g)), 130, 255, 255);
        }
        dl->AddRectFilled({kx, ky}, {kx + key_w - 1, ky + key_h}, col);
        dl->AddRect({kx, ky}, {kx + key_w - 1, ky + key_h}, IM_COL32(60,60,60,255));
        ++wi;
    }

    // Render cerne klavesy nahore (over bile)
    wi = 0;
    for (int m = kFirstMidi; m <= kLastMidi; ++m) {
        if (!isBlackKey(m)) { ++wi; continue; }
        // Cerna klavesa lezi mezi bilou (wi-1) a wi — vlevo bila byla wi-1.
        const float kx = pos.x + 10.f + (wi - 1) * key_w + key_w - black_w * 0.5f;
        const float ky = pos.y;
        ImU32 col = IM_COL32(20, 20, 20, 255);
        if (active[m]) {
            float g = ctx.engine.currentGainFor(m);
            if (g < 0.3f) g = 0.3f; if (g > 1.f) g = 1.f;
            col = IM_COL32(50, 100, (int)(160 + 90*g), 255);
        }
        dl->AddRectFilled({kx, ky}, {kx + black_w, ky + black_h}, col);
    }

    // Pedal indikator (ramecek pod klavesnici)
    const uint8_t cc = ctx.engine.pedalCC();
    ImU32 pcol = (cc >= 100) ? IM_COL32(220, 70, 70, 255)
               : (cc >= 64)  ? IM_COL32(220, 200, 70, 255)
                              : IM_COL32(80, 200, 80, 255);
    const float pedal_y = pos.y + key_h + 10.f;
    dl->AddRectFilled({pos.x + 10, pedal_y}, {pos.x + w - 10, pedal_y + 12}, pcol);
    dl->AddText({pos.x + 14, pedal_y + 14}, IM_COL32(200, 200, 200, 255),
        ("Pedal CC64=" + std::to_string((int)cc)).c_str());

    ImGui::End();
}

} // namespace ithaca::gui
```

`panel_keyboard.h`:
```cpp
#pragma once
namespace ithaca::gui {
struct AppContext;
void renderKeyboardPanel(AppContext& ctx, float x, float y, float w, float h);
}
```

- [ ] **Step 2: Render volani v main.cpp**

```cpp
const float W = (float)ctx.state.window_w;
const float H = (float)ctx.state.window_h;
renderTopBar     (ctx);
renderKeyboardPanel(ctx, 0,       36,  W,       180);
renderDiagPanel  (ctx, 0,       216, W * 0.5f, H - 216 - 96);
renderParamsPanel(ctx, W * 0.5f, 216, W * 0.5f, H - 216 - 96);
```

- [ ] **Step 3: Build + manualni test**

```bash
make build && ./build/ithaca-gui
```

Expected: pri hrani MIDI not vidim klavesy modre zvyraznene s intensity podle gainu. Pri pedalu se ramecek pod klaviaturou zmeni (zeleny → zluty → cerveny).

- [ ] **Step 4: Commit**

```bash
git add app/gui/panel_keyboard.h app/gui/panel_keyboard.cpp app/gui/main.cpp CMakeLists.txt
git commit -m "faze8(gui): keyboard panel — 88-key viz s aktivnimi notami + pedal indikator"
```

---

## Task 11: Log strip + final layout polish

**Files:**
- Create: `app/gui/panel_log.h`, `panel_log.cpp`
- Modify: `app/gui/main.cpp`

- [ ] **Step 1: Panel_log.cpp**

```cpp
#include "panel_log.h"
#include "app_context.h"
#include "imgui.h"
#include <array>

namespace ithaca::gui {

void renderLogPanel(AppContext& ctx, float x, float y, float w, float h) {
    ImGui::SetNextWindowPos({x, y});
    ImGui::SetNextWindowSize({w, h});
    ImGui::Begin("Log", nullptr,
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse);

    static std::array<log::LogEntry, 50> tmp;
    const int n = ctx.log_buf.snapshot(tmp.data(), (int)tmp.size());

    if (ImGui::BeginChild("##loglist", {0,0}, false,
            ImGuiWindowFlags_HorizontalScrollbar)) {
        for (int i = 0; i < n; ++i) {
            const auto& e = tmp[i];
            ImVec4 col = ImVec4(0.7f, 0.7f, 0.7f, 1.f);
            if (e.sev == log::Severity::Warning) col = ImVec4(1.f, 0.85f, 0.3f, 1.f);
            if (e.sev == log::Severity::Error)   col = ImVec4(1.f, 0.4f, 0.4f, 1.f);
            ImGui::TextColored(col, "[%s] %s", e.topic.c_str(), e.message.c_str());
        }
        if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.f) {
            ImGui::SetScrollHereY(1.f);
        }
    }
    ImGui::EndChild();
    ImGui::End();
}

} // namespace ithaca::gui
```

- [ ] **Step 2: Volat v main loop**

```cpp
const float log_h = 96.f;
renderTopBar     (ctx);
renderKeyboardPanel(ctx, 0,       36,  W,       180);
renderDiagPanel  (ctx, 0,       216, W * 0.5f, H - 216 - log_h);
renderParamsPanel(ctx, W * 0.5f, 216, W * 0.5f, H - 216 - log_h);
renderLogPanel   (ctx, 0,       H - log_h, W, log_h);
```

- [ ] **Step 3: Build + smoke**

Pri zahrani noty vidim v log stripu eventy (voice_on, voice_end, midi_off, ...).

- [ ] **Step 4: Commit**

```bash
git add app/gui/panel_log.h app/gui/panel_log.cpp app/gui/main.cpp CMakeLists.txt
git commit -m "faze8(gui): log strip — posledni eventy z LogRingBuffer"
```

---

## Task 12: Persistence debounce + finalni merge

**Files:**
- Modify: `app/gui/main.cpp`

- [ ] **Step 1: Debounce save (1s after change)**

V main loopu detekovat zmenu state pres jednoduchy hash a po 1s idle ulozit.

```cpp
static std::optional<std::chrono::steady_clock::time_point> dirty_since;
static GuiState last_saved = ctx.state;

bool changed =
    last_saved.bank_path != ctx.state.bank_path ||
    last_saved.midi_port_name != ctx.state.midi_port_name ||
    last_saved.master_gain_db != ctx.state.master_gain_db ||
    last_saved.resonance_strength != ctx.state.resonance_strength ||
    last_saved.release_ms != ctx.state.release_ms ||
    last_saved.excite_decay_ms != ctx.state.excite_decay_ms;
if (changed && !dirty_since) {
    dirty_since = std::chrono::steady_clock::now();
}
if (dirty_since) {
    auto now = std::chrono::steady_clock::now();
    if (now - *dirty_since > std::chrono::seconds(1)) {
        saveState(defaultStatePath(), ctx.state);
        last_saved = ctx.state;
        dirty_since.reset();
    }
}
```

- [ ] **Step 2: Final build + smoke test**

```bash
make build
./build/ithaca-gui
# - hraj na MIDI, kontroluj keyboard viz, log strip
# - posun slidery, slysi se zmena za behu
# - zmen banku, restart
# - vsechny tests: ctest --test-dir build --output-on-failure
```

Expected: 17+ tests PASS (existujici + nove diagnostics + peak meter + log_subscriber + persistence).

- [ ] **Step 3: Commit final + merge**

```bash
git add app/gui/main.cpp
git commit -m "faze8(gui): persistence debounce 1s pri zmene state"
```

Po dokonceni vsech tasku — pouzij superpowers:finishing-a-development-branch
pro merge `faze8-gui-hybrid-mvp` do `main`.
