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

- **Linear render target?** If `PE_COLOR_FORMAT`'s tiling field allows a
  LINEAR RT on this core (etnaviv supports it on some), LTDC could scan the
  RT directly and the per-frame RS resolve (~1.5-2 ms at 720x1280)
  disappears, along with the scanout copy.
- **Resolve async.** Even with tiling, the end-of-frame resolve is
  submit_and_wait'ed; the CPU could start the next frame's sim while the RS
  runs (needs a second RT or careful fencing).

## Features known missing (stubs or absent)

- `arc()` — needed by Pie_Chart, Shape_Primitives; a partial triangle fan,
  small to add.
- `PVector`, `ArrayList` equivalents — most Topics/simulate and vectors
  sketches use them; a small header (`pvector.hh` + using std::vector)
  would unlock a large group.
- Perlin `noise()` — the Basics/math/Noise_* family.
- `second()`/`minute()`/`hour()` — Clock; no RTC, could derive fake
  wall-time from millis().
- Text (`text()`, `textSize()`, fonts) — needs the texture path (gpu/ M3).
- Images (`PImage`, `loadImage`) — texture path + a data source.
- Mouse — `mouseX/mouseY` pinned to center; a real pointer needs an input
  device (USB HID? encoder?). Key events work over the console UART.
