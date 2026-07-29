#include "interrupt_m33/interrupt.hh"
#include "ipcc.hh"
#include "print/print.hh"
#include "stm32mp2xx.h"
#include "work.hh"
#include <atomic>
#include <cstdint>

using namespace Work;

namespace
{
std::atomic<bool> request_pending{false}; // set by the IPCC RX interrupt

// The "work": look up a string by index (1-based).
constexpr std::array colors{
	"Orange",
	"Blue",
	"Green",
	"Crimson",
	"Violet",
	"Silver",
	"Teal",
	"Chartreuse",
};
constexpr uint32_t NumColors = colors.size();

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

			// The mailbox is our ownership until we respond
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
