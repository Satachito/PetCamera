/*
 * app_orientation — turns the BMI270 accelerometer into a screen rotation.
 *
 * The camera and the panel are bolted to the same body, so their alignment never
 * changes; only the reader moves. That means the UI chrome should rotate with
 * the device while the live image keeps a fixed relationship to the screen —
 * exactly how a phone camera app behaves.
 */
#pragma once

#include <stdbool.h>
#include "esp_err.h"

/** @brief Screen rotation in degrees, counter-clockwise: 0, 90, 180 or 270. */
typedef void (*app_orientation_cb_t)(int rotation_degrees);

/**
 * @brief Start reading the IMU and report settled orientation changes.
 *
 * The callback runs on the sensor task, so it must take the LVGL lock itself.
 */
esp_err_t app_orientation_start(app_orientation_cb_t on_change);

/** @brief Most recent settled rotation in degrees. */
int app_orientation_get(void);

/**
 * @brief Latest raw accelerometer reading, in G.
 *
 * Exposed so the mounting convention can be calibrated against a real pose
 * rather than guessed: hold the device the intended way and read the values.
 */
void app_orientation_get_accel(float *x, float *y, float *z, int *raw_quadrant);
