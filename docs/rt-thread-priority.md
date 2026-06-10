# RT priorita audio vlakna — napric platformami

> Status: **implementovano** (`engine/util/rt_priority.{h,cpp}` + hook
> v `Engine::processBlock`; viz Pull-time briefing nize). Od vetve
> `fix/revize-2026-06-10` je RT priorita **opt-in pres `EngineConfig::rt_priority`**
> (default `false`): zapinaji ji jen realne audio aplikace — GUI
> (`app_context.cpp`) a CLI `--play`. Testy a offline batch render RT prioritu
> NEDOSTAVAJI (SCHED_FIFO na main threadu by hrozil vyhladovenim systemu /
> RLIMIT_RTTIME killem). Puvodni motivace: pozorovani na Windows
> (main, commit `cd53292`): DSP LOAD osciluje 40–120 % i bez aktivniho hrani —
> indicator pocita `render_us / block_period_us`, pri konstantnim mnozstvi prace
> kolisani znamena, ze kolisa **rychlost CPU nebo dostupnost casu**. Bezne priciny
> jsou P-states (Turbo Boost ramp), HT sibling interference, WASAPI shared-mode
> jitter a chybejici MMCSS task registrace. Resi to RT priorita audio vlakna +
> platformove specificka registrace u OS scheduleru.

## Cil

1. Audio vlakno (`Engine::processBlock` invocace) ma na vsech platformach
   zaruceny CPU slot s vyssi prioritou nez bezne user-space vlakna.
2. OS scheduler ho nepreemptuje pri kompetici s GUI / system tasks po dobu jednoho
   bloku.
3. Implementace je **idempotentni**, **soft-failure** (warning bez crash, fallback
   na default prioritu), **per-thread** (analogicky s `enableFlushDenormals`).

## Spolecne rozhrani

Pattern presne kopiruje `engine/util/denormals.h`:

```cpp
// engine/util/rt_priority.h (implementovany stav)
namespace ithaca {

// Nastavi RT prioritu / kernel-level RT politiku na aktualnim vlakne.
// Idempotentni — pri opakovanem volani zustane RT zapnute.
// Nezhazi proces — caller loguje a pokracuje na default priorite.
//
// Parametry pripadne tunuji kernel hint (macOS time_constraint, Linux rtprio
// hodnotu) podle realne periody audio bloku.
enum class RtAudioStatus {
    Full    = 0,   // ok
    Partial = 1,   // castecny — primary API ok, sekundarni (MMCSS) ne
    Failed  = 2,   // primary API selhalo, default scheduling
};
struct RtAudioParams {
    int sample_rate;   // napr. 48000
    int block_size;    // napr. 256
};
// err_code je out param (errno / GetLastError / kern_return_t; smi byt nullptr).
RtAudioStatus enableRealtimeAudio(const RtAudioParams& p, int* err_code) noexcept;

// Vrati ven OS zdroje (pouze Windows MMCSS task handle, jinde no-op).
// Volat pri shutdown audio vlakna; nevolat z RT.
void disableRealtimeAudio() noexcept;

} // namespace ithaca
```

Pouziti v `Engine::processBlock` (analog k existujicimu FTZ/DAZ guardu) — guard
je **opt-in pres `cfg_.rt_priority`** (default `false`; GUI a CLI `--play`
nastavuji `true`):

```cpp
static thread_local bool rt_set = false;
if (!rt_set && cfg_.rt_priority) {
    const RtAudioParams rp{ cfg_.sample_rate, cfg_.block_size };
    int err = 0;
    switch (enableRealtimeAudio(rp, &err)) {
        case RtAudioStatus::Full:
            LOG_RT_INFO("audio", "RT priorita aktivni (sr=%d block=%d)", ...);
            break;
        case RtAudioStatus::Partial:   // Win: TIME_CRITICAL ano, MMCSS ne
            LOG_RT_INFO("audio", "RT priorita castecna ... (err=%d)", err);
            break;
        case RtAudioStatus::Failed:
            LOG_RT_WARN("audio", "RT priorita selhala (err=%d) — default "
                        "scheduling, jitter risk", err);
            // + per-platform LOG_RT_INFO TIP (limits.conf / secpol / sandbox)
            break;
    }
    rt_set = true;
}
```

Symetricky cleanup volat v destruktoru `AudioDevice` nebo pres registraci
`std::atexit` v ramci audio-thread-init wrapperu.

