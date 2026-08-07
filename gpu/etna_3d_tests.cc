#include "aarch64/system_reg.hh" // read_cntpct
#include "cube_cpu_render.hh"
#include "cube_scene.hh"
#include "etna.hh"
#include "etna_3d.hh"
#include "etna_arena.hh"
#include "etna_context.hh"
#include "print/print.hh"
#include <algorithm>
#include <array>

using namespace VivanteGpu;
using namespace etna;

static bool verify_shape(std::span<const uint32_t> img,
						 uint32_t w,
						 uint32_t h,
						 std::span<const float, 6> ndc,
						 uint32_t inside,
						 uint32_t outside);

// -------------------------------------------------------
// Shaders
// -------------------------------------------------------

// Trivial HALTI5 shaders (Vivante ISA, 1 instruction = 4 dwords each).
// VS: MOV t1, t0  -- pass the position attribute (lands in t0) straight through
//     as the clip-space position; vs_pos_out_reg = 1.
constexpr std::array<uint32_t, 4> kVsCode = {0x07811009, 0x00000000, 0x00000000, 0x00390008};
// FS: MOV t1, u0  -- output the constant color from uniform u0; ps_color_out = 1.
constexpr std::array<uint32_t, 4> kPsCode = {0x07811009, 0x00000000, 0x00000000, 0x20390008};

// Per-vertex-color shaders. Same MOV encoding, decoded from the two above:
//   opcode MOV = 0x09 (word0[5:0]); DST_REG = word0[22:16]; DST_COMPS=0xF (xyzw);
//   MOV reads SRC2: SRC2_REG = word3[12:4], SRC2_RGROUP = word3[30:28]
//   (0 = temp, 2 = uniform -- that lone bit is the only diff between kVsCode and
//   kPsCode above). So word0 = 0x07801009 | (dst<<16), word3 = 0x00390008 |
//   (src<<4) [| 0x20000000 for a uniform].
// VS: pass position (in t0 -> out t2) and color (in t1 -> out t3, a varying).
constexpr std::array<uint32_t, 8> kVsColorCode = {
	// clang-format off
	0x07821009, 0x00000000, 0x00000000, 0x00390008, // MOV t2, t0  (position)
	0x07831009, 0x00000000, 0x00000000, 0x00390018, // MOV t3, t1  (color -> varying)
	// clang-format on
};
// FS: output the interpolated color varying (rasterizer lands it in t1); out t1.
constexpr std::array<uint32_t, 4> kPsColorCode = {0x07811009, 0x00000000, 0x00000000, 0x00390018}; // MOV t1, t1

// Texture-sampling FS: TEXLD t2, tex0, t1 -- sample sampler 0 at the UV varying
// (interpolated into t1, .xy used) and write RGBA to t2; ps_color_out_reg = 2.
// TEXLD = opcode 0x18 (isa: pattern 011000, OPCODE_BIT6=0). Encoding verified
// against the isa bitset "#instruction-tex":
//   word0 = 0x18 | DST_USE(1<<12) | DST_REG<<16 | COMPS(0xF<<23) | TEX_ID<<27
//   word1 = TEX_SWIZ(0xE4)<<3 | SRC0_USE(1<<11) | SRC0_REG<<12 | SRC0_SWIZ(0xE4)<<22
//   words 2,3 = 0 (no src1/src2; FS TEXLD needs no explicit LOD -- implicit
//   derivatives, and MIP=NONE/MAXLOD=0 pins level 0)
constexpr std::array<uint32_t, 4> kPsTexCode = {0x07821018, 0x39001F20, 0x00000000, 0x00000000};

// =============================================================================
// Basic drawing test
// =============================================================================
//
// Basic drawing test: clear a small tiled RT to blue, draw a screen-covering red triangle,
// read the RT back directly (solid color -> tiling-invariant) and report how
// many pixels changed from clear -> triangle color.
// Then, RS-resolve the tiled RT to a linear buffer and verify the
// triangle's actual shape -- pixel positions, not just counts.
bool triangle_test(Gpu &gpu)
{
	constexpr uint32_t W = 64, H = 64;
	constexpr uint32_t pw = (W + 15) & ~15u; // pad width to 16
	constexpr uint32_t ph = (H + 3) & ~3u;	 // pad height to 4
	constexpr uint32_t stride = pw * 4;
	constexpr uint32_t rt_size = stride * ph;
	constexpr uint32_t CLEAR = 0xFF0000FF; // blue (B,G,R,A = FF,00,00,FF)

	Bo rt = gpu.alloc(rt_size);
	Bo vtx = gpu.alloc(3 * 3 * 4); // 3 verts x vec3 float
	Bo vs = gpu.alloc(sizeof(kVsCode));
	Bo ps = gpu.alloc(sizeof(kPsCode));
	if (!rt || !vtx || !vs || !ps)
		return false;

	// clear RT (solid -> same in every byte regardless of tiling)
	auto rp = rt.span<uint32_t>();
	std::ranges::fill(rp, CLEAR);
	rt.cpu_fini(RelocWrite);

	// a triangle fully INSIDE the clip volume (no fullscreen/guardband trick),
	// z = 0.5 (mid depth range, off the near plane). Covers most of the screen.
	const std::array<float, 9> verts = {-0.8f, -0.8f, 0.5f, 0.8f, -0.8f, 0.5f, 0.0f, 0.8f, 0.5f};
	std::ranges::copy(verts, vtx.span<float>().begin());
	vtx.cpu_fini(RelocWrite);

	std::ranges::copy(kVsCode, vs.span<uint32_t>().begin());
	vs.cpu_fini(RelocWrite);

	std::ranges::copy(kPsCode, ps.span<uint32_t>().begin());
	ps.cpu_fini(RelocWrite);

	const std::array<float, 4> red = {1.0f, 0.0f, 0.0f, 1.0f};

	auto cs = gpu.new_cmd_stream(1024);
	emit_triangle(cs, rt, stride, vtx, 12, vs, ps, W, H, red, 3);

	auto start = read_cntpct();
	if (!gpu.submit_and_wait(cs)) {
		gpu.dump_status("triangle draw");
		return false;
	}
	print("3D triangle drawn in ", (read_cntpct() - start), " ticks\n");

	rt.cpu_prep(RelocRead);
	// The triangle is a solid color, so it reads back the same in the tiled RT
	// regardless of layout. Count changed pixels and confirm they're one uniform
	// non-clear color (= the FS constant reached the render target).
	uint32_t drawn = 0, sample = 0;
	bool uniform = true;
	for (uint32_t i = 0; i < rt_size / 4; i++) {
		uint32_t p = rp[i];
		if (p != CLEAR) {
			if (!drawn)
				sample = p;
			else if (p != sample)
				uniform = false;
			drawn++;
		}
	}
	print("RT: ", drawn, " of ", int(rt_size / 4), " pixels drawn, color 0x", Hex{sample});
	print(uniform ? " (uniform)\n" : " (NOT uniform!)\n");
	if (drawn == 0) {
		print("FAILED: the draw wrote no pixels (pipe/state issue)\n");
		return false;
	}
	if (!uniform || sample == 0) {
		print("FAILED: triangle pixels are not a single non-zero color\n");
		return false;
	}
	print("GPU drew a solid triangle in 0x", Hex{sample}, " -- 3D pipe verified. \\o/\n");

	// RS resolve (untile), then verify the shape
	// The resolve makes pixel (x,y) addressable at y*W + x, so we can finally
	// check where the fragments landed: strictly inside the expected window-
	// space triangle must be the draw color, strictly outside must be clear.
	Bo lin = gpu.alloc(W * H * 4);
	if (!lin)
		return false;
	std::ranges::fill(lin.span<uint32_t>(), 0xDEADBEEFu); // poison: resolve must overwrite
	lin.cpu_fini(RelocWrite);

	auto cs2 = gpu.new_cmd_stream(256);
	resolve(cs2, lin, rt, W, H, stride, W * 4);
	auto rstart = read_cntpct();
	if (!gpu.submit_and_wait(cs2)) {
		gpu.dump_status("RS resolve");
		return false;
	}
	print("RS resolve (untile ", W, "x", H, ") in ", (uint32_t)(read_cntpct() - rstart), " ticks\n");

	lin.cpu_prep(RelocRead);
	const std::array<float, 6> ndc_xy = {verts[0], verts[1], verts[3], verts[4], verts[6], verts[7]};
	if (!verify_shape(lin.span<uint32_t>(), W, H, ndc_xy, sample, CLEAR))
		return false;
	print("resolved image matches the expected triangle -- shape verified. \\o/\n");
	return true;
}

