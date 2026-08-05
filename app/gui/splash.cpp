// app/gui/splash.cpp — uvodni obrazovka.
//
// Sled: setreni shora → ITHACA prilétá shora a LEGACY zdola → DOTKNOU SE
// (v ten okamzik obe kratce zesili zar) → rozejdou se a dosednou. JEDNA vlna,
// zadny druhy overshoot: prejeti je odmerene presne na polovinu mezery mezi
// slovy, takze se slova opravdu dotknou a hned se rozejdou na misto.
//
// Zesileni zare v okamziku kontaktu neni ozdoba: na OLED a VFD panelech jas
// realne kolisa se zatezi, takze je to napodobeni fyziky displeje, ze ktereho
// cely vzhled vychazi.
//
// Splash kryje SKUTECNOU praci a konci, az dobehne load — ne po pevnem case.
// Minimum je doba animace, aby se neusekla v pulce; kdyz load trva dyl,
// splash pocka a ukazuje jeho prubeh.
#include "splash.h"
#include "app_context.h"
#include "theme.h"
#include "layout.h"
#include "widgets.h"
#include "midi/midi_input.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>

namespace ithaca::gui {

namespace L = ithaca::gui::layout;
using theme::Colors;
using theme::Fonts;

namespace {

// Casovani (s). Animace je kratka zamerne — pri kazdem startu ji uvidis znovu.
constexpr float kWipeEnd   = 0.28f;   // setreni shora
constexpr float kFlyStart  = 0.24f;
constexpr float kTouch     = 0.72f;   // okamzik dotyku
constexpr float kSettled   = 1.05f;   // dosednuti
constexpr float kSubIn     = 1.15f;   // podtitul
constexpr float kMinShow   = 1.70f;   // minimum, aby se animace neusekla
constexpr float kFadeOut   = 0.40f;

// Mezera mezi slovy v klidu. Prejeti je presne jeji polovina na kazde slovo,
// takze v okamziku dotyku je mezera nulova.
constexpr float kGap = 20.f;

float smoothstep(float a, float b, float x) {
    if (b <= a) return 1.f;
    const float t = std::clamp((x - a) / (b - a), 0.f, 1.f);
    return t * t * (3.f - 2.f * t);
}

// Posun slova: prilétá z dálky, prejede o kGap/2 a vrati se na 0.
// dir = +1 pro slovo prichazejici shora, -1 pro zdola.
float wordOffset(float t, float dir) {
    if (t <= kFlyStart) return dir * -520.f;
    if (t <= kTouch) {
        // Prilet az do dotyku. Zpomaluje (smoothstep), takze dosedne mekce.
        const float s = smoothstep(kFlyStart, kTouch, t);
        return dir * (-520.f + s * (520.f + kGap * 0.5f));
    }
    // Rozchod z dotyku na klidovou polohu — jedina korekce, zadny dalsi kmit.
    const float s = smoothstep(kTouch, kSettled, t);
    return dir * (kGap * 0.5f) * (1.f - s);
}

void stepLine(ImDrawList* dl, ImVec2 pos, const char* label, const char* value,
              float alpha, ImU32 val_col) {
    const float px = wdg::fontPx(Fonts::small);
    const auto A = [&](ImU32 c) {
        return (c & 0x00FFFFFF) | ((ImU32)(((c >> 24) & 0xFF) * alpha) << 24);
    };
    dl->AddText(Fonts::small, px, pos, A(Colors::dimmer), label);
    dl->AddText(Fonts::small, px,
                ImVec2(pos.x + 300.f, pos.y), A(val_col), value);
}

} // namespace

bool renderSplash(AppContext& ctx, float W, float H) {
    auto& sp = ctx.panels;
    const float dt = ImGui::GetIO().DeltaTime;

    if (sp.splash_done) return false;
    sp.splash_t += std::min(dt, 0.05f);
    const float t = sp.splash_t;

    // Konci, az dobehne skutecny load — ne po pevnem case. Minimum je doba
    // animace, aby se neusekla v pulce.
    const bool busy = ctx.reloadInProgress();
    const bool want_end = (t > kMinShow) && !busy;
    if (want_end) sp.splash_fade += dt / kFadeOut;
    const float alpha = std::clamp(1.f - sp.splash_fade, 0.f, 1.f);
    if (alpha <= 0.f) { sp.splash_done = true; return false; }

    ImGui::SetNextWindowPos({0, 0});
    ImGui::SetNextWindowSize({W, H});
    ImGui::SetNextWindowFocus();
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0, 0, 0, 0));
    ImGui::Begin("##splash", nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoInputs);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const auto A = [&](ImU32 c, float m = 1.f) {
        return (c & 0x00FFFFFF)
             | ((ImU32)(((c >> 24) & 0xFF) * alpha * std::clamp(m, 0.f, 1.f)) << 24);
    };

    const float b = L::Dims::bezel;
    dl->AddRectFilled({0, 0}, {W, H}, A(Colors::bezel));
    const ImVec2 lo(b, b), hi(W - b, H - b);
    dl->AddRectFilledMultiColor(lo, hi,
        A(Colors::lcd), A(IM_COL32(0x18, 0x46, 0xa8, 255)),
        A(IM_COL32(0x0c, 0x27, 0x66, 255)), A(IM_COL32(0x08, 0x1c, 0x4c, 255)));

    // Setreni shora: displej se na okamzik cely rozsviti a svetlo se stahne
    // nahoru. Presne to delaly LCD panely pri zapnuti jako test segmentu.
    if (t < kWipeEnd) {
        const float k = 1.f - smoothstep(0.f, kWipeEnd, t);
        dl->AddRectFilled(lo, ImVec2(hi.x, lo.y + (hi.y - lo.y) * k),
                          A(Colors::inv_bg));
    }

    // -- Wordmark ----------------------------------------------------------
    const float cx = (lo.x + hi.x) * 0.5f;
    const float cy = (lo.y + hi.y) * 0.42f;
    const float bp = wdg::fontPx(Fonts::brand);

    const float w1 = wdg::textW(Fonts::brand,  bp, "ITHACA");
    const float w2 = wdg::textW(Fonts::brandl, bp, "LEGACY");

    // Zar v okamziku dotyku: kratky vrchol kolem kTouch.
    const float flash = std::exp(-std::pow((t - kTouch) / 0.13f, 2.f));

    const float y1 = cy - bp - kGap * 0.5f + wordOffset(t, +1.f);
    const float y2 = cy + kGap * 0.5f      + wordOffset(t, -1.f);
    const float appear = smoothstep(kFlyStart, kFlyStart + 0.12f, t);

    // Zar = tyz text nekolikrat s malym rozostrenim pod ostrym.
    auto word = [&](ImFont* f, const char* s, float x, float y, ImU32 col) {
        if (flash > 0.02f) {
            const float g = flash * 0.5f;
            for (int i = 0; i < 4; ++i) {
                const float o = 1.5f + i * 1.6f;
                dl->AddText(f, bp, ImVec2(x - o, y), A(Colors::ink, g * 0.20f), s);
                dl->AddText(f, bp, ImVec2(x + o, y), A(Colors::ink, g * 0.20f), s);
            }
        }
        dl->AddText(f, bp, ImVec2(x, y),
                    A(Colors::lerp(col, IM_COL32(0xff, 0xff, 0xff, 255), flash * 0.85f),
                      appear), s);
    };
    word(Fonts::brand,  "ITHACA", cx - w1 * 0.5f, y1, Colors::ink);
    word(Fonts::brandl, "LEGACY", cx - w2 * 0.5f, y2, Colors::inv_bg);

    // Podtitul — az po dosednuti, aby nesoutezil s pohybem.
    {
        const float sa = smoothstep(kSubIn, kSubIn + 0.4f, t);
        const float px = wdg::fontPx(Fonts::small);
        const char* sub = "DIGITAL INSTRUMENT";
        dl->AddText(Fonts::small, px,
                    ImVec2(cx - wdg::textW(Fonts::small, px, sub) * 0.5f,
                           y2 + bp + 18.f),
                    A(Colors::dim, sa * 0.9f), sub);
    }

    // -- Skutecne kroky initu ---------------------------------------------
    // Splash slouzi zaroven jako diagnostika: kdyz se audio device neotevre
    // nebo neni MIDI port, je to videt hned na miste, kde jinak sviti OK.
    const float sa = smoothstep(kSubIn, kSubIn + 0.35f, t);
    if (sa > 0.01f) {
        const float px = wdg::fontPx(Fonts::small);
        float y = hi.y - 150.f;
        const float x = cx - 150.f;
        char buf[96];

        stepLine(dl, ImVec2(x, y), "ENGINE", "OK", sa, Colors::ink);
        y += px + 8.f;

        std::snprintf(buf, sizeof(buf), "%d / %d",
                      ctx.engine.sampleRate(), ctx.engine.blockSize());
        stepLine(dl, ImVec2(x, y), "AUDIO", ctx.audioOk() ? buf : "UNAVAILABLE",
                 sa, ctx.audioOk() ? Colors::ink : Colors::dim);
        y += px + 8.f;

        stepLine(dl, ImVec2(x, y), "MIDI",
                 ctx.midi.isOpen() ? ctx.state.midi_port_name.c_str() : "-",
                 sa, ctx.midi.isOpen() ? Colors::ink : Colors::dim);
        y += px + 8.f;

        const std::string bank =
            std::filesystem::path(ctx.state.bank_path).filename().string();
        stepLine(dl, ImVec2(x, y), "BANK",
                 bank.empty() ? "-" : bank.c_str(), sa, Colors::ink);
        y += px + 14.f;

        // Progres jako retez bloku, stejne jako v modalnim overlayi.
        const auto& p = ctx.loadProgress();
        const float frac = busy
            ? ithaca::bankLoadFraction(p.phase.load(std::memory_order_relaxed),
                                       p.done.load(std::memory_order_relaxed),
                                       p.total.load(std::memory_order_relaxed))
            : 1.f;
        constexpr int kSeg = 30;
        const float bw = 300.f, seg = bw / (float)kSeg;
        const int lit = (int)(frac * kSeg + 0.5f);
        for (int i = 0; i < kSeg; ++i) {
            const ImVec2 a(x + i * seg + 1.f, y);
            const ImVec2 q(x + (i + 1) * seg - 1.f, y + 12.f);
            if (i < lit) dl->AddRectFilled(a, q, A(Colors::inv_bg, sa));
            else         dl->AddRect(a, q, A(Colors::line, sa));
        }
    }

    ImGui::End();
    ImGui::PopStyleColor();
    return true;
}

} // namespace ithaca::gui
