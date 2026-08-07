#include "psketch.hh"
#include "aarch64/system_reg.hh" // read_cntpct/read_cntfreq for millis()
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
int pmouseX = 0, pmouseY = 0;
int mouseDX = 0, mouseDY = 0;
char key = 0;
int keyCode = 0;
bool _keyPressed = false;
bool _mousePressed = false;

namespace
{
PApplet papplet_instance;
}
PApplet *PApplet::g_papplet = &papplet_instance;

// Weak no-op event handlers; a sketch that defines one overrides it.
__attribute__((weak)) void keyPressed()
{
}
__attribute__((weak)) void keyReleased()
{
}
__attribute__((weak)) void mousePressed()
{
}
__attribute__((weak)) void mouseReleased()
{
}

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

// --- deferred strokes ---------------------------------------------------------
// Stroke lines are not drawn where they are issued: segments accumulate here
// and go out in bulk when something forces it -- a stroke-state change, a
// matrix change (vertices transform at emit time), a clear, or frame end.
// Fills stay immediate, so a run of stroked shapes becomes one long triangle
// batch plus one line batch instead of splitting at every shape: Game of Life
// drops from 73,728 GPU draws a frame to ~130. This is Processing's own P2D
// "optimized stroke" behavior, including its known quirk: within a flush
// window, strokes render on top of later fills. Identical for shapes that
// don't overlap.
// 8 floats per segment: x1,y1,x2,y2,r,g,b,a. Stroke COLOR rides along with
// each segment (it is a vertex attribute, not batch state, so color changes
// never force a flush -- a sketch stroking every segment differently still
// batches). Stroke WEIGHT is real pipeline state and does flush.
std::vector<float> pend_lines;
float pend_wt = 1.0f;
constexpr size_t kSegFloats = 8;
// One glBegin block must fit mini-GL's 4096-vertex begin buffer: 2048 segs.
constexpr size_t kStrokeChunkSegs = 2048;
// Flush when ~1 MB of segments is pending. Flushing early only costs a few
// extra batches; letting the vector grow unbounded cost the whole heap --
// hardware-debugged: a dense sketch deferred 2.4 MB and the doubling realloc
// (old + new block live at once) blew the 8 MB heap into abort(). The buffer
// is reserved at this size once, so steady state never reallocates.
constexpr size_t kStrokeCapFloats = 256u * 1024;

void flush_strokes()
{
	if (pend_lines.empty())
		return;
	glLineWidth(pend_wt);
	const size_t nsegs = pend_lines.size() / kSegFloats;
	for (size_t s = 0; s < nsegs;) {
		size_t chunk = nsegs - s;
		if (chunk > kStrokeChunkSegs)
			chunk = kStrokeChunkSegs;
		glBegin(GL_LINES);
		for (size_t k = 0; k < chunk; k++) {
			const float *p = &pend_lines[(s + k) * kSegFloats];
			glColor4f(p[4], p[5], p[6], p[7]);
			glVertex2f(p[0], p[1]);
			glVertex2f(p[2], p[3]);
		}
		glEnd();
		s += chunk;
	}
	pend_lines.clear();
}

