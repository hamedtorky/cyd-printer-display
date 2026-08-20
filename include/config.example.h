#pragma once

// Moonraker normally listens on port 7125.
#define MOONRAKER_HOST "printer.lan"
#define MOONRAKER_PORT 7125

// Leave empty when Moonraker trusts devices on the local network.
#define MOONRAKER_API_KEY ""

// Optional AI gateway running on the printer computer. The ESP32 never stores
// an OpenAI API key; it only talks to this local service.
#define AI_GATEWAY_HOST "printer.lan"
#define AI_GATEWAY_PORT 8787
