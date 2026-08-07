// app/gui/page_bank.cpp — BANK: prohlizec adresaru + fakta o nactene bance.
//
// Na panelu neni klavesnice, takze cestu nelze napsat. Kdyz neni nastaveny
// bank_search_dir, prochazi se adresare od toho, ve kterem aplikace startovala.
//
// Zobrazuji se JEN adresare, ktere vypadaji jako banka, a adresare, ktere
// nejakou banku obsahuji — ne cely filesystem. Rozpoznani je zamerne LEVNA
// SONDA (existuje uvnitr slozka m###/ nebo soubor banky?), ne plny sken:
// procházení musi byt okamzite, load trva vteriny a deje se az na PLAY.
#include "pages.h"
#include "app_context.h"
#include "widgets.h"
#include "theme.h"
#include "layout.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <system_error>

namespace ithaca::gui {

namespace fs = std::filesystem;
namespace L  = ithaca::gui::layout;
using theme::Colors;
using theme::Fonts;

namespace {

// Levna sonda: vypada tenhle adresar jako banka? Hledame charakteristickou
// strukturu (m060/, m061/ …) nebo pakovany soubor — a hned prestaneme.
// ZADNY rekurzivni sken; na sitovem disku by to jinak zamrzlo.
bool looksLikeBank(const fs::path& dir) {
    std::error_code ec;
    int checked = 0;
    for (fs::directory_iterator it(dir, ec), end; it != end && checked < 64;
         it.increment(ec), ++checked) {
        if (ec) break;
        const std::string nm = it->path().filename().string();
        if (nm.size() == 4 && nm[0] == 'm' &&
            std::isdigit((unsigned char)nm[1]) &&
            std::isdigit((unsigned char)nm[2]) &&
            std::isdigit((unsigned char)nm[3]))
            return true;                                   // m### = rozbalena banka
        if (nm.size() > 7 && nm.rfind(".ithaca") == nm.size() - 7)
            return true;                                   // pakovana banka
    }
    return false;
}

// Sesbira nabidku: podadresare, ktere jsou banka. Kdyz zadny takovy neni,
// nabidne podadresare k dalsimu prochazeni (aby slo dojit hloub).
void rescan(PanelState& ps) {
    ps.banks.clear();
    std::error_code ec;
    if (ps.browse_dir.empty() || !fs::is_directory(ps.browse_dir, ec)) {
        ps.banks_valid = true;
        return;
    }
    std::vector<BankEntry> banks, dirs;
    for (fs::directory_iterator it(ps.browse_dir, ec), end; it != end; it.increment(ec)) {
        if (ec) break;
        if (!it->is_directory(ec)) continue;
        const std::string nm = it->path().filename().string();
        if (!nm.empty() && nm[0] == '.') continue;         // skryte preskoc
        BankEntry e{ it->path().string(), nm };
        (looksLikeBank(it->path()) ? banks : dirs).push_back(std::move(e));
    }
    auto byName = [](const BankEntry& a, const BankEntry& b) { return a.name < b.name; };
    std::sort(banks.begin(), banks.end(), byName);
    std::sort(dirs.begin(),  dirs.end(),  byName);
    ps.banks = std::move(banks);
    // Prochazet dal jde jen tam, kde zadna banka primo neni — jinak by seznam
    // michal banky s nahodnymi slozkami.
    if (ps.banks.empty()) ps.banks = std::move(dirs);
    ps.banks_valid = true;
}

} // namespace

void ensureBankList(AppContext& ctx) {
    auto& ps = ctx.panels;
    if (ps.browse_dir.empty()) {
        // Priorita: nastaveny search dir → rodic nactene banky → adresar startu.
        std::error_code ec;
        if (!ctx.state.bank_search_dir.empty())
            ps.browse_dir = ctx.state.bank_search_dir;
        else if (!ctx.state.bank_path.empty())
            ps.browse_dir = fs::path(ctx.state.bank_path).parent_path().string();
        else
            ps.browse_dir = fs::current_path(ec).string();
        ps.banks_valid = false;
    }
    if (ps.banks_valid) return;

    rescan(ps);
    // Po prescanu ukaz na prave nactenou banku, at vytah nezacina jinde.
    for (size_t i = 0; i < ps.banks.size(); ++i) {
        if (ps.banks[i].dir == ctx.state.bank_path) {
            ps.reel_sel = (int)i;
            ps.reel.snapTo((float)i * layout::Dims::row_h);
            break;
        }
    }
}

void pageBank(AppContext& ctx, const Rect& r) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    auto& ps = ctx.panels;

    ensureBankList(ctx);

    const float px_s = wdg::fontPx(Fonts::small);
    const float px_u = wdg::fontPx(Fonts::ui);

    // -- Rozpocet plochy ----------------------------------------------------
    // Odshora zahlavi, odzdola akcni pas a nad nim radek faktu; co zbyde,
    // je seznam. Drive to byl retez rucnich odectu (btn_y, fy, sep_y), ve
    // kterem nebylo poznat, ktere cislo je vyska pasu a ktere mezera.
    L::Band band{r};
    const Rect head  = band.takeTop(px_s, 14.f);
    const Rect btn   = band.takeBottom(L::Dims::touch, 16.f);
    const Rect fact_row = band.takeBottom(px_s, 10.f);
    const Rect list     = band.rest();

