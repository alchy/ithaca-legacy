# GUI Log-Level (dropdown + CLI flag) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Let the user choose the minimum logged severity (Debug/Info/Warn/Error/Fatal) in `ithaca-gui` via a startup `--log-level` flag and a runtime dropdown, persisted in `state.json`; move the noisy resonance debug logs to Debug so the default Info level is quiet and audio-thread cost drops when above Info.

**Architecture:** The logger (`engine/util/log.{h,cpp}`) already has an atomic, runtime-safe `setMinSeverity` and gates on severity *before* formatting (`vlogRT`/`vlog`). Work is: (1) add a `LOG_RT_DEBUG` macro and demote 3 resonance log lines to Debug; (2) extend GUI `GuiState`/persistence (schema v2, no backward compat) with a `log_level` string; (3) parse a `--log-level` startup flag; (4) apply the level in `AppContext::initFromState`; (5) add a runtime combo box in the Params panel. CLI is unchanged (it already has `--log-level`).

**Tech Stack:** C++20, Dear ImGui, doctest, CMake. Process-wide logger singleton `ithaca::log::Logger::default_()`.

---

## File Structure

| File | Responsibility | Change |
|------|----------------|--------|
| `engine/util/log.h` | log macros | add `LOG_RT_DEBUG` |
| `engine/resonance/resonance_engine.cpp` | resonance spawn/excite logging | `LOG_RT_INFO` → `LOG_RT_DEBUG` (EXCITE+, SPAWN) |
| `engine/voice/resonance_voice.cpp` | resonance voice start logging | `LOG_RT_INFO` → `LOG_RT_DEBUG` (START) |
| `app/gui/persistence.h` | GuiState struct | `schema_version=2`, `+ log_level` |
| `app/gui/persistence.cpp` | JSON load/save | load/save `log_level`, reject schema != 2 |
| `app/gui/main.cpp` | startup args + save debounce | `--log-level` flag, debounce compare |
| `app/gui/app_context.cpp` | engine/log init | `setMinSeverity` from state |
| `app/gui/panel_params.cpp` | params UI | runtime log-level combo |
| `tests/test_persistence.cpp` | persistence tests | `log_level` round-trip + schema-v1 reject |

Note: `app/gui/*.cpp` are compiled into the `ithaca-gui` target which only builds when ImGui/GLFW are vendored. The `test_persistence` target compiles `app/gui/persistence.cpp` directly (see `tests/CMakeLists.txt:115-120`) and does NOT depend on ImGui — persistence changes are testable headless.

---

## Task 1: Add `LOG_RT_DEBUG` macro

**Files:**
- Modify: `engine/util/log.h:164-166`

- [ ] **Step 1: Add the macro**

In `engine/util/log.h`, the RT macros currently are (lines 164-166):

```cpp
#define LOG_RT_INFO(comp_, ...)  ITHACA_LOG_RT_(::ithaca::log::Severity::Info,    comp_, __VA_ARGS__)
#define LOG_RT_WARN(comp_, ...)  ITHACA_LOG_RT_(::ithaca::log::Severity::Warning, comp_, __VA_ARGS__)
#define LOG_RT_ERROR(comp_, ...) ITHACA_LOG_RT_(::ithaca::log::Severity::Error,   comp_, __VA_ARGS__)
```

Add a Debug variant as the first of the group:

```cpp
#define LOG_RT_DEBUG(comp_, ...) ITHACA_LOG_RT_(::ithaca::log::Severity::Debug,   comp_, __VA_ARGS__)
#define LOG_RT_INFO(comp_, ...)  ITHACA_LOG_RT_(::ithaca::log::Severity::Info,    comp_, __VA_ARGS__)
#define LOG_RT_WARN(comp_, ...)  ITHACA_LOG_RT_(::ithaca::log::Severity::Warning, comp_, __VA_ARGS__)
#define LOG_RT_ERROR(comp_, ...) ITHACA_LOG_RT_(::ithaca::log::Severity::Error,   comp_, __VA_ARGS__)
```

- [ ] **Step 2: Build to verify it compiles**

Run: `cmake --build build -j 2>&1 | tail -3`
Expected: builds with no errors (macro is unused so far — fine).

- [ ] **Step 3: Commit**

```bash
git add engine/util/log.h
git commit -m "feat(log): add LOG_RT_DEBUG macro"
```

