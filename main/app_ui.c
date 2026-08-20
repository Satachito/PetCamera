#include <inttypes.h>
#include <time.h>
#include "esp_timer.h"
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "bsp/esp-bsp.h"

#include "app_ui.h"
#include "driver/jpeg_encode.h"
#include "esp_heap_caps.h"
#include "app_audio.h"
#include "app_camera.h"
#include "app_httpd.h"
#include "app_preview.h"
#include "app_orientation.h"
#include "app_setup.h"
#include "app_wifi.h"

static const char *TAG = "ui";

#define COLOR_TEXT   lv_color_hex(0xe6e7ea)
#define COLOR_MUTED  lv_color_hex(0x9a9da6)
#define COLOR_ACCENT lv_color_hex(0x3ddc84)

#define QR_SIZE 180

#define COLOR_REC lv_color_hex(0xe5484d)

static struct {
    lv_obj_t *canvas;
    lv_obj_t *setup_btn;
    lv_obj_t *rec_btn;
    lv_obj_t *rec_overlay;
    lv_obj_t *rec_big;
    lv_obj_t *rec_sub;
    lv_obj_t *bar;
    lv_obj_t *url_label;
    lv_obj_t *stats_label;
    lv_obj_t *qr;
    lv_obj_t *status_label;
    char      url[64];
    char      status_text[64];
    bool      status_dirty;
} s_ui;

/* The overlay sits on top of the live image, so it needs its own background to
 * stay readable against whatever the camera happens to be pointing at. */
