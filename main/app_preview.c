#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "driver/ppa.h"

#include "app_preview.h"

static const char *TAG = "preview";

static inline const char *preview_tag(void) { return TAG; }

/* PSRAM output for the PPA has to satisfy both cache levels. */
#define PPA_OUT_ALIGN 128

#define MIN_INTERVAL_US (1000000 / CONFIG_PETCAM_PREVIEW_FPS)

static struct {
    ppa_client_handle_t ppa;
    lv_obj_t           *canvas;
    uint8_t            *buf[2];
    size_t              buf_size;
    int                 back;        /* index the PPA writes into */
    volatile bool       pending;     /* back buffer holds an unshown frame */
    SemaphoreHandle_t   lock;
    uint32_t            width;
    uint32_t            height;
    int                 ppa_angle;      /* degrees applied to the camera frame */
    bool                hidden;
    int64_t             last_us;
} s_pv;

static ppa_srm_rotation_angle_t angle_enum(int degrees)
{
    switch (((degrees % 360) + 360) % 360) {
    case 90:  return PPA_SRM_ROTATION_ANGLE_90;
    case 180: return PPA_SRM_ROTATION_ANGLE_180;
    case 270: return PPA_SRM_ROTATION_ANGLE_270;
    default:  return PPA_SRM_ROTATION_ANGLE_0;
    }
}

esp_err_t app_preview_init(lv_obj_t *parent, uint32_t width, uint32_t height,
                           lv_obj_t **out_canvas)
{
    const ppa_client_config_t ppa_cfg = {
        .oper_type = PPA_OPERATION_SRM,
        .max_pending_trans_num = 1,
    };
    size_t size = (size_t)width * height * 2;

    size = (size + PPA_OUT_ALIGN - 1) & ~((size_t)PPA_OUT_ALIGN - 1);

    ESP_RETURN_ON_ERROR(ppa_register_client(&ppa_cfg, &s_pv.ppa), TAG,
                        "cannot claim a PPA client");

    for (int i = 0; i < 2; i++) {
        s_pv.buf[i] = heap_caps_aligned_alloc(PPA_OUT_ALIGN, size,
                                              MALLOC_CAP_SPIRAM | MALLOC_CAP_CACHE_ALIGNED);
        if (!s_pv.buf[i]) {
            ESP_LOGE(TAG, "out of memory for preview buffer %d (%u bytes)", i, (unsigned)size);
            return ESP_ERR_NO_MEM;
        }
        memset(s_pv.buf[i], 0, size);
    }

    s_pv.lock = xSemaphoreCreateMutex();
    if (!s_pv.lock) {
        return ESP_ERR_NO_MEM;
    }

    s_pv.buf_size = size;
    s_pv.width  = width;
    s_pv.height = height;
    s_pv.back   = 0;
    s_pv.ppa_angle = CONFIG_PETCAM_CAMERA_MOUNT_ROTATION;

    s_pv.canvas = lv_canvas_create(parent);
    lv_canvas_set_buffer(s_pv.canvas, s_pv.buf[1], width, height, LV_COLOR_FORMAT_RGB565);
    lv_obj_set_size(s_pv.canvas, width, height);

    if (out_canvas) {
        *out_canvas = s_pv.canvas;
    }

    ESP_LOGI(TAG, "preview %ux%u, 2 x %u KB, up to %d fps",
             (unsigned)width, (unsigned)height, (unsigned)(size / 1024),
             CONFIG_PETCAM_PREVIEW_FPS);
    return ESP_OK;
}

