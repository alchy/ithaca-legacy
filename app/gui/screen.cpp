// app/gui/screen.cpp — shell znakoveho displeje.
//
// Sklada se odshora dolu:
//   ramecek (mrtva zona)  →  radek zalozek  →  stranka  →  kontrolky  →  paticka
//
// Ramecek neni ozdoba: je to odsazeni od kraje panelu, aby se u okraje nedalo
// omylem trefit ovladani. Radek zalozek je nahore, protoze zvyraznena zalozka
// slouzi zaroven jako nadpis stranky — tim odpada hlavicka a na stranku se
// vejde o jeden parametr vic.
#include "pages.h"
#include "app_context.h"
#include "theme.h"
#include "layout.h"
#include "widgets.h"
#include "dsp/dsp_stage.h"

#include <cstdio>

namespace ithaca::gui {

namespace L = ithaca::gui::layout;
using theme::Colors;
using theme::Fonts;

namespace {

const char* const kTabs[PAGE_COUNT] = {
    "PLAY", "BANK", "TONE", "RESO", "DSP", "SYS", "LOG"
};

// Radek kontrolek. Zhasle jsou taky videt — aby bylo poznat, ze existuji.
// Zustava na KAZDE strance: co je bezpecnostne dulezite, nesmi zmizet jen
// proto, ze uzivatel zrovna neco ladi.
void lampRow(AppContext& ctx, ImDrawList* dl, ImVec2 pos, float w) {
    const bool ur = ctx.engine.mainStreamUnderrunRecent(4000.f) ||
                    ctx.engine.resonanceStreamUnderrunRecent(4000.f);
    const bool clip = ctx.engine.masterPeakL() >= 0.999f ||
                      ctx.engine.masterPeakR() >= 0.999f;
    float x = pos.x;
    wdg::lamp(dl, ImVec2(x, pos.y), "UNDERRUN", ur, Colors::error);
    x += wdg::lampW("UNDERRUN");
    wdg::lamp(dl, ImVec2(x, pos.y), "CLIP", clip, Colors::error);
    x += wdg::lampW("CLIP");
    wdg::lamp(dl, ImVec2(x, pos.y), "LOG", ctx.panels.log_unseen, Colors::warn);
    (void)w;
}

// Paticka: stitek nastroje vlevo, audio rezim vpravo. Stitek je tu proto,
// ze tohle JE celni panel nastroje — ne aplikace, ktera se jmenuje v titulku okna.
void footer(AppContext& ctx, ImDrawList* dl, ImVec2 pos, float w) {
    const float px = wdg::fontPx(Fonts::small);
    dl->AddText(Fonts::small, px, pos, Colors::dimmer, "ITHACA LEGACY");

    char buf[48];
    const int sr = ctx.engine.sampleRate();
    std::snprintf(buf, sizeof(buf), "%g kHz \xC2\xB7 %d",
                  (double)(sr > 0 ? sr : 48000) / 1000.0, ctx.engine.blockSize());
    dl->AddText(Fonts::small, px,
                ImVec2(pos.x + w - wdg::textW(Fonts::small, px, buf), pos.y),
                Colors::dimmer, buf);
}

} // namespace

void renderScreen(AppContext& ctx, ithaca::dsp::IParamPage** pages, int n_pages) {
    const float W = (float)ctx.state.window.w;
    const float H = (float)ctx.state.window.h;

    ImGui::SetNextWindowPos({0, 0});
    ImGui::SetNextWindowSize({W, H});
    ImGui::PushStyleColor(ImGuiCol_WindowBg, Colors::v(Colors::bezel));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::Begin("##panel", nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollbar);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float b = L::Dims::bezel;

    // Plocha displeje uvnitr ramecku.
    const ImVec2 lcd_lo(b, b), lcd_hi(W - b, H - b);
    dl->AddRectFilled(lcd_lo, lcd_hi, Colors::lcd);

    // Vnitrni odsazeni obsahu od hrany displeje.
    const float pad = 12.f;
    const float cx = lcd_lo.x + pad;
    const float cw = (lcd_hi.x - lcd_lo.x) - pad * 2.f;

    // -- Zalozky --
    ImGui::SetCursorScreenPos(ImVec2(cx, lcd_lo.y + pad));
    ImGui::PushID("tabs");
    if (ctx.panels.page < 0 || ctx.panels.page >= PAGE_COUNT) ctx.panels.page = PAGE_PLAY;
    int page = ctx.panels.page;
    // Sirku pro tabBar bere z content region — omezime ji na plochu displeje.
    ImGui::PushItemWidth(cw);
    {
        // tabBar cte GetContentRegionAvail().x; docasne zuzime pres child.
        ImGui::BeginChild("##tabhost", ImVec2(cw, L::Dims::tab_h), false,
                          ImGuiWindowFlags_NoScrollbar);
        wdg::tabBar("tab", kTabs, PAGE_COUNT, page);
        ImGui::EndChild();
    }
    ImGui::PopItemWidth();
    ImGui::PopID();
    if (page != ctx.panels.page) {
        ctx.panels.page = page;
        ctx.state.config_page = page;      // prezije restart
        if (page == PAGE_LOG) ctx.panels.log_unseen = false;   // videl jsi to
    }

    // -- Rozvrzeni zbytku --
    const float top    = lcd_lo.y + pad + L::Dims::tab_h + L::Dims::gap;
    const float lamp_y = lcd_hi.y - pad - L::Dims::foot_h - L::Dims::lamp_h;
    const float foot_y = lcd_hi.y - pad - L::Dims::foot_h;
    const Rect body{ ImVec2(cx, top), ImVec2(cx + cw, lamp_y - L::Dims::gap) };

    ensureBankList(ctx);

    switch (ctx.panels.page) {
        case PAGE_PLAY: pagePlay(ctx, body); break;
        case PAGE_BANK: pageBank(ctx, body); break;
        case PAGE_TONE: if (n_pages > 0) pageParams(ctx, body, *pages[0]); break;
        case PAGE_RESO: if (n_pages > 1) pageParams(ctx, body, *pages[1]); break;
        case PAGE_DSP:  if (n_pages > 2) pageDsp(ctx, body, pages + 2, n_pages - 2); break;
        case PAGE_SYS:  pageSys(ctx, body); break;
        case PAGE_LOG:  pageLog(ctx, body); break;
        default: break;
    }

    lampRow(ctx, dl, ImVec2(cx, lamp_y), cw);
    footer(ctx, dl, ImVec2(cx, foot_y), cw);

    ImGui::End();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

} // namespace ithaca::gui
