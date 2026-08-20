#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/errno.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/jpeg_encode.h"
#include "driver/ppa.h"
#include "freertos/semphr.h"
#include "esp_heap_caps.h"
#include "linux/videodev2.h"
#include "esp_video_isp_ioctl.h"
/* Defines ESP_VIDEO_MIPI_CSI_DEVICE_NAME, which the BSP header refers to via
 * BSP_CAMERA_DEVICE without including it itself. */
#include "esp_video_device.h"
#include "bsp/esp-bsp.h"

#include "app_camera.h"
#include "app_motion.h"
#include "app_preview.h"
#include "frame_bus.h"

static const char *TAG = "camera";

/* Two capture buffers keep the sensor streaming while we encode the other one.
 * More would only add latency — we always want the newest frame. */
#define CAPTURE_BUFFERS 2

/* The JPEG pool needs one frame being encoded, one published, and headroom for
 * readers still sending an older one. */
#define FRAME_POOL_SIZE 4

/* Worst case for a detailed 1600x1200 scene at quality 100. Sized generously
 * because a truncated JPEG shows up as a corrupt image, not an error. */
#define JPEG_BUFFER_BYTES (512 * 1024)

/* esp_video checks the USERPTR against its own align_size; 64 bytes covers the
 * PSRAM cache line on the ESP32-P4. */
#define CAPTURE_BUFFER_ALIGN 64

#define MIN_FRAME_INTERVAL_US (1000000 / CONFIG_PETCAM_MAX_FPS)

static struct {
    int                   fd;
    /* Filled by the capture task when a still is asked for. Doing the rotation
     * there avoids copying every frame just in case someone wants one. */
    ppa_client_handle_t   still_ppa;
    uint8_t              *still_rgb;
    uint8_t              *still_jpeg;
    size_t                still_rgb_size;
    size_t                still_jpeg_size;
    size_t                still_len;
    int                   still_degrees;
    volatile bool         still_wanted;
    volatile bool         still_ready;
    SemaphoreHandle_t     still_lock;
    jpeg_encoder_handle_t encoder;
    uint8_t              *capture_buf[CAPTURE_BUFFERS];
    uint32_t              capture_len[CAPTURE_BUFFERS];
    uint32_t              width;
    uint32_t              height;
    app_camera_stats_t    stats;
} s_cam;

/* Sizes worth offering for a room-watching stream, largest first. */
static const struct { uint32_t w, h; } k_candidate_sizes[] = {
    { 1600, 1200 }, { 1280, 720 }, { 1024, 768 }, { 800, 600 }, { 640, 480 },
};

static esp_err_t try_format(int fd, uint32_t width, uint32_t height)
{
    struct v4l2_format format = {
        .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
        .fmt.pix.width  = width,
        .fmt.pix.height = height,
        /* RGB565 is what the P4 ISP emits by default from the SC202CS RAW8
         * stream, and the JPEG encoder takes it directly — no conversion. */
        .fmt.pix.pixelformat = V4L2_PIX_FMT_RGB565,
    };

    return ioctl(fd, VIDIOC_S_FMT, &format) == 0 ? ESP_OK : ESP_FAIL;
}

/* VIDIOC_ENUM_FRAMESIZES is not implemented for the CSI device, so the only
 * honest way to find out what the pipeline accepts is to ask it. Probing with
 * VIDIOC_S_FMT before any buffers exist is harmless, and the caller sets the
 * format it actually wants afterwards. */
static void log_accepted_frame_sizes(int fd)
{
    ESP_LOGI(TAG, "probing which RGB565 frame sizes this pipeline accepts.");
    ESP_LOGI(TAG, "the driver logs 'format width or height is invalid' at error "
             "level for each size it rejects — that is this probe working, not a fault:");
    for (size_t i = 0; i < sizeof(k_candidate_sizes) / sizeof(k_candidate_sizes[0]); i++) {
        bool ok = try_format(fd, k_candidate_sizes[i].w, k_candidate_sizes[i].h) == ESP_OK;
        ESP_LOGI(TAG, "  %4ux%-4u %s",
                 (unsigned)k_candidate_sizes[i].w, (unsigned)k_candidate_sizes[i].h,
                 ok ? "yes" : "no");
    }
}

