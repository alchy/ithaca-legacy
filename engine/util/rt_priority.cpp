// engine/util/rt_priority.cpp — viz rt_priority.h + docs/rt-thread-priority.md.
#include "util/rt_priority.h"

#if defined(_WIN32)
  #include <windows.h>
  #include <avrt.h>
#elif defined(__APPLE__)
  #include <mach/mach.h>
  #include <mach/mach_init.h>
  #include <mach/thread_act.h>
  #include <mach/thread_policy.h>
  #include <mach/mach_time.h>
#elif defined(__linux__)
  #include <pthread.h>
  #include <sched.h>
  #include <cerrno>
#endif

namespace ithaca {

#if defined(_WIN32)
// MMCSS handle drzime per-thread; uvolnujeme v disableRealtimeAudio() volanem
// z non-RT pri shutdown. Pokud caller zapomene, MMCSS handle se uvolni s
// procesem — neni katastrofa, ale styl je explicit cleanup.
static thread_local HANDLE g_mmcss_handle = nullptr;
#endif

RtAudioStatus enableRealtimeAudio(const RtAudioParams& p, int* err_code) noexcept {
    if (err_code) *err_code = 0;

#if defined(_WIN32)
    (void)p;   // MMCSS si dela vlastni rozhodnuti, neresi nas period

    if (!SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL)) {
        if (err_code) *err_code = (int)GetLastError();
        return RtAudioStatus::Failed;
    }

    DWORD task_index = 0;
    HANDLE h = AvSetMmThreadCharacteristicsW(L"Pro Audio", &task_index);
    if (h == nullptr) {
        // SetThreadPriority prosel, MMCSS ne — castecny uspech.
        if (err_code) *err_code = (int)GetLastError();
        return RtAudioStatus::Partial;
    }
    g_mmcss_handle = h;
    return RtAudioStatus::Full;

#elif defined(__APPLE__)
    // THREAD_TIME_CONSTRAINT_POLICY: rikame kernelu "potrebuju X cyklu kazdych
    // Y casovych jednotek, hard deadline Z". Mach abs time je tickove, prevod
    // pres mach_timebase_info (numer/denom).
    mach_timebase_info_data_t tb;
    if (mach_timebase_info(&tb) != KERN_SUCCESS) {
        if (err_code) *err_code = -1;
        return RtAudioStatus::Failed;
    }
    const double period_ns = (double)p.block_size * 1.0e9 / (double)p.sample_rate;
    const uint64_t period_abs = (uint64_t)(period_ns * (double)tb.denom / (double)tb.numer);

    thread_time_constraint_policy_data_t pol;
    pol.period      = (uint32_t)period_abs;
    pol.computation = (uint32_t)(period_abs / 2);   // committed pulka bloku
    pol.constraint  = (uint32_t)period_abs;          // hard deadline = period
    pol.preemptible = 0;                             // non-preemptible v ramci slotu

    kern_return_t kr = thread_policy_set(
        mach_thread_self(),
        THREAD_TIME_CONSTRAINT_POLICY,
        (thread_policy_t)&pol,
        THREAD_TIME_CONSTRAINT_POLICY_COUNT);

    if (kr != KERN_SUCCESS) {
        if (err_code) *err_code = (int)kr;
        return RtAudioStatus::Failed;
    }
    return RtAudioStatus::Full;

#elif defined(__linux__)
    (void)p;
    struct sched_param sp;
    sp.sched_priority = 80;   // audio konvence: 70-90 (ne 99, RT watchdog headroom)
    int err = pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);
    if (err == 0) return RtAudioStatus::Full;

    if (err_code) *err_code = err;
    return RtAudioStatus::Failed;

#else
    // Neznama platforma — no-op, hlasi Failed. Audio bezi.
    (void)p;
    if (err_code) *err_code = -1;
    return RtAudioStatus::Failed;
#endif
}

void disableRealtimeAudio() noexcept {
#if defined(_WIN32)
    if (g_mmcss_handle) {
        AvRevertMmThreadCharacteristics(g_mmcss_handle);
        g_mmcss_handle = nullptr;
    }
#endif
}

} // namespace ithaca
