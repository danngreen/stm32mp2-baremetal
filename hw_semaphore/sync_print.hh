#pragma once
#include "aarch64/system_reg.hh"
#include "hsem.hh"
#include <atomic>

// A35 cores can unlock each other's HSEMs unless we use the PROCID field
// to set a unique Process ID which is needed to unlock. The M33 ProcID is
// not really needed but keeps it symmetrical.
enum ProcId { A35_0, A35_1, M33 };

inline uint32_t my_procid()
{
#ifdef CORE_CA35
	return (get_mpid() & 0xFF) == 0 ? ProcId::A35_0 : ProcId::A35_1;
#else
	return ProcId::M33;
#endif
}

enum {
	LockA35c0Ready,
	LockA35c1Ready,
	LockM33Ready,
	LockUart,
};

#ifdef CORE_CA35
// Cross-core release flags for the interrupt test. HSEM sees the two A35
// cores as a single core (ID 1), so both A35 channels share one SCnIER bank
// and one GIC interrupt line -- only one core can service it. Core 0 owns the
// handler for both channels and posts core 1's wake-up here. The word lives
// in non-cached memory
struct A35ReleaseFlags {
	std::atomic<uint32_t> core0;
	std::atomic<uint32_t> core1;
};
inline A35ReleaseFlags &a35_released = *reinterpret_cast<A35ReleaseFlags *>(0x8A000000);

#endif

// print() the arguments as one uninterruptible unit, so the three cores sharing
// the console UART don't interleave characters mid-line.
// This can lead to dead-lock if used inside an IRQ handler
template<typename... Ts>
inline void sync_print(Ts... args)
{
	while (HWSemaphore<LockUart>::lock(my_procid()) != HWSemaphoreFlag::LockedOk)
		;

	print(args...);

	HWSemaphore<LockUart>::unlock(my_procid());
}
