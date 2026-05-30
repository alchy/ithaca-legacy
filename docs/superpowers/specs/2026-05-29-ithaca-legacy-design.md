# Ithaca-legacy — navrh (design spec)

> Pozn. k jazyku: cely dokument je psan cesky bez diakritiky. Anglicke jsou jen
> identifikatory v kodu a zavedene audio/DSP terminy (sample, voice, envelope,
> sustain, round-robin, ...). Princip: explicit je lepsi nez implicit.

Datum: 2026-05-29
Stav: schvaleno (vsechny 4 casti navrhu), pred psanim implementacniho planu.

---

## 1. Ucel a kontext

Ithaca-legacy je novy **sample player s jedinym core (sample core)**, ciste prepsany
z destilace dvou predchudcu:

- **icr** (`/Users/j/Projects/icr`): starsi multi-core engine. Ma jedinou existujici
  implementaci sample playbacku (SamplerCore) a minimalisticky logger.
- **icr2** (`/Users/j/Projects/icr2`): nastupce zamereny na aditivni syntezu. Ma lepsi
  oddeleni GUI od engine, cistsi strukturu projektu, plnou cross-platform `make`
  automatizaci, vendored zavislosti a nejlepsi dokumentaci. Aditivni cesta v icr2 ale
  **neni production ready** (monoliticka Voice trida, zamotane zodpovednosti) — proto
  ji opoustime.

**Rozhodnuti:** aditivni cestu zahazujeme; ithaca-legacy bude sample player s jednim
core, podobny Spectrasonics Keyscape (jak schopnostmi engine, tak podobou interface).

### Cile

- Crossplatform a HW-nezavisly: Windows, macOS, Linux.
- `make` automatizace (z icr2).
- Silna modularita a segregace (engine = headless knihovna, GUI = vymenitelny frontend).
- Lepsi dokumentace, snadna pochopitelnost.
- Nizka latence.
- Bez nutnosti zachovavat zpetnou kompatibilitu — volnost cisteho refaktoru.

### Cilovy hardware

Raspberry Pi prototyp pro core (pripojeny disk). **Pamet je hlavni omezeni**: 4GB Pi
neudrzi celou multi-velocity + round-robin + multi-mic banku v RAM. Proto je klicove
vylepseni **asynchronni streaming samplu z disku** (DFD — Direct From Disk). Pi ma vic
jader → paralelizujeme, kde to dava smysl.

Motivujici fakt: basove tony jsou 30 s i delsi; stereo float @48k ≈ 11 MB na sampl,
× 8 vrstev × round-robin × spodni oktavy = gigabajty jen za basy → plny preload nemozny.

### Zdroje zvuku

Uzivatel nahraje vlastni samply pozdeji. Banky v `/Users/j/SoundBanks/Ithaca/`
(as-blackgrand, vsl-imperial, sp-*, vi-ravenscroft, ...) slouzi jen pro **vyvoj a ladeni**,
ne jako finalni obsah.

---

## 2. Klicove koncepty

### 2.1 Dvojity format banky (engine auto-detekuje)

Engine sam rozpozna, ktery format je v bance pouzit, a podle toho se chova.

**Legacy format** (soucasne banky, single stereo par):
- `mNNN-velV-fSS.wav` — 8 velocity vrstev (napr. `m060-vel3-f48.wav`). Jen tato varianta;
  16-vrstvou variantu (`mNNN-vVVV-fSS.wav`) NEpodporujeme.

**Extended format** (nove banky, multi-mic) — ADRESAROVA STRUKTURA (REVIZE 2026-05-29):
- Layout: `PianoBankFolder/mNNN/<hash>.wav` — pro KAZDOU MIDI notu vlastni PODADRESAR `mNNN/`,
  v nem vsechny soubory te noty (vsechny velocity + pripadne mic pozice).
- Soubory NEMAJI jmennou konvenci — jen **hash** (zkraceny, napr. `bcdaf9123fekcne.wav`).
  Duvod: pri samplovani jedne noty vznikne souvisly WAV, splicer ho nakraje a pojmenuje
  hashem; staci hotove vyrezy nasypat do adresare noty. V jednom plochem adresari by bylo
  prilis mnoho souboru (velocity × noty × mic pozice), proto podadresar per nota.
