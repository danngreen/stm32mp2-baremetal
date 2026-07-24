#include "aarch64/system_reg.hh"
#include "display.hh"
#include "dma2d.hh"
#include "drivers/hal_cnt.hh"
#include "ltdc.hh"
#ifdef DEVBOARD_0_1
#include "panel_ili9881c.hh" // custom devboard: ER-TFT050-10, 720x1280 MIPI-DSI
#else
#include "panel_etml0700z9.hh" // EV1: B-LVDS7-WSVGA, 1024x600 LVDS
#endif
#include "print/print.hh"
#include <atomic>
#include <cstdint>

// =============================================================================
//  gui2d-demo -- 2D rendering straight into the framebuffer (no GPU)
// =============================================================================
// Draw strategies to compare (set with `Mode` below)
//   FullCpu       : CPU repaints the whole screen every frame
//   DirtyCpu      : CPU erases+redraws only the moving sprites (dirty rectangles)
//   DirtyDma      : same dirty rectangles, but HPDMA does the fills/blits, one
//                   blocking transfer per rect (CPU spins on each)
//   DirtyDmaAsync : same rects queued as one HPDMA linked list, fired once. The
//                   CPU issues the whole batch and is free while the DMA runs
//                   (issue cost is ~constant regardless of rect count)

namespace
{
using namespace Panel;

enum class Draw { FullCpu, DirtyCpu, DirtyDma, DirtyDmaAsync };
constexpr Draw Mode = Draw::DirtyDmaAsync;
constexpr bool UseDma = (Mode == Draw::DirtyDma || Mode == Draw::DirtyDmaAsync);

constexpr uint32_t FbStride = HActive * 4; // ARGB8888, one linear buffer per frame
constexpr uint32_t FbSize = FbStride * VActive;

constexpr uint32_t align1m(uint32_t s)
{
	return (s + 0xFFFFFu) & ~0xFFFFFu;
}

constexpr uint32_t FbAddr0 = 0x90000000;
constexpr uint32_t FbAddr1 = FbAddr0 + align1m(FbSize);
constexpr uint32_t BgAddr = FbAddr1 + align1m(FbSize);	   // clean background (gradient) for DMA erase
constexpr uint32_t ColorAddr = BgAddr + align1m(FbSize);   // scratch word: fill color source for HPDMA
constexpr uint32_t NodeAddr = ColorAddr + align1m(FbSize); // async linked-list nodes+colors (1MB => 64KB-aligned)

constexpr uint32_t NSprites = 10;
struct Sprite {
	int x, y, w, h;	  // current rect (top-left + size)
	int vx, vy;		  // velocity, px/frame
	uint32_t color;	  // ARGB8888
	int dx[2], dy[2]; // where it was last drawn into buffer 0 / 1 (for dirty-rect erase)
};

inline uint32_t *fb(uint32_t base)
{
	return reinterpret_cast<uint32_t *>(base);
}

// Static background: a smooth two-axis gradient, recomputable per pixel (so the
// dirty-rect path can restore the background under a sprite without keeping a
// separate clean copy).
inline uint32_t bg_at(uint32_t x, uint32_t y)
{
	uint32_t r = 20 + (x * 60) / HActive;
	uint32_t g = 24 + (y * 40) / VActive;
	uint32_t b = 40 + (x * 40) / HActive + (y * 60) / VActive;
	return 0xFF000000 | (r << 16) | (g << 8) | (b & 0xFF);
}

// Bytes touched this frame (CPU writes) -- reported to compare the two strategies.
uint32_t g_dirty_bytes = 0;

int clampi(int v, int lo, int hi)
{
	return v < lo ? lo : (v > hi ? hi : v);
}

// Fill a rect (clamped to the screen) from the background gradient.
void paint_bg_rect(uint32_t base, int x0, int y0, int w, int h)
{
	int xa = clampi(x0, 0, HActive), xb = clampi(x0 + w, 0, HActive);
	int ya = clampi(y0, 0, VActive), yb = clampi(y0 + h, 0, VActive);
	auto *p = fb(base);
	for (int y = ya; y < yb; y++)
		for (int x = xa; x < xb; x++)
			p[y * HActive + x] = bg_at(x, y);
	g_dirty_bytes += uint32_t((xb - xa > 0 ? xb - xa : 0)) * uint32_t((yb - ya > 0 ? yb - ya : 0)) * 4;
}

// Fill a rect (clamped) with a solid color.
void paint_solid_rect(uint32_t base, int x0, int y0, int w, int h, uint32_t color)
{
	int xa = clampi(x0, 0, HActive), xb = clampi(x0 + w, 0, HActive);
	int ya = clampi(y0, 0, VActive), yb = clampi(y0 + h, 0, VActive);
	auto *p = fb(base);
	for (int y = ya; y < yb; y++)
		for (int x = xa; x < xb; x++)
			p[y * HActive + x] = color;
	g_dirty_bytes += uint32_t((xb - xa > 0 ? xb - xa : 0)) * uint32_t((yb - ya > 0 ? yb - ya : 0)) * 4;
}

// Clean the dcache for a rect so the LTDC DMA reads our writes (per-row: the
// framebuffer is strided, so a rect is not contiguous in memory).
void clean_rect(uint32_t base, int x0, int y0, int w, int h)
{
	int xa = clampi(x0, 0, HActive), xb = clampi(x0 + w, 0, HActive);
	int ya = clampi(y0, 0, VActive), yb = clampi(y0 + h, 0, VActive);
	if (xb <= xa)
		return;
	for (int y = ya; y < yb; y++)
		clean_dcache_range(reinterpret_cast<void *>(base + uint32_t(y) * FbStride + uint32_t(xa) * 4),
						   uint32_t(xb - xa) * 4);
}

// Paint the whole buffer with just the background gradient (used both to seed the
// framebuffers and to build the clean background the DMA erase blits from).
void paint_bg_full(uint32_t base)
{
	for (uint32_t y = 0; y < VActive; y++)
		for (uint32_t x = 0; x < HActive; x++)
			fb(base)[y * HActive + x] = bg_at(x, y);
	clean_dcache_range(reinterpret_cast<void *>(base), FbSize);
}

void paint_full(uint32_t base, const Sprite *sp)
{
	paint_bg_full(base);
	for (uint32_t i = 0; i < NSprites; i++)
		paint_solid_rect(base, sp[i].x, sp[i].y, sp[i].w, sp[i].h, sp[i].color);
	g_dirty_bytes = FbSize; // whole screen
	clean_dcache_range(reinterpret_cast<void *>(base), FbSize);
}
} // namespace

