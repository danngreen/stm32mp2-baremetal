#include "aarch64/system_reg.hh"
#include "display.hh"
#include "drivers/hal_cnt.hh"
#include "etna.hh"
#include "gl/mini_gl.hh"
#include "gl/mini_gl_gpu.hh"
#include "ltdc.hh"
#include "print/print.hh"
#include "psketch.hh"

#ifdef DEVBOARD_0_1
#include "panel_ili9881c.hh" // custom devboard: ER-TFT050-10, 720x1280 MIPI-DSI
#else
#include "panel_etml0700z9.hh" // EV1: B-LVDS7-WSVGA, 1024x600 LVDS
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>

// =============================================================================
//  gpu-miniGL -- a Processing sketch on the panel, drawn through mini-GL
// =============================================================================
//
// The frame loop is gpu-ltdc-demo's (double-buffered, vblank-flipped), but the
// rendering is a hand-translated Processing sketch (sketch.cc) making ordinary
// GL 1.x calls into mini-GL (gpu/gl/), which batches each frame into one GPU
// submission and resolves it straight into the LTDC back buffer
// (GpuBackend::set_scanout).

using namespace mgl;

namespace
{
using namespace Panel; // EV1 LVDS: 1024x600  /  devboard DSI: 720x1280
constexpr uint32_t FbStride = HActive * 4;
constexpr uint32_t FbSize = FbStride * VActive;
} // namespace

void panic()
{
	while (true)
		asm volatile("wfe");
}

int main()
{
	print("\nmini-GL -> LTDC: a Processing sketch on the panel\n");
	print("=================================================\n\n");

	SystemA35_SYSTICK_Config(0);

	etna::Gpu gpu;
	if (!gpu.init()) {
		print("FAILED: GPU init\n");
		panic();
	}

	// The sketch is 2D, so no depth buffer -- GL_DEPTH_TEST would be inert.
	static mgl::GpuBackend be;
	if (!be.init(gpu, HActive, VActive, /*with_depth=*/false)) {
		print("FAILED: mini-GL backend init\n");
		panic();
	}
	mglInit(be);

	// Double-buffered scanout. The backend resolves into the back buffer each
	// frame; LTDC latches the flip at vblank.
	std::array<etna::Bo, 2> fbs = {gpu.alloc(FbSize), gpu.alloc(FbSize)};
	if (!fbs[0] || !fbs[1]) {
		print("FAILED: framebuffer alloc\n");
		panic();
	}
	for (auto &fb : fbs) {
		std::ranges::fill(fb.span<uint32_t>(), 0xFF000000);
		fb.cpu_fini(etna::RelocWrite);
	}

	width = HActive;
	height = VActive;
	sketch_setup();

	if (!display_init(fbs[0].gpu_addr())) {
		print("FAILED: display PLL never locked\n");
		panic();
	}
	print("Display up: ", int(HActive), "x", int(VActive), ", sketch running\n");

	std::atomic<bool> frame_ready{};
	ltdc_set_callback([&] { frame_ready.store(true, std::memory_order_release); });

	uint32_t cur = 1; // fbs[0] is being scanned; render into fbs[1] first
	uint32_t frames = 0;
	auto t0 = read_cntpct();
	const uint32_t tick_khz = read_cntfreq() / 1000;
	uint32_t worst_us = 0;
	uint64_t next_render = 0;

	while (true) {
		if (!frame_ready.load(std::memory_order_acquire))
			continue;
		frame_ready.store(false, std::memory_order_release);

		// frameRate() throttle: a slow sketch (frameRate(1)) renders on the
		// first vblank after its period elapses; 0 means every vblank.
		const auto r0 = read_cntpct();
		if (r0 < next_render)
			continue;
		next_render = r0 + uint64_t(psk_frame_period_us()) * tick_khz / 1000;

		be.set_scanout(&fbs[cur], FbStride);
		mglBeginFrame();
		psk_frame_begin();
		sketch_draw();
		mglEndFrame();
		frameCount++;

		if (be.overflowed()) {
			print("FAILED: backend out of arena or command stream\n");
			panic();
		}
		if (glGetError() != GL_NO_ERROR) {
			print("FAILED: sketch raised a GL error\n");
			panic();
		}

		const uint32_t render_us = (read_cntpct() - r0) * 1000 / tick_khz;
		worst_us = std::max(worst_us, render_us);

		ltdc_set_framebuffer(fbs[cur].gpu_addr()); // vblank-latched flip
		cur ^= 1;

		// Immediate feedback for slow sketches (frameRate(1) would otherwise
		// stay silent for two minutes before the first stats line).
		if (frames == 0)
			print("first frame: ", render_us, " us, ", be.draws_submitted(), " draw(s), ", be.stream_dwords(),
				  " dwords\n");

		if (++frames % 120 == 0) {
			const auto now = read_cntpct();
			const uint32_t us = (now - t0) * 1000 / 120 / tick_khz;
			print(us ? (1000000 + us / 2) / us : 0, " fps, worst render ", worst_us, " us, ", be.draws_submitted(),
				  " draw(s), ", be.stream_dwords(), " dwords\n");
			t0 = now;
			worst_us = 0;
		}
	}
}

extern "C" void assert_failed(uint8_t *file, uint32_t line)
{
	print("assert failed: ", file, ":", int(line), "\n");
}
