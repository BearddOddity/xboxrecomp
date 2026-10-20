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
| `RECOMP_VBLANK` | off (on under `RECOMP_PB_WALK=dma`) | raise the GPU's vertical blank at about 60 Hz to the title's ISR | `src/kernel/kernel_bridge.c` |
| `RECOMP_TIMER_HIRES` | off | with `RECOMP_VBLANK`: the same as `RECOMP_VBLANK=paced` (a true 60 Hz vblank, 1 ms timer granularity). Kept as the tcny-port spelling. | `src/kernel/kernel_bridge.c` |
| `RECOMP_VBLANK_LATCH` | off | with `RECOMP_VBLANK`: keep the raised vblank pending until the title acknowledges it. The NV2A ack thread otherwise zeroes it before XDK D3D's deferred routine reads it, and the vblank handler is skipped. | `src/kernel/kernel_bridge.c`, `src/kernel/xbox_memory_layout.c` |
| `RECOMP_PB_WALK` | off (set to `dma` to turn it on) | the DMA-engine walk of the pushbuffer. Only the walk delivers D3D's software-method interrupt (`NO_OPERATION` with a value), which is where XDK D3D queues each swap for the next vblank. Without it no swap completes: the title's vblank callback never sees `D3DVBLANK_SWAPDONE`, `PCRTC_START` is never written and nothing is scanned out. Needs the title's GPU interrupt connected, and a title that does not acknowledge the interrupt can freeze under it. | `src/kernel/nv2a_pb_scan.c`, `src/kernel/nv2a_pb_exec.c` |
| `RECOMP_PB_FLIP_PACE` | off | with `RECOMP_VBLANK`: `FLIP_STALL` waits until the title's vblank handler has put the buffer on screen (PGRAPH_INCREMENT bit 1), up to 100 ms; three misses in a row turn it off with a message. Without it the stall ends at once, the title's frame loop runs as fast as the CPU can submit, and its swap pacing collapses (about 2 game frames a second with 1 s gaps in T()ny). Needs the title's GPU interrupt connected. | `src/kernel/nv2a_pb_exec.c`, `src/kernel/xbox_memory_layout.c` |
| `RECOMP_TEX_MIP_FROM_FORMAT` | off | a swizzled texture takes its mip levels from FORMAT. Without it, a linear pitch left in CONTROL1 by an earlier texture flattens it to level 0, so a title's detailed textures draw at their smallest level. The fork's texture reader used the format's levels. Costs decode time: the back end then uploads and decodes every mip. | `src/kernel/nv2a_pb_exec.c` |
| `RECOMP_STRIP_ALT_WINDING` | off | the pushbuffer executor emits every odd triangle of a triangle strip with its first two vertices swapped, so all of a strip's triangles wind the same way, as the hardware's face test and the fork's executor had them. Without it a back end that culls or lights two-sided from the winding drops or back-lights every second strip triangle (missing faces, odd shapes, a road lit in alternating triangles in T()ny). | `src/kernel/nv2a_pb_exec.c` |
| `RECOMP_FFP_FLIP_NORMALS` | off | fixed-function lighting lights the side of a mesh that faces the viewer, flipping a normal that points away from the eye, even when the title left two-sided lighting off. Not hardware behaviour. Some titles' streets and sidewalks carry normals that face away and light black without it (T()ny's road and walls, X-Men Legends' streets). | `src/kernel/nv2a_pb_exec.c` |
| `RECOMP_VERTEX_CURRENT_ATTRS` | off | the pushbuffer executor: a vertex attribute with no array reads its current value (the last `SET_VERTEX_DATA*` write; 0,0,0,1 at reset) instead of zero, and a stride-0 array is read as a constant stream instead of counted absent. The hardware does this and the fork's executor did. A title that feeds a vertex program its diffuse colour or normal this way draws black without it (T()ny's lit road, cars and walls). | `src/kernel/nv2a_pb_exec.c` |
| `RECOMP_HEAP_RECLAIM` | off | kernel memory: reclaim freed guest heap | `src/kernel/xbox_memory_layout.c` |
| `RECOMP_EXT_VMA` | off | kernel memory: extended virtual-memory areas | `src/kernel/guest_vmem.c`, `src/kernel/xbox_memory_layout.c` |
| `RECOMP_GUEST_LOCK` | off | kernel events: guest-side lock | `src/kernel/kernel_bridge.c` |
| `RECOMP_TITLE_KEVENTS` | off | kernel events: title kernel events | `src/kernel/kernel_bridge.c` |
| `RECOMP_GUI` | off | runtime overlay (F1 menu, performance panel); `RECOMP_GUI_FONT` and `RECOMP_GUI_SCALE` tune it | `src/gui/gui_overlay.c` |

Set a switch to any value to turn it on (`RECOMP_PB_EXEC=1`); `RECOMP_PB_WALK` is a mode and reads
only `dma` (the gate sets it that way). The list above is the one the
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
