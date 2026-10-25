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
- Flip counts vary run to run (about 10% on the same build); treat a single run's "helpful" as
  provisional.
- Not probed: default-on switches (`RECOMP_PRESENT_VSYNC`, `RECOMP_APU_DSP_ALIVE`) and debug
  switches.
