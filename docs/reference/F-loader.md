# Loader

Oblast Loader pokrývá vše od fyzického adresáře s WAV soubory až po strukturu `Bank` plně připravenou k použití audio vláknem. Vstupní bod je `loadBank()`, která nejprve zavolá `scanBank()` k detekci formátu banky — buď *fixed-velocity* (ploché soubory `mNNN-velV-fSS.wav` přímo v adresáři) nebo *dynamic-velocity* (podsložky `m<MIDI>/` s libovolně pojmenovanými WAV soubory pro jednotlivé velocity vrstvy). Formát *extended* (`mNN-MIC-HASH.wav`) je parsován, ale zatím odmítnut (fáze 7). Po skenování každý nalezený WAV projde `ingestSampleFile()`: přečte se hlavička (`peekWavInfo`), rozhodne se, zda je sampl dostatečně krátký pro plné uložení v RAM (*FullyLoaded*) nebo bude streamován (*Streamed*), načte se preload head, změří se peak RMS a hranice attacku (u Streamed se nastaví `resonance_start_frame = attack_end`), a výsledek se uloží jako `VelocitySlot` → `SampleAsset` → `MicLayer`. Nakonec se sloty každé noty seřadí vzestupně podle peak RMS dB. **Rezonanční okno se NEplní v ingest** (fáze 8): po sortu `buildResonanceCache(bank, target_db, window_ms)` naplní `preload_resonance` (6 s, z disku) **jen pro per-notu cílovou velocity vrstvu** (`nearestSlotByRms(target_db)`) — ostatní vrstvy ho nemají (rezonance je používá jen pro cílovou vrstvu → net úspora RAM). Tatáž funkce slouží i pro runtime přestavbu cache při změně „Resonance Layer" slideru. Veškerá práce probíhá výhradně ve vlákně pro načítání (off-RT); audio vlákno konzumuje `preload_head` a `preload_resonance` read-only prostřednictvím `Voice` a `ResonanceVoice`.

---

## Implementováno v souborech

| soubor | odpovědnost | klíčové typy |
|---|---|---|
| `engine/sample/sample_types.h` | Datový model banky v paměti | `Bank`, `NoteSlots`, `VelocitySlot`, `SampleAsset`, `MicLayer`, `SampleFile`, `BankFormat`, `MicLayerMode` |
| `engine/sample/bank_index.h/.cpp` | Sken adresáře a parsování názvů souborů | `BankScan`, `BankFileEntry`, `ParsedName` |
| `engine/sample/sample_loader.h/.cpp` | Analýza audio dat (peak RMS, attack end) | — (čisté funkce) |
| `engine/sample/sample_store.h/.cpp` | Orchestrace načítání banky; ingest jednoho WAV souboru | `Bank` |
| `engine/io/wav_reader.h/.cpp` | Čtení WAV souborů (hlavička, celý soubor, výřez) | `WavData`, `WavInfo` |
| `engine/io/wav_writer.h/.cpp` | Zápis interleaved stereo float do 16-bit PCM WAV | — |

---

## Soubory

### `engine/sample/sample_types.h`

Definuje kompletní datový model banky. Neobsahuje žádnou logiku ani I/O.

#### Struktury a jejich pole

