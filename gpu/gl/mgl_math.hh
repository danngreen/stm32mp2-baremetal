#pragma once
#include <bit>
#include <cstdint>

// =============================================================================
//  mgl_math.hh -- the float math mini-GL needs, without <cmath>
// =============================================================================
//
// The firmware builds freestanding (-nostdlib), where <cmath> is unavailable:
// it drags in <math.h> and the tr1 special-function headers, which fail with
// "This header is not available in freestanding mode". That is the same reason
// cube_scene.hh hand-rolls its own tsin().
//
// These are the four functions the fixed-function pipeline actually needs:
// sqrt for normalising vectors, sin/cos for glRotatef, and pow for the
// specular highlight. Accuracy only has to be good enough to look right --
// nothing here feeds a comparison or a conveyed value.
//
// The same header is used by the host tests, so target and host run identical
// arithmetic and a host test result means something on the board.

namespace mgl
{

inline constexpr float kPi = 3.14159265358979f;

// aarch64 has FSQRT, so this is one instruction and needs no library.
inline float m_sqrt(float x)
{
	return __builtin_sqrtf(x);
}

inline float m_abs(float x)
{
	return x < 0 ? -x : x;
}

// Range-reduced odd polynomial, the same approach as cube_scene.hh's tsin.
inline float m_sin(float x)
{
	while (x > kPi)
		x -= 2 * kPi;
	while (x < -kPi)
		x += 2 * kPi;
	if (x > kPi / 2)
		x = kPi - x;
	else if (x < -kPi / 2)
		x = -kPi - x;
	const float x2 = x * x;
	return x * (1.0f - x2 / 6.0f * (1.0f - x2 / 20.0f * (1.0f - x2 / 42.0f)));
}

inline float m_cos(float x)
{
	return m_sin(x + kPi / 2);
}

// log2/exp2 by splitting the float into exponent and mantissa and running a
// small polynomial on the mantissa. std::bit_cast keeps this well-defined
// rather than relying on union type-punning.
inline float m_log2(float x)
{
	if (x <= 0.0f)
		return -1.0e30f;
	uint32_t i = std::bit_cast<uint32_t>(x);
	const int e = int((i >> 23) & 0xFF) - 127;
	i = (i & 0x007FFFFFu) | 0x3F800000u; // mantissa into [1,2)
	const float m = std::bit_cast<float>(i);
	const float p = -1.7417939f + (2.8212026f + (-1.4699568f + (0.44717955f - 0.056570851f * m) * m) * m) * m;
	return p + float(e);
}

inline float m_exp2(float x)
{
	if (x < -126.0f)
		return 0.0f;
	if (x > 127.0f)
		return 3.4e38f;
	const float fl = float(int(x) - (x < 0.0f && x != float(int(x)) ? 1 : 0)); // floor
	const float f = x - fl;												  // [0,1)
	const float p = 1.0f + f * (0.6931472f + f * (0.2402265f + f * (0.0555041f + f * 0.0096181f)));
	const uint32_t bits = uint32_t(int(fl) + 127) << 23;
	return p * std::bit_cast<float>(bits);
}

// Only used for the specular exponent, where the base is a clamped dot product
// in [0,1] and the exponent is a shininess value.
inline float m_pow(float x, float y)
{
	if (x <= 0.0f)
		return 0.0f;
	if (y == 0.0f)
		return 1.0f;
	return m_exp2(y * m_log2(x));
}

} // namespace mgl
