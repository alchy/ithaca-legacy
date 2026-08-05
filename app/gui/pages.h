#pragma once
// app/gui/pages.h — stranky rozhrani.
// ----------------------------------------------------------------------------
// Plocha struktura sedmi stranek prepinanych hornim radkem zalozek. Zvyraznena
// zalozka je zaroven nadpis stranky, proto zadna stranka nema vlastni hlavicku.
//
// Kazda render funkce dostane obdelnik, ve kterem smi kreslit — shell uz
// odectl ramecek, zalozky, radek kontrolek i paticku.
#include "imgui.h"

namespace ithaca::gui {
// Obdelnik v obrazovkovych souradnicich. Vlastni typ, at nemusime tahnout
// imgui_internal.h (ImRect je interni API).
struct Rect {
    ImVec2 lo, hi;
    float w() const { return hi.x - lo.x; }
    float h() const { return hi.y - lo.y; }
};
}

namespace ithaca::dsp { struct IParamPage; }

namespace ithaca::gui {

struct AppContext;

enum Page { PAGE_PLAY = 0, PAGE_BANK, PAGE_TONE, PAGE_RESO, PAGE_DSP, PAGE_SYS, PAGE_LOG,
            PAGE_COUNT };

// Shell: ramecek, zalozky, dispatch stranky, radek kontrolek, paticka.
// Vola se jednou za frame z main().
void renderScreen(AppContext& ctx, ithaca::dsp::IParamPage** pages, int n_pages);

// Jednotlive stranky. `r` je oblast, do ktere smi stranka kreslit (obrazovkove
// souradnice: r.lo = levy horni roh, r.hi = pravy dolni).
// Zajisti, ze je nactena nabidka bank (a vybrana ta aktualni). Vola shell pred
// dispatchem — vytah v PLAY ji potrebuje stejne jako stranka BANK, a PLAY je
// vychozi stranka, takze cekat na navstevu BANKu nejde.
void ensureBankList(AppContext& ctx);

void pagePlay (AppContext& ctx, const Rect& r);
void pageBank (AppContext& ctx, const Rect& r);
void pageSys  (AppContext& ctx, const Rect& r);
void pageLog  (AppContext& ctx, const Rect& r);

// Genericky renderer libovolne IParamPage — slouzi strankam TONE, RESO
// i vsem ctyrem DSP stage. Nezna konkretni parametry, jede pres paramCount()
// a Param tabulky, takze novy parametr se objevi sam.
void pageParams(AppContext& ctx, const Rect& r, ithaca::dsp::IParamPage& page);

// DSP: druhy radek zalozek (CONVOLVER/AGC/ENHANCER/LIMITER) + parametry vybrane.
void pageDsp(AppContext& ctx, const Rect& r, ithaca::dsp::IParamPage** stages, int n);

} // namespace ithaca::gui
