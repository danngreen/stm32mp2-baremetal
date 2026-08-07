# GPU example

This example brings up the GPU clocks/power/reset and shows three engines:
- RS: (Resolve Engine) to do solid fills and blits
- PPU: (Parallel Processing Unit) for running shaders that we generate. The
  shaders are small "programs" that run arbitrary per-pixel computations
- 3D graphics pipe: Drawing a triangle via vertex fetch -> vertex shader ->
  rasterizer -> fragment shader -> pixel engine

I knew very little about how GPUs work before doing this project,
so I learned a lot in getting the GPU to do some real graphics
processing, from the ground up. 

The GPU is a Vivante (VeriSilicon) GC8000 Nano Ultra VIP (rev 0x6205, "HALTI5"
generation). I believe it has:
  — 2 unified shader cores
  - 1 pixel pipe
  - full 3D graphics pipeline
  - 2 NN cores

The etnaviv project, the Mesa project, the Linux etnaviv drivers, and the gcnano
sources were the main sources of knowledge, as there are no reference manuals
for this GPU like we have for the CPU. Everything is built on a small reusable
API (`etna.hh`).

## Bring-up (`etna::Gpu::init()`)

1. **Power**: VDDGPU is an independent supply, driven by the STPMIC25's buck3 on
   the EV1. If TF-A has not turned it on, `init()` enables it over I2C7. 

2. **Clock**: PLL3 is dedicated to the GPU; we run it at 800 MHz. That speed
   needs VDDGPU at 0.90 V. The PMIC init in this project sets buck3 to 0.90 V
   (400 MHz would run at the 0.80 V default).

3. **RIF**: the GPU's register port is RIFSC peripheral 79. We set it secure so
   the GPU issues secure transactions that pass the RISAF firewall to our DDR
   buffers.

At this point we are able to read the chip identity (model 0x8000, rev 0x6205,
product 0x80003, customer 0x15) to confirm the GPU is powered up and responding.

Next, we set up a ring buffer to hold our comannds. At the tail we put
WAIT/LINK commands so the GPU runs in an idle self-loop whenever it gets to the
end processing commands we gave it. When we want to queue some commands we
append them and move the WAIT/LINK to the end. That way the GPU doesn't need to
be re-init each time we use it.

## RS engine — 2D fill and blit

We use the Resolve engine (RS) to fill or do simple operations on areas of
pixels. We do this by putting a list of commands into the command buffer that
specify things like the input/output buffer address and size, pixel format,
color to fill, etc. Then we kick off the front end engine (FE) to process these
commands.

When reading the GPU's feature bits, the BLT bit is set. From what I understand,
the BLT engine would be more appropriate for this kind of operation, but the
etnaviv hardware database in mainline Linux says there is no
BLT engine for our chip. To support that, there's a comment that the database
takes precedence over the feature bits in the registers. On our chip, attempts
to drive BLT registers hangs the FE, and since the Mesa driver uses the RS
engine instead of the BLT for clears, we do that too. 

The RS engine gets its name "resolve" from its primary function of resolving
the tiled GPU-native pixel ordering into the linear ordering a display driver
would expect.

We do two tests:
- `test_fill()`: calls `etna::clear()` to solid-color fill of an RGBA8888 image
- `test_blit_convert()`: calls `etna::blit()` to copy with a per-pixel
  transform. The test swaps R<->B pixels (BlitFlags::BlitSwapRB)


```
RS fill (1024x1024) in 106120 ticks (2529 MB/s)   -- verified. \o/
  vs. CPU fill (1024x1024) in 65341 ticks (4108 MB/s)
```

The RS is about 1.6x slower at filling bytes in DDR RAM than a CPU memset
(about ~106k ticks vs. ~65k ticks). GPUs are good at doing parallel operations,
so this is the worst case test (as you'll see later, when you put the GPU to
work, it's 10x or faster than the CPU). Also, the GPU runs asynchronously (so
is basically "free") and won't dirty our CPU's L1/L2 data cache if we only
intend to fill an area of a framebuffer that's directly displayed on a screen.
It would be interesting to see if the HPDMA would be much faster if you needed
to fill a large area while the CPU does other work.