static esp_err_t open_and_configure_device(void)
{
    struct v4l2_format format = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE };

    s_cam.fd = open(BSP_CAMERA_DEVICE, O_RDWR);
    if (s_cam.fd < 0) {
        ESP_LOGE(TAG, "cannot open %s (errno %d) — did bsp_camera_start() succeed?",
                 BSP_CAMERA_DEVICE, errno);
        return ESP_FAIL;
    }

    log_accepted_frame_sizes(s_cam.fd);

    /* The SC202CS pipeline only offers a fixed set of sizes; a request it does
     * not recognise is rejected outright rather than rounded. Fall back to
     * whatever the driver has already selected so an awkward Kconfig value
     * degrades into a working stream instead of no camera at all. */
    if (try_format(s_cam.fd, CONFIG_PETCAM_WIDTH, CONFIG_PETCAM_HEIGHT) != ESP_OK) {
        ESP_LOGW(TAG, "%ux%u was rejected — using the driver's own default. Pick one "
                 "of the sizes listed above in menuconfig to choose deliberately.",
                 (unsigned)CONFIG_PETCAM_WIDTH, (unsigned)CONFIG_PETCAM_HEIGHT);
    }

    if (ioctl(s_cam.fd, VIDIOC_G_FMT, &format) != 0) {
        ESP_LOGE(TAG, "cannot read back the capture format");
        return ESP_FAIL;
    }

    if (format.fmt.pix.pixelformat != V4L2_PIX_FMT_RGB565) {
        ESP_LOGE(TAG, "driver settled on a non-RGB565 format; this build only "
                 "feeds RGB565 to the JPEG encoder");
        return ESP_ERR_NOT_SUPPORTED;
    }

    s_cam.width  = format.fmt.pix.width;
    s_cam.height = format.fmt.pix.height;
    s_cam.stats.width  = s_cam.width;
    s_cam.stats.height = s_cam.height;
    ESP_LOGI(TAG, "capturing %ux%u RGB565", (unsigned)s_cam.width, (unsigned)s_cam.height);
    return ESP_OK;
}

static esp_err_t setup_capture_buffers(void)
{
    /* USERPTR rather than MMAP so the sensor DMAs straight into memory the JPEG
     * encoder can read, and no frame is ever copied.
     *
     * These are allocated to esp_video's rules, not the JPEG encoder's:
     * VIDIOC_QBUF rejects any pointer that is not in PSRAM or not aligned to the
     * driver's align_size. The encoder is the lenient side here — per the
     * esp_driver_jpeg source, only its *output* buffer needs cache alignment,
     * while the input has no alignment restriction at all. */
    struct v4l2_requestbuffers req = {
        .count  = CAPTURE_BUFFERS,
        .type   = V4L2_BUF_TYPE_VIDEO_CAPTURE,
        .memory = V4L2_MEMORY_USERPTR,
    };

    if (ioctl(s_cam.fd, VIDIOC_REQBUFS, &req) != 0) {
        ESP_LOGE(TAG, "VIDIOC_REQBUFS failed (errno %d)", errno);
        return ESP_FAIL;
    }

    for (int i = 0; i < CAPTURE_BUFFERS; i++) {
        struct v4l2_buffer buf = {
            .type   = V4L2_BUF_TYPE_VIDEO_CAPTURE,
            .memory = V4L2_MEMORY_USERPTR,
            .index  = i,
        };

        if (ioctl(s_cam.fd, VIDIOC_QUERYBUF, &buf) != 0) {
            ESP_LOGE(TAG, "VIDIOC_QUERYBUF %d failed", i);
            return ESP_FAIL;
        }

        s_cam.capture_buf[i] = heap_caps_aligned_alloc(CAPTURE_BUFFER_ALIGN, buf.length,
                                                       MALLOC_CAP_SPIRAM | MALLOC_CAP_CACHE_ALIGNED);
        if (!s_cam.capture_buf[i]) {
            ESP_LOGE(TAG, "out of memory for capture buffer %d (%u bytes)",
                     i, (unsigned)buf.length);
            return ESP_ERR_NO_MEM;
        }
        s_cam.capture_len[i] = buf.length;

        buf.m.userptr = (unsigned long)s_cam.capture_buf[i];
        buf.length    = s_cam.capture_len[i];
        if (ioctl(s_cam.fd, VIDIOC_QBUF, &buf) != 0) {
            ESP_LOGE(TAG, "VIDIOC_QBUF %d failed", i);
            return ESP_FAIL;
        }
    }

    return ESP_OK;
}

