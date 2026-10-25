#!/usr/bin/env python3
"""Find which RECOMP_* opt-ins a recompiled title needs, helps from, or breaks.

  py -3 tools/variant_probe/probe.py --exe <title.exe> --out <dir> [--seconds 40]
        [--base RECOMP_X=1,RECOMP_Y=2] [--only RECOMP_A,RECOMP_B] [--selftest]

Runs the exe headless once with no opt-ins (baseline, plus whatever --base sets), then once
per candidate switch on top of that base. Every run stops at the deadline: RECOMP_WATCHDOG_SECS
is a one-shot timer, and its [WATCHDOG] line plus exit 3 mean "still running at the deadline".
Evidence per run, read from the run's own output:
  crashed   exit code other than 0 or 3, before the deadline
  ended     exit 0 before the deadline
  stalled   at the deadline, the [GPU] flip count did not grow over the second half of the run
  flips     last "(N flips)" from [GPU] reports (needs RECOMP_PB_EXEC; null otherwise)
  silent    share of silent frames in the APU mixer (RECOMP_APU_MIXSTAT), else from the WAV
  frames    samples the device was given (RECOMP_AUDIO_WAV); 0 is a zero-sample run
  clipped   share of clipped samples in the WAV (tools/audio/wav_analyze.py)
A run fails if it crashed, ended, stalled, or (only when some run in the probe made audio)
delivered no samples. Each switch is classified against the baseline:
  needed    baseline fails, the switch alone passes
  helpful   both pass, and the switch is measurably better (flips +20%, silent -10 points)
  harmful   the switch fails where the baseline passed, or is measurably worse
  no_effect anything else
Nothing here names a title: all of it comes from the exe and its output.
Writes <out>/recomp_probe.json; each run keeps its output in <out>/runs/<name>/.
"""
import argparse, json, os, re, subprocess, sys, time, wave
from pathlib import Path

HERE = Path(__file__).resolve().parent
WAV_ANALYZE = HERE.parent / "audio" / "wav_analyze.py"

# Behavioural opt-ins only (debug switches only print, and default-on switches are not opt-ins).
# Value is what the run sets. Titles often need one of these in combination; --base tests that.
CANDIDATES = [
    ("RECOMP_AC97_READY", "1"),
    ("RECOMP_PB_EXEC", "1"),
    ("RECOMP_USB", "1"),
    ("RECOMP_VBLANK", "paced"),
    ("RECOMP_VBLANK_LATCH", "1"),
    ("RECOMP_TIMER_HIRES", "1"),
    ("RECOMP_FLIP_PACING", "1"),
    ("RECOMP_HONOR_SUSPEND", "1"),
    ("RECOMP_TITLE_KEVENTS", "1"),
    ("RECOMP_GUEST_LOCK", "1"),
    ("RECOMP_HEAP_RECLAIM", "1"),
    ("RECOMP_EXT_VMA", "1"),
    ("RECOMP_DISPATCH_GATE", "1"),
    ("RECOMP_CPUID_NO_SIMD", "1"),
    ("RECOMP_SAGE", "1"),
    ("RECOMP_AUDIO_LIMITER", "0.95"),
    ("RECOMP_AUDIO_AHEAD_PERIODS", "4"),
]
FLIP_RE = re.compile(r"\((\d+) flips\)")
# Lift gaps a run reveals (what tools/seed_from_log reads): no function at a called address, or a
# thread start routine with no dispatch entry. A switch cannot fix these; the lift must be re-seeded.
GAP_RE = re.compile(r"Failed to resolve VA (0x[0-9A-Fa-f]+)|start routine (0x[0-9A-Fa-f]+) not found in dispatch")
MIX_RE = re.compile(r"frames: full (\d+), front end trapped (\d+), silent (\d+)")
SILENT_PCT_RE = re.compile(r"silent \(all-zero\) frames: ([\d.]+)%")
CLIP_PCT_RE = re.compile(r"clipped samples: \d+ \(([\d.]+)%\)")


