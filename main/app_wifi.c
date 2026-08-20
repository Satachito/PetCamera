#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "mdns.h"
#include "esp_netif_sntp.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "bsp/esp-bsp.h"

#include "app_wifi.h"

static const char *TAG = "wifi";

#define CONNECTED_BIT BIT0

/* How long a new network gets to prove itself before the change is rejected. */
#define CREDENTIAL_TRIAL_MS 20000

#define NVS_NAMESPACE "petcam"
#define NVS_KEY_COUNT "n"

typedef struct {
    char ssid[33];
    char password[65];
} credential_t;

static struct {
    EventGroupHandle_t events;
    TaskHandle_t       manager;
    char               ip[16];
    char               ssid[33];      /* network currently being used */
    char               password[65];
    credential_t       saved[APP_WIFI_MAX_SAVED];
    int                saved_count;
    int                retries;
    bool               connected;
    bool               scanning;
} s_wifi = { .ip = "0.0.0.0" };

/* NVS keys are short by design; the index is always 0..APP_WIFI_MAX_SAVED-1. */
static void key_for(char *out, size_t len, const char *prefix, int index)
{
    snprintf(out, len, "%s%u", prefix, (unsigned)(index & 0xF));
}

static void persist_credentials(void)
{
    nvs_handle_t nvs;

    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK) {
        return;
    }
    for (int i = 0; i < APP_WIFI_MAX_SAVED; i++) {
        char key[8];

        key_for(key, sizeof(key), "s", i);
        if (i < s_wifi.saved_count) {
            nvs_set_str(nvs, key, s_wifi.saved[i].ssid);
        } else {
            nvs_erase_key(nvs, key);
        }
        key_for(key, sizeof(key), "p", i);
        if (i < s_wifi.saved_count) {
            nvs_set_str(nvs, key, s_wifi.saved[i].password);
        } else {
            nvs_erase_key(nvs, key);
        }
    }
    nvs_set_i32(nvs, NVS_KEY_COUNT, s_wifi.saved_count);
    nvs_commit(nvs);
    nvs_close(nvs);
}

static int find_saved(const char *ssid)
{
    for (int i = 0; i < s_wifi.saved_count; i++) {
        if (strcmp(s_wifi.saved[i].ssid, ssid) == 0) {
            return i;
        }
    }
    return -1;
}

static void remember(const char *ssid, const char *password)
{
    int slot = find_saved(ssid);

    if (slot < 0) {
        if (s_wifi.saved_count < APP_WIFI_MAX_SAVED) {
            slot = s_wifi.saved_count++;
        } else {
            /* Full: drop the oldest entry to make room for one that just
             * proved it works. */
            memmove(&s_wifi.saved[0], &s_wifi.saved[1],
                    sizeof(s_wifi.saved[0]) * (APP_WIFI_MAX_SAVED - 1));
            slot = APP_WIFI_MAX_SAVED - 1;
        }
    }
    strlcpy(s_wifi.saved[slot].ssid, ssid, sizeof(s_wifi.saved[slot].ssid));
    strlcpy(s_wifi.saved[slot].password, password ? password : "",
            sizeof(s_wifi.saved[slot].password));
    persist_credentials();
}

/* The build-time credentials are treated as one more remembered network rather
 * than a special case, so the selection logic has a single code path. */
