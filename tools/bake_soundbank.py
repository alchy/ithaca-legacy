#!/usr/bin/env python3
"""bake_soundbank.py — zabali dynamickou banku do soundbank.ithaca (v1).

Format (LE): [hlavicka 408 B][metadata JSON][index 64 B/zaznam][names]
[blob: doslovne WAVy zarovnane na 4096 B]. Layout viz
docs/bank-format-packed.md.

Analyzu (rms_db, attack_end) pocita nastroj SAM v numpy — replikuje algoritmus
enginu (engine/sample/sample_loader.cpp): klouzave okno 50 ms, hop = pul okna,
mono mix 0.5*(L+R), 20*log10(max_rms), podlaha -120 dB, mereno jen pres preload
head [0, head_frames). Tim se zachova poradi velocity vrstev shodne s
adresarovym nactenim (parita overena python self-testem).

Pouziti:
  python3 tools/bake_soundbank.py \
      --source-soundbank-dir <dynamicka banka> \
      --destination-soundbank-dir <cil> [--preload-ms 150] [--verify] [--force]
"""
import argparse
import hashlib
import json
import math
import os
import struct
import sys
import time

import numpy as np

MAGIC = b"ITHACABK"
VERSION = 1
HEADER_SIZE = 408
ENTRY_SIZE = 64
BLOB_ALIGN = 4096
NO_NAME = 0xFFFFFFFF
FMT_PCM16, FMT_PCM24, FMT_FLOAT32, FMT_PCM32 = 1, 2, 3, 4
TOOL_VERSION = "1.0"

SILENCE_FLOOR_DB = -120.0
WINDOW_MS = 50.0


class BakeError(Exception):
    pass


