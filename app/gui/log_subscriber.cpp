// app/gui/log_subscriber.cpp - viz log_subscriber.h.
#include "log_subscriber.h"

namespace ithaca::gui {

void LogRingBuffer::push(const log::LogEntry& e) {
    std::lock_guard<std::mutex> lk(mtx_);
    buf_[head_] = e;
    head_ = (head_ + 1) % kCapacity;
    if (size_ < kCapacity) ++size_;
}

int LogRingBuffer::snapshot(log::LogEntry* out, int max_n) const {
    std::lock_guard<std::mutex> lk(mtx_);
    if (max_n <= 0 || size_ == 0) return 0;
    const int n = (max_n < size_) ? max_n : size_;
    // Zacatek logickeho okna = head_ - size_ (modulo kCapacity).
    int start = (head_ - size_ + kCapacity) % kCapacity;
    // Pokud chceme jen poslednich `max_n` z `size_`, preskoc starsi.
    if (size_ > max_n) start = (start + (size_ - max_n)) % kCapacity;
    for (int i = 0; i < n; ++i) {
        out[i] = buf_[(start + i) % kCapacity];
    }
    return n;
}

int LogRingBuffer::size() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return size_;
}

void LogRingBuffer::clear() {
    std::lock_guard<std::mutex> lk(mtx_);
    head_ = 0;
    size_ = 0;
    // Obsah bufferu zamerne nemazeme — slice [0, size_) je jediny relevantni,
    // takze prepiseme pri dalsim push. Setrime alokace v std::string field.
}

} // namespace ithaca::gui