static void load_credentials(void)
{
    nvs_handle_t nvs;
    int32_t count = 0;

    s_wifi.saved_count = 0;

    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) == ESP_OK) {
        if (nvs_get_i32(nvs, NVS_KEY_COUNT, &count) == ESP_OK) {
            if (count > APP_WIFI_MAX_SAVED) {
                count = APP_WIFI_MAX_SAVED;
            }
            for (int i = 0; i < count; i++) {
                char key[8];
                size_t len;

                key_for(key, sizeof(key), "s", i);
                len = sizeof(s_wifi.saved[s_wifi.saved_count].ssid);
                if (nvs_get_str(nvs, key, s_wifi.saved[s_wifi.saved_count].ssid, &len) != ESP_OK) {
                    continue;
                }
                key_for(key, sizeof(key), "p", i);
                len = sizeof(s_wifi.saved[s_wifi.saved_count].password);
                nvs_get_str(nvs, key, s_wifi.saved[s_wifi.saved_count].password, &len);
                s_wifi.saved_count++;
            }
        }
        nvs_close(nvs);
    }

    if (CONFIG_PETCAM_WIFI_SSID[0] && find_saved(CONFIG_PETCAM_WIFI_SSID) < 0 &&
        s_wifi.saved_count < APP_WIFI_MAX_SAVED) {
        strlcpy(s_wifi.saved[s_wifi.saved_count].ssid, CONFIG_PETCAM_WIFI_SSID,
                sizeof(s_wifi.saved[0].ssid));
        strlcpy(s_wifi.saved[s_wifi.saved_count].password, CONFIG_PETCAM_WIFI_PASSWORD,
                sizeof(s_wifi.saved[0].password));
        s_wifi.saved_count++;
    }

    strlcpy(s_wifi.ssid, s_wifi.saved_count ? s_wifi.saved[0].ssid : "",
            sizeof(s_wifi.ssid));
    strlcpy(s_wifi.password, s_wifi.saved_count ? s_wifi.saved[0].password : "",
            sizeof(s_wifi.password));
    ESP_LOGI(TAG, "%d remembered network(s)", s_wifi.saved_count);
}

static void apply_credentials(void)
{
    wifi_config_t cfg = { .sta = { .threshold.authmode = WIFI_AUTH_WPA2_PSK } };

    strlcpy((char *)cfg.sta.ssid, s_wifi.ssid, sizeof(cfg.sta.ssid));
    strlcpy((char *)cfg.sta.password, s_wifi.password, sizeof(cfg.sta.password));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &cfg));
}

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
        return;
    }

    if (id == WIFI_EVENT_STA_DISCONNECTED) {
        s_wifi.connected = false;
        strcpy(s_wifi.ip, "0.0.0.0");
        xEventGroupClearBits(s_wifi.events, CONNECTED_BIT);

        /* Keep retrying forever once the retry budget is spent — an unattended
         * camera should recover from a router reboot without a power cycle,
         * just more slowly so it is not hammering a network that is not there. */
        if (s_wifi.scanning) {
            /* Reconnecting mid-scan makes the scan return nothing, which is
             * exactly when the user needs the network list most. */
            return;
        }
        if (s_wifi.retries < CONFIG_PETCAM_WIFI_MAX_RETRY) {
            s_wifi.retries++;
            ESP_LOGW(TAG, "disconnected from \"%s\", retry %d/%d", s_wifi.ssid,
                     s_wifi.retries, CONFIG_PETCAM_WIFI_MAX_RETRY);
            esp_wifi_connect();
        } else {
            /* This one is not coming back. Another remembered network may be in
             * range — that is the whole point of keeping several. */
            ESP_LOGW(TAG, "giving up on \"%s\"; rescanning for a known network",
                     s_wifi.ssid);
            s_wifi.retries = 0;
            app_wifi_reselect();
        }
    }
}

static void on_got_ip(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    ip_event_got_ip_t *event = (ip_event_got_ip_t *)data;

    snprintf(s_wifi.ip, sizeof(s_wifi.ip), IPSTR, IP2STR(&event->ip_info.ip));
    s_wifi.retries = 0;
    s_wifi.connected = true;
    ESP_LOGI(TAG, "got %s", s_wifi.ip);
    xEventGroupSetBits(s_wifi.events, CONNECTED_BIT);
}

/* When the configured SSID is nowhere to be found, guessing at the name is a
 * slow loop. The co-processor is 2.4 GHz only, so everything a scan turns up is
 * a network this device could actually join — printing the list turns "why
 * won't it connect" into a choice. */
