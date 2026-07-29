#include "drivers/copro_m33.hh"
#include "drivers/rcc.hh"
#include "interrupt/interrupt.hh"
#include "ipcc.hh"
#include "print/print.hh"
#include "stm32mp2xx.h"
#include "work.hh"
#include <atomic>
#include <cstdint>

#include "firmware_m33.h"

namespace
{
constexpr uintptr_t M33_LOAD_ADDR = 0x0E060000UL;

// RIFSC peripheral id (stm32mp2xx_hal_rif.h): expose the mailbox peripheral
// to our secure context.
constexpr uint32_t RifscId_IPCC1 = 144;

std::atomic<bool> response_ready = false;

void delay(unsigned n)
{
	for (unsigned i = 0; i < n; i++)
		asm("nop");
}

constexpr unsigned RequestPauseNops = 100'000'000;

void ipcc_init_secure()
{
	using namespace Work;

	RCC_Enable::IPCC1_::set();
	RCC_Reset::IPCC1_::set();
	RCC_Reset::IPCC1_::clear();

	// mark secure so our secure masters own it
	RISC->SECCFGR[RifscId_IPCC1 / 32] |= (1u << (RifscId_IPCC1 % 32));

	// Secure/NS for three things have to agree:
	//   1. the channel security bits set here
	//   2. interrupt unmask bit: SECRXOIE or RXOIE
	//   3. IRQ responded to IPCC1_RX_S_IRQn or IPCC1_RX_IRQn
	constexpr uint32_t chan_mask = (1u << (ChannelToM33 - 1)) | (1u << (ChannelToA35 - 1));
	IPCC1->C1SECCFGR |= chan_mask;
	IPCC1->C2SECCFGR |= chan_mask;
	IPCC1->C1PRIVCFGR |= chan_mask;
	IPCC1->C2PRIVCFGR |= chan_mask;

	// Start from a known state: no stale flags from a previous run.
	IPCC1->C1SCR = 0xFFFFu; // clear all of processor 1's "message waiting" flags
	IPCC1->C2SCR = 0xFFFFu;
}

// Wait (bounded) for the M33's channel 2 notification. Returns false on timeout.
bool wait_for_response()
{
	for (unsigned tries = 0; tries < 2000; tries++) {
		if (response_ready) {
			response_ready = false;
			return true;
		}
		delay(100'000);
	}
	return false;
}
} // namespace

int main()
{
	using namespace Work;

	print("\nIPCC request/response demo (A35 <-> M33)\n");
	print("===============================================\n\n");

	park_m33();

	ipcc_init_secure();

	mailbox().index = 0;
	mailbox().served = 0;
	mailbox().result[0] = '\0';
	mailbox().flush();

	// Register before starting the M33 so its "online" notification can't be
	// missed. The ISR only acks the mailbox flag and raises ours.
	IPCC1_<1>::enable_all_rxocc_isr_secure(); // our channels are secure -> secure line
	IPCC1_<1>::enable_chan_rxocc_isr<ChannelToA35>();
	InterruptManager::register_and_start_isr(IPCC1_RX_S_IRQn, 1, 0, [] {
		if (IPCC1_<1>::is_rx_occupied<ChannelToA35>()) {
			IPCC1_<1>::clear_flag<ChannelToA35>(); // ack, or it re-fires forever
			response_ready = true;
		}
	});

	print("A35_0: starting M33 (", m33_firmware_len, " bytes in SRAM2)\n");
	start_m33(std::span<const uint8_t>{m33_firmware, m33_firmware_len}, M33_LOAD_ADDR);

	// The M33 raises channel 2 once when it is listening (and prints its own
	// banner meanwhile -- we stay off the UART until it's done).
	if (wait_for_response())
		print("A35_0: M33 is online\n\n");
	else
		print("A35_0: WARNING: no answer from the M33 -- asking anyway\n\n");

	uint32_t index = 1;
	while (index < 12) {

		// The mailbox is ours: write the request and hand it to the M33
		mailbox().index = index;
		mailbox().flush();
		IPCC1_<1>::set_flag<ChannelToM33>();

		// The mailbox now belongs to the M33 -- hands off until it notifies us
		if (!wait_for_response()) {
			print("A35_0: WARNING: no answer for index ", index, "\n");
		} else {
			mailbox().refresh();
			print("A35_0: asked for ", index, ", ");
			print("M33 answered \"", mailbox().result, "\" ");
			print("(", mailbox().served, " served)\n");
		}

		index++;

		delay(RequestPauseNops);
	}

	print("Done.\n");
	while (true)
		asm("nop");
}