static void capture_task(void *arg)
{
    const jpeg_encode_cfg_t encode_cfg = {
        .width         = s_cam.width,
        .height        = s_cam.height,
        .src_type      = JPEG_ENCODE_IN_FORMAT_RGB565,
        /* 4:2:0 halves the chroma resolution. On a room-watching stream this is
         * invisible and cuts the JPEG roughly in half. */
        .sub_sample    = JPEG_DOWN_SAMPLING_YUV420,
        .image_quality = CONFIG_PETCAM_JPEG_QUALITY,
#if CONFIG_PETCAM_PIXEL_REVERSE
        .pixel_reverse = true,
#else
        .pixel_reverse = false,
#endif
    };
    int64_t last_publish_us = 0;
    int64_t window_start_us = esp_timer_get_time();
    uint32_t window_frames = 0;
    uint32_t seconds_elapsed = 0;

    for (;;) {
        struct v4l2_buffer buf = {
            .type   = V4L2_BUF_TYPE_VIDEO_CAPTURE,
            .memory = V4L2_MEMORY_USERPTR,
        };

        if (ioctl(s_cam.fd, VIDIOC_DQBUF, &buf) != 0) {
            ESP_LOGE(TAG, "VIDIOC_DQBUF failed (errno %d)", errno);
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        int64_t now_us = esp_timer_get_time();
        bool usable = (buf.flags & V4L2_BUF_FLAG_DONE) != 0;
        bool due    = (now_us - last_publish_us) >= MIN_FRAME_INTERVAL_US;

        if (usable && due) {
            frame_t *frame = frame_bus_acquire_writable();
            if (frame) {
                uint32_t jpeg_len = 0;
                esp_err_t err = jpeg_encoder_process(s_cam.encoder, &encode_cfg,
                                                     s_cam.capture_buf[buf.index],
                                                     buf.bytesused,
                                                     frame->data, frame->capacity,
                                                     &jpeg_len);
                if (err == ESP_OK) {
#if CONFIG_PETCAM_ENABLE_MOTION
                    app_motion_submit_jpeg(frame->data, jpeg_len);
#endif
                    frame_bus_publish(frame, jpeg_len);
                    s_cam.stats.last_jpeg_len = jpeg_len;
                    s_cam.stats.frames++;
                    window_frames++;
                    last_publish_us = now_us;
                } else {
                    ESP_LOGW(TAG, "JPEG encode failed: %s", esp_err_to_name(err));
                    frame_bus_release(frame);
                    s_cam.stats.dropped++;
                }
            } else {
                /* Every pooled frame is still being sent. Skipping is the right
                 * call: a live view wants the newest frame, not a backlog. */
                s_cam.stats.dropped++;
            }
        } else if (!usable) {
            s_cam.stats.dropped++;
        }

        if (usable && s_cam.still_wanted) {
            /* PPA turns counter-clockwise; view_rotation is clockwise. */
            int ccw = ((360 - s_cam.still_degrees) % 360 + 360) % 360;
            bool swapped = (ccw == 90 || ccw == 270);
            uint32_t out_w = swapped ? s_cam.height : s_cam.width;
            uint32_t out_h = swapped ? s_cam.width : s_cam.height;
            ppa_srm_oper_config_t op = {
                .in = {
                    .buffer  = s_cam.capture_buf[buf.index],
                    .pic_w   = s_cam.width,
                    .pic_h   = s_cam.height,
                    .block_w = s_cam.width,
                    .block_h = s_cam.height,
                    .srm_cm  = PPA_SRM_COLOR_MODE_RGB565,
                },
                .out = {
                    .buffer      = s_cam.still_rgb,
                    .buffer_size = s_cam.still_rgb_size,
                    .pic_w       = out_w,
                    .pic_h       = out_h,
                    .srm_cm      = PPA_SRM_COLOR_MODE_RGB565,
                },
                .rotation_angle = (ccw == 90)  ? PPA_SRM_ROTATION_ANGLE_90 :
                                  (ccw == 180) ? PPA_SRM_ROTATION_ANGLE_180 :
                                  (ccw == 270) ? PPA_SRM_ROTATION_ANGLE_270 :
                                                 PPA_SRM_ROTATION_ANGLE_0,
                .scale_x = 1.0f,
                .scale_y = 1.0f,
                .mode = PPA_TRANS_MODE_BLOCKING,
            };

            if (ppa_do_scale_rotate_mirror(s_cam.still_ppa, &op) == ESP_OK) {
                jpeg_encode_cfg_t still_cfg = encode_cfg;
                uint32_t jlen = 0;

                still_cfg.width  = out_w;
                still_cfg.height = out_h;
                if (jpeg_encoder_process(s_cam.encoder, &still_cfg, s_cam.still_rgb,
                                         (uint32_t)out_w * out_h * 2,
                                         s_cam.still_jpeg, s_cam.still_jpeg_size,
                                         &jlen) == ESP_OK) {
                    s_cam.still_len = jlen;
                    s_cam.still_ready = true;
                }
            }
            s_cam.still_wanted = false;
        }

#if CONFIG_PETCAM_ENABLE_MOTION
        if (usable) {
            app_motion_submit_frame(s_cam.capture_buf[buf.index], s_cam.width, s_cam.height);
        }
#endif
#if CONFIG_PETCAM_ENABLE_PREVIEW
        if (usable) {
            /* Must happen before the buffer goes back to the sensor. The PPA
             * copy is blocking but hardware, so it costs the capture task very
             * little, and the preview throttles itself internally. */
            app_preview_submit(s_cam.capture_buf[buf.index], s_cam.width, s_cam.height);
        }
#endif

        /* Requeue unconditionally — a buffer we fail to return is a buffer the
         * sensor never gets back. */
        buf.m.userptr = (unsigned long)s_cam.capture_buf[buf.index];
        buf.length    = s_cam.capture_len[buf.index];
        if (ioctl(s_cam.fd, VIDIOC_QBUF, &buf) != 0) {
            ESP_LOGE(TAG, "VIDIOC_QBUF failed (errno %d)", errno);
        }

        if (now_us - window_start_us >= 1000000) {
            s_cam.stats.fps = window_frames * 1000000.0f / (now_us - window_start_us);
            window_start_us = now_us;
            window_frames = 0;

            /* An unattended camera is usually watched through its serial log, so
             * report health periodically rather than only on request. */
            if (++seconds_elapsed % 5 == 0) {
                ESP_LOGI(TAG, "%.1f fps, %u KB/frame, %u frames, %u dropped",
                         s_cam.stats.fps, (unsigned)(s_cam.stats.last_jpeg_len / 1024),
                         (unsigned)s_cam.stats.frames, (unsigned)s_cam.stats.dropped);
            }
        }
    }
}

esp_err_t app_camera_start(void)
{
    const jpeg_encode_engine_cfg_t engine_cfg = {
        .intr_priority = 0,
        .timeout_ms    = 100,
    };
    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    esp_err_t err;

    err = open_and_configure_device();
    if (err != ESP_OK) {
        return err;
    }

    err = setup_capture_buffers();
    if (err != ESP_OK) {
        return err;
    }

    err = frame_bus_init(FRAME_POOL_SIZE, JPEG_BUFFER_BYTES);
    if (err != ESP_OK) {
        return err;
    }

    err = jpeg_new_encoder_engine(&engine_cfg, &s_cam.encoder);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "cannot create the JPEG encoder: %s", esp_err_to_name(err));
        return err;
    }

    if (ioctl(s_cam.fd, VIDIOC_STREAMON, &type) != 0) {
        ESP_LOGE(TAG, "VIDIOC_STREAMON failed (errno %d)", errno);
        return ESP_FAIL;
    }

    {
        const ppa_client_config_t ppa_cfg = {
            .oper_type = PPA_OPERATION_SRM,
            .max_pending_trans_num = 1,
        };
        size_t rgb = (size_t)s_cam.width * s_cam.height * 2;

        s_cam.still_lock = xSemaphoreCreateMutex();
        s_cam.still_rgb_size = rgb;
        s_cam.still_rgb = heap_caps_aligned_alloc(128, rgb,
                                                  MALLOC_CAP_SPIRAM | MALLOC_CAP_CACHE_ALIGNED);
        s_cam.still_jpeg_size = JPEG_BUFFER_BYTES;
        {
            const jpeg_encode_memory_alloc_cfg_t mem = {
                .buffer_direction = JPEG_ENC_ALLOC_OUTPUT_BUFFER,
            };
            size_t got = 0;
            s_cam.still_jpeg = jpeg_alloc_encoder_mem(JPEG_BUFFER_BYTES, &mem, &got);
        }
        if (!s_cam.still_lock || !s_cam.still_rgb || !s_cam.still_jpeg ||
            ppa_register_client(&ppa_cfg, &s_cam.still_ppa) != ESP_OK) {
            ESP_LOGW(TAG, "rotated stills unavailable; /snapshot will serve the raw frame");
        }
    }

    /* Priority 5 keeps capture ahead of the HTTP tasks; the encode is hardware
     * so this task is mostly blocked in ioctl(). */
    if (xTaskCreatePinnedToCore(capture_task, "petcam_capture", 6144, NULL, 5, NULL, 1) != pdPASS) {
        ESP_LOGE(TAG, "cannot create the capture task");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "streaming at up to %d fps, JPEG quality %d",
             CONFIG_PETCAM_MAX_FPS, CONFIG_PETCAM_JPEG_QUALITY);
    return ESP_OK;
}

