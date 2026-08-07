// =============================================================================
//  mini_gl_test.cc -- host tests for the mini-GL state machine
// =============================================================================
//
// Builds and runs on the development machine, NOT on the board:
//     make -f gl/Makefile.host test
//
// Everything mini-GL does above the backend seam is pure computation -- matrix
// stack, vertex transform, lighting, primitive conversion, batching -- which is
// exactly where the bugs are and exactly what does not need hardware to find.
// The backend here records what it is handed so tests can assert on it.

#include "mini_gl.hh"
#include <cmath>
#include <cstdio>
#include <span>
#include <string>
#include <vector>

using namespace mgl;

namespace
{

struct RecordBackend : Backend {
	struct Draw {
		BatchState state;
		std::vector<float> verts;
		uint32_t count;
	};
	struct Clear {
		uint32_t mask;
		float r, g, b, a;
	};
	std::vector<Draw> draws;
	std::vector<Clear> clears;
	uint32_t w_ = 200, h_ = 100;

	void clear(uint32_t mask, float r, float g, float b, float a, float) override
	{
		clears.push_back({mask, r, g, b, a});
	}
	void draw(const BatchState &s, std::span<const float> v, uint32_t n) override
	{
		draws.push_back({s, std::vector<float>(v.begin(), v.end()), n});
	}
	uint32_t width() const override
	{
		return w_;
	}
	uint32_t height() const override
	{
		return h_;
	}
	void reset()
	{
		draws.clear();
		clears.clear();
	}
};

int failures = 0;
int checks = 0;

void check(bool ok, const std::string &what)
{
	checks++;
	if (!ok) {
		failures++;
		printf("  FAIL: %s\n", what.c_str());
	}
}

void close_to(float got, float want, const std::string &what, float tol = 1e-4f)
{
	checks++;
	if (std::fabs(got - want) > tol) {
		failures++;
		printf("  FAIL: %s (got %.6f want %.6f)\n", what.c_str(), got, want);
	}
}

// A vertex out of a recorded draw.
struct V {
	float x, y, z, r, g, b, a;
};
V vert(const RecordBackend::Draw &d, uint32_t i)
{
	const float *p = d.verts.data() + i * kFloatsPerVertex;
	return V{p[0], p[1], p[2], p[3], p[4], p[5], p[6]};
}

// -----------------------------------------------------------------------------
void test_matrix_stack(RecordBackend &be)
{
	printf("matrix stack:\n");
	mglInit(be);

	// GL post-multiplies, so the LAST command is applied FIRST to a vertex:
	// translate-then-scale scales in the translated frame.
	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();
	glTranslatef(10, 0, 0);
	glScalef(2, 2, 2);
	float m[16];
	glGetFloatv(GL_MODELVIEW_MATRIX, m);
	// Point (1,0,0) -> scale -> (2,0,0) -> translate -> (12,0,0)
	close_to(m[0], 2.0f, "translate*scale: scale survives in m[0]");
	close_to(m[12], 10.0f, "translate*scale: translation in m[12]");

	// push/pop restores exactly.
	glLoadIdentity();
	glTranslatef(5, 6, 7);
	glPushMatrix();
	glRotatef(90, 0, 0, 1);
	glScalef(3, 3, 3);
	glPopMatrix();
	glGetFloatv(GL_MODELVIEW_MATRIX, m);
	close_to(m[12], 5.0f, "popMatrix restores translation x");
	close_to(m[13], 6.0f, "popMatrix restores translation y");
	close_to(m[0], 1.0f, "popMatrix discards the rotation/scale");

	// The two stacks are independent.
	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	glOrtho(0, 200, 100, 0, -1, 1);
	glMatrixMode(GL_MODELVIEW);
	glGetFloatv(GL_MODELVIEW_MATRIX, m);
	close_to(m[12], 5.0f, "projection edits do not disturb modelview");

	// Underflow is an error, not a crash.
	glGetError();
	glPopMatrix();
	check(glGetError() == GL_INVALID_OPERATION, "popping an empty stack sets GL_INVALID_OPERATION");

	// Deep push/pop is balanced.
	glLoadIdentity();
	for (int i = 0; i < 20; i++) {
		glPushMatrix();
		glTranslatef(1, 0, 0);
	}
	for (int i = 0; i < 20; i++)
		glPopMatrix();
	glGetFloatv(GL_MODELVIEW_MATRIX, m);
	close_to(m[12], 0.0f, "20 balanced push/pop pairs return to identity");
}

// -----------------------------------------------------------------------------
void test_y_flip(RecordBackend &be)
{
	printf("viewport orientation (the Y flip):\n");
	mglInit(be);
	be.reset();

	// Processing's usual setup: origin top-left, +Y downward on screen.
	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	glOrtho(0, 200, 100, 0, -1, 1);
	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();

	mglBeginFrame();
	glBegin(GL_TRIANGLES);
	glVertex2f(0, 0);	  // top-left in Processing coords
	glVertex2f(200, 0);	  // top-right
	glVertex2f(0, 100);	  // bottom-left
	glEnd();
	mglEndFrame();

	check(be.draws.size() == 1, "one batch emitted");
	if (be.draws.empty())
		return;
	const auto &d = be.draws[0];

	// The GPU maps NDC +Y to INCREASING framebuffer rows (rows run downward),
	// so a Processing y=0 (top of screen) vertex must come out at NDC y = -1.
	const V top = vert(d, 0), bottom = vert(d, 2);
	close_to(top.x, -1.0f, "x=0 maps to NDC x=-1");
	close_to(top.y, -1.0f, "Processing y=0 (top) maps to device NDC y=-1");
	close_to(bottom.y, 1.0f, "Processing y=height (bottom) maps to device NDC y=+1");
	check(top.y < bottom.y, "increasing Processing y increases device NDC y (downward)");

	// And a standard bottom-left-origin GL projection must come out the other
	// way round -- proving the flip lives in the viewport step, not the
	// projection, so both conventions work.
	be.reset();
	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	glOrtho(0, 200, 0, 100, -1, 1); // GL-style: y=0 at the BOTTOM
	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();
	mglBeginFrame();
	glBegin(GL_TRIANGLES);
	glVertex2f(0, 0); // bottom-left in GL coords
	glVertex2f(200, 0);
	glVertex2f(0, 100); // top-left
	glEnd();
	mglEndFrame();
	if (!be.draws.empty()) {
		const V gl_bottom = vert(be.draws[0], 0), gl_top = vert(be.draws[0], 2);
		close_to(gl_bottom.y, 1.0f, "GL y=0 (bottom) maps to device NDC y=+1");
		close_to(gl_top.y, -1.0f, "GL y=height (top) maps to device NDC y=-1");
	}
}

// -----------------------------------------------------------------------------
void test_primitive_conversion(RecordBackend &be)
{
	printf("primitive conversion:\n");

	auto run = [&](GLenum mode, int nverts) {
		mglInit(be);
		be.reset();
		mglBeginFrame();
		glBegin(mode);
		for (int i = 0; i < nverts; i++)
			glVertex2f(float(i), 0);
		glEnd();
		mglEndFrame();
		return be.draws.empty() ? 0u : be.draws[0].count;
	};

	// Triangle-class modes all become triangle LISTS.
	check(run(GL_TRIANGLES, 6) == 6, "GL_TRIANGLES x2 -> 6 verts");
	check(run(GL_TRIANGLE_STRIP, 5) == 9, "GL_TRIANGLE_STRIP of 5 -> 3 tris = 9 verts");
	check(run(GL_TRIANGLE_FAN, 5) == 9, "GL_TRIANGLE_FAN of 5 -> 3 tris = 9 verts");
	check(run(GL_QUADS, 8) == 12, "GL_QUADS x2 -> 4 tris = 12 verts");
	check(run(GL_QUAD_STRIP, 6) == 12, "GL_QUAD_STRIP of 6 -> 4 tris = 12 verts");
	check(run(GL_POLYGON, 6) == 12, "GL_POLYGON of 6 -> 4 tris = 12 verts");

	// Line-class modes become line LISTS. The loop adds the closing edge.
	check(run(GL_LINES, 4) == 4, "GL_LINES x2 -> 4 verts");
	check(run(GL_LINE_STRIP, 4) == 6, "GL_LINE_STRIP of 4 -> 3 segments = 6 verts");
	check(run(GL_LINE_LOOP, 4) == 8, "GL_LINE_LOOP of 4 -> 4 segments = 8 verts");
	check(run(GL_POINTS, 5) == 5, "GL_POINTS -> 5 verts");

	// Degenerate input must not emit garbage.
	check(run(GL_TRIANGLES, 2) == 0, "2 verts as GL_TRIANGLES emits nothing");
	check(run(GL_LINES, 1) == 0, "1 vert as GL_LINES emits nothing");

	// Counts alone are far too weak -- they cannot tell a correct quad split
	// from a wrong one. Tag each input vertex by its x and assert the exact
	// index sequence that comes out, which pins the winding and the topology.
	// (Both matrices are identity here, so clip x == input x.)
	auto order = [&](GLenum mode, int nverts) {
		mglInit(be);
		be.reset();
		mglBeginFrame();
		glBegin(mode);
		for (int i = 0; i < nverts; i++)
			glVertex2f(float(i), 0);
		glEnd();
		mglEndFrame();
		std::vector<int> out;
		if (!be.draws.empty())
			for (uint32_t i = 0; i < be.draws[0].count; i++)
				out.push_back(int(std::lround(vert(be.draws[0], i).x)));
		return out;
	};
	auto seq = [&](GLenum mode, int n, std::vector<int> want, const char *what) {
		const std::vector<int> got = order(mode, n);
		checks++;
		if (got != want) {
			failures++;
			printf("  FAIL: %s\n         got ", what);
			for (int v : got)
				printf("%d ", v);
			printf("\n        want ");
			for (int v : want)
				printf("%d ", v);
			printf("\n");
		}
	};

	seq(GL_TRIANGLES, 6, {0, 1, 2, 3, 4, 5}, "GL_TRIANGLES order");
	// Strip: winding alternates, so the odd triangle swaps its first two.
	seq(GL_TRIANGLE_STRIP, 5, {0, 1, 2, 2, 1, 3, 2, 3, 4}, "GL_TRIANGLE_STRIP order");
	seq(GL_TRIANGLE_FAN, 5, {0, 1, 2, 0, 2, 3, 0, 3, 4}, "GL_TRIANGLE_FAN order");
	seq(GL_POLYGON, 5, {0, 1, 2, 0, 2, 3, 0, 3, 4}, "GL_POLYGON fans like a triangle fan");
	// Quads: v0,v1,v2,v3 -> (0,1,2) + (0,2,3), keeping the winding.
	seq(GL_QUADS, 8, {0, 1, 2, 0, 2, 3, 4, 5, 6, 4, 6, 7}, "GL_QUADS order");
	// Quad strip: each PAIR adds a quad (i, i+1, i+3, i+2) -- note the swap,
	// without which the quad comes out as a bow tie.
	seq(GL_QUAD_STRIP, 6, {0, 1, 3, 0, 3, 2, 2, 3, 5, 2, 5, 4}, "GL_QUAD_STRIP order");
	seq(GL_LINE_STRIP, 4, {0, 1, 1, 2, 2, 3}, "GL_LINE_STRIP order");
	seq(GL_LINE_LOOP, 4, {0, 1, 1, 2, 2, 3, 3, 0}, "GL_LINE_LOOP closes back to vertex 0");

	// TRIANGLE_STRIP must alternate winding, or culling would drop every other
	// triangle. Check the second triangle's first two indices are swapped.
	mglInit(be);
	be.reset();
	mglBeginFrame();
	glBegin(GL_TRIANGLE_STRIP);
	glVertex2f(0, 0);
	glVertex2f(1, 0);
	glVertex2f(2, 0);
	glVertex2f(3, 0);
	glEnd();
	mglEndFrame();
	if (!be.draws.empty() && be.draws[0].count == 6) {
		// Vertices were tagged by their x, which survives the transform's sign
		// but not its scale; compare relative order instead.
		const V a = vert(be.draws[0], 3), b = vert(be.draws[0], 4);
		check(a.x > b.x, "TRIANGLE_STRIP alternates winding on the 2nd triangle");
	}
}

// -----------------------------------------------------------------------------
void test_batching(RecordBackend &be)
{
	printf("batching:\n");
	mglInit(be);
	be.reset();
	mglBeginFrame();

	// Three separate glBegin/glEnd pairs with identical state must MERGE into
	// one backend draw -- this is what makes a Processing sketch fast.
	for (int i = 0; i < 3; i++) {
		glBegin(GL_TRIANGLES);
		glVertex2f(0, 0);
		glVertex2f(1, 0);
		glVertex2f(0, 1);
		glEnd();
	}
	mglEndFrame();
	check(be.draws.size() == 1, "3 same-state glBegin/glEnd pairs merge into 1 draw");
	check(!be.draws.empty() && be.draws[0].count == 9, "merged batch holds all 9 vertices");
	check(mglFrameStats().begin_end == 3, "frame stats count 3 begin/end pairs");

	// A state change must break the batch.
	be.reset();
	mglBeginFrame();
	glBegin(GL_TRIANGLES);
	glVertex2f(0, 0);
	glVertex2f(1, 0);
	glVertex2f(0, 1);
	glEnd();
	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	glBegin(GL_TRIANGLES);
	glVertex2f(0, 0);
	glVertex2f(1, 0);
	glVertex2f(0, 1);
	glEnd();
	mglEndFrame();
	check(be.draws.size() == 2, "enabling blending splits the batch");
	check(be.draws.size() == 2 && !be.draws[0].state.blend && be.draws[1].state.blend,
		  "the split lands blending on the second batch only");

	// Different primitive CLASSES cannot merge...
	be.reset();
	mglBeginFrame();
	glDisable(GL_BLEND);
	glBegin(GL_TRIANGLES);
	glVertex2f(0, 0);
	glVertex2f(1, 0);
	glVertex2f(0, 1);
	glEnd();
	glBegin(GL_LINES);
	glVertex2f(0, 0);
	glVertex2f(1, 1);
	glEnd();
	mglEndFrame();
	check(be.draws.size() == 2, "triangles and lines do not merge");

	// ...but different modes of the SAME class do, because both convert to
	// triangle lists. This is the payoff of converting on the CPU.
	be.reset();
	mglBeginFrame();
	glBegin(GL_TRIANGLES);
	glVertex2f(0, 0);
	glVertex2f(1, 0);
	glVertex2f(0, 1);
	glEnd();
	glBegin(GL_QUADS);
	glVertex2f(0, 0);
	glVertex2f(1, 0);
	glVertex2f(1, 1);
	glVertex2f(0, 1);
	glEnd();
	mglEndFrame();
	check(be.draws.size() == 1, "GL_TRIANGLES and GL_QUADS merge into one triangle batch");
	check(!be.draws.empty() && be.draws[0].count == 9, "merged tri+quad batch has 3+6 = 9 verts");

	// A clear must flush what is already queued, or it would erase it.
	be.reset();
	mglBeginFrame();
	glBegin(GL_TRIANGLES);
	glVertex2f(0, 0);
	glVertex2f(1, 0);
	glVertex2f(0, 1);
	glEnd();
	glClear(GL_COLOR_BUFFER_BIT);
	mglEndFrame();
	check(be.draws.size() == 1 && be.clears.size() == 1, "glClear flushes the pending batch first");

	// A frame with more same-state geometry than one batch buffer holds (4096
	// verts) must SPLIT into multiple draws, not drop geometry or flag an
	// error. 60 fans of 40 verts convert to 60 * 114 = 6840 triangle verts.
	be.reset();
	mglBeginFrame();
	for (int f = 0; f < 60; f++) {
		glBegin(GL_TRIANGLE_FAN);
		for (int i = 0; i < 40; i++)
			glVertex2f(float(i), float(f));
		glEnd();
	}
	mglEndFrame();
	uint32_t total = 0;
	for (const auto &d : be.draws)
		total += d.count;
	check(be.draws.size() == 2, "an over-full frame splits into 2 batches");
	check(total == 60 * 114, "the split keeps every vertex (60 fans x 114)");
	check(glGetError() == GL_NO_ERROR, "the split raises no GL error");
}

// -----------------------------------------------------------------------------
void test_color_and_state(RecordBackend &be)
{
	printf("colour and state plumbing:\n");
	mglInit(be);
	be.reset();
	mglBeginFrame();

	// glColor is per-vertex state captured at glVertex time.
	glBegin(GL_TRIANGLES);
	glColor4f(1, 0, 0, 1);
	glVertex2f(0, 0);
	glColor4f(0, 1, 0, 0.5f);
	glVertex2f(1, 0);
	glColor4f(0, 0, 1, 1);
	glVertex2f(0, 1);
	glEnd();
	mglEndFrame();

	if (!be.draws.empty() && be.draws[0].count == 3) {
		close_to(vert(be.draws[0], 0).r, 1.0f, "vertex 0 keeps red");
		close_to(vert(be.draws[0], 1).g, 1.0f, "vertex 1 keeps green");
		close_to(vert(be.draws[0], 1).a, 0.5f, "vertex 1 keeps its alpha");
		close_to(vert(be.draws[0], 2).b, 1.0f, "vertex 2 keeps blue");
	}

	// The exact call from the original question must reach the backend intact.
	be.reset();
	mglInit(be);
	mglBeginFrame();
	glColor4f(0.2f, 0.4f, 0.6f, 0.8f);
	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	glBegin(GL_TRIANGLES);
	glVertex2f(0, 0);
	glVertex2f(1, 0);
	glVertex2f(0, 1);
	glEnd();
	mglEndFrame();
	check(be.draws.size() == 1, "the motivating snippet emits one batch");
	if (!be.draws.empty()) {
		const auto &s = be.draws[0].state;
		check(s.blend, "GL_BLEND reached the backend");
		check(s.blend_src == GL_SRC_ALPHA && s.blend_dst == GL_ONE_MINUS_SRC_ALPHA,
			  "blend factors reached the backend");
		close_to(vert(be.draws[0], 0).a, 0.8f, "glColor4f alpha reached the vertex");
	}

	// Enable/disable round-trips through glIsEnabled.
	glEnable(GL_DEPTH_TEST);
	check(glIsEnabled(GL_DEPTH_TEST) == GL_TRUE, "glIsEnabled sees GL_DEPTH_TEST");
	glDisable(GL_DEPTH_TEST);
	check(glIsEnabled(GL_DEPTH_TEST) == GL_FALSE, "glIsEnabled sees the disable");

	// Viewport read-back, which Processing does.
    GLint vp[4] = {0, 0, 0, 0};
	glGetIntegerv(GL_VIEWPORT, vp);
	check(vp[2] == 200 && vp[3] == 100, "glGetIntegerv(GL_VIEWPORT) returns the target size");

	// An unknown enum is an error, not a crash.
	glGetError();
	glEnable(0xDEAD);
	check(glGetError() == GL_INVALID_ENUM, "an unknown cap sets GL_INVALID_ENUM");
}

// -----------------------------------------------------------------------------
void test_lighting(RecordBackend &be)
{
	printf("fixed-function lighting:\n");
	mglInit(be);
	be.reset();

	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();
	glEnable(GL_LIGHTING);
	glEnable(GL_LIGHT0);
	glEnable(GL_COLOR_MATERIAL);

	// A directional light straight down +Z, white diffuse, no ambient.
	const GLfloat pos[4] = {0, 0, 1, 0};
	const GLfloat white[4] = {1, 1, 1, 1};
	const GLfloat black[4] = {0, 0, 0, 1};
	glLightfv(GL_LIGHT0, GL_POSITION, pos);
	glLightfv(GL_LIGHT0, GL_DIFFUSE, white);
	glLightfv(GL_LIGHT0, GL_AMBIENT, black);
	glLightModelfv(GL_LIGHT_MODEL_AMBIENT, black);

	mglBeginFrame();
	glColor4f(1, 1, 1, 1);
	glBegin(GL_TRIANGLES);
	glNormal3f(0, 0, 1); // facing the light
	glVertex3f(0, 0, 0);
	glNormal3f(0, 0, -1); // facing away
	glVertex3f(1, 0, 0);
	glNormal3f(1, 0, 0); // edge-on
	glVertex3f(0, 1, 0);
	glEnd();
	mglEndFrame();

	if (!be.draws.empty() && be.draws[0].count == 3) {
		close_to(vert(be.draws[0], 0).r, 1.0f, "normal toward the light is fully lit");
		close_to(vert(be.draws[0], 1).r, 0.0f, "normal away from the light is unlit");
		close_to(vert(be.draws[0], 2).r, 0.0f, "edge-on normal is unlit (N.L = 0)");
		close_to(vert(be.draws[0], 0).a, 1.0f, "lighting leaves alpha alone");
	}

	// Lighting off -> the raw current colour passes through untouched.
	be.reset();
	glDisable(GL_LIGHTING);
	mglBeginFrame();
	glColor4f(0.25f, 0.5f, 0.75f, 1);
	glBegin(GL_TRIANGLES);
	glNormal3f(0, 0, -1);
	glVertex2f(0, 0);
	glVertex2f(1, 0);
	glVertex2f(0, 1);
	glEnd();
	mglEndFrame();
	if (!be.draws.empty())
		close_to(vert(be.draws[0], 0).g, 0.5f, "with lighting off the colour is untouched");

	// A light position is captured in EYE space, i.e. transformed by the
	// modelview in force when glLightfv is called -- a classic GL gotcha.
	mglInit(be);
	be.reset();
	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();
	glTranslatef(0, 0, 10);
	const GLfloat lp[4] = {0, 0, 0, 1}; // positional light at the local origin
	glLightfv(GL_LIGHT0, GL_POSITION, lp);
	glEnable(GL_LIGHTING);
	glEnable(GL_LIGHT0);
	glEnable(GL_COLOR_MATERIAL);
	glLightfv(GL_LIGHT0, GL_DIFFUSE, white);
	glLightfv(GL_LIGHT0, GL_AMBIENT, black);
	glLightModelfv(GL_LIGHT_MODEL_AMBIENT, black);
	glLoadIdentity(); // move the modelview AFTER specifying the light
	mglBeginFrame();
	glColor4f(1, 1, 1, 1);
	glBegin(GL_TRIANGLES);
	glNormal3f(0, 0, 1);
	glVertex3f(0, 0, 0); // faces +Z, light is at eye-space (0,0,10)
	glVertex3f(1, 0, 0);
	glVertex3f(0, 1, 0);
	glEnd();
	mglEndFrame();
	if (!be.draws.empty())
		close_to(vert(be.draws[0], 0).r, 1.0f,
				 "light position was captured in eye space at glLightfv time");
}

// -----------------------------------------------------------------------------
void test_unimplemented(RecordBackend &be)
{
	printf("unimplemented surface:\n");
	mglInit(be);
	// Texture coordinates are accepted and ignored, so untextured geometry from
	// a texturing sketch still draws rather than disappearing.
	glGetError();
	be.reset();
	mglBeginFrame();
	glBegin(GL_TRIANGLES);
	glTexCoord2f(0, 0);
	glVertex2f(0, 0);
	glTexCoord2f(1, 0);
	glVertex2f(1, 0);
	glTexCoord2f(0, 1);
	glVertex2f(0, 1);
	glEnd();
	mglEndFrame();
	check(be.draws.size() == 1 && be.draws[0].count == 3,
		  "a texcoord-bearing triangle still draws its geometry");
	check(glGetError() == GL_NO_ERROR, "glTexCoord2f does not raise an error");
}

} // namespace

int main()
{
	RecordBackend be;
	printf("mini-GL host tests\n==================\n\n");
	test_matrix_stack(be);
	test_y_flip(be);
	test_primitive_conversion(be);
	test_batching(be);
	test_color_and_state(be);
	test_lighting(be);
	test_unimplemented(be);

	printf("\n%d checks, %d failures\n", checks, failures);
	if (failures == 0)
		printf("ALL MINI-GL HOST TESTS PASS\n");
	return failures != 0;
}
