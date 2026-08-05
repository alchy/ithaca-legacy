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

#include <algorithm>
#include <cmath>
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

// Pozadi ve stylu PS3 XMB: gradient do svetla vpravo nahore + nekolik mekce
// se vlnicich stuh.
//
// KLICOVE: tvar NENI prubeh zvuku. Kreslit vzorky primo bylo pri pomalem tempu
// prilis neklidne — pozadi ma indikovat, ze zvuk hraje, ne aby se z nej dal
// cist tvar vlny. Tvar je proto parametricky (soucet tri pomalych sinusovek
// s driftujici fazi) a zvuk mu jen MODULUJE amplitudu pres pomalou obalku.
// Vysledek pri hre dycha, v tichu se sotva znatelne vlni.
// `vis` skaluje sytost vlny. PLAY je ambientni obrazovka, tam je vlna hvezda;
// ostatni jsou pracovni, tam ustoupi, aby neprochazela textem. Vypnout ji ale
// nelze — indikace, ze zvuk hraje, ma platit vsude.
void background(AppContext& ctx, ImDrawList* dl, ImVec2 lo, ImVec2 hi,
                ImVec2 wave_lo, ImVec2 wave_hi, float vis) {
    const float w = hi.x - lo.x;

    // Gradient: vpravo nahore svetlejsi, vlevo dole tmavsi.
    dl->AddRectFilledMultiColor(lo, hi,
        Colors::lcd,
        IM_COL32(0x18, 0x46, 0xa8, 255),
        IM_COL32(0x0c, 0x27, 0x66, 255),
        IM_COL32(0x08, 0x1c, 0x4c, 255));

    // -- Obalka ze zvuku ---------------------------------------------------
    auto& wv = ctx.panels.wave;
    constexpr int kN = ithaca::Engine::kScopeSize;
    static float raw_l[kN], raw_r[kN];
    ctx.engine.scopeSnapshot(raw_l, raw_r, kN);

    float sl = 0.f, sr = 0.f;
    for (int i = 0; i < kN; ++i) { sl += raw_l[i] * raw_l[i]; sr += raw_r[i] * raw_r[i]; }
    const float rms_l = std::sqrt(sl / (float)kN);
    const float rms_r = std::sqrt(sr / (float)kN);

    // Auto-rozsah, aby vlna vypadala stejne pri tichem i hlasitem hrani —
    // pozadi neni meridlo. Spodni mez drzi ticho klidne.
    wv.norm_rms = std::max(std::max(rms_l, rms_r), wv.norm_rms * 0.995f);
    const float ref = std::max(wv.norm_rms, 0.005f);

    // Pomaly follower: svizne nahoru (aby nastup noty byl videt), liny dolu
    // (aby dozniv plynul misto skoku). Tohle je to, co dela klid.
    auto follow = [](float& env, float target) {
        const float k = (target > env) ? 0.10f : 0.020f;
        env += (target - env) * k;
    };
    follow(wv.env_l, std::clamp(rms_l / ref, 0.f, 1.f));
    follow(wv.env_r, std::clamp(rms_r / ref, 0.f, 1.f));

    // Viditelnost: rychle nabehne, ale vyhasina pomalu (~4 s), aby vizualizace
    // po dohrani jeste chvili dozila misto aby zmizela rezem. Prah je
    // ABSOLUTNI — kdyby se poustel z normalizovane urovne, auto-rozsah by
    // v tichu zesilil sum a vlna by nezhasla nikdy.
    const float lvl = std::max(rms_l, rms_r);
    const float vis_target = (lvl > 2.0e-4f) ? 1.f : 0.f;
    wv.vis += (vis_target - wv.vis) * (vis_target > wv.vis ? 0.14f : 0.006f);

    // Blizkost prehlceni: od -9 dB (0) k 0 dB (1). Bere skutecny peak metr,
    // ktery ma vlastni decay, takze kratka spicka zustane chvili videt.
    const float pk = std::max(ctx.engine.masterPeakL(), ctx.engine.masterPeakR());
    const float pk_db = (pk > 1e-6f) ? 20.f * std::log10(pk) : -120.f;
    wv.clip = std::clamp((pk_db + 9.f) / 9.f, 0.f, 1.f);

    // Nova hodnota do historie: HLASITOST bloku (0..1), ne znamenkova spicka.
    auto absPeak = [](const float* v, int n) {
        float m = 0.f;
        for (int i = 0; i < n; ++i) m = std::max(m, std::fabs(v[i]));
        return m;
    };
    const float pk_l = absPeak(raw_l, kN);
    const float pk_r = absPeak(raw_r, kN);
    wv.norm_peak = std::max(std::max(pk_l, pk_r), wv.norm_peak * 0.995f);
    const float pref = std::max(wv.norm_peak, 0.01f);

    const int prev = wv.head;
    wv.head = (wv.head + 1) % PanelState::Wave::kHist;
    // Casove vyhlazeni pri vstupu: bez nej by kazdy frame skocil jinam a podel
    // vlny by vznikaly schody.
    wv.hist_l[wv.head] = 0.55f * std::clamp(pk_l / pref, 0.f, 1.f)
                       + 0.45f * wv.hist_l[prev];
    wv.hist_r[wv.head] = 0.55f * std::clamp(pk_r / pref, 0.f, 1.f)
                       + 0.45f * wv.hist_r[prev];

    // -- Tvar --------------------------------------------------------------
    const float wh = wave_hi.y - wave_lo.y;
    const float axis = (ctx.panels.scope_center_y > 0.f)
                     ? ctx.panels.scope_center_y : (wave_lo.y + wh * 0.5f);
    const float room = std::max(24.f, std::min(axis - wave_lo.y, wave_hi.y - axis));
    const float t = (float)ImGui::GetTime();

    constexpr int kPts = 128;
    static float pts[kPts];

    // Ctyri stuhy ve dvou rodinach. KAZDY KANAL JE JINAK PROSVICEN — levy
    // svetly, pravy hlubsi modry — takze je od sebe poznas, i kdyz se prolinaji
    // kolem teze osy. Je to ambientni vizualizer: ma dychat, ne informovat.
    struct Ribbon { float phase, speed, scale, alpha, th; ImU32 col; bool right; };
    const Ribbon ribs[] = {
        { 0.0f, 0.16f, 1.00f, 0.30f, 2.6f, Colors::ink,    false },
        { 2.3f, 0.11f, 0.72f, 0.15f, 1.8f, Colors::inv_bg, false },
        { 1.1f, 0.13f, 0.88f, 0.26f, 2.6f, Colors::fill,   true  },
        { 3.7f, 0.09f, 0.58f, 0.13f, 1.6f, Colors::line,   true  },
    };

    // Zar: tyz tvar trikrat pres sebe — siroky a slaby vespod, uzky a jasny
    // nahore. Levny bloom, ktery z care udela svetlo.
    auto glow = [&](const float* p, int n, float amp, ImU32 col, float a, float th) {
        wdg::waveLine(dl, wave_lo.x, w, axis, p, n, amp, 1.f, col, a * 0.16f, th * 3.4f);
        wdg::waveLine(dl, wave_lo.x, w, axis, p, n, amp, 1.f, col, a * 0.36f, th * 1.9f);
        wdg::waveLine(dl, wave_lo.x, w, axis, p, n, amp, 1.f, col, a,         th);
    };

    dl->PushClipRect(wave_lo, wave_hi, true);
    for (const Ribbon& R : ribs) {
        const float env = R.right ? wv.env_r : wv.env_l;
        // Amplituda i sytost jdou s viditelnosti — vlna se pri utichnuti
        // zaroven splaskne a vytrati, misto aby zustala viset jako prazdny vzor.
        const float amp = room * R.scale * (0.14f + 0.86f * env) * wv.vis;
        // Od -9 dB se stuha zacne barvit do cervena, v 0 dB je cervena.
        const ImU32 col = Colors::lerp(
            Colors::lerp(R.col, Colors::clip_lo, std::min(wv.clip * 2.f, 1.f)),
            Colors::clip_hi, std::max(0.f, wv.clip * 2.f - 1.f));
        const float* hist = R.right ? wv.hist_r : wv.hist_l;

        for (int i = 0; i < kPts; ++i) {
            const float u = (float)i / (float)(kPts - 1);
            const float a = u * 6.2831853f;
            // Nosna vlna. Vsechny slozky maji ZAPORNE znamenko u casu, takze
            // vzor putuje doprava — kdyby se znamenka michala, jen by se
            // treplo na miste. Ruzne rychlosti delaji mekky rozpad tvaru.
            // Koeficienty davaji v souctu 1.0, takze nosna sama nikdy nepresahne
            // rozsah a tanh nize pracuje v linearni oblasti misto v nasyceni.
            float v = 0.55f * std::sin(a * 1.0f - t * R.speed)
                    + 0.30f * std::sin(a * 1.9f - t * R.speed * 1.4f + R.phase)
                    + 0.15f * std::sin(a * 3.1f - t * R.speed * 2.1f + R.phase * 0.6f);

            // Zvuk MODULUJE AMPLITUDU nosne vlny, neprictava se k vychylce —
            // proto zustava tvar hladky. Historie plyne doprava: nejnovejsi
            // hodnota je vlevo, starsi se odsouvaji.
            const int hn = PanelState::Wave::kHist;
            const int c = (int)(u * (hn - 1));
            float hsum = 0.f; int hcnt = 0;
            for (int d = -3; d <= 3; ++d) {          // prostorove vyhlazeni
                const int k = (wv.head - (c + d) + hn * 3) % hn;
                hsum += hist[k]; ++hcnt;
            }
            const float hv = hsum / (float)hcnt;
            pts[i] = v * (0.28f + 0.72f * hv);
        }
        glow(pts, kPts, amp, col, R.alpha * vis * wv.vis, R.th);
    }
    dl->PopClipRect();
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

    const ImVec2 lcd_lo(b, b), lcd_hi(W - b, H - b);

    // Vnitrni odsazeni obsahu od hrany displeje.
    const float pad = 12.f;
    const float cx = lcd_lo.x + pad;
    const float cw = (lcd_hi.x - lcd_lo.x) - pad * 2.f;

    // Rozvrzeni se pocita PRED kreslenim, protoze pozadi potrebuje vedet,
    // kde konci lista a kde zacinaji kontrolky.
    const float top    = lcd_lo.y + pad + L::Dims::tab_h + L::Dims::gap;
    const float lamp_y = lcd_hi.y - pad - L::Dims::foot_h - L::Dims::lamp_h;
    const float foot_y = lcd_hi.y - pad - L::Dims::foot_h;
    const Rect body{ ImVec2(cx, top), ImVec2(cx + cw, lamp_y - L::Dims::gap) };

    background(ctx, dl, lcd_lo, lcd_hi, body.lo, body.hi,
               ctx.panels.page == PAGE_PLAY ? 1.0f : 0.42f);

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