| struktura | pole | typ | popis |
|---|---|---|---|
| `BankFormat` (enum class) | `Unknown` | — | formát nebyl rozpoznán nebo banka je prázdná |
| | `FixedVelocity` | — | ploché soubory `mNNN-velV-fSS.wav` |
| | `Extended` | — | ploché soubory `mNN-MIC-HASH.wav` (fáze 7) |
| | `DynamicVelocity` | — | podsložky `m<MIDI>/` s hašovanými WAV |
| `MicLayerMode` (enum class) | `FullyLoaded` | — | celý sampl je v `preload_head`; žádný streaming |
| | `Streamed` | — | jen začátek + rezonanční okno v RAM, zbytek se streamuje z disku |
| `SampleFile` | `path` | `std::string` | absolutní cesta ke zdrojovému WAV |
| | `frames` | `int` | celkový počet stereo framů v souboru |
| | `sample_rate` | `int` | vzorkovací frekvence |
| | `valid` | `bool` | true = hlavička byla úspěšně přečtena |
| `MicLayer` | `mic_name` | `std::string` | název mikrofonu; legacy = `"stereo"` |
| | `file` | `SampleFile` | odkaz na zdrojový WAV |
| | `mode` | `MicLayerMode` | `FullyLoaded` nebo `Streamed` |
| | `preload_head` | `std::vector<float>` | interleaved stereo, framey `[0 .. head_frames)` |
| | `head_frames` | `int` | počet framů v `preload_head` |
| | `preload_resonance` | `std::vector<float>` | interleaved stereo, rezonanční okno (6 s); naplněné JEN pro per-notu cílovou velocity vrstvu (`buildResonanceCache`), jinak prázdné. Audio vlákno z něj čte v cache módu `ResonanceVoice` |
| | `resonance_start_frame` | `int` | absolutní pozice v souboru, kde začíná `preload_resonance`; = `attack_end_frame` z SampleAsset |
| | `resonance_frames` | `int` | skutečný počet načtených framů v `preload_resonance` |
| `SampleAsset` | `mics` | `std::vector<MicLayer>` | mic perspektivy (legacy: 1 stereo) |
| | `peak_rms_db` | `float` | maximum krátkookénkového RMS v dBFS, měřeno z reference (první/front) mic |
| | `attack_end_frame` | `int` | přibližný konec attacku (frame, kde bylo dosaženo peak RMS); základ pro resonance skip-attack |
| `VelocitySlot` | `variants` | `std::vector<SampleAsset>` | round-robin varianty stejné dynamiky (legacy: 1) |
| | `rms_db` | `float` | reprezentativní RMS slotu (kopie z první varianty); základ pro řazení velocity vrstev |
| `NoteSlots` | `slots` | `std::vector<VelocitySlot>` | velocity sloty seřazené vzestupně podle `rms_db` (nejtiší → nejhlasitější) |
| | `recorded` | `bool` | true = banka obsahuje alespoň jeden sampl pro tuto MIDI notu |
| `Bank` | `name` | `std::string` | jméno banky (basename adresáře) |
| | `path` | `std::string` | absolutní cesta k adresáři |
| | `format` | `BankFormat` | detekovaný formát |
| | `notes[128]` | `NoteSlots` | pole 128 MIDI not |
| | `resident_frames` | `size_t` | celkem framů rezidentních v RAM (součet `head_frames + resonance_frames` přes všechny MicLayery) |
| | `total_bytes` | `size_t` | odhad paměti v bajtech (pro diagnostiku) |
| | `loaded_samples` | `int` | počet úspěšně ingested SampleAssetů |

#### Funkce

| funkce (signatura) | vlákno | vstup → výstup | volá ji | volá (proč) | parametry | vysvětlení |
|---|---|---|---|---|---|---|
| `bankFormatName(BankFormat) → const char*` | libovolné | `BankFormat` → C-string název | `sample_store.cpp` (logování), `engine.cpp` | — | `f` — hodnota výčtu | Inline pomocná funkce; vrací lidsky čitelný řetězec `"fixed-velocity"`, `"extended"`, `"dynamic-velocity"` nebo `"unknown"`. Triviální switch. |

---

### `engine/sample/bank_index.h` / `bank_index.cpp`

Detekce formátu banky a parsování názvů souborů. Parsovací funkce jsou čisté (bez I/O), `scanBank()` provádí adresářový sken.

#### Struktury

| struktura | pole | popis |
|---|---|---|
| `ParsedName` | `ok`, `midi`, `vel`, `sr_tag`, `mic`, `hash`, `filename` | výsledek parsování jednoho názvu souboru; `ok=false` → soubor neodpovídá žádnému vzoru |
| `BankFileEntry` | `parsed`, `full_path` | jeden nalezený soubor: metadata + absolutní cesta |
| `BankScan` | `format`, `files`, `skipped` | výsledek skenu celého adresáře |

#### Funkce

