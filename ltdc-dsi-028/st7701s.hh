#pragma once

// =============================================================================
//  st7701s.hh -- ER-TFT028-2 (ST7701S) panel init over DSI
// =============================================================================

// Walk the vendor init table (st7701s_init.hh) and push every record to the
// panel as a DCS write in LP mode, honouring the embedded delays. Ends with
// sleep-out (0x11) + 120 ms, COLMOD/MADCTL/TE, then display-on (0x29). Requires
// the DSI PLL up and the panel already hardware-reset. Send BEFORE starting the
// video stream.
void st7701s_send_init();
