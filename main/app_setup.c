#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "bsp/esp-bsp.h"

#include "app_setup.h"
#include "app_preview.h"
#include "app_wifi.h"

static const char *TAG = "setup";

#define MAX_NETWORKS 20

static struct {
    lv_obj_t *panel;        /* whole modal, NULL when closed */
    lv_obj_t *list;
    lv_obj_t *password;
    lv_obj_t *keyboard;
    lv_obj_t *hint;
    char      chosen_ssid[33];
} s_setup;

static void close_panel(void)
{
    if (s_setup.panel) {
        lv_obj_del(s_setup.panel);
        memset(&s_setup, 0, sizeof(s_setup));
        app_preview_set_visible(true);
    }
}

static void cancel_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    close_panel();
}

/* Trying the network takes up to 20 seconds. Running that on the LVGL task
 * would freeze the panel for the whole attempt, so it gets its own. */
static void save_task(void *arg)
{
    char *pass = (char *)arg;
    esp_err_t err = app_wifi_set_credentials(s_setup.chosen_ssid, pass);

    free(pass);

    if (bsp_display_lock(3000)) {
        if (s_setup.panel) {
            if (err == ESP_OK) {
                close_panel();
            } else {
                lv_label_set_text(s_setup.hint,
                                  "Could not connect - wrong password? Still on the old network.");
            }
        }
        bsp_display_unlock();
    }
    vTaskDelete(NULL);
}

static void save_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    ESP_LOGI(TAG, "save tapped, ssid=\"%s\"", s_setup.chosen_ssid);

    if (s_setup.chosen_ssid[0] == '\0') {
        lv_label_set_text(s_setup.hint, "Pick a network first");
        return;
    }

    char *pass = strdup(lv_textarea_get_text(s_setup.password));
    if (!pass) {
        return;
    }

    lv_label_set_text(s_setup.hint, "Connecting...");
    if (xTaskCreate(save_task, "petcam_save", 6144, pass, 3, NULL) != pdPASS) {
        free(pass);
        lv_label_set_text(s_setup.hint, "Out of memory");
    }
}

/* Forgets the highlighted network, or all of them when none is highlighted. */
static void forget_task(void *arg)
{
    char *ssid = (char *)arg;

    app_wifi_forget(ssid[0] ? ssid : NULL);

    if (bsp_display_lock(3000)) {
        if (s_setup.hint) {
            lv_label_set_text_fmt(s_setup.hint, "Forgotten. %d network(s) remembered",
                                  app_wifi_saved_count());
        }
        bsp_display_unlock();
    }
    free(ssid);
    vTaskDelete(NULL);
}

static void forget_cb(lv_event_t *e)
{
    char *ssid = strdup(s_setup.chosen_ssid);

    LV_UNUSED(e);
    ESP_LOGI(TAG, "forget tapped (ssid=\"%s\")", s_setup.chosen_ssid);
    if (!ssid) {
        return;
    }
    lv_label_set_text(s_setup.hint, "Clearing...");
    if (xTaskCreate(forget_task, "petcam_forget", 4096, ssid, 3, NULL) != pdPASS) {
        free(ssid);
    }
}

static void network_cb(lv_event_t *e)
{
    ESP_LOGI(TAG, "network row tapped");
    lv_obj_t *btn = lv_event_get_target(e);
    const char *text = lv_list_get_button_text(s_setup.list, btn);

    /* List rows read "SSID   -61 dBm"; keep only the name. */
    const char *sep = strstr(text, "   ");
    size_t len = sep ? (size_t)(sep - text) : strlen(text);

    if (len >= sizeof(s_setup.chosen_ssid)) {
        len = sizeof(s_setup.chosen_ssid) - 1;
    }
    memcpy(s_setup.chosen_ssid, text, len);
    s_setup.chosen_ssid[len] = '\0';

    /* Re-selecting the network already in use should not mean retyping its
     * password. Anything else starts empty — leaving the previous entry in the
     * field would silently submit the wrong password for the new network. */
    const char *saved = app_wifi_saved_password(s_setup.chosen_ssid);

    if (saved && saved[0]) {
        lv_textarea_set_text(s_setup.password, saved);
        lv_label_set_text_fmt(s_setup.hint, "Password for %s (saved)", s_setup.chosen_ssid);
    } else {
        lv_textarea_set_text(s_setup.password, "");
        lv_label_set_text_fmt(s_setup.hint, "Password for %s", s_setup.chosen_ssid);
    }

    lv_obj_clear_flag(s_setup.password, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_setup.keyboard, LV_OBJ_FLAG_HIDDEN);
    lv_keyboard_set_textarea(s_setup.keyboard, s_setup.password);
}

/* Scanning blocks for a couple of seconds, so it runs off the LVGL task and
 * fills the list afterwards under the lock. */