| funkce (signatura) | vlákno | vstup → výstup | volá ji | volá (proč) | parametry | vysvětlení |
|---|---|---|---|---|---|---|
| `parseFixedVelocityName(const std::string& filename) → ParsedName` | off-RT | název souboru → `ParsedName` | `scanBank()`, `detectFormatFromName()`, testy | regex `fixedVelRe()`, `std::stoi` | `filename` — samotný název souboru (bez cesty) | Porovná název s regexem `m(\d{3})-vel(\d)-f(\d{2,3})\.wav` (case-insensitive). Regex omezuje délku číslic (3/1/2–3), čímž brání `std::out_of_range` z patologicky dlouhých čísel. Všechny `std::stoi` volání jsou navíc obalena v try/catch — poškozený název vrátí prázdný `ParsedName{ok=false}`. Nastaví `midi`, `vel` (0–7), `sr_tag` (44/48/96) a `filename`. |
| `parseExtendedName(const std::string& filename) → ParsedName` | off-RT | název souboru → `ParsedName` | `scanBank()`, `detectFormatFromName()`, testy | regex `extendedRe()`, `std::stoi` | `filename` | Porovná s regexem `m(\d{1,3})-([a-z]+)-([A-Za-z0-9]+)\.wav`. Nastaví `midi`, `mic` (např. `"front"`, `"soundboard"`) a `hash`. Legacyový formát je podmnožinou tohoto vzoru (token `"vel"` by prošel jako mic), ale `detectFormatFromName()` dává legacyi prioritu, takže sem se fixedVelocity soubory nedostanou. |
| `detectFormatFromName(const std::string& filename) → BankFormat` | off-RT | název souboru → `BankFormat` | testy | `parseFixedVelocityName()`, `parseExtendedName()` | `filename` | Zkusí postupně fixed, pak extended; vrátí první shodu. Slouží primárně pro testovatelnost. `scanBank()` interně používá přímo `parseFixedVelocityName()` / `parseExtendedName()`. |
| `scanBank(const std::string& dir) → BankScan` | off-RT (load) | cesta k adresáři → `BankScan` | `loadBank()` v `sample_store.cpp` | `parseNoteFolder()`, `isWavFile()`, `parseFixedVelocityName()`, `parseExtendedName()`, `std::filesystem::directory_iterator` | `dir` — cesta k adresáři banky | Dvoustupňová detekce formátu: **(1) Dynamic-velocity**: prochází položky adresáře a hledá podadresáře jejichž název odpovídá `m(\d{1,3})` (funkce `parseNoteFolder()`). Pokud alespoň jeden takový podadresář existuje, označí formát jako `DynamicVelocity`, projde každý podadresář, přijme `.wav` soubory (ostatní incrementují `skipped`) a vytvoří `BankFileEntry` s `midi` ze jména složky a `filename` z WAV souboru. Vrátí okamžitě — ploché formáty se ani netestují. **(2) Ploché formáty (FixedVelocity / Extended)**: projde soubory v kořenu, zkusí nejprve `parseFixedVelocityName()`, pak `parseExtendedName()`. Počítá shody zvlášť (`fixed_count`, `extended_count`). Formát určí **nadpoloviční většina** rozpoznaných souborů (`fixed_count >= extended_count` → FixedVelocity). Pokud není žádný rozpoznán, formát je `Unknown`. Chyby adresáře (`std::error_code ec`) ukončí vnitřní smyčku break-em — funkce nikdy nevyhodí výjimku. |

---

### `engine/sample/sample_loader.h` / `sample_loader.cpp`

Čisté analytické funkce nad interleaved stereo float bufferem. Žádné I/O, žádné globální state.

#### Konstanty

| konstanta | hodnota | popis |
|---|---|---|
| `kSilenceFloorDb` | `-120.0f` | podlaha dBFS pro případ absolutního ticha (vyhnout se `log10(0)`) |
| `kWindowMs` (anonymní ns) | `50.0f` ms | velikost okna klouzkavého RMS |

#### Interní typ

| typ | pole | popis |
|---|---|---|
| `PeakResult` (anonymní ns) | `max_rms`, `peak_frame` | výsledek `slidingPeakRms()`: maximum amplitudy a frame, kde bylo dosaženo |

