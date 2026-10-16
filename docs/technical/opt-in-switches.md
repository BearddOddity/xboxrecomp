# Opt-in runtime switches

Every runtime feature that changes what a title does ships **off**. An unset variable keeps the
default path byte-identical to a build without the feature. A feature is turned on per title,
after that title has been run with it.

Two rules decide what may live in shared code:

1. **Title-agnostic.** Behaviour is derived from the XBE and the hardware, never from which game is
   running. Anything that names one title's addresses or data stays in that title's project.
2. **No regressions.** A change that alters the default path needs a switch (this table) or
   validation on every variant we can run.

## Switches

| Switch | Default | Enables | Read in |
|---|---|---|---|
| `RECOMP_PB_EXEC` | off | NV2A pushbuffer executor and render back-end interface | `src/kernel/nv2a_pb_scan.c`, `src/kernel/xbox_memory_layout.c` |
| `RECOMP_USB` | off | USB (OHCI) gamepad model | `src/usb/ohci.c` |
| `RECOMP_HEAP_RECLAIM` | off | kernel memory: reclaim freed guest heap | `src/kernel/xbox_memory_layout.c` |
| `RECOMP_EXT_VMA` | off | kernel memory: extended virtual-memory areas | `src/kernel/guest_vmem.c`, `src/kernel/xbox_memory_layout.c` |
| `RECOMP_GUEST_LOCK` | off | kernel events: guest-side lock | `src/kernel/kernel_bridge.c` |
| `RECOMP_TITLE_KEVENTS` | off | kernel events: title kernel events | `src/kernel/kernel_bridge.c` |
| `RECOMP_GUI` | off | runtime overlay (F1 menu, performance panel); `RECOMP_GUI_FONT` and `RECOMP_GUI_SCALE` tune it | `src/gui/gui_overlay.c` |

Set a switch to any value to turn it on (`RECOMP_PB_EXEC=1`). The list above is the one the
pipeline's variant gate knows (`OPT_INS` in the toolkit's `recomp_mcp.py`); keep both in step.

## How the variant gate uses this table

The variant gate reads one XBE and picks pipeline options (`--extra-sections`, seeds, data
ranges). It also reports the state of each switch above for that XBE:

- `off`: no marker in the XBE. This is the default.
- `suggest`: the XBE links the library the switch serves (a `D3D` section for `RECOMP_PB_EXEC`, an
  `XPP` section for `RECOMP_USB`). A suggestion is a hint to try the switch, never a setting.
- `on` / `off` set by hand: the title's own `recomp_variant.json` carries
  `{"opt_ins": {"RECOMP_PB_EXEC": "on"}}` once the title has been measured with the switch.

The environment for a run is only the switches set `on`. A title with no `recomp_variant.json`
runs with an empty environment, so the default path is untouched.

## Adding a switch

1. Read it with `getenv` at one place, default off.
2. Add a row above and an `OPT_INS` entry in the toolkit (marker `None` unless the XBE shows a
   reason to try it).
3. Show the default path is unchanged with the switch unset.
