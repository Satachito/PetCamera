/*
 * app_ui — the Tab5's own screen, used as a local console.
 *
 * A headless camera makes you hunt for its DHCP address. This panel shows the
 * URL, a QR code to open it, and whether anyone is watching.
 */
#pragma once

#include "esp_err.h"

/** @brief Build the console. bsp_display_start() must have run first. */
esp_err_t app_ui_start(void);

/** @brief Replace the status line, e.g. while Wi-Fi is still connecting. */
void app_ui_set_status(const char *text);
