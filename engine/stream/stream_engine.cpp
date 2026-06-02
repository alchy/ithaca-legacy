// engine/stream/stream_engine.cpp — viz stream_engine.h.
#include "stream/stream_engine.h"

#include "io/wav_reader.h"
#include "util/log.h"

#include <algorithm>
#include <chrono>
#include <cstring>

namespace ithaca {

// ----- RingHandle ----------------------------------------------------------

int RingHandle::push(const float* src_interleaved, int n_frames) {
    if (capacity_frames <= 0 || n_frames <= 0) return 0;
    const size_t w  = w_.load(std::memory_order_relaxed);
    const size_t rd = r_.load(std::memory_order_acquire);
    const size_t used = w - rd;
    const size_t free_frames = (size_t)capacity_frames - used;
    if (free_frames == 0) return 0;
    const size_t to_write = (size_t)n_frames < free_frames
                          ? (size_t)n_frames : free_frames;
    // Mozne preklenuti pres konec bufferu (wrap) → 1-2 memcpy.
    const size_t cap = (size_t)capacity_frames;
    size_t off = w % cap;
    size_t first = (off + to_write <= cap) ? to_write : (cap - off);
    std::memcpy(buf.data() + off * 2, src_interleaved, first * 2 * sizeof(float));
    if (first < to_write) {
        size_t second = to_write - first;
        std::memcpy(buf.data(), src_interleaved + first * 2,
                    second * 2 * sizeof(float));
    }
    w_.store(w + to_write, std::memory_order_release);
    return (int)to_write;
}

bool RingHandle::popFrame(float& L, float& R) noexcept {
    if (capacity_frames <= 0) return false;
    const size_t rd = r_.load(std::memory_order_relaxed);
    const size_t w  = w_.load(std::memory_order_acquire);
    if (rd >= w) return false;
    const size_t off = rd % (size_t)capacity_frames;
    L = buf[off * 2];
    R = buf[off * 2 + 1];
    r_.store(rd + 1, std::memory_order_release);
    return true;
}

int RingHandle::available() const noexcept {
    const size_t w  = w_.load(std::memory_order_acquire);
    const size_t rd = r_.load(std::memory_order_relaxed);
    return (int)(w - rd);
}

void RingHandle::resetForReuse() noexcept {
    // Vola se z toho threadu, ktery prave ring zustanovil (typicky audio thread
    // pri releaseRing) — worker uz s nim nepracuje (Voice se odpojil pred
    // releasem, request fronta byla flushnuta workerem). Sequential
    // consistency stacne, prepoklad zadne souciene operace.
    w_.store(0, std::memory_order_relaxed);
    r_.store(0, std::memory_order_relaxed);
    eof_.store(false, std::memory_order_relaxed);
}

// ----- StreamEngine --------------------------------------------------------

StreamEngine::StreamEngine(int n_rings, int ring_capacity_frames, int n_workers)
    : ring_capacity_frames_(ring_capacity_frames > 0 ? ring_capacity_frames : 8192)
    , n_workers_(n_workers > 0 ? n_workers : 4) {
    int n = (n_rings > 0) ? n_rings : 32;
    rings_.reserve((size_t)n);
    for (int i = 0; i < n; ++i) {
        auto r = std::make_unique<RingHandle>();
        r->capacity_frames = ring_capacity_frames_;
        // Stereo interleaved → 2 floats per frame.
        r->buf.resize((size_t)ring_capacity_frames_ * 2, 0.f);
        rings_.push_back(std::move(r));
    }
    // Default refill threshold: pulka ringu (block_size se prepocita pres
    // setRefillThresholdFrames z Engine::setBlockSize).
    refill_threshold_.store(ring_capacity_frames_ / 2, std::memory_order_relaxed);
}

StreamEngine::~StreamEngine() {
    stop();
}

void StreamEngine::start() {
    bool expected = false;
    if (!run_.compare_exchange_strong(expected, true)) return;  // uz bezi
    workers_.clear();
    workers_.reserve((size_t)n_workers_);
    for (int i = 0; i < n_workers_; ++i) {
        workers_.emplace_back([this] { workerLoop(); });
    }
}

void StreamEngine::stop() {
    bool expected = true;
    if (!run_.compare_exchange_strong(expected, false)) return; // uz stoji
    for (auto& w : workers_) {
        if (w.joinable()) w.join();
    }
    workers_.clear();
}

RingHandle* StreamEngine::acquireRing() {
    for (auto& uptr : rings_) {
        bool expected = false;
        if (uptr->in_use_.compare_exchange_strong(expected, true,
                std::memory_order_acquire, std::memory_order_relaxed)) {
            // Restartujeme stav ringu pred predanim novemu konzumentovi.
            uptr->resetForReuse();
            return uptr.get();
        }
    }
    return nullptr;
}

int StreamEngine::numRingsUsed() const noexcept {
    // Spocti, kolik slotu ma `in_use_` flag nastaveny. Snapshot bez locku —
    // hodnota muze byt o jeden mimo, kdyz se zrovna acquire/release deje,
    // ale pro diagnostiku/GUI staci (refresh ~30 Hz).
    int n = 0;
    for (const auto& r : rings_) {
        if (r->in_use_.load(std::memory_order_acquire)) ++n;
    }
    return n;
}

void StreamEngine::releaseRing(RingHandle* r) {
    if (!r) return;
    // Reset PRED uvolnenim flagu — kdyby si jiny thread chnel ring po flagu
    // a my zacali resetovat data, ktera prave producent zapsal. Voice musi
    // zaroven garantovat, ze pri releaseRing uz neexistuje pending request
    // (Voice nepushuje refill mezi posledni pop a release). Worker, ktery
    // pripadne stale drzi request na tento ring, zapise data do ringu, ale
    // Voice uz je necte — nic strasneho, jen plyvanie.
    r->resetForReuse();
    r->in_use_.store(false, std::memory_order_release);
}

void StreamEngine::requestRead(RingHandle* ring, const std::string& path,
                               int64_t frame_off, int64_t n_frames,
                               bool eof_when_done) noexcept {
    if (!ring) return;
    StreamRequest req;
    req.ring          = ring;
    req.path          = path;       // kopie (SBO casto staci)
    req.frame_off     = frame_off;
    req.n_frames      = n_frames;
    req.eof_when_done = eof_when_done;
    // Drop-on-full je RT-safe; voice prezije diky underrun fade.
    (void)req_q_.push(req);
}

void StreamEngine::workerLoop() {
    using namespace std::chrono_literals;
    while (run_.load(std::memory_order_acquire)) {
        StreamRequest req;
        bool got = req_q_.pop(req);
        if (!got) {
            std::this_thread::sleep_for(1ms);
            continue;
        }
        if (!req.ring || req.n_frames <= 0) {
            // Defensivne: vyhnout se 0-byte cteni / NPE.
            continue;
        }

        // Cteme po chunci, ktere se vejdou do ringu, dokud nedostaneme vsechno
        // pozadovane nebo nedosahneme EOF.
        int64_t off    = req.frame_off;
        int64_t remain = req.n_frames;
        bool    eof    = false;

        while (remain > 0 && run_.load(std::memory_order_acquire)) {
            // Maximalni chunk = volne misto v ringu (do capacity).
            int free_frames = req.ring->capacity_frames - req.ring->available();
            if (free_frames <= 0) {
                // Ring je plny → pockej kratce, Voice si bere data.
                std::this_thread::sleep_for(1ms);
                continue;
            }
            int64_t chunk = (remain < (int64_t)free_frames)
                          ? remain : (int64_t)free_frames;

            WavData data = readWavRange(req.path, off, chunk);
            if (!data.valid) {
                // Cteni selhalo (chyba souboru): zaloguj a koncim s tymto requestem.
                // POZN.: logger je non-RT-safe, jsme na workeru, OK.
                log::Logger::default_().log(
                    "stream", log::Severity::Warning,
                    "readWavRange failed: %s @ %lld", req.path.c_str(),
                    (long long)off);
                break;
            }
            if (data.frames == 0) {
                // EOF (offset >= total_frames).
                eof = true;
                break;
            }
            int wrote = req.ring->push(data.samples.data(), data.frames);
            // wrote by mel byt presne data.frames (free_frames jsme spocitali).
            // Pokud Voice mezi-tim odebral, jsme OK (push vrati ten kus, co
            // se vejde; zbytek si pomyslne ukrojime z remain a posuneme off).
            off    += wrote;
            remain -= wrote;
            if (wrote < data.frames) {
                // Cast dat se nezmestila — neztraceji se: pristi iterace
                // pocita s posunutym off a doctene v dalsim chunku.
                // To je OK protoze readWavRange umi cist od libovolneho offsetu.
                // POZN.: efektivnost je sub-optimalni (precteme znovu i tu cast,
                // co uz mame v dat). Pro prvni iteraci akceptujeme; FUTURE:
                // partial-push s pamatovanim zbytku.
                // (V praxi ring je vetsi nez chunk → wrote == data.frames vzdy.)
            }
            if (data.frames < chunk) {
                // Mene nez pozadovano → soubor skoncil, EOF.
                eof = true;
                break;
            }
        }

        if (eof && req.eof_when_done) {
            req.ring->eof_.store(true, std::memory_order_release);
        }
    }
}

namespace {
uint64_t nowMicrosSE() {
    using namespace std::chrono;
    return (uint64_t)duration_cast<microseconds>(
        steady_clock::now().time_since_epoch()).count();
}
} // namespace

void StreamEngine::noteUnderrun() noexcept {
    last_underrun_us_.store(nowMicrosSE(), std::memory_order_relaxed);
}

bool StreamEngine::underrunRecent(float ms) const noexcept {
    const uint64_t t = last_underrun_us_.load(std::memory_order_relaxed);
    if (t == 0) return false;
    return (nowMicrosSE() - t) < (uint64_t)(ms * 1000.f);
}

} // namespace ithaca
