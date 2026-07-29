// Cortex-A35 firmware that embeds, loads, and starts the
// Cortex-M33 coprocessor, then runs in parallel with it.
//
// Both cores share the console UART and print occasionally; the A35 toggles
// PH8 and the M33 toggles PF9.

#include "aarch64/system_reg.hh"
#include "drivers/copro_m33.hh"
#include "drivers/pin.hh"
#include "print/print.hh"
#include "stm32mp2xx.h"
#include <cstdint>

#include "firmware_m33.h"

namespace
{
constexpr uintptr_t M33_LOAD_ADDR = 0x0E060000UL;

void delay(unsigned n);
void report_m33_boot_state();

} // namespace

int main()
{
	print("\nA35: Hello from Cortex-A35!\n");

	Pin ph8{GPIO::H, PinNum::_8, PinMode::Output};

	print("A35: Loading M33 firmware (", (int)m33_firmware_len, " bytes) into SRAM2\n");

	print("A35: Starting M33 core after LED goes off in 3 ");
	ph8.on();
	delay(8'000'000);
	print("2 ");
	delay(8'000'000);
	print("1 ");
	ph8.off();
	delay(8'000'000);
	print("now\n");

	start_m33({m33_firmware, m33_firmware_len}, M33_LOAD_ADDR);
	print("A35: M33 released from hold-boot\n");

	report_m33_boot_state();

	unsigned count = 0;
	while (true) {
		ph8.on();
		delay(3'000'000);
		ph8.off();
		delay(3'000'000);

		if ((++count % 4) == 0)
			print("A35: tick ", (int)count, "\n");
	}
}

namespace
{
void report_m33_boot_state()
{
	print("A35: --- M33 boot diagnostics ---\n");
	print("A35: M33_ACCESS_CR     = ", Hex{CA35SYSCFG->M33_ACCESS_CR}, " (reset: 0x3 = secure+priv only)\n");
	print("A35: M33_TZEN_CR       = ", Hex{CA35SYSCFG->M33_TZEN_CR}, " (want 1 = M33 TrustZone/secure enabled)\n");
	print(
		"A35: M33_INITSVTOR_CR  = ", Hex{CA35SYSCFG->M33_INITSVTOR_CR}, " (want ", Hex{(unsigned)M33_LOAD_ADDR}, ")\n");
	print("A35: RCC CPUBOOTCR     = ", Hex{RCC->CPUBOOTCR}, " (want bit0 BOOT_CPU2 = 1)\n");
	print("A35: RCC C2RSTCSETR    = ", Hex{RCC->C2RSTCSETR}, " (want 0 = CPU2 reset released)\n");

	// PWR->CPU2D2SR: bit0 HOLD_BOOT (1 = still held), bits[3:2] CSTATE
	// (0 = in reset, 1 = CRun), bits[10:8] DSTATE (0 = Run)
	print("A35: PWR CPU2D2SR      = ", Hex{PWR->CPU2D2SR}, " (bit0 HOLD_BOOT, CSTATE[3:2]=01 CRun)\n");

	// Breadcrumbs: M33 startup writes 0xB007C0DE into vector slot 8 as its
	// first instructions, and 0xB007C0DF into slot 9 right before main().
	// Poll for a while, invalidating our dcache so we see the M33's writes.
	auto crumb0 = reinterpret_cast<uint32_t *>(M33_LOAD_ADDR + 0x20);
	auto crumb1 = reinterpret_cast<uint32_t *>(M33_LOAD_ADDR + 0x24);
	invalidate_dcache_address(M33_LOAD_ADDR + 0x20);
	dsb_sy();
	print("A35: M33 breadcrumb[0] = ", Hex{*crumb0}, " (0xB007C0DE = M33 reset handler ran)\n");
	print("A35: M33 breadcrumb[1] = ", Hex{*crumb1}, " (0xB007C0DF = M33 reached main)\n");
	print("A35: PWR CPU2D2SR      = ", Hex{PWR->CPU2D2SR}, " (after wait)\n");
	print("A35: -------------------------\n");
}

void delay(unsigned n)
{
	for (unsigned i = 0; i < n; i++)
		asm("nop");
}
} // namespace
