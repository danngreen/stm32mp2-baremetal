#pragma once
#include <cstdint>

// =============================================================================
//  panel_st7701s.hh -- ER-TFT028-2 (ST7701S) 480x480 DSI panel parameters
// =============================================================================
// BuyDisplay ER-TFT028-2: 2.8" IPS, 480(H)x480(V), ST7701S controller, 2-lane
// MIPI-DSI (+ clock lane), RGB888, video mode. The ST7701S supports 1 or 2 data
// lanes, max 550 Mbps/lane on 2 lanes (datasheet 1.2 Features).
//
// There is no vendor DSI timing table, but the vendor init sequence programs
// the panel's own timing generator (st7701s_init.hh, BK0):
//   LNESET  C0 = 3B 00 -> 480 lines
//   PORCTRL C1 = 10 0C -> VBP = 16, VFP = 12
//   INVSET  C2 = 07 0A -> RTNI = 0x0A -> minimum 512 + 10*16 = 672 pclk per line
// so the mode below uses those vertical porches verbatim and an HTotal just
// above the 672-clock minimum. 60 Hz at 680 x 512 needs a ~20.9 MHz pixel clock.
// LTDC register fields are derived with the same formulas Linux ltdc.c uses.
//
// Clocking chain:
//   HSE 40 MHz --/2--> ck_ker_dsiphy 20 MHz --[D-PHY PLL]--> 400 Mbps/lane
//   lane byte clock = 400/8 = 50 MHz   (time base for the DSI video regs)
//   DSI pixel throughput = lane_rate*lanes/bpp = 400*2/24 = 33.3 MHz (max)
//   LTDC pixel clock = ck_ker_ltdc = PLL4/N <= PixelClkMaxHz (SYSCFG mux=0b10;
//     display.cc picks N at runtime). PLL4 is HSE-sourced, so the pixel clock
//     stays phase-locked to the (HSE-derived) DSI byte clock -- an unrelated
//     source (HSI) makes the video FIFO slip and cycles the colors. ~21 < 33 MHz
//     is the headroom burst mode needs.  ~20.7 MHz / (HTotal * VTotal) ~= 59 Hz.

