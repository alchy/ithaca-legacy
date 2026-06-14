#pragma once
// engine/stream/stream_engine.h
// -----------------------------
// Background streamovaci engine: pool ring bufferu + worker thread, ktery
// plni ringy z WAV souboru. Audio thread cte vyhradne z RAM (ringu); disk
// I/O se nikdy nedeje na audio threadu.
//
// SPSC kontrakt:
//   - kazdy RingHandle je SPSC kanal: producent = worker thread, konzument =
//     prave JEDEN Voice (audio thread). Allocator pool slotu (acquire/release)
//     je lock-free pres atomic flag in_use_ na kazdem slotu.
//   - StreamRequestQueue je SPSC: producent = audio thread (Voice posila
//     refill pozadavky), konzument = worker thread. Plna fronta = drop-on-full
//     (RT-safe; pri vytrvalem dropu Voice underrune a fadne do ticha).
//
// Vzor lock-free SPSC patternu je engine/midi/midi_queue.h — stejne pouziti
// release/acquire memory orderingu a drop-on-full pri plnem bufferu.

#include "sample/sample_types.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace ithaca {

// SPSC ring na interleaved stereo float (jedna "rourka" pro jeden voice).
struct RingHandle {
    // Interleaved stereo float; velikost vektoru = capacity_frames * 2
    // (alokovano jednou pri StreamEngine ctoru).
    std::vector<float>  buf;
    int                 capacity_frames = 0;

    // SPSC kursory. POZN.: w_ - r_ vraci pocet platnych frames v ringu
    // (modulo dela az %= capacity_frames pri pristupu do buf).
    std::atomic<size_t> w_{0};
    std::atomic<size_t> r_{0};

    // Worker po dokonceni posledniho streamovaciho chunku (kdyz frame_off
    // dosahl konce souboru) nastavi eof_=true. Voice ho cte jen diagnosticky
    // (do logu) — cisty konec vs. underrun rozlisuje pres file_request_off_
    // a OBA dozni stejnym 5ms fade (cisty konec = Info, underrun = Warning).
    // ResonanceVoice eof_ cte (acquire) pro hold-last-sample / konec hlasu.
    std::atomic<bool>   eof_{false};

    // Allocator flag (acquireRing / releaseRing).
    std::atomic<bool>   in_use_{false};

    // Generace vlastnictvi: inkrement pri kazdem releaseRing. StreamRequest
    // nese snapshot — worker pred zapisem overi shodu, takze pozdni/stale
    // request nikdy nezapise data/EOF do ringu noveho vlastnika (ABA guard
    // pro steal/retrigger streamovaneho hlasu s in-flight requestem).
    std::atomic<uint32_t> gen_{0};

    // Producer try-lock (0/1): per-ring smi zapisovat jen jeden worker a
    // acquireRing na nej kratce pocka pred resetem kurzoru. Drzi se JEN pres
    // push (memcpy), ne pres disk I/O → ceka se max ~desitky us.
    std::atomic<int>    producers_{0};

    // Producent (worker): zapise az n_frames stereo frames. Vrati pocet
    // skutecne zapsanych (kdyz se ring zaplni). Bez alokaci.
    int push(const float* src_interleaved, int n_frames);

    // Konzument (Voice, audio thread): pop 1 stereo frame. Vrati false kdyz
    // je ring prazdny.
    bool popFrame(float& L, float& R) noexcept;

    // Bulk cteni pro StreamedSampleReader: konzument si 1x za blok snapshotne
    // w_ (acquire), cte lokalnim kurzorem a 1x za blok commitne r_ (release)
    // — misto 3 atomickych operaci na KAZDY frame. Hodnoty identicke s
    // popFrame (bit-exact); popFrame zustava pro testy a popInto.
    size_t snapshotW() const noexcept { return w_.load(std::memory_order_acquire); }
    size_t cursorR()   const noexcept { return r_.load(std::memory_order_relaxed); }
    void   commitR(size_t r) noexcept { r_.store(r, std::memory_order_release); }

    // Konzument: kolik frames je k dispozici k cteni (atomic snapshot).
    int available() const noexcept;

    // Reset cursors pro recyklaci po releaseRing → dalsi acquireRing.
    // Vola se z workeru (po dokonceni) NEBO z release strany kdyz worker
    // nema rozdelanou praci. Bezpecne pouze kdyz nejsou aktivni operace.
    void resetForReuse() noexcept;
};

// Pozadavek na nacteni z disku do konkretniho ringu.
struct StreamRequest {
    RingHandle* ring          = nullptr;
    // Zdroj dat: SampleFile lokator (WAV cesta NEBO packed blob). Kopiruje se
    // pri push: string copy (SBO pro kratke cesty) + shared_ptr refcount
    // (atomic inc, bez zamku/alokace) — stejna RT uvaha jako drive u path.
    // shared_ptr drzi blob handle nazivu i kdyz se banka mezitim vymeni
    // (worker docte ze stareho handle; gen guard data zahodi).
    SampleFile  file;
    int64_t     frame_off     = 0;
    int64_t     n_frames      = 0;
    bool        eof_when_done = false;   // worker po dokonceni nastavi ring->eof_
    uint32_t    gen           = 0;       // snapshot RingHandle::gen_ pri odeslani
};

