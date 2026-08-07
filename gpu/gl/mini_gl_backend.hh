#pragma once
#include <cstdint>
#include <span>

// =============================================================================
//  mini_gl_backend.hh -- the seam under mini-GL
// =============================================================================
//
// mini-GL does all the fixed-function work on the CPU -- matrix stack, vertex
// transform, lighting, primitive conversion, batching -- and hands the backend
// nothing but "here is a run of already-transformed vertices, with this state".
// Everything above this line is pure computation and runs (and is tested) on
// the host; everything below it talks to hardware.
//
// The two implementations are:
//   GpuBackend   -- etna::Context + MeshDraw (mini_gl_gpu.cc, target only)
//   RecordBackend -- appends to a vector for the host tests to assert on
//
// VERTEX FORMAT matches emit_mesh's fixed layout exactly: 7 floats per vertex,
// pos xyz then colour rgba, so a batch can be handed to the GPU with no repack.
// Positions are CLIP SPACE (the CPU already applied modelview+projection), and
// the Y flip described in mini_gl.cc has already been applied.

namespace mgl
{

inline constexpr uint32_t kFloatsPerVertex = 7;

// The primitive classes a backend must draw. mini-GL converts every glBegin
// mode down to one of these three on the CPU, which is what lets consecutive
// glBegin/glEnd pairs merge into a single draw.
enum class Prim : uint32_t {
	Triangles,
	Lines,
	Points,
};

// The pipeline state a batch is drawn under. A change to any field forces the
// current batch to flush, so keep it to things that genuinely must differ.
struct BatchState {
	Prim prim = Prim::Triangles;

	bool blend = false;
	uint32_t blend_src = 0, blend_dst = 0; // GL factor enums
	uint32_t blend_eq = 0;				   // GL equation enum

	bool depth_test = false;
	bool depth_write = true;
	uint32_t depth_func = 0; // GL compare enum

	bool cull = false;
	uint32_t cull_face = 0;	 // GL_FRONT / GL_BACK / GL_FRONT_AND_BACK
	uint32_t front_face = 0; // GL_CW / GL_CCW

	bool scissor = false;
	int32_t scissor_x = 0, scissor_y = 0;
	uint32_t scissor_w = 0, scissor_h = 0;

	float line_width = 1.0f;
	float point_size = 1.0f;

	bool operator==(const BatchState &) const = default;
};

class Backend {
public:
	virtual ~Backend() = default;

	// Start a frame: nothing has been drawn yet.
	virtual void begin_frame() {}

	// glClear. `mask` carries the GL_*_BUFFER_BIT flags mini-GL saw.
	virtual void clear(uint32_t mask, float r, float g, float b, float a, float depth) = 0;

	// Draw one batch. `verts` is vertex_count * kFloatsPerVertex floats.
	virtual void draw(const BatchState &state, std::span<const float> verts, uint32_t vertex_count) = 0;

	// Finish the frame: flush everything to the render target.
	virtual void end_frame() {}

	// Target size in pixels, used for the viewport and for resolving scissor.
	virtual uint32_t width() const = 0;
	virtual uint32_t height() const = 0;
};

} // namespace mgl
