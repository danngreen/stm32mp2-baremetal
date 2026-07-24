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

// Program video-mode timings (from panel_ili9881c.hh), DPI color coding
// (RGB888) and the wrapper, then enable the host + wrapper so DPI pixels from
// the LTDC are packetised onto the link. Call after the LTDC is configured.
// The horizontal timing registers are in lane-byte-clock cycles, so they depend
// on the actual LTDC pixel clock (pass what display_clocks_setup() returned)
void dsi_video_start(uint32_t pixel_clk_hz);

// Send one DCS write over the link (LP): len==1 -> short write no-param (0x05),
// len==2 -> short write 1-param (0x15), len>=3 -> long write (0x39). Blocks
// until the command FIFO has accepted it.
void dsi_dcs_write(const uint8_t *data, uint32_t len);

uint32_t dsi_version();
