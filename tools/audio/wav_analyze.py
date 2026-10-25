#!/usr/bin/env python3
"""Look for crackle, clipping and gaps in a RECOMP_AUDIO_WAV dump (16-bit PCM, 48 kHz stereo).

  py -3 tools/audio/wav_analyze.py dump.wav [--win 0.5]   (needs numpy)

Per window and in total it reports: clipped samples (|x| >= 32767), peak and RMS in dBFS,
exact-zero runs (dropouts: 2 ms or longer), and clicks (a sample step over 0.5 of full
scale, or over 12 sigma of the local step). A clean mix has no clipped samples, no zero runs
inside sound and few clicks; the worst windows are listed so they can be matched to game time.
"""
import argparse, sys, wave
import numpy as np


def db(x):
    return 20 * np.log10(x) if x > 0 else -120.0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("wav")
    ap.add_argument("--win", type=float, default=0.5, help="window seconds")
    a = ap.parse_args()
    w = wave.open(a.wav, "rb")
    ch, sr, sw = w.getnchannels(), w.getframerate(), w.getsampwidth()
    if sw != 2:
        sys.exit(f"expected 16-bit PCM, got {8*sw}-bit")
    x = np.frombuffer(w.readframes(w.getnframes()), dtype="<i2").reshape(-1, ch).astype(np.float32)
    n = len(x)
    secs = n / sr
    print(f"{a.wav}: {secs:.1f} s, {sr} Hz, {ch} ch")
    if not n:
        return
    clip = np.abs(x) >= 32767
    step = np.abs(np.diff(x, axis=0))
    big = (step > 16384).any(axis=1)
    zero = (x == 0).all(axis=1)
    # zero runs >= 2 ms inside the file
    zr = 0
    run = 0
    min_run = int(0.002 * sr)
    zero_runs = []
    for i, z in enumerate(zero):
        if z:
            run += 1
        else:
            if run >= min_run:
                zero_runs.append((i - run, run))
            run = 0
    wlen = int(a.win * sr)
    rows = []
    for k in range(0, n, wlen):
        seg = x[k:k + wlen]
        if len(seg) < wlen // 2:
            break
        c = int(clip[k:k + wlen].sum())
        s = step[k:k + wlen - 1]
        sig = s.std() if len(s) else 0.0
        cl = int((s > max(12 * sig, 3000)).any(axis=1).sum()) if len(s) else 0
        rows.append((k / sr, db(np.abs(seg).max() / 32768), db(np.sqrt((seg ** 2).mean()) / 32768), c, cl))
    tot_clip = int(clip.sum())
    print(f"clipped samples: {tot_clip} ({100.0 * tot_clip / clip.size:.3f}%), peak {db(np.abs(x).max() / 32768):.1f} dBFS, "
          f"RMS {db(np.sqrt((x ** 2).mean()) / 32768):.1f} dBFS")
    print(f"silent (all-zero) frames: {100.0 * zero.mean():.1f}%, zero runs >= 2 ms: {len(zero_runs)}"
          + (f", first at {zero_runs[0][0] / sr:.2f} s" if zero_runs else ""))
    print(f"large steps (> half scale between samples): {int(big.sum())}")
    print("worst windows by clicks, then clipping (t, peak dB, rms dB, clipped, clicks):")
    for r in sorted(rows, key=lambda r: (-r[4], -r[3]))[:8]:
        print(f"  {r[0]:7.1f}s  {r[1]:6.1f}  {r[2]:6.1f}  {r[3]:6d}  {r[4]:5d}")


main()
