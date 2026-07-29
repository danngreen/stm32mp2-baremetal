// Cortex-M33 side of the IPCC request/response demo.
//
// The A35 loads this image into SRAM2 and releases CPU2 from hold-boot. We
// answer "work" requests: the A35 puts an index in the shared mailbox and
// raises IPCC1 channel 1; we write the matching string back and raise
// channel 2. See work.hh for the protocol and why the mailbox has no
// semaphore around it.
//
// The M33 runs *secure* (the A35 sets CFG_SECEXT before releasing it) with the
// SAU disabled, so the whole address map is Secure: the same peripheral
// addresses and the same secure IPCC resources the A35 uses.
//
// No data cache here, so the shared mailbox needs no maintenance on this side
// -- Work::flush()/refresh() reduce to a barrier.

#include "interrupt_m33/interrupt.hh"
#include "print/print.hh"
#include "stm32mp2xx.h"
#include "work.hh"
#include <cstdint>

using namespace Work;

namespace
{
volatile bool request_pending = false; // set by the IPCC RX interrupt

// The "work": look up a string by index (1-based).
constexpr const char *colors[] = {
	"Orange",
	"Blue",
	"Green",
	"Crimson",
	"Violet",
	"Silver",
	"Teal",
	"Chartreuse",
};
constexpr uint32_t NumColors = sizeof(colors) / sizeof(colors[0]);

void write_result(const char *str)
{
	auto &m = mailbox();
	uint32_t i = 0;
	while (str[i] && i < sizeof(m.result) - 1) {
		m.result[i] = str[i];
		i++;
	}
	m.result[i] = '\0';
	m.served++;
}
} // namespace

int main()
{
	// Deliberately no init_uart() here: the A35 brought the console up in its
	// startup, long before it released us from hold-boot. Re-initialising would
	// write USART->CR1 = 0 and kill whatever character the A35 had in flight.
	//
	// This is the only thing the M33 ever prints, and the A35 is silently
	// waiting for our channel 2 "online" notification while we print it -- the
	// two cores never use the UART at the same time.
	print("M33: online, serving ", NumColors, " colors\n");

	// Unmask our side of the request channel and let the NVIC deliver it. The
	// channel was marked secure by the A35, so its interrupt arrives on the
	// secure line.
	IPCC1_<2>::enable_all_rxocc_isr_secure();
	IPCC1_<2>::enable_chan_rxocc_isr<ChannelToM33>();

	// Note: The IRQ should be 173 in M33's numbering
	static_assert(IPCC1_RX_S_IRQn == 173);
	InterruptManager::register_and_start_isr(IPCC1_RX_S_IRQn, 1, 0, [] {
		if (IPCC1_<2>::is_rx_occupied<ChannelToM33>()) {
			IPCC1_<2>::clear_flag<ChannelToM33>(); // ack, or it re-fires forever
			request_pending = true;
		}
	});

	// Tell the A35 we are listening: its first channel 2 interrupt means
	// "M33 online", every later one means "your result is ready".
	IPCC1_<2>::set_flag<ChannelToA35>();

	while (true) {
		if (request_pending) {
			request_pending = false;

			// The mailbox is ours from the channel 1 interrupt until we raise
			// channel 2 -- no other protection, by design (see work.hh).
			uint32_t index = mailbox().index;
			if (index >= 1 && index <= NumColors)
				write_result(colors[index - 1]);
			else
				write_result("no such color");
			__DMB();

			IPCC1_<2>::set_flag<ChannelToA35>();
		} else {
			// Sleep until an interrupt is pended. PRIMASK is set around the
			// check so a request arriving between the flag test and the WFI
			// still wakes it (a pended-but-masked interrupt wakes WFI).
			__disable_irq();
			if (!request_pending)
				__WFI();
			__enable_irq();
		}
	}
}
