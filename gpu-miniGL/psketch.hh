#pragma once
#include "gl/mgl_math.hh" // for kPi; the wrappers below use real libm
#include <cmath>

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
extern int width, height;	// set by the harness from the panel size
extern int frameCount;		// incremented by the harness after each draw()
extern int mouseX, mouseY;	// no input device yet: pinned to the screen center

// Keyboard: characters typed into the console UART (e.g. a minicom session on
// the board's serial port). Each received byte sets `key` and fires the
// keyPressed() event below.
extern char key;
extern int keyCode; // == key for printable keys; no escape-sequence decoding yet

// Processing's `keyPressed`/`mousePressed` BOOLEANS. C++ cannot give a
// variable and a function the same name, so sketches using the booleans must
// spell them with the underscore; the event *functions* keep the real names.
// A serial console has no key-up events, so _keyPressed is only true during
// the frame in which a byte arrived.
extern bool _keyPressed;
extern bool _mousePressed; // no mouse: always false for now

// Event handlers. Weak no-op defaults; a sketch that defines one overrides it.
void keyPressed();
void keyReleased(); // never fired yet (no key-up over a serial console)
void mousePressed();
void mouseReleased();

// --- Processing constants (values from PConstants.java) -----------------------
inline constexpr int RGB = 1;
inline constexpr int HSB = 3;
inline constexpr int CORNER = 0;  // ellipseMode / rectMode
inline constexpr int CORNERS = 1;
inline constexpr int RADIUS = 2;
inline constexpr int CENTER = 3;
inline constexpr int POINTS = 3;  // beginShape kinds
inline constexpr int LINES = 5;
inline constexpr int TRIANGLES = 9;
inline constexpr int TRIANGLE_STRIP = 10;
inline constexpr int TRIANGLE_FAN = 11;
inline constexpr int QUADS = 17;
inline constexpr int QUAD_STRIP = 18;
inline constexpr int POLYGON = 20;
inline constexpr int OPEN = 1;	  // endShape
inline constexpr int CLOSE = 2;

// --- the sketch entry points (implemented in sketch.cc) -----------------------
void sketch_setup();
void sketch_draw();

// --- math, Processing names ---------------------------------------------------
// These builds link libm (no -ffreestanding), so the wrappers are the real
// float functions -- exact where mgl_math (which mini-GL itself uses, so the
// host tests stay bit-identical to the target) is approximate.
inline constexpr float PI = mgl::kPi;
inline constexpr float TWO_PI = 2 * mgl::kPi;
inline constexpr float HALF_PI = mgl::kPi / 2;

inline float sqrt(float x)
{
	return ::sqrtf(x);
}
inline float sin(float x)
{
	return ::sinf(x);
}
inline float cos(float x)
{
	return ::cosf(x);
}
inline float tan(float x)
{
	return ::tanf(x);
}
inline float atan2(float y, float x)
{
	return ::atan2f(y, x);
}
inline float abs(float x)
{
	return ::fabsf(x);
}
inline float pow(float x, float y)
{
	return ::powf(x, y);
}
inline float dist(float x1, float y1, float x2, float y2)
{
	const float dx = x2 - x1, dy = y2 - y1;
	return ::sqrtf(dx * dx + dy * dy);
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
inline float radians(float deg)
{
	return deg * (mgl::kPi / 180.0f);
}
inline float degrees(float rad)
{
	return rad * (180.0f / mgl::kPi);
}
inline float sq(float x)
{
	return x * x;
}
inline float min(float a, float b)
{
	return a < b ? a : b;
}
inline float max(float a, float b)
{
	return a > b ? a : b;
}

float random(float hi); // deterministic xorshift, seeded at boot
float random(float lo, float hi);

int millis(); // ms since boot (the generic timer, not wall time)

// --- color and stroke state ---------------------------------------------------
// Channels are interpreted through colorMode: default RGB with 0..255 ranges.
// colorMode(HSB, 360, 100, 100) makes fill(h, s, b) hue-based, as in Processing.
void colorMode(int mode);
void colorMode(int mode, float max);
void colorMode(int mode, float max1, float max2, float max3);
void colorMode(int mode, float max1, float max2, float max3, float maxA);

// color() packs channels (interpreted through the current colorMode) into a
// 0xAARRGGBB int, Processing's `color` type. The int overloads of
// fill/stroke/background make Processing's distinction: a value with alpha
// bits set is a packed color; a small bare int is a gray level.
int color(float gray);
int color(float gray, float alpha);
int color(float r, float g, float b);
int color(float r, float g, float b, float a);

void background(float gray);
void background(float r, float g, float b);
void background(int c);
void fill(float gray);
void fill(float gray, float alpha);
void fill(float r, float g, float b);
void fill(float r, float g, float b, float a);
void fill(int c); // packed color() value, or a gray level (see color())
void noFill();
void stroke(float gray);
void stroke(float gray, float alpha);
void stroke(float r, float g, float b);
void stroke(float r, float g, float b, float a);
void stroke(int c); // likewise
void noStroke();
void strokeWeight(float w);

// --- shapes -------------------------------------------------------------------
void ellipseMode(int mode); // CENTER (default), RADIUS, CORNER, CORNERS
void rectMode(int mode);	// likewise (rect() honours it)
void ellipse(float a, float b, float c, float d); // interpreted per ellipseMode
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

// --- custom shapes ------------------------------------------------------------
void beginShape();		   // POLYGON
void beginShape(int kind); // POINTS/LINES/TRIANGLES/TRIANGLE_STRIP/... above
void vertex(float x, float y);
void endShape();		   // open
void endShape(int mode);   // CLOSE joins the last vertex back to the first

// --- transforms ---------------------------------------------------------------
void pushMatrix();
void popMatrix();
void translate(float x, float y);
void rotate(float radians);
void scale(float s);
void scale(float sx, float sy);

// --- environment --------------------------------------------------------------
void frameRate(float fps); // the harness throttles the frame loop to this

// Accepted-and-ignored stubs, so more sketches compile untouched. size() is a
// no-op because the panel decides the real size -- sketches should use
// width/height, which most already do.
inline void size(int, int)
{
}
inline void smooth()
{
}
inline void noSmooth()
{
}

// --- harness hooks (main.cc only; not part of the sketch-facing API) ----------
// Reset matrices/projection/blending to Processing defaults at the top of a
// frame. Sizes come from the globals above, which the harness sets first.
void psk_frame_begin();
// Minimum microseconds between draw() calls (0 = every vblank); from frameRate().
unsigned psk_frame_period_us();
