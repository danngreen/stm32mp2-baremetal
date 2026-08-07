#include "psketch.hh"
#include "gl/mini_gl.hh"
#include <cstdint>

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

namespace
{

bool fill_on = true, stroke_on = true;
float fill_c[4] = {1, 1, 1, 1};
float stroke_c[4] = {0, 0, 0, 1};
float stroke_wt = 1.0f;
uint32_t rng = 0x243F6A88; // pi, why not

void gl_color(const float c[4])
{
	glColor4f(c[0], c[1], c[2], c[3]);
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
void background(float r, float g, float b)
{
	glClearColor(r / 255.0f, g / 255.0f, b / 255.0f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
}
void background(float gray)
{
	background(gray, gray, gray);
}

void fill(float r, float g, float b, float a)
{
	fill_on = true;
	fill_c[0] = r / 255.0f;
	fill_c[1] = g / 255.0f;
	fill_c[2] = b / 255.0f;
	fill_c[3] = a / 255.0f;
}
void fill(float r, float g, float b)
{
	fill(r, g, b, 255);
}
void fill(float gray, float alpha)
{
	fill(gray, gray, gray, alpha);
}
void fill(float gray)
{
	fill(gray, gray, gray, 255);
}
void noFill()
{
	fill_on = false;
}

void stroke(float r, float g, float b, float a)
{
	stroke_on = true;
	stroke_c[0] = r / 255.0f;
	stroke_c[1] = g / 255.0f;
	stroke_c[2] = b / 255.0f;
	stroke_c[3] = a / 255.0f;
}
void stroke(float r, float g, float b)
{
	stroke(r, g, b, 255);
}
void stroke(float gray, float alpha)
{
	stroke(gray, gray, gray, alpha);
}
void stroke(float gray)
{
	stroke(gray, gray, gray, 255);
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
void ellipse(float cx, float cy, float w, float h)
{
	const float rx = w * 0.5f, ry = h * 0.5f;
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

void rect(float x, float y, float w, float h)
{
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