void defer_line(float x1, float y1, float x2, float y2)
{
	if (!pend_lines.empty() && pend_wt != stroke_wt)
		flush_strokes();
	if (pend_lines.empty()) {
		if (pend_lines.capacity() < kStrokeCapFloats)
			pend_lines.reserve(kStrokeCapFloats);
		pend_wt = stroke_wt;
	}
	pend_lines.insert(pend_lines.end(),
					  {x1, y1, x2, y2, stroke_c[0], stroke_c[1], stroke_c[2], stroke_c[3]});
	if (pend_lines.size() >= kStrokeCapFloats)
		flush_strokes();
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

// --- time ---------------------------------------------------------------------
int millis()
{
	return int(read_cntpct() * 1000u / read_cntfreq());
}

int second()
{
	return (millis() / 1000) % 60;
}
int minute()
{
	return (millis() / 60000) % 60;
}
int hour()
{
	return (millis() / 3600000) % 24;
}

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

void randomSeed(unsigned s)
{
	rng = s ? s : 1; // xorshift is stuck at zero
}

float randomGaussian()
{
	// Box-Muller, keeping the second value for the next call as Processing does.
	static bool have = false;
	static float spare = 0;
	if (have) {
		have = false;
		return spare;
	}
	float u, v, s;
	do {
		u = random(-1, 1);
		v = random(-1, 1);
		s = u * u + v * v;
	} while (s >= 1 || s == 0);
	const float f = ::sqrtf(-2.0f * ::logf(s) / s);
	spare = v * f;
	have = true;
	return u * f;
}

PVector PVector::random2D()
{
	return fromAngle(random(TWO_PI));
}

PVector PVector::random3D()
{
	const float angle = random(TWO_PI);
	const float vz = random(-1, 1);
	const float vxy = ::sqrtf(1.0f - vz * vz);
	return {vxy * ::cosf(angle), vxy * ::sinf(angle), vz};
}

// --- Perlin noise -------------------------------------------------------------
// Processing's own algorithm (PApplet.noise): a table of random values indexed
// by the integer lattice, cosine-interpolated, summed over `octaves` with each
// octave at half amplitude and double frequency. Reproducing it rather than
// inventing one matters -- sketches are tuned to how this specific noise looks.
namespace
{
constexpr int kPerlinYWrapB = 4, kPerlinYWrap = 1 << kPerlinYWrapB;
constexpr int kPerlinZWrapB = 8, kPerlinZWrap = 1 << kPerlinZWrapB;
constexpr int kPerlinSize = 4095;

float perlin[kPerlinSize + 1];
bool perlin_init = false;
int perlin_octaves = 4;
float perlin_falloff = 0.5f;

// The interpolant: 0.5*(1-cos(t*PI)), a cosine ease between lattice points.
float noise_fsc(float t)
{
	return 0.5f * (1.0f - ::cosf(t * PI));
}

void perlin_seed(unsigned s)
{
	const uint32_t save = rng;
	rng = s ? s : 1;
	for (int i = 0; i <= kPerlinSize; i++)
		perlin[i] = random(1.0f);
	rng = save;
	perlin_init = true;
}
} // namespace

void noiseSeed(unsigned s)
{
	perlin_seed(s);
}

void noiseDetail(int octaves)
{
	if (octaves > 0)
		perlin_octaves = octaves;
}

void noiseDetail(int octaves, float falloff)
{
	if (octaves > 0)
		perlin_octaves = octaves;
	perlin_falloff = falloff;
}

float noise(float x, float y, float z)
{
	if (!perlin_init)
		perlin_seed(0x9E3779B9u);

	if (x < 0)
		x = -x;
	if (y < 0)
		y = -y;
	if (z < 0)
		z = -z;

	int xi = int(x), yi = int(y), zi = int(z);
	float xf = x - float(xi), yf = y - float(yi), zf = z - float(zi);
	float r = 0, ampl = 0.5f;

	for (int i = 0; i < perlin_octaves; i++) {
		int of = xi + (yi << kPerlinYWrapB) + (zi << kPerlinZWrapB);
		const float rxf = noise_fsc(xf), ryf = noise_fsc(yf);

		float n1 = perlin[of & kPerlinSize];
		n1 += rxf * (perlin[(of + 1) & kPerlinSize] - n1);
		float n2 = perlin[(of + kPerlinYWrap) & kPerlinSize];
		n2 += rxf * (perlin[(of + kPerlinYWrap + 1) & kPerlinSize] - n2);
		n1 += ryf * (n2 - n1);

		of += kPerlinZWrap;
		n2 = perlin[of & kPerlinSize];
		n2 += rxf * (perlin[(of + 1) & kPerlinSize] - n2);
		float n3 = perlin[(of + kPerlinYWrap) & kPerlinSize];
		n3 += rxf * (perlin[(of + kPerlinYWrap + 1) & kPerlinSize] - n3);
		n2 += ryf * (n3 - n2);

		n1 += noise_fsc(zf) * (n2 - n1);

		r += n1 * ampl;
		ampl *= perlin_falloff;
		xi <<= 1;
		xf *= 2;
		yi <<= 1;
		yf *= 2;
		zi <<= 1;
		zf *= 2;
		if (xf >= 1.0f) {
			xi++;
			xf--;
		}
		if (yf >= 1.0f) {
			yi++;
			yf--;
		}
		if (zf >= 1.0f) {
			zi++;
			zf--;
		}
	}
	return r;
}

float noise(float x, float y)
{
	return noise(x, y, 0);
}
float noise(float x)
{
	return noise(x, 0, 0);
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

// --- packed colors (Processing's `color` type) --------------------------------
namespace
{
int pack(const float c[4])
{
	auto u8 = [](float v) { return uint32_t(v * 255.0f + 0.5f) & 0xFFu; };
	return int((u8(c[3]) << 24) | (u8(c[0]) << 16) | (u8(c[1]) << 8) | u8(c[2]));
}
// A packed color always has its alpha bits set (color() never produces
// alpha 0 from default ranges); a bare gray level like fill(48) never does.
bool is_packed(int c)
{
	return (uint32_t(c) & 0xFF000000u) != 0;
}
void unpack(int c, float out[4])
{
	out[0] = float((uint32_t(c) >> 16) & 0xFF) / 255.0f;
	out[1] = float((uint32_t(c) >> 8) & 0xFF) / 255.0f;
	out[2] = float(uint32_t(c) & 0xFF) / 255.0f;
	out[3] = float((uint32_t(c) >> 24) & 0xFF) / 255.0f;
}
} // namespace

color::color(float r, float g, float b, float a)
{
	float c[4];
	to_rgba(r, g, b, a, c);
	v = pack(c);
}
color::color(float r, float g, float b)
	: color(r, g, b, cmax[3])
{}
color::color(float gray, float alpha)
{
	float c[4];
	gray_rgba(gray, alpha, c);
	v = pack(c);
}
color::color(float gray)
	: color(gray, cmax[3])
{}

color lerpColor(int c1, int c2, float amt)
{
	amt = amt < 0 ? 0 : (amt > 1 ? 1 : amt);
	float a[4], b[4];
	unpack(c1, a);
	unpack(c2, b);
	for (int i = 0; i < 4; i++)
		a[i] += (b[i] - a[i]) * amt;
	color r;
	r.v = pack(a);
	return r;
}

void background(float r, float g, float b)
{
	float c[4];
	to_rgba(r, g, b, cmax[3], c);
	glClearColor(c[0], c[1], c[2], 1.0f);
	pend_lines.clear(); // pending strokes would be painted over by the clear
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
}
void background(float gray)
{
	float c[4];
	gray_rgba(gray, cmax[3], c);
	glClearColor(c[0], c[1], c[2], 1.0f);
	pend_lines.clear(); // pending strokes would be painted over by the clear
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
}
void background(int c)
{
	if (!is_packed(c)) {
		background(float(c));
		return;
	}
	float f[4];
	unpack(c, f);
	glClearColor(f[0], f[1], f[2], 1.0f);
	pend_lines.clear(); // pending strokes would be painted over by the clear
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
void fill(int c)
{
	if (!is_packed(c)) {
		fill(float(c));
		return;
	}
	fill_on = true;
	unpack(c, fill_c);
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
void stroke(int c)
{
	if (!is_packed(c)) {
		stroke(float(c));
		return;
	}
	stroke_on = true;
	unpack(c, stroke_c);
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

// ellipseMode applied: a/b/c/d -> centre + radii. Shared by ellipse and arc.
static void resolve_ellipse(float a, float b, float c, float d, float &cx, float &cy, float &rx, float &ry)
{
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
}

void ellipse(float a, float b, float c, float d)
{
	float cx, cy, rx, ry;
	resolve_ellipse(a, b, c, d, cx, cy, rx, ry);
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
		float px = cx + rx, py = cy; // the i = 0 point
		for (int i = 1; i <= n; i++) {
			const float a = TWO_PI * float(i) / float(n);
			const float qx = cx + rx * cos(a), qy = cy + ry * sin(a);
			defer_line(px, py, qx, qy);
			px = qx;
			py = qy;
		}
	}
}

void arc(float a, float b, float c, float d, float start, float stop, int mode)
{
	if (stop < start)
		stop += TWO_PI;
	const float sweep = stop - start;
	if (sweep <= 0)
		return;

	float cx, cy, rx, ry;
	resolve_ellipse(a, b, c, d, cx, cy, rx, ry);

	// Segment the arc at the same angular density a full ellipse would use, so
	// a 90-degree arc is as smooth as the circle it came from.
	const int full = ellipse_segments(rx > ry ? rx : ry);
	int n = int(float(full) * sweep / TWO_PI + 0.5f);
	if (n < 2)
		n = 2;

	auto px = [&](int i) { return cx + rx * cos(start + sweep * float(i) / float(n)); };
	auto py = [&](int i) { return cy + ry * sin(start + sweep * float(i) / float(n)); };

	if (fill_on) {
		gl_color(fill_c);
		glBegin(GL_TRIANGLE_FAN);
		// PIE fans from the centre; OPEN/CHORD fill only the region the chord
		// closes off, which is the fan from the arc's first point.
		if (mode == PIE)
			glVertex2f(cx, cy);
		for (int i = 0; i <= n; i++)
			glVertex2f(px(i), py(i));
		glEnd();
	}

	if (stroke_on) {
		for (int i = 0; i < n; i++)
			defer_line(px(i), py(i), px(i + 1), py(i + 1));
		if (mode == CHORD)
			defer_line(px(n), py(n), px(0), py(0));
		else if (mode == PIE) {
			defer_line(px(n), py(n), cx, cy);
			defer_line(cx, cy, px(0), py(0));
		}
	}
}

void arc(float a, float b, float c, float d, float start, float stop)
{
	arc(a, b, c, d, start, stop, OPEN);
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
		defer_line(x, y, x + w, y);
		defer_line(x + w, y, x + w, y + h);
		defer_line(x + w, y + h, x, y + h);
		defer_line(x, y + h, x, y);
	}
}

void line(float x1, float y1, float x2, float y2)
{
	if (!stroke_on)
		return;
	defer_line(x1, y1, x2, y2);
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
		defer_line(x1, y1, x2, y2);
		defer_line(x2, y2, x3, y3);
		defer_line(x3, y3, x1, y1);
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
		defer_line(x1, y1, x2, y2);
		defer_line(x2, y2, x3, y3);
		defer_line(x3, y3, x4, y4);
		defer_line(x4, y4, x1, y1);
	}
}

void point(float x, float y)
{
	if (!stroke_on)
		return;
	flush_strokes(); // keep stroke ordering: pending lines predate this point
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
int bezier_detail = 20;

// The cubic Bezier basis, evaluated at t.
void bezier_point(float t, float x1, float y1, float cx1, float cy1, float cx2, float cy2, float x2, float y2,
				  float &ox, float &oy)
{
	const float u = 1.0f - t;
	const float b0 = u * u * u, b1 = 3 * u * u * t, b2 = 3 * u * t * t, b3 = t * t * t;
	ox = b0 * x1 + b1 * cx1 + b2 * cx2 + b3 * x2;
	oy = b0 * y1 + b1 * cy1 + b2 * cy2 + b3 * y2;
}
} // namespace

void bezierDetail(int n)
{
	if (n > 0)
		bezier_detail = n;
}

void bezierVertex(float cx1, float cy1, float cx2, float cy2, float x, float y)
{
	if (shape_pts.size() < 2) // no anchor to curve away from
		return;
	const float x1 = shape_pts[shape_pts.size() - 2], y1 = shape_pts.back();
	for (int i = 1; i <= bezier_detail; i++) {
		float px, py;
		bezier_point(float(i) / float(bezier_detail), x1, y1, cx1, cy1, cx2, cy2, x, y, px, py);
		vertex(px, py);
	}
}

void bezier(float x1, float y1, float x2, float y2, float x3, float y3, float x4, float y4)
{
	if (!stroke_on)
		return;
	float px = x1, py = y1;
	for (int i = 1; i <= bezier_detail; i++) {
		float qx, qy;
		bezier_point(float(i) / float(bezier_detail), x1, y1, x2, y2, x3, y3, x4, y4, qx, qy);
		defer_line(px, py, qx, qy);
		px = qx;
		py = qy;
	}
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
	defer_line(sx(a), sy(a), sx(b), sy(b));
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
	// Processing shows each triangle/quad of a strip. Everything goes through
	// defer_line (see above); only points draw immediately.
	if (stroke_on) {
		switch (shape_kind) {
			case POINTS:
				flush_strokes();
				gl_color(stroke_c);
				glBegin(GL_POINTS);
				for (int i = 0; i < n; i++)
					glVertex2f(sx(i), sy(i));
				glEnd();
				break;
			case LINES:
				for (int i = 0; i + 1 < n; i += 2)
					edge(i, i + 1);
				break;
			case TRIANGLES:
				for (int i = 0; i + 2 < n; i += 3) {
					edge(i, i + 1);
					edge(i + 1, i + 2);
					edge(i + 2, i);
				}
				break;
			case TRIANGLE_STRIP:
				for (int i = 0; i + 2 < n; i++) {
					edge(i, i + 1);
					edge(i, i + 2);
					edge(i + 1, i + 2);
				}
				break;
			case TRIANGLE_FAN:
				for (int i = 1; i + 1 < n; i++) {
					edge(0, i);
					edge(i, i + 1);
					edge(i + 1, 0);
				}
				break;
			case QUADS:
				for (int i = 0; i + 3 < n; i += 4) {
					edge(i, i + 1);
					edge(i + 1, i + 2);
					edge(i + 2, i + 3);
					edge(i + 3, i);
				}
				break;
			case QUAD_STRIP:
				for (int i = 0; i + 3 < n; i += 2) {
					edge(i, i + 1);
					edge(i + 1, i + 3);
					edge(i + 3, i + 2);
					edge(i + 2, i);
				}
				break;
			default: // POLYGON: the outline, closed or open
				for (int i = 0; i + 1 < n; i++)
					edge(i, i + 1);
				if (mode == CLOSE && n > 2)
					edge(n - 1, 0);
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
// Each flushes pending strokes first: deferred vertices transform when they
// are finally emitted, so they must go out under the matrix they were drawn
// with.
void pushMatrix()
{
	flush_strokes();
	glPushMatrix();
}
void popMatrix()
{
	flush_strokes();
	glPopMatrix();
}
void translate(float x, float y)
{
	flush_strokes();
	glTranslatef(x, y, 0);
}
void rotate(float radians)
{
	flush_strokes();
	glRotatef(radians * (180.0f / PI), 0, 0, 1);
}
void scale(float s)
{
	flush_strokes();
	glScalef(s, s, 1);
}
void scale(float sx, float sy)
{
	flush_strokes();
	glScalef(sx, sy, 1);
}

// --- harness ------------------------------------------------------------------
void psk_frame_begin()
{
	pend_lines.clear(); // defensive; psk_frame_end() flushed the last frame

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

void psk_frame_end()
{
	flush_strokes();
}

// --- direct pixel access ------------------------------------------------------
Array<int> pixels;
namespace
{
bool pixels_dirty = false;
}

void loadPixels()
{
	const int n = width * height;
	if (pixels.length != n) {
		pixels.assign(n, 0);
		pixels.length = {n};
	}
}

void updatePixels()
{
	if (pixels.length == width * height)
		pixels_dirty = true;
}

const int *psk_take_pixels()
{
	if (!pixels_dirty)
		return nullptr;
	pixels_dirty = false;
	return pixels.data();
}
