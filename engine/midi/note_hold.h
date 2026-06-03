#pragma once
// engine/midi/note_hold.h
// -----------------------
// Per-pitch channel hold-mask. Skutecny klavir ma na vysku JEDEN tlumitko —
// spadne az kdyz POSLEDNI klavesa te vysky je pustena. MIDI s vice kanaly
// (napr. Synthesia: leva ruka ch0, prava ruka ch1) muze stejnou vysku drzet
// vic kanaly soucasne / predavat si ji. Channel-blind engine nechal note-off
// jednoho kanalu zhasnout notu druheho → vypadek noty (typicky C).
//
// NoteHoldTracker drzi pro kazdou ze 128 not 16-bitovou masku kanalu, ktere ji
// prave drzi. noteOn vrati, jestli je to PRVNI drzitel (→ zapni tlumitko/voice
// jako "key down"), noteOff vrati jestli to byl POSLEDNI drzitel (→ teprve ted
// release). Drzeno a aktualizovano vyhradne na audio threadu pri drainu MIDI
// fronty, takze bez atomic / zamku.

#include <cstdint>

namespace ithaca {

class NoteHoldTracker {
public:
    // Zaznamena ze `ch` (0..15) drzi `note` (0..127). Vrati true, pokud pred
    // tim notu nedrzel zadny kanal (= prvni drzitel → key-down transition).
    bool noteOn(int note, int ch) {
        if (!valid(note, ch)) return false;
        const uint16_t bit = (uint16_t)(1u << ch);
        const bool first = held_[note] == 0;
        held_[note] |= bit;
        return first;
    }

    // Zaznamena ze `ch` notu pustil. Vrati true, pokud uz ji nedrzi zadny jiny
    // kanal (= posledni drzitel → teprve ted skutecny release). Off od kanalu
    // ktery notu nedrzel je no-op a vrati false (zadny falesny release).
    bool noteOff(int note, int ch) {
        if (!valid(note, ch)) return false;
        const uint16_t bit = (uint16_t)(1u << ch);
        if ((held_[note] & bit) == 0) return false;   // nedrzel → no-op
        held_[note] &= (uint16_t)~bit;
        return held_[note] == 0;                        // nikdo dalsi → release
    }

    // Drzi notu aspon jeden kanal?
    bool held(int note) const {
        return note >= 0 && note < 128 && held_[note] != 0;
    }

    // Panika / reload: vsechny noty rozdrzeny.
    void allNotesOff() {
        for (int i = 0; i < 128; ++i) held_[i] = 0;
    }

private:
    static bool valid(int note, int ch) {
        return note >= 0 && note < 128 && ch >= 0 && ch < 16;
    }
    uint16_t held_[128] = {0};   // bit c = kanal c drzi tuto notu
};

} // namespace ithaca
