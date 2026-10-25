# variant_probe

Runs a built title headless, once with no opt-ins and once per candidate `RECOMP_*` switch, and
classifies each switch as needed, helpful, harmful or no effect from what the runs show. Nothing in
it names a title.

```
py -3 tools/variant_probe/probe.py --exe <title.exe> --out <dir> [--seconds 40]
      [--base RECOMP_AC97_READY=1,...] [--only A,B] [--reuse] [--selftest]
```

Output: `<out>/recomp_probe.json` (per title) and `<out>/runs/<name>/` (stderr, stdout, WAV, frame
dumps, metrics). `opt_ins` in the JSON is what the gate writes into `recomp_variant.json`.

## Reading the result

- **One pass is not enough when the baseline crashes.** Every switch then reads "no effect"
  because the title dies first. Re-run with `--base` set to the switch the first pass found
  (for example `RECOMP_AC97_READY=1`, then add `RECOMP_PB_EXEC=1`).
- **`RECOMP_WATCHDOG_SECS` is a one-shot timer**, used as the run deadline (exit 3 plus a
  `[WATCHDOG]` line means "still running"). Stalls come from the `[GPU]` flip count, which needs
  `RECOMP_PB_EXEC`; without it a run has no frame measurement.
- **`lift_gaps`** in the JSON lists addresses the lift missed (unresolved indirect calls, thread
  start routines). A switch cannot fix those. Feed `runs/baseline/stderr.txt` to
  `tools/seed_from_log` (gate tool `xbox_seed_from_log`), re-lift, rebuild, probe again.
- **Flips mean "alive", not "better".** On 50 Cent the same build alternates between about 2250
  and 3150 flips in 40 s, and a correctly paced run (60 Hz, ~59 fps) flips less than an unpaced
  one (~89 fps). The probe judges liveness (stall, crash) from flips and quality from pacing
  (55-65 fps) and audio (silent frames, clipping).
- **Use `--repeat 3`.** Rounds are interleaved (baseline, each switch, again) so host drift hits
  every config alike; each config keeps a median and min-max range. A single run's numeric
  call is marked provisional.
- **Switches can need each other.** `avoid_on_this_base` lists switches that failed on the base
  used; the gate does not apply it. `RECOMP_TITLE_KEVENTS` stalls at 0 flips alone, and
  works (with 60 Hz pacing) once `RECOMP_VBLANK=paced` is also set. Re-test a failed switch on
  top of the switches that came out needed or neutral before ruling it out.
- Not probed: default-on switches (`RECOMP_PRESENT_VSYNC`, `RECOMP_APU_DSP_ALIVE`) and debug
  switches.