## Logovaci kontrakt

Projekt ma dvojicovy logger (`engine/util/log.h`): non-RT (`LOG_INFO/WARN/...`,
mutex + printf) a RT-safe (`LOG_RT_INFO/WARN/...`, SPSC ring buffer, zadne
alokace). Plati pravidla:

- **`enableRealtimeAudio`** je volana z **audio threadu** (per-thread init pri
  prvnim `processBlock`) → loguje pres **`LOG_RT_*`**, ne `LOG_*`. Strncpy do
  ring bufferu je RT-safe, varargs formatovani uvnitr loggeru ma zarucenou
  hranici (`MESSAGE_MAX=256` viz `log.h:113`).
- **Component label** = `"audio"` (zarazuje to do stejneho proudu jako
  underrun warningy a DSP load events; konzistentni s `LOG_RT_WARN("audio", ...)`
  cestami v `Engine::processBlock` jinde).
- **Severity zvolba:**
  - `LOG_RT_INFO("audio", "RT priorita aktivni (sr=%d block=%d)", ...)` — pri
    uspechu, jednou per thread (thread_local guard). Volitelne — silent uspech
    je take prijatelny styl (denormals.h to dela tak). Doporucujem **log INFO**,
    aby slo overit v logu, ze RT skutecne sedlo.
  - `LOG_RT_WARN("audio", "RT priorita selhala (errno=%d) — default scheduling", err)`
    — pri selhani API. Vcetne errno / KERN_ status / `GetLastError()` aby slo
    diagnostikovat. Nevolat ERROR/FATAL — audio porad bezi.
- **`disableRealtimeAudio`** je volana z **non-RT** (shutdown cesta, mimo audio
  callback) → loguje pres `LOG_INFO` / `LOG_DEBUG`. Pri uspechu silent;
  pri unexpected stavu `LOG_WARN("audio", "MMCSS revert selhal")`.
- **Per-platform implementace** uvnitr `rt_priority.cpp` pouziva pouze
  predane error kody (errno/KERN/HRESULT) a vraci `bool`; samotny log dela
  **caller** v `processBlock`. To drzi `rt_priority.cpp` bez zavislosti na
  loggeru a umozni unit-volat ho i z testu bez `log::Logger::default_()`.
  *(Vyjimka by byla, pokud bys chtel zlogovat dilcji warning per-platform —
  napr. "MMCSS OK ale SetThreadPriority selhal" — pak pridat callback parametr
  do `enableRealtimeAudio` neci pouzit interni LOG_RT primo. Zacit s
  jednoduchym bool returnem.)*

Priklad ocekavaneho logu pri startu (z `flushRTBuffer` outputu):

```
[14:32:17.041] [audio] [INFO]: RT priorita aktivni (sr=48000 block=256)
[14:32:17.062] [resonance] [INFO]: active rezonance = 0 (cc64=0)
```

Pripadny warning:

```
[14:32:17.041] [audio] [WARNING]: RT priorita selhala (errno=1) — default scheduling
```

## Navodne hlasky pred fallback (uzivatelska self-help diagnostika)

**Pravidlo:** kdyz API selze a aplikace prejde na suboptimalni cestu (vyssi
jitter), zaloguj **INFO** zpravu hned PRED fallbackem s **konkretnim navodem**,
co uzivatel muze udelat. Cil: aby uzivatel videl bez ctenia zdrojaku, proc je
audio glitchy a jak to opravit.

Severity = **INFO** (ne WARN): nejde o chybu (audio bezi), je to "k zlepseni".
Component = `"audio"`. Pouzit `LOG_RT_INFO` (audio thread context).

### Linux — chybi RT permissions

```
[..] [audio] [INFO]: pthread_setschedparam EPERM — RT scheduling neni dostupny
[..] [audio] [INFO]: TIP: pridej do /etc/security/limits.conf:
[..] [audio] [INFO]:   @audio - rtprio 99
[..] [audio] [INFO]:   @audio - memlock unlimited
[..] [audio] [INFO]: a userem do skupiny 'audio' (gpasswd -a $USER audio),
[..] [audio] [INFO]: pak relogin. Bez toho audio bezi na default scheduling.
```

### Linux — chybi PREEMPT_RT (extra tip, jen LOG_DEBUG)

