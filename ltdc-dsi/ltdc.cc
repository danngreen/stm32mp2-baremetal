#include "ltdc.hh"
#include "aarch64/system_reg.hh"  // read_cntpct/read_cntfreq (vblank timeout)
#include "interrupt/interrupt.hh" // InterruptManager (LTDC LINE IRQ)
#include "panel_ili9881c.hh"
#include "stm32mp2xx_hal.h"

// =============================================================================
//  ltdc.cc -- LTDC timings + one ARGB8888 layer, via ST's HAL driver
// =============================================================================
// Configuration (timings, GCR, layer window/format/blending) goes through ST's
// stm32mp2xx_hal_ltdc.c. Two things are done by hand, deliberately:
//
//  1. CFBLR pitch. This vendored HAL's LTDC_SetConfig() (used by ConfigLayer and
//     SetAddress) writes CFBP = 0x10000 - ImageWidth*stride -- a *negative* pitch
//     copied from the rotation/AFBLR path. The register is a plain byte pitch
//     (HAL_LTDC_SetPitch and the mirror helpers write the raw ImageWidth*stride
//     to the same field), and the raw pitch is what actually scans out on this
//     silicon. So after ConfigLayer we rewrite CFBLR with the raw pitch and
//     re-latch. The vendored HAL is left untouched.
//  2. The vblank flip is a 2-register hot path (CFBAR + per-layer VBR reload) --
//     using HAL_LTDC_SetAddress would re-run the buggy LTDC_SetConfig every frame.
//
// MP25 note: layer shadow registers latch via the *per-layer* reload register
// (LxRCR: IMR bit0 immediate, VBR bit1 at next vblank); the reload-done IRQ
// (IT_RR) never fires for per-layer reloads, so the flip is paced off the LINE
// interrupt instead.

namespace
{
using namespace Panel;

LTDC_HandleTypeDef g_ltdc; // zero-init => State = HAL_LTDC_STATE_RESET

volatile uint32_t g_vblank = 0; // bumped by the LINE IRQ, drives ltdc_wait_vblank()
Callback vblank_cb = [] {};

// Hardware rotation (see ltdc.hh). Zero base = off.
uint32_t g_rot_base = 0, g_rot_size = 0;
constexpr uint32_t kRotPitch = ((VActive + 9u) / 10u) * 64u;
uint32_t g_layer_w = 0, g_layer_h = 0; // the surface the layer scans
unsigned g_scan_mode = 0;			   // see ltdc_set_scan_mode
uint32_t g_fb_addr = 0;

// CFBLR = pitch<<16 | (line_bytes + bus_width_bytes - 1); 64-bit bus -> +7.
void apply_scan_mode()
{
	if (g_layer_w == 0)
		return;
	const uint32_t raw = g_layer_w * 4;
	const uint32_t pitch = (g_scan_mode & 1u) ? (0x10000u - raw) : raw;
	LTDC_Layer1->CFBLR = (pitch << 16) | (raw + 8 - 1);
}

// With a backwards (negative-pitch) scan the first line fetched is the last
// one in memory, so the base address has to move there.
uint32_t scan_base(uint32_t fb_addr)
{
	if ((g_scan_mode & 2u) && g_layer_h > 1)
		return fb_addr + (g_layer_h - 1) * g_layer_w * 4;
	return fb_addr;
}

void ltdc_on_irq()
{
	uint32_t isr = LTDC->ISR;
	LTDC->ICR = isr; // write-1-to-clear everything we took
	if (isr & LTDC_ISR_LIF) {
		g_vblank = g_vblank + 1;
		vblank_cb();
	}
}
} // namespace

// uint32_t ltdc_rotation_pitch()
// { return kRotPitch; }

uint32_t ltdc_rotation_mem_size()
{
	// Two intermediate frames at 3 bytes/pixel -- the size ST's Linux driver
	// checks for (hdisplay * vdisplay * 2 * 3). That is the size for the
	// NATURAL pitch (HActive * 3); a larger pitch needs proportionally more,
	// see ltdc_rotation_bytes_for_pitch.
	return HActive * VActive * 2u * 4u;
}

