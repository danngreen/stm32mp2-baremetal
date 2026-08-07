#include "psketch.hh"
#include "gl/mini_gl.hh"
#include <cstdint>
#include <vector>

// =============================================================================
//  psketch.cc -- Processing API -> mini-GL calls
// =============================================================================
//
// This is (a small slice of) what processing.cpp's 2D renderer does: shapes
// tessellate on the CPU into glBegin/glEnd blocks, transforms go straight to
// the GL matrix stack, and fill/stroke are two colors applied around each
// shape. mini-GL then batches every same-state block of a frame into one GPU
// submission, so hundreds of these shapes still cost one stall-free draw.

using namespace mgl;

int width = 0, height = 0, frameCount = 0;
int mouseX = 0, mouseY = 0;

namespace
{

bool fill_on = true, stroke_on = true;
float fill_c[4] = {1, 1, 1, 1};
float stroke_c[4] = {0, 0, 0, 1};
float stroke_wt = 1.0f;
int color_mode = RGB;
float cmax[4] = {255, 255, 255, 255}; // per-channel ranges from colorMode()
int ellipse_mode = CENTER;
int rect_mode = CORNER;
unsigned frame_period_us = 0; // 0 = every vblank
uint32_t rng = 0x243F6A88;	  // pi, why not

// beginShape/vertex accumulate here; endShape draws it. A vector, because
// this project has a real heap and shapes have no natural size limit.
int shape_kind = POLYGON;
std::vector<float> shape_pts; // x,y pairs

void gl_color(const float c[4])
{
	glColor4f(c[0], c[1], c[2], c[3]);
}

// Resolve channel values through the current colorMode into linear 0..1 RGBA.
// HSB conversion is the standard 6-sector one (hue wraps, as in Processing).
void to_rgba(float a, float b, float c, float alpha, float out[4])
{
	float v1 = a / cmax[0], v2 = b / cmax[1], v3 = c / cmax[2];
	const float va = alpha / cmax[3];
	v2 = v2 < 0 ? 0 : (v2 > 1 ? 1 : v2);
	v3 = v3 < 0 ? 0 : (v3 > 1 ? 1 : v3);
	if (color_mode == HSB) {
		float h = v1 - int(v1); // wrap hue into [0,1)
		if (h < 0)
			h += 1.0f;
		const float s = v2, v = v3;
		const float h6 = h * 6.0f;
		const int sector = int(h6);
		const float f = h6 - sector;
		const float p = v * (1 - s), q = v * (1 - s * f), t = v * (1 - s * (1 - f));
		switch (sector % 6) {
			case 0: out[0] = v; out[1] = t; out[2] = p; break;
			case 1: out[0] = q; out[1] = v; out[2] = p; break;
			case 2: out[0] = p; out[1] = v; out[2] = t; break;
			case 3: out[0] = p; out[1] = q; out[2] = v; break;
			case 4: out[0] = t; out[1] = p; out[2] = v; break;
			default: out[0] = v; out[1] = p; out[2] = q; break;
		}
	} else {
		v1 = v1 < 0 ? 0 : (v1 > 1 ? 1 : v1);
		out[0] = v1;
		out[1] = v2;
		out[2] = v3;
	}
	out[3] = va < 0 ? 0 : (va > 1 ? 1 : va);
}

// A single "gray" value is brightness against the third channel's range,
// whatever the mode -- matches Processing (gray never goes through HSB).
void gray_rgba(float gray, float alpha, float out[4])
{
	float v = gray / cmax[2];
	v = v < 0 ? 0 : (v > 1 ? 1 : v);
	float va = alpha / cmax[3];
	out[0] = out[1] = out[2] = v;
	out[3] = va < 0 ? 0 : (va > 1 ? 1 : va);
}

// Segment count for an ellipse: enough that the largest radius looks round,
// bounded so a screen-full of small circles stays cheap. (Processing sizes
// its ellipse detail similarly, from the bounding size.)
int ellipse_segments(float rmax)
{
	const int n = int(rmax * 0.6f);
	return n < 12 ? 12 : (n > 64 ? 64 : n);
}

} // namespace

