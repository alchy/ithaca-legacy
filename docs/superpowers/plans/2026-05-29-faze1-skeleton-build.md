# Faze 1 — Skeleton + build system + logger + smoke — implementacni plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development
> (doporuceno) nebo superpowers:executing-plans pro implementaci tohoto planu task-po-tasku.
> Kroky pouzivaji checkbox (`- [ ]`) syntaxi pro tracking.
>
> Pozn. k jazyku: komentare v kodu, docs i commit messages cesky bez diakritiky.
> Identifikatory anglicky. Komentovat spise vice (explicit > implicit).

**Goal:** Postavit kostru projektu ithaca-legacy s cross-platform build systemem (CMake +
Makefile), vendorovanymi zavislostmi, RT-safe loggerem a fungujicim headless `ithaca-cli`,
ktery overi smoke test.

**Architecture:** Projekt ma headless knihovnu `libithaca_core` (zatim jen logger + verze) a
tenkeho konzumenta `ithaca-cli`. Build je z icr2: CMake konfiguruje, top-level Makefile
orchestruje (auto-detekce platformy/generatoru/jader), zavislosti jsou source-vendored v
`third-party/` (zadny FetchContent). Testy pres doctest (single-header). Smoke test spusti
`ithaca-cli --selftest` a overi nenulovy exit + ocekavany vystup.

**Tech Stack:** C++20, CMake >=3.20, GNU Make, doctest (vendored), miniaudio + RtMidi +
nlohmann/json (vendored, zapojeni az v dalsich fazich — fetchnou se uz ted). Build a verifikace
zatim JEN na macOS (Apple clang); Win/Linux/Pi support se pise, ale neoveruje (uzivatel nema HW).

---

## Kontext a zdrojove vzory

Tento plan kopiruje OVERENE vzory z `/Users/j/Projects/icr2`. Kdyz si nejsi jisty stylem,
podivej se na tyto soubory (jsou funkcni):
- `/Users/j/Projects/icr2/Makefile` — top-level orchestrator (platform detection, barvy, smoke).
- `/Users/j/Projects/icr2/CMakeLists.txt` — root CMake (C++20, warning flags, lib + exe).
- `/Users/j/Projects/icr2/third-party/CMakeLists.txt` — vendored deps jako CMake targety.
- `/Users/j/Projects/icr2/tools/fetch-third-party.sh` — stahovani vendored deps (pinned verze).
- `/Users/j/Projects/icr2/engine/util/log.h` + `log.cpp` — RT-safe logger (adoptujeme styl).

Spec: `/Users/j/Projects/ithaca-legacy/docs/superpowers/specs/2026-05-29-ithaca-legacy-design.md`

Git repo uz existuje v `/Users/j/Projects/ithaca-legacy` (3 commity se specem). Pracuj v nem.

### Cilova struktura po fazi 1

```
ithaca-legacy/
  CMakeLists.txt            root CMake
  Makefile                  build orchestrator
  .gitignore
  config.json               default runtime config (zatim minimalni)
  engine/
    CMakeLists.txt          (volitelne — nebo vse v root CMake; viz Task 5)
    util/
      log.h                 RT-safe logger (deklarace)
      log.cpp               logger implementace
      version.h             ITHACA_VERSION string
  app/
    cli/
      main.cpp              ithaca-cli: --version, --selftest, --log-level
  third-party/
    CMakeLists.txt          vendored deps jako targety
    doctest/doctest.h       (fetchnuto)
    nlohmann_json/nlohmann/json.hpp   (fetchnuto)
    miniaudio/miniaudio.h   (fetchnuto)
    rtmidi/RtMidi.{cpp,h}   (fetchnuto)
  tools/
    fetch-third-party.sh    stahovani deps
  tests/
    CMakeLists.txt          doctest runner target
    test_log.cpp            unit testy loggeru
  docs/
    superpowers/{specs,plans}/   (uz existuje)
```

### Konvence pro vsechny commity v teto fazi

- Format: `typ(faze1): kratky popis` cesky bez diakritiky, napr. `feat(faze1): RT-safe logger`.
- Pridavej konkretni soubory (`git add path1 path2`), ne `git add -A`.
- Pridej trailer `Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>`.
- gpg podpis je v tomto repu vypnuty per-commit (`-c commit.gpgsign=false`), user.name nastav
  `-c user.name="Jindrich Nemec"` (icr2/icr commituji takto).

---

## Task 1: .gitignore + adresarova kostra

**Files:**
- Create: `.gitignore`
- Create: `engine/util/.gitkeep`, `app/cli/.gitkeep`, `tests/.gitkeep`, `third-party/.gitkeep`,
  `tools/.gitkeep` (placeholdery aby prazdne adresare sly do gitu; smazou se az tam budou soubory)

- [ ] **Step 1: Vytvor `.gitignore`**

```gitignore
# build artifacts
/build/
compile_commands.json

# vendored deps (stahuji se pres tools/fetch-third-party.sh, necommituji se)
/third-party/doctest/
/third-party/nlohmann_json/
/third-party/miniaudio/
/third-party/rtmidi/
/third-party/.staging/

# test outputs
/test-samples/

# OS / editor
.DS_Store
*.swp
*~

# logs
*.log
```

- [ ] **Step 2: Vytvor adresarovou kostru**

Run:
```bash
cd /Users/j/Projects/ithaca-legacy
mkdir -p engine/util app/cli tests third-party tools
touch engine/util/.gitkeep app/cli/.gitkeep tests/.gitkeep third-party/.gitkeep tools/.gitkeep
```

- [ ] **Step 3: Commit**

```bash
git add .gitignore engine/util/.gitkeep app/cli/.gitkeep tests/.gitkeep third-party/.gitkeep tools/.gitkeep
git -c user.name="Jindrich Nemec" -c commit.gpgsign=false commit -m "$(cat <<'EOF'
chore(faze1): gitignore + adresarova kostra projektu

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
EOF
)"
```

---

## Task 2: Fetch skript pro vendored zavislosti

**Files:**
- Create: `tools/fetch-third-party.sh`

