#pragma once
// engine/midi/midi_queue.h
// ------------------------
// Lock-free SPSC fronta MIDI udalosti: producent = MIDI/GUI thread, konzument
// = audio thread. Stejny princip jako RT logger (publish write indexu
// release/acquire, drop-on-full). Audio thread NIKDY neblokuje.

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace ithaca {

struct MidiEvent {
    enum Type : uint8_t { NoteOn, NoteOff, Sustain, AllNotesOff };
    Type    type    = NoteOn;
    uint8_t data1   = 0;   // midi nota (NoteOn/Off) nebo hodnota (Sustain)
    uint8_t data2   = 0;   // velocity (NoteOn)
    uint8_t channel = 0;   // MIDI kanal 0..15 (NoteOn/Off; cross-channel hold)
};

class MidiQueue {
public:
    static constexpr int MIDI_Q_SIZE = 1024;

    // Producent (MIDI/GUI thread). Vrati false kdyz je fronta plna (drop).
    bool push(const MidiEvent& e) {
        const size_t w = w_.load(std::memory_order_relaxed);
        const size_t r = r_.load(std::memory_order_acquire);
        if (w - r >= MIDI_Q_SIZE) return false;          // plna → drop
        buf_[w % MIDI_Q_SIZE] = e;
        w_.store(w + 1, std::memory_order_release);
        return true;
    }

    // Konzument (audio thread). Vrati false kdyz je fronta prazdna.
    bool pop(MidiEvent& out) {
        const size_t r = r_.load(std::memory_order_relaxed);
        const size_t w = w_.load(std::memory_order_acquire);
        if (r >= w) return false;
        out = buf_[r % MIDI_Q_SIZE];
        r_.store(r + 1, std::memory_order_release);
        return true;
    }

private:
    MidiEvent           buf_[MIDI_Q_SIZE];
    std::atomic<size_t> w_{0};
    std::atomic<size_t> r_{0};
};

} // namespace ithaca