// --- random -------------------------------------------------------------------
float random(float hi)
{
	return random(0.0f, hi);
}

float random(float lo, float hi)
{
	rng ^= rng << 13;
	rng ^= rng >> 17;
	rng ^= rng << 5;
	return lo + (hi - lo) * (float(rng >> 8) / 16777216.0f);
}

// --- color state --------------------------------------------------------------
void colorMode(int mode, float max1, float max2, float max3, float maxA)
{
	color_mode = mode;
	cmax[0] = max1;
	cmax[1] = max2;
	cmax[2] = max3;
	cmax[3] = maxA;
}
void colorMode(int mode, float max1, float max2, float max3)
{
	colorMode(mode, max1, max2, max3, cmax[3]);
}
void colorMode(int mode, float max)
{
	colorMode(mode, max, max, max, max);
}
void colorMode(int mode)
{
	color_mode = mode;
}

void background(float r, float g, float b)
{
	float c[4];
	to_rgba(r, g, b, cmax[3], c);
	glClearColor(c[0], c[1], c[2], 1.0f);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
}
void background(float gray)
{
	float c[4];
	gray_rgba(gray, cmax[3], c);
	glClearColor(c[0], c[1], c[2], 1.0f);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
}

void fill(float r, float g, float b, float a)
{
	fill_on = true;
	to_rgba(r, g, b, a, fill_c);
}
void fill(float r, float g, float b)
{
	fill(r, g, b, cmax[3]);
}
void fill(float gray, float alpha)
{
	fill_on = true;
	gray_rgba(gray, alpha, fill_c);
}
void fill(float gray)
{
	fill(gray, cmax[3]);
}
void noFill()
{
	fill_on = false;
}

void stroke(float r, float g, float b, float a)
{
	stroke_on = true;
	to_rgba(r, g, b, a, stroke_c);
}
void stroke(float r, float g, float b)
{
	stroke(r, g, b, cmax[3]);
}
void stroke(float gray, float alpha)
{
	stroke_on = true;
	gray_rgba(gray, alpha, stroke_c);
}
void stroke(float gray)
{
	stroke(gray, cmax[3]);
}
void noStroke()
{
	stroke_on = false;
}
void strokeWeight(float w)
{
	stroke_wt = w;
}

// --- shapes -------------------------------------------------------------------
void ellipseMode(int mode)
{
	ellipse_mode = mode;
}
void rectMode(int mode)
{
	rect_mode = mode;
}

void ellipse(float a, float b, float c, float d)
{
	float cx, cy, rx, ry;
	switch (ellipse_mode) {
		case RADIUS:
			cx = a, cy = b, rx = c, ry = d;
			break;
		case CORNER:
			rx = c * 0.5f, ry = d * 0.5f, cx = a + rx, cy = b + ry;
			break;
		case CORNERS:
			cx = (a + c) * 0.5f, cy = (b + d) * 0.5f, rx = abs(c - a) * 0.5f, ry = abs(d - b) * 0.5f;
			break;
		default: // CENTER
			cx = a, cy = b, rx = c * 0.5f, ry = d * 0.5f;
			break;
	}
	const int n = ellipse_segments(rx > ry ? rx : ry);

	if (fill_on) {
		gl_color(fill_c);
		glBegin(GL_TRIANGLE_FAN);
		glVertex2f(cx, cy);
		for (int i = 0; i <= n; i++) {
			const float a = TWO_PI * float(i) / float(n);
			glVertex2f(cx + rx * cos(a), cy + ry * sin(a));
		}
		glEnd();
	}
	if (stroke_on) {
		glLineWidth(stroke_wt);
		gl_color(stroke_c);
		glBegin(GL_LINE_LOOP);
		for (int i = 0; i < n; i++) {
			const float a = TWO_PI * float(i) / float(n);
			glVertex2f(cx + rx * cos(a), cy + ry * sin(a));
		}
		glEnd();
	}
}

