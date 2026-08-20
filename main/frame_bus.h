/*
 * frame_bus — a tiny hand-off between the one camera task that produces JPEG
 * frames and the HTTP handlers that consume them.
 *
 * Frames live in a fixed pool and are reference counted, so a slow HTTP client
 * can keep reading an older frame while the camera has already moved on. No
 * copies are made: a consumer borrows the producer's buffer until it releases
 * it. If every buffer is busy the producer drops the frame rather than stall
 * the capture pipeline.
 */
#pragma once

#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

typedef struct frame_s frame_t;

struct frame_s {
    uint8_t *data;      /* JPEG bytes */
    size_t   capacity;  /* allocated size */
    size_t   len;       /* bytes actually used */
    uint32_t seq;       /* monotonic frame number */
    int      refs;      /* internal */
};

/**
 * @brief Allocate the frame pool.
 *
 * Buffers come from jpeg_alloc_encoder_mem() so the hardware encoder can write
 * into them directly.
 *
 * @param count     Number of frames in the pool. Must exceed the number of
 *                  simultaneous readers, or the producer starts dropping.
 * @param capacity  Bytes per frame. Sized for the worst-case JPEG.
 */
esp_err_t frame_bus_init(int count, size_t capacity);

/**
 * @brief Claim a free frame to encode into, or NULL if all are in use.
 *
 * The returned frame carries the producer's reference; hand it to
 * frame_bus_publish() or frame_bus_release() — never drop it on the floor.
 */
frame_t *frame_bus_acquire_writable(void);

/** @brief Publish a written frame as the newest one and wake up readers. */
void frame_bus_publish(frame_t *f, size_t len);

/**
 * @brief Borrow the newest frame, waiting until it is newer than *last_seq.
 *
 * @param last_seq   In: the seq the caller already sent. Out: the seq returned.
 *                   Pass 0 on the first call to get whatever is current.
 * @param timeout_ms How long to wait for a newer frame.
 * @return Borrowed frame (release it with frame_bus_release), or NULL on timeout.
 */
frame_t *frame_bus_acquire_latest(uint32_t *last_seq, int timeout_ms);

/** @brief Give back a frame obtained from acquire_latest/acquire_writable. */
void frame_bus_release(frame_t *f);

/** @brief Frames published since boot. */
uint32_t frame_bus_seq(void);

/** @brief Frames dropped because the pool was fully borrowed. */
uint32_t frame_bus_dropped(void);
