#include "dsi.hh"
#include "panel_ili9881c.hh"
#include "stm32mp2xx_hal.h"

// =============================================================================
//  dsi.cc -- MP25 DSI host + Synopsys D-PHY, via ST's HAL driver
// =============================================================================
// This drives the DSI through ST's stm32mp2xx_hal_dsi.c so the bring-up reads
// like ST's own examples. All tunable numbers (PLL dividers, the
// per-rate PHY table selector, video timings, PHY transition timers) live
// in panel_ili9881c.hh.
//
// The HAL uses HAL_Delay()/HAL_GetTick() for its sequencing and lock timeouts;
// on this bare-metal target those are backed by the ARM generic timer (see
// shared/drivers/hal_cnt.cc), so SystemA35_SYSTICK_Config() must have run first.
// HAL_DSI_MspInit() is a no-op here -- the DSI bus clock/reset and the PHY
// reference clock are set up in display.cc before dsi_pll_init() is called.

namespace
{
using namespace Panel;

DSI_HandleTypeDef g_dsi; // zero-init => State = HAL_DSI_STATE_RESET
} // namespace

bool dsi_pll_init()
{
	g_dsi.Instance = DSI;
	g_dsi.Init.AutomaticClockLaneControl = DSI_AUTO_CLK_LANE_CTRL_DISABLE; // continuous HS clock
	g_dsi.Init.TXEscapeCkdiv = TxEscapeClkDiv;
	g_dsi.Init.NumberOfLanes = DSI_FOUR_DATA_LANES;
	// PhyDataRate selects the row of ST's D-PHY regulation table (vco/prop/hs_freq)
	// used for the PLL tuning -- 450 Mbps/lane for this panel. PllOdf must match
	// that row's ODF column (see panel hh).
	g_dsi.Init.PhyDataRate = HAL_DSI_DT_450;

	DSI_PLLInitTypeDef pll{};
	pll.WrapPHYFrequency = 0; // unused by the driver
	pll.RefClkin = DsiRefClkinKHz;
	pll.PLLNDIV = PllNdiv;
	pll.PLLIDF = PllIdf;
	pll.PLLODF = PllOdf; // 1 == DSI_PLL_OUT_DIV2

	// HAL_DSI_Init: PLL tuning + lock, PHY power-up, wait for all lanes in stop-state.
	return HAL_DSI_Init(&g_dsi, &pll) == HAL_OK;
}

void dsi_command_mode()
{
	// Send every command class in low power (the panel init goes out in LP before
	// the video stream starts). Mirrors the old all-CMCR-TX-bits configuration.
	DSI_LPCmdTypeDef lp{};
	lp.LPGenShortWriteNoP = DSI_LP_GSW0P_ENABLE;
	lp.LPGenShortWriteOneP = DSI_LP_GSW1P_ENABLE;
	lp.LPGenShortWriteTwoP = DSI_LP_GSW2P_ENABLE;
	lp.LPGenShortReadNoP = DSI_LP_GSR0P_ENABLE;
	lp.LPGenShortReadOneP = DSI_LP_GSR1P_ENABLE;
	lp.LPGenShortReadTwoP = DSI_LP_GSR2P_ENABLE;
	lp.LPGenLongWrite = DSI_LP_GLW_ENABLE;
	lp.LPDcsShortWriteNoP = DSI_LP_DSW0P_ENABLE;
	lp.LPDcsShortWriteOneP = DSI_LP_DSW1P_ENABLE;
	lp.LPDcsShortReadNoP = DSI_LP_DSR0P_ENABLE;
	lp.LPDcsLongWrite = DSI_LP_DLW_ENABLE;
	lp.LPMaxReadPacket = DSI_LP_MRDP_ENABLE;
	lp.AcknowledgeRequest = DSI_ACKNOWLEDGE_DISABLE;
	HAL_DSI_ConfigCommand(&g_dsi, &lp);

	// PHY LP<->HS transition timers (lane-byte-clock cycles), from the panel table.
	DSI_PHY_TimerTypeDef t{};
	t.ClockLaneHS2LPTime = ClockLaneHS2LP;
	t.ClockLaneLP2HSTime = ClockLaneLP2HS;
	t.DataLaneHS2LPTime = DataLaneHS2LP;
	t.DataLaneLP2HSTime = DataLaneLP2HS;
	t.DataLaneMaxReadTime = 0;
	t.StopWaitTime = StopWaitTime;
	HAL_DSI_ConfigPhyTimer(&g_dsi, &t);

	// Enable the host (not the wrapper yet) so the LP DCS init writes transmit.
	__HAL_DSI_ENABLE(&g_dsi);
}

