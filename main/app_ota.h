/*
 * app_ota — firmware updates over Wi-Fi.
 *
 * The camera lives wherever the pet is, not next to a USB port. Updates are
 * pushed to POST /update and written to the inactive app slot; the bootloader
 * swaps to it on the next boot.
 *
 * Rollback is what makes this safe to use unattended. A newly written image
 * boots as "pending verify" and is only made permanent once the application has
 * proved it works — here, once the camera and the web server are running. A
 * build that crashes or cannot serve is reverted automatically on the next boot
 * rather than leaving a device that has to be fetched and re-cabled.
 */
#pragma once

#include "esp_err.h"
#include "esp_http_server.h"

/** @brief Register /update on @p server. */
esp_err_t app_ota_register(httpd_handle_t server);

/**
 * @brief Confirm the running image so the bootloader stops holding a rollback.
 *
 * Call only once everything that matters is up. Calling it at the top of
 * app_main would confirm images that never manage to do anything.
 */
void app_ota_mark_working(void);

/** @brief Name of the partition the running firmware booted from. */
const char *app_ota_running_partition(void);