Vendorujeme: doctest (testy), nlohmann/json (config + banky pozdeji), miniaudio (audio out
pozdeji), rtmidi (MIDI in pozdeji). Stahuji se uz ted, i kdyz se zapoji az v dalsich fazich —
aby byl build od zacatku self-contained. Skript je idempotentni (preskoci uz stazene).

- [ ] **Step 1: Vytvor `tools/fetch-third-party.sh`**

```bash
#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# fetch-third-party.sh
# Stahne vendored deps pro ithaca-legacy do third-party/.
# Spustit jednou po cerstvem clonu (nebo po bumpu verzi). Idempotentni —
# uz stazene preskoci. Verze jsou pinned nize; pro upgrade bumpni _VER + re-run.
# ---------------------------------------------------------------------------
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TP="$REPO_ROOT/third-party"
TMP="$TP/.staging"

# -- Pinned verze ----------------------------------------------------------
DOCTEST_VER="v2.4.11"
NLOHMANN_VER="v3.11.3"
MINIAUDIO_VER="0.11.21"
RTMIDI_VER="6.0.0"

have() { command -v "$1" >/dev/null 2>&1; }
log()  { printf "\033[1;34m[fetch]\033[0m %s\n" "$*"; }
err()  { printf "\033[1;31m[ERROR]\033[0m %s\n" "$*" >&2; exit 1; }

have curl || err "curl je potreba"

mkdir -p "$TMP"
trap 'rm -rf "$TMP"' EXIT

# -- doctest (single header) -----------------------------------------------
log "doctest $DOCTEST_VER"
mkdir -p "$TP/doctest"
if [ ! -f "$TP/doctest/doctest.h" ]; then
    curl -fsSL "https://raw.githubusercontent.com/doctest/doctest/${DOCTEST_VER}/doctest/doctest.h" \
        -o "$TP/doctest/doctest.h"
    curl -fsSL "https://raw.githubusercontent.com/doctest/doctest/${DOCTEST_VER}/LICENSE.txt" \
        -o "$TP/doctest/LICENSE.txt"
    log "  ok doctest"
else
    log "  (uz je, skip)"
fi

# -- nlohmann/json (single header) -----------------------------------------
log "nlohmann/json $NLOHMANN_VER"
mkdir -p "$TP/nlohmann_json/nlohmann"
if [ ! -f "$TP/nlohmann_json/nlohmann/json.hpp" ]; then
    curl -fsSL "https://github.com/nlohmann/json/releases/download/${NLOHMANN_VER}/json.hpp" \
        -o "$TP/nlohmann_json/nlohmann/json.hpp"
    curl -fsSL "https://raw.githubusercontent.com/nlohmann/json/${NLOHMANN_VER}/LICENSE.MIT" \
        -o "$TP/nlohmann_json/LICENSE.MIT"
    log "  ok nlohmann/json"
else
    log "  (uz je, skip)"
fi

# -- miniaudio (single header) ---------------------------------------------
log "miniaudio $MINIAUDIO_VER"
mkdir -p "$TP/miniaudio"
if [ ! -f "$TP/miniaudio/miniaudio.h" ]; then
    curl -fsSL "https://raw.githubusercontent.com/mackron/miniaudio/${MINIAUDIO_VER}/miniaudio.h" \
        -o "$TP/miniaudio/miniaudio.h"
    curl -fsSL "https://raw.githubusercontent.com/mackron/miniaudio/${MINIAUDIO_VER}/LICENSE" \
        -o "$TP/miniaudio/LICENSE"
    log "  ok miniaudio"
else
    log "  (uz je, skip)"
fi

# -- RtMidi (maly zdrojak) -------------------------------------------------
log "RtMidi $RTMIDI_VER"
mkdir -p "$TP/rtmidi"
if [ ! -f "$TP/rtmidi/RtMidi.cpp" ]; then
    curl -fsSL "https://github.com/thestk/rtmidi/archive/refs/tags/${RTMIDI_VER}.tar.gz" \
        -o "$TMP/rtmidi.tar.gz"
    mkdir -p "$TMP/rtmidi-tmp"
    tar -xzf "$TMP/rtmidi.tar.gz" -C "$TMP/rtmidi-tmp" --strip-components=1
    cp "$TMP/rtmidi-tmp/RtMidi.cpp" "$TMP/rtmidi-tmp/RtMidi.h" \
        "$TMP/rtmidi-tmp/LICENSE" "$TP/rtmidi/"
    log "  ok RtMidi"
else
    log "  (uz je, skip)"
fi

log "Hotovo. Vendored deps v third-party/"
```

- [ ] **Step 2: Udelej skript spustitelny a spust ho**

Run:
```bash
chmod +x tools/fetch-third-party.sh
bash tools/fetch-third-party.sh
```
Expected: vypise `[fetch] ok doctest`, `ok nlohmann/json`, `ok miniaudio`, `ok RtMidi`,
nakonec `Hotovo.`. Soubory `third-party/doctest/doctest.h`, `third-party/nlohmann_json/nlohmann/json.hpp`,
`third-party/miniaudio/miniaudio.h`, `third-party/rtmidi/RtMidi.cpp` existuji.

- [ ] **Step 3: Over ze deps existuji**

Run:
```bash
ls third-party/doctest/doctest.h third-party/nlohmann_json/nlohmann/json.hpp third-party/miniaudio/miniaudio.h third-party/rtmidi/RtMidi.cpp
```
Expected: vsechny 4 cesty vypsany bez chyby.

- [ ] **Step 4: Commit** (deps jsou v .gitignore, commituje se jen skript)

```bash
git add tools/fetch-third-party.sh
git -c user.name="Jindrich Nemec" -c commit.gpgsign=false commit -m "$(cat <<'EOF'
build(faze1): fetch skript pro vendored deps (doctest, json, miniaudio, rtmidi)

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
EOF
)"
```

---

## Task 3: Verze + RT-safe logger (TDD)

**Files:**
- Create: `engine/util/version.h`
- Create: `engine/util/log.h`
- Create: `engine/util/log.cpp`
- Test: `tests/test_log.cpp`

Logger je zjednodusena varianta icr2 loggeru (`/Users/j/Projects/icr2/engine/util/log.h`):
namespace `ithaca::log`, 5 severity, dual API (`log()` non-RT mutex-protected + `logRT()`
lock-free ring buffer), console + volitelny file, atomic severity filtr, `default_()` singleton,
printf-style makra. Timestampy a flush ring bufferu zachovavame.