void app_camera_get_stats(app_camera_stats_t *out)
{
    *out = s_cam.stats;
    out->dropped += frame_bus_dropped();
}

esp_err_t app_camera_still(int degrees, const uint8_t **out, size_t *len, int timeout_ms)
{
    int waited = 0;

    if (!s_cam.still_ppa || !s_cam.still_lock) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    xSemaphoreTake(s_cam.still_lock, portMAX_DELAY);
    s_cam.still_degrees = degrees;
    s_cam.still_ready = false;
    s_cam.still_wanted = true;

    while (!s_cam.still_ready && waited < timeout_ms) {
        vTaskDelay(pdMS_TO_TICKS(20));
        waited += 20;
    }

    if (!s_cam.still_ready) {
        s_cam.still_wanted = false;
        xSemaphoreGive(s_cam.still_lock);
        return ESP_ERR_TIMEOUT;
    }

    *out = s_cam.still_jpeg;
    *len = s_cam.still_len;
    /* Held until the caller releases it: the buffer is reused by the next
     * request, so overlapping snapshots must not share it. */
    return ESP_OK;
}

void app_camera_still_release(void)
{
    if (s_cam.still_lock) {
        xSemaphoreGive(s_cam.still_lock);
    }
}

/* The ISP has its own device node; the capture node knows nothing about colour
 * correction. */
