/*
 * app_preview — shows the live camera image on the Tab5's own panel.
 *
 * The frame arrives as RGB565 from the capture task and has to be rotated: the
 * sensor is mounted at BSP_CAMERA_ROTATION (270 degrees) relative to the screen.
 * The ESP32-P4's PPA does the rotate and scale in hardware, so the capture task
 * pays almost nothing for it.
 *
 * Two output buffers are used. The PPA writes into the back buffer while LVGL
 * is still reading the front one; they are swapped under the LVGL lock. Sharing
 * a single buffer would tear on every frame.
 */
#pragma once

#include <stdint.h>
#include "esp_err.h"
#include "lvgl.h"

/**
 * @brief Create the preview canvas and claim a PPA client.
 *
 * @param parent    LVGL parent object.
 * @param width     Canvas width in pixels.
 * @param height    Canvas height in pixels.
 * @param out_canvas Receives the canvas object so the caller can position it.
 */
esp_err_t app_preview_init(lv_obj_t *parent, uint32_t width, uint32_t height,
                           lv_obj_t **out_canvas);

/**
 * @brief Hand a captured RGB565 frame to the preview. Safe to call from the
 *        capture task; returns immediately if the previous frame is still
 *        waiting to be shown.
 */
void app_preview_submit(const void *rgb565, uint32_t src_w, uint32_t src_h);

/**
 * @brief React to a screen rotation. Call with the LVGL lock held.
 *
 * The PPA angle becomes (mount correction - screen rotation), which keeps the
 * image fixed relative to the panel while LVGL turns the rest of the UI. The
 * output dimensions always land exactly on the rotated logical resolution, so
 * the image fills the screen in every orientation.
 */
void app_preview_set_rotation(int screen_rotation_degrees);

/** @brief Swap in the newest frame. Call with the LVGL lock held. */
void app_preview_refresh(void);

/**
 * @brief Show or hide the live image.
 *
 * The canvas is a full-screen 720x1280 image and by far the most expensive
 * thing LVGL draws. Taking it out of the render tree while a modal panel covers
 * it removes that cost entirely — LVGL's built-in allocator only has 64 KB to
 * work with, and it spins in lv_draw_dispatch rather than failing when a draw
 * task cannot be served.
 *
 * Call with the LVGL lock held.
 */
void app_preview_set_visible(bool visible);

/** @brief True if a preview canvas exists. */
bool app_preview_enabled(void);