- **Mic pozice pres suffix:**
  - `<hash>.wav` (BEZ suffixu) = **primarni** mic pozice ("main"). Je VZDY STEREO.
  - `<hash>-micpos-A.wav` ... `-micpos-Z.wav` = dalsi mic pozice, razene ABECEDNE.
  - Parovani uhozu = stejny `<hash>` (main + jeho micpos-A/B/... patri k jednomu uhozu).
- **Mono / stereo per mic pozice:** main je vzdy stereo. micpos-A..Z mohou byt MONO nebo STEREO
  (priznak je ve strukture samplu). Mono se EXPANDUJE na stereo az v RT pri prehravani → setri
  pamet. (Duvod: rezonancni deska / room mic nema vyraznou stereo sirku, lze samplovat mono.)
- **Velocity se DETEKUJE z RMS** primarni (main) pozice — viz 2.2. Bez velocity tokenu.
- **micpos je VOLITELNY:** kdyz nejsou zadne micpos soubory, pouzije se jen main `<hash>.wav`.
  Main je povinny (nese velocity). Chybejici micpos se jen zaloguje a pokracuje.
- Mix mic pozic je v configu (viz 2.3): default main 100%, nebo napr. main 80% + micpos-A 20%.

### 2.2 Mereni RMS a dynamicke sloty

RMS se **neuklada do nazvu**. Loader meri peak RMS sam pri nacitani samplu do RAM
(stejne se cachuje do RAM — meri se behem teze prvni pruchodu, napr. prvni sekunda).

Sjednocene pravidlo skupinovani (pro extended format, RMS z front samplu):
- Samply s ~stejnym RMS (v toleranci) = **round-robin varianty** tehoz velocity slotu.
- Samply s ruznym RMS = **ruzne velocity sloty**.

Tim vznika **dynamicky pocet slotu per nota**: m60 s 20 samply → N slotu × round-robin
varianty; nota s 5 samply → mene slotu. MIDI velocity 0–127 se mapuje na tuto per-nota
dynamickou krivku s interpolaci mezi sousednimi sloty. Legacy banka (8 vrstev) projde stejnou
cestou — namapuje se na 8 slotu podle `vel` tokenu. **Jeden kod pro oba formaty.**

- Round-robin tolerance: default ±1.5 dB, konfigurovatelne.
- Round-robin vyber: nahodny, s pravidlem "neopakuj naposledy hranou variantu".

### 2.3 Multi-mic

Nove piano se nahrava **dvema stereo pary mikrofonu** — jeden par vepredu (`front`),
jeden u rezonancni desky (`soundboard`). "Sampl" je tedy **multi-mic asset** (N stereo
perspektiv, napr. 4 kanaly = 2 stereo pary), ne jedna stereo dvojice. Core musi mic
perspektivy **michat** s nastavitelnymi urovnemi (mic mixer, jako Keyscape/Kontakt).

Dusledky:
- Sample/voice model drzi vice mic kanalu prehravanych synchronne.
- Mic mixer: per-mic level + per-mic mute + per-mic **invert phase** (proti fazovemu
  ruseni pri spatnem umisteni mikrofonu — jednoduche, ale pomuze overit zvuk).
- Mutovany mic se NESTREAMUJE → uspora RAM/disku/propustnosti (cena jinak roste
  ×pocet-mic; 4ch vs 2ch = 2× pamet).
- Default mic mix je konfiguracni polozka (navrh: oba pary 50/50). Zadna banka zatim neni.
- Legacy banky jsou single stereo par; multi-mic plati pro novy format.

### 2.4 ZADNE odvozovani chybejicich not (REVIZE 2026-05-29 + 2026-05-30)

**Rozhodnuti:** chybejici sample se NIJAK neodvozuje od existujiciho (zadny pitch-shift sousedni
noty, zadne casove protahovani, zadne dosamplovani RMS). Plati pro legacy I nove banky.