// =============================================================================
//  Color test
// =============================================================================
//
// Draw an RGB-gradient triangle (red/green/blue vertices) and confirm the
// varying interpolated: the RT is tiled, but "did a red-dominant, a
// green-dominant, and a blue-dominant fragment all appear?" is tiling-invariant,
// so we can verify per-vertex interpolation without an RS untile.
bool triangle_color_test(Gpu &gpu)
{
	constexpr uint32_t W = 64, H = 64;
	constexpr uint32_t pw = (W + 15) & ~15u;
	constexpr uint32_t ph = (H + 3) & ~3u;
	constexpr uint32_t stride = pw * 4;
	constexpr uint32_t rt_size = stride * ph;
	// Clear to opaque black. The gradient's colors all satisfy R+G+B ~= 255
	// (barycentric blend of the three corners), so no drawn pixel is black --
	// black is a safe "not drawn" sentinel (unlike blue, which collides with a
	// corner color).
	constexpr uint32_t CLEAR = 0xFF000000;

	Bo rt = gpu.alloc(rt_size);
	Bo vtx = gpu.alloc(3 * 7 * 4); // 3 verts x (vec3 pos + vec4 color)
	Bo vs = gpu.alloc(sizeof(kVsColorCode));
	Bo ps = gpu.alloc(sizeof(kPsColorCode));
	if (!rt || !vtx || !vs || !ps)
		return false;

	auto rp = rt.span<uint32_t>();
	std::ranges::fill(rp, CLEAR);
	rt.cpu_fini(RelocWrite);

	// interleaved pos.xyz + color.rgba, 7 floats/vertex: red, green, blue corners
	const std::array<float, 3 * 7> verts = {
		-0.8f, -0.8f, 0.5f, 1.0f, 0.0f, 0.0f, 1.0f, // v0 red
		0.8f,  -0.8f, 0.5f, 0.0f, 1.0f, 0.0f, 1.0f, // v1 green
		0.0f,  0.8f,  0.5f, 0.0f, 0.0f, 1.0f, 1.0f, // v2 blue
	};
	std::ranges::copy(verts, vtx.span<float>().begin());
	vtx.cpu_fini(RelocWrite);

	std::ranges::copy(kVsColorCode, vs.span<uint32_t>().begin());
	vs.cpu_fini(RelocWrite);

	std::ranges::copy(kPsColorCode, ps.span<uint32_t>().begin());
	ps.cpu_fini(RelocWrite);

	auto cs = gpu.new_cmd_stream(1024);
	emit_triangle_color(cs, rt, stride, vtx, 28, vs, ps, W, H, 3);

	auto start = read_cntpct();
	if (!gpu.submit_and_wait(cs)) {
		gpu.dump_status("triangle color draw");
		return false;
	}
	print("gradient triangle drawn in ", (uint32_t)(read_cntpct() - start), " ticks\n");

	rt.cpu_prep(RelocRead);
	uint32_t drawn = 0, first = 0;
	bool uniform = true, sawR = false, sawG = false, sawB = false;
	for (uint32_t i = 0; i < rt_size / 4; i++) {
		uint32_t p = rp[i];
		if (p == CLEAR)
			continue;
		if (!drawn)
			first = p;
		else if (p != first)
			uniform = false;
		drawn++;
		uint32_t R = (p >> 16) & 0xFF, G = (p >> 8) & 0xFF, B = p & 0xFF;
		if (R > 100 && R > G && R > B)
			sawR = true;
		if (G > 100 && G > R && G > B)
			sawG = true;
		if (B > 100 && B > R && B > G)
			sawB = true;
	}
	print("RT: ", drawn, " of ", int(rt_size / 4), " pixels drawn.");
	print(" corners seen R=", sawR, " G=", sawG, " B=", sawB, uniform ? " (uniform!)\n" : " (varied)\n");
	if (drawn == 0) {
		print("FAILED: the draw wrote no pixels\n");
		return false;
	}
	if (uniform) {
		print("FAILED: triangle is a single color -- varying did not interpolate\n");
		return false;
	}
	if (!(sawR && sawG && sawB)) {
		print("FAILED: not all three vertex colors reached fragments\n");
		return false;
	}
	print("GPU interpolated a per-vertex-color varying across the triangle. \\o/\n");
	return true;
}

// =============================================================================
//  Depth test: the nearer triangle occludes the farther one
// =============================================================================
//
// Draw the two triangles with the same coordinates and same center, but one is
// flipped over the X axis. Do this into one RT sharing a D16 depth buffer, in ONE
// command stream (so the PE's depth cache stays hot and draw 2 sees draw 1's
// depth).
// Draw the near triangle (z=0.3) in red first, then far tri (z=0.7) in green.
// Window depth = 0.5*z + 0.5, so near->0.65, far->0.85.
// The overlap area should be 50% of the green, so we should see 2x as many red
// pixels as green.
bool triangle_depth_test(Gpu &gpu)
{
	constexpr uint32_t W = 64, H = 64;
	constexpr uint32_t pw = (W + 15) & ~15u;
	constexpr uint32_t ph = (H + 3) & ~3u;
	constexpr uint32_t stride = pw * 4;
	constexpr uint32_t rt_size = stride * ph;
	constexpr uint32_t dstride = pw * 2; // D16 = 2 bytes/pixel, same 16-wide tiling
	constexpr uint32_t depth_size = dstride * ph;
	constexpr uint32_t CLEAR = 0xFF0000FF; // blue background

	Bo rt = gpu.alloc(rt_size);
	Bo depth = gpu.alloc(depth_size);
	Bo vnear = gpu.alloc(3 * 3 * 4);
	Bo vfar = gpu.alloc(3 * 3 * 4);
	Bo vs = gpu.alloc(sizeof(kVsCode));
	Bo ps = gpu.alloc(sizeof(kPsCode));
	if (!rt || !depth || !vnear || !vfar || !vs || !ps)
		return false;

	// Clear color to blue and depth to 0xFFFF (far). Both uniform => the tiled
	// layout doesn't matter for the clear, and the GPU addresses depth via
	// PE_DEPTH_STRIDE consistently for both draws.
	std::ranges::fill(rt.span<uint32_t>(), CLEAR);
	rt.cpu_fini(RelocWrite);
	std::ranges::fill(depth.span<uint16_t>(), uint16_t(0xFFFF));
	depth.cpu_fini(RelocWrite);

	// Red triangle in front (z=0.3) and blocking 50% of green triangle in back (z=0.7)
	// The green triangle is the same as the red triangle but flipped over the X-axis.
	const std::array<float, 9> near_v = {-0.8f, -0.8f, 0.3f, 0.8f, -0.8f, 0.3f, 0.0f, 0.8f, 0.3f};
	const std::array<float, 9> far_v = {-0.8f, 0.8f, 0.7f, 0.8f, 0.8f, 0.7f, 0.0f, -0.8f, 0.7f};
	std::ranges::copy(near_v, vnear.span<float>().begin());
	vnear.cpu_fini(RelocWrite);
	std::ranges::copy(far_v, vfar.span<float>().begin());
	vfar.cpu_fini(RelocWrite);

	std::ranges::copy(kVsCode, vs.span<uint32_t>().begin());
	vs.cpu_fini(RelocWrite);
	std::ranges::copy(kPsCode, ps.span<uint32_t>().begin());
	ps.cpu_fini(RelocWrite);

	const std::array<float, 4> red = {1.0f, 0.0f, 0.0f, 1.0f};
	const std::array<float, 4> green = {0.0f, 1.0f, 0.0f, 1.0f};

	// Both draws in one stream: near red, then far green, against the shared depth.
	auto cs = gpu.new_cmd_stream(2048);
	emit_triangle(cs, rt, stride, vnear, 12, vs, ps, W, H, red, 3, &depth, dstride);
	emit_triangle(cs, rt, stride, vfar, 12, vs, ps, W, H, green, 3, &depth, dstride);

	auto start = read_cntpct();
	if (!gpu.submit_and_wait(cs)) {
		gpu.dump_status("depth draw");
		return false;
	}
	print("depth: two triangles drawn in ", (read_cntpct() - start), " ticks\n");

	rt.cpu_prep(RelocRead);

	uint32_t drawn = 0, reds = 0, greens = 0;
	for (uint32_t p : rt.span<uint32_t>()) {
		if (p == CLEAR)
			continue;
		drawn++;
		uint32_t R = (p >> 16) & 0xFF, G = (p >> 8) & 0xFF;
		if (R > 128 && G < 64)
			reds++;
		else if (G > 128 && R < 64)
			greens++;
	}
	print("depth test: ", drawn, " drawn -> ", reds, " red (near), ", greens, " green (far)\n");
	if (drawn == 0) {
		print("FAILED: nothing drawn\n");
		return false;
	}
	if (greens > (reds / 2 + 6) || greens < (reds / 2 - 6)) { // +/-6 in case we have an aliased pixel each intersection
		print("FAILED: wrong number of green fragments survived -> we should see ~50% green");
		return false;
	}
	if (reds == 0) {
		print("FAILED: no red -> near triangle missing\n");
		return false;
	}
	print("GPU depth test occluded the farther triangle -- depth buffer works. \\o/\n");
	return true;
}

// =============================================================================
//  Texture test
// =============================================================================
//
// A 64x64 tiled A8R8G8B8 texture with four solid quadrants
// (TL red, TR green, BL blue, BR white), sampled by the usual triangle with UVs
// (0,0) (1,0) (0.5,1) -- which covers all four UV quadrants. NEAREST filtering
// returns exact texel values, so every drawn pixel must be one of the four
// quadrant colors, and (tiling-invariantly) all four must appear.
bool triangle_texture_test(Gpu &gpu)
{
	constexpr uint32_t W = 64, H = 64;	 // render target
	constexpr uint32_t TW = 64, TH = 64; // texture (pow2)
	constexpr uint32_t pw = (W + 15) & ~15u;
	constexpr uint32_t ph = (H + 3) & ~3u;
	constexpr uint32_t stride = pw * 4;
	constexpr uint32_t rt_size = stride * ph;
	constexpr uint32_t tpw = (TW + 15) & ~15u; // texture padded width
	constexpr uint32_t tstride = tpw * 4;
	constexpr uint32_t tex_size = tstride * ((TH + 3) & ~3u);
	constexpr uint32_t CLEAR = 0xFF000000; // black; no quadrant color is black
	constexpr std::array<uint32_t, 4> QUAD = {
		0xFFFF0000, // TL red
		0xFF00FF00, // TR green
		0xFF0000FF, // BL blue
		0xFFFFFFFF, // BR white
	};

	Bo rt = gpu.alloc(rt_size);
	Bo tex = gpu.alloc(tex_size); // 64B aligned (alloc default)
	Bo desc = gpu.alloc(256);	  // TXDESC: 256 B, 64B aligned
	Bo vtx = gpu.alloc(3 * 7 * 4);
	Bo vs = gpu.alloc(sizeof(kVsColorCode));
	Bo ps = gpu.alloc(sizeof(kPsTexCode));
	if (!rt || !tex || !desc || !vtx || !vs || !ps)
		return false;

	std::ranges::fill(rt.span<uint32_t>(), CLEAR);
	rt.cpu_fini(RelocWrite);

	// Linear (x, y) -> 32-bit-word index in the basic-tiled texture layout the
	// sampler reads (4x4-pixel tiles, row-major; Mesa etnaviv_tiling.c).
	constexpr auto tiled_index = [](uint32_t x, uint32_t y, uint32_t stride_px) -> uint32_t {
		return (y / 4) * (stride_px * 4) + (y % 4) * 4 + (x / 4) * 16 + (x % 4);
	};

	// Texel data, written directly in the basic-tiled layout the sampler reads.
	auto tp = tex.span<uint32_t>();
	for (uint32_t y = 0; y < TH; y++)
		for (uint32_t x = 0; x < TW; x++)
			tp[tiled_index(x, y, tpw)] = QUAD[(y < TH / 2 ? 0 : 2) + (x < TW / 2 ? 0 : 1)];
	tex.cpu_fini(RelocWrite);

	fill_tex_descriptor(desc.span<uint32_t>(), tex.gpu_addr(), TW, TH, tstride);
	desc.cpu_fini(RelocWrite);

	// pos vec3 + UV as vec4 (u, v, 0, 1) -- same layout/stride as the color draw
	const std::array<float, 3 * 7> verts = {
		-0.8f, -0.8f, 0.5f, 0.0f, 0.0f, 0.0f, 1.0f, // v0: UV (0,0)
		0.8f,  -0.8f, 0.5f, 1.0f, 0.0f, 0.0f, 1.0f, // v1: UV (1,0)
		0.0f,  0.8f,  0.5f, 0.5f, 1.0f, 0.0f, 1.0f, // v2: UV (0.5,1)
	};
	std::ranges::copy(verts, vtx.span<float>().begin());
	vtx.cpu_fini(RelocWrite);

	std::ranges::copy(kVsColorCode, vs.span<uint32_t>().begin()); // same VS: pass pos + vec4 varying
	vs.cpu_fini(RelocWrite);
	std::ranges::copy(kPsTexCode, ps.span<uint32_t>().begin());
	ps.cpu_fini(RelocWrite);

	auto cs = gpu.new_cmd_stream(1024);
	emit_triangle_tex(cs, rt, stride, vtx, 28, vs, ps, desc, W, H, 3);

	auto start = read_cntpct();
	if (!gpu.submit_and_wait(cs)) {
		gpu.dump_status("textured draw");
		return false;
	}
	print("textured triangle drawn in ", (uint32_t)(read_cntpct() - start), " ticks\n");

	rt.cpu_prep(RelocRead);
	uint32_t drawn = 0, other = 0;
	std::array<uint32_t, 4> counts{};
	for (uint32_t p : rt.span<uint32_t>()) {
		if (p == CLEAR)
			continue;
		drawn++;
		bool matched = false;
		for (uint32_t q = 0; q < 4; q++)
			if (p == QUAD[q]) {
				counts[q]++;
				matched = true;
				break;
			}
		if (!matched)
			other++;
	}
	print("texture test: ", drawn, " drawn -> R=", counts[0]);
	print(" G=", counts[1], " B=", counts[2]);
	print(" W=", counts[3], " other=", other, "\n");
	if (drawn == 0) {
		print("FAILED: nothing drawn\n");
		return false;
	}
	if (!(counts[0] && counts[1] && counts[2] && counts[3])) {
		print("FAILED: not all four texel quadrants were sampled\n");
		return false;
	}
	print("GPU sampled a 2D texture across the triangle -- texturing works. \\o/\n");
	return true;
}

