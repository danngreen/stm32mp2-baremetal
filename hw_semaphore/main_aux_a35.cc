#include "print/print.hh"
#include "sync_print.hh"

extern "C" void aux_main()
{
	sync_print("A35_1: online\n");

	// Set M33 lock (used in Test 2)
	HWSemaphore<LockM33Ready>::lock(my_procid());

	///////////////////////////////////////
	// Test 1: HSEM Locks

	constexpr unsigned BackoffNops = 1500;
	unsigned ctr = 0;

	while (true) {
		for (unsigned i = 0; i < BackoffNops; i++)
			asm("nop");

		sync_print("____________________22222222222222222222____________________\n");

		if (++ctr >= 100)
			break;
	}

	sync_print("A35_1: printed 100 times\n");

	///////////////////////////////////////
	// Test 2: HSEM Interrupts

	sync_print("A35_1: waiting for A35_0 to release me...\n");

	// Check shared memory for the release flag
	// (cleared in the HSEM IRQ handler setup by A35_0)
	while (a35_released.core1 == 0) {
	}
	print("A35_1: I got released, running!\n");

	for (unsigned i = 0; i < 10'000'000; i++)
		asm("nop");

	// Release M33
	print("A35_1: releasing M33\n");
	HWSemaphore<LockM33Ready>::unlock(my_procid());

	while (true) {
		asm("nop");
	}
}
