#pragma once
#include "mat4.hh"
#include "mini_gl_backend.hh"
#include <cstdint>

// =============================================================================
//  mini_gl.hh -- a fixed-function OpenGL 1.x subset, on the etna 3D pipe
// =============================================================================
//
// SCOPE. This is not "OpenGL". It is exactly the entry points processing.cpp
// calls, which were extracted from its source rather than guessed -- a closed
// set of ~90 functions and ~60 enums. Everything here is fixed-function GL 1.x:
// no GLSL, so no shader compiler is required (see the README).
//
// HOW IT WORKS
// All fixed-function work happens on the CPU: the matrix stack, the vertex
// transform, per-vertex lighting, primitive conversion, and batching. The
// backend receives only already-transformed vertices plus the state to draw
// them under (mini_gl_backend.hh).
//
// Transforming on the CPU is a deliberate performance choice, not a shortcut.
// Feeding the GPU an MVP as a uniform would make the uniform bank change on
// every draw, and on HALTI5 writing shader state costs an FE->PE pipeline stall
// each time. With the CPU doing the transform, the shader state never changes,
// so a whole frame of geometry batches into one stall-free submission. It is
// also what fixed-function GL does conceptually -- there is no vertex shader.
//
// NOT IMPLEMENTED YET (deliberately; these come next):
//   textures      -- glBindTexture/glTexImage2D/glTexCoord2f/glTexParameteri
//   framebuffers  -- the FBO and renderbuffer calls
//   shaders       -- glCreateShader and friends (PShader); needs an offline
//                    GLSL compiler, see the README
//   pixel ops     -- glReadPixels/glDrawPixels
//   stencil       -- Processing's clip() is rectangular, so glScissor covers it
// Calling an unimplemented entry point records GL_INVALID_OPERATION rather than
// failing silently, so a sketch that needs one says so.

