#pragma once
#include "interrupt/callable.hh"
#include <cstdint>

// LTDC (0x48010000) config for the MP25 (HW version 4.x: per-layer shadow
// reload, 64-bit bus). Ported from Linux drivers/gpu/drm/stm/ltdc.c.

// Program timings + one ARGB8888 layer scanning out `fb_addr`, latch it, enable
// the controller, and hook up the vblank (LINE) interrupt. The layer is a `w` x
// `h` window at (x, y) in the active area; w/h = 0 mean full-screen. `bg_argb`
// (RGB) fills the area outside the window. The pixel clock (ck_ker_ltdc via the
// SYSCFG mux) must be ticking first.
void ltdc_init(uint32_t fb_addr, uint32_t x = 0, uint32_t y = 0, uint32_t w = 0, uint32_t h = 0, uint32_t bg_argb = 0);

// Turn on the LTDC's hardware 90-degree rotation, so a PORTRAIT panel can be
// driven from a LANDSCAPE framebuffer with no software rotate anywhere. Call
// before ltdc_init()/display_init(); after this the layer geometry and the
// framebuffer are landscape (VActive x HActive).
//
// `rot_base` needs >= 2 * HActive * VActive * 3 bytes (the controller rotates
// through two intermediate RGB24 frames of its own), and must stay reserved
// for as long as the display is running.
void ltdc_enable_rotation(uint32_t rot_base, uint32_t rot_size);
// Bytes of scratch the above needs for this panel.
uint32_t ltdc_rotation_mem_size();

// How the layer walks the framebuffer, for bringing up rotation. 90 degrees on
// this controller is rotate + vertical mirror, and the mirror is expressed as a
// NEGATIVE pitch (0x10000 - pitch) -- which also means the scan starts at the
// LAST line. Which combination this silicon actually wants is what these select:
//   bit 0: negative pitch (mirror)   bit 1: base address = last line
void ltdc_set_scan_mode(unsigned mode);
unsigned ltdc_get_scan_mode();

// The rotation buffer pitch (RBPR), and with it the split between the two
// intermediate buffers -- each is pitch * panel_height bytes, so the second
// buffer's address MOVES when the pitch changes. Sizing them independently of
// the pitch is a bug: the buffers overlap and every other frame is corrupt.
uint32_t ltdc_rotation_bytes();

// Point the layer at a new framebuffer and latch on the next vblank (tear-free).
void ltdc_set_framebuffer(uint32_t fb_addr);

uint32_t ltdc_current_line(); // CPSR CYPOS -- advancing == pixel clock alive
uint32_t ltdc_isr();		  // ISR flags (bit1 FIFO warn, 2 transfer err, 6 FIFO err)
uint32_t ltdc_hw_version();	  // IDR, expect 0x0401xx on MP25

bool ltdc_wait_vblank();
void ltdc_set_callback(Callback &&cb);
