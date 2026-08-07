#pragma once
#include "gpu_regs_3d.hh"
#include <cstdint>

// =============================================================================
//  etna_blend.hh -- glBlendFunc/glBlendEquation -> PE register words
// =============================================================================
//
// A port of Mesa's etna_blend_state_create() + etna_update_blend()
// (src/gallium/drivers/etnaviv/etnaviv_blend.c, MIT) reduced to the one thing
// we have: a single render target, always A8R8G8B8, always LOGIC_OP_COPY.
//
// Two register words come out of a blend state:
//   - PE_ALPHA_CONFIG : the enable bit + the four factors + two equations
//   - PE_COLOR_FORMAT : specifically its OVERWRITE bit, which tells the PE it
//     may skip reading the render target. Blending *needs* that read, so
//     OVERWRITE must be cleared whenever blending is live. Getting this wrong
//     is silent: the blend math runs against stale/garbage destination.
//
// Everything is constexpr so a blend state costs nothing at runtime and the
// encodings can be proven against the known-good current values at compile
// time (see the static_asserts at the bottom).
//
// Not implemented: the CONSTANT_* factors (BLEND_FUNC 11..14) -- see the note
// in gpu_regs_3d.hh. Callers therefore never need PE_ALPHA_BLEND_COLOR, which
// stays 0.

namespace etna
{

// Blend factors and equations, named as in GL. Values are the hardware
// BLEND_FUNC / BLEND_EQ encodings, so no translation table is needed.
enum class BlendFactor : uint32_t {
	Zero = VivanteGpu::BLEND_FUNC_ZERO,
	One = VivanteGpu::BLEND_FUNC_ONE,
	SrcColor = VivanteGpu::BLEND_FUNC_SRC_COLOR,
	OneMinusSrcColor = VivanteGpu::BLEND_FUNC_ONE_MINUS_SRC_COLOR,
	SrcAlpha = VivanteGpu::BLEND_FUNC_SRC_ALPHA,
	OneMinusSrcAlpha = VivanteGpu::BLEND_FUNC_ONE_MINUS_SRC_ALPHA,
	DstAlpha = VivanteGpu::BLEND_FUNC_DST_ALPHA,
	OneMinusDstAlpha = VivanteGpu::BLEND_FUNC_ONE_MINUS_DST_ALPHA,
	DstColor = VivanteGpu::BLEND_FUNC_DST_COLOR,
	OneMinusDstColor = VivanteGpu::BLEND_FUNC_ONE_MINUS_DST_COLOR,
	SrcAlphaSaturate = VivanteGpu::BLEND_FUNC_SRC_ALPHA_SATURATE,
};

enum class BlendEq : uint32_t {
	Add = VivanteGpu::BLEND_EQ_ADD,
	Subtract = VivanteGpu::BLEND_EQ_SUBTRACT,
	ReverseSubtract = VivanteGpu::BLEND_EQ_REVERSE_SUBTRACT,
	Min = VivanteGpu::BLEND_EQ_MIN,
	Max = VivanteGpu::BLEND_EQ_MAX,
};

// One render target's blend state. Default-constructed = blending off, which
// reproduces the register words the pipe used before blending existed.
//
// The alpha_* factors/equation are only consulted when they differ from the
// rgb_* ones; when they match, BLEND_SEPARATE_ALPHA stays clear and the
// hardware applies the color factors to all four channels (Mesa does the
// same). So the common glBlendFunc() case -- which sets one factor pair for
// everything -- needs only rgb_src/rgb_dst.
struct BlendState {
	bool enable = false;
	BlendFactor rgb_src = BlendFactor::One;
	BlendFactor rgb_dst = BlendFactor::Zero;
	BlendEq rgb_eq = BlendEq::Add;
	BlendFactor alpha_src = BlendFactor::One;
	BlendFactor alpha_dst = BlendFactor::Zero;
	BlendEq alpha_eq = BlendEq::Add;

	// src*ONE + dst*ZERO with ADD is arithmetically a plain overwrite, so the
	// hardware blender is left off even if `enable` is set -- that keeps the
	// OVERWRITE fast path (no render-target read). Mirrors Mesa's alpha_enable.
	constexpr bool blending_active() const
	{
		return enable && !(rgb_src == BlendFactor::One && rgb_dst == BlendFactor::Zero && rgb_eq == BlendEq::Add &&
						   alpha_src == BlendFactor::One && alpha_dst == BlendFactor::Zero &&
						   alpha_eq == BlendEq::Add);
	}

	// Separate alpha blending is only requested when the alpha side actually
	// differs; otherwise the color factors already cover all four channels.
	constexpr bool separate_alpha() const
	{
		return blending_active() &&
			   !(rgb_src == alpha_src && rgb_dst == alpha_dst && rgb_eq == alpha_eq);
	}

	// True when the PE may skip reading the render target (Mesa: fo_allowed &&
	// full colormask). We have no color mask and never use a logic op, so this
	// is exactly "not blending".
	constexpr bool full_overwrite() const
	{
		return !blending_active();
	}

