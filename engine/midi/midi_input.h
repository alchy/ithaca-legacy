#pragma once
// engine/midi/midi_input.h
// ------------------------
// Tenky RtMidi wrapper. Otevre MIDI vstupni port a RtMidi callback (bezi na
// vlastnim threadu) prekladá MIDI zpravy → Engine API:
//   0x90 NoteOn (vel>0)  → engine.noteOn(midi, vel)
//   0x90 NoteOn (vel=0)  → engine.noteOff(midi)   (MIDI konvence)
//   0x80 NoteOff         → engine.noteOff(midi)
//   0xB0 CC 64 Sustain   → engine.sustainPedal(cc) (spojite 0..127, half-pedal)
//   0xB0 CC 123 AllOff   → engine.allNotesOff()
//   ostatni              → ignore (pridavat dle potreby)
//
// Engine API uz frontuje vse pres lock-free MidiQueue, takze callback z RtMidi
// threadu nemusi resit thread-safety. listPorts() vraci dostupne porty.

#include <cstdint>
#include <string>
#include <vector>

class RtMidiIn;

namespace ithaca {

class Engine;

class MidiInput {
public:
    MidiInput() = default;
    ~MidiInput() { close(); }
    MidiInput(const MidiInput&)            = delete;
    MidiInput& operator=(const MidiInput&) = delete;

    // Statika: vypsat dostupne MIDI vstupni porty (pro --midi-list).
    static std::vector<std::string> listPorts();

    // Channel filtr: -1 = OMNI (vse), 0..15 = jen ten MIDI kanal (0-based).
    void setChannel(int ch) { channel_ = (ch < 0 || ch > 15) ? -1 : ch; }
    int  channel() const { return channel_; }
    // Cista testovatelna logika: prijmout zpravu se status bytem `status`
    // pri zvolenem `channel` (-1 OMNI)? Channel = status & 0x0F.
    static bool channelAccepts(int channel, uint8_t status) {
        if (channel < 0) return true;
        return (int)(status & 0x0F) == channel;
    }

    // Otevre port `index` a smeruje udalosti do `engine`. Vrati false pri chybe
    // (zadne porty / index mimo rozsah / RtMidi exception).
    bool open(Engine& engine, int port_index = 0);

    // Otevre virtualni port (macOS / Linux) — umoznuje DAW posilat MIDI dovnitr.
    bool openVirtual(Engine& engine, const std::string& name = "ithaca-cli");

    void close();
    bool isOpen() const { return midi_ != nullptr; }
    const std::string& portName() const { return port_name_; }

private:
    static void callback(double timestamp,
                         std::vector<unsigned char>* msg,
                         void* user_data);

    RtMidiIn*   midi_   = nullptr;
    Engine*     engine_ = nullptr;
    std::string port_name_;
    int channel_ = -1;  // OMNI default
};

} // namespace ithaca