- Kdyz pro pozadovanou (notu, velocity) neni sampl, player jednoduse VI, ze chybi: priznak ve
  strukture samplu / nulova delka → ticho. Zadna nahrada.
- Velocity-slot VYBER (mapovani MIDI velocity 0-127 na nahrane dynamiky noty, viz 2.2) zustava —
  to neni odvozovani, jen vyber z toho, co je nahrane.
- Puvodne navrzeny pitch-shift "ve dvou osach" je ZRUSEN. Jiz napsany resampling/pitch-shift kod
  (transpozice sousedni noty) presunut do `engine/voice/_reserved_resampling.h` — mimo build,
  *reserved for future use*.
- **SR normalizace samplu (44.1 / 48 / 96 kHz) ZUSTAVA** (potvrzeno 2026-05-30). To NENI
  "odvozovani" — engine jen prehrava sampl ve spravne vysce, kdyz se jeho SR lisi od engine SR
  (`pos_inc = pitch_ratio × (sample_sr / engine_sr)` ve `Voice::start`). pitch_ratio je vzdy 1.0
  (zadna note-transpozice).

---

## 3. Architektura

### 3.1 Tri vrstvy (tvrde oddeleni)

```
app/             frontend (vymenitelny) — cli (ted), gui Keyscape-style (potom)
   |  jen pres fasadu Engine: lock-free MIDI fronta dolu, atomicke metry/parametry nahoru
libithaca_core   headless knihovna — veskera logika, zadne GUI
   |
disk + audio HW  miniaudio (out), RtMidi (in)
```

GUI nikdy nevola engine primo do audia — jen plni MIDI frontu a cte atomiky. To umoznuje
uplne jiny frontend na embedded displeji (Apple displej vs HW displej jsou jine).

### 3.2 Datovy model (od disku k hlasu)

```
BankIndex          objevi banku, parsuje nazvy, AUTO-DETEKCE legacy vs extended
  +- NoteMap[128]  pro kazdou MIDI notu: seznam VelocitySlot
       +- VelocitySlot   ma zmerene RMS; obsahuje 1..N round-robin variant
            +- SampleAsset   = 1..N mic perspektiv
                 +- MicLayer  preload buffer (RAM) + handle na disk (zbytek)
```

Dve ruzne osy: **VelocitySlot drzi round-robin varianty**, **SampleAsset drzi mic
perspektivy**.

### 3.3 Moduly knihovny (kazdy samostatne testovatelny)

| Modul | Zodpovednost |
|---|---|
| `sample_loader` | nacti WAV, zmer peak RMS, najdi attack/sustain hranici |
| `bank_index` | discovery, parsing nazvu, detekce formatu, postaveni NoteMap |
| `sample_store` | RAM preload cache, konfigurovatelny strop, evikce |
| `stream_engine` | background streaming thread(y), fronta pozadavku, ring buffery |
| `voice` + `voice_pool` | 128 slotu, alokace/kradez, retrigger-damp, multi-mic playback |
| `patch_manager` | MIDI → nota → slot → round-robin pick → pitch-shift kdyz chybi |
| `resonance` | sympaticke hlasy rizene zivym stavem |
| `pedal` | sustain + half-pedaling (CC64 0–127); udrzuje mnozinu neztlumenych strun |
| `dsp_chain` | seznam bypass-ovatelnych stagi (norm, comp, biquad, BBE, conv) |
| `mic_mixer` | mix mic perspektiv, per-mic level/mute/invert-phase |
| `master_bus` | gain, pan, metering |
| `engine` | fasada: init/start/processBlock/stop + lock-free MIDI fronta |
| `logger` | icr2-style RT-safe (lock-free ring buffer, printf-style, severity, timestampy) |

---

## 4. Streaming, pamet a threading

### 4.1 Typy threadu

```
Audio RT thread (miniaudio callback)   — JEN cte z RAM, zadna alokace, zadny disk, zadny lock
   +- vybere MIDI z lock-free fronty
   +- voice thread pool (work-stealing) — render hlasu paralelne na N jader
   +- secte hlasy → dsp_chain → mic_mixer → master_bus → out
Streaming thread(s)                    — cte z disku do ring bufferu aktivnich hlasu
MIDI/GUI thread                        — plni MIDI frontu, cte atomicke metry
```

