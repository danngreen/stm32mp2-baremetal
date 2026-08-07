#pragma once
#include "gl/mgl_math.hh"

// =============================================================================
//  psketch.hh -- the slice of the Processing API a sketch calls
// =============================================================================
//
// Free functions in the global namespace, spelled exactly like Processing, so a
// .pde sketch translates almost line-for-line into sketch.cc. Everything here
// is implemented in terms of mini-GL calls (psketch.cc); the harness in main.cc
// owns the frame loop and calls sketch_setup() once, then sketch_draw() every
// frame -- Processing's setup()/draw() contract.
//
// Processing conventions honoured here:
//  - origin top-left, +y down, one unit per pixel (width x height)
//  - colors are 0..255, default color mode RGB
//  - fill and stroke are separate persistent states; shapes draw fill first,
//    then the stroke outline on top
//  - blending is on by default (SRC_ALPHA / ONE_MINUS_SRC_ALPHA), so an alpha
//    fill like fill(255, 204) just works

// --- globals ------------------------------------------------------------------
extern int width, height; // set by the harness from the panel size
extern int frameCount;	  // incremented by the harness after each draw()

// --- the sketch entry points (implemented in sketch.cc) -----------------------
void sketch_setup();
void sketch_draw();

// --- math, Processing names ---------------------------------------------------
inline constexpr float PI = mgl::kPi;
inline constexpr float TWO_PI = 2 * mgl::kPi;
inline constexpr float HALF_PI = mgl::kPi / 2;

inline float sqrt(float x)
{
	return mgl::m_sqrt(x);
}
inline float sin(float x)
{
	return mgl::m_sin(x);
}
inline float cos(float x)
{
	return mgl::m_cos(x);
}
inline float abs(float x)
{
	return mgl::m_abs(x);
}
inline float pow(float x, float y)
{
	return mgl::m_pow(x, y);
}
inline float dist(float x1, float y1, float x2, float y2)
{
	const float dx = x2 - x1, dy = y2 - y1;
	return mgl::m_sqrt(dx * dx + dy * dy);
}
inline float constrain(float v, float lo, float hi)
{
	return v < lo ? lo : (v > hi ? hi : v);
}
inline float map(float v, float a0, float a1, float b0, float b1)
{
	return b0 + (b1 - b0) * ((v - a0) / (a1 - a0));
}
inline float lerp(float a, float b, float t)
{
	return a + (b - a) * t;
}

float random(float hi); // deterministic xorshift, seeded at boot
float random(float lo, float hi);

// --- color and stroke state (0..255, like Processing's default mode) ----------
void background(float gray);
void background(float r, float g, float b);
void fill(float gray);
void fill(float gray, float alpha);
void fill(float r, float g, float b);
void fill(float r, float g, float b, float a);
void noFill();
void stroke(float gray);
void stroke(float gray, float alpha);
void stroke(float r, float g, float b);
void stroke(float r, float g, float b, float a);
void noStroke();
void strokeWeight(float w);

// --- shapes -------------------------------------------------------------------
void ellipse(float cx, float cy, float w, float h); // CENTER mode, w/h diameters
inline void circle(float cx, float cy, float d)
{
	ellipse(cx, cy, d, d);
}
void rect(float x, float y, float w, float h); // CORNER mode
inline void square(float x, float y, float s)
{
	rect(x, y, s, s);
}
void line(float x1, float y1, float x2, float y2);
void triangle(float x1, float y1, float x2, float y2, float x3, float y3);
void quad(float x1, float y1, float x2, float y2, float x3, float y3, float x4, float y4);
void point(float x, float y);

// --- transforms ---------------------------------------------------------------
void pushMatrix();
void popMatrix();
void translate(float x, float y);
void rotate(float radians);
void scale(float s);
void scale(float sx, float sy);

// --- harness hooks (main.cc only; not part of the sketch-facing API) ----------
// Reset matrices/projection/blending to Processing defaults at the top of a
// frame. Sizes come from the globals above, which the harness sets first.
void psk_frame_begin();
