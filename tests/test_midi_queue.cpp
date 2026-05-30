// tests/test_midi_queue.cpp
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "midi/midi_queue.h"

using namespace ithaca;

TEST_CASE("MidiQueue push/pop zachova poradi (FIFO)") {
    MidiQueue q;
    CHECK(q.push({MidiEvent::NoteOn, 60, 100}));
    CHECK(q.push({MidiEvent::NoteOff, 60, 0}));
    MidiEvent e;
    REQUIRE(q.pop(e)); CHECK(e.type == MidiEvent::NoteOn);  CHECK(e.data1 == 60); CHECK(e.data2 == 100);
    REQUIRE(q.pop(e)); CHECK(e.type == MidiEvent::NoteOff); CHECK(e.data1 == 60);
    CHECK_FALSE(q.pop(e));                                  // prazdna
}

TEST_CASE("MidiQueue drop-on-full nepretece") {
    MidiQueue q;
    int pushed = 0;
    for (int i = 0; i < 10000; ++i) if (q.push({MidiEvent::NoteOn, 60, 100})) pushed++;
    // Kapacita je omezena (MIDI_Q_SIZE); push nad ni vrati false, nespadne.
    CHECK(pushed > 0);
    CHECK(pushed < 10000);
}
