#pragma once
#include <cstdint>

// Board-level bring-up for the custom devboard + ER-TFT028-2 (ST7701S)
// MIPI-DSI panel. Pieces are individually callable so main.cc can print between
// stages, mirroring the ltdc/ example.
// Or call display_init() to do everything at once.

void display_rif_setup(); // LTDC + DSI register ports + LTDC DMA master secure

// LTDC/DSI gates + resets, ck_ker_dsiphy (HSE/2), and the LTDC pixel clock
// (ck_ker_ltdc). Returns the actual LTDC pixel-clock frequency in Hz -- it comes
// from an HSE-locked PLL when available, else HSE direct, so pass it to
// dsi_video_start() which sizes the DSI horizontal timings against it.
uint32_t display_clocks_setup();
void display_panel_reset();	 // pulse the ST7701S RST pin (active-low)
void display_backlight_on(); // enable the boost LED-driver backlight

// (Re)start the DSI video stream with the LTDC gated off around it, so the
// stream always begins on a clean frame with an empty DPI FIFO. Use this for
// every start/restart; starting the DSI while the LTDC is already scanning
// leaves a random 0-2 byte offset in the pixel stream (observed 2026-09-10 as
// red-shows-green / wrapped image that changed with every restart).
struct DsiVideoOpts;
void display_video_restart(uint32_t pixel_clk, const DsiVideoOpts &opts);

bool display_init(
	uint32_t first_fb, uint32_t x = 0, uint32_t y = 0, uint32_t w = 0, uint32_t h = 0, uint32_t bg_argb = 0);
