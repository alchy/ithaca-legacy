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
#include "dsp_state.h"
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

// -- Prostorove vyhlazeni modulace vlny -------------------------------------
// Historie hlasitosti moduluje amplitudu nosne vlny. Vzorek na vzorek umi
// skocit (uhoz do akordu), takze se okoli prumeruje — ale ZALEZI CIM.
//
// Puvodne to bylo obdelnikove okno sedmi vzorku s rovnymi vahami. Obdelnik ma
// na skok odezvu ve tvaru RAMPY s ostrymi rohy na obou koncich, a prave tyhle
// rohy byly na vlne videt jako zlomy ve chvilich naraznych zmen — na hladkem
// vstupu se neprojevily, takze to dlouho neslo poznat.
//
// Hannovo okno vahy na krajich stahuje k nule, takze odezva na skok je
// esovka bez rohu. Sirka je volena tak, aby sila vyhlazeni zustala stejna:
// obdelnik o sedmi vzorcich ma efektivni sirku ~2,0 vzorku, tohle devitivzorkove
// Hannovo take. Vlna tedy neztratila zivost, jen prestala mit rohy.
constexpr int   kHistHalf = 4;
constexpr float kHistW[2 * kHistHalf + 1] = {
    0.09549f, 0.34549f, 0.65451f, 0.90451f, 1.00000f,
    0.90451f, 0.65451f, 0.34549f, 0.09549f
};
constexpr float kHistWNorm = 1.f / 5.f;      // soucet vah je presne 5

// Kontrolky. Zhasle jsou taky videt — aby bylo poznat, ze existuji.
// Jsou na KAZDE strance: co je bezpecnostne dulezite, nesmi zmizet jen proto,
// ze uzivatel zrovna neco ladi.
//
// Sedi ve stejnem radku jako paticka, ne ve vlastnim pasu: samostatny pruh
// ukrajel 36 px vysky na kazde strance, coz je na 7" panelu citelne — a jde
// o tyz druh informace jako zbytek paticky (stav pristroje).
// NOTE a OFF sem pribyly z PLAY. Je to stav PRISTROJE, ne stranky: ze do
// nastroje chodi MIDI potrebujes vedet i ve chvili, kdy zrovna nastavujes DSP
// a nic nehraje — presne jako u UNDERRUN nebo CLIP. Na PLAY tim navic zbyl
// jeden sloupec, ktery na uzkem panelu chybel.
void lampRow(AppContext& ctx, ImDrawList* dl, ImVec2 pos, float w) {
    auto& ps = ctx.panels;
    const bool ur = ctx.engine.mainStreamUnderrunRecent(4000.f) ||
                    ctx.engine.resonanceStreamUnderrunRecent(4000.f);
    const bool clip = ctx.engine.masterPeakL() >= 0.999f ||
                      ctx.engine.masterPeakR() >= 0.999f;

    // Vyhlazeni MIDI lamp: engine dava jen ano/ne s oknem 120 ms, bez nej by
    // lampa cvakala. Vazane na CAS, ne na snimek, takze vypada stejne pri
    // jakemkoli fps.
    const float dt = ImGui::GetIO().DeltaTime;
    const float k  = 1.f - std::exp(-dt / 0.20f);
    ps.lamp_note += ((ctx.engine.noteOnRecent(120.f)  ? 1.f : 0.f) - ps.lamp_note) * k;
    ps.lamp_off  += ((ctx.engine.noteOffRecent(120.f) ? 1.f : 0.f) - ps.lamp_off)  * k;

    struct Lamp { const char* label; float on; ImU32 col; };
    const Lamp lamps[] = {
        { "NOTE",     ps.lamp_note,                  Colors::ink  },
        { "OFF",      ps.lamp_off,                   Colors::dim  },
        { "UNDERRUN", ur ? 1.f : 0.f,                Colors::warn },
        { "CLIP",     clip ? 1.f : 0.f,              Colors::warn },
        { "LOG",      ps.log_unseen ? 1.f : 0.f,     Colors::warn },
    };

    // Na stred paticky, mezi stitek a audio rezim.
    float total = 0.f;
    for (const Lamp& l : lamps) total += wdg::lampW(l.label);
    float x = pos.x + (w - total) * 0.5f;
    for (const Lamp& l : lamps) {
        wdg::lamp(dl, ImVec2(x, pos.y), l.label, l.on, l.col);
        x += wdg::lampW(l.label);
    }
}

