#pragma once
#include "../etna.hh"
#include "../etna_arena.hh"
#include "../etna_context.hh"
#include "mini_gl_backend.hh"

// =============================================================================
//  mini_gl_gpu.hh -- the mini-GL backend that drives the etna 3D pipe
// =============================================================================
//
// Owns a tiled render target plus an optional D16 depth buffer, batches every
// draw of a frame into ONE command stream through etna::Context, and resolves
// the result into a linear framebuffer that LTDC can scan out.
//
// The frame shape is:
//     begin_frame()  -- open a command stream and a Context
//     clear()/draw() -- appended to that one stream
//     end_frame()    -- drain, submit once, wait, RS-resolve to framebuffer()
//
// Vertices go into an Arena rather than a reused buffer: the FE reads vertex
// memory asynchronously until the fence, so every draw in the batch needs its
// own slice (see etna_arena.hh).

namespace mgl
{

class GpuBackend : public Backend {
public:
	// `w` x `h` is the visible size. `with_depth` adds a D16 buffer; without
	// one, GL_DEPTH_TEST is silently inert (as it would be with no depth
	// attachment in real GL).
	bool init(etna::Gpu &gpu, uint32_t w, uint32_t h, bool with_depth = true,
			  uint32_t arena_bytes = 512 * 1024, uint32_t stream_words = 4032);

	void begin_frame() override;
	void clear(uint32_t mask, float r, float g, float b, float a, float depth) override;
	void draw(const BatchState &state, std::span<const float> verts, uint32_t vertex_count) override;
	void end_frame() override;

	uint32_t width() const override
	{
		return w_;
	}
	uint32_t height() const override
	{
		return h_;
	}

	// The linear, resolved image -- what a display controller scans out.
	const etna::Bo &framebuffer() const
	{
		return fb_;
	}
	uint32_t framebuffer_stride() const
	{
		return w_ * 4;
	}

	// Resolve the NEXT end_frame() into an external linear buffer instead of
	// framebuffer() -- for double-buffered scanout, point this at the back
	// buffer each frame. `stride` 0 means w*4; nullptr reverts to the internal
	// buffer. The Bo must outlive the frame (the GPU reads/writes it async).
	void set_scanout(etna::Bo *fb, uint32_t stride = 0)
	{
		scanout_ = fb;
		scanout_stride_ = stride;
	}

	// Diagnostics for the frame just ended.
	uint32_t draws_submitted() const
	{
		return draws_;
	}
	uint32_t stream_dwords() const
	{
		return stream_dwords_;
	}
	bool overflowed() const
	{
		return overflow_;
	}

private:
	// Flush the current stream and start a new one -- used when a frame needs
	// more command space than one stream (or one ring block) can hold.
	bool flush_stream();

	etna::Gpu *gpu_ = nullptr;
	uint32_t w_ = 0, h_ = 0;
	uint32_t pw_ = 0, ph_ = 0; // tiled RT padding (w->16, h->4)
	bool has_depth_ = false;

	etna::Bo rt_{}, depth_{}, fb_{}, vs_{}, ps_{};
	etna::Bo *scanout_ = nullptr;
	uint32_t scanout_stride_ = 0;
	etna::Arena arena_{};
	uint32_t stream_words_ = 0;

	// cs_ is constructed once in init() and reset() between uses -- the GPU
	// pool cannot free, so per-frame stream allocation would leak it dry.
	// ctx_ is live only between begin_frame() and end_frame().
	etna::CmdStream *cs_ = nullptr;
	etna::Context *ctx_ = nullptr;
	alignas(etna::CmdStream) unsigned char cs_storage_[sizeof(etna::CmdStream)]{};
	alignas(etna::Context) unsigned char ctx_storage_[sizeof(etna::Context)]{};

	uint32_t draws_ = 0;
	uint32_t stream_dwords_ = 0;
	bool overflow_ = false;
};

} // namespace mgl
