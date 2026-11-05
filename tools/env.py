#!/usr/bin/env python3
"""How fast something comes in: the time from AT seconds until a 24-bit stereo WAV's 20 ms RMS reaches -6 dB and
   -1 dB of its level at AT+SETTLE (default 2 s), and to -40 dB (the first sound).
   tools/env.py a.wav AT [SETTLE]"""
import sys, wave, math
p, at = sys.argv[1], float(sys.argv[2])
settle = float(sys.argv[3]) if len(sys.argv) > 3 else 2.0
w = wave.open(p); sr = w.getframerate()
w.setpos(int(at * sr)); d = w.readframes(int((settle + 0.2) * sr))
m = [(int.from_bytes(d[i:i+3], 'little', signed=True) + int.from_bytes(d[i+3:i+6], 'little', signed=True)) / 16777216
     for i in range(0, len(d), 6)]
win = int(0.02 * sr)
rms = [math.sqrt(sum(x * x for x in m[i:i + win]) / win + 1e-20) for i in range(0, len(m) - win, win // 4)]
ref = max(rms[int(settle / 0.005) - 20:int(settle / 0.005)])
db = lambda x: 20 * math.log10(x / ref + 1e-20)
def first(th):
    for k, r in enumerate(rms):
        if db(r) >= th:
            return f"{k * 0.005 * 1000:6.0f} ms"
    return "  never"
print(f"{p.split('/')[-1]:14s} -40 dB {first(-40)}   -6 dB {first(-6)}   -1 dB {first(-1)}")