namespace Panel
{

// ---- display mode ----------------------------------------------------------
constexpr uint32_t HActive = 480;
constexpr uint32_t HFront = 140; // HFP
constexpr uint32_t HSync = 10;	 // HSA
constexpr uint32_t HBack = 50;	 // HBP   -> HTotal 680 >= 672 (INVSET RTNI minimum)

constexpr uint32_t VActive = 480;
constexpr uint32_t VFront = 12; // VFP  (= PORCTRL VFP)
constexpr uint32_t VSync = 4;	// VSA
constexpr uint32_t VBack = 16;	// VBP  (= PORCTRL VBP)

// The DSI regenerates the vertical frame from its own VBP/VFP and can land one
// line out of phase with the LTDC's DE (symptom: image shifted up by one line,
// top row wraps to the bottom). This offset nudges ONLY the DSI's VBP/VFP (the
// LTDC and the frame total are untouched, so refresh rate is unchanged): +1
// shifts the image DOWN one line. Flip to -1 if it moves the wrong way; 0 =
// matched to the LTDC.
constexpr int DsiVBpOffset = 0;
constexpr uint32_t DsiVBack = VBack + DsiVBpOffset;	  // VVBPCR
constexpr uint32_t DsiVFront = VFront - DsiVBpOffset; // VVFPCR

// Upper bound for the LTDC pixel clock display_clocks_setup() picks from PLL4
// (largest PLL4/N not above this). 680*512*60 Hz = 20.9 MHz. Must stay below the
// DSI pixel throughput (33.3 MHz) for burst mode to have headroom.
constexpr uint32_t PixelClkMaxHz = 21'000'000;

constexpr uint32_t LaneRateMbps = 400; // per-lane HS data rate
constexpr uint32_t NumLanes = 2;

// DRM-style mode points (as ltdc.c consumes them)
constexpr uint32_t HSyncStart = HActive + HFront; // 620
constexpr uint32_t HSyncEnd = HSyncStart + HSync; // 630
constexpr uint32_t HTotal = HSyncEnd + HBack;	  // 680
constexpr uint32_t VSyncStart = VActive + VFront; // 492
constexpr uint32_t VSyncEnd = VSyncStart + VSync; // 496
constexpr uint32_t VTotal = VSyncEnd + VBack;	  // 512

// ---- LTDC accumulated-timing register fields (formulas ltdc.c:1183-1209) ----
constexpr uint32_t SyncW = HSyncEnd - HSyncStart - 1;  // 9
constexpr uint32_t SyncH = VSyncEnd - VSyncStart - 1;  // 3
constexpr uint32_t AccumHbp = HTotal - HSyncStart - 1; // 59
constexpr uint32_t AccumVbp = VTotal - VSyncStart - 1; // 19
constexpr uint32_t AccumActW = AccumHbp + HActive;	   // 539
constexpr uint32_t AccumActH = AccumVbp + VActive;	   // 499
constexpr uint32_t TotalW = HTotal - 1;				   // 679
constexpr uint32_t TotalH = VTotal - 1;				   // 511

// ---- DSI D-PHY PLL solution -------------------------------------------------
// Reference = ck_ker_dsiphy = HSE/2 = 20 MHz (set in display.cc). Target the
// 400 Mbps row of ST's PLL regulation table (dsi.c:339, HAL_DSI_DT_400). Formula
// from ST's downstream dw_mipi_dsi-stm.c (HWVER_141 = MP25):
//   f_vco     = f_ref * NDIV / IDF          = 20 * 80 / 4     = 400 MHz  (band 320..1250)
//   data_rate = 2 * f_ref * NDIV / (IDF*ODF) = 2*20*80/(4*2)  = 400 Mbps/lane  (x2 = DDR)
//   lane byte clk = data_rate / 8 = 50 MHz
// ODF must equal the table's odf column so f_vco lands where the `vco` code is
// tuned: DT_400 -> odf /2, vco 0x0F. Keeping f_vco/vco consistent is what makes
// the PLL lock.
constexpr uint32_t DsiRefClkinKHz = 20'000; // 20 MHz, must be 17-27 MHz
constexpr uint32_t PllNdiv = 80;			// WRPCR0.NDIV (64..625)
constexpr uint32_t PllIdf = 4;				// WRPCR0.IDF  (1..16)
constexpr uint32_t PllOdf = 1;				// WRPCR1.ODF: 0/1/2/3 = /1,/2,/4,/8

// Per-rate PHY regulation values, HAL_DSI_DT_400 row {hs_freq, odf, vco, prop}
// (dsi.c:339) = {0x05, 0x01, 0x0F, 0x0B}; the HAL writes them from PhyDataRate.
constexpr uint32_t PhyHsFreq = 0x05; // WPCR1.HSFR
constexpr uint32_t PhyVco = 0x0F;	 // WRPCR1.VCO
constexpr uint32_t PhyProp = 0x0B;	 // WRPCR1.PROP

// ---- DSI host / video config ------------------------------------------------
// Burst mode: each active line is buffered and sent as one HS burst, which
// decouples the DSI byte clock from the (independent) LTDC pixel clock; this is
// also what Linux's panel-sitronix-st7701 driver uses. Verified 2026-09-10 (with
// the LTDC gated around the DSI start, see display_video_restart): burst = good
// picture, non-burst sync-events and sync-pulses = bad picture on this panel.
// Key 'b' cycles the modes live.
constexpr uint32_t VideoModeType = 2; // DSI_VID_MODE_BURST

// TX escape clock = lane_byte_clock / div <= 20 MHz. 50/4 = 12.5 MHz.
constexpr uint32_t TxEscapeClkDiv = 4;

// Horizontal DSI video-timing regs are in lane-byte-clock cycles: dsi.cc
// converts from pixels by ceil(px * LaneByteClkHz / pixel_clk) at runtime, since
// the LTDC pixel clock is chosen at runtime (PLL4-derived). Vertical regs
// (VVSACR/VVBPCR/VVFPCR/VVACR) are in lines -> VSync/VBack/VFront/VActive.
constexpr uint32_t LaneByteClkHz = LaneRateMbps * 125'000; // rate*1e6/8 = 50 MHz

// Vertical-blanking LP is kept on (standard video-mode frame delimiter; the
// ILI9881C in ltdc-dsi/ required it and the ST7701S datasheet's video-mode
// figures show LP in every blanking period), so dsi.cc always keeps
// LPVSAE/LPVBPE/LPVFPE on.
//
// Active-line horizontal blanking: true = drop to LP in HFP/HBP of every line;
// false = keep the H-porches in HS blanking packets. MUST be true here: with
// false the host's DPI pixel FIFO overflows every line (ISR1 LPWRE recurring)
// and the panel shows noise (verified 2026-09-10). Console key 'l' toggles it.
constexpr bool LpDuringActiveHBlank = true;

// LP packet-size limits during blanking (HAL_DSI_ConfigVideoMode LPMCR).
constexpr uint32_t LpLargestPacket = 4;
constexpr uint32_t LpVactLargestPacket = 0;

// PHY LP<->HS transition timers, lane-byte-clock cycles (HAL_DSI_ConfigPhyTimer).
// From ST's per-rate table hstt_phy_141_table in dw_mipi_dsi-stm.c (HWVER_141 =
// MP25, v6.6-stm32mp): HSTT(400, clk_lp2hs=59, clk_hs2lp=37, data_lp2hs=44,
// data_hs2lp=21). Getting these right (esp. data_lp2hs) is what makes the
// LP->HS transition into each active line land cleanly.
constexpr uint32_t ClockLaneLP2HS = 59;
constexpr uint32_t ClockLaneHS2LP = 37;
constexpr uint32_t DataLaneLP2HS = 44;
constexpr uint32_t DataLaneHS2LP = 21;
constexpr uint32_t StopWaitTime = 10;

} // namespace Panel
