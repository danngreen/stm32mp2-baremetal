#pragma once
#include <cstdint>

// Configure + lock the D-PHY PLL, power up the PHY, and wait for all lanes to
// reach stop-state. Call after the DSI bus clock is on, reset released, and the
// PHY reference clock (ck_ker_dsiphy) is running. Returns false on PLL-lock or
// lane-stop timeout.
bool dsi_pll_init();

// Program the host for LP command transmission (used to push the panel init
// sequence before the video stream starts). Safe to call once the PLL is up.
void dsi_command_mode();

// Program video-mode timings (from panel_st7701s.hh), DPI color coding
// (RGB888) and the wrapper, then enable the host + wrapper so DPI pixels from
// the LTDC are packetised onto the link. Call after the LTDC is configured.
// The horizontal timing registers are in lane-byte-clock cycles, so they depend
// on the actual LTDC pixel clock (pass what display_clocks_setup() returned)
struct DsiVideoOpts {
	uint32_t mode = 2;		// DSI_VID_MODE_NB_PULSES=0, NB_EVENTS=1, BURST=2
	bool lp_hblank = true;	// drop to LP in HFP/HBP of active lines
};
void dsi_video_start(uint32_t pixel_clk_hz, DsiVideoOpts opts = {});

// Disable host + wrapper (HAL_DSI_Stop). Call dsi_command_mode() afterwards
// to send LP commands again, or dsi_video_start() to restart the stream.
void dsi_video_stop();

// Route DCS/generic *writes* through HS (true) or LP (false). Reads always
// stay LP. HS writes need the clock lane + D0 HS path working, so a write that
// reads back correctly in LP afterwards proves CLK/D0 HS on the panel.
void dsi_set_write_hs(bool hs);

// Send one DCS write over the link (LP): len==1 -> short write no-param (0x05),
// len==2 -> short write 1-param (0x15), len>=3 -> long write (0x39). Blocks
// until the command FIFO has accepted it.
void dsi_dcs_write(const uint8_t *data, uint32_t len);

// DCS read in LP over D0 (bus turnaround): send `cmd` as a DCS short read and
// collect `len` returned bytes into buf. Sets the max-return-packet-size first.
// Returns false on timeout / packet-size error (panel did not answer). Use to
// prove the panel is alive on the D0 pair before chasing the HS side.
bool dsi_dcs_read(uint8_t cmd, uint8_t *buf, uint32_t len);

uint32_t dsi_version();
uint32_t dsi_isr0(); // ISR0: bits0-15 ACK errors reported by the panel, 16-20 PHY errors
uint32_t dsi_isr1(); // ISR1: bit0 HS-TX timeout, 1 LP-RX timeout, 2/3 ECC, 4 CRC, 5 packet size, 6 EOTP, 7+ FIFO write errs
uint32_t dsi_psr();	 // PSR: PHY status (lane stop-states, ULPS, RX-active)