#### Funkce

| funkce (signatura) | vlákno | vstup → výstup | volá ji | volá (proč) | parametry | vysvětlení |
|---|---|---|---|---|---|---|
| `slidingPeakRms(const float*, int, int) → PeakResult` | off-RT | interleaved stereo buffer → `{max_rms, peak_frame}` | `measurePeakRmsDb()`, `findAttackEnd()` | `std::sqrt` | `data` — buffer, `frames` — stereo framů, `sample_rate` — Hz | Anonymní pomocná funkce. Vypočítá délku okna jako `kWindowMs * sample_rate / 1000` (min 1, max frames). Krok = polovina okna (50% překryv). Pro každé okno spočítá mono mix `(L+R)/2`, RMS pomocí `double` akumulátoru (přesnost), převede na float. Pokud je aktuální RMS větší než dosavadní maximum, uloží `max_rms` a pozici středu okna jako `peak_frame`. |
| `measurePeakRmsDb(const float* data, int frames, int sample_rate) → float` | off-RT | interleaved stereo buffer → peak RMS v dBFS | `ingestSampleFile()` v `sample_store.cpp`, testy | `slidingPeakRms()`, `std::log10` | `data` — buffer, `frames` — stereo framů, `sample_rate` — Hz | Ochrana pro null nebo prázdný buffer (vrátí `kSilenceFloorDb`). Volá `slidingPeakRms()`, pokud je `max_rms <= 0` vrátí podlahu. Jinak `20 * log10(max_rms)` a výsledek ořízne na `kSilenceFloorDb` zdola. Výsledek jde do `SampleAsset.peak_rms_db` a `VelocitySlot.rms_db` — slouží pro řazení velocity vrstev. |
| `findAttackEnd(const float* data, int frames, int sample_rate) → int` | off-RT | interleaved stereo buffer → index framu | `ingestSampleFile()` v `sample_store.cpp`, testy | `slidingPeakRms()` | `data`, `frames`, `sample_rate` | Ochrana pro null/prázdné (vrátí 0). Vrátí `peak_frame` z `slidingPeakRms()` — přibližný střed okna s maximálním RMS, tedy konec attacku / začátek sustainu. Tento index jde do `SampleAsset.attack_end_frame` a slouží jako `resonance_start_frame` pro rezonanční hlasy (fáze 5). U velmi krátkých samplů (okno = celý sampl) vrátí 0. |

---

### `engine/sample/sample_store.h` / `sample_store.cpp`

Orchestrace načítání banky. Veřejné API: `loadBank()` + `buildResonanceCache()` (fáze 8); vše ostatní je v anonymním namespace.

#### Funkce