PREEMPT_RT kernel neni podminka — `SCHED_FIFO` na stock kernelu staci pro nase
nizko-latencni cilovky. Kontrolovat ho jen pri vyhlubsi diagnostice. **DEBUG
severity** (nezaplavovat default log):

```
[..] [audio] [DEBUG]: kernel: 6.5.0-generic (non-PREEMPT_RT)
[..] [audio] [DEBUG]: TIP: pro audio production setup zvaz PREEMPT_RT kernel
[..] [audio] [DEBUG]:      (Ubuntu Studio / RT patched kernel). Stock kernel
[..] [audio] [DEBUG]:      ma jitter ~1-2 ms, RT kernel ~50-200 us.
```

Detekce: `uname -r` z `<sys/utsname.h>`, look for "rt" substring nebo
`PREEMPT_RT` v `/proc/version`. Volitelna — zacit bez teto kontroly.

### Linux — chybi RTKit (RealtimeKit D-Bus)

Pri fallbacku, pokud jsme zkousili RTKit a nepripojili se:

```
[..] [audio] [INFO]: RTKit (org.freedesktop.RealtimeKit1) nedostupny
[..] [audio] [INFO]: TIP: bud nainstaluj 'rtkit' balicek (apt install rtkit /
[..] [audio] [INFO]:      dnf install rtkit), nebo nastav limits.conf rucne
[..] [audio] [INFO]:      (viz vyssi instrukce). Bez RT scheduling bude jitter.
```

### macOS — `thread_policy_set` selhalo

Vzacne (sandbox bez entitlement). Pokud nase distribuce neni Mac App Store,
nemelo by se stat. Pri selhani:

```
[..] [audio] [INFO]: thread_policy_set KERN_FAILURE — time_constraint nedostupny
[..] [audio] [INFO]: TIP: pravdepodobne sandbox bez audio entitlement.
[..] [audio] [INFO]:      Spousti se aplikace mimo App Sandbox? Pokud ji
[..] [audio] [INFO]:      distribujes podepsanou s entitlements, pridej
[..] [audio] [INFO]:      com.apple.security.temporary-exception.audio-unit-host.
```

### Windows — chybi `avrt.dll` (extreme edge case)

Server Core, WINE, custom Windows obrazy. Pri selhani `LoadLibrary("avrt.dll")`
nebo `AvSetMmThreadCharacteristicsW` vrati NULL:

```
[..] [audio] [INFO]: MMCSS 'Pro Audio' task registrace selhala (GetLastError=%lu)
[..] [audio] [INFO]: TIP: zkontroluj ze 'Multimedia Class Scheduler' service
[..] [audio] [INFO]:      bezi (services.msc -> MMCSS). Bez ni audio thread
[..] [audio] [INFO]:      dostava fair share -> jitter pri vetsi systemove
[..] [audio] [INFO]:      zatezi. SetThreadPriority zustava aktivni.
```

### Windows — `SetThreadPriority` selhalo

Pseudo-nemozne na normalnich Windows. Pri selhani logovat raw error a navod
na user-side check:

```
[..] [audio] [INFO]: SetThreadPriority(TIME_CRITICAL) selhalo (GetLastError=%lu)
[..] [audio] [INFO]: TIP: GPO ci 3rd-party security software omezuje thread
[..] [audio] [INFO]:      priorities. Zkontroluj 'Increase scheduling priority'
[..] [audio] [INFO]:      privilege (secpol.msc -> Local Policies -> User Rights).
```

### Princip "navod = jeden text"

Zadny `LOG_INFO` se navodem nesmi krizove referencovat ostatni navody nebo
ocekavat sekvencni cteni — kazdy je samostatne actionable. Uzivatel vidi log
napr. v `flushRTBuffer` outputu, v `--log-level info` CLI provozu, nebo v
GUI log strip panelu (subscriber API).

### Anti-patterny v navodech

- **NE-** "kontaktuj support". Tohle je single-uzivatelska aplikace, support
  je ctena dokumentace.
- **NE-** dlouhe URL. Pokud potreba odkaz, dej zkracene host: "viz docs/
  rt-thread-priority.md sekce Linux" — Markdown clickable v IDE.
- **NE-** alarmistic "VAROVANI" pri INFO. Audio bezi. Zachovat tonalitu:
  "TIP: ..." je presnejsi nez "WARNING: ...".
