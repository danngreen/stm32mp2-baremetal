#pragma once
#include "mgl_math.hh"
#include <array>

// =============================================================================
//  mat4.hh -- the matrix math behind the GL matrix stack
// =============================================================================
//
// CONVENTIONS, because getting these wrong is silent and expensive:
//
//  - Storage is COLUMN-MAJOR, like OpenGL: m[c * 4 + r] is row r, column c.
//    A glLoadMatrixf/glMultMatrixf array from an app can be memcpy'd straight
//    in, and glGetFloatv(GL_MODELVIEW_MATRIX) can hand ours straight back.
//  - Vectors are COLUMN vectors, transformed as v' = M * v.
//  - GL's matrix commands POST-multiply the current matrix: glRotatef etc. do
//    C = C * R. So the last command issued is the first transform applied to a
//    vertex, which is why glTranslate-then-glRotate rotates about the
//    translated origin.
//
// Everything is constexpr-friendly and free of GPU dependencies so the whole
// matrix stack can be exercised on the host.

namespace mgl
{

struct Vec4 {
	float x = 0, y = 0, z = 0, w = 1;
};

struct Mat4 {
	// Column-major: m[c * 4 + r].
	std::array<float, 16> m{};

	static constexpr Mat4 identity()
	{
		Mat4 r{};
		r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1.0f;
		return r;
	}

	constexpr float at(int row, int col) const
	{
		return m[col * 4 + row];
	}
	constexpr void set(int row, int col, float v)
	{
		m[col * 4 + row] = v;
	}

	// this * rhs
	constexpr Mat4 operator*(const Mat4 &rhs) const
	{
		Mat4 r{};
		for (int c = 0; c < 4; c++)
			for (int row = 0; row < 4; row++) {
				float s = 0;
				for (int k = 0; k < 4; k++)
					s += at(row, k) * rhs.at(k, c);
				r.set(row, c, s);
			}
		return r;
	}

	constexpr Vec4 operator*(const Vec4 &v) const
	{
		return Vec4{
			at(0, 0) * v.x + at(0, 1) * v.y + at(0, 2) * v.z + at(0, 3) * v.w,
			at(1, 0) * v.x + at(1, 1) * v.y + at(1, 2) * v.z + at(1, 3) * v.w,
			at(2, 0) * v.x + at(2, 1) * v.y + at(2, 2) * v.z + at(2, 3) * v.w,
			at(3, 0) * v.x + at(3, 1) * v.y + at(3, 2) * v.z + at(3, 3) * v.w,
		};
	}

	// Transform a direction (w = 0), for normals under a rigid transform.
	constexpr Vec4 transform_dir(float x, float y, float z) const
	{
		return Vec4{at(0, 0) * x + at(0, 1) * y + at(0, 2) * z,
					at(1, 0) * x + at(1, 1) * y + at(1, 2) * z,
					at(2, 0) * x + at(2, 1) * y + at(2, 2) * z, 0.0f};
	}
};

constexpr Mat4 translation(float x, float y, float z)
{
	Mat4 r = Mat4::identity();
	r.set(0, 3, x);
	r.set(1, 3, y);
	r.set(2, 3, z);
	return r;
}

constexpr Mat4 scaling(float x, float y, float z)
{
	Mat4 r{};
	r.set(0, 0, x);
	r.set(1, 1, y);
	r.set(2, 2, z);
	r.set(3, 3, 1.0f);
	return r;
}

// glRotatef: angle in DEGREES about the axis (x,y,z), which is normalised here
// (GL does the same). A zero-length axis yields identity rather than NaNs.
inline Mat4 rotation(float angle_deg, float x, float y, float z)
{
	const float len = m_sqrt(x * x + y * y + z * z);
	if (len == 0.0f)
		return Mat4::identity();
	x /= len;
	y /= len;
	z /= len;

	const float a = angle_deg * kPi / 180.0f;
	const float c = m_cos(a), s = m_sin(a), t = 1.0f - c;

	Mat4 r = Mat4::identity();
	r.set(0, 0, t * x * x + c);
	r.set(0, 1, t * x * y - s * z);
	r.set(0, 2, t * x * z + s * y);
	r.set(1, 0, t * x * y + s * z);
	r.set(1, 1, t * y * y + c);
	r.set(1, 2, t * y * z - s * x);
	r.set(2, 0, t * x * z - s * y);
	r.set(2, 1, t * y * z + s * x);
	r.set(2, 2, t * z * z + c);
	return r;
}

// glOrtho. Maps [l,r]x[b,t]x[-n,-f] to the [-1,1] cube.
constexpr Mat4 ortho(float l, float r, float b, float t, float n, float f)
{
	Mat4 o{};
	o.set(0, 0, 2.0f / (r - l));
	o.set(1, 1, 2.0f / (t - b));
	o.set(2, 2, -2.0f / (f - n));
	o.set(0, 3, -(r + l) / (r - l));
	o.set(1, 3, -(t + b) / (t - b));
	o.set(2, 3, -(f + n) / (f - n));
	o.set(3, 3, 1.0f);
	return o;
}

// glFrustum. Note the -1 in row 3: this is where w picks up -z, which is what
// makes the perspective divide happen.
constexpr Mat4 frustum(float l, float r, float b, float t, float n, float f)
{
	Mat4 o{};
	o.set(0, 0, 2.0f * n / (r - l));
	o.set(1, 1, 2.0f * n / (t - b));
	o.set(0, 2, (r + l) / (r - l));
	o.set(1, 2, (t + b) / (t - b));
	o.set(2, 2, -(f + n) / (f - n));
	o.set(2, 3, -2.0f * f * n / (f - n));
	o.set(3, 2, -1.0f);
	return o;
}

// --- compile-time sanity ------------------------------------------------------
static_assert(Mat4::identity().at(0, 0) == 1.0f && Mat4::identity().at(0, 1) == 0.0f);
// Identity is a multiplicative unit.
static_assert((Mat4::identity() * translation(3, 4, 5)).at(0, 3) == 3.0f);
// Column-major storage: a translation puts its offsets in the LAST column,
// i.e. elements 12,13,14 -- exactly where OpenGL apps expect to find them.
static_assert(translation(3, 4, 5).m[12] == 3.0f);
static_assert(translation(3, 4, 5).m[13] == 4.0f);
static_assert(translation(3, 4, 5).m[14] == 5.0f);
// Translation applies to a point.
static_assert((translation(1, 2, 3) * Vec4{10, 20, 30, 1}).x == 11.0f);
// ...but not to a direction (w = 0).
static_assert((translation(1, 2, 3) * Vec4{10, 20, 30, 0}).x == 10.0f);
// glOrtho maps the box corners onto the NDC cube: left -> -1, right -> +1.
static_assert((ortho(0, 100, 0, 50, -1, 1) * Vec4{0, 0, 0, 1}).x == -1.0f);
static_assert((ortho(0, 100, 0, 50, -1, 1) * Vec4{100, 0, 0, 1}).x == 1.0f);
static_assert((ortho(0, 100, 0, 50, -1, 1) * Vec4{0, 50, 0, 1}).y == 1.0f);
// Processing's flipped ortho (top = 0, bottom = height) puts y=0 at NDC +1.
static_assert((ortho(0, 100, 50, 0, -1, 1) * Vec4{0, 0, 0, 1}).y == 1.0f);

} // namespace mgl
