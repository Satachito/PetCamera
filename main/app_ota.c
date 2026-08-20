#include <string.h>

#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_app_format.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "app_ota.h"

static const char *TAG = "ota";

/* Big enough that a 1.7 MB image is not thousands of round trips, small enough
 * to sit comfortably in the HTTP task's stack budget as a heap buffer. */
#define OTA_CHUNK 4096

static esp_err_t update_handler(httpd_req_t *req)
{
    const esp_partition_t *target = esp_ota_get_next_update_partition(NULL);
    esp_ota_handle_t handle = 0;
    char *buf;
    int remaining = req->content_len;
    int written = 0;
    esp_err_t err;
    char reply[128];

    if (!target) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no update partition");
        return ESP_FAIL;
    }
    if (remaining <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "empty body");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "update starting: %d bytes into %s", remaining, target->label);

    buf = malloc(OTA_CHUNK);
    if (!buf) {
        return httpd_resp_send_500(req);
    }

    err = esp_ota_begin(target, remaining, &handle);
    if (err != ESP_OK) {
        free(buf);
        ESP_LOGE(TAG, "esp_ota_begin failed: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(err));
        return ESP_FAIL;
    }

    while (remaining > 0) {
        int want = remaining > OTA_CHUNK ? OTA_CHUNK : remaining;
        int got = httpd_req_recv(req, buf, want);

        if (got == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (got <= 0) {
            ESP_LOGE(TAG, "upload aborted after %d bytes", written);
            esp_ota_abort(handle);
            free(buf);
            return ESP_FAIL;
        }

        err = esp_ota_write(handle, buf, got);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_write failed: %s", esp_err_to_name(err));
            esp_ota_abort(handle);
            free(buf);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(err));
            return ESP_FAIL;
        }
        written += got;
        remaining -= got;
    }
    free(buf);

    err = esp_ota_end(handle);
    if (err != ESP_OK) {
        /* A truncated or corrupt image is rejected here rather than at boot. */
        ESP_LOGE(TAG, "esp_ota_end failed: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, esp_err_to_name(err));
        return ESP_FAIL;
    }

    err = esp_ota_set_boot_partition(target);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(err));
        return ESP_FAIL;
    }

    snprintf(reply, sizeof(reply),
             "{\"written\":%d,\"partition\":\"%s\",\"rebooting\":true}",
             written, target->label);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, reply);

    ESP_LOGI(TAG, "update written to %s; rebooting", target->label);
    /* Let the response reach the client before the reset. */
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;
}

esp_err_t app_ota_register(httpd_handle_t server)
{
    static const httpd_uri_t update_uri = {
        .uri = "/update", .method = HTTP_POST, .handler = update_handler,
    };

    return httpd_register_uri_handler(server, &update_uri);
}

void app_ota_mark_working(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state;

    if (esp_ota_get_state_partition(running, &state) != ESP_OK) {
        return;
    }
    if (state == ESP_OTA_IMG_PENDING_VERIFY) {
        esp_ota_mark_app_valid_cancel_rollback();
        ESP_LOGI(TAG, "new image on %s confirmed working", running->label);
    }
}

const char *app_ota_running_partition(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();

    return running ? running->label : "?";
}
