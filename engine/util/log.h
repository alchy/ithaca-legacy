#pragma once
// engine/util/log.h
// -----------------
// RT-safe logger pro ithaca-legacy. Adaptovany styl z icr2 (engine/util/log.h):
//   - 5 severity (Debug/Info/Warning/Error/Fatal)
//   - dual API: log() pro non-RT (mutex + varargs) a logRT() pro RT-safe
//     lock-free ring buffer (audio thread nesmi alokovat ani zamykat)
//   - console + volitelny file output, oba togglovatelne
//   - atomic severity filtr, menitelny za behu
//   - default_() singleton pro kod, ktery nedostane Logger& parametrem
//   - format: [HH:MM:SS.mmm] [component] [SEVERITY]: message
//
// Pouziti (non-RT):  LOG_INFO("bank", "nacteno: %s vrstvy=%d", path, n);
// Pouziti (RT):      LOG_RT_WARN("voice", "underrun na midi=%d", midi);
//                    // z non-RT threadu pravidelne: Logger::default_().flushRTBuffer();

#include <atomic>
#include <array>
#include <cstdarg>
#include <cstdint>
#include <fstream>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace ithaca::log {

enum class Severity : uint8_t {
    Debug = 0, Info = 1, Warning = 2, Error = 3, Fatal = 4,
};

// Strukturovany log event — predavany Subscriberum (napr. GUI log strip).
// timestamp_us je v jednotce, jakou pouziva Logger interne (currently
// wall-clock micros od epochy, viz Logger::nowMicros()). Pro UI staci
// relativni razeni; absolutni interpretace neni zaruceny kontrakt.
struct LogEntry {
    long long   timestamp_us;
    std::string topic;
    Severity    sev;
    std::string message;
};

const char* severity_to_string(Severity s);

// Parsuje "debug"/"info"/"warn"/"warning"/"error"/"fatal" (case-insensitive).
// Vraci default_value kdyz nelze parsovat.
Severity severity_from_string(const char* s,
                              Severity default_value = Severity::Info);

class Logger {
public:
    Logger();
    ~Logger();
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    // Process-wide default instance pouzivana LOG_* makry.
    static Logger& default_();

    // -- Konfigurace (thread-safe atomic) --
    void setMinSeverity(Severity s);
    Severity getMinSeverity() const;
    // console = stdout/stderr; file vyzaduje predchozi setLogFile().
    void setOutputMode(bool useConsole, bool useFile);
    bool setLogFile(const std::string& path);  // false on failure
    void closeLogFile();

    // -- Non-RT API (printf-style, mutex) --
    void log(const char* component, Severity severity, const char* format, ...)
#if defined(__GNUC__) || defined(__clang__)
        __attribute__((format(printf, 4, 5)))
#endif
        ;
    void vlog(const char* component, Severity severity,
              const char* format, va_list args);

    // -- RT-safe API (SPSC ring buffer) --
    // Single-producer: logRT smi volat jen JEDEN (audio/RT) thread. Format do
    // entry, pak release-publish write indexu. Zadne alokace, zadne mutexy. RT
    // thread NIKDY neblokuje: kdyz je ring plny (flush z non-RT threadu nestiha),
    // zprava se ZAHODI a zapocita do rtDroppedCount(). Caller MUSI pravidelne
    // volat flushRTBuffer() z non-RT threadu, jinak rostou drop-y.
    void logRT(const char* component, Severity severity, const char* format, ...)
#if defined(__GNUC__) || defined(__clang__)
        __attribute__((format(printf, 4, 5)))
#endif
        ;
    void vlogRT(const char* component, Severity severity,
                const char* format, va_list args);
    // Vyprazdni ring buffer → console + file. Volat z non-RT threadu.
    // Vraci pocet flushnutych zprav.
    int flushRTBuffer();
    // Pocet RT zprav zahozenych kvuli plnemu ring bufferu (flush nestihal).
    uint64_t rtDroppedCount() const { return rt_dropped_.load(std::memory_order_relaxed); }

