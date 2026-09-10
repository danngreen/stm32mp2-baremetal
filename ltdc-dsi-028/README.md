# LTDC + MIPI-DSI example: ER-TFT028-2 (ST7701S)

Same demo as `ltdc-dsi/`, built for a different panel: the BuyDisplay
ER-TFT028-2, a 2.8" 480x480 IPS panel with a Sitronix ST7701S controller,
driven over 2 MIPI-DSI data lanes + clock, RGB888, burst video mode. The test
pattern is drawn by the CPU (not the GPU): RGB gradient, 1px white border, x^y
hash on green, so offset/wrap/stride bugs are visible at a glance.

What differs from `ltdc-dsi/` (ILI9881C, 720x1280, 4 lanes):

- `panel_st7701s.hh`: 480x480 mode. The vendor init sequence programs the
  panel's own timing generator (BK0 `C0`/`C1`/`C2`), so the vertical porches
  (VBP 16 / VFP 12) are taken from it verbatim and HTotal (680) is kept just
  above the `RTNI` minimum of 672 clocks per line. Pixel clock target is ~21 MHz
  (680 x 512 x 60 Hz); `display.cc` picks the largest PLL4/N under
  `Panel::PixelClkMaxHz`.
- DSI runs 2 lanes at 400 Mbps/lane (ST7701S max on 2 lanes is 550), 33 MHz
  of pixel throughput against a ~21 MHz pixel clock. Burst mode is the only
  mode that gives a correct picture on this panel (both non-burst modes show a
  bad picture), and LP in the horizontal porches must stay on or the host's
  pixel FIFO overflows every line.
- The DSI stream is only ever started with the LTDC gated off
  (`display_video_restart`): starting the host while the LTDC is already
  scanning leaves a partial pixel in the host's DPI FIFO and the whole stream
  comes out offset by 1-2 bytes (red shows as green, image wrapped), differently
  on every restart. The host is also fully stopped before its video config is
  written. PLL: 20 MHz ref x 80 / 4 = 400 MHz VCO, ODF /2 (HAL row
  `HAL_DSI_DT_400`). PHY LP/HS timers are the 400 Mbps row of ST's
  `hstt_phy_141_table`.
- `st7701s_init.hh`: converted from
  `reference/ER-TFT028-2_Initialization Program.txt` (41 DCS writes, one
  120 ms delay after sleep-out). `st7701s.cc` walks it.
- Panel reset follows the ST7701S datasheet (pulse >= 10 us, then wait before
  the first command).

Everything else (`ltdc.cc`, `dsi.cc`, `display.cc` clock/RIF setup, `main.cc`)
is the `ltdc-dsi/` code with the panel header swapped. See that README for the
notes on the HAL `CFBLR` pitch workaround and the per-layer reload.

Build (custom devboard only; the EV1 has no DSI connector):

```
make            # BOARD=devboard, USART1 console
```

After bring-up a key console runs on the UART (`?` lists the keys): test
patterns, DSI video-mode / LP-porch switching, a panel read-back (`p`), an HS
probe that writes MADCTL in HS and reads it back in LP (`h`), and a status dump
of the DSI error registers sampled twice (`s`). ISR1 bit 7 recurring = DPI FIFO
overflow.

Expected console output:

```
LTDC + DSI Example (ST7701S / ER-TFT028-2)
==========================================

1. Test pattern at 0x90000000 (480x480)
2. RIF: LTDC + DSI secure, LTDC DMA master (RIMU 11) MSEC
3. Clocks on, resets pulsed, ck_ker_dsiphy = HSE/2 = 20 MHz
   LTDC pixel clock = 20 MHz (59 Hz refresh)
   DSI version 0x20, LTDC version 0x40101
4. DSI PLL locked (400 Mbps/lane, 2 lanes), PHY up
5. DSI host on, low-power command mode
6. ST7701S reset + init sequence sent (41 DCS writes)
7. LTDC configured + enabled
8. DSI video mode on (RGB888, burst) -- pixels streaming
9. Backlight on

   Panel read-back over D0 (LP): ... RDDPM 9C sleep-out, display on ...
DSI ISR0=0x0 ISR1=0x0 PSR=0x15B9 ... LTDC line ... (advancing)
```

## Board wiring (devboard)

Set in `display.cc`, same pins as `ltdc-dsi/`:

- **Panel RESET** (ER-TFT028-2 pin 6 `RST`, active-low): PH7
- **Backlight enable**: PB0. The ER-TFT028-2 backlight is a bare 4-chip LED
  string (LEDA/LEDK, 60 mA typ), not the ~18 V string the 5" panel had, so check
  the boost driver's output voltage/current limit suits it before enabling.

Unused panel pins: TE (pin 7) and PWM (pin 8) are left unconnected; the touch
panel pins (25-30) are not used here.

## Tuning knobs if the picture is wrong

All in `panel_st7701s.hh`:

- `DsiVBpOffset`: image shifted one line vertically (top row wraps to bottom).
- `VideoModeType` / `LpDuringActiveHBlank`: keys `b`/`l` try the alternatives
  live; on this panel only burst + LP porches works (verified). Rotated colors
  or a wrapped image after a restart means the DSI was started while the LTDC
  was scanning.
- `HFront`/`HBack`: line period. Keep HTotal >= 672 (the init sequence's
  `INVSET` RTNI); if you change it, or the vertical porches, the BK0 `C1`/`C2`
  values in `st7701s_init.hh` should be changed to match.
- `PixelClkMaxHz` / `LaneRateMbps`: refresh rate and link headroom. Changing
  the lane rate means changing `PllNdiv`/`PllIdf`/`PllOdf`, the
  `HAL_DSI_DT_*` row in `dsi.cc`, and the four PHY timer values together.
