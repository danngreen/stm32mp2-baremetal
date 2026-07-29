#pragma once
#include <cstdint>

#ifdef CORE_CA35
#include "aarch64/system_reg.hh"
#endif

namespace Work
{

// --- shared memory -----------------------------------------------------------
// Top 4 kB of SRAM2, above the M33 image
constexpr uintptr_t SharedAddr = 0x0E07F000;

struct alignas(64) Mailbox {
	uint32_t index;	 // request: which string A35_0 wants
	uint32_t served; // running count of requests the M33 has answered
	char result[56]; // response: null-terminated string, written by the M33

	// The M33 is not in the A35's cache-coherency domain, so the A35 must push its
	// writes out and pull the M33's writes in by hand.
#ifdef CORE_CA35
	void flush()
	{
		clean_dcache_range(this, sizeof(Mailbox));
	}
	void refresh()
	{
		invalidate_dcache_range(this, sizeof(Mailbox));
	}
#else
	void flush()
	{
		__DMB();
	}
	void refresh()
	{
		__DMB();
	}
#endif
};
static_assert(sizeof(Mailbox) == 64, "Mailbox must be exactly one cache line");

inline Mailbox &mailbox()
{
	return *reinterpret_cast<Mailbox *>(SharedAddr);
}

// IPCC channels
enum { ChannelToM33 = 1, ChannelToA35 = 2 };

} // namespace Work
