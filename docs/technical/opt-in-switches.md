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

## Back-end flags (a game project's renderer asks, not the environment)

A render back end (`nv2a_backend.h`) opts in to extra work by setting bits in `Nv2aBackend.flags`.
Unset bits cost nothing: the executor builds the extra data only for a back end that asks.

| Flag | Value | What the back end receives |
|---|---|---|
| `NV2A_BACKEND_CLIPS_HOMOGENEOUS` | `0x1` | Triangles that reach behind the eye whole, with a signed `1/w` in `Nv2aVertex.rhw`, instead of the executor's near-plane clip. |
| `NV2A_BACKEND_GPU_VERTEX_PROGRAMS` | `0x2` | Vertex-program batches untransformed through `draw_vp` (`Nv2aVpBatch`: microcode, constants, per-vertex program inputs). `RECOMP_GPU_VP=0` turns it off at run time. |
| `NV2A_BACKEND_WANT_PIPELINE` | `0x4` | `Nv2aBatch.ex` (per-vertex colours, fog, four stages' texture coordinates), `stages[4]` and `comb` (the combiner registers). |

Texture palettes (`Nv2aTexture.palette`, `palette_len`) are always filled: they are plain data a back
end may ignore.

## Adding a switch

1. Read it with `getenv` at one place, default off.
2. Add a row above and an `OPT_INS` entry in the toolkit (marker `None` unless the XBE shows a
   reason to try it).
3. Show the default path is unchanged with the switch unset.