uint32_t ltdc_rotation_bytes()
{
	// RM Example 4:
	// display is landscape h=1080 w=1920 => want to use it portrait w=1080 h=1920:
	// PITCH = (1080 + 9)/10 * 64 = 6912
	// SIZE = ((1920+1) / 2) * ((1080+9)/10) * 64 = 6480 Kbytes per rotation buffer
	//
	// PITCH = ((hardware-height + 9) / 10) * 64
	// SIZE = (w+1 / 2) * (h+9 / 10) * 64 bytes per rotation buffer
	//
	//  For us: hardware-height = 1280(VActive), hardware-w = 720(HActive)
	return ((HActive + 1) / 2) * ((VActive + 9) / 10) * 64;
}

// void ltdc_set_rotation_pitch(uint32_t pitch)
// {
// 	if (!g_rot_base || pitch == 0)
// 		return;
// 	LTDC->RBPR = pitch;
// 	// The second buffer sits exactly one frame after the first -- which means
// 	// its address depends on the pitch.
// 	LTDC->RB0AR = g_rot_base;
// 	LTDC->RB1AR = g_rot_base + pitch * VActive;
// 	LTDC->SRCR = LTDC_SRCR_IMR; // latch immediately
// }

void ltdc_enable_rotation(uint32_t rot_base, uint32_t rot_size)
{
	g_rot_base = rot_base;
	g_rot_size = rot_size;
}

void ltdc_init(uint32_t fb_addr, uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t bg_argb)
{
// layer and framebuffer are VActive x HActive.
#ifdef LANDSCAPE
	const bool rotate = true;
#else
	const bool rotate = false;
#endif
	if (w == 0)
		w = rotate ? VActive : HActive;
	if (h == 0)
		h = rotate ? HActive : VActive; // 0 -> full screen

	// --- controller: timings + polarities (values from panel_ili9881c.hh) ------
	// No active-edge overrides (the "AL"/"IPC" constants are all 0, i.e. a zeroed
	// GCR polarity field) -- the syncs are embedded in the DSI stream anyway.
	g_ltdc.Instance = LTDC;
	g_ltdc.Init.HSPolarity = LTDC_HSPOLARITY_AL;
	g_ltdc.Init.VSPolarity = LTDC_VSPOLARITY_AL;
	g_ltdc.Init.DEPolarity = LTDC_DEPOLARITY_AL;
	g_ltdc.Init.PCPolarity = LTDC_PCPOLARITY_IPC;
	g_ltdc.Init.HorizontalSync = SyncW;
	g_ltdc.Init.VerticalSync = SyncH;
	g_ltdc.Init.AccumulatedHBP = AccumHbp;
	g_ltdc.Init.AccumulatedVBP = AccumVbp;
	g_ltdc.Init.AccumulatedActiveW = AccumActW;
	g_ltdc.Init.AccumulatedActiveH = AccumActH;
	g_ltdc.Init.TotalWidth = TotalW;
	g_ltdc.Init.TotalHeigh = TotalH;
	g_ltdc.Init.Backcolor.Red = (bg_argb >> 16) & 0xFF;
	g_ltdc.Init.Backcolor.Green = (bg_argb >> 8) & 0xFF;
	g_ltdc.Init.Backcolor.Blue = bg_argb & 0xFF;
	g_ltdc.Init.FifoUnderThresh = 0x80; // driver-default underrun warning threshold
#ifdef LANDSCAPE
	g_ltdc.Init.RotEn = LTDC_GCR_ROTEN;
	g_ltdc.Init.Rotation.BufferPitch = kRotPitch;
	g_ltdc.Init.Rotation.Buffer0Addr = g_rot_base;
	g_ltdc.Init.Rotation.Buffer1Addr = g_rot_base + g_rot_size;
	g_ltdc.Init.Rotation.InterFrameRed = 0;
	g_ltdc.Init.Rotation.InterFrameGreen = 0;
	g_ltdc.Init.Rotation.InterFrameBlue = 0;
#endif
	HAL_LTDC_Init(&g_ltdc); // programs GCR/timings + enables LTDCEN

	// Quiet interrupts: HAL_LTDC_Init turns on TE/FU (and the secure copies); we
	// only want the LINE interrupt (fires once per frame at start of vblank).
	LTDC->IER = LTDC_IER_LIE;
	LTDC->IER2 = 0; // secure IER off
	LTDC->ICR = 0x3F;
	LTDC->LIPCR = AccumActH + 1; // line IRQ at start of vblank

	// --- layer 1: a w x h ARGB8888 window at (x, y); HAL adds the accumulated
	// back-porch offsets internally, so pass raw positions. --
	LTDC_LayerCfgTypeDef lc{};
	lc.WindowX0 = x;
	lc.WindowX1 = x + w;
	lc.WindowY0 = y;
	lc.WindowY1 = y + h;
	lc.PixelFormat = LTDC_PIXEL_FORMAT_ARGB8888;
	lc.Alpha = 0xFF; // constant alpha 1.0
	lc.Alpha0 = 0;
	lc.BlendingFactor1 = LTDC_BLENDING_FACTOR1_PAxCA;
	lc.BlendingFactor2 = LTDC_BLENDING_FACTOR2_PAxCA;
	lc.FBStartAdress = fb_addr;
	lc.ImageWidth = w;
	lc.ImageHeight = h;
	lc.BurstLength = 0; // max burst
	HAL_LTDC_ConfigLayer(&g_ltdc, &lc, LTDC_LAYER_1);

#ifdef LANDSCAPE
	// HAL_LTDC_EnableHMirror(&g_ltdc, &lc, LTDC_LAYER_1);
#endif

	// Fix the CFBLR pitch the HAL got wrong (see file header), then re-latch it.
	// CFBLR = pitch<<16 | (line_bytes + bus_width_bytes - 1); 64-bit bus -> +7.
	//
	// ROTATED: the NEGATIVE pitch the HAL writes is not a bug here -- 90 degrees
	// on this controller is rotate + vertical mirror, and the mirror is exactly
	// this 0x10000 - pitch form (ST's Linux driver does the same for
	// REFLECT_Y). So in rotated mode we keep it.
	g_layer_w = w;
	g_layer_h = h;
	// Non-rotated scans forward with the raw pitch -- proven by every sketch so
	// far. Rotated starts there too and is switchable at runtime while we work
	// out what the rotation datapath wants (ltdc_set_scan_mode).
	g_scan_mode = 0;
	// apply_scan_mode();
	LTDC_Layer1->RCR = LTDC_LxRCR_IMR; // per-layer immediate reload

	InterruptManager::register_and_start_isr(LTDC_IRQn, 0, 0, [] { ltdc_on_irq(); });
}

