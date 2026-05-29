#pragma once
// engine/sample/sample_types.h
// ----------------------------
// Datovy model banky. Hierarchie (spec 3.2):
//   Bank → NoteSlots[128] → VelocitySlot → SampleAsset → MicLayer
// Dve nezavisle osy: VelocitySlot drzi round-robin varianty, SampleAsset drzi
// mic perspektivy. Faze 2 je all-in-RAM (MicLayer.data = cely sampl); preload/
// disk rozdeleni a round-robin/multi-mic naplno prijdou v dalsich fazich.

#include <cstddef>
#include <string>
#include <vector>

namespace ithaca {

enum class BankFormat { Unknown, Legacy, Extended };

inline const char* bankFormatName(BankFormat f) {
    switch (f) {
        case BankFormat::Legacy:   return "legacy";
        case BankFormat::Extended: return "extended";
        case BankFormat::Unknown:  return "unknown";
    }
    return "unknown";
}

// Jedna mic perspektiva jednoho uhozu. Faze 2: cela data v RAM.
struct MicLayer {
    std::string        mic_name;     // legacy: "stereo"; extended: "front"/"soundboard"
    std::vector<float> data;         // interleaved stereo [L,R,...]
    int                frames = 0;
    int                sample_rate = 0;
};

// Jeden uhoz: 1..N mic perspektiv hranych synchronne. Legacy: 1 (stereo).
struct SampleAsset {
    std::vector<MicLayer> mics;
    float peak_rms_db     = 0.f;     // zmereno z referencni (prvni/front) perspektivy
    int   attack_end_frame = 0;      // hranice attack/sustain (pro resonance skip-attack)
};

// Jeden velocity slot: 1..N round-robin variant stejne dynamiky. Legacy: 1.
struct VelocitySlot {
    std::vector<SampleAsset> variants;
    float rms_db = 0.f;              // reprezentativni RMS slotu (pro velocity krivku)
};

// Vsechny sloty jedne MIDI noty, serazene VZESTUPNE podle rms_db (nejtissi → nejhlasitejsi).
struct NoteSlots {
    std::vector<VelocitySlot> slots;
    bool recorded = false;          // true = mame realny sampl pro tuto MIDI notu
};

// Cela banka v pameti.
struct Bank {
    std::string name;
    std::string path;
    BankFormat  format = BankFormat::Unknown;
    NoteSlots   notes[128];
    // diagnostika
    size_t total_frames = 0;        // soucet frames vsech nactenych mic layeru
    size_t total_bytes  = 0;        // odhad RAM (data.size()*sizeof(float))
    int    loaded_samples = 0;      // pocet uspesne nactenych SampleAssetu
};

} // namespace ithaca
