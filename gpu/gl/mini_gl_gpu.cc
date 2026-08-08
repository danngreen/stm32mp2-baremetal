#include "mini_gl_gpu.hh"
#include "../cube_scene.hh" // kVsColorCode-equivalent pass-through shaders
#include "mini_gl.hh"
#include "print/print.hh"
#include <algorithm>
#include <new>

// =============================================================================
//  mini_gl_gpu.cc -- BatchState -> MeshDraw
// =============================================================================
//
// Two coordinate gotchas live here, both flips, and they are NOT the same flip:
//
//  1. Vertex Y. Already applied by mini_gl.cc before the vertices reach us.
//
//  2. Scissor Y. glScissor's rectangle is measured from the BOTTOM-left of the
//     window, while etna::Scissor is measured in framebuffer rows from the top.
//     So a GL box (x, y, w, h) becomes rows [H - (y + h), H - y). Getting this
//     wrong puts the clip rectangle in the mirrored place -- which looks
//     plausible for a centred box and obviously wrong for anything else.

namespace mgl
{
namespace
{

// The pass-through VS/FS: position straight through, colour as a varying. Since
// mini-GL transforms on the CPU there is no matrix uniform, which is exactly
// what keeps the shader state constant and the batch stall-free.
constexpr std::array<uint32_t, 8> kVsPassthrough = {
	0x07821009, 0x00000000, 0x00000000, 0x00390008, // MOV t2, t0  (position)
	0x07831009, 0x00000000, 0x00000000, 0x00390018, // MOV t3, t1  (colour -> varying)
};
constexpr std::array<uint32_t, 4> kPsPassthrough = {0x07811009, 0, 0, 0x00390018}; // MOV t1, t1

etna::Primitive to_prim(Prim p)
{
	switch (p) {
		case Prim::Lines:
			return etna::Primitive::Lines;
		case Prim::Points:
			return etna::Primitive::Points;
		default:
			return etna::Primitive::Triangles;
	}
}

// GL blend factor -> hardware factor. The constant-colour factors are not
// implemented (see gpu_regs_3d.hh), so they fall back to the nearest safe
// thing rather than emitting an unverified encoding.
etna::BlendFactor to_factor(uint32_t glf)
{
	switch (glf) {
		case GL_ZERO:
			return etna::BlendFactor::Zero;
		case GL_ONE:
			return etna::BlendFactor::One;
		case GL_SRC_COLOR:
			return etna::BlendFactor::SrcColor;
		case GL_ONE_MINUS_SRC_COLOR:
			return etna::BlendFactor::OneMinusSrcColor;
		case GL_SRC_ALPHA:
			return etna::BlendFactor::SrcAlpha;
		case GL_ONE_MINUS_SRC_ALPHA:
			return etna::BlendFactor::OneMinusSrcAlpha;
		case GL_DST_ALPHA:
			return etna::BlendFactor::DstAlpha;
		case GL_ONE_MINUS_DST_ALPHA:
			return etna::BlendFactor::OneMinusDstAlpha;
		case GL_DST_COLOR:
			return etna::BlendFactor::DstColor;
		case GL_ONE_MINUS_DST_COLOR:
			return etna::BlendFactor::OneMinusDstColor;
		case GL_SRC_ALPHA_SATURATE:
			return etna::BlendFactor::SrcAlphaSaturate;
		default:
			return etna::BlendFactor::One;
	}
}

etna::BlendEq to_eq(uint32_t gle)
{
	switch (gle) {
		case GL_FUNC_SUBTRACT:
			return etna::BlendEq::Subtract;
		case GL_FUNC_REVERSE_SUBTRACT:
			return etna::BlendEq::ReverseSubtract;
		case GL_MIN:
			return etna::BlendEq::Min;
		case GL_MAX:
			return etna::BlendEq::Max;
		default:
			return etna::BlendEq::Add;
	}
}

// GL compare enums are 0x0200 + n in exactly the hardware's order.
etna::CompareFunc to_compare(uint32_t glf)
{
	if (glf < GL_NEVER || glf > GL_ALWAYS)
		return etna::CompareFunc::Less;
	return static_cast<etna::CompareFunc>(glf - GL_NEVER);
}

} // namespace

bool GpuBackend::init(etna::Gpu &gpu, uint32_t w, uint32_t h, bool with_depth, uint32_t arena_bytes,
					  uint32_t stream_words)
{
	gpu_ = &gpu;
	w_ = w;
	h_ = h;
	pw_ = (w + 15) & ~15u;
	ph_ = (h + 3) & ~3u;
	has_depth_ = with_depth;
	stream_words_ = stream_words;

	rt_ = gpu.alloc(pw_ * 4 * ph_);
	fb_ = gpu.alloc(w_ * 4 * h_);
	vs_ = gpu.alloc(sizeof(kVsPassthrough));
	ps_ = gpu.alloc(sizeof(kPsPassthrough));
	if (with_depth)
		depth_ = gpu.alloc(pw_ * 2 * ph_);
	if (!rt_ || !fb_ || !vs_ || !ps_ || (with_depth && !depth_))
		return false;
	if (!arena_.init(gpu, arena_bytes))
		return false;

	std::ranges::copy(kVsPassthrough, vs_.span<uint32_t>().begin());
	vs_.cpu_fini(etna::RelocWrite);
	std::ranges::copy(kPsPassthrough, ps_.span<uint32_t>().begin());
	ps_.cpu_fini(etna::RelocWrite);

	// ONE command stream for the backend's whole life, reset() between uses.
	// The GPU pool is a bump allocator with no free, so allocating a stream
	// per frame (as this originally did) exhausts the pool in ~4000 frames --
	// about 70 seconds of a 58 fps sketch. submit_and_wait() copies the stream
	// into the ring, so the backing Bo is reusable the moment it returns.
	cs_ = new (cs_storage_) etna::CmdStream(gpu.new_cmd_stream(stream_words));
	if (!cs_->bo())
		return false;
	return true;
}

void GpuBackend::begin_frame()
{
	arena_.reset();
	draws_ = 0;
	stream_dwords_ = 0;
	overflow_ = false;
	cs_->reset();
	if (ctx_)
		ctx_->~Context();
	ctx_ = new (ctx_storage_) etna::Context(*cs_);
}

bool GpuBackend::flush_stream()
{
	if (!ctx_)
		return false;
	ctx_->drain();
	stream_dwords_ += cs_->offset();
	const bool ok = gpu_->submit_and_wait(*cs_);
	if (!ok)
		gpu_->dump_status("mini-gl submit");
	ctx_->~Context();
	// Same stream, reset; the pipe state is unchanged, but the new Context has
	// not emitted anything yet, so it re-emits the full state on its next draw.
	cs_->reset();
	ctx_ = new (ctx_storage_) etna::Context(*cs_);
	return ok;
}

void GpuBackend::clear(uint32_t mask, float r, float g, float b, float a, float)
{
	if (!ctx_)
		return;

	// The RS clear brings its own PE drain, so anything already batched must be
	// submitted first -- otherwise the clear could land on top of it.
	if (draws_ > 0)
		flush_stream();

	if (mask & GL_COLOR_BUFFER_BIT) {
		auto u8 = [](float v) { return uint32_t(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f) & 0xFFu; };
		const uint32_t argb = (u8(a) << 24) | (u8(r) << 16) | (u8(g) << 8) | u8(b);
		// A solid colour is tiling-invariant, so filling the padded extent of
		// the tiled target as if it were linear is correct.
		etna::clear(*cs_, rt_, pw_, ph_, argb);
		gpu_->submit_and_wait(*cs_);
		cs_->reset();
	}

	if ((mask & GL_DEPTH_BUFFER_BIT) && has_depth_) {
		// Reset to far. Viewed as 32-bit pixels a D16 row is pw_/2 wide; the RS
		// needs a multiple of 16, so fall back to a CPU fill when it is not.
		const uint32_t w32 = pw_ / 2;
		if (w32 % 16 == 0) {
			etna::clear(*cs_, depth_, w32, ph_, 0xFFFFFFFF);
			gpu_->submit_and_wait(*cs_);
			cs_->reset();
		} else {
			std::ranges::fill(depth_.span<uint16_t>(), uint16_t(0xFFFF));
			depth_.cpu_fini(etna::RelocWrite);
		}
	}

	// The RS ops above do not touch 3D state, but this Context has not emitted
	// anything into the fresh stream, so make the next draw emit in full.
	ctx_->invalidate();
}

void GpuBackend::draw(const BatchState &s, std::span<const float> verts, uint32_t vertex_count)
{
	if (!ctx_ || vertex_count == 0)
		return;

	etna::Bo vb = arena_.upload(verts);
	if (!vb) {
		// Out of vertex memory: submitting what we have frees the arena, since
		// the fence retires with it.
		flush_stream();
		arena_.reset();
		vb = arena_.upload(verts);
		if (!vb) {
			overflow_ = true;
			return;
		}
	}

	etna::MeshDraw d{};
	d.rt = &rt_;
	d.rt_stride = pw_ * 4;
	d.vtx = &vb;
	d.vtx_stride = kFloatsPerVertex * 4;
	d.pos_components = 4; // clip-space xyzw; the GPU does the perspective divide
	d.vs = &vs_;
	d.vs_words = kVsPassthrough.size();
	d.vs_temps = 4;
	d.ps = &ps_;
	d.ps_words = kPsPassthrough.size();
	d.ps_temps = 2;
	d.ps_out_reg = 1;
	d.width = w_;
	d.height = h_;
	// GL clip z is [-1,1]; the depth buffer wants window z in [0,1]. The PA
	// applies this after the perspective divide, so it is the glDepthRange(0,1)
	// mapping, correct for ortho and frustum alike.
	d.vp_scale_z = 0.5f;
	d.vp_offset_z = 0.5f;
	d.vertex_count = vertex_count;
	d.prim = to_prim(s.prim);
	d.line_width = s.line_width;
	d.point_size = s.point_size;

	if (has_depth_) {
		d.depth = &depth_;
		d.depth_stride = pw_ * 2;
		d.depth_state = {.test = s.depth_test, .write = s.depth_write, .func = to_compare(s.depth_func)};
	}

	if (s.blend) {
		const etna::BlendFactor sf = to_factor(s.blend_src), df = to_factor(s.blend_dst);
		const etna::BlendEq eq = to_eq(s.blend_eq);
		d.blend = {.enable = true,
				   .rgb_src = sf,
				   .rgb_dst = df,
				   .rgb_eq = eq,
				   .alpha_src = sf,
				   .alpha_dst = df,
				   .alpha_eq = eq};
	}

	if (s.cull && s.cull_face != GL_FRONT_AND_BACK) {
		d.cull = (s.cull_face == GL_FRONT) ? etna::CullMode::Front : etna::CullMode::Back;
		d.front_face = (s.front_face == GL_CW) ? etna::FrontFace::CW : etna::FrontFace::CCW;
	}

	if (s.scissor) {
		// THE SCISSOR FLIP -- see the file header. GL measures y from the
		// bottom; etna::Scissor counts framebuffer rows from the top.
		const int32_t gl_top = static_cast<int32_t>(s.scissor_y) + static_cast<int32_t>(s.scissor_h);
		const int32_t row_min = static_cast<int32_t>(h_) - gl_top;
		const int32_t row_max = static_cast<int32_t>(h_) - static_cast<int32_t>(s.scissor_y);
		d.scissor = {.enable = true,
					 .minx = static_cast<uint32_t>(std::max(0, s.scissor_x)),
					 .miny = static_cast<uint32_t>(std::max(0, row_min)),
					 .maxx = static_cast<uint32_t>(std::max(0, s.scissor_x + int32_t(s.scissor_w))),
					 .maxy = static_cast<uint32_t>(std::max(0, row_max))};
	}

	if (!ctx_->draw(d)) {
		// The stream is full; submit and retry once in a fresh one.
		flush_stream();
		if (!ctx_->draw(d))
			overflow_ = true;
	}
	draws_++;
}

void GpuBackend::end_frame()
{
	if (!ctx_)
		return;

	if (draws_ > 0)
		flush_stream();
	else
		stream_dwords_ += cs_->offset();

	// Untile into the linear framebuffer a display controller can scan out --
	// either the internal one or, when set_scanout() was called, an external
	// (e.g. LTDC back) buffer.
	etna::Bo &dst = scanout_ ? *scanout_ : fb_;
	const uint32_t dst_stride = (scanout_ && scanout_stride_) ? scanout_stride_ : w_ * 4;
	cs_->reset();
	etna::resolve(*cs_, dst, rt_, w_, h_, pw_ * 4, dst_stride);
	if (!gpu_->submit_and_wait(*cs_))
		gpu_->dump_status("mini-gl resolve");

	// cs_ lives on (see init); only the Context is per-frame.
	ctx_->~Context();
	ctx_ = nullptr;
}

} // namespace mgl
