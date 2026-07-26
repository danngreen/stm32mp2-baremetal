// Cortex-A35 side of the IPCC/HSEM token ring.
//
// Core 0 brings everything up (clocks, security, the M33 image, A35 core 1),
// then starts a token going around the ring and prints a summary each lap.
// Core 1 runs aux_main() below and just forwards the token.
//
// Both A35 cores run the same binary out of DDR; the M33 image is embedded in
// this one's .rodata and copied into SRAM2 at boot.

#include "aarch64/system_reg.hh"
#include "drivers/copro_m33.hh"
#include "drivers/rcc.hh"
#include "interrupt/interrupt.hh"
#include "print/print.hh"
#include "psci.hh"
#include "ring.hh"
#include "stm32mp2xx.h"
#include <cstdint>

#include "firmware_m33.h"

using namespace Ring;

// Defined in aux_core_startup.s -- the reset entry point for A35 core 1.
extern "C" void aux_core_startup();
extern "C" void aux_main();

namespace
{
constexpr uintptr_t M33_LOAD_ADDR = 0x0E060000UL;

// RIFSC peripheral ids (stm32mp2xx_hal_rif.h): expose the two mailbox
// peripherals to our secure context.
constexpr uint32_t RifscId_IPCC1 = 144;
constexpr uint32_t RifscId_HSEM = 146;

void delay(unsigned n)
{
	for (unsigned i = 0; i < n; i++)
		asm("nop");
}

// Crude nop-loop counts (this build is -O2, so these are not milliseconds --
// tune them if the console scrolls too fast or the ring feels sluggish).
constexpr unsigned LapPauseNops = 50'000'000; // between lap summaries
constexpr unsigned BackoffNops = 2000;		  // between lock attempts in the idle loop

// Set by the IPCC RX interrupt; the ring work itself is done in the main loop
// so that no interrupt handler ever holds a semaphore (see ring.hh).
volatile bool token_returned = false; // core 0: M33 handed the token back
volatile bool token_at_core1 = false; // core 1: core 0 handed us the token

void ipcc_init_secure()
{
	RCC_Enable::IPCC1_::set();
	RCC_Reset::IPCC1_::set();
	RCC_Reset::IPCC1_::clear();

	// mark secure so our secure masters own it
	RISC->SECCFGR[RifscId_IPCC1 / 32] |= (1u << (RifscId_IPCC1 % 32));

	// The whole system is secure, so mark the resources we use secure too, and
	// use the secure interrupt lines. A channel's security decides which
	// of the two IRQ lines its interrupt comes out on, so three things have to
	// agree:
	//   1. the channel security bits set here
	//   2. unmask secure interrupt: SECRXOIE, not RXOIE
	//   3. IRQ is on IPCC1_RX_S_IRQn, not IPCC1_RX_IRQn
	constexpr uint32_t chan_mask = (1u << (Chan_ToM33 - 1)) | (1u << (Chan_ToA35 - 1));
	IPCC1->C1SECCFGR |= chan_mask;
	IPCC1->C2SECCFGR |= chan_mask;
	IPCC1->C1PRIVCFGR |= chan_mask;
	IPCC1->C2PRIVCFGR |= chan_mask;

	// Start from a known state: no stale flags from a previous run.
	IPCC1->C1SCR = 0xFFFFu; // clear all of processor 1's "message waiting" flags
	IPCC1->C2SCR = 0xFFFFu;
}

void hsem_init()
{
	RCC_Enable::HSEM_::set();

	// Reset to clear all locks
	RCC_Reset::HSEM_::set();
	RCC_Reset::HSEM_::clear();

	// mark peripheral secure so our secure masters own it
	RISC->SECCFGR[RifscId_HSEM / 32] |= (1u << (RifscId_HSEM % 32));

	constexpr uint32_t sem_mask = (1u << Sem_State) | (1u << Sem_Uart);
	HSEM->SECCFGR |= sem_mask;
	HSEM->PRIVCFGR |= sem_mask;
}

void lap_test_init()
{
	auto &s = state();
	s.lap = 0;
	s.hop = 0;
	s.total = 0;
	s.ready = 0;
	for (auto &t : s.trail)
		t = Tag_None;
	for (uint32_t i = 0; i < NumCores; i++) {
		s.per_core[i] = 0;
		s.contended[i] = 0;
	}
	flush();
}

// Spin until every core has hooked up its interrupts. Bounded, so a core that
// never arrives degrades to a printed warning instead of a silent hang.
// Returns the last ready mask seen.
uint32_t wait_for_all_cores()
{
	uint32_t ready = 0;
	for (unsigned tries = 0; tries < 2000 && ready != Ready_All; tries++) {
		lock_spin<StateSem>();
		refresh();
		ready = state().ready;
		unlock<StateSem>();
		if (ready != Ready_All)
			delay(100'000);
	}
	return ready;
}

// --- ring ------------------------------------------------------------------
// Stamp this core into the token's trail.
void stamp_token(uint32_t tag, uint32_t core_idx)
{
	bool contended = lock_spin<StateSem>();
	refresh();

	auto &s = state();
	if (s.hop < TrailLen)
		s.trail[s.hop] = tag;
	s.hop++;
	if (contended)
		s.contended[core_idx]++;

	flush();
	unlock<StateSem>();
}

// Grab the lock just to bump a counter. This is the contention generator: all
// three cores do it continuously, so the ring's own lock attempts really do
// collide with other cores.
void bump_counter(uint32_t core_idx)
{
	bool contended = lock_spin<StateSem>();
	refresh();

	auto &s = state();
	s.per_core[core_idx]++;
	s.total++; // must stay equal to the sum of per_core[]
	if (contended)
		s.contended[core_idx]++;

	flush();
	unlock<StateSem>();
}

void start_lap()
{
	bool contended = lock_spin<StateSem>();
	refresh();

	auto &s = state();
	s.hop = 1;
	s.trail[0] = Tag_A35_0;
	for (uint32_t i = 1; i < TrailLen; i++)
		s.trail[i] = Tag_None;
	if (contended)
		s.contended[0]++;

	flush();
	unlock<StateSem>();

	// Hand off to A35 core 1. HSEM and IPCC both see the two A35 cores as one
	// "core", so an SGI is the only way to address core 1 specifically.
	GIC_SendSGI(static_cast<IRQn_Type>(Sgi_TokenToCore1), 0b10, 0b00);
}

void finish_lap()
{
	// Take a copy under the lock, then print outside it: holding the state
	// semaphore while waiting for the UART one would invert the lock order.
	State snap;
	{
		bool contended = lock_spin<StateSem>();
		refresh();

		auto &s = state();
		s.lap++;
		if (contended)
			s.contended[0]++;
		snap = s;

		flush();
		unlock<StateSem>();
	}

	uint32_t sum = 0;
	for (uint32_t i = 0; i < NumCores; i++)
		sum += snap.per_core[i];

	// The line is built from several print() calls, so hold the UART semaphore
	// across all of them -- one sync_print() per piece would let another core
	// slip its own output into the middle of the line.
	lock_spin<UartSem>();
	print("lap ", (int)snap.lap, ": ");
	for (uint32_t i = 0; i < TrailLen; i++)
		print(tag_name(snap.trail[i]), i + 1 < TrailLen ? " -> " : "");
	print("  | locked bumps A35_0/A35_1/M33 = ",
		  (int)snap.per_core[0],
		  "/",
		  (int)snap.per_core[1],
		  "/",
		  (int)snap.per_core[2],
		  ", total ",
		  (int)snap.total,
		  sum == snap.total ? " (sum OK)" : " (SUM MISMATCH!)",
		  ", contended ",
		  (int)(snap.contended[0] + snap.contended[1] + snap.contended[2]),
		  "\n");
	unlock<UartSem>();
}
} // namespace

int main()
{
	print("\nIPCC + HSEM token ring (A35 core 0, A35 core 1, M33)\n");
	print("=====================================================\n\n");

	park_m33(); // before periph_init resets the mailbox peripherals

	ipcc_init_secure();

	hsem_init();

	lap_test_init();

	// From here on other cores may be printing too, so take the UART semaphore.
	sync_print("A35_0: HSEM + IPCC1 clocked, secured, flags cleared\n");

	// Register before starting the other cores so no hand-off can be missed.
	// The ISR only acks the mailbox and raises a flag -- see ring.hh.
	IPCC1_<1>::enable_all_rxocc_isr_secure(); // our channels are secure -> secure line
	IPCC1_<1>::enable_chan_rxocc_isr<Chan_ToA35>();
	InterruptManager::register_and_start_isr(IPCC1_RX_S_IRQn, 1, 0, [] {
		if (IPCC1_<1>::is_rx_occupied<Chan_ToA35>()) {
			IPCC1_<1>::clear_flag<Chan_ToA35>(); // ack, or it re-fires forever
			token_returned = true;
		}
	});

	start_m33(std::span<const uint8_t>{m33_firmware, m33_firmware_len}, M33_LOAD_ADDR);
	sync_print("A35_0: M33 released from hold-boot (", (int)m33_firmware_len, " bytes in SRAM2)\n");

	auto ret = start_cpu1(aux_core_startup, 0);
	sync_print("A35_0: start A35 core 1 returned ", ret, " (0=success)\n");

	announce_ready(Ready_A35_0);
	uint32_t ready = wait_for_all_cores();
	if (ready == Ready_All)
		sync_print("A35_0: all three cores ready -- starting the ring\n\n");
	else
		sync_print("A35_0: WARNING: only cores ",
				   Hex{ready},
				   " of ",
				   Hex{Ready_All},
				   " reported ready -- starting anyway\n\n");

	start_lap();

	while (true) {
		if (token_returned) {
			token_returned = false;
			finish_lap();
			delay(LapPauseNops); // keep the console readable
			start_lap();
		}
		bump_counter(0);
		// HSEM has no fairness, and the A35s are far faster than the M33 --
		// pause between attempts so the M33 can win the lock too.
		delay(BackoffNops);
	}
}

// --- A35 core 1 -------------------------------------------------------------
extern "C" void aux_main()
{
	sync_print("A35_1: online\n");

	InterruptManager::register_and_start_isr(static_cast<IRQn_Type>(Sgi_TokenToCore1), 2, 0, [] {
		token_at_core1 = true; // SGIs need no ack: the EOI clears them
	});
	announce_ready(Ready_A35_1);

	while (true) {
		if (token_at_core1) {
			token_at_core1 = false;
			stamp_token(Tag_A35_1, 1);
			// Hand off to the M33. Setting our flag raises the M33's
			// RX-occupied interrupt on this channel.

			IPCC1_<1>::set_flag<Chan_ToM33>();
		}
		bump_counter(1);
		delay(BackoffNops); // see the note in main()
	}
}
