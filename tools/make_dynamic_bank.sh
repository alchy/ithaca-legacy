#!/usr/bin/env bash
# tools/make_dynamic_bank.sh
# -----------------------------------------------------------------------------
# Prevede plochou multi-velocity banku (napr. sp-customgrand-16 s nazvy
# m<MIDI>-v<VEL>-f<SR>.wav) do DYNAMIC-VELOCITY folder formatu:
#
#   <OUT>/m<MIDI>/<md5-16>.wav
#
# Pro kazdy WAV: MD5 obsahu -> prvnich 16 hex znaku + ".wav", ulozeno do
# podslozky podle puvodni MIDI noty. Nazvy velocity se zahazuji (engine si
# poradi velocity vrstvy sam z mereneho RMS).
#
# Pouziti:
#   tools/make_dynamic_bank.sh <src-flat-bank> <out-dynamic-bank> [thin]
#
#   thin  (volitelne) -> v kazde note ponecha jen variabilni pocet samplu
#         (4..16 dle MIDI), aby slo overit dynamicky pocet souboru.
#
# Vstup se NEMENI; zapisuje se jen do <out-dynamic-bank>.
set -euo pipefail

SRC="${1:?usage: make_dynamic_bank.sh <src-flat-bank> <out-dynamic-bank> [thin]}"
OUT="${2:?usage: make_dynamic_bank.sh <src-flat-bank> <out-dynamic-bank> [thin]}"
THIN="${3:-}"

[ -d "$SRC" ] || { echo "ERR: zdrojovy adresar neexistuje: $SRC" >&2; exit 1; }

# md5 helper (macOS: md5 -q; Linux: md5sum).
md5hex() {
    if command -v md5 >/dev/null 2>&1; then md5 -q "$1"
    else md5sum "$1" | cut -d' ' -f1; fi
}

shopt -s nullglob
mkdir -p "$OUT"

copied=0
for f in "$SRC"/m[0-9][0-9][0-9]-*.wav; do
    base=$(basename "$f")
    midi="${base:0:4}"                 # "m060"
    h=$(md5hex "$f" | cut -c1-16)
    mkdir -p "$OUT/$midi"
    cp "$f" "$OUT/$midi/$h.wav"
    copied=$((copied + 1))
done
echo "make_dynamic_bank: zkopirovano $copied samplu -> $OUT"

if [ "$THIN" = "thin" ]; then
    pruned=0
    for d in "$OUT"/m[0-9][0-9][0-9]; do
        mnum=$(basename "$d"); mnum="${mnum#m}"; mnum=$((10#$mnum))
        keep=$(( 4 + mnum % 13 ))       # variabilni 4..16 dle MIDI (deterministicke)
        i=0
        while IFS= read -r w; do
            i=$((i + 1))
            if [ "$i" -gt "$keep" ]; then rm -f "$w"; pruned=$((pruned + 1)); fi
        done < <(ls "$d"/*.wav | sort)
    done
    echo "make_dynamic_bank: thin rezim, smazano $pruned samplu (variabilni pocet 4..16)"
fi

echo "Hotovo: $OUT"