	// The PE_ALPHA_CONFIG word. All the *_MASK write-enable bits stay 0 because
	// we re-emit the whole register on every draw.
	constexpr uint32_t pe_alpha_config() const
	{
		using namespace VivanteGpu;
		if (!blending_active())
			return 0;

		return PE_ALPHA_CONFIG_BLEND_ENABLE_COLOR |
			   (separate_alpha() ? PE_ALPHA_CONFIG_BLEND_SEPARATE_ALPHA : 0u) |
			   (static_cast<uint32_t>(rgb_src) << PE_ALPHA_CONFIG_SRC_FUNC_COLOR_SHIFT) |
			   (static_cast<uint32_t>(rgb_dst) << PE_ALPHA_CONFIG_DST_FUNC_COLOR_SHIFT) |
			   (static_cast<uint32_t>(rgb_eq) << PE_ALPHA_CONFIG_EQ_COLOR_SHIFT) |
			   (static_cast<uint32_t>(alpha_src) << PE_ALPHA_CONFIG_SRC_FUNC_ALPHA_SHIFT) |
			   (static_cast<uint32_t>(alpha_dst) << PE_ALPHA_CONFIG_DST_FUNC_ALPHA_SHIFT) |
			   (static_cast<uint32_t>(alpha_eq) << PE_ALPHA_CONFIG_EQ_ALPHA_SHIFT);
	}

	// The full PE_COLOR_FORMAT word for our fixed A8R8G8B8 tiled target.
	constexpr uint32_t pe_color_format() const
	{
		using namespace VivanteGpu;
		return PE_FORMAT_A8R8G8B8 | PE_COLOR_FORMAT_COMPONENTS_ALL |
			   (full_overwrite() ? PE_COLOR_FORMAT_OVERWRITE : 0u);
	}
};

// The canonical Processing/GL "transparency" blend:
//   glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
inline constexpr BlendState kBlendSrcAlpha{
	.enable = true,
	.rgb_src = BlendFactor::SrcAlpha,
	.rgb_dst = BlendFactor::OneMinusSrcAlpha,
	.alpha_src = BlendFactor::SrcAlpha,
	.alpha_dst = BlendFactor::OneMinusSrcAlpha,
};

// Additive blending: glBlendFunc(GL_SRC_ALPHA, GL_ONE). Processing's ADD mode.
inline constexpr BlendState kBlendAdditive{
	.enable = true,
	.rgb_src = BlendFactor::SrcAlpha,
	.rgb_dst = BlendFactor::One,
	.alpha_src = BlendFactor::SrcAlpha,
	.alpha_dst = BlendFactor::One,
};

// --- compile-time proofs ------------------------------------------------------
// 1. The default (blending off) reproduces exactly the two register words the
//    3D pipe hardcoded before this existed -- so every pre-blend test is
//    provably unaffected.
static_assert(BlendState{}.pe_alpha_config() == 0);
static_assert(BlendState{}.pe_color_format() ==
			  (VivanteGpu::PE_FORMAT_A8R8G8B8 | VivanteGpu::PE_COLOR_FORMAT_COMPONENTS_ALL |
			   VivanteGpu::PE_COLOR_FORMAT_OVERWRITE));

// 2. A blend state that is arithmetically a no-op (ONE/ZERO/ADD) also collapses
//    to the overwrite fast path rather than turning the blender on.
static_assert(BlendState{.enable = true}.pe_alpha_config() == 0);
static_assert(BlendState{.enable = true}.full_overwrite());

// 3. SRC_ALPHA / ONE_MINUS_SRC_ALPHA, same factors on both sides:
//    ENABLE(1) | SRC_COLOR(4)<<4 | DST_COLOR(5)<<8 | EQ_COLOR(0)<<12
//              | SRC_ALPHA(4)<<20 | DST_ALPHA(5)<<24 | EQ_ALPHA(0)<<28
//    = 0x1 | 0x40 | 0x500 | 0x400000 | 0x5000000 = 0x05400541.
//    SEPARATE_ALPHA (bit 16) stays clear because both sides match.
static_assert(kBlendSrcAlpha.pe_alpha_config() == 0x05400541);
static_assert(!kBlendSrcAlpha.full_overwrite());
static_assert(kBlendSrcAlpha.pe_color_format() ==
			  (VivanteGpu::PE_FORMAT_A8R8G8B8 | VivanteGpu::PE_COLOR_FORMAT_COMPONENTS_ALL));

// 4. Additive: DST becomes ONE(1) on both sides -> 0x01400141.
static_assert(kBlendAdditive.pe_alpha_config() == 0x01400141);

// 5. A genuinely separate alpha side sets bit 16. Straight-alpha color with a
//    saturating alpha channel: rgb = SRC_ALPHA/ONE_MINUS_SRC_ALPHA,
//    alpha = ONE/ONE_MINUS_SRC_ALPHA.
static_assert(BlendState{.enable = true,
						 .rgb_src = BlendFactor::SrcAlpha,
						 .rgb_dst = BlendFactor::OneMinusSrcAlpha,
						 .alpha_src = BlendFactor::One,
						 .alpha_dst = BlendFactor::OneMinusSrcAlpha}
				  .pe_alpha_config() == (0x05100541 | 0x10000));

} // namespace etna
