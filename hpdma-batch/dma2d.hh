#pragma once
#include <cstdint>

// Note, this is hardwired to HPDMA1, Channel 12 for demonstration purposes.
//
// This uses the HPDMA to write a batch (linked-list) of 2D strided DMA transfers,
// i.e. to fill a series of rectangles in a framebuffer. The DMA will walk the whole list
// on its own while the CPU does other work.

namespace dma2d
{
// Enable HPDMA1, make the channel secure/privileged. `color_scratch_ddr` is a
// DMA-reachable DDR word (in the framebuffer pool, not the code image) used as
// the fixed source for solid fills. `node_region_ddr` is a 64KB-aligned,
// DMA-reachable DDR region for the async linked-list nodes + fill-color words.
void init(uint32_t color_scratch_ddr, uint32_t node_region_ddr);

void fill(uint32_t dst_base, uint32_t stride, int x, int y, int w, int h, uint32_t color);

void copy(uint32_t dst_base, uint32_t src_base, uint32_t stride, int x, int y, int w, int h);

// Usage:
//   batch_begin();
//   for (...) { batch_copy(...); batch_fill(...); }  // queue nodes
//   batch_commit();                                  // fire, returns at once
//   ... CPU is free here ...
//   batch_wait();                                    // block until the list is done

void batch_init(uint32_t node_region_ddr);
void batch_begin();
void batch_fill(uint32_t dst_base, uint32_t stride, int x, int y, int w, int h, uint32_t color);
void batch_copy(uint32_t dst_base, uint32_t src_base, uint32_t stride, int x, int y, int w, int h);
void batch_commit();
bool batch_wait(); // false on timeout
} // namespace dma2d
