#include "aarch64/system_reg.hh" // clean_dcache_range
#include "display.hh"
#include "drivers/hal_cnt.hh" // SystemA35_SYSTICK_Config, udelay
#include "dsi.hh"
#include "ili9881c.hh"
#include "ltdc.hh"
#include "panel_ili9881c.hh"
#include "print/print.hh"
#include "stm32mp2xx.h"
#include <cstdint>
#include <span>

//  LTDC + DSI example -- put a framebuffer on the ER-TFT050-10 (ILI9881C) panel
//
// Display pipeline:  framebuffer (DDR) -> LTDC -> DSI host -> D-PHY -> panel
//
// Panel: BuyDisplay ER-TFT050-10, 5" 720x1280 IPS, ILI9881C controller, 4-lane
// MIPI-DSI, RGB888, DSI burst video mode (timings in panel_ili9881c.hh). The DSI
// runs in burst mode off an HSE-locked LTDC pixel clock (ck_ker_ltdc from PLL4,
// ~66 MHz) that is deliberately below the DSI's 75 MHz throughput so the link has
// headroom; the DSI PHY PLL must be locked before the LTDC scans.
//
// Custom devboard ONLY (the EV1 has no DSI panel). Build: `make` (defaults to
// BOARD=devboard, USART1 console).

namespace
{

constexpr uint32_t FbAddr = 0x90000000;

#define PATTERN_TEST 1

// A test pattern that makes scanout bugs obvious: RGB gradient field, 1px white
// border (offset/timing errors show as a missing/wrapped edge), and the x^y hash
// in green (stride errors scramble it).
void fill_test_pattern(std::span<uint32_t> fb)
{
	using namespace Panel;
	for (uint32_t y = 0; y < VActive; y++)
		for (uint32_t x = 0; x < HActive; x++) {

#if PATTERN_TEST == 1
			uint32_t r = (x * 255) / (HActive - 1);
			uint32_t b = (y * 255) / (VActive - 1);
			uint32_t px = 0xFF000000 | (r << 16) | (((x ^ y) & 0xFF) << 8) | b; // 0xAARRGGBB
			if (x == 0 || y == 0 || x == HActive - 1 || y == VActive - 1)
				px = 0xFFFFFFFF; // 1px white border

#elif PATTERN_TEST == 2
			// border around edges, and center-line cross
			// every other pixel is black/white
			uint32_t px = 0;
			if (x == HActive / 2)
				px = 0xFFFF00FF;
			else if (y == VActive / 2)
				px = 0xFF00FFFF;
			else if (y >= VActive - 2 || y <= 1 || x <= 1 || x >= HActive - 2)
				px = 0xFFFF0000;
			else
				px = ((y & 1) && (x & 1)) ? 0xFFFFFFFF : 0xFF000000;
#endif
			fb[y * HActive + x] = px;
		}
	clean_dcache_range(reinterpret_cast<void *>(FbAddr), HActive * VActive * 4);
}

} // namespace

int main()
{
	print("\nLTDC + DSI Example (ILI9881C / ER-TFT050-10)\n");
	print("============================================\n\n");

	SystemA35_SYSTICK_Config(0);

	auto fb = std::span<uint32_t>{reinterpret_cast<uint32_t *>(FbAddr), Panel::HActive * Panel::VActive};
	fill_test_pattern(fb);
	print("1. Test pattern at 0x", Hex{FbAddr}, " (", Panel::HActive, "x", Panel::VActive, ")\n");

	display_rif_setup();
	print("2. RIF: LTDC + DSI secure, LTDC DMA master (RIMU 11) MSEC\n");

	uint32_t pixel_clk = display_clocks_setup();
	print("3. Clocks on, resets pulsed, ck_ker_dsiphy = HSE/2 = 20 MHz\n");
	print("   LTDC pixel clock = ",
		  pixel_clk / 1'000'000,
		  " MHz (",
		  pixel_clk / (Panel::HTotal * Panel::VTotal),
		  " Hz refresh)\n");
	print("   DSI version 0x", Hex{dsi_version()}, ", LTDC version 0x", Hex{ltdc_hw_version()}, "\n");

	if (!dsi_pll_init()) {
		print("FAILED: DSI D-PHY PLL never locked / lanes never reached stop-state\n");
		while (true)
			asm volatile("wfe");
	}
	print("4. DSI PLL locked (", Panel::LaneRateMbps, " Mbps/lane, 4 lanes), PHY up\n");

	dsi_command_mode();
	print("5. DSI host on, low-power command mode\n");

	display_panel_reset();
	ili9881c_send_init();
	print("6. ILI9881C reset + init sequence sent (", Panel::InitWrites, " DCS writes)\n");

	ltdc_init(FbAddr);
	print("7. LTDC configured + enabled\n");

	dsi_video_start(pixel_clk);
	print("8. DSI video mode on (RGB888, burst) -- pixels streaming\n");

	display_backlight_on();
	print("9. Backlight on\n");

	// --- verify scanout is alive (the LTDC line counter advances iff the DSI
	// PHY pixel clock is reaching it through the SYSCFG mux) -------------------
	uint32_t l0 = ltdc_current_line();
	udelay(5000);
	uint32_t l1 = ltdc_current_line();
	print("\nLTDC line counter: ", l0, " -> ", l1, l0 != l1 ? "  (advancing \\o/)\n" : "  (FROZEN: no pixel clock!)\n");
	print("LTDC ISR flags: 0x", Hex{ltdc_isr()}, "  (bit1 FIFO-warn, bit2 xfer-err, bit6 FIFO-err)\n");

	if (l0 == l1)
		print("\nFAILED: pixel clock dead -- check DSI PLL lock / SYSCFG mux value\n");
	else
		print("\nScanout running -- look at the panel!\n");

	while (true)
		asm volatile("wfe");
}

extern "C" void assert_failed(uint8_t *file, uint32_t line)
{ print("assert failed: ", file, ":", int(line), "\n"); }
