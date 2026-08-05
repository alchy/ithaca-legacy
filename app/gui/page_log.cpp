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

#include <cstdio>

namespace ithaca::gui {

namespace L = ithaca::gui::layout;
using theme::Colors;
using theme::Fonts;

void pageLog(AppContext& ctx, const Rect& r) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    auto& tmp = ctx.panels.log_scratch;
    const int n = ctx.log_buf.snapshot(tmp.data(), (int)tmp.size());

    const float px = wdg::fontPx(Fonts::small);
    const float row = px + 8.f;

    // Kolik radku se vejde; zobrazujeme konec (nejnovejsi dole).
    const int fits = (int)((r.h() - 4.f) / row);
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