- [ ] **Step 1: Napis selhavajici test `tests/test_log.cpp`**

```cpp
// tests/test_log.cpp
// Unit testy RT-safe loggeru. doctest single-header runner.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "util/log.h"
#include "util/version.h"

#include <string>

using namespace ithaca;

TEST_CASE("severity_from_string parsuje zname hodnoty") {
    CHECK(log::severity_from_string("debug")   == log::Severity::Debug);
    CHECK(log::severity_from_string("info")    == log::Severity::Info);
    CHECK(log::severity_from_string("warn")    == log::Severity::Warning);
    CHECK(log::severity_from_string("warning") == log::Severity::Warning);
    CHECK(log::severity_from_string("error")   == log::Severity::Error);
    CHECK(log::severity_from_string("fatal")   == log::Severity::Fatal);
}

TEST_CASE("severity_from_string vraci default pro neznamou hodnotu") {
    CHECK(log::severity_from_string("blabla", log::Severity::Warning)
          == log::Severity::Warning);
}

TEST_CASE("severity_to_string je nenulovy retezec") {
    CHECK(std::string(log::severity_to_string(log::Severity::Info)) == "INFO");
    CHECK(std::string(log::severity_to_string(log::Severity::Error)) == "ERROR");
}

TEST_CASE("setMinSeverity / getMinSeverity round-trip") {
    log::Logger lg;
    lg.setMinSeverity(log::Severity::Warning);
    CHECK(lg.getMinSeverity() == log::Severity::Warning);
}

TEST_CASE("logRT zapise do ring bufferu a flushRTBuffer ho vyprazdni") {
    log::Logger lg;
    lg.setMinSeverity(log::Severity::Debug);
    lg.setOutputMode(/*console=*/false, /*file=*/false);  // tichy pro test
    lg.logRT("test", log::Severity::Info, "ahoj %d", 42);
    int flushed = lg.flushRTBuffer();
    CHECK(flushed == 1);
    // Druhy flush uz nic nema
    CHECK(lg.flushRTBuffer() == 0);
}

TEST_CASE("logRT pretece pri prekroceni kapacity ringu a zahozene zpravy se pocitaji") {
    log::Logger lg;
    lg.setMinSeverity(log::Severity::Debug);
    lg.setOutputMode(/*console=*/false, /*file=*/false);  // tichy pro test
    // RT_BUFFER_SIZE je 1024 (privatni konstanta). Zapis 1500 zprav bez flushe.
    const int N = 1500;
    for (int i = 0; i < N; ++i)
        lg.logRT("test", log::Severity::Info, "zprava %d", i);
    // Ring pojme 1024 → flush vrati presne 1024, zbytek je zahozeny.
    CHECK(lg.flushRTBuffer() == 1024);
    CHECK(lg.rtDroppedCount() == (uint64_t)(N - 1024));
    // Po flushe je ring prazdny.
    CHECK(lg.flushRTBuffer() == 0);
}

TEST_CASE("version string neni prazdny") {
    CHECK(std::string(ITHACA_VERSION).size() > 0);
}
```

- [ ] **Step 2: Vytvor `engine/util/version.h`**

```cpp
#pragma once
// Verze projektu ithaca-legacy. Jeden zdroj pravdy pro --version i logy.
#define ITHACA_VERSION "0.1.0-faze1"
```

- [ ] **Step 3: Vytvor `engine/util/log.h`**

