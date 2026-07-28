#include "aarch64/system_reg.hh"
#include "drivers/copro_m33.hh"
#include "drivers/psci.hh"
#include "drivers/rcc.hh"
#include "interrupt/interrupt.hh"
#include "print/print.hh"
#include "stm32mp2xx.h"
#include <cstdint>

#include "firmware_m33.h"
#include "sync_print.hh"

// Defined in aux_core_startup.s -- the reset entry point for A35 core 1.
extern "C" void aux_core_startup();

namespace
{

constexpr uintptr_t M33_LOAD_ADDR = 0x0E060000UL;

void delay(unsigned n)
{
	for (unsigned i = 0; i < n; i++)
		asm("nop");
}

void hsem_init()
{
	RCC_Enable::HSEM_::set();

	// Reset to clear all locks
	RCC_Reset::HSEM_::set();
	RCC_Reset::HSEM_::clear();

	// mark peripheral secure so our secure masters own it
	constexpr uint32_t RifscId_HSEM = 146;
	RISC->SECCFGR[RifscId_HSEM / 32] |= (1u << (RifscId_HSEM % 32));

	constexpr uint32_t sem_mask =
		(1u << LockA35c0Ready) | (1u << LockA35c1Ready) | (1u << LockM33Ready) | (1u << LockUart);
	HSEM->SECCFGR |= sem_mask;
	HSEM->PRIVCFGR |= sem_mask;
}

} // namespace

int main()
{
	print("\nHSEM demo (A35 core 0, A35 core 1, M33)\n");
	print("=====================================================\n\n");

	park_m33(); // before periph_init resets the mailbox peripherals

	hsem_init();

	a35_released.core0 = 0;
	a35_released.core1 = 0;

	sync_print("A35_0: HSEM clocked and secured\n");

	start_m33(std::span<const uint8_t>{m33_firmware, m33_firmware_len}, M33_LOAD_ADDR);
	sync_print("A35_0: started M33\n");

	start_cpu1(aux_core_startup, 0);
	sync_print("A35_0: started A35 core 1\n");

	// Set A35_1 lock (used in Test 2)
	HWSemaphore<LockA35c1Ready>::lock(my_procid());

	///////////////////////////////////////
	// Test 1: HSEM Locks

	unsigned ctr = 0;
	constexpr unsigned BackoffNops = 1500;

	while (true) {
		sync_print("11111111111111111111........................................\n");

		for (unsigned i = 0; i < BackoffNops; i++)
			asm("nop");

		if (++ctr >= 100)
			break;
	}

	sync_print("A35_0: printed 100 times\n");

	// Delay to make sure all cores are spinning in Test 2
	for (unsigned i = 0; i < 10'000'000; i++)
		asm("nop");

	///////////////////////////////////////
	// Test 2: HSEM Interrupts

	sync_print("A35_0: Setting up HSEM interrupt\n");

	// One ISR handler for both A35 channels, since they share an HSEM IRQ.
	HWSemaphore<LockA35c0Ready>::clear_ISR();
	HWSemaphore<LockA35c1Ready>::clear_ISR();

	InterruptManager::register_and_start_isr(HSEM_S_IRQn, 1, 1, [] {
		if (HWSemaphore<LockA35c0Ready>::is_ISR_triggered_and_enabled()) {
			HWSemaphore<LockA35c0Ready>::clear_ISR();
			// Write shared memory
			a35_released.core0 = 1;
		}
		if (HWSemaphore<LockA35c1Ready>::is_ISR_triggered_and_enabled()) {
			HWSemaphore<LockA35c1Ready>::clear_ISR();
			// Write shared memory
			a35_released.core1 = 1;
		}
	});

	HWSemaphore<LockA35c0Ready>::enable_channel_ISR();
	HWSemaphore<LockA35c1Ready>::enable_channel_ISR();

	// Release A35_1, then spin until the M33 releases us (via the chain).
	sync_print("A35_0: releasing A35_1\n");
	HWSemaphore<LockA35c1Ready>::unlock(my_procid());

	while (a35_released.core0 == 0) {
	}
	print("A35_0: I got released, test is complete!\n");

	while (true)
		asm("nop");
}