static esp_err_t ccm_ioctl(unsigned long request, esp_video_isp_ccm_t *ccm)
{
    int isp = open(ESP_VIDEO_ISP1_DEVICE_NAME, O_RDWR);
    esp_err_t err;

    struct v4l2_ext_control control = {
        .id = V4L2_CID_USER_ESP_ISP_CCM,
        .size = sizeof(*ccm),
        .ptr = ccm,
    };
    struct v4l2_ext_controls controls = {
        .ctrl_class = V4L2_CTRL_CLASS_USER,
        .count = 1,
        .controls = &control,
    };

    if (isp < 0) {
        ESP_LOGE(TAG, "cannot open %s (errno %d)", ESP_VIDEO_ISP1_DEVICE_NAME, errno);
        return ESP_FAIL;
    }
    err = ioctl(isp, request, &controls) == 0 ? ESP_OK : ESP_FAIL;
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "CCM ioctl failed (errno %d)", errno);
    }
    close(isp);
    return err;
}

esp_err_t app_camera_get_ccm(float *matrix, bool *enabled)
{
    esp_video_isp_ccm_t ccm = { 0 };

    if (ccm_ioctl(VIDIOC_G_EXT_CTRLS, &ccm) != ESP_OK) {
        return ESP_FAIL;
    }
    *enabled = ccm.enable;
    for (int r = 0; r < 3; r++) {
        for (int c = 0; c < 3; c++) {
            matrix[r * 3 + c] = ccm.matrix[r][c];
        }
    }
    return ESP_OK;
}