```cpp
#pragma once
// engine/util/log.h
// -----------------
// RT-safe logger pro ithaca-legacy. Adaptovany styl z icr2 (engine/util/log.h):
//   - 5 severity (Debug/Info/Warning/Error/Fatal)
//   - dual API: log() pro non-RT (mutex + varargs) a logRT() pro RT-safe
//     lock-free ring buffer (audio thread nesmi alokovat ani zamykat)
//   - console + volitelny file output, oba togglovatelne
//   - atomic severity filtr, menitelny za behu
//   - default_() singleton pro kod, ktery nedostane Logger& parametrem
//   - format: [HH:MM:SS.mmm] [component] [SEVERITY]: message
//
// Pouziti (non-RT):  LOG_INFO("bank", "nacteno: %s vrstvy=%d", path, n);
// Pouziti (RT):      LOG_RT_WARN("voice", "underrun na midi=%d", midi);
//                    // z non-RT threadu pravidelne: Logger::default_().flushRTBuffer();

#include <atomic>
#include <array>
#include <cstdarg>
#include <cstdint>
#include <fstream>
#include <mutex>
#include <string>

namespace ithaca::log {

enum class Severity : uint8_t {
    Debug = 0, Info = 1, Warning = 2, Error = 3, Fatal = 4,
};

const char* severity_to_string(Severity s);

// Parsuje "debug"/"info"/"warn"/"warning"/"error"/"fatal" (case-insensitive).
// Vraci default_value kdyz nelze parsovat.
Severity severity_from_string(const char* s,
                              Severity default_value = Severity::Info);

class Logger {
public:
    Logger();
    ~Logger();
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    // Process-wide default instance pouzivana LOG_* makry.
    static Logger& default_();

    // -- Konfigurace (thread-safe atomic) --
    void setMinSeverity(Severity s);
    Severity getMinSeverity() const;
    // console = stdout/stderr; file vyzaduje predchozi setLogFile().
    void setOutputMode(bool useConsole, bool useFile);
    bool setLogFile(const std::string& path);  // false on failure
    void closeLogFile();

    // -- Non-RT API (printf-style, mutex) --
    void log(const char* component, Severity severity, const char* format, ...)
#if defined(__GNUC__) || defined(__clang__)
        __attribute__((format(printf, 4, 5)))
#endif
        ;
    void vlog(const char* component, Severity severity,
              const char* format, va_list args);

    // -- RT-safe API (SPSC ring buffer) --
    // Single-producer: logRT smi volat jen JEDEN (audio/RT) thread. Format do
    // entry, pak release-publish write indexu. Zadne alokace, zadne mutexy. RT
    // thread NIKDY neblokuje: kdyz je ring plny (flush z non-RT threadu nestiha),
    // zprava se ZAHODI a zapocita do rtDroppedCount(). Caller MUSI pravidelne
    // volat flushRTBuffer() z non-RT threadu, jinak rostou drop-y.
    void logRT(const char* component, Severity severity, const char* format, ...)
#if defined(__GNUC__) || defined(__clang__)
        __attribute__((format(printf, 4, 5)))
#endif
        ;
    void vlogRT(const char* component, Severity severity,
                const char* format, va_list args);
    // Vyprazdni ring buffer → console + file. Volat z non-RT threadu.
    // Vraci pocet flushnutych zprav.
    int flushRTBuffer();
    // Pocet RT zprav zahozenych kvuli plnemu ring bufferu (flush nestihal).
    uint64_t rtDroppedCount() const { return rt_dropped_.load(std::memory_order_relaxed); }

private:
    static constexpr size_t COMPONENT_MAX  = 32;
    static constexpr size_t MESSAGE_MAX    = 256;
    static constexpr size_t RT_BUFFER_SIZE = 1024;

    struct Entry {
        char     component[COMPONENT_MAX];
        char     message[MESSAGE_MAX];
        uint64_t timestamp_us;
        Severity severity;
        Entry() : timestamp_us(0), severity(Severity::Info) {
            component[0] = '\0';
            message[0]   = '\0';
        }
    };

    std::array<Entry, RT_BUFFER_SIZE> rt_buffer_;
    std::atomic<size_t> rt_write_idx_{0};
    std::atomic<size_t> rt_read_idx_{0};
    std::atomic<uint64_t> rt_dropped_{0};

    std::string           log_file_path_;
    std::ofstream         log_file_;
    mutable std::mutex    log_mutex_;
    std::atomic<Severity> min_severity_{Severity::Info};
    std::atomic<bool>     use_console_{true};
    std::atomic<bool>     use_file_{false};

    bool shouldLog(Severity s) const;
    void writeEntry(const char* component, Severity severity,
                    const char* message, uint64_t timestamp_us);
    static uint64_t nowMicros();
    static std::string formatTimestamp(uint64_t micros);
};

} // namespace ithaca::log

// -- Convenience makra (printf-style) --
#define ITHACA_LOG_(sev_, comp_, ...) \
    ::ithaca::log::Logger::default_().log((comp_), (sev_), __VA_ARGS__)

#define LOG_DEBUG(comp_, ...) ITHACA_LOG_(::ithaca::log::Severity::Debug,   comp_, __VA_ARGS__)
#define LOG_INFO(comp_, ...)  ITHACA_LOG_(::ithaca::log::Severity::Info,    comp_, __VA_ARGS__)
#define LOG_WARN(comp_, ...)  ITHACA_LOG_(::ithaca::log::Severity::Warning, comp_, __VA_ARGS__)
#define LOG_ERROR(comp_, ...) ITHACA_LOG_(::ithaca::log::Severity::Error,   comp_, __VA_ARGS__)
#define LOG_FATAL(comp_, ...) ITHACA_LOG_(::ithaca::log::Severity::Fatal,   comp_, __VA_ARGS__)

#define ITHACA_LOG_RT_(sev_, comp_, ...) \
    ::ithaca::log::Logger::default_().logRT((comp_), (sev_), __VA_ARGS__)

#define LOG_RT_INFO(comp_, ...)  ITHACA_LOG_RT_(::ithaca::log::Severity::Info,    comp_, __VA_ARGS__)
#define LOG_RT_WARN(comp_, ...)  ITHACA_LOG_RT_(::ithaca::log::Severity::Warning, comp_, __VA_ARGS__)
#define LOG_RT_ERROR(comp_, ...) ITHACA_LOG_RT_(::ithaca::log::Severity::Error,   comp_, __VA_ARGS__)
```

- [ ] **Step 4: Vytvor `engine/util/log.cpp`**

