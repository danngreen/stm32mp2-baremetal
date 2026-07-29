#include "aarch64/system_reg.hh"
#include "drivers/smc.hh"
#include "stm32mp257cxx_ca35.h"
#include <cstdint>

#define PSCI_CPU_ON_AARCH64 0xC4000003ULL

// If a debugger (or a debug-aware bootloader) left CPU1 halted in external
// debug state, the reset-based release in start_cpu1 has no effect: the reset
// request never completes for a debug-halted core. Detect that state and
// restart the core through its cross-trigger interface first. The CoreSight
// debug components are visible to software on the system bus at 0x4A3xxxxx
// (CPU1 external debug at 0x4A310000, its CTI at 0x4A320000).
static void unhalt_cpu1()
{
	auto reg32 = [](uintptr_t addr) -> volatile uint32_t & { return *reinterpret_cast<volatile uint32_t *>(addr); };

	constexpr uintptr_t CPU1_DBG = 0x4A310000; // CPU1 external debug (EDPRSR etc.)
	constexpr uintptr_t CPU1_CTI = 0x4A320000; // CPU1 cross-trigger interface

	// Debug not enabled => nothing can be halted, and the debug APB may not
	// be clocked, so don't touch it.
	if ((DBGMCU->CR & 0x7) == 0)
		return;

	constexpr uint32_t EDPRSR_HALTED = 1 << 4;
	if ((reg32(CPU1_DBG + 0x314) & EDPRSR_HALTED) == 0) // EDPRSR
		return;

	reg32(CPU1_CTI + 0xFB0) = 0xC5ACCE55; // CTILAR: unlock sw access
	reg32(CPU1_CTI + 0x000) = 1;		  // CTICONTROL: enable CTI
	reg32(CPU1_CTI + 0x010) = 1;		  // CTIINTACK: ack halt trigger (out 0)
	reg32(CPU1_CTI + 0x0A4) = 2;		  // CTIOUTEN1: channel 1 -> restart trigger
	reg32(CPU1_CTI + 0x01C) = 2;		  // CTIAPPPULSE: pulse channel 1

	while (reg32(CPU1_DBG + 0x314) & EDPRSR_HALTED)
		;
}

int start_cpu1(void (*cpu1_entry)(void), uint64_t context)
{
	uint64_t mpidr0 = get_mpid();

	// Keep affinity levels except AFF0, then set AFF0=1
	// (AFF0 is bits[7:0] in MPIDR on Armv8-A)
	uint64_t target = (mpidr0 & ~0xFFULL) | 1ULL;

	if (get_current_el() == 3) {
		unhalt_cpu1();

		// Set reset vector for CPU1 in 64-bit mode
		CA35SYSCFG->VBAR_CR = ((uint32_t)(uintptr_t)cpu1_entry) & ~0b11;

		// Reset CPU1 processor core 1
		RCC->C1P1RSTCSETR = RCC_C1P1RSTCSETR_C1P1RST;
		while (RCC->C1P1RSTCSETR & RCC_C1P1RSTCSETR_C1P1RST) {
			;
		}

		return 0;
	} else {
		auto ret = smc_call(PSCI_CPU_ON_AARCH64, target, (uint64_t)cpu1_entry, context, 0, 0, 0, 0);

		return ret.a0; // 0 means success
	}
}
