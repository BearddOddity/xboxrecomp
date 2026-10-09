# Pushbuffer Executor

A title that statically links the XDK's Direct3D never calls the toolkit's
D3D8 layer. It writes NV2A methods into a pushbuffer and kicks the GPU, and the
only way to see its frames is to execute that pushbuffer. `RECOMP_PB_SCAN`
surveys it; `RECOMP_PB_EXEC` executes it, on the NV2A poll thread, into guest
memory with a CPU rasteriser or into a render back end the game project
registers (`nv2a_backend.h`).

The rasteriser itself — vertex programs (`nv2a_vsh_interp.c`), register
combiners (`nv2a_combiner.c`), four texture stages, near-plane clipping and a
threaded fill (`nv2a_pb_exec.c`) — arrived in v0.13.0. This page covers what
sits around it, in the order the problems appeared while bringing up
*X-Men Legends*: reading the ring, answering the title while it runs,
fixed-function transform and lighting, anti-aliased surfaces, and the back-end
interface.

## Reading the Pushbuffer Like the DMA Engine

The executor used to walk the bytes between the previous and the current
`DMA_PUT` in a straight line, following `CALL`s since v0.13.0 but still treating
each PUT-to-PUT span on its own:

| Case | Straight-line behaviour |
|---|---|
| `JUMP` | ended the span; the target was read only if it was a `CALL`'s |
| PUT below the last PUT (the ring wrapped) | the ring's bounds were learned from the PUTs seen, so the first lap lost a segment and every later wrap scanned "a little short of the end" |
| the title writes `DMA_GET` | not noticed |

XDK D3D sends state blocks as CALLs into pre-built pushbuffers and the ring
wraps many times a second, so every lost segment is state the executor never
saw: wrong textures, world-space geometry pushed through D3D's
two-instruction pre-transformed program (`mov oPos, v0; mov oT0, v9`).

`nv2a_pb_scan.c` now keeps its own GET — a physical address, like the register
— and walks the way the DMA engine does: it follows `JUMP` (old and new style),
`CALL` and `RETURN` (one level of subroutine, as on NV2A), and stops when GET
reaches PUT. The PUT it stops at is re-read from the register every command,
and only ever moved back to the live writer: a level load moves PUT while a
walk is running (the title wraps the ring, or `CDevice::KickOff`'s restart
writes a fresh GET/PUT pair), and walking on to the PUT read before the walk
runs past the live writer into words the title is writing for the next lap —
a pair of 16-bit vertex indices whose low bits are 1 or 2 decodes as a JUMP or
CALL. That was the garage desync: `[PB] bad target`, and the desync's dump
shows the ring words the walk had read rewritten underneath it. A PUT that
moved *forward* is not chased: the poll thread reads PUT every pass, and the
next pass continues from where this one stopped. The ring's wrap is itself a
JUMP back to the start, so following it replaces learning the ring's bounds.
A jump or call outside RAM means the walk is out of step; it snaps GET to PUT
and logs `[PB] bad target ...` with the last commands it decoded, the live
register values, and any ring word it had read that has since changed. If the
title moves GET itself (XDK D3D resets the ring by writing `DMA_PUT` and
`DMA_GET` together) the walker resyncs, and it starts at the title's first
GET, so device state sent before the first kickoff — the depth function, for
one — is not lost.

`tests/nv2a_pb_walk` checks the walk with a stubbed executor: `CALL` and
`RETURN`, `JUMP`, non-increasing methods, a resync, a bad target, and a PUT
that moves under the walk — forward, where the walk stops at what it was
given and the next pass reads on, and backwards, where the walk follows the
ring's JUMP to the new lap and stops at the live PUT without reading what
lies past it.

## Answering the Title While It Runs

XDK D3D asks the GPU questions while it submits, and the executor has to
answer them from inside the walk, because the thread that executes the ring is
the thread that would otherwise answer.

- **Kickoff.** Every kickoff sets `0x100410` bit 16 and spins until it clears.
  With the bits cleared only between passes of the poll loop, one pass can be a
  whole rendered frame, so a level load — thousands of kickoffs — looked
  frozen. The walker now clears them every 256 words
  (`xbox_Nv2aAckBusyBits`).
