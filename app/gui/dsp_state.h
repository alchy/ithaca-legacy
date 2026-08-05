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

namespace ithaca::gui {

// Chain -> state. Volano kazdy frame z render loopu, aby persistence videla
// aktualni hodnoty i po zmene primo v panelu.
inline void dspStateFromChain(GuiState& s, ithaca::dsp::DspChain& chain) {
    for (int i = 0; i < chain.stageCount(); ++i) {
        auto& page = chain.stage(i);
        DspStageState st;
        st.enabled = page.enabled();
        st.choice  = page.currentChoice();   // -1 kdyz stage volic nema
        for (int j = 0; j < page.paramCount(); ++j)
            st.params[page.param(j).id] = page.get(j);
        s.dsp[page.name()] = std::move(st);
    }
}

// State -> chain. Volano pri startu z persistovaneho stavu. Chybejici stage
// nebo parametr se preskoci (stage si nechá vlastni default), neznamy klic se
// ignoruje — state.json muze byt starsi nebo rucne editovany.
inline void applyDspStateToChain(const GuiState& s, ithaca::dsp::DspChain& chain) {
    for (int i = 0; i < chain.stageCount(); ++i) {
        auto& page = chain.stage(i);
        const auto it = s.dsp.find(page.name());
        if (it == s.dsp.end()) continue;
        const DspStageState& st = it->second;
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
}

} // namespace ithaca::gui