esp_err_t app_camera_set_ccm(const float *matrix)
{
    esp_video_isp_ccm_t ccm = { .enable = true };

    for (int r = 0; r < 3; r++) {
        for (int c = 0; c < 3; c++) {
            ccm.matrix[r][c] = matrix[r * 3 + c];
        }
    }
    return ccm_ioctl(VIDIOC_S_EXT_CTRLS, &ccm);
}

static esp_err_t isp_int_ctrl(unsigned long request, uint32_t id, int32_t *value)
{
    int isp = open(ESP_VIDEO_ISP1_DEVICE_NAME, O_RDWR);
    struct v4l2_ext_control control = { .id = id, .value = *value };
    struct v4l2_ext_controls controls = {
        .ctrl_class = V4L2_CTRL_CLASS_USER,
        .count = 1,
        .controls = &control,
    };
    esp_err_t err;

    if (isp < 0) {
        return ESP_FAIL;
    }
    err = ioctl(isp, request, &controls) == 0 ? ESP_OK : ESP_FAIL;
    if (err == ESP_OK) {
        *value = control.value;
    } else {
        ESP_LOGE(TAG, "ISP control 0x%08x failed (errno %d)", (unsigned)id, errno);
    }
    close(isp);
    return err;
}

esp_err_t app_camera_get_wb(float *red, float *blue)
{
    int32_t r = 0, b = 0;

    if (isp_int_ctrl(VIDIOC_G_EXT_CTRLS, V4L2_CID_RED_BALANCE, &r) != ESP_OK ||
        isp_int_ctrl(VIDIOC_G_EXT_CTRLS, V4L2_CID_BLUE_BALANCE, &b) != ESP_OK) {
        return ESP_FAIL;
    }
    *red = (float)r / V4L2_CID_RED_BALANCE_DEN;
    *blue = (float)b / V4L2_CID_BLUE_BALANCE_DEN;
    return ESP_OK;
}

esp_err_t app_camera_set_wb(float red, float blue)
{
    int32_t r = (int32_t)(red * V4L2_CID_RED_BALANCE_DEN);
    int32_t b = (int32_t)(blue * V4L2_CID_BLUE_BALANCE_DEN);

    if (isp_int_ctrl(VIDIOC_S_EXT_CTRLS, V4L2_CID_RED_BALANCE, &r) != ESP_OK) {
        return ESP_FAIL;
    }
    return isp_int_ctrl(VIDIOC_S_EXT_CTRLS, V4L2_CID_BLUE_BALANCE, &b);
}

esp_err_t app_camera_set_awb_auto(bool enable)
{
    int32_t v = enable ? 1 : 0;

    return isp_int_ctrl(VIDIOC_S_EXT_CTRLS, V4L2_CID_AUTO_WHITE_BALANCE, &v);
}