static void log_visible_networks(void)
{
    wifi_scan_config_t scan_cfg = { .show_hidden = false };
    wifi_ap_record_t *records;
    uint16_t count = 0;

    ESP_LOGW(TAG, "\"%s\" was not found. Scanning for 2.4 GHz networks in range:",
             s_wifi.ssid);

    esp_wifi_disconnect();
    if (esp_wifi_scan_start(&scan_cfg, true) != ESP_OK) {
        ESP_LOGE(TAG, "scan failed");
        return;
    }

    esp_wifi_scan_get_ap_num(&count);
    if (count == 0) {
        ESP_LOGW(TAG, "  no networks visible at all — check the antenna switch");
        return;
    }
    if (count > 20) {
        count = 20;
    }

    records = calloc(count, sizeof(*records));
    if (!records) {
        return;
    }
    if (esp_wifi_scan_get_ap_records(&count, records) == ESP_OK) {
        for (int i = 0; i < count; i++) {
            ESP_LOGW(TAG, "  %-32s ch%-3d %d dBm",
                     (const char *)records[i].ssid, records[i].primary, records[i].rssi);
        }
        ESP_LOGW(TAG, "every entry above is 2.4 GHz and joinable; set one in "
                 "sdkconfig.defaults.local");
    }
    free(records);

    esp_wifi_connect();
}

/* Scans and joins whichever remembered network is strongest right now. This is
 * what makes the camera portable: it is the signal in the room that decides,
 * not a fixed SSID in the build. */
static void select_and_connect(void)
{
    wifi_ap_record_t *records = calloc(24, sizeof(*records));
    uint16_t found = 0;
    int best = -1;
    int best_rssi = -128;

    if (!records) {
        return;
    }

    if (app_wifi_scan(records, 24, &found) == ESP_OK) {
        for (int i = 0; i < found; i++) {
            int slot = find_saved((const char *)records[i].ssid);

            if (slot >= 0 && records[i].rssi > best_rssi) {
                best = slot;
                best_rssi = records[i].rssi;
            }
        }
    }

    if (best >= 0) {
        ESP_LOGI(TAG, "joining \"%s\" (%d dBm), the strongest of %d remembered",
                 s_wifi.saved[best].ssid, best_rssi, s_wifi.saved_count);
        strlcpy(s_wifi.ssid, s_wifi.saved[best].ssid, sizeof(s_wifi.ssid));
        strlcpy(s_wifi.password, s_wifi.saved[best].password, sizeof(s_wifi.password));
    } else if (s_wifi.saved_count > 0) {
        ESP_LOGW(TAG, "none of the %d remembered networks are in range; "
                 "retrying \"%s\"", s_wifi.saved_count, s_wifi.saved[0].ssid);
        strlcpy(s_wifi.ssid, s_wifi.saved[0].ssid, sizeof(s_wifi.ssid));
        strlcpy(s_wifi.password, s_wifi.saved[0].password, sizeof(s_wifi.password));
    }

    free(records);

    s_wifi.retries = 0;
    apply_credentials();
    esp_wifi_connect();
}

/* Scanning blocks for seconds and cannot run inside an event handler. */
static void manager_task(void *arg)
{
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        select_and_connect();
    }
}

void app_wifi_reselect(void)
{
    if (s_wifi.manager) {
        xTaskNotifyGive(s_wifi.manager);
    }
}

