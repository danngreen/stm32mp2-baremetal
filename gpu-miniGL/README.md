# gpu-miniGL — Processing sketches on the panel

Real Processing examples running on the STM32MP257's GPU, baremetal, at the
panel's refresh rate. This is the end-to-end proof of the project goal:
`.pde` sketch → mini-GL (`gpu/gl/`) → the etna 3D pipe → LTDC scanout.

Sketches live under `sketches/` as `.pde` files, compiled as C++ untouched.
Pick one with the `DEMO` make variable:

```bash
make BOARD=devboard DEMO=sketches/Basics/color/Radial_Gradient.pde
make flash-stlink
```

The default `DEMO` is **Bouncy Bubbles** (Topics/Motion, based on code by
Keith Peters): twelve semi-transparent bubbles bouncing and colliding under
gravity.

```
mini-GL -> LTDC: a Processing sketch on the panel
Display up: 720x1280, sketch running
58 fps, worst render 4986 us, 1 draw(s), 246 dwords
```

That last line is the design working as intended: 12 balls × (a triangle-fan
ellipse each) collapse into **one** GPU draw of 246 dwords per frame, because
every shape shares the same state (white fill, alpha blend on) and mini-GL
batches same-state geometry. The frame rate is vsync-locked to the DSI panel.

The other current sketches exercise more of the surface:

- `Basics/color/Radial_Gradient.pde` — 180 concentric ellipses per frame
  through `colorMode(HSB)`, `ellipseMode(RADIUS)` and `frameRate(1)`. At ~21k
  vertices a frame it overflows both the 4096-vertex batch and the vertex
  arena, so one frame becomes 19 draws — the splitting paths doing their job
  (98 ms a frame, which `frameRate(1)` doesn't notice).
- `Basics/forms/Triangle_Strip/Triangle_Strip.pde` — a ring via
  `beginShape(TRIANGLE_STRIP)`/`vertex`/`endShape`, sized by `mouseX`
  (pinned to the screen center until there is an input device). Two draws:
  the white fill batch and the black triangle-edge stroke batch.
- `Topics/cellular_automata/Game_Of_Life/Game_Of_Life.pde` — 144×256 cells,
  each a stroked rect. Naively, fill/stroke alternate primitive classes every
  cell: 73,728 draws and 1.58M command dwords a frame (~5 fps) — the workload
  that exposed (and now regression-tests) the ring-wrap-over-tail-WAIT bug in
  `Gpu::submit()`; see the gpu/ README. With deferred strokes (below) the
  same frame is **129 draws and 1,812 dwords**, ~8 fps, now bounded by the
  CPU-side vertex path (516k verts/frame), not by submission. Uses
  `millis()`, `color()`, the packed-color `fill(int)`/`stroke(int)`
  overloads, and `keyPressed()`: space pauses, `r` reseeds, `c` clears.

## Deferred strokes

Stroke lines are not drawn where they are issued: psketch accumulates
segments and emits them in bulk when something forces it — a stroke
color/weight change, any matrix change (vertices transform at emit time), a
clear, or frame end. Fills stay immediate, so a run of stroked shapes becomes
one long triangle batch plus one line batch instead of splitting the batch at
every shape. This is Processing's own P2D "optimized stroke" behavior,
including its known quirk: within a flush window, strokes render on top of
later fills. Identical for shapes that don't overlap.

Two hardware-found sizing rules live in `psketch.cc`: the pending buffer is
capped (and pre-reserved) at 1 MB — an unbounded vector's doubling realloc
blew the 8 MB heap on Game of Life — and flushes emit in 4096-vertex
`glBegin` chunks to fit mini-GL's begin buffer. mini-GL also caches the
combined projection×modelview matrix (rebuilt on any matrix change), so the
unlit vertex path is one matrix multiply, not two.

## Keyboard input

Characters typed into the board's serial console (a minicom session on the
UART) become Processing key events: each received byte sets `key`, fires the
sketch's `keyPressed()`, and is echoed to the console (`key: 'r'`). The
receiver is polled from the frame loop (`uart_getchar()` in
`shared/print/uart_print.c`), so no interrupt plumbing was needed. There are
no key-up events over a serial line, so `_keyPressed` is only true during the
frame a byte arrived in — and the booleans are spelled `_keyPressed` /
`_mousePressed` because C++ cannot give a variable and an event function the
same name the way Processing's Java does.

