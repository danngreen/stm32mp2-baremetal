#include "ili9881c.hh"
#include "dsi.hh"
#include "drivers/hal_cnt.hh" // udelay
#include "ili9881c_init.hh"
#include <cstddef>

// The init table is a flat stream of records produced from the vendor's
// reference/ILI9881C_Initial.h (see ili9881c_init.hh for the encoding):
//   [len][len bytes...]        -> one DCS write of those bytes
//   [DELAY][ms]                -> busy-wait ms milliseconds
// 2-byte records are register writes (reg + value); the 4-byte 0xFF records
// switch the ILI9881C command page.
void ili9881c_send_init()
{
	using namespace ILI9881C;
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
