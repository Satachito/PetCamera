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

/**
 * @brief A still, rotated to match what viewers actually see.
 *
 * The stream is the raw sensor image and the browser turns it with CSS, so a
 * saved copy of that JPEG comes out at whatever angle the sensor happens to sit
 * at — right in one device orientation and wrong in the other three. This turns
 * the frame before encoding it, so the file matches the picture.
 *
 * @param degrees   Clockwise rotation to apply, as published in view_rotation.
 * @param out       Receives a borrowed buffer, valid until the next call.
 * @param len       Receives its length.
 * @param timeout_ms How long to wait for the next frame.
 */
esp_err_t app_camera_still(int degrees, const uint8_t **out, size_t *len, int timeout_ms);

/** @brief Release the buffer handed out by app_camera_still(). */
void app_camera_still_release(void);

/**
 * @brief Read the ISP's colour correction matrix.
 *
 * Neutral areas came out magenta — red and blue nearly equal and high with green
 * about a quarter lower — which is a colour-balance problem rather than the
 * channel-order one it resembles.
 *
 * @param matrix Receives nine floats, row-major.
 */
esp_err_t app_camera_get_ccm(float *matrix, bool *enabled);

/** @brief Install a colour correction matrix. */
esp_err_t app_camera_set_ccm(const float *matrix);

/**
 * @brief Read the ISP's white balance gains.
 *
 * These are applied to the raw Bayer data, before anything can clip — which is
 * where an over-bright sky loses its colour, so it is the only place the magenta
 * highlight can actually be fixed.
 */
esp_err_t app_camera_get_wb(float *red, float *blue);

/**
 * @brief Set the white balance gains.
 *
 * The IPA's auto white balance writes these continuously from its own
 * statistics, so a value set here holds only until the next update unless
 * automatic balancing is switched off.
 */
esp_err_t app_camera_set_wb(float red, float blue);

/** @brief Turn the IPA's automatic white balance on or off. */
esp_err_t app_camera_set_awb_auto(bool enable);
