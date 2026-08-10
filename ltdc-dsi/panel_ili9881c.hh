#pragma once
#include <cstdint>

// =============================================================================
//  panel_ili9881c.hh -- ER-TFT050-10 (ILI9881C) 720x1280 DSI panel parameters
// =============================================================================
// BuyDisplay ER-TFT050-10: 5" IPS, 720(H)x1280(V) portrait, ILI9881C controller,
// 4-lane MIPI-DSI, RGB888, video mode. There is no vendor DSI timing table, so
// the porches below are chosen for ~60 Hz (we hit 58fps).
// The ILI9881C is tolerant of the exact blanking. LTDC register fields are
// derived with the same formulas ltdc.c uses
//
// Clocking chain:
//   HSE 40 MHz --/2--> ck_ker_dsiphy 20 MHz --[D-PHY PLL]--> 450 Mbps/lane
//   lane byte clock = 450/8 = 56.25 MHz   (time base for the DSI video regs)
//   DSI pixel throughput = lane_rate*lanes/bpp = 450*4/24 = 75 MHz (max)
//   LTDC pixel clock = ck_ker_ltdc = PLL4/N ~= 66 MHz (SYSCFG mux=0b10; display.cc
//     picks N at runtime). PLL4 is HSE-sourced, so the pixel clock stays phase-
//     locked to the (HSE-derived) DSI byte clock -- an unrelated source (HSI)
//     makes the video FIFO slip and cycles the colors. 66 < 75 = the DSI headroom
//     that lets burst mode work.  ~66 MHz / (HTotal * VTotal) ~= 53 Hz.