### 4.2 Pamet: dve urovne na MicLayer

- **Preload buffer** (RAM): zacatek samplu, ~50–200 ms (konfigurovatelne). Note-on hraje
  okamzite odtud → nulova latence, audio thread nikdy neceka na disk.
- **Disk handle**: zbytek. Kratky sampl (vejde se cely do preloadu) → zadny streaming,
  drzi se cely v RAM. Streamuji se jen dlouhe samply (basy 30 s).

### 4.3 Ring buffery — pool, ne per-sampl

Ring buffery jsou pre-alokovane v poolu a prirazuji se jen aktivnim hlasum, ktere
streamuji. Mutovany mic se nestreamuje → jeho ring buffery i disk I/O odpadaji.

### 4.3a Preload pro rezonancni okno (dva preload regiony)

Rezonancni hlas (viz 5.5) startuje od peak RMS a hraje sustain region. Standardni preload
ale drzi jen ZACATEK samplu (`[0 .. attack]`), takze sustain region od peak RMS dal lezi na
disku — rezonance by cekala na stream (latence) nebo by nemela z ceho hrat. Proto sampl,
ktery slouzi jako ZDROJ rezonance, musi mit v RAM preload nejen attack, ale i dostatecny
kus OD peak RMS dal (rezonancni okno). Tedy dva preload regiony na MicLayer:
`[0 .. attack_konec]` + `[peak_RMS .. peak_RMS + resonance_window_ms]`.

Pozn.: u piana je peak RMS casto brzy (uvnitr attacku), takze "az do peak RMS" byva uz v
prvnim regionu; kriticky je ten DRUHY region — dost samplu OD peak RMS dal (sustain), ktery
v zakladnim preloadu chybi. `resonance_window_ms` je konfigurovatelne. Loader peak RMS i
hranice attack/sustain meri v tomtez mericim pruchodu (viz 2.2), takze obe pozice zna.

### 4.4 Underrun

Kdyz disk nestiha naplnit ring (SD karta, moc hlasu), hlas **rychle vyfaduje do ticha**
(kratky ramp) misto cvaknuti — predejdeme audio explozi. Indikuje se v logu a/nebo GUI.

### 4.5 Konfigurovatelne stropy

```json
{
  "preload_ms": 150,
  "resonance_window_ms": 500,
  "cache_budget_mb": 2400,
  "max_voices": 128,
  "stream_threads": 2,
  "render_threads": 0,
  "midi_channel": 0
}
```

- `cache_budget_mb` ~2400 jako default na 4GB modelu; konfigurovatelne (4GB model nemusi
  byt casem ani prodavan, takze nic nehardcodovat).
- `render_threads` 0 = auto dle poctu jader (skaluje automaticky).
- Cilovy disk na Pi: pravdepodobne SSD (USB vs NVMe TBD). Agresivita streamingu se doladi
  pozdeji — zatim best-effort, neoptimalizovat na jeden typ disku.
- **Preload ma prednost**: musi se vejit zacatky vseho, aby note-on nikdy necekal na disk.
  Kdyz se nevejde, zaloguje warning a automaticky snizi `preload_ms`.

---

## 5. Voice engine, retrigger, pedal, rezonance

### 5.1 Hlas (Voice)

Jeden hlas prehrava jeden `SampleAsset` (vsechny jeho mic perspektivy synchronne). Drzi:
pozici (double, kvuli pitch-shiftu/SR konverzi), envelope stav, pan, stream-cursor, gain.
Render: cte mic kanaly → mixuje pres `mic_mixer` → nasobi envelope/gain → scita do vystupu.

### 5.2 Note-on → vyber

```
noteOn(midi, vel):
  nota = NoteMap[midi]   (chybi? → najdi nejblizsi + spocti pitch ratio)
  slot = vyber VelocitySlot podle vel (interpolace mezi sousednimi sloty)
  variant = round-robin random (neopakuj posledni)
  alokuj hlas z poolu → nastav pitch ratio, envelope, mic layery
```

