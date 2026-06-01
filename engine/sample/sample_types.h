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

enum class BankFormat { Unknown, FixedVelocity, Extended, DynamicVelocity };

inline const char* bankFormatName(BankFormat f) {
    switch (f) {
        case BankFormat::FixedVelocity:   return "fixed-velocity";
        case BankFormat::Extended:        return "extended";
        case BankFormat::DynamicVelocity: return "dynamic-velocity";
        case BankFormat::Unknown:         return "unknown";
    }
    return "unknown";
}

// Rezim, jak je MicLayer drzen v pameti:
// - FullyLoaded: kratky sampl, vejde se cely do preload_head (zadny streaming).
// - Streamed:    dlouhy sampl, preload_head ma jen zacatek, zbytek se streamuje
//                z `file` cestou ring bufferu.
enum class MicLayerMode { FullyLoaded, Streamed };

// Reference na zdrojovy WAV — drzi cestu a klicove metadata.
struct SampleFile {
    std::string path;
    int  frames      = 0;     // celkovy pocet frames v souboru
    int  sample_rate = 0;
    bool valid       = false;
};

// Jedna mic perspektiva jednoho uhozu. Faze 4: preload jen zacatek (head) +
// rezonancni okno (od peak RMS pozice) v RAM; zbytek lezi v souboru a streamuje
// se. Kratky sampl drzi cely v preload_head a `mode = FullyLoaded`.
struct MicLayer {
    std::string mic_name;            // legacy: "stereo"; extended: "main"/"micpos-A"/...
    SampleFile  file;                // odkaz na zdrojovy soubor
    MicLayerMode mode = MicLayerMode::FullyLoaded;

    // Preload region "hlava": [0..head_frames). VZDY pritomny (i pro kratke,
    // ktere ho maji = celemu samplu).
    std::vector<float> preload_head;     // interleaved stereo
    int head_frames = 0;

    // Preload region "rezonance": [resonance_start_frame..+resonance_frames).
    // Pouziva se az ve fazi 5 (rezonancni hlasy). Ve fazi 4 muze byt prazdny
    // pokud peak_RMS lezi v preload_head (jiz pokryto) nebo pro kratke samply.
    std::vector<float> preload_resonance;
    int resonance_start_frame = 0;
    int resonance_frames      = 0;
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
    size_t resident_frames = 0;     // frames rezidentni v RAM (head + resonance regiony)
    size_t total_bytes  = 0;        // odhad RAM (data.size()*sizeof(float))
    int    loaded_samples = 0;      // pocet uspesne nactenych SampleAssetu
};

} // namespace ithaca