---

## Task 2: Demote resonance debug logs to Debug

**Files:**
- Modify: `engine/resonance/resonance_engine.cpp` (EXCITE+ ~line 123, SPAWN ~line 155)
- Modify: `engine/voice/resonance_voice.cpp` (START ~line 95)

These are diagnostic lines; moving them to Debug makes the default Info level quiet and makes them free on the audio thread above Debug.

- [ ] **Step 1: Demote EXCITE+ and SPAWN**

In `engine/resonance/resonance_engine.cpp`, change the EXCITE+ call from:

```cpp
            LOG_RT_INFO("resonance",
                "EXCITE+ played=%d N=%d harm=%.3f excite=%.4f cc64=%d damping[N]=%.3f",
```
to:
```cpp
            LOG_RT_DEBUG("resonance",
                "EXCITE+ played=%d N=%d harm=%.3f excite=%.4f cc64=%d damping[N]=%.3f",
```

And the SPAWN call from:

```cpp
        LOG_RT_INFO("resonance",
            "SPAWN  played=%d N=%d harm=%.3f excite=%.4f init_gain=%.4f cc64=%d damping[N]=%.3f",
```
to:
```cpp
        LOG_RT_DEBUG("resonance",
            "SPAWN  played=%d N=%d harm=%.3f excite=%.4f init_gain=%.4f cc64=%d damping[N]=%.3f",
```

- [ ] **Step 2: Demote START**

In `engine/voice/resonance_voice.cpp`, change the START call from:

```cpp
    LOG_RT_INFO("resonance_voice",
        "START midi=%d init_gain=%.4f mic_mode=%s pos=%d",
```
to:
```cpp
    LOG_RT_DEBUG("resonance_voice",
        "START midi=%d init_gain=%.4f mic_mode=%s pos=%d",
```

- [ ] **Step 3: Build and run full test suite (no regression)**

Run: `cmake --build build -j && ctest --test-dir build 2>&1 | tail -3`
Expected: build OK, `100% tests passed ... out of 23`.

- [ ] **Step 4: Commit**

```bash
git add engine/resonance/resonance_engine.cpp engine/voice/resonance_voice.cpp
git commit -m "refactor(log): demote resonance EXCITE+/SPAWN/START to Debug"
```

---

## Task 3: GuiState gains `log_level`, schema bumped to 2 (persistence + tests)

**Files:**
- Modify: `app/gui/persistence.h:11,18` (struct)
- Modify: `app/gui/persistence.cpp:102,103-114,131-143` (load/save)
- Test: `tests/test_persistence.cpp`

This task is fully testable headless via the `test_persistence` target. TDD: write the failing test first.

- [ ] **Step 1: Write the failing round-trip test**

In `tests/test_persistence.cpp`, inside `TEST_CASE("Persistence round-trip")`, after the line `s.window_x = 200; s.window_y = 300; s.window_w = 800; s.window_h = 600;` (line 20) add:

```cpp
    s.log_level = "debug";
```

And after the existing `CHECK(loaded->window_h == 600);` (line 33) add:

```cpp
    CHECK(loaded->log_level == "debug");
```

- [ ] **Step 2: Add a schema-v1-rejected test**

In `tests/test_persistence.cpp`, after `TEST_CASE("Persistence wrong schema_version")` (ends line 54) add a new test case:

```cpp
TEST_CASE("Persistence schema v1 odmitnuta (zadna zpetna kompatibilita)") {
    using namespace ithaca::gui;
    auto p = std::filesystem::temp_directory_path() / "ithaca_v1_schema.json";
    {
        std::ofstream f(p);
        f << "{\"schema_version\":1,\"bank_path\":\"/old/bank\"}\n";
    }
    CHECK_FALSE(loadState(p).has_value());   // v1 se zahodi → GUI nastartuje s defaulty
    std::filesystem::remove(p);
}
```

- [ ] **Step 3: Run tests — verify they FAIL**

Run: `cmake --build build --target test_persistence -j && ./build/tests/test_persistence`
Expected: FAIL. The round-trip fails because `GuiState` has no `log_level` member yet (compile error) — that is the expected red. (If the build dir layout differs, the binary is wherever CMake puts it; locate via `ctest --test-dir build -R test_persistence -V` after the implementation steps. For the RED check a compile error is acceptable proof the member is missing.)

