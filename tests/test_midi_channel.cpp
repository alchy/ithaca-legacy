// tests/test_midi_channel.cpp — channel filter accept logika.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"
#include "midi/midi_input.h"

using namespace ithaca;

TEST_CASE("OMNI (-1) prijima vsechny kanaly") {
    CHECK(MidiInput::channelAccepts(-1, 0x90)); // ch1 note-on
    CHECK(MidiInput::channelAccepts(-1, 0x9F)); // ch16 note-on
    CHECK(MidiInput::channelAccepts(-1, 0xB3)); // ch4 CC
}

TEST_CASE("Konkretni kanal prijima jen svuj") {
    // channel_ je 0-based interne: 0 = MIDI kanal 1.
    CHECK(MidiInput::channelAccepts(0, 0x90));        // ch1 note-on → pass
    CHECK_FALSE(MidiInput::channelAccepts(0, 0x91));  // ch2 → reject
    CHECK(MidiInput::channelAccepts(3, 0xB3));        // ch4 CC → pass
    CHECK_FALSE(MidiInput::channelAccepts(3, 0xB4));  // ch5 → reject
}

// -- Maska kanalu -----------------------------------------------------------
// OMNI neni zvlastni rezim, je to maska se vsemi bity. Diky tomu jde nastavit
// i libovolna podmnozina, coz jediny index neumel.

TEST_CASE("maskAccepts: plna maska propusti vsechno") {
    using ithaca::MidiInput;
    for (int ch = 0; ch < 16; ++ch)
        CHECK(MidiInput::maskAccepts(0xFFFF, (uint8_t)(0x90 | ch)));
}

TEST_CASE("maskAccepts: prazdna maska nepropusti nic") {
    using ithaca::MidiInput;
    for (int ch = 0; ch < 16; ++ch)
        CHECK_FALSE(MidiInput::maskAccepts(0x0000, (uint8_t)(0x90 | ch)));
}

TEST_CASE("maskAccepts: podmnozina kanalu") {
    using ithaca::MidiInput;
    const uint16_t m = (1u << 0) | (1u << 2);      // kanaly 1 a 3
    CHECK(MidiInput::maskAccepts(m, 0x90));        // ch1 note-on
    CHECK_FALSE(MidiInput::maskAccepts(m, 0x91));  // ch2
    CHECK(MidiInput::maskAccepts(m, 0xB2));        // ch3 CC
    CHECK_FALSE(MidiInput::maskAccepts(m, 0x9F));  // ch16
}

TEST_CASE("setChannel mapuje stary index na masku") {
    ithaca::MidiInput mi;
    mi.setChannel(-1);
    CHECK(mi.channelMask() == 0xFFFF);
    mi.setChannel(4);
    CHECK(mi.channelMask() == (1u << 4));
    mi.setChannel(99);                 // mimo rozsah → OMNI, ne nahodny bit
    CHECK(mi.channelMask() == 0xFFFF);
}
