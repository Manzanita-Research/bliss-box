#!/usr/bin/env python3
"""Loudness of 24-bit stereo WAVs, for balancing voices: the RMS of 0..12 s and the loudest 400 ms window.
   tools/loud.py a.wav b.wav ..."""
import sys, wave, math, array
for p in sys.argv[1:]:
    w = wave.open(p); sr = w.getframerate(); d = w.readframes(w.getnframes())
    m = [(int.from_bytes(d[i:i+3], 'little', signed=True) + int.from_bytes(d[i+3:i+6], 'little', signed=True)) / 16777216
         for i in range(0, len(d), 6)]
    sq = [x * x for x in m]
    ph = sum(sq[:12 * sr]) / (12 * sr)
    win = int(0.4 * sr); s = sum(sq[:win]); best = s
    for i in range(win, len(sq), 64):
        s = sum(sq[i - win + 1:i + 1]) if i % (64 * 50) == 0 else s + sum(sq[i - 63:i + 1]) - sum(sq[i - win - 63:i - win + 1])
        best = max(best, s)
    db = lambda x: 10 * math.log10(x + 1e-20)
    print(f"{p.split('/')[-1]:14s} phrase {db(ph):6.1f}  loudest-400ms {db(best / win):6.1f}")