```cpp
// engine/util/log.cpp — implementace RT-safe loggeru (viz log.h).
#include "util/log.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>

namespace ithaca::log {

const char* severity_to_string(Severity s) {
    switch (s) {
        case Severity::Debug:   return "DEBUG";
        case Severity::Info:    return "INFO";
        case Severity::Warning: return "WARNING";
        case Severity::Error:   return "ERROR";
        case Severity::Fatal:   return "FATAL";
    }
    return "INFO";
}

Severity severity_from_string(const char* s, Severity default_value) {
    if (!s) return default_value;
    std::string v;
    for (const char* p = s; *p; ++p) v += (char)std::tolower((unsigned char)*p);
    if (v == "debug")              return Severity::Debug;
    if (v == "info")               return Severity::Info;
    if (v == "warn" || v == "warning") return Severity::Warning;
    if (v == "error")              return Severity::Error;
    if (v == "fatal")              return Severity::Fatal;
    return default_value;
}

Logger::Logger() {}
Logger::~Logger() { closeLogFile(); }

Logger& Logger::default_() {
    static Logger instance;   // Meyers singleton — thread-safe init v C++11+
    return instance;
}

void Logger::setMinSeverity(Severity s) {
    min_severity_.store(s, std::memory_order_relaxed);
}
Severity Logger::getMinSeverity() const {
    return min_severity_.load(std::memory_order_relaxed);
}
void Logger::setOutputMode(bool useConsole, bool useFile) {
    use_console_.store(useConsole, std::memory_order_relaxed);
    use_file_.store(useFile, std::memory_order_relaxed);
}

bool Logger::setLogFile(const std::string& path) {
    std::lock_guard<std::mutex> lk(log_mutex_);
    if (log_file_.is_open()) log_file_.close();
    log_file_.open(path, std::ios::app);
    if (!log_file_.is_open()) return false;
    log_file_path_ = path;
    return true;
}
void Logger::closeLogFile() {
    std::lock_guard<std::mutex> lk(log_mutex_);
    if (log_file_.is_open()) log_file_.close();
    log_file_path_.clear();
}

bool Logger::shouldLog(Severity s) const {
    return (uint8_t)s >= (uint8_t)min_severity_.load(std::memory_order_relaxed);
}

uint64_t Logger::nowMicros() {
    using namespace std::chrono;
    return (uint64_t)duration_cast<microseconds>(
        system_clock::now().time_since_epoch()).count();
}

std::string Logger::formatTimestamp(uint64_t micros) {
    std::time_t secs = (std::time_t)(micros / 1000000ULL);
    int ms = (int)((micros / 1000ULL) % 1000ULL);
    std::tm tmv{};
#if defined(_WIN32)
    localtime_s(&tmv, &secs);
#else
    localtime_r(&secs, &tmv);
#endif
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d.%03d",
                  tmv.tm_hour, tmv.tm_min, tmv.tm_sec, ms);
    return std::string(buf);
}

void Logger::writeEntry(const char* component, Severity severity,
                        const char* message, uint64_t timestamp_us) {
    // Caller drzi log_mutex_ pro non-RT cestu; pro flush taky.
    std::string line = "[" + formatTimestamp(timestamp_us) + "] ["
                     + component + "] [" + severity_to_string(severity)
                     + "]: " + message + "\n";
    if (use_console_.load(std::memory_order_relaxed)) {
        std::FILE* out = ((uint8_t)severity >= (uint8_t)Severity::Warning)
                       ? stderr : stdout;
        std::fputs(line.c_str(), out);
        std::fflush(out);
    }
    if (use_file_.load(std::memory_order_relaxed) && log_file_.is_open()) {
        log_file_ << line;
        log_file_.flush();
    }
}

void Logger::log(const char* component, Severity severity,
                 const char* format, ...) {
    va_list args;
    va_start(args, format);
    vlog(component, severity, format, args);
    va_end(args);
}

void Logger::vlog(const char* component, Severity severity,
                  const char* format, va_list args) {
    if (!shouldLog(severity)) return;
    char buf[MESSAGE_MAX];
    std::vsnprintf(buf, sizeof(buf), format, args);
    std::lock_guard<std::mutex> lk(log_mutex_);
    writeEntry(component, severity, buf, nowMicros());
}

void Logger::logRT(const char* component, Severity severity,
                   const char* format, ...) {
    va_list args;
    va_start(args, format);
    vlogRT(component, severity, format, args);
    va_end(args);
}

void Logger::vlogRT(const char* component, Severity severity,
                    const char* format, va_list args) {
    if (!shouldLog(severity)) return;
    // SPSC: sem zapisuje jen jediny (audio/RT) thread, ten je vlastnikem
    // write indexu. Synchronizace se ctenarem (flushRTBuffer) je pres publish
    // write indexu nize (release parovan s acquire ve flush).
    const size_t w = rt_write_idx_.load(std::memory_order_relaxed);
    const size_t r = rt_read_idx_.load(std::memory_order_acquire);
    if (w - r >= RT_BUFFER_SIZE) {
        // Ring je plny — flush z non-RT threadu nestiha. RT thread NESMI
        // blokovat, takze zpravu zahodime a jen zvedneme citac pro diagnostiku.
        rt_dropped_.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    Entry& e = rt_buffer_[w % RT_BUFFER_SIZE];
    std::vsnprintf(e.message, MESSAGE_MAX, format, args);
    std::strncpy(e.component, component, COMPONENT_MAX - 1);
    e.component[COMPONENT_MAX - 1] = '\0';
    e.severity     = severity;
    e.timestamp_us = nowMicros();
    // Publikuj az kdyz je cely entry zapsany — release zaruci, ze ctenar,
    // ktery uvidi novy write index (acquire), uvidi i kompletni data entry.
    rt_write_idx_.store(w + 1, std::memory_order_release);
}

int Logger::flushRTBuffer() {
    std::lock_guard<std::mutex> lk(log_mutex_);
    size_t r = rt_read_idx_.load(std::memory_order_relaxed);
    const size_t w = rt_write_idx_.load(std::memory_order_acquire);
    int flushed = 0;
    while (r < w) {
        Entry& e = rt_buffer_[r % RT_BUFFER_SIZE];
        writeEntry(e.component, e.severity, e.message, e.timestamp_us);
        ++r;
        ++flushed;
    }
    rt_read_idx_.store(r, std::memory_order_release);
    return flushed;
}

} // namespace ithaca::log
```

Pozn. — SPSC drop-on-full design (oprava race z code review):
Puvodni vlogRT rezervoval slot pres `fetch_add % SIZE` a NIKDY nekontroloval,
zda slot jeste neobsahuje neprectenou zpravu → pri preteceni (> RT_BUFFER_SIZE
zapisu pred flushem) producent prepsal slot, ktery flusher soubezne cetl =
data race / torn read. Per-entry `ready` flag to neresil. Oprava: korektni
SPSC ring (jeden RT producent = audio thread, jeden konzument = flushRTBuffer
serializovany mutexem). Synchronizace je pres publish write indexu
(release/acquire), `ready` flag uplne odstranen. Na plnem ringu se zprava
zahodi a zapocita do `rt_dropped_` (citac dostupny pres `rtDroppedCount()`).

- [ ] **Step 5: Commit** (build a spusteni testu prijde az v Task 5–6, kdy existuje CMake)

```bash
git add engine/util/version.h engine/util/log.h engine/util/log.cpp tests/test_log.cpp
git -c user.name="Jindrich Nemec" -c commit.gpgsign=false commit -m "$(cat <<'EOF'
feat(faze1): RT-safe logger + version + unit testy (jeste nebuildeno)

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
EOF
)"
```

---

## Task 4: ithaca-cli (--version, --selftest, --log-level)

**Files:**
- Create: `app/cli/main.cpp`

CLI je zatim tenke — overuje, ze se lib linkuje a logger funguje. `--selftest` provede par
log volani + flush ring bufferu a vrati 0 pri uspechu (pouziva ho `make smoke`).

- [ ] **Step 1: Vytvor `app/cli/main.cpp`**

