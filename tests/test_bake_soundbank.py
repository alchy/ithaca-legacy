#!/usr/bin/env python3
# tests/test_bake_soundbank.py
# Unit testy bake nastroje: RMS/attack algoritmus (replika sample_loader.cpp),
# RIFF parse, analyze_bank (poradi dle midi,rms), layout, write/verify.
import math
import os
import struct
import sys
import tempfile
import unittest
import wave

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "tools"))
import bake_soundbank as bake


def make_wav(path, frames=1000, sr=48000, amp=8000):
    # Sawtooth-ish (pro write/verify testy — rms se injektuje).
    f = wave.open(path, "wb")
    f.setnchannels(2); f.setsampwidth(2); f.setframerate(sr)
    f.writeframes(b"".join(struct.pack("<hh", (i * amp) % 32767, -((i * amp) % 32767))
                           for i in range(frames)))
    f.close()


def make_const_wav(path, frames, sr, value, channels=2):
    # Konstantni hodnota — predikovatelne RMS.
    f = wave.open(path, "wb")
    f.setnchannels(channels); f.setsampwidth(2); f.setframerate(sr)
    if channels == 2:
        frame = struct.pack("<hh", value, value)
    else:
        frame = struct.pack("<h", value)
    f.writeframes(frame * frames)
    f.close()


class TestParseRiff(unittest.TestCase):
    def test_parse_pcm16(self):
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, "a.wav")
            make_wav(p, frames=1000, sr=44100)
            info = bake.parse_riff(p)
            self.assertEqual(info["channels"], 2)
            self.assertEqual(info["sample_rate"], 44100)
            self.assertEqual(info["sample_format"], bake.FMT_PCM16)
            self.assertEqual(info["frames"], 1000)
            self.assertGreaterEqual(info["pcm_data_offset"], 44)
            self.assertEqual(info["entry_size"], os.path.getsize(p))

    def test_reject_non_wav(self):
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, "x.wav")
            with open(p, "wb") as f:
                f.write(b"not a wav at all")
            with self.assertRaises(bake.BakeError):
                bake.parse_riff(p)


class TestAnalysis(unittest.TestCase):
    def test_rms_constant_signal(self):
        # Konstantni PCM16 stereo 16384 → float 0.5 → rms 0.5 → 20log10(0.5).
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, "c.wav")
            make_const_wav(p, frames=5000, sr=48000, value=16384)
            info = bake.parse_riff(p)
            hf = bake.head_frames_for(info["frames"], info["sample_rate"], 150)
            mono = bake.read_head_mono(p, info, hf)
            db = bake.measure_peak_rms_db(mono, info["sample_rate"])
            self.assertAlmostEqual(db, 20.0 * math.log10(0.5), places=2)

    def test_attack_constant_is_first_window_center(self):
        # Konstantni signal: prvni okno vyhrava (strict >), peak = win//2.
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, "c.wav")
            make_const_wav(p, frames=5000, sr=48000, value=16384)
            info = bake.parse_riff(p)
            mono = bake.read_head_mono(p, info, 5000)
            win = int(50.0 * 0.001 * 48000)   # 2400
            ae = bake.find_attack_end(mono, info["sample_rate"])
            self.assertEqual(ae, win // 2)

    def test_silence_floor(self):
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, "z.wav")
            make_const_wav(p, frames=5000, sr=48000, value=0)
            info = bake.parse_riff(p)
            mono = bake.read_head_mono(p, info, 5000)
            db = bake.measure_peak_rms_db(mono, info["sample_rate"])
            self.assertEqual(db, bake.SILENCE_FLOOR_DB)

    def test_attack_in_loud_region(self):
        # Tiche [0,5000) + hlasite [5000,10000) → peak v hlasite casti.
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, "a.wav")
            f = wave.open(p, "wb")
            f.setnchannels(2); f.setsampwidth(2); f.setframerate(48000)
            quiet = struct.pack("<hh", 300, 300) * 5000
            loud  = struct.pack("<hh", 16000, 16000) * 5000
            f.writeframes(quiet + loud); f.close()
            info = bake.parse_riff(p)
            mono = bake.read_head_mono(p, info, 10000)
            ae = bake.find_attack_end(mono, info["sample_rate"])
            self.assertGreaterEqual(ae, 5000)

    def test_mono_file(self):
        # Mono konstantni 16384 → mono mix = sam vzorek = 0.5.
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, "m.wav")
            make_const_wav(p, frames=5000, sr=48000, value=16384, channels=1)
            info = bake.parse_riff(p)
            self.assertEqual(info["channels"], 1)
            mono = bake.read_head_mono(p, info, 5000)
            db = bake.measure_peak_rms_db(mono, info["sample_rate"])
            self.assertAlmostEqual(db, 20.0 * math.log10(0.5), places=2)


