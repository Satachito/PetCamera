#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <errno.h>
#include <unistd.h>
#include <sys/stat.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "driver/ppa.h"
#include "bsp/esp-bsp.h"
#include "esp_vfs_fat.h"
#include "app_audio.h"

#include "app_motion.h"

static const char *TAG = "motion";

/* The PPA cannot shrink by more than 1/16 in a single pass (its scale fraction
 * is 4 bits), so 1280x720 bottoms out at 80x45. 96x54 keeps 16:9 with margin
 * above that floor, and 5184 pixels is still nothing to compare. */
/* 80x45 is exactly 1280x720 divided by 16.
 *
 * The PPA's scale factor is a 4-bit fraction: it shrinks only in sixteenths and
 * TRUNCATES anything in between. Asking for 96x54 (a factor of 0.075) silently
 * got 0.0625, so the hardware wrote just the top-left 80x45 and left the other
 * 16 columns and 9 rows holding whatever was there before. Those stale edges
 * differed on every comparison and kept a motionless scene at 11-13%; the
 * diagnostic map pinned the change to exactly a 4-cell right strip and a 3-cell
 * bottom band, which is 16 and 9 pixels.
 *
 * Matching the real output size also stops ~140 permanently blank cells from
 * diluting the percentage. */
#define THUMB_W 80
#define THUMB_H 45
#define THUMB_PIXELS (THUMB_W * THUMB_H)

/* The thumbnail is averaged down again, in software, into cells of this size.
 *
 * Sensor noise is independent per pixel, so averaging N of them divides its
 * standard deviation by sqrt(N); real movement is spatially coherent and
 * survives untouched. A 4x3 cell averages 12 pixels and so cuts the noise by
 * about 3.5x, which is what separates a cat from a dim room's grain. Measured
 * before this step, a completely still scene scored 13% — above the 8% trigger,
 * so the camera recorded continuously. */
#define CELL_W 4
#define CELL_H 3   /* 80x45 divides evenly into 20x15 cells */
#define CELLS_X (THUMB_W / CELL_W)
#define CELLS_Y (THUMB_H / CELL_H)
#define CELL_COUNT (CELLS_X * CELLS_Y)
#define PPA_OUT_ALIGN 128

/* Files live in a subdirectory, not the card root.
 *
 * A FAT16 root directory is a fixed-size table, and every long filename eats
 * several of its entries — a timestamped clip name costs three. Once it fills,
 * creating any new file fails with EACCES while gigabytes are still free, which
 * is a confusing way to discover the limit. Subdirectories grow on demand. */
#define CLIPS_DIR CONFIG_BSP_SD_MOUNT_POINT "/clips"

/* Per-pixel brightness change that counts as "this pixel moved", on 0-255.
 * A dim room makes the sensor raise its gain, and the extra noise can push a
 * static scene over the percentage threshold on its own — watch motion.score in
 * /status with nothing moving and raise this if it does not settle near zero. */
#define PIXEL_DELTA_THRESHOLD CONFIG_PETCAM_MOTION_PIXEL_DELTA

typedef struct {
    uint8_t *data;
    size_t   len;
} write_job_t;

static struct {
    ppa_client_handle_t ppa;
    uint8_t            *thumb[2];   /* RGB565 thumbnails, ping-pong */
    size_t              thumb_size;
    int                 current;
    bool                have_previous;

    uint8_t            *cells[2];   /* averaged 8-bit brightness per cell */

    QueueHandle_t       writes;     /* JPEGs waiting to reach the card */
    FILE               *clip;
    int64_t             clip_started_us;
    int64_t             last_motion_us;

    uint8_t             diffmap[CELL_COUNT];
    app_motion_stats_t  stats;
} s_mo;

