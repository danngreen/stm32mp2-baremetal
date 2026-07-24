# HPDMA Batch - Demo of 2D rendering straight into the framebuffer

This showcases the HPDMA's ability to queue requests, making it extremely efficient
for things like drawing a series of rectangles in a framebuffer.

If you have an LVDS or MIPI-DSI panel attached, you should see bouncing colored
rectangles over a gradient, drawn directly into the linear framebuffer with the
CPU (no GPU). If you don't have a display, you can still see the performance 
numbers.

Double-buffered and flip on vblank (tear-free).

## Four draw strategies

`main.cc` has a `Mode` (`enum class Draw`) selecting how each frame is drawn:

- **`FullCpu`** — CPU repaints the whole screen every frame (background +
  sprites). On a 720×1280 screen it's ~3.7 MB of CPU writes/frame.
- **`DirtyCpu`** — keep the background, each frame only
  erase the sprites' old footprints and redraw them at
  their new positions. Touches only a few % of the screen.
- **`DirtyDma`** — the same as DirtyCpu but using the HPDMA to do the fills
  and the background-restore blits. This uses dma2d.cc, sending one fill() or clear()
  at a time.
- **`DirtyDmaAsync`** (default) — the same as DirtyDma but queues all the rect
  clears and fills into a batch (aka a queue) and sends one HPDMA request to do
  all the drawing. The DMA processes the queue on its own while the CPU is free
  to do whatever it wants. The CPU takes about 1us per ~60fps to build and send
  the HPDMA request, which is about a 0.006% processing load.

The per-frame log shows the difference directly:

## Running

```bash
make                  # EV1: 1024×600 LVDS  (USART2 console)
make BOARD=devboard   # custom devboard: 720×1280 MIPI-DSI  (USART1 console)
```

You should see the rectangles bouncing over the gradient, and the fps / dirty-
bytes line printing every 120 frames. Change `Mode` (top of `main.cc`) between
`FullCpu`, `DirtyCpu`, `DirtyDma`, and `DirtyDmaAsync` and rebuild to compare:

Typical output for DirtyDmaAsync mode:
```
58 fps, worst draw 5659 us (CPU issue 1 us, DMA runs async), 738 KiB/frame (of 3600 KiB full)
```

This indicates it took the CPU 1 us to build and kick off the DMA sequence for
the entire frame (58fps). The worst draw time of 5.6ms is the time it took the
DMA to excecute the sequence (plus the CPU's 1us). KiB/frame is essentially the
number of pixels drawn, and the "3600 KiB full" refers to how many pixels the 
entire screen has (so you can estimate 738/3600 = 20% of the screen was re-drawn)