void dsi_dcs_write(const uint8_t *data, uint32_t len)
{
	if (len <= 2)
		HAL_DSI_ShortWrite(&g_dsi,
						   0,
						   (len == 1) ? DSI_DCS_SHORT_PKT_WRITE_P0 : DSI_DCS_SHORT_PKT_WRITE_P1,
						   data[0],
						   (len == 2) ? data[1] : 0);
	else
		// Param1 = DCS command byte, the remaining len-1 bytes are parameters.
		HAL_DSI_LongWrite(&g_dsi, 0, DSI_DCS_LONG_PKT_WRITE, len - 1, data[0], &data[1]);
}

void dsi_video_start(uint32_t pixel_clk_hz)
{
	// Horizontal DSI timings are in lane-byte-clock cycles = ceil(pixels * byte_clk
	// / pixel_clk). pixel_clk is the runtime (PLL4-derived) LTDC clock.
	auto lbcc = [pixel_clk_hz](uint32_t px) {
		return static_cast<uint32_t>((static_cast<uint64_t>(px) * LaneByteClkHz + (pixel_clk_hz - 1)) / pixel_clk_hz);
	};

	DSI_VidCfgTypeDef v{};
	v.VirtualChannelID = 0;
	v.ColorCoding = DSI_RGB888;
	v.LooselyPacked = DSI_LOOSELY_PACKED_DISABLE;
	v.Mode = DSI_VID_MODE_BURST; // buffered active line sent as one HS burst
	v.PacketSize = HActive;
	v.NumberOfChunks = 0;
	v.NullPacketSize = 0;
	v.HSPolarity = DSI_HSYNC_ACTIVE_HIGH;
	v.VSPolarity = DSI_VSYNC_ACTIVE_HIGH;
	v.DEPolarity = DSI_DATA_ENABLE_ACTIVE_HIGH;
	v.HorizontalSyncActive = lbcc(HSync);
	v.HorizontalBackPorch = lbcc(HBack);
	v.HorizontalLine = lbcc(HTotal);
	v.VerticalSyncActive = VSync;
	v.VerticalBackPorch = DsiVBack;
	v.VerticalFrontPorch = DsiVFront;
	v.VerticalActive = VActive;

	// The ILI9881C needs LP during vertical blanking as its frame delimiter
	// (holding vblank in HS blanks the panel), so VSA/VBP/VFP LP are always on.
	v.LPCommandEnable = DSI_LP_COMMAND_ENABLE;
	v.LPVerticalSyncActiveEnable = DSI_LP_VSYNC_ENABLE;
	v.LPVerticalBackPorchEnable = DSI_LP_VBP_ENABLE;
	v.LPVerticalFrontPorchEnable = DSI_LP_VFP_ENABLE;
	// Active-line horizontal-porch LP is the knob the pixel-clock headroom buys:
	// with spare HS bandwidth we can hold the H-porches in HS instead of dropping
	// to LP every line (which wrapped the image). See panel hh.
	v.LPHorizontalFrontPorchEnable = LpDuringActiveHBlank ? DSI_LP_HFP_ENABLE : DSI_LP_HFP_DISABLE;
	v.LPHorizontalBackPorchEnable = LpDuringActiveHBlank ? DSI_LP_HBP_ENABLE : DSI_LP_HBP_DISABLE;
	v.LPVerticalActiveEnable = LpDuringActiveHBlank ? DSI_LP_VACT_ENABLE : DSI_LP_VACT_DISABLE;
	v.LPLargestPacketSize = LpLargestPacket;
	v.LPVACTLargestPacketSize = LpVactLargestPacket;
	v.FrameBTAAcknowledgeEnable = DSI_FBTAA_DISABLE;
	HAL_DSI_ConfigVideoMode(&g_dsi, &v);

	// Start: enable the host (idempotent -- already on from command mode) and the
	// wrapper, so LTDC DPI pixels get packetised onto the link.
	HAL_DSI_Start(&g_dsi);
}

uint32_t dsi_version()
{
	return DSI->VERR;
}