```cpp
// app/cli/main.cpp — ithaca-cli
// ------------------------------
// Tenky headless konzument libithaca_core. Ve fazi 1 jen overuje, ze se
// knihovna linkuje a logger funguje:
//   --version           vypise verzi a skonci
//   --log-level <lvl>    nastavi min severity (debug/info/warn/error/fatal)
//   --selftest           provede self-test loggeru, vrati 0 pri uspechu
//
// V dalsich fazich sem pribyde nacteni banky, prehravani not, atd.
#include "util/log.h"
#include "util/version.h"

#include <cstdio>
#include <cstring>
#include <string>

using namespace ithaca;

static void printUsage(const char* argv0) {
    std::printf(
        "ithaca-cli %s\n"
        "\n"
        "Pouziti:\n"
        "  %s --version\n"
        "  %s --selftest [--log-level <lvl>]\n"
        "\n"
        "Volby:\n"
        "  --version            vypise verzi a skonci\n"
        "  --log-level <lvl>    debug | info | warn | error | fatal (default info)\n"
        "  --selftest           self-test loggeru (exit 0 = OK)\n"
        "  --help, -h           tato napoveda\n",
        ITHACA_VERSION, argv0, argv0);
}

int main(int argc, char* argv[]) {
    bool do_selftest = false;
    log::Severity level = log::Severity::Info;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--version") {
            std::printf("ithaca-cli %s\n", ITHACA_VERSION);
            return 0;
        } else if (a == "--help" || a == "-h") {
            printUsage(argv[0]);
            return 0;
        } else if (a == "--log-level" && i + 1 < argc) {
            level = log::severity_from_string(argv[++i], log::Severity::Info);
        } else if (a == "--selftest") {
            do_selftest = true;
        } else {
            std::fprintf(stderr, "Neznama volba: %s\n\n", a.c_str());
            printUsage(argv[0]);
            return 1;
        }
    }

    auto& L = log::Logger::default_();
    L.setMinSeverity(level);
    L.setOutputMode(/*console=*/true, /*file=*/false);

    if (!do_selftest) {
        printUsage(argv[0]);
        return 0;
    }

    // -- Self-test: par log volani na obou kanalech + flush ring bufferu --
    LOG_INFO("selftest", "ithaca-cli %s startuje self-test", ITHACA_VERSION);
    LOG_DEBUG("selftest", "debug zprava (viditelna jen pri --log-level debug)");
    LOG_WARN("selftest", "warning zprava jde na stderr");
    LOG_RT_INFO("selftest", "RT zprava cislo %d z audio-like kontextu", 1);
    int flushed = L.flushRTBuffer();
    LOG_INFO("selftest", "flushnuto %d RT zprav", flushed);

    if (flushed < 1) {
        LOG_ERROR("selftest", "ocekaval jsem aspon 1 RT zpravu, dostal %d", flushed);
        return 1;
    }
    LOG_INFO("selftest", "self-test OK");
    return 0;
}
```

- [ ] **Step 2: Commit**

```bash
git add app/cli/main.cpp
git -c user.name="Jindrich Nemec" -c commit.gpgsign=false commit -m "$(cat <<'EOF'
feat(faze1): ithaca-cli s --version/--selftest/--log-level

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
EOF
)"
```

---

## Task 5: CMake build (root + third-party + tests)

**Files:**
- Create: `third-party/CMakeLists.txt`
- Create: `CMakeLists.txt` (root)
- Create: `tests/CMakeLists.txt`

Layout: root CMake postavi `libithaca_core` (zatim jen `engine/util/log.cpp`), `ithaca-cli`
a (pres CTest) test runner. Include root je `engine/`, takze `#include "util/log.h"` funguje.
Vendored deps jsou INTERFACE/STATIC targety v `third-party/`.

- [ ] **Step 1: Vytvor `third-party/CMakeLists.txt`**

```cmake
# third-party/CMakeLists.txt
# --------------------------
# Vendored deps jako CMake targety. Zadny FetchContent — vse source-vendored
# pres tools/fetch-third-party.sh. Targety:
#   doctest         INTERFACE  (header-only, jen pro testy)
#   nlohmann_json   INTERFACE  (header-only, config + banky v dalsich fazich)
#   miniaudio_inc   INTERFACE  (header-only, audio out v dalsich fazich)
#   rtmidi          STATIC     (MIDI in v dalsich fazich)

# -- doctest (header-only) --
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/doctest/doctest.h")
    add_library(doctest INTERFACE)
    target_include_directories(doctest INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/doctest)
else()
    message(FATAL_ERROR
        "doctest/doctest.h chybi. Spust: ./tools/fetch-third-party.sh")
endif()

# -- nlohmann/json (header-only) --
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/nlohmann_json/nlohmann/json.hpp")
    add_library(nlohmann_json INTERFACE)
    target_include_directories(nlohmann_json INTERFACE
        ${CMAKE_CURRENT_SOURCE_DIR}/nlohmann_json)
else()
    message(FATAL_ERROR
        "nlohmann_json/nlohmann/json.hpp chybi. Spust: ./tools/fetch-third-party.sh")
endif()

# -- miniaudio (header-only) --
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/miniaudio/miniaudio.h")
    add_library(miniaudio_inc INTERFACE)
    target_include_directories(miniaudio_inc INTERFACE
        ${CMAKE_CURRENT_SOURCE_DIR}/miniaudio)
endif()

# -- RtMidi (static) — platform backend defs --
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/rtmidi/RtMidi.cpp")
    add_library(rtmidi STATIC rtmidi/RtMidi.cpp)
    target_include_directories(rtmidi PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/rtmidi)
    if(APPLE)
        target_compile_definitions(rtmidi PRIVATE __MACOSX_CORE__)
        target_link_libraries(rtmidi PRIVATE
            "-framework CoreMIDI" "-framework CoreFoundation" "-framework CoreAudio")
    elseif(UNIX)
        target_compile_definitions(rtmidi PRIVATE __LINUX_ALSA__)
        target_link_libraries(rtmidi PRIVATE asound pthread)
    elseif(WIN32)
        target_compile_definitions(rtmidi PRIVATE __WINDOWS_MM__)
        target_link_libraries(rtmidi PRIVATE winmm)
    endif()
endif()
```

- [ ] **Step 2: Vytvor root `CMakeLists.txt`**