// Paticka: stitek nastroje vlevo, audio rezim vpravo. Stitek je tu proto,
// ze tohle JE celni panel nastroje — ne aplikace, ktera se jmenuje v titulku okna.
void footer(AppContext& ctx, ImDrawList* dl, ImVec2 pos, float w,
            const char* profile) {
    const float px = wdg::fontPx(Fonts::small);
    // Stitek nese i to, na kterem profilu nastroj prave jede. Je to udaj,
    // ktery clovek chce videt, aniz by kvuli nemu lezl na SYS — a stitek je
    // jinak mrtve misto.
    dl->AddText(Fonts::small, px, pos, Colors::dimmer, "ITHACA LEGACY");
    if (profile) {
        const float x = pos.x + wdg::textW(Fonts::small, px, "ITHACA LEGACY") + 10.f;
        dl->AddText(Fonts::small, px, ImVec2(x, pos.y), Colors::dim, profile);
    }

    char buf[48];
    const int sr = ctx.engine.sampleRate();
    std::snprintf(buf, sizeof(buf), "%g kHz \xC2\xB7 %d",
                  (double)(sr > 0 ? sr : 48000) / 1000.0, ctx.engine.blockSize());
    dl->AddText(Fonts::small, px,
                ImVec2(pos.x + w - wdg::textW(Fonts::small, px, buf), pos.y),
                Colors::dimmer, buf);
}

// Vypln plochy displeje. Gradient: vpravo nahore svetlejsi, vlevo dole tmavsi.
void backgroundFill(ImDrawList* dl, ImVec2 lo, ImVec2 hi) {
    dl->AddRectFilledMultiColor(lo, hi,
        Colors::lcd,
        IM_COL32(0x18, 0x46, 0xa8, 255),
        IM_COL32(0x0c, 0x27, 0x66, 255),
        IM_COL32(0x08, 0x1c, 0x4c, 255));
}

// Stav vlny — JEDNOU ZA SNIMEK, oddelene od kresleni.
//
// Drive to bylo v jedne funkci s kreslenim, a ta se pri zapnutem sporici volala
// DVAKRAT (jednou pod ovladani, podruhe nad zavoj). Historie se tim posouvala
// dvakrat za snimek, takze vlna ve sporici plynula dvojnasobnou rychlosti a
// obalky mely polovicni casovou konstantu. Rozdeleni to resi z podstaty: stav
// se aktualizuje jednou, kreslit se smi kolikrat je potreba.
// -- Casove konstanty vlny --------------------------------------------------
// Vsechno vyhlazovani vlny bylo puvodne vazane na SNIMEK: koeficienty jako
// 0,10 nebo 0,995 se aplikovaly jednou za prekresleni. Pri 60 fps to vypadalo
// spravne, ale znamenalo to, ze animace zavisi na snimkove frekvenci —
// zahozeny snimek ji zmenil a pri 12 fps by se vlna sama zpomalila petkrat.
// Bez teto opravy nejde snizit tempo prekreslovani (a to je na Pi ta nejvetsi
// uspora, jaka je k dispozici).
//
// Konstanty jsou prepocitane z puvodnich koeficientu pri 60 fps:
//   tau = -(1/60) / ln(1 - k)
// takze pri 60 fps vypada vsechno PRESNE jako predtim a lisi se to teprve
// tam, kde bylo puvodni chovani spatne.
namespace wave_tau {
    inline constexpr float env_up   = 0.158f;   // bylo k = 0,10  (nastup noty)
    inline constexpr float env_down = 0.825f;   // bylo k = 0,020 (dozniv)
    inline constexpr float vis_up   = 0.110f;   // bylo k = 0,14
    inline constexpr float vis_down = 2.770f;   // bylo k = 0,006 (~4 s vyhasnuti)
    inline constexpr float ped_up   = 0.0747f;  // bylo k = 0,20
    inline constexpr float ped_down = 0.200f;   // bylo k = 0,08
    inline constexpr float norm     = 3.325f;   // bylo *0,995 za snimek
}

