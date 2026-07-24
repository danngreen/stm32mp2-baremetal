#pragma once

// =============================================================================
//  ili9881c.hh -- ER-TFT050-10 (ILI9881C) panel init over DSI
// =============================================================================

// Walk the vendor init table (ili9881c_init.hh) and push every record to the
// panel as a DCS write in LP mode, honouring the embedded delays. Ends with
// sleep-out (0x11) + 120 ms + display-on (0x29). Requires the DSI PLL up and
// the panel already hardware-reset. Send BEFORE starting the video stream.
void ili9881c_send_init();