| funkce (signatura) | vlákno | vstup → výstup | volá ji | volá (proč) | parametry | vysvětlení |
|---|---|---|---|---|---|---|
| `ingestSampleFile(Bank&, int midi, const std::string& full_path, const std::string& filename, log::Logger&, int preload_ms, int resonance_window_ms) → bool` (anonymní ns) | off-RT | cesta + metadata → naplní `bank.notes[midi]`; vrátí `true` při úspěchu | `loadBank()` | `peekWavInfo()`, `readWavRange()`, `measurePeakRmsDb()`, `findAttackEnd()` | `bank` — cílová banka (in-out); `midi` — MIDI nota 0–127; `full_path` — absolutní cesta; `filename` — pro logy; `preload_ms` — délka head preloadu v ms; `resonance_window_ms` — délka rezonančního okna v ms | Hlavní logika ingestu jednoho WAV souboru. **Postup:** (1) `peekWavInfo()` — přečte hlavičku bez dat; při neúspěchu vrátí false + WARNING. (2) Vypočítá `preload_frames = preload_ms * sample_rate / 1000` a `short_threshold = preload_frames * 2`. Pokud `info.frames <= short_threshold` → `mode = FullyLoaded`, `head_frames = info.frames`; jinak `mode = Streamed`, `head_frames = preload_frames`. (3) `readWavRange(path, 0, head_frames)` — načte preload head; při neúspěchu vrátí false. Pokud vráceno méně framů než požadováno (oříznutý soubor), vypíše WARNING a upraví `head_frames`; pokud je `head_frames >= file.frames`, přeřadí do FullyLoaded. (4) `measurePeakRmsDb()` + `findAttackEnd()` nad `preload_head` — viz předpoklad INVARIANT: u piano-class samplů je peak RMS vždy v attack fázi, která se vejde do preload head. (5) Pro `Streamed` nastaví `resonance_start_frame = attack_end_frame` (rezonanční buffer se NEPLNÍ zde — fáze 8; plní ho `buildResonanceCache` po sortu). (6) Sestaví `MicLayer → SampleAsset → VelocitySlot`, vloží do `bank.notes[midi].slots`, nastaví `recorded = true`, aktualizuje diagnostické čítače `resident_frames`, `total_bytes`, `loaded_samples`. (`resonance_window_ms` param je nyní v ingest nevyužitý.) |
| `sortBankSlotsByRms(Bank&) → void` (anonymní ns) | off-RT | `Bank` (in-out) | `loadBank()` | `std::sort` | `bank` | Projde všech 128 not a seřadí `slots` vzestupně podle `VelocitySlot.rms_db` (nejtiší → nejhlasitější). Toto je autoritativní řazení velocity vrstev pro oba formáty — velocity tag `vel` z názvu souboru (fixed-velocity) se po seřazení nepoužívá. |
| `buildResonanceCache(Bank&, float target_db, int window_ms, log::Logger&) → std::array<bool,128>` (fáze 8) | off-RT (load / background rebuild) | banka + cíl + okno → naplní `preload_resonance` cílových vrstev; vrátí per-notu ready | `Engine::loadBank`, `Engine::rebuildResonanceCache` (bg vlákno) | `nearestSlotByRms()`, `readWavRange()` | `target_db` — cíl výběru vrstvy; `window_ms` — délka okna (6000) | Po sortu: pro každou notu zjistí cílový slot (`nearestSlotByRms`), načte `window_ms` od `resonance_start_frame` z disku do jeho `preload_resonance`, ostatní sloty vyčistí (`clear` + `shrink_to_fit`). FullyLoaded cílová vrstva buffer nepotřebuje (hraje z `preload_head`) → ready=true bez čtení. Vrací `array<bool,128>` (true = nota má použitelnou cache); caller (Engine) z něj nastaví `ResonanceEngine::setCacheReady`. |
| `logBankSummary(const Bank&, log::Logger&, int cache_budget_mb) → void` (anonymní ns) | off-RT | — | `loadBank()` | `logger.log()` | `cache_budget_mb` — 0 = bez kontroly | Zaloguje `loaded_samples`, `resident_frames` a přibližnou velikost v MB. Pokud `cache_budget_mb > 0` a `total_bytes > budget_bytes`, vypíše WARNING (načítání pokračuje bez ohledu na překročení). |
| `loadBank(const std::string& dir, log::Logger& logger, int cache_budget_mb, int midi_from, int midi_to, int preload_ms, int resonance_window_ms) → Bank` | off-RT (load) | cesta k adresáři → `Bank` v paměti | `Engine::loadBank()` v `engine.cpp`, `app/cli/main.cpp` | `scanBank()`, `ingestSampleFile()`, `sortBankSlotsByRms()`, `logBankSummary()`, `bankFormatName()` | `dir` — adresář banky; `cache_budget_mb` — varování při překročení (0 = vypnuto); `midi_from/midi_to` — inkluzivní rozsah MIDI not (default 0–127); `preload_ms` — délka head preloadu (default 150 ms); `resonance_window_ms` — délka rezonančního okna (default 500 ms) | Vstupní bod Loaderu. **Postup:** (1) Vytvoří prázdnou `Bank`, nastaví `path` a `name` (basename adresáře). (2) Zavolá `scanBank(dir)` a uloží `bank.format`. (3) Pokud `Unknown` → WARNING + prázdná banka. (4) Pokud `Extended` → WARNING "zatím nepodporován (fáze 7)" + prázdná banka. (5) Pro `FixedVelocity` i `DynamicVelocity`: iteruje přes `scan.files`, přeskočí soubory mimo MIDI rozsah nebo s `midi < 0`, volá `ingestSampleFile()` pro každý. (6) `sortBankSlotsByRms()` + `logBankSummary()`. Vrátí `Bank` hodnotou (move). Celé zpracování je synchronní, blokující — volat pouze z načítacího vlákna. |

