#pragma once
#include "ipcc_regs.hh"

// Ported from mdrivlib (target/stm32mp1/drivers/ipcc.hh) to the STM32MP2.
// Changes from the MP15 original:
//   - Templated on the IPCC instance base address (MP2 has IPCC1 and IPCC2).
//   - The overloads that also registered an ISR were dropped.
//
// Mental model, which the register names do not make obvious:
//   Each channel is one flag per direction. A core SETS its own flag to say
//   "message waiting for you", and CLEARS the other core's flag to say "I've
//   taken your message". So:
//     set_flag<C>()   -> notify the other core        (raises its RX-occupied)
//     clear_flag<C>() -> acknowledge the other core   (raises its TX-free)
//   "RX occupied" therefore fires on the *other* core's flag going high, and
//   "TX free" fires on *this* core's flag going low.

namespace mdrivlib
{
template<size_t N, uint32_t Base>
struct IPCC_ {
	static_assert(N == 1 || N == 2, "IPCC has Core = 1 and Core = 2 only");

	template<uint32_t C>
	using ThisCore = typename IPCCRegs::Core<N, Base>::template Chan<C>;
	// The peer's register bank: Core 1 <-> Core 2.
	template<uint32_t C>
	using OtherCore = typename IPCCRegs::Core<3 - N, Base>::template Chan<C>;

	// Set/Clear the IPCC Flag:
	// Note: Clearing acts upon the other core's flag
	// and reading the rx occupied flag requires reading the other Core's flags
	template<uint32_t C>
	static void set_flag()
	{
		ThisCore<C>::ChangeFlag::set();
	}
	template<uint32_t C>
	static void clear_flag()
	{
		ThisCore<C>::ChangeFlag::clear();
	}
	template<uint32_t C>
	static bool is_tx_free()
	{
		return ThisCore<C>::FlagStatus::read() == 0;
	}
	template<uint32_t C>
	static bool is_rx_occupied()
	{
		return OtherCore<C>::FlagStatus::read() != 0;
	}
	template<uint32_t C>
	static bool is_other_rx_occupied()
	{
		return ThisCore<C>::FlagStatus::read() != 0;
	}

	// TXFree ISR:
	template<uint32_t C>
	static void enable_chan_txfree_isr()
	{
		ThisCore<C>::FreeISRMasked::clear(); // enable = clear a mask bit (unmask)
	}
	template<uint32_t C>
	static void disable_chan_txfree_isr()
	{
		ThisCore<C>::FreeISRMasked::set();
	}
	// ..._secure() arms the peripheral's secure interrupt line instead of the
	// non-secure one. A channel marked secure in CnSECCFGR only ever asserts
	// the secure line, so a secure channel needs the _secure enable.
	static void enable_all_txfree_isr()
	{
		IPCCRegs::Core<N, Base>::TXFreeISREnable::set();
	}
	static void enable_all_txfree_isr_secure()
	{
		IPCCRegs::Core<N, Base>::SecTXFreeISREnable::set();
	}
	static void disable_all_txfree_isr()
	{
		IPCCRegs::Core<N, Base>::TXFreeISREnable::clear();
	}
	static void disable_all_txfree_isr_secure()
	{
		IPCCRegs::Core<N, Base>::SecTXFreeISREnable::clear();
	}
	// Note: the mask bit is 1 == masked, so "enabled" is the cleared bit.
	// (mdrivlib's version returned the raw mask bit.)
	template<uint32_t C>
	static bool is_chan_txfree_isr_enabled()
	{
		return ThisCore<C>::FreeISRMasked::read() == 0;
	}

	// RXOccupied:
	// Note: RXOcc ISR triggers on the other core's flag going high
	template<uint32_t C>
	static void enable_chan_rxocc_isr()
	{
		ThisCore<C>::OccISRMasked::clear(); // enable = clear a mask bit (unmask)
	}
	template<uint32_t C>
	static void disable_chan_rxocc_isr()
	{
		ThisCore<C>::OccISRMasked::set(); // disable = set a mask bit
	}
	static void enable_all_rxocc_isr()
	{
		IPCCRegs::Core<N, Base>::RXOccISREnable::set();
	}
	static void enable_all_rxocc_isr_secure()
	{
		IPCCRegs::Core<N, Base>::SecRXOccISREnable::set();
	}
	static void disable_all_rxocc_isr()
	{
		IPCCRegs::Core<N, Base>::RXOccISREnable::clear(); // disable = clear an enable bit
	}
	static void disable_all_rxocc_isr_secure()
	{
		IPCCRegs::Core<N, Base>::SecRXOccISREnable::clear();
	}
	template<uint32_t C>
	static bool is_chan_rxocc_isr_enabled()
	{
		return ThisCore<C>::OccISRMasked::read() == 0;
	}
};

// The two instances on the MP2. IPCC1 is the A35 <-> M33 mailbox used here.
template<size_t N>
using IPCC1_ = IPCC_<N, IPCC1_BASE>;
template<size_t N>
using IPCC2_ = IPCC_<N, IPCC2_BASE>;

} // namespace mdrivlib