static void scan_task(void *arg)
{
    wifi_ap_record_t *records = calloc(MAX_NETWORKS, sizeof(*records));
    uint16_t found = 0;

    int64_t started = esp_timer_get_time();
    esp_err_t err = records ? app_wifi_scan(records, MAX_NETWORKS, &found) : ESP_ERR_NO_MEM;

    if (err != ESP_OK) {
        found = 0;
    }
    ESP_LOGI(TAG, "scan finished: %s, %u networks, %lld ms",
             esp_err_to_name(err), (unsigned)found,
             (long long)((esp_timer_get_time() - started) / 1000));

    if (bsp_display_lock(5000)) {
        if (s_setup.list) {
            lv_obj_clean(s_setup.list);
            if (found == 0) {
                lv_label_set_text(s_setup.hint, "No networks found");
            } else {
                for (int i = 0; i < found; i++) {
                    char row[80];
                    bool known = app_wifi_is_saved((const char *)records[i].ssid);

                    /* The three-space separator is what network_cb splits on, so
                     * the marker has to sit after it. */
                    snprintf(row, sizeof(row), "%s   %d dBm%s",
                             (const char *)records[i].ssid, records[i].rssi,
                             known ? "  " LV_SYMBOL_OK : "");
                    lv_obj_t *btn = lv_list_add_button(s_setup.list,
                                                       known ? LV_SYMBOL_OK : LV_SYMBOL_WIFI,
                                                       row);
                    lv_obj_add_event_cb(btn, network_cb, LV_EVENT_CLICKED, NULL);
                }
                lv_label_set_text_fmt(s_setup.hint, "Tap a network (%d remembered)",
                                      app_wifi_saved_count());
            }
            ESP_LOGI(TAG, "list now has %" PRIu32 " rows, size %" PRId32 "x%" PRId32
                     " at (%" PRId32 ",%" PRId32 "), panel %" PRId32 "x%" PRId32
                     ", hidden=%d",
                     lv_obj_get_child_count(s_setup.list),
                     lv_obj_get_width(s_setup.list), lv_obj_get_height(s_setup.list),
                     lv_obj_get_x(s_setup.list), lv_obj_get_y(s_setup.list),
                     lv_obj_get_width(s_setup.panel), lv_obj_get_height(s_setup.panel),
                     lv_obj_has_flag(s_setup.list, LV_OBJ_FLAG_HIDDEN) ? 1 : 0);
        }
        bsp_display_unlock();
    } else {
        /* The LVGL lock is held for the whole preview blit; if that ever runs
         * long the list would silently never appear. */
        ESP_LOGE(TAG, "could not take the LVGL lock to show the scan results");
    }

    free(records);
    vTaskDelete(NULL);
}