static lv_obj_t *make_overlay(lv_obj_t *parent, int32_t width, int32_t height)
{
    lv_obj_t *o = lv_obj_create(parent);

    lv_obj_set_size(o, width, height);
    lv_obj_set_style_bg_color(o, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_70, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_radius(o, 0, 0);
    lv_obj_set_style_pad_all(o, 16, 0);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

/* Everything that depends on the logical resolution lives here so a rotation is
 * a re-layout rather than a rebuild of the whole screen. */
static void layout(void)
{
    lv_display_t *disp = lv_display_get_default();
    int32_t w = lv_display_get_horizontal_resolution(disp);

    lv_obj_set_size(s_ui.bar, w, 104);
    lv_obj_align(s_ui.bar, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_align(s_ui.url_label, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_align(s_ui.stats_label, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_align(s_ui.qr, LV_ALIGN_TOP_RIGHT, -16, 16);
    lv_obj_align(s_ui.status_label, LV_ALIGN_CENTER, 0, 0);

    if (s_ui.setup_btn) {
        lv_obj_align(s_ui.setup_btn, LV_ALIGN_TOP_LEFT, 16, 16);
    }
    if (s_ui.rec_btn) {
        lv_obj_align(s_ui.rec_btn, LV_ALIGN_TOP_LEFT, 16, 96);
    }
    if (s_ui.rec_overlay) {
        lv_obj_set_size(s_ui.rec_overlay, w, 190);
        lv_obj_align(s_ui.rec_overlay, LV_ALIGN_CENTER, 0, 0);
        lv_obj_align(s_ui.rec_big, LV_ALIGN_CENTER, 0, -22);
        lv_obj_align(s_ui.rec_sub, LV_ALIGN_CENTER, 0, 44);
    }
}

#if CONFIG_PETCAM_AUTO_ROTATE
static void on_orientation_change(int degrees)
{
    static const lv_display_rotation_t map[] = {
        LV_DISPLAY_ROTATION_0, LV_DISPLAY_ROTATION_90,
        LV_DISPLAY_ROTATION_180, LV_DISPLAY_ROTATION_270,
    };

    if (!bsp_display_lock(1000)) {
        return;
    }
    bsp_display_rotate(lv_display_get_default(), map[(degrees / 90) % 4]);
    app_preview_set_rotation(degrees);
    layout();
    app_setup_relayout();
    bsp_display_unlock();
}
#endif

/* Recording is triggered from a phone, but the person who speaks is here. The
 * overlay is deliberately unmissable: a countdown they can speak after, then a
 * clear indication that the microphone is live. */
static void update_recording_overlay(void)
{
    int ms_left = 0;
    app_audio_phase_t phase = app_audio_get_phase(&ms_left);
    static app_audio_phase_t last = APP_AUDIO_IDLE;
    static int last_shown = -1;
    static int64_t idle_since;
    int seconds = (ms_left + 999) / 1000;

    if (phase == APP_AUDIO_IDLE) {
        if (last != APP_AUDIO_IDLE) {
            /* Land on zero and stay there for a moment. Swapping straight back
             * to the camera image the instant the count runs out looks like the
             * screen glitched rather than like the recording completing. */
            lv_label_set_text(s_ui.rec_big, "0");
            lv_obj_set_style_text_color(s_ui.rec_big, COLOR_TEXT, 0);
            lv_label_set_text(s_ui.rec_sub, "saved");
            idle_since = esp_timer_get_time();
            last = phase;
            last_shown = -1;
            return;
        }
        if (idle_since &&
            esp_timer_get_time() - idle_since >= CONFIG_PETCAM_RECORD_HOLD_MS * 1000LL) {
            lv_obj_add_flag(s_ui.rec_overlay, LV_OBJ_FLAG_HIDDEN);
            /* Whatever hid the live image — the record panel or this overlay —
             * it comes back now that the sequence is over. */
            app_preview_set_visible(true);
            idle_since = 0;
        }
        return;
    }
    idle_since = 0;

    if (phase == last && seconds == last_shown) {
        return;
    }
    last = phase;
    last_shown = seconds;

    /* The camera image is the most expensive thing on the screen; with it out
     * of the way the countdown redraws immediately. */
    app_preview_set_visible(false);
    lv_obj_clear_flag(s_ui.rec_overlay, LV_OBJ_FLAG_HIDDEN);
    /* Anything created after this — the settings panel, the record panel — sits
     * above it in the child order, so raise it each time rather than trusting
     * the order it was built in. */
    lv_obj_move_foreground(s_ui.rec_overlay);
    lv_obj_invalidate(s_ui.rec_overlay);

    switch (phase) {
    case APP_AUDIO_COUNTDOWN:
        lv_label_set_text_fmt(s_ui.rec_big, "%d", seconds);
        lv_obj_set_style_text_color(s_ui.rec_big, COLOR_TEXT, 0);
        lv_label_set_text(s_ui.rec_sub, "get ready to speak");
        break;
    case APP_AUDIO_RECORDING:
        lv_label_set_text(s_ui.rec_big, LV_SYMBOL_AUDIO " REC");
        lv_obj_set_style_text_color(s_ui.rec_big, COLOR_REC, 0);
        lv_label_set_text_fmt(s_ui.rec_sub, "speak now  ·  %d s left", seconds);
        break;
    case APP_AUDIO_PLAYING:
        lv_label_set_text(s_ui.rec_big, LV_SYMBOL_VOLUME_MAX);
        lv_obj_set_style_text_color(s_ui.rec_big, COLOR_ACCENT, 0);
        lv_label_set_text(s_ui.rec_sub, "playing a saved sound");
        break;
    default:
        break;
    }
}

/* @param restore_preview  false while a recording follows: bringing the
 *        full-screen camera image back costs a redraw long enough that the
 *        three-second countdown had already finished before the first digit
 *        appeared, so it looked as though recording started immediately. */
/* Straight to recording: the name is generated here and changed later from the
 * phone, which has a real keyboard. Typing on the device delayed the thing the
 * button is for. */
static void record_cb(lv_event_t *e)
{
    char name[32];
    time_t now = time(NULL);
    struct tm tm;

    LV_UNUSED(e);
    if (app_audio_get_phase(&(int){ 0 }) != APP_AUDIO_IDLE) {
        return;
    }

    localtime_r(&now, &tm);
    if (tm.tm_year < 120) {
        snprintf(name, sizeof(name), "rec_%08lu.wav",
                 (unsigned long)(esp_timer_get_time() / 1000000));
    } else {
        snprintf(name, sizeof(name), "rec_%02d%02d%02d.wav",
                 tm.tm_hour, tm.tm_min, tm.tm_sec);
    }

    /* The camera image is the most expensive thing on this screen; dropping it
     * now means the countdown appears immediately rather than after a redraw. */
    app_preview_set_visible(false);
    ESP_LOGI(TAG, "recording to %s", name);
    app_audio_record(name, 3000);
}

static void refresh_task(void *arg)
{
    for (;;) {
        app_camera_stats_t stats;
        char buf[96];
        static int tick;

        app_camera_get_stats(&stats);
        /* Stats change slowly; only the overlay needs the faster tick. */
        bool slow = (++tick % 5) == 0;

        if (bsp_display_lock(200)) {
            app_preview_refresh();
            update_recording_overlay();

            if (s_ui.status_dirty) {
                s_ui.status_dirty = false;
                lv_label_set_text(s_ui.status_label, s_ui.status_text);
                lv_obj_clear_flag(s_ui.status_label, LV_OBJ_FLAG_HIDDEN);
                lv_obj_align(s_ui.status_label, LV_ALIGN_CENTER, 0, 0);
            }

            /* Once there is a picture and an address, the banner has said all it
             * can and only covers the view. */
            if (slow && stats.fps > 0.5f && app_wifi_is_connected() &&
                !lv_obj_has_flag(s_ui.status_label, LV_OBJ_FLAG_HIDDEN) &&
                !s_ui.status_dirty) {
                lv_obj_add_flag(s_ui.status_label, LV_OBJ_FLAG_HIDDEN);
            }

            if (slow) {
            snprintf(buf, sizeof(buf), "%.0f fps   %u viewer%s   %u KB/frame",
                     stats.fps, (unsigned)app_httpd_client_count(),
                     app_httpd_client_count() == 1 ? "" : "s",
                     (unsigned)(stats.last_jpeg_len / 1024));
            lv_label_set_text(s_ui.stats_label, buf);

            if (app_wifi_is_connected() && s_ui.url[0] == '\0') {
                snprintf(s_ui.url, sizeof(s_ui.url), "http://%s/", app_wifi_ip());
                lv_label_set_text(s_ui.url_label, s_ui.url);
                lv_qrcode_update(s_ui.qr, s_ui.url, strlen(s_ui.url));
                lv_obj_clear_flag(s_ui.qr, LV_OBJ_FLAG_HIDDEN);
                lv_obj_add_flag(s_ui.status_label, LV_OBJ_FLAG_HIDDEN);
            } else if (!app_wifi_is_connected() && s_ui.url[0] != '\0') {
                s_ui.url[0] = '\0';
                lv_label_set_text(s_ui.url_label, "reconnecting...");
                lv_obj_add_flag(s_ui.qr, LV_OBJ_FLAG_HIDDEN);
            }
            }

            bsp_display_unlock();
        }

        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

esp_err_t app_ui_start(void)
{
    lv_display_t *disp = lv_display_get_default();
    int32_t w = lv_display_get_horizontal_resolution(disp);
    int32_t h = lv_display_get_vertical_resolution(disp);

    if (!bsp_display_lock(1000)) {
        ESP_LOGE(TAG, "cannot take the LVGL lock");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "console on a %" PRId32 "x%" PRId32 " panel", w, h);

    lv_obj_t *screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, lv_color_black(), 0);
    lv_obj_set_style_pad_all(screen, 0, 0);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

#if CONFIG_PETCAM_ENABLE_PREVIEW
    /* Full-screen live image underneath everything else. */
    if (app_preview_init(screen, w, h, &s_ui.canvas) == ESP_OK) {
        lv_obj_align(s_ui.canvas, LV_ALIGN_TOP_LEFT, 0, 0);
    } else {
        ESP_LOGW(TAG, "preview unavailable; console runs without it");
    }
#endif

    /* QR in the top-right so it never sits under the text bar. */
    s_ui.qr = lv_qrcode_create(screen);
    lv_qrcode_set_size(s_ui.qr, QR_SIZE);
    lv_qrcode_set_dark_color(s_ui.qr, lv_color_black());
    lv_qrcode_set_light_color(s_ui.qr, lv_color_white());
    lv_obj_set_style_border_color(s_ui.qr, lv_color_white(), 0);
    lv_obj_set_style_border_width(s_ui.qr, 8, 0);
    lv_obj_align(s_ui.qr, LV_ALIGN_TOP_RIGHT, -16, 16);
    lv_obj_add_flag(s_ui.qr, LV_OBJ_FLAG_HIDDEN);

    s_ui.bar = make_overlay(screen, w, 104);
    lv_obj_align(s_ui.bar, LV_ALIGN_BOTTOM_MID, 0, 0);

    s_ui.url_label = lv_label_create(s_ui.bar);
    lv_label_set_text(s_ui.url_label, "connecting to Wi-Fi...");
    lv_obj_set_style_text_color(s_ui.url_label, COLOR_ACCENT, 0);
    lv_obj_set_style_text_font(s_ui.url_label, &lv_font_montserrat_28, 0);
    lv_obj_align(s_ui.url_label, LV_ALIGN_TOP_LEFT, 0, 0);

    s_ui.stats_label = lv_label_create(s_ui.bar);
    lv_label_set_text(s_ui.stats_label, "starting up");
    lv_obj_set_style_text_color(s_ui.stats_label, COLOR_MUTED, 0);
    lv_obj_align(s_ui.stats_label, LV_ALIGN_BOTTOM_LEFT, 0, 0);

    /* Startup and error messages get the middle of the screen, where they are
     * unmissable, and disappear once the camera is reachable. */
    s_ui.status_label = lv_label_create(screen);
    lv_label_set_text(s_ui.status_label, "starting up");
    lv_obj_set_style_text_color(s_ui.status_label, COLOR_TEXT, 0);
    lv_obj_set_style_text_font(s_ui.status_label, &lv_font_montserrat_28, 0);
    lv_obj_set_style_bg_color(s_ui.status_label, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_ui.status_label, LV_OPA_70, 0);
    lv_obj_set_style_pad_all(s_ui.status_label, 14, 0);
    lv_obj_set_style_radius(s_ui.status_label, 10, 0);
    lv_obj_align(s_ui.status_label, LV_ALIGN_CENTER, 0, 0);

    s_ui.setup_btn = app_setup_attach(screen);

    /* Record button, under the settings one. */
    s_ui.rec_btn = lv_button_create(screen);
    lv_obj_set_size(s_ui.rec_btn, 64, 64);
    lv_obj_set_style_bg_color(s_ui.rec_btn, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_ui.rec_btn, LV_OPA_60, 0);
    lv_obj_set_style_radius(s_ui.rec_btn, 32, 0);
    lv_obj_add_event_cb(s_ui.rec_btn, record_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *rec_icon = lv_label_create(s_ui.rec_btn);
    lv_label_set_text(rec_icon, LV_SYMBOL_AUDIO);
    lv_obj_set_style_text_color(rec_icon, COLOR_REC, 0);
    lv_obj_center(rec_icon);

    /* Full-width banner rather than a corner badge: it has to be readable from
     * across the room by someone who is not holding the device. */
    s_ui.rec_overlay = lv_obj_create(screen);
    lv_obj_set_style_bg_color(s_ui.rec_overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_ui.rec_overlay, LV_OPA_80, 0);
    lv_obj_set_style_border_width(s_ui.rec_overlay, 0, 0);
    lv_obj_set_style_radius(s_ui.rec_overlay, 0, 0);
    lv_obj_set_style_clip_corner(s_ui.rec_overlay, false, 0);
    lv_obj_clear_flag(s_ui.rec_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_ui.rec_overlay, LV_OBJ_FLAG_HIDDEN);

    s_ui.rec_big = lv_label_create(s_ui.rec_overlay);
    lv_label_set_text(s_ui.rec_big, "3");
    lv_obj_set_style_text_font(s_ui.rec_big, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(s_ui.rec_big, COLOR_TEXT, 0);

    s_ui.rec_sub = lv_label_create(s_ui.rec_overlay);
    lv_label_set_text(s_ui.rec_sub, "");
    lv_obj_set_style_text_font(s_ui.rec_sub, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(s_ui.rec_sub, COLOR_MUTED, 0);

    layout();

    bsp_display_unlock();

#if CONFIG_PETCAM_AUTO_ROTATE
    /* Started after the layout exists, so the first callback has something to
     * rearrange. */
    app_orientation_start(on_orientation_change);
#endif

    if (xTaskCreate(refresh_task, "petcam_ui", 4096, NULL, 2, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

/* Stores the message rather than writing it. Writing straight to LVGL meant a
 * status that arrived while the lock was held — which is most of startup — was
 * silently dropped, leaving whichever earlier message had got through sitting
 * on the screen. */
void app_ui_set_status(const char *text)
{
    strlcpy(s_ui.status_text, text ? text : "", sizeof(s_ui.status_text));
    s_ui.status_dirty = true;
}

esp_err_t app_ui_screenshot(uint8_t *out, size_t capacity, size_t *out_len)
{
    lv_display_t *disp = lv_display_get_default();
    int32_t w = lv_display_get_horizontal_resolution(disp);
    int32_t h = lv_display_get_vertical_resolution(disp);
    static jpeg_encoder_handle_t encoder;
    lv_draw_buf_t wrapper;
    lv_draw_buf_t *buf = NULL;
    uint8_t *pixels = NULL;
    esp_err_t err = ESP_FAIL;
    uint32_t len = 0;

    if (!encoder) {
        const jpeg_encode_engine_cfg_t cfg = { .timeout_ms = 200 };

        if (jpeg_new_encoder_engine(&cfg, &encoder) != ESP_OK) {
            return ESP_FAIL;
        }
    }

    /* lv_draw_buf_create() allocates through LVGL, whose pool is 64 KB — a
     * full-screen snapshot is 1.8 MB and never fits. Wrap PSRAM instead and let
     * lv_snapshot draw into that. */
    {
        uint32_t stride = (uint32_t)w * 2;
        size_t size = stride * (size_t)h;

        pixels = heap_caps_aligned_alloc(64, size,
                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_CACHE_ALIGNED);
        if (!pixels) {
            return ESP_ERR_NO_MEM;
        }
        buf = &wrapper;
        if (lv_draw_buf_init(buf, w, h, LV_COLOR_FORMAT_RGB565, stride,
                             pixels, size) != LV_RESULT_OK) {
            free(pixels);
            return ESP_ERR_NO_MEM;
        }
    }

    if (bsp_display_lock(2000)) {
        if (lv_snapshot_take_to_draw_buf(lv_screen_active(), LV_COLOR_FORMAT_RGB565,
                                         buf) == LV_RESULT_OK) {
            err = ESP_OK;
        }
        bsp_display_unlock();
    }

    if (err == ESP_OK) {
        const jpeg_encode_cfg_t enc = {
            .width         = w,
            .height        = h,
            .src_type      = JPEG_ENCODE_IN_FORMAT_RGB565,
            .sub_sample    = JPEG_DOWN_SAMPLING_YUV420,
            .image_quality = 80,
            /* The UI is rendered in LVGL's RGB565 order while the preview
             * canvas holds the PPA's, and the two differ — one setting cannot
             * make both right in a single image. The UI wins: this endpoint
             * exists to see what the panel is showing, and the camera's real
             * colour is available from /snapshot. The panel itself is correct
             * either way. */
            .pixel_reverse = false,
        };

        err = jpeg_encoder_process(encoder, &enc, buf->data, buf->data_size,
                                   out, capacity, &len);
        *out_len = len;
    }

    free(pixels);
    return err;
}
