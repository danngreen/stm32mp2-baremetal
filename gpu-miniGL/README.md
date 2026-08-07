# gpu-miniGL — a Processing sketch on the panel

A real Processing example running on the STM32MP257's GPU, baremetal, at the
panel's refresh rate. This is the first end-to-end proof of the project goal:
sketch code → mini-GL (`gpu/gl/`) → the etna 3D pipe → LTDC scanout.

The sketch is **Bouncy Bubbles** (Processing examples, Topics/Motion, based on
code by Keith Peters): twelve semi-transparent bubbles bouncing and colliding
under gravity. `sketch.cc` is a line-for-line translation of the `.pde` — the
physics, constants, and drawing calls are the original's.

```
mini-GL -> LTDC: a Processing sketch on the panel
Display up: 720x1280, sketch running
58 fps, worst render 4998 us, 1 draw(s), 246 dwords
```

That last line is the design working as intended: 12 balls × (a triangle-fan
ellipse each) collapse into **one** GPU draw of 246 dwords per frame, because
every shape shares the same state (white fill, alpha blend on) and mini-GL
batches same-state geometry. The frame rate is vsync-locked to the DSI panel.

## The pieces

| file | what |
| --- | --- |
| `sketch.cc` | the Processing sketch: `sketch_setup()` + `sketch_draw()` |
| `psketch.hh/.cc` | the Processing API surface (`background`, `fill`, `ellipse`, `rect`, `line`, `pushMatrix`, `random`, …) implemented as mini-GL calls |
| `main.cc` | the harness: GPU + display bring-up, double-buffered vblank-flipped frame loop (modeled on `gpu-ltdc-demo`) |

`psketch` mirrors what processing.cpp's fixed-function renderer does: shapes
tessellate on the CPU (`ellipse` → triangle fan, `rect` → quad, strokes → line
loops), transforms go to the GL matrix stack, and `fill`/`stroke` are two
persistent colors applied around each shape. Colors are 0–255, origin is
top-left with +y down, and alpha blending is on by default — all Processing's
defaults, so `fill(255, 204)` just works.

When processing.cpp's own drawing functions get lifted onto mini-GL (the real
plan), `psketch` is what they replace; `sketch.cc` would then compile against
those instead.

## What this exercised for the first time

- **`GpuBackend::set_scanout()`** — the backend resolves each frame directly
  into the LTDC back buffer instead of its internal framebuffer, so display
  and mini-GL share no copies. Double-buffered, flipped at vblank.
- **Batch overflow splitting** — a frame with more same-state geometry than
  one batch buffer holds (4096 vertices) now splits into multiple backend
  draws at a `glBegin`/`glEnd` boundary instead of dropping geometry. (Not hit
  by this sketch — 12 fans is well under — but any denser sketch needs it;
  covered by a host test.)

## Running

```bash
make BOARD=devboard        # custom devboard: 720x1280 MIPI-DSI (ILI9881C)
make                       # EV1: 1024x600 LVDS
make flash-stlink          # or: make flash SD=/dev/diskX
```

The sketch scales its ball sizes by `width/640` and gravity by `height/360`
(the original is `size(640, 360)`), so it plays the same on either panel.
