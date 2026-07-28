#include "interrupt_m33/interrupt.hh"
#include "print/print.hh"
#include "sync_print.hh"
#include <atomic>

int main()
{
	sync_print("M33: online\n");

	// Set A35_0 lock (used in Test 2)
	HWSemaphore<LockA35c0Ready>::lock(my_procid());

	///////////////////////////////////////
	// Test 1: HSEM Locks

	constexpr unsigned BackoffNops = 300;
	unsigned ctr = 0;

	while (true) {
		for (unsigned i = 0; i < BackoffNops; i++)
			asm("nop");

		sync_print("----------------------------------------33333333333333333333\n");

		if (++ctr >= 100)
			break;
	}

	sync_print("M33: printed 100 times\n");

	///////////////////////////////////////
	// Test 2: HSEM Interrupts

	sync_print("M33: Setting up HSEM interrupt\n");

	std::atomic<bool> ready_flag{false};

	HWSemaphore<LockM33Ready>::clear_ISR();

	InterruptManager::register_and_start_isr(HSEM_S_IRQn, 1, 1, [&] {
		if (HWSemaphore<LockM33Ready>::is_ISR_triggered_and_enabled()) {
			ready_flag.store(true, std::memory_order_release);
			HWSemaphore<LockM33Ready>::clear_ISR();
		} else {
			sync_print("M33: Wrong HSEM channel interrupt\n");
		}
	});

	HWSemaphore<LockM33Ready>::enable_channel_ISR();

	sync_print("M33: waiting for A35_1 to release me...\n");

	while (ready_flag.load(std::memory_order_acquire) == false) {
	}
	print("M33: I got released, running!\n");

	for (unsigned i = 0; i < 10'000'000; i++)
		asm("nop");

	// Release A35_0
	print("M33: releasing A35_0\n");
	HWSemaphore<LockA35c0Ready>::unlock(my_procid());

	while (true) {
		asm("nop");
	}
}