// --- shape verification (needs a resolved / linear image) --------------------
// Signed distance (in px) from point p to the directed edge a->b. With a
// positive-winding triangle, all three edge distances are positive inside.
static float edge_dist(float ax, float ay, float bx, float by, float px, float py)
{
	float ex = bx - ax, ey = by - ay;
	return (ex * (py - ay) - ey * (px - ax)) / __builtin_sqrtf(ex * ex + ey * ey);
}

// Verify a resolved (linear, y*w + x indexed) image is exactly `inside` color
// within the window-space triangle given by NDC verts, and `outside` elsewhere.
// Pixels whose center is within 1 px of a triangle edge are ignored -- the
// rasterizer's fill rule owns the boundary. The window-Y direction (does NDC
// +Y go up or down the framebuffer rows?) isn't pinned yet, so try both and
// report which one the hardware uses.
static bool verify_shape(std::span<const uint32_t> img,
						 uint32_t w,
						 uint32_t h,
						 std::span<const float, 6> ndc, // x0,y0, x1,y1, x2,y2
						 uint32_t inside,
						 uint32_t outside)
{
	std::array<uint32_t, 2> bad{};
	for (int flip = 0; flip < 2; flip++) {
		// NDC -> window pixels via our viewport (scale/offset = w/2, h/2)
		std::array<float, 6> v;
		for (int i = 0; i < 3; i++) {
			v[i * 2 + 0] = (w / 2.0f) + (w / 2.0f) * ndc[i * 2 + 0];
			v[i * 2 + 1] = (h / 2.0f) + (h / 2.0f) * (flip ? -ndc[i * 2 + 1] : ndc[i * 2 + 1]);
		}
		// force positive winding so "inside" == all edge distances positive
		float area2 = (v[2] - v[0]) * (v[5] - v[1]) - (v[3] - v[1]) * (v[4] - v[0]);
		if (area2 < 0) {
			std::swap(v[2], v[4]);
			std::swap(v[3], v[5]);
		}
		uint32_t mismatches = 0;
		for (uint32_t y = 0; y < h; y++)
			for (uint32_t x = 0; x < w; x++) {
				float px = x + 0.5f, py = y + 0.5f;
				float d0 = edge_dist(v[0], v[1], v[2], v[3], px, py);
				float d1 = edge_dist(v[2], v[3], v[4], v[5], px, py);
				float d2 = edge_dist(v[4], v[5], v[0], v[1], px, py);
				float dmin = std::min({d0, d1, d2});
				uint32_t p = img[y * w + x];
				if (dmin > 1.0f) { // strictly inside (by > 1 px)
					if (p != inside)
						mismatches++;
				} else if (dmin < -1.0f) { // strictly outside
					if (p != outside)
						mismatches++;
				}
			}
		bad[flip] = mismatches;
		if (mismatches == 0) {
			print("shape: exact (NDC +Y = ", flip ? "decreasing" : "increasing", " framebuffer rows)\n");
			return true;
		}
	}
	print("shape FAILED: ", bad[0], " / ", bad[1], " strict mismatches (y-as-is / y-flipped)\n");
	return false;
}

// =============================================================================
//  Spinning cube: matrix transform in the VS + depth + CPU-reference verify
// =============================================================================
//
// Multi-instruction vertex shader: clip = M * position, with the 4x4
// matrix as 4 column vec4s in VS uniforms u0..u3 (MUL + 3x MAD).
// Verification is numeric against a CPU reference renderer.
// Frames must match the resolved GPU image pixel outside a ~1.25 px band
// around projected triangle edges
bool spinning_cube_test(etna::Gpu &gpu)
{
	constexpr uint32_t W = 64, H = 64;
	constexpr uint32_t pw = (W + 15) & ~15u;
	constexpr uint32_t ph = (H + 3) & ~3u;
	constexpr uint32_t stride = pw * 4;
	constexpr uint32_t rt_size = stride * ph;
	constexpr uint32_t dstride = pw * 2;
	constexpr uint32_t depth_size = dstride * ph;
	constexpr uint32_t CLEAR = 0xFF000000;
	constexpr int FRAMES = 8;

	Bo rt = gpu.alloc(rt_size);
	Bo depthb = gpu.alloc(depth_size);
	Bo vtx = gpu.alloc(sizeof(kCubeVerts));
	Bo vsb = gpu.alloc(sizeof(kCubeVs));
	Bo psb = gpu.alloc(sizeof(kPsColorCode));
	Bo lin = gpu.alloc(W * H * 4);
	if (!rt || !depthb || !vtx || !vsb || !psb || !lin)
		return false;

	std::ranges::copy(kCubeVerts, vtx.span<float>().begin());
	vtx.cpu_fini(RelocWrite);
	std::ranges::copy(kCubeVs, vsb.span<uint32_t>().begin());
	vsb.cpu_fini(RelocWrite);
	std::ranges::copy(kPsColorCode, psb.span<uint32_t>().begin());
	psb.cpu_fini(RelocWrite);

	static std::array<uint32_t, W * H> cpu_img;
	static std::array<uint8_t, W * H> band;
	static std::array<float, W * H> zbuf;

	uint32_t colors_seen = 0;
	uint64_t gpu_ticks = 0;
	for (int frame = 0; frame < FRAMES; frame++) {
		float angle = 0.3f + frame * (2 * kPi / FRAMES);
		Mat4 m = cube_mvp(angle, 0.6f * tsin(angle));

		std::ranges::fill(rt.span<uint32_t>(), CLEAR);
		rt.cpu_fini(RelocWrite);
		std::ranges::fill(depthb.span<uint16_t>(), uint16_t(0xFFFF));
		depthb.cpu_fini(RelocWrite);

		auto cs = gpu.new_cmd_stream(1024);
		etna::MeshDraw d{
			.rt = &rt,
			.rt_stride = stride,
			.vtx = &vtx,
			.vtx_stride = 28,
			.vs = &vsb,
			.vs_words = kCubeVs.size(),
			.vs_temps = 4,
			.ps = &psb,
			.ps_words = kPsColorCode.size(),
			.ps_temps = 2,
			.ps_out_reg = 1,
			.uniforms = m,
			.width = W,
			.height = H,
			.vertex_count = 36,
			.depth = &depthb,
			.depth_stride = dstride,
		};
		etna::emit_mesh(cs, d);
		auto t0 = read_cntpct();
		if (!gpu.submit_and_wait(cs)) {
			gpu.dump_status("cube draw");
			return false;
		}
		auto cs2 = gpu.new_cmd_stream(256);
		etna::resolve(cs2, lin, rt, W, H, stride, W * 4);
		if (!gpu.submit_and_wait(cs2)) {
			gpu.dump_status("cube resolve");
			return false;
		}
		gpu_ticks += read_cntpct() - t0;
		lin.cpu_prep(RelocRead);

		cpu_render_cube(m, W, H, cpu_img, band, zbuf, CLEAR);

		uint32_t mismatches = 0, alien = 0, banded = 0, drawn = 0;
		auto gp = lin.span<const uint32_t>();
		for (uint32_t i = 0; i < W * H; i++) {
			uint32_t p = gp[i];
			if (p != CLEAR) {
				drawn++;
				bool known = false;
				for (unsigned f = 0; f < 6; f++)
					if (p == face_argb(f)) {
						colors_seen |= 1u << f;
						known = true;
						break;
					}
				if (!known)
					alien++;
			}
			if (band[i]) {
				banded++;
				continue;
			}
			if (p != cpu_img[i]) {
				if (mismatches < 3) {
					print("  frame ", frame, " mismatch at (", i % W, ",", (i / W), "):");
					print(" gpu 0x", Hex{p}, " cpu 0x", Hex{cpu_img[i]}, "\n");
				}
				mismatches++;
			}
		}
		print("cube frame ", frame, ": ", drawn, " px drawn, ");
		print(mismatches, " mismatches (", banded, " edge px ignored)\n");

		if (mismatches || alien) {
			print("FAILED: frame ", frame, " -- ", mismatches, " mismatches, ", alien, " alien colors\n");
			return false;
		}
	}
	print("spinning cube: ", FRAMES, " frames avg ", uint32_t(gpu_ticks / FRAMES));
	print(" ticks (draw+resolve), faces seen 0x", Hex{colors_seen}, "\n");

	if (colors_seen != 0x3F) {
		print("FAILED: not all 6 faces appeared over the spin\n");
		return false;
	}

	print("GPU spun a cube: VS matrix transform + depth + rasterization all match the CPU. \\o/\n");
	return true;
}