A final test we do after `test_fill()` and `test_blit_convert()` is to test
the throughoutput of the ring buffer when using the WAIT/LINK scheme vs. when
just sending commands sequentially.

## Compute — running shaders on the PPU

The PPU is the Parallel Processing Unit, which runs shaders. Shaders are small
programs we generate that process pixels. The instruction format is documented in
reference/vivante_isa.xml and implemented in `gckPPU_*` encoders in gcnano.
The instruction set is fairly simple with fixed-width (4 words, or 128 bits)
instructions.

To run a test, we build a shader (which is done by emitting a series of 4-word
instructions) and put it somewhere in RAM that the GPU can access. This happens
in `build_*_shader()` in `ppu_asm.hh`. 

Then we create a command stream similar to the RS command streams, but these
commands are designed to execute ("dispatch") the shader. The command stream is
built in `emit_ppu_dispatch()` in `etna_compute.cc`. The dispatch command stream
is a 118-word binary blob that we extracted from the gcnano sources
"flop-reset". We use it as a template and fill in the addresses to our buffers,
buffer sizes, etc.

In `compute_test()` various shaders are built and then tested with some simple images
(usually solid color areas).

The simplest shader is just a copy operation: it has two instructions: load a
pixel, then store the value in another pixel.

In addition to that, `compute_test()` tests add, saturating add, multiply, bitwise ops,
two-image add/blend, and a per-pixel fractional alpha blend (`out =
mul_hi(a,alpha) + mul_hi(b,255−alpha)`).

These are all tested with small buffers (<16k bytes).

Finally, we do `test_image_blend` which alpha-blends two 512×512 ARGB images
(treated as u8). We measure the time to do that, and compare that to the time
to do the same operation on the CPU.

Here we are actually doing computation with GPU, so we see it's 6-7x faster than a
naive CPU implementation:

```
Alpha-blended two 512x512 ARGB images (GPU) in 155468 ticks
GPU alpha-blended 512x512 ARGB (per-channel lerp) -- verified. \o/
Same blend on the CPU in 1084726 ticks (CPU / GPU = 6.9x)
```

## 3D — the graphics pipe (drawing triangles)

To test the 3D pipeline, we draw triangles and then check the frame buffer
to verify it has the expected pixels.

These tests all have a similar procedure: create an array of vertices (`vtx`),
get some pre-built simple shaders (`vs` and `ps`) that do something simple like
move the color arguments to the right engine. Then plug all these into a
command stream ported from the Mesa Gallium driver. If you want to see how the
porting happened, and where in the Mesa code everything came from, see the
header comments in `etna_3d.cc`.

Compared to other examples, the command stream is complex and runs through
several engines: vertex input (NFE) -> viewport / scissor / rasterizer / PS /
PE state -> shader ICACHE upload (VS + FS from BOs) -> inline uniform -> 
`RA->PE` stall → DRAW_INSTANCED -> drain. The shaders are trivial `MOV`s
(position pass-through VS, constant-color FS). I can't say I understand 
all the stages happening here, but the end result is tested, and draws the
expected triangles with the expected colors, textures and z-depth.

After drawing to the render target, the pixels are in a tiled format, meaning
they are arranged in an order that convenient for the GPU but not the same
order a display driver will want its framebuffer to be in. To resolve this, we
use the Resolver Engine. For one of the examples, we resolve the buffer
and then check if the shape is correct.

The tests are:
- triangle_test(): draw a simple triangle, check if pixels were set and if
  color is uniform. Then resolve it to a framebuffer and check if the shape is
  correct. 
- triangle_color_test(): draw an rgb gradient triangle (each vertex gets a
  color). Check that we see red, green, and blue pixels. Tests the rasterizer
  interpolation.
- triangle_depth_test(): draw two triangles at different z depths. The top
  one is red and the bottom one is green and flipped upside down. Count the 
  number of pixels of each color to check if the red triangle is blocking ~50%
  of the green one.