```cmake
cmake_minimum_required(VERSION 3.20)
project(ithaca_legacy LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Export compile_commands.json pro clangd (LSP) — editor pak vidi stejne
# include paths jako build a nehlasi spurious "file not found".
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(MSVC)
    add_compile_options(/W4 /permissive- /Zc:preprocessor)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS NOMINMAX)
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

# -- vendored deps --
add_subdirectory(third-party)

# -- libithaca_core (headless knihovna) --
# Ve fazi 1 obsahuje jen logger. V dalsich fazich pribydou sample/voice/dsp moduly.
add_library(ithaca_core STATIC
    engine/util/log.cpp
)
target_include_directories(ithaca_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/engine)
target_compile_features(ithaca_core PUBLIC cxx_std_20)

# -- ithaca-cli (headless konzument) --
add_executable(ithaca-cli app/cli/main.cpp)
target_link_libraries(ithaca-cli PRIVATE ithaca_core)
set_target_properties(ithaca-cli PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

# -- testy (doctest pres CTest) --
enable_testing()
add_subdirectory(tests)
```

- [ ] **Step 3: Vytvor `tests/CMakeLists.txt`**

```cmake
# tests/CMakeLists.txt — doctest unit testy
add_executable(ithaca_tests test_log.cpp)
target_link_libraries(ithaca_tests PRIVATE ithaca_core doctest)
add_test(NAME ithaca_tests COMMAND ithaca_tests)
```

- [ ] **Step 4: Configure + build pres raw CMake**

Run:
```bash
cd /Users/j/Projects/ithaca-legacy
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
```
Expected: build skonci bez chyb; vzniknou `build/ithaca-cli` a `build/tests/ithaca_tests`
(presne umisteni test binarky muze byt `build/tests/ithaca_tests`).

- [ ] **Step 5: Spust testy pres CTest**

Run:
```bash
cd /Users/j/Projects/ithaca-legacy && ctest --test-dir build --output-on-failure
```
Expected: `100% tests passed, 0 tests failed out of 1`.

- [ ] **Step 6: Over selftest binarky**

Run:
```bash
./build/ithaca-cli --version
./build/ithaca-cli --selftest; echo "exit=$?"
```
Expected: prvni vypise `ithaca-cli 0.1.0-faze1`; druhy vypise log radky vc. `self-test OK`
a `exit=0`.

- [ ] **Step 7: Commit**

```bash
git add CMakeLists.txt third-party/CMakeLists.txt tests/CMakeLists.txt
git -c user.name="Jindrich Nemec" -c commit.gpgsign=false commit -m "$(cat <<'EOF'
build(faze1): CMake — libithaca_core + ithaca-cli + doctest testy

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
EOF
)"
```

---

## Task 6: Makefile orchestrator + config.json + smoke

**Files:**
- Create: `Makefile`
- Create: `config.json`

Makefile je zjednoduseny port icr2 orchestratoru: platform/generator/jobs auto-detekce,
cile `help/check-tools/fetch-third-party/configure/build/rebuild/test/smoke/clean`. Smoke
spusti `ithaca-cli --selftest`. Python cile vynechavame (faze 1 nema Python).

- [ ] **Step 1: Vytvor `config.json`** (minimalni, rozsiri se v dalsich fazich)

```json
{
  "preload_ms": 150,
  "resonance_window_ms": 500,
  "cache_budget_mb": 2400,
  "max_voices": 128,
  "stream_threads": 2,
  "render_threads": 0,
  "midi_channel": 0,
  "log_level": "info"
}
```

- [ ] **Step 2: Vytvor `Makefile`**

```makefile
# ithaca-legacy — top-level Makefile
# -----------------------------------
# Build orchestrator: auto-detekce platformy / generatoru / jader.
# Cile: help (default) / check-tools / fetch-third-party / configure /
#       build / rebuild / test / smoke / clean / info
#
# Override-able promenne: BUILD_DIR, BUILD_TYPE, GENERATOR, JOBS
# Priklady:
#   make check-tools && make fetch-third-party && make build && make smoke
#   make BUILD_TYPE=Debug build
#   make GENERATOR=Ninja configure

.DEFAULT_GOAL := help
.DELETE_ON_ERROR:

# -- Platform detection --
ifeq ($(OS),Windows_NT)
    PLATFORM := Windows
    EXE      := .exe
    NULL     := NUL
    RM_RF    := cmake -E rm -rf
    MKDIR_P  := cmake -E make_directory
else
    UNAME_S := $(shell uname -s 2>/dev/null)
    ifeq ($(UNAME_S),Darwin)
        PLATFORM := macOS
    else ifeq ($(UNAME_S),Linux)
        PLATFORM := Linux
    else
        PLATFORM := Unknown
    endif
    EXE      :=
    NULL     := /dev/null
    RM_RF    := rm -rf
    MKDIR_P  := mkdir -p
endif

BUILD_DIR  ?= build
BUILD_TYPE ?= Release

ifeq ($(PLATFORM),macOS)
    JOBS ?= $(shell sysctl -n hw.ncpu 2>/dev/null || echo 4)
else ifeq ($(PLATFORM),Linux)
    JOBS ?= $(shell nproc 2>/dev/null || echo 4)
else
    JOBS ?= 4
endif

# Generator: Ninja kdyz je v PATH, jinak Unix Makefiles (na non-Windows).
ifeq ($(GENERATOR),)
    HAS_NINJA := $(shell command -v ninja 2>$(NULL) >$(NULL) && echo yes)
    ifeq ($(HAS_NINJA),yes)
        GENERATOR := Ninja
    else
        GENERATOR := Unix Makefiles
    endif
endif

.PHONY: help
help:
	@printf "\nithaca-legacy — build orchestrator\n"
	@printf "  Platforma: $(PLATFORM)   Generator: $(GENERATOR)   Jobs: $(JOBS)\n\n"
	@printf "Cile:\n"
	@printf "  make check-tools        over cmake / ninja\n"
	@printf "  make fetch-third-party  stahni vendored deps\n"
	@printf "  make configure          CMake configure -> $(BUILD_DIR)/\n"
	@printf "  make build              zkompiluj vse\n"
	@printf "  make rebuild            clean + configure + build\n"
	@printf "  make test               spust doctest pres ctest\n"
	@printf "  make smoke              ithaca-cli --selftest\n"
	@printf "  make clean              smaze $(BUILD_DIR)/\n"
	@printf "  make info               vypis detekovane hodnoty\n\n"

.PHONY: info
info:
	@printf "PLATFORM=$(PLATFORM) GENERATOR=$(GENERATOR) BUILD_TYPE=$(BUILD_TYPE) JOBS=$(JOBS)\n"

.PHONY: check-tools
check-tools:
	@cmake --version >$(NULL) 2>&1 || (printf "cmake neni v PATH (brew install cmake)\n" && exit 1)
	@printf "cmake OK: "; cmake --version | head -1
	@command -v ninja >$(NULL) 2>&1 && printf "ninja OK\n" || printf "ninja chybi (fallback $(GENERATOR))\n"

.PHONY: fetch-third-party
fetch-third-party:
	@bash tools/fetch-third-party.sh

.PHONY: configure
configure:
	@cmake -S . -B $(BUILD_DIR) -G "$(GENERATOR)" -DCMAKE_BUILD_TYPE=$(BUILD_TYPE)

$(BUILD_DIR)/CMakeCache.txt:
	@$(MAKE) --no-print-directory configure

.PHONY: build
build: $(BUILD_DIR)/CMakeCache.txt
	@cmake --build $(BUILD_DIR) --config $(BUILD_TYPE) --parallel $(JOBS)
	@printf "Build OK. Binarka: $(BUILD_DIR)/ithaca-cli$(EXE)\n"

.PHONY: rebuild
rebuild: clean configure build

.PHONY: test
test: build
	@ctest --test-dir $(BUILD_DIR) --output-on-failure

.PHONY: smoke
smoke: build
	@printf "Smoke test — ithaca-cli --selftest\n"
	@if [ -x $(BUILD_DIR)/ithaca-cli$(EXE) ]; then \
	    $(BUILD_DIR)/ithaca-cli$(EXE) --selftest; \
	else \
	    printf "ithaca-cli nenalezen v $(BUILD_DIR)/\n" && exit 1; \
	fi
	@printf "Smoke OK.\n"

.PHONY: clean
clean:
	@$(RM_RF) $(BUILD_DIR)
	@printf "Build dir smazan.\n"
```

