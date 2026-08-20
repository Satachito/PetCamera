/*
 * Tab5 Pet Camera
 *
 * Pipeline:
 *   SC2356 (MIPI-CSI) -> esp_video/V4L2 -> hardware JPEG encoder -> frame_bus
 *                                                                     |
 *                    ESP32-C6 (ESP-Hosted Wi-Fi) <- HTTP MJPEG <------+
 *
 * Frames are never copied: the sensor DMAs into buffers the JPEG encoder can
 * read, and HTTP handlers borrow the encoder's output buffer while sending.
 */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_err.h"
#include "nvs_flash.h"
#include "bsp/esp-bsp.h"

#include "app_audio.h"
#include "app_camera.h"
#include "app_httpd.h"
#include "app_motion.h"
#include "app_ota.h"
#include "app_ui.h"
#include "app_wifi.h"

static const char *TAG = "petcam";

#define WIFI_CONNECT_TIMEOUT_MS 20000

void app_main(void)
{
    esp_err_t err;

    esp_err_t nvs_err = nvs_flash_init();
    if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES || nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs_err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(nvs_err);

    /* The IO expander that gates the camera, Wi-Fi and LCD rails hangs off I2C,
     * so this has to come before any bsp_feature_enable(). */
    ESP_ERROR_CHECK(bsp_i2c_init());

    /* Bring the panel up first so the console can report progress — including
     * failures — instead of leaving a black screen during a slow Wi-Fi join. */
    /* sw_rotate is what lets LVGL turn the screen at all — and on the ESP32-P4
     * esp_lvgl_port implements it with the PPA, so the rotation is hardware and
     * costs almost nothing despite the name. */
    const bsp_display_cfg_t display_cfg = {
        .lvgl_port_cfg = ESP_LVGL_PORT_INIT_CONFIG(),
        .buffer_size   = BSP_LCD_H_RES * CONFIG_BSP_LCD_DRAW_BUF_HEIGHT,
        .double_buffer = true,
        .flags = {
            .buff_dma    = false,
            .buff_spiram = true,
            .sw_rotate   = true,
        },
    };

    bsp_display_start_with_config(&display_cfg);
    /* bsp_display_start() brings up the panel and LVGL but leaves the backlight
     * off; without this the console renders perfectly into a black screen. */
    ESP_ERROR_CHECK(bsp_display_brightness_set(CONFIG_PETCAM_SCREEN_BRIGHTNESS));
    ESP_ERROR_CHECK(app_ui_start());

    app_ui_set_status("connecting to Wi-Fi");
    err = app_wifi_start(WIFI_CONNECT_TIMEOUT_MS);
    if (err == ESP_ERR_TIMEOUT) {
        ESP_LOGW(TAG, "no IP yet; retrying in the background");
        app_ui_set_status("Wi-Fi not connected - retrying");
    } else if (err != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi unavailable (%s); running without a network",
                 esp_err_to_name(err));
        app_ui_set_status("Wi-Fi unavailable - see the serial log");
    }

    app_ui_set_status("starting the camera");
    err = bsp_camera_start(NULL);
    if (err == ESP_OK) {
        err = app_camera_start();
    }
    if (err != ESP_OK) {
        /* Same reasoning as Wi-Fi: leave the console up with an explanation
         * rather than boot-looping where nobody can read the error. */
        ESP_LOGE(TAG, "camera unavailable: %s", esp_err_to_name(err));
        app_ui_set_status("camera failed - see the serial log");
    }

#if CONFIG_PETCAM_ENABLE_MOTION
    /* Recording is a bonus, not a requirement: no card simply means no clips. */
    app_motion_start();
#endif

    /* Audio is a bonus like recording is: no codec simply means no sound. */
    app_audio_start();

    ESP_ERROR_CHECK(app_httpd_start());

    if (app_wifi_is_connected()) {
        ESP_LOGI(TAG, "ready: http://%s/ (also http://%s.local/)",
                 app_wifi_ip(), CONFIG_PETCAM_HOSTNAME);
    }
    /* Everything that matters is up, so a freshly flashed image can stop being
     * provisional. A build that crashed before here is rolled back instead. */
    app_ota_mark_working();

    app_ui_set_status("streaming");
}