Also verified on hardware (first sweep, all vsync-locked unless the sketch
sets its own frameRate): Star, Regular_Polygon, Sine_Cosine, Bounce, Linear,
Recursion, Rotate, Tree, Wolfram, Brownian. Brownian's 2000 per-segment
stroke colors still batch into **one** draw — deferred strokes carry color
per segment, since color is a vertex attribute, not pipeline state.

## How a .pde compiles

Java ignores definition order; C++ does not. `tools/pde_prototypes.py`
generates forward declarations for the sketch's top-level functions (the same
job Processing's own preprocessor does), and `sketch_pde.cc` includes those,
then the `.pde` itself, and maps `setup()`/`draw()` onto the harness. That is
the whole pipeline — the `.pde` files themselves stay untouched, provided
they are C++-compatible Processing code (the examples largely are; `fmod`
instead of `%` on floats is the usual edit).

## Dynamic memory

This project links the real newlib/libstdc++ (no `-nostdlib`, no
`-ffreestanding`), so sketches can `new`, and psketch itself uses
`std::vector` for `beginShape` geometry. The costs, all in this directory:

- `newlib_syscalls.cc` — `_sbrk` over the linker script's 8 MB `.heap`
  region (after `.bss`, before the stacks, so exhaustion is ENOMEM rather
  than a corrupted stack), plus the stdio syscalls libstdc++'s error paths
  want; `_write` goes to the UART so abort messages are visible.
- `shared/newlib/libcpp_stub.cc` is deliberately **not** linked — its
  trap-loop `operator delete` would override the real malloc-backed one.
- `-nostartfiles` stays, so `startup.s` still owns boot (it already ran
  `__libc_init_array`, so global constructors work as before).

## The pieces

| file | what |
| --- | --- |
| `sketches/**/*.pde` | the Processing sketches, compiled as C++ |
| `sketch_pde.cc` | includes generated prototypes + the selected `.pde` |
| `tools/pde_prototypes.py` | forward-declaration generator |
| `psketch.hh/.cc` | the Processing API surface (`background`, `fill`, `ellipse`, `beginShape`, `colorMode`, `pushMatrix`, `random`, …) implemented as mini-GL calls |
| `newlib_syscalls.cc` | `_sbrk` + stdio hooks for the hosted build |
| `main.cc` | the harness: GPU + display bring-up, double-buffered vblank-flipped frame loop (modeled on `gpu-ltdc-demo`), `frameRate()` throttling |

`psketch` mirrors what processing.cpp's fixed-function renderer does: shapes
tessellate on the CPU (`ellipse` → triangle fan, `rect` → quad, strokes → line
loops, `beginShape` kinds → the matching GL primitive), transforms go to the
GL matrix stack, and `fill`/`stroke` are two persistent colors applied around
each shape. Colors go through `colorMode` (default RGB 0–255, HSB supported),
origin is top-left with +y down, and alpha blending is on by default — all
Processing's defaults, so `fill(255, 204)` just works. `mouseX`/`mouseY`
exist but sit at the screen center until there is an input device. `size()`,
`smooth()`/`noSmooth()` are accepted and ignored (the panel decides the size).

When processing.cpp's own drawing functions get lifted onto mini-GL (the real
plan), `psketch` is what they replace; the `.pde` files would then compile
against those instead.

## What the first sketch exercised for the first time

- **`GpuBackend::set_scanout()`** — the backend resolves each frame directly
  into the LTDC back buffer instead of its internal framebuffer, so display
  and mini-GL share no copies. Double-buffered, flipped at vblank.
- **Batch overflow splitting** — a frame with more same-state geometry than
  one batch buffer holds (4096 vertices) splits into multiple backend draws
  at a `glBegin`/`glEnd` boundary instead of dropping geometry (covered by a
  host test, and exercised for real by Radial_Gradient).

## Running

```bash
make BOARD=devboard [DEMO=sketches/....pde]  # devboard: 720x1280 MIPI-DSI
make [DEMO=...]                              # EV1: 1024x600 LVDS
make flash-stlink                            # or: make flash SD=/dev/diskX
```

Sketches written for `size(640, 360)` should draw from `width`/`height` to
fill the panel; BouncyBubbles scales its ball sizes and gravity that way.