POZOR: v Makefile musi byt odsazeni recept-radku TABULATOREM, ne mezerami.

- [ ] **Step 3: Over `make` cile na cisto**

Run:
```bash
cd /Users/j/Projects/ithaca-legacy
make clean
make check-tools
make build
```
Expected: `check-tools` vypise `cmake OK`; `make build` skonci `Build OK. Binarka: build/ithaca-cli`.

- [ ] **Step 4: Spust `make test` a `make smoke`**

Run:
```bash
make test
make smoke
```
Expected: `make test` → `100% tests passed`; `make smoke` → log radky + `self-test OK` + `Smoke OK.`.

- [ ] **Step 5: Commit**

```bash
git add Makefile config.json
git -c user.name="Jindrich Nemec" -c commit.gpgsign=false commit -m "$(cat <<'EOF'
build(faze1): Makefile orchestrator + default config.json + smoke test

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
EOF
)"
```

---

## Task 7: README + ocisteni .gitkeep placeholderu

**Files:**
- Create: `README.md`
- Delete: `.gitkeep` soubory v adresarich, ktere uz maji realne soubory

- [ ] **Step 1: Vytvor `README.md`**

```markdown
# ithaca-legacy

Sample player s jedinym core (sample core). Cilem je nizko-latencni
crossplatform prehravac vlastnich pianovych samplu s dynamickymi velocity
sloty, round-robin, sympatetickou rezonanci a half-pedalingem; jadro pojede
i na Raspberry Pi se streamovanim samplu z disku.

Navrh: `docs/superpowers/specs/2026-05-29-ithaca-legacy-design.md`.

## Build (macOS)

Jednorazove stazeni vendored deps:

    make fetch-third-party

Build + test + smoke:

    make build
    make test
    make smoke

Vyber Debug/Release: `make BUILD_TYPE=Debug build`. Napoveda: `make`.

> Pozn.: cross-platform (Windows/Linux/Raspberry Pi) je cil a build soubory
> ho nesou, ale zatim se overuje jen na macOS.

## Struktura

    engine/    headless knihovna libithaca_core (zatim util/logger)
    app/cli/   ithaca-cli — headless konzument
    third-party/  vendored deps (fetch-third-party.sh)
    tests/     doctest unit testy
    docs/      dokumentace + specs/plans

## Jazyk

Dokumentace a komentare v kodu jsou cesky bez diakritiky; identifikatory
anglicky. Princip: explicit je lepsi nez implicit.
```

- [ ] **Step 2: Smaz .gitkeep tam, kde uz jsou realne soubory**

Run:
```bash
cd /Users/j/Projects/ithaca-legacy
rm -f engine/util/.gitkeep app/cli/.gitkeep tests/.gitkeep third-party/.gitkeep tools/.gitkeep
```

- [ ] **Step 3: Over cely cyklus na cisto (finalni verifikace faze 1)**

Run:
```bash
make clean && make build && make test && make smoke
```
Expected: build OK, `100% tests passed`, `self-test OK`, `Smoke OK.`.

- [ ] **Step 4: Commit**

```bash
git add README.md
git add -u engine/util app/cli tests third-party tools
git -c user.name="Jindrich Nemec" -c commit.gpgsign=false commit -m "$(cat <<'EOF'
docs(faze1): README + odstraneni .gitkeep placeholderu

Co-Authored-By: Claude Opus 4.7 <noreply@anthropic.com>
EOF
)"
```

---

## Hotovo — kriteria dokonceni faze 1

- `make fetch-third-party` stahne deps (doctest, json, miniaudio, rtmidi).
- `make build` zkompiluje `libithaca_core` + `ithaca-cli` bez chyb a warningu na macOS.
- `make test` → vsechny doctest testy projdou (logger, severity parsing, ring buffer).
- `make smoke` → `ithaca-cli --selftest` vrati exit 0 s `self-test OK`.
- `./build/ithaca-cli --version` vypise verzi.
- Repo ma cistou strukturu (engine/app/third-party/tests/tools/docs), .gitignore ignoruje
  build/ a vendored deps.

Tim je polozena kostra, na kterou faze 2 (loader + bank_index + sample_store) navesi nacitani
legacy banky a mereni RMS.