// =============================================================================
//  Alpha blending
// =============================================================================
//
// Two overlapping axis-aligned quads, drawn in two submits:
//   A -- opaque red, blending OFF   (left 2/3, full height)
//   B -- green at alpha 0.5, SRC_ALPHA / ONE_MINUS_SRC_ALPHA (right 2/3, middle band)
//
// which partitions the target into four regions we can probe:
//
//   +--------------------------------------------+
//   |  A only (red)          |  clear (blue)     |
//   |            +-----------+-------------+     |
//   |            | A n B     | B only      |     |  <- B's band
//   |            | green/red | green/blue  |     |
//   |            +-----------+-------------+     |
//   |  A only (red)          |  clear (blue)     |
//   +--------------------------------------------+
//
// Blending against *two different destinations* is the point: it proves the PE
// actually re-read the render target rather than overwriting it. If the
// OVERWRITE bit were left set (the pre-blend behaviour), both blended regions
// would come out the same flat colour instead of picking up red vs. blue.
//
// Rects rather than the usual triangles so every probe sits far from a rasterised
// edge, where a point-sampled CPU reference can't be expected to agree.
bool triangle_blend_test(Gpu &gpu)
{
	constexpr uint32_t W = 64, H = 64;
	constexpr uint32_t pw = (W + 15) & ~15u;
	constexpr uint32_t ph = (H + 3) & ~3u;
	constexpr uint32_t stride = pw * 4;
	constexpr uint32_t rt_size = stride * ph;
	constexpr uint32_t CLEAR = 0xFF0000FF; // opaque blue (A,R,G,B = FF,00,00,FF)
	constexpr uint32_t RED = 0xFFFF0000;   // what quad A lays down
	constexpr float SrcAlpha = 0.5f;

	// Interleaved pos-vec3 + colour-vec4, stride 28 -- emit_mesh's fixed format.
	// Two triangles per quad, wound the same way as the other tests.
	auto quad = [](float x0, float y0, float x1, float y1, float r, float g, float b, float a) {
		return std::array<float, 6 * 7>{
			// clang-format off
			x0, y0, 0.5f, r, g, b, a,   x1, y0, 0.5f, r, g, b, a,   x1, y1, 0.5f, r, g, b, a,
			x0, y0, 0.5f, r, g, b, a,   x1, y1, 0.5f, r, g, b, a,   x0, y1, 0.5f, r, g, b, a,
			// clang-format on
		};
	};
	// Overlap is ndc x in [-0.3, 0.3] -- ~19 px wide at W=64, so a 5x5 probe
	// block in the middle clears both rasterised edges by ~7 px.
	const auto quad_a = quad(-0.9f, -0.9f, 0.3f, 0.9f, 1.0f, 0.0f, 0.0f, 1.0f);		// opaque red
	const auto quad_b = quad(-0.3f, -0.5f, 0.9f, 0.5f, 0.0f, 1.0f, 0.0f, SrcAlpha); // half-alpha green

	Bo rt = gpu.alloc(rt_size);
	Bo va = gpu.alloc(sizeof(quad_a));
	Bo vb = gpu.alloc(sizeof(quad_b));
	Bo vsb = gpu.alloc(sizeof(kVsColorCode));
	Bo psb = gpu.alloc(sizeof(kPsColorCode));
	Bo lin = gpu.alloc(W * H * 4);
	if (!rt || !va || !vb || !vsb || !psb || !lin)
		return false;

	std::ranges::fill(rt.span<uint32_t>(), CLEAR);
	rt.cpu_fini(RelocWrite);
	std::ranges::copy(quad_a, va.span<float>().begin());
	va.cpu_fini(RelocWrite);
	std::ranges::copy(quad_b, vb.span<float>().begin());
	vb.cpu_fini(RelocWrite);
	std::ranges::copy(kVsColorCode, vsb.span<uint32_t>().begin());
	vsb.cpu_fini(RelocWrite);
	std::ranges::copy(kPsColorCode, psb.span<uint32_t>().begin());
	psb.cpu_fini(RelocWrite);

	// Shared between the two draws: same shaders, same target, no depth. Only
	// the vertex buffer and the blend state differ.
	etna::MeshDraw d{
		.rt = &rt,
		.rt_stride = stride,
		.vtx = &va,
		.vtx_stride = 28,
		.vs = &vsb,
		.vs_words = kVsColorCode.size(),
		.vs_temps = 4,
		.ps = &psb,
		.ps_words = kPsColorCode.size(),
		.ps_temps = 2,
		.ps_out_reg = 1,
		.width = W,
		.height = H,
		.vertex_count = 6,
	};

	auto cs_a = gpu.new_cmd_stream(1024);
	etna::emit_mesh(cs_a, d); // quad A: blend defaults to off
	auto start = read_cntpct();
	if (!gpu.submit_and_wait(cs_a)) {
		gpu.dump_status("blend: opaque draw");
		return false;
	}

	d.vtx = &vb;
	d.blend = etna::kBlendSrcAlpha;
	auto cs_b = gpu.new_cmd_stream(1024);
	etna::emit_mesh(cs_b, d);
	if (!gpu.submit_and_wait(cs_b)) {
		gpu.dump_status("blend: blended draw");
		return false;
	}
	print("blend: two quads drawn in ", uint32_t(read_cntpct() - start), " ticks");
	print(" (PE_ALPHA_CONFIG 0x", Hex{etna::kBlendSrcAlpha.pe_alpha_config()}, ")\n");

	// Untile so probes can address pixel (x,y) at y*W + x.
	std::ranges::fill(lin.span<uint32_t>(), 0xDEADBEEFu);
	lin.cpu_fini(RelocWrite);
	auto cs_r = gpu.new_cmd_stream(256);
	resolve(cs_r, lin, rt, W, H, stride, W * 4);
	if (!gpu.submit_and_wait(cs_r)) {
		gpu.dump_status("blend: resolve");
		return false;
	}
	lin.cpu_prep(RelocRead);

	// CPU reference: out = src*srcA + dst*(1-srcA), per channel, unorm8.
	// Alpha blends by the same factors (no BLEND_SEPARATE_ALPHA), so over an
	// opaque destination out.A = 0.5*0.5 + 0.5*1.0 = 0.75 -> 191.
	auto blend_ref = [](float sr, float sg, float sb, float sa, uint32_t dst) {
		auto ch = [](float s, uint32_t d, float a) {
			float o = s * 255.0f * a + float(d) * (1.0f - a);
			return uint32_t(o + 0.5f) & 0xFFu;
		};
		uint32_t da = (dst >> 24) & 0xFF, dr = (dst >> 16) & 0xFF;
		uint32_t dg = (dst >> 8) & 0xFF, db = dst & 0xFF;
		return (ch(sa, da, sa) << 24) | (ch(sr, dr, sa) << 16) | (ch(sg, dg, sa) << 8) | ch(sb, db, sa);
	};

	// ndc -> window pixel. +Y is increasing framebuffer rows (see triangle_test).
	auto px = [](float ndc_x, float ndc_y) {
		return std::pair<uint32_t, uint32_t>{uint32_t((ndc_x * 0.5f + 0.5f) * W),
											 uint32_t((ndc_y * 0.5f + 0.5f) * H)};
	};

	struct Probe {
		const char *name;
		float ndc_x, ndc_y;
		uint32_t expect;
	};
	const std::array<Probe, 4> probes = {{
		{"A only (opaque red)", -0.6f, 0.0f, RED},
		{"A n B (green over red)", 0.0f, 0.0f, blend_ref(0, 1, 0, SrcAlpha, RED)},
		{"B only (green over blue)", 0.6f, 0.0f, blend_ref(0, 1, 0, SrcAlpha, CLEAR)},
		{"untouched (clear blue)", 0.6f, 0.8f, CLEAR},
	}};

	// Hardware unorm blend rounding can differ from the CPU reference by a
	// bit; allow +-2 per channel. Every pixel of a 5x5 block must agree, which
	// also catches a region landing in the wrong place.
	constexpr int Tol = 2;
	constexpr int Half = 2;
	auto img = lin.span<const uint32_t>();
	bool ok = true;
	for (const auto &p : probes) {
		auto [cx, cy] = px(p.ndc_x, p.ndc_y);
		uint32_t worst = 0;
		int worst_delta = 0;
		for (int dy = -Half; dy <= Half; dy++) {
			for (int dx = -Half; dx <= Half; dx++) {
				uint32_t got = img[(cy + dy) * W + (cx + dx)];
				for (int sh = 0; sh < 32; sh += 8) {
					int delta = int((got >> sh) & 0xFF) - int((p.expect >> sh) & 0xFF);
					delta = delta < 0 ? -delta : delta;
					if (delta > worst_delta) {
						worst_delta = delta;
						worst = got;
					}
				}
			}
		}
		print("  ", p.name, " at (", cx, ",", cy, "): expect 0x", Hex{p.expect});
		if (worst_delta > Tol) {
			print(" got 0x", Hex{worst}, " -- FAILED (off by ", uint32_t(worst_delta), ")\n");
			ok = false;
		} else {
			print(" ok (max delta ", uint32_t(worst_delta), ")\n");
		}
	}
	if (!ok) {
		print("FAILED: blended pixels do not match the CPU reference\n");
		return false;
	}

	print("GPU alpha-blended over two different destinations -- PE blending works. \\o/\n");
	return true;
}

// =============================================================================
//  Primitive types
// =============================================================================
//
// Exercises every primitive the FE can assemble (see etna_prim.hh for why
// these seven and not GL_QUADS). Each sub-draw goes into a freshly cleared
// target and is untiled, then reduced to a drawn-pixel count and bounding box.
//
// The assertions are deliberately about primitive *semantics* rather than
// exact pixel counts, so they don't depend on rasterisation fill rules:
//   - TRIANGLE_STRIP and TRIANGLE_FAN, given the same four corners in their
//     respective orders, must cover the SAME quad -- so their counts must
//     match each other, and their bounding box must be the quad.
//   - LINE_LOOP closes back to the first vertex, so on the same three vertices
//     it must draw strictly more than LINE_STRIP (3 edges vs 2).
//   - LINES/POINTS are checked by where they land, via the bounding box.
//
// A line primitive that renders nothing is the specific failure to watch for:
// that is the PA_CONFIG.WIDE_LINE trap described in etna_prim.hh.
// -----------------------------------------------------------------------------
//  Shared harness for the pipeline-state tests
// -----------------------------------------------------------------------------
// primitive/cull/scissor/depth-func all want the same thing: clear a small
// target, run one or more emit_mesh draws into it, untile, and reduce the
// result to "how many pixels were drawn and where". This owns those buffers and
// the pass-through shaders so each test only expresses what it is varying.
namespace
{
struct StateTarget {
	static constexpr uint32_t W = 64, H = 64;
	static constexpr uint32_t Pw = (W + 15) & ~15u;
	static constexpr uint32_t Ph = (H + 3) & ~3u;
	static constexpr uint32_t Stride = Pw * 4;
	static constexpr uint32_t DepthStride = Pw * 2;
	static constexpr uint32_t Clear = 0xFF000000; // opaque black

