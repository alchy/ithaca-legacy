#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# fetch-third-party.sh
# Stahne vendored deps pro ithaca-legacy do third-party/.
# Spustit jednou po cerstvem clonu (nebo po bumpu verzi). Idempotentni —
# uz stazene preskoci. Verze jsou pinned nize; pro upgrade bumpni _VER + re-run.
# ---------------------------------------------------------------------------
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TP="$REPO_ROOT/third-party"
TMP="$TP/.staging"

# -- Pinned verze ----------------------------------------------------------
DOCTEST_VER="v2.4.11"
NLOHMANN_VER="v3.11.3"
MINIAUDIO_VER="0.11.21"
RTMIDI_VER="6.0.0"

have() { command -v "$1" >/dev/null 2>&1; }
log()  { printf "\033[1;34m[fetch]\033[0m %s\n" "$*"; }
err()  { printf "\033[1;31m[ERROR]\033[0m %s\n" "$*" >&2; exit 1; }

have curl || err "curl je potreba"

mkdir -p "$TMP"
trap 'rm -rf "$TMP"' EXIT

# -- doctest (single header) -----------------------------------------------
log "doctest $DOCTEST_VER"
mkdir -p "$TP/doctest"
if [ ! -f "$TP/doctest/doctest.h" ]; then
    curl -fsSL "https://raw.githubusercontent.com/doctest/doctest/${DOCTEST_VER}/doctest/doctest.h" \
        -o "$TP/doctest/doctest.h"
    curl -fsSL "https://raw.githubusercontent.com/doctest/doctest/${DOCTEST_VER}/LICENSE.txt" \
        -o "$TP/doctest/LICENSE.txt"
    log "  ok doctest"
else
    log "  (uz je, skip)"
fi

# -- nlohmann/json (single header) -----------------------------------------
log "nlohmann/json $NLOHMANN_VER"
mkdir -p "$TP/nlohmann_json/nlohmann"
if [ ! -f "$TP/nlohmann_json/nlohmann/json.hpp" ]; then
    curl -fsSL "https://github.com/nlohmann/json/releases/download/${NLOHMANN_VER}/json.hpp" \
        -o "$TP/nlohmann_json/nlohmann/json.hpp"
    curl -fsSL "https://raw.githubusercontent.com/nlohmann/json/${NLOHMANN_VER}/LICENSE.MIT" \
        -o "$TP/nlohmann_json/LICENSE.MIT"
    log "  ok nlohmann/json"
else
    log "  (uz je, skip)"
fi

# -- miniaudio (single header) ---------------------------------------------
log "miniaudio $MINIAUDIO_VER"
mkdir -p "$TP/miniaudio"
if [ ! -f "$TP/miniaudio/miniaudio.h" ]; then
    curl -fsSL "https://raw.githubusercontent.com/mackron/miniaudio/${MINIAUDIO_VER}/miniaudio.h" \
        -o "$TP/miniaudio/miniaudio.h"
    curl -fsSL "https://raw.githubusercontent.com/mackron/miniaudio/${MINIAUDIO_VER}/LICENSE" \
        -o "$TP/miniaudio/LICENSE"
    log "  ok miniaudio"
else
    log "  (uz je, skip)"
fi

# -- RtMidi (maly zdrojak) -------------------------------------------------
log "RtMidi $RTMIDI_VER"
mkdir -p "$TP/rtmidi"
if [ ! -f "$TP/rtmidi/RtMidi.cpp" ]; then
    curl -fsSL "https://github.com/thestk/rtmidi/archive/refs/tags/${RTMIDI_VER}.tar.gz" \
        -o "$TMP/rtmidi.tar.gz"
    mkdir -p "$TMP/rtmidi-tmp"
    tar -xzf "$TMP/rtmidi.tar.gz" -C "$TMP/rtmidi-tmp" --strip-components=1
    cp "$TMP/rtmidi-tmp/RtMidi.cpp" "$TMP/rtmidi-tmp/RtMidi.h" \
        "$TMP/rtmidi-tmp/LICENSE" "$TP/rtmidi/"
    log "  ok RtMidi"
else
    log "  (uz je, skip)"
fi

log "Hotovo. Vendored deps v third-party/"
