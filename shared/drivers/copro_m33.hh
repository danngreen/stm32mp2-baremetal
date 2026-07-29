#pragma once
#include "aarch64/system_reg.hh"
#include "stm32mp2xx.h"
#include <span>

// Functions are inline because realistically a project will only ever
// call these once in the A35 binary.

inline void load_m33_firmware(std::span<const uint8_t> firmware, uint32_t address)
{
	// Copy the embedded blob into SRAM2
	auto *dst = reinterpret_cast<volatile uint8_t *>(address);
	for (unsigned i = 0; i < firmware.size(); i++)
		dst[i] = firmware[i];

	// Clean the cache so a write to memory is ensured
	for (uintptr_t a = address; a < address + firmware.size(); a += 64)
		clean_dcache_address(a);

	dsb_sy();
	isb();
}

// The M33 runs secure, so a secure instruction fetch to SRAM2 must land on
// secure RISAB pages: RISAB SRWIAD only forgives secure *data* accesses to
// non-secure pages, not fetches. block_ram_enable_el3() cleared all page
// security bits at boot, so flip SRAM2's pages (RISAB4) back to secure. The
// A35's own accesses are secure too, so it can still read/write SRAM2.
inline void sram2_set_secure()
{
	for (auto i = 0u; i < 32; i++)
		RISAB4->PGSECCFGR[i] = 0xFF; // all 8 blocks of each page secure
}

// Park CPU2 first for a known-clean state: assert hold-boot (BOOT_CPU2=0)
// and assert the M33 reset. Mirrors OP-TEE rproc_stop().
inline void park_m33()
{
	RCC->CPUBOOTCR &= ~RCC_CPUBOOTCR_BOOT_CPU2;
	RCC->C2RSTCSETR = RCC_C2RSTCSETR_C2RST;
}

inline void start_m33(std::span<const uint8_t> firmware, uint32_t address)
{
	// enable hold-boot and reset:
	park_m33();

	load_m33_firmware(firmware, address);

	sram2_set_secure();

	// Enable TrustZone security so M33 can run in secure mode:
	CA35SYSCFG->M33_TZEN_CR |= CA35SYSCFG_M33_TZEN_CR_CFG_SECEXT;

	// Set the secure vector table:
	CA35SYSCFG->M33_INITSVTOR_CR = address & CA35SYSCFG_M33_INITSVTOR_CR_INITSVTOR_Msk;

	dsb_sy();
	isb();

	// Release hold-boot -> the M33 boots from INITSVTOR. The hardware
	// automatically releases the M33 reset (see OP-TEE stm32_rproc_start()).
	RCC->CPUBOOTCR |= RCC_CPUBOOTCR_BOOT_CPU2;
}
