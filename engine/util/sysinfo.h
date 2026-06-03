#pragma once
// engine/util/sysinfo.h
// ---------------------
// Drobne platformove dotazy na HW. Zatim jen celkova fyzicka RAM — pouziva se
// pro AUTO RAM budget pri nacitani banky (ochrana pred OOM na embedded, napr.
// Raspberry Pi 5 / 4 GB). Header-only.

#include <cstddef>
#include <cstdint>

#if defined(__linux__)
  #include <fstream>
  #include <string>
#elif defined(__APPLE__)
  #include <sys/sysctl.h>
#elif defined(_WIN32)
  #include <windows.h>
#endif

namespace ithaca {

// Celkova fyzicka RAM v bytech; 0 = nezname (volajici pak budget vypne).
inline std::size_t systemTotalRamBytes() {
#if defined(__linux__)
    std::ifstream f("/proc/meminfo");
    std::string key;
    long kb = 0;
    while (f >> key) {
        if (key == "MemTotal:") { f >> kb; break; }
        std::string rest;
        std::getline(f, rest);
    }
    return kb > 0 ? (std::size_t)kb * 1024 : 0;
#elif defined(__APPLE__)
    uint64_t mem = 0;
    std::size_t len = sizeof(mem);
    if (sysctlbyname("hw.memsize", &mem, &len, nullptr, 0) == 0) return (std::size_t)mem;
    return 0;
#elif defined(_WIN32)
    MEMORYSTATUSEX st;
    st.dwLength = sizeof(st);
    if (GlobalMemoryStatusEx(&st)) return (std::size_t)st.ullTotalPhys;
    return 0;
#else
    return 0;
#endif
}

} // namespace ithaca