void rect(float a, float b, float c, float d)
{
	float x, y, w, h;
	switch (rect_mode) {
		case CENTER:
			w = c, h = d, x = a - w * 0.5f, y = b - h * 0.5f;
			break;
		case RADIUS:
			w = 2 * c, h = 2 * d, x = a - c, y = b - d;
			break;
		case CORNERS:
			x = a, y = b, w = c - a, h = d - b;
			break;
		default: // CORNER
			x = a, y = b, w = c, h = d;
			break;
	}
	if (fill_on) {
		gl_color(fill_c);
		glBegin(GL_QUADS);
		glVertex2f(x, y);
		glVertex2f(x + w, y);
		glVertex2f(x + w, y + h);
		glVertex2f(x, y + h);
		glEnd();
	}
	if (stroke_on) {
		glLineWidth(stroke_wt);
		gl_color(stroke_c);
		glBegin(GL_LINE_LOOP);
		glVertex2f(x, y);
		glVertex2f(x + w, y);
		glVertex2f(x + w, y + h);
		glVertex2f(x, y + h);
		glEnd();
	}
}

void line(float x1, float y1, float x2, float y2)
{
	if (!stroke_on)
		return;
	glLineWidth(stroke_wt);
	gl_color(stroke_c);
	glBegin(GL_LINES);
	glVertex2f(x1, y1);
	glVertex2f(x2, y2);
	glEnd();
}

void triangle(float x1, float y1, float x2, float y2, float x3, float y3)
{
	if (fill_on) {
		gl_color(fill_c);
		glBegin(GL_TRIANGLES);
		glVertex2f(x1, y1);
		glVertex2f(x2, y2);
		glVertex2f(x3, y3);
		glEnd();
	}
	if (stroke_on) {
		glLineWidth(stroke_wt);
		gl_color(stroke_c);
		glBegin(GL_LINE_LOOP);
		glVertex2f(x1, y1);
		glVertex2f(x2, y2);
		glVertex2f(x3, y3);
		glEnd();
	}
}

void quad(float x1, float y1, float x2, float y2, float x3, float y3, float x4, float y4)
{
	if (fill_on) {
		gl_color(fill_c);
		glBegin(GL_QUADS);
		glVertex2f(x1, y1);
		glVertex2f(x2, y2);
		glVertex2f(x3, y3);
		glVertex2f(x4, y4);
		glEnd();
	}
	if (stroke_on) {
		glLineWidth(stroke_wt);
		gl_color(stroke_c);
		glBegin(GL_LINE_LOOP);
		glVertex2f(x1, y1);
		glVertex2f(x2, y2);
		glVertex2f(x3, y3);
		glVertex2f(x4, y4);
		glEnd();
	}
}

void point(float x, float y)
{
	if (!stroke_on)
		return;
	gl_color(stroke_c);
	glBegin(GL_POINTS);
	glVertex2f(x, y);
	glEnd();
}

// --- custom shapes ------------------------------------------------------------
void beginShape(int kind)
{
	shape_kind = kind;
	shape_pts.clear();
}
void beginShape()
{
	beginShape(POLYGON);
}
void vertex(float x, float y)
{
	shape_pts.push_back(x);
	shape_pts.push_back(y);
}

namespace
{
float sx(int i)
{
	return shape_pts[2 * i];
}
float sy(int i)
{
	return shape_pts[2 * i + 1];
}
void edge(int a, int b)
{
	glVertex2f(sx(a), sy(a));
	glVertex2f(sx(b), sy(b));
}
} // namespace