// Historie plyne v PEVNEM tempu, ne po snimcich: 60 polozek za vterinu, tedy
// pruchod sirkou (128 polozek) trva 2,13 s bez ohledu na to, kolikrat se
// mezitim prekreslilo.
constexpr float kHistRate  = 60.f;
// Dlouhy vypadek (load banky, prepnuti okna) se NEDOHANI — jinak by vlna
// skokem poskocila a obalky by se propadly. Stejna uvaha jako clamp
// v motion::Settle.
constexpr float kWaveDtMax = 0.10f;

void waveUpdate(AppContext& ctx) {
    auto& wv = ctx.panels.wave;
    constexpr int kN = ithaca::Engine::kScopeSize;
    // Lokalni, ne `static`: 8 KB na zasobniku je levnejsi nez skryty globalni
    // stav, ktery by po vyclenení GUI do knihovny sdilely vsechny instance.
    float raw_l[kN], raw_r[kN];
    ctx.engine.scopeSnapshot(raw_l, raw_r, kN);

    // JEDEN pruchod pro obe veliciny. Drive to byly tri (suma ctvercu + dvakrat
    // absPeak) pres tychz 2048 vzorku. Poradi akumulace je zachovane, takze
    // vysledek je bitove tentyz.
    float sl = 0.f, sr = 0.f, pk_l = 0.f, pk_r = 0.f;
    for (int i = 0; i < kN; ++i) {
        const float a = raw_l[i], b = raw_r[i];
        sl += a * a; sr += b * b;
        pk_l = std::max(pk_l, std::fabs(a));
        pk_r = std::max(pk_r, std::fabs(b));
    }
    const float rms_l = std::sqrt(sl / (float)kN);
    const float rms_r = std::sqrt(sr / (float)kN);

    // Krok casu misto "jednoho snimku" — viz wave_tau vyse.
    const float dt   = std::min(ImGui::GetIO().DeltaTime, kWaveDtMax);
    const auto  lag  = [dt](float tau) { return 1.f - std::exp(-dt / tau); };
    const float decay = std::exp(-dt / wave_tau::norm);

    // Auto-rozsah, aby vlna vypadala stejne pri tichem i hlasitem hrani —
    // pozadi neni meridlo. Spodni mez drzi ticho klidne.
    wv.norm_rms = std::max(std::max(rms_l, rms_r), wv.norm_rms * decay);
    const float ref = std::max(wv.norm_rms, 0.005f);

    // Pomaly follower: svizne nahoru (aby nastup noty byl videt), liny dolu
    // (aby dozniv plynul misto skoku). Tohle je to, co dela klid.
    const float k_up   = lag(wave_tau::env_up);
    const float k_down = lag(wave_tau::env_down);
    auto follow = [k_up, k_down](float& env, float target) {
        env += (target - env) * ((target > env) ? k_up : k_down);
    };
    follow(wv.env_l, std::clamp(rms_l / ref, 0.f, 1.f));
    follow(wv.env_r, std::clamp(rms_r / ref, 0.f, 1.f));

    // Viditelnost: rychle nabehne, ale vyhasina pomalu (~4 s), aby vizualizace
    // po dohrani jeste chvili dozila misto aby zmizela rezem. Prah je
    // ABSOLUTNI — kdyby se poustel z normalizovane urovne, auto-rozsah by
    // v tichu zesilil sum a vlna by nezhasla nikdy.
    const float lvl = std::max(rms_l, rms_r);
    const float vis_target = (lvl > 2.0e-4f) ? 1.f : 0.f;
    wv.vis += (vis_target - wv.vis)
            * lag(vis_target > wv.vis ? wave_tau::vis_up : wave_tau::vis_down);

    // Blizkost prehlceni: od -9 dB (0) k 0 dB (1). Bere skutecny peak metr,
    // ktery ma vlastni decay, takze kratka spicka zustane chvili videt.
    const float pk = std::max(ctx.engine.masterPeakL(), ctx.engine.masterPeakR());
    const float pk_db = (pk > 1e-6f) ? 20.f * std::log10(pk) : -120.f;
    wv.clip = std::clamp((pk_db + 9.f) / 9.f, 0.f, 1.f);

    // Nova hodnota do historie: HLASITOST bloku (0..1), ne znamenkova spicka.
    // pk_l/pk_r uz jsou spocitane vyse ve spolecnem pruchodu.
    wv.norm_peak = std::max(std::max(pk_l, pk_r), wv.norm_peak * decay);
    const float pref = std::max(wv.norm_peak, 0.01f);

    // Pedal: vlastni obalka i historie. Neni to zvuk, takze se nenormalizuje —
    // 0..127 je uz absolutni skala.
    const float ped = std::clamp((float)ctx.engine.pedalCC() / 127.f, 0.f, 1.f);
    wv.env_p += (ped - wv.env_p)
              * lag((ped > wv.env_p) ? wave_tau::ped_up : wave_tau::ped_down);

    // Posun historie je vazany na CAS, ne na snimek: nova polozka vstupuje
    // 60x za vterinu at se kresli jakkoli casto. Drive to byla jedna polozka
    // na snimek, takze pruchod sirkou trval 2,1 s pri 60 fps, ale 10,7 s pri
    // 12 fps — vlna by se pri usporne snimkove frekvenci sama zpomalila.
    wv.hist_acc += dt * kHistRate;
    // Strop poctu kroku: po dlouhem vypadku nema smysl dohanet, vlna by
    // poskocila. dt uz je omezene, tohle je jen pojistka.
    int steps = (int)wv.hist_acc;
    if (steps > 8) { steps = 8; wv.hist_acc = 0.f; }
    wv.hist_acc -= (float)steps;

    for (int s = 0; s < steps; ++s) {
        const int prev = wv.head;
        wv.head = (wv.head + 1) % PanelState::Wave::kHist;
        // Casove vyhlazeni pri vstupu: bez nej by kazdy krok skocil jinam a
        // podel vlny by vznikaly schody.
        wv.hist_l[wv.head] = 0.55f * std::clamp(pk_l / pref, 0.f, 1.f)
                           + 0.45f * wv.hist_l[prev];
        wv.hist_r[wv.head] = 0.55f * std::clamp(pk_r / pref, 0.f, 1.f)
                           + 0.45f * wv.hist_r[prev];
        wv.hist_p[wv.head] = 0.35f * wv.env_p + 0.65f * wv.hist_p[prev];
    }
}

