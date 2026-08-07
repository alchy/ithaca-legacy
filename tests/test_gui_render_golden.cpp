// tests/test_gui_render_golden.cpp — otisk vykresleneho snimku bez displeje.
// ----------------------------------------------------------------------------
// K cemu to je. Refaktor GUI (sjednoceni widgetu, jedna geometrie misto tri,
// vymena GLFW za SDL3) ma jednu spolecnou vlastnost: vysledny snimek se NESMI
// zmenit. Overovat to okem na panelu, ktery je na stole az pristi tyden, nejde.
//
// ImGui zadny backend nepotrebuje — `Render()` vyprodukuje ImDrawData i do
// prazdna. Otisk vertex a index bufferu je tedy presny popis toho, co by se
// nakreslilo. Kdyz se otisk nezmeni, nezmenil se ani obraz.
//
// Vedle otisku se vypisuje POCET VERTEXU. Ten je narozdil od casu nezasumeny
// (viz app/gui/frame_stats.h) a je to hlavni merítko ucinku optimalizaci: kdyz
// se `kPts` snizi na polovinu, ma pocet vertexu klesnout na polovinu, a kdyz se
// zapne texturovy antialiasing, ma klesnout skokove.
//
// Baseline je LOKALNI (soubor vedle testu), ne commitnuta: rasterizace pisma i
// poradi float operaci se lisi mezi prekladaci a platformami, takze sdilena
// baseline by na jinem stroji hlasila faleSne poplachy. Prvni spusteni ji
// zalozi a projde; kazde dalsi porovnava. Zamerna zmena vzhledu:
//
//     ITHACA_GOLDEN_UPDATE=1 ctest -R gui_render_golden
//
// POZOR na poradi scenaru: ImGui::GetTime() bezi napric celym testem, takze
// scenar zavisi na tom, kolik snimku se odehralo pred nim. Nove scenare se
// proto pridavaji NA KONEC — jinak se prepisou otisky vsech nasledujicich.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "imgui.h"

#include "app_context.h"
#include "pages.h"
#include "master_page.h"
#include "resonance_page.h"
#include "theme.h"
#include "layout.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <string>
#include <vector>

namespace {

using namespace ithaca::gui;

// -- Otisk -------------------------------------------------------------------

uint64_t fnv1a(const void* data, size_t n, uint64_t h = 1469598103934665603ull) {
    const unsigned char* p = static_cast<const unsigned char*>(data);
    for (size_t i = 0; i < n; ++i) { h ^= p[i]; h *= 1099511628211ull; }
    return h;
}

struct Shot {
    uint64_t hash = 0;
    int      vtx = 0, idx = 0, cmds = 0;
};

Shot fingerprint(const ImDrawData* dd) {
    Shot s;
    if (!dd) return s;
    s.cmds = dd->CmdListsCount;
    s.vtx  = dd->TotalVtxCount;
    s.idx  = dd->TotalIdxCount;
    uint64_t h = fnv1a(&s.cmds, sizeof(s.cmds));
    for (int i = 0; i < dd->CmdListsCount; ++i) {
        const ImDrawList* cl = dd->CmdLists[i];
        h = fnv1a(cl->VtxBuffer.Data, (size_t)cl->VtxBuffer.Size * sizeof(ImDrawVert), h);
        h = fnv1a(cl->IdxBuffer.Data, (size_t)cl->IdxBuffer.Size * sizeof(ImDrawIdx), h);
    }
    s.hash = h;
    return s;
}

// -- Prostredi ---------------------------------------------------------------
// Vsechno, co by cetlo skutecny stroj (MIDI porty, adresare s bankami), je
// predvyplnene pevnymi hodnotami — jinak by otisk zavisel na tom, co ma clovek
// zrovna zapojene a v jakem adresari test spustil.

struct Harness {
    AppContext                 ctx;
    MasterPage                 master{ctx};
    ResonancePage              reso{ctx};
    std::vector<ithaca::dsp::IParamPage*> pages;

    Harness() {
        ithaca::EngineConfig cfg;
        cfg.sample_rate = 48000;
        cfg.block_size  = 256;
        cfg.rt_priority = false;            // test neni audio aplikace
        REQUIRE(ctx.engine.init(cfg));

        pages = { &master, &reso,
                  &ctx.engine.dspChain().stage(0), &ctx.engine.dspChain().stage(1),
                  &ctx.engine.dspChain().stage(2), &ctx.engine.dspChain().stage(3) };

        resetPanels();
    }

