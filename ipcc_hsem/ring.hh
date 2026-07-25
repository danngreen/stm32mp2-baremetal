#pragma once
#include "hsem.hh"
#include "ipcc.hh"
#include "print/print.hh"
#include <cstdint>

#ifdef CORE_CA35
#include "aarch64/system_reg.hh" // clean/invalidate_dcache_range
#endif

// =============================================================================
//  ring.hh -- the token-ring protocol shared by all three cores
// =============================================================================
// One token travels A35_0 -> A35_1 -> M33 -> A35_0, forever. Each core stamps
// its tag into the token's trail, then hands off using whichever mechanism can
// actually reach the next core:
//
//   A35_0 -> A35_1   SGI  (both cores are one HSEM/IPCC "core", so neither
//                          peripheral can address one A35 core specifically)
//   A35_1 -> M33     IPCC1 channel 1   (A35 is IPCC processor 1)
//   M33   -> A35_0   IPCC1 channel 2   (M33 is IPCC processor 2)
//
// HSEM is the mutual exclusion underneath all of it: one semaphore guards the
// shared state below, another guards the console UART that all three cores
// print to. Every core also hammers the shared counters from its idle loop, so
// the semaphore is genuinely contended and `total` vs the sum of `per_core`
// is a live check that it works.

namespace Ring
{

// --- shared memory -----------------------------------------------------------
// Top 4 kB of SRAM2, above the M33 image (whose linkscript stops short of it).
// Both cores use the *secure* SRAM2 alias, so the address is literally the same
// number on the A35 and the M33.
constexpr uintptr_t SharedAddr = 0x0E07F000;

enum CoreTag : uint32_t { Tag_None = 0, Tag_A35_0 = 1, Tag_A35_1 = 2, Tag_M33 = 3 };
constexpr uint32_t NumCores = 3;
constexpr uint32_t TrailLen = 3; // one slot per hop

struct alignas(64) State {
	uint32_t lap;				  // completed laps
	uint32_t trail[TrailLen];	  // CoreTag of each hop, in order
	uint32_t hop;				  // hops taken so far this lap
	uint32_t per_core[NumCores];  // increments done by each core, under the lock
	uint32_t total;				  // increments seen in total: must equal the sum
	uint32_t contended[NumCores]; // times each core found the lock already taken
	uint32_t ready;				  // bit per core, set once it can receive the token
	uint32_t pad[3];			  // pad to exactly one cache line (see flush/refresh)
};
static_assert(sizeof(State) == 64, "State must be exactly one cache line");

// A hand-off sent before its target is listening would be lost and the ring
// would stall, so core 0 waits for these before starting the first lap.
constexpr uint32_t Ready_A35_0 = 1u << 0;
constexpr uint32_t Ready_A35_1 = 1u << 1;
constexpr uint32_t Ready_M33 = 1u << 2;
constexpr uint32_t Ready_All = Ready_A35_0 | Ready_A35_1 | Ready_M33;

inline State &state() {
	return *reinterpret_cast<State *>(SharedAddr);
}

// The M33 is not in the A35's cache-coherency domain, so the A35 must push its
// writes out and pull the M33's writes in by hand. (The struct is cache-line
// aligned and sized so `dc ivac` can never discard an unrelated dirty line.)
// On the M33 these are no-ops -- it runs with no data cache.
inline void flush() {
#ifdef CORE_CA35
	clean_dcache_range(&state(), sizeof(State));
#else
	__DMB();
#endif
}
inline void refresh() {
#ifdef CORE_CA35
	invalidate_dcache_range(&state(), sizeof(State));
#else
	__DMB();
#endif
}

// --- HSEM --------------------------------------------------------------------
constexpr uint32_t Sem_State = 0; // guards State
constexpr uint32_t Sem_Uart = 1;  // guards the shared console UART

using StateSem = mdrivlib::HWSemaphore<Sem_State>;
using UartSem = mdrivlib::HWSemaphore<Sem_Uart>;

// --- IPCC --------------------------------------------------------------------
// IPCC1, processor 1 = A35, processor 2 = M33.
constexpr uint32_t Chan_ToM33 = 1;
constexpr uint32_t Chan_ToA35 = 2;

using IpccA35 = mdrivlib::IPCC1_<1>;
using IpccM33 = mdrivlib::IPCC1_<2>;

// --- A35 <-> A35 -------------------------------------------------------------
// SGI used to hand the token from A35 core 0 to A35 core 1.
constexpr uint32_t Sgi_TokenToCore1 = 1;

// --- locking helpers ---------------------------------------------------------
// HSEM identifies an owner by core ID, and *both A35 cores are core ID 1*. So
// the one-step (RLR) lock, whose owner word is just LOCK|SEC|PRIV|COREID,
// cannot arbitrate between them: while core 0 holds a semaphore, core 1's
// read-back matches the value it wanted and it wrongly concludes it acquired
// the lock. Both then enter the critical section.
//
// That is what the 8-bit PROCID field in the two-step lock is for. Each
// participant uses a distinct PROCID, so a write to an already-locked
// semaphore is ignored and the read-back returns the *owner's* PROCID --
// which no longer matches, and the loser correctly fails.
constexpr uint32_t ProcId_A35_0 = 1;
constexpr uint32_t ProcId_A35_1 = 2;
constexpr uint32_t ProcId_M33 = 3;

inline uint32_t my_procid() {
#ifdef CORE_CA35
	return (get_mpid() & 0xFF) == 0 ? ProcId_A35_0 : ProcId_A35_1;
#else
	return ProcId_M33;
#endif
}

// Two more rules the demo sticks to, since HSEM still cannot arbitrate between
// two contexts on the *same* core (they share core ID and PROCID):
//   1. Interrupt handlers never take a semaphore -- they only set a flag, and
//      the main loop does the locked work.
//   2. Never hold Sem_State while taking Sem_Uart (copy out, unlock, then
//      print), so the two are always acquired in the same order.

// Spin until this core owns the semaphore. Returns true if it was contended,
// i.e. someone else held it and we had to wait.
template<typename Sem>
inline bool lock_spin() {
	const uint32_t pid = my_procid();
	bool contended = false;
	while (Sem::lock(pid) != mdrivlib::HWSemaphoreFlag::LockedOk)
		contended = true;
	return contended;
}

// Release must present the same PROCID that took it, or the hardware ignores it.
template<typename Sem>
inline void unlock() {
	Sem::unlock(my_procid());
}

// print() the arguments as one uninterruptible unit, so the three cores sharing
// the console UART don't interleave characters mid-line.
template<typename... Ts>
inline void sync_print(Ts... args) {
	lock_spin<UartSem>();
	print(args...);
	unlock<UartSem>();
}

// Announce that this core has its interrupts hooked up and can take the token.
inline void announce_ready(uint32_t ready_bit) {
	lock_spin<StateSem>();
	refresh();
	state().ready |= ready_bit;
	flush();
	unlock<StateSem>();
}

inline const char *tag_name(uint32_t tag) {
	return tag == Tag_A35_0 ? "A35_0" : tag == Tag_A35_1 ? "A35_1" : tag == Tag_M33 ? "M33" : "?";
}

} // namespace Ring
