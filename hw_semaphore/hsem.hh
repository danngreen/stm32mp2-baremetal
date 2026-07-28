#pragma once
#include "stm32mp2xx.h"
#include <cstdint>

// Ported from mdrivlib.
// Changes from the MP15 original:
//   - 16 semaphores, not 32.
//   - Three core IDs, not two: CA35 = 1, CM33 = 2, CM0PLUS = 3. Both A35 cores
//     share core ID 1 -- HSEM cannot tell them apart, so use PROCID (or a
//     different notification path) to distinguish them.
//   - MP2 adds SEC and PRIV bits to the lock word, and a separate set of
//     secure interrupt registers (SCnIER/ICR/ISR/MISR) alongside the non-secure
//     ones. Both cores here run secure and privileged, so every lock carries
//     SEC|PRIV and the secure bank is the one that matters.
//   - The read-back check must ignore CFEN (bit 30), which the hardware sets to
//     report CID filtering -- it is never part of what we wrote.

enum class HWSemaphoreFlag {
	LockFailed = 0,
	LockedOk = 1,
};

// This core's ID as encoded in the lock word: 1 = CA35, 2 = CM33, 3 = CM0PLUS.
// The CMSIS device header hardcodes HSEM_CR_COREID_CURRENT per core, so the
// same template compiles correctly for both the A35 and M33 builds.
constexpr uint32_t HSemCurrentCoreId = HSEM_CR_COREID_CURRENT >> HSEM_CR_COREID_Pos;

constexpr uint32_t HSemSecPriv = HSEM_R_SEC | HSEM_R_PRIV;

template<uint32_t SemaphoreID>
struct HWSemaphore {
	static_assert(SemaphoreID <= HSEM_SEMID_MAX, "STM32MP2 HSEM has semaphores 0 to 15");

	HWSemaphore() = delete;

	// One-step ("fast") lock: reading RLR attempts the take, and the value read
	// back tells you whether you now own it.
	// CAUTION: this cannot arbitrate between the A35 cores since they both
	// have the same core ID 1. Use the two-step lock(processID) with a distinct
	// ID per participant unless every contender has its own core ID.
	static HWSemaphoreFlag lock()
	{
		constexpr uint32_t want = HSEM_RLR_LOCK | HSemSecPriv | HSEM_CR_COREID_CURRENT;
		return ((HSEM->RLR[SemaphoreID] & ~HSEM_RLR_CFEN) == want) ? HWSemaphoreFlag::LockedOk :
																	 HWSemaphoreFlag::LockFailed;
	}

	// Two-step lock: write the lock word, then read it back to confirm we won.
	static HWSemaphoreFlag lock(uint32_t processID)
	{
		const uint32_t want = HSEM_R_LOCK | HSemSecPriv | HSEM_CR_COREID_CURRENT | processID;
		HSEM->R[SemaphoreID] = want;
		return ((HSEM->R[SemaphoreID] & ~HSEM_R_CFEN) == want) ? HWSemaphoreFlag::LockedOk :
																 HWSemaphoreFlag::LockFailed;
	}

	// Releasing = writing the same core/process ID back with the LOCK bit clear.
	// A write from a core that does not hold it is ignored by the hardware.
	static void unlock(uint32_t processID = 0)
	{
		HSEM->R[SemaphoreID] = HSemSecPriv | HSEM_CR_COREID_CURRENT | processID;
	}

	static void unlock_nonrecursive(uint32_t processID)
	{
		disable_channel_ISR();
		unlock(processID);
		enable_channel_ISR();
	}

	static bool is_locked()
	{
		return HSEM->R[SemaphoreID] & HSEM_R_LOCK;
	}

	// --- interrupts (fire when the semaphore is *released*) ------------------
	// Secure register bank, selected at compile time by this core's ID.
	static volatile uint32_t &ier()
	{
		if constexpr (HSemCurrentCoreId == 1)
			return HSEM->SC1IER;
		else if constexpr (HSemCurrentCoreId == 2)
			return HSEM->SC2IER;
		else
			return HSEM->SC3IER;
	}
	static volatile uint32_t &icr()
	{
		if constexpr (HSemCurrentCoreId == 1)
			return HSEM->SC1ICR;
		else if constexpr (HSemCurrentCoreId == 2)
			return HSEM->SC2ICR;
		else
			return HSEM->SC3ICR;
	}
	static volatile uint32_t &misr()
	{
		if constexpr (HSemCurrentCoreId == 1)
			return HSEM->SC1MISR;
		else if constexpr (HSemCurrentCoreId == 2)
			return HSEM->SC2MISR;
		else
			return HSEM->SC3MISR;
	}

	static void enable_channel_ISR()
	{
		ier() = ier() | (1u << SemaphoreID);
	}

	static void disable_channel_ISR()
	{
		ier() = ier() & ~(1u << SemaphoreID);
	}

	static void clear_ISR()
	{
		icr() = 1u << SemaphoreID;
	}

	// aka: is_status_after_masking_pending()
	static bool is_ISR_triggered_and_enabled()
	{
		return misr() & (1u << SemaphoreID);
	}
};