    // Pevny stav panelu. Vola se pred kazdym scenarem, aby jeden nezavisel na
    // tom, co po sobe nechal predchozi.
    void resetPanels() {
        ctx.panels = PanelState{};
        ctx.panels.browse_dir = "/fixed/banks";
        ctx.panels.banks = { {"/fixed/banks/alpha", "alpha"},
                             {"/fixed/banks/beta",  "beta"},
                             {"/fixed/banks/gamma", "gamma"} };
        ctx.panels.banks_valid = true;      // ensureBankList uz nesahne na disk
        ctx.panels.midi_ports = { "Fixed Port A", "Fixed Port B" };
        ctx.panels.midi_ports_scanned = true;
        ctx.state.bank_path = "/fixed/banks/beta";
        ctx.state.midi_port_name = "Fixed Port A";
        // Rezim nahledu: prestavi dosah zare, aby slo porovnat varianty bez
        // displeje. Otisk se v tomhle rezimu NEporovnava (viz nize) — je to
        // prohlizeni, ne overovani.
        if (const char* g = std::getenv("ITHACA_GLOW"))
            ctx.state.wave_glow = (float)std::atof(g);
        seedWaveHistory();
    }

    // Rozehrat vlnu a DRZET ji rozehranou. Bez tohohle je test slepy vuci tomu
    // nejdrazsimu, co panel kresli: v tichu je `vis` nula, waveLine se hned na
    // zacatku vrati a stuhy se nenakresli VUBEC — pocet vertexu vyjde kolem 700
    // misto tisicu a optimalizace stuh (jedna geometrie misto tri, mensi kPts,
    // texturovy antialiasing) by na otisku nebyly videt.
    //
    // Engine v testu mlci (zadna banka, zadne audio), takze obalky pri kazdem
    // snimku klesaji k nule. Proto se nepinuji jednou na zacatku, ale PRED
    // KAZDYM snimkem: scenar tim odpovida stavu "nastroj hraje", coz je stav,
    // ve kterem je panel nejdrazsi a ktery chceme optimalizovat.
    void pinWave() {
        auto& w = ctx.panels.wave;
        w.vis = 1.f;
        w.env_l = 0.80f; w.env_r = 0.60f; w.env_p = 0.30f;
        w.norm_rms = 0.20f; w.norm_peak = 0.50f;
        w.clip = 0.f;
    }

