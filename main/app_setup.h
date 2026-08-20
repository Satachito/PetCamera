/*
 * app_setup — the on-screen Wi-Fi setup panel.
 *
 * A camera you have to rebuild and reflash to move to another room is not
 * finished. This uses the hardware the Tab5 already has: scan, tap a network,
 * type the password on the touchscreen, and it is saved to NVS.
 */
#pragma once

#include "esp_err.h"
#include "lvgl.h"

/**
 * @brief Create the settings button and return it, so the caller can reposition
 *        it when the screen rotates. Call with the LVGL lock held.
 */
lv_obj_t *app_setup_attach(lv_obj_t *parent);

/** @brief Open the panel without a touch, so the path can be exercised remotely. */
void app_setup_open(void);

/** @brief Resize an open panel after a screen rotation. LVGL lock must be held. */
void app_setup_relayout(void);

/**
 * @brief Report what the panel is actually showing.
 *
 * Reading the on-screen state over HTTP beats inferring it: the log proves the
 * data model was updated, not that the panel in front of you reflects it.
 */
void app_setup_get_state(bool *open, char *hint, size_t hint_len, int *rows);