static void open_panel(void)
{
    lv_obj_t *screen = lv_screen_active();
    int32_t w = lv_display_get_horizontal_resolution(lv_display_get_default());
    int32_t h = lv_display_get_vertical_resolution(lv_display_get_default());

    if (s_setup.panel) {
        ESP_LOGW(TAG, "panel already open");
        return;
    }

    /* The panel covers the screen anyway, so nothing is lost by dropping the
     * live image from the render tree while it is up. */
    app_preview_set_visible(false);

    s_setup.panel = lv_obj_create(screen);
    lv_obj_set_size(s_setup.panel, w, h);
    lv_obj_align(s_setup.panel, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_color(s_setup.panel, lv_color_hex(0x0e0f12), 0);
    lv_obj_set_style_bg_opa(s_setup.panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_setup.panel, 0, 0);
    lv_obj_set_style_pad_all(s_setup.panel, 16, 0);
    lv_obj_set_style_radius(s_setup.panel, 0, 0);
    lv_obj_set_style_clip_corner(s_setup.panel, false, 0);
    lv_obj_clear_flag(s_setup.panel, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(s_setup.panel);
    lv_label_set_text(title, "Wi-Fi setup");
    lv_obj_set_style_text_color(title, lv_color_hex(0xe6e7ea), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, 0);

    s_setup.hint = lv_label_create(s_setup.panel);
    lv_label_set_text(s_setup.hint, "Scanning...");
    lv_obj_set_style_text_color(s_setup.hint, lv_color_hex(0x9a9da6), 0);
    lv_obj_align(s_setup.hint, LV_ALIGN_TOP_LEFT, 0, 44);

    s_setup.list = lv_list_create(s_setup.panel);
    lv_obj_set_size(s_setup.list, w - 32, h / 3);
    lv_obj_align(s_setup.list, LV_ALIGN_TOP_LEFT, 0, 78);
    /* The default theme gives a list rounded corners, and clipping children to
     * a rounded rect makes LVGL allocate a layer the width of the widget. At
     * 1248 px wide that is ~124 KB per draw strip against a 64 KB pool, so the
     * allocation never succeeds — and lv_draw_dispatch retries forever instead
     * of failing, which shows up as a frozen UI, not an error. Square corners
     * need no layer at all. */
    lv_obj_set_style_radius(s_setup.list, 0, 0);
    lv_obj_set_style_clip_corner(s_setup.list, false, 0);

    s_setup.password = lv_textarea_create(s_setup.panel);
    lv_textarea_set_one_line(s_setup.password, true);
    lv_textarea_set_password_mode(s_setup.password, true);
    lv_textarea_set_placeholder_text(s_setup.password, "password");
    lv_obj_set_width(s_setup.password, w - 32);
    lv_obj_align_to(s_setup.password, s_setup.list, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 16);
    lv_obj_add_flag(s_setup.password, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *save = lv_button_create(s_setup.panel);
    lv_obj_set_size(save, 160, 56);
    lv_obj_align_to(save, s_setup.password, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 16);
    lv_obj_add_event_cb(save, save_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *save_label = lv_label_create(save);
    lv_label_set_text(save_label, "Save");
    lv_obj_center(save_label);

    lv_obj_t *cancel = lv_button_create(s_setup.panel);
    lv_obj_set_size(cancel, 160, 56);
    lv_obj_align_to(cancel, save, LV_ALIGN_OUT_RIGHT_MID, 16, 0);
    lv_obj_set_style_bg_color(cancel, lv_color_hex(0x3a3d45), 0);
    lv_obj_add_event_cb(cancel, cancel_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *cancel_label = lv_label_create(cancel);
    lv_label_set_text(cancel_label, "Cancel");
    lv_obj_center(cancel_label);

    /* A way back when a wrong password has been saved: without it the only
     * recovery is erasing NVS over USB. */
    lv_obj_t *forget = lv_button_create(s_setup.panel);
    lv_obj_set_size(forget, 200, 56);
    lv_obj_align_to(forget, cancel, LV_ALIGN_OUT_RIGHT_MID, 16, 0);
    lv_obj_set_style_bg_color(forget, lv_color_hex(0x5a2d31), 0);
    lv_obj_set_style_radius(forget, 0, 0);
    lv_obj_add_event_cb(forget, forget_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *forget_label = lv_label_create(forget);
    lv_label_set_text(forget_label, "Forget saved");
    lv_obj_center(forget_label);

    s_setup.keyboard = lv_keyboard_create(s_setup.panel);
    lv_obj_set_size(s_setup.keyboard, w - 32, h / 3);
    lv_obj_set_style_radius(s_setup.keyboard, 0, 0);
    lv_obj_set_style_clip_corner(s_setup.keyboard, false, 0);
    lv_obj_align(s_setup.keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_flag(s_setup.keyboard, LV_OBJ_FLAG_HIDDEN);

    if (xTaskCreate(scan_task, "petcam_scan", 6144, NULL, 3, NULL) != pdPASS) {
        ESP_LOGE(TAG, "cannot start the scan task");
        lv_label_set_text(s_setup.hint, "Cannot scan (out of memory)");
    }
}

static void open_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    ESP_LOGI(TAG, "settings button tapped");
    open_panel();
}

/* The panel is sized from the logical resolution when it opens. Rotating the
 * device swaps those dimensions, so a panel left open would keep the old shape
 * and hang off the screen. */
void app_setup_relayout(void)
{
    int32_t w = lv_display_get_horizontal_resolution(lv_display_get_default());
    int32_t h = lv_display_get_vertical_resolution(lv_display_get_default());

    if (!s_setup.panel) {
        return;
    }

    lv_obj_set_size(s_setup.panel, w, h);
    lv_obj_align(s_setup.panel, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_size(s_setup.list, w - 32, h / 3);
    lv_obj_align(s_setup.list, LV_ALIGN_TOP_LEFT, 0, 78);
    lv_obj_set_width(s_setup.password, w - 32);
    lv_obj_align_to(s_setup.password, s_setup.list, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 16);
    lv_obj_set_size(s_setup.keyboard, w - 32, h / 3);
    lv_obj_align(s_setup.keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
}

void app_setup_open(void)
{
    if (bsp_display_lock(2000)) {
        open_panel();
        bsp_display_unlock();
    }
}

lv_obj_t *app_setup_attach(lv_obj_t *parent)
{
    lv_obj_t *btn = lv_button_create(parent);

    lv_obj_set_size(btn, 64, 64);
    lv_obj_align(btn, LV_ALIGN_TOP_LEFT, 16, 16);
    lv_obj_set_style_bg_color(btn, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_60, 0);
    lv_obj_set_style_radius(btn, 32, 0);
    lv_obj_add_event_cb(btn, open_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *icon = lv_label_create(btn);
    lv_label_set_text(icon, LV_SYMBOL_SETTINGS);
    lv_obj_center(icon);

    ESP_LOGI(TAG, "Wi-Fi setup available from the on-screen button");
    return btn;
}

void app_setup_get_state(bool *open, char *hint, size_t hint_len, int *rows)
{
    *open = s_setup.panel != NULL;
    *rows = 0;
    hint[0] = '\0';

    if (!*open) {
        return;
    }
    if (bsp_display_lock(1000)) {
        const char *text = s_setup.hint ? lv_label_get_text(s_setup.hint) : "";
        strlcpy(hint, text ? text : "", hint_len);
        *rows = s_setup.list ? (int)lv_obj_get_child_count(s_setup.list) : -1;
        bsp_display_unlock();
    } else {
        strlcpy(hint, "(LVGL lock timed out)", hint_len);
        *rows = -1;
    }
}
