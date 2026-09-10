#include "aarch64/system_reg.hh" // clean_dcache_range
#include "display.hh"
#include "drivers/hal_cnt.hh" // SystemA35_SYSTICK_Config, udelay
#include "dsi.hh"
#include "ltdc.hh"
#include "panel_st7701s.hh"
#include "print/print.hh"
#include "st7701s.hh"
#include "st7701s_init.hh" // ST7701S::NumWrites
#include "stm32mp2xx.h"
#include <cstdint>
#include <span>

//  LTDC + DSI example -- put a framebuffer on the ER-TFT028-2 (ST7701S) panel
//
// Display pipeline:  framebuffer (DDR) -> LTDC -> DSI host -> D-PHY -> panel
//
// Panel: BuyDisplay ER-TFT028-2, 2.8" 480x480 IPS, ST7701S controller, 2-lane
// MIPI-DSI, RGB888, non-burst sync-pulses video mode (timings and the reasons in
// panel_st7701s.hh). The LTDC pixel clock is HSE-locked (ck_ker_ltdc from PLL4,
// ~21 MHz), below the DSI's 33 MHz throughput; the DSI PHY PLL must be locked
// before the LTDC scans.
//
// After bring-up a key console on the UART lets you change the DSI video mode,
// probe the HS path with a command write, re-init the panel, and swap test
// patterns without reflashing. Press '?' for the list.
//
// Custom devboard ONLY (the EV1 has no DSI panel). Build: `make` (defaults to
// BOARD=devboard, USART1 console).