	Gpu *gpu = nullptr;
	Bo rt, depth, vb, vsb, psb, lin;
	bool has_depth = false;

	// `max_verts` sizes the vertex buffer; `with_depth` adds a D16 buffer.
	bool init(Gpu &g, uint32_t max_verts, bool with_depth)
	{
		gpu = &g;
		has_depth = with_depth;
		rt = g.alloc(Stride * Ph);
		vb = g.alloc(max_verts * 7 * 4);
		vsb = g.alloc(sizeof(kVsColorCode));
		psb = g.alloc(sizeof(kPsColorCode));
		lin = g.alloc(W * H * 4);
		if (with_depth)
			depth = g.alloc(DepthStride * Ph);
		if (!rt || !vb || !vsb || !psb || !lin || (with_depth && !depth))
			return false;

		std::ranges::copy(kVsColorCode, vsb.span<uint32_t>().begin());
		vsb.cpu_fini(RelocWrite);
		std::ranges::copy(kPsColorCode, psb.span<uint32_t>().begin());
		psb.cpu_fini(RelocWrite);
		return true;
	}

	// Reset the target before a sequence of draws. Depth resets to far (0xFFFF).
	void begin(uint32_t clear_color = Clear)
	{
		std::ranges::fill(rt.span<uint32_t>(), clear_color);
		rt.cpu_fini(RelocWrite);
		if (has_depth) {
			std::ranges::fill(depth.span<uint16_t>(), uint16_t(0xFFFF));
			depth.cpu_fini(RelocWrite);
		}
	}

	// A MeshDraw wired to this target; callers override only what they vary.
	MeshDraw base() const
	{
		return MeshDraw{
			.rt = &rt,
			.rt_stride = Stride,
			.vtx = &vb,
			.vtx_stride = 28,
			.vs = &vsb,
			.vs_words = kVsColorCode.size(),
			.vs_temps = 4,
			.ps = &psb,
			.ps_words = kPsColorCode.size(),
			.ps_temps = 2,
			.ps_out_reg = 1,
			.width = W,
			.height = H,
			.depth = has_depth ? &depth : nullptr,
			.depth_stride = has_depth ? DepthStride : 0,
		};
	}

	// Upload vertices and issue one draw. Does NOT clear -- call begin() first.
	bool draw(MeshDraw d, std::span<const float> verts, uint32_t nverts)
	{
		std::ranges::copy(verts, vb.span<float>().begin());
		vb.cpu_fini(RelocWrite);
		d.vertex_count = nverts;

		auto cs = gpu->new_cmd_stream(1024);
		etna::emit_mesh(cs, d);
		if (!gpu->submit_and_wait(cs)) {
			gpu->dump_status("state test draw");
			return false;
		}
		return true;
	}

	struct Stats {
		uint32_t drawn = 0;
		uint32_t min_x = W, max_x = 0, min_y = H, max_y = 0;
		bool ok = false;

		// Did every drawn pixel fall inside [x0,x1) x [y0,y1)?
		bool within(uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1) const
		{
			return drawn && min_x >= x0 && max_x < x1 && min_y >= y0 && max_y < y1;
		}
	};

	// Untile and reduce to a count + bounding box of non-clear pixels.
	Stats finish(uint32_t clear_color = Clear)
	{
		Stats s;
		std::ranges::fill(lin.span<uint32_t>(), clear_color);
		lin.cpu_fini(RelocWrite);
		auto cs = gpu->new_cmd_stream(256);
		resolve(cs, lin, rt, W, H, Stride, W * 4);
		if (!gpu->submit_and_wait(cs)) {
			gpu->dump_status("state test resolve");
			return s;
		}
		lin.cpu_prep(RelocRead);

		auto img = lin.span<const uint32_t>();
		for (uint32_t y = 0; y < H; y++)
			for (uint32_t x = 0; x < W; x++)
				if (img[y * W + x] != clear_color) {
					s.drawn++;
					s.min_x = std::min(s.min_x, x);
					s.max_x = std::max(s.max_x, x);
					s.min_y = std::min(s.min_y, y);
					s.max_y = std::max(s.max_y, y);
				}
		s.ok = true;
		return s;
	}

	uint32_t pixel_at(uint32_t x, uint32_t y) const
	{
		return lin.span<const uint32_t>()[y * W + x];
	}

	static void report(const char *name, const Stats &s)
	{
		print("  ", name, ": ", s.drawn, " px");
		if (s.drawn)
			print(" bbox x[", s.min_x, "..", s.max_x, "] y[", s.min_y, "..", s.max_y, "]");
		print("\n");
	}
};

// One vertex for emit_mesh's layout: pos vec3 + colour vec4.
constexpr std::array<float, 7> mkvtx(float x, float y, float z, float r, float g, float b, float a = 1.0f)
{
	return {x, y, z, r, g, b, a};
}

// Pack up to 8 vertices into one interleaved buffer.
inline std::array<float, 8 * 7> packv(std::initializer_list<std::array<float, 7>> vs)
{
	std::array<float, 8 * 7> out{};
	uint32_t w = 0;
	for (const auto &v : vs)
		for (float f : v)
			out[w++] = f;
	return out;
}

// An axis-aligned quad as a 6-vertex triangle list, in NDC.
inline std::array<float, 8 * 7>
quad_tris(float x0, float y0, float x1, float y1, float z, float r, float g, float b, float a = 1.0f)
{
	return packv({mkvtx(x0, y0, z, r, g, b, a), mkvtx(x1, y0, z, r, g, b, a), mkvtx(x1, y1, z, r, g, b, a),
				  mkvtx(x0, y0, z, r, g, b, a), mkvtx(x1, y1, z, r, g, b, a), mkvtx(x0, y1, z, r, g, b, a)});
}
} // namespace

