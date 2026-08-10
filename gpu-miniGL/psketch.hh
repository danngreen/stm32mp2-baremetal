#pragma once
#include "gl/mgl_math.hh" // for kPi; the wrappers below use real libm
#include "pvector.hh"	  // PVector and ArrayList, assumed by most sketches
#include <cmath>
#include <string>
#include <vector>

using String = std::string; // Processing's String
using boolean = bool;		// Java's spelling, used verbatim by some sketches

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
// There is no pointer device, so the cursor takes a random walk of its own
// (see mouse_walk in psketch.cc): the velocity wanders and the position
// integrates it, bouncing off the edges. Mouse-driven sketches therefore
// animate rather than sitting at one value.
extern int mouseX, mouseY;
extern int pmouseX, pmouseY; // where it was last frame
extern int mouseDX, mouseDY; // how far it moved this frame

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
// Renderers, as passed to size(). P3D turns on the depth buffer and
// Processing's default perspective camera; OPENGL is its old name for P3D.
inline constexpr int P2D = 2;
inline constexpr int P3D = 3;
inline constexpr int OPENGL = 3;

inline constexpr int OPEN = 1;	  // endShape, and arc() mode
inline constexpr int CLOSE = 2;
inline constexpr int CHORD = 3; // arc() modes
inline constexpr int PIE = 4;

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