### 5.3 Retrigger / note-off (koncept z puvodniho playeru)

Nova nota na znejici klavesu: stary hlas se nekopiruje pryc skokem, ale rychle se utlumi
jako **samostatny damping hlas** (fast-decay ramp, ~20 ms), novy hlas startuje paralelne.
Nepozna se to, ale necvakne. icr to dela `damp_buf` bufferem — prevezmeme princip, ciste
jako "damping voice" v poolu.

### 5.4 Pedal: sustain + half-pedaling

- CC64 jako **spojita hodnota 0–127**, ne prah.
- 0 = tlumitka dole (noty se po note-off normalne tlumi).
- 127 = plne zvednuto (noty zni dal).
- Mezi = half-pedaling → skaluje rychlost release tlumeni **plynule** podle hloubky +
  spousti **dedikovane half-pedal samply** (az budou). Legacy fallback: plynule skalovani
  release time.

### 5.5 Sympaticka rezonance — rizena zivym stavem

`pedal` modul udrzuje jeden vstup: **aktualni mnozinu neztlumenych strun**:
- Pedal dole → neztlumene jsou VSECHNY struny.
- Pedal nahore → neztlumene jen prave drzene klavesy.

Rezonancni engine budi harmonicky pribuzne struny z teto mnoziny — bez ohledu na to, jak
vznikla. Tim pada **held-key rezonance i pedalova rezonance do jednoho kodu** (held-key
delame hned od zacatku, ne pozdeji).

```
Kdyz se zahraje nota N s velocity V a existuje mnozina neztlumenych strun:
  1. urci rezonujici struny: PLNE — vsechny neztlumene struny vazene harmonickou blizkosti
  2. pro kazdou spust REZONANCNI HLAS z HLAVNIHO samplu te struny (zadne dedikovane samply):
       - PRESKOC attack: start ~od peak RMS pozice (loader ji zna z mericiho pruchodu)
       - rychly nabeh na NIZKOU hlasitost
       - hlasitost = f(V hrane noty) × parametr_sily_rezonance × harmonicka_blizkost
  3. drzi sustain dokud struna neni ztlumena (pedal/klavesa)
  4. kdyz prijde dalsi nota → rezonancni pole se AKTUALIZUJE
```

Provazani (dulezite — promyslet pri implementaci):
- Velocity hrane noty → silnejsi uhoz budi silnejsi rezonanci.
- Zmena hlavniho tonu pri pedalu → rezonancni hlasy reaguji na to, co aktualne zni; nejsou
  staticke.
- **Rozpocet hlasu pro rezonanci:** oddeleny mekky strop. Pod tlakem (malo slotu, vysoka
  latence) ustupuje → **NIKDY neukradne hrany hlas**.
- Sila rezonance je parametrizovatelna za behu.

### 5.6 Kradez hlasu

Pri zaplneni poolu kradni **nejtissi sampl z celeho poolu** (maskuje se lip nez preruseni
streamovaneho hlasu). Uvolneni ring bufferu streamovaneho hlasu je v poradku.

### 5.7 MIDI kanalovy filtr

Jako icr2: engine umi **bind na konkretni MIDI kanal (1–16)** nebo **OMNI** (prijima vsechny
kanaly). Je to jen filtr na vstupu MIDI fronty — pred zarazenim do fronty se zahodi udalosti
z nepozadovaneho kanalu. Konfigurovatelne (`midi_channel`: 0 = OMNI, 1–16 = konkretni kanal).

### 5.8 Vyber a refresh MIDI portu

Jako icr2: engine musi umet **vyjmenovat dostupne MIDI vstupni porty** ze systemu, **otevrit /
prepnout port za behu** a **znovu naskenovat (refresh)**, kdyz se pripoji nove zarizeni (typicky
tlacitko v GUI). Patri do `midi_input` (enumerace + open/close pres RtMidi); fasada `engine` to
vystavi frontendu (`listMidiPorts()`, `openMidiPort(idx)`, `refreshMidiPorts()`). Vychozi port je
konfigurovatelny (jmeno nebo index). Headless CLI vybira port z configu/CLI argumentu.