// Stuhy. Cte stav, ktery pripravil waveUpdate — sam nic nemeni, takze se smi
// volat vickrat za snimek (pod ovladanim i nad zavojem sporice).
//
// KLICOVE: tvar NENI prubeh zvuku. Kreslit vzorky primo bylo pri pomalem tempu
// prilis neklidne — pozadi ma indikovat, ze zvuk hraje, ne aby se z nej dal
// cist tvar vlny. Tvar je proto parametricky (soucet tri pomalych sinusovek
// s driftujici fazi) a zvuk mu jen MODULUJE amplitudu pres pomalou obalku.
// Vysledek pri hre dycha, v tichu se sotva znatelne vlni.
// `vis` skaluje sytost vlny. PLAY je ambientni obrazovka, tam je vlna hvezda;
// ostatni jsou pracovni, tam ustoupi, aby neprochazela textem. Vypnout ji ale
// nelze — indikace, ze zvuk hraje, ma platit vsude.
void waveRibbons(AppContext& ctx, ImDrawList* dl, float w,
                 ImVec2 wave_lo, ImVec2 wave_hi, float vis) {
    const auto& wv = ctx.panels.wave;

    // -- Tvar --------------------------------------------------------------
    const float wh = wave_hi.y - wave_lo.y;
    const float axis = (ctx.panels.scope_center_y > 0.f)
                     ? ctx.panels.scope_center_y : (wave_lo.y + wh * 0.5f);
    const float room = std::max(24.f, std::min(axis - wave_lo.y, wave_hi.y - axis));
    const float t = (float)ImGui::GetTime();

    constexpr int kPts = 128;
    float  pts[kPts];             // lokalni, ne `static` — viz waveUpdate
    ImVec2 poly[kPts];            // body jednoho tahu; sdili je vsechny tri prujezdy zare

    // Ctyri stuhy ve dvou rodinach. KAZDY KANAL JE JINAK PROSVICEN — levy
    // svetly, pravy hlubsi modry — takze je od sebe poznas, i kdyz se prolinaji
    // kolem teze osy. Je to ambientni vizualizer: ma dychat, ne informovat.
    // src: 0 = levy kanal, 1 = pravy, 2 = pedal.
    // Pedalova stuha ma nizsi frekvenci — je to dlouhy klidny nadech pres celou
    // sirku, ktery se neplete s zivymi zvukovymi stuhami.
    //
    // POZOR na rychlost: fazova rychlost je speed/(2*pi*freq), takze prejezd
    // sirky trva 2*pi*freq/speed sekund. Pri freq 0.55 a speed 0.05 to bylo
    // 69 s a stuha vypadala zaseknuta s vrcholem porad na temze miste.
    // Se speed 0.19 je to ~18 s — plyne viditelne, ale porad klidneji nez zvuk.
    // Amplituda je zamerne jen ~3/4 puvodni: pedal je doprovodny stav, nema
    // prekrikovat stuhy, ktere nesou samotny zvuk.
    struct Ribbon { float phase, speed, freq, scale, alpha, th; ImU32 col; int src; };
    const Ribbon ribs[] = {
        { 0.0f, 0.16f, 1.00f, 1.00f, 0.30f, 2.6f, Colors::ink,    0 },
        { 2.3f, 0.11f, 1.00f, 0.72f, 0.15f, 1.8f, Colors::inv_bg, 0 },
        { 1.1f, 0.13f, 1.00f, 0.88f, 0.26f, 2.6f, Colors::fill,   1 },
        { 3.7f, 0.09f, 1.00f, 0.58f, 0.13f, 1.6f, Colors::line,   1 },
        { 5.2f, 0.19f, 0.55f, 0.86f, 0.20f, 3.0f, Colors::dim,    2 },
    };

    // Zar kolem krivky. Jadro je uzke a jasne, kolem nej spojity spad do ztracena
    // — cara ma pohasinat do okoli, ne byt pas s hranou. Tvar nese primo
    // geometrie (pruhlednost ve vrcholech), viz wdg::waveGlow.
    //
    // Jadro se drzi POD jednim pixelem polomeru: nad nim uz cara opticky
    // ztloustne a spojity spad se ztrati v plose. Dosah zare zustava tam, kde
    // byl nejsirsi z byvalych tri obtahu (th * 3.4).
    // Dosah zare je nasobeny parametrem (GuiState::wave_glow) pres regulator,
    // ktery ho smi snizit pod tlakem — viz glow_auto.h. Nula = hola cara.
    const float glow_scale = ctx.panels.glow.scale();
    auto glow = [&](const float* p, int n, float amp, ImU32 col, float a, float th) {
        if (a <= 0.004f) return;
        const int m = wdg::waveBuild(poly, kPts, wave_lo.x, w, axis, p, n, amp, 1.f);
        if (m < 2) return;
        // Nasobitel skaluje SPAD, ne cely polomer: pri 1 vyjde presne puvodni
        // dosah (th * 3.4), pri 0 zbyde jadro, tedy hola cara.
        //
        // Strop na VYSCE PLOCHY: zar se stejne orezava na `wave_hi/lo`, takze
        // za tou hranici uz je kazdy dalsi pixel jen vyplna navic pri naprosto
        // stejnem obrazu. Bez toho by --wave-glow 1000 vypadalo identicky jako
        // --wave-glow 50 a jen by to zabijelo grafiku.
        const float core = std::min(0.8f, th * 0.30f);
        const float span = std::min(std::max(th * 3.4f - core, 0.f) * glow_scale,
                                    wave_hi.y - wave_lo.y);
        wdg::waveGlow(dl, poly, m, core, core + span, col, a);
    };

    dl->PushClipRect(wave_lo, wave_hi, true);

    for (const Ribbon& R : ribs) {
        const float env = (R.src == 2) ? wv.env_p : (R.src == 1 ? wv.env_r : wv.env_l);
        // Pedalova stuha ma VLASTNI viditelnost: seslapnuty pedal ma byt videt
        // i v tichu, protoze drzeni pedalu je stav, ne zvuk.
        const float vis_r = (R.src == 2) ? wv.env_p : wv.vis;
        // Amplituda i sytost jdou s viditelnosti — vlna se pri utichnuti
        // zaroven splaskne a vytrati, misto aby zustala viset jako prazdny vzor.
        const float amp = room * R.scale * (0.14f + 0.86f * env) * vis_r;
        // Od -9 dB se stuha zacne barvit do cervena, v 0 dB je cervena.
        const ImU32 col = Colors::lerp(
            Colors::lerp(R.col, Colors::clip_lo, std::min(wv.clip * 2.f, 1.f)),
            Colors::clip_hi, std::max(0.f, wv.clip * 2.f - 1.f));
        const float* hist = (R.src == 2) ? wv.hist_p
                          : (R.src == 1 ? wv.hist_r : wv.hist_l);

        for (int i = 0; i < kPts; ++i) {
            const float u = (float)i / (float)(kPts - 1);
            const float a = u * 6.2831853f;
            // Nosna vlna. Vsechny slozky maji ZAPORNE znamenko u casu, takze
            // vzor putuje doprava — kdyby se znamenka michala, jen by se
            // treplo na miste. Ruzne rychlosti delaji mekky rozpad tvaru.
            // Koeficienty davaji v souctu 1.0, takze nosna sama nikdy nepresahne
            // rozsah a tanh nize pracuje v linearni oblasti misto v nasyceni.
            float v = 0.55f * std::sin(a * 1.0f * R.freq - t * R.speed)
                    + 0.30f * std::sin(a * 1.9f * R.freq - t * R.speed * 1.4f + R.phase)
                    + 0.15f * std::sin(a * 3.1f * R.freq - t * R.speed * 2.1f + R.phase * 0.6f);

            // Zvuk MODULUJE AMPLITUDU nosne vlny, neprictava se k vychylce —
            // proto zustava tvar hladky. Historie plyne doprava: nejnovejsi
            // hodnota je vlevo, starsi se odsouvaji.
            const int hn = PanelState::Wave::kHist;
            const int c = (int)(u * (hn - 1));
            float hsum = 0.f;
            for (int d = -kHistHalf; d <= kHistHalf; ++d) {
                // Index se ORIZNE na rozsah historie (opakuje se krajni
                // vzorek). Preskakovani by u kraju zuzilo okno a zmenila by se
                // tam sila vyhlazeni; a modulo pres kruhovy buffer by na levem
                // okraji sahlo "pred nejnovejsi vzorek" a pretecklo na konec
                // kruhu, kde lezi data stara dve vteriny.
                const int cd = std::clamp(c + d, 0, hn - 1);
                const int k  = (wv.head - cd + hn * 3) % hn;
                hsum += hist[k] * kHistW[d + kHistHalf];
            }
            const float hv = hsum * kHistWNorm;
            pts[i] = v * (0.28f + 0.72f * hv);
        }
        glow(pts, kPts, amp, col, R.alpha * vis * vis_r, R.th);
    }

    dl->PopClipRect();
}

} // namespace