def stalled(flips: list) -> bool:
    """Flip count not growing over the second half of the reports (~10 s apart). Needs 3+
    reports so the middle one is before the last half; two reports cannot show a stall."""
    return len(flips) >= 3 and flips[-1] == flips[len(flips) // 2]


def read_run(d: Path, rc: int, killed: bool) -> dict:
    err = (d / "stderr.txt").read_text(errors="replace")
    deadline = killed or "[WATCHDOG] no exit" in err
    flips = [int(x) for x in FLIP_RE.findall(err)]
    m = {
        "exit": rc,
        "deadline": deadline,
        "crashed": (not deadline) and rc != 0,
        "ended": (not deadline) and rc == 0,
        "stalled": deadline and stalled(flips),
        "flips": flips[-1] if flips else None,
        "lift_gaps": sorted({a or b for a, b in GAP_RE.findall(err)}),
        "silent": None,
        "clipped": None,
        "frames": 0,
        "watchdog_line": next((ln for ln in err.splitlines() if "[WATCHDOG]" in ln), None),
    }
    mix = MIX_RE.findall(err)
    if mix:
        full, trapped, silent = (int(x) for x in mix[-1])
        if full + trapped + silent:
            m["silent"] = silent / (full + trapped + silent)
    wav = d / "audio.wav"
    if wav.exists() and wav.stat().st_size > 44:
        try:
            with wave.open(str(wav), "rb") as w:
                m["frames"] = w.getnframes()
        except (wave.Error, EOFError):
            m["frames"] = 0
    if m["frames"]:
        out = subprocess.run([sys.executable, str(WAV_ANALYZE), str(wav)], capture_output=True, text=True).stdout
        s = SILENT_PCT_RE.search(out)
        c = CLIP_PCT_RE.search(out)
        if s and m["silent"] is None:
            m["silent"] = float(s.group(1)) / 100
        if c:
            m["clipped"] = float(c.group(1))
    return m


def run_one(exe: Path, out: Path, name: str, env_set: dict, seconds: int, grace: int) -> dict:
    d = out / "runs" / name
    d.mkdir(parents=True, exist_ok=True)
    # Start from a clean RECOMP_* slate so the operator's shell cannot change a run.
    env = {k: v for k, v in os.environ.items() if not k.startswith("RECOMP_")}
    env.update(env_set)
    env.update({
        "RECOMP_HEADLESS": "1",
        "RECOMP_WATCHDOG_SECS": str(seconds),
        "RECOMP_AUDIO_WAV": str(d / "audio.wav"),
        "RECOMP_AUDIO_WAV_SECS": str(seconds),
        "RECOMP_APU_MIXSTAT": "5",
        "RECOMP_FB_DUMP": str(d / "fb"),
    })
    t0 = time.time()
    with open(d / "stdout.txt", "wb") as so, open(d / "stderr.txt", "wb") as se:
        p = subprocess.Popen([str(exe)], cwd=str(exe.parent), env=env, stdout=so, stderr=se)
        try:
            rc, killed = p.wait(timeout=seconds + grace), False
        except subprocess.TimeoutExpired:
            p.kill()
            rc, killed = p.wait(), True
    m = read_run(d, rc, killed)
    m["wall_s"] = round(time.time() - t0, 1)
    (d / "metrics.json").write_text(json.dumps(m, indent=2))
    return m


def fails(m: dict, audio_expected: bool) -> bool:
    return m["crashed"] or m["ended"] or m["stalled"] or (audio_expected and m["frames"] == 0)


def classify(b: dict, s: dict, audio_expected: bool):
    """Returns (classification, reason) for switch run s against baseline run b."""
    bf, sf = fails(b, audio_expected), fails(s, audio_expected)
    if bf:
        if not sf:
            return "needed", f"baseline fails ({fail_word(b)}), alone passes"
        if s["crashed"] and not b["crashed"]:
            return "harmful", "crashes where baseline does not"
        return "no_effect", f"baseline and run both fail ({fail_word(b)})"
    if sf:
        return "harmful", f"run fails ({fail_word(s)}), baseline passes"
    harm, help_ = [], []
    if s["flips"] is not None:
        if b["flips"] is None:
            if s["flips"] > 0:
                help_.append(f"frames appear ({s['flips']} flips)")
        elif b["flips"] > 0:
            rel = (s["flips"] - b["flips"]) / b["flips"]
            if rel >= 0.2:
                help_.append(f"flips {b['flips']} -> {s['flips']}")
            elif rel <= -0.2:
                harm.append(f"flips {b['flips']} -> {s['flips']}")
    if b["silent"] is not None and s["silent"] is not None:
        d = s["silent"] - b["silent"]
        if d <= -0.10:
            help_.append(f"silent {b['silent']:.0%} -> {s['silent']:.0%}")
        elif d >= 0.10:
            harm.append(f"silent {b['silent']:.0%} -> {s['silent']:.0%}")
    if b["clipped"] is not None and s["clipped"] is not None and s["clipped"] - b["clipped"] >= 0.5:
        harm.append(f"clipped {b['clipped']:.2f}% -> {s['clipped']:.2f}%")
    if harm:
        return "harmful", "; ".join(harm)
    if help_:
        return "helpful", "; ".join(help_)
    return "no_effect", "same measurements"


def fail_word(m: dict) -> str:
    if m["crashed"]:
        return f"crashed, exit {m['exit']}"
    if m["ended"]:
        return "exited before deadline"
    if m["stalled"]:
        return f"stalled at {m['flips']} flips"
    return "no audio samples"


def parse_base(text: str) -> dict:
    out = {}
    for item in filter(None, (text or "").split(",")):
        k, _, v = item.partition("=")
        out[k.strip()] = v.strip() or "1"
    return out


def selftest():
    ok = {"crashed": False, "ended": False, "stalled": False, "flips": 60, "silent": 0.5,
          "clipped": 0.0, "frames": 1000, "exit": 3, "deadline": True}
    bad = dict(ok, crashed=True, exit=-1073741819, deadline=False, flips=None, silent=None, frames=0)
    stall = dict(ok, stalled=True, flips=0)
    assert classify(bad, ok, True)[0] == "needed"
    assert classify(ok, bad, True)[0] == "harmful"
    assert classify(stall, ok, True)[0] == "needed"
    assert classify(bad, bad, True)[0] == "no_effect"
    assert classify(ok, dict(ok, flips=80), True)[0] == "helpful"
    assert classify(ok, dict(ok, silent=0.9), True)[0] == "harmful"
    assert classify(ok, dict(ok), True)[0] == "no_effect"
    assert classify(dict(ok, flips=None), dict(ok, flips=12), True)[0] == "helpful"
    assert stalled([1365, 2243]) is False  # two reports: growing, not stalled
    assert stalled([100, 100, 100, 100]) is True
    assert stalled([10, 20, 30, 40]) is False
    print("selftest ok")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe")
    ap.add_argument("--out")
    ap.add_argument("--seconds", type=int, default=40)
    ap.add_argument("--grace", type=int, default=30, help="seconds past the deadline before a kill")
    ap.add_argument("--base", default="", help="opt-ins every run has, e.g. RECOMP_AC97_READY=1")
    ap.add_argument("--only", default="", help="comma list of switches to test (default: all candidates)")
    ap.add_argument("--selftest", action="store_true")
    ap.add_argument("--reuse", action="store_true", help="reclassify runs already in --out (metrics.json), no launch")
    a = ap.parse_args()
    if a.selftest:
        selftest()
        return
    if not a.exe or not a.out:
        ap.error("--exe and --out are required")
    exe, out = Path(a.exe).resolve(), Path(a.out).resolve()
    base = parse_base(a.base)
    only = {s.strip() for s in a.only.split(",") if s.strip()}
    cands = [(k, v) for k, v in CANDIDATES if (not only or k in only) and k not in base]

    if a.reuse:
        # Re-parse each run's output rather than trusting its stored metrics.json.
        runs = {}
        for n in ["baseline"] + [k for k, _ in cands]:
            d = out / "runs" / n
            runs[n] = read_run(d, json.loads((d / "metrics.json").read_text())["exit"], False)
            (d / "metrics.json").write_text(json.dumps(runs[n], indent=2))
    else:
        print(f"baseline + {len(cands)} switches, {a.seconds}s each -> {out}", flush=True)
        runs = {"baseline": run_one(exe, out, "baseline", base, a.seconds, a.grace)}
        print(f"  baseline: {runs['baseline']}", flush=True)
        for k, v in cands:
            runs[k] = run_one(exe, out, k, dict(base, **{k: v}), a.seconds, a.grace)
            print(f"  {k}={v}: exit {runs[k]['exit']} flips {runs[k]['flips']} frames {runs[k]['frames']}", flush=True)

    audio_expected = any(r["frames"] > 0 for r in runs.values())
    b = runs["baseline"]
    switches, opt_ins = {}, {}
    for k, v in cands:
        cls, why = classify(b, runs[k], audio_expected)
        switches[k] = {"value": v, "classification": cls, "reason": why, "metrics": runs[k], "run": f"runs/{k}"}
        if cls in ("needed", "helpful"):
            opt_ins[k] = v
        elif cls == "harmful":
            opt_ins[k] = "off"
    result = {
        "probe_version": 1,
        "exe": str(exe),
        "seconds": a.seconds,
        "base": base,
        "audio_expected": audio_expected,
        "baseline": {"metrics": b, "fails": fails(b, audio_expected), "run": "runs/baseline"},
        "switches": switches,
        "opt_ins": opt_ins,
        # Not switch results: addresses the lift missed. Seed them before trusting any class above.
        "lift_gaps": sorted({g for r in runs.values() for g in r.get("lift_gaps", [])}),
        "lift_gap_log": "runs/baseline/stderr.txt",
    }
    (out / "recomp_probe.json").write_text(json.dumps(result, indent=2))
    if result["lift_gaps"]:
        print(f"\nLIFT GAPS {result['lift_gaps']}: seed them (xbox_seed_from_log on {out / 'runs/baseline/stderr.txt'}), "
              "re-lift, rebuild, probe again. The classes below are provisional until then.")
    print(f"\n{'switch':28} {'class':10} reason")
    for k, s in switches.items():
        print(f"{k:28} {s['classification']:10} {s['reason']}")
    print(f"\nwrote {out / 'recomp_probe.json'}")


if __name__ == "__main__":
    main()
