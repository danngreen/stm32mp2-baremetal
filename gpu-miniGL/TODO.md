# gpu-miniGL TODO

## Performance: the CPU vertex path

After deferred strokes collapsed Game of Life from 73,728 GPU draws to 129,
its frame time only fell 190 ms → 119 ms: the bottleneck is now the CPU-side
per-vertex chain, not submission. GoL pushes ~516k verts/frame; at 119 ms
that is ~230 ns per vertex spread across four copies of every vertex:

  emit_vertex (transform + write to begin buffer)
  -> convert_and_append (copy begin -> batch buffer)
  -> backend draw (memcpy batch -> arena slice + dcache clean)
  -> GPU vertex fetch

Options, roughly in effort order:

- **Check the A35 clock.** Nothing in these projects sets the CPU PLL; if
  TF-A left the A35 at a boot clock instead of 1.5 GHz, everything CPU-bound
  is slower by that ratio. One register read to diagnose; possibly the
  single biggest lever. (Verify against the CA35 PLL/chgclkreq registers.)
- **Immediate-mode fast path for quads/rects.** `rect()` goes through
  glBegin/glVertex/glEnd: 4 emit_vertex calls, a batch-state check per
  block, and a 4->6 vertex conversion. A dedicated path that appends 6
  pre-transformed verts straight into the batch buffer (one mvp multiply of
  2 corners for axis-aligned rects under a scale/translate-only matrix)
  would skip most of the chain for the most common Processing shape.
- **NEON the transform + copy.** emit_vertex's mat4 multiply and the
  28-byte vertex copies are scalar today. The M4 columns and vertex layout
  are NEON-friendly; the compiler is not auto-vectorizing the
  strided-through-struct form.
- **Skip the begin->batch copy.** For the common "block fits, no
  conversion" cases (TRIANGLES, LINES), convert_and_append could write
  directly into the batch buffer at emit_vertex time instead of staging in
  g_begin_buf. Fans/strips/quads still need the staging pass.
- **Arena cache maintenance.** Every batch's arena slice gets a dcache
  clean; small batches pay proportionally more. Cleaning once per
  stream-flush over the arena's dirty span (instead of per-slice) would
  amortize it.

## Performance: GPU side (from the gpu/ handoff, still open)

- **Linear render target: ANSWERED, and it is a trade, not a win.** The PE
  *can* render untiled -- verified on hardware at every stride up to full
  screen and with a depth buffer (`linear_rt_*` tests in gpu/). The layout is
  not in `PE_COLOR_FORMAT` (which only has SUPER_TILED) but in
  `PE_LOGIC_OP.SINGLE_BUFFER`: 2 = tiled, 1 = linear, gated on the LINEAR_PE
  feature (chipMinorFeatures2 bit 4), which this core has.

  It removes the per-frame resolve (~3 ms at 720x1280), but the PE writes an
  untiled target more slowly, because a tile-shaped write scatters across
  rows instead of landing in one burst. Measured end to end:

  | sketch | tiled | direct linear |
  | --- | --- | --- |
  | Flocking (sparse) | 9.7 ms | **6.6 ms** |
  | Brownian (sparse) | 5.6 ms | **3.4 ms** |
  | Rotate_Push_Pop (fill-heavy 3D) | 10.8 ms | 13.4 ms |
  | Radial_Gradient (fill-heavy 2D) | 166 ms | 378 ms |

  So it pays only when a frame's fill is small enough that the resolve
  dominates. It is implemented and off by default:
  `make BOARD=devboard DIRECT_LINEAR=1`. Enabling it per sketch would need a
  fill estimate the backend does not have; an adaptive version could time
  both modes and keep the faster, which is the obvious next step if this
  matters.

  Related: a linear->linear RS blit measured **2.9x slower** than the
  tiled->linear resolve (a copy-only frame: 8.9 ms vs 3.1 ms), so the RS
  clearly prefers a tiled source. That is why the accumulate path still
  renders tiled and resolves.
- **Resolve async.** Even with tiling, the end-of-frame resolve is
  submit_and_wait'ed; the CPU could start the next frame's sim while the RS
  runs (needs a second RT or careful fencing).

## Per-pixel sketches are CPU-bound

`loadPixels()`/`pixels[]`/`updatePixels()` work, but the panel is 921,600
pixels — 4x the 640x360 these sketches were written for. Noise_2D costs
1.76 s/frame and Noise_3D ~1.0 s/frame, essentially all of it CPU Perlin
noise plus the full-frame copy into the scanout buffer. Options: run the
noise on the PPU (it is exactly the kind of per-pixel kernel the compute path
already does — see gpu/'s PPU tests), or let a sketch render at its native
size into a smaller buffer and upscale.

## Features known missing (stubs or absent)

- Text (`text()`, `textSize()`, fonts) — accepted and ignored today, so
  sketches run without their labels. Needs the texture path (gpu/ M3).
- Images (`PImage`, `loadImage`) — texture path plus a data source.
- Mouse — `mouseX`/`mouseY` random-walk across the screen (no pointer
  hardware) and Enter on the console synthesises a click at the cursor. A
  real pointer still needs an input device (USB HID? encoder?). Key events
  already work over the console UART.
- `PShape` / `loadShape`, `createGraphics` (FBOs, cheap — see gpu/ M4),
  `PImage`-based `filter()`. `bezier()`/`bezierVertex()` are done;
  `curve()`/`curveVertex()` (Catmull-Rom) are not.
- 3D is implemented (P3D, depth, perspective camera, lights, box/sphere), but
  with gaps: `spotLight()` degrades to a point light (mini-GL's lighting has
  no cone term), `emissive()` is ignored (no emission term in the material
  model), and `vertex(x, y, z)` inside `beginShape()` drops z -- the shape
  buffer is 2D. `PShape`/OBJ loading needs file I/O as well.
- Carrying clip-space `w` made the vertex 8 floats instead of 7, which costs
  the vertex-bound sketches about 20% (Game_Of_Life 119 -> 143 ms). Sketches
  that are not vertex-bound are unaffected (still 58 fps). Recovering it
  would mean two vertex layouts selected by whether the projection is
  affine -- only worth it if the CPU vertex path above gets optimised first.
