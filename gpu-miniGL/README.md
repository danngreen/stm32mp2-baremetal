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
  each a stroked rect, so fill/stroke alternate primitive classes every cell:
  73,728 draws and 1.58M command dwords a frame, ~5 fps. This is the workload
  that exposed (and now regression-tests) the ring-wrap-over-tail-WAIT bug in
  `Gpu::submit()` — see the gpu/ README. Uses `millis()`, `color()`, the
  packed-color `fill(int)`/`stroke(int)` overloads, and `keyPressed()`:
  space pauses, `r` reseeds, `c` clears.

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
