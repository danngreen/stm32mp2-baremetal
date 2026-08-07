#pragma once
#include "gpu_regs_3d.hh"
#include <cstdint>

// =============================================================================
//  etna_depth.hh -- glDepthFunc / glDepthMask -> PE_DEPTH_CONFIG
// =============================================================================
//
// Ported from Mesa's etna_update_zsa() (etnaviv_state.c), restricted to what we
// have: a D16 depth buffer, no stencil, and always LATE depth (no early-Z).
//
// PE_DEPTH_CONFIG is assembled from two independent groups:
//   - framebuffer-derived : DEPTH_MODE, DEPTH_FORMAT, UNK18
//   - depth-state-derived : DEPTH_FUNC, WRITE_ENABLE, EARLY_Z, DISABLE_ZS
// which is why a "depth off" draw and a "depth buffer present but test off"
// draw are different words -- the first has DEPTH_MODE = NONE.
//
// DISABLE_ZS turns off the late depth/stencil stage entirely. It must be set
// when nothing needs the late stage (no test AND no write), and per rnndb it
// must also be set if early depth writes are ever enabled, "otherwise the GPU
// hangs". We never enable early-Z, so the second case cannot arise here.
//
// Early-Z is deliberately left off. Mesa only enables it under a pile of
// conditions (RA_WRITE_DEPTH feature, no alpha test, shader doesn't write Z or
// discard, render target not linear), and our RA_EARLY_DEPTH value is the
// late-Z one the depth test was verified with. Revisit only with a hardware
// test; a wrong early/late split is exactly the "GPU hangs" case above.

namespace etna
{

// Same numbering as glDepthFunc order and Gallium's PIPE_FUNC (rnndb notes the
// two coincide), so no translation table.
enum class CompareFunc : uint32_t {
	Never = 0,
	Less = 1,
	Equal = 2,
	LEqual = 3,
	Greater = 4,
	NotEqual = 5,
	GEqual = 6,
	Always = 7,
};

struct DepthState {
	bool test = false;						 // glEnable(GL_DEPTH_TEST)
	bool write = false;						 // glDepthMask
	CompareFunc func = CompareFunc::Less; // glDepthFunc

	// The word to emit when a D16 depth buffer IS bound.
	constexpr uint32_t pe_depth_config() const
	{
		using namespace VivanteGpu;
		// A disabled test still runs the stage with ALWAYS, exactly as Mesa does
		// -- that keeps depth *writes* working with the test off.
		const uint32_t f = static_cast<uint32_t>(test ? func : CompareFunc::Always);
		return PE_DEPTH_CONFIG_MODE_Z | PE_DEPTH_CONFIG_UNK18 | (f << PE_DEPTH_CONFIG_FUNC_SHIFT) |
			   (write ? PE_DEPTH_CONFIG_WRITE_ENABLE : 0u) |
			   // Nothing for the late stage to do -> switch it off.
			   ((!test && !write) ? PE_DEPTH_CONFIG_DISABLE_ZS : 0u);
	}
};

// The classic depth test: LESS with writes on.
inline constexpr DepthState kDepthLessWrite{.test = true, .write = true, .func = CompareFunc::Less};

// Depth-tested but read-only -- what a GL front end uses for transparent
// geometry, so blended fragments occlude correctly without polluting the buffer.
inline constexpr DepthState kDepthTestNoWrite{.test = true, .write = false, .func = CompareFunc::Less};

// --- compile-time proofs ------------------------------------------------------
// The LESS+write state must reproduce the exact constant the depth and cube
// tests were verified with, so adding depth-func control changes nothing.
static_assert(kDepthLessWrite.pe_depth_config() == VivanteGpu::PE_DEPTH_CONFIG_D16_LESS_WRITE);
// Read-only depth just drops WRITE_ENABLE.
static_assert(kDepthTestNoWrite.pe_depth_config() ==
			  (VivanteGpu::PE_DEPTH_CONFIG_D16_LESS_WRITE & ~VivanteGpu::PE_DEPTH_CONFIG_WRITE_ENABLE));
// Neither test nor write: ALWAYS, and the late stage is disabled.
static_assert(DepthState{}.pe_depth_config() ==
			  (VivanteGpu::PE_DEPTH_CONFIG_MODE_Z | VivanteGpu::PE_DEPTH_CONFIG_UNK18 |
			   (7u << VivanteGpu::PE_DEPTH_CONFIG_FUNC_SHIFT) | VivanteGpu::PE_DEPTH_CONFIG_DISABLE_ZS));
// Write-only (test off) keeps the stage alive with ALWAYS -- a depth prepass.
static_assert((DepthState{.write = true}.pe_depth_config() & VivanteGpu::PE_DEPTH_CONFIG_DISABLE_ZS) == 0);
static_assert((DepthState{.write = true}.pe_depth_config() >> VivanteGpu::PE_DEPTH_CONFIG_FUNC_SHIFT & 7) == 7);
// GREATER lands in the right field (used by the reverse-order sanity draw).
static_assert((DepthState{.test = true, .func = CompareFunc::Greater}.pe_depth_config() >>
			   VivanteGpu::PE_DEPTH_CONFIG_FUNC_SHIFT & 7) == 4);

} // namespace etna
