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

#include <atomic>
#include <thread>

TEST_CASE("MPSC: dva producenti soubezne — zadny event ztracen ani poskozen") {
    MidiQueue q;
    constexpr int N = 20000;
    std::atomic<bool> done_a{false}, done_b{false};
    std::thread ta([&] {
        for (int i = 0; i < N; ++i) {
            MidiEvent e{MidiEvent::NoteOn, (uint8_t)(i & 127), 1, 0};
            while (!q.push(e)) std::this_thread::yield();
        }
        done_a = true;
    });
    std::thread tb([&] {
        for (int i = 0; i < N; ++i) {
            MidiEvent e{MidiEvent::NoteOff, (uint8_t)(i & 127), 2, 1};
            while (!q.push(e)) std::this_thread::yield();
        }
        done_b = true;
    });
    int got_a = 0, got_b = 0, exp_a = 0, exp_b = 0;
    bool ok_order = true, ok_payload = true;
    while (!done_a.load() || !done_b.load() || got_a + got_b < 2 * N) {
        MidiEvent e;
        if (!q.pop(e)) { std::this_thread::yield(); continue; }
        if (e.type == MidiEvent::NoteOn) {
            ok_payload = ok_payload && e.data2 == 1 && e.channel == 0;
            ok_order   = ok_order && (int)e.data1 == (exp_a & 127);
            ++exp_a; ++got_a;
        } else {
            ok_payload = ok_payload && e.data2 == 2 && e.channel == 1;
            ok_order   = ok_order && (int)e.data1 == (exp_b & 127);
            ++exp_b; ++got_b;
        }
    }
    ta.join(); tb.join();
    CHECK(got_a == N);
    CHECK(got_b == N);
    CHECK(ok_order);     // per-producer FIFO
    CHECK(ok_payload);   // zadny torn event
}