namespace
{

constexpr uint32_t FbAddr = 0x90000000;
uint32_t g_pixel_clk = 0;
DsiVideoOpts g_vid{};

// Test patterns:
//  1: RGB gradient field, 1px white border (offset/timing errors show as a
//     missing/wrapped edge), x^y hash in green (stride errors scramble it).
//  2: black/white checker with a magenta/cyan center cross and red border.
//  3: 8 vertical color bars (white, yellow, cyan, green, magenta, red, blue, black).
//  w/k/r: solid white / black / red -- "does anything at all change?" tests.
void fill_test_pattern(std::span<uint32_t> fb, char pattern)
{
	using namespace Panel;
	static constexpr uint32_t bars[8] = {
		0xFFFFFFFF, 0xFFFFFF00, 0xFF00FFFF, 0xFF00FF00, 0xFFFF00FF, 0xFFFF0000, 0xFF0000FF, 0xFF000000};
	for (uint32_t y = 0; y < VActive; y++)
		for (uint32_t x = 0; x < HActive; x++) {
			uint32_t px = 0xFF000000;
			switch (pattern) {
				case '1': {
					uint32_t r = (x * 255) / (HActive - 1);
					uint32_t b = (y * 255) / (VActive - 1);
					px = 0xFF000000 | (r << 16) | (((x ^ y) & 0xFF) << 8) | b; // 0xAARRGGBB
					if (x == 0 || y == 0 || x == HActive - 1 || y == VActive - 1)
						px = 0xFFFFFFFF;
				} break;
				case '2':
					if (x == HActive / 2)
						px = 0xFFFF00FF;
					else if (y == VActive / 2)
						px = 0xFF00FFFF;
					else if (y >= VActive - 2 || y <= 1 || x <= 1 || x >= HActive - 2)
						px = 0xFFFF0000;
					else
						px = ((y & 1) && (x & 1)) ? 0xFFFFFFFF : 0xFF000000;
					break;
				case '3':
					px = bars[x * 8 / HActive];
					break;
				case 'w':
					px = 0xFFFFFFFF;
					break;
				case 'r':
					px = 0xFFFF0000;
					break;
				case 'k':
				default:
					break;
			}
			fb[y * HActive + x] = px;
		}
	clean_dcache_range(reinterpret_cast<void *>(FbAddr), HActive * VActive * 4);
}

int console_getc()
{
#if UART == 1
#define BOARD_UART USART1
#elif UART == 6
#define BOARD_UART USART6
#else
#define BOARD_UART USART2
#endif
	if (BOARD_UART->ISR & USART_ISR_ORE)
		BOARD_UART->ICR = USART_ICR_ORECF; // clear overrun so RX keeps going
	if (BOARD_UART->ISR & USART_ISR_RXFNE)
		return BOARD_UART->RDR & 0xFF;
	return -1;
}

// Read the ST7701S back over D0 in LP. Nothing here needs the HS clock or D1,
// so a sane answer proves panel power, IOVCC, reset and the D0 pair, and points
// the search at the HS side (CLK/D1 pairs, lane rate, video timing). A timeout
// on every read points at D0, reset, power, or the DSI host itself.
//   RDID1/2/3 (DA/DB/DC): module IDs (the ER-TFT028-2 returns FF FF FF).
//   RDDPM (0A):  bit7 booster on, bit4 sleep-out, bit3 always 1, bit2 display on
//                -> 0x9C after a good init; 0x08 = still in reset defaults.
//   RDCOLMOD (0C): bits 6:4 = pixel format, 111 = 24-bit (we wrote 0x77; the
//                  low bits are not implemented and read 0 -> 0x70 is correct).
//   RDDST (09): 4 status bytes.
bool panel_readback()
{
	struct
	{
		uint8_t cmd;
		uint8_t len;
		const char *name;
	} regs[] = {
		{0xDA, 1, "RDID1   "},
		{0xDB, 1, "RDID2   "},
		{0xDC, 1, "RDID3   "},
		{0x0A, 1, "RDDPM   "},
		{0x0C, 1, "RDCOLMOD"},
		{0x0B, 1, "RDMADCTL"},
		{0x09, 4, "RDDST   "},
	};
	print("   Panel read-back over D0 (LP):\n");
	uint32_t ok_count = 0;
	for (auto &r : regs) {
		uint8_t buf[4] = {};
		bool ok = dsi_dcs_read(r.cmd, buf, r.len);
		print("     ", r.name, " (0x", Hex{r.cmd}, "): ");
		if (!ok) {
			print("NO RESPONSE  ISR0=0x", Hex{dsi_isr0()}, " ISR1=0x", Hex{dsi_isr1()}, "\n");
			continue;
		}
		ok_count++;
		for (uint32_t i = 0; i < r.len; i++)
			print(Hex{buf[i]}, " ");
		if (r.cmd == 0x0A)
			print((buf[0] & 0x10) ? " sleep-out" : " SLEEP-IN", (buf[0] & 0x04) ? ", display on" : ", DISPLAY OFF");
		if (r.cmd == 0x0C)
			print((buf[0] & 0x70) == 0x70 ? " (24-bit, as written)" : " (NOT 24-bit: bits 6:4 should be 111!)");
		print("\n");
	}
	if (ok_count == 0)
		print("   => panel never answered: check D0P/D0N, RST, VCC/IOVCC, panel FPC seating\n");
	else
		print("   => D0 pair + panel alive; if still no picture, suspect CLK/D1 pairs or HS timing\n");
	return ok_count > 0;
}

// ISR0/ISR1 are clear-on-read, so two samples 100 ms apart tell a start-up
// one-off from a persistent error. ISR1 bit7 (LPWRE) = the DPI pixel FIFO
// overflowed while storing a line -- pixels are being dropped/corrupted if it
// keeps coming back.
void status()
{
	print("DSI ISR0=0x", Hex{dsi_isr0()}, " ISR1=0x", Hex{dsi_isr1()}, " PSR=0x", Hex{dsi_psr()});
	udelay(100'000);
	uint32_t isr0 = dsi_isr0(), isr1 = dsi_isr1();
	print("  ... 100 ms later: ISR0=0x", Hex{isr0}, " ISR1=0x", Hex{isr1});
	print(isr1 & 0x80 ? "  <-- LPWRE recurring: DPI FIFO overflow!\n" : "\n");
	print("    PSR bits: PLL-lock=", (dsi_psr() >> 0) & 1, " clk-stop=", (dsi_psr() >> 2) & 1,
		  " d0-stop=", (dsi_psr() >> 4) & 1, " d1-stop=", (dsi_psr() >> 7) & 1,
		  "  (stop=0 during video means the lane is in HS)\n");
	uint32_t l0 = ltdc_current_line();
	udelay(5000);
	uint32_t l1 = ltdc_current_line();
	print("LTDC line ", l0, " -> ", l1, l0 != l1 ? " (advancing)" : " (FROZEN)", ", LTDC ISR=0x", Hex{ltdc_isr()}, "\n");
	print("Video: mode=", g_vid.mode == 2 ? "burst" : g_vid.mode == 1 ? "non-burst/sync-events" : "non-burst/sync-pulses",
		  ", LP in H-porches=", g_vid.lp_hblank ? "yes" : "no", ", pixel clk=", g_pixel_clk / 1000, " kHz\n");
}

void restart_video()
{
	display_video_restart(g_pixel_clk, g_vid);
	print("video restarted\n");
	status();
}

// HS probe: the panel only takes commands on D0, so a write sent in HS that
// reads back correctly (read in LP) proves the HS clock lane + D0 HS path
// end-to-end at the panel. D1 cannot be probed this way; if this passes and
// video still fails, D1 is the remaining unknown (or the video timing).
// Uses MADCTL (0x36): write 0x08 (BGR bit) in HS, read RDMADCTL, restore 0x00.
void hs_write_test()
{
	ltdc_enable(false);
	dsi_video_stop();
	dsi_command_mode();
	uint8_t before = 0xEE;
	dsi_dcs_read(0x0B, &before, 1);

	dsi_set_write_hs(true);
	const uint8_t set_bgr[] = {0x36, 0x08};
	dsi_dcs_write(set_bgr, 2);
	udelay(1000);
	print("HS test: ISR0=0x", Hex{dsi_isr0()}, " ISR1=0x", Hex{dsi_isr1()}, " after HS write\n");
	dsi_set_write_hs(false);

	uint8_t after = 0xEE;
	bool ok = dsi_dcs_read(0x0B, &after, 1);
	const uint8_t restore[] = {0x36, 0x00};
	dsi_dcs_write(restore, 2); // LP, so the panel is clean whatever happened
	print("HS test: MADCTL before=0x", Hex{before}, " after HS write of 0x08 -> 0x", Hex{after}, ": ");
	if (ok && after == 0x08)
		print("PASS -- CLK + D0 HS path works at the panel\n");
	else
		print("FAIL -- panel did not take an HS write: check CLK+/- pair (and D0 HS levels)\n");

	display_video_restart(g_pixel_clk, g_vid);
}

void reinit_panel()
{
	ltdc_enable(false);
	dsi_video_stop();
	dsi_command_mode();
	display_panel_reset();
	st7701s_send_init();
	print("panel reset + init re-sent\n");
	panel_readback();
	display_video_restart(g_pixel_clk, g_vid);
}

void help()
{
	print("\nKeys:\n"
		  "  1/2/3   test pattern: gradient / checker+cross / color bars\n"
		  "  w/k/r   solid white / black / red\n"
		  "  b       cycle DSI video mode: burst -> non-burst sync-events -> sync-pulses\n"
		  "  l       toggle LP in H-porches of active lines (restarts video)\n"
		  "  h       HS probe: write MADCTL in HS, read back in LP\n"
		  "  i       panel hardware reset + re-send init sequence, then read back\n"
		  "  p       panel read-back (IDs, power mode, pixel format)\n"
		  "  v       stop + restart the video stream\n"
		  "  s       status (DSI errors x2, PHY, LTDC line counter)\n"
		  "  ?       this help\n");
}

} // namespace

int main()
{
	print("\nLTDC + DSI Example (ST7701S / ER-TFT028-2)\n");
	print("==========================================\n\n");

	SystemA35_SYSTICK_Config(0);

	auto fb = std::span<uint32_t>{reinterpret_cast<uint32_t *>(FbAddr), Panel::HActive * Panel::VActive};
	fill_test_pattern(fb, '1');
	print("1. Test pattern at 0x", Hex{FbAddr}, " (", Panel::HActive, "x", Panel::VActive, ")\n");

	display_rif_setup();
	print("2. RIF: LTDC + DSI secure, LTDC DMA master (RIMU 11) MSEC\n");

	g_pixel_clk = display_clocks_setup();
	print("3. Clocks on, resets pulsed, ck_ker_dsiphy = HSE/2 = 20 MHz\n");
	print("   LTDC pixel clock = ",
		  g_pixel_clk / 1'000'000,
		  " MHz (",
		  g_pixel_clk / (Panel::HTotal * Panel::VTotal),
		  " Hz refresh)\n");
	print("   DSI version 0x", Hex{dsi_version()}, ", LTDC version 0x", Hex{ltdc_hw_version()}, "\n");

	if (!dsi_pll_init()) {
		print("FAILED: DSI D-PHY PLL never locked / lanes never reached stop-state\n");
		while (true)
			asm volatile("wfe");
	}
	print("4. DSI PLL locked (", Panel::LaneRateMbps, " Mbps/lane, ", Panel::NumLanes, " lanes), PHY up\n");

	dsi_command_mode();
	print("5. DSI host on, low-power command mode\n");

	display_panel_reset();
	st7701s_send_init();
	print("6. ST7701S reset + init sequence sent (", ST7701S::NumWrites, " DCS writes)\n");
	print("   DSI ISR0=0x", Hex{dsi_isr0()}, " ISR1=0x", Hex{dsi_isr1()}, " PSR=0x", Hex{dsi_psr()}, "\n");
	panel_readback();

	ltdc_init(FbAddr);
	print("7. LTDC configured + enabled\n");

	g_vid.mode = Panel::VideoModeType;
	g_vid.lp_hblank = Panel::LpDuringActiveHBlank;
	display_video_restart(g_pixel_clk, g_vid); // LTDC gated off while the DSI starts
	print("8. DSI video mode on (RGB888, ",
		  g_vid.mode == 2 ? "burst" : g_vid.mode == 1 ? "non-burst/sync-events" : "non-burst/sync-pulses",
		  ") -- pixels streaming\n");

	display_backlight_on();
	print("9. Backlight on\n");
	udelay(50'000);
	status();

	help();

	while (true) {
		int c = console_getc();
		if (c < 0) {
			udelay(1000);
			continue;
		}
		switch (c) {
			case '1':
			case '2':
			case '3':
			case 'w':
			case 'k':
			case 'r':
				fill_test_pattern(fb, static_cast<char>(c));
				print("pattern '", char(c), "'\n");
				break;
			case 'b':
				g_vid.mode = (g_vid.mode == 2) ? 1 : (g_vid.mode == 1) ? 0 : 2;
				restart_video();
				break;
			case 'l':
				g_vid.lp_hblank = !g_vid.lp_hblank;
				restart_video();
				break;
			case 'h':
				hs_write_test();
				break;
			case 'i':
				reinit_panel();
				break;
			case 'p':
				panel_readback();
				break;
			case 'v':
				restart_video();
				break;
			case 's':
				status();
				break;
			case '?':
				help();
				break;
			case '\r':
			case '\n':
				break;
			default:
				print("key '", char(c), "'? (press ? for help)\n");
				break;
		}
	}
}

extern "C" void assert_failed(uint8_t *file, uint32_t line)
{
	print("assert failed: ", file, ":", int(line), "\n");
}
