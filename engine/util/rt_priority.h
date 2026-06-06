#pragma once
// engine/util/rt_priority.h
// ------------------------
// RT priorita audio vlakna napric platformami. Per-thread, idempotentni,
// soft-failure: API vraci stav, caller loguje. Viz docs/rt-thread-priority.md
// pro design a navodne hlasky.
//
//   Linux: pthread_setschedparam(SCHED_FIFO, prio=80)
//   macOS: thread_policy_set(THREAD_TIME_CONSTRAINT_POLICY)
//   Win:   SetThreadPriority(TIME_CRITICAL) + MMCSS task "Pro Audio"
//
// Pri selhani podstatne API → Failed (caller loguje LOG_RT_WARN + TIP).
// Pri castecnem uspechu (Win: priority OK, MMCSS not) → Partial (caller loguje
// LOG_RT_INFO s TIP o sekundarnim, audio bezi s pouze TIME_CRITICAL).
// Pri plnem uspechu → Full.
//
// `err_code`: platform-specificky raw error code (errno / GetLastError /
// kern_return_t) pro diagnostiku v logu. 0 pri Full.

namespace ithaca {

enum class RtAudioStatus {
    Full    = 0,   // ok
    Partial = 1,   // castecny — primary API ok, sekundarni (MMCSS) ne
    Failed  = 2,   // primary API selhalo, default scheduling
};

struct RtAudioParams {
    int sample_rate;   // napr. 48000
    int block_size;    // napr. 256
};

// Volat z audio vlakna pri prvnim processBlock (thread_local guard u callera).
// Vraci stav; err_code je out param (smi byt nullptr).
RtAudioStatus enableRealtimeAudio(const RtAudioParams& p, int* err_code) noexcept;

// Volat z non-RT pri shutdown (rusi MMCSS handle na Windows; jinde no-op).
void disableRealtimeAudio() noexcept;

} // namespace ithaca
