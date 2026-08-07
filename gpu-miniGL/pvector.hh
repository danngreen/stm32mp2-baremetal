#pragma once
#include <cmath>
#include <vector>

// =============================================================================
//  pvector.hh -- PVector and ArrayList, the two Java types sketches assume
// =============================================================================
//
// Processing sketches lean on these constantly, so they are part of the
// sketch-facing surface rather than something each sketch reimplements.
//
// PVector matches processing.core.PVector: instance methods MUTATE and return
// *this (so they chain -- `d.normalize().mult(k)` is real sketch code), while
// the static versions return a new vector. That asymmetry is not a wart to
// fix: sketches depend on both halves (`vel.add(acc)` mutates in place;
// `PVector::sub(a, b)` leaves its operands alone).

struct PVector {
	float x = 0, y = 0, z = 0;

	PVector() = default;
	PVector(float ix, float iy, float iz = 0)
		: x{ix}
		, y{iy}
		, z{iz}
	{}

	PVector &set(float ix, float iy, float iz = 0)
	{
		x = ix;
		y = iy;
		z = iz;
		return *this;
	}
	PVector &set(const PVector &v)
	{
		x = v.x;
		y = v.y;
		z = v.z;
		return *this;
	}

	PVector copy() const
	{
		return *this;
	}
	PVector get() const // deprecated alias in Processing, still common in examples
	{
		return *this;
	}

	// --- mutating instance ops ------------------------------------------------
	PVector &add(const PVector &v)
	{
		x += v.x;
		y += v.y;
		z += v.z;
		return *this;
	}
	PVector &add(float ix, float iy, float iz = 0)
	{
		x += ix;
		y += iy;
		z += iz;
		return *this;
	}
	PVector &sub(const PVector &v)
	{
		x -= v.x;
		y -= v.y;
		z -= v.z;
		return *this;
	}
	PVector &sub(float ix, float iy, float iz = 0)
	{
		x -= ix;
		y -= iy;
		z -= iz;
		return *this;
	}
	PVector &mult(float s)
	{
		x *= s;
		y *= s;
		z *= s;
		return *this;
	}
	PVector &div(float s)
	{
		if (s == 0)
			return *this; // Processing warns and leaves the vector alone
		x /= s;
		y /= s;
		z /= s;
		return *this;
	}

	float mag() const
	{
		return ::sqrtf(x * x + y * y + z * z);
	}
	float magSq() const
	{
		return x * x + y * y + z * z;
	}

	PVector &normalize()
	{
		const float m = mag();
		if (m != 0)
			div(m);
		return *this;
	}
	PVector &limit(float max)
	{
		if (magSq() > max * max) {
			normalize();
			mult(max);
		}
		return *this;
	}
	PVector &setMag(float len)
	{
		normalize();
		mult(len);
		return *this;
	}

	// Angle of the 2D projection, in radians.
	float heading() const
	{
		return ::atan2f(y, x);
	}
	// Rotate about z (2D rotation).
	PVector &rotate(float theta)
	{
		const float c = ::cosf(theta), s = ::sinf(theta);
		const float nx = x * c - y * s;
		y = x * s + y * c;
		x = nx;
		return *this;
	}

	float dist(const PVector &v) const
	{
		const float dx = x - v.x, dy = y - v.y, dz = z - v.z;
		return ::sqrtf(dx * dx + dy * dy + dz * dz);
	}
	float dot(const PVector &v) const
	{
		return x * v.x + y * v.y + z * v.z;
	}
	PVector cross(const PVector &v) const
	{
		return {y * v.z - z * v.y, z * v.x - x * v.z, x * v.y - y * v.x};
	}
	PVector &lerp(const PVector &v, float amt)
	{
		x += (v.x - x) * amt;
		y += (v.y - y) * amt;
		z += (v.z - z) * amt;
		return *this;
	}