namespace mgl
{

// --- types -------------------------------------------------------------------
using GLenum = uint32_t;
using GLbitfield = uint32_t;
using GLint = int32_t;
using GLuint = uint32_t;
using GLsizei = int32_t;
using GLfloat = float;
using GLdouble = double;
using GLboolean = unsigned char;
using GLclampf = float;

// --- enums (standard OpenGL values, so app code compiles unchanged) ----------
inline constexpr GLenum GL_FALSE = 0;
inline constexpr GLenum GL_TRUE = 1;

// primitives
inline constexpr GLenum GL_POINTS = 0x0000;
inline constexpr GLenum GL_LINES = 0x0001;
inline constexpr GLenum GL_LINE_LOOP = 0x0002;
inline constexpr GLenum GL_LINE_STRIP = 0x0003;
inline constexpr GLenum GL_TRIANGLES = 0x0004;
inline constexpr GLenum GL_TRIANGLE_STRIP = 0x0005;
inline constexpr GLenum GL_TRIANGLE_FAN = 0x0006;
inline constexpr GLenum GL_QUADS = 0x0007;
inline constexpr GLenum GL_QUAD_STRIP = 0x0008;
inline constexpr GLenum GL_POLYGON = 0x0009;

// matrix modes
inline constexpr GLenum GL_MODELVIEW = 0x1700;
inline constexpr GLenum GL_PROJECTION = 0x1701;
inline constexpr GLenum GL_TEXTURE = 0x1702;

// enable/disable caps
inline constexpr GLenum GL_DEPTH_TEST = 0x0B71;
inline constexpr GLenum GL_BLEND = 0x0BE2;
inline constexpr GLenum GL_CULL_FACE = 0x0B44;
inline constexpr GLenum GL_SCISSOR_TEST = 0x0C11;
inline constexpr GLenum GL_STENCIL_TEST = 0x0B90;
inline constexpr GLenum GL_LIGHTING = 0x0B50;
inline constexpr GLenum GL_LIGHT0 = 0x4000;
inline constexpr GLenum GL_COLOR_MATERIAL = 0x0B57;
inline constexpr GLenum GL_NORMALIZE = 0x0BA1;
inline constexpr GLenum GL_TEXTURE_2D = 0x0DE1;
inline constexpr GLenum GL_MULTISAMPLE = 0x809D;
inline constexpr GLenum GL_LINE_SMOOTH = 0x0B20;
inline constexpr GLenum GL_POINT_SMOOTH = 0x0B10;
inline constexpr GLenum GL_POLYGON_OFFSET_FILL = 0x8037;

// clear bits
inline constexpr GLbitfield GL_COLOR_BUFFER_BIT = 0x00004000;
inline constexpr GLbitfield GL_DEPTH_BUFFER_BIT = 0x00000100;
inline constexpr GLbitfield GL_STENCIL_BUFFER_BIT = 0x00000400;

// blend factors
inline constexpr GLenum GL_ZERO = 0;
inline constexpr GLenum GL_ONE = 1;
inline constexpr GLenum GL_SRC_COLOR = 0x0300;
inline constexpr GLenum GL_ONE_MINUS_SRC_COLOR = 0x0301;
inline constexpr GLenum GL_SRC_ALPHA = 0x0302;
inline constexpr GLenum GL_ONE_MINUS_SRC_ALPHA = 0x0303;
inline constexpr GLenum GL_DST_ALPHA = 0x0304;
inline constexpr GLenum GL_ONE_MINUS_DST_ALPHA = 0x0305;
inline constexpr GLenum GL_DST_COLOR = 0x0306;
inline constexpr GLenum GL_ONE_MINUS_DST_COLOR = 0x0307;
inline constexpr GLenum GL_SRC_ALPHA_SATURATE = 0x0308;

// blend equations
inline constexpr GLenum GL_FUNC_ADD = 0x8006;
inline constexpr GLenum GL_FUNC_SUBTRACT = 0x800A;
inline constexpr GLenum GL_FUNC_REVERSE_SUBTRACT = 0x800B;
inline constexpr GLenum GL_MIN = 0x8007;
inline constexpr GLenum GL_MAX = 0x8008;

// compare funcs
inline constexpr GLenum GL_NEVER = 0x0200;
inline constexpr GLenum GL_LESS = 0x0201;
inline constexpr GLenum GL_EQUAL = 0x0202;
inline constexpr GLenum GL_LEQUAL = 0x0203;
inline constexpr GLenum GL_GREATER = 0x0204;
inline constexpr GLenum GL_NOTEQUAL = 0x0205;
inline constexpr GLenum GL_GEQUAL = 0x0206;
inline constexpr GLenum GL_ALWAYS = 0x0207;

// faces / winding
inline constexpr GLenum GL_FRONT = 0x0404;
inline constexpr GLenum GL_BACK = 0x0405;
inline constexpr GLenum GL_FRONT_AND_BACK = 0x0408;
inline constexpr GLenum GL_CW = 0x0900;
inline constexpr GLenum GL_CCW = 0x0901;

// shade model
inline constexpr GLenum GL_FLAT = 0x1D00;
inline constexpr GLenum GL_SMOOTH = 0x1D01;

// lighting params
inline constexpr GLenum GL_AMBIENT = 0x1200;
inline constexpr GLenum GL_DIFFUSE = 0x1201;
inline constexpr GLenum GL_SPECULAR = 0x1202;
inline constexpr GLenum GL_POSITION = 0x1203;
inline constexpr GLenum GL_SHININESS = 0x1601;
inline constexpr GLenum GL_AMBIENT_AND_DIFFUSE = 0x1602;
inline constexpr GLenum GL_LIGHT_MODEL_AMBIENT = 0x0B53;
inline constexpr GLenum GL_LIGHT_MODEL_TWO_SIDE = 0x0B52;
inline constexpr GLenum GL_CONSTANT_ATTENUATION = 0x1207;
inline constexpr GLenum GL_LINEAR_ATTENUATION = 0x1208;
inline constexpr GLenum GL_QUADRATIC_ATTENUATION = 0x1209;

// gets
inline constexpr GLenum GL_MODELVIEW_MATRIX = 0x0BA6;
inline constexpr GLenum GL_PROJECTION_MATRIX = 0x0BA7;
inline constexpr GLenum GL_VIEWPORT = 0x0BA2;
inline constexpr GLenum GL_CURRENT_COLOR = 0x0B00;
inline constexpr GLenum GL_MAX_TEXTURE_SIZE = 0x0D33;

// errors
inline constexpr GLenum GL_NO_ERROR = 0;
inline constexpr GLenum GL_INVALID_ENUM = 0x0500;
inline constexpr GLenum GL_INVALID_VALUE = 0x0501;
inline constexpr GLenum GL_INVALID_OPERATION = 0x0502;

// =============================================================================
//  lifecycle (not GL -- how the app binds mini-GL to a backend)
// =============================================================================
void mglInit(Backend &backend);
void mglBeginFrame();
void mglEndFrame(); // flushes any open batch and ends the frame
Backend *mglBackend();

// Diagnostics: how the current frame batched.
struct FrameStats {
	uint32_t batches = 0;	   // draws handed to the backend
	uint32_t vertices = 0;	   // vertices across all batches
	uint32_t begin_end = 0;	   // glBegin/glEnd pairs
	uint32_t state_flushes = 0; // batches broken by a state change
};
FrameStats mglFrameStats();

// =============================================================================
//  the GL entry points
// =============================================================================

// --- matrix stack ---
void glMatrixMode(GLenum mode);
void glLoadIdentity();
void glPushMatrix();
void glPopMatrix();
void glMultMatrixf(const GLfloat *m);
void glMultMatrixd(const GLdouble *m);
void glLoadMatrixf(const GLfloat *m);
void glTranslatef(GLfloat x, GLfloat y, GLfloat z);
void glTranslated(GLdouble x, GLdouble y, GLdouble z);
void glRotatef(GLfloat angle, GLfloat x, GLfloat y, GLfloat z);
void glScalef(GLfloat x, GLfloat y, GLfloat z);
void glOrtho(GLdouble l, GLdouble r, GLdouble b, GLdouble t, GLdouble n, GLdouble f);
void glFrustum(GLdouble l, GLdouble r, GLdouble b, GLdouble t, GLdouble n, GLdouble f);

// --- immediate mode ---
void glBegin(GLenum mode);
void glEnd();
void glVertex2f(GLfloat x, GLfloat y);
void glVertex3f(GLfloat x, GLfloat y, GLfloat z);
void glColor3f(GLfloat r, GLfloat g, GLfloat b);
void glColor4f(GLfloat r, GLfloat g, GLfloat b, GLfloat a);
void glNormal3f(GLfloat x, GLfloat y, GLfloat z);
void glTexCoord2f(GLfloat s, GLfloat t);

// --- state ---
void glEnable(GLenum cap);
void glDisable(GLenum cap);
GLboolean glIsEnabled(GLenum cap);
void glBlendFunc(GLenum src, GLenum dst);
void glBlendEquation(GLenum mode);
void glDepthFunc(GLenum func);
void glDepthMask(GLboolean flag);
void glCullFace(GLenum mode);
void glFrontFace(GLenum mode);
void glShadeModel(GLenum mode);
void glLineWidth(GLfloat w);
void glPointSize(GLfloat s);
void glViewport(GLint x, GLint y, GLsizei w, GLsizei h);
void glScissor(GLint x, GLint y, GLsizei w, GLsizei h);
void glColorMask(GLboolean r, GLboolean g, GLboolean b, GLboolean a);
void glPolygonOffset(GLfloat factor, GLfloat units);
void glHint(GLenum target, GLenum mode);
void glPixelStorei(GLenum pname, GLint param);

// --- clear ---
void glClear(GLbitfield mask);
void glClearColor(GLclampf r, GLclampf g, GLclampf b, GLclampf a);
void glClearStencil(GLint s);

// --- lighting (evaluated per-vertex on the CPU, as fixed-function GL does) ---
void glLightfv(GLenum light, GLenum pname, const GLfloat *params);
void glLightf(GLenum light, GLenum pname, GLfloat param);
void glLightModelfv(GLenum pname, const GLfloat *params);
void glLightModeli(GLenum pname, GLint param);
void glMaterialfv(GLenum face, GLenum pname, const GLfloat *params);
void glMaterialf(GLenum face, GLenum pname, GLfloat param);
void glColorMaterial(GLenum face, GLenum mode);

// --- queries ---
void glGetFloatv(GLenum pname, GLfloat *params);
void glGetDoublev(GLenum pname, GLdouble *params);
void glGetIntegerv(GLenum pname, GLint *params);
void glGetBooleanv(GLenum pname, GLboolean *params);
GLenum glGetError();

// --- flush ---
void glFlush();
void glFinish();

} // namespace mgl
