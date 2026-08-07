#include "../etna.hh"
#include "aarch64/system_reg.hh"
#include "mini_gl.hh"
#include "mini_gl_gpu.hh"
#include "print/print.hh"

// =============================================================================
//  mini_gl_gpu_test.cc -- mini-GL over the real 3D pipe
// =============================================================================
//
// The host tests (gl/mini_gl_test.cc) already prove the state machine: matrix
// stack, transform, lighting, primitive conversion, batching. What they cannot
// prove is that the translation to MeshDraw is right and that the resulting
// pixels land where GL says they should. That is what this checks, and it is
// deliberately written in ordinary GL calls -- the same ones processing.cpp
// makes -- rather than in etna:: terms.

using namespace mgl;

namespace
{
constexpr uint32_t W = 64, H = 64;

// Read back the resolved, linear framebuffer.
uint32_t px(GpuBackend &be, uint32_t x, uint32_t y)
{
	return be.framebuffer().span<const uint32_t>()[y * W + x];
}

// Channel helpers on 0xAARRGGBB.
uint32_t chan_r(uint32_t p)
{
	return (p >> 16) & 0xFF;
}
uint32_t chan_g(uint32_t p)
{
	return (p >> 8) & 0xFF;
}
uint32_t chan_b(uint32_t p)
{
	return p & 0xFF;
}

bool near_eq(uint32_t a, uint32_t b, uint32_t tol = 3)
{
	return (a > b ? a - b : b - a) <= tol;
}

// Set up Processing's usual 2D projection: origin top-left, +Y down, one unit
// per pixel. Everything below is written in those coordinates.
void setup_2d()
{
	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	glOrtho(0, W, H, 0, -1, 1);
	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();
}
} // namespace