- triangle_texture_test(): draw a triangle and fill it with a texture. The
  texture is 64x64 with each quadrant a different color. The triangle's UVs
  span all four colors and we use a "NEAREST" filtering so that only exact
  colors from the texture will be used. Then we count how many pixels of 
  each color we found. Here we use a new shader opcode: TEXLD.
- triangle_blend_test(): alpha blending (`glEnable(GL_BLEND)` +
  `glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA)`). Two overlapping quads:
  an opaque red one with blending off, then a half-alpha green one with
  blending on. That splits the target into four regions -- red only, green
  over red, green over blue background, and untouched -- and we probe a 5x5
  block in each against a CPU reference. Blending over *two different*
  destinations is the point: it proves the pixel engine really re-read the
  render target instead of overwriting it.
- primitive_test(): draws all seven primitive types and reduces each to a
  drawn-pixel count and bounding box. The checks are about primitive
  *semantics* rather than exact pixel counts, so they don't depend on
  rasterisation fill rules: TRIANGLE_STRIP and TRIANGLE_FAN fed the same four
  corners must cover the same quad (equal counts), LINE_LOOP must draw
  strictly more than LINE_STRIP on the same three vertices (the closing edge),
  and LINES/POINTS are checked by where their bounding box lands.
- cull_test(): face culling and `glFrontFace`. Asserts only handedness-agnostic
  relationships (see below) and prints the handedness it observes.
- scissor_test(): draws a target-covering quad through several scissor
  rectangles -- disabled, a box, a corner, oversized, and degenerate -- and
  checks the surviving pixels are exactly the rectangle.
- depth_func_test(): lays down a red quad at z=0.5, then a green one at z=0.7
  under each compare function, so the surviving colour reads out the function
  directly. Then `glDepthMask`: a z=0.3 draw with writes off must change the
  colour but not the buffer, proven by a following z=0.4 draw still passing.
- batch_test(): draws the same 24-quad scene twice -- once as 24 emit_mesh
  submits, once as one `Context` batch in a single submission -- and requires
  the two images to be pixel-identical. Reports the dwords each draw emitted
  (first vs. subsequent) and the wall-clock for both paths, for same-state and
  uniform-changing workloads.


### Alpha blending

The pixel engine can blend a fragment with what is already in the render
target, which is what `fill()` with an alpha value needs. Three registers
control it (`PE_ALPHA_CONFIG` holds an enable bit, the four
source/destination factors, and the two blend equations), but the subtle part
is a fourth: `PE_COLOR_FORMAT` has an `OVERWRITE` bit that tells the PE it may
skip *reading* the render target. Blending needs that read, so `OVERWRITE`
must be cleared whenever blending is live. Getting that wrong fails silently
-- the blend math runs against a stale destination.

Both words are derived from one `etna::BlendState` (`etna_blend.hh`), ported
from Mesa's `etna_blend_state_create()` / `etna_update_blend()`. It is all
`constexpr`, so the encodings are proven at compile time -- including that a
default (blending off) `BlendState` reproduces the exact register values the
pipe used before blending existed, which makes the change provably a no-op for
every earlier test.

Two details carried over from Mesa: a blend state that is arithmetically a
no-op (`ONE`/`ZERO`/`ADD`) leaves the blender off so the `OVERWRITE` fast path
survives, and `BLEND_SEPARATE_ALPHA` is only set when the alpha-side factors
actually differ from the color-side ones.

The constant-color factors (`BLEND_FUNC` 11..14) are deliberately not
implemented: the old vendor `gceBLEND_FUNCTION` enum and Mesa/rnndb disagree
on their ordering, and HALTI5 has a second fp16 blend-color register pair we
have not verified. Nothing in the fixed-function GL path needs them.


### Primitive types

The draw command carries a primitive type, so points, lines and triangle
strips/fans cost nothing extra -- they are the same command with a different
field. `etna::Primitive` (`etna_prim.hh`) covers the seven the hardware
assembles:

| type | value | notes |
| --- | --- | --- |
| POINTS | 1 | size from `PA_POINT_SIZE` |
| LINES | 2 | needs `WIDE_LINE`, see below |
| LINE_STRIP | 3 | needs `WIDE_LINE` |
| TRIANGLES | 4 | what everything used before |
| TRIANGLE_STRIP | 5 | needs the `BugFixes8` core fix |
| TRIANGLE_FAN | 6 | |
| LINE_LOOP | 7 | needs the `LineLoop` core feature, needs `WIDE_LINE` |

