#include "st7701s.hh"
#include "drivers/hal_cnt.hh" // udelay
#include "dsi.hh"
#include "st7701s_init.hh"
#include <cstddef>

// The init table is a flat stream of records produced from the vendor's
// reference/ER-TFT028-2_Initialization Program.txt (see st7701s_init.hh for
// the encoding):
//   [len][len bytes...]        -> one DCS write of those bytes
//   [DELAY][ms]                -> busy-wait ms milliseconds
// 1-byte records are parameterless DCS commands (sleep-out, display-on), 2-byte
// records are register writes (reg + value), longer ones are multi-parameter
// writes; the 6-byte 0xFF records switch the ST7701S Command2 bank.
void st7701s_send_init()
{
	using namespace ST7701S;
	const uint8_t *p = init_seq;
	const uint8_t *const end = init_seq + sizeof(init_seq);
	while (p < end) {
		uint8_t len = *p++;
		if (len == DELAY) {
			udelay(1000u * static_cast<uint32_t>(*p++));
			continue;
		}
		dsi_dcs_write(p, len);
		p += len;
	}
}