- **ANO** vlozit konkretni prikaz / cestu / parametr, kterou ma uzivatel
  zkontrolovat — ne abstraktni doporuceni.

---

## macOS — Mach `THREAD_TIME_CONSTRAINT_POLICY`

### API

```cpp
#include <mach/mach.h>
#include <mach/thread_policy.h>
#include <mach/mach_time.h>
```

### Princip

CoreAudio audio thread si tuto politiku nastavi automaticky (pres CoreAudio HAL).
Kdyz pouzivame **vlastni audio thread** (miniaudio over CoreAudio HAL kontextu),
musime tuto registraci provest sami. Politika rika kernelu: *potrebuju X cyklu
kazdych Y casovych jednotek, hard deadline Z*; scheduler pak garantuje slot a
neda nam ho odebrat na bezne pre-emption hranici.

### Implementace

```cpp
#if defined(__APPLE__)
mach_timebase_info_data_t tb;
mach_timebase_info(&tb);                       // ticks-to-ns ratio

const double period_ns = (double)p.block_size * 1e9 / (double)p.sample_rate;
// Period v Mach ticks (abs time units):
const uint64_t period_abs = (uint64_t)(period_ns * tb.denom / tb.numer);

thread_time_constraint_policy_data_t pol;
pol.period      = (uint32_t)period_abs;
pol.computation = (uint32_t)(period_abs / 2);   // poviname pulku bloku
pol.constraint  = (uint32_t)period_abs;          // hard deadline = period
pol.preemptible = 0;                             // ne-preemptible v ramci slotu

kern_return_t kr = thread_policy_set(
    mach_thread_self(),
    THREAD_TIME_CONSTRAINT_POLICY,
    (thread_policy_t)&pol,
    THREAD_TIME_CONSTRAINT_POLICY_COUNT);
return kr == KERN_SUCCESS;
#endif
```

### Cleanup

No-op. Politika padne s vlaknem.

### Failure mody

- `KERN_NOT_SUPPORTED` na neznamem kernelu → log warning, return false.
- Sandbox restrikce (Mac App Store apps): time_constraint nelze nastavit bez
  entitlement. Pro nase pouziti (vlastni distribuce) bezne nehrozi.

### Alternativa

POSIX `pthread_setschedparam(SCHED_FIFO, prio=80)` taky funguje a je
jednodussi, ale **time_constraint je preferovany** — explicitne modeluje
audio deadline a Apple ho doporucuje pro audio v dokumentaci.

---

## Linux — POSIX `SCHED_FIFO` + RealtimeKit fallback

### API

```cpp
#include <pthread.h>
#include <sched.h>
#include <sys/resource.h>
```

### Princip

Linux RT scheduling je standardni POSIX: `SCHED_FIFO` policy + RT priorita 1–99.
Audio konvence: 70–90 (nez 99, aby RT watchdog kernelu nikdy nebyl unable to run).

**Hacky:**

1. Vyzaduje `CAP_SYS_NICE` nebo limit v `/etc/security/limits.conf`:
   ```
   @audio - rtprio 99
   @audio - memlock unlimited
   ```
   Bez toho `pthread_setschedparam` vraci `EPERM`.

2. Moderni distribuce (Ubuntu/Fedora s PipeWire/PulseAudio) reseji to bez root
   pres **RealtimeKit (RTKit)** — D-Bus daemon `org.freedesktop.RealtimeKit1`,
   ktery bezi jako root a grantuje RT prioritu uzivatelskym procesum po validaci
   (proces nesmi byt uplne neresponzivni atd.).

### Implementace

```cpp
#if defined(__linux__)
struct sched_param sp;
sp.sched_priority = 80;
int err = pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);
if (err == 0) return true;

// Fallback: zkus RealtimeKit pres D-Bus.
// Pseudokod — vyzaduje libdbus nebo librtkit.
if (rtkit_make_realtime(pthread_self(), 80) == 0) return true;

// Posledni fallback: alespon nice priorita (best effort, ne RT).
errno = 0;
int nice_ret = setpriority(PRIO_PROCESS, 0, -10);
return (nice_ret == 0 && errno == 0);   // nice ne-RT, ale nez nic
#endif
```

### Cleanup

No-op (RT policy padne s vlaknem).

### Failure mody

- `EPERM` bez RTKit: log warning, fallback `setpriority(-10)` jako konzolacni
  bonus. Audio porad funguje, jen s vyssi jitter.