---

## 6. Signalni retezec a DSP

```
hlasy (paralelne) ---+
rezonancni hlasy ----+--> mic_mixer --> dsp_chain --> master_bus --> out
                     |    (per-mic       (poradi      (gain, pan,
                     |     level/mute/    stagi,        metering)
                     |     invert-phase)  kazda bypass)
```

### 6.1 mic_mixer

Per mic perspektiva: level, mute, invert-phase. Mute = mic se nestreamuje. Slouci N
perspektiv do stereo vystupu.

### 6.2 dsp_chain — modularni

Seznam stagi se spolecnym rozhranim:

```cpp
struct DspStage {
  virtual void prepare(float sr, int block) = 0;
  virtual void process(float* l, float* r, int n) = 0;
  virtual void setParam(std::string key, float val) = 0;
  bool bypass = false;   // kazda stage zvlast vypnutelna
};
```

Default poradi stagi: `normalize` → `compressor` → `biquad` (volitelny) → `BBE` →
`convolution`. Pridat novou stage = pridat tridu, zadny zasah do chainu. Kazda zvlast
parametrizovatelna i bypass.

---

## 7. Build, konfigurace, dokumentace

### 7.1 Build (z icr2)

- **CMake + Makefile**: `make configure/build/smoke/rebuild/clean`, auto-detekce
  platformy/generatoru/jader, vendored deps (miniaudio, rtmidi, nlohmann_json).
- `make smoke` = nacti legacy banku, zahraj par not headless → WAV, over.
- Cile: `libithaca_core` (lib), `ithaca-cli` (headless), pozdeji `ithaca-gui`.

### 7.2 Konfigurace

Jeden JSON soubor: cesta k bance, preload cache, audio device, MIDI port, polyfonie,
parametry rezonance, mic mix, atd.

### 7.3 Tech stack

- Audio backend: miniaudio (vendored, vc. Pi/ALSA). MIDI: RtMidi.
- Sample rate: nove samply budou nahravany ve TREch SR spektrech — **44100 / 48000 / 96000 Hz**
  (rozhodnuto 2026-05-29). Engine s nimi musi umet pracovat. Vychozi model: engine ma
  konfigurovatelny vystupni/zpracovaci SR; samply v jinem SR nez engine se resampluji pri
  note-on. Legacy banky nesou SR tag v nazvu (`fSS`, napr. f48); pro novy format se doresi, zda
  SR tag ponechat nebo cist z WAV hlavicky (faze 7). Detail kvality resamplingu (linearni vs
  vyssi rad) k doladeni.
- Logger: icr2-style RT-safe.
- Jazyk: cestina bez diakritiky v komentarich, docs, README; identifikatory anglicky;
  komentovat spise vice (explicit > implicit).

### 7.4 Dokumentace

`docs/` indexovana dle role, architektura + diagramy (styl icr2).

---

## 7a. Dalsi architektonicke pozadavky (zaznamenano 2026-05-30)

Tyto pozadavky pribyly behem implementace; nepatri do jedne konkretni faze, ale ovlivnuji vic z nich.

- **Audio buffer size — runtime selector.** Velikost audio bufferu (block size) ma jit menit
  za behu, ne jen pri startu. icr i icr2 maji v GUI rolldown. Engine vystavi `setBlockSize(int)`
  (re-init audio_device, prepocet streamingu — viz nize). Default 256 frames. Faze 4 = backend
  podpora (CLI `--block-size`); faze 8 = GUI selector.
- **Reload rate streamingu skaluje s block size** (faze 4). `refill_threshold = max(ring/2,
  block_size * 4)` — vetsi audio blok = vic frames spotrebovanych per tick = ring potrebuje plnit
  drive. Pri zmene block size se prepocita.
- **Render API: castecny buffer.** Engine `processBlock(L, R, n_samples)` uz prijima libovolny
  `n_samples`. Pozadavek: zajistit, ze lze volat opakovane s ruznymi n (ne nutne plnym blokem).
  Pouziva offline batch render, JUCE/VST hosti s ruznou block velikosti. Dnes uz tak je;
  zafixovat jako KONTRAKT (test + komentar v engine.h).