// SP-MC fronta StreamRequestu. Producent (audio thread) je single lock-free;
// konzumenti (worker threads) serializuji pop pres mutex (rychle, nesahaji
// na audio thread). Vzor pro single-producer push: engine/midi/midi_queue.h.
//
// POZN.: predtim byla SPSC s jednim workerem. S vice workery byla SPSC pop
// race-conditional. Misto MPMC lock-free struktury volime jednoduchy mutex
// na pop side; audio thread (push) zustava lock-free a non-blocking.
class StreamRequestQueue {
public:
    static constexpr int kCap = 256;

    // Producent (audio thread): push kopirovanim. Vrati false kdyz plne (drop).
    // Lock-free, RT-safe.
    bool push(const StreamRequest& r) noexcept {
        const size_t w = w_.load(std::memory_order_relaxed);
        const size_t rd = r_.load(std::memory_order_acquire);
        if (w - rd >= (size_t)kCap) return false;
        buf_[w % kCap] = r;
        w_.store(w + 1, std::memory_order_release);
        return true;
    }

    // Konzument (worker thread): vyzvedne dalsi request. Mutex serializuje
    // multiple konzumenty mezi sebou (NE proti push, ten zustava lock-free).
    bool pop(StreamRequest& out) noexcept {
        std::lock_guard<std::mutex> lk(pop_mtx_);
        const size_t rd = r_.load(std::memory_order_relaxed);
        const size_t w  = w_.load(std::memory_order_acquire);
        if (rd >= w) return false;
        out = buf_[rd % kCap];
        r_.store(rd + 1, std::memory_order_release);
        return true;
    }

private:
    StreamRequest       buf_[kCap];
    std::atomic<size_t> w_{0};
    std::atomic<size_t> r_{0};
    std::mutex          pop_mtx_;   // SPMC: serialize multiple worker pops
};

class StreamEngine {
public:
    // n_workers: pocet worker threadu paralelne pres frontu. Vice workeru =
    // vetsi propustnost disku I/O = mensi sance underrunu pri akordu + rezonanci
    // v jednom audio bloku. Default 4 (rozumny kompromis CPU vs prustok).
    explicit StreamEngine(int n_rings = 32, int ring_capacity_frames = 8192,
                          int n_workers = 4);
    ~StreamEngine();

    StreamEngine(const StreamEngine&) = delete;
    StreamEngine& operator=(const StreamEngine&) = delete;

    // Spusti vsechny worker threads. Idempotent.
    void start();
    // Zastavi vsechny workers (join). Idempotent.
    void stop();

    // Allocator: najdi volny slot. Vrati nullptr kdyz vsechny obsazeny
    // (Voice si pri tom musi cestou damp/skip). Atomic CAS na in_use_.
    RingHandle* acquireRing();
    // Vrati slot do poolu. Volat NA POSLEDNIM mestu po dokonceni
    // (Voice po EOF / underrun fade / release / steal).
    void releaseRing(RingHandle* r);

    // Naplnovac (volat z Voice po vypoctu, ze je v ringu malo dat). Drop-on-full
    // pri zaplneni fronty; tim padem RT-safe. Vraci false pri plne fronte —
    // caller pak NEPOSOUVA file_request_off_ (request prirozene zopakuje
    // pristi blok; drive tichy drop maskoval underrun jako END-OF-SAMPLE).
    bool requestRead(RingHandle* ring, const SampleFile& file,
                     int64_t frame_off, int64_t n_frames,
                     bool eof_when_done = false) noexcept;

    // Diagnostika/test.
    int  capacityFrames() const noexcept { return ring_capacity_frames_; }
    int  numRings()       const noexcept { return (int)rings_.size(); }
    // Pocet ringu, ktere jsou aktualne in_use (diagnostika).
    int  numRingsUsed()   const noexcept;

    // -- Underrun diagnostika (per-pool). Hlas pri underrunu vola noteUnderrun();
    //    GUI cte underrunRecent (vzor jako Engine::noteOnRecent). --
    void noteUnderrun() noexcept;                 // orazitkuje steady_clock micros
    bool underrunRecent(float ms) const noexcept; // true kdyz posledni underrun < ms

    // Refill threshold v stereo frames. Voice si pravidlo "kdyz je v ringu
    // mene nez refill_threshold a soubor nedohran → posli refill" cte odsud.
    // Skalovani s block_size (vetsi block → vetsi spotreba na audio tick →
    // ring potrebuje vyssi prah; alespon 4 bloky napred).
    int  refillThresholdFrames() const noexcept {
        return refill_threshold_.load(std::memory_order_relaxed);
    }
    // Engine::recomputeRefillThreshold (init/setBlockSize) sem posila
    // min(max(capacity/2, block_size*4), capacity-64).
    void setRefillThresholdFrames(int v) noexcept {
        refill_threshold_.store(v, std::memory_order_relaxed);
    }

private:
    void workerLoop();

    // Pool je drzeny v unique_ptr aby se neinvalidovaly pointery (RingHandle*
    // se drzi Voice — nesmi se realokovat). std::vector<RingHandle> by se
    // realokoval pri reserve/push_back. Zde alokujeme 1x v ctoru a hotovo,
    // ale pro jistotu pouzivame raw alokaci pres unique_ptr<RingHandle[]>.
    // POZN.: RingHandle ma atomic membery → ani move/copy nedavaji smysl.
    std::vector<std::unique_ptr<RingHandle>> rings_;
    int                 ring_capacity_frames_ = 0;
    int                 n_workers_ = 4;

    StreamRequestQueue  req_q_;

    std::atomic<bool>   run_{false};
    std::atomic<int>    refill_threshold_{4096};
    std::atomic<uint64_t> last_underrun_us_{0};
    std::vector<std::thread> workers_;
};

} // namespace ithaca
