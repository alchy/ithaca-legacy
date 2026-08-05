// app/gui/page_params.cpp — genericky renderer parametru + stranka DSP.
//
// pageParams nezna zadny konkretni parametr: jede pres IParamPage::paramCount()
// a Param tabulky, takze novy parametr ve stage se objevi sam a nikde jinde se
// nic menit nemusi. Slouzi strankam TONE a RESO i vsem ctyrem DSP stage.
#include "pages.h"
#include "app_context.h"
#include "widgets.h"
#include "theme.h"
#include "layout.h"
#include "dsp/dsp_stage.h"

#include <cstdio>

namespace ithaca::gui {

namespace L = ithaca::gui::layout;
using theme::Colors;
using theme::Fonts;

void pageParams(AppContext& ctx, const Rect& r, ithaca::dsp::IParamPage& page) {
    (void)ctx;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float px_s = wdg::fontPx(Fonts::small);

    float y = r.lo.y;

    // ON/OFF + volic (napr. IR u Convolveru) na jednom radku nahore.
    if (page.hasEnable()) {
        ImGui::SetCursorScreenPos(ImVec2(r.lo.x, y));
        if (wdg::toggle("##en", page.name(), page.enabled()))
            page.setEnabled(!page.enabled());

        if (page.choiceCount() > 0) {
            // Volic jako radek inverznich poli — na dotyku lepsi nez rozbalovaci
            // seznam, ktery by se musel trefovat dvakrat.
            const int cur = page.currentChoice();
            float x = r.lo.x + 260.f;
            for (int i = 0; i < page.choiceCount(); ++i) {
                const char* nm = page.choiceName(i);
                const float w = wdg::textW(Fonts::small, px_s, nm) + 24.f;
                if (x + w > r.hi.x) break;
                const bool on = (i == cur);
                if (on) wdg::invField(dl, ImVec2(x, y + 8.f), Fonts::small, nm, 12.f, 40.f);
                else {
                    dl->AddRect(ImVec2(x, y + 8.f), ImVec2(x + w, y + 48.f), Colors::line);
                    dl->AddText(Fonts::small, px_s,
                                ImVec2(x + 12.f, y + 8.f + (40.f - px_s) * 0.5f),
                                Colors::dim, nm);
                }
                ImGui::SetCursorScreenPos(ImVec2(x, y));
                char id[32]; std::snprintf(id, sizeof(id), "##ch%d", i);
                if (ImGui::InvisibleButton(id, ImVec2(w, L::Dims::touch)) && !on)
                    page.selectChoice(i);
                x += w + 6.f;
            }
        }
        y += L::Dims::touch + L::Dims::gap;
    }

    // Parametry.
    const bool on = !page.hasEnable() || page.enabled();
    for (int i = 0; i < page.paramCount(); ++i) {
        if (y + L::Dims::param_h > r.hi.y) break;      // radeji orez nez pretect
        const auto& p = page.param(i);
        float v = page.get(i);
        ImGui::SetCursorScreenPos(ImVec2(r.lo.x, y));
        ImGui::PushItemWidth(r.w());
        char id[32]; std::snprintf(id, sizeof(id), "##p%d", i);
        // Sirku bere paramSlider z content region; omezime ho childem.
        ImGui::BeginChild(id, ImVec2(r.w(), L::Dims::param_h), false,
                          ImGuiWindowFlags_NoScrollbar);
        if (wdg::paramSlider("##s", p.label, &v, p.min, p.max, p.fmt, on && !p.readonly))
            page.set(i, v);
        ImGui::EndChild();
        ImGui::PopItemWidth();
        y += L::Dims::param_h;
    }

    // Volitelny metr (AGC CURRENT GAIN, LIMITER GAIN REDUCTION).
    float mv; const char* ml;
    if (page.meter(mv, ml) && y + 40.f <= r.hi.y) {
        char buf[32]; std::snprintf(buf, sizeof(buf), "%.2f", mv);
        dl->AddText(Fonts::small, px_s, ImVec2(r.lo.x, y), Colors::dimmer, ml);
        const float np = wdg::fontPx(Fonts::ui);
        dl->AddText(Fonts::ui, np, ImVec2(r.lo.x, y + px_s + 4.f), Colors::ink, buf);
    }
}

void pageDsp(AppContext& ctx, const Rect& r, ithaca::dsp::IParamPage** stages, int n) {
    if (n <= 0) return;
    auto& ps = ctx.panels;
    if (ps.dsp_stage < 0 || ps.dsp_stage >= n) ps.dsp_stage = 0;

    // Druhy radek zalozek. Nizsi nez horni (56 px), ale porad nad prstem —
    // je to podnavigace, ne hlavni, takze smi byt opticky mene vyrazna.
    const char* labels[8];
    for (int i = 0; i < n && i < 8; ++i) labels[i] = stages[i]->name();

    ImGui::SetCursorScreenPos(r.lo);
    ImGui::BeginChild("##dsptabs", ImVec2(r.w(), L::Dims::subtab_h), false,
                      ImGuiWindowFlags_NoScrollbar);
    wdg::tabBar("dsptab", labels, n, ps.dsp_stage, L::Dims::subtab_h);
    ImGui::EndChild();

    const Rect inner{ ImVec2(r.lo.x, r.lo.y + L::Dims::subtab_h + L::Dims::gap), r.hi };
    pageParams(ctx, inner, *stages[ps.dsp_stage]);
}

} // namespace ithaca::gui
