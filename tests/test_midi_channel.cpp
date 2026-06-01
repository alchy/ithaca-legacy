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