- RPi5 cilovka: root muze nastavit RT primo; bez root je nutny `limits.conf`
  setup nebo systemd `LimitRTPRIO=99`. PREEMPT_RT kernel je idealni, ale ne
  podminka.

### Zavislosti

RTKit fallback vyzaduje `libdbus-1` nebo `librtkit`. Pro RPi5 jako standalone
audio aplikaci RTKit obvykle neni potreba (instalujeme s vhodnym sudo / capability
setupem). Pro desktop Linux je RTKit vhodny default.

**Doporuceni:** zacit *bez* RTKit (jen `pthread_setschedparam` + nice fallback),
pridat RTKit teprve pokud uzivatel hlasi problem na desktop distru bez audio
limits config.

---

## Windows — `SetThreadPriority` + MMCSS "Pro Audio"

### API

```cpp
#include <windows.h>
#include <avrt.h>          // MMCSS API
// link: avrt.lib
```

### Princip

Dva sloupce:

1. **`SetThreadPriority(THREAD_PRIORITY_TIME_CRITICAL)`** — zvedne thread priority
   na nejvyssi user-space hodnotu (16). Pouhe tohle ale nestaci: Windows
   scheduler stejne audio thready preemptne kvuli "fairness" jinych user-space
   tasku, pokud system veri, ze audio nema specialni narok.

2. **MMCSS task "Pro Audio"** (`AvSetMmThreadCharacteristicsW`) — zaregistruje
   thread jako audio worker u Multimedia Class Scheduler Service. Kernel pak audio
   threadu garantuje typicky 80 % kvanta i pod tezkym multitaskem (browser,
   compile, antivirus scan). Bez teto registrace dostane audio thread jen "fair
   share", coz vede k nepredvidatelnym preempci a jitteru.

### Implementace

```cpp
#if defined(_WIN32)
static thread_local HANDLE g_mmcss_handle = nullptr;
static thread_local DWORD  g_mmcss_task   = 0;

bool enableRealtimeAudio(const RtAudioParams& p) noexcept {
    (void)p;   // Windows MMCSS si dela vlastni rozhodnuti

    if (!SetThreadPriority(GetCurrentThread(),
                           THREAD_PRIORITY_TIME_CRITICAL)) {
        return false;
    }

    DWORD task_index = 0;
    HANDLE h = AvSetMmThreadCharacteristicsW(L"Pro Audio", &task_index);
    if (h == nullptr) {
        // SetThreadPriority prosel, MMCSS ne — ulozit aspon to.
        // (Stary Windows bez avrt.dll. Nebo non-Windows-audio session.)
        return true;
    }
    g_mmcss_handle = h;
    g_mmcss_task   = task_index;
    return true;
}

void disableRealtimeAudio() noexcept {
    if (g_mmcss_handle) {
        AvRevertMmThreadCharacteristics(g_mmcss_handle);
        g_mmcss_handle = nullptr;
        g_mmcss_task   = 0;
    }
}
#endif
```

### Cleanup

`AvRevertMmThreadCharacteristics(handle)` z `disableRealtimeAudio()`. Pokud
zapomeneme, MMCSS handle se uvolni pri exitu procesu — ne katastrofa, ale styl.

### Failure mody

- `AvSetMmThreadCharacteristicsW` selze na strange Windows variantach (Server
  Core bez audio session, WINE) → fallback jen na `SetThreadPriority`. Audio
  porad bezi, jen vice jitteru.
- `SetThreadPriority` selze typicky jen pri zlomenem handle — neocekavame.

### CMake

Pridat `avrt` do link libraries pro `ithaca_core` v MSVC branchi:

```cmake
elseif(WIN32)
    target_link_libraries(ithaca_core PUBLIC winmm avrt)
```

(`winmm` uz tam je pro RtMidi.)

---

## Testovaci plan

Per-platforma overit:

