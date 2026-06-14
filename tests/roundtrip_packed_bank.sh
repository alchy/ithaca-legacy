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

# -- licensed varianta: bake se secretem z secret/bank_secret.key (tyz, ktery je
#    zkompilovan v ithaca-cli pri buildu) → engine ho desifruje. --
SK="$ROOT/secret/bank_secret.key"
if [ ! -f "$SK" ]; then
    echo "CHYBA: chybi $SK (mel ho vygenerovat build); preskakuji licensed cast"
    exit 1
fi
LIC_DST="$WORK/lic"; mkdir -p "$LIC_DST"
printf '%s' '{"bank_name":"rt","owner_email":"a@b.cz","owner_name":"A","transaction_id":"TX","issued_at":"2026-06-14T00:00:00"}' \
    > "$WORK/license.json"
python3 "$ROOT/tools/bake_soundbank.py" \
    --source-soundbank-dir "$SRC" --destination-soundbank-dir "$LIC_DST" \
    --license-json "$WORK/license.json" --secret-file "$SK" --verify

"$CLI" --inspect "$LIC_DST" >"$WORK/lic.txt" 2>&1 \
    || { echo "CHYBA: inspect licensed banky selhal"; cat "$WORK/lic.txt"; exit 1; }
grep -q "Format: packed-ithaca" "$WORK/lic.txt" \
    || { echo "CHYBA: licensed banka neni packed-ithaca"; cat "$WORK/lic.txt"; exit 1; }
python3 - "$WORK/lic.txt" <<'PYEOF'
import re, sys
s = open(sys.argv[1]).read()
m = re.search(r"Celkem samplu: (\d+)", s)
assert m and int(m.group(1)) == 3, "licensed: spatny pocet samplu\n" + s
print("LICENSED LOAD OK")
PYEOF

# Tamper: edit license → engine ji odmitne (banka nenactena → --inspect exit 1).
printf '%s' '{"tampered":true}' > "$LIC_DST/license.ithaca"
if "$CLI" --inspect "$LIC_DST" >/dev/null 2>&1; then
    echo "CHYBA: editovana license se nacetla (mela selhat)"; exit 1
fi
echo "LICENSED TAMPER REJECTED OK"
echo "roundtrip_packed_bank: OK"