static esp_err_t shrink_frame(const void *rgb565, uint32_t w, uint32_t h, uint8_t *out)
{
    ppa_srm_oper_config_t op = {
        .in = {
            .buffer  = rgb565,
            .pic_w   = w,
            .pic_h   = h,
            .block_w = w,
            .block_h = h,
            .srm_cm  = PPA_SRM_COLOR_MODE_RGB565,
        },
        .out = {
            .buffer      = out,
            .buffer_size = s_mo.thumb_size,
            .pic_w       = THUMB_W,
            .pic_h       = THUMB_H,
            .srm_cm      = PPA_SRM_COLOR_MODE_RGB565,
        },
        .rotation_angle = PPA_SRM_ROTATION_ANGLE_0,
        .scale_x = (float)THUMB_W / (float)w,
        .scale_y = (float)THUMB_H / (float)h,
        .mode = PPA_TRANS_MODE_BLOCKING,
    };

    return ppa_do_scale_rotate_mirror(s_mo.ppa, &op);
}

/* Brightness only, averaged per cell. Colour is deliberately discarded: shifts
 * from auto-white-balance would otherwise read as movement every time the
 * lighting changes slightly. */
static void thumb_to_cells(const uint8_t *rgb565, uint8_t *cells)
{
    const uint16_t *px = (const uint16_t *)rgb565;

    for (int cy = 0; cy < CELLS_Y; cy++) {
        for (int cx = 0; cx < CELLS_X; cx++) {
            uint32_t sum = 0;

            for (int y = 0; y < CELL_H; y++) {
                const uint16_t *row = px + (cy * CELL_H + y) * THUMB_W + cx * CELL_W;
                for (int x = 0; x < CELL_W; x++) {
                    uint16_t v = row[x];
                    uint32_t r = ((v >> 11) & 0x1F) << 3;
                    uint32_t g = ((v >> 5) & 0x3F) << 2;
                    uint32_t b = (v & 0x1F) << 3;
                    sum += (r * 77 + g * 150 + b * 29) >> 8;
                }
            }
            cells[cy * CELLS_X + cx] = (uint8_t)(sum / (CELL_W * CELL_H));
        }
    }
}

/* Compares cells after removing the frame-wide brightness shift.
 *
 * Averaging cells barely moved the score (13% to 12%) on a still scene, which
 * ruled out per-pixel sensor noise: averaging twelve independent samples should
 * have cut that by about 3.5x. What it cannot cut is a change common to every
 * cell — the ISP's auto-exposure and auto-white-balance continually retuning,
 * which lifts or drops the whole frame together. Subtracting the mean shift
 * leaves only what moved relative to the scene. */
static uint8_t compare_cells(const uint8_t *a, const uint8_t *b)
{
    int sum_a = 0, sum_b = 0;
    int shift;
    int changed = 0;

    for (int i = 0; i < CELL_COUNT; i++) {
        sum_a += a[i];
        sum_b += b[i];
    }
    shift = (sum_b - sum_a) / CELL_COUNT;

    for (int i = 0; i < CELL_COUNT; i++) {
        int d = ((int)b[i] - (int)a[i]) - shift;
        if (d < 0) {
            d = -d;
        }
        s_mo.diffmap[i] = (uint8_t)(d > 255 ? 255 : d);
        if (d >= PIXEL_DELTA_THRESHOLD) {
            changed++;
        }
    }
    return (uint8_t)((changed * 100) / CELL_COUNT);
}

/* Finds the oldest clip by modification time, skipping the one being written.
 * Returns false when there is nothing left to remove. */
static bool find_oldest_clip(char *out, size_t out_len, const char *skip)
{
    DIR *dir = opendir(CLIPS_DIR);
    struct dirent *entry;
    time_t oldest = 0;
    bool found = false;

    if (!dir) {
        return false;
    }

    while ((entry = readdir(dir)) != NULL) {
        char path[300];
        struct stat st;

        if (!strstr(entry->d_name, ".mjpeg")) {
            continue;
        }
        if (skip && strcmp(entry->d_name, skip) == 0) {
            continue;
        }
        snprintf(path, sizeof(path), "%s/%s", CLIPS_DIR, entry->d_name);
        if (stat(path, &st) != 0) {
            continue;
        }
        if (!found || st.st_mtime < oldest) {
            oldest = st.st_mtime;
            strlcpy(out, entry->d_name, out_len);
            found = true;
        }
    }

    closedir(dir);
    return found;
}

