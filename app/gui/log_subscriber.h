// app/gui/log_subscriber.h - cirkularni buffer log eventu pro GUI log strip.
//
// Konzument je hlavni (GUI/ImGui render) thread, ktery ~30 Hz cte snapshot
// poslednich N zaznamu. Producent je libovolny thread, ktery loguje pres
// Logger — push se vola z Subscriber callbacku, ktery Logger volau pod svym
// mutexem; my pridame jeste svuj vlastni mutex pro samotny buffer. push je
// triviální (lock + 1 array assign), takze drzeni Logger mutexu je kratke.
#pragma once

#include "util/log.h"

#include <array>
#include <mutex>

namespace ithaca::gui {

// Cirkularni buffer N poslednich log eventu, thread-safe.
class LogRingBuffer {
public:
    static constexpr int kCapacity = 256;

    // Zapis noveho eventu — pretece-li, prepise nejstarsi.
    void push(const log::LogEntry& e);

    // Snapshot: zkopiruj `max_n` nejnovejsich zaznamu do `out` (chronologicky;
    // nejstarsi prvni). Vraci pocet skutecne zkopirovanych (<= max_n a <= size).
    int snapshot(log::LogEntry* out, int max_n) const;

    // Pocet aktualne ulozenych zaznamu (0..kCapacity). Pro UI tooltipy/diag.
    int size() const;

    // Reset bufferu — pro testy i pro UI "Clear" tlacitko v dalsi fazi.
    void clear();

private:
    mutable std::mutex                   mtx_;
    std::array<log::LogEntry, kCapacity> buf_{};
    int                                  head_ = 0;  // index pro dalsi zapis
    int                                  size_ = 0;  // pocet platnych zaznamu
};

} // namespace ithaca::gui