bool primitive_test(Gpu &gpu)
{
	constexpr uint32_t W = 64, H = 64;
	constexpr uint32_t pw = (W + 15) & ~15u;
	constexpr uint32_t ph = (H + 3) & ~3u;
	constexpr uint32_t stride = pw * 4;
	constexpr uint32_t rt_size = stride * ph;
	constexpr uint32_t CLEAR = 0xFF000000; // opaque black

	// NDC of a pixel's centre -- keeps points and thin lines off pixel
	// boundaries, where which row they land in would be a coin flip.
	auto ndc = [](float pixel) { return (pixel + 0.5f) / float(W) * 2.0f - 1.0f; };

	// One vertex = pos vec3 + colour vec4 (emit_mesh's fixed layout). Every
	// vertex is opaque white, so "drawn" is simply "not the clear colour".
	auto vtx7 = [](float x, float y) { return std::array<float, 7>{x, y, 0.5f, 1.0f, 1.0f, 1.0f, 1.0f}; };

	Bo rt = gpu.alloc(rt_size);
	Bo vb = gpu.alloc(64 * 7 * 4); // room for plenty of vertices
	Bo vsb = gpu.alloc(sizeof(kVsColorCode));
	Bo psb = gpu.alloc(sizeof(kPsColorCode));
	Bo lin = gpu.alloc(W * H * 4);
	if (!rt || !vb || !vsb || !psb || !lin)
		return false;

	std::ranges::copy(kVsColorCode, vsb.span<uint32_t>().begin());
	vsb.cpu_fini(RelocWrite);
	std::ranges::copy(kPsColorCode, psb.span<uint32_t>().begin());
	psb.cpu_fini(RelocWrite);

	// What one sub-draw reports back.
	struct Result {
		uint32_t drawn = 0;
		uint32_t min_x = W, max_x = 0, min_y = H, max_y = 0;
		bool ok = false;
	};

	// Clear -> upload vertices -> draw -> resolve -> reduce.
	auto run = [&](const char *name, etna::Primitive p, std::span<const float> verts, uint32_t nverts) {
		Result r;

		std::ranges::fill(rt.span<uint32_t>(), CLEAR);
		rt.cpu_fini(RelocWrite);
		std::ranges::copy(verts, vb.span<float>().begin());
		vb.cpu_fini(RelocWrite);

		if (!etna::valid_vertex_count(p, nverts)) {
			print("  ", name, ": bad vertex count ", nverts, " for this primitive\n");
			return r;
		}

		etna::MeshDraw d{
			.rt = &rt,
			.rt_stride = stride,
			.vtx = &vb,
			.vtx_stride = 28,
			.vs = &vsb,
			.vs_words = kVsColorCode.size(),
			.vs_temps = 4,
			.ps = &psb,
			.ps_words = kPsColorCode.size(),
			.ps_temps = 2,
			.ps_out_reg = 1,
			.width = W,
			.height = H,
			.vertex_count = nverts,
			.prim = p,
		};

		auto cs = gpu.new_cmd_stream(1024);
		etna::emit_mesh(cs, d);
		if (!gpu.submit_and_wait(cs)) {
			gpu.dump_status("primitive draw");
			return r;
		}

		std::ranges::fill(lin.span<uint32_t>(), CLEAR);
		lin.cpu_fini(RelocWrite);
		auto cs_r = gpu.new_cmd_stream(256);
		resolve(cs_r, lin, rt, W, H, stride, W * 4);
		if (!gpu.submit_and_wait(cs_r)) {
			gpu.dump_status("primitive resolve");
			return r;
		}
		lin.cpu_prep(RelocRead);

		auto img = lin.span<const uint32_t>();
		for (uint32_t y = 0; y < H; y++)
			for (uint32_t x = 0; x < W; x++)
				if (img[y * W + x] != CLEAR) {
					r.drawn++;
					r.min_x = std::min(r.min_x, x);
					r.max_x = std::max(r.max_x, x);
					r.min_y = std::min(r.min_y, y);
					r.max_y = std::max(r.max_y, y);
				}

		print("  ", name, ": ", r.drawn, " px");
		if (r.drawn)
			print(" bbox x[", r.min_x, "..", r.max_x, "] y[", r.min_y, "..", r.max_y, "]");
		print(" (", etna::primitive_count(p, nverts), " prims)\n");
		r.ok = true;
		return r;
	};

	// --- geometry -------------------------------------------------------------
	// An axis-aligned quad spanning ndc [-0.5,0.5]^2 -> pixels [16..48].
	const auto BL = vtx7(-0.5f, -0.5f), BR = vtx7(0.5f, -0.5f);
	const auto TR = vtx7(0.5f, 0.5f), TL = vtx7(-0.5f, 0.5f);
	auto pack = [](std::initializer_list<std::array<float, 7>> vs) {
		std::array<float, 8 * 7> out{};
		uint32_t w = 0;
		for (const auto &v : vs)
			for (float f : v)
				out[w++] = f;
		return out;
	};

	const auto strip_quad = pack({BL, BR, TL, TR}); // strip: (BL,BR,TL) + (TL,BR,TR)
	const auto fan_quad = pack({BL, BR, TR, TL});   // fan:   (BL,BR,TR) + (BL,TR,TL)

	// A triangle outline for the strip-vs-loop comparison.
	const auto outline = pack({vtx7(-0.6f, -0.6f), vtx7(0.6f, -0.6f), vtx7(0.0f, 0.6f)});

	// A horizontal line along a pixel-centre row, spanning ndc x [-0.5, 0.5].
	const auto hline = pack({vtx7(-0.5f, ndc(32)), vtx7(0.5f, ndc(32))});

	// Four points at pixel centres 16 and 48 in each axis.
	const auto points = pack({vtx7(ndc(16), ndc(16)), vtx7(ndc(48), ndc(16)), vtx7(ndc(48), ndc(48)),
							  vtx7(ndc(16), ndc(48))});

	print("primitive types:\n");
	auto r_tris = run("TRIANGLES    ", etna::Primitive::Triangles, strip_quad, 3);
	auto r_strip = run("TRIANGLE_STRIP", etna::Primitive::TriangleStrip, strip_quad, 4);
	auto r_fan = run("TRIANGLE_FAN ", etna::Primitive::TriangleFan, fan_quad, 4);
	auto r_lines = run("LINES        ", etna::Primitive::Lines, hline, 2);
	auto r_lstrip = run("LINE_STRIP   ", etna::Primitive::LineStrip, outline, 3);
	auto r_lloop = run("LINE_LOOP    ", etna::Primitive::LineLoop, outline, 3);
	auto r_points = run("POINTS       ", etna::Primitive::Points, points, 4);

	for (const auto *r : {&r_tris, &r_strip, &r_fan, &r_lines, &r_lstrip, &r_lloop, &r_points})
		if (!r->ok) {
			print("FAILED: a primitive draw did not complete\n");
			return false;
		}

	// Any primitive rendering nothing is a real failure. For the line types
	// this is almost certainly PA_CONFIG.WIDE_LINE (etna_prim.hh).
	const bool lines_empty = r_lines.drawn == 0 || r_lstrip.drawn == 0 || r_lloop.drawn == 0;
	if (lines_empty) {
		print("FAILED: a line primitive drew no pixels -- suspect PA_CONFIG.WIDE_LINE (bit 22)\n");
		return false;
	}
	if (r_points.drawn == 0 || r_strip.drawn == 0 || r_fan.drawn == 0) {
		print("FAILED: a primitive drew no pixels\n");
		return false;
	}

	// Strip and fan describe the same quad, so they must agree with each other
	// and cover pixels [16..48] in both axes.
	if (r_strip.drawn != r_fan.drawn) {
		print("FAILED: TRIANGLE_STRIP (", r_strip.drawn, " px) and TRIANGLE_FAN (", r_fan.drawn,
			  " px) should cover the same quad\n");
		return false;
	}
	if (r_strip.min_x > 17 || r_strip.max_x < 46 || r_strip.min_y > 17 || r_strip.max_y < 46) {
		print("FAILED: strip/fan quad is not where it should be (expected about x[16..48] y[16..48])\n");
		return false;
	}
	// Two triangles must beat the single one built from the same first 3 verts.
	if (r_strip.drawn <= r_tris.drawn) {
		print("FAILED: strip (2 triangles) should cover more than TRIANGLES (1 triangle)\n");
		return false;
	}

	// LINE_LOOP adds the closing edge, so it must draw strictly more.
	if (r_lloop.drawn <= r_lstrip.drawn) {
		print("FAILED: LINE_LOOP (", r_lloop.drawn, " px) should exceed LINE_STRIP (", r_lstrip.drawn,
			  " px) by the closing edge\n");
		return false;
	}

	// The horizontal line must be flat and span the requested range.
	if (r_lines.max_y - r_lines.min_y > 1) {
		print("FAILED: horizontal LINES span ", r_lines.max_y - r_lines.min_y + 1, " rows, expected 1-2\n");
		return false;
	}
	if (r_lines.min_x > 17 || r_lines.max_x < 46) {
		print("FAILED: LINES bbox x[", r_lines.min_x, "..", r_lines.max_x, "] does not span the segment\n");
		return false;
	}

	// The four points must sit at the corners of the [16..48] box.
	if (r_points.min_x > 17 || r_points.max_x < 47 || r_points.min_y > 17 || r_points.max_y < 47) {
		print("FAILED: POINTS did not land at the four expected corners\n");
		return false;
	}

	print("GPU assembled points, lines, line strips/loops, and triangle strips/fans. \\o/\n");
	return true;
}

// =============================================================================
//  Face culling and winding
// =============================================================================
//
// Which winding the hardware calls "clockwise" is a window-space question, and
// our viewport maps NDC +Y to increasing framebuffer rows -- the opposite of
// the usual GL convention. Rather than assume a handedness, this test asserts
// only the relationships that must hold whichever way round it is:
//
//   1. With culling off, the triangle draws.
//   2. For a given vertex order, exactly ONE of cull-front / cull-back removes
//      it (a face is either front or back; it cannot be neither or both).
//   3. Reversing the vertex order swaps which one removes it.
//   4. Flipping glFrontFace swaps which one removes it.
//
// Together those prove the cull mode reaches the hardware AND that the front
// face setting is wired, without hard-coding a handedness. The test prints the
// handedness it observes, which is the useful output.
bool cull_test(Gpu &gpu)
{
	StateTarget t;
	if (!t.init(gpu, 8, false))
		return false;

	// A triangle listed counter-clockwise in NDC (+Y up).
	const auto ccw = packv({mkvtx(-0.7f, -0.7f, 0.5f, 1, 1, 1), mkvtx(0.7f, -0.7f, 0.5f, 1, 1, 1),
							mkvtx(0.0f, 0.7f, 0.5f, 1, 1, 1)});
	// The same triangle with two vertices swapped -> opposite winding.
	const auto cw = packv({mkvtx(0.7f, -0.7f, 0.5f, 1, 1, 1), mkvtx(-0.7f, -0.7f, 0.5f, 1, 1, 1),
						   mkvtx(0.0f, 0.7f, 0.5f, 1, 1, 1)});

	auto shot = [&](std::span<const float> verts, etna::CullMode c, etna::FrontFace f) {
		t.begin();
		auto d = t.base();
		d.cull = c;
		d.front_face = f;
		if (!t.draw(d, verts, 3))
			return StateTarget::Stats{};
		return t.finish();
	};

	print("face culling:\n");
	struct Case {
		const char *name;
		std::span<const float> verts;
		etna::CullMode cull;
		etna::FrontFace front;
	};
	const Case cases[] = {
		{"ccw verts, cull off  ", ccw, etna::CullMode::None, etna::FrontFace::CCW},
		{"ccw verts, cull back ", ccw, etna::CullMode::Back, etna::FrontFace::CCW},
		{"ccw verts, cull front", ccw, etna::CullMode::Front, etna::FrontFace::CCW},
		{"cw  verts, cull back ", cw, etna::CullMode::Back, etna::FrontFace::CCW},
		{"cw  verts, cull front", cw, etna::CullMode::Front, etna::FrontFace::CCW},
		{"ccw verts, cull back, frontFace=CW ", ccw, etna::CullMode::Back, etna::FrontFace::CW},
		{"ccw verts, cull front, frontFace=CW", ccw, etna::CullMode::Front, etna::FrontFace::CW},
	};
	std::array<StateTarget::Stats, 7> r;
	for (uint32_t i = 0; i < 7; i++) {
		r[i] = shot(cases[i].verts, cases[i].cull, cases[i].front);
		if (!r[i].ok) {
			print("FAILED: a cull draw did not complete\n");
			return false;
		}
		StateTarget::report(cases[i].name, r[i]);
	}

	if (r[0].drawn == 0) {
		print("FAILED: culling disabled should still draw the triangle\n");
		return false;
	}
	// 2. Exactly one of cull-front / cull-back removes a given winding.
	auto exactly_one_culled = [](const StateTarget::Stats &a, const StateTarget::Stats &b, const char *what) {
		const bool a0 = a.drawn == 0, b0 = b.drawn == 0;
		if (a0 == b0) {
			print("FAILED: ", what, " -- cull-back drew ", a.drawn, " px and cull-front drew ", b.drawn,
				  " px; exactly one should have been culled\n");
			return false;
		}
		return true;
	};
	if (!exactly_one_culled(r[1], r[2], "ccw vertex order"))
		return false;
	if (!exactly_one_culled(r[3], r[4], "cw vertex order"))
		return false;
	if (!exactly_one_culled(r[5], r[6], "ccw verts with frontFace=CW"))
		return false;

	// 3. Reversing the winding swaps which cull mode removes the triangle.
	if ((r[1].drawn == 0) == (r[3].drawn == 0)) {
		print("FAILED: reversing the vertex order should swap which face is culled\n");
		return false;
	}
	// 4. Flipping glFrontFace does the same.
	if ((r[1].drawn == 0) == (r[5].drawn == 0)) {
		print("FAILED: flipping frontFace should swap which face is culled\n");
		return false;
	}

	// Report the handedness we actually observed -- the genuinely new information.
	print("  observed: with frontFace=CCW, a CCW-in-NDC triangle is ",
		  (r[1].drawn == 0) ? "BACK-facing" : "FRONT-facing", " in window space\n");
	print("GPU culled by winding, and glFrontFace flips it. \\o/\n");
	return true;
}

