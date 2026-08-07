#pragma once
#include "gpu_regs_3d.hh"
#include <algorithm>
#include <cstdint>

// =============================================================================
//  etna_raster.hh -- face culling and scissor
// =============================================================================
//
// glCullFace/glFrontFace map to PA_CONFIG.CULL_FACE_MODE; glScissor maps to the
// SE scissor + clip register pairs. Ported from Mesa's translate_cull_face()
// (etnaviv_translate.h) and etna_update_clipping() (etnaviv_state.c).

namespace etna
{

enum class CullMode : uint32_t {
	None,  // glDisable(GL_CULL_FACE)
	Front, // glCullFace(GL_FRONT)
	Back,  // glCullFace(GL_BACK)
};

enum class FrontFace : uint32_t {
	CW,  // glFrontFace(GL_CW)
	CCW, // glFrontFace(GL_CCW)
};

// PA_CONFIG.CULL_FACE_MODE is [9:8], and its value names the winding to
// DISCARD (0 = off, 1 = cull clockwise, 2 = cull counter-clockwise) -- not the
// front face. Combining glCullFace with glFrontFace therefore collapses to:
// cull CCW exactly when "cull the front face" and "front faces are CCW" agree.
//
// NOTE: which winding the hardware calls clockwise is a WINDOW-space question,
// and our viewport maps NDC +Y to increasing framebuffer rows (see
// triangle_test), which is the opposite of the usual GL convention. So do not
// assume a screen-space handedness here -- primitive_cull_test() determines it
// empirically and only asserts the relationships that must hold either way.
constexpr uint32_t cull_bits(CullMode cull, FrontFace front)
{
	if (cull == CullMode::None)
		return 0;
	const bool cull_ccw = (cull == CullMode::Front) == (front == FrontFace::CCW);
	return (cull_ccw ? VivanteGpu::PA_CONFIG_CULL_CCW : VivanteGpu::PA_CONFIG_CULL_CW);
}

// A scissor rectangle. maxx/maxy are EXCLUSIVE, matching Mesa (which clips
// against fb->width / fb->height directly). Default-constructed = disabled,
// meaning "the whole render target".
struct Scissor {
	bool enable = false;
	uint32_t minx = 0, miny = 0;
	uint32_t maxx = 0, maxy = 0;

	// Resolve against the target size: disabled means full target, and an
	// enabled rect is clamped to the target the way etna_update_clipping()
	// intersects the scissor with the framebuffer.
	constexpr Scissor resolved(uint32_t width, uint32_t height) const
	{
		if (!enable)
			return {true, 0, 0, width, height};
		Scissor r{true, std::min(minx, width), std::min(miny, height), std::min(maxx, width),
				  std::min(maxy, height)};
		// Canonicalize empty/inverted rects to [1,1)x[1,1): the SE registers
		// take (max<<16)-1 for the exclusive edge (left > right rejects every
		// pixel), and max = 0 would underflow that to a full-open scissor.
		if (r.maxx <= r.minx || r.maxy <= r.miny)
			return {true, 1, 1, 1, 1};
		return r;
	}

	constexpr bool empty() const
	{
		return maxx <= minx || maxy <= miny;
	}
};

// --- compile-time proofs ------------------------------------------------------
// Culling off must contribute nothing, so PA_CONFIG stays byte-identical to
// what the existing verified tests emitted.
static_assert(cull_bits(CullMode::None, FrontFace::CW) == 0);
static_assert(cull_bits(CullMode::None, FrontFace::CCW) == 0);
// All four combinations, cross-checked against Mesa's translate_cull_face:
//   cull BACK,  front CCW -> cull CW      cull FRONT, front CCW -> cull CCW
//   cull BACK,  front CW  -> cull CCW     cull FRONT, front CW  -> cull CW
static_assert(cull_bits(CullMode::Back, FrontFace::CCW) == VivanteGpu::PA_CONFIG_CULL_CW);
static_assert(cull_bits(CullMode::Front, FrontFace::CCW) == VivanteGpu::PA_CONFIG_CULL_CCW);
static_assert(cull_bits(CullMode::Back, FrontFace::CW) == VivanteGpu::PA_CONFIG_CULL_CCW);
static_assert(cull_bits(CullMode::Front, FrontFace::CW) == VivanteGpu::PA_CONFIG_CULL_CW);
// Flipping either the cull face or the winding must flip the culled winding.
static_assert(cull_bits(CullMode::Back, FrontFace::CCW) != cull_bits(CullMode::Front, FrontFace::CCW));
static_assert(cull_bits(CullMode::Back, FrontFace::CCW) != cull_bits(CullMode::Back, FrontFace::CW));

// Disabled scissor resolves to the full target.
static_assert(Scissor{}.resolved(64, 48).maxx == 64);
static_assert(Scissor{}.resolved(64, 48).maxy == 48);
static_assert(Scissor{}.resolved(64, 48).minx == 0);
// An enabled rect is preserved, and an oversized one is clamped.
static_assert(Scissor{true, 8, 8, 32, 24}.resolved(64, 48).maxx == 32);
static_assert(Scissor{true, 8, 8, 999, 999}.resolved(64, 48).maxx == 64);
static_assert(Scissor{true, 8, 8, 999, 999}.resolved(64, 48).maxy == 48);
// An inverted rect collapses to empty rather than wrapping.
static_assert(Scissor{true, 40, 40, 10, 10}.resolved(64, 48).empty());
// Empty rects resolve away from 0 so (maxx<<16)-1 in the SE registers can
// never underflow into a scissor that accepts everything.
static_assert(Scissor{true, 0, 0, 0, 0}.resolved(64, 48).empty());
static_assert(Scissor{true, 0, 0, 0, 0}.resolved(64, 48).maxx >= 1);
static_assert(Scissor{true, 30, 30, 30, 30}.resolved(64, 48).empty());

} // namespace etna