    // Historie se plni jen jednou, na zacatku scenare — ta se smi vyvijet.
    void seedWaveHistory() {
        auto& w = ctx.panels.wave;
        w.head = 0;
        // Rezim nahledu ITHACA_WAVE_STEP: misto hladkeho tvaru se do historie
        // da SKOK. Odpovida to naraznemu zesileni (uhoz do akordu) a je to
        // jediny stav, ve kterem je videt, jak se chova prostorove vyhlazeni
        // modulace — pri hladkem vstupu vypada dobre kazde okno.
        const bool step = std::getenv("ITHACA_WAVE_STEP") != nullptr;
        for (int i = 0; i < PanelState::Wave::kHist; ++i) {
            const float u = (float)i / (float)PanelState::Wave::kHist;
            if (step) {
                const float s = (i > PanelState::Wave::kHist / 2) ? 0.95f : 0.10f;
                w.hist_l[i] = s; w.hist_r[i] = s; w.hist_p[i] = s;
            } else {
                // Pevny nemonotonni tvar — jen aby historie nebyla konstantni.
                w.hist_l[i] = 0.50f + 0.40f * std::sin(u * 12.f);
                w.hist_r[i] = 0.45f + 0.35f * std::sin(u * 9.f + 1.3f);
                w.hist_p[i] = 0.30f + 0.20f * std::sin(u * 3.f + 0.7f);
            }
        }
    }
};

void imguiInit() {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;               // zadny imgui.ini vedle testu
    io.LogFilename = nullptr;
    ithaca::gui::layout::g_scale = 1.f;
    theme::apply_theme();
    theme::load_fonts(1.f);
    io.FontGlobalScale = 1.f;
    if (theme::Fonts::ui) io.FontDefault = theme::Fonts::ui;
    // Bez rendereru se atlas nepostavi sam; NewFrame() na to ma assert.
    io.Fonts->Build();
    io.Fonts->SetTexID((ImTextureID)(intptr_t)1);
}

// Odrenderuje `frames` snimku dane stranky a vrati otisk POSLEDNIHO.
// Vic nez jeden snimek proto, ze cast panelu je animovana (vlna, dosedani
// vytahu) — otisk prvniho snimku by nerekl nic o ustalenem stavu.
Shot renderPage(Harness& h, int page, float W, float H, bool saver = false,
                int frames = 8) {
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(W, H);
    h.resetPanels();
    h.ctx.panels.page = page;
    // Sporic: po peti minutach necinnosti se ovladani vytraci. V tom stavu
    // screen.cpp kresli pozadi PODRUHE (zavoj + stuhy pres nej), takze se
    // geometrie vlny zdvoji — a protoze background() zaroven posouva historii,
    // vlna v sporici plyne dvojnasobnou rychlosti. Scenar to drzi pod dohledem.
    h.ctx.panels.idle_t = saver ? 400.f : 0.f;

    // Rezim nahledu ITHACA_FPS: prekresluje se jinou frekvenci, ale po STEJNOU
    // simulovanou dobu (pocet snimku se dopocita). Slouzi k overeni, ze
    // vyhlazovani vlny je vazane na cas a ne na snimek — pri 12 fps ma vyjit
    // temer tentyz obraz jako pri 60.
    float fps = 60.f;
    if (const char* f = std::getenv("ITHACA_FPS")) {
        const float v = (float)std::atof(f);
        if (v > 1.f && v < 1000.f) {
            fps    = v;
            frames = std::max(2, (int)((float)frames * v / 60.f + 0.5f));
        }
    }

    Shot s;
    for (int f = 0; f < frames; ++f) {
        h.pinWave();                        // stav "nastroj hraje", viz Harness
        io.DeltaTime = 1.f / fps;           // pevny krok = deterministicky cas
        ImGui::NewFrame();
        renderScreen(h.ctx, h.pages.data(), (int)h.pages.size(), W, H);
        ImGui::Render();
        s = fingerprint(ImGui::GetDrawData());
    }
    return s;
}

// -- Softwarovy rasterizer ---------------------------------------------------
// ImDrawData jsou obycejne trojuhelniky s barvou a UV. Slozit si je do bufferu
// znamena videt panel bez jakehokoli displeje — a diky tomu posuzovat vzhled
// (profil zare napric carou, prekryv, mezery) i pri praci na stroji, kde zadny
// panel neni. Zapina se promennou ITHACA_GOLDEN_DUMP=<scenar>.
struct Image {
    int w = 0, h = 0;
    std::vector<unsigned char> rgba;
};

void blend(unsigned char* dst, float r, float g, float b, float a) {
    dst[0] = (unsigned char)(r * a + dst[0] * (1.f - a) + 0.5f);
    dst[1] = (unsigned char)(g * a + dst[1] * (1.f - a) + 0.5f);
    dst[2] = (unsigned char)(b * a + dst[2] * (1.f - a) + 0.5f);
    dst[3] = (unsigned char)(255.f * a + dst[3] * (1.f - a) + 0.5f);
}

void rasterize(const ImDrawData* dd, Image& img) {
    ImGuiIO& io = ImGui::GetIO();
    unsigned char* tex = nullptr; int tw = 0, th = 0;
    io.Fonts->GetTexDataAsAlpha8(&tex, &tw, &th);

    img.rgba.assign((size_t)img.w * img.h * 4, 0);

    for (int n = 0; n < dd->CmdListsCount; ++n) {
        const ImDrawList* cl = dd->CmdLists[n];
        for (const ImDrawCmd& cmd : cl->CmdBuffer) {
            if (cmd.UserCallback) continue;
            const ImDrawIdx* idx = cl->IdxBuffer.Data + cmd.IdxOffset;
            const ImDrawVert* vtx = cl->VtxBuffer.Data + cmd.VtxOffset;
            const float cx0 = std::max(cmd.ClipRect.x, 0.f);
            const float cy0 = std::max(cmd.ClipRect.y, 0.f);
            const float cx1 = std::min(cmd.ClipRect.z, (float)img.w);
            const float cy1 = std::min(cmd.ClipRect.w, (float)img.h);

            for (unsigned e = 0; e + 2 < cmd.ElemCount; e += 3) {
                const ImDrawVert& a = vtx[idx[e + 0]];
                const ImDrawVert& b = vtx[idx[e + 1]];
                const ImDrawVert& c = vtx[idx[e + 2]];
                const float area = (b.pos.x - a.pos.x) * (c.pos.y - a.pos.y)
                                 - (c.pos.x - a.pos.x) * (b.pos.y - a.pos.y);
                if (std::fabs(area) < 1e-9f) continue;

                int x0 = (int)std::floor(std::min({a.pos.x, b.pos.x, c.pos.x}));
                int x1 = (int)std::ceil (std::max({a.pos.x, b.pos.x, c.pos.x}));
                int y0 = (int)std::floor(std::min({a.pos.y, b.pos.y, c.pos.y}));
                int y1 = (int)std::ceil (std::max({a.pos.y, b.pos.y, c.pos.y}));
                x0 = std::max(x0, (int)cx0); x1 = std::min(x1, (int)cx1);
                y0 = std::max(y0, (int)cy0); y1 = std::min(y1, (int)cy1);

                for (int y = y0; y < y1; ++y)
                for (int x = x0; x < x1; ++x) {
                    const float px = x + 0.5f, py = y + 0.5f;
                    float w0 = ((b.pos.x - px) * (c.pos.y - py)
                              - (c.pos.x - px) * (b.pos.y - py)) / area;
                    float w1 = ((c.pos.x - px) * (a.pos.y - py)
                              - (a.pos.x - px) * (c.pos.y - py)) / area;
                    float w2 = 1.f - w0 - w1;
                    if (w0 < 0.f || w1 < 0.f || w2 < 0.f) continue;

                    auto ch = [&](const ImDrawVert& v, int s) {
                        return (float)((v.col >> (s * 8)) & 0xFF);
                    };
                    const float r = w0 * ch(a,0) + w1 * ch(b,0) + w2 * ch(c,0);
                    const float g = w0 * ch(a,1) + w1 * ch(b,1) + w2 * ch(c,1);
                    const float bl = w0 * ch(a,2) + w1 * ch(b,2) + w2 * ch(c,2);
                    float al = (w0 * ch(a,3) + w1 * ch(b,3) + w2 * ch(c,3)) / 255.f;

                    // Textura je alpha8 atlas pisma; vlna sazi na bily pixel.
                    const float u = w0 * a.uv.x + w1 * b.uv.x + w2 * c.uv.x;
                    const float v = w0 * a.uv.y + w1 * b.uv.y + w2 * c.uv.y;
                    const int tx = std::clamp((int)(u * tw), 0, tw - 1);
                    const int ty = std::clamp((int)(v * th), 0, th - 1);
                    al *= (float)tex[ty * tw + tx] / 255.f;
                    if (al <= 0.f) continue;
                    blend(&img.rgba[((size_t)y * img.w + x) * 4], r, g, bl, al);
                }
            }
        }
    }
}

void writePPM(const char* path, const Image& img) {
    std::ofstream f(path, std::ios::binary);
    f << "P6\n" << img.w << ' ' << img.h << "\n255\n";
    for (size_t i = 0; i < (size_t)img.w * img.h; ++i)
        f.write((const char*)&img.rgba[i * 4], 3);
}

// -- Baseline ----------------------------------------------------------------

const char* kBaselineFile = "gui_golden.txt";

std::map<std::string, Shot> loadBaseline() {
    std::map<std::string, Shot> m;
    std::ifstream f(kBaselineFile);
    if (!f) return m;
    std::string name;
    Shot s;
    while (f >> name >> s.hash >> s.vtx >> s.idx >> s.cmds) m[name] = s;
    return m;
}

void saveBaseline(const std::map<std::string, Shot>& m) {
    std::ofstream f(kBaselineFile, std::ios::trunc);
    for (const auto& [name, s] : m)
        f << name << ' ' << s.hash << ' ' << s.vtx << ' ' << s.idx << ' '
          << s.cmds << '\n';
}

bool wantUpdate() {
    const char* e = std::getenv("ITHACA_GOLDEN_UPDATE");
    return e && *e && std::strcmp(e, "0") != 0;
}

} // namespace