    // -- Zahlavi: aktualni adresar + o uroven vys --------------------------
    dl->AddText(Fonts::small, px_s, head.lo, Colors::dimmer, ps.browse_dir.c_str());

    ImGui::SetCursorScreenPos(ImVec2(head.hi.x - L::Dims::nav_w, head.lo.y - 6.f));
    if (ImGui::InvisibleButton("##up", ImVec2(L::Dims::nav_w, L::Dims::touch * 0.6f))) {
        std::error_code ec;
        const fs::path p(ps.browse_dir);
        if (p.has_parent_path() && p.parent_path() != p) {
            ps.browse_dir = p.parent_path().string();
            ps.banks_valid = false;
        }
    }
    {
        const char* up = "\xE2\x96\xB2 UP ONE LEVEL";
        dl->AddText(Fonts::small, px_s,
                    ImVec2(head.hi.x - wdg::textW(Fonts::small, px_s, up), head.lo.y),
                    Colors::dim, up);
    }

    // -- Seznam ------------------------------------------------------------
    const float sep_y = list.hi.y;
    const float row = L::Dims::row_h;
    float y = list.lo.y;
    const int n = (int)ps.banks.size();

    if (n == 0) {
        dl->AddText(Fonts::ui, px_u, ImVec2(r.lo.x, y), Colors::dimmer,
                    "no bank in this folder");
    }
    for (int i = 0; i < n && y + row <= sep_y - L::Dims::gap; ++i, y += row + 4.f) {
        const auto& e = ps.banks[(size_t)i];
        const bool loaded = (e.dir == ctx.state.bank_path);

        if (loaded) dl->AddRectFilled(ImVec2(r.lo.x, y), ImVec2(r.hi.x, y + row), Colors::inv_bg);
        else        dl->AddRect(ImVec2(r.lo.x, y), ImVec2(r.hi.x, y + row), Colors::line);
        dl->AddText(Fonts::ui, px_u, ImVec2(r.lo.x + 12.f, y + (row - px_u) * 0.5f),
                    loaded ? Colors::inv_fg : Colors::ink, e.name.c_str());

        ImGui::SetCursorScreenPos(ImVec2(r.lo.x, y));
        char id[32]; std::snprintf(id, sizeof(id), "##bank%d", i);
        if (ImGui::InvisibleButton(id, ImVec2(r.w(), row))) {
            if (looksLikeBank(e.dir)) {
                ctx.state.bank_path = e.dir;
                ctx.state.bank_search_dir = ps.browse_dir;
                ctx.requestBankReload(e.dir);
                ps.banks_valid = false;      // seznam muze mit jiny vyznam
            } else {
                ps.browse_dir = e.dir;       // jen slozka na ceste — jdi hloub
                ps.banks_valid = false;
            }
        }
    }

    // -- Fakta o NACTENE bance --------------------------------------------
    const float fy = fact_row.lo.y;
    dl->AddLine(ImVec2(r.lo.x, sep_y), ImVec2(r.hi.x, sep_y), Colors::line);

    const char* type = "\xE2\x80\x94";
    switch (ctx.engine.bankType()) {
        case BankFormat::FixedVelocity:   type = "FIXED";    break;
        case BankFormat::DynamicVelocity: type = "DYNAMIC";  break;
        case BankFormat::Extended:        type = "EXTENDED"; break;
        case BankFormat::PackedIthaca:    type = "PACKED";   break;
        default: break;
    }
    float x = r.lo.x;
    dl->AddText(Fonts::small, px_s, ImVec2(x, fy), Colors::dimmer, "TYPE");
    x += wdg::textW(Fonts::small, px_s, "TYPE") + 10.f;
    const ImVec2 fsz = wdg::invField(dl, ImVec2(x, fy - 3.f), Fonts::small, type);
    x += fsz.x + 16.f;

    char facts[64];
    std::snprintf(facts, sizeof(facts), "%d notes \xC2\xB7 %d samples",
                  ctx.engine.recordedNotes(), ctx.engine.loadedSamples());
    dl->AddText(Fonts::small, px_s, ImVec2(x, fy), Colors::dim, facts);

    // Varovani na tentyz radek vpravo — pod nim uz je akcni pas.
    if (ctx.bank_truncated_) {
        const char* w = "INCOMPLETE - bank exceeded RAM budget";
        dl->AddText(Fonts::small, px_s,
                    ImVec2(r.hi.x - wdg::textW(Fonts::small, px_s, w), fy),
                    Colors::warn, w);
    }

    // RELOAD dostava CELY radek u spodni hrany: je to jedina akce stranky,
    // a jako uzke tlacitko v rohu se na dotyku hleda hur nez pas pres celou sirku.
    ImGui::SetCursorScreenPos(btn.lo);
    if (wdg::button("##reload", "RELOAD", btn.w()) && !ctx.state.bank_path.empty())
        ctx.requestBankReload(ctx.state.bank_path);
}

} // namespace ithaca::gui