---

### `engine/io/wav_reader.h` / `wav_reader.cpp`

Minimální WAV parser. Vždy vrací interleaved stereo float `[-1, 1]` — mono soubory se zdvojí do L+R. Podporuje audio formáty 16-bit PCM, 24-bit PCM, 32-bit PCM a 32-bit IEEE float. Závisí jen na C stdio (`std::fopen`/`fread`/`fseek`).

#### Struktury

| struktura | pole | popis |
|---|---|---|
| `WavData` | `samples` (interleaved stereo float), `frames`, `sample_rate`, `valid` | plná audio data v RAM |
| `WavInfo` | `frames`, `sample_rate`, `channels`, `valid` | pouze hlavičkové informace, bez dat |
| `FmtInfo` (anonymní ns) | `audio_format`, `channels`, `sample_rate`, `bits`, `have` | výsledek parsování `fmt ` chunku |

#### Funkce

| funkce (signatura) | vlákno | vstup → výstup | volá ji | volá (proč) | parametry | vysvětlení |
|---|---|---|---|---|---|---|
| `parseHeader(std::FILE*, FmtInfo&, uint32_t&, bool&) → bool` (anonymní ns) | off-RT | otevřený soubor → naplní `FmtInfo`, `data_size`, `found_data` | `readWav()`, `readWavRange()`, `peekWavInfo()` | `std::fread`, `std::fseek`, `std::strncmp` | `f` — soubor; `fmt` (out); `data_size` (out); `found_data` (out) | Ověří RIFF/WAVE hlavičku (4+4 bajty). Poté prochází chunky ve smyčce: `fmt ` chunk přečte prvních 16 bajtů (audio_format, channels, sample_rate přes `memcpy` kvůli word-alignmentu, bits), zbytek chunku přeskočí. `data` chunk uloží velikost, nastaví `found_data=true` a vrátí — soubor je **pozicován na začátku audio dat**. Neznámé chunky přeskočí (word-aligned: `chunk_size + chunk_size & 1`). Poznámka v kódu: WAV je little-endian, cílové platformy (x86/ARM/RPi) jsou taktéž LE, proto se čte přímo bez byte-swap. |
| `sampleToFloat(const uint8_t*, uint16_t bits, uint16_t audio_format) → float` (anonymní ns) | off-RT | surové bajty → float `[-1, 1]` | `readWav()`, `readWavRange()` | `std::memcpy` | `p` — ukazatel na první bajt vzorku; `bits`; `audio_format` (1=PCM, 3=float) | Konverze podle formátu: IEEE float 32-bit → `memcpy`; 16-bit PCM → `/32768.f`; 24-bit PCM → sign-extend 23. bitu ručně, `/8388608.f`; 32-bit PCM → `/2147483648.f`. Neznámý formát vrátí `0.f`. |
| `readWav(const std::string& path) → WavData` | off-RT | cesta → celá audio data | testy (`test_wav_reader.cpp`, `test_wav_writer.cpp`, `test_batch_renderer.cpp`) | `parseHeader()`, `sampleToFloat()`, `std::fread` | `path` — absolutní cesta | Otevře soubor, zavolá `parseHeader()`, alokuje `raw` buffer o velikosti `data_size`, přečte vše najednou (`fread`). Tolerantní k oříznutému souboru — pokud `fread` vrátí méně než `data_size`, upraví `data_size` na skutečně přečtené bajty. Konvertuje každý frame do stereo float (mono → zdvoj). Nakonec nastaví `valid=true`. Při jakékoliv chybě vrátí `WavData{valid=false}`. |
| `peekWavInfo(const std::string& path) → WavInfo` | off-RT | cesta → hlavičkové metadata | `ingestSampleFile()` v `sample_store.cpp`, testy | `parseHeader()`, `std::ftell`, `std::fseek` | `path` | Otevře soubor, zavolá `parseHeader()`, poté ověří skutečnou velikost `data` chunku: `ftell` po parsování = `data_start`, `fseek(SEEK_END)` = `file_end`; pokud `file_end - data_start < data_size`, ořízne `data_size` na skutečný zbytek. Tím koriguje případ, kdy WAV hlavička lže (oříznutý / streamovaný soubor). Vypočítá `frames = data_size / (bytes_per_sample * channels)`. Nastaví `valid=true`. Soubor okamžitě uzavře (nečte audio data). |
| `readWavRange(const std::string& path, int64_t frame_off, int64_t frame_count) → WavData` | off-RT (load) / disk-streaming vlákno (`stream_engine.cpp`) | cesta + offset + počet framů → výřez audio dat | `ingestSampleFile()` (head + resonance preload), `stream_engine.cpp` (streaming worker) | `parseHeader()`, `sampleToFloat()`, `fseeko` / `_fseeki64`, `std::fread` | `path`; `frame_off` — offset od začátku audio dat v stereo framech (int64, pro soubory >2 GB); `frame_count` — požadovaný počet framů | Defensivní vstupy: `frame_off < 0` → invalid; `frame_count == 0` → `valid=true, frames=0`. Po parsování hlavičky změří skutečný zbytek souboru (stejně jako `peekWavInfo`), takže `data_size` odpovídá realitě. `frame_off >= total_frames` → vrátí `valid=true, frames=0` (EOF signal pro streaming worker). Jinak ořízne `read_frames = min(frame_count, available)`, provede 64-bit seek (`fseeko`/`_fseeki64` dle platformy), přečte a konvertuje. Pokud `actual_frames > INT32_MAX`, ořízne na `INT32_MAX` (FUTURE: rozšíření `WavData.frames` na int64). Nastaví `valid=true`. |