Mesa gates three of these on feature bits. Rather than guess, they were checked
against Mesa's ST feature database
(`src/etnaviv/hwdb/st/gc_feature_database.h`), which has an entry for our exact
core -- `GCNANOULTRA31_VIP2`, ChipID 0x8000 / Rev 0x6205 / Product 0x80003 /
Customer 0x15. It reports `REG_LineLoop = 1`, `REG_BugFixes8 = 1` and
`REG_WideLine = 1`, so all seven types are usable here.

**The `WIDE_LINE` trap.** rnndb annotates `PA_CONFIG` bit 22 with "MUST be set
when drawing lines when WIDE_LINE feature available, otherwise GC3000+ will not
render lines at all". Our core has the feature, so a line draw *without* this
bit completes cleanly, faults nothing, and produces an empty target -- exactly
the silent failure that is expensive to chase. `pa_config()` sets it for the
three line types. Mesa sets it on every draw when the feature is present; we
set it only for lines so triangle draws keep emitting the byte-identical
`PA_CONFIG` the existing verified tests used (there is a `static_assert` for
that).

The three width registers (`PA_LINE_WIDTH`, `PA_WIDE_LINE_WIDTH0/1`) all take
**half** the width, which is what the previously unexplained `fui(0.5f)` in the
draw path meant: a line width of 1.0. `MeshDraw::line_width` / `point_size` now
carry the real value and the halving happens at emit time.

`GL_QUADS` is deliberately not exposed. The hardware does define a QUADS
primitive (value 8) and Mesa's `translate_draw_mode()` maps to it, but Gallium
never advertises it in `supported_prim_modes`, so that path is untested
upstream. `expand_quads()` turns quad vertices into triangles on the CPU
instead -- which a GL front end has to do for `GL_QUAD_STRIP` regardless.


### Culling, scissor, and depth functions

Three more pieces of fixed-function GL state, all ported from the Mesa etnaviv
driver and all `constexpr` so the encodings are proven at compile time.

**Culling** (`etna_raster.hh`) is `PA_CONFIG.CULL_FACE_MODE`, bits [9:8]. The
field names the winding to **discard** -- 0 off, 1 cull clockwise, 2 cull
counter-clockwise -- not the front face. So `glCullFace` and `glFrontFace`
collapse into one value: cull CCW exactly when "cull the front face" and "front
faces are CCW" agree. All four combinations are checked against Mesa's
`translate_cull_face()` by `static_assert`.

Which winding the hardware calls clockwise is a *window-space* question, and our
viewport maps NDC +Y to increasing framebuffer rows -- the opposite of the usual
GL convention. Rather than guess, `cull_test()` asserts only what must hold
either way: with culling off the triangle draws; for a given vertex order
exactly one of cull-front/cull-back removes it; and reversing either the vertex
order or `glFrontFace` swaps which one does. It then prints the handedness it
actually observed, which is the useful output.

**Scissor** (`etna_raster.hh`) is the `SE_SCISSOR_*` and `SE_CLIP_*` pairs, which
were already being emitted at full-target size. `maxx`/`maxy` are exclusive,
matching Mesa (which clips against `fb->width` directly), and an enabled rect is
clamped to the target the way `etna_update_clipping()` intersects the two. Mesa
emits no `CLIP_LEFT`/`CLIP_TOP` -- the clip rect's origin is implicitly 0 and
only the scissor carries the min corner. This is all Processing's `clip()`
needs, since that is rectangular, so no stencil work is required.

**Depth** (`etna_depth.hh`) is `PE_DEPTH_CONFIG`, which is assembled from two
independent groups: framebuffer-derived (`DEPTH_MODE`, `DEPTH_FORMAT`, `UNK18`)
and depth-state-derived (`DEPTH_FUNC`, `WRITE_ENABLE`, `EARLY_Z`,
`DISABLE_ZS`). That is why "no depth buffer" and "depth buffer present but test
off" are different words -- only the first has `DEPTH_MODE = NONE`. A disabled
test still runs the stage with `ALWAYS`, which is what keeps depth *writes*
working with the test off. `DISABLE_ZS` switches the late depth/stencil stage
off entirely and is set when neither a test nor a write needs it.