    // -- Subscriber API (pro GUI log strip, audit hooks atd.) --
    // Subscriber je callback volany synchronnousne pri kazdem uspesnem log()
    // volani (po formatu zpravy, pod separe mutex). Pozn.: zatim NEvolano z
    // RT cesty (vlogRT); ta jde pres ring buffer a flushRTBuffer() ho jen
    // tiskne — pokud bude treba notifikovat i flush zpravy, prida se zvlast.
    // KRITICKE: subscriber callback nesmi sam volat log() (deadlock pres
    // subscriber_mtx_, byt log_mutex_ je separe).
    using Subscriber = std::function<void(const LogEntry&)>;
    void addSubscriber(Subscriber s);
    void clearSubscribers();

private:
    static constexpr size_t COMPONENT_MAX  = 32;
    static constexpr size_t MESSAGE_MAX    = 256;
    static constexpr size_t RT_BUFFER_SIZE = 1024;

    struct Entry {
        char     component[COMPONENT_MAX];
        char     message[MESSAGE_MAX];
        uint64_t timestamp_us;
        Severity severity;
        Entry() : timestamp_us(0), severity(Severity::Info) {
            component[0] = '\0';
            message[0]   = '\0';
        }
    };

    std::array<Entry, RT_BUFFER_SIZE> rt_buffer_;
    std::atomic<size_t> rt_write_idx_{0};
    std::atomic<size_t> rt_read_idx_{0};
    std::atomic<uint64_t> rt_dropped_{0};

    std::string           log_file_path_;
    std::ofstream         log_file_;
    mutable std::mutex    log_mutex_;
    std::atomic<Severity> min_severity_{Severity::Info};
    std::atomic<bool>     use_console_{true};
    std::atomic<bool>     use_file_{false};

    // Subscribery — separe mutex aby drzeni log_mutex_ a notify nebylo
    // smichano (a aby pridani subscribera neblokovalo bezici log call s file
    // I/O na log_mutex_ zbytecne dlouho).
    mutable std::mutex      subscriber_mtx_;
    std::vector<Subscriber> subscribers_;

    bool shouldLog(Severity s) const;
    void writeEntry(const char* component, Severity severity,
                    const char* message, uint64_t timestamp_us);
    static uint64_t nowMicros();
    static std::string formatTimestamp(uint64_t micros);
};

} // namespace ithaca::log

// -- Convenience makra (printf-style) --
#define ITHACA_LOG_(sev_, comp_, ...) \
    ::ithaca::log::Logger::default_().log((comp_), (sev_), __VA_ARGS__)

#define LOG_DEBUG(comp_, ...) ITHACA_LOG_(::ithaca::log::Severity::Debug,   comp_, __VA_ARGS__)
#define LOG_INFO(comp_, ...)  ITHACA_LOG_(::ithaca::log::Severity::Info,    comp_, __VA_ARGS__)
#define LOG_WARN(comp_, ...)  ITHACA_LOG_(::ithaca::log::Severity::Warning, comp_, __VA_ARGS__)
#define LOG_ERROR(comp_, ...) ITHACA_LOG_(::ithaca::log::Severity::Error,   comp_, __VA_ARGS__)
#define LOG_FATAL(comp_, ...) ITHACA_LOG_(::ithaca::log::Severity::Fatal,   comp_, __VA_ARGS__)

#define ITHACA_LOG_RT_(sev_, comp_, ...) \
    ::ithaca::log::Logger::default_().logRT((comp_), (sev_), __VA_ARGS__)

#define LOG_RT_DEBUG(comp_, ...) ITHACA_LOG_RT_(::ithaca::log::Severity::Debug,   comp_, __VA_ARGS__)
#define LOG_RT_INFO(comp_, ...)  ITHACA_LOG_RT_(::ithaca::log::Severity::Info,    comp_, __VA_ARGS__)
#define LOG_RT_WARN(comp_, ...)  ITHACA_LOG_RT_(::ithaca::log::Severity::Warning, comp_, __VA_ARGS__)
#define LOG_RT_ERROR(comp_, ...) ITHACA_LOG_RT_(::ithaca::log::Severity::Error,   comp_, __VA_ARGS__)
