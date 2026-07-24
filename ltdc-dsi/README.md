# LTDC + MIPI-DSI example

This project demos using the MIPI DSI peripheral. I made a custom PCB with a
header to connect to a 720x1280 5" display from BuyDisplay (ER-TFT050-10, which
has a ILI9881C controller, 4-lane MIPI-DSI, RGB888) The test pattern is drawn
by the CPU (not the GPU): RGB gradient, 1px white border, x^y hash on green. 
Any issues with offset, wrap and stride bugs are visible at a glance.

Unlike LVDS, the DSI panel needs a controller init sequence of about 190
register writes pushed over the link as DCS commands before video starts. The
vendor's `reference/ILI9881C_Initial.h` was converted into `ili9881c_init.hh`.

The DSI and LTDC are driven through ST's HAL drivers, so the bring-up is essentially
ST's own examples. `dsi.cc` and `ltdc.cc` are thin wrappers that fill the HAL
config structs from the panel parameters in `panel_ili9881c.hh` and call
`HAL_DSI_Init` / `HAL_DSI_ConfigVideoMode` / `HAL_LTDC_Init` / etc. 

Two things in `ltdc.cc` are done by hand and commented as such: the vblank flip
is a 2-register hot path (address + per-layer VBR reload), and the layer pitch
(`CFBLR`) is rewritten after `HAL_LTDC_ConfigLayer` because this vendored HAL's
`LTDC_SetConfig` writes a *negative* pitch (`0x10000 - width*stride`, copied from
its rotation path) instead of the raw byte pitch this silicon actually scans out.

When running, you should see a test pattern on the panel, and this:

```
LTDC + DSI Example (ILI9881C / ER-TFT050-10)
============================================

1. Test pattern at 0x90000000 (720x1280)
2. RIF: LTDC + DSI secure, LTDC DMA master (RIMU 11) MSEC
3. Clocks on, resets pulsed, ck_ker_dsiphy = HSE/2 = 20 MHz
   LTDC pixel clock = 66 MHz (57 Hz refresh)
   DSI version 0x20, LTDC version 0x40101
4. DSI PLL locked (450 Mbps/lane, 4 lanes), PHY up
5. DSI host on, low-power command mode
6. ILI9881C reset + init sequence sent (193 DCS writes)
7. LTDC configured + enabled
8. DSI video mode on (RGB888, burst) -- pixels streaming
9. Backlight on

LTDC line counter: 677 -> 1059  (advancing \o/)
LTDC ISR flags: 0x0  (bit1 FIFO-warn, bit2 xfer-err, bit6 FIFO-err)

Scanout running -- look at the panel!
```

## Board wiring (devboard)

Set in `display.cc`:

- **Panel RESET** (ILI9881C pin 29, active-low): `PanelResetPort`/`PanelResetPin`
- **Backlight enable** (on-board ~18 V boost LED driver, active-high):
  `BacklightPort`/`BacklightPin`