Early-Z is deliberately left off. Mesa enables it only under a pile of
conditions (the `RA_WRITE_DEPTH` feature, no alpha test, shader neither writes Z
nor discards, render target not linear), and rnndb warns that the late stage
must be disabled when early writes are active "otherwise the GPU hangs". Our
`RA_EARLY_DEPTH` value is the late-Z one the depth test was verified with;
revisit only with a hardware test.


### Batching and dirty-state emission

`emit_mesh()` is self-contained: every call re-emits ~100 state registers, both
shader programs, the uniforms, and four pipeline stalls, then drains the pixel
engine. That is right for one draw per submission, and it is what the tests
above use — but it is roughly 250 dwords and several full pipeline drains per
draw. Fine for twelve cubes; hopeless for a sketch drawing a thousand shapes.

`etna::Context` (`etna_context.hh`) remembers the state it last emitted. Each
draw diffs against it and emits only the register blocks whose group changed —
and, the part that actually matters, emits a pipeline sync **only when the state
it guards has changed**. Ported from Mesa's `etna_emit_state()`, whose three
sync points are the complete list:

1. cache flush + `RA->PE` stall, before changing framebuffer/blend/depth state
   (the caches still hold pixels rendered with the *old* state)
2. `FE->PE` stall, before loading shader/uniform state — from HALTI0 on those
   registers are **not self-synchronizing**, so writing them while a draw is in
   flight corrupts it
3. ICACHE prefetch + `RA->PE` stall, closing the shader/uniform block

There is deliberately no sync *between* draws: the PE orders fragments to the
same render target itself, which is why Mesa emits draws back to back. The
stall/flush/stall drain is a per-**submission** cost and lives in `drain()`.

```cpp
Context ctx{cs};
for (auto &d : draws) ctx.draw(d);   // deltas only
ctx.drain();                         // once
gpu.submit_and_wait(cs);
```

`emit_mesh()` is now just a one-shot Context — `draw()` then `drain()` — so
there is a single implementation, and the first draw of any Context emits
exactly the register sequence, in the same order, that the pipe emitted before
dirty tracking existed. The five hardware-verified 3D tests all go through
`emit_mesh`, so they are the regression suite for the full-emit path.

**The vertex arena is not optional.** With one draw per submission you can
upload vertices into one buffer, submit, and wait — the GPU is done before the
CPU touches it again. As soon as several draws share a submission that stops
being true: the FE reads vertex buffers asynchronously right up until the fence,
so re-uploading into the same buffer between batched draws races the GPU and
renders nondeterministic garbage. `etna::Arena` (`etna_arena.hh`) hands out
non-overlapping slices of a pool and is reset only after the fence.

Two other sharp edges worth knowing:

- **Ring capacity.** A batch is one big submission, and the WAIT/LINK ring is
  4096 dwords. `Gpu::submit()` previously wrapped by resetting the head, which
  for an oversized block would have copied past the end of the ring — silent
  memory corruption. It now refuses and says so.
- **Context staleness.** Context tracks what *it* emitted. RS clears, blits and
  resolves don't touch 3D state, so they interleave freely; anything else that
  reprograms the pipe behind its back (a PPU compute dispatch does) must be
  followed by `invalidate()`.

One expected non-win: changing uniforms per draw means writing shader state,
which costs the `FE->PE` stall every time. That is correct, not a regression —
and it is exactly why the Processing fast path wants a constant transform with
colour in the vertex attributes rather than a per-draw MVP uniform.


### Spinning cube

`spinning_cube_test` draws a cube with 36-vertices (six solid-colored faces),
at 8 rotation angles. Rotation is done by giving the GPU some CPU-calculated
transformation coefficients ("model-view-projection"). Snapshots at 8 different
angles are made and compared to a 100% CPU-rendered image.

