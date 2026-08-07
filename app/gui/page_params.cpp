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

#include <algorithm>
#include <cstdio>

namespace ithaca::gui {

namespace L = ithaca::gui::layout;
using theme::Colors;
using theme::Fonts;

void pageParams(AppContext& ctx, const Rect& r, ithaca::dsp::IParamPage& page) {
    (void)ctx;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float px_s = wdg::fontPx(Fonts::small);

    float top = r.lo.y;

    // -- Hlavicka: ON/OFF + volic v JEDNOM roztazenem radku ------------------
    // Bunky maji stejnou sirku, i kdyz jsou popisky ruzne dlouhe. Volic jako
    // radek poli je na dotyku lepsi nez rozbalovaci seznam (jedno klepnuti
    // misto dvou a vzdy je videt cela nabidka).
    if (page.hasEnable()) {
        const float cell_h  = L::Dims::touch * 0.62f;   // opticky, zona je plna
        const int   n_cells = 1 + page.choiceCount();
        const L::Row row = L::splitRow(ImVec2(r.lo.x, top), r.w(), cell_h, n_cells);

        ImGui::SetCursorScreenPos(row.at(0));
        if (wdg::toggle("##en", page.name(), page.enabled(), row.cell))
            page.setEnabled(!page.enabled());

        const int cur = page.currentChoice();
        for (int i = 0; i < page.choiceCount(); ++i) {
            const ImVec2 p = row.at(i + 1);
            const bool on = (i == cur);
            wdg::cell(dl, p, row.end(i + 1), page.choiceName(i), on);
            ImGui::SetCursorScreenPos(p);
            char id[32]; std::snprintf(id, sizeof(id), "##ch%d", i);
            if (ImGui::InvisibleButton(id, ImVec2(row.cell, L::Dims::touch)) && !on)
                page.selectChoice(i);
        }
        // Dotykova zona je vyssi nez vykreslena bunka, takze se odsazuje od ni.
        top += row.height() - cell_h + L::Dims::touch + L::Dims::gap;
    }

    // -- Volitelny metr (AGC CURRENT GAIN, LIMITER GAIN REDUCTION) ----------
    // Patri k hlavicce, ne k parametrum: je to odectena hodnota, ne ovladani.
    float mv; const char* ml;
    if (page.meter(mv, ml)) {
        char buf[32]; std::snprintf(buf, sizeof(buf), "%.2f", mv);
        dl->AddText(Fonts::small, px_s, ImVec2(r.lo.x, top), Colors::dimmer, ml);
        const float np = wdg::fontPx(Fonts::ui);
        dl->AddText(Fonts::ui, np, ImVec2(r.lo.x, top + px_s + 4.f), Colors::ink, buf);
        top += px_s + np + L::Dims::gap;
    }

    // -- Parametry, sazene ODSPODU ------------------------------------------
    // Slidery drzi spodni hranu plochy, ne horni. Dve veci tim ziskame:
    // opticky se oddeli od zalozek nahore a prst na ne dosahne dal od menu,
    // takze pri mireni do kraje netrefi omylem prepinac stranky.
    const int n = page.paramCount();
    if (n <= 0) return;

    // Kdyz se blok nevejde (CONVOLVER ma ctyri parametry a nad nimi jeste
    // podzalozky), track se stlaci — az na param_trk_min. Radeji o neco nizsi
    // pas nez parametr, ktery na panelu vubec neni videt.
    //
    // Na uzkem panelu ale ani param_h_min nestacilo: na 800x480 zbylo na ctyri
    // parametry CONVOLVERu 136 px, vesly se dva a smycka na tretim delala
    // `break` — dva parametry tise ZMIZELY, bez jakekoli stopy na obrazovce.
    // Proto se pas smi stlacit i pod param_h_min, az na tvrdou podlahu
    // citelnosti; a kdyz se nevejdou ani tak, REKNE se to.
    const float avail = r.hi.y - top;
    float row_h = L::Dims::param_h;
    if ((float)n * row_h > avail)
        row_h = std::max(L::Dims::param_h_min, avail / (float)n);
    if ((float)n * row_h > avail)
        row_h = std::max(L::Dims::param_h_floor, avail / (float)n);
    const float trk_h = row_h - L::Dims::param_gap;

    float y = std::max(top, r.hi.y - (float)n * row_h);

    const bool on = !page.hasEnable() || page.enabled();
    int shown = 0;
    for (int i = 0; i < n; ++i) {
        if (y + row_h > r.hi.y + 1.f) break;           // radeji orez nez pretect
        const auto& p = page.param(i);
        float v = page.get(i);
        ImGui::SetCursorScreenPos(ImVec2(r.lo.x, y));
        ImGui::PushItemWidth(r.w());
        char id[32]; std::snprintf(id, sizeof(id), "##p%d", i);
        // Sirku bere paramSlider z content region; omezime ho childem.
        ImGui::BeginChild(id, ImVec2(r.w(), row_h), false,
                          ImGuiWindowFlags_NoScrollbar);
        if (wdg::paramSlider("##s", p.label, &v, p.min, p.max, p.fmt,
                             on && !p.readonly, trk_h))
            page.set(i, v);
        ImGui::EndChild();
        ImGui::PopItemWidth();
        y += row_h;
        ++shown;
    }

    // Kdyz se neco presto nevejde, musi to byt VIDET. Tiche zahozeni parametru
    // je nejhorsi mozne chovani: uzivatel nema jak poznat, ze stage ma dalsi
    // ovladani, a hleda chybu ve zvuku.
    if (shown < n) {
        char note[48];
        std::snprintf(note, sizeof(note), "+%d MORE - NOT ENOUGH HEIGHT", n - shown);
        const float px = wdg::fontPx(Fonts::small);
        wdg::invField(dl, ImVec2(r.lo.x, top - px * 1.45f - 4.f), Fonts::small, note);
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
