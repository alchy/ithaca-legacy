#pragma once
// engine/midi/midi_queue.h
// ------------------------
// Lock-free MPSC fronta MIDI udalosti: producenti = MIDI callback thread,
// GUI thread (allNotesOff pri reloadu) a CLI/main thread; konzument = audio
// thread. Vyukov bounded queue: CAS claim slotu + per-slot seq publish —
// soubezni producenti se nikdy neperou o tentyz slot (drive SPSC push =
// ztraceny event + torn write pri kolizi). Drop-on-full. Audio thread
// NIKDY neblokuje.

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

    MidiQueue() {
        for (size_t i = 0; i < MIDI_Q_SIZE; ++i)
            cells_[i].seq.store(i, std::memory_order_relaxed);
    }

    // Producent (MIDI/GUI/CLI thread — MULTI-producer safe). Slot se claimuje
    // CAS na w_, data se publikuji per-slot seq store (release). Vrati false
    // kdyz je fronta plna (drop). Bez zamku a alokaci.
    bool push(const MidiEvent& e) {
        size_t pos = w_.load(std::memory_order_relaxed);
        Cell* c;
        for (;;) {
            c = &cells_[pos % MIDI_Q_SIZE];
            const size_t   seq = c->seq.load(std::memory_order_acquire);
            const intptr_t dif = (intptr_t)seq - (intptr_t)pos;
            if (dif == 0) {
                if (w_.compare_exchange_weak(pos, pos + 1,
                                             std::memory_order_relaxed))
                    break;                       // slot claimnut
            } else if (dif < 0) {
                return false;                    // plna → drop
            } else {
                pos = w_.load(std::memory_order_relaxed);
            }
        }
        c->e = e;
        c->seq.store(pos + 1, std::memory_order_release);   // publish
        return true;
    }

    // Konzument (JEDINY — audio thread). Vrati false kdyz je prazdna NEBO
    // slot jeste neni publikovan (probihajici push → event dorazi pristim pop).
    bool pop(MidiEvent& out) {
        const size_t pos = r_.load(std::memory_order_relaxed);
        Cell* c = &cells_[pos % MIDI_Q_SIZE];
        const size_t seq = c->seq.load(std::memory_order_acquire);
        if ((intptr_t)seq - (intptr_t)(pos + 1) < 0) return false;
        out = c->e;
        c->seq.store(pos + MIDI_Q_SIZE, std::memory_order_release);
        r_.store(pos + 1, std::memory_order_relaxed);
        return true;
    }

private:
    struct Cell {
        std::atomic<size_t> seq{0};
        MidiEvent           e;
    };
    Cell                cells_[MIDI_Q_SIZE];
    std::atomic<size_t> w_{0};
    std::atomic<size_t> r_{0};
};

} // namespace ithaca