class TestAnalyzeBank(unittest.TestCase):
    def test_orders_by_midi_then_rms(self):
        with tempfile.TemporaryDirectory() as d:
            src = os.path.join(d, "src")
            os.makedirs(os.path.join(src, "m060"))
            os.makedirs(os.path.join(src, "m072"))
            make_const_wav(os.path.join(src, "m060", "loud.wav"), 5000, 48000, 20000)
            make_const_wav(os.path.join(src, "m060", "quiet.wav"), 5000, 48000, 2000)
            make_const_wav(os.path.join(src, "m072", "x.wav"), 5000, 48000, 10000)
            analysis = bake.analyze_bank(src, 150)
            # Predrazeno dle (midi, rms): m060 quiet pred m060 loud, pak m072.
            order = [(a["midi"], round(a["rms_db"], 2)) for a in
                     sorted(analysis, key=lambda a: (a["midi"], a["rms_db"]))]
            self.assertEqual([o[0] for o in order], [60, 60, 72])
            self.assertLess(order[0][1], order[1][1])   # quiet < loud

    def test_rejects_empty(self):
        with tempfile.TemporaryDirectory() as d:
            with self.assertRaises(bake.BakeError):
                bake.analyze_bank(d, 150)


class TestBakeRoundtrip(unittest.TestCase):
    def _bake_minimal(self, d):
        src = os.path.join(d, "src")
        os.makedirs(os.path.join(src, "m060"))
        os.makedirs(os.path.join(src, "m072"))
        specs = [("m060/a.wav", 60, -30.0, 100, 3000),
                 ("m060/b.wav", 60, -20.0, 120, 3000),
                 ("m072/c.wav", 72, -25.0, 90, 2000)]
        analysis = []
        for rel, midi, rms, ae, frames in specs:
            p = os.path.join(src, rel)
            make_wav(p, frames=frames)
            analysis.append({"path": p, "midi": midi, "frames": frames,
                             "sample_rate": 48000, "rms_db": rms, "attack_end": ae})
        out = os.path.join(d, "soundbank.ithaca")
        bake.write_ithaca(out, analysis, bank_name="test", analysis_preload_ms=150)
        return src, out, analysis

    def test_write_and_header(self):
        with tempfile.TemporaryDirectory() as d:
            _, out, analysis = self._bake_minimal(d)
            hdr = bake.read_ithaca_header(out)
            self.assertEqual(hdr["version"], 1)
            self.assertEqual(hdr["flags"], 0)
            self.assertEqual(hdr["entry_count"], 3)
            self.assertEqual(hdr["blob_offset"] % bake.BLOB_ALIGN, 0)
            self.assertEqual([e["midi"] for e in hdr["entries"]], [60, 60, 72])
            self.assertAlmostEqual(hdr["entries"][0]["rms_db"], -30.0, places=4)

    def test_verify_ok(self):
        with tempfile.TemporaryDirectory() as d:
            _, out, analysis = self._bake_minimal(d)
            bake.verify_ithaca(out, analysis)

    def test_verify_detects_corruption(self):
        with tempfile.TemporaryDirectory() as d:
            _, out, analysis = self._bake_minimal(d)
            with open(out, "r+b") as f:
                f.seek(os.path.getsize(out) - 1); f.write(b"\xff")
            with self.assertRaises(bake.BakeError):
                bake.verify_ithaca(out, analysis)


if __name__ == "__main__":
    unittest.main()