esp_err_t app_wifi_start(int timeout_ms)
{
    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();

    esp_err_t err;

    /* Powers the ESP32-C6 rail through the IO expander. Without this the SDIO
     * probe fails and esp_wifi_init() reports a transport error. */
    ESP_ERROR_CHECK(bsp_feature_enable(BSP_FEATURE_WIFI, true));
    vTaskDelay(pdMS_TO_TICKS(100));

    s_wifi.events = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    /* Nothing from here on is fatal. A camera that reboots in a loop because the
     * co-processor did not answer is useless; one that keeps showing a local
     * picture and an error on its own screen can at least be diagnosed. */
    err = esp_wifi_init(&init_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_init failed: %s", esp_err_to_name(err));
        ESP_LOGE(TAG, "the ESP32-C6 did not come up over SDIO. Check that "
                 "CONFIG_ESP32P4_TAB5_C6_BOARD is set (run ./check_sdkconfig.sh) "
                 "and that the C6 carries matching ESP-Hosted firmware.");
        return err;
    }

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                        on_wifi_event, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                        on_got_ip, NULL, NULL));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    load_credentials();
    apply_credentials();

    if (xTaskCreate(manager_task, "petcam_wifimgr", 6144, NULL, 4, &s_wifi.manager) != pdPASS) {
        ESP_LOGE(TAG, "cannot start the Wi-Fi manager task");
    }

    err = esp_wifi_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_start failed: %s", esp_err_to_name(err));
        return err;
    }

    /* Pick by signal rather than trusting the first stored entry. */
    if (s_wifi.saved_count > 1) {
        select_and_connect();
    }
    ESP_LOGI(TAG, "connecting to \"%s\"", s_wifi.ssid);
    EventBits_t bits = xEventGroupWaitBits(s_wifi.events, CONNECTED_BIT, pdFALSE, pdTRUE,
                                           pdMS_TO_TICKS(timeout_ms));
    if (!(bits & CONNECTED_BIT)) {
        ESP_LOGW(TAG, "no IP after %d ms; continuing, retries carry on in the background",
                 timeout_ms);
        log_visible_networks();
        return ESP_ERR_TIMEOUT;
    }

    ESP_ERROR_CHECK(mdns_init());
    ESP_ERROR_CHECK(mdns_hostname_set(CONFIG_PETCAM_HOSTNAME));
    ESP_ERROR_CHECK(mdns_instance_name_set("Tab5 Pet Camera"));
    ESP_ERROR_CHECK(mdns_service_add(NULL, "_http", "_tcp", CONFIG_PETCAM_HTTP_PORT, NULL, 0));
    ESP_LOGI(TAG, "reachable at http://%s.local/", CONFIG_PETCAM_HOSTNAME);

    /* Recordings are much less useful when every filename is an uptime counter,
     * so pick up a real clock. Not fatal if the network has no route to NTP. */
    esp_sntp_config_t sntp_cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    if (esp_netif_sntp_init(&sntp_cfg) == ESP_OK) {
        if (esp_netif_sntp_sync_wait(pdMS_TO_TICKS(8000)) == ESP_OK) {
            setenv("TZ", CONFIG_PETCAM_TIMEZONE, 1);
            tzset();
            time_t now = time(NULL);
            struct tm tm;
            char stamp[32];
            localtime_r(&now, &tm);
            strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &tm);
            ESP_LOGI(TAG, "clock set: %s (%s)", stamp, CONFIG_PETCAM_TIMEZONE);
        } else {
            ESP_LOGW(TAG, "no NTP reply; clips will be named by uptime");
        }
    }

    return ESP_OK;
}

bool app_wifi_is_connected(void)
{
    return s_wifi.connected;
}

const char *app_wifi_ip(void)
{
    return s_wifi.ip;
}

int app_wifi_rssi(void)
{
    /* The RSSI RPC is not encodable against the co-processor firmware this Tab5
     * ships with — it fails inside tx_worker with pack_req_payload rc=-1. The UI
     * asks once a second, so give up permanently after a few tries rather than
     * hammer a call that cannot succeed. */
    static int consecutive_failures;
    static bool unsupported;

    wifi_ap_record_t ap;
    int rssi = 0;

    if (!s_wifi.connected || unsupported) {
        return 0;
    }

    if (esp_wifi_sta_get_rssi(&rssi) == ESP_OK && rssi != 0) {
        consecutive_failures = 0;
        return rssi;
    }
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK && ap.rssi != 0) {
        consecutive_failures = 0;
        return ap.rssi;
    }

    if (++consecutive_failures >= 3) {
        unsupported = true;
        ESP_LOGW(TAG, "RSSI is unavailable from this co-processor firmware; "
                 "reporting 0 and no longer asking");
    }
    return 0;
}

const char *app_wifi_ssid(void)
{
    return s_wifi.ssid;
}

/* Try the new credentials first and only write them to NVS once they have
 * actually produced an address.
 *
 * Saving first and hoping is a trap: one wrong password and the device reboots
 * into settings it cannot reach the network to fix, with no way back through
 * the on-screen panel. Recovery then means erasing NVS over USB. */
