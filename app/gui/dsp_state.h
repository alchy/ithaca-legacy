#pragma once
// app/gui/dsp_state.h — most mezi DspChain a GuiState::dsp.
// ----------------------------------------------------------------------------
// Obe funkce jsou GENERICKE: jedou pres stageCount()/paramCount() a klicuji
// parametry pres Param::id. Diky tomu pridani parametru (nebo cele stage) do
// chainu nevyzaduje zadnou zmenu v GUI ani v persistenci. Drive byl kazdy
// parametr vypsany rucne v initFromState i v zrcadleni render loopu a pri
// pridani se na nej snadno zapomnelo (tise se neulozil / neaplikoval).
#include "persistence.h"
#include "dsp/dsp_chain.h"

#include <map>
#include <string>

namespace ithaca::gui {

// -- Jedna stranka ----------------------------------------------------------
// Zaklad vseho ostatniho tady. Pracuje s IParamPage, ne s DspStage, takze
// funguje i na MASTER a RESONANCE — ty zadne DSP nejsou, ale rozhrani sdili.

inline DspStageState snapshotPage(const ithaca::dsp::IParamPage& page) {
    DspStageState st;
    st.enabled = page.enabled();
    st.choice  = page.currentChoice();   // -1 kdyz stranka volic nema
    for (int j = 0; j < page.paramCount(); ++j)
        st.params[page.param(j).id] = page.get(j);
    return st;
}

inline void applyToPage(const DspStageState& st, ithaca::dsp::IParamPage& page) {
    for (int j = 0; j < page.paramCount(); ++j) {
        const auto pv = st.params.find(page.param(j).id);
        if (pv != st.params.end()) page.set(j, pv->second);
    }
    // Volic az po parametrech: selectChoice u Convolveru prestavuje IR,
    // ktere z parametru (decay/tone/size) vychazi.
    if (st.choice >= 0 && st.choice < page.choiceCount())
        page.selectChoice(st.choice);
    page.setEnabled(st.enabled);
}

// -- Pole stranek (GUI: MASTER, RESONANCE + vsechny DSP stage) --------------
// Pouziva SAVE AS DEFAULT / RESET PARAMS na strance SYS. Klicem je name().

inline void snapshotPages(std::map<std::string, DspStageState>& out,
                          ithaca::dsp::IParamPage* const* pages, int n) {
    for (int i = 0; i < n; ++i)
        if (pages[i]) out[pages[i]->name()] = snapshotPage(*pages[i]);
}

inline void applyPagesState(const std::map<std::string, DspStageState>& in,
                            ithaca::dsp::IParamPage* const* pages, int n) {
    for (int i = 0; i < n; ++i) {
        if (!pages[i]) continue;
        const auto it = in.find(pages[i]->name());
        if (it != in.end()) applyToPage(it->second, *pages[i]);
    }
}

// -- Porovnani (stitek v paticce) -------------------------------------------
// Jede nastroj na tovarnim profilu, nebo uz na uzivatelskem? USER profil se
// uklada sam pri ukonceni, takze staci porovnat s tovarnim: cokoli jineho uz
// je uzivatelovo nastaveni.
//
// Hodnoty se porovnavaji s toleranci: projdou float konverzi a u nekterych
// stranek jeste clampem do rozsahu odvozeneho z banky.
inline bool nearlyEq(float a, float b) {
    const float d = a - b;
    const float m = (a < 0.f ? -a : a);
    return (d < 0.f ? -d : d) <= 1e-3f * (m > 1.f ? m : 1.f);
}

// Jen HODNOTY parametru proti Param::def. Enabled a volic se neporovnavaji:
// tovarni stav prepinacu neni v Param tabulce a resetToDefaults ho u DSP
// stage nechava byt.
inline bool pagesAreFactory(ithaca::dsp::IParamPage* const* pages, int n) {
    for (int i = 0; i < n; ++i) {
        if (!pages[i]) continue;
        for (int j = 0; j < pages[i]->paramCount(); ++j)
            if (!nearlyEq(pages[i]->param(j).def, pages[i]->get(j))) return false;
    }
    return true;
}

// -- Cely chain -------------------------------------------------------------

// Chain -> state. Volano kazdy frame z render loopu, aby persistence videla
// aktualni hodnoty i po zmene primo v panelu.
inline void dspStateFromChain(GuiState& s, ithaca::dsp::DspChain& chain) {
    for (int i = 0; i < chain.stageCount(); ++i) {
        auto& page = chain.stage(i);
        s.dsp[page.name()] = snapshotPage(page);
    }
}

// State -> chain. Volano pri startu z persistovaneho stavu. Chybejici stage
// nebo parametr se preskoci (stage si nechá vlastni default), neznamy klic se
// ignoruje — state.json muze byt starsi nebo rucne editovany.
inline void applyDspStateToChain(const GuiState& s, ithaca::dsp::DspChain& chain) {
    for (int i = 0; i < chain.stageCount(); ++i) {
        auto& page = chain.stage(i);
        const auto it = s.dsp.find(page.name());
        if (it != s.dsp.end()) applyToPage(it->second, page);
    }
}

} // namespace ithaca::gui
