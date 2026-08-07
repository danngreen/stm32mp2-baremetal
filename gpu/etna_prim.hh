#pragma once
#include "gpu_regs_3d.hh"
#include <cstdint>
#include <span>

// =============================================================================
//  etna_prim.hh -- primitive types (glBegin modes) for the 3D pipe
// =============================================================================
//
// The FE's DRAW_INSTANCED header carries a primitive type; values are from
// Mesa src/etnaviv/hw/cmdstream.xml.h (PRIMITIVE_TYPE_*). The draw count is
// always a VERTEX count, never a primitive count -- the hardware derives the
// primitive count from the type.
//
// WHICH TYPES OUR CHIP ACTUALLY SUPPORTS
// Mesa gates three of these on feature bits (etnaviv_screen.c
// supported_prim_modes). Checked against Mesa's ST feature database entry for
// our exact core -- src/etnaviv/hwdb/st/gc_feature_database.h,
// "GCNANOULTRA31_VIP2", ChipID 0x8000 / Rev 0x6205 / Product 0x80003 /
// Customer 0x15:
//     REG_LineLoop  = 1  -> LINE_LOOP is available
//     REG_BugFixes8 = 1  -> TRIANGLE_STRIP is safe (without this fix, indexed
//                           triangle strips are broken and Mesa disables the
//                           mode entirely)
//     REG_WideLine  = 1  -> wide lines available, AND see the WIDE_LINE trap below
// So all seven types below are usable on this chip.
//
// GL_QUADS is deliberately absent. The hardware does define a QUADS primitive
// (value 8) and Mesa's translate_draw_mode() maps to it, but Gallium never
// advertises MESA_PRIM_QUADS in supported_prim_modes, so that path is never
// exercised upstream and we treat it as unproven. Use expand_quads() to turn
// quad vertices into triangles on the CPU instead -- that is what a GL front
// end has to do for GL_QUAD_STRIP anyway.

