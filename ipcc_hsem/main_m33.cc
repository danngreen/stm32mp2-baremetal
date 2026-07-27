// Cortex-M33 side of the IPCC/HSEM token ring.
//
// The A35 loads this image into SRAM2 and releases CPU2 from hold-boot. We then
// wait for the token on IPCC1 channel 1, stamp it, and send it back to the A35
// on channel 2 -- while also competing for the shared HSEM semaphores from the
// idle loop, same as the two A35 cores.
//
// The M33 runs *secure* (the A35 sets CFG_SECEXT before releasing it) with the
// SAU disabled, so the whole address map is Secure: the same peripheral
// addresses and the same secure IPCC/HSEM resources the A35 uses.
//
// No data cache here, so the shared state needs no maintenance on this side --
// Ring::flush()/refresh() reduce to a barrier (see ring.hh).

#include "interrupt_m33/interrupt.hh"
#include "ring.hh"
#include "stm32mp2xx.h"
#include <cstdint>

using namespace Ring;

namespace
{
constexpr uint32_t MyCore = 2; // index into per_core[]/contended[]

volatile bool token_here = false; // set by the IPCC RX interrupt

void delay(unsigned n)
{
	for (unsigned i = 0; i < n; i++)
		asm("nop");
}

void bump_counter()
{
	bool contended = lock_spin<LockState>();
	refresh();

	auto &s = state();
	s.per_core[MyCore]++;
	s.total++;
	if (contended)
		s.contended[MyCore]++;

	flush();
	unlock<LockState>();
}

void stamp_token()
{
	bool contended = lock_spin<LockState>();
	refresh();

	auto &s = state();
	if (s.hop < TrailLen)
		s.trail[s.hop] = Tag_M33;
	s.hop++;
	if (contended)
		s.contended[MyCore]++;

	flush();
	unlock<LockState>();
}
} // namespace

int main()
{
	// Deliberately no init_uart() here: the A35 brought the console up in its
	// startup, long before it released us from hold-boot. Re-initialising would
	// write USART->CR1 = 0 and kill whatever character the A35 had in flight at
	// that instant -- which corrupted the tail of its line every single boot.
	sync_print("M33: online\n");

	// Unmask our side of the token channel and let the NVIC deliver it. The
	// channel was marked secure by the A35, so its interrupt arrives on the
	// secure line.
	IPCC1_<2>::enable_all_rxocc_isr_secure();
	IPCC1_<2>::enable_chan_rxocc_isr<CommChannelToM33>();

	// Note: The IRQ should be 173 in M33's numbering
	static_assert(IPCC1_RX_S_IRQn == 173);
	InterruptManager::register_and_start_isr(IPCC1_RX_S_IRQn, 1, 0, [] {
		if (IPCC1_<2>::is_rx_occupied<CommChannelToM33>()) {
			IPCC1_<2>::clear_flag<CommChannelToM33>(); // ack, or it re-fires forever
			token_here = true;
		}
	});

	announce_ready(Ready_M33);

	while (true) {
		if (token_here) {
			token_here = false;
			stamp_token();
			// Send the token back to A35 core 0: setting our flag raises the
			// A35's RX-occupied interrupt on channel 2.
			IPCC1_<2>::set_flag<CommChannelToA35>();
		}
		bump_counter();
		delay(500); // leave the semaphore free for the (much faster) A35 cores
	}
}
