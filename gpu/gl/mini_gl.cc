#include "mini_gl.hh"
#include "mgl_math.hh"
#include <algorithm>
#include <array>

// =============================================================================
//  mini_gl.cc -- the fixed-function state machine
// =============================================================================
//
// THE Y FLIP, which is the single most confusable thing in this file.
//
// OpenGL's window origin is BOTTOM-left: after the perspective divide, NDC
// y = -1 maps to the bottom of the viewport and y = +1 to the top.
//
// Our framebuffer's row 0 is the TOP (that is how LTDC scans it out), and the
// GPU's viewport transform maps NDC +Y to INCREASING framebuffer rows -- i.e.
// NDC +Y goes DOWN the screen. That was measured, not assumed: triangle_test
// in etna_3d_tests.cc reports "NDC +Y = increasing framebuffer rows".
//
// So the two conventions differ by a vertical flip, and mini-GL negates
// clip-space Y on the way out (see emit_vertex). Doing it here, once, at the
// very end of the transform means every GL entry point above behaves exactly as
// the spec says -- including apps that already flip via glOrtho(0,w,h,0,..),
// which Processing does. Flipping in the projection instead would double-flip
// those and look correct only by accident for one particular projection.

namespace mgl
{
namespace
{

constexpr uint32_t kMaxStack = 32;	   // GL guarantees 32 for MODELVIEW
constexpr uint32_t kMaxBatchVerts = 4096;
constexpr uint32_t kMaxLights = 8;

struct Light {
	bool enabled = false;
	std::array<float, 4> ambient{0, 0, 0, 1};
	std::array<float, 4> diffuse{1, 1, 1, 1};
	std::array<float, 4> specular{1, 1, 1, 1};
	std::array<float, 4> position{0, 0, 1, 0}; // eye space, w=0 -> directional
	float constant_att = 1, linear_att = 0, quadratic_att = 0;
};

struct Material {
	std::array<float, 4> ambient{0.2f, 0.2f, 0.2f, 1};
	std::array<float, 4> diffuse{0.8f, 0.8f, 0.8f, 1};
	std::array<float, 4> specular{0, 0, 0, 1};
	float shininess = 0;
};

struct State {
	Backend *backend = nullptr;

	// --- matrix stacks ---
	GLenum matrix_mode = GL_MODELVIEW;
	std::array<Mat4, kMaxStack> modelview{};
	std::array<Mat4, kMaxStack> projection{};
	uint32_t mv_depth = 0, proj_depth = 0;
	// Combined projection*modelview, rebuilt lazily: the unlit vertex path is
	// one matrix multiply instead of two, which matters at 500k verts/frame.
	Mat4 mvp{};
	bool mvp_dirty = true;

	// --- immediate mode ---
	bool in_begin = false;
	GLenum begin_mode = GL_TRIANGLES;
	std::array<float, 4> cur_color{1, 1, 1, 1};
	std::array<float, 3> cur_normal{0, 0, 1};
	// The two big vertex buffers live OUTSIDE this struct, at namespace scope --
	// see g_begin_buf / g_batch_buf below. State has non-zero defaults, so
	// anything inside it needs static initialisation and lands in .data;
	// keeping ~229 KB of zeroes out of the image matters on a 16 MB target.
	uint32_t begin_count = 0;

	// --- the current batch handed to the backend ---
	uint32_t batch_count = 0;
	BatchState batch_state{};
	bool batch_open = false;

	// --- pipeline state ---
	bool depth_test = false, blend = false, cull = false, scissor = false;
	bool lighting = false, color_material = false, normalize = false;
	bool depth_write = true;
	GLenum depth_func = GL_LESS;
	GLenum blend_src = GL_ONE, blend_dst = GL_ZERO, blend_eq = GL_FUNC_ADD;
	GLenum cull_face = GL_BACK, front_face = GL_CCW;
	GLenum shade_model = GL_SMOOTH;
	float line_width = 1.0f, point_size = 1.0f;
	std::array<GLint, 4> viewport{0, 0, 0, 0};
	std::array<GLint, 4> scissor_box{0, 0, 0, 0};
	std::array<float, 4> clear_color{0, 0, 0, 1};