def parse_riff(path):
    """Vrati dict: channels, sample_rate, sample_format, frames,
    pcm_data_offset, entry_size. BakeError pri nepodporovanem formatu."""
    size = os.path.getsize(path)
    with open(path, "rb") as f:
        if f.read(4) != b"RIFF":
            raise BakeError(f"{path}: neni RIFF")
        f.seek(4, 1)
        if f.read(4) != b"WAVE":
            raise BakeError(f"{path}: neni WAVE")
        fmt = None
        while True:
            hdr = f.read(8)
            if len(hdr) < 8:
                raise BakeError(f"{path}: chybi data chunk")
            cid, csz = hdr[:4], struct.unpack("<I", hdr[4:])[0]
            if cid == b"fmt ":
                raw = f.read(min(csz, 16))
                audio_format, channels, sample_rate = struct.unpack("<HHI", raw[:8])
                bits = struct.unpack("<H", raw[14:16])[0]
                fmt = (audio_format, channels, sample_rate, bits)
                if csz > 16:
                    f.seek(csz - 16, 1)
            elif cid == b"data":
                if fmt is None:
                    raise BakeError(f"{path}: data pred fmt chunkem")
                audio_format, channels, sample_rate, bits = fmt
                if channels not in (1, 2):
                    raise BakeError(f"{path}: channels={channels}")
                if (audio_format, bits) == (1, 16):
                    sample_format = FMT_PCM16
                elif (audio_format, bits) == (1, 24):
                    sample_format = FMT_PCM24
                elif (audio_format, bits) == (3, 32):
                    sample_format = FMT_FLOAT32
                elif (audio_format, bits) == (1, 32):
                    sample_format = FMT_PCM32
                else:
                    raise BakeError(
                        f"{path}: nepodporovany format ({audio_format}/{bits})")
                data_off = f.tell()
                avail = max(0, size - data_off)
                data_sz = min(csz, avail)
                frame_bytes = channels * (bits // 8)
                return {"channels": channels, "sample_rate": sample_rate,
                        "sample_format": sample_format,
                        "frames": data_sz // frame_bytes,
                        "pcm_data_offset": data_off, "entry_size": size}
            else:
                f.seek(csz + (csz & 1), 1)


# -- Analyza (replika engine/sample/sample_loader.cpp) --

def head_frames_for(frames, sample_rate, preload_ms):
    """Region, pres ktery engine meri RMS/attack (preload head)."""
    preload_frames = int(preload_ms * sample_rate / 1000)
    if frames <= preload_frames * 2:
        return frames
    return preload_frames


def read_head_mono(path, info, head_frames):
    """Precte [0, head_frames) → mono float64 (0.5*(L+R); mono soubor = vzorek).
    Normalizace shodna s wavSampleToFloat."""
    sf = info["sample_format"]
    channels = info["channels"]
    bps = {FMT_PCM16: 2, FMT_PCM24: 3, FMT_FLOAT32: 4, FMT_PCM32: 4}[sf]
    want_bytes = head_frames * channels * bps
    with open(path, "rb") as f:
        f.seek(info["pcm_data_offset"])
        data = f.read(want_bytes)
    avail_frames = len(data) // (channels * bps)
    data = data[:avail_frames * channels * bps]
    n = avail_frames * channels
    if n == 0:
        return np.zeros(0, dtype=np.float64)
    if sf == FMT_PCM16:
        a = np.frombuffer(data, dtype="<i2", count=n).astype(np.float64) / 32768.0
    elif sf == FMT_PCM32:
        a = np.frombuffer(data, dtype="<i4", count=n).astype(np.float64) / 2147483648.0
    elif sf == FMT_FLOAT32:
        a = np.frombuffer(data, dtype="<f4", count=n).astype(np.float64)
    else:   # FMT_PCM24
        raw = np.frombuffer(data, dtype=np.uint8, count=n * 3).reshape(-1, 3).astype(np.int32)
        vals = raw[:, 0] | (raw[:, 1] << 8) | (raw[:, 2] << 16)
        vals = vals - ((vals & 0x800000) << 1)   # sign-extend 24-bit
        a = vals.astype(np.float64) / 8388608.0
    if channels == 2:
        a = a.reshape(-1, 2)
        return 0.5 * (a[:, 0] + a[:, 1])
    return a


def sliding_peak_rms(mono, sample_rate):
    """(max_rms, peak_frame) klouzavym oknem 50 ms, hop = pul okna, strict >."""
    frames = int(len(mono))
    if frames <= 0:
        return (0.0, 0)
    win = int(WINDOW_MS * 0.001 * sample_rate)
    if win < 1:
        win = 1
    if win > frames:
        win = frames
    hop = win // 2 if win > 1 else 1
    sq = mono * mono
    max_rms = 0.0
    peak_frame = 0
    start = 0
    while start + win <= frames:
        rms = math.sqrt(float(np.sum(sq[start:start + win])) / win)
        if rms > max_rms:
            max_rms = rms
            peak_frame = start + win // 2
        start += hop
    return (max_rms, peak_frame)


def measure_peak_rms_db(mono, sample_rate):
    if len(mono) <= 0:
        return SILENCE_FLOOR_DB
    max_rms, _ = sliding_peak_rms(mono, sample_rate)
    if max_rms <= 0.0:
        return SILENCE_FLOOR_DB
    db = 20.0 * math.log10(max_rms)
    return db if db > SILENCE_FLOOR_DB else SILENCE_FLOOR_DB


def find_attack_end(mono, sample_rate):
    if len(mono) <= 0:
        return 0
    _, peak_frame = sliding_peak_rms(mono, sample_rate)
    return peak_frame


def analyze_bank(src_dir, preload_ms):
    """Projde m###/*.wav, spocita rms_db + attack_end (engine algoritmus).
    Vraci list dictu {path, midi, frames, sample_rate, rms_db, attack_end}."""
    analysis = []
    for note_dir in sorted(os.listdir(src_dir)):
        full = os.path.join(src_dir, note_dir)
        if not os.path.isdir(full):
            continue
        if not (note_dir.lower().startswith("m") and note_dir[1:].isdigit()):
            continue
        midi = int(note_dir[1:])
        if midi > 127:
            continue
        for fn in sorted(os.listdir(full)):
            if not fn.lower().endswith(".wav"):
                continue
            path = os.path.join(full, fn)
            info = parse_riff(path)
            # Engine validator (ithaca_bank.cpp) odmita frames<=0 a tim CELOU
            # banku — zachyt prazdny/poskozeny WAV uz tady s presnym jmenem.
            if info["frames"] <= 0:
                raise BakeError(f"{path}: 0 frames (prazdny/poskozeny WAV)")
            hf = head_frames_for(info["frames"], info["sample_rate"], preload_ms)
            mono = read_head_mono(path, info, hf)
            max_rms, peak_frame = sliding_peak_rms(mono, info["sample_rate"])
            if max_rms <= 0.0:
                rms_db = SILENCE_FLOOR_DB
            else:
                rms_db = max(20.0 * math.log10(max_rms), SILENCE_FLOOR_DB)
            analysis.append({"path": path, "midi": midi,
                             "frames": info["frames"],
                             "sample_rate": info["sample_rate"],
                             "rms_db": rms_db, "attack_end": peak_frame})
    if not analysis:
        raise BakeError(f"{src_dir}: zadne m###/*.wav soubory")
    return analysis


def compute_layout(analysis, metadata_bytes):
    """Spocita offsety sekci + zaznamy. analysis: list dictu obohaceny o RIFF
    pole (parse_riff). Vraci (hdr, entries, names_blob)."""
    entries = sorted(analysis, key=lambda a: (a["midi"], a["rms_db"], a["path"]))
    names_blob = b""
    for e in entries:
        name = os.path.basename(e["path"]).encode("utf-8")
        e["name_offset"] = len(names_blob)
        names_blob += struct.pack("<H", len(name)) + name
    metadata_offset = HEADER_SIZE
    index_offset = metadata_offset + len(metadata_bytes)
    index_size = len(entries) * ENTRY_SIZE
    names_offset = index_offset + index_size
    names_size = len(names_blob)
    blob_offset = names_offset + names_size
    blob_offset = (blob_offset + BLOB_ALIGN - 1) // BLOB_ALIGN * BLOB_ALIGN
    off = blob_offset
    for e in entries:
        e["entry_offset"] = off
        off += e["entry_size"]
        off = (off + BLOB_ALIGN - 1) // BLOB_ALIGN * BLOB_ALIGN
    blob_size = (entries[-1]["entry_offset"] + entries[-1]["entry_size"]
                 - blob_offset) if entries else 0
    hdr = {"metadata_offset": metadata_offset, "metadata_size": len(metadata_bytes),
           "index_offset": index_offset, "index_size": index_size,
           "names_offset": names_offset, "names_size": names_size,
           "blob_offset": blob_offset, "blob_size": blob_size,
           "entry_count": len(entries)}
    return hdr, entries, names_blob


def pack_entry(e):
    # 64 B: sample_format u16, reserved u16 (=0), frames i64...
    return struct.pack("<HHIQQIHHqfII12x",
                       e["midi"], e["channels"], e["sample_rate"],
                       e["entry_offset"], e["entry_size"],
                       e["pcm_data_offset"], e["sample_format"], 0,
                       e["frames"], e["rms_db"], e["attack_end"], e["name_offset"])


def write_ithaca(out_path, analysis, bank_name, analysis_preload_ms,
                 created_at="", progress=None):
    """Zapise soundbank.ithaca. analysis: list dictu {path, midi, frames,
    sample_rate, rms_db, attack_end}. RIFF layout pole doplni z parse_riff."""
    if not analysis:
        raise BakeError("prazdna analyza (zadne soubory)")
    for a in analysis:
        riff = parse_riff(a["path"])
        if riff["frames"] != a["frames"] or riff["sample_rate"] != a["sample_rate"]:
            raise BakeError(
                f"{a['path']}: neshoda analyzy vs RIFF "
                f"(frames {a['frames']}/{riff['frames']}, "
                f"sr {a['sample_rate']}/{riff['sample_rate']})")
        a.update(riff)
    metadata = json.dumps({
        "bank_name": bank_name, "created_at": created_at,
        "bake_tool_version": TOOL_VERSION,
        "analysis_preload_ms": analysis_preload_ms,
        "source_format": "dynamic"}).encode("utf-8")
    hdr, entries, names_blob = compute_layout(analysis, metadata)
    index_bytes = b"".join(pack_entry(e) for e in entries)
    sha_index = hashlib.sha256(metadata + index_bytes + names_blob).digest()

    with open(out_path, "wb") as f:
        header = struct.pack("<8sII", MAGIC, VERSION, 0)
        header += struct.pack("<8Q", hdr["metadata_offset"], hdr["metadata_size"],
                              hdr["index_offset"], hdr["index_size"],
                              hdr["names_offset"], hdr["names_size"],
                              hdr["blob_offset"], hdr["blob_size"])
        header += struct.pack("<II", hdr["entry_count"], 0)
        header += sha_index + b"\x00" * 32 + b"\x00" * 256
        assert len(header) == HEADER_SIZE
        f.write(header + metadata + index_bytes + names_blob)
        f.write(b"\x00" * (hdr["blob_offset"] - f.tell()))
        sha_payload = hashlib.sha256()
        for i, e in enumerate(entries):
            assert f.tell() == e["entry_offset"]
            with open(e["path"], "rb") as src:
                while True:
                    chunk = src.read(1 << 20)
                    if not chunk:
                        break
                    f.write(chunk)
                    sha_payload.update(chunk)
            pad = -(f.tell() - hdr["blob_offset"]) % BLOB_ALIGN
            if i == len(entries) - 1:
                pad = 0
            f.write(b"\x00" * pad)
            sha_payload.update(b"\x00" * pad)
            if progress:
                progress(i + 1, len(entries))
        f.seek(120)
        f.write(sha_payload.digest())


def read_ithaca_header(path):
    with open(path, "rb") as f:
        hb = f.read(HEADER_SIZE)
        if len(hb) < HEADER_SIZE or hb[:8] != MAGIC:
            raise BakeError(f"{path}: neni soundbank.ithaca")
        version, flags = struct.unpack("<II", hb[8:16])
        (metadata_offset, metadata_size, index_offset, index_size,
         names_offset, names_size, blob_offset, blob_size) = struct.unpack(
            "<8Q", hb[16:80])
        entry_count = struct.unpack("<I", hb[80:84])[0]
        sha_index, sha_payload = hb[88:120], hb[120:152]
        f.seek(metadata_offset); metadata = f.read(metadata_size)
        f.seek(index_offset);    index_bytes = f.read(index_size)
        f.seek(names_offset);    names = f.read(names_size)
        if hashlib.sha256(metadata + index_bytes + names).digest() != sha_index:
            raise BakeError(f"{path}: hash indexu nesouhlasi")
        entries = []
        for i in range(entry_count):
            p = index_bytes[i * ENTRY_SIZE:(i + 1) * ENTRY_SIZE]
            (midi, channels, sample_rate, entry_offset, entry_size,
             pcm_data_offset, sample_format, _rsv, frames, rms_db,
             attack_end, name_offset) = struct.unpack("<HHIQQIHHqfII", p[:52])
            entries.append({"midi": midi, "channels": channels,
                            "sample_rate": sample_rate, "entry_offset": entry_offset,
                            "entry_size": entry_size, "pcm_data_offset": pcm_data_offset,
                            "sample_format": sample_format, "frames": frames,
                            "rms_db": rms_db, "attack_end": attack_end,
                            "name_offset": name_offset})
        return {"version": version, "flags": flags, "entry_count": entry_count,
                "blob_offset": blob_offset, "blob_size": blob_size,
                "sha_payload": sha_payload,
                "metadata": json.loads(metadata.decode("utf-8")), "entries": entries}


def verify_ithaca(path, analysis):
    """Overi oba hashe + bit-exact extrakci kazdeho zaznamu proti zdrojum."""
    hdr = read_ithaca_header(path)
    sha = hashlib.sha256()
    with open(path, "rb") as f:
        f.seek(hdr["blob_offset"])
        remain = hdr["blob_size"]
        while remain > 0:
            chunk = f.read(min(1 << 20, remain))
            if not chunk:
                raise BakeError(f"{path}: blob kratsi nez blob_size")
            sha.update(chunk)
            remain -= len(chunk)
    if sha.digest() != hdr["sha_payload"]:
        raise BakeError(f"{path}: hash blobu nesouhlasi")
    by_size = {}
    for a in analysis:
        by_size.setdefault((a["midi"], os.path.getsize(a["path"])), []).append(a)
    with open(path, "rb") as f:
        for e in hdr["entries"]:
            cands = by_size.get((e["midi"], e["entry_size"]), [])
            if not cands:
                raise BakeError(f"verify: zaznam midi {e['midi']} bez zdroje")
            f.seek(e["entry_offset"])
            packed = f.read(e["entry_size"])
            matched = False
            for c in cands:
                with open(c["path"], "rb") as cf:   # with: bez leaku fd na velke bance
                    if packed == cf.read():
                        matched = True
                        break
            if not matched:
                raise BakeError(f"verify: data midi {e['midi']} nesedi se zdrojem")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--source-soundbank-dir", required=True)
    ap.add_argument("--destination-soundbank-dir", required=True)
    ap.add_argument("--preload-ms", type=int, default=150)
    ap.add_argument("--verify", action="store_true")
    ap.add_argument("--force", action="store_true")
    args = ap.parse_args()

    src = args.source_soundbank_dir
    if not os.path.isdir(src):
        sys.exit(f"CHYBA: zdrojovy adresar neexistuje: {src}")
    has_note_dirs = any(
        n.lower().startswith("m") and n[1:].isdigit()
        and os.path.isdir(os.path.join(src, n)) for n in os.listdir(src))
    if not has_note_dirs:
        sys.exit(f"CHYBA: {src} neni dynamicka banka (zadne m<NNN>/ slozky); "
                 "fixni banku preved tools/make_dynamic_bank.sh")
    os.makedirs(args.destination_soundbank_dir, exist_ok=True)
    out = os.path.join(args.destination_soundbank_dir, "soundbank.ithaca")
    if os.path.exists(out) and not args.force:
        sys.exit(f"CHYBA: {out} existuje (pouzij --force)")

    print(f"Analyza banky {src} (preload {args.preload_ms} ms)...")
    analysis = analyze_bank(src, args.preload_ms)
    print(f"Analyza: {len(analysis)} souboru")
    write_ithaca(out, analysis,
                 bank_name=os.path.basename(os.path.normpath(src)),
                 analysis_preload_ms=args.preload_ms,
                 created_at=time.strftime("%Y-%m-%dT%H:%M:%S"),
                 progress=lambda d, t: print(f"\r  pack {d}/{t}", end="", flush=True))
    print(f"\nZapsano: {out} ({os.path.getsize(out)} B)")
    if args.verify:
        verify_ithaca(out, analysis)
        print("Verify OK (hash indexu, hash blobu, bit-exact extrakce)")


if __name__ == "__main__":
    main()