- [ ] **Step 4: Add the struct field + bump schema default**

In `app/gui/persistence.h`, change line 11 from:

```cpp
    int         schema_version    = 1;
```
to:
```cpp
    int         schema_version    = 2;
```

And after line 18 (`std::string midi_port_name;`) add:

```cpp
    std::string log_level         = "info";   // debug|info|warn|error|fatal
```

- [ ] **Step 5: Update loadState — reject != 2, read log_level**

In `app/gui/persistence.cpp`, change line 102 from:

```cpp
        if (s.schema_version != 1) return std::nullopt;
```
to:
```cpp
        if (s.schema_version != 2) return std::nullopt;
```

And after line 105 (`s.midi_port_name = findValue(json, "midi_port_name");`) add:

```cpp
        s.log_level             = findValue(json, "log_level");
        if (s.log_level.empty()) s.log_level = "info";
```

- [ ] **Step 6: Update saveState — write log_level**

In `app/gui/persistence.cpp`, after line 134 (the `midi_port_name` write line ending in `<< "\",\n";`) add:

```cpp
        f << "  \"log_level\": \""       << jsonEscape(s.log_level)      << "\",\n";
```

- [ ] **Step 7: Run tests — verify they PASS**

Run: `cmake --build build --target test_persistence -j && ctest --test-dir build -R test_persistence --output-on-failure`
Expected: PASS (round-trip incl. log_level; v1 rejected).

- [ ] **Step 8: Run full suite (no regression)**

Run: `ctest --test-dir build 2>&1 | tail -3`
Expected: `100% tests passed`.

- [ ] **Step 9: Commit**

```bash
git add app/gui/persistence.h app/gui/persistence.cpp tests/test_persistence.cpp
git commit -m "feat(gui): persist log_level (state.json schema v2, no v1 compat)"
```

---

## Task 4: Apply log level in AppContext::initFromState

**Files:**
- Modify: `app/gui/app_context.cpp:36-45` (start of `initFromState`)

Apply the persisted level BEFORE `engine.init` so even bank-load logs honor it. `app_context.cpp` already includes `"util/log.h"`.

- [ ] **Step 1: Set min severity at the top of initFromState**

In `app/gui/app_context.cpp`, `initFromState` begins:

```cpp
bool AppContext::initFromState(const GuiState& s) {
    state = s;

    // Subscriber loggeru → ring buffer. ...
    log::Logger::default_().addSubscriber(
        [this](const log::LogEntry& e) { log_buf.push(e); });
```

Immediately after `state = s;` insert:

```cpp
    // Min severity z perzistovaneho/CLI-overridnuteho log_level. Nastavit PRED
    // engine.init(), aby i bank-load logy ctily zvolenou uroven.
    log::Logger::default_().setMinSeverity(
        log::severity_from_string(state.log_level.c_str(), log::Severity::Info));
```

- [ ] **Step 2: Build the GUI target**

Run: `cmake --build build --target ithaca-gui -j 2>&1 | tail -3`
Expected: builds with no errors. (If ImGui/GLFW are not vendored and `ithaca-gui` does not exist, run `cmake --build build -j 2>&1 | tail -3` and confirm no errors; the GUI code is then untested-by-build — note this and continue.)

- [ ] **Step 3: Commit**

```bash
git add app/gui/app_context.cpp
git commit -m "feat(gui): apply persisted log_level on init"
```

---

## Task 5: `--log-level` startup flag in GUI main

**Files:**
- Modify: `app/gui/main.cpp:31-37` (printUsage), `:42-62` (arg parse + override)

Mirror the existing `--bank-dir` override pattern. `main.cpp` includes `"persistence.h"`; it needs `"util/log.h"` only if it validates the level — we keep parsing as a plain string and let `severity_from_string` validate later in Task 4's code path, so no new include is required.

- [ ] **Step 1: Update printUsage**

In `app/gui/main.cpp`, `printUsage` currently is:

```cpp
static void printUsage(const char* argv0) {
    std::fprintf(stderr,
        "Pouziti: %s [--bank-dir <path>] [--help]\n"
        "  --bank-dir <path>  adresar s bankami (dropdown bude scanovat odtud);\n"
        "                     persistovano v state.json, staci zadat jednou.\n"
        "  --help, -h         tato napoveda\n", argv0);
}
```

Replace with:

```cpp
static void printUsage(const char* argv0) {
    std::fprintf(stderr,
        "Pouziti: %s [--bank-dir <path>] [--log-level <lvl>] [--help]\n"
        "  --bank-dir <path>  adresar s bankami (dropdown bude scanovat odtud);\n"
        "                     persistovano v state.json, staci zadat jednou.\n"
        "  --log-level <lvl>  debug | info | warn | error | fatal (default info);\n"
        "                     persistovano v state.json, menitelne i za behu v UI.\n"
        "  --help, -h         tato napoveda\n", argv0);
}
```

- [ ] **Step 2: Parse the flag**

In `app/gui/main.cpp`, the arg loop is:

```cpp
    std::string cli_bank_dir;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--help" || a == "-h") { printUsage(argv[0]); return 0; }
        if (a == "--bank-dir" && i + 1 < argc) {
            cli_bank_dir = argv[++i];
        } else {
            std::fprintf(stderr, "Neznama volba: %s\n", a.c_str());
            printUsage(argv[0]);
            return 1;
        }
    }
```

Replace with (adds `cli_log_level` and a branch; note the `--bank-dir` branch becomes `else if`):

```cpp
    std::string cli_bank_dir;
    std::string cli_log_level;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--help" || a == "-h") { printUsage(argv[0]); return 0; }
        if (a == "--bank-dir" && i + 1 < argc) {
            cli_bank_dir = argv[++i];
        } else if (a == "--log-level" && i + 1 < argc) {
            cli_log_level = argv[++i];
        } else {
            std::fprintf(stderr, "Neznama volba: %s\n", a.c_str());
            printUsage(argv[0]);
            return 1;
        }
    }
```

- [ ] **Step 3: Apply the override after state load**

In `app/gui/main.cpp`, the override block is:

```cpp
    // CLI override: --bank-dir nahrad persistovany bank_search_dir.
    if (!cli_bank_dir.empty()) st.bank_search_dir = cli_bank_dir;
```

Replace with:

```cpp
    // CLI override: --bank-dir nahrad persistovany bank_search_dir.
    if (!cli_bank_dir.empty()) st.bank_search_dir = cli_bank_dir;
    // CLI override: --log-level nahrad persistovany log_level (aplikuje se
    // v AppContext::initFromState pres setMinSeverity).
    if (!cli_log_level.empty()) st.log_level = cli_log_level;
```

- [ ] **Step 4: Build the GUI target**

Run: `cmake --build build --target ithaca-gui -j 2>&1 | tail -3`
Expected: builds with no errors. (If `ithaca-gui` target absent, build all and confirm no errors.)

- [ ] **Step 5: Commit**

```bash
git add app/gui/main.cpp
git commit -m "feat(gui): --log-level startup flag (overrides persisted)"
```

---

## Task 6: Runtime log-level dropdown in Params panel

**Files:**
- Modify: `app/gui/panel_params.cpp` (include + combo near other params)

Add a combo box. On change: write `ctx.state.log_level` and call `setMinSeverity` live. The string/label uses the same vocabulary as `severity_from_string` ("debug/info/warn/error/fatal").

- [ ] **Step 1: Add the log.h include**

In `app/gui/panel_params.cpp`, the includes are:

```cpp
#include "panel_params.h"
#include "app_context.h"
#include "imgui.h"
#include <cmath>
```

Replace with:

```cpp
#include "panel_params.h"
#include "app_context.h"
#include "imgui.h"
#include "util/log.h"
#include <cmath>
```

- [ ] **Step 2: Add the combo box before the Separator**

In `app/gui/panel_params.cpp`, find the block (after the disabled Max-resonance slider):

```cpp
    ImGui::Separator();
    if (ImGui::Button("Reset to defaults")) {
```

Insert this BEFORE the `ImGui::Separator();` line:

```cpp
    // Log level (runtime) — meni min severity loggeru za behu. Vyssi nez Info
    // = audio thread preskoci formatovani ladicich RT zprav (vykon).
    {
        static const char* kLevels[] = { "debug", "info", "warn", "error", "fatal" };
        int cur = 1;   // default info
        for (int i = 0; i < 5; ++i)
            if (ctx.state.log_level == kLevels[i]) { cur = i; break; }
        if (ImGui::Combo("Log level", &cur, kLevels, 5)) {
            ctx.state.log_level = kLevels[cur];
            log::Logger::default_().setMinSeverity(
                log::severity_from_string(ctx.state.log_level.c_str(),
                                          log::Severity::Info));
        }
    }

```

