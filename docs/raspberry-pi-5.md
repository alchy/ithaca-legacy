# Raspberry Pi 5 — kompletni setup pro ithaca-legacy

> Kompletni postup od cisteho SD karty / NVMe po prvni hrani s ithaca-gui.
> Vychazi z toho, ze RPi5 je explicit target projektu (`docs/config-file.md`
> zminuje RPi pri `cache_budget_mb` ochrane, ARM tuning je v `CMakeLists.txt`
> radek 41, RT priorita ma RPi-specificke instrukce v
> `docs/rt-thread-priority.md`).

## Obsah

1. [Hardware](#1-hardware)
2. [Instalace OS](#2-instalace-os)
3. [System config (config.txt + governor + swap)](#3-system-config)
4. [User a RT permissions](#4-user-a-rt-permissions)
5. [Audio HAT / DAC setup](#5-audio-hat--dac-setup)
6. [Build dependencies + clone + build](#6-build-dependencies--clone--build)
7. [Projektova konfigurace (config.json)](#7-projektova-konfigurace-configjson)
8. [Prvni spusteni a verifikace](#8-prvni-spusteni-a-verifikace)
9. [CPU affinity (advanced, volitelne)](#9-cpu-affinity-advanced-volitelne)
10. [Troubleshooting](#10-troubleshooting)

---

## 1) Hardware

| Komponenta | Doporuceni | Proc |
|---|---|---|
| **Board** | RPi 5 (**8 GB**) | 4× Cortex-A76 @ 2.4 GHz + NEON 4-wide SIMD; 8 GB pro `cache_budget` ~4.5 GB + headroom. 4 GB stale funkcni, ale male banky a vyssi `cache_budget_mb` clamping. |
| **Storage** | **NVMe SSD pres PCIe HAT** (Pimoroni NVMe Base, Geekworm X1001, Pineboards HatDrive!) — 256-1000 GB | Sample streaming chce nizkou random read latenci. NVMe ~3000 MB/s vs micro-SD A2 ~100 MB/s. |
| **Alt. storage** | USB 3.0 SSD + USB-SATA adapter | Lacinejsi fallback, ~400 MB/s, taky staci. |
| **Boot media** | micro-SD (A2 class) pro bootloader, NVMe pro rootfs (od Bookworm `rpi-eeprom` umoznuje boot primo z NVMe) | Bezpecne pri prvnich pokusech: boot z SD, instalace, az pak migrace na NVMe. |
| **Audio out** | **I2S DAC HAT** (HiFiBerry DAC+/DAC2 Pro, IQaudio DAC Pro, Pimoroni Audio DAC SHIM) | Nizka latence, bit-exact 24/48 nebo 24/96, zadny USB jitter. |
| **Alt. audio** | USB Class Compliant DAC (Topping E30 II, Schiit Modi 3+, Behringer UCA222) | Plug-and-play pres ALSA. |
| **NE** | Onboard 3.5mm PWM audio na RPi5 | Horsi kvalita nez na RPi4 (PWM-only); pro audio playing nepouzitelne. |
| **Napajeni** | **Oficialni 27W USB-C PSU** | RPi5 + NVMe + audio HAT zere ~12W pod zatezi; underpower → CPU throttle = audio dropouts. Tretostranne PSU jsou casto pod-spec. |
| **Chlazeni** | **Oficialni Active Cooler** nebo case s ventilatorem (Argon NEO 5, FLIRC) | A76 + 100 % load = >80 °C → thermal throttle do 1.5 GHz → audio se rozpadne. |
| **MIDI in** | USB MIDI klavir (priamo) **nebo** USB → DIN5 MIDI adapter (M-Audio Uno, Roland UM-ONE) | RtMidi to chyta pres ALSA Sequencer (`__LINUX_ALSA__` define v `third-party/CMakeLists.txt`). |

---

## 2) Instalace OS

**Doporuceny:** Raspberry Pi OS **64-bit Bookworm** (Debian 12 base).

- **Lite verze** pokud RPi pobezi jako audio module bez monitoru (deploy mode)
- **Standard** (s desktop) pokud chces buildit + GUI dev primo na RPi

### Postup

1. Stahnout **Raspberry Pi Imager** (Win/macOS/Linux): https://www.raspberrypi.com/software/
2. Vlozit SD kartu (>= 16 GB) nebo NVMe via USB-NVMe enclosure
3. V Imageru: vybrat **Raspberry Pi 5** → **Raspberry Pi OS (64-bit)** → SD/NVMe
4. **Pred zapisem** kliknout na ozubene kolecko (advanced):
   - Hostname: `ithaca-pi` (nebo cokoliv)
   - Enable SSH: ano (klic nebo password)
   - User: vytvorit (NE `pi` — Bookworm default)
   - Wi-Fi nebo Ethernet config
   - Locale: `cs_CZ.UTF-8` nebo `en_US.UTF-8`, timezone `Europe/Prague`
5. Write → wait → eject → boot RPi
6. Pres SSH se pripojit: `ssh user@ithaca-pi.local`
7. Update: `sudo apt update && sudo apt full-upgrade -y && sudo reboot`

### Boot z NVMe (volitelne, doporucene)

Pri rychle storage je rozdil oproti SD obrovsky pri load bance. Pri prvnim
spusteni z SD aktualizovat EEPROM:

```bash
sudo rpi-eeprom-update -a
sudo reboot
sudo raspi-config nonint do_boot_order B2   # NVMe first
```

Pak naklonovat SD → NVMe pres `rpi-clone` nebo `dd`, pak boot z NVMe.

---

## 3) System config

### `/boot/firmware/config.txt`

Pridej na konec:

```
# --- ithaca-legacy: audio + perf ---

# I2S DAC HAT (priklad pro HiFiBerry; pro jine HATy viz jejich dokumentaci):
dtoverlay=hifiberry-dacplus

# Vypnout onboard PWM audio (koliduje s I2S routou + zbytecna spotreba):
dtparam=audio=off

# Maximalni boost (RPi5 A76 staticky na 2.4 GHz; bez boostu nizsi):
arm_boost=1

# NVMe na PCIe HAT (pokud relevantni):
dtparam=nvme
dtparam=pciex1_gen=3
```

Po editaci reboot.

### CPU governor: performance (kriticke)

Default `ondemand` governor downclockuje CPU pri male zatezi (analog Windows
P-states). Audio thread tim dostava nizsi clock → kolisajici wall-time render.
Fix: trvale **performance** governor.

**One-shot test:**

```bash
sudo cpufreq-set -g performance
# overit:
cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor   # → "performance"
```

**Trvale** pres systemd service:

```bash
sudo tee /etc/systemd/system/cpu-performance.service <<'EOF'
[Unit]
Description=Set CPU governor to performance
After=multi-user.target

[Service]
Type=oneshot
ExecStart=/bin/sh -c 'echo performance | tee /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor'
RemainAfterExit=yes

[Install]
WantedBy=multi-user.target
EOF

sudo systemctl daemon-reload
sudo systemctl enable --now cpu-performance.service
```

### Swap off

Audio + RT priority + `mlock` neprej s page-out. Swap vypnout:

```bash
sudo systemctl disable dphys-swapfile
sudo swapoff -a
```

Overit: `free -h` ma Swap radek nulovy.

### Disable wireless (volitelne, pro nejnizsi jitter)

Pokud nepotrebujes Wi-Fi/Bluetooth a jedes pres ethernet:

```bash
echo 'dtoverlay=disable-wifi'      | sudo tee -a /boot/firmware/config.txt
echo 'dtoverlay=disable-bt'        | sudo tee -a /boot/firmware/config.txt
```

(Interrupts z bezpecnejsiho wireless driveru pridavaji ~50-200 μs jitteru.)

---

## 4) User a RT permissions

### `audio` group

Bookworm uz `audio` group standardne ma. Pridej do ni svuj user:

```bash
sudo gpasswd -a $USER audio
# overit:
groups   # mel by obsahovat "audio"
```

Pak **relogin** (vyjit ze SSH a znovu prihlasit), groups se promitnou az
v novem shell.

### `/etc/security/limits.conf`

Pridat na konec:

```
# --- RT scheduling pro audio (ithaca-legacy) ---
@audio - rtprio 99
@audio - memlock unlimited
@audio - nice -20
```

Po reloginu overit:

```bash
ulimit -r       # → 99 (rtprio)
ulimit -l       # → unlimited (memlock)
ulimit -e       # → 40 nebo similar (nice rozsah)
```

Bez teto konfigurace nas `pthread_setschedparam(SCHED_FIFO)` vrati EPERM
a v logu uvidis "RT priorita selhala" + TIP s tema radkama. Viz
`docs/rt-thread-priority.md`.

### Alt: spustit jako root

Pro standalone deploy (RPi jako audio modul bez user interaction) je nejjedu-
sssi spustit pres systemd jako root — RT prava ma automaticky bez limits.conf.
Viz systemd service priklad nize.

---

## 5) Audio HAT / DAC setup

### I2S HAT (priklad: HiFiBerry DAC+ / DAC2 Pro)

Po `dtoverlay=hifiberry-dacplus` v `config.txt` + reboot:

```bash
aplay -l
# Mel bys videt:
# card 0: sndrpihifiberry [snd_rpi_hifiberry_dacplus], device 0: HiFiBerry DAC+ HiFi pcm5102a-hifi-0 [HiFiBerry DAC+ HiFi pcm5102a-hifi-0]
```

Sample test:

```bash
speaker-test -D hw:0,0 -c 2 -t sine -f 440 -l 1
```

Pokud slysis sinus → HAT funguje. Kontrola SR support:

```bash
aplay -D hw:0,0 --dump-hw-params /dev/zero
# hleda "RATE: ... 48000 ..." a "FORMAT: ... S16_LE S24_3LE S32_LE ..."
```

### USB DAC

Stejne jako I2S, jen v `aplay -l` bude `card 1: <jmeno DAC>`. Mineaudio si
device picker.

### Vsechno dohromady: deterministic ALSA target

Vypsat moznosti, kterou pak da uzivatel do GUI / CLI:

```bash
aplay -L | grep -A2 '^hw:'
```

Typicky chces `hw:CARD=sndrpihifiberry,DEV=0` (stabilni napric reboot).

### NE pouzivat PipeWire / PulseAudio pro audio playing

Bookworm v default desktop instalaci ma PipeWire. Pridava 5-20 ms latence +
buffer copy. Pro nasi cilovku jit primo na ALSA `hw:` device.

Pokud nechces PipeWire vubec (Lite verze ho nema):

```bash
sudo apt remove --purge pipewire pulseaudio
```

---

## 6) Build dependencies + clone + build

### Apt balicky

```bash
sudo apt install -y \
    build-essential cmake git curl tar \
    libasound2-dev \
    python3 \
    pkg-config

# Volitelne pro CLI / GUI:
sudo apt install -y \
    libglfw3-dev libgl1-mesa-dev \
    xorg-dev
```

Poznamka: `libglfw3-dev` z apt **neni nutny** — GLFW si nas `fetch-third-party.sh`
stahuje a buildi z vendoru source. Apt balicek tam je jen pro pripad, ze by
GLFW build z vendoru selhal kvuli OpenGL dev headerum.

`libasound2-dev` je nutny pro miniaudio + RtMidi ALSA backend.

### Clone

```bash
cd ~
git clone https://github.com/alchy/ithaca-legacy.git
cd ithaca-legacy
```

### Build

```bash
make check-tools           # over cmake/ninja v PATH
make fetch-third-party     # stahne doctest, nlohmann, miniaudio, RtMidi, ImGui, GLFW
make build                 # cmake configure + build
```

Build trva na RPi5 ~3-5 minut s prvnim chodem (clean + full compile). Inkremen-
talne pak < 30 s.

CMakeLists.txt aktivuje pro `aarch64 Linux Release`: `-mcpu=native` (radek 41).
NEON FMLA pak autovektorizuje convolver inner loop.

### Test suite

```bash
make test     # ctest, 33 testu
```

Vsechny by mely projit. `M_PI` na Linux GCC neni problem (na rozdil od MSVC),
test_ir_modal a test_convolver projdou bez `_USE_MATH_DEFINES`.

### CLI smoke

```bash
./build/ithaca-cli --selftest
```

Vystup ma koncit `self-test OK`.

---

## 7) Projektova konfigurace (config.json)

Vychozi `config.json` v rootu repa je univerzalni. Pro RPi5 8 GB doporucuju:

```json
{
  "preload_ms": 250,
  "resonance_window_ms": 500,
  "cache_budget_mb": 4500,
  "max_voices": 96,
  "stream_threads": 2,
  "render_threads": 0,
  "midi_channel": 0,
  "log_level": "info"
}
```

Klicove rozdily:

- `preload_ms` **250** misto 150 — vic samplu v RAM, mene disk streamu per voice (NVMe to ustoji, ale setri CPU na FS overhead)
- `cache_budget_mb` **4500** explicitne (~55 % z 8 GB) — deterministicky cap; auto by dalo ~4800, nechame 3 GB pro OS + GUI
- `max_voices` **96** misto 128 — snizene strop pro voice scheduling spike, mene RT pressure
- `stream_threads` **2** — engine auto-sizing by stejnak dal 2 (4 jadra / 2), explicitne

### Pro 4 GB variantu

```json
{
  "preload_ms": 150,
  "cache_budget_mb": 2000,
  "max_voices": 64,
  "stream_threads": 2
}
```

(Ostatni klice stejne jako vyse.)

### Umisteni `state.json` (GUI)

Na Linuxu `state.json` se uklada do `$XDG_CONFIG_HOME/ithaca-legacy/state.json`
(typicky `~/.config/ithaca-legacy/state.json`). Vznika pri prvnim shutdown GUI.
Viz `app/gui/persistence.cpp:25-31`.

---

## 8) Prvni spusteni a verifikace

### CLI rezim (deploy bez GUI)

```bash
./build/ithaca-cli --play /path/to/bank --midi-in "USB MIDI" --block-size 256
```

V logu sleduj:

```
[..] [engine] [INFO]: stream workers: main=2 resonance=1 (jader=4)
[..] [loader] [INFO]: RAM budget banky: ...
[..] [audio] [INFO]: Audio start: <DAC name> SR=48000 block=256
[..] [audio] [INFO]: RT priorita aktivni (sr=48000 block=256)   ← KRITICKE
```

**Posledni radek** = potvrzeni ze `SCHED_FIFO prio=80` se aktivoval. Pokud
misto toho uvidis:

```
[audio] [WARNING]: RT priorita selhala (err=1) — default scheduling, jitter risk
[audio] [INFO]: TIP: pridej do /etc/security/limits.conf radky ...
```

→ neudelals krok 4 (`audio` group + `limits.conf` + relogin).

### GUI rezim

```bash
./build/ithaca-gui --bank-dir /path/to/banks --log-level info
```

GUI okno se otevre, v indicator stripu vidis VOICES / RESONANCE / MAIN RINGS /
RESO RINGS / DSP LOAD dlazdice. Hrej.

**DSP LOAD** by mel zustat stabilne **pod 60 %** s pravidelnym workloadem
(akord + sustain). Pokud kolisa nad 100 % → underrun, audio dropoutuje.

### Latence test

```bash
# Naplni input event → akusticky output, idealne mereno mikrofonem +
# osciloscopem. Bez nej alespon "subjektivne" test:
# Stisknout klavesu — odezva by mela byt "okamzite" (< 10 ms).
```

Pro merici test: nahrat hrani s mikrofonem + click track, porovnat MIDI input
timestamp s audio event v DAW (Reaper, Ardour).

### Deploy jako systemd service (auto-start)

```bash
sudo tee /etc/systemd/system/ithaca.service <<EOF
[Unit]
Description=ithaca-legacy audio engine
After=sound.target

[Service]
Type=simple
User=$USER
WorkingDirectory=/home/$USER/ithaca-legacy
ExecStart=/home/$USER/ithaca-legacy/build/ithaca-cli --play /path/to/bank --midi-in "USB MIDI" --block-size 256
LimitRTPRIO=99
LimitMEMLOCK=infinity
Nice=-10
Restart=on-failure

[Install]
WantedBy=multi-user.target
EOF

sudo systemctl daemon-reload
sudo systemctl enable --now ithaca.service
journalctl -u ithaca -f      # sleduj log
```

`LimitRTPRIO` + `LimitMEMLOCK` v service unitu prepise default limity per-
process bez ohledu na `/etc/security/limits.conf` — bezpecna cesta, kdyz
nechces ladit system-wide limits.

---

## 9) CPU affinity (advanced, volitelne)

Pro nejnizsi jitter izolovat jeden core jen pro audio:

V `/boot/firmware/cmdline.txt` na konec radky (NE nove radky — cmdline.txt
musi byt single line):

```
isolcpus=3 nohz_full=3 rcu_nocbs=3
```

Reboot. Pak `cat /proc/cmdline` pro overeni.

Po startu sistemd service `ithaca.service` dodal pinnig:

```ini
[Service]
ExecStart=/usr/bin/taskset -c 3 /home/.../ithaca-cli --play ...
```

Audio thread tim dostane core 3 exkluzivne (zadne ostatni user-space tasky
ho nepreemptnou). Core 0-2 nesou GUI + OS + streaming workers + ostatni.

**Pozor:** isolcpus znamena, ze core 3 nedostane balanced scheduling — bezi
*jen* tasky pinnute na nej. Pokud zapomenes pinnout audio thread, core 3
sedi nevyuziti.

**Soft variant** bez isolcpus: jen pinnig na core 3 pres taskset; ostatni
tasky tam taky muzou byt, ale aspon audio thread tam ma prioritu.

---

## 10) Troubleshooting

### "RT priorita selhala (err=1)"

err=1 = EPERM = nedostatecna prava.

- Overit `groups | grep audio` — mas group?
- Overit `ulimit -r` — vraci 99?
- Pokud ne obojeci → krok 4 (limits.conf + gpasswd + relogin)
- Pri sistemd service: kontrolovat `LimitRTPRIO=99` v unit souboru

### Audio nehraje (zadny zvuk)

- `aplay -l` — vidis HAT/DAC card?
- `speaker-test -D hw:0,0 -c 2 -t sine -f 440 -l 1` — slysis sinus?
- V `config.json` / `state.json` overit, ze audio device dropdown vede na
  spravnou kartu (ne PipeWire `pulse` device)
- Hlasitost: `alsamixer` — neni mute / nizko?

### Audio dropoutuje pri hrani

- Sleduj `DSP LOAD` dlazdici (GUI) nebo `dspLoadPeak()` (CLI log)
- > 100 %: zvys `block_size` z 256 na 512 (lipsi tolerance, vyssi latence)
- `MAIN RINGS` / `RESO RINGS` cervene = stream underrun → bank je na pomalem
  storage (SD card?), migrace na NVMe
- Voltage drop pri vetsi zatezi: `vcgencmd get_throttled` — non-zero = napajeci
  problem, pouzij oficialni 27W PSU

### Vysoka teplota / thermal throttle

```bash
vcgencmd measure_temp   # melo by byt < 75 °C pod zatezi
vcgencmd get_throttled  # non-zero bit = throttle aktivni
```

→ Active Cooler, lepsi pasivni chlazeni, vetsi case s vetrakem.

### Build selze

- `libasound2-dev` chybi? Pri compile RtMidi → "ALSA/asoundlib.h not found"
- GLFW build z vendoru selze na ARM? Zkontroluj `xorg-dev` apt balicek
- Permissions na vendor adresari? `fetch-third-party.sh` ma chmod 755 dostat

### Kernel / driver problemy

Pri silnem jitteru zvazit:

- PREEMPT_RT kernel (oficialni Ubuntu Studio image pro RPi5, nebo self-build)
- Vyssi block_size 512/1024 pro toleranci ke jitter
- Vypnout USB power management: `echo on > /sys/bus/usb/devices/.../power/control`

---

## Reference

- Setup oficialnich RPi: https://www.raspberrypi.com/documentation/computers/getting-started.html
- HiFiBerry konfigurace: https://www.hifiberry.com/docs/
- Raspberry Pi audio benchmark: https://www.crazyaudio.com/
- Linux audio realtime guide: https://wiki.linuxaudio.org/wiki/system_configuration
- Souvisejici dokumenty:
  - `docs/rt-thread-priority.md` — RT priorita audio threadu, per-platform
  - `docs/config-file.md` — `config.json` referencni schema
  - `docs/reference/I-multithreading.md` — vlakna a synchronizace v enginu
