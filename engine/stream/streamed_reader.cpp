// engine/stream/streamed_reader.cpp — viz streamed_reader.h.
// Kod prenesen 1:1 z Voice/ResonanceVoice (bit-exact extrakce; hlida
// tests/test_render_regression.cpp + behavioralni streaming testy).
#include "stream/streamed_reader.h"

#include "stream/stream_engine.h"

namespace ithaca {

bool StreamedSampleReader::begin(StreamEngine* se, const std::string& path,
                                 int64_t start_frame,
                                 int64_t total_frames) noexcept {
    total_frames_     = total_frames;
    file_request_off_ = start_frame;
    ring_ = se ? se->acquireRing() : nullptr;
    if (!ring_) return false;
    const int64_t cap          = (int64_t)ring_->capacity_frames;
    const int64_t total_stream = total_frames - start_frame;
    const bool    eof_done     = (cap >= total_stream);
    const int64_t actual       = (cap < total_stream) ? cap : total_stream;
    if (se->requestRead(ring_, path, start_frame, actual, eof_done)) {
        file_request_off_ = start_frame + actual;
        stream_pending_   = true;
    }
    // false → fronta plna: offset NEposouvat, refill() request zopakuje.
    return true;
}

bool StreamedSampleReader::beginEofOnly(StreamEngine* se,
                                        int64_t request_off) noexcept {
    total_frames_     = request_off;
    file_request_off_ = request_off;
    ring_ = se ? se->acquireRing() : nullptr;
    if (!ring_) return false;
    ring_->eof_.store(true, std::memory_order_release);
    return true;
}

void StreamedSampleReader::seed(float lo_l, float lo_r,
                                int64_t lo_idx) noexcept {
    ring_lo_idx_ = lo_idx;
    ring_lo_l_   = lo_l;
    ring_lo_r_   = lo_r;
    // hi = prvni ring frame (lookahead). Kdyz neni, fallback hi=lo —
    // pripadny posun okna / EOF policy to vyresi v advance().
    float L, R;
    if (popFrameRaw(L, R)) { ring_hi_l_ = L; ring_hi_r_ = R; }
    else                   { ring_hi_l_ = ring_lo_l_; ring_hi_r_ = ring_lo_r_; }
}

StreamedSampleReader::Advance
StreamedSampleReader::advance(int64_t target) noexcept {
    while (ring_lo_idx_ < target) {
        // posun okna: hi → lo, novy hi z ringu.
        ring_lo_l_ = ring_hi_l_;
        ring_lo_r_ = ring_hi_r_;
        float L, R;
        if (!popFrameRaw(L, R)) return Advance::RingEmpty;
        ring_hi_l_ = L;
        ring_hi_r_ = R;
        ring_lo_idx_++;
    }
    return Advance::Reached;
}

void StreamedSampleReader::refill(StreamEngine* se,
                                  const std::string& path) noexcept {
    if (!ring_ || !se) return;
    const int avail    = ring_->available();
    const int thr      = se->refillThresholdFrames();
    const int half_cap = ring_->capacity_frames / 2;
    if (stream_pending_ && avail >= half_cap) {
        stream_pending_ = false;   // worker dohnal predchozi request
    }
    if (!stream_pending_ && avail < thr) {
        const int64_t remain = total_frames_ - file_request_off_;
        if (remain > 0) {
            // Pozadej tolik, kolik se ted vejde do volneho mista v ringu.
            int64_t want = (int64_t)(ring_->capacity_frames - avail);
            if (want > remain) want = remain;
            const bool eof_done =
                (file_request_off_ + want >= total_frames_);
            if (se->requestRead(ring_, path, file_request_off_, want,
                                eof_done)) {
                file_request_off_ += want;
                stream_pending_    = true;
            }
            // false → drop (fronta plna); zadny posun offsetu, jinak by se
            // underrun maskoval jako END-OF-SAMPLE a framy se uz nedozadaly.
        }
        // remain <= 0: cely zbytek souboru uz byl pozadan; worker po dohrani
        // nastavi eof_ a hlas dohaje cisto. Zadny dalsi request.
    }
}

int StreamedSampleReader::popInto(float* dst_interleaved,
                                  int max_frames) noexcept {
    int n = 0;
    for (; n < max_frames; ++n) {
        float L, R;
        if (!popFrameRaw(L, R)) break;
        dst_interleaved[n * 2]     = L;
        dst_interleaved[n * 2 + 1] = R;
    }
    return n;
}

void StreamedSampleReader::beginBlock() noexcept {
    if (!ring_ || blk_open_) return;
    blk_w_    = ring_->snapshotW();
    blk_r_    = ring_->cursorR();
    blk_open_ = true;
}

void StreamedSampleReader::endBlock() noexcept {
    if (!blk_open_) return;
    if (ring_) ring_->commitR(blk_r_);
    blk_open_ = false;
}

int StreamedSampleReader::ringAvailable() const noexcept {
    if (!ring_) return -1;
    if (blk_open_) {
        // Pri 0 refreshni snapshot (stejna semantika jako atomic available();
        // EOF hard-guard nesmi deaktivovat kvuli stale snapshotu).
        if (blk_r_ >= blk_w_) blk_w_ = ring_->snapshotW();
        return (int)(blk_w_ - blk_r_);
    }
    return ring_->available();
}

bool StreamedSampleReader::eofAcquire() const noexcept {
    return ring_ && ring_->eof_.load(std::memory_order_acquire);
}

bool StreamedSampleReader::eofRelaxed() const noexcept {
    return ring_ && ring_->eof_.load(std::memory_order_relaxed);
}

void StreamedSampleReader::release(StreamEngine* se) noexcept {
    endBlock();   // pripadny otevreny bulk blok commitni pred vracenim ringu
    if (ring_ && se) se->releaseRing(ring_);
    ring_              = nullptr;
    total_frames_      = 0;
    file_request_off_  = 0;
    stream_pending_    = false;
    ring_lo_l_ = 0.f; ring_lo_r_ = 0.f;
    ring_hi_l_ = 0.f; ring_hi_r_ = 0.f;
    ring_lo_idx_ = -1;
}

bool StreamedSampleReader::popFrameRaw(float& L, float& R) noexcept {
    if (!ring_) return false;
    if (!blk_open_) return ring_->popFrame(L, R);
    // Bulk rezim: lokalni kurzor nad snapshotem (commit dela endBlock).
    if (blk_r_ >= blk_w_) {
        // Re-snapshot pri vycerpani: worker mohl dodat data BEHEM bloku —
        // semantika prazdneho ringu zustava shodna s per-frame popFrame
        // (jinak by mid-block dodavka vedla k falesnemu underrunu).
        blk_w_ = ring_->snapshotW();
        if (blk_r_ >= blk_w_) return false;
    }
    const size_t off = blk_r_ % (size_t)ring_->capacity_frames;
    L = ring_->buf[off * 2];
    R = ring_->buf[off * 2 + 1];
    ++blk_r_;
    return true;
}

} // namespace ithaca