	// --- lighting ---
	std::array<Light, kMaxLights> lights{};
	Material material{};
	std::array<float, 4> light_model_ambient{0.2f, 0.2f, 0.2f, 1};

	GLenum error = GL_NO_ERROR;
	FrameStats stats{};
};

State g;

// Zero-initialised, so these go to .bss and cost nothing in the image.
std::array<float, kMaxBatchVerts * kFloatsPerVertex> g_begin_buf;
std::array<float, kMaxBatchVerts * kFloatsPerVertex> g_batch_buf;

void set_error(GLenum e)
{
	if (g.error == GL_NO_ERROR)
		g.error = e;
}

Mat4 &current_matrix()
{
	// Every matrix mutator funnels through here for its target, so this is
	// where the cached mvp is invalidated. (Conservative: read-only callers
	// dirty it too, which only costs a rebuild.)
	g.mvp_dirty = true;
	return g.matrix_mode == GL_PROJECTION ? g.projection[g.proj_depth] : g.modelview[g.mv_depth];
}

// GL matrix commands POST-multiply: C = C * M.
void post_mult(const Mat4 &m)
{
	Mat4 &c = current_matrix();
	c = c * m;
}

// Which of the three backend primitives a glBegin mode becomes. Everything
// reduces to triangles, lines or points on the CPU, which is what lets
// consecutive glBegin/glEnd pairs merge into one draw.
Prim prim_class(GLenum mode)
{
	switch (mode) {
		case GL_POINTS:
			return Prim::Points;
		case GL_LINES:
		case GL_LINE_STRIP:
		case GL_LINE_LOOP:
			return Prim::Lines;
		default:
			return Prim::Triangles;
	}
}

BatchState current_batch_state(GLenum mode)
{
	BatchState s;
	s.prim = prim_class(mode);
	s.blend = g.blend;
	s.blend_src = g.blend_src;
	s.blend_dst = g.blend_dst;
	s.blend_eq = g.blend_eq;
	s.depth_test = g.depth_test;
	s.depth_write = g.depth_write;
	s.depth_func = g.depth_func;
	s.cull = g.cull;
	s.cull_face = g.cull_face;
	s.front_face = g.front_face;
	s.scissor = g.scissor;
	s.scissor_x = g.scissor_box[0];
	s.scissor_y = g.scissor_box[1];
	s.scissor_w = static_cast<uint32_t>(std::max(0, g.scissor_box[2]));
	s.scissor_h = static_cast<uint32_t>(std::max(0, g.scissor_box[3]));
	s.line_width = g.line_width;
	s.point_size = g.point_size;
	return s;
}

void flush_batch()
{
	if (!g.batch_open || g.batch_count == 0) {
		g.batch_open = false;
		g.batch_count = 0;
		return;
	}
	if (g.backend) {
		g.backend->draw(g.batch_state, std::span<const float>(g_batch_buf.data(), g.batch_count * kFloatsPerVertex),
						g.batch_count);
		g.stats.batches++;
		g.stats.vertices += g.batch_count;
	}
	g.batch_count = 0;
	g.batch_open = false;
}

// Open or continue a batch for `state`; flushes first if the state differs.
void want_batch(const BatchState &state)
{
	if (g.batch_open && !(g.batch_state == state)) {
		flush_batch();
		g.stats.state_flushes++;
	}
	if (!g.batch_open) {
		g.batch_state = state;
		g.batch_open = true;
	}
}

void push_vertex(const float *v)
{
	if (g.batch_count >= kMaxBatchVerts) {
		// Splitting mid-primitive would tear geometry, so the caller (glEnd)
		// only ever adds whole primitives; here we just refuse and flag.
		set_error(GL_INVALID_OPERATION);
		return;
	}
	std::copy_n(v, kFloatsPerVertex, g_batch_buf.begin() + g.batch_count * kFloatsPerVertex);
	g.batch_count++;
}

// --- lighting ----------------------------------------------------------------
// Fixed-function GL evaluates lighting per VERTEX, at glVertex time, producing
// a colour that the rasteriser then interpolates. Doing the same on the CPU
// means the GPU never needs a lighting shader, and the result reuses the
// existing pass-through colour shader.
std::array<float, 4> lit_color(const Vec4 &eye_pos, const Vec4 &eye_normal)
{
	const Material &m = g.material;
	// With GL_COLOR_MATERIAL the current colour replaces ambient+diffuse,
	// which is the mode Processing uses (it calls glColorMaterial with
	// GL_AMBIENT_AND_DIFFUSE).
	const std::array<float, 4> base = g.color_material ? g.cur_color : m.diffuse;
	const std::array<float, 4> amb = g.color_material ? g.cur_color : m.ambient;

	float nx = eye_normal.x, ny = eye_normal.y, nz = eye_normal.z;
	const float nlen = m_sqrt(nx * nx + ny * ny + nz * nz);
	if (nlen > 0) {
		nx /= nlen;
		ny /= nlen;
		nz /= nlen;
	}

	std::array<float, 4> out{
		g.light_model_ambient[0] * amb[0],
		g.light_model_ambient[1] * amb[1],
		g.light_model_ambient[2] * amb[2],
		base[3], // alpha comes from the material/current colour, never the lights
	};

	for (const Light &l : g.lights) {
		if (!l.enabled)
			continue;

		// w = 0 -> directional (position IS the direction); otherwise positional.
		float lx = l.position[0], ly = l.position[1], lz = l.position[2];
		float atten = 1.0f;
		if (l.position[3] != 0.0f) {
			lx -= eye_pos.x;
			ly -= eye_pos.y;
			lz -= eye_pos.z;
			const float d = m_sqrt(lx * lx + ly * ly + lz * lz);
			if (d > 0) {
				lx /= d;
				ly /= d;
				lz /= d;
			}
			const float denom = l.constant_att + l.linear_att * d + l.quadratic_att * d * d;
			atten = denom > 0 ? 1.0f / denom : 1.0f;
		} else {
			const float d = m_sqrt(lx * lx + ly * ly + lz * lz);
			if (d > 0) {
				lx /= d;
				ly /= d;
				lz /= d;
			}
		}

		const float ndotl = std::max(0.0f, nx * lx + ny * ly + nz * lz);
		for (int i = 0; i < 3; i++)
			out[i] += atten * (l.ambient[i] * amb[i] + l.diffuse[i] * base[i] * ndotl);

		if (m.shininess > 0 && ndotl > 0) {
			// Eye is at the origin in eye space, so the view direction is -pos.
			float ex = -eye_pos.x, ey = -eye_pos.y, ez = -eye_pos.z;
			const float elen = m_sqrt(ex * ex + ey * ey + ez * ez);
			if (elen > 0) {
				ex /= elen;
				ey /= elen;
				ez /= elen;
			}
			float hx = lx + ex, hy = ly + ey, hz = lz + ez;
			const float hlen = m_sqrt(hx * hx + hy * hy + hz * hz);
			if (hlen > 0) {
				hx /= hlen;
				hy /= hlen;
				hz /= hlen;
			}
			const float ndoth = std::max(0.0f, nx * hx + ny * hy + nz * hz);
			const float sp = m_pow(ndoth, m.shininess);
			for (int i = 0; i < 3; i++)
				out[i] += atten * l.specular[i] * m.specular[i] * sp;
		}
	}

	for (int i = 0; i < 3; i++)
		out[i] = std::clamp(out[i], 0.0f, 1.0f);
	return out;
}

// Transform one app-space vertex into the clip-space form the backend wants.
void emit_vertex(float x, float y, float z)
{
	if (g.begin_count >= kMaxBatchVerts) {
		set_error(GL_INVALID_OPERATION);
		return;
	}

	const Mat4 &mv = g.modelview[g.mv_depth];
	const Mat4 &pr = g.projection[g.proj_depth];

	std::array<float, 4> color = g.cur_color;
	Vec4 clip;
	if (g.lighting) {
		// Lighting needs the eye-space position anyway, so transform in two
		// steps as before.
		const Vec4 eye = mv * Vec4{x, y, z, 1.0f};
		clip = pr * eye;
		Vec4 n = mv.transform_dir(g.cur_normal[0], g.cur_normal[1], g.cur_normal[2]);
		color = lit_color(eye, n);
	} else {
		// Hot path: one cached projection*modelview multiply per vertex.
		if (g.mvp_dirty) {
			g.mvp = pr * mv;
			g.mvp_dirty = false;
		}
		clip = g.mvp * Vec4{x, y, z, 1.0f};
	}

	// THE Y FLIP -- see the file header. GL's viewport origin is bottom-left;
	// ours is top-left, so clip-space Y is negated exactly once, here.
	clip.y = -clip.y;

	float *v = g_begin_buf.data() + g.begin_count * kFloatsPerVertex;
	v[0] = clip.x;
	v[1] = clip.y;
	v[2] = clip.z;
	v[3] = color[0];
	v[4] = color[1];
	v[5] = color[2];
	v[6] = color[3];
	g.begin_count++;
}

const float *vtx(uint32_t i)
{
	return g_begin_buf.data() + i * kFloatsPerVertex;
}

// Convert what glBegin accumulated into the batch's primitive class.
void convert_and_append(GLenum mode, uint32_t n)
{
	auto tri = [](uint32_t a, uint32_t b, uint32_t c) {
		push_vertex(vtx(a));
		push_vertex(vtx(b));
		push_vertex(vtx(c));
	};
	auto line = [](uint32_t a, uint32_t b) {
		push_vertex(vtx(a));
		push_vertex(vtx(b));
	};

	switch (mode) {
		case GL_POINTS:
			for (uint32_t i = 0; i < n; i++)
				push_vertex(vtx(i));
			break;

		case GL_LINES:
			for (uint32_t i = 0; i + 1 < n; i += 2)
				line(i, i + 1);
			break;
		case GL_LINE_STRIP:
			for (uint32_t i = 0; i + 1 < n; i++)
				line(i, i + 1);
			break;
		case GL_LINE_LOOP:
			for (uint32_t i = 0; i + 1 < n; i++)
				line(i, i + 1);
			if (n > 2)
				line(n - 1, 0); // the closing edge
			break;

		case GL_TRIANGLES:
			for (uint32_t i = 0; i + 2 < n; i += 3)
				tri(i, i + 1, i + 2);
			break;
		case GL_TRIANGLE_STRIP:
			// Winding alternates so every triangle faces the same way; culling
			// would reject every other one otherwise.
			for (uint32_t i = 0; i + 2 < n; i++) {
				if (i & 1)
					tri(i + 1, i, i + 2);
				else
					tri(i, i + 1, i + 2);
			}
			break;
		case GL_TRIANGLE_FAN:
		case GL_POLYGON: // convex polygons fan correctly, which is all GL promises
			for (uint32_t i = 1; i + 1 < n; i++)
				tri(0, i, i + 1);
			break;

		case GL_QUADS:
			// The hardware has a QUADS primitive but Mesa never advertises it,
			// so it is unproven -- split on the CPU instead (etna_prim.hh).
			for (uint32_t i = 0; i + 3 < n; i += 4) {
				tri(i, i + 1, i + 2);
				tri(i, i + 2, i + 3);
			}
			break;
		case GL_QUAD_STRIP:
			// Each additional PAIR of vertices adds a quad: (i, i+1, i+3, i+2).
			for (uint32_t i = 0; i + 3 < n; i += 2) {
				tri(i, i + 1, i + 3);
				tri(i, i + 3, i + 2);
			}
			break;

		default:
			set_error(GL_INVALID_ENUM);
			break;
	}
}

} // namespace

// =============================================================================
//  lifecycle
// =============================================================================
void mglInit(Backend &backend)
{
	g = State{};
	g.backend = &backend;
	g.modelview[0] = Mat4::identity();
	g.projection[0] = Mat4::identity();
	g.viewport = {0, 0, static_cast<GLint>(backend.width()), static_cast<GLint>(backend.height())};
	g.scissor_box = g.viewport;
}

void mglBeginFrame()
{
	g.stats = FrameStats{};
	g.batch_count = 0;
	g.batch_open = false;
	if (g.backend)
		g.backend->begin_frame();
}

void mglEndFrame()
{
	flush_batch();
	if (g.backend)
		g.backend->end_frame();
}

Backend *mglBackend()
{
	return g.backend;
}

FrameStats mglFrameStats()
{
	return g.stats;
}

// =============================================================================
//  matrix stack
// =============================================================================
void glMatrixMode(GLenum mode)
{
	if (mode != GL_MODELVIEW && mode != GL_PROJECTION && mode != GL_TEXTURE) {
		set_error(GL_INVALID_ENUM);
		return;
	}
	g.matrix_mode = mode;
}

void glLoadIdentity()
{
	current_matrix() = Mat4::identity();
}

void glPushMatrix()
{
	if (g.matrix_mode == GL_PROJECTION) {
		if (g.proj_depth + 1 >= kMaxStack) {
			set_error(GL_INVALID_OPERATION);
			return;
		}
		g.projection[g.proj_depth + 1] = g.projection[g.proj_depth];
		g.proj_depth++;
	} else {
		if (g.mv_depth + 1 >= kMaxStack) {
			set_error(GL_INVALID_OPERATION);
			return;
		}
		g.modelview[g.mv_depth + 1] = g.modelview[g.mv_depth];
		g.mv_depth++;
	}
}

void glPopMatrix()
{
	if (g.matrix_mode == GL_PROJECTION) {
		if (g.proj_depth == 0) {
			set_error(GL_INVALID_OPERATION);
			return;
		}
		g.proj_depth--;
	} else {
		if (g.mv_depth == 0) {
			set_error(GL_INVALID_OPERATION);
			return;
		}
		g.mv_depth--;
	}
	g.mvp_dirty = true; // the top matrix changed without current_matrix()
}

void glLoadMatrixf(const GLfloat *m)
{
	Mat4 &c = current_matrix();
	for (int i = 0; i < 16; i++)
		c.m[i] = m[i];
}

void glMultMatrixf(const GLfloat *m)
{
	Mat4 mm{};
	for (int i = 0; i < 16; i++)
		mm.m[i] = m[i];
	post_mult(mm);
}

void glMultMatrixd(const GLdouble *m)
{
	Mat4 mm{};
	for (int i = 0; i < 16; i++)
		mm.m[i] = static_cast<float>(m[i]);
	post_mult(mm);
}

void glTranslatef(GLfloat x, GLfloat y, GLfloat z)
{
	post_mult(translation(x, y, z));
}
void glTranslated(GLdouble x, GLdouble y, GLdouble z)
{
	post_mult(translation(static_cast<float>(x), static_cast<float>(y), static_cast<float>(z)));
}
void glRotatef(GLfloat a, GLfloat x, GLfloat y, GLfloat z)
{
	post_mult(rotation(a, x, y, z));
}
void glScalef(GLfloat x, GLfloat y, GLfloat z)
{
	post_mult(scaling(x, y, z));
}
void glOrtho(GLdouble l, GLdouble r, GLdouble b, GLdouble t, GLdouble n, GLdouble f)
{
	post_mult(ortho(float(l), float(r), float(b), float(t), float(n), float(f)));
}
void glFrustum(GLdouble l, GLdouble r, GLdouble b, GLdouble t, GLdouble n, GLdouble f)
{
	post_mult(frustum(float(l), float(r), float(b), float(t), float(n), float(f)));
}

// =============================================================================
//  immediate mode
// =============================================================================
void glBegin(GLenum mode)
{
	if (g.in_begin) {
		set_error(GL_INVALID_OPERATION);
		return;
	}
	g.in_begin = true;
	g.begin_mode = mode;
	g.begin_count = 0;
}

void glEnd()
{
	if (!g.in_begin) {
		set_error(GL_INVALID_OPERATION);
		return;
	}
	g.in_begin = false;
	g.stats.begin_end++;

	const uint32_t n = g.begin_count;
	g.begin_count = 0;
	if (n == 0)
		return;

	const BatchState bs = current_batch_state(g.begin_mode);
	want_batch(bs);
	// A glBegin/glEnd block is never split across batches (that would tear a
	// primitive), so if this block's worst-case expansion (3 output verts per
	// input vert, the TRIANGLE_STRIP/FAN bound) cannot fit in what remains of
	// the open batch, hand the batch to the backend and start a fresh one.
	// Only a single block bigger than the whole buffer still overflows.
	if (g.batch_count + 3 * n > kMaxBatchVerts) {
		flush_batch();
		want_batch(bs);
	}
	convert_and_append(g.begin_mode, n);
}

void glVertex2f(GLfloat x, GLfloat y)
{
	emit_vertex(x, y, 0.0f);
}
void glVertex3f(GLfloat x, GLfloat y, GLfloat z)
{
	emit_vertex(x, y, z);
}
void glColor3f(GLfloat r, GLfloat gg, GLfloat b)
{
	g.cur_color = {r, gg, b, 1.0f};
}
void glColor4f(GLfloat r, GLfloat gg, GLfloat b, GLfloat a)
{
	g.cur_color = {r, gg, b, a};
}
void glNormal3f(GLfloat x, GLfloat y, GLfloat z)
{
	g.cur_normal = {x, y, z};
}
void glTexCoord2f(GLfloat, GLfloat)
{
	// Texturing is not implemented yet; coordinates are accepted and ignored so
	// that sketches which set them still draw their (untextured) geometry.
}

// =============================================================================
//  state
// =============================================================================
namespace
{
bool *cap_flag(GLenum cap)
{
	switch (cap) {
		case GL_DEPTH_TEST:
			return &g.depth_test;
		case GL_BLEND:
			return &g.blend;
		case GL_CULL_FACE:
			return &g.cull;
		case GL_SCISSOR_TEST:
			return &g.scissor;
		case GL_LIGHTING:
			return &g.lighting;
		case GL_COLOR_MATERIAL:
			return &g.color_material;
		case GL_NORMALIZE:
			return &g.normalize;
		default:
			return nullptr;
	}
}
} // namespace

void glEnable(GLenum cap)
{
	if (cap >= GL_LIGHT0 && cap < GL_LIGHT0 + kMaxLights) {
		g.lights[cap - GL_LIGHT0].enabled = true;
		return;
	}
	if (bool *f = cap_flag(cap)) {
		*f = true;
		return;
	}
	// Caps we knowingly ignore (they affect quality, not correctness) rather
	// than reporting as errors: multisample, smoothing hints, texturing,
	// stencil, polygon offset.
	switch (cap) {
		case GL_TEXTURE_2D:
		case GL_MULTISAMPLE:
		case GL_LINE_SMOOTH:
		case GL_POINT_SMOOTH:
		case GL_STENCIL_TEST:
		case GL_POLYGON_OFFSET_FILL:
			return;
		default:
			set_error(GL_INVALID_ENUM);
	}
}

void glDisable(GLenum cap)
{
	if (cap >= GL_LIGHT0 && cap < GL_LIGHT0 + kMaxLights) {
		g.lights[cap - GL_LIGHT0].enabled = false;
		return;
	}
	if (bool *f = cap_flag(cap)) {
		*f = false;
		return;
	}
	switch (cap) {
		case GL_TEXTURE_2D:
		case GL_MULTISAMPLE:
		case GL_LINE_SMOOTH:
		case GL_POINT_SMOOTH:
		case GL_STENCIL_TEST:
		case GL_POLYGON_OFFSET_FILL:
			return;
		default:
			set_error(GL_INVALID_ENUM);
	}
}

GLboolean glIsEnabled(GLenum cap)
{
	if (cap >= GL_LIGHT0 && cap < GL_LIGHT0 + kMaxLights)
		return g.lights[cap - GL_LIGHT0].enabled ? GL_TRUE : GL_FALSE;
	if (const bool *f = cap_flag(cap))
		return *f ? GL_TRUE : GL_FALSE;
	return GL_FALSE;
}

void glBlendFunc(GLenum src, GLenum dst)
{
	g.blend_src = src;
	g.blend_dst = dst;
}
void glBlendEquation(GLenum mode)
{
	g.blend_eq = mode;
}
void glDepthFunc(GLenum func)
{
	g.depth_func = func;
}
void glDepthMask(GLboolean flag)
{
	g.depth_write = flag != GL_FALSE;
}
void glCullFace(GLenum mode)
{
	g.cull_face = mode;
}
void glFrontFace(GLenum mode)
{
	g.front_face = mode;
}
void glShadeModel(GLenum mode)
{
	g.shade_model = mode;
}
void glLineWidth(GLfloat w)
{
	g.line_width = w;
}
void glPointSize(GLfloat s)
{
	g.point_size = s;
}
void glViewport(GLint x, GLint y, GLsizei w, GLsizei h)
{
	g.viewport = {x, y, w, h};
}
void glScissor(GLint x, GLint y, GLsizei w, GLsizei h)
{
	g.scissor_box = {x, y, w, h};
}
void glColorMask(GLboolean, GLboolean, GLboolean, GLboolean)
{
	// The PE has a colour-mask field, but nothing routes it through MeshDraw
	// yet; accepted silently so sketches that toggle it still render.
}
void glPolygonOffset(GLfloat, GLfloat)
{}
void glHint(GLenum, GLenum)
{}
void glPixelStorei(GLenum, GLint)
{}

// =============================================================================
//  clear
// =============================================================================
void glClear(GLbitfield mask)
{
	// A clear affects everything already queued, so the batch must go first.
	flush_batch();
	if (g.backend)
		g.backend->clear(mask, g.clear_color[0], g.clear_color[1], g.clear_color[2], g.clear_color[3], 1.0f);
}

void glClearColor(GLclampf r, GLclampf gg, GLclampf b, GLclampf a)
{
	g.clear_color = {r, gg, b, a};
}
void glClearStencil(GLint)
{}

// =============================================================================
//  lighting
// =============================================================================
namespace
{
Light *light_at(GLenum light)
{
	if (light < GL_LIGHT0 || light >= GL_LIGHT0 + kMaxLights) {
		set_error(GL_INVALID_ENUM);
		return nullptr;
	}
	return &g.lights[light - GL_LIGHT0];
}
} // namespace

void glLightfv(GLenum light, GLenum pname, const GLfloat *params)
{
	Light *l = light_at(light);
	if (!l)
		return;
	switch (pname) {
		case GL_AMBIENT:
			l->ambient = {params[0], params[1], params[2], params[3]};
			break;
		case GL_DIFFUSE:
			l->diffuse = {params[0], params[1], params[2], params[3]};
			break;
		case GL_SPECULAR:
			l->specular = {params[0], params[1], params[2], params[3]};
			break;
		case GL_POSITION:
			// GL transforms the light position by the CURRENT modelview at the
			// time it is specified, giving an eye-space position.
			{
				const Mat4 &mv = g.modelview[g.mv_depth];
				const Vec4 p = mv * Vec4{params[0], params[1], params[2], params[3]};
				l->position = {p.x, p.y, p.z, params[3]};
			}
			break;
		default:
			set_error(GL_INVALID_ENUM);
	}
}

void glLightf(GLenum light, GLenum pname, GLfloat param)
{
	Light *l = light_at(light);
	if (!l)
		return;
	switch (pname) {
		case GL_CONSTANT_ATTENUATION:
			l->constant_att = param;
			break;
		case GL_LINEAR_ATTENUATION:
			l->linear_att = param;
			break;
		case GL_QUADRATIC_ATTENUATION:
			l->quadratic_att = param;
			break;
		default:
			set_error(GL_INVALID_ENUM);
	}
}

void glLightModelfv(GLenum pname, const GLfloat *params)
{
	if (pname == GL_LIGHT_MODEL_AMBIENT)
		g.light_model_ambient = {params[0], params[1], params[2], params[3]};
	else
		set_error(GL_INVALID_ENUM);
}

void glLightModeli(GLenum, GLint)
{}

void glMaterialfv(GLenum, GLenum pname, const GLfloat *params)
{
	// Front and back materials are not tracked separately; Processing sets both.
	switch (pname) {
		case GL_AMBIENT:
			g.material.ambient = {params[0], params[1], params[2], params[3]};
			break;
		case GL_DIFFUSE:
			g.material.diffuse = {params[0], params[1], params[2], params[3]};
			break;
		case GL_AMBIENT_AND_DIFFUSE:
			g.material.ambient = {params[0], params[1], params[2], params[3]};
			g.material.diffuse = g.material.ambient;
			break;
		case GL_SPECULAR:
			g.material.specular = {params[0], params[1], params[2], params[3]};
			break;
		case GL_SHININESS:
			g.material.shininess = params[0];
			break;
		default:
			set_error(GL_INVALID_ENUM);
	}
}

void glMaterialf(GLenum, GLenum pname, GLfloat param)
{
	if (pname == GL_SHININESS)
		g.material.shininess = param;
	else
		set_error(GL_INVALID_ENUM);
}

void glColorMaterial(GLenum, GLenum)
{
	// Only GL_AMBIENT_AND_DIFFUSE is meaningful here, and it is what Processing
	// uses; the mode is implied by GL_COLOR_MATERIAL being enabled.
}

// =============================================================================
//  queries
// =============================================================================
void glGetFloatv(GLenum pname, GLfloat *params)
{
	switch (pname) {
		case GL_MODELVIEW_MATRIX:
			std::copy_n(g.modelview[g.mv_depth].m.begin(), 16, params);
			break;
		case GL_PROJECTION_MATRIX:
			std::copy_n(g.projection[g.proj_depth].m.begin(), 16, params);
			break;
		case GL_CURRENT_COLOR:
			std::copy_n(g.cur_color.begin(), 4, params);
			break;
		default:
			set_error(GL_INVALID_ENUM);
	}
}

void glGetDoublev(GLenum pname, GLdouble *params)
{
	std::array<GLfloat, 16> tmp{};
	const GLenum saved = g.error;
	glGetFloatv(pname, tmp.data());
	if (g.error != saved)
		return;
	const int n = (pname == GL_CURRENT_COLOR) ? 4 : 16;
	for (int i = 0; i < n; i++)
		params[i] = tmp[i];
}

void glGetIntegerv(GLenum pname, GLint *params)
{
	switch (pname) {
		case GL_VIEWPORT:
			std::copy_n(g.viewport.begin(), 4, params);
			break;
		case GL_MAX_TEXTURE_SIZE:
			params[0] = 2048;
			break;
		default:
			set_error(GL_INVALID_ENUM);
	}
}

void glGetBooleanv(GLenum pname, GLboolean *params)
{
	params[0] = glIsEnabled(pname);
}

GLenum glGetError()
{
	const GLenum e = g.error;
	g.error = GL_NO_ERROR;
	return e;
}

void glFlush()
{
	flush_batch();
}
void glFinish()
{
	flush_batch();
}

} // namespace mgl