Some new things checked here:
- Multi-instruction vertex shader (VS): `clip = M × position` as `MUL` +
  3×`MAD` accumulating the matrix columns, plus the color passthrough. This is
  built by a small `constexpr` instruction builder (`alu_inst()` in
  `etna_3d_tests.cc`). The builder is self-checking: `static_assert`s prove it
  reproduces the two hardware-verified `MOV` encodings bit-for-bit at compile
  time.
- `emit_mesh()` is a general helper in the `etna_3d.hh` that draws a `MeshDraw`
  struct — any vertex count, parametric shader sizes, optional uniforms and
  depth.

## The `etna` API

The "API" is modeled on libdrm's `etna_cmd_stream` / `etna_bo` interface, 
which goes between Gallium and the kernel drivers.

- **`etna::Gpu`**  
    - `init()`: bring-up + ring buffer setup-
    - `alloc()` DDR pool
    - `submit()` appends to the ring and patches the idle WAIT into a LINK so ops queue back-to-back (ops must not emit
  `END` — that halts the ring). 
    - `wait()` sleeps on `WFE` until the GPU interrupt fires
- **`etna::Bo`** 
    — a physically-contiguous buffer
    - `cpu_prep()`/`cpu_fini()` need to be used before/after reading/writing because the buffer is cached 
- **`etna::CmdStream`** 
    — a growable command buffer with helpers similar to libdrm/Mesa (`emit`/`reserve`/`set_state`/`emit_reloc`/`stall`)
- **Operations** 
    — `clear()`/`blit()` (RS)
    - `make_kernel()`/`compute()` (PPU),


## How this was written/ported

ST does not document the GPU register map. Everything comes from the
reverse-engineered **etnaviv** project and Mesa's etnaviv/gallium drivers.
All sources we consulted as licensed MIT or GPL-v2.0.
I used an LLM extensively to do things like mechanically port the command
sequences from the Mesa driver and etnaviv project into the unified
step-by-step sequences you see in etna_3d.cc, and to test the behavior of each
opcode and refine the bit-level assembly of shaders until the results were as
expected.

- Register values (`gpu_regs.hh`): Mostly copied from etnaviv `state.xml.h`,
  `state_hi.xml.h`, `cmdstream.xml.h`. 
    - RS command stream from `etnaviv_rs.c`
- Shader ISA (`ppu_asm.hh`): the gcnano vendor `gckPPU_*` encoders and the
  Vivante `rnndb/isa.xml` opcode table from etna_viv.
- **3D pipe** (`gpu_regs_3d.hh`, `etna_3d.cc`): Mesa's
  `src/etnaviv/hw/state_3d.xml.h` and the Gallium `etnaviv_emit.c` and
  `etnaviv_context.c` draw path.

