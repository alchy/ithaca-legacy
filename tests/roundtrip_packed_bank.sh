#!/usr/bin/env bash
# tests/roundtrip_packed_bank.sh <cesta-k-ithaca-cli> <repo-root>
# End-to-end: vygeneruj fixture dynamickou banku -> bake_soundbank.py --verify
# (hashe + bit-exact extrakce) -> ithaca-cli --inspect nad pakovanou i
# adresarovou variantou; over format packed-ithaca a shodny pocet samplu.
# Bez --dump-bank-index (zrusen) — RMS paritu kryje python self-test.
set -euo pipefail
CLI="$1"; ROOT="$2"

# numpy je potreba pro bake; bez nej test SKIP (ctest SKIP_RETURN_CODE=77).
if ! python3 -c "import numpy" 2>/dev/null; then
    echo "SKIP: python3 nema numpy (bake_soundbank.py ho vyzaduje)"
    exit 77
fi

WORK="$(mktemp -d "${TMPDIR:-/tmp}/ithaca_roundtrip.XXXXXX")"
trap 'rm -rf "$WORK"' EXIT
SRC="$WORK/src"; DST="$WORK/dst"
mkdir -p "$SRC/m060" "$SRC/m064" "$DST"

# Fixture: 2 vrstvy noty 60 (ruzna amplituda -> ruzna RMS) + 1 vrstva noty 64.
python3 - "$SRC" <<'PYEOF'
import math, struct, sys, wave
src = sys.argv[1]
for path, amp, frames in [(f"{src}/m060/aa11.wav",  6000, 30000),
                          (f"{src}/m060/bb22.wav", 24000, 30000),
                          (f"{src}/m064/cc33.wav", 12000, 20000)]:
    f = wave.open(path, "wb")
    f.setnchannels(2); f.setsampwidth(2); f.setframerate(48000)
    f.writeframes(b"".join(
        struct.pack("<hh", int(amp * math.sin(i * 0.03)),
                    int(amp * math.sin(i * 0.031)))
        for i in range(frames)))
    f.close()
PYEOF

python3 "$ROOT/tools/bake_soundbank.py" \
    --source-soundbank-dir "$SRC" --destination-soundbank-dir "$DST" --verify

# --inspect: Info jde na stdout, Warning+ na stderr; zachytime oboji.
"$CLI" --inspect "$DST" >"$WORK/packed.txt" 2>&1 \
    || { echo "CHYBA: inspect pakovane banky selhal"; cat "$WORK/packed.txt"; exit 1; }
"$CLI" --inspect "$SRC" >"$WORK/src.txt" 2>&1 \
    || { echo "CHYBA: inspect adresarove banky selhal"; cat "$WORK/src.txt"; exit 1; }

grep -q "Format: packed-ithaca" "$WORK/packed.txt" \
    || { echo "CHYBA: pakovana banka nema format packed-ithaca"; \
         cat "$WORK/packed.txt"; exit 1; }

python3 - "$WORK/packed.txt" "$WORK/src.txt" <<'PYEOF'
import re, sys
def count(path):
    s = open(path).read()
    m = re.search(r"Celkem samplu: (\d+)", s)
    assert m, "nenalezeno 'Celkem samplu' v " + path + ":\n" + s
    return int(m.group(1))
p = count(sys.argv[1]); s = count(sys.argv[2])
assert p == s == 3, f"pocet samplu: packed={p}, src={s}, ocekavano 3"
print(f"ROUNDTRIP OK (packed={p}, src={s})")
PYEOF
echo "roundtrip_packed_bank: OK"