void renderScreen(AppContext& ctx, ithaca::dsp::IParamPage** pages, int n_pages,
                  float W, float H) {

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
    // Profil displeje: na sirokem panelu ctvercove zalozky, na uzkem
    // obdelnikove (ctverec by tam snedl ctvrtinu vysky). Vyska radku se proto
    // pocita az tady, kdyz je znama velikost displeje. Nastavuje se JEDNOU za
    // snimek a cte ho cely panel — viz layout::Screen.
    L::setScreen(lcd_hi.x - lcd_lo.x, lcd_hi.y - lcd_lo.y, cw, PAGE_COUNT);
    const float tab_h = L::g_screen.tab_h;
    const float top    = lcd_lo.y + pad + tab_h + L::Dims::gap;
    // Paticka sedi u SPODNI hrany se stejnym odsazenim, jake ma pas zalozek
    // od horni (`pad`). Drive mela vlastni pasmo `foot_h` a text se kreslil
    // u jeho horniho okraje, takze pod nim zbyvalo 20 px navic a paticka
    // opticky plavala nad spodkem displeje.
    const float foot_h = wdg::fontPx(Fonts::small);
    const float foot_y = lcd_hi.y - pad - foot_h;
    const Rect body{ ImVec2(cx, top), ImVec2(cx + cw, foot_y - L::Dims::gap) };

    ctx.panels.lcd_center_y = (lcd_lo.y + lcd_hi.y) * 0.5f;

    const float wave_vis = (ctx.panels.page == PAGE_PLAY) ? 1.0f : 0.42f;
    const float lcd_w    = lcd_hi.x - lcd_lo.x;

    // Stav vlny se posune JEDNOU za snimek, at se pak kresli kolikrat chce.
    waveUpdate(ctx);
    backgroundFill(dl, lcd_lo, lcd_hi);

    // Dosah zare: rucni zmena parametru prebije to, kam dosel regulator.
    // Vstupem regulace je PERIODA snimku, ne cas kresleni — viz glow_auto.h.
    if (ctx.panels.glow_want != ctx.state.wave_glow) {
        ctx.panels.glow_want = ctx.state.wave_glow;
        ctx.panels.glow.reset(ctx.state.wave_glow);
    }
    {
        const float dt = ImGui::GetIO().DeltaTime;
        ctx.panels.glow.step(dt * 1000.f, ctx.state.wave_glow,
                             ctx.state.wave_glow_budget_ms, dt);
    }

    // Stuhy POD ovladanim. Ve sporici je prekryje zavoj, takze kdyz uz je
    // temer neprusvitny, nema smysl je kreslit — prah je tentyz, jaky pouziva
    // wdg::waveGlow pro zanedbatelnou pruhlednost.
    const float veil0 = 1.f - ctx.panels.chrome_a;
    if (veil0 < 0.996f)
        waveRibbons(ctx, dl, lcd_w, body.lo, body.hi, wave_vis);

    // -- Sporic ------------------------------------------------------------
    // Po peti minutach bez DOTYKU se ovladaci prvky pomalu vytrati a zustane
    // jen vlna. Hrani obrazovku NEprobouzi: kdyz hrajes, panel nepotrebujes.
    // Prvni dotek vrati vsechno hned — proto je nabeh rychly a odchod pomaly.
    //
    // Ma to i prakticky duvod nez ze to vypada dobre: lista, ramecky a inverzni
    // pole jsou porad na stejnem miste. Na OLED panelu (a z tech tvoje reference
    // vychazi) je to presne recept na vypaleni.
    {
        auto& ps = ctx.panels;
        ImGuiIO& io = ImGui::GetIO();
        const float dt = io.DeltaTime;
        const bool touched = ImGui::IsAnyMouseDown() || io.MouseWheel != 0.f
                          || io.MouseDelta.x != 0.f || io.MouseDelta.y != 0.f;
        ps.idle_t = touched ? 0.f : (ps.idle_t + dt);
        const bool saver = ps.idle_t > 300.f;          // 5 minut
        const float k = 1.f - std::exp(-dt / (saver ? 2.5f : 0.18f));
        ps.chrome_a += ((saver ? 0.f : 1.f) - ps.chrome_a) * k;
    }

    // -- Zalozky --
    ImGui::SetCursorScreenPos(ImVec2(cx, lcd_lo.y + pad));
    ImGui::PushID("tabs");
    if (ctx.panels.page < 0 || ctx.panels.page >= PAGE_COUNT) ctx.panels.page = PAGE_PLAY;
    int page = ctx.panels.page;
    // Sirku pro tabBar bere z content region — omezime ji na plochu displeje.
    ImGui::PushItemWidth(cw);
    {
        // tabBar cte GetContentRegionAvail().x; docasne zuzime pres child.
        ImGui::BeginChild("##tabhost", ImVec2(cw, tab_h), false,
                          ImGuiWindowFlags_NoScrollbar);
        wdg::tabBar("tab", kTabs, PAGE_COUNT, page, tab_h, true);
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
        case PAGE_SYS:  pageSys(ctx, body, pages, n_pages); break;
        case PAGE_LOG:  pageLog(ctx, body); break;
        default: break;
    }

    // Na kterem profilu nastroj jede. Staci porovnat s TOVARNIM: USER profil
    // se uklada sam pri ukonceni, takze cokoli jineho nez tovarni hodnoty uz
    // je uzivatelovo nastaveni — treti stav "rozpracovano" by nic nerekl.
    const char* profile = pagesAreFactory(pages, n_pages) ? "[FACTORY]" : "[USER]";
    footer(ctx, dl, ImVec2(cx, foot_y), cw, profile);
    lampRow(ctx, dl, ImVec2(cx, foot_y), cw);

    // Zavoj sporice: prekryje UZ NAKRESLENE ovladani barvou pozadi, a vlna se
    // pak dokresli znovu pres nej. Diky tomu se ztlumi jen chrome — vlnu by
    // pruhledny prekryv jinak ztlumil taky.
    if (ctx.panels.chrome_a < 0.995f) {
        const float veil = 1.f - ctx.panels.chrome_a;
        ImGui::PushClipRect(lcd_lo, lcd_hi, false);
        dl->AddRectFilledMultiColor(lcd_lo, lcd_hi,
            IM_COL32(0x0e, 0x2f, 0x7a, (int)(veil * 255)),
            IM_COL32(0x18, 0x46, 0xa8, (int)(veil * 255)),
            IM_COL32(0x0c, 0x27, 0x66, (int)(veil * 255)),
            IM_COL32(0x08, 0x1c, 0x4c, (int)(veil * 255)));
        ImGui::PopClipRect();
        // Jen stuhy — vypln uz je pod zavojem. Drive se tady volalo cele
        // pozadi vcetne gradientu a vcetne posunu historie vlny.
        waveRibbons(ctx, dl, lcd_w, body.lo, body.hi, wave_vis * veil);
    }

    ImGui::End();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

} // namespace ithaca::gui