- **Modularita pro JUCE / VST wrappery (klicovy invariant).** libithaca_core musi zustat
  cista hosti-agnosticka knihovna (zadny direkt audio_device dep v `Engine`; audio_device je
  konzument fasady, ne soucast jadra). Pravidla:
  - Engine fasada NEZAKLADA audio device; konstruktor a init nemaji audio side effects.
  - Audio I/O (miniaudio) zije v `engine/io/audio_device.*`; JUCE/VST wrapper ho proste
    nelinkuje a misto toho dela `engine.processBlock(L, R, n)` ze sve audio callbacky.
  - Engine vystavi vsechny parametry (block size, SR, master gain, atd.) pres explicitni
    metody — host muze cokoliv menit za behu.
  - Pripadny JUCE/VST wrapper bude samostatny `app/juce/` modul, neovlivni libithaca_core.
  Tento invariant resime az kdyz se k JUCE/VST dostaneme; faze 4-7 se ho drzi pasivne.

## 8. Fazovy plan implementace

1. **Skeleton + build** — struktura, CMake/Makefile, logger, vendored deps, `make smoke` stub.
2. **Loader + bank_index + sample_store** — nacti legacy banku, zmer RMS, postav NoteMap,
   preload do RAM.
3. **Voice pool + engine fasada + CLI** — headless prehrani legacy banky, polyfonie,
   velocity, retrigger, pitch-shift. (Bez streamingu — zatim vse v RAM.)
4. **Streaming** — preload + ring buffery + stream thread, underrun fade.
5. **Pedal + sympaticka rezonance.**
6. **DSP chain + mic_mixer** (mic_mixer naplno az s extended bankami).
7. **Extended format** (multi-mic, round-robin) — az bude mit uzivatel vlastni samply.
8. **GUI** — Keyscape-style frontend.

### 8.x GUI pozadavky (zaznamenano 2026-05-30, ke specifikaci ve fazi 8)

Minimalni vec, kterou GUI MUSI mit (vychazi z icr/icr2):

- **MIDI input selector + Refresh** — list portu (RtMidi `listMidiPorts`), tlacitko Refresh
  pro znovunaskenovani po pripojeni noveho zarizeni, current port indikator. (Backend uz mame —
  spec sekce 5.7/5.8.)
- **Bank selector (rolldown)** — list bank v `bank_root_dir`, vyber → engine.loadBank(...). Po
  vyberu noveho ukladani do configu.
- **Audio buffer size selector (rolldown)** — typicke hodnoty 64/128/256/512/1024/2048; vyber
  zavola engine.setBlockSize(n). Default z configu.
- **Master gain + meter, polyphony display, CPU usage** — read-only metry.
- **DSP chain bypass per stage** + parametry (jakmile bude DSP chain ve fazi 6).
- **Mic mixer** — per-mic level + mute + invert-phase (jakmile bude extended format ve fazi 7).
- **Sympathetic resonance amount** — parametrizovatelne (faze 5).
- **GUI persistuje stav do configu** — pri ukonceni programu serializuje vsechny GUI-nastavene
  hodnoty (block size, master gain, vybrana banka, MIDI port, DSP/mic/rezonancni parametry, ...)
  zpet do JSON configu. Pri pristim startu GUI nacte z configu. icr/icr2 to tak delaji.

Ladi se na legacy bankach z `/Users/j/SoundBanks/Ithaca/` (faze 1–6), extended format a
vlastni samply prichazeji ve fazi 7.

---

## 9. Otevrene body (doresit behem implementace prislusne faze)

- Presny vyber rezonujicich strun a vahovani harmonickou blizkosti (faze 5).
- Detaily streamingu vuci konkretnimu disku (USB SSD vs NVMe) — best-effort, doladit (faze 4).
- Presna podoba half-pedal samplu a jejich krivky (faze 5/7).
- Zda extended format ponecha SR tag, nebo cte SR z WAV hlavicky (faze 7).
- GUI: konkretni podoba Keyscape-style interface a embedded varianta (faze 8).
