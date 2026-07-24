#include "display.hh"
#include "drivers/hal_cnt.hh" // udelay
#include "drivers/pin.hh"
#include "drivers/rcc_pll.hh"  // PLL4 read-back for the pixel clock
#include "drivers/rcc_xbar.hh" // FlexbarConf
#include "dsi.hh"
#include "ili9881c.hh"
#include "ltdc.hh"
#include "stm32mp2xx.h"

// =============================================================================
//  display.cc -- custom-devboard wiring for the DSI display path
// =============================================================================
// Pipeline:  framebuffer (DDR) -> LTDC -> DSI host -> D-PHY -> ILI9881C panel.

namespace
{
constexpr GPIO PanelResetPort = GPIO::H;
constexpr uint8_t PanelResetPin = PinNum::_7;
constexpr GPIO BacklightPort = GPIO::B;
constexpr uint8_t BacklightPin = PinNum::_0;

// SYSCFG DISPLAYCLKCR.PIXEL_CLK_SEL -- LTDC pixel-clock source (RM0457):
//   0b00 = DSI clock from the DSI PLL   (loopback; pixel clock locked to the PHY)
//   0b01 = LVDS clock from the LVDS PLL  (what ltdc/ uses)
//   0b10 = ck_ker_ltdc (flexgen ch27)   (independent, for parallel or DSI)
// We drive ck_ker_ltdc independently (below the DSI's pixel throughput) so the
// DSI link has bandwidth headroom -- the loopback (0b00) locks pixel_clk to the
// lane rate exactly (zero headroom), which forced LP on every line and wrapped
// the image. See panel_ili9881c.hh.
constexpr uint32_t PixelClkSel_DSI = 2;

// RIFSC RISUP peripheral id for the DSI host, to expose its register port to
// our secure CA35 context (LTDC = 80, LVDS = 84, LTDC layers = 119). DSI = 81,
// confirmed: RIF_PERIPH_DSI_CMN = REG2 | SEC81 (stm32mp2xx_hal_rif.h:386).
constexpr uint32_t RifscId_DSI = 81;
} // namespace

// RIF: expose the LTDC + DSI register ports to
// our context and make the LTDC's DMA reads secure so they pass the RISAF DDR
// firewall. The DSI is not a DMA master (the LTDC feeds it pixels over DPI), so
// it needs no RIMU entry.
//   RISUP 80 = LTDC common, 119 = LTDC layers, RifscId_DSI = DSI host
//   RIMU  11 = LTDC_L1/L2 master
void display_rif_setup()
{
	RISC->SECCFGR[80 / 32] |= (1u << (80 % 32));
	RISC->SECCFGR[RifscId_DSI / 32] |= (1u << (RifscId_DSI % 32));
	RISC->SECCFGR[119 / 32] |= (1u << (119 % 32));
	auto attr = RIMC->ATTR[11];
	RIMC->ATTR[11] = (attr & ~RIMC_ATTR_CIDSEL) | RIMC_ATTR_MSEC | RIMC_ATTR_MPRIV;
}

uint32_t display_clocks_setup()
{
	// --- ck_ker_dsiphy (flexgen output 28) = HSE 40 MHz / 2 = 20 MHz ----------
	// This is the D-PHY PLL reference (17-27 MHz).
	constexpr unsigned ch = 28;
	RCC->PREDIVxCFGR[ch] = 0;						 // /1
	RCC->XBARxCFGR[ch] = RCC_XBARxCFGR_XBARxEN | 6u; // source = HSE
	while (RCC->XBARxCFGR[ch] & RCC_XBARxCFGR_XBARxSTS)
		;
	RCC->FINDIVxCFGR[ch] = RCC_FINDIVxCFGR_FINDIVxEN | 1u; // /2
	while (RCC->FINDIVSR1 & (1u << ch))
		;

	// --- ck_ker_ltdc (flexgen output 27) = the LTDC pixel clock -----------------
	// Must be derived from the same clock as the DSI byte clock (HSE) so they're
	// phase-locked, otherwise the colors cycle.
	// Tries to use PLL4 first, if that's not driven from HSE, then tries HSE directly.
	using namespace RCC_Clocks;
	constexpr unsigned chp = 27;
	uint32_t pix;
	auto s4 = get_pll_settings<PLL4>();
	uint32_t pll4 = (s4.src == MuxSelSource::hse) ? s4.calc_freq(40'000'000) : 0;
	if (pll4 >= 60'000'000) {
		uint32_t findiv = (pll4 + 67'999'999) / 68'000'000; // ceil -> pix <= 68 MHz
		if (findiv < 1)
			findiv = 1;
		if (findiv > 64)
			findiv = 64;
		FlexbarConf{.PLL = FlexbarConf::PLLx::_4, .findiv = static_cast<uint8_t>(findiv - 1), .prediv = 0}.init(chp);
		pix = pll4 / findiv;
	} else {
		// HSE direct, /1 = 40 MHz (locked, but low refresh)
		RCC->PREDIVxCFGR[chp] = 0;
		RCC->XBARxCFGR[chp] = RCC_XBARxCFGR_XBARxEN | 6u; // source = HSE
		while (RCC->XBARxCFGR[chp] & RCC_XBARxCFGR_XBARxSTS)
			;
		RCC->FINDIVxCFGR[chp] = RCC_FINDIVxCFGR_FINDIVxEN | 0u; // /1
		while (RCC->FINDIVSR1 & (1u << chp))
			;
		pix = 40'000'000;
	}

	// --- bus/kernel clock gates (single gate per IP on MP25, bit 1) -----------
	RCC->LTDCCFGR |= 1u << 1;
	RCC->DSICFGR |= 1u << 1;

	// --- pulse resets (bit 0) -------------------------------------------------
	RCC->LTDCCFGR |= 1u << 0;
	RCC->DSICFGR |= 1u << 0;
	udelay(20);
	RCC->LTDCCFGR &= ~(1u << 0);
	RCC->DSICFGR &= ~(1u << 0);

	// --- DSI clock-source muxes (RCC_DSICFGR) ---------------------------------
	RCC->DSICFGR &= ~RCC_DSICFGR_DSIPHYCKREFSEL; // PHY PLL ref = ck_ker_dsiphy (20 MHz)
	RCC->DSICFGR &= ~RCC_DSICFGR_DSIBLSEL;		 // byte-lane clock = DSI-PHY

	// --- LTDC pixel clock source = ck_ker_ltdc (SYSCFG mux 0b10) --------------
	SYSCFG->DISPLAYCLKCR = PixelClkSel_DSI;
	return pix;
}

void display_panel_reset()
{
	// ILI9881C RST is active-low: hold low, release, then let the controller
	// settle (datasheet: >=10 ms after release).
	Pin rst{PanelResetPort, PanelResetPin, PinMode::Output};
	rst.low();
	udelay(10'000);
	rst.high();
	udelay(120'000);
}

void display_backlight_on()
{
	Pin bl{BacklightPort, BacklightPin, PinMode::Output};
	bl.high();
}

bool display_init(uint32_t first_fb, uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t bg_argb)
{
	display_rif_setup();
	uint32_t pixel_clk = display_clocks_setup();
	if (!dsi_pll_init())
		return false;
	dsi_command_mode();
	display_panel_reset();
	ili9881c_send_init();
	ltdc_init(first_fb, x, y, w, h, bg_argb);
	dsi_video_start(pixel_clk);
	display_backlight_on();
	return true;
}
