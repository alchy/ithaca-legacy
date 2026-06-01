// engine/util/log.cpp — implementace RT-safe loggeru (viz log.h).
#include "util/log.h"

#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>

namespace ithaca::log {

const char* severity_to_string(Severity s) {
    switch (s) {
        case Severity::Debug:   return "DEBUG";
        case Severity::Info:    return "INFO";
        case Severity::Warning: return "WARNING";
        case Severity::Error:   return "ERROR";
        case Severity::Fatal:   return "FATAL";
        case Severity::Off:     return "OFF";
    }
    return "INFO";
}

Severity severity_from_string(const char* s, Severity default_value) {
    if (!s) return default_value;
    std::string v;
    for (const char* p = s; *p; ++p) v += (char)std::tolower((unsigned char)*p);
    if (v == "debug")              return Severity::Debug;
    if (v == "info")               return Severity::Info;
    if (v == "warn" || v == "warning") return Severity::Warning;
    if (v == "error")              return Severity::Error;
    if (v == "fatal")              return Severity::Fatal;
    if (v == "off")                return Severity::Off;
    return default_value;
}

Logger::Logger() {}
Logger::~Logger() { closeLogFile(); }

Logger& Logger::default_() {
    static Logger instance;   // Meyers singleton — thread-safe init v C++11+
    return instance;
}

void Logger::setMinSeverity(Severity s) {
    min_severity_.store(s, std::memory_order_relaxed);
}
Severity Logger::getMinSeverity() const {
    return min_severity_.load(std::memory_order_relaxed);
}
void Logger::setOutputMode(bool useConsole, bool useFile) {
    use_console_.store(useConsole, std::memory_order_relaxed);
    use_file_.store(useFile, std::memory_order_relaxed);
}

bool Logger::setLogFile(const std::string& path) {
    std::lock_guard<std::mutex> lk(log_mutex_);
    if (log_file_.is_open()) log_file_.close();
    log_file_.open(path, std::ios::app);
    if (!log_file_.is_open()) return false;
    log_file_path_ = path;
    return true;
}
void Logger::closeLogFile() {
    std::lock_guard<std::mutex> lk(log_mutex_);
    if (log_file_.is_open()) log_file_.close();
    log_file_path_.clear();
}

bool Logger::shouldLog(Severity s) const {
    return (uint8_t)s >= (uint8_t)min_severity_.load(std::memory_order_relaxed);
}

uint64_t Logger::nowMicros() {
    using namespace std::chrono;
    return (uint64_t)duration_cast<microseconds>(
        system_clock::now().time_since_epoch()).count();
}

std::string Logger::formatTimestamp(uint64_t micros) {
    std::time_t secs = (std::time_t)(micros / 1000000ULL);
    int ms = (int)((micros / 1000ULL) % 1000ULL);
    std::tm tmv{};
#if defined(_WIN32)
    localtime_s(&tmv, &secs);
#else
    localtime_r(&secs, &tmv);
#endif
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d.%03d",
                  tmv.tm_hour, tmv.tm_min, tmv.tm_sec, ms);
    return std::string(buf);
}

void Logger::writeEntry(const char* component, Severity severity,
                        const char* message, uint64_t timestamp_us) {
    // Caller drzi log_mutex_ pro non-RT cestu; pro flush taky.
    std::string line = "[" + formatTimestamp(timestamp_us) + "] ["
                     + component + "] [" + severity_to_string(severity)
                     + "]: " + message + "\n";
    if (use_console_.load(std::memory_order_relaxed)) {
        std::FILE* out = ((uint8_t)severity >= (uint8_t)Severity::Warning)
                       ? stderr : stdout;
        std::fputs(line.c_str(), out);
        std::fflush(out);
    }
    if (use_file_.load(std::memory_order_relaxed) && log_file_.is_open()) {
        log_file_ << line;
        log_file_.flush();
    }
}

void Logger::log(const char* component, Severity severity,
                 const char* format, ...) {
    va_list args;
    va_start(args, format);
    vlog(component, severity, format, args);
    va_end(args);
}

void Logger::vlog(const char* component, Severity severity,
                  const char* format, va_list args) {
    if (!shouldLog(severity)) return;
    char buf[MESSAGE_MAX];
    std::vsnprintf(buf, sizeof(buf), format, args);
    const uint64_t ts_us = nowMicros();
    {
        std::lock_guard<std::mutex> lk(log_mutex_);
        writeEntry(component, severity, buf, ts_us);
    }
    // Notifikace subscriberum — po I/O, mimo log_mutex_, pod separe mutexem.
    // Double-check empty bez locku jako fast path (cteni vectoru size je race,
    // ale benigni: nejhure projdeme do locku zbytecne a najdeme prazdno).
    if (!subscribers_.empty()) {
        std::lock_guard<std::mutex> lk(subscriber_mtx_);
        if (!subscribers_.empty()) {
            LogEntry e{
                static_cast<long long>(ts_us),
                std::string(component ? component : ""),
                severity,
                std::string(buf),
            };
            for (auto& sub : subscribers_) {
                if (sub) sub(e);
            }
        }
    }
}

void Logger::addSubscriber(Subscriber s) {
    std::lock_guard<std::mutex> lk(subscriber_mtx_);
    subscribers_.push_back(std::move(s));
}

void Logger::clearSubscribers() {
    std::lock_guard<std::mutex> lk(subscriber_mtx_);
    subscribers_.clear();
}

void Logger::logRT(const char* component, Severity severity,
                   const char* format, ...) {
    va_list args;
    va_start(args, format);
    vlogRT(component, severity, format, args);
    va_end(args);
}

void Logger::vlogRT(const char* component, Severity severity,
                    const char* format, va_list args) {
    if (!shouldLog(severity)) return;
    // SPSC: sem zapisuje jen jediny (audio/RT) thread, ten je vlastnikem
    // write indexu. Synchronizace se ctenarem (flushRTBuffer) je pres publish
    // write indexu nize (release parovan s acquire ve flush).
    const size_t w = rt_write_idx_.load(std::memory_order_relaxed);
    const size_t r = rt_read_idx_.load(std::memory_order_acquire);
    if (w - r >= RT_BUFFER_SIZE) {
        // Ring je plny — flush z non-RT threadu nestiha. RT thread NESMI
        // blokovat, takze zpravu zahodime a jen zvedneme citac pro diagnostiku.
        rt_dropped_.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    Entry& e = rt_buffer_[w % RT_BUFFER_SIZE];
    std::vsnprintf(e.message, MESSAGE_MAX, format, args);
    std::strncpy(e.component, component, COMPONENT_MAX - 1);
    e.component[COMPONENT_MAX - 1] = '\0';
    e.severity     = severity;
    e.timestamp_us = nowMicros();
    // Publikuj az kdyz je cely entry zapsany — release zaruci, ze ctenar,
    // ktery uvidi novy write index (acquire), uvidi i kompletni data entry.
    rt_write_idx_.store(w + 1, std::memory_order_release);
}

int Logger::flushRTBuffer() {
    std::lock_guard<std::mutex> lk(log_mutex_);
    size_t r = rt_read_idx_.load(std::memory_order_relaxed);
    const size_t w = rt_write_idx_.load(std::memory_order_acquire);
    int flushed = 0;
    while (r < w) {
        Entry& e = rt_buffer_[r % RT_BUFFER_SIZE];
        writeEntry(e.component, e.severity, e.message, e.timestamp_us);
        ++r;
        ++flushed;
    }
    rt_read_idx_.store(r, std::memory_order_release);
    return flushed;
}

} // namespace ithaca::log
