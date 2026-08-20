/*
 * app_ui — the Tab5's own screen, used as a local console.
 *
 * A headless camera makes you hunt for its DHCP address. This panel shows the
 * URL, a QR code to open it, and whether anyone is watching.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/** @brief Build the console. bsp_display_start() must have run first. */
esp_err_t app_ui_start(void);

/** @brief Replace the status line, e.g. while Wi-Fi is still connecting. */
void app_ui_set_status(const char *text);

/**
 * @brief Render what is on the panel right now into a JPEG.
 *
 * Useful for seeing the device's own screen without standing in front of it —
 * checking the countdown, the QR code, or what the settings panel is showing.
 *
 * The camera area comes out colour-shifted: the interface and the preview canvas
 * hold RGB565 in different byte orders, and one JPEG cannot honour both. The
 * interface is the part this is for; /snapshot gives the camera's real colour.
 *
 * @param out       Caller's buffer for the JPEG.
 * @param capacity  Its size.
 * @param out_len   Receives the JPEG length.
 */
esp_err_t app_ui_screenshot(uint8_t *out, size_t capacity, size_t *out_len);
