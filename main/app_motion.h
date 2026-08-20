/*
 * app_motion — motion detection and clip recording to the microSD card.
 *
 * Detection works on the raw RGB565 frame the capture task already holds, not on
 * the encoded JPEG: the PPA shrinks each frame to a thumbnail in hardware and
 * successive thumbnails are differenced. That avoids a JPEG decode per frame
 * entirely, and the arithmetic on a 40x22 image is negligible.
 *
 * Clips are written as concatenated JPEGs (.mjpeg), which ffmpeg and VLC play
 * directly and which can be appended to without rewriting a container.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

typedef struct {
    bool     armed;           /* SD card mounted and detection running */
    bool     recording;
    uint32_t events;          /* motion events since boot */
    uint32_t clips_written;
    uint32_t clips_deleted;   /* removed to make room */
    uint64_t free_bytes;      /* space left on the card */
    uint32_t frames_dropped;  /* card could not keep up */
    uint8_t  last_score;      /* 0-100, how much the last frame changed */
} app_motion_stats_t;

/** @brief Mount the SD card and start detection. Safe to fail: returns an error
 *         and the rest of the camera carries on without recording. */
esp_err_t app_motion_start(void);

/** @brief Feed a captured RGB565 frame. Called from the capture task. */
void app_motion_submit_frame(const void *rgb565, uint32_t width, uint32_t height);

/** @brief Offer the encoded JPEG for the current clip, if one is open. */
void app_motion_submit_jpeg(const uint8_t *jpeg, size_t len);

void app_motion_get_stats(app_motion_stats_t *out);

/**
 * @brief Copy the most recent per-cell difference map.
 *
 * Two plausible explanations for a high score on a still scene — sensor noise
 * and global exposure drift — both failed to explain the measurements, so this
 * exposes where the change actually is. Scattered means noise; concentrated
 * means something in the scene really is moving.
 *
 * @param out  Receives one absolute delta per cell, row-major.
 * @param max  Capacity of @p out.
 * @param w    Receives the grid width.
 * @param h    Receives the grid height.
 * @return Number of cells written.
 */
int app_motion_get_diffmap(uint8_t *out, int max, int *w, int *h);

/**
 * @brief Delete recordings.
 *
 * @param name  One clip to remove, or NULL for every clip.
 * @return Number of files deleted, or -1 if the card is not available.
 *
 * A clip being written is closed first — unlinking an open file leaves the
 * writer appending to something that no longer has a directory entry.
 */
int app_motion_delete(const char *name);

/**
 * @brief Reformat the card and recreate the directories.
 *
 * Everything on the card is lost. Needed when the filesystem reports free space
 * it cannot actually allocate — writes then fail with ENOSPC while df looks
 * healthy, and no amount of deleting fixes it.
 */
esp_err_t app_motion_format_card(void);
