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

extern "C" int uart_getchar(void); // shared/print/uart_print.c, non-blocking

void panic()
{
	while (true)
		asm volatile("wfe");
}

// Drain the console UART into Processing key events: each typed character
// (from a minicom session on the board's serial port) sets `key` and fires
// the sketch's keyPressed(). Polled every loop spin so a frameRate()-throttled
// sketch still gets its keys promptly.
static void poll_keys()
{
	for (int c; (c = uart_getchar()) >= 0;) {
		// Enter stands in for a mouse click at the cursor (screen centre)
		// until there is a real pointer -- enough to reach the content of the
		// many examples that are gated behind mousePressed().
		if (c == '\r' || c == '\n') {
			print("(synthetic click at ", mouseX, ",", mouseY, ")\n");
			_mousePressed = true;
			mousePressed();
			continue;
		}
		key = char(c);
		keyCode = c;
		_keyPressed = true;
		const char echo[2] = {key, 0}; // print() has no char overload
		print("key: '", echo, "'\n");
		keyPressed();
	}
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

	// A depth buffer costs 1.8 MB of the 64 MB pool and is inert for a 2D
	// sketch (psk_frame_begin disables the test), so allocate it always
	// rather than depending on setup() having run -- size(.., P3D) is called
	// inside setup(), which needs the backend up first.
	// 8 MB vertex arena: a dense sketch (Game of Life is ~14 MB of vertex data
	// a frame) then splits into 2 arena flushes instead of 28.
	static mgl::GpuBackend be;
	if (!be.init(gpu, HActive, VActive, /*with_depth=*/true, 8 * 1024 * 1024)) {
		print("FAILED: mini-GL backend init\n");
		panic();
	}
#ifdef DIRECT_LINEAR
	// make BOARD=devboard DIRECT_LINEAR=1 -- render clear-first frames straight
	// into the scanout buffer, skipping the resolve. A trade, not a free win:
	// see set_direct_linear() in gl/mini_gl_gpu.hh.
	be.set_direct_linear(true);
	print("direct-linear scanout: ON\n");
#endif
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

	// Run setup() inside a real frame, resolved into the first scanout
	// buffer: Processing presents whatever setup() draws (many sketches only
	// ever clear or paint here -- Recursion draws once under noLoop()), and
	// backend clears/draws are no-ops outside a frame.
	be.set_scanout(&fbs[0], FbStride);
	mglBeginFrame();
	psk_frame_begin();
	glClearColor(0, 0, 0, 1); // defined RT contents even if setup draws nothing
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	sketch_setup();
	psk_frame_end();
	mglEndFrame();
	// A "static mode" sketch does all its drawing here and leaves draw()
	// empty, so this is the only evidence it rendered anything at all.
	print("setup frame: ", be.draws_submitted(), " draw(s), ", be.stream_dwords(), " dwords",
		  psk_wants_3d() ? " [P3D]" : "", "\n");

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
	bool started = false;

	while (true) {
		poll_keys();
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
		psk_frame_end(); // deferred strokes go out here
		mglEndFrame();

		// updatePixels(): the sketch's own pixel array, straight into the
		// buffer about to be scanned out -- after the resolve, so it wins.
		if (const int *px = psk_take_pixels()) {
			auto dst = fbs[cur].span<uint32_t>();
			std::copy_n(reinterpret_cast<const uint32_t *>(px), size_t(HActive) * VActive, dst.begin());
			fbs[cur].cpu_fini(etna::RelocWrite);
		}
		frameCount++;
		// The "held" booleans last one frame per event: a serial console has
		// no key-up or button-up to end them.
		_keyPressed = false;
		if (_mousePressed) {
			_mousePressed = false;
			mouseReleased();
		}

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

		// Report on a TIME interval, not a frame count: a sketch rendering at
		// 1 fps would otherwise take two minutes to say anything, which reads
		// as a hang. Average frame time is the honest number at any speed --
		// fps alone rounds to 0 below one frame a second.
		//
		// The measurement window starts at a COMPLETED frame, not at boot:
		// `frames` counts intervals, so including the startup gap ahead of
		// the first frame made a 1 fps sketch read as 1.5 fps.
		if (!started) {
			started = true;
			// Immediate feedback for slow sketches (frameRate(1) would
			// otherwise stay silent for two minutes).
			print("first frame: ", render_us, " us, ", be.draws_submitted(), " draw(s), ", be.stream_dwords(),
				  " dwords\n");
			t0 = read_cntpct();
			frames = 0;
			worst_us = 0;
		} else {
			frames++;
			const auto now = read_cntpct();
			const uint32_t elapsed_us = (now - t0) * 1000 / tick_khz;
			if (elapsed_us >= 2000000) {
				const uint32_t avg_us = elapsed_us / frames;
				print(avg_us ? (1000000 + avg_us / 2) / avg_us : 0, " fps (avg ", avg_us, " us/frame, worst render ",
					  worst_us, " us), ", be.draws_submitted(), " draw(s), ", be.stream_dwords(), " dwords\n");
				t0 = now;
				worst_us = 0;
				frames = 0;
			}
		}
	}
}

extern "C" void assert_failed(uint8_t *file, uint32_t line)
{
	print("assert failed: ", file, ":", int(line), "\n");
}
