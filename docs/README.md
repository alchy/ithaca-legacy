# Dokumentace ithaca — knížka

Tahle složka není sbírka odtržených poznámek — je to **knížka**, kterou lze číst
od začátku do konce. Vede tě od „co to je a jak to spustit" až po „jak to uvnitř
funguje", a na konci ti řekne, co se teprve chystá.

Začni [úvodem](prirucka/01-uvod.md) — vysvětlí, co ithaca dělá a jak teče signál
od klávesy ke zvuku. Zbytek navazuje.

> **Jazyk:** čeština s diakritikou (knížka se má dobře číst). Komentáře přímo v
> kódu drží projektovou konvenci „česky bez diakritiky"; identifikátory jsou
> anglicky.

---

## Část I — Provoz a tvorba bank

Praktická část pro toho, kdo chce ithacu postavit, rozjet a nakrmit vlastními
bankami.

| # | Kapitola | O čem je |
|---|----------|----------|
| 1 | [Co je ithaca a jak přemýšlí](prirucka/01-uvod.md) | Úvod, tok signálu, klíčové vlastnosti |
| 2 | [Build a Makefile](prirucka/02-build-makefile.md) | Orchestrace nad CMake, cíle, bank secret, bakování |
| 3 | [Raspberry Pi 5](prirucka/03-raspberry-pi-5.md) | Kompletní setup od karty po hraní |
| 4 | [Konfigurace (`state.json`)](prirucka/04-konfigurace.md) | Persistované nastavení GUI, migrace, CLI overrides |
| 5 | [Formát banky](prirucka/05-format-banky.md) | Model v paměti, tři podoby na disku, načítání, sample rate, bakování |

## Část II — Jak engine funguje uvnitř

Referenční část po **oblastech kontextu** — každá kapitola říká, které soubory
oblast implementují a jak. Rozcestník a tok signálu jsou v
[přehledu](reference/00-overview.md).

| # | Oblast | Kapitola |
|---|--------|----------|
| — | Přehled + tok signálu | [00 · Overview](reference/00-overview.md) |
| A | Core / scaffold | [A-core](reference/A-core.md) |
| B | Zpracování eventů | [B-events](reference/B-events.md) |
| C | Zpracování bufferu | [C-buffers](reference/C-buffers.md) |
| D | Polyfonie | [D-polyphony](reference/D-polyphony.md) |
| E | Rezonance | [E-resonance](reference/E-resonance.md) |
| F | Loader | [F-loader](reference/F-loader.md) |
| G | DSP | [G-dsp](reference/G-dsp.md) |
| H | GUI | [H-gui](reference/H-gui.md) |
| I | Multithreading | [I-multithreading](reference/I-multithreading.md) |
| J | RT priorita audio vlákna | [J-rt-priorita](reference/J-rt-priorita.md) |

## Dodatek — co se chystá

| Dokument | O čem je |
|----------|----------|
| [Plán a nedodělky](plan2do.md) | Vše navržené/odložené: multi-mic, runtime SRC, round-robin, partitioned-FFT convolver, testy… + nálezy k rozebrání v kódu |

---

## Jak je knížka udržovaná

- **Část II zrcadlí kód.** Když se mění engine, mění se i příslušná kapitola
  `reference/` — tabulky funkcí, konstanty a memory-ordering popisy mají sedět na
  zdroj. Kotvy `soubor:řádek` jsou orientační (kód se posouvá); spolehlivější je
  název symbolu.
- **„Bude implementováno" patří do [plan2do.md](plan2do.md)**, ne do výkladu.
  Kapitoly popisují *současný* stav; budoucí osy jen krátce zmíní s odkazem do
  plánu. Tím se výklad nerozjíždí s realitou.
- **Historie žije v gitu.** Dokončené plány a revize se neudržují jako
  dokumentace — po vytěžení do kódu a plánu se mažou; git je archiv.