| Platforma | Co overit | Jak |
|-----------|-----------|-----|
| Windows | DSP LOAD prestane oscilovat 40–120 % bez hrani | spustit `ithaca-gui` v klidu, sledovat DSP LOAD dlazdici 30 s. Cíl: variabilita < 10 %. |
| Windows | MMCSS handle se uvolni pri shutdown | Process Explorer → check zadne stale audio handles. |
| Windows | Fallback bez MMCSS funguje | docasne stub `AvSetMmThreadCharacteristicsW` na NULL — overit ze audio bezi a log hlasi warning. |
| macOS | Audio thread ma `THREAD_TIME_CONSTRAINT_POLICY` | `top -F -R -o cpu` ; nebo `Instruments → Time Profiler` → audio thread ma "Realtime" scheduler class. |
| macOS | Pri high system load (zip celeho repa) audio nepustuje | spustit kompresi v pozadi, sledovat dropouts indicator. |
| Linux | `pthread_setschedparam` uspeje (root) nebo loguje EPERM (uzivatel) | spustit `ithaca-cli --selftest` pres root i bez nej, overit log. |
| Linux | Fallback `setpriority(-10)` se uplatni | spustit bez RT permissions, overit log + nice value `ps -o pri,ni,cmd`. |
| RPi5 | RT priorita zlepsi jitter pri mereni | testovat dropouts s & bez RT. Postup setupu RPi5 viz [`docs/raspberry-pi-5.md`](raspberry-pi-5.md). |

Unit testy nepiseme — RT scheduling se nedetektuje deterministicky v testu, jen
integration overenim ze API vraci uspech a `enableRealtimeAudio` nezhazi proces.

---

## Implementacni kroky (cekuji navazne PRy)

1. **Hlavicka + cross-platform fasada** — `engine/util/rt_priority.h` + impl
   v jednom souboru `engine/util/rt_priority.cpp` s `#ifdef` vetvemi.
   Hook do `Engine::processBlock` per-thread guard (analog FTZ/DAZ).
   ~120 radku LOC.

2. **CMake** — pridat `avrt` link na Windows; nic noveho na macOS/Linux.

3. **Smoke test per-platform** — manualni QA podle tabulky vyse + log
   warning hlasky pri failu.

4. **Dokumentace** — preklopit relevantni casti do `docs/reference/I-multithreading.md`
   (audio vlakno radek "Co dela") jakmile implementace existuje.

5. **(Volitelne, druha vlna)** RTKit integrace pro Linux desktop distribuce.
   Bez ni potrebujeme `limits.conf` setup. S ni out-of-the-box i pro
   neprivilegovaneho usera.

---

## Reference