/* The reserve has to be a share of the card, not a flat number. A 512 MB card
 * against a 512 MB target can never satisfy the condition, and the reclaim loop
 * happily deletes every recording trying. Cap it at a fifth of the volume. */
static uint64_t reserve_bytes(uint64_t total)
{
    uint64_t configured = (uint64_t)CONFIG_PETCAM_CLIP_MIN_FREE_MB * 1024 * 1024;
    uint64_t ceiling = total / 5;

    return configured < ceiling ? configured : ceiling;
}

/* An unattended camera that stops recording when the card fills is worse than
 * one that forgets the oldest footage, so make room before each new clip. */
static void reclaim_space(const char *skip)
{
    uint64_t total = 0, freeb = 0;
    uint64_t want;

    if (esp_vfs_fat_info(CONFIG_BSP_SD_MOUNT_POINT, &total, &freeb) != ESP_OK) {
        return;
    }
    s_mo.stats.free_bytes = freeb;
    want = reserve_bytes(total);

    while (freeb < want) {
        char name[96];
        char path[300];

        if (!find_oldest_clip(name, sizeof(name), skip)) {
            ESP_LOGW(TAG, "only %lluMB free and no clip left to delete",
                     (unsigned long long)(freeb / (1024 * 1024)));
            return;
        }

        snprintf(path, sizeof(path), "%s/%s", CLIPS_DIR, name);
        if (unlink(path) != 0) {
            ESP_LOGE(TAG, "cannot delete %s", name);
            return;
        }
        s_mo.stats.clips_deleted++;
        ESP_LOGI(TAG, "deleted %s to make room", name);

        if (esp_vfs_fat_info(CONFIG_BSP_SD_MOUNT_POINT, &total, &freeb) != ESP_OK) {
            return;
        }
        s_mo.stats.free_bytes = freeb;
    }
}

static void open_clip(void)
{
    char path[64];
    time_t now = time(NULL);
    struct tm tm;

    localtime_r(&now, &tm);
    /* Without NTP the clock starts at the epoch, so fall back to uptime to keep
     * filenames unique and ordered. */
    if (tm.tm_year < 120) {
        snprintf(path, sizeof(path), "%s/clip_%08" PRIu32 ".mjpeg",
                 CLIPS_DIR, (uint32_t)(esp_timer_get_time() / 1000000));
    } else {
        snprintf(path, sizeof(path), "%s/clip_%04d%02d%02d_%02d%02d%02d.mjpeg",
                 CLIPS_DIR, tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                 tm.tm_hour, tm.tm_min, tm.tm_sec);
    }

    reclaim_space(NULL);

    s_mo.clip = fopen(path, "wb");
    if (!s_mo.clip) {
        ESP_LOGE(TAG, "cannot open %s for writing", path);
        return;
    }
    s_mo.clip_started_us = esp_timer_get_time();
    s_mo.stats.recording = true;
    s_mo.stats.events++;
    ESP_LOGI(TAG, "motion — recording %s", path);
}

static void close_clip(void)
{
    if (!s_mo.clip) {
        return;
    }
    fclose(s_mo.clip);
    s_mo.clip = NULL;
    s_mo.stats.recording = false;
    s_mo.stats.clips_written++;
    ESP_LOGI(TAG, "clip closed (%" PRIu32 " total)", s_mo.stats.clips_written);
}

/* Writing to the card blocks for tens of milliseconds at a time. Doing that
 * inline in the capture task dragged capture from 14 fps down to under 6, so the
 * frame is copied to a queue and a lower-priority task does the I/O. */
