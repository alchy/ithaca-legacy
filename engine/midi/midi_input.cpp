// engine/midi/midi_input.cpp — viz midi_input.h.
// Adaptovano z icr engine/midi_input.{h,cpp}, zjednoduseno: zadne MIDI out,
// zadne SysEx, zadne CC mimo sustain a all-notes-off (extra CC pridame v
// budouci fazi 6/8 podle potreby).
#include "midi/midi_input.h"

#include "engine.h"
#include "util/log.h"

#include "RtMidi.h"   // vendored, include path third-party/rtmidi

#include <cstdint>

namespace ithaca {

std::vector<std::string> MidiInput::listPorts() {
    std::vector<std::string> names;
    try {
        RtMidiIn midi;
        unsigned int n = midi.getPortCount();
        for (unsigned int i = 0; i < n; ++i)
            names.push_back(midi.getPortName(i));
    } catch (...) {
        // RtMidi muze hodit pri inicializaci; pri selhani vratime co mame.
    }
    return names;
}

bool MidiInput::open(Engine& engine, int port_index) {
    close();
    auto& L = log::Logger::default_();
    try {
        midi_   = new RtMidiIn();
        engine_ = &engine;

        unsigned int n = midi_->getPortCount();
        if (n == 0) {
            L.log("midi", log::Severity::Warning, "Zadne MIDI porty nenalezeny");
            delete midi_; midi_ = nullptr;
            return false;
        }
        int idx = (port_index < 0 || port_index >= (int)n) ? 0 : port_index;
        port_name_ = midi_->getPortName(idx);
        midi_->openPort(idx);
        // accept sysex, ignore timing + active sensing (irelevantni pro samply).
        midi_->ignoreTypes(false, true, true);
        midi_->setCallback(&MidiInput::callback, this);
        L.log("midi", log::Severity::Info, "Otevren port: %s", port_name_.c_str());
        return true;
    } catch (RtMidiError& e) {
        L.log("midi", log::Severity::Error, "RtMidi open: %s", e.getMessage().c_str());
        delete midi_; midi_ = nullptr;
        return false;
    }
}

bool MidiInput::openVirtual(Engine& engine, const std::string& name) {
    close();
    auto& L = log::Logger::default_();
    try {
        midi_   = new RtMidiIn();
        engine_ = &engine;
        midi_->openVirtualPort(name);
        midi_->ignoreTypes(false, true, true);
        midi_->setCallback(&MidiInput::callback, this);
        port_name_ = name + " (virtual)";
        L.log("midi", log::Severity::Info, "Virtualni port: %s", name.c_str());
        return true;
    } catch (RtMidiError& e) {
        L.log("midi", log::Severity::Error, "RtMidi virtual: %s", e.getMessage().c_str());
        delete midi_; midi_ = nullptr;
        return false;
    }
}

void MidiInput::close() {
    if (midi_) {
        if (midi_->isPortOpen()) midi_->closePort();
        delete midi_;
        midi_ = nullptr;
    }
    engine_ = nullptr;
    port_name_.clear();
}

// Callback bezi na RtMidi threadu. Engine API frontuje vse pres lock-free
// MidiQueue, takze tady jen prelozime status byte na metodu fasady.
void MidiInput::callback(double ts,
                         std::vector<unsigned char>* msg,
                         void* user_data) {
    if (!msg || msg->size() < 2) return;
    auto* self = reinterpret_cast<MidiInput*>(user_data);
    if (!self || !self->engine_) return;

    const uint8_t status = (*msg)[0];
    const uint8_t data1  = (*msg)[1];
    const uint8_t data2  = (msg->size() > 2) ? (*msg)[2] : 0;
    const uint8_t type   = status & 0xF0;

    // DIAG: log kazdou prichozi MIDI udalost JESTE PRED channel filtrem — at
    // vidime, jestli nota vubec dorazi z hardwaru a na jakem kanale (ts = RtMidi
    // delta-cas od minule udalosti v sekundach).
    if (type == 0x90 || type == 0x80) {
        log::Logger::default_().log("midi_in", log::Severity::Debug,
            "RX %s midi=%d vel=%d ch=%d dt=%.4f%s",
            type == 0x90 ? "NoteOn " : "NoteOff", (int)data1, (int)data2,
            (int)(status & 0x0F), ts,
            channelAccepts(self->channel_, status) ? "" : " [FILTERED-OUT]");
    }

    if (!channelAccepts(self->channel_, status)) return;

    switch (type) {
        case 0x90:  // Note On (vel=0 == Note Off dle MIDI konvence)
            if (data2 > 0) self->engine_->noteOn((int)data1, (int)data2);
            else           self->engine_->noteOff((int)data1);
            break;
        case 0x80:  // Note Off
            self->engine_->noteOff((int)data1);
            break;
        case 0xB0:  // Control Change
            switch (data1) {
                case 64:  // Sustain pedal (spojite 0..127)
                    self->engine_->sustainPedal(data2);
                    break;
                case 120: // All Sound Off
                case 123: // All Notes Off
                    self->engine_->allNotesOff();
                    break;
                default:
                    break;
            }
            break;
        default:
            break;
    }
}

} // namespace ithaca
