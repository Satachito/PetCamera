#include <math.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "bsp/esp-bsp.h"

#include "app_orientation.h"

static const char *TAG = "orient";

/* How far the device has to be off flat before the reading means anything. Laid
 * face-up on a table, gravity is almost entirely on Z and the X/Y values are
 * noise — rotating the screen on that noise would be maddening. */
#define TILT_THRESHOLD_G 0.35f

/* Consecutive agreeing samples before the screen actually turns. At the sample
 * period below this is about a second, which stops a glance or a bump from
 * flipping the display. */
#define STABLE_SAMPLES 5

#define SAMPLE_PERIOD_MS 200

static struct {
    sensor_handle_t      imu;
    app_orientation_cb_t on_change;
    int                  rotation;        /* settled, degrees */
    int                  candidate;
    int                  candidate_count;
    bool                 logged_once;
    float                ax, ay, az;
    int                  raw_quadrant;
} s_or;

/* Maps the gravity vector to one of four quadrants. Which physical pose counts
 * as "upright" depends on how the BMI270 is mounted, so the result is shifted by
 * a build-time offset rather than hard-coding this board's convention. */
static int rotation_from_accel(float x, float y, float z)
{
    (void)z;

    if (fabsf(x) < TILT_THRESHOLD_G && fabsf(y) < TILT_THRESHOLD_G) {
        return -1;  /* lying flat: no opinion */
    }

    int quadrant;
    if (fabsf(y) >= fabsf(x)) {
        quadrant = (y > 0) ? 0 : 180;
    } else {
        quadrant = (x > 0) ? 90 : 270;
    }

    return (quadrant + CONFIG_PETCAM_ORIENTATION_OFFSET) % 360;
}

static void on_accel(void *handler_args, esp_event_base_t base, int32_t id, void *event_data)
{
    sensor_data_t *sample = (sensor_data_t *)event_data;

    if (id != SENSOR_ACCE_DATA_READY) {
        return;
    }

    float x = sample->acce.x;
    float y = sample->acce.y;
    float z = sample->acce.z;

    if (!s_or.logged_once) {
        /* One line so the mounting convention can be calibrated against a known
         * physical pose instead of guessed. */
        s_or.logged_once = true;
        ESP_LOGI(TAG, "first accelerometer sample: x=%.2f y=%.2f z=%.2f g", x, y, z);
    }

    s_or.ax = x;
    s_or.ay = y;
    s_or.az = z;

    int candidate = rotation_from_accel(x, y, z);
    s_or.raw_quadrant = candidate;
    if (candidate < 0) {
        s_or.candidate_count = 0;
        return;
    }

    if (candidate != s_or.candidate) {
        s_or.candidate = candidate;
        s_or.candidate_count = 1;
        return;
    }
    if (++s_or.candidate_count < STABLE_SAMPLES) {
        return;
    }
    s_or.candidate_count = STABLE_SAMPLES;   /* stop counting up forever */

    if (candidate != s_or.rotation) {
        ESP_LOGI(TAG, "rotating screen to %d deg (accel x=%.2f y=%.2f z=%.2f)",
                 candidate, x, y, z);
        s_or.rotation = candidate;
        if (s_or.on_change) {
            s_or.on_change(candidate);
        }
    }
}

esp_err_t app_orientation_start(app_orientation_cb_t on_change)
{
    bsp_sensor_config_t cfg = {
        .type   = IMU_ID,
        .mode   = MODE_POLLING,
        .period = SAMPLE_PERIOD_MS,
    };
    esp_err_t err;

    s_or.on_change = on_change;
    s_or.rotation = CONFIG_PETCAM_ORIENTATION_OFFSET % 360;

    err = bsp_sensor_init(&cfg, &s_or.imu);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "no IMU (%s); the screen stays at %d deg",
                 esp_err_to_name(err), s_or.rotation);
        return err;
    }

    err = iot_sensor_handler_register(s_or.imu, on_accel, NULL);
    if (err != ESP_OK) {
        return err;
    }

    err = iot_sensor_start(s_or.imu);
    if (err != ESP_OK) {
        return err;
    }

    ESP_LOGI(TAG, "auto-rotation active (offset %d deg, %d ms sampling)",
             CONFIG_PETCAM_ORIENTATION_OFFSET, SAMPLE_PERIOD_MS);
    return ESP_OK;
}

int app_orientation_get(void)
{
    return s_or.rotation;
}

void app_orientation_get_accel(float *x, float *y, float *z, int *raw_quadrant)
{
    *x = s_or.ax;
    *y = s_or.ay;
    *z = s_or.az;
    *raw_quadrant = s_or.raw_quadrant;
}