// =============================================================================
//  Scissor
// =============================================================================
//
// Draws a target-covering quad through several scissor rectangles and checks
// that the drawn pixels are exactly the rectangle. This is also what a GL front
// end needs for Processing's clip(), which is rectangular -- so no stencil.
bool scissor_test(Gpu &gpu)
{
	StateTarget t;
	if (!t.init(gpu, 8, false))
		return false;

	// Covers the whole target, so whatever survives is the scissor's doing.
	const auto full = quad_tris(-1.0f, -1.0f, 1.0f, 1.0f, 0.5f, 1, 1, 1);

	auto shot = [&](etna::Scissor sc) {
		t.begin();
		auto d = t.base();
		d.scissor = sc;
		if (!t.draw(d, full, 6))
			return StateTarget::Stats{};
		return t.finish();
	};

	print("scissor:\n");
	constexpr uint32_t W = StateTarget::W, H = StateTarget::H;

	const auto off = shot({});
	const auto box = shot({.enable = true, .minx = 16, .miny = 8, .maxx = 48, .maxy = 40});
	const auto corner = shot({.enable = true, .minx = 0, .miny = 0, .maxx = 8, .maxy = 8});
	const auto over = shot({.enable = true, .minx = 0, .miny = 0, .maxx = 999, .maxy = 999});
	const auto empty = shot({.enable = true, .minx = 30, .miny = 30, .maxx = 30, .maxy = 30});

	for (const auto *s : {&off, &box, &corner, &over, &empty})
		if (!s->ok) {
			print("FAILED: a scissor draw did not complete\n");
			return false;
		}
	StateTarget::report("disabled      ", off);
	StateTarget::report("[16,8)-(48,40)", box);
	StateTarget::report("[0,0)-(8,8)   ", corner);
	StateTarget::report("oversized     ", over);
	StateTarget::report("empty         ", empty);

	// Disabled and oversized must both mean "the whole target". The exact count
	// here depends on the quad's own edge/fill rule (its edges sit on the target
	// boundary), so require "essentially full" plus a full-target bounding box
	// rather than a precise 4096 -- a fill-rule detail is not what is under test.
	if (off.drawn < W * H - 128 || off.min_x != 0 || off.min_y != 0 || off.max_x != W - 1 ||
		off.max_y != H - 1) {
		print("FAILED: scissor disabled should cover the whole target, got ", off.drawn, " px\n");
		return false;
	}
	if (over.drawn != off.drawn) {
		print("FAILED: an oversized scissor should clamp to the target\n");
		return false;
	}
	// Here all four edges are the SCISSOR's, not the quad's -- the scissor is a
	// hard window-space clamp, so this one should be exact. If it is off by a
	// pixel that is a real finding about the SE margin constants.
	if (box.drawn != 32 * 32 || !box.within(16, 8, 48, 40)) {
		print("FAILED: scissored quad should be exactly 1024 px inside x[16..47] y[8..39]\n");
		return false;
	}
	// The corner rect's min edges coincide with the quad's own edges, so allow
	// the fill-rule slack there; the max edges are still the scissor's.
	if (corner.drawn < 7 * 7 || corner.drawn > 8 * 8 || !corner.within(0, 0, 8, 8)) {
		print("FAILED: corner scissor should clip to about 8x8 px at the origin, got ", corner.drawn, "\n");
		return false;
	}
	// A degenerate rect must draw nothing rather than wrapping.
	if (empty.drawn != 0) {
		print("FAILED: an empty scissor drew ", empty.drawn, " px\n");
		return false;
	}

	print("GPU clipped to the scissor rectangle. \\o/\n");
	return true;
}

// =============================================================================
//  Depth compare functions and the depth mask
// =============================================================================
//
// A red quad is laid down at z = 0.5 with LESS+write, then a green quad at
// z = 0.7 (farther) is drawn over it with each compare function. Whether the
// green survives is a direct read-out of the function:
//
//   LESS / NEVER      -> 0.7 fails against 0.5  -> stays red
//   GREATER / ALWAYS  -> passes                 -> turns green
//
// Then glDepthMask: draw green at z = 0.3 with LESS but writes OFF, so the
// colour changes but the depth buffer keeps 0.5. A following blue quad at
// z = 0.4 must therefore still pass (0.4 < 0.5). Had the masked draw written,
// depth would be 0.3 and the blue quad would fail -- so the blue quad's fate
// is what actually proves the mask.
bool depth_func_test(Gpu &gpu)
{
	StateTarget t;
	if (!t.init(gpu, 8, true))
		return false;

	constexpr uint32_t RED = 0xFFFF0000, GREEN = 0xFF00FF00, BLUE = 0xFF0000FF;
	// Same footprint every time, so "which colour is at the centre" is the answer.
	auto quad_at = [](float z, float r, float g, float b) {
		return quad_tris(-0.6f, -0.6f, 0.6f, 0.6f, z, r, g, b);
	};
	const auto near_red = quad_at(0.5f, 1, 0, 0);
	const auto far_green = quad_at(0.7f, 0, 1, 0);

	// Draw the red base, then a second quad with `ds`, and report the centre pixel.
	auto probe = [&](std::span<const float> second, etna::DepthState ds) -> uint32_t {
		t.begin();
		auto base = t.base();
		base.depth_state = etna::kDepthLessWrite;
		if (!t.draw(base, near_red, 6))
			return 0;
		auto d = t.base();
		d.depth_state = ds;
		if (!t.draw(d, second, 6))
			return 0;
		if (!t.finish().ok)
			return 0;
		return t.pixel_at(StateTarget::W / 2, StateTarget::H / 2);
	};

	print("depth compare functions (red at z=0.5, then green at z=0.7):\n");
	struct Case {
		const char *name;
		etna::CompareFunc func;
		uint32_t expect;
	};
	const Case cases[] = {
		{"LESS    ", etna::CompareFunc::Less, RED},
		{"GREATER ", etna::CompareFunc::Greater, GREEN},
		{"ALWAYS  ", etna::CompareFunc::Always, GREEN},
		{"NEVER   ", etna::CompareFunc::Never, RED},
		{"LEQUAL  ", etna::CompareFunc::LEqual, RED},
		{"GEQUAL  ", etna::CompareFunc::GEqual, GREEN},
	};
	bool ok = true;
	for (const auto &c : cases) {
		const uint32_t got = probe(far_green, {.test = true, .write = true, .func = c.func});
		const bool pass = got == c.expect;
		print("  ", c.name, ": got 0x", Hex{got}, " expect 0x", Hex{c.expect}, pass ? "  ok\n" : "  MISMATCH\n");
		ok = ok && pass;
	}
	if (!ok) {
		print("FAILED: a depth compare function did not behave as specified\n");
		return false;
	}

	// --- glDepthMask ----------------------------------------------------------
	// Green at z=0.3 passes LESS but must not update the buffer.
	t.begin();
	auto base = t.base();
	base.depth_state = etna::kDepthLessWrite;
	if (!t.draw(base, near_red, 6))
		return false;
	auto masked = t.base();
	masked.depth_state = etna::kDepthTestNoWrite; // LESS, write off
	if (!t.draw(masked, quad_at(0.3f, 0, 1, 0), 6))
		return false;
	// Blue at z=0.4: passes only if depth is still 0.5.
	auto after = t.base();
	after.depth_state = etna::kDepthLessWrite;
	if (!t.draw(after, quad_at(0.4f, 0, 0, 1), 6))
		return false;
	if (!t.finish().ok)
		return false;

	const uint32_t masked_px = t.pixel_at(StateTarget::W / 2, StateTarget::H / 2);
	print("  depth mask: after z=0.3 (write off) then z=0.4, centre is 0x", Hex{masked_px}, " expect 0x",
		  Hex{BLUE}, "\n");
	if (masked_px != BLUE) {
		print("FAILED: the z=0.3 draw wrote depth despite glDepthMask being off"
			  " (blue at z=0.4 should still have passed against 0.5)\n");
		return false;
	}

	print("GPU honoured all six depth compare functions and the depth write mask. \\o/\n");
	return true;
}

