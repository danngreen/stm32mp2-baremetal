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

## Input over the console

Characters typed into the board's serial console (a minicom session on the
UART) become Processing key events: each received byte sets `key`, fires the
sketch's `keyPressed()`, and is echoed to the console (`key: 'r'`). The
receiver is polled from the frame loop (`uart_getchar()` in
`shared/print/uart_print.c`), so no interrupt plumbing was needed.

**Enter synthesises a mouse click** at the cursor, which is pinned to the
screen centre until there is a real pointer. Without it a whole category of
examples is unreachable — Multiple_Particle_Systems draws literally nothing
until something calls `mousePressed()`.

There are no key-up or button-up events over a serial line, so `_keyPressed`
and `_mousePressed` are true only during the frame the event arrived in. They
are spelled with the underscore because C++ cannot give a variable and an
event function the same name the way Processing's Java does.

Also verified on hardware, all vsync-locked at 58 fps unless noted:

- **first sweep** — Star, Regular_Polygon, Sine_Cosine, Bounce, Linear,
  Recursion, Rotate, Tree, Wolfram, Brownian. Brownian's 2000 per-segment
  stroke colors still batch into **one** draw: deferred strokes carry color
  per segment, since color is a vertex attribute, not pipeline state.
- **fourth sweep** (`boolean` / `lerpColor` / `IntList`) — Points_and_Lines,
  Bezier, Color_Variables, Simple_Linear_Gradient (33 fps: a per-row
  `lerpColor` gradient), Logical_Operators, Distance_1D, Sine,
  Multiple_Constructors, Functions, Loop, Translate, Milliseconds, Pentigree,
  Penrose_Tile, Follow1, Reach1, IntList_Lottery_example, and three paced by
  their own `frameRate()`: Double_Random and Koch at 1.01 s/frame
  (`frameRate(1)`), Random at 515 ms (`frameRate(2)`). Graphing_2D_Equations
  runs 363 ms/frame — another per-pixel sketch, see TODO.md.
- **third sweep** (class hoisting / `color` type / bezier) — Array,
  Array_2D, Array_Objects, Additive_Wave, Arctangent, Polar_to_Cartesian,
  Sine_Wave, Linear_Interpolation, Distance_2D, Objects, Composite_Objects,
  Inheritance, Arm, Coordinates, Hue, Easing, Penrose_Snowflake,
  ArrayList_of_objects, Button, Reflection1. Everything at 58 fps except
  three that are honest about their own cost: Additive_Wave (its own
  `frameRate(30)`), Distance_2D (46 fps, a 22-draw grid) and Array (34 fps,
  ~2160 line segments a frame — the CPU vertex path, see TODO.md).
- **second sweep** (PVector / noise / arc) — Flocking (300 draws/frame),
  Simple_Particle_System, Multiple_Particle_Systems, Forces_With_Vectors,
  Acceleration_With_Vectors, Bouncing_Ball, Vector_Math, Circle_Collision,
  Reflection2, Morph, Moving_On_Curves, Bouncy_Bubbles, Noise_1D,
  Noise_Wave, Random_Gaussian, Pie_Chart, Shape_Primitives, plus three that
  are slow for their own reasons: Koch (1.01 s/frame — it calls
  `frameRate(1)` itself; its render is 4.8 ms), and Noise_2D / Noise_3D
  (1.8 s and 1.0 s per frame, CPU Perlin noise over 921,600 pixels — see
  TODO.md).

## How a .pde compiles

`tools/pde_prototypes.py` does the three jobs Processing's own preprocessor
does, and `sketch_pde.cc` includes its output:

1. **Forward declarations.** Java ignores declaration order; C++ does not.
   Functions, classes, and cross-tab globals are all declared up front.
2. **One translation unit per sketch folder.** The editor's "tabs" are not
   separate compilation units, so every `.pde` beside the selected one is
   compiled with it — topologically ordered, because a class used as a base
   or a value member must already be complete. (Flocking's `Boid` before
   `Flock`; Multiple_Particle_Systems' `Particle` before `Crazy_Particle`,
   which alphabetical order would get wrong.)
3. **Hoisted class definitions.** A sketch will happily call
   `new EggRing(...)` inside `setup()` and define `EggRing` at the bottom of
   the file. Class and struct bodies are lifted above the functions (brace
   matching that skips comments and literals), so the definition is in scope.
4. **Static mode.** A sketch that is only a list of statements with no
   `setup()`/`draw()` at all — Shape_Primitives, Points_and_Lines,
   Coordinates, most of `Basics/control` — is wrapped into a `setup()` body.
   It draws once and the render target persists, so the image stays up with
   0 draws per frame.

The body is emitted as one generated file rather than a chain of includes,
with `#line` directives so compiler errors still point into the original
`.pde` at the original line.

The `.pde` files themselves stay untouched, provided they are C++-compatible
Processing code (the examples largely are; `fmod` instead of `%` on floats is
the usual edit).

## Java types the sketches assume

`pvector.hh` supplies the three that appear everywhere, matching Processing's
semantics rather than tidier C++ ones:

- **`PVector`** — instance methods mutate *and* return `*this` so they chain
  (`d.normalize().mult(k)` is real sketch code); the statics
  (`PVector::sub(a, b)`) return a new vector and leave their operands alone.
- **`ArrayList<T>`** — holds `T*`, because Java object references are
  pointers and the converted sketches rely on it (`particles.get(i)->run()`).
  `remove(i)` **deletes** the element: in Java that drops the last reference
  and the GC reclaims it, and without it a particle system exhausts the 8 MB
  heap in under an hour.
- **`Array<T>`** — a fixed-size Java array. Its `.length` answers to both
  `a.length` and `a.length()`, since the corpus uses both spellings.

**`color`** is the same collision from the other side: Processing uses it as
both a type (`color c1, c2;`) and a constructor-like function
(`color(0, 200, 0)`), which Java's separate namespaces allow. Here it is a
type whose constructors read exactly like the calls and which converts
implicitly to the packed `0xAARRGGBB` int that `fill()`/`stroke()` take.

Processing's `circle()`/`square()` shorthands are deliberately absent for the
same reason: they are just `ellipse()`/`rect()` with equal dimensions, and as
free functions they collide with the sketch variables of the same name (Morph
declares `ArrayList<PVector> circle`).

The math wrappers (`sqrt`, `sin`, `atan2`, `pow`, …) are templates rather
than `float` overloads: a sketch writing `atan2(y - 5, x - 3)` on ints would
otherwise be ambiguous against `<cmath>`'s float and double versions. A
template loses to an exact non-template match, so genuine float and double
calls still go straight to libm. `text()` is templated for the same reason —
sketches print literals, ints, floats and Strings through it.

`IntList` / `FloatList` / `StringList` (`NumList<T>`) hold values with Java's
method names, and `boolean` and `String` are aliases, so sketches that were
converted with Java spellings intact still compile.

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
