#include "dma2d.hh"
#include "aarch64/system_reg.hh" // clean_dcache_range
#include "drivers/rcc.hh"		 // RCC_Enable::HPDMA1_
#include "stm32mp2xx.h"

namespace dma2d
{
namespace
{
// Only HPDMA channels 12..15 support 2D (strided) addressing; ch0..11 are
// linear-only and silently ignore the 2D offset. Use channel 12.
constexpr uint32_t CHNUM = 12;
auto *const CH = HPDMA1_Channel12;
constexpr uint32_t DW_WORD = 2; // data-width log2: 2 -> 4-byte word (one ARGB8888 pixel)

uint32_t g_color_addr = 0; // DDR word: fill color source for the blocking path

// src_inc selects whether the source buffer is a single word (false) or a buffer (true)
uint32_t ctr1_for(bool src_inc)
{
	constexpr uint32_t BURST = 15; // 16 beats (faster writes)
	uint32_t sbl = src_inc ? BURST : 0u;
	return (DW_WORD << DMA_CTR1_SDW_LOG2_Pos) | (DW_WORD << DMA_CTR1_DDW_LOG2_Pos) |
		   (src_inc ? (1u << DMA_CTR1_SINC_Pos) : 0u) | (1u << DMA_CTR1_DINC_Pos) | (sbl << DMA_CTR1_SBL_1_Pos) |
		   (BURST << DMA_CTR1_DBL_1_Pos) | (1u << DMA_CTR1_SSEC_Pos) | (1u << DMA_CTR1_DSEC_Pos) |
		   (0u << DMA_CTR1_SAP_Pos) | (1u << DMA_CTR1_DAP_Pos); // src port0, dst port1
}

// One block = one row (w px, BNDT bytes)
// BRC = h-1 rows.
// Stride: CBR2.BRDAO (block-repeat dest offset)
uint32_t cbr1_for(int w, int h)
{
	return (uint32_t(w) * 4 & DMA_CBR1_BNDT_Msk) | ((uint32_t(h - 1) << DMA_CBR1_BRC_Pos) & DMA_CBR1_BRC_Msk);
}
uint32_t cbr2_for(int w, uint32_t stride, bool src_inc)
{
	uint32_t row_gap = stride - uint32_t(w) * 4;
	return ((row_gap << DMA_CBR2_BRDAO_Pos) & DMA_CBR2_BRDAO_Msk) | ((src_inc ? row_gap : 0u) & DMA_CBR2_BRSAO_Msk);
}
} // namespace

void init(uint32_t color_scratch_ddr, uint32_t node_region_ddr)
{
	g_color_addr = color_scratch_ddr;
	RCC_Enable::HPDMA1_::set();

	HPDMA1->SECCFGR |= (1u << CHNUM);  // channel secure (reach secure DDR from EL3)
	HPDMA1->PRIVCFGR |= (1u << CHNUM); // channel privileged
	CH->CCIDCFGR = 0;				   // no CID filtering
	CH->CCR = 0;					   // disabled

	batch_init(node_region_ddr);
}

// ---------------------------------------------------------------------------
//  Blocking path (one transfer at a time, CPU spins on completion)
// ---------------------------------------------------------------------------
namespace
{
void run_2d(uint32_t src, uint32_t dst, int w, int h, bool src_inc, uint32_t stride)
{
	CH->CCR = 0;
	CH->CFCR = 0x7F00u;
	CH->CTR1 = ctr1_for(src_inc);
	CH->CTR2 = (1u << DMA_CTR2_SWREQ_Pos) | (1u << DMA_CTR2_TCEM_Pos); // TC at repeated-block end
	CH->CBR1 = cbr1_for(w, h);
	CH->CSAR = src;
	CH->CDAR = dst;
	CH->CTR3 = 0;
	CH->CBR2 = cbr2_for(w, stride, src_inc);
	CH->CLLR = 0; // no linked list
	CH->CCR = (1u << DMA_CCR_EN_Pos);
	uint32_t guard = 2'000'000;
	while (!(CH->CSR & (1u << DMA_CSR_TCF_Pos)) && --guard)
		;
	CH->CCR = 0;
	CH->CFCR = 0x7F00u;
}
} // namespace

void fill(uint32_t dst_base, uint32_t stride, int x, int y, int w, int h, uint32_t color)
{
	if (w <= 0 || h <= 0)
		return;
	*reinterpret_cast<volatile uint32_t *>(g_color_addr) = color;
	clean_dcache_range(reinterpret_cast<void *>(g_color_addr), 4);
	run_2d(g_color_addr, dst_base + uint32_t(y) * stride + uint32_t(x) * 4, w, h, /*src_inc=*/false, stride);
}

void copy(uint32_t dst_base, uint32_t src_base, uint32_t stride, int x, int y, int w, int h)
{
	if (w <= 0 || h <= 0)
		return;
	uint32_t off = uint32_t(y) * stride + uint32_t(x) * 4;
	run_2d(src_base + off, dst_base + off, w, h, /*src_inc=*/true, stride);
}

// ---------------------------------------------------------------------------
//  Async path: queue many rects as an HPDMA linked list, fire once, wait later
// ---------------------------------------------------------------------------
// Each list item ("node") is 8 words in DDR: CTR1,CTR2,CBR1,CSAR,CDAR,CTR3,CBR2,
// CLLR (the HAL's 2D-node order). A node's CLLR = (next_node & LA) | update-mask;
// the last node's CLLR = 0.
namespace
{
constexpr uint32_t MAX_NODES = 48; // >= NSprites * 2
constexpr uint32_t NODE_WORDS = 8;
// Update-mask: reload CTR1,CTR2,CBR1,CSAR,CDAR,CTR3,CBR2 (+ CLLR) from each node.
constexpr uint32_t UPD = (1u << DMA_CLLR_UT1_Pos) | (1u << DMA_CLLR_UT2_Pos) | (1u << DMA_CLLR_UB1_Pos) |
						 (1u << DMA_CLLR_USA_Pos) | (1u << DMA_CLLR_UDA_Pos) | (1u << DMA_CLLR_UT3_Pos) |
						 (1u << DMA_CLLR_UB2_Pos) | (1u << DMA_CLLR_ULL_Pos);

uint32_t g_nodes = 0;  // DDR base of the node array (must be 64KB-aligned)
uint32_t g_colors = 0; // DDR base of per-fill color words
uint32_t g_n = 0;	   // nodes built this batch

volatile uint32_t *node(uint32_t i)
{
	return reinterpret_cast<volatile uint32_t *>(g_nodes + i * NODE_WORDS * 4);
}
void add_node(uint32_t ctr1, uint32_t cbr1, uint32_t csar, uint32_t cdar, uint32_t cbr2)
{
	if (g_n >= MAX_NODES)
		return;
	auto *n = node(g_n);
	n[0] = ctr1;
	n[1] = (1u << DMA_CTR2_SWREQ_Pos) | (3u << DMA_CTR2_TCEM_Pos); // SW req, TC only at last LLI
	n[2] = cbr1;
	n[3] = csar;
	n[4] = cdar;
	n[5] = 0;	 // CTR3
	n[6] = cbr2; // CBR2 (per-row stride)
	n[7] = 0;	 // CLLR -- linked in batch_commit()
	g_n++;
}
} // namespace

void batch_init(uint32_t node_region_ddr)
{
	g_nodes = node_region_ddr;
	g_colors = node_region_ddr + MAX_NODES * NODE_WORDS * 4; // color words after the nodes
	g_n = 0;
}

void batch_begin()
{
	g_n = 0;
}

void batch_copy(uint32_t dst_base, uint32_t src_base, uint32_t stride, int x, int y, int w, int h)
{
	if (w <= 0 || h <= 0)
		return;
	uint32_t off = uint32_t(y) * stride + uint32_t(x) * 4;
	add_node(ctr1_for(true), cbr1_for(w, h), src_base + off, dst_base + off, cbr2_for(w, stride, true));
}

void batch_fill(uint32_t dst_base, uint32_t stride, int x, int y, int w, int h, uint32_t color)
{
	if (w <= 0 || h <= 0)
		return;
	uint32_t idx = g_n;
	uint32_t caddr = g_colors + idx * 4;
	*reinterpret_cast<volatile uint32_t *>(caddr) = color; // per-node fill color source
	add_node(ctr1_for(false),
			 cbr1_for(w, h),
			 caddr,
			 dst_base + uint32_t(y) * stride + uint32_t(x) * 4,
			 cbr2_for(w, stride, false));
}

void batch_commit()
{
	if (g_n == 0)
		return;
	// Link: node i -> node i+1; last node -> 0 (end).
	for (uint32_t i = 0; i < g_n; i++)
		node(i)[7] = (i + 1 < g_n) ? ((g_nodes + (i + 1) * NODE_WORDS * 4) & DMA_CLLR_LA) | UPD : 0u;
	// Push nodes + color words to DDR for the DMA to read.
	clean_dcache_range(reinterpret_cast<void *>(g_nodes), g_n * NODE_WORDS * 4);
	clean_dcache_range(reinterpret_cast<void *>(g_colors), g_n * 4);
	// Fire: point the channel at node 0 and enable -- it walks the list itself.
	CH->CCR = 0;
	CH->CFCR = 0x7F00u;
	CH->CLBAR = g_nodes & DMA_CLBAR_LBA;	  // 64KB region base
	CH->CLLR = (g_nodes & DMA_CLLR_LA) | UPD; // -> node 0, reload all regs
	CH->CCR = (1u << DMA_CCR_EN_Pos);		  // start; returns immediately
}

bool batch_wait()
{
	if (g_n == 0)
		return true;
	uint32_t guard = 8'000'000;
	while (!(CH->CSR & (1u << DMA_CSR_TCF_Pos)) && --guard)
		;
	CH->CCR = 0;
	CH->CFCR = 0x7F00u;
	return guard != 0;
}
} // namespace dma2d
