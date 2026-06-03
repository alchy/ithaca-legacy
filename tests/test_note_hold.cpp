// tests/test_note_hold.cpp — per-pitch channel hold-mask.
// Root cause vypadku C v Synthesia: leva ruka (ch0) a prava ruka (ch1) si
// predavaji stejnou vysku; channel-blind engine nechal note-off jedne ruky
// zhasnout notu druhe. NoteHoldTracker drzi masku kanalu drzicich kazdou notu
// → release teprve kdyz pusti POSLEDNI kanal.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"
#include "midi/note_hold.h"

using namespace ithaca;

TEST_CASE("Prvni hold = first=true, posledni release = last=true") {
    NoteHoldTracker h;
    CHECK(h.noteOn(60, 0) == true);    // ch0 zapnul C → prvni drzitel
    CHECK(h.held(60));
    CHECK(h.noteOff(60, 0) == true);   // ch0 pustil → posledni → release
    CHECK_FALSE(h.held(60));
}

TEST_CASE("Cross-channel hand-off: off jednoho kanalu nezhasne notu druheho") {
    NoteHoldTracker h;
    CHECK(h.noteOn(60, 0) == true);    // leva ruka C
    CHECK(h.noteOn(60, 1) == false);   // prava ruka C — uz drzena → ne prvni
    CHECK(h.held(60));
    // leva pusti C, ale prava ji porad drzi → NESMI to byt posledni release
    CHECK(h.noteOff(60, 0) == false);
    CHECK(h.held(60));                 // C porad zni
    // prava pusti → ted teprve posledni → release
    CHECK(h.noteOff(60, 1) == true);
    CHECK_FALSE(h.held(60));
}

TEST_CASE("Off od kanalu ktery nedrzel = no-op (zadny falesny release)") {
    NoteHoldTracker h;
    h.noteOn(60, 0);
    CHECK(h.noteOff(60, 5) == false);  // ch5 nikdy C nedrzel → ne posledni
    CHECK(h.held(60));                 // ch0 porad drzi
    CHECK(h.noteOff(60, 0) == true);
}

TEST_CASE("Opakovany on stejneho kanalu je idempotentni (jeden off staci)") {
    NoteHoldTracker h;
    CHECK(h.noteOn(72, 2) == true);
    CHECK(h.noteOn(72, 2) == false);   // stejny kanal → uz drzi, ne prvni
    CHECK(h.noteOff(72, 2) == true);   // jeden off vyrovna
    CHECK_FALSE(h.held(72));
}

TEST_CASE("allNotesOff vycisti vsechny noty") {
    NoteHoldTracker h;
    h.noteOn(60,0); h.noteOn(64,1); h.noteOn(67,0);
    h.allNotesOff();
    CHECK_FALSE(h.held(60));
    CHECK_FALSE(h.held(64));
    CHECK_FALSE(h.held(67));
}

TEST_CASE("Hranice: neplatna nota/kanal se ignoruji bez padu") {
    NoteHoldTracker h;
    CHECK_FALSE(h.noteOn(-1, 0));
    CHECK_FALSE(h.noteOn(128, 0));
    CHECK_FALSE(h.noteOn(60, -1));
    CHECK_FALSE(h.noteOn(60, 16));
    CHECK_FALSE(h.held(60));
    CHECK_FALSE(h.noteOff(200, 0));
}