bool mini_gl_test(etna::Gpu &gpu)
{
	static GpuBackend be;
	if (!be.init(gpu, W, H, /*with_depth=*/true)) {
		print("FAILED: mini-GL backend init\n");
		return false;
	}
	mglInit(be);

	// --- 1. orientation: a rect in the TOP-LEFT quadrant must land there -----
	// This is the end-to-end check on the Y flip. If the flip were wrong or
	// doubled, the rect would appear in the bottom-left and everything else in
	// this file would still pass -- so it is checked first and explicitly.
	mglBeginFrame();
	setup_2d();
	glClearColor(0, 0, 0, 1);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	glColor4f(1, 0, 0, 1);
	glBegin(GL_QUADS);
	glVertex2f(4, 4);
	glVertex2f(28, 4);
	glVertex2f(28, 28);
	glVertex2f(4, 28);
	glEnd();
	mglEndFrame();

	const uint32_t top_left = px(be, 16, 16), bottom_left = px(be, 16, 48);
	print("mini-GL orientation: rect(4,4,24,24) -> (16,16)=0x", Hex{top_left}, " (16,48)=0x",
		  Hex{bottom_left}, "\n");
	if (chan_r(top_left) < 200 || chan_r(bottom_left) > 60) {
		print("FAILED: a top-left rect did not land in the top-left"
			  " -- the viewport Y flip is wrong\n");
		return false;
	}

	// --- 2. the motivating snippet: alpha blending ---------------------------
	// Opaque red, then 50% green over half of it. This is the exact sequence
	// from the original question, expressed in GL.
	mglBeginFrame();
	setup_2d();
	glClearColor(0, 0, 1, 1); // blue background
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	glDisable(GL_BLEND);
	glColor4f(1, 0, 0, 1);
	glBegin(GL_QUADS);
	glVertex2f(0, 16);
	glVertex2f(32, 16);
	glVertex2f(32, 48);
	glVertex2f(0, 48);
	glEnd();

	glColor4f(0, 1, 0, 0.5f);
	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	glBegin(GL_QUADS);
	glVertex2f(16, 16);
	glVertex2f(64, 16);
	glVertex2f(64, 48);
	glVertex2f(16, 48);
	glEnd();
	mglEndFrame();

	const uint32_t red_only = px(be, 8, 32);	 // red, unblended
	const uint32_t over_red = px(be, 24, 32);	 // green 50% over red
	const uint32_t over_blue = px(be, 48, 32);	 // green 50% over blue
	print("mini-GL blend: red 0x", Hex{red_only}, " green/red 0x", Hex{over_red}, " green/blue 0x",
		  Hex{over_blue}, "\n");
	if (!near_eq(chan_r(red_only), 255) || chan_g(red_only) > 8) {
		print("FAILED: the opaque red quad is wrong\n");
		return false;
	}
	if (!near_eq(chan_r(over_red), 128) || !near_eq(chan_g(over_red), 128)) {
		print("FAILED: 50% green over red should be about (128,128,0)\n");
		return false;
	}
	if (!near_eq(chan_g(over_blue), 128) || !near_eq(chan_b(over_blue), 128)) {
		print("FAILED: 50% green over blue should be about (0,128,128)\n");
		return false;
	}

	// --- 3. the matrix stack moves geometry ----------------------------------
	// Same rect drawn through a translate; it must appear translated, and
	// popMatrix must put the transform back.
	mglBeginFrame();
	setup_2d();
	glClearColor(0, 0, 0, 1);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	glDisable(GL_BLEND);
	glColor4f(0, 1, 0, 1);
	glPushMatrix();
	glTranslatef(32, 32, 0);
	glBegin(GL_QUADS);
	glVertex2f(0, 0);
	glVertex2f(24, 0);
	glVertex2f(24, 24);
	glVertex2f(0, 24);
	glEnd();
	glPopMatrix();
	mglEndFrame();

	const uint32_t translated = px(be, 44, 44), origin = px(be, 8, 8);
	print("mini-GL transform: translated(44,44)=0x", Hex{translated}, " origin(8,8)=0x", Hex{origin}, "\n");
	if (chan_g(translated) < 200 || chan_g(origin) > 60) {
		print("FAILED: glTranslatef did not move the geometry\n");
		return false;
	}

	// --- 4. batching: many shapes, one submission ----------------------------
	// The whole frame is same-state geometry, so it should collapse to ONE
	// backend batch no matter how many glBegin/glEnd pairs there were.
	mglBeginFrame();
	setup_2d();
	glClearColor(0, 0, 0, 1);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	const auto t0 = read_cntpct();
	for (uint32_t i = 0; i < 100; i++) {
		const float x = float((i * 7) % 56), y = float((i * 13) % 56);
		glColor4f(float(40 + (i * 37) % 200) / 255.0f, float(40 + (i * 91) % 200) / 255.0f,
				  float(40 + (i * 53) % 200) / 255.0f, 1.0f);
		glBegin(GL_QUADS);
		glVertex2f(x, y);
		glVertex2f(x + 6, y);
		glVertex2f(x + 6, y + 6);
		glVertex2f(x, y + 6);
		glEnd();
	}
	mglEndFrame();
	const uint32_t ticks = uint32_t(read_cntpct() - t0);
	// Stats read after mglEndFrame: the last batch is only handed to the
	// backend (and counted) by the flush inside it.
	const FrameStats st = mglFrameStats();

	print("mini-GL batching: 100 quads -> ", st.batches, " batch(es), ", st.vertices, " verts, ",
		  st.begin_end, " begin/end pairs, ", ticks, " ticks\n");
	if (st.batches != 1) {
		print("FAILED: 100 same-state quads should merge into 1 batch, got ", st.batches, "\n");
		return false;
	}
	if (st.vertices != 600) {
		print("FAILED: 100 quads should be 600 triangle vertices, got ", st.vertices, "\n");
		return false;
	}
	if (be.overflowed()) {
		print("FAILED: the backend ran out of arena or command stream\n");
		return false;
	}

	// --- 5. depth test through GL --------------------------------------------
	mglBeginFrame();
	setup_2d();
	glClearColor(0, 0, 0, 1);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	glDisable(GL_BLEND);
	glEnable(GL_DEPTH_TEST);
	glDepthFunc(GL_LESS);
	// GL convention (and Processing's): the camera looks down -z, so +z is
	// TOWARD the viewer. z=+0.5 is the near quad, z=-0.5 the far one.
	glColor4f(1, 0, 0, 1); // near
	glBegin(GL_QUADS);
	glVertex3f(8, 8, 0.5f);
	glVertex3f(56, 8, 0.5f);
	glVertex3f(56, 56, 0.5f);
	glVertex3f(8, 56, 0.5f);
	glEnd();
	glColor4f(0, 1, 0, 1); // farther: must be rejected
	glBegin(GL_QUADS);
	glVertex3f(8, 8, -0.5f);
	glVertex3f(56, 8, -0.5f);
	glVertex3f(56, 56, -0.5f);
	glVertex3f(8, 56, -0.5f);
	glEnd();
	glDisable(GL_DEPTH_TEST);
	mglEndFrame();

	const uint32_t depth_px = px(be, 32, 32);
	print("mini-GL depth: near red then far green -> 0x", Hex{depth_px}, "\n");
	if (chan_r(depth_px) < 200 || chan_g(depth_px) > 60) {
		print("FAILED: the farther quad should have been depth-rejected\n");
		return false;
	}

	if (glGetError() != GL_NO_ERROR) {
		print("FAILED: mini-GL reported an error during the run\n");
		return false;
	}

	print("mini-GL drew through the real pipe: orientation, blending, transforms,"
		  " batching and depth. \\o/\n");
	return true;
}