// Templates, not plain float overloads: a sketch writing `atan2(y - 5, x - 3)`
// on ints would otherwise be ambiguous between this and <cmath>'s float and
// double versions. A template loses to an exact non-template match, so calls
// with real floats or doubles still go straight to libm.
template <typename T>
inline float sqrt(T x)
{
	return ::sqrtf(float(x));
}
template <typename T>
inline float sin(T x)
{
	return ::sinf(float(x));
}
template <typename T>
inline float cos(T x)
{
	return ::cosf(float(x));
}
template <typename T>
inline float tan(T x)
{
	return ::tanf(float(x));
}
template <typename A, typename B>
inline float atan2(A y, B x)
{
	return ::atan2f(float(y), float(x));
}
inline float abs(float x)
{
	return ::fabsf(x);
}
template <typename A, typename B>
inline float pow(A x, B y)
{
	return ::powf(float(x), float(y));
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
void randomSeed(unsigned s);
float randomGaussian(); // mean 0, standard deviation 1 (Box-Muller)

// Perlin noise, Processing's algorithm: 0..1, smooth, and the same value for
// the same coordinate. noiseDetail() sets how many octaves are summed and how
// fast their amplitude falls off -- more octaves = more fine detail.
float noise(float x);
float noise(float x, float y);
float noise(float x, float y, float z);
void noiseDetail(int octaves);
void noiseDetail(int octaves, float falloff);
void noiseSeed(unsigned s);

// -----------------------------------------------------------------------------
// Processing's IntList / FloatList / StringList: a resizable list with Java
// method names. (ArrayList, in pvector.hh, is the one that holds objects by
// pointer; these hold values.)
template <typename T>
struct NumList : std::vector<T> {
	using Base = std::vector<T>;
	using Base::Base;

	int size() const
	{
		return static_cast<int>(Base::size());
	}
	T get(int i) const
	{
		return (*this)[i];
	}
	void set(int i, T v)
	{
		(*this)[i] = v;
	}
	void append(T v)
	{
		Base::push_back(v);
	}
	void remove(int i)
	{
		Base::erase(Base::begin() + i);
	}
	bool hasValue(T v) const
	{
		for (const T &e : *this)
			if (e == v)
				return true;
		return false;
	}
	void shuffle() // Fisher-Yates on the sketch RNG
	{
		for (int i = size() - 1; i > 0; i--) {
			const int j = static_cast<int>(random(float(i + 1)));
			T tmp = (*this)[i];
			(*this)[i] = (*this)[j];
			(*this)[j] = tmp;
		}
	}
};
using IntList = NumList<int>;
using FloatList = NumList<float>;
using StringList = NumList<String>;

int millis(); // ms since boot (the generic timer, not wall time)

// Fake wall clock: there is no RTC, so this "time of day" starts at boot.
// Enough for sketches that use time as an animation source (Rotate, Clock).
int second();
int minute();
int hour();

// --- color and stroke state ---------------------------------------------------
// Channels are interpreted through colorMode: default RGB with 0..255 ranges.
// colorMode(HSB, 360, 100, 100) makes fill(h, s, b) hue-based, as in Processing.
void colorMode(int mode);
void colorMode(int mode, float max);
void colorMode(int mode, float max1, float max2, float max3);
void colorMode(int mode, float max1, float max2, float max3, float maxA);

// In Processing `color` is BOTH a type (`color c1, c2;`) and a
// constructor-like function (`color(0, 200, 0)`) -- Java keeps those in
// separate namespaces, C++ cannot. So it is a type whose constructors read
// exactly like the calls, converting implicitly to the packed 0xAARRGGBB int
// that fill()/stroke()/background() accept. Channels go through the current
// colorMode, and a one-argument color() is a gray level, as in Processing.
struct color {
	int v = 0;
	color() = default;
	color(float gray);
	color(float gray, float alpha);
	color(float r, float g, float b);
	color(float r, float g, float b, float a);
	constexpr operator int() const
	{
		return v;
	}
};

// Blend two packed colors; amt 0..1. Straight lerp of the channels.
color lerpColor(int c1, int c2, float amt);

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
void rect(float x, float y, float w, float h);	 // interpreted per rectMode
// NOTE: Processing's circle()/square() shorthands are deliberately absent.
// They are just ellipse()/rect() with equal dimensions, and as free functions
// they collide with the sketch variables of the same name that examples
// declare (Morph has `ArrayList<PVector> circle`) -- Java keeps method and
// field namespaces apart, C++ does not.
void line(float x1, float y1, float x2, float y2);
void line(float x1, float y1, float z1, float x2, float y2, float z2);
// A standalone cubic Bezier: (x1,y1) and (x4,y4) are the endpoints, the
// middle pair the controls. Stroked, like Processing's.
void bezier(float x1, float y1, float x2, float y2, float x3, float y3, float x4, float y4);
// Arc of the ellipse a/b/c/d (interpreted per ellipseMode), from `start` to
// `stop` radians, clockwise on screen from the +x axis. Default mode OPEN
// fills the region closed by the chord and strokes only the curve; CHORD adds
// the closing line; PIE fills/strokes to the centre.
void arc(float a, float b, float c, float d, float start, float stop);
void arc(float a, float b, float c, float d, float start, float stop, int mode);
void triangle(float x1, float y1, float x2, float y2, float x3, float y3);
void quad(float x1, float y1, float x2, float y2, float x3, float y3, float x4, float y4);
void point(float x, float y);

// --- custom shapes ------------------------------------------------------------
void beginShape();		   // POLYGON
void beginShape(int kind); // POINTS/LINES/TRIANGLES/TRIANGLE_STRIP/... above
void vertex(float x, float y);
void vertex(float x, float y, float z);
// Cubic Bezier from the previous vertex, through two control points, to
// (x, y) -- tessellated on the CPU into ordinary vertices.
void bezierVertex(float cx1, float cy1, float cx2, float cy2, float x, float y);
void bezierDetail(int n);
void endShape();		   // open
void endShape(int mode);   // CLOSE joins the last vertex back to the first

// --- transforms ---------------------------------------------------------------
void pushMatrix();
void popMatrix();
void translate(float x, float y);
void translate(float x, float y, float z);
void rotate(float radians); // about z, the 2D rotation
void rotate(float angle, float x, float y, float z);
void rotateX(float angle);
void rotateY(float angle);
void rotateZ(float angle);
void scale(float s);
void scale(float sx, float sy);
void scale(float sx, float sy, float sz);

// --- 3D camera and projection -------------------------------------------------
// The no-argument forms are Processing's defaults: the eye sits back far
// enough on +z that one world unit is one pixel at z = 0, so 2D-style
// coordinates keep working in P3D.
void camera();
void camera(float eyeX, float eyeY, float eyeZ, float centerX, float centerY, float centerZ, float upX, float upY,
			float upZ);
void perspective();
void perspective(float fovy, float aspect, float zNear, float zFar);
void ortho();
void ortho(float left, float right, float bottom, float top);
void ortho(float left, float right, float bottom, float top, float near, float far);

// --- lighting -----------------------------------------------------------------
// Lights are per-frame in Processing: they reset every draw(), so a sketch
// calls lights() (or the individual ones) each frame.
void lights();	 // ambient + a directional light from the viewer
void noLights();
void ambientLight(float r, float g, float b);
void directionalLight(float r, float g, float b, float nx, float ny, float nz);
void pointLight(float r, float g, float b, float x, float y, float z);
void spotLight(float r, float g, float b, float x, float y, float z, float nx, float ny, float nz, float angle,
			   float concentration);
void lightFalloff(float constant, float linear, float quadratic);
void lightSpecular(float r, float g, float b);
void normal(float nx, float ny, float nz);

// Material response, applied to subsequent geometry.
void specular(float r, float g, float b);
void specular(float gray);
void shininess(float s);
void emissive(float r, float g, float b);
void emissive(float gray);
void ambient(float r, float g, float b);
void ambient(float gray);

// --- 3D primitives ------------------------------------------------------------
void box(float size);
void box(float w, float h, float d);
void sphere(float r);
void sphereDetail(int n);
void sphereDetail(int ures, int vres);

// --- direct pixel access ------------------------------------------------------
// `pixels` is the frame as 0xAARRGGBB, one int per pixel, row-major.
// loadPixels() sizes it; write into it; updatePixels() puts it on screen.
// It lands in the scanout buffer AFTER the GPU has resolved the frame, so a
// sketch that touches pixels is painting over anything it also drew.
extern Array<int> pixels;
void loadPixels();
void updatePixels();

// --- text ---------------------------------------------------------------------
// Accepted and ignored: drawing glyphs needs the texture path (see TODO.md).
// Sketches that label their output still run, just without the labels.
// Templated on what is being printed: sketches pass literals, ints, floats,
// chars and Strings, and fixed overloads would make an int argument ambiguous
// between the float and char ones.
template <typename T>
inline void text(T, float, float)
{
}
template <typename T>
inline void text(T, float, float, float, float)
{
}
inline void textAlign(int)
{
}
inline void textAlign(int, int)
{
}
inline void textSize(float)
{
}
inline void textLeading(float)
{
}
inline float textWidth(const char *)
{
	return 0;
}

// --- environment --------------------------------------------------------------
void frameRate(float fps); // the harness throttles the frame loop to this

// The panel decides the real size, so the dimensions are ignored -- sketches
// should use width/height, which most already do. The RENDERER argument is
// not ignored: P3D switches on the depth buffer and the 3D camera.
inline void size(int, int)
{
}
void size(int, int, int renderer);
inline void smooth()
{
}
inline void noSmooth()
{
}
// The harness redraws every frame regardless; a noLoop() sketch just redraws
// the same static image, which produces the same pixels.
inline void noLoop()
{
}
inline void loop()
{
}
inline void redraw()
{
}

// --- PApplet compatibility shim -----------------------------------------------
// In Processing a sketch *is* a PApplet, so a class method can always reach
// the drawing API through the enclosing instance. Converted sketches use that
// escape hatch where a member name would otherwise shadow a global (a class's
// own `dist()` hiding the free `dist()`), so the shim just forwards.
struct PApplet {
	static float dist(float x1, float y1, float x2, float y2)
	{
		return ::dist(x1, y1, x2, y2);
	}
	void noStroke()
	{
		::noStroke();
	}
	void noFill()
	{
		::noFill();
	}
	static PApplet *g_papplet; // always non-null; sketches null-check anyway
};

// --- harness hooks (main.cc only; not part of the sketch-facing API) ----------
// Reset matrices/projection/blending to Processing defaults at the top of a
// frame. Sizes come from the globals above, which the harness sets first.
void psk_frame_begin();
// Minimum microseconds between draw() calls (0 = every vblank); from frameRate().
unsigned psk_frame_period_us();
// True when the sketch asked for P3D: the harness needs a depth buffer.
bool psk_wants_3d();
// The sketch's pixel image, non-null from updatePixels() until the next
// background() retires it. The harness copies it into the scanout buffer
// after the resolve, every frame -- it is frame content, not a one-off event.
const int *psk_live_pixels();
// After sketch_draw(), before the backend ends the frame: emits any deferred
// stroke geometry (see psketch.cc's deferred-strokes note).
void psk_frame_end();
