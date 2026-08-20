/*
 * app_httpd — serves the viewer page, single snapshots, a JSON status endpoint
 * and the MJPEG stream.
 *
 * Two server instances are started. esp_http_server handles one request at a
 * time per instance, and an MJPEG response never completes, so a stream sharing
 * the main port would block the page from ever loading. The stream therefore
 * gets its own port.
 */
#pragma once

#include "esp_err.h"

esp_err_t app_httpd_start(void);

/** @brief Number of viewers currently receiving the MJPEG stream. */
int app_httpd_client_count(void);