void app_preview_submit(const void *rgb565, uint32_t src_w, uint32_t src_h)
{
    if (!s_pv.ppa || s_pv.pending || s_pv.hidden) {
        /* Nothing to draw into, or the last frame has not been shown yet.
         * Dropping is correct — a preview wants the newest frame, and the
         * capture task must not wait on the display. */
        return;
    }

    int64_t now = esp_timer_get_time();
    if (now - s_pv.last_us < MIN_INTERVAL_US) {
        return;
    }
    s_pv.last_us = now;

    int a = ((s_pv.ppa_angle % 360) + 360) % 360;
    bool swapped = (a == 90 || a == 270);

    ppa_srm_oper_config_t op = {
        .in = {
            .buffer         = rgb565,
            .pic_w          = src_w,
            .pic_h          = src_h,
            .block_w        = src_w,
            .block_h        = src_h,
            .block_offset_x = 0,
            .block_offset_y = 0,
            .srm_cm         = PPA_SRM_COLOR_MODE_RGB565,
        },
        .out = {
            .buffer         = s_pv.buf[s_pv.back],
            .buffer_size    = s_pv.buf_size,
            .pic_w          = s_pv.width,
            .pic_h          = s_pv.height,
            .block_offset_x = 0,
            .block_offset_y = 0,
            .srm_cm         = PPA_SRM_COLOR_MODE_RGB565,
        },
        .rotation_angle = angle_enum(s_pv.ppa_angle),
        /* A 90 or 270 degree turn swaps which source axis feeds which output
         * axis, so the scale factors have to swap with it. */
        .scale_x = swapped ? (float)s_pv.width / (float)src_h
                           : (float)s_pv.width / (float)src_w,
        .scale_y = swapped ? (float)s_pv.height / (float)src_w
                           : (float)s_pv.height / (float)src_h,
        .mode = PPA_TRANS_MODE_BLOCKING,
    };

    if (ppa_do_scale_rotate_mirror(s_pv.ppa, &op) == ESP_OK) {
        s_pv.pending = true;
    }
}

void app_preview_set_rotation(int screen_rotation_degrees)
{
    lv_display_t *disp = lv_display_get_default();
    uint32_t w = lv_display_get_horizontal_resolution(disp);
    uint32_t h = lv_display_get_vertical_resolution(disp);

    if (!s_pv.canvas) {
        return;
    }
    if ((size_t)w * h * 2 > s_pv.buf_size) {
        ESP_LOGE(preview_tag(), "rotated resolution %ux%u does not fit the buffers", 
                 (unsigned)w, (unsigned)h);
        return;
    }

    s_pv.width  = w;
    s_pv.height = h;
    /* PLUS, not minus. The PPA rotates counter-clockwise while LVGL's display
     * rotation goes the other way, so subtracting made the composed angle
     * mount - 2R: identical within the portrait and landscape families but 180
     * degrees apart between them. Adding cancels R exactly and pins the physical
     * rotation at the mount correction in all four poses. */
    s_pv.ppa_angle = CONFIG_PETCAM_CAMERA_MOUNT_ROTATION + screen_rotation_degrees;
    s_pv.pending = false;

    for (int i = 0; i < 2; i++) {
        memset(s_pv.buf[i], 0, s_pv.buf_size);
    }
    lv_canvas_set_buffer(s_pv.canvas, s_pv.buf[1 - s_pv.back], w, h, LV_COLOR_FORMAT_RGB565);
    lv_obj_set_size(s_pv.canvas, w, h);
    lv_obj_align(s_pv.canvas, LV_ALIGN_TOP_LEFT, 0, 0);

    ESP_LOGI(preview_tag(), "screen %d deg -> preview %ux%u, camera rotated %d deg",
             screen_rotation_degrees, (unsigned)w, (unsigned)h,
             ((s_pv.ppa_angle % 360) + 360) % 360);
}

void app_preview_refresh(void)
{
    if (!s_pv.canvas || !s_pv.pending || s_pv.hidden) {
        return;
    }

    xSemaphoreTake(s_pv.lock, portMAX_DELAY);
    lv_canvas_set_buffer(s_pv.canvas, s_pv.buf[s_pv.back], s_pv.width, s_pv.height,
                         LV_COLOR_FORMAT_RGB565);
    s_pv.back = 1 - s_pv.back;
    s_pv.pending = false;
    xSemaphoreGive(s_pv.lock);

    lv_obj_invalidate(s_pv.canvas);
}

void app_preview_set_visible(bool visible)
{
    if (!s_pv.canvas) {
        return;
    }
    if (visible) {
        lv_obj_clear_flag(s_pv.canvas, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_pv.canvas, LV_OBJ_FLAG_HIDDEN);
    }
    s_pv.hidden = !visible;
}

bool app_preview_enabled(void)
{
    return s_pv.canvas != NULL;
}
