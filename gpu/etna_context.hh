#pragma once
#include "etna_3d.hh"
#include <array>
#include <cstdint>

// =============================================================================
//  etna_context.hh -- dirty-state tracking so a draw emits only what changed
// =============================================================================
//
// THE PROBLEM
// emit_mesh() is self-contained: every call re-emits ~110 state registers, both
// shader programs, the uniforms, and four pipeline stalls, then drains the PE.
// That is right for one draw per submission, and it is what the existing tests
// use. It is roughly 250 dwords and several full pipeline drains per draw --
// fine for twelve cubes, hopeless for a sketch drawing a thousand shapes.
//
// THE MODEL (ported from Mesa's etna_emit_state(), etnaviv_emit.c)
// A Context remembers the state it last emitted. Each draw diffs the new
// MeshDraw against it, emits only the register blocks whose group changed, and
// -- the part that actually matters for speed -- emits a pipeline sync ONLY
// when the state it guards has changed. Mesa's three sync points are the
// complete list:
//
//   1. cache flush + RA->PE stall, before changing framebuffer/blend/depth
//      state (the caches hold pixels rendered with the OLD state)
//   2. FE->PE stall, before loading shader/uniform state -- from HALTI0 on,
//      those registers are NOT self-synchronizing, so writing them while a
//      draw is in flight corrupts it
//   3. ICACHE prefetch + RA->PE stall, at the end of the shader/uniform block
//
// There is deliberately no sync BETWEEN draws: the PE orders fragments to the
// same render target itself, which is why Mesa emits draws back to back. The
// stall/flush/stall drain is a per-SUBMISSION cost and lives in drain().
//
// USAGE
//     Context ctx{cs};
//     for (auto &d : draws) ctx.draw(d);   // deltas only
//     ctx.drain();                          // once
//     gpu.submit_and_wait(cs);
//
// Vertex data for the draws must come from an Arena (etna_arena.hh), NOT from
// one reused buffer -- see the warning there.
//
// CAVEAT: Context tracks what IT emitted. RS clears/blits and resolves don't
// touch 3D state so they can be interleaved freely, but anything else that
// reprograms the pipe behind its back (a PPU compute dispatch does) must be
// followed by invalidate().

namespace etna
{

// State groups. A MeshDraw field belongs to exactly one; a register block is
// re-emitted when any group it depends on is dirty.
enum DirtyBits : uint32_t {
	DirtyFramebuffer = 1u << 0, // rt / depth buffers, their strides, draw size
	DirtyBlend = 1u << 1,
	DirtyDepthState = 1u << 2,
	DirtyRaster = 1u << 3,	 // primitive type, cull, line width, point size
	DirtyScissor = 1u << 4,
	DirtyShaders = 1u << 5,	 // shader Bos, their sizes, temp/output registers
	DirtyUniforms = 1u << 6,
	DirtyVertex = 1u << 7,	 // vertex buffer address and stride
	DirtyStatic = 1u << 8,	 // the one-time pipe init + invariant blocks
	DirtyAll = 0x1FFu,
};

// Worst-case dwords one full draw can emit; draw() refuses rather than
// overrunning the command stream's fixed backing buffer.
inline constexpr uint32_t kMaxDrawDwords = 512;

// Uniform bank we mirror for change detection. The unified bank is larger, but
// a 4x4 transform is 16 floats and nothing here uses more.
inline constexpr uint32_t kMaxTrackedUniforms = 64;

class Context {
public:
	explicit Context(CmdStream &cs)
		: cs_{&cs}
	{}

	// Forget everything; the next draw re-emits the whole pipe. Use after
	// anything else has driven the 3D registers.
	void invalidate()
	{
		dirty_ = DirtyAll;
	}

	// Emit one draw, only touching state that changed. Returns false without
	// emitting anything if the command stream lacks room for a worst-case draw
	// (the caller should drain, submit, and start a new stream).
	bool draw(const MeshDraw &d);

	// The per-submission PE drain: stall, flush the color/depth caches to DDR,
	// stall again. Call once after the last draw, before submit().
	void drain();

	uint32_t draw_count() const
	{
		return draws_;
	}
	// Dwords the most recent draw() emitted -- the delta metric.
	uint32_t last_draw_dwords() const
	{
		return last_dwords_;
	}

private:
	// The last-emitted state, kept as plain values rather than a copy of the
	// MeshDraw: a MeshDraw holds Bo POINTERS and a uniforms span, and a Context
	// can outlive the caller's structs. Comparing stored addresses never
	// dereferences anything that may have gone away.
	struct Tracked {
		uint32_t rt = 0, rt_stride = 0, depth = 0, depth_stride = 0;
		uint32_t width = 0, height = 0;
		uint32_t alpha_config = 0, color_format = 0;
		uint32_t depth_config = 0;
		uint32_t pa_config = 0, line_width = 0, point_size = 0; // widths as fui() bits
		uint32_t sc_minx = 0, sc_miny = 0, sc_maxx = 0, sc_maxy = 0;
		uint32_t vs = 0, vs_words = 0, vs_temps = 0;
		uint32_t ps = 0, ps_words = 0, ps_temps = 0, ps_out_reg = 0;
		uint32_t vtx = 0, vtx_stride = 0;
	};

	static Tracked snapshot(const MeshDraw &d);
	uint32_t compute_dirty(const Tracked &t, const MeshDraw &d) const;

	CmdStream *cs_;
	Tracked cur_{};
	std::array<float, kMaxTrackedUniforms> uniforms_{};
	uint32_t uniform_count_ = 0;
	bool uniforms_untracked_ = false; // more uniforms than we can mirror
	uint32_t dirty_ = DirtyAll;
	uint32_t draws_ = 0;
	uint32_t last_dwords_ = 0;
};

} // namespace etna