namespace etna
{

enum class Primitive : uint32_t {
	Points = 1,
	Lines = 2,
	LineStrip = 3,
	Triangles = 4,
	TriangleStrip = 5,
	TriangleFan = 6,
	LineLoop = 7,
};

// Line primitives need PA_CONFIG.WIDE_LINE (see pa_config below).
constexpr bool is_line_prim(Primitive p)
{
	return p == Primitive::Lines || p == Primitive::LineStrip || p == Primitive::LineLoop;
}

// How many primitives `verts` vertices produce. 0 when there are too few.
constexpr uint32_t primitive_count(Primitive p, uint32_t verts)
{
	switch (p) {
		case Primitive::Points:
			return verts;
		case Primitive::Lines:
			return verts / 2;
		case Primitive::LineStrip:
			return verts < 2 ? 0 : verts - 1;
		case Primitive::LineLoop:
			// Closes back to the first vertex, so one more segment than a strip.
			// (Two vertices degenerate to a single segment drawn twice.)
			return verts < 2 ? 0 : verts;
		case Primitive::Triangles:
			return verts / 3;
		case Primitive::TriangleStrip:
		case Primitive::TriangleFan:
			return verts < 3 ? 0 : verts - 2;
	}
	return 0;
}

// Whether `verts` is a usable count -- enough vertices, and for the
// non-strip types an exact multiple (GL silently drops the remainder; we would
// rather catch it).
constexpr bool valid_vertex_count(Primitive p, uint32_t verts)
{
	if (primitive_count(p, verts) == 0)
		return false;
	if (p == Primitive::Lines)
		return verts % 2 == 0;
	if (p == Primitive::Triangles)
		return verts % 3 == 0;
	return true;
}

// PA_CONFIG for a draw: smooth shading, solid fill, culling off -- plus the
// WIDE_LINE bit for line primitives.
//
// THE WIDE_LINE TRAP: rnndb state_3d.xml flags bit 22 with "MUST be set when
// drawing lines when WIDE_LINE feature available, otherwise GC3000+ will not
// render lines at all". Our core has REG_WideLine = 1, so without this bit a
// line draw completes cleanly and produces no fragments -- no fault, no hang,
// just an empty target.
//
// Mesa sets WIDE_LINE for every draw whenever the feature is present, not just
// for lines. We set it only for line primitives so that triangle draws keep
// emitting the exact PA_CONFIG the existing hardware-verified tests used
// (proven by the static_assert below).
constexpr uint32_t pa_config(Primitive p)
{
	return VivanteGpu::PA_CONFIG_TRIANGLE | (is_line_prim(p) ? VivanteGpu::PA_CONFIG_WIDE_LINE : 0u);
}

// Expand quads to triangles: each group of 4 vertices v0,v1,v2,v3 becomes the
// two triangles (v0,v1,v2) and (v0,v2,v3) -- GL_QUADS ordering, and the same
// fan split a GL front end uses. `floats_per_vertex` lets this work on any
// interleaved vertex layout (emit_mesh's is 7: pos vec3 + colour vec4).
//
// `out` must have room for 6 vertices per quad. Returns the number of vertices
// written, or 0 if the input is not a whole number of quads or `out` is short.
constexpr uint32_t
expand_quads(std::span<const float> quads, std::span<float> out, uint32_t floats_per_vertex = 7)
{
	const uint32_t stride = floats_per_vertex;
	const uint32_t in_verts = static_cast<uint32_t>(quads.size()) / stride;
	if (stride == 0 || in_verts < 4 || in_verts % 4 != 0)
		return 0;

	const uint32_t nquads = in_verts / 4;
	const uint32_t out_verts = nquads * 6;
	if (out.size() < static_cast<size_t>(out_verts) * stride)
		return 0;

	constexpr uint32_t kOrder[6] = {0, 1, 2, 0, 2, 3};
	uint32_t w = 0;
	for (uint32_t q = 0; q < nquads; q++)
		for (uint32_t i = 0; i < 6; i++) {
			const uint32_t src = (q * 4 + kOrder[i]) * stride;
			for (uint32_t f = 0; f < stride; f++)
				out[w++] = quads[src + f];
		}
	return out_verts;
}

// --- compile-time proofs ------------------------------------------------------
// Triangle draws must keep emitting the byte-identical PA_CONFIG the existing
// verified tests used, so adding primitives cannot perturb them.
static_assert(pa_config(Primitive::Triangles) == VivanteGpu::PA_CONFIG_TRIANGLE);
static_assert(pa_config(Primitive::TriangleStrip) == VivanteGpu::PA_CONFIG_TRIANGLE);
static_assert(pa_config(Primitive::TriangleFan) == VivanteGpu::PA_CONFIG_TRIANGLE);
static_assert(pa_config(Primitive::Points) == VivanteGpu::PA_CONFIG_TRIANGLE);
// ...and every line type must carry WIDE_LINE.
static_assert(pa_config(Primitive::Lines) == (VivanteGpu::PA_CONFIG_TRIANGLE | 0x400000));
static_assert(pa_config(Primitive::LineStrip) == (VivanteGpu::PA_CONFIG_TRIANGLE | 0x400000));
static_assert(pa_config(Primitive::LineLoop) == (VivanteGpu::PA_CONFIG_TRIANGLE | 0x400000));

// Primitive counting, including the strip-vs-loop difference the test relies on.
static_assert(primitive_count(Primitive::Points, 5) == 5);
static_assert(primitive_count(Primitive::Lines, 6) == 3);
static_assert(primitive_count(Primitive::LineStrip, 3) == 2);
static_assert(primitive_count(Primitive::LineLoop, 3) == 3); // one more edge than the strip
static_assert(primitive_count(Primitive::Triangles, 9) == 3);
static_assert(primitive_count(Primitive::TriangleStrip, 4) == 2);
static_assert(primitive_count(Primitive::TriangleFan, 4) == 2);
static_assert(primitive_count(Primitive::TriangleFan, 2) == 0);

static_assert(!valid_vertex_count(Primitive::Lines, 5));
static_assert(valid_vertex_count(Primitive::Lines, 4));
static_assert(!valid_vertex_count(Primitive::Triangles, 4));
static_assert(!valid_vertex_count(Primitive::TriangleStrip, 2));

} // namespace etna