- macOS: [Apple Threading Programming Guide § Real-Time Threads](https://developer.apple.com/library/archive/documentation/Darwin/Conceptual/KernelProgramming/scheduler/scheduler.html)
- Linux: `man 7 sched`, `man 2 sched_setscheduler`
- Linux RTKit: [freedesktop RealtimeKit spec](https://www.freedesktop.org/wiki/Software/RealtimeKit/)
- Windows MMCSS: [Multimedia Class Scheduler Service](https://learn.microsoft.com/en-us/windows/win32/procthread/multimedia-class-scheduler-service)
- Audio threading best practices: [Ross Bencina — *Real-Time Audio Programming 101*](http://www.rossbencina.com/code/real-time-audio-programming-101-time-waits-for-nothing)

---

## Pull-time briefing — commit `f751c4c`

Tato sekce shrnuje konkretni nasazeni navrhu vyse jako referenci pri checkoutu
na druhem prostredi (macOS/Linux), kdyz feature pulluje uzivatel, ktery ji
nepsal a chce vedet co testovat / instalovat bez ctenia diffu.

### Co se pridalo

| Soubor | Ucel |
|--------|------|
| `engine/util/rt_priority.h` | Cross-platform fasada (enum `RtAudioStatus`, struct `RtAudioParams`, `enableRealtimeAudio`, `disableRealtimeAudio`) |
| `engine/util/rt_priority.cpp` | Per-platform impl s `#ifdef _WIN32 / __APPLE__ / __linux__` vetvemi |
| `docs/rt-thread-priority.md` | Tato specifikace |
| `CMakeLists.txt` | +5 radku: `engine/util/rt_priority.cpp` mezi sources, `avrt` link na Windows |
| `engine/engine.cpp` | +50 radku: include + `thread_local` guard v `processBlock` po FTZ/DAZ + per-platform TIP logy pri failu |

### Per-platform chovani po pullu

**Windows** (overeno na MSVC 19.44 / Windows 11):

- Default cesta = **Full** status: `SetThreadPriority(TIME_CRITICAL)` + MMCSS task "Pro Audio"
- Default Win11 instalace ma MMCSS service zapnutou → bez konfigurace funguje
- Log pri startu: `[audio] [INFO]: RT priorita aktivni (sr=48000 block=256)`
- Buildi pres `avrt.lib` (linkly v CMakeLists.txt do `elseif(WIN32)` vetve)
- Pokud nekde uvidis **Partial** log + TIP o MMCSS → MMCSS service je vypnuta;
  `services.msc → Multimedia Class Scheduler → Start`

**macOS** (neovereno na M-series — chybi target machine):

- Cesta: `thread_policy_set(THREAD_TIME_CONSTRAINT_POLICY)` s `period = block_size * 1e9 / sample_rate` prevedene na Mach abs time pres `mach_timebase_info`
- `computation = period / 2`, `constraint = period`, `preemptible = 0`
- Zadny extra link — `CoreAudio/AudioToolbox/CoreFoundation` jsou uz v `if(APPLE)` vetvi
- Po pullu na M1/M2/M3: `make build` by melo projit bez modifikaci CMake. Pokud `mach/thread_policy.h` chybi v include path, zkontroluj `xcode-select --install`
- Ocekavany log: `[audio] [INFO]: RT priorita aktivni (sr=48000 block=256)` (Full)
- Kdyz selze (sandbox bez audio entitlement): TIP o `com.apple.security.temporary-exception.audio-unit-host` — relevantni jen pro App Store distribuci, ne pro lokalni buildy

**Linux** (neovereno — chybi target machine):

- Cesta: `pthread_setschedparam(SCHED_FIFO, prio=80)`
- Bez noveho CMake linku (POSIX, `Threads::Threads` uz je v `elseif(UNIX)`)
- **Pozor — vyzaduje permissions:**
  - Bud spustit jako root (RPi5 native setup)
  - Nebo doplnit `/etc/security/limits.conf`:
    ```
    @audio - rtprio 99
    @audio - memlock unlimited
    ```
    a `gpasswd -a $USER audio` + relogin
- Bez permissions: log ukaze `[audio] [WARNING]: RT priorita selhala (err=1) — default scheduling` + TIP s temi limits.conf radky (presny TIP je v `engine/engine.cpp` v `#elif defined(__linux__)` vetvi)
- RTKit fallback **neni zatim implementovany** — viz "Implementacni kroky" v teto specifikaci jako druha vlna pro desktop distra

### Jak overit po pullu

1. **Kompilace:** `make build` — mela by projit beze zmeny CMake na macOS/Linux. Na Windows overeno.
2. **Test suita:** `ctest --test-dir build -C Release` — zadny novy test (RT scheduling neni deterministicky testovatelny), ale stavajicich 33 musi projit.
3. **Smoke (CLI):** `./build/Release/ithaca-cli --selftest` — probehne bez audio device, nezobrazi RT log.
4. **GUI smoke s RT logem:**

   ```bash
   ./build/Release/ithaca-gui --bank-dir <cesta> --log-level info
   ```

   V prvnich ~3 vterinach po startu vidis v stdoutu jednu ze tri hlasek (`Full` / `Partial` / `Failed`). To je primarni signal, ze RT init probehl.

5. **Realny impact** se meri DSP LOAD dlazdici v indicator stripu pri hrani + v klidu. Bez RT na Windows kolisal 40–120 %, po RT je load nizsi a stabilnejsi bez overload eventu (overeno).

### Co kdyby na macOS/Linux neslo zkompilovat

Pouzite headery:

- macOS: `<mach/mach.h>`, `<mach/mach_init.h>`, `<mach/thread_act.h>`, `<mach/thread_policy.h>`, `<mach/mach_time.h>` — vse soucast macOS SDK (`xcode-select`)
- Linux: `<pthread.h>`, `<sched.h>`, `<cerrno>` — POSIX, standardni

Pri problemech zkontrolovat sekci konkretni platformy vyse — popisuje API a alternativy (POSIX `SCHED_FIFO` fallback na macOS, RTKit na Linuxu).

### Co rozhodne NEZMENI chovani

- Pokud RT API selze, status = `Failed`, audio se chova **presne jako pred timto commitem** (default scheduling). Zadna regrese.
- `disableRealtimeAudio()` je no-op vsude krome Windows. Neni kriticke volat pri shutdown — handle se uvolni s procesem. Ale pattern rika explicit cleanup.