- **Fence mirrors.** A project that registers `xbox_Nv2aMirrorFence` has the
  poll thread copy the title's "submitted" fence into its "consumed" one. The
  walk now follows every CALL and JUMP, so a pass can be long, and a mirror
  ticked only between passes lags the title by a whole walk. D3D then takes its
  fence wait, which a mirrored fence cannot satisfy (Burnout 3 hung there at its
  first menu). `xbox_Nv2aAckBusyBits` ticks the mirrors as well, so they keep
  pace from inside the walk. Under the DMA walk a mirror is only raised when
  the last walk *finished its segment*: hardware raises the mirror as the
  engine completes the fence, and the title takes the mirror as permission to
  reuse a pushbuffer — a RenderWare geometry buffer behind a `CALL`, the case
  that desynced the garage — while the walk is still reading it. A pass that
  snapped to PUT (lost sync, bad target, the word-budget guard) has skipped
  unread commands and says so with `nv2a_pb_caught_up()`; the segment scan
  still mirrors every pass, as before.
- **Semaphore.** `BACK_END_WRITE_SEMAPHORE_RELEASE` (`0x1D70`) writes its value
  where `nv2a_pb_set_semaphore_target()` was told, once everything before it
  has run. `SET_SEMAPHORE_OFFSET` is not added: in *X-Men Legends* it held
  `0xFF000000` at times. How a title's fence wait uses it is in
  [D3D8LTCG Device Context](d3d8ltcg-device-context.md#when-something-does-read-the-pushbuffer).
- **Method `0x0310` on subchannel 5.** D3D's `KickOff` ends every kick with it,
  carrying `((pushbuffer address * 8 | fence & 0x1F) << 2) | wrap & 3`. The
  GPU latches it in PGRAPH `0x400B10`, and D3D waits for the fence bits there.
  The executor latches the value. Without it `0x400B10` read 0 and the wait
  ended only when the semaphore was a multiple of 32.
- **`DMA_GET`.** It is the GPU's fetch pointer — nothing at or past it has been
  read — and D3D's fence wait relies on that: when its fence is far enough
  ahead of GET it patches a "wake me" software method into the pushbuffer at
  the fence and sleeps on an event. GET used to move only when a walk reached
  PUT, so it lagged the executor by up to a whole kick; D3D then patched
  commands the executor had already passed and slept for good. The walker now
  publishes its position before every command, and finishes on PUT. A GET that
  is not the value it last wrote means the title moved it (the ring reset), and
  the walk follows it rather than overwriting it.

## Switches Read Once

The NV2A poll thread paid for `getenv()` on every method — it scans the whole
environment, and a 3D frame is millions of methods. Debug switches are read
once, and the answer "not set" is cached as well as "set": a cache that only
remembered "set" ran `getenv` on every pass of the default path, where neither
`RECOMP_PB_SCAN` nor `RECOMP_PB_EXEC` is set. The method inventories (the
survey's and the unhandled-method ranking) are indexed by method instead of
searched: the survey's search was about 40% of the thread in a *X-Men Legends*
level.

## Fixed-Function Transform

Batches drawn with `SET_TRANSFORM_EXECUTION_MODE` (`0x1E94`) on "fixed" used to
count as "not screen-space" and be skipped unless their positions already
happened to be pixels:

```
rasterised 0 triangles; 8073 batches skipped as not screen-space
```

D3D uploads a composite matrix — world × view × projection × viewport — with
methods `0x0680`–`0x06BC`, and a viewport offset with `0x0A20`–`0x0A2C`, which
the executor keeps in the constant file at `c[59]`. With a composite matrix
uploaded and a fixed-function mode, `ffp_vertex()` transforms attribute 0,
divides by w and adds the offset, and hands the result on in the shape a vertex
program produces. So a fixed-function batch shares everything after the
transform with a program batch: near-plane clipping, the four texture stages,
the combiners and the depth buffer. The 16 floats apply as
`clip[i] = Σ M[4i+j] · pos[j]` (translation in `M[3]`, `M[7]`, `M[11]`),
**not** as a row vector times the matrix; the other order puts every vertex
near `(0, 0)`. `RECOMP_FFP_TRACE` prints the matrix and a few raw and
transformed vertices, which is how the order was found.

Texture coordinates are taken from the attribute streams as they are, with no
texture-coordinate generation or texture matrix; there is no fog coordinate,
specular, or point sprite.

### Lighting

With `SET_LIGHTING_ENABLE` on, the diffuse colour is computed per vertex from
the normal and up to eight lights, and the vertex's own colour stream is
ignored unless `SET_COLOR_MATERIAL` routes it in. Using the colour stream
anyway is what drew every lit mesh — characters, streets — black. XDK D3D
pre-multiplies the material into the light colours and folds emission and
ambient into `SCENE_AMBIENT_COLOR`, so the hardware sum is

```
colour = scene_ambient + Σ atten_i · (ambient_i + diffuse_i · max(0, N·L_i))
```

in eye space, alpha from `SET_MATERIAL_ALPHA`. The registers are read from a
mirror of the last value written to every 3D-class method (`s_reg`), because
lights and material colours are the kind of state the executor does not model
one field at a time. Normals go through the model-view matrix itself rather than
the inverse transpose (exact for rotations and uniform scale); there is no
specular, no spot cone, no back-face colour and no skinning.

### Normals facing away

Some meshes store normals that face away from the camera, and that is
authentic: a write watch traced the road normal `(0, -1, 0.03)` to the title's
file reader (*X-Men Legends*), and it equals `(v1-v0)×(v2-v0)` of the first
triangle. The model-view matrix mirrors (determinant −1) and culling is off, so
with `max(0, N·L)` those surfaces got only the ambient term.

What makes such a mesh look right on hardware is not established:
*X-Men Legends* writes `0x17C4` (two-sided lighting) as 0, and xemu uses front
colours only. So by default `lit_color` lights the normal as stored.
`RECOMP_FFP_FLIP_NORMALS` flips an eye-space normal that points away from the
eye instead — two-sided lighting with the back material equal to the front, per
vertex and not per face. It is a compatibility option, not hardware behaviour:
a game project sets it for a mesh it has seen need it, so it cannot change how
another title is lit. Check against xemu or hardware if a surface ever looks
lit from the wrong side.

## Anti-Aliased Surfaces

With multisampling on, the surface is 2× (or 2×2) the logical size, and
`SET_SURFACE_FORMAT` bits 12–15 say which. The executor used the logical
640-pixel width with the real 5120-byte pitch, got 8 bytes per pixel, and wrote
`0 pixels`. The clip rectangle is now scaled by that factor, and so are the
positions that come out of a vertex program or the composite matrix (the
viewport the title programs is in logical pixels). The near-plane clipper takes
its viewport scale and offset from the same place, so clipped triangles land
where unclipped ones do. The framebuffer window takes every other pixel of a
2×-wide surface instead of showing it black, and the host-side depth buffer is
wide enough for 1280×960.

## Index Width

The index list was `uint16_t` and held 4096 entries, but `DRAW_ARRAYS` starts
are 24-bit, so vertices past 65535 wrapped to the start of the buffer and big
batches were cut short. Indices are 32-bit now, batches may hold 65536, and
`ARRAY_ELEMENT32` (`0x1808`) is handled. An index comes from the pushbuffer, so
`fetch_attr` checks every vertex read against guest memory (ordinary RAM and the
contiguous window); a read outside it returns no vertex and the batch is dropped.
`SET_VERTEX_DATA_ARRAY_FORMAT` size 7
(`3W`) reads as three components.

## Render Back Ends

`nv2a_backend.h` is the interface. A game project registers `clear`, `draw` and
`flip`, and receives:

- the colour surface and its anti-aliasing factor;
- triangle lists in surface pixels, **already clipped at the near plane** by the
  executor, with per-vertex colour, `1/w`, and texel-space UVs — whichever path
  produced them (program, fixed-function or pre-transformed);
- the texture the batch samples (`nv2a_backend_decode_texture` decodes every
  format the executor can sample), with its mip level count and filter word;
- blend, alpha, depth, colour-mask, cull and stencil state in the NV2A's own
  enum values.

All calls come from the NV2A poll thread, one at a time, so a back end can
create its window and device lazily and pump messages from `flip`.

Things a back end should get right. These come from a Direct3D 11 back end
written against this interface for one title; that back end is not in this
tree, so treat them as advice from one implementation, not as tested behaviour
of the code here:

- **Window z is compared as it is.** `SET_CLIP_MIN`/`MAX` only clip. A title
  narrows the range for a low-detail layer so it is cut off near the eye; a back
  end that rescales each draw's z over its own range makes that layer about 237
  times deeper (the figure from that back end's title) and puts it in front of
  everything near.
- **Stencil.** A title can mark pixels with a stencil-writing mesh and then draw
  a full-screen quad that only the marked pixels pass. A back end that ignores
  `stencil_*` draws that quad over the whole surface.
- **Cull from projected positions.** D3D11 decides facing after clipping, so a
  triangle that reaches behind the eye keeps its true winding; the NV2A decides
  from the projected, mirrored one. Decide culling per triangle from the
  projected screen positions and draw with the rasteriser's culling off.

Not passed on yet: the register-combiner state, and texture stages 1–3 (a back
end sees stage 0 and the diffuse colour), so effects that need a second stage
or a combiner program only appear on the CPU rasteriser. Extend `Nv2aBatch`
when it matters rather than adding callbacks.

## Switches

| Variable | Does |
|---|---|
| `RECOMP_PB_SCAN` | read-only survey of the pushbuffer; ranks what is not implemented |
| `RECOMP_PB_EXEC` | execute it |
| `RECOMP_PB_WALK=dma` | walk the pushbuffer like the DMA engine (JUMPs, wraps, GET per command, the `0x0310` latch) instead of scanning each submitted segment; see [The DMA Walk Is Opt-In](#the-dma-walk-is-opt-in) |
| `RECOMP_PB_NOP_TRAP=0` | with the DMA walk on, stop trapping the software method (`NO_OPERATION` with a value); the default on the walk is to trap and deliver the GPU's interrupt; see [Software Methods](#software-methods) |
| `RECOMP_TITLE_KEVENTS`, `RECOMP_VBLANK` | kernel switches the walk depends on (events a title builds itself; the GPU's vertical blank). Off by default, **on by default under `RECOMP_PB_WALK=dma`**; `=0` turns either off |
| `RECOMP_PB_EXEC_VERBOSE` | per-batch trace |
| `RECOMP_FFP_TRACE` | composite matrix and sample vertices of the first fixed-function batches |
| `RECOMP_FFP_FLIP_NORMALS` | light the side of a fixed-function mesh that faces the viewer, even when the title left two-sided lighting off. Not hardware behaviour: a compatibility option for a game project to turn on, off by default |

## The DMA Walk Is Opt-In

`RECOMP_PB_WALK=dma` turns on the DMA-engine walk; without it the executor
scans each segment between the previous `DMA_PUT` and the new one, as it did
before the walk existed.

The walk is the more faithful of the two, and that is the reason it is opt-in.
Told exactly how far the GPU has got (`DMA_GET` per command and the `0x0310`
latch), XDK D3D's fence code takes its "GPU far behind" path: it patches a
software method (`NO_OPERATION` with a value) into the pushbuffer and sleeps on
an event that the GPU's interrupt for that method sets. Until the runtime
delivers that interrupt, a title that runs under the segment scan can freeze
under the walk on the first screen whose pushbuffer makes the walk long
enough. Burnout 3 did, at its Crash Nav menu, with the main thread in
`KeWaitForSingleObject` under D3D's fence wait.

The runtime now delivers that interrupt when the walk is on -- see [Software
Methods](#software-methods) below. Two kernel switches go with it and default on
under the walk: `RECOMP_TITLE_KEVENTS`, because the event D3D sleeps on is one
it builds itself rather than through `KeInitializeEvent`, and without a host
event behind it the interrupt's `KeSetEvent` is lost and the fence wait spins
for ever; and `RECOMP_VBLANK`, because once those events are real D3D's
vertical-blank event is too, and nothing else sets it. Either can still be
turned off with `=0`.

With all three, Burnout 3 runs its Crash Nav menu and loads the garage car under
the walk, as it does under the scan. The walk stays opt-in all the same: the
segment scan does not trap the software method, so its behaviour is unchanged,
and the walk has had less mileage. Turn it on to bring up a title the segment
scan draws wrongly.

## Software Methods

`NO_OPERATION` (`0x0100`) with a non-zero parameter is not an operation the GPU
performs. XDK D3D writes one into the pushbuffer when it wants the graphics
interrupt -- a "signal me when you get here" marker. On hardware PGRAPH stops at
the method, latches the method and its data in `PGRAPH_TRAPPED_ADDR`
(`0x400704`, `0x100 | subchannel << 16`) and `PGRAPH_TRAPPED_DATA_LOW`
(`0x400708`), sets the `NSOURCE` notification bit, raises the `ERROR` bit in
`PGRAPH_INTR` (`0x400100` bit 20) and the `PGRAPH` bit in `PMC_INTR`
(`0x000100` bit 12), and holds there until the driver has acknowledged the
`ERROR` bit and re-enabled FIFO access (`PGRAPH_FIFO`, `0x400720` bit 0).

XDK D3D's handler is the usual NVIDIA split. The ISR masks `PMC_INTR_EN`
(`0x000140`) and queues a DPC. The DPC loops over the units with a bit in
`PMC_INTR`; for PGRAPH it turns FIFO access off, reads `PGRAPH_INTR`, writes the
value it read back to acknowledge it, hands the trapped data to its
software-method switch -- which is where a swap is queued for the next vertical
blank -- turns FIFO access back on, and returns `PGRAPH_INTR` read again. The
DPC goes round again while that is non-zero, then restores `PMC_INTR_EN`. A
title that waits on the swap for a fence, in `KeWaitForSingleObject` under D3D's
fence code, sleeps forever if the interrupt never comes.

The executor traps the method only when `nv2a_pb_dma_walk()` is on
(`RECOMP_PB_WALK=dma`): `nv2a_pb_exec_method` (`src/kernel/nv2a_pb_exec.c`)
hands `0x0100` with a value to `nv2a_pgraph_trap_nop`
(`src/kernel/nv2a_pgraph_trap.c`). The segment scan never routes a kick's
trailing software method there, so the default path is unchanged; and if
`RECOMP_PB_NOP_TRAP=0`, no handler is connected on vector 3, or the title has not
enabled the `ERROR` interrupt in `PGRAPH_INTR_EN` (`0x400140`), the method is
left to the unhandled count exactly as before.

The trap sets the registers above and raises vector 3 on a level line
(`xbox_set_irq_line`, `src/kernel/kernel_bridge.c`), which the timer thread
services by calling the connected ISR; the DPC it queues runs on the same
thread straight after. The walk waits, as PGRAPH does, until the acknowledgement
-- so a second trap can never overwrite `TRAPPED_DATA_LOW` before the handler has
read the first. The wait is bounded at one second; a trap nobody answers is
logged and dropped.

The acknowledgement is the part plain memory cannot do. Against plain memory,
writing back the value read changes nothing, the DPC reads `PGRAPH_INTR` still
set, and it runs the same software method again, for ever, at `DISPATCH_LEVEL`.
That queues the swap over and over, and because the guest never drops below
`DISPATCH_LEVEL`, every USB interrupt is held off and the pad goes dead. So for
the length of a trap the 4 KB PGRAPH register page is made read-only. Reads run
at full speed against plain memory; a write faults, is single-stepped, and then
has the register's semantics applied -- write-1-to-clear for `PGRAPH_INTR`, at
whatever width the store was -- with `PMC_INTR`'s PGRAPH bit and the interrupt
line following `PGRAPH_INTR & PGRAPH_INTR_EN`. It is the same trap the AC'97
bus-master reset bits use in `src/kernel/xbox_memory_layout.c`. Nothing about it
is title-specific: it is how the register behaves, whoever writes it. Outside a
trap the page is plain memory, as it always was.

While the trap is outstanding the busy-bit pass (`xbox_Nv2aAckBusyBits`), which
otherwise holds `PMC_INTR` and `PGRAPH_INTR` at zero, leaves `PGRAPH_INTR` and
`PMC_INTR`'s PGRAPH bit alone (`g_nv2a_intr_hold`, under `g_nv2a_intr_lock`) --
and the waiting walk keeps running that pass. The walk's thread is the one that
normally runs it, and D3D's deferred routine acknowledges a vertical blank and
then spins until `PMC_INTR` bit 24 drops; with the walk blocked and the pass
stopped, that routine never reached the trapped method and every trap ran out
its one-second bound.

The trap is Windows-only, like the AC'97 trap it follows; elsewhere it is off
and the method is unhandled. `tests/nv2a_pgraph_trap` drives it with real page
faults and a second thread playing the XDK handler.