void endShape(int mode)
{
	const int n = int(shape_pts.size() / 2);
	if (n == 0)
		return;

	// Fill pass. POINTS/LINES shapes have no interior; everything else maps
	// 1:1 onto a GL primitive (the CPU-side conversion in mini-GL handles the
	// strip/fan/quad topologies).
	if (fill_on && n >= 3 && shape_kind != POINTS && shape_kind != LINES) {
		GLenum prim = GL_POLYGON;
		switch (shape_kind) {
			case TRIANGLES: prim = GL_TRIANGLES; break;
			case TRIANGLE_STRIP: prim = GL_TRIANGLE_STRIP; break;
			case TRIANGLE_FAN: prim = GL_TRIANGLE_FAN; break;
			case QUADS: prim = GL_QUADS; break;
			case QUAD_STRIP: prim = GL_QUAD_STRIP; break;
		}
		gl_color(fill_c);
		glBegin(prim);
		for (int i = 0; i < n; i++)
			glVertex2f(sx(i), sy(i));
		glEnd();
	}

	// Stroke pass: outline every primitive the shape assembled, the way
	// Processing shows each triangle/quad of a strip.
	if (stroke_on) {
		glLineWidth(stroke_wt);
		gl_color(stroke_c);
		switch (shape_kind) {
			case POINTS:
				glBegin(GL_POINTS);
				for (int i = 0; i < n; i++)
					glVertex2f(sx(i), sy(i));
				glEnd();
				break;
			case LINES:
				glBegin(GL_LINES);
				for (int i = 0; i + 1 < n; i += 2)
					edge(i, i + 1);
				glEnd();
				break;
			case TRIANGLES:
				glBegin(GL_LINES);
				for (int i = 0; i + 2 < n; i += 3) {
					edge(i, i + 1);
					edge(i + 1, i + 2);
					edge(i + 2, i);
				}
				glEnd();
				break;
			case TRIANGLE_STRIP:
				glBegin(GL_LINES);
				for (int i = 0; i + 2 < n; i++) {
					edge(i, i + 1);
					edge(i, i + 2);
					edge(i + 1, i + 2);
				}
				glEnd();
				break;
			case TRIANGLE_FAN:
				glBegin(GL_LINES);
				for (int i = 1; i + 1 < n; i++) {
					edge(0, i);
					edge(i, i + 1);
					edge(i + 1, 0);
				}
				glEnd();
				break;
			case QUADS:
				glBegin(GL_LINES);
				for (int i = 0; i + 3 < n; i += 4) {
					edge(i, i + 1);
					edge(i + 1, i + 2);
					edge(i + 2, i + 3);
					edge(i + 3, i);
				}
				glEnd();
				break;
			case QUAD_STRIP:
				glBegin(GL_LINES);
				for (int i = 0; i + 3 < n; i += 2) {
					edge(i, i + 1);
					edge(i + 1, i + 3);
					edge(i + 3, i + 2);
					edge(i + 2, i);
				}
				glEnd();
				break;
			default: // POLYGON: the outline, closed or open
				glBegin(mode == CLOSE ? GL_LINE_LOOP : GL_LINE_STRIP);
				for (int i = 0; i < n; i++)
					glVertex2f(sx(i), sy(i));
				glEnd();
				break;
		}
	}
	shape_pts.clear();
}
void endShape()
{
	endShape(OPEN);
}

// --- environment --------------------------------------------------------------
void frameRate(float fps)
{
	frame_period_us = fps > 0 ? unsigned(1000000.0f / fps) : 0;
}
unsigned psk_frame_period_us()
{
	return frame_period_us;
}

// --- transforms ---------------------------------------------------------------
void pushMatrix()
{
	glPushMatrix();
}
void popMatrix()
{
	glPopMatrix();
}
void translate(float x, float y)
{
	glTranslatef(x, y, 0);
}
void rotate(float radians)
{
	glRotatef(radians * (180.0f / PI), 0, 0, 1);
}
void scale(float s)
{
	glScalef(s, s, 1);
}
void scale(float sx, float sy)
{
	glScalef(sx, sy, 1);
}

// --- harness ------------------------------------------------------------------
void psk_frame_begin()
{
	// No input device yet: the mouse sits at the screen center. Sketches that
	// map() from mouseX/mouseY get their mid-range behavior.
	mouseX = width / 2;
	mouseY = height / 2;

	// Processing's usual 2D frame: origin top-left, +y down, one unit per
	// pixel, and alpha blending on (its default BLEND mode).
	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	glOrtho(0, width, height, 0, -1, 1);
	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();
	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	glDisable(GL_DEPTH_TEST);
}