int main()
{
	print("\ngui2d-demo -- 2D direct-to-framebuffer, ",
		  Mode == Draw::FullCpu	 ? "full-repaint (CPU)" :
		  Mode == Draw::DirtyCpu ? "dirty-rect (CPU)" :
		  Mode == Draw::DirtyDma ? "dirty-rect (HPDMA, blocking)" :
								   "dirty-rect (HPDMA, async linked-list)",
		  "\n");
	print("=====================================================\n\n");

	SystemA35_SYSTICK_Config(0);

	static const uint32_t palette[10] = {0xFFE23D3D,
										 0xFF3DBB4A,
										 0xFF3D6FE2,
										 0xFFE2B23D,
										 0xFFE23DBE,
										 0xFF3DD0D0,
										 0xFFE2803D,
										 0xFF9A6BFF,
										 0xFF6BFF9A,
										 0xFFDCDCEC};

	Sprite sp[NSprites];
	for (uint32_t i = 0; i < NSprites; i++) {
		sp[i].w = 70 + int(i % 4) * 22; // 70..136
		sp[i].h = 70 + int(i % 3) * 30; // 70..130
		sp[i].x = int((i * 137) % (HActive - sp[i].w));
		sp[i].y = int((i * 251) % (VActive - sp[i].h));
		sp[i].vx = (1 + int(i % 3)) * ((i & 1) ? 1 : -1);
		sp[i].vy = (1 + int(i % 4)) * ((i & 2) ? 1 : -1);
		sp[i].color = palette[i % 10];
		for (int b = 0; b < 2; b++) {
			sp[i].dx[b] = sp[i].x;
			sp[i].dy[b] = sp[i].y;
		}
	}

	// Paint both buffers fully once (background + sprites at start positions).
	paint_full(FbAddr0, sp);
	paint_full(FbAddr1, sp);

	if (UseDma) {
		paint_bg_full(BgAddr);			  // clean background the HPDMA erase blits from
		dma2d::init(ColorAddr, NodeAddr); // HPDMA1 ch12 secure; fill-color + node region
	}

	if (!display_init(FbAddr0)) {
		print("FAILED: display PLL never locked\n");
		while (true)
			asm volatile("wfe");
	}
	print(HActive, "x", VActive, ", two buffers at 0x", Hex{FbAddr0}, " / 0x", Hex{FbAddr1}, "\n");

	std::atomic<bool> frame_ready{};
	ltdc_set_callback([&] { frame_ready.store(true, std::memory_order_release); });

	uint32_t cur = 1; // buffer 0 is being scanned; draw into buffer 1 first
	uint32_t frames = 0;
	auto t0 = read_cntpct();
	const uint32_t tick_khz = read_cntfreq() / 1000;
	uint32_t worst_us = 0, worst_issue_us = 0, dirty_kib = 0;

	while (true) {
		if (!frame_ready.load(std::memory_order_acquire))
			continue;
		frame_ready.store(false, std::memory_order_release);

		uint32_t base = cur ? FbAddr1 : FbAddr0;
		g_dirty_bytes = 0;
		auto r0 = read_cntpct();
		uint32_t issue_us = 0;

		if (Mode == Draw::FullCpu) {
			paint_full(base, sp);
		} else if (Mode == Draw::DirtyDmaAsync) {
			// Queue every erase-blit + fill as one HPDMA linked list
			dma2d::batch_begin();
			for (uint32_t i = 0; i < NSprites; i++)
				dma2d::batch_copy(base, BgAddr, FbStride, sp[i].dx[cur], sp[i].dy[cur], sp[i].w, sp[i].h);
			for (uint32_t i = 0; i < NSprites; i++) {
				dma2d::batch_fill(base, FbStride, sp[i].x, sp[i].y, sp[i].w, sp[i].h, sp[i].color);
				g_dirty_bytes += uint32_t(sp[i].w) * uint32_t(sp[i].h) * 4 * 2; // erase + fill
				sp[i].dx[cur] = sp[i].x;
				sp[i].dy[cur] = sp[i].y;
			}
			dma2d::batch_commit(); // fire; the CPU is free from here until batch_wait()
			issue_us = (read_cntpct() - r0) * 1000 / tick_khz;
			dma2d::batch_wait(); // list must complete before the flip latches this buffer
		} else {
			// CPU or blocking HPDMA mode: erase each sprite's footprint
			// from 2 frames ago (its last position in this buffer), then draw all
			// sprites at their current positions.
			for (uint32_t i = 0; i < NSprites; i++) {
				int ox = sp[i].dx[cur], oy = sp[i].dy[cur];
				if (Mode == Draw::DirtyDma) {
					dma2d::copy(base, BgAddr, FbStride, ox, oy, sp[i].w, sp[i].h);
				} else {
					paint_bg_rect(base, ox, oy, sp[i].w, sp[i].h);
					clean_rect(base, ox, oy, sp[i].w, sp[i].h);
				}
			}
			for (uint32_t i = 0; i < NSprites; i++) {
				if (Mode == Draw::DirtyDma) {
					dma2d::fill(base, FbStride, sp[i].x, sp[i].y, sp[i].w, sp[i].h, sp[i].color);
					g_dirty_bytes += uint32_t(sp[i].w) * uint32_t(sp[i].h) * 4 * 2; // erase + fill
				} else {
					paint_solid_rect(base, sp[i].x, sp[i].y, sp[i].w, sp[i].h, sp[i].color);
					clean_rect(base, sp[i].x, sp[i].y, sp[i].w, sp[i].h);
				}
				sp[i].dx[cur] = sp[i].x; // remember where we drew it in this buffer
				sp[i].dy[cur] = sp[i].y;
			}
		}

		uint32_t us = (read_cntpct() - r0) * 1000 / tick_khz;
		worst_us = us > worst_us ? us : worst_us;
		worst_issue_us = issue_us > worst_issue_us ? issue_us : worst_issue_us;
		dirty_kib = g_dirty_bytes >> 10;

		ltdc_set_framebuffer(base); // vblank-latched flip
		cur ^= 1;

		// advance sprites (bounce off the edges)
		for (auto &s : sp) {
			s.x += s.vx;
			if (s.x < 0 || s.x + s.w > int(HActive))
				s.vx = -s.vx, s.x += s.vx;
			s.y += s.vy;
			if (s.y < 0 || s.y + s.h > int(VActive))
				s.vy = -s.vy, s.y += s.vy;
		}

		if (++frames % 120 == 0) {
			auto now = read_cntpct();
			uint32_t fps_us = (now - t0) * 1000 / 120 / tick_khz;
			print(fps_us ? 1000000 / fps_us : 0, " fps, worst draw ", worst_us, " us");
			if (Mode == Draw::DirtyDmaAsync)
				print(" (CPU issue ", worst_issue_us, " us, DMA runs async)");
			print(", ", dirty_kib, " KiB/frame (of ", FbSize >> 10, " KiB full)\n");
			t0 = now;
			worst_us = 0;
			worst_issue_us = 0;
		}
	}
}

extern "C" void assert_failed(uint8_t *file, uint32_t line)
{
	print("assert failed: ", file, ":", int(line), "\n");
}