- [ ] **Step 3: Build the GUI target**

Run: `cmake --build build --target ithaca-gui -j 2>&1 | tail -3`
Expected: builds with no errors. (If `ithaca-gui` absent, build all; confirm no errors.)

- [ ] **Step 4: Commit**

```bash
git add app/gui/panel_params.cpp
git commit -m "feat(gui): runtime log-level dropdown in Params panel"
```

---

## Task 7: Persist log_level changes (save debounce)

**Files:**
- Modify: `app/gui/main.cpp:177-183` (the `changed` debounce comparison)

The render loop saves state 1s after the last change. Add `log_level` to the change detector so dropdown/flag changes get persisted.

- [ ] **Step 1: Add log_level to the change comparison**

In `app/gui/main.cpp`, the change detector is:

```cpp
        bool changed =
            last_saved.bank_path           != ctx.state.bank_path ||
            last_saved.midi_port_name      != ctx.state.midi_port_name ||
            last_saved.master_gain_db      != ctx.state.master_gain_db ||
            last_saved.resonance_strength  != ctx.state.resonance_strength ||
            last_saved.release_ms          != ctx.state.release_ms ||
            last_saved.excite_decay_ms     != ctx.state.excite_decay_ms;
```

Replace with (adds the `log_level` line):

```cpp
        bool changed =
            last_saved.bank_path           != ctx.state.bank_path ||
            last_saved.midi_port_name      != ctx.state.midi_port_name ||
            last_saved.master_gain_db      != ctx.state.master_gain_db ||
            last_saved.resonance_strength  != ctx.state.resonance_strength ||
            last_saved.release_ms          != ctx.state.release_ms ||
            last_saved.excite_decay_ms     != ctx.state.excite_decay_ms ||
            last_saved.log_level           != ctx.state.log_level;
```

- [ ] **Step 2: Build the GUI target**

Run: `cmake --build build --target ithaca-gui -j 2>&1 | tail -3`
Expected: builds with no errors.

- [ ] **Step 3: Commit**

```bash
git add app/gui/main.cpp
git commit -m "feat(gui): persist log_level on change (save debounce)"
```

---

## Task 8: Final verification

**Files:** none (verification only)

- [ ] **Step 1: Clean build + full suite**

Run: `cmake --build build -j 2>&1 | tail -5 && ctest --test-dir build 2>&1 | tail -3`
Expected: build OK (incl. `ithaca-gui` if vendored), `100% tests passed ... out of 23`.

- [ ] **Step 2: Manual smoke (if ImGui/GLFW available)**

Run: `./build/ithaca-gui --log-level debug --bank-dir /Users/j/SoundBanks/Ithaca`
Expected: GUI opens; Params panel shows a "Log level" combo defaulting to `debug`; playing notes prints `SPAWN`/`START`/`EXCITE+` lines. Switch the combo to `warn` → those lines stop. Close GUI, reopen WITHOUT the flag → combo reflects the last saved level (`warn`). Then `./build/ithaca-gui --log-level info` overrides it back to `info` at startup.

- [ ] **Step 3: Verify state.json schema**

Run: `cat "$HOME/Library/Application Support/ithaca-legacy/state.json"`
Expected: contains `"schema_version": 2` and `"log_level": "<level>"`.

---

## Notes for the implementer

- The logger is a **process-wide singleton** (`ithaca::log::Logger::default_()`); the Params panel and AppContext talk to it directly — do NOT route through `Engine`.
- `severity_from_string` accepts `debug|info|warn|warning|error|fatal` case-insensitively and falls back to the provided default on anything else (`engine/util/log.cpp:23-33`). The combo only emits the canonical 5 strings, so invalid values can't come from the UI — only from a hand-edited state.json, which falls back to Info safely.
- Do NOT commit `imgui.ini` (untracked GUI layout file, unrelated).
- `test_persistence` links `app/gui/persistence.cpp` directly and is ImGui-free, so Task 3 is verifiable on any machine. Tasks 4–7 touch ImGui-dependent translation units; if ImGui/GLFW are not vendored, they compile only as part of `ithaca-gui` (which won't exist) — in that case rely on the build of the full project and the headless persistence test, and note the GUI paths as build-verified-only.