// =============================================================================
//  Batched drawing with dirty-state tracking
// =============================================================================
//
// Draws the same scene two ways and requires the results to be pixel-identical:
//
//   unbatched -- one emit_mesh + submit + wait per quad, which re-emits ~100
//                registers, both shaders and four pipeline stalls every time
//   batched   -- one Context over one command stream, so each draw after the
//                first emits only what changed, then a single drain + submit
//
// It reports the dwords each draw emitted and the wall-clock for both paths, in
// two workloads: same-state draws (colour carried in the vertex attributes,
// the shape a GL front end would produce) and uniform-changing draws (a
// per-draw transform, like the cube demo). The second is expected to stay
// slower per draw: changing uniforms means writing shader state, and HALTI5
// shader state is not self-synchronizing, so it costs an FE->PE stall. That is
// correct behaviour, not a regression -- it is the reason the Processing fast
// path wants a constant transform and per-vertex colour.
//
// NOTE the arena. Each draw in a batch needs its own vertex memory: the FE
// reads vertex buffers asynchronously until the fence, so reusing one buffer
// across batched draws races the GPU. See etna_arena.hh.
bool batch_test(Gpu &gpu)
{
	constexpr uint32_t NQuads = 24;
	constexpr uint32_t VertsPerQuad = 6;
	constexpr uint32_t FloatsPerQuad = VertsPerQuad * 7;

	StateTarget t;
	if (!t.init(gpu, 8, false))
		return false;

	etna::Arena arena;
	if (!arena.init(gpu, 64 * 1024)) {
		print("FAILED: could not allocate the vertex arena\n");
		return false;
	}

	// A grid of small quads, each a different colour so a mis-ordered or
	// dropped draw shows up as a pixel difference rather than by luck.
	auto quad_i = [](uint32_t i) {
		const uint32_t col = i % 6, row = i / 6;
		const float w = 2.0f / 6.0f, h = 2.0f / 4.0f;
		const float x0 = -1.0f + col * w + 0.02f, x1 = x0 + w - 0.04f;
		const float y0 = -1.0f + row * h + 0.02f, y1 = y0 + h - 0.04f;
		// Kept well away from 0 so no quad is black-on-black against the clear
		// colour -- an invisible quad would silently weaken the comparison.
		const float r = float(40 + (i * 37) % 200) / 255.0f;
		const float g = float(40 + (i * 91) % 200) / 255.0f;
		const float b = float(40 + (i * 53) % 200) / 255.0f;
		return quad_tris(x0, y0, x1, y1, 0.5f, r, g, b);
	};

	// Snapshot the resolved image so the two paths can be compared exactly.
	static std::array<uint32_t, StateTarget::W * StateTarget::H> ref;

	// --- path A: one submit per draw -----------------------------------------
	arena.reset();
	t.begin();
	auto t0 = read_cntpct();
	for (uint32_t i = 0; i < NQuads; i++) {
		const auto v = quad_i(i);
		Bo vb = arena.upload(std::span<const float>(v.data(), FloatsPerQuad));
		if (!vb) {
			print("FAILED: vertex arena exhausted\n");
			return false;
		}
		auto d = t.base();
		d.vtx = &vb;
		d.vertex_count = VertsPerQuad;
		auto cs = gpu.new_cmd_stream(1024);
		etna::emit_mesh(cs, d);
		if (!gpu.submit_and_wait(cs)) {
			gpu.dump_status("unbatched draw");
			return false;
		}
	}
	const uint32_t unbatched_ticks = uint32_t(read_cntpct() - t0);
	auto s_unbatched = t.finish();
	if (!s_unbatched.ok)
		return false;
	std::ranges::copy(t.lin.span<const uint32_t>(), ref.begin());

	// --- path B: one Context, one submit -------------------------------------
	// Bos must outlive the submit, so the slices are held for the whole batch.
	std::array<Bo, NQuads> vbs{};
	arena.reset();
	t.begin();
	auto t1 = read_cntpct();
	auto cs = gpu.new_cmd_stream(2048);
	etna::Context ctx{cs};
	uint32_t first_dwords = 0, later_dwords = 0;
	for (uint32_t i = 0; i < NQuads; i++) {
		const auto v = quad_i(i);
		vbs[i] = arena.upload(std::span<const float>(v.data(), FloatsPerQuad));
		if (!vbs[i]) {
			print("FAILED: vertex arena exhausted\n");
			return false;
		}
		auto d = t.base();
		d.vtx = &vbs[i];
		d.vertex_count = VertsPerQuad;
		if (!ctx.draw(d)) {
			print("FAILED: command stream full after ", i, " batched draws\n");
			return false;
		}
		if (i == 0)
			first_dwords = ctx.last_draw_dwords();
		else if (i == 1)
			later_dwords = ctx.last_draw_dwords();
	}
	ctx.drain();
	if (!gpu.submit_and_wait(cs)) {
		gpu.dump_status("batched draw");
		return false;
	}
	const uint32_t batched_ticks = uint32_t(read_cntpct() - t1);
	auto s_batched = t.finish();
	if (!s_batched.ok)
		return false;

	print("batching (", NQuads, " same-state quads):\n");
	print("  per-draw dwords: first ", first_dwords, ", subsequent ", later_dwords, "\n");
	print("  total stream ", cs.offset(), " dwords for ", ctx.draw_count(), " draws\n");
	print("  unbatched ", unbatched_ticks, " ticks (", NQuads, " submits), batched ", batched_ticks,
		  " ticks (1 submit)");
	if (batched_ticks)
		print(" -- ", (unbatched_ticks * 10 / batched_ticks) / 10, ".",
			  (unbatched_ticks * 10 / batched_ticks) % 10, "x");
	print("\n");

	// Same geometry, same state, same order -> the images must match exactly.
	uint32_t diffs = 0;
	auto got = t.lin.span<const uint32_t>();
	for (uint32_t i = 0; i < StateTarget::W * StateTarget::H; i++)
		if (got[i] != ref[i])
			diffs++;
	if (diffs) {
		print("FAILED: batched render differs from unbatched in ", diffs, " pixels\n");
		return false;
	}
	if (s_batched.drawn == 0 || s_batched.drawn != s_unbatched.drawn) {
		print("FAILED: batched drew ", s_batched.drawn, " px, unbatched ", s_unbatched.drawn, "\n");
		return false;
	}
	// The whole point: a repeat draw must not re-emit the pipe.
	if (later_dwords >= first_dwords) {
		print("FAILED: a same-state repeat draw emitted ", later_dwords, " dwords, no better than the first (",
			  first_dwords, ")\n");
		return false;
	}

	// --- uniform-changing workload -------------------------------------------
	// Every draw carries a different transform, so shader state changes and the
	// FE->PE stall is unavoidable. Reported for contrast, not asserted against.
	{
		std::array<Bo, NQuads> uvbs{};
		arena.reset();
		t.begin();
		auto cs2 = gpu.new_cmd_stream(4032);
		etna::Context uctx{cs2};
		std::array<float, 16> mvp{};
		uint32_t u_first = 0, u_later = 0;
		auto t2 = read_cntpct();
		for (uint32_t i = 0; i < NQuads; i++) {
			const auto v = quad_i(i);
			uvbs[i] = arena.upload(std::span<const float>(v.data(), FloatsPerQuad));
			if (!uvbs[i])
				return false;
			// Identity with a per-draw nudge, so the uniform genuinely differs.
			mvp = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, float(i) * 0.001f, 0, 0, 1};
			auto d = t.base();
			d.vtx = &uvbs[i];
			d.vertex_count = VertsPerQuad;
			// The pass-through VS ignores the uniform bank; what is being
			// measured here is the cost of writing it, not its effect.
			d.uniforms = mvp;
			if (!uctx.draw(d)) {
				print("FAILED: command stream full during the uniform batch at draw ", i, "\n");
				return false;
			}
			if (i == 0)
				u_first = uctx.last_draw_dwords();
			else if (i == 1)
				u_later = uctx.last_draw_dwords();
		}
		uctx.drain();
		if (!gpu.submit_and_wait(cs2)) {
			gpu.dump_status("uniform batch");
			return false;
		}
		const uint32_t u_ticks = uint32_t(read_cntpct() - t2);
		print("  uniform-changing batch: per-draw dwords first ", u_first, ", subsequent ", u_later, "; ",
			  u_ticks, " ticks (each draw costs an FE->PE stall -- expected)\n");
	}

	print("GPU batched ", NQuads, " draws into one submission, pixel-identical to per-draw submits. \\o/\n");
	return true;
}

// This test was made to help diagnose a rendering issue that ended up
// being a result of the shader ALU not being reset (running a dp2x8 shader on boot
// fixes it).
bool cube_size_sweep_test(Gpu &gpu)
{
	print("-- cube render/resolve size sweep --\n");
	constexpr uint32_t MAX = 512;
	constexpr uint32_t CLEAR = 0xFF000000;
	// Allocate max-size buffers once, reuse for every (smaller) size.
	Bo rt = gpu.alloc(((MAX + 15) & ~15u) * 4 * MAX);
	Bo depth = gpu.alloc(((MAX + 15) & ~15u) * 2 * MAX);
	Bo lin = gpu.alloc(MAX * MAX * 4);
	Bo vtx = gpu.alloc(sizeof(kCubeVerts));
	Bo vs = gpu.alloc(sizeof(kCubeVs));
	Bo ps = gpu.alloc(sizeof(kPsColorCode));
	static std::array<uint32_t, MAX * MAX> cimg;
	static std::array<uint8_t, MAX * MAX> cband;
	static std::array<float, MAX * MAX> czbuf;
	if (!rt || !depth || !lin || !vtx || !vs || !ps)
		return false;
	std::ranges::copy(kCubeVerts, vtx.span<float>().begin());
	vtx.cpu_fini(RelocWrite);
	std::ranges::copy(kCubeVs, vs.span<uint32_t>().begin());
	vs.cpu_fini(RelocWrite);
	std::ranges::copy(kPsColorCode, ps.span<uint32_t>().begin());
	ps.cpu_fini(RelocWrite);

	constexpr uint32_t S = 512;
	uint32_t fails = 0;
	constexpr std::array<float, 6> angles = {0.9f, 0.3f, 0.6f, 1.2f, 2.4f, 3.9f}; // 0.9 = the demo's verify angle
	for (float angle : angles) {
		const uint32_t pw = (S + 15) & ~15u, ph = (S + 3) & ~3u;
		const uint32_t rtstride = pw * 4, dstride = pw * 2;
		Mat4 m = cube_mvp(angle, 0.5f * tsin(angle * 0.7f), 1.0f); // the DEMO's exact matrix

		// color via RS, DEPTH via CPU-fill (triangle_depth_test does this and
		// works in the demo; the RS depth-clear seems not to take there).
		auto csc = gpu.new_cmd_stream(256);
		etna::clear(csc, rt, pw, ph, CLEAR);
		if (!gpu.submit_and_wait(csc))
			return false;
		std::ranges::fill(depth.span<uint16_t>().first(dstride / 2 * ph), uint16_t(0xFFFF));
		depth.cpu_fini(RelocWrite);

		auto cs = gpu.new_cmd_stream(1024);
		MeshDraw d{.rt = &rt,
				   .rt_stride = rtstride,
				   .vtx = &vtx,
				   .vtx_stride = 28,
				   .vs = &vs,
				   .vs_words = kCubeVs.size(),
				   .vs_temps = 4,
				   .ps = &ps,
				   .ps_words = kPsColorCode.size(),
				   .ps_temps = 2,
				   .ps_out_reg = 1,
				   .uniforms = m,
				   .width = S,
				   .height = S,
				   .vertex_count = 36,
				   .depth = &depth,
				   .depth_stride = dstride};
		etna::emit_mesh(cs, d);
		if (!gpu.submit_and_wait(cs))
			return false;

		// (a) render health from the RAW tiled RT: which face colors appear
		rt.cpu_prep(RelocRead);
		uint32_t faces = 0, drawn_raw = 0;
		for (uint32_t p : rt.span<const uint32_t>().first(rtstride / 4 * ph)) {
			if (p == CLEAR)
				continue;
			drawn_raw++;
			for (unsigned f = 0; f < 6; f++)
				if (p == face_argb(f))
					faces |= 1u << f;
		}

		auto cs2 = gpu.new_cmd_stream(256);
		etna::resolve(cs2, lin, rt, S, S, rtstride, S * 4);
		if (!gpu.submit_and_wait(cs2))
			return false;
		lin.cpu_prep(RelocRead);

		// (b) position-exact vs CPU reference
		cpu_render_cube(m, S, S, {cimg.data(), S * S}, {cband.data(), S * S}, {czbuf.data(), S * S}, CLEAR);
		auto g = lin.span<const uint32_t>();
		uint32_t mm = 0, extra = 0, missing = 0, wrong = 0, miss_face = 0;
		for (uint32_t i = 0; i < S * S; i++) {
			if (cband[i] || g[i] == cimg[i])
				continue;
			mm++;
			if (cimg[i] == CLEAR)
				extra++;
			else if (g[i] == CLEAR) {
				missing++;
				for (unsigned f = 0; f < 6; f++)
					if (cimg[i] == face_argb(f))
						miss_face |= 1u << f; // which CPU faces the GPU dropped
			} else
				wrong++;
		}
		if (mm > 50)
			fails++;
		print("  ang ",
			  int(angle * 100),
			  ": faces 0x",
			  Hex{faces},
			  " | ",
			  mm,
			  " mismatch (missing ",
			  missing,
			  " wrong ",
			  wrong,
			  "), dropped-face 0x",
			  Hex{miss_face},
			  "\n");
	}
	return true;
}
