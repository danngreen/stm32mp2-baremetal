# mini-GL

A fixed-function OpenGL 1.x subset on top of the `etna` 3D pipe — enough to run
Processing sketches without a GLSL compiler.

## Why this is tractable

processing.cpp targets **fixed-function OpenGL 1.x**, not modern GLSL. The API
surface it uses is a closed set — extracted from its source rather than guessed:

```
glVertex2f/3f, glBegin/glEnd, glColor4f, glNormal3f, glTexCoord2f,
glMatrixMode/LoadIdentity/PushMatrix/PopMatrix/Ortho/Frustum/Rotatef/Scalef/
  Translatef/MultMatrixd/f,
glEnable/Disable/IsEnabled, glBlendFunc/BlendEquation, glDepthFunc, glCullFace,
glFrontFace, glLineWidth, glPointSize, glScissor, glViewport, glClear/ClearColor,
glLightfv/Lightf/Materialfv/ColorMaterial/ShadeModel,
glGetFloatv/Integerv/Doublev/Booleanv, glGetError, ...
```

~90 functions and ~60 enums. Every draw fixed-function GL can express maps onto
one of a handful of canned shader variants, so **no GLSL compiler is needed** —
the single hardest piece of the Mesa stack is simply not on the path.

## The design decision that matters

**All fixed-function work happens on the CPU** — matrix stack, vertex transform,
lighting, primitive conversion, batching. The backend receives nothing but
already-transformed vertices plus the state to draw them under.

Transforming on the CPU is a performance choice, not a shortcut. Handing the GPU
an MVP as a uniform would make the uniform bank change on every draw, and on
HALTI5 writing shader state costs an `FE→PE` pipeline stall each time (see the
batching section of the main README). With the CPU doing the transform, shader
state never changes, so **a whole frame of geometry batches into one stall-free
submission**. It is also what fixed-function GL does conceptually — there is no
vertex shader in GL 1.x.

The same reasoning applies to lighting: GL evaluates it per-vertex, at
`glVertex` time, producing a colour the rasteriser interpolates. Doing that on
the CPU means the GPU only ever needs the existing pass-through colour shader.

## Layout

| file | what |
| --- | --- |
| `mat4.hh` | column-major 4x4 matrices, GL conventions, `constexpr` |
| `mgl_math.hh` | sqrt/sin/cos/pow without `<cmath>` (unavailable freestanding) |
| `mini_gl.hh` | the GL API surface: types, enums, entry points |
| `mini_gl.cc` | the state machine: stacks, immediate mode, lighting, batching |
| `mini_gl_backend.hh` | the seam — everything above it is pure computation |
| `mini_gl_gpu.hh/.cc` | the backend that drives `etna::Context` |
| `mini_gl_test.cc` | host tests (no hardware) |
| `mini_gl_gpu_test.cc` | on-target test, written in ordinary GL calls |

## Two flips, and they are not the same flip

**Vertex Y.** OpenGL's window origin is bottom-left: NDC y = −1 is the bottom of
the viewport. Our framebuffer's row 0 is the top, and the GPU maps NDC +Y to
*increasing* rows — measured, not assumed (`triangle_test` reports "NDC +Y =
increasing framebuffer rows"). So mini-GL negates clip-space Y once, at the very
end of the transform, in `emit_vertex`. Doing it there rather than in the
projection means every entry point behaves exactly as the GL spec says,
including sketches that already flip via `glOrtho(0,w,h,0,..)` as Processing
does — flipping in the projection would double-flip those.

**Scissor Y.** `glScissor`'s rectangle is measured from the bottom-left, while
`etna::Scissor` counts framebuffer rows from the top, so a GL box `(x,y,w,h)`
becomes rows `[H-(y+h), H-y)`. Handled in `mini_gl_gpu.cc`. Getting this wrong
looks plausible for a centred box and obviously wrong for anything else.

And one range remap that is not a flip: **depth z**. GL clip z spans `[-1,1]`,
but the depth buffer wants window z in `[0,1]`. The backend sets the draw's
viewport z transform to scale/offset `0.5/0.5` — the `glDepthRange(0,1)`
mapping, applied by the PA *after* the perspective divide, so it is correct for
ortho and frustum alike. (The raw etna tests feed z in `[0,1]` directly and use
the default `1.0/0.0`.) Without it, half the GL z range sits below the depth
buffer's floor and the depth test silently misorders geometry. Conventions as
in GL and Processing: the camera looks down −z, so **+z is toward the viewer**
— `mini_gl_gpu_test.cc` proves the z=+0.5 quad occludes the z=−0.5 one.

## Testing

Everything above the backend seam is pure computation, so it is tested on the
development machine:

```bash
make -f gl/Makefile.host test
```

The host backend records what it is handed, and the tests assert on the actual
vertex data. Crucially they check **exact vertex order**, not just counts —
counts cannot distinguish a correct quad split from a bow tie. The suite was
mutation-tested: eleven deliberate bugs (dropped Y flip, pre- instead of
post-multiply, un-alternated triangle-strip winding, wrong `QUAD_STRIP` order,
`LINE_LOOP` without its closing edge, lighting ignoring N·L, batching that never
splits, dropped vertex alpha, a light position not taken into eye space, …) were
each introduced and confirmed to fail the suite.

`mini_gl_gpu_test.cc` then covers what the host cannot: that the translation to
`MeshDraw` is right and the pixels land where GL says. It is written in ordinary
GL calls — the same ones processing.cpp makes.

## Not implemented yet

Deliberately, and calling one records `GL_INVALID_OPERATION` rather than failing
silently:

- **textures** — needs the linear→tiled upload path. `glTexCoord2f` is accepted
  and ignored so a texturing sketch still draws its geometry.
- **framebuffer objects** — `createGraphics()` / `PGraphics`
- **shaders** — `loadShader()` / PShader; needs an offline GLSL compiler
- **`glReadPixels` / `glDrawPixels`** — `loadPixels()` / `save()`
- **stencil** — Processing's `clip()` is rectangular, so `glScissor` covers it
- **`glViewport`** — tracked and reported by `glGetIntegerv`, but only the
  full-target case is honoured; a partial viewport needs `PA_VIEWPORT` changes
- **`glColorMask`** — the PE has the field, but nothing routes it through
  `MeshDraw` yet
