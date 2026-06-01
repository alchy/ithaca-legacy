# ithaca-gui Art Deco Redesign Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Redesign ithaca-gui into an Art Deco interface (silver = structure, gold = primary/live accents) with a centralized theme + reusable widget layer (icr2-style), a 3-column main layout (BANK | VOICE | DSP RACK), continuous sustain/peak indicators, MIDI port reload + channel/OMNI filter, and a reserved DSP RACK placeholder for the future chain.

**Architecture:** Mirror icr2's GUI structure (same ImGui+GLFW+OpenGL3 stack): `theme.h` (color tokens + fonts + `apply_theme`/`load_fonts`), `widgets.h` (Eyebrow/LabelValue/Indicator/ParamSlider/Card/DrawHBar/DrawKeyboard/grid ticks), and one file per panel taking `AppContext& ctx`. The only engine change is a MIDI channel filter (today the callback discards the channel nibble). Fonts load from disk via a CWD-relative path (Cormorant TTF vendored in `third-party/cormorant/`). Fixed-px layout (no Hi-DPI yet), no glow.

**Tech Stack:** C++20, Dear ImGui 1.91.0 (no docking branch), GLFW, OpenGL3, doctest, CMake. Cormorant (OFL) serif font.

---

## Visual tokens (single source of truth — used across all tasks)

Palette (silver structure, gold accent — "scheme A"):
```
bg        #0d0e10   (near-black window)
bg_panel  #090a0b   (log strip / deepest)
ink       #e2e6ea   (brightest silver — key values)
silver    #cdd2d6   (default text)
silver2   #aab0b6   (light silver — section titles, master/peak)
line      #3a4046   (hairline dividers)
line_soft #242a2f   (faint inner dividers)
muted     #7e858c   (dimmed labels / eyebrow)
gold      #d4af37   (ACCENT ONLY: logo, VOICES value, MIDI NOTE lamp, SUSTAIN fill,
                     RESONANCE slider grab, active keys, DSP LED, LEGACY badge, grid ticks)
```
Gold is used ONLY on the listed live/primary elements. RESONANCE *value* is silver (secondary); VOICES *value* is gold (primary).

Fonts (Cormorant, loaded at multiple sizes; log strip stays mono):
```
body     18px  (Cormorant, default text)        GlyphExtraAdvanceX 0
eyebrow  11px  (small uppercase labels)          GlyphExtraAdvanceX 1.5
value    26px  (stat tile numbers)               GlyphExtraAdvanceX 0
brand    20px  (logo "ITHACA")                    GlyphExtraAdvanceX 6
```
(Cormorant runs larger than Inter at same px; 18/26 read well. Adjust at smoke test if needed.)

---

## File Structure

| File | Responsibility | Action |
|------|----------------|--------|
| `app/gui/theme.h` | color tokens, fonts, apply_theme, load_fonts, asset-path finder | CREATE |
| `app/gui/widgets.h` | Eyebrow/LabelValue/Indicator/ParamSlider/Card/DrawHBar/DrawKeyboard/GridTick | CREATE |
| `third-party/cormorant/Cormorant-Medium.ttf` (+ OFL.txt) | vendored font | ADD |
| `engine/midi/midi_input.h` / `.cpp` | + channel filter (setChannel + callback compare) | MODIFY |
| `tests/test_midi_channel.cpp` | channel filter unit test | CREATE |
| `app/gui/persistence.h` / `.cpp` | + `midi_channel` field, schema 2→3 | MODIFY |
| `tests/test_persistence.cpp` | midi_channel round-trip + schema reject | MODIFY |
| `app/gui/main.cpp` | apply_theme/load_fonts at init; 3-column layout; grid ticks | MODIFY |
| `app/gui/panel_topbar.cpp` | logo + MIDI dropdown + reload + channel; remove bank | MODIFY |
| `app/gui/panel_bank.{h,cpp}` | NEW bank column (select + type badge + facts + reload) | CREATE |
| `app/gui/panel_indicators.{h,cpp}` | MIDI lamps + sustain bar + diag tiles + peak bars | CREATE (from diag) |
| `app/gui/panel_diag.{h,cpp}` | superseded by panel_indicators | DELETE |
| `app/gui/panel_params.cpp` | restyle sliders via widgets | MODIFY |
| `app/gui/panel_keyboard.cpp` | restyle via DrawKeyboard | MODIFY |
| `app/gui/panel_dsp.{h,cpp}` | NEW DSP RACK placeholder column | CREATE |
| `app/gui/panel_log.cpp` | restyle (muted, mono) | MODIFY |
| `CMakeLists.txt` | add panel_bank/indicators/dsp to ithaca-gui sources; test_midi_channel | MODIFY |

---

## PHASE 1 — Theme foundation

### Task 1: Vendor Cormorant font + asset-path finder + theme.h colors

**Files:**
- Add: `third-party/cormorant/Cormorant-Medium.ttf`, `third-party/cormorant/OFL.txt`
- Create: `app/gui/theme.h`

- [ ] **Step 1: Vendor the font**

