/*
 * app_wifi — joins the configured network through the on-board ESP32-C6.
 *
 * The ESP32-P4 has no radio. esp_wifi_remote forwards the normal esp_wifi_*
 * calls to the C6 over SDIO (ESP-Hosted), so nothing here looks unusual — but
 * the C6 has to be powered up via the IO expander first, and the SDIO pin
 * mapping in sdkconfig.defaults is Tab5-specific.
 */
#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "esp_wifi.h"

/** @brief How many networks can be remembered at once. */
#define APP_WIFI_MAX_SAVED 8

/**
 * @brief Power up the C6, connect, and start mDNS.
 *
 * @param timeout_ms How long to wait for an IP address before returning.
 *                   The connection keeps retrying in the background either way.
 */
esp_err_t app_wifi_start(int timeout_ms);

/** @brief True once a DHCP lease is held. */
bool app_wifi_is_connected(void);

/** @brief Current IPv4 address as a string, or "0.0.0.0" while disconnected. */
const char *app_wifi_ip(void);

/** @brief RSSI of the current AP in dBm, or 0 when disconnected. */
int app_wifi_rssi(void);

/** @brief SSID currently configured (from NVS, or the build-time default). */
const char *app_wifi_ssid(void);

/**
 * @brief Store new credentials in NVS and reconnect with them.
 *
 * Survives a reboot, so a Tab5 moved to a different room does not need a rebuild
 * or a USB cable — only the on-screen setup panel.
 */
esp_err_t app_wifi_set_credentials(const char *ssid, const char *password);

/** @brief Forget one saved network, or every one when @p ssid is NULL. */
esp_err_t app_wifi_forget(const char *ssid);

/** @brief Number of remembered networks. */
int app_wifi_saved_count(void);

/** @brief SSID of remembered network @p index, or NULL. */
const char *app_wifi_saved_ssid(int index);

/** @brief True if @p ssid is remembered. */
bool app_wifi_is_saved(const char *ssid);

/**
 * @brief Rescan and join whichever remembered network has the strongest signal.
 *
 * Runs in the background; returns immediately.
 */
void app_wifi_reselect(void);

/**
 * @brief The stored password for @p ssid, or NULL if that network is not saved.
 *
 * For the on-screen panel only — never return this over the network.
 */
const char *app_wifi_saved_password(const char *ssid);

/**
 * @brief Scan for 2.4 GHz networks.
 *
 * @param out    Caller-provided array filled with the strongest results.
 * @param max    Capacity of @p out.
 * @param found  Receives the number written.
 */
esp_err_t app_wifi_scan(wifi_ap_record_t *out, uint16_t max, uint16_t *found);