static void writer_task(void *arg)
{
    write_job_t job;

    for (;;) {
        if (xQueueReceive(s_mo.writes, &job, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (s_mo.clip && fwrite(job.data, 1, job.len, s_mo.clip) != job.len) {
            ESP_LOGE(TAG, "write failed — card full or removed; stopping this clip");
            close_clip();
        }
        free(job.data);
    }
}

esp_err_t app_motion_start(void)
{
    const ppa_client_config_t ppa_cfg = {
        .oper_type = PPA_OPERATION_SRM,
        .max_pending_trans_num = 1,
    };
    size_t size = (THUMB_PIXELS * 2 + PPA_OUT_ALIGN - 1) & ~((size_t)PPA_OUT_ALIGN - 1);
    esp_err_t err;

    err = bsp_sdcard_mount();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "no SD card (%s); motion detection stays off",
                 esp_err_to_name(err));
        return err;
    }

    ESP_RETURN_ON_ERROR(ppa_register_client(&ppa_cfg, &s_mo.ppa), TAG,
                        "cannot claim a PPA client");

    for (int i = 0; i < 2; i++) {
        s_mo.thumb[i] = heap_caps_aligned_alloc(PPA_OUT_ALIGN, size,
                                                MALLOC_CAP_SPIRAM | MALLOC_CAP_CACHE_ALIGNED);
        if (s_mo.thumb[i]) {
            /* Belt and braces: if a future size ever leaves a border unwritten,
             * at least it is a stable one rather than the previous frame. */
            memset(s_mo.thumb[i], 0, size);
        }
        s_mo.cells[i] = calloc(1, CELL_COUNT);
        if (!s_mo.thumb[i] || !s_mo.cells[i]) {
            return ESP_ERR_NO_MEM;
        }
    }

    if (mkdir(CLIPS_DIR, 0777) != 0 && errno != EEXIST) {
        ESP_LOGE(TAG, "cannot create %s: errno %d (%s). A FAT16 root directory "
                 "holds a fixed number of entries and long filenames use several "
                 "each; delete the clips still in the card root to free it.",
                 CLIPS_DIR, errno, strerror(errno));
    }

    s_mo.writes = xQueueCreate(CONFIG_PETCAM_CLIP_FPS * 2, sizeof(write_job_t));
    if (!s_mo.writes) {
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreate(writer_task, "petcam_sdwrite", 4096, NULL, 3, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    s_mo.thumb_size = size;
    s_mo.stats.armed = true;

    {
        uint64_t total = 0, freeb = 0;
        if (esp_vfs_fat_info(CONFIG_BSP_SD_MOUNT_POINT, &total, &freeb) == ESP_OK) {
            s_mo.stats.free_bytes = freeb;
            ESP_LOGI(TAG, "card: %lluMB free of %lluMB, keeping %lluMB clear",
                     (unsigned long long)(freeb / (1024 * 1024)),
                     (unsigned long long)(total / (1024 * 1024)),
                     (unsigned long long)(reserve_bytes(total) / (1024 * 1024)));
        }
    }
    ESP_LOGI(TAG, "armed: %dx%d thumbnail averaged to %dx%d cells, %d%% of cells "
             "must change by %d levels, clips go to %s",
             THUMB_W, THUMB_H, CELLS_X, CELLS_Y, CONFIG_PETCAM_MOTION_SENSITIVITY,
             PIXEL_DELTA_THRESHOLD, CLIPS_DIR);
    return ESP_OK;
}

void app_motion_submit_frame(const void *rgb565, uint32_t width, uint32_t height)
{
    static int64_t last_check_us;
    int64_t now = esp_timer_get_time();

    if (!s_mo.stats.armed) {
        return;
    }
    /* One comparison a second is plenty; a pet does not cross a room in 60 ms,
     * and this keeps the capture task free for encoding. */
    if (now - last_check_us < 1000000) {
        return;
    }
    last_check_us = now;

    int next = 1 - s_mo.current;
    esp_err_t err = shrink_frame(rgb565, width, height, s_mo.thumb[next]);
    if (err != ESP_OK) {
        static bool complained;
        if (!complained) {
            complained = true;
            ESP_LOGE(TAG, "thumbnail scaling failed (%s); detection is inactive",
                     esp_err_to_name(err));
        }
        return;
    }
    thumb_to_cells(s_mo.thumb[next], s_mo.cells[next]);

    if (s_mo.have_previous) {
        uint8_t score = compare_cells(s_mo.cells[s_mo.current], s_mo.cells[next]);
        s_mo.stats.last_score = score;

        if (score >= CONFIG_PETCAM_MOTION_SENSITIVITY) {
            s_mo.last_motion_us = now;
            if (!s_mo.clip) {
                open_clip();
            }
        } else if (s_mo.clip &&
                   now - s_mo.last_motion_us > CONFIG_PETCAM_MOTION_LINGER_S * 1000000LL) {
            close_clip();
        }
    }

    s_mo.current = next;
    s_mo.have_previous = true;
}

void app_motion_submit_jpeg(const uint8_t *jpeg, size_t len)
{
    static int64_t last_write_us;
    write_job_t job;
    int64_t now;

    if (!s_mo.clip || !s_mo.writes) {
        return;
    }

    /* Recording every encoded frame filled 37 MB a minute and kept the card
     * busy at 1.3 MB/s. A clip only has to show what happened. */
    now = esp_timer_get_time();
    if (now - last_write_us < 1000000 / CONFIG_PETCAM_CLIP_FPS) {
        return;
    }
    last_write_us = now;

    if (now - s_mo.clip_started_us >
        CONFIG_PETCAM_MOTION_MAX_CLIP_S * 1000000LL) {
        /* Cap the length so a curtain moving in a draught cannot fill the card
         * with one enormous file. Continued motion opens a fresh clip. */
        close_clip();
        return;
    }

    /* The capture buffer is reused immediately, so the queue needs its own copy. */
    job.data = malloc(len);
    if (!job.data) {
        return;
    }
    memcpy(job.data, jpeg, len);
    job.len = len;

    if (xQueueSend(s_mo.writes, &job, 0) != pdTRUE) {
        /* Card cannot keep up. Dropping a frame is better than stalling capture
         * or growing the queue without bound. */
        free(job.data);
        s_mo.stats.frames_dropped++;
    }
}

void app_motion_get_stats(app_motion_stats_t *out)
{
    *out = s_mo.stats;
}

int app_motion_get_diffmap(uint8_t *out, int max, int *w, int *h)
{
    int n = CELL_COUNT < max ? CELL_COUNT : max;

    *w = CELLS_X;
    *h = CELLS_Y;
    memcpy(out, s_mo.diffmap, n);
    return n;
}

static int delete_in(const char *directory, const char *name)
{
    DIR *dir = opendir(directory);
    struct dirent *entry;
    int removed = 0;

    if (!dir) {
        return 0;
    }
    while ((entry = readdir(dir)) != NULL) {
        char path[300];

        if (!strstr(entry->d_name, ".mjpeg")) {
            continue;
        }
        if (name && strcmp(entry->d_name, name) != 0) {
            continue;
        }
        snprintf(path, sizeof(path), "%s/%s", directory, entry->d_name);
        if (unlink(path) == 0) {
            removed++;
        } else {
            ESP_LOGE(TAG, "cannot delete %s", entry->d_name);
        }
    }
    closedir(dir);
    return removed;
}

int app_motion_delete(const char *name)
{
    int removed = 0;

    if (!s_mo.stats.armed) {
        return -1;
    }

    /* Whatever is being recorded has to stop first: unlinking an open file
     * leaves the writer task appending to a vanished directory entry. */
    if (s_mo.clip) {
        close_clip();
    }

    removed += delete_in(CLIPS_DIR, name);
    /* Clips written before the move to a subdirectory still occupy root
     * entries, and a full root is what stops new directories being created at
     * all. */
    removed += delete_in(CONFIG_BSP_SD_MOUNT_POINT, name);

    if (removed && mkdir(CLIPS_DIR, 0777) == 0) {
        ESP_LOGI(TAG, "root freed up; %s created", CLIPS_DIR);
    }

    if (removed) {
        uint64_t total = 0, freeb = 0;
        if (esp_vfs_fat_info(CONFIG_BSP_SD_MOUNT_POINT, &total, &freeb) == ESP_OK) {
            s_mo.stats.free_bytes = freeb;
        }
        ESP_LOGI(TAG, "deleted %d clip(s) on request", removed);
    }
    return removed;
}
