/*
 * app_camera — owns /dev/video0, encodes each captured frame to JPEG with the
 * ESP32-P4's hardware encoder, and publishes it on the frame_bus.
 */
#pragma once

#include <stdint.h>
#include "esp_err.h"

typedef struct {
    uint32_t width;
    uint32_t height;
    uint32_t frames;        /* frames published since boot */
    uint32_t dropped;       /* frames dropped: pool busy, or over the fps cap */
    float    fps;           /* publish rate over the last second */
    uint32_t last_jpeg_len; /* size of the most recent JPEG, bytes */
} app_camera_stats_t;

/**
 * @brief Start capturing.
 *
 * bsp_camera_start() must have run first — that is what powers the sensor and
 * makes esp_video create the video device.
 */
esp_err_t app_camera_start(void);

/** @brief Snapshot of the capture statistics. */
void app_camera_get_stats(app_camera_stats_t *out);