---

### `engine/io/wav_writer.h` / `wav_writer.cpp`

Zápis interleaved stereo float bufferu do 16-bit PCM WAV. Používá ho batch renderer a testy (round-trip s `readWav`).

#### Interní pomocné funkce (anonymní ns)

| funkce | popis |
|---|---|
| `wU32(std::FILE*, uint32_t)` | zapíše 4 bajty little-endian |
| `wU16(std::FILE*, uint16_t)` | zapíše 2 bajty little-endian |

#### Funkce

| funkce (signatura) | vlákno | vstup → výstup | volá ji | volá (proč) | parametry | vysvětlení |
|---|---|---|---|---|---|---|
| `writeWavStereo16(const std::string& path, const std::vector<float>& samples, int sample_rate) → bool` | off-RT | interleaved stereo float → soubor na disku; vrátí `false` pokud nelze otevřít | `engine/render/batch_renderer.cpp`, testy | `wU32()`, `wU16()`, `std::fwrite`, `std::lround` | `path` — cílový soubor; `samples` — interleaved stereo `[L, R, ...]`; `sample_rate` — Hz | Zapíše standard RIFF/WAVE hlavičku: fmt chunk (PCM, 2 kanály, 16 bit, byte_rate = `sample_rate * 4`), poté data chunk. Každý float vzorek clampuje na `[-1, 1]` a kvantuje `std::lround(s * 32767.f)` na `int16_t`. Zápis probíhá vzorek po vzorku (`fwrite` 2 bajty). Vrátí `true` při úspěchu, `false` při selhání `fopen`. |

---

## Křížové odkazy