Reference repos:
- [Mesa](https://gitlab.freedesktop.org/mesa/mesa):
    - [src/gallium/drivers/etnaviv](https://gitlab.freedesktop.org/mesa/mesa/-/tree/main/src/gallium/drivers/etnaviv?ref_type=heads)
    - [src/etnaviv/hw](https://gitlab.freedesktop.org/mesa/mesa/-/tree/main/src/etnaviv/hw?ref_type=heads)
- [Mesa Libdrm](https://gitlab.freedesktop.org/mesa/libdrm)
- [ST's Linux fork, v6.6-stm32mp](https://github.com/STMicroelectronics/linux/tree/v6.6-stm32mp)
    - [drivers/gpu/drm/etnaviv](https://github.com/STMicroelectronics/linux/tree/v6.6-stm32mp/drivers/gpu/drm/etnaviv)
- (gcnano 6.4.19 sources)[https://github.com/STMicroelectronics/gcnano-binaries]
- [etna_viv](https://github.com/etnaviv/etna_viv)


## Expected output

```
GPU Example (etna API)
======================

etna: bringing up GPU
etna: VDDGPU not present (CR12 = 0x0)
etna: VDDGPU off -- trying to enable buck3 over I2C7...
pmic: product ID 0x20, version 0x11
pmic: Buck3 (VDDGPU) was: voltage code 0, control 0x0
pmic: Buck3 (VDDGPU) now: voltage code 40 (900mV), control 0x1
etna: gpu pll set to 800 MHz
etna: GPU mem-clock ~600 MHz

etna: GC model 0x8000 rev 0x6205 (product 0x80003, customer 0x15)

RS Engine tests:
RS fill (1024x1024) in 106120 ticks (2529 MB/s)   -- verified. \o/
  vs. CPU fill (1024x1024) in 65341 ticks (4108 MB/s)
RS blit+convert (1024x1024) in 607153 ticks
GPU copied 1024x1024, swapping R<->B -- verified. \o/
Ring throughput (16 x 64x64 clears): sequential 5269 ticks, pipelined 3336 ticks

PPU compute/shader tests:
GPU copy over 64x6 (384 bytes) in 422 ticks -- verified. \o/
GPU copy over 128x32 (4096 bytes) in 618 ticks -- verified. \o/
GPU copy over 256x64 (16384 bytes) in 1170 ticks -- verified. \o/
GPU copy over 32x4 (128 bytes) in 353 ticks -- verified. \o/
GPU add(out=in+in) over 128x32 (4096 bytes) in 529 ticks -- verified. \o/
GPU addsat(min(in+in,255)) over 128x32 (4096 bytes) in 531 ticks -- verified. \o/
GPU add2(A+B, ramp+inv=255) over 128x32 (4096 bytes) in 786 ticks -- verified. \o/
GPU blend-add(sat(A+B)) over 128x32 (4096 bytes) in 770 ticks -- verified. \o/
GPU and(in & 0x0F, imm) over 64x6 (384 bytes) in 355 ticks -- verified. \o/
GPU flop-reset(dp2x8, const in) over 64x6 (384 bytes) in 385 ticks -- verified. \o/
GPU mul2((A*B)&0xFF) over 64x6 (384 bytes) in 456 ticks -- verified. \o/
GPU mulhi2(mul_hi(A,B)) over 64x6 (384 bytes) in 474 ticks -- verified. \o/
GPU not(~in) over 64x6 (384 bytes) in 437 ticks -- verified. \o/
GPU blend-lerp(a*A + b*(1-A)) over 128x32 (4096 bytes) in 947 ticks -- verified. \o/
Alpha-blended two 512x512 ARGB images (GPU) in 155770 ticks
GPU alpha-blended 512x512 ARGB (per-channel lerp) -- verified. \o/
Same blend on the CPU in 1084172 ticks (CPU / GPU = 6.9x)

3D tests:
3D triangle drawn in 510 ticks
RT: 1301 of 4096 pixels drawn, color 0xFFFF0000 (uniform)
GPU drew a solid triangle in 0xFFFF0000 -- 3D pipe verified. \o/
RS resolve (untile 64x64) in 417 ticks
shape: exact (NDC +Y = increasing framebuffer rows)
resolved image matches the expected triangle -- shape verified. \o/
gradient triangle drawn in 605 ticks
RT: 1301 of 4096 pixels drawn. corners seen R=1 G=1 B=1 (varied)
GPU interpolated a per-vertex-color varying across the triangle. \o/
depth: two triangles drawn in 944 ticks
depth test: 1951 drawn -> 1301 red (near), 650 green (far)
GPU depth test occluded the farther triangle -- depth buffer works. \o/
textured triangle drawn in 669 ticks
texture test: 1301 drawn -> R=469 G=494 B=156 W=182 other=0
GPU sampled a 2D texture across the triangle -- texturing works. \o/
(in the blend and primitive blocks below, values marked NNN are placeholders
 until a board run. The triangle strip/fan counts and all the bounding boxes
 ARE predicted -- they come from a CPU rasterisation of the same geometry --
 so a mismatch there is a real signal, not just an unfilled placeholder.)
blend: two quads drawn in NNNN ticks (PE_ALPHA_CONFIG 0x05400541)
  A only (opaque red) at (12,32): expect 0xFFFF0000 ok (max delta N)
  A n B (green over red) at (32,32): expect 0xBF808000 ok (max delta N)
  B only (green over blue) at (51,32): expect 0xBF008080 ok (max delta N)
  untouched (clear blue) at (51,57): expect 0xFF0000FF ok (max delta N)
GPU alpha-blended over two different destinations -- PE blending works. \o/
primitive types:
  TRIANGLES    : 528 px bbox x[16..47] y[16..47] (1 prims)
  TRIANGLE_STRIP: 1024 px bbox x[16..47] y[16..47] (2 prims)
  TRIANGLE_FAN : 1024 px bbox x[16..47] y[16..47] (2 prims)
  LINES        : NNN px bbox x[16..48] y[32..32] (1 prims)
  LINE_STRIP   : NNN px bbox x[12..51] y[12..51] (2 prims)
  LINE_LOOP    : NNN px bbox x[12..51] y[12..51] (3 prims)
  POINTS       : NNN px bbox x[16..48] y[16..48] (4 prims)
GPU assembled points, lines, line strips/loops, and triangle strips/fans. \o/
face culling:
  ccw verts, cull off  : NNN px bbox x[..] y[..]
  ccw verts, cull back : NNN px
  ccw verts, cull front: NNN px
  cw  verts, cull back : NNN px
  cw  verts, cull front: NNN px
  ccw verts, cull back, frontFace=CW : NNN px
  ccw verts, cull front, frontFace=CW: NNN px
  observed: with frontFace=CCW, a CCW-in-NDC triangle is ???-facing in window space
GPU culled by winding, and glFrontFace flips it. \o/
scissor:
  disabled      : 4096 px bbox x[0..63] y[0..63]
  [16,8)-(48,40): 1024 px bbox x[16..47] y[8..39]
  [0,0)-(8,8)   : 64 px bbox x[0..7] y[0..7]
  oversized     : 4096 px bbox x[0..63] y[0..63]
  empty         : 0 px
GPU clipped to the scissor rectangle. \o/
depth compare functions (red at z=0.5, then green at z=0.7):
  LESS     : got 0xFFFF0000 expect 0xFFFF0000  ok
  GREATER  : got 0xFF00FF00 expect 0xFF00FF00  ok
  ALWAYS   : got 0xFF00FF00 expect 0xFF00FF00  ok
  NEVER    : got 0xFFFF0000 expect 0xFFFF0000  ok
  LEQUAL   : got 0xFFFF0000 expect 0xFFFF0000  ok
  GEQUAL   : got 0xFF00FF00 expect 0xFF00FF00  ok
  depth mask: after z=0.3 (write off) then z=0.4, centre is 0xFF0000FF expect 0xFF0000FF
GPU honoured all six depth compare functions and the depth write mask. \o/
batching (24 same-state quads):
  per-draw dwords: first NNN, subsequent NNN
  total stream NNN dwords for 24 draws
  unbatched NNN ticks (24 submits), batched NNN ticks (1 submit) -- N.Nx
  uniform-changing batch: per-draw dwords first NNN, subsequent NNN; NNN ticks (each draw costs an FE->PE stall -- expected)
GPU batched 24 draws into one submission, pixel-identical to per-draw submits. \o/
cube frame 0: 658 px drawn, 0 mismatches (562 edge px ignored)
cube frame 1: 761 px drawn, 0 mismatches (687 edge px ignored)
cube frame 2: 721 px drawn, 0 mismatches (613 edge px ignored)
cube frame 3: 715 px drawn, 0 mismatches (623 edge px ignored)
cube frame 4: 635 px drawn, 0 mismatches (558 edge px ignored)
cube frame 5: 771 px drawn, 0 mismatches (686 edge px ignored)
cube frame 6: 739 px drawn, 0 mismatches (628 edge px ignored)
cube frame 7: 735 px drawn, 0 mismatches (645 edge px ignored)
spinning cube: 8 frames avg 1102 ticks (draw+resolve), faces seen 0x3F
GPU spun a cube: VS matrix transform + depth + rasterization all match the CPU. \o/

SUCCESS
```

## Running

```bash
make
make flash SD=/dev/diskX   # or copy build/main.uimg to the app partition

# or flash using TRACE32: make flash-t32
```