	// --- static ops: these RETURN a new vector, operands untouched ------------
	static PVector add(const PVector &a, const PVector &b)
	{
		return {a.x + b.x, a.y + b.y, a.z + b.z};
	}
	static PVector sub(const PVector &a, const PVector &b)
	{
		return {a.x - b.x, a.y - b.y, a.z - b.z};
	}
	static PVector mult(const PVector &a, float s)
	{
		return {a.x * s, a.y * s, a.z * s};
	}
	static PVector div(const PVector &a, float s)
	{
		return s == 0 ? a : PVector{a.x / s, a.y / s, a.z / s};
	}
	static float dist(const PVector &a, const PVector &b)
	{
		return a.dist(b);
	}
	static float dot(const PVector &a, const PVector &b)
	{
		return a.dot(b);
	}
	static PVector cross(const PVector &a, const PVector &b)
	{
		return a.cross(b);
	}
	static PVector lerp(const PVector &a, const PVector &b, float amt)
	{
		PVector r = a;
		r.lerp(b, amt);
		return r;
	}
	static float angleBetween(const PVector &a, const PVector &b)
	{
		const float ma = a.mag(), mb = b.mag();
		if (ma == 0 || mb == 0)
			return 0;
		float c = a.dot(b) / (ma * mb);
		c = c < -1.0f ? -1.0f : (c > 1.0f ? 1.0f : c);
		return ::acosf(c);
	}
	static PVector fromAngle(float angle)
	{
		return {::cosf(angle), ::sinf(angle)};
	}
	// Defined in psketch.cc -- they draw on the sketch RNG.
	static PVector random2D();
	static PVector random3D();
};

// -----------------------------------------------------------------------------
// ArrayList<T>: Java's list, and Java's REFERENCE semantics -- the converted
// sketches store `new T(...)` and read elements back as `T*` (`p->run()`), so
// the element type is T*, not T.
//
// remove(i) DELETES the element. In Java, dropping a particle from the list
// drops its last reference and the GC reclaims it; without that, a particle
// system leaks its whole history and exhausts the 8 MB heap in under an hour.
// Sketches here follow the standard "erase the dead particle" idiom, so this
// is the faithful translation -- but it does mean a pointer held elsewhere
// dangles after remove().
//
// Deriving from std::vector is normally poor form (no virtual destructor);
// nothing here deletes an ArrayList through a base pointer, and inheriting
// gives range-for and indexing for free.
template <typename T>
struct ArrayList : std::vector<T *> {
	using Base = std::vector<T *>;
	using Base::Base;

	void add(T *p)
	{
		Base::push_back(p);
	}
	void add(int i, T *p)
	{
		Base::insert(Base::begin() + i, p);
	}
	T *get(int i) const
	{
		return (*this)[i];
	}
	// int, not size_t: `for (int i = 0; i < list.size(); i++)` is the idiom
	// every one of these sketches uses.
	int size() const
	{
		return static_cast<int>(Base::size());
	}
	void remove(int i)
	{
		delete (*this)[i];
		Base::erase(Base::begin() + i);
	}
	bool isEmpty() const
	{
		return Base::empty();
	}
};

// -----------------------------------------------------------------------------
// Array<T>: a Java array -- fixed size. Unlike ArrayList this holds T
// directly, because the converted sketches declare the pointer-ness
// themselves (`Array<Mover*> movers(10)`).
//
// In Java `.length` is a field; the sketches here are split between writing
// `a.length` and `a.length()`, so it is a small object that answers to both.
struct ArrayLength {
	int n = 0;
	constexpr operator int() const
	{
		return n;
	}
	constexpr int operator()() const
	{
		return n;
	}
};

template <typename T>
struct Array : std::vector<T> {
	using Base = std::vector<T>;
	ArrayLength length{};

	Array() = default;
	explicit Array(int n)
		: Base(n)
		, length{n}
	{}
	Array(std::initializer_list<T> il)
		: Base(il)
		, length{static_cast<int>(il.size())}
	{}
};
