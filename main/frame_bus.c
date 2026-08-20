#include <string.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "driver/jpeg_encode.h"
#include "esp_log.h"

#include "frame_bus.h"

static const char *TAG = "frame_bus";

/* Readers poll for a newer sequence number rather than waiting on a condition
 * variable. FreeRTOS has no broadcast primitive, and a 4 ms poll costs far less
 * than the bookkeeping needed to wake N readers correctly. */
#define POLL_INTERVAL_MS 4

static struct {
    SemaphoreHandle_t lock;
    frame_t          *pool;
    int               count;
    frame_t          *latest;
    uint32_t          seq;
    uint32_t          dropped;
} s_bus;

esp_err_t frame_bus_init(int count, size_t capacity)
{
    if (count < 2 || capacity == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    s_bus.lock = xSemaphoreCreateMutex();
    if (!s_bus.lock) {
        return ESP_ERR_NO_MEM;
    }

    s_bus.pool = calloc(count, sizeof(frame_t));
    if (!s_bus.pool) {
        return ESP_ERR_NO_MEM;
    }

    const jpeg_encode_memory_alloc_cfg_t mem_cfg = {
        .buffer_direction = JPEG_ENC_ALLOC_OUTPUT_BUFFER,
    };

    for (int i = 0; i < count; i++) {
        size_t allocated = 0;
        s_bus.pool[i].data = jpeg_alloc_encoder_mem(capacity, &mem_cfg, &allocated);
        if (!s_bus.pool[i].data) {
            ESP_LOGE(TAG, "out of memory allocating frame %d of %d (%u bytes)",
                     i, count, (unsigned)capacity);
            return ESP_ERR_NO_MEM;
        }
        s_bus.pool[i].capacity = allocated;
    }

    s_bus.count = count;
    ESP_LOGI(TAG, "pool ready: %d frames x %u bytes", count, (unsigned)capacity);
    return ESP_OK;
}

frame_t *frame_bus_acquire_writable(void)
{
    frame_t *found = NULL;

    xSemaphoreTake(s_bus.lock, portMAX_DELAY);
    for (int i = 0; i < s_bus.count; i++) {
        if (s_bus.pool[i].refs == 0) {
            found = &s_bus.pool[i];
            found->refs = 1;
            break;
        }
    }
    if (!found) {
        s_bus.dropped++;
    }
    xSemaphoreGive(s_bus.lock);

    return found;
}

void frame_bus_publish(frame_t *f, size_t len)
{
    frame_t *previous;

    xSemaphoreTake(s_bus.lock, portMAX_DELAY);
    f->len = len;
    f->seq = ++s_bus.seq;
    /* The producer's reference becomes the reference held by 'latest'. */
    previous = s_bus.latest;
    s_bus.latest = f;
    xSemaphoreGive(s_bus.lock);

    if (previous) {
        frame_bus_release(previous);
    }
}

frame_t *frame_bus_acquire_latest(uint32_t *last_seq, int timeout_ms)
{
    int waited_ms = 0;

    for (;;) {
        frame_t *f = NULL;

        xSemaphoreTake(s_bus.lock, portMAX_DELAY);
        if (s_bus.latest && s_bus.latest->seq != *last_seq) {
            f = s_bus.latest;
            f->refs++;
            *last_seq = f->seq;
        }
        xSemaphoreGive(s_bus.lock);

        if (f) {
            return f;
        }
        if (waited_ms >= timeout_ms) {
            return NULL;
        }
        vTaskDelay(pdMS_TO_TICKS(POLL_INTERVAL_MS));
        waited_ms += POLL_INTERVAL_MS;
    }
}

void frame_bus_release(frame_t *f)
{
    if (!f) {
        return;
    }
    xSemaphoreTake(s_bus.lock, portMAX_DELAY);
    if (f->refs > 0) {
        f->refs--;
    }
    xSemaphoreGive(s_bus.lock);
}

uint32_t frame_bus_seq(void)
{
    return s_bus.seq;
}

uint32_t frame_bus_dropped(void)
{
    return s_bus.dropped;
}