namespace Panel
{

// ---- display mode ----------------------------------------------------------
constexpr uint32_t HActive = 720;
constexpr uint32_t HFront = 190; // HFP: was 120, =>180-190 with HBack=20-40 makes it stable.
constexpr uint32_t HSync = 20;	 // HSA

constexpr uint32_t HBack = 40;

constexpr uint32_t VActive = 1280;
constexpr uint32_t VFront = 20; // VFP
constexpr uint32_t VSync = 4;	// VSA
constexpr uint32_t VBack = 12;	// VBP

// The DSI regenerates the vertical frame from its own VBP/VFP and can land one
// line out of phase with the LTDC's DE (symptom: image shifted up by one line,
// top row wraps to the bottom). This offset nudges ONLY the DSI's VBP/VFP (the
// LTDC and the frame total are untouched, so refresh rate is unchanged): +1
// shifts the image DOWN one line. Flip to -1 if it moves the wrong way; 0 =
// matched to the LTDC.
constexpr int DsiVBpOffset = 0;
constexpr uint32_t DsiVBack = VBack + DsiVBpOffset;	  // VVBPCR
constexpr uint32_t DsiVFront = VFront - DsiVBpOffset; // VVFPCR

// (the LTDC pixel clock is chosen at runtime -- see display_clocks_setup())
constexpr uint32_t LaneRateMbps = 450; // per-lane HS data rate
constexpr uint32_t InitWrites = 193;   // DCS writes in the ILI9881C init table

// DRM-style mode points (as ltdc.c consumes them)
constexpr uint32_t HSyncStart = HActive + HFront; // 840
constexpr uint32_t HSyncEnd = HSyncStart + HSync; // 860
constexpr uint32_t HTotal = HSyncEnd + HBack;	  // 950
constexpr uint32_t VSyncStart = VActive + VFront; // 1300
constexpr uint32_t VSyncEnd = VSyncStart + VSync; // 1304
constexpr uint32_t VTotal = VSyncEnd + VBack;	  // 1316

// ---- LTDC accumulated-timing register fields (formulas ltdc.c:1183-1209) ----
constexpr uint32_t SyncW = HSyncEnd - HSyncStart - 1;  // 19
constexpr uint32_t SyncH = VSyncEnd - VSyncStart - 1;  // 3
constexpr uint32_t AccumHbp = HTotal - HSyncStart - 1; // 109
constexpr uint32_t AccumVbp = VTotal - VSyncStart - 1; // 15
constexpr uint32_t AccumActW = AccumHbp + HActive;	   // 829
constexpr uint32_t AccumActH = AccumVbp + VActive;	   // 1295
constexpr uint32_t TotalW = HTotal - 1;				   // 949
constexpr uint32_t TotalH = VTotal - 1;				   // 1315

// ---- DSI D-PHY PLL solution -------------------------------------------------
// Reference = ck_ker_dsiphy = HSE/2 = 20 MHz (set in display.cc). Target the
// 450 Mbps row of ST's PLL regulation table (dsi.c:340, HAL_DSI_DT_450). Formula
// confirmed from ST's downstream dw_mipi_dsi-stm.c (HWVER_141 = MP25):
//   f_vco     = f_ref * NDIV / IDF          = 20 * 90 / 4     = 450 MHz  (band 320..1250)
//   data_rate = 2 * f_ref * NDIV / (IDF*ODF) = 2*20*90/(4*2)  = 450 Mbps/lane  (x2 = DDR)
//   lane byte clk = data_rate / 8 = 56.25 MHz
// ODF must equal the table's odf column so f_vco lands where the `vco` code is
// tuned: DT_450 -> odf /2, vco 0x09 (~450 MHz). Keeping f_vco/vco consistent is
// what makes the PLL lock.
constexpr uint32_t DsiRefClkinKHz = 20'000; // 20 MHz, must be 17-27 MHz
constexpr uint32_t PllNdiv = 90;			// WRPCR0.NDIV (64..625)
constexpr uint32_t PllIdf = 4;				// WRPCR0.IDF  (1..16)
constexpr uint32_t PllOdf = 1;				// WRPCR1.ODF: 0/1/2/3 = /1,/2,/4,/8

// Per-rate PHY regulation values, HAL_DSI_DT_450 row {hs_freq, odf, vco, prop}
// (dsi.c:340). The table's odf is mirrored in PllOdf above; vco/prop/hs_freq
// are written verbatim.
constexpr uint32_t PhyHsFreq = 0x16; // WPCR1.HSFR
constexpr uint32_t PhyVco = 0x09;	 // WRPCR1.VCO
constexpr uint32_t PhyProp = 0x0B;	 // WRPCR1.PROP

// ---- DSI host / video config ------------------------------------------------
// Burst mode: each active line is buffered and sent as one HS burst, which
// decouples the DSI byte clock from the (now independent) LTDC pixel clock --
// required once they are not frequency-locked. Non-burst (0) would drift the
// pixels line-to-line into hash. Burst needs the lane headroom we set up so the
// burst fits inside the active period.
constexpr uint32_t VideoModeType = 2; // DSI_VID_MODE_BURST

// TX escape clock = lane_byte_clock / div <= 20 MHz. 56.25/4 = 14.06 MHz.
constexpr uint32_t TxEscapeClkDiv = 4;

// Horizontal DSI video-timing regs are in lane-byte-clock cycles: dsi.cc
// converts from pixels by ceil(px * LaneByteClkHz / pixel_clk) at runtime, since
// the LTDC pixel clock is chosen at runtime (PLL4-derived). Vertical regs
// (VVSACR/VVBPCR/VVFPCR/VVACR) are in lines -> VSync/VBack/VFront/VActive.
constexpr uint32_t LaneByteClkHz = LaneRateMbps * 125'000; // rate*1e6/8 = 56.25 MHz

// Vertical-blanking LP is REQUIRED (the ILI9881C uses the vblank LP as its frame
// delimiter -- holding vblank in HS blanks the screen), so dsi.cc always keeps
// LPVSAE/LPVBPE/LPVFPE on.
//
// Active-line horizontal blanking is the knob headroom buys us: at zero headroom
// it HAD to go LP (no spare HS bandwidth), and that per-line LP->HS is what
// wrapped each line by ~136 px. Now that the pixel clock is below the lane
// throughput there's room to keep the H-porches in HS -- false does that (should
// remove the wrap). Set true to restore the old all-LP behaviour.
constexpr bool LpDuringActiveHBlank = true;

// LP packet-size limits during blanking (HAL_DSI_ConfigVideoMode LPMCR).
constexpr uint32_t LpLargestPacket = 4;
constexpr uint32_t LpVactLargestPacket = 0;

// PHY LP<->HS transition timers, lane-byte-clock cycles (HAL_DSI_ConfigPhyTimer).
// From ST's per-rate table hstt_phy_141_table in dw_mipi_dsi-stm.c (HWVER_141 =
// MP25): the 450 Mbps row is HSTT(450, clk_lp2hs=65, clk_hs2lp=40, data_lp2hs=49,
// data_hs2lp=23). Getting these right (esp. data_lp2hs) is what makes the LP->HS
// transition into each active line land cleanly instead of disturbing it.
constexpr uint32_t ClockLaneLP2HS = 65;
constexpr uint32_t ClockLaneHS2LP = 40;
constexpr uint32_t DataLaneLP2HS = 49;
constexpr uint32_t DataLaneHS2LP = 23;
constexpr uint32_t StopWaitTime = 10;

} // namespace Panel