void ltdc_set_framebuffer(uint32_t fb_addr)
{
	// Tear-free flip: new address + per-layer reload at the next vblank. Done by
	// hand (not HAL_LTDC_SetAddress) to avoid re-running the HAL's buggy pitch.
	g_fb_addr = fb_addr;
	LTDC_Layer1->CFBAR = fb_addr; // scan_base(fb_addr);
	LTDC_Layer1->RCR = LTDC_LxRCR_VBR;
}

void ltdc_set_scan_mode(unsigned mode)
{
	g_scan_mode = mode & 3u;
	apply_scan_mode();
	if (g_fb_addr)
		LTDC_Layer1->CFBAR = scan_base(g_fb_addr);
	LTDC_Layer1->RCR = LTDC_LxRCR_VBR;
}

unsigned ltdc_get_scan_mode()
{ return g_scan_mode; }

void ltdc_set_callback(Callback &&cb)
{ vblank_cb = std::move(cb); }

bool ltdc_wait_vblank()
{
	// Block until the next vblank (the LINE IRQ bumps g_vblank at the start of
	// every frame). The VBR flip we armed latches at that same vblank, so this
	// paces exactly one rendered+displayed frame per refresh, tear-free -- the CPU
	// sits in WFE meanwhile. ~25 ms deadline in case the IRQ never arrives.
	uint32_t start = g_vblank;
	uint64_t deadline = read_cntpct() + read_cntfreq() / 40; // ~25 ms
	while (g_vblank == start) {
		asm volatile("wfe");
		if (read_cntpct() > deadline)
			return false;
	}
	return true;
}

uint32_t ltdc_current_line()
{
	return LTDC->CPSR & 0xFFFF; // CYPOS
}

uint32_t ltdc_isr()
{ return LTDC->ISR; }

uint32_t ltdc_hw_version()
{ return LTDC->IDR; }