esp_err_t app_wifi_set_credentials(const char *ssid, const char *password)
{
    char previous_ssid[sizeof(s_wifi.ssid)];
    char previous_pass[sizeof(s_wifi.password)];
    EventBits_t bits;

    if (!ssid || !ssid[0]) {
        return ESP_ERR_INVALID_ARG;
    }

    strlcpy(previous_ssid, s_wifi.ssid, sizeof(previous_ssid));
    strlcpy(previous_pass, s_wifi.password, sizeof(previous_pass));

    strlcpy(s_wifi.ssid, ssid, sizeof(s_wifi.ssid));
    strlcpy(s_wifi.password, password ? password : "", sizeof(s_wifi.password));

    ESP_LOGI(TAG, "trying \"%s\" before saving it", s_wifi.ssid);
    s_wifi.connected = false;
    s_wifi.retries = 0;
    xEventGroupClearBits(s_wifi.events, CONNECTED_BIT);
    esp_wifi_disconnect();
    apply_credentials();
    esp_wifi_connect();

    bits = xEventGroupWaitBits(s_wifi.events, CONNECTED_BIT, pdFALSE, pdTRUE,
                               pdMS_TO_TICKS(CREDENTIAL_TRIAL_MS));
    if (!(bits & CONNECTED_BIT)) {
        ESP_LOGW(TAG, "\"%s\" did not connect; keeping the previous network", ssid);
        strlcpy(s_wifi.ssid, previous_ssid, sizeof(s_wifi.ssid));
        strlcpy(s_wifi.password, previous_pass, sizeof(s_wifi.password));
        s_wifi.retries = 0;
        esp_wifi_disconnect();
        apply_credentials();
        esp_wifi_connect();
        return ESP_ERR_WIFI_NOT_CONNECT;
    }

    remember(s_wifi.ssid, s_wifi.password);
    ESP_LOGI(TAG, "connected and saved \"%s\" (%d remembered)",
             s_wifi.ssid, s_wifi.saved_count);
    return ESP_OK;
}

const char *app_wifi_saved_password(const char *ssid)
{
    int slot = ssid ? find_saved(ssid) : -1;

    return slot >= 0 ? s_wifi.saved[slot].password : NULL;
}

esp_err_t app_wifi_forget(const char *ssid)
{
    if (!ssid) {
        s_wifi.saved_count = 0;
        persist_credentials();
        ESP_LOGI(TAG, "forgot every remembered network");
    } else {
        int slot = find_saved(ssid);

        if (slot < 0) {
            return ESP_ERR_NOT_FOUND;
        }
        memmove(&s_wifi.saved[slot], &s_wifi.saved[slot + 1],
                sizeof(s_wifi.saved[0]) * (s_wifi.saved_count - slot - 1));
        s_wifi.saved_count--;
        persist_credentials();
        ESP_LOGI(TAG, "forgot \"%s\" (%d left)", ssid, s_wifi.saved_count);
    }

    /* If the network in use was the one dropped, move to another that is in
     * range rather than sitting on credentials that are no longer wanted. */
    if (!ssid || strcmp(ssid, s_wifi.ssid) == 0) {
        app_wifi_reselect();
    }
    return ESP_OK;
}

int app_wifi_saved_count(void)
{
    return s_wifi.saved_count;
}

const char *app_wifi_saved_ssid(int index)
{
    if (index < 0 || index >= s_wifi.saved_count) {
        return NULL;
    }
    return s_wifi.saved[index].ssid;
}

bool app_wifi_is_saved(const char *ssid)
{
    return ssid && find_saved(ssid) >= 0;
}

esp_err_t app_wifi_scan(wifi_ap_record_t *out, uint16_t max, uint16_t *found)
{
    wifi_scan_config_t cfg = { .show_hidden = false };
    uint16_t count = 0;
    esp_err_t err;

    *found = 0;

    s_wifi.scanning = true;
    esp_wifi_disconnect();
    err = esp_wifi_scan_start(&cfg, true);
    if (err != ESP_OK) {
        s_wifi.scanning = false;
        esp_wifi_connect();
        return err;
    }
    esp_wifi_scan_get_ap_num(&count);
    if (count == 0) {
        return ESP_OK;
    }
    if (count > max) {
        count = max;
    }

    err = esp_wifi_scan_get_ap_records(&count, out);
    if (err == ESP_OK) {
        *found = count;
    }

    s_wifi.scanning = false;
    /* A scan drops the association on this transport; get it back. */
    esp_wifi_connect();
    return err;
}