Download Cormorant (OFL) — `Cormorant-Medium.ttf` — into `third-party/cormorant/`, plus its `OFL.txt` license. Source: Google Fonts (https://fonts.google.com/specimen/Cormorant) or the GitHub repo (CatharsisFonts/Cormorant). Verify the file is a valid TTF:

Run: `ls -l third-party/cormorant/ && file third-party/cormorant/Cormorant-Medium.ttf`
Expected: the `.ttf` exists and `file` reports `TrueType Font data` (or `OpenType`). If only a variable font is available, use a static Medium instance.

If the font cannot be fetched in this environment, STOP and report NEEDS_CONTEXT — the rest of Phase 1 needs it. (The code falls back to the default ImGui font if the TTF is missing, but the visual goal requires Cormorant.)

- [ ] **Step 2: Decide gitignore — DO commit the font**

The font is small and required at runtime. Confirm it will be committed (it must NOT match a `.gitignore` rule). `third-party/*` subdirs ARE gitignored (vendored deps fetched by script), so add an explicit un-ignore.

In `.gitignore`, after the existing `/third-party/...` block, add:
```
# Cormorant font IS committed (small, required at runtime; unlike fetched deps)
!/third-party/cormorant/
```
Run: `git check-ignore third-party/cormorant/Cormorant-Medium.ttf; echo "exit=$?"`
Expected: `exit=1` (NOT ignored). If exit=0, the un-ignore rule didn't take — fix the pattern.

- [ ] **Step 3: Create theme.h with colors + asset finder (no fonts yet)**

Create `app/gui/theme.h`:
```cpp
#pragma once
// app/gui/theme.h — Art Deco theme: barevne tokeny (stribro=struktura,
// zlato=zivy akcent), fonty (Cormorant), apply_theme/load_fonts. Header-only,
// vzor prevzat z icr2 player/gui/theme.h.
#include "imgui.h"
#include <cstdint>
#include <fstream>
#include <string>

namespace ithaca::gui::theme {

// -- Barevne tokeny (schema A) -------------------------------------------
struct Colors {
    static constexpr ImU32 bg        = IM_COL32(0x0d, 0x0e, 0x10, 255);
    static constexpr ImU32 bg_panel  = IM_COL32(0x09, 0x0a, 0x0b, 255);
    static constexpr ImU32 ink       = IM_COL32(0xe2, 0xe6, 0xea, 255); // jasne stribro
    static constexpr ImU32 silver    = IM_COL32(0xcd, 0xd2, 0xd6, 255); // default text
    static constexpr ImU32 silver2   = IM_COL32(0xaa, 0xb0, 0xb6, 255); // titulky, master, peak
    static constexpr ImU32 line      = IM_COL32(0x3a, 0x40, 0x46, 255);
    static constexpr ImU32 line_soft = IM_COL32(0x24, 0x2a, 0x2f, 255);
    static constexpr ImU32 muted     = IM_COL32(0x7e, 0x85, 0x8c, 255); // eyebrow/dim
    static constexpr ImU32 gold      = IM_COL32(0xd4, 0xaf, 0x37, 255); // AKCENT only

    static ImVec4 v(ImU32 c) {
        return ImVec4(( c        & 0xFF) / 255.f,
                      ((c >>  8) & 0xFF) / 255.f,
                      ((c >> 16) & 0xFF) / 255.f,
                      ((c >> 24) & 0xFF) / 255.f);
    }
};

// Najdi asset relativne k CWD (vzor icr2 util::find_asset_path). Vraci prazdne
// kdyz nenalezeno → load_fonts spadne na default ImGui font.
inline std::string find_asset_path(const std::string& rel) {
    auto exists = [](const std::string& p) {
        std::ifstream f(p); return f.good();
    };
    if (exists(rel)) return rel;
    static const char* prefixes[] = {
        "./third-party/", "./", "../third-party/",
    };
    for (const char* pre : prefixes) {
        std::string c = std::string(pre) + rel;
        if (exists(c)) return c;
    }
    return {};
}

} // namespace ithaca::gui::theme
```

- [ ] **Step 4: Build (theme.h unused so far — just must compile when included)**

Add a throwaway include check: `cmake --build build --target ithaca-gui -j 2>&1 | tail -3` will NOT yet compile theme.h (not included anywhere). Instead verify syntax by a quick compile:
Run: `echo '#include "app/gui/theme.h"
int main(){return (int)ithaca::gui::theme::Colors::gold;}' > /tmp/t.cpp && c++ -std=c++20 -I third-party/imgui -I app/gui -fsyntax-only /tmp/t.cpp && echo OK`
Expected: `OK` (imgui.h on the include path; if imgui needs more includes, this is fine — we only need theme.h to parse; if it errors only on imgui internals, skip and rely on Step in Task with real integration).

- [ ] **Step 5: Commit**
```bash
git add third-party/cormorant/ .gitignore app/gui/theme.h
git commit -m "feat(gui): vendor Cormorant + theme.h color tokens + asset finder"
```

### Task 2: theme.h fonts + apply_theme

**Files:**
- Modify: `app/gui/theme.h`

- [ ] **Step 1: Add Fonts struct + load_fonts + apply_theme**

In `app/gui/theme.h`, before the closing `} // namespace`, add:
```cpp
// -- Fonty (Cormorant @ vice velikosti) ----------------------------------
struct Fonts {
    static inline ImFont* body    = nullptr; // 18px
    static inline ImFont* eyebrow = nullptr; // 11px, +1.5 tracking
    static inline ImFont* value   = nullptr; // 26px stat cisla
    static inline ImFont* brand   = nullptr; // 20px logo, +6 tracking
};

inline void load_fonts(const std::string& ttf_path) {
    if (Fonts::body) return;
    ImGuiIO& io = ImGui::GetIO();
    if (ttf_path.empty()) {
        Fonts::body = io.Fonts->AddFontDefault();
        Fonts::eyebrow = Fonts::value = Fonts::brand = Fonts::body;
        return;
    }
    static const ImWchar ranges[] = {
        0x0020, 0x00FF, // Latin + Latin-1 (CZ + bösendorf ö)
        0x0100, 0x017F, // Latin Extended-A
        0x2010, 0x205E, // punctuation (dashes, …)
        0x2190, 0x21FF, // arrows (↻ U+21BB rescan)
        0x25A0, 0x25FF, // geometric (● ○ indikatory)
        0,
    };
    ImFontConfig cfg;
    cfg.OversampleH = 2; cfg.OversampleV = 2; cfg.PixelSnapH = true;

    cfg.GlyphExtraAdvanceX = 0.f;
    Fonts::body = io.Fonts->AddFontFromFileTTF(ttf_path.c_str(), 18.f, &cfg, ranges);
    cfg.GlyphExtraAdvanceX = 1.5f;
    Fonts::eyebrow = io.Fonts->AddFontFromFileTTF(ttf_path.c_str(), 11.f, &cfg, ranges);
    cfg.GlyphExtraAdvanceX = 0.f;
    Fonts::value = io.Fonts->AddFontFromFileTTF(ttf_path.c_str(), 26.f, &cfg, ranges);
    cfg.GlyphExtraAdvanceX = 6.f;
    Fonts::brand = io.Fonts->AddFontFromFileTTF(ttf_path.c_str(), 20.f, &cfg, ranges);

    if (!Fonts::body) { // load failed → default
        Fonts::body = io.Fonts->AddFontDefault();
        Fonts::eyebrow = Fonts::value = Fonts::brand = Fonts::body;
    } else {
        if (!Fonts::eyebrow) Fonts::eyebrow = Fonts::body;
        if (!Fonts::value)   Fonts::value   = Fonts::body;
        if (!Fonts::brand)   Fonts::brand   = Fonts::body;
    }
}

inline void apply_theme() {
    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowRounding = s.ChildRounding = s.FrameRounding = 0.f;
    s.GrabRounding = s.PopupRounding = s.ScrollbarRounding = s.TabRounding = 0.f;
    s.WindowPadding    = ImVec2(0, 0);   // panely si delaji vlastni padding
    s.FramePadding     = ImVec2(8, 4);
    s.ItemSpacing      = ImVec2(10, 8);
    s.ItemInnerSpacing = ImVec2(6, 4);
    s.ScrollbarSize    = 8.f;
    s.WindowBorderSize = s.ChildBorderSize = s.FrameBorderSize = 0.f;
    s.GrabMinSize      = 4.f;

    auto& c = s.Colors;
    c[ImGuiCol_WindowBg]       = Colors::v(Colors::bg);
    c[ImGuiCol_ChildBg]        = Colors::v(Colors::bg);
    c[ImGuiCol_PopupBg]        = Colors::v(Colors::bg_panel);
    c[ImGuiCol_Border]         = Colors::v(Colors::line);
    c[ImGuiCol_BorderShadow]   = ImVec4(0,0,0,0);
    c[ImGuiCol_Text]           = Colors::v(Colors::silver);
    c[ImGuiCol_TextDisabled]   = Colors::v(Colors::muted);
    c[ImGuiCol_FrameBg]        = Colors::v(Colors::line_soft);
    c[ImGuiCol_FrameBgHovered] = Colors::v(Colors::line);
    c[ImGuiCol_FrameBgActive]  = Colors::v(Colors::line);
    c[ImGuiCol_SliderGrab]       = Colors::v(Colors::silver2);
    c[ImGuiCol_SliderGrabActive] = Colors::v(Colors::ink);
    c[ImGuiCol_Button]         = ImVec4(0,0,0,0);
    c[ImGuiCol_ButtonHovered]  = Colors::v(Colors::line_soft);
    c[ImGuiCol_ButtonActive]   = Colors::v(Colors::line);
    c[ImGuiCol_Header]         = Colors::v(Colors::line_soft);
    c[ImGuiCol_HeaderHovered]  = Colors::v(Colors::line);
    c[ImGuiCol_HeaderActive]   = Colors::v(Colors::line);
    c[ImGuiCol_Separator]      = Colors::v(Colors::line_soft);
    c[ImGuiCol_ScrollbarBg]    = ImVec4(0,0,0,0);
    c[ImGuiCol_ScrollbarGrab]  = Colors::v(Colors::line);
}
```

- [ ] **Step 2: Commit**
```bash
git add app/gui/theme.h
git commit -m "feat(gui): theme.h fonts (Cormorant) + apply_theme"
```

### Task 3: Wire theme into main.cpp init

**Files:**
- Modify: `app/gui/main.cpp` (after `ImGui::CreateContext()`, replace `ImGui::StyleColorsDark()`)

- [ ] **Step 1: Replace StyleColorsDark with apply_theme + load_fonts**

In `app/gui/main.cpp`, the init block currently is:
```cpp
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();
    ImGui_ImplGlfw_InitForOpenGL(w, true);
    ImGui_ImplOpenGL3_Init("#version 150");
```
Replace with:
```cpp
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    {
        ithaca::gui::theme::apply_theme();
        std::string ttf = ithaca::gui::theme::find_asset_path("cormorant/Cormorant-Medium.ttf");
        if (ttf.empty())
            std::fprintf(stderr, "WARN: Cormorant TTF nenalezen — default font.\n");
        ithaca::gui::theme::load_fonts(ttf);
        ImGuiIO& io = ImGui::GetIO();
        if (ithaca::gui::theme::Fonts::body) io.FontDefault = ithaca::gui::theme::Fonts::body;
    }
    ImGui_ImplGlfw_InitForOpenGL(w, true);
    ImGui_ImplOpenGL3_Init("#version 150");
```
And add near the top includes: `#include "theme.h"`.

- [ ] **Step 2: Build the GUI**

Run: `cmake --build build --target ithaca-gui -j 2>&1 | tail -4`
Expected: builds clean. (theme.h is now compiled via main.cpp.)

- [ ] **Step 3: Full suite (no regression)**

Run: `ctest --test-dir build 2>&1 | tail -3`
Expected: `100% tests passed` (theme change doesn't touch tested code).

- [ ] **Step 4: Manual smoke**

Run from repo root so `./third-party/cormorant/...` resolves: `./build/ithaca-gui --bank-dir /Users/j/SoundBanks/Ithaca`
Expected: window opens with dark `#0d0e10` background and Cormorant text (panels still in old positions — restyle comes later). Close it.

- [ ] **Step 5: Commit**
```bash
git add app/gui/main.cpp
git commit -m "feat(gui): apply Art Deco theme + Cormorant fonts at startup"
```

---

## PHASE 2 — Widget library

### Task 4: widgets.h — text/indicator/slider/card + bars + keyboard + grid tick

**Files:**
- Create: `app/gui/widgets.h`

- [ ] **Step 1: Create widgets.h**

Create `app/gui/widgets.h` (uses theme.h tokens; all header-only inline; draws via ImDrawList for bars/keyboard):
```cpp
#pragma once
// app/gui/widgets.h — Art Deco widget vrstva. Vzor icr2 player/gui/widgets.h,
// adaptovano na nase tokeny (stribro/zlato) a nase indikatory (sustain/peak
// vodorovne, half-pedal ryska, 88-key klaviatura, grid rysky).
#include "theme.h"
#include "imgui.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

namespace ithaca::gui::wdg {
using theme::Colors;
using theme::Fonts;

// Eyebrow: maly prostrkany uppercase popisek.
inline void Eyebrow(const char* t, ImU32 col = Colors::muted) {
    if (Fonts::eyebrow) ImGui::PushFont(Fonts::eyebrow);
    ImGui::PushStyleColor(ImGuiCol_Text, Colors::v(col));
    ImGui::TextUnformatted(t);
    ImGui::PopStyleColor();
    if (Fonts::eyebrow) ImGui::PopFont();
}

// StatTile: eyebrow popisek nad velkou hodnotou. value_col rozhoduje zlato/stribro.
inline void StatTile(const char* label, const char* value, ImU32 value_col) {
    Eyebrow(label);
    if (Fonts::value) ImGui::PushFont(Fonts::value);
    ImGui::PushStyleColor(ImGuiCol_Text, Colors::v(value_col));
    ImGui::TextUnformatted(value);
    ImGui::PopStyleColor();
    if (Fonts::value) ImGui::PopFont();
}

// Lampa: ● rozsvicena (col) / ○ zhasla (muted).
inline void Lamp(const char* label, bool on, ImU32 on_col = Colors::gold) {
    if (Fonts::eyebrow) ImGui::PushFont(Fonts::eyebrow);
    ImGui::PushStyleColor(ImGuiCol_Text, Colors::v(on ? on_col : Colors::muted));
    ImGui::Text("%s %s", on ? "\xE2\x97\x8F" : "\xE2\x97\x8B", label); // ● / ○
    ImGui::PopStyleColor();
    if (Fonts::eyebrow) ImGui::PopFont();
}

// ParamSlider: eyebrow label + plnosirkovy slider. grab barva pres caller theme.
inline bool ParamSliderF(const char* label, float* v, float lo, float hi,
                         const char* fmt = "%.2f") {
    Eyebrow(label);
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
    return ImGui::SliderFloat((std::string("##") + label).c_str(), v, lo, hi, fmt);
}

// Vodorovny dvojity gradient fill bar s volitelnou ryskou. Pouziti: sustain
// (jeden, threshold ryska), peak L/R (dva, bez rysky). Kresli na aktualni
// cursor, rezervuje (width x h).
inline void HBar(float frac01, float width, float h,
                 ImU32 fill_lo, ImU32 fill_hi,
                 float tick01 = -1.f) {
    frac01 = std::clamp(frac01, 0.f, 1.f);
    if (width <= 0) width = ImGui::GetContentRegionAvail().x;
    ImVec2 o = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(width, h));
    auto* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(o, ImVec2(o.x + width, o.y + h), Colors::v(Colors::line_soft));
    if (frac01 > 0.f)
        dl->AddRectFilledMultiColor(
            o, ImVec2(o.x + width * frac01, o.y + h),
            fill_lo, fill_hi, fill_hi, fill_lo); // horizontalni gradient
    if (tick01 >= 0.f) {
        float tx = o.x + width * std::clamp(tick01, 0.f, 1.f);
        dl->AddLine(ImVec2(tx, o.y - 2.f), ImVec2(tx, o.y + h + 2.f),
                    Colors::silver2, 1.f);
    }
}

// Kratka zlata grid ryska (na krizeni sloupcove drahy s vodorovnym delitkem).
// Volat s absolutni X (screen), Y = aktualni delitko.
inline void GridTick(float screen_x, float screen_y, float len = 9.f) {
    ImGui::GetForegroundDrawList()->AddLine(
        ImVec2(screen_x, screen_y), ImVec2(screen_x, screen_y + len),
        Colors::gold, 1.f);
}

// 88-key klaviatura (MIDI 21..108). active = maska, gain_fn vraci 0..1 pro jas.
// Aktivni klavesa zlata. Kresli do (width x height) na aktualni cursor.
template <typename ActiveFn, typename GainFn>
inline void Keyboard(float width, float height, ActiveFn active, GainFn gain_fn) {
    constexpr int FIRST = 21, LAST = 108;
    auto is_black = [](int m){ int n=m%12; return n==1||n==3||n==6||n==8||n==10; };
    int n_white = 0; for (int m=FIRST;m<=LAST;++m) if(!is_black(m)) ++n_white;
    if (width <= 0) width = ImGui::GetContentRegionAvail().x;
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(width, height));
    float kw = width / (float)n_white, bw = kw*0.6f, bh = height*0.62f;
    auto* dl = ImGui::GetWindowDrawList();
    int wi = 0;
    for (int m=FIRST;m<=LAST;++m){ if(is_black(m)) continue;
        float kx=p.x+wi*kw; ImU32 col = active(m) ? Colors::gold : IM_COL32(0x1c,0x20,0x24,255);
        dl->AddRectFilled({kx,p.y},{kx+kw-1.f,p.y+height},col);
        dl->AddLine({kx+kw-1.f,p.y},{kx+kw-1.f,p.y+height},Colors::line,0.5f);
        ++wi; }
    wi = 0;
    for (int m=FIRST;m<=LAST;++m){ if(!is_black(m)){++wi;continue;}
        float kx=p.x+(wi-1)*kw+kw-bw*0.5f;
        ImU32 col = active(m) ? IM_COL32(0x8a,0x73,0x30,255) : Colors::bg;
        dl->AddRectFilled({kx,p.y},{kx+bw,p.y+bh},col); }
    (void)gain_fn;
}

} // namespace ithaca::gui::wdg
```

- [ ] **Step 2: Syntax-compile check**

Run: `c++ -std=c++20 -I third-party/imgui -I app/gui -fsyntax-only -include app/gui/widgets.h -x c++ /dev/null && echo OK || echo "see errors"`
Expected: `OK` (or only imgui-internal notes). If `AddRectFilledMultiColor` signature differs in 1.91, fix arg order per `third-party/imgui/imgui.h`.

- [ ] **Step 3: Commit**
```bash
git add app/gui/widgets.h
git commit -m "feat(gui): widgets.h (Eyebrow/StatTile/Lamp/ParamSlider/HBar/Keyboard/GridTick)"
```

---

## PHASE 3 — Engine: MIDI channel filter (TDD) + persistence

### Task 5: MIDI channel filter in engine (TDD)

**Files:**
- Modify: `engine/midi/midi_input.h` (add `setChannel`, member), `engine/midi/midi_input.cpp` (callback compare)
- Create: `tests/test_midi_channel.cpp`
- Modify: `CMakeLists.txt` (register test), `tests/CMakeLists.txt`

The callback today (`midi_input.cpp:95-98`) reads `status & 0xF0` and ignores `status & 0x0F` (channel). We add a channel filter: `channel_ == -1` (OMNI) passes all; otherwise only messages whose low nibble equals `channel_`.

Because the RtMidi callback is static and hard to invoke in a unit test, extract the channel-accept decision into a free, testable function.

- [ ] **Step 1: Write the failing test**

Create `tests/test_midi_channel.cpp`:
```cpp
// tests/test_midi_channel.cpp — channel filter accept logika.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"
#include "midi/midi_input.h"

using namespace ithaca;

TEST_CASE("OMNI (-1) prijima vsechny kanaly") {
    CHECK(MidiInput::channelAccepts(-1, 0x90)); // ch1 note-on
    CHECK(MidiInput::channelAccepts(-1, 0x9F)); // ch16 note-on
    CHECK(MidiInput::channelAccepts(-1, 0xB3)); // ch4 CC
}

TEST_CASE("Konkretni kanal prijima jen svuj") {
    // channel_ je 0-based interne: 0 = MIDI kanal 1.
    CHECK(MidiInput::channelAccepts(0, 0x90));        // ch1 note-on → pass
    CHECK_FALSE(MidiInput::channelAccepts(0, 0x91));  // ch2 → reject
    CHECK(MidiInput::channelAccepts(3, 0xB3));        // ch4 CC → pass
    CHECK_FALSE(MidiInput::channelAccepts(3, 0xB4));  // ch5 → reject
}
```

- [ ] **Step 2: Run — verify it FAILS (compile error: no channelAccepts)**

Add the CMake target first so it compiles. In `tests/CMakeLists.txt`, after the test_persistence block, add:
```cmake
add_executable(test_midi_channel test_midi_channel.cpp)
target_link_libraries(test_midi_channel PRIVATE ithaca_core doctest)
add_test(NAME test_midi_channel COMMAND test_midi_channel)
```
Run: `cmake -S . -B build >/dev/null && cmake --build build --target test_midi_channel -j 2>&1 | tail -5`
Expected: FAIL — `channelAccepts` not a member of MidiInput.

- [ ] **Step 3: Implement channelAccepts + setChannel + member**

In `engine/midi/midi_input.h`, add to the public section (after `listPorts`):
```cpp
    // Channel filtr: -1 = OMNI (vse), 0..15 = jen ten MIDI kanal (0-based).
    void setChannel(int ch) { channel_ = (ch < 0 || ch > 15) ? -1 : ch; }
    int  channel() const { return channel_; }
    // Cista testovatelna logika: prijmout zpravu se status bytem `status`
    // pri zvolenem `channel` (-1 OMNI)? Channel = status & 0x0F.
    static bool channelAccepts(int channel, uint8_t status) {
        if (channel < 0) return true;
        return (int)(status & 0x0F) == channel;
    }
```
Add `#include <cstdint>` at the top includes, and in the private members add:
```cpp
    int channel_ = -1;  // OMNI default
```

In `engine/midi/midi_input.cpp` callback, after `const uint8_t status = (*msg)[0];` (line ~95) add:
```cpp
    if (!channelAccepts(self->channel_, status)) return;
```
(`self` is the MidiInput*; `channel_` is a member — accessible since callback is a static member function.)

- [ ] **Step 4: Run — verify PASS**

Run: `cmake --build build --target test_midi_channel -j && ctest --test-dir build -R test_midi_channel --output-on-failure`
Expected: PASS (both cases).

- [ ] **Step 5: Full suite**

Run: `ctest --test-dir build 2>&1 | tail -3`
Expected: all pass (count +1 target).

- [ ] **Step 6: Commit**
```bash
git add engine/midi/midi_input.h engine/midi/midi_input.cpp tests/test_midi_channel.cpp tests/CMakeLists.txt
git commit -m "feat(midi): channel/OMNI filter (channelAccepts + setChannel)"
```

### Task 6: persistence — midi_channel field, schema 2→3 (TDD)

**Files:**
- Modify: `app/gui/persistence.h` (schema=3, +`midi_channel`), `app/gui/persistence.cpp` (load/save, reject !=3)
- Modify: `tests/test_persistence.cpp`

- [ ] **Step 1: Write failing tests**

In `tests/test_persistence.cpp`, in `TEST_CASE("Persistence round-trip")`, after the `s.log_level = "debug";` line add:
```cpp
    s.midi_channel = 4;   // 0-based; -1 = OMNI
```
and after `CHECK(loaded->log_level == "debug");` add:
```cpp
    CHECK(loaded->midi_channel == 4);
```
Then add a new case after the schema-v1 reject test:
```cpp
TEST_CASE("Persistence schema v2 odmitnuta (po bumpu na 3)") {
    using namespace ithaca::gui;
    auto p = std::filesystem::temp_directory_path() / "ithaca_v2_schema.json";
    { std::ofstream f(p); f << "{\"schema_version\":2,\"bank_path\":\"/x\"}\n"; }
    CHECK_FALSE(loadState(p).has_value());
    std::filesystem::remove(p);
}
```

- [ ] **Step 2: Run — verify FAIL (no midi_channel member)**

Run: `cmake --build build --target test_persistence -j 2>&1 | tail -5`
Expected: compile error — `GuiState` has no `midi_channel`.

- [ ] **Step 3: Implement**

In `app/gui/persistence.h`: change `int schema_version = 2;` → `= 3;`; after `std::string log_level = "info";` add:
```cpp
    int         midi_channel      = -1;   // -1 = OMNI, 0..15 = MIDI kanal (0-based)
```
In `app/gui/persistence.cpp` loadState: change `if (s.schema_version != 2)` → `!= 3`; after the `log_level` read block add:
```cpp
        { std::string mc = findValue(json, "midi_channel");
          s.midi_channel = mc.empty() ? -1 : std::stoi(mc); }
```
In saveState, after the `log_level` write line add:
```cpp
        f << "  \"midi_channel\": " << s.midi_channel << ",\n";
```

- [ ] **Step 4: Run — verify PASS + full suite**

Run: `cmake --build build --target test_persistence -j && ctest --test-dir build 2>&1 | tail -3`
Expected: all pass.

- [ ] **Step 5: Commit**
```bash
git add app/gui/persistence.h app/gui/persistence.cpp tests/test_persistence.cpp
git commit -m "feat(gui): persist midi_channel (state schema v3)"
```

---

## PHASE 4 — Layout shell (3-column grid + restyled top bar)

### Task 7: main.cpp 3-column layout + grid ticks + new panel wiring

**Files:**
- Modify: `app/gui/main.cpp` (render loop layout), `CMakeLists.txt` (add new panel sources)
- Create stubs: `app/gui/panel_bank.{h,cpp}`, `app/gui/panel_indicators.{h,cpp}`, `app/gui/panel_dsp.{h,cpp}`

This task replaces the absolute-rect panel calls with a single full-window child + the layout regions from the spec. To keep it incremental, create minimal stubs for the 3 new panels (filled in Phase 5), and call them.

- [ ] **Step 1: Create panel stubs**

Create `app/gui/panel_bank.h`:
```cpp
#pragma once
namespace ithaca::gui { struct AppContext; void renderBankPanel(AppContext& ctx); }
```
Create `app/gui/panel_bank.cpp`:
```cpp
#include "panel_bank.h"
#include "app_context.h"
#include "widgets.h"
#include "imgui.h"
namespace ithaca::gui {
void renderBankPanel(AppContext& ctx) {
    wdg::Eyebrow("BANK");
    (void)ctx; // naplni se v Phase 5
}
} // namespace ithaca::gui
```
Create `app/gui/panel_indicators.h`:
```cpp
#pragma once
namespace ithaca::gui { struct AppContext;
  void renderIndicatorStrip(AppContext& ctx, float col1_w, float col3_w); }
```
Create `app/gui/panel_indicators.cpp`:
```cpp
#include "panel_indicators.h"
#include "app_context.h"
#include "widgets.h"
#include "imgui.h"
namespace ithaca::gui {
void renderIndicatorStrip(AppContext& ctx, float, float) {
    wdg::Eyebrow("MIDI / DIAG / PEAK"); (void)ctx;
}
} // namespace ithaca::gui
```
Create `app/gui/panel_dsp.h`:
```cpp
#pragma once
namespace ithaca::gui { struct AppContext; void renderDspRack(AppContext& ctx); }
```
Create `app/gui/panel_dsp.cpp`:
```cpp
#include "panel_dsp.h"
#include "app_context.h"
#include "widgets.h"
#include "imgui.h"
namespace ithaca::gui {
void renderDspRack(AppContext& ctx) { wdg::Eyebrow("DSP RACK"); (void)ctx; }
} // namespace ithaca::gui
```

- [ ] **Step 2: Add new sources to CMake**

In `CMakeLists.txt`, the `add_executable(ithaca-gui ...)` list — add after `app/gui/panel_diag.cpp`:
```cmake
        app/gui/panel_bank.cpp
        app/gui/panel_indicators.cpp
        app/gui/panel_dsp.cpp
```
(panel_diag.cpp stays for now; removed in Phase 5.)

- [ ] **Step 3: Rewrite main.cpp render section to the 3-column shell**

In `app/gui/main.cpp`, the current render block (lines ~162-181 with `topbar_h`, `renderTopBar`, `renderKeyboardPanel`, `renderDiagPanel`/`renderParamsPanel`, `renderLogPanel`) — replace the panel-call portion with one full-window layout. Replace from `const float W = ...` through `renderLogPanel(...)` with:
```cpp
        const float W = (float)ctx.state.window_w;
        const float H = (float)ctx.state.window_h;
        const float COL1 = 230.f, COL3 = 280.f;
        const float topbar_h = 48.f, strip_h = 78.f, kbd_h = 70.f, log_h = 64.f;

        ImGui::SetNextWindowPos({0,0});
        ImGui::SetNextWindowSize({W,H});
        ImGui::Begin("##root", nullptr,
            ImGuiWindowFlags_NoTitleBar|ImGuiWindowFlags_NoResize|
            ImGuiWindowFlags_NoMove|ImGuiWindowFlags_NoCollapse|
            ImGuiWindowFlags_NoBringToFrontOnFocus|ImGuiWindowFlags_NoScrollbar);

        renderTopBar(ctx);                                  // own row, full width
        ImGui::Dummy({0,2});
        renderIndicatorStrip(ctx, COL1, COL3);              // 3-col strip
        ImGui::Dummy({0,2});

        // MAIN ROW: BANK | VOICE | DSP
        const float main_h = H - topbar_h - strip_h - kbd_h - log_h - 24.f;
        ImGui::BeginChild("##bank", {COL1, main_h}, false);  renderBankPanel(ctx);   ImGui::EndChild();
        ImGui::SameLine(0, 0);
        ImGui::BeginChild("##voice", {W-COL1-COL3, main_h}, false); renderParamsPanel(ctx); ImGui::EndChild();
        ImGui::SameLine(0, 0);
        ImGui::BeginChild("##dsp", {COL3, main_h}, false);   renderDspRack(ctx);     ImGui::EndChild();

        renderKeyboardPanel(ctx);
        renderLogPanel(ctx);

        // grid ticks na krizeni sloupcove drahy (screen coords)
        {
            ImVec2 wp = ImGui::GetWindowPos();
            wdg::GridTick(wp.x + COL1, wp.y + topbar_h);
            wdg::GridTick(wp.x + W - COL3, wp.y + topbar_h);
            wdg::GridTick(wp.x + COL1, wp.y + topbar_h + strip_h);
            wdg::GridTick(wp.x + W - COL3, wp.y + topbar_h + strip_h);
        }
        ImGui::End();
```
Add includes near the top of main.cpp: `#include "panel_bank.h"`, `#include "panel_indicators.h"`, `#include "panel_dsp.h"`, `#include "widgets.h"`.

NOTE: `renderParamsPanel`, `renderKeyboardPanel`, `renderLogPanel`, `renderTopBar` signatures change from the old `(ctx, x, y, w, h)` to `(ctx)` in Phase 5. For THIS task, temporarily adapt the calls to the OLD signatures so it compiles, OR change the four signatures to `(AppContext&)` now and make each panel use `ImGui::GetContentRegionAvail()` instead of x/y/w/h. **Do the latter** — change each panel's signature to `(AppContext& ctx)` and replace its `SetNextWindowPos/Size`+`Begin/End` with direct drawing into the current child (remove the per-panel window). This is the bulk of Phase 5; if too large for one task, split: first make signatures compile with minimal bodies, then restyle.

Because this couples tightly, **implement Task 7 + Task 8 together if needed** (the implementer may treat them as one unit). Keep commits per working build.

- [ ] **Step 4: Build + smoke**

Run: `cmake -S . -B build >/dev/null && cmake --build build --target ithaca-gui -j 2>&1 | tail -5`
Expected: builds. Smoke: `./build/ithaca-gui --bank-dir /Users/j/SoundBanks/Ithaca` → window shows 3 column regions + top bar + keyboard + log (content minimal/stub). Layout proportions match the mockup.

- [ ] **Step 5: Commit**
```bash
git add app/gui/main.cpp app/gui/panel_bank.* app/gui/panel_indicators.* app/gui/panel_dsp.* CMakeLists.txt
git commit -m "feat(gui): 3-column layout shell (BANK|VOICE|DSP) + grid ticks + panel stubs"
```

### Task 8: Restyle top bar (logo + MIDI dropdown + reload + channel; remove bank)

**Files:**
- Modify: `app/gui/panel_topbar.{h,cpp}` (signature → `(AppContext&)`, draw into current window row)

- [ ] **Step 1: Rewrite panel_topbar.cpp**

Change `renderTopBar` signature to `void renderTopBar(AppContext& ctx)` (update `panel_topbar.h` too). Body draws a single row (no own window): logo "ITHACA" in brand font/gold, then MIDI IN combo + reload ⟳ button + CHANNEL combo (OMNI/1-16), then MASTER slider right-aligned. Remove the BANK combo entirely (moves to panel_bank). Use the existing `scanBanks`-free MIDI logic. Concretely:
```cpp
void renderTopBar(AppContext& ctx) {
    // logo
    if (wdg::Fonts::brand) ImGui::PushFont(wdg::Fonts::brand);
    ImGui::PushStyleColor(ImGuiCol_Text, theme::Colors::v(theme::Colors::gold));
    ImGui::TextUnformatted("ITHACA");
    ImGui::PopStyleColor(); if (wdg::Fonts::brand) ImGui::PopFont();
    ImGui::SameLine(0, 24);

    // MIDI IN combo
    auto ports = ithaca::MidiInput::listPorts();
    wdg::Eyebrow("MIDI IN"); ImGui::SameLine();
    ImGui::SetNextItemWidth(150);
    const char* cur = ctx.state.midi_port_name.empty() ? "(none)" : ctx.state.midi_port_name.c_str();
    if (ImGui::BeginCombo("##midi", cur)) {
        for (size_t i=0;i<ports.size();++i){
            bool sel = ports[i]==ctx.state.midi_port_name;
            if (ImGui::Selectable(ports[i].c_str(), sel)) {
                if (ports[i]!=ctx.state.midi_port_name){ ctx.midi.close();
                    if (ctx.midi.open(ctx.engine,(int)i)){ ctx.state.midi_port_name=ports[i];
                        ctx.midi.setChannel(ctx.state.midi_channel); } }
            }
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    if (ImGui::Button("\xE2\x86\xBB##reload")) { /* listPorts() re-scanne kazdy frame; tlacitko jen vizualni refresh hook */ }
    ImGui::SameLine(0,18);

    // CHANNEL combo: OMNI + 1..16
    wdg::Eyebrow("CH"); ImGui::SameLine(); ImGui::SetNextItemWidth(70);
    char chlbl[8]; if (ctx.state.midi_channel<0) std::snprintf(chlbl,8,"OMNI");
                   else std::snprintf(chlbl,8,"%d",ctx.state.midi_channel+1);
    if (ImGui::BeginCombo("##ch", chlbl)) {
        if (ImGui::Selectable("OMNI", ctx.state.midi_channel<0)) { ctx.state.midi_channel=-1; ctx.midi.setChannel(-1); }
        for (int c=0;c<16;++c){ char b[4]; std::snprintf(b,4,"%d",c+1);
            if (ImGui::Selectable(b, ctx.state.midi_channel==c)) { ctx.state.midi_channel=c; ctx.midi.setChannel(c); } }
        ImGui::EndCombo();
    }

    // MASTER right-aligned
    const float right = 240.f;
    ImGui::SameLine(ImGui::GetWindowWidth()-right);
    wdg::Eyebrow("MASTER"); ImGui::SameLine(); ImGui::SetNextItemWidth(150);
    if (ImGui::SliderFloat("##master", &ctx.state.master_gain_db, -60.f, 6.f, "%.1f dB")) {
        ctx.engine.setMasterGain(std::pow(10.f, ctx.state.master_gain_db/20.f));
    }
}
```
Add includes: `theme.h`, `widgets.h`, `<cmath>`, `<cstdio>`. Remove the old bank-combo + scanBanks code (it relocates to panel_bank in Phase 5; keep the `scanBanks` helper by MOVING it to panel_bank.cpp).

Note: the reload ⟳ button is mostly a no-op because `listPorts()` is already called every frame (live). If a cached list is introduced later, the button clears the cache. Document that inline.

Also apply the channel on initial MIDI open in `app_context.cpp::initFromState` — after a successful `midi.open(...)` add `midi.setChannel(state.midi_channel);`.

- [ ] **Step 2: Build + smoke + full suite**

Run: `cmake --build build -j 2>&1 | tail -3 && ctest --test-dir build 2>&1 | tail -3`
Expected: builds, tests pass. Smoke: top bar shows gold ITHACA + MIDI/CH/MASTER; selecting CH changes filter (play notes on another channel → silence at fixed CH, sound at OMNI).

- [ ] **Step 3: Commit**
```bash
git add app/gui/panel_topbar.h app/gui/panel_topbar.cpp app/gui/app_context.cpp
git commit -m "feat(gui): restyle top bar + MIDI reload + channel/OMNI selector"
```

---

## PHASE 5 — Panel content (bank, indicators, params, keyboard, log, dsp)

### Task 9: BANK panel (select + type badge + facts + reload)

**Files:**
- Modify: `app/gui/panel_bank.cpp`; move `scanBanks` helper here from old topbar.

- [ ] **Step 1: Implement renderBankPanel**

Fill `app/gui/panel_bank.cpp` with: section title "BANK"; SELECT combo (scan `ctx.state.bank_search_dir`, on change `ctx.state.bank_path=...; ctx.engine.reloadBank(b)`); TYPE read-only gold badge "LEGACY" (engine exposes only legacy today — hardcode "LEGACY" + "· auto" until bank.type exists; add a `// FUTURE: ctx.engine.bankType()` comment); bank facts line (`8 velocity · N samplů · MB · 88 not · 48 kHz` — pull what's available from engine, hardcode labels that aren't exposed with a FUTURE note); RELOAD button calling `ctx.engine.reloadBank(ctx.state.bank_path)`. Use `wdg::Eyebrow` for labels, theme colors. Move the `scanBanks(search_root)` helper verbatim from the old panel_topbar.cpp into an anonymous namespace here.

- [ ] **Step 2: Build + smoke**

Run: `cmake --build build --target ithaca-gui -j 2>&1 | tail -3`
Expected: builds; BANK column shows dropdown + LEGACY badge + facts + reload; switching bank works (reloadBank is the already-fixed safe path).

- [ ] **Step 3: Commit**
```bash
git add app/gui/panel_bank.cpp
git commit -m "feat(gui): BANK panel (select + type badge + facts + reload)"
```

### Task 10: INDICATOR strip (MIDI lamps + sustain bar + diag tiles + peak bars)

**Files:**
- Modify: `app/gui/panel_indicators.cpp`; delete `app/gui/panel_diag.{h,cpp}` + remove from CMake.

- [ ] **Step 1: Implement renderIndicatorStrip**

Three columns matching the grid (col1 width `col1_w`=230, center flex, col3 `col3_w`=280), via `BeginChild` regions side by side:
- **col1:** `wdg::Lamp("NOTE", anyNoteActive, gold)` + `wdg::Lamp("CC", false, silver)` in a row; then SUSTAIN: eyebrow "SUSTAIN · CC64" + value (pedalCC), `wdg::HBar(cc/127, w, 8, gold, gold, 0.5f)` (tick at half-pedal), legend "UP / ½ HALF / DOWN" via small eyebrow row.
- **center:** three `wdg::StatTile`: VOICES (gold value), RESONANCE (silver value), RINGS (silver, "9/32").
- **col3:** "PEAK" eyebrow + two `wdg::HBar` (L, R) using `dbTo01` mapping (reuse the toDb/dbTo01 helpers from old panel_diag — move them here), silver gradient fills; dB legend row.

`anyNoteActive` = OR over `ctx.engine.activeMidiNotes(mask)`. Pull `pedalCC()`, `masterPeakL/R()`, `activeVoices()`, `resonanceVoices()`, `numRingsUsed()` from engine (all exist).

- [ ] **Step 2: Delete panel_diag**

Remove `app/gui/panel_diag.cpp` + `.h`; remove `app/gui/panel_diag.cpp` from `CMakeLists.txt` ithaca-gui sources; remove `renderDiagPanel` include/call from main.cpp (already replaced by renderIndicatorStrip in Task 7).

Run: `git rm app/gui/panel_diag.cpp app/gui/panel_diag.h`

- [ ] **Step 3: Build + smoke + suite**

Run: `cmake -S . -B build >/dev/null && cmake --build build -j 2>&1 | tail -3 && ctest --test-dir build 2>&1 | tail -3`
Expected: builds, tests pass. Smoke: strip shows lamps, horizontal sustain bar with half-pedal tick, gold VOICES / silver RESONANCE tiles, horizontal L/R peak bars. Play + pedal → bars move.

- [ ] **Step 4: Commit**
```bash
git add app/gui/panel_indicators.cpp CMakeLists.txt app/gui/main.cpp
git rm app/gui/panel_diag.cpp app/gui/panel_diag.h
git commit -m "feat(gui): indicator strip (MIDI lamps, sustain bar, diag tiles, peak bars); drop panel_diag"
```

### Task 11: Restyle VOICE params + keyboard + log + DSP rack placeholder

**Files:**
- Modify: `app/gui/panel_params.cpp`, `app/gui/panel_keyboard.cpp`, `app/gui/panel_log.cpp`, `app/gui/panel_dsp.cpp` (signatures → `(AppContext&)`, draw into current child)

- [ ] **Step 1: panel_params** — change signature to `(AppContext&)`; replace per-window with section title "VOICE" + `wdg::ParamSliderF` for resonance/release/excite-decay (resonance grab gold via `PushStyleColor(ImGuiCol_SliderGrab, gold)` around that one slider), disabled max-resonance, LOG LEVEL combo + RESET button (keep existing logic, restyle). Keep the existing log-level + reset behavior verbatim, just reskinned.

- [ ] **Step 2: panel_keyboard** — change signature to `(AppContext&)`; replace the manual key drawing with `wdg::Keyboard(avail_w, kbd_h, activeFn, gainFn)` where `activeFn=[&](int m){return active[m];}` from `activeMidiNotes`. Add the "A0 — SUSTAIN ½ — C8" caption line below in muted eyebrow.

- [ ] **Step 3: panel_log** — change signature to `(AppContext&)`; draw into current child (no own window); restyle colors to muted/silver, Warning=gold-ish, Error=red; keep snapshot + autoscroll logic.

- [ ] **Step 4: panel_dsp** — fill DSP RACK placeholder: section title "DSP RACK" + 4 static rows (AGC / CONVOLVER / BBE / LIMITER) each = gold LED dot + name + dim value, last (LIMITER) dimmed "off"; bottom "signál ▸ AGC ▸ conv ▸ bbe ▸ lim ▸ out" caption. Pure static placeholder; add `// FUTURE: real DSP chain controls` comment.

- [ ] **Step 5: Build + smoke + suite**

Run: `cmake --build build -j 2>&1 | tail -3 && ctest --test-dir build 2>&1 | tail -3`
Expected: builds, tests pass. Smoke: full window now matches the final mockup — gold ITHACA, silver structure, gold accents on VOICES/NOTE/sustain/resonance-grab/active-keys/DSP-LED/LEGACY, 3 columns, horizontal bars, deco keyboard, DSP rack placeholder.

- [ ] **Step 6: Commit**
```bash
git add app/gui/panel_params.cpp app/gui/panel_keyboard.cpp app/gui/panel_keyboard.h app/gui/panel_log.cpp app/gui/panel_log.h app/gui/panel_params.h app/gui/panel_dsp.cpp
git commit -m "feat(gui): restyle params/keyboard/log + DSP rack placeholder"
```

---

## PHASE 6 — Persist channel on change + final verification

### Task 12: Persist midi_channel on change; final pass

**Files:**
- Modify: `app/gui/main.cpp` (debounce comparator)

- [ ] **Step 1: Add midi_channel to save debounce**

In `app/gui/main.cpp` render-loop `bool changed =` comparator, add a line:
```cpp
            last_saved.midi_channel != ctx.state.midi_channel ||
```
(append to the existing OR chain alongside `log_level`).

- [ ] **Step 2: Build + full suite**

Run: `cmake --build build -j 2>&1 | tail -3 && ctest --test-dir build 2>&1 | tail -3`
Expected: builds, all tests pass.

- [ ] **Step 3: Manual end-to-end smoke**

Run from repo root: `./build/ithaca-gui --bank-dir /Users/j/SoundBanks/Ithaca`
Verify: theme + Cormorant render; pick bank (loads); select MIDI port; set CHANNEL=1 → notes on ch1 sound, other channels silent; OMNI → all sound; pedal moves sustain bar with half-pedal tick; active notes light gold on keyboard; peak bars move; close + reopen → midi_channel + log_level + bank persisted (`cat "$HOME/Library/Application Support/ithaca-legacy/state.json"` shows `schema_version: 3`, `midi_channel`).

- [ ] **Step 4: Confirm no leftovers**

Run: `grep -rn "renderDiagPanel\|StyleColorsDark" app/gui/ && echo "FOUND" || echo clean`
Expected: `clean` (diag panel gone, StyleColorsDark replaced).

- [ ] **Step 5: Commit**
```bash
git add app/gui/main.cpp
git commit -m "feat(gui): persist midi_channel on change"
```

---

## Notes for the implementer

- **Run from repo root** so `./third-party/cormorant/Cormorant-Medium.ttf` resolves (icr2 convention; no install step). If launched elsewhere, the font falls back to default — note in smoke if text looks wrong.
- **GUI is build-verified + manually smoke-tested**, not unit-tested (ImGui context). The ONLY unit-tested logic here is the MIDI channel filter (Task 5) and persistence (Task 6). Don't try to unit-test rendering.
- **Tasks 7+8 are coupled** (layout shell needs panel signatures `(AppContext&)`). If a single task is too big, the implementer may split into "make it compile with stubs" then "restyle", committing at each green build — but keep each commit building.
- **Channel is 0-based internally** (`midi_channel=0` ⇒ MIDI channel 1, shown as "1"; `-1` ⇒ OMNI). Keep this consistent across persistence, MidiInput, and the combo label.
- **TYPE badge is hardcoded "LEGACY"** until the engine exposes `bank.type` (folder-type loader + autodetect is a SEPARATE engine task — see spec "ENGINE DEPENDENCY / FUTURE"). Mark with `// FUTURE` so it's findable.
- **DSP RACK is a static placeholder** — no real controls. Real AGC/Convolver/BBE/Limiter is a separate feature (see [[dsp-reference-repos]] memory + its own future spec).
- **Glow: none.** Active = gold fill / brighter color, never a shadow/bloom.
- **No Hi-DPI** this round (fixed px). On Retina it'll look small; that's accepted (future).
- `AddRectFilledMultiColor` arg order in ImGui 1.91: `(p_min, p_max, col_upr_left, col_upr_right, col_bot_right, col_bot_left)`. For a horizontal gradient pass `(lo, hi, hi, lo)`.
- Do not commit `imgui.ini` (gitignored) or stray build dirs.
- Window default size: bump `GuiState.window_w/h` defaults if the 3-column layout needs more than current 1024×768 — the mockup assumes ~1280×760. Adjust in persistence.h defaults if smoke shows cramping (not a separate task; do it in Task 7 if needed).
