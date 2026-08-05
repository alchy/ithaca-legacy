// app/gui/page_log.cpp — LOG: vypis udalosti ve stejnem pojeti jako zbytek.
//
// Severita se nerozlisuje barvou, ale JASEM a inverzi — presne jako na znakovem
// displeji, ktery vic nez jednu barvu nemel:
//   bezne    tlumene
//   varovani plny jas + jantarova
//   chyba    inverzni blok
#include "pages.h"
#include "app_context.h"
#include "widgets.h"
#include "theme.h"
#include "layout.h"
#include "util/log.h"

#include <algorithm>
#include <cstdio>

namespace ithaca::gui {

namespace L = ithaca::gui::layout;
using theme::Colors;
using theme::Fonts;

float logLevelRow(AppContext& ctx, ImVec2 pos, float w, float cell_h, float hit_h) {
    static const char* kLv[] = { "debug","info","warn","error","fatal","off" };
    int cur = 1;
    for (int i = 0; i < IM_ARRAYSIZE(kLv); ++i)
        if (ctx.state.log_level == kLv[i]) { cur = i; break; }

    const int hit = wdg::chipRow("##lvl", pos, w, kLv, IM_ARRAYSIZE(kLv), cur,
                                 cell_h, hit_h);
    if (hit >= 0) {
        ctx.state.log_level = kLv[hit];
        log::Logger::default_().setMinSeverity(
            log::severity_from_string(ctx.state.log_level.c_str(), log::Severity::Info));
    }
    return wdg::chipRowHeight(w, IM_ARRAYSIZE(kLv), cell_h);
}

void pageLog(AppContext& ctx, const Rect& r) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    auto& tmp = ctx.panels.log_scratch;
    const int n = ctx.log_buf.snapshot(tmp.data(), (int)tmp.size());

    const float px = wdg::fontPx(Fonts::small);
    const float row = px + 8.f;

    // Volba urovne u SPODNI hrany, vypis nad ni: nejnovejsi radky tak lezi
    // hned nad ovladanim, kterym se s nimi hybe.
    const float cell_h = 48.f;
    const float sel_y  = r.hi.y - L::Dims::touch;
    logLevelRow(ctx, ImVec2(r.lo.x, sel_y + (L::Dims::touch - cell_h) * 0.5f),
                r.w(), cell_h, L::Dims::touch);

    const float list_h = sel_y - L::Dims::gap - r.lo.y;

    // Kolik radku se vejde; zobrazujeme konec (nejnovejsi dole).
    const int fits = (int)((list_h - 4.f) / row);
    const int from = (n > fits) ? n - fits : 0;

    float y = r.lo.y;
    for (int i = from; i < n; ++i, y += row) {
        const auto& e = tmp[(size_t)i];
        char line[256];
        std::snprintf(line, sizeof(line), "[%s] %s", e.topic.c_str(), e.message.c_str());

        if ((int)e.sev >= (int)log::Severity::Error) {
            // Chyba = inverzni blok pres celou sirku. Nejde prehlednout.
            const float w = wdg::textW(Fonts::small, px, line) + 12.f;
            dl->AddRectFilled(ImVec2(r.lo.x, y - 1.f),
                              ImVec2(std::min(r.lo.x + w, r.hi.x), y + row - 3.f),
                              Colors::error);
            dl->AddText(Fonts::small, px, ImVec2(r.lo.x + 6.f, y), Colors::lcd, line);
        } else {
            const ImU32 c = (e.sev == log::Severity::Warning) ? Colors::warn : Colors::dimmer;
            dl->AddText(Fonts::small, px, ImVec2(r.lo.x, y), c, line);
        }
    }

    if (n == 0)
        dl->AddText(Fonts::small, px, r.lo, Colors::dimmer, "no events");
}

} // namespace ithaca::gui
