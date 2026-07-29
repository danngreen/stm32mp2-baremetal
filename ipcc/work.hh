#pragma once
#include "ipcc.hh"
#include <cstdint>

#ifdef CORE_CA35
#include "aarch64/system_reg.hh" // clean/invalidate_dcache_range
#endif

namespace Work
{

// --- shared memory -----------------------------------------------------------
// Top 4 kB of SRAM2, above the M33 image (whose linkscript stops short of it).
// Both cores use the *secure* SRAM2 alias, so the address is literally the same
// number on the A35 and the M33.
constexpr uintptr_t SharedAddr = 0x0E07F000;

struct alignas(64) Mailbox {
	uint32_t index;	 // request: which string A35_0 wants
	uint32_t served; // running count of requests the M33 has answered
	char result[56]; // response: NUL-terminated string, written by the M33
};
static_assert(sizeof(Mailbox) == 64, "Mailbox must be exactly one cache line");

inline Mailbox &mailbox()
{
	return *reinterpret_cast<Mailbox *>(SharedAddr);
}

// The M33 is not in the A35's cache-coherency domain, so the A35 must push its
// writes out and pull the M33's writes in by hand. (The struct is cache-line
// aligned and sized so the invalidate can never discard an unrelated dirty
// line.) On the M33 these are no-ops -- it runs with no data cache.
inline void flush()
{
#ifdef CORE_CA35
	clean_dcache_range(&mailbox(), sizeof(Mailbox));
#else
	__DMB();
#endif
}
inline void refresh()
{
#ifdef CORE_CA35
	invalidate_dcache_range(&mailbox(), sizeof(Mailbox));
#else
	__DMB();
#endif
}

// --- IPCC --------------------------------------------------------------------
// One channel per direction. The M33 also raises ChannelToA35 once at boot,
// before any request: that first notification means "M33 is listening".
enum { ChannelToM33 = 1, ChannelToA35 = 2 };

} // namespace Work