using namespace ithaca::gui;

TEST_CASE("otisk vykresleneho panelu") {
    imguiInit();
    Harness h;

    struct Scenario { const char* name; int page; float w, h;
                      bool saver = false; int frames = 8; };
    // Poradi je soucasti otisku (viz hlavicka souboru) — nove pridavat NA KONEC.
    const Scenario kScenarios[] = {
        { "play_1280x720",  PAGE_PLAY, 1280.f, 720.f },
        { "bank_1280x720",  PAGE_BANK, 1280.f, 720.f },
        { "tone_1280x720",  PAGE_TONE, 1280.f, 720.f },
        { "reso_1280x720",  PAGE_RESO, 1280.f, 720.f },
        { "dsp_1280x720",   PAGE_DSP,  1280.f, 720.f },
        { "sys_1280x720",   PAGE_SYS,  1280.f, 720.f },
        { "log_1280x720",   PAGE_LOG,  1280.f, 720.f },
        // 4,3" panel. Dnes se na nem tri stranky rozpadaji (SYS prekryv radku,
        // DSP zahodi dva parametry, PLAY prekryv sloupcu) — otisk to nehodnoti,
        // ale az se Compact profil postavi, bude proti cemu porovnavat.
        { "play_800x480",   PAGE_PLAY,  800.f, 480.f },
        { "bank_800x480",   PAGE_BANK,  800.f, 480.f },
        { "tone_800x480",   PAGE_TONE,  800.f, 480.f },
        { "reso_800x480",   PAGE_RESO,  800.f, 480.f },
        { "dsp_800x480",    PAGE_DSP,   800.f, 480.f },
        { "sys_800x480",    PAGE_SYS,   800.f, 480.f },
        { "log_800x480",    PAGE_LOG,   800.f, 480.f },
        // Sporic — stav, ve kterem panel travi vetsinu zivota. Dvacet vterin
        // proto, ze zavoj nabiha s casovou konstantou 2,5 s a plne neprusvitny
        // je az kolem 14. vteriny; teprve tam je videt cela cena kresleni
        // pozadi nadvakrat. Sporic se zapina po peti minutach necinnosti a pak
        // drzi hodiny, takze ustaleny stav je ten realny.
        { "play_saver_1280x720", PAGE_PLAY, 1280.f, 720.f, true, 1200 },
    };

    // DOCASNA SONDA: obsah pasu pecenych car v atlasu.
    {
        ImGuiIO& io = ImGui::GetIO();
        unsigned char* px = nullptr; int tw = 0, th = 0;
        io.Fonts->GetTexDataAsAlpha8(&px, &tw, &th);
        std::printf("[diag] atlas %dx%d\n", tw, th);
        for (int wq = 1; wq <= 5; ++wq) {
            const ImVec4& uv = io.Fonts->TexUvLines[wq];
            const int x0 = (int)(uv.x * tw), x1 = (int)(uv.z * tw);
            const int yy = (int)(uv.y * th);
            std::printf("[diag] sirka %d: y=%d x=%d..%d  alfa:", wq, yy, x0, x1);
            for (int x = x0 - 2; x <= x1 + 2; ++x)
                std::printf(" %3d", (x >= 0 && x < tw) ? px[yy * tw + x] : -1);
            std::printf("\n");
        }
    }

    const char* dump = std::getenv("ITHACA_GOLDEN_DUMP");

    std::map<std::string, Shot> now;
    for (const auto& sc : kScenarios) {
        now[sc.name] = renderPage(h, sc.page, sc.w, sc.h, sc.saver, sc.frames);
        if (dump && std::strcmp(dump, sc.name) == 0) {
            Image img; img.w = (int)sc.w; img.h = (int)sc.h;
            rasterize(ImGui::GetDrawData(), img);
            char path[128];
            std::snprintf(path, sizeof(path), "%s.ppm", sc.name);
            writePPM(path, img);
            std::printf("[dump] %s (%dx%d)\n", path, img.w, img.h);
        }
    }

    // Tabulka se tiskne VZDY — pocty vertexu jsou to, cim se meri ucinek
    // optimalizaci na stroji, kde je mereni casu bezcenne.
    std::printf("\n%-18s %10s %10s %6s\n", "scenar", "vtx", "idx", "cmd");
    for (const auto& [name, s] : now)
        std::printf("%-18s %10d %10d %6d\n", name.c_str(), s.vtx, s.idx, s.cmds);
    std::printf("\n");

    // Nahled varianty vzhledu — otisk by nutne nesedel, protoze se kresli
    // neco jineho, nez na cem byla baseline porizena.
    if (std::getenv("ITHACA_GLOW") || std::getenv("ITHACA_WAVE_STEP")
        || std::getenv("ITHACA_FPS")) {
        std::printf("[golden] rezim nahledu (ITHACA_GLOW), otisk se neporovnava\n");
        ImGui::DestroyContext();
        return;
    }

    const auto base = loadBaseline();

    if (base.empty() || wantUpdate()) {
        saveBaseline(now);
        std::printf("[golden] baseline %s (%zu scenaru) %s\n", kBaselineFile,
                    now.size(), base.empty() ? "zalozena" : "prepsana");
        ImGui::DestroyContext();
        return;
    }

    for (const auto& [name, s] : now) {
        const auto it = base.find(name);
        if (it == base.end()) {
            std::printf("[golden] novy scenar '%s' — pridej pres "
                        "ITHACA_GOLDEN_UPDATE=1\n", name.c_str());
            continue;
        }
        const Shot& b = it->second;
        INFO("scenar " << name << ": vtx " << b.vtx << " -> " << s.vtx
             << ", idx " << b.idx << " -> " << s.idx
             << ", cmd " << b.cmds << " -> " << s.cmds);
        CHECK(s.hash == b.hash);
    }

    ImGui::DestroyContext();
}