| oblast | vazba |
|---|---|
| **Core — Engine** | `Engine::loadBank()` a `Engine::reloadBank()` v `engine/engine.cpp` volají `ithaca::loadBank()` ze `sample_store.h`. `reloadBank()` implementuje bezpečný drain (audio vlákno nejprve ztichne přes příznak `reload_pending_`, VoicePool a ResonanceEngine se hard-resetují) a teprve pak nahradí `bank_`. |
| **Polyfonie — VoicePool / Voice** | `Voice` (audio vlákno) čte `MicLayer::preload_head` read-only a po překročení head_frames čte z ring bufferu (StreamEngine). Viz `engine/voice/voice.cpp`. |
| **Rezonance — ResonanceEngine / ResonanceVoice** | `ResonanceVoice` (audio vlákno) čte `MicLayer::preload_resonance` (u Streamed) nebo `preload_head` s offsetem `resonance_start_frame` (u FullyLoaded). Po vyčerpání preloadu pokračuje z ring bufferu (`stream_resonance_`). Viz `engine/voice/resonance_voice.cpp`. |
| **Streaming — StreamEngine** | Disk-streaming worker v `engine/stream/stream_engine.cpp` volá `readWavRange()` přímo k plnění ring bufferů za preload_head / preload_resonance regionem. Sdílí stejnou funkci s Loaderem. |
| **Batch renderer** | `engine/render/batch_renderer.cpp` volá `writeWavStereo16()` k zápisu výsledku offline renderování. |

---

## Nálezy revize

### 1. Edge-case detekce formátu — smíšená banka (FixedVelocity vs Extended)

`scanBank()` rozhoduje formát podle prosté majoritní logiky (`fixed_count >= extended_count`). Soubory menšinového formátu jsou **tiše zahozeny** — ani nepropadnou do `skipped`, jen se vůbec nezařadí do `scan.files`. Pokud banka obsahuje smíšené soubory (např. z neúplné migrace), ztráta souborů není viditelná v logu; `skipped` odráží jen soubory neodpovídající žádnému vzoru, nikoliv soubory odmítnutého formátu. Navíc remíza (stejný počet) dává přednost FixedVelocity, což může být překvapivé při migraci.

### 2. FullyLoaded přeřazení po oříznutém souboru (ingestSampleFile)

Po zjištění, že soubor vrátil méně framů než `head_frames`, se nastaví `mic.head_frames = head.frames` a podmíněně `mic.mode = FullyLoaded` (pokud `head_frames >= file.frames`). Avšak `mic.file.frames` zůstane na hodnotě z `peekWavInfo` (tj. původní, potenciálně vyšší číslo). Po přeřazení do FullyLoaded tedy `mic.file.frames > mic.head_frames` — audio vlákno (`Voice`) porovnává pozici s `mic.file.frames`, což by mohlo způsobit, že hlas očekává více dat než je v `preload_head`. Závisí na tom, zda `Voice` testuje délku podle `head_frames` nebo `file.frames`; pokud `file.frames`, hlas by se mohl pokusit číst za koncem bufferu (potenciální OOB přístup v audio vlákně).

### 3. Použití peak RMS z preload_head jako RMS velocity vrstvy

`sortBankSlotsByRms()` řadí podle `VelocitySlot.rms_db`, který je nastaven z `measurePeakRmsDb()` volané nad `preload_head`. Kód obsahuje INVARIANT komentář: pro piano-class samply je peak RMS vždy v attack fázi, která se vejde do preload head. To platí pro typická klavírní data. Pro non-piano zvuky s pomalým nástupem (looped pady, smyčce) by peak RMS mohl ležet mimo preload_head — pak by `rms_db` bylo podhodnoceno a velocity řazení nesprávné. Komentář toto explicitně zmiňuje jako budoucí problém fáze 5+.

### 4. Hranice short_threshold a možná nekonzistence mode u borderline samplů

`short_threshold_frames = preload_frames * 2`. Sampl přesně roven `short_threshold_frames` je označen jako FullyLoaded (`info.frames <= short_threshold`), ale `head_frames = info.frames`. Pokud by pak `readWavRange` vrátil méně framů (oříznutý soubor), přeřazení proběhne, ale viz bod 2 výše.

### 5. WAV parser předpokládá little-endian platformu

`parseHeader()` čte `sample_rate` přímo přes `memcpy` bez byte-swap. Komentář v kódu toto dokumentuje a uvádí, že big-endian port by vyžadoval byte-swap. Pro aktuální cílové platformy (x86/ARM/RPi) to není problém.
